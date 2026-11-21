// SPDX-License-Identifier: GPL-3.0-or-later
#include "colorlight.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>

namespace
{
void Check(bool condition, const char * message)
{
    if (!condition)
        throw std::runtime_error(message);
}

template<typename Action>
void Rejects(Action action, const char * message)
{
    try { action(); }
    catch (const std::exception &) { return; }
    throw std::runtime_error(message);
}

ColorlightConfig Config(int width, int height)
{
    ColorlightConfig config;
    config.width = width;
    config.height = height;
    config.interface = "eth0";
    config.receivers.push_back({0, 0, 0, width, height});
    config.firmware_profile = "legacy";
    return config;
}

class CaptureTransport final : public EthernetTransport
{
  public:
    std::vector<EthernetPacket> packets;
    std::vector<int> events;
    std::vector<DisplayTiming::Clock::time_point> sent_at;
    size_t fail_after = SIZE_MAX;
    bool fail_drain = false;

    void Send(std::span<const uint8_t> packet) override
    {
        if (packets.size() == fail_after)
            throw std::runtime_error("injected send failure");
        sent_at.push_back(DisplayTiming::Clock::now());
        packets.emplace_back(packet.begin(), packet.end());
        events.push_back(packet[12]);
    }

    void FinishRows(std::chrono::microseconds guard) override
    {
        Check(guard.count() == 500, "configured row guard passed to transport");
        events.push_back(-1);
        if (fail_drain)
            throw std::runtime_error("injected drain failure");
    }
};

void GoldenPackets()
{
    const auto config = Config(2, 2);
    const std::array<CRGB, 4> colors = {CRGB(255, 0, 0), CRGB(0, 255, 0), CRGB(0, 0, 255), CRGB(1, 2, 3)};
    const auto packets = EncodeColorlightFrame(config, colors);
    Check(packets.data.size() == 3 && packets.sync.size() == 1, "legacy packet counts");

    // Golden complete first row, including Ethernet minimum-length padding.
    EthernetPacket expected = {
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66,
        0x22, 0x22, 0x33, 0x44, 0x55, 0x66,
        0x55, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x08, 0x88,
        0xff, 0x00, 0x00, 0x00, 0xff, 0x00
    };
    expected.resize(60, 0);
    Check(packets.data[1] == expected, "golden RGB first row Ethernet bytes");
    expected[14] = 1;
    expected[21] = 0; expected[22] = 0; expected[23] = 255;
    expected[24] = 1; expected[25] = 2; expected[26] = 3;
    Check(packets.data[2] == expected, "second logical row is transmitted in order");

    auto expectedBrightness = EthernetPacket(77, 0);
    std::copy_n(expected.begin(), 12, expectedBrightness.begin());
    expectedBrightness[12] = 0x0a;
    expectedBrightness[13] = expectedBrightness[14] = expectedBrightness[15] = expectedBrightness[16] = 255;
    Check(packets.data[0] == expectedBrightness, "golden brightness packet");
    auto expectedSync = EthernetPacket(112, 0);
    std::copy_n(expected.begin(), 12, expectedSync.begin());
    expectedSync[12] = 0x01; expectedSync[13] = 0x07;
    expectedSync[35] = 255; expectedSync[36] = 0x05;
    expectedSync[38] = expectedSync[39] = expectedSync[40] = 255;
    Check(packets.sync[0] == expectedSync, "golden display/sync packet");
}

void SplittingAndCoordinates()
{
    const auto config = Config(1000, 2);
    std::vector<CRGB> colors(2000);
    for (size_t i = 0; i < colors.size(); ++i)
        colors[i] = CRGB(i % 251, i % 239, i % 233);
    const auto packets = EncodeColorlightFrame(config, colors);
    Check(packets.data.size() == 7, "1000 pixel rows split into three packets");
    for (size_t row = 0; row < 2; ++row)
    {
        for (size_t part = 0; part < 3; ++part)
        {
            const auto & packet = packets.data[1 + row * 3 + part];
            const auto offset = part * 497;
            const auto count = part < 2 ? 497 : 6;
            Check(packet.size() == (part < 2 ? 1512 : 60), "packet size obeys Ethernet MTU and minimum");
            Check(packet[13] == 0 && packet[14] == row, "row encoded big endian");
            Check(packet[15] == offset / 256 && packet[16] == offset % 256, "offset is in pixels, big endian");
            Check(packet[17] == count / 256 && packet[18] == count % 256, "count encoded big endian");
            for (size_t pixel = 0; pixel < static_cast<size_t>(count); ++pixel)
            {
                const auto & input = colors[row * 1000 + offset + pixel];
                Check(packet[21 + pixel * 3] == input.r && packet[22 + pixel * 3] == input.g && packet[23 + pixel * 3] == input.b,
                      "split rows reconstruct the complete logical frame");
            }
        }
    }
    const auto tall = EncodeColorlightFrame(Config(1, 257), std::vector<CRGB>(257, CRGB(1, 2, 3)));
    Check(tall.data[257][12] == 0x55 && tall.data[257][13] == 1 && tall.data[257][14] == 0,
          "row 256 uses high row byte, preserving packet type");
    for (int width : {1, 496, 497, 498, 994, 995})
    {
        const auto boundary = EncodeColorlightFrame(Config(width, 1), std::vector<CRGB>(width, CRGB(1, 2, 3)));
        Check(boundary.data.size() == 1 + static_cast<size_t>((width + 496) / 497), "exact split boundary packet count");
    }

    // Exercise the full 16-bit coordinate field without claiming a single
    // receiver can carry the whole strip. The declared cards remain bounded.
    for (bool vertical : {false, true})
    {
        auto edge = Config(vertical ? 1 : 65535, vertical ? 65535 : 1);
        edge.refresh_hz = 1;
        edge.receivers.clear();
        for (int index = 0; index < 8; ++index)
        {
            const int start = index * 8192;
            const int extent = std::min(8192, 65535 - start);
            edge.receivers.push_back({index, vertical ? 0 : start, vertical ? start : 0,
                                      vertical ? 1 : extent, vertical ? extent : 1});
        }
        const auto encoded = EncodeColorlightFrame(edge, std::vector<CRGB>(65535, CRGB(7, 8, 9)));
        const auto & last = encoded.data.back();
        if (vertical)
            Check(last[13] == 255 && last[14] == 254, "largest supported row high byte");
        else
        {
            Check(last[15] == 254 && last[16] == 83, "last split offset retains high coordinate byte");
            Check(last[17] == 1 && last[18] == 172, "last split count at maximum logical width");
        }
    }
}

void OrientationAndBrightness()
{
    auto config = Config(2, 2);
    const std::array<CRGB, 4> colors = {CRGB(1, 2, 3), CRGB(4, 5, 6), CRGB(7, 8, 9), CRGB(10, 11, 12)};
    config.flip_x = true;
    auto packets = EncodeColorlightFrame(config, colors);
    Check(packets.data[1][21] == 4 && packets.data[1][24] == 1 && packets.data[2][21] == 10,
          "explicit horizontal flip");
    config.flip_x = false;
    config.flip_y = true;
    packets = EncodeColorlightFrame(config, colors);
    Check(packets.data[1][21] == 7 && packets.data[1][24] == 10 && packets.data[2][21] == 1,
          "explicit vertical flip");
    config.flip_x = true;
    config.color_order = "BGR";
    packets = EncodeColorlightFrame(config, colors);
    Check(packets.data[1][21] == 12 && packets.data[1][22] == 11 && packets.data[1][23] == 10,
          "combined flips and BGR conversion applied exactly once");

    config.flip_x = config.flip_y = false;
    config.color_order = "RGB";
    config.firmware_profile = "v13";
    for (int brightness : {0, 128, 255})
    {
        config.brightness = brightness;
        packets = EncodeColorlightFrame(config, colors);
        Check(packets.data.size() == 4 && packets.sync.size() == 2, "v13 duplicates brightness and sync only");
        Check(packets.data[0] == packets.data[1] && packets.sync[0] == packets.sync[1], "firmware duplicate packet identity");
        Check(packets.data[0][13] == brightness && packets.data[0][14] == brightness && packets.data[0][15] == brightness && packets.data[0][16] == 255,
              "brightness including zero encoded directly");
        Check(packets.sync[0][35] == brightness && packets.sync[0][38] == brightness && packets.sync[0][39] == brightness && packets.sync[0][40] == brightness,
              "display brightness fields");
        Check(packets.data[2][21] == 1 && packets.data[2][22] == 2 && packets.data[2][23] == 3,
              "brightness does not double-scale RGB pixels or apply luminance correction");
    }
}

void DuplicatePixelPackets()
{
    auto config = Config(512, 32);
    config.firmware_profile = "v13";
    std::vector<CRGB> pixels(512 * 32);
    for (size_t index = 0; index < pixels.size(); ++index)
        pixels[index] = CRGB(index % 251, index % 239, index % 233);
    const auto single = EncodeColorlightFrame(config, pixels);
    config.duplicate_pixel_packets = true;
    const auto duplicated = EncodeColorlightFrame(config, pixels);
    Check(duplicated.data.size() == 130, "512x32 duplicate mode emits two brightness and 128 pixel packets");
    Check(duplicated.sync == single.sync, "pixel duplication does not alter display sync");
    Check(duplicated.data[0] == single.data[0] && duplicated.data[1] == single.data[1], "brightness packets remain unchanged");
    for (size_t index = 2; index < single.data.size(); ++index)
    {
        const auto at = 2 + (index - 2) * 2;
        Check(duplicated.data[at] == single.data[index] && duplicated.data[at + 1] == single.data[index],
              "each complete pixel fragment is repeated consecutively without coordinate or payload changes");
    }
}

void SingleSyncOverride()
{
    auto config = Config(512, 32);
    config.firmware_profile = "v13";
    std::vector<CRGB> pixels(512 * 32, CRGB(1, 2, 3));
    const auto baseline = EncodeColorlightFrame(config, pixels);
    config.sync_packet_count = 1;
    const auto single = EncodeColorlightFrame(config, pixels);
    Check(single.data == baseline.data, "single sync does not change brightness or pixel packets");
    Check(single.sync.size() == 1 && single.sync[0] == baseline.sync[0], "single sync override preserves packet bytes");
    for (int bad : {0, 3})
    {
        config.sync_packet_count = bad;
        Rejects([&] { EncodeColorlightFrame(config, pixels); }, "invalid sync count accepted");
    }
}

void SmallPacketRows()
{
    auto config = Config(512, 32);
    config.max_pixels_per_packet = 128;
    config.flip_x = true;
    std::vector<CRGB> pixels(512 * 32);
    for (size_t i = 0; i < pixels.size(); ++i)
        pixels[i] = CRGB(i % 251, (i / 512) % 256, i % 241);
    const auto packets = EncodeColorlightFrame(config, pixels);
    Check(packets.data.size() == 129, "four equal fragments per row");
    for (size_t y = 0; y < 32; ++y)
        for (size_t part = 0; part < 4; ++part)
        {
            const auto & p = packets.data[1 + y * 4 + part];
            const auto x = part * 128;
            Check(p[13] == 0 && p[14] == y && p[15] == x / 256 && p[16] == x % 256,
                  "small fragment coordinates");
            Check(p[17] == 0 && p[18] == 128 && p.size() == 405, "128-pixel fragment length");
            for (size_t j = 0; j < 128; ++j)
            {
                const auto & source = pixels[y * 512 + 511 - (x + j)];
                Check(p[21 + j * 3] == source.r && p[22 + j * 3] == source.g && p[23 + j * 3] == source.b,
                      "horizontal flip remains continuous across fragments");
            }
        }
}

void ScheduledLatch()
{
    auto config = Config(2, 1);
    config.prepare_ahead_us = 2000;
    config.refresh_hz = 60;
    auto fake = std::make_unique<CaptureTransport>();
    auto * capture = fake.get();
    ColorlightOutput output(config, std::move(fake));
    Check(output.PreparationLeadTime().count() == 2000, "configured preparation lead");
    Check(output.RefreshPeriod().count() == 16666666, "60 Hz period retains nanosecond precision");
    const std::vector<CRGB> frame = {CRGB(1,2,3),CRGB(4,5,6)};
    const auto deadline = DisplayTiming::Clock::now() + std::chrono::milliseconds(25);
    output.PresentAt(frame, deadline);
    Check(capture->events == std::vector<int>({0x0a,0x55,-1,0x01}), "staged rows drain before latch");
    Check(capture->sent_at[1] < deadline - std::chrono::milliseconds(5), "row upload occurs before display deadline");
    Check(capture->sent_at.back() >= deadline, "display sync never precedes deadline");
    const auto saved = capture->packets;
    const auto refreshDeadline = DisplayTiming::Clock::now() + std::chrono::milliseconds(25);
    output.RefreshAt(refreshDeadline);
    Check(capture->sent_at[saved.size()+1] < refreshDeadline - std::chrono::milliseconds(5), "retained rows also upload early");
    Check(capture->sent_at.back() >= refreshDeadline, "retained sync uses its deadline");
    Check(std::equal(saved.begin(),saved.end(),capture->packets.begin()+saved.size()), "staged repeat retains exact image bytes");
    auto failing = std::make_unique<CaptureTransport>();
    auto * failed = failing.get();
    failed->fail_drain = true;
    ColorlightOutput broken(config,std::move(failing));
    Rejects([&] { broken.PresentAt(frame,DisplayTiming::Clock::now()); }, "failed staged upload was ignored");
    Check(std::none_of(failed->packets.begin(),failed->packets.end(),[](const auto & p){return p[12]==1;}), "failed staged upload never latches");
}

void OutputLifecycle()
{
    auto fake = std::make_unique<CaptureTransport>();
    auto * capture = fake.get();
    ColorlightOutput output(Config(2, 1), std::move(fake));
    Check(output.Width() == 2 && output.Height() == 1, "logical output dimensions");
    Check(output.RefreshInterval().count() == 33333, "configured periodic refresh interval");
    output.Refresh();
    Check(capture->packets.empty(), "refresh waits for a complete frame");
    std::vector<CRGB> frame = {CRGB(1, 2, 3), CRGB(4, 5, 6)};
    output.Present(frame);
    Check(capture->events == std::vector<int>({0x0a, 0x55, -1, 0x01}), "rows finish before sync");
    const auto saved = capture->packets;
    frame[0] = CRGB(99, 99, 99);
    output.Refresh();
    Check(capture->packets.size() == saved.size() * 2, "refresh retransmits entire frame");
    Check(std::equal(saved.begin(), saved.end(), capture->packets.begin() + saved.size()), "refresh caches the presented frame");
    const auto before = capture->packets.size();
    Rejects([&] { output.Present(std::span<const CRGB>(frame).first(1)); }, "short frame accepted");
    Rejects([&] { output.Present(std::vector<CRGB>(3)); }, "oversized frame accepted");
    Check(capture->packets.size() == before, "bad frame emits no packet");

    auto badConfig = Config(2, 1);
    badConfig.width = 0;
    Rejects([&] { ColorlightOutput bad(badConfig); }, "invalid config not rejected before raw transport creation");

    for (bool drainFailure : {false, true})
    {
        auto failing = std::make_unique<CaptureTransport>();
        auto * failureCapture = failing.get();
        failureCapture->fail_after = drainFailure ? SIZE_MAX : 1;
        failureCapture->fail_drain = drainFailure;
        ColorlightOutput failed(Config(2, 1), std::move(failing));
        Rejects([&] { failed.Present(frame); }, "transport failure was ignored");
        Check(std::none_of(failureCapture->packets.begin(), failureCapture->packets.end(), [](const auto & packet) { return packet[12] == 0x01; }),
              "incomplete transmission must never issue display/sync");
    }
}
}

int main()
{
    try
    {
        GoldenPackets();
        SplittingAndCoordinates();
        OrientationAndBrightness();
        DuplicatePixelPackets();
        SingleSyncOverride();
        SmallPacketRows();
        ScheduledLatch();
        OutputLifecycle();
        std::cout << "Colorlight encoder/output tests passed\n";
        return 0;
    }
    catch (const std::exception & error)
    {
        std::cerr << "Colorlight test failure: " << error.what() << '\n';
        return 1;
    }
}
