#pragma once

#include "microbrowser/vm.h"

#include <string_view>

namespace mb {

Value read_json_scalar(std::string_view document, std::string_view path,
                       Value::Type expected_type);

} // namespace mb
