from __future__ import annotations

import math
import struct
import zlib
from dataclasses import dataclass
from pathlib import Path

from .device_api import (
    CAPABILITY_IDS,
    ENDPOINT_RE,
    EMPTY_HANDLE,
    CAPABILITY_NAMES,
    IO_BY_ID,
    MAX_BYTES_PER_BUFFER,
    MAX_ENDPOINT_BYTES,
    ScriptValue,
    ValueType,
)
from .errors import PackageError
from .script import BUILTINS, Opcode

MAGIC = b"MBPK"
VERSION_MAJOR = 2
HARDWARE_VERSION_MAJOR = 3
VERSION_MINOR = 0
FONT_SIZES = (12, 14, 16, 20, 28, 36)
FONT_WEIGHTS = (400, 600)
HEADER_SIZE = 20
MAX_PACKAGE_BYTES = 16 * 1024 * 1024
MAX_STATES = 256
MAX_FUNCTIONS = 128
MAX_LOCALS = 32
MAX_CODE_BYTES = 65_535
MAX_CONSTANTS = 256
MAX_PAGES = 32
MAX_NODES = 2048
MAX_PROPERTIES = 40
MAX_DEPTH = 32
MAX_ASSET_DIMENSION = 2048
CONTAINER_KINDS = {0, 1, 2, 11}
PROPERTY_NAMES = {
    "id",
    "text",
    "bind",
    "format",
    "placeholder",
    "src",
    "min",
    "max",
    "step",
    "max-length",
    "samples",
    "animate",
    "on_press",
    "on_change",
    "on_submit",
    "on_load",
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
}
EVENT_PROPERTIES = {"on_press", "on_change", "on_submit", "on_load"}
PROPERTY_VALUE_KINDS = {
    **dict.fromkeys(("id", "text", "format", "placeholder", "src", "animate", "samples"), {0}),
    **dict.fromkeys(("padding", "gap", "font-size", "font-weight", "radius", "x", "y", "min", "max", "step", "max-length"), {1}),
    **dict.fromkeys(("background", "color", "border"), {3}),
    **dict.fromkeys(("align", "justify", "position"), {0}),
    **dict.fromkeys(("width", "height"), {0, 1}),
    **dict.fromkeys(("on_press", "on_change", "on_submit", "on_load"), {1}),
    "scroll": {2},
    "bind": {4},
}


BUILTIN_BY_ID = {builtin.id: builtin for builtin in BUILTINS.values()}
INDEX_U16_OPS = {Opcode.PUSH_CONST, Opcode.LOAD_STATE, Opcode.STORE_STATE, Opcode.LOAD_LOCAL, Opcode.STORE_LOCAL}
NO_IMMEDIATE_OPS = {
    Opcode.POP,
    Opcode.ADD,
    Opcode.SUBTRACT,
    Opcode.MULTIPLY,
    Opcode.DIVIDE,
    Opcode.NEGATE,
    Opcode.EQUAL,
    Opcode.NOT_EQUAL,
    Opcode.LESS,
    Opcode.LESS_EQUAL,
    Opcode.GREATER,
    Opcode.GREATER_EQUAL,
    Opcode.NOT,
    Opcode.RETURN,
}

@dataclass(frozen=True, slots=True)
class DecodedConstant:
    type: ValueType
    value: ScriptValue


@dataclass(frozen=True, slots=True)
class DecodedState:
    name: str
    type: ValueType
    value: ScriptValue


@dataclass(frozen=True, slots=True)
class DecodedCapability:
    capability_id: int
    major: int
    minor: int
    required: bool

@dataclass(frozen=True, slots=True)
class DecodedFunction:
    name: str
    parameter_count: int
    local_count: int
    code: bytes
    constants: tuple[DecodedConstant, ...]


@dataclass(frozen=True, slots=True)
class DecodedProperty:
    kind: int
    value: str | float | bool | int


@dataclass(frozen=True, slots=True)
class DecodedNode:
    kind: int
    properties: dict[str, DecodedProperty]
    children: tuple[DecodedNode, ...]


@dataclass(frozen=True, slots=True)
class DecodedPage:
    name: str
    root: DecodedNode


