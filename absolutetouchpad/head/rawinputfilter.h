// Pure Win32 precision touchpad RAWINPUT parser
#pragma once

#ifdef _WIN32
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <queue>
#include <vector>
#include <windows.h>

// Forward declaration
class TouchpadStateManager;

// TouchPadRawInputFilter installs a RAWINPUT subscription for precision touchpads
// and parses contact information from HID reports.
class TouchPadRawInputFilter
{
  public:
    explicit TouchPadRawInputFilter(HWND targetWindow);
    ~TouchPadRawInputFilter();

    // Singleton accessor: returns the global TouchPadRawInputFilter instance if any
    static TouchPadRawInputFilter* instance();

    // Non-copyable and non-movable
    TouchPadRawInputFilter(const TouchPadRawInputFilter&) = delete;
    TouchPadRawInputFilter& operator=(const TouchPadRawInputFilter&) = delete;
    TouchPadRawInputFilter(TouchPadRawInputFilter&&) = delete;
    TouchPadRawInputFilter& operator=(TouchPadRawInputFilter&&) = delete;

    // Parse a single RAWINPUT HID report and update contact state
    bool processRawInput(HRAWINPUT rawInputHandle);

    bool isRegistered() const
    {
        return m_registered;
    }
    struct ContactLog
    {
        uint32_t id;
        int32_t x;
        int32_t y;
    };

    enum class Mode
    {
        simple,
        absmouse,
        pen
    };

    struct Rect
    {
        double x, y, width, height;
    };

    struct Size
    {
        double width, height;
    };

    struct Point
    {
        double x, y;
    };
    struct Touchpadframe
    {
        std::vector<ContactLog> contacts;
        int64_t scantime = 0;
    };
    Mode mode = Mode::simple;
    Rect absMapRect{};
    Size penMapSize{};
    Size screenSize{}; //
    Point mousePos{};
    void handleMode();
    void fingerReleased();

    // 定时器驱动的帧处理（用于无输入时也能及时响应释放）
    void onTimer();
    void startProcessingTimer();
    void stopProcessingTimer();

    // 获取状态管理器（用于外部协调）
    TouchpadStateManager* getStateManager() const;
    const std::deque<Touchpadframe> getFrames() const
    {
        return m_touchpadframes;
    }

  private:
    enum class MouseStates
    {
        not_init,
        click,
        move,
    } states = MouseStates::not_init;
    int fingernum = 0;
    bool registerRawInput();
    bool ensurePrecisionTouchpadPresent();

    std::deque<Touchpadframe> m_touchpadframes;
    std::deque<Touchpadframe> m_touchpadframes_dealed;
    size_t m_max_touchpadframes = 4;
    HWND m_targetWindow = nullptr;
    bool m_registered = false;
    std::unique_ptr<TouchpadStateManager> m_stateManager;

    // Singleton storage
    static TouchPadRawInputFilter* s_instance;

    // 定时器相关
    static constexpr UINT_PTR TIMER_ID = 1;
    static constexpr UINT TIMER_INTERVAL_MS = 16; // ~60Hz
    bool m_timerActive = false;
    int64_t m_lastFrameTimestamp = 0;
};

#endif // _WIN32
