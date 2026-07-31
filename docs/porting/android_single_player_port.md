# Android Single-Player Port Status

## Playable Vertical Slice

The current ARM64 build runs the original single-player legacy AI/game
simulation and renders the mission through bgfx with staged terrain, converted
original unit/static geometry, materials, supported infantry animation, legacy
fog of war, descriptor-driven water, combat effects, minimap, selected-unit
cards, and the shipped
mission HUD. The right action panel follows the original twelve-slot layout and
is rebuilt from the selected `CAIUnit` action data. Move, Attack, Stop,
Entrench, Stand Ground, Rotate, Spyglass, Clear Mines, Place Mines, and Build
Trenches call the legacy command path. Rotate, Spyglass, Clear Mines, and Place
Mines consume the next valid terrain tap as their forced point. Build Trenches
collects a start and end point and emits the same `ENTRENCH_BEGIN` /
`ENTRENCH_END` command pair as the desktop client. Visible actions whose mobile
interaction is not implemented remain disabled instead of pretending to work.
The minimap-corner Esc/F10 and Objectives controls use the shipped
`EscMenuBtn_003.tga` and `ObjectivesBtn_002.tga` art at the original
descriptor coordinates `(7,92)` and `(193,92)`. They open the native mission
menu and objective summary instead of leaving invisible touch targets beside
the command grid. Mission play also enters sticky immersive mode so the Android
navigation handle does not cover the center panel.
The minimap decodes each map's original `MapInfo.pMiniMap` DDS through the
legacy texture backend, falling back to terrain-type colors only when the map
has no usable texture. It samples the same `GetMiniMapWarForInfo` snapshot as
the battlefield and follows the desktop `WindowMiniMap` constants: hidden
cells receive a black overlay with alpha 128 and fully visible cells receive
alpha 0. Friendly and currently visible hostile presentation entities are
drawn after that layer, with the active selection highlighted in yellow.
Touches are clipped to the original diamond; tap and one-finger drag convert
its normalized coordinates back to terrain coordinates, update the shared
camera target and terrain height, and leave the current zoom and yaw intact.
The desktop `pViewPortLayer` is also restored: four rays through the actual
bgfx content viewport intersect the terrain and produce the original magenta
camera polygon above fog and unit markers. On the ARM64 emulator `US1.2` loaded
its shipped 256x256 minimap; two taps moved both the camera and polygon from
`177.692,184.25,0` to `130.549,213.714,3.99608`.
The static presentation pass also includes general `SObjectRPGStats` map
records. USA US1.2 contains 1,965 such vegetation and small-prop records, which
were previously omitted even though their converted Granny geometry was
available. The verified runtime count increased from 215 to 2,180 rendered
objects out of 2,254 map records with `missing_converted_geometry=0`. Flora
uses its original DDS material and the `SMaterial::AlphaMode` exported beside
each texture in `geometry_index.tsv`; this replaces the old case-sensitive
flora-path heuristic. `AM_ALPHA_TEST` leaves go through a dedicated fragment
shader, which discards texels below the desktop renderer's reference value of
120 and keeps depth writes enabled. `AM_TRANSPARENT` brushwood and reeds use
blending, so their texture cards no longer render as opaque black rectangles.
Distinct material layers prevent a shared texture from merging incompatible
alpha modes. Checked-in GLES3 and SPIR-V alpha-test binaries cover both Android
bgfx backends.

Alpha-tested flora shadows reuse the projected Granny triangles and UVs with a
second masked shader, so the ground receives translucent leaf and branch
silhouettes rather than the complete crossed billboard cards. Opaque small
props keep their convex projected shadows. GB3.1 produced 43 alpha-test layers
with 69,638 triangles and 10 blended world layers with 30,580 triangles; the
blend total also includes its road and river passes. US1.2 produced 19 masked
shadow layers and 199,194 projected triangles. Unit `5198` remained selectable
over the restored vegetation and populated its original world ring, portrait,
health card, and action grid.

