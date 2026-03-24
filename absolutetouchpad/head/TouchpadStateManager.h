#pragma once

#ifdef _WIN32
#include <Windows.h>
#include <atomic>
#include <chrono>
#include <qdebug.h>
// 触摸板状态机：管理触摸板活动、光标保存/恢复的完整生命周期
class TouchpadStateManager
{
public:
    using Clock = std::chrono::steady_clock;
    using Duration = std::chrono::milliseconds;

    explicit TouchpadStateManager(HWND mainWindow, Duration suppressDuration = std::chrono::milliseconds(12))
        : m_mainWindow(mainWindow)
        , m_suppressDuration(suppressDuration)
    {
    }

    // 标记触摸板激活（收到 RAWINPUT HID 报告）
    void markTouchpadActive()
    {
        m_touchpadActive.store(true, std::memory_order_relaxed);
        m_lastTouchpadMicros.store(nowMicros(), std::memory_order_relaxed);
    }

    // 检查触摸板是否仍处于活动状态
    bool isTouchpadActive() const
    {
        return m_touchpadActive.load(std::memory_order_relaxed);
    }

    // 强制停用触摸板
    void deactivateTouchpad()
    {
        m_touchpadActive.store(false, std::memory_order_relaxed);
    }

    // 保存光标位置（如果尚未保存）
    void saveCursorIfNeeded()
    {
        bool expected = false;
        if (!m_cursorSaved.compare_exchange_strong(expected, true, std::memory_order_relaxed))
        {
            return;
        }

        POINT pt{};
        if (::GetCursorPos(&pt))
        {
            m_savedCursorX.store(pt.x, std::memory_order_relaxed);
            m_savedCursorY.store(pt.y, std::memory_order_relaxed);
            qDebug() << "save:" <<"x:"<< pt.x <<" y:"<< pt.y;
        }
    }

    // 恢复光标位置（如果已保存）
    void restoreCursorIfSaved()
    {
        if (!m_cursorSaved.exchange(false, std::memory_order_relaxed))
        {
            return;
        }

        const LONG x = m_savedCursorX.load(std::memory_order_relaxed);
        const LONG y = m_savedCursorY.load(std::memory_order_relaxed);
        ::SetCursorPos(x, y);
        qDebug() << "restore:" << "x:" << x << " y:" << y;
    }

    // 请求光标恢复（通过消息队列异步触发）
    void requestCursorRestore(UINT restoreMessage)
    {
        if (!m_cursorSaved.load(std::memory_order_relaxed))
        {
            return;
        }

        bool expected = false;
        if (!m_restorePosted.compare_exchange_strong(expected, true, std::memory_order_relaxed))
        {
            return;
        }

        if (!m_mainWindow || !::PostMessage(m_mainWindow, restoreMessage, 0, 0))
        {
            m_restorePosted.store(false, std::memory_order_relaxed);
        }
    }

    // 处理恢复消息到达时的清理
    void onRestoreMessageReceived()
    {
        m_restorePosted.store(false, std::memory_order_relaxed);
    }

    // 检查触摸板事件是否在压制窗口内
    bool isWithinSuppressWindow() const
    {
        if (!m_touchpadActive.load(std::memory_order_relaxed))
        {
            return false;
        }

        const long long last = m_lastTouchpadMicros.load(std::memory_order_relaxed);
        if (last == 0)
        {
            return false;
        }

        const long long elapsed = nowMicros() - last;
        const long long windowMicros = std::chrono::duration_cast<std::chrono::microseconds>(m_suppressDuration).count();
        return elapsed <= windowMicros;
    }

    // 检查触摸板是否超时（用于被动超时检查）
    bool hasTimedOut() const
    {
        if (!m_touchpadActive.load(std::memory_order_relaxed))
        {
            return false;
        }

        const long long last = m_lastTouchpadMicros.load(std::memory_order_relaxed);
        if (last == 0)
        {
            return true;
        }

        const long long elapsed = nowMicros() - last;
        const long long windowMicros = std::chrono::duration_cast<std::chrono::microseconds>(m_suppressDuration).count();
        return elapsed > windowMicros;
    }

private:
    static long long nowMicros()
    {
        return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();
    }

    HWND m_mainWindow;
    Duration m_suppressDuration;

    std::atomic_bool m_touchpadActive{false};
    std::atomic<long long> m_lastTouchpadMicros{0};
    std::atomic_bool m_cursorSaved{false};
    std::atomic<LONG> m_savedCursorX{0};
    std::atomic<LONG> m_savedCursorY{0};
    std::atomic_bool m_restorePosted{false};
};

#endif // _WIN32
