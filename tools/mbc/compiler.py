from __future__ import annotations

import math
import re
import struct
import zlib
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path

# Imported for package self-validation; keep this module usable independently of cli.py.
from .errors import CompileError, PngError
from .package import FONT_SIZES, FONT_WEIGHTS, MAX_PACKAGE_BYTES, VERSION_MAJOR, VERSION_MINOR, decode_package
from .png import rgb565_asset
from .script import Constant, EncodedFunction, ValueType, compile_script
from .xml_source import XmlElement, parse_xml_bytes, parse_xml_file


NODE_KINDS = {
    "view": 0,
    "row": 1,
    "column": 2,
    "text": 3,
    "button": 4,
    "input": 5,
    "slider": 6,
    "progress": 7,
    "chart": 8,
    "image": 9,
    "spacer": 10,
    "stack": 11,
}
CONTAINERS = {"view", "row", "column", "stack"}
COMMON_PROPERTIES = {
    "id",
    "width",
    "height",
    "padding",
    "gap",
    "font-size",
    "font-weight",
    "radius",
    "background",
    "color",
    "border",
    "align",
    "justify",
    "x",
    "y",
    "position",
    "scroll",
    "animate",
}
NODE_PROPERTIES = {
    "view": set(),
    "row": set(),
    "column": set(),
    "stack": set(),
    "text": {"text", "value", "bind", "format"},
    "button": {"text", "on_press"},
    "input": {"bind", "value", "placeholder", "max-length", "on_change", "on_submit"},
    "slider": {"bind", "value", "min", "max", "step", "on_change"},
    "progress": {"bind", "value", "min", "max"},
    "chart": {"samples", "min", "max"},
    "image": {"src"},
    "spacer": set(),
}
EVENTS = {"on_press", "on_change", "on_submit", "on_load"}
EVENT_FUNCTIONS = {"on-press", "on-change", "on-submit", "on-load"}
PALETTE = {
    "transparent": 0x00000000,
    "black": 0x000000FF,
    "white": 0xFFFFFFFF,
    "ink": 0x1F293BFF,
    "muted": 0x68758AFF,
    "background": 0xF3F5FAFF,
    "surface": 0xFFFFFFFF,
    "border": 0xDCE3EDFF,
    "accent": 0x12826FFF,
    "blue": 0x4263EBFF,
    "warning": 0xD98924FF,
    "error": 0xBC2D3EFF,
}
IDENTIFIER = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
CSS_NAME = re.compile(r"[A-Za-z_][A-Za-z0-9_-]*")
DECIMAL = re.compile(r"[+-]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?")
BINDING = re.compile(r"\{state\.([A-Za-z_][A-Za-z0-9_]*)\}")
HANDLER = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)\s*\(\s*\)")
INTEGER = re.compile(r"[+-]?[0-9]+")
FORMAT_CONVERSION = re.compile(r"%(?:\.[0-6]f|[fds])")
MAX_ELEMENT_ID_BYTES = 128
MAX_FIXED_PIXELS = 16_384
INTEGER_PROPERTY_BOUNDS = {
    "padding": (0, 256),
    "gap": (0, 256),
    "radius": (0, 256),
    "x": (-MAX_FIXED_PIXELS, MAX_FIXED_PIXELS),
    "y": (-MAX_FIXED_PIXELS, MAX_FIXED_PIXELS),
    "max-length": (0, 65_535),
}



@dataclass(frozen=True, slots=True)
class StateDef:
    name: str
    type: ValueType
    value: float | bool | str


@dataclass(frozen=True, slots=True)
class AssetDef:
    name: str
    source: str
    resize: tuple[int, int] | None
    element: XmlElement


@dataclass(frozen=True, slots=True)
class Document:
    name: str
    states: list[StateDef]
    style: XmlElement | None
    assets: list[AssetDef]
    pages: list[XmlElement]
    script: XmlElement | None
    path: str


@dataclass(frozen=True, slots=True)
class Rule:
    class_name: str
    properties: dict[str, str]


