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
grid and hides enemies the player's party cannot see, restores sea/lake
surfaces from each map's original `seaMask` with the season-specific DDS and
descriptor-timed waves, reconstructs terrain roads from their original
`SRoadDesc`/`SVSOInstance` curves and seasonal DDS atlases, carves and renders
descriptor-driven rivers from the original `SRiverDesc` control points and
water materials, including the seeded bank disturbance and per-layer
`StreamSpeed` UV motion, rebuilds the original terrain precipices — crag
ridges, their foot skirts, and both river bank walls — from each map's
serialized `SPrecipice` columns and the shipped ridge/precipice materials, and
uses the shipped mission HUD, minimap, unit portraits, and hit bars. Its original 4x3 command grid is rebuilt from the
selected legacy
unit's available actions instead of a fixed mobile button row; Move, Attack,
Rotate, Stop, Entrench, Stand Ground,
Spyglass, Clear Mines, Place Mines, and the original two-point Build Trenches
command reach the original simulation, while commands without a ported handler
use their original disabled artwork. The original minimap-corner Esc/F10 and
Objectives buttons now use their shipped art, XDB placement, and live Android
actions instead of invisible touch zones. The minimap also applies the legacy
map-specific DDS background and AI war-fog snapshot with the original
128-alpha darkening, accepts tap/drag camera navigation inside its diamond, and
draws the original magenta viewport polygon from the current 3D camera. The APK
carries the small original HUD subset as a fallback, so an incomplete external
content sync no longer replaces that panel with a dark debug rectangle.
Campaign mission markers now open the shipped mission-briefing screen before
combat instead of bypassing it. The port fills its original panels with the
selected map's localized name, wrapped operations order, objective summary,
and DDS minimap; Back returns to the chapter map and Play starts that exact
mission. Starting a new campaign now uses the `CampaignSelection2` screen
registered by `GameRoot.xdb`, rather than the obsolete four-card descriptor.
Its three production panels are populated from the real USA, Germany, and USSR
campaign records with the original names, descriptions, DDS artwork, selected
state, and four-level difficulty mapping before opening the selected campaign's
chapter map.
The chapter map now follows the original `CInterfaceChapterMapMenu` state
flow instead of treating every map location as a direct launch button. It
builds enabled, locked, completed, recommended, and selected target states from
the campaign tracker, renders the matching shipped marker state, selects the
lowest-order recommended mission, and blocks Play for locked targets. The
right panel updates the localized mission name, chapter/mission reserve
counters, available reinforcement branches, reward icons, final-mission panel,
and enabled light for the selected target. Completed progress is consumed from
the Android mission-runtime checkpoint when returning from an Action Report;
a newly selected campaign starts from the chapter descriptor's original
availability rules.
Selecting a chapter-map target also restores the original road-arrow layer:
the port reads the selected mission and dependency fields from the shipped
chapter `roads`, converts every authored polyline into rotated textured
segments, and consumes each arrow texture once across its full route just as
`CWindowPotentialLines::DrawArrows` does. Main-mission dependency arrows use
the original reduced alpha until their prerequisite target is completed. The
four final arrow DDS files are bundled as a non-destructive fallback because
their sparse-checkout XDB references are absent; external full-game data still
takes precedence.
The chapter map also regenerates the original `CWindowPotentialLines` state
from the current campaign tracker. It reads the chapter's sea/noise mask and
alternate-colour TGA, applies the authored main-strike gradient and each
mission's incomplete/completed potential at its details-map node, then builds
the masked territory and ten-pixel zero contour before arrows and markers.
After a successful mission the last node now starts at its incomplete value
and follows the desktop controller's five-second interpolation to its completed
value. The generated texture updates in place and the transition is requested
only by the successful statistics-to-chapter route.
The chapter and selected-mission reinforcement counters also use the original
animated number strip rather than replacement font glyphs. Android packages a
lossless DDS atlas built from all 1,440 frames of `Number18x33.bik` and follows
the desktop `PlayRollerAnim` decimal-place, direction, wrap, 30 fps, and
two-second duration-cap rules when entering the chapter, selecting another
mission, or returning after a win.
The map renderer now includes general `SObjectRPGStats` records in addition to
buildings, fences, entrenchments, squads, and mechanized units. This restores
the original mission's vegetation and small props through their converted
Granny geometry and DDS materials. Flora now uses the desktop
material's declared alpha mode instead of a path-based guess: alpha-tested
leaves use the original reference value of 120 and transparent brushwood and
reeds use blending. This removes the opaque black texture cards around
vegetation. Alpha-tested flora writes depth only for surviving texels and casts
translucent projected shadows through the same texture mask instead of the
port's coarse full-card convex hull. On USA US1.2 the rendered static-object
count increased from 215 to 2,180 of 2,254 map records with zero missing
converted geometries.
Original combat textures now provide tracer ribbons and muzzle flashes. Hit and
death events also resolve their original `SComplexEffect` XDB descriptors,
including emitter timing, texture sequences, scale, speed, and alpha/additive
blend mode. Descriptor light instances add a short original-texture additive
ground flash; the old fixed fire/smoke recipe is retained only as a fallback
for content without a usable descriptor. Moving mechanized units now also
trigger their authored `EffectDiesel` recipe once at the transition into
movement and place its small exhaust puffs on the exact converted
`LExhaust*` skeleton locators, matching the desktop
`CMOUnitMechanical::AIUpdateMovement` lifecycle instead of emitting generic
smoke from the hull center. Destructible buildings, bridge spans,
and connected fence sections are published from their live legacy AI objects
instead of immutable scenery. Their shipped damaged/destroyed models follow
the original HP or frame state; breaking a fence also switches both connected
neighbors to the original left/right damaged visuals. Trench segments use the
same live path, so engineering construction created after mission startup now
appears with the original line, fireplace, arc, and terminator models while
respecting fog-delayed appearance. Static objects and live
units now cast
translucent projected silhouettes
derived from their actual converted Granny vertices; animated infantry shadows
follow the current frame instead of using fixed circles.

