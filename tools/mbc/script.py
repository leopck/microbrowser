from __future__ import annotations

import re
from collections.abc import Iterable
import struct
from dataclasses import dataclass
from enum import IntEnum
from pathlib import Path

from .device_api import (
    Argument,
    CAPABILITY_IDS,
    CAPABILITY_NAMES,
    DeviceOperation,
    HardwareDeclaration,
    IO_CALLS,
    IoPrototype,
    METADATA_FIELDS,
    MAX_BYTES_PER_BUFFER,
    NativeOperation,
    ScriptValue,
    ValueType,
)
from .errors import CompileError

MAX_FUNCTIONS = 128
MAX_LOCALS = 32
MAX_CONSTANTS = 256
MAX_CODE_BYTES = 65_535
INT64_MIN = -(1 << 63)
INT64_MAX = (1 << 63) - 1
IDENTIFIER_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
NUMBER_RE = re.compile(r"(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?")
INTEGER_LITERAL_RE = re.compile(r"([0-9]+)i64(?![A-Za-z0-9_])")
DECIMAL_I64_RE = re.compile(r"-?[0-9]+")
RESERVED = {
    "fn",
    "let",
    "if",
    "else",
    "while",
    "return",
    "break",
    "continue",
    "true",
    "false",
    "state",
    "io",
}


@dataclass(frozen=True, slots=True)
class SourcePoint:
    path: str
    line: int
    column: int

    def error(self, message: str) -> CompileError:
        return CompileError(self.path, self.line, self.column, message)



@dataclass(frozen=True, slots=True)
class Constant:
    type: ValueType
    value: ScriptValue


@dataclass(frozen=True, slots=True)
class Builtin:
    id: int
    arguments: tuple[ValueType, ...]
    result: ValueType


BUILTINS = {
    "min": Builtin(0, (ValueType.NUMBER, ValueType.NUMBER), ValueType.NUMBER),
    "max": Builtin(1, (ValueType.NUMBER, ValueType.NUMBER), ValueType.NUMBER),
    "clamp": Builtin(
        2, (ValueType.NUMBER, ValueType.NUMBER, ValueType.NUMBER), ValueType.NUMBER
    ),
    "abs": Builtin(3, (ValueType.NUMBER,), ValueType.NUMBER),
    "round": Builtin(4, (ValueType.NUMBER,), ValueType.NUMBER),
    "floor": Builtin(5, (ValueType.NUMBER,), ValueType.NUMBER),
    "len": Builtin(6, (ValueType.STRING,), ValueType.NUMBER),
    "go": Builtin(7, (ValueType.STRING,), ValueType.VOID),
    "back": Builtin(8, (), ValueType.VOID),
    "animate": Builtin(
        9,
        (ValueType.STRING, ValueType.STRING, ValueType.NUMBER, ValueType.NUMBER),
        ValueType.VOID,
    ),
    "after": Builtin(10, (ValueType.NUMBER, ValueType.STRING), ValueType.NUMBER),
    "every": Builtin(11, (ValueType.NUMBER, ValueType.STRING), ValueType.NUMBER),
    "cancel": Builtin(12, (ValueType.NUMBER,), ValueType.VOID),
    "save_number": Builtin(13, (ValueType.STRING, ValueType.NUMBER), ValueType.VOID),
    "load_number": Builtin(14, (ValueType.STRING, ValueType.NUMBER), ValueType.NUMBER),
    "save_bool": Builtin(15, (ValueType.STRING, ValueType.BOOLEAN), ValueType.VOID),
    "load_bool": Builtin(16, (ValueType.STRING, ValueType.BOOLEAN), ValueType.BOOLEAN),
    "fetch": Builtin(17, (ValueType.STRING, ValueType.STRING), ValueType.NUMBER),
    "fetch_json": Builtin(
        18, (ValueType.STRING, ValueType.STRING, ValueType.STRING), ValueType.NUMBER
    ),
    "save_string": Builtin(19, (ValueType.STRING, ValueType.STRING), ValueType.VOID),
    "load_string": Builtin(20, (ValueType.STRING, ValueType.STRING), ValueType.STRING),
    "sin": Builtin(21, (ValueType.NUMBER,), ValueType.NUMBER),
    "cos": Builtin(22, (ValueType.NUMBER,), ValueType.NUMBER),
    "string": Builtin(23, (ValueType.NUMBER,), ValueType.STRING),
    "log": Builtin(24, (ValueType.STRING,), ValueType.VOID),
    "chart_push": Builtin(25, (ValueType.STRING, ValueType.NUMBER), ValueType.VOID),
    "json_number": Builtin(26, (ValueType.STRING, ValueType.STRING), ValueType.NUMBER),
    "json_string": Builtin(27, (ValueType.STRING, ValueType.STRING), ValueType.STRING),
    "json_bool": Builtin(28, (ValueType.STRING, ValueType.STRING), ValueType.BOOLEAN),
}


class Opcode(IntEnum):
    PUSH_CONST = 1
    PUSH_BOOL = 2
    LOAD_STATE = 3
    STORE_STATE = 4
    LOAD_LOCAL = 5
    STORE_LOCAL = 6
    POP = 7
    ADD = 8
    SUBTRACT = 9
    MULTIPLY = 10
    DIVIDE = 11
    NEGATE = 12
    EQUAL = 13
    NOT_EQUAL = 14
    LESS = 15
    LESS_EQUAL = 16
    GREATER = 17
    GREATER_EQUAL = 18
    NOT = 19
    JUMP = 20
    JUMP_FALSE = 21
    CALL = 22
    BUILTIN = 23
    RETURN = 24
    DEVICE_CALL = 25


@dataclass(frozen=True, slots=True)
class Token:
    kind: str
    text: str
    value: float | str | bool | int | None
    point: SourcePoint


