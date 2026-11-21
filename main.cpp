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
//    Hosts a socket server on port 49152 to receive LED data from a remote
//    NightDriverDesktop instance and then draws that data to the LED matrix
//
// History:     Aug-14-2024     Davepl      Created from NightDriverStrip
//---------------------------------------------------------------------------

#include <csignal>
#include <algorithm>
#include <limits>
#include <memory>
#include <thread>
#include <vector>
#include "apptime.h"
#include "globals.h"
#include "ledbuffer.h"
#include "socketserver.h"
#include "matrixdraw.h"
#include "testpattern.h"
#include "outputconfig.h"
#include "colorlight.h"

#ifndef WITH_HUB75
#define WITH_HUB75 1
#endif
#if WITH_HUB75
#include "hub75output.h"
#endif

std::atomic<std::sig_atomic_t> interrupt_received{0};
static_assert(decltype(interrupt_received)::is_always_lock_free);
static void InterruptHandler(int) { interrupt_received.store(1, std::memory_order_relaxed); }

int usage(const char * progname, int result = 1)
{
    fprintf(stderr, "Usage: %s [--output=hub75] [led-matrix-options]\n"
                    "       %s --output=colorlight --config=wall.json [--port=49152]\n"
                    "       %s --config=wall.json [--port=49152]\n"
                    "       Add --test-pattern [--test-fps=30] for a local counter/motion test.\n"
                    "HUB75 is the default. A Colorlight config selects Colorlight unless it conflicts\n"
                    "with an explicit --output. --help prints this message.\n", progname, progname, progname);
#if WITH_HUB75
    rgb_matrix::PrintMatrixFlags(stderr);
#else
    fprintf(stderr, "This build has no HUB75 support (make WITH_HUB75=1 to enable it).\n");
#endif
    return result;
}

static std::unique_ptr<DisplayOutput> CreateOutput(const AppOptions & options, int & argc, char **& argv)
{
    if (options.output == OutputType::Colorlight)
    {
        // In particular, do not parse matrix flags, initialize GPIO, or construct
        // an RGBMatrix in this branch, even in a build supporting both outputs.
        if (argc != 1)
            throw std::runtime_error(std::string("Unexpected option in Colorlight mode: ") + argv[1]);
        auto config = LoadColorlightConfig(options.config_path);
        if (options.test_pattern)
        {
            // The locally generated diagnostic labels native receiver positions.
            // Incoming-frame orientation belongs only to normal playback.
            config.flip_x = false;
            config.flip_y = false;
        }
        return std::make_unique<ColorlightOutput>(config);
    }
#if WITH_HUB75
    rgb_matrix::RGBMatrix::Options matrixOptions;
    matrixOptions.hardware_mapping = kDefaultHardwareMapping;
    matrixOptions.chain_length = kDefaultChainLength;
    matrixOptions.rows = kDefualtRows;
    matrixOptions.cols = kDefaultColumns;
    matrixOptions.limit_refresh_rate_hz = kDefaultRefreshRate;
    matrixOptions.disable_busy_waiting = kDefaultDisableBusyWaiting;
    rgb_matrix::RuntimeOptions runtimeOptions;
    if (!rgb_matrix::ParseOptionsFromFlags(&argc, &argv, &matrixOptions, &runtimeOptions))
    {
        usage(argv[0]);
        throw std::runtime_error("Invalid HUB75 matrix options; use --help for usage");
    }
    if (argc != 1)
        throw std::runtime_error(std::string("Unexpected option: ") + argv[1]);
    runtimeOptions.gpio_slowdown = kDefaultGPIOSlowdown;
    return std::make_unique<Hub75Output>(rgb_matrix::RGBMatrix::CreateFromOptions(matrixOptions, runtimeOptions));
#else
    throw std::runtime_error("HUB75 support is disabled in this build; select --output=colorlight --config=wall.json");
#endif
}

int main(int argc, char * argv[])
{
    std::signal(SIGTERM, InterruptHandler);
    std::signal(SIGINT, InterruptHandler);
    // A disconnected upstream socket must not terminate the display process.
    std::signal(SIGPIPE, SIG_IGN);
    try
    {
        const auto options = ParseAppOptions(argc, argv);
        if (options.help)
            return usage(argv[0], 0);
        auto output = CreateOutput(options, argc, argv);
        if (output->Width() <= 0 || output->Height() <= 0 ||
            size_t(output->Width()) > (std::numeric_limits<size_t>::max() - 24) / sizeof(CRGB) / size_t(output->Height()))
            throw std::runtime_error("Display geometry exceeds supported frame size");
        const size_t pixels = size_t(output->Width()) * size_t(output->Height());
        printf("Display Size: %dx%d (%zu LEDs)\n", output->Width(), output->Height(), pixels);
        if (options.test_pattern)
        {
            const int refreshHz = options.output == OutputType::Colorlight
                ? LoadColorlightConfig(options.config_path).refresh_hz : 60;
            if (options.test_fps > refreshHz)
                throw std::invalid_argument("--test-fps cannot exceed the output refresh_hz");
            if (output->Width() < 384 || output->Height() < 32)
                throw std::invalid_argument("The counter/motion test needs at least 384x32 pixels");
            output->Present(DisplayTest::Render(output->Width(),output->Height(),0,options.test_fps,refreshHz));
            if (options.output == OutputType::Colorlight)
                static_cast<ColorlightOutput &>(*output).ConfigureDisplayThread();
            printf("Local test pattern: %d content fps, %d Hz output; no NightDriver input socket.\n"
                   "F = content frame, S = missed host ticks; cyan bars = binary frame ID.\n",
                   options.test_fps, refreshHz);
            fflush(stdout);
            DisplayTest::Run(*output,options.test_fps,refreshHz,interrupt_received);
            return 0;
        }
        size_t capacity = kMaxBuffers;
        if (options.output == OutputType::Colorlight)
        {
            // Bound queue storage for large Ethernet walls; report the actual
            // capacity in the unchanged NightDriver status response.
            constexpr size_t bufferBudget = 256u * 1024u * 1024u;
            capacity = std::max(size_t(1), std::min(size_t(kMaxBuffers), bufferBudget / (pixels * sizeof(CRGB))));
        }
        LEDBufferManager bufferManager(capacity);
        SocketServer socketServer(options.incoming_port, pixels);
        if (!socketServer.begin())
            return 1;
        printf("NightDriver input: TCP port %d\n", options.incoming_port);
        if (options.output == OutputType::Colorlight)
        {
            output->Present(std::vector<CRGB>(pixels, CRGB(0, 0, 128)));
            printf("Colorlight: solid blue until the first NightDriver frame is due\n");
        }
        std::thread receiver([&]()
        {
            socketServer.ProcessIncomingConnectionsLoop(bufferManager);
        });
        try
        {
            if (options.output == OutputType::Colorlight)
                static_cast<ColorlightOutput &>(*output).ConfigureDisplayThread();
            MatrixDraw::RunDrawLoop(bufferManager, *output);
        }
        catch (...)
        {
            socketServer.end();
            receiver.join();
            throw;
        }
        socketServer.end();
        receiver.join();
        return 0;
    }
    catch (const std::exception & e)
    {
        fprintf(stderr, "ndpi: %s\n", e.what());
        return 1;
    }
}
