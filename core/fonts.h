#pragma once

#include "font_data.h"

namespace mb::fonts {

[[nodiscard]] const Face& face_for(int pixel_size, int weight = 400);
[[nodiscard]] const Glyph& glyph_for(const Face& face, std::size_t index);
[[nodiscard]] int pair_advance(const Face& face, std::size_t previous, std::size_t current) noexcept;
[[nodiscard]] bool face_exists(int pixel_size, int weight = 400) noexcept;
} // namespace mb::fonts
