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
// Helper class for getting the current time in seconds since the epoch

class CAppTime
{
  public:

    static double CurrentTime()
    {
        timeval tv;
        gettimeofday(&tv, nullptr);
        return (double)tv.tv_sec + (tv.tv_usec/(double)MICROS_PER_SECOND);
    }
};