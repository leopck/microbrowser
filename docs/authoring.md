# Microbrowser authoring guide

Microbrowser applications are well-formed UTF-8 XML documents with an `.mb`
extension. `tools/mbc` compiles the application, PNG assets, and scripts into a
version-1 `.mbp` package. The runtime does not parse XML, CSS, scripts, or PNG
files.

Run the compiler from any working directory:

```sh
python3 /path/to/microbrowser/tools/mbc build app.mb
python3 /path/to/microbrowser/tools/mbc build app.mb -o app.mbp
python3 /path/to/microbrowser/tools/mbc inspect app.mbp
python3 /path/to/microbrowser/tools/mbc run app.mb -- --size 480x320
```

The default output path is the input path with its suffix changed to `.mbp`.
`run` passes arguments after `--` to the desktop program. Source and asset paths
are resolved from the `.mb` file, not from the current working directory.
Diagnostics use `path:line:column: message` and a failed build does not produce a
package.

## Document structure

```xml
<microbrowser name="Home">
  <state>
    <field name="temperature" type="f32" value="22.4"/>
    <field name="room" type="string" value="Living room"/>
    <field name="enabled" type="bool" value="true"/>
  </state>
  <style>
    .card { background: #182232; radius: 12; padding: 12; gap: 8; }
  </style>
  <asset name="leaf" src="assets/leaf.png" resize="32x32"/>
  <page name="dashboard">
    <column id="root" class="card" on_load="start()">
      <text text="{state.temperature}" format="%.1f °C"/>
      <button text="Settings" on_press="open_settings()"/>
    </column>
  </page>
  <script><![CDATA[
    fn start() {
      every(1000, "refresh");
    }
    fn refresh() {
      log("refresh complete");
    }
    fn open_settings() {
      go("settings");
    }
  ]]></script>
</microbrowser>
```

`<microbrowser>` has a nonempty `name`. It may contain one `<state>`, one
`<style>`, one `<script>`, one or more `<page>` elements, and zero or more
`<asset>` elements. Assets must precede all pages. There is at most one root
element per page; the first page is the initial page. Page names are nonempty
and unique. Whitespace between elements is ignored; other character content and
nested elements in `<state>`, `<style>`, or `<script>` are rejected. Use CDATA
for scripts so operators such as `<` and `&&` need no XML escaping.

State fields have unique C-like names, at most 256 fields per package. `type` is
`f32`, `bool`, or `string`; values use finite decimal numbers, `true`/`false`, or
literal text respectively. XML character references are decoded before UTF-8
strings are stored. A script refers to state as `state.field`.

Assets have unique, nonempty names. `src` is required and names a PNG file
relative to the `.mb` file. `resize="WIDTHxHEIGHT"` is optional; either dimension
is 1 through 2048. Without it, the source dimensions are used and must be in the
same range. The decoder supports non-interlaced PNG grayscale, grayscale-alpha,
RGB, RGBA, and palette images at their standard bit depths, including `tRNS`
transparency. Interlaced PNG is rejected as unsupported. Conversion discards
alpha and writes little-endian RGB565 pixels. Resizing is nearest-neighbor;
output pixel `(x,y)` samples source pixel `(x * source_width / output_width,
y * source_height / output_height)` using integer floor division.

## Style resolution

A stylesheet contains only rules of the form `.class { property: value; }`.
Class names begin with a letter or underscore, followed by letters, digits,
underscores, or hyphens. Every declaration, including the last, ends in `;`.
An element's `class` attribute contains whitespace-separated class names.
Matching rules apply in stylesheet order: a later declaration of a property
replaces an earlier one. The order of names in `class` does not affect priority.
Whitespace around declarations and values is ignored.

`style="property: value; ..."` has higher priority than classes. Direct XML
attributes have higher priority still. Property keywords in `style` use CSS
hyphens. XML event attributes conventionally use underscores; `_` and `-` are
accepted interchangeably in property names and normalize to the same property.
`text`, literal `value`, and `bind` form one content choice. Before tiers are
merged, a state-valued `text` or `value` normalizes to `bind`, and literal
`value` normalizes to `text`. A higher-priority content choice replaces the
lower-priority choice even when it uses a different source alias.
Duplicate properties in one declaration block or duplicate direct attributes
are errors.

