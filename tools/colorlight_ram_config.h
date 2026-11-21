// SPDX-License-Identifier: GPL-3.0-or-later
// Independent RAM-packet facts from receiverproxy (MIT), commit
// dab8b41aa7e05e91607eeda1ad4b5d97bc085b48, crates/ops/src/params.rs and
// docs/receiver-identity.md. No receiverproxy or vendor SDK code is incorporated.
// https://github.com/AlecDusheck/receiverproxy/tree/dab8b41aa7e05e91607eeda1ad4b5d97bc085b48
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace colorlight_ram_config
{
inline constexpr size_t ImageBytes = 64 * 1024;
using Packet = std::vector<uint8_t>;

struct ImageDetails
{
    uint16_t width;
    uint16_t height;
    uint8_t scan;
    uint8_t gray_bits;
    uint16_t chip_id;
    uint8_t line_direction;
    uint32_t basic_crc;
};

inline uint16_t Big16(std::span<const uint8_t> bytes, size_t offset)
{
    return uint16_t(uint16_t(bytes[offset]) << 8) | bytes[offset + 1];
}

inline uint32_t Big32(std::span<const uint8_t> bytes, size_t offset)
{
    return (uint32_t(Big16(bytes, offset)) << 16) | Big16(bytes, offset + 2);
}

// IEEE CRC-32, identical to zlib.crc32. Chip selection is patched after the SDK
// calculates this checksum, so the three chip-ID bytes are hashed as zero.
inline uint32_t BasicCrc(std::span<const uint8_t> basic)
{
    if (basic.size() != 256)
        throw std::invalid_argument("Basic parameter body must contain exactly 256 bytes");
    uint32_t crc = 0xffffffffu;
    for (size_t offset = 0; offset < 252; ++offset)
    {
        crc ^= offset == 0x1b || offset == 0xe7 || offset == 0xe8 ? 0 : basic[offset];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0u);
    }
    return ~crc;
}

inline bool Filled(std::span<const uint8_t> bytes, uint8_t value)
{
    return std::all_of(bytes.begin(), bytes.end(), [value](uint8_t byte) { return byte == value; });
}

inline ImageDetails ValidateImage(std::span<const uint8_t> image)
{
    if (image.size() != ImageBytes)
        throw std::invalid_argument("Expected a complete 65536-byte compiled parameter image, not an .rcvbp file");
    const auto basic = image.first(256);
    if (basic[0] != 0xa8)
        throw std::invalid_argument("Compiled basic parameter body is missing its 0xA8 marker");
    const uint32_t stored = uint32_t(basic[252]) | (uint32_t(basic[253]) << 8) |
                            (uint32_t(basic[254]) << 16) | (uint32_t(basic[255]) << 24);
    const uint32_t actual = BasicCrc(basic);
    if (stored != actual)
        throw std::invalid_argument("Basic parameter CRC-32 is invalid; rebuild the compiled image before applying it");
    const uint16_t chip = basic[0x1b] == 0xfe ? Big16(basic, 0xe7) : basic[0x1b];
    ImageDetails details{Big16(basic, 0x88), Big16(basic, 0x8a), basic[7], basic[8], chip, basic[0x1d], actual};
    if (!details.width || !details.height || uint64_t(details.width) * details.height > 655360)
        throw std::invalid_argument("Compiled geometry must be positive and contain at most 655360 pixels");
    if (details.scan < 1 || details.scan > 128)
        throw std::invalid_argument("Compiled scan value must be between 1 and 128");
    if (details.gray_bits < 1 || details.gray_bits > 16 || details.line_direction > 3)
        throw std::invalid_argument("Unsupported gray depth or line direction in compiled basic parameters");
    // This first utility supports the reviewed generic/normal driver path.
    // FM6124 is selected through normal base chip 0x0A plus its custom data.
    if (chip != 0 && chip != 0x0a)
        throw std::invalid_argument("Only reviewed generic chip 0 or normal base chip 0x0A is supported");
    const unsigned red = basic[0x10] & 3;
    const unsigned green = (basic[0x10] >> 2) & 3;
    const unsigned blue = (basic[0x10] >> 4) & 3;
    if (red > 2 || green > 2 || blue > 2 || red == green || red == blue || green == blue)
        throw std::invalid_argument("Compiled RGB source fields must be a permutation of 0, 1, 2");
    const auto registers = image.subspan(0x0900, 0x100);
    if (!Filled(registers, 0) && !Filled(registers, 0xff))
        throw std::invalid_argument("Nonempty chip-register region 0x0900 requires an unsupported register pack");
    // Current-segment data still has no reviewed RAM packet in this utility.
    const auto currentSegment = image.subspan(0x0a00, 0x200);
    if (!Filled(currentSegment, 0) && !Filled(currentSegment, 0xff))
        throw std::invalid_argument("Nonempty current-segment region 0x0A00 has no reviewed RAM wire type");
    return details;
}

