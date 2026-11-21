// SPDX-License-Identifier: GPL-3.0-or-later
// Independently implemented from Colorlight packet-format observations in FPP:
// https://github.com/FalconChristmas/fpp/blob/88304ed82865f892fc70df1afe4e6e2efd55ec4a/src/channeloutput/ColorLight-5a-75.cpp
// No FPP source code is incorporated (that reference file is GPL-v2-only).
#include "colorlight.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <thread>

#ifdef __APPLE__
#include "macethernet.h"
#endif

#ifdef __linux__
#include <arpa/inet.h>
#include <fstream>
#include <linux/if_packet.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace
{
constexpr size_t kDataOffset = 13;
constexpr size_t kPixelOffset = kDataOffset + 8;
constexpr std::array<uint8_t, 12> kAddresses = {
    0x11, 0x22, 0x33, 0x44, 0x55, 0x66,
    0x22, 0x22, 0x33, 0x44, 0x55, 0x66
};

EthernetPacket Packet(uint8_t type, size_t length)
{
    // Minimum Ethernet length excluding FCS. Padding has no pixel content.
    EthernetPacket result(std::max<size_t>(60, length), 0);
    std::copy(kAddresses.begin(), kAddresses.end(), result.begin());
    result[12] = type;
    return result;
}

void Store16(EthernetPacket & packet, size_t offset, size_t value)
{
    packet[offset] = static_cast<uint8_t>(value >> 8);
    packet[offset + 1] = static_cast<uint8_t>(value);
}

#ifdef __linux__
class RawEthernetTransport final : public EthernetTransport
{
    int _fd = -1;
    int _linkSpeed = 0;
    std::string _interface;
    sockaddr_ll _address{};
    std::chrono::steady_clock::time_point _wireReady{};

    [[noreturn]] void Fail(const std::string & operation, int error) const
    {
        std::string message = "Colorlight " + operation + " on interface '" + _interface + "': " + std::strerror(error);
        if (error == EPERM || error == EACCES)
            message += "; run as root or grant the executable CAP_NET_RAW (see docs/colorlight.md)";
        throw std::runtime_error(message);
    }

  public:
    explicit RawEthernetTransport(const std::string & interface) : _interface(interface)
    {
        if (interface.empty() || interface.size() >= IFNAMSIZ)
            throw std::runtime_error("Colorlight interface name must contain 1..15 characters");
        const auto index = if_nametoindex(interface.c_str());
        if (index == 0)
            Fail("finding interface", errno == 0 ? ENODEV : errno);

        // Protocol zero creates a send-only packet socket; there is no IP layer.
        _fd = socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC, 0);
        if (_fd < 0)
            Fail("opening raw Ethernet socket", errno);

        try
        {
            ifreq request{};
            std::memcpy(request.ifr_name, interface.c_str(), interface.size() + 1);
            if (ioctl(_fd, SIOCGIFHWADDR, &request) < 0)
                Fail("checking interface type", errno);
            if (request.ifr_hwaddr.sa_family != ARPHRD_ETHER)
                throw std::runtime_error("Colorlight requires an Ethernet interface: " + interface);
            if (ioctl(_fd, SIOCGIFFLAGS, &request) < 0)
                Fail("checking interface flags", errno);
            if ((request.ifr_flags & (IFF_UP | IFF_RUNNING)) != (IFF_UP | IFF_RUNNING))
                throw std::runtime_error("Colorlight interface '" + interface + "' is down or has no active link");
            if (ioctl(_fd, SIOCGIFMTU, &request) < 0)
                Fail("checking interface MTU", errno);
            if (request.ifr_mtu < 1500)
                throw std::runtime_error("Colorlight requires interface MTU >= 1500: " + interface);

            std::ifstream speed("/sys/class/net/" + interface + "/speed");
            if (!(speed >> _linkSpeed) || _linkSpeed < 1000)
                throw std::runtime_error("Colorlight requires a verified active gigabit Ethernet link (>=1000 Mb/s) on '" + interface + "'; check cabling and /sys/class/net/" + interface + "/speed");

            _address.sll_family = AF_PACKET;
            _address.sll_ifindex = static_cast<int>(index);
            _address.sll_halen = 6;
            std::copy_n(kAddresses.begin(), 6, _address.sll_addr);
            if (bind(_fd, reinterpret_cast<const sockaddr *>(&_address), sizeof(_address)) < 0)
                Fail("binding raw Ethernet socket", errno);
        }
        catch (...)
        {
            close(_fd);
            _fd = -1;
            throw;
        }
    }

    ~RawEthernetTransport() override
    {
        if (_fd >= 0)
            close(_fd);
    }

    void Send(std::span<const uint8_t> packet) override
    {
        using namespace std::chrono;
        const auto deadline = steady_clock::now() + milliseconds(100);
        for (;;)
        {
            const auto result = sendto(_fd, packet.data(), packet.size(), MSG_DONTWAIT | MSG_NOSIGNAL,
                                       reinterpret_cast<const sockaddr *>(&_address), sizeof(_address));
            if (result >= 0)
            {
                if (static_cast<size_t>(result) != packet.size())
                    throw std::runtime_error("Colorlight short Ethernet send on '" + _interface + "'");
                // Preamble, FCS and inter-packet gap add 24 bytes on the wire.
                // A projected completion time also accounts for sync packets
                // and idle intervals between frames; a batch start timestamp
                // would become stale after the previous frame's sync.
                const auto wireTime = nanoseconds(((packet.size() + 24) * 8000 + _linkSpeed - 1) / _linkSpeed);
                _wireReady = std::max(steady_clock::now(), _wireReady) + wireTime;
                return;
            }
            const int error = errno;
            if (error == EINTR && steady_clock::now() < deadline)
                continue;
            if ((error == EAGAIN || error == EWOULDBLOCK || error == ENOBUFS) && steady_clock::now() < deadline)
            {
                std::this_thread::sleep_for(microseconds(50));
                continue;
            }
            Fail("sending Ethernet frame", error);
        }
    }

    void FinishRows(std::chrono::microseconds guard) override
    {
        using namespace std::chrono;
        ifreq request{};
        std::memcpy(request.ifr_name, _interface.c_str(), _interface.size() + 1);
        if (ioctl(_fd, SIOCGIFFLAGS, &request) < 0)
            Fail("checking link before display/sync", errno);
        if ((request.ifr_flags & (IFF_UP | IFF_RUNNING)) != (IFF_UP | IFF_RUNNING))
            throw std::runtime_error("Colorlight link lost on '" + _interface + "'; frame was not latched");
        const auto deadline = steady_clock::now() + milliseconds(100);
        int queued = 0;
        // Do not latch while rows remain in the kernel's transmit queue.
        while (ioctl(_fd, SIOCOUTQ, &queued) == 0 && queued > 0)
        {
            if (steady_clock::now() >= deadline)
                throw std::runtime_error("Colorlight transmit queue did not drain on '" + _interface + "'; frame was not latched");
            std::this_thread::sleep_for(microseconds(50));
        }
        // Some NICs hand buffers to hardware before reporting queue length zero,
        // and some packet sockets do not support SIOCOUTQ. Allow wire time too.
        std::this_thread::sleep_until(std::max(steady_clock::now(), _wireReady) + guard);
    }
};
#endif
}

