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
#include <stdlib.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include <zlib.h>
#include <memory>
#include <iostream>

#include "ledbuffer.h"

#define STANDARD_DATA_HEADER_SIZE   24                                              // Size of the header for expanded data
#define COMPRESSED_HEADER_SIZE      16                                              // Size of the header for compressed data
#define LED_DATA_SIZE               sizeof(CRGB)                                    // Data size of an LED (24 bits or 3 bytes)

// We allocate whatever the max packet is, and use it to validate incoming packets, so right now, it's set to the maximum
// LED data packet you could have (header plus 3 RGBs per NUM_LED)

#define COMPRESSED_HEADER_TAG (0x44415645)                                              // asci "DAVE" as header

extern volatile bool interrupt_received;

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
    std::unique_ptr<uint8_t []> _pBuffer;
    std::unique_ptr<uint8_t []> _abOutputBuffer;
    size_t                      _maximumPacketSize;

public:

    size_t                      _cbReceived;

    SocketServer(int port, size_t maxLEDs) :
        _port(port),
        _server_fd(-1),
        _maximumPacketSize(STANDARD_DATA_HEADER_SIZE + LED_DATA_SIZE * maxLEDs),
        _cbReceived(0)
    {
        _abOutputBuffer = std::make_unique<uint8_t []>(_maximumPacketSize);        // Must add +1 for LZ bug case in Arduino library if ever backported there
        _pBuffer = std::make_unique<uint8_t []>(_maximumPacketSize);
        memset(&_address, 0, sizeof(_address));
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
            release();
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

        memset(&_address, 0, sizeof(_address));
        _address.sin_family = AF_INET;
        _address.sin_addr.s_addr = INADDR_ANY;
        _address.sin_port = htons( _port );

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
        return true;
    }

    void end()
    {
        release();
        return;
    }
    
    // ProcessIncomingData
    //
    // Takes the packet in raw form, decodes enough of it to inspect the command and channel, and then creates and pushes
    // a new LEDBuffer for the data when appropriate.

    bool ProcessIncomingData(LEDBufferManager & bufferManager, std::unique_ptr<uint8_t []> & payloadData, size_t payloadLength)
    {
        uint16_t command16 = payloadData[1] << 8 | payloadData[0];
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
                bufferManager.PushNewBuffer( LEDBuffer::CreateFromWire(payloadData, payloadLength) );
            }
            catch(const LEDBufferException & e)
            {
                std::cerr << e.what() << '\n';
                return false;
            }
        }
        return true;
    }

    void ResetReadBuffer()
    {
        _cbReceived = 0;
        memset(_pBuffer.get(), 0, _maximumPacketSize);
    }

    // ReadUntilNBytesReceived
    //
    // Read from the socket until the buffer contains at least cbNeeded bytes

    bool ReadUntilNBytesReceived(size_t socket, size_t cbNeeded)
    {
        if (cbNeeded <= _cbReceived)                            // If we already have that many bytes, we're already done
            return true;

        // This test caps maximum packet size as a full buffer read of LED data.  If other packets wind up being longer,
        // the buffer itself and this test might need to change

        if (cbNeeded > _maximumPacketSize)
            return false;

        do
        {
            // If we're reading at a point in the buffer more than just the header, we're actually transferring data, so light up the LED

            // Read data from the socket until we have _bcNeeded bytes in the buffer

            int cbRead = 0;
            do 
            {
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
        while (!interrupt_received)
        {
            if (0 >= _server_fd)
            {
                printf("No _server_fd, returning.");
                continue;
            }

            int new_socket = 0;

            // Accept a new incoming connnection
            int addrlen = sizeof(_address);
            if ((new_socket = accept(_server_fd, (struct sockaddr *)&_address, (socklen_t*)&addrlen))<0)
            {
                printf("Error accepting data!");
                continue;
            }

            // Report where this connection is coming from

            struct sockaddr_in addr;
            socklen_t addr_size = sizeof(struct sockaddr_in);
            if (0 != getpeername(new_socket, (struct sockaddr *)&addr, &addr_size))
            {
                close(new_socket);
                ResetReadBuffer();
                continue;
            }

            printf("Incoming connection from: %s", inet_ntoa(addr.sin_addr));

            // Set a timeout of 3 seconds on the socket so we don't permanently hang on a corrupt or partial packet

            struct timeval to;
            to.tv_sec = 3;
            to.tv_usec = 0;
            if (setsockopt(new_socket,SOL_SOCKET,SO_RCVTIMEO,&to,sizeof(to)) < 0)
            {
                printf("Unable to set read timeout on socket!");
                close(new_socket);
                ResetReadBuffer();
                continue;
            }

            if (_pBuffer == nullptr) 
            {
                printf("Buffer not allocated!");
                close(new_socket);
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

                const uint32_t header  = _pBuffer[3] << 24  | _pBuffer[2] << 16  | _pBuffer[1] << 8  | _pBuffer[0];
                if (header == COMPRESSED_HEADER_TAG)
                {
                    uint32_t compressedSize = _pBuffer[7] << 24  | _pBuffer[6] << 16  | _pBuffer[5] << 8  | _pBuffer[4];
                    uint32_t expandedSize   = _pBuffer[11] << 24 | _pBuffer[10] << 16 | _pBuffer[9] << 8  | _pBuffer[8];
                    // Unused: uint32_t reserved       = _pBuffer[15] << 24 | _pBuffer[14] << 16 | _pBuffer[13] << 8 | _pBuffer[12];
                    //printf("Compressed Header: compressedSize: %u, expandedSize: %u, reserved: %u", compressedSize, expandedSize, reserved);

                    if (expandedSize > _maximumPacketSize)
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

                    auto pSourceBuffer = &_pBuffer[COMPRESSED_HEADER_SIZE];

                    if (!DecompressBuffer(pSourceBuffer, compressedSize, _abOutputBuffer.get(), expandedSize))
                    {
                        printf("Error decompressing data\n");
                        break;
                    }

                    if (false == ProcessIncomingData(bufferManager, _abOutputBuffer, expandedSize))
                    {
                        printf("Error processing data\n");
                        break;

                    }
                    ResetReadBuffer();
                    bSendResponsePacket = true;
                }
                else
                {     
                    uint16_t command16   = WORDFromMemory(&_pBuffer.get()[0]);            
                    if (command16 == WIFI_COMMAND_PIXELDATA64)
                    {
                        // We know it's pixel data, so we do some validation before calling Process.

                        // Unused: uint16_t channel16 = WORDFromMemory(&_pBuffer.get()[2]);
                        uint32_t length32  = DWORDFromMemory(&_pBuffer.get()[4]);
                        // Unused: uint64_t seconds   = ULONGFromMemory(&_pBuffer.get()[8]);
                        // Unused: uint64_t micros    = ULONGFromMemory(&_pBuffer.get()[16]);

                        //printf("Uncompressed Header: channel16=%u, length=%u, seconds=%llu, micro=%llu", channel16, length32, seconds, micros);

                        size_t totalExpected = STANDARD_DATA_HEADER_SIZE + length32 * LED_DATA_SIZE;
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

                        if (false == ProcessIncomingData(bufferManager, _pBuffer, totalExpected))
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
                                                .flashVersion = 0,
                                                .currentClock = CAppTime::CurrentTime(),
                                                .oldestPacket = bufferManager.AgeOfOldestBuffer(),
                                                .newestPacket = bufferManager.AgeOfNewestBuffer(),
                                                .brightness   = 100,
                                                .wifiSignal   = 99,
                                                .bufferSize   = (uint32_t)bufferManager.Capacity(),
                                                .bufferPos    = (uint32_t)bufferManager.Size(),
                                                .fpsDrawing   = 0,
                                                .watts        = 0
                                            };

                    // I dont think this is fatal, and doesn't affect the read buffer, so content to ignore for now if it happens
                    if (sizeof(response) != write(new_socket, &response, sizeof(response)))
                        printf("Unable to send response back to server.");
                }
            } while (true);

            close(new_socket);
            ResetReadBuffer();
            sleep(1);
        }
        return true;
    }

    // DecompressBuffer
    //
    // Use unzlib to decompress a memory buffer

    bool DecompressBuffer(const uint8_t* pBuffer, size_t cBuffer, uint8_t* pOutput, size_t expectedOutputSize) 
    {
        z_stream stream;
        memset(&stream, 0, sizeof(stream));

        // Initialize the stream for decompression
        stream.next_in = const_cast<Bytef*>(pBuffer); // Input buffer
        stream.avail_in = cBuffer;                   // Input buffer size
        stream.next_out = pOutput;                   // Output buffer
        stream.avail_out = expectedOutputSize;       // Output buffer size

        // Choose appropriate initialization based on compression format
        int ret = inflateInit2(&stream, MAX_WBITS);  // Use -MAX_WBITS for raw deflate; for zlib/gzip header use different options
        if (ret != Z_OK) {
            printf("ERROR: zlib inflateInit2 failed with code %d\n", ret);
            return false;
        }

        // Perform the decompression
        do {
            ret = inflate(&stream, Z_NO_FLUSH); // Use Z_NO_FLUSH for incremental decompression
            if (ret == Z_STREAM_ERROR) {
                printf("Stream error during decompression\n");
                inflateEnd(&stream);
                return false;
            }

            if (ret == Z_DATA_ERROR) {
                printf("Data error during decompression (possibly corrupted data)\n");
                inflateEnd(&stream);
                return false;
            }

            if (ret == Z_MEM_ERROR) {
                printf("Memory error during decompression\n");
                inflateEnd(&stream);
                return false;
            }

            if (ret == Z_BUF_ERROR) {
                // Buffer error, not necessarily fatal, but output buffer may be too small
                printf("Buffer error during decompression. Possibly insufficient output buffer size.\n");
            }

        } while (ret != Z_STREAM_END);

        // Ensure the decompressed size matches the expected size
        if (stream.total_out != expectedOutputSize) {
            printf("Expected %zu bytes, but decompressed to %lu bytes instead\n", expectedOutputSize, stream.total_out);
            inflateEnd(&stream);
            return false;
        }

        // Clean up the stream
        inflateEnd(&stream);

        // Success
        return true;
    }
};
