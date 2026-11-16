from __future__ import annotations

from pathlib import Path


class CompileError(Exception):
    """A source error with a stable one-based source location."""

    def __init__(self, path: str | Path, line: int, column: int, message: str) -> None:
        self.path = str(path)
        self.line = max(1, line)
        self.column = max(1, column)
        self.message = message
        super().__init__(f"{self.path}:{self.line}:{self.column}: {message}")


class PackageError(Exception):
    """A malformed or unsupported binary package."""


class PngError(Exception):
    """A malformed or unsupported PNG asset."""
