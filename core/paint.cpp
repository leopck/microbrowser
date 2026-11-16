#include "paint.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace mb {
namespace {

struct TextWindow {
    std::string_view text;
    int caret_pixels = 0;
};

std::size_t advance_codepoints(std::string_view text, std::size_t offset, int count) {
    for (int index = 0; index < count && offset < text.size(); ++index) {
        offset += next_utf8(text, offset).length;
    }
    return offset;
}

TextWindow input_window(const std::string& value, std::size_t caret_byte, int capacity, int font_size) {
    const int total = static_cast<int>(std::count_if(value.begin(), value.end(), [](char ch) {
        return (static_cast<unsigned char>(ch) & 0xc0U) != 0x80U;
    }));
    const std::size_t caret = std::min(caret_byte, value.size());
    int before_caret = 0;
    for (std::size_t offset = 0; offset < caret;) {
        offset += next_utf8(value, offset).length;
        ++before_caret;
    }
    const int skip = std::max(0, std::max(before_caret + 1 - capacity, total - capacity));
    const std::size_t begin = advance_codepoints(value, 0, skip);
    const std::string_view shown = std::string_view(value).substr(begin);
    return {shown, Canvas::text_width(std::string_view(value).substr(begin, caret - begin), font_size)};
}

} // namespace

Rect visual_bounds(const Node& node) {
    const int width = std::max(0, int(float(node.box.width) * node.scale));
    const int height = std::max(0, int(float(node.box.height) * node.scale));
    return {node.box.x + (node.box.width - width) / 2,
            node.box.y + (node.box.height - height) / 2 + int(node.offset_y), width, height};
}

void Painter::content_box(const Node& node, Rect area, float number, Color color, uint8_t opacity, Rect clip) {
    const float fraction = node.maximum > node.minimum
        ? std::clamp((number - node.minimum) / (node.maximum - node.minimum), 0.0F, 1.0F) : 0.0F;
    canvas.fill_rounded(area, node.style.radius, palette::border, opacity, clip);
    canvas.fill_rounded({area.x, area.y, int(area.width * fraction), area.height},
                        node.style.radius, color, opacity, clip);
}

void Painter::chart(const Node& node, Rect area, uint8_t opacity, Rect clip) {
    canvas.fill_rounded(area, node.style.radius, node.style.background, opacity, clip);
    canvas.stroke(area, palette::border, opacity, clip);
    const Rect plot = area.clipped(clip);
    if (plot.empty()) return;
    for (int row = 1; row < 4; ++row) {
        const int y = area.y + area.height * row / 4;
        canvas.line(area.x + 1, y, area.x + area.width - 2, y, palette::border, opacity / 2, plot);
    }
    if (node.sample_count == 0) return;
    const uint8_t oldest = node.sample_count == max_chart_samples ? node.sample_head : 0;
    int previous_x = 0;
    int previous_y = 0;
    for (uint8_t index = 0; index < node.sample_count; ++index) {
        const uint8_t sample = uint8_t((int(oldest) + int(index)) % int(max_chart_samples));
        const float fraction = node.maximum > node.minimum
            ? std::clamp((node.samples[sample] - node.minimum) / (node.maximum - node.minimum), 0.0F, 1.0F) : 0.0F;
        const int x = area.x + 2 + (node.sample_count == 1 ? (area.width - 4) / 2
            : int(index) * (area.width - 4) / (int(node.sample_count) - 1));
        const int y = area.y + area.height - 2 - int(fraction * float(area.height - 4));
        if (index == 0) canvas.fill({x - 1, y - 1, 3, 3}, node.style.foreground, opacity, plot);
        else canvas.line(previous_x, previous_y, x, y, node.style.foreground, opacity, plot);
        previous_x = x;
        previous_y = y;
    }
}