class Lexer:
    def __init__(self, text: str, path: str | Path, line: int, column: int) -> None:
        self.text = text
        self.path = str(path)
        self.line = line
        self.column = column
        self.index = 0

    def point(self) -> SourcePoint:
        return SourcePoint(self.path, self.line, self.column)

    def error(self, message: str, point: SourcePoint | None = None) -> CompileError:
        return (point or self.point()).error(message)

    def advance(self) -> str:
        char = self.text[self.index]
        self.index += 1
        if char == "\n":
            self.line += 1
            self.column = 1
        else:
            self.column += 1
        return char

    def tokens(self) -> list[Token]:
        result: list[Token] = []
        while True:
            self._skip_space_and_comments()
            if self.index == len(self.text):
                result.append(Token("eof", "", None, self.point()))
                return result
            result.append(self._token())

    def _skip_space_and_comments(self) -> None:
        while self.index < len(self.text):
            if self.text[self.index].isspace():
                self.advance()
                continue
            if self.text.startswith("//", self.index):
                while self.index < len(self.text) and self.text[self.index] != "\n":
                    self.advance()
                continue
            if self.text.startswith("/*", self.index):
                start = self.point()
                self.advance()
                self.advance()
                while self.index < len(self.text) and not self.text.startswith("*/", self.index):
                    self.advance()
                if self.index == len(self.text):
                    raise self.error("unterminated block comment", start)
                self.advance()
                self.advance()
                continue
            return

    def _token(self) -> Token:
        char = self.text[self.index]
        start = self.point()
        if char in "\"'":
            return Token("literal", "", self._string(char, start), start)
        integer_match = INTEGER_LITERAL_RE.match(self.text, self.index)
        if integer_match:
            raw = integer_match.group(0)
            magnitude = int(integer_match.group(1))
            if magnitude > INT64_MAX + 1:
                raise self.error("integer literal is outside the signed 64-bit range", start)
            for _ in raw:
                self.advance()
            return Token("integer", raw, magnitude, start)
        match = NUMBER_RE.match(self.text, self.index)
        if match:
            raw = match.group(0)
            for _ in raw:
                self.advance()
            try:
                value = float(raw)
            except ValueError as error:
                raise self.error("invalid number literal", start) from error
            if not _finite_f32(value):
                raise self.error("number literal is not a finite binary32 value", start)
            return Token("literal", raw, value, start)
        match = IDENTIFIER_RE.match(self.text, self.index)
        if match:
            raw = match.group(0)
            for _ in raw:
                self.advance()
            return Token("identifier", raw, raw, start)
        for symbol in ("||", "&&", "==", "!=", "<=", ">="):
            if self.text.startswith(symbol, self.index):
                self.advance()
                self.advance()
                return Token("symbol", symbol, symbol, start)
        if char in "+-*/!<>=(){};,. ":
            self.advance()
            return Token("symbol", char, char, start)
        raise self.error(f"unexpected character {char!r} in script", start)

    def _string(self, quote: str, start: SourcePoint) -> str:
        self.advance()
        result: list[str] = []
        while self.index < len(self.text):
            char_point = self.point()
            char = self.advance()
            if char == quote:
                value = "".join(result)
                if "\x00" in value:
                    raise self.error("string literal cannot contain NUL", start)
                return value
            if char == "\\":
                result.append(self._escape(char_point))
                continue
            if ord(char) < 0x20 or ord(char) == 0x7F:
                raise self.error("unescaped control character in string literal", char_point)
            result.append(char)
        raise self.error("unterminated string literal", start)

    def _escape(self, escape_point: SourcePoint) -> str:
        if self.index == len(self.text):
            raise self.error("unterminated string escape", escape_point)
        char = self.advance()
        simple = {
            "n": "\n",
            "r": "\r",
            "t": "\t",
            "b": "\b",
            "f": "\f",
            "v": "\v",
            "\\": "\\",
            "\"": "\"",
            "'": "'",
        }
        if char in simple:
            return simple[char]
        if char not in "xuU":
            raise self.error(f"unknown string escape \\{char}", escape_point)
        digits = {"x": 2, "u": 4, "U": 8}[char]
        value = self._hex_escape(digits, escape_point)
        if char == "u" and 0xD800 <= value <= 0xDBFF:
            if not self.text.startswith("\\u", self.index):
                raise self.error("high surrogate must be followed by a low-surrogate escape", escape_point)
            self.advance()
            self.advance()
            low = self._hex_escape(4, escape_point)
            if not 0xDC00 <= low <= 0xDFFF:
                raise self.error("high surrogate must be followed by a low-surrogate escape", escape_point)
            value = 0x10000 + ((value - 0xD800) << 10) + low - 0xDC00
        if 0xD800 <= value <= 0xDFFF or value > 0x10FFFF:
            raise self.error("escape is not a Unicode scalar value", escape_point)
        return chr(value)

    def _hex_escape(self, digits: int, escape_point: SourcePoint) -> int:
        raw = self.text[self.index : self.index + digits]
        if len(raw) != digits or any(char not in "0123456789abcdefABCDEF" for char in raw):
            raise self.error(f"expected {digits} hexadecimal digits in string escape", escape_point)
        for _ in raw:
            self.advance()
        return int(raw, 16)


@dataclass(slots=True)
class Number:
    token: Token
    value: float


@dataclass(slots=True)
class Integer:
    token: Token
    value: int


@dataclass(slots=True)
class String:
    token: Token
    value: str


@dataclass(slots=True)
class Boolean:
    token: Token
    value: bool


@dataclass(slots=True)
class Variable:
    token: Token
    name: str
    slot: int


@dataclass(slots=True)
class StateReference:
    token: Token
    name: str


@dataclass(slots=True)
class Call:
    token: Token
    name: str
    arguments: list[Expression]


@dataclass(slots=True)
class Unary:
    token: Token
    operator: str
    operand: Expression


@dataclass(slots=True)
class Binary:
    token: Token
    operator: str
    left: Expression
    right: Expression


Expression = Integer | Number | String | Boolean | Variable | StateReference | Call | Unary | Binary


@dataclass(slots=True)
class Let:
    token: Token
    name: str
    slot: int
    initial: Expression


@dataclass(slots=True)
class Assignment:
    token: Token
    target: Variable | StateReference
    value: Expression


@dataclass(slots=True)
class ExpressionStatement:
    token: Token
    expression: Expression


@dataclass(slots=True)
class Block:
    token: Token
    statements: list[Statement]


@dataclass(slots=True)
class If:
    token: Token
    condition: Expression
    then_branch: Statement
    else_branch: Statement | None


@dataclass(slots=True)
class While:
    token: Token
    condition: Expression
    body: Statement


