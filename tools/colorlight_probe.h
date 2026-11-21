// SPDX-License-Identifier: GPL-3.0-or-later
// Independent protocol facts from receiverproxy (MIT), commit
// dab8b41aa7e05e91607eeda1ad4b5d97bc085b48:
// https://github.com/AlecDusheck/receiverproxy/tree/dab8b41aa7e05e91607eeda1ad4b5d97bc085b48/crates/colorlight/src
// No receiverproxy code is incorporated. Only discovery and READ are exposed.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace colorlight_probe
{
inline constexpr std::array<uint8_t, 6> CardMac{0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
inline constexpr std::array<uint8_t, 6> SenderMac{0x22, 0x22, 0x33, 0x44, 0x55, 0x66};

inline std::vector<uint8_t> DiscoveryRequest()
{
    std::vector<uint8_t> frame(284, 0);
    std::copy(CardMac.begin(), CardMac.end(), frame.begin());
    std::copy(SenderMac.begin(), SenderMac.end(), frame.begin() + 6);
    frame[12] = 0x07;
    return frame;
}

inline std::vector<uint8_t> FlashReadRequest(uint16_t page)
{
    std::vector<uint8_t> frame(140, 0);
    std::copy(CardMac.begin(), CardMac.end(), frame.begin());
    std::copy(SenderMac.begin(), SenderMac.end(), frame.begin() + 6);
    frame[12] = 0x06;
    // Bytes 15/16 address receiver index zero. This is strictly a READ frame:
    // no erase, unlock, data upload, reload, layout, or write opcode exists here.
    frame[17] = 0x44;
    frame[18] = 0x01;
    frame[19] = static_cast<uint8_t>(page >> 8);
    frame[20] = static_cast<uint8_t>(page);
    return frame;
}

inline bool CardReply(std::span<const uint8_t> frame, uint16_t type)
{
    return frame.size() >= 14 && std::equal(CardMac.begin(), CardMac.end(), frame.begin() + 6) &&
           frame[12] == static_cast<uint8_t>(type >> 8) && frame[13] == static_cast<uint8_t>(type);
}

inline bool DiscoveryReply(std::span<const uint8_t> frame)
{
    // The known discovery frame is 1070 bytes. Requiring the full shape avoids
    // interpreting a truncated packet or a different 0x08 response as identity.
    return frame.size() >= 1070 && CardReply(frame, 0x0805);
}

inline bool FlashReply(std::span<const uint8_t> frame)
{
    return frame.size() >= 15 + 1024 && CardReply(frame, 0x0901);
}

inline std::array<uint8_t, 1024> FlashData(std::span<const uint8_t> frame)
{
    if (!FlashReply(frame))
        throw std::invalid_argument("Not a complete Colorlight 0x0901 flash-read reply");
    std::array<uint8_t, 1024> data;
    std::copy_n(frame.begin() + 15, data.size(), data.begin());
    return data;
}
}
