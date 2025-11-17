#ifndef _MAINWINDOW_H_
#define _MAINWINDOW_H_
#include <QMainWindow>
#include <QTouchEvent>
class Mainwindow : public QMainWindow
{
    bool event(QEvent* event) override
    {
        switch (event->type())
        {
        case QEvent::TouchBegin:
        case QEvent::TouchUpdate:
        case QEvent::TouchEnd:
            qDebug() << "QTouchEvent" << event->type();
            // static_cast<QTouchEvent*>(event)->touchPoints()...
            break;
        case QEvent::NativeGesture:
            qDebug() << "QNativeGestureEvent" << static_cast<QNativeGestureEvent*>(event)->value()
                     << static_cast<QNativeGestureEvent*>(event)->gestureType();
            break;
        default:
            break;
        }
        return QMainWindow::event(event);
    }
};
#endif