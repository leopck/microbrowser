from __future__ import annotations

import struct
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from tools.mbc.compiler import compile_source
from tools.mbc.device_api import EMPTY_HANDLE, ValueType
from tools.mbc.errors import CompileError
from tools.mbc.package import DecodedPackage, decode_package


def source(*, hardware: str = "", states: str = "", script: str = "", on_load: str = "") -> bytes:
    page_attribute = f' on_load="{on_load}"' if on_load else ""
    state_markup = f"<state>{states}</state>" if states else ""
    hardware_markup = f"<hardware>{hardware}</hardware>" if hardware else ""
    script_markup = f"<script><![CDATA[{script}]]></script>" if script else ""
    return (
        f'<microbrowser name="Hardware test">{state_markup}{hardware_markup}'
        f'<page name="home"><column id="root"{page_attribute}/></page>'
        f"{script_markup}</microbrowser>"
    ).encode("utf-8")


def field(name: str, kind: str, value: str) -> str:
    return f'<field name="{name}" type="{kind}" value="{value}"/>'


def build(markup: bytes) -> tuple[bytes, DecodedPackage]:
    encoded = compile_source(markup, "hardware-test.mb")
    return encoded, decode_package(encoded)


def raw_sections(data: bytes) -> list[tuple[bytes, bytes]]:
    major, minor, _, count, _ = struct.unpack_from("<HHIII", data, 4)
    if (major, minor) not in ((2, 0), (3, 0)):
        raise AssertionError("unexpected package version")
    offset = 20
    result: list[tuple[bytes, bytes]] = []
    for _ in range(count):
        if offset + 8 > len(data):
            raise AssertionError("truncated section header")
        name = data[offset : offset + 4]
        size = struct.unpack_from("<I", data, offset + 4)[0]
        begin = offset + 8
        end = begin + size
        result.append((name, data[begin:end]))
        offset = end
    if offset != len(data):
        raise AssertionError("unparsed package bytes")
    return result


def section(data: bytes, name: bytes) -> bytes:
    matches = [payload for section_name, payload in raw_sections(data) if section_name == name]
    if len(matches) != 1:
        raise AssertionError(f"expected one {name!r} section")
    return matches[0]


def instructions(code: bytes) -> list[tuple[int, tuple[int, ...], int]]:
    result: list[tuple[int, tuple[int, ...], int]] = []
    offset = 0
    while offset < len(code):
        start = offset
        opcode = code[offset]
        offset += 1
        if opcode in (1, 3, 4, 5, 6):
            operands = (struct.unpack_from("<H", code, offset)[0],)
            offset += 2
        elif opcode == 2:
            operands = (code[offset],)
            offset += 1
        elif opcode in (20, 21):
            relative = struct.unpack_from("<h", code, offset)[0]
            offset += 2
            operands = (offset + relative,)
        elif opcode == 22:
            operands = (struct.unpack_from("<H", code, offset)[0], code[offset + 2])
            offset += 3
        elif opcode == 23:
            operands = (code[offset], code[offset + 1])
            offset += 2
        elif opcode == 25:
            operands = (struct.unpack_from("<H", code, offset)[0], code[offset + 2])
            offset += 3
        else:
            operands = ()
        result.append((opcode, operands, start))
    if offset != len(code):
        raise AssertionError("truncated instruction")
    return result


def device_calls(code: bytes) -> list[tuple[int, int]]:
    return [operands for opcode, operands, _ in instructions(code) if opcode == 25]


GPIO_MANIFEST = '<require capability="gpio_output" version="1.0"/><permission endpoint="status_led"/>'


