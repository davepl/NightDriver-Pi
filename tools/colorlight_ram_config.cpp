// SPDX-License-Identifier: GPL-3.0-or-later
// Separate commissioning utility. Normal ndpi playback never invokes this tool.
#include "colorlight_ram_config.h"
#include "colorlight.h"
#include "outputconfig.h"

#include <chrono>
#include <charconv>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <thread>

namespace
{
struct Options
{
    std::string image;
    std::string packets;
    std::string interface;
    std::string config;
    unsigned packet_gap_ms = 5;
    bool apply = false;
    bool area_only = false;
    bool help = false;
};

void Usage()
{
    std::cerr << "Usage: colorlight-ram-config --packets audited-runtime.bundle\n"
                 "       colorlight-ram-config --packets BUNDLE --interface en0 --apply-ram\n"
                 "       colorlight-ram-config --packets BUNDLE --config wall.json --apply-ram\n"
                 "       colorlight-ram-config --image IMAGE [--area-only]\n"
                 "Default: offline validation and a dry run; no network device is opened.\n"
                 "--image and --packets are mutually exclusive. Full --image application is refused: \n"
                 "images lack runtime routing tables; use an audited packet bundle.\n"
                 "Image application is restricted to --area-only (the volatile card window).\n"
                 "Bundles may include exactly all three reviewed volatile gamma LUTs, or none.\n"
                 "Reviewed opcode-zero runtime shadow records are allowed; persistent record writes are rejected.\n"
                 "--config reads the playback interface; it cannot be combined with --interface.\n"
                 "--packet-gap-ms 1..1000 controls spacing between RAM frames (default 5).\n"
                 "No flash, EEPROM persistence, save, reload, or firmware operation exists.\n";
}

Options Parse(int argc, char **argv)
{
    Options options;
    bool imageSeen = false, packetsSeen = false, interfaceSeen = false, configSeen = false, gapSeen = false;
    for (int index = 1; index < argc; ++index)
    {
        const std::string argument = argv[index];
        auto Value = [&](const std::string &name)
        {
            if (argument == name)
            {
                if (++index == argc || std::string(argv[index]).starts_with("--"))
                    throw std::invalid_argument(name + " requires a value");
                return std::string(argv[index]);
            }
            return argument.substr(name.size() + 1);
        };
        if (argument == "--help" || argument == "-h") options.help = true;
        else if (argument == "--area-only")
        {
            if (options.area_only) throw std::invalid_argument("Duplicate --area-only");
            options.area_only = true;
        }
        else if (argument == "--apply-ram")
        {
            if (options.apply) throw std::invalid_argument("Duplicate --apply-ram");
            options.apply = true;
        }
        else if (argument == "--image" || argument.starts_with("--image="))
        {
            if (imageSeen) throw std::invalid_argument("Duplicate --image");
            imageSeen = true;
            options.image = Value("--image");
        }
        else if (argument == "--packets" || argument.starts_with("--packets="))
        {
            if (packetsSeen) throw std::invalid_argument("Duplicate --packets");
            packetsSeen = true;
            options.packets = Value("--packets");
        }
        else if (argument == "--interface" || argument.starts_with("--interface="))
        {
            if (interfaceSeen) throw std::invalid_argument("Duplicate --interface");
            interfaceSeen = true;
            options.interface = Value("--interface");
        }
        else if (argument == "--config" || argument.starts_with("--config="))
        {
            if (configSeen) throw std::invalid_argument("Duplicate --config");
            configSeen = true;
            options.config = Value("--config");
        }
        else if (argument == "--packet-gap-ms" || argument.starts_with("--packet-gap-ms="))
        {
            if (gapSeen) throw std::invalid_argument("Duplicate --packet-gap-ms");
            gapSeen = true;
            const auto value = Value("--packet-gap-ms");
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), options.packet_gap_ms);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
                options.packet_gap_ms < 1 || options.packet_gap_ms > 1000)
                throw std::invalid_argument("--packet-gap-ms must be an integer from 1 to 1000");
        }
        else throw std::invalid_argument("Unknown option: " + argument);
    }
    if (!options.help)
    {
        if (imageSeen == packetsSeen)
            throw std::invalid_argument("Supply exactly one of --image or --packets; they are mutually exclusive");
        if ((imageSeen && options.image.empty()) || (packetsSeen && options.packets.empty()))
            throw std::invalid_argument("The selected input path must be nonempty");
        if (configSeen && interfaceSeen)
            throw std::invalid_argument("--config and --interface are mutually exclusive");
        if (configSeen && options.config.empty())
            throw std::invalid_argument("--config requires a nonempty path");
        if (options.apply && !interfaceSeen && !configSeen)
            throw std::invalid_argument("--apply-ram requires --config or an explicit --interface");
        if (interfaceSeen && (options.interface.empty() || options.interface.size() > 15 ||
            options.interface.find_first_of("/ :\t\n\r") != std::string::npos))
            throw std::invalid_argument("--interface must name an Ethernet interface of 1..15 characters");
    }
    return options;
}

