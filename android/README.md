# Blitzkrieg 2 Android Port Bootstrap

This directory is the native Android shell for the single-player source port.
It is intentionally small: the current target builds the GameActivity entrypoint
and Android path/lifecycle bridge, then legacy runtime modules are enabled one by
one after their Win32/D3D9/FMOD/Granny blockers are removed.

## Build Shape

- `:app` builds one native library: `libblitzkrieg2.so`.
- `arm64-v8a` is the only enabled ABI.
- `minSdk` is 24; `compileSdk`/`targetSdk` are 36.
- `GameActivity` is integrated through AndroidX AAR + Prefab. Its static archive
  is linked whole so the Java JNI entry point is retained in
  `libblitzkrieg2.so`; the Activity uses an AppCompat fullscreen theme.
- Oboe `1.10.0` is integrated through Prefab with the shared C++ runtime. It
  selects AAudio where available and OpenSL ES on older supported devices.
- `BK2_ENABLE_BGFX_RENDERER=ON` fetches the pinned `bgfx.cmake` integration and
  links the first Android `NGfx` backend boundary. Real devices prefer Vulkan
  with bgfx fallback enabled; Android emulators use the GLES3 backend because the
  `ranchu` Vulkan driver crashes inside debug-utils object naming. The backend
  now renders the real mission heightfield and terrain materials plus converted
  original Granny meshes for mapped static objects and live AI units. Material
  textures, skinning/animation, and the full legacy UI are still pending.
- `BK2_ENABLE_LEGACY_TEXTURE_RUNTIME=ON` links the Android
  `NGfx::CTexture`/`I2DBuffer` contract. Legacy callers can allocate textures,
  lock mip levels with `CTextureLock`, write the original pixel formats into CPU
  storage, read back `SPixel8888`, resolve container metadata, and upload
  supported formats to bgfx RGBA8 textures after unlock. This option also links
  the portable `3Dmotor/GTexture.cpp` loader path far enough for Android to load
  a file-backed DDS through `NDb::STexture`, Android VFS, `CFileTexture`, and
  the Android texture runtime. DXT1/DXT3/DXT5 block textures are decoded to
  RGBA8 during Android upload/readback; DXT2/DXT4 follow the same alpha block
  paths as their DXT3/DXT5 counterparts.
- The portable legacy `zlib` C sources are compiled into the Android target.
- `BK2_ENABLE_LEGACY_CORE_SOURCES=ON` links the first Android-ported legacy
  `Misc`/`System` runtime subset: math/string helpers, object/refcounting,
  chunk/bin/XML serialization helpers, VFS registration/combining, path
  utilities, binary resource naming, unicode text loading, command registry,
  config load/save, console/log buffers, async resource file requests, logging,
  command-line parsing, time helpers, and related core support. It also links
  the first runtime `libdb` slice: variants, DB type definitions, meta-info
  reporting, bind/manipulator helpers, XML bind save/load helpers, and the
  single-player `GameDatabase` facade. `EditorDatabase.cpp` remains excluded.
