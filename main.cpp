// -*- mode: c++; c-basic-offset: 2; indent-tabs-mode: nil; -*-
//
// Example how to display an image, including animated images using
// ImageMagick. For a full utility that does a few more things, have a look
// at the led-image-viewer in ../utils
//
// Showing an image is not so complicated, essentially just copy all the
// pixels to the canvas. How to get the pixels ? In this example we're using
// the graphicsmagick library as universal image loader library that
// can also deal with animated images.
// You can of course do your own image loading or use some other library.
//
// This requires an external dependency, so install these first before you
// can call `make image-example`
//   sudo apt-get update
//   sudo apt-get install libgraphicsmagick++-dev libwebp-dev -y
//   make image-example

#include "led-matrix.h"

#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

#include <exception>

using rgb_matrix::Canvas;
using rgb_matrix::RGBMatrix;
using rgb_matrix::FrameCanvas;

// Our interrupt handler sets a global flag indicating that its time to exit

volatile bool interrupt_received = false;
static void InterruptHandler(int signo) 
{
	interrupt_received = true;
}

/*
void CopyImageToCanvas(const Magick::Image &image, Canvas *canvas) {
  const int offset_x = 0, offset_y = 0;  // If you want to move the image.
  // Copy all the pixels to the canvas.
  for (size_t y = 0; y < image.rows(); ++y) {
    for (size_t x = 0; x < image.columns(); ++x) {
      const Magick::Color &c = image.pixelColor(x, y);
      if (c.alphaQuantum() < 256) {
        canvas->SetPixel(x + offset_x, y + offset_y,
                         ScaleQuantumToChar(c.redQuantum()),
                         ScaleQuantumToChar(c.greenQuantum()),
                         ScaleQuantumToChar(c.blueQuantum()));
      }
    }
  }
}
*/

// usage
//
// Display the command line usage options

int usage(const char *progname) 
{
	fprintf(stderr, "Usage: %s [led-matrix-options]\n", progname);
	rgb_matrix::PrintMatrixFlags(stderr);
	return 1;
}

// Matrix Defaults
//
// Can be overridden from the command line

constexpr auto HardwareMapping = "adafruit-hat";
constexpr auto ChainLength     = 8;
constexpr auto Rows            = 32;
constexpr auto Columns         = 64;
constexpr auto GPIOSlowdown    = 5;

// main
//
// Main program entry point

int main(int argc, char *argv[]) 
{
	// Initialize the RGB matrix with
	RGBMatrix::Options matrix_options;

	matrix_options.hardware_mapping = HardwareMapping;
	matrix_options.chain_length     = ChainLength;
	matrix_options.rows             = Rows;
	matrix_options.cols             = Columns;

	rgb_matrix::RuntimeOptions runtime_opt;
	if (!rgb_matrix::ParseOptionsFromFlags(&argc, &argv, &matrix_options, &runtime_opt)) 
		return usage(argv[0]);

	runtime_opt.gpio_slowdown = GPIOSlowdown;

	signal(SIGTERM, InterruptHandler);
	signal(SIGINT, InterruptHandler);

	RGBMatrix *matrix = RGBMatrix::CreateFromOptions(matrix_options, runtime_opt);
	if (matrix == NULL)
		return 1;

	matrix->Clear();
	matrix->Fill(0, 0, 255);

	while (!interrupt_received)
		sleep(1);

	delete matrix;
	return 0;
}
