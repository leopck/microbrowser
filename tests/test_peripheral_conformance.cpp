#include "peripheral_conformance_fixture.h"

#include "device_test_fixture.h"
#include "microbrowser/browser.h"
#include "microbrowser/device_api.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

int failures = 0;
#define CHECK(condition) do { if (!(condition)) { std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: " #condition "\n"; ++failures; } } while (false)
#define REQUIRE(condition) do { if (!(condition)) throw std::runtime_error(std::string(#condition) + " at " + std::to_string(__LINE__)); } while (false)

constexpr std::int64_t above_binary53 = 9'007'199'254'740'993LL;

void expect_rejected(const mb::SubmissionResult& submission, mb::IoStatus status) {
    if (submission.status != status) {
        std::cerr << "conformance rejection expected=" << static_cast<unsigned>(status)
                  << " actual=" << static_cast<unsigned>(submission.status) << '\n';
    }
    CHECK(submission.status == status);
    CHECK(submission.operation.is_empty());
}

template <typename Response>
const Response& response_as(const mb::DeviceResult& result) {
    CHECK(result.status == mb::IoStatus::completed);
    const auto* response = std::get_if<Response>(&result.response);
    REQUIRE(response != nullptr);
    return *response;
}

mb::DeviceResult poll_result(mb::DeviceSession& session) {
    auto result = session.poll();
    REQUIRE(result.has_value());
    return std::move(*result);
}

mb::DeviceResult finish(mb::DeviceSession& session, const mb::SubmissionResult& submission,
                        mb::DeviceOperation operation) {
    REQUIRE(submission.status == mb::IoStatus::completed);
    REQUIRE(!submission.operation.is_empty());
    auto result = poll_result(session);
    CHECK(result.kind == mb::DeviceResultKind::completion);
    CHECK(result.operation == submission.operation);
    CHECK(result.id == operation);
    CHECK(result.status == mb::IoStatus::completed);
    return result;
}

mb::DeviceResult execute_finished(mb::DeviceSession& session, mb::DeviceHandle device,
                                  mb::DeviceOperation operation, mb::DeviceRequest request) {
    return finish(session, session.execute(device, operation, std::move(request)), operation);
}

mb::DeviceHandle open_device(mb::DeviceSession& session, std::string_view endpoint,
                             mb::CapabilityVersion version) {
    auto result = finish(session, session.open(endpoint, mb::OpenRequest{version}), mb::DeviceOperation::open);
    const auto& open = response_as<mb::OpenResponse>(result);
    CHECK(!open.device.is_empty());
    CHECK(open.version == version);
    CHECK(open.modeled);
    CHECK(result.device == open.device);
    return open.device;
}

void expect_notification(const mb::DeviceResult& result, mb::DeviceHandle subscription) {
    CHECK(result.kind == mb::DeviceResultKind::notification);
    CHECK(result.subscription == subscription);
    CHECK(result.id == mb::DeviceOperation::notify);
    CHECK(result.status == mb::IoStatus::completed);
}

void test_exact_grants_and_rejections_before_models() {
    ConformanceFixture fixture;
    auto session = fixture.session();
    const auto endpoints = session->endpoints();
    CHECK(endpoints.size() == fixture.endpoint_names.size());
    for (const auto& endpoint : endpoints) {
        CHECK(endpoint.descriptor.modeled);
        CHECK(endpoint.supports(mb::DeviceOperation::open));
        CHECK(endpoint.supports(mb::DeviceOperation::close));
        CHECK(!endpoint.supports(mb::DeviceOperation::none));
        CHECK(!endpoint.supports(mb::DeviceOperation::cancel));
    }
    const auto found_spi = session->endpoints();
    const auto spi = std::find_if(found_spi.begin(), found_spi.end(), [](const auto& endpoint) {
        return endpoint.descriptor.logical_name == spi_name;
    });
    REQUIRE(spi != found_spi.end());
    CHECK(spi->supports(mb::DeviceOperation::transfer));
    CHECK(!spi->supports(mb::DeviceOperation::read));
    CHECK(!spi->supports(mb::DeviceOperation::write));

    const auto found_event = std::find_if(endpoints.begin(), endpoints.end(), [](const auto& endpoint) {
        return endpoint.descriptor.logical_name == event_link_name;
    });
    REQUIRE(found_event != endpoints.end());
    constexpr std::array<mb::DeviceOperation, 5> event_operations{
        mb::DeviceOperation::open, mb::DeviceOperation::start, mb::DeviceOperation::stop,
        mb::DeviceOperation::read, mb::DeviceOperation::close};
    REQUIRE(found_event->operations.size() == event_operations.size());
    CHECK(std::equal(found_event->operations.begin(), found_event->operations.end(),
                     event_operations.begin(), event_operations.end()));
    CHECK(!found_event->supports(mb::DeviceOperation::configure));
    CHECK(!found_event->supports(mb::DeviceOperation::subscribe));
    CHECK(!fixture.registry.find_endpoint("event_link_groups").has_value());

    const std::array<mb::CapabilityVersion, 1> spi_capability{
        conformance_version(mb::Capability::spi_controller)};
    const std::array<std::string_view, 1> prefix_name{"spi"};
    auto prefix_session = fixture.registry.create_session({spi_capability, prefix_name});
    expect_rejected(prefix_session->open(spi_name, mb::OpenRequest{spi_capability[0]}),
                    mb::IoStatus::permission_denied);

    const std::array<std::string_view, 1> exact_name{spi_name};
    const std::array<mb::CapabilityVersion, 1> wrong_capability{
        conformance_version(mb::Capability::adc)};
    auto wrong_capability_session = fixture.registry.create_session({wrong_capability, exact_name});
    expect_rejected(wrong_capability_session->open(spi_name, mb::OpenRequest{spi_capability[0]}),
                    mb::IoStatus::permission_denied);
    CHECK(fixture.spi->pending_count() == 0);

    mb::DeviceBudget budget;
    budget.maximum_bytes_per_buffer = 8;
    budget.maximum_retained_bytes = 64;
    auto bounded_session = fixture.session(budget);
    const auto device = open_device(*bounded_session, spi_name, spi_capability[0]);
    expect_rejected(bounded_session->execute(device, static_cast<mb::DeviceOperation>(0xffff),
                                             mb::SpiTransferRequest{}),
                    mb::IoStatus::unsupported);
    expect_rejected(bounded_session->execute(device, mb::DeviceOperation::read,
                                             mb::SpiTransferRequest{}),
                    mb::IoStatus::unsupported);
    expect_rejected(bounded_session->execute(device, mb::DeviceOperation::transfer,
                                             mb::UartReadRequest{1}),
                    mb::IoStatus::invalid_argument);
    expect_rejected(bounded_session->execute(device, mb::DeviceOperation::transfer,
                                             mb::SpiTransferRequest{conformance_bytes({1}), 1}),
                    mb::IoStatus::busy);
    CHECK(fixture.spi->pending_count() == 0);

    mb::SpiTransferRequest oversized_payload;
    oversized_payload.transmit = conformance_bytes(mb::ByteBuffer(9, 0xa5));
    expect_rejected(bounded_session->execute(device, mb::DeviceOperation::transfer, oversized_payload),
                    mb::IoStatus::memory_exhausted);
    mb::SpiTransferRequest oversized_receive;
    oversized_receive.transmit = conformance_bytes({});
    oversized_receive.receive_bytes = std::numeric_limits<std::uint32_t>::max();
    expect_rejected(bounded_session->execute(device, mb::DeviceOperation::transfer, oversized_receive),
                    mb::IoStatus::memory_exhausted);
    CHECK(fixture.spi->pending_count() == 0);
    CHECK(!bounded_session->poll().has_value());
}