- `BK2_ENABLE_LEGACY_DB_TYPE_SOURCES=ON` links the first Android-verified
  generated DB resource descriptors without bringing the full renderer,
  simulation loop, multiplayer, server utilities, or editor runtime. It now
  covers base render/UI descriptors, `GameX/DBScenario.cpp` campaign/chapter
  descriptors, `GameX/DBGameRoot.cpp`, `GameX/DBConsts.cpp`,
  `GameX/dbgameoptions.cpp`, and the const/resource descriptors referenced by
  the single-player game root: AI/client/net/scene/UI/multiplayer const
  metadata, camera limits, notifications, map and difficulty descriptors,
  terrain/prelight/water/VSO descriptors, sound/music descriptors, and the
  `Stats_B2_M1` RPG/M1 unit, reinforcement, acknowledgement, icon,
  action-remap, and constructor-helper descriptors needed by shipped
  single-player mission data. `GameX/DBMPConsts.cpp` is included only as shared
  `GameConsts` metadata; multiplayer runtime and networking remain excluded.
  `UISpecificB2/DBUISpecificB2.cpp` is now included as a generated descriptor
  slice so `SUIConstsB2` and related Blitzkrieg 2 UI metadata can load from
  `GameRoot` without linking the old UI runtime implementation.
  The Android startup probe logs representative registered type IDs for
  `TextEntry`, `Texture`, `WindowScreen`, `GameRoot`, `GameConsts`,
  `AIGameConsts`, `NetGameConsts`, `ClientGameConsts`, `SceneConsts`,
  `MultiplayerConsts`, `TooltipContext`, `OptionSystem`, `Campaign`, `Chapter`,
  `MapInfo`, `DifficultyLevel`, `CameraLimits`, `Notification`,
  `NotificationEvent`, `MechUnitRPGStats`, `SquadRPGStats`, `PlayerRank`,
  `ReinforcementTypes`, `TerraSet`, `MapMusic`, `ComplexSoundDesc`, and
  `UIConstsB2`.
- `BK2_ENABLE_LEGACY_SCRIPT_SOURCES=ON` links the legacy Lua/Script runtime:
  Lua VM/parser/state sources, `Script.cpp`, `ScriptWrapperInternal.cpp`,
  common function registration, script pointer helpers, and save/load
  registration. Script sources keep C++ exceptions enabled per-source because
  the original Lua protection path uses `try`/`catch`/`throw`; the rest of the
  Android target remains on the existing no-exceptions profile. The startup
  probe reports `legacy_script=linked`.
- `BK2_ENABLE_LEGACY_GAMEX_RUNTIME_SOURCES=ON` links the first Android-safe
  non-rendering `GameX` runtime helpers: `CClientGameConsts.cpp` and
  `CustomMissions.cpp`. The old `GameX/Initialization.cpp` remains excluded
  because it registers renderer, scene, sound, UI, camera, pathfinding, and MP
  runtime singletons. Android provides `bk2_android_gamex_consts.cpp` instead,
  so `NGameX::GetGameRoot()` and the client/scene/AI/net/shared-MP const
  accessors can resolve through the staged single-player `GameRoot.xdb` without
  pulling the full runtime. `NGameX::GetUIConsts()` now resolves
  `SUIConstsB2` through the generated `UISpecificB2` descriptor slice.
- `BK2_ENABLE_LEGACY_SOUND_RUNTIME=ON` links the FMOD-free Android
  `CSoundSample`, `CSound2D`, `CSound3D`, `CSoundManager`, and `CSoundEngine`
  path. The public `ISFX` contract now maps sample loading, channel ownership,
  play/stop, seek, pause, volume, pan, listener updates, and 3D distance
  attenuation onto `AndroidAudioBackend`.
- `BK2_ENABLE_LEGACY_MUSIC_RUNTIME=ON` links the original
  `MusicSystem`, `Track`, `PlayList`, `PlayTime`, `Fade`, and `Pause` state
  machines without FMOD. OGG/Vorbis tracks are decoded on a worker thread
  through NDK `AMediaExtractor`/`AMediaCodec`, buffered in a lock-free PCM ring,
  and mixed through the same Android channels as sample audio. The existing
  playlist switching, duration, fade, volume, pause, stop, and save/load
  interfaces are preserved.
- `BK2_ANDROID_PROFILE_RUNTIME=1` enables `bk2_android_profiles.cpp`, an
  Android-native implementation of the legacy `NProfile` API. Profiles,
  generated `global.cfg`/`user.cfg`/`input.cfg`, and `Saves/` are created under
  app-private `<files>/Profiles`; the old Win32 `Profiles.cpp` remains excluded
  because it depends on `CoCreateGuid`, `CopyFile`, `NMainLoop::GetBaseDir()`,
  and the legacy input binding runtime.
- `bk2_android_save_inventory.*` scans the current Android profile `Saves/`
  directory without linking the old `GameX/SaveLoadHelper.cpp` UI/runtime
  graph. The startup probe reports `.sav` files, `.sfo` info files, paired
  entries, orphan info files, newest save timestamp, and whether the directory
  is writable.
