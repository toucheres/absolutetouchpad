// absolutetouchpad.cpp: 定义应用程序的入口点。
#include "main.h"

#include <cstdlib>

#ifdef _WIN32
#include <Windows.h>
#include <memory>
#include <mousehook.h>
#include <rawinputfilter.h>
#include <TouchpadStateManager.h>

constexpr UINT WM_APP_RESTORE_CURSOR = WM_APP + 1;

struct WindowContext
{
    std::unique_ptr<TouchPadRawInputFilter> filter;
    std::unique_ptr<TouchpadStateManager> stateManager;
    std::unique_ptr<MouseHook> mouseHook;
    HWND hwnd = nullptr;
};

bool handleTouchpadMouseEvent(WindowContext* ctx, WPARAM e, const MSLLHOOKSTRUCT& inf)
{
    if (!ctx || !ctx->stateManager)
    {
        return false;
    }

    // 检查是否在压制窗口内
    if (ctx->stateManager->isWithinSuppressWindow())
    {
        ctx->stateManager->saveCursorIfNeeded();
        if (ctx->filter)
        {
            ctx->filter->handleMode();
        }
        return true; // 阻断此鼠标事件
    }

    // 超时或非活动状态，请求恢复光标
    if (ctx->stateManager->hasTimedOut() || !ctx->stateManager->isTouchpadActive())
    {
        ctx->stateManager->deactivateTouchpad();
        ctx->stateManager->requestCursorRestore(WM_APP_RESTORE_CURSOR);
    }

    return false;
}

void noteMouseActivity(WindowContext* ctx)
{
    if (!ctx || !ctx->stateManager)
    {
        return;
    }

    if (!ctx->stateManager->isTouchpadActive())
    {
        return;
    }

    if (ctx->stateManager->hasTimedOut())
    {
        ctx->stateManager->deactivateTouchpad();
        ctx->stateManager->requestCursorRestore(WM_APP_RESTORE_CURSOR);
    }
}

LRESULT CALLBACK RawInputWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    WindowContext* ctx = reinterpret_cast<WindowContext*>(::GetWindowLongPtr(hwnd, GWLP_USERDATA));

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
        if (ctx)
        {
            ctx->hwnd = hwnd;
            ctx->stateManager = std::make_unique<TouchpadStateManager>(hwnd);
            ctx->filter = std::make_unique<TouchPadRawInputFilter>(hwnd);
            
            if (!ctx->filter->isRegistered())
            {
                ::OutputDebugStringW(L"Precision touchpad RAWINPUT registration failed.\n");
            }
            else
            {
                ::OutputDebugStringW(L"Precision touchpad RAWINPUT interception active.\n");
            }

            // 安装鼠标钩子
            ctx->mouseHook = std::make_unique<MouseHook>([ctx](WPARAM e, const MSLLHOOKSTRUCT& info) -> bool
            {
                if (info.flags & LLMHF_INJECTED)
                {
                    return false;
                }
                return handleTouchpadMouseEvent(ctx, e, info);
            });
        }

        // 注册鼠标 RAWINPUT 以接收被动活动检测
        RAWINPUTDEVICE mouseDevice{};
        mouseDevice.usUsagePage = 0x01;
        mouseDevice.usUsage = 0x02;
        mouseDevice.dwFlags = RIDEV_INPUTSINK;
        mouseDevice.hwndTarget = hwnd;
        if (!::RegisterRawInputDevices(&mouseDevice, 1, sizeof(mouseDevice)))
        {
            ::OutputDebugStringW(L"Mouse RAWINPUT registration failed.\n");
        }
        return 0;
    }
    case WM_INPUT:
    {
        if (!ctx)
        {
            break;
        }

        RAWINPUTHEADER header{};
        UINT headerSize = sizeof(header);
        if (::GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_HEADER, &header,
                              &headerSize, sizeof(RAWINPUTHEADER)) != sizeof(header))
        {
            ::OutputDebugStringW(L"Failed to query RAWINPUT header.\n");
            break;
        }

        if (header.dwType == RIM_TYPEHID)
        {
            const bool active = ctx->filter && ctx->filter->processRawInput(reinterpret_cast<HRAWINPUT>(lParam));
            if (active && ctx->stateManager)
            {
                ctx->stateManager->markTouchpadActive();
                // 立即处理触摸板数据帧，驱动鼠标移动
                if (ctx->filter)
                {
                    ctx->filter->handleMode();
                }
            }
            else if (ctx->stateManager)
            {
                ctx->stateManager->deactivateTouchpad();
                ctx->stateManager->requestCursorRestore(WM_APP_RESTORE_CURSOR);
            }
        }
        else if (header.dwType == RIM_TYPEMOUSE)
        {
            noteMouseActivity(ctx);
        }

        return 0;
    }
    case WM_CLOSE:
    {
        ::DestroyWindow(hwnd);
        return 0;
    }
    case WM_DESTROY:
    {
        if (ctx)
        {
            if (ctx->stateManager)
            {
                ctx->stateManager->deactivateTouchpad();
                ctx->stateManager->restoreCursorIfSaved();
            }
            ctx->mouseHook.reset();
            ctx->filter.reset();
            ctx->stateManager.reset();
            delete ctx;
            ::SetWindowLongPtr(hwnd, GWLP_USERDATA, 0);
        }
        ::PostQuitMessage(0);
        return 0;
    }
    case WM_APP_RESTORE_CURSOR:
    {
        if (ctx && ctx->stateManager)
        {
            ctx->stateManager->onRestoreMessageReceived();
            ctx->stateManager->restoreCursorIfSaved();
        }
        return 0;
    }
    default:
        break;
    }

    return ::DefWindowProc(hwnd, message, wParam, lParam);
}

int main(int argc, char* argv[])
{
    (void)argc;
    (void)argv;

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
        ::OutputDebugStringW(L"Failed to register raw input host window class.\n");
        return EXIT_FAILURE;
    }

    auto* context = new WindowContext{};
    HWND hwnd = ::CreateWindowExW(0, kWindowClassName, L"Precision Touchpad RAWINPUT Monitor",
                                  WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 800, 600,
                                  nullptr, nullptr, instance, context);

    if (!hwnd)
    {
        ::OutputDebugStringW(L"Failed to create raw input host window.\n");
        delete context;
        return EXIT_FAILURE;
    }

    ::ShowWindow(hwnd, SW_SHOW);
    ::UpdateWindow(hwnd);

    MSG msg;
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    return static_cast<int>(msg.wParam);
}

#else // !_WIN32

#include <stdio.h>

int main(int argc, char* argv[])
{
    (void)argc;
    (void)argv;
    fprintf(stderr, "Precision touchpad RAWINPUT interception requires Windows.\n");
    return EXIT_FAILURE;
}

#endif // _WIN32