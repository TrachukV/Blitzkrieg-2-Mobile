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


def geometry_info(
    data_root: Path,
    model: Path,
) -> tuple[int, list[int]] | None:
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
    if not value.isdigit():
        return None
    quantities: list[int] = []
    material_quantities = child(geometry, "MaterialQuantities")
    if material_quantities is not None:
        for item in material_quantities:
            text = (item.text or "").strip()
            if text.isdigit():
                quantities.append(int(text))
    return int(value), quantities


def texture_paths(data_root: Path, model: Path) -> list[str]:
    model_root = parse_root(model)
    if model_root is None:
        return []
    materials = child(model_root, "Materials")
    if materials is None:
        return []
    result: list[str] = []
    for item in materials:
        material_path = reference_path(
            data_root,
            model,
            item.attrib.get("href", ""),
        )
        if material_path is None:
            result.append("")
            continue
        material = parse_root(material_path)
        texture_path = (
            href_child_path(data_root, material_path, material, "Texture")
            if material is not None
            else None
        )
        if texture_path is None:
            result.append("")
            continue
        texture = parse_root(texture_path)
        dest_name = child(texture, "DestName") if texture is not None else None
        dds_path = (
            reference_path(
                data_root,
                texture_path,
                dest_name.attrib.get("href", ""),
            )
            if dest_name is not None
            else None
        )
        if dds_path is None:
            result.append("")
            continue
        result.append(dds_path.resolve().relative_to(data_root).as_posix())
    return result


def normalized_path(path: Path, data_root: Path) -> str:
    return path.relative_to(data_root).as_posix().lstrip("/").lower()


def fnv1a64(value: str) -> int:
    result = 0xCBF29CE484222325
    for byte in value.encode("utf-8"):
        result ^= byte
        result = (result * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return result


def build_index(
    data_root: Path,
) -> dict[int, tuple[int, int, list[int], list[str]]]:
    result: dict[int, tuple[int, int, list[int], list[str]]] = {}
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
        geometry = geometry_info(data_root, model)
        if geometry is not None:
            geometry_record_id, material_quantities = geometry
            result[fnv1a64(normalized_path(stats_path, data_root))] = (
                int(record),
                geometry_record_id,
                material_quantities,
                texture_paths(data_root, model),
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
        "# path_hash, stats_id, geometry_id, material_quantities, texture_paths",
        *(
            f"{path_hash:016x}\t{stats_id}\t{geometry_id}\t"
            f"{','.join(str(value) for value in quantities)}\t"
            f"{'|'.join(textures)}"
            for path_hash, (
                stats_id,
                geometry_id,
                quantities,
                textures,
            ) in sorted(index.items())
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