@dataclass(frozen=True, slots=True)
class DecodedAsset:
    name: str
    width: int
    height: int
    pixels: bytes


@dataclass(frozen=True, slots=True)
class DecodedPackage:
    strings: tuple[str, ...]
    states: tuple[DecodedState, ...]
    functions: tuple[DecodedFunction, ...]
    pages: tuple[DecodedPage, ...]
    assets: tuple[DecodedAsset, ...]
    crc32: int
    major: int = VERSION_MAJOR
    minor: int = VERSION_MINOR
    capabilities: tuple[DecodedCapability, ...] = ()
    permission_endpoints: tuple[str, ...] = ()


class _Reader:
    def __init__(self, data: bytes, base: int = 0, label: str = "package") -> None:
        self.data = data
        self.base = base
        self.position = 0
        self.label = label

    @property
    def remaining(self) -> int:
        return len(self.data) - self.position

    def take(self, size: int, what: str) -> bytes:
        if size < 0 or size > self.remaining:
            raise PackageError(
                f"offset 0x{self.base + self.position:x}: truncated {what} in {self.label}"
            )
        start = self.position
        self.position += size
        return self.data[start : start + size]

    def unpack(self, fmt: str, what: str) -> tuple[int | float, ...]:
        size = struct.calcsize(fmt)
        offset = self.position
        raw = self.take(size, what)
        try:
            return struct.unpack(fmt, raw)
        except struct.error as error:  # pragma: no cover - take already checks size
            raise PackageError(f"offset 0x{self.base + offset:x}: invalid {what}") from error

    def u8(self, what: str) -> int:
        return int(self.unpack("<B", what)[0])

    def u16(self, what: str) -> int:
        return int(self.unpack("<H", what)[0])

    def i16(self, what: str) -> int:
        return int(self.unpack("<h", what)[0])

    def u32(self, what: str) -> int:
        return int(self.unpack("<I", what)[0])

    def f32(self, what: str) -> float:
        return float(self.unpack("<f", what)[0])

    def i64(self, what: str) -> int:
        return int(self.unpack("<q", what)[0])

    def finish(self) -> None:
        if self.remaining:
            raise PackageError(
                f"offset 0x{self.base + self.position:x}: {self.remaining} trailing bytes in {self.label}"
            )


def _strings(payload: bytes, offset: int) -> tuple[str, ...]:
    reader = _Reader(payload, offset, "STRS")
    count = reader.u32("string count")
    table_size = count * 4
    if table_size > reader.remaining:
        raise PackageError("STRS count exceeds its offset table")
    offsets = [
        struct.unpack_from("<I", reader.take(4, "string offset"))[0] for _ in range(count)
    ]
    storage_size = reader.remaining
    strings: list[str] = []
    previous = -1
    for index, relative in enumerate(offsets):
        if relative >= storage_size:
            raise PackageError(f"STRS offset {relative} for string {index} is outside its storage")
        if relative <= previous:
            raise PackageError("STRS offsets must be strictly increasing")
        end = payload.find(b"\x00", reader.position + relative)
        if end < 0:
            raise PackageError(f"STRS string {index} has no NUL terminator")
        raw = payload[reader.position + relative : end]
        try:
            strings.append(raw.decode("utf-8"))
        except UnicodeDecodeError as error:
            raise PackageError(f"STRS string {index} is not valid UTF-8") from error
        previous = relative
    if count:
        last_start = reader.position + offsets[-1]
        last_end = payload.find(b"\x00", last_start)
        if last_end != len(payload) - 1:
            raise PackageError("final STRS byte must be the final string's NUL")
    reader.position = len(payload)
    reader.finish()
    return tuple(strings)


def _string(strings: tuple[str, ...], index: int, what: str) -> str:
    if index >= len(strings):
        raise PackageError(f"{what} string ID {index} is outside STRS")
    return strings[index]


