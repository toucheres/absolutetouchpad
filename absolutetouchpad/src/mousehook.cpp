#include "mousehook.h"

#ifdef Q_OS_WIN

#include <QDebug>

#include <utility>

namespace
{
    MouseHook* g_activeHook = nullptr;
}

MouseHook::MouseHook(Callback callback) : m_callback(std::move(callback))
{
    install();
}

MouseHook::~MouseHook()
{
    uninstall();
}

bool MouseHook::install()
{
    if (m_hook)
    {
        return true;
    }

    if (g_activeHook)
    {
        qWarning() << "MouseHook already active.";
        return false;
    }

    m_hook = ::SetWindowsHookExW(WH_MOUSE_LL, &MouseHook::hookProc, nullptr, 0);
    if (!m_hook)
    {
        qWarning() << "Failed to install mouse hook.";
        return false;
    }

    g_activeHook = this;
    return true;
}

void MouseHook::uninstall()
{
    if (!m_hook)
    {
        return;
    }

    ::UnhookWindowsHookEx(m_hook);
    m_hook = nullptr;
    if (g_activeHook == this)
    {
        g_activeHook = nullptr;
    }
}

LRESULT CALLBACK MouseHook::hookProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code < 0)
    {
        return ::CallNextHookEx(nullptr, code, wParam, lParam);
    }

    const MSLLHOOKSTRUCT* data = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
    if (g_activeHook && data)
    {
        if (g_activeHook->m_callback(wParam, *data))
        {
            return 1;
        }
    }

    return ::CallNextHookEx(nullptr, code, wParam, lParam);
}

#endif // Q_OS_WIN
