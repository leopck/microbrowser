from __future__ import annotations

import re
from dataclasses import dataclass
from enum import Enum, IntEnum


class ValueType(Enum):
    UNKNOWN = 0
    NUMBER = 1
    BOOLEAN = 2
    STRING = 3
    VOID = 4
    INTEGER = 5
    BYTES = 6
    HANDLE = 7

    @property
    def label(self) -> str:
        return {
            ValueType.UNKNOWN: "unknown",
            ValueType.NUMBER: "number",
            ValueType.BOOLEAN: "boolean",
            ValueType.STRING: "string",
            ValueType.VOID: "void",
            ValueType.INTEGER: "i64",
            ValueType.BYTES: "bytes",
            ValueType.HANDLE: "handle",
        }[self]


@dataclass(frozen=True, slots=True)
class EmptyHandle:
    pass


EMPTY_HANDLE = EmptyHandle()
ScriptValue = float | bool | str | int | bytes | EmptyHandle

MAX_ENDPOINT_BYTES = 64
MAX_ENDPOINTS = 0xFFFF
ENDPOINT_RE = re.compile(r"[A-Za-z0-9_.-]+")


class DeviceOperation(IntEnum):
    OPEN = 1
    CONFIGURE = 2
    START = 3
    STOP = 4
    CLOSE = 5
    READ = 6
    WRITE = 7
    TRANSFER = 8
    SUBSCRIBE = 9
    UNSUBSCRIBE = 10
    CANCEL = 11
    NOTIFY = 12
    CONNECT = 13
    DISCONNECT = 14
    DISCOVER = 15
    ADVERTISE = 16


class NativeOperation(IntEnum):
    CAPABILITY_SUPPORTED = 200
    METADATA = 201
    BYTES = 202
    BYTES_LENGTH = 203
    BYTE_AT = 204
    INTEGER = 205
    NUMBER = 206
    ONE_BYTE = 207
    APPEND_BYTE = 208
    CONCATENATE_BYTES = 209
    USB_SETUP = 210


# Names match mb::Capability. GPIO role aliases select the same capability ID;
# configure's direction field distinguishes input from output.
CAPABILITIES_BY_NAME = {
    "gpio": 1,
    "gpio_input": 1,
    "gpio_output": 1,
    "adc": 2,
    "pwm": 3,
    "pulse_capture": 4,
    "pulse_output": 5,
    "spi_controller": 6,
    "spi_peripheral": 7,
    "i2c_controller": 8,
    "i2c_peripheral": 9,
    "uart": 10,
    "rs485": 11,
    "i2s_transmit": 12,
    "i2s_receive": 13,
    "pdm_receive": 14,
    "ble_central": 15,
    "ble_peripheral": 16,
    "event_link": 17,
    "usb_device": 18,
    "usb_host": 19,
    "spi_multilane": 20,
    "event_link_groups": 21,
    "espressif_gpio_matrix": 22,
}
CAPABILITY_NAMES = {
    1: "gpio",
    2: "adc",
    3: "pwm",
    4: "pulse_capture",
    5: "pulse_output",
    6: "spi_controller",
    7: "spi_peripheral",
    8: "i2c_controller",
    9: "i2c_peripheral",
    10: "uart",
    11: "rs485",
    12: "i2s_transmit",
    13: "i2s_receive",
    14: "pdm_receive",
    15: "ble_central",
    16: "ble_peripheral",
    17: "event_link",
    18: "usb_device",
    19: "usb_host",
    20: "spi_multilane",
    21: "event_link_groups",
    22: "espressif_gpio_matrix",
}
CAPABILITY_IDS = frozenset(CAPABILITY_NAMES)
MAX_BYTES_PER_BUFFER = 4096


@dataclass(frozen=True, slots=True)
class CapabilityDeclaration:
    capability_id: int
    source_name: str
    major: int
    minor: int
    required: bool


@dataclass(frozen=True, slots=True)
class HardwareDeclaration:
    capabilities: tuple[CapabilityDeclaration, ...]
    permission_endpoints: tuple[str, ...]


@dataclass(frozen=True, slots=True)
class Argument:
    types: tuple[ValueType, ...]
    choices: tuple[int, ...] | None = None
    minimum: int | None = None
    maximum: int | None = None

    @property
    def label(self) -> str:
        return " or ".join(value.label for value in self.types)


