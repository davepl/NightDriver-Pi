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

#include <unistd.h>
#include <stdio.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include <zlib.h>
#include <span>
#include <memory>
#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <csignal>
#include <mutex>
#include <limits>

#include "ledbuffer.h"
#include "matrixdraw.h"

using namespace std::chrono_literals;

#define STANDARD_DATA_HEADER_SIZE   24                                              // Size of the header for expanded data
#define COMPRESSED_HEADER_SIZE      16                                              // Size of the header for compressed data
#define LED_DATA_SIZE               sizeof(CRGB)                                    // Data size of an LED (24 bits or 3 bytes)

// Expanded packets contain the header plus three RGB bytes per configured pixel.
// Incoming storage also allows the zlib worst case plus its compressed header.

#define COMPRESSED_HEADER_TAG (0x44415645)                                              // asci "DAVE" as header


// SocketResponse
//
// Response data sent back to server every time we receive a packet

struct SocketResponse
{
    uint32_t    size;              // 4
    uint32_t    flashVersion;      // 4
    double      currentClock;      // 8
    double      oldestPacket;      // 8
    double      newestPacket;      // 8
    double      brightness;        // 8
    double      wifiSignal;        // 8
    uint32_t    bufferSize;        // 4
    uint32_t    bufferPos;         // 4
    uint32_t    fpsDrawing;        // 4
    uint32_t    watts;             // 4
};

static_assert(sizeof(double) == 8);             // SocketResponse on wire uses 8 byte floats
static_assert(sizeof(float)  == 4);             // PeakData on wire uses 4 byte floats

// Two things must be true for this to work and interop with the C# side:  floats must be 8 bytes, not the default
// of 4 for Arduino.  So that must be set in 'platformio.ini,' and you must ensure that you align things such that
// floats land on byte multiples of 8; otherwise, you'll get packing bytes inserted.  Welcome to my world! Once upon
// a time, I ported about a billion lines of x86 'pragma_pack(1)' code to the MIPS (davepl)!

static_assert( sizeof(SocketResponse) == 64, "SocketResponse struct size is not what is expected - check alignment and float size" );

// SocketServer
//
// Handles incoming connections from the server and passes the data that comes in

class SocketServer
{
private:

    int                         _port;
    int                         _server_fd;
    struct sockaddr_in          _address;
    size_t                      _maximumPacketSize;
    size_t                      _maximumReadSize;
    size_t                      _expectedPixels;
    std::atomic<bool>            _stopping{false};
    std::mutex                  _socketMutex;
    int                         _client_fd = -1;
    std::unique_ptr<uint8_t []> _pBuffer;
    std::unique_ptr<uint8_t []> _abOutputBuffer;


    static size_t MaximumPacketSize(size_t pixels)
    {
        constexpr size_t maximum = std::min(size_t(std::numeric_limits<uInt>::max()),
                                             std::numeric_limits<size_t>::max());
        if (pixels == 0 || pixels > std::numeric_limits<uint32_t>::max() ||
            pixels > (maximum - STANDARD_DATA_HEADER_SIZE) / LED_DATA_SIZE)
            throw std::invalid_argument("Display pixel count exceeds supported NightDriver frame size");
        const size_t raw = STANDARD_DATA_HEADER_SIZE + pixels * LED_DATA_SIZE;
        const uLong bound = compressBound(raw);
        if (bound < raw || bound > maximum - COMPRESSED_HEADER_SIZE)
            throw std::invalid_argument("Compressed frame capacity exceeds supported size");
        return raw;
    }

public:

    size_t                      _cbReceived;

    SocketServer(int port, size_t maxLEDs) :
        _port(port),
        _server_fd(-1),
        _address{},
        _maximumPacketSize(MaximumPacketSize(maxLEDs)),
        _maximumReadSize(std::max(_maximumPacketSize, size_t(COMPRESSED_HEADER_SIZE) + compressBound(_maximumPacketSize))),
        _expectedPixels(maxLEDs),
        _pBuffer(std::make_unique<uint8_t []>(_maximumReadSize)),
        _abOutputBuffer(std::make_unique<uint8_t []>(_maximumPacketSize)),
        _cbReceived(0)
    {
    }

