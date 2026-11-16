#include "microbrowser/browser.h"
#include "microbrowser/canvas.h"
#include "microbrowser/json.h"

#include <algorithm>
#include <cmath>

namespace mb {
namespace {

const Value& argument(std::span<const Value> arguments, std::size_t index, Value::Type type, const char* function) {
    if (index >= arguments.size() || arguments[index].type != type) {
        throw ScriptError(std::string(function) + ": invalid argument type or arity");
    }
    return arguments[index];
}

void arity(std::span<const Value> arguments, std::size_t expected, const char* function) {
    if (arguments.size() != expected) throw ScriptError(std::string(function) + ": wrong argument count");
}

uint64_t milliseconds(const Value& value, uint64_t minimum) {
    if (!std::isfinite(value.number) || value.number < 0 || double(value.number) < double(minimum) ||
        double(value.number) > 30.0 * 24.0 * 60.0 * 60.0 * 1000.0) {
        throw ScriptError("time interval is outside its allowed range");
    }
    return uint64_t(value.number);
}

uint32_t timer_id(const Value& value) {
    if (!std::isfinite(value.number) || value.number < 1 || value.number > 16'777'215.0F ||
        std::floor(value.number) != value.number) {
        throw ScriptError("timer id must be an exact positive integer");
    }
    return uint32_t(value.number);
}

const std::string& storage_key(std::span<const Value> arguments) {
    const std::string& key = argument(arguments, 0, Value::Type::string, "storage").string;
    if (key.empty() || key.size() > 128) throw ScriptError("storage key must contain 1..128 bytes");
    return key;
}

} // namespace

Value Browser::builtin(uint8_t id, std::span<const Value> arguments) {
    if (id <= 5) {
        arity(arguments, id == 2 ? 3 : id <= 1 ? 2 : 1, "math");
        for (const Value& value : arguments) {
            if (value.type != Value::Type::number || !std::isfinite(value.number)) throw ScriptError("math requires finite numbers");
        }
        if (id == 0) return Value::from_number(std::min(arguments[0].number, arguments[1].number));
        if (id == 1) return Value::from_number(std::max(arguments[0].number, arguments[1].number));
        if (id == 2) {
            if (arguments[1].number > arguments[2].number) throw ScriptError("clamp bounds are reversed");
            return Value::from_number(std::clamp(arguments[0].number, arguments[1].number, arguments[2].number));
        }
        if (id == 3) return Value::from_number(std::fabs(arguments[0].number));
        if (id == 4) return Value::from_number(std::round(arguments[0].number));
        return Value::from_number(std::floor(arguments[0].number));
    }
    if (id == 6) {
        arity(arguments, 1, "len");
        const std::string& value = argument(arguments, 0, Value::Type::string, "len").string;
        std::size_t count = 0;
        for (std::size_t offset = 0; offset < value.size();) {
            offset += next_utf8(value, offset).length;
            ++count;
        }
        return Value::from_number(float(count));
    }
    if (id == 7 || id == 8) {
        if (id == 7) {
            arity(arguments, 1, "go");
            open_page(argument(arguments, 0, Value::Type::string, "go").string, true);
        } else {
            arity(arguments, 0, "back");
            if (m_history_size == 0) throw ScriptError("navigation history is empty");
            open_page_index(m_history[--m_history_size], false);
        }
        throw Navigation{};
    }
    if (id == 9) {
        arity(arguments, 4, "animate");
        const std::string& element = argument(arguments, 0, Value::Type::string, "animate").string;
        const std::string& property = argument(arguments, 1, Value::Type::string, "animate").string;
        const float target = argument(arguments, 2, Value::Type::number, "animate").number;
        const uint64_t duration = milliseconds(argument(arguments, 3, Value::Type::number, "animate"), 0);
        if (!std::isfinite(target)) throw ScriptError("animation target must be finite");
        Node* node = find_node(element);
        if (node == nullptr) throw ScriptError("animation references an unknown element: " + element);
        if (property == "scale") {
            if (target < 0.1F || target > 3.0F) throw ScriptError("scale must be between 0.1 and 3");
            animate_node(*node, Animation::Property::scale, target, duration);
        } else if (property == "opacity") {
            if (target < 0 || target > 1) throw ScriptError("opacity must be between 0 and 1");
            animate_node(*node, Animation::Property::opacity, target, duration);
        } else if (property == "offset_y") {
            if (target < -512 || target > 512) throw ScriptError("offset_y exceeds its limits");
            animate_node(*node, Animation::Property::offset_y, target, duration);
        } else {
            throw ScriptError("animation property must be scale, opacity, or offset_y");
        }
        return Value::from_void();
    }
    if (id == 10 || id == 11) {
        arity(arguments, 2, id == 10 ? "after" : "every");
        const uint64_t delay = milliseconds(argument(arguments, 0, Value::Type::number, "timer"), id == 11 ? 20 : 0);
        const std::string& name = argument(arguments, 1, Value::Type::string, "timer").string;
        const uint32_t function = find_function(name);
        const auto available = std::find_if(m_timers.begin(), m_timers.end(), [](const Timer& timer) { return !timer.active; });
        if (available == m_timers.end()) throw ScriptError("all 16 timer slots are in use");
        if (m_next_id > 16'777'215) throw ScriptError("timer identifier space is exhausted");
        available->active = true;
        available->repeating = id == 11;
        available->id = uint32_t(m_next_id++);
        available->function = function;
        available->interval = delay;
        available->due = m_platform.now_ms() + delay;
        return Value::from_number(float(available->id));
    }
    if (id == 12) {
        arity(arguments, 1, "cancel");
        const uint32_t requested = timer_id(argument(arguments, 0, Value::Type::number, "cancel"));
        const auto timer = std::find_if(m_timers.begin(), m_timers.end(),
            [requested](const Timer& candidate) { return candidate.active && candidate.id == requested; });
        if (timer == m_timers.end()) throw ScriptError("cannot cancel an inactive timer");
        *timer = {};
        return Value::from_void();
    }
    if (id >= 13 && id <= 16) {
        const bool store = id == 13 || id == 15;
        const Value::Type type = id == 13 || id == 14 ? Value::Type::number : Value::Type::boolean;
        const std::string& key = storage_key(arguments);
        if (store) {
            arity(arguments, 2, "save");
            m_platform.save_value(key, argument(arguments, 1, type, "save"));
            return Value::from_void();
        }
        arity(arguments, 2, "load");
        std::optional<Value> saved = m_platform.load_value(key);
        if (!saved) return argument(arguments, 1, type, "load fallback");
        if (saved->type != type) throw ScriptError("saved value type differs from load type");
        return std::move(*saved);
    }
    if (id == 17 || id == 18) {
        const bool parse_json = id == 18;
        arity(arguments, parse_json ? 3 : 2, parse_json ? "fetch_json" : "fetch");
        const std::string& url = argument(arguments, 0, Value::Type::string, "fetch").string;
        const std::string& path = parse_json
            ? argument(arguments, 1, Value::Type::string, "fetch_json").string : std::string{};
        const std::string& state_name = argument(arguments, parse_json ? 2 : 1, Value::Type::string, "fetch").string;
        if ((!url.starts_with("http://") && !url.starts_with("https://")) || url.size() > 2047) {
            throw ScriptError("fetch requires an HTTP(S) URL of at most 2047 bytes");
        }
        if (state_name.size() > 255 || path.size() > 512) throw ScriptError("fetch destination or JSON path is too long");
        const uint32_t state = find_state(state_name);
        if (!parse_json && m_state[state].type != Value::Type::string) throw ScriptError("fetch destination must be a string state");
        const auto available = std::find_if(m_requests.begin(), m_requests.end(),
            [](const PendingRequest& request) { return !request.active; });
        if (available == m_requests.end()) throw ScriptError("all 8 network request slots are in use");
        if (m_next_id > 16'777'215) throw ScriptError("network identifier space is exhausted");
        const uint32_t request_id = uint32_t(m_next_id++);
        m_platform.start_request(request_id, url);
        available->active = true;
        available->id = request_id;
        available->state = state;
        available->json_path = path;
        available->expected_type = m_state[state].type;
        available->json_request = parse_json;
        m_metrics.active_requests += 1;
        return Value::from_number(float(request_id));
    }
    if (id >= 19 && id <= 20) {
        const std::string& key = storage_key(arguments);
        arity(arguments, 2, id == 19 ? "save_string" : "load_string");
        const std::string& value = argument(arguments, 1, Value::Type::string, "string storage").string;
        if (id == 19) {
            m_platform.save_value(key, Value::from_string(value));
            return Value::from_void();
        }
        std::optional<Value> saved = m_platform.load_value(key);
        if (!saved) return Value::from_string(value);
        if (saved->type != Value:: Type::string) throw ScriptError("saved value is not a string");
        return std::move(*saved);
    }
    if (id == 21 || id == 22) {
        arity(arguments, 1, "trigonometry");
        const float number = argument(arguments, 0, Value::Type::number, "trigonometry").number;
        const float result = id == 21 ? std::sin(number) : std::cos(number);
        if (!std::isfinite(result)) throw ScriptError("trigonometric result is not finite");
        return Value::from_number(result);
    }
    if (id == 23) {
        arity(arguments, 1, "string");
        return Value::from_string(format_value(argument(arguments, 0, Value::Type::number, "string"), {}));
    }
    if (id == 24) {
        arity(arguments, 1, "log");
        m_platform.log(argument(arguments, 0, Value::Type::string, "log").string);
        return Value::from_void();
    }
    if (id == 25) {
        arity(arguments, 2, "chart_push");
        const std::string& element = argument(arguments, 0, Value::Type::string, "chart_push").string;
        const float value = argument(arguments, 1, Value::Type::number, "chart_push").number;
        if (!std::isfinite(value)) throw ScriptError("chart sample must be finite");
        Node* node = find_node(element);
        if (node == nullptr || node->kind != NodeKind::chart) throw ScriptError("chart_push requires an existing chart id");
        if (node->sample_count < max_chart_samples) {
            node->samples[node->sample_count++] = value;
        } else {
            node->samples[node->sample_head] = value;
            node->sample_head = uint8_t((node->sample_head + 1) % max_chart_samples);
        }
        invalidate(*node);
        return Value::from_void();
    }
    if (id >= 26 && id <= 28) {
        arity(arguments, 2, "JSON accessor");
        const std::string& document = argument(arguments, 0, Value::Type::string, "JSON accessor").string;
        const std::string& path = argument(arguments, 1, Value::Type::string, "JSON accessor").string;
        const Value::Type type = id == 26 ? Value::Type::number : id == 27 ? Value::Type::string : Value::Type::boolean;
        return read_json_scalar(document, path, type);
    }
    throw ScriptError("unknown builtin opcode " + std::to_string(id));
}

} // namespace mb