def _argument(
    *types: ValueType,
    choices: tuple[int, ...] | None = None,
    minimum: int | None = None,
    maximum: int | None = None,
) -> Argument:
    return Argument(types, choices, minimum, maximum)


HANDLE = _argument(ValueType.HANDLE)
INTEGER = _argument(ValueType.INTEGER)
BOOLEAN = _argument(ValueType.BOOLEAN)
STRING = _argument(ValueType.STRING)
BYTES = _argument(ValueType.BYTES)
CALLBACK = STRING
U8 = _argument(ValueType.INTEGER, minimum=0, maximum=0xFF)
U16 = _argument(ValueType.INTEGER, minimum=0, maximum=0xFFFF)
U32 = _argument(ValueType.INTEGER, minimum=0, maximum=0xFFFFFFFF)
NON_NEGATIVE = _argument(ValueType.INTEGER, minimum=0)
I2C_ADDRESS = _argument(ValueType.INTEGER, minimum=0, maximum=0x3FF)
BLE_UUID = BYTES
BLE_ADDRESS = BYTES


@dataclass(frozen=True, slots=True)
class IoPrototype:
    arguments: tuple[Argument, ...]
    callback_results: tuple[ValueType, ...] = ()
    notification_results: tuple[ValueType, ...] = ()

    def accepts_arity(self, count: int) -> bool:
        return count == len(self.arguments)


def _prototype(
    *arguments: Argument,
    callback: tuple[ValueType, ...] = (),
    notifications: tuple[ValueType, ...] = (),
) -> IoPrototype:
    return IoPrototype(tuple(arguments), callback, notifications)


@dataclass(frozen=True, slots=True)
class IoCall:
    name: str
    operation: int
    prototypes: tuple[IoPrototype, ...]
    result: ValueType

    def matching_prototypes(self, argument_count: int) -> list[IoPrototype]:
        return [
            prototype for prototype in self.prototypes if prototype.accepts_arity(argument_count)
        ]

    def accepts_arity(self, argument_count: int) -> bool:
        return any(prototype.accepts_arity(argument_count) for prototype in self.prototypes)


def _call(
    name: str,
    operation: int,
    *prototypes: IoPrototype,
    result: ValueType = ValueType.HANDLE,
) -> tuple[str, IoCall]:
    full_name = f"io.{name}"
    return full_name, IoCall(full_name, int(operation), tuple(prototypes), result)


