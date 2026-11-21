#pragma once

#include "freertos/FreeRTOS.h"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace mb::detail {

// Queued retired work performs no hardware transaction, so one I/O timeout plus
// scheduling/delivery slack bounds a normal worker exit. Multiplication occurs
// in 64 bits before conversion to the target's TickType_t.
[[nodiscard]] inline TickType_t worker_shutdown_ticks(
    std::uint32_t io_timeout_ms) noexcept {
    constexpr std::uint64_t shutdown_slack_ms = 1000;
    const std::uint64_t wait_ms = static_cast<std::uint64_t>(io_timeout_ms) + shutdown_slack_ms;
    const std::uint64_t rounded_ticks = (wait_ms * configTICK_RATE_HZ + 999U) / 1000U;
    const auto maximum = static_cast<std::uint64_t>(std::numeric_limits<TickType_t>::max());
    const std::uint64_t ticks = std::max<std::uint64_t>(1, std::min(rounded_ticks, maximum));
    return static_cast<TickType_t>(ticks);
}

} // namespace mb::detail
