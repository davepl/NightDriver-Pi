// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "displayoutput.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <thread>
#include <vector>
#ifdef __APPLE__
#include <mach/mach_time.h>
#endif

namespace DisplayTest
{
// Small, sharp bitmap lettering remains legible in phone slow-motion footage.
inline std::array<unsigned, 7> Glyph(char c)
{
    switch (c)
    {
        case '0': return {14,17,19,21,25,17,14};
        case '1': return {4,12,4,4,4,4,14};
        case '2': return {14,17,1,2,4,8,31};
        case '3': return {30,1,1,14,1,1,30};
        case '4': return {2,6,10,18,31,2,2};
        case '5': return {31,16,16,30,1,1,30};
        case '6': return {14,16,16,30,17,17,14};
        case '7': return {31,1,2,4,8,8,8};
        case '8': return {14,17,17,14,17,17,14};
        case '9': return {14,17,17,15,1,1,14};
        case 'F': return {31,16,16,30,16,16,16};
        case 'P': return {30,17,17,30,16,16,16};
        case 'S': return {15,16,16,14,1,1,30};
        case 'H': return {17,17,17,31,17,17,17};
        case 'Z': return {31,1,2,4,8,16,31};
        default: return {};
    }
}

inline std::vector<CRGB> Render(int width, int height, uint64_t frame, int fps, int refreshHz, uint64_t missed = 0)
{
    if (width < 1 || height < 1) throw std::invalid_argument("Test pattern needs positive geometry");
    std::vector<CRGB> pixels(size_t(width) * height, CRGB(0,0,4));
    auto Dot = [&](int x, int y, CRGB color)
    {
        if (x >= 0 && x < width && y >= 0 && y < height) pixels[size_t(y) * width + x] = color;
    };
    auto Box = [&](int x, int y, int w, int h, CRGB color)
    {
        for (int dy=0; dy<h; ++dy) for (int dx=0; dx<w; ++dx) Dot(x+dx,y+dy,color);
    };
    auto Text = [&](int x, int y, const char * text, CRGB color, int scale=1)
    {
        for (; *text; ++text, x += 6 * scale)
        {
            const auto glyph = Glyph(*text);
            for (int row=0; row<7; ++row) for (int col=0; col<5; ++col)
                if (glyph[row] & (1u << (4-col))) Box(x+col*scale,y+row*scale,scale,scale,color);
        }
    };
    // Dim 64-pixel panel boundaries make layout repeats and wraps conspicuous.
    for (int x=0; x<width; x+=64) Box(x,0,1,height,CRGB(14,14,20));
    const CRGB panelColors[] = {{90,0,0},{0,90,0},{0,0,90},{70,70,70},{70,70,0},{0,70,70},{70,0,70},{90,35,0}};
    for (int x=0; x<width; ++x) Dot(x,15,panelColors[(x/64)%8]);
    char label[40];
    std::snprintf(label,sizeof(label),"F%08llu",static_cast<unsigned long long>(frame%100000000));
    Text(3,0,label,CRGB(160,160,160),2);
    std::snprintf(label,sizeof(label),"%d FPS",fps); Text(116,0,label,CRGB(0,130,150));
    std::snprintf(label,sizeof(label),"%d HZ",refreshHz); Text(116,8,label,CRGB(90,110,140));
    std::snprintf(label,sizeof(label),"S%04llu",static_cast<unsigned long long>(missed%10000));
    Text(width-62,0,label,missed ? CRGB(180,35,0) : CRGB(0,120,50));
    // Matching top/bottom 16-bit frame codes expose mixed-frame scanout.
    // MSB on the left, LSB on the right. Cyan = one; dim blue = zero.
    if (width >= 320)
        for (int bit=0; bit<16; ++bit)
        {
            CRGB color = frame & (uint64_t(1)<<(15-bit)) ? CRGB(0,160,160) : CRGB(0,0,12);
            Box(184+bit*8,1,6,11,color);
            Box(184+bit*8,height-4,6,4,color);
        }
    // Tick marks are fixed; white markers above/below the lane must align.
    const int marker = int(frame % uint64_t(width));
    for (int x=0; x<width; x+=16) Dot(x,17,CRGB(30,35,45));
    Box(marker,17,2,3,CRGB(180,180,180));
    Box(marker,height-7,2,3,CRGB(180,180,180));
    // A faster orange comet and bouncing magenta diamond make stalls visible.
    const int comet = int((frame * 4) % uint64_t(width));
    for (int tail=19; tail>=0; --tail)
    {
        int x = (comet - tail + width) % width;
        Dot(x,22,CRGB(uint8_t(160-tail*7),uint8_t(70-tail*3),0));
    }
    const int diamond = int((frame * 3) % uint64_t(width));
    const int bounce = int(frame % 8);
    const int centerY = 20 + (bounce < 4 ? bounce : 7-bounce);
    for (int dy=-2; dy<=2; ++dy)
        for (int dx=-2; dx<=2; ++dx)
            if (std::abs(dx)+std::abs(dy)<=2) Dot((diamond+dx+width)%width,centerY+dy,CRGB(120,0,160));
    return pixels;
}

// Frame zero is already presented by main before display-thread setup. At
// 30/60, the second tick retransmits identical bytes; the counter/motion only
// changes on the next content frame. Missed host ticks skip IDs, not catch up.
inline void Run(DisplayOutput & output, int fps, int refreshHz,
                const std::atomic<std::sig_atomic_t> & stop)
{
    using Clock = std::chrono::steady_clock;
    auto period = output.RefreshPeriod();
    const auto lead = output.PreparationLeadTime();
    if (period.count() <= 0) period = std::chrono::nanoseconds(1000000000 / refreshHz);
    const auto epoch = Clock::now();
    uint64_t tick=1, lastFrame=0, missed=0;
    while (!stop.load(std::memory_order_relaxed))
    {
        auto deadline = epoch + period * static_cast<int64_t>(tick);
        DisplayTiming::WaitUntil(deadline - lead);
        if (stop.load(std::memory_order_relaxed)) break;
        const auto now = Clock::now();
        if (now >= deadline + period)
        {
            const auto skipped = uint64_t((now-deadline)/period);
            tick += skipped;
            missed += skipped;
            deadline = epoch + period * static_cast<int64_t>(tick);
        }
        const uint64_t frame = tick * uint64_t(fps) / uint64_t(refreshHz);
        if (frame != lastFrame)
        {
            output.PresentAt(Render(output.Width(),output.Height(),frame,fps,refreshHz,missed), deadline);
            lastFrame=frame;
        }
        else output.RefreshAt(deadline);
        ++tick;
    }
}
}
