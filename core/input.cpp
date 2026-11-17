#include "input.h"

#include "fonts.h"

#include <algorithm>
#include <cstdint>

namespace {
int glyph_advance(const mb::fonts::Face& face, std::size_t previous, std::size_t current) {
    return mb::fonts::pair_advance(face, previous, current) + mb::fonts::glyph_for(face, current).advance;
}

std::size_t glyph_index(uint32_t codepoint) {
    return mb::fonts::glyph_index(codepoint == '\t' ? ' ' : codepoint);
}

std::size_t last_visible_byte(std::string_view value, std::size_t begin, int width,
                              const mb::fonts::Face& face) {
    if (width <= 0) return begin;
    std::size_t previous = mb::fonts::glyph_count;
    int used = 0;
    std::size_t offset = begin;
    while (offset < value.size()) {
        const auto character = mb::next_utf8(value, offset);
        const std::size_t current = glyph_index(character.codepoint);
        const int advance = glyph_advance(face, previous, current);
        if (offset > begin && used + advance > width) break;
        used += advance;
        previous = current;
        offset += character.length;
    }
    return offset;
}
}

namespace mb {

TextWindow input_window(std::string_view value, std::size_t caret_byte, int capacity,
                        int font_size, int font_weight) {
    if (capacity <= 0) return {{}, 0, 0};
    const fonts::Face& face = fonts::face_for(font_size, font_weight);
    const std::size_t caret = std::min(caret_byte, value.size());
    int remaining = Canvas::text_width(value.substr(0, caret), font_size, font_weight);
    std::size_t previous = fonts::glyph_count;
    std::size_t begin = 0;
    while (begin < caret && remaining > capacity) {
        const auto character = next_utf8(value, begin);
        const std::size_t current = glyph_index(character.codepoint);
        remaining -= glyph_advance(face, previous, current);
        previous = current;
        begin += character.length;
    }
    const std::size_t end = last_visible_byte(value, begin, capacity, face);
    const int caret_pixels = Canvas::text_width(value.substr(begin, caret - begin), font_size, font_weight);
    return {value.substr(begin, end - begin), begin, caret_pixels};
}

std::size_t caret_byte_at_x(std::string_view value, int pixel, int font_size, int font_weight) {
    if (pixel <= 0) return 0;
    const fonts::Face& face = fonts::face_for(font_size, font_weight);
    std::size_t previous = fonts::glyph_count;
    int position = 0;
    for (std::size_t offset = 0; offset < value.size();) {
        const auto character = next_utf8(value, offset);
        const std::size_t current = glyph_index(character.codepoint);
        const int advance = glyph_advance(face, previous, current);
        if (pixel < position + (advance + 1) / 2) return offset;
        position += advance;
        previous = current;
        offset += character.length;
    }
    return value.size();
}

} // namespace mb