void Painter::draw(const Node& node, uint8_t parent_opacity, Rect clip) {
    if (clip.empty()) return;
    const float rounded_opacity = std::clamp(node.opacity, 0.0F, 1.0F) * parent_opacity + 0.5F;
    const uint8_t opacity = uint8_t(std::clamp(rounded_opacity, 0.0F, 255.0F));
    if (opacity == 0) return;
    const Rect area = visual_bounds(node);
    if (node.kind == NodeKind::button) {
        const bool pressed = &node == browser.m_pointer_target && browser.m_pointer_down;
        canvas.fill_rounded(area, node.style.radius, pressed ? palette::accent : node.style.background, opacity, clip);
        canvas.stroke(area, node.style.border, opacity, clip);
        const int text_width = Canvas::text_width(node.text, node.style.font_size);
        canvas.text(area.x + std::max(node.style.padding, (area.width - text_width) / 2),
                    area.y + (area.height - Canvas::text_height(node.style.font_size)) / 2,
                    node.text, node.style.font_size, palette::ink, opacity, clip);
        return;
    }
    if (node.kind == NodeKind::image) {
        const auto asset = std::find_if(browser.m_package.assets.begin(), browser.m_package.assets.end(),
            [&](const Asset& candidate) { return candidate.name == node.source; });
        if (asset == browser.m_package.assets.end()) throw PackageError("rendering references a missing image asset");
        canvas.image(area, *asset, opacity, clip.clipped(area));
        return;
    }
    if (node.kind == NodeKind::input) {
        canvas.fill_rounded(area, node.style.radius, palette::background, opacity, clip);
        const bool focused = &node == browser.m_focus;
        canvas.stroke(area, focused ? palette::accent : node.style.border, opacity, clip);
        const int capacity = std::max(0, (area.width - 16) / (6 * node.style.font_size));
        const TextWindow window = input_window(browser.m_state[node.binding].string, browser.m_cursor_byte,
                                               capacity, node.style.font_size);
        const std::string_view shown = window.text.empty()
            ? std::string_view(node.placeholder) : window.text;
        canvas.text(area.x + 8, area.y + (area.height - 7 * node.style.font_size) / 2, shown,
                    node.style.font_size, window.text.empty() ? palette::muted : node.style.foreground, opacity, clip);
        if (focused && browser.m_cursor_visible && area.width > 18) {
            const int x = std::min(area.x + 8 + window.caret_pixels, area.x + area.width - 5);
            canvas.fill({x, area.y + 6, 2, std::max(1, area.height - 12)}, palette::accent, opacity, clip);
        }
        return;
    }
    if (node.kind == NodeKind::slider) {
        const Rect track{area.x + 8, area.y + (area.height - 6) / 2, std::max(0, area.width - 16), 6};
        const float value = browser.m_state[node.binding].number;
        content_box(node, track, value, node.style.foreground, opacity, clip);
        const float fraction = node.maximum > node.minimum
            ? std::clamp((value - node.minimum) / (node.maximum - node.minimum), 0.0F, 1.0F) : 0.0F;
        const int center = track.x + int(track.width * fraction);
        canvas.fill_rounded({center - 6, area.y + (area.height - 16) / 2, 12, 16}, 6,
                            palette::white, opacity, clip);
        return;
    }
    if (node.kind == NodeKind::progress) {
        content_box(node, area, browser.m_state[node.binding].number, node.style.foreground, opacity, clip);
        return;
    }
    if (node.kind == NodeKind::chart) {
        chart(node, area, opacity, clip);
        return;
    }
    canvas.fill_rounded(area, node.style.radius, node.style.background, opacity, clip);
    if (node.style.border.alpha() != 0) canvas.stroke(area, node.style.border, opacity, clip);
    if (node.kind == NodeKind::text) {
        std::array<char, 512> buffer{};
        const std::string_view content = node.binding == no_index ? std::string_view(node.text)
            : format_value(browser.m_state[node.binding], node.format, buffer);
        const int y = area.y + node.style.padding +
            std::max(0, (area.height - 2 * node.style.padding - Canvas::text_height(node.style.font_size)) / 2);
        canvas.text(area.x + node.style.padding, y, content, node.style.font_size,
                    node.style.foreground, opacity, clip);
    }
    draw_children(node, opacity, clip);
}

void Painter::run(Rect clip) {
    if (browser.m_page == nullptr || clip.empty()) return;
    canvas.fill(clip, palette::background, 255, clip);
    draw(browser.m_page->root, 255, clip);
    overlay(clip);
}

} // namespace mb
