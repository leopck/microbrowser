#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace mb::fonts {

enum class CoverageEncoding : uint8_t { packed_nibbles = 0, byte_runs = 1 };

struct Glyph {
  // Byte offset to this glyph's data within Face::coverage.
  uint32_t bitmap_offset;
  uint8_t width;
  uint8_t height;
  uint8_t advance;
  int8_t bearing_x;
  int8_t bearing_y;
  CoverageEncoding coverage_encoding = CoverageEncoding::packed_nibbles;
};
struct Face {
  uint16_t pixel_size;
  uint16_t weight;
  uint8_t height;
  std::span<const Glyph> glyphs;
  std::span<const uint8_t> coverage;
  // Seven bits per glyph index and four bits for advance, biased by eight.
  // Records occupy 18 bits and retain the original pair ordering.
  std::span<const uint8_t> kerning;
};
extern const std::array<Face, 12> faces;
constexpr std::size_t glyph_count = 101;
std::size_t glyph_index(uint32_t codepoint) noexcept;

} // namespace mb::fonts