@dataclass(slots=True)
class Return:
    token: Token
    value: Expression | None


@dataclass(slots=True)
class LoopControl:
    token: Token
    keyword: str


Statement = Let | Assignment | ExpressionStatement | Block | If | While | Return | LoopControl


@dataclass(slots=True)
class FunctionDecl:
    token: Token
    name: str
    parameters: list[str]
    local_names: list[str]
    body: Block
    returns_value: bool


@dataclass(slots=True)
class Program:
    functions: list[FunctionDecl]
    uses_device_calls: bool


class Parser:
    def __init__(self, tokens: list[Token]) -> None:
        self.tokens = tokens
        self.index = 0
        self.functions: list[FunctionDecl] = []
        self.function_names = set(BUILTINS)
        self.locals: dict[str, int] = {}
        self.local_names: list[str] = []
        self.parameters: list[str] = []
        self.loop_depth = 0
        self.returns_value = False
        self.uses_device_calls = False

    @property
    def token(self) -> Token:
        return self.tokens[self.index]

    def advance(self) -> Token:
        token = self.token
        self.index += 1
        return token

    def match(self, text: str) -> Token | None:
        if self.token.text == text:
            return self.advance()
        return None

    def expect(self, text: str) -> Token:
        if self.token.text != text:
            raise self.token.point.error(f"expected {text!r}, found {self.token.text or 'end of input'!r}")
        return self.advance()

    def name(self, purpose: str) -> Token:
        token = self.token
        if token.kind != "identifier" or token.text in RESERVED:
            raise token.point.error(f"expected {purpose}, found {token.text or 'end of input'!r}")
        self.advance()
        return token

    def parse(self) -> Program:
        while self.token.kind != "eof":
            self.functions.append(self.function())
            if len(self.functions) > MAX_FUNCTIONS:
                raise self.token.point.error("script exceeds 128 functions")
        return Program(self.functions, self.uses_device_calls)

    def function(self) -> FunctionDecl:
        self.expect("fn")
        name = self.name("function name")
        if name.text in self.function_names:
            raise name.point.error(f"duplicate function or builtin name {name.text!r}")
        self.function_names.add(name.text)
        self.locals = {}
        self.local_names = []
        self.parameters = []
        self.loop_depth = 0
        self.returns_value = False
        self.expect("(")
        if self.token.text != ")":
            while True:
                parameter = self.name("parameter name")
                self.declare_local(parameter, "parameter")
                self.parameters.append(parameter.text)
                if not self.match(","):
                    break
        self.expect(")")
        body = self.block()
        return FunctionDecl(
            token=name,
            name=name.text,
            parameters=self.parameters.copy(),
            local_names=self.local_names.copy(),
            body=body,
            returns_value=self.returns_value,
        )

    def declare_local(self, name: Token, purpose: str) -> None:
        if name.text in self.locals:
            raise name.point.error(f"duplicate {purpose} {name.text!r} in function")
        if len(self.local_names) >= MAX_LOCALS:
            raise name.point.error("function exceeds 32 local variables including parameters")
        self.locals[name.text] = len(self.local_names)
        self.local_names.append(name.text)

    def block(self) -> Block:
        token = self.expect("{")
        statements: list[Statement] = []
        while self.token.text != "}":
            if self.token.kind == "eof":
                raise token.point.error("unterminated block")
            statements.append(self.statement())
        self.advance()
        return Block(token, statements)

    def statement(self) -> Statement:
        token = self.token
        if token.text == "{":
            return self.block()
        if token.text == "let":
            return self.let()
        if token.text == "if":
            return self.if_statement()
        if token.text == "while":
            return self.while_statement()
        if token.text == "return":
            return self.return_statement()
        if token.text in ("break", "continue"):
            self.advance()
            if self.loop_depth == 0:
                raise token.point.error(f"{token.text} outside a loop")
            self.expect(";")
            return LoopControl(token, token.text)
        expression = self.expression()
        if self.match("="):
            if not isinstance(expression, (Variable, StateReference)):
                raise token.point.error("assignment target must be a local or state.field")
            value = self.expression()
            self.expect(";")
            return Assignment(token, expression, value)
        self.expect(";")
        return ExpressionStatement(token, expression)

    def let(self) -> Let:
        token = self.expect("let")
        name = self.name("local variable name")
        self.expect("=")
        initial = self.expression()
        self.expect(";")
        self.declare_local(name, "local variable")
        return Let(token, name.text, self.locals[name.text], initial)

    def if_statement(self) -> If:
        token = self.expect("if")
        condition = self.parenthesized_expression()
        then_branch = self.statement()
        else_branch = self.statement() if self.match("else") else None
        return If(token, condition, then_branch, else_branch)

    def while_statement(self) -> While:
        token = self.expect("while")
        condition = self.parenthesized_expression()
        self.loop_depth += 1
        try:
            body = self.statement()
        finally:
            self.loop_depth -= 1
        return While(token, condition, body)

    def return_statement(self) -> Return:
        token = self.expect("return")
        value = None if self.match(";") else self.expression()
        if value is not None:
            self.returns_value = True
            self.expect(";")
        return Return(token, value)

    def parenthesized_expression(self) -> Expression:
        self.expect("(")
        expression = self.expression()
        self.expect(")")
        return expression

    def expression(self) -> Expression:
        return self.logical_or()

    def logical_or(self) -> Expression:
        expression = self.logical_and()
        while self.token.text == "||":
            token = self.advance()
            expression = Binary(token, "||", expression, self.logical_and())
        return expression

    def logical_and(self) -> Expression:
        expression = self.equality()
        while self.token.text == "&&":
            token = self.advance()
            expression = Binary(token, "&&", expression, self.equality())
        return expression

    def equality(self) -> Expression:
        expression = self.comparison()
        while self.token.text in ("==", "!="):
            token = self.advance()
            expression = Binary(token, token.text, expression, self.comparison())
        return expression

    def comparison(self) -> Expression:
        expression = self.term()
        while self.token.text in ("<", "<=", ">", ">="):
            token = self.advance()
            expression = Binary(token, token.text, expression, self.term())
        return expression

    def term(self) -> Expression:
        expression = self.factor()
        while self.token.text in ("+", "-"):
            token = self.advance()
            expression = Binary(token, token.text, expression, self.factor())
        return expression

    def factor(self) -> Expression:
        expression = self.unary()
        while self.token.text in ("*", "/"):
            token = self.advance()
            expression = Binary(token, token.text, expression, self.unary())
        return expression

    def unary(self) -> Expression:
        if self.token.text == "-" and self.tokens[self.index + 1].kind == "integer":
            self.advance()
            token = self.advance()
            return Integer(token, -int(token.value))
        if self.token.text in ("!", "-"):
            token = self.advance()
            return Unary(token, token.text, self.unary())
        return self.primary()

    def complete_call(self, token: Token, name: str) -> Call:
        self.expect("(")
        arguments: list[Expression] = []
        if self.token.text != ")":
            while True:
                arguments.append(self.expression())
                if not self.match(","):
                    break
        self.expect(")")
        return Call(token, name, arguments)

    def primary(self) -> Expression:
        token = self.token
        if token.kind == "integer":
            self.advance()
            value = int(token.value)
            if value > INT64_MAX:
                raise token.point.error("integer literal is outside the signed 64-bit range")
            return Integer(token, value)
        if token.kind == "literal":
            self.advance()
            if isinstance(token.value, float):
                return Number(token, token.value)
            return String(token, str(token.value))
        if token.text in ("true", "false"):
            self.advance()
            return Boolean(token, token.text == "true")
        if self.match("("):
            expression = self.expression()
            self.expect(")")
            return expression
        if self.token.text == "state":
            state_token = self.advance()
            self.expect(".")
            field = self.name("state field name")
            return StateReference(state_token, field.text)
        if self.match("io"):
            io_token = self.tokens[self.index - 1]
            self.expect(".")
            operation = self.name("io operation name")
            full_name = f"io.{operation.text}"
            if full_name not in IO_CALLS:
                raise operation.point.error(f"unknown io operation {operation.text!r}")
            self.uses_device_calls = True
            return self.complete_call(io_token, full_name)
        name = self.name("expression")
        if self.token.text == "(":
            return self.complete_call(name, name.text)
        if name.text not in self.locals:
            raise name.point.error(f"undefined local variable {name.text!r}")
        return Variable(name, name.text, self.locals[name.text])




