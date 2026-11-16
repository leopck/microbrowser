from __future__ import annotations

import os
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from tools.mbc.compiler import compile_file, compile_source
from tools.mbc.errors import CompileError, PackageError, PngError
from tools.mbc.package import DecodedNode, DecodedPackage, decode_package
from tools.mbc.png import decode_png, rgb565_asset

MBC = ROOT / "tools" / "mbc"


def source(
    page: str = '<column id="root"/>',
    *,
    states: str = "",
    style: str = "",
    before_pages: str = "",
    script: str = "",
    pages: str | None = None,
) -> bytes:
    page_markup = pages if pages is not None else f'<page name="home">{page}</page>'
    script_markup = f"<script><![CDATA[{script}]]></script>" if script else ""
    style_markup = f"<style>{style}</style>" if style else ""
    state_markup = f"<state>{states}</state>" if states else ""
    return (
        f'<microbrowser name="Test">{state_markup}{style_markup}{before_pages}'
        f'{page_markup}{script_markup}</microbrowser>'
    ).encode("utf-8")


def build(markup: bytes, path: Path | str = "test.mb") -> DecodedPackage:
    return decode_package(compile_source(markup, path))


def field(name: str, kind: str, value: str) -> str:
    return f'<field name="{name}" type="{kind}" value="{value}"/>'


def _find_kind(node: DecodedNode, kind: int) -> DecodedNode | None:
    if node.kind == kind:
        return node
    for child in node.children:
        found = _find_kind(child, kind)
        if found is not None:
            return found
    return None


def find_kind(node: DecodedNode, kind: int) -> DecodedNode:
    found = _find_kind(node, kind)
    if found is None:
        raise AssertionError(f"no node kind {kind}")
    return found


def instructions(code: bytes) -> list[tuple[int, tuple[int, ...], int]]:
    result: list[tuple[int, tuple[int, ...], int]] = []
    offset = 0
    u16_opcodes = {1, 3, 4, 5, 6}
    while offset < len(code):
        start = offset
        opcode = code[offset]
        offset += 1
        if opcode in u16_opcodes:
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
        else:
            operands = ()
        result.append((opcode, operands, start))
    if offset != len(code):
        raise AssertionError("truncated instruction in package")
    boundaries = {start for _, _, start in result}
    for opcode, operands, _ in result:
        if opcode in (20, 21):
            if operands[0] not in boundaries:
                raise AssertionError("jump does not target an instruction boundary")
    return result


def png_chunk(kind: bytes, data: bytes) -> bytes:
    crc = zlib.crc32(kind + data) & 0xFFFFFFFF
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", crc)


def make_png(width: int, height: int, color_type: int, depth: int, raw: bytes, *chunks: bytes) -> bytes:
    header = struct.pack(">IIBBBBB", width, height, depth, color_type, 0, 0, 0)
    after_header = b"".join(chunks)
    return (
        b"\x89PNG\r\n\x1a\n"
        + png_chunk(b"IHDR", header)
        + after_header
        + png_chunk(b"IDAT", zlib.compress(raw))
        + png_chunk(b"IEND", b"")
    )


def rgb_pixel(red: int, green: int, blue: int) -> int:
    return ((red & 0xF8) << 8) | ((green & 0xFC) << 3) | (blue >> 3)


