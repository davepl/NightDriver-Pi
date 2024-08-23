#pragma once

#include <cstdint>

constexpr auto WIFI_COMMAND_PIXELDATA64 = 3;             // Wifi command with color data and 64-bit clock vals
constexpr auto WIFI_COMMAND_PEAKDATA    = 4;             // Wifi command that delivers audio peaks

// Matrix Defaults
//
// Can be overridden from the command line

constexpr auto kDefaultHardwareMapping    = "adafruit-hat-pwm";
constexpr auto kDefaultChainLength        = 8;
constexpr auto kDefualtRows               = 32;
constexpr auto kDefaultColumns            = 64;
constexpr auto kDefaultGPIOSlowdown       = 5;
constexpr auto kDefaultRefreshRate        = 60;
constexpr auto kDefaultDisableBusyWaiting = true;

constexpr auto kIncomingSocketPort        = 49152;
constexpr auto kMaxBuffers                = 500;

#define NUM_LEDS (Rows * Columns * ChainLength)

// Helpers for extracting values from memory in a system-independent way

constexpr uint64_t ULONGFromMemory(const uint8_t * payloadData)
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

constexpr uint32_t DWORDFromMemory(const uint8_t * payloadData)
{
    return  (uint32_t)payloadData[3] << 24  |
            (uint32_t)payloadData[2] << 16  |
            (uint32_t)payloadData[1] << 8   |
            (uint32_t)payloadData[0];
}

constexpr uint16_t WORDFromMemory(const uint8_t * payloadData)
{
    return  (uint16_t)payloadData[1] << 8   |
            (uint16_t)payloadData[0];
}
