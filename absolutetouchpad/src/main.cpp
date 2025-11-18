// absolutetouchpad.cpp: 定义应用程序的入口点。
#include "main.h"

#include <QtGlobal>
#include <cstdlib>

#ifdef Q_OS_WIN
#include <QDebug>

#include <Windows.h>
#include <memory>
#include <mousehook.h>
#include <rawinputfilter.h>
bool enable;
bool lastistoucpad;
namespace
{
    struct WindowContext
    {
        std::unique_ptr<TouchPadRawInputFilter> filter;
    };

    LRESULT CALLBACK RawInputWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        auto* context = reinterpret_cast<WindowContext*>(::GetWindowLongPtr(hwnd, GWLP_USERDATA));
        static size_t times = 0;
        switch (message)
        {
        case WM_NCCREATE:
        {
            auto* createStruct = reinterpret_cast<CREATESTRUCT*>(lParam);
            auto* passedContext = static_cast<WindowContext*>(createStruct->lpCreateParams);
            ::SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(passedContext));
            return TRUE;
        }
        case WM_CREATE:
        {
            if (context)
            {
                context->filter = std::make_unique<TouchPadRawInputFilter>(hwnd);
                if (!context->filter->isRegistered())
                {
                    qWarning() << "Precision touchpad RAWINPUT registration failed.";
                }
                else
                {
                    qDebug() << "Precision touchpad RAWINPUT interception active.";
                }
            }
            return 0;
        }
        case WM_INPUT:
        {
            if (context && context->filter)
            {
                enable = context->filter->processRawInput(reinterpret_cast<HRAWINPUT>(lParam));
            }
            // qDebug() << "times: " << times << "raw input";
            times++;
            return 0;
        }
        case WM_MOUSEMOVE:
        {
            // qDebug() << "times: " << times << "mouse move";
            times++;
            return 0;
        }
        case WM_CLOSE:
        {
            ::DestroyWindow(hwnd);
            return 0;
        }
        case WM_DESTROY:
        {
            if (context)
            {
                context->filter.reset();
                delete context;
                ::SetWindowLongPtr(hwnd, GWLP_USERDATA, 0);
            }
            ::PostQuitMessage(0);
            return 0;
        }
        default:
            break;
        }

        return ::DefWindowProc(hwnd, message, wParam, lParam);
    }
} // namespace

int main(int argc, char* argv[])
{
    Q_UNUSED(argc);
    Q_UNUSED(argv);

    const HINSTANCE instance = ::GetModuleHandle(nullptr);

    const wchar_t kWindowClassName[] = L"PrecisionTouchpadRawInputSink";

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = RawInputWindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = ::LoadCursor(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kWindowClassName;

    if (!::RegisterClassExW(&windowClass))
    {
        qWarning() << "Failed to register raw input host window class.";
        return EXIT_FAILURE;
    }

    auto* context = new WindowContext{};
    HWND hwnd = ::CreateWindowExW(0, kWindowClassName, L"Precision Touchpad RAWINPUT Monitor",
                                  WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 800, 600,
                                  nullptr, nullptr, instance, context);

    if (!hwnd)
    {
        qWarning() << "Failed to create raw input host window.";
        delete context;
        return EXIT_FAILURE;
    }

    ::ShowWindow(hwnd, SW_SHOW);
    ::UpdateWindow(hwnd);
    MouseHook hooker{[](WPARAM e, const MSLLHOOKSTRUCT& info)
                     {
                         qDebug() << "enable: " << enable << "Hook" << info.pt.x << info.pt.y << e;
                         return 0;
                     }};
    if (!hooker.install())
    {
        qWarning() << "Failed to install global mouse hook.";
    }

    MSG msg;
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    return static_cast<int>(msg.wParam);
}

#else // !Q_OS_WIN

#include <QDebug>

int main(int argc, char* argv[])
{
    Q_UNUSED(argc);
    Q_UNUSED(argv);
    qWarning() << "Precision touchpad RAWINPUT interception requires Windows.";
    return EXIT_FAILURE;
}

#endif // Q_OS_WIN