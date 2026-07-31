#!/usr/bin/env python3
"""Builds the Android launcher icon from the original main-menu key art.

The shipped Windows icon (``Sources/Game/main.ico``) tops out at 48x48, which
is far below what a modern launcher asks for, so the icon is cut from the
original 1024x1024 ``Consts/Common/MainMenu/Background_Texture.dds`` instead.
The crop is centred on the key art's soldier and is wide enough that an
adaptive-icon mask, which shows only the middle 66% of the layer, still frames
him.

The output is deterministic: running this again on the same source reproduces
the checked-in PNGs byte for byte.
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:  # pragma: no cover - environment guard
    sys.exit("Pillow is required: pip install Pillow")

# The art is authored for the 1024x768 interface surface; the soldier's head
# and shoulders sit inside this box in that space.
KEY_ART_CROP = (450, 312, 905, 767)

# Legacy square icons, per density bucket.
LEGACY_SIZES = {
    "mdpi": 48,
    "hdpi": 72,
    "xhdpi": 96,
    "xxhdpi": 144,
    "xxxhdpi": 192,
}

# Adaptive-icon layers are 108dp; only the middle 72dp is guaranteed visible.
ADAPTIVE_SIZES = {
    "mdpi": 108,
    "hdpi": 162,
    "xhdpi": 216,
    "xxhdpi": 324,
    "xxxhdpi": 432,
}


def read_uncompressed_dds(path: Path) -> Image.Image:
    """Decodes the A8R8G8B8 DDS the menu background ships as."""
    data = path.read_bytes()
    if data[:4] != b"DDS ":
        raise ValueError(f"{path} is not a DDS file")
    header = struct.unpack("<31I", data[4:128])
    height = header[2]
    width = header[3]
    fourcc = data[84:88]
    rgb_bit_count = header[21]
    if fourcc != b"\x00\x00\x00\x00" or rgb_bit_count != 32:
        raise ValueError(
            f"{path} is not the expected uncompressed 32-bit DDS"
        )
    expected = width * height * 4
    pixels = data[128:128 + expected]
    if len(pixels) != expected:
        raise ValueError(f"{path} is truncated")
    # The original stores B, G, R, A per texel.
    return Image.frombytes("RGBA", (width, height), pixels, "raw", "BGRA")


def write_png(image: Image.Image, path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    # optimize=True keeps the output stable across Pillow's default settings.
    image.save(path, format="PNG", optimize=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--source",
        type=Path,
        default=Path("Versions/Current/Data/Consts/Common/MainMenu/"
                     "Background_Texture.dds"),
        help="Original main-menu background texture",
    )
    parser.add_argument(
        "--res-root",
        type=Path,
        default=Path("android/app/src/main/res"),
        help="Android resource root to write mipmaps into",
    )
    arguments = parser.parse_args()

    art = read_uncompressed_dds(arguments.source)
    key_art = art.crop(KEY_ART_CROP)

    for bucket, size in LEGACY_SIZES.items():
        write_png(
            key_art.resize((size, size), Image.LANCZOS),
            arguments.res_root / f"mipmap-{bucket}" / "ic_launcher.png",
        )
    for bucket, size in ADAPTIVE_SIZES.items():
        write_png(
            key_art.resize((size, size), Image.LANCZOS),
            arguments.res_root / f"mipmap-{bucket}" /
            "ic_launcher_background.png",
        )
    print(
        f"Wrote launcher icons for {len(LEGACY_SIZES)} density buckets from "
        f"{arguments.source}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
