// Add a native event filter to receive Raw Input HID reports on Windows
#pragma once
#include <qsystemdetection.h>

#ifdef Q_OS_WIN
#include <QAbstractNativeEventFilter>
#include <QByteArray>
#include <QtGlobal>
#include <windows.h>
#include <QRectF>
// TouchPadRawInputFilter installs a RAWINPUT subscription for precision touchpads
// and logs contact information extracted from HID reports.
class TouchPadRawInputFilter : public QAbstractNativeEventFilter
{
  public:
    explicit TouchPadRawInputFilter(HWND targetWindow);
    ~TouchPadRawInputFilter() override;

    bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) override;

    // Exposes WM_INPUT parsing so non-Qt message pumps can reuse the decoder.
    bool processRawInput(HRAWINPUT rawInputHandle);

    bool isRegistered() const
    {
        return m_registered;
    }
    struct ContactLog
    {
        quint32 id;
        qint32 x;
        qint32 y;
    };
    enum class Mode
    {
        simple,
        absmouse,
        pen
    };
    Mode mode = Mode::simple;
    QRectF absMapRect{};
    QSizeF penMapSize{};
    QSizeF screenSize{};
    QPointF mousePos{};
    void handleMode();
    void handleMode();
  private:
    bool registerRawInput();
    bool ensurePrecisionTouchpadPresent();

    std::vector<ContactLog> contacts;
    std::vector<ContactLog> lastcontacts;
    HWND m_targetWindow = nullptr;
    bool m_registered = false;
};
#endif
