// Test double for the few matrix APIs used by ndpi. No GPIO code is linked.
#pragma once
#include <cstdio>
#include <stdexcept>
#include <vector>

namespace rgb_matrix
{
struct Pixel { int x, y, r, g, b; };
class FrameCanvas
{
  public:
    std::vector<Pixel> pixels;
    void SetPixel(int x, int y, int r, int g, int b) { pixels.push_back({x, y, r, g, b}); }
};
struct RuntimeOptions { int gpio_slowdown = 0; };
class RGBMatrix
{
  public:
    struct Options
    {
        const char * hardware_mapping;
        int chain_length, rows, cols, limit_refresh_rate_hz;
        bool disable_busy_waiting;
    };
    bool luminance = false;
    Pixel fill{};
    int swaps = 0;
    FrameCanvas canvas;
    int width() const { return 3; }
    int height() const { return 2; }
    void set_luminance_correct(bool value) { luminance = value; }
    void Fill(int r, int g, int b) { fill = {0, 0, r, g, b}; }
    FrameCanvas * CreateFrameCanvas() { return &canvas; }
    FrameCanvas * SwapOnVSync(FrameCanvas * frame) { ++swaps; return frame; }
    static RGBMatrix * CreateFromOptions(const Options &, const RuntimeOptions &)
    {
        throw std::runtime_error("TEST_GPIO_INITIALIZATION");
    }
};
inline bool ParseOptionsFromFlags(int *, char ***, RGBMatrix::Options *, RuntimeOptions *) { return true; }
inline void PrintMatrixFlags(FILE * stream) { fprintf(stream, "[test matrix options]\n"); }
}
