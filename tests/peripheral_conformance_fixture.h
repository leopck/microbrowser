#pragma once

#include "microbrowser/peripherals.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

struct ConformanceResult {
    mb::IoStatus status = mb::IoStatus::completed;
    mb::DeviceResponse response = std::monostate{};
};

inline ConformanceResult modeled_success(mb::DeviceResponse response) {
    return {mb::IoStatus::completed, std::move(response)};
}

inline ConformanceResult modeled_failure(mb::IoStatus status) {
    return {status, std::monostate{}};
}

inline mb::ByteBufferPtr conformance_bytes(const mb::ByteBuffer& bytes) {
    return mb::make_immutable_bytes(std::span<const std::uint8_t>(bytes));
}

class ConformanceBackend : public mb::PeripheralBackend {
public:
    ConformanceBackend(mb::CapabilityVersion version, std::vector<mb::DeviceOperation> operations)
        : m_version(version), m_operations(std::move(operations)) {}

    [[nodiscard]] std::span<const mb::DeviceOperation> operations() const noexcept final {
        return m_operations;
    }

    mb::IoStatus start_operation(const mb::BackendOperation& operation,
                                 std::weak_ptr<mb::PeripheralCompletionSink> completions) final {
        if (std::find(m_operations.begin(), m_operations.end(), operation.id) == m_operations.end()) {
            return mb::IoStatus::unsupported;
        }
        if (operation.id == mb::DeviceOperation::open) {
            if (!std::holds_alternative<mb::OpenRequest>(operation.request)) {
                return mb::IoStatus::invalid_argument;
            }
            if (m_open || m_open_pending) return mb::IoStatus::busy;
        } else {
            if (!m_open || (m_device && operation.device != *m_device)) {
                return mb::IoStatus::invalid_argument;
            }
            if (operation.id == mb::DeviceOperation::close &&
                !std::holds_alternative<mb::CloseRequest>(operation.request)) {
                return mb::IoStatus::invalid_argument;
            }
        }

        if (operation.id != mb::DeviceOperation::open && operation.id != mb::DeviceOperation::close) {
            const mb::IoStatus status = validate_model(operation);
            if (status != mb::IoStatus::completed) return status;
        }
        m_pending.push_back(Pending{operation, std::move(completions)});
        if (operation.id == mb::DeviceOperation::open) {
            m_open_pending = true;
        } else if (!m_device) {
            m_device = operation.device;
        }
        return mb::IoStatus::completed;
    }

    mb::IoStatus cancel_operation(const mb::BackendOperation&) final {
        return mb::IoStatus::unsupported;
    }

    void release_application(std::uint64_t owner_generation) final {
        m_released_owners.push_back(owner_generation);
        std::erase_if(m_subscriptions, [&](const Subscription& subscription) {
            return subscription.operation.owner_generation == owner_generation;
        });
    }

    static void deliver_pending(mb::PeripheralBackend& backend) {
        static_cast<ConformanceBackend&>(backend).deliver_one();
    }

    void deliver_one() {
        if (m_pending.empty()) return;
        Pending pending = std::move(m_pending.front());
        m_pending.pop_front();
        m_delivering_completions = pending.completions;

        ConformanceResult result;
        if (pending.cancellation_requested) {
            result = modeled_failure(mb::IoStatus::cancelled);
        } else if (pending.operation.id == mb::DeviceOperation::open) {
            m_open_pending = false;
            m_open = true;
            result = modeled_success(mb::OpenResponse{{}, m_version, true});
        } else if (pending.operation.id == mb::DeviceOperation::close) {
            close_model();
            forget_device_subscriptions(pending.operation.device);
            m_open = false;
            m_device.reset();
            result = modeled_success(mb::EmptyResponse{});
        } else {
            result = complete_model(pending.operation);
        }

        const auto completions = pending.completions.lock();
        m_delivering_completions.reset();
        if (completions) {
            completions->complete(pending.operation, result.status, std::move(result.response));
        }
    }

    [[nodiscard]] std::size_t pending_count() const { return m_pending.size(); }

    [[nodiscard]] const mb::BackendOperation& pending_operation(std::size_t index = 0) const {
        if (index >= m_pending.size()) throw std::runtime_error("conformance model has no requested pending operation");
        return (m_pending.begin() + static_cast<std::ptrdiff_t>(index))->operation;
    }

protected:
    struct Subscription {
        mb::BackendOperation operation;
        std::weak_ptr<mb::PeripheralCompletionSink> completions;
    };

    virtual mb::IoStatus validate_model(const mb::BackendOperation& operation) = 0;
    virtual ConformanceResult complete_model(const mb::BackendOperation& operation) = 0;
    virtual void close_model() {}

    void remember_subscription(const mb::BackendOperation& operation) {
        if (!m_delivering_completions) throw std::runtime_error("subscription retained outside model delivery");
        m_subscriptions.push_back(Subscription{operation, *m_delivering_completions});
    }

    void forget_subscription(mb::DeviceHandle subscription) {
        std::erase_if(m_subscriptions, [&](const Subscription& candidate) {
            return candidate.operation.subscription == subscription;
        });
    }

    [[nodiscard]] bool has_subscription(mb::DeviceHandle device, mb::DeviceHandle subscription) const {
        return std::any_of(m_subscriptions.begin(), m_subscriptions.end(), [&](const Subscription& candidate) {
            return candidate.operation.device == device && candidate.operation.subscription == subscription;
        });
    }

    mb::IoStatus publish(const Subscription& subscription, mb::DeviceResponse response) const {
        const auto completions = subscription.completions.lock();
        if (!completions) return mb::IoStatus::device_lost;
        return completions->publish(subscription.operation.owner_generation,
                                    subscription.operation.subscription, std::move(response));
    }

    [[nodiscard]] std::vector<Subscription>& subscriptions() { return m_subscriptions; }

private:
    struct Pending {
        mb::BackendOperation operation;
        std::weak_ptr<mb::PeripheralCompletionSink> completions;
        bool cancellation_requested = false;
    };

    void forget_device_subscriptions(mb::DeviceHandle device) {
        std::erase_if(m_subscriptions, [&](const Subscription& subscription) {
            return subscription.operation.device == device;
        });
    }

    mb::CapabilityVersion m_version;
    std::vector<mb::DeviceOperation> m_operations;
    std::deque<Pending> m_pending;
    std::vector<Subscription> m_subscriptions;
    std::vector<std::uint64_t> m_released_owners;
    std::optional<std::weak_ptr<mb::PeripheralCompletionSink>> m_delivering_completions;
    std::optional<mb::DeviceHandle> m_device;
    bool m_open = false;
    bool m_open_pending = false;
};

constexpr mb::CapabilityVersion conformance_version(mb::Capability capability) {
    return {capability, mb::peripheral_abi_major, 0};
}

class SpiModel final : public ConformanceBackend {
public:
    SpiModel()
        : ConformanceBackend(conformance_version(mb::Capability::spi_controller),
                             {mb::DeviceOperation::open, mb::DeviceOperation::configure,
                              mb::DeviceOperation::start, mb::DeviceOperation::stop,
                              mb::DeviceOperation::transfer, mb::DeviceOperation::close}) {}

    [[nodiscard]] std::size_t transfer_count() const { return m_transfer_count; }
    [[nodiscard]] std::size_t transmitted_bytes() const { return m_transmitted_bytes; }
    [[nodiscard]] std::size_t received_bytes() const { return m_received_bytes; }
    [[nodiscard]] const mb::SpiConfigureRequest& configuration() const { return m_configuration; }

private:
    mb::IoStatus validate_model(const mb::BackendOperation& operation) final {
        if (const auto* configure = std::get_if<mb::SpiConfigureRequest>(&operation.request)) {
            if (operation.id != mb::DeviceOperation::configure || configure->frequency_hz == 0) {
                return mb::IoStatus::invalid_argument;
            }
            return m_started ? mb::IoStatus::busy : mb::IoStatus::completed;
        }
        if (operation.id == mb::DeviceOperation::start) {
            if (!std::holds_alternative<mb::StartRequest>(operation.request)) return mb::IoStatus::invalid_argument;
            return m_configured && !m_started ? mb::IoStatus::completed : mb::IoStatus::busy;
        }
        if (operation.id == mb::DeviceOperation::stop) {
            if (!std::holds_alternative<mb::StopRequest>(operation.request)) return mb::IoStatus::invalid_argument;
            return m_started ? mb::IoStatus::completed : mb::IoStatus::busy;
        }
        const auto* transfer = std::get_if<mb::SpiTransferRequest>(&operation.request);
        if (operation.id != mb::DeviceOperation::transfer || !transfer || !transfer->transmit) {
            return mb::IoStatus::invalid_argument;
        }
        return m_configured && m_started ? mb::IoStatus::completed : mb::IoStatus::busy;
    }

    ConformanceResult complete_model(const mb::BackendOperation& operation) final {
        if (const auto* configure = std::get_if<mb::SpiConfigureRequest>(&operation.request)) {
            m_configuration = *configure;
            m_configured = true;
            return modeled_success(mb::SpiConfigureResponse{std::min(configure->frequency_hz, 8'000'000U)});
        }
        if (operation.id == mb::DeviceOperation::start) {
            m_started = true;
            return modeled_success(mb::EmptyResponse{});
        }
        if (operation.id == mb::DeviceOperation::stop) {
            m_started = false;
            return modeled_success(mb::EmptyResponse{});
        }
        const auto& transfer = std::get<mb::SpiTransferRequest>(operation.request);
        ++m_transfer_count;
        mb::ByteBuffer received(transfer.receive_bytes);
        for (std::size_t index = 0; index < received.size(); ++index) {
            if (index == 0) {
                received[index] = 0x00;
            } else if (index == 1) {
                received[index] = 0xff;
            } else if (index == 2) {
                received[index] = 0x80;
            } else {
                received[index] = static_cast<std::uint8_t>(m_transfer_count + index - 3U);
            }
        }
        m_transmitted_bytes += transfer.transmit->size();
        m_received_bytes += received.size();
        const std::size_t transferred = std::max(transfer.transmit->size(), received.size());
        return modeled_success(mb::SpiTransferResponse{conformance_bytes(received), transferred});
    }