ColorlightFramePackets EncodeColorlightFrame(const ColorlightConfig & config,
                                            std::span<const CRGB> frame)
{
    ValidateColorlightConfig(config);
    const auto width = static_cast<size_t>(config.width);
    const auto height = static_cast<size_t>(config.height);
    const auto pixelsPerPacket = static_cast<size_t>(config.max_pixels_per_packet);
    if (frame.size() != width * height)
        throw std::runtime_error("Colorlight frame size does not match configured width * height");

    ColorlightFramePackets result;
    const auto copies = config.firmware_profile == "v13" ? 2u : 1u;
    const unsigned pixelCopies = config.duplicate_pixel_packets ? 2u : 1u;
    result.data.reserve(copies + pixelCopies * height * ((width + pixelsPerPacket - 1) / pixelsPerPacket));
    auto brightness = Packet(0x0a, 77);
    brightness[13] = brightness[14] = brightness[15] = static_cast<uint8_t>(config.brightness);
    brightness[16] = 0xff;
    for (unsigned int copy = 0; copy < copies; ++copy)
        result.data.push_back(brightness);

    for (size_t y = 0; y < height; ++y)
    {
        for (size_t x = 0; x < width; x += pixelsPerPacket)
        {
            const auto count = std::min(pixelsPerPacket, width - x);
            auto packet = Packet(0x55, kPixelOffset + 3 * count);
            Store16(packet, 13, y);
            Store16(packet, 15, x);
            Store16(packet, 17, count);
            packet[19] = 0x08;
            packet[20] = 0x88;
            const auto sourceY = config.flip_y ? height - 1 - y : y;
            for (size_t pixel = 0; pixel < count; ++pixel)
            {
                const auto sourceX = config.flip_x ? width - 1 - (x + pixel) : x + pixel;
                const auto & color = frame[sourceY * width + sourceX];
                const auto destination = kPixelOffset + pixel * 3;
                const bool bgr = config.color_order == "BGR";
                packet[destination] = bgr ? color.b : color.r;
                packet[destination + 1] = color.g;
                packet[destination + 2] = bgr ? color.r : color.b;
            }
            if (config.duplicate_pixel_packets)
                result.data.push_back(packet);
            result.data.push_back(std::move(packet));
        }
    }

    auto sync = Packet(0x01, 112);
    sync[13] = 0x07;
    sync[13 + 22] = sync[13 + 25] = sync[13 + 26] = sync[13 + 27] = static_cast<uint8_t>(config.brightness);
    sync[13 + 23] = 0x05;
    for (int copy = 0; copy < config.sync_packet_count.value_or(copies); ++copy)
        result.sync.push_back(sync);
    return result;
}