void test_spi_transfer_accounting_and_lifecycle() {
    ConformanceFixture fixture;
    auto session = fixture.session();
    const auto device = open_device(*session, spi_name, conformance_version(mb::Capability::spi_controller));
    mb::SpiConfigureRequest configure{12'000'000, mb::SpiMode::mode_2,
                                      mb::BitOrder::least_significant_first, mb::SpiLaneCount::quad};
    const auto submission = session->execute(device, mb::DeviceOperation::configure, configure);
    REQUIRE(submission.status == mb::IoStatus::completed);
    REQUIRE(fixture.spi->pending_count() == 1);
    const auto& observed = std::get<mb::SpiConfigureRequest>(fixture.spi->pending_operation().request);
    CHECK(observed.frequency_hz == 12'000'000);
    CHECK(observed.mode == mb::SpiMode::mode_2);
    CHECK(observed.bit_order == mb::BitOrder::least_significant_first);
    CHECK(observed.lanes == mb::SpiLaneCount::quad);
    const auto configured = finish(*session, submission, mb::DeviceOperation::configure);
    CHECK(response_as<mb::SpiConfigureResponse>(configured).actual_frequency_hz == 8'000'000);
    CHECK(fixture.spi->configuration().mode == mb::SpiMode::mode_2);
    CHECK(std::holds_alternative<mb::EmptyResponse>(
        execute_finished(*session, device, mb::DeviceOperation::start, mb::StartRequest{}).response));

    const mb::ByteBuffer transmit{0x00, 0xff, 0x80, 0xc3, 0x28};
    const auto first = execute_finished(*session, device, mb::DeviceOperation::transfer,
                                        mb::SpiTransferRequest{conformance_bytes(transmit), 4});
    const auto& first_response = response_as<mb::SpiTransferResponse>(first);
    REQUIRE(first_response.received != nullptr);
    CHECK(*first_response.received == mb::ByteBuffer({0x00, 0xff, 0x80, 0x01}));
    CHECK(first_response.bytes_transferred == transmit.size());
    CHECK(fixture.spi->transfer_count() == 1);
    CHECK(fixture.spi->transmitted_bytes() == transmit.size());
    CHECK(fixture.spi->received_bytes() == 4);

    const auto second = execute_finished(*session, device, mb::DeviceOperation::transfer,
                                         mb::SpiTransferRequest{conformance_bytes({0x11, 0x22}), 6});
    const auto& second_response = response_as<mb::SpiTransferResponse>(second);
    REQUIRE(second_response.received != nullptr);
    CHECK(*second_response.received == mb::ByteBuffer({0x00, 0xff, 0x80, 0x02, 0x03, 0x04}));
    CHECK(second_response.bytes_transferred == 6);
    CHECK(fixture.spi->transfer_count() == 2);
    CHECK(fixture.spi->transmitted_bytes() == 7);
    CHECK(fixture.spi->received_bytes() == 10);

    execute_finished(*session, device, mb::DeviceOperation::stop, mb::StopRequest{});
    expect_rejected(session->execute(device, mb::DeviceOperation::transfer,
                                     mb::SpiTransferRequest{conformance_bytes({1}), 1}),
                    mb::IoStatus::busy);
    CHECK(fixture.spi->transfer_count() == 2);
    execute_finished(*session, device, mb::DeviceOperation::close, mb::CloseRequest{});
    expect_rejected(session->execute(device, mb::DeviceOperation::transfer,
                                     mb::SpiTransferRequest{conformance_bytes({1}), 1}),
                    mb::IoStatus::invalid_argument);
}