- `bk2_android_mission_runtime.*` is the first Android-safe mission-start
  state layer. It opens the real `GameRoot`, selects a campaign/chapter/mission
  or tutorial map, and records the active mission DB id, map data ref, script
  ref, intro movie ref, objective states, player count, script movie count, and
  recommended reinforcement calls without linking `GameX/ScenarioTracker.cpp`
  yet. It also exposes the first tracker-like state operations for objective
  state changes plus mission win/cancel flags. The bridge now tracks
  campaign/chapter activity, completed/won mission IDs, enabled mission counts,
  `nMissionsToEnable`, chapter and mission reinforcement-call pools, enemy
  reinforcement-call pools, mission statistics, objective XP, campaign XP,
  rank thresholds, available promotions, mission reward reinforcements, bonus
  calls, chapter/kills/tactics/economy medal awards, and chapter completion
  after mission win using the same basic rules as the legacy `CScenarioTracker`.
  Chapter reinforcements are now represented as Android inventory slots matching
  the legacy disabled/not-enabled/enabled state model, including DB ids,
  localized name/description refs, previous-chapter flags, and an old inventory
  snapshot before mission rewards are applied. Reward reinforcement changes are
  represented through that inventory and as enabled-mission inputs. The bridge
  now also tracks reinforcement progress slots, favorite reinforcement counts,
  free/assigned leader pools, leader rank/XP/debt, leader kill/loss counters,
  and rank thresholds from `AIConsts.common.expLevels`. The startup smoke flow
  now also logs
  `NativeBridge.runMissionProgressionProbe()`, which temporarily starts a real
  campaign mission with objective/reward preference, applies objective XP,
  statistics, a reinforcement call, leader assignment, Android-side unit kill
  events, leader XP/debt, favorite reinforcement tracking, and mission win, then
  restores the previous state. The real simulation/`MapObj` kill feed is not
  wired into this bridge yet; reinforcement XP-level mutations for non-leader
  units, the munchkin medal check, UI statistics screen, and legacy tracker
  object graph are also still not linked.
  The bridge can now continue the current campaign/chapter state into another
  enabled mission with `StartCurrentCampaignMissionState()` or
  `StartFirstEnabledCampaignMissionState()` without resetting player XP, won
  mission IDs, leader state, reinforcement inventory, campaign statistics, or
  chapter call pools.
  Android also has a versioned mission-runtime checkpoint path:
  `SerializeMissionRuntimeState()`, `RestoreMissionRuntimeState()`,
  `SaveMissionRuntimeCheckpoint()`, and `LoadMissionRuntimeCheckpoint()` persist
  this Android-side campaign state under the current profile `Saves/` directory.
  This is a lifecycle/progression checkpoint for the port layer, not the final
  legacy `.sav` object graph.
  The full legacy tracker still depends on `InterfaceState`, UI colors,
  `MapObj`, AI, text, and the rest of reinforcement/stat code; this layer gives
  Android a verified bridge point before those dependencies are ported.
- `bk2_android_vfs.*` installs the Android `NVFS::GetMainVFS()` and
  `NVFS::GetMainFileCreator()` implementations at startup. Reads search
  `DataAndroid`, `DataAndroid/Data`, app files, and profiles; writes go through
  app-private storage. Legacy root-relative refs such as `/Scenario/...` are
  normalized against the Android data roots instead of being treated as host
  absolute filesystem paths. VFS-created writes for `Profiles/...` are also
  routed to `<files>/Profiles/...` rather than duplicating the `Profiles`
  segment under the save root.
- `bk2_android_database.*` opens the legacy `GameDatabase` in game mode after
  Android VFS setup and closes it before VFS shutdown. It logs whether
  `types.xml` and `index.bin` are visible through the staged `DataAndroid`
  roots; editor database mode remains excluded from Android.
