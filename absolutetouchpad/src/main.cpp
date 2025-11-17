// absolutetouchpad.cpp: 定义应用程序的入口点。
#include "main.h"
#include <QApplication>
#include <QDebug>
#include <QMainWindow>
#include <QObject>
#include <hidapi.h>
#include <rawinputfilter.h>
#include <windows.h>
#include <mainwidow.h>
// 精确式触摸板不触发touchevent, 一指视为鼠标，二指视为滚轮，3/4/5指由操作系统拦截处理
int main(int argc, char* argv[])
{
    QApplication app(argc, argv);

    Mainwindow mainWindow;
    mainWindow.show();

    // Ensure native window handle exists and pass it to the filter so
    // RIDEV_INPUTSINK can target this window (receives input even when not focused)
    HWND hwnd = (HWND)mainWindow.winId();

    auto* filter = new TouchPadRawInputFilter(hwnd);
    if (!filter->isRegistered())
    {
        qWarning() << "Precision touchpad raw input registration failed; filter not installed.";
        delete filter;
    }
    else
    {
        qApp->installNativeEventFilter(filter);
        QObject::connect(&app, &QCoreApplication::aboutToQuit, [filter]() {
            qApp->removeNativeEventFilter(filter);
            delete filter;
        });
        qDebug() << "Raw input filter installed.";
    }

    return app.exec();
}