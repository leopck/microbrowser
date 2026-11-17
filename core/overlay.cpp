#include "paint.h"
#include "input.h"

#include <array>
#include <cstdio>
#include <cstring>

namespace mb {
namespace {
constexpr int overlay_font_size = 14;
}

void Painter::draw_children(const Node& node, uint8_t opacity, Rect clip) {
    const Rect child_clip = node.style.scroll ? clip.clipped(node.box) : clip;
    if (child_clip.empty()) return;
    for (const auto& child : node.children) draw(*child, opacity, child_clip);
}

void Painter::keyboard(Rect clip) {
    const Rect panel{0, browser.m_size.height - keyboard_height(browser.m_size),
                     browser.m_size.width, keyboard_height(browser.m_size)};
    canvas.fill(panel, palette::background, 255, clip);
    canvas.line(0, panel.y, browser.m_size.width, panel.y, palette::border, 255, clip);
    const KeyboardKeys keys = keyboard_keys(browser.m_size);
    for (std::size_t index = 0; index < keys.count; ++index) {
        const KeyboardKey& key = keys.entries[index];
        const bool action = key.action == KeyAction::space || key.action == KeyAction::enter;
        const Color foreground = action ? palette::white : palette::ink;
        const Color background = action ? palette::blue
            : key.action == KeyAction::character ? palette::surface : palette::border;
        canvas.fill_rounded(key.bounds, 5, background, 255, clip);
        std::string_view label(key.label.data());
        if (key.action == KeyAction::space) label = "space";
        if (key.action == KeyAction::enter) label = "enter";
        const int x = key.bounds.x + (key.bounds.width - Canvas::text_width(label, overlay_font_size, 600)) / 2;
        const int y = key.bounds.y + (key.bounds.height - Canvas::text_height(overlay_font_size, 600)) / 2;
        canvas.text(x, y, label, overlay_font_size, foreground, 255, clip, 600);
    }
}

void Painter::overlay(Rect clip) {
    if (browser.m_show_keyboard) keyboard(clip);
    const int footer = browser.m_show_keyboard ? keyboard_height(browser.m_size) : 0;
    if (browser.m_error_until > browser.m_platform.now_ms() && !browser.m_error.empty()) {
        const Rect banner{0, browser.m_size.height - footer - 24, browser.m_size.width, 24};
        canvas.fill(banner, Color::rgba(188, 45, 62, 245), 255, clip);
        const int y = banner.y + (banner.height - Canvas::text_height(overlay_font_size)) / 2;
        canvas.text(8, y, browser.m_error, overlay_font_size, palette::white, 255, clip);
    }
    if (!browser.m_show_stats) return;
    const Metrics& metrics = browser.m_metrics;
    const Rect panel = metrics_bounds(browser.m_size);
    canvas.fill_rounded(panel, 10, Color::rgba(31, 41, 59, 240), 255, clip);
    std::array<char, 160> text{};
    std::snprintf(text.data(), text.size(), "Host paint %.2f ms\nLayout %.2f ms\n%llu nodes  %u requests\nF1: hide metrics",
        double(metrics.last_render_us) / 1000.0, double(metrics.last_layout_us) / 1000.0,
        static_cast<unsigned long long>(metrics.node_count), static_cast<unsigned int>(metrics.active_requests));
    int y = panel.y + 7;
    std::size_t begin = 0;
    const std::size_t length = std::strlen(text.data());
    for (std::size_t end = 0; end <= length; ++end) {
        if (text[end] != '\n' && text[end] != '\0') continue;
        canvas.text(panel.x + 8, y, std::string_view(text.data() + begin, end - begin),
                    overlay_font_size, palette::white, 255, clip);
        y += Canvas::text_height(overlay_font_size);
        begin = end + 1;
    }
}

} // namespace mb
