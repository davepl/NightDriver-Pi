#include "hub75output.h"
#include <cassert>
#include <iostream>

int main()
{
    auto * matrix = new rgb_matrix::RGBMatrix;
    Hub75Output output(matrix);
    assert(matrix->luminance);
    assert(matrix->fill.r == 0 && matrix->fill.g == 0 && matrix->fill.b == 128);
    assert(output.Width() == 3 && output.Height() == 2);
    std::vector<CRGB> frame = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {1, 2, 3}, {4, 5, 6}, {7, 8, 9}};
    output.Present(frame);
    assert(matrix->swaps == 1 && matrix->canvas.pixels.size() == 6);
    const auto & first = matrix->canvas.pixels.front();
    const auto & last = matrix->canvas.pixels.back();
    assert(first.x == 2 && first.y == 0 && first.r == 255 && first.g == 0 && first.b == 0);
    assert(last.x == 0 && last.y == 1 && last.r == 7 && last.g == 8 && last.b == 9);
    bool rejected = false;
    try { output.Present(std::span(frame).first(5)); } catch (const std::runtime_error &) { rejected = true; }
    assert(rejected && matrix->swaps == 1);
    std::cout << "HUB75 presentation tests passed\n";
}