    void close_model() final {
        m_configured = false;
        m_started = false;
    }

    mb::SpiConfigureRequest m_configuration{};
    std::size_t m_transfer_count = 0;
    std::size_t m_transmitted_bytes = 0;
    std::size_t m_received_bytes = 0;
    bool m_configured = false;
    bool m_started = false;
};

class UartModel final : public ConformanceBackend {
public:
    static constexpr std::size_t receive_capacity = 12;

    UartModel()
        : ConformanceBackend(conformance_version(mb::Capability::uart),
                             {mb::DeviceOperation::open, mb::DeviceOperation::configure,
                              mb::DeviceOperation::start, mb::DeviceOperation::stop,
                              mb::DeviceOperation::read, mb::DeviceOperation::write,
                              mb::DeviceOperation::close}) {}

    [[nodiscard]] std::size_t queued_bytes() const { return m_receive.size(); }
    [[nodiscard]] std::size_t transmitted_bytes() const { return m_transmitted_bytes; }
    [[nodiscard]] const mb::UartConfigureRequest& configuration() const { return m_configuration; }

private:
    mb::IoStatus validate_model(const mb::BackendOperation& operation) final {
        if (const auto* configure = std::get_if<mb::UartConfigureRequest>(&operation.request)) {
            if (operation.id != mb::DeviceOperation::configure || configure->baud == 0 ||
                configure->data_bits < 5 || configure->data_bits > 8 ||
                (configure->stop_bits != 1 && configure->stop_bits != 2)) {
                return mb::IoStatus::invalid_argument;
            }
            return m_started ? mb::IoStatus::busy : mb::IoStatus::completed;
        }
        if (operation.id == mb::DeviceOperation::start) {
            if (!std::holds_alternative<mb::StartRequest>(operation.request)) return mb::IoStatus::invalid_argument;
            return m_configured && !m_started ? mb::IoStatus::completed : mb::IoStatus::busy;
        }
        if (operation.id == mb::DeviceOperation::stop) {
            if (!std::holds_alternative<mb::StopRequest>(operation.request)) return mb::IoStatus::invalid_argument;
            return m_started ? mb::IoStatus::completed : mb::IoStatus::busy;
        }
        if (!m_configured || !m_started) return mb::IoStatus::busy;
        if (const auto* write = std::get_if<mb::UartWriteRequest>(&operation.request)) {
            if (operation.id != mb::DeviceOperation::write || !write->bytes) return mb::IoStatus::invalid_argument;
            return write->bytes->size() <= receive_capacity - m_receive.size()
                       ? mb::IoStatus::completed
                       : mb::IoStatus::busy;
        }
        const auto* read = std::get_if<mb::UartReadRequest>(&operation.request);
        if (operation.id != mb::DeviceOperation::read || !read || read->maximum_bytes == 0) {
            return mb::IoStatus::invalid_argument;
        }
        return mb::IoStatus::completed;
    }

    ConformanceResult complete_model(const mb::BackendOperation& operation) final {
        if (const auto* configure = std::get_if<mb::UartConfigureRequest>(&operation.request)) {
            m_configuration = *configure;
            m_configured = true;
            return modeled_success(mb::UartConfigureResponse{configure->baud});
        }
        if (operation.id == mb::DeviceOperation::start) {
            m_started = true;
            return modeled_success(mb::EmptyResponse{});
        }
        if (operation.id == mb::DeviceOperation::stop) {
            m_started = false;
            return modeled_success(mb::EmptyResponse{});
        }
        if (const auto* write = std::get_if<mb::UartWriteRequest>(&operation.request)) {
            m_receive.insert(m_receive.end(), write->bytes->begin(), write->bytes->end());
            m_transmitted_bytes += write->bytes->size();
            return modeled_success(mb::UartWriteResponse{write->bytes->size()});
        }
        const auto& read = std::get<mb::UartReadRequest>(operation.request);
        const std::size_t count = std::min<std::size_t>(read.maximum_bytes, m_receive.size());
        mb::ByteBuffer bytes(count);
        for (std::uint8_t& byte : bytes) {
            byte = m_receive.front();
            m_receive.pop_front();
        }
        return modeled_success(mb::UartReadResponse{conformance_bytes(bytes), m_timestamp_ns++, {}});
    }

    void close_model() final {
        m_configured = false;
        m_started = false;
        m_receive.clear();
    }

    mb::UartConfigureRequest m_configuration{};
    std::deque<std::uint8_t> m_receive;
    std::size_t m_transmitted_bytes = 0;
    std::int64_t m_timestamp_ns = 9'007'199'254'741'081LL;
    bool m_configured = false;
    bool m_started = false;
};

class I2cModel final : public ConformanceBackend {
public:
    static constexpr std::uint16_t primary_address = 0x52;
    static constexpr std::uint16_t ten_bit_address = 0x2a5;

    I2cModel()
        : ConformanceBackend(conformance_version(mb::Capability::i2c_controller),
                             {mb::DeviceOperation::open, mb::DeviceOperation::configure,
                              mb::DeviceOperation::read, mb::DeviceOperation::write,
                              mb::DeviceOperation::transfer, mb::DeviceOperation::close}) {}

    struct Transaction {
        mb::DeviceOperation operation;
        std::uint16_t address;
        bool ten_bit;
        std::size_t written;
        std::size_t read;
        bool repeated_start;
    };

    [[nodiscard]] const std::vector<Transaction>& transactions() const { return m_transactions; }
    [[nodiscard]] std::size_t repeated_start_count() const { return m_repeated_starts; }

private:
    struct Registers {
        std::array<std::uint8_t, 256> values{};
        std::uint8_t selected = 0;
    };

    Registers* registers(std::uint16_t address, bool ten_bit) {
        if (!ten_bit && address == primary_address) return &m_primary;
        if (ten_bit && address == ten_bit_address) return &m_ten_bit;
        return nullptr;
    }

    static void write_registers(Registers& registers, const mb::ByteBuffer& bytes) {
        if (bytes.empty()) return;
        registers.selected = bytes.front();
        for (std::size_t index = 1; index < bytes.size(); ++index) {
            registers.values[registers.selected] = bytes[index];
            registers.selected = static_cast<std::uint8_t>(registers.selected + 1U);
        }
    }

    static mb::ByteBufferPtr read_registers(Registers& registers, std::uint32_t count) {
        mb::ByteBuffer bytes(count);
        for (std::uint8_t& byte : bytes) {
            byte = registers.values[registers.selected];
            registers.selected = static_cast<std::uint8_t>(registers.selected + 1U);
        }
        return conformance_bytes(bytes);
    }

    mb::IoStatus validate_model(const mb::BackendOperation& operation) final {
        if (const auto* configure = std::get_if<mb::I2cConfigureRequest>(&operation.request)) {
            return operation.id == mb::DeviceOperation::configure && configure->frequency_hz != 0
                       ? mb::IoStatus::completed
                       : mb::IoStatus::invalid_argument;
        }
        if (!m_configured) return mb::IoStatus::busy;
        if (const auto* write = std::get_if<mb::I2cWriteRequest>(&operation.request)) {
            return operation.id == mb::DeviceOperation::write && write->bytes && !write->bytes->empty()
                       ? mb::IoStatus::completed
                       : mb::IoStatus::invalid_argument;
        }
        if (const auto* read = std::get_if<mb::I2cReadRequest>(&operation.request)) {
            return operation.id == mb::DeviceOperation::read && read->byte_count != 0 && read->byte_count <= 256
                       ? mb::IoStatus::completed
                       : mb::IoStatus::invalid_argument;
        }
        const auto* transfer = std::get_if<mb::I2cTransferRequest>(&operation.request);
        if (operation.id != mb::DeviceOperation::transfer || !transfer || !transfer->write_bytes ||
            transfer->write_bytes->empty() || transfer->read_bytes > 256) {
            return mb::IoStatus::invalid_argument;
        }
        return mb::IoStatus::completed;
    }

    ConformanceResult nack() const {
        return modeled_success(mb::I2cTransferResponse{conformance_bytes({}), 0, mb::I2cFault::nack, false});
    }

    ConformanceResult complete_model(const mb::BackendOperation& operation) final {
        if (const auto* configure = std::get_if<mb::I2cConfigureRequest>(&operation.request)) {
            m_configured = true;
            return modeled_success(mb::I2cConfigureResponse{configure->frequency_hz});
        }
        if (const auto* write = std::get_if<mb::I2cWriteRequest>(&operation.request)) {
            Registers* target = registers(write->address, write->ten_bit_address);
            if (!target) return nack();
            write_registers(*target, *write->bytes);
            m_transactions.push_back({operation.id, write->address, write->ten_bit_address,
                                      write->bytes->size(), 0, false});
            return modeled_success(mb::I2cTransferResponse{nullptr, write->bytes->size(), mb::I2cFault::none, false});
        }
        if (const auto* read = std::get_if<mb::I2cReadRequest>(&operation.request)) {
            Registers* target = registers(read->address, read->ten_bit_address);
            if (!target) return nack();
            auto received = read_registers(*target, read->byte_count);
            m_transactions.push_back({operation.id, read->address, read->ten_bit_address, 0, received->size(), false});
            return modeled_success(mb::I2cTransferResponse{std::move(received), 0, mb::I2cFault::none, false});
        }
        const auto& transfer = std::get<mb::I2cTransferRequest>(operation.request);
        Registers* target = registers(transfer.address, transfer.ten_bit_address);
        if (!target) return nack();
        write_registers(*target, *transfer.write_bytes);
        auto received = read_registers(*target, transfer.read_bytes);
        const bool repeated = transfer.repeated_start && transfer.read_bytes != 0;
        if (repeated) ++m_repeated_starts;
        m_transactions.push_back({operation.id, transfer.address, transfer.ten_bit_address,
                                  transfer.write_bytes->size(), received->size(), repeated});
        return modeled_success(mb::I2cTransferResponse{std::move(received), transfer.write_bytes->size(),
                                                       mb::I2cFault::none, repeated});
    }

