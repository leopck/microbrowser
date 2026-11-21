#pragma once

#include "microbrowser/platform.h"
#include "platform/zephyr_http.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace mb {

class ZephyrPlatform final : public Platform {
public:
    explicit ZephyrPlatform(ZephyrHttpConfig http_config = {});
    ~ZephyrPlatform() override;

    ZephyrPlatform(const ZephyrPlatform&) = delete;
    ZephyrPlatform& operator=(const ZephyrPlatform&) = delete;
    ZephyrPlatform(ZephyrPlatform&&) = delete;
    ZephyrPlatform& operator=(ZephyrPlatform&&) = delete;

    void start_network();
    void stop_network();
    [[nodiscard]] bool network_online() const;

    void start_package_download(uint32_t id, std::string url, std::size_t maximum_bytes);
    [[nodiscard]] bool poll_package_download(HttpResult& result);
    [[nodiscard]] bool cancel_package_download(uint32_t id);

    [[nodiscard]] uint64_t now_ms() const override;
    void start_request(uint32_t id, std::string url) override;
    [[nodiscard]] bool poll_request(HttpResult& result) override;
    void start_content(uint32_t id, ContentRequest request) override;
    [[nodiscard]] bool poll_content(HttpResult& result) override;

    [[nodiscard]] std::optional<Value> load_value(const std::string& name) const override;
    void save_value(const std::string& name, const Value& value) override;
    // Counts exact matches among the latest 32 sanitized application log entries.
    // Platform logging is confined to its owning application/UI thread.
    [[nodiscard]] std::size_t log_count(std::string_view message) const;
    void log(std::string_view message) override;

private:
    struct State;
    std::unique_ptr<State> m_state;
};

} // namespace mb