Shadow direction is no longer a global Android constant. The renderer resolves
the map's `SAmbientLight`, applies the desktop shadow-pitch fallback rule, and
projects with the descriptor's pitch/yaw. Opaque convex silhouettes are
clipped per heightfield triangle, while alpha-masked foliage vertices sample
the same terrain surface; both therefore follow slopes rather than floating
at the object's base height. Crossed foliage cards use reduced opacity to
avoid accumulating into the dark rectangular blocks visible in the earlier
port. US1.0 resolved Summer Daylight pitch `27`, yaw `40`, vector
`0.390319,-0.327517`, blur strength `1.5`, and a `193x193`
shadow heightfield. It submitted 6,624 terrain-clipped opaque shadow triangles
and 189,583 masked triangles in 19 foliage layers.
`Data/Scene/Lights` is now an explicit sparse-checkout, staging, and
incremental-install requirement.
US1.2 then resolved the distinct Summer Sunset descriptor at pitch `50`, yaw
`215`, vector `-0.976227,0.683562`, submitting 3,204 terrain-clipped opaque
triangles and 199,194 masked triangles without a renderer failure.
Combat presentation now also consumes real `SAIHitUpdate` records. The Android
bridge follows the desktop shell mapping for hit, miss, reflection, ground,
water, and air impacts, then snapshots the selected `SComplexEffect` variant.
Mechanized death similarly selects `pEffectSmoke` or `pEffectFatality` from the
unit RPG descriptor according to the legacy death animation. The bgfx particle
path preserves the descriptor's emitter timing, texture sequence, scale, speed,
count, and alpha/additive blend mode. Descriptor light instances also preserve
their placement and cycle timing and use the shipped additive
`LightFX/Flare_Texture.dds` for a ground pulse. In a verified US1.2 emulator
run, the scenario triggered `AntitankMineExplosion_ComplexEffect.xdb`; Android
loaded both emitters, their shipped `Explosion1_Texture.dds`, and one light
instance backed by `Flare_Texture.dds`, then removed the effect after its
declared lifetime without a process failure.
Long combustion recipes retain their full descriptor lifetime instead of being
clipped to twelve seconds. Mechanized wreck geometry remains until the
simulation explicitly sends `ACTION_NOTIFY_DISSAPEAR_OBJ`; Android then plays
the unit's original `pEffectDisappear` recipe when requested and removes the
wreck. Infantry bodies clear after thirty seconds. The debug installer
explicitly stages `Scene/Effects/All/Destructions`,
`Scene/Effects/All/Exhaust`, `Effects/_Lights`, `Other/Projectile`, and the
shipped effect texture tree even when the rest of the multi-gigabyte game data
was already present on the device. It also refreshes `Spots` and
`Scene/TexAndMats/All/Objects/TerraObjects` for the terrain overlay renderer.
The old fixed fire/smoke recipe now runs only when no usable descriptor exists.
The compiled desktop particle keyframe tracks are not available to this path,
so per-particle position, size, and color curves remain a billboard
approximation rather than a byte-identical rendering of the original effect.
The approximation derives the missing visual size from the original smoke,
fire, and flash texture families; using the DDS pixel dimensions alone made a
64-pixel combustion frame only two world units wide and effectively invisible
at the desktop camera distance.
Lua `PlayEffect` calls now reach the same bridge through their native
`SPlayEffectUpdate`. This restores mission-authored smoke screens, bridge
demolitions, and other positioned `ScriptEffects` instead of consuming those
updates without presenting them.
The projectile presentation path now consumes the original
`SAINewProjectileUpdate`, `SAIPlacementUpdate`,
`SExplodeProjectileUpdate`, and dead/disappear updates. It resolves the exact
shell `SProjectile`, renders its converted model without a unit health bar or
team tint, moves its attached exhaust descriptors with the AI placement, and
removes it on the same lifecycle event as the desktop world.
Destructible buildings are no longer baked into the immutable scenery mesh.
The presentation bridge enumerates the live `CExistingObject` building set,
preserves its unique AI id, placement, visibility, HP, and heading, and selects
the same `whole` / `damaged01` / `damaged02` / `damaged03` / `destroyed`
`SVisObj` that the desktop `ChooseVisObjForHP` path selects. The geometry index
keys every damage-level visual by its exact resource path, so stage changes do
not fall back to proxy boxes or overlap an undamaged static copy. Damage effects
advance only when the building reaches a worse stage and reproduce the desktop
smoke-point transform for every `SBuildingRPGStats::smokePoints` entry. A
verified RUS3.1 run published 62 live buildings, three visible to the local
party at startup, with zero geometry fallbacks. A non-key railroad house
progressed from 100 to 60, 20, and 0 HP, rendered its final ruin, played the
shipped building fatality audio, and selected
`StHouseCrashPhase_ComplexEffect.xdb` followed by
`StHouseCrashTotal_ComplexEffect.xdb`.
Bridge spans now use the same live-object path instead of remaining inside the
immutable scenery batch. Android enumerates `CBridgeSpan`, preserves its
frame index, visibility, HP, placement, and heading, and selects the desktop
`end` model for frame zero or `center` model for every other frame. RPG stage
updates use the matching element's `SBridgeDamageState`, reproduce the original
smoke-point/origin rotation, and play its shipped `pSmokeEffect` when the
descriptor supplies smoke points. A destroyed span is rendered through the
death animation declared by that model's own skeleton, including the original
per-span collapse delay. `convert_bridge_death_geometry.py` discovers all 14
center/end geometry bindings under the six shipped `Data/Bridges` families;
the Granny converter now correctly treats a mesh with one bone binding and no
redundant vertex-weight array as rigidly attached to that bone.
Bridge centers are already terrain-adjusted by
`CStaticObjects::AddNewBridgeSpan`; the Android presentation path therefore
uses that absolute Z directly instead of adding `GetVisZ` twice. On-device
validation destroyed all five visible asphalt spans in `GER4.2`, selected
geometry 373's death animation, and left the bridge destroyed. A second launch
of `RUS3.1` loaded its three initially destroyed railway spans directly at
geometry 509's final death frame. All bridge bindings resolved to converted
geometry in both checks.
Fence sections use the same live-object boundary instead of remaining baked
into scenery. Android enumerates `CFence`, resolves its current
`GetFrameIndex()` through `SFenceRPGStats::GetVisObjByFrameIndex`, and publishes
the exact center, left-damaged, right-damaged, or destroyed visual selected by
the legacy simulation. The geometry index keys each fence `SVisObj` by exact
resource path, so a live frame transition does not require extending or
duplicating the simulation state in the presentation API. Destroyed sections
keep their original broken mesh and report zero HP; terrain Z, heading,
diplomacy, and fog visibility remain simulation-driven.

The RUS3.1 device check enumerated 618 fence sections and published 13 visible
sections at mission start. Destroying one field-fence section through the
original `CFence::Die`/`Delete` path changed its frame from 0 to 4 and changed
both connected neighbors to partial-damage frames. The rendered result retained
zero static and dynamic geometry fallback types.
Entrenchment parts are now live presentation entities too. Each
`CEntrenchmentPart` retains its unique legacy id, current segment frame,
terrain placement, heading, and `ACTION_NOTIFY_NEW_ST_OBJ` suspension state.
The renderer hashes the exact segment `SVisObj`; both the frame-index binding
used by map loading and the exact-path binding used by live construction remain
in `geometry_index.tsv`. This lets trenches built after world-mesh creation
appear without duplicating the static copy and keeps unrevealed enemy/neutral
construction hidden.

US1.0 exposes 71 unique live trench parts, 25 visible at startup, with zero
geometry fallback types. A device-only construction check invoked the original
`CEntrenchmentCreation::PreCreate` and `BuildAll` path after startup and raised
the part count from 71 to 75: two segment models and two terminators appeared
without rebuilding scenery. `CEntrenchment::TakeDamage` remains the intentional
2005 no-op from the desktop game; Android does not invent a destruction state.

Mines also leave the immutable scenery mesh and are published from live
`CMineStaticObject` state. Registration preserves the original discovery
boundary, while removal from the legacy link-object map makes detonation remove
the rendered mine without a presentation-only lifetime. Mine placement still
uses `CUnitCreation::CreateMine`, and detonation still creates the original
`CInvisShell`/`CBurstExpl` from the mine weapon descriptor.

The US1.0 runtime check placed universal mine 8216 after mission startup,
changing the live count from zero to one with the original visual and no
geometry fallback. `CMineStaticObject::Detonate` then changed the count back to
zero and forwarded the shipped antipersonnel and antitank mine explosion
descriptors observed during the validation run to the Android particle/light
renderer; the direct debug detonation selected the antitank descriptor.

Static objects with `SObjectRPGStats::bCanFall` now use a live presentation
path rather than the cached scenery mesh. Android snapshots each living
`CCommonStaticObject`, receives the original `SAITreeBrokenUpdate`, and keeps
the model after the simulation deletes the AI object. Root geometry and both
shadow paths use the same fall axis, terrain-slope end angle, duration, cycle
count, and damped-cosine coefficient as `CTreeFallingMutator`. The seasonal
`SComplexSeasonedEffect`, or the non-seasonal fallback, is resolved from the
object descriptor and forwarded to the existing effect renderer.

