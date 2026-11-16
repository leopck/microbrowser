#pragma once

#include "microbrowser/types.h"

#include <string_view>

#include <span>
namespace mb {

[[nodiscard]] uint16_t to_rgb565(Color color);
[[nodiscard]] Color from_rgb565(uint16_t pixel);

class Canvas {
public:
    Canvas(uint16_t* pixels, Size size) : m_pixels(pixels), m_size(size) {}

    void fill(Rect rectangle, Color color, uint8_t opacity = 255, Rect clip = {});
    void fill_rounded(Rect rectangle, int radius, Color color, uint8_t opacity, Rect clip);
    void stroke(Rect rectangle, Color color, uint8_t opacity, Rect clip);
    void line(int x0, int y0, int x1, int y1, Color color, uint8_t opacity, Rect clip);
    void text(int x, int y, std::string_view utf8, int scale, Color color, uint8_t opacity, Rect clip);
    void image(Rect target, const Asset& asset, uint8_t opacity, Rect clip);
    [[nodiscard]] static int text_width(std::string_view utf8, int scale);
    [[nodiscard]] static int text_height(int scale) { return 7 * scale; }

private:
    uint16_t* m_pixels;
    Size m_size;

    void pixel(int x, int y, Color color, uint8_t opacity, Rect clip);
};

// Decodes a well-formed codepoint. Invalid bytes are rendered as '?'.
struct Utf8Character {
    uint32_t codepoint;
    uint8_t length;
};
Utf8Character next_utf8(std::string_view text, std::size_t offset);
std::string_view format_value(const Value& value, std::string_view format, std::span<char> output);
std::string format_value(const Value& value, std::string_view format);

} // namespace mb