- `bk2_android_startup_probe.*` exposes `NativeBridge.runStartupProbe()` for
  Android runtime smoke checks. `Blitzkrieg2Activity` calls it after configuring
  app-specific storage paths; logcat then shows direct `DataAndroid` layout
  checks, legacy VFS visibility for `types.xml`/`index.bin`, whether the
  single-player `GameDatabase` is open, and a `sp_content=probed` pass that
  walks `GameRoot` campaigns, chapters, mission map refs, tutorial maps,
  script refs, map data refs, and source/transcoded movie refs.
- `bk2_android_sp_catalog.*` exposes
  `NativeBridge.runSinglePlayerCatalogProbe()`. It walks every campaign,
  chapter, mission path, and tutorial map reachable through the real Android
  `GameRoot`, writes `single_player_catalog_probe.json` under the app log root,
  and logs `sp_catalog=probed` with loaded campaign/chapter/mission-map counts,
  map data/script/movie coverage, objective counts, script movie counts, and
  issue count. This is the runtime evidence gate for the requirement that all
  single-player missions are visible to Android.
- `bk2_legacy_streams_android.cpp` provides the current Android stream runtime:
  memory streams plus file-backed `CFileStream` read/write without Win32 file
  mapping. Relative read paths are resolved against app storage and
  `DataAndroid`; relative writes go to app-private profile storage. Buffer growth
  preserves the requested write cursor/file extent, preventing the heap
  underrun previously hit while creating `Profiles/global.cfg`.
- `bk2_legacy_random_android.cpp` provides the Android `NRandom` seed/state
  bridge needed by core serialization and the `NWin32Random` facade used by
  weighted music playlists and randomized pauses/play times.
- `bk2_android_platform.*` is the first Android-side replacement layer for
  `GetTickCount`, `Sleep`, lifecycle state, logging, and non-blocking message
  reporting.
- `bk2_legacy_win_compat.h` now includes Android replacements for the Win32
  event/thread wait functions used by the legacy resource loader.
- `bk2_android_audio_backend.*` is the first Android-side replacement layer for
  the legacy FMOD FSOUND backend. It now has a thread-safe in-memory stereo PCM
  mixer for channel state, looped/one-shot playback, pause/resume, seek,
  volume/pan, simple sample-rate conversion, listener state, and 3D distance
  attenuation. Finished-channel destruction is deferred to the game thread and
  the callback uses a preallocated mix buffer.
- `bk2_android_audio_output.*` connects that mixer to a low-latency Oboe output
  stream. It starts, pauses, resumes, and closes with GameActivity lifecycle
  events, and schedules a stream rebuild after device disconnect errors.
- `bk2_android_audio_decode.*` decodes RIFF/WAVE PCM, 32-bit float, and
  Microsoft ADPCM into the mixer PCM format. The host smoke tool verifies a
  shipped ADPCM voice file byte-for-byte against FFmpeg output.
- `bk2_android_vorbis_stream.*` resolves legacy root-relative music refs,
  reads track metadata, seeks by timestamp, decodes OGG/Vorbis to PCM16, and
  streams it through `bk2_android_pcm_stream.*`. The device probe verifies both
  direct streaming and the original DB-driven music facade.
- `bk2_render_backend.*`, `bk2_bgfx_render_backend.cpp`, and
  `bk2_legacy_gfx_android.cpp` provide the first Android renderer bootstrap.
  Legacy `NGfx::Init3D`, `SetMode`, `Flip`, and lifecycle shutdown now route to
  bgfx. Debug bgfx annotations are forced off in debug APKs to avoid the
  emulator Vulkan `vk_common_SetDebugUtilsObjectNameEXT` crash observed during
  smoke testing. The backend also exposes `queue_solid_rect()` and
  `queue_textured_rect()`, backed by bgfx transient vertex/index buffers and
  embedded debugdraw fill shaders, as the first primitive adapters for the
  legacy 2D/UI rectangle path.