@dataclass(frozen=True, slots=True)
class EncodedProperty:
    name: str
    kind: int
    value: str | float | bool | int


@dataclass(frozen=True, slots=True)
class EncodedNode:
    kind: int
    properties: list[EncodedProperty]
    children: list[EncodedNode]


@dataclass(frozen=True, slots=True)
class EncodedAsset:
    name: str
    width: int
    height: int
    pixels: bytes


class _StringTable:
    def __init__(self) -> None:
        self.strings: list[str] = []
        self.indices: dict[str, int] = {}

    def intern(self, value: str) -> int:
        existing = self.indices.get(value)
        if existing is not None:
            return existing
        if len(self.strings) == 0xFFFFFFFF:
            raise ValueError("package has too many strings")
        index = len(self.strings)
        self.strings.append(value)
        self.indices[value] = index
        return index

    def encode(self) -> bytes:
        offsets: list[int] = []
        strings = bytearray()
        for value in self.strings:
            offsets.append(len(strings))
            strings.extend(value.encode("utf-8"))
            strings.append(0)
        result = bytearray(struct.pack("<I", len(self.strings)))
        for offset in offsets:
            result.extend(struct.pack("<I", offset))
        result.extend(strings)
        return bytes(result)


def _allowed_attributes(element: XmlElement, allowed: set[str]) -> None:
    for name in element.attributes:
        if name not in allowed:
            raise element.error(f"unknown attribute {name!r} on <{element.name}>")


def _no_nested_text(element: XmlElement) -> None:
    if element.children:
        raise element.children[0].error(f"<{element.name}> cannot contain elements")
    if element.text.strip():
        raise element.error(f"<{element.name}> contains unexpected character data")

def _no_child_elements(element: XmlElement) -> None:
    if element.children:
        raise element.children[0].error(f"<{element.name}> cannot contain elements")


def _number(raw: str, element: XmlElement, property_name: str) -> float:
    value_text = raw.strip()
    if not DECIMAL.fullmatch(value_text):
        raise element.error(f"{property_name} expects a finite decimal number, found {raw!r}")
    try:
        value = float(value_text)
        converted = struct.unpack("<f", struct.pack("<f", value))[0]
    except (ValueError, OverflowError) as error:
        raise element.error(f"{property_name} is not a finite binary32 number") from error
    if not math.isfinite(converted):
        raise element.error(f"{property_name} is not a finite binary32 number")
    return converted


def _integer(raw: str, element: XmlElement, property_name: str) -> int:
    if not INTEGER.fullmatch(raw.strip()):
        raise element.error(f"{property_name} expects an integer, found {raw!r}")
    return int(raw.strip())


def _state_fields(element: XmlElement | None) -> list[StateDef]:
    if element is None:
        return []
    _allowed_attributes(element, set())
    if element.text.strip():
        raise element.error("<state> contains unexpected character data")
    states: list[StateDef] = []
    names: set[str] = set()
    for field in element.children:
        if field.tail.strip():
            raise field.error("unexpected character data after state field")
        if field.name != "field":
            raise field.error(f"expected <field>, found <{field.name}>")
        _no_nested_text(field)
        _allowed_attributes(field, {"name", "type", "value"})
        missing = {"name", "type", "value"} - field.attributes.keys()
        if missing:
            raise field.error(f"state field is missing {sorted(missing)[0]!r}")
        name = field.attributes["name"]
        if not IDENTIFIER.fullmatch(name) or name == "state":
            raise field.error(f"invalid state field name {name!r}")
        if name in names:
            raise field.error(f"duplicate state field {name!r}")
        names.add(name)
        if len(states) >= 256:
            raise field.error("package exceeds 256 state fields")
        value_type = field.attributes["type"]
        raw_value = field.attributes["value"]
        if value_type == "f32":
            states.append(StateDef(name, ValueType.NUMBER, _number(raw_value, field, "state value")))
        elif value_type == "bool":
            if raw_value not in ("true", "false"):
                raise field.error("boolean state value must be true or false")
            states.append(StateDef(name, ValueType.BOOLEAN, raw_value == "true"))
        elif value_type == "string":
            states.append(StateDef(name, ValueType.STRING, raw_value))
        else:
            raise field.error(f"unknown state type {value_type!r}; expected f32, bool, or string")
    return states


