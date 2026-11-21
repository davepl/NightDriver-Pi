#include "outputconfig.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>


namespace
{
void Rejects(const std::function<void()> &action)
{
    bool rejected = false;
    try { action(); } catch (const std::invalid_argument &) { rejected = true; }
    assert(rejected);
}

class ConfigFile
{
    std::filesystem::path _path;
public:
    ConfigFile()
    {
        _path = std::filesystem::temp_directory_path() / ("ndpi-config-test-" + std::to_string(getpid()) + ".json");
    }
    ~ConfigFile() { std::filesystem::remove(_path); }
    ColorlightConfig Read(const std::string &text)
    {
        std::ofstream(_path) << text;
        return LoadColorlightConfig(_path.string());
    }
};

const std::string valid = R"({"width":256,"height":64,"output":{"type":"colorlight","interface":"eth0","firmware_profile":"legacy","receivers":[{"index":0,"x":0,"y":0,"width":128,"height":64},{"index":1,"x":128,"y":0,"width":128,"height":64}]}})";

AppOptions Parse(std::vector<std::string> &args)
{
    std::vector<char *> argv;
    for (auto &arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);
    int argc = static_cast<int>(args.size());
    auto result = ParseAppOptions(argc, argv.data());
    std::vector<std::string> remainder;
    for (int i = 0; i < argc; ++i) remainder.emplace_back(argv[i]);
    assert(argv[argc] == nullptr);
    args = remainder;
    return result;
}
}