US1.0 exposed 1081 live fallable objects and 157 visible objects at startup.
Knocking down `Palm01` object 7108 through
`CCommonStaticObject::AnimateFalling` reduced the live count by one, delivered
the tree-broken client update, rendered `PalmFall_ComplexEffect`, and left the
fallen model in the world with zero geometry fallbacks. Random per-leaf-joint
motion from the desktop mutator remains a renderer follow-up; the root fall
and simulation lifecycle are live.

The compiled animated-light intensity track is similarly represented by a
short flash envelope rather than a full dynamic-light shader.
The Android build packages the required 4.2 MiB original HUD subset and the TGA
loader uses it only when the corresponding external `Complete/UI` file is
missing. This keeps the original panel functional after a partial content sync
without replacing the full external content layout. The view also restores the
desktop mission's black backdrop beneath the TGA layers instead of exposing the
full-screen Android render surface through the center panel.
The Java HUD reports its actual 112dp height to the renderer. The bgfx terrain
view, camera projection, touch picking, drag selection, and gesture bounds all
use the remaining content height instead of projecting the battlefield behind
the panel. On the ARM64 emulator this produces a `2856x944` playable view over a
`2856x1280` surface with a 336-pixel bottom inset. A tap at `(1240,390)` then
selected legacy unit `5198` and immediately populated its original HUD card,
confirming that rendering and touch projection remain aligned.
Double-tapping a selected unit expands the selection to up to twelve nearby
units with the same legacy stats/type. The renderer marks every selected
`CAIUnit`, the HUD shows a health card for every member, and commands are sent
through a registered `CGroupLogic` group so the original subgroup movement
logic remains active. A 350 ms hold followed by a one-finger drag draws a
screen-space selection rectangle and selects up to twelve friendly `CAIUnit`
instances of any type inside it; a quick one-finger drag remains camera pan.
Tapping a member card moves that unit to the active slot. Common desktop
actions address the full selection, while type-specific actions are derived
from and dispatched to the active unit's matching legacy stats group. `P` and
`Space` toggle the native pause state. The Java HUD polls that state and centers
the orange `PAUSED` label over the playable viewport while leaving the original
bottom panel visible, matching the desktop mission presentation. A dedicated
JNI headline call resolves the active primary `SMissionObjective` header through
the shipped UTF-16 localization and displays it at the top-left; the internal
campaign/map ID is now only a startup fallback. A thread-safe stack of up to
three active five-second messages temporarily replaces that header with
original objective and reinforcement notifications. It consumes
`EFB_OBJECTIVE_CHANGED` and
`EFB_REINFORCEMENT_CENTER_LOCAL_PLAYER`, resolves the shipped UTF-16 text
resources, and restores the active objective after expiry. Converted static and
dynamic models also produce one translucent ground silhouette from the convex
hull of their sun-projected Granny vertices. The dynamic path samples the same
current runtime-skinned pose used by the visible model, so infantry
shadows move with the unit rather than remaining fixed proxy circles. The
battlefield camera loads horizontal FOV, default pitch/yaw, and
min/average/max distance from the legacy `ClientGameConsts`, then applies
`SMapInfo::players[local].camera` after the original AI/map load. The anchor is
always used; distance, pitch, and yaw are replaced only when the shipped
`UseAnchorOnly` flag is false, matching the desktop
`InterfaceMissionInternal` order. Screen projection, terrain picking, drag
selection, and bgfx rendering all consume that one camera state.
Water has its own backend resource path: terrain stays in immutable buffers,
while `STerrainInfo::seaMask` is compacted into a dedicated 32-bit-indexed
dynamic water mesh. It uses the original constant surface height `z=0.1`,
seasonal DDS selection, wrapping texture coordinates, transparent coastline
vertices, and the active `SWater` descriptor's first amplitude/period and
tiling values. The backend updates only the water vertex buffer at 20 Hz and
submits it between terrain and world objects. A US1.2 ARM64 run loaded the Asia
water DDS and reported 2,575 mask nodes, 3,021 rendered nodes, and 5,532
triangles with no runtime error.
Terrain roads now follow the original `RoadsBuilder.cpp` data path without
linking the old D3D scene renderer. The Android runtime walks
`STerrain::roads`, converts every `SVSOPoint` position and half-width from AI
to visual coordinates, derives layer widths and U ranges from the center and
optional border spans in `SRoadDesc`, advances V by segment length, preserves
per-point and descriptor opacity, and samples the heightfield for the four
ribbon corners at `z+0.1`. The current binary DB view can resolve the
`SMaterial` resource while leaving its `pTexture` unloaded, so a descriptor-name
fallback selects the corresponding shipped seasonal road DDS. US1.2 verified
12 rendered road instances, 524 segments, 1,048 triangles, three distinct Asia
DDS paths, and three ready GPU texture handles. The visible starting-area road
also confirms that the required `AI2Vis` transform is applied; using the raw
0-to-8192 editor coordinates would place all road geometry outside the
0-to-352 rendered world.
Terrain rivers now use the same coordinate and descriptor path. Before terrain
material layers are copied, the runtime applies the four-unit depth modifier
from `RiversBuilder.cpp` to the shared heightfield and reproduces its four-tile
ridge transition. It then emits the original eight-cell bottom ribbon and up to
two alpha-water ribbons using the `SRiverDesc` cell counts, center opacity,
tiling, per-point width/opacity, and seasonal `bottom.dds`, `water.dds`, and
`water2.dds`. A local implementation of the original Win32 LCG starts from
`GetVSOSeed`'s control-point-distance seed, applies the same `fBorderRand`
offset to the carved terrain and ribbon centerline, and consumes the remaining
sequence for each layer's internal-cell `fDisturbance`. It is isolated from the
gameplay RNG. Each water layer's V coordinate also advances by the original
signed `fStreamSpeed` against simulation animation time.

GB3.1 verified one 279-point river, 278 segments, 11,676 triangles, 3,348
disturbed internal water vertices, 1,933 carved heightfield vertices, two
animated water layers, and three ready Summer DDS GPU handles.
Terrain precipices complete the same terrain layer. The runtime walks every
serialized `STerrainInfo::SPrecipice` with the original
`PrecipicesRender.cpp` `CreatePrecipiceMesh` rules: per-node vertex columns
clipped to the node's `minHeights`/`maxHeights`, the desktop
`DEF_MIN_PRECIPICE_HEIGHT` skip for flat columns, the same interpolated column
stitching, and `fTexGeomScale`-scaled U/V from planar column distance and 3D
edge length. `SFoot` skirts are rebuilt from their `pFootMaterial` as blended
layers. Both precipice sources from `PrecipicesManager.cpp` are covered: crag
precipices resolve `SCragDesc::pRidgeMaterial` by crag VSO id, and river bank
precipices resolve `SRiverDesc::pPrecipiceMaterial` through the `0x10000` /
`0x20000` bank markers. Bank textures use the river texture resolution rather
than the crag material naming convention, which would otherwise select an
unstaged `Scene/TexAndMats` path.

