// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <chrono>
#include <thread>
#ifdef __APPLE__
#include <mach/mach_time.h>
#endif

namespace DisplayTiming
{
using Clock = std::chrono::steady_clock;
inline void WaitUntil(Clock::time_point deadline, bool precise = false)
{
#ifdef __APPLE__
    static const auto timebase = []
    {
        mach_timebase_info_data_t t{};
        mach_timebase_info(&t);
        return t;
    }();
    // Bound busy waiting to 100 us per latch (0.6% of one core at 60 Hz).
    const auto wake = deadline - (precise ? std::chrono::microseconds(100) : std::chrono::microseconds(0));
    const auto delay = std::chrono::duration_cast<std::chrono::nanoseconds>(wake - Clock::now()).count();
    if (delay > 0)
        mach_wait_until(mach_absolute_time() + uint64_t(delay) * timebase.denom / timebase.numer);
    if (precise) while (Clock::now() < deadline) {}
#else
    (void)precise;
    std::this_thread::sleep_until(deadline);
#endif
}
}
