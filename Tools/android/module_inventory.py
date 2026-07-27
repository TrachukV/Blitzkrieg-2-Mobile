#!/usr/bin/env python3
"""Inventory Blitzkrieg 2 runtime modules for the Android single-player port.

The script does not try to compile the legacy code. It gives the next porter a
stable module list, source counts, and blocker categories that must be removed
before a module can join the Android CMake target.
"""

from __future__ import annotations

import argparse
import json
import re
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Iterable


RUNTIME_MODULES = [
    "Misc",
    "System",
    "Game",
    "GameX",
    "Main",
    "Input",
    "Image",
    "AILogic",
    "zlib",
    "Script",
    "3Dmotor",
    "MemoryLib",
    "3DLib",
    "SceneB2",
    "UI",
    "Sound",
    "UISpecificB2",
    "Stats_B2_M1",
    "Common_RTS_AI",
    "B2_M1_World",
    "B2_M1_Terrain",
    "libdb",
    "Parser",
]

EXCLUDED_MODULES = [
    "MapEditor",
    "MapEditorLib",
    "ED_B2",
    "ED_B2_M1",
    "ED_Common",
    "ED_RTS",
    "ELK_A7",
    "Scintilla",
    "ShaderCompiler",
    "dbcodegen",
    "dbindex",
    "dbstruct",
    "Server",
    "Client",
    "Net",
    "Server_Client_Common",
    "TestClient",
    "TestDB",
    "TestParsing",
    "XDBRefsAnswerer",
    "XDBWatcher",
    "XDBWatcherClient",
    "DebugTools",
]

ANDROID_BUILD_VERIFIED_MODULES = {
    "zlib",
}

