#!/usr/bin/env python3
"""Resolve original RPGStats -> VisObj -> Model -> Geometry references."""

from __future__ import annotations

import argparse
from functools import lru_cache
import re
import struct
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

LEGACY_RELOCATED_PREFIXES = (
    "scene/geoms/all/",
    "scene/texandmats/all/",
)

RESOURCE_UUID_PATTERN = re.compile(
    r"^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-"
    r"[0-9a-f]{4}-[0-9a-f]{12}$",
    re.IGNORECASE,
)

MISSING_MODEL_FALLBACKS = {
    (
        "units/technics/german/tanks/pz_iv_f2/"
        "summer_whole_model.xdb"
    ): (
        "units/technics/german/tanks/pz_iv_ausf_g/"
        "summer_whole_model.xdb"
    ),
    (
        "buildings/common/concretedot_2/1_1_model.xdb"
    ): (
        "buildings/common/concretedot/summer_whole_model.xdb"
    ),
    (
        "objects/flora/bigpine/1_1_model.xdb"
    ): (
        "objects/flora/bigpine/summer_bigpine_model.xdb"
    ),
}

UNSUPPORTED_MODEL_FALLBACKS = {
    (
        "units/infantry/japan/assault_officer/"
        "1_1_model.xdb"
    ): (
        "units/infantry/japan/main_squad/ms_officer_nambu/"
        "summer_jp_officer_pistol_model.xdb",
        1.0,
    ),
    (
        "units/technics/japan/ships/assaultlandingboat/"
        "1_model.xdb"
    ): (
        "units/technics/japan/ships/ka-tsu/"
        "summer_katsu_model.xdb",
        1.3,
    ),
    (
        "units/technics/gb/ships/lst_lsi_gb/1_model.xdb"
    ): (
        "units/technics/gb/ships/elko/summer_whole_model.xdb",
        1.6,
    ),
    (
        "units/technics/ussr/spg/zsu_37/1_1_model.xdb"
    ): (
        "units/technics/ussr/spg/su_76/summer_whole_model.xdb",
        1.0,
    ),
    (
        "units/technics/ussr/tanks/t_60/1_1_model.xdb"
    ): (
        "units/technics/ussr/tanks/t_70/summer_whole_model.xdb",
        1.1,
    ),
}

# The converter reconstructs this Oodle1-compressed German assault boat under
# its original runtime geometry ID and keeps the original model material.
PROCEDURAL_GEOMETRY_RESOURCES = {
    "8E1CF9C4-6B9E-4930-90A2-2B92D658005E",
}

# The early StuG III A/B/C resource is Oodle1-compressed. The Ausf. D mesh has
# the same 2.90494 x 5.40997 footprint, height within one millimetre, and the
# same seven-part layout. Reuse only its geometry while retaining the A/B/C
# model's original material and texture.
UNSUPPORTED_GEOMETRY_FALLBACKS = {
    "1000178": "795",
}


def child(element: ET.Element, name: str) -> ET.Element | None:
    wanted = name.lower()
    for item in element:
        if item.tag.rsplit("}", 1)[-1].lower() == wanted:
            return item
    return None


def canonical_legacy_stem(value: str) -> str:
    value = value.lower().removesuffix("_visobj")
    value = re.sub(r"(?<![0-9])0+([0-9]+)", r"\1", value)
    return re.sub(r"[^a-z0-9]", "", value)


def runtime_geometry_id(value: str) -> int | None:
    if value.isdigit():
        return int(value)
    if not RESOURCE_UUID_PATTERN.fullmatch(value):
        return None
    result = 0x811C9DC5
    for byte in value.upper().encode("ascii"):
        result ^= byte
        result = (result * 0x01000193) & 0xFFFFFFFF
    return 0x40000000 | (result & 0x3FFFFFFF)


def geometry_resource_keys(root: ET.Element) -> list[str]:
    result: list[str] = []
    record = root.attrib.get("ObjectRecordID", "").strip()
    if record.isdigit():
        result.append(record)
    uid = child(root, "uid")
    value = (uid.text or "").strip().upper() if uid is not None else ""
    if RESOURCE_UUID_PATTERN.fullmatch(value) and value not in result:
        result.append(value)
    return result


