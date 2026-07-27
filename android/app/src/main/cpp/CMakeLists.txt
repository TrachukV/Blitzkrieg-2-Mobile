cmake_minimum_required(VERSION 3.22.1)
project(blitzkrieg2_android LANGUAGES C CXX)

option(BK2_ENABLE_LEGACY_SOURCES "Attempt to compile selected Blitzkrieg 2 legacy runtime sources" OFF)
option(BK2_ENABLE_LEGACY_CORE_SOURCES "Compile the first Android-ported legacy System/Misc runtime sources" ON)
option(BK2_ENABLE_LEGACY_DB_TYPE_SOURCES "Compile Android-verified generated DB resource type sources" ON)
option(BK2_ENABLE_LEGACY_SCRIPT_SOURCES "Compile the Android-ported legacy Lua script runtime" ON)
option(BK2_ENABLE_LEGACY_GAMEX_RUNTIME_SOURCES "Compile Android-safe GameX runtime helpers for single-player content discovery" ON)
option(BK2_ENABLE_LEGACY_SCENARIO_TRACKER "Compile the original single-player campaign and mission tracker" ON)
option(BK2_ENABLE_LEGACY_AI_FOUNDATION "Compile the portable Common_RTS_AI pathfinding foundation" ON)
option(BK2_ENABLE_LEGACY_AI_LOGIC_RUNTIME "Compile the original single-player unit and combat simulation" ON)
option(BK2_ENABLE_LEGACY_SOUND_RUNTIME "Compile the Android legacy ISFX sample runtime" ON)
option(BK2_ENABLE_LEGACY_MUSIC_RUNTIME "Compile the Android legacy playlist and music runtime" ON)
option(BK2_ENABLE_BGFX_RENDERER "Compile the Android bgfx Vulkan/GLES renderer bootstrap" ON)
option(BK2_ENABLE_LEGACY_TEXTURE_RUNTIME "Compile the Android legacy CTexture/I2DBuffer runtime" ON)
option(BK2_ENABLE_LEGACY_ZLIB "Compile the portable legacy zlib module into the Android bootstrap" ON)

find_package(game-activity REQUIRED CONFIG)
find_package(oboe REQUIRED CONFIG)

