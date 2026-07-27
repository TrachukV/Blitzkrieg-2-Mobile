#!/usr/bin/env python3
"""Validate Android video outputs created from the Bink transcode manifest."""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
from dataclasses import asdict, dataclass
from pathlib import Path


@dataclass
class VideoValidation:
    source: str | None
    destination: str
    runtime_key: str
    referenced_by: list[str]
    source_exists: bool
    source_is_lfs_pointer: bool
    exists: bool
    duration_seconds: float | None
    width: int | None
    height: int | None
    error: str | None


def probe_video(path: Path) -> tuple[float | None, int | None, int | None, str | None]:
    if shutil.which("ffprobe") is None:
        return None, None, None, "ffprobe is not installed"

    command = [
        "ffprobe",
        "-v",
        "error",
        "-select_streams",
        "v:0",
        "-show_entries",
        "stream=width,height:format=duration",
        "-of",
        "json",
        str(path),
    ]
    try:
        result = subprocess.run(command, check=True, capture_output=True, text=True)
        data = json.loads(result.stdout)
    except (subprocess.CalledProcessError, json.JSONDecodeError) as error:
        return None, None, None, str(error)

    streams = data.get("streams") or []
    stream = streams[0] if streams else {}
    duration_text = (data.get("format") or {}).get("duration")
    duration = float(duration_text) if duration_text else None
    width = stream.get("width")
    height = stream.get("height")
    return duration, width, height, None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", type=Path, default=Path("."), help="Repository root.")
    parser.add_argument("--manifest", type=Path, default=Path("build/android/bink_transcode_manifest.json"))
    parser.add_argument("--json", type=Path, default=Path("build/android/video_validation_report.json"))
    args = parser.parse_args()

    repo = args.repo.resolve()
    manifest_path = args.manifest if args.manifest.is_absolute() else repo / args.manifest
    jobs = json.loads(manifest_path.read_text(encoding="utf-8"))

    validations: list[VideoValidation] = []
    for job in jobs:
        destination = repo / job["destination"]
        exists = destination.exists()
        duration = width = height = None
        error = None
        if not job.get("source_exists", True):
            error = "missing source video"
        elif job["source_is_lfs_pointer"]:
            error = "source is a Git LFS pointer file"
        elif exists:
            duration, width, height, error = probe_video(destination)
            if error is None and (duration is None or duration <= 0):
                error = "duration is missing or zero"
        else:
            error = "missing transcode output"

        validations.append(
            VideoValidation(
                source=job["source"],
                destination=job["destination"],
                runtime_key=job.get("runtime_key", job["destination"]),
                referenced_by=job.get("referenced_by", []),
                source_exists=job.get("source_exists", True),
                source_is_lfs_pointer=job["source_is_lfs_pointer"],
                exists=exists,
                duration_seconds=duration,
                width=width,
                height=height,
                error=error,
            )
        )

    args.json.parent.mkdir(parents=True, exist_ok=True)
    args.json.write_text(
        json.dumps([asdict(validation) for validation in validations], indent=2),
        encoding="utf-8",
    )

    missing_sources = [item for item in validations if not item.source_exists]
    lfs_blocked = [item for item in validations if item.source_exists and item.source_is_lfs_pointer]
    failed = [
        item
        for item in validations
        if item.source_exists and not item.source_is_lfs_pointer and item.error
    ]
    ready = [item for item in validations if not item.source_is_lfs_pointer and item.error is None]
    ready_referenced = [item for item in ready if item.referenced_by]
    blocked_referenced = [
        item
        for item in validations
        if item.referenced_by and item.error is not None
    ]

    print("Android video validation report")
    print(f"ready videos: {len(ready)}")
    print(f"ready referenced videos: {len(ready_referenced)}")
    print(f"blocked referenced videos: {len(blocked_referenced)}")
    print(f"missing source videos: {len(missing_sources)}")
    print(f"LFS-blocked videos: {len(lfs_blocked)}")
    print(f"failed videos: {len(failed)}")
    for item in missing_sources:
        print(f"  - {item.runtime_key}: missing source video")
    for item in lfs_blocked:
        print(f"  - {item.runtime_key}: source is a Git LFS pointer file")
    for item in failed:
        print(f"  - {item.destination}: {item.error}")
    return 2 if failed or lfs_blocked or missing_sources else 0


if __name__ == "__main__":
    raise SystemExit(main())
