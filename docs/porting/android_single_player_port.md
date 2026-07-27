# Android Single-Player Port Status

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
  reinforcement XP-level mutations, the munchkin medal check, and UI statistics
  screen are still not linked.
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
  It also treats `Data/Scenario`, `Data/Consts`, and `Data/Other/Text` as
  runtime gates because the Android DB can otherwise see only indexed headers
  while campaign bodies, game const bodies, or map designer text refs remain
  missing.

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

The next critical path is no longer the first render boundary; clear/present,
solid primitive submit, minimal Android texture lock/upload, textured-quad
submission, the first `C2DQuadsRenderer::AddRect()` adapter, and a
file-backed `GTexture.cpp` DDS/DXT load gate are proven. The next renderer
layer is legacy UI integration: connect real DB-referenced UI textures,
font/text, and menu/briefing layouts to this adapter, then replace the temporary
immediate-mode stubs with proper batching/render-target behavior.
After that, the larger render work remains terrain,
static/skinned meshes, particles, water, shadows, shader translation, and post
effects. In parallel, the bootstrap loop still needs to be split into the real
legacy main/simulation frame, and the Android mission-state bridge still needs
replacement by the portable parts of `CScenarioTracker`.

## Current Content Evidence

The sparse checkout now includes the runtime single-player roots needed for the
Android DB view, not only loose file counting:

- `Versions/Current/Data` stages 39,193 files into `DataAndroid/Data`.
- `Data/bin/Geometries`, `Skeletons`, `Animations`, and `AIGeometries` are
  present with 2,817, 2,585, 7,557, and 2,783 files respectively.
- `Data/Scenario` is present with 3,496 files, `Data/Consts` with 162 files,
  and `Data/Other/Text` with 11,254 files.
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
