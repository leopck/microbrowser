#pragma once

#include "microbrowser/canvas.h"
#include "fonts.h"

#include <cstddef>
#include <string_view>

namespace mb {

struct TextLine {
    std::string_view content;
    std::size_t next_offset;
    int width;
};

[[nodiscard]] TextLine next_text_line(std::string_view text, int max_width,
                                      const fonts::Face& face);
[[nodiscard]] Size wrapped_text_size(std::string_view text, int max_width,
                                     const fonts::Face& face);

} // namespace mb
