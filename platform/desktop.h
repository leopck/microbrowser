#pragma once

#include "microbrowser/platform.h"

#include <chrono>
#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

namespace mb {

class DesktopPlatform final : public Platform {
public:
    DesktopPlatform();
    ~DesktopPlatform() override;

    DesktopPlatform(const DesktopPlatform&) = delete;
    DesktopPlatform& operator=(const DesktopPlatform&) = delete;
    DesktopPlatform(DesktopPlatform&&) = delete;
    DesktopPlatform& operator=(DesktopPlatform&&) = delete;

    [[nodiscard]] uint64_t now_ms() const override;
    void start_request(uint32_t id, std::string url) override;
    [[nodiscard]] bool poll_request(HttpResult& result) override;

    [[nodiscard]] std::optional<Value> load_value(const std::string& name) const override;
    void save_value(const std::string& name, const Value& value) override;
    void log(std::string_view message) override;

private:
    struct Request {
        uint32_t id = 0;
        std::string url;
    };

    void worker_loop();

    const std::chrono::steady_clock::time_point m_epoch = std::chrono::steady_clock::now();
    const std::filesystem::path m_state_path;

    std::mutex m_request_mutex;
    std::condition_variable m_request_ready;
    std::deque<Request> m_pending_requests;
    std::deque<HttpResult> m_completed_requests;
    std::size_t m_outstanding_requests = 0;
    bool m_stopping = false;
    std::thread m_worker;

    mutable std::mutex m_storage_mutex;
    std::mutex m_log_mutex;
};

} // namespace mb