    void close_model() final { m_configured = false; }

    Registers m_primary;
    Registers m_ten_bit;
    std::vector<Transaction> m_transactions;
    std::size_t m_repeated_starts = 0;
    bool m_configured = false;
};

class AdcModel final : public ConformanceBackend {
public:
    AdcModel()
        : ConformanceBackend(conformance_version(mb::Capability::adc),
                             {mb::DeviceOperation::open, mb::DeviceOperation::configure,
                              mb::DeviceOperation::read, mb::DeviceOperation::close}) {}

private:
    mb::IoStatus validate_model(const mb::BackendOperation& operation) final {
        if (const auto* configure = std::get_if<mb::AdcConfigureRequest>(&operation.request)) {
            return operation.id == mb::DeviceOperation::configure && configure->sample_time_ns != 0 &&
                           configure->reference_microvolts != 0
                       ? mb::IoStatus::completed
                       : mb::IoStatus::invalid_argument;
        }
        if (operation.id != mb::DeviceOperation::read || !std::holds_alternative<mb::AdcReadRequest>(operation.request)) {
            return mb::IoStatus::invalid_argument;
        }
        return m_configured ? mb::IoStatus::completed : mb::IoStatus::busy;
    }

    ConformanceResult complete_model(const mb::BackendOperation& operation) final {
        if (const auto* configure = std::get_if<mb::AdcConfigureRequest>(&operation.request)) {
            m_sample_time_ns = configure->sample_time_ns;
            m_reference_microvolts = configure->reference_microvolts;
            m_configured = true;
            return modeled_success(mb::AdcConfigureResponse{m_sample_time_ns, static_cast<std::uint32_t>(m_reference_microvolts), 18, true});
        }
        const std::int64_t sample = m_next_sample;
        m_next_sample += 2;
        return modeled_success(mb::AdcReadResponse{sample, m_reference_microvolts, 18, m_next_timestamp_ns++, true});
    }

    void close_model() final { m_configured = false; }

    std::uint64_t m_sample_time_ns = 0;
    std::int64_t m_reference_microvolts = 0;
    std::int64_t m_next_sample = 9'007'199'254'740'995LL;
    std::int64_t m_next_timestamp_ns = 9'007'199'254'740'999LL;
    bool m_configured = false;
};

class PwmModel final : public ConformanceBackend {
public:
    PwmModel()
        : ConformanceBackend(conformance_version(mb::Capability::pwm),
                             {mb::DeviceOperation::open, mb::DeviceOperation::configure,
                              mb::DeviceOperation::start, mb::DeviceOperation::stop,
                              mb::DeviceOperation::write, mb::DeviceOperation::close}) {}

    [[nodiscard]] std::uint64_t active_time_ns() const { return m_active_time_ns; }
    [[nodiscard]] std::uint64_t period_ns() const { return m_period_ns; }
    [[nodiscard]] std::size_t write_count() const { return m_write_count; }

private:
    mb::IoStatus validate_model(const mb::BackendOperation& operation) final {
        if (const auto* configure = std::get_if<mb::PwmConfigureRequest>(&operation.request)) {
            if (operation.id != mb::DeviceOperation::configure || configure->period_ns == 0 ||
                configure->initial_active_time_ns > configure->period_ns) {
                return mb::IoStatus::invalid_argument;
            }
            return m_started ? mb::IoStatus::busy : mb::IoStatus::completed;
        }
        if (operation.id == mb::DeviceOperation::start) {
            if (!std::holds_alternative<mb::StartRequest>(operation.request)) return mb::IoStatus::invalid_argument;
            return m_configured && !m_started ? mb::IoStatus::completed : mb::IoStatus::busy;
        }
        if (operation.id == mb::DeviceOperation::stop) {
            if (!std::holds_alternative<mb::StopRequest>(operation.request)) return mb::IoStatus::invalid_argument;
            return m_started ? mb::IoStatus::completed : mb::IoStatus::busy;
        }
        const auto* write = std::get_if<mb::PwmWriteRequest>(&operation.request);
        if (operation.id != mb::DeviceOperation::write || !write) return mb::IoStatus::invalid_argument;
        if (!m_configured) return mb::IoStatus::busy;
        return write->active_time_ns <= m_period_ns ? mb::IoStatus::completed : mb::IoStatus::invalid_argument;
    }

    ConformanceResult complete_model(const mb::BackendOperation& operation) final {
        if (const auto* configure = std::get_if<mb::PwmConfigureRequest>(&operation.request)) {
            m_period_ns = configure->period_ns;
            m_active_time_ns = configure->initial_active_time_ns;
            m_configured = true;
            return modeled_success(mb::PwmConfigureResponse{m_period_ns});
        }
        if (operation.id == mb::DeviceOperation::start) {
            m_started = true;
            return modeled_success(mb::EmptyResponse{});
        }
        if (operation.id == mb::DeviceOperation::stop) {
            m_started = false;
            return modeled_success(mb::EmptyResponse{});
        }
        m_active_time_ns = std::get<mb::PwmWriteRequest>(operation.request).active_time_ns;
        ++m_write_count;
        return modeled_success(mb::EmptyResponse{});
    }

    void close_model() final {
        m_configured = false;
        m_started = false;
    }

    std::uint64_t m_period_ns = 0;
    std::uint64_t m_active_time_ns = 0;
    std::size_t m_write_count = 0;
    bool m_configured = false;
    bool m_started = false;
};

class PulseCaptureModel final : public ConformanceBackend {
public:
    PulseCaptureModel()
        : ConformanceBackend(conformance_version(mb::Capability::pulse_capture),
                             {mb::DeviceOperation::open, mb::DeviceOperation::configure,
                              mb::DeviceOperation::read, mb::DeviceOperation::close}) {}

    bool inject(const std::vector<mb::PulseSample>& samples) {
        if (!m_configured || m_samples.size() + samples.size() > capture_capacity) return false;
        for (const auto& sample : samples) {
            if (sample.duration_ns >= static_cast<std::int64_t>(m_minimum_duration_ns)) {
                m_samples.push_back(sample);
            }
        }
        return true;
    }

private:
    static constexpr std::size_t capture_capacity = 8;

    mb::IoStatus validate_model(const mb::BackendOperation& operation) final {
        if (const auto* configure = std::get_if<mb::PulseCaptureConfigureRequest>(&operation.request)) {
            return operation.id == mb::DeviceOperation::configure && configure->minimum_duration_ns != 0 &&
                           configure->maximum_pulses != 0 && configure->maximum_pulses <= capture_capacity
                       ? mb::IoStatus::completed
                       : mb::IoStatus::invalid_argument;
        }
        const auto* read = std::get_if<mb::PulseCaptureReadRequest>(&operation.request);
        if (operation.id != mb::DeviceOperation::read || !read || read->maximum_pulses == 0 ||
            read->maximum_pulses > m_maximum_pulses) {
            return mb::IoStatus::invalid_argument;
        }
        return m_configured ? mb::IoStatus::completed : mb::IoStatus::busy;
    }

    ConformanceResult complete_model(const mb::BackendOperation& operation) final {
        if (const auto* configure = std::get_if<mb::PulseCaptureConfigureRequest>(&operation.request)) {
            m_minimum_duration_ns = configure->minimum_duration_ns;
            m_maximum_pulses = configure->maximum_pulses;
            m_configured = true;
            return modeled_success(mb::PulseCaptureConfigureResponse{m_minimum_duration_ns});
        }
        const auto& read = std::get<mb::PulseCaptureReadRequest>(operation.request);
        const std::size_t count = std::min<std::size_t>(read.maximum_pulses, m_samples.size());
        std::vector<mb::PulseSample> pulses;
        pulses.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            pulses.push_back(m_samples.front());
            m_samples.pop_front();
        }
        return modeled_success(mb::PulseCaptureReadResponse{std::move(pulses)});
    }

    void close_model() final {
        m_configured = false;
        m_samples.clear();
    }

    std::deque<mb::PulseSample> m_samples;
    std::uint64_t m_minimum_duration_ns = 0;
    std::uint32_t m_maximum_pulses = 0;
    bool m_configured = false;
};

class PulseOutputModel final : public ConformanceBackend {
public:
    PulseOutputModel()
        : ConformanceBackend(conformance_version(mb::Capability::pulse_output),
                             {mb::DeviceOperation::open, mb::DeviceOperation::configure,
                              mb::DeviceOperation::start, mb::DeviceOperation::stop,
                              mb::DeviceOperation::write, mb::DeviceOperation::close}) {}

