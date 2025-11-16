// absolutetouchpad.cpp: 定义应用程序的入口点。
#include "main.h"
#include <QApplication>
#include <QDebug>
#include <QMainWindow>
#include <hidapi.h>
#include <rawinputfilter.h>
#include <windows.h>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);

    QMainWindow mainWindow;
    mainWindow.show();

    // Ensure native window handle exists and pass it to the filter so
    // RIDEV_INPUTSINK can target this window (receives input even when not focused)
    HWND hwnd = (HWND)mainWindow.winId();

    RawInputFilter* filter = new RawInputFilter(hwnd);

    qApp->installNativeEventFilter(filter);

    qDebug() << "Raw input filter installed.";

    return app.exec();
}