def _constant(reader: _Reader, strings: tuple[str, ...], major: int) -> DecodedConstant:
    tag = reader.u8("value kind")
    if tag == 0:
        return DecodedConstant(ValueType.NUMBER, reader.f32("number value"))
    if tag == 1:
        value = reader.u8("boolean value")
        if value not in (0, 1):
            raise PackageError("boolean value must be exactly zero or one")
        return DecodedConstant(ValueType.BOOLEAN, bool(value))
    if tag == 2:
        index = reader.u32("string value ID")
        return DecodedConstant(ValueType.STRING, _string(strings, index, "value"))
    if tag not in (3, 4, 5):
        raise PackageError(f"unknown value kind {tag}")
    if major < HARDWARE_VERSION_MAJOR:
        raise PackageError(f"value kind {tag} requires package major version 3")
    if tag == 3:
        return DecodedConstant(ValueType.INTEGER, reader.i64("i64 value"))
    if tag == 4:
        length = reader.u32("bytes length")
        if length > MAX_BYTES_PER_BUFFER:
            raise PackageError(f"bytes value exceeds {MAX_BYTES_PER_BUFFER} bytes")
        return DecodedConstant(ValueType.BYTES, reader.take(length, "bytes value"))
    if tag == 5:
        if reader.u16("empty handle reserved value") != 0:
            raise PackageError("only an empty handle with reserved value zero may be packaged")
        return DecodedConstant(ValueType.HANDLE, EMPTY_HANDLE)


def _states(
    payload: bytes, offset: int, strings: tuple[str, ...], major: int
) -> tuple[DecodedState, ...]:
    if not strings:
        raise PackageError("STAT requires a valid STRS section")
    reader = _Reader(payload, offset, "STAT")
    count = reader.u32("state count")
    if count > MAX_STATES:
        raise PackageError("STAT exceeds 256 fields")
    states: list[DecodedState] = []
    names: set[str] = set()
    for _ in range(count):
        name = _string(strings, reader.u32("state name ID"), "state name")
        if name in names:
            raise PackageError(f"duplicate state name {name!r}")
        names.add(name)
        initial = _constant(reader, strings, major)
        if initial.type == ValueType.VOID:
            raise PackageError("state cannot have void type")
        states.append(DecodedState(name, initial.type, initial.value))
    reader.finish()
    return tuple(states)


def _capabilities(payload: bytes, offset: int) -> tuple[DecodedCapability, ...]:
    reader = _Reader(payload, offset, "CAPS")
    count = reader.u16("capability count")
    capabilities: list[DecodedCapability] = []
    identifiers: set[int] = set()
    for _ in range(count):
        capability_id = reader.u16("capability ID")
        major = reader.u16("capability major version")
        minor = reader.u16("capability minor version")
        required = reader.u16("capability required flag")
        if capability_id not in CAPABILITY_IDS:
            raise PackageError(f"unrecognized capability ID {capability_id}")
        if capability_id in identifiers:
            raise PackageError(f"duplicate capability {CAPABILITY_NAMES[capability_id]!r}")
        identifiers.add(capability_id)
        if major != 1:
            raise PackageError(f"capability {CAPABILITY_NAMES[capability_id]!r} requires major version 1")
        if required not in (0, 1):
            raise PackageError("capability required flag must be exactly zero or one")
        capabilities.append(DecodedCapability(capability_id, major, minor, bool(required)))
    reader.finish()
    return tuple(capabilities)


def _permissions(payload: bytes, offset: int) -> tuple[str, ...]:
    reader = _Reader(payload, offset, "PERM")
    count = reader.u16("endpoint count")
    endpoints: list[str] = []
    names: set[str] = set()
    for _ in range(count):
        length = reader.u16("endpoint UTF-8 byte length")
        raw = reader.take(length, "endpoint name")
        try:
            name = raw.decode("ascii")
        except UnicodeDecodeError as error:
            raise PackageError("permission endpoint must be ASCII") from error
        if not 1 <= len(name) <= MAX_ENDPOINT_BYTES or not ENDPOINT_RE.fullmatch(name):
            raise PackageError(
                "permission endpoint must match [A-Za-z0-9_.-]+ and be 1 through 64 ASCII bytes"
            )
        if name in names:
            raise PackageError(f"duplicate permission endpoint {name!r}")
        names.add(name)
        endpoints.append(name)
    reader.finish()
    return tuple(endpoints)


