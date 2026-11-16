from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from xml.parsers import expat

from .errors import CompileError

MAX_SOURCE_BYTES = 16 * 1024 * 1024


@dataclass(slots=True)
class XmlElement:
    path: str
    name: str
    attributes: dict[str, str]
    line: int
    column: int
    children: list[XmlElement] = field(default_factory=list)
    parts: list[str] = field(default_factory=list)
    tail: str = ""
    text_line: int = 1
    text_column: int = 1

    @property
    def text(self) -> str:
        return "".join(self.parts)

    def location(self) -> tuple[str, int, int]:
        return self.path, self.line, self.column

    def error(self, message: str) -> CompileError:
        return CompileError(self.path, self.line, self.column, message)

    def text_error(self, offset: int, message: str) -> CompileError:
        prefix = self.text[: max(0, min(offset, len(self.text)))]
        newlines = prefix.count("\n")
        if newlines:
            line = self.text_line + newlines
            column = len(prefix.rsplit("\n", 1)[1]) + 1
        else:
            line = self.text_line
            column = self.text_column + len(prefix)
        return CompileError(self.path, line, column, message)


class _TreeBuilder:
    def __init__(self, path: str) -> None:
        self.path = path
        self.parser = expat.ParserCreate()
        self.stack: list[XmlElement] = []
        self.root: XmlElement | None = None
        self.parser.StartElementHandler = self.start
        self.parser.EndElementHandler = self.end
        self.parser.CharacterDataHandler = self.characters
        self.parser.StartDoctypeDeclHandler = self.doctype
        self.parser.ExternalEntityRefHandler = self.external_entity
        self.last_closed: XmlElement | None = None

    def start(self, name: str, attributes: dict[str, str]) -> None:
        self.last_closed = None
        element = XmlElement(
            path=self.path,
            name=name,
            attributes=dict(attributes),
            line=self.parser.CurrentLineNumber,
            column=self.parser.CurrentColumnNumber + 1,
            text_line=self.parser.CurrentLineNumber,
            text_column=self.parser.CurrentColumnNumber + 1,
        )
        if self.stack:
            self.stack[-1].children.append(element)
        elif self.root is not None:
            raise element.error("multiple XML root elements")
        else:
            self.root = element
        self.stack.append(element)

    def end(self, _name: str) -> None:
        self.last_closed = self.stack.pop()

    def characters(self, text: str) -> None:
        if not self.stack:
            return
        if self.last_closed is not None:
            self.last_closed.tail += text
            return
        current = self.stack[-1]
        if not current.parts:
            current.text_line = self.parser.CurrentLineNumber
            current.text_column = self.parser.CurrentColumnNumber + 1
        current.parts.append(text)

    def doctype(self, *_args: object) -> None:
        raise CompileError(
            self.path,
            self.parser.CurrentLineNumber,
            self.parser.CurrentColumnNumber + 1,
            "DOCTYPE and external entities are not allowed",
        )

    def external_entity(self, *_args: object) -> int:
        raise CompileError(
            self.path,
            self.parser.CurrentLineNumber,
            self.parser.CurrentColumnNumber + 1,
            "external XML entities are not allowed",
        )


def parse_xml_bytes(data: bytes, path: str | Path) -> XmlElement:
    source_path = str(path)
    if len(data) > MAX_SOURCE_BYTES:
        raise CompileError(source_path, 1, 1, "source exceeds 16 MiB")
    try:
        source = data.decode("utf-8-sig")
    except UnicodeDecodeError as error:
        prefix = data[: error.start].decode("utf-8", errors="ignore")
        line = prefix.count("\n") + 1
        column = len(prefix.rsplit("\n", 1)[-1]) + 1
        raise CompileError(source_path, line, column, "source is not valid UTF-8") from error
    builder = _TreeBuilder(source_path)
    try:
        builder.parser.Parse(source, True)
    except CompileError:
        raise
    except expat.ExpatError as error:
        raise CompileError(source_path, error.lineno, error.offset + 1, expat.errors.messages[error.code]) from error
    if builder.root is None:
        raise CompileError(source_path, 1, 1, "document has no root element")
    return builder.root


def parse_xml_file(path: str | Path) -> XmlElement:
    source_path = Path(path)
    try:
        data = source_path.read_bytes()
    except OSError as error:
        raise CompileError(source_path, 1, 1, str(error)) from error
    return parse_xml_bytes(data, source_path)