    [[nodiscard]] const mb::NanosecondSequence& last_sequence() const { return m_last_sequence; }

private:
    mb::IoStatus validate_model(const mb::BackendOperation& operation) final {
        if (const auto* configure = std::get_if<mb::PulseOutputConfigureRequest>(&operation.request)) {
            return operation.id == mb::DeviceOperation::configure && configure->resolution_ns != 0
                       ? mb::IoStatus::completed
                       : mb::IoStatus::invalid_argument;
        }
        if (operation.id == mb::DeviceOperation::start) {
            if (!std::holds_alternative<mb::StartRequest>(operation.request)) return mb::IoStatus::invalid_argument;
            return m_configured && !m_started ? mb::IoStatus::completed : mb::IoStatus::busy;
        }
        if (operation.id == mb::DeviceOperation::stop) {
            if (!std::holds_alternative<mb::StopRequest>(operation.request)) return mb::IoStatus::invalid_argument;
            return m_started ? mb::IoStatus::completed : mb::IoStatus::busy;
        }
        const auto* write = std::get_if<mb::PulseOutputWriteRequest>(&operation.request);
        if (operation.id != mb::DeviceOperation::write || !write || !write->durations_ns ||
            write->durations_ns->empty()) {
            return mb::IoStatus::invalid_argument;
        }
        if (!m_configured || !m_started) return mb::IoStatus::busy;
        return std::all_of(write->durations_ns->begin(), write->durations_ns->end(), [&](std::uint64_t duration) {
                   return duration != 0 && duration % m_resolution_ns == 0;
               })
                   ? mb::IoStatus::completed
                   : mb::IoStatus::invalid_argument;
    }

    ConformanceResult complete_model(const mb::BackendOperation& operation) final {
        if (const auto* configure = std::get_if<mb::PulseOutputConfigureRequest>(&operation.request)) {
            m_resolution_ns = configure->resolution_ns;
            m_idle_level = configure->idle_level;
            m_configured = true;
            return modeled_success(mb::PulseOutputConfigureResponse{m_resolution_ns});
        }
        if (operation.id == mb::DeviceOperation::start) {
            m_started = true;
            return modeled_success(mb::EmptyResponse{});
        }
        if (operation.id == mb::DeviceOperation::stop) {
            m_started = false;
            return modeled_success(mb::EmptyResponse{});
        }
        const auto& write = std::get<mb::PulseOutputWriteRequest>(operation.request);
        m_last_sequence = *write.durations_ns;
        return modeled_success(mb::PulseOutputWriteResponse{m_last_sequence.size()});
    }

    void close_model() final {
        m_configured = false;
        m_started = false;
    }

    mb::NanosecondSequence m_last_sequence;
    std::uint64_t m_resolution_ns = 1;
    mb::PulseLevel m_idle_level = mb::PulseLevel::low;
    bool m_configured = false;
    bool m_started = false;
};

enum class PcmRole { i2s_transmit, i2s_receive, pdm_receive };

class PcmModel final : public ConformanceBackend {
public:
    explicit PcmModel(PcmRole role)
        : ConformanceBackend(conformance_version(capability(role)), operations(role)), m_role(role) {}

    bool inject_capture(const mb::ByteBuffer& bytes) {
        if (!m_configured || m_role == PcmRole::i2s_transmit || bytes.size() % frame_bytes() != 0) return false;
        if (bytes.size() > m_capture_capacity - m_capture.size()) return false;
        m_capture.insert(m_capture.end(), bytes.begin(), bytes.end());
        return true;
    }

    [[nodiscard]] std::size_t capture_bytes() const { return m_capture.size(); }
    [[nodiscard]] std::uint64_t transmitted_frames() const { return m_transmitted_frames; }
    [[nodiscard]] std::size_t frame_bytes() const { return m_frame_bytes; }
    [[nodiscard]] const mb::ByteBuffer& last_transmit() const { return m_last_transmit; }

private:
    static mb::Capability capability(PcmRole role) {
        switch (role) {
        case PcmRole::i2s_transmit: return mb::Capability::i2s_transmit;
        case PcmRole::i2s_receive: return mb::Capability::i2s_receive;
        case PcmRole::pdm_receive: return mb::Capability::pdm_receive;
        }
        throw std::logic_error("unknown PCM model role");
    }

    static std::vector<mb::DeviceOperation> operations(PcmRole role) {
        std::vector<mb::DeviceOperation> result{mb::DeviceOperation::open, mb::DeviceOperation::configure,
                                               mb::DeviceOperation::start, mb::DeviceOperation::stop};
        if (role == PcmRole::i2s_transmit) result.push_back(mb::DeviceOperation::write);
        if (role != PcmRole::pdm_receive) result.push_back(mb::DeviceOperation::transfer);
        if (role != PcmRole::i2s_transmit) result.push_back(mb::DeviceOperation::read);
        result.push_back(mb::DeviceOperation::close);
        return result;
    }

    static std::size_t sample_bytes(mb::PcmSampleFormat format) {
        switch (format) {
        case mb::PcmSampleFormat::signed_16_le: return 2;
        case mb::PcmSampleFormat::signed_24_le: return 3;
        case mb::PcmSampleFormat::signed_32_le:
        case mb::PcmSampleFormat::float_32_le: return 4;
        }
        return 0;
    }

    std::size_t format_frame_bytes(const mb::PcmFormat& format) const {
        return static_cast<std::size_t>(format.channel_count) * sample_bytes(format.sample_format);
    }

    mb::IoStatus validate_format(const mb::PcmConfigureRequest& configure, const mb::DeviceBudget& budget,
                                 std::size_t& negotiated_frame_bytes) const {
        if (configure.format.frame_rate_hz == 0 || configure.format.channel_count == 0 ||
            configure.maximum_frames_per_block == 0) {
            return mb::IoStatus::invalid_argument;
        }
        if (m_role == PcmRole::pdm_receive) {
            if (configure.format.channel_count != 1 ||
                configure.format.sample_format != mb::PcmSampleFormat::signed_16_le) {
                return mb::IoStatus::invalid_argument;
            }
        } else if (configure.format.channel_count > 2) {
            return mb::IoStatus::invalid_argument;
        }
        negotiated_frame_bytes = format_frame_bytes(configure.format);
        if (negotiated_frame_bytes == 0) return mb::IoStatus::invalid_argument;
        const std::uint64_t block_bytes =
            static_cast<std::uint64_t>(negotiated_frame_bytes) * configure.maximum_frames_per_block;
        if (block_bytes > budget.maximum_bytes_per_buffer) return mb::IoStatus::memory_exhausted;
        return mb::IoStatus::completed;
    }

    mb::IoStatus validate_model(const mb::BackendOperation& operation) final {
        if (const auto* configure = std::get_if<mb::PcmConfigureRequest>(&operation.request)) {
            if (operation.id != mb::DeviceOperation::configure || m_started) return mb::IoStatus::invalid_argument;
            std::size_t negotiated = 0;
            return validate_format(*configure, operation.budget, negotiated);
        }
        if (operation.id == mb::DeviceOperation::start) {
            if (!std::holds_alternative<mb::StartRequest>(operation.request)) return mb::IoStatus::invalid_argument;
            return m_configured && !m_started ? mb::IoStatus::completed : mb::IoStatus::busy;
        }
        if (operation.id == mb::DeviceOperation::stop) {
            if (!std::holds_alternative<mb::StopRequest>(operation.request)) return mb::IoStatus::invalid_argument;
            return m_started ? mb::IoStatus::completed : mb::IoStatus::busy;
        }
        if (!m_configured || !m_started) return mb::IoStatus::busy;
        if (const auto* write = std::get_if<mb::PcmWriteRequest>(&operation.request)) {
            if (operation.id != mb::DeviceOperation::write || m_role != PcmRole::i2s_transmit || !write->frames ||
                write->frames->size() % m_frame_bytes != 0 || write->frames->empty()) {
                return mb::IoStatus::invalid_argument;
            }
            return write->frames->size() / m_frame_bytes <= m_maximum_frames
                       ? mb::IoStatus::completed
                       : mb::IoStatus::invalid_argument;
        }
        if (const auto* read = std::get_if<mb::PcmReadRequest>(&operation.request)) {
            return operation.id == mb::DeviceOperation::read && m_role != PcmRole::i2s_transmit &&
                           read->frame_count != 0 && read->frame_count <= m_maximum_frames
                       ? mb::IoStatus::completed
                       : mb::IoStatus::invalid_argument;
        }
        const auto* transfer = std::get_if<mb::PcmTransferRequest>(&operation.request);
        if (operation.id != mb::DeviceOperation::transfer || m_role == PcmRole::pdm_receive || !transfer ||
            !transfer->transmit_frames || transfer->receive_frames == 0 ||
            transfer->receive_frames > m_maximum_frames ||
            transfer->transmit_frames->size() % m_frame_bytes != 0 ||
            transfer->transmit_frames->size() / m_frame_bytes > m_maximum_frames) {
            return mb::IoStatus::invalid_argument;
        }
        return mb::IoStatus::completed;
    }

    mb::ByteBufferPtr take_capture(std::uint32_t requested_frames) {
        const std::size_t count = std::min<std::size_t>(requested_frames, m_capture.size() / m_frame_bytes);
        mb::ByteBuffer bytes(count * m_frame_bytes);
        for (std::uint8_t& byte : bytes) {
            byte = m_capture.front();
            m_capture.pop_front();
        }
        return conformance_bytes(bytes);
    }

    mb::ByteBufferPtr loopback(std::uint32_t frames) {
        mb::ByteBuffer bytes(static_cast<std::size_t>(frames) * m_frame_bytes);
        for (std::size_t index = 0; index < bytes.size(); ++index) {
            bytes[index] = static_cast<std::uint8_t>(m_sequence * 17 + index * 29);
        }
        return conformance_bytes(bytes);
    }

