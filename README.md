This is an early checkin of the in-progress work to bring the NightDriver wifi client to the Pi for HUB75 matrices.

It uses the rgb-matrix-rpi library to manage the actual matrix.  It then spins up a thread to receive color data packets in standard NightDriver format, which is basically a timestamp and color data, one CRGB object per pixel.  As those packets are received they are placed in a circular buffer.

The main thread monitors the buffer, and when a frame is ready to be drawn, pulls it off and sends it to the matrix.

Building:

The code expects you have enlisted in the rpi-matrix project as a peer folder of your enlistment, and that you’ve already built it, since NDPi links to its output lib:
https://github.com/hzeller/rpi-rgb-led-matrix

Run make in it, then run make in this, should “just work”.  Not that you can do a lot with it!  