- `bk2_legacy_texture_android.cpp` provides the first D3D-free
  `GfxBuffers.h` texture runtime for Android: `MakeTexture`, `MakeCubeTexture`,
  `GetTextureContainer`, texture/cache stubs, CPU linear buffers, render-target
  readback helpers, and bgfx RGBA8 upload for `SPixel8888`, `SPixel4444`,
  `SPixel565`, `SPixel1555`, and DXT block locks. Its smoke texture is
  submitted through the Android `C2DQuadsRenderer::AddRect()` adapter each
  frame. Its startup probe also writes A8R8G8B8, DXT1, DXT3, and DXT5 DDS files
  into `DataAndroid` and reloads them through the legacy
  `GTexture.cpp`/`CFileTexture` path, proving the legacy-facing 2D texture path
  before the full old UI runtime is linked.
- `bk2_legacy_2d_quads_android.cpp` implements the first D3D-free
  `C2DQuadsRenderer` and minimal `CRenderContext` path. Axis-aligned solid and
  textured rectangles are mapped directly to the bgfx primitive queues; old
  render-targets, user shader effects, and geometry batching are still staged
  behind stubs.
- `bk2_android_video_bridge.*` and `VideoPlayerActivity` are the Android-native
  fullscreen playback path for transcoded Bink movies. The native bridge now
  exposes `AndroidVideoRefForLegacyBink()`, `AndroidVideoPathForLegacyBink()`,
  `AndroidVideoRefsForLegacyMovie()`,
  `AndroidVideoPathsForLegacyMovie()`, and request helpers for both single
  videos and movie sequences. Ported legacy callers can pass refs like
  `Movies\Nival.bik` and get the canonical `DataAndroid/Movies/Nival.mp4` file
  path, or pass a movie sequence XML such as `Movies\intro.xml` and have its
  `<FileName>` entries resolved to Android MP4 paths. The Java
  `VideoPlayerActivity` can now play an ordered path array, so XML sequences
  are not collapsed to a single clip.
- `BK2_ENABLE_LEGACY_SOURCES=OFF` by default. Turning it on currently fails
  fast until the blocker inventory is addressed.
- `bk2_legacy_system_probe.cpp` intentionally includes the legacy
  `System/stdafx.h` path in the Android build. This proves the first Win32/MSVC
  compatibility shim layer compiles. It now sits alongside the first linked
  legacy core source subset, Android game DB open path, generated DB descriptor
  slice, Lua script runtime, and bgfx clear/present bootstrap. The full legacy
  3D scene/UI/gameplay simulation runtime is still not linked.

## Useful Commands

From the repository root:

```sh
tools/android/install_playable_debug.sh

python3 tools/android/module_inventory.py \
  --json build/android/module_inventory.json \
  --cmake build/android/generated_legacy_sources.cmake

python3 tools/android/validate_single_player_content.py \
  --json build/android/content_report.json

python3 tools/android/transcode_bink_manifest.py \
  --manifest build/android/bink_transcode_manifest.json

python3 tools/android/validate_transcoded_videos.py \
  --manifest build/android/bink_transcode_manifest.json \
  --json build/android/video_validation_report.json

git sparse-checkout add \
  Versions/Current/Data/Scenario \
  Versions/Current/Data/Consts \
  Versions/Current/Data/Other/Text

python3 tools/android/prepare_data_android.py \
  --output DataAndroid \
  --mode symlink

python3 Tools/android/build_geometry_index.py \
  --data-root Versions/Current/Data \
  --output DataAndroid/Converted/geometry_index.tsv

(
  cd Tools/android
  npm install --ignore-scripts
  node convert_granny_geometry.mjs \
    --input ../../Versions/Current/Data/bin/Geometries \
    --output ../../DataAndroid/Converted/Geometries \
    --all
)

clang++ -std=c++17 -Wall -Wextra -Werror \
  -Iandroid/app/src/main/cpp \
  tools/android/audio_decode_smoke.cpp \
  android/app/src/main/cpp/bk2_android_audio_decode.cpp \
  -o build/android/audio_decode_smoke

build/android/audio_decode_smoke \
  Sound/acknowledgements/US/tank/tank1_voice0/tank1-23-1.wav
```