IO_CALLS = dict(
    (
        _call(
            "capability_supported",
            NativeOperation.CAPABILITY_SUPPORTED,
            _prototype(INTEGER, STRING),
            result=ValueType.BOOLEAN,
        ),
        _call(
            "metadata",
            NativeOperation.METADATA,
            _prototype(HANDLE, INTEGER),
            _prototype(HANDLE, INTEGER, U32),
            result=ValueType.UNKNOWN,
        ),
        _call(
            "bytes",
            NativeOperation.BYTES,
            _prototype(),
            _prototype(STRING),
            result=ValueType.BYTES,
        ),
        _call(
            "bytes_length",
            NativeOperation.BYTES_LENGTH,
            _prototype(BYTES),
            result=ValueType.INTEGER,
        ),
        _call(
            "byte_at",
            NativeOperation.BYTE_AT,
            _prototype(BYTES, NON_NEGATIVE),
            result=ValueType.INTEGER,
        ),
        _call(
            "integer",
            NativeOperation.INTEGER,
            _prototype(_argument(ValueType.INTEGER, ValueType.NUMBER, ValueType.STRING)),
            result=ValueType.INTEGER,
        ),
        _call(
            "number",
            NativeOperation.NUMBER,
            _prototype(_argument(ValueType.INTEGER, ValueType.NUMBER)),
            result=ValueType.NUMBER,
        ),
        _call(
            "one_byte",
            NativeOperation.ONE_BYTE,
            _prototype(U8),
            result=ValueType.BYTES,
        ),
        _call(
            "append_byte",
            NativeOperation.APPEND_BYTE,
            _prototype(BYTES, U8),
            result=ValueType.BYTES,
        ),
        _call(
            "concatenate_bytes",
            NativeOperation.CONCATENATE_BYTES,
            _prototype(BYTES, BYTES),
            result=ValueType.BYTES,
        ),
        _call(
            "usb_setup",
            NativeOperation.USB_SETUP,
            _prototype(U8, U8, U16, U16, U16),
            result=ValueType.BYTES,
        ),
        _call(
            "open",
            DeviceOperation.OPEN,
            _prototype(INTEGER, STRING, U16, CALLBACK, callback=(ValueType.HANDLE,)),
        ),
        _call(
            "configure",
            DeviceOperation.CONFIGURE,
            # GPIO: direction 0=input/1=output, pull 0=none/1=up/2=down.
            _prototype(
                HANDLE,
                _argument(ValueType.INTEGER, choices=(0, 1)),
                _argument(ValueType.INTEGER, choices=(0, 1, 2)),
                BOOLEAN,
                CALLBACK,
                callback=(ValueType.VOID,),
            ),
            # ADC and PWM use their two request fields in declaration order.
            _prototype(HANDLE, NON_NEGATIVE, NON_NEGATIVE, CALLBACK, callback=(ValueType.INTEGER,)),
            _prototype(
                HANDLE,
                U32,
                _argument(ValueType.INTEGER, choices=(0, 1, 2, 3)),
                _argument(ValueType.INTEGER, choices=(0, 1)),
                _argument(ValueType.INTEGER, choices=(1, 2, 4)),
                CALLBACK,
                callback=(ValueType.INTEGER,),
            ),
            _prototype(HANDLE, U32, CALLBACK, callback=(ValueType.INTEGER,)),
            _prototype(
                HANDLE,
                U32,
                U8,
                _argument(ValueType.INTEGER, choices=(1, 2)),
                _argument(ValueType.INTEGER, choices=(0, 1, 2)),
                _argument(ValueType.INTEGER, choices=(0, 1)),
                _argument(ValueType.INTEGER, choices=(0, 1)),
                CALLBACK,
                callback=(ValueType.INTEGER,),
            ),
            _prototype(
                HANDLE,
                NON_NEGATIVE,
                _argument(ValueType.INTEGER, minimum=0),
                CALLBACK,
                callback=(ValueType.INTEGER,),
            ),
            _prototype(
                HANDLE,
                U32,
                U8,
                _argument(ValueType.INTEGER, choices=(0, 1, 2, 3)),
                U32,
                CALLBACK,
                callback=(ValueType.INTEGER,),
            ),
            # USB interface tables have bounded binary records, not VM argument lists.
            _prototype(HANDLE, BYTES, BYTES, CALLBACK, callback=(ValueType.INTEGER,)),
        ),
        _call(
            "start", DeviceOperation.START, _prototype(HANDLE, CALLBACK, callback=(ValueType.VOID,))
        ),
        _call(
            "stop", DeviceOperation.STOP, _prototype(HANDLE, CALLBACK, callback=(ValueType.VOID,))
        ),
        _call(
            "close", DeviceOperation.CLOSE, _prototype(HANDLE, CALLBACK, callback=(ValueType.VOID,))
        ),
        _call(
            "read",
            DeviceOperation.READ,
            # GPIO/event link return boolean; ADC returns its sample integer.
            _prototype(
                HANDLE,
                CALLBACK,
                callback=(ValueType.BOOLEAN, ValueType.INTEGER),
            ),
            # UART/BLE use void for an absent payload; integer fallbacks are counts.
            _prototype(
                HANDLE,
                U32,
                CALLBACK,
                callback=(ValueType.BYTES, ValueType.INTEGER),
            ),
            _prototype(
                HANDLE,
                I2C_ADDRESS,
                BOOLEAN,
                U32,
                CALLBACK,
                callback=(ValueType.BYTES, ValueType.INTEGER),
            ),
            _prototype(HANDLE, BLE_UUID, BLE_UUID, CALLBACK, callback=(ValueType.BYTES,)),
            _prototype(
                HANDLE,
                U8,
                U32,
                U32,
                CALLBACK,
                callback=(ValueType.BYTES, ValueType.INTEGER),
            ),
        ),
        _call(
            "write",
            DeviceOperation.WRITE,
            _prototype(HANDLE, BOOLEAN, CALLBACK, callback=(ValueType.VOID,)),
            _prototype(HANDLE, NON_NEGATIVE, CALLBACK, callback=(ValueType.VOID,)),
            # Count-producing byte writes, and SPI-peripheral's empty response.
            _prototype(HANDLE, BYTES, CALLBACK, callback=(ValueType.INTEGER,)),
            _prototype(HANDLE, BYTES, CALLBACK, callback=(ValueType.VOID,)),
            _prototype(
                HANDLE,
                I2C_ADDRESS,
                BOOLEAN,
                BYTES,
                CALLBACK,
                callback=(ValueType.INTEGER,),
            ),
            _prototype(
                HANDLE,
                U8,
                BYTES,
                U32,
                CALLBACK,
                callback=(ValueType.INTEGER,),
            ),
            _prototype(
                HANDLE,
                BLE_UUID,
                BLE_UUID,
                _argument(ValueType.INTEGER, choices=(0, 1)),
                BYTES,
                CALLBACK,
                callback=(ValueType.INTEGER,),
            ),
        ),
        _call(
            "transfer",
            DeviceOperation.TRANSFER,
            # An absent receive buffer yields the completed count instead.
            _prototype(
                HANDLE,
                BYTES,
                U32,
                CALLBACK,
                callback=(ValueType.BYTES, ValueType.INTEGER),
            ),
            _prototype(
                HANDLE,
                I2C_ADDRESS,
                BOOLEAN,
                BYTES,
                U32,
                BOOLEAN,
                CALLBACK,
                callback=(ValueType.BYTES, ValueType.INTEGER),
            ),
            _prototype(
                HANDLE,
                BYTES,
                BYTES,
                U16,
                U32,
                CALLBACK,
                callback=(ValueType.BYTES, ValueType.INTEGER),
            ),
        ),
        _call(
            "subscribe",
            DeviceOperation.SUBSCRIBE,
            _prototype(
                HANDLE,
                _argument(ValueType.INTEGER, choices=(0, 1, 2)),
                CALLBACK,
                CALLBACK,
                callback=(ValueType.HANDLE,),
                notifications=(ValueType.BOOLEAN,),
            ),
            _prototype(
                HANDLE,
                _argument(ValueType.INTEGER, choices=(0, 1)),
                BLE_UUID,
                BLE_UUID,
                CALLBACK,
                CALLBACK,
                callback=(ValueType.HANDLE,),
                notifications=(ValueType.BOOLEAN, ValueType.BYTES),
            ),
        ),
        _call(
            "unsubscribe",
            DeviceOperation.UNSUBSCRIBE,
            _prototype(HANDLE, HANDLE, CALLBACK, callback=(ValueType.VOID,)),
        ),
        _call(
            "cancel",
            DeviceOperation.CANCEL,
            _prototype(HANDLE),
            result=ValueType.VOID,
        ),
        _call(
            "notify",
            DeviceOperation.NOTIFY,
            _prototype(
                HANDLE,
                BLE_UUID,
                BLE_UUID,
                BYTES,
                CALLBACK,
                callback=(ValueType.VOID,),
            ),
        ),
        _call(
            "connect",
            DeviceOperation.CONNECT,
            _prototype(
                HANDLE,
                BLE_ADDRESS,
                BOOLEAN,
                U32,
                CALLBACK,
                callback=(ValueType.BOOLEAN,),
            ),
            _prototype(HANDLE, U8, CALLBACK, callback=(ValueType.VOID,)),
        ),
        _call(
            "disconnect",
            DeviceOperation.DISCONNECT,
            _prototype(HANDLE, BLE_ADDRESS, BOOLEAN, CALLBACK, callback=(ValueType.VOID,)),
            _prototype(HANDLE, U8, CALLBACK, callback=(ValueType.VOID,)),
        ),
        _call(
            "discover",
            DeviceOperation.DISCOVER,
            # Empty UUID buffers mean no filter. discovery kind is 0=scan,
            # 1=services, 2=characteristics. The primary result is entry count.
            _prototype(
                HANDLE,
                _argument(ValueType.INTEGER, choices=(0, 1, 2)),
                BLE_UUID,
                BLE_UUID,
                U32,
                CALLBACK,
                callback=(ValueType.INTEGER,),
            ),
            _prototype(HANDLE, U32, CALLBACK, callback=(ValueType.INTEGER,)),
        ),
        _call(
            "advertise",
            DeviceOperation.ADVERTISE,
            _prototype(HANDLE, BYTES, U32, CALLBACK, callback=(ValueType.VOID,)),
        ),
    )
)
IO_BY_ID = {call.operation: call for call in IO_CALLS.values()}


