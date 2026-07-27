#include "bk2_android_sp_catalog.h"

#include "bk2_android_database.h"
#include "bk2_android_vfs.h"
#include "bk2_android_video_bridge.h"
#include "bk2_port_paths.h"

#include "GameX/stdafx.h"
#include "GameX/DBGameRoot.h"
#include "GameX/GetConsts.h"
#include "Stats_B2_M1/DBMapInfo.h"
#include "System/VFSOperations.h"

#include <jni.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <set>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace bk2::android {
namespace {

struct CatalogStats {
    int campaigns = 0;
    int campaigns_missing = 0;
    int chapters = 0;
    int chapters_missing = 0;
    int missions = 0;
    int mission_maps_loaded = 0;
    int mission_maps_missing = 0;
    int tutorials = 0;
    int tutorial_maps_loaded = 0;
    int tutorial_maps_missing = 0;
    int unique_maps = 0;
    int map_data_refs = 0;
    int map_data_refs_present = 0;
    int script_refs = 0;
    int script_refs_present = 0;
    int movie_refs = 0;
    int movie_source_refs_present = 0;
    int movie_android_refs_present = 0;
    int objectives = 0;
    int script_movie_sequences = 0;
};

struct CatalogIssue {
    std::string kind;
    std::string owner;
    std::string ref;
};

std::string JoinPath(const std::string& left, const std::string& right) {
    if (left.empty()) {
        return right;
    }
    if (right.empty()) {
        return left;
    }
    return left[left.size() - 1] == '/' ? left + right : left + "/" + right;
}

bool IsDirectory(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool EnsureDirectory(const std::string& directory) {
    if (directory.empty() || IsDirectory(directory)) {
        return true;
    }

    std::string current;
    size_t position = 0;
    if (directory[0] == '/') {
        current = "/";
        position = 1;
    }

    while (position <= directory.size()) {
        const size_t slash = directory.find('/', position);
        const std::string part = directory.substr(position, slash - position);
        if (!part.empty()) {
            current = current == "/" ? current + part : JoinPath(current, part);
            if (mkdir(current.c_str(), 0775) != 0 && errno != EEXIST) {
                return false;
            }
        }
        if (slash == std::string::npos) {
            break;
        }
        position = slash + 1;
    }
    return true;
}

std::string ToStdString(const string& value) {
    return value.c_str();
}

std::string FileRef(const NFile::CFilePath& value) {
    return value.empty() ? std::string() : std::string(value.c_str());
}

std::string NormalizeLegacyRef(std::string path) {
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

std::string JsonEscape(const std::string& value) {
    std::ostringstream out;
    for (std::string::const_iterator it = value.begin(); it != value.end(); ++it) {
        const unsigned char ch = static_cast<unsigned char>(*it);
        switch (ch) {
            case '\\':
                out << "\\\\";
                break;
            case '"':
                out << "\\\"";
                break;
            case '\n':
                out << "\\n";
                break;
            case '\r':
                out << "\\r";
                break;
            case '\t':
                out << "\\t";
                break;
            default:
                if (ch < 0x20) {
                    static const char* kHex = "0123456789abcdef";
                    out << "\\u00" << kHex[ch >> 4] << kHex[ch & 0x0f];
                } else {
                    out << static_cast<char>(ch);
                }
                break;
        }
    }
    return out.str();
}

void JsonKey(std::ostringstream& out, const char* key) {
    out << "\"" << key << "\":";
}

void JsonString(std::ostringstream& out, const std::string& value) {
    out << "\"" << JsonEscape(value) << "\"";
}

std::string DbIdOf(const NDb::CResource* resource) {
    return resource != nullptr ? ToStdString(resource->GetDBID().ToString()) : std::string();
}

template <class T>
std::string DbIdOfPtr(const CDBPtr<T>& ptr) {
    return DbIdOf(ptr.GetBarePtrNoLoad());
}

bool VfsFileExists(NVFS::IVFS* vfs, const string& ref) {
    if (vfs == nullptr || ref.empty()) {
        return false;
    }
    return vfs->DoesFileExist(ref) || vfs->DoesFileExist(NormalizeLegacyRef(ref.c_str()).c_str());
}

void AddIssue(
        std::vector<CatalogIssue>* issues,
        const std::string& kind,
        const std::string& owner,
        const std::string& ref) {
    if (issues == nullptr) {
        return;
    }
    CatalogIssue issue;
    issue.kind = kind;
    issue.owner = owner;
    issue.ref = ref;
    issues->push_back(issue);
}

void CountFileRef(
        NVFS::IVFS* vfs,
        const NFile::CFilePath& ref,
        int* refs,
        int* present,
        const std::string& owner,
        const char* issue_kind,
        std::vector<CatalogIssue>* issues) {
    if (ref.empty()) {
        return;
    }
    ++(*refs);
    if (VfsFileExists(vfs, ref)) {
        ++(*present);
        return;
    }
    AddIssue(issues, issue_kind, owner, FileRef(ref));
}

void CountMovieRef(
        NVFS::IVFS* vfs,
        const NFile::CFilePath& ref,
        const std::string& owner,
        CatalogStats* stats,
        std::vector<CatalogIssue>* issues) {
    if (ref.empty()) {
        return;
    }
    ++stats->movie_refs;
    if (VfsFileExists(vfs, ref)) {
        ++stats->movie_source_refs_present;
    } else {
        AddIssue(issues, "missing_source_movie", owner, FileRef(ref));
    }

    const std::vector<std::string> android_refs = AndroidVideoRefsForLegacyMovie(ref.c_str());
    bool all_android_refs_present = !android_refs.empty();
    for (std::vector<std::string>::const_iterator it = android_refs.begin(); it != android_refs.end(); ++it) {
        if (!VfsFileExists(vfs, it->c_str())) {
            all_android_refs_present = false;
            AddIssue(issues, "missing_android_movie", owner, *it);
        }
    }
    if (all_android_refs_present) {
        ++stats->movie_android_refs_present;
    } else if (android_refs.empty()) {
        AddIssue(issues, "missing_android_movie", owner, FileRef(ref));
    }
}

void AppendIssuesJson(std::ostringstream& out, const std::vector<CatalogIssue>& issues) {
    out << "[";
    for (size_t i = 0; i < issues.size(); ++i) {
        if (i != 0) {
            out << ",";
        }
        out << "{";
        JsonKey(out, "kind");
        JsonString(out, issues[i].kind);
        out << ",";
        JsonKey(out, "owner");
        JsonString(out, issues[i].owner);
        out << ",";
        JsonKey(out, "ref");
        JsonString(out, issues[i].ref);
        out << "}";
    }
    out << "]";
}

void AppendMapJson(
        std::ostringstream& out,
        NVFS::IVFS* vfs,
        const NDb::SMapInfo* map,
        const std::string& owner,
        bool tutorial,
        CatalogStats* stats,
        std::vector<CatalogIssue>* issues) {
    if (map == nullptr) {
        if (tutorial) {
            ++stats->tutorial_maps_missing;
        } else {
            ++stats->mission_maps_missing;
        }
        AddIssue(issues, "missing_map", owner, "");
        out << "{\"owner\":";
        JsonString(out, owner);
        out << ",\"loaded\":false}";
        return;
    }

    if (tutorial) {
        ++stats->tutorial_maps_loaded;
    } else {
        ++stats->mission_maps_loaded;
    }
    stats->objectives += map->objectives.size();
    stats->script_movie_sequences += map->scriptMovies.scriptMovieSequences.size();

    const int before_map_refs = stats->map_data_refs_present;
    CountFileRef(
            vfs,
            map->szMapDesignerFileRef,
            &stats->map_data_refs,
            &stats->map_data_refs_present,
            owner,
            "missing_map_data",
            issues);
    const bool map_data_present = stats->map_data_refs_present != before_map_refs || map->szMapDesignerFileRef.empty();

    const int before_script_refs = stats->script_refs_present;
    CountFileRef(
            vfs,
            map->szScriptFileRef,
            &stats->script_refs,
            &stats->script_refs_present,
            owner,
            "missing_map_script",
            issues);
    const bool script_present = stats->script_refs_present != before_script_refs || map->szScriptFileRef.empty();

    out << "{";
    JsonKey(out, "owner");
    JsonString(out, owner);
    out << ",";
    JsonKey(out, "dbid");
    JsonString(out, DbIdOf(map));
    out << ",";
    JsonKey(out, "loaded");
    out << "true,";
    JsonKey(out, "map_data_ref");
    JsonString(out, FileRef(map->szMapDesignerFileRef));
    out << ",";
    JsonKey(out, "map_data_present");
    out << (map_data_present ? "true" : "false") << ",";
    JsonKey(out, "script_ref");
    JsonString(out, FileRef(map->szScriptFileRef));
    out << ",";
    JsonKey(out, "script_present");
    out << (script_present ? "true" : "false") << ",";
    JsonKey(out, "objectives");
    out << map->objectives.size() << ",";
    JsonKey(out, "script_movie_sequences");
    out << map->scriptMovies.scriptMovieSequences.size();
    out << "}";
}

std::string WriteCatalogReport(const std::string& json) {
    const std::string root = GetPortPaths().log_root();
    if (root.empty()) {
        return "";
    }
    EnsureDirectory(root);
    const std::string path = JoinPath(root, "single_player_catalog_probe.json");
    FILE* file = fopen(path.c_str(), "wb");
    if (file == nullptr) {
        return "";
    }
    const size_t written = fwrite(json.data(), 1, json.size(), file);
    const int closed = fclose(file);
    return written == json.size() && closed == 0 ? path : std::string();
}

}  // namespace

std::string RunSinglePlayerCatalogProbe() {
    if (!InitializeLegacyVfs()) {
        return "sp_catalog=failed; sp_catalog_error=legacy_vfs_failed";
    }
    if (!InitializeLegacyDatabase()) {
        return "sp_catalog=failed; sp_catalog_error=legacy_database_closed";
    }

    const NDb::SGameRoot* game_root = NGameX::GetGameRoot();
    if (game_root == nullptr) {
        return "sp_catalog=failed; sp_catalog_error=game_root_missing";
    }

    NVFS::IVFS* vfs = NVFS::GetMainVFS();
    CatalogStats stats;
    std::vector<CatalogIssue> issues;
    std::set<std::string> unique_maps;

    std::ostringstream json;
    json << "{";
    CountMovieRef(vfs, game_root->szIntroMovie, "game_root:intro", &stats, &issues);
    JsonKey(json, "game_root_intro_movie_ref");
    JsonString(json, FileRef(game_root->szIntroMovie));
    json << ",";
    JsonKey(json, "campaigns");
    json << "[";

    stats.campaigns = game_root->campaigns.size();
    for (int campaign_index = 0; campaign_index < game_root->campaigns.size(); ++campaign_index) {
        if (campaign_index != 0) {
            json << ",";
        }
        const NDb::SCampaign* campaign = game_root->campaigns[campaign_index].GetPtr();
        const std::string campaign_owner = "campaign:" + std::to_string(campaign_index);
        json << "{";
        JsonKey(json, "index");
        json << campaign_index << ",";
        JsonKey(json, "dbid");
        JsonString(json, DbIdOf(campaign));
        json << ",";
        JsonKey(json, "loaded");
        json << (campaign != nullptr ? "true" : "false") << ",";
        JsonKey(json, "chapters");
        json << "[";
        if (campaign == nullptr) {
            ++stats.campaigns_missing;
            AddIssue(&issues, "missing_campaign", campaign_owner, "");
        } else {
            CountFileRef(
                    vfs,
                    campaign->szScriptFileRef,
                    &stats.script_refs,
                    &stats.script_refs_present,
                    campaign_owner,
                    "missing_campaign_script",
                    &issues);
            CountMovieRef(vfs, campaign->szIntroMovie, campaign_owner + ":intro", &stats, &issues);
            CountMovieRef(vfs, campaign->szOutroMovie, campaign_owner + ":outro", &stats, &issues);

            stats.chapters += campaign->chapters.size();
            for (int chapter_index = 0; chapter_index < campaign->chapters.size(); ++chapter_index) {
                if (chapter_index != 0) {
                    json << ",";
                }
                const NDb::SChapter* chapter = campaign->chapters[chapter_index].GetPtr();
                const std::string chapter_owner = campaign_owner + "/chapter:" + std::to_string(chapter_index);
                json << "{";
                JsonKey(json, "index");
                json << chapter_index << ",";
                JsonKey(json, "dbid");
                JsonString(json, DbIdOf(chapter));
                json << ",";
                JsonKey(json, "loaded");
                json << (chapter != nullptr ? "true" : "false") << ",";
                JsonKey(json, "missions");
                json << "[";
                if (chapter == nullptr) {
                    ++stats.chapters_missing;
                    AddIssue(&issues, "missing_chapter", chapter_owner, "");
                } else {
                    CountFileRef(
                            vfs,
                            chapter->szScriptFileRef,
                            &stats.script_refs,
                            &stats.script_refs_present,
                            chapter_owner,
                            "missing_chapter_script",
                            &issues);
                    CountMovieRef(vfs, chapter->szIntroMovie, chapter_owner + ":intro", &stats, &issues);

                    stats.missions += chapter->missionPath.size();
                    for (int mission_index = 0; mission_index < chapter->missionPath.size(); ++mission_index) {
                        if (mission_index != 0) {
                            json << ",";
                        }
                        const NDb::SMissionEnableInfo& mission = chapter->missionPath[mission_index];
                        const NDb::SMapInfo* map = mission.pMap.GetPtr();
                        const std::string mission_owner = chapter_owner + "/mission:" + std::to_string(mission_index);
                        const std::string map_id = DbIdOfPtr(mission.pMap);
                        if (!map_id.empty()) {
                            unique_maps.insert(map_id);
                        }
                        json << "{";
                        JsonKey(json, "index");
                        json << mission_index << ",";
                        JsonKey(json, "map");
                        AppendMapJson(json, vfs, map, mission_owner, false, &stats, &issues);
                        json << "}";
                    }
                }
                json << "]}";
            }
        }
        json << "]}";
    }
    json << "],";

    JsonKey(json, "tutorials");
    json << "[";
    stats.tutorials = game_root->tutorialMaps.size();
    for (int tutorial_index = 0; tutorial_index < game_root->tutorialMaps.size(); ++tutorial_index) {
        if (tutorial_index != 0) {
            json << ",";
        }
        const NDb::SGameRoot::STutorialMap& tutorial = game_root->tutorialMaps[tutorial_index];
        const std::string owner = "tutorial:" + std::to_string(tutorial_index);
        const std::string map_id = DbIdOfPtr(tutorial.pMapInfo);
        if (!map_id.empty()) {
            unique_maps.insert(map_id);
        }
        CountFileRef(
                vfs,
                tutorial.szDifficultyFileRef,
                &stats.script_refs,
                &stats.script_refs_present,
                owner,
                "missing_tutorial_difficulty",
                &issues);
        json << "{";
        JsonKey(json, "index");
        json << tutorial_index << ",";
        JsonKey(json, "difficulty_ref");
        JsonString(json, FileRef(tutorial.szDifficultyFileRef));
        json << ",";
        JsonKey(json, "map");
        AppendMapJson(json, vfs, tutorial.pMapInfo.GetPtr(), owner, true, &stats, &issues);
        json << "}";
    }
    json << "],";

    stats.unique_maps = unique_maps.size();
    JsonKey(json, "stats");
    json << "{";
    JsonKey(json, "campaigns");
    json << stats.campaigns << ",";
    JsonKey(json, "campaigns_missing");
    json << stats.campaigns_missing << ",";
    JsonKey(json, "chapters");
    json << stats.chapters << ",";
    JsonKey(json, "chapters_missing");
    json << stats.chapters_missing << ",";
    JsonKey(json, "missions");
    json << stats.missions << ",";
    JsonKey(json, "mission_maps_loaded");
    json << stats.mission_maps_loaded << ",";
    JsonKey(json, "mission_maps_missing");
    json << stats.mission_maps_missing << ",";
    JsonKey(json, "tutorials");
    json << stats.tutorials << ",";
    JsonKey(json, "tutorial_maps_loaded");
    json << stats.tutorial_maps_loaded << ",";
    JsonKey(json, "tutorial_maps_missing");
    json << stats.tutorial_maps_missing << ",";
    JsonKey(json, "unique_maps");
    json << stats.unique_maps << ",";
    JsonKey(json, "map_data_refs_present");
    json << stats.map_data_refs_present << ",";
    JsonKey(json, "map_data_refs");
    json << stats.map_data_refs << ",";
    JsonKey(json, "script_refs_present");
    json << stats.script_refs_present << ",";
    JsonKey(json, "script_refs");
    json << stats.script_refs << ",";
    JsonKey(json, "movie_source_refs_present");
    json << stats.movie_source_refs_present << ",";
    JsonKey(json, "movie_refs");
    json << stats.movie_refs << ",";
    JsonKey(json, "movie_android_refs_present");
    json << stats.movie_android_refs_present << ",";
    JsonKey(json, "objectives");
    json << stats.objectives << ",";
    JsonKey(json, "script_movie_sequences");
    json << stats.script_movie_sequences << ",";
    JsonKey(json, "issues");
    json << issues.size();
    json << "},";
    JsonKey(json, "issues");
    AppendIssuesJson(json, issues);
    json << "}";

    const std::string report_path = WriteCatalogReport(json.str());
    std::ostringstream summary;
    summary << "sp_catalog=probed"
            << "; sp_catalog_path=" << (report_path.empty() ? "<write_failed>" : report_path)
            << "; sp_catalog_campaigns=" << (stats.campaigns - stats.campaigns_missing) << "/" << stats.campaigns
            << "; sp_catalog_chapters=" << (stats.chapters - stats.chapters_missing) << "/" << stats.chapters
            << "; sp_catalog_missions=" << stats.missions
            << "; sp_catalog_mission_maps=" << stats.mission_maps_loaded << "/" << stats.missions
            << "; sp_catalog_tutorial_maps=" << stats.tutorial_maps_loaded << "/" << stats.tutorials
            << "; sp_catalog_unique_maps=" << stats.unique_maps
            << "; sp_catalog_map_data_refs=" << stats.map_data_refs_present << "/" << stats.map_data_refs
            << "; sp_catalog_script_refs=" << stats.script_refs_present << "/" << stats.script_refs
            << "; sp_catalog_movie_source_refs=" << stats.movie_source_refs_present << "/" << stats.movie_refs
            << "; sp_catalog_movie_android_refs=" << stats.movie_android_refs_present << "/" << stats.movie_refs
            << "; sp_catalog_objectives=" << stats.objectives
            << "; sp_catalog_script_movies=" << stats.script_movie_sequences
            << "; sp_catalog_issues=" << issues.size();
    return summary.str();
}

}  // namespace bk2::android

extern "C" JNIEXPORT jstring JNICALL
Java_com_nival_blitzkrieg2_NativeBridge_runSinglePlayerCatalogProbe(JNIEnv* env, jclass) {
    const std::string text = bk2::android::RunSinglePlayerCatalogProbe();
    return env->NewStringUTF(text.c_str());
}