Terrain and converted Granny geometry now also carry their original normals
into bgfx instead of being submitted as unlit texture cards. Terrain follows
the desktop `SPreLight` calculation, including its separate light/shade
ambient values and always-on 4x whitening scale, while static, CPU-posed, and
GPU-skinned models use each map's `SAmbientLight` direction and colors. The
Daylight setup on USA US1.0 and the differently directed Summer Sunset setup
on US1.2 have both been exercised on the ARM64 GLES3 emulator.

The battlefield camera
and terrain projection use the render-surface height minus the actual 112dp
mission HUD inset, so units remain visible and touch picking stays aligned in
the playable area above the original bottom panel. The battlefield camera
now reads the original mission's 26-degree horizontal FOV, 45-degree default
pitch/yaw, and 150/170/200 distance limits from the loaded legacy
`ClientGameConsts`, then applies the shipped local-player camera anchor and any
map-specific placement override. Double-tapping a selected unit expands the
selection to nearby units of the same legacy type (up to twelve), gives each
unit an original selection marker and HUD card, and sends Move, Attack, Stop,
and supported abilities through one legacy AI command group. Holding for
350 ms and then dragging draws
an original-style selection rectangle and selects up to twelve friendly units
of any type inside it; a quick drag still pans the camera. Tapping a member card
makes that unit active, so its type-specific actions replace the command grid
while common commands continue to address the whole selection. Native pause
from `P` or `Space` now displays the original orange `PAUSED` treatment centered
over the battlefield without covering the shipped bottom HUD. The top-left
mission line now comes from the localized header of the active primary legacy
objective instead of exposing the internal campaign/map identifier. Up to three
active original scenario notifications temporarily replace that line for five
seconds and then restore the objective; the legacy reinforcement feedback now
displays the shipped `The Reinforcement Has Arrived` text instead of an
Android-authored copy. See
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
