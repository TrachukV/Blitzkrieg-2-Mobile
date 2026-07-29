#include "bk2_android_single_player_runtime.h"

#include "bk2_android_audio_backend.h"
#include "bk2_android_audio_output.h"
#include "bk2_android_database.h"
#include "bk2_android_legacy_game_runtime.h"
#include "bk2_android_mission_runtime.h"
#include "bk2_android_platform.h"
#include "bk2_legacy_texture_probe.h"
#include "bk2_presentation_internal.h"
#include "bk2_port_paths.h"
#include "bk2_render_backend.h"

#include "SceneB2/stdafx.h"
#include "3Dmotor/DBScene.h"
#include "3Dmotor/GTexture.h"
#include "SceneB2/TerrainInfo.h"
#include "Stats_B2_M1/DBMapInfo.h"
#include "Stats_B2_M1/DBVisObj.h"
#include "Stats_B2_M1/UserActions.h"
#include "Stats_B2_M1/Vis2AI.h"
#include "System/BinSaver.h"
#include "System/VFSOperations.h"
#include "libdb/Db.h"
#include "libdb/Database.h"

#include <algorithm>
#include <cmath>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <jni.h>
#include <limits>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace bk2::android {
namespace {

constexpr float kPackedHeightScale = 0.01f;
constexpr float kMinCameraDistance = 24.0f;
constexpr float kInitialCameraMinDistance = 72.0f;
constexpr float kCameraMaxTerrainFraction = 0.48f;
constexpr const char* kInfantryTraceTexture =
        "Scene/TexAndMats/All/Units/Weapons/GunShotTraceBlue_Texture.dds";
constexpr const char* kMechanizedTraceTexture =
        "Scene/TexAndMats/All/Units/Weapons/GunShotTraceOrange_texture.dds";
constexpr const char* kMuzzleFlashTexture =
        "Scene/TexAndMats/All/Effects/Shots/CannonShot/Shot8_Texture.dds";
constexpr const char* kDestructionFireTextures[] = {
        "Scene/TexAndMats/All/Effects/Destructions/Fire/Fire2_Texture.dds",
        "Scene/TexAndMats/All/Effects/Destructions/Fire/Fire3_Texture.dds",
        "Scene/TexAndMats/All/Effects/Destructions/Fire/Fire4_Texture.dds",
        "Scene/TexAndMats/All/Effects/Destructions/Fire/Fire5_Texture.dds",
};
constexpr const char* kDestructionSmokeTextures[] = {
        "Scene/TexAndMats/All/Effects/Explosions/GroundExplosion/Explosion2_Texture.dds",
        "Scene/TexAndMats/All/Effects/Explosions/GroundExplosion/Explosion3_Texture.dds",
};

std::mutex g_runtime_mutex;
bool g_ready = false;
bool g_user_paused = false;
std::string g_last_error;
std::string g_mission_id;
std::string g_map_path;
int g_height_width = 0;
int g_height_height = 0;
size_t g_triangle_count = 0;
size_t g_map_object_count = 0;
size_t g_scenario_object_count = 0;
size_t g_rendered_object_count = 0;
size_t g_dynamic_rendered_object_count = 0;
bool g_presentation_snapshot_written = false;
uint64_t g_rendered_presentation_generation = 0;
uint64_t g_rendered_war_fog_generation = 0;
float g_animation_elapsed_seconds = 0.0f;
TerrainCamera g_camera;
TerrainMesh g_terrain_mesh;
WorldObjectMesh g_static_world_object_mesh;
WorldObjectMesh g_world_object_mesh;
CObj<NGfx::CTexture> g_terrain_texture;
std::string g_terrain_texture_path;
std::vector<CObj<NGfx::CTexture> > g_terrain_layer_textures;
std::vector<std::string> g_terrain_layer_texture_paths;
size_t g_terrain_layer_count = 0;
size_t g_terrain_layer_texture_count = 0;
std::vector<int> g_terrain_type_map;
std::vector<uint32_t> g_terrain_type_colors;
int g_terrain_type_width = 0;
int g_terrain_type_height = 0;
size_t g_converted_geometry_instance_count = 0;
size_t g_converted_geometry_fallback_count = 0;
size_t g_animated_geometry_part_count = 0;
size_t g_move_animation_instance_count = 0;
size_t g_attack_animation_instance_count = 0;
size_t g_death_animation_instance_count = 0;
size_t g_lying_idle_animation_instance_count = 0;
size_t g_lying_move_animation_instance_count = 0;
size_t g_lying_attack_animation_instance_count = 0;
size_t g_combat_effect_render_count = 0;
size_t g_active_combat_effect_count = 0;
size_t g_active_destruction_effect_count = 0;
size_t g_active_unit_indicator_count = 0;
bool g_combat_effect_trace_texture_logged = false;
bool g_muzzle_flash_texture_logged = false;
bool g_destruction_effect_texture_logged = false;

enum class TouchCommandMode {
    Contextual = 0,
    Move = 1,
    Attack = 2,
    Rotate = 3,
    Spyglass = 4,
    ClearMines = 5,
    PlaceMines = 6,
    BuildTrenches = 7,
};

TouchCommandMode g_touch_command_mode = TouchCommandMode::Contextual;
bool g_trench_first_point_defined = false;
float g_trench_first_world_x = 0.0f;
float g_trench_first_world_y = 0.0f;
int g_trench_first_unit_id = -1;
uint64_t g_last_friendly_tap_millis = 0;
int g_last_friendly_tap_unit_id = -1;
float g_last_friendly_tap_x = 0.0f;
float g_last_friendly_tap_y = 0.0f;

void ResetPendingTrenchCommandLocked() {
    g_trench_first_point_defined = false;
    g_trench_first_world_x = 0.0f;
    g_trench_first_world_y = 0.0f;
    g_trench_first_unit_id = -1;
}

void ResetFriendlyDoubleTapLocked() {
    g_last_friendly_tap_millis = 0;
    g_last_friendly_tap_unit_id = -1;
    g_last_friendly_tap_x = 0.0f;
    g_last_friendly_tap_y = 0.0f;
}

enum class ConvertedAnimationVariant {
    Base,
    Move,
    Attack,
    Death,
    LyingIdle,
    LyingMove,
    LyingAttack,
};

struct ConvertedGeometryVertex {
    float x;
    float y;
    float z;
    float nx;
    float ny;
    float nz;
    float u;
    float v;
};

struct ConvertedGeometryGroup {
    uint32_t material_index = 0;
    uint32_t first_index = 0;
    uint32_t index_count = 0;
};

struct ConvertedGeometryPart {
    std::vector<ConvertedGeometryVertex> vertices;
    std::vector<std::vector<ConvertedGeometryVertex> > animation_frames;
    float animation_duration_seconds = 0.0f;
    std::vector<uint32_t> triangle_indices;
    std::vector<ConvertedGeometryGroup> groups;
};

struct ConvertedGeometry {
    std::vector<ConvertedGeometryPart> parts;
};

struct GeometryBinding {
    int geometry_record_id = -1;
    std::vector<int> material_quantities;
    std::vector<std::string> texture_paths;
    float geometry_scale = 1.0f;
};

std::unordered_map<int, ConvertedGeometry> g_converted_geometries;
std::unordered_set<int> g_missing_converted_geometries;
std::unordered_map<int, ConvertedGeometry> g_move_converted_geometries;
std::unordered_set<int> g_missing_move_converted_geometries;
std::unordered_map<int, ConvertedGeometry> g_attack_converted_geometries;
std::unordered_set<int> g_missing_attack_converted_geometries;
std::unordered_map<int, ConvertedGeometry> g_death_converted_geometries;
std::unordered_set<int> g_missing_death_converted_geometries;
std::unordered_map<int, ConvertedGeometry> g_lying_idle_converted_geometries;
std::unordered_set<int> g_missing_lying_idle_converted_geometries;
std::unordered_map<int, ConvertedGeometry> g_lying_move_converted_geometries;
std::unordered_set<int> g_missing_lying_move_converted_geometries;
std::unordered_map<int, ConvertedGeometry> g_lying_attack_converted_geometries;
std::unordered_set<int> g_missing_lying_attack_converted_geometries;
std::unordered_map<int32_t, float> g_death_animation_start_seconds;
std::unordered_map<uint64_t, GeometryBinding> g_stats_geometry_index;
std::unordered_map<
        uint64_t,
        std::unordered_map<int, GeometryBinding> > g_stats_geometry_variants;
bool g_stats_geometry_index_loaded = false;
std::unordered_map<std::string, CObj<NGfx::CTexture> > g_model_textures;
size_t g_model_texture_count = 0;
std::unordered_map<std::string, size_t> g_static_fallback_stats_paths;
std::unordered_map<uint64_t, size_t> g_dynamic_fallback_stats_hashes;

void RefreshWorldObjectTextureHandles(WorldObjectMesh* mesh);
float TerrainMeshHeightAtLocked(float world_x, float world_y);

struct MissionLaunchOverride {
    bool present = false;
    bool tutorial = false;
    bool continue_campaign = false;
    int campaign_index = -1;
    int chapter_index = -1;
    int mission_index = -1;
    int tutorial_index = -1;
    int difficulty = 0;
    std::string mission_id;
    std::string error;
};

std::string ToStdString(const string& value) {
    return value.c_str();
}

std::string JoinHostPath(const std::string& left, const std::string& right) {
    if (left.empty()) {
        return right;
    }
    if (right.empty()) {
        return left;
    }
    return left[left.size() - 1] == '/' ? left + right : left + "/" + right;
}

std::string TrimAscii(std::string value) {
    while (!value.empty() &&
           (value[0] == ' ' || value[0] == '\t' ||
            value[0] == '\r' || value[0] == '\n')) {
        value.erase(value.begin());
    }
    while (!value.empty()) {
        const char ch = value[value.size() - 1];
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
            break;
        }
        value.resize(value.size() - 1);
    }
    return value;
}

bool ParseIntValue(const std::string& value, int* out) {
    if (out == nullptr || value.empty()) {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (errno != 0 || end == value.c_str() || *end != 0) {
        return false;
    }
    *out = static_cast<int>(parsed);
    return true;
}

MissionLaunchOverride ReadMissionLaunchOverride() {
    MissionLaunchOverride result;
    const PortPaths paths = GetPortPaths();
    if (paths.files_dir.empty() && paths.external_files_dir.empty()) {
        return result;
    }

    std::vector<std::string> candidate_paths;
    if (!paths.external_files_dir.empty()) {
        candidate_paths.push_back(
                JoinHostPath(paths.external_files_dir, "selected_mission.txt"));
    }
    if (!paths.files_dir.empty()) {
        candidate_paths.push_back(
                JoinHostPath(paths.files_dir, "selected_mission.txt"));
    }

    std::ifstream file;
    for (std::vector<std::string>::const_iterator it = candidate_paths.begin();
         it != candidate_paths.end();
         ++it) {
        file.open(it->c_str());
        if (file.is_open()) {
            break;
        }
        file.clear();
    }
    if (!file.is_open()) {
        return result;
    }

    result.present = true;
    std::string line;
    int line_number = 0;
    while (std::getline(file, line)) {
        ++line_number;
        const size_t comment = line.find('#');
        if (comment != std::string::npos) {
            line.resize(comment);
        }
        line = TrimAscii(line);
        if (line.empty()) {
            continue;
        }
        const size_t equals = line.find('=');
        if (equals == std::string::npos) {
            result.error = "selected_mission_parse_error:" + std::to_string(line_number);
            return result;
        }
        const std::string key = TrimAscii(line.substr(0, equals));
        const std::string value = TrimAscii(line.substr(equals + 1));
        if (key == "mission_id") {
            result.mission_id = value;
            continue;
        }
        int parsed = 0;
        if (!ParseIntValue(value, &parsed)) {
            result.error = "selected_mission_bad_int:" + key;
            return result;
        }
        if (key == "campaign") {
            result.campaign_index = parsed;
        } else if (key == "chapter") {
            result.chapter_index = parsed;
        } else if (key == "mission") {
            result.mission_index = parsed;
        } else if (key == "continue") {
            result.continue_campaign = parsed != 0;
        } else if (key == "tutorial") {
            result.tutorial = true;
            result.tutorial_index = parsed;
        } else if (key == "difficulty") {
            result.difficulty = parsed;
        } else {
            result.error = "selected_mission_unknown_key:" + key;
            return result;
        }
    }
    return result;
}

MissionRuntimeResult StartConfiguredMissionState() {
    MissionLaunchOverride launch = ReadMissionLaunchOverride();
    if (!launch.error.empty()) {
        MissionRuntimeResult result;
        result.error = launch.error;
        return result;
    }
    if (!launch.present) {
        return StartFirstCampaignMissionState();
    }
    if (launch.continue_campaign) {
        MissionRuntimeResult result =
                LoadMissionRuntimeCheckpoint("android_autosave");
        if (!result.ok) {
            return result;
        }
        if (result.state.mission_active) {
            result.ok = false;
            result.error = "autosave_contains_active_mission";
            return result;
        }
        if (result.state.campaign_finished) {
            result.ok = false;
            result.error = "autosave_campaign_finished";
            return result;
        }
        if (result.state.chapter_finished) {
            result = AdvanceToNextChapter();
            if (!result.ok) {
                return result;
            }
        }
        return StartFirstEnabledCampaignMissionState(launch.difficulty);
    }
    if (!launch.mission_id.empty()) {
        return StartDirectMissionState(launch.mission_id, launch.difficulty);
    }
    if (launch.tutorial) {
        return StartTutorialMissionState(launch.tutorial_index, launch.difficulty);
    }
    if (launch.campaign_index >= 0 &&
        launch.chapter_index < 0 &&
        launch.mission_index < 0) {
        return StartFirstCampaignMissionState(
                launch.campaign_index,
                launch.difficulty);
    }
    return StartCampaignMissionState(
            launch.campaign_index,
            launch.chapter_index,
            launch.mission_index,
            launch.difficulty);
}

std::string MapBinaryPath(const NDb::SMapInfo* map) {
    if (map == nullptr) {
        return std::string();
    }
    std::string folder = ToStdString(NDb::GetFolderName(map->GetDBID()));
    std::replace(folder.begin(), folder.end(), '\\', '/');
    while (!folder.empty() && folder[folder.size() - 1] == '/') {
        folder.resize(folder.size() - 1);
    }
    return folder + "/map.b2m";
}

bool ReadTerrainInfo(
        const NDb::SMapInfo* map,
        STerrainInfo* terrain_info,
        std::string* error) {
    if (map == nullptr || terrain_info == nullptr) {
        *error = "mission_map_missing";
        return false;
    }

    const std::string map_path = MapBinaryPath(map);
    CFileStream stream(NVFS::GetMainVFS(), map_path.c_str());
    if (!stream.IsOk()) {
        *error = std::string("map_binary_open_failed:") + map_path;
        return false;
    }

    CPtr<IBinSaver> saver = CreateBinSaver(&stream, SAVER_MODE_READ);
    if (!saver) {
        *error = std::string("map_binary_saver_failed:") + map_path;
        return false;
    }
    saver->Add(1, terrain_info);
    g_map_path = map_path;
    return true;
}

int HeightWidth(const STerrainInfo& info) {
    return info.heights.GetSizeX() > 0
            ? info.heights.GetSizeX()
            : info.optimizedHeights.GetSizeX();
}

int HeightHeight(const STerrainInfo& info) {
    return info.heights.GetSizeY() > 0
            ? info.heights.GetSizeY()
            : info.optimizedHeights.GetSizeY();
}

float BaseHeight(const STerrainInfo& info, int x, int y) {
    if (info.heights.GetSizeX() > 0 && info.heights.GetSizeY() > 0) {
        return info.heights[y][x];
    }
    return static_cast<float>(info.optimizedHeights[y][x]) *
           kPackedHeightScale;
}

float AddedHeight(const STerrainInfo& info, int x, int y) {
    if (info.addHeights.GetSizeX() == HeightWidth(info) &&
        info.addHeights.GetSizeY() == HeightHeight(info)) {
        return info.addHeights[y][x];
    }
    if (info.optimizedAddHeights.GetSizeX() == HeightWidth(info) &&
        info.optimizedAddHeights.GetSizeY() == HeightHeight(info)) {
        return static_cast<float>(info.optimizedAddHeights[y][x]) *
               kPackedHeightScale;
    }
    return 0.0f;
}

bool BuildTerrainMesh(
        const STerrainInfo& info,
        TerrainMesh* mesh,
        std::string* error) {
    const int width = HeightWidth(info);
    const int height = HeightHeight(info);
    if (width < 2 || height < 2) {
        *error = "terrain_heightfield_missing";
        return false;
    }
    if (info.optimizedHeights.GetSizeX() > 0 &&
        (info.optimizedHeights.GetSizeX() != width ||
         info.optimizedHeights.GetSizeY() != height)) {
        *error = "terrain_heightfield_dimensions_invalid";
        return false;
    }

    mesh->vertices.reserve(static_cast<size_t>(width) * height);
    float min_height = std::numeric_limits<float>::max();
    float max_height = std::numeric_limits<float>::lowest();
    double height_sum = 0.0;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float z = BaseHeight(info, x, y) + AddedHeight(info, x, y);
            mesh->vertices.push_back(
                    TerrainVertex{
                            static_cast<float>(x) * VIS_TILE_SIZE,
                            static_cast<float>(y) * VIS_TILE_SIZE,
                            z,
                            static_cast<float>(x) /
                                    static_cast<float>(DEF_PATCH_SIZE),
                            static_cast<float>(y) /
                                    static_cast<float>(DEF_PATCH_SIZE),
                            0xffffffffu});
            min_height = std::min(min_height, z);
            max_height = std::max(max_height, z);
            height_sum += z;
        }
    }

    const size_t tile_count =
            static_cast<size_t>(width - 1) * static_cast<size_t>(height - 1);
    mesh->triangle_indices.reserve(tile_count * 6);
    for (int y = 0; y < height - 1; ++y) {
        for (int x = 0; x < width - 1; ++x) {
            const uint32_t top_left = static_cast<uint32_t>(y * width + x);
            const uint32_t top_right = top_left + 1;
            const uint32_t bottom_left = top_left + static_cast<uint32_t>(width);
            const uint32_t bottom_right = bottom_left + 1;
            mesh->triangle_indices.push_back(top_left);
            mesh->triangle_indices.push_back(bottom_left);
            mesh->triangle_indices.push_back(top_right);
            mesh->triangle_indices.push_back(top_right);
            mesh->triangle_indices.push_back(bottom_left);
            mesh->triangle_indices.push_back(bottom_right);
        }
    }

    mesh->line_indices.reserve(
            static_cast<size_t>(height) * (width - 1) * 2 +
            static_cast<size_t>(width) * (height - 1) * 2);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width - 1; ++x) {
            mesh->line_indices.push_back(static_cast<uint32_t>(y * width + x));
            mesh->line_indices.push_back(
                    static_cast<uint32_t>(y * width + x + 1));
        }
    }
    for (int x = 0; x < width; ++x) {
        for (int y = 0; y < height - 1; ++y) {
            mesh->line_indices.push_back(static_cast<uint32_t>(y * width + x));
            mesh->line_indices.push_back(
                    static_cast<uint32_t>((y + 1) * width + x));
        }
    }

    mesh->center_x = static_cast<float>(width - 1) * VIS_TILE_SIZE * 0.5f;
    mesh->center_y = static_cast<float>(height - 1) * VIS_TILE_SIZE * 0.5f;
    mesh->center_z =
            static_cast<float>(height_sum / static_cast<double>(mesh->vertices.size()));
    mesh->world_size = std::max(
            static_cast<float>(width - 1) * VIS_TILE_SIZE,
            static_cast<float>(height - 1) * VIS_TILE_SIZE);

    g_height_width = width;
    g_height_height = height;
    g_triangle_count = tile_count * 2;
    return true;
}

