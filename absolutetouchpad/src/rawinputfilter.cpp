#include <rawinputfilter.h>
#include <QDebug>

#ifdef Q_OS_WIN
#include <Windows.h>
#include <cstring>
#include <hidapi.h>
#include <vector>
#include <string>
#include <algorithm>

static void logDeviceName(HANDLE hDevice)
{
    UINT nameSize = 0;
    GetRawInputDeviceInfoA(hDevice, RIDI_DEVICENAME, NULL, &nameSize);
    if (nameSize > 0) {
        std::string name;
        name.resize(nameSize);
        GetRawInputDeviceInfoA(hDevice, RIDI_DEVICENAME, &name[0], &nameSize);
        qDebug() << "Device name:" << QString::fromStdString(name);
        // if a log callback is set, emit as well
        // (we can't access member callback here; caller will log via other points)
    }
}

static void logDeviceInfo(HANDLE hDevice)
{
    RID_DEVICE_INFO info;
    UINT cbSize = sizeof(info);
    memset(&info, 0, sizeof(info));
    info.cbSize = cbSize;
    if (GetRawInputDeviceInfo(hDevice, RIDI_DEVICEINFO, &info, &cbSize) == (UINT)-1) {
        qWarning() << "GetRawInputDeviceInfo failed";
        return;
    }
    if (info.dwType == RIM_TYPEHID) {
        qDebug() << "  HID: Vendor" << info.hid.dwVendorId
                 << "Product" << info.hid.dwProductId
                 << "UsagePage" << QString::number(info.hid.usUsagePage, 16)
                 << "Usage" << QString::number(info.hid.usUsage, 16);
    } else if (info.dwType == RIM_TYPEMOUSE) {
        qDebug() << "  MOUSE device";
    } else if (info.dwType == RIM_TYPEKEYBOARD) {
        qDebug() << "  KEYBOARD device";
    }
}

static void listRawInputDevices()
{
    UINT numDevices = 0;
    if (GetRawInputDeviceList(NULL, &numDevices, sizeof(RAWINPUTDEVICELIST)) != 0) {
        qWarning() << "GetRawInputDeviceList failed to get count";
        return;
    }
    if (numDevices == 0) {
        qDebug() << "No raw input devices";
        return;
    }
    std::vector<RAWINPUTDEVICELIST> devs(numDevices);
    if (GetRawInputDeviceList(devs.data(), &numDevices, sizeof(RAWINPUTDEVICELIST)) == (UINT)-1) {
        qWarning() << "GetRawInputDeviceList failed to enumerate";
        return;
    }
    qDebug() << "Raw input devices count:" << numDevices;
    for (UINT i = 0; i < numDevices; ++i) {
        logDeviceName(devs[i].hDevice);
        logDeviceInfo(devs[i].hDevice);
    }
}

// helper method to invoke the instance callback if available
static void invokeLogCallback(const std::function<void(const QString&)> &cb, const QString &msg)
{
    if (cb) cb(msg);
}