// Offline image-region diagnostic only. An image cannot reconstruct every
// runtime routing table; full hardware application requires an audited bundle.
inline std::vector<Packet> BuildRamPackets(std::span<const uint8_t> image)
{
    const auto details = ValidateImage(image);
    constexpr std::array<uint8_t, 12> addresses{
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x22, 0x22, 0x33, 0x44, 0x55, 0x66};
    std::vector<Packet> packets;
    packets.reserve(36);
    auto Append = [&](uint8_t kind, uint8_t sub, size_t header, size_t offset, size_t length)
    {
        Packet frame(12 + header + length, 0);
        std::copy(addresses.begin(), addresses.end(), frame.begin());
        frame[12] = kind;
        frame[15] = sub;
        std::copy_n(image.begin() + offset, length, frame.begin() + 12 + header);
        packets.push_back(std::move(frame));
    };
    Append(0x05, 2, 4, 0x0500, 0x100); // Data swap precedes basic parameters.
    Append(0x05, 0, 4, 0x0000, 0x100);
    const auto currentExchange = image.subspan(0x0c00, 0x100);
    if (!Filled(currentExchange, 0) && !Filled(currentExchange, 0xff))
    {
        // Audited vendor GetParamPacksBasic @ 0x32475f: immutable 12-byte
        // RAM current-exchange header, followed by the image's 256-byte body.
        // Its use is capability-conditional in the SDK; 5A acceptance is not
        // established, and unsupported receiver firmware may ignore it.
        constexpr std::array<uint8_t, 12> header{
            0x86, 0x00, 0x00, 0xff, 0xff, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00};
        Packet exchange(12 + header.size() + currentExchange.size(), 0);
        std::copy(addresses.begin(), addresses.end(), exchange.begin());
        std::copy(header.begin(), header.end(), exchange.begin() + 12);
        std::copy(currentExchange.begin(), currentExchange.end(), exchange.begin() + 24);
        packets.push_back(std::move(exchange));
    }
    Append(0x10, 0, 4, 0x0100, 0x400);
    Append(0x17, 0, 5, 0x0600, 0x300);
    packets.back()[15] = 0xff;
    packets.back()[16] = 0xff;
    for (uint8_t sub = 0; sub < 16; ++sub)
        Append(0x03, sub, 4, 0x3000 + size_t(sub) * 0x300, 0x300);
    constexpr std::array<size_t, 4> voidLine{0x1000, 0x1400, 0x6800, 0x6c00};
    for (uint8_t sub = 0; sub < voidLine.size(); ++sub)
        Append(0x1f, sub, 8, voidLine[sub], 0x400);
    constexpr std::array<size_t, 8> antiVoid{0x1800, 0x1c00, 0x2000, 0x2400, 0x7000, 0x7400, 0x7800, 0x7c00};
    for (uint8_t sub = 0; sub < antiVoid.size(); ++sub)
        Append(0x32, sub, 8, antiVoid[sub], 0x400);
    Append(0x18, 0, 4, 0x6000, 0x400);
    // Extended SDK scan tables need a second pack. Erased space is not a table.
    if (!Filled(image.subspan(0x6400, 0x400), 0xff))
        Append(0x18, 1, 4, 0x6400, 0x400);

    // The SDK cycles the supplied rectangles through all 128 entries. For an
    // isolated single receiver, repeat its rectangle rather than leaving zero
    // windows in the remainder of the packet (GetCardAreaPacksEx).
    Packet area(16 + 128 * 10, 0);
    std::copy(addresses.begin(), addresses.end(), area.begin());
    area[12] = 0x02;
    for (size_t entry = 0; entry < 128; ++entry)
    {
        const size_t at = 16 + entry * 10;
        area[at + 4] = static_cast<uint8_t>(details.width >> 8);
        area[at + 5] = static_cast<uint8_t>(details.width);
        area[at + 6] = static_cast<uint8_t>(details.height >> 8);
        area[at + 7] = static_cast<uint8_t>(details.height);
    }
    packets.push_back(std::move(area));
    return packets;
}