class BytecodeTests(unittest.TestCase):
    def test_full_grammar_uses_version_one_instruction_layout(self) -> None:
        states = field("energy", "f32", "1") + field("message", "string", "") + field("enabled", "bool", "true")
        script = r"""
fn choose(value) {
  if (value <= 0) { return abs(value); }
  return min(value, 10);
}
fn start() {
  let number = 1;
  let ready = true;
  number = number + 3 * 2 - 1;
  state.energy = number / 2;
  ready = number != 6;
  if (ready && !false || false) {
    state.message = string(number) + " C";
  } else {
    state.message = 'other';
  }
  while (number < 12) {
    number = number + 1;
    if (number == 8) { continue; }
    if (number >= 10) { break; }
  }
  state.enabled = state.enabled && false;
  choose(number);
}
"""
        package = build(source(states=states, script=script))
        self.assertEqual([function.name for function in package.functions], ["choose", "start"])
        choose, start = package.functions
        self.assertEqual(choose.parameter_count, 1)
        self.assertEqual(choose.local_count, 1)
        self.assertIn(16, [opcode for opcode, _, _ in instructions(choose.code)])
        self.assertEqual(start.local_count, 2)
        code = instructions(start.code)
        opcodes = [opcode for opcode, _, _ in code]
        self.assertTrue({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 14, 15, 18, 19, 20, 21, 22, 23, 24}.issubset(opcodes))
        self.assertIn((23, (23, 1)), [(opcode, operands) for opcode, operands, _ in code])
        self.assertIn((22, (0, 1)), [(opcode, operands) for opcode, operands, _ in code])
        multiplication = opcodes.index(10)
        addition = opcodes.index(8)
        self.assertLess(multiplication, addition)
        starts = [offset for opcode, _, offset in code if opcode in (20, 21)]
        self.assertGreaterEqual(len(starts), 4)
        self.assertNotIn(12, opcodes)  # no unary minus in this source; multiplication remains distinct
        self.assertEqual(code[-1][0], 24)

    def test_unary_minus_comparisons_and_inequality_have_distinct_opcodes(self) -> None:
        package = build(
            source(
                script="fn start() { let x = -2; let ok = x != 0; if (x > -3) { ok = !ok; } }"
            )
        )
        opcodes = [opcode for opcode, _, _ in instructions(package.functions[0].code)]
        for expected in (12, 14, 17, 19):
            self.assertIn(expected, opcodes)

    def test_every_builtin_id_matches_the_format_table(self) -> None:
        calls = [
            "min(1, 2)",
            "max(1, 2)",
            "clamp(1, 0, 2)",
            "abs(-1)",
            "round(1.2)",
            "floor(1.2)",
            "len(\"x\")",
            "go(\"home\")",
            "back()",
            "animate(\"root\", \"opacity\", 1, 10)",
            'after(1, "start")',
            'every(1, "start")',
            "cancel(1)",
            "save_number(\"k\", 1)",
            "load_number(\"k\", 1)",
            "save_bool(\"k\", true)",
            "load_bool(\"k\", false)",
            "fetch(\"offline\", \"body\")",
            "fetch_json(\"offline\", \"v\", \"body\")",
            "save_string(\"k\", \"v\")",
            "load_string(\"k\", \"v\")",
            "sin(0)",
            "cos(0)",
            "string(1)",
            "log(\"safe\")",
            "chart_push(\"chart\", 1)",
            "json_number(\"{}\", \"v\")",
            "json_string(\"{}\", \"v\")",
            "json_bool(\"{}\", \"v\")",
        ]
        package = build(source(script="fn start() { " + "; ".join(calls) + "; }"))
        ids = [operands[0] for opcode, operands, _ in instructions(package.functions[0].code) if opcode == 23]
        self.assertEqual(ids, list(range(29)))

    def test_void_and_typed_call_errors_leave_no_ambiguous_bytecode(self) -> None:
        with self.assertRaisesRegex(CompileError, "min expects 2 arguments, found 1"):
            build(source(script="fn start() { min(1); }"))
        with self.assertRaisesRegex(CompileError, "cannot use a void value"):
            build(source(script="fn start() { let x = go('home') + 1; }"))
        with self.assertRaisesRegex(CompileError, "matching types"):
            build(source(script="fn id(x) { return x; } fn start() { id('wrong') + 1; }"))

    def test_inconsistent_returns_and_loop_control_are_rejected(self) -> None:
        with self.assertRaisesRegex(CompileError, "may reach its end"):
            build(source(script="fn value(x) { if (x) { return 1; } }"))
        with self.assertRaisesRegex(CompileError, "mixes value and void returns"):
            build(source(script="fn value(x) { if (x) { return 1; } else { return; } }"))
        with self.assertRaisesRegex(CompileError, "break outside a loop"):
            build(source(script="fn start() { break; }"))