if(BK2_ENABLE_BGFX_RENDERER)
    include(FetchContent)
    set(BGFX_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(BGFX_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BGFX_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(BGFX_INSTALL OFF CACHE BOOL "" FORCE)
    set(BGFX_CUSTOM_TARGETS OFF CACHE BOOL "" FORCE)
    set(BGFX_CONFIG_VIDEO OFF CACHE BOOL "" FORCE)
    set(BGFX_CONFIG_MULTITHREADED ON CACHE BOOL "" FORCE)
    set(BGFX_CONFIG_DEBUG_ANNOTATION OFF CACHE BOOL "" FORCE)
    set(BGFX_OPENGLES_VERSION 30 CACHE STRING "" FORCE)
    FetchContent_Declare(
        bgfx_cmake
        GIT_REPOSITORY https://github.com/bkaradzic/bgfx.cmake.git
        GIT_TAG efee36ac4ec86839d12ea228349ace636f48fc4d
        GIT_SHALLOW TRUE
        GIT_SUBMODULES bgfx bimg bx
        GIT_SUBMODULES_RECURSE TRUE
    )
    FetchContent_MakeAvailable(bgfx_cmake)
    foreach(_bk2_bgfx_property COMPILE_DEFINITIONS INTERFACE_COMPILE_DEFINITIONS)
        get_target_property(_bk2_bgfx_defs bgfx ${_bk2_bgfx_property})
        if(_bk2_bgfx_defs)
            list(FILTER _bk2_bgfx_defs EXCLUDE REGEX "^BGFX_CONFIG_DEBUG_ANNOTATION=")
            set_property(TARGET bgfx PROPERTY ${_bk2_bgfx_property} "${_bk2_bgfx_defs}")
        endif()
    endforeach()
    target_compile_definitions(bgfx PUBLIC BGFX_CONFIG_DEBUG_ANNOTATION=0)
endif()

add_library(blitzkrieg2 SHARED
    bk2_android_audio_backend.cpp
    bk2_android_audio_decode.cpp
    bk2_android_audio_output.cpp
    bk2_android_audio_probe.cpp
    bk2_android_pcm_stream.cpp
    bk2_android_database.cpp
    bk2_android_main.cpp
    bk2_android_mission_runtime.cpp
    bk2_android_mission_script.cpp
    bk2_android_platform.cpp
    bk2_android_profiles.cpp
    bk2_android_save_inventory.cpp
    bk2_android_single_player_runtime.cpp
    bk2_android_sp_catalog.cpp
    bk2_android_startup_probe.cpp
    bk2_android_vfs.cpp
    bk2_android_video_bridge.cpp
    bk2_android_vorbis_stream.cpp
    bk2_legacy_system_probe.cpp
    bk2_legacy_random_android.cpp
    bk2_legacy_streams_android.cpp
    bk2_presentation_api.cpp
    bk2_port_paths.cpp
)

target_include_directories(blitzkrieg2 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ../../../../../Versions/Temporary/Engine/Sources
)

target_compile_definitions(blitzkrieg2 PRIVATE
    BK2_ANDROID=1
    BK2_SINGLE_PLAYER_ONLY=1
    BK2_ANDROID_PROFILE_RUNTIME=1
)

target_link_libraries(blitzkrieg2
    android
    log
    mediandk
    -Wl,--whole-archive
    game-activity::game-activity_static
    -Wl,--no-whole-archive
    oboe::oboe
)

if(BK2_ENABLE_BGFX_RENDERER)
    target_compile_definitions(blitzkrieg2 PRIVATE
        BK2_BGFX_RENDERER_ENABLED=1
    )
    target_include_directories(blitzkrieg2 PRIVATE
        ${bgfx_SOURCE_DIR}/bx/include
        ${bgfx_SOURCE_DIR}/bgfx/examples/common/debugdraw
    )
    target_sources(blitzkrieg2 PRIVATE
        bk2_bgfx_render_backend.cpp
        bk2_legacy_gfx_android.cpp
    )
    target_link_libraries(blitzkrieg2
        bgfx
        EGL
        GLESv2
    )
endif()

if(BK2_ENABLE_LEGACY_AI_FOUNDATION)
    target_compile_definitions(blitzkrieg2 PRIVATE
        BK2_LEGACY_AI_FOUNDATION_ENABLED=1
    )
    set(BK2_SOURCES_ROOT ../../../../../Versions/Temporary/Engine/Sources)
    target_sources(blitzkrieg2 PRIVATE
        ${BK2_SOURCES_ROOT}/Common_RTS_AI/AIAngle.cpp
        ${BK2_SOURCES_ROOT}/Common_RTS_AI/AIMap.cpp
        ${BK2_SOURCES_ROOT}/Common_RTS_AI/BasePathUnit.cpp
        ${BK2_SOURCES_ROOT}/Common_RTS_AI/ChecksumSaver.cpp
        ${BK2_SOURCES_ROOT}/Common_RTS_AI/CollisionInternal2.cpp
        ${BK2_SOURCES_ROOT}/Common_RTS_AI/CommonPathFinder.cpp
        ${BK2_SOURCES_ROOT}/Common_RTS_AI/GroupSmoothPath.cpp
        ${BK2_SOURCES_ROOT}/Common_RTS_AI/HungarianMethod.cpp
        ${BK2_SOURCES_ROOT}/Common_RTS_AI/InFormationPath.cpp
        ${BK2_SOURCES_ROOT}/Common_RTS_AI/InGroupPath.cpp
        ${BK2_SOURCES_ROOT}/Common_RTS_AI/PassMarkers.cpp
        ${BK2_SOURCES_ROOT}/Common_RTS_AI/PathSearchFuncs.cpp
        ${BK2_SOURCES_ROOT}/Common_RTS_AI/Pathfinders.cpp
        ${BK2_SOURCES_ROOT}/Common_RTS_AI/StandartDirPath.cpp
        ${BK2_SOURCES_ROOT}/Common_RTS_AI/StandartDirectPath.cpp
        ${BK2_SOURCES_ROOT}/Common_RTS_AI/StandartPath2.cpp
        ${BK2_SOURCES_ROOT}/Common_RTS_AI/StandartSmoothMechPath.cpp
        ${BK2_SOURCES_ROOT}/Common_RTS_AI/StandartSmoothPath.cpp
        ${BK2_SOURCES_ROOT}/Common_RTS_AI/StaticMapHeights.cpp
        ${BK2_SOURCES_ROOT}/Common_RTS_AI/StaticPathInternal.cpp
        ${BK2_SOURCES_ROOT}/Common_RTS_AI/StdAfx.cpp
        ${BK2_SOURCES_ROOT}/Common_RTS_AI/TerraAIObserver.cpp
        ${BK2_SOURCES_ROOT}/Common_RTS_AI/Terrain.cpp
        ${BK2_SOURCES_ROOT}/Common_RTS_AI/Tools.cpp
        ${BK2_SOURCES_ROOT}/Common_RTS_AI/aiobjectbase.cpp
        bk2_android_ai_runtime.cpp
        bk2_android_ai_image.cpp
    )
endif()

if(BK2_ENABLE_LEGACY_AI_LOGIC_RUNTIME)
    target_compile_definitions(blitzkrieg2 PRIVATE
        BK2_LEGACY_AI_LOGIC_RUNTIME_ENABLED=1
    )
    set(BK2_AI_LOGIC_ROOT ../../../../../Versions/Temporary/Engine/Sources/AILogic)
    file(GLOB BK2_AI_LOGIC_SOURCES CONFIGURE_DEPENDS
        ${BK2_AI_LOGIC_ROOT}/*.cpp
    )
    list(FILTER BK2_AI_LOGIC_SOURCES EXCLUDE REGEX
        "/(AIDebugInfo|AIGeometryDebug|AILogicCommand|AILogicCommands|BalanceTest)\\.cpp$"
    )
    target_sources(blitzkrieg2 PRIVATE
        ${BK2_AI_LOGIC_SOURCES}
        ${BK2_SOURCES_ROOT}/Main/GameTimerInternal.cpp
        ${BK2_SOURCES_ROOT}/Stats_B2_M1/AIUpdates.cpp
        ${BK2_SOURCES_ROOT}/Stats_B2_M1/StatusUpdates.cpp
        bk2_android_legacy_game_runtime.cpp
        bk2_android_legacy_ai_bridges.cpp
    )
endif()

if(BK2_ENABLE_LEGACY_TEXTURE_RUNTIME)
    target_compile_definitions(blitzkrieg2 PRIVATE
        BK2_LEGACY_TEXTURE_RUNTIME_ENABLED=1
    )
    target_sources(blitzkrieg2 PRIVATE
        bk2_legacy_2d_quads_android.cpp
        bk2_legacy_texture_android.cpp
        ../../../../../Versions/Temporary/Engine/Sources/3Dmotor/GTexture.cpp
    )
endif()

if(BK2_ENABLE_LEGACY_ZLIB)
    target_sources(blitzkrieg2 PRIVATE
        ../../../../../Versions/Temporary/Engine/Sources/zlib/adler32.c
        ../../../../../Versions/Temporary/Engine/Sources/zlib/compress.c
        ../../../../../Versions/Temporary/Engine/Sources/zlib/crc32.c
        ../../../../../Versions/Temporary/Engine/Sources/zlib/deflate.c
        ../../../../../Versions/Temporary/Engine/Sources/zlib/gzio.c
        ../../../../../Versions/Temporary/Engine/Sources/zlib/infblock.c
        ../../../../../Versions/Temporary/Engine/Sources/zlib/infcodes.c
        ../../../../../Versions/Temporary/Engine/Sources/zlib/inffast.c
        ../../../../../Versions/Temporary/Engine/Sources/zlib/inflate.c
        ../../../../../Versions/Temporary/Engine/Sources/zlib/inftrees.c
        ../../../../../Versions/Temporary/Engine/Sources/zlib/infutil.c
        ../../../../../Versions/Temporary/Engine/Sources/zlib/trees.c
        ../../../../../Versions/Temporary/Engine/Sources/zlib/uncompr.c
        ../../../../../Versions/Temporary/Engine/Sources/zlib/zutil.c
    )
    target_include_directories(blitzkrieg2 PRIVATE
        ../../../../../Versions/Temporary/Engine/Sources/zlib
    )
endif()

if(BK2_ENABLE_LEGACY_CORE_SOURCES)
    target_compile_definitions(blitzkrieg2 PRIVATE
        BK2_LEGACY_CORE_SOURCES_ENABLED=1
    )
    set(BK2_SOURCES_ROOT ../../../../../Versions/Temporary/Engine/Sources)
    target_sources(blitzkrieg2 PRIVATE
        ${BK2_SOURCES_ROOT}/Misc/stdafx.cpp
        ${BK2_SOURCES_ROOT}/Misc/Tools.cpp
        ${BK2_SOURCES_ROOT}/Misc/Geom.cpp
        ${BK2_SOURCES_ROOT}/Misc/Spline.cpp
        ${BK2_SOURCES_ROOT}/Misc/StrProc.cpp
        ${BK2_SOURCES_ROOT}/System/stdafx.cpp
        ${BK2_SOURCES_ROOT}/System/Basic.cpp
        ${BK2_SOURCES_ROOT}/System/BasicShare.cpp
        ${BK2_SOURCES_ROOT}/System/BinChunkSaver.cpp
        ${BK2_SOURCES_ROOT}/System/BinaryResources.cpp
        ${BK2_SOURCES_ROOT}/System/BitStreams.cpp
        ${BK2_SOURCES_ROOT}/System/CheckSumLog.cpp
        ${BK2_SOURCES_ROOT}/System/ChunklessSaver.cpp
        ${BK2_SOURCES_ROOT}/System/CombinerVFS.cpp
        ${BK2_SOURCES_ROOT}/System/CmdLine.cpp
        ${BK2_SOURCES_ROOT}/System/Commands.cpp
        ${BK2_SOURCES_ROOT}/System/ConsoleBufferInternal.cpp
        ${BK2_SOURCES_ROOT}/System/Cruncher.cpp
        ${BK2_SOURCES_ROOT}/System/Dg.cpp
        ${BK2_SOURCES_ROOT}/System/FastMath.cpp
        ${BK2_SOURCES_ROOT}/System/FilePath.cpp
        ${BK2_SOURCES_ROOT}/System/Formatting.cpp
        ${BK2_SOURCES_ROOT}/System/FreeIDs.cpp
        ${BK2_SOURCES_ROOT}/System/GameDB.cpp
        ${BK2_SOURCES_ROOT}/System/GlobalVars.cpp
        ${BK2_SOURCES_ROOT}/System/GResource.cpp
        ${BK2_SOURCES_ROOT}/System/LightXML.cpp
        ${BK2_SOURCES_ROOT}/System/LightXMLUtils.cpp
        ${BK2_SOURCES_ROOT}/System/LogStream.cpp
        ${BK2_SOURCES_ROOT}/System/Logger.cpp
        ${BK2_SOURCES_ROOT}/System/ObjectFactory.cpp
        ${BK2_SOURCES_ROOT}/System/Serialize.cpp
        ${BK2_SOURCES_ROOT}/System/Singleton.cpp
        ${BK2_SOURCES_ROOT}/System/Text.cpp
        ${BK2_SOURCES_ROOT}/System/Time.cpp
        ${BK2_SOURCES_ROOT}/System/VFS.cpp
        ${BK2_SOURCES_ROOT}/System/VFSOperations.cpp
        ${BK2_SOURCES_ROOT}/System/XMLChunkSaver.cpp
        ${BK2_SOURCES_ROOT}/System/XMLReader.cpp
        ${BK2_SOURCES_ROOT}/System/XMLSAXPArser.cpp
        ${BK2_SOURCES_ROOT}/System/XmlUtils.cpp
        ${BK2_SOURCES_ROOT}/libdb/stdafx.cpp
        ${BK2_SOURCES_ROOT}/libdb/Bind.cpp
        ${BK2_SOURCES_ROOT}/libdb/BindArray.cpp
        ${BK2_SOURCES_ROOT}/libdb/BindProcessor.cpp
        ${BK2_SOURCES_ROOT}/libdb/BindProcessorSaveLoad.cpp
        ${BK2_SOURCES_ROOT}/libdb/Checksum.cpp
        ${BK2_SOURCES_ROOT}/libdb/DBObserverContainer.cpp
        ${BK2_SOURCES_ROOT}/libdb/Database.cpp
        ${BK2_SOURCES_ROOT}/libdb/GameDatabase.cpp
        ${BK2_SOURCES_ROOT}/libdb/Logger.cpp
        ${BK2_SOURCES_ROOT}/libdb/ReportMetaInfo.cpp
        ${BK2_SOURCES_ROOT}/libdb/StructMetaInfo.cpp
        ${BK2_SOURCES_ROOT}/libdb/StructMetaInfoSetGet.cpp
        ${BK2_SOURCES_ROOT}/libdb/Type.cpp
        ${BK2_SOURCES_ROOT}/libdb/TypeDef.cpp
        ${BK2_SOURCES_ROOT}/libdb/TypeDefType.cpp
        ${BK2_SOURCES_ROOT}/libdb/Variant.cpp
    )
endif()

if(BK2_ENABLE_LEGACY_DB_TYPE_SOURCES)
    target_compile_definitions(blitzkrieg2 PRIVATE
        BK2_LEGACY_DB_TYPE_SOURCES_ENABLED=1
    )
    set(BK2_SOURCES_ROOT ../../../../../Versions/Temporary/Engine/Sources)
    target_sources(blitzkrieg2 PRIVATE
        ${BK2_SOURCES_ROOT}/3Dmotor/DBScene.cpp
        ${BK2_SOURCES_ROOT}/AILogic/DBAIConsts.cpp
        ${BK2_SOURCES_ROOT}/B2_M1_Terrain/DBPreLight.cpp
        ${BK2_SOURCES_ROOT}/B2_M1_Terrain/DBTerrain.cpp
        ${BK2_SOURCES_ROOT}/B2_M1_Terrain/DBTerrainSpot.cpp
        ${BK2_SOURCES_ROOT}/B2_M1_Terrain/DBVSO.cpp
        ${BK2_SOURCES_ROOT}/B2_M1_Terrain/DBWater.cpp
        ${BK2_SOURCES_ROOT}/GameX/DBConsts.cpp
        ${BK2_SOURCES_ROOT}/GameX/DBGameRoot.cpp
        ${BK2_SOURCES_ROOT}/GameX/DBMPConsts.cpp
        ${BK2_SOURCES_ROOT}/GameX/DBScenario.cpp
        ${BK2_SOURCES_ROOT}/GameX/dbgameoptions.cpp
        ${BK2_SOURCES_ROOT}/Main/DBNetConsts.cpp
        ${BK2_SOURCES_ROOT}/SceneB2/DBSceneConsts.cpp
        ${BK2_SOURCES_ROOT}/Sound/DBMusicSystem.cpp
        ${BK2_SOURCES_ROOT}/Sound/DBSound.cpp
        ${BK2_SOURCES_ROOT}/Sound/DBSoundDesc.cpp
        ${BK2_SOURCES_ROOT}/Stats_B2_M1/AckTypes.cpp
        ${BK2_SOURCES_ROOT}/Stats_B2_M1/ActionsRemap.cpp
        ${BK2_SOURCES_ROOT}/Stats_B2_M1/Commands_Actions.cpp
        ${BK2_SOURCES_ROOT}/Stats_B2_M1/ConstructorInfo.cpp
        ${BK2_SOURCES_ROOT}/Stats_B2_M1/DBAttachedModelVisObj.cpp
        ${BK2_SOURCES_ROOT}/Stats_B2_M1/DBCameraConsts.cpp
        ${BK2_SOURCES_ROOT}/Stats_B2_M1/DBClientConsts.cpp
        ${BK2_SOURCES_ROOT}/Stats_B2_M1/DBConstructorProfile.cpp
        ${BK2_SOURCES_ROOT}/Stats_B2_M1/DBMapInfo.cpp
        ${BK2_SOURCES_ROOT}/Stats_B2_M1/DBNotifications.cpp
        ${BK2_SOURCES_ROOT}/Stats_B2_M1/DBPassProfile.cpp
        ${BK2_SOURCES_ROOT}/Stats_B2_M1/DBPlaneManuvers.cpp
        ${BK2_SOURCES_ROOT}/Stats_B2_M1/DBVisObj.cpp
        ${BK2_SOURCES_ROOT}/Stats_B2_M1/dbreinforcements.cpp
        ${BK2_SOURCES_ROOT}/Stats_B2_M1/FeedBackUpdates.cpp
        ${BK2_SOURCES_ROOT}/Stats_B2_M1/IconsSet.cpp
        ${BK2_SOURCES_ROOT}/Stats_B2_M1/M1Actions.cpp
        ${BK2_SOURCES_ROOT}/Stats_B2_M1/M1UnitActions.cpp
        ${BK2_SOURCES_ROOT}/Stats_B2_M1/M1UnitSpecific.cpp
        ${BK2_SOURCES_ROOT}/Stats_B2_M1/M1UnitType.cpp
        ${BK2_SOURCES_ROOT}/Stats_B2_M1/RPGStats.cpp
        ${BK2_SOURCES_ROOT}/Stats_B2_M1/RPGStatsAddIn.cpp
        ${BK2_SOURCES_ROOT}/Stats_B2_M1/RPGStatsAutomagic.cpp
        ${BK2_SOURCES_ROOT}/Stats_B2_M1/Season.cpp
        ${BK2_SOURCES_ROOT}/Stats_B2_M1/UIEntries.cpp
        ${BK2_SOURCES_ROOT}/Stats_B2_M1/UnitTypes.cpp
        ${BK2_SOURCES_ROOT}/Stats_B2_M1/UserActions.cpp
        ${BK2_SOURCES_ROOT}/UI/DBUIConsts.cpp
        ${BK2_SOURCES_ROOT}/UI/DBUserInterface.cpp
        ${BK2_SOURCES_ROOT}/UISpecificB2/DBUISpecificB2.cpp
    )
endif()

if(BK2_ENABLE_LEGACY_SOUND_RUNTIME)
    if(NOT BK2_ENABLE_LEGACY_DB_TYPE_SOURCES)
        message(FATAL_ERROR "BK2_ENABLE_LEGACY_SOUND_RUNTIME requires BK2_ENABLE_LEGACY_DB_TYPE_SOURCES")
    endif()
    target_compile_definitions(blitzkrieg2 PRIVATE
        BK2_LEGACY_SOUND_RUNTIME_ENABLED=1
    )
    set(BK2_SOURCES_ROOT ../../../../../Versions/Temporary/Engine/Sources)
    target_sources(blitzkrieg2 PRIVATE
        bk2_legacy_sound_engine_android.cpp
        bk2_legacy_sound_probe.cpp
        ${BK2_SOURCES_ROOT}/Sound/Sound2D.cpp
        ${BK2_SOURCES_ROOT}/Sound/SoundManager.cpp
        ${BK2_SOURCES_ROOT}/Sound/SoundSample.cpp
    )
endif()

if(BK2_ENABLE_LEGACY_MUSIC_RUNTIME)
    if(NOT BK2_ENABLE_LEGACY_DB_TYPE_SOURCES)
        message(FATAL_ERROR "BK2_ENABLE_LEGACY_MUSIC_RUNTIME requires BK2_ENABLE_LEGACY_DB_TYPE_SOURCES")
    endif()
    target_compile_definitions(blitzkrieg2 PRIVATE
        BK2_LEGACY_MUSIC_RUNTIME_ENABLED=1
    )
    set(BK2_SOURCES_ROOT ../../../../../Versions/Temporary/Engine/Sources)
    target_sources(blitzkrieg2 PRIVATE
        bk2_legacy_music_probe.cpp
        ${BK2_SOURCES_ROOT}/Sound/Fade.cpp
        ${BK2_SOURCES_ROOT}/Sound/MusicSystem.cpp
        ${BK2_SOURCES_ROOT}/Sound/Pause.cpp
        ${BK2_SOURCES_ROOT}/Sound/PlayList.cpp
        ${BK2_SOURCES_ROOT}/Sound/PlayTime.cpp
        ${BK2_SOURCES_ROOT}/Sound/Track.cpp
    )
endif()

if(BK2_ENABLE_LEGACY_SCRIPT_SOURCES)
    target_compile_definitions(blitzkrieg2 PRIVATE
        BK2_LEGACY_SCRIPT_SOURCES_ENABLED=1
    )
    set(BK2_SOURCES_ROOT ../../../../../Versions/Temporary/Engine/Sources)
    set(BK2_SCRIPT_SOURCES
        ${BK2_SOURCES_ROOT}/Script/CommonFunctions.cpp
        ${BK2_SOURCES_ROOT}/Script/Script.cpp
        ${BK2_SOURCES_ROOT}/Script/ScriptWrapperInternal.cpp
        ${BK2_SOURCES_ROOT}/Script/lapi.cpp
        ${BK2_SOURCES_ROOT}/Script/lcode.cpp
        ${BK2_SOURCES_ROOT}/Script/ldebug.cpp
        ${BK2_SOURCES_ROOT}/Script/ldo.cpp
        ${BK2_SOURCES_ROOT}/Script/lfunc.cpp
        ${BK2_SOURCES_ROOT}/Script/lgc.cpp
        ${BK2_SOURCES_ROOT}/Script/llex.cpp
        ${BK2_SOURCES_ROOT}/Script/lmem.cpp
        ${BK2_SOURCES_ROOT}/Script/lobject.cpp
        ${BK2_SOURCES_ROOT}/Script/lparser.cpp
        ${BK2_SOURCES_ROOT}/Script/lsaver.cpp
        ${BK2_SOURCES_ROOT}/Script/lstate.cpp
        ${BK2_SOURCES_ROOT}/Script/lstring.cpp
        ${BK2_SOURCES_ROOT}/Script/ltable.cpp
        ${BK2_SOURCES_ROOT}/Script/ltm.cpp
        ${BK2_SOURCES_ROOT}/Script/lundump.cpp
        ${BK2_SOURCES_ROOT}/Script/lvm.cpp
        ${BK2_SOURCES_ROOT}/Script/lzio.cpp
        ${BK2_SOURCES_ROOT}/Script/scriptCommon.cpp
        ${BK2_SOURCES_ROOT}/Script/scriptPtr.cpp
        ${BK2_SOURCES_ROOT}/Script/stdafx.cpp
    )
    target_sources(blitzkrieg2 PRIVATE ${BK2_SCRIPT_SOURCES})
    set_source_files_properties(
        ${BK2_SCRIPT_SOURCES}
        PROPERTIES COMPILE_OPTIONS "-fexceptions"
    )
endif()

if(BK2_ENABLE_LEGACY_GAMEX_RUNTIME_SOURCES)
    target_compile_definitions(blitzkrieg2 PRIVATE
        BK2_LEGACY_GAMEX_RUNTIME_SOURCES_ENABLED=1
    )
    set(BK2_SOURCES_ROOT ../../../../../Versions/Temporary/Engine/Sources)
    target_sources(blitzkrieg2 PRIVATE
        bk2_android_gamex_consts.cpp
        ${BK2_SOURCES_ROOT}/GameX/CClientGameConsts.cpp
        ${BK2_SOURCES_ROOT}/GameX/CustomMissions.cpp
    )
endif()

if(BK2_ENABLE_LEGACY_SCENARIO_TRACKER)
    if(NOT BK2_ENABLE_LEGACY_GAMEX_RUNTIME_SOURCES)
        message(FATAL_ERROR "BK2_ENABLE_LEGACY_SCENARIO_TRACKER requires BK2_ENABLE_LEGACY_GAMEX_RUNTIME_SOURCES")
    endif()
    target_compile_definitions(blitzkrieg2 PRIVATE
        BK2_LEGACY_SCENARIO_TRACKER_ENABLED=1
    )
    target_sources(blitzkrieg2 PRIVATE
        ${BK2_SOURCES_ROOT}/GameX/ScenarioTracker.cpp
    )
endif()

if(BK2_ENABLE_LEGACY_SOURCES)
    include(${CMAKE_CURRENT_LIST_DIR}/../../../../cmake/bk2_runtime_modules.cmake)
    message(FATAL_ERROR
        "BK2_ENABLE_LEGACY_SOURCES is intentionally gated. "
        "Run tools/android/module_inventory.py, port the platform/render/audio blockers it reports, "
        "then enable modules incrementally instead of linking all legacy sources at once.")
endif()
