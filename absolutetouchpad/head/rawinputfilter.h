// Add a native event filter to receive Raw Input HID reports on Windows
#pragma once
#include <qsystemdetection.h>

#ifdef Q_OS_WIN
#include <QAbstractNativeEventFilter>
#include <QByteArray>
#include <QtGlobal>
#include <windows.h>

// TouchPadRawInputFilter installs a RAWINPUT subscription for precision touchpads
// and logs contact information extracted from HID reports.
class TouchPadRawInputFilter : public QAbstractNativeEventFilter
{
public:
	explicit TouchPadRawInputFilter(HWND targetWindow);
	~TouchPadRawInputFilter() override;

	bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result) override;
	bool isRegistered() const { return m_registered; }

private:
	bool registerRawInput();
	bool ensurePrecisionTouchpadPresent();
	void handleRawInput(HRAWINPUT rawInputHandle);

	HWND m_targetWindow = nullptr;
	bool m_registered = false;
};
#endif
