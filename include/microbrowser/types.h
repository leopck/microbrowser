#pragma once

#include "microbrowser/device_identity.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <memory>
#include <vector>

namespace mb {

inline constexpr uint32_t no_index = UINT32_MAX;
inline constexpr std::size_t max_chart_samples = 64;

struct Size {
    int width;
    int height;
};

struct Rect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    [[nodiscard]] bool empty() const { return width <= 0 || height <= 0; }
    [[nodiscard]] bool contains(int px, int py) const;
    [[nodiscard]] Rect clipped(Rect other) const;
    [[nodiscard]] Rect united(Rect other) const;
};

struct Color {
    uint32_t rgba_value = 0;

    [[nodiscard]] static constexpr Color rgb(uint8_t red, uint8_t green, uint8_t blue) {
        return Color{(uint32_t(red) << 24U) | (uint32_t(green) << 16U) | (uint32_t(blue) << 8U) | 0xffU};
    }
    [[nodiscard]] static constexpr Color rgba(uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha) {
        return Color{(uint32_t(red) << 24U) | (uint32_t(green) << 16U) | (uint32_t(blue) << 8U) | uint32_t(alpha)};
    }
    [[nodiscard]] constexpr uint8_t red() const { return uint8_t(rgba_value >> 24U); }
    [[nodiscard]] constexpr uint8_t green() const { return uint8_t(rgba_value >> 16U); }
    [[nodiscard]] constexpr uint8_t blue() const { return uint8_t(rgba_value >> 8U); }
    [[nodiscard]] constexpr uint8_t alpha() const { return uint8_t(rgba_value); }
};

namespace palette {
inline constexpr auto transparent = Color::rgba(0, 0, 0, 0);
inline constexpr auto black = Color::rgb(0, 0, 0);
inline constexpr auto white = Color::rgb(255, 255, 255);
inline constexpr auto ink = Color::rgb(31, 41, 59);
inline constexpr auto muted = Color::rgb(104, 117, 138);
inline constexpr auto background = Color::rgb(243, 245, 250);
inline constexpr auto surface = Color::rgb(255, 255, 255);
inline constexpr auto border = Color::rgb(220, 227, 237);
inline constexpr auto accent = Color::rgb(18, 130, 111);
inline constexpr auto blue = Color::rgb(66, 99, 235);
inline constexpr auto warning = Color::rgb(217, 137, 36);
inline constexpr auto error = Color::rgb(188, 45, 62);
} // namespace palette

struct Value {
    enum class Type : uint8_t {
        number = 0,
        boolean = 1,
        string = 2,
        integer = 3,
        bytes = 4,
        handle = 5,
        void_value = 255,
    };

    Type type = Type::void_value;
    float number = 0.0F;
    bool boolean = false;
    std::int64_t integer = 0;
    std::string string;
    ByteBufferPtr bytes;
    DeviceHandle handle;

    [[nodiscard]] static Value from_number(float value);
    [[nodiscard]] static Value from_bool(bool value);
    [[nodiscard]] static Value from_string(std::string value);
    [[nodiscard]] static Value from_integer(std::int64_t value);
    [[nodiscard]] static Value from_bytes(ByteBuffer value);
    [[nodiscard]] static Value from_bytes(ByteBufferPtr value);
    [[nodiscard]] static Value from_handle(DeviceHandle value = {});
    [[nodiscard]] static Value from_void();
};

struct StateField {
    std::string name;
    Value initial;
};

enum class NodeKind : uint8_t {
    view = 0,
    row = 1,
    column = 2,
    text = 3,
    button = 4,
    input = 5,
    slider = 6,
    progress = 7,
    chart = 8,
    image = 9,
    spacer = 10,
    stack = 11,
};

enum class LengthKind : uint8_t { automatic, fixed, percent, grow };
struct Length {
    LengthKind kind = LengthKind::automatic;
    float value = 0.0F;
};

enum class Align : uint8_t { start, center, end_position, stretch };
enum class Justify : uint8_t { start, center, end_position, space_between };

struct Style {
    Length width;
    Length height;
    int padding = 0;
    int gap = 0;
    int font_size = 14;
    int font_weight = 400;
    int radius = 0;
    Color background = palette::transparent;
    Color foreground = palette::ink;
    Color border = palette::transparent;
    Align align = Align::stretch;
    Justify justify = Justify::start;
    int x = 0;
    int y = 0;
    bool absolute = false;
    bool scroll = false;
};

struct Function {
    std::string name;
    uint16_t parameter_count = 0;
    uint16_t local_count = 0;
    std::vector<Value> constants;
    std::vector<uint8_t> code;
};

struct Node {
    NodeKind kind = NodeKind::view;
    Style style;
    uint32_t parent = no_index;
    // Unique ownership keeps child addresses stable as sibling lists grow.
    std::vector<std::unique_ptr<Node>> children;

    std::string id;
    std::string text;
    std::string format;
    std::string placeholder;
    std::string source;
    std::string animation;
    uint32_t binding = no_index;
    float minimum = 0.0F;
    float maximum = 100.0F;
    float step = 1.0F;
    uint16_t max_length = 64;
    std::array<float, max_chart_samples> samples{};
    uint8_t sample_count = 0;
    uint8_t sample_head = 0;
    uint32_t on_press = no_index;
    uint32_t on_change = no_index;
    uint32_t on_submit = no_index;
    uint32_t on_load = no_index;
    Rect box;
    float opacity = 1.0F;
    float scale = 1.0F;
    float offset_y = 0.0F;
    float scroll = 0.0F;
    float scroll_max = 0.0F;
    uint32_t animation_property = 0;
    uint64_t animation_started = 0;
    uint64_t animation_duration = 0;
    float animation_from = 0.0F;
    float animation_target = 0.0F;
    uint64_t pulse_started = 0;
    uint64_t pulse_period = 0;
};

struct Page {
    std::string name;
    Node root;
};

struct Asset {
    std::string name;
    uint16_t width = 0;
    uint16_t height = 0;
    std::vector<uint16_t> pixels;
};

struct Package {
    std::vector<std::string> strings;
    std::vector<StateField> states;
    std::vector<Function> functions;
    std::vector<Page> pages;
    std::vector<Asset> assets;
    std::uint16_t format_major = 2;
    std::uint16_t format_minor = 0;
    std::vector<CapabilityVersion> required_capabilities;
    std::vector<CapabilityVersion> optional_capabilities;
    std::vector<std::string> permission_endpoints;
};

} // namespace mb