    ConformanceResult complete_model(const mb::BackendOperation& operation) final {
        if (const auto* configure = std::get_if<mb::PcmConfigureRequest>(&operation.request)) {
            m_format = configure->format;
            m_maximum_frames = configure->maximum_frames_per_block;
            m_frame_bytes = format_frame_bytes(m_format);
            m_capture_capacity = static_cast<std::size_t>(m_maximum_frames) * m_frame_bytes;
            m_capture.clear();
            m_configured = true;
            return modeled_success(mb::PcmConfigureResponse{m_format});
        }
        if (operation.id == mb::DeviceOperation::start) {
            m_started = true;
            return modeled_success(mb::EmptyResponse{});
        }
        if (operation.id == mb::DeviceOperation::stop) {
            m_started = false;
            return modeled_success(mb::EmptyResponse{});
        }
        if (const auto* write = std::get_if<mb::PcmWriteRequest>(&operation.request)) {
            const std::uint64_t frames = write->frames->size() / m_frame_bytes;
            m_last_transmit.assign(write->frames->begin(), write->frames->end());
            m_transmitted_frames += frames;
            return modeled_success(mb::PcmWriteResponse{frames});
        }
        if (const auto* read = std::get_if<mb::PcmReadRequest>(&operation.request)) {
            auto frames = take_capture(read->frame_count);
            const std::uint64_t count = frames->size() / m_frame_bytes;
            return modeled_success(mb::PcmReadResponse{std::move(frames), count, m_sequence++});
        }
        const auto& transfer = std::get<mb::PcmTransferRequest>(operation.request);
        m_last_transmit.assign(transfer.transmit_frames->begin(), transfer.transmit_frames->end());
        m_transmitted_frames += transfer.transmit_frames->size() / m_frame_bytes;
        auto frames = m_role == PcmRole::i2s_transmit ? loopback(transfer.receive_frames)
                                                       : take_capture(transfer.receive_frames);
        const std::uint64_t count = frames->size() / m_frame_bytes;
        return modeled_success(mb::PcmReadResponse{std::move(frames), count, m_sequence++});
    }

    void close_model() final {
        m_configured = false;
        m_started = false;
        m_capture.clear();
    }

    PcmRole m_role;
    mb::PcmFormat m_format{};
    std::deque<std::uint8_t> m_capture;
    mb::ByteBuffer m_last_transmit;
    std::size_t m_frame_bytes = 0;
    std::size_t m_capture_capacity = 0;
    std::uint32_t m_maximum_frames = 0;
    std::uint64_t m_transmitted_frames = 0;
    std::int64_t m_sequence = 0;
    bool m_configured = false;
    bool m_started = false;
};

inline bool same_uuid(const mb::BleUuid& left, const mb::BleUuid& right) {
    return left.length == right.length &&
           std::equal(left.bytes.begin(), left.bytes.begin() + left.length, right.bytes.begin());
}

inline bool same_address(const mb::BleAddress& left, const mb::BleAddress& right) {
    return left.bytes == right.bytes && left.random == right.random;
}

inline bool same_attribute(const mb::BleAttribute& left, const mb::BleAttribute& right) {
    return same_uuid(left.service, right.service) && same_uuid(left.characteristic, right.characteristic);
}

inline mb::BleAttribute conformance_ble_attribute() {
    return {mb::BleUuid::from_uint16(0xff00), mb::BleUuid::from_uint16(0xff01)};
}

inline mb::BleAttribute secondary_ble_attribute() {
    return {mb::BleUuid::from_uint16(0xff00), mb::BleUuid::from_uint16(0xff02)};
}

inline mb::BleAddress conformance_ble_address() {
    return {{0x01, 0x80, 0xff, 0x00, 0xc3, 0x28}, true};
}

inline mb::BleAddress secondary_ble_address() {
    return {{0x02, 0x00, 0x7f, 0x80, 0x10, 0x20}, false};
}

class BleCentralModel final : public ConformanceBackend {
public:
    BleCentralModel()
        : ConformanceBackend(conformance_version(mb::Capability::ble_central),
                             {mb::DeviceOperation::open, mb::DeviceOperation::discover,
                              mb::DeviceOperation::connect, mb::DeviceOperation::disconnect,
                              mb::DeviceOperation::read, mb::DeviceOperation::write,
                              mb::DeviceOperation::subscribe, mb::DeviceOperation::unsubscribe,
                              mb::DeviceOperation::close}),
          m_value(conformance_bytes({0x00, 0xff, 0x80})) {}

    mb::IoStatus emit(const mb::BleAttribute& attribute, const mb::ByteBuffer& bytes) {
        bool found = false;
        for (const auto& subscription : subscriptions()) {
            const auto& request = std::get<mb::BleSubscribeRequest>(subscription.operation.request);
            if (request.target != mb::BleSubscriptionTarget::characteristic ||
                !same_attribute(request.attribute, attribute)) {
                continue;
            }
            found = true;
            const mb::IoStatus status = publish(subscription, mb::BleReadResponse{attribute, conformance_bytes(bytes)});
            if (status != mb::IoStatus::completed) return status;
        }
        return found ? mb::IoStatus::completed : mb::IoStatus::invalid_argument;
    }

private:
    static std::vector<mb::BleCharacteristicDescriptor> descriptors() {
        return {{conformance_ble_attribute(), true, true, true},
                {secondary_ble_attribute(), true, false, false}};
    }

    static bool service_matches(const mb::BleAttribute& filter, const mb::BleAttribute& attribute) {
        return (filter.service.length == 0 || same_uuid(filter.service, attribute.service)) &&
               (filter.characteristic.length == 0 ||
                same_uuid(filter.characteristic, attribute.characteristic));
    }

    mb::IoStatus validate_model(const mb::BackendOperation& operation) final {
        if (const auto* discover = std::get_if<mb::BleDiscoverRequest>(&operation.request)) {
            if (operation.id != mb::DeviceOperation::discover || discover->maximum_results == 0) {
                return mb::IoStatus::invalid_argument;
            }
            if (discover->kind == mb::BleDiscoveryKind::scan) {
                return discover->filter.service.length == 0 && discover->filter.characteristic.length == 0
                           ? mb::IoStatus::completed
                           : mb::IoStatus::invalid_argument;
            }
            return m_connected ? mb::IoStatus::completed : mb::IoStatus::busy;
        }
        if (const auto* connect = std::get_if<mb::BleConnectRequest>(&operation.request)) {
            if (operation.id != mb::DeviceOperation::connect || connect->supervision_timeout_ms == 0) {
                return mb::IoStatus::invalid_argument;
            }
            if (m_connected) return mb::IoStatus::busy;
            const bool discovered = std::any_of(m_discovered.begin(), m_discovered.end(), [&](const mb::BleAddress& peer) {
                return same_address(peer, connect->peer);
            });
            return discovered ? mb::IoStatus::completed : mb::IoStatus::invalid_argument;
        }
        if (const auto* disconnect = std::get_if<mb::BleDisconnectRequest>(&operation.request)) {
            if (operation.id != mb::DeviceOperation::disconnect) return mb::IoStatus::invalid_argument;
            return m_connected && same_address(m_peer, disconnect->peer) ? mb::IoStatus::completed
                                                                         : mb::IoStatus::invalid_argument;
        }
        if (operation.id == mb::DeviceOperation::unsubscribe) {
            const auto* unsubscribe = std::get_if<mb::UnsubscribeRequest>(&operation.request);
            return unsubscribe && has_subscription(operation.device, unsubscribe->subscription)
                       ? mb::IoStatus::completed
                       : mb::IoStatus::invalid_argument;
        }
        if (const auto* subscribe = std::get_if<mb::BleSubscribeRequest>(&operation.request)) {
            if (operation.id != mb::DeviceOperation::subscribe) return mb::IoStatus::invalid_argument;
            if (subscribe->target == mb::BleSubscriptionTarget::connection_state) {
                return subscribe->attribute.service.length == 0 && subscribe->attribute.characteristic.length == 0
                           ? mb::IoStatus::completed
                           : mb::IoStatus::invalid_argument;
            }
            if (!m_connected) return mb::IoStatus::busy;
            return same_attribute(subscribe->attribute, conformance_ble_attribute())
                       ? mb::IoStatus::completed
                       : mb::IoStatus::invalid_argument;
        }
        if (!m_connected) return mb::IoStatus::busy;
        if (const auto* read = std::get_if<mb::BleReadRequest>(&operation.request)) {
            return operation.id == mb::DeviceOperation::read && known_attribute(read->attribute)
                       ? mb::IoStatus::completed
                       : mb::IoStatus::invalid_argument;
        }
        const auto* write = std::get_if<mb::BleWriteRequest>(&operation.request);
        return operation.id == mb::DeviceOperation::write && write && write->value && !write->value->empty() &&
                       write->value->size() <= 20 && same_attribute(write->attribute, conformance_ble_attribute())
                   ? mb::IoStatus::completed
                   : mb::IoStatus::invalid_argument;
    }

    static bool known_attribute(const mb::BleAttribute& attribute) {
        return same_attribute(attribute, conformance_ble_attribute()) ||
               same_attribute(attribute, secondary_ble_attribute());
    }

    void publish_connection(const mb::BleAddress& peer, bool connected, bool paired) {
        for (const auto& subscription : subscriptions()) {
            const auto& request = std::get<mb::BleSubscribeRequest>(subscription.operation.request);
            if (request.target != mb::BleSubscriptionTarget::connection_state) continue;
            if (publish(subscription, mb::BleConnectionResponse{peer, connected, paired}) != mb::IoStatus::completed) {
                throw std::runtime_error("modeled BLE connection notification was rejected");
            }
        }
    }

