#pragma once

#include "microbrowser/browser.h"
#include "reader.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mb {

enum class TransferKind : uint8_t { native_package, reader, direct_document, interactive };

// Browser's private state is separate from application bytecode and persistent values.
struct Browser::UrlState {
    using TransferKind = mb::TransferKind;

    struct PendingTransfer {
        uint32_t id = 0;
        std::string original_url;
        TransferKind kind = TransferKind::reader;
    };

    struct Image {
        std::string asset;
        std::string url;
        Node* node = nullptr; // Owned by the generated Page's unique_ptr children.
        std::size_t block_index = 0;
        uint32_t y = 0;
        uint16_t width = 0;
        uint16_t height = 0;
        uint32_t total_height = 0;
        bool queued = false;
        bool loaded = false;
        bool expanded = false;
        bool failed = false;
    };

    struct PendingImage {
        uint32_t id = 0;
        std::size_t image = 0;
    };

    explicit UrlState(std::string gateway) : gateway_url(std::move(gateway)) {}

    [[nodiscard]] bool active() const { return generated_page != nullptr; }
    [[nodiscard]] uint32_t active_requests() const;

    std::vector<std::string> approved_packages;
    std::string gateway_url;
    std::string location;
    std::string session;
    std::vector<std::string> history;
    std::vector<uint8_t> home_package;
    std::optional<ReaderDocument> document;
    std::unique_ptr<RemotePage> generated_page;
    std::optional<PendingTransfer> pending_transfer;
    std::vector<Image> images;
    std::vector<PendingImage> pending_images;
    std::size_t cached_image_bytes = 0;
    ReaderMode mode = ReaderMode::reader;
};

// Gateway origins are deployment configuration, not website-provided metadata.
[[nodiscard]] std::string validated_gateway_origin(const std::string& value);

} // namespace mb
