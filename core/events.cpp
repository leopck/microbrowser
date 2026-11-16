#include "microbrowser/browser.h"
#include "input.h"
#include "paint.h"

#include <algorithm>
#include <cmath>

#include <string>
namespace mb {
namespace {

constexpr int kScrollDragThresholdPixels = 8;

void set_scroll_offset(Node& node, float offset) {
    node.scroll = std::clamp(offset, 0.0F, node.scroll_max);
}
bool interactive(const Node& node) {
    return node.kind == NodeKind::button || node.kind == NodeKind::input || node.kind == NodeKind::slider;
}

Node* hit_node(Node& node, int x, int y, Rect clip) {
    const Rect area = visual_bounds(node);
    const Rect child_clip = node.style.scroll ? clip.clipped(node.box) : clip;
    for (auto child = node.children.rbegin(); child != node.children.rend(); ++child) {
        if (Node* result = hit_node(**child, x, y, child_clip)) return result;
    }
    return interactive(node) && area.contains(x, y) && clip.contains(x, y) ? &node : nullptr;
}

Node* scroll_node(Node& node, int x, int y) {

    if (!node.box.contains(x, y)) return nullptr;
    for (auto child = node.children.rbegin(); child != node.children.rend(); ++child) {
        if (Node* result = scroll_node(**child, x, y)) return result;
    }
    return node.style.scroll ? &node : nullptr;
}

std::size_t previous_character(std::string_view text, std::size_t offset) {
    if (offset == 0) return 0;
    std::size_t previous = 0;
    for (std::size_t current = 0; current < offset;) {
        previous = current;
        current += next_utf8(text, current).length;
    }
    return previous;
}

std::size_t character_count(std::string_view text) {
    std::size_t count = 0;
    for (std::size_t offset = 0; offset < text.size();) {
        offset += next_utf8(text, offset).length;
        ++count;
    }
    return count;
}

bool valid_input(std::string_view text) {
    for (std::size_t offset = 0; offset < text.size();) {
        const Utf8Character character = next_utf8(text, offset);
        if (character.codepoint < 32 || character.codepoint == 127) return false;
        if (character.codepoint == '?' && text.substr(offset, character.length) != "?") return false;
        offset += character.length;
    }
    return true;
}

} // namespace

Node* Browser::hit_test(int x, int y) {
    if (m_layout_dirty) perform_layout();
    return m_page == nullptr ? nullptr : hit_node(m_page->root, x, y, {0, 0, m_size.width, m_size.height});
}

void Browser::edit_value(Node& node, float x_normalized) {
    if (node.kind != NodeKind::slider || node.binding == no_index) return;
    const float fraction = std::clamp(x_normalized, 0.0F, 1.0F);
    float value = node.minimum + fraction * (node.maximum - node.minimum);
    if (node.step > 0) value = node.minimum + std::round((value - node.minimum) / node.step) * node.step;
    value = std::clamp(value, node.minimum, node.maximum);
    if (m_state[node.binding].number == value) return;
    write_state(node.binding, Value::from_number(value));
    changed(node);
}

void Browser::changed(Node& node) {
    if (node.on_change != no_index) invoke(node.on_change);
}

void Browser::cancel_pointer_gesture() {
    m_pointer_down = false;
    m_pointer_target = nullptr;
    m_scroll_dragging = false;
    m_scroll_target = nullptr;
    m_scroll_start_y = 0;
    m_scroll_origin = 0.0F;
}

void Browser::apply_scroll_drag() {
    const float delta_y = float(m_pointer_y) - float(m_scroll_start_y);
    set_scroll_offset(*m_scroll_target, m_scroll_origin - delta_y);
    invalidate_layout();
}

void Browser::pointer_down(int x, int y) {
    cancel_pointer_gesture();
    m_pointer_x = x;
    m_pointer_y = y;
    m_pointer_down = true;
    if (m_show_keyboard) {
        const Rect panel{0, m_size.height - keyboard_height(m_size), m_size.width, keyboard_height(m_size)};
        if (panel.contains(x, y)) {
            const KeyboardKeys keys = keyboard_keys(m_size);
            for (std::size_t index = 0; index < keys.count; ++index) {
                if (!keys.entries[index].bounds.contains(x, y) || m_focus == nullptr) continue;
                switch (keys.entries[index].action) {
                case KeyAction::character: text_input(std::string_view(&keys.entries[index].character, 1)); break;
                case KeyAction::space: text_input(" "); break;
                case KeyAction::backspace: key(Key::backspace); break;
                case KeyAction::enter: key(Key::enter); break;
                }
                return;
            }
            return;
        }
    }
    Node* target = hit_test(x, y);
    m_scroll_target = m_page == nullptr ? nullptr : scroll_node(m_page->root, x, y);
    m_scroll_origin = m_scroll_target == nullptr ? 0.0F : m_scroll_target->scroll;
    m_scroll_start_y = y;
    m_pointer_target = target;
    if (target == nullptr) return;
    if (target->kind == NodeKind::button) {
        invalidate(*target);
        return;
    }
    if (target->kind == NodeKind::slider) {
        m_focus = nullptr;
        m_show_keyboard = false;
        edit_value(*target, float(x - target->box.x - 8) / float(std::max(1, target->box.width - 16)));
        invalidate_layout();
        return;
    }
    if (target->kind == NodeKind::input) {
        if (m_focus == target) {
            const int character = std::clamp((x - target->box.x - 8) / (6 * target->style.font_size),
                0, int(character_count(m_state[target->binding].string)));
            std::size_t cursor = 0;
            for (int index = 0; index < character; ++index) {
                cursor += next_utf8(m_state[target->binding].string, cursor).length;
            }
            m_cursor_byte = cursor;
        } else {
            m_cursor_byte = m_state[target->binding].string.size();
        }
        m_focus = target;
        m_show_keyboard = true;
        m_cursor_visible = true;
        invalidate_layout();
    } else {
        m_focus = nullptr;
        m_show_keyboard = false;
        invalidate_layout();
    }
}

void Browser::pointer_move(int x, int y) {
    m_pointer_x = x;
    m_pointer_y = y;
    if (!m_pointer_down) return;
    if (m_scroll_dragging) {
        apply_scroll_drag();
        return;
    }
    if (m_pointer_target != nullptr && m_pointer_target->kind == NodeKind::slider) {
        edit_value(*m_pointer_target, float(x - m_pointer_target->box.x - 8) /
            float(std::max(1, m_pointer_target->box.width - 16)));
        return;
    }
    if (m_scroll_target == nullptr) return;
    const float delta_y = float(m_pointer_y) - float(m_scroll_start_y);
    if (std::abs(delta_y) < kScrollDragThresholdPixels) return;
    if (m_pointer_target != nullptr) invalidate(*m_pointer_target);
    if (m_layout_dirty) perform_layout();
    m_pointer_target = nullptr;
    m_scroll_dragging = true;
    apply_scroll_drag();
}

void Browser::pointer_up(int x, int y) {
    m_pointer_x = x;
    m_pointer_y = y;
    Node* target = m_pointer_target;
    cancel_pointer_gesture();
    if (target == nullptr) return;
    invalidate(*target);
    if (target->kind == NodeKind::button && hit_test(x, y) == target) {
        invoke(target->on_press);
    }
}

void Browser::scroll(int delta) {
    if (m_page == nullptr) return;
    if (m_layout_dirty) perform_layout();
    Node* target = scroll_node(m_page->root, m_pointer_x, m_pointer_y);
    if (target == nullptr) target = scroll_node(m_page->root, m_size.width / 2, m_size.height / 2);
    if (target == nullptr) return;
    set_scroll_offset(*target, target->scroll - float(delta) * 32.0F);
    invalidate_layout();
}

void Browser::key(Key pressed) {
    if (pressed == Key::toggle_stats) {
        m_show_stats = !m_show_stats;
        invalidate({std::max(0, m_size.width - 184), 4, std::min(184, m_size.width), 58});
        return;
    }
    if (pressed == Key::escape) {
        m_focus = nullptr;
        m_show_keyboard = false;
        cancel_pointer_gesture();
        invalidate_layout();
        return;
    }
    if (m_focus == nullptr) return;
    Node& input = *m_focus;
    std::string& value = m_state[input.binding].string;
    m_cursor_byte = std::min(m_cursor_byte, value.size());
    switch (pressed) {
    case Key::left: m_cursor_byte = previous_character(value, m_cursor_byte); break;
    case Key::right:
        if (m_cursor_byte < value.size()) m_cursor_byte += next_utf8(value, m_cursor_byte).length;
        break;
    case Key::home: m_cursor_byte = 0; break;
    case Key::end_key: m_cursor_byte = value.size(); break;
    case Key::backspace:
        if (m_cursor_byte > 0) {
            const std::size_t previous = previous_character(value, m_cursor_byte);
            std::string modified = value;
            modified.erase(previous, m_cursor_byte - previous);
            m_cursor_byte = previous;
            write_state(input.binding, Value::from_string(std::move(modified)));
            changed(input);
        }
        break;
    case Key::enter: {
        const uint32_t handler = input.on_submit;
        m_focus = nullptr;
        m_show_keyboard = false;
        invalidate_layout();
        invoke(handler);
        return;
    }
    case Key::escape:
    case Key::toggle_stats:
        return;
    }
    m_cursor_visible = true;
    invalidate(input);
}

void Browser::text_input(std::string_view utf8) {
    if (m_focus == nullptr || !valid_input(utf8)) return;
    Node& input = *m_focus;
    const std::string& previous = m_state[input.binding].string;
    m_cursor_byte = std::min(m_cursor_byte, previous.size());
    const std::size_t allowed = input.max_length > character_count(previous)
        ? input.max_length - character_count(previous) : 0;
    std::size_t bytes = 0;
    for (std::size_t count = 0; count < allowed && bytes < utf8.size(); ++count) {
        bytes += next_utf8(utf8, bytes).length;
    }
    if (bytes == 0) return;
    std::string modified = previous;
    modified.insert(m_cursor_byte, utf8.substr(0, bytes));
    m_cursor_byte += bytes;
    write_state(input.binding, Value::from_string(std::move(modified)));
    // write_state follows external-value updates; this edit has a specific insertion point.
    m_cursor_byte = std::min(m_cursor_byte, m_state[input.binding].string.size());
    changed(input);
    m_cursor_visible = true;
}

} // namespace mb
