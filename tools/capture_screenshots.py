#!/usr/bin/env python3
"""Regenerate docs/screenshots from real headless framebuffer replays.

Run from any directory after `make`. Requires Pillow (tools/requirements-fonts.txt).
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

from PIL import Image

REPO = Path(__file__).resolve().parent.parent
SCREENSHOTS = REPO / "docs" / "screenshots"
RUNTIME = REPO / "build" / "microbrowser"
SCREEN_SIZE = "480x320"
DEMO_FRAME_MS = 80
DEMO_FRAME_COUNT_LIMIT = 400
GIF_COLORS = 128
APPLICATION_FRAME_MS = 16
REPLAY_SLACK_FRAMES = 60


@dataclass(frozen=True)
class Replay:
    """One application replay and the screenshots it publishes."""

    source: str
    events: str
    published: dict[str, str]


STILLS = (
    Replay(
        source="examples/dashboard.mb",
        events="tests/dashboard.events",
        published={
            "build/dashboard-main.ppm": "native-dashboard.png",
            "build/dashboard-details.ppm": "native-details.png",
            "build/dashboard-input.ppm": "native-input.png",
            "build/dashboard-settings.ppm": "native-scrolled-settings.png",
        },
    ),
    Replay(
        source="examples/counter.mb",
        events="tests/counter.events",
        published={"build/counter.ppm": "counter.png"},
    ),
)

# Each step is (replay command or None, number of DEMO_FRAME_MS frames to record after it).
DEMO_STEPS: tuple[tuple[str | None, int], ...] = (
    (None, 8),
    ("call increase_target", 3),
    ("call increase_target", 3),
    ("call increase_target", 4),
    ("call toggle_mode", 6),
    ("call open_details", 6),
    ("call refresh_sensors", 8),
    ("call go_back", 5),
    ("call open_settings", 5),
    ("click 240 165", 3),
    *(("key backspace", 1) for _ in range(len("room"))),
    *((f"text {letter}", 1) for letter in "Lab"),
    ("key enter", 5),
    *(
        (command, 1)
        for _ in range(3)
        for command in ("down 400 240", "move 400 140", "move 400 40", "up 400 40")
    ),
    (None, 6),
    ("call go_back", 10),
)


def compile_package(source: str) -> Path:
    package = REPO / "build" / (Path(source).stem + ".mbp")
    subprocess.run([sys.executable, str(REPO / "tools" / "mbc"), "build", source, "-o", str(package)],
                   cwd=REPO, check=True)
    return package


def run_replay(package: Path, events: Path, frames: int, state_file: Path) -> None:
    environment = dict(os.environ, MB_STATE_FILE=str(state_file))
    subprocess.run(
        [str(RUNTIME), "--package", str(package), "--headless", "--size", SCREEN_SIZE,
         "--frames", str(frames), "--events", str(events)],
        cwd=REPO, env=environment, check=True, stdout=subprocess.DEVNULL,
    )


def publish_stills(workspace: Path) -> None:
    for replay in STILLS:
        events = REPO / replay.events
        budget = replay_frame_budget(events.read_text(encoding="utf-8"))
        run_replay(compile_package(replay.source), events, budget, workspace / f"{events.stem}.state")
        for capture, name in replay.published.items():
            save_png(REPO / capture, SCREENSHOTS / name)


def save_png(capture: Path, destination: Path) -> None:
    with Image.open(capture) as image:
        image.convert("RGB").save(destination, optimize=True)
    print(f"wrote {destination.relative_to(REPO)}")


def demo_events(frame_directory: Path) -> tuple[str, list[Path]]:
    lines: list[str] = ["wait 300"]
    frames: list[Path] = []
    for command, count in DEMO_STEPS:
        if command is not None:
            lines.append(command)
        for _ in range(count):
            lines.append(f"wait {DEMO_FRAME_MS}")
            frame = frame_directory / f"frame-{len(frames):04d}.ppm"
            lines.append(f"shot {frame}")
            frames.append(frame)
    return "\n".join(lines) + "\n", frames


def replay_frame_budget(script: str) -> int:
    """Frames needed to finish SCRIPT: one per command plus every wait, with slack."""
    commands = script.splitlines()
    wait_ms = sum(int(line.split()[1]) for line in commands if line.startswith("wait "))
    return len(commands) + wait_ms // APPLICATION_FRAME_MS + REPLAY_SLACK_FRAMES


def publish_demo(workspace: Path) -> None:
    script, frames = demo_events(workspace)
    if len(frames) > DEMO_FRAME_COUNT_LIMIT:
        raise ValueError(f"demo has {len(frames)} frames; limit is {DEMO_FRAME_COUNT_LIMIT}")
    events = workspace / "demo.events"
    events.write_text(script, encoding="utf-8")
    application_frames = replay_frame_budget(script)
    run_replay(compile_package("examples/dashboard.mb"), events, application_frames, workspace / "demo.state")
    images = [load_palette_frame(frame) for frame in frames]
    destination = SCREENSHOTS / "demo.gif"
    # Each capture follows its wait plus one application frame for the shot command.
    duration_ms = DEMO_FRAME_MS + APPLICATION_FRAME_MS
    images[0].save(destination, save_all=True, append_images=images[1:], duration=duration_ms,
                   loop=0, optimize=True, disposal=1)
    print(f"wrote {destination.relative_to(REPO)} ({len(images)} frames)")


def load_palette_frame(capture: Path) -> Image.Image:
    with Image.open(capture) as image:
        return image.convert("RGB").quantize(colors=GIF_COLORS, method=Image.Quantize.MEDIANCUT)


def main() -> None:
    if not RUNTIME.exists():
        raise SystemExit("build/microbrowser not found; run `make` first")
    SCREENSHOTS.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="microbrowser-shots-") as directory:
        workspace = Path(directory)
        publish_stills(workspace)
        publish_demo(workspace)


if __name__ == "__main__":
    main()
