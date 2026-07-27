#!/usr/bin/env python3
"""Create or execute the Bink-to-Android-video transcode manifest."""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import xml.etree.ElementTree as ET
from dataclasses import asdict, dataclass
from pathlib import Path


@dataclass
class TranscodeJob:
    source: str | None
    destination: str
    runtime_key: str
    kind: str
    referenced_by: list[str]
    source_exists: bool
    source_size: int
    source_is_lfs_pointer: bool
    command: list[str]


MOVIE_SOURCE_ROOTS = [
    Path("Complete/Movies"),
    Path("Complete/UI"),
    Path("Versions/Current/Data/Movies"),
    Path("Versions/Temporary/Engine/Disk_J/Versions/Current/Data/Movies"),
]

EXCLUDED_SEGMENTS = {
    "multiplayer",
}


def has_excluded_segment(path: Path) -> bool:
    return any(segment.lower() in EXCLUDED_SEGMENTS for segment in path.parts)


def is_lfs_pointer(path: Path) -> bool:
    try:
        with path.open("rb") as file:
            return file.read(64).startswith(b"version https://git-lfs.github.com/spec/")
    except OSError:
        return False


def normalize_legacy_ref(ref: str) -> str:
    normalized = ref.split("#", 1)[0].replace("\\", "/")
    while normalized.startswith("/"):
        normalized = normalized[1:]
    return normalized


def normalize_movie_key(ref: str) -> str:
    key = normalize_legacy_ref(ref)
    if not key:
        return ""
    if key.lower().endswith(".xml"):
        key = key[:-4] + ".bik"
    elif not key.lower().endswith(".bik"):
        key += ".bik"
    return key


def runtime_key_for(source: Path, repo: Path) -> str:
    rel = source.relative_to(repo)
    disk_j_data = Path("Versions/Temporary/Engine/Disk_J/Versions/Current/Data")
    current_data = Path("Versions/Current/Data")
    complete_movies = Path("Complete/Movies")
    complete_ui = Path("Complete/UI")
    try:
        return rel.relative_to(disk_j_data).as_posix()
    except ValueError:
        pass
    try:
        return rel.relative_to(current_data).as_posix()
    except ValueError:
        pass
    try:
        return (Path("Movies") / rel.relative_to(complete_movies)).as_posix()
    except ValueError:
        pass
    try:
        return (Path("UI") / rel.relative_to(complete_ui)).as_posix()
    except ValueError:
        pass
    return rel.as_posix()


def video_kind(source: Path, repo: Path) -> str:
    rel = source.relative_to(repo)
    return video_kind_for_key(rel.as_posix())


def video_kind_for_key(runtime_key: str) -> str:
    rel_text = runtime_key.lower()
    if "/ui/" in rel_text or "/counter/" in rel_text:
        return "ui"
    return "fullscreen"


def discover_bik(repo: Path) -> list[Path]:
    movies: list[Path] = []
    for root_rel in MOVIE_SOURCE_ROOTS:
        root = repo / root_rel
        if not root.exists():
            continue
        movies.extend(path for path in root.rglob("*.bik") if path.is_file() and not has_excluded_segment(path.relative_to(repo)))
    return sorted(set(movies))


def discover_movie_references(repo: Path) -> dict[str, list[str]]:
    references: dict[str, list[str]] = {}
    for root_rel in MOVIE_SOURCE_ROOTS:
        root = repo / root_rel
        if not root.exists():
            continue
        for path in root.rglob("*.xml"):
            if not path.is_file() or has_excluded_segment(path.relative_to(repo)):
                continue
            try:
                root_element = ET.parse(path).getroot()
            except (ET.ParseError, OSError):
                continue
            rel = path.relative_to(repo).as_posix()
            for element in root_element.iter():
                if element.tag.lower() != "filename" or element.text is None:
                    continue
                key = normalize_movie_key(element.text.strip())
                if not key:
                    continue
                references.setdefault(key, []).append(rel)
    return {key: sorted(set(value)) for key, value in sorted(references.items())}


def build_source_index(repo: Path) -> dict[str, Path]:
    sources: dict[str, Path] = {}
    for source in discover_bik(repo):
        key = runtime_key_for(source, repo)
        lower_key = key.lower()
        current = sources.get(lower_key)
        if current is None:
            sources[lower_key] = source
            continue
        if is_lfs_pointer(current) and not is_lfs_pointer(source):
            sources[lower_key] = source
    return sources