    ConformanceResult complete_model(const mb::BackendOperation& operation) final {
        if (const auto* discover = std::get_if<mb::BleDiscoverRequest>(&operation.request)) {
            if (discover->kind == mb::BleDiscoveryKind::scan) {
                const std::array<mb::BlePeer, 2> peers{
                    mb::BlePeer{conformance_ble_address(), -42, conformance_bytes({0x02, 0x00, 0xff, 0x80})},
                    mb::BlePeer{secondary_ble_address(), -67, conformance_bytes({0x01, 0xc3, 0x28})}};
                m_discovered.clear();
                std::vector<mb::BlePeer> response;
                for (const auto& peer : peers) {
                    if (response.size() == discover->maximum_results) break;
                    response.push_back(peer);
                    m_discovered.push_back(peer.address);
                }
                return modeled_success(mb::BleScanResponse{std::move(response)});
            }
            std::vector<mb::BleCharacteristicDescriptor> characteristics;
            for (const auto& descriptor : descriptors()) {
                if (characteristics.size() == discover->maximum_results) break;
                if (service_matches(discover->filter, descriptor.attribute)) characteristics.push_back(descriptor);
            }
            return modeled_success(mb::BleGattDiscoveryResponse{std::move(characteristics)});
        }
        if (const auto* connect = std::get_if<mb::BleConnectRequest>(&operation.request)) {
            m_peer = connect->peer;
            m_connected = true;
            publish_connection(m_peer, true, true);
            return modeled_success(mb::BleConnectionResponse{m_peer, true, true});
        }
        if (operation.id == mb::DeviceOperation::disconnect) {
            const mb::BleAddress peer = m_peer;
            m_connected = false;
            std::erase_if(subscriptions(), [](const Subscription& subscription) {
                return std::get<mb::BleSubscribeRequest>(subscription.operation.request).target ==
                       mb::BleSubscriptionTarget::characteristic;
            });
            publish_connection(peer, false, false);
            return modeled_success(mb::EmptyResponse{});
        }
        if (const auto* subscribe = std::get_if<mb::BleSubscribeRequest>(&operation.request)) {
            (void)subscribe;
            remember_subscription(operation);
            return modeled_success(mb::SubscriptionResponse{operation.subscription});
        }
        if (const auto* unsubscribe = std::get_if<mb::UnsubscribeRequest>(&operation.request)) {
            forget_subscription(unsubscribe->subscription);
            return modeled_success(mb::EmptyResponse{});
        }
        if (const auto* read = std::get_if<mb::BleReadRequest>(&operation.request)) {
            return modeled_success(mb::BleReadResponse{
                read->attribute, same_attribute(read->attribute, conformance_ble_attribute())
                                     ? m_value
                                     : conformance_bytes({0x7f, 0x00, 0xff})});
        }
        const auto& write = std::get<mb::BleWriteRequest>(operation.request);
        m_value = write.value;
        return modeled_success(mb::BleWriteResponse{write.value->size()});
    }

    void close_model() final { m_connected = false; }

    std::vector<mb::BleAddress> m_discovered;
    mb::BleAddress m_peer{};
    mb::ByteBufferPtr m_value;
    bool m_connected = false;
};

class BlePeripheralModel final : public ConformanceBackend {
public:
    BlePeripheralModel()
        : ConformanceBackend(conformance_version(mb::Capability::ble_peripheral),
                             {mb::DeviceOperation::open, mb::DeviceOperation::read,
                              mb::DeviceOperation::write, mb::DeviceOperation::subscribe,
                              mb::DeviceOperation::unsubscribe, mb::DeviceOperation::advertise,
                              mb::DeviceOperation::notify, mb::DeviceOperation::close}),
          m_value(conformance_bytes({0x10, 0x00, 0xff})) {}

    [[nodiscard]] bool advertising() const { return m_advertising; }
    [[nodiscard]] std::uint32_t advertisement_interval_ms() const { return m_interval_ms; }
    [[nodiscard]] const mb::ByteBuffer& last_notification() const { return m_last_notification; }
    [[nodiscard]] std::size_t notification_count() const { return m_notification_count; }
    [[nodiscard]] const mb::ByteBufferPtr& advertisement() const { return m_advertisement; }

private:
    mb::IoStatus validate_model(const mb::BackendOperation& operation) final {
        if (operation.id == mb::DeviceOperation::unsubscribe) {
            const auto* unsubscribe = std::get_if<mb::UnsubscribeRequest>(&operation.request);
            return unsubscribe && has_subscription(operation.device, unsubscribe->subscription)
                       ? mb::IoStatus::completed
                       : mb::IoStatus::invalid_argument;
        }
        if (const auto* subscribe = std::get_if<mb::BleSubscribeRequest>(&operation.request)) {
            return operation.id == mb::DeviceOperation::subscribe &&
                           subscribe->target == mb::BleSubscriptionTarget::characteristic &&
                           same_attribute(subscribe->attribute, conformance_ble_attribute())
                       ? mb::IoStatus::completed
                       : mb::IoStatus::invalid_argument;
        }
        if (const auto* advertise = std::get_if<mb::BleAdvertiseRequest>(&operation.request)) {
            return operation.id == mb::DeviceOperation::advertise && advertise->payload && advertise->interval_ms != 0
                       ? mb::IoStatus::completed
                       : mb::IoStatus::invalid_argument;
        }
        if (const auto* notify = std::get_if<mb::BleNotifyRequest>(&operation.request)) {
            return operation.id == mb::DeviceOperation::notify && notify->value && !notify->value->empty() &&
                           same_attribute(notify->attribute, conformance_ble_attribute())
                       ? mb::IoStatus::completed
                       : mb::IoStatus::invalid_argument;
        }
        if (const auto* read = std::get_if<mb::BleReadRequest>(&operation.request)) {
            return operation.id == mb::DeviceOperation::read &&
                           same_attribute(read->attribute, conformance_ble_attribute())
                       ? mb::IoStatus::completed
                       : mb::IoStatus::invalid_argument;
        }
        const auto* write = std::get_if<mb::BleWriteRequest>(&operation.request);
        return operation.id == mb::DeviceOperation::write && write && write->value && !write->value->empty() &&
                       same_attribute(write->attribute, conformance_ble_attribute())
                   ? mb::IoStatus::completed
                   : mb::IoStatus::invalid_argument;
    }

    ConformanceResult complete_model(const mb::BackendOperation& operation) final {
        if (const auto* advertise = std::get_if<mb::BleAdvertiseRequest>(&operation.request)) {
            m_advertising = true;
            m_interval_ms = advertise->interval_ms;
            m_advertisement = advertise->payload;
            return modeled_success(mb::EmptyResponse{});
        }
        if (const auto* subscribe = std::get_if<mb::BleSubscribeRequest>(&operation.request)) {
            (void)subscribe;
            remember_subscription(operation);
            return modeled_success(mb::SubscriptionResponse{operation.subscription});
        }
        if (const auto* unsubscribe = std::get_if<mb::UnsubscribeRequest>(&operation.request)) {
            forget_subscription(unsubscribe->subscription);
            return modeled_success(mb::EmptyResponse{});
        }
        if (const auto* read = std::get_if<mb::BleReadRequest>(&operation.request)) {
            return modeled_success(mb::BleReadResponse{read->attribute, m_value});
        }
        if (const auto* write = std::get_if<mb::BleWriteRequest>(&operation.request)) {
            m_value = write->value;
            return modeled_success(mb::BleWriteResponse{write->value->size()});
        }
        const auto& notify = std::get<mb::BleNotifyRequest>(operation.request);
        m_last_notification = *notify.value;
        ++m_notification_count;
        for (const auto& subscription : subscriptions()) {
            const auto& request = std::get<mb::BleSubscribeRequest>(subscription.operation.request);
            if (request.target != mb::BleSubscriptionTarget::characteristic ||
                !same_attribute(request.attribute, notify.attribute)) {
                continue;
            }
            if (publish(subscription, mb::BleReadResponse{notify.attribute, notify.value}) != mb::IoStatus::completed) {
                throw std::runtime_error("modeled BLE characteristic notification was rejected");
            }
        }
        return modeled_success(mb::EmptyResponse{});
    }

    void close_model() final { m_advertising = false; }

    mb::ByteBufferPtr m_value;
    mb::ByteBufferPtr m_advertisement;
    mb::ByteBuffer m_last_notification;
    std::uint32_t m_interval_ms = 0;
    std::size_t m_notification_count = 0;
    bool m_advertising = false;
};

inline mb::ByteBuffer usb_descriptor(std::uint8_t marker) {
    return {0x12, 0x01, 0x00, 0x02, 0xff, 0x80, 0x00, 0x40, marker, 0x00,
            0x34, 0x12, 0x01, 0x02, 0x03, 0x00, 0x00, 0x01};
}

class UsbHostModel final : public ConformanceBackend {
public:
    UsbHostModel()
        : ConformanceBackend(conformance_version(mb::Capability::usb_host),
                             {mb::DeviceOperation::open, mb::DeviceOperation::discover,
                              mb::DeviceOperation::connect, mb::DeviceOperation::disconnect,
                              mb::DeviceOperation::read, mb::DeviceOperation::write,
                              mb::DeviceOperation::transfer, mb::DeviceOperation::close}) {
        m_devices.push_back({7, 0xcafe, 0x1007, conformance_bytes(usb_descriptor(0xc3))});
        m_devices.push_back({9, 0xbeef, 0x1009, conformance_bytes(usb_descriptor(0x80))});
    }

    bool inject_input(std::uint8_t endpoint, const mb::ByteBuffer& bytes) {
        auto& input = m_input[endpoint];
        if (input.size() + bytes.size() > input_capacity) return false;
        input.insert(input.end(), bytes.begin(), bytes.end());
        return true;
    }

    [[nodiscard]] std::uint8_t last_write_endpoint() const { return m_last_write_endpoint; }
    [[nodiscard]] const mb::ByteBuffer& last_write() const { return m_last_write; }

private:
    static constexpr std::size_t input_capacity = 64;