void test_uart_bounded_echo_and_binary_reads() {
    ConformanceFixture fixture;
    auto session = fixture.session();
    const auto device = open_device(*session, uart_name, conformance_version(mb::Capability::uart));
    expect_rejected(session->execute(device, mb::DeviceOperation::write,
                                     mb::UartWriteRequest{conformance_bytes({1})}),
                    mb::IoStatus::busy);
    mb::UartConfigureRequest configure{115'200, 8, 1, mb::UartParity::even,
                                       mb::UartFlowControl::hardware, mb::Rs485Mode::disabled};
    const auto configured = execute_finished(*session, device, mb::DeviceOperation::configure, configure);
    CHECK(response_as<mb::UartConfigureResponse>(configured).actual_baud == 115'200);
    CHECK(fixture.uart->configuration().parity == mb::UartParity::even);
    CHECK(fixture.uart->configuration().flow_control == mb::UartFlowControl::hardware);
    execute_finished(*session, device, mb::DeviceOperation::start, mb::StartRequest{});

    const mb::ByteBuffer first_bytes{0x00, 0xff, 0x80, 0xc3, 0x28, 0x7f, 0x10};
    const mb::ByteBuffer second_bytes{0x11, 0x22, 0x00, 0xfe, 0x80};
    auto written = execute_finished(*session, device, mb::DeviceOperation::write,
                                    mb::UartWriteRequest{conformance_bytes(first_bytes)});
    CHECK(response_as<mb::UartWriteResponse>(written).bytes_written == first_bytes.size());
    written = execute_finished(*session, device, mb::DeviceOperation::write,
                               mb::UartWriteRequest{conformance_bytes(second_bytes)});
    CHECK(response_as<mb::UartWriteResponse>(written).bytes_written == second_bytes.size());
    CHECK(fixture.uart->queued_bytes() == UartModel::receive_capacity);
    expect_rejected(session->execute(device, mb::DeviceOperation::write,
                                     mb::UartWriteRequest{conformance_bytes({0xaa})}),
                    mb::IoStatus::busy);
    CHECK(fixture.uart->pending_count() == 0);
    CHECK(fixture.uart->transmitted_bytes() == first_bytes.size() + second_bytes.size());

    auto read = execute_finished(*session, device, mb::DeviceOperation::read, mb::UartReadRequest{5});
    const auto& first_read = response_as<mb::UartReadResponse>(read);
    REQUIRE(first_read.bytes != nullptr);
    CHECK(*first_read.bytes == mb::ByteBuffer({0x00, 0xff, 0x80, 0xc3, 0x28}));
    CHECK(first_read.timestamp_ns == above_binary53 + 88);
    CHECK(!first_read.errors.overflow && !first_read.errors.framing && !first_read.errors.parity &&
          !first_read.errors.break_detected);
    read = execute_finished(*session, device, mb::DeviceOperation::read, mb::UartReadRequest{20});
    const auto& second_read = response_as<mb::UartReadResponse>(read);
    REQUIRE(second_read.bytes != nullptr);
    mb::ByteBuffer remaining{0x7f, 0x10};
    remaining.insert(remaining.end(), second_bytes.begin(), second_bytes.end());
    CHECK(*second_read.bytes == remaining);
    CHECK(second_read.timestamp_ns == above_binary53 + 89);
    CHECK(fixture.uart->queued_bytes() == 0);
    expect_rejected(session->execute(device, mb::DeviceOperation::read, mb::UartReadRequest{0}),
                    mb::IoStatus::invalid_argument);
    execute_finished(*session, device, mb::DeviceOperation::stop, mb::StopRequest{});
    expect_rejected(session->execute(device, mb::DeviceOperation::read, mb::UartReadRequest{1}),
                    mb::IoStatus::busy);
}

void test_i2c_address_fault_and_atomic_repeated_start() {
    ConformanceFixture fixture;
    auto session = fixture.session();
    const auto device = open_device(*session, i2c_name, conformance_version(mb::Capability::i2c_controller));
    expect_rejected(session->execute(device, mb::DeviceOperation::read,
                                     mb::I2cReadRequest{I2cModel::primary_address, false, 1}),
                    mb::IoStatus::busy);
    const auto configured = execute_finished(*session, device, mb::DeviceOperation::configure,
                                              mb::I2cConfigureRequest{400'000});
    CHECK(response_as<mb::I2cConfigureResponse>(configured).actual_frequency_hz == 400'000);

    const mb::ByteBuffer payload{0x00, 0xff, 0x80, 0xc3, 0x28};
    mb::ByteBuffer initial_write{0x10};
    initial_write.insert(initial_write.end(), payload.begin(), payload.end());
    auto write = execute_finished(*session, device, mb::DeviceOperation::write,
                                  mb::I2cWriteRequest{I2cModel::primary_address, false,
                                                      conformance_bytes(initial_write)});
    CHECK(response_as<mb::I2cTransferResponse>(write).bytes_written == initial_write.size());

    const auto transfer = execute_finished(
        *session, device, mb::DeviceOperation::transfer,
        mb::I2cTransferRequest{I2cModel::primary_address, false, conformance_bytes({0x10}),
                               static_cast<std::uint32_t>(payload.size()), true});
    const auto& transfer_response = response_as<mb::I2cTransferResponse>(transfer);
    REQUIRE(transfer_response.received != nullptr);
    CHECK(*transfer_response.received == payload);
    CHECK(transfer_response.bytes_written == 1);
    CHECK(transfer_response.fault == mb::I2cFault::none);
    CHECK(transfer_response.repeated_start);
    CHECK(fixture.i2c->repeated_start_count() == 1);
    REQUIRE(fixture.i2c->transactions().size() == 2);
    CHECK(fixture.i2c->transactions().back().operation == mb::DeviceOperation::transfer);
    CHECK(fixture.i2c->transactions().back().read == payload.size());

    auto ten_bit = execute_finished(
        *session, device, mb::DeviceOperation::transfer,
        mb::I2cTransferRequest{I2cModel::ten_bit_address, true, conformance_bytes({0x20, 0x00, 0xff}), 0, false});
    CHECK(response_as<mb::I2cTransferResponse>(ten_bit).bytes_written == 3);
    ten_bit = execute_finished(
        *session, device, mb::DeviceOperation::transfer,
        mb::I2cTransferRequest{I2cModel::ten_bit_address, true, conformance_bytes({0x20}), 2, true});
    const auto& ten_bit_response = response_as<mb::I2cTransferResponse>(ten_bit);
    REQUIRE(ten_bit_response.received != nullptr);
    CHECK(*ten_bit_response.received == mb::ByteBuffer({0x00, 0xff}));
    CHECK(ten_bit_response.repeated_start);

    const auto transaction_count = fixture.i2c->transactions().size();
    const auto nack = execute_finished(*session, device, mb::DeviceOperation::read,
                                       mb::I2cReadRequest{0x60, false, 3});
    const auto& fault = response_as<mb::I2cTransferResponse>(nack);
    CHECK(fault.fault == mb::I2cFault::nack);
    CHECK(fault.bytes_written == 0);
    CHECK(!fault.repeated_start);
    REQUIRE(fault.received != nullptr);
    CHECK(fault.received->empty());
    CHECK(fixture.i2c->transactions().size() == transaction_count);
}

void test_adc_and_pwm_actual_configuration_fields() {
    ConformanceFixture fixture;
    auto session = fixture.session();
    const auto adc = open_device(*session, adc_name, conformance_version(mb::Capability::adc));
    expect_rejected(session->execute(adc, mb::DeviceOperation::read, mb::AdcReadRequest{}), mb::IoStatus::busy);
    const auto configure = execute_finished(*session, adc, mb::DeviceOperation::configure,
                                             mb::AdcConfigureRequest{static_cast<std::uint64_t>(above_binary53 + 1),
                                                                     3'300'000});
    const auto& actual = response_as<mb::AdcConfigureResponse>(configure);
    CHECK(actual.actual_sample_time_ns == static_cast<std::uint64_t>(above_binary53 + 1));
    CHECK(actual.actual_reference_microvolts == 3'300'000);
    CHECK(actual.resolution_bits == 18);
    CHECK(actual.calibrated);
    const auto reading = execute_finished(*session, adc, mb::DeviceOperation::read, mb::AdcReadRequest{});
    const auto& sample = response_as<mb::AdcReadResponse>(reading);
    CHECK(sample.sample == above_binary53 + 2);
    CHECK(sample.timestamp_ns == above_binary53 + 6);
    CHECK(sample.reference_microvolts == 3'300'000);
    CHECK(sample.resolution_bits == 18);
    CHECK(sample.calibrated);

    const auto pwm = open_device(*session, pwm_name, conformance_version(mb::Capability::pwm));
    const auto pwm_configured = execute_finished(*session, pwm, mb::DeviceOperation::configure,
                                                  mb::PwmConfigureRequest{1'000'000, 250'000});
    CHECK(response_as<mb::PwmConfigureResponse>(pwm_configured).actual_period_ns == 1'000'000);
    CHECK(fixture.pwm->active_time_ns() == 250'000);
    execute_finished(*session, pwm, mb::DeviceOperation::start, mb::StartRequest{});
    execute_finished(*session, pwm, mb::DeviceOperation::write, mb::PwmWriteRequest{750'000});
    CHECK(fixture.pwm->active_time_ns() == 750'000);
    CHECK(fixture.pwm->period_ns() == 1'000'000);
    CHECK(fixture.pwm->write_count() == 1);
    expect_rejected(session->execute(pwm, mb::DeviceOperation::write, mb::PwmWriteRequest{1'000'001}),
                    mb::IoStatus::invalid_argument);
    CHECK(fixture.pwm->active_time_ns() == 750'000);
    execute_finished(*session, pwm, mb::DeviceOperation::stop, mb::StopRequest{});
}

void test_pulse_capture_sequences_and_output_quantization() {
    ConformanceFixture fixture;
    auto session = fixture.session();
    const auto capture = open_device(*session, pulse_capture_name,
                                     conformance_version(mb::Capability::pulse_capture));
    const auto configured = execute_finished(*session, capture, mb::DeviceOperation::configure,
                                              mb::PulseCaptureConfigureRequest{100, 4});
    CHECK(response_as<mb::PulseCaptureConfigureResponse>(configured).actual_minimum_duration_ns == 100);
    const std::vector<mb::PulseSample> injected{
        {mb::PulseLevel::low, 50, 1},
        {mb::PulseLevel::high, above_binary53 + 8, above_binary53 + 9},
        {mb::PulseLevel::low, 250, above_binary53 + 10}};
    REQUIRE(fixture.pulse_capture->inject(injected));
    auto read = execute_finished(*session, capture, mb::DeviceOperation::read,
                                 mb::PulseCaptureReadRequest{1});
    auto pulses = response_as<mb::PulseCaptureReadResponse>(read).pulses;
    REQUIRE(pulses.size() == 1);
    CHECK(pulses[0].level == mb::PulseLevel::high);
    CHECK(pulses[0].duration_ns == above_binary53 + 8);
    CHECK(pulses[0].timestamp_ns == above_binary53 + 9);
    read = execute_finished(*session, capture, mb::DeviceOperation::read, mb::PulseCaptureReadRequest{4});
    pulses = response_as<mb::PulseCaptureReadResponse>(read).pulses;
    REQUIRE(pulses.size() == 1);
    CHECK(pulses[0].duration_ns == 250);
    expect_rejected(session->execute(capture, mb::DeviceOperation::read, mb::PulseCaptureReadRequest{5}),
                    mb::IoStatus::invalid_argument);

    const auto output = open_device(*session, pulse_output_name,
                                    conformance_version(mb::Capability::pulse_output));
    const auto output_configured = execute_finished(*session, output, mb::DeviceOperation::configure,
                                                     mb::PulseOutputConfigureRequest{8, mb::PulseLevel::high});
    CHECK(response_as<mb::PulseOutputConfigureResponse>(output_configured).actual_resolution_ns == 8);
    execute_finished(*session, output, mb::DeviceOperation::start, mb::StartRequest{});
    const auto durations = std::make_shared<const mb::NanosecondSequence>(
        mb::NanosecondSequence{16, static_cast<std::uint64_t>(above_binary53 + 7), 24});
    const auto written = execute_finished(*session, output, mb::DeviceOperation::write,
                                          mb::PulseOutputWriteRequest{durations});
    CHECK(response_as<mb::PulseOutputWriteResponse>(written).transitions_written == 3);
    CHECK(fixture.pulse_output->last_sequence() == *durations);
    expect_rejected(session->execute(output, mb::DeviceOperation::write,
                                     mb::PulseOutputWriteRequest{
                                         std::make_shared<const mb::NanosecondSequence>(mb::NanosecondSequence{3})}),
                    mb::IoStatus::invalid_argument);
    CHECK(fixture.pulse_output->last_sequence() == *durations);
    execute_finished(*session, output, mb::DeviceOperation::stop, mb::StopRequest{});
}

mb::PcmConfigureRequest pcm_configuration(std::uint32_t rate, std::uint8_t channels,
                                          mb::PcmSampleFormat format, std::uint32_t maximum_frames) {
    return mb::PcmConfigureRequest{{rate, channels, format}, maximum_frames};
}

void test_i2s_transmit_frames_and_transfer_loopback() {
    ConformanceFixture fixture;
    auto session = fixture.session();
    const auto device = open_device(*session, i2s_tx_name, conformance_version(mb::Capability::i2s_transmit));
    const auto configured = execute_finished(
        *session, device, mb::DeviceOperation::configure,
        pcm_configuration(48'000, 2, mb::PcmSampleFormat::signed_24_le, 4));
    const auto& format = response_as<mb::PcmConfigureResponse>(configured).actual_format;
    CHECK(format.frame_rate_hz == 48'000);
    CHECK(format.channel_count == 2);
    CHECK(format.sample_format == mb::PcmSampleFormat::signed_24_le);
    CHECK(fixture.i2s_tx->frame_bytes() == 6);
    execute_finished(*session, device, mb::DeviceOperation::start, mb::StartRequest{});

    mb::ByteBuffer frames{0x00, 0xff, 0x80, 0xc3, 0x28, 0x7f, 0x10, 0x20, 0x30, 0x40, 0x50, 0x60};
    auto immutable = conformance_bytes(frames);
    const auto expected = frames;
    frames.assign(frames.size(), 0x55);
    CHECK(*immutable == expected);
    const auto written = execute_finished(*session, device, mb::DeviceOperation::write,
                                          mb::PcmWriteRequest{immutable});
    CHECK(response_as<mb::PcmWriteResponse>(written).frames_consumed == 2);
    CHECK(fixture.i2s_tx->transmitted_frames() == 2);
    CHECK(fixture.i2s_tx->last_transmit() == expected);

    const auto transfer = execute_finished(*session, device, mb::DeviceOperation::transfer,
                                            mb::PcmTransferRequest{conformance_bytes({1, 2, 3, 4, 5, 6}), 2});
    const auto& loopback = response_as<mb::PcmReadResponse>(transfer);
    REQUIRE(loopback.frames != nullptr);
    REQUIRE(loopback.frames->size() == 12);
    for (std::size_t index = 0; index < loopback.frames->size(); ++index) {
        CHECK((*loopback.frames)[index] == static_cast<std::uint8_t>(index * 29));
    }
    CHECK(loopback.frame_count == 2);
    CHECK(loopback.sequence == 0);
    CHECK(fixture.i2s_tx->transmitted_frames() == 3);
    expect_rejected(session->execute(device, mb::DeviceOperation::read, mb::PcmReadRequest{1}),
                    mb::IoStatus::unsupported);
}

void test_i2s_receive_and_pdm_buffer_bounds() {
    ConformanceFixture fixture;
    auto session = fixture.session();
    const auto receiver = open_device(*session, i2s_rx_name, conformance_version(mb::Capability::i2s_receive));
    execute_finished(*session, receiver, mb::DeviceOperation::configure,
                     pcm_configuration(44'100, 1, mb::PcmSampleFormat::signed_16_le, 4));
    const mb::ByteBuffer captured{0x00, 0xff, 0x80, 0x7f, 0xc3, 0x28, 0x10, 0x20};
    REQUIRE(fixture.i2s_rx->inject_capture(captured));
    CHECK(!fixture.i2s_rx->inject_capture({1, 2}));
    CHECK(fixture.i2s_rx->capture_bytes() == captured.size());
    execute_finished(*session, receiver, mb::DeviceOperation::start, mb::StartRequest{});
    const auto read = execute_finished(*session, receiver, mb::DeviceOperation::read, mb::PcmReadRequest{2});
    const auto& first = response_as<mb::PcmReadResponse>(read);
    REQUIRE(first.frames != nullptr);
    CHECK(*first.frames == mb::ByteBuffer({0x00, 0xff, 0x80, 0x7f}));
    CHECK(first.frame_count == 2);
    CHECK(first.sequence == 0);
    const auto transfer = execute_finished(*session, receiver, mb::DeviceOperation::transfer,
                                            mb::PcmTransferRequest{conformance_bytes({0, 0}), 2});
    const auto& second = response_as<mb::PcmReadResponse>(transfer);
    REQUIRE(second.frames != nullptr);
    CHECK(*second.frames == mb::ByteBuffer({0xc3, 0x28, 0x10, 0x20}));
    CHECK(second.frame_count == 2);
    CHECK(second.sequence == 1);
    CHECK(fixture.i2s_rx->capture_bytes() == 0);

    const auto pdm = open_device(*session, pdm_name, conformance_version(mb::Capability::pdm_receive));
    expect_rejected(session->execute(pdm, mb::DeviceOperation::configure,
                                     pcm_configuration(16'000, 2, mb::PcmSampleFormat::signed_16_le, 4)),
                    mb::IoStatus::invalid_argument);
    expect_rejected(session->execute(pdm, mb::DeviceOperation::configure,
                                     pcm_configuration(16'000, 1, mb::PcmSampleFormat::signed_16_le, 4096)),
                    mb::IoStatus::memory_exhausted);
    CHECK(fixture.pdm->pending_count() == 0);
    const auto pdm_configured = execute_finished(
        *session, pdm, mb::DeviceOperation::configure,
        pcm_configuration(16'000, 1, mb::PcmSampleFormat::signed_16_le, 4));
    CHECK(response_as<mb::PcmConfigureResponse>(pdm_configured).actual_format.frame_rate_hz == 16'000);
    REQUIRE(fixture.pdm->inject_capture({0x00, 0xff, 0x80, 0x7f, 0xc3, 0x28}));
    execute_finished(*session, pdm, mb::DeviceOperation::start, mb::StartRequest{});
    const auto pdm_read = execute_finished(*session, pdm, mb::DeviceOperation::read, mb::PcmReadRequest{3});
    const auto& pdm_response = response_as<mb::PcmReadResponse>(pdm_read);
    REQUIRE(pdm_response.frames != nullptr);
    CHECK(*pdm_response.frames == mb::ByteBuffer({0x00, 0xff, 0x80, 0x7f, 0xc3, 0x28}));
    CHECK(pdm_response.frame_count == 3);
    expect_rejected(session->execute(pdm, mb::DeviceOperation::transfer,
                                     mb::PcmTransferRequest{conformance_bytes({}), 1}),
                    mb::IoStatus::unsupported);
}

mb::BleAttribute empty_ble_filter() {
    mb::BleAttribute filter;
    filter.service.length = 0;
    filter.characteristic.length = 0;
    return filter;
}

void test_ble_central_discovery_connection_attributes_and_cleanup() {
    ConformanceFixture fixture;
    auto session = fixture.session();
    const auto device = open_device(*session, ble_central_name, conformance_version(mb::Capability::ble_central));
    expect_rejected(session->execute(device, mb::DeviceOperation::connect,
                                     mb::BleConnectRequest{conformance_ble_address(), 5'000}),
                    mb::IoStatus::invalid_argument);
    const auto scan = execute_finished(
        *session, device, mb::DeviceOperation::discover,
        mb::BleDiscoverRequest{mb::BleDiscoveryKind::scan, empty_ble_filter(), 2});
    const auto& peers = response_as<mb::BleScanResponse>(scan).peers;
    REQUIRE(peers.size() == 2);
    CHECK(same_address(peers[0].address, conformance_ble_address()));
    CHECK(peers[0].received_signal_strength_dbm == -42);
    REQUIRE(peers[0].advertisement != nullptr);
    CHECK(*peers[0].advertisement == mb::ByteBuffer({0x02, 0x00, 0xff, 0x80}));
    CHECK(peers[1].received_signal_strength_dbm == -67);

    const auto ready = execute_finished(
        *session, device, mb::DeviceOperation::subscribe,
        mb::BleSubscribeRequest{mb::BleSubscriptionTarget::connection_state, empty_ble_filter()});
    const auto connection_subscription = response_as<mb::SubscriptionResponse>(ready).subscription;
    CHECK(!connection_subscription.is_empty());
    const auto connected = execute_finished(*session, device, mb::DeviceOperation::connect,
                                            mb::BleConnectRequest{conformance_ble_address(), 5'000});
    const auto& connection = response_as<mb::BleConnectionResponse>(connected);
    CHECK(connection.connected && connection.paired);
    CHECK(same_address(connection.peer, conformance_ble_address()));
    auto notification = poll_result(*session);
    expect_notification(notification, connection_subscription);
    CHECK(response_as<mb::BleConnectionResponse>(notification).connected);
    expect_rejected(session->execute(device, mb::DeviceOperation::connect,
                                     mb::BleConnectRequest{conformance_ble_address(), 5'000}),
                    mb::IoStatus::busy);

    mb::BleAttribute service_filter{conformance_ble_attribute().service, {}};
    service_filter.characteristic.length = 0;
    const auto services = execute_finished(
        *session, device, mb::DeviceOperation::discover,
        mb::BleDiscoverRequest{mb::BleDiscoveryKind::services, service_filter, 4});
    const auto& characteristics = response_as<mb::BleGattDiscoveryResponse>(services).characteristics;
    REQUIRE(characteristics.size() == 2);
    CHECK(characteristics[0].readable && characteristics[0].writable && characteristics[0].notifiable);
    CHECK(characteristics[1].readable && !characteristics[1].writable && !characteristics[1].notifiable);
    CHECK(same_uuid(characteristics[0].attribute.service, conformance_ble_attribute().service));
    CHECK(same_uuid(characteristics[0].attribute.characteristic,
                    conformance_ble_attribute().characteristic));

    auto read = execute_finished(*session, device, mb::DeviceOperation::read,
                                 mb::BleReadRequest{conformance_ble_attribute()});
    auto value = response_as<mb::BleReadResponse>(read).value;
    REQUIRE(value != nullptr);
    CHECK(*value == mb::ByteBuffer({0x00, 0xff, 0x80}));
    const mb::ByteBuffer replacement{0xc3, 0x28, 0x00, 0xff};
    const auto written = execute_finished(
        *session, device, mb::DeviceOperation::write,
        mb::BleWriteRequest{conformance_ble_attribute(), mb::BleWriteMode::without_response,
                            conformance_bytes(replacement)});
    CHECK(response_as<mb::BleWriteResponse>(written).bytes_written == replacement.size());
    read = execute_finished(*session, device, mb::DeviceOperation::read,
                            mb::BleReadRequest{conformance_ble_attribute()});
    value = response_as<mb::BleReadResponse>(read).value;
    REQUIRE(value != nullptr);
    CHECK(*value == replacement);

    const auto characteristic_ready = execute_finished(
        *session, device, mb::DeviceOperation::subscribe,
        mb::BleSubscribeRequest{mb::BleSubscriptionTarget::characteristic, conformance_ble_attribute()});
    const auto characteristic_subscription = response_as<mb::SubscriptionResponse>(characteristic_ready).subscription;
    const mb::ByteBuffer emitted{0x80, 0x00, 0xff, 0xc3, 0x28};
    CHECK(fixture.ble_central->emit(conformance_ble_attribute(), emitted) == mb::IoStatus::completed);
    notification = poll_result(*session);
    expect_notification(notification, characteristic_subscription);
    const auto& characteristic = response_as<mb::BleReadResponse>(notification);
    REQUIRE(characteristic.value != nullptr);
    CHECK(*characteristic.value == emitted);
    CHECK(same_attribute(characteristic.attribute, conformance_ble_attribute()));
    execute_finished(*session, device, mb::DeviceOperation::unsubscribe,
                     mb::UnsubscribeRequest{characteristic_subscription});
    CHECK(fixture.ble_central->emit(conformance_ble_attribute(), emitted) == mb::IoStatus::invalid_argument);

    execute_finished(*session, device, mb::DeviceOperation::disconnect,
                     mb::BleDisconnectRequest{conformance_ble_address()});
    notification = poll_result(*session);
    expect_notification(notification, connection_subscription);
    const auto& disconnected = response_as<mb::BleConnectionResponse>(notification);
    CHECK(!disconnected.connected && !disconnected.paired);
    expect_rejected(session->execute(device, mb::DeviceOperation::read,
                                     mb::BleReadRequest{conformance_ble_attribute()}),
                    mb::IoStatus::busy);
}

void test_ble_peripheral_advertisement_write_notify_and_unsubscribe() {
    ConformanceFixture fixture;
    auto session = fixture.session();
    const auto device = open_device(*session, ble_peripheral_name,
                                    conformance_version(mb::Capability::ble_peripheral));
    const mb::ByteBuffer advertisement{0x02, 0x01, 0x80, 0x03, 0xff, 0xc3, 0x28};
    execute_finished(*session, device, mb::DeviceOperation::advertise,
                     mb::BleAdvertiseRequest{conformance_bytes(advertisement), 100});
    CHECK(fixture.ble_peripheral->advertising());
    CHECK(fixture.ble_peripheral->advertisement_interval_ms() == 100);
    CHECK(*fixture.ble_peripheral->advertisement() == advertisement);

    const mb::ByteBuffer local_value{0x00, 0xff, 0x80, 0x7f};
    const auto write = execute_finished(
        *session, device, mb::DeviceOperation::write,
        mb::BleWriteRequest{conformance_ble_attribute(), mb::BleWriteMode::with_response,
                            conformance_bytes(local_value)});
    CHECK(response_as<mb::BleWriteResponse>(write).bytes_written == local_value.size());
    const auto read = execute_finished(*session, device, mb::DeviceOperation::read,
                                       mb::BleReadRequest{conformance_ble_attribute()});
    REQUIRE(response_as<mb::BleReadResponse>(read).value != nullptr);
    CHECK(*response_as<mb::BleReadResponse>(read).value == local_value);

    const auto ready = execute_finished(
        *session, device, mb::DeviceOperation::subscribe,
        mb::BleSubscribeRequest{mb::BleSubscriptionTarget::characteristic, conformance_ble_attribute()});
    const auto subscription = response_as<mb::SubscriptionResponse>(ready).subscription;
    const mb::ByteBuffer notification_bytes{0x00, 0xc3, 0x28, 0xff, 0x80};
    execute_finished(*session, device, mb::DeviceOperation::notify,
                     mb::BleNotifyRequest{conformance_ble_attribute(), conformance_bytes(notification_bytes)});
    const auto notification = poll_result(*session);
    expect_notification(notification, subscription);
    REQUIRE(response_as<mb::BleReadResponse>(notification).value != nullptr);
    CHECK(*response_as<mb::BleReadResponse>(notification).value == notification_bytes);
    CHECK(fixture.ble_peripheral->notification_count() == 1);
    CHECK(fixture.ble_peripheral->last_notification() == notification_bytes);
    execute_finished(*session, device, mb::DeviceOperation::unsubscribe, mb::UnsubscribeRequest{subscription});
    execute_finished(*session, device, mb::DeviceOperation::notify,
                     mb::BleNotifyRequest{conformance_ble_attribute(), conformance_bytes({1, 2})});
    CHECK(!session->poll().has_value());
    CHECK(fixture.ble_peripheral->notification_count() == 2);
    expect_rejected(session->execute(device, mb::DeviceOperation::discover,
                                     mb::BleDiscoverRequest{mb::BleDiscoveryKind::scan, {}, 1}),
                    mb::IoStatus::unsupported);
}

void test_usb_host_discovery_bulk_io_control_and_disconnect() {
    ConformanceFixture fixture;
    auto session = fixture.session();
    const auto device = open_device(*session, usb_host_name, conformance_version(mb::Capability::usb_host));
    expect_rejected(session->execute(device, mb::DeviceOperation::connect, mb::UsbConnectRequest{7}),
                    mb::IoStatus::invalid_argument);
    const auto discovery = execute_finished(*session, device, mb::DeviceOperation::discover,
                                             mb::UsbDiscoverRequest{2});
    const auto& devices = response_as<mb::UsbDiscoveryResponse>(discovery).devices;
    REQUIRE(devices.size() == 2);
    CHECK(devices[0].address == 7);
    CHECK(devices[0].vendor_id == 0xcafe);
    CHECK(devices[0].product_id == 0x1007);
    REQUIRE(devices[0].device_descriptor != nullptr);
    CHECK(devices[0].device_descriptor->size() == 18);
    CHECK((*devices[0].device_descriptor)[8] == 0xc3);

    execute_finished(*session, device, mb::DeviceOperation::connect, mb::UsbConnectRequest{7});
    const mb::ByteBuffer input{0x00, 0xff, 0x80, 0xc3, 0x28, 0x7f};
    REQUIRE(fixture.usb_host->inject_input(0x81, input));
    const auto read = execute_finished(*session, device, mb::DeviceOperation::read,
                                       mb::UsbReadRequest{0x81, 5, 25});
    const auto& input_response = response_as<mb::UsbTransferResponse>(read);
    REQUIRE(input_response.received != nullptr);
    CHECK(*input_response.received == mb::ByteBuffer({0x00, 0xff, 0x80, 0xc3, 0x28}));
    CHECK(input_response.bytes_transferred == 5);
    const mb::ByteBuffer output{0x10, 0x00, 0xff, 0x80};
    const auto written = execute_finished(*session, device, mb::DeviceOperation::write,
                                          mb::UsbWriteRequest{0x02, conformance_bytes(output), 25});
    CHECK(response_as<mb::UsbWriteResponse>(written).bytes_written == output.size());
    CHECK(fixture.usb_host->last_write_endpoint() == 0x02);
    CHECK(fixture.usb_host->last_write() == output);

    mb::UsbTransferRequest control;
    control.setup = {0x80, 6, 0x0100, 0};
    control.bytes = conformance_bytes({});
    control.receive_bytes = 8;
    control.timeout_ms = 25;
    const auto transferred = execute_finished(*session, device, mb::DeviceOperation::transfer, control);
    const auto& control_response = response_as<mb::UsbTransferResponse>(transferred);
    REQUIRE(control_response.received != nullptr);
    CHECK(control_response.received->size() == 8);
    CHECK(std::equal(control_response.received->begin(), control_response.received->end(),
                     devices[0].device_descriptor->begin()));
    CHECK(control_response.bytes_transferred == 8);
    execute_finished(*session, device, mb::DeviceOperation::disconnect, mb::UsbDisconnectRequest{7});
    expect_rejected(session->execute(device, mb::DeviceOperation::read,
                                     mb::UsbReadRequest{0x81, 1, 25}),
                    mb::IoStatus::busy);
}

void test_usb_device_descriptors_stream_io_and_control() {
    ConformanceFixture fixture;
    auto session = fixture.session();
    const auto device = open_device(*session, usb_device_name, conformance_version(mb::Capability::usb_device));
    expect_rejected(session->execute(device, mb::DeviceOperation::start, mb::StartRequest{}), mb::IoStatus::busy);
    const mb::ByteBuffer descriptor = usb_descriptor(0xc3);
    const mb::ByteBuffer interface_bytes{0x00, 0xff, 0x80, 0x7f};
    mb::UsbInterfaceDescriptor interface{3, 0xff, 0x80, 0x7f, conformance_bytes(interface_bytes)};
    const auto configured = execute_finished(
        *session, device, mb::DeviceOperation::configure,
        mb::UsbDeviceConfigureRequest{conformance_bytes(descriptor), {interface}});
    CHECK(response_as<mb::UsbConfigureResponse>(configured).actual_configuration == 1);
    REQUIRE(fixture.usb_device->interfaces().size() == 1);
    CHECK(fixture.usb_device->interfaces()[0].interface_number == 3);
    CHECK(fixture.usb_device->interfaces()[0].interface_class == 0xff);
    REQUIRE(fixture.usb_device->interfaces()[0].descriptors != nullptr);
    CHECK(*fixture.usb_device->interfaces()[0].descriptors == interface_bytes);
    execute_finished(*session, device, mb::DeviceOperation::start, mb::StartRequest{});

    const mb::ByteBuffer host_output{0x00, 0xff, 0x80, 0xc3, 0x28};
    REQUIRE(fixture.usb_device->inject_host_output(host_output));
    const auto read = execute_finished(*session, device, mb::DeviceOperation::read,
                                       mb::UsbReadRequest{0x01, 5, 25});
    REQUIRE(response_as<mb::UsbTransferResponse>(read).received != nullptr);
    CHECK(*response_as<mb::UsbTransferResponse>(read).received == host_output);
    const mb::ByteBuffer host_input{0x12, 0x00, 0xff, 0x80};
    const auto written = execute_finished(*session, device, mb::DeviceOperation::write,
                                          mb::UsbWriteRequest{0x81, conformance_bytes(host_input), 25});
    CHECK(response_as<mb::UsbWriteResponse>(written).bytes_written == host_input.size());
    CHECK(fixture.usb_device->last_host_input() == host_input);

    mb::UsbTransferRequest control;
    control.setup = {0x80, 6, 0x0100, 0};
    control.bytes = conformance_bytes({});
    control.receive_bytes = 6;
    const auto transferred = execute_finished(*session, device, mb::DeviceOperation::transfer, control);
    const auto& control_response = response_as<mb::UsbTransferResponse>(transferred);
    REQUIRE(control_response.received != nullptr);
    CHECK(control_response.received->size() == 6);
    CHECK(std::equal(control_response.received->begin(), control_response.received->end(), descriptor.begin()));
    execute_finished(*session, device, mb::DeviceOperation::stop, mb::StopRequest{});
    expect_rejected(session->execute(device, mb::DeviceOperation::read, mb::UsbReadRequest{1, 1, 25}),
                    mb::IoStatus::busy);
    expect_rejected(session->execute(device, mb::DeviceOperation::discover, mb::UsbDiscoverRequest{1}),
                    mb::IoStatus::unsupported);
}

void test_owner_selected_event_link_lifecycle_and_exact_count() {
    ConformanceFixture fixture;
    auto session = fixture.session();
    const auto device = open_device(*session, event_link_name, conformance_version(mb::Capability::event_link));
    auto read = execute_finished(*session, device, mb::DeviceOperation::read, mb::EventLinkReadRequest{});
    auto status = response_as<mb::EventLinkReadResponse>(read);
    CHECK(!status.enabled && !status.hardware_autonomous && status.trigger_count == 0);
    execute_finished(*session, device, mb::DeviceOperation::start, mb::StartRequest{});
    fixture.event_link->select_triggers(above_binary53 + 20);
    read = execute_finished(*session, device, mb::DeviceOperation::read, mb::EventLinkReadRequest{});
    status = response_as<mb::EventLinkReadResponse>(read);
    CHECK(status.enabled);
    CHECK(!status.hardware_autonomous);
    CHECK(status.trigger_count == above_binary53 + 20);
    execute_finished(*session, device, mb::DeviceOperation::stop, mb::StopRequest{});
    fixture.event_link->select_triggers(50);
    read = execute_finished(*session, device, mb::DeviceOperation::read, mb::EventLinkReadRequest{});
    status = response_as<mb::EventLinkReadResponse>(read);
    CHECK(!status.enabled && !status.hardware_autonomous);
    CHECK(status.trigger_count == above_binary53 + 20);
    expect_rejected(session->execute(device, mb::DeviceOperation::configure, mb::GpioConfigureRequest{}),
                    mb::IoStatus::unsupported);
    expect_rejected(session->execute(device, mb::DeviceOperation::subscribe, mb::GpioSubscribeRequest{}),
                    mb::IoStatus::unsupported);
}

device_test::PackageSpec adc_package() {
    device_test::PackageSpec package;
    package.capabilities.push_back({conformance_version(mb::Capability::adc), true});
    package.permissions.emplace_back(adc_name);
    package.state("device", mb::Value::from_handle());
    package.state("operation", mb::Value::from_handle());
    package.state("status", mb::Value::from_integer(-1));
    package.state("callback_count", mb::Value::from_integer(0));
    package.state("observed_config", mb::Value::from_integer(0));
    package.state("observed_sample", mb::Value::from_integer(0));
    package.state("observed_timestamp", mb::Value::from_integer(0));
    return package;
}

void add_adc_open(device_test::PackageSpec& package) {
    device_test::add_callback(package, "on_open", "device");
    auto& open = package.function("open_adc");
    open.push(mb::Value::from_integer(static_cast<std::int64_t>(mb::Capability::adc)))
        .push(mb::Value::from_string(std::string(adc_name)))
        .push(mb::Value::from_integer(0))
        .push(mb::Value::from_string("on_open"))
        .device(static_cast<std::uint16_t>(mb::DeviceOperation::open), 4)
        .pop()
        .finish();
}

void add_adc_configure(device_test::PackageSpec& package, std::string name, std::int64_t sample_time_ns) {
    device_test::add_callback(package, "on_config", "observed_config");
    auto& configure = package.function(std::move(name));
    configure.load_state(package.state("device"))
        .push(mb::Value::from_integer(sample_time_ns))
        .push(mb::Value::from_integer(3'300'000))
        .push(mb::Value::from_string("on_config"))
        .device(static_cast<std::uint16_t>(mb::DeviceOperation::configure), 4)
        .pop()
        .finish();
}

void add_exact_adc_read(device_test::PackageSpec& package) {
    auto& callback = package.function("on_adc", 3, 3);
    callback.load_local(1).store_state(package.state("status"));
    callback.load_local(0).store_state(package.state("operation"));
    callback.load_local(2).store_state(package.state("observed_sample"));
    device_test::increment_callbacks(package, callback);
    callback.load_local(0)
        .push(mb::Value::from_integer(static_cast<std::int64_t>(mb::DeviceMetadataField::timestamp_ns)))
        .device(static_cast<std::uint16_t>(mb::DeviceNativeOperation::metadata), 2)
        .store_state(package.state("observed_timestamp"))
        .finish();
    auto& read = package.function("read_adc");
    read.load_state(package.state("device"))
        .push(mb::Value::from_string("on_adc"))
        .device(static_cast<std::uint16_t>(mb::DeviceOperation::read), 2)
        .pop()
        .finish();
}

void add_short_callback_read(device_test::PackageSpec& package) {
    package.function("short_callback", 2, 2).finish();
    auto& read = package.function("read_short_callback");
    read.load_state(package.state("device"))
        .push(mb::Value::from_string("short_callback"))
        .device(static_cast<std::uint16_t>(mb::DeviceOperation::read), 2)
        .pop()
        .finish();
}

void add_integer_export(device_test::PackageSpec& package, std::string function,
                        std::string_view state, std::string key) {
    auto& exporter = package.function(std::move(function));
    exporter.push(mb::Value::from_string(std::move(key)))
        .load_state(package.state(state))
        .device(static_cast<std::uint16_t>(mb::DeviceNativeOperation::number), 1)
        .builtin(13, 2)
        .pop()
        .finish();
}

void add_exact_integer_comparison(device_test::PackageSpec& package, std::string function,
                                  std::string_view state, std::string key, std::int64_t expected) {
    auto& comparison = package.function(std::move(function));
    comparison.push(mb::Value::from_string(std::move(key)))
        .load_state(package.state(state))
        .push(mb::Value::from_integer(expected))
        .equal()
        .builtin(15, 2)
        .pop()
        .finish();
}

struct VmConformanceHarness {
    explicit VmConformanceHarness(const device_test::PackageSpec& package)
        : browser(platform, {64, 64}, "http://127.0.0.1:8765", &fixture.registry) {
        browser.load(package.encode());
    }

    std::int64_t exported_integer(std::string_view exporter, std::string_view key) {
        browser.dispatch(exporter);
        const auto found = platform.values.find(std::string(key));
        REQUIRE(found != platform.values.end());
        REQUIRE(found->second.type == mb::Value::Type::number);
        return static_cast<std::int64_t>(found->second.number);
    }

    bool exported_boolean(std::string_view exporter, std::string_view key) {
        browser.dispatch(exporter);
        const auto found = platform.values.find(std::string(key));
        REQUIRE(found != platform.values.end());
        REQUIRE(found->second.type == mb::Value::Type::boolean);
        return found->second.boolean;
    }

    ConformanceFixture fixture;
    device_test::TestPlatform platform;
    mb::Browser browser;
};

void test_vm_adc_exact_int64_metadata_and_single_callback_delivery() {
    auto package = adc_package();
    add_adc_open(package);
    add_adc_configure(package, "configure_adc", above_binary53 + 1);
    add_exact_adc_read(package);
    add_integer_export(package, "export_count", "callback_count", "count");
    add_exact_integer_comparison(package, "export_sample", "observed_sample", "sample", above_binary53 + 2);
    add_exact_integer_comparison(package, "export_timestamp", "observed_timestamp", "timestamp",
                                 above_binary53 + 6);
    add_exact_integer_comparison(package, "export_config", "observed_config", "config", above_binary53 + 1);
    VmConformanceHarness harness(package);

    harness.browser.dispatch("open_adc");
    REQUIRE(harness.fixture.adc->pending_count() == 1);
    harness.browser.tick();
    CHECK(harness.exported_integer("export_count", "count") == 1);
    harness.browser.dispatch("configure_adc");
    REQUIRE(harness.fixture.adc->pending_count() == 1);
    const auto& configuration = std::get<mb::AdcConfigureRequest>(harness.fixture.adc->pending_operation().request);
    CHECK(configuration.sample_time_ns == static_cast<std::uint64_t>(above_binary53 + 1));
    CHECK(configuration.reference_microvolts == 3'300'000);
    harness.browser.tick();
    CHECK(harness.exported_integer("export_count", "count") == 2);
    CHECK(harness.exported_boolean("export_config", "config"));

    harness.browser.dispatch("read_adc");
    REQUIRE(harness.fixture.adc->pending_count() == 1);
    harness.browser.tick();
    CHECK(harness.fixture.adc->pending_count() == 0);
    CHECK(harness.exported_integer("export_count", "count") == 3);
    CHECK(harness.exported_boolean("export_sample", "sample"));
    CHECK(harness.exported_boolean("export_timestamp", "timestamp"));
    harness.browser.tick();
    harness.browser.tick();
    CHECK(harness.exported_integer("export_count", "count") == 3);
    CHECK(harness.browser.last_error().empty());
}

void test_vm_callback_and_overflow_flakes_never_reach_model() {
    auto package = adc_package();
    add_adc_open(package);
    add_adc_configure(package, "configure_adc", above_binary53 + 1);
    add_adc_configure(package, "configure_overflow", -1);
    add_short_callback_read(package);
    add_integer_export(package, "export_count", "callback_count", "count");
    VmConformanceHarness harness(package);
    harness.browser.dispatch("open_adc");
    harness.browser.tick();
    harness.browser.dispatch("configure_adc");
    harness.browser.tick();
    CHECK(harness.exported_integer("export_count", "count") == 2);

    harness.browser.dispatch("configure_overflow");
    CHECK(std::string(harness.browser.last_error()).find("outside 0..") != std::string::npos);
    CHECK(harness.fixture.adc->pending_count() == 0);
    harness.browser.dispatch("read_short_callback");
    CHECK(std::string(harness.browser.last_error()).find("exactly three arguments") != std::string::npos);
    CHECK(harness.fixture.adc->pending_count() == 0);
    CHECK(harness.exported_integer("export_count", "count") == 2);
    harness.browser.tick();
    CHECK(harness.fixture.adc->pending_count() == 0);
}

} // namespace

int main() {
    try {
        test_exact_grants_and_rejections_before_models();
        test_spi_transfer_accounting_and_lifecycle();
        test_uart_bounded_echo_and_binary_reads();
        test_i2c_address_fault_and_atomic_repeated_start();
        test_adc_and_pwm_actual_configuration_fields();
        test_pulse_capture_sequences_and_output_quantization();
        test_i2s_transmit_frames_and_transfer_loopback();
        test_i2s_receive_and_pdm_buffer_bounds();
        test_ble_central_discovery_connection_attributes_and_cleanup();
        test_ble_peripheral_advertisement_write_notify_and_unsubscribe();
        test_usb_host_discovery_bulk_io_control_and_disconnect();
        test_usb_device_descriptors_stream_io_and_control();
        test_owner_selected_event_link_lifecycle_and_exact_count();
        test_vm_adc_exact_int64_metadata_and_single_callback_delivery();
        test_vm_callback_and_overflow_flakes_never_reach_model();
    } catch (const std::exception& error) {
        std::cerr << "peripheral conformance exception: " << error.what() << '\n';
        return 1;
    }
    if (failures != 0) {
        std::cerr << failures << " peripheral conformance checks failed\n";
        return 1;
    }
    std::cout << "peripheral conformance passed: SPI, UART, I2C, ADC, PWM, pulse, I2S, PDM, BLE, USB, "
                 "event link, exact VM callbacks, permissions and rejection paths\n";
    return 0;
}
