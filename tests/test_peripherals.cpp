#include "microbrowser/peripherals.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace {

int failures = 0;
#define CHECK(condition) do { if (!(condition)) { std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: " #condition "\n"; ++failures; } } while (false)
#define REQUIRE(condition) do { if (!(condition)) throw std::runtime_error(std::string(#condition) + " at " + std::to_string(__LINE__)); } while (false)

constexpr std::string_view gpio_input_name = "gpio_input";
constexpr std::string_view gpio_output_name = "gpio_output";
constexpr std::string_view adc_name = "adc_counter";
constexpr std::string_view i2c_name = "i2c_registers";
constexpr std::string_view notification_name = "ble_notifications";
constexpr std::string_view protected_name = "protected_gpio";

constexpr mb::CapabilityVersion gpio_version{mb::Capability::gpio, 1, 0};
constexpr mb::CapabilityVersion adc_version{mb::Capability::adc, 1, 0};
constexpr mb::CapabilityVersion i2c_version{mb::Capability::i2c_controller, 1, 0};
constexpr mb::CapabilityVersion notification_version{mb::Capability::ble_peripheral, 1, 0};

struct ModeledResult {
    mb::IoStatus status = mb::IoStatus::completed;
    mb::DeviceResponse response = std::monostate{};
};

ModeledResult success(mb::DeviceResponse response) {
    return {mb::IoStatus::completed, std::move(response)};
}

class ManualBackend : public mb::PeripheralBackend {
public:
    ManualBackend(mb::CapabilityVersion version, std::vector<mb::DeviceOperation> operations)
        : m_version(version), m_operations(std::move(operations)) {}

    [[nodiscard]] std::span<const mb::DeviceOperation> operations() const noexcept final {
        return m_operations;
    }

    mb::IoStatus start_operation(const mb::BackendOperation& operation,
                                 std::weak_ptr<mb::PeripheralCompletionSink> completions) final {
        if (std::find(m_operations.begin(), m_operations.end(), operation.id) == m_operations.end()) {
            return mb::IoStatus::unsupported;
        }
        if (!valid_request(operation)) {
            return mb::IoStatus::invalid_argument;
        }
        if ((operation.id == mb::DeviceOperation::close ||
             operation.id == mb::DeviceOperation::unsubscribe) &&
            control_rejections_remaining != 0) {
            --control_rejections_remaining;
            return mb::IoStatus::busy;
        }

        m_pending.push_back({operation, std::move(completions), false});
        if (operation.id == mb::DeviceOperation::subscribe) {
            m_subscription = operation;
            m_subscription_completions = m_pending.back().completions;
        }
        if (complete_before_accepting == operation.id) {
            complete_at(m_pending.size() - 1);
        }
        return mb::IoStatus::completed;
    }
    std::size_t control_rejections_remaining = 0;
    std::optional<mb::DeviceOperation> complete_before_accepting;
    std::thread::id last_delivery_thread;

    static void deliver_pending(mb::PeripheralBackend& backend) {
        auto& model = static_cast<ManualBackend&>(backend);
        model.last_delivery_thread = std::this_thread::get_id();
        if (model.pending_count() != 0) model.complete_next();
    }


    mb::IoStatus cancel_operation(const mb::BackendOperation& operation) final {
        const auto found = std::find_if(m_pending.begin(), m_pending.end(), [&](const Pending& pending) {
            return pending.operation.operation == operation.operation;
        });
        if (found == m_pending.end() || found->cancellation_requested) {
            return mb::IoStatus::invalid_argument;
        }
        found->cancellation_requested = true;
        return mb::IoStatus::completed;
    }

    // The fixture deliberately retains captured operations. Tests use that to
    // inject late completions after shutdown and application replacement.
    void release_application(std::uint64_t owner_generation) final {
        m_released_owners.push_back(owner_generation);
    }

    void complete_next() {
        complete_at(0);
    }

    void complete_at(std::size_t index) {
        if (index >= m_pending.size()) {
            throw std::runtime_error("modeled backend has no requested pending operation");
        }
        Pending pending = std::move(m_pending[index]);
        m_pending.erase(m_pending.begin() + static_cast<std::ptrdiff_t>(index));
        ModeledResult result = pending.cancellation_requested
            ? ModeledResult{mb::IoStatus::cancelled, std::monostate{}}
            : completion_for(pending.operation);
        const auto completions = pending.completions.lock();
        if (!completions) {
            throw std::runtime_error("modeled completion sink expired");
        }
        completions->complete(pending.operation, result.status, std::move(result.response));
    }

    [[nodiscard]] std::size_t pending_count() const {
        return m_pending.size();
    }

    [[nodiscard]] const mb::BackendOperation& pending_operation(std::size_t index = 0) const {
        return m_pending.at(index).operation;
    }

    [[nodiscard]] std::size_t release_count(std::uint64_t owner_generation) const {
        return static_cast<std::size_t>(std::count(m_released_owners.begin(), m_released_owners.end(),
                                                   owner_generation));
    }

protected:
    [[nodiscard]] const mb::CapabilityVersion& version() const {
        return m_version;
    }

    mb::IoStatus publish_subscription(mb::DeviceResponse response) {
        if (!m_subscription) {
            throw std::runtime_error("modeled backend has no subscription");
        }
        const auto completions = m_subscription_completions.lock();
        if (!completions) {
            throw std::runtime_error("modeled notification sink expired");
        }
        return completions->publish(m_subscription->owner_generation, m_subscription->subscription,
                                    std::move(response));
    }

private:
    struct Pending {
        mb::BackendOperation operation;
        std::weak_ptr<mb::PeripheralCompletionSink> completions;
        bool cancellation_requested = false;
    };

    [[nodiscard]] bool valid_request(const mb::BackendOperation& operation) const {
        if (operation.id == mb::DeviceOperation::open) {
            const auto* request = std::get_if<mb::OpenRequest>(&operation.request);
            return request != nullptr && request->minimum_version.capability == m_version.capability &&
                   request->minimum_version.major == mb::peripheral_abi_major &&
                   request->minimum_version.minor <= m_version.minor;
        }
        if (operation.id == mb::DeviceOperation::close) {
            return std::holds_alternative<mb::CloseRequest>(operation.request);
        }
        if (operation.id == mb::DeviceOperation::unsubscribe) {
            return std::holds_alternative<mb::UnsubscribeRequest>(operation.request);
        }
        return valid_model_request(operation.request);
    }

    [[nodiscard]] ModeledResult completion_for(const mb::BackendOperation& operation) {
        if (operation.id == mb::DeviceOperation::open) {
            return success(mb::OpenResponse{operation.device, m_version, true});
        }
        if (operation.id == mb::DeviceOperation::close ||
            operation.id == mb::DeviceOperation::unsubscribe) {
            return success(mb::EmptyResponse{});
        }
        return model_completion(operation);
    }

