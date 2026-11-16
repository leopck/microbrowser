#pragma once

#include "microbrowser/types.h"

#include <span>
#include <stdexcept>

namespace mb {

class ScriptError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Propagates a successful page change out of the current event, not an application error.
class NavigationSignal final : public ScriptError {
public:
    NavigationSignal() : ScriptError("page navigation complete") {}
};

struct VmHost {
    virtual ~VmHost() = default;
    virtual Value read_state(uint32_t index) const = 0;
    virtual void write_state(uint32_t index, const Value& value) = 0;
    virtual Value builtin(uint8_t id, std::span<const Value> arguments) = 0;
};

// Calls, immediate validation, and bounds are checked on every event. Compilation
// checks these too; the runtime still verifies downloaded packages.
Value execute_script(const Package& package, uint32_t function_id,
                     std::span<const Value> arguments, VmHost& host,
                     uint32_t instruction_budget = 20'000);

} // namespace mb
