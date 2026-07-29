#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
ANDROID_DIR="${REPO_ROOT}/android"
DATA_SOURCE="${BK2_DATA_SOURCE:-${REPO_ROOT}/DataAndroid}"
PACKAGE="com.nival.blitzkrieg2"
ADB_BIN="${ADB:-adb}"
APK="${ANDROID_DIR}/app/build/outputs/apk/debug/app-debug.apk"
GEOMETRY_SOURCE="${DATA_SOURCE}/Data/bin/Geometries"
CONVERTED_ROOT="${DATA_SOURCE}/Converted"
CONVERTED_GEOMETRY="${CONVERTED_ROOT}/Geometries"

if [[ -z "${ANDROID_HOME:-}" &&
      -d "${HOME}/Library/Android/sdk" ]]; then
    export ANDROID_HOME="${HOME}/Library/Android/sdk"
fi
if [[ -z "${ANDROID_SDK_ROOT:-}" &&
      -n "${ANDROID_HOME:-}" ]]; then
    export ANDROID_SDK_ROOT="${ANDROID_HOME}"
fi

if ! "${ADB_BIN}" get-state >/dev/null 2>&1; then
    echo "No ready Android device or emulator was found." >&2
    exit 1
fi

if [[ "${BK2_SKIP_GEOMETRY_CONVERSION:-0}" != "1" ]]; then
    if [[ ! -d "${GEOMETRY_SOURCE}" ]]; then
        echo "Granny geometry source is missing: ${GEOMETRY_SOURCE}" >&2
        exit 1
    fi
    (
        cd "${SCRIPT_DIR}"
        npm install --ignore-scripts
        node convert_granny_geometry.mjs \
            --input "${GEOMETRY_SOURCE}" \
            --output "${CONVERTED_GEOMETRY}" \
            --idle-animation "${DATA_SOURCE}/Data/bin/Animations/3977" \
            --move-animation "${DATA_SOURCE}/Data/bin/Animations/3967" \
            --skip-unsupported \
            --all
        python3 build_geometry_index.py \
            --data-root "${DATA_SOURCE}/Data" \
            --converted-geometry-root "${CONVERTED_GEOMETRY}" \
            --output "${DATA_SOURCE}/Converted/geometry_index.tsv"
    )
fi

if [[ "${BK2_SKIP_BUILD:-0}" != "1" ]]; then
    (
        cd "${ANDROID_DIR}"
        ./gradlew :app:assembleDebug --no-daemon
    )
fi

if [[ ! -f "${APK}" ]]; then
    echo "Debug APK is missing: ${APK}" >&2
    exit 1
fi

"${ADB_BIN}" install -r "${APK}"

if "${ADB_BIN}" shell run-as "${PACKAGE}" \
        test -r files/DataAndroid/Data/types.xml &&
   "${ADB_BIN}" shell run-as "${PACKAGE}" \
        test -r files/DataAndroid/Data/index.bin &&
   "${ADB_BIN}" shell run-as "${PACKAGE}" \
        test -d files/DataAndroid/Data/Scenario &&
   "${ADB_BIN}" shell run-as "${PACKAGE}" \
        test -r files/DataAndroid/Data/Terrain/TGTerraSet/Sets/Asia_TGTerraSet.xdb &&
   "${ADB_BIN}" shell run-as "${PACKAGE}" \
        test -r files/DataAndroid/Data/Scene/TexAndMats/All/Terrain/Sets/Asia/Grass_Texture.dds; then
    echo "Game data is already staged in app-private storage."
else
    if [[ ! -r "${DATA_SOURCE}/Data/types.xml" ||
          ! -r "${DATA_SOURCE}/Data/index.bin" ||
          ! -d "${DATA_SOURCE}/Data/Scenario" ||
          ! -r "${DATA_SOURCE}/Data/Terrain/TGTerraSet/Sets/Asia_TGTerraSet.xdb" ||
          ! -r "${DATA_SOURCE}/Data/Scene/TexAndMats/All/Terrain/Sets/Asia/Grass_Texture.dds" ]]; then
        echo "Incomplete DataAndroid source: ${DATA_SOURCE}" >&2
        exit 1
    fi
    echo "Staging DataAndroid into app-private storage; this is a multi-GB transfer."
    "${ADB_BIN}" shell run-as "${PACKAGE}" mkdir -p files/DataAndroid
    COPYFILE_DISABLE=1 tar -chf - -C "${DATA_SOURCE}" . |
        "${ADB_BIN}" shell run-as "${PACKAGE}" tar -xf - -C files/DataAndroid
fi

if [[ -d "${CONVERTED_GEOMETRY}" &&
      -f "${CONVERTED_ROOT}/geometry_index.tsv" ]]; then
    echo "Staging converted original models into app-private storage."
    "${ADB_BIN}" shell run-as "${PACKAGE}" mkdir -p files/DataAndroid/Converted
    COPYFILE_DISABLE=1 tar -chf - -C "${CONVERTED_ROOT}" . |
        "${ADB_BIN}" shell run-as "${PACKAGE}" tar -xf - \
            -C files/DataAndroid/Converted
fi

"${ADB_BIN}" shell am force-stop "${PACKAGE}"
"${ADB_BIN}" shell am start \
    -n "${PACKAGE}/.MissionSelectActivity"
echo "Blitzkrieg 2 mission selector is running on the Android device."
