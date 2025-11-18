// Implements TouchPadRawInputFilter for processing precision touchpad RAWINPUT events.
#include "rawinputfilter.h"

#ifdef Q_OS_WIN

#include <QByteArray>
#include <QCursor>
#include <QDebug>
#include <QLatin1Char>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <optional>
#include <unordered_map>
#include <vector>

#include <Windows.h>
#include <hidsdi.h>
#include <hidusage.h>

namespace
{
    QString formatSystemError(DWORD errorCode)
    {
        if (errorCode == 0)
        {
            return QStringLiteral("no error");
        }

        LPWSTR buffer = nullptr;
        const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                            FORMAT_MESSAGE_IGNORE_INSERTS;
        const DWORD written =
            ::FormatMessageW(flags, nullptr, errorCode, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                             reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);

        QString message = written && buffer ? QString::fromWCharArray(buffer, written).trimmed()
                                            : QStringLiteral("unknown error");
        if (buffer)
        {
            ::LocalFree(buffer);
        }

        return message;
    }

    void logWin32Failure(const QString& context)
    {
        const DWORD error = ::GetLastError();
        qWarning().noquote() << QStringLiteral("%1: %2 (code %3)")
                                    .arg(context)
                                    .arg(formatSystemError(error))
                                    .arg(error);
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
        qWarning() << "TouchPadRawInputFilter requires a valid HWND.";
        return;
    }

    if (ensurePrecisionTouchpadPresent())
    {
        qDebug() << "Precision touchpad detected on the system.";
    }
    else
    {
        qWarning() << "Precision touchpad not detected via RAWINPUT device list.";
    }

    m_registered = registerRawInput();
    if (!m_registered)
    {
        logWin32Failure(QStringLiteral("RegisterRawInputDevices failed"));
    }
    else
    {
        qDebug() << "Precision touchpad RAWINPUT registration successful.";
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
            logWin32Failure(QStringLiteral("Failed to unregister RAWINPUT device"));
        }
    }
}

bool TouchPadRawInputFilter::nativeEventFilter(const QByteArray& eventType, void* message,
                                               qintptr* result)
{
    Q_UNUSED(result);

    if (eventType != "windows_generic_MSG" && eventType != "windows_dispatcher_MSG")
    {
        return false;
    }

    MSG* msg = static_cast<MSG*>(message);
    if (!msg || msg->message != WM_INPUT)
    {
        return false;
    }

    processRawInput(reinterpret_cast<HRAWINPUT>(msg->lParam));
    return false;
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
        logWin32Failure(QStringLiteral("GetRawInputDeviceList size query failed"));
        return false;
    }

    if (deviceCount == 0)
    {
        return false;
    }

    std::vector<RAWINPUTDEVICELIST> devices(deviceCount);
    if (::GetRawInputDeviceList(devices.data(), &deviceCount, deviceSize) == static_cast<UINT>(-1))
    {
        logWin32Failure(QStringLiteral("GetRawInputDeviceList enumeration failed"));
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
            const QString deviceId = QStringLiteral("VID_%1&PID_%2")
                                         .arg(info.hid.dwVendorId, 4, 16, QLatin1Char('0'))
                                         .arg(info.hid.dwProductId, 4, 16, QLatin1Char('0'))
                                         .toUpper();
            qDebug().noquote() << QStringLiteral(
                                      "Found precision touchpad raw device: %1 (version %2)")
                                      .arg(deviceId)
                                      .arg(info.hid.dwVersionNumber);
            found = true;
            break;
        }
    }

    return found;
}

bool TouchPadRawInputFilter::processRawInput(HRAWINPUT rawInputHandle)
{
    UINT requiredSize = 0;
    if (::GetRawInputData(rawInputHandle, RID_INPUT, nullptr, &requiredSize,
                          sizeof(RAWINPUTHEADER)) != 0)
    {
        logWin32Failure(QStringLiteral("GetRawInputData size query failed"));
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
        logWin32Failure(QStringLiteral("GetRawInputData retrieval failed"));
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
        logWin32Failure(QStringLiteral("GetRawInputDeviceInfo preparsed size failed"));
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
        logWin32Failure(QStringLiteral("GetRawInputDeviceInfo preparsed data failed"));
        return false;
    }

    HIDP_CAPS caps{};
    if (::HidP_GetCaps(reinterpret_cast<PHIDP_PREPARSED_DATA>(preparsed.data()), &caps) !=
        HIDP_STATUS_SUCCESS)
    {
        qWarning() << "HidP_GetCaps failed for precision touchpad report.";
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
        qWarning() << "HidP_GetValueCaps failed for precision touchpad report.";
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
        std::optional<quint32> contactId;
        std::optional<qint32> x;
        std::optional<qint32> y;
    };

    std::unordered_map<USHORT, ContactState> contactStates;

    quint32 scanTime = 0;
    quint32 contactCount = 0;

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

    std::sort(contacts.begin(), contacts.end(),
              [](const ContactLog& lhs, const ContactLog& rhs) { return lhs.id < rhs.id; });

    QStringList contactLines;
    contactLines.reserve(static_cast<int>(contacts.size()));
    for (const ContactLog& entry : contacts)
    {
        contactLines << QStringLiteral("id=%1 x=%2 y=%3").arg(entry.id).arg(entry.x).arg(entry.y);
    }

    // if (!contactLines.isEmpty())
    // {
    //     qDebug().noquote() << QStringLiteral("PTP scanTime=%1 rawContactCount=%2 -> %3")
    //                               .arg(scanTime)
    //                               .arg(contactCount)
    //                               .arg(contactLines.join(QStringLiteral(" | ")));
    // }
    // else
    // {
    //     qDebug().noquote() << QStringLiteral(
    //                               "PTP scanTime=%1 rawContactCount=%2 (no contacts parsed)")
    //                               .arg(scanTime)
    //                               .arg(contactCount);
    // }
    // handleMode(contacts);
    return !contacts.empty();
}

void TouchPadRawInputFilter::handleMode()
{
    contacts.clear();
    ::SetCursorPos(50, 50);
    // TODO: map contact coordinates into absolute mouse space when Mode::absmouse is enabled.
}

#endif // Q_OS_WIN