@dataclass(slots=True)
class FunctionInfo:
    declaration: FunctionDecl
    types: list[ValueType]
    return_type: ValueType


class _TypeChecker:
    def __init__(
        self,
        info: FunctionInfo,
        states: dict[str, ValueType],
        functions: dict[str, FunctionInfo],
        changed: list[bool],
        hardware: HardwareDeclaration | None,
    ) -> None:
        self.info = info
        self.states = states
        self.functions = functions
        self.changed = changed
        self.hardware = hardware

    def check(self) -> None:
        self.statement(self.info.declaration.body)
        mixed = _has_void_return(self.info.declaration.body)
        if self.info.declaration.returns_value and mixed:
            raise self.info.declaration.token.point.error(
                f"function {self.info.declaration.name!r} mixes value and void returns"
            )
        if self.info.declaration.returns_value and not _guaranteed_return(self.info.declaration.body):
            raise self.info.declaration.token.point.error(
                f"function {self.info.declaration.name!r} may reach its end without returning a value"
            )

    def refine(self, types: list[ValueType], slot: int, new_type: ValueType, token: Token) -> None:
        old_type = types[slot]
        if new_type == ValueType.UNKNOWN or old_type == new_type:
            return
        if old_type != ValueType.UNKNOWN:
            raise token.point.error(f"expected {old_type.label}, found {new_type.label}")
        types[slot] = new_type
        self.changed[0] = True

    def expect(self, expression: Expression, expected: ValueType) -> ValueType:
        if expected == ValueType.UNKNOWN:
            return self.expression(expression)
        return self.expect_types(expression, (expected,))

    def expect_types(
        self, expression: Expression, expected: tuple[ValueType, ...]
    ) -> ValueType:
        actual = self.expression(expression)
        if actual == ValueType.VOID and ValueType.VOID not in expected:
            raise expression.token.point.error(
                f"expected {_type_list(expected)}, found void expression"
            )
        if actual == ValueType.UNKNOWN and isinstance(expression, Variable) and len(expected) == 1:
            self.refine(self.info.types, expression.slot, expected[0], expression.token)
            return expected[0]
        if actual not in expected and actual != ValueType.UNKNOWN:
            raise expression.token.point.error(
                f"expected {_type_list(expected)}, found {actual.label}"
            )
        if actual == ValueType.UNKNOWN and len(expected) == 1:
            return expected[0]
        return actual

    def statement(self, statement: Statement) -> None:
        if isinstance(statement, Block):
            for child in statement.statements:
                self.statement(child)
            return
        if isinstance(statement, Let):
            actual = self.expect(statement.initial, self.info.types[statement.slot])
            if actual == ValueType.VOID:
                raise statement.token.point.error("a local variable cannot hold void")
            self.refine(self.info.types, statement.slot, actual, statement.token)
            return
        if isinstance(statement, Assignment):
            if isinstance(statement.target, StateReference):
                state_type = self.states.get(statement.target.name)
                if state_type is None:
                    raise statement.target.token.point.error(f"unknown state field {statement.target.name!r}")
                self.expect(statement.value, state_type)
                return
            expected = self.info.types[statement.target.slot]
            actual = self.expect(statement.value, expected)
            if actual == ValueType.VOID:
                raise statement.token.point.error("a local variable cannot hold void")
            self.refine(self.info.types, statement.target.slot, actual, statement.token)
            return
        if isinstance(statement, ExpressionStatement):
            self.expression(statement.expression)
            return
        if isinstance(statement, If):
            self.expect(statement.condition, ValueType.BOOLEAN)
            self.statement(statement.then_branch)
            if statement.else_branch is not None:
                self.statement(statement.else_branch)
            return
        if isinstance(statement, While):
            self.expect(statement.condition, ValueType.BOOLEAN)
            self.statement(statement.body)
            return
        if isinstance(statement, Return):
            if statement.value is None:
                return
            actual = self.expression(statement.value)
            if actual == ValueType.VOID:
                raise statement.value.token.point.error("cannot return a void expression")
            if actual != ValueType.UNKNOWN:
                if self.info.return_type == ValueType.UNKNOWN:
                    self.info.return_type = actual
                    self.changed[0] = True
                elif self.info.return_type != actual:
                    raise statement.value.token.point.error(
                        f"function returns {self.info.return_type.label}, found {actual.label}"
                    )
            return
        if isinstance(statement, LoopControl):
            return
        raise AssertionError(f"unknown statement {type(statement).__name__}")

    def expression(self, expression: Expression) -> ValueType:
        if isinstance(expression, Integer):
            return ValueType.INTEGER
        if isinstance(expression, Number):
            return ValueType.NUMBER
        if isinstance(expression, String):
            return ValueType.STRING
        if isinstance(expression, Boolean):
            return ValueType.BOOLEAN
        if isinstance(expression, Variable):
            return self.info.types[expression.slot]
        if isinstance(expression, StateReference):
            state_type = self.states.get(expression.name)
            if state_type is None:
                raise expression.token.point.error(f"unknown state field {expression.name!r}")
            return state_type
        if isinstance(expression, Call):
            return self.call(expression)
        if isinstance(expression, Unary):
            if expression.operator == "!":
                self.expect(expression.operand, ValueType.BOOLEAN)
                return ValueType.BOOLEAN
            actual = self.expression(expression.operand)
            if actual == ValueType.UNKNOWN:
                return actual
            if actual not in (ValueType.INTEGER, ValueType.NUMBER):
                raise expression.token.point.error(
                    f"operator - requires f32 or i64, found {actual.label}"
                )
            return actual
        if isinstance(expression, Binary):
            return self.binary(expression)
        raise AssertionError(f"unknown expression {type(expression).__name__}")

    def call(self, call: Call) -> ValueType:
        io_call = IO_CALLS.get(call.name)
        if io_call is not None:
            return self.device_call(call)
        builtin = BUILTINS.get(call.name)
        if builtin is not None:
            self.arity(call, len(builtin.arguments))
            for argument, expected in zip(call.arguments, builtin.arguments, strict=True):
                self.expect(argument, expected)
            return builtin.result
        target = self.functions.get(call.name)
        if target is None:
            raise call.token.point.error(f"unknown function {call.name!r}")
        self.arity(call, len(target.declaration.parameters))
        for slot, argument in enumerate(call.arguments):
            expected = target.types[slot]
            actual = self.expect(argument, expected)
            if actual == ValueType.VOID:
                raise argument.token.point.error("void cannot be passed as an argument")
            self.refine(target.types, slot, actual, argument.token)
        return target.return_type

    def arity(self, call: Call, expected: int) -> None:
        if len(call.arguments) != expected:
            raise call.token.point.error(
                f"{call.name} expects {expected} argument{'s' if expected != 1 else ''}, "
                f"found {len(call.arguments)}"
            )

    def device_call(self, call: Call) -> ValueType:
        specification = IO_CALLS[call.name]
        count = len(call.arguments)
        matching = specification.matching_prototypes(count)
        if not matching:
            if len(specification.prototypes) == 1:
                self.arity(call, len(specification.prototypes[0].arguments))
            raise call.token.point.error(f"{call.name} has no prototype for {count} arguments")
        actual = [self.expression(argument) for argument in call.arguments]
        viable = [
            prototype
            for prototype in matching
            if all(
                value == ValueType.UNKNOWN or value in argument.types
                for value, argument in zip(actual, prototype.arguments, strict=True)
            )
        ]
        if not viable:
            self.device_type_error(call, matching, actual)
        for index, (argument, value) in enumerate(zip(call.arguments, actual, strict=True)):
            expected = _unique_types(
                candidate_type
                for prototype in viable
                for candidate_type in prototype.arguments[index].types
            )
            if value == ValueType.UNKNOWN and isinstance(argument, Variable) and len(expected) == 1:
                self.refine(self.info.types, argument.slot, expected[0], argument.token)
            self.check_argument_domain(argument, [prototype.arguments[index] for prototype in viable])
        self.check_callbacks(call, viable)
        self.check_hardware_constraint(call)
        result = self.check_device_literals(call)
        return specification.result if result is None else result

    def device_type_error(
        self, call: Call, matching: list[IoPrototype], actual: list[ValueType]
    ) -> None:
        for index, value in enumerate(actual):
            if value == ValueType.UNKNOWN:
                continue
            expected = _unique_types(
                candidate_type
                for prototype in matching
                for candidate_type in prototype.arguments[index].types
            )
            if value not in expected:
                raise call.arguments[index].token.point.error(
                    f"{call.name} expects {_type_list(expected)} for argument {index + 1}, "
                    f"found {value.label}"
                )
        raise call.token.point.error(f"{call.name} arguments do not match one capability prototype")

    def check_argument_domain(self, expression: Expression, arguments: list[Argument]) -> None:
        literal = _literal_integer(expression)
        if literal is None:
            return
        if any(
            (argument.choices is None or literal in argument.choices)
            and (argument.minimum is None or literal >= argument.minimum)
            and (argument.maximum is None or literal <= argument.maximum)
            for argument in arguments
        ):
            return
        argument = arguments[0]
        if argument.choices is not None:
            expected = ", ".join(str(choice) for choice in argument.choices)
            raise expression.token.point.error(f"argument must be one of {expected}, found {literal}")
        raise expression.token.point.error(
            f"argument {literal} is outside {argument.minimum}..{argument.maximum}"
        )

    def check_callbacks(self, call: Call, viable: list[IoPrototype]) -> None:
        if not any(prototype.callback_results for prototype in viable):
            return
        if any(prototype.notification_results for prototype in viable):
            self.check_callback(
                call.arguments[-2],
                _callback_types(prototype.callback_results for prototype in viable),
            )
            self.check_callback(
                call.arguments[-1],
                _callback_types(prototype.notification_results for prototype in viable),
            )
            return
        self.check_callback(
            call.arguments[-1],
            _callback_types(prototype.callback_results for prototype in viable),
        )

    def check_callback(self, expression: Expression, result_types: tuple[ValueType, ...]) -> None:
        if not isinstance(expression, String):
            return
        target = self.functions.get(expression.value)
        if target is None:
            raise expression.token.point.error(f"unknown callback function {expression.value!r}")
        if len(target.declaration.parameters) != 3:
            raise expression.token.point.error(
                f"callback {expression.value!r} expects 3 arguments, "
                f"found {len(target.declaration.parameters)}"
            )
        self.refine(target.types, 0, ValueType.HANDLE, expression.token)
        self.refine(target.types, 1, ValueType.INTEGER, expression.token)
        current = target.types[2]
        if current != ValueType.UNKNOWN and current not in result_types:
            raise expression.token.point.error(
                f"callback {expression.value!r} cannot receive primary result type {current.label}"
            )
        if len(result_types) == 1:
            self.refine(target.types, 2, result_types[0], expression.token)

    def check_hardware_constraint(self, call: Call) -> None:
        operation = IO_CALLS[call.name].operation
        if operation not in (DeviceOperation.OPEN, NativeOperation.CAPABILITY_SUPPORTED):
            return
        capability = _literal_integer(call.arguments[0])
        if capability is not None:
            if capability not in CAPABILITY_IDS:
                raise call.arguments[0].token.point.error(
                    f"unrecognized capability ID {capability}"
                )
            declaration = next(
                (
                    item
                    for item in (self.hardware.capabilities if self.hardware else ())
                    if item.capability_id == capability
                ),
                None,
            )
            if declaration is None:
                name = CAPABILITY_NAMES[capability]
                raise call.arguments[0].token.point.error(
                    f"hardware capability {name!r} is not declared"
                )
            if operation == DeviceOperation.OPEN:
                minimum_minor = _literal_integer(call.arguments[2])
                if minimum_minor is not None and declaration.minor < minimum_minor:
                    raise call.arguments[2].token.point.error(
                        f"declared {declaration.source_name} minor {declaration.minor} "
                        f"is below requested minimum {minimum_minor}"
                    )
        endpoint = call.arguments[1]
        if isinstance(endpoint, String):
            allowed = self.hardware.permission_endpoints if self.hardware else ()
            if endpoint.value not in allowed:
                raise endpoint.token.point.error(
                    f"hardware endpoint {endpoint.value!r} is not declared"
                )

    def check_device_literals(self, call: Call) -> ValueType | None:
        operation = IO_CALLS[call.name].operation
        if operation == NativeOperation.METADATA:
            field_id = _literal_integer(call.arguments[1])
            if field_id is None:
                return None
            field = METADATA_FIELDS.get(field_id)
            if field is None:
                raise call.arguments[1].token.point.error(f"unknown metadata field ID {field_id}")
            if field.indexed != (len(call.arguments) == 3):
                expectation = "requires" if field.indexed else "rejects"
                raise call.token.point.error(f"metadata field {field.name} {expectation} an entry index")
            return field.result
        if operation == NativeOperation.BYTES and len(call.arguments) == 1:
            argument = call.arguments[0]
            if isinstance(argument, String):
                try:
                    parse_hex_bytes(argument.value)
                except ValueError as error:
                    raise argument.token.point.error(str(error)) from error
        if operation == NativeOperation.INTEGER:
            argument = call.arguments[0]
            try:
                if isinstance(argument, String):
                    parse_decimal_i64(argument.value)
                elif isinstance(argument, Number):
                    parse_decimal_i64(str(int(_binary32(argument.value))))
                    value = _binary32(argument.value)
                    if not value.is_integer() or not INT64_MIN <= value <= INT64_MAX:
                        raise ValueError("f32 value is not an exact signed 64-bit integer")
            except ValueError as error:
                raise argument.token.point.error(str(error)) from error
        if operation == NativeOperation.NUMBER and isinstance(call.arguments[0], Integer):
            argument = call.arguments[0]
            value = argument.value
            try:
                converted = _binary32(float(value))
            except (OverflowError, ValueError) as error:
                raise argument.token.point.error("i64 cannot be represented exactly as f32") from error
            if not _finite_f32(converted) or int(converted) != value:
                raise argument.token.point.error(
                    f"i64 value {value} cannot be represented exactly as f32"
                )
        return None

    def binary(self, binary: Binary) -> ValueType:
        operator = binary.operator
        if operator in ("&&", "||"):
            self.expect(binary.left, ValueType.BOOLEAN)
            self.expect(binary.right, ValueType.BOOLEAN)
            return ValueType.BOOLEAN
        left = self.expression(binary.left)
        right = self.expression(binary.right)
        if ValueType.VOID in (left, right):
            raise binary.token.point.error(f"operator {operator} cannot use a void value")
        if left != ValueType.UNKNOWN and right != ValueType.UNKNOWN and left != right:
            raise binary.token.point.error(
                f"operator {operator} requires matching types, found {left.label} and {right.label}"
            )
        if left != ValueType.UNKNOWN and right == ValueType.UNKNOWN:
            self.expect(binary.right, left)
            right = left
        elif right != ValueType.UNKNOWN and left == ValueType.UNKNOWN:
            self.expect(binary.left, right)
            left = right
        result = left if left != ValueType.UNKNOWN else right
        if operator == "+":
            if result not in (ValueType.UNKNOWN, ValueType.INTEGER, ValueType.NUMBER, ValueType.STRING):
                raise binary.token.point.error("operator + requires two numbers, two integers, or two strings")
            return result
        if operator in ("==", "!="):
            return ValueType.BOOLEAN
        if result not in (ValueType.UNKNOWN, ValueType.INTEGER, ValueType.NUMBER):
            raise binary.token.point.error(
                f"operator {operator} requires two matching numeric types"
            )
        if operator in ("<", "<=", ">", ">="):
            return ValueType.BOOLEAN
        if operator in ("-", "*", "/"):
            return result
        raise AssertionError(f"unknown binary operator {operator}")

