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

#include "led-matrix.h"     // Raspberry Pi LED Matrix library
#include "ledbuffer.h"      // The LED circular buffer manager
#include <thread>           // For spawning threads
#include <chrono>           // Time and delays
#include <omp.h>            // Include OpenMP header

using rgb_matrix::RGBMatrix;

extern volatile bool interrupt_received;

class MatrixDraw
{
    static double _FPS;

  protected:
	
    // DrawFrame
    //
    // Sends a frame's worth of color data to the matrix and then swaps it on the next VSync
	
    static void DrawFrame(std::unique_ptr<LEDBuffer> & buffer, RGBMatrix & matrix)
    {
        static double lastTime = 0.0;
        double currentTime = CAppTime::CurrentTime();
        double delta = currentTime - lastTime + DBL_EPSILON;        // Add epsilon to avoid divide by zero
        lastTime = currentTime;
        _FPS =  1.0 / delta;

        const size_t width = matrix.width();
        const size_t height = matrix.height();
        const size_t numpixels = width * height;

        // TODO: This code could center a smaller buffer on the matrix or scale it up to fill the matrix
        //       if the matrix is larger than the frame, but for now, we just require that the frames being
        //       sent are the same size as the matrix.

        if (buffer->ColorData().size() > numpixels)
            throw std::runtime_error("More data received than matrix can accomodate");
            
        // Process the entire frame in a single loop for better cache locality

        #pragma omp parallel for
        for (size_t idx = 0; idx < numpixels; ++idx)
        {
            const int x = idx % width;
            const int y = idx / width;

            // Get the pixel color
            const CRGB color = buffer->ColorData()[idx];

            // Set the pixel in the matrix (x is flipped)
            matrix.SetPixel(matrix.width() - 1 - x, y, color.r, color.g, color.b);
        }
    }

  public:

    // FPS
    // 
    // The framerate as of the last drawing operation

    static double FPS()
    {
        return _FPS;
    }

    // RunDrawLoop
    // 
    // Loops looking for frames that have matured on the buffer manager, then drawing them on the matrix as they do

    static bool RunDrawLoop(LEDBufferManager & bufferManager, RGBMatrix & matrix)
    {
        // If set to true, this will cause backlogged frames to be discarded.  If false, they will be drawn
        // as fast as possible to catch up to the current time
        constexpr auto burnExtraFrames = false;

        // How long to wait (micros) when no frames available in the buffer (about 1/24th of a second).  Can't be
        // too long, as if frames come in it will take that long to catch up.  But can't be too short, as it would
        // burn too much CPU spinning in a tight loop.  So this is a compromise that seems appropriate for video.

        constexpr auto kMaximumWait = 40000.0;  

        while (!interrupt_received)
        {
            while (bufferManager.AgeOfOldestBuffer() <= 0)
            {
                std::optional<std::unique_ptr<LEDBuffer>> buffer = bufferManager.PopOldestBuffer();
                if (!buffer.has_value())
                    continue;

                if (burnExtraFrames && bufferManager.AgeOfOldestBuffer() <= 0)
                    continue;

                DrawFrame(buffer.value(), matrix);
            }
            const int64_t delay = std::min(kMaximumWait, bufferManager.AgeOfOldestBuffer() * MICROS_PER_SECOND);
            if (delay > 0)
                std::this_thread::sleep_for(std::chrono::microseconds(delay));
        }
	    return true;
    }
};
