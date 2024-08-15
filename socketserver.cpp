
#include "socketserver.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

bool SocketServer::ProcessIncomingConnectionsLoop(LEDBufferManager & bufferManager)
{
    if (0 >= _server_fd)
    {
        printf("No _server_fd, returning.");
        return false;
    }

    int new_socket = 0;

    // Accept a new incoming connnection
    int addrlen = sizeof(_address);
    if ((new_socket = accept(_server_fd, (struct sockaddr *)&_address, (socklen_t*)&addrlen))<0)
    {
        printf("Error accepting data!");
        return false;
    }

    // Report where this connection is coming from

    struct sockaddr_in addr;
    socklen_t addr_size = sizeof(struct sockaddr_in);
    if (0 != getpeername(new_socket, (struct sockaddr *)&addr, &addr_size))
    {
        close(new_socket);
        ResetReadBuffer();
        return false;
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
        return false;
    }

    if (_pBuffer == nullptr) 
    {
        printf("Buffer not allocated!");
        close(new_socket);
        ResetReadBuffer();
        return false;
    }

    // Ensure the new_socket is valid
    if (new_socket < 0) {
        printf("Invalid socket!");
        ResetReadBuffer();
        return false;
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
        if (header == COMPRESSED_HEADER)
        {
            uint32_t compressedSize = _pBuffer[7] << 24  | _pBuffer[6] << 16  | _pBuffer[5] << 8  | _pBuffer[4];
            uint32_t expandedSize   = _pBuffer[11] << 24 | _pBuffer[10] << 16 | _pBuffer[9] << 8  | _pBuffer[8];
            uint32_t reserved       = _pBuffer[15] << 24 | _pBuffer[14] << 16 | _pBuffer[13] << 8 | _pBuffer[12];
            //printf("Compressed Header: compressedSize: %u, expandedSize: %u, reserved: %u", compressedSize, expandedSize, reserved);

            if (expandedSize > MAXIMUM_PACKET_SIZE)
            {
                printf("Expanded packet would be %u but buffer is only %lu !!!!\n", expandedSize, MAXIMUM_PACKET_SIZE);
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

            if (false == ProcessIncomingData(_abOutputBuffer, expandedSize))
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

                uint16_t channel16 = WORDFromMemory(&_pBuffer.get()[2]);
                uint32_t length32  = DWORDFromMemory(&_pBuffer.get()[4]);
                uint64_t seconds   = ULONGFromMemory(&_pBuffer.get()[8]);
                uint64_t micros    = ULONGFromMemory(&_pBuffer.get()[16]);

                //printf("Uncompressed Header: channel16=%u, length=%u, seconds=%llu, micro=%llu", channel16, length32, seconds, micros);

                size_t totalExpected = STANDARD_DATA_HEADER_SIZE + length32 * LED_DATA_SIZE;
                if (totalExpected > MAXIMUM_PACKET_SIZE)
                {
                    printf("Too many bytes promised (%zu) - more than we can use for our LEDs at max packet (%lu)\n", totalExpected, MAXIMUM_PACKET_SIZE);
                    break;
                }

                //printf("Expecting %zu total bytes", totalExpected);
                if (false == ReadUntilNBytesReceived(new_socket, totalExpected))
                {
                    printf("Error in getting pixel data from network\n");
                    break;
                }

                // Add it to the buffer ring

                if (false == ProcessIncomingData(_pBuffer, totalExpected))
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
    return false;
}