bool BuildTerrainLayers(
        const NDb::SMapInfo* map,
        const STerrainInfo& info,
        TerrainMesh* mesh) {
    if (map == nullptr || mesh == nullptr || !map->pTerraSet) {
        return false;
    }
    const NDb::STGTerraSet* terra_set = map->pTerraSet.GetPtr();
    if (terra_set == nullptr || terra_set->terraTypes.empty() ||
        info.tileTerraMap.GetSizeX() <= 0 ||
        info.tileTerraMap.GetSizeY() <= 0) {
        return false;
    }

    struct OrderedTerrainType {
        int terrain_type_index = -1;
        int priority = 0;
    };
    std::vector<OrderedTerrainType> ordered_types;
    ordered_types.reserve(terra_set->terraTypes.size());
    g_terrain_type_colors.resize(terra_set->terraTypes.size(), 0xff42583du);
    for (size_t index = 0; index < terra_set->terraTypes.size(); ++index) {
        const NDb::STGTerraType* type = terra_set->terraTypes[index].GetPtr();
        int priority = 0;
        if (type != nullptr) {
            g_terrain_type_colors[index] =
                    static_cast<uint32_t>(type->nColor);
            if (type->pMaterial) {
                priority = type->pMaterial->nPriority;
            }
        }
        ordered_types.push_back(
                OrderedTerrainType{static_cast<int>(index), priority});
    }
    std::stable_sort(
            ordered_types.begin(),
            ordered_types.end(),
            [](const OrderedTerrainType& left,
               const OrderedTerrainType& right) {
                return left.priority < right.priority;
            });

    const int width = HeightWidth(info);
    const int height = HeightHeight(info);
    const int mask_width = info.tileTerraMap.GetSizeX();
    const int mask_height = info.tileTerraMap.GetSizeY();
    if (mask_width < 2 || mask_height < 2) {
        return false;
    }
    g_terrain_type_width = width - 1;
    g_terrain_type_height = height - 1;
    g_terrain_type_map.assign(
            static_cast<size_t>(g_terrain_type_width) *
                    g_terrain_type_height,
            -1);
    for (int y = 0; y < height - 1; ++y) {
        for (int x = 0; x < width - 1; ++x) {
            const int map_x = std::min(x, mask_width - 1);
            const int map_y = std::min(y, mask_height - 1);
            const int type_index = info.tileTerraMap[map_y][map_x];
            g_terrain_type_map[
                    static_cast<size_t>(y * g_terrain_type_width + x)] =
                    type_index;
        }
    }

    const size_t mask_size =
            static_cast<size_t>(mask_width) * mask_height;
    std::vector<std::vector<uint8_t> > masks(
            terra_set->terraTypes.size(),
            std::vector<uint8_t>(mask_size, 0));
    constexpr int kBlurWeights[3][3] = {
            {1, 2, 1},
            {2, 4, 2},
            {1, 2, 1}};
    for (const OrderedTerrainType& ordered : ordered_types) {
        std::vector<uint8_t>& mask = masks[ordered.terrain_type_index];
        for (int y = 0; y < mask_height; ++y) {
            for (int x = 0; x < mask_width; ++x) {
                int weighted_value = 0;
                int weight_sum = 0;
                for (int offset_y = -1; offset_y <= 1; ++offset_y) {
                    const int source_y = y + offset_y;
                    if (source_y < 0 || source_y >= mask_height) {
                        continue;
                    }
                    for (int offset_x = -1; offset_x <= 1; ++offset_x) {
                        const int source_x = x + offset_x;
                        if (source_x < 0 || source_x >= mask_width) {
                            continue;
                        }
                        const int weight =
                                kBlurWeights[offset_y + 1][offset_x + 1];
                        weight_sum += weight;
                        if (info.tileTerraMap[source_y][source_x] ==
                            ordered.terrain_type_index) {
                            weighted_value += weight * 255;
                        }
                    }
                }
                mask[static_cast<size_t>(y * mask_width + x)] =
                        static_cast<uint8_t>(
                                weight_sum > 0
                                        ? weighted_value / weight_sum
                                        : 0);
            }
        }
    }

    mesh->layers.clear();
    mesh->layers.resize(ordered_types.size());
    std::vector<int> type_to_layer(ordered_types.size(), -1);
    std::vector<std::vector<int> > shared_to_local(
            ordered_types.size(),
            std::vector<int>(mesh->vertices.size(), -1));
    for (size_t layer_index = 0;
         layer_index < ordered_types.size();
         ++layer_index) {
        TerrainLayer& layer = mesh->layers[layer_index];
        layer.terrain_type_index =
                ordered_types[layer_index].terrain_type_index;
        type_to_layer[layer.terrain_type_index] =
                static_cast<int>(layer_index);
        const NDb::STGTerraType* type =
                terra_set->terraTypes[layer.terrain_type_index].GetPtr();
        if (type != nullptr) {
            layer.fallback_argb = static_cast<uint32_t>(type->nColor);
        }
    }

    auto add_layer_vertex =
            [&](int layer_index,
                uint32_t shared_index,
                uint8_t alpha) -> uint32_t {
        std::vector<int>& remap = shared_to_local[layer_index];
        int& local_index = remap[shared_index];
        TerrainLayer& layer = mesh->layers[layer_index];
        if (local_index < 0) {
            TerrainVertex vertex = mesh->vertices[shared_index];
            const NDb::STGTerraType* type =
                    terra_set
                            ->terraTypes[layer.terrain_type_index]
                            .GetPtr();
            const float texture_scale =
                    type != nullptr ? type->fScaleCoeff : 1.0f;
            vertex.u *= texture_scale;
            vertex.v *= texture_scale;
            vertex.abgr =
                    (static_cast<uint32_t>(alpha) << 24) | 0x00ffffffu;
            local_index = static_cast<int>(layer.vertices.size());
            layer.vertices.push_back(vertex);
        } else {
            TerrainVertex& vertex = layer.vertices[local_index];
            const uint8_t old_alpha =
                    static_cast<uint8_t>(vertex.abgr >> 24);
            if (alpha > old_alpha) {
                vertex.abgr =
                        (static_cast<uint32_t>(alpha) << 24) |
                        (vertex.abgr & 0x00ffffffu);
            }
        }
        return static_cast<uint32_t>(local_index);
    };

    for (int y = 0; y < height - 1; ++y) {
        for (int x = 0; x < width - 1; ++x) {
            const int mask_x = std::min(x, mask_width - 2);
            const int mask_y = std::min(y, mask_height - 2);
            const size_t top_left_mask =
                    static_cast<size_t>(mask_y * mask_width + mask_x);
            const size_t top_right_mask = top_left_mask + 1;
            const size_t bottom_left_mask =
                    top_left_mask + static_cast<size_t>(mask_width);
            const size_t bottom_right_mask = bottom_left_mask + 1;
            std::vector<int> present_types;
            for (auto ordered = ordered_types.rbegin();
                 ordered != ordered_types.rend();
                 ++ordered) {
                const std::vector<uint8_t>& mask =
                        masks[ordered->terrain_type_index];
                if (mask[top_left_mask] > 0 ||
                    mask[top_right_mask] > 0 ||
                    mask[bottom_left_mask] > 0 ||
                    mask[bottom_right_mask] > 0) {
                    present_types.push_back(
                            ordered->terrain_type_index);
                }
            }
            if (present_types.empty()) {
                continue;
            }

            const uint32_t top_left =
                    static_cast<uint32_t>(y * width + x);
            const uint32_t top_right = top_left + 1;
            const uint32_t bottom_left =
                    top_left + static_cast<uint32_t>(width);
            const uint32_t bottom_right = bottom_left + 1;
            int accumulated[4] = {0, 0, 0, 0};
            for (size_t present_index = 0;
                 present_index < present_types.size();
                 ++present_index) {
                const int terrain_type = present_types[present_index];
                if (present_index + 1 < present_types.size()) {
                    const std::vector<uint8_t>& mask =
                            masks[terrain_type];
                    accumulated[0] = std::min(
                            accumulated[0] + mask[top_left_mask],
                            255);
                    accumulated[1] = std::min(
                            accumulated[1] + mask[top_right_mask],
                            255);
                    accumulated[2] = std::min(
                            accumulated[2] + mask[bottom_left_mask],
                            255);
                    accumulated[3] = std::min(
                            accumulated[3] + mask[bottom_right_mask],
                            255);
                } else {
                    std::fill(
                            std::begin(accumulated),
                            std::end(accumulated),
                            255);
                }
                const int layer_index = type_to_layer[terrain_type];
                TerrainLayer& layer = mesh->layers[layer_index];
                const uint32_t local_top_left = add_layer_vertex(
                        layer_index,
                        top_left,
                        static_cast<uint8_t>(accumulated[0]));
                const uint32_t local_top_right = add_layer_vertex(
                        layer_index,
                        top_right,
                        static_cast<uint8_t>(accumulated[1]));
                const uint32_t local_bottom_left = add_layer_vertex(
                        layer_index,
                        bottom_left,
                        static_cast<uint8_t>(accumulated[2]));
                const uint32_t local_bottom_right = add_layer_vertex(
                        layer_index,
                        bottom_right,
                        static_cast<uint8_t>(accumulated[3]));
                layer.triangle_indices.push_back(local_top_left);
                layer.triangle_indices.push_back(local_bottom_left);
                layer.triangle_indices.push_back(local_top_right);
                layer.triangle_indices.push_back(local_top_right);
                layer.triangle_indices.push_back(local_bottom_left);
                layer.triangle_indices.push_back(local_bottom_right);
            }
        }
    }

    mesh->layers.erase(
            std::remove_if(
                    mesh->layers.begin(),
                    mesh->layers.end(),
                    [](const TerrainLayer& layer) {
                        return layer.triangle_indices.empty();
                    }),
            mesh->layers.end());
    g_terrain_layer_count = mesh->layers.size();
    return !mesh->layers.empty();
}

float TerrainHeightAt(
        const STerrainInfo& info,
        float world_x,
        float world_y) {
    const int width = HeightWidth(info);
    const int height = HeightHeight(info);
    if (width < 2 || height < 2) {
        return 0.0f;
    }

    const float grid_x = std::clamp(
            world_x / VIS_TILE_SIZE,
            0.0f,
            static_cast<float>(width - 1));
    const float grid_y = std::clamp(
            world_y / VIS_TILE_SIZE,
            0.0f,
            static_cast<float>(height - 1));
    const int x0 = std::min(static_cast<int>(grid_x), width - 2);
    const int y0 = std::min(static_cast<int>(grid_y), height - 2);
    const int x1 = x0 + 1;
    const int y1 = y0 + 1;
    const float tx = grid_x - static_cast<float>(x0);
    const float ty = grid_y - static_cast<float>(y0);

    const float h00 = BaseHeight(info, x0, y0) + AddedHeight(info, x0, y0);
    const float h10 = BaseHeight(info, x1, y0) + AddedHeight(info, x1, y0);
    const float h01 = BaseHeight(info, x0, y1) + AddedHeight(info, x0, y1);
    const float h11 = BaseHeight(info, x1, y1) + AddedHeight(info, x1, y1);
    const float top = h00 + (h10 - h00) * tx;
    const float bottom = h01 + (h11 - h01) * tx;
    return top + (bottom - top) * ty;
}

uint32_t ArgbToAbgr(uint32_t argb) {
    return (argb & 0xff00ff00u) |
           ((argb & 0x00ff0000u) >> 16) |
           ((argb & 0x000000ffu) << 16);
}

uint32_t ObjectColor(int player, bool scenario_object) {
    if (scenario_object) {
        return ArgbToAbgr(0xffffc857u);
    }
    static const uint32_t kPlayerColors[] = {
            0xff65e572u,
            0xffdf5b4fu,
            0xffff665au,
            0xff67b85au,
            0xffd98b3au,
            0xffb56ad9u,
            0xff42b8a8u,
            0xffeeeeeeu,
    };
    const int color_count =
            static_cast<int>(sizeof(kPlayerColors) / sizeof(kPlayerColors[0]));
    const int index = player >= 0 ? player % color_count : 0;
    return ArgbToAbgr(kPlayerColors[index]);
}

int GeometryRecordId(const NDb::SHPObjectRPGStats* stats) {
    if (stats == nullptr) {
        return -1;
    }
    const NDb::SVisObj* visual = stats->pvisualObject.GetPtr();
    if (visual == nullptr) {
        return -1;
    }
    const NDb::SModel* fallback = nullptr;
    const NDb::SModel* summer = nullptr;
    for (const NDb::SVisObj::SSingleObj& candidate : visual->models) {
        const NDb::SModel* model = candidate.pModel.GetPtr();
        if (model == nullptr || model->pGeometry.IsEmpty()) {
            continue;
        }
        if (fallback == nullptr) {
            fallback = model;
        }
        if (candidate.eSeason == NDb::SEASON_ASIA) {
            return model->pGeometry->GetRecordID();
        }
        if (candidate.eSeason == NDb::SEASON_SUMMER) {
            summer = model;
        }
    }
    const NDb::SModel* model = summer == nullptr ? fallback : summer;
    return model == nullptr ? -1 : model->pGeometry->GetRecordID();
}

uint64_t StatsPathHash(const NDb::SHPObjectRPGStats* stats) {
    if (stats == nullptr) {
        return 0;
    }
    std::string path(stats->GetDBID().ToString().c_str());
    const size_t xpointer = path.find('#');
    if (xpointer != std::string::npos) {
        path.resize(xpointer);
    }
    while (!path.empty() && (path[0] == '/' || path[0] == '\\')) {
        path.erase(path.begin());
    }
    uint64_t result = 0xcbf29ce484222325ull;
    for (char character : path) {
        unsigned char byte = static_cast<unsigned char>(
                character == '\\' ? '/' : character);
        if (byte >= 'A' && byte <= 'Z') {
            byte = static_cast<unsigned char>(byte - 'A' + 'a');
        }
        result ^= byte;
        result *= 0x100000001b3ull;
    }
    return result;
}

bool ReadExact(std::ifstream* input, void* output, size_t size) {
    if (input == nullptr || output == nullptr ||
        size > static_cast<size_t>(std::numeric_limits<std::streamsize>::max())) {
        return false;
    }
    input->read(
            reinterpret_cast<char*>(output),
            static_cast<std::streamsize>(size));
    return input->good();
}