std::vector<uint8_t> ReadInput(const std::string &path, size_t maximum)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot open input '" + path + "'");
    std::vector<uint8_t> image(maximum + 1);
    input.read(reinterpret_cast<char *>(image.data()), image.size());
    if (input.bad()) throw std::runtime_error("Cannot read input '" + path + "'");
    image.resize(static_cast<size_t>(input.gcount()));
    return image;
}

void Describe(const colorlight_ram_config::ImageDetails &details,
              const std::vector<colorlight_ram_config::Packet> &packets, bool applied, bool bundle,
              bool areaOnly, unsigned gapMs)
{
    std::map<unsigned, size_t> counts;
    size_t bytes = 0;
    for (const auto &packet : packets) { ++counts[packet[12]]; bytes += packet.size(); }
    std::cout << "mode=" << (applied ? "sent-ram" : "dry-run") << " receiver_index=0 geometry="
              << details.width << 'x' << details.height << " scan=" << unsigned(details.scan)
              << " gray_bits=" << unsigned(details.gray_bits) << " chip_id=0x" << std::hex
              << details.chip_id << " basic_crc=0x" << details.basic_crc << std::dec
              << " packets=" << packets.size() << " bytes=" << bytes << " gap_ms=" << gapMs << '\n';
    std::cout << "source=" << (bundle ? "audited-packets" : "image-diagnostic")
              << " area_only=" << (areaOnly ? "true" : "false") << '\n';
    if (!bundle && !areaOnly)
        std::cout << "runtime_routing=incomplete full_image_application=refused\n";
    for (const auto &[kind, count] : counts)
        std::cout << "type=0x" << std::hex << std::setw(2) << std::setfill('0') << kind
                  << std::dec << " count=" << count << '\n';
    if (counts.contains(0x86))
        std::cout << "type=0x86 capability_conditional=true receiver_acceptance=unverified\n";
}
}

int main(int argc, char **argv)
{
    try
    {
        const auto options = Parse(argc, argv);
        if (options.help) { Usage(); return 0; }
        const auto interface = options.config.empty() ? options.interface :
            LoadColorlightConfig(options.config).interface;
        const bool fromBundle = !options.packets.empty();
        if (options.apply && !fromBundle && !options.area_only)
            throw std::invalid_argument("image lacks runtime routing tables; use audited --packets bundle");
        colorlight_ram_config::ImageDetails details{};
        std::vector<colorlight_ram_config::Packet> packets;
        if (fromBundle)
        {
            auto bundle = colorlight_ram_config::ParsePacketBundle(
                ReadInput(options.packets, colorlight_ram_config::MaximumBundleBytes));
            details = bundle.details;
            packets = std::move(bundle.packets);
        }
        else
        {
            const auto image = ReadInput(options.image, colorlight_ram_config::ImageBytes);
            details = colorlight_ram_config::ValidateImage(image);
            packets = colorlight_ram_config::BuildRamPackets(image);
        }
        if (options.area_only)
        {
            const auto area = std::find_if(packets.begin(), packets.end(),
                [](const auto &packet) { return packet[12] == 0x02; });
            if (area == packets.end()) throw std::invalid_argument("Input has no volatile card-area packet");
            packets = {*area};
        }
        if (!options.apply) { Describe(details, packets, false, fromBundle, options.area_only, options.packet_gap_ms); return 0; }
        auto transport = CreateColorlightTransport(interface);
        for (const auto &packet : packets)
        {
            transport->Send(packet);
            std::this_thread::sleep_for(std::chrono::milliseconds(options.packet_gap_ms));
        }
        transport->FinishRows(std::chrono::microseconds(0));
        Describe(details, packets, true, fromBundle, options.area_only, options.packet_gap_ms);
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "colorlight-ram-config: " << error.what() << '\n';
        return 1;
    }
}
