// Implements TouchPadRawInputFilter for processing precision touchpad RAWINPUT events.
#include "rawinputfilter.h"
#include "TouchpadStateManager.h"

#ifdef _WIN32

#include <algorithm>
#include <cmath>
#include <deque>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "InputSender.h"
#include <QDebug>
#include <QString>
#include <Windows.h>
#include <hidsdi.h>
#include <hidusage.h>
#include <qstringliteral.h>
#include <stdio.h>
namespace
{
    std::wstring formatSystemError(DWORD errorCode)
    {
        if (errorCode == 0)
        {
            return L"no error";
        }

        LPWSTR buffer = nullptr;
        const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                            FORMAT_MESSAGE_IGNORE_INSERTS;
        const DWORD written =
            ::FormatMessageW(flags, nullptr, errorCode, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                             reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);

        std::wstring message =
            (written && buffer) ? std::wstring(buffer, written) : L"unknown error";
        if (buffer)
        {
            ::LocalFree(buffer);
        }

        // Trim trailing whitespace
        while (!message.empty() &&
               (message.back() == L'\n' || message.back() == L'\r' || message.back() == L' '))
        {
            message.pop_back();
        }

        return message;
    }

    void logWin32Failure(const wchar_t* context)
    {
        const DWORD error = ::GetLastError();
        wchar_t buffer[512];
        swprintf_s(buffer, L"%s: %s (code %lu)\n", context, formatSystemError(error).c_str(),
                   error);
        ::OutputDebugStringW(buffer);
    }

    USHORT firstUsage(const HIDP_VALUE_CAPS& cap)
    {
        return cap.IsRange ? cap.Range.UsageMin : cap.NotRange.Usage;
    }
} // namespace

// Extrapolate a small number of trailing frames using simple per-contact linear velocity.
// history must contain at least two frames; we use the last two frames to estimate velocity.
static std::deque<TouchPadRawInputFilter::Touchpadframe> extrapolateTrailingFrames(
    const std::deque<TouchPadRawInputFilter::Touchpadframe>& history, size_t missingCount)
{
    std::deque<TouchPadRawInputFilter::Touchpadframe> result;
    if (missingCount == 0 || history.size() < 2)
    {
        return result;
    }

    // We'll prefer using last 3 frames to estimate velocity and acceleration.
    const size_t n = history.size();
    const auto& last = history[n - 1];
    const auto& prev = history[n - 2];
    const bool havePrev2 = (n >= 3);
    const auto& prev2 = havePrev2 ? history[n - 3] : prev;

    // Time deltas in ticks
    int64_t dt1 = prev.scantime - prev2.scantime; // between prev2 and prev
    int64_t dt2 = last.scantime - prev.scantime;  // between prev and last
    if (dt1 <= 0)
        dt1 = dt2 > 0 ? dt2 : 1;
    if (dt2 <= 0)
        dt2 = dt1 > 0 ? dt1 : 1;

    // Build maps of positions by contact id for prev2, prev and last
    std::unordered_map<uint32_t, std::pair<double, double>> pos2;
    std::unordered_map<uint32_t, std::pair<double, double>> pos1;
    std::unordered_map<uint32_t, std::pair<double, double>> pos0;
    for (const auto& c : prev2.contacts)
        pos2[c.id] = {static_cast<double>(c.x), static_cast<double>(c.y)};
    for (const auto& c : prev.contacts)
        pos1[c.id] = {static_cast<double>(c.x), static_cast<double>(c.y)};
    for (const auto& c : last.contacts)
        pos0[c.id] = {static_cast<double>(c.x), static_cast<double>(c.y)};

    // For each requested trailing frame, extrapolate using constant acceleration model:
    // x(t) = x_last + v_last * t + 0.5 * a * t^2
    for (size_t step = 1; step <= missingCount; ++step)
    {
        TouchPadRawInputFilter::Touchpadframe f;
        f.scantime = last.scantime + dt2 * static_cast<int64_t>(step);

        for (const auto& kv : pos0)
        {
            const uint32_t id = kv.first;
            const double x0 = kv.second.first;
            const double y0 = kv.second.second;

            double vx = 0.0;
            double vy = 0.0;
            double ax = 0.0;
            double ay = 0.0;

            auto it1 = pos1.find(id);
            if (it1 != pos1.end())
            {
                const double x1 = it1->second.first;
                const double y1 = it1->second.second;
                // last velocity (per tick)
                vx = (x0 - x1) / static_cast<double>(dt2);
                vy = (y0 - y1) / static_cast<double>(dt2);

                if (havePrev2)
                {
                    auto it2 = pos2.find(id);
                    if (it2 != pos2.end())
                    {
                        const double x2 = it2->second.first;
                        const double y2 = it2->second.second;
                        // previous velocity
                        const double vx_prev = (x1 - x2) / static_cast<double>(dt1);
                        const double vy_prev = (y1 - y2) / static_cast<double>(dt1);
                        // acceleration per tick^2
                        ax = (vx - vx_prev) / static_cast<double>(dt2);
                        ay = (vy - vy_prev) / static_cast<double>(dt2);
                    }
                }
            }

            // time offset in ticks from last frame
            const double t = static_cast<double>(dt2 * static_cast<int64_t>(step));
            const double nx = x0 + vx * t + 0.5 * ax * t * t;
            const double ny = y0 + vy * t + 0.5 * ay * t * t;

            TouchPadRawInputFilter::ContactLog contact;
            contact.id = id;
            contact.x = static_cast<int32_t>(std::lround(nx));
            contact.y = static_cast<int32_t>(std::lround(ny));
            f.contacts.push_back(contact);
        }

        result.push_back(std::move(f));
    }

    return result;
}

