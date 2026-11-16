#include "microbrowser/browser.h"
#include "layout.h"
#include "paint.h"
#include "microbrowser/canvas.h"
#include "microbrowser/json.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

namespace mb {
namespace {

Size validated_size(Size size) {
    if (size.width < 64 || size.width > 2048 || size.height < 64 || size.height > 2048) {
        throw PackageError("screen dimensions must be between 64 and 2048 pixels");
    }
    return size;
}

template<typename Callback>
void for_each_node(Node& node, Callback&& callback) {
    callback(node);
    for (auto& child : node.children) for_each_node(*child, callback);
}

void validate_tree(const Package& package, const Node& node, uint32_t& count, uint32_t depth = 0) {
    if (depth > 32) throw PackageError("maximum document nesting depth is 32");
    if (node.children.size() > 64) throw PackageError("maximum children per container is 64");
    if (++count > 2048) throw PackageError("maximum node count is 2048");
    if (node.binding != no_index && node.binding >= package.states.size()) {
        throw PackageError("element references a missing state field");
    }
    if (node.id.size() > 128) throw PackageError("element id exceeds 128 bytes");
    if (node.style.padding < 0 || node.style.padding > 256 || node.style.gap < 0 ||
        node.style.gap > 256 || node.style.radius < 0 || node.style.radius > 256 ||
        node.style.font_size < 1 || node.style.font_size > 8) {
        throw PackageError("element paint metrics are outside their limits");
    }
    for (Length length : {node.style.width, node.style.height}) {
        if (!std::isfinite(length.value) || (length.kind == LengthKind::percent && (length.value < 0 || length.value > 100))) {
            throw PackageError("element has an invalid flexible length");
        }
    }
    if (!std::isfinite(node.minimum) || !std::isfinite(node.maximum) || !std::isfinite(node.step) || node.step < 0) {
        throw PackageError("element has a non-finite numeric property");
    }
    for (uint32_t handler : {node.on_press, node.on_change, node.on_submit, node.on_load}) {
        if (handler != no_index && handler >= package.functions.size()) throw PackageError("element references a missing handler");
    }
    if (depth > 0 && node.on_load != no_index) throw PackageError("on_load is only valid on a page root");
    for (const auto& child : node.children) validate_tree(package, *child, count, depth + 1);
}

void validate_package(const Package& package) {
    if (package.pages.empty()) throw PackageError("package has no pages");
    uint32_t total = 0;
    for (const Page& page : package.pages) validate_tree(package, page.root, total);
}

std::pair<uint64_t, std::string_view> animation_timing(std::string_view specification) {
    uint64_t duration = 250;
    std::string_view kind = specification;
    const std::size_t colon = specification.find(':');
    if (colon != std::string_view::npos) {
        kind = specification.substr(0, colon);
        const std::string_view digits = specification.substr(colon + 1);
        if (digits.empty() || digits.size() > 5 ||
            !std::all_of(digits.begin(), digits.end(), [](char ch) { return ch >= '0' && ch <= '9'; })) {
            throw PackageError("animation duration must be a millisecond integer");
        }
        duration = 0;
        for (char digit : digits) duration = duration * 10 + uint64_t(digit - '0');
        if (duration == 0 || duration > 60'000) throw PackageError("animation duration must be 1..60000 milliseconds");
    }
    if (colon == std::string_view::npos && specification == "pulse") duration = 1'400;
    return {duration, kind};
}

} // namespace

Browser::Browser(Platform& platform, Size size)
    : m_platform(platform), m_size(validated_size(size)),
      m_pixels(std::size_t(size.width) * std::size_t(size.height), to_rgb565(palette::background)) {
    m_pending_regions.reserve(32);
}

uint32_t Browser::find_function(std::string_view name) const {
    const auto found = std::find_if(m_package.functions.begin(), m_package.functions.end(),
        [&](const Function& function) { return function.name == name; });
    if (found == m_package.functions.end()) throw ScriptError("unknown function: " + std::string(name));
    return uint32_t(found - m_package.functions.begin());
}

uint32_t Browser::find_state(std::string_view name) const {
    const auto found = std::find_if(m_package.states.begin(), m_package.states.end(),
        [&](const StateField& state) { return state.name == name; });
    if (found == m_package.states.end()) throw ScriptError("unknown state field: " + std::string(name));
    return uint32_t(found - m_package.states.begin());
}

Node* Browser::find_node(std::string_view id) {
    if (m_page == nullptr) return nullptr;
    Node* result = nullptr;
    for_each_node(m_page->root, [&](Node& node) { if (node.id == id) result = &node; });
    return result;
}

void Browser::load(std::vector<uint8_t> bytes) {
    Package package = parse_package(bytes);
    validate_package(package);
    const uint64_t package_bytes = bytes.size();
    const uint32_t state_count = uint32_t(package.states.size());
    std::vector<Value> initial_state;
    initial_state.reserve(package.states.size());
    for (const StateField& field : package.states) initial_state.push_back(field.initial);
    cancel_pointer_gesture();
    m_package = std::move(package);
    m_state = std::move(initial_state);
    m_page = nullptr;
    m_history_size = 0;
    m_focus = nullptr;
    m_cursor_byte = 0;
    m_show_keyboard = false;
    m_timers = {};
    m_requests = {};
    m_error.clear();
    m_error_until = 0;
    m_metrics = {};
    m_metrics.package_bytes = package_bytes;
    m_metrics.state_count = state_count;
    for (Page& page : m_package.pages) for_each_node(page.root, [&](Node&) { ++m_metrics.node_count; });
    open_page_index(0, false);
}

void Browser::open_page(std::string_view name, bool remember) {
    const auto found = std::find_if(m_package.pages.begin(), m_package.pages.end(),
        [&](const Page& page) { return page.name == name; });
    if (found == m_package.pages.end()) throw ScriptError("unknown page: " + std::string(name));
    open_page_index(uint32_t(found - m_package.pages.begin()), remember);
}

void Browser::open_page_index(uint32_t index, bool remember) {
    if (index >= m_package.pages.size()) throw ScriptError("page index is out of bounds");
    if (remember && m_page != nullptr) {
        const uint32_t previous = uint32_t(m_page - m_package.pages.data());
        if (m_history_size == m_history.size()) {
            std::shift_left(m_history.begin(), m_history.end(), 1);
            --m_history_size;
        }
        m_history[m_history_size++] = previous;
    }
    cancel_pointer_gesture();
    m_page = &m_package.pages[index];
    m_focus = nullptr;
    m_show_keyboard = false;
    m_cursor_byte = 0;
    m_cursor_visible = true;
    m_next_blink = m_platform.now_ms() + 500;
    ++m_generations;
    invalidate_layout();
    start_animations();
    m_page_loaded = true;
    if (m_page->root.on_load != no_index) invoke(m_page->root.on_load);
}

std::string_view Browser::page_name() const {
    return m_page == nullptr ? std::string_view{} : std::string_view(m_page->name);
}

void Browser::resize(Size size) {
    const Size validated = validated_size(size);
    m_pixels = std::vector<uint16_t>(std::size_t(validated.width) * std::size_t(validated.height), to_rgb565(palette::background));
    m_size = validated;
    invalidate_layout();
}

Value Browser::read_state(uint32_t index) const {
    if (index >= m_state.size()) throw ScriptError("state index is out of bounds");
    return m_state[index];
}

void Browser::write_state(uint32_t index, const Value& value) {
    if (index >= m_state.size()) throw ScriptError("state index is out of bounds");
    const Value& previous = m_state[index];
    if (previous.type != value.type || value.type == Value::Type::void_value) {
        throw ScriptError("state assignment does not preserve its declared type");
    }
    if (value.type == Value::Type::number && !std::isfinite(value.number)) throw ScriptError("non-finite state value");
    const bool unchanged = value.type == Value::Type::string ? previous.string == value.string
        : value.type == Value::Type::number ? previous.number == value.number
        : previous.boolean == value.boolean;
    if (unchanged) return;
    m_state[index] = value;
    if (m_focus != nullptr && m_focus->binding == index) m_cursor_byte = m_state[index].string.size();
    if (m_page == nullptr) return;
    bool requires_layout = false;
    for_each_node(m_page->root, [&](Node& node) {
        if (node.binding != index) return;
        invalidate(node);
        if (node.kind == NodeKind::text &&
            (node.style.width.kind == LengthKind::automatic || node.style.height.kind == LengthKind::automatic)) {
            requires_layout = true;
        }
    });
    if (requires_layout) invalidate_layout();
}

void Browser::handle_error(std::string_view message) {
    m_error = message;
    m_error_until = m_platform.now_ms() + 5'000;
    m_platform.log(m_error);
    m_full_repaint = true;
}

void Browser::invoke(uint32_t function) {
    if (function == no_index) return;
    if (function >= m_package.functions.size()) throw ScriptError("handler index is out of bounds");
    if (m_invocations >= 8) {
        handle_error("page on_load navigation exceeds eight levels");
        return;
    }
    ++m_invocations;
    ++m_metrics.scripts;
    m_in_script = true;
    struct Restore {
        uint32_t& invocations;
        bool& in_script;
        ~Restore() { --invocations; in_script = invocations != 0; }
    } restore{m_invocations, m_in_script};
    try {
        execute_script(m_package, function, {}, *this);
    } catch (const Navigation&) {
        cancel_pointer_gesture();
    } catch (const std::exception& error) {
        handle_error(error.what());
    }
}

void Browser::dispatch(std::string_view function_name) {
    const uint32_t function = find_function(function_name);
    invoke(function);
}

void Browser::start_animations() {
    if (m_page == nullptr) return;
    for_each_node(m_page->root, [&](Node& node) {
        node.opacity = 1.0F;
        node.scale = 1.0F;
        node.offset_y = 0.0F;
        node.scroll = 0.0F;
        node.pulse_period = 0;
        node.animation_property = 0;
        const auto [duration, kind] = animation_timing(node.animation);
        if (kind.empty()) return;
        if (kind == "fade") {
            node.opacity = 0.0F;
            animate_node(node, Animation::Property::opacity, 1.0F, duration);
        } else if (kind == "slide") {
            node.offset_y = 12.0F;
            animate_node(node, Animation::Property::offset_y, 0.0F, duration);
        } else if (kind == "pulse") {
            node.pulse_period = duration;
            node.pulse_started = m_platform.now_ms();
        } else {
            throw PackageError("animation must be fade, pulse, or slide");
        }
    });
}

void Browser::animate_node(Node& node, Animation::Property property, float target, uint64_t duration) {
    invalidate(node);
    const float final_scale = property == Animation::Property::scale ? std::max(node.scale, target) : node.scale;
    const float final_offset = property == Animation::Property::offset_y ? target : node.offset_y;
    const int final_width = std::max(0, int(float(node.box.width) * final_scale));
    const int final_height = std::max(0, int(float(node.box.height) * final_scale));
    invalidate({node.box.x + (node.box.width - final_width) / 2,
                node.box.y + (node.box.height - final_height) / 2 + int(final_offset),
                final_width, final_height});
    node.animation_property = uint32_t(property);
    node.animation_started = m_platform.now_ms();
    node.animation_duration = duration;
    if (property == Animation::Property::scale) node.animation_from = node.scale;
    if (property == Animation::Property::opacity) node.animation_from = node.opacity;
    if (property == Animation::Property::offset_y) node.animation_from = node.offset_y;
    if (property == Animation::Property::scale) node.pulse_period = 0;
    node.animation_target = target;
}

bool Browser::advance_animations(uint64_t now) {
    if (m_page == nullptr) return false;
    bool animated = false;
    for_each_node(m_page->root, [&](Node& node) {
        bool needs_damage = false;
        if (node.animation_property != uint32_t(Animation::Property::none)) {
            const uint64_t elapsed = now - node.animation_started;
            float fraction = node.animation_duration == 0 ? 1.0F
                : float(std::min(elapsed, node.animation_duration)) / float(node.animation_duration);
            fraction = fraction * fraction * (3.0F - 2.0F * fraction);
            const float value = node.animation_from + (node.animation_target - node.animation_from) * fraction;
            if (node.animation_property == uint32_t(Animation::Property::scale)) node.scale = value;
            if (node.animation_property == uint32_t(Animation::Property::opacity)) node.opacity = value;
            if (node.animation_property == uint32_t(Animation::Property::offset_y)) node.offset_y = value;
            if (elapsed >= node.animation_duration) node.animation_property = uint32_t(Animation::Property::none);
            needs_damage = true;
        }
        if (node.pulse_period != 0) {
            const uint64_t phase_ms = (now - node.pulse_started) % node.pulse_period;
            const float phase = float(phase_ms) / float(node.pulse_period);
            node.scale = 1.0F + 0.045F * std::sin(phase * 6.28318530718F);
            needs_damage = true;
        }
        if (needs_damage) {
            invalidate(node);
            animated = true;
        }
    });
    return animated;
}

void Browser::advance_timers(uint64_t now) {
    for (Timer& timer : m_timers) {
        if (!timer.active || timer.due > now) continue;
        const uint32_t function = timer.function;
        if (timer.repeating) {
            timer.due = now + timer.interval;
        } else {
            timer = {};
        }
        invoke(function);
    }
}

void Browser::poll_network() {
    HttpResult result;
    while (m_platform.poll_request(result)) {
        const auto found = std::find_if(m_requests.begin(), m_requests.end(),
            [&](const PendingRequest& request) { return request.active && request.id == result.id; });
        if (found == m_requests.end()) {
            m_platform.log("Discarded stale network response");
            continue;
        }
        PendingRequest request = std::move(*found);
        *found = {};
        if (!result.succeeded()) {
            handle_error(result.error.empty() ? "Network request failed" : result.error);
            continue;
        }
        try {
            const Value value = !request.json_request ? Value::from_string(std::move(result.body))
                : read_json_scalar(result.body, request.json_path, request.expected_type);
            if (value.type != m_state[request.state].type) throw ScriptError("response does not match the destination state type");
            write_state(request.state, value);
        } catch (const std::exception& error) {
            handle_error(error.what());
        }
    }
    const uint32_t active = uint32_t(std::count_if(m_requests.begin(), m_requests.end(),
        [](const PendingRequest& request) { return request.active; }));
    m_metrics.active_requests = active;
}

void Browser::invalidate(Rect area) {
    if (area.empty()) return;
    area = area.clipped({0, 0, m_size.width, m_size.height});
    if (area.empty() || m_full_repaint) return;
    for (Rect& previous : m_pending_regions) {
        const Rect joined = previous.united(area);
        if (!previous.clipped(area).empty()) {
            previous = joined;
            return;
        }
    }
    if (m_pending_regions.size() >= 32) {
        m_full_repaint = true;
        m_pending_regions.clear();
    } else {
        m_pending_regions.push_back(area);
    }
}

void Browser::invalidate(const Node& node) {
    invalidate(visual_bounds(node));
    invalidate({node.box.x - 16, node.box.y - 16, node.box.width + 32, node.box.height + 32});
}

void Browser::invalidate_layout() {
    m_layout_dirty = true;
    m_full_repaint = true;
}

void Browser::consume_regions() {
    m_frame_damage.count = 0;
    if (m_full_repaint) {
        m_frame_damage.regions[0] = {0, 0, m_size.width, m_size.height};
        m_frame_damage.count = 1;
    } else {
        for (const Rect& rectangle : m_pending_regions) {
            if (m_frame_damage.count == Damage::capacity) {
                m_frame_damage.regions[0] = {0, 0, m_size.width, m_size.height};
                m_frame_damage.count = 1;
                break;
            }
            m_frame_damage.regions[m_frame_damage.count++] = rectangle;
        }
    }
    m_full_repaint = false;
    m_pending_regions.clear();
}

void Browser::perform_layout() {
    const auto started = std::chrono::steady_clock::now();
    m_layout_dirty = false;
    Layout{*this}.run();
    ++m_metrics.layout_count;
    m_metrics.last_layout_us = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started).count());
}

