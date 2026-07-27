#!/usr/bin/env python3
"""Stage Android runtime data without copying gigabytes by default."""

from __future__ import annotations

import argparse
import json
import os
import shutil
from dataclasses import asdict, dataclass
from pathlib import Path


STAGE_ITEMS = [
    ("Versions/Current/Data", "Data", True),
    ("Complete", "Complete", True),
    ("Sound", "Sound", False),
    (
        "Versions/Temporary/Engine/Disk_J/Versions/Current/Data/Movies",
        "MovieSources/DiskJ",
        False,
    ),
]

DEFAULT_EXCLUDE_SEGMENTS = [
    "Editor",
    "Multiplayer",
    "Test3",
    "TESTERS",
    "ArtTests",
    "ProgTests",
]

EXCLUDED_MISSION_SEGMENTS = {
    "arttests",
    "crap",
    "designertest",
    "distest",
    "editor",
    "m1-test",
    "m1_test",
    "multi",
    "multiplayer",
    "progtests",
    "test",
    "test3",
    "testers",
}

REQUIRED_DB_FILES = [
    Path("Data/types.xml"),
    Path("Data/index.bin"),
]

REQUIRED_BIN_DIRS = [
    Path("Data/bin/Geometries"),
    Path("Data/bin/Skeletons"),
    Path("Data/bin/Animations"),
    Path("Data/bin/AIGeometries"),
]

REQUIRED_RUNTIME_DIRS = [
    Path("Data/Scenario"),
    Path("Data/Consts"),
    Path("Data/Other/Text"),
]

REQUIRED_DB_PAYLOAD_DIRS = [
    Path("Data/Units"),
    Path("Data/Squads"),
    Path("Data/Objects"),
    Path("Data/Buildings"),
    Path("Data/Entrenchments"),
    Path("Data/Test"),
    Path("Data/Sounds"),
    Path("Data/SoundAndMusic"),
]


@dataclass
class StagedItem:
    source: str
    destination: str
    mode: str
    files: int
    required: bool


@dataclass
class LayoutEntry:
    path: str
    exists: bool
    files: int | None = None


def normalize_data_root(path: Path) -> Path:
    data_child = path / "Data"
    if data_child.is_dir() and (
        (data_child / "types.xml").is_file()
        or (data_child / "Units").exists()
        or (data_child / "Objects").exists()
    ):
        return data_child
    return path


def count_files(path: Path) -> int:
    if path.is_file():
        return 1
    return sum(1 for item in path.rglob("*") if item.is_file())


def should_exclude(path: Path, exclude_segments: set[str]) -> bool:
    return any(segment.lower() in exclude_segments for segment in path.parts)


def is_single_player_map_info(path: Path, root: Path) -> bool:
    try:
        rel = path.relative_to(root)
    except ValueError:
        rel = path
    segments = {segment.lower() for segment in rel.parts}
    return path.name.lower() == "mapinfo.xdb" and not segments.intersection(EXCLUDED_MISSION_SEGMENTS)


def single_player_map_infos(root: Path) -> list[str]:
    if not root.is_dir():
        return []
    paths: list[str] = []
    for path in root.rglob("*"):
        if path.is_file() and is_single_player_map_info(path, root):
            paths.append(path.relative_to(root).as_posix())
    return sorted(paths, key=str.lower)


def copy_or_link_tree(source: Path, destination: Path, mode: str, exclude_segments: set[str]) -> None:
    for item in source.rglob("*"):
        rel = item.relative_to(source)
        if should_exclude(rel, exclude_segments):
            continue
        target = destination / rel
        if item.is_dir():
            target.mkdir(parents=True, exist_ok=True)
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        if mode == "copy":
            shutil.copy2(item, target)
        elif mode == "hardlink":
            os.link(item, target)
        else:
            raise ValueError(f"Unsupported filtered tree mode: {mode}")


def link_or_copy(source: Path, destination: Path, mode: str, exclude_segments: set[str]) -> None:
    if destination.exists() or destination.is_symlink():
        if destination.is_dir() and not destination.is_symlink():
            shutil.rmtree(destination)
        else:
            destination.unlink()
    destination.parent.mkdir(parents=True, exist_ok=True)

    if mode == "symlink":
        destination.symlink_to(source.resolve(), target_is_directory=source.is_dir())
    elif mode == "copy":
        if source.is_dir():
            copy_or_link_tree(source, destination, mode, exclude_segments)
        else:
            shutil.copy2(source, destination)
    elif mode == "hardlink":
        if source.is_file():
            os.link(source, destination)
            return
        copy_or_link_tree(source, destination, mode, exclude_segments)
    else:
        raise ValueError(f"Unknown mode: {mode}")


def display_path(path: Path, repo: Path) -> str:
    try:
        return path.relative_to(repo).as_posix()
    except ValueError:
        return str(path)


def probe_required_dir(output: Path, item: Path) -> LayoutEntry:
    candidates = [
        output / item,
        output / "Overlay" / item,
    ]
    existing = [path for path in candidates if path.is_dir()]
    return LayoutEntry(
        path=item.as_posix(),
        exists=bool(existing),
        files=sum(count_files(path) for path in existing),
    )


