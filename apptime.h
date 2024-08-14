#pragma once

#include <cstddef>
#include <cstdint>
#include <sys/time.h>
#include <optional>

#ifndef MICROS_PER_SECOND
    #define MICROS_PER_SECOND 1000000
#endif

// AppTime
//
// A class that keeps track of the clock, how long the last frame took, calculating FPS, etc.

class CAppTime
{
  protected:

    double _lastFrame = CurrentTime();
    double _deltaTime = 1.0;

  public:

    // NewFrame
    //
    // Call this at the start of every frame or udpate, and it'll figure out and keep track of how
    // long between frames

    void NewFrame()
    {
        timeval tv;
        gettimeofday(&tv, nullptr);
        double current = CurrentTime();
        _deltaTime = current - _lastFrame;

        // Cap the delta time at one full second

        if (_deltaTime > 1.0)
            _deltaTime = 1.0;

        _lastFrame = current;
    }

    CAppTime() : _lastFrame(CurrentTime())
    {
        NewFrame();
    }

    double FrameStartTime() const
    {
        return _lastFrame;
    }

    static double CurrentTime()
    {
        timeval tv;
        gettimeofday(&tv, nullptr);
        return (double)tv.tv_sec + (tv.tv_usec/(double)MICROS_PER_SECOND);
    }

    double FrameElapsedTime() const
    {
        return FrameStartTime() - CurrentTime();
    }

    static double TimeFromTimeval(const timeval & tv)
    {
        return tv.tv_sec + (tv.tv_usec/(double)MICROS_PER_SECOND);
    }

    static timeval TimevalFromTime(double t)
    {
        timeval tv;
        tv.tv_sec = (long)t;
        tv.tv_usec = t - tv.tv_sec;
        return tv;
    }

    double LastFrameTime() const
    {
        return _deltaTime;
    }
};