int main()
{
    ConfigFile file;
    const auto config = file.Read(valid);
    assert(config.width == 256 && config.height == 64 && config.receivers.size() == 2);
    assert(config.brightness == 255 && config.color_order == "RGB" && !config.flip_x && !config.flip_y);
    assert(config.firmware_profile == "legacy" && config.refresh_hz == 30 && config.sync_gap_us == 500);

    auto RejectModified = [&](const std::function<void(ColorlightConfig &)> &change)
    {
        auto modified = config;
        change(modified);
        Rejects([&] { ValidateColorlightConfig(modified); });
    };
    RejectModified([](auto &c) { c.width = 0; });
    RejectModified([](auto &c) { c.height = -1; });
    RejectModified([](auto &c) { c.width = 65536; });
    RejectModified([](auto &c) { c.width = c.height = 65535; });
    RejectModified([](auto &c) { c.interface = ""; });
    RejectModified([](auto &c) { c.interface = "sixteencharacters"; });
    RejectModified([](auto &c) { c.interface = "eth 0"; });
    RejectModified([](auto &c) { c.interface = "eth/0"; });
    RejectModified([](auto &c) { c.interface = std::string("eth\0", 4); });
    RejectModified([](auto &c) { c.brightness = -1; });
    RejectModified([](auto &c) { c.brightness = 256; });
    RejectModified([](auto &c) { c.refresh_hz = 0; });
    RejectModified([](auto &c) { c.refresh_hz = 61; });
    RejectModified([](auto &c) { c.sync_gap_us = -1; });
    RejectModified([](auto &c) { c.sync_gap_us = 20001; });
    RejectModified([](auto &c) { c.color_order = "GRB"; });
    RejectModified([](auto &c) { c.firmware_profile.clear(); });
    RejectModified([](auto &c) { c.receivers.clear(); });
    RejectModified([](auto &c) { c.receivers[0].index = -1; });
    RejectModified([](auto &c) { c.receivers[0].index = 64; });
    RejectModified([](auto &c) { c.receivers[1].index = 0; });
    RejectModified([](auto &c) { c.receivers[1].x = 127; });
    RejectModified([](auto &c) { c.receivers[0].x = -1; });
    RejectModified([](auto &c) { c.receivers[0].width = 0; });
    RejectModified([](auto &c) { c.receivers[0].width = 2147483647; });
    RejectModified([](auto &c) { c.receivers[0].x = 2147483647; });
    RejectModified([](auto &c) { c.receivers[0].height = 65; });
    RejectModified([](auto &c) { c.width = c.height = 512; c.receivers = {{0,0,0,512,512}}; });
    RejectModified([](auto &c) { c.width = 8193; c.receivers = {{0,0,0,8193,1}}; });
    auto gap = config;
    gap.receivers.pop_back();
    ValidateColorlightConfig(gap); // Unused regions are allowed.
    auto rotated = config;
    rotated.width = 64; rotated.height = 1024;
    rotated.receivers = {{63, 0, 0, 64, 1024}};
    ValidateColorlightConfig(rotated); // Card loading cap does not impose a 256px side.

    for (const auto &invalid : std::vector<std::string>{
        "", "{", "[]", "null", "{}", valid + " garbage", valid + "{}",
        R"({"width":256,"width":256})", R"({"width":01})", R"({"width":-01})",
        R"({"width":256.0})", R"({"width":2e2})", R"({"width":9223372036854775808})",
        R"({"width":2147483648})", R"({"width":"256"})",
        R"({"width":1,"height":1,"output":{"type":"hub75"}})",
        R"({"width":1,"height":1,"output":{"type":"colorlight","interface":"eth0","receivers":[]}})",
        std::string(65537, ' '), std::string(20, '[') + std::string(20, ']')})
        Rejects([&] { file.Read(invalid); });
    auto Typo = [&](const std::string &from, const std::string &to)
    {
        auto text = valid;
        text.replace(text.find(from), from.size(), to);
        Rejects([&] { file.Read(text); });
    };
    Typo("\"width\":256", "\"width\":256,\"widht\":256");
    Typo("\"interface\":\"eth0\"", "\"interface\":\"eth0\",\"brightnesss\":50");
    Typo("\"interface\":\"eth0\"", "\"interface\":\"eth0\",\"duplicate_pixel_packets\":1");
    Typo("\"index\":0", "\"index\":0,\"offset\":0");
    Typo("\"index\":0", "\"index\":true");
    Typo("\"index\":0", "\"index\":0,\"index\":1");
    // Invalid UTF-8 must not sneak through raw JSON string values.
    for (const auto &encoding : std::vector<std::string>{
        std::string("\xc0\x80", 2), std::string("\xed\xa0\x80", 3),
        std::string("\xf4\x90\x80\x80", 4), std::string("\xe2\x82", 2)})
    {
        auto text = valid;
        text.replace(text.find("eth0"), 4, encoding);
        Rejects([&] { file.Read(text); });
    }
    auto with_options = valid;
    with_options.replace(with_options.find("\"interface\""), 0,
                         "\"flip_x\":true,\"flip_y\":true,\"duplicate_pixel_packets\":true,\"color_order\":\"BGR\",\"brightness\":0,\"refresh_hz\":60,\"sync_gap_us\":0,\"max_pixels_per_packet\":128,\"sync_packet_count\":1,\"prepare_ahead_us\":2000,");
    Typo("\"interface\":\"eth0\"", "\"interface\":\"eth0\",\"max_pixels_per_packet\":498");
    Typo("\"interface\":\"eth0\"", "\"interface\":\"eth0\",\"max_pixels_per_packet\":0");
    Typo("\"interface\":\"eth0\"", "\"interface\":\"eth0\",\"sync_packet_count\":0");
    Typo("\"interface\":\"eth0\"", "\"interface\":\"eth0\",\"sync_packet_count\":3");
    Typo("\"interface\":\"eth0\"", "\"interface\":\"eth0\",\"prepare_ahead_us\":-1");
    Typo("\"interface\":\"eth0\"", "\"interface\":\"eth0\",\"refresh_hz\":60,\"prepare_ahead_us\":16667");
    const auto options_config = file.Read(with_options);
    assert(options_config.flip_x && options_config.flip_y && options_config.color_order == "BGR");
    assert(options_config.brightness == 0 && options_config.refresh_hz == 60 && options_config.sync_gap_us == 0);
    assert(options_config.duplicate_pixel_packets && options_config.max_pixels_per_packet == 128 && options_config.sync_packet_count == 1 && options_config.prepare_ahead_us == 2000);
    Rejects([] { LoadColorlightConfig("/path/that/does/not/exist/ndpi.json"); });

    std::vector<std::string> args = {"ndpi", "--led-rows=64", "--led-chain=2"};
    auto options = Parse(args);
    assert(options.output == OutputType::Hub75 && options.incoming_port == 49152 && args.size() == 3);
    args = {"ndpi", "--port=49153", "--led-rows=64", "--led-chain=2"};
    options = Parse(args);
    assert(options.incoming_port == 49153 && args.size() == 3 && args[1] == "--led-rows=64");
    args = {"ndpi", "--test-pattern", "--test-fps=60"};
    options = Parse(args);
    assert(options.test_pattern && options.test_fps == 60 && args.size() == 1);
    args = {"ndpi", "--test-pattern"};
    assert(Parse(args).test_fps == 30);
    args = {"ndpi", "--port", "65535"};
    assert(Parse(args).incoming_port == 65535 && args.size() == 1);
    args = {"ndpi", "--port=1"};
    assert(Parse(args).incoming_port == 1);
    args = {"ndpi", "--output=colorlight", "--config=wall.json"};
    options = Parse(args);
    assert(options.output == OutputType::Colorlight && options.config_path == "wall.json" && args.size() == 1);
    args = {"ndpi", "--config", "wall.json", "--output", "colorlight", "--led-rows=64"};
    options = Parse(args);
    assert(options.output == OutputType::Colorlight && args.size() == 2 && args[1] == "--led-rows=64");
    args = {"ndpi", "--config=wall.json"};
    assert(Parse(args).output == OutputType::Colorlight);
    args = {"ndpi", "--output=colorlight", "--help"};
    assert(Parse(args).help);
    args = {"ndpi", "--output=hub75", "--led-help"};
    assert(Parse(args).output == OutputType::Hub75 && args.size() == 2);
    for (const auto &bad : std::vector<std::vector<std::string>>{
        {"ndpi", "--test-fps=30"}, {"ndpi", "--test-pattern", "--test-fps=0"},
        {"ndpi", "--test-pattern", "--test-fps=61"}, {"ndpi", "--test-pattern", "--test-fps=30.5"},
        {"ndpi", "--test-pattern", "--test-pattern"},
        {"ndpi", "--test-pattern", "--test-fps=30", "--test-fps=60"},
        {"ndpi", "--output"}, {"ndpi", "--output="}, {"ndpi", "--output=UDP"},
        {"ndpi", "--config"}, {"ndpi", "--config="}, {"ndpi", "--config", "--output=colorlight"},
        {"ndpi", "--output=colorlight"}, {"ndpi", "--output=hub75", "--config=wall.json"},
        {"ndpi", "--output=hub75", "--output=hub75"}, {"ndpi", "--config=a", "--config=b"},
        {"ndpi", "--port"}, {"ndpi", "--port="}, {"ndpi", "--port", "--help"},
        {"ndpi", "--port=0"}, {"ndpi", "--port=-1"}, {"ndpi", "--port=65536"},
        {"ndpi", "--port=4294967297"}, {"ndpi", "--port=abc"}, {"ndpi", "--port=1.5"},
        {"ndpi", "--port=123abc"}, {"ndpi", "--port= 123"}, {"ndpi", "--port=+123"},
        {"ndpi", "--port=49153", "--port", "49153"}})
    {
        auto copy = bad;
        Rejects([&] { Parse(copy); });
    }
    std::cout << "configuration tests passed\n";
}
