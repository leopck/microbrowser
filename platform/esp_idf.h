#pragma once

#include "microbrowser/platform.h"

#include <memory>

namespace mb {

class EspIdfPlatform final : public Platform {
public:
    EspIdfPlatform();
    ~EspIdfPlatform() override;

    EspIdfPlatform(const EspIdfPlatform&) = delete;
    EspIdfPlatform& operator=(const EspIdfPlatform&) = delete;
    EspIdfPlatform(EspIdfPlatform&&) = delete;
    EspIdfPlatform& operator=(EspIdfPlatform&&) = delete;

    // Network unavailability, including a DHCP timeout, leaves an offline platform
    // rather than preventing local pages from starting.
    void start_network();
    [[nodiscard]] bool network_online() const;

    [[nodiscard]] uint64_t now_ms() const override;
    void start_request(uint32_t id, std::string url) override;
    [[nodiscard]] bool poll_request(HttpResult& result) override;
    void start_content(uint32_t id, ContentRequest request) override;
    [[nodiscard]] bool poll_content(HttpResult& result) override;

    [[nodiscard]] std::optional<Value> load_value(const std::string& name) const override;
    void save_value(const std::string& name, const Value& value) override;
    void log(std::string_view message) override;

private:
    struct State;
    std::unique_ptr<State> m_state;
};

} // namespace mb