def build_layout_probe(output: Path) -> tuple[dict[str, list[LayoutEntry]], list[str]]:
    required_files = [
        LayoutEntry(path=item.as_posix(), exists=(output / item).is_file())
        for item in REQUIRED_DB_FILES
    ]
    required_dirs = [
        LayoutEntry(
            path=item.as_posix(),
            exists=(output / item).is_dir(),
            files=count_files(output / item) if (output / item).is_dir() else 0,
        )
        for item in REQUIRED_BIN_DIRS
    ]
    required_runtime_dirs = [
        LayoutEntry(
            path=item.as_posix(),
            exists=(output / item).is_dir(),
            files=count_files(output / item) if (output / item).is_dir() else 0,
        )
        for item in REQUIRED_RUNTIME_DIRS
    ]
    required_db_payload_dirs = [
        probe_required_dir(output, item)
        for item in REQUIRED_DB_PAYLOAD_DIRS
    ]

    blockers: list[str] = []
    for item in required_files:
        if not item.exists:
            blockers.append(f"Missing staged DB file: {item.path}")
    for item in required_dirs:
        if not item.exists or item.files == 0:
            blockers.append(f"Missing or empty staged bin directory: {item.path}")
    for item in required_runtime_dirs:
        if not item.exists or item.files == 0:
            blockers.append(f"Missing or empty staged runtime directory: {item.path}")
    for item in required_db_payload_dirs:
        if not item.exists or item.files == 0:
            blockers.append(f"Missing or empty staged DB payload directory: {item.path}")

    return {
        "required_files": required_files,
        "required_bin_dirs": required_dirs,
        "required_runtime_dirs": required_runtime_dirs,
        "required_db_payload_dirs": required_db_payload_dirs,
    }, blockers


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", type=Path, default=Path("."), help="Repository root.")
    parser.add_argument("--output", type=Path, default=Path("DataAndroid"), help="Output directory.")
    parser.add_argument("--mode", choices=("symlink", "hardlink", "copy"), default="symlink")
    parser.add_argument(
        "--overlay-data-root",
        action="append",
        type=Path,
        default=[],
        help="Additional full Data payload root. May point either to Data or to a directory containing Data.",
    )
    parser.add_argument(
        "--exclude-segment",
        action="append",
        default=DEFAULT_EXCLUDE_SEGMENTS,
        help="Path segment to skip in copy/hardlink mode. Symlink mode records but cannot filter excludes.",
    )
    parser.add_argument("--allow-missing", action="store_true")
    args = parser.parse_args()

    repo = args.repo.resolve()
    output = args.output if args.output.is_absolute() else repo / args.output
    output.mkdir(parents=True, exist_ok=True)
    exclude_segments = {segment.lower() for segment in args.exclude_segment}

    staged: list[StagedItem] = []
    missing: list[str] = []
    for source_rel, destination_rel, required in STAGE_ITEMS:
        source = repo / source_rel
        if not source.exists():
            if required:
                missing.append(source_rel)
            continue
        destination = output / destination_rel
        link_or_copy(source, destination, args.mode, exclude_segments)
        staged.append(
            StagedItem(
                source=source_rel,
                destination=display_path(destination, repo),
                mode=args.mode,
                files=count_files(source),
                required=required,
            )
        )

    overlay_roots = [normalize_data_root(path.resolve()) for path in args.overlay_data_root]
    overlay_staged: list[StagedItem] = []
    for root in overlay_roots:
        if not root.exists():
            missing.append(str(root))
            continue
        for item in REQUIRED_DB_PAYLOAD_DIRS:
            source = root / item.relative_to("Data")
            if not source.exists():
                continue
            destination = output / "Overlay" / item
            link_or_copy(source, destination, args.mode, exclude_segments)
            overlay_staged.append(
                StagedItem(
                    source=str(source),
                    destination=display_path(destination, repo),
                    mode=args.mode,
                    files=count_files(source),
                    required=True,
                )
            )

    layout_probe, layout_blockers = build_layout_probe(output)
    source_single_player_map_infos = single_player_map_infos(repo / "Versions/Current/Data")
    staged_single_player_map_infos = single_player_map_infos(output / "Data")
    missing_staged_map_infos = sorted(
        set(source_single_player_map_infos) - set(staged_single_player_map_infos),
        key=str.lower,
    )
    mission_blockers: list[str] = []
    if not staged_single_player_map_infos:
        mission_blockers.append("No staged single-player MapInfo.xdb files found in DataAndroid/Data.")
    if missing_staged_map_infos:
        mission_blockers.append(
            "Staged DataAndroid/Data is missing one or more source single-player MapInfo.xdb files."
        )
    manifest = {
        "output": display_path(output, repo),
        "exclude_segments": sorted(exclude_segments),
        "staged": [asdict(item) for item in staged],
        "overlay_roots": [str(root) for root in overlay_roots],
        "overlay_staged": [asdict(item) for item in overlay_staged],
        "missing": missing,
        "layout_probe": {
            key: [asdict(item) for item in value]
            for key, value in layout_probe.items()
        },
        "single_player_map_infos": {
            "source_count": len(source_single_player_map_infos),
            "staged_count": len(staged_single_player_map_infos),
            "missing_staged_count": len(missing_staged_map_infos),
            "missing_staged_samples": missing_staged_map_infos[:20],
        },
        "blockers": [*missing, *layout_blockers, *mission_blockers],
    }
    (output / "port_manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")

    print(f"Staged {len(staged)} content roots into {output}")
    if overlay_roots:
        print(f"Staged {len(overlay_staged)} overlay payload roots into {output / 'Overlay'}")
    print(
        "Single-player MapInfo files: "
        f"source={len(source_single_player_map_infos)} "
        f"staged={len(staged_single_player_map_infos)} "
        f"missing={len(missing_staged_map_infos)}"
    )
    if missing:
        print("Missing roots:")
        for item in missing:
            print(f"  - {item}")
    if layout_blockers:
        print("Layout blockers:")
        for item in layout_blockers:
            print(f"  - {item}")
    if mission_blockers:
        print("Mission blockers:")
        for item in mission_blockers:
            print(f"  - {item}")
        for item in missing_staged_map_infos[:20]:
            print(f"    missing: {item}")
    if (missing or layout_blockers or mission_blockers) and not args.allow_missing:
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