TouchPadRawInputFilter::TouchPadRawInputFilter(HWND targetWindow) : m_targetWindow(targetWindow)
{
    if (!m_targetWindow)
    {
        ::OutputDebugStringW(L"TouchPadRawInputFilter requires a valid HWND.\n");
        return;
    }

    // 创建状态管理器
    m_stateManager = std::make_unique<TouchpadStateManager>(m_targetWindow);

    if (ensurePrecisionTouchpadPresent())
    {
        ::OutputDebugStringW(L"Precision touchpad detected on the system.\n");
    }
    else
    {
        ::OutputDebugStringW(L"Precision touchpad not detected via RAWINPUT device list.\n");
    }

    m_registered = registerRawInput();
    if (!m_registered)
    {
        logWin32Failure(L"RegisterRawInputDevices failed");
    }
    else
    {
        ::OutputDebugStringW(L"Precision touchpad RAWINPUT registration successful.\n");
    }

    // Register singleton instance (warn if one already exists)
    if (s_instance && s_instance != this)
    {
        ::OutputDebugStringW(L"Warning: multiple TouchPadRawInputFilter instances created.\n");
    }
    s_instance = this;
}

TouchPadRawInputFilter::~TouchPadRawInputFilter()
{
    if (m_registered)
    {
        RAWINPUTDEVICE device{};
        device.usUsagePage = 0x000D;
        device.usUsage = 0x0005;
        device.dwFlags = RIDEV_REMOVE;
        device.hwndTarget = nullptr;

        if (!::RegisterRawInputDevices(&device, 1, sizeof(device)))
        {
            logWin32Failure(L"Failed to unregister RAWINPUT device");
        }
    }

    if (s_instance == this)
    {
        s_instance = nullptr;
    }
}

// Define static member
TouchPadRawInputFilter* TouchPadRawInputFilter::s_instance = nullptr;

TouchPadRawInputFilter* TouchPadRawInputFilter::instance()
{
    return s_instance;
}

bool TouchPadRawInputFilter::registerRawInput()
{
    RAWINPUTDEVICE device{};
    device.usUsagePage = 0x000D; // Digitizer
    device.usUsage = 0x0005;     // Touch pad
    device.dwFlags = RIDEV_INPUTSINK;
    device.hwndTarget = m_targetWindow;

    if (::RegisterRawInputDevices(&device, 1, sizeof(device)))
    {
        return true;
    }

    return false;
}

