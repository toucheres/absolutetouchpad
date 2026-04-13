#pragma once

#include <iostream>
#include <qdebug.h>
#include <qlogging.h>
#include <stacktrace>
#ifdef _WIN32
#include <Windows.h>
#endif

class InputSender
{
  public:
    enum class Type
    {
        mouse
    };

    template <Type T> struct Impl;
};

namespace detail
{
    inline INPUT makeMouseInput(DWORD flags)
    {
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = flags;
        return input;
    }

#ifdef _WIN32
    inline void normalizeAbsoluteCoordinates(LONG& x, LONG& y)
    {
        const LONG screenX = ::GetSystemMetrics(SM_CXSCREEN);
        const LONG screenY = ::GetSystemMetrics(SM_CYSCREEN);
        if (screenX > 0 && screenY > 0)
        {
            const double factorX = 65535.0 / static_cast<double>(screenX);
            const double factorY = 65535.0 / static_cast<double>(screenY);
            x = static_cast<LONG>(x * factorX);
            y = static_cast<LONG>(y * factorY);
        }
    }
#endif
} // namespace detail

template <> struct InputSender::Impl<InputSender::Type::mouse>
{
    static void moveRelative(LONG dx, LONG dy)
    {
        qDebug() << "moveRelative " << dx << " " << dy;
        if (dx == 0 && dy == 0)
        {
            std::cerr << std::stacktrace::current() << '\n';
        }
    }

    static void moveTo(LONG x, LONG y)
    {
        qDebug() << "move to " << x << " " << y;
    }

    static void pressLeft()
    {
        qDebug() << "pressLeft";
    }

    static void releaseLeft()
    {
        qDebug() << "releaseLeft";
    }
};
// template <> struct InputSender::Impl<InputSender::Type::mouse>
// {
//     static void moveRelative(LONG dx, LONG dy)
//     {
// #ifdef _WIN32
//         INPUT input = detail::makeMouseInput(MOUSEEVENTF_MOVE);
//         input.mi.dx = dx;
//         input.mi.dy = dy;
//         ::SendInput(1, &input, sizeof(input));
// #endif
//     }

//     static void moveTo(LONG x, LONG y)
//     {
// #ifdef _WIN32
//         detail::normalizeAbsoluteCoordinates(x, y);
//         INPUT input = detail::makeMouseInput(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE);
//         input.mi.dx = x;
//         input.mi.dy = y;
//         ::SendInput(1, &input, sizeof(input));
// #endif
//     }

//     static void pressLeft()
//     {
// #ifdef _WIN32
//         INPUT input = detail::makeMouseInput(MOUSEEVENTF_LEFTDOWN);
//         ::SendInput(1, &input, sizeof(input));
// #endif
//     }

//     static void releaseLeft()
//     {
// #ifdef _WIN32
//         INPUT input = detail::makeMouseInput(MOUSEEVENTF_LEFTUP);
//         ::SendInput(1, &input, sizeof(input));
// #endif
//     }
// };

template <InputSender::Type T> using InputSenderT = InputSender::Impl<T>;