// NightDriver-Pi output selection and local receiver layout. GPL-3.0-or-later.
#include "outputconfig.h"

#define PICOJSON_USE_INT64
#include "third_party/picojson.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <set>
#include <stdexcept>

namespace
{
using Object = picojson::object;
using Value = picojson::value;

[[noreturn]] void Invalid(const std::string &message)
{
    throw std::invalid_argument("Colorlight config: " + message);
}

// picojson's context interface lets us reject duplicate keys and bound recursive
// nesting without changing the vendored parser. This schema has no float fields.
class ConfigParseContext : public picojson::default_parse_context
{
    int _depth;
public:
    ConfigParseContext(Value *value, int depth = 0)
        : picojson::default_parse_context(value), _depth(depth) {}

    bool set_number(double)
    {
        Invalid("numbers must be integers in the signed 64-bit range");
    }

    template <typename Iter> bool parse_array_item(picojson::input<Iter> &in, size_t)
    {
        if (_depth >= 8)
            Invalid("JSON nesting is too deep");
        auto &array = out_->get<picojson::array>();
        array.emplace_back();
        ConfigParseContext child(&array.back(), _depth + 1);
        return picojson::_parse(child, in);
    }

    template <typename Iter> bool parse_object_item(picojson::input<Iter> &in, const std::string &key)
    {
        if (_depth >= 8)
            Invalid("JSON nesting is too deep");
        auto &object = out_->get<Object>();
        if (object.find(key) != object.end())
            Invalid("duplicate key '" + key + "'");
        ConfigParseContext child(&object[key], _depth + 1);
        return picojson::_parse(child, in);
    }
};

// JSON files use UTF-8. picojson preserves raw non-ASCII string bytes, so
// validate encoding before parsing rather than accepting malformed names/keys.
void ValidateUTF8(const std::string &text)
{
    for (size_t i = 0; i < text.size(); ++i)
    {
        const auto first = static_cast<unsigned char>(text[i]);
        if (first < 0x80) continue;
        unsigned remaining;
        uint32_t codepoint;
        uint32_t minimum;
        if (first >= 0xc2 && first <= 0xdf) { remaining = 1; codepoint = first & 0x1f; minimum = 0x80; }
        else if (first >= 0xe0 && first <= 0xef) { remaining = 2; codepoint = first & 0x0f; minimum = 0x800; }
        else if (first >= 0xf0 && first <= 0xf4) { remaining = 3; codepoint = first & 0x07; minimum = 0x10000; }
        else Invalid("file is not valid UTF-8");
        while (remaining--)
        {
            if (++i == text.size()) Invalid("file contains truncated UTF-8");
            const auto byte = static_cast<unsigned char>(text[i]);
            if ((byte & 0xc0) != 0x80) Invalid("file is not valid UTF-8");
            codepoint = (codepoint << 6) | (byte & 0x3f);
        }
        if (codepoint < minimum || codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff))
            Invalid("file contains an invalid UTF-8 code point");
    }
}

// picojson accepts leading zeroes in integer tokens; JSON does not. Scan only
// outside quoted strings, leaving all other syntax validation to its parser.
void RejectLeadingZeroes(const std::string &text)
{
    bool quoted = false;
    bool escaped = false;
    for (size_t i = 0; i < text.size(); ++i)
    {
        const char c = text[i];
        if (quoted)
        {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
            continue;
        }
        if (c == '"') { quoted = true; continue; }
        if (c != '-' && (c < '0' || c > '9')) continue;
        size_t first = i + (c == '-');
        if (first + 1 < text.size() && text[first] == '0' &&
            text[first + 1] >= '0' && text[first + 1] <= '9')
            Invalid("JSON integer tokens must not have leading zeroes");
        while (i + 1 < text.size() && text[i + 1] >= '0' && text[i + 1] <= '9') ++i;
    }
}

