// NightDriver-Pi - GPL-3.0-or-later
#pragma once

#include <chrono>
#include <span>
#include "pixeltypes.h"
#include "displaytiming.h"

// A complete logical, row-major RGB frame crosses this boundary once per
// presentation. Pixel orientation and hardware color conversion belong here.
class DisplayOutput
{
  public:
    virtual ~DisplayOutput() = default;
    virtual int Width() const = 0;
    virtual int Height() const = 0;
    virtual void Present(std::span<const CRGB> frame) = 0;

    // Outputs that need a keepalive may retransmit their last completed frame.
    // The shared scheduler always gives newly due input frames priority.
    virtual std::chrono::microseconds RefreshInterval() const { return {}; }
    virtual void Refresh() {}
    virtual std::chrono::nanoseconds RefreshPeriod() const { return RefreshInterval(); }
    virtual std::chrono::microseconds PreparationLeadTime() const { return {}; }
    virtual void PresentAt(std::span<const CRGB> frame, DisplayTiming::Clock::time_point deadline)
    {
        DisplayTiming::WaitUntil(deadline);
        Present(frame);
    }
    virtual void RefreshAt(DisplayTiming::Clock::time_point deadline)
    {
        DisplayTiming::WaitUntil(deadline);
        Refresh();
    }
};