// Find raw input devices whose RID_DEVICE_INFO indicates Digitizer TouchPad (UsagePage=13, Usage=5)
static std::vector<std::pair<HANDLE, std::string>> findRawTouchpadDevices()
{
    std::vector<std::pair<HANDLE, std::string>> found;
    UINT numDevices = 0;
    if (GetRawInputDeviceList(NULL, &numDevices, sizeof(RAWINPUTDEVICELIST)) != 0) return found;
    if (numDevices == 0) return found;
    std::vector<RAWINPUTDEVICELIST> devs(numDevices);
    if (GetRawInputDeviceList(devs.data(), &numDevices, sizeof(RAWINPUTDEVICELIST)) == (UINT)-1) return found;

    for (UINT i = 0; i < numDevices; ++i) {
        RID_DEVICE_INFO info;
        UINT cbSize = sizeof(info);
        memset(&info, 0, sizeof(info));
        info.cbSize = cbSize;
        if (GetRawInputDeviceInfo(devs[i].hDevice, RIDI_DEVICEINFO, &info, &cbSize) == (UINT)-1) continue;
        if (info.dwType == RIM_TYPEHID) {
            if (info.hid.usUsagePage == 0x0D && info.hid.usUsage == 0x05) {
                // likely touchpad
                // get device name
                UINT nameSize = 0;
                GetRawInputDeviceInfoA(devs[i].hDevice, RIDI_DEVICENAME, NULL, &nameSize);
                std::string devname;
                if (nameSize > 0) {
                    devname.resize(nameSize);
                    if (GetRawInputDeviceInfoA(devs[i].hDevice, RIDI_DEVICENAME, &devname[0], &nameSize) == (UINT)-1) devname.clear();
                }
                found.emplace_back(devs[i].hDevice, devname);
            }
        }
    }
    return found;
}

static bool ContainsIgnoreCase(const std::wstring &s, const std::wstring &sub) {
    auto it = std::search(
        s.begin(), s.end(),
        sub.begin(), sub.end(),
        [](wchar_t a, wchar_t b){ return towlower(a) == towlower(b); }
    );
    return it != s.end();
}

// Use hidapi to enumerate HID devices and try to detect likely precision touchpads
// Returns a vector of device paths (UTF-8) that are likely touchpads
static std::vector<std::string> listHidApiDevices()
{
    if (hid_init() != 0) {
        qWarning() << "hidapi init failed";
        return {};
    }

    struct hid_device_info *devs = hid_enumerate(0x0, 0x0);
    struct hid_device_info *cur = devs;
    qDebug() << "hidapi devices list:";
    std::vector<std::string> foundPaths;
    while (cur) {
        bool likely = false;

        // Check usage page/usage for Digitizers (0x0D)
        if (cur->usage_page == 0x0D) {
            if (cur->usage == 0x05 || cur->usage == 0x04) {
                likely = true;
            }
        }

        // Check product string or manufacturer for keywords
        if (!likely && cur->product_string) {
            std::wstring prod(cur->product_string);
            const std::vector<std::wstring> keywords = { L"precision", L"touchpad", L"synaptics", L"elan", L"trackpad", L"hidi2c", L"i2c" };
            for (auto &kw : keywords) {
                if (ContainsIgnoreCase(prod, kw)) { likely = true; break; }
            }
        }

        QString qpath = cur->path ? QString::fromUtf8(cur->path) : QString();
        qDebug() << "  HID: Vendor" << cur->vendor_id
                 << "Product" << cur->product_id
                 << "UsagePage" << QString::number(cur->usage_page, 16)
                 << "Usage" << QString::number(cur->usage, 16)
                 << "Path" << qpath
                 << "ProductStr" << (cur->product_string ? QString::fromWCharArray(cur->product_string) : QString())
                 << (likely ? "[Likely Touchpad]" : "");

        if (likely && cur->path) {
            // store the raw device path (UTF-8)
            foundPaths.emplace_back(cur->path);
        }

        cur = cur->next;
    }
    hid_free_enumeration(devs);
    // do not call hid_exit here; keep library initialized if further opens are needed
    return foundPaths;
}

RawInputFilter::RawInputFilter(HWND target)
    : m_target(target)
{
    // List devices so user can inspect UsagePage/Usage for their touchpad
    listRawInputDevices();
    // Additionally enumerate via hidapi for richer info (product strings, usage_page, etc.)
    m_touchpadPaths = listHidApiDevices();
    // Also detect Raw Input devices directly whose device info reports UsagePage=13, Usage=5 (TouchPad)
    auto rawFound = findRawTouchpadDevices();
    for (auto &p : rawFound) {
        m_touchpadDeviceHandles.push_back(p.first);
        if (!p.second.empty()) m_touchpadPaths.push_back(p.second);
        qDebug() << "Detected raw touchpad device handle:" << (quintptr)p.first << "name:" << QString::fromStdString(p.second);
    }
    registerRawInput();
}