bool TouchPadRawInputFilter::ensurePrecisionTouchpadPresent()
{
    UINT deviceCount = 0;
    const UINT deviceSize = sizeof(RAWINPUTDEVICELIST);

    if (::GetRawInputDeviceList(nullptr, &deviceCount, deviceSize) != 0)
    {
        logWin32Failure(L"GetRawInputDeviceList size query failed");
        return false;
    }

    if (deviceCount == 0)
    {
        return false;
    }

    std::vector<RAWINPUTDEVICELIST> devices(deviceCount);
    if (::GetRawInputDeviceList(devices.data(), &deviceCount, deviceSize) == static_cast<UINT>(-1))
    {
        logWin32Failure(L"GetRawInputDeviceList enumeration failed");
        return false;
    }

    bool found = false;
    for (UINT index = 0; index < deviceCount; ++index)
    {
        const RAWINPUTDEVICELIST& entry = devices[index];
        if (entry.dwType != RIM_TYPEHID)
        {
            continue;
        }

        RID_DEVICE_INFO info{};
        info.cbSize = sizeof(info);
        UINT infoSize = sizeof(info);

        if (::GetRawInputDeviceInfo(entry.hDevice, RIDI_DEVICEINFO, &info, &infoSize) ==
            static_cast<UINT>(-1))
        {
            continue;
        }

        if (info.dwType == RIM_TYPEHID && info.hid.usUsagePage == 0x000D &&
            info.hid.usUsage == 0x0005)
        {
            wchar_t buffer[256];
            swprintf_s(buffer, L"Found precision touchpad: VID_%04X&PID_%04X (version %u)\n",
                       info.hid.dwVendorId, info.hid.dwProductId, info.hid.dwVersionNumber);
            ::OutputDebugStringW(buffer);
            found = true;
            break;
        }
    }

    return found;
}

