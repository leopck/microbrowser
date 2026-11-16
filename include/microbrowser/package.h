#pragma once

#include "microbrowser/types.h"

#include <cstdint>
#include <stdexcept>
#include <vector>

namespace mb {

class PackageError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

Package parse_package(const std::vector<uint8_t>& bytes);
std::vector<uint8_t> read_package_file(const char* path);

} // namespace mb