const Object &AsObject(const Value &value, const std::string &path)
{
    if (!value.is<Object>()) Invalid(path + " must be an object");
    return value.get<Object>();
}

void Keys(const Object &object, std::initializer_list<const char *> allowed, const std::string &path)
{
    for (const auto &[key, value] : object)
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end())
            Invalid("unknown field '" + path + "." + key + "'");
}

const Value &Required(const Object &object, const char *key, const std::string &path)
{
    auto found = object.find(key);
    if (found == object.end()) Invalid("missing field '" + path + "." + key + "'");
    return found->second;
}

int Integer(const Value &value, const std::string &path)
{
    if (!value.is<int64_t>()) Invalid(path + " must be an integer");
    auto integer = value.get<int64_t>();
    if (integer < std::numeric_limits<int>::min() || integer > std::numeric_limits<int>::max())
        Invalid(path + " exceeds the supported integer range");
    return static_cast<int>(integer);
}

std::string String(const Value &value, const std::string &path)
{
    if (!value.is<std::string>()) Invalid(path + " must be a string");
    return value.get<std::string>();
}

bool Boolean(const Value &value, const std::string &path)
{
    if (!value.is<bool>()) Invalid(path + " must be true or false");
    return value.get<bool>();
}

void Range(int value, int minimum, int maximum, const std::string &path)
{
    if (value < minimum || value > maximum)
        Invalid(path + " must be between " + std::to_string(minimum) + " and " + std::to_string(maximum));
}
}

AppOptions ParseAppOptions(int &argc, char **argv)
{
    AppOptions options;
    bool output_seen = false;
    bool config_seen = false;
    bool port_seen = false;
    bool test_fps_seen = false;
    int remaining = 1;
    for (int i = 1; i < argc; ++i)
    {
        const std::string argument(argv[i]);
        if (argument == "--help" || argument == "-h")
        {
            options.help = true;
            continue;
        }
        auto ValueFor = [&](const std::string &name) -> std::string
        {
            if (argument == name)
            {
                if (++i >= argc || std::string(argv[i]).starts_with("--"))
                    throw std::invalid_argument(name + " requires a value");
                return argv[i];
            }
            return argument.substr(name.size() + 1);
        };
        if (argument == "--test-pattern")
        {
            if (options.test_pattern) throw std::invalid_argument("--test-pattern may only be supplied once");
            options.test_pattern = true;
        }
        else if (argument == "--test-fps" || argument.starts_with("--test-fps="))
        {
            if (test_fps_seen) throw std::invalid_argument("--test-fps may only be supplied once");
            test_fps_seen = true;
            const auto value = ValueFor("--test-fps");
            int fps = 0;
            const auto [end, error] = std::from_chars(value.data(), value.data()+value.size(), fps);
            if (error != std::errc{} || end != value.data()+value.size() || fps < 1 || fps > 60)
                throw std::invalid_argument("--test-fps must be an integer between 1 and 60");
            options.test_fps = fps;
        }
        else if (argument == "--output" || argument.starts_with("--output="))
        {
            if (output_seen) throw std::invalid_argument("--output may only be supplied once");
            output_seen = true;
            const auto value = ValueFor("--output");
            if (value == "hub75") options.output = OutputType::Hub75;
            else if (value == "colorlight") options.output = OutputType::Colorlight;
            else throw std::invalid_argument("--output must be hub75 or colorlight");
        }
        else if (argument == "--config" || argument.starts_with("--config="))
        {
            if (config_seen) throw std::invalid_argument("--config may only be supplied once");
            config_seen = true;
            options.config_path = ValueFor("--config");
            if (options.config_path.empty()) throw std::invalid_argument("--config requires a nonempty path");
        }
        else if (argument == "--port" || argument.starts_with("--port="))
        {
            if (port_seen) throw std::invalid_argument("--port may only be supplied once");
            port_seen = true;
            const auto value = ValueFor("--port");
            int port = 0;
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), port);
            if (error != std::errc{} || end != value.data() + value.size() || port < 1 || port > 65535)
                throw std::invalid_argument("--port must be an integer between 1 and 65535");
            options.incoming_port = port;
        }
        else argv[remaining++] = argv[i];
    }
    if (test_fps_seen && !options.test_pattern)
        throw std::invalid_argument("--test-fps requires --test-pattern");
    argc = remaining;
    argv[remaining] = nullptr;
    if (config_seen)
    {
        if (output_seen && options.output == OutputType::Hub75)
            throw std::invalid_argument("--output=hub75 conflicts with the Colorlight --config file");
        options.output = OutputType::Colorlight;
    }
    if (!options.help && options.output == OutputType::Colorlight && !config_seen)
        throw std::invalid_argument("--output=colorlight requires --config=FILE");
    return options;
}