def _instructions(
    code: bytes,
    offset: int,
    constant_count: int,
    local_count: int,
    state_count: int,
    major: int,
) -> list[tuple[int, int]]:
    reader = _Reader(code, offset, "function code")
    boundaries: set[int] = set()
    jumps: list[tuple[int, int]] = []
    calls: list[tuple[int, int]] = []
    while reader.remaining:
        instruction = reader.position
        boundaries.add(instruction)
        raw_opcode = reader.u8("opcode")
        try:
            opcode = Opcode(raw_opcode)
        except ValueError as error:
            raise PackageError(f"unknown opcode {raw_opcode}") from error
        if opcode in INDEX_U16_OPS:
            index = reader.u16("instruction index")
            if opcode == Opcode.PUSH_CONST and index >= constant_count:
                raise PackageError(f"PUSH_CONST index {index} is outside the constant pool")
            if opcode in (Opcode.LOAD_LOCAL, Opcode.STORE_LOCAL) and index >= local_count:
                raise PackageError(f"local index {index} exceeds {local_count} locals")
            if opcode in (Opcode.LOAD_STATE, Opcode.STORE_STATE) and index >= state_count:
                raise PackageError(f"state index {index} exceeds {state_count} state fields")
        elif opcode == Opcode.PUSH_BOOL:
            if reader.u8("PUSH_BOOL immediate") not in (0, 1):
                raise PackageError("PUSH_BOOL immediate must be zero or one")
        elif opcode in (Opcode.JUMP, Opcode.JUMP_FALSE):
            relative = reader.i16("jump offset")
            jumps.append((instruction, reader.position + relative))
        elif opcode == Opcode.CALL:
            calls.append((reader.u16("CALL function index"), reader.u8("CALL argument count")))
        elif opcode == Opcode.BUILTIN:
            builtin_id = reader.u8("BUILTIN id")
            arguments = reader.u8("BUILTIN argument count")
            builtin = BUILTIN_BY_ID.get(builtin_id)
            if builtin is None:
                raise PackageError(f"unknown builtin ID {builtin_id}")
            if arguments != len(builtin.arguments):
                raise PackageError(
                    f"builtin ID {builtin_id} expects {len(builtin.arguments)} arguments, found {arguments}"
                )
        elif opcode == Opcode.DEVICE_CALL:
            if major < HARDWARE_VERSION_MAJOR:
                raise PackageError("device calls require package major version 3")
            operation = reader.u16("device operation")
            arguments = reader.u8("device argument count")
            device_call = IO_BY_ID.get(operation)
            if device_call is None:
                raise PackageError(f"unknown device operation {operation}")
            if not device_call.accepts_arity(arguments):
                raise PackageError(
                    f"device operation {operation} has no prototype for {arguments} arguments"
                )
        elif opcode not in NO_IMMEDIATE_OPS:
            raise PackageError(f"unhandled opcode {raw_opcode}")
    for instruction, target in jumps:
        if target not in boundaries:
            raise PackageError(
                f"jump at code offset {instruction} targets non-boundary or out-of-range offset {target}"
            )
    return calls


def _functions(
    payload: bytes, offset: int, strings: tuple[str, ...], states: tuple[DecodedState, ...], major: int
) -> tuple[DecodedFunction, ...]:
    reader = _Reader(payload, offset, "CODE")
    count = reader.u32("function count")
    if count > MAX_FUNCTIONS:
        raise PackageError("CODE exceeds 128 functions")
    functions: list[DecodedFunction] = []
    all_calls: list[list[tuple[int, int]]] = []
    names: set[str] = set()
    for _ in range(count):
        name = _string(strings, reader.u32("function name ID"), "function name")
        if name in names:
            raise PackageError(f"duplicate function name {name!r}")
        names.add(name)
        parameter_count = reader.u16("parameter count")
        local_count = reader.u16("local count")
        code_size = reader.u32("code size")
        constant_count = reader.u32("constant count")
        if local_count > MAX_LOCALS or parameter_count > local_count:
            raise PackageError("invalid local/parameter count (at most 32 locals including parameters)")
        if code_size > MAX_CODE_BYTES:
            raise PackageError("function code exceeds 65,535 bytes")
        if constant_count > MAX_CONSTANTS:
            raise PackageError("function exceeds 256 constants")
        constants = tuple(_constant(reader, strings, major) for _ in range(constant_count))
        code_offset = offset + reader.position
        code = reader.take(code_size, "function bytecode")
        calls = _instructions(code, code_offset, len(constants), local_count, len(states), major)
        all_calls.append(calls)
        functions.append(
            DecodedFunction(name, parameter_count, local_count, code, constants)
        )
    reader.finish()
    for function, calls in zip(functions, all_calls, strict=True):
        for target, arguments in calls:
            if target >= len(functions):
                raise PackageError(f"CALL function index {target} is outside CODE")
            expected = functions[target].parameter_count
            if arguments != expected:
                raise PackageError(
                    f"CALL to {functions[target].name!r} has {arguments} arguments, expected {expected}"
                )
    return tuple(functions)


