#include "bk2_android_startup_probe.h"

#include "bk2_android_database.h"
#include "bk2_android_platform.h"
#include "bk2_android_save_inventory.h"
#include "bk2_android_video_bridge.h"
#include "bk2_android_vfs.h"
#include "bk2_port_paths.h"

#include "System/stdafx.h"
#include "System/VFSOperations.h"
#include "Misc/StrProc.h"

#if defined(BK2_ANDROID_PROFILE_RUNTIME)
#include "Main/Profiles.h"
#endif

#if defined(BK2_LEGACY_DB_TYPE_SOURCES_ENABLED)
#include "3Dmotor/DBScene.h"
#include "AILogic/DBAIConsts.h"
#include "B2_M1_Terrain/DBTerrain.h"
#include "GameX/DBConsts.h"
#include "GameX/DBGameRoot.h"
#include "GameX/DBMPConsts.h"
#include "GameX/DBScenario.h"
#include "GameX/dbgameoptions.h"
#include "Main/DBNetConsts.h"
#include "SceneB2/DBSceneConsts.h"
#include "Sound/DBMusicSystem.h"
#include "Sound/DBSound.h"
#include "Stats_B2_M1/DBCameraConsts.h"
#include "Stats_B2_M1/DBClientConsts.h"
#include "Stats_B2_M1/DBMapInfo.h"
#include "Stats_B2_M1/DBNotifications.h"
#include "Stats_B2_M1/RPGStats.h"
#include "Stats_B2_M1/UIEntries.h"
#include "UI/DBUIConsts.h"
#include "UI/DBUserInterface.h"
#include "UISpecificB2/DBUISpecificB2.h"
#endif

#if defined(BK2_LEGACY_GAMEX_RUNTIME_SOURCES_ENABLED)
#include "GameX/CustomMissions.h"
#include "GameX/DBGameRoot.h"
#include "GameX/GetConsts.h"
#endif

#include <dirent.h>
#include <jni.h>
#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>
#include <string>
#include <sys/stat.h>