ColorlightConfig LoadColorlightConfig(const std::string &path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) Invalid("cannot open '" + path + "'");
    // Layout metadata is small (at most 64 cards); refuse unbounded input before
    // allocating or parsing it.
    constexpr size_t maximum_config_bytes = 64 * 1024;
    std::string text(maximum_config_bytes + 1, '\0');
    input.read(text.data(), text.size());
    const auto size = input.gcount();
    if (size > static_cast<std::streamsize>(maximum_config_bytes)) Invalid("file exceeds 64 KiB");
    if (input.bad()) Invalid("cannot read '" + path + "'");
    text.resize(size);
    ValidateUTF8(text);
    RejectLeadingZeroes(text);
    Value root;
    ConfigParseContext context(&root);
    std::string error;
    auto end = picojson::_parse(context, text.cbegin(), text.cend(), &error);
    if (!error.empty()) Invalid(error);
    if (std::any_of(end, text.cend(), [](unsigned char c) { return !std::isspace(c); }))
        Invalid("unexpected content after JSON object");
    const auto &object = AsObject(root, "root");
    Keys(object, {"width", "height", "output"}, "root");
    ColorlightConfig config;
    config.width = Integer(Required(object, "width", "root"), "width");
    config.height = Integer(Required(object, "height", "root"), "height");
    const auto &output = AsObject(Required(object, "output", "root"), "output");
    Keys(output, {"type", "interface", "receivers", "firmware_profile", "brightness", "color_order",
                  "flip_x", "flip_y", "refresh_hz", "sync_gap_us", "duplicate_pixel_packets", "max_pixels_per_packet", "sync_packet_count", "prepare_ahead_us"}, "output");
    if (String(Required(output, "type", "output"), "output.type") != "colorlight")
        Invalid("output.type must be 'colorlight'");
    config.interface = String(Required(output, "interface", "output"), "output.interface");
    config.firmware_profile = String(Required(output, "firmware_profile", "output"), "output.firmware_profile");
    auto OptionalInt = [&](const char *key, int &target)
    {
        if (auto entry = output.find(key); entry != output.end()) target = Integer(entry->second, std::string("output.") + key);
    };
    auto OptionalBool = [&](const char *key, bool &target)
    {
        if (auto entry = output.find(key); entry != output.end()) target = Boolean(entry->second, std::string("output.") + key);
    };
    if (auto entry = output.find("sync_packet_count"); entry != output.end())
        config.sync_packet_count = Integer(entry->second, "output.sync_packet_count");
    OptionalInt("brightness", config.brightness);
    OptionalInt("refresh_hz", config.refresh_hz);
    OptionalInt("sync_gap_us", config.sync_gap_us);
    OptionalInt("prepare_ahead_us", config.prepare_ahead_us);
    OptionalInt("max_pixels_per_packet", config.max_pixels_per_packet);
    OptionalBool("flip_x", config.flip_x);
    OptionalBool("flip_y", config.flip_y);
    OptionalBool("duplicate_pixel_packets", config.duplicate_pixel_packets);
    if (auto entry = output.find("color_order"); entry != output.end()) config.color_order = String(entry->second, "output.color_order");
    const auto &receivers = Required(output, "receivers", "output");
    if (!receivers.is<picojson::array>()) Invalid("output.receivers must be an array");
    for (const auto &value : receivers.get<picojson::array>())
    {
        const auto &receiver = AsObject(value, "receiver");
        Keys(receiver, {"index", "x", "y", "width", "height"}, "receiver");
        config.receivers.push_back({
            Integer(Required(receiver, "index", "receiver"), "receiver.index"),
            Integer(Required(receiver, "x", "receiver"), "receiver.x"),
            Integer(Required(receiver, "y", "receiver"), "receiver.y"),
            Integer(Required(receiver, "width", "receiver"), "receiver.width"),
            Integer(Required(receiver, "height", "receiver"), "receiver.height")});
    }
    ValidateColorlightConfig(config);
    return config;
}