void Browser::paint() {
    const auto started = std::chrono::steady_clock::now();
    for (std::size_t index = 0; index < m_frame_damage.count; ++index) {
        const Rect region = m_frame_damage.regions[index];
        Painter{*this, Canvas{m_pixels.data(), m_size}}.run(region);
        m_metrics.flushed_pixels += uint64_t(region.width) * uint64_t(region.height);
    }
    ++m_metrics.frames;
    m_metrics.last_render_us = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started).count());
    m_last_paint = m_platform.now_ms();
}

bool Browser::tick() {
    m_frame_damage.count = 0;
    if (m_page == nullptr) return false;
    const uint64_t now = m_platform.now_ms();
    poll_network();
    advance_timers(now);
    advance_animations(now);
    if (m_error_until != 0 && m_error_until <= now) {
        m_error_until = 0;
        m_full_repaint = true;
    }
    if (m_focus != nullptr && now >= m_next_blink) {
        m_cursor_visible = !m_cursor_visible;
        m_next_blink = now + 500;
        invalidate(*m_focus);
    }
    if (m_show_stats && now - m_last_paint >= 1'000) m_full_repaint = true;
    if (m_layout_dirty) perform_layout();
    if (!m_full_repaint && m_pending_regions.empty()) return false;
    consume_regions();
    paint();
    return true;
}

} // namespace mb