GB3.1 verified all 7 serialized precipices including both river banks, 867
node columns, 8,658 triangles, 5 foot skirts with 1,094 triangles, and 5 ready
GPU layers over 3 textures, with the bank resolving to
`Terrain/Water/summer/crags.dds`. GER1.1 verified 35 of 36 precipices — 34
crags and one river bank — with 5,472 triangles, 34 foot skirts, and 11 ready
GPU layers; its remaining bank has no column above the minimum precipice
height. US1.2, which has no river, verified 36 crag precipices, 36 foot skirts,
and 7 ready GPU layers. `bStayedOnTerrain` bottom snapping is still pending.
The serialized `SPeak` path now recreates the
five-band `PeaksCreator.cpp` pendent profile and resolves `pPeakMaterial`; its
secondary TileMask blend is not yet reproduced. US1.0, GB3.1, and GER1.1
contain no serialized peaks, so the path is runtime-instrumented but empty on
those validation maps.

The renderer now consumes `SMapInfo::spots` rather than dropping the authored
terrain overlay layer. Each four-corner spot is converted from AI space,
clipped per terrain triangle, lifted by the desktop `0.1` bias, and submitted
with its `AM_OVERLAY` material. US1.0 rendered all 223 descriptors as 17,216
triangles across 36 textures, and every texture reached a valid GPU handle.
The sparse checkout and incremental debug installer include both
`Data/Spots` and
`Data/Scene/TexAndMats/All/Objects/TerraObjects`, restoring grass variation,
grazes, flowers, split ground, and crater decals on already staged devices.

## Implemented In This Slice

- Android Gradle project using GameActivity and one native `libblitzkrieg2.so`.
  The GameActivity static archive is whole-linked so its JNI bootstrap symbol is
  exported, and the shell uses a compatible fullscreen AppCompat theme.
- Native bootstrap loop with lifecycle and input polling hooks.
- Android path bridge for data, saves, and logs.
- Android-only `PlatformRuntime` for lifecycle state, monotonic time, sleep,
  logging, and non-blocking message reporting.
- First Android `NGfx` render-backend boundary. The Android target now fetches a
  pinned `bgfx.cmake`/bgfx stack, links `bk2_render_backend.*`,
  `bk2_bgfx_render_backend.cpp`, and `bk2_legacy_gfx_android.cpp`, and routes
  legacy `NGfx::Init3D`, `SetMode`, `Flip`, and shutdown through bgfx. Real
  devices request Vulkan first with bgfx fallback enabled; Android emulators use
  GLES3 directly because the `ranchu` Vulkan driver crashed inside debug-utils
  object naming during testing. Debug bgfx annotations are forced off in the
  Android build.
- First bgfx primitive adapter for the legacy 2D/UI rectangle path:
  `IRenderBackend::queue_solid_rect()` submits screen-space solid rectangles and
  `IRenderBackend::queue_textured_rect()` submits sampled texture rectangles
  with transient vertex/index buffers and bgfx embedded debugdraw fill shaders.
  The temporary bootstrap smoke overlay now reaches the textured path through
  the Android `C2DQuadsRenderer::AddRect()` adapter.
- First Android `C2DQuadsRenderer` adapter:
  `bk2_legacy_2d_quads_android.cpp` implements the legacy 2D rectangle API and
  a minimal `CRenderContext` without linking old D3D9 `GfxRender.cpp` or
  `GfxUtils.cpp`. Axis-aligned solid and textured rects are immediately mapped
  to the bgfx backend; render-targets, user shader effects, and old geometry
  batching are still stubs.
- First Android `GfxBuffers.h` texture runtime:
  `bk2_legacy_texture_android.cpp` defines the Android `NGfx::CTexture`,
  `I2DBuffer`, `CTextureLock`, texture cache/container, CPU linear-buffer, and
  cube-texture contracts without D3D9. Supported legacy lock formats
  (`SPixel8888`, `SPixel4444`, `SPixel565`, `SPixel1555`, and DXT block
  formats) are stored in CPU memory and uploaded to bgfx RGBA8 textures after
  write locks are released. DXT1/DXT3/DXT5 DDS data is decoded to RGBA8 during
  Android upload/readback; DXT2/DXT4 follow the same alpha paths as DXT3/DXT5.
  The persistent probe texture is submitted every frame through the
  `C2DQuadsRenderer` adapter and textured rectangle backend.
- First Android `GTexture.cpp` file texture gate:
  `BK2_ENABLE_LEGACY_TEXTURE_RUNTIME=ON` now also links the legacy
  `3Dmotor/GTexture.cpp` loader far enough for Android to create an
  `NDb::STexture`, write small A8R8G8B8, DXT1, DXT3, and DXT5 DDS files into
  `DataAndroid`, load them through Android VFS and `NGScene::CFileTexture`, and
  read them back from the Android `NGfx::CTexture` implementation. This proves
  the legacy DB texture descriptor, DDS loader, and compressed texture decode
  path before real UI/font/menu textures are wired in.
- The first renderer device gate passes on the ARM64 emulator: `:app:assembleDebug`
  succeeds, the APK installs, `GameActivity` logs
  `bgfx renderer initialized: OpenGL ES 3.0`, then
  `render_backend=ready`, `legacy_texture=probed; ... gpu_handle=yes;
  uploaded_levels=1; ... gtexture_dds=probed; gtexture_dds_write=true;
  gtexture_dxt1=probed; gtexture_dxt3=probed; gtexture_dxt5=probed`, with each
  DXT probe reporting `8x8`, `render_backend=ready; ... frames=2;
  primitives=4`, and a `2856x1280` screenshot shows the expected bgfx clear
  color, three submitted solid rectangles, and one sampled legacy texture
  rectangle presented through the Android window. A sampled pixel check over the
  texture rectangle reported 18 distinct colors, confirming texture sampling
  rather than a flat fallback.
- Gradle 9.4.1 wrapper and passing `:app:assembleDebug` bootstrap build.
- Portable legacy `zlib` sources now compile and link into the Android target.
- First Android compatibility shim for legacy `System/stdafx.h`; the Android
  build now compiles a `bk2_legacy_system_probe.cpp` translation unit that
  includes the old `System`, `Misc`, and database headers.
- First linked legacy core runtime subset behind
  `BK2_ENABLE_LEGACY_CORE_SOURCES=ON`: selected `Misc` and `System` `.cpp`
  files now compile and link into `libblitzkrieg2.so`.
