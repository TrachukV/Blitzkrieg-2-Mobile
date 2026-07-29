#!/usr/bin/env python3
"""Validate the single-player content surface needed by the Android port."""

from __future__ import annotations

import argparse
import json
import xml.etree.ElementTree as ET
from collections import Counter
from dataclasses import asdict, dataclass
from pathlib import Path


REQUIRED_CONTENT_ROOTS = [
    Path("Versions/Current/Data"),
    Path("Versions/Current/Data/Reinforcements"),
    Path("Complete"),
]

OPTIONAL_CONTENT_ROOTS = [
    Path("Sound"),
]

MOVIE_SOURCE_ROOTS = [
    Path("Complete/Movies"),
    Path("Complete/UI"),
    Path("Versions/Current/Data/Movies"),
    Path("Versions/Temporary/Engine/Disk_J/Versions/Current/Data/Movies"),
]

REF_RESOLUTION_ROOTS = [
    Path("Versions/Current/Data"),
    Path("Complete"),
    Path("Sound"),
    Path("Versions/Temporary/Engine/Disk_J/Versions/Current/Data"),
]

MISSION_SOURCE_ROOTS = [
    Path("Complete/Maps"),
    Path("Complete/CustomMissions"),
    Path("Versions/Current/Data/Maps"),
]

