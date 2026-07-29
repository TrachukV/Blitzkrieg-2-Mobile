[English](README.md)        [Русский](README_Russian.md)        [中文](README_Chinese.md)        [हिन्दी](README_Hindi.md)        [Español](README_Spanish.md)        [Français](README_French.md)        [Deutsch](README_German.md)        [Português](README_Portuguese.md)        [日本語](README_Japanese.md)        [Bahasa Indonesia](README_Indonesian.md)

> [!NOTE]
> This is a community fork of the original
> [nival/Blitzkrieg-2](https://github.com/nival/Blitzkrieg-2) repository,
> focused on a full native Android mobile port. The original game, source code,
> trademarks, and assets remain the property of their respective owners. This
> port follows the original repository's non-commercial license.

[![Blitzkrieg II Trailer](Blitzkrieg_2.png)](https://www.youtube.com/watch?v=Cw8rA2hvDGg)

The computer game [Blitzkrieg 2](https://en.wikipedia.org/wiki/Blitzkrieg_2) is the second installment of the legendary series of real-time strategy war games, developed by [Nival Interactive](http://nival.com/) and released in 2005.

The game is still available on [Steam](https://store.steampowered.com/app/313500/Blitzkrieg_2_Anthology) and [GOG.com](https://www.gog.com/en/game/blitzkrieg_2_anthology).

In 2025, the game's source code was released under a [special license](LICENSE.md) that prohibits commercial use but is completely open for the game's community, education and research.
Please review the terms of the [license agreement](LICENSE.md) carefully before using it.

## Tech stack

- **Game engine**: Custom 3D engine, mostly written in C++
- **Scripting language**: Lua
- **Animation**: Granny Animation (RAD Game Tools) ⚠️ *Commercial license - not included*
- **Video**: Bink Video Technology ⚠️ *Commercial license - not included*
- **Audio**: FMOD sound system ⚠️ *Commercial license - not included*

## What is in this repository

- `Complete` — game data and resources
- `Design` — design documents and art resources  
- `Soft` — source code and development tools
- `Sound` — game sound resources
- `Tools` — development and build tools
- `Localizations` — localization files
- `Versions` — different build configurations and testing environments
- `Versions/Temporary/Engine/Sources` — complete game engine source code

## Android mobile port status

The Android port is a playable native ARM64 milestone, not a finished
release. It currently runs original single-player campaign data through the
legacy C++ AI/game simulation, renders original terrain, models, materials, and
supported infantry animations through bgfx, renders the live legacy fog-of-war
grid and hides enemies the player's party cannot see, and uses the shipped
mission HUD, minimap, unit portraits, and hit bars. Its original 4x3 command
grid is rebuilt from the selected legacy unit's available actions instead of a
fixed mobile button row; Move, Attack, Rotate, Stop, Entrench, Stand Ground,
Spyglass, Clear Mines, Place Mines, and the original two-point Build Trenches
command reach the original simulation, while commands without a ported handler
use their original disabled artwork. Original combat textures now provide
tracer ribbons, muzzle flashes, and animated fire/smoke on destroyed mechanized
units. Static objects and live units now cast translucent projected silhouettes
derived from their actual converted Granny vertices; animated infantry shadows
follow the current frame instead of using fixed circles. The battlefield camera
now reads the original mission's 26-degree horizontal FOV, 45-degree default
pitch/yaw, and 150/170/200 distance limits from the loaded legacy
`ClientGameConsts`. Double-tapping a selected unit expands the selection to
nearby units of the same legacy type (up to twelve), gives each unit an original
selection marker and HUD card, and sends Move, Attack, Stop, and supported abilities
through one legacy AI command group. Holding for 350 ms and then dragging draws
an original-style selection rectangle and selects up to twelve friendly units
of any type inside it; a quick drag still pans the camera. Tapping a member card
makes that unit active, so its type-specific actions replace the command grid
while common commands continue to address the whole selection. Native pause
from `P` or `Space` now displays the original orange `PAUSED` treatment centered
over the battlefield without covering the shipped bottom HUD. The top-left
mission line now comes from the localized header of the active primary legacy
objective instead of exposing the internal campaign/map identifier. Original
scenario notifications temporarily replace that line for five seconds and
then restore the objective; the legacy reinforcement feedback now displays the
shipped `The Reinforcement Has Arrived` text instead of an Android-authored
copy. See
[`android/README.md`](android/README.md) for build, content-staging, verification,
and remaining-port details.

---

# Running the game

## Basic launch
1. Navigate to the `Complete/bin/` directory
2. Run the game executable (if available)

---

# Map editor and development tools

## Map editor
- **Location**: `Complete/Editor/`
- **Documentation**: `Design/Manuals/MapEditorManual/`
- **Manual**: `Design/Manuals/MapEditorManual/Final/`
- **FAQ**: `Design/Manuals/MapEditorManual/FAQ/`

## Development tools
- **Maya plugins**: `Tools/MayaScripts/`
- **Texture converters**: `Tools/TexConv.exe`, `Tools/DxTex.exe`
- **Font generator**: `Tools/FontGen.exe`
- **Granny tools**: `Tools/Granny/`

---


# Building the project

## Build requirements
- Microsoft Visual Studio (2003)
- DirectX SDK
- Additional dependencies are specified in the documentation

---

## License Information

This project is released under a **special non-commercial license** from NIVAL INTERNATIONAL LTD.

### ✅ What's included and open source:
- **Game engine source code** - Custom license from NIVAL INTERNATIONAL LTD (non-commercial use only)
- **zlib compression library** - zlib License (permissive, commercial use allowed)
- **Game scripts, assets, and data** - Custom license from NIVAL INTERNATIONAL LTD (non-commercial use only)

### ⚠️ Additional tools not included in source code:
- **FMOD Audio System**
- **Bink Video Technology**
- **Granny3D Animation System**
- **Stingray Studio UI Components**
- **MySQL Database**
- **S3TC Texture Compression**

### 📋 Third-party licenses:
- **zlib** (v1.1.3) - Copyright (C) 1995-1998 Jean-loup Gailly and Mark Adler - zlib License

Please review the complete [license agreement](LICENSE.md) before using this code.
