# Blitzkrieg 2 Android Port Bootstrap

This Android port is maintained in
[`TrachukV/Blitzkrieg-2-Mobile`](https://github.com/TrachukV/Blitzkrieg-2-Mobile),
a community fork of the original
[`nival/Blitzkrieg-2`](https://github.com/nival/Blitzkrieg-2) repository. It is
a non-commercial native mobile port under the original project's license.

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
  original Granny meshes for mapped static objects and live AI units. Original
  model DDS materials are wired, and compatible infantry meshes use baked
  frames from the original rifle idle, move, and shoot clips. The shipped
  mission HUD, minimap, selected-unit cards, hit bars, and the original 4x3
  command grid are active. The grid is populated from the selected legacy
  unit's `CUserActions`; Move, Attack, Rotate, Stop, Entrench, Stand Ground,
  Spyglass, Clear Mines, Place Mines, and Build Trenches have native command
  round trips. Rotate, Spyglass, Clear Mines, and Place Mines consume the next
  terrain tap. Build Trenches preserves the desktop two-point interaction and
  sends `ACTION_COMMAND_ENTRENCH_BEGIN` followed by
  `ACTION_COMMAND_ENTRENCH_END`. Unavailable handlers render the shipped
  disabled icons. Double-tapping a selected unit expands selection to as many
  as twelve nearby units with matching legacy stats/type. All selected units
  receive world markers and individual HUD cards, while supported commands are
  registered and dispatched through `CGroupLogic::GroupCommand`. Holding for
  350 ms before a one-finger drag draws a screen-space selection rectangle and
  selects up to twelve friendly units of any type; quick drags still pan the
  camera. Tapping a member card changes the active unit and therefore the
  type-specific command grid, while common desktop actions still address the
  full mixed selection. Original converted geometry now also supplies projected
  silhouette shadows for static objects and live units; animated infantry
  shadows use the same current pre-skinned frame as the visible mesh. The camera
  reads the original horizontal FOV, pitch/yaw defaults, and distance range from
  the loaded `ClientGameConsts`, then applies the local player's shipped map
  anchor and optional placement exactly where the desktop mission does. The
  top-left headline now has a thread-safe three-line stack for original
  five-second scenario notifications. Objective feedback combines the shipped
  notification prefix with the objective header, while
  `EFB_REINFORCEMENT_CENTER_LOCAL_PLAYER` resolves and displays the shipped
  reinforcement text. General runtime skinning, remaining action-specific
  clips, a multi-line notification console, command subpanels, briefings, and
  the rest of the legacy UI are still pending.
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
  restores the previous state. Real AI combat now forwards the original
  `CStatistics::UnitKilled` payload into this Android state as well, preserving
  killer/victim players, reinforcement types, infantry classification, and
  experience price for campaign matrices, player XP, and leader progress.
  Reinforcement XP-level mutations for non-leader units, the munchkin medal
  check, UI statistics screen, and the final legacy save object graph are still
  not linked.
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
  Versions/Current/Data/Reinforcements \
  Versions/Current/Data/Other/Text \
  Versions/Current/Data/Scene/TexAndMats/All/Effects \
  Versions/Current/Data/Scene/TexAndMats/All/Units/Weapons \
  Versions/Current/Data/UI \
  Versions/Current/Data/Fonts \
  Versions/Current/Data/Weapons

# The menu click sound descriptor lives under the 18k-file acknowledgement
# tree, so add just that one path rather than the whole directory.
git sparse-checkout add \
  Versions/Current/Data/Other/AckSetRPGStats/ButtonClickSound.xdb

python3 tools/android/prepare_data_android.py \
  --output DataAndroid \
  --mode symlink

(
  cd Tools/android
  npm install --ignore-scripts
  node convert_granny_geometry.mjs \
    --input ../../Versions/Current/Data/bin/Geometries \
    --output ../../DataAndroid/Converted/Geometries \
    --idle-animation ../../Versions/Current/Data/bin/Animations/3977 \
    --move-animation ../../Versions/Current/Data/bin/Animations/3967 \
    --attack-animation ../../Versions/Current/Data/bin/Animations/3972 \
    --death-animation ../../Versions/Current/Data/bin/Animations/3961 \
    --lying-idle-animation ../../Versions/Current/Data/bin/Animations/3968 \
    --lying-move-animation ../../Versions/Current/Data/bin/Animations/3984 \
    --lying-attack-animation ../../Versions/Current/Data/bin/Animations/3970 \
    --skip-unsupported \
    --all
)

python3 Tools/android/build_geometry_index.py \
  --data-root Versions/Current/Data \
  --converted-geometry-root DataAndroid/Converted/Geometries \
  --output DataAndroid/Converted/geometry_index.tsv

clang++ -std=c++17 -Wall -Wextra -Werror \
  -Iandroid/app/src/main/cpp \
  tools/android/audio_decode_smoke.cpp \
  android/app/src/main/cpp/bk2_android_audio_decode.cpp \
  -o build/android/audio_decode_smoke

build/android/audio_decode_smoke \
  Sound/acknowledgements/US/tank/tank1_voice0/tank1-23-1.wav

build/android/audio_decode_smoke \
  Versions/Current/Data/Sounds/menu/clik01.wav
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
    Reinforcements/
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

`Data/Reinforcements` is runtime payload, not optional catalog metadata. If a
sparse checkout omits it, Lua still reaches `LandReinforcementFromMap` and can
emit the arrival notification, but no player formation is created and the
legacy war fog remains closed. The content validator and staging manifest now
reject that incomplete layout.

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
terrain in the current bgfx path. Terrain transitions now follow the original
`TileMasks.cpp` rules: material priority, the 3x3 Gaussian mask, cumulative
alpha layers, and each terrain type's `ScaleCoeff` are applied to
layer-specific vertices. USA 1.0, GB3.1, and GER1.0 have been exercised on the
ARM64 emulator with all 19, 20, and 19 terrain textures respectively.

Water is now a separate dynamic bgfx mesh instead of a terrain color fallback.
The runtime reconstructs its coastline from `STerrainInfo::seaMask` (or the
packed `optimizedSeaMask`), keeps the desktop surface height of `z=0.1`, and
loads the shipped season-specific `Terrain/Water/<season>/water.dds`. The first
`SWater` wave amplitude, period, enable flag, and texture tiling count drive a
20 Hz vertex/UV animation through a dedicated dynamic vertex buffer, so water
updates no longer require re-uploading all units and static objects. On ARM64
US1.2 this produced a `129x129` mask, 2,575 water nodes, 3,021 rendered
coastline nodes, 5,532 triangles, and a ready Asia DDS GPU handle. Two
one-second-apart captures of a static 600x300 lake crop measured PSNR 31.76 dB,
confirming the surface changed between frames without a camera move.

Roads are no longer baked into the coarse terrain fallback. The runtime reads
each map's `STerrain::roads`, converts the `SVSOInstance` centerline positions
and widths from legacy AI coordinates with `AI2Vis`, reconstructs the
left/center/right atlas spans from `SRoadDesc`, projects every ribbon endpoint
onto the current heightfield at the desktop `z+0.1` offset, and submits the
result as alpha-blended world layers. Some binary road materials do not expose
their `pTexture` through the current Android DB bridge, so the fallback resolver
maps the original seasonal road descriptor names to the shipped
`Terrain/roads/<season>/*.dds` files; it does not introduce replacement art.
On ARM64 US1.2 all 12 descriptors rendered as 524 segments and 1,048 triangles,
and all three referenced Asia road DDS files received valid GPU handles. The
road running through the starting friendly formation is visible above the
terrain and below units, vegetation, shadows, and legacy fog of war.

Terrain rivers now follow the original `RiversBuilder.cpp` path far enough to
restore both the terrain modifier and the visible water. The runtime converts
each `SRiverDesc`/`SVSOInstance` control point and width with `AI2Vis`, carves
the heightfield by the desktop four-unit river depth with its four-tile ridge
transition, and rebuilds the eight-cell bottom plus two descriptor-sized
alpha-water ribbons. The original per-point opacity, center opacity, tiling,
season, and `bottom.dds`, `water.dds`, and `water2.dds` assets are preserved. A
local copy of the original Win32 LCG uses `GetVSOSeed`'s first/last-control-point
distance without touching the simulation RNG. It applies the same seeded
`fBorderRand` displacement to both the terrain modifier and rendered ribbon,
then applies each water layer's `fDisturbance` to its internal cells. The two
water layers also scroll their V coordinate by the original signed
`fStreamSpeed` while simulation animation time advances.

On ARM64 GB3.1 the one 279-point river produced 11,676 triangles, disturbed
3,348 internal water vertices, carved 1,933 terrain vertices after the seeded
border displacement, animated two layers at their descriptor speeds, and
loaded all three Summer DDS files into valid GPU handles.

Terrain precipices now follow the original `PrecipicesRender.cpp`
`CreatePrecipiceMesh` path. Every serialized `STerrainInfo::SPrecipice` is
walked node by node; each visible node contributes the vertex column between
its `minHeights`/`maxHeights` entries, columns shorter than the desktop
`DEF_MIN_PRECIPICE_HEIGHT` of `0.025` are skipped exactly as in the original,
and consecutive columns are stitched with the same `nMaxVertsNum`/`fCoeff`
index interpolation. Texture U advances by the largest planar gap between the
interpolated column pairs and V accumulates the 3D edge length downward from
the top vertex, both scaled by `fTexGeomScale` over the material texture size.
The `SFoot` skirts are rebuilt from the same terrain data and use their
`pFootMaterial` as alpha-blended layers.

`PrecipicesManager.cpp` collects two kinds of precipice, and both are now
rendered. Crag precipices keep the crag VSO id and take `SCragDesc`'s
`pRidgeMaterial`; river precipices carry the river VSO id plus the `0x10000`
(left) or `0x20000` (right) bank marker and take `SRiverDesc`'s
`pPrecipiceMaterial`. River bank materials are resolved the same way the river
water layers are — the texture's `szDestName` first, then the seasonal
`Terrain/Water/<season>/crags.dds` — because the crag material naming
convention would otherwise point at an unstaged `Scene/TexAndMats` path. The
precipice pass reports its exact texture set, so the river gate keeps counting
only the three water layers.

On ARM64 GB3.1 all 7 serialized precipices rendered, including both banks of
its single river, for 867 node columns and 8,658 triangles plus 5 foot skirts
with 1,094 triangles across 3 textures and 5 GPU-ready layers; the river bank
resolved `River_RiverDesc.xdb` to `Terrain/Water/summer/crags.dds`. GER1.1
rendered 35 of its 36 serialized precipices — 34 crags and one river bank —
with 5,472 triangles, 34 foot skirts, and 11 GPU-ready layers; its remaining
bank has no column above the minimum precipice height, so the original draws
nothing there either. US1.2, which has no river, rendered all 36 crag
precipices and 36 foot skirts with 7 GPU-ready layers. `bStayedOnTerrain`
bottom-vertex snapping and the `SPeak` collection are still follow-up work.

The original menu screens are now loaded from their shipped descriptors rather
than approximated by an Android layout. `bk2_android_menu_runtime.*` resolves
`UI/Game/Menu/MainMenu_WindowScreen.xdb` through the Android game database,
walks the `SWindowScreenShared` child graph, merges every unset instance
placement field with its shared descriptor the way `CWindow::InitByDesc` does,
and positions each window with the desktop `NUITools::ApplyWindowAlign` rules
(`EPA_LOW_END`, `ERA_CENTER`, `EPA_HIGH_END`, and the `EPA_MARGIN` case that
also rewrites the window size). Rects stay in the original 1024x768 interface
space the screens are authored in. Button background and pushed textures come
from the first `SButtonVisualState`, and captions come from the shipped UTF-16
files; the inline markup tags such as `<val button>` are split off as the
requested text style instead of being drawn as characters.

The resolved screen is also submitted through the existing bgfx 2D path.
`SBackgroundTiledTexture` panels are rebuilt with the desktop
`InitBorderAndFill` / `DivideSubrects` nine-band algorithm, including the
clipped texture maps on the last repeated row and column, and
`SBackgroundSimpleTexture` follows `CBackgroundSimpleTexture::Visit` with
`NUITools::ApplyTextureAllign`. Descriptor texture maps are authored in texture
pixels and are normalized against each `STexture` size. Submission uses the
desktop `VirtualToScreenX`/`VirtualToScreenY` mapping, which scales X and Y
independently from the 1024x768 layout to the active surface, so the screen
adapts to any device aspect exactly like the original does to any resolution.

Launching `Blitzkrieg2Activity` with the `SHOW_MENU` extra writes `menu=1` into
`selected_mission.txt`; the single-player runtime then reports
`menu_requested` instead of falling back to the first campaign mission, and the
shell renders the menu with no mission HUD over it.

Captions use the shipped bitmap fonts rather than Android text. `Data/Fonts`
supplies the `SFont` descriptors and glyph atlases, and each font's metrics
cache is opened from `Data/bin/fonts` by descriptor uid the way
`CFileFont::Recalc` does. Glyphs are placed with the original `STFCharacter`
fields — `nA` pre-space, `nBC` advance — plus the kerning pair table. The
markup tags are expanded through `SUIConstsB2::tags`, so `<val button>` resolves
to its `<font face=h2 ...>`, `<color=FFD1C6A4>` and `<center>` directives and
the caption is drawn in the original face, colour and alignment.

Window traversal follows `CWindow::Visit`: background, then text, then children
in their priority-sorted draw order, then the foreground. Priority matters here
because the menu's black backing panel is authored at `-1` and has to land
under the buttons. Invisible windows contribute no geometry and neither do
their children, which is what keeps the hidden Single Player sub-panel from
drawing over the main list.

On the ARM64 emulator the main menu screen resolves 28 windows, 16 buttons, 20
textures, and 17 captions, loads 4 shipped fonts, and submits 158 quads with
all 7 referenced textures on valid GPU handles over a 2856x1280 surface. The
rendered panel shows the original Main Menu header over Single Player,
Multiplayer, Options, Load MOD, Credits, Encyclopedia and Exit. The right-hand column reproduces the original
layout exactly: the panel lands at `x=779` with 170x42 buttons at `y=143`
(Single Player), `193` (Multiplayer), `243` (Options), `293` (Load MOD), `343`
(Credits), `393` (Encyclopedia), and `443` (Exit), matching the shipped
`-15,65` / `290x470` panel placement with its 60-pixel button margins. Each
button also reports its original reaction id (`single_player`, `multiplayer`,
`options`, `LoadMod`, `Credits`, `Encyclopedia`, `exit`, `single_player_back`).

The Android content step now decodes the shipped Granny format-6/Oodle0
geometry into a small native `BK2MSH1` cache. The complete current pass yields
2,510 renderable geometry files (1,126,692 vertices and 731,200 triangles);
44 geometry records contain no renderable mesh and are skipped, while 263
Oodle1 resources remain blocked. An offline index follows
the original `RPGStats -> VisObj -> Model -> Geometry` XDB chain, preferring the
Asia season and falling back through the other seasons. The runtime keys this
index by a stable normalized DB-path hash because the current Android DB bridge
does not preserve `ObjectRecordID` on loaded RPG resources. On the first USA
mission, the verified runtime loads 31 distinct original geometries with no
missing converted file. Unmapped objects retain the temporary proxy instead of
disappearing.

The converter raises the open decoder's default 32-mesh ceiling to the cache
reader's validated 128-mesh limit. Four referenced original resources exceed
the old ceiling: the railway center span, both large wooden bridge spans, and
the Erebus ship. Their complete 46, 55, 60, and 50 mesh sets are now preserved
instead of silently dropping the remaining model parts.

The same offline index resolves each selected Model's material list and each
Texture descriptor's original DDS `DestName`. Version 3 of the `BK2MSH1` cache
preserves Granny triangle material groups and can carry pre-skinned animation
frames. The converter samples the shipped RIFLE idle animation through the
mesh's real bone bindings and vertex weights; compatible infantry instances
advance those frames in the live mission. Additional caches are emitted only
for the same skinned meshes using the shipped RIFLE move, shoot, death,
lying-idle, crawl, and lying-shoot clips: the current pass creates 259 files per
action (32,913,536 bytes each). The presentation bridge marks infantry from the
original AI movement, attacking, and `CSoldier::IsLying()` states. The renderer
selects standing or prone action caches from those live states and falls back to
the base idle cache when a variant is unavailable. Android's headless runtime
also forwards `CStatistics::UnitDead`
directly because the old desktop world client that consumed
`SAIDeadUnitUpdate` is not linked. The renderer plays each death clip once,
clamps on its last frame, keeps the corpse for ten seconds, and falls back to
idle if an action cache is unavailable. The bgfx backend submits a separate
index layer per model texture. Direct DDS loading deliberately avoids the incomplete
Android `ObjectRecordID` table. On the first USA mission the verified runtime
loads all 38 referenced model textures for 38 material layers. Model textures
upload their complete shipped mip chain: allocating all DDS levels but uploading
only level zero made buildings and vehicles turn into black silhouettes at the
default camera distance. The full-chain path has been
exercised on USA1.0, GB3.1, and GER1.0 with 38, 84, and 81 model textures
respectively.

Fallback accounting now runs only on the final presentation meshes. The
discarded pre-presentation preview no longer counts squad spawn descriptors as
visible proxy models. Runtime checks on USA `US1.0`, GB `GB3.1`, and GER
`GER1.0` all report zero converted-geometry fallbacks for static and live
entities.

Some late content descriptors reference Granny record IDs whose binary streams
are absent from both this checkout and `origin/main`. The offline index rejects
those dead bindings instead of sending them to the runtime loader. Two explicit
compatibility substitutions cover visible GER1.0 objects: Pz IV F2 record
`1000090` uses the shipped Pz IV Ausf G model, the closest available variant,
and `Concretedot_2` record `1000206` uses the older shipped concrete pillbox.
These are original game assets but are documented stand-ins, not recovered
missing meshes. On GER1.0 these substitutions take
`missing_converted_geometry` from four to zero and remove the only real dynamic
proxy; the previously reported remaining fallbacks came from the obsolete
preview pass rather than the rendered world.

The Japanese assault landing boat uses Oodle1-compressed geometry
`92D68E1C-6B9E-4930-90A2-2B92D658005E`, which the open converter cannot decode.
The Android index therefore uses the shipped Japanese `Ka-Tsu` amphibious model
at `1.3x` scale as an explicit stand-in. Its dimensions and gameplay class are
closer than the other decodable naval assets; the original descriptor and
blocked stream remain untouched for later replacement.

The Japan assault-squad officer has the same Oodle1 limitation in
`Assault_officer/1_1_Model.xdb`. Its shipped main-squad Nambu officer model
(record `1805`) has identical declared dimensions and is used without scaling.
This removes the last live proxy encountered on `US1.2` while preserving the
correct nation, role, skeleton family, and original material.

Granny resources can use either numeric filenames or UUID filenames. The
converter and index now assign the latter the same stable positive runtime ID
and reject collisions before conversion. The current complete pass requests
2,817 resources: 2,510 convert, 44 contain no renderable mesh, 263 are reported
as blocked, and none fail unexpectedly. The blocked UUID files use Granny
Oodle1 compression; the bundled open decoder supports Oodle0, while the
repository's Oodle1 implementation is only present in the original 32-bit
Windows `granny2.dll`. Unsupported files are not reported as converted.
The offline index reads the Granny section table as well and omits those
blocked resources, so a mission cannot resolve an Oodle1 descriptor to a
nonexistent Android cache file. It is also generated against the completed
cache directory, which removes streams that decoded successfully but contained
no renderable mesh.

USA1.0's LST/LSI is one of those Oodle1 resources. Until a Windows offline
conversion stage is added, it uses the shipped ELKO hull at 1.6 scale as an
explicit naval stand-in. This removes the last USA1.0 dynamic proxy and reduces
its diagnostic fallbacks from 17 to 15. The original LST descriptor, texture,
and compressed geometry remain untouched so the stand-in can be removed when
that conversion stage is available.

The Android VFS resolves legacy asset paths case-insensitively while preserving
the actual on-disk spelling. This is required for content such as GB3.1 whose
XML uses `units/.../mechunitrpgstats.xdb` while the staged files use
`Units/.../MechUnitRPGStats.xdb`. On the ARM64 emulator, GB3.1 now reaches the
ready game stage with 358 live units and six script segments; GER1.0 reaches it
with 969 game units, 975 presentation units, and six script segments.

The offline geometry index applies the same compatibility to content that the
open repository relocated out of `/Scene/Geoms/All` and
`/Scene/TexAndMats/All`. It resolves case differences and legacy aliases such
as `damaged1_visobj.xdb` to the available `damaged01.xdb`. This restores the
original building meshes on GB3.1 and reduces its remaining geometry fallbacks
from 377 to 33.

Touch controls currently implemented:

- tap a green player unit or formation to select it;
- double-tap a selected player unit to select up to twelve nearby units of the
  same legacy type; the HUD shows one health card per selected unit;
- hold for 350 ms and drag one finger to draw a selection rectangle and select
  up to twelve friendly units of any type inside it; a quick drag still pans;
- tap any selected-unit HUD card to make that unit active, update the
  type-specific action grid, and keep common commands bound to the full group;
- press `P` or `Space` to toggle native mission pause and the centered
  original-style orange `PAUSED` overlay without opening the Android menu;
- the top-left headline resolves the localized header of the active primary
  legacy objective, with the internal mission ID used only before objectives
  are ready;
- original objective and reinforcement feedback temporarily replaces that
  headline in a stack of up to three active lines, then restores the active
  objective after five seconds;
- tap a red hostile unit while a player unit is selected to issue the original
  legacy attack command;
- tap terrain to issue the original legacy move command and move the selected
  unit through the Android presentation bridge;
- tap the original Move or Attack HUD button to arm a highlighted one-shot
  command mode, then tap its terrain destination or hostile target;
- tap the original Stop HUD button to send `ACTION_COMMAND_STOP` immediately
  through `CGroupLogic`;
- tap Clear Mines or Place Mines and then terrain to send the original
  `ACTION_COMMAND_CLEARMINE` or `ACTION_COMMAND_PLACEMINE`; mine placement
  retains the desktop command's visualization/queue flag;
- tap Build Trenches, then tap its start and end points to queue the original
  `ACTION_COMMAND_ENTRENCH_BEGIN` and `ACTION_COMMAND_ENTRENCH_END` pair;
- Move, Attack, Stop, and supported ability commands use one registered legacy
  AI group when multiple units are selected, preserving the original
  subgroup/formation command path;
- tap the original F10 button to pause the legacy simulation and audio, show
  the prominent orange `PAUSED` indicator, and open the mission menu; closing
  the menu resumes both;
- drag with one finger to pan;
- pinch to zoom;
- rotate with a two-finger twist.

Touch projection uses the same left-handed camera basis as bgfx. An earlier
cross-product order mirrored picking across the screen's X axis even though the
rendered units were correct; selection, explicit attack targets, and
screen-to-terrain movement now agree with their visible positions. The bgfx
vertical projection is derived from the original horizontal FOV and the current
surface aspect ratio, matching the desktop `CTransformStack::MakeProjective`
contract. Tap selection and drag selection use that same conversion.

Debug APKs also accept keyboard `F` to select the closest valid player/enemy
pair and issue the real legacy attack command. Keyboard `T` emits a debug-only
combat-effect payload between that pair to smoke-test the tracer renderer
without pretending that an AI shot occurred. Keyboard `K` then kills a hostile
infantry member through the original `CStatistics::UnitKilled` and
`CAIUnit::Die` paths so campaign statistics and death presentation can be
smoke-tested together. Keyboard `V` sends the same `local_win` event used by
mission Lua, allowing campaign autosave and continuation to be tested without
completing a full battle. Keyboard `L` toggles one live soldier between the
original standing and prone states for animation validation. Keyboard `M`
kills a visible mechanized unit through the same statistics/death path and is
used to validate the destruction presentation. Keyboard `N` injects the
reinforcement notification type so descriptor lookup, UTF-16 decoding, JNI
polling, and five-second expiry can be checked independently of scenario
timing. The original `CScripts::LandReinforcementFromMap` path remains active in
the linked AI runtime; `GER3.3` has also produced the same notification from a
real scenario call. All seven shortcuts are absent from release builds.

`Data/Weapons` is required runtime DB payload. Without it, mine descriptors keep
an unresolved `pWeapon`; the original `CMineStaticObject::Detonate` path then
dereferences that null resource when a unit crosses a mine. The staging
validator treats a missing or empty Weapons directory as a blocker.

The in-game Android HUD now uses the shipped `MissionMain.tga` panel,
`MiniMap/foreground.tga`, and original Move, Attack, and Stop button art
instead of the temporary top debug card. The minimap-corner Esc/F10 and
Objectives controls now use the shipped `EscMenuBtn_003.tga` and
`ObjectivesBtn_002.tga` images at their descriptor positions `(7,92)` and
`(193,92)`. Their live touch targets open the native pause menu and objective
summary instead of remaining invisible beside the command grid. Mission play
uses sticky immersive mode so the Android navigation handle no longer covers
the center panel. A
compact Android TGA decoder reads the real assets directly from staged
`Complete/UI`. `stageOriginalHudAssets` also packages the 4.2 MiB subset needed
for the mission panel, diamond minimap frame, selected-unit cards, hit bars,
and the action grid. Runtime file content remains the primary source, but the
decoder falls back to those APK assets if an external sync omitted
`Complete/UI`; the Gradle build fails if the required originals are absent.
The HUD view paints the desktop mission's black UI backdrop before compositing
the semitransparent TGA layers, so the 3D battlefield no longer leaks through
the center panel. The HUD also reports its actual 112dp height to native code.
The bgfx terrain view and camera projection stop at that boundary, while touch
picking, drag selection, pan, and pinch use the same playable content height.
On the ARM64 emulator the resulting diagnostic was a `2856x944` battlefield
over the `2856x1280` render surface with a 336-pixel bottom inset. Tapping
`(1240,390)` selected legacy unit `5198`, produced the matching world marker,
and populated the original portrait, health card, and command grid, verifying
that projection and input remain aligned after the inset.
The detailed Android objective summary is hidden during normal battlefield play
and toggled by the Objectives touch zone instead of remaining as a permanent
debug overlay. The minimap combines the map-specific background and live
presentation entities, then clips them into the original diamond frame. When
the map declares `MapInfo.pMiniMap`, Android decodes that original
mission-specific DDS through the legacy texture backend and uses terrain-type
colors only as a fallback. It applies the live
`GetMiniMapWarForInfo` grid using the desktop client's 128-alpha hidden-cell
overlay and 0-alpha visible-cell endpoint. Friendly and currently visible
hostile units remain readable above that layer; the active selection is yellow.
Tap or drag inside the diamond converts back to terrain coordinates and updates
the shared camera target without changing zoom or yaw. As in desktop
`CWindowMiniMap::SetViewport`, a magenta polygon traces the four current
battlefield camera rays and is clipped by the minimap frame. The `US1.2`
runtime loaded `us1_2_8x8_minimap_texture.dds` at `256x256`; verified taps
moved both the camera and that polygon from `177.692,184.25,0` to
`130.549,213.714,3.99608`. The F10
menu returns to the native single-player mission list without terminating the
app process. The center panel now follows the original battle-screen layout:
mission/objective text is kept at the top-left, while selecting a real legacy
AI unit populates the center with the shipped preview frame, Soldier or Tank
portrait, icon background, and live green/yellow/red HP bar. No card is drawn
when the selection is empty. This was exercised on the ARM64 emulator with
infantry unit `5236`; the native touch path selected that unit and the HUD
immediately rendered its original infantry art and current/max RPG-stat HP. A
second emulator pass removed external `Complete/UI` entirely and verified the
same panel, minimap, portrait, health card, and original action icons from the
bundled fallback.

The static presentation pass now accepts general `SObjectRPGStats` map records,
not only the previously enumerated building, fence, entrenchment, squad, mine,
and mechanized-unit types. USA US1.2 contains 1,965 of these general records;
restoring them raises the runtime's rendered map-object count from 215 to 2,180
of 2,254, while `missing_converted_geometry` remains zero. This returns the
original palms, bushes, reeds, crates, and other small props through the same
converted Granny geometry and DDS material cache. The geometry-index generator
now resolves every model material's original `SMaterial::AlphaMode` and carries
it beside the texture path. Android creates distinct render layers for
`AM_OPAQUE`, `AM_ALPHA_TEST`, and `AM_TRANSPARENT` materials, even when two
modes reference the same texture. Alpha-tested leaves use a dedicated fragment
shader that discards texels below the original engine's reference value of 120
while preserving depth writes; transparent brushwood and reeds use blending.
Precompiled GLES3 and SPIR-V alpha-test variants are checked in beside the
shader source, covering the emulator and physical-device bgfx backends without
requiring `shaderc` on the Android build host. This removes the opaque black
cards previously shown around transparent vegetation.

The renderer does not use its coarse convex projected-shadow fallback for
alpha-tested flora: it projects the original Granny triangles, preserves their
UVs, applies the same alpha-test mask, and outputs a reduced-opacity shadow
color. Crossed billboard planes therefore produce leaf and branch silhouettes
instead of large dark hulls. Opaque small props keep their projected shadows. A
fresh ARM64 GLES3 emulator run on GB3.1 reported 43 alpha-test layers with
69,638 triangles and 10 blended world layers with 30,580 triangles. The latter
count also includes the transparent road and river passes. An earlier US1.2
runtime pass reported
`map_objects=2254`, `rendered_objects=2180`,
`dynamic_rendered_objects=160`, `converted_geometry_cache=66`, and
`missing_converted_geometry=0`. The masked shadow pass contributed 19 texture
layers and 199,194 projected triangles. Selecting unit `5198` still populated
its world ring, portrait, health card, and action grid over the restored
vegetation.

The F10 menu
now owns an explicit native user-pause state rather than merely
covering a still-running battle. It resets pending touch-command mode, pauses
the mixer/output, stops legacy ticks and animation time, and shows the large
orange `PAUSED` label above the menu. Two paused battlefield captures taken two
seconds apart produced the same raw-frame checksum; closing F10 logged
`player_pause=false` and resumed the runtime. The objective
header is decoded directly from the shipped UTF-16 text resource through the
case-insensitive Android VFS instead of relying on the legacy
16-bit-`wchar_t` assumption. The Android client now drains the original AI
update stream every simulation tick instead of allowing visual/client updates
to accumulate. `EFB_OBJECTIVE_CHANGED` updates are applied to both the original
scenario tracker and the Android campaign checkpoint state. Received,
completed, and failed objective notifications use the original localized
prefix plus objective header. Local-player reinforcement feedback uses
`NTF_REINFORCEMENT_ARRIVED` and the shipped UTF-16
`ReinfArrived/Text.txt`. Up to three messages occupy the top-left stack for five
seconds from their event time before the active objective returns. This is
still a bounded approximation of the desktop mission console, not its complete
scrolling history.

The battle renderer now consumes `IAILogic::GetMiniMapWarForInfo()` instead of
approximating vision with Android-side circles. The legacy AI exports its
smoothed 0-to-`VIS_POWER()` visibility grid; Android converts it into a black
alpha mesh drawn after terrain, static objects, and units, while units that
`IsVisible(playerParty)` rejects are omitted from the presentation snapshot.
The mesh is capped at 96 quads per axis and rebuilt only for a new fog
generation. On USA US1.2 the ARM64 runtime reported a real `128x128`, power-7
grid with 1,894 fully visible cells; the resulting battlefield has the same
black unexplored perimeter and soft visible-area boundary as the original
battle view. The bgfx clear color is also black so uncovered map edges no
longer expose the old teal diagnostic background.

Original Lua `Win()`/`Loose()` calls now cross the Android input bridge.
They freeze the finished simulation, commit the corresponding campaign
win/cancel state, and show a centered victory/defeat panel with a return to the
mission selector. Previously those `local_win`/`local_loose` events were
discarded because the old desktop `WorldClient` was not linked. The in-game
`Surrender` action uses the same defeat path; it has been exercised on the
ARM64 emulator through the result panel and back to the campaign selector.
Successful missions now also persist `android_autosave.bk2checkpoint`. The
selector exposes **Continue campaign** only when that checkpoint exists; it
restores the campaign state, advances a completed chapter when necessary, and
starts the next enabled mission. New-campaign buttons are built from the real
`GameRoot.xdb` order and the shipped UTF-16 campaign-name resources for USA,
Germany, and USSR. The selected campaign and one of the four original difficulty
levels are handed to the C++ mission tracker, which selects the first mission
whose unlock and reinforcement requirements are satisfied instead of
incorrectly launching the locked final mission at index zero. The raw 75-map
list is hidden behind an explicit debug browser for port testing.

Selected units now get a yellow ground ring and the current attack target gets
an orange ring, so touch feedback remains visible on converted original models
whose DDS materials intentionally ignore the old proxy tint. The central HUD
also shows the selected unit's live current/maximum HP from the legacy RPG
stats. Presentation API v3 carries both values for external renderers. Moving
units follow the real terrain height. Converted original meshes use their
resolved original DDS material. The geometry index also resolves frame-specific
`segments/Item/VisObj` bindings used by composite map objects such as
entrenchments. If a referenced visual Granny stream is absent but its original
AI geometry is available, the converter uses the legacy `AI_TO_VIS` scale from
`Vis2AI.h`. On USA 1.0 this replaces all 142 entrenchment proxies with the
original segment meshes and materials. Fence frames follow the original
`GetSegmentsByFrameIndex` order across center, both damaged, and destroyed
segment lists; minor fence objects are now included in the presentation mesh.
On GB3.1 this reduces converted-geometry fallbacks from 1,263 to 377, and on
GER1.0 from 905 to 95. Bridge spans follow `CMOBridge::GetElement`: frame zero
uses the end model and all other frames use the center model. The open content
tree omits each bridge's `center_visobj.xdb`, so the offline index recovers its
shipped seasonal `summer_center_model.xdb` directly. This restores all four
asphalt bridge spans on GB3.1 and reduces its remaining diagnostic fallbacks
from 33 to 29. Unmapped formations and objects remain green/red proxies. The
camera keeps the player formation as its startup anchor but now loads the
desktop mission values from `ClientGameConsts`: 26-degree horizontal FOV,
45-degree default pitch and yaw, and 150/170/200
minimum/average/maximum distance. Pan anchors are clamped to the same terrain
rectangle as `CWorldClient::LoadMap`, and pinch zoom is clamped to the loaded
distance range. The HUD campaign and mission title comes from the mission ID
loaded by the native runtime instead of assuming the first USA map.

This is a playable runtime milestone, not a complete visual port. Terrain now
uses original game materials and the first runtime model path uses original
Granny geometry and DDS materials. Compatible rifle infantry now uses original
bone weights plus standing idle/move/shoot/death and prone
idle/crawl/shoot animation frames selected from live AI state. Real
`SAIInfantryShotUpdate` and `SAIMechShotUpdate` events now produce moving tracer
ribbons at the source and destination coordinates supplied by the legacy AI
simulation. Infantry ribbons use the shipped DXT3
`GunShotTraceBlue_Texture.dds`; mechanized ribbons use the shipped
`GunShotTraceOrange_texture.dds`. These effect-only layers enable alpha blending
without changing the opaque model layers. The native particle bridge also
replaces the temporary shot marker with the shipped `Shot8_Texture.dds` muzzle
flash. More importantly, it now consumes real `SAIHitUpdate` events and
resolves the desktop shell hit mapping (`HIT`, `MISS`, `REFLECT`, `GROUND`,
`WATER`, and `AIR`) to each shell's original `SComplexEffect`. Mechanized death
uses the unit RPG descriptor's `pEffectSmoke` or `pEffectFatality`, matching the
selected legacy death animation. The bridge snapshots each resolved
descriptor's scene-effect variant and carries its emitter texture sequence,
dimensions, cycle duration, time offset, scale, speed, particle count, and
alpha/additive conversion mode to bgfx. XDB light instances carry the same
placement, cycle, speed, offset, and scale timing and produce an additive
ground pulse through the shipped `LightFX/Flare_Texture.dds`; the unavailable
desktop animated-light track is approximated with a fast flash envelope. The
visible wreck remains for ten seconds. The earlier fixed `Fire2`–`Fire5` plus
`Explosion2`/`Explosion3` recipe remains only as a fallback when the content
has no usable effect descriptor. Legacy effect DDS files that encode
transparency through RGB
luminance with a zero alpha channel receive a compatible alpha channel during
GPU upload. Particle layers depth-test against the scene but do not write the
depth buffer, so overlapping fire and smoke remain visible.

The first model-shadow layer projects every converted Granny vertex along one
sun direction, computes a convex ground hull, and submits that hull once through
an alpha-blended white-texture layer. Static hulls remain in the cached world
mesh; dynamic hulls are rebuilt from the currently selected animation frame.
Alpha-tested flora takes a more precise path: the projected mesh retains the
original UVs and passes through a dedicated masked-shadow shader, so only the
texture's leaf/branch silhouette darkens the ground.
This restores the strong readable ground contact visible in the original game
without using generic circular blobs. It is not yet the desktop engine's
terrain-conforming shadow-map renderer, so steep terrain and self-shadowing
remain part of the larger renderer port.

The bridge is descriptor-driven but is not a byte-for-byte port of the desktop
particle evaluator. The checked-in content exposes the XDB emitter recipes and
textures, while the compiled particle keyframe tracks used by the old renderer
are not available to this Android path. Android therefore preserves descriptor
selection, timing, texture animation, scale, speed, count, and blend mode but
approximates per-particle position, size, and color curves with camera-facing
billboards and animated light intensity with a ground flare. A verified US1.2
emulator run consumed
`AntitankMineExplosion_ComplexEffect.xdb`, rendered both of its emitters from
`Explosion1_Texture.dds`, resolved one XDB light instance through
`Flare_Texture.dds`, and retired the effect after its descriptor lifetime.
Multi-mesh Granny models preserve all of their model mesh bindings;
global `InitialPlacement` is intentionally not reapplied because shipped
infantry geometry already contains the correct root placement. Standing-to-prone
and prone-to-standing transition clips, GPU/runtime skinning, original
effect-attached Granny geometry, complete briefing HUD behavior, and the original
chapter-map/statistics/progression UI remain unfinished.

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
- `Data/Reinforcements`
- `Data/Other/Text`