- Android stream implementation for `CDataStream`/`CMemoryStream` plus
  file-backed `CFileStream`, avoiding Win32 file mapping while preserving core
  serialization behavior and app-private save writes. Android buffer growth now
  preserves the logical write cursor/file extent; this fixes the heap underrun
  found on-device while creating the initial profile config.
- Legacy `BinChunkSaver`, `VFS`, and `VFSOperations` are now linked into the
  Android target, backed by the Android stream/path shim.
- Legacy path utilities, `CombinerVFS`, lightweight XML parser/reader,
  `XMLChunkSaver`, binary resource naming helpers, and unicode text loading are
  now linked into the Android target.
- Android `NVFS::GetMainVFS()`/`GetMainFileCreator()` implementations are now
  installed during bootstrap, so legacy resource code can read from
  `DataAndroid`/app storage and write generated config/profile data into
  app-private storage. The Android VFS now normalizes legacy root-relative refs
  such as `/Scenario/...` against staged data roots, which is required for
  `GameRoot` campaign/chapter references. VFS-created profile writes now map
  `Profiles/...` to `<files>/Profiles/...` instead of nesting
  `Profiles/Profiles/...` under the save root.
- Legacy `GResource`, command registry/config handling, console buffer,
  log stream, singleton registry, and DG helpers are now linked into the Android
  target. The old Win32 resource-loader event/thread layer is backed by Android
  compatibility primitives.
- First runtime `libdb` slice is now linked into the Android target: `CVariant`,
  DB type definitions and constraints, struct meta-info reporting, bind arrays,
  bind processors, XML bind load/save helpers, `Database.cpp`, and the
  single-player `GameDatabase.cpp`. The Android build explicitly rejects editor
  database mode instead of linking `EditorDatabase.cpp`.
- Android bootstrap now opens the legacy `GameDatabase` in `DATABASE_MODE_GAME`
  through `NVFS::GetMainVFS()`/`GetMainFileCreator()` after VFS initialization
  and closes it before VFS shutdown. The startup log reports whether
  `types.xml` and `index.bin` are visible through `DataAndroid`.
- Android startup now also exposes a Java-callable runtime smoke probe:
  `NativeBridge.runStartupProbe()`. It is invoked by `Blitzkrieg2Activity`
  after app path configuration and logs direct `DataAndroid` layout checks,
  legacy VFS visibility for `types.xml`/`index.bin`, and the current
  single-player `GameDatabase` open state. The probe now also performs a
  `sp_content=probed` pass through `GameRoot` campaigns, chapters, mission map
  refs, tutorial maps, script refs, map data refs, and source/transcoded movie
  refs, giving Android a runtime gate for the single-player content graph before
  the full `ScenarioTracker` layer is ported.
- Android now also exposes `NativeBridge.runAudioBackendProbe()` through
  `bk2_android_audio_backend.*`, `bk2_android_audio_decode.*`, and
  `bk2_android_audio_probe.cpp`. The probe decodes a valid Microsoft ADPCM WAV
  block, mixes one-shot and looped PCM clips into interleaved stereo, and
  verifies pause/resume, volume/pan, looping, one-shot cleanup, and sample-rate
  conversion without depending on an Android audio device. It also runs the
  actual legacy `CreateSoundEngine`/`ISFX` facade through `CSoundSample`,
  `CSound2D`, and `CSound3D`, verifying play/stop, seek, channel cleanup, and 3D
  distance attenuation. The host `audio_decode_smoke` tool also decodes a
  shipped ADPCM voice file to the same PCM byte stream as FFmpeg.
- Oboe `1.10.0` is now linked through Prefab. `bk2_android_audio_output.*`
  opens a low-latency PCM16 stereo stream, feeds it from the Android mixer,
  pauses/resumes it with GameActivity focus/lifecycle events, and rebuilds it
  after disconnect errors. The mixer callback uses preallocated accumulation
  storage and defers finished-channel destruction to the game thread.
  `NativeBridge.runAudioDeviceProbe()` reports the selected API, state, sample
  rate, channels, callback count, and rendered frame count.
- Android now also exposes `NativeBridge.runSinglePlayerCatalogProbe()` through
  `bk2_android_sp_catalog.*`. It walks every campaign, chapter, mission path,
  and tutorial map reachable through the real loaded Android `GameRoot`, writes
  `single_player_catalog_probe.json` under the app log root, and logs
  `sp_catalog=probed` with campaign/chapter/mission-map coverage, map-data,
  script, source-movie, Android-movie, objective, and script-movie counters plus
  an issue count. This is stronger evidence than loose-file counting because it
  validates the Android runtime DB view that mission loading will actually use.
- First generated DB resource descriptor slice is now linked into the Android
  target without the full renderer/UI runtime: `3Dmotor/DBScene.cpp`,
  `UI/DBUserInterface.cpp`, and `Stats_B2_M1/UIEntries.cpp`. This registers
  resource metadata and cast helpers for `STexture`, `SWindowScreen`, and
  `STextEntry`, which are needed before `GameRoot`/campaign UI resources can
  load cleanly.
- The Android-verified generated DB descriptor slice now extends into
  single-player campaign and mission data:
  `GameX/DBScenario.cpp` is linked for campaign/chapter/mission descriptors;
  `Stats_B2_M1/DBMapInfo.cpp`, `RPGStats.cpp`, `DBPassProfile.cpp`,
  `DBPlaneManuvers.cpp`, `DBVisObj.cpp`, `DBConstructorProfile.cpp`,
  `DBAttachedModelVisObj.cpp`, `dbreinforcements.cpp`, `AckTypes.cpp`,
  `IconsSet.cpp`, `M1Actions.cpp`, `M1UnitActions.cpp`,
  `M1UnitSpecific.cpp`, `M1UnitType.cpp`, `Season.cpp`, `UnitTypes.cpp`,
  `UserActions.cpp`, `ActionsRemap.cpp`, `Commands_Actions.cpp`,
  `ConstructorInfo.cpp`, and `RPGStatsAddIn.cpp` are linked for RPG unit,
  reinforcement, acknowledgement, icon, action, season, passability, and
  constructor metadata used by shipped single-player content.
- The Android DB descriptor slice also now includes terrain and sound metadata:
  `B2_M1_Terrain/DBTerrain.cpp`, `DBTerrainSpot.cpp`, `DBVSO.cpp`,
  `DBPreLight.cpp`, `DBWater.cpp`, plus `Sound/DBMusicSystem.cpp`,
  `DBSound.cpp`, and `DBSoundDesc.cpp`.