def _asset(element: XmlElement) -> AssetDef:
    _no_nested_text(element)
    _allowed_attributes(element, {"name", "src", "resize"})
    missing = {"name", "src"} - element.attributes.keys()
    if missing:
        raise element.error(f"asset is missing {sorted(missing)[0]!r}")
    name = element.attributes["name"]
    source = element.attributes["src"]
    if not name or not source:
        raise element.error("asset name and src must be nonempty")
    resize: tuple[int, int] | None = None
    if "resize" in element.attributes:
        raw = element.attributes["resize"]
        match = re.fullmatch(r"([0-9]+)x([0-9]+)", raw)
        if not match:
            raise element.error("resize must have the form WIDTHxHEIGHT")
        resize = (int(match.group(1)), int(match.group(2)))
        if not all(1 <= dimension <= 2048 for dimension in resize):
            raise element.error("resize dimensions must be 1 through 2048")
    return AssetDef(name, source, resize, element)


def _document(root: XmlElement) -> Document:
    if root.name != "microbrowser":
        raise root.error("root element must be <microbrowser>")
    _allowed_attributes(root, {"name"})
    if "name" not in root.attributes or not root.attributes["name"]:
        raise root.error("<microbrowser> requires a nonempty name")
    if root.text.strip():
        raise root.error("<microbrowser> contains unexpected character data")
    state_element: XmlElement | None = None
    style_element: XmlElement | None = None
    script_element: XmlElement | None = None
    assets: list[AssetDef] = []
    pages: list[XmlElement] = []
    singles: dict[str, XmlElement] = {}
    asset_names: set[str] = set()
    page_names: set[str] = set()
    for element in root.children:
        if element.tail.strip():
            raise element.error(f"unexpected character data after <{element.name}>")
        if element.name in ("state", "style", "script"):
            if element.name in singles:
                raise element.error(f"duplicate <{element.name}> element")
            singles[element.name] = element
            if element.name == "state":
                state_element = element
            elif element.name == "style":
                _no_child_elements(element)
                style_element = element
            else:
                _no_child_elements(element)
                script_element = element
        elif element.name == "asset":
            if pages:
                raise element.error("assets must precede all pages")
            asset = _asset(element)
            if asset.name in asset_names:
                raise element.error(f"duplicate asset name {asset.name!r}")
            asset_names.add(asset.name)
            assets.append(asset)
        elif element.name == "page":
            _allowed_attributes(element, {"name"})
            name = element.attributes.get("name", "")
            if not name:
                raise element.error("page requires a nonempty name")
            if name in page_names:
                raise element.error(f"duplicate page name {name!r}")
            page_names.add(name)
            if len(element.children) != 1:
                raise element.error("page must contain exactly one root element")
            if element.text.strip() or element.children[0].tail.strip():
                raise element.error("page contains unexpected character data")
            if len(pages) >= 32:
                raise element.error("package exceeds 32 pages")
            pages.append(element)
        else:
            raise element.error(f"unknown <microbrowser> child <{element.name}>")
    if not pages:
        raise root.error("application must contain at least one page")
    states = _state_fields(state_element)
    return Document(
        root.attributes["name"], states, style_element, assets, pages, script_element, root.path
    )


def _canonical_property(name: str) -> str:
    canonical = name.replace("_", "-")
    return canonical.replace("-", "_") if canonical in EVENT_FUNCTIONS else canonical