RawInputFilter::~RawInputFilter()
{
    unregisterRawInput();
    // cleanup hidapi
    hid_exit();
}

void RawInputFilter::registerRawInput()
{
    // Register both mouse and digitizer (touchpad) pages.
    // Many precision touchpads report under UsagePage = 0x0D (Digitizers)
    // Adjust the usUsage value after inspecting the device list above.
    RAWINPUTDEVICE rids[3];
    memset(rids, 0, sizeof(rids));

    // Mouse (fallback)
    rids[0].usUsagePage = 0x01; // Generic Desktop Controls
    rids[0].usUsage = 0x02;     // Mouse
    rids[0].dwFlags = m_target ? RIDEV_INPUTSINK : 0;
    rids[0].hwndTarget = m_target;

    // Digitizer (touchpad) - many devices use usage 0x04 or 0x05; change if needed
    // Register both common digitizer usages (0x04 and 0x05) to cover more devices
    rids[1].usUsagePage = 0x0D; // Digitizers
    rids[1].usUsage = 0x04;     // Digitizer collection (may be TouchPad/TouchScreen)
    rids[1].dwFlags = m_target ? RIDEV_INPUTSINK : 0;
    rids[1].hwndTarget = m_target;

    rids[2].usUsagePage = 0x0D; // Digitizers
    rids[2].usUsage = 0x05;     // Touch Pad (some devices use usage 0x05)
    rids[2].dwFlags = m_target ? RIDEV_INPUTSINK : 0;
    rids[2].hwndTarget = m_target;

    if (!RegisterRawInputDevices(rids, 3, sizeof(RAWINPUTDEVICE))) {
        DWORD err = GetLastError();
        qWarning() << "RegisterRawInputDevices failed:" << err;
        if (err == ERROR_INVALID_PARAMETER) {
            qWarning() << "Invalid parameter when registering raw input devices."
                       << "If you constructed RawInputFilter without a valid HWND,"
                       << "try passing a window handle or allow registration without RIDEV_INPUTSINK.";
        }

        // Fallback: if we attempted INPUTSINK but failed, try registering without it
        if (m_target && (rids[0].dwFlags != 0 || rids[1].dwFlags != 0 || rids[2].dwFlags != 0)) {
            rids[0].dwFlags = 0;
            rids[0].hwndTarget = NULL;
            rids[1].dwFlags = 0;
            rids[1].hwndTarget = NULL;
            rids[2].dwFlags = 0;
            rids[2].hwndTarget = NULL;
            if (RegisterRawInputDevices(rids, 3, sizeof(RAWINPUTDEVICE))) {
                qDebug() << "RegisterRawInputDevices succeeded without INPUTSINK (will receive raw input only when focused)";
            } else {
                qWarning() << "Fallback RegisterRawInputDevices also failed:" << GetLastError();
            }
        }
    } else {
        qDebug() << "RegisterRawInputDevices succeeded.";
    }
}

void RawInputFilter::unregisterRawInput()
{
    RAWINPUTDEVICE rid;
    rid.usUsagePage = 0x01;
    rid.usUsage = 0x02;
    rid.dwFlags = RIDEV_REMOVE;
    rid.hwndTarget = NULL;
    RegisterRawInputDevices(&rid, 1, sizeof(rid));

    RAWINPUTDEVICE rid2;
    rid2.usUsagePage = 0x0D;
    rid2.usUsage = 0x04;
    rid2.dwFlags = RIDEV_REMOVE;
    rid2.hwndTarget = NULL;
    RegisterRawInputDevices(&rid2, 1, sizeof(rid2));

    RAWINPUTDEVICE rid3;
    rid3.usUsagePage = 0x0D;
    rid3.usUsage = 0x05;
    rid3.dwFlags = RIDEV_REMOVE;
    rid3.hwndTarget = NULL;
    RegisterRawInputDevices(&rid3, 1, sizeof(rid3));
}

