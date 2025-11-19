// Implements TouchPadRawInputFilter for processing precision touchpad RAWINPUT events.
#include "rawinputfilter.h"

#ifdef _WIN32

#include <algorithm>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <Windows.h>
#include <hidsdi.h>
#include <hidusage.h>
#include <stdio.h>
#include <QString>
#include <qstringliteral.h>

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

        std::wstring message = (written && buffer) ? std::wstring(buffer, written) : L"unknown error";
        if (buffer)
        {
            ::LocalFree(buffer);
        }

        // Trim trailing whitespace
        while (!message.empty() && (message.back() == L'\n' || message.back() == L'\r' || message.back() == L' '))
        {
            message.pop_back();
        }

        return message;
    }

    void logWin32Failure(const wchar_t* context)
    {
        const DWORD error = ::GetLastError();
        wchar_t buffer[512];
        swprintf_s(buffer, L"%s: %s (code %lu)\n", context, formatSystemError(error).c_str(), error);
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
    m_touchpadframes.push(Touchpadframe{contacts, static_cast<int64_t>(scanTime)});
    return !contacts.empty();
}

void TouchPadRawInputFilter::handleMode()
{
}

#endif // _WIN32