def _node(
    reader: _Reader,
    strings: tuple[str, ...],
    states: tuple[DecodedState, ...],
    functions: tuple[DecodedFunction, ...],
    depth: int,
    node_count: list[int],
) -> DecodedNode:
    if depth > MAX_DEPTH:
        raise PackageError("PAGE exceeds 32 nested elements")
    node_count[0] += 1
    if node_count[0] > MAX_NODES:
        raise PackageError("package exceeds 2,048 elements")
    kind = reader.u8("node kind")
    flags = reader.u8("node flags")
    if kind > 11:
        raise PackageError(f"unknown node kind {kind}")
    if flags != 0:
        raise PackageError("version-2 node flags must be zero")
    property_count = reader.u16("property count")
    if property_count > MAX_PROPERTIES:
        raise PackageError("node exceeds 40 properties")
    properties: dict[str, DecodedProperty] = {}

    for _ in range(property_count):
        name = _string(strings, reader.u32("property name ID"), "property name")
        if name not in PROPERTY_NAMES:
            raise PackageError(f"unknown property name {name!r}")
        if name in properties:
            raise PackageError(f"duplicate property {name!r}")
        value_kind = reader.u8("property value kind")
        value: str | float | bool | int
        if value_kind == 0:
            value = _string(strings, reader.u32("property string ID"), f"property {name}")
        elif value_kind == 1:
            value = reader.f32("property number")
        elif value_kind == 2:
            boolean = reader.u8("property boolean")
            if boolean not in (0, 1):
                raise PackageError("property boolean must be zero or one")
            value = bool(boolean)
        elif value_kind == 3:
            value = reader.u32("property color")
        elif value_kind == 4:
            value = reader.u32("property state index")
            if value >= len(states):
                raise PackageError(f"property {name} refers to missing state {value}")
        else:
            raise PackageError(f"unknown property value kind {value_kind}")
        if value_kind not in PROPERTY_VALUE_KINDS[name]:
            raise PackageError(f"property {name} uses invalid value kind {value_kind}")
        if name == "bind" and value_kind != 4:
            raise PackageError("bind must use state-index value kind 4")
        if name in EVENT_PROPERTIES:
            if value_kind != 1 or not math.isfinite(float(value)) or not float(value).is_integer():
                raise PackageError(f"{name} must contain a numeric function index")
            if not 0 <= int(value) < len(functions):
                raise PackageError(f"{name} refers to missing function {int(value)}")
        properties[name] = DecodedProperty(value_kind, value)
        size = properties.get("font-size")
        if size is not None and float(size.value) not in FONT_SIZES:
            raise PackageError(f"font-size must be one of {FONT_SIZES}")
        weight = properties.get("font-weight")
        if weight is not None and float(weight.value) not in FONT_WEIGHTS:
            raise PackageError(f"font-weight must be one of {FONT_WEIGHTS}")
    child_count = reader.u32("child count")
    if child_count > 64:
        raise PackageError("node exceeds 64 direct children")
    if kind not in CONTAINER_KINDS and child_count:
        raise PackageError(f"node kind {kind} cannot contain children")
    children = tuple(
        _node(reader, strings, states, functions, depth + 1, node_count)
        for _ in range(child_count)
    )
    return DecodedNode(kind, properties, children)