bool RawInputFilter::nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result)
{
    if (eventType != "windows_generic_MSG" && eventType != "windows_dispatcher_MSG")
        return false;

    MSG *msg = static_cast<MSG*>(message);
    if (!msg) return false;

    if (msg->message == WM_INPUT)
    {
        HRAWINPUT hRaw = reinterpret_cast<HRAWINPUT>(msg->lParam);
        UINT size = 0;
        if (GetRawInputData(hRaw, RID_INPUT, NULL, &size, sizeof(RAWINPUTHEADER)) == (UINT)-1) {
            qWarning() << "GetRawInputData size failed";
            return false;
        }
        QByteArray buffer;
        buffer.resize(size);
        if (GetRawInputData(hRaw, RID_INPUT, buffer.data(), &size, sizeof(RAWINPUTHEADER)) != size) {
            qWarning() << "GetRawInputData read failed";
            return false;
        }
        RAWINPUT* raw = reinterpret_cast<RAWINPUT*>(buffer.data());
        // Filter: if we have detected touchpad paths, only process input from those devices
        // If we detected touchpad handles from Raw Input device info, prefer handle comparison (most reliable)
        if (!m_touchpadDeviceHandles.empty()) {
            bool matchedHandle = false;
            for (auto h : m_touchpadDeviceHandles) {
                if (h == raw->header.hDevice) { matchedHandle = true; break; }
            }
            if (!matchedHandle) {
                // not from a touchpad handle we detected
                return false;
            }
        } else if (!m_touchpadPaths.empty()) {
            // fallback: match by device name/path strings as before
            UINT nameSize = 0;
            if (GetRawInputDeviceInfoA(raw->header.hDevice, RIDI_DEVICENAME, NULL, &nameSize) != (UINT)-1 && nameSize > 0) {
                std::string devname;
                devname.resize(nameSize);
                if (GetRawInputDeviceInfoA(raw->header.hDevice, RIDI_DEVICENAME, &devname[0], &nameSize) != (UINT)-1) {
                    qDebug() << "WM_INPUT from device name:" << QString::fromStdString(devname);
                    for (const auto &p : m_touchpadPaths) qDebug() << "  candidate path:" << QString::fromStdString(p);
                    bool matched = false;
                    for (const auto &p : m_touchpadPaths) {
                        if (p.empty()) continue;
                        if (devname.find(p) != std::string::npos) { matched = true; break; }
                        std::string pLower = p; std::string dnLower = devname;
                        std::transform(pLower.begin(), pLower.end(), pLower.begin(), ::tolower);
                        std::transform(dnLower.begin(), dnLower.end(), dnLower.begin(), ::tolower);
                        if (dnLower.find(pLower) != std::string::npos) { matched = true; break; }
                        auto pos = p.find('#');
                        if (pos != std::string::npos) {
                            std::string tail = p.substr(pos);
                            if (!tail.empty() && (devname.find(tail) != std::string::npos || dnLower.find(tail) != std::string::npos)) { matched = true; break; }
                        }
                    }
                    qDebug() << "  matched touchpad?" << (matched ? "YES" : "NO");
                    if (!matched) return false;
                }
            }
        }
        if (raw->header.dwType == RIM_TYPEHID)
        {
            auto &hid = raw->data.hid;
            int reportSize = static_cast<int>(hid.dwSizeHid * hid.dwCount);
            QByteArray report(reinterpret_cast<const char*>(hid.bRawData), reportSize);
            qDebug() << "HID report from device" << (quintptr)(raw->header.hDevice) << ":"
                     << report.toHex(' ');
            // TODO: parse PTP HID report here
        }
        return false;
    }
    return false;
}

#endif