@dataclass(frozen=True, slots=True)
class MetadataField:
    id: int
    name: str
    result: ValueType
    indexed: bool = False


def _metadata_fields() -> dict[int, MetadataField]:
    # Keep this table in the same order as DeviceMetadataField in device_api.h.
    definitions = (
        (1, "io_status", ValueType.INTEGER, False),
        (2, "timestamp_ns", ValueType.INTEGER, False),
        (3, "sequence", ValueType.INTEGER, False),
        (4, "modeled", ValueType.BOOLEAN, False),
        (5, "capability_major", ValueType.INTEGER, False),
        (6, "capability_minor", ValueType.INTEGER, False),
        (7, "actual_sample_time_ns", ValueType.INTEGER, False),
        (8, "actual_reference_microvolts", ValueType.INTEGER, False),
        (9, "reference_microvolts", ValueType.INTEGER, False),
        (10, "resolution_bits", ValueType.INTEGER, False),
        (11, "calibrated", ValueType.BOOLEAN, False),
        (12, "actual_period_ns", ValueType.INTEGER, False),
        (13, "actual_frequency_hz", ValueType.INTEGER, False),
        (14, "actual_baud", ValueType.INTEGER, False),
        (15, "actual_minimum_duration_ns", ValueType.INTEGER, False),
        (16, "actual_resolution_ns", ValueType.INTEGER, False),
        (17, "frame_rate_hz", ValueType.INTEGER, False),
        (18, "channel_count", ValueType.INTEGER, False),
        (19, "pcm_sample_format", ValueType.INTEGER, False),
        (20, "actual_configuration", ValueType.INTEGER, False),
        (21, "sample", ValueType.INTEGER, False),
        (22, "received", ValueType.BYTES, False),
        (23, "bytes_transferred", ValueType.INTEGER, False),
        (24, "bytes_written", ValueType.INTEGER, False),
        (25, "i2c_fault", ValueType.INTEGER, False),
        (26, "repeated_start", ValueType.BOOLEAN, False),
        (27, "uart_bytes", ValueType.BYTES, False),
        (28, "uart_error_overflow", ValueType.BOOLEAN, False),
        (29, "uart_error_framing", ValueType.BOOLEAN, False),
        (30, "uart_error_parity", ValueType.BOOLEAN, False),
        (31, "uart_error_break_detected", ValueType.BOOLEAN, False),
        (32, "pulse_count", ValueType.INTEGER, False),
        (33, "pulse_samples_le64", ValueType.BYTES, False),
        (34, "durations_ns_le64", ValueType.BYTES, False),
        (35, "pulse_timestamps_ns_le64", ValueType.BYTES, False),
        (36, "transitions_written", ValueType.INTEGER, False),
        (37, "pcm_frames", ValueType.BYTES, False),
        (38, "frame_count", ValueType.INTEGER, False),
        (39, "frames_consumed", ValueType.INTEGER, False),
        (40, "peer_count", ValueType.INTEGER, False),
        (41, "peer_address", ValueType.BYTES, True),
        (42, "peer_address_random", ValueType.BOOLEAN, True),
        (43, "received_signal_strength_dbm", ValueType.INTEGER, True),
        (44, "advertisement", ValueType.BYTES, True),
        (45, "characteristic_count", ValueType.INTEGER, False),
        (46, "service_uuid", ValueType.BYTES, True),
        (47, "characteristic_uuid", ValueType.BYTES, True),
        (48, "readable", ValueType.BOOLEAN, True),
        (49, "writable", ValueType.BOOLEAN, True),
        (50, "notifiable", ValueType.BOOLEAN, True),
        (51, "connected", ValueType.BOOLEAN, False),
        (52, "connected_peer_address", ValueType.BYTES, False),
        (53, "connected_peer_address_random", ValueType.BOOLEAN, False),
        (54, "paired", ValueType.BOOLEAN, False),
        (55, "attribute_service_uuid", ValueType.BYTES, False),
        (56, "attribute_characteristic_uuid", ValueType.BYTES, False),
        (57, "ble_value", ValueType.BYTES, False),
        (58, "enabled", ValueType.BOOLEAN, False),
        (59, "hardware_autonomous", ValueType.BOOLEAN, False),
        (60, "trigger_count", ValueType.INTEGER, False),
        (61, "usb_device_count", ValueType.INTEGER, False),
        (62, "usb_device_address", ValueType.INTEGER, True),
        (63, "usb_vendor_id", ValueType.INTEGER, True),
        (64, "usb_product_id", ValueType.INTEGER, True),
        (65, "usb_device_descriptor", ValueType.BYTES, True),
    )
    return {
        field_id: MetadataField(field_id, name, result, indexed)
        for field_id, name, result, indexed in definitions
    }


METADATA_FIELDS = _metadata_fields()
