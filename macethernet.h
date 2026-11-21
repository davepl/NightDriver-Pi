// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "colorlight.h"

// Native macOS BPF transmission, preserving the complete protocol Ethernet
// header. No IP stack, libpcap installation or additional frameworks required.
std::unique_ptr<EthernetTransport> CreateMacColorlightTransport(const std::string & interface);
void ConfigureMacColorlightDisplayThread(std::chrono::microseconds period, size_t packetCount);