inline constexpr std::array<uint8_t, 8> BundleMagic{'N', 'D', 'R', 'A', 'M', '0', '1', 0};
inline constexpr size_t MaximumBundleBytes = 256 * 1024;
inline constexpr std::array<uint8_t, 12> ProtocolAddresses{
    0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x22, 0x22, 0x33, 0x44, 0x55, 0x66};

struct PacketBundle
{
    ImageDetails details{};
    std::vector<Packet> packets;
};

// Only exact, reviewed volatile SDK frame shapes are accepted. Additional
// routing/LUT headers and lengths were checked against actual SDK runtime
// output; these are wire-format facts, not imported SDK code. Packet bodies
// remain opaque and unchanged; no vendor-generated payload is stored in source.
inline void ValidateBundleFrame(std::span<const uint8_t> frame)
{
    if (frame.size() < 60 || frame.size() > 1514)
        throw std::invalid_argument("RAM bundle frame length must be 60..1514 bytes");
    if (!std::equal(ProtocolAddresses.begin(), ProtocolAddresses.end(), frame.begin()) || frame[13] != 0 || frame[14] != 0)
        throw std::invalid_argument("RAM bundle frame has an invalid Ethernet/protocol header");
    auto Require = [](bool valid)
    {
        if (!valid) throw std::invalid_argument("RAM bundle frame has an unreviewed length, sub-index or header");
    };
    switch (frame[12])
    {
        case 0x05:
            Require(frame.size() == 272 && (frame[15] == 0 || frame[15] == 2));
            break;
        case 0x02:
            Require(frame.size() == 1296 && frame[15] == 0);
            break;
        case 0x03:
            Require(frame.size() == 784 && frame[15] < 16);
            break;
        case 0x10:
            Require(frame.size() == 1040 && frame[15] == 0);
            break;
        case 0x17:
            Require(frame.size() == 785 && frame[15] == 0xff && frame[16] == 0xff);
            break;
        case 0x18:
            Require(frame.size() == 1040 && frame[15] < 2);
            break;
        case 0x1f:
        case 0x32:
            Require(frame.size() == 1048 && frame[15] < (frame[12] == 0x1f ? 4 : 8) &&
                    Filled(frame.subspan(16, 4), 0));
            break;
        case 0x41:
            Require(frame.size() == 1066 && frame[15] == 0xff && frame[16] == 0xff);
            break;
        case 0x83:
            Require(frame.size() == 788 && frame[15] < 4 && frame[16] == 0 && frame[17] == 0 &&
                    frame[18] == 4 && frame[19] == 0);
            break;
        case 0x37:
            // SDK normal-layout conversion produces pixel-route fragments,
            // scan routes and active-group routes; the old empty route only
            // clears groups. Both forms are volatile, never EEPROM commands.
            Require(frame.size() == 801 && frame[15] <= 2 &&
                    frame[16] <= (frame[15] == 0 ? 1 : 0) &&
                    ((frame[17] == 0 && frame[18] == 1) ||
                     (frame[15] == 2 && frame[16] == 0 && frame[17] == 0xff && frame[18] == 0)));
            break;
        case 0x76:
        case 0x73:
            // Audited SDK Get8bitGamaTablePack: volatile input-to-drive LUTs.
            // The independent EEPROM persistence path is never accepted here.
            Require(frame.size() == 1171 && Filled(frame.subspan(16, 3), 0) &&
                    (frame[12] == 0x76 ? frame[15] == 0 : (frame[15] == 1 || frame[15] == 2)));
            break;
        case 0x1b:
        case 0x2b:
        {
            // Native SDK GetSendCMD_Eeprom[Ex]Param emits opcode-zero runtime
            // shadow records. These are distinct from GetSaveCMD_EepromParam's
            // type 0x19 / opcode 0x85 persistence path. Only the exact reviewed
            // runtime record shapes are accepted; write/save opcodes stay banned.
            Require(frame.size() == 282 && frame[15] == 0xff && frame[16] == 0xff && frame[17] == 0);
            struct Record { uint8_t type; uint16_t address; uint16_t length; };
            constexpr std::array<Record, 22> records{{
                {0x1b,0x41,5}, {0x1b,0x4b,1}, {0x1b,0x4d,4}, {0x1b,0x51,4},
                {0x1b,0x55,5}, {0x1b,0x6f,1}, {0x1b,0x71,1}, {0x1b,0x7c,1},
                {0x1b,0x7d,1}, {0x1b,0x7e,3}, {0x1b,0xb2,1}, {0x1b,0xb3,4},
                {0x1b,0xce,16}, {0x1b,0xde,2}, {0x1b,0xe0,1}, {0x1b,0xe2,28},
                {0x1b,0xf9,1}, {0x1b,0xfa,2}, {0x1b,0xfd,1},
                {0x2b,0x100,24}, {0x2b,0x118,15}, {0x2b,0x127,22}
            }};
            const auto address = Big32(frame, 18), length = Big32(frame, 22);
            Require(std::any_of(records.begin(), records.end(), [&](const auto &record)
            { return record.type == frame[12] && record.address == address && record.length == length; }));
            Require(Filled(frame.subspan(26 + length), 0));
            break;
        }
        default:
            // All unknown types, including persistent EEPROM, flash, save, reload and
            // firmware command families, are rejected before opening BPF.
            throw std::invalid_argument("Unapproved packet type in RAM bundle");
    }
}