namespace bk2::android {
namespace {

struct PathProbe {
    bool exists = false;
    int entries = -1;
};

const char* Present(bool value) {
    return value ? "present" : "missing";
}

std::string JoinPath(const std::string& left, const std::string& right) {
    if (left.empty()) {
        return right;
    }
    if (right.empty()) {
        return left;
    }
    return left[left.size() - 1] == '/' ? left + right : left + "/" + right;
}

bool IsRegularFile(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool IsDirectory(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

PathProbe ProbeDirectory(const std::string& path) {
    PathProbe probe;
    probe.exists = IsDirectory(path);
    if (!probe.exists) {
        return probe;
    }

    DIR* dir = opendir(path.c_str());
    if (dir == nullptr) {
        return probe;
    }

    int entries = 0;
    while (dirent* entry = readdir(dir)) {
        const char* name = entry->d_name;
        if (std::string(name) == "." || std::string(name) == "..") {
            continue;
        }
        ++entries;
    }
    closedir(dir);
    probe.entries = entries;
    return probe;
}

std::string NormalizeLegacyRefStd(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    const size_t xpointer = path.find('#');
    if (xpointer != std::string::npos) {
        path.resize(xpointer);
    }
    while (!path.empty() && path[0] == '/') {
        path.erase(0, 1);
    }
    return path;
}

std::string NormalizeLegacyRef(const string& path) {
    return NormalizeLegacyRefStd(path.c_str());
}

std::string LowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool EndsWithNoCase(const std::string& value, const char* suffix) {
    const std::string lower_value = LowerAscii(value);
    const std::string lower_suffix = LowerAscii(suffix);
    return lower_value.size() >= lower_suffix.size() &&
           lower_value.compare(lower_value.size() - lower_suffix.size(), lower_suffix.size(), lower_suffix) == 0;
}

bool VfsFileExists(NVFS::IVFS* vfs, const string& ref) {
    if (vfs == 0 || ref.empty()) {
        return false;
    }
    return vfs->DoesFileExist(ref) || vfs->DoesFileExist(NormalizeLegacyRef(ref).c_str());
}

void AppendPathValue(std::ostringstream& report, const char* name, const std::string& value) {
    report << "; " << name << "=" << (value.empty() ? "<unset>" : value);
}

void AppendDirectoryProbe(std::ostringstream& report, const std::string& root, const char* relative_path) {
    const PathProbe probe = ProbeDirectory(JoinPath(root, relative_path));
    report << "; direct " << relative_path << "=" << Present(probe.exists);
    if (probe.entries >= 0) {
        report << "(" << probe.entries << " entries)";
    }
}

void AppendFileProbe(std::ostringstream& report, const std::string& root, const char* relative_path) {
    report << "; direct " << relative_path << "="
           << Present(IsRegularFile(JoinPath(root, relative_path)));
}

void AppendVfsProbe(std::ostringstream& report, NVFS::IVFS* vfs, const char* legacy_path) {
    report << "; vfs " << legacy_path << "="
           << Present(vfs != nullptr && vfs->DoesFileExist(legacy_path));
}

void AppendDbTypeProbe(std::ostringstream& report) {
#if defined(BK2_LEGACY_DB_TYPE_SOURCES_ENABLED)
    report << "; db_type_TextEntry=0x" << std::hex << NDb::STextEntry::typeID
           << "; db_type_Texture=0x" << NDb::STexture::typeID
           << "; db_type_WindowScreen=0x" << NDb::SWindowScreen::typeID
           << "; db_type_GameRoot=0x" << NDb::SGameRoot::typeID
           << "; db_type_GameConsts=0x" << NDb::SGameConsts::typeID
           << "; db_type_AIGameConsts=0x" << NDb::SAIGameConsts::typeID
           << "; db_type_NetGameConsts=0x" << NDb::SNetGameConsts::typeID
           << "; db_type_ClientGameConsts=0x" << NDb::SClientGameConsts::typeID
           << "; db_type_SceneConsts=0x" << NDb::SSceneConsts::typeID
           << "; db_type_MultiplayerConsts=0x" << NDb::SMultiplayerConsts::typeID
           << "; db_type_TooltipContext=0x" << NDb::STooltipContext::typeID
           << "; db_type_OptionSystem=0x" << NDb::SOptionSystem::typeID
           << "; db_type_Campaign=0x" << NDb::SCampaign::typeID
           << "; db_type_Chapter=0x" << NDb::SChapter::typeID
           << "; db_type_MapInfo=0x" << NDb::SMapInfo::typeID
           << "; db_type_DifficultyLevel=0x" << NDb::SDifficultyLevel::typeID
           << "; db_type_CameraLimits=0x" << NDb::SCameraLimits::typeID
           << "; db_type_Notification=0x" << NDb::SNotification::typeID
           << "; db_type_NotificationEvent=0x" << NDb::SNotificationEvent::typeID
           << "; db_type_MechUnitRPGStats=0x" << NDb::SMechUnitRPGStats::typeID
           << "; db_type_SquadRPGStats=0x" << NDb::SSquadRPGStats::typeID
           << "; db_type_PlayerRank=0x" << NDb::SPlayerRank::typeID
           << "; db_type_ReinforcementTypes=0x" << NDb::SReinforcementTypes::typeID
           << "; db_type_TerraSet=0x" << NDb::STGTerraSet::typeID
           << "; db_type_MapMusic=0x" << NDb::SMapMusic::typeID
           << "; db_type_ComplexSoundDesc=0x" << NDb::SComplexSoundDesc::typeID
           << "; db_type_UIConstsB2=0x" << NDb::SUIConstsB2::typeID
           << std::dec;
#else
    report << "; db_type_TextEntry=disabled";
#endif
}

void AppendScriptProbe(std::ostringstream& report) {
#if defined(BK2_LEGACY_SCRIPT_SOURCES_ENABLED)
    report << "; legacy_script=linked";
#else
    report << "; legacy_script=disabled";
#endif
}

void AppendProfileProbe(std::ostringstream& report, NVFS::IVFS* vfs) {
#if defined(BK2_ANDROID_PROFILE_RUNTIME)
    NProfile::LoadProfile();
    vector<wstring> profiles;
    NProfile::GetAllProfiles(&profiles);
    const wstring profile_name = NProfile::GetCurrentProfileName();
    const string profile_dir = NProfile::GetCurrentProfileDir();
    const string user_cfg = profile_dir + "user.cfg";
    const string input_cfg = profile_dir + "input.cfg";
    const string saves_dir = profile_dir + "Saves\\";

    report << "; profile_runtime=linked"
           << "; profile_name=" << NStr::ToMBCS(profile_name).c_str()
           << "; profile_dir=" << profile_dir.c_str()
           << "; profile_count=" << profiles.size()
           << "; profile_user_cfg=" << Present(VfsFileExists(vfs, user_cfg))
           << "; profile_input_cfg=" << Present(VfsFileExists(vfs, input_cfg))
           << "; profile_saves_dir=" << Present(IsDirectory(JoinPath(GetPortPaths().files_dir, NormalizeLegacyRef(saves_dir))));
#else
    report << "; profile_runtime=disabled";
#endif
}

void AppendSaveInventoryProbe(std::ostringstream& report) {
    const SaveInventory inventory = ScanSaveInventory();
    report << "; save_inventory=probed"
           << "; save_dir=" << (inventory.legacy_save_dir.empty() ? "<unset>" : inventory.legacy_save_dir)
           << "; save_dir_exists=" << Present(inventory.save_dir_exists)
           << "; save_dir_writable=" << Present(inventory.save_dir_writable)
           << "; save_files=" << inventory.save_files
           << "; save_info_files=" << inventory.info_files
           << "; save_paired_entries=" << inventory.paired_entries
           << "; save_orphan_info_files=" << inventory.orphan_info_files
           << "; save_other_files=" << inventory.other_files
           << "; save_newest_mtime=" << inventory.newest_modified_time;
}

struct SinglePlayerContentStats {
    int campaign_refs = 0;
    int campaigns_loaded = 0;
    int campaigns_missing = 0;
    int chapter_refs = 0;
    int chapters_loaded = 0;
    int chapters_missing = 0;
    int mission_refs = 0;
    int unique_mission_refs = 0;
    int maps_loaded = 0;
    int maps_missing = 0;
    int tutorial_refs = 0;
    int tutorial_maps_loaded = 0;
    int tutorial_maps_missing = 0;
    int script_refs = 0;
    int script_refs_present = 0;
    int map_data_refs = 0;
    int map_data_refs_present = 0;
    int movie_refs = 0;
    int movie_source_refs_present = 0;
    int movie_android_refs_present = 0;
    int map_objective_refs = 0;
    int script_movie_sequences = 0;
};

void CountFileRef(NVFS::IVFS* vfs, const NFile::CFilePath& ref, int* refs, int* present) {
    if (ref.empty()) {
        return;
    }
    ++(*refs);
    if (VfsFileExists(vfs, ref)) {
        ++(*present);
    }
}

void CountMovieRef(NVFS::IVFS* vfs, const NFile::CFilePath& ref, SinglePlayerContentStats* stats) {
    if (ref.empty()) {
        return;
    }
    ++stats->movie_refs;
    if (VfsFileExists(vfs, ref)) {
        ++stats->movie_source_refs_present;
    }
    const std::vector<std::string> android_refs = AndroidVideoRefsForLegacyMovie(ref.c_str());
    bool all_android_refs_present = !android_refs.empty();
    for (std::vector<std::string>::const_iterator it = android_refs.begin(); it != android_refs.end(); ++it) {
        if (!VfsFileExists(vfs, it->c_str())) {
            all_android_refs_present = false;
        }
    }
    if (all_android_refs_present) {
        ++stats->movie_android_refs_present;
    }
}

void ProbeMapInfo(
        NVFS::IVFS* vfs,
        const NDb::SMapInfo* map,
        SinglePlayerContentStats* stats) {
    if (map == 0) {
        ++stats->maps_missing;
        return;
    }

    ++stats->maps_loaded;
    CountFileRef(vfs, map->szMapDesignerFileRef, &stats->map_data_refs, &stats->map_data_refs_present);
    CountFileRef(vfs, map->szScriptFileRef, &stats->script_refs, &stats->script_refs_present);
    stats->map_objective_refs += map->objectives.size();
    stats->script_movie_sequences += map->scriptMovies.scriptMovieSequences.size();
}

std::string DbPtrId(const CDBPtr<NDb::SMapInfo>& ptr) {
    const NDb::CResource* bare = ptr.GetBarePtrNoLoad();
    return bare != 0 ? bare->GetDBID().ToString().c_str() : "";
}

void AppendSinglePlayerContentProbe(
        std::ostringstream& report,
        NVFS::IVFS* vfs,
        const NDb::SGameRoot* game_root) {
    SinglePlayerContentStats stats;
    if (game_root == 0) {
        report << "; sp_content=game_root_missing";
        return;
    }

    CountMovieRef(vfs, game_root->szIntroMovie, &stats);
    stats.campaign_refs = game_root->campaigns.size();

    std::set<std::string> unique_maps;
    for (vector<CDBPtr<NDb::SCampaign> >::const_iterator campaign_it = game_root->campaigns.begin();
         campaign_it != game_root->campaigns.end();
         ++campaign_it) {
        const NDb::SCampaign* campaign = campaign_it->GetPtr();
        if (campaign == 0) {
            ++stats.campaigns_missing;
            continue;
        }

        ++stats.campaigns_loaded;
        CountFileRef(vfs, campaign->szScriptFileRef, &stats.script_refs, &stats.script_refs_present);
        CountMovieRef(vfs, campaign->szIntroMovie, &stats);
        CountMovieRef(vfs, campaign->szOutroMovie, &stats);
        stats.chapter_refs += campaign->chapters.size();

        for (vector<CDBPtr<NDb::SChapter> >::const_iterator chapter_it = campaign->chapters.begin();
             chapter_it != campaign->chapters.end();
             ++chapter_it) {
            const NDb::SChapter* chapter = chapter_it->GetPtr();
            if (chapter == 0) {
                ++stats.chapters_missing;
                continue;
            }

            ++stats.chapters_loaded;
            CountFileRef(vfs, chapter->szScriptFileRef, &stats.script_refs, &stats.script_refs_present);
            CountMovieRef(vfs, chapter->szIntroMovie, &stats);
            stats.mission_refs += chapter->missionPath.size();

            for (vector<NDb::SMissionEnableInfo>::const_iterator mission_it = chapter->missionPath.begin();
                 mission_it != chapter->missionPath.end();
                 ++mission_it) {
                const std::string map_id = DbPtrId(mission_it->pMap);
                if (!map_id.empty() && !unique_maps.insert(map_id).second) {
                    continue;
                }
                ProbeMapInfo(vfs, mission_it->pMap.GetPtr(), &stats);
            }
        }
    }

    stats.tutorial_refs = game_root->tutorialMaps.size();
    for (vector<NDb::SGameRoot::STutorialMap>::const_iterator tutorial_it = game_root->tutorialMaps.begin();
         tutorial_it != game_root->tutorialMaps.end();
         ++tutorial_it) {
        CountFileRef(vfs, tutorial_it->szDifficultyFileRef, &stats.script_refs, &stats.script_refs_present);
        const std::string map_id = DbPtrId(tutorial_it->pMapInfo);
        if (!map_id.empty()) {
            unique_maps.insert(map_id);
        }
        const NDb::SMapInfo* tutorial_map = tutorial_it->pMapInfo.GetPtr();
        if (tutorial_map != 0) {
            ++stats.tutorial_maps_loaded;
            ProbeMapInfo(vfs, tutorial_map, &stats);
        } else {
            ++stats.tutorial_maps_missing;
        }
    }
    stats.unique_mission_refs = unique_maps.size();

    report << "; sp_content=probed"
           << "; sp_campaigns=" << stats.campaigns_loaded << "/" << stats.campaign_refs
           << "; sp_campaigns_missing=" << stats.campaigns_missing
           << "; sp_chapters=" << stats.chapters_loaded << "/" << stats.chapter_refs
           << "; sp_chapters_missing=" << stats.chapters_missing
           << "; sp_mission_refs=" << stats.mission_refs
           << "; sp_unique_maps=" << stats.unique_mission_refs
           << "; sp_maps_loaded=" << stats.maps_loaded
           << "; sp_maps_missing=" << stats.maps_missing
           << "; sp_tutorial_maps=" << stats.tutorial_maps_loaded << "/" << stats.tutorial_refs
           << "; sp_tutorial_maps_missing=" << stats.tutorial_maps_missing
           << "; sp_map_data_refs=" << stats.map_data_refs_present << "/" << stats.map_data_refs
           << "; sp_script_refs=" << stats.script_refs_present << "/" << stats.script_refs
           << "; sp_movie_source_refs=" << stats.movie_source_refs_present << "/" << stats.movie_refs
           << "; sp_movie_android_refs=" << stats.movie_android_refs_present << "/" << stats.movie_refs
           << "; sp_objective_refs=" << stats.map_objective_refs
           << "; sp_script_movie_sequences=" << stats.script_movie_sequences;
}

void AppendGameXRuntimeProbe(std::ostringstream& report, bool database_ready) {
#if defined(BK2_LEGACY_GAMEX_RUNTIME_SOURCES_ENABLED)
    report << "; gamex_runtime=linked";
    if (!database_ready) {
        report << "; game_root=database_closed";
        return;
    }

    const NDb::SGameRoot* game_root = NGameX::GetGameRoot();
    const NDb::SGameConsts* game_consts = NGameX::GetGameConsts();
    const NDb::SClientGameConsts* client_consts = NGameX::GetClientConsts();
    const NDb::SSceneConsts* scene_consts = NGameX::GetSceneConsts();
    const NDb::SAIGameConsts* ai_consts = NGameX::GetAIConsts();
    const NDb::SNetGameConsts* net_consts = NGameX::GetNetConsts();
    const NDb::SMultiplayerConsts* mp_consts = NGameX::GetMPConsts();
    const NDb::SUIConstsB2* ui_consts = NGameX::GetUIConsts();

    report << "; game_root=" << Present(game_root != 0)
           << "; game_consts=" << Present(game_consts != 0)
           << "; client_consts=" << Present(client_consts != 0)
           << "; scene_consts=" << Present(scene_consts != 0)
           << "; ai_consts=" << Present(ai_consts != 0)
           << "; net_consts=" << Present(net_consts != 0)
           << "; shared_mp_consts=" << Present(mp_consts != 0)
           << "; ui_consts=" << Present(ui_consts != 0);

    if (game_root != 0) {
        report << "; campaign_refs=" << game_root->campaigns.size()
               << "; tutorial_refs=" << game_root->tutorialMaps.size()
               << "; game_root_movies=" << (game_root->szIntroMovie.empty() ? "missing" : "present");
    }
    AppendSinglePlayerContentProbe(report, NVFS::GetMainVFS(), game_root);

    vector<CDBID> custom_missions;
    vector<CDBID> custom_campaigns;
    NCustom::GetCustomMissions(&custom_missions);
    NCustom::GetCustomCampaigns(&custom_campaigns);
    report << "; custom_missions=" << custom_missions.size()
           << "; custom_campaigns=" << custom_campaigns.size();
#else
    report << "; gamex_runtime=disabled";
#endif
}

}  // namespace

std::string RunStartupProbe() {
    auto& platform = PlatformRuntime::instance();
    const PortPaths paths = GetPortPaths();

    const bool vfs_ready = InitializeLegacyVfs();
    const bool database_ready = InitializeLegacyDatabase();
    NVFS::IVFS* vfs = NVFS::GetMainVFS();

    std::ostringstream report;
    report << "BK2 Android startup probe";
    AppendPathValue(report, "files_dir", paths.files_dir);
    AppendPathValue(report, "external_files_dir", paths.external_files_dir);
    AppendPathValue(report, "data_root", paths.data_root());
    report << "; legacy_vfs=" << (vfs_ready && IsLegacyVfsInitialized() ? "ready" : "failed");

    AppendFileProbe(report, paths.data_root(), "Data/types.xml");
    AppendFileProbe(report, paths.data_root(), "Data/index.bin");
    AppendDirectoryProbe(report, paths.data_root(), "Data/bin/Geometries");
    AppendDirectoryProbe(report, paths.data_root(), "Data/bin/Skeletons");
    AppendDirectoryProbe(report, paths.data_root(), "Data/bin/Animations");
    AppendDirectoryProbe(report, paths.data_root(), "Data/bin/AIGeometries");

    AppendVfsProbe(report, vfs, "types.xml");
    AppendVfsProbe(report, vfs, "index.bin");
    AppendDbTypeProbe(report);
    AppendScriptProbe(report);
    AppendProfileProbe(report, vfs);
    AppendSaveInventoryProbe(report);
    AppendGameXRuntimeProbe(report, database_ready);
    report << "; legacy_database=" << (database_ready ? "open" : "closed");

    const std::string text = report.str();
    platform.log_info(text);
    return text;
}

}  // namespace bk2::android

extern "C" JNIEXPORT jstring JNICALL
Java_com_nival_blitzkrieg2_NativeBridge_runStartupProbe(JNIEnv* env, jclass) {
    const std::string report = bk2::android::RunStartupProbe();
    return env->NewStringUTF(report.c_str());
}