class _Declarations:
    def __init__(
        self, text: str, error: Callable[[int, str], CompileError]
    ) -> None:
        self.text = text
        self.index = 0
        self.error = error

    def whitespace(self) -> None:
        while self.index < len(self.text) and self.text[self.index].isspace():
            self.index += 1

    def name(self, what: str) -> str:
        match = CSS_NAME.match(self.text, self.index)
        if not match:
            raise self.error(self.index, f"expected {what}")
        self.index = match.end()
        return match.group(0)

    def class_rule(self) -> Rule:
        if self.index >= len(self.text) or self.text[self.index] != ".":
            raise self.error(self.index, "CSS rule must start with a .class selector")
        self.index += 1
        class_name = self.name("class name")
        self.whitespace()
        if self.index >= len(self.text) or self.text[self.index] != "{":
            raise self.error(self.index, "expected '{' after class selector")
        self.index += 1
        properties: dict[str, str] = {}
        while True:
            self.whitespace()
            if self.index < len(self.text) and self.text[self.index] == "}":
                self.index += 1
                return Rule(class_name, properties)
            if self.index == len(self.text):
                raise self.error(self.index, "unterminated CSS rule")
            name, value = self.declaration()
            if name in properties:
                raise self.error(self.index, f"duplicate CSS property {name!r} in one rule")
            properties[name] = value

    def declaration(self) -> tuple[str, str]:
        start = self.index
        name = _canonical_property(self.name("property name"))
        self.whitespace()
        if self.index >= len(self.text) or self.text[self.index] != ":":
            raise self.error(start, f"expected ':' after {name!r}")
        self.index += 1
        value_start = self.index
        end = self.text.find(";", self.index)
        if end < 0:
            raise self.error(value_start, f"declaration {name!r} must end with ';'")
        value = self.text[value_start:end].strip()
        if not value:
            raise self.error(value_start, f"declaration {name!r} has an empty value")
        self.index = end + 1
        return name, value


def _rules(element: XmlElement | None) -> list[Rule]:
    if element is None:
        return []
    parser = _Declarations(element.text, element.text_error)
    rules: list[Rule] = []
    while True:
        parser.whitespace()
        if parser.index == len(parser.text):
            return rules
        rules.append(parser.class_rule())


def _inline(element: XmlElement, text: str) -> dict[str, str]:
    parser = _Declarations(
        text,
        lambda _offset, message: element.error(f"inline style: {message}"),
    )
    properties: dict[str, str] = {}
    while True:
        parser.whitespace()
        if parser.index == len(text):
            return properties
        name, value = parser.declaration()
        if name in properties:
            raise element.error(f"duplicate inline style property {name!r}")
        properties[name] = value


def _merge_properties(properties: dict[str, str], incoming: dict[str, str]) -> None:
    if incoming.keys() & {"text", "bind"}:
        properties.pop("text", None)
        properties.pop("bind", None)
    properties.update(incoming)


def _properties(element: XmlElement, rules: list[Rule], kind: str) -> dict[str, str]:
    classes = set(element.attributes.get("class", "").split())
    properties: dict[str, str] = {}
    for rule in rules:
        if rule.class_name in classes:
            _merge_properties(properties, _normalise_content(rule.properties, kind, element))
    if "style" in element.attributes:
        inline = _inline(element, element.attributes["style"])
        _merge_properties(properties, _normalise_content(inline, kind, element))
    direct: dict[str, str] = {}
    for name, value in element.attributes.items():
        if name in ("class", "style"):
            continue
        canonical = _canonical_property(name)
        if canonical in direct:
            raise element.error(f"duplicate direct attribute {canonical!r} through underscore/hyphen alias")
        direct[canonical] = value
    _merge_properties(properties, _normalise_content(direct, kind, element))
    return properties


def _color(raw: str, element: XmlElement) -> int:
    if raw in PALETTE:
        return PALETTE[raw]
    if not raw.startswith("#") or len(raw) not in (4, 5, 7, 9):
        raise element.error(f"invalid color {raw!r}")
    digits = raw[1:]
    if any(character not in "0123456789abcdefABCDEF" for character in digits):
        raise element.error(f"invalid hexadecimal color {raw!r}")
    if len(digits) in (3, 4):
        digits = "".join(character * 2 for character in digits)
    if len(digits) == 6:
        digits += "FF"
    return int(digits, 16)