From `android/`:

```sh
./gradlew :app:assembleDebug
```

The wrapper uses Gradle 9.4.1, matching Android Gradle Plugin 9.2.x
compatibility requirements.

To smoke-test the bootstrap on device after installing the debug APK and
staging `DataAndroid`, filter logcat for:

```sh
adb logcat -s Blitzkrieg2
```

The expected diagnostic line starts with `BK2 Android startup probe` and reports
  `legacy_vfs=ready`, the representative `db_type_*` IDs from the Android-linked
  single-player descriptor slice, `legacy_script=linked`,
  `profile_runtime=linked`, `save_inventory=probed`, `gamex_runtime=linked`,
  `game_root=present`, `sp_content=probed` with campaign/map/movie counters,
  custom mission counts, and `legacy_database=open` when the staged data is
  visible. `Blitzkrieg2Activity` also logs a second line from
  `NativeBridge.runAudioBackendProbe()` starting with `audio_backend=probed`;
  it verifies the Android PCM mixer with one-shot and looped clips, pause/resume
  behavior, seek, pan/volume, and mixed stereo output. The same line must report
  `legacy_isfx=probed`, `legacy_stopped=true`, and
  `legacy_3d_attenuation=true`, proving the real legacy `ISFX` facade reaches the
  mixer. `NativeBridge.runAudioDeviceProbe()` then reports `audio_device=open`,
  `state=Started`, the selected API, stream format, callback count, and rendered
  frame count. `NativeBridge.runMusicStreamingProbe()` reports
  `music_stream=probed`, decoded/consumed frame counts, source format, duration,
  and ring underruns. `NativeBridge.runLegacyMusicProbe()` must then report
  `legacy_music=probed`, `legacy_music_channel_paused=true`,
  `legacy_music_pause_held=true`, `legacy_music_resume_advanced=true`, and
  `legacy_music_stopped=true`. The native GameActivity loop also logs
  `render_backend=ready`; on the ARM64 emulator this currently reports
  `renderer=OpenGL ES 3.0`. The next native line must report
  `legacy_texture=probed`, `container_whole=yes`, `readback=4x4`,
  `gpu_handle=yes`, `uploaded_levels=1`, `gtexture_color=probed`,
  `gtexture_checker=probed`, `gtexture_dds=probed`, and
  `gtexture_dxt1=probed`, `gtexture_dxt3=probed`, and
  `gtexture_dxt5=probed`, proving the Android `CTextureLock` path, bgfx texture
  upload after window creation, file-backed DDS loading through the legacy
  `CFileTexture` path, and DXT block decode to RGBA8. A later render diagnostic
  reports the window size, a nonzero frame count after the first `NGfx::Flip()`
  call, and `primitives=4` from the temporary overlay: three solid rectangles
  and one textured rectangle submitted through `C2DQuadsRenderer::AddRect()`
  from a legacy `CTexture`. Real devices should prefer Vulkan unless bgfx falls
  back. A
  further line from
  `NativeBridge.runSinglePlayerCatalogProbe()`
  starts with `sp_catalog=probed` and points to
  `single_player_catalog_probe.json`, the detailed per-campaign/per-mission
  Android runtime catalog. With `Versions/Current/Data`, `Scenario`, `Consts`,
  and `Other/Text` staged on the ARM64 emulator, the current gate reports
  `sp_catalog_campaigns=3/3`, `sp_catalog_chapters=12/12`,
  `sp_catalog_missions=69`, `sp_catalog_mission_maps=69/69`,
  `sp_catalog_tutorial_maps=4/4`, `sp_catalog_unique_maps=73`,
  `sp_catalog_map_data_refs=34/34`, `sp_catalog_script_refs=74/74`,
  `sp_catalog_objectives=253`, `sp_catalog_script_movies=13`, and
  `sp_catalog_issues=21`, all of them currently `missing_android_movie` items
  for campaign/chapter cinematics whose Bink sources are missing or Git LFS
  pointers. The same startup line reports
  `client_consts=present`, `scene_consts=present`, `ai_consts=present`,
  `net_consts=present`, `shared_mp_consts=present`, and `ui_consts=present`.
  With the currently available real Bink files transcoded and pushed to the
  emulator, startup reports `sp_movie_source_refs=19/19` and
  `sp_movie_android_refs=1/19`; the catalog reports
  `sp_catalog_movie_source_refs=19/19` and
  `sp_catalog_movie_android_refs=1/19`. The ready Android ref is the root intro
  sequence (`cdv`, `Nival`, `Intro`). A fourth line comes from
  `NativeBridge.startFirstCampaignMissionProbe()`; when data is staged it starts
  an Android mission-state snapshot and reports `mission_state=active`,
  `mission_id`, map/script refs, objective state counters, chapter enabled and
  completed mission counters, reinforcement-call counters, player XP/rank
  counters, mission statistics, reward counters, chapter reinforcement inventory
  counters, win/cancel flags, and script movie counts. A fifth line from
  `NativeBridge.runMissionProgressionProbe()`
  starts with `mission_progression=probed` and verifies objective XP, mission
  statistics, reinforcement-call accounting, win-state progression, rewards,
  rank/promotion counters, Android-side kill matrix counters, leader
  assignment/XP/debt counters, favorite reinforcement counters, and medal
  counters against real staged campaign data without leaving the probe mission
  active. When the won mission leaves enabled missions in the same chapter, the
  probe also starts the first enabled follow-up mission and reports
  `probe_continued_mission_started=true` plus `continued_mission_starts`.
  A sixth line from `NativeBridge.runMissionCheckpointProbe()` starts with
  `mission_checkpoint=probed`; it saves the post-win/continued Android mission
  runtime to the current profile `Saves/android_progression_probe.bk2checkpoint`,
  resets the in-memory state, loads the checkpoint back, and reports
  `checkpoint_roundtrip=true` with the restored mission-state summary.

