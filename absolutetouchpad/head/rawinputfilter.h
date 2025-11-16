// Add a native event filter to receive Raw Input HID reports on Windows
#pragma once
#include <qsystemdetection.h>
#ifdef Q_OS_WIN
#include <QAbstractNativeEventFilter>
#include <QByteArray>
#include <QtGlobal>
#include <windows.h>
#include <functional>

class RawInputFilter : public QAbstractNativeEventFilter
{
public:
    explicit RawInputFilter(HWND target = nullptr);
    ~RawInputFilter() override;
    bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result) override;

    // set a callback to receive textual log messages from the filter
    void setLogCallback(std::function<void(const QString&)> cb) { m_logCallback = std::move(cb); }

private:
    void registerRawInput();
    void unregisterRawInput();
    HWND m_target;
    // set of device paths that we identified as precision touchpads (via hidapi)
    std::vector<std::string> m_touchpadPaths;
    // Raw Input device handles detected as touchpads (UsagePage=13, Usage=5)
    std::vector<HANDLE> m_touchpadDeviceHandles;
    // optional logging callback (not owned)
    std::function<void(const QString&)> m_logCallback;
};
#endif
