#include "paint.h"

#include "input.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace mb {
namespace {

Color pressed_color(Color color) {
    constexpr unsigned scale = 224;
    return Color::rgba(uint8_t(unsigned(color.red()) * scale / 255U),
                       uint8_t(unsigned(color.green()) * scale / 255U),
                       uint8_t(unsigned(color.blue()) * scale / 255U), color.alpha());
}

} // namespace

Rect visual_bounds(const Node& node) {
    const int width = std::max(0, int(float(node.box.width) * node.scale));
    const int height = std::max(0, int(float(node.box.height) * node.scale));
    return {node.box.x + (node.box.width - width) / 2,
            node.box.y + (node.box.height - height) / 2 + int(node.offset_y), width, height};
}

void Painter::content_box(const Node& node, Rect area, float number, Color color,
                          uint8_t opacity, Rect clip) {
    const float fraction = node.maximum > node.minimum
        ? std::clamp((number - node.minimum) / (node.maximum - node.minimum), 0.0F, 1.0F) : 0.0F;
    const int radius = node.kind == NodeKind::slider ? area.height / 2 : node.style.radius;
    canvas.fill_rounded(area, radius, palette::border, opacity, clip);
    canvas.fill_rounded({area.x, area.y, int(area.width * fraction), area.height},
                        radius, color, opacity, clip);
}

void Painter::chart(const Node& node, Rect area, uint8_t opacity, Rect clip) {
    canvas.fill_rounded(area, node.style.radius, node.style.background, opacity, clip);
    canvas.stroke_rounded(area, node.style.radius, palette::border, opacity, clip);
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

void Painter::button(const Node& node, Rect area, uint8_t opacity, Rect clip) {
    const auto& style = node.style;
    const bool pressed = &node == browser.m_pointer_target && browser.m_pointer_down;
    const Color background = pressed ? pressed_color(style.background) : style.background;
    canvas.fill_rounded(area, style.radius, background, opacity, clip);
    canvas.stroke_rounded(area, style.radius, style.border, opacity, clip);
    const int width = Canvas::text_width(node.text, style.font_size, style.font_weight);
    canvas.text(area.x + std::max(style.padding, (area.width - width) / 2),
                area.y + (area.height - Canvas::text_height(style.font_size, style.font_weight)) / 2,
                node.text, style.font_size, style.foreground, opacity, clip, style.font_weight);
}

void Painter::input(const Node& node, Rect area, uint8_t opacity, Rect clip) {
    const auto& style = node.style;
    const bool focused = &node == browser.m_focus;
    const Color background = style.background.alpha() == 0 ? palette::surface : style.background;
    canvas.fill_rounded(area, style.radius, background, opacity, clip);
    canvas.stroke_rounded(area, style.radius, focused ? palette::blue : style.border, opacity, clip);
    const std::string_view value = browser.m_state[node.binding].string;
    const TextWindow window = input_window(value, browser.m_cursor_byte,
        std::max(0, area.width - 16), style.font_size, style.font_weight);
    const std::string_view shown = value.empty() ? std::string_view(node.placeholder) : window.text;
    const Color foreground = value.empty() ? palette::muted : style.foreground;
    const Rect text_clip = area.clipped(clip);
    const int text_height = Canvas::text_height(style.font_size, style.font_weight);
    canvas.text(area.x + 8, area.y + (area.height - text_height) / 2, shown,
                style.font_size, foreground, opacity, text_clip, style.font_weight);
    if (focused && browser.m_cursor_visible && area.width > 18) {
        const int x = std::min(area.x + 8 + window.caret_pixels, area.x + area.width - 5);
        const int y = area.y + std::max(4, (area.height - text_height) / 2);
        canvas.fill({x, y, 2, std::min(text_height, area.height - 8)}, palette::blue, opacity, text_clip);
    }
}

void Painter::text(const Node& node, Rect area, uint8_t opacity, Rect clip) {
    std::array<char, 512> buffer{};
    const std::string_view content = node.binding == no_index ? std::string_view(node.text)
        : format_value(browser.m_state[node.binding], node.format, buffer);
    const auto& style = node.style;
    Rect text_area{area.x + style.padding, area.y + style.padding,
                   std::max(0, area.width - 2 * style.padding),
                   std::max(0, area.height - 2 * style.padding)};
    const Size text_size = Canvas::wrapped_text_size(content, text_area.width, style.font_size, style.font_weight);
    text_area.y += std::max(0, (text_area.height - text_size.height) / 2);
    canvas.paragraph(text_area, content, style.font_size, style.foreground, opacity, clip, style.font_weight);
}

void Painter::draw(const Node& node, uint8_t parent_opacity, Rect clip) {
    if (clip.empty()) return;
    const float rounded_opacity = std::clamp(node.opacity, 0.0F, 1.0F) * parent_opacity + 0.5F;
    const uint8_t opacity = uint8_t(std::clamp(rounded_opacity, 0.0F, 255.0F));
    if (opacity == 0) return;
    const Rect area = visual_bounds(node);
    if (node.kind == NodeKind::button) {
        button(node, area, opacity, clip);
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
        input(node, area, opacity, clip);
        return;
    }
    if (node.kind == NodeKind::slider) {
        const Rect track{area.x + 8, area.y + (area.height - 6) / 2, std::max(0, area.width - 16), 6};
        const float value = browser.m_state[node.binding].number;
        content_box(node, track, value, node.style.foreground, opacity, clip);
        const float fraction = node.maximum > node.minimum
            ? std::clamp((value - node.minimum) / (node.maximum - node.minimum), 0.0F, 1.0F) : 0.0F;
        const int center = track.x + int(track.width * fraction);
        const Rect thumb{center - 7, area.y + (area.height - 18) / 2, 14, 18};
        canvas.fill_rounded(thumb, 7, palette::white, opacity, clip);
        canvas.stroke_rounded(thumb, 7, node.style.foreground, opacity, clip);
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
    if (node.style.border.alpha() != 0) canvas.stroke_rounded(area, node.style.radius, node.style.border, opacity, clip);
    if (node.kind == NodeKind::text) text(node, area, opacity, clip);
    draw_children(node, opacity, clip);
    if (node.style.scroll && node.scroll_max > 0) scrollbar(node, area, opacity, clip);
}

void Painter::scrollbar(const Node& node, Rect area, uint8_t opacity, Rect clip) {
    if (node.kind == NodeKind::row || !node.style.scroll || node.scroll_max <= 0) return;
    const Rect track{area.x + area.width - 2, area.y, 2, area.height};
    canvas.fill(track, palette::border, opacity / 2, clip);
    const int height = std::max(18, int(int64_t(area.height) * area.height /
        (area.height + int(node.scroll_max))));
    const float progress = std::clamp(node.scroll / node.scroll_max, 0.0F, 1.0F);
    const int offset = int(progress * float(area.height - height));
    canvas.fill_rounded({area.x + area.width - 3, area.y + offset, 3, height}, 2,
                        palette::muted, opacity, clip);
}

void Painter::run(Rect clip) {
    if (browser.m_page == nullptr || clip.empty()) return;
    canvas.fill(clip, palette::background, 255, clip);
    draw(browser.m_page->root, 255, clip);
    overlay(clip);
}

} // namespace mb
