#include "microbrowser/canvas.h"

#include "fonts.h"
#include "text_layout.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace mb {
namespace {

constexpr std::array<uint8_t, 16> coverage_alpha{
    0, 17, 34, 51, 68, 85, 102, 119, 136, 153, 170, 187, 204, 221, 238, 255,
};

int corner_inset(int radius, int relative_y, int height) {
    if (relative_y < radius) {
        const int dy = radius - relative_y - 1;
        for (int inset = 0; inset < radius; ++inset) {
            if ((radius - inset - 1) * (radius - inset - 1) + dy * dy < radius * radius) return inset;
        }
        return radius;
    }
    if (relative_y < height - radius) return 0;
    return corner_inset(radius, height - relative_y - 1, height);
}

int bounded_radius(Rect rectangle, int radius) {
    return std::max(0, std::min(radius, std::min(rectangle.width, rectangle.height) / 2));
}

void append_text(std::span<char> output, std::size_t& used, std::string_view text) {
    if (used >= output.size()) return;
    std::size_t count = std::min(text.size(), output.size() - used);
    if (count < text.size()) {
        while (count > 0 && (uint8_t(text[count]) & 0xc0U) == 0x80U) --count;
        if (count > 0 && (uint8_t(text[count - 1]) & 0x80U) != 0) --count;
    }
    std::memcpy(output.data() + used, text.data(), count);
    used += count;
}

void default_value(const Value& value, std::string_view format, std::span<char> output,
                   std::size_t& used) {
    char number[64];
    switch (value.type) {
    case Value::Type::number: {
        char conversion[24];
        int precision = 6;
        bool integer = false;
        if (!format.empty()) {
            if (format == "%d") {
                integer = true;
            } else if (format == "%f") {
                precision = 6;
            } else if (format.size() >= 4 && format.starts_with("%.") && format.back() == 'f') {
                const auto result = std::from_chars(format.data() + 2, format.data() + format.size() - 1, precision);
                if (result.ec != std::errc{} || result.ptr != format.data() + format.size() - 1 || precision > 6) {
                    throw std::invalid_argument("invalid numeric format");
                }
            } else {
                throw std::invalid_argument("numeric value has an incompatible format");
            }
        }
        int length = 0;
        if (integer) {
            const double numeric_value = value.number;
            constexpr double int64_minimum = static_cast<double>(std::numeric_limits<int64_t>::min());
            constexpr double int64_upper_bound = -int64_minimum;
            if (!std::isfinite(numeric_value) || numeric_value < int64_minimum ||
                numeric_value >= int64_upper_bound) {
                throw std::invalid_argument("%d requires a finite value in the int64 range");
            }
            length = std::snprintf(number, sizeof(number), "%lld",
                                   static_cast<long long>(std::llround(numeric_value)));
        } else if (format.empty()) {
            length = std::snprintf(number, sizeof(number), "%.3f", double(value.number));
            while (length > 0 && number[length - 1] == '0') number[--length] = '\0';
            if (length > 0 && number[length - 1] == '.') number[--length] = '\0';
        } else {
            std::snprintf(conversion, sizeof(conversion), "%%.%df", precision);
            length = std::snprintf(number, sizeof(number), conversion, double(value.number));
        }
        if (length < 0) throw std::invalid_argument("number formatting failed");
        append_text(output, used, std::string_view(number, std::min<std::size_t>(length, sizeof(number))));
        return;
    }
    case Value::Type::boolean:
        if (format == "%s") append_text(output, used, value.boolean ? "true" : "false");
        else if (!format.empty()) append_text(output, used, value.boolean ? "1" : "0");
        else append_text(output, used, value.boolean ? "true" : "false");
        return;
    case Value::Type::string:
        if (!format.empty() && format != "%s") throw std::invalid_argument("string value has an incompatible format");
        append_text(output, used, value.string);
        return;
    case Value::Type::void_value:
        return;
    }
}

} // namespace

uint16_t to_rgb565(Color color) {
    return uint16_t(((color.red() & 0xf8U) << 8U) | ((color.green() & 0xfcU) << 3U) |
                    (color.blue() >> 3U));
}

Color from_rgb565(uint16_t pixel) {
    const uint8_t r = uint8_t((pixel >> 11U) & 31U);
    const uint8_t g = uint8_t((pixel >> 5U) & 63U);
    const uint8_t b = uint8_t(pixel & 31U);
    return Color::rgb(uint8_t((r << 3U) | (r >> 2U)),
                      uint8_t((g << 2U) | (g >> 4U)),
                      uint8_t((b << 3U) | (b >> 2U)));
}

