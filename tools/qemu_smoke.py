#!/usr/bin/env python3
"""Boot the actual ESP32-S3 firmware in Espressif QEMU, test it over UART, and dump the LCD."""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import select
import shutil
import subprocess
import sys
import time


def missing_tool(name: str) -> None:
    raise SystemExit(f"{name} is unavailable. Export ESP-IDF first: . /path/to/esp-idf/export.sh")


def prepare_flash(repository: Path, build: Path) -> Path:
    elf = build / "microbrowser_firmware.elf"
    application = build / "microbrowser_firmware.bin"
    bootloader = build / "bootloader" / "bootloader.bin"
    partitions = build / "partition_table" / "partition-table.bin"
    for path in (elf, application, bootloader, partitions):
        if not path.is_file():
            raise SystemExit(f"Missing {path}; run 'idf.py -C firmware build' first")
    flash = build / "qemu_flash.bin"
    inputs = (application, bootloader, partitions)
    if flash.is_file() and all(flash.stat().st_mtime >= path.stat().st_mtime for path in inputs):
        return flash
    if shutil.which("esptool") is None:
        missing_tool("esptool")
    command = [
        "esptool", "--chip=esp32s3", "merge-bin", f"--output={flash}",
        "--pad-to-size=8MB", "--flash-mode=dio", "--flash-freq=80m", "--flash-size=8MB",
        "0x0", str(bootloader), "0x8000", str(partitions), "0x10000", str(application),
    ]
    subprocess.run(command, cwd=build, check=True)
    if not flash.is_file() or flash.stat().st_size != 8 * 1024 * 1024:
        raise RuntimeError("esptool did not create the expected 8 MiB QEMU flash image")
    return flash