    const mb::UsbDeviceDescription* connected_device() const {
        const auto found = std::find_if(m_devices.begin(), m_devices.end(), [&](const auto& device) {
            return m_connected && device.address == m_address;
        });
        return found == m_devices.end() ? nullptr : &*found;
    }

    mb::IoStatus validate_model(const mb::BackendOperation& operation) final {
        if (const auto* discover = std::get_if<mb::UsbDiscoverRequest>(&operation.request)) {
            return operation.id == mb::DeviceOperation::discover && discover->maximum_results != 0
                       ? mb::IoStatus::completed
                       : mb::IoStatus::invalid_argument;
        }
        if (const auto* connect = std::get_if<mb::UsbConnectRequest>(&operation.request)) {
            if (operation.id != mb::DeviceOperation::connect) return mb::IoStatus::invalid_argument;
            if (m_connected) return mb::IoStatus::busy;
            return std::find(m_discovered.begin(), m_discovered.end(), connect->device_address) != m_discovered.end()
                       ? mb::IoStatus::completed
                       : mb::IoStatus::invalid_argument;
        }
        if (const auto* disconnect = std::get_if<mb::UsbDisconnectRequest>(&operation.request)) {
            if (operation.id != mb::DeviceOperation::disconnect) return mb::IoStatus::invalid_argument;
            return m_connected && disconnect->device_address == m_address ? mb::IoStatus::completed
                                                                          : mb::IoStatus::invalid_argument;
        }
        if (!m_connected) return mb::IoStatus::busy;
        if (const auto* read = std::get_if<mb::UsbReadRequest>(&operation.request)) {
            return operation.id == mb::DeviceOperation::read && read->byte_count != 0 ? mb::IoStatus::completed
                                                                                      : mb::IoStatus::invalid_argument;
        }
        if (const auto* write = std::get_if<mb::UsbWriteRequest>(&operation.request)) {
            return operation.id == mb::DeviceOperation::write && write->bytes ? mb::IoStatus::completed
                                                                              : mb::IoStatus::invalid_argument;
        }
        const auto* transfer = std::get_if<mb::UsbTransferRequest>(&operation.request);
        return operation.id == mb::DeviceOperation::transfer && transfer && transfer->bytes
                   ? mb::IoStatus::completed
                   : mb::IoStatus::invalid_argument;
    }

    ConformanceResult complete_model(const mb::BackendOperation& operation) final {
        if (const auto* discover = std::get_if<mb::UsbDiscoverRequest>(&operation.request)) {
            m_discovered.clear();
            std::vector<mb::UsbDeviceDescription> response;
            for (const auto& device : m_devices) {
                if (response.size() == discover->maximum_results) break;
                response.push_back(device);
                m_discovered.push_back(device.address);
            }
            return modeled_success(mb::UsbDiscoveryResponse{std::move(response)});
        }
        if (const auto* connect = std::get_if<mb::UsbConnectRequest>(&operation.request)) {
            m_address = connect->device_address;
            m_connected = true;
            return modeled_success(mb::EmptyResponse{});
        }
        if (operation.id == mb::DeviceOperation::disconnect) {
            m_connected = false;
            m_input.clear();
            return modeled_success(mb::EmptyResponse{});
        }
        if (const auto* read = std::get_if<mb::UsbReadRequest>(&operation.request)) {
            auto& input = m_input[read->endpoint];
            const std::size_t count = std::min<std::size_t>(read->byte_count, input.size());
            mb::ByteBuffer bytes(count);
            for (std::uint8_t& byte : bytes) {
                byte = input.front();
                input.pop_front();
            }
            return modeled_success(mb::UsbTransferResponse{conformance_bytes(bytes), bytes.size()});
        }
        if (const auto* write = std::get_if<mb::UsbWriteRequest>(&operation.request)) {
            m_last_write_endpoint = write->endpoint;
            m_last_write = *write->bytes;
            return modeled_success(mb::UsbWriteResponse{write->bytes->size()});
        }
        const auto& transfer = std::get<mb::UsbTransferRequest>(operation.request);
        mb::ByteBuffer received;
        if (transfer.setup.request_type == 0x80 && transfer.setup.request == 6) {
            const auto* device = connected_device();
            if (!device || !device->device_descriptor) return modeled_failure(mb::IoStatus::device_lost);
            const std::size_t count = std::min<std::size_t>(transfer.receive_bytes, device->device_descriptor->size());
            received.assign(device->device_descriptor->begin(), device->device_descriptor->begin() + count);
        } else if (transfer.receive_bytes != 0) {
            received.resize(transfer.receive_bytes, 0xa5);
        }
        const std::size_t transferred = transfer.bytes->size() + received.size();
        return modeled_success(mb::UsbTransferResponse{conformance_bytes(received), transferred});
    }

    void close_model() final {
        m_connected = false;
        m_input.clear();
    }

    std::vector<mb::UsbDeviceDescription> m_devices;
    std::vector<std::uint8_t> m_discovered;
    std::map<std::uint8_t, std::deque<std::uint8_t>> m_input;
    std::uint8_t m_address = 0;
    std::uint8_t m_last_write_endpoint = 0;
    mb::ByteBuffer m_last_write;
    bool m_connected = false;
};

class UsbDeviceModel final : public ConformanceBackend {
public:
    UsbDeviceModel()
        : ConformanceBackend(conformance_version(mb::Capability::usb_device),
                             {mb::DeviceOperation::open, mb::DeviceOperation::configure,
                              mb::DeviceOperation::start, mb::DeviceOperation::stop,
                              mb::DeviceOperation::read, mb::DeviceOperation::write,
                              mb::DeviceOperation::transfer, mb::DeviceOperation::close}) {}

    bool inject_host_output(const mb::ByteBuffer& bytes) {
        if (m_host_output.size() + bytes.size() > output_capacity) return false;
        m_host_output.insert(m_host_output.end(), bytes.begin(), bytes.end());
        return true;
    }

    [[nodiscard]] const mb::ByteBuffer& last_host_input() const { return m_last_host_input; }
    [[nodiscard]] const std::vector<mb::UsbInterfaceDescriptor>& interfaces() const { return m_interfaces; }

private:
    static constexpr std::size_t output_capacity = 64;

    mb::IoStatus validate_model(const mb::BackendOperation& operation) final {
        if (const auto* configure = std::get_if<mb::UsbDeviceConfigureRequest>(&operation.request)) {
            if (operation.id != mb::DeviceOperation::configure || !configure->device_descriptor ||
                configure->device_descriptor->size() < 18 || configure->interfaces.empty() ||
                configure->interfaces.size() > 8) {
                return mb::IoStatus::invalid_argument;
            }
            std::array<bool, 256> numbers{};
            for (const auto& interface : configure->interfaces) {
                if (numbers[interface.interface_number] || !interface.descriptors) return mb::IoStatus::invalid_argument;
                numbers[interface.interface_number] = true;
            }
            return m_started ? mb::IoStatus::busy : mb::IoStatus::completed;
        }
        if (operation.id == mb::DeviceOperation::start) {
            if (!std::holds_alternative<mb::StartRequest>(operation.request)) return mb::IoStatus::invalid_argument;
            return m_configured && !m_started ? mb::IoStatus::completed : mb::IoStatus::busy;
        }
        if (operation.id == mb::DeviceOperation::stop) {
            if (!std::holds_alternative<mb::StopRequest>(operation.request)) return mb::IoStatus::invalid_argument;
            return m_started ? mb::IoStatus::completed : mb::IoStatus::busy;
        }
        if (!m_configured || !m_started) return mb::IoStatus::busy;
        if (const auto* read = std::get_if<mb::UsbReadRequest>(&operation.request)) {
            return operation.id == mb::DeviceOperation::read && read->byte_count != 0 ? mb::IoStatus::completed
                                                                                      : mb::IoStatus::invalid_argument;
        }
        if (const auto* write = std::get_if<mb::UsbWriteRequest>(&operation.request)) {
            return operation.id == mb::DeviceOperation::write && write->bytes ? mb::IoStatus::completed
                                                                              : mb::IoStatus::invalid_argument;
        }
        const auto* transfer = std::get_if<mb::UsbTransferRequest>(&operation.request);
        return operation.id == mb::DeviceOperation::transfer && transfer && transfer->bytes
                   ? mb::IoStatus::completed
                   : mb::IoStatus::invalid_argument;
    }

    ConformanceResult complete_model(const mb::BackendOperation& operation) final {
        if (const auto* configure = std::get_if<mb::UsbDeviceConfigureRequest>(&operation.request)) {
            m_device_descriptor = configure->device_descriptor;
            m_interfaces = configure->interfaces;
            m_configured = true;
            return modeled_success(mb::UsbConfigureResponse{1});
        }
        if (operation.id == mb::DeviceOperation::start) {
            m_started = true;
            return modeled_success(mb::EmptyResponse{});
        }
        if (operation.id == mb::DeviceOperation::stop) {
            m_started = false;
            m_host_output.clear();
            return modeled_success(mb::EmptyResponse{});
        }
        if (const auto* read = std::get_if<mb::UsbReadRequest>(&operation.request)) {
            (void)read;
            const std::size_t count = std::min<std::size_t>(read->byte_count, m_host_output.size());
            mb::ByteBuffer bytes(count);
            for (std::uint8_t& byte : bytes) {
                byte = m_host_output.front();
                m_host_output.pop_front();
            }
            return modeled_success(mb::UsbTransferResponse{conformance_bytes(bytes), bytes.size()});
        }
        if (const auto* write = std::get_if<mb::UsbWriteRequest>(&operation.request)) {
            m_last_host_input = *write->bytes;
            return modeled_success(mb::UsbWriteResponse{write->bytes->size()});
        }
        const auto& transfer = std::get<mb::UsbTransferRequest>(operation.request);
        mb::ByteBuffer received;
        if (transfer.setup.request_type == 0x80 && transfer.setup.request == 6) {
            const std::size_t count = std::min<std::size_t>(transfer.receive_bytes, m_device_descriptor->size());
            received.assign(m_device_descriptor->begin(), m_device_descriptor->begin() + count);
        } else if (transfer.receive_bytes != 0) {
            received.resize(transfer.receive_bytes, 0x5a);
        }
        return modeled_success(mb::UsbTransferResponse{conformance_bytes(received),
                                                       transfer.bytes->size() + received.size()});
    }