def geometry_resource_path(data_root: Path, value: str) -> Path | None:
    direct = data_root / "bin" / "Geometries" / value
    if direct.is_file():
        return direct
    resolved = resolve_relative_path(
        str(data_root),
        f"bin/Geometries/{value}",
    )
    return resolved if resolved is not None and resolved.is_file() else None


@lru_cache(maxsize=None)
def granny_resource_supported(resource_value: str) -> bool:
    """Return whether the open converter can decode every Granny section."""
    try:
        resource = Path(resource_value)
        with resource.open("rb") as stream:
            header = stream.read(52)
            if len(header) != 52:
                return False
            version, _, _, section_offset, section_count = struct.unpack_from(
                "<IIIII",
                header,
                32,
            )
            if version not in (6, 7) or section_count > 64:
                return False
            stream.seek(32 + section_offset)
            sections = stream.read(section_count * 44)
    except OSError:
        return False
    if len(sections) != section_count * 44:
        return False
    return all(
        struct.unpack_from("<I", sections, section_index * 44)[0] in (0, 1)
        for section_index in range(section_count)
    )


def geometry_resource_supported(data_root: Path, value: str) -> bool:
    resource = geometry_resource_path(data_root, value)
    return (
        resource is not None
        and granny_resource_supported(str(resource.resolve()))
    )


def converted_geometry_exists(
    converted_geometry_root: Path | None,
    runtime_id: int,
) -> bool:
    return (
        converted_geometry_root is None
        or (converted_geometry_root / f"{runtime_id}.bk2mesh").is_file()
    )


def geometry_resource_available(
    data_root: Path,
    converted_geometry_root: Path | None,
    value: str,
    runtime_id: int,
) -> bool:
    return (
        geometry_resource_supported(data_root, value)
        or value.upper() in PROCEDURAL_GEOMETRY_RESOURCES
    ) and converted_geometry_exists(converted_geometry_root, runtime_id)


def available_geometry_id(
    data_root: Path,
    converted_geometry_root: Path | None,
    values: list[str],
) -> int | None:
    for value in values:
        runtime_id = runtime_geometry_id(value)
        if (
            runtime_id is not None
            and geometry_resource_available(
                data_root,
                converted_geometry_root,
                value,
                runtime_id,
            )
        ):
            return runtime_id
    return None


def validate_geometry_resource_ids(data_root: Path) -> None:
    geometry_root = resolve_relative_path(
        str(data_root),
        "bin/Geometries",
    )
    if geometry_root is None or not geometry_root.is_dir():
        return
    runtime_ids: dict[int, str] = {}
    for resource in geometry_root.iterdir():
        if not resource.is_file():
            continue
        runtime_id = runtime_geometry_id(resource.name)
        if runtime_id is None:
            continue
        previous = runtime_ids.get(runtime_id)
        if previous is not None and previous != resource.name:
            raise ValueError(
                "Runtime geometry ID collision: "
                f"{previous} and {resource.name} -> {runtime_id}"
            )
        runtime_ids[runtime_id] = resource.name


@lru_cache(maxsize=None)
def resolve_relative_path(
    data_root_value: str,
    relative_value: str,
) -> Path | None:
    data_root = Path(data_root_value)
    relative = Path(relative_value)
    current = data_root
    parts = relative.parts
    for part_index, part in enumerate(parts):
        direct = current / part
        if direct.exists():
            current = direct
            continue
        try:
            children = tuple(current.iterdir())
        except OSError:
            return None
        match = next(
            (
                child_path
                for child_path in children
                if child_path.name.casefold() == part.casefold()
            ),
            None,
        )
        if match is None and part_index + 1 == len(parts):
            wanted = canonical_legacy_stem(Path(part).stem)
            suffix = Path(part).suffix.casefold()
            match = next(
                (
                    child_path
                    for child_path in children
                    if child_path.suffix.casefold() == suffix
                    and canonical_legacy_stem(child_path.stem) == wanted
                ),
                None,
            )
        if match is None:
            return None
        current = match
    return current


