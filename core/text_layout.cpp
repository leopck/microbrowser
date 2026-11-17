#include "text_layout.h"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace {
constexpr std::size_t no_break = std::numeric_limits<std::size_t>::max();

std::size_t skip_spaces(std::string_view text, std::size_t offset) {
    while (offset < text.size() && (text[offset] == ' ' || text[offset] == '\t')) ++offset;
    return offset;
}
}

namespace mb {

TextLine next_text_line(std::string_view text, int max_width, const fonts::Face& face) {
    const std::size_t line_start = skip_spaces(text, 0);
    std::size_t cursor = line_start;
    std::size_t last_space_end = no_break;
    std::size_t after_last_space = no_break;
    std::size_t previous = fonts::glyph_count;
    int width = 0;
    int width_before_space = 0;
    while (cursor < text.size()) {
        const auto character = next_utf8(text, cursor);
        if (character.codepoint == '\n') {
            return {text.substr(line_start, cursor - line_start), cursor + character.length, width};
        }
        const uint32_t codepoint = character.codepoint == '\t' ? ' ' : character.codepoint;
        const std::size_t current = fonts::glyph_index(codepoint);
        const auto& glyph = fonts::glyph_for(face, current);
        const int next_width = width + fonts::pair_advance(face, previous, current) + glyph.advance;
        if (next_width > max_width && cursor > line_start) {
            if (last_space_end != no_break) {
                return {text.substr(line_start, last_space_end - line_start), skip_spaces(text, after_last_space), width_before_space};
            }
            return {text.substr(line_start, cursor - line_start), cursor, width};
        }
        width = next_width;
        cursor += character.length;
        previous = current;
        if (codepoint == ' ') {
            last_space_end = cursor - character.length;
            width_before_space = width - glyph.advance;
            after_last_space = cursor;
        }
    }
    return {text.substr(line_start), text.size(), width};
}

Size wrapped_text_size(std::string_view text, int max_width, const fonts::Face& face) {
    if (text.empty() || max_width <= 0) return {};
    std::size_t offset = 0;
    int lines = 0;
    int width = 0;
    while (offset < text.size()) {
        const TextLine line = next_text_line(text.substr(offset), max_width, face);
        if (line.next_offset == 0) break;
        width = std::max(width, std::min(max_width, line.width));
        offset += line.next_offset;
        ++lines;
    }
    return {width, lines * int(face.height)};
}

} // namespace mb
