// NightDriver-Pi - GPL-3.0-or-later
#pragma once

#include <memory>
#include <stdexcept>
#include "displayoutput.h"
#include "led-matrix.h"

class Hub75Output final : public DisplayOutput
{
    std::unique_ptr<rgb_matrix::RGBMatrix> _matrix;
    rgb_matrix::FrameCanvas * _canvas;

  public:
    explicit Hub75Output(rgb_matrix::RGBMatrix * matrix) : _matrix(matrix), _canvas(nullptr)
    {
        if (!_matrix)
            throw std::runtime_error("Error creating RGBMatrix object");
        // Preserve the existing HUB75 startup color and appearance.
        _matrix->set_luminance_correct(true);
        _matrix->Fill(0, 0, 128);
        _canvas = _matrix->CreateFrameCanvas();
        if (!_canvas)
            throw std::runtime_error("Unable to create offscreen matrix canvas");
    }

    int Width() const override { return _matrix->width(); }
    int Height() const override { return _matrix->height(); }

    void Present(std::span<const CRGB> frame) override
    {
        if (frame.size() != size_t(Width()) * size_t(Height()))
            throw std::runtime_error("Frame size does not match HUB75 geometry");
        for (size_t idx = 0; idx < frame.size(); ++idx)
        {
            const int x = idx % Width();
            const int y = idx / Width();
            const auto & color = frame[idx];
            _canvas->SetPixel(Width() - 1 - x, y, color.r, color.g, color.b);
        }
        _canvas = _matrix->SwapOnVSync(_canvas);
        if (!_canvas)
            throw std::runtime_error("SwapOnVSync failed");
    }
};