def _pages(
    payloads: list[tuple[bytes, int]],
    strings: tuple[str, ...],
    states: tuple[DecodedState, ...],
    functions: tuple[DecodedFunction, ...],
) -> tuple[DecodedPage, ...]:
    if not payloads or len(payloads) > MAX_PAGES:
        raise PackageError("package must contain between 1 and 32 PAGE sections")
    pages: list[DecodedPage] = []
    names: set[str] = set()
    node_count = [0]
    for payload, offset in payloads:
        reader = _Reader(payload, offset, "PAGE")
        name = _string(strings, reader.u32("page name ID"), "page name")
        if name in names:
            raise PackageError(f"duplicate page name {name!r}")
        names.add(name)
        pages.append(DecodedPage(name, _node(reader, strings, states, functions, 1, node_count)))
        reader.finish()
    return tuple(pages)


def _assets(
    payloads: list[tuple[bytes, int]], strings: tuple[str, ...]
) -> tuple[DecodedAsset, ...]:
    assets: list[DecodedAsset] = []
    names: set[str] = set()
    for payload, offset in payloads:
        reader = _Reader(payload, offset, "DATA")
        name = _string(strings, reader.u32("asset name ID"), "asset name")
        if name in names:
            raise PackageError(f"duplicate asset name {name!r}")
        names.add(name)
        width = reader.u16("asset width")
        height = reader.u16("asset height")
        data_size = reader.u32("asset data size")
        if not 1 <= width <= MAX_ASSET_DIMENSION or not 1 <= height <= MAX_ASSET_DIMENSION:
            raise PackageError("asset dimensions must be 1 through 2048")
        expected = width * height * 2
        if data_size != expected:
            raise PackageError(f"asset data size {data_size} does not equal {expected}")
        pixels = reader.take(data_size, "RGB565 pixels")
        reader.finish()
        assets.append(DecodedAsset(name, width, height, pixels))
    return tuple(assets)


def _read_sections(data: bytes, major: int, count: int) -> list[tuple[bytes, bytes, int]]:
    sections: list[tuple[bytes, bytes, int]] = []
    offset = HEADER_SIZE
    legacy_ids = (b"STRS", b"STAT", b"CODE", b"PAGE", b"DATA")
    v3_ids = (*legacy_ids, b"CAPS", b"PERM")
    allowed_ids = legacy_ids if major == VERSION_MAJOR else v3_ids
    for _ in range(count):
        if len(data) - offset < 8:
            raise PackageError("truncated section header")
        section_id = data[offset : offset + 4]
        size = struct.unpack_from("<I", data, offset + 4)[0]
        payload_offset = offset + 8
        end = payload_offset + size
        if end > len(data):
            name = section_id.decode("ascii", errors="replace")
            raise PackageError(f"truncated {name} section")
        if section_id not in allowed_ids:
            raise PackageError(f"unknown section {section_id!r}")
        sections.append((section_id, data[payload_offset:end], payload_offset))
        offset = end
    if offset != len(data):
        raise PackageError("section count leaves trailing package bytes")
    return sections