    [[nodiscard]] virtual bool valid_model_request(const mb::DeviceRequest& request) const = 0;
    [[nodiscard]] virtual ModeledResult model_completion(const mb::BackendOperation& operation) = 0;

    mb::CapabilityVersion m_version;
    std::vector<mb::DeviceOperation> m_operations;
    std::vector<Pending> m_pending;
    std::vector<std::uint64_t> m_released_owners;
    std::optional<mb::BackendOperation> m_subscription;
    std::weak_ptr<mb::PeripheralCompletionSink> m_subscription_completions;
};

struct GpioLine {
    bool high = false;
};

enum class GpioRole { input, output };

class GpioBackend final : public ManualBackend {
public:
    GpioBackend(std::shared_ptr<GpioLine> line, GpioRole role)
        : ManualBackend(gpio_version,
                        role == GpioRole::input
                            ? std::vector<mb::DeviceOperation>{mb::DeviceOperation::open,
                                                               mb::DeviceOperation::read,
                                                               mb::DeviceOperation::cancel,
                                                               mb::DeviceOperation::close}
                            : std::vector<mb::DeviceOperation>{mb::DeviceOperation::open,
                                                               mb::DeviceOperation::write,
                                                               mb::DeviceOperation::cancel,
                                                               mb::DeviceOperation::close}),
          m_line(std::move(line)), m_role(role) {}

private:
    [[nodiscard]] bool valid_model_request(const mb::DeviceRequest& request) const final {
        if (m_role == GpioRole::input) {
            return std::holds_alternative<mb::GpioReadRequest>(request);
        }
        return std::holds_alternative<mb::GpioWriteRequest>(request);
    }

    [[nodiscard]] ModeledResult model_completion(const mb::BackendOperation& operation) final {
        if (operation.id == mb::DeviceOperation::read && m_role == GpioRole::input) {
            const std::int64_t timestamp_ns = m_next_timestamp_ns++;
            return success(mb::GpioReadResponse{m_line->high, timestamp_ns});
        }
        if (operation.id == mb::DeviceOperation::write && m_role == GpioRole::output) {
            m_line->high = std::get<mb::GpioWriteRequest>(operation.request).high;
            return success(mb::EmptyResponse{});
        }
        throw std::logic_error("GPIO model received an unadvertised operation");
    }

    std::shared_ptr<GpioLine> m_line;
    GpioRole m_role;
    std::int64_t m_next_timestamp_ns = 100;
};

class AdcBackend final : public ManualBackend {
public:
    explicit AdcBackend(std::int64_t sample)
        : ManualBackend(adc_version,
                        {mb::DeviceOperation::open, mb::DeviceOperation::read, mb::DeviceOperation::close}),
          m_sample(sample) {}

private:
    [[nodiscard]] bool valid_model_request(const mb::DeviceRequest& request) const final {
        return std::holds_alternative<mb::AdcReadRequest>(request);
    }

    [[nodiscard]] ModeledResult model_completion(const mb::BackendOperation& operation) final {
        if (operation.id != mb::DeviceOperation::read) {
            throw std::logic_error("ADC model received an unadvertised operation");
        }
        return success(mb::AdcReadResponse{m_sample, 3'300'000, 16, m_sample + 1, false});
    }

    std::int64_t m_sample;
};

class I2cRegisterBackend final : public ManualBackend {
public:
    static constexpr std::uint16_t primary_address = 0x52;
    static constexpr std::uint16_t secondary_address = 0x53;

    I2cRegisterBackend()
        : ManualBackend(i2c_version,
                        {mb::DeviceOperation::open, mb::DeviceOperation::write, mb::DeviceOperation::read,
                         mb::DeviceOperation::transfer, mb::DeviceOperation::close}) {}

    [[nodiscard]] std::size_t transaction_count() const {
        return m_transactions.size();
    }

    [[nodiscard]] std::size_t repeated_start_count() const {
        return m_repeated_start_count;
    }

    struct Transaction {
        mb::DeviceOperation operation = mb::DeviceOperation::none;
        std::uint16_t address = 0;
        std::size_t write_bytes = 0;
        std::size_t read_bytes = 0;
        bool repeated_start = false;
    };

    [[nodiscard]] const Transaction& last_transaction() const {
        return m_transactions.at(m_transactions.size() - 1);
    }

private:
    struct Registers {
        std::array<std::uint8_t, 256> bytes{};
        std::uint8_t selected = 0;
    };

    [[nodiscard]] bool valid_model_request(const mb::DeviceRequest& request) const {
        if (const auto* write = std::get_if<mb::I2cWriteRequest>(&request)) {
            return write->bytes && !write->bytes->empty();
        }
        if (const auto* read = std::get_if<mb::I2cReadRequest>(&request)) {
            return read->byte_count <= 256;
        }
        if (const auto* transfer = std::get_if<mb::I2cTransferRequest>(&request)) {
            return transfer->write_bytes && !transfer->write_bytes->empty() && transfer->read_bytes <= 256;
        }
        return false;
    }

    [[nodiscard]] Registers* find_registers(std::uint16_t address, bool ten_bit_address) {
        if (ten_bit_address) {
            return nullptr;
        }
        if (address == primary_address) {
            return &m_registers[0];
        }
        if (address == secondary_address) {
            return &m_registers[1];
        }
        return nullptr;
    }

    static void apply_write_phase(Registers& registers, const mb::ByteBuffer& bytes) {
        registers.selected = bytes[0];
        for (std::size_t index = 1; index < bytes.size(); ++index) {
            registers.bytes[registers.selected] = bytes[index];
            registers.selected = static_cast<std::uint8_t>(registers.selected + 1U);
        }
    }

    static mb::ByteBufferPtr apply_read_phase(Registers& registers, std::uint32_t byte_count) {
        mb::ByteBuffer bytes(byte_count);
        for (std::uint8_t& byte : bytes) {
            byte = registers.bytes[registers.selected];
            registers.selected = static_cast<std::uint8_t>(registers.selected + 1U);
        }
        return mb::make_immutable_bytes(std::span<const std::uint8_t>(bytes));
    }

    [[nodiscard]] ModeledResult nack() const {
        const mb::ByteBuffer empty;
        return success(mb::I2cTransferResponse{mb::make_immutable_bytes(std::span<const std::uint8_t>(empty)),
                                               0, mb::I2cFault::nack, false});
    }

