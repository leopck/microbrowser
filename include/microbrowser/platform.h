#pragma once

#include "microbrowser/types.h"

#include <cstdint>
#include <optional>
#include <string>

namespace mb {

struct HttpResult {
    uint32_t id = 0;
    int status = 0;
    std::string body;
    std::string error;
    [[nodiscard]] bool succeeded() const { return status >= 200 && status < 300 && error.empty(); }
};

class Platform {
public:
    virtual ~Platform() = default;
    [[nodiscard]] virtual uint64_t now_ms() const = 0;

    // start_request returns after queuing work, never after network I/O.
    virtual void start_request(uint32_t id, std::string url) = 0;
    // poll_request is nonblocking and returns one completion at most.
    [[nodiscard]] virtual bool poll_request(HttpResult& result) = 0;

    [[nodiscard]] virtual std::optional<Value> load_value(const std::string& name) const = 0;
    virtual void save_value(const std::string& name, const Value& value) = 0;
    virtual void log(std::string_view message) = 0;
};

} // namespace mb