def reference_path(data_root: Path, owner: Path, href: str) -> Path | None:
    value = href.split("#", 1)[0].strip().replace("\\", "/")
    if not value:
        return None
    if value.startswith("/"):
        relative = value.lstrip("/")
        candidates = [relative]
        lowered = relative.lower()
        for prefix in LEGACY_RELOCATED_PREFIXES:
            if lowered.startswith(prefix):
                candidates.append(relative[len(prefix):])
        for candidate in candidates:
            resolved = resolve_relative_path(
                str(data_root),
                candidate,
            )
            if resolved is not None:
                return resolved
        return data_root / candidates[-1]
    direct = owner.parent / value
    if direct.exists():
        return direct
    try:
        relative = direct.relative_to(data_root)
    except ValueError:
        return direct
    resolved = resolve_relative_path(
        str(data_root),
        relative.as_posix(),
    )
    return resolved if resolved is not None else direct


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
        stem = vis_path.stem
        if stem.lower().endswith("_visobj"):
            stem = stem[:-len("_visobj")]
        for season in SEASON_PRIORITY:
            season_name = season.removeprefix("SEASON_").lower()
            for file_name in (
                f"{season_name}_{stem}_model.xdb",
                f"{stem}_{season_name}_model.xdb",
            ):
                try:
                    relative = (vis_path.parent / file_name).relative_to(
                        data_root
                    )
                except ValueError:
                    continue
                resolved = resolve_relative_path(
                    str(data_root),
                    relative.as_posix(),
                )
                if resolved is not None:
                    return resolved
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
    converted_geometry_root: Path | None,
) -> tuple[int, list[int], float] | None:
    model_root = parse_root(model)
    if model_root is None:
        return None
    geometry_path = href_child_path(data_root, model, model_root, "Geometry")
    if geometry_path is None:
        return None
    geometry = parse_root(geometry_path)
    if geometry is None:
        return None
    values = geometry_resource_keys(geometry)
    geometry_record_id = available_geometry_id(
        data_root,
        converted_geometry_root,
        values,
    )
    geometry_scale = 1.0
    if geometry_record_id is None:
        fallback_values = [
            UNSUPPORTED_GEOMETRY_FALLBACKS[value.upper()]
            for value in values
            if value.upper() in UNSUPPORTED_GEOMETRY_FALLBACKS
        ]
        geometry_record_id = available_geometry_id(
            data_root,
            converted_geometry_root,
            fallback_values,
        )
    if geometry_record_id is None:
        ai_geometry_path = href_child_path(
            data_root,
            geometry_path,
            geometry,
            "AIGeometry",
        )
        ai_geometry = (
            parse_root(ai_geometry_path)
            if ai_geometry_path is not None
            else None
        )
        geometry_record_id = (
            available_geometry_id(
                data_root,
                converted_geometry_root,
                geometry_resource_keys(ai_geometry),
            )
            if ai_geometry is not None
            else None
        )
        if geometry_record_id is None:
            return None
        # Matches AI_TO_VIS from Stats_B2_M1/Vis2AI.h.
        geometry_scale = 2.75 / 64.0
    quantities: list[int] = []
    material_quantities = child(geometry, "MaterialQuantities")
    if material_quantities is not None:
        for item in material_quantities:
            text = (item.text or "").strip()
            if text.isdigit():
                quantities.append(int(text))
    return geometry_record_id, quantities, geometry_scale


def material_bindings(
    data_root: Path,
    model: Path,
) -> tuple[list[str], list[str]]:
    model_root = parse_root(model)
    if model_root is None:
        return [], []
    materials = child(model_root, "Materials")
    if materials is None:
        return [], []
    textures: list[str] = []
    alpha_modes: list[str] = []
    for item in materials:
        material_path = reference_path(
            data_root,
            model,
            item.attrib.get("href", ""),
        )
        if material_path is None:
            textures.append("")
            alpha_modes.append("")
            continue
        material = parse_root(material_path)
        alpha_mode = (
            child(material, "AlphaMode")
            if material is not None
            else None
        )
        alpha_modes.append(
            (alpha_mode.text or "").strip()
            if alpha_mode is not None
            else ""
        )
        texture_path = (
            href_child_path(data_root, material_path, material, "Texture")
            if material is not None
            else None
        )
        if texture_path is None:
            textures.append("")
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
            textures.append("")
            continue
        textures.append(
            dds_path.resolve().relative_to(data_root).as_posix()
        )
    return textures, alpha_modes


