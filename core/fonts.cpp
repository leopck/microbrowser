#include "fonts.h"

#include <algorithm>
#include <stdexcept>

namespace mb::fonts {

bool face_exists(int pixel_size, int weight) noexcept {
    return std::any_of(faces.begin(), faces.end(), [=](const Face& face) {
        return face.pixel_size == pixel_size && face.weight == weight;
    });
}

const Face& face_for(int pixel_size, int weight) {
    const auto match = std::find_if(faces.begin(), faces.end(), [=](const Face& face) {
        return face.pixel_size == pixel_size && face.weight == weight;
    });
    if (match == faces.end()) throw std::invalid_argument("requested font size and weight are not built in");
    return *match;
}

const Glyph& glyph_for(const Face& face, std::size_t index) {
    if (index >= face.glyphs.size()) throw std::out_of_range("glyph index exceeds its font table");
    return face.glyphs[index];
}

int pair_advance(const Face& face, std::size_t previous, std::size_t current) noexcept {
    if (previous == glyph_count) return 0;
    const auto match = std::lower_bound(face.kerning.begin(), face.kerning.end(),
        Pair{static_cast<uint8_t>(previous), static_cast<uint8_t>(current), 0},
        [](const Pair& left, const Pair& right) {
            if (left.previous != right.previous) return left.previous < right.previous;
            return left.current < right.current;
        });
    if (match == face.kerning.end() || match->previous != previous || match->current != current) return 0;
    return match->advance;
}

} // namespace mb::fonts