## Data Layout

Runtime data is expected outside the base APK, while user-writable state stays
in app-private storage:

```text
<files>/DataAndroid/
  Data/
    Scenario/
    Consts/
    Other/Text/
    bin/
    Music/
  Movies/

<files>/Profiles/
  global.cfg
  <profile>/
    user.cfg
    input.cfg
    Saves/
```

Android 11+ scoped storage prevents the native runtime from reliably reading
files pushed by `adb` into `<external-files>`. The install helper therefore
streams `DataAndroid` through `run-as` into app-private `<files>/DataAndroid`.
The runtime prefers that complete private tree and retains external-files only
as a compatibility fallback.

The port generates this tree from `Versions/Current/Data`,
the required `Complete` and `Sound` roots, Disk_J movie sources, and converted
`.bik` videos. If Git LFS returns pointer files instead of media blobs, the
video/content validators report that as a blocker.

## Current Interactive Runtime

The debug APK now opens a native mission selector. In the current staged data it
lists 75 map descriptors that have an adjacent readable `map.b2m` and hides 42
incomplete descriptors. The first USA campaign mission has been verified on the
ARM64 Android emulator with the real `map.b2m`, a `193x193` heightfield, 73,728
terrain triangles, Lua mission scripts, and 198 live legacy AI units.
The terrain renderer now reads `tileTerraMap`, resolves all 19 materials from
the mission's `TGTerraSet`, and uploads the shipped 512x512 DXT3 DDS textures.
It no longer stretches the minimap across the heightfield or draws the
diagnostic radar grid. Terrain textures use an opaque, base-level Android GPU
upload because their legacy DXT3 alpha and mip chain were authored for the
original multi-layer terrain shader and otherwise produced black distant
terrain in the current bgfx path. The current renderer assigns one dominant
material per tile; the original soft terrain-mask blending is still pending.