MAP_INFO_SOURCE_ROOTS = [
    Path("Versions/Current/Data/Scenario/Campaigns"),
    Path("Versions/Current/Data/Maps"),
    Path("Complete/Maps"),
    Path("Complete/CustomMissions"),
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

CONTENT_SUFFIXES = {
    ".xdb",
    ".b2m",
    ".b2x",
    ".lua",
    ".dds",
    ".tga",
    ".wav",
    ".ogg",
    ".bik",
    ".txt",
    ".xml",
}


@dataclass
class ContentReport:
    extra_data_roots: list[str]
    present_roots: list[str]
    missing_roots: list[str]
    present_movie_roots: list[str]
    missing_movie_roots: list[str]
    suffix_counts: dict[str, int]
    bin_counts: dict[str, int]
    bik_files: list[str]
    real_bik_files: list[str]
    lfs_pointer_files: list[str]
    mission_candidates: list[str]
    single_player_missions: list[str]
    excluded_mission_candidates: list[str]
    game_root_refs: list[str]
    missing_game_root_source_refs: list[str]
    map_info_files: list[str]
    mission_object_refs: list[str]
    mission_object_ref_groups: dict[str, int]
    missing_mission_object_refs: list[str]
    missing_mission_object_ref_groups: dict[str, int]
    missing_unit_payload_refs: list[str]
    missing_squad_payload_refs: list[str]
    movie_sequence_files: list[str]
    referenced_movie_keys: list[str]
    referenced_movie_sources: dict[str, str]
    missing_referenced_movie_sources: list[str]
    lfs_referenced_movie_sources: list[str]
    blockers: list[str]


def iter_files(root: Path):
    for path in root.rglob("*"):
        if path.is_file():
            yield path


def is_lfs_pointer(path: Path) -> bool:
    try:
        with path.open("rb") as file:
            return file.read(64).startswith(b"version https://git-lfs.github.com/spec/")
    except OSError:
        return False


def is_single_player_mission(path: Path, repo: Path) -> bool:
    rel = path.relative_to(repo)
    segments = {segment.lower() for segment in rel.parts}
    return not segments.intersection(EXCLUDED_MISSION_SEGMENTS)


def strip_xpointer(ref: str) -> str:
    return ref.split("#", 1)[0]


def normalize_legacy_ref(ref: str) -> str:
    normalized = strip_xpointer(ref).replace("\\", "/")
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


def movie_runtime_key_for(path: Path, repo: Path) -> str:
    rel = path.relative_to(repo)
    disk_j_data = Path("Versions/Temporary/Engine/Disk_J/Versions/Current/Data")
    current_data = Path("Versions/Current/Data")
    complete_movies = Path("Complete/Movies")
    complete_ui = Path("Complete/UI")
    for root in (disk_j_data, current_data):
        try:
            return rel.relative_to(root).as_posix()
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


def normalize_extra_data_root(path: Path) -> Path:
    data_child = path / "Data"
    if data_child.is_dir() and (
        (data_child / "types.xml").is_file()
        or (data_child / "Units").exists()
        or (data_child / "Objects").exists()
    ):
        return data_child
    return path


def resolve_content_ref(repo: Path, ref: str, extra_data_roots: list[Path] | None = None) -> Path | None:
    normalized = normalize_legacy_ref(ref)
    if not normalized:
        return None
    for root in extra_data_roots or []:
        candidate = root / normalized
        if candidate.is_file():
            return candidate
    for root in REF_RESOLUTION_ROOTS:
        candidate = repo / root / normalized
        if candidate.is_file():
            return candidate
    return None


def hrefs_from_file(path: Path) -> list[str]:
    refs: list[str] = []
    try:
        root = ET.parse(path).getroot()
    except (ET.ParseError, OSError):
        return refs
    for element in root.iter():
        href = element.attrib.get("href")
        if href:
            refs.append(href)
    return refs


def game_root_refs(repo: Path) -> list[str]:
    game_root = repo / "Versions/Current/Data/GameRoot.xdb"
    if not game_root.is_file():
        return []
    refs: list[str] = []
    try:
        root = ET.parse(game_root).getroot()
    except (ET.ParseError, OSError):
        return refs
    for element in root.iter():
        tag = element.tag.lower()
        href = element.attrib.get("href")
        if not href:
            continue
        if tag in {"item", "tutorial", "mapinfo", "difficultyfileref"}:
            normalized = normalize_legacy_ref(href)
            if normalized.startswith("Scenario/"):
                refs.append(normalized)
    return sorted(set(refs))


def ref_group(ref: str) -> str:
    normalized = normalize_legacy_ref(ref)
    if not normalized:
        return ""
    return normalized.split("/", 1)[0]


def map_info_files(repo: Path) -> list[Path]:
    files: list[Path] = []
    seen: set[Path] = set()
    for root_rel in MAP_INFO_SOURCE_ROOTS:
        root = repo / root_rel
        if not root.exists():
            continue
        for path in root.rglob("*"):
            if not path.is_file() or path.name.lower() != "mapinfo.xdb":
                continue
            if not is_single_player_mission(path, repo):
                continue
            resolved = path.resolve()
            if resolved in seen:
                continue
            seen.add(resolved)
            files.append(path)
    return sorted(files, key=lambda path: path.relative_to(repo).as_posix().lower())


def mission_object_refs(repo: Path) -> tuple[list[str], dict[str, int]]:
    refs: list[str] = []
    counts: Counter[str] = Counter()
    for path in map_info_files(repo):
        try:
            root = ET.parse(path).getroot()
        except (ET.ParseError, OSError):
            continue
        for element in root.iter():
            if element.tag.lower() != "object":
                continue
            href = element.attrib.get("href")
            if not href:
                continue
            normalized = normalize_legacy_ref(href)
            if not normalized:
                continue
            refs.append(normalized)
            group = ref_group(normalized)
            if group:
                counts[group] += 1
    return sorted(set(refs)), dict(sorted(counts.items()))


def grouped_ref_counts(refs: list[str]) -> dict[str, int]:
    counts: Counter[str] = Counter()
    for ref in refs:
        group = ref_group(ref)
        if group:
            counts[group] += 1
    return dict(sorted(counts.items()))


def refs_with_suffix(refs: list[str], suffix: str) -> list[str]:
    suffix_lower = suffix.lower()
    return sorted(ref for ref in refs if ref.lower().endswith(suffix_lower))


def movie_sources(repo: Path) -> dict[str, Path]:
    sources: dict[str, Path] = {}
    for root_rel in MOVIE_SOURCE_ROOTS:
        root = repo / root_rel
        if not root.exists():
            continue
        for path in root.rglob("*.bik"):
            if not path.is_file():
                continue
            key = movie_runtime_key_for(path, repo)
            lower_key = key.lower()
            current = sources.get(lower_key)
            if current is None:
                sources[lower_key] = path
                continue
            if is_lfs_pointer(current) and not is_lfs_pointer(path):
                sources[lower_key] = path
    return sources


def movie_sequence_refs(repo: Path) -> tuple[list[str], list[str]]:
    sequence_files: list[str] = []
    movie_keys: list[str] = []
    for root_rel in MOVIE_SOURCE_ROOTS:
        root = repo / root_rel
        if not root.exists():
            continue
        for path in root.rglob("*.xml"):
            if not path.is_file():
                continue
            try:
                root_element = ET.parse(path).getroot()
            except (ET.ParseError, OSError):
                continue
            found_in_file = False
            for element in root_element.iter():
                if element.tag.lower() != "filename" or element.text is None:
                    continue
                key = normalize_movie_key(element.text.strip())
                if key:
                    movie_keys.append(key)
                    found_in_file = True
            if found_in_file:
                sequence_files.append(path.relative_to(repo).as_posix())
    return sorted(set(sequence_files)), sorted(set(movie_keys))


def build_report(repo: Path, extra_data_roots: list[Path] | None = None) -> ContentReport:
    normalized_extra_data_roots = [
        normalize_extra_data_root(path.resolve())
        for path in (extra_data_roots or [])
    ]
    present_roots: list[str] = []
    missing_roots: list[str] = []
    present_movie_roots: list[str] = []
    missing_movie_roots: list[str] = []
    suffix_counts: Counter[str] = Counter()
    content_files: dict[str, Path] = {}
    mission_candidates: list[str] = []
    single_player_missions: list[str] = []
    excluded_mission_candidates: list[str] = []

    def record_content_file(path: Path) -> None:
        rel_path = path.relative_to(repo).as_posix()
        if rel_path in content_files:
            return
        content_files[rel_path] = path
        suffix = path.suffix.lower()
        if suffix in CONTENT_SUFFIXES:
            suffix_counts[suffix] += 1
        if suffix in {".b2m", ".b2x"}:
            mission_candidates.append(rel_path)
            if is_single_player_mission(path, repo):
                single_player_missions.append(rel_path)
            else:
                excluded_mission_candidates.append(rel_path)

    for rel_root in [*REQUIRED_CONTENT_ROOTS, *OPTIONAL_CONTENT_ROOTS]:
        root = repo / rel_root
        if not root.exists():
            if rel_root in REQUIRED_CONTENT_ROOTS:
                missing_roots.append(rel_root.as_posix())
            continue
        present_roots.append(rel_root.as_posix())
        for path in iter_files(root):
            record_content_file(path)

    for rel_root in MOVIE_SOURCE_ROOTS:
        root = repo / rel_root
        if root.exists():
            present_movie_roots.append(rel_root.as_posix())
            for path in root.rglob("*.bik"):
                if path.is_file():
                    record_content_file(path)
        else:
            missing_movie_roots.append(rel_root.as_posix())

    lfs_pointer_files = sorted(
        rel_path for rel_path, path in content_files.items() if is_lfs_pointer(path)
    )
    bik_files = sorted(rel_path for rel_path in content_files if rel_path.lower().endswith(".bik"))
    lfs_pointer_set = set(lfs_pointer_files)
    real_bik_files = sorted(rel_path for rel_path in bik_files if rel_path not in lfs_pointer_set)

    root_refs = game_root_refs(repo)
    missing_game_root_source_refs = sorted(
        ref for ref in root_refs
        if resolve_content_ref(repo, ref, normalized_extra_data_roots) is None
    )
    map_infos = [path.relative_to(repo).as_posix() for path in map_info_files(repo)]
    object_refs, object_ref_groups = mission_object_refs(repo)
    missing_object_refs = sorted(
        ref for ref in object_refs
        if resolve_content_ref(repo, ref, normalized_extra_data_roots) is None
    )
    missing_unit_payload_refs = refs_with_suffix(
        missing_object_refs,
        "/MechUnitRPGStats.xdb",
    ) + refs_with_suffix(
        missing_object_refs,
        "/InfantryRPGStats.xdb",
    )
    missing_squad_payload_refs = refs_with_suffix(
        missing_object_refs,
        "/SquadRPGStats.xdb",
    )

    movie_sequence_files, referenced_movie_keys = movie_sequence_refs(repo)
    source_index = movie_sources(repo)
    referenced_movie_sources: dict[str, str] = {}
    missing_referenced_movie_sources: list[str] = []
    lfs_referenced_movie_sources: list[str] = []
    for key in referenced_movie_keys:
        source = source_index.get(key.lower())
        if source is None:
            missing_referenced_movie_sources.append(key)
            continue
        source_rel = source.relative_to(repo).as_posix()
        referenced_movie_sources[key] = source_rel
        if is_lfs_pointer(source):
            lfs_referenced_movie_sources.append(source_rel)

    bin_counts: Counter[str] = Counter()
    bin_root = repo / "Versions/Current/Data/bin"
    if bin_root.is_dir():
        for path in bin_root.iterdir():
            if path.is_dir():
                bin_counts[path.name] = sum(1 for _ in path.iterdir())

    blockers: list[str] = []
    if missing_roots:
        blockers.append("Sparse checkout is missing one or more required content roots.")
    if not bik_files:
        blockers.append("No .bik files found; fetch movie source roots before video validation.")
    if lfs_pointer_files:
        blockers.append("One or more required assets are Git LFS pointer files, not downloaded content.")
    if missing_referenced_movie_sources:
        blockers.append("One or more movie XML references have no matching .bik source file.")
    if lfs_referenced_movie_sources:
        blockers.append("One or more movie XML references resolve only to Git LFS pointer files.")
    if not single_player_missions:
        blockers.append("No single-player .b2m/.b2x mission files found; fetch Complete/Maps and scenario content.")
    if missing_object_refs:
        blockers.append("One or more single-player MapInfo object refs have no source DB payload file.")
    if missing_unit_payload_refs or missing_squad_payload_refs:
        blockers.append("One or more single-player unit/squad RPG stats refs have no source DB payload file.")
    for required in ("Animations", "Geometries", "Skeletons", "AIGeometries"):
        if bin_counts.get(required, 0) == 0:
            blockers.append(f"Data/bin/{required} is missing or empty.")

    return ContentReport(
        extra_data_roots=[path.as_posix() for path in normalized_extra_data_roots],
        present_roots=present_roots,
        missing_roots=missing_roots,
        present_movie_roots=present_movie_roots,
        missing_movie_roots=missing_movie_roots,
        suffix_counts=dict(sorted(suffix_counts.items())),
        bin_counts=dict(sorted(bin_counts.items())),
        bik_files=bik_files,
        real_bik_files=real_bik_files,
        lfs_pointer_files=lfs_pointer_files,
        mission_candidates=sorted(mission_candidates),
        single_player_missions=sorted(single_player_missions),
        excluded_mission_candidates=sorted(excluded_mission_candidates),
        game_root_refs=root_refs,
        missing_game_root_source_refs=missing_game_root_source_refs,
        map_info_files=map_infos,
        mission_object_refs=object_refs,
        mission_object_ref_groups=object_ref_groups,
        missing_mission_object_refs=missing_object_refs,
        missing_mission_object_ref_groups=grouped_ref_counts(missing_object_refs),
        missing_unit_payload_refs=sorted(set(missing_unit_payload_refs)),
        missing_squad_payload_refs=sorted(set(missing_squad_payload_refs)),
        movie_sequence_files=movie_sequence_files,
        referenced_movie_keys=referenced_movie_keys,
        referenced_movie_sources=dict(sorted(referenced_movie_sources.items())),
        missing_referenced_movie_sources=sorted(missing_referenced_movie_sources),
        lfs_referenced_movie_sources=sorted(set(lfs_referenced_movie_sources)),
        blockers=blockers,
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", type=Path, default=Path("."), help="Repository root.")
    parser.add_argument(
        "--extra-data-root",
        action="append",
        type=Path,
        default=[],
        help="Additional full Data payload root. May point either to Data or to a directory containing Data.",
    )
    parser.add_argument("--json", type=Path, help="Write JSON report.")
    args = parser.parse_args()

    repo = args.repo.resolve()
    report = build_report(repo, args.extra_data_root)

    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(asdict(report), indent=2, sort_keys=True), encoding="utf-8")

    print("Single-player content report")
    print(f"extra data roots: {', '.join(report.extra_data_roots) or '-'}")
    print(f"present roots: {', '.join(report.present_roots) or '-'}")
    print(f"missing roots: {', '.join(report.missing_roots) or '-'}")
    print(f"present movie roots: {', '.join(report.present_movie_roots) or '-'}")
    print(f"missing movie roots: {', '.join(report.missing_movie_roots) or '-'}")
    print(f"content counts: {report.suffix_counts}")
    print(f"Data/bin counts: {report.bin_counts}")
    print(f"movies: {len(report.bik_files)}")
    print(f"real movies: {len(report.real_bik_files)}")
    print(f"LFS pointer files: {len(report.lfs_pointer_files)}")
    print(f"mission candidates: {len(report.mission_candidates)}")
    print(f"single-player missions: {len(report.single_player_missions)}")
    print(f"excluded mission candidates: {len(report.excluded_mission_candidates)}")
    print(f"GameRoot Scenario refs: {len(report.game_root_refs)}")
    print(f"missing raw GameRoot source refs: {len(report.missing_game_root_source_refs)}")
    print(f"single-player MapInfo files: {len(report.map_info_files)}")
    print(f"MapInfo object refs: {len(report.mission_object_refs)}")
    print(f"missing MapInfo object refs: {len(report.missing_mission_object_refs)}")
    print(f"missing unit payload refs: {len(report.missing_unit_payload_refs)}")
    print(f"missing squad payload refs: {len(report.missing_squad_payload_refs)}")
    print(f"movie sequence XML files: {len(report.movie_sequence_files)}")
    print(f"referenced movie keys: {len(report.referenced_movie_keys)}")
    print(f"missing referenced movie sources: {len(report.missing_referenced_movie_sources)}")
    print(f"LFS referenced movie sources: {len(report.lfs_referenced_movie_sources)}")
    if report.blockers:
        print("blockers:")
        for blocker in report.blockers:
            print(f"  - {blocker}")
    return 2 if report.blockers else 0


if __name__ == "__main__":
    raise SystemExit(main())
