#pragma once

#include "font_data.h"

namespace mb::fonts {

[[nodiscard]] const Face& face_for(int pixel_size, int weight = 400);
// Returns the original packed rows, using caller-owned storage when expanding.
[[nodiscard]] std::span<const uint8_t> decode_glyph_coverage(
    const Face& face, const Glyph& glyph, std::span<uint8_t> scratch);
[[nodiscard]] const Glyph& glyph_for(const Face& face, std::size_t index);
[[nodiscard]] int pair_advance(const Face& face, std::size_t previous, std::size_t current) noexcept;
[[nodiscard]] bool face_exists(int pixel_size, int weight = 400) noexcept;
} // namespace mb::fonts