- The Android DB descriptor slice now reaches the single-player root layer:
  `GameX/DBGameRoot.cpp`, `DBConsts.cpp`, `dbgameoptions.cpp`, and the const
  descriptors referenced by `SGameConsts` are linked and verified in the APK:
  `AILogic/DBAIConsts.cpp`, `Main/DBNetConsts.cpp`,
  `Stats_B2_M1/DBClientConsts.cpp`, `Stats_B2_M1/DBCameraConsts.cpp`,
  `SceneB2/DBSceneConsts.cpp`, `UI/DBUIConsts.cpp`, and `GameX/DBMPConsts.cpp`.
  The multiplayer const descriptor is retained only because the shared
  `SGameConsts` schema references it; multiplayer runtime and networking remain
  outside the Android target.
- `UISpecificB2/DBUISpecificB2.cpp` is now linked as a descriptor-only slice.
  This registers `SUIConstsB2` and related Blitzkrieg 2 UI metadata used by
  `GameRoot`/menu data without linking the old UI runtime implementation.
- Notification and options descriptors needed by game root/menu data are also
  linked: `Stats_B2_M1/DBNotifications.cpp` and `GameX/dbgameoptions.cpp`.
- The legacy Lua/Script runtime is now linked behind
  `BK2_ENABLE_LEGACY_SCRIPT_SOURCES=ON`: Lua VM/parser/state sources,
  `Script.cpp`, `ScriptWrapperInternal.cpp`, common script functions, script
  pointer helpers, and save/load registration. Script sources use scoped
  `-fexceptions` because the original protected Lua execution path uses
  `try`/`catch`/`throw`; other Android target sources still compile with the
  existing no-exceptions profile.
- The first Android-safe `GameX` runtime helper slice is now linked behind
  `BK2_ENABLE_LEGACY_GAMEX_RUNTIME_SOURCES=ON`: `CClientGameConsts.cpp` and
  `CustomMissions.cpp`. Android deliberately does not link
  `GameX/Initialization.cpp` yet; instead `bk2_android_gamex_consts.cpp`
  implements `NGameX::GetGameRoot()` and the client/scene/AI/net/shared-MP
  const accessors through `GameRoot.xdb`, including `SUIConstsB2` through the
  `UISpecificB2` descriptor slice. This lets the startup probe enumerate
  campaign/tutorial references and custom mission/campaign folders without
  registering renderer, sound, scene, UI, pathfinding, or MP runtime singletons.
- Android now provides `bk2_android_profiles.cpp`, a native implementation of
  the legacy `NProfile` API for single-player profiles and save directories.
  It keeps public paths in legacy `Profiles\\...` form for existing game code,
  but stores all generated `global.cfg`, `user.cfg`, `input.cfg`, and
  `Saves/` files under app-private `<files>/Profiles`. This replaces the old
  Win32 `Main/Profiles.cpp` dependency on `NMainLoop::GetBaseDir()`,
  `CoCreateGuid`, `CopyFile`, and legacy input binding commands for Android.
  The startup probe reports `profile_runtime=linked`, current profile name,
  profile dir, config visibility, and saves directory state.
- Android now also provides `bk2_android_save_inventory.*`, a small save-state
  scanner for the current profile `Saves/` directory. It intentionally avoids
  linking the full legacy `GameX/SaveLoadHelper.cpp` for now, because that file
  immediately pulls `InterfaceState`, screenshot textures, `ImageScale`,
  `ScenarioTracker`, UI widgets, MOD helpers, and replay/MP code. The Android
  scanner already gives the port a build-verified contract for `.sav` files,
  `.sfo` info files, paired save/info entries, orphan info files, newest save
  timestamps, and save directory writability via `save_inventory=probed`.
- Android now has the first mission-start state bridge in
  `bk2_android_mission_runtime.*`. It uses the real loaded `GameRoot` and DB
  descriptors to select a campaign/chapter/mission or tutorial map, then records
  an active mission snapshot: campaign/chapter/map DB ids, map designer file ref,
  map script ref, intro movie ref, player count, initial objective states,
  primary objective count, script movie/camera counts, and recommended
  reinforcement calls. `NativeBridge.startFirstCampaignMissionProbe()` logs this
  as `mission_state=active` when staged data is available. The bridge also now
  exposes tracker-like objective state changes and mission win/cancel flags, so
  future script callbacks have an Android state target before the full tracker
  is linked. It now mirrors more of `CScenarioTracker`'s campaign/chapter
  progression: completed/won mission IDs, enabled mission counts from
  `nMissionsToEnable`, chapter completion after mission win, mission and chapter
  reinforcement-call pools, enemy reinforcement calls, and calls-used tracking.
  The bridge now also mirrors the first statistics/reward slice from the legacy
  tracker: objective completion adds/removes pending XP, mission win commits
  campaign XP, rank thresholds, available promotions, campaign/mission
  statistics, reinforcement-call statistics, chapter reward reinforcements,
  disabled enemy reinforcement types, bonus calls, and chapter/kills/tactics/
  economy medal awards. Reward reinforcements are represented as Android
  mission state and enabled-mission inputs for now. Chapter reinforcements now
  have Android inventory slots that mirror the legacy
  disabled/not-enabled/enabled state model, DB ids, localized name/description
  refs, previous-chapter flags, and an old-inventory snapshot before mission
  rewards are applied. The bridge now also tracks reinforcement progress slots,
  favorite reinforcement counts, free/assigned leader pools, leader rank/XP/debt,
  leader kill/loss counters, and leader rank thresholds from
  `AIConsts.common.expLevels`. Android-side unit kill events now update the
  kill/price-kill matrices, local mission kill/loss statistics, pending player
  XP, leader XP, leader loss debt, score, and campaign win statistics. The real
  simulation/`MapObj` kill feed is not wired into the bridge yet; non-leader
  reinforcement XP-level mutations, the munchkin medal check, and the
  result-screen rank/medal popup graph are still not linked. The shipped
  `SingleStatistics2` Action Report screen is linked and populated from a real
  tracker snapshot, including the original reinforcement reward/upgrade rows.
  `NativeBridge.runMissionProgressionProbe()` now performs a temporary real
  campaign mission progression smoke test with objective/reward preference,
  applies objective XP, statistics, a reinforcement call, leader assignment,
  Android-side unit kill/loss events, leader XP/debt, favorite reinforcement
  tracking, and mission win, then restores the previous Android mission state
  after logging `mission_progression=probed`. The bridge can also continue the
  current campaign/chapter state into another enabled mission through
  `StartCurrentCampaignMissionState()` or `StartFirstEnabledCampaignMissionState()`
  without resetting player XP, won mission IDs, leader state, reinforcement
  inventory, campaign statistics, or chapter call pools; the progression probe
  exercises this path when the post-win chapter still has enabled missions.
  Android now also has a versioned mission-runtime checkpoint path for this
  bridge state. `SerializeMissionRuntimeState()` / `RestoreMissionRuntimeState()`
  round-trip the value state, while `SaveMissionRuntimeCheckpoint()` /
  `LoadMissionRuntimeCheckpoint()` persist it under the current profile
  `Saves/` directory. `NativeBridge.runMissionCheckpointProbe()` verifies a
  post-win/continued campaign state by saving it, resetting the runtime, loading
  it back, and logging `mission_checkpoint=probed` plus
  `checkpoint_roundtrip=true`. This checkpoint is not the final legacy `.sav`
  object graph; it is the Android lifecycle/progression bridge until the old
  `SaveLoadHelper`/scenario tracker graph is portable.
  This is not a replacement for the full legacy `GameX/ScenarioTracker.cpp`; it
  is the Android bridge point before porting the remaining tracker dependencies
  on `InterfaceState`, UI colors, `MapObj`, AI logic, text, reinforcement, and
  statistics code.
