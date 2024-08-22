# NightDriver-Pi

This is an early checkin of the in-progress work to bring the NightDriver wifi client to the Pi for HUB75 matrices.

It uses the rgb-matrix-rpi library to manage the actual matrix.  It then spins up a thread to receive color data packets in standard NightDriver format, which is basically a timestamp and color data, one CRGB object per pixel.  As those packets are received they are placed in a circular buffer.

The main thread monitors the buffer, and when a frame is ready to be drawn, pulls it off and sends it to the matrix.

## Cloning and building

The code in this project uses [`rpi-rgb-led-matrix`](https://github.com/hzeller/rpi-rgb-led-matrix) as a dependency, which is included in this repository as a submodule. There are two ways to initialize this library when preparing the build of this repo's code:

1. Include the git clone of `rpi-rgb-led-matrix` in the clone of this repo. For this, use the following command to clone this repo:

   ```shell
   git clone --recurse-submodules https://github.com/davepl/NightDriver-Pi.git
   ```

2. Initialize and update the `rpi-rgb-led-matrix` submodule after cloning NightDriver-Pi itself - for instance because you already cloned the latter before you read this. This can be done by running the following command after cloning this repo:

   ```shell
   git submodule update --init --recursive
   ```

If you use a development environment like VSCode to clone NightDriver-Pi, the cloning of `rpi-rgb-led-matrix` may well happen automatically.

Building NightDriver-Pi and its dependency can simply be done by running `make` - not that you can do a lot with it!
The build of `rpi-rgb-led-matrix` will be included when necessary.

