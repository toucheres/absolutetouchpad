#pragma once

#include <iostream>
#include <qdebug.h>
#include <qlogging.h>
#include <stacktrace>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <vector>
#include <atomic>
#include <chrono>
#include <cstdlib>
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
    // Internal batched sender: coalesces relative moves and batches SendInput calls.
    struct BatchedSender
    {
        std::mutex mtx;
        std::condition_variable cv;
        std::deque<INPUT> queue;
        std::atomic<bool> running{false};
        std::thread worker;
        const std::chrono::milliseconds interval{10}; // flush interval (10ms)

        BatchedSender()
        {
            running = true;
            worker = std::thread([this]() { this->run(); });
            std::atexit([]() {
                // ensure the static instance is stopped at exit
            });
        }

        ~BatchedSender()
        {
            running = false;
            cv.notify_one();
            if (worker.joinable())
                worker.join();
        }

        void enqueueRelative(LONG dx, LONG dy)
        {
            if (dx == 0 && dy == 0)
                return; // skip no-op

            std::unique_lock lock(mtx);
            // Try to coalesce with last queued relative move if present and small
            if (!queue.empty())
            {
                INPUT &last = queue.back();
                if (last.type == INPUT_MOUSE && (last.mi.dwFlags & MOUSEEVENTF_MOVE) != 0 &&
                    (last.mi.dwFlags & MOUSEEVENTF_ABSOLUTE) == 0)
                {
                    last.mi.dx += dx;
                    last.mi.dy += dy;
                    cv.notify_one();
                    return;
                }
            }

            INPUT in = detail::makeMouseInput(MOUSEEVENTF_MOVE);
            in.mi.dx = dx;
            in.mi.dy = dy;
            queue.push_back(in);
            cv.notify_one();
        }

        void enqueueAbsolute(LONG x, LONG y)
        {
            std::unique_lock lock(mtx);
            // push absolute move as its own event
            INPUT in = detail::makeMouseInput(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE);
            detail::normalizeAbsoluteCoordinates(x, y);
            in.mi.dx = x;
            in.mi.dy = y;
            queue.push_back(in);
            cv.notify_one();
        }

        void enqueueButton(DWORD flags)
        {
            std::unique_lock lock(mtx);
            INPUT in = detail::makeMouseInput(flags);
            queue.push_back(in);
            cv.notify_one();
        }

        void run()
        {
            std::vector<INPUT> batch;
            while (running)
            {
                {
                    std::unique_lock lock(mtx);
                    if (queue.empty())
                    {
                        cv.wait_for(lock, interval);
                    }

                    // drain queue
                    while (!queue.empty())
                    {
                        batch.push_back(queue.front());
                        queue.pop_front();
                        // limit batch size to avoid huge bursts
                        if (batch.size() >= 256)
                            break;
                    }
                }

                if (!batch.empty())
                {
#ifdef _WIN32
                    ::SendInput(static_cast<UINT>(batch.size()), batch.data(), sizeof(INPUT));
#else
                    Q_UNUSED(batch);
                    qDebug() << "Would send" << batch.size() << "inputs";
#endif
                    batch.clear();
                }
            }

            // flush remaining on shutdown
            std::unique_lock lock(mtx);
            if (!queue.empty())
            {
                while (!queue.empty())
                {
                    batch.push_back(queue.front());
                    queue.pop_front();
                }
#ifdef _WIN32
                ::SendInput(static_cast<UINT>(batch.size()), batch.data(), sizeof(INPUT));
#endif
            }
        }
    };

    static BatchedSender& sender()
    {
        static BatchedSender inst;
        return inst;
    }

    static void moveRelative(LONG dx, LONG dy)
    {
        qDebug() << "moveRelative " << dx << " " << dy;
        sender().enqueueRelative(dx, dy);
    }

    static void moveTo(LONG x, LONG y)
    {
        qDebug() << "move to " << x << " " << y;
        sender().enqueueAbsolute(x, y);
    }

    static void pressLeft()
    {
        qDebug() << "pressLeft";
        sender().enqueueButton(MOUSEEVENTF_LEFTDOWN);
    }

    static void releaseLeft()
    {
        qDebug() << "releaseLeft";
        sender().enqueueButton(MOUSEEVENTF_LEFTUP);
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