const ConvertedGeometry* LoadConvertedGeometry(
        int record_id,
        ConvertedAnimationVariant animation_variant) {
    std::unordered_map<int, ConvertedGeometry>* converted_geometries =
            &g_converted_geometries;
    std::unordered_set<int>* missing_converted_geometries =
            &g_missing_converted_geometries;
    const char* filename_suffix = ".bk2mesh";
    if (animation_variant == ConvertedAnimationVariant::Move) {
        converted_geometries = &g_move_converted_geometries;
        missing_converted_geometries = &g_missing_move_converted_geometries;
        filename_suffix = ".move.bk2mesh";
    } else if (animation_variant == ConvertedAnimationVariant::Attack) {
        converted_geometries = &g_attack_converted_geometries;
        missing_converted_geometries = &g_missing_attack_converted_geometries;
        filename_suffix = ".attack.bk2mesh";
    } else if (animation_variant == ConvertedAnimationVariant::Death) {
        converted_geometries = &g_death_converted_geometries;
        missing_converted_geometries = &g_missing_death_converted_geometries;
        filename_suffix = ".death.bk2mesh";
    } else if (animation_variant == ConvertedAnimationVariant::LyingIdle) {
        converted_geometries = &g_lying_idle_converted_geometries;
        missing_converted_geometries =
                &g_missing_lying_idle_converted_geometries;
        filename_suffix = ".lying.bk2mesh";
    } else if (animation_variant == ConvertedAnimationVariant::LyingMove) {
        converted_geometries = &g_lying_move_converted_geometries;
        missing_converted_geometries =
                &g_missing_lying_move_converted_geometries;
        filename_suffix = ".lying.move.bk2mesh";
    } else if (animation_variant == ConvertedAnimationVariant::LyingAttack) {
        converted_geometries = &g_lying_attack_converted_geometries;
        missing_converted_geometries =
                &g_missing_lying_attack_converted_geometries;
        filename_suffix = ".lying.attack.bk2mesh";
    }
    const auto loaded = converted_geometries->find(record_id);
    if (loaded != converted_geometries->end()) {
        return &loaded->second;
    }
    if (record_id < 0 ||
        missing_converted_geometries->find(record_id) !=
                missing_converted_geometries->end()) {
        return nullptr;
    }

    const std::string path = JoinHostPath(
            GetPortPaths().data_root(),
            "Converted/Geometries/" + std::to_string(record_id) +
                    filename_suffix);
    std::ifstream input(path, std::ios::in | std::ios::binary);
    char magic[8] = {};
    uint32_t version = 0;
    uint32_t mesh_count = 0;
    const char expected_magic[8] = {'B', 'K', '2', 'M', 'S', 'H', '1', '\0'};
    if (!input.is_open() ||
        !ReadExact(&input, magic, sizeof(magic)) ||
        std::memcmp(magic, expected_magic, sizeof(magic)) != 0 ||
        !ReadExact(&input, &version, sizeof(version)) ||
        !ReadExact(&input, &mesh_count, sizeof(mesh_count)) ||
        (version != 1 && version != 2 && version != 3) ||
        mesh_count == 0 ||
        mesh_count > 128) {
        missing_converted_geometries->insert(record_id);
        return nullptr;
    }

    ConvertedGeometry geometry;
    size_t animated_parts = 0;
    for (uint32_t mesh_index = 0; mesh_index < mesh_count; ++mesh_index) {
        uint32_t vertex_count = 0;
        uint32_t index_count = 0;
        uint32_t group_count = 1;
        uint32_t animation_frame_count = 1;
        float animation_duration_seconds = 0.0f;
        if (!ReadExact(&input, &vertex_count, sizeof(vertex_count)) ||
            !ReadExact(&input, &index_count, sizeof(index_count)) ||
            (version >= 2 &&
             !ReadExact(&input, &group_count, sizeof(group_count))) ||
            (version >= 3 &&
             (!ReadExact(
                      &input,
                      &animation_frame_count,
                      sizeof(animation_frame_count)) ||
              !ReadExact(
                      &input,
                      &animation_duration_seconds,
                      sizeof(animation_duration_seconds)))) ||
            vertex_count == 0 ||
            vertex_count > 5000000 ||
            index_count < 3 ||
            index_count > 15000000 ||
            index_count % 3 != 0 ||
            group_count == 0 ||
            group_count > 4096 ||
            animation_frame_count == 0 ||
            animation_frame_count > 120 ||
            !std::isfinite(animation_duration_seconds) ||
            animation_duration_seconds < 0.0f) {
            missing_converted_geometries->insert(record_id);
            return nullptr;
        }
        ConvertedGeometryPart part;
        part.animation_duration_seconds = animation_duration_seconds;
        part.animation_frames.resize(animation_frame_count);
        if (animation_frame_count > 1) {
            ++animated_parts;
        }
        for (std::vector<ConvertedGeometryVertex>& frame :
             part.animation_frames) {
            frame.resize(vertex_count);
            if (!ReadExact(
                        &input,
                        frame.data(),
                        static_cast<size_t>(vertex_count) *
                                sizeof(ConvertedGeometryVertex))) {
                missing_converted_geometries->insert(record_id);
                return nullptr;
            }
        }
        part.vertices = part.animation_frames.front();
        part.triangle_indices.resize(index_count);
        if (!ReadExact(
                    &input,
                    part.triangle_indices.data(),
                    static_cast<size_t>(index_count) * sizeof(uint32_t))) {
            missing_converted_geometries->insert(record_id);
            return nullptr;
        }
        for (uint32_t index : part.triangle_indices) {
            if (index >= vertex_count) {
                missing_converted_geometries->insert(record_id);
                return nullptr;
            }
        }
        if (version == 1) {
            part.groups.push_back(ConvertedGeometryGroup{
                    0,
                    0,
                    index_count});
        } else {
            for (uint32_t group_index = 0;
                 group_index < group_count;
                 ++group_index) {
                uint32_t material_index = 0;
                uint32_t first_triangle = 0;
                uint32_t triangle_count = 0;
                if (!ReadExact(
                            &input,
                            &material_index,
                            sizeof(material_index)) ||
                    !ReadExact(
                            &input,
                            &first_triangle,
                            sizeof(first_triangle)) ||
                    !ReadExact(
                            &input,
                            &triangle_count,
                            sizeof(triangle_count)) ||
                    triangle_count == 0 ||
                    (static_cast<uint64_t>(first_triangle) +
                     triangle_count) * 3ull >
                            index_count) {
                    missing_converted_geometries->insert(record_id);
                    return nullptr;
                }
                part.groups.push_back(ConvertedGeometryGroup{
                        material_index,
                        first_triangle * 3,
                        triangle_count * 3});
            }
        }
        geometry.parts.push_back(std::move(part));
    }

    auto inserted = converted_geometries->emplace(
            record_id,
            std::move(geometry));
    g_animated_geometry_part_count += animated_parts;
    return &inserted.first->second;
}

