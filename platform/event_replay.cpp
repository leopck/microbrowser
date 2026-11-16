#include "event_replay.h"

#include <array>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <type_traits>

namespace mb {
namespace {

struct Click {
    int x = 0;
    int y = 0;
};
struct PointerDown {
    int x = 0;
    int y = 0;
};
struct PointerMove {
    int x = 0;
    int y = 0;
};
struct PointerUp {
    int x = 0;
    int y = 0;
};
struct Wait {
    uint64_t milliseconds = 0;
};
struct KeyPress {
    Key key = Key::enter;
};
struct TextInput {
    std::string text;
};
struct FunctionCall {
    std::string name;
};
struct Scroll {
    int delta = 0;
};
struct Screenshot {
    std::filesystem::path path;
};

using ReplayAction = std::variant<Click, PointerDown, PointerMove, PointerUp, Wait,
                                  KeyPress, TextInput, FunctionCall, Scroll, Screenshot>;

[[noreturn]] void replay_error(const std::filesystem::path& source, std::size_t line,
                               std::string_view message) {
    throw std::runtime_error(source.string() + ":" + std::to_string(line) + ": " +
                             std::string(message));
}

template <typename T>
T parse_number(std::string_view text) {
    T value{};
    const char* const begin = text.data();
    const char* const end = begin + text.size();
    const auto parsed = std::from_chars(begin, end, value);
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != end) {
        throw std::invalid_argument("invalid integer '" + std::string(text) + "'");
    }
    return value;
}

void require_end(std::istringstream& tokens) {
    tokens >> std::ws;
    if (!tokens.eof()) throw std::invalid_argument("unexpected trailing arguments");
}

ReplayAction parse_key(std::istringstream& tokens) {
    std::string name;
    tokens >> name;
    require_end(tokens);
    constexpr std::array names{
        std::pair<std::string_view, Key>{"enter", Key::enter},
        std::pair<std::string_view, Key>{"backspace", Key::backspace},
        std::pair<std::string_view, Key>{"left", Key::left},
        std::pair<std::string_view, Key>{"right", Key::right},
        std::pair<std::string_view, Key>{"home", Key::home},
        std::pair<std::string_view, Key>{"end", Key::end_key},
        std::pair<std::string_view, Key>{"escape", Key::escape},
        std::pair<std::string_view, Key>{"stats", Key::toggle_stats},
    };
    for (const auto& [candidate, key] : names) {
        if (name == candidate) return KeyPress{key};
    }
    throw std::invalid_argument("unknown key '" + name + "'");
}

ReplayAction parse_tokens(std::string_view name, std::istringstream& tokens) {
    if (name == "click") {
        int x = 0;
        int y = 0;
        tokens >> x >> y;
        if (tokens.fail()) throw std::invalid_argument("click requires integer X and Y");
        require_end(tokens);
        return Click{x, y};
    }
    if (name == "down" || name == "move" || name == "up") {
        int x = 0;
        int y = 0;
        tokens >> x >> y;
        if (tokens.fail()) throw std::invalid_argument("pointer command requires integer X and Y");
        require_end(tokens);
        if (name == "down") return PointerDown{x, y};
        if (name == "move") return PointerMove{x, y};
        return PointerUp{x, y};
    }
    if (name == "wait") {
        std::string value;
        tokens >> value;
        if (value.empty()) throw std::invalid_argument("wait requires milliseconds");
        require_end(tokens);
        const uint64_t milliseconds = parse_number<uint64_t>(value);
        if (milliseconds > static_cast<uint64_t>(
                std::numeric_limits<std::chrono::milliseconds::rep>::max())) {
            throw std::invalid_argument("wait duration is too large");
        }
        return Wait{milliseconds};
    }
    if (name == "key") return parse_key(tokens);
    if (name == "call") {
        std::string function;
        tokens >> function;
        if (function.empty()) throw std::invalid_argument("call requires a function name");
        require_end(tokens);
        return FunctionCall{std::move(function)};
    }
    if (name == "scroll") {
        std::string value;
        tokens >> value;
        if (value.empty()) throw std::invalid_argument("scroll requires a delta");
        require_end(tokens);
        return Scroll{parse_number<int>(value)};
    }
    throw std::invalid_argument("unknown command '" + std::string(name) + "'");
}

} // namespace

