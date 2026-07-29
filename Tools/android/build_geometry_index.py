#!/usr/bin/env python3
"""Resolve original RPGStats -> VisObj -> Model -> Geometry references."""

from __future__ import annotations

import argparse
import xml.etree.ElementTree as ET
from pathlib import Path


SEASON_PRIORITY = (
    "SEASON_ASIA",
    "SEASON_SUMMER",
    "SEASON_SPRING",
    "SEASON_AUTUMN",
    "SEASON_WINTER",
    "SEASON_AFRICA",
)


def child(element: ET.Element, name: str) -> ET.Element | None:
    wanted = name.lower()
    for item in element:
        if item.tag.rsplit("}", 1)[-1].lower() == wanted:
            return item
    return None


def reference_path(data_root: Path, owner: Path, href: str) -> Path | None:
    value = href.split("#", 1)[0].strip().replace("\\", "/")
    if not value:
        return None
    if value.startswith("/"):
        return data_root / value.lstrip("/")
    return owner.parent / value


def parse_root(path: Path) -> ET.Element | None:
    try:
        return ET.parse(path).getroot()
    except (ET.ParseError, OSError):
        return None


def href_child_path(
    data_root: Path,
    owner: Path,
    root: ET.Element,
    name: str,
) -> Path | None:
    item = child(root, name)
    if item is None:
        return None
    return reference_path(data_root, owner, item.attrib.get("href", ""))


def model_path(data_root: Path, vis_path: Path) -> Path | None:
    vis = parse_root(vis_path)
    if vis is None:
        return None
    models = child(vis, "Models")
    if models is None:
        return None
    candidates: list[tuple[str, Path]] = []
    for item in models:
        model = child(item, "Model")
        season = child(item, "Season")
        if model is None:
            continue
        path = reference_path(data_root, vis_path, model.attrib.get("href", ""))
        if path is not None:
            candidates.append(
                ((season.text or "").strip() if season is not None else "", path)
            )
    for wanted in SEASON_PRIORITY:
        for season, path in candidates:
            if season == wanted:
                return path
    return candidates[0][1] if candidates else None


def geometry_id(data_root: Path, model: Path) -> int | None:
    model_root = parse_root(model)
    if model_root is None:
        return None
    geometry_path = href_child_path(data_root, model, model_root, "Geometry")
    if geometry_path is None:
        return None
    geometry = parse_root(geometry_path)
    if geometry is None:
        return None
    value = geometry.attrib.get("ObjectRecordID", "")
    return int(value) if value.isdigit() else None


def normalized_path(path: Path, data_root: Path) -> str:
    return path.relative_to(data_root).as_posix().lstrip("/").lower()


def fnv1a64(value: str) -> int:
    result = 0xCBF29CE484222325
    for byte in value.encode("utf-8"):
        result ^= byte
        result = (result * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return result


def build_index(data_root: Path) -> dict[int, tuple[int, int]]:
    result: dict[int, tuple[int, int]] = {}
    for stats_path in data_root.rglob("*.xdb"):
        stats = parse_root(stats_path)
        if stats is None:
            continue
        visual = child(stats, "visualObject")
        record = stats.attrib.get("ObjectRecordID", "")
        if visual is None or not record.isdigit():
            continue
        vis_path = reference_path(
            data_root,
            stats_path,
            visual.attrib.get("href", ""),
        )
        if vis_path is None:
            continue
        model = model_path(data_root, vis_path)
        if model is None:
            continue
        geometry = geometry_id(data_root, model)
        if geometry is not None:
            result[fnv1a64(normalized_path(stats_path, data_root))] = (
                int(record),
                geometry,
            )
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--data-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    data_root = args.data_root.resolve()
    index = build_index(data_root)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    lines = [
        "# RPGStats path FNV-1a 64, RPGStats record ID, Geometry record ID",
        *(
            f"{path_hash:016x}\t{stats_id}\t{geometry_id}"
            for path_hash, (stats_id, geometry_id) in sorted(index.items())
        ),
        "",
    ]
    args.output.write_text("\n".join(lines), encoding="utf-8")
    print(
        f"geometry_index_complete=1; mappings={len(index)}; "
        f"output={args.output.resolve()}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