def _type_list(types: tuple[ValueType, ...]) -> str:
    return " or ".join(value.label for value in types)


def _unique_types(values: Iterable[ValueType]) -> tuple[ValueType, ...]:
    result: list[ValueType] = []
    for value in values:
        if value not in result:
            result.append(value)
    return tuple(result)


def _callback_types(groups: Iterable[tuple[ValueType, ...]]) -> tuple[ValueType, ...]:
    # Failed operations and data-less results carry void rather than a fake zero.
    return _unique_types(
        value
        for group in groups
        for value in (*group, ValueType.VOID)
    )


def _literal_integer(expression: Expression) -> int | None:
    return expression.value if isinstance(expression, Integer) else None


def _binary32(value: float) -> float:
    return struct.unpack("<f", struct.pack("<f", value))[0]


def parse_decimal_i64(text: str) -> int:
    if not DECIMAL_I64_RE.fullmatch(text):
        raise ValueError("expected a strict signed-decimal i64 string")
    value = int(text)
    if not INT64_MIN <= value <= INT64_MAX:
        raise ValueError("signed 64-bit integer is out of range")
    return value


def parse_hex_bytes(text: str) -> bytes:
    if len(text) % 2:
        raise ValueError("bytes hex string must have exact even length")
    if not re.fullmatch(r"[0-9A-Fa-f]*", text):
        raise ValueError("bytes hex string must contain only ASCII hexadecimal digits")
    result = bytes.fromhex(text)
    if len(result) > MAX_BYTES_PER_BUFFER:
        raise ValueError(f"bytes value exceeds {MAX_BYTES_PER_BUFFER} bytes")
    return result


