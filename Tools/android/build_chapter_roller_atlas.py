#!/usr/bin/env python3
"""Build the Android chapter-counter atlas from the original Bink strip."""

from __future__ import annotations

import argparse
import hashlib
import shutil
import subprocess
import tempfile
from pathlib import Path


SOURCE = Path("Versions/Current/Data/Movies/counter/number18x33.bik")
DESTINATION = Path(
    "android/app/src/main/assets/UI/chaptermap/number18x33_atlas.dds"
)
FRAME_WIDTH = 18
FRAME_HEIGHT = 33
ATLAS_COLUMNS = 40
ATLAS_ROWS = 36


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Decode all 1,440 original counter frames into the DDS atlas "
            "consumed by the Android native menu renderer."
        )
    )
    parser.add_argument(
        "--repo",
        type=Path,
        default=Path("."),
        help="Repository root.",
    )
    parser.add_argument(
        "--source",
        type=Path,
        default=SOURCE,
        help="Bink source, relative to the repository unless absolute.",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=DESTINATION,
        help="DDS output, relative to the repository unless absolute.",
    )
    args = parser.parse_args()

    if shutil.which("ffmpeg") is None:
        raise SystemExit("ffmpeg is required")
    try:
        from PIL import Image
    except ImportError as error:
        raise SystemExit("Pillow is required: python3 -m pip install Pillow") from error

    repo = args.repo.resolve()
    source = args.source if args.source.is_absolute() else repo / args.source
    output = args.output if args.output.is_absolute() else repo / args.output
    if not source.is_file():
        raise SystemExit(f"counter source is missing: {source}")
    if source.read_bytes()[:64].startswith(
        b"version https://git-lfs.github.com/spec/"
    ):
        raise SystemExit(f"counter source is still a Git LFS pointer: {source}")

    with tempfile.TemporaryDirectory(prefix="bk2-chapter-roller-") as temporary:
        png = Path(temporary) / "number18x33_atlas.png"
        subprocess.run(
            [
                "ffmpeg",
                "-hide_banner",
                "-loglevel",
                "error",
                "-i",
                str(source),
                "-vf",
                f"tile={ATLAS_COLUMNS}x{ATLAS_ROWS}:padding=0:margin=0",
                "-frames:v",
                "1",
                str(png),
            ],
            check=True,
        )
        with Image.open(png) as image:
            expected_size = (
                FRAME_WIDTH * ATLAS_COLUMNS,
                FRAME_HEIGHT * ATLAS_ROWS,
            )
            if image.size != expected_size:
                raise SystemExit(
                    f"unexpected atlas size {image.size}; expected {expected_size}"
                )
            output.parent.mkdir(parents=True, exist_ok=True)
            image.convert("RGBA").save(output)

    print(
        f"wrote {output.relative_to(repo) if output.is_relative_to(repo) else output}"
    )
    print(f"sha256 {sha256(output)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