def decode_package(data: bytes) -> DecodedPackage:
    if len(data) < HEADER_SIZE:
        raise PackageError("package is shorter than its 20-byte header")
    if len(data) > MAX_PACKAGE_BYTES:
        raise PackageError("package exceeds 16 MiB")
    magic, major, minor, total_size, section_count, expected_crc = struct.unpack_from(
        "<4sHHIII", data, 0
    )
    if magic != MAGIC:
        raise PackageError("bad package magic")
    if major not in (VERSION_MAJOR, HARDWARE_VERSION_MAJOR) or minor != VERSION_MINOR:
        raise PackageError(f"unsupported package version {major}.{minor}")
    if total_size != len(data):
        raise PackageError(f"package header size {total_size} does not equal byte length {len(data)}")
    zero_crc = data[:16] + b"\x00\x00\x00\x00" + data[20:]
    actual_crc = zlib.crc32(zero_crc) & 0xFFFFFFFF
    if actual_crc != expected_crc:
        raise PackageError(f"CRC32 mismatch: header has {expected_crc:08x}, calculated {actual_crc:08x}")
    if section_count < 4:
        raise PackageError("package requires STRS, STAT, CODE, and at least one PAGE")
    sections = _read_sections(data, major, section_count)
    state_index = 1
    code_index = 2
    capabilities: tuple[DecodedCapability, ...] = ()
    permissions: tuple[str, ...] = ()
    if major == HARDWARE_VERSION_MAJOR:
        cursor = 1
        if sections[cursor][0] == b"CAPS":
            capabilities = _capabilities(*sections[cursor][1:])
            cursor += 1
        if sections[cursor][0] == b"PERM":
            permissions = _permissions(*sections[cursor][1:])
            cursor += 1
        state_index = cursor
        code_index = cursor + 1
    if code_index >= len(sections) or sections[0][0] != b"STRS":
        raise PackageError("package sections must begin with STRS")
    if (sections[state_index][0], sections[code_index][0]) != (b"STAT", b"CODE"):
        expected = "STRS, [CAPS], [PERM], STAT, CODE" if major == HARDWARE_VERSION_MAJOR else "STRS, STAT, CODE"
        raise PackageError(f"sections must begin with {expected} in that order")
    for section_id in (b"STRS", b"STAT", b"CODE"):
        if sum(section[0] == section_id for section in sections) != 1:
            raise PackageError(f"exactly one {section_id.decode()} section is required")
    page_sections: list[tuple[bytes, int]] = []
    asset_sections: list[tuple[bytes, int]] = []
    in_assets = False
    for section_id, payload, payload_offset in sections[code_index + 1 :]:
        if section_id == b"PAGE":
            if in_assets:
                raise PackageError("PAGE sections must precede DATA sections")
            page_sections.append((payload, payload_offset))
        elif section_id == b"DATA":
            in_assets = True
            asset_sections.append((payload, payload_offset))
        else:
            raise PackageError(f"misordered {section_id!r} section")
    strings = _strings(*sections[0][1:])
    states = _states(*sections[state_index][1:], strings, major)
    functions = _functions(*sections[code_index][1:], strings, states, major)
    pages = _pages(page_sections, strings, states, functions)
    assets = _assets(asset_sections, strings)
    asset_names = {asset.name for asset in assets}

    def check_images(node: DecodedNode) -> None:
        if node.kind == 9:
            source = node.properties.get("src")
            if source is None or source.kind != 0 or source.value not in asset_names:
                raise PackageError("image src must name a DATA asset")
        for child in node.children:
            check_images(child)

    for page in pages:
        check_images(page.root)
    return DecodedPackage(
        strings,
        states,
        functions,
        pages,
        assets,
        expected_crc,
        major,
        minor,
        capabilities,
        permissions,
    )
def read_package(path: str | Path) -> DecodedPackage:
    try:
        data = Path(path).read_bytes()
    except OSError as error:
        raise PackageError(f"{path}: {error}") from error
    try:
        return decode_package(data)
    except PackageError as error:
        raise PackageError(f"{path}: {error}") from error


def count_nodes(node: DecodedNode) -> int:
    return 1 + sum(count_nodes(child) for child in node.children)


def inspect_summary(path: str | Path, package: DecodedPackage) -> str:
    lines = [
        f"Package: {path}",
        f"Version: {package.major}.{package.minor}",
        f"Strings: {len(package.strings)}",
        f"State fields: {len(package.states)}",
    ]
    for index, state in enumerate(package.states):
        lines.append(f"  [{index}] {state.name}: {state.type.label} = {state.value!r}")
    lines.append(f"Functions: {len(package.functions)}")
    for index, function in enumerate(package.functions):
        lines.append(
            f"  [{index}] {function.name}: {function.parameter_count} parameter"
            f"{'s' if function.parameter_count != 1 else ''}, {function.local_count} locals, "
            f"{len(function.constants)} constants, {len(function.code)} code bytes"
        )
    lines.append(f"Pages: {len(package.pages)}")
    for index, page in enumerate(package.pages):
        lines.append(f"  [{index}] {page.name}: {count_nodes(page.root)} nodes")
    lines.append(f"Assets: {len(package.assets)}")
    for asset in package.assets:
        lines.append(
            f"  {asset.name}: {asset.width}x{asset.height}, RGB565, {len(asset.pixels)} bytes"
        )
    lines.append(f"CRC32: {package.crc32:08x}")
    return "\n".join(lines)
