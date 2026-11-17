# Microbrowser package format, version 2

Integers are little-endian. A string ID is a zero-based `uint32_t` index into
that package's `STRS` section. A missing state, function, or string reference
must be rejected by the reader. Sizes are byte counts. A reader must reject
truncation, section overflow, excessive nesting, trailing bytes in a section,
invalid UTF-8 in strings, unknown value kinds, and a package size above 16 MiB.

The source authoring language is documented in `authoring.md`.

## Envelope

The 20-byte header is:

| Field | Encoding |
|---|---|
| Magic | Four bytes `MBPK` |
| Major version | `uint16_t`, value 2 |
| Minor version | `uint16_t`, value 0 |
| Total package size | `uint32_t`, including this header |
| Section count | `uint32_t` |
| CRC32 | `uint32_t`, IEEE polynomial; calculate over the entire package with these four bytes set to zero |

Each section starts with four identifier bytes followed by its payload size as
`uint32_t`. Exactly one `STRS`, `STAT`, and `CODE` section is required. One or
more `PAGE` sections and zero or more `DATA` sections follow. No other sections
are accepted in version 2. Section identifiers are `STRS`, `STAT`, `CODE`,
`PAGE`, and `DATA`.

## Strings: `STRS`

`uint32_t count`, followed by `count` `uint32_t` offsets, followed by
NUL-terminated UTF-8 strings. An offset is relative to the first byte after the
offset table. Every string must fit in the section; the final byte of the final
string is its NUL. Empty strings are valid.

## Shared state: `STAT`

`uint32_t count`, then for each state field a name string ID, a type byte, and
its initial value. Types and value tags are:

| Tag | Type | Value encoding |
|---|---|---|
| 0 | Number | IEEE-754 binary32 |
| 1 | Boolean | `uint8_t`, exactly 0 or 1 |
| 2 | String | String ID |

The declared type and encoded value kind must match. Names must be unique.
The maximum is 256 fields.

## Functions: `CODE`

`uint32_t function_count`, then each function as:

1. Name string ID
2. Parameter count (`uint16_t`)
3. Local-variable count, including parameters (`uint16_t`, maximum 32)
4. Code size in bytes (`uint32_t`, maximum 65,535)
5. Constant count (`uint32_t`, maximum 256)
6. Constants, each a type/value pair as defined for `STAT`
7. Exactly `code_size` code bytes

The maximum is 128 named functions. Names must be unique. Even event handlers
with no arguments are named functions (`on_press="increase()"` refers to one).
Function calls pass arguments in source order.

### Instructions

An unknown opcode or a truncated immediate is invalid. Jump offsets are signed
16-bit counts relative to the instruction after the complete jump instruction;
their targets must land on an instruction boundary within the same function.
`PUSH_CONST`, `LOAD_STATE`, `STORE_STATE`, `LOAD_LOCAL`, and `STORE_LOCAL` have
one `uint16_t` index immediate. `CALL` has a `uint16_t` function index and a
`uint8_t` argument count. `BUILTIN` has a `uint8_t` builtin ID and a `uint8_t`
argument count. `PUSH_BOOL` has one `uint8_t` immediate, exactly zero or one.

| Opcode | Instruction |
|---|---|
| 1 | `PUSH_CONST` |
| 2 | `PUSH_BOOL` |
| 3 | `LOAD_STATE` |
| 4 | `STORE_STATE` (pops value, pushes no result) |
| 5 | `LOAD_LOCAL` |
| 6 | `STORE_LOCAL` (pops value, pushes no result) |
| 7 | `POP` |
| 8 | `ADD` |
| 9 | `SUBTRACT` |
| 10 | `MULTIPLY` |
| 11 | `DIVIDE` (division by zero is a script error) |
| 12 | `NEGATE` |
| 13 | `EQUAL` |
| 14 | `NOT_EQUAL` |
| 15 | `LESS` |
| 16 | `LESS_EQUAL` |
| 17 | `GREATER` |
| 18 | `GREATER_EQUAL` |
| 19 | `NOT` |
| 20 | `JUMP` |
| 21 | `JUMP_FALSE` (pops the condition) |
| 22 | `CALL` |
| 23 | `BUILTIN` |
| 24 | `RETURN` (pops one value, or returns void if the operand stack is empty) |

The reader or interpreter must bound evaluation at 64 operand values, 16 active
function calls, 32 local variables per function, and 20,000 instructions for
one event including nested calls. Conditions require booleans, not generic
truthiness. Arithmetic requires numbers, except `ADD`, which can concatenate
two strings. Equality requires matching types. Script assignments do not
change a state's declared type.

### Builtins

Unless otherwise stated, numbers are binary32 values. `BUILTIN` pops its
arguments and pushes exactly one result. A failed operation raises a script
error instead of returning a sentinel.