std::vector<std::string> SplitPreservingEmpty(
        const std::string& value,
        char delimiter) {
    std::vector<std::string> result;
    size_t start = 0;
    while (start <= value.size()) {
        const size_t end = value.find(delimiter, start);
        result.push_back(value.substr(
                start,
                end == std::string::npos
                        ? std::string::npos
                        : end - start));
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return result;
}

GeometryBinding ResolveGeometryBinding(
        uint64_t stats_path_hash,
        int stats_record_id,
        int geometry_record_id,
        int frame_index) {
    (void)stats_record_id;
    if (!g_stats_geometry_index_loaded) {
        g_stats_geometry_index_loaded = true;
        const std::string path = JoinHostPath(
                GetPortPaths().data_root(),
                "Converted/geometry_index.tsv");
        std::ifstream input(path);
        std::string line;
        while (std::getline(input, line)) {
            if (line.empty() || line[0] == '#') {
                continue;
            }
            const std::vector<std::string> fields =
                    SplitPreservingEmpty(line, '\t');
            if (fields.size() < 5 || fields[0].empty()) {
                continue;
            }
            char* end = nullptr;
            const uint64_t path_hash = std::strtoull(
                    fields[0].c_str(),
                    &end,
                    16);
            const int stats = std::atoi(fields[1].c_str());
            const int geometry = std::atoi(fields[2].c_str());
            if (end == fields[0].c_str() ||
                *end != '\0' ||
                stats < 0 ||
                geometry < 0) {
                continue;
            }
            GeometryBinding binding;
            binding.geometry_record_id = geometry;
            if (!fields[3].empty()) {
                for (const std::string& quantity :
                     SplitPreservingEmpty(fields[3], ',')) {
                    binding.material_quantities.push_back(
                            std::max(std::atoi(quantity.c_str()), 0));
                }
            }
            binding.texture_paths =
                    SplitPreservingEmpty(fields[4], '|');
            const int indexed_frame =
                    fields.size() >= 6 ? std::atoi(fields[5].c_str()) : -1;
            if (fields.size() >= 7) {
                const float scale = std::strtof(fields[6].c_str(), nullptr);
                if (std::isfinite(scale) && scale > 0.0f) {
                    binding.geometry_scale = scale;
                }
            }
            if (indexed_frame >= 0) {
                g_stats_geometry_variants[path_hash][indexed_frame] =
                        std::move(binding);
            } else {
                g_stats_geometry_index[path_hash] = std::move(binding);
            }
        }
    }
    if (frame_index >= 0) {
        const auto variants =
                g_stats_geometry_variants.find(stats_path_hash);
        if (variants != g_stats_geometry_variants.end()) {
            const auto variant = variants->second.find(frame_index);
            if (variant != variants->second.end()) {
                return variant->second;
            }
        }
    }
    const auto mapped = g_stats_geometry_index.find(stats_path_hash);
    if (mapped != g_stats_geometry_index.end()) {
        return mapped->second;
    }
    GeometryBinding fallback;
    fallback.geometry_record_id = geometry_record_id;
    return fallback;
}

WorldObjectMesh::Layer* FindOrAddWorldObjectLayer(
        WorldObjectMesh* mesh,
        const std::string& texture_path) {
    if (mesh == nullptr || texture_path.empty()) {
        return nullptr;
    }
    for (WorldObjectMesh::Layer& layer : mesh->layers) {
        if (layer.texture_path == texture_path) {
            return &layer;
        }
    }
    mesh->layers.push_back(WorldObjectMesh::Layer());
    mesh->layers.back().texture_path = texture_path;
    return &mesh->layers.back();
}

void AppendPartIndices(
        std::vector<uint32_t>* output,
        const ConvertedGeometryPart& part,
        uint32_t vertex_base,
        uint32_t first_index,
        uint32_t index_count) {
    if (output == nullptr ||
        static_cast<uint64_t>(first_index) + index_count >
                part.triangle_indices.size()) {
        return;
    }
    output->reserve(output->size() + index_count);
    for (uint32_t index = 0; index < index_count; ++index) {
        output->push_back(
                vertex_base +
                part.triangle_indices[first_index + index]);
    }
}

bool AppendConvertedGeometry(
        WorldObjectMesh* mesh,
        uint64_t stats_path_hash,
        int stats_record_id,
        int geometry_record_id,
        float x,
        float y,
        float z,
        float heading,
        uint32_t abgr,
        ConvertedAnimationVariant animation_variant,
        float animation_time_seconds,
        int frame_index) {
    if (mesh == nullptr) {
        return false;
    }
    const GeometryBinding binding =
            ResolveGeometryBinding(
                    stats_path_hash,
                    stats_record_id,
                    geometry_record_id,
                    frame_index);
    const ConvertedGeometry* geometry =
            animation_variant == ConvertedAnimationVariant::Base
                    ? nullptr
                    : LoadConvertedGeometry(
                              binding.geometry_record_id,
                              animation_variant);
    if (geometry != nullptr) {
        size_t* instance_count = &g_move_animation_instance_count;
        const char* diagnostic = "move_animation_runtime=active; geometry=";
        if (animation_variant == ConvertedAnimationVariant::Attack) {
            instance_count = &g_attack_animation_instance_count;
            diagnostic = "attack_animation_runtime=active; geometry=";
        } else if (animation_variant == ConvertedAnimationVariant::Death) {
            instance_count = &g_death_animation_instance_count;
            diagnostic = "death_animation_runtime=active; geometry=";
        } else if (animation_variant ==
                   ConvertedAnimationVariant::LyingIdle) {
            instance_count = &g_lying_idle_animation_instance_count;
            diagnostic = "lying_idle_animation_runtime=active; geometry=";
        } else if (animation_variant ==
                   ConvertedAnimationVariant::LyingMove) {
            instance_count = &g_lying_move_animation_instance_count;
            diagnostic = "lying_move_animation_runtime=active; geometry=";
        } else if (animation_variant ==
                   ConvertedAnimationVariant::LyingAttack) {
            instance_count = &g_lying_attack_animation_instance_count;
            diagnostic = "lying_attack_animation_runtime=active; geometry=";
        }
        ++*instance_count;
        if (*instance_count == 1) {
            PlatformRuntime::instance().log_info(
                    std::string(diagnostic) +
                    std::to_string(binding.geometry_record_id));
        }
    } else {
        geometry = LoadConvertedGeometry(
                binding.geometry_record_id,
                ConvertedAnimationVariant::Base);
    }
    if (geometry == nullptr) {
        return false;
    }
    const float cosine = std::cos(heading);
    const float sine = std::sin(heading);
    size_t total_vertices = 0;
    for (const ConvertedGeometryPart& part : geometry->parts) {
        total_vertices += part.vertices.size();
    }
    mesh->vertices.reserve(mesh->vertices.size() + total_vertices);
    const bool has_textures = std::any_of(
            binding.texture_paths.begin(),
            binding.texture_paths.end(),
            [](const std::string& value) {
                return !value.empty();
            });
    int material_base = 0;
    for (size_t part_index = 0;
         part_index < geometry->parts.size();
         ++part_index) {
        const ConvertedGeometryPart& part = geometry->parts[part_index];
        const std::vector<ConvertedGeometryVertex>* vertices =
                &part.vertices;
        if (part.animation_frames.size() > 1 &&
            part.animation_duration_seconds > 0.0f) {
            const float animation_time =
                    animation_variant == ConvertedAnimationVariant::Death
                    ? std::min(
                              std::max(animation_time_seconds, 0.0f),
                              part.animation_duration_seconds)
                    : std::fmod(
                              std::max(animation_time_seconds, 0.0f),
                              part.animation_duration_seconds);
            const size_t animation_frame = std::min(
                    static_cast<size_t>(
                            animation_time /
                            part.animation_duration_seconds *
                            part.animation_frames.size()),
                    part.animation_frames.size() - 1);
            vertices = &part.animation_frames[animation_frame];
        }
        const uint32_t vertex_base =
                static_cast<uint32_t>(mesh->vertices.size());
        for (const ConvertedGeometryVertex& vertex : *vertices) {
            mesh->vertices.push_back(TerrainVertex{
                    x + binding.geometry_scale *
                            (cosine * vertex.x - sine * vertex.y),
                    y + binding.geometry_scale *
                            (sine * vertex.x + cosine * vertex.y),
                    z + binding.geometry_scale * vertex.z + 0.05f,
                    vertex.u,
                    vertex.v,
                    has_textures ? 0xffffffffu : abgr});
        }

        if (binding.material_quantities.empty()) {
            const int material_index = binding.texture_paths.empty()
                    ? -1
                    : std::min(
                            static_cast<int>(part_index),
                            static_cast<int>(
                                    binding.texture_paths.size()) - 1);
            const std::string texture_path = material_index < 0
                    ? std::string()
                    : binding.texture_paths[material_index];
            WorldObjectMesh::Layer* layer =
                    FindOrAddWorldObjectLayer(mesh, texture_path);
            std::vector<uint32_t>* output = layer == nullptr
                    ? &mesh->triangle_indices
                    : &layer->triangle_indices;
            AppendPartIndices(
                    output,
                    part,
                    vertex_base,
                    0,
                    static_cast<uint32_t>(
                            part.triangle_indices.size()));
            continue;
        }

        const int material_count =
                part_index < binding.material_quantities.size()
                ? binding.material_quantities[part_index]
                : 0;
        for (const ConvertedGeometryGroup& group : part.groups) {
            const int material_index =
                    material_count > 0
                    ? material_base +
                            std::min(
                                    static_cast<int>(group.material_index),
                                    material_count - 1)
                    : -1;
            const std::string texture_path =
                    material_index >= 0 &&
                            material_index <
                                    static_cast<int>(
                                            binding.texture_paths.size())
                    ? binding.texture_paths[material_index]
                    : std::string();
            WorldObjectMesh::Layer* layer =
                    FindOrAddWorldObjectLayer(mesh, texture_path);
            AppendPartIndices(
                    layer == nullptr
                            ? &mesh->triangle_indices
                            : &layer->triangle_indices,
                    part,
                    vertex_base,
                    group.first_index,
                    group.index_count);
        }
        material_base += material_count;
    }
    ++g_converted_geometry_instance_count;
    return true;
}

void AppendObjectMarker(
        WorldObjectMesh* mesh,
        float x,
        float y,
        float z,
        float half_size,
        float height,
        uint32_t abgr) {
    const uint32_t base = static_cast<uint32_t>(mesh->vertices.size());
    const float base_z = z + 0.15f;
    mesh->vertices.push_back(
            TerrainVertex{x - half_size, y - half_size, base_z, 0.0f, 0.0f, abgr});
    mesh->vertices.push_back(
            TerrainVertex{x + half_size, y - half_size, base_z, 1.0f, 0.0f, abgr});
    mesh->vertices.push_back(
            TerrainVertex{x + half_size, y + half_size, base_z, 1.0f, 1.0f, abgr});
    mesh->vertices.push_back(
            TerrainVertex{x - half_size, y + half_size, base_z, 0.0f, 1.0f, abgr});
    mesh->vertices.push_back(
            TerrainVertex{x, y, base_z + height, 0.5f, 0.5f, abgr});

    const uint32_t indices[] = {
            0, 2, 1,
            0, 3, 2,
            0, 1, 4,
            1, 2, 4,
            2, 3, 4,
            3, 0, 4,
    };
    for (uint32_t index : indices) {
        mesh->triangle_indices.push_back(base + index);
    }
}

void AppendOrientedBox(
        WorldObjectMesh* mesh,
        float x,
        float y,
        float z,
        float half_length,
        float half_width,
        float height,
        float heading,
        uint32_t abgr) {
    const uint32_t base = static_cast<uint32_t>(mesh->vertices.size());
    const float forward_x = std::cos(heading);
    const float forward_y = std::sin(heading);
    const float right_x = -forward_y;
    const float right_y = forward_x;
    const float base_z = z + 0.15f;
    const float top_z = base_z + height;
    const float local[][2] = {
            {-half_length, -half_width},
            {half_length, -half_width},
            {half_length, half_width},
            {-half_length, half_width},
    };
    for (int layer = 0; layer < 2; ++layer) {
        for (const auto& point : local) {
            mesh->vertices.push_back(TerrainVertex{
                    x + forward_x * point[0] + right_x * point[1],
                    y + forward_y * point[0] + right_y * point[1],
                    layer == 0 ? base_z : top_z,
                    point[0] / (half_length * 2.0f) + 0.5f,
                    point[1] / (half_width * 2.0f) + 0.5f,
                    abgr});
        }
    }
    const uint32_t indices[] = {
            0, 2, 1, 0, 3, 2,
            4, 5, 6, 4, 6, 7,
            0, 1, 5, 0, 5, 4,
            1, 2, 6, 1, 6, 5,
            2, 3, 7, 2, 7, 6,
            3, 0, 4, 3, 4, 7,
    };
    for (uint32_t index : indices) {
        mesh->triangle_indices.push_back(base + index);
    }
}

void AppendUnitIndicator(
        WorldObjectMesh* mesh,
        const Bk2PresentationEntity& entity,
        uint32_t abgr) {
    if (mesh == nullptr) {
        return;
    }
    constexpr int kSegmentCount = 32;
    const float selection_scale =
            std::isfinite(entity.visual_scale)
            ? std::clamp(entity.visual_scale, 0.6f, 2.5f)
            : 1.0f;
    const float base_radius =
            (entity.flags & BK2_PRESENTATION_ENTITY_MECHANIZED) != 0
            ? 3.0f
            : (entity.flags & BK2_PRESENTATION_ENTITY_FORMATION) != 0
                    ? 2.5f
                    : 1.25f;
    const float outer_radius = base_radius * selection_scale;
    const float inner_radius = outer_radius * 0.78f;
    const float indicator_z = entity.z + 0.12f;
    const uint32_t base =
            static_cast<uint32_t>(mesh->vertices.size());
    mesh->vertices.reserve(
            mesh->vertices.size() +
            static_cast<size_t>(kSegmentCount + 1) * 2);
    mesh->triangle_indices.reserve(
            mesh->triangle_indices.size() +
            static_cast<size_t>(kSegmentCount) * 6);
    for (int segment = 0; segment <= kSegmentCount; ++segment) {
        const float angle =
                static_cast<float>(segment) *
                2.0f * 3.14159265358979323846f /
                static_cast<float>(kSegmentCount);
        const float cosine = std::cos(angle);
        const float sine = std::sin(angle);
        mesh->vertices.push_back(TerrainVertex{
                entity.x + cosine * outer_radius,
                entity.y + sine * outer_radius,
                indicator_z,
                0.0f,
                0.0f,
                abgr});
        mesh->vertices.push_back(TerrainVertex{
                entity.x + cosine * inner_radius,
                entity.y + sine * inner_radius,
                indicator_z,
                0.0f,
                0.0f,
                abgr});
    }
    for (int segment = 0; segment < kSegmentCount; ++segment) {
        const uint32_t outer = base + static_cast<uint32_t>(segment * 2);
        const uint32_t inner = outer + 1;
        const uint32_t next_outer = outer + 2;
        const uint32_t next_inner = outer + 3;
        mesh->triangle_indices.push_back(outer);
        mesh->triangle_indices.push_back(next_outer);
        mesh->triangle_indices.push_back(inner);
        mesh->triangle_indices.push_back(inner);
        mesh->triangle_indices.push_back(next_outer);
        mesh->triangle_indices.push_back(next_inner);
    }
}

void AppendEntityModel(
        WorldObjectMesh* mesh,
        const Bk2PresentationEntity& entity,
        uint32_t abgr,
        bool selected,
        float animation_time_seconds) {
    ConvertedAnimationVariant animation_variant =
            ConvertedAnimationVariant::Base;
    if ((entity.flags & BK2_PRESENTATION_ENTITY_INFANTRY) != 0) {
        if ((entity.flags & BK2_PRESENTATION_ENTITY_DEAD) != 0) {
            animation_variant = ConvertedAnimationVariant::Death;
        } else if ((entity.flags & BK2_PRESENTATION_ENTITY_LYING) != 0 &&
                   (entity.flags & BK2_PRESENTATION_ENTITY_ATTACKING) != 0) {
            animation_variant = ConvertedAnimationVariant::LyingAttack;
        } else if ((entity.flags & BK2_PRESENTATION_ENTITY_LYING) != 0 &&
                   (entity.flags & BK2_PRESENTATION_ENTITY_MOVING) != 0) {
            animation_variant = ConvertedAnimationVariant::LyingMove;
        } else if ((entity.flags & BK2_PRESENTATION_ENTITY_LYING) != 0) {
            animation_variant = ConvertedAnimationVariant::LyingIdle;
        } else if ((entity.flags & BK2_PRESENTATION_ENTITY_ATTACKING) != 0) {
            animation_variant = ConvertedAnimationVariant::Attack;
        } else if ((entity.flags & BK2_PRESENTATION_ENTITY_MOVING) != 0) {
            animation_variant = ConvertedAnimationVariant::Move;
        }
    }
    if (AppendConvertedGeometry(
                mesh,
                entity.rpg_stats_path_hash,
                entity.rpg_stats_record_id,
                entity.geometry_record_id,
                entity.x,
                entity.y,
                entity.z,
                entity.heading_radians,
                abgr,
                animation_variant,
                animation_time_seconds,
                -1)) {
        return;
    }
    ++g_converted_geometry_fallback_count;
    ++g_dynamic_fallback_stats_hashes[entity.rpg_stats_path_hash];
    const float scale = selected ? 1.25f : 1.0f;
    if ((entity.flags & BK2_PRESENTATION_ENTITY_MECHANIZED) != 0) {
        AppendOrientedBox(
                mesh,
                entity.x,
                entity.y,
                entity.z,
                2.15f * scale,
                1.25f * scale,
                1.15f * scale,
                entity.heading_radians,
                abgr);
        const float forward_x = std::cos(entity.heading_radians);
        const float forward_y = std::sin(entity.heading_radians);
        AppendOrientedBox(
                mesh,
                entity.x + forward_x * 0.2f,
                entity.y + forward_y * 0.2f,
                entity.z + 1.2f * scale,
                0.9f * scale,
                0.72f * scale,
                0.7f * scale,
                entity.heading_radians,
                abgr);
        AppendOrientedBox(
                mesh,
                entity.x + forward_x * 1.45f * scale,
                entity.y + forward_y * 1.45f * scale,
                entity.z + 1.48f * scale,
                0.85f * scale,
                0.12f * scale,
                0.18f * scale,
                entity.heading_radians,
                abgr);
        return;
    }

    const bool formation =
            (entity.flags & BK2_PRESENTATION_ENTITY_FORMATION) != 0;
    const int figure_count = formation ? 4 : 1;
    const float offsets[][2] = {
            {-0.8f, -0.65f},
            {0.8f, -0.65f},
            {-0.8f, 0.65f},
            {0.8f, 0.65f},
    };
    for (int index = 0; index < figure_count; ++index) {
        const float offset_x = formation ? offsets[index][0] * scale : 0.0f;
        const float offset_y = formation ? offsets[index][1] * scale : 0.0f;
        AppendOrientedBox(
                mesh,
                entity.x + offset_x,
                entity.y + offset_y,
                entity.z,
                0.28f * scale,
                0.28f * scale,
                1.7f * scale,
                entity.heading_radians,
                abgr);
    }
}

void AppendCameraFacingSprite(
        WorldObjectMesh* mesh,
        float x,
        float y,
        float z,
        float width,
        float height,
        uint32_t abgr,
        const std::string& texture_path) {
    if (mesh == nullptr || texture_path.empty() ||
        width <= 0.0f || height <= 0.0f) {
        return;
    }
    const float half_width = width * 0.5f;
    const float half_height = height * 0.5f;
    const float right_x = std::cos(g_camera.yaw_radians);
    const float right_y = std::sin(g_camera.yaw_radians);
    const uint32_t base =
            static_cast<uint32_t>(mesh->vertices.size());
    mesh->vertices.push_back(TerrainVertex{
            x - right_x * half_width,
            y - right_y * half_width,
            z - half_height,
            0.0f,
            1.0f,
            abgr});
    mesh->vertices.push_back(TerrainVertex{
            x + right_x * half_width,
            y + right_y * half_width,
            z - half_height,
            1.0f,
            1.0f,
            abgr});
    mesh->vertices.push_back(TerrainVertex{
            x + right_x * half_width,
            y + right_y * half_width,
            z + half_height,
            1.0f,
            0.0f,
            abgr});
    mesh->vertices.push_back(TerrainVertex{
            x - right_x * half_width,
            y - right_y * half_width,
            z + half_height,
            0.0f,
            0.0f,
            abgr});
    WorldObjectMesh::Layer* layer =
            FindOrAddWorldObjectLayer(mesh, texture_path);
    if (layer == nullptr) {
        return;
    }
    layer->alpha_blended = true;
    const uint32_t indices[] = {0, 1, 2, 0, 2, 3};
    for (uint32_t index : indices) {
        layer->triangle_indices.push_back(base + index);
    }
}

void AppendDestructionEffect(
        WorldObjectMesh* mesh,
        const AndroidDestructionEffect& effect) {
    if (mesh == nullptr || effect.lifetime_millis == 0) {
        return;
    }
    constexpr uint32_t kSmokeCycleMillis = 2600u;
    constexpr int kSmokePuffCount = 3;
    for (int puff = 0; puff < kSmokePuffCount; ++puff) {
        const uint32_t cycle_age =
                (effect.age_millis +
                 static_cast<uint32_t>(puff) *
                         (kSmokeCycleMillis / kSmokePuffCount)) %
                kSmokeCycleMillis;
        const float phase =
                static_cast<float>(cycle_age) /
                static_cast<float>(kSmokeCycleMillis);
        const float envelope =
                std::sin(
                        phase *
                        3.14159265358979323846f);
        const uint32_t alpha =
                static_cast<uint32_t>(
                        std::lround(
                                std::max(envelope, 0.0f) * 190.0f));
        const float seed =
                static_cast<float>(
                        (effect.unit_id & 0xff) + puff * 37);
        const float drift_x =
                std::sin(seed * 0.73f) * phase * 1.2f;
        const float drift_y =
                std::cos(seed * 0.51f) * phase * 1.2f;
        const float size = 3.8f + phase * 4.2f;
        AppendCameraFacingSprite(
                mesh,
                effect.x + drift_x,
                effect.y + drift_y,
                effect.z + 3.0f + phase * 7.0f,
                size,
                size,
                (alpha << 24) | 0x00404040u,
                kDestructionSmokeTextures[
                        static_cast<size_t>(puff) %
                         (sizeof(kDestructionSmokeTextures) /
                         sizeof(kDestructionSmokeTextures[0]))]);
    }

    const uint32_t fire_frame =
            (effect.age_millis / 110u) %
            static_cast<uint32_t>(
                    sizeof(kDestructionFireTextures) /
                    sizeof(kDestructionFireTextures[0]));
    const float flicker =
            0.9f +
            0.12f *
                    std::sin(
                            static_cast<float>(effect.age_millis) *
                            0.035f);
    AppendCameraFacingSprite(
            mesh,
            effect.x,
            effect.y,
            effect.z + 3.2f,
            4.8f * flicker,
            6.4f * flicker,
            0xffffffffu,
            kDestructionFireTextures[fire_frame]);
}

void AppendCombatEffectRibbon(
        WorldObjectMesh* mesh,
        const AndroidCombatEffect& effect) {
    if (mesh == nullptr || effect.lifetime_millis == 0) {
        return;
    }
    const float delta_x =
            effect.destination_x - effect.source_x;
    const float delta_y =
            effect.destination_y - effect.source_y;
    const float delta_z =
            effect.destination_z - effect.source_z;
    const float distance =
            std::sqrt(delta_x * delta_x + delta_y * delta_y);
    if (!std::isfinite(distance) || distance < 0.01f) {
        return;
    }

    const float progress = std::min(
            static_cast<float>(effect.age_millis) /
                    static_cast<float>(effect.lifetime_millis),
            1.0f);
    const float maximum_streak =
            effect.type == AndroidCombatEffectType::InfantryShot
            ? 10.0f
            : 18.0f;
    const float streak_length = std::min(distance, maximum_streak);
    const float streak_end = std::min(
            distance,
            std::max(streak_length, progress * distance));
    const float streak_start =
            std::max(0.0f, streak_end - streak_length);
    const float inverse_distance = 1.0f / distance;
    const float direction_x = delta_x * inverse_distance;
    const float direction_y = delta_y * inverse_distance;
    const float perpendicular_x = -direction_y;
    const float perpendicular_y = direction_x;
    const float width =
            effect.type == AndroidCombatEffectType::InfantryShot
            ? 0.08f
            : 0.16f;
    const float start_ratio = streak_start * inverse_distance;
    const float end_ratio = streak_end * inverse_distance;
    const float source_height =
            effect.type == AndroidCombatEffectType::InfantryShot
            ? 1.25f
            : 1.75f;
    const float start_x =
            effect.source_x + direction_x * streak_start;
    const float start_y =
            effect.source_y + direction_y * streak_start;
    const float start_z =
            effect.source_z + source_height + delta_z * start_ratio;
    const float end_x =
            effect.source_x + direction_x * streak_end;
    const float end_y =
            effect.source_y + direction_y * streak_end;
    const float end_z =
            effect.source_z + source_height + delta_z * end_ratio;
    const uint32_t color =
            effect.type == AndroidCombatEffectType::InfantryShot
            ? ArgbToAbgr(0xffffdf69u)
            : ArgbToAbgr(0xffff9d35u);

    const uint32_t ribbon_base =
            static_cast<uint32_t>(mesh->vertices.size());
    mesh->vertices.push_back(TerrainVertex{
            start_x + perpendicular_x * width,
            start_y + perpendicular_y * width,
            start_z,
            0.0f,
            0.0f,
            color});
    mesh->vertices.push_back(TerrainVertex{
            start_x - perpendicular_x * width,
            start_y - perpendicular_y * width,
            start_z,
            0.0f,
            1.0f,
            color});
    mesh->vertices.push_back(TerrainVertex{
            end_x - perpendicular_x * width,
            end_y - perpendicular_y * width,
            end_z,
            1.0f,
            1.0f,
            color});
    mesh->vertices.push_back(TerrainVertex{
            end_x + perpendicular_x * width,
            end_y + perpendicular_y * width,
            end_z,
            1.0f,
            0.0f,
            color});
    WorldObjectMesh::Layer* trace_layer = FindOrAddWorldObjectLayer(
            mesh,
            effect.type == AndroidCombatEffectType::InfantryShot
                    ? kInfantryTraceTexture
                    : kMechanizedTraceTexture);
    if (trace_layer != nullptr) {
        trace_layer->alpha_blended = true;
    }
    std::vector<uint32_t>* trace_indices =
            trace_layer == nullptr
                    ? &mesh->triangle_indices
                    : &trace_layer->triangle_indices;
    const uint32_t ribbon_indices[] = {0, 2, 1, 0, 3, 2};
    for (uint32_t index : ribbon_indices) {
        trace_indices->push_back(ribbon_base + index);
    }

    if (effect.age_millis <= 120u) {
        const float flash_size =
                effect.type == AndroidCombatEffectType::InfantryShot
                ? 0.9f
                : 1.75f;
        const float flash_progress =
                static_cast<float>(effect.age_millis) / 120.0f;
        const uint32_t flash_alpha =
                static_cast<uint32_t>(
                        std::lround(
                                (1.0f - flash_progress) * 255.0f));
        AppendCameraFacingSprite(
                mesh,
                effect.source_x + direction_x * flash_size * 0.45f,
                effect.source_y + direction_y * flash_size * 0.45f,
                effect.source_z + source_height,
                flash_size,
                flash_size,
                (flash_alpha << 24) | 0x00ffffffu,
                kMuzzleFlashTexture);
    }
    ++g_combat_effect_render_count;
    if (g_combat_effect_render_count == 1) {
        PlatformRuntime::instance().log_info(
                std::string("combat_effect_render=active; source=") +
                std::to_string(effect.source_unit_id) +
                "; trace_texture=" +
                (effect.type == AndroidCombatEffectType::InfantryShot
                         ? kInfantryTraceTexture
                         : kMechanizedTraceTexture));
    }
}

void AppendMapObjects(
        const vector<NDb::SMapObjectInfo>& objects,
        const STerrainInfo& terrain_info,
        bool scenario_objects,
        bool include_dynamic_units,
        bool include_minor_objects,
        bool count_rendered,
        WorldObjectMesh* mesh) {
    for (size_t i = 0; i < objects.size(); ++i) {
        const NDb::SMapObjectInfo& object = objects[i];
        const NDb::SHPObjectRPGStats* stats =
                object.pObject.GetPtrNoLoad();
        if (stats == nullptr) {
            continue;
        }
        const int type_id = stats->GetTypeID();
        const bool dynamic_unit =
                type_id == NDb::SMechUnitRPGStats::typeID ||
                type_id == NDb::SSquadRPGStats::typeID ||
                type_id == NDb::SInfantryRPGStats::typeID;
        if (dynamic_unit && !include_dynamic_units) {
            continue;
        }
        const bool visible_gameplay_object =
                dynamic_unit ||
                type_id == NDb::SBuildingRPGStats::typeID ||
                type_id == NDb::SBridgeRPGStats::typeID ||
                type_id == NDb::SEntrenchmentRPGStats::typeID ||
                (include_minor_objects &&
                 (type_id == NDb::SFenceRPGStats::typeID ||
                  type_id == NDb::SMineRPGStats::typeID));
        if (!visible_gameplay_object) {
            continue;
        }
        const float x = AI2Vis(object.vPos.x);
        const float y = AI2Vis(object.vPos.y);
        const float z =
                TerrainHeightAt(terrain_info, x, y) + AI2Vis(object.vPos.z);
        const float heading =
                static_cast<float>(object.nDir & 0xffff) /
                65536.0f * 6.28318530717958647692f;
        if (!AppendConvertedGeometry(
                    mesh,
                    StatsPathHash(stats),
                    stats->GetRecordID(),
                    GeometryRecordId(stats),
                    x,
                    y,
                    z,
                    heading,
                    ObjectColor(object.nPlayer, scenario_objects),
                    ConvertedAnimationVariant::Base,
                    0.0f,
                    object.nFrameIndex)) {
            ++g_converted_geometry_fallback_count;
            ++g_static_fallback_stats_paths[
                    stats->GetDBID().ToString().c_str()];
            AppendObjectMarker(
                    mesh,
                    x,
                    y,
                    z,
                    scenario_objects ? 1.25f : 0.85f,
                    scenario_objects ? 5.0f : 3.5f,
                    ObjectColor(object.nPlayer, scenario_objects));
        }
        if (count_rendered) {
            ++g_rendered_object_count;
        }
    }
}

void BuildPresentationStaticWorldMesh(
        const NDb::SMapInfo* map,
        const STerrainInfo& terrain_info,
        WorldObjectMesh* mesh) {
    if (map == nullptr || mesh == nullptr) {
        return;
    }
    g_map_object_count = map->objects.size();
    g_scenario_object_count = map->scenarioObjects.size();
    g_rendered_object_count = 0;
    const size_t total = g_map_object_count + g_scenario_object_count;
    mesh->vertices.reserve(total * 5);
    mesh->triangle_indices.reserve(total * 18);
    AppendMapObjects(
            map->objects,
            terrain_info,
            false,
            false,
            true,
            true,
            mesh);
    AppendMapObjects(
            map->scenarioObjects,
            terrain_info,
            true,
            false,
            true,
            true,
            mesh);
}

std::vector<Bk2PresentationVertex> PresentationVertices(
        const std::vector<TerrainVertex>& vertices) {
    std::vector<Bk2PresentationVertex> result;
    result.reserve(vertices.size());
    for (const TerrainVertex& vertex : vertices) {
        result.push_back(Bk2PresentationVertex{
                vertex.x,
                vertex.y,
                vertex.z,
                vertex.u,
                vertex.v,
                vertex.abgr});
    }
    return result;
}

void PublishPresentationMeshes(
        const std::string& mission_id,
        const TerrainMesh& terrain,
        const WorldObjectMesh& world) {
    bk2::presentation::Reset();
    bk2::presentation::PublishMission(mission_id);
    bk2::presentation::PublishTerrain(
            PresentationVertices(terrain.vertices),
            terrain.triangle_indices,
            terrain.center_x,
            terrain.center_y,
            terrain.center_z,
            terrain.world_size);
    bk2::presentation::PublishWorld(
            PresentationVertices(world.vertices),
            world.triangle_indices);
}

bool RefreshDynamicWorldMeshLocked(bool force) {
    const Bk2PresentationSnapshotInfo info =
            bk2_presentation_snapshot_info();
    const std::vector<AndroidCombatEffect> combat_effects =
            CopyActiveAndroidCombatEffects();
    const std::vector<AndroidDestructionEffect> destruction_effects =
            CopyActiveAndroidDestructionEffects();
    const AndroidWarFogSnapshot war_fog =
            CopyAndroidWarFogSnapshot();
    if (!force &&
        info.generation == g_rendered_presentation_generation &&
        war_fog.generation == g_rendered_war_fog_generation &&
        g_animated_geometry_part_count == 0 &&
        combat_effects.empty() &&
        g_active_combat_effect_count == 0 &&
        destruction_effects.empty() &&
        g_active_destruction_effect_count == 0) {
        return true;
    }
    std::vector<Bk2PresentationEntity> entities(info.entity_count);
    if (!entities.empty()) {
        const size_t copied = bk2_presentation_copy_entities(
                entities.data(),
                entities.size());
        if (copied != entities.size()) {
            return false;
        }
    }

    WorldObjectMesh combined = g_static_world_object_mesh;
    combined.vertices.reserve(
            combined.vertices.size() + entities.size() * 24);
    combined.triangle_indices.reserve(
            combined.triangle_indices.size() + entities.size() * 108);
    g_dynamic_rendered_object_count = 0;
    g_active_unit_indicator_count = 0;
    std::unordered_set<int32_t> visible_corpse_ids;
    for (const Bk2PresentationEntity& entity : entities) {
        const bool dead =
                (entity.flags & BK2_PRESENTATION_ENTITY_DEAD) != 0;
        if ((entity.flags & BK2_PRESENTATION_ENTITY_ALIVE) == 0 && !dead) {
            continue;
        }
        float animation_time_seconds = g_animation_elapsed_seconds;
        if (dead) {
            visible_corpse_ids.insert(entity.id);
            const auto start =
                    g_death_animation_start_seconds
                            .emplace(
                                    entity.id,
                                    g_animation_elapsed_seconds)
                            .first;
            animation_time_seconds =
                    g_animation_elapsed_seconds - start->second;
        }
        const bool selected =
                (entity.flags & BK2_PRESENTATION_ENTITY_SELECTED) != 0;
        const bool targeted =
                (entity.flags & BK2_PRESENTATION_ENTITY_TARGETED) != 0;
        if (selected || targeted) {
            AppendUnitIndicator(
                    &combined,
                    entity,
                    selected ? ArgbToAbgr(0xffffe066u)
                             : ArgbToAbgr(0xffff6a36u));
            ++g_active_unit_indicator_count;
        }
        AppendEntityModel(
                &combined,
                entity,
                selected ? ArgbToAbgr(0xffffe066u)
                         : targeted ? ArgbToAbgr(0xffff8a3du)
                         : ObjectColor(entity.player, false),
                selected || targeted,
                animation_time_seconds);
        ++g_dynamic_rendered_object_count;
    }
    for (auto death = g_death_animation_start_seconds.begin();
         death != g_death_animation_start_seconds.end();) {
        if (visible_corpse_ids.find(death->first) ==
            visible_corpse_ids.end()) {
            death = g_death_animation_start_seconds.erase(death);
        } else {
            ++death;
        }
    }
    const size_t previous_active_combat_effect_count =
            g_active_combat_effect_count;
    for (const AndroidCombatEffect& effect : combat_effects) {
        AppendCombatEffectRibbon(&combined, effect);
    }
    g_active_combat_effect_count = combat_effects.size();
    if (previous_active_combat_effect_count > 0 &&
        g_active_combat_effect_count == 0) {
        PlatformRuntime::instance().log_info(
                "combat_effect_render=cleared");
    }
    const size_t previous_active_destruction_effect_count =
            g_active_destruction_effect_count;
    for (const AndroidDestructionEffect& effect :
         destruction_effects) {
        AppendDestructionEffect(&combined, effect);
    }
    g_active_destruction_effect_count =
            destruction_effects.size();
    if (g_active_destruction_effect_count > 0 &&
        previous_active_destruction_effect_count == 0) {
        PlatformRuntime::instance().log_info(
                "destruction_effect_render=active");
    } else if (
            g_active_destruction_effect_count == 0 &&
            previous_active_destruction_effect_count > 0) {
        PlatformRuntime::instance().log_info(
                "destruction_effect_render=cleared");
    }
    if (war_fog.width >= 2 &&
        war_fog.height >= 2 &&
        war_fog.visibility.size() ==
                static_cast<size_t>(
                        war_fog.width * war_fog.height)) {
        constexpr int kMaxFogQuadsPerAxis = 96;
        const int x_step = std::max(
                1,
                (war_fog.width - 1 + kMaxFogQuadsPerAxis - 1) /
                        kMaxFogQuadsPerAxis);
        const int y_step = std::max(
                1,
                (war_fog.height - 1 + kMaxFogQuadsPerAxis - 1) /
                        kMaxFogQuadsPerAxis);
        std::vector<int> fog_x;
        std::vector<int> fog_y;
        for (int x = 0; x < war_fog.width - 1; x += x_step) {
            fog_x.push_back(x);
        }
        fog_x.push_back(war_fog.width - 1);
        for (int y = 0; y < war_fog.height - 1; y += y_step) {
            fog_y.push_back(y);
        }
        fog_y.push_back(war_fog.height - 1);

        const uint32_t vertex_base =
                static_cast<uint32_t>(combined.vertices.size());
        combined.vertices.reserve(
                combined.vertices.size() +
                fog_x.size() * fog_y.size());
        for (int y : fog_y) {
            for (int x : fog_x) {
                const uint8_t visibility =
                        war_fog.visibility[
                                static_cast<size_t>(
                                        y * war_fog.width + x)];
                const float hidden =
                        1.0f -
                        std::clamp(
                                static_cast<float>(visibility) /
                                        static_cast<float>(
                                                std::max<int>(
                                                        war_fog.visibility_power,
                                                        1)),
                                0.0f,
                                1.0f);
                const uint32_t alpha = static_cast<uint32_t>(
                        std::lround(hidden * 248.0f));
                const float world_x =
                        static_cast<float>(x) * VIS_TILE_SIZE;
                const float world_y =
                        static_cast<float>(y) * VIS_TILE_SIZE;
                combined.vertices.push_back(TerrainVertex{
                        world_x,
                        world_y,
                        TerrainMeshHeightAtLocked(world_x, world_y) + 0.25f,
                        0.0f,
                        0.0f,
                        alpha << 24});
            }
        }

        WorldObjectMesh::Layer fog_layer;
        fog_layer.alpha_blended = true;
        fog_layer.depth_test_always = true;
        fog_layer.triangle_indices.reserve(
                (fog_x.size() - 1) *
                (fog_y.size() - 1) * 6);
        const uint32_t fog_width =
                static_cast<uint32_t>(fog_x.size());
        for (uint32_t y = 0;
             y + 1 < static_cast<uint32_t>(fog_y.size());
             ++y) {
            for (uint32_t x = 0; x + 1 < fog_width; ++x) {
                const uint32_t top_left =
                        vertex_base + y * fog_width + x;
                const uint32_t top_right = top_left + 1;
                const uint32_t bottom_left =
                        top_left + fog_width;
                const uint32_t bottom_right = bottom_left + 1;
                fog_layer.triangle_indices.push_back(top_left);
                fog_layer.triangle_indices.push_back(bottom_left);
                fog_layer.triangle_indices.push_back(top_right);
                fog_layer.triangle_indices.push_back(top_right);
                fog_layer.triangle_indices.push_back(bottom_left);
                fog_layer.triangle_indices.push_back(bottom_right);
            }
        }
        combined.layers.push_back(std::move(fog_layer));
    }
    g_world_object_mesh = std::move(combined);
    RefreshWorldObjectTextureHandles(&g_world_object_mesh);
    g_rendered_presentation_generation = info.generation;
    g_rendered_war_fog_generation = war_fog.generation;
    return RenderBackend().set_world_object_mesh(g_world_object_mesh);
}

bool FocusCameraOnPlayerLocked(int player) {
    const Bk2PresentationSnapshotInfo info =
            bk2_presentation_snapshot_info();
    std::vector<Bk2PresentationEntity> entities(info.entity_count);
    if (!entities.empty() &&
        bk2_presentation_copy_entities(
                entities.data(),
                entities.size()) != entities.size()) {
        return false;
    }

    float min_x = std::numeric_limits<float>::max();
    float min_y = std::numeric_limits<float>::max();
    float max_x = std::numeric_limits<float>::lowest();
    float max_y = std::numeric_limits<float>::lowest();
    float height_sum = 0.0f;
    size_t count = 0;
    for (const Bk2PresentationEntity& entity : entities) {
        if (entity.player != player ||
            (entity.flags & BK2_PRESENTATION_ENTITY_ALIVE) == 0) {
            continue;
        }
        min_x = std::min(min_x, entity.x);
        min_y = std::min(min_y, entity.y);
        max_x = std::max(max_x, entity.x);
        max_y = std::max(max_y, entity.y);
        height_sum += entity.z;
        ++count;
    }
    if (count == 0) {
        return false;
    }

    g_camera.target_x = (min_x + max_x) * 0.5f;
    g_camera.target_y = (min_y + max_y) * 0.5f;
    g_camera.target_z = height_sum / static_cast<float>(count);
    const float camera_from_center_x =
            g_camera.target_x - g_terrain_mesh.center_x;
    const float camera_from_center_y =
            g_camera.target_y - g_terrain_mesh.center_y;
    if (camera_from_center_x * camera_from_center_x +
            camera_from_center_y * camera_from_center_y >
        g_terrain_mesh.world_size * g_terrain_mesh.world_size * 0.01f) {
        g_camera.yaw_radians = std::atan2(
                camera_from_center_x,
                -camera_from_center_y);
    }
    const float formation_span = std::max(max_x - min_x, max_y - min_y);
    g_camera.distance = std::clamp(
            formation_span * 1.8f + 42.0f,
            kInitialCameraMinDistance,
            std::max(
                    g_terrain_mesh.world_size *
                            kCameraMaxTerrainFraction,
                    kInitialCameraMinDistance));

    std::ostringstream report;
    report << "camera_focus=player"
           << "; player=" << player
           << "; units=" << count
           << "; target=" << g_camera.target_x << "," << g_camera.target_y
           << "; distance=" << g_camera.distance;
    PlatformRuntime::instance().log_info(report.str());
    return true;
}

float TerrainMeshHeightAtLocked(float world_x, float world_y) {
    if (g_height_width < 2 ||
        g_height_height < 2 ||
        g_terrain_mesh.vertices.size() !=
                static_cast<size_t>(g_height_width * g_height_height)) {
        return g_camera.target_z;
    }
    const float grid_x = std::clamp(
            world_x / VIS_TILE_SIZE,
            0.0f,
            static_cast<float>(g_height_width - 1));
    const float grid_y = std::clamp(
            world_y / VIS_TILE_SIZE,
            0.0f,
            static_cast<float>(g_height_height - 1));
    const int x0 = std::min(static_cast<int>(grid_x), g_height_width - 2);
    const int y0 = std::min(static_cast<int>(grid_y), g_height_height - 2);
    const int x1 = x0 + 1;
    const int y1 = y0 + 1;
    const float tx = grid_x - static_cast<float>(x0);
    const float ty = grid_y - static_cast<float>(y0);
    const auto height_at = [](int x, int y) {
        return g_terrain_mesh.vertices[
                static_cast<size_t>(y * g_height_width + x)].z;
    };
    const float top =
            height_at(x0, y0) +
            (height_at(x1, y0) - height_at(x0, y0)) * tx;
    const float bottom =
            height_at(x0, y1) +
            (height_at(x1, y1) - height_at(x0, y1)) * tx;
    return top + (bottom - top) * ty;
}

struct ScreenProjectionVec3 {
    float x;
    float y;
    float z;
};

struct ScreenProjectionBasis {
    ScreenProjectionVec3 eye;
    ScreenProjectionVec3 forward;
    ScreenProjectionVec3 right;
    ScreenProjectionVec3 up;
    float tangent = 0.0f;
    float aspect = 1.0f;
};

ScreenProjectionVec3 NormalizeProjectionVector(
        ScreenProjectionVec3 value) {
    const float length = std::sqrt(
            value.x * value.x +
            value.y * value.y +
            value.z * value.z);
    if (length > 0.0001f) {
        value.x /= length;
        value.y /= length;
        value.z /= length;
    }
    return value;
}

ScreenProjectionVec3 CrossProjectionVectors(
        const ScreenProjectionVec3& left,
        const ScreenProjectionVec3& right) {
    return {
            left.y * right.z - left.z * right.y,
            left.z * right.x - left.x * right.z,
            left.x * right.y - left.y * right.x};
}

float DotProjectionVectors(
        const ScreenProjectionVec3& left,
        const ScreenProjectionVec3& right) {
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

bool BuildScreenProjectionBasisLocked(
        uint32_t viewport_width,
        uint32_t viewport_height,
        ScreenProjectionBasis* basis) {
    if (viewport_width == 0 ||
        viewport_height == 0 ||
        basis == nullptr) {
        return false;
    }

    const float horizontal_distance =
            g_camera.distance * std::cos(g_camera.pitch_radians);
    basis->eye = {
            g_camera.target_x +
                    std::sin(g_camera.yaw_radians) * horizontal_distance,
            g_camera.target_y -
                    std::cos(g_camera.yaw_radians) * horizontal_distance,
            g_camera.target_z +
                    std::sin(g_camera.pitch_radians) * g_camera.distance};
    basis->forward = NormalizeProjectionVector({
            g_camera.target_x - basis->eye.x,
            g_camera.target_y - basis->eye.y,
            g_camera.target_z - basis->eye.z});
    basis->right = NormalizeProjectionVector(CrossProjectionVectors(
            {0.0f, 0.0f, 1.0f},
            basis->forward));
    basis->up = NormalizeProjectionVector(CrossProjectionVectors(
            basis->forward,
            basis->right));
    basis->tangent =
            std::tan(48.0f * 0.5f * 3.14159265358979323846f / 180.0f);
    basis->aspect =
            static_cast<float>(viewport_width) /
            static_cast<float>(viewport_height);
    return true;
}

bool ProjectEntityToScreenLocked(
        const Bk2PresentationEntity& entity,
        const ScreenProjectionBasis& basis,
        uint32_t viewport_width,
        uint32_t viewport_height,
        float* screen_x,
        float* screen_y) {
    if (screen_x == nullptr || screen_y == nullptr) {
        return false;
    }
    const ScreenProjectionVec3 relative = {
            entity.x - basis.eye.x,
            entity.y - basis.eye.y,
            entity.z + 1.0f - basis.eye.z};
    const float depth = DotProjectionVectors(
            relative,
            basis.forward);
    if (depth <= 0.001f) {
        return false;
    }
    const float ndc_x =
            DotProjectionVectors(relative, basis.right) /
            (depth * basis.tangent * basis.aspect);
    const float ndc_y =
            DotProjectionVectors(relative, basis.up) /
            (depth * basis.tangent);
    *screen_x = (ndc_x + 1.0f) * 0.5f * viewport_width;
    *screen_y = (1.0f - ndc_y) * 0.5f * viewport_height;
    return true;
}

int FindEntityNearScreenLocked(
        float screen_x,
        float screen_y,
        uint32_t viewport_width,
        uint32_t viewport_height,
        int player,
        bool invert_player_match,
        float radius_pixels) {
    if (radius_pixels <= 0.0f) {
        return -1;
    }
    ScreenProjectionBasis basis;
    if (!BuildScreenProjectionBasisLocked(
                viewport_width,
                viewport_height,
                &basis)) {
        return -1;
    }
    const Bk2PresentationSnapshotInfo snapshot =
            bk2_presentation_snapshot_info();
    std::vector<Bk2PresentationEntity> entities(snapshot.entity_count);
    if (!entities.empty() &&
        bk2_presentation_copy_entities(
                entities.data(),
                entities.size()) != entities.size()) {
        return -1;
    }

    const float max_distance_squared = radius_pixels * radius_pixels;
    float best_distance_squared = std::numeric_limits<float>::max();
    int best_id = -1;
    for (const Bk2PresentationEntity& entity : entities) {
        const bool player_matches = entity.player == player;
        if ((invert_player_match ? player_matches : !player_matches) ||
            (entity.flags & BK2_PRESENTATION_ENTITY_ALIVE) == 0) {
            continue;
        }
        float projected_x = 0.0f;
        float projected_y = 0.0f;
        if (!ProjectEntityToScreenLocked(
                    entity,
                    basis,
                    viewport_width,
                    viewport_height,
                    &projected_x,
                    &projected_y)) {
            continue;
        }
        const float delta_x = projected_x - screen_x;
        const float delta_y = projected_y - screen_y;
        const float distance_squared =
                delta_x * delta_x + delta_y * delta_y;
        if (distance_squared <= max_distance_squared &&
            distance_squared < best_distance_squared) {
            best_distance_squared = distance_squared;
            best_id = entity.id;
        }
    }
    return best_id;
}

bool ScreenToTerrainLocked(
        float screen_x,
        float screen_y,
        uint32_t viewport_width,
        uint32_t viewport_height,
        float* world_x,
        float* world_y) {
    if (world_x == nullptr ||
        world_y == nullptr ||
        viewport_width == 0 ||
        viewport_height == 0 ||
        g_height_width < 2 ||
        g_height_height < 2) {
        return false;
    }

    struct Vec3 {
        float x;
        float y;
        float z;
    };
    const auto normalize = [](Vec3 value) {
        const float length = std::sqrt(
                value.x * value.x +
                value.y * value.y +
                value.z * value.z);
        if (length > 0.0001f) {
            value.x /= length;
            value.y /= length;
            value.z /= length;
        }
        return value;
    };
    const auto cross = [](const Vec3& left, const Vec3& right) {
        return Vec3{
                left.y * right.z - left.z * right.y,
                left.z * right.x - left.x * right.z,
                left.x * right.y - left.y * right.x};
    };

    const float horizontal_distance =
            g_camera.distance * std::cos(g_camera.pitch_radians);
    const Vec3 eye = {
            g_camera.target_x +
                    std::sin(g_camera.yaw_radians) * horizontal_distance,
            g_camera.target_y -
                    std::cos(g_camera.yaw_radians) * horizontal_distance,
            g_camera.target_z +
                    std::sin(g_camera.pitch_radians) * g_camera.distance};
    const Vec3 forward = normalize(Vec3{
            g_camera.target_x - eye.x,
            g_camera.target_y - eye.y,
            g_camera.target_z - eye.z});
    const Vec3 world_up = {0.0f, 0.0f, 1.0f};
    const Vec3 right = normalize(cross(world_up, forward));
    const Vec3 camera_up = normalize(cross(forward, right));
    const float ndc_x =
            screen_x / static_cast<float>(viewport_width) * 2.0f - 1.0f;
    const float ndc_y =
            1.0f - screen_y / static_cast<float>(viewport_height) * 2.0f;
    const float tangent = std::tan(48.0f * 0.5f * 3.14159265358979323846f / 180.0f);
    const float aspect =
            static_cast<float>(viewport_width) /
            static_cast<float>(viewport_height);
    const Vec3 ray = normalize(Vec3{
            forward.x + right.x * ndc_x * tangent * aspect +
                    camera_up.x * ndc_y * tangent,
            forward.y + right.y * ndc_x * tangent * aspect +
                    camera_up.y * ndc_y * tangent,
            forward.z + right.z * ndc_x * tangent * aspect +
                    camera_up.z * ndc_y * tangent});
    if (ray.z >= -0.0001f) {
        return false;
    }

    float height = g_camera.target_z;
    float intersection_x = 0.0f;
    float intersection_y = 0.0f;
    for (int iteration = 0; iteration < 5; ++iteration) {
        const float distance = (height - eye.z) / ray.z;
        if (distance <= 0.0f) {
            return false;
        }
        intersection_x = eye.x + ray.x * distance;
        intersection_y = eye.y + ray.y * distance;
        height = TerrainMeshHeightAtLocked(intersection_x, intersection_y);
    }

    const float max_x =
            static_cast<float>(g_height_width - 1) * VIS_TILE_SIZE;
    const float max_y =
            static_cast<float>(g_height_height - 1) * VIS_TILE_SIZE;
    if (intersection_x < 0.0f ||
        intersection_y < 0.0f ||
        intersection_x > max_x ||
        intersection_y > max_y) {
        return false;
    }
    *world_x = intersection_x;
    *world_y = intersection_y;
    return true;
}

bool LoadTextureImmediately(
        const NDb::STexture* texture_desc,
        CObj<NGfx::CTexture>* texture) {
    if (texture_desc == nullptr || texture == nullptr) {
        return false;
    }
    NDb::STexture* mutable_desc =
            const_cast<NDb::STexture*>(texture_desc);
    const bool was_instant_load = mutable_desc->bInstantLoad;
    mutable_desc->bInstantLoad = true;
    CObj<NGScene::CFileTexture> texture_node =
            new NGScene::CFileTexture();
    GUID uid;
    Zero(uid);
    texture_node->SetKey(NGScene::GetKey(texture_desc), uid);
    CDGPtr<NGScene::CFileTexture> texture_ref(texture_node.GetPtr());
    texture_ref.Refresh();
    *texture = texture_ref->GetValue();
    mutable_desc->bInstantLoad = was_instant_load;
    return IsValid(*texture);
}

uint16_t ModelTextureHandle(const std::string& texture_path) {
    if (texture_path.empty()) {
        return UINT16_MAX;
    }
    auto cached = g_model_textures.find(texture_path);
    if (cached == g_model_textures.end()) {
        CPtr<NDb::STexture> texture_desc = new NDb::STexture();
        NDb::CResourceHelper::SetDBID(
                texture_desc.GetPtr(),
                CDBID("Android/OriginalModelTexture.xdb"));
        NDb::CResourceHelper::SetLoaded(texture_desc.GetPtr());
        texture_desc->szDestName = texture_path.c_str();
        texture_desc->eType = NDb::STexture::REGULAR;
        texture_desc->eAddrType = NDb::STexture::WRAP;
        texture_desc->bInstantLoad = true;
        CObj<NGfx::CTexture> texture;
        if (!LoadTextureImmediately(texture_desc.GetPtr(), &texture)) {
            g_model_textures.emplace(
                    texture_path,
                    CObj<NGfx::CTexture>());
            return UINT16_MAX;
        }
        cached = g_model_textures.emplace(
                texture_path,
                texture).first;
        ++g_model_texture_count;
    }
    if (!IsValid(cached->second)) {
        return UINT16_MAX;
    }
    if (texture_path == kMuzzleFlashTexture ||
        texture_path.find(
                "Scene/TexAndMats/All/Effects/Destructions/Fire/") == 0) {
        ConfigureLegacyLuminanceAlphaTexture(cached->second);
    }
    EnsureLegacyTextureMipChainUploaded(cached->second);
    return LegacyTextureHandleIndex(cached->second);
}

void RefreshWorldObjectTextureHandles(WorldObjectMesh* mesh) {
    if (mesh == nullptr) {
        return;
    }
    for (WorldObjectMesh::Layer& layer : mesh->layers) {
        layer.texture_handle =
                ModelTextureHandle(layer.texture_path);
        if (layer.alpha_blended &&
            (layer.texture_path == kInfantryTraceTexture ||
             layer.texture_path == kMechanizedTraceTexture) &&
            !g_combat_effect_trace_texture_logged) {
            PlatformRuntime::instance().log_info(
                    std::string("combat_effect_trace_texture=") +
                    (layer.texture_handle == UINT16_MAX
                             ? "unavailable"
                             : "ready") +
                    "; path=" + layer.texture_path);
            g_combat_effect_trace_texture_logged = true;
        }
        if (layer.texture_path == kMuzzleFlashTexture &&
            !g_muzzle_flash_texture_logged) {
            PlatformRuntime::instance().log_info(
                    std::string("muzzle_flash_texture=") +
                    (layer.texture_handle == UINT16_MAX
                             ? "unavailable"
                             : "ready") +
                    "; path=" + layer.texture_path);
            g_muzzle_flash_texture_logged = true;
        }
        if ((layer.texture_path.find(
                     "Scene/TexAndMats/All/Effects/Destructions/Fire/") == 0 ||
             layer.texture_path.find(
                     "Scene/TexAndMats/All/Effects/Explosions/GroundExplosion/") == 0) &&
            !g_destruction_effect_texture_logged) {
            PlatformRuntime::instance().log_info(
                    std::string("destruction_effect_texture=") +
                    (layer.texture_handle == UINT16_MAX
                             ? "unavailable"
                             : "ready") +
                    "; path=" + layer.texture_path);
            g_destruction_effect_texture_logged = true;
        }
    }
}

bool LoadTerrainTexture(const NDb::SMapInfo* map) {
    if (map == nullptr || !map->pMiniMap || !map->pMiniMap->pTexture) {
        return false;
    }

    const NDb::STexture* texture_desc = map->pMiniMap->pTexture;
    if (!LoadTextureImmediately(texture_desc, &g_terrain_texture)) {
        return false;
    }

    g_terrain_texture_path = texture_desc->szDestName.c_str();
    return true;
}

size_t LoadTerrainLayerTextures(
        const NDb::SMapInfo* map,
        TerrainMesh* mesh) {
    g_terrain_layer_textures.clear();
    g_terrain_layer_texture_paths.clear();
    g_terrain_layer_texture_count = 0;
    if (map == nullptr || mesh == nullptr || !map->pTerraSet) {
        return 0;
    }
    const NDb::STGTerraSet* terra_set = map->pTerraSet.GetPtr();
    if (terra_set == nullptr) {
        return 0;
    }

    g_terrain_layer_textures.resize(mesh->layers.size());
    g_terrain_layer_texture_paths.resize(mesh->layers.size());
    for (size_t layer_index = 0;
         layer_index < mesh->layers.size();
         ++layer_index) {
        TerrainLayer& layer = mesh->layers[layer_index];
        if (layer.terrain_type_index < 0 ||
            layer.terrain_type_index >=
                    static_cast<int>(terra_set->terraTypes.size())) {
            continue;
        }
        const NDb::STGTerraType* type =
                terra_set->terraTypes[layer.terrain_type_index].GetPtr();
        if (type == nullptr || !type->pMaterial ||
            !type->pMaterial->pTexture) {
            continue;
        }
        const NDb::STexture* texture_desc = type->pMaterial->pTexture;
        CObj<NGfx::CTexture> texture;
        if (!LoadTextureImmediately(texture_desc, &texture)) {
            continue;
        }
        ConfigureLegacyTerrainTexture(texture);
        g_terrain_layer_textures[layer_index] = texture;
        g_terrain_layer_texture_paths[layer_index] =
                texture_desc->szDestName.c_str();
        ++g_terrain_layer_texture_count;
    }
    return g_terrain_layer_texture_count;
}

bool RefreshRenderResourcesLocked() {
    if (g_terrain_mesh.vertices.empty()) {
        return false;
    }
    g_terrain_mesh.texture_handle = UINT16_MAX;
    if (IsValid(g_terrain_texture)) {
        EnsureLegacyTextureUploaded(g_terrain_texture, 0);
        g_terrain_mesh.texture_handle =
                LegacyTextureHandleIndex(g_terrain_texture);
    }
    for (size_t index = 0;
         index < g_terrain_mesh.layers.size();
         ++index) {
        g_terrain_mesh.layers[index].texture_handle = UINT16_MAX;
        if (index >= g_terrain_layer_textures.size() ||
            !IsValid(g_terrain_layer_textures[index])) {
            continue;
        }
        EnsureLegacyTextureUploaded(g_terrain_layer_textures[index], 0);
        g_terrain_mesh.layers[index].texture_handle =
                LegacyTextureHandleIndex(g_terrain_layer_textures[index]);
    }
    if (!RenderBackend().set_terrain_mesh(g_terrain_mesh)) {
        return false;
    }
    RefreshWorldObjectTextureHandles(&g_world_object_mesh);
    if (!g_world_object_mesh.vertices.empty() &&
        !RenderBackend().set_world_object_mesh(g_world_object_mesh)) {
        return false;
    }
    return true;
}

void ApplyCameraLocked() {
    RenderBackend().set_terrain_camera(g_camera);
}

}  // namespace

bool InitializeSinglePlayerRuntime() {
    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    if (g_ready) {
        return true;
    }
    if (!IsLegacyDatabaseOpen()) {
        g_last_error = "legacy_database_not_open";
        return false;
    }

    const MissionRuntimeResult mission = StartConfiguredMissionState();
    if (!mission.ok || mission.state.mission_id.empty()) {
        g_last_error = mission.error.empty()
                ? "first_campaign_mission_missing"
                : mission.error;
        return false;
    }

    const NDb::SMapInfo* map =
            NDb::Get<NDb::SMapInfo>(CDBID(mission.state.mission_id.c_str()));
    if (map == nullptr) {
        g_last_error = "mission_map_db_resource_missing";
        return false;
    }

    STerrainInfo terrain_info;
    if (!ReadTerrainInfo(map, &terrain_info, &g_last_error)) {
        return false;
    }

    TerrainMesh mesh;
    if (!BuildTerrainMesh(terrain_info, &mesh, &g_last_error)) {
        return false;
    }
    BuildTerrainLayers(map, terrain_info, &mesh);
    WorldObjectMesh presentation_world_mesh;
    BuildPresentationStaticWorldMesh(map, terrain_info, &presentation_world_mesh);
    if (LoadTerrainLayerTextures(map, &mesh) == 0) {
        LoadTerrainTexture(map);
    }

    g_camera.target_x = mesh.center_x;
    g_camera.target_y = mesh.center_y;
    g_camera.target_z = mesh.center_z;
    g_camera.yaw_radians = 0.72f;
    g_camera.pitch_radians = 0.92f;
    g_camera.distance = std::max(mesh.world_size * 1.05f, kMinCameraDistance);

    g_terrain_mesh = std::move(mesh);
    g_static_world_object_mesh = std::move(presentation_world_mesh);
    g_world_object_mesh = g_static_world_object_mesh;
    PublishPresentationMeshes(
            mission.state.mission_id,
            g_terrain_mesh,
            g_static_world_object_mesh);
    if (!InitializeLegacyGameRuntime(
                map,
                terrain_info,
                mission.state.campaign_index,
                mission.state.chapter_index,
                mission.state.difficulty,
                &g_last_error)) {
        return false;
    }
    if (!RefreshDynamicWorldMeshLocked(true)) {
        ShutdownLegacyGameRuntime();
        g_last_error = "dynamic_world_snapshot_failed";
        return false;
    }
    FocusCameraOnPlayerLocked(0);
    if (!RefreshRenderResourcesLocked()) {
        ShutdownLegacyGameRuntime();
        g_last_error = RenderBackend().last_error().empty()
                ? "terrain_renderer_rejected_mesh"
                : RenderBackend().last_error();
        return false;
    }
    ApplyCameraLocked();

    g_mission_id = mission.state.mission_id;
    const PortPaths paths = GetPortPaths();
    const std::string snapshot_path =
            JoinHostPath(paths.log_root(), "presentation_snapshot.json");
    g_presentation_snapshot_written =
            bk2_presentation_write_json(snapshot_path.c_str()) != 0;
    g_last_error.clear();
    g_user_paused = false;
    g_ready = true;
    return true;
}

void RefreshSinglePlayerRenderResources() {
    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    if (!g_ready) {
        return;
    }
    if (!RefreshRenderResourcesLocked()) {
        g_last_error = RenderBackend().last_error().empty()
                ? "terrain_render_resource_refresh_failed"
                : RenderBackend().last_error();
        return;
    }
    ApplyCameraLocked();
}

void TickSinglePlayerRuntime(uint32_t elapsed_millis) {
    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    if (!g_ready || g_user_paused) {
        return;
    }
    TickLegacyGameRuntime(elapsed_millis);
    g_animation_elapsed_seconds +=
            static_cast<float>(elapsed_millis) * 0.001f;
    if (g_animation_elapsed_seconds > 3600.0f) {
        g_animation_elapsed_seconds =
                std::fmod(g_animation_elapsed_seconds, 3600.0f);
    }
    if (!RefreshDynamicWorldMeshLocked(false)) {
        g_last_error = "dynamic_world_render_refresh_failed";
    }
}

void ShutdownSinglePlayerRuntime() {
    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    ShutdownLegacyGameRuntime();
    RenderBackend().clear_terrain_mesh();
    RenderBackend().clear_world_object_mesh();
    g_terrain_texture = 0;
    g_terrain_layer_textures.clear();
    g_terrain_layer_texture_paths.clear();
    g_terrain_mesh = TerrainMesh();
    g_static_world_object_mesh = WorldObjectMesh();
    g_world_object_mesh = WorldObjectMesh();
    bk2::presentation::Reset();
    g_ready = false;
    g_user_paused = false;
    g_mission_id.clear();
    g_map_path.clear();
    g_terrain_texture_path.clear();
    g_terrain_layer_count = 0;
    g_terrain_layer_texture_count = 0;
    g_terrain_type_map.clear();
    g_terrain_type_colors.clear();
    g_terrain_type_width = 0;
    g_terrain_type_height = 0;
    g_converted_geometry_instance_count = 0;
    g_converted_geometry_fallback_count = 0;
    g_animated_geometry_part_count = 0;
    g_move_animation_instance_count = 0;
    g_attack_animation_instance_count = 0;
    g_death_animation_instance_count = 0;
    g_lying_idle_animation_instance_count = 0;
    g_lying_move_animation_instance_count = 0;
    g_lying_attack_animation_instance_count = 0;
    g_combat_effect_render_count = 0;
    g_active_combat_effect_count = 0;
    g_active_destruction_effect_count = 0;
    g_active_unit_indicator_count = 0;
    g_combat_effect_trace_texture_logged = false;
    g_muzzle_flash_texture_logged = false;
    g_destruction_effect_texture_logged = false;
    g_touch_command_mode = TouchCommandMode::Contextual;
    ResetPendingTrenchCommandLocked();
    ResetFriendlyDoubleTapLocked();
    g_converted_geometries.clear();
    g_missing_converted_geometries.clear();
    g_move_converted_geometries.clear();
    g_missing_move_converted_geometries.clear();
    g_attack_converted_geometries.clear();
    g_missing_attack_converted_geometries.clear();
    g_death_converted_geometries.clear();
    g_missing_death_converted_geometries.clear();
    g_lying_idle_converted_geometries.clear();
    g_missing_lying_idle_converted_geometries.clear();
    g_lying_move_converted_geometries.clear();
    g_missing_lying_move_converted_geometries.clear();
    g_lying_attack_converted_geometries.clear();
    g_missing_lying_attack_converted_geometries.clear();
    g_death_animation_start_seconds.clear();
    g_stats_geometry_index.clear();
    g_stats_geometry_variants.clear();
    g_stats_geometry_index_loaded = false;
    g_model_textures.clear();
    g_model_texture_count = 0;
    g_static_fallback_stats_paths.clear();
    g_dynamic_fallback_stats_hashes.clear();
    g_height_width = 0;
    g_height_height = 0;
    g_triangle_count = 0;
    g_map_object_count = 0;
    g_scenario_object_count = 0;
    g_rendered_object_count = 0;
    g_dynamic_rendered_object_count = 0;
    g_presentation_snapshot_written = false;
    g_rendered_presentation_generation = 0;
    g_rendered_war_fog_generation = 0;
    g_animation_elapsed_seconds = 0.0f;
}

void PanSinglePlayerCamera(float delta_x_pixels, float delta_y_pixels) {
    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    if (!g_ready) {
        return;
    }
    const float scale =
            std::max(g_camera.distance, kMinCameraDistance) * 0.0015f;
    const float right_x = std::cos(g_camera.yaw_radians);
    const float right_y = std::sin(g_camera.yaw_radians);
    const float forward_x = -right_y;
    const float forward_y = right_x;
    g_camera.target_x -= right_x * delta_x_pixels * scale;
    g_camera.target_y -= right_y * delta_x_pixels * scale;
    g_camera.target_x += forward_x * delta_y_pixels * scale;
    g_camera.target_y += forward_y * delta_y_pixels * scale;
    ApplyCameraLocked();
}

void ZoomSinglePlayerCamera(float scale) {
    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    if (!g_ready || scale <= 0.0f) {
        return;
    }
    const float maximum_distance = std::max(
            kMinCameraDistance,
            g_terrain_mesh.world_size * kCameraMaxTerrainFraction);
    g_camera.distance = std::clamp(
            g_camera.distance /
                    std::max(0.25f, std::min(scale, 4.0f)),
            kMinCameraDistance,
            maximum_distance);
    ApplyCameraLocked();
}

void RotateSinglePlayerCamera(float delta_radians) {
    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    if (!g_ready) {
        return;
    }
    g_camera.yaw_radians += delta_radians;
    ApplyCameraLocked();
}

bool HandleSinglePlayerSelectionRect(
        float start_x,
        float start_y,
        float end_x,
        float end_y,
        uint32_t viewport_width,
        uint32_t viewport_height) {
    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    if (!g_ready ||
        g_user_paused ||
        g_touch_command_mode != TouchCommandMode::Contextual) {
        return false;
    }
    const float left = std::min(start_x, end_x);
    const float right = std::max(start_x, end_x);
    const float top = std::min(start_y, end_y);
    const float bottom = std::max(start_y, end_y);
    if (right - left < 16.0f || bottom - top < 16.0f) {
        return false;
    }
    ScreenProjectionBasis basis;
    if (!BuildScreenProjectionBasisLocked(
                viewport_width,
                viewport_height,
                &basis)) {
        return false;
    }
    const Bk2PresentationSnapshotInfo snapshot =
            bk2_presentation_snapshot_info();
    std::vector<Bk2PresentationEntity> entities(snapshot.entity_count);
    if (!entities.empty() &&
        bk2_presentation_copy_entities(
                entities.data(),
                entities.size()) != entities.size()) {
        return false;
    }
    struct SelectionCandidate {
        int id;
        float screen_x;
        float screen_y;
    };
    std::vector<SelectionCandidate> candidates;
    for (const Bk2PresentationEntity& entity : entities) {
        if (entity.player != 0 ||
            (entity.flags & BK2_PRESENTATION_ENTITY_ALIVE) == 0 ||
            (entity.flags & BK2_PRESENTATION_ENTITY_SELECTABLE) == 0) {
            continue;
        }
        float projected_x = 0.0f;
        float projected_y = 0.0f;
        if (!ProjectEntityToScreenLocked(
                    entity,
                    basis,
                    viewport_width,
                    viewport_height,
                    &projected_x,
                    &projected_y) ||
            projected_x < left ||
            projected_x > right ||
            projected_y < top ||
            projected_y > bottom) {
            continue;
        }
        candidates.push_back({
                entity.id,
                projected_x,
                projected_y});
    }
    std::sort(
            candidates.begin(),
            candidates.end(),
            [](const SelectionCandidate& left_candidate,
               const SelectionCandidate& right_candidate) {
                if (left_candidate.screen_y != right_candidate.screen_y) {
                    return left_candidate.screen_y <
                            right_candidate.screen_y;
                }
                return left_candidate.screen_x <
                        right_candidate.screen_x;
            });
    std::vector<int> unit_ids;
    unit_ids.reserve(candidates.size());
    for (const SelectionCandidate& candidate : candidates) {
        unit_ids.push_back(candidate.id);
    }
    const int selected_count = SelectLegacyUnits(unit_ids, 0);
    std::ostringstream report;
    report << "player_drag_selection=" << selected_count
           << "; rect=" << left << "," << top
           << "-" << right << "," << bottom;
    PlatformRuntime::instance().log_info(report.str());
    return selected_count > 0 &&
            RefreshDynamicWorldMeshLocked(true);
}

bool HandleSinglePlayerTap(
        float screen_x,
        float screen_y,
        uint32_t viewport_width,
        uint32_t viewport_height) {
    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    if (!g_ready || g_user_paused) {
        return false;
    }
    constexpr float kEntityTapRadiusPixels = 52.0f;
    if (g_touch_command_mode == TouchCommandMode::Attack) {
        constexpr float kAttackCommandRadiusPixels = 96.0f;
        const int hostile_unit = FindEntityNearScreenLocked(
                screen_x,
                screen_y,
                viewport_width,
                viewport_height,
                0,
                true,
                kAttackCommandRadiusPixels);
        if (hostile_unit < 0 ||
            !AttackSelectedLegacyUnit(hostile_unit)) {
            PlatformRuntime::instance().log_info(
                    "player_touch_command=attack; result=no_target");
            return false;
        }
        g_touch_command_mode = TouchCommandMode::Contextual;
        ResetPendingTrenchCommandLocked();
        PlatformRuntime::instance().log_info(
                std::string(
                        "player_touch_command=attack; result=issued; target=") +
                std::to_string(hostile_unit));
        return RefreshDynamicWorldMeshLocked(true);
    }
    if (g_touch_command_mode == TouchCommandMode::Move) {
        float world_x = 0.0f;
        float world_y = 0.0f;
        if (!ScreenToTerrainLocked(
                    screen_x,
                    screen_y,
                    viewport_width,
                    viewport_height,
                    &world_x,
                    &world_y) ||
            !MoveSelectedLegacyUnit(world_x, world_y)) {
            PlatformRuntime::instance().log_info(
                    "player_touch_command=move; result=invalid_target");
            return false;
        }
        g_touch_command_mode = TouchCommandMode::Contextual;
        ResetPendingTrenchCommandLocked();
        PlatformRuntime::instance().log_info(
                std::string(
                        "player_touch_command=move; result=issued; target=") +
                std::to_string(world_x) + "," + std::to_string(world_y));
        return RefreshDynamicWorldMeshLocked(true);
    }
    if (g_touch_command_mode == TouchCommandMode::Rotate ||
        g_touch_command_mode == TouchCommandMode::Spyglass) {
        float world_x = 0.0f;
        float world_y = 0.0f;
        const int user_action =
                g_touch_command_mode == TouchCommandMode::Rotate ? 6 : 40;
        if (!ScreenToTerrainLocked(
                    screen_x,
                    screen_y,
                    viewport_width,
                    viewport_height,
                    &world_x,
                    &world_y) ||
            !PerformSelectedLegacyUnitPointAction(
                    user_action,
                    world_x,
                    world_y)) {
            PlatformRuntime::instance().log_info(
                    std::string("player_touch_point_action=") +
                    std::to_string(user_action) +
                    "; result=invalid_target");
            return false;
        }
        g_touch_command_mode = TouchCommandMode::Contextual;
        ResetPendingTrenchCommandLocked();
        PlatformRuntime::instance().log_info(
                std::string("player_touch_point_action=") +
                std::to_string(user_action) +
                "; result=issued; target=" +
                std::to_string(world_x) + "," +
                std::to_string(world_y));
        return RefreshDynamicWorldMeshLocked(true);
    }
    if (g_touch_command_mode == TouchCommandMode::ClearMines ||
        g_touch_command_mode == TouchCommandMode::PlaceMines) {
        float world_x = 0.0f;
        float world_y = 0.0f;
        const int user_action =
                g_touch_command_mode == TouchCommandMode::ClearMines
                ? NDb::USER_ACTION_ENGINEER_CLEAR_MINES
                : NDb::USER_ACTION_ENGINEER_PLACE_MINES;
        if (!ScreenToTerrainLocked(
                    screen_x,
                    screen_y,
                    viewport_width,
                    viewport_height,
                    &world_x,
                    &world_y) ||
            !PerformSelectedLegacyUnitPointAction(
                    user_action,
                    world_x,
                    world_y)) {
            PlatformRuntime::instance().log_info(
                    std::string("player_touch_point_action=") +
                    std::to_string(user_action) +
                    "; result=invalid_target");
            return false;
        }
        g_touch_command_mode = TouchCommandMode::Contextual;
        ResetPendingTrenchCommandLocked();
        PlatformRuntime::instance().log_info(
                std::string("player_touch_point_action=") +
                std::to_string(user_action) +
                "; result=issued; target=" +
                std::to_string(world_x) + "," +
                std::to_string(world_y));
        return RefreshDynamicWorldMeshLocked(true);
    }
    if (g_touch_command_mode == TouchCommandMode::BuildTrenches) {
        float world_x = 0.0f;
        float world_y = 0.0f;
        if (!ScreenToTerrainLocked(
                    screen_x,
                    screen_y,
                    viewport_width,
                    viewport_height,
                    &world_x,
                    &world_y)) {
            PlatformRuntime::instance().log_info(
                    "player_touch_segment_action=21; "
                    "result=invalid_target");
            return false;
        }
        const int selected_unit_id = SelectedLegacyUnitId();
        if (!CanSelectedLegacyUnitPerformAction(
                    NDb::USER_ACTION_ENGINEER_BUILD_ENTRENCHMENT)) {
            ResetPendingTrenchCommandLocked();
            PlatformRuntime::instance().log_info(
                    "player_touch_segment_action=21; "
                    "result=action_unavailable");
            return false;
        }
        if (!g_trench_first_point_defined ||
            g_trench_first_unit_id != selected_unit_id) {
            g_trench_first_point_defined = true;
            g_trench_first_world_x = world_x;
            g_trench_first_world_y = world_y;
            g_trench_first_unit_id = selected_unit_id;
            PlatformRuntime::instance().log_info(
                    std::string("player_touch_segment_action=21; "
                                "result=first_point; target=") +
                    std::to_string(world_x) + "," +
                    std::to_string(world_y));
            return true;
        }
        if (!PerformSelectedLegacyUnitSegmentAction(
                    NDb::USER_ACTION_ENGINEER_BUILD_ENTRENCHMENT,
                    g_trench_first_world_x,
                    g_trench_first_world_y,
                    world_x,
                    world_y)) {
            PlatformRuntime::instance().log_info(
                    "player_touch_segment_action=21; "
                    "result=invalid_second_point");
            return false;
        }
        PlatformRuntime::instance().log_info(
                std::string("player_touch_segment_action=21; "
                            "result=issued; start=") +
                std::to_string(g_trench_first_world_x) + "," +
                std::to_string(g_trench_first_world_y) +
                "; end=" + std::to_string(world_x) + "," +
                std::to_string(world_y));
        g_touch_command_mode = TouchCommandMode::Contextual;
        ResetPendingTrenchCommandLocked();
        return RefreshDynamicWorldMeshLocked(true);
    }
    const int friendly_unit = FindEntityNearScreenLocked(
            screen_x,
            screen_y,
            viewport_width,
            viewport_height,
            0,
            false,
            kEntityTapRadiusPixels);
    if (friendly_unit >= 0) {
        const uint64_t now_millis =
                PlatformRuntime::instance().monotonic_millis();
        const float tap_delta_x =
                screen_x - g_last_friendly_tap_x;
        const float tap_delta_y =
                screen_y - g_last_friendly_tap_y;
        const bool double_tap =
                friendly_unit == g_last_friendly_tap_unit_id &&
                now_millis >= g_last_friendly_tap_millis &&
                now_millis - g_last_friendly_tap_millis <= 450 &&
                tap_delta_x * tap_delta_x +
                        tap_delta_y * tap_delta_y <=
                        64.0f * 64.0f;
        const int selected_count = double_tap
                ? SelectLegacyUnitsByTypeNear(
                        friendly_unit,
                        std::max(
                                g_camera.distance * 1.35f,
                                VIS_TILE_SIZE * 8.0f),
                        0)
                : (SelectLegacyUnit(friendly_unit, 0) ? 1 : 0);
        g_last_friendly_tap_millis = now_millis;
        g_last_friendly_tap_unit_id = friendly_unit;
        g_last_friendly_tap_x = screen_x;
        g_last_friendly_tap_y = screen_y;
        if (selected_count <= 0) {
            return false;
        }
        std::ostringstream report;
        report << "player_tap=select_screen"
               << "; unit=" << friendly_unit
               << "; count=" << selected_count
               << "; double_tap="
               << (double_tap ? "true" : "false")
               << "; radius_pixels=" << kEntityTapRadiusPixels;
        PlatformRuntime::instance().log_info(report.str());
        return RefreshDynamicWorldMeshLocked(true);
    }
    if (SelectedLegacyUnitId() >= 0) {
        const int hostile_unit = FindEntityNearScreenLocked(
                screen_x,
                screen_y,
                viewport_width,
                viewport_height,
                0,
                true,
                kEntityTapRadiusPixels);
        if (hostile_unit >= 0 &&
            AttackSelectedLegacyUnit(hostile_unit)) {
            std::ostringstream report;
            report << "player_tap=attack_screen"
                   << "; target=" << hostile_unit
                   << "; radius_pixels=" << kEntityTapRadiusPixels;
            PlatformRuntime::instance().log_info(report.str());
            return RefreshDynamicWorldMeshLocked(true);
        }
    }
    float world_x = 0.0f;
    float world_y = 0.0f;
    if (!ScreenToTerrainLocked(
                screen_x,
                screen_y,
                viewport_width,
                viewport_height,
                &world_x,
                &world_y)) {
        std::ostringstream report;
        report << "player_tap=outside_terrain"
               << "; screen=" << screen_x << "," << screen_y
               << "; viewport=" << viewport_width << "x" << viewport_height;
        PlatformRuntime::instance().log_info(report.str());
        return false;
    }
    {
        std::ostringstream report;
        report << "player_tap=terrain"
               << "; screen=" << screen_x << "," << screen_y
               << "; world=" << world_x << "," << world_y;
        PlatformRuntime::instance().log_info(report.str());
    }
    const float world_units_per_pixel =
            2.0f *
            g_camera.distance *
            std::tan(48.0f * 0.5f * 3.14159265358979323846f / 180.0f) /
            static_cast<float>(std::max(viewport_height, 1u));
    const float selection_radius =
            std::max(world_units_per_pixel * 42.0f, VIS_TILE_SIZE * 1.5f);
    const int selected_unit = SelectLegacyUnitNear(
                world_x,
                world_y,
                selection_radius,
                0);
    if (selected_unit >= 0) {
        std::ostringstream report;
        report << "player_tap=select"
               << "; unit=" << selected_unit
               << "; radius=" << selection_radius;
        PlatformRuntime::instance().log_info(report.str());
        return RefreshDynamicWorldMeshLocked(true);
    }
    if (!MoveSelectedLegacyUnit(world_x, world_y)) {
        PlatformRuntime::instance().log_info("player_tap=no_action");
        return false;
    }
    PlatformRuntime::instance().log_info("player_tap=move");
    return RefreshDynamicWorldMeshLocked(true);
}

bool SetSinglePlayerTouchCommandMode(int mode) {
    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    if (!g_ready ||
        g_user_paused ||
        mode < 0 ||
        mode > static_cast<int>(TouchCommandMode::BuildTrenches)) {
        return false;
    }
    const TouchCommandMode requested =
            static_cast<TouchCommandMode>(mode);
    if (requested != TouchCommandMode::Contextual &&
        SelectedLegacyUnitId() < 0) {
        PlatformRuntime::instance().log_info(
                "player_touch_command_mode=rejected; reason=no_selection");
        return false;
    }
    int required_action = NDb::USER_ACTION_UNKNOWN;
    if (requested == TouchCommandMode::ClearMines) {
        required_action = NDb::USER_ACTION_ENGINEER_CLEAR_MINES;
    } else if (requested == TouchCommandMode::PlaceMines) {
        required_action = NDb::USER_ACTION_ENGINEER_PLACE_MINES;
    } else if (requested == TouchCommandMode::BuildTrenches) {
        required_action =
                NDb::USER_ACTION_ENGINEER_BUILD_ENTRENCHMENT;
    }
    if (required_action != NDb::USER_ACTION_UNKNOWN &&
        !CanSelectedLegacyUnitPerformAction(required_action)) {
        PlatformRuntime::instance().log_info(
                "player_touch_command_mode=rejected; "
                "reason=action_unavailable");
        return false;
    }
    if (requested != g_touch_command_mode) {
        ResetPendingTrenchCommandLocked();
    }
    g_touch_command_mode = requested;
    const char* name = "contextual";
    switch (requested) {
        case TouchCommandMode::Move:
            name = "move";
            break;
        case TouchCommandMode::Attack:
            name = "attack";
            break;
        case TouchCommandMode::Rotate:
            name = "rotate";
            break;
        case TouchCommandMode::Spyglass:
            name = "spyglass";
            break;
        case TouchCommandMode::ClearMines:
            name = "clear_mines";
            break;
        case TouchCommandMode::PlaceMines:
            name = "place_mines";
            break;
        case TouchCommandMode::BuildTrenches:
            name = "build_trenches";
            break;
        case TouchCommandMode::Contextual:
            break;
    }
    PlatformRuntime::instance().log_info(
            std::string("player_touch_command_mode=") + name);
    return true;
}

int SinglePlayerTouchCommandMode() {
    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    return static_cast<int>(g_touch_command_mode);
}

bool ActivateSelectedSinglePlayerUnit(int unit_id) {
    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    if (!g_ready ||
        g_user_paused ||
        !ActivateSelectedLegacyUnit(unit_id)) {
        return false;
    }
    g_touch_command_mode = TouchCommandMode::Contextual;
    ResetPendingTrenchCommandLocked();
    return RefreshDynamicWorldMeshLocked(true);
}

bool StopSelectedSinglePlayerUnit() {
    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    if (!g_ready || g_user_paused || !StopSelectedLegacyUnit()) {
        return false;
    }
    g_touch_command_mode = TouchCommandMode::Contextual;
    ResetPendingTrenchCommandLocked();
    return RefreshDynamicWorldMeshLocked(true);
}

bool PerformSelectedSinglePlayerUnitAction(int user_action) {
    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    if (!g_ready ||
        g_user_paused ||
        !PerformSelectedLegacyUnitAction(user_action)) {
        return false;
    }
    g_touch_command_mode = TouchCommandMode::Contextual;
    ResetPendingTrenchCommandLocked();
    return RefreshDynamicWorldMeshLocked(true);
}

void SetSinglePlayerPaused(bool paused) {
    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    if (!g_ready || g_user_paused == paused) {
        return;
    }
    g_user_paused = paused;
    g_touch_command_mode = TouchCommandMode::Contextual;
    ResetPendingTrenchCommandLocked();
    AudioBackend().set_paused(paused);
    if (paused) {
        AudioOutput().pause();
    } else if (
            PlatformRuntime::instance().lifecycle_state() ==
            LifecycleState::Focused) {
        AudioOutput().resume();
    }
    PlatformRuntime::instance().log_info(
            std::string("player_pause=") + (paused ? "true" : "false"));
}

bool IsSinglePlayerPaused() {
    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    return g_user_paused;
}

bool IsSinglePlayerRuntimeReady() {
    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    return g_ready;
}

std::string CurrentSinglePlayerMissionId() {
    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    return g_mission_id;
}

std::string SelectedSinglePlayerUnitHudStatus() {
    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    return SelectedLegacyUnitHudStatus();
}

std::string SelectedSinglePlayerUnitHudSnapshot() {
    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    return SelectedLegacyUnitHudSnapshot();
}

std::string SinglePlayerRuntimeReport() {
    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    size_t terrain_layer_triangles = 0;
    for (const TerrainLayer& layer : g_terrain_mesh.layers) {
        terrain_layer_triangles += layer.triangle_indices.size() / 3;
    }
    std::vector<std::pair<std::string, size_t> > static_fallbacks(
            g_static_fallback_stats_paths.begin(),
            g_static_fallback_stats_paths.end());
    std::sort(
            static_fallbacks.begin(),
            static_fallbacks.end(),
            [](const auto& left, const auto& right) {
                return left.second > right.second;
            });
    std::vector<std::pair<uint64_t, size_t> > dynamic_fallbacks(
            g_dynamic_fallback_stats_hashes.begin(),
            g_dynamic_fallback_stats_hashes.end());
    std::sort(
            dynamic_fallbacks.begin(),
            dynamic_fallbacks.end(),
            [](const auto& left, const auto& right) {
                return left.second > right.second;
            });
    std::ostringstream report;
    report << "single_player_runtime=" << (g_ready ? "ready" : "not_ready")
           << "; error=" << (g_last_error.empty() ? "<none>" : g_last_error)
           << "; mission=" << (g_mission_id.empty() ? "<none>" : g_mission_id)
           << "; map=" << (g_map_path.empty() ? "<none>" : g_map_path)
           << "; heightfield=" << g_height_width << "x" << g_height_height
           << "; triangles=" << g_triangle_count
           << "; terrain_texture="
           << (g_terrain_texture_path.empty()
                       ? "<none>"
                       : g_terrain_texture_path)
           << "; terrain_layers=" << g_terrain_layer_count
           << "; terrain_layer_textures=" << g_terrain_layer_texture_count
           << "; terrain_layer_triangles=" << terrain_layer_triangles
           << "; terrain_unassigned_triangles="
           << (g_triangle_count > terrain_layer_triangles
                       ? g_triangle_count - terrain_layer_triangles
                       : 0)
           << "; texture_gpu="
           << ((g_terrain_layer_texture_count > 0 ||
                g_terrain_mesh.texture_handle != UINT16_MAX)
                       ? "ready"
                       : "not_ready")
           << "; map_objects=" << g_map_object_count
           << "; scenario_objects=" << g_scenario_object_count
           << "; rendered_objects=" << g_rendered_object_count
           << "; dynamic_rendered_objects="
           << g_dynamic_rendered_object_count
           << "; converted_geometry_cache="
           << g_converted_geometries.size()
           << "; missing_converted_geometry="
           << g_missing_converted_geometries.size()
           << "; move_geometry_cache="
           << g_move_converted_geometries.size()
           << "; missing_move_geometry="
           << g_missing_move_converted_geometries.size()
           << "; attack_geometry_cache="
           << g_attack_converted_geometries.size()
           << "; missing_attack_geometry="
           << g_missing_attack_converted_geometries.size()
           << "; death_geometry_cache="
           << g_death_converted_geometries.size()
           << "; missing_death_geometry="
           << g_missing_death_converted_geometries.size()
           << "; lying_idle_geometry_cache="
           << g_lying_idle_converted_geometries.size()
           << "; missing_lying_idle_geometry="
           << g_missing_lying_idle_converted_geometries.size()
           << "; lying_move_geometry_cache="
           << g_lying_move_converted_geometries.size()
           << "; missing_lying_move_geometry="
           << g_missing_lying_move_converted_geometries.size()
           << "; lying_attack_geometry_cache="
           << g_lying_attack_converted_geometries.size()
           << "; missing_lying_attack_geometry="
           << g_missing_lying_attack_converted_geometries.size()
           << "; stats_geometry_index="
           << g_stats_geometry_index.size()
           << "; model_texture_layers="
           << g_world_object_mesh.layers.size()
           << "; model_textures=" << g_model_texture_count
           << "; converted_geometry_instances="
           << g_converted_geometry_instance_count
           << "; converted_geometry_fallbacks="
           << g_converted_geometry_fallback_count
           << "; animated_geometry_parts="
           << g_animated_geometry_part_count
           << "; move_animation_instances="
           << g_move_animation_instance_count
           << "; attack_animation_instances="
           << g_attack_animation_instance_count
           << "; death_animation_instances="
           << g_death_animation_instance_count
           << "; lying_idle_animation_instances="
           << g_lying_idle_animation_instance_count
           << "; lying_move_animation_instances="
           << g_lying_move_animation_instance_count
           << "; lying_attack_animation_instances="
           << g_lying_attack_animation_instance_count
           << "; combat_effect_renders="
           << g_combat_effect_render_count
           << "; active_combat_effects_rendered="
           << g_active_combat_effect_count
           << "; active_unit_indicators="
           << g_active_unit_indicator_count
           << "; static_fallback_types="
           << g_static_fallback_stats_paths.size()
           << "; dynamic_fallback_types="
           << g_dynamic_fallback_stats_hashes.size()
           << "; static_fallback_sample=";
    for (size_t index = 0;
         index < std::min<size_t>(static_fallbacks.size(), 8);
         ++index) {
        if (index > 0) {
            report << "|";
        }
        report << static_fallbacks[index].first
               << ":" << static_fallbacks[index].second;
    }
    report << "; dynamic_fallback_sample=";
    for (size_t index = 0;
         index < std::min<size_t>(dynamic_fallbacks.size(), 8);
         ++index) {
        if (index > 0) {
            report << "|";
        }
        report << std::hex << dynamic_fallbacks[index].first
               << std::dec << ":" << dynamic_fallbacks[index].second;
    }
    report << "; presentation_snapshot="
           << (g_presentation_snapshot_written ? "written" : "not_written")
           << "; " << LegacyGameRuntimeReport();
    return report.str();
}

std::vector<int32_t> MissionMinimapArgb(int width, int height) {
    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    if (!g_ready ||
        width <= 0 ||
        height <= 0 ||
        width > 1024 ||
        height > 1024 ||
        g_terrain_type_width <= 0 ||
        g_terrain_type_height <= 0 ||
        g_terrain_type_map.empty()) {
        return {};
    }
    std::vector<int32_t> pixels(
            static_cast<size_t>(width) * height,
            static_cast<int32_t>(0xff1d3027u));
    for (int y = 0; y < height; ++y) {
        const int source_y = std::min(
                g_terrain_type_height - 1,
                (height - 1 - y) * g_terrain_type_height / height);
        for (int x = 0; x < width; ++x) {
            const int source_x = std::min(
                    g_terrain_type_width - 1,
                    x * g_terrain_type_width / width);
            const int type_index = g_terrain_type_map[
                    static_cast<size_t>(
                            source_y * g_terrain_type_width + source_x)];
            if (type_index >= 0 &&
                type_index <
                        static_cast<int>(g_terrain_type_colors.size())) {
                uint32_t color = g_terrain_type_colors[type_index];
                color = 0xff000000u | (color & 0x00ffffffu);
                pixels[static_cast<size_t>(y * width + x)] =
                        static_cast<int32_t>(color);
            }
        }
    }

    const Bk2PresentationSnapshotInfo snapshot =
            bk2_presentation_snapshot_info();
    std::vector<Bk2PresentationEntity> entities(snapshot.entity_count);
    if (!entities.empty() &&
        bk2_presentation_copy_entities(
                entities.data(),
                entities.size()) == entities.size()) {
        const float world_size = std::max(g_terrain_mesh.world_size, 1.0f);
        for (const Bk2PresentationEntity& entity : entities) {
            if ((entity.flags & BK2_PRESENTATION_ENTITY_ALIVE) == 0) {
                continue;
            }
            const int center_x = std::clamp(
                    static_cast<int>(
                            entity.x / world_size * static_cast<float>(width)),
                    0,
                    width - 1);
            const int center_y = std::clamp(
                    height - 1 -
                            static_cast<int>(
                                    entity.y / world_size *
                                    static_cast<float>(height)),
                    0,
                    height - 1);
            const int radius =
                    (entity.flags & BK2_PRESENTATION_ENTITY_MECHANIZED) != 0
                    ? 2
                    : 1;
            const uint32_t color = entity.player == 0
                    ? 0xff70e08au
                    : 0xffe55f54u;
            for (int dy = -radius; dy <= radius; ++dy) {
                for (int dx = -radius; dx <= radius; ++dx) {
                    const int px = center_x + dx;
                    const int py = center_y + dy;
                    if (px >= 0 && px < width && py >= 0 && py < height) {
                        pixels[static_cast<size_t>(py * width + px)] =
                                static_cast<int32_t>(color);
                    }
                }
            }
        }
    }
    return pixels;
}

}  // namespace bk2::android

