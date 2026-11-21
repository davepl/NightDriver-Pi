//+--------------------------------------------------------------------------
//
// File:        SocketServer.h
//
// NightDriverPi - (c) 2018 Plummer's Software LLC.  All Rights Reserved.
//
// This file is part of the NightDriver software project.
//
//    NightDriver is free software: you can redistribute it and/or modify
//    it under the terms of the GNU General Public License as published by
//    the Free Software Foundation, either version 3 of the License, or
//    (at your option) any later version.
//
//    NightDriver is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU General Public License for more details.
//
//    You should have received a copy of the GNU General Public License
//    along with Nightdriver.  It is normally found in copying.txt
//    If not, see <https://www.gnu.org/licenses/>.
//
//
// Description:
//
//    Hosts a socket server on port 49152 to receive LED data from the master
//
// History:     Aug-14-2024     Davepl      Created from NightDriverStrip
//---------------------------------------------------------------------------
#pragma once

#include "displayoutput.h"
#include "ledbuffer.h"
#include <algorithm>
#include <atomic>
#include <csignal>
#include <thread>
#include <chrono>
#ifdef __APPLE__
#include <mach/mach_time.h>
#endif

extern std::atomic<std::sig_atomic_t> interrupt_received;

class MatrixDraw
{
    // Status packets carry whole FPS. A 32-bit atomic also avoids requiring
    // 64-bit atomic runtime helpers on older, 32-bit Raspberry Pi builds.
    inline static std::atomic<uint32_t> _FPS = 0;

  public:
    static double FPS() { return _FPS.load(std::memory_order_relaxed); }

    // Timestamp scheduling is common to every output. HUB75 Present blocks on
    // VSync; Colorlight completes all row packets before its display sequence.
    static bool RunDrawLoop(LEDBufferManager & bufferManager, DisplayOutput & output)
    {
        using Clock = std::chrono::steady_clock;
        const auto refreshInterval = output.RefreshPeriod();
        const auto preparationLead = output.PreparationLeadTime();
        auto nextRefresh = Clock::now() + refreshInterval;
        auto lastFrame = Clock::now();
        constexpr auto maximumWait = std::chrono::microseconds(40000);

        while (!interrupt_received.load(std::memory_order_relaxed))
        {
            auto Present = [&](const LEDBuffer & buffer)
            {
                output.Present(buffer.ColorData());
                const auto now = Clock::now();
                const double delta = std::chrono::duration<double>(now - lastFrame).count();
                if (delta > 0)
                    _FPS.store(static_cast<uint32_t>(std::min(1.0 / delta, double(UINT32_MAX))),
                               std::memory_order_relaxed);
                lastFrame = now;
            };
            auto now = Clock::now();
            if (refreshInterval.count() > 0)
            {
                // Upload before the deadline; only the latch belongs on it.
                if (now >= nextRefresh - preparationLead)
                {
                    if (now >= nextRefresh + refreshInterval)
                        nextRefresh += refreshInterval * ((now - nextRefresh) / refreshInterval);
                    const double latchTime = CAppTime::CurrentTime() +
                        std::chrono::duration<double>(nextRefresh - Clock::now()).count();
                    auto newest = bufferManager.PopNewestBufferDueAt(latchTime);
                    if (interrupt_received.load(std::memory_order_relaxed)) break;
                    if (newest)
                    {
                        output.PresentAt(newest->ColorData(), nextRefresh);
                        const auto presented = Clock::now();
                        const double delta = std::chrono::duration<double>(presented - lastFrame).count();
                        if (delta > 0)
                            _FPS.store(static_cast<uint32_t>(std::min(1.0 / delta, double(UINT32_MAX))), std::memory_order_relaxed);
                        lastFrame = presented;
                    }
                    else output.RefreshAt(nextRefresh);
                    now = Clock::now();
                    nextRefresh += refreshInterval;
                    if (nextRefresh <= now)
                        nextRefresh += refreshInterval * ((now - nextRefresh) / refreshInterval + 1);
                }
            }
            else
            {
                // HUB75 provides its own VSync and retains the existing policy.
                while (!interrupt_received.load(std::memory_order_relaxed) &&
                       bufferManager.AgeOfOldestBuffer() <= 0)
                {
                    auto buffer = bufferManager.PopOldestBuffer();
                    if (!buffer) break;
                    Present(**buffer);
                }
                now = Clock::now();
            }
            if (interrupt_received.load(std::memory_order_relaxed)) break;
            auto delay = maximumWait;
            const double age = bufferManager.AgeOfOldestBuffer();
            if (refreshInterval.count() > 0)
            {
                // New arrivals wait for the next tick, even when already due.
                delay = std::min(delay, std::max(std::chrono::microseconds::zero(),
                    std::chrono::duration_cast<std::chrono::microseconds>(nextRefresh - preparationLead - now)));
            }
            else
            {
                // Clamp before converting: an empty queue returns DBL_MAX.
                if (age <= 0) continue;
                if (age < 0.04)
                    delay = std::chrono::microseconds(static_cast<int64_t>(age * MICROS_PER_SECOND));
            }
#ifdef __APPLE__
            if (refreshInterval.count() > 0 && delay.count() > 0)
            {
                static const auto timebase = []
                {
                    mach_timebase_info_data_t value{};
                    mach_timebase_info(&value);
                    return value;
                }();
                const auto ticks = uint64_t(delay.count()) * 1000 * timebase.denom / timebase.numer;
                mach_wait_until(mach_absolute_time() + ticks);
            }
            else
#endif
                std::this_thread::sleep_for(delay);
        }
        return true;
    }
};