std::unique_ptr<EthernetTransport> CreateColorlightTransport(const std::string & interface)
{
#ifdef __linux__
    return std::make_unique<RawEthernetTransport>(interface);
#elif defined(__APPLE__)
    return CreateMacColorlightTransport(interface);
#else
    (void) interface;
    throw std::runtime_error("Colorlight raw Ethernet output requires Linux or macOS");
#endif
}

ColorlightOutput::ColorlightOutput(const ColorlightConfig & config,
                                   std::unique_ptr<EthernetTransport> transport)
    : _config(config), _transport(std::move(transport))
{
    ValidateColorlightConfig(_config);
    if (!_transport)
        _transport = CreateColorlightTransport(_config.interface);
}

void ColorlightOutput::TransmitRows(const ColorlightFramePackets & packets)
{
    for (const auto & packet : packets.data)
        _transport->Send(packet);
    _transport->FinishRows(std::chrono::microseconds(_config.sync_gap_us));
}

void ColorlightOutput::Latch(const ColorlightFramePackets & packets)
{
    for (const auto & packet : packets.sync)
        _transport->Send(packet);
}

void ColorlightOutput::Transmit(const ColorlightFramePackets & packets)
{
    TransmitRows(packets);
    Latch(packets);
}

std::chrono::nanoseconds ColorlightOutput::RefreshPeriod() const
{
    return std::chrono::nanoseconds(1000000000 / _config.refresh_hz);
}

std::chrono::microseconds ColorlightOutput::PreparationLeadTime() const
{
    return std::chrono::microseconds(_config.prepare_ahead_us);
}

void ColorlightOutput::PresentAt(std::span<const CRGB> frame, DisplayTiming::Clock::time_point deadline)
{
    if (!_config.prepare_ahead_us)
    {
        DisplayTiming::WaitUntil(deadline);
        Present(frame);
        return;
    }
    auto packets = EncodeColorlightFrame(_config, frame);
    TransmitRows(packets);
    DisplayTiming::WaitUntil(deadline, true);
    Latch(packets);
    _lastFrame = std::move(packets);
}

void ColorlightOutput::RefreshAt(DisplayTiming::Clock::time_point deadline)
{
    if (!_config.prepare_ahead_us)
    {
        DisplayTiming::WaitUntil(deadline);
        Refresh();
        return;
    }
    if (_lastFrame.data.empty()) return;
    TransmitRows(_lastFrame);
    DisplayTiming::WaitUntil(deadline, true);
    Latch(_lastFrame);
}

void ColorlightOutput::Present(std::span<const CRGB> frame)
{
    auto packets = EncodeColorlightFrame(_config, frame);
    Transmit(packets);
    _lastFrame = std::move(packets);
}

std::chrono::microseconds ColorlightOutput::RefreshInterval() const
{
    return std::chrono::microseconds(1000000 / _config.refresh_hz);
}

void ColorlightOutput::Refresh()
{
    if (!_lastFrame.data.empty())
        Transmit(_lastFrame);
}

void ColorlightOutput::ConfigureDisplayThread()
{
#ifdef __APPLE__
    ConfigureMacColorlightDisplayThread(RefreshInterval(), _lastFrame.data.size() + _lastFrame.sync.size());
#endif
}