def _has_void_return(statement: Statement) -> bool:
    if isinstance(statement, Return):
        return statement.value is None
    if isinstance(statement, Block):
        return any(_has_void_return(child) for child in statement.statements)
    if isinstance(statement, If):
        return _has_void_return(statement.then_branch) or (
            statement.else_branch is not None and _has_void_return(statement.else_branch)
        )
    if isinstance(statement, While):
        return _has_void_return(statement.body)
    return False


def _guaranteed_return(statement: Statement) -> bool:
    if isinstance(statement, Return):
        return statement.value is not None
    if isinstance(statement, Block):
        return any(_guaranteed_return(child) for child in statement.statements)
    if isinstance(statement, If):
        return statement.else_branch is not None and _guaranteed_return(
            statement.then_branch
        ) and _guaranteed_return(statement.else_branch)
    return False


def _finite_f32(value: float) -> bool:
    try:
        packed = struct.pack("<f", value)
    except OverflowError:
        return False
    converted = struct.unpack("<f", packed)[0]
    return converted == converted and abs(converted) != float("inf")


def check_types(
    program: Program,
    states: dict[str, ValueType],
    hardware: HardwareDeclaration | None = None,
) -> dict[str, FunctionInfo]:
    infos = {
        function.name: FunctionInfo(
            declaration=function,
            types=[ValueType.UNKNOWN for _ in function.local_names],
            return_type=ValueType.UNKNOWN if function.returns_value else ValueType.VOID,
        )
        for function in program.functions
    }
    changed = [True]
    remaining = 1 + sum(len(info.types) + 1 for info in infos.values())
    while changed[0]:
        changed[0] = False
        for function in program.functions:
            _TypeChecker(infos[function.name], states, infos, changed, hardware).check()
        remaining -= 1
        if changed[0] and remaining < 0:
            raise AssertionError("script type checking did not converge")
    return infos