- Android compatibility guards were added to the new generated descriptor
  headers/stdafx files where Clang rejects old MSVC-only constructs such as
  opaque enum forward declarations and `__int64`/Windows includes. These guards
  are scoped to `BK2_ANDROID` and preserve the existing Windows source layout.
- Android compatibility guards were added to `3Dmotor/stdafx.h`,
  `Stats_B2_M1/stdafx.h`, and `UI/stdafx.h`; `UI/Specific.h` is skipped for the
  DB-descriptor-only Android slice so it does not pull `Sound`, `Input`, and
  full UI runtime prematurely.
- Android random seed/state bridge for `NRandom`, including save/load state
  capture through `IRandomSeed`.
- Android fullscreen video bridge for transcoded Bink playback. The bridge now
  resolves direct `.bik` refs, extension-less movie sequence `<FileName>`
  entries, and `.xml` movie sequences into Android MP4 refs. Runtime lookup uses
  the Android VFS and canonicalizes case mismatches such as
  `Movies/chronicles/...` versus `Movies/Chronicles/...`, which matters on
  Android filesystems. `VideoPlayerActivity` can now play an ordered path array,
  so sequence XML can become multiple fullscreen clips instead of one collapsed
  movie.
- Android audio backend with channel state, pause/resume, listener updates,
  looped/one-shot PCM playback, seek, volume/pan, 3D distance attenuation,
  simple sample-rate conversion, and thread-safe in-memory stereo mixing. The
  portable WAV decoder handles PCM, 32-bit float, and Microsoft ADPCM, including
  the shipped voice/SFX ADPCM format. The Android target now links the
  FMOD-free legacy sample path (`CSoundSample`, `CSound2D`, `CSound3D`,
  `CSoundManager`, and `CSoundEngine`) behind
  `BK2_ENABLE_LEGACY_SOUND_RUNTIME=ON`. Oboe now supplies AAudio output on new
  devices and OpenSL ES fallback on old supported devices.
- Android OGG/Vorbis streaming through NDK `AMediaExtractor`/`AMediaCodec`, a
  worker decoder, and a lock-free PCM ring. The original `MusicSystem`,
  `Track`, `PlayList`, `PlayTime`, `Fade`, and `Pause` sources now compile under
  `BK2_ENABLE_LEGACY_MUSIC_RUNTIME=ON` and drive Android mixer channels without
  FMOD. On the ARM64 emulator, the legacy facade read a 79,313 ms shipped track,
  played it, held the PCM cursor during per-channel pause, resumed it, and
  stopped it through `Clear()`.
- Runtime module inventory with blocker detection.
- Single-player content validator for data/bin, missions, movies, and sparse checkout gaps.
- Bink transcode manifest generator for Android-native video files.
- Transcoded-video validator using `ffprobe`; it now reports ready referenced
  videos separately from blocked referenced videos.
- `DataAndroid` staging helper for symlink/copy/hardlink based asset layout.
  Its manifest now probes the DB startup files and the main prebuilt geometry,
  skeleton, animation, and AI geometry directories expected by the Android VFS.
  It also treats `Data/Scenario`, `Data/Consts`, `Data/Reinforcements`, and
  `Data/Other/Text` as runtime gates because the Android DB can otherwise see
  only indexed headers while campaign bodies, game const bodies, scripted
  reinforcement formations, or map designer text refs remain missing.

## Deliberately Not Linked Yet

The full legacy engine runtime is not linked into the Android target yet. The
current build links the first `System`/`Misc` core subset, the VFS/bin/XML saver
pieces, path/resource/text/config/console helpers, a runtime `libdb` subset
including `GameDatabase`, portable zlib, Android VFS, Android stream/random/
platform replacements, and a bgfx-backed clear/present plus solid-rectangle
renderer bootstrap plus the first Android `CTexture`/`I2DBuffer` lock/upload
runtime, plus a build-verified `GTexture.cpp` DDS/DXT file texture path.
Directly
linking the rest of the runtime modules would still fail because many modules
include or expose:

- `windows.h`, `HWND`, `HANDLE`, `DWORD`, Win32 events and critical sections.
- Direct3D 9 and D3DX types in unported `3Dmotor` draw/resource paths.
- DirectInput in `Input`.
- Residual FMOD FSOUND and DirectSound assumptions in unlinked sound tooling
  and scene implementation files; the linked sample and music paths no longer
  use them.
- Granny runtime API in mesh, skeleton, and animation loaders.
- MSVC-only language extensions and pragmas.

