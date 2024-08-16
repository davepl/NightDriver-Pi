This is an early checkin of the in-progress work to bring the NightDriver wifi client to the Pi for HUB75 matrices.

It uses the rgb-matrix-rpi library to manage the actual matrix.  It then spins up a thread to receive color data packets in standard NightDriver format, which is basically a timestamp and color data, one CRGB object per pixel.  As those packets are received they are placed in a circular buffer.

The main thread monitors the buffer, and when a frame is ready to be drawn, pulls it off and sends it to the matrix.