The Android content step now decodes the shipped Granny format-6/Oodle0
geometry into a small native `BK2MSH1` cache. The current checkout yields 1,457
renderable geometry files (640,989 vertices and 414,400 triangles); 26 geometry
records contain no renderable mesh and are skipped. An offline index follows
the original `RPGStats -> VisObj -> Model -> Geometry` XDB chain, preferring the
Asia season and falling back through the other seasons. The runtime keys this
index by a stable normalized DB-path hash because the current Android DB bridge
does not preserve `ObjectRecordID` on loaded RPG resources. On the first USA
mission, the verified runtime loads 31 distinct original geometries with no
missing converted file. Unmapped objects retain the temporary proxy instead of
disappearing.

The Android VFS resolves legacy asset paths case-insensitively while preserving
the actual on-disk spelling. This is required for content such as GB3.1 whose
XML uses `units/.../mechunitrpgstats.xdb` while the staged files use
`Units/.../MechUnitRPGStats.xdb`. On the ARM64 emulator, GB3.1 now reaches the
ready game stage with 358 live units and six script segments; GER1.0 reaches it
with 969 game units, 975 presentation units, and six script segments.

Touch controls currently implemented:

- tap a green player unit or formation to select it;
- tap a red hostile unit while a player unit is selected to issue the original
  legacy attack command;
- tap terrain to issue the original legacy move command and move the selected
  unit through the Android presentation bridge;
- drag with one finger to pan;
- pinch to zoom;
- rotate with a two-finger twist.

The in-game Android HUD shows the active mission and these controls. Its
`Missions` button returns to the native single-player mission list without
terminating the app process. It also shows completed, active, and failed
objective counts and the localized header of the current primary objective.
The header is decoded directly from the shipped UTF-16 text resource through
the case-insensitive Android VFS instead of relying on the legacy
16-bit-`wchar_t` assumption. The Android client now drains the original AI
update stream every simulation tick instead of allowing visual/client updates
to accumulate. `EFB_OBJECTIVE_CHANGED` updates are applied to both the original
scenario tracker and the Android campaign checkpoint state.

Original Lua `Win()`/`Loose()` calls now cross the Android input bridge.
They freeze the finished simulation, commit the corresponding campaign
win/cancel state, and show a centered victory/defeat panel with a return to the
mission selector. Previously those `local_win`/`local_loose` events were
discarded because the old desktop `WorldClient` was not linked. The in-game
`Surrender` action uses the same defeat path; it has been exercised on the
ARM64 emulator through the result panel and back to the 75-map selector.

The selected unit is yellow, the current attack target is orange, and moving
units follow the real terrain height. Converted original meshes keep the
current player/hostile tint until original material textures are wired.
Unmapped formations and objects remain green/red proxies. The camera starts
focused on the player's formation.

This is a playable runtime milestone, not a complete visual port. Terrain now
uses original game materials and the first runtime model path uses original
Granny geometry. Original model material textures, multi-part attachment
transforms, skeleton skinning, animations, combat effects, briefing/game HUD,
and the complete campaign-selection/progression UI remain unfinished.

The video transcode manifest now writes Android-canonical runtime paths. A Bink
ref such as `Movies\Nival.bik` maps to `DataAndroid/Movies/Nival.mp4`, not to a
duplicated `DataAndroid/Movies/Movies/...` path. The manifest also parses movie
sequence XML `<FileName>` entries, deduplicates duplicate Bink sources by runtime
key, prefers real media over Git LFS pointers, and emits missing-source jobs for
referenced movies that have no `.bik` file in the checkout. On the current
checkout, `validate_transcoded_videos.py` reports 8 ready MP4 files, 4 ready
referenced Bink keys, 22 blocked referenced Bink keys, 18 Git LFS pointer
sources, and 4 missing referenced sources.

`prepare_data_android.py` now writes `port_manifest.json` with a layout probe
for the files the Android DB bridge needs at startup:

- `Data/types.xml`
- `Data/index.bin`
- `Data/bin/Geometries`
- `Data/bin/Skeletons`
- `Data/bin/Animations`
- `Data/bin/AIGeometries`
- `Data/Scenario`
- `Data/Consts`
- `Data/Other/Text`
