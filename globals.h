#pragma once

#include <cstdint>
#include <bit>

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
constexpr auto kDefaultRefreshRate        = 100;
constexpr auto kDefaultDisableBusyWaiting = true;

constexpr auto kIncomingSocketPort        = 49152;
constexpr auto kMaxBuffers                = 500;

// Helpers for extracting values from memory in a system-independent way

constexpr uint64_t ULONGFromMemory(const uint8_t * payloadData)
{
    const auto result = *(uint64_t *)payloadData;
    if constexpr (std::endian::native == std::endian::little) 
        return result;                      // Already little-endian, no swap needed
    else 
        return __builtin_bswap64(result);   // Swap for big-endian systems
}

constexpr uint32_t DWORDFromMemory(const uint8_t * payloadData)
{
    const auto result = *(uint32_t *)payloadData;
    if constexpr (std::endian::native == std::endian::little) 
        return result;                      // Already little-endian, no swap needed
    else 
        return __builtin_bswap32(result);   // Swap for big-endian systems
}

constexpr uint16_t WORDFromMemory(const uint8_t * payloadData)
{
    const auto result = *(uint16_t *)payloadData;
    if constexpr (std::endian::native == std::endian::little) 
        return result;                      // Already little-endian, no swap needed
    else 
        return __builtin_bswap16(result);   // Swap for big-endian systems
}