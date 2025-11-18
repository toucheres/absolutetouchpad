// absolutetouchpad.cpp: 定义应用程序的入口点。
#include "main.h"

#include <QtGlobal>
#include <cstdlib>

#ifdef Q_OS_WIN
#include <QDebug>

#include <Windows.h>
#include <atomic>
#include <chrono>
#include <memory>
#include <mousehook.h>
#include <rawinputfilter.h>
namespace
{
    using Clock = std::chrono::steady_clock;
    constexpr auto kTouchpadSuppressDuration = std::chrono::milliseconds(30);

    std::atomic_bool g_touchpadActive{false};
    std::atomic<long long> g_lastTouchpadMicros{0};

    long long nowMicros()
    {
        return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch())
            .count();
    }

    void markTouchpadActive()
    {
        g_touchpadActive.store(true, std::memory_order_relaxed);
        g_lastTouchpadMicros.store(nowMicros(), std::memory_order_relaxed);
    }

    void noteMouseActivity()
    {
        if (!g_touchpadActive.load(std::memory_order_relaxed))
        {
            return;
        }

        const long long last = g_lastTouchpadMicros.load(std::memory_order_relaxed);
        if (last == 0)
        {
            g_touchpadActive.store(false, std::memory_order_relaxed);
            return;
        }

        const long long elapsed = nowMicros() - last;
        const long long windowMicros = std::chrono::duration_cast<std::chrono::microseconds>(
                                            kTouchpadSuppressDuration)
                                            .count();
        if (elapsed > windowMicros)
        {
            g_touchpadActive.store(false, std::memory_order_relaxed);
        }
    }

    bool shouldBlockTouchpad()
    {
        if (!g_touchpadActive.load(std::memory_order_relaxed))
        {
            return false;
        }

        const long long last = g_lastTouchpadMicros.load(std::memory_order_relaxed);
        if (last == 0)
        {
            g_touchpadActive.store(false, std::memory_order_relaxed);
            return false;
        }

        const long long elapsed = nowMicros() - last;
        const long long windowMicros = std::chrono::duration_cast<std::chrono::microseconds>(
                                            kTouchpadSuppressDuration)
                                            .count();
        if (elapsed <= windowMicros)
        {
            return true;
        }

        g_touchpadActive.store(false, std::memory_order_relaxed);
        return false;
    }

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

            RAWINPUTDEVICE mouseDevice{};
            mouseDevice.usUsagePage = 0x01;
            mouseDevice.usUsage = 0x02;
            mouseDevice.dwFlags = RIDEV_INPUTSINK;
            mouseDevice.hwndTarget = hwnd;
            if (!::RegisterRawInputDevices(&mouseDevice, 1, sizeof(mouseDevice)))
            {
                qWarning() << "Mouse RAWINPUT registration failed.";
            }
            return 0;
        }
        case WM_INPUT:
        {
            RAWINPUTHEADER header{};
            UINT headerSize = sizeof(header);
            if (::GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_HEADER, &header,
                                  &headerSize, sizeof(RAWINPUTHEADER)) != sizeof(header))
            {
                qWarning() << "Failed to query RAWINPUT header.";
                break;
            }

            if (header.dwType == RIM_TYPEHID)
            {
                const bool active =
                    context && context->filter &&
                    context->filter->processRawInput(reinterpret_cast<HRAWINPUT>(lParam));
                if (active)
                {
                    markTouchpadActive();
                }
            }
            else if (header.dwType == RIM_TYPEMOUSE)
            {
                noteMouseActivity();
            }

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
    MouseHook hooker{[](WPARAM, const MSLLHOOKSTRUCT& info) -> bool
                     {
                         if (info.flags & LLMHF_INJECTED)
                         {
                             return false;
                         }

                         return shouldBlockTouchpad();
                     }};

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