void Canvas::pixel(int x, int y, Color color, uint8_t opacity, Rect clip) {
    if (x < 0 || y < 0 || x >= m_size.width || y >= m_size.height || !clip.contains(x, y)) return;
    const unsigned alpha = (unsigned(color.alpha()) * opacity + 127U) / 255U;
    if (alpha == 0) return;
    const auto target = std::size_t(y) * std::size_t(m_size.width) + std::size_t(x);
    if (alpha == 255) {
        m_pixels[target] = to_rgb565(color);
        return;
    }
    const Color previous = from_rgb565(m_pixels[target]);
    const auto blend = [alpha](uint8_t source, uint8_t destination) {
        return uint8_t((unsigned(source) * alpha + unsigned(destination) * (255U - alpha) + 127U) / 255U);
    };
    m_pixels[target] = to_rgb565(Color::rgb(blend(color.red(), previous.red()),
                                           blend(color.green(), previous.green()),
                                           blend(color.blue(), previous.blue())));
}

void Canvas::fill(Rect rectangle, Color color, uint8_t opacity, Rect clip) {
    const Rect area{0, 0, m_size.width, m_size.height};
    const Rect bounds = rectangle.clipped(clip.empty() ? area : clip).clipped(area);
    if (bounds.empty() || color.alpha() == 0 || opacity == 0) return;
    if (unsigned(color.alpha()) * opacity >= 254U * 255U) {
        const uint16_t pixel_value = to_rgb565(color);
        for (int y = bounds.y; y < bounds.y + bounds.height; ++y) {
            auto* row = m_pixels + std::size_t(y) * std::size_t(m_size.width);
            std::fill_n(row + bounds.x, bounds.width, pixel_value);
        }
    } else {
        for (int y = bounds.y; y < bounds.y + bounds.height; ++y) {
            for (int x = bounds.x; x < bounds.x + bounds.width; ++x) pixel(x, y, color, opacity, area);
        }
    }
}

void Canvas::fill_rounded(Rect rectangle, int radius, Color color, uint8_t opacity, Rect clip) {
    const Rect screen{0, 0, m_size.width, m_size.height};
    const Rect bounds = rectangle.clipped(clip.empty() ? screen : clip).clipped(screen);
    if (bounds.empty() || color.alpha() == 0 || opacity == 0) return;
    radius = bounded_radius(rectangle, radius);
    for (int y = bounds.y; y < bounds.y + bounds.height; ++y) {
        const int inset = corner_inset(radius, y - rectangle.y, rectangle.height);
        fill({rectangle.x + inset, y, rectangle.width - inset * 2, 1}, color, opacity, bounds);
    }
}

void Canvas::stroke_rounded(Rect rectangle, int radius, Color color, uint8_t opacity, Rect clip) {
    const Rect screen{0, 0, m_size.width, m_size.height};
    const Rect bounds = rectangle.clipped(clip.empty() ? screen : clip).clipped(screen);
    if (bounds.empty() || color.alpha() == 0 || opacity == 0) return;
    radius = bounded_radius(rectangle, radius);
    if (radius == 0 || rectangle.width < 3 || rectangle.height < 3) {
        stroke(rectangle, color, opacity, bounds);
        return;
    }
    int previous = radius;
    if (bounds.y > rectangle.y) {
        previous = corner_inset(radius, bounds.y - rectangle.y - 1, rectangle.height);
    }
    for (int y = bounds.y; y < bounds.y + bounds.height; ++y) {
        const int relative_y = y - rectangle.y;
        const int inset = corner_inset(radius, relative_y, rectangle.height);
        const int near = std::min(previous, inset);
        const int far = std::max(previous, inset);
        fill({rectangle.x + near, y, far - near + 1, 1}, color, opacity, bounds);
        fill({rectangle.x + rectangle.width - far - 1, y, far - near + 1, 1}, color, opacity, bounds);
        if (relative_y == 0 || relative_y == rectangle.height - 1) {
            fill({rectangle.x + radius, y, rectangle.width - 2 * radius, 1}, color, opacity, bounds);
        }
        previous = inset;
    }
}

