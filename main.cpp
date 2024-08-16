//
// File:        SocketServer.h
//
// NightDriverPi - (c) 2024 Plummer's Software LLC.  All Rights Reserved.
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
// Description:
//
//    Hosts a socket server on port 49152 to receive LED data from the master
//    and then draws that data to the LED matrix
//
// History:     Aug-14-2024     Davepl      Created from NightDriverStrip
//---------------------------------------------------------------------------

#include "led-matrix.h"

#include <signal.h>
#include "apptime.h"
#include "globals.h"
#include "ledbuffer.h"
#include "socketserver.h"
#include "matrixdraw.h"

using rgb_matrix::Canvas;
using rgb_matrix::RGBMatrix;
using rgb_matrix::FrameCanvas;

// Our interrupt handler sets a global flag indicating that it is time to exit

volatile bool interrupt_received = false;
static void InterruptHandler(int signo) 
{
    interrupt_received = true;
}

// usage
//
// Display the command line usage options

int usage(const char *progname) 
{
    fprintf(stderr, "Usage: %s [led-matrix-options]\n", progname);
    rgb_matrix::PrintMatrixFlags(stderr);
    return 1;
}

// main
//
// Main program entry point, which performs initialization and then spins off a thread
// for the socket server.  It then loops, drawing any frames that are available until it is
// interrupted by ctrl-c or a SIGTERM.

int main(int argc, char *argv[]) 
{
    // Register signal handlers to catch when it's time to shut down
    signal(SIGTERM, InterruptHandler);
    signal(SIGINT, InterruptHandler);

    // Initialize the RGB matrix with
    RGBMatrix::Options matrix_options;

    // Our defaults, which can all be overridden from the command line
    matrix_options.hardware_mapping = DefaultHardwareMapping;
    matrix_options.chain_length     = DefaultChainLength;
    matrix_options.rows             = DefualtRows;
    matrix_options.cols             = DefaultColumns;

    rgb_matrix::RuntimeOptions runtime_opt;
    if (!rgb_matrix::ParseOptionsFromFlags(&argc, &argv, &matrix_options, &runtime_opt)) 
        return usage(argv[0]);
    runtime_opt.gpio_slowdown = DefaultGPIOSlowdown;

    RGBMatrix *matrix = RGBMatrix::CreateFromOptions(matrix_options, runtime_opt);
    if (!matrix)
    {
	fprintf(stderr, "Error creating RGBMatrix object\n");
     	return 1;
    }   

    auto maxLEDs = matrix->width() * matrix->height();
    printf("Matrix Size: %dx%d (%d LEDs)\n", matrix->width(), matrix->height(), maxLEDs);

    matrix->Clear();
    matrix->Fill(0, 0, 128);

    LEDBufferManager bufferManager(250);
    SocketServer socketServer(49152, maxLEDs);

    // Launch the socket server on its own thread to process incoming packets

    if (socketServer.begin())
    {
		std::thread([&socketServer, &bufferManager]() 
		{
			socketServer.ProcessIncomingConnectionsLoop(bufferManager);
		}).detach();  // Detach to allow the thread to run independently

		// Loop forever, looking for frames to draw on the matrix until we are interrupted
		MatrixDraw::RunDrawLoop(bufferManager, *matrix);

		socketServer.end();
    }
    delete matrix;
    return 0;
}

