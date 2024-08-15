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
// History:     AUg-14-2024     Davepl      Created from NightDriverStrip
//---------------------------------------------------------------------------
#pragma once

#include "led-matrix.h"
#include "ledbuffer.h"
#include <thread>
#include <chrono>

using rgb_matrix::Canvas;
using rgb_matrix::RGBMatrix;
using rgb_matrix::FrameCanvas;

class MatrixDraw
{
  protected:
	
	// DrawFrame
	//
	// Sends a 
	
	static void DrawFrame(std::unique_ptr<LEDBuffer> & buffer, RGBMatrix & matrix)
	{
		if (buffer->ColorData().size() != (size_t) matrix.width() * matrix.height())
			throw std::runtime_error("Size mismatch between data and matrix.");
		
		for (int x = 0; x < matrix.width(); x++)
		{
			for (int y = 0; y < matrix.height(); y++)
			{
				CRGB color = buffer->ColorData()[y * matrix.width() + x];
				matrix.SetPixel(x, y, color.r, color.g, color.b);
			}
		}
		matrix.SwapOnVSync(nullptr);
	}

  public:

	// RunDrawLoop
	// 
	// Loops looking for frames that have matured on the buffer manager, then drawing them on the matrix as they do

    static bool RunDrawLoop(LEDBufferManager & bufferManager, RGBMatrix & matrix)
    {
		while (!interrupt_received)
		{
			while (bufferManager.AgeOfOldestBuffer() <= 0)
			{
				std::unique_ptr<LEDBuffer> buffer = bufferManager.PopOldestBuffer();
				if (bufferManager.AgeOfOldestBuffer() <= 0)
					continue;
				DrawFrame(buffer, matrix);
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return true;
    }
};