void Canvas::stroke(Rect rectangle, Color color, uint8_t opacity, Rect clip) {
    if (rectangle.width < 2 || rectangle.height < 2) return;
    fill({rectangle.x, rectangle.y, rectangle.width, 1}, color, opacity, clip);
    fill({rectangle.x, rectangle.y + rectangle.height - 1, rectangle.width, 1}, color, opacity, clip);
    fill({rectangle.x, rectangle.y, 1, rectangle.height}, color, opacity, clip);
    fill({rectangle.x + rectangle.width - 1, rectangle.y, 1, rectangle.height}, color, opacity, clip);
}

void Canvas::line(int x0, int y0, int x1, int y1, Color color, uint8_t opacity, Rect clip) {
    if (clip.empty()) clip = {0, 0, m_size.width, m_size.height};
    const int dx = std::abs(x1 - x0);
    const int sx = x0 < x1 ? 1 : -1;
    const int dy = -std::abs(y1 - y0);
    const int sy = y0 < y1 ? 1 : -1;
    int error_value = dx + dy;
    while (true) {
        pixel(x0, y0, color, opacity, clip);
        if (x0 == x1 && y0 == y1) break;
        const int twice = 2 * error_value;
        if (twice >= dy) { error_value += dy; x0 += sx; }
        if (twice <= dx) { error_value += dx; y0 += sy; }
    }
}

void Canvas::coverage(Rect area, std::span<const uint8_t> data, Color color,
                      uint8_t opacity, Rect clip) {
    const Rect screen{0, 0, m_size.width, m_size.height};
    const Rect bounds = area.clipped(clip.empty() ? screen : clip).clipped(screen);
    if (bounds.empty() || opacity == 0 || color.alpha() == 0) return;
    const int row_bytes = (area.width + 1) / 2;
    for (int y = bounds.y; y < bounds.y + bounds.height; ++y) {
        const uint8_t* row = data.data() + std::size_t(y - area.y) * row_bytes;
        for (int x = bounds.x; x < bounds.x + bounds.width; ++x) {
            const std::size_t packed = std::size_t(x - area.x) / 2;
            const uint8_t byte = row[packed];
            const uint8_t value = (x - area.x) % 2 == 0 ? byte >> 4U : byte & 0x0fU;
            if (value == 0) continue;
            pixel(x, y, color, coverage_alpha[value], clip.empty() ? screen : clip);
        }
    }
}

void Canvas::text(int x, int y, std::string_view utf8, int font_size, Color color,
                  uint8_t opacity, Rect clip, int font_weight) {
    const fonts::Face& face = fonts::face_for(font_size, font_weight);
    std::size_t previous = fonts::glyph_count;
    for (std::size_t offset = 0; offset < utf8.size();) {
        const auto character = next_utf8(utf8, offset);
        offset += character.length;
        const uint32_t codepoint = character.codepoint == '\t' ? ' ' : character.codepoint;
        const std::size_t index = fonts::glyph_index(codepoint);
        const fonts::Glyph& glyph = fonts::glyph_for(face, index);
        x += fonts::pair_advance(face, previous, index);
        if (glyph.width != 0 && glyph.height != 0) {
            const int row_bytes = (int(glyph.width) + 1) / 2;
            coverage({x + glyph.bearing_x, y + glyph.bearing_y, glyph.width, glyph.height},
                     face.coverage.subspan(glyph.bitmap_offset, std::size_t(row_bytes) * glyph.height),
                     color, opacity, clip);
        }
        x += glyph.advance;
        previous = index;
    }
}

void Canvas::paragraph(Rect area, std::string_view utf8, int font_size, Color color,
                       uint8_t opacity, Rect clip, int font_weight) {
    const fonts::Face& face = fonts::face_for(font_size, font_weight);
    if (area.empty()) return;
    const Rect text_clip = area.clipped(clip.empty() ? Rect{0, 0, m_size.width, m_size.height} : clip);
    if (text_clip.empty()) return;
    std::size_t offset = 0;
    int y = area.y;
    while (offset < utf8.size()) {
        const TextLine line = next_text_line(utf8.substr(offset), area.width, face);
        if (line.next_offset == 0) break;
        text(area.x, y, line.content, font_size, color, opacity, text_clip, font_weight);
        y += face.height;
        offset += line.next_offset;
    }
}

int Canvas::text_width(std::string_view utf8, int font_size, int font_weight) {
    const fonts::Face& face = fonts::face_for(font_size, font_weight);
    int width = 0;
    std::size_t previous = fonts::glyph_count;
    for (std::size_t offset = 0; offset < utf8.size();) {
        const auto character = next_utf8(utf8, offset);
        offset += character.length;
        const uint32_t codepoint = character.codepoint == '\t' ? ' ' : character.codepoint;
        const std::size_t current = fonts::glyph_index(codepoint);
        width += fonts::pair_advance(face, previous, current);
        width += fonts::glyph_for(face, current).advance;
        previous = current;
    }
    return width;
}

int Canvas::text_height(int font_size, int font_weight) {
    return fonts::face_for(font_size, font_weight).height;
}

Size Canvas::wrapped_text_size(std::string_view utf8, int max_width, int font_size, int font_weight) {
    return mb::wrapped_text_size(utf8, max_width, fonts::face_for(font_size, font_weight));
}

void Canvas::image(Rect target, const Asset& asset, uint8_t opacity, Rect clip) {
    const Rect screen{0, 0, m_size.width, m_size.height};
    Rect target_clip = target.clipped(clip);
    if (!clip.empty() && target_clip.empty()) return;
    if (clip.empty()) {
        clip = screen;
        target_clip = target;
    }
    target_clip = target_clip.clipped(screen);
    if (target_clip.empty() || target.width <= 0 || target.height <= 0) return;
    for (int y = target_clip.y; y < target_clip.y + target_clip.height; ++y) {
        const int source_y = int((int64_t(y - target.y) * asset.height) / target.height);
        for (int x = target_clip.x; x < target_clip.x + target_clip.width; ++x) {
            const int source_x = int((int64_t(x - target.x) * asset.width) / target.width);
            pixel(x, y, from_rgb565(asset.pixels[std::size_t(source_y) * asset.width + source_x]), opacity, clip);
        }
    }
}

Utf8Character next_utf8(std::string_view text, std::size_t offset) {
    const auto byte = uint8_t(text[offset]);
    if (byte < 0x80U) return {byte, 1};
    unsigned length = 0;
    uint32_t value = 0;
    if ((byte & 0xe0U) == 0xc0U) { length = 2; value = byte & 0x1fU; }
    else if ((byte & 0xf0U) == 0xe0U) { length = 3; value = byte & 0x0fU; }
    else if ((byte & 0xf8U) == 0xf0U) { length = 4; value = byte & 0x07U; }
    else return {'?', 1};
    if (offset + length > text.size()) return {'?', 1};
    for (unsigned index = 1; index < length; ++index) {
        const auto continuation = uint8_t(text[offset + index]);
        if ((continuation & 0xc0U) != 0x80U) return {'?', 1};
        value = (value << 6U) | (continuation & 0x3fU);
    }
    if ((length == 2 && value < 0x80U) || (length == 3 && value < 0x800U) ||
        (length == 4 && value < 0x10000U) || (value >= 0xd800U && value <= 0xdfffU) || value > 0x10ffffU) {
        return {'?', 1};
    }
    return {value, uint8_t(length)};
}

std::string_view format_value(const Value& value, std::string_view format, std::span<char> output) {
    if (output.empty()) return {};
    if (format.empty()) {
        std::size_t used = 0;
        default_value(value, {}, output, used);
        return {output.data(), used};
    }
    if (value.type == Value::Type::void_value) return {};
    std::size_t used = 0;
    std::size_t offset = 0;
    while (offset < format.size()) {
        const std::size_t percent = format.find('%', offset);
        if (percent == std::string_view::npos) {
            append_text(output, used, format.substr(offset));
            break;
        }
        append_text(output, used, format.substr(offset, percent - offset));
        if (percent + 1 < format.size() && format[percent + 1] == '%') {
            append_text(output, used, "%");
            offset = percent + 2;
            continue;
        }
        std::size_t end = percent + 1;
        while (end < format.size() && format[end] != 'f' && format[end] != 'd' && format[end] != 's') ++end;
        if (end == format.size()) throw std::invalid_argument("invalid binding format");
        default_value(value, std::string_view(format.substr(percent, end - percent + 1)), output, used);
        offset = end + 1;
    }
    return {output.data(), used};
}

std::string format_value(const Value& value, std::string_view format) {
    std::array<char, 256> buffer{};
    const auto result = format_value(value, format, buffer);
    return std::string(result);
}

} // namespace mb
