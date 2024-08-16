#pragma once

#include <cstdint>

constexpr auto WIFI_COMMAND_PIXELDATA64 = 3;             // Wifi command with color data and 64-bit clock vals
constexpr auto WIFI_COMMAND_PEAKDATA    = 4;             // Wifi command that delivers audio peaks

// Matrix Defaults
//
// Can be overridden from the command line

constexpr auto DefaultHardwareMapping = "adafruit-hat";
constexpr auto DefaultChainLength     = 8;
constexpr auto DefualtRows            = 32;
constexpr auto DefaultColumns         = 64;
constexpr auto DefaultGPIOSlowdown    = 5;

#define NUM_LEDS (Rows * Columns * ChainLength)

// Helpers for extracting values from memory in a system-independent way

inline uint64_t ULONGFromMemory(const uint8_t * payloadData)
{
    return  (uint64_t)payloadData[7] << 56  |
            (uint64_t)payloadData[6] << 48  |
            (uint64_t)payloadData[5] << 40  |
            (uint64_t)payloadData[4] << 32  |
            (uint64_t)payloadData[3] << 24  |
            (uint64_t)payloadData[2] << 16  |
            (uint64_t)payloadData[1] << 8   |
            (uint64_t)payloadData[0];
}

inline uint32_t DWORDFromMemory(const uint8_t * payloadData)
{
    return  (uint32_t)payloadData[3] << 24  |
            (uint32_t)payloadData[2] << 16  |
            (uint32_t)payloadData[1] << 8   |
            (uint32_t)payloadData[0];
}

inline uint16_t WORDFromMemory(const uint8_t * payloadData)
{
    return  (uint16_t)payloadData[1] << 8   |
            (uint16_t)payloadData[0];
}
