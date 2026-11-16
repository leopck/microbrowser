from __future__ import annotations

import argparse
import shlex
import subprocess
import sys
from pathlib import Path

from .compiler import compile_file, compile_source
from .errors import CompileError, PackageError
from .package import decode_package, inspect_summary, read_package


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="mbc", description="Compile and inspect Microbrowser version-1 packages"
    )
    commands = parser.add_subparsers(dest="command", required=True)
    build = commands.add_parser("build", help="compile an .mb source file")
    build.add_argument("input", type=Path, help="input .mb file")
    build.add_argument("-o", "--output", type=Path, help="output .mbp file")
    inspect = commands.add_parser("inspect", help="validate and summarize a package")
    inspect.add_argument("input", type=Path, help="input .mbp package (or .mb source)")
    run = commands.add_parser("run", help="compile and launch the desktop runtime")
    run.add_argument("input", type=Path, help="input .mb file")
    run.add_argument("-o", "--output", type=Path, help="output .mbp file")
    run.add_argument("desktop_args", nargs=argparse.REMAINDER, help="arguments after -- for the desktop runtime")
    return parser


def _output_path(input_path: Path, output: Path | None) -> Path:
    return output if output is not None else input_path.with_suffix(".mbp")


def _build(input_path: Path, output_path: Path) -> bytes:
    package = compile_file(input_path)
    try:
        output_path.write_bytes(package)
    except OSError as error:
        raise CompileError(output_path, 1, 1, f"cannot write package: {error}") from error
    return package


def _missing_runtime(repository: Path) -> int:
    build_directory = repository / "build"
    command = (
        f"make -C {shlex.quote(str(repository))}"
    )
    print(f"mbc: desktop runtime not found: {build_directory / 'microbrowser'}", file=sys.stderr)
    print(f"mbc: build it with:\n  {command}", file=sys.stderr)
    return 1


def main(argv: list[str] | None = None) -> int:
    arguments = _parser().parse_args(argv)
    try:
        if arguments.command == "build":
            output = _output_path(arguments.input, arguments.output)
            package = _build(arguments.input, output)
            decoded = decode_package(package)
            print(
                f"built {output} ({len(package)} bytes; {len(decoded.pages)} pages, "
                f"{len(decoded.functions)} functions, {len(decoded.assets)} assets)"
            )
            return 0
        if arguments.command == "inspect":
            if arguments.input.suffix == ".mb":
                package = compile_source(arguments.input.read_bytes(), arguments.input)
                decoded = decode_package(package)
            else:
                decoded = read_package(arguments.input)
            print(inspect_summary(arguments.input, decoded))
            return 0
        output = _output_path(arguments.input, arguments.output)
        _build(arguments.input, output)
        repository = Path(__file__).resolve().parents[2]
        runtime = repository / "build" / "microbrowser"
        if not runtime.is_file():
            return _missing_runtime(repository)
        desktop_args = list(arguments.desktop_args)
        if desktop_args and desktop_args[0] == "--":
            desktop_args.pop(0)
        completed = subprocess.run([str(runtime), "--package", str(output), *desktop_args], check=False)
        return completed.returncode
    except (CompileError, PackageError, OSError) as error:
        print(f"mbc: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