def _validate_format(raw: str, bound_type: ValueType, element: XmlElement) -> None:
    conversions: list[str] = []
    index = 0
    while index < len(raw):
        if raw[index] != "%":
            index += 1
            continue
        if raw.startswith("%%", index):
            index += 2
            continue
        match = FORMAT_CONVERSION.match(raw, index)
        if not match:
            raise element.error(f"unsafe or unsupported format conversion at offset {index}")
        conversions.append(match.group(0))
        index = match.end()
    if len(conversions) != 1:
        raise element.error("format must contain exactly one %f, %.Nf, %d, or %s conversion")
    conversion = conversions[0]
    if conversion == "%s":
        if bound_type != ValueType.STRING:
            raise element.error("%s format requires a string state binding")
    elif bound_type != ValueType.NUMBER:
        raise element.error(f"{conversion} format requires a numeric state binding")


def _length(raw: str, element: XmlElement, name: str) -> EncodedProperty:
    value = raw.strip()
    if value in ("auto", "grow"):
        return EncodedProperty(name, 0, value)
    if value.endswith("%"):
        percentage = _number(value[:-1], element, name)
        if not 0 <= percentage <= 100:
            raise element.error(f"{name} percentage must be from 0 through 100")
        return EncodedProperty(name, 0, f"{percentage:.9g}%")
    pixels = _number(value, element, name)
    if not 0 <= pixels <= MAX_FIXED_PIXELS:
        raise element.error(f"{name} must be between 0 and {MAX_FIXED_PIXELS} pixels")
    return EncodedProperty(name, 1, pixels)


def _samples(raw: str, element: XmlElement) -> str:
    if not raw.strip():
        return ""
    values = raw.split(",")
    if len(values) > 64:
        raise element.error("chart samples exceed 64 values")
    converted: list[str] = []
    for index, value in enumerate(values):
        if not value.strip():
            raise element.error(f"chart sample {index} is empty")
        converted.append(f"{_number(value, element, 'samples'):.9g}")
    return ",".join(converted)


def _binding_name(raw: str) -> str | None:
    match = BINDING.fullmatch(raw)
    return match.group(1) if match else None


def _normalise_content(raw: dict[str, str], kind: str, element: XmlElement) -> dict[str, str]:
    result = dict(raw)
    value = result.pop("value", None)
    text = result.get("text")
    binding_sources: list[tuple[str, str]] = []
    if "bind" in result:
        binding = _binding_name(result["bind"])
        if binding is None:
            raise element.error("bind must be a whole {state.field} binding")
        binding_sources.append(("bind", binding))
    if text is not None:
        binding = _binding_name(text)
        if binding is not None:
            binding_sources.append(("text", binding))
            del result["text"]
    if value is not None:
        binding = _binding_name(value)
        if binding is not None:
            binding_sources.append(("value", binding))
        elif kind == "text":
            if "text" in result:
                raise element.error("text and literal value cannot both be specified")
            result["text"] = value
        else:
            raise element.error(f"{kind} value must be a whole {{state.field}} binding")
    if len(binding_sources) > 1:
        names = ", ".join(source for source, _ in binding_sources)
        raise element.error(f"multiple state bindings specified through {names}")
    if binding_sources:
        result["bind"] = "{state." + binding_sources[0][1] + "}"
    if "bind" in result and "text" in result:
        raise element.error("literal text and a state binding cannot both be specified")
    return result