    ~SocketServer()
    {
        // The owning main thread joins the worker before this destructor.
        end();
        release();
    }

    void release()
    {
        if (_server_fd >= 0)
        {
            close(_server_fd);
            _server_fd = -1;
        }
    }

    bool begin()
    {
        _cbReceived = 0;

        // Creating socket file descriptor
        if ((_server_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        {
            printf("socket error\n");
            return false;
        }

        // When an error occurs and we close and reopen the port, we need to specify reuse flags
        // or it might be too soon to use the port again since close doesn't actually close it
        // until the socket is no longer in use.

        int opt = 1;
        if (setsockopt(_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)))
        {
            perror("setsockopt");
            release();
            return false;
        }

        _address.sin_family      = AF_INET;
        _address.sin_addr.s_addr = INADDR_ANY;
        _address.sin_port        = htons( _port );

        if (bind(_server_fd, (struct sockaddr *)&_address, sizeof(_address)) < 0)       // Bind socket to port
        {
            perror("bind failed\n");
            release();
            return false;
        }
        if (listen(_server_fd, 6) < 0)                                                  // Start listening for connections
        {
            perror("listen failed\n");
            release();
            return false;
        }
        // poll() bounds shutdown latency. A nonblocking listener also prevents
        // accept() from blocking if readiness changes before it is called.
        const int flags = fcntl(_server_fd, F_GETFL, 0);
        if (flags < 0 || fcntl(_server_fd, F_SETFL, flags | O_NONBLOCK) < 0)
        {
            perror("setting listener nonblocking");
            release();
            return false;
        }
        return true;
    }

    int Port() const
    {
        sockaddr_in address{};
        socklen_t length = sizeof(address);
        if (getsockname(_server_fd, reinterpret_cast<sockaddr *>(&address), &length) != 0)
            throw std::runtime_error("Unable to query listening port");
        return ntohs(address.sin_port);
    }

    void end()
    {
        _stopping.store(true);
        std::lock_guard<std::mutex> lock(_socketMutex);
        // poll() bounds the idle listener wait; shutdown wakes active reads and
        // writes. Do not close a descriptor while the worker can still use it.
        // The worker closes the client; the destructor closes the listener.
        if (_client_fd >= 0)
            shutdown(_client_fd, SHUT_RDWR);
    }

    void CloseClient(int fd)
    {
        std::lock_guard<std::mutex> lock(_socketMutex);
        if (_client_fd == fd)
            _client_fd = -1;
        close(fd);
    }

    // ProcessIncomingData
    //
    // Takes the packet in raw form, decodes enough of it to inspect the command and channel, and then creates and pushes
    // a new LEDBuffer for the data when appropriate.

    static bool ProcessIncomingData(LEDBufferManager & bufferManager, std::span<const uint8_t> payload, size_t expectedPixels)
    {
        if (payload.size() < STANDARD_DATA_HEADER_SIZE)
            return false;
        const auto payloadData = payload.data();
        uint16_t command16 = WORDFromMemory(&payloadData[0]);
        if (command16 == WIFI_COMMAND_PIXELDATA64)
        {
            uint16_t channel16 = WORDFromMemory(&payloadData[2]);
            if (channel16 != 0 && (channel16 & 0x01) == 0)
            {
                printf("Channel mismatch, not intended for us");
                return false;
            }

            // Now we attempt to parse the data and add it to the buffer.  If the data can't be parsed properly, we'll
            // catch the exception and return false.
            
            try
            {
                bufferManager.PushNewBuffer( LEDBuffer::CreateFromWire(payload, expectedPixels) );
                return true;
            }
            catch(const LEDBufferException & e)
            {
                std::cerr << e.what() << '\n';
            }
        }
        return false;
    }

    void ResetReadBuffer()
    {
        _cbReceived = 0;
        memset(_pBuffer.get(), 0, _maximumReadSize);
    }

    // ReadUntilNBytesReceived
    //
    // Read from the socket until the buffer contains at least cbNeeded bytes

    bool ReadUntilNBytesReceived(int socket, size_t cbNeeded)
    {
        if (cbNeeded <= _cbReceived)                            // If we already have that many bytes, we're already done
            return true;

        // This test caps maximum packet size as a full buffer read of LED data.  If other packets wind up being longer,
        // the buffer itself and this test might need to change

        if (cbNeeded > _maximumReadSize)
        {
            printf("Packet size %zu exceeds maximum packet size %zu\n", cbNeeded, _maximumReadSize);
            return false;
        }

        do
        {   // If we're reading at a point in the buffer more than just the header, we're actually transferring data, so light up the LED
            // Read data from the socket until we have _bcNeeded bytes in the buffer

            int cbRead = 0;
            do {
                cbRead = read(socket, (uint8_t *) _pBuffer.get() + _cbReceived, cbNeeded - _cbReceived);
            } while (cbRead < 0 && errno == EINTR);

            // Restore the old state

            if (cbRead > 0)
            {
                _cbReceived += cbRead;
            }
            else
            {
                printf("ERROR: %d bytes read in ReadUntilNBytesReceived trying to read %ld\n", cbRead, cbNeeded-_cbReceived);
                return false;
            }
        } while (_cbReceived < cbNeeded);
        return true;
    }

    // ProcessIncomingConnectionsLoop
    //
    // Socket server main ProcessIncomingConnectionsLoop - accepts new connections and reads from them, dispatching
    // data packets into our buffer and closing the socket if anything goes weird.

    bool ProcessIncomingConnectionsLoop(LEDBufferManager & bufferManager)
    {
        // If an accept fails, we'll sleep this long in ms and try again
        constexpr std::chrono::milliseconds kSocketRetryDelay = 500ms;

        while (!_stopping.load())
        {
            if (0 > _server_fd)
            {
                printf("No _server_fd, returning.");
                return false;
            }

            // shutdown() does not wake a blocked listening accept() on macOS.
            // Wait in bounded poll() calls, then use nonblocking accept().
            pollfd listener{_server_fd, POLLIN, 0};
            const int ready = poll(&listener, 1, 100);
            if (_stopping.load())
                break;
            if (ready < 0)
            {
                if (errno == EINTR)
                    continue;
                perror("polling listener");
                return false;
            }
            if (ready == 0)
                continue;
            if (listener.revents & (POLLERR | POLLHUP | POLLNVAL))
            {
                fprintf(stderr, "Listener became unavailable\n");
                return false;
            }
            if (!(listener.revents & POLLIN))
                continue;

            socklen_t addrlen = sizeof(_address);
            const int new_socket = accept(_server_fd, reinterpret_cast<sockaddr *>(&_address), &addrlen);
            if (new_socket < 0)
            {
                if (_stopping.load())
                    break;
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                    continue;
                perror("accepting data");
                std::this_thread::sleep_for(kSocketRetryDelay);
                continue;
            }

            {
                std::lock_guard<std::mutex> lock(_socketMutex);
                if (_stopping.load())
                {
                    close(new_socket);
                    break;
                }
                _client_fd = new_socket;
            }

            // BSD systems can inherit the listener's nonblocking flag. Keep
            // accepted sockets blocking, with the read/write timeouts below.
            const int clientFlags = fcntl(new_socket, F_GETFL, 0);
            if (clientFlags < 0 || fcntl(new_socket, F_SETFL, clientFlags & ~O_NONBLOCK) < 0)
            {
                perror("setting client blocking");
                CloseClient(new_socket);
                continue;
            }

            // Report where this connection is coming from

            struct sockaddr_in addr;
            socklen_t addr_size = sizeof(struct sockaddr_in);
            if (0 != getpeername(new_socket, (struct sockaddr *)&addr, &addr_size))
            {
                printf("Unable to get peer name from socket!");
                CloseClient(new_socket);
                ResetReadBuffer();
                continue;
            }

            //printf("Incoming connection from: %s", inet_ntoa(addr.sin_addr));

            // Set a timeout of 3 seconds on the socket so we don't permanently hang on a corrupt or partial packet

            struct timeval to;
            to.tv_sec = 3;
            to.tv_usec = 0;
            if (setsockopt(new_socket,SOL_SOCKET,SO_RCVTIMEO,&to,sizeof(to)) < 0 ||
                setsockopt(new_socket,SOL_SOCKET,SO_SNDTIMEO,&to,sizeof(to)) < 0)
            {
                printf("Unable to set read timeout on socket!");
                CloseClient(new_socket);
                ResetReadBuffer();
                continue;
            }

            if (_pBuffer == nullptr) 
            {
                printf("Buffer not allocated!");
                CloseClient(new_socket);
                ResetReadBuffer();
                continue;
            }

            // Ensure the new_socket is valid
            if (new_socket < 0) 
            {
                printf("Invalid socket!");
                ResetReadBuffer();
                continue;
            }

            do
            {
                bool bSendResponsePacket = false;

                // Read until we have at least enough for the data header
                if (false == ReadUntilNBytesReceived(new_socket, STANDARD_DATA_HEADER_SIZE))
                {
                    printf("Read error in getting header.\n");
                    break;
                }

                // Now that we have the header we can see how much more data is expected to follow

                const uint32_t header  = DWORDFromMemory(_pBuffer.get());
                if (header == COMPRESSED_HEADER_TAG)
                {
                    uint32_t compressedSize = DWORDFromMemory(_pBuffer.get() + 4);
                    uint32_t expandedSize   = DWORDFromMemory(_pBuffer.get() + 8);
      
                    if (expandedSize > _maximumPacketSize || expandedSize < STANDARD_DATA_HEADER_SIZE ||
                        compressedSize > _maximumReadSize - COMPRESSED_HEADER_SIZE)
                    {
                        printf("Expanded packet would be %u but buffer is only %lu !!!!\n", expandedSize, _maximumPacketSize);
                        break;
                    }

                    if (false == ReadUntilNBytesReceived(new_socket, COMPRESSED_HEADER_SIZE + compressedSize))
                    {
                        printf("Could not read compressed data from stream\n");
                        break;
                    }
                    //printf("Successfuly read %u bytes", COMPRESSED_HEADER_SIZE + compressedSize);

                    // Define the source buffer span, excluding the compressed header
                    
                    auto sourceSpan = std::span<const uint8_t>(&_pBuffer[COMPRESSED_HEADER_SIZE], compressedSize);
                    auto outputSpan = std::span<uint8_t>(_abOutputBuffer.get(), expandedSize);

                    // Call the DecompressBuffer function with the spans
                    if (!DecompressBuffer(sourceSpan, outputSpan))
                    {
                        printf("Error decompressing data\n");
                        break;
                    }

                    auto payload = std::span<const uint8_t>(_abOutputBuffer.get(), expandedSize);
                    if (false == ProcessIncomingData(bufferManager, payload, _expectedPixels))
                    {
                        printf("Error processing data\n");
                        break;

                    }
                    ResetReadBuffer();
                    bSendResponsePacket = true;
                }
                else
                {     
                    uint16_t command16   = WORDFromMemory(_pBuffer.get());            
                    if (command16 == WIFI_COMMAND_PIXELDATA64)
                    {
                        // We know it's pixel data, so we do some validation before calling Process.
                        uint32_t length32  = DWORDFromMemory(_pBuffer.get()+4);
                        if (length32 != _expectedPixels)
                        {
                            printf("Frame pixel count %u does not match configured geometry (%zu)\n", length32, _expectedPixels);
                            break;
                        }
                        size_t totalExpected = STANDARD_DATA_HEADER_SIZE + size_t(length32) * LED_DATA_SIZE;
                        if (totalExpected > _maximumPacketSize)
                        {
                            printf("Too many bytes promised (%zu) - more than we can use for our LEDs at max packet (%lu)\n", totalExpected, _maximumPacketSize);
                            break;
                        }

                        //printf("Expecting %zu total bytes", totalExpected);
                        if (false == ReadUntilNBytesReceived(new_socket, totalExpected))
                        {
                            printf("Error in getting pixel data from network\n");
                            break;
                        }

                        // Add it to the buffer ring

                        auto payload = std::span<const uint8_t>(_pBuffer.get(), totalExpected);
                        if (false == ProcessIncomingData(bufferManager, payload, _expectedPixels))
                        {
                            printf("Error in processing pixel data from network\n");
                            break;
                        }

                        // Consume the data by resetting the buffer
                        //printf("Consuming the data as WIFI_COMMAND_PIXELDATA64 by setting _cbReceived to from %zu down 0.", _cbReceived);
                        ResetReadBuffer();

                        bSendResponsePacket = true;
                    }
                    else
                    {
                        printf("Unknown command in packet received: %u\n", command16);
                        break;
                    }
                }

                // If we make it to this point, it should be success, so we consume

                ResetReadBuffer();

                if (bSendResponsePacket)
                {
                    //printf("Sending Response Packet from Socket Server");

                    SocketResponse response = {
                                                .size = sizeof(SocketResponse),
                                                .flashVersion = 41,
                                                .currentClock = CAppTime::CurrentTime(),
                                                .oldestPacket = bufferManager.AgeOfOldestBuffer(),
                                                .newestPacket = bufferManager.AgeOfNewestBuffer(),
                                                .brightness   = 100,
                                                .wifiSignal   = 1000,
                                                .bufferSize   = (uint32_t)bufferManager.Capacity(),
                                                .bufferPos    = (uint32_t)bufferManager.Size(),
                                                .fpsDrawing   = (uint32_t)MatrixDraw::FPS(),
                                                .watts        = 0
                                            };

                    // I dont think this is fatal, and doesn't affect the read buffer, so content to ignore for now if it happens
                    try
                    {
                        size_t sent = 0;
                        while (sent < sizeof(response))
                        {
                            const auto count = write(new_socket, reinterpret_cast<const uint8_t *>(&response) + sent,
                                                     sizeof(response) - sent);
                            if (count < 0 && errno == EINTR)
                                continue;
                            if (count <= 0)
                                break;
                            sent += count;
                        }
                        if (sent != sizeof(response))
                            printf("Unable to send response back to server.");
                    }
                    catch(const std::exception& e)
                    {
                        std::cerr << e.what() << '\n';
                    }
                }
            } while (!_stopping.load());

            CloseClient(new_socket);
            ResetReadBuffer();
            if (!_stopping.load())
                std::this_thread::sleep_for(kSocketRetryDelay);
        }
        return true;
    }

    // DecompressBuffer
    //
    // Use unzlib to decompress a memory buffer

    static bool DecompressBuffer(std::span<const uint8_t> inputBuffer, std::span<uint8_t> outputBuffer)
    {
        z_stream stream;
        memset(&stream, 0, sizeof(stream));

        // Initialize the stream for decompression
        stream.next_in   = const_cast<Bytef*>(inputBuffer.data()); // Input buffer
        stream.avail_in  = inputBuffer.size();                    // Input buffer size
        stream.next_out  = outputBuffer.data();                   // Output buffer
        stream.avail_out = outputBuffer.size();                  // Output buffer size

        // Choose appropriate initialization based on compression format
        int ret = inflateInit2(&stream, MAX_WBITS);  
        if (ret != Z_OK) 
        {
            std::printf("ERROR: zlib inflateInit2 failed with code %d\n", ret);
            return false;
        }

        // Perform the decompression
        do 
        {
            ret = inflate(&stream, Z_FINISH);
            if (ret == Z_STREAM_ERROR) 
            {
                printf("Stream error during decompression\n");
                inflateEnd(&stream);
                return false;
            }
            if (ret == Z_DATA_ERROR) 
            {
                printf("Data error during decompression (possibly corrupted data)\n");
                inflateEnd(&stream);
                return false;
            }
            if (ret == Z_MEM_ERROR) 
            {
                printf("Memory error during decompression\n");
                inflateEnd(&stream);
                return false;
            }
            if (ret == Z_BUF_ERROR) 
            {
                // No progress is possible with these fixed input/output spans.
                printf("Buffer error during decompression.\n");
                inflateEnd(&stream);
                return false;
            }
        } while (ret != Z_STREAM_END);

        // Ensure the decompressed size matches the expected size
        if (stream.total_out != outputBuffer.size() || stream.avail_in != 0)
        {
            printf("Invalid decompressed frame: expected %zu bytes, got %lu with %u trailing input bytes\n",
                   outputBuffer.size(), stream.total_out, stream.avail_in);
            inflateEnd(&stream);
            return false;
        }

        // Clean up the stream
        inflateEnd(&stream);

        // Success
        return true;
    }
};