@dataclass(slots=True)
class EncodedFunction:
    name: str
    parameter_count: int
    local_count: int
    constants: list[Constant]
    code: bytes


class _CodeGenerator:
    def __init__(
        self,
        info: FunctionInfo,
        functions: dict[str, FunctionInfo],
        function_indices: dict[str, int],
        states: dict[str, tuple[int, ValueType]],
    ) -> None:
        self.info = info
        self.functions = functions
        self.function_indices = function_indices
        self.states = states
        self.code = bytearray()
        self.constants: list[Constant] = []
        self.constant_indices: dict[tuple[ValueType, ScriptValue], int] = {}
        self.loops: list[tuple[int, list[int], Token]] = []

    def generate(self) -> EncodedFunction:
        self.statement(self.info.declaration.body)
        self.emit(Opcode.RETURN, self.info.declaration.token)
        if len(self.code) > MAX_CODE_BYTES:
            raise self.info.declaration.token.point.error("function bytecode exceeds 65,535 bytes")
        return EncodedFunction(
            name=self.info.declaration.name,
            parameter_count=len(self.info.declaration.parameters),
            local_count=len(self.info.declaration.local_names),
            constants=self.constants,
            code=bytes(self.code),
        )

    def emit(self, opcode: Opcode, token: Token, immediate: bytes = b"") -> int:
        position = len(self.code)
        self.code.append(opcode.value)
        self.code.extend(immediate)
        if len(self.code) > MAX_CODE_BYTES:
            raise token.point.error("function bytecode exceeds 65,535 bytes")
        return position

    def jump(self, opcode: Opcode, token: Token) -> int:
        position = self.emit(opcode, token, b"\x00\x00")
        return position + 1

    def patch(self, immediate_position: int, target: int, token: Token) -> None:
        relative = target - (immediate_position + 2)
        if not -32_768 <= relative <= 32_767:
            raise token.point.error("jump offset exceeds signed 16-bit range")
        struct.pack_into("<h", self.code, immediate_position, relative)

    def constant(self, constant: Constant, token: Token) -> int:
        key = (constant.type, constant.value)
        existing = self.constant_indices.get(key)
        if existing is not None:
            return existing
        if len(self.constants) >= MAX_CONSTANTS:
            raise token.point.error("function exceeds 256 constants")
        index = len(self.constants)
        self.constants.append(constant)
        self.constant_indices[key] = index
        return index

    def push_constant(self, value_type: ValueType, value: ScriptValue, token: Token) -> None:
        index = self.constant(Constant(value_type, value), token)
        self.emit(Opcode.PUSH_CONST, token, struct.pack("<H", index))

    def device_call(self, expression: Call, operation: int) -> None:
        for argument in expression.arguments:
            self.expression(argument)
        immediate = struct.pack("<HB", operation, len(expression.arguments))
        self.emit(Opcode.DEVICE_CALL, expression.token, immediate)

    def statement(self, statement: Statement) -> None:
        if isinstance(statement, Block):
            for child in statement.statements:
                self.statement(child)
            return
        if isinstance(statement, Let):
            self.expression(statement.initial)
            self.emit(Opcode.STORE_LOCAL, statement.token, struct.pack("<H", statement.slot))
            return
        if isinstance(statement, Assignment):
            self.expression(statement.value)
            if isinstance(statement.target, StateReference):
                index = self.states[statement.target.name][0]
                self.emit(Opcode.STORE_STATE, statement.token, struct.pack("<H", index))
            else:
                self.emit(Opcode.STORE_LOCAL, statement.token, struct.pack("<H", statement.target.slot))
            return
        if isinstance(statement, ExpressionStatement):
            self.expression(statement.expression)
            self.emit(Opcode.POP, statement.token)
            return
        if isinstance(statement, If):
            self.expression(statement.condition)
            false_jump = self.jump(Opcode.JUMP_FALSE, statement.token)
            self.statement(statement.then_branch)
            if statement.else_branch is not None:
                end_jump = self.jump(Opcode.JUMP, statement.token)
                self.patch(false_jump, len(self.code), statement.token)
                self.statement(statement.else_branch)
                self.patch(end_jump, len(self.code), statement.token)
            else:
                self.patch(false_jump, len(self.code), statement.token)
            return
        if isinstance(statement, While):
            start = len(self.code)
            self.expression(statement.condition)
            exit_jump = self.jump(Opcode.JUMP_FALSE, statement.token)
            breaks: list[int] = []
            self.loops.append((start, breaks, statement.token))
            self.statement(statement.body)
            self.loops.pop()
            back_jump = self.jump(Opcode.JUMP, statement.token)
            self.patch(back_jump, start, statement.token)
            end = len(self.code)
            self.patch(exit_jump, end, statement.token)
            for break_jump in breaks:
                self.patch(break_jump, end, statement.token)
            return
        if isinstance(statement, Return):
            if statement.value is not None:
                self.expression(statement.value)
            self.emit(Opcode.RETURN, statement.token)
            return
        if isinstance(statement, LoopControl):
            start, breaks, _ = self.loops[-1]
            if statement.keyword == "continue":
                jump = self.jump(Opcode.JUMP, statement.token)
                self.patch(jump, start, statement.token)
            else:
                breaks.append(self.jump(Opcode.JUMP, statement.token))
            return
        raise AssertionError(f"unknown statement {type(statement).__name__}")

    def expression(self, expression: Expression) -> None:
        if isinstance(expression, Integer):
            self.push_constant(ValueType.INTEGER, expression.value, expression.token)
            return
        if isinstance(expression, Number):
            self.push_constant(ValueType.NUMBER, expression.value, expression.token)
            return
        if isinstance(expression, String):
            self.push_constant(ValueType.STRING, expression.value, expression.token)
            return
        if isinstance(expression, Boolean):
            self.emit(Opcode.PUSH_BOOL, expression.token, bytes([int(expression.value)]))
            return
        if isinstance(expression, Variable):
            self.emit(Opcode.LOAD_LOCAL, expression.token, struct.pack("<H", expression.slot))
            return
        if isinstance(expression, StateReference):
            index = self.states[expression.name][0]
            self.emit(Opcode.LOAD_STATE, expression.token, struct.pack("<H", index))
            return
        if isinstance(expression, Call):
            io_call = IO_CALLS.get(expression.name)
            if io_call is not None:
                self.device_call(expression, io_call.operation)
                return
            for argument in expression.arguments:
                self.expression(argument)
            if expression.name in BUILTINS:
                builtin = BUILTINS[expression.name]
                self.emit(Opcode.BUILTIN, expression.token, bytes([builtin.id, len(expression.arguments)]))
            else:
                self.emit(
                    Opcode.CALL,
                    expression.token,
                    struct.pack("<HB", self.function_indices[expression.name], len(expression.arguments)),
                )
            return
        if isinstance(expression, Unary):
            self.expression(expression.operand)
            self.emit(Opcode.NOT if expression.operator == "!" else Opcode.NEGATE, expression.token)
            return
        if isinstance(expression, Binary):
            self.binary(expression)
            return
        raise AssertionError(f"unknown expression {type(expression).__name__}")

    def binary(self, binary: Binary) -> None:
        if binary.operator == "||":
            self.expression(binary.left)
            right_jump = self.jump(Opcode.JUMP_FALSE, binary.token)
            self.emit(Opcode.PUSH_BOOL, binary.token, b"\x01")
            end_jump = self.jump(Opcode.JUMP, binary.token)
            self.patch(right_jump, len(self.code), binary.token)
            self.expression(binary.right)
            self.patch(end_jump, len(self.code), binary.token)
            return
        if binary.operator == "&&":
            self.expression(binary.left)
            false_jump = self.jump(Opcode.JUMP_FALSE, binary.token)
            self.expression(binary.right)
            end_jump = self.jump(Opcode.JUMP, binary.token)
            self.patch(false_jump, len(self.code), binary.token)
            self.emit(Opcode.PUSH_BOOL, binary.token, b"\x00")
            self.patch(end_jump, len(self.code), binary.token)
            return
        self.expression(binary.left)
        self.expression(binary.right)
        opcode = {
            "+": Opcode.ADD,
            "-": Opcode.SUBTRACT,
            "*": Opcode.MULTIPLY,
            "/": Opcode.DIVIDE,
            "==": Opcode.EQUAL,
            "!=": Opcode.NOT_EQUAL,
            "<": Opcode.LESS,
            "<=": Opcode.LESS_EQUAL,
            ">": Opcode.GREATER,
            ">=": Opcode.GREATER_EQUAL,
        }[binary.operator]
        self.emit(opcode, binary.token)


@dataclass(frozen=True, slots=True)
class ScriptOutput:
    functions: list[EncodedFunction]
    types: dict[str, ValueType]
    uses_device_calls: bool


def compile_script(
    text: str,
    path: str | Path,
    line: int,
    column: int,
    states: dict[str, tuple[int, ValueType]],
    hardware: HardwareDeclaration | None = None,
) -> ScriptOutput:
    tokens = Lexer(text, path, line, column).tokens()
    program = Parser(tokens).parse()
    state_types = {name: value[1] for name, value in states.items()}
    infos = check_types(program, state_types, hardware)
    indices = {function.name: index for index, function in enumerate(program.functions)}
    encoded = [
        _CodeGenerator(infos[function.name], infos, indices, states).generate()
        for function in program.functions
    ]
    return ScriptOutput(
        encoded,
        {name: info.return_type for name, info in infos.items()},
        program.uses_device_calls,
    )