class HardwareCompilerTests(unittest.TestCase):
    def assert_invalid(self, markup: bytes, message: str) -> None:
        with self.assertRaisesRegex(CompileError, message):
            compile_source(markup, "hardware-test.mb")

    def test_gpio_workflow_writes_caps_perm_and_opcode_25(self) -> None:
        states = field("device", "handle", "") + field("last_status", "i64", "0")
        script = r"""
fn opened(operation, status, device) {
  state.device = device;
  state.last_status = status;
  io.configure(device, 1i64, 0i64, false, "configured");
}
fn configured(operation, status, result) {
  io.write(state.device, true, "wrote_high");
}
fn wrote_high(operation, status, result) {
  io.write(state.device, false, "wrote_low");
}
fn wrote_low(operation, status, result) {
  state.last_status = status;
}
fn start() {
  io.open(1i64, "status_led", 0i64, "opened");
}
"""
        encoded, package = build(source(hardware=GPIO_MANIFEST, states=states, script=script, on_load="start()"))
        self.assertEqual(package.major, 3)
        self.assertEqual(section(encoded, b"CAPS"), struct.pack("<HHHHH", 1, 1, 1, 0, 1))
        self.assertEqual(
            section(encoded, b"PERM"),
            struct.pack("<HH", 1, len(b"status_led")) + b"status_led",
        )
        self.assertEqual(
            [(item.capability_id, item.major, item.minor, item.required) for item in package.capabilities],
            [(1, 1, 0, True)],
        )
        self.assertEqual(package.states[0].type, ValueType.HANDLE)
        self.assertEqual(package.states[1].type, ValueType.INTEGER)
        functions = {function.name: function for function in package.functions}
        self.assertEqual(device_calls(functions["start"].code), [(1, 4)])
        self.assertEqual(device_calls(functions["opened"].code), [(2, 5)])
        self.assertEqual(device_calls(functions["configured"].code), [(7, 3)])
        self.assertEqual(device_calls(functions["wrote_high"].code), [(7, 3)])
        for name in ("opened", "configured", "wrote_high", "wrote_low"):
            self.assertEqual(functions[name].parameter_count, 3)
        integer_constants = [constant.value for constant in functions["opened"].constants if constant.type == ValueType.INTEGER]
        self.assertEqual(integer_constants[:2], [1, 0])
        high_arguments = [operands[0] for opcode, operands, _ in instructions(functions["configured"].code) if opcode == 2]
        low_arguments = [operands[0] for opcode, operands, _ in instructions(functions["wrote_high"].code) if opcode == 2]
        self.assertEqual(high_arguments, [1])
        self.assertEqual(low_arguments, [0])

    def test_typed_values_round_trip_without_losing_i64_spelling(self) -> None:
        states = "".join(
            (
                field("minimum", "i64", "-9223372036854775808"),
                field("precise", "i64", "9007199254740993"),
                field("blob", "bytes", "00fF"),
                field("device", "handle", ""),
            )
        )
        script = "fn limits() { let low = -9223372036854775808i64; let exact = 9007199254740993i64; }"
        encoded, package = build(source(states=states, script=script))
        self.assertEqual(package.major, 3)
        self.assertEqual(package.states[0].value, -(1 << 63))
        self.assertEqual(package.states[1].value, 9007199254740993)
        self.assertEqual(package.states[2].value, b"\x00\xff")
        self.assertEqual(package.states[3].value, EMPTY_HANDLE)
        self.assertEqual(
            [constant.value for constant in package.functions[0].constants],
            [-(1 << 63), 9007199254740993],
        )
        payload = section(encoded, b"STAT")
        self.assertEqual(struct.unpack_from("<I", payload)[0], 4)
        offset = 4
        offset += 4
        self.assertEqual(payload[offset], 3)
        self.assertEqual(struct.unpack_from("<q", payload, offset + 1)[0], -(1 << 63))
        offset += 9
        offset += 4
        self.assertEqual(payload[offset], 3)
        self.assertEqual(struct.unpack_from("<q", payload, offset + 1)[0], 9007199254740993)
        offset += 9
        offset += 4
        self.assertEqual(payload[offset : offset + 7], b"\x04\x02\x00\x00\x00\x00\xff")
        offset += 7
        offset += 4
        self.assertEqual(payload[offset : offset + 3], b"\x05\x00\x00")
        self.assertEqual(offset + 3, len(payload))

    def test_native_value_calls_and_metadata_have_typed_results(self) -> None:
        states = field("timestamp", "i64", "0") + field("size", "i64", "0")
        script = r"""
fn inspect(operation, status, result) {
  state.timestamp = io.metadata(operation, 2i64);
  state.size = io.bytes_length(io.metadata(operation, 41i64, 0i64));
}
fn start() {
  io.bytes();
  io.bytes("00fF");
  io.bytes_length(io.bytes("0102"));
  io.byte_at(io.bytes("0102"), 1i64);
  io.integer("9007199254740993");
  io.number(16777216i64);
  io.one_byte(255i64);
  io.append_byte(io.bytes("00"), 1i64);
  io.concatenate_bytes(io.bytes("00"), io.bytes("ff"));
  io.usb_setup(0i64, 1i64, 2i64, 3i64, 4i64);
}
"""
        _, package = build(source(states=states, script=script))
        functions = {function.name: function for function in package.functions}
        self.assertEqual(device_calls(functions["inspect"].code), [(201, 2), (201, 3), (203, 1)])
        self.assertEqual(
            device_calls(functions["start"].code),
            [
                (202, 0),
                (202, 1),
                (202, 1),
                (203, 1),
                (202, 1),
                (204, 2),
                (205, 1),
                (206, 1),
                (207, 1),
                (202, 1),
                (208, 2),
                (202, 1),
                (202, 1),
                (209, 2),
                (210, 5),
            ],
        )

    def test_subscription_has_separate_ready_and_notification_callbacks(self) -> None:
        script = r"""
fn ready(operation, status, subscription) {}
fn changed(subscription, status, high) {}
fn start(device) {
  io.subscribe(device, 0i64, "ready", "changed");
}
"""
        _, package = build(source(script=script))
        functions = {function.name: function for function in package.functions}
        self.assertEqual(device_calls(functions["start"].code), [(9, 4)])
        self.assertEqual(functions["ready"].parameter_count, 3)
        self.assertEqual(functions["changed"].parameter_count, 3)

    def test_usb_uses_bounded_bytes_and_fixed_arity(self) -> None:
        script = r"""
fn done(operation, status, result) {}
fn start(device) {
  io.configure(device, io.bytes(""), io.bytes(""), "done");
  io.transfer(device, io.usb_setup(0i64, 1i64, 2i64, 3i64, 4i64), io.bytes("00"), 4i64, 100i64, "done");
}
"""
        _, package = build(source(script=script))
        self.assertEqual(
            device_calls(package.functions[1].code),
            [(202, 1), (202, 1), (2, 4), (210, 5), (202, 1), (8, 6)],
        )

    def test_exact_integer_and_bytes_failures_are_compile_errors(self) -> None:
        for literal in ("9223372036854775808i64", "-9223372036854775809i64"):
            self.assert_invalid(source(script=f"fn start() {{ let value = {literal}; }}"), "signed 64-bit")
        self.assert_invalid(source(script="fn start() { io.number(16777217i64); }"), "exactly as f32")
        self.assert_invalid(source(script='fn start() { io.bytes("0"); }'), "even length")
        self.assert_invalid(source(script='fn start() { io.bytes("0g"); }'), "ASCII hexadecimal")
        self.assert_invalid(source(script="fn start() { io.one_byte(256i64); }"), "outside 0..255")
        self.assert_invalid(source(script="fn start() { io.usb_setup(256i64, 0i64, 0i64, 0i64, 0i64); }"), "outside 0..255")

    def test_callback_hardware_and_call_form_failures(self) -> None:
        callback = "fn done(operation, status, result) {} "
        self.assert_invalid(
            source(script=callback + 'fn start() { io.open(1i64, "status_led", 0i64, "done"); }'),
            "capability 'gpio' is not declared",
        )
        self.assert_invalid(
            source(
                hardware='<require capability="gpio_output" version="1.0"/>',
                script=callback + 'fn start() { io.open(1i64, "status_led", 0i64, "done"); }',
            ),
            "hardware endpoint 'status_led' is not declared",
        )
        self.assert_invalid(
            source(hardware=GPIO_MANIFEST, script="fn done(operation, status) {} fn start() { io.open(1i64, \"status_led\", 0i64, \"done\"); }"),
            "callback 'done' expects 3 arguments, found 2",
        )
        self.assert_invalid(
            source(hardware=GPIO_MANIFEST, script=callback + 'fn start() { io.open(1i64, "status_led", 0i64, "missing"); }'),
            "unknown callback function 'missing'",
        )
        self.assert_invalid(source(script="fn start() { missing(); }"), "unknown function 'missing'")
        self.assert_invalid(source(script="fn start() { io.undefined(); }"), "unknown io operation 'undefined'")
        self.assert_invalid(
            source(script="fn inspect(operation, status, result) { io.metadata(operation, 41i64); }"),
            "peer_address requires an entry index",
        )
        self.assert_invalid(
            source(script="fn inspect(operation, status, result) { io.metadata(operation, 999i64); }"),
            "unknown metadata field ID",
        )

    def test_manifest_records_are_sorted_and_validated(self) -> None:
        hardware = (
            '<optional capability="pwm" version="1.2"/>'
            '<permission endpoint="zeta"/><require capability="gpio_output" version="1.0"/>'
            '<permission endpoint="alpha"/>'
        )
        encoded, package = build(source(hardware=hardware))
        self.assertEqual(
            section(encoded, b"CAPS"),
            struct.pack("<H", 2)
            + struct.pack("<HHHH", 1, 1, 0, 1)
            + struct.pack("<HHHH", 3, 1, 2, 0),
        )
        self.assertEqual(package.permission_endpoints, ("alpha", "zeta"))
        invalid_manifests = (
            ('<require capability="adc" version="1.0"/><optional capability="adc" version="1.0"/>', "duplicate or conflicting"),
            ('<require capability="gpio_input" version="1.0"/><optional capability="gpio_output" version="1.0"/>', "duplicate or conflicting"),
            ('<require capability="adc" version="2.0"/>', "major 1"),
            ('<require capability="adc" version="1.65536"/>', "minor 0 through 65535"),
            ('<require capability="-1" version="1.0"/>', "must not be negative"),
            ('<require capability="not_hardware" version="1.0"/>', "unrecognized capability family"),
            ('<permission endpoint=""/>', "permission endpoint"),
            ('<permission endpoint="bad/name"/>', "permission endpoint"),
            ('<permission endpoint="bad space"/>', "permission endpoint"),
        )
        for declarations, message in invalid_manifests:
            with self.subTest(declarations=declarations):
                self.assert_invalid(source(hardware=declarations), message)

    def test_legacy_source_keeps_version_two_layout(self) -> None:
        markup = source(
            states=field("value", "f32", "1"),
            script="fn start() { state.value = state.value + 1; }",
            on_load="start()",
        )
        first, package = build(markup)
        second, _ = build(markup)
        self.assertEqual(first, second)
        self.assertEqual(package.major, 2)
        self.assertEqual(package.capabilities, ())
        self.assertEqual(package.permission_endpoints, ())
        self.assertEqual([name for name, _ in raw_sections(first)], [b"STRS", b"STAT", b"CODE", b"PAGE"])
        self.assertFalse(any(opcode == 25 for opcode, _, _ in instructions(package.functions[0].code)))


if __name__ == "__main__":
    unittest.main()
