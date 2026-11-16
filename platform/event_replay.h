#pragma once

#include "microbrowser/browser.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <vector>

namespace mb {

class EventReplay {
public:
    using ScreenshotWriter = void (*)(const Browser&, const std::filesystem::path&);

    explicit EventReplay(const std::filesystem::path& source);
    ~EventReplay();

    EventReplay(const EventReplay&) = delete;
    EventReplay& operator=(const EventReplay&) = delete;
    EventReplay(EventReplay&&) = delete;
    EventReplay& operator=(EventReplay&&) = delete;

    void advance(Browser& browser, ScreenshotWriter write_screenshot);
    [[nodiscard]] bool finished() const;

private:
    struct Command;

    std::filesystem::path m_source;
    std::vector<Command> m_commands;
    std::size_t m_next = 0;
    bool m_waiting = false;
    std::chrono::steady_clock::time_point m_deadline{};
};

} // namespace mb