bool TouchPadRawInputFilter::processRawInput(HRAWINPUT rawInputHandle)
{

    std::vector<ContactLog> contacts;
    contacts.clear();
    UINT requiredSize = 0;
    if (::GetRawInputData(rawInputHandle, RID_INPUT, nullptr, &requiredSize,
                          sizeof(RAWINPUTHEADER)) != 0)
    {
        logWin32Failure(L"GetRawInputData size query failed");
        return false;
    }

    if (requiredSize == 0)
    {
        return false;
    }

    std::vector<BYTE> rawInputBuffer(requiredSize);
    if (::GetRawInputData(rawInputHandle, RID_INPUT, rawInputBuffer.data(), &requiredSize,
                          sizeof(RAWINPUTHEADER)) != requiredSize)
    {
        logWin32Failure(L"GetRawInputData retrieval failed");
        return false;
    }

    RAWINPUT* rawInput = reinterpret_cast<RAWINPUT*>(rawInputBuffer.data());
    if (!rawInput || rawInput->header.dwType != RIM_TYPEHID)
    {
        return false;
    }

    const DWORD reportSize = rawInput->data.hid.dwSizeHid * rawInput->data.hid.dwCount;
    if (reportSize == 0)
    {
        return false;
    }

    std::vector<BYTE> report(reportSize);
    const BYTE* hidRawData = rawInput->data.hid.bRawData;
    std::copy_n(hidRawData, reportSize, report.begin());
    const ULONG reportLength = static_cast<ULONG>(report.size());

    UINT preparsedSize = 0;
    if (::GetRawInputDeviceInfo(rawInput->header.hDevice, RIDI_PREPARSEDDATA, nullptr,
                                &preparsedSize) != 0)
    {
        logWin32Failure(L"GetRawInputDeviceInfo preparsed size failed");
        return false;
    }

    if (preparsedSize == 0)
    {
        return false;
    }

    std::vector<BYTE> preparsed(preparsedSize);
    if (::GetRawInputDeviceInfo(rawInput->header.hDevice, RIDI_PREPARSEDDATA, preparsed.data(),
                                &preparsedSize) != preparsedSize)
    {
        logWin32Failure(L"GetRawInputDeviceInfo preparsed data failed");
        return false;
    }

    HIDP_CAPS caps{};
    if (::HidP_GetCaps(reinterpret_cast<PHIDP_PREPARSED_DATA>(preparsed.data()), &caps) !=
        HIDP_STATUS_SUCCESS)
    {
        ::OutputDebugStringW(L"HidP_GetCaps failed for precision touchpad report.\n");
        return false;
    }

    USHORT valueCapsLength = caps.NumberInputValueCaps;
    if (valueCapsLength == 0)
    {
        return false;
    }

    std::vector<HIDP_VALUE_CAPS> valueCaps(valueCapsLength);
    if (::HidP_GetValueCaps(HidP_Input, valueCaps.data(), &valueCapsLength,
                            reinterpret_cast<PHIDP_PREPARSED_DATA>(preparsed.data())) !=
        HIDP_STATUS_SUCCESS)
    {
        ::OutputDebugStringW(L"HidP_GetValueCaps failed for precision touchpad report.\n");
        return false;
    }
    valueCaps.resize(valueCapsLength);

    std::sort(valueCaps.begin(), valueCaps.end(),
              [](const HIDP_VALUE_CAPS& lhs, const HIDP_VALUE_CAPS& rhs)
              {
                  if (lhs.LinkCollection != rhs.LinkCollection)
                  {
                      return lhs.LinkCollection < rhs.LinkCollection;
                  }
                  if (lhs.UsagePage != rhs.UsagePage)
                  {
                      return lhs.UsagePage < rhs.UsagePage;
                  }
                  const USHORT lhsUsage = firstUsage(lhs);
                  const USHORT rhsUsage = firstUsage(rhs);
                  return lhsUsage < rhsUsage;
              });

    struct ContactState
    {
        std::optional<uint32_t> contactId;
        std::optional<int32_t> x;
        std::optional<int32_t> y;
    };

    std::unordered_map<USHORT, ContactState> contactStates;

    uint32_t scanTime = 0;
    uint32_t contactCount = 0;

    for (const HIDP_VALUE_CAPS& cap : valueCaps)
    {
        const USHORT usagePage = cap.UsagePage;
        const USHORT usage = firstUsage(cap);
        ULONG value = 0;

        const NTSTATUS status =
            ::HidP_GetUsageValue(HidP_Input, usagePage, cap.LinkCollection, usage, &value,
                                 reinterpret_cast<PHIDP_PREPARSED_DATA>(preparsed.data()),
                                 reinterpret_cast<PCHAR>(report.data()), reportLength);
        if (status != HIDP_STATUS_SUCCESS)
        {
            continue;
        }

        if (cap.LinkCollection == 0)
        {
            if (usagePage == 0x0D && usage == 0x56)
            {
                scanTime = value;
            }
            else if (usagePage == 0x0D && usage == 0x54)
            {
                contactCount = value;
            }
            continue;
        }

        ContactState& state = contactStates[cap.LinkCollection];
        if (usagePage == 0x0D && usage == 0x51)
        {
            state.contactId = value;
        }
        else if (usagePage == 0x01 && usage == 0x30)
        {
            state.x = static_cast<qint32>(value);
        }
        else if (usagePage == 0x01 && usage == 0x31)
        {
            state.y = static_cast<qint32>(value);
        }
    }

    // std::vector<ContactLog> contacts;
    contacts.reserve(contactStates.size());
    for (const auto& entry : contactStates)
    {
        const ContactState& state = entry.second;
        if (!state.contactId.has_value() || !state.x.has_value() || !state.y.has_value())
        {
            continue;
        }

        contacts.push_back(ContactLog{*state.contactId, *state.x, *state.y});
    }

    // Remove empty/zero contacts and keep only valid samples.
    std::erase_if(contacts, [](const auto& it) { return it.x == 0 && it.y == 0; });

    // Ensure deterministic ordering by contact id (LinkCollection ordering from HID may vary).
    std::sort(contacts.begin(), contacts.end(),
              [](const ContactLog& a, const ContactLog& b) { return a.id < b.id; });

    // 使用高精度系统时间戳而非 HID scanTime（可能循环）
    LARGE_INTEGER perfCounter;
    ::QueryPerformanceCounter(&perfCounter);
    const int64_t timestamp = perfCounter.QuadPart;
    // qDebug() << contacts.size();
    m_touchpadframes.push_back(Touchpadframe{contacts, timestamp});
    m_lastFrameTimestamp = timestamp;
    // qDebug() << "timestamp" << timestamp;
    // 启动定时器以确保在无输入时也能处理缓冲区
    startProcessingTimer();
    // [BUG] 移动过快丢帧，尤其是末尾, 尝试补偿
    // qDebug() << "finger num: " << contacts.size() << "timestamp: " << timestamp << "scantime "
    //          << scanTime << "x: " << contacts[0].x << "y: " << contacts[0].y;

    return !contacts.empty();
}
// [finished]handleMode不会在touchpad单点长按时不会调用，导致鼠标卡顿
// [TODO] :Input频率过高导致程序卡死，内建eventcache减少频率
void TouchPadRawInputFilter::handleMode()
{
    // qDebug() << m_stateManager->isTouchpadActive();
    // 1.连续滑动时帧间隔处于45000-65000间，视为滑动
    // 2.若帧间隔大于100000,
    // 说明中间无操作, 视为指头的按下,
    // ,延迟0.1ms观测为单指还是多指滑动(cache n 帧判断)，pen模式下附加左键
    // 3.若无新增帧，说明本函数由WM_APP_RESTORE_CURSOR事件触发, 鼠标移动, pen模式下松开左键
    // if (m_touchpadframes.size() > 2)
    // {
    //     // qDebug() << m_touchpadframes[1].scantime - m_touchpadframes[0].scantime;
    //     m_touchpadframes.clear();
    // }
    // 这里先只考虑且默认全局pen模式
    // qDebug() << "size: " << m_touchpadframes.size() << " " << "active? "
    //          << m_stateManager->isTouchpadActive();
    // if (!m_stateManager->isTouchpadActive())
    // {
    //     qDebug() << "lastpoint: " << "x: " << m_touchpadframes.back().contacts[0].x
    //              << "y: " << m_touchpadframes.back().contacts[0].y << '\n';
    // }
    // qDebug() << "m_touchpadframes.size()" << m_touchpadframes.size();
    // [BUG][TODO] 移动轨迹最后4帧x,y固定，丢失最后三帧x,y信息(可能还少录一帧), 计划补帧
    InputSenderT<InputSender::Type::mouse> sender;
    if (states == MouseStates::not_init)
    {
        if (m_stateManager->isTouchpadActive())
        {
            if (m_touchpadframes.size() >= 5)
            {
                auto movedsize = sqrtf(pow(m_touchpadframes.back().contacts[0].x -
                                               m_touchpadframes.front().contacts[0].x,
                                           2) +
                                       pow(m_touchpadframes.back().contacts[0].y -
                                               m_touchpadframes.front().contacts[0].y,
                                           2));
                if (movedsize > 1.5 * m_touchpadframes.size())
                {
                    // 提前判断为move
                    qDebug() << "movedsize > 1.5 * m_touchpadframes.size()";
                    sender.moveTo(m_touchpadframes.front().contacts.front().x,
                                  m_touchpadframes.front().contacts.front().y);
                    sender.pressLeft();
                    states = MouseStates::move;
                    return;
                }
            }
            if (m_touchpadframes.size() <= 15) // 累计10帧
            {
                return;
            }
            else
            {
                // 提前判断为move
                qDebug() << "m_touchpadframes.size() > 10";
                sender.moveTo(m_touchpadframes.front().contacts.front().x,
                              m_touchpadframes.front().contacts.front().y);
                sender.pressLeft();
                states = MouseStates::move;
                return;
            }
        }
        else // 为轨迹结束
        {
            // 点击时长小于10帧, 可能是点击
            auto movedsize = sqrtf(
                pow(m_touchpadframes.back().contacts[0].x - m_touchpadframes.front().contacts[0].x,
                    2) +
                pow(m_touchpadframes.back().contacts[0].y - m_touchpadframes.front().contacts[0].y,
                    2));
            if (movedsize < 1.5 * m_touchpadframes.size())
            {
                // 视为点击
                qDebug() << "movedsize < 1.5 * m_touchpadframes.size()";
                sender.releaseLeft();
                sender.moveTo(m_touchpadframes.front().contacts[0].x,
                              m_touchpadframes.front().contacts[0].y);
                sender.pressLeft();
                sender.releaseLeft();
                m_touchpadframes.clear();
                m_touchpadframes_dealed.clear();
                states = MouseStates::not_init; // 等待下一次轨迹
            }
            else
            {
                // 视为短距离滑动
                qDebug() << "movedsize >= 1.5 * m_touchpadframes.size()";
                sender.releaseLeft();
                sender.pressLeft();
                for (int i = 0; i < m_touchpadframes.size(); i++)
                {
                    sender.moveTo(m_touchpadframes.front().contacts[i].x,
                                  m_touchpadframes.front().contacts[i].y);
                }
                sender.releaseLeft();
                m_touchpadframes.clear();
                m_touchpadframes_dealed.clear();
                states = MouseStates::not_init; // 等待下一次轨迹
            }
        }
    }
    else // 已判断为move
    {
        if (m_stateManager->isTouchpadActive())
        {
            // if (m_touchpadframes.size() <= 4) // 至少缓存3帧, 因为后3帧位置信息不确定, 这里是恒等
            // {
            //     return;
            // }
            // else
            {
                while (m_touchpadframes.size() > 4)
                {
                    sender.moveRelative(
                        m_touchpadframes[1].contacts[0].x - m_touchpadframes[0].contacts[0].x,
                        m_touchpadframes[1].contacts[0].y - m_touchpadframes[0].contacts[0].y);
                    m_touchpadframes_dealed.push_back(m_touchpadframes.front());
                    m_touchpadframes.pop_front();
                }
                return;
            }
        }
        else
        {
            // 滑动结束了, 补全后3帧轨迹
            // 使用已处理帧历史对最后的若干帧做线性外推，减少末尾丢帧带来的突变
            if (!m_touchpadframes_dealed.empty())
            {
                const size_t missing = 3;
                auto extra = extrapolateTrailingFrames(m_touchpadframes_dealed, missing);
                // 将外推帧当作继续的移动事件发送并追加到已处理队列
                for (const auto& ef : extra)
                {
                    if (ef.contacts.empty())
                        continue;
                    const auto& c = ef.contacts.front();
                    // 以相对移动发送（与最后一个已处理帧相比）
                    const auto& last = m_touchpadframes_dealed.back();
                    // 找到同 id 的最后位置（若找不到则以 last.contacts.front() 为基准）
                    int32_t lastX = 0, lastY = 0;
                    bool found = false;
                    for (const auto& lc : last.contacts)
                    {
                        if (lc.id == c.id)
                        {
                            lastX = lc.x;
                            lastY = lc.y;
                            found = true;
                            break;
                        }
                    }
                    if (!found && !last.contacts.empty())
                    {
                        lastX = last.contacts.front().x;
                        lastY = last.contacts.front().y;
                    }

                    const int32_t dx = c.x - lastX;
                    const int32_t dy = c.y - lastY;
                    if (dx != 0 || dy != 0)
                    {
                        sender.moveRelative(dx, dy);
                    }
                    // 即便不需要移动，也将外推帧记为已处理，保持历史连续性
                    m_touchpadframes_dealed.push_back(ef);
                }
            }
            sender.releaseLeft();
            m_touchpadframes.clear();
            m_touchpadframes_dealed.clear();
            states = MouseStates::not_init;
        }
    }
    return;
}