### Properties and defaults

Lengths `width` and `height` are `auto` (the default), `grow`, a nonnegative
number of pixels, or a percentage from `0%` through `100%`. Pixel lengths are
encoded as numbers; `auto`, `grow`, and percentages remain strings in PAGE.
`padding`, `gap`, `radius`, and integer `font-size` are nonnegative pixels;
their defaults are `0`, `0`, `0`, and `1`. `font-size` must be at least `1` when
specified. `x` and `y` are signed integer coordinates, defaulting to `0`.
`position` is `static` (default) or `absolute`. `scroll` is `false` (default) or
`true`.

`align` is `start`, `center`, `end`, or `stretch` (default). `justify` is
`start` (default), `center`, `end`, or `space-between`. `color` defaults to the
named `ink` color; `background` and `border` default to `transparent`.

Colors are `#RGB`, `#RGBA`, `#RRGGBB`, `#RRGGBBAA`, or one of `transparent`,
`black`, `white`, `ink`, `muted`, `background`, `surface`, `border`, `accent`,
`blue`, `warning`, and `error`. Short hex digits are doubled. PAGE colors are
unsigned `0xRRGGBBAA` values.

All elements accept `id`, `class`, `style`, the layout/paint properties above,
and `animate`. IDs are nonempty and unique within their page. `animate` is
`fade`, `pulse`, or `slide`, optionally followed by `:` and a positive integer
duration in milliseconds (for example `fade:250`). It is stored as a string.

Content and behavior properties are:

| Element | Accepted properties and requirements |
|---|---|
| `view`, `row`, `column`, `stack` | Element children. `on_load` is accepted on the page root only. |
| `text` | `text` or `value`, `bind`, and `format`. A binding is optional. `text="{state.field}"`, `value="{state.field}"`, and `bind="{state.field}"` are equivalent. |
| `button` | Optional literal `text`; `on_press`. No children. |
| `input` | Exactly one string-state binding through `bind` or `value`; optional `placeholder`, numeric `max-length` (0 through 65535, default 64), `on_change`, and `on_submit`. |
| `slider` | Exactly one numeric-state binding through `bind` or `value`; numeric `min` (default 0), `max` (default 100), and `step` (default 1), with `min <= max` and `step > 0`; optional `on_change`. |
| `progress` | Exactly one numeric-state binding through `bind` or `value`; numeric `min` and `max`, with `min < max`; no children. |
| `chart` | Optional `samples`, a comma-separated list of at most 64 finite numbers. Numeric `min` and `max` default to 0 and 100, with `min < max`. Give the chart an `id` when calling `chart_push`. |
| `image` | Required `src`, naming a declared `<asset>` (not a file path). |
| `spacer` | Only common properties. |

Only `view`, `row`, `column`, and `stack` have children. A button's `text`
defaults to an empty string. Text without a literal or binding also defaults to
an empty string. Literal `text` and `value` preserve UTF-8. `placeholder` and
`format` default to empty strings, and chart `samples` defaults to an empty
series. Event attributes contain exactly one no-argument call, such as
`on_press="save_target()"`, and must name a declared function.

A `format` applies only to bound text. It contains literal text and exactly one
of `%f`, `%.Nf` (where `N` is 0 through 6), `%d`, or `%s`. `%%` is accepted as a
literal percent sign. Other flags, widths, precisions, conversions, and multiple
conversions are rejected. Number states require a numeric conversion; string
states require `%s`. A boolean binding cannot have a `format`.

## PAGE property encoding

The compiler merges aliases and writes canonical property names. `value` never
appears in PAGE: a literal becomes `text`, and a state value becomes `bind`.
Hyphenated event aliases become `on_press`, `on_change`, `on_submit`, and
`on_load`. Other names retain their hyphenated spelling (`font-size`,
`max-length`).

