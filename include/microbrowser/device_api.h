#pragma once

#include "microbrowser/peripherals.h"

#include <cstdint>
#include <string_view>

namespace mb {

// Opcode 25 operands 200..210 are scalar conversions and callback metadata,
// not DeviceOperation submission identifiers.
enum class DeviceNativeOperation : std::uint16_t {
    capability_supported = 200,
    metadata = 201,
    bytes = 202,
    bytes_length = 203,
    byte_at = 204,
    integer = 205,
    number = 206,
    one_byte = 207,
    append_byte = 208,
    concatenate_bytes = 209,
    usb_setup = 210,
};

// Metadata numbers are append-only. Indexed fields require metadata's third
// argument; all other fields reject it. Index zero denotes the first entry.
enum class DeviceMetadataField : std::uint16_t {
    io_status = 1,
    timestamp_ns = 2,
    sequence = 3,
    modeled = 4,
    capability_major = 5,
    capability_minor = 6,
    actual_sample_time_ns = 7,
    actual_reference_microvolts = 8,
    reference_microvolts = 9,
    resolution_bits = 10,
    calibrated = 11,
    actual_period_ns = 12,
    actual_frequency_hz = 13,
    actual_baud = 14,
    actual_minimum_duration_ns = 15,
    actual_resolution_ns = 16,
    frame_rate_hz = 17,
    channel_count = 18,
    pcm_sample_format = 19,
    actual_configuration = 20,
    sample = 21,
    received = 22,
    bytes_transferred = 23,
    bytes_written = 24,
    i2c_fault = 25,
    repeated_start = 26,
    uart_bytes = 27,
    uart_error_overflow = 28,
    uart_error_framing = 29,
    uart_error_parity = 30,
    uart_error_break_detected = 31,
    pulse_count = 32,
    // pulse_samples_le64 is 24 bytes per pulse: duration_ns, timestamp_ns,
    // then level (0=low, 1=high), each encoded as an unsigned little-endian
    // 64-bit word. Signed nanoseconds use their two's-complement bit pattern.
    pulse_samples_le64 = 33,
    durations_ns_le64 = 34,
    pulse_timestamps_ns_le64 = 35,
    transitions_written = 36,
    pcm_frames = 37,
    frame_count = 38,
    frames_consumed = 39,
    peer_count = 40,
    peer_address = 41,
    peer_address_random = 42,
    received_signal_strength_dbm = 43,
    advertisement = 44,
    characteristic_count = 45,
    service_uuid = 46,
    characteristic_uuid = 47,
    readable = 48,
    writable = 49,
    notifiable = 50,
    connected = 51,
    connected_peer_address = 52,
    connected_peer_address_random = 53,
    paired = 54,
    attribute_service_uuid = 55,
    attribute_characteristic_uuid = 56,
    ble_value = 57,
    enabled = 58,
    hardware_autonomous = 59,
    trigger_count = 60,
    usb_device_count = 61,
    usb_device_address = 62,
    usb_vendor_id = 63,
    usb_product_id = 64,
    usb_device_descriptor = 65,
};

[[nodiscard]] std::string_view device_metadata_field_name(DeviceMetadataField field);

// Source-level request forms. `device`/`operation`/`subscription` are opaque
// handles, integers marked i are int64 or exactly-integral f32 values, and cb
// names a three-argument function. Every asynchronous form returns its new
// operation handle. The completion receives (operation, status, primary).
// Subscription ready receives (operation, status, subscription); subsequent
// notifications receive (subscription, status, primary). Failed/data-less
// completions have void primary, never a fabricated zero.
//
// open(capability:i, endpoint:string, minimum_minor:i, cb) [operation 1; ABI major 1]
// configure(device, ..., cb) [2], by device capability:
//   gpio(direction:i 0..1, pull:i 0..2, initial_high:bool)
//   adc(sample_time_ns:i, reference_microvolts:i)
//   pwm(period_ns:i, initial_active_time_ns:i)
//   spi/spi_peripheral/spi_multilane(frequency_hz:i, mode:i 0..3,
//       bit_order:i 0..1, lanes:i one of 1,2,4)
//   i2c_controller/i2c_peripheral(frequency_hz:i)
//   uart/rs485(baud:i, data_bits:i, stop_bits:i, parity:i 0..2,
//       flow_control:i 0..1, rs485_mode:i 0..1)
//   pulse_capture(minimum_duration_ns:i, maximum_pulses:i)
//   pulse_output(resolution_ns:i, idle_level:i 0..1)
//   i2s_transmit/i2s_receive/pdm_receive(frame_rate_hz:i, channels:i,
//       sample_format:i 0..3, maximum_frames_per_block:i)
//   usb_device(device_descriptor:bytes, interface_table:bytes). Version-1
//       interface_table is u16_le count<=8 followed by count records of
//       {u8 interface_number,class,subclass,protocol,u16_le descriptor_length,
//       descriptor_bytes}; numbers must be unique and the table exact.
// start(device, cb) [3]; stop(device, cb) [4]; close(device, cb) [5]
// read(device, ..., cb) [6], by capability:
//   gpio/adc/event_link()
//   pulse_capture(maximum_pulses:i)
//   spi_peripheral(maximum_bytes:i)
//   i2c_controller(address:i, ten_bit:bool, byte_count:i)
//   i2c_peripheral(maximum_bytes:i)
//   uart/rs485(maximum_bytes:i)
//   i2s_receive/pdm_receive(frame_count:i)
//   ble_central/ble_peripheral(service_uuid:bytes, characteristic_uuid:bytes)
//   usb_device/usb_host(endpoint:i, byte_count:i, timeout_ms:i)
// write(device, ..., cb) [7], by capability:
//   gpio(high:bool); pwm(active_time_ns:i)
//   pulse_output(durations_ns_le64:bytes, nonempty, size divisible by 8)
//   spi_peripheral/i2c_peripheral/uart/rs485/i2s_transmit(bytes:bytes)
//   i2c_controller(address:i, ten_bit:bool, bytes:bytes)
//   ble_central/ble_peripheral(service_uuid:bytes, characteristic_uuid:bytes,
//       mode:i 0..1, value:bytes)
//   usb_device/usb_host(endpoint:i, bytes:bytes, timeout_ms:i)
// transfer(device, ..., cb) [8]:
//   spi/spi_peripheral/spi_multilane(transmit:bytes, receive_bytes:i)
//   i2c_controller(address:i, ten_bit:bool, write_bytes:bytes, read_bytes:i,
//       repeated_start:bool)
//   i2s_transmit/i2s_receive(transmit_frames:bytes, receive_frames:i)
//   usb_device/usb_host(standard_8_byte_setup:bytes, transmit:bytes,
//       receive_bytes:i, timeout_ms:i). SETUP encodes request_type, request,
//       wValueLE, wIndexLE, wLengthLE and transfer direction must match lengths.
// subscribe(device, ..., ready_cb, notification_cb) [9]:
//   gpio(edge:i 0..2)
//   ble_central/ble_peripheral(target:i 0..1, service_uuid:bytes,
//       characteristic_uuid:bytes). Connection-state target 0 requires both
//       UUID buffers empty; characteristic target 1 requires 2- or 16-byte UUIDs.
// unsubscribe(device, subscription, cb) [10]
// cancel(operation) [11] is synchronous, requests cancellation, returns void.
// notify(device, service_uuid:bytes, characteristic_uuid:bytes, value:bytes, cb)
//   [12, ble_peripheral]
// connect [13] / disconnect [14]:
//   ble_central(device, peer_address:bytes[6], random:bool,
//       supervision_timeout_ms:i, cb) for connect; disconnect omits the timeout.
// One BLE DeviceHandle has at most one active remote peer. A second connect
// while bound requires disconnect first; drivers reject it busy without changing
// the peer beneath outstanding attribute operations.
//   usb_host(device, device_address:i, cb)
// discover(device, ..., cb) [15]:
//   ble_central(kind:i 0..2, service_uuid:bytes, characteristic_uuid:bytes,
//       maximum_results:i). Scan kind 0 requires both UUID buffers empty;
//       services/characteristics use 2- or 16-byte filters (empty means wildcard).
//   usb_host(maximum_results:i)
// advertise(device, payload:bytes, interval_ms:i, cb) [16, ble_peripheral]
//
// Native forms: capability_supported(capability:i, endpoint:string) -> bool;
// metadata(callback_token, field:i[, index:i]) -> actual scalar/bytes;
// bytes([even_ascii_hex:string]), one_byte(0..255), append_byte(bytes,0..255),
// and concatenate_bytes(bytes,bytes) return NEW immutable byte buffers;
// bytes_length(bytes), byte_at(bytes,index:i), one_byte's operand, and
// integer(int64|exact_f32|strict_decimal_string) use exact int64;
// number(exactly_representable_int64|f32) returns f32.
// usb_setup(request_type:i,request:i,value:i,index:i,length:i)->bytes emits
// the standard eight-byte USB SETUP record (checked u8/u16/u16/u16/u16 fields).
// Native buffer construction is limited to the negotiated
// DeviceBudget::maximum_bytes_per_buffer (Browser currently uses the default).
//
// Metadata exists only while its exact operation/subscription callback token is
// active. Missing fields, wrong response types, indexes on scalar fields, and
// out-of-range indexes throw ScriptError. List selectors address one SDK entry
// directly and never copy a whole list. pulse_*_le64/durations_ns_le64 sequence
// conversions require their encoded size to fit maximum_bytes_per_buffer;
// oversize or overflowing conversions throw rather than truncate. BLE/USB list
// maximum_results are pre-bounded by retained-device storage and every list is
// independently bounded again by DeviceSession result budget accounting.
// UUIDs and addresses are returned in their native byte-array order.

} // namespace mb