inline PacketBundle ParsePacketBundle(std::span<const uint8_t> bytes)
{
    if (bytes.size() < 10 || bytes.size() > MaximumBundleBytes ||
        !std::equal(BundleMagic.begin(), BundleMagic.end(), bytes.begin()))
        throw std::invalid_argument("Invalid RAM bundle magic or file size (maximum 256 KiB)");
    const size_t count = size_t(bytes[8]) | (size_t(bytes[9]) << 8);
    if (count < 1 || count > 128)
        throw std::invalid_argument("RAM bundle must contain 1..128 frames");
    PacketBundle result;
    result.packets.reserve(count);
    size_t offset = 10;
    size_t basics = 0, areas = 0, modules = 0;
    std::array<size_t, 3> gamma{};
    for (size_t index = 0; index < count; ++index)
    {
        if (bytes.size() - offset < 2)
            throw std::invalid_argument("Truncated RAM bundle frame length");
        const size_t length = size_t(bytes[offset]) | (size_t(bytes[offset + 1]) << 8);
        offset += 2;
        if (length < 60 || length > 1514 || length > bytes.size() - offset)
            throw std::invalid_argument("Invalid or truncated RAM bundle frame");
        const auto frame = bytes.subspan(offset, length);
        ValidateBundleFrame(frame);
        if (frame[12] == 0x05 && frame[15] == 0)
        {
            if (++basics != 1) throw std::invalid_argument("RAM bundle must have exactly one basic-parameter frame");
            std::vector<uint8_t> image(ImageBytes, 0);
            std::copy_n(frame.begin() + 16, 256, image.begin());
            result.details = ValidateImage(image);
        }
        if (frame[12] == 0x02) ++areas;
        if (frame[12] == 0x17) ++modules;
        if (frame[12] == 0x76) ++gamma[0];
        if (frame[12] == 0x73) ++gamma[frame[15]];
        result.packets.emplace_back(frame.begin(), frame.end());
        offset += length;
    }
    if (offset != bytes.size())
        throw std::invalid_argument("RAM bundle has trailing bytes");
    if (basics != 1 || areas != 1 || modules != 1)
        throw std::invalid_argument("RAM bundle requires exactly one basic, card-area, and module-position frame");
    if (gamma != std::array<size_t, 3>{} && gamma != std::array<size_t, 3>{1, 1, 1})
        throw std::invalid_argument("RAM bundle gamma LUTs must contain exactly all three reviewed frames or none");
    return result;
}

// The serializer is useful for offline bundle builders/tests. Validation is
// mandatory here too, so it cannot package an unapproved frame for application.
inline std::vector<uint8_t> EncodePacketBundle(std::span<const Packet> packets)
{
    if (packets.empty() || packets.size() > 128)
        throw std::invalid_argument("RAM bundle must contain 1..128 frames");
    std::vector<uint8_t> bytes(BundleMagic.begin(), BundleMagic.end());
    bytes.push_back(static_cast<uint8_t>(packets.size()));
    bytes.push_back(static_cast<uint8_t>(packets.size() >> 8));
    for (const auto &packet : packets)
    {
        ValidateBundleFrame(packet);
        if (bytes.size() + 2 + packet.size() > MaximumBundleBytes)
            throw std::invalid_argument("RAM bundle exceeds 256 KiB");
        bytes.push_back(static_cast<uint8_t>(packet.size()));
        bytes.push_back(static_cast<uint8_t>(packet.size() >> 8));
        bytes.insert(bytes.end(), packet.begin(), packet.end());
    }
    (void) ParsePacketBundle(bytes);
    return bytes;
}
}