class _UiCompiler:
    def __init__(
        self,
        states: dict[str, tuple[int, ValueType]],
        functions: list[EncodedFunction],
        rules: list[Rule],
        asset_names: set[str],
        node_count: list[int],
    ) -> None:
        self.states = states
        self.functions = functions
        self.function_indices = {function.name: index for index, function in enumerate(functions)}
        self.function_parameters = {function.name: function.parameter_count for function in functions}
        self.rules = rules
        self.asset_names = asset_names
        self.node_count = node_count
        self.ids: set[str] = set()

    def page(self, page: XmlElement) -> EncodedNode:
        self.ids = set()
        return self.node(page.children[0], True, 1)

    def node(self, element: XmlElement, is_root: bool, depth: int) -> EncodedNode:
        if depth > 32:
            raise element.error("page exceeds 32 nested elements")
        if element.name not in NODE_KINDS:
            raise element.error(f"unknown element <{element.name}>")
        kind = element.name
        if len(element.children) > 64:
            raise element.error("element exceeds 64 direct children")
        self.node_count[0] += 1
        if self.node_count[0] > 2048:
            raise element.error("package exceeds 2,048 elements")
        if element.text.strip() or any(child.tail.strip() for child in element.children):
            raise element.error("element text must be supplied through text or value attributes")
        if kind not in CONTAINERS and element.children:
            raise element.error(f"<{kind}> cannot contain element children")
        raw = _properties(element, self.rules, kind)
        allowed = COMMON_PROPERTIES | NODE_PROPERTIES[kind]
        if is_root and kind in CONTAINERS:
            allowed = allowed | {"on_load"}
        for name in raw:
            if name not in allowed:
                raise element.error(f"property {name!r} is not meaningful on <{kind}>")
        if kind in ("input", "slider", "progress") and "bind" not in raw:
            raise element.error(f"{kind} requires one {kind == 'input' and 'string' or 'numeric'} state binding")
        properties = [self.property(element, name, value) for name, value in raw.items()]
        if len(properties) > 40:
            raise element.error("element exceeds 40 resolved properties")
        self.semantics(element, kind, raw, properties)
        children = [self.node(child, False, depth + 1) for child in element.children]
        return EncodedNode(NODE_KINDS[kind], properties, children)

    def property(self, element: XmlElement, name: str, value: str) -> EncodedProperty:
        canonical = name.replace("-", "_") if name in EVENT_FUNCTIONS else name
        if canonical in ("id", "text", "format", "placeholder", "src", "animate"):
            if canonical in ("id", "src") and not value:
                raise element.error(f"{canonical} must be nonempty")
            if canonical == "id":
                if len(value.encode("utf-8")) > MAX_ELEMENT_ID_BYTES:
                    raise element.error(f"element id exceeds {MAX_ELEMENT_ID_BYTES} bytes")
                if value in self.ids:
                    raise element.error(f"duplicate element id {value!r} on one page")
                self.ids.add(value)
            if canonical == "animate":
                match = re.fullmatch(r"(fade|pulse|slide)(?::([0-9]+))?", value.strip())
                if not match or (match.group(2) is not None and not 1 <= int(match.group(2)) <= 60_000):
                    raise element.error("animate must be fade, pulse, or slide; duration must be 1..60000 milliseconds")
                value = match.group(1)
                if match.group(2) is not None:
                    value += ":" + str(int(match.group(2)))
            if canonical == "src" and value not in self.asset_names:
                raise element.error(f"image src {value!r} does not name a declared asset")
            return EncodedProperty(canonical, 0, value)
        if canonical == "samples":
            return EncodedProperty(canonical, 0, _samples(value, element))
        if canonical == "bind":
            state_name = _binding_name(value)
            if state_name is None or state_name not in self.states:
                raise element.error(f"unknown state binding {value!r}")
            return EncodedProperty(canonical, 4, self.states[state_name][0])
        if canonical in EVENTS:
            match = HANDLER.fullmatch(value.strip())
            if not match:
                raise element.error(f"{name} must contain exactly one no-argument function call")
            function_name = match.group(1)
            if function_name not in self.function_indices:
                raise element.error(f"unknown event function {function_name!r}")
            if self.function_parameters[function_name] != 0:
                raise element.error(f"event function {function_name!r} must have no parameters")
            return EncodedProperty(canonical, 1, float(self.function_indices[function_name]))
        if name in ("width", "height"):
            return _length(value, element, name)
        if name in ("background", "color", "border"):
            return EncodedProperty(name, 3, _color(value.strip(), element))
        if name == "scroll":
            if value.strip() not in ("true", "false"):
                raise element.error("scroll must be true or false")
            return EncodedProperty(name, 2, value.strip() == "true")
        if name == "align":
            if value.strip() not in ("start", "center", "end", "stretch"):
                raise element.error("align must be start, center, end, or stretch")
            return EncodedProperty(name, 0, value.strip())
        if name == "justify":
            if value.strip() not in ("start", "center", "end", "space-between"):
                raise element.error("justify must be start, center, end, or space-between")
            return EncodedProperty(name, 0, value.strip())
        if name == "position":
            if value.strip() not in ("static", "absolute"):
                raise element.error("position must be static or absolute")
            return EncodedProperty(name, 0, value.strip())
        if name in ("font-size", "font-weight"):
            integer = _integer(value, element, name)
            allowed = FONT_SIZES if name == "font-size" else FONT_WEIGHTS
            if integer not in allowed:
                raise element.error(f"{name} must be one of {allowed}")
            return EncodedProperty(name, 1, float(integer))
        if name in ("padding", "gap", "radius", "x", "y", "max-length"):
            integer = _integer(value, element, name)
            minimum, maximum = INTEGER_PROPERTY_BOUNDS[name]
            if not minimum <= integer <= maximum:
                raise element.error(f"{name} must be an integer between {minimum} and {maximum}")
            return EncodedProperty(name, 1, float(integer))
        if name in ("min", "max", "step"):
            return EncodedProperty(name, 1, _number(value, element, name))
        raise element.error(f"unsupported property {name!r}")

    def semantics(
        self,
        element: XmlElement,
        kind: str,
        raw: dict[str, str],
        properties: list[EncodedProperty],
    ) -> None:
        encoded = {prop.name: prop for prop in properties}
        if "bind" in encoded:
            state_name = _binding_name(raw["bind"])
            assert state_name is not None
            bound_type = self.states[state_name][1]
            if kind == "input" and bound_type != ValueType.STRING:
                raise element.error("input requires a string state binding")
            if kind in ("slider", "progress") and bound_type != ValueType.NUMBER:
                raise element.error(f"{kind} requires a numeric state binding")
            if "format" in encoded:
                if kind != "text":
                    raise element.error("format is meaningful only on bound text")
                _validate_format(str(encoded["format"].value), bound_type, element)
        elif "format" in encoded:
            raise element.error("format requires a bound text state")
        if kind in ("slider", "progress", "chart"):
            minimum = float(encoded.get("min", EncodedProperty("min", 1, 0.0)).value)
            maximum = float(encoded.get("max", EncodedProperty("max", 1, 100.0)).value)
            if kind == "slider" and minimum > maximum:
                raise element.error("slider min must not exceed max")
            if kind in ("progress", "chart") and minimum >= maximum:
                raise element.error(f"{kind} min must be less than max")
            if kind == "slider" and "step" in encoded and float(encoded["step"].value) <= 0:
                raise element.error("slider step must be greater than zero")