def binding_from_vis_path(
    data_root: Path,
    vis_path: Path,
    converted_geometry_root: Path | None,
) -> tuple[int, list[int], list[str], float, list[str]] | None:
    model = model_path(data_root, vis_path)
    if model is None:
        return None
    try:
        model_key = model.relative_to(data_root).as_posix().lower()
    except ValueError:
        model_key = ""
    geometry_scale_multiplier = 1.0
    unsupported_fallback = UNSUPPORTED_MODEL_FALLBACKS.get(model_key)
    if unsupported_fallback is not None:
        fallback_value, fallback_scale = unsupported_fallback
        fallback_model = resolve_relative_path(
            str(data_root),
            fallback_value,
        )
        if fallback_model is not None:
            model = fallback_model
            geometry_scale_multiplier = fallback_scale
    geometry = geometry_info(data_root, model, converted_geometry_root)
    if geometry is None:
        fallback_value = MISSING_MODEL_FALLBACKS.get(model_key)
        fallback_model = (
            resolve_relative_path(str(data_root), fallback_value)
            if fallback_value is not None
            else None
        )
        if fallback_model is not None:
            model = fallback_model
            geometry = geometry_info(
                data_root,
                model,
                converted_geometry_root,
            )
    if geometry is None:
        return None
    geometry_record_id, material_quantities, geometry_scale = geometry
    geometry_scale *= geometry_scale_multiplier
    textures, alpha_modes = material_bindings(data_root, model)
    return (
        geometry_record_id,
        material_quantities,
        textures,
        geometry_scale,
        alpha_modes,
    )


def normalized_path(path: Path, data_root: Path) -> str:
    return path.relative_to(data_root).as_posix().lstrip("/").lower()