class QemuProcess:
    def __init__(self, repository: Path, screenshot: Path, show_window: bool, timeout_s: float,
                 *, website: tuple[str, str] | None = None) -> None:
        if shutil.which("qemu-system-xtensa") is None:
            missing_tool("qemu-system-xtensa")
        flash = prepare_flash(repository, repository / "firmware" / "build")
        command = [
            "qemu-system-xtensa", "-M", "esp32s3", "-m", "8M",
            "-drive", f"file={flash},if=mtd,format=raw",
            "-global", "driver=timer.esp32s3.timg,property=wdt_disable,value=true",
            "-nic", "user,model=open_eth",
            "-chardev", "stdio,id=char_console,mux=on",
            "-mon", "chardev=char_console,mode=readline",
            "-serial", "chardev:char_console",
        ]
        self.screenshot = screenshot.resolve()
        command += ["-display", "sdl"]
        environment = os.environ.copy()
        # Keep the virtual LCD's SDL update path, but no desktop window in headless mode.
        if not show_window and not environment.get("SDL_VIDEODRIVER"):
            environment["SDL_VIDEODRIVER"] = "dummy"
        self.process = subprocess.Popen(
            command, cwd=repository, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, bufsize=0, env=environment,
        )
        self.output = bytearray()
        self.timeout_s = timeout_s
        self.website = website

    def wait_text(self, tokens: bytes | tuple[bytes, ...]) -> None:
        expected_tokens = (tokens,) if isinstance(tokens, bytes) else tokens
        deadline = time.monotonic() + self.timeout_s
        while not any(expected in self.output for expected in expected_tokens):
            if b"[ESP RGB] Invalid color content address or length" in self.output:
                raise RuntimeError("QEMU RGB panel rejected the framebuffer address or length")
            if self.process.poll() is not None:
                raise RuntimeError(f"QEMU exited with {self.process.returncode}; expected {tokens!r}")
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError(f"timed out waiting for any of {tokens!r}")
            if self.process.stdout is None:
                raise RuntimeError("QEMU stdout is unavailable")
            readable, _, _ = select.select([self.process.stdout], [], [], remaining)
            if not readable:
                continue
            chunk = os.read(self.process.stdout.fileno(), 8192)
            if not chunk:
                raise RuntimeError(f"QEMU closed its console; expected {token!r}")
            self.output.extend(chunk)
            if len(self.output) > 131072:
                del self.output[:-65536]
            sys.stdout.buffer.write(chunk)
            sys.stdout.buffer.flush()

    def send(self, command: str | bytes) -> None:
        if self.process.stdin is None:
            raise RuntimeError("QEMU stdin is unavailable")
        self.process.stdin.write(command.encode() if isinstance(command, str) else command)
        self.process.stdin.flush()

    def finish(self) -> None:
        if self.process.poll() is not None:
            return
        self.process.terminate()
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=5)

    def open_website(self) -> None:
        if self.website is None:
            return
        url, expected_page = self.website
        self.output.clear()
        self.send(f"url {url}\n")
        self.wait_text((b"URL QUEUED", b"COMMAND FAIL"))
        if b"COMMAND FAIL" in self.output:
            raise RuntimeError("UART URL command was rejected")
        expected = f"PAGE name={expected_page} error_present=0".encode()
        deadline = time.monotonic() + self.timeout_s
        while True:
            self.output.clear()
            self.send("page\r\n")
            self.wait_text(b"PAGE name=")
            self.wait_text(b"\n")
            current = bytes(self.output)
            if b"error_present=1" in current or b"COMMAND FAIL" in current:
                raise RuntimeError(f"Website load failed: {current.decode(errors='replace')}")
            if expected in current:
                return
            if time.monotonic() >= deadline:
                raise TimeoutError(f"timed out waiting for {expected!r}")
            time.sleep(0.2)

    def run(self) -> None:
        try:
            self.wait_text(b"MICROBROWSER_QEMU_READY")
            self.output.clear()
            self.send("page\r\n")
            self.wait_text(b"PAGE name=dashboard")
            if b"COMMAND FAIL" in self.output:
                raise RuntimeError("UART parser rejected or duplicated a CRLF-terminated command")
            self.send("selftest\n")
            self.output.clear()
            self.wait_text((b"SELFTEST PASS", b"SELFTEST FAIL"))
            if b"SELFTEST FAIL" in self.output:
                raise RuntimeError("firmware reported SELFTEST FAIL")
            self.open_website()
            self.output.clear()
            self.send("stats\n")
            self.wait_text(b"stack_min_free_bytes=")
            self.wait_text(b"\n")
            self.output.clear()
            self.send(b"\x01c")
            self.wait_text(b"(qemu)")
            self.output.clear()
            self.send(f"screendump {self.screenshot}\n")
            self.wait_text(b"(qemu)")
            self.output.clear()
            self.send("quit\n")
            self.process.wait(timeout=10)
            if self.process.returncode != 0:
                raise RuntimeError(f"QEMU did not shut down cleanly: {self.process.returncode}")
            if not self.screenshot.is_file() or self.screenshot.stat().st_size == 0:
                raise RuntimeError(f"QEMU did not create the LCD dump: {self.screenshot}")
            print(f"\nQEMU smoke passed; actual virtual LCD: {self.screenshot}")
        finally:
            self.finish()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    repo = Path(__file__).resolve().parents[1]
    parser.add_argument("--screenshot", type=Path, default=repo / "build" / "qemu-dashboard.ppm")
    parser.add_argument("--headless", action="store_true", help="omit QEMU's graphical display window")
    parser.add_argument("--url", help="open a website through the running Reader gateway")
    parser.add_argument("--expect-page", help="expected Reader page title when --url is used")
    parser.add_argument("--timeout", type=float, default=90.0)
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    if bool(args.url) != bool(args.expect_page):
        parser.error("--url and --expect-page must be supplied together")
    args.screenshot.parent.mkdir(parents=True, exist_ok=True)
    try:
            QemuProcess(repo, args.screenshot, not args.headless, args.timeout,
                        website=(args.url, args.expect_page) if args.url else None).run()
    except (RuntimeError, subprocess.SubprocessError, OSError) as error:
        print(f"QEMU smoke failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