    [[nodiscard]] ModeledResult model_completion(const mb::BackendOperation& operation) final {
        if (const auto* write = std::get_if<mb::I2cWriteRequest>(&operation.request)) {
            Registers* registers = find_registers(write->address, write->ten_bit_address);
            if (registers == nullptr) {
                return nack();
            }
            apply_write_phase(*registers, *write->bytes);
            m_transactions.push_back({mb::DeviceOperation::write, write->address, write->bytes->size(), 0, false});
            return success(mb::I2cTransferResponse{nullptr, write->bytes->size(), mb::I2cFault::none, false});
        }
        if (const auto* read = std::get_if<mb::I2cReadRequest>(&operation.request)) {
            Registers* registers = find_registers(read->address, read->ten_bit_address);
            if (registers == nullptr) {
                return nack();
            }
            auto received = apply_read_phase(*registers, read->byte_count);
            m_transactions.push_back({mb::DeviceOperation::read, read->address, 0, received->size(), false});
            return success(mb::I2cTransferResponse{std::move(received), 0, mb::I2cFault::none, false});
        }
        if (const auto* transfer = std::get_if<mb::I2cTransferRequest>(&operation.request)) {
            Registers* registers = find_registers(transfer->address, transfer->ten_bit_address);
            if (registers == nullptr) {
                return nack();
            }
            apply_write_phase(*registers, *transfer->write_bytes);
            auto received = apply_read_phase(*registers, transfer->read_bytes);
            const bool repeated_start = transfer->repeated_start && transfer->read_bytes != 0;
            if (repeated_start) {
                ++m_repeated_start_count;
            }
            // One record denotes one addressed transaction. No other model code
            // runs between the write and read phases.
            m_transactions.push_back({mb::DeviceOperation::transfer, transfer->address,
                                      transfer->write_bytes->size(), received->size(), repeated_start});
            return success(mb::I2cTransferResponse{std::move(received), transfer->write_bytes->size(),
                                                   mb::I2cFault::none, repeated_start});
        }
        throw std::logic_error("I2C model received an unadvertised operation");
    }

    std::array<Registers, 2> m_registers{};
    std::vector<Transaction> m_transactions;
    std::size_t m_repeated_start_count = 0;
};

class NotificationBackend final : public ManualBackend {
public:
    NotificationBackend()
        : ManualBackend(notification_version,
                        {mb::DeviceOperation::open, mb::DeviceOperation::subscribe,
                         mb::DeviceOperation::unsubscribe, mb::DeviceOperation::close}) {}

    mb::IoStatus publish(mb::ByteBufferPtr value) {
        return publish_subscription(mb::BleReadResponse{m_attribute, std::move(value)});
    }

private:
    [[nodiscard]] bool valid_model_request(const mb::DeviceRequest& request) const final {
        return std::holds_alternative<mb::BleSubscribeRequest>(request);
    }

    [[nodiscard]] ModeledResult model_completion(const mb::BackendOperation& operation) final {
        if (operation.id != mb::DeviceOperation::subscribe) {
            throw std::logic_error("notification model received an unadvertised operation");
        }
        m_attribute = std::get<mb::BleSubscribeRequest>(operation.request).attribute;
        return success(mb::SubscriptionResponse{operation.subscription});
    }

    mb::BleAttribute m_attribute{};
};

struct ModelFixture {
    ModelFixture() {
        registry.register_endpoint({gpio_input_name, gpio_version, true}, mb::EndpointAccess::application, gpio_input);
        registry.register_endpoint({gpio_output_name, gpio_version, true}, mb::EndpointAccess::application, gpio_output);
        registry.register_endpoint({adc_name, adc_version, true}, mb::EndpointAccess::application, adc);
        registry.register_endpoint({i2c_name, i2c_version, true}, mb::EndpointAccess::application, i2c);
        registry.register_endpoint({notification_name, notification_version, true},
                                   mb::EndpointAccess::application, notifications);
        registry.register_endpoint({protected_name, gpio_version, true}, mb::EndpointAccess::owner_only,
                                   protected_gpio);
    }

    [[nodiscard]] mb::ApplicationPermissions permissions() const {
        return {std::span<const mb::CapabilityVersion>(capabilities),
                std::span<const std::string_view>(endpoints)};
    }

