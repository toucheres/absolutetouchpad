// Implements TouchPadRawInputFilter for processing precision touchpad RAWINPUT events.
#include "rawinputfilter.h"

#ifdef _WIN32

#include <algorithm>
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

TouchPadRawInputFilter::TouchPadRawInputFilter(HWND targetWindow) : m_targetWindow(targetWindow)
{
    if (!m_targetWindow)
    {
        ::OutputDebugStringW(L"TouchPadRawInputFilter requires a valid HWND.\n");
        return;
    }

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

    std::erase_if(contacts, [](const auto& it) { return it.x == 0 && it.y == 0; });
    contacts.resize(contactCount);

    // 使用高精度系统时间戳而非 HID scanTime（可能循环）
    LARGE_INTEGER perfCounter;
    ::QueryPerformanceCounter(&perfCounter);
    const int64_t timestamp = perfCounter.QuadPart;
    // qDebug() << contacts.size();
    m_touchpadframes.push_back(Touchpadframe{contacts, timestamp});
    return !contacts.empty();
}
// handleMode不会在touchpad单点长按时不会调用，导致鼠标卡顿
void TouchPadRawInputFilter::handleMode()
{
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
    InputSenderT<InputSender::Type::mouse> sender;
    constexpr size_t cached_frames_nums = 10;
    // qDebug() << "m_touchpadframes.size()" << m_touchpadframes.size();
    if (m_touchpadframes.size() <= 1)
    {
        // 滑动的第一帧，不处理
        return;
    }
    // 检查是否是新滑动
    if (m_touchpadframes.back().scantime - m_touchpadframes[m_touchpadframes.size() - 2].scantime >
        100000)
    {
        // 是新滑动
        // m_touchpadframes.back()为这次滑动的第一帧,m_touchpadframes[m_touchpadframes.size()-
        // 2]为上一次滑动的最后一帧
        // 清除上一次滑动的过时帧
        qDebug() << "新滑动";
        auto tp = m_touchpadframes.back();
        m_touchpadframes.clear();
        m_touchpadframes.push_back(tp);
        return;
    }
    if (m_touchpadframes.size() <= cached_frames_nums)
    {
        // 继续积累帧
        return;
    }

    if (m_touchpadframes.size() >= cached_frames_nums)
    {
        // 计算 m_touchpadframes 中每帧 contacts.size() 的众数视为这n帧的触摸数
        std::unordered_map<size_t, int> freq;
        for (const auto& f : m_touchpadframes)
        {
            ++freq[f.contacts.size()];
        }

        size_t modeContacts = 0;
        int modeCount = 0;
        for (const auto& kv : freq)
        {
            if (kv.second > modeCount || (kv.second == modeCount && kv.first < modeContacts))
            {
                modeContacts = kv.first;
                modeCount = kv.second;
            }
        }

        qDebug() << "mode contacts size:" << static_cast<int>(modeContacts)
                 << "count:" << modeCount;

        if (modeContacts == 1)
        {
            // 单指模式处理
            const Touchpadframe& frame = m_touchpadframes.back();
            if (!frame.contacts.empty())
            {
                const auto& c = frame.contacts.back();
                // sender.moveTo(c.x / 3, c.y / 3);
                qDebug() << "want to :" << c.x << c.y;
            }
        }
        qDebug() << "滑动";
        m_touchpadframes.pop_front(); // 滑动窗口
    }

    return;
    // if (m_touchpadframes.size() % 3 == 0)
    // {
    //     const Touchpadframe& frame = m_touchpadframes.back();
    //     if (frame.contacts.size() == 1) // 单指时视为鼠标move
    //     {
    //         const auto& contact = frame.contacts[0]; // 使用第一个触点
    //         InputSenderT<InputSender::Type::mouse> sender;
    //         // sender.moveTo(contact.x / 3, contact.y / 3);
    //         // qDebug() << m_touchpadframes.size();
    //         // 映射到 absMapRect 并调用 InputSender::moveTo()
    //     }

    //     m_touchpadframes.clear();
    // }
}

void TouchPadRawInputFilter::fingerReleased()
{
    // pen模式下释放左键
}

#endif // _WIN32
