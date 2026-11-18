#pragma once

#include "microbrowser/types.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mb {

enum class ReaderKind : uint8_t { article, manga, unsupported, native_package_confirmation };
enum class ReaderMode : uint8_t { reader, interactive };
enum class ReaderBlockKind : uint8_t { heading, paragraph, link, image, notice };

struct ReaderBlock {
    ReaderBlockKind kind = ReaderBlockKind::paragraph;
    std::string text;
    std::string url;
    std::optional<int> width;
    std::optional<int> height;
};

struct ReaderDocument {
    ReaderKind kind = ReaderKind::article;
    ReaderMode mode = ReaderMode::reader;
    std::string title;
    std::string source_url;
    std::optional<std::string> previous_url;
    std::optional<std::string> next_url;
    std::optional<std::string> session;
    bool interactive_available = false;
    std::optional<std::string> unsupported_reason;
    std::vector<ReaderBlock> blocks;
};

ReaderDocument parse_reader_document(std::string_view json);

enum class RemoteAction : uint8_t {
    none,
    submit_address,
    go_home,
    go_back,
    reader_url,
    interactive_url,
    run_native_package,
};

struct RemoteLink {
    Node* node = nullptr;
    RemoteAction action = RemoteAction::none;
    std::string url;
};

struct RemotePage {
    Page page;
    std::vector<RemoteLink> links;
    std::vector<StateField> states;
};

RemotePage make_reader_page(const ReaderDocument& document, std::string_view address,
                            Size viewport);

} // namespace mb
