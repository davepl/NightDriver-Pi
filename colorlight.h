// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "displayoutput.h"
#include "outputconfig.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using EthernetPacket = std::vector<uint8_t>;

// A frame is latched only after every data packet has been transmitted.
struct ColorlightFramePackets
{
    std::vector<EthernetPacket> data;
    std::vector<EthernetPacket> sync;
};

// Encodes complete Ethernet frames, excluding NIC-generated preamble/FCS.
// RGB/BGR and orientation are applied here, once, to the logical RGB image.
ColorlightFramePackets EncodeColorlightFrame(const ColorlightConfig & config,
                                            std::span<const CRGB> frame);

class EthernetTransport
{
  public:
    virtual ~EthernetTransport() = default;
    virtual void Send(std::span<const uint8_t> packet) = 0;
    // Drain queued row data and leave the receiver time to store the last row.
    virtual void FinishRows(std::chrono::microseconds guard) = 0;
};

std::unique_ptr<EthernetTransport> CreateColorlightTransport(const std::string & interface);

class ColorlightOutput final : public DisplayOutput
{
    ColorlightConfig _config;
    std::unique_ptr<EthernetTransport> _transport;
    ColorlightFramePackets _lastFrame;

    void TransmitRows(const ColorlightFramePackets & packets);
    void Latch(const ColorlightFramePackets & packets);
    void Transmit(const ColorlightFramePackets & packets);

  public:
    explicit ColorlightOutput(const ColorlightConfig & config,
                              std::unique_ptr<EthernetTransport> transport = nullptr);
    int Width() const override { return _config.width; }
    int Height() const override { return _config.height; }
    void Present(std::span<const CRGB> frame) override;
    std::chrono::microseconds RefreshInterval() const override;
    void Refresh() override;
    std::chrono::nanoseconds RefreshPeriod() const override;
    std::chrono::microseconds PreparationLeadTime() const override;
    void PresentAt(std::span<const CRGB> frame, DisplayTiming::Clock::time_point deadline) override;
    void RefreshAt(DisplayTiming::Clock::time_point deadline) override;
    // Called on the display thread after other application threads are started.
    void ConfigureDisplayThread();
};