class UiAndPackageTests(unittest.TestCase):
    def test_css_direct_attribute_and_inline_precedence(self) -> None:
        markup = source(
            '<text id="reading" class="first second" style="color: #010203; padding: 7;" text="ok" color="#040506"/>',
            style=".first { color: #101112; padding: 1; gap: 2; } .second { color: #202122; padding: 3; }",
        )
        text = find_kind(build(markup).pages[0].root, 3)
        self.assertEqual(text.properties["color"].value, 0x040506FF)
        self.assertEqual(text.properties["padding"].value, 7.0)
        self.assertEqual(text.properties["gap"].value, 2.0)
        self.assertEqual(text.properties["text"].value, "ok")

    def test_class_order_does_not_override_stylesheet_order(self) -> None:
        markup = source(
            '<text class="second first" text="ok"/>',
            style=".first { padding: 1; } .second { padding: 2; }",
        )
        self.assertEqual(find_kind(build(markup).pages[0].root, 3).properties["padding"].value, 2.0)

    def test_content_aliases_respect_css_precedence(self) -> None:
        title = field("title", "string", "Home")
        literal = build(
            source('<text class="bound" value="literal"/>', states=title, style=".bound { bind: {state.title}; }")
        ).pages[0].root
        self.assertEqual(literal.properties["text"].value, "literal")
        self.assertNotIn("bind", literal.properties)
        bound = build(
            source('<text class="literal" text="{state.title}"/>', states=title, style=".literal { text: class value; }")
        ).pages[0].root
        self.assertEqual(bound.properties["bind"].value, 0)
        self.assertNotIn("text", bound.properties)

    def test_bind_aliases_event_aliases_lengths_and_colors_encode_exactly(self) -> None:
        package = build(
            source(
                '<column id="root" width="100%" height="grow" on-load="begin()">'
                '<text value="{state.title}" format="%s"/>'
                '<slider id="target" value="{state.temperature}" min="16" max="28" step="0.5" on-change="begin()"/>'
                '<input value="{state.title}" max-length="64"/>'
                '</column>',
                states=field("temperature", "f32", "22") + field("title", "string", "Home"),
                script="fn begin() { state.temperature = state.temperature + 1; }",
            )
        )
        root = package.pages[0].root
        self.assertEqual(root.properties["on_load"], root.properties["on_load"].__class__(1, 0.0))
        self.assertEqual(root.properties["width"].value, "100%")
        self.assertEqual(root.properties["height"].value, "grow")
        text, slider, field_input = root.children
        self.assertEqual(text.properties["bind"].kind, 4)
        self.assertEqual(text.properties["bind"].value, 1)
        self.assertNotIn("value", text.properties)
        self.assertEqual(slider.properties["on_change"].kind, 1)
        self.assertEqual(slider.properties["min"].value, 16.0)
        self.assertEqual(field_input.properties["max-length"].value, 64.0)

    def test_direct_max_length_and_chart_samples_are_bounded(self) -> None:
        package = build(
            source(
                '<column><input bind="{state.title}" max-length="12"/>'
                '<chart id="history" min="0" max="40" samples="1, -2.5, 3e1" animate="pulse:250"/></column>',
                states=field("title", "string", "Home"),
            )
        )
        input_node = package.pages[0].root.children[0]
        chart = package.pages[0].root.children[1]
        self.assertEqual(input_node.properties["max-length"].value, 12.0)
        self.assertEqual(chart.properties["samples"].value, "1,-2.5,30")
        self.assertEqual(chart.properties["min"].value, 0.0)
        self.assertEqual(chart.properties["max"].value, 40.0)
        self.assertEqual(chart.properties["animate"].value, "pulse:250")

    def test_dashboard_compiles_without_hardware_or_network(self) -> None:
        package = compile_file(ROOT / "examples" / "dashboard.mb")
        decoded = decode_package(package)
        self.assertGreaterEqual(len(decoded.pages), 3)
        self.assertGreaterEqual(len(decoded.assets), 1)
        self.assertGreater(len(decoded.functions), 8)
        self.assertGreater(decoded.node_count, 40)
        builtin_ids = {
            operands[0]
            for function in decoded.functions
            for opcode, operands, _ in instructions(function.code)
            if opcode == 23
        }
        self.assertTrue({7, 8, 10, 11, 13, 14, 15, 19, 20, 24, 25}.issubset(builtin_ids))
        self.assertFalse({17, 18}.intersection(builtin_ids))
        self.assertTrue(
            {"selftest", "open_details", "open_settings", "go_back", "refresh_sensors"}.issubset(
                {function.name for function in decoded.functions}
            )
        )
        literal_strings = {
            constant.value
            for function in decoded.functions
            for constant in function.constants
            if isinstance(constant.value, str)
        }
        self.assertIn("tick", literal_strings)
        self.assertIn("refresh_sensors", literal_strings)
        self.assertNotIn("tick()", literal_strings)
        self.assertNotIn("refresh_sensors()", literal_strings)
        for page in decoded.pages:
            self.assertLessEqual(len(page.root.children), 64)

    def test_binary_reader_rejects_wrong_property_value_kind(self) -> None:
        changed = bytearray(compile_source(source('<column scroll="true"/>'), "test.mb"))
        section_count = struct.unpack_from("<I", changed, 12)[0]
        section_offset = 20
        page_payload = -1
        for _ in range(section_count):
            section_id = bytes(changed[section_offset : section_offset + 4])
            section_size = struct.unpack_from("<I", changed, section_offset + 4)[0]
            if section_id == b"PAGE":
                page_payload = section_offset + 8
                break
            section_offset += 8 + section_size
        self.assertGreaterEqual(page_payload, 0)
        self.assertEqual(changed[page_payload + 12], 2)
        changed[page_payload + 12] = 1
        changed[page_payload + 14 : page_payload + 14] = b"\x00\x00\x00"
        struct.pack_into("<I", changed, section_offset + 4, section_size + 3)
        struct.pack_into("<I", changed, 8, len(changed))
        struct.pack_into("<I", changed, 16, 0)
        struct.pack_into("<I", changed, 16, zlib.crc32(changed) & 0xFFFFFFFF)
        with self.assertRaisesRegex(PackageError, "scroll uses invalid value kind 1"):
            decode_package(bytes(changed))

    def test_crc_corruption_and_unknown_opcodes_are_detected(self) -> None:
        package = bytearray(compile_source(source(), "test.mb"))
        package[-1] ^= 1
        with self.assertRaisesRegex(PackageError, "CRC32 mismatch"):
            decode_package(bytes(package))

        changed = bytearray(compile_source(source(script="fn start() { if (true) { go('home'); } }"), "test.mb"))
        section_count = struct.unpack_from("<I", changed, 12)[0]
        section_offset = 20
        code_payload = -1
        for _ in range(section_count):
            section_id = bytes(changed[section_offset : section_offset + 4])
            section_size = struct.unpack_from("<I", changed, section_offset + 4)[0]
            if section_id == b"CODE":
                code_payload = section_offset + 8
                break
            section_offset += 8 + section_size
        self.assertGreaterEqual(code_payload, 0)
        function = code_payload + 4
        code_size = struct.unpack_from("<I", changed, function + 8)[0]
        constant_count = struct.unpack_from("<I", changed, function + 12)[0]
        code_offset = function + 16
        for _ in range(constant_count):
            tag = changed[code_offset]
            code_offset += 5 if tag in (0, 2) else 2
        changed[code_offset + code_size - 1] = 255
        struct.pack_into("<I", changed, 16, 0)
        crc = zlib.crc32(changed) & 0xFFFFFFFF
        struct.pack_into("<I", changed, 16, crc)
        with self.assertRaisesRegex(PackageError, "unknown opcode 255"):
            decode_package(bytes(changed))

    def test_cli_build_and_inspect_work_from_another_directory(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            temporary = Path(directory)
            input_path = temporary / "app.mb"
            output_path = temporary / "custom.mbp"
            input_path.write_bytes(source(script="fn start() { log('safe'); }"))
            environment = os.environ.copy()
            environment.pop("PYTHONPATH", None)
            completed = subprocess.run(
                [sys.executable, str(MBC), "build", str(input_path), "-o", str(output_path)],
                cwd=temporary,
                env=environment,
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(completed.returncode, 0, completed.stderr)
            decode_package(output_path.read_bytes())
            inspected = subprocess.run(
                [sys.executable, str(MBC), "inspect", str(output_path)],
                cwd=temporary,
                env=environment,
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(inspected.returncode, 0, inspected.stderr)
            self.assertIn("Version: 1.0", inspected.stdout)
            self.assertIn("home", inspected.stdout)
            self.assertIn("[0] start", inspected.stdout)

    def test_invalid_cli_input_has_a_precise_nonzero_diagnostic(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            input_path = Path(directory) / "bad.mb"
            input_path.write_bytes(source(script="fn start( { }"))
            completed = subprocess.run(
                [sys.executable, str(MBC), "build", str(input_path)],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertNotEqual(completed.returncode, 0)
            self.assertIn(f"{input_path}:1:", completed.stderr)
            self.assertIn("parameter name", completed.stderr)
            self.assertFalse(input_path.with_suffix(".mbp").exists())


class ValidationTests(unittest.TestCase):
    def assert_invalid(self, markup: bytes, message: str) -> None:
        with self.assertRaisesRegex(CompileError, message):
            compile_source(markup, "invalid.mb")

    def test_bad_xml_duplicate_names_and_css_syntax_are_diagnosed(self) -> None:
        self.assert_invalid(b"<microbrowser>", "no element found")
        self.assert_invalid(source('<text text="a" text="b"/>'), "duplicate attribute")
        self.assert_invalid(source(style=".card { color: #fff }"), "must end")
        self.assert_invalid(source('<text text="x"/>', style="text { color: #fff; }"), "class selector")

    def test_wrong_bindings_formats_children_and_handlers_are_rejected(self) -> None:
        number = field("temperature", "f32", "20")
        self.assert_invalid(source('<input bind="{state.temperature}"/>', states=number), "string state binding")
        self.assert_invalid(source('<progress/>', states=number), "requires one numeric")
        self.assert_invalid(source('<text text="{state.temperature}" format="%llu"/>', states=number), "unsupported format")
        self.assert_invalid(source('<text text="{state.temperature}" format="%f %f"/>', states=number), "exactly one")
        self.assert_invalid(source('<button>child</button>'), "element text")
        self.assert_invalid(source('<button text="x" on_press="run(1)"/>', script="fn run() { }"), "no-argument function call")
        self.assert_invalid(source('<button text="x" on_press="run()"/>', script="fn run(x) { }"), "must have no parameters")

    def test_script_type_and_state_errors_are_rejected(self) -> None:
        states = field("temperature", "f32", "20")
        self.assert_invalid(source(states=states, script="fn start() { state.temperature = 'hot'; }"), "expected number")
        self.assert_invalid(source(script="fn start(x, x) { }"), "duplicate parameter")
        self.assert_invalid(source(script="fn start() { let x = 1; let x = 2; }"), "duplicate local variable")
        self.assert_invalid(source(script="fn start() { unknown(); }"), "unknown function")
        self.assert_invalid(source(script="fn start() { if (1) { } }"), "expected boolean")
        self.assert_invalid(source(script="fn start() { 'x' + 1; }"), "matching types")
        self.assert_invalid(source(script="fn start() { return 1; return 'x'; }"), "function returns")

    def test_nul_in_script_strings_is_rejected(self) -> None:
        self.assert_invalid(source(script=r"fn start() { log('\x00'); }"), "cannot contain NUL")

    def test_resource_limits_and_png_input_errors_fail_fast(self) -> None:
        self.assert_invalid(source('<chart samples="1,' + ",".join(str(number) for number in range(64)) + '"/>'), "exceed 64")
        fields = "".join(field(f"n{index}", "f32", "0") for index in range(257))
        self.assert_invalid(source(states=fields), "exceeds 256")
        self.assert_invalid(source('<slider bind="{state.n}" step="0"/>', states=field("n", "f32", "1")), "greater than zero")
        self.assert_invalid(source('<text text="x" width="101%"/>'), "0 through 100")

    def test_child_cardinality_and_page_depth_limits_are_enforced(self) -> None:
        children = "<text/>" * 65
        self.assert_invalid(source(f"<column>{children}</column>"), "64 direct children")
        nested = "<column>" * 33 + "<text/>" + "</column>" * 33
        self.assert_invalid(source(nested), "32 nested elements")
        self.assert_invalid(source('<chart min="2" max="1"/>'), "chart min must be less than max")


class PngTests(unittest.TestCase):
    def test_rgb565_dimensions_and_pixels_require_no_external_decoder(self) -> None:
        raw = b"\x00" + bytes((255, 0, 0, 0, 255, 0))
        image = make_png(2, 1, 2, 8, raw)
        self.assertEqual(decode_png(image).rgba, bytes((255, 0, 0, 255, 0, 255, 0, 255)))
        asset = rgb565_asset(image)
        self.assertEqual((asset.width, asset.height), (2, 1))
        self.assertEqual(asset.pixels, struct.pack("<HH", 0xF800, 0x07E0))

    def test_nearest_neighbor_resize_uses_integer_source_coordinates(self) -> None:
        raw = b"\x00" + bytes((255, 0, 0, 0, 255, 0))
        image = make_png(2, 1, 2, 8, raw)
        asset = rgb565_asset(image, (4, 2))
        self.assertEqual((asset.width, asset.height), (4, 2))
        values = struct.unpack("<8H", asset.pixels)
        self.assertEqual(values, (0xF800, 0xF800, 0x07E0, 0x07E0) * 2)

    def test_all_png_filters_and_alpha_channel_are_decoded(self) -> None:
        rows = [bytes((255, 0, 0, 255, 0, 255, 0, 128)), bytes((0, 0, 255, 255, 255, 255, 255, 64))]
        rows = rows * 2
        filtered: list[bytes] = []
        previous = bytes(8)
        for filter_type, row in enumerate(rows, start=1):
            encoded = bytearray([filter_type])
            for index, value in enumerate(row):
                left = row[index - 4] if index >= 4 else 0
                above = previous[index]
                upper_left = previous[index - 4] if index >= 4 else 0
                if filter_type == 1:
                    predictor = left
                elif filter_type == 2:
                    predictor = above
                elif filter_type == 3:
                    predictor = (left + above) // 2
                else:
                    estimate = left + above - upper_left
                    distances = (abs(estimate - left), abs(estimate - above), abs(estimate - upper_left))
                    predictor = (left, above, upper_left)[distances.index(min(distances))]
                encoded.append((value - predictor) & 0xFF)
            filtered.append(bytes(encoded))
            previous = row
        image = make_png(2, 4, 6, 8, b"".join(filtered))
        self.assertEqual(decode_png(image).rgba, b"".join(rows))
        self.assertEqual(rgb565_asset(image).pixels[2:4], struct.pack("<H", rgb_pixel(0, 255, 0)))

    def test_palette_packed_bits_and_transparency_are_supported(self) -> None:
        palette = bytes((255, 0, 0, 0, 255, 0, 0, 0, 255))
        raw = b"\x00\x18"  # indices 0, 1, 2, 0 in two-bit samples
        image = make_png(4, 1, 3, 2, raw, png_chunk(b"PLTE", palette), png_chunk(b"tRNS", b"\x00"))
        decoded = decode_png(image)
        self.assertEqual(decoded.rgba[:8], bytes((255, 0, 0, 0, 0, 255, 0, 255)))
        self.assertEqual(decode_png(make_png(4, 1, 0, 2, raw)).rgba[0:4], bytes((0, 0, 0, 255)))

    def test_asset_data_section_contains_predecoded_little_endian_pixels(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "pixel.png").write_bytes(make_png(1, 1, 6, 8, b"\x00\x11\x22\x33\x44"))
            markup = source(
                '<image src="dot"/>',
                before_pages='<asset name="dot" src="pixel.png" resize="2x1"/>',
            )
            package = build(markup, path / "source.mb")
            asset = package.assets[0]
            self.assertEqual((asset.width, asset.height), (2, 1))
            expected = rgb_pixel(0x11, 0x22, 0x33)
            self.assertEqual(asset.pixels, struct.pack("<HH", expected, expected))

    def test_png_chunk_names_accept_only_ascii_letters(self) -> None:
        malformed = make_png(1, 1, 0, 8, b"\x00\x00", png_chunk(b"te_t", b"x"))
        with self.assertRaisesRegex(PngError, "invalid PNG chunk type"):
            decode_png(malformed)

    def test_crc_truncation_decompression_and_dimensions_are_validated(self) -> None:
        with self.assertRaisesRegex(PngError, "signature"):
            decode_png(b"not a png")
        valid = make_png(1, 1, 0, 8, b"\x00\x00")
        corrupted = bytearray(valid)
        corrupted[-1] ^= 1
        with self.assertRaisesRegex(PngError, "CRC mismatch"):
            decode_png(bytes(corrupted))
        with self.assertRaisesRegex(PngError, "decompressed size"):
            decode_png(make_png(1, 2, 0, 8, b"\x00\x00"))
        header = struct.pack(">IIBBBBB", 2049, 1, 8, 0, 0, 0, 0)
        oversized = b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", header)
        with self.assertRaisesRegex(PngError, "2048"):
            decode_png(oversized)


if __name__ == "__main__":
    unittest.main()