def _pack_constant(constant: Constant, strings: _StringTable) -> bytes:
    if constant.type == ValueType.NUMBER:
        return b"\x00" + struct.pack("<f", float(constant.value))
    if constant.type == ValueType.BOOLEAN:
        return b"\x01" + bytes([int(bool(constant.value))])
    if constant.type == ValueType.STRING:
        return b"\x02" + struct.pack("<I", strings.intern(str(constant.value)))
    raise ValueError("void cannot be a bytecode constant")


def _code_section(functions: list[EncodedFunction], strings: _StringTable) -> bytes:
    result = bytearray(struct.pack("<I", len(functions)))
    for function in functions:
        result.extend(struct.pack("<I", strings.intern(function.name)))
        result.extend(struct.pack("<HHII", function.parameter_count, function.local_count, len(function.code), len(function.constants)))
        for constant in function.constants:
            result.extend(_pack_constant(constant, strings))
        result.extend(function.code)
    return bytes(result)


def _state_section(states: list[StateDef], strings: _StringTable) -> bytes:
    result = bytearray(struct.pack("<I", len(states)))
    for state in states:
        result.extend(struct.pack("<I", strings.intern(state.name)))
        result.extend(_pack_constant(Constant(state.type, state.value), strings))
    return bytes(result)


def _page_section(name: str, root: EncodedNode, strings: _StringTable) -> bytes:
    result = bytearray(struct.pack("<I", strings.intern(name)))

    def append_node(node: EncodedNode) -> None:
        result.extend(bytes([node.kind, 0]))
        result.extend(struct.pack("<H", len(node.properties)))
        for prop in node.properties:
            result.extend(struct.pack("<I", strings.intern(prop.name)))
            result.append(prop.kind)
            if prop.kind == 0:
                result.extend(struct.pack("<I", strings.intern(str(prop.value))))
            elif prop.kind == 1:
                result.extend(struct.pack("<f", float(prop.value)))
            elif prop.kind == 2:
                result.append(int(bool(prop.value)))
            elif prop.kind in (3, 4):
                result.extend(struct.pack("<I", int(prop.value)))
            else:
                raise ValueError(f"unknown property kind {prop.kind}")
        result.extend(struct.pack("<I", len(node.children)))
        for child in node.children:
            append_node(child)

    append_node(root)
    return bytes(result)


