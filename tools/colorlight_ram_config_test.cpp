// SPDX-License-Identifier: GPL-3.0-or-later
// Offline only: these tests never include or open a hardware transport.
#include "colorlight_ram_config.h"

#include <cassert>
#include <functional>
#include <iostream>
#include <set>

using namespace colorlight_ram_config;

namespace
{
void Checksum(std::vector<uint8_t> &image)
{
    const uint32_t crc = BasicCrc(std::span(image).first(256));
    for (unsigned byte = 0; byte < 4; ++byte)
        image[252 + byte] = static_cast<uint8_t>(crc >> (8 * byte));
}

std::vector<uint8_t> Fixture()
{
    std::vector<uint8_t> image(ImageBytes, 0xff);
    std::fill_n(image.begin(), 256, 0);
    image[0] = 0xa8;
    image[4] = 16;
    image[5] = 64;
    image[6] = 8;
    image[7] = 16;
    image[8] = 14;
    image[0x10] = 0x24;
    image[0x1b] = 0x0a;
    image[0x88] = 2;
    image[0x8b] = 32;
    Checksum(image);
    std::fill_n(image.begin() + 0x0100, 0x400, 0x10);
    std::fill_n(image.begin() + 0x0500, 0x100, 0x05);
    std::fill_n(image.begin() + 0x0600, 0x300, 0x17);
    std::fill_n(image.begin() + 0x0900, 0x400, 0);
    constexpr std::array<size_t, 4> voidLine{0x1000, 0x1400, 0x6800, 0x6c00};
    for (size_t sub = 0; sub < voidLine.size(); ++sub)
        std::fill_n(image.begin() + voidLine[sub], 0x400, 0x40 + sub);
    constexpr std::array<size_t, 8> antiVoid{0x1800, 0x1c00, 0x2000, 0x2400, 0x7000, 0x7400, 0x7800, 0x7c00};
    for (size_t sub = 0; sub < antiVoid.size(); ++sub)
        std::fill_n(image.begin() + antiVoid[sub], 0x400, 0x50 + sub);
    for (size_t sub = 0; sub < 16; ++sub)
        std::fill_n(image.begin() + 0x3000 + sub * 0x300, 0x300, 0x60 + sub);
    std::fill_n(image.begin() + 0x6000, 0x400, 0x18);
    return image;
}

std::vector<Packet> SyntheticBundlePackets(const std::vector<uint8_t> &image)
{
    // Fresh synthetic payloads exercise frame preservation; no vendor packet
    // body or machine-specific receiver configuration is checked into source.
    auto Frame = [](uint8_t kind, size_t length, uint8_t sub = 0)
    {
        Packet packet(length, 0);
        std::copy(ProtocolAddresses.begin(), ProtocolAddresses.end(), packet.begin());
        packet[12] = kind;
        packet[15] = sub;
        for (size_t byte = 24; byte < packet.size(); ++byte)
            packet[byte] = static_cast<uint8_t>(byte * 31);
        return packet;
    };
    std::vector<Packet> packets;
    packets.push_back(Frame(5, 272, 2));
    auto basic = Frame(5, 272);
    std::copy_n(image.begin(), 256, basic.begin() + 16);
    packets.push_back(std::move(basic));
    packets.push_back(Frame(0x10, 1040));
    packets.push_back(Frame(3, 784));
    packets.push_back(Frame(0x1f, 1048));
    packets.push_back(Frame(0x32, 1048));
    packets.push_back(Frame(0x18, 1040));
    packets.push_back(Frame(0x18, 1040, 1));
    auto columns = Frame(0x41, 1066, 0xff);
    columns[16] = 0xff;
    packets.push_back(std::move(columns));
    auto routes = Frame(0x83, 788);
    routes[18] = 4;
    packets.push_back(std::move(routes));
    packets.push_back(BuildRamPackets(image).back());
    auto modules = Frame(0x17, 785, 0xff);
    modules[16] = 0xff;
    packets.push_back(std::move(modules));
    auto groups = Frame(0x37, 801, 2);
    groups[17] = 0xff;
    packets.push_back(std::move(groups));
    packets.push_back(Frame(0x76, 1171));
    packets.push_back(Frame(0x73, 1171, 1));
    packets.push_back(Frame(0x73, 1171, 2));
    return packets;
}

std::array<Packet, 4> ConvertedRoutePackets()
{
    // Independent synthetic bodies under the four reviewed normal-layout route
    // headers: two pixel fragments, one scan route, and one active-group route.
    std::array<Packet, 4> routes;
    constexpr std::array<uint8_t, 4> sub{0, 0, 1, 2};
    constexpr std::array<uint8_t, 4> fragment{0, 1, 0, 0};
    for (size_t index = 0; index < routes.size(); ++index)
    {
        auto &packet = routes[index];
        packet.resize(801, 0);
        std::copy(ProtocolAddresses.begin(), ProtocolAddresses.end(), packet.begin());
        packet[12] = 0x37;
        packet[15] = sub[index];
        packet[16] = fragment[index];
        packet[18] = 1; // Converted layout version 0x0001 at offsets 17/18.
        for (size_t byte = 19; byte < packet.size(); ++byte)
            packet[byte] = static_cast<uint8_t>(byte * 17 + index * 31);
    }
    return routes;
}

// Deliberately unchecked serializer, solely to construct malformed test files.
std::vector<uint8_t> UncheckedBundle(const std::vector<Packet> &packets)
{
    std::vector<uint8_t> bytes(BundleMagic.begin(), BundleMagic.end());
    bytes.push_back(static_cast<uint8_t>(packets.size()));
    bytes.push_back(static_cast<uint8_t>(packets.size() >> 8));
    for (const auto &packet : packets)
    {
        bytes.push_back(static_cast<uint8_t>(packet.size()));
        bytes.push_back(static_cast<uint8_t>(packet.size() >> 8));
        bytes.insert(bytes.end(), packet.begin(), packet.end());
    }
    return bytes;
}

void Rejects(const std::function<void()> &action)
{
    bool rejected = false;
    try { action(); } catch (const std::invalid_argument &) { rejected = true; }
    assert(rejected);
}

const Packet &Find(const std::vector<Packet> &packets, uint8_t kind, uint8_t sub)
{
    const auto found = std::find_if(packets.begin(), packets.end(), [=](const Packet &packet)
    { return packet[12] == kind && packet[15] == sub; });
    assert(found != packets.end());
    return *found;
}

void MatchBody(const std::vector<Packet> &packets, const std::vector<uint8_t> &image,
               uint8_t kind, uint8_t sub, size_t header, size_t offset, size_t length)
{
    const auto &packet = Find(packets, kind, sub);
    assert(packet.size() == 12 + header + length);
    assert(std::equal(image.begin() + offset, image.begin() + offset + length, packet.begin() + 12 + header));
}
}

