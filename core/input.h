#pragma once

#include "microbrowser/canvas.h"

#include <algorithm>
#include <array>
#include <cstddef>

namespace mb {

enum class KeyAction : uint8_t { character, backspace, space, enter };
struct KeyboardKey {
    Rect bounds;
    KeyAction action = KeyAction::character;
    char character = 0;
    std::array<char, 2> label{};
};
struct KeyboardKeys {
    std::array<KeyboardKey, 40> entries{};
    std::size_t count = 0;
};

inline int keyboard_height(Size screen) {
    return std::clamp(screen.height * 44 / 100, 100, 160);
}

inline KeyboardKeys keyboard_keys(Size screen) {
    KeyboardKeys result;
    const int top = screen.height - keyboard_height(screen);
    const int row_height = keyboard_height(screen) / 4;
    static constexpr std::array<std::string_view, 3> rows{"1234567890", "qwertyuiop", "asdfghjkl<"};
    for (std::size_t row = 0; row < rows.size(); ++row) {
        for (int column = 0; column < 10; ++column) {
            KeyboardKey key{{column * screen.width / 10, top + int(row) * row_height,
                             (column + 1) * screen.width / 10 - column * screen.width / 10, row_height - 2}};
            const char ch = rows[row][column];
            if (ch == '<') {
                key.action = KeyAction::backspace;
                key.label = {'<', '\0'};
            } else {
                key.character = ch;
                key.label = {ch, '\0'};
            }
            result.entries[result.count++] = key;
        }
    }
    const int unit = screen.width / 16;
    for (int index = 0; index < 10; ++index) {
        KeyboardKey key{{0, top + 3 * row_height, unit, row_height - 2}};
        if (index < 8) {
            key.bounds.x = index * unit;
            key.character = std::string_view("zxcvbnm,.")[index];
            key.label[0] = key.character;
        } else if (index == 8) {
            key.bounds = {8 * unit, top + 3 * row_height, 4 * unit, row_height - 2};
            key.action = KeyAction::space;
        } else {
            key.bounds = {12 * unit, top + 3 * row_height, screen.width - 12 * unit, row_height - 2};
            key.action = KeyAction::enter;
        }
        result.entries[result.count++] = key;
    }
    return result;
}

struct TextWindow {
    std::string_view text;
    std::size_t begin_byte;
    int caret_pixels;
};

[[nodiscard]] TextWindow input_window(std::string_view value, std::size_t caret_byte, int capacity,
                                      int font_size, int font_weight);
[[nodiscard]] std::size_t caret_byte_at_x(std::string_view value, int pixel,
                                         int font_size, int font_weight);

[[nodiscard]] inline Rect metrics_bounds(Size screen) {
    const int width = std::min(208, screen.width);
    return {screen.width - width, 4, width, std::min(76, screen.height)};
}

} // namespace mb