def _data_section(asset: EncodedAsset, strings: _StringTable) -> bytes:
    return (
        struct.pack("<IHHI", strings.intern(asset.name), asset.width, asset.height, len(asset.pixels))
        + asset.pixels
    )


def _envelope(sections: list[tuple[bytes, bytes]]) -> bytes:
    body = bytearray()
    for name, payload in sections:
        body.extend(name)
        body.extend(struct.pack("<I", len(payload)))
        body.extend(payload)
    total_size = 20 + len(body)
    if total_size > MAX_PACKAGE_BYTES:
        raise CompileError("package", 1, 1, f"package size {total_size} exceeds 16 MiB")
    header = struct.pack("<4sHHIII", b"MBPK", VERSION_MAJOR, VERSION_MINOR, total_size, len(sections), 0)
    without_crc = header + bytes(body)
    crc = zlib.crc32(without_crc) & 0xFFFFFFFF
    return without_crc[:16] + struct.pack("<I", crc) + without_crc[20:]


def build_package(root: XmlElement) -> bytes:
    document = _document(root)
    states = document.states
    state_indices = {state.name: (index, state.type) for index, state in enumerate(states)}
    if document.script is None:
        functions: list[EncodedFunction] = []
    else:
        functions = compile_script(
            document.script.text,
            document.path,
            document.script.text_line,
            document.script.text_column,
            state_indices,
        ).functions
    strings = _StringTable()
    state_payload = _state_section(states, strings)
    code_payload = _code_section(functions, strings)
    node_count = [0]
    ui = _UiCompiler(
        state_indices,
        functions,
        _rules(document.style),
        {asset.name for asset in document.assets},
        node_count,
    )
    page_payloads = [
        _page_section(page.attributes["name"], ui.page(page), strings) for page in document.pages
    ]
    assets: list[EncodedAsset] = []
    source_directory = Path(document.path).parent
    for asset in document.assets:
        asset_path = source_directory / asset.source
        try:
            data = asset_path.read_bytes()
        except OSError as error:
            raise asset.element.error(f"cannot read PNG {asset.source!r}: {error}") from error
        try:
            image = rgb565_asset(data, asset.resize)
        except PngError as error:
            raise asset.element.error(f"asset {asset.source!r}: {error}") from error
        assets.append(EncodedAsset(asset.name, image.width, image.height, image.pixels))
    sections = [
        (b"STRS", b""),
        (b"STAT", state_payload),
        (b"CODE", code_payload),
        *[(b"PAGE", payload) for payload in page_payloads],
        *[(b"DATA", _data_section(asset, strings)) for asset in assets],
    ]
    sections[0] = (b"STRS", strings.encode())
    package = _envelope(sections)
    decode_package(package)
    return package


def compile_source(data: bytes, path: str | Path) -> bytes:
    return build_package(parse_xml_bytes(data, path))


def compile_file(path: str | Path) -> bytes:
    return build_package(parse_xml_file(path))