int main()
{
    auto image = Fixture();
    const auto details = ValidateImage(image);
    assert(details.width == 512 && details.height == 32 && details.scan == 16);
    assert(details.gray_bits == 14 && details.chip_id == 0x0a && details.line_direction == 0);
    // Independent golden value from Python's zlib.crc32 with the specified
    // chip-ID bytes zeroed, rather than another implementation of this loop.
    assert(details.basic_crc == 0xd103804du);
    auto generic = image;
    generic[0x1b] = 0;
    assert(ValidateImage(generic).chip_id == 0); // Chip ID is excluded from CRC.

    const auto packets = BuildRamPackets(image);
    assert(packets.size() == 34);
    constexpr std::array<uint8_t, 12> addresses{
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x22, 0x22, 0x33, 0x44, 0x55, 0x66};
    Packet golden(272, 0x05);
    std::copy(addresses.begin(), addresses.end(), golden.begin());
    golden[12] = 5; golden[13] = golden[14] = 0; golden[15] = 2;
    assert(packets[0] == golden);
    assert(packets[1][12] == 5 && packets[1][15] == 0);
    MatchBody(packets, image, 5, 0, 4, 0, 0x100);
    MatchBody(packets, image, 0x10, 0, 4, 0x100, 0x400);
    MatchBody(packets, image, 0x17, 0xff, 5, 0x600, 0x300);
    for (uint8_t sub = 0; sub < 16; ++sub)
        MatchBody(packets, image, 3, sub, 4, 0x3000 + size_t(sub) * 0x300, 0x300);
    constexpr std::array<size_t, 4> voidLine{0x1000, 0x1400, 0x6800, 0x6c00};
    for (uint8_t sub = 0; sub < 4; ++sub)
        MatchBody(packets, image, 0x1f, sub, 8, voidLine[sub], 0x400);
    constexpr std::array<size_t, 8> antiVoid{0x1800, 0x1c00, 0x2000, 0x2400, 0x7000, 0x7400, 0x7800, 0x7c00};
    for (uint8_t sub = 0; sub < 8; ++sub)
        MatchBody(packets, image, 0x32, sub, 8, antiVoid[sub], 0x400);
    MatchBody(packets, image, 0x18, 0, 4, 0x6000, 0x400);
    const auto &area = packets.back();
    Packet expectedArea(1296, 0);
    std::copy(addresses.begin(), addresses.end(), expectedArea.begin());
    expectedArea[12] = 2;
    constexpr std::array<uint8_t, 10> rectangle{0, 0, 0, 0, 2, 0, 0, 32, 0, 0};
    for (size_t entry = 0; entry < 128; ++entry)
        std::copy(rectangle.begin(), rectangle.end(), expectedArea.begin() + 16 + entry * 10);
    assert(area == expectedArea); // Vendor single-card layout repeats every slot.

    auto extended = image;
    extended[0x6400] = 0x12;
    extended[0x0c00 + 7] = 3;
    const auto extraPackets = BuildRamPackets(extended);
    assert(extraPackets.size() == 36);
    MatchBody(extraPackets, extended, 0x18, 1, 4, 0x6400, 0x400);
    const auto exchange = std::find_if(extraPackets.begin(), extraPackets.end(),
                                     [](const Packet &packet) { return packet[12] == 0x86; });
    assert(exchange != extraPackets.end());
    Packet expectedExchange(280, 0);
    std::copy(addresses.begin(), addresses.end(), expectedExchange.begin());
    constexpr std::array<uint8_t, 12> header{
        0x86, 0, 0, 0xff, 0xff, 1, 0, 0, 0, 0, 1, 0};
    std::copy(header.begin(), header.end(), expectedExchange.begin() + 12);
    expectedExchange[24 + 7] = 3;
    assert(*exchange == expectedExchange);
    const std::set<uint8_t> allowed{2, 3, 5, 0x10, 0x17, 0x18, 0x1f, 0x32, 0x86};
    const std::set<uint8_t> forbidden{0x06, 0x19, 0x11, 0x21, 0x23};
    for (const auto &packet : extraPackets)
    {
        assert(allowed.contains(packet[12]) && !forbidden.contains(packet[12]));
        assert(packet[13] == 0 && packet.size() <= 1514);
        assert(std::equal(addresses.begin(), addresses.end(), packet.begin()));
        assert(!(packet[12] == 5 && packet[15] == 1)); // No chip-register pack.
    }

    Rejects([&] { ValidateImage(std::span(image).first(ImageBytes - 1)); });
    auto large = image; large.push_back(0);
    Rejects([&] { BuildRamPackets(large); });
    Rejects([&] { BasicCrc(std::span(image).first(255)); });
    auto Bad = [&](const std::function<void(std::vector<uint8_t> &)> &change, bool checksum = true)
    {
        auto invalid = image; change(invalid);
        if (checksum) Checksum(invalid);
        Rejects([&] { BuildRamPackets(invalid); });
    };
    Bad([](auto &b) { b[0] = 0; });
    Bad([](auto &b) { b[5] ^= 1; }, false);
    Bad([](auto &b) { b[0x88] = b[0x89] = 0; });
    Bad([](auto &b) { b[0x88] = b[0x89] = b[0x8a] = b[0x8b] = 255; });
    Bad([](auto &b) { b[7] = 0; });
    Bad([](auto &b) { b[7] = 129; });
    Bad([](auto &b) { b[8] = 0; });
    Bad([](auto &b) { b[8] = 17; });
    Bad([](auto &b) { b[0x1d] = 4; });
    Bad([](auto &b) { b[0x10] = 0x3f; });
    Bad([](auto &b) { b[0x1b] = 0xfe; b[0xe7] = 1; b[0xe8] = 0x4c; });
    Bad([](auto &b) { b[0x900] = 1; });
    Bad([](auto &b) { b[0xa00] = 1; });
    auto erasedRegisters = image;
    std::fill_n(erasedRegisters.begin() + 0x900, 0x100, 0xff);
    assert(BuildRamPackets(erasedRegisters).size() == 34);
    auto sdkShape = SyntheticBundlePackets(image);
    const auto encoded = EncodePacketBundle(sdkShape);
    const auto decoded = ParsePacketBundle(encoded);
    assert(decoded.details.width == 512 && decoded.details.height == 32);
    assert(decoded.packets == sdkShape && EncodePacketBundle(decoded.packets) == encoded);
    assert(encoded[8] == sdkShape.size() && encoded[9] == 0);
    assert(encoded[10] == 0x10 && encoded[11] == 1); // First length 272, LE.
    std::reverse(sdkShape.begin(), sdkShape.end());
    assert(ParsePacketBundle(EncodePacketBundle(sdkShape)).packets == sdkShape);
    std::reverse(sdkShape.begin(), sdkShape.end());

    const auto convertedRoutes = ConvertedRoutePackets();
    for (const auto &packet : convertedRoutes)
        ValidateBundleFrame(packet);
    auto convertedPackets = sdkShape;
    convertedPackets.erase(convertedPackets.begin() + 12); // Replace legacy group-clear route.
    convertedPackets.insert(convertedPackets.begin() + 12, convertedRoutes.begin(), convertedRoutes.end());
    const auto convertedDecoded = ParsePacketBundle(EncodePacketBundle(convertedPackets));
    assert(convertedDecoded.packets == convertedPackets);
    for (size_t route = 0; route < convertedRoutes.size(); ++route)
        assert(convertedDecoded.packets[12 + route] == convertedRoutes[route]);
    auto BadRoute = [&](size_t route, const std::function<void(Packet &)> &change)
    {
        auto packet = convertedRoutes[route];
        change(packet);
        Rejects([&] { ValidateBundleFrame(packet); });
        auto invalidBundle = convertedPackets;
        invalidBundle[12 + route] = packet;
        Rejects([&] { ParsePacketBundle(UncheckedBundle(invalidBundle)); });
    };
    BadRoute(0, [](auto &p) { p[15] = 3; });
    BadRoute(0, [](auto &p) { p[15] = 255; });
    BadRoute(0, [](auto &p) { p[16] = 2; }); // Pixel fragments only 0 and 1.
    BadRoute(2, [](auto &p) { p[16] = 1; }); // Scan routes have no second fragment.
    BadRoute(3, [](auto &p) { p[16] = 1; }); // Active-group routes have no second fragment.
    for (size_t route = 0; route < convertedRoutes.size(); ++route)
    {
        BadRoute(route, [](auto &p) { p[18] = 0; }); // Version 0 is not converted layout.
        BadRoute(route, [](auto &p) { p[18] = 2; }); // Unknown version 2.
        BadRoute(route, [](auto &p) { p[17] = 1; }); // Unknown version 0x0101.
        BadRoute(route, [](auto &p) { p.resize(800); });
    }
    for (size_t route : {size_t(0), size_t(1), size_t(2)})
        BadRoute(route, [](auto &p) { p[17] = 0xff; p[18] = 0; }); // Legacy only clears group sub 2.
    BadRoute(3, [](auto &p) { p[17] = 0xff; p[18] = 0xff; });
    // Keep the specifically reviewed legacy active-group clear form working.
    auto legacyGroup = convertedRoutes[3];
    legacyGroup[17] = 0xff; legacyGroup[18] = 0;
    ValidateBundleFrame(legacyGroup);

    auto genericGray12 = image;
    genericGray12[0x1b] = 0;
    genericGray12[8] = 12;
    Checksum(genericGray12);
    assert(ValidateImage(genericGray12).basic_crc == 0x9ea30835u); // Python zlib golden.
    auto genericPackets = SyntheticBundlePackets(genericGray12);
    genericPackets.erase(genericPackets.begin() + 12);
    genericPackets.insert(genericPackets.begin() + 12, convertedRoutes.begin(), convertedRoutes.end());
    const auto genericDecoded = ParsePacketBundle(EncodePacketBundle(genericPackets));
    assert(genericDecoded.details.chip_id == 0 && genericDecoded.details.gray_bits == 12);
    assert(genericDecoded.details.width == 512 && genericDecoded.details.height == 32);
    assert(genericDecoded.packets == genericPackets);

    // Runtime shadow records use opcode zero. Persistent EEPROM write/save
    // opcodes must fail before any transport can open, even at valid addresses.
    auto Shadow = [](uint8_t type, uint16_t address, uint8_t length)
    {
        Packet packet(282, 0);
        std::copy(ProtocolAddresses.begin(), ProtocolAddresses.end(), packet.begin());
        packet[12] = type; packet[15] = packet[16] = 0xff;
        packet[20] = address >> 8; packet[21] = address;
        packet[25] = length;
        std::fill_n(packet.begin() + 26, length, 0xa5);
        return packet;
    };
    for (const auto &shadow : {Shadow(0x1b, 0x51, 4), Shadow(0x2b, 0x100, 24)})
    {
        ValidateBundleFrame(shadow);
        auto withShadow = genericPackets;
        withShadow.push_back(shadow);
        assert(ParsePacketBundle(EncodePacketBundle(withShadow)).packets == withShadow);
        for (uint8_t opcode : {0x44, 0x85, 0x87, 0x88, 0xff})
        {
            auto bad = shadow; bad[17] = opcode;
            Rejects([&] { ValidateBundleFrame(bad); });
        }
        for (size_t offset : {size_t(15), size_t(16), size_t(18), size_t(21), size_t(25), size_t(281)})
        {
            auto bad = shadow; bad[offset] ^= 1;
            Rejects([&] { ValidateBundleFrame(bad); });
        }
        auto persistent = shadow; persistent[12] = 0x19;
        Rejects([&] { ValidateBundleFrame(persistent); });
        auto truncated = shadow; truncated.resize(281);
        Rejects([&] { ValidateBundleFrame(truncated); });
    }

    auto BadBundle = [&](const std::function<void(std::vector<Packet> &)> &change)
    {
        auto bad = sdkShape;
        change(bad);
        Rejects([&] { ParsePacketBundle(UncheckedBundle(bad)); });
    };
    for (uint8_t kind : {0x06, 0x19, 0x1b, 0x2b, 0x11, 0x21, 0x23, 0x1a, 0x86, 0xff})
        BadBundle([&](auto &p) { p[0][12] = kind; });
    BadBundle([](auto &p) { p[0][0] ^= 1; });
    BadBundle([](auto &p) { p[0][6] ^= 1; });
    BadBundle([](auto &p) { p[0][13] = 1; });
    BadBundle([](auto &p) { p[0][14] = 1; });
    BadBundle([](auto &p) { p[0][15] = 1; }); // Unreviewed register pack.
    BadBundle([](auto &p) { p[0].resize(59); });
    BadBundle([](auto &p) { p[0].resize(1515); });
    BadBundle([](auto &p) { p[1][16 + 5] ^= 1; }); // Basic CRC fails.
    BadBundle([](auto &p) { p.erase(p.begin() + 1); }); // Missing basic.
    BadBundle([](auto &p) { p.push_back(p[1]); }); // Duplicate basic.
    BadBundle([](auto &p) { p.push_back(p[10]); }); // Duplicate area.
    BadBundle([](auto &p) { p.erase(p.begin() + 10); }); // Missing area.
    BadBundle([](auto &p) { p.erase(p.begin() + 11); }); // Missing modules.
    BadBundle([](auto &p) { p[11][15] = 0; }); // Previous, incomplete module prefix.
    BadBundle([](auto &p) { p[4].resize(1044); }); // Previous table length.
    BadBundle([](auto &p) { p[8][16] = 0; });
    BadBundle([](auto &p) { p[9][18] = 0; });
    BadBundle([](auto &p) { p[12][17] = 0; });
    BadBundle([](auto &p) { p.erase(p.begin() + 14); }); // Incomplete gamma triplet.
    BadBundle([](auto &p) { p.push_back(p[13]); }); // Duplicate gamma LUT.
    BadBundle([](auto &p) { p[13][16] = 1; });
    BadBundle([](auto &p) { p[14][15] = 0; });
    auto withoutGamma = sdkShape;
    withoutGamma.resize(13);
    assert(ParsePacketBundle(EncodePacketBundle(withoutGamma)).packets == withoutGamma);
    auto trailing = encoded;
    trailing.push_back(0);
    Rejects([&] { ParsePacketBundle(trailing); });
    auto badMagic = encoded;
    badMagic[0] ^= 1;
    Rejects([&] { ParsePacketBundle(badMagic); });
    auto badCount = encoded;
    badCount[8] = 0; badCount[9] = 0;
    Rejects([&] { ParsePacketBundle(badCount); });
    badCount[8] = 129;
    Rejects([&] { ParsePacketBundle(badCount); });
    auto hugeBundle = encoded;
    hugeBundle.resize(MaximumBundleBytes + 1);
    Rejects([&] { ParsePacketBundle(hugeBundle); });
    for (size_t length : {size_t(0), size_t(1), size_t(7), size_t(8), size_t(9), size_t(10), size_t(11),
                          size_t(12), size_t(12 + 271), encoded.size() - 1})
        Rejects([&] { ParsePacketBundle(std::span(encoded).first(length)); });
    std::cout << "RAM-only parameter, strict SDK bundle, CRC and rejection tests passed\n";
}
