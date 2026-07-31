#!/usr/bin/env python3
"""Build bridge geometry with the death animation declared by its skeleton."""

from __future__ import annotations

import argparse
import struct
import subprocess
import xml.etree.ElementTree as ET
from pathlib import Path

from build_geometry_index import (
    SEASON_PRIORITY,
    child,
    parse_root,
    reference_path,
)


def child_text(element: ET.Element, name: str) -> str:
    value = child(element, name)
    return (value.text or "").strip() if value is not None else ""


def selected_model_path(
    data_root: Path,
    vis_path: Path,
) -> Path | None:
    visual = parse_root(vis_path)
    models = child(visual, "Models") if visual is not None else None
    if models is None:
        return None
    candidates: list[tuple[str, Path]] = []
    for item in models:
        model = child(item, "Model")
        if model is None:
            continue
        model_path = reference_path(
            data_root,
            vis_path,
            model.attrib.get("href", ""),
        )
        if model_path is not None:
            candidates.append((child_text(item, "Season"), model_path))
    for wanted in SEASON_PRIORITY:
        for season, model_path in candidates:
            if season == wanted:
                return model_path
    return candidates[0][1] if candidates else None


def resource_record(path: Path) -> str | None:
    root = parse_root(path)
    record = (
        root.attrib.get("ObjectRecordID", "").strip()
        if root is not None
        else ""
    )
    return record if record.isdigit() else None


def death_animation_record(
    data_root: Path,
    model_path: Path,
) -> str | None:
    model = parse_root(model_path)
    skeleton_ref = child(model, "Skeleton") if model is not None else None
    skeleton_path = (
        reference_path(
            data_root,
            model_path,
            skeleton_ref.attrib.get("href", ""),
        )
        if skeleton_ref is not None
        else None
    )
    skeleton = (
        parse_root(skeleton_path)
        if skeleton_path is not None
        else None
    )
    animations = (
        child(skeleton, "Animations")
        if skeleton is not None
        else None
    )
    if animations is None or skeleton_path is None:
        return None
    for animation_ref in animations:
        animation_path = reference_path(
            data_root,
            skeleton_path,
            animation_ref.attrib.get("href", ""),
        )
        animation = (
            parse_root(animation_path)
            if animation_path is not None
            else None
        )
        if (
            animation is not None
            and child_text(animation, "Type") == "ANIMATION_DEATH"
        ):
            return resource_record(animation_path)
    return None


def visual_paths(
    data_root: Path,
    stats_path: Path,
    stats: ET.Element,
) -> list[Path]:
    result: list[Path] = []
    for element_name in ("Center", "End"):
        element = child(stats, element_name)
        if element is None:
            continue
        groups: list[ET.Element] = []
        base = child(element, "VisualObjects")
        if base is not None:
            groups.append(base)
        damage_states = child(element, "DamageStates")
        if damage_states is not None:
            for state in damage_states:
                damaged = child(state, "VisObjects")
                if damaged is not None:
                    groups.append(damaged)
        for group in groups:
            for visual_ref in group:
                visual_path = reference_path(
                    data_root,
                    stats_path,
                    visual_ref.attrib.get("href", ""),
                )
                if visual_path is not None and visual_path not in result:
                    result.append(visual_path)
    return result


def discover_bindings(
    data_root: Path,
) -> dict[str, str]:
    bridge_root = data_root / "Bridges"
    bindings: dict[str, str] = {}
    if not bridge_root.is_dir():
        return bindings
    for stats_path in bridge_root.rglob("*.xdb"):
        stats = parse_root(stats_path)
        if (
            stats is None
            or stats.tag.rsplit("}", 1)[-1].lower()
            != "bridgerpgstats"
        ):
            continue
        for visual_path in visual_paths(data_root, stats_path, stats):
            model_path = selected_model_path(data_root, visual_path)
            model = (
                parse_root(model_path)
                if model_path is not None
                else None
            )
            geometry_ref = (
                child(model, "Geometry")
                if model is not None
                else None
            )
            geometry_path = (
                reference_path(
                    data_root,
                    model_path,
                    geometry_ref.attrib.get("href", ""),
                )
                if geometry_ref is not None and model_path is not None
                else None
            )
            geometry = (
                resource_record(geometry_path)
                if geometry_path is not None
                else None
            )
            animation = (
                death_animation_record(data_root, model_path)
                if model_path is not None
                else None
            )
            if geometry is not None and animation is not None:
                bindings.setdefault(geometry, animation)
    return bindings


def animated_part_count(path: Path) -> int:
    """Return the number of animated mesh parts in one BK2MSH cache."""
    data = path.read_bytes()
    if len(data) < 16 or data[:8] != b"BK2MSH1\0":
        return 0
    version, mesh_count = struct.unpack_from("<II", data, 8)
    if version < 3 or version > 4 or mesh_count == 0:
        return 0
    offset = 16
    animated = 0
    try:
        for _ in range(mesh_count):
            (
                vertex_count,
                index_count,
                group_count,
                frame_count,
            ) = struct.unpack_from("<IIII", data, offset)
            offset += 20
            if frame_count > 1:
                animated += 1
            offset += frame_count * vertex_count * 8 * 4
            offset += index_count * 4
            offset += group_count * 12
            if version >= 4:
                (bone_count,) = struct.unpack_from("<I", data, offset)
                offset += 4
                for _ in range(bone_count):
                    (name_length,) = struct.unpack_from(
                        "<I",
                        data,
                        offset + 16,
                    )
                    offset += 20 + ((name_length + 3) & ~3)
                offset += vertex_count * 4
        return animated if offset == len(data) else 0
    except struct.error:
        return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--data-root", type=Path, required=True)
    parser.add_argument("--converter", type=Path, required=True)
    parser.add_argument("--geometry-source", type=Path, required=True)
    parser.add_argument("--animation-source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    data_root = args.data_root.resolve()
    bindings = discover_bindings(data_root)
    args.output.mkdir(parents=True, exist_ok=True)
    converted = 0
    for geometry, animation in sorted(
        bindings.items(),
        key=lambda item: int(item[0]),
    ):
        geometry_source = args.geometry_source / geometry
        animation_source = args.animation_source / animation
        if not geometry_source.is_file() or not animation_source.is_file():
            continue
        subprocess.run(
            [
                "node",
                str(args.converter),
                "--input",
                str(args.geometry_source),
                "--output",
                str(args.output),
                "--death-animation",
                str(animation_source),
                "--skip-unsupported",
                geometry,
            ],
            check=True,
        )
        death_output = args.output / f"{geometry}.death.bk2mesh"
        if (
            death_output.is_file()
            and animated_part_count(death_output) > 0
        ):
            converted += 1
    print(
        "bridge_death_geometry_complete=1; "
        f"bindings={len(bindings)}; converted={converted}; "
        f"output={args.output.resolve()}"
    )
    return 0 if bindings and converted == len(bindings) else 1


if __name__ == "__main__":
    raise SystemExit(main())
