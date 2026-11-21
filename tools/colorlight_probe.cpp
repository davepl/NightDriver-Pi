// SPDX-License-Identifier: GPL-3.0-or-later
// Read-only Colorlight discovery / one 1024-byte flash READ. See the pinned
// protocol provenance in colorlight_probe.h. This tool never configures cards.
#include "colorlight_probe.h"
#include "macethernet.h"

#include <charconv>
#include <chrono>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#ifdef __APPLE__
#include <net/bpf.h>
#include <pcap/pcap.h>
#include <sys/ioctl.h>
#endif

namespace
{
struct Options
{
    std::string interface;
    std::optional<uint16_t> page;
    std::string out;
    bool help = false;
};

void Usage()
{
    std::cerr << "Usage: colorlight-probe --interface en0\n"
                 "       colorlight-probe --interface en0 --read-page 0x0700 --out page.bin\n"
                 "Discovery or one read-only 1024-byte flash request, receiver index 0.\n"
                 "Reply timeout is 1 second; there are no writes, reloads, or automatic retries.\n";
}

Options Parse(int argc, char **argv)
{
    Options result;
    bool interfaceSeen = false, pageSeen = false, outSeen = false;
    for (int index = 1; index < argc; ++index)
    {
        const std::string arg = argv[index];
        if (arg == "--help" || arg == "-h") { result.help = true; continue; }
        auto Value = [&](const std::string &name)
        {
            if (arg == name)
            {
                if (++index == argc || std::string(argv[index]).starts_with("--"))
                    throw std::invalid_argument(name + " requires a value");
                return std::string(argv[index]);
            }
            return arg.substr(name.size() + 1);
        };
        if (arg == "--interface" || arg.starts_with("--interface="))
        {
            if (interfaceSeen) throw std::invalid_argument("Duplicate --interface");
            interfaceSeen = true;
            result.interface = Value("--interface");
        }
        else if (arg == "--read-page" || arg.starts_with("--read-page="))
        {
            if (pageSeen) throw std::invalid_argument("Duplicate --read-page");
            pageSeen = true;
            auto value = Value("--read-page");
            int base = 10;
            if (value.starts_with("0x") || value.starts_with("0X")) { value.erase(0, 2); base = 16; }
            unsigned int page = 0;
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), page, base);
            if (error != std::errc{} || end != value.data() + value.size() || page > 65535)
                throw std::invalid_argument("--read-page must be 0..65535 or 0x0000..0xffff");
            result.page = static_cast<uint16_t>(page);
        }
        else if (arg == "--out" || arg.starts_with("--out="))
        {
            if (outSeen) throw std::invalid_argument("Duplicate --out");
            outSeen = true;
            result.out = Value("--out");
        }
        else throw std::invalid_argument("Unknown option: " + arg);
    }
    if (!result.help)
    {
        if (result.interface.empty() || result.interface.size() > 15)
            throw std::invalid_argument("--interface requires an Ethernet interface name of 1..15 characters");
        if (pageSeen != outSeen || (outSeen && result.out.empty()))
            throw std::invalid_argument("--read-page and a nonempty --out must be supplied together");
    }
    return result;
}

std::string Json(const std::string &value)
{
    constexpr char hex[] = "0123456789abcdef";
    std::string result = "\"";
    for (unsigned char c : value)
    {
        if (c == '\\' || c == '"') { result += '\\'; result += c; }
        else if (c < 0x20) { result += "\\u00"; result += hex[c >> 4]; result += hex[c & 15]; }
        else result += c;
    }
    return result + '"';
}

#ifdef __APPLE__
class Capture
{
    std::unique_ptr<pcap_t, decltype(&pcap_close)> _capture{nullptr, pcap_close};

  public:
    explicit Capture(const std::string &interface)
    {
        char error[PCAP_ERRBUF_SIZE]{};
        // Promiscuous capture is needed because receiver replies may target a
        // broadcast or the protocol's synthetic sender MAC rather than the NIC.
        _capture.reset(pcap_open_live(interface.c_str(), 65536, 1, 1, error));
        if (!_capture)
            throw std::runtime_error(std::string("Opening reply capture: ") + error +
                                     "; use sudo or an account with /dev/bpf* access");
        if (pcap_datalink(_capture.get()) != DLT_EN10MB)
            throw std::runtime_error("Reply capture requires an Ethernet interface");
        if (pcap_setnonblock(_capture.get(), 1, error) != 0)
            throw std::runtime_error(std::string("Setting nonblocking capture: ") + error);
        unsigned int immediate = 1;
        if (ioctl(pcap_fileno(_capture.get()), BIOCIMMEDIATE, &immediate) < 0)
            throw std::runtime_error("Cannot enable immediate BPF reply capture");
        bpf_program filter{};
        if (pcap_compile(_capture.get(), &filter,
                         "ether src 11:22:33:44:55:66 and (ether[12:2] = 0x0805 or ether[12:2] = 0x0901)",
                         1, PCAP_NETMASK_UNKNOWN) < 0)
            throw std::runtime_error(pcap_geterr(_capture.get()));
        const int installed = pcap_setfilter(_capture.get(), &filter);
        pcap_freecode(&filter);
        if (installed < 0) throw std::runtime_error(pcap_geterr(_capture.get()));
    }

