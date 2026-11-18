#pragma once

#include "microbrowser/types.h"

#include <cstdint>
#include <optional>
#include <string>

namespace mb {

enum class ContentRequestMethod : uint8_t { get, post };

struct ContentRequest {
    std::string url;
    ContentRequestMethod method = ContentRequestMethod::get;
    std::string body;
};

struct HttpResult {
    uint32_t id = 0;
    int status = 0;
    std::string body;
    std::string error;
    std::string content_type;
    std::string response_url;
    std::string total_height;
    std::string image_width;
    std::string image_height;
    std::string image_y;
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

    // start_content returns after queuing work, never after network I/O.
    virtual void start_content(uint32_t id, ContentRequest request) = 0;
    // poll_content is nonblocking and returns one content completion at most.
    [[nodiscard]] virtual bool poll_content(HttpResult& result) = 0;

    [[nodiscard]] virtual std::optional<Value> load_value(const std::string& name) const = 0;
    virtual void save_value(const std::string& name, const Value& value) = 0;
    virtual void log(std::string_view message) = 0;
};

} // namespace mb