ANDROID_BUILD_VERIFIED_SOURCE_SUBSETS = {
    "Misc": [
        "Misc/stdafx.cpp",
        "Misc/Tools.cpp",
        "Misc/Geom.cpp",
        "Misc/Spline.cpp",
        "Misc/StrProc.cpp",
    ],
    "System": [
        "System/stdafx.cpp",
        "System/Basic.cpp",
        "System/BasicShare.cpp",
        "System/BinChunkSaver.cpp",
        "System/BinaryResources.cpp",
        "System/BitStreams.cpp",
        "System/CheckSumLog.cpp",
        "System/ChunklessSaver.cpp",
        "System/CombinerVFS.cpp",
        "System/CmdLine.cpp",
        "System/Commands.cpp",
        "System/ConsoleBufferInternal.cpp",
        "System/Cruncher.cpp",
        "System/Dg.cpp",
        "System/FastMath.cpp",
        "System/FilePath.cpp",
        "System/Formatting.cpp",
        "System/FreeIDs.cpp",
        "System/GameDB.cpp",
        "System/GlobalVars.cpp",
        "System/GResource.cpp",
        "System/LightXML.cpp",
        "System/LightXMLUtils.cpp",
        "System/LogStream.cpp",
        "System/Logger.cpp",
        "System/ObjectFactory.cpp",
        "System/Serialize.cpp",
        "System/Singleton.cpp",
        "System/Text.cpp",
        "System/Time.cpp",
        "System/VFS.cpp",
        "System/VFSOperations.cpp",
        "System/XMLChunkSaver.cpp",
        "System/XMLReader.cpp",
        "System/XMLSAXPArser.cpp",
        "System/XmlUtils.cpp",
    ],
    "3Dmotor": [
        "3Dmotor/DBScene.cpp",
    ],
    "AILogic": [
        "AILogic/DBAIConsts.cpp",
    ],
    "B2_M1_Terrain": [
        "B2_M1_Terrain/DBPreLight.cpp",
        "B2_M1_Terrain/DBTerrain.cpp",
        "B2_M1_Terrain/DBTerrainSpot.cpp",
        "B2_M1_Terrain/DBVSO.cpp",
        "B2_M1_Terrain/DBWater.cpp",
    ],
    "GameX": [
        "GameX/CClientGameConsts.cpp",
        "GameX/CustomMissions.cpp",
        "GameX/DBConsts.cpp",
        "GameX/DBGameRoot.cpp",
        "GameX/DBMPConsts.cpp",
        "GameX/DBScenario.cpp",
        "GameX/dbgameoptions.cpp",
    ],
    "Main": [
        "Main/DBNetConsts.cpp",
    ],
    "SceneB2": [
        "SceneB2/DBSceneConsts.cpp",
    ],
    "Script": [
        "Script/CommonFunctions.cpp",
        "Script/Script.cpp",
        "Script/ScriptWrapperInternal.cpp",
        "Script/lapi.cpp",
        "Script/lcode.cpp",
        "Script/ldebug.cpp",
        "Script/ldo.cpp",
        "Script/lfunc.cpp",
        "Script/lgc.cpp",
        "Script/llex.cpp",
        "Script/lmem.cpp",
        "Script/lobject.cpp",
        "Script/lparser.cpp",
        "Script/lsaver.cpp",
        "Script/lstate.cpp",
        "Script/lstring.cpp",
        "Script/ltable.cpp",
        "Script/ltm.cpp",
        "Script/lundump.cpp",
        "Script/lvm.cpp",
        "Script/lzio.cpp",
        "Script/scriptCommon.cpp",
        "Script/scriptPtr.cpp",
        "Script/stdafx.cpp",
    ],
    "Sound": [
        "Sound/DBMusicSystem.cpp",
        "Sound/DBSound.cpp",
        "Sound/DBSoundDesc.cpp",
    ],
    "Stats_B2_M1": [
        "Stats_B2_M1/AckTypes.cpp",
        "Stats_B2_M1/ActionsRemap.cpp",
        "Stats_B2_M1/Commands_Actions.cpp",
        "Stats_B2_M1/ConstructorInfo.cpp",
        "Stats_B2_M1/DBAttachedModelVisObj.cpp",
        "Stats_B2_M1/DBCameraConsts.cpp",
        "Stats_B2_M1/DBClientConsts.cpp",
        "Stats_B2_M1/DBConstructorProfile.cpp",
        "Stats_B2_M1/DBMapInfo.cpp",
        "Stats_B2_M1/DBNotifications.cpp",
        "Stats_B2_M1/DBPassProfile.cpp",
        "Stats_B2_M1/DBPlaneManuvers.cpp",
        "Stats_B2_M1/DBVisObj.cpp",
        "Stats_B2_M1/dbreinforcements.cpp",
        "Stats_B2_M1/IconsSet.cpp",
        "Stats_B2_M1/M1Actions.cpp",
        "Stats_B2_M1/M1UnitActions.cpp",
        "Stats_B2_M1/M1UnitSpecific.cpp",
        "Stats_B2_M1/M1UnitType.cpp",
        "Stats_B2_M1/RPGStats.cpp",
        "Stats_B2_M1/RPGStatsAddIn.cpp",
        "Stats_B2_M1/Season.cpp",
        "Stats_B2_M1/UIEntries.cpp",
        "Stats_B2_M1/UnitTypes.cpp",
        "Stats_B2_M1/UserActions.cpp",
    ],
    "UI": [
        "UI/DBUIConsts.cpp",
        "UI/DBUserInterface.cpp",
    ],
    "UISpecificB2": [
        "UISpecificB2/DBUISpecificB2.cpp",
    ],
    "libdb": [
        "libdb/stdafx.cpp",
        "libdb/Bind.cpp",
        "libdb/BindArray.cpp",
        "libdb/BindProcessor.cpp",
        "libdb/BindProcessorSaveLoad.cpp",
        "libdb/Checksum.cpp",
        "libdb/DBObserverContainer.cpp",
        "libdb/Database.cpp",
        "libdb/GameDatabase.cpp",
        "libdb/Logger.cpp",
        "libdb/ReportMetaInfo.cpp",
        "libdb/StructMetaInfo.cpp",
        "libdb/StructMetaInfoSetGet.cpp",
        "libdb/Type.cpp",
        "libdb/TypeDef.cpp",
        "libdb/TypeDefType.cpp",
        "libdb/Variant.cpp",
    ],
}

BLOCKER_PATTERNS = {
    "win32": re.compile(
        r"#include\s*[<\"]windows\.h|HWND|HINSTANCE|HANDLE|DWORD|GetTickCount|"
        r"CreateWindow|MessageBox|Sleep\s*\(|CRITICAL_SECTION|CreateEvent|WaitForSingleObject",
        re.IGNORECASE,
    ),
    "d3d9": re.compile(r"\bD3D|Direct3D|d3d9|d3dx|IDirect3D", re.IGNORECASE),
    "directinput": re.compile(r"\bdinput|DirectInput|DIK_|DIMOFS_|IDirectInput", re.IGNORECASE),
    "fmod": re.compile(r"\bFMOD|FSOUND_", re.IGNORECASE),
    "granny": re.compile(r"\bGranny|granny_|granny2", re.IGNORECASE),
    "bink": re.compile(r"\bBink|RAD Game Tools", re.IGNORECASE),
    "mfc": re.compile(r"\bafxwin|afxext|afxcmn|CWinApp|CDialog|CString\b", re.IGNORECASE),
    "msvc": re.compile(
        r"__declspec|__forceinline|__int64|__asm|__debugbreak|#pragma\s+comment|"
        r"#pragma\s+warning|_MSC_VER",
        re.IGNORECASE,
    ),
    "network": re.compile(r"\bsocket|winsock|SOCKET|WSA[A-Z_]+|NetPeer|LAN", re.IGNORECASE),
}

SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx"}
HEADER_SUFFIXES = {".h", ".hh", ".hpp", ".inl"}


@dataclass
class ModuleInventory:
    name: str
    included_in_android_plan: bool
    android_build_verified: bool = False
    android_build_verified_sources: list[str] = field(default_factory=list)
    source_files: list[str] = field(default_factory=list)
    header_files: list[str] = field(default_factory=list)
    blockers: dict[str, int] = field(default_factory=dict)

    @property
    def source_count(self) -> int:
        return len(self.source_files)

    @property
    def header_count(self) -> int:
        return len(self.header_files)


def iter_text_files(module_dir: Path) -> Iterable[Path]:
    for path in module_dir.rglob("*"):
        if path.suffix.lower() in SOURCE_SUFFIXES | HEADER_SUFFIXES:
            yield path


def scan_module(root: Path, name: str, included: bool) -> ModuleInventory:
    module_dir = root / name
    inventory = ModuleInventory(
        name=name,
        included_in_android_plan=included,
        android_build_verified=name in ANDROID_BUILD_VERIFIED_MODULES,
        android_build_verified_sources=ANDROID_BUILD_VERIFIED_SOURCE_SUBSETS.get(name, []),
    )
    if not module_dir.is_dir():
        return inventory

    blockers = {key: 0 for key in BLOCKER_PATTERNS}
    for path in iter_text_files(module_dir):
        rel = path.relative_to(root).as_posix()
        suffix = path.suffix.lower()
        if suffix in SOURCE_SUFFIXES:
            inventory.source_files.append(rel)
        elif suffix in HEADER_SUFFIXES:
            inventory.header_files.append(rel)

        try:
            text = path.read_text(encoding="utf-8", errors="ignore")
        except OSError:
            continue
        for key, pattern in BLOCKER_PATTERNS.items():
            if pattern.search(text):
                blockers[key] += 1

    inventory.blockers = (
        {}
        if inventory.android_build_verified
        else {key: value for key, value in blockers.items() if value}
    )
    return inventory


def write_cmake(path: Path, inventories: list[ModuleInventory], root: Path) -> None:
    lines = [
        "# Generated by tools/android/module_inventory.py",
        "# Do not enable wholesale; add modules after their blockers are removed.",
        "set(BK2_LEGACY_RUNTIME_SOURCES",
    ]
    for inventory in inventories:
        if not inventory.included_in_android_plan:
            continue
        for rel in inventory.source_files:
            lines.append(f"    ${{BK2_SOURCES_ROOT}}/{rel}")
    lines.append(")")
    lines.append("")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--sources-root",
        type=Path,
        default=Path("Versions/Temporary/Engine/Sources"),
        help="Path to the legacy engine Sources directory.",
    )
    parser.add_argument("--json", type=Path, help="Write full inventory JSON.")
    parser.add_argument("--cmake", type=Path, help="Write generated source-list CMake.")
    args = parser.parse_args()

    root = args.sources_root
    if not root.is_dir():
        raise SystemExit(f"Missing sources root: {root}")

    names = sorted(set(RUNTIME_MODULES + EXCLUDED_MODULES))
    inventories = [
        scan_module(root, name, included=name in RUNTIME_MODULES)
        for name in names
    ]

    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(
            json.dumps([asdict(item) for item in inventories], indent=2, sort_keys=True),
            encoding="utf-8",
        )

    if args.cmake:
        write_cmake(args.cmake, inventories, root)

    print("Blitzkrieg 2 Android module inventory")
    for item in inventories:
        status = "runtime" if item.included_in_android_plan else "excluded"
        blockers = ", ".join(f"{key}:{value}" for key, value in sorted(item.blockers.items()))
        print(
            f"{item.name:22} {status:8} "
            f"src={item.source_count:3} hdr={item.header_count:3} "
            f"blockers={blockers or '-'}"
            f"{' android-build-verified' if item.android_build_verified else ''}"
            f"{f' android-source-subset={len(item.android_build_verified_sources)}' if item.android_build_verified_sources else ''}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
