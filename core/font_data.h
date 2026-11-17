#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace mb::fonts {

struct Glyph {
  uint32_t bitmap_offset;
  uint8_t width;
  uint8_t height;
  uint8_t advance;
  int8_t bearing_x;
  int8_t bearing_y;
};
struct Pair { uint8_t previous; uint8_t current; int8_t advance; };
struct Face {
  uint16_t pixel_size;
  uint16_t weight;
  uint8_t height;
  std::span<const Glyph> glyphs;
  std::span<const uint8_t> coverage;
  std::span<const Pair> kerning;
};
extern const std::array<Face, 12> faces;
constexpr std::size_t glyph_count = 101;
std::size_t glyph_index(uint32_t codepoint) noexcept;

} // namespace mb::fonts