def build_jobs(repo: Path, output_root: Path, codec: str) -> list[TranscodeJob]:
    jobs: list[TranscodeJob] = []
    source_index = build_source_index(repo)
    references = discover_movie_references(repo)
    references_by_lower = {key.lower(): value for key, value in references.items()}
    runtime_keys = sorted(set(source_index) | {key.lower() for key in references})
    display_keys: dict[str, str] = {runtime_key_for(source, repo).lower(): runtime_key_for(source, repo) for source in source_index.values()}
    for key in references:
        display_keys.setdefault(key.lower(), key)

    for lower_key in runtime_keys:
        source = source_index.get(lower_key)
        runtime_key = display_keys[lower_key]
        destination = output_root / Path(runtime_key).with_suffix(".mp4")
        if codec == "h264":
            codec_args = [
                "-vf",
                "pad=ceil(iw/2)*2:ceil(ih/2)*2",
                "-c:v",
                "libx264",
                "-pix_fmt",
                "yuv420p",
                "-c:a",
                "aac",
                "-movflags",
                "+faststart",
            ]
        else:
            codec_args = ["-c:v", "libvpx-vp9", "-b:v", "0", "-crf", "32", "-c:a", "libopus"]
            destination = destination.with_suffix(".webm")
        command = [] if source is None else ["ffmpeg", "-y", "-i", str(source), *codec_args, str(destination)]
        jobs.append(
            TranscodeJob(
                None if source is None else source.relative_to(repo).as_posix(),
                destination.relative_to(repo).as_posix(),
                runtime_key,
                video_kind_for_key(runtime_key),
                references_by_lower.get(lower_key, []),
                source is not None,
                0 if source is None else source.stat().st_size,
                False if source is None else is_lfs_pointer(source),
                command,
            )
        )
    return jobs


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", type=Path, default=Path("."), help="Repository root.")
    parser.add_argument("--output-root", type=Path, default=Path("DataAndroid"), help="Output root, relative to repo unless absolute.")
    parser.add_argument("--codec", choices=("h264", "vp9"), default="h264")
    parser.add_argument("--manifest", type=Path, default=Path("build/android/bink_transcode_manifest.json"))
    parser.add_argument("--execute", action="store_true", help="Run ffmpeg commands instead of writing only the manifest.")
    parser.add_argument("--skip-lfs-pointers", action="store_true", help="When executing, skip Git LFS pointer files instead of failing.")
    parser.add_argument("--skip-existing", action="store_true", help="Do not re-run ffmpeg for destinations that already exist.")
    parser.add_argument("--continue-on-error", action="store_true", help="Keep processing remaining jobs if a transcode command fails.")
    args = parser.parse_args()

    repo = args.repo.resolve()
    output_root = args.output_root if args.output_root.is_absolute() else repo / args.output_root
    jobs = build_jobs(repo, output_root, args.codec)

    manifest = args.manifest if args.manifest.is_absolute() else repo / args.manifest
    manifest.parent.mkdir(parents=True, exist_ok=True)
    manifest.write_text(json.dumps([asdict(job) for job in jobs], indent=2), encoding="utf-8")

    print(f"Found {len(jobs)} Bink videos.")
    pointer_count = sum(1 for job in jobs if job.source_is_lfs_pointer)
    missing_source_count = sum(1 for job in jobs if not job.source_exists)
    referenced_count = sum(1 for job in jobs if job.referenced_by)
    if pointer_count:
        print(f"Git LFS pointer videos: {pointer_count}")
    if missing_source_count:
        print(f"Missing referenced video sources: {missing_source_count}")
    print(f"Referenced videos: {referenced_count}")
    print(f"Wrote manifest: {manifest}")

    if args.execute:
        if shutil.which("ffmpeg") is None:
            raise SystemExit("ffmpeg is required for --execute")
        for job in jobs:
            if not job.source_exists:
                if args.continue_on_error:
                    print(f"Skipping missing referenced video source: {job.runtime_key}")
                    continue
                raise SystemExit(f"Missing referenced video source: {job.runtime_key}")
            if job.source_is_lfs_pointer:
                if args.skip_lfs_pointers:
                    print(f"Skipping Git LFS pointer file: {job.source}")
                    continue
                raise SystemExit(f"Cannot transcode Git LFS pointer file: {job.source}")
            destination = repo / job.destination
            if args.skip_existing and destination.exists() and destination.stat().st_size > 0:
                print(f"Skipping existing transcode: {job.destination}")
                continue
            destination.parent.mkdir(parents=True, exist_ok=True)
            try:
                subprocess.run(job.command, check=True)
            except subprocess.CalledProcessError as error:
                if not args.continue_on_error:
                    raise
                print(f"Transcode failed with exit {error.returncode}: {job.source}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
