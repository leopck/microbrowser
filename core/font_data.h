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
  // Start indices of each previous glyph's sorted kerning entries.
  std::span<const uint16_t> kerning_rows;
  // Each entry contains seven bits for the current glyph and four bits for
  // advance, biased by eight. Entries occupy 11 bits in pair-index order.
  std::span<const uint8_t> kerning;
};
extern const std::array<Face, 12> faces;
constexpr std::size_t glyph_count = 101;
std::size_t glyph_index(uint32_t codepoint) noexcept;

} // namespace mb::fonts
