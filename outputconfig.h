// NightDriver-Pi output selection and local receiver layout. GPL-3.0-or-later.
#pragma once

#include <optional>
#include <string>
#include <vector>

enum class OutputType { Hub75, Colorlight };

struct AppOptions
{
    OutputType output = OutputType::Hub75;
    std::string config_path;
    int incoming_port = 49152;
    bool help = false;
    bool test_pattern = false;
    int test_fps = 30;
};

struct ColorlightReceiver
{
    int index;
    int x;
    int y;
    int width;
    int height;
};

struct ColorlightConfig
{
    int width = 0;
    int height = 0;
    std::string interface;
    std::vector<ColorlightReceiver> receivers;
    int brightness = 255;
    // Deliberately required: firmware variants have different latch sequences.
    std::string firmware_profile;
    std::string color_order = "RGB";
    bool flip_x = false;
    bool flip_y = false;
    int refresh_hz = 30;
    int sync_gap_us = 500;
    int max_pixels_per_packet = 497;
    int prepare_ahead_us = 0;
    bool duplicate_pixel_packets = false;
    std::optional<int> sync_packet_count;
};

// Remove application flags in place, leaving all matrix-library flags untouched.
// A config selects Colorlight unless an explicitly conflicting output is given.
AppOptions ParseAppOptions(int &argc, char **argv);
ColorlightConfig LoadColorlightConfig(const std::string &path);
void ValidateColorlightConfig(const ColorlightConfig &config);