void TouchPadRawInputFilter::fingerReleased()
{
    // pen模式下释放左键
}

TouchpadStateManager* TouchPadRawInputFilter::getStateManager() const
{
    return m_stateManager.get();
}

void TouchPadRawInputFilter::startProcessingTimer()
{
    if (!m_timerActive && m_targetWindow)
    {
        // qDebug() << "startProcessingTimer";
        // [TODO] 会多次注册吗
        if (::SetTimer(m_targetWindow, TIMER_ID, TIMER_INTERVAL_MS, nullptr))
        {
            // qDebug() << "SetTimer";
            m_timerActive = true;
        }
    }
}

void TouchPadRawInputFilter::stopProcessingTimer()
{
    if (m_timerActive && m_targetWindow)
    {
        ::KillTimer(m_targetWindow, TIMER_ID);
        m_timerActive = false;
    }
}

void TouchPadRawInputFilter::onTimer()
{
    // qDebug() << "onTimer\n";
    // 定时器回调：处理缓冲区并检测触摸板释放
    if (m_touchpadframes.empty())
    {
        stopProcessingTimer();
        return;
    }

    // 检查是否超时（没有新帧）
    LARGE_INTEGER perfCounter, freq;
    ::QueryPerformanceCounter(&perfCounter);
    ::QueryPerformanceFrequency(&freq);

    const int64_t currentTime = perfCounter.QuadPart;
    const int64_t elapsedTicks = currentTime - m_lastFrameTimestamp;
    const double elapsedMs = (elapsedTicks * 1000.0) / freq.QuadPart;

    // qDebug() << "elapsedMs:" << elapsedMs;
    // 如果超过 10ms 没有新帧，认为触摸板已释放
    // [TODO] 当前通过定时轮训判断最后一帧与现在时间戳之差，由于轮询频率低60Hz(16.7ms), 不一定准确
    if (elapsedMs > 10.0)
    {
        // 帧处理完全交由handlemode
        // 处理剩余帧
        // static int tp = 0;
        // // qDebug() << "handleMode for end" << tp++;
        // // handleMode();
        // if (m_touchpadframes.size() >= 1 && m_touchpadframes.size() < m_max_touchpadframes) //
        // 点击
        // {
        //     auto tp = m_touchpadframes.back();
        //     m_touchpadframes.clear();
        //     m_touchpadframes.push_back(tp); // 留下一帧方便后续判断
        // }

        // 触发释放事件
        // fingerReleased();

        // 停止定时器
        stopProcessingTimer();

        // 通知状态管理器
        if (m_stateManager)
        {
            // 主动判断
            if (m_stateManager->isTouchpadActive())
            {
                m_stateManager->deactivateTouchpad();
                handleMode(); // 发送轨迹结束通知
            }
            else
            {
                m_stateManager->deactivateTouchpad();
            }
        }
    }
}

#endif // _WIN32