extern "C" JNIEXPORT jstring JNICALL
Java_com_nival_blitzkrieg2_NativeBridge_getCurrentMissionId(
        JNIEnv* env,
        jclass) {
    const std::string mission_id =
            bk2::android::CurrentSinglePlayerMissionId();
    return env->NewStringUTF(mission_id.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_nival_blitzkrieg2_NativeBridge_getSelectedUnitHudStatus(
        JNIEnv* env,
        jclass) {
    const std::string status =
            bk2::android::SelectedSinglePlayerUnitHudStatus();
    return env->NewStringUTF(status.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_nival_blitzkrieg2_NativeBridge_getSelectedUnitHudSnapshot(
        JNIEnv* env,
        jclass) {
    const std::string snapshot =
            bk2::android::SelectedSinglePlayerUnitHudSnapshot();
    return env->NewStringUTF(snapshot.c_str());
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_nival_blitzkrieg2_NativeBridge_setTouchCommandMode(
        JNIEnv*,
        jclass,
        jint mode) {
    return bk2::android::SetSinglePlayerTouchCommandMode(mode)
            ? JNI_TRUE
            : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_nival_blitzkrieg2_NativeBridge_stopSelectedUnit(
        JNIEnv*,
        jclass) {
    return bk2::android::StopSelectedSinglePlayerUnit()
            ? JNI_TRUE
            : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_nival_blitzkrieg2_NativeBridge_setActiveSelectedUnit(
        JNIEnv*,
        jclass,
        jint unit_id) {
    return bk2::android::ActivateSelectedSinglePlayerUnit(
                   static_cast<int>(unit_id))
            ? JNI_TRUE
            : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_nival_blitzkrieg2_NativeBridge_performSelectedUnitAction(
        JNIEnv*,
        jclass,
        jint user_action) {
    return bk2::android::PerformSelectedSinglePlayerUnitAction(
                   static_cast<int>(user_action))
            ? JNI_TRUE
            : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_nival_blitzkrieg2_NativeBridge_setMissionPaused(
        JNIEnv*,
        jclass,
        jboolean paused) {
    bk2::android::SetSinglePlayerPaused(paused == JNI_TRUE);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_nival_blitzkrieg2_NativeBridge_isMissionPaused(
        JNIEnv*,
        jclass) {
    return bk2::android::IsSinglePlayerPaused() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_nival_blitzkrieg2_NativeBridge_getTouchCommandMode(
        JNIEnv*,
        jclass) {
    return static_cast<jint>(
            bk2::android::SinglePlayerTouchCommandMode());
}

extern "C" JNIEXPORT jintArray JNICALL
Java_com_nival_blitzkrieg2_NativeBridge_getMissionMinimapArgb(
        JNIEnv* env,
        jclass,
        jint width,
        jint height) {
    const std::vector<int32_t> pixels =
            bk2::android::MissionMinimapArgb(width, height);
    if (pixels.empty()) {
        return nullptr;
    }
    jintArray result = env->NewIntArray(
            static_cast<jsize>(pixels.size()));
    if (result != nullptr) {
        env->SetIntArrayRegion(
                result,
                0,
                static_cast<jsize>(pixels.size()),
                reinterpret_cast<const jint*>(pixels.data()));
    }
    return result;
}