The first render boundary is no longer the critical path: terrain materials,
converted static/skinned meshes, action animations, descriptor particles and
lights, fog of war, original HUD textures, and dynamic water are all proven on
ARM64. Transition clips are no longer part of the remaining renderer list: the
presentation bridge now consumes
the original `ACTION_NOTIFY_ANIMATION_CHANGED` stream, forwards its animation
type and simulation time, and selects non-looping standing-to-prone and
prone-to-standing caches sampled from the shipped RIFLE resources `3965` and
`3988`. A complete conversion covered all 259 compatible infantry
geometries for each direction, and an ARM64 US1.0 run received the live
animation stream without a geometry fallback. Version-5 animated meshes with
at most 48 bones now use a dedicated bgfx vertex-skinning path: immutable
bind-pose geometry is cached once, while each visible instance submits its
world transform and selected matrix palette. Checked-in ESSL and SPIR-V shaders
cover the Android OpenGL ES and Vulkan backends. Older cache versions, larger
skeletons, procedural vehicle subparts, and projected silhouettes keep the
validated CPU fallback. An installed ARM64 US1.0 run reported
`gpu_skinning=active` for idle geometry 1791 (217 vertices, 21 bones, 16
frames), then entered attack, lying-attack, and death variants with the models
and original HUD still visible and no shader/GL/native-process error. Remaining
renderer work includes effect-attached Granny geometry, more complete legacy
shader translation, and post effects. The
original
`SingleStatistics2` Action Report is now live: it renders the shipped layout,
participating-player rows and flags, mission/campaign time, rank progress, and
outcome-specific navigation from the real mission statistics snapshot. It also
reads `SMissionStats::bonusReinforcements` and `oldReinfs` to render up to four
localized new-branch/upgrade rows with the shipped reinforcement icons. ARM64
validation covered victory, Surrender defeat, same-mission Restart, Next to the
shipped chapter map, and direct selection of `US1.2` from its generated map
marker. That mission produced the expected Main Infantry upgrade and Bombers
branch rows. The chapter targets now preserve their mission index and run the
original availability/selection flow instead of all launching path index 0.
The Android controller consumes enabled/completed/won mission IDs from
`MissionRuntimeState` after an Action Report, derives a new campaign's initial
state from `SMissionEnableInfo`, renders the shipped seven-state regular and
final markers, binds the mission/reinforcement/reward panel, and prevents a
locked target from emitting Play. ARM64 checks covered the five-target USA and
Germany chapter-one maps, locked-final selection, and an exact USA mission
index 2 briefing route. Selected-mission road arrows now follow the shipped
chapter `roads`, full-route UVs, arrow types, and dependency alpha; ARM64 checks
covered USA mission indices 1 and 2 plus Germany mission index 1. UI work still
includes the chapter-map reinforcement digit rollers and detail dialogs,
rank/medal result popups, and replacement of temporary immediate-mode stubs
with proper batching and render-target behavior. The static
`CWindowPotentialLines` result is now generated from the chapter TGA layers,
main-strike gradient, mission nodes, current completion state, potential
values, and the original marching-squares contour. ARM64 validation covered
the distinct USA gold and Germany blue territory states. The successful
statistics route now also restores `nFrontLineAnim`: only the last completed
mission is initialized at `fPotentialIncomplete`, then its contribution is
interpolated to `fPotentialComplete` over the original 5,000 milliseconds while
the generated texture is updated in place.

Mission startup also treats an empty initial dynamic-world mesh as valid:
static scenery is uploaded separately and later presentation generations fill
the dynamic buffer. If AI retires an entity between the snapshot count and
copy calls, Android keeps the complete shortened copy instead of aborting the
mission. Together these changes removed the intermittent
`dynamic_world_snapshot_failed` seen when launching `US1.2` from the chapter
map.

## Current Content Evidence

The sparse checkout now includes the runtime single-player roots needed for the
Android DB view, not only loose file counting:

- `Versions/Current/Data` stages 75,695 files into `DataAndroid/Data` in the
  current verified sparse checkout.
- `Data/bin/Geometries`, `Skeletons`, `Animations`, and `AIGeometries` are
  present with 2,817, 2,585, 7,557, and 2,783 files respectively.
- `Data/Scenario` is present with 3,496 files, `Data/Consts` with 162 files,
  and `Data/Other/Text` with 11,254 files.
- `Data/Reinforcements` is present with 1,970 files. GER3.3 then creates the
  original local-player formations, opens 873 legacy war-fog cells on the first
  completed snapshot, and renders the battlefield at the shipped player camera
  anchor instead of remaining black.
- `prepare_data_android.py --output build/android/DataAndroid --mode symlink`
  reports no layout blockers for those runtime gates.
- On the ARM64 emulator, the Android startup probe opens the real legacy
  `GameDatabase` and reports `legacy_database=open`.
- The same probe resolves `game_root=present`, `game_consts=present`,
  `client_consts=present`, `scene_consts=present`, `ai_consts=present`,
  `net_consts=present`, `shared_mp_consts=present`, and `ui_consts=present`.
- The single-player runtime catalog reports `sp_catalog_campaigns=3/3`,
  `sp_catalog_chapters=12/12`, `sp_catalog_missions=69`,
  `sp_catalog_mission_maps=69/69`, `sp_catalog_tutorial_maps=4/4`,
  `sp_catalog_unique_maps=73`, `sp_catalog_map_data_refs=34/34`,
  `sp_catalog_script_refs=74/74`, `sp_catalog_objectives=253`,
  and `sp_catalog_script_movies=13`. Non-video content issues are currently
  clear; the remaining `sp_catalog_issues=21` are `missing_android_movie`
  entries for campaign/chapter cinematics whose source Bink files are missing or
  Git LFS pointer files in this checkout.
- `mission_state=active` starts the first USA campaign mission from the real
  DB graph, including map/script/movie refs, four players, objectives,
  reinforcement-call pools, rank/XP counters, and chapter mission counts.
- `mission_progression=probed` advances a real staged campaign mission through
  objective XP, rewards, leader assignment, kill/stat counters, mission win,
  and a follow-up enabled mission.
- `mission_checkpoint=probed` writes and reloads the Android mission-runtime
  checkpoint with `checkpoint_roundtrip=true`.
- Audio, music, render, texture, DDS, and DXT gates also pass in the same
  installed debug APK: AAudio output starts, OGG music streams, GLES3 bgfx
  presents, and `gtexture_dds`, `gtexture_dxt1`, `gtexture_dxt3`, and
  `gtexture_dxt5` all load through the legacy `GTexture.cpp` path.

The remaining content blocker is video packaging, not mission catalog
visibility. The current checkout has 30 deduplicated Android video jobs. The
transcode validator reports 8 ready MP4 outputs, 4 ready referenced Bink keys,
22 blocked referenced Bink keys, 18 Git LFS pointer sources, and 4 missing
referenced sources. Those MP4 files were pushed to the ARM64 emulator; startup
and catalog probes now report `sp_movie_android_refs=1/19` and
`sp_catalog_movie_android_refs=1/19`, respectively. The ready Android movie ref
is the root intro sequence (`cdv`, `Nival`, `Intro`). The campaign/chapter
cinematics remain blocked until every referenced `.bik` has a lawful source blob
and a validated Android-native transcode.

## Single-Player Scope Rules

Keep:

- All shipped campaigns, missions, maps, briefings, scripts, localized text.
- All single-player sounds, music, UI assets, and videos after conversion.
- Save/load and profile/config behavior.

Exclude:

- Multiplayer UX and network protocol.
- Dedicated server and server/client utility targets.
- Map editor, MFC editor tools, code generators, and dev-only test apps.

Network symbols needed by menus or shared code should be replaced by no-op
single-player facades rather than by carrying the full networking stack.

## Video Policy

All `.bik` files must be represented in `DataAndroid`. The preferred path is
offline conversion to H.264/AAC `.mp4` for full-screen movies. Tiny UI movies can
be converted either to looped video textures or sprite sheets, depending on how
the original UI references them.