    std::shared_ptr<GpioLine> line = std::make_shared<GpioLine>();
    std::shared_ptr<GpioLine> protected_line = std::make_shared<GpioLine>();
    std::shared_ptr<GpioBackend> gpio_input = std::make_shared<GpioBackend>(line, GpioRole::input);
    std::shared_ptr<GpioBackend> gpio_output = std::make_shared<GpioBackend>(line, GpioRole::output);
    std::shared_ptr<GpioBackend> protected_gpio = std::make_shared<GpioBackend>(protected_line, GpioRole::output);
    std::shared_ptr<AdcBackend> adc = std::make_shared<AdcBackend>(9'007'199'254'740'993LL);
    std::shared_ptr<I2cRegisterBackend> i2c = std::make_shared<I2cRegisterBackend>();
    std::shared_ptr<NotificationBackend> notifications = std::make_shared<NotificationBackend>();
    mb::PeripheralRegistry registry;
    std::array<mb::CapabilityVersion, 4> capabilities{gpio_version, adc_version, i2c_version,
                                                       notification_version};
    std::array<std::string_view, 6> endpoints{gpio_input_name, gpio_output_name, adc_name, i2c_name,
                                              notification_name, protected_name};
};

void expect_rejected(const mb::SubmissionResult& submission, mb::IoStatus status) {
    if (submission.status != status) {
        std::cerr << "rejection status expected=" << static_cast<unsigned>(status)
                  << " actual=" << static_cast<unsigned>(submission.status)
                  << " handle_slot=" << submission.operation.slot << '\n';
    }
    CHECK(submission.status == status);
    CHECK(submission.operation.is_empty());
}

mb::DeviceResult finish_completion(ManualBackend& backend, mb::DeviceSession& session,
                                   mb::DeviceHandle operation) {
    backend.complete_next();
    auto result = session.poll();
    REQUIRE(result.has_value());
    CHECK(result->kind == mb::DeviceResultKind::completion);
    CHECK(result->operation == operation);
    return std::move(*result);
}

mb::DeviceResult execute_completed(mb::DeviceSession& session, ManualBackend& backend,
                                   mb::DeviceHandle device, mb::DeviceOperation operation,
                                   mb::DeviceRequest request) {
    const auto submission = session.execute(device, operation, std::move(request));
    REQUIRE(submission.status == mb::IoStatus::completed);
    REQUIRE(!submission.operation.is_empty());
    return finish_completion(backend, session, submission.operation);
}

mb::DeviceHandle open_device(mb::DeviceSession& session, ManualBackend& backend,
                             std::string_view logical_name, mb::CapabilityVersion version) {
    const auto submission = session.open(logical_name, mb::OpenRequest{version});
    REQUIRE(submission.status == mb::IoStatus::completed);
    REQUIRE(!submission.operation.is_empty());
    const auto result = finish_completion(backend, session, submission.operation);
    CHECK(result.id == mb::DeviceOperation::open);
    CHECK(result.status == mb::IoStatus::completed);
    const auto* response = std::get_if<mb::OpenResponse>(&result.response);
    REQUIRE(response != nullptr);
    CHECK(!response->device.is_empty());
    CHECK(response->version == version);
    CHECK(response->modeled);
    return response->device;
}

mb::DeviceHandle open_device(mb::DeviceSession& session, ManualBackend& backend,
                             std::string_view logical_name) {
    const auto endpoints = session.endpoints();
    const auto metadata = std::find_if(endpoints.begin(), endpoints.end(), [&](const mb::EndpointMetadata& endpoint) {
        return endpoint.descriptor.logical_name == logical_name;
    });
    REQUIRE(metadata != endpoints.end());
    return open_device(session, backend, logical_name, metadata->descriptor.version);
}

mb::DeviceHandle subscribe_notifications(mb::DeviceSession& session, NotificationBackend& backend,
                                         mb::DeviceHandle device) {
    const auto result = execute_completed(session, backend, device, mb::DeviceOperation::subscribe,
                                          mb::BleSubscribeRequest{});
    CHECK(result.status == mb::IoStatus::completed);
    const auto* response = std::get_if<mb::SubscriptionResponse>(&result.response);
    REQUIRE(response != nullptr);
    CHECK(!response->subscription.is_empty());
    return response->subscription;
}

mb::ByteBufferPtr immutable_copy(const mb::ByteBuffer& bytes) {
    return mb::make_immutable_bytes(std::span<const std::uint8_t>(bytes));
}

void test_descriptor_discovery_and_open_versions() {
    ModelFixture fixture;
    auto session = fixture.registry.create_session(fixture.permissions());
    REQUIRE(session);

    const auto endpoints = session->endpoints();
    CHECK(endpoints.size() == 5);
    CHECK(std::find_if(endpoints.begin(), endpoints.end(), [](const mb::EndpointMetadata& endpoint) {
              return endpoint.descriptor.logical_name == protected_name;
          }) == endpoints.end());
    for (const auto& endpoint : endpoints) {
        CHECK(endpoint.descriptor.modeled);
        CHECK(endpoint.supports(mb::DeviceOperation::open));
        CHECK(endpoint.supports(mb::DeviceOperation::close));
        CHECK(!endpoint.supports(mb::DeviceOperation::none));
        CHECK(std::find(endpoint.operations.begin(), endpoint.operations.end(), mb::DeviceOperation::configure) ==
              endpoint.operations.end());
    }
    const auto input = fixture.registry.find_endpoint(gpio_input_name);
    REQUIRE(input.has_value());
    CHECK(input->supports(mb::DeviceOperation::read));
    CHECK(!input->supports(mb::DeviceOperation::write));
    CHECK(!input->supports(mb::DeviceOperation::transfer));
    const auto i2c = fixture.registry.find_endpoint(i2c_name);
    REQUIRE(i2c.has_value());
    CHECK(i2c->supports(mb::DeviceOperation::transfer));
    CHECK(!i2c->supports(mb::DeviceOperation::subscribe));

    const auto pending_before = fixture.gpio_input->pending_count();
    expect_rejected(session->open(gpio_input_name, mb::OpenRequest{}), mb::IoStatus::invalid_argument);
    expect_rejected(session->open(gpio_input_name, mb::OpenRequest{{mb::Capability::none, 1, 0}}),
                    mb::IoStatus::invalid_argument);
    expect_rejected(session->open(gpio_input_name, mb::OpenRequest{i2c_version}), mb::IoStatus::invalid_argument);
    expect_rejected(session->open(gpio_input_name, mb::OpenRequest{{mb::Capability::gpio, 2, 0}}),
                    mb::IoStatus::unsupported);
    expect_rejected(session->open(gpio_input_name, mb::OpenRequest{{mb::Capability::gpio, 1, 1}}),
                    mb::IoStatus::unsupported);
    CHECK(fixture.gpio_input->pending_count() == pending_before);
    CHECK(!session->poll().has_value());

    const auto handle = open_device(*session, *fixture.gpio_input, gpio_input_name);
    CHECK(session->open_handle_count() == 1);
    const auto read = execute_completed(*session, *fixture.gpio_input, handle, mb::DeviceOperation::read,
                                        mb::GpioReadRequest{});
    CHECK(std::get<mb::GpioReadResponse>(read.response).high == false);
}

void test_permissions_use_capabilities_and_exact_endpoint_names() {
    ModelFixture fixture;

    const std::array<mb::CapabilityVersion, 1> gpio_capabilities{gpio_version};
    const std::array<std::string_view, 3> exact_endpoints{gpio_input_name, protected_name, adc_name};
    auto session = fixture.registry.create_session(
        {std::span<const mb::CapabilityVersion>(gpio_capabilities), std::span<const std::string_view>(exact_endpoints)});
    expect_rejected(session->open(gpio_output_name, mb::OpenRequest{gpio_version}), mb::IoStatus::permission_denied);
    expect_rejected(session->open(adc_name, mb::OpenRequest{adc_version}), mb::IoStatus::permission_denied);
    expect_rejected(session->open(protected_name, mb::OpenRequest{gpio_version}), mb::IoStatus::permission_denied);
    const auto handle = open_device(*session, *fixture.gpio_input, gpio_input_name);
    CHECK(!handle.is_empty());

    const std::array<mb::CapabilityVersion, 4> all_capabilities{gpio_version, adc_version, i2c_version,
                                                                 notification_version};
    const std::array<std::string_view, 1> prefix_endpoint{"gpio"};
    auto prefix_session = fixture.registry.create_session(
        {std::span<const mb::CapabilityVersion>(all_capabilities), std::span<const std::string_view>(prefix_endpoint)});
    expect_rejected(prefix_session->open(gpio_input_name, mb::OpenRequest{gpio_version}),
                    mb::IoStatus::permission_denied);

    auto empty_endpoint_session = fixture.registry.create_session(
        {std::span<const mb::CapabilityVersion>(all_capabilities), std::span<const std::string_view>{}});
    expect_rejected(empty_endpoint_session->open(gpio_input_name, mb::OpenRequest{gpio_version}),
                    mb::IoStatus::permission_denied);
    CHECK(fixture.gpio_input->pending_count() == 0);
    CHECK(fixture.protected_gpio->pending_count() == 0);
}

void test_gpio_wiring_and_positive_io() {
    ModelFixture fixture;
    auto session = fixture.registry.create_session(fixture.permissions());
    const auto output = open_device(*session, *fixture.gpio_output, gpio_output_name);
    const auto input = open_device(*session, *fixture.gpio_input, gpio_input_name);
    CHECK(session->open_handle_count() == 2);

    const auto initial = execute_completed(*session, *fixture.gpio_input, input, mb::DeviceOperation::read,
                                           mb::GpioReadRequest{});
    CHECK(!std::get<mb::GpioReadResponse>(initial.response).high);

    const auto high = session->execute(output, mb::DeviceOperation::write, mb::GpioWriteRequest{true});
    REQUIRE(high.status == mb::IoStatus::completed);
    CHECK(!fixture.line->high);
    const auto high_result = finish_completion(*fixture.gpio_output, *session, high.operation);
    CHECK(high_result.status == mb::IoStatus::completed);
    CHECK(std::holds_alternative<mb::EmptyResponse>(high_result.response));
    CHECK(fixture.line->high);
    const auto read_high = execute_completed(*session, *fixture.gpio_input, input, mb::DeviceOperation::read,
                                             mb::GpioReadRequest{});
    CHECK(std::get<mb::GpioReadResponse>(read_high.response).high);

    const auto low = execute_completed(*session, *fixture.gpio_output, output, mb::DeviceOperation::write,
                                       mb::GpioWriteRequest{false});
    CHECK(std::holds_alternative<mb::EmptyResponse>(low.response));
    const auto read_low = execute_completed(*session, *fixture.gpio_input, input, mb::DeviceOperation::read,
                                            mb::GpioReadRequest{});
    CHECK(!std::get<mb::GpioReadResponse>(read_low.response).high);

    const auto closed_output = execute_completed(*session, *fixture.gpio_output, output, mb::DeviceOperation::close,
                                                 mb::CloseRequest{});
    CHECK(closed_output.status == mb::IoStatus::completed);
    CHECK(std::holds_alternative<mb::EmptyResponse>(closed_output.response));
    CHECK(session->open_handle_count() == 1);
    const auto closed_input = execute_completed(*session, *fixture.gpio_input, input, mb::DeviceOperation::close,
                                                mb::CloseRequest{});
    CHECK(closed_input.status == mb::IoStatus::completed);
    CHECK(session->open_handle_count() == 0);
}

void test_invalid_forged_stale_and_wrong_capability_handles() {
    ModelFixture fixture;
    auto session = fixture.registry.create_session(fixture.permissions());
    const auto handle = open_device(*session, *fixture.gpio_input, gpio_input_name);

    expect_rejected(session->execute(mb::DeviceHandle{}, mb::DeviceOperation::read, mb::GpioReadRequest{}),
                    mb::IoStatus::invalid_argument);
    auto forged = handle;
    forged.owner_generation ^= 1U;
    expect_rejected(session->execute(forged, mb::DeviceOperation::read, mb::GpioReadRequest{}),
                    mb::IoStatus::invalid_argument);
    forged = handle;
    forged.object_generation += 1;
    expect_rejected(session->execute(forged, mb::DeviceOperation::read, mb::GpioReadRequest{}),
                    mb::IoStatus::invalid_argument);
    forged = handle;
    forged.slot += 20;
    expect_rejected(session->execute(forged, mb::DeviceOperation::read, mb::GpioReadRequest{}),
                    mb::IoStatus::invalid_argument);
    forged = handle;
    forged.capability = mb::Capability::i2c_controller;
    expect_rejected(session->execute(forged, mb::DeviceOperation::read, mb::I2cReadRequest{}),
                    mb::IoStatus::invalid_argument);
    CHECK(fixture.gpio_input->pending_count() == 0);

    const auto closed = execute_completed(*session, *fixture.gpio_input, handle, mb::DeviceOperation::close,
                                          mb::CloseRequest{});
    CHECK(closed.status == mb::IoStatus::completed);
    expect_rejected(session->execute(handle, mb::DeviceOperation::read, mb::GpioReadRequest{}),
                    mb::IoStatus::invalid_argument);
    expect_rejected(session->execute(handle, mb::DeviceOperation::close, mb::CloseRequest{}),
                    mb::IoStatus::invalid_argument);

    const auto replacement = open_device(*session, *fixture.gpio_input, gpio_input_name);
    CHECK(replacement != handle);
    if (replacement.slot == handle.slot) {
        CHECK(replacement.object_generation != handle.object_generation);
    }
    CHECK(replacement.owner_generation == handle.owner_generation);
    expect_rejected(session->execute(handle, mb::DeviceOperation::read, mb::GpioReadRequest{}),
                    mb::IoStatus::invalid_argument);
    const auto valid = execute_completed(*session, *fixture.gpio_input, replacement, mb::DeviceOperation::read,
                                         mb::GpioReadRequest{});
    CHECK(valid.status == mb::IoStatus::completed);
    const auto wrong_capability = open_device(*session, *fixture.adc, adc_name);
    expect_rejected(session->execute(wrong_capability, mb::DeviceOperation::read, mb::GpioReadRequest{}),
                    mb::IoStatus::invalid_argument);
    CHECK(fixture.adc->pending_count() == 0);
}

void test_unknown_unsupported_and_mismatched_operations() {
    ModelFixture fixture;
    auto session = fixture.registry.create_session(fixture.permissions());
    const auto handle = open_device(*session, *fixture.gpio_output, gpio_output_name);

    expect_rejected(session->execute(handle, static_cast<mb::DeviceOperation>(0xffff), mb::GpioWriteRequest{true}),
                    mb::IoStatus::unsupported);
    expect_rejected(session->execute(handle, mb::DeviceOperation::read, mb::GpioReadRequest{}),
                    mb::IoStatus::unsupported);
    expect_rejected(session->execute(handle, mb::DeviceOperation::transfer, mb::I2cTransferRequest{}),
                    mb::IoStatus::unsupported);
    expect_rejected(session->execute(handle, mb::DeviceOperation::write, mb::GpioReadRequest{}),
                    mb::IoStatus::invalid_argument);
    CHECK(fixture.gpio_output->pending_count() == 0);

    const auto valid = execute_completed(*session, *fixture.gpio_output, handle, mb::DeviceOperation::write,
                                         mb::GpioWriteRequest{true});
    CHECK(valid.status == mb::IoStatus::completed);

    mb::BackendOperation direct;
    direct.id = mb::DeviceOperation::none;
    direct.request = mb::GpioWriteRequest{true};
    CHECK(fixture.gpio_output->start_operation(direct, {}) == mb::IoStatus::unsupported);
    direct.id = mb::DeviceOperation::write;
    direct.request = mb::GpioReadRequest{};
    CHECK(fixture.gpio_output->start_operation(direct, {}) == mb::IoStatus::invalid_argument);
    CHECK(fixture.gpio_output->pending_count() == 0);
}

void test_exact_large_int64_response() {
    ModelFixture fixture;
    auto session = fixture.registry.create_session(fixture.permissions());
    const auto handle = open_device(*session, *fixture.adc, adc_name);
    const auto result = execute_completed(*session, *fixture.adc, handle, mb::DeviceOperation::read,
                                          mb::AdcReadRequest{});
    REQUIRE(result.status == mb::IoStatus::completed);
    const auto* response = std::get_if<mb::AdcReadResponse>(&result.response);
    REQUIRE(response != nullptr);
    constexpr std::int64_t greater_than_binary53 = 9'007'199'254'740'993LL; // 2^53 + 1
    CHECK(response->sample == greater_than_binary53);
    CHECK(response->timestamp_ns == greater_than_binary53 + 1);
    CHECK(response->reference_microvolts == 3'300'000);
    CHECK(response->resolution_bits == 16);
}

void test_i2c_registers_binary_round_trip_and_repeated_start() {
    ModelFixture fixture;
    auto session = fixture.registry.create_session(fixture.permissions());
    const auto handle = open_device(*session, *fixture.i2c, i2c_name);
    constexpr std::uint8_t first_register = 0x10;
    const mb::ByteBuffer binary{0x00, 0xff, 0x80, 0x7f, 0xc3, 0x28, 0xfe};
    mb::ByteBuffer write_bytes{first_register};
    write_bytes.insert(write_bytes.end(), binary.begin(), binary.end());
    auto immutable = immutable_copy(write_bytes);
    const auto expected_request_bytes = write_bytes;
    write_bytes.assign(write_bytes.size(), 0x55);
    CHECK(*immutable == expected_request_bytes);

    mb::I2cWriteRequest initial_write;
    initial_write.address = I2cRegisterBackend::primary_address;
    initial_write.bytes = immutable;
    const auto written = execute_completed(*session, *fixture.i2c, handle, mb::DeviceOperation::write, initial_write);
    const auto& write_response = std::get<mb::I2cTransferResponse>(written.response);
    CHECK(write_response.bytes_written == expected_request_bytes.size());
    CHECK(write_response.fault == mb::I2cFault::none);
    CHECK(!write_response.repeated_start);

    const auto selector = immutable_copy(mb::ByteBuffer{first_register});
    mb::I2cWriteRequest select_request;
    select_request.address = I2cRegisterBackend::primary_address;
    select_request.bytes = selector;
    const auto selected = execute_completed(*session, *fixture.i2c, handle, mb::DeviceOperation::write, select_request);
    CHECK(std::get<mb::I2cTransferResponse>(selected.response).bytes_written == 1);

    mb::I2cReadRequest read_request;
    read_request.address = I2cRegisterBackend::primary_address;
    read_request.byte_count = static_cast<std::uint32_t>(binary.size());
    const auto read = execute_completed(*session, *fixture.i2c, handle, mb::DeviceOperation::read, read_request);
    const auto& read_response = std::get<mb::I2cTransferResponse>(read.response);
    REQUIRE(read_response.received != nullptr);
    CHECK(*read_response.received == binary);
    CHECK(read_response.received != immutable);
    CHECK(read_response.bytes_written == 0);
    CHECK(read_response.fault == mb::I2cFault::none);

    const mb::ByteBuffer transfer_write{0x11, 0x5a};
    mb::I2cTransferRequest transfer_request;
    transfer_request.address = I2cRegisterBackend::primary_address;
    transfer_request.write_bytes = immutable_copy(transfer_write);
    transfer_request.read_bytes = 3;
    transfer_request.repeated_start = true;
    const auto transactions_before = fixture.i2c->transaction_count();
    const auto repeated_starts_before = fixture.i2c->repeated_start_count();
    const auto transferred = execute_completed(*session, *fixture.i2c, handle, mb::DeviceOperation::transfer,
                                                transfer_request);
    const auto& transfer_response = std::get<mb::I2cTransferResponse>(transferred.response);
    REQUIRE(transfer_response.received != nullptr);
    const mb::ByteBuffer expected_received{0x80, 0x7f, 0xc3};
    CHECK(*transfer_response.received == expected_received);
    CHECK(*transfer_response.received != transfer_write);
    CHECK(transfer_response.bytes_written == transfer_write.size());
    CHECK(transfer_response.fault == mb::I2cFault::none);
    CHECK(transfer_response.repeated_start);
    CHECK(fixture.i2c->repeated_start_count() == repeated_starts_before + 1);
    CHECK(fixture.i2c->transaction_count() == transactions_before + 1);
    const auto& transaction = fixture.i2c->last_transaction();
    CHECK(transaction.operation == mb::DeviceOperation::transfer);
    CHECK(transaction.address == I2cRegisterBackend::primary_address);
    CHECK(transaction.write_bytes == transfer_write.size());
    CHECK(transaction.read_bytes == expected_received.size());
    CHECK(transaction.repeated_start);

    mb::I2cReadRequest missing_device;
    missing_device.address = 0x60;
    missing_device.byte_count = 2;
    const auto nack = execute_completed(*session, *fixture.i2c, handle, mb::DeviceOperation::read, missing_device);
    const auto& nack_response = std::get<mb::I2cTransferResponse>(nack.response);
    CHECK(nack_response.fault == mb::I2cFault::nack);
    REQUIRE(nack_response.received != nullptr);
    CHECK(nack_response.received->empty());
    CHECK(!nack_response.repeated_start);
}

void test_pending_queue_saturation_and_recovery() {
    ModelFixture fixture;
    auto session = fixture.registry.create_session(fixture.permissions());
    const auto handle = open_device(*session, *fixture.adc, adc_name);
    const std::uint32_t queue_capacity = mb::DeviceBudget{}.maximum_pending_operations;
    std::vector<mb::DeviceHandle> operations;
    for (std::uint32_t index = 0; index < queue_capacity; ++index) {
        const auto submission = session->execute(handle, mb::DeviceOperation::read, mb::AdcReadRequest{});
        REQUIRE(submission.status == mb::IoStatus::completed);
        operations.push_back(submission.operation);
    }
    CHECK(session->pending_operation_count() == queue_capacity);
    CHECK(fixture.adc->pending_count() == queue_capacity);
    expect_rejected(session->execute(handle, mb::DeviceOperation::read, mb::AdcReadRequest{}), mb::IoStatus::busy);
    CHECK(fixture.adc->pending_count() == queue_capacity);

    fixture.adc->complete_next();
    auto recovered_result = session->poll();
    REQUIRE(recovered_result.has_value());
    CHECK(recovered_result->operation == operations.front());
    CHECK(session->pending_operation_count() == queue_capacity - 1);
    const auto recovered = session->execute(handle, mb::DeviceOperation::read, mb::AdcReadRequest{});
    REQUIRE(recovered.status == mb::IoStatus::completed);
    CHECK(session->pending_operation_count() == queue_capacity);

    for (std::uint32_t index = 0; index < queue_capacity; ++index) {
        fixture.adc->complete_next();
        auto result = session->poll();
        REQUIRE(result.has_value());
        CHECK(result->kind == mb::DeviceResultKind::completion);
        CHECK(result->status == mb::IoStatus::completed);
    }
    CHECK(session->pending_operation_count() == 0);
    CHECK(!session->poll().has_value());
}

void test_retained_byte_saturation_notification_bound_and_poll_recovery() {
    ModelFixture fixture;
    mb::DeviceBudget budget{};
    budget.maximum_pending_operations = budget.maximum_retained_bytes / budget.maximum_bytes_per_buffer;
    auto session = fixture.registry.create_session(fixture.permissions(), budget);
    const auto handle = open_device(*session, *fixture.notifications, notification_name);
    subscribe_notifications(*session, *fixture.notifications, handle);


    const std::size_t bytes_per_notification = budget.maximum_bytes_per_buffer;
    REQUIRE(bytes_per_notification != 0);
    REQUIRE(budget.maximum_retained_bytes % bytes_per_notification == 0);
    const std::size_t notifications_to_fill = budget.maximum_retained_bytes / bytes_per_notification;
    for (std::size_t index = 0; index < notifications_to_fill; ++index) {
        const mb::ByteBuffer bytes(bytes_per_notification, static_cast<std::uint8_t>(index));
        CHECK(fixture.notifications->publish(immutable_copy(bytes)) == mb::IoStatus::completed);
    }
    CHECK(session->queued_notification_count() == notifications_to_fill);
    CHECK(session->retained_byte_count() == budget.maximum_retained_bytes);

    const mb::ByteBuffer overflow_bytes(bytes_per_notification, 0xa5);
    const auto overflow = fixture.notifications->publish(immutable_copy(overflow_bytes));
    CHECK(overflow == mb::IoStatus::busy || overflow == mb::IoStatus::memory_exhausted);
    CHECK(session->queued_notification_count() == notifications_to_fill);
    CHECK(session->retained_byte_count() == budget.maximum_retained_bytes);

    auto first = session->poll();
    REQUIRE(first.has_value());
    CHECK(first->kind == mb::DeviceResultKind::notification);
    const auto* first_response = std::get_if<mb::BleReadResponse>(&first->response);
    REQUIRE(first_response != nullptr);
    REQUIRE(first_response->value != nullptr);
    CHECK(first_response->value->size() == bytes_per_notification);
    CHECK(session->retained_byte_count() == budget.maximum_retained_bytes - bytes_per_notification);

    CHECK(fixture.notifications->publish(immutable_copy(overflow_bytes)) == mb::IoStatus::completed);
    CHECK(session->queued_notification_count() == notifications_to_fill);
    CHECK(session->retained_byte_count() == budget.maximum_retained_bytes);
    for (std::size_t index = 0; index < notifications_to_fill; ++index) {
        auto result = session->poll();
        REQUIRE(result.has_value());
        CHECK(result->kind == mb::DeviceResultKind::notification);
        CHECK(result->status == mb::IoStatus::completed);
    }
    CHECK(!session->poll().has_value());
    CHECK(session->queued_notification_count() == 0);
    CHECK(session->retained_byte_count() == 0);
}

void test_duplicate_close_and_cancel() {
    ModelFixture fixture;
    auto session = fixture.registry.create_session(fixture.permissions());
    const auto handle = open_device(*session, *fixture.gpio_input, gpio_input_name);

    const auto reading = session->execute(handle, mb::DeviceOperation::read, mb::GpioReadRequest{});
    REQUIRE(reading.status == mb::IoStatus::completed);
    CHECK(session->cancel(reading.operation) == mb::IoStatus::completed);
    CHECK(session->cancel(reading.operation) == mb::IoStatus::invalid_argument);
    CHECK(fixture.gpio_input->pending_count() == 1);
    const auto cancelled = finish_completion(*fixture.gpio_input, *session, reading.operation);
    CHECK(cancelled.status == mb::IoStatus::cancelled);
    CHECK(session->pending_operation_count() == 0);
    CHECK(session->cancel(reading.operation) == mb::IoStatus::invalid_argument);

    const auto closing = session->execute(handle, mb::DeviceOperation::close, mb::CloseRequest{});
    REQUIRE(closing.status == mb::IoStatus::completed);
    expect_rejected(session->execute(handle, mb::DeviceOperation::close, mb::CloseRequest{}),
                    mb::IoStatus::invalid_argument);
    CHECK(fixture.gpio_input->pending_count() == 1);
    const auto closed = finish_completion(*fixture.gpio_input, *session, closing.operation);
    CHECK(closed.status == mb::IoStatus::completed);
    CHECK(session->open_handle_count() == 0);
    expect_rejected(session->execute(handle, mb::DeviceOperation::close, mb::CloseRequest{}),
                    mb::IoStatus::invalid_argument);

    const auto reopened = open_device(*session, *fixture.gpio_input, gpio_input_name);
    const auto read = execute_completed(*session, *fixture.gpio_input, reopened, mb::DeviceOperation::read,
                                        mb::GpioReadRequest{});
    CHECK(read.status == mb::IoStatus::completed);
}

void test_rejected_close_preserves_device() {
    ModelFixture fixture;
    auto session = fixture.registry.create_session(fixture.permissions());
    const auto device = open_device(*session, *fixture.gpio_input, gpio_input_name);
    fixture.gpio_input->control_rejections_remaining = 1;

    expect_rejected(session->execute(device, mb::DeviceOperation::close, mb::CloseRequest{}),
                    mb::IoStatus::busy);
    CHECK(session->open_handle_count() == 1);
    CHECK(session->pending_operation_count() == 0);
    CHECK(fixture.gpio_input->pending_count() == 0);
    CHECK(fixture.gpio_input->control_rejections_remaining == 0);

    const auto read = execute_completed(*session, *fixture.gpio_input, device,
                                        mb::DeviceOperation::read, mb::GpioReadRequest{});
    CHECK(read.status == mb::IoStatus::completed);
    CHECK(std::holds_alternative<mb::GpioReadResponse>(read.response));
}

void test_rejected_unsubscribe_preserves_subscription() {
    ModelFixture fixture;
    auto session = fixture.registry.create_session(fixture.permissions());
    const auto device = open_device(*session, *fixture.notifications, notification_name);
    const auto subscription = subscribe_notifications(*session, *fixture.notifications, device);
    fixture.notifications->control_rejections_remaining = 1;

    expect_rejected(session->execute(device, mb::DeviceOperation::unsubscribe,
                                     mb::UnsubscribeRequest{subscription}),
                    mb::IoStatus::busy);
    CHECK(session->open_handle_count() == 2);
    CHECK(session->pending_operation_count() == 0);
    CHECK(fixture.notifications->pending_count() == 0);

    const mb::ByteBuffer payload{0x00, 0xff, 0x80};
    CHECK(fixture.notifications->publish(immutable_copy(payload)) == mb::IoStatus::completed);
    const auto notification = session->poll();
    REQUIRE(notification.has_value());
    CHECK(notification->kind == mb::DeviceResultKind::notification);
    CHECK(notification->subscription == subscription);
}

void test_synchronous_unsubscribe_revokes_subscription() {
    ModelFixture fixture;
    auto session = fixture.registry.create_session(fixture.permissions());
    const auto device = open_device(*session, *fixture.notifications, notification_name);
    const auto subscription = subscribe_notifications(*session, *fixture.notifications, device);
    fixture.notifications->complete_before_accepting = mb::DeviceOperation::unsubscribe;

    const auto submission = session->execute(device, mb::DeviceOperation::unsubscribe,
                                             mb::UnsubscribeRequest{subscription});
    REQUIRE(submission.status == mb::IoStatus::completed);
    CHECK(session->open_handle_count() == 1);
    CHECK(session->pending_operation_count() == 1);
    CHECK(fixture.notifications->pending_count() == 0);
    CHECK(fixture.notifications->publish(immutable_copy({0x01})) == mb::IoStatus::invalid_argument);

    const auto result = session->poll();
    REQUIRE(result.has_value());
    CHECK(result->operation == submission.operation);
    CHECK(result->id == mb::DeviceOperation::unsubscribe);
    CHECK(result->status == mb::IoStatus::completed);
}

void test_backend_polling_delivers_on_the_polling_thread() {
    auto adc = std::make_shared<AdcBackend>(9'007'199'254'740'993LL);
    mb::PeripheralRegistry registry;
    const std::thread::id polling_thread = std::this_thread::get_id();
    registry.register_endpoint({adc_name, adc_version, true}, mb::EndpointAccess::application, adc,
                               ManualBackend::deliver_pending);
    const std::array<mb::CapabilityVersion, 1> capabilities{adc_version};
    const std::array<std::string_view, 1> endpoints{adc_name};
    auto session = registry.create_session({capabilities, endpoints});
    const auto device = open_device(*session, *adc, adc_name);
    const auto submission = session->execute(device, mb::DeviceOperation::read, mb::AdcReadRequest{});
    REQUIRE(submission.status == mb::IoStatus::completed);
    CHECK(adc->pending_count() == 1);

    const auto result = session->poll();
    REQUIRE(result.has_value());
    CHECK(result->operation == submission.operation);
    CHECK(result->status == mb::IoStatus::completed);
    CHECK(adc->last_delivery_thread == polling_thread);
    const auto* reading = std::get_if<mb::AdcReadResponse>(&result->response);
    REQUIRE(reading != nullptr);
    CHECK(reading->sample == 9'007'199'254'740'993LL);
}

void test_shutdown_and_replacement_drop_late_completions() {
    ModelFixture fixture;
    auto old_session = fixture.registry.create_session(fixture.permissions());
    const auto old_owner = old_session->owner_generation();
    const auto old_device = open_device(*old_session, *fixture.gpio_input, gpio_input_name);
    const auto late = old_session->execute(old_device, mb::DeviceOperation::read, mb::GpioReadRequest{});
    REQUIRE(late.status == mb::IoStatus::completed);

    old_session->request_shutdown();
    old_session->request_shutdown();
    CHECK(fixture.gpio_input->release_count(old_owner) == 1);
    CHECK(!old_session->poll().has_value());

    auto replacement = fixture.registry.create_session(fixture.permissions());
    CHECK(replacement->owner_generation() != old_owner);
    const auto new_open = replacement->open(gpio_input_name, mb::OpenRequest{gpio_version});
    REQUIRE(new_open.status == mb::IoStatus::completed);
    REQUIRE(fixture.gpio_input->pending_count() == 2);

    // Index zero belongs to the invalidated owner; index one is the new open.
    fixture.gpio_input->complete_at(0);
    CHECK(!old_session->poll().has_value());
    CHECK(!replacement->poll().has_value());
    fixture.gpio_input->complete_next();
    auto opened = replacement->poll();
    REQUIRE(opened.has_value());
    CHECK(opened->operation == new_open.operation);
    CHECK(opened->id == mb::DeviceOperation::open);
    CHECK(opened->operation != late.operation);
    const auto* response = std::get_if<mb::OpenResponse>(&opened->response);
    REQUIRE(response != nullptr);
    CHECK(response->device != old_device);

    const auto valid = execute_completed(*replacement, *fixture.gpio_input, response->device,
                                          mb::DeviceOperation::read, mb::GpioReadRequest{});
    CHECK(valid.status == mb::IoStatus::completed);
    CHECK(!old_session->poll().has_value());
    CHECK(fixture.gpio_input->release_count(old_owner) == 1);
}

void test_backend_delivery_never_executes_application_handlers() {
    ModelFixture fixture;
    auto session = fixture.registry.create_session(fixture.permissions());
    const std::thread::id polling_thread = std::this_thread::get_id();
    std::size_t handler_calls = 0;
    const auto handler = [&](const mb::DeviceResult&) {
        CHECK(std::this_thread::get_id() == polling_thread);
        ++handler_calls;
    };

    const auto adc = open_device(*session, *fixture.adc, adc_name);
    const auto reading = session->execute(adc, mb::DeviceOperation::read, mb::AdcReadRequest{});
    REQUIRE(reading.status == mb::IoStatus::completed);
    fixture.adc->complete_next();
    CHECK(handler_calls == 0);
    auto completion = session->poll();
    REQUIRE(completion.has_value());
    CHECK(handler_calls == 0);
    handler(*completion);
    CHECK(handler_calls == 1);

    const auto notifications = open_device(*session, *fixture.notifications, notification_name);
    const auto subscription = subscribe_notifications(*session, *fixture.notifications, notifications);
    CHECK(handler_calls == 1);
    const mb::ByteBuffer notification_bytes{0x00, 0xff, 0x80};
    CHECK(fixture.notifications->publish(immutable_copy(notification_bytes)) == mb::IoStatus::completed);
    CHECK(handler_calls == 1);
    auto notification = session->poll();
    REQUIRE(notification.has_value());
    CHECK(notification->kind == mb::DeviceResultKind::notification);
    CHECK(notification->subscription == subscription);
    CHECK(handler_calls == 1);
    handler(*notification);
    CHECK(handler_calls == 2);
    CHECK(!session->poll().has_value());
    CHECK(handler_calls == 2);
}

} // namespace

int main() {
    try {
        test_descriptor_discovery_and_open_versions();
        test_permissions_use_capabilities_and_exact_endpoint_names();
        test_gpio_wiring_and_positive_io();
        test_invalid_forged_stale_and_wrong_capability_handles();
        test_unknown_unsupported_and_mismatched_operations();
        test_exact_large_int64_response();
        test_i2c_registers_binary_round_trip_and_repeated_start();
        test_pending_queue_saturation_and_recovery();
        test_retained_byte_saturation_notification_bound_and_poll_recovery();
        test_duplicate_close_and_cancel();
        test_rejected_close_preserves_device();
        test_rejected_unsubscribe_preserves_subscription();
        test_synchronous_unsubscribe_revokes_subscription();
        test_backend_polling_delivers_on_the_polling_thread();
        test_shutdown_and_replacement_drop_late_completions();
        test_backend_delivery_never_executes_application_handlers();
    } catch (const std::exception& error) {
        std::cerr << "peripheral test exception: " << error.what() << '\n';
        ++failures;
    }
    if (failures != 0) {
        std::cerr << "peripheral failures: " << failures << '\n';
        return failures;
    }
    std::cout << "peripheral tests passed: discovery, permissions, GPIO, handles, I2C, binary and int64 values, "
                 "bounded delivery, cancellation, shutdown, explicit handlers\n";
    return 0;
}
