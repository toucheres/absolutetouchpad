// Pure Win32 precision touchpad RAWINPUT parser
#pragma once

#ifdef _WIN32
#include <windows.h>
#include <atomic>
#include <queue>
#include <vector>
#include <cstdint>

// TouchPadRawInputFilter installs a RAWINPUT subscription for precision touchpads
// and parses contact information from HID reports.
class TouchPadRawInputFilter
{
  public:
    explicit TouchPadRawInputFilter(HWND targetWindow);
    ~TouchPadRawInputFilter();

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
    
    Mode mode = Mode::simple;
    Rect absMapRect{};
    Size penMapSize{};
    Size screenSize{};
    Point mousePos{};
    void handleMode();
    
    struct Touchpadframe
    {
        std::vector<ContactLog> contacts;
        int64_t scantime = 0;
    };

  private:
    bool registerRawInput();
    bool ensurePrecisionTouchpadPresent();
    
    std::vector<Touchpadframe> m_touchpadframes;
    HWND m_targetWindow = nullptr;
    bool m_registered = false;
};

#endif // _WIN32
