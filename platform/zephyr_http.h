#pragma once

#include "microbrowser/platform.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace mb {

enum class ZephyrRequestKind : uint8_t { script, content, package };

struct ZephyrHttpConfig {
    bool enabled = false;
    bool allow_plain_http = false;
    int tls_ca_security_tag = -1;
    uint32_t wifi_connect_timeout_ms = 30'000;
    uint32_t socket_connect_timeout_ms = 10'000;
    uint32_t request_timeout_ms = 15'000;
    uint32_t shutdown_timeout_ms = 90'000;
    std::size_t receive_buffer_bytes = 2048;
    std::size_t script_response_limit_bytes = 64U * 1024U;
    std::size_t content_response_limit_bytes = 128U * 1024U;
};

// One worker owns all network I/O. UI calls only submit bounded jobs and take
// completed results. Jobs and their sockets remain stable until UI teardown.
class ZephyrHttp final {
public:
    explicit ZephyrHttp(ZephyrHttpConfig config);
    ~ZephyrHttp();

    ZephyrHttp(const ZephyrHttp&) = delete;
    ZephyrHttp& operator=(const ZephyrHttp&) = delete;
    ZephyrHttp(ZephyrHttp&&) = delete;
    ZephyrHttp& operator=(ZephyrHttp&&) = delete;

    void start_network();
    [[nodiscard]] bool network_online() const;
    void start(ZephyrRequestKind kind, uint32_t id, ContentRequest request);
    void start_package(uint32_t id, std::string url, std::size_t response_limit_bytes);
    [[nodiscard]] bool poll(ZephyrRequestKind kind, HttpResult& result);
// Cancellation is cooperative: the worker completes its bounded socket/DNS
// operation and only then makes its slot reusable. No cross-thread close can
// race descriptor reuse.
    [[nodiscard]] bool cancel(ZephyrRequestKind kind, uint32_t id);
    void shutdown() noexcept;

private:
    struct State;
    std::unique_ptr<State> m_state;
};

} // namespace mb
