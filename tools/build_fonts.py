from __future__ import annotations

import argparse
from pathlib import Path

from font_data import DEFAULT_FONT_PATH, REPO_ROOT, generate_faces, render_cpp


def main() -> None:
    parser = argparse.ArgumentParser(description="Pre-rasterize Inter into fixed C++ arrays")
    parser.add_argument(
        "--output",
        type=Path,
        default=REPO_ROOT / "core" / "font_data.cpp",
        help="C++ output path (default: core/font_data.cpp)",
    )
    arguments = parser.parse_args()

    faces = generate_faces(DEFAULT_FONT_PATH)
    arguments.output.write_text(
        render_cpp(DEFAULT_FONT_PATH, faces), encoding="utf-8", newline="\n"
    )
    total_coverage = sum(len(face.coverage) for face in faces)
    print(f"Generated {arguments.output}: {len(faces)} faces, {total_coverage} coverage bytes")


if __name__ == "__main__":
    main()