| Canonical property | PAGE value kind / encoding |
|---|---|
| `id`, `text`, `format`, `placeholder`, `src`, `animate` | string |
| `samples` | string containing canonical comma-separated numbers, or empty |
| `width`, `height` | number for fixed pixels; string for `auto`, `grow`, or `N%` |
| `padding`, `gap`, `font-size`, `radius`, `x`, `y`, `min`, `max`, `step`, `max-length` | binary32 number |
| `background`, `color`, `border` | 32-bit `0xRRGGBBAA` color |
| `align`, `justify`, `position` | string |
| `scroll` | boolean |
| `bind` | 32-bit state index |
| `on_press`, `on_change`, `on_submit`, `on_load` | binary32 number containing the zero-based function index |

The compiler emits only properties explicitly present after style resolution;
the runtime supplies the defaults listed above. `bind` is the single state index
for text, input, slider, and progress. There can be at most 40 emitted properties
per node, 64 direct children per element, 32 elements of page nesting, 32 pages,
and 2,048 nodes per package.

## Script language

Scripts contain C-like function declarations:

```c
fn clamp_target(value) {
  if (value < 16) {
    return 16;
  } else if (value > 28) {
    return 28;
  }
  return value;
}
```

There are at most 128 uniquely named functions. Each has at most 32 local
variables including parameters. Parameters and `let` names are unique across an
entire function; locals declared in blocks remain function-scoped. Declare a
local with `let name = expression;`. Assign with `name = expression;` or
`state.field = expression;`. Use `if (...) statement`, optional `else`,
`while (...) statement`, braces, `break;`, `continue;`, `return;`, and
`return expression;`. Calls and other expressions may be expression statements.
Semicolons terminate `let`, assignments, returns, loop-control statements, and
expression statements; no semicolon follows a block.

Expressions support grouping, function calls, literals, local names,
`state.field`, unary `!` and `-`, `* /`, `+ -`, `< <= > >=`, `== !=`, `&&`, and
`||`, listed from tighter to looser binding. `&&` and `||` short-circuit.
Conditions require booleans. Arithmetic is numeric except that `+` also
concatenates two strings; comparisons require numbers and equality requires
matching types. Division by zero is a runtime script error.

Number literals are finite decimal forms such as `1`, `2.5`, `.5`, `1.`, and
`1e-3`; `nan`, `inf`, and hex numbers are not accepted. Strings use single or
double quotes and preserve UTF-8. Escapes are `\n`, `\r`, `\t`, `\b`, `\f`,
`\v`, `\\`, `\"`, `\'`, `\xHH`, `\uHHHH`, and `\UHHHHHHHH` with the usual
hexadecimal digits. Unescaped control characters, unknown escapes, and invalid
Unicode escapes are errors. Embedded NUL (`\x00` or `\u0000`) is also forbidden
because the package string table is NUL-terminated. C++-style `//` line comments
and `/* ... */` block comments are accepted between tokens.

The compiler checks all call arities, statically known argument and assignment
types, duplicate names, undefined names, and incompatible return categories.
A function returning a value must return a value on every path. Functions vary
local slot indices explicitly in bytecode; scripts do not access those indices.
Calls may be forward references or recursive. Runtime checks and the per-event
20,000-instruction budget still apply, including loops defending against
unbounded work in downloaded packages.

Builtins use the names, signatures, and numeric IDs in `package-format.md`.
Page and timer callback names are strings: `go("settings")`, `back()`,
`after(250, "refresh")`, and `every(1000, "refresh")`. Timer strings are exact
function names without parentheses. `animate` takes an element ID, one of
`scale`, `opacity`, or `offset_y`, a number, and a duration in milliseconds.
Timers belong to the loaded package. Local page navigation does not invalidate
their IDs; loading a new package discards all timers created by the old one.
`chart_push` takes an element ID and number. Persistence keys are
strings, and `save_*`/`load_*` pairs must use matching value types. `fetch` and
`fetch_json` are optional; applications that must work offline can instead use
timers and local state. `log` is for short application-safe diagnostics.

## Validation and inspection

`inspect` validates the envelope, checksum, string table, state and function
references, constants, instruction boundaries and jumps, PAGE structure, and
RGB565 asset sizes before printing counts and functions, pages, and assets.
Packages are limited to 16 MiB. Compilation also validates the completed bytes,
so a successful `build` is a well-formed version-1 package even before the
runtime performs its independent checks.