void ValidateColorlightConfig(const ColorlightConfig &config)
{
    // Protocol coordinates are unsigned 16-bit. Per-card loading restrictions
    // below are intentionally more conservative than this address space.
    Range(config.width, 1, 65535, "width");
    Range(config.height, 1, 65535, "height");
    const auto pixels = static_cast<uint64_t>(config.width) * config.height;
    if (pixels > 655360) Invalid("received geometry exceeds the 655360 pixel application limit");
    if (config.interface.empty() || config.interface.size() > 15 || config.interface == "." || config.interface == ".." ||
        std::any_of(config.interface.begin(), config.interface.end(), [](unsigned char c)
        { return c == '/' || c == ':' || c == '\0' || std::isspace(c); }))
        Invalid("interface must be an Ethernet interface name of 1..15 characters without slash, colon or whitespace");
    if (config.firmware_profile != "legacy" && config.firmware_profile != "v13")
        Invalid("firmware_profile must be explicitly set to 'legacy' or 'v13'");
    if (config.color_order != "RGB" && config.color_order != "BGR") Invalid("color_order must be 'RGB' or 'BGR'");
    if (config.sync_packet_count) Range(*config.sync_packet_count, 1, 2, "sync_packet_count");
    Range(config.brightness, 0, 255, "brightness");
    Range(config.refresh_hz, 1, 60, "refresh_hz");
    Range(config.prepare_ahead_us, 0, std::min(20000, 1000000 / config.refresh_hz - 1), "prepare_ahead_us");
    Range(config.sync_gap_us, 0, 20000, "sync_gap_us");
    Range(config.max_pixels_per_packet, 1, 497, "max_pixels_per_packet");
    if (config.receivers.empty() || config.receivers.size() > 64) Invalid("receivers must contain 1..64 entries");
    std::set<int> indices;
    for (const auto &receiver : config.receivers)
    {
        Range(receiver.index, 0, 63, "receiver.index");
        if (!indices.insert(receiver.index).second) Invalid("duplicate receiver index " + std::to_string(receiver.index));
        Range(receiver.width, 1, 8192, "receiver.width");
        Range(receiver.height, 1, 8192, "receiver.height");
        if (static_cast<uint64_t>(receiver.width) * receiver.height > 65536) Invalid("receiver exceeds 65536 pixel loading limit");
        if (receiver.x < 0 || receiver.y < 0 || receiver.x > config.width || receiver.y > config.height ||
            receiver.width > config.width - receiver.x || receiver.height > config.height - receiver.y)
            Invalid("receiver rectangle is outside the received geometry");
    }
    // Gaps are permitted; overlapping expected regions usually signal a layout
    // mistake and are rejected. These rectangles do not program receiver cards.
    for (size_t i = 0; i < config.receivers.size(); ++i)
        for (size_t j = i + 1; j < config.receivers.size(); ++j)
        {
            const auto &a = config.receivers[i];
            const auto &b = config.receivers[j];
            if (a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height)
                Invalid("receiver rectangles overlap");
        }
}