    void close_model() final {
        m_configured = false;
        m_started = false;
        m_host_output.clear();
    }

    mb::ByteBufferPtr m_device_descriptor;
    std::vector<mb::UsbInterfaceDescriptor> m_interfaces;
    std::deque<std::uint8_t> m_host_output;
    mb::ByteBuffer m_last_host_input;
    bool m_configured = false;
    bool m_started = false;
};

class EventLinkModel final : public ConformanceBackend {
public:
    EventLinkModel()
        : ConformanceBackend(conformance_version(mb::Capability::event_link),
                             {mb::DeviceOperation::open, mb::DeviceOperation::start,
                              mb::DeviceOperation::stop, mb::DeviceOperation::read,
                              mb::DeviceOperation::close}) {}

    void select_triggers(std::int64_t count) {
        if (m_enabled && count > 0 && count <= std::numeric_limits<std::int64_t>::max() - m_trigger_count) {
            m_trigger_count += count;
        }
    }

private:
    mb::IoStatus validate_model(const mb::BackendOperation& operation) final {
        if (operation.id == mb::DeviceOperation::read) {
            return std::holds_alternative<mb::EventLinkReadRequest>(operation.request) ? mb::IoStatus::completed
                                                                                       : mb::IoStatus::invalid_argument;
        }
        if (operation.id == mb::DeviceOperation::start) {
            if (!std::holds_alternative<mb::StartRequest>(operation.request)) return mb::IoStatus::invalid_argument;
            return m_enabled ? mb::IoStatus::busy : mb::IoStatus::completed;
        }
        if (operation.id == mb::DeviceOperation::stop) {
            if (!std::holds_alternative<mb::StopRequest>(operation.request)) return mb::IoStatus::invalid_argument;
            return m_enabled ? mb::IoStatus::completed : mb::IoStatus::busy;
        }
        return mb::IoStatus::unsupported;
    }

    ConformanceResult complete_model(const mb::BackendOperation& operation) final {
        if (operation.id == mb::DeviceOperation::start) m_enabled = true;
        if (operation.id == mb::DeviceOperation::stop) m_enabled = false;
        if (operation.id == mb::DeviceOperation::start || operation.id == mb::DeviceOperation::stop) {
            return modeled_success(mb::EmptyResponse{});
        }
        return modeled_success(mb::EventLinkReadResponse{m_enabled, false, m_trigger_count});
    }

    void close_model() final { m_enabled = false; }

    std::int64_t m_trigger_count = 0;
    bool m_enabled = false;
};

inline constexpr std::string_view spi_name = "spi_flash_model";
inline constexpr std::string_view uart_name = "uart_echo_model";
inline constexpr std::string_view i2c_name = "i2c_register_model";
inline constexpr std::string_view adc_name = "adc_sequence_model";
inline constexpr std::string_view pwm_name = "pwm_waveform_model";
inline constexpr std::string_view pulse_capture_name = "pulse_capture_model";
inline constexpr std::string_view pulse_output_name = "pulse_output_model";
inline constexpr std::string_view i2s_tx_name = "i2s_transmit_model";
inline constexpr std::string_view i2s_rx_name = "i2s_receive_model";
inline constexpr std::string_view pdm_name = "pdm_receive_model";
inline constexpr std::string_view ble_central_name = "ble_central_model";
inline constexpr std::string_view ble_peripheral_name = "ble_peripheral_model";
inline constexpr std::string_view usb_host_name = "usb_host_model";
inline constexpr std::string_view usb_device_name = "usb_device_model";
inline constexpr std::string_view event_link_name = "owner_selected_button_event";

struct ConformanceFixture {
    ConformanceFixture() {
        register_endpoint(spi_name, spi);
        register_endpoint(uart_name, uart);
        register_endpoint(i2c_name, i2c);
        register_endpoint(adc_name, adc);
        register_endpoint(pwm_name, pwm);
        register_endpoint(pulse_capture_name, pulse_capture);
        register_endpoint(pulse_output_name, pulse_output);
        register_endpoint(i2s_tx_name, i2s_tx);
        register_endpoint(i2s_rx_name, i2s_rx);
        register_endpoint(pdm_name, pdm);
        register_endpoint(ble_central_name, ble_central);
        register_endpoint(ble_peripheral_name, ble_peripheral);
        register_endpoint(usb_host_name, usb_host);
        register_endpoint(usb_device_name, usb_device);
        register_endpoint(event_link_name, event_link);
    }

    [[nodiscard]] mb::ApplicationPermissions permissions() const {
        return {capabilities, endpoint_names};
    }

    [[nodiscard]] std::unique_ptr<mb::DeviceSession> session(mb::DeviceBudget budget = {}) {
        return registry.create_session(permissions(), budget);
    }

private:
    template <typename Model>
    void register_endpoint(std::string_view name, const std::shared_ptr<Model>& model) {
        const mb::CapabilityVersion version = model_versions.at(name);
        registry.register_endpoint({name, version, true}, mb::EndpointAccess::application, model,
                                   ConformanceBackend::deliver_pending);
    }

    inline static const std::map<std::string_view, mb::CapabilityVersion> model_versions{
        {spi_name, conformance_version(mb::Capability::spi_controller)},
        {uart_name, conformance_version(mb::Capability::uart)},
        {i2c_name, conformance_version(mb::Capability::i2c_controller)},
        {adc_name, conformance_version(mb::Capability::adc)},
        {pwm_name, conformance_version(mb::Capability::pwm)},
        {pulse_capture_name, conformance_version(mb::Capability::pulse_capture)},
        {pulse_output_name, conformance_version(mb::Capability::pulse_output)},
        {i2s_tx_name, conformance_version(mb::Capability::i2s_transmit)},
        {i2s_rx_name, conformance_version(mb::Capability::i2s_receive)},
        {pdm_name, conformance_version(mb::Capability::pdm_receive)},
        {ble_central_name, conformance_version(mb::Capability::ble_central)},
        {ble_peripheral_name, conformance_version(mb::Capability::ble_peripheral)},
        {usb_host_name, conformance_version(mb::Capability::usb_host)},
        {usb_device_name, conformance_version(mb::Capability::usb_device)},
        {event_link_name, conformance_version(mb::Capability::event_link)}};

public:
    std::shared_ptr<SpiModel> spi = std::make_shared<SpiModel>();
    std::shared_ptr<UartModel> uart = std::make_shared<UartModel>();
    std::shared_ptr<I2cModel> i2c = std::make_shared<I2cModel>();
    std::shared_ptr<AdcModel> adc = std::make_shared<AdcModel>();
    std::shared_ptr<PwmModel> pwm = std::make_shared<PwmModel>();
    std::shared_ptr<PulseCaptureModel> pulse_capture = std::make_shared<PulseCaptureModel>();
    std::shared_ptr<PulseOutputModel> pulse_output = std::make_shared<PulseOutputModel>();
    std::shared_ptr<PcmModel> i2s_tx = std::make_shared<PcmModel>(PcmRole::i2s_transmit);
    std::shared_ptr<PcmModel> i2s_rx = std::make_shared<PcmModel>(PcmRole::i2s_receive);
    std::shared_ptr<PcmModel> pdm = std::make_shared<PcmModel>(PcmRole::pdm_receive);
    std::shared_ptr<BleCentralModel> ble_central = std::make_shared<BleCentralModel>();
    std::shared_ptr<BlePeripheralModel> ble_peripheral = std::make_shared<BlePeripheralModel>();
    std::shared_ptr<UsbHostModel> usb_host = std::make_shared<UsbHostModel>();
    std::shared_ptr<UsbDeviceModel> usb_device = std::make_shared<UsbDeviceModel>();
    std::shared_ptr<EventLinkModel> event_link = std::make_shared<EventLinkModel>();
    mb::PeripheralRegistry registry;

    std::array<mb::CapabilityVersion, 15> capabilities{
        conformance_version(mb::Capability::spi_controller), conformance_version(mb::Capability::uart),
        conformance_version(mb::Capability::i2c_controller), conformance_version(mb::Capability::adc),
        conformance_version(mb::Capability::pwm), conformance_version(mb::Capability::pulse_capture),
        conformance_version(mb::Capability::pulse_output), conformance_version(mb::Capability::i2s_transmit),
        conformance_version(mb::Capability::i2s_receive), conformance_version(mb::Capability::pdm_receive),
        conformance_version(mb::Capability::ble_central), conformance_version(mb::Capability::ble_peripheral),
        conformance_version(mb::Capability::usb_host), conformance_version(mb::Capability::usb_device),
        conformance_version(mb::Capability::event_link)};
    std::array<std::string_view, 15> endpoint_names{spi_name, uart_name, i2c_name, adc_name, pwm_name,
        pulse_capture_name, pulse_output_name, i2s_tx_name, i2s_rx_name, pdm_name, ble_central_name,
        ble_peripheral_name, usb_host_name, usb_device_name, event_link_name};
};

} // namespace