| ID | Name and arguments | Result |
|---|---|---|
| 0 | `min(number, number)` | Number |
| 1 | `max(number, number)` | Number |
| 2 | `clamp(number, number, number)` | Number |
| 3 | `abs(number)` | Number |
| 4 | `round(number)` | Number |
| 5 | `floor(number)` | Number |
| 6 | `len(string)` | Number |
| 7 | `go(page_name)` | Void; later handlers in the current event are cancelled |
| 8 | `back()` | Void; same event-cancellation behavior as `go` |
| 9 | `animate(node_id, property_name, number, duration_ms)` | Void; properties `scale`, `opacity`, `offset_y` |
| 10 | `after(delay_ms, function_name)` | Number timer ID |
| 11 | `every(interval_ms, function_name)` | Number timer ID |
| 12 | `cancel(timer_id)` | Void |
| 13 | `save_number(key, number)` | Void |
| 14 | `load_number(key, fallback)` | Number |
| 15 | `save_bool(key, boolean)` | Void |
| 16 | `load_bool(key, fallback)` | Boolean |
| 17 | `fetch(url, state_name)` | Number request ID; asynchronously stores the response body in the named string state |
| 18 | `fetch_json(url, path, state_name)` | Number request ID; asynchronously stores a scalar from a JSON response in the named state |
| 19 | `save_string(key, string)` | Void |
| 20 | `load_string(key, fallback)` | String |
| 21 | `sin(number)` | Number; argument in radians |
| 22 | `cos(number)` | Number; argument in radians |
| 23 | `string(number)` | String |
| 24 | `log(string)` | Void; sends one application-safe diagnostic to the platform logger |
| 25 | `chart_push(node_id, number)` | Void; appends to a chart's bounded series and invalidates its region |
| 26 | `json_number(json, path)` | Number extracted from JSON |
| 27 | `json_string(json, path)` | String extracted from JSON |
| 28 | `json_bool(json, path)` | Boolean extracted from JSON |

`fetch` and its variants limit the state name string to 255 bytes and URL to
2,047 bytes. Their result indicates acceptance into the bounded request queue,
not HTTP success. Network errors produce a descriptive runtime diagnostic and
leave the previous state untouched.

JSON paths contain dot-separated object keys and zero-based array indexes, for
example `current.temperature` or `rows.0.value`. A missing key or a scalar of
the wrong type is an error. The JSON reader rejects trailing data, deep
nesting above 16 levels, malformed escapes and numbers, and documents over
64 KiB. It does not silently convert strings to numbers.

## Pages: `PAGE`

A page name string ID followed by one root node. Page names must be unique;
the first page is the initial page. The maximum is 32 pages and 2,048 elements
across a package.

Each node has:

1. Kind (`uint8_t`)
2. Flags (`uint8_t`, reserved and zero in version 2)
3. Property count (`uint16_t`, maximum 40)
4. Property records
5. Child count (`uint32_t`)
6. That many complete child nodes

Each property is a name string ID, a value-kind byte, and its payload. Value
kinds are: 0 string ID, 1 binary32 number, 2 boolean byte, 3 32-bit color, and
4 32-bit state index. Colors are `0xRRGGBBAA`; every named color is resolved by
the compiler. Duplicate properties and unknown node or value kinds are errors.
Unknown property names are errors.

Node kinds and their meaningful content are:

| Kind | Element |
|---|---|
| 0 | `view`, a neutral container |
| 1 | `row`, horizontal flex container |
| 2 | `column`, vertical flex container |
| 3 | `text`, displays `text` or its bound state |
| 4 | `button`, displays `text` and dispatches `on_press` |
| 5 | `input`, edits one bound string state |
| 6 | `slider`, edits one bound numeric state |
| 7 | `progress`, displays one bound numeric state |
| 8 | `chart`, draws a bounded scalar time series |
| 9 | `image`, displays the predecoded asset named by `src` |
| 10 | `spacer`, consumes available space |
| 11 | `stack`, overlays its children |

Only containers and `stack` may have element children. The UI compiler merges
CSS-like classes and inline properties before writing these records. Integer
coordinates, bounds, and pixels are used by layout. Supported properties and
defaults are listed in `authoring.md`.

Fixed `width` and `height` values may range from 0 through 16,384 pixels.
Absolute `x` and `y` offsets may range from -16,384 through 16,384 pixels.
Non-finite lengths and offsets are errors. Zero fixed dimensions are valid.

## Typography

Fonts are built into the renderer rather than packaged separately. Inter glyph
advance, kerning, and four-bit antialiased coverage are generated at authoring
time into fixed C++ tables. The runtime performs table lookup and RGB565 pixel
blending; it does not parse a TTF or call a platform font library during drawing.
Supported pixel sizes are `12`, `14`, `16`, `20`, `28`, and `36`, with weights
`400` and `600`. Glyph selection and validation reject other sizes and weights.
The encoded `font-size` and `font-weight` properties are binary32 numbers.
`font-size` changed from bitmap scale to pixels in this version; packages from
version 1 must be regenerated to avoid ambiguous layout semantics.

## Images: `DATA`

One asset per section. Payload is name string ID, `uint16_t` width,
`uint16_t` height, `uint32_t` data size, then pixels. Width and height must be
nonzero, dimensions at most 2,048, and data size must equal `width * height * 2`.
Pixels are RGB565 in little-endian, row-major order. The host compiler performs
PNG decoding, color conversion, and any requested resizing. The runtime only
samples these pixels.

## Limits

Readers enforce limits before indexing or allocating wherever practical. All
arithmetic on sizes and rectangle extents is checked. Resource exhaustion is a
package error; malformed input cannot read or write outside a buffer.