def fnv1a64(value: str) -> int:
    result = 0xCBF29CE484222325
    for byte in value.encode("utf-8"):
        result ^= byte
        result = (result * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return result


def stats_visual_binding(
    data_root: Path,
    stats_path: Path,
    stats: ET.Element,
    converted_geometry_root: Path | None,
) -> tuple[int, list[int], list[str], float, list[str]] | None:
    visual = child(stats, "visualObject")
    if visual is not None:
        vis_path = reference_path(
            data_root,
            stats_path,
            visual.attrib.get("href", ""),
        )
        binding = (
            binding_from_vis_path(
                data_root,
                vis_path,
                converted_geometry_root,
            )
            if vis_path is not None
            else None
        )
        if binding is not None:
            return binding

    # Several shipped campaign maps still point at the pre-migration
    # Foo/whole/*RPGStats.xdb. Those descriptors reference the removed
    # Scene/Geoms tree, while the sibling Foo/*RPGStats.xdb with the same
    # record ID points at the converted current model.
    if stats_path.parent.name.casefold() != "whole":
        return None
    try:
        sibling_relative = (
            stats_path.parent.parent / stats_path.name
        ).relative_to(data_root)
    except ValueError:
        return None
    sibling_path = resolve_relative_path(
        str(data_root),
        sibling_relative.as_posix(),
    )
    sibling = (
        parse_root(sibling_path)
        if sibling_path is not None and sibling_path != stats_path
        else None
    )
    if sibling is None or sibling.attrib.get(
        "ObjectRecordID"
    ) != stats.attrib.get("ObjectRecordID"):
        return None
    sibling_visual = child(sibling, "visualObject")
    if sibling_visual is None:
        return None
    sibling_vis_path = reference_path(
        data_root,
        sibling_path,
        sibling_visual.attrib.get("href", ""),
    )
    return (
        binding_from_vis_path(
            data_root,
            sibling_vis_path,
            converted_geometry_root,
        )
        if sibling_vis_path is not None
        else None
    )


def build_index(
    data_root: Path,
    converted_geometry_root: Path | None = None,
) -> dict[
    tuple[int, int],
    tuple[int, int, list[int], list[str], float, list[str]],
]:
    validate_geometry_resource_ids(data_root)
    result: dict[
        tuple[int, int],
        tuple[int, int, list[int], list[str], float, list[str]],
    ] = {}
    for stats_path in data_root.rglob("*.xdb"):
        stats = parse_root(stats_path)
        if stats is None:
            continue
        record = stats.attrib.get("ObjectRecordID", "")
        if not record.isdigit():
            continue
        path_hash = fnv1a64(normalized_path(stats_path, data_root))
        binding = stats_visual_binding(
            data_root,
            stats_path,
            stats,
            converted_geometry_root,
        )
        if binding is not None:
            (
                geometry_record_id,
                material_quantities,
                textures,
                geometry_scale,
                alpha_modes,
            ) = binding
            result[(path_hash, -1)] = (
                int(record),
                geometry_record_id,
                material_quantities,
                textures,
                geometry_scale,
                alpha_modes,
            )
        segments = child(stats, "segments")
        if segments is not None:
            for frame_index, item in enumerate(segments):
                vis = child(item, "VisObj")
                if vis is None:
                    continue
                vis_path = reference_path(
                    data_root,
                    stats_path,
                    vis.attrib.get("href", ""),
                )
                binding = (
                    binding_from_vis_path(
                        data_root,
                        vis_path,
                        converted_geometry_root,
                    )
                    if vis_path is not None
                    else None
                )
                if binding is None:
                    continue
                (
                    geometry_record_id,
                    material_quantities,
                    textures,
                    geometry_scale,
                    alpha_modes,
                ) = binding
                result[(path_hash, frame_index)] = (
                    int(record),
                    geometry_record_id,
                    material_quantities,
                    textures,
                    geometry_scale,
                    alpha_modes,
                )
        bridge_bindings: dict[
            int,
            tuple[int, list[int], list[str], float, list[str]],
        ] = {}
        for frame_index, element_name in ((0, "End"), (1, "Center")):
            element = child(stats, element_name)
            visual_objects = (
                child(element, "VisualObjects")
                if element is not None
                else None
            )
            if visual_objects is None:
                continue
            for visual_object in visual_objects:
                vis_path = reference_path(
                    data_root,
                    stats_path,
                    visual_object.attrib.get("href", ""),
                )
                binding = (
                    binding_from_vis_path(
                        data_root,
                        vis_path,
                        converted_geometry_root,
                    )
                    if vis_path is not None
                    else None
                )
                if binding is not None:
                    bridge_bindings[frame_index] = binding
                    break
        for frame_index, binding in bridge_bindings.items():
            (
                geometry_record_id,
                material_quantities,
                textures,
                geometry_scale,
                alpha_modes,
            ) = binding
            result[(path_hash, frame_index)] = (
                int(record),
                geometry_record_id,
                material_quantities,
                textures,
                geometry_scale,
                alpha_modes,
            )
        if 1 in bridge_bindings:
            (
                geometry_record_id,
                material_quantities,
                textures,
                geometry_scale,
                alpha_modes,
            ) = bridge_bindings[1]
            result[(path_hash, -1)] = (
                int(record),
                geometry_record_id,
                material_quantities,
                textures,
                geometry_scale,
                alpha_modes,
            )
        fence_frame_index = 0
        for group_name in (
            "CenterSegments",
            "DamagedSegmentsOtherSide",
            "DamagedSegments",
            "DestroyedSegments",
        ):
            group = child(stats, group_name)
            vis_objects = (
                child(group, "VisObjes")
                if group is not None
                else None
            )
            if vis_objects is None:
                continue
            for vis in vis_objects:
                vis_path = reference_path(
                    data_root,
                    stats_path,
                    vis.attrib.get("href", ""),
                )
                binding = (
                    binding_from_vis_path(
                        data_root,
                        vis_path,
                        converted_geometry_root,
                    )
                    if vis_path is not None
                    else None
                )
                if binding is not None:
                    (
                        geometry_record_id,
                        material_quantities,
                        textures,
                        geometry_scale,
                        alpha_modes,
                    ) = binding
                    result[(path_hash, fence_frame_index)] = (
                        int(record),
                        geometry_record_id,
                        material_quantities,
                        textures,
                        geometry_scale,
                        alpha_modes,
                    )
                fence_frame_index += 1
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--data-root", type=Path, required=True)
    parser.add_argument("--converted-geometry-root", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    data_root = args.data_root.resolve()
    converted_geometry_root = (
        args.converted_geometry_root.resolve()
        if args.converted_geometry_root is not None
        else None
    )
    index = build_index(data_root, converted_geometry_root)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    lines = [
        "# path_hash, stats_id, geometry_id, material_quantities, texture_paths, frame_index, geometry_scale, material_alpha_modes",
        *(
            f"{path_hash:016x}\t{stats_id}\t{geometry_id}\t"
            f"{','.join(str(value) for value in quantities)}\t"
            f"{'|'.join(textures)}\t{frame_index}\t"
            f"{geometry_scale:.9g}\t{'|'.join(alpha_modes)}"
            for (path_hash, frame_index), (
                stats_id,
                geometry_id,
                quantities,
                textures,
                geometry_scale,
                alpha_modes,
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