    void Drain()
    {
        // A new capture descriptor plus its source/type filter exclude our own
        // outgoing requests. Flush any kernel packets and userspace leftovers
        // before the single request, since read replies carry no page echo.
        for (int count = 0; count < 4096; ++count)
        {
            pcap_pkthdr *header = nullptr;
            const u_char *packet = nullptr;
            const int result = pcap_next_ex(_capture.get(), &header, &packet);
            if (result == 0) break;
            if (result < 0) throw std::runtime_error(pcap_geterr(_capture.get()));
            if (count == 4095) throw std::runtime_error("Reply capture is not quiet; stop other receiver controllers");
        }
        if (ioctl(pcap_fileno(_capture.get()), BIOCFLUSH, nullptr) < 0)
            throw std::runtime_error("Cannot flush stale BPF replies");
    }

    std::vector<uint8_t> Await(bool flash)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (std::chrono::steady_clock::now() < deadline)
        {
            pcap_pkthdr *header = nullptr;
            const u_char *packet = nullptr;
            const int result = pcap_next_ex(_capture.get(), &header, &packet);
            if (result < 0) throw std::runtime_error(pcap_geterr(_capture.get()));
            if (result == 1 && header->caplen == header->len)
            {
                const std::span<const uint8_t> frame(packet, header->caplen);
                if (flash ? colorlight_probe::FlashReply(frame) : colorlight_probe::DiscoveryReply(frame))
                    return {frame.begin(), frame.end()};
            }
            if (result == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        throw std::runtime_error(flash ? "No complete 0x0901 flash-read reply within 1 second" :
                                        "No complete 0x0805 discovery reply within 1 second");
    }
};
#endif
}

int main(int argc, char **argv)
{
    try
    {
        const auto options = Parse(argc, argv);
        if (options.help) { Usage(); return 0; }
#ifdef __APPLE__
        Capture capture(options.interface);
        auto transport = CreateMacColorlightTransport(options.interface);
        capture.Drain();
        const auto request = options.page ? colorlight_probe::FlashReadRequest(*options.page) :
                                            colorlight_probe::DiscoveryRequest();
        // These two builders are the complete transmit surface of this tool.
        transport->Send(request);
        const auto reply = capture.Await(options.page.has_value());
        if (options.page)
        {
            const auto data = colorlight_probe::FlashData(reply);
            std::ofstream output(options.out, std::ios::binary | std::ios::trunc);
            output.write(reinterpret_cast<const char *>(data.data()), data.size());
            output.close();
            if (!output) throw std::runtime_error("Cannot save flash data to '" + options.out + "'");
            // The audited source does not define the meaning of byte 14.
            // Report its raw value without making an unsupported success claim.
            std::cout << "{\"operation\":\"read-page\",\"page\":" << *options.page
                      << ",\"receiver_index\":0,\"status_byte_14\":" << unsigned(reply[14])
                      << ",\"reply_bytes\":" << reply.size() << ",\"saved_bytes\":1024,\"path\":" << Json(options.out) << "}\n";
        }
        else
        {
            std::cout << "{\"operation\":\"discovery\",\"reply_bytes\":" << reply.size()
                      << ",\"family_byte_13\":" << unsigned(reply[13])
                      << ",\"card_type_byte_14\":" << unsigned(reply[14])
                      << ",\"firmware_major_byte_15\":" << unsigned(reply[15])
                      << ",\"firmware_minor_byte_16\":" << unsigned(reply[16])
                      << ",\"receiver_index_byte_98\":" << unsigned(reply[98]) << "}\n";
        }
        return 0;
#else
        throw std::runtime_error("colorlight-probe currently requires macOS BPF and libpcap");
#endif
    }
    catch (const std::exception &error)
    {
        std::cerr << "colorlight-probe: " << error.what() << '\n';
        return 1;
    }
}