struct EventReplay::Command {
    ReplayAction action;
    std::size_t line = 0;
};

EventReplay::EventReplay(const std::filesystem::path& source) : m_source(source) {
    std::ifstream input(source, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open event replay file '" + source.string() + "'");

    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::size_t begin = line.find_first_not_of(" \t");
        if (begin == std::string::npos) continue;
        const std::size_t separator = line.find_first_of(" \t", begin);
        const std::size_t end = separator == std::string::npos ? line.size() : separator;
        const std::string_view name(line.data() + begin, end - begin);
        try {
            if (name == "text" || name == "shot") {
                if (separator == std::string::npos) {
                    throw std::invalid_argument(std::string(name) + " requires an argument");
                }
                std::string argument = line.substr(separator + 1);
                if (name == "text") m_commands.push_back(Command{TextInput{std::move(argument)}, line_number});
                else {
                    if (argument.empty()) throw std::invalid_argument("shot requires an output path");
                    m_commands.push_back(Command{Screenshot{std::filesystem::path(std::move(argument))}, line_number});
                }
                continue;
            }

            std::istringstream tokens(line.substr(begin));
            std::string parsed_name;
            tokens >> parsed_name;
            m_commands.push_back(Command{parse_tokens(parsed_name, tokens), line_number});
        } catch (const std::invalid_argument& error) {
            replay_error(source, line_number, error.what());
        }
    }
    if (input.bad() || !input.eof()) {
        throw std::runtime_error("failed while reading event replay file '" + source.string() + "'");
    }
}

EventReplay::~EventReplay() = default;
bool EventReplay::finished() const {
    return !m_waiting && m_next == m_commands.size();
}

void EventReplay::advance(Browser& browser, ScreenshotWriter write_screenshot) {
    if (finished()) return;
    const auto now = std::chrono::steady_clock::now();
    if (m_waiting) {
        if (now < m_deadline) return;
        m_waiting = false;
        if (finished()) return;
    }

    Command& command = m_commands[m_next];
    try {
        std::visit(
            [&](const auto& action) {
                using Action = std::decay_t<decltype(action)>;
                if constexpr (std::is_same_v<Action, Click>) {
                    browser.pointer_down(action.x, action.y);
                    browser.pointer_up(action.x, action.y);
                } else if constexpr (std::is_same_v<Action, PointerDown>) {
                    browser.pointer_down(action.x, action.y);
                } else if constexpr (std::is_same_v<Action, PointerMove>) {
                    browser.pointer_move(action.x, action.y);
                } else if constexpr (std::is_same_v<Action, PointerUp>) {
                    browser.pointer_up(action.x, action.y);
                } else if constexpr (std::is_same_v<Action, Wait>) {
                    const auto maximum = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::time_point::max() - now).count();
                    if (action.milliseconds > static_cast<uint64_t>(maximum)) {
                        throw std::invalid_argument("wait duration exceeds the monotonic clock range");
                    }
                    m_deadline = now + std::chrono::milliseconds(action.milliseconds);
                    m_waiting = true;
                } else if constexpr (std::is_same_v<Action, KeyPress>) {
                    browser.key(action.key);
                } else if constexpr (std::is_same_v<Action, TextInput>) {
                    browser.text_input(action.text);
                } else if constexpr (std::is_same_v<Action, FunctionCall>) {
                    browser.dispatch(action.name);
                } else if constexpr (std::is_same_v<Action, Scroll>) {
                    browser.scroll(action.delta);
                } else if constexpr (std::is_same_v<Action, Screenshot>) {
                    if (write_screenshot == nullptr) {
                        throw std::invalid_argument("no screenshot writer was provided");
                    }
                    write_screenshot(browser, action.path);
                }
            },
            command.action);
    } catch (const std::exception& error) {
        replay_error(m_source, command.line, error.what());
    }
    ++m_next;
}

} // namespace mb
