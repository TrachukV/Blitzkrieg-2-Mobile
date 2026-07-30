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
#include "B2_M1_Terrain/DBWater.h"
#include "GameX/GetConsts.h"
#include "SceneB2/TerrainInfo.h"
#include "Stats_B2_M1/DBCameraConsts.h"
#include "Stats_B2_M1/DBClientConsts.h"
#include "Stats_B2_M1/DBMapInfo.h"
#include "Stats_B2_M1/DBVisObj.h"
#include "Stats_B2_M1/UserActions.h"
#include "Stats_B2_M1/Vis2AI.h"
#include "System/BinSaver.h"
#include "System/GlobalVars.h"
#include "Misc/StrProc.h"
#include "System/VFSOperations.h"
#include "libdb/Db.h"
#include "libdb/Database.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
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
constexpr float kPi = 3.14159265358979323846f;
constexpr float kDefaultCameraMinDistance = 150.0f;
constexpr float kDefaultCameraMaxDistance = 200.0f;
constexpr float kDefaultCameraDistance = 170.0f;
constexpr float kDefaultCameraPitchDegrees = 45.0f;
constexpr float kDefaultCameraYawDegrees = 45.0f;
constexpr float kDefaultCameraHorizontalFovDegrees = 26.0f;
constexpr float kLegacyWaterHeight = 0.1f;
constexpr float kWaterAnimationStepSeconds = 1.0f / 20.0f;
constexpr float kWaterWaveHeightScale = 0.08f;
constexpr uint32_t kWaterVertexColor = 0xd8ffffffu;
constexpr float kLegacyRoadHeight = 0.1f;
constexpr float kLegacyRiverDepth = 4.0f;
constexpr float kLegacyRiverWaterLevel = 0.1f;
constexpr float kLegacyRiverWaterHeightBias = 0.5f;
constexpr float kLegacyRiverWaterExpand = 0.25f;
constexpr float kLegacyRiverRidgeTiles = 4.0f;
constexpr float kLegacyRiverRidgeNull =
        1.41421356237309504880f / kLegacyRiverRidgeTiles;
constexpr int kLegacyRiverBottomCells = 8;
constexpr const char* kInfantryTraceTexture =
        "Scene/TexAndMats/All/Units/Weapons/GunShotTraceBlue_Texture.dds";
constexpr const char* kMechanizedTraceTexture =
        "Scene/TexAndMats/All/Units/Weapons/GunShotTraceOrange_texture.dds";
constexpr const char* kMuzzleFlashTexture =
        "Scene/TexAndMats/All/Effects/Shots/CannonShot/Shot8_Texture.dds";
constexpr const char* kEffectLightTexture =
        "Scene/TexAndMats/All/Effects/LightFX/Flare_Texture.dds";
constexpr const char* kProjectedShadowLayer =
        "__android_projected_shadow__";
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
struct CameraRuntimeLimits {
    float min_distance = kDefaultCameraMinDistance;
    float max_distance = kDefaultCameraMaxDistance;
    float default_distance = kDefaultCameraDistance;
};
CameraRuntimeLimits g_camera_limits;
TerrainMesh g_terrain_mesh;
WaterMesh g_water_mesh;
std::vector<TerrainVertex> g_water_base_vertices;
int g_water_mask_width = 0;
int g_water_mask_height = 0;
size_t g_water_mask_node_count = 0;
size_t g_water_rendered_node_count = 0;
size_t g_water_triangle_count = 0;
float g_water_wave_amplitude = 1.4f;
float g_water_wave_period = 0.3f;
float g_water_texture_tile_count = 6.0f;
float g_water_last_update_seconds = -1.0f;
bool g_water_waves_enabled = true;
bool g_water_texture_logged = false;
size_t g_road_instance_count = 0;
size_t g_road_segment_count = 0;
size_t g_road_triangle_count = 0;
size_t g_road_texture_count = 0;
size_t g_road_texture_gpu_count = 0;
bool g_road_texture_logged = false;
size_t g_river_instance_count = 0;
size_t g_river_point_count = 0;
size_t g_river_triangle_count = 0;
size_t g_river_texture_count = 0;
size_t g_river_texture_gpu_count = 0;
size_t g_river_carved_vertex_count = 0;
size_t g_river_disturbed_vertex_count = 0;
size_t g_river_animated_layer_count = 0;
bool g_river_texture_logged = false;
size_t g_crag_precipice_count = 0;
size_t g_river_bank_precipice_count = 0;
bool g_river_bank_precipice_logged = false;
size_t g_crag_node_count = 0;
size_t g_crag_triangle_count = 0;
size_t g_crag_foot_count = 0;
size_t g_crag_foot_segment_count = 0;
size_t g_crag_foot_triangle_count = 0;
size_t g_crag_texture_count = 0;
size_t g_crag_texture_gpu_count = 0;
std::unordered_set<std::string> g_crag_texture_paths;
bool g_crag_texture_logged = false;
bool g_turret_pose_logged = false;
// Mirrors the shipped gfx_noshadows option, refreshed once a frame.
bool g_shadows_disabled = false;
bool g_wheel_roll_logged = false;
size_t g_wheel_roll_part_count = 0;
WorldObjectMesh g_static_world_object_mesh;
// Static layers whose texture scrolls (rivers). They have to be rebuilt every
// frame, so they live apart from the geometry that is uploaded once.
WorldObjectMesh g_animated_static_world_object_mesh;
WorldObjectMesh g_world_object_mesh;
CObj<NGfx::CTexture> g_terrain_texture;
std::string g_terrain_texture_path;
CObj<NGfx::CTexture> g_minimap_texture;
std::string g_minimap_texture_path;
std::vector<uint32_t> g_minimap_base_pixels;
int g_minimap_base_width = 0;
int g_minimap_base_height = 0;
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
size_t g_culled_entity_count = 0;
size_t g_move_animation_instance_count = 0;
size_t g_attack_animation_instance_count = 0;
size_t g_death_animation_instance_count = 0;
size_t g_lying_idle_animation_instance_count = 0;
size_t g_lying_move_animation_instance_count = 0;
size_t g_lying_attack_animation_instance_count = 0;
size_t g_combat_effect_render_count = 0;
size_t g_effect_light_render_count = 0;
size_t g_active_combat_effect_count = 0;
size_t g_active_scene_effect_count = 0;
size_t g_active_destruction_effect_count = 0;
size_t g_active_unit_indicator_count = 0;
bool g_combat_effect_trace_texture_logged = false;
bool g_muzzle_flash_texture_logged = false;
bool g_descriptor_particle_texture_logged = false;
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

// Skeleton bone carried by the converted mesh, so a turret or gun subtree can
// be posed at runtime the way CMOUnitMechanical::AIUpdateTurretTurn does.
struct ConvertedGeometryBone {
    std::string name;
    int32_t parent = -1;
    float pivot_x = 0.0f;
    float pivot_y = 0.0f;
    float pivot_z = 0.0f;
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
    // Skeleton and per-vertex dominant bone, present from format 4 on.
    std::vector<ConvertedGeometryBone> bones;
    std::vector<uint32_t> vertex_bones;
    // Set when the whole part hangs off a road-wheel bone, so it can be
    // rolled about its axle as the vehicle covers ground. Classified once at
    // load; the per-frame path only reads it.
    bool wheel_roll = false;
    float wheel_pivot_y = 0.0f;
    float wheel_pivot_z = 0.0f;
    float wheel_radius = 0.0f;
};

struct ConvertedGeometry {
    std::vector<ConvertedGeometryPart> parts;
};

enum class GeometryMaterialAlphaMode : uint8_t {
    Inherit,
    Opaque,
    Blend,
    Test,
};

struct GeometryBinding {
    int geometry_record_id = -1;
    std::vector<int> material_quantities;
    std::vector<std::string> texture_paths;
    std::vector<GeometryMaterialAlphaMode> material_alpha_modes;
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
size_t g_material_alpha_test_layer_count = 0;
size_t g_material_alpha_test_triangle_count = 0;
size_t g_material_alpha_blend_layer_count = 0;
size_t g_material_alpha_blend_triangle_count = 0;
std::unordered_map<std::string, size_t> g_static_fallback_stats_paths;
std::unordered_map<uint64_t, size_t> g_dynamic_fallback_stats_hashes;

void RefreshWorldObjectTextureHandles(WorldObjectMesh* mesh);
float TerrainMeshHeightAtLocked(float world_x, float world_y);

struct MissionLaunchOverride {
    bool present = false;
    // The original client boots into its menu; "menu = 1" asks the shell for
    // that instead of dropping straight into a mission.
    bool menu = false;
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
        if (key == "menu") {
            result.menu = value != "0";
            result.present = true;
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
    if (launch.menu) {
        MissionRuntimeResult result;
        result.error = "menu_requested";
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

class LegacyRiverRandom {
public:
    explicit LegacyRiverRandom(uint32_t seed) : state_(seed) {
    }

    float Range(float minimum, float maximum) {
        if (maximum < minimum) {
            return minimum;
        }
        state_ = state_ * 214013u + 2531011u;
        const uint32_t value = (state_ >> 16u) & 0x7fffu;
        return minimum +
                static_cast<float>(value) /
                        32767.0f *
                        (maximum - minimum);
    }

private:
    uint32_t state_;
};

uint32_t LegacyRiverSeed(const NDb::SVSOInstance& river) {
    if (river.controlPoints.empty()) {
        return 0;
    }
    const CVec3& first = river.controlPoints.front();
    const CVec3& last = river.controlPoints.back();
    const float dx = last.x - first.x;
    const float dy = last.y - first.y;
    const float dz = last.z - first.z;
    return static_cast<uint32_t>(
            std::sqrt(dx * dx + dy * dy + dz * dz) *
            1000.0f);
}

void CarveTerrainRivers(
        const NDb::SMapInfo* map,
        TerrainMesh* mesh) {
    g_river_carved_vertex_count = 0;
    if (map == nullptr || mesh == nullptr || mesh->vertices.empty()) {
        return;
    }

    struct RiverCarveSegment {
        float start_x = 0.0f;
        float start_y = 0.0f;
        float end_x = 0.0f;
        float end_y = 0.0f;
        float start_half_width = 0.0f;
        float end_half_width = 0.0f;
    };
    std::vector<RiverCarveSegment> segments;
    for (const NDb::SVSOInstance& river : map->rivers) {
        if (river.points.size() < 2 ||
            !river.pDescriptor ||
            river.pDescriptor->GetTypeID() !=
                    NDb::SRiverDesc::typeID) {
            continue;
        }
        const NDb::SRiverDesc* descriptor =
                static_cast<const NDb::SRiverDesc*>(
                        river.pDescriptor.GetPtr());
        LegacyRiverRandom river_random(LegacyRiverSeed(river));
        struct RiverCarvePoint {
            float x = 0.0f;
            float y = 0.0f;
            float half_width = 0.0f;
        };
        std::vector<RiverCarvePoint> carve_points;
        carve_points.reserve(river.points.size());
        for (const NDb::SVSOPoint& point : river.points) {
            const float border_offset = river_random.Range(
                    -descriptor->fBorderRand * 0.5f,
                    descriptor->fBorderRand * 0.5f);
            carve_points.push_back(RiverCarvePoint{
                    AI2Vis(point.vPos.x) +
                            point.vNorm.x * border_offset,
                    AI2Vis(point.vPos.y) +
                            point.vNorm.y * border_offset,
                    AI2Vis(point.fWidth)});
        }
        segments.reserve(
                segments.size() + river.points.size() - 1);
        for (size_t index = 0;
             index + 1 < carve_points.size();
             ++index) {
            const RiverCarvePoint& start = carve_points[index];
            const RiverCarvePoint& end = carve_points[index + 1];
            RiverCarveSegment segment{
                    start.x,
                    start.y,
                    end.x,
                    end.y,
                    start.half_width,
                    end.half_width};
            const float dx = segment.end_x - segment.start_x;
            const float dy = segment.end_y - segment.start_y;
            if (std::isfinite(dx) &&
                std::isfinite(dy) &&
                dx * dx + dy * dy > 0.000001f &&
                segment.start_half_width > 0.0f &&
                segment.end_half_width > 0.0f) {
                segments.push_back(segment);
            }
        }
    }
    if (segments.empty()) {
        return;
    }

    const float ridge_width =
            VIS_TILE_SIZE * kLegacyRiverRidgeTiles;
    double carved_height_sum = 0.0;
    for (TerrainVertex& vertex : mesh->vertices) {
        float carve_depth = 0.0f;
        for (const RiverCarveSegment& segment : segments) {
            const float segment_x = segment.end_x - segment.start_x;
            const float segment_y = segment.end_y - segment.start_y;
            const float segment_length_squared =
                    segment_x * segment_x +
                    segment_y * segment_y;
            const float projection = std::clamp(
                    ((vertex.x - segment.start_x) * segment_x +
                     (vertex.y - segment.start_y) * segment_y) /
                            segment_length_squared,
                    0.0f,
                    1.0f);
            const float closest_x =
                    segment.start_x + segment_x * projection;
            const float closest_y =
                    segment.start_y + segment_y * projection;
            const float distance_x = vertex.x - closest_x;
            const float distance_y = vertex.y - closest_y;
            const float distance = std::sqrt(
                    distance_x * distance_x +
                    distance_y * distance_y);
            const float half_width =
                    segment.start_half_width +
                    (segment.end_half_width -
                     segment.start_half_width) *
                            projection;
            if (distance > half_width + ridge_width) {
                continue;
            }
            float profile = 1.0f;
            if (distance > half_width) {
                const float ridge_position =
                        (distance - half_width) / ridge_width;
                if (ridge_position > kLegacyRiverRidgeNull) {
                    const float transition = std::clamp(
                            (ridge_position -
                             kLegacyRiverRidgeNull) /
                                    (1.0f -
                                     kLegacyRiverRidgeNull),
                            0.0f,
                            1.0f);
                    profile = 1.0f - transition * transition;
                }
            }
            carve_depth = std::max(
                    carve_depth,
                    kLegacyRiverDepth * profile);
        }
        if (carve_depth <= 0.0001f) {
            continue;
        }
        vertex.z -= carve_depth;
        carved_height_sum += carve_depth;
        ++g_river_carved_vertex_count;
    }
    if (!mesh->vertices.empty()) {
        mesh->center_z -= static_cast<float>(
                carved_height_sum /
                static_cast<double>(mesh->vertices.size()));
    }

    std::ostringstream report;
    report << "terrain_river_carve="
           << (g_river_carved_vertex_count > 0
                       ? "ready"
                       : "empty")
           << "; rivers=" << map->rivers.size()
           << "; segments=" << segments.size()
           << "; vertices=" << g_river_carved_vertex_count
           << "; depth=" << kLegacyRiverDepth
           << "; ridge_width=" << ridge_width;
    PlatformRuntime::instance().log_info(report.str());
}

uint8_t WaterMaskAt(const STerrainInfo& info, int x, int y) {
    if (!info.seaMask.IsEmpty() &&
        x >= 0 &&
        y >= 0 &&
        x < info.seaMask.GetSizeX() &&
        y < info.seaMask.GetSizeY()) {
        return info.seaMask[y][x];
    }
    if (!info.optimizedSeaMask.IsEmpty() &&
        x >= 0 &&
        y >= 0 &&
        x < info.optimizedSeaMask.GetSizeX() &&
        y < info.optimizedSeaMask.GetSizeY()) {
        return info.optimizedSeaMask.GetData(x, y) ? 1u : 0u;
    }
    return 0u;
}

std::string WaterTexturePath(NDb::ESeason season) {
    const char* folder = "summer";
    switch (season) {
        case NDb::SEASON_WINTER:
            folder = "winter";
            break;
        case NDb::SEASON_SPRING:
            folder = "spring";
            break;
        case NDb::SEASON_AUTUMN:
            folder = "autumn";
            break;
        case NDb::SEASON_AFRICA:
            folder = "africa";
            break;
        case NDb::SEASON_ASIA:
            folder = "asia";
            break;
        case NDb::SEASON_SUMMER:
        default:
            break;
    }
    return std::string("Terrain/Water/") + folder + "/water.dds";
}

void ConfigureWaterDescriptor(const NDb::SMapInfo* map) {
    g_water_wave_amplitude = 1.4f;
    g_water_wave_period = 0.3f;
    g_water_texture_tile_count = 6.0f;
    g_water_waves_enabled = true;
    if (map == nullptr || !map->pOceanWater) {
        return;
    }
    const NDb::SWater* water = map->pOceanWater.GetPtr();
    if (water == nullptr) {
        return;
    }
    g_water_waves_enabled = water->bUseWaves;
    g_water_texture_tile_count = static_cast<float>(
            std::max(water->nTilesNumPerWaterTexture, 1));
    if (!water->waves.empty()) {
        const NDb::SWater::SWaterWaveType& wave = water->waves.front();
        if (std::isfinite(wave.fAmplitude)) {
            g_water_wave_amplitude =
                    std::clamp(std::fabs(wave.fAmplitude), 0.0f, 4.0f);
        }
        if (std::isfinite(wave.fPeriod) &&
            std::fabs(wave.fPeriod) >= 0.05f) {
            g_water_wave_period = std::fabs(wave.fPeriod);
        }
    }
}

bool BuildWaterMesh(
        const NDb::SMapInfo* map,
        const STerrainInfo& info,
        WaterMesh* mesh) {
    if (mesh == nullptr || map == nullptr) {
        return false;
    }
    *mesh = WaterMesh();
    g_water_base_vertices.clear();
    g_water_mask_width = !info.seaMask.IsEmpty()
            ? info.seaMask.GetSizeX()
            : info.optimizedSeaMask.GetSizeX();
    g_water_mask_height = !info.seaMask.IsEmpty()
            ? info.seaMask.GetSizeY()
            : info.optimizedSeaMask.GetSizeY();
    g_water_mask_node_count = 0;
    g_water_rendered_node_count = 0;
    g_water_triangle_count = 0;
    g_water_last_update_seconds = -1.0f;
    ConfigureWaterDescriptor(map);
    if (g_water_mask_width < 2 || g_water_mask_height < 2) {
        return false;
    }

    std::vector<int32_t> vertex_lookup(
            static_cast<size_t>(g_water_mask_width) *
                    static_cast<size_t>(g_water_mask_height),
            -1);
    const auto add_vertex =
            [&](int x, int y) -> uint32_t {
        const size_t source_index =
                static_cast<size_t>(y * g_water_mask_width + x);
        int32_t& existing = vertex_lookup[source_index];
        if (existing >= 0) {
            return static_cast<uint32_t>(existing);
        }
        const bool water = WaterMaskAt(info, x, y) != 0;
        existing = static_cast<int32_t>(mesh->vertices.size());
        mesh->vertices.push_back(TerrainVertex{
                static_cast<float>(x) * VIS_TILE_SIZE,
                static_cast<float>(y) * VIS_TILE_SIZE,
                kLegacyWaterHeight,
                static_cast<float>(x) / g_water_texture_tile_count,
                static_cast<float>(y) / g_water_texture_tile_count,
                water ? kWaterVertexColor : 0x00ffffffu});
        return static_cast<uint32_t>(existing);
    };

    for (int y = 0; y < g_water_mask_height; ++y) {
        for (int x = 0; x < g_water_mask_width; ++x) {
            if (WaterMaskAt(info, x, y) != 0) {
                ++g_water_mask_node_count;
            }
        }
    }
    for (int y = 0; y + 1 < g_water_mask_height; ++y) {
        for (int x = 0; x + 1 < g_water_mask_width; ++x) {
            const bool top_left_water =
                    WaterMaskAt(info, x, y) != 0;
            const bool top_right_water =
                    WaterMaskAt(info, x + 1, y) != 0;
            const bool bottom_left_water =
                    WaterMaskAt(info, x, y + 1) != 0;
            const bool bottom_right_water =
                    WaterMaskAt(info, x + 1, y + 1) != 0;
            if (!top_left_water &&
                !top_right_water &&
                !bottom_left_water &&
                !bottom_right_water) {
                continue;
            }
            const uint32_t top_left = add_vertex(x, y);
            const uint32_t top_right = add_vertex(x + 1, y);
            const uint32_t bottom_left = add_vertex(x, y + 1);
            const uint32_t bottom_right = add_vertex(x + 1, y + 1);
            mesh->triangle_indices.push_back(top_left);
            mesh->triangle_indices.push_back(bottom_left);
            mesh->triangle_indices.push_back(top_right);
            mesh->triangle_indices.push_back(top_right);
            mesh->triangle_indices.push_back(bottom_left);
            mesh->triangle_indices.push_back(bottom_right);
        }
    }
    mesh->texture_path = WaterTexturePath(map->eSeason);
    g_water_base_vertices = mesh->vertices;
    g_water_rendered_node_count = mesh->vertices.size();
    g_water_triangle_count = mesh->triangle_indices.size() / 3;

    std::ostringstream report;
    report << "water_mesh="
           << (mesh->triangle_indices.empty() ? "empty" : "ready")
           << "; mask=" << g_water_mask_width << "x"
           << g_water_mask_height
           << "; water_nodes=" << g_water_mask_node_count
           << "; rendered_nodes=" << g_water_rendered_node_count
           << "; triangles=" << g_water_triangle_count
           << "; texture=" << mesh->texture_path
           << "; waves="
           << (g_water_waves_enabled ? "descriptor" : "disabled")
           << "; amplitude=" << g_water_wave_amplitude
           << "; period=" << g_water_wave_period
           << "; tiles_per_texture=" << g_water_texture_tile_count;
    PlatformRuntime::instance().log_info(report.str());
    return !mesh->triangle_indices.empty();
}

bool UpdateWaterAnimationLocked(bool force) {
    if (g_water_mesh.vertices.empty() ||
        g_water_base_vertices.size() != g_water_mesh.vertices.size()) {
        return true;
    }
    if (!force &&
        g_water_last_update_seconds >= 0.0f &&
        g_animation_elapsed_seconds >= g_water_last_update_seconds &&
        g_animation_elapsed_seconds - g_water_last_update_seconds <
                kWaterAnimationStepSeconds) {
        return true;
    }
    const float animation_time = g_animation_elapsed_seconds;
    const float angular_speed =
            2.0f * kPi / std::max(g_water_wave_period, 0.05f);
    const float wave_height =
            g_water_waves_enabled
            ? g_water_wave_amplitude * kWaterWaveHeightScale
            : 0.0f;
    const float uv_offset_u = animation_time * 0.012f;
    const float uv_offset_v = animation_time * 0.008f;
    for (size_t index = 0;
         index < g_water_base_vertices.size();
         ++index) {
        const TerrainVertex& base = g_water_base_vertices[index];
        TerrainVertex& animated = g_water_mesh.vertices[index];
        animated = base;
        const float tile_x = base.x / VIS_TILE_SIZE;
        const float tile_y = base.y / VIS_TILE_SIZE;
        animated.z +=
                wave_height *
                (0.62f *
                         std::sin(
                                 tile_x * 0.31f +
                                 tile_y * 0.17f +
                                 animation_time * angular_speed) +
                 0.38f *
                         std::sin(
                                 tile_x * 0.11f -
                                 tile_y * 0.27f +
                                 animation_time *
                                         angular_speed * 0.61f));
        animated.u += uv_offset_u;
        animated.v += uv_offset_v;
    }
    g_water_last_update_seconds = animation_time;
    return RenderBackend().update_water_vertices(g_water_mesh.vertices);
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

// Vehicle models name the running gear; a cover panel is not a wheel.
bool IsWheelBoneName(const std::string& name) {
    if (name.find("WheelsCover") != std::string::npos) {
        return false;
    }
    return name.find("Wheel") != std::string::npos ||
            name.find("ReaWheels") != std::string::npos;
}

// A rigidly bound part carries exactly one bone, which is what the converter
// writes for vehicle running gear.
void ClassifyWheelPart(ConvertedGeometryPart* part) {
    if (part == nullptr ||
        part->bones.empty() ||
        part->vertex_bones.size() != part->vertices.size() ||
        part->vertices.empty()) {
        return;
    }
    const uint32_t bone = part->vertex_bones.front();
    if (bone >= part->bones.size()) {
        return;
    }
    for (uint32_t vertex_bone : part->vertex_bones) {
        if (vertex_bone != bone) {
            return;
        }
    }
    if (!IsWheelBoneName(part->bones[bone].name)) {
        return;
    }
    const float pivot_y = part->bones[bone].pivot_y;
    const float pivot_z = part->bones[bone].pivot_z;
    float radius_squared = 0.0f;
    for (const ConvertedGeometryVertex& vertex : part->vertices) {
        const float offset_y = vertex.y - pivot_y;
        const float offset_z = vertex.z - pivot_z;
        radius_squared = std::max(
                radius_squared,
                offset_y * offset_y + offset_z * offset_z);
    }
    const float radius = std::sqrt(radius_squared);
    if (!std::isfinite(radius) || radius < 0.05f) {
        return;
    }
    part->wheel_roll = true;
    part->wheel_pivot_y = pivot_y;
    part->wheel_pivot_z = pivot_z;
    part->wheel_radius = radius;
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
        (version < 1 || version > 4) ||
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
        if (version >= 4) {
            uint32_t bone_count = 0;
            if (!ReadExact(&input, &bone_count, sizeof(bone_count)) ||
                bone_count > 512) {
                missing_converted_geometries->insert(record_id);
                return nullptr;
            }
            part.bones.reserve(bone_count);
            for (uint32_t bone_index = 0;
                 bone_index < bone_count;
                 ++bone_index) {
                ConvertedGeometryBone bone;
                uint32_t name_length = 0;
                if (!ReadExact(&input, &bone.parent, sizeof(bone.parent)) ||
                    !ReadExact(
                            &input, &bone.pivot_x, sizeof(bone.pivot_x)) ||
                    !ReadExact(
                            &input, &bone.pivot_y, sizeof(bone.pivot_y)) ||
                    !ReadExact(
                            &input, &bone.pivot_z, sizeof(bone.pivot_z)) ||
                    !ReadExact(
                            &input, &name_length, sizeof(name_length)) ||
                    name_length > 256) {
                    missing_converted_geometries->insert(record_id);
                    return nullptr;
                }
                // Names are padded to a four byte boundary.
                const uint32_t padded = (name_length + 3u) & ~3u;
                std::vector<char> name(padded, '\0');
                if (padded > 0 &&
                    !ReadExact(&input, name.data(), padded)) {
                    missing_converted_geometries->insert(record_id);
                    return nullptr;
                }
                bone.name.assign(name.data(), name_length);
                part.bones.push_back(std::move(bone));
            }
            part.vertex_bones.resize(vertex_count);
            if (vertex_count > 0 &&
                !ReadExact(
                        &input,
                        part.vertex_bones.data(),
                        static_cast<size_t>(vertex_count) *
                                sizeof(uint32_t))) {
                missing_converted_geometries->insert(record_id);
                return nullptr;
            }
        }
        ClassifyWheelPart(&part);
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

GeometryMaterialAlphaMode ParseGeometryMaterialAlphaMode(
        const std::string& value) {
    if (value == "AM_OPAQUE") {
        return GeometryMaterialAlphaMode::Opaque;
    }
    if (value == "AM_ALPHA_TEST") {
        return GeometryMaterialAlphaMode::Test;
    }
    if (value == "AM_TRANSPARENT" ||
        value == "AM_OVERLAY" ||
        value == "AM_OVERLAY_ZWRITE" ||
        value == "AM_DECAL") {
        return GeometryMaterialAlphaMode::Blend;
    }
    return GeometryMaterialAlphaMode::Inherit;
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
            if (fields.size() >= 8 && !fields[7].empty()) {
                for (const std::string& alpha_mode :
                     SplitPreservingEmpty(fields[7], '|')) {
                    binding.material_alpha_modes.push_back(
                            ParseGeometryMaterialAlphaMode(alpha_mode));
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
        const std::string& texture_path,
        bool alpha_masked_shadow = false) {
    if (mesh == nullptr || texture_path.empty()) {
        return nullptr;
    }
    for (WorldObjectMesh::Layer& layer : mesh->layers) {
        if (layer.texture_path == texture_path &&
            layer.alpha_masked_shadow == alpha_masked_shadow) {
            return &layer;
        }
    }
    mesh->layers.push_back(WorldObjectMesh::Layer());
    mesh->layers.back().texture_path = texture_path;
    mesh->layers.back().alpha_masked_shadow = alpha_masked_shadow;
    return &mesh->layers.back();
}

size_t FindOrAddAlphaWorldObjectLayer(
        WorldObjectMesh* mesh,
        const std::string& texture_path,
        float texture_v_scroll_speed = 0.0f) {
    if (mesh == nullptr || texture_path.empty()) {
        return std::numeric_limits<size_t>::max();
    }
    for (size_t index = 0; index < mesh->layers.size(); ++index) {
        const WorldObjectMesh::Layer& layer = mesh->layers[index];
        if (layer.texture_path == texture_path &&
            layer.alpha_blended &&
            !layer.additive_blended &&
            !layer.alpha_tested &&
            !layer.alpha_masked_shadow &&
            !layer.depth_test_always &&
            layer.texture_v_scroll_speed ==
                    texture_v_scroll_speed) {
            return index;
        }
    }
    mesh->layers.push_back(WorldObjectMesh::Layer());
    WorldObjectMesh::Layer& layer = mesh->layers.back();
    layer.texture_path = texture_path;
    layer.alpha_blended = true;
    layer.texture_v_scroll_speed = texture_v_scroll_speed;
    return mesh->layers.size() - 1;
}

size_t FindOrAddOpaqueWorldObjectLayer(
        WorldObjectMesh* mesh,
        const std::string& texture_path) {
    if (mesh == nullptr || texture_path.empty()) {
        return std::numeric_limits<size_t>::max();
    }
    for (size_t index = 0; index < mesh->layers.size(); ++index) {
        const WorldObjectMesh::Layer& layer = mesh->layers[index];
        if (layer.texture_path == texture_path &&
            !layer.alpha_blended &&
            !layer.additive_blended &&
            !layer.alpha_tested &&
            !layer.alpha_masked_shadow &&
            !layer.depth_test_always &&
            layer.texture_v_scroll_speed == 0.0f) {
            return index;
        }
    }
    mesh->layers.push_back(WorldObjectMesh::Layer());
    mesh->layers.back().texture_path = texture_path;
    return mesh->layers.size() - 1;
}

WorldObjectMesh::Layer* FindOrAddMaterialWorldObjectLayer(
        WorldObjectMesh* mesh,
        const std::string& texture_path,
        bool alpha_blended,
        bool alpha_tested) {
    if (mesh == nullptr || texture_path.empty()) {
        return nullptr;
    }
    for (WorldObjectMesh::Layer& layer : mesh->layers) {
        if (layer.texture_path == texture_path &&
            layer.alpha_blended == alpha_blended &&
            layer.alpha_tested == alpha_tested &&
            !layer.additive_blended &&
            !layer.alpha_masked_shadow &&
            !layer.depth_test_always &&
            layer.texture_v_scroll_speed == 0.0f) {
            return &layer;
        }
    }
    mesh->layers.push_back(WorldObjectMesh::Layer());
    WorldObjectMesh::Layer& layer = mesh->layers.back();
    layer.texture_path = texture_path;
    layer.alpha_blended = alpha_blended;
    layer.alpha_tested = alpha_tested;
    return &layer;
}

struct RoadRenderLayer {
    size_t mesh_layer_index = std::numeric_limits<size_t>::max();
    float texture_u_min = 0.0f;
    float texture_u_max = 1.0f;
    float width_min = -1.0f;
    float width_max = 1.0f;
    float opacity = 1.0f;
};

std::string NormalizedDescriptorPath(const NDb::SVSODesc* descriptor) {
    if (descriptor == nullptr) {
        return std::string();
    }
    std::string descriptor_path =
            descriptor->GetDBID().ToString().c_str();
    std::replace(
            descriptor_path.begin(),
            descriptor_path.end(),
            '\\',
            '/');
    std::transform(
            descriptor_path.begin(),
            descriptor_path.end(),
            descriptor_path.begin(),
            [](unsigned char value) {
                return static_cast<char>(std::tolower(value));
            });
    return descriptor_path;
}

const char* DescriptorSeasonFolder(const std::string& descriptor_path) {
    const char* season = nullptr;
    for (const char* candidate : {
                 "winter",
                 "spring",
                 "summer",
                 "autumn",
                 "africa",
                 "asia"}) {
        if (descriptor_path.find(
                    std::string("/") + candidate + "/") !=
                std::string::npos) {
            season = candidate;
            break;
        }
    }
    return season;
}

std::string FallbackRoadTexturePath(const NDb::SVSODesc* descriptor) {
    const std::string descriptor_path =
            NormalizedDescriptorPath(descriptor);
    const char* season =
            DescriptorSeasonFolder(descriptor_path);
    if (season == nullptr) {
        return std::string();
    }

    const auto has_name =
            [&](const char* value) {
        return descriptor_path.find(value) != std::string::npos;
    };
    const char* texture_name = nullptr;
    if (has_name("railroadembankment")) {
        texture_name = "railroadembankment.dds";
    } else if (has_name("railroadgravel") ||
               has_name("/railroad_roaddesc")) {
        texture_name = "railroadgravel.dds";
    } else if (has_name("railroadtown")) {
        texture_name = "railroadtown.dds";
    } else if (has_name("townasphalt")) {
        texture_name = "townasphalt.dds";
    } else if (
            has_name("usedasphalt") ||
            has_name("useasphalt") ||
            has_name("used_aspahlt")) {
        texture_name = "useasphalt.dds";
    } else if (has_name("roadgroundsnow")) {
        texture_name = "roadgroundsnow.dds";
    } else if (has_name("roadground")) {
        texture_name = "roadground.dds";
    } else if (has_name("roadpaved")) {
        texture_name = "roadpaved .dds";
    } else if (has_name("track_roaddesc")) {
        texture_name = std::strcmp(season, "africa") == 0
                ? "road_track.dds"
                : "roadtrack.dds";
    } else if (has_name("path_roaddesc")) {
        texture_name = std::strcmp(season, "africa") == 0
                ? "road_path.dds"
                : "roadpath.dds";
    }
    if (texture_name == nullptr) {
        return std::string();
    }
    return std::string("Terrain/roads/") + season + "/" +
            texture_name;
}

bool ConfigureRoadRenderLayer(
        const NDb::SVSOLayerBaseDesc& layer_desc,
        const NDb::SMaterial* material,
        int from_pixel,
        int to_pixel,
        const std::string& fallback_texture_path,
        WorldObjectMesh* mesh,
        RoadRenderLayer* layer) {
    if (material == nullptr ||
        layer == nullptr ||
        mesh == nullptr) {
        return false;
    }
    const NDb::STexture* texture = material->pTexture
            ? material->pTexture.GetPtr()
            : nullptr;
    const std::string texture_path =
            texture != nullptr && !texture->szDestName.empty()
            ? std::string(texture->szDestName.c_str())
            : fallback_texture_path;
    if (texture_path.empty()) {
        return false;
    }
    const float texture_width =
            static_cast<float>(
                    texture == nullptr
                    ? 128
                    : std::max(texture->nWidth, 1));
    layer->texture_u_min =
            static_cast<float>(from_pixel) / texture_width;
    layer->texture_u_max =
            static_cast<float>(to_pixel) / texture_width;
    layer->opacity = std::clamp(
            layer_desc.fCenterOpacity,
            0.0f,
            1.0f);
    layer->mesh_layer_index = FindOrAddAlphaWorldObjectLayer(
            mesh,
            texture_path);
    return layer->mesh_layer_index !=
            std::numeric_limits<size_t>::max();
}

void AppendRoadLayerSegment(
        const NDb::SVSOPoint& current,
        const NDb::SVSOPoint& next,
        const STerrainInfo& terrain_info,
        float texture_v,
        float next_texture_v,
        const RoadRenderLayer& road_layer,
        WorldObjectMesh* mesh) {
    if (mesh == nullptr ||
        road_layer.mesh_layer_index >= mesh->layers.size()) {
        return;
    }
    const auto road_position =
            [&](const NDb::SVSOPoint& point, float width) {
        const float center_x = AI2Vis(point.vPos.x);
        const float center_y = AI2Vis(point.vPos.y);
        const float half_width = AI2Vis(point.fWidth);
        const float x =
                center_x + point.vNorm.x * half_width * width;
        const float y =
                center_y + point.vNorm.y * half_width * width;
        const float z = std::max(
                TerrainHeightAt(terrain_info, x, y),
                0.0f) + kLegacyRoadHeight;
        return TerrainVertex{x, y, z, 0.0f, 0.0f, 0xffffffffu};
    };
    TerrainVertex current_left =
            road_position(current, road_layer.width_min);
    TerrainVertex current_right =
            road_position(current, road_layer.width_max);
    TerrainVertex next_left =
            road_position(next, road_layer.width_min);
    TerrainVertex next_right =
            road_position(next, road_layer.width_max);
    const auto road_color =
            [&](float point_opacity) {
        const uint32_t alpha = static_cast<uint32_t>(
                std::lround(
                        std::clamp(
                                point_opacity * road_layer.opacity,
                                0.0f,
                                1.0f) *
                        255.0f));
        return (alpha << 24) | 0x00ffffffu;
    };
    current_left.u = road_layer.texture_u_min;
    current_right.u = road_layer.texture_u_max;
    next_left.u = road_layer.texture_u_min;
    next_right.u = road_layer.texture_u_max;
    current_left.v = current_right.v = texture_v;
    next_left.v = next_right.v = next_texture_v;
    current_left.abgr = current_right.abgr =
            road_color(current.fOpacity);
    next_left.abgr = next_right.abgr =
            road_color(next.fOpacity);

    const uint32_t vertex_base =
            static_cast<uint32_t>(mesh->vertices.size());
    mesh->vertices.push_back(current_left);
    mesh->vertices.push_back(current_right);
    mesh->vertices.push_back(next_left);
    mesh->vertices.push_back(next_right);
    WorldObjectMesh::Layer& output =
            mesh->layers[road_layer.mesh_layer_index];
    output.triangle_indices.push_back(vertex_base);
    output.triangle_indices.push_back(vertex_base + 2);
    output.triangle_indices.push_back(vertex_base + 1);
    output.triangle_indices.push_back(vertex_base + 1);
    output.triangle_indices.push_back(vertex_base + 2);
    output.triangle_indices.push_back(vertex_base + 3);
    g_road_triangle_count += 2;
}

bool AppendRoadInstance(
        const NDb::SVSOInstance& road,
        const STerrainInfo& terrain_info,
        WorldObjectMesh* mesh,
        std::unordered_set<std::string>* texture_paths) {
    if (mesh == nullptr ||
        road.points.size() < 2 ||
        !road.pDescriptor) {
        return false;
    }
    const NDb::SVSODesc* base_desc = road.pDescriptor.GetPtr();
    if (base_desc == nullptr ||
        base_desc->GetTypeID() != NDb::SRoadDesc::typeID) {
        return false;
    }
    const NDb::SRoadDesc* descriptor =
            static_cast<const NDb::SRoadDesc*>(base_desc);
    const std::string fallback_texture_path =
            FallbackRoadTexturePath(descriptor);
    std::vector<RoadRenderLayer> layers;
    layers.reserve(3);
    const auto add_layer =
            [&](const NDb::SVSOLayerBaseDesc& layer_desc,
                const NDb::SMaterial* material,
                int from_pixel,
                int to_pixel) {
        RoadRenderLayer layer;
        if (!ConfigureRoadRenderLayer(
                    layer_desc,
                    material,
                    from_pixel,
                    to_pixel,
                    fallback_texture_path,
                    mesh,
                    &layer)) {
            return;
        }
        if (texture_paths != nullptr) {
            texture_paths->insert(
                    mesh->layers[layer.mesh_layer_index].texture_path);
        }
        layers.push_back(layer);
    };
    if (!descriptor->center.materials.empty()) {
        add_layer(
                descriptor->center,
                descriptor->center.materials.front().GetPtr(),
                descriptor->center.nUseFromPixel,
                descriptor->center.nUseToPixel);
    }
    add_layer(
            descriptor->leftBorder,
            descriptor->leftBorder.pMaterial.GetPtr(),
            descriptor->leftBorder.nUseFromPixel,
            descriptor->leftBorder.nUseToPixel);
    add_layer(
            descriptor->rightBorder,
            descriptor->rightBorder.pMaterial.GetPtr(),
            descriptor->rightBorder.nUseFromPixel,
            descriptor->rightBorder.nUseToPixel);
    if (layers.empty()) {
        return false;
    }

    float global_u_min = std::numeric_limits<float>::max();
    float global_u_max = std::numeric_limits<float>::lowest();
    for (const RoadRenderLayer& layer : layers) {
        global_u_min = std::min(
                global_u_min,
                std::min(layer.texture_u_min, layer.texture_u_max));
        global_u_max = std::max(
                global_u_max,
                std::max(layer.texture_u_min, layer.texture_u_max));
    }
    const float global_u_range = global_u_max - global_u_min;
    if (!std::isfinite(global_u_range) || global_u_range <= 0.0001f) {
        return false;
    }
    for (RoadRenderLayer& layer : layers) {
        layer.width_min =
                (layer.texture_u_min - global_u_min) /
                        global_u_range * 2.0f -
                1.0f;
        layer.width_max =
                (layer.texture_u_max - global_u_min) /
                        global_u_range * 2.0f -
                1.0f;
    }

    bool appended = false;
    float texture_v = 0.0f;
    for (size_t point_index = 0;
         point_index + 1 < road.points.size();
         ++point_index) {
        const NDb::SVSOPoint& current = road.points[point_index];
        const NDb::SVSOPoint& next = road.points[point_index + 1];
        if (!std::isfinite(current.vPos.x) ||
            !std::isfinite(current.vPos.y) ||
            !std::isfinite(next.vPos.x) ||
            !std::isfinite(next.vPos.y) ||
            current.fWidth <= 0.0f ||
            next.fWidth <= 0.0f) {
            continue;
        }
        const float dx = AI2Vis(next.vPos.x - current.vPos.x);
        const float dy = AI2Vis(next.vPos.y - current.vPos.y);
        const float segment_length = std::sqrt(dx * dx + dy * dy);
        if (!std::isfinite(segment_length) ||
            segment_length <= 0.001f) {
            continue;
        }
        const float next_texture_v =
                texture_v +
                segment_length /
                        std::max(AI2Vis(next.fWidth) * 2.0f, 0.001f) *
                        global_u_range;
        for (const RoadRenderLayer& layer : layers) {
            AppendRoadLayerSegment(
                    current,
                    next,
                    terrain_info,
                    texture_v,
                    next_texture_v,
                    layer,
                    mesh);
        }
        texture_v = next_texture_v;
        ++g_road_segment_count;
        appended = true;
    }
    return appended;
}

void AppendTerrainRoads(
        const NDb::SMapInfo* map,
        const STerrainInfo& terrain_info,
        WorldObjectMesh* mesh) {
    g_road_instance_count = 0;
    g_road_segment_count = 0;
    g_road_triangle_count = 0;
    g_road_texture_count = 0;
    g_road_texture_gpu_count = 0;
    g_road_texture_logged = false;
    if (map == nullptr || mesh == nullptr) {
        return;
    }
    std::unordered_set<std::string> texture_paths;
    bool first_descriptor_logged = false;
    for (const NDb::SVSOInstance& road : map->roads) {
        if (!first_descriptor_logged) {
            const NDb::SVSODesc* descriptor =
                    road.pDescriptor
                    ? road.pDescriptor.GetPtr()
                    : nullptr;
            std::ostringstream descriptor_report;
            descriptor_report
                    << "terrain_road_descriptor="
                    << (descriptor == nullptr
                                ? "<unavailable>"
                                : descriptor->GetDBID().ToString().c_str())
                    << "; type="
                    << (descriptor == nullptr
                                ? -1
                                : descriptor->GetTypeID())
                    << "; expected_type="
                    << NDb::SRoadDesc::typeID
                    << "; points=" << road.points.size()
                    << "; fallback_texture="
                    << (descriptor == nullptr
                                ? "<unavailable>"
                                : FallbackRoadTexturePath(descriptor));
            if (descriptor != nullptr &&
                descriptor->GetTypeID() ==
                        NDb::SRoadDesc::typeID) {
                const NDb::SRoadDesc* road_descriptor =
                        static_cast<const NDb::SRoadDesc*>(descriptor);
                descriptor_report
                        << "; center_materials="
                        << road_descriptor->center.materials.size();
                if (!road_descriptor->center.materials.empty() &&
                    road_descriptor->center.materials.front()) {
                    const NDb::SMaterial* material =
                            road_descriptor->center.materials.front().GetPtr();
                    descriptor_report
                            << "; material="
                            << (material == nullptr
                                        ? "<unavailable>"
                                        : material->GetDBID().ToString().c_str())
                            << "; texture="
                            << (material == nullptr ||
                                        !material->pTexture
                                        ? "<unavailable>"
                                        : material->pTexture->szDestName.c_str());
                }
            }
            PlatformRuntime::instance().log_info(
                    descriptor_report.str());
            first_descriptor_logged = true;
        }
        if (AppendRoadInstance(
                    road,
                    terrain_info,
                    mesh,
                    &texture_paths)) {
            ++g_road_instance_count;
        }
    }
    g_road_texture_count = texture_paths.size();
    std::ostringstream report;
    report << "terrain_roads="
           << (g_road_instance_count > 0 ? "ready" : "empty")
           << "; descriptors=" << map->roads.size()
           << "; rendered=" << g_road_instance_count
           << "; segments=" << g_road_segment_count
           << "; triangles=" << g_road_triangle_count
           << "; textures=" << g_road_texture_count;
    PlatformRuntime::instance().log_info(report.str());
}

// PrecipicesManager.cpp collects crag precipices under the plain crag VSO id
// and the two river bank precipices under the river VSO id plus the
// 0x10000 (left) or 0x20000 (right) marker bit.
constexpr int kRiverPrecipiceIDBase = 0x10000;
constexpr int kPrecipiceSourceIDMask = 0xffff;

const NDb::SRiverDesc* ResolvePrecipiceRiverDescriptor(
        const NDb::SMapInfo* map,
        const STerrainInfo::SPrecipice& precipice) {
    if (map == nullptr ||
        precipice.nID < kRiverPrecipiceIDBase) {
        return nullptr;
    }
    const int river_id =
            precipice.nID & kPrecipiceSourceIDMask;
    for (const NDb::SVSOInstance& river : map->rivers) {
        if (river.nVSOID != river_id ||
            !river.pDescriptor ||
            river.pDescriptor->GetTypeID() !=
                    NDb::SRiverDesc::typeID) {
            continue;
        }
        return static_cast<const NDb::SRiverDesc*>(
                river.pDescriptor.GetPtr());
    }
    return nullptr;
}

const NDb::SMaterial* ResolvePrecipiceMaterial(
        const NDb::SMapInfo* map,
        const STerrainInfo::SPrecipice& precipice,
        const NDb::SRiverDesc* river_descriptor) {
    if (precipice.pMaterial) {
        const NDb::SMaterial* material =
                precipice.pMaterial.GetPtr();
        if (material != nullptr) {
            return material;
        }
    }
    if (river_descriptor != nullptr) {
        return river_descriptor->pPrecipiceMaterial
                ? river_descriptor->pPrecipiceMaterial.GetPtr()
                : nullptr;
    }
    if (map == nullptr) {
        return nullptr;
    }
    for (const NDb::SVSOInstance& crag : map->crags) {
        if (crag.nVSOID != precipice.nID ||
            !crag.pDescriptor ||
            crag.pDescriptor->GetTypeID() !=
                    NDb::SCragDesc::typeID) {
            continue;
        }
        const NDb::SCragDesc* descriptor =
                static_cast<const NDb::SCragDesc*>(
                        crag.pDescriptor.GetPtr());
        return descriptor != nullptr &&
                        descriptor->pRidgeMaterial
                ? descriptor->pRidgeMaterial.GetPtr()
                : nullptr;
    }
    return nullptr;
}

std::string PrecipiceTexturePath(
        const NDb::SMaterial* material) {
    if (material == nullptr) {
        return std::string();
    }
    const NDb::STexture* texture =
            material->pTexture
            ? material->pTexture.GetPtr()
            : nullptr;
    if (texture != nullptr && !texture->szDestName.empty()) {
        std::string texture_path = texture->szDestName.c_str();
        std::replace(
                texture_path.begin(),
                texture_path.end(),
                '\\',
                '/');
        while (!texture_path.empty() &&
               texture_path.front() == '/') {
            texture_path.erase(texture_path.begin());
        }
        if (texture_path.find('/') == std::string::npos) {
            std::string folder = ToStdString(
                    NDb::GetFolderName(material->GetDBID()));
            std::replace(
                    folder.begin(),
                    folder.end(),
                    '\\',
                    '/');
            while (!folder.empty() &&
                   folder.back() == '/') {
                folder.pop_back();
            }
            texture_path = folder + "/" + texture_path;
        }
        return texture_path;
    }
    std::string material_path =
            material->GetDBID().ToString().c_str();
    std::replace(
            material_path.begin(),
            material_path.end(),
            '\\',
            '/');
    while (!material_path.empty() &&
           material_path.front() == '/') {
        material_path.erase(material_path.begin());
    }
    std::string lower_path = material_path;
    std::transform(
            lower_path.begin(),
            lower_path.end(),
            lower_path.begin(),
            [](unsigned char value) {
                return static_cast<char>(std::tolower(value));
            });
    constexpr const char* kMaterialSuffix = "ridge_material.xdb";
    const size_t suffix = lower_path.rfind(kMaterialSuffix);
    if (suffix == std::string::npos) {
        return std::string();
    }
    material_path.replace(
            suffix,
            std::strlen(kMaterialSuffix),
            "ridge_Texture.dds");
    return material_path;
}

std::string FallbackRiverTexturePath(
        const NDb::SVSODesc* descriptor,
        const char* texture_name);

bool AppendPrecipiceMesh(
        const NDb::SMapInfo* map,
        const STerrainInfo& terrain_info,
        const STerrainInfo::SPrecipice& precipice,
        WorldObjectMesh* mesh,
        std::unordered_set<std::string>* texture_paths,
        bool* river_bank) {
    if (river_bank != nullptr) {
        *river_bank = false;
    }
    if (mesh == nullptr ||
        precipice.nodes.size() < 2 ||
        precipice.minHeights.size() !=
                precipice.nodes.size() ||
        precipice.maxHeights.size() !=
                precipice.nodes.size()) {
        return false;
    }
    const NDb::SRiverDesc* river_descriptor =
            ResolvePrecipiceRiverDescriptor(map, precipice);
    const NDb::SMaterial* material =
            ResolvePrecipiceMaterial(
                    map,
                    precipice,
                    river_descriptor);
    // River bank precipices carry the river's own precipice material, whose
    // shipped texture lives beside the water assets. Resolve it the way the
    // river water layers do instead of through the crag material naming
    // convention, which would invent an unstaged Scene/TexAndMats path.
    std::string texture_path;
    if (river_descriptor != nullptr) {
        const NDb::STexture* river_texture =
                material != nullptr && material->pTexture
                ? material->pTexture.GetPtr()
                : nullptr;
        texture_path =
                river_texture != nullptr &&
                                !river_texture->szDestName.empty()
                ? std::string(river_texture->szDestName.c_str())
                : FallbackRiverTexturePath(
                          river_descriptor,
                          "crags");
    } else {
        texture_path = PrecipiceTexturePath(material);
    }
    if (texture_path.empty()) {
        return false;
    }
    if (river_bank != nullptr) {
        *river_bank = river_descriptor != nullptr;
    }
    if (river_descriptor != nullptr &&
        !g_river_bank_precipice_logged) {
        std::ostringstream report;
        report << "terrain_river_bank_precipice="
               << NormalizedDescriptorPath(river_descriptor)
               << "; precipice_id=" << precipice.nID
               << "; side="
               << ((precipice.nID & (kRiverPrecipiceIDBase << 1))
                           ? "right"
                           : "left")
               << "; material="
               << (material == nullptr
                           ? std::string("<none>")
                           : std::string(
                                     material->GetDBID()
                                             .ToString()
                                             .c_str()))
               << "; texture=" << texture_path;
        PlatformRuntime::instance().log_info(report.str());
        g_river_bank_precipice_logged = true;
    }
    const NDb::STexture* texture =
            material != nullptr && material->pTexture
            ? material->pTexture.GetPtr()
            : nullptr;
    const float texture_scale_x =
            precipice.fTexGeomScale /
            static_cast<float>(
                    texture == nullptr
                    ? 512
                    : std::max(texture->nWidth, 1));
    const float texture_scale_y =
            precipice.fTexGeomScale /
            static_cast<float>(
                    texture == nullptr
                    ? 512
                    : std::max(texture->nHeight, 1));
    const size_t layer_index =
            FindOrAddOpaqueWorldObjectLayer(
                    mesh,
                    texture_path);
    if (layer_index == std::numeric_limits<size_t>::max()) {
        return false;
    }
    if (texture_paths != nullptr) {
        texture_paths->insert(texture_path);
    }

    constexpr float kMinimumPrecipiceHeight = 0.025f;
    constexpr float kHeightEpsilon = 0.001f;
    uint32_t previous_vertex_offset = 0;
    int previous_vertex_count = 0;
    int previous_node_index = -1;
    float texture_x = 0.0f;
    bool appended = false;
    for (size_t node_position = 0;
         node_position < precipice.nodes.size();
         ++node_position) {
        const bool visible =
                precipice.visibles.empty() ||
                (node_position < precipice.visibles.size() &&
                 precipice.visibles[node_position] != 0);
        const int node_index = precipice.nodes[node_position];
        if (!visible ||
            node_index < 0 ||
            node_index >=
                    static_cast<int>(
                            terrain_info.precNodes.size())) {
            previous_vertex_count = 0;
            previous_node_index = -1;
            continue;
        }
        const STerrainInfo::SPrecipiceNode& node =
                terrain_info.precNodes[
                        static_cast<size_t>(node_index)];
        const float minimum_height =
                precipice.minHeights[node_position];
        const float maximum_height =
                precipice.maxHeights[node_position];
        const bool low_column =
                maximum_height <=
                minimum_height + kMinimumPrecipiceHeight;
        const bool previous_low_column =
                node_position == 0 ||
                precipice.maxHeights[node_position - 1] <=
                        precipice.minHeights[node_position - 1] +
                                kMinimumPrecipiceHeight;
        const bool next_low_column =
                node_position + 1 >= precipice.nodes.size() ||
                precipice.maxHeights[node_position + 1] <=
                        precipice.minHeights[node_position + 1] +
                                kMinimumPrecipiceHeight;
        if (node.verts.size() < 2 ||
            (low_column &&
             previous_low_column &&
             next_low_column)) {
            previous_vertex_count = 0;
            previous_node_index = -1;
            continue;
        }

        size_t first_vertex = 0;
        while (first_vertex + 1 < node.verts.size() &&
               node.verts[first_vertex].z >
                       maximum_height + kHeightEpsilon) {
            ++first_vertex;
        }
        size_t last_vertex = first_vertex;
        while (last_vertex + 1 < node.verts.size() &&
               node.verts[last_vertex].z >
                       minimum_height + kHeightEpsilon) {
            ++last_vertex;
        }
        const int current_vertex_count =
                static_cast<int>(
                        last_vertex - first_vertex + 1);
        if (current_vertex_count < 1) {
            previous_vertex_count = 0;
            previous_node_index = -1;
            continue;
        }

        if (previous_vertex_count > 0 &&
            previous_node_index != node_index) {
            const int maximum_vertex_count =
                    std::max(
                            previous_vertex_count,
                            current_vertex_count);
            const float previous_coefficient =
                    previous_vertex_count > current_vertex_count
                    ? 1.0f
                    : static_cast<float>(previous_vertex_count) /
                            static_cast<float>(
                                    current_vertex_count);
            const float current_coefficient =
                    current_vertex_count > previous_vertex_count
                    ? 1.0f
                    : static_cast<float>(current_vertex_count) /
                            static_cast<float>(
                                    previous_vertex_count);
            float maximum_distance = 0.0f;
            for (int index = 0;
                 index < maximum_vertex_count;
                 ++index) {
                const uint32_t previous_index =
                        previous_vertex_offset +
                        static_cast<uint32_t>(
                                previous_coefficient *
                                static_cast<float>(index));
                const size_t current_index =
                        first_vertex +
                        static_cast<size_t>(
                                current_coefficient *
                                static_cast<float>(index));
                if (previous_index >= mesh->vertices.size() ||
                    current_index >= node.verts.size()) {
                    continue;
                }
                const TerrainVertex& previous =
                        mesh->vertices[previous_index];
                const CVec3& current =
                        node.verts[current_index];
                const float dx = current.x - previous.x;
                const float dy = current.y - previous.y;
                maximum_distance = std::max(
                        maximum_distance,
                        std::sqrt(dx * dx + dy * dy));
            }
            texture_x += maximum_distance * texture_scale_x;
        }

        const uint32_t current_vertex_offset =
                static_cast<uint32_t>(mesh->vertices.size());
        std::vector<float> texture_y(
                static_cast<size_t>(current_vertex_count),
                0.0f);
        texture_y.back() =
                node.verts[last_vertex].z *
                texture_scale_y;
        for (size_t local_index =
                     texture_y.size() - 1;
             local_index > 0;
             --local_index) {
                const CVec3& lower = node.verts[
                        first_vertex + local_index];
                const CVec3& current = node.verts[
                        first_vertex + local_index - 1];
                const float dx = current.x - lower.x;
                const float dy = current.y - lower.y;
                const float dz = current.z - lower.z;
                texture_y[local_index - 1] =
                        texture_y[local_index] +
                        std::sqrt(
                                dx * dx + dy * dy + dz * dz) *
                        texture_scale_y;
        }
        for (size_t local_index = 0;
             local_index < texture_y.size();
             ++local_index) {
            const size_t vertex_index =
                    first_vertex + local_index;
            const CVec3& source = node.verts[vertex_index];
            mesh->vertices.push_back(TerrainVertex{
                    source.x,
                    source.y,
                    source.z,
                    texture_x,
                    texture_y[local_index],
                    0xffffffffu});
        }

        if (previous_vertex_count > 0 &&
            previous_node_index != node_index) {
            const int maximum_vertex_count =
                    std::max(
                            previous_vertex_count,
                            current_vertex_count);
            const float previous_coefficient =
                    previous_vertex_count > current_vertex_count
                    ? 1.0f
                    : static_cast<float>(previous_vertex_count) /
                            static_cast<float>(
                                    current_vertex_count);
            const float current_coefficient =
                    current_vertex_count > previous_vertex_count
                    ? 1.0f
                    : static_cast<float>(current_vertex_count) /
                            static_cast<float>(
                                    previous_vertex_count);
            WorldObjectMesh::Layer& layer =
                    mesh->layers[layer_index];
            for (int index = 0;
                 index + 1 < maximum_vertex_count;
                 ++index) {
                const uint32_t previous_top =
                        previous_vertex_offset +
                        static_cast<uint32_t>(
                                previous_coefficient *
                                static_cast<float>(index));
                const uint32_t previous_bottom =
                        previous_vertex_offset +
                        static_cast<uint32_t>(
                                previous_coefficient *
                                static_cast<float>(index + 1));
                const uint32_t current_top =
                        current_vertex_offset +
                        static_cast<uint32_t>(
                                current_coefficient *
                                static_cast<float>(index));
                const uint32_t current_bottom =
                        current_vertex_offset +
                        static_cast<uint32_t>(
                                current_coefficient *
                                static_cast<float>(index + 1));
                if (previous_top != previous_bottom) {
                    layer.triangle_indices.push_back(previous_top);
                    layer.triangle_indices.push_back(previous_bottom);
                    layer.triangle_indices.push_back(current_bottom);
                    ++g_crag_triangle_count;
                    appended = true;
                }
                if (current_top != current_bottom) {
                    layer.triangle_indices.push_back(current_bottom);
                    layer.triangle_indices.push_back(current_top);
                    layer.triangle_indices.push_back(previous_top);
                    ++g_crag_triangle_count;
                    appended = true;
                }
            }
        }
        ++g_crag_node_count;
        previous_vertex_offset = current_vertex_offset;
        previous_vertex_count = current_vertex_count;
        previous_node_index = node_index;
    }
    return appended;
}

void AppendTerrainFoots(
        const STerrainInfo& terrain_info,
        WorldObjectMesh* mesh,
        std::unordered_set<std::string>* texture_paths) {
    g_crag_foot_count = 0;
    g_crag_foot_segment_count = 0;
    g_crag_foot_triangle_count = 0;
    if (mesh == nullptr) {
        return;
    }
    constexpr float kFootWidthBase = 1.5f;
    constexpr float kFootWidthPerRadius = 0.3f;
    for (const STerrainInfo::SFoot& foot : terrain_info.foots) {
        const NDb::SMaterial* material =
                foot.pFootMaterial
                ? foot.pFootMaterial.GetPtr()
                : nullptr;
        const std::string texture_path =
                PrecipiceTexturePath(material);
        if (texture_path.empty()) {
            continue;
        }
        const NDb::STexture* texture =
                material != nullptr && material->pTexture
                ? material->pTexture.GetPtr()
                : nullptr;
        const float texture_scale_x =
                foot.fTexGeomScale /
                static_cast<float>(
                        texture == nullptr
                        ? 512
                        : std::max(texture->nWidth, 1));
        const float texture_scale_y =
                foot.fTexGeomScale /
                static_cast<float>(
                        texture == nullptr
                        ? 512
                        : std::max(texture->nHeight, 1));
        const size_t layer_index =
                FindOrAddAlphaWorldObjectLayer(mesh, texture_path);
        if (layer_index == std::numeric_limits<size_t>::max()) {
            continue;
        }
        if (texture_paths != nullptr) {
            texture_paths->insert(texture_path);
        }
        bool appended = false;
        for (const vector<STerrainInfo::SVSOPoint>& points :
             foot.points) {
            if (points.size() < 2) {
                continue;
            }
            float texture_x = 0.0f;
            for (size_t point_index = 0;
                 point_index < points.size();
                 ++point_index) {
                const STerrainInfo::SVSOPoint& point =
                        points[point_index];
                const float width =
                        kFootWidthBase +
                        kFootWidthPerRadius * point.fRadius;
                const float inner_x = point.vPos.x;
                const float inner_y = point.vPos.y;
                const float outer_x =
                        inner_x + point.vNorm.x * width;
                const float outer_y =
                        inner_y + point.vNorm.y * width;
                const float inner_z =
                        std::max(
                                TerrainHeightAt(
                                        terrain_info,
                                        inner_x,
                                        inner_y),
                                0.0f) +
                        kLegacyRoadHeight;
                const float outer_z =
                        std::max(
                                TerrainHeightAt(
                                        terrain_info,
                                        outer_x,
                                        outer_y),
                                0.0f) +
                        kLegacyRoadHeight;
                float inner_alpha = 1.0f;
                if (point_index == 0 ||
                    point_index + 1 == points.size()) {
                    inner_alpha = 0.0f;
                }
                const uint32_t inner_color =
                        (static_cast<uint32_t>(
                                 std::lround(inner_alpha * 255.0f))
                         << 24) |
                        0x00ffffffu;
                const uint32_t vertex_base =
                        static_cast<uint32_t>(mesh->vertices.size());
                mesh->vertices.push_back(TerrainVertex{
                        inner_x,
                        inner_y,
                        inner_z,
                        texture_x,
                        point.vPos.z * texture_scale_y,
                        inner_color});
                mesh->vertices.push_back(TerrainVertex{
                        outer_x,
                        outer_y,
                        outer_z,
                        texture_x,
                        point.vPos.z * texture_scale_y -
                                width * texture_scale_y,
                        0x00ffffffu});
                if (point_index > 0) {
                    WorldObjectMesh::Layer& layer =
                            mesh->layers[layer_index];
                    layer.triangle_indices.push_back(
                            vertex_base - 2);
                    layer.triangle_indices.push_back(vertex_base);
                    layer.triangle_indices.push_back(
                            vertex_base - 1);
                    layer.triangle_indices.push_back(
                            vertex_base - 1);
                    layer.triangle_indices.push_back(vertex_base);
                    layer.triangle_indices.push_back(
                            vertex_base + 1);
                    ++g_crag_foot_segment_count;
                    g_crag_foot_triangle_count += 2;
                    appended = true;
                }
                if (point_index + 1 < points.size()) {
                    const STerrainInfo::SVSOPoint& next =
                            points[point_index + 1];
                    const float dx = next.vPos.x - point.vPos.x;
                    const float dy = next.vPos.y - point.vPos.y;
                    texture_x +=
                            std::sqrt(dx * dx + dy * dy) *
                            texture_scale_x;
                }
            }
        }
        if (appended) {
            ++g_crag_foot_count;
        }
    }
}

void AppendTerrainPrecipices(
        const NDb::SMapInfo* map,
        const STerrainInfo& terrain_info,
        WorldObjectMesh* mesh) {
    g_crag_precipice_count = 0;
    g_river_bank_precipice_count = 0;
    g_river_bank_precipice_logged = false;
    g_crag_node_count = 0;
    g_crag_triangle_count = 0;
    g_crag_foot_count = 0;
    g_crag_foot_segment_count = 0;
    g_crag_foot_triangle_count = 0;
    g_crag_texture_count = 0;
    g_crag_texture_gpu_count = 0;
    g_crag_texture_paths.clear();
    g_crag_texture_logged = false;
    if (mesh == nullptr) {
        return;
    }
    std::unordered_set<std::string> texture_paths;
    float rendered_min_x = std::numeric_limits<float>::max();
    float rendered_min_y = std::numeric_limits<float>::max();
    float rendered_max_x = std::numeric_limits<float>::lowest();
    float rendered_max_y = std::numeric_limits<float>::lowest();
    float largest_min_x = 0.0f;
    float largest_min_y = 0.0f;
    float largest_max_x = 0.0f;
    float largest_max_y = 0.0f;
    int largest_id = -1;
    size_t largest_triangles = 0;
    for (const STerrainInfo::SPrecipice& precipice :
         terrain_info.precipices) {
        const size_t triangles_before = g_crag_triangle_count;
        bool river_bank = false;
        if (AppendPrecipiceMesh(
                    map,
                    terrain_info,
                    precipice,
                    mesh,
                    &texture_paths,
                    &river_bank)) {
            ++g_crag_precipice_count;
            if (river_bank) {
                ++g_river_bank_precipice_count;
            }
            float precipice_min_x =
                    std::numeric_limits<float>::max();
            float precipice_min_y =
                    std::numeric_limits<float>::max();
            float precipice_max_x =
                    std::numeric_limits<float>::lowest();
            float precipice_max_y =
                    std::numeric_limits<float>::lowest();
            for (int node_index : precipice.nodes) {
                if (node_index < 0 ||
                    node_index >= static_cast<int>(
                            terrain_info.precNodes.size())) {
                    continue;
                }
                const CVec2& position =
                        terrain_info
                                .precNodes[static_cast<size_t>(node_index)]
                                .vPos;
                precipice_min_x =
                        std::min(precipice_min_x, position.x);
                precipice_min_y =
                        std::min(precipice_min_y, position.y);
                precipice_max_x =
                        std::max(precipice_max_x, position.x);
                precipice_max_y =
                        std::max(precipice_max_y, position.y);
            }
            if (precipice_min_x <= precipice_max_x &&
                precipice_min_y <= precipice_max_y) {
                rendered_min_x =
                        std::min(rendered_min_x, precipice_min_x);
                rendered_min_y =
                        std::min(rendered_min_y, precipice_min_y);
                rendered_max_x =
                        std::max(rendered_max_x, precipice_max_x);
                rendered_max_y =
                        std::max(rendered_max_y, precipice_max_y);
                const size_t triangle_count =
                        g_crag_triangle_count - triangles_before;
                if (triangle_count > largest_triangles) {
                    largest_id = precipice.nID;
                    largest_triangles = triangle_count;
                    largest_min_x = precipice_min_x;
                    largest_min_y = precipice_min_y;
                    largest_max_x = precipice_max_x;
                    largest_max_y = precipice_max_y;
                }
            }
        }
    }
    AppendTerrainFoots(
            terrain_info,
            mesh,
            &texture_paths);
    g_crag_texture_count = texture_paths.size();
    g_crag_texture_paths = texture_paths;
    std::ostringstream report;
    report << "terrain_crags="
           << (g_crag_precipice_count > 0
                       ? "ready"
                       : "empty")
           << "; descriptors="
           << (map == nullptr ? 0 : map->crags.size())
           << "; river_descriptors="
           << (map == nullptr ? 0 : map->rivers.size())
           << "; serialized_precipices="
           << terrain_info.precipices.size()
           << "; serialized_nodes="
           << terrain_info.precNodes.size()
           << "; rendered_precipices="
           << g_crag_precipice_count
           << "; rendered_river_banks="
           << g_river_bank_precipice_count
           << "; rendered_nodes="
           << g_crag_node_count
           << "; triangles="
           << g_crag_triangle_count
           << "; serialized_foots="
           << terrain_info.foots.size()
           << "; rendered_foots="
           << g_crag_foot_count
           << "; foot_segments="
           << g_crag_foot_segment_count
           << "; foot_triangles="
           << g_crag_foot_triangle_count
           << "; textures="
           << g_crag_texture_count;
    if (rendered_min_x <= rendered_max_x &&
        rendered_min_y <= rendered_max_y) {
        report << "; bounds="
               << rendered_min_x << "," << rendered_min_y
               << "-" << rendered_max_x << "," << rendered_max_y
               << "; largest_id=" << largest_id
               << "; largest_triangles=" << largest_triangles
               << "; largest_bounds="
               << largest_min_x << "," << largest_min_y
               << "-" << largest_max_x << "," << largest_max_y;
    }
    PlatformRuntime::instance().log_info(report.str());
}

std::string FallbackRiverTexturePath(
        const NDb::SVSODesc* descriptor,
        const char* texture_name) {
    if (descriptor == nullptr ||
        texture_name == nullptr ||
        *texture_name == '\0') {
        return std::string();
    }
    const char* season = DescriptorSeasonFolder(
            NormalizedDescriptorPath(descriptor));
    if (season == nullptr) {
        return std::string();
    }
    return std::string("Terrain/Water/") + season + "/" +
            texture_name + ".dds";
}

struct RiverRenderLayer {
    size_t mesh_layer_index = std::numeric_limits<size_t>::max();
    int cells = 2;
    float width_scale = 1.0f;
    float opacity = 1.0f;
    float tiling_step = 0.1f;
    float stream_speed = 0.0f;
    int water_layer_index = -1;
    bool bottom = false;
};

struct RiverPointGeometry {
    float center_x = 0.0f;
    float center_y = 0.0f;
    float normal_x = 0.0f;
    float normal_y = 1.0f;
    float half_width = 0.0f;
    float bottom_height = 0.0f;
    float water_height = 0.0f;
    float opacity = 1.0f;
    std::array<std::vector<float>, 2> water_disturbance_offsets;
};

bool ConfigureRiverRenderLayer(
        const NDb::SVSODesc* descriptor,
        const NDb::SMaterial* material,
        const char* fallback_texture_name,
        int cells,
        float width_scale,
        float opacity,
        float tiling_step,
        float stream_speed,
        int water_layer_index,
        bool bottom,
        WorldObjectMesh* mesh,
        RiverRenderLayer* layer) {
    if (descriptor == nullptr ||
        mesh == nullptr ||
        layer == nullptr) {
        return false;
    }
    const NDb::STexture* texture =
            material != nullptr && material->pTexture
            ? material->pTexture.GetPtr()
            : nullptr;
    const std::string texture_path =
            texture != nullptr && !texture->szDestName.empty()
            ? std::string(texture->szDestName.c_str())
            : FallbackRiverTexturePath(
                      descriptor,
                      fallback_texture_name);
    if (texture_path.empty()) {
        return false;
    }
    layer->cells = std::clamp(cells, 2, 24);
    layer->width_scale = std::max(width_scale, 0.05f);
    layer->opacity = std::clamp(opacity, 0.0f, 1.0f);
    layer->tiling_step =
            std::isfinite(tiling_step) && tiling_step > 0.0f
            ? tiling_step
            : 0.1f;
    layer->stream_speed =
            std::isfinite(stream_speed)
            ? stream_speed
            : 0.0f;
    layer->water_layer_index = water_layer_index;
    layer->bottom = bottom;
    layer->mesh_layer_index =
            bottom
            ? FindOrAddOpaqueWorldObjectLayer(mesh, texture_path)
            : FindOrAddAlphaWorldObjectLayer(
                      mesh,
                      texture_path,
                      layer->stream_speed);
    return layer->mesh_layer_index !=
            std::numeric_limits<size_t>::max();
}

std::vector<RiverPointGeometry> BuildRiverPointGeometry(
        const NDb::SVSOInstance& river,
        const NDb::SRiverDesc& descriptor,
        const STerrainInfo& terrain_info,
        LegacyRiverRandom* random) {
    std::vector<RiverPointGeometry> points;
    points.reserve(river.points.size());
    float previous_bottom_pre_height =
            std::numeric_limits<float>::max();
    for (size_t index = 0;
         index < river.points.size();
         ++index) {
        const NDb::SVSOPoint& source = river.points[index];
        RiverPointGeometry point;
        const float border_offset = random == nullptr
                ? 0.0f
                : random->Range(
                          -descriptor.fBorderRand * 0.5f,
                          descriptor.fBorderRand * 0.5f);
        point.center_x =
                AI2Vis(source.vPos.x) +
                source.vNorm.x * border_offset;
        point.center_y =
                AI2Vis(source.vPos.y) +
                source.vNorm.y * border_offset;
        point.half_width = AI2Vis(source.fWidth);
        point.normal_x = source.vNorm.x;
        point.normal_y = source.vNorm.y;
        float normal_length = std::sqrt(
                point.normal_x * point.normal_x +
                point.normal_y * point.normal_y);
        if (!std::isfinite(normal_length) ||
            normal_length <= 0.0001f) {
            const NDb::SVSOPoint* adjacent =
                    index + 1 < river.points.size()
                    ? &river.points[index + 1]
                    : (index > 0 ? &river.points[index - 1] : nullptr);
            if (adjacent != nullptr) {
                const float direction_x =
                        AI2Vis(adjacent->vPos.x - source.vPos.x);
                const float direction_y =
                        AI2Vis(adjacent->vPos.y - source.vPos.y);
                normal_length = std::sqrt(
                        direction_x * direction_x +
                        direction_y * direction_y);
                if (normal_length > 0.0001f) {
                    point.normal_x = -direction_y / normal_length;
                    point.normal_y = direction_x / normal_length;
                    normal_length = 1.0f;
                }
            }
        }
        if (!std::isfinite(point.center_x) ||
            !std::isfinite(point.center_y) ||
            !std::isfinite(point.half_width) ||
            point.half_width <= 0.001f ||
            !std::isfinite(normal_length) ||
            normal_length <= 0.0001f) {
            continue;
        }
        point.normal_x /= normal_length;
        point.normal_y /= normal_length;
        point.opacity = std::clamp(source.fOpacity, 0.0f, 1.0f);

        const float left_x =
                point.center_x -
                point.normal_x * point.half_width;
        const float left_y =
                point.center_y -
                point.normal_y * point.half_width;
        const float right_x =
                point.center_x +
                point.normal_x * point.half_width;
        const float right_y =
                point.center_y +
                point.normal_y * point.half_width;
        const float bottom_pre_height = std::min(
                std::min(
                        TerrainHeightAt(terrain_info, left_x, left_y),
                        TerrainHeightAt(terrain_info, right_x, right_y)) -
                        kLegacyRiverDepth,
                previous_bottom_pre_height);
        point.bottom_height =
                std::max(bottom_pre_height, 0.0f);
        point.water_height =
                std::max(
                        bottom_pre_height +
                                kLegacyRiverWaterLevel,
                        point.bottom_height + 0.04f) +
                kLegacyRiverWaterHeightBias;
        previous_bottom_pre_height = bottom_pre_height;
        points.push_back(point);
    }
    return points;
}

void BuildRiverDisturbanceOffsets(
        const NDb::SRiverDesc& descriptor,
        LegacyRiverRandom* random,
        std::vector<RiverPointGeometry>* points) {
    if (random == nullptr || points == nullptr) {
        return;
    }
    for (RiverPointGeometry& point : *points) {
        const size_t layer_count =
                std::min<size_t>(descriptor.waterLayers.size(), 2);
        for (size_t layer_index = 0;
             layer_index < layer_count;
             ++layer_index) {
            const NDb::SVSOLayerCenterDesc& layer =
                    descriptor.waterLayers[layer_index];
            const int cell_count = std::max(layer.nNumCells, 0);
            std::vector<float>& offsets =
                    point.water_disturbance_offsets[layer_index];
            offsets.assign(static_cast<size_t>(cell_count), 0.0f);
            for (int cell = 1;
                 cell + 1 < cell_count;
                 ++cell) {
                offsets[static_cast<size_t>(cell)] =
                        random->Range(
                                -layer.fDisturbance,
                                layer.fDisturbance);
                ++g_river_disturbed_vertex_count;
            }
        }
    }
}

void AppendRiverLayerSegment(
        const RiverPointGeometry& current,
        const RiverPointGeometry& next,
        float texture_v,
        float next_texture_v,
        const RiverRenderLayer& river_layer,
        WorldObjectMesh* mesh) {
    if (mesh == nullptr ||
        river_layer.mesh_layer_index >= mesh->layers.size()) {
        return;
    }
    const uint32_t vertex_base =
            static_cast<uint32_t>(mesh->vertices.size());
    const auto append_row =
            [&](const RiverPointGeometry& point, float v) {
        for (int cell = 0; cell < river_layer.cells; ++cell) {
            const float u =
                    static_cast<float>(cell) /
                    static_cast<float>(river_layer.cells - 1);
            const float side = u * 2.0f - 1.0f;
            float x =
                    point.center_x +
                    point.normal_x *
                            point.half_width *
                            river_layer.width_scale *
                            side;
            float y =
                    point.center_y +
                    point.normal_y *
                            point.half_width *
                            river_layer.width_scale *
                            side;
            if (river_layer.water_layer_index >= 0 &&
                river_layer.water_layer_index <
                        static_cast<int>(
                                point.water_disturbance_offsets.size())) {
                const std::vector<float>& offsets =
                        point.water_disturbance_offsets[
                                static_cast<size_t>(
                                        river_layer.water_layer_index)];
                if (cell < static_cast<int>(offsets.size())) {
                    x += point.normal_x *
                            offsets[static_cast<size_t>(cell)];
                    y += point.normal_y *
                            offsets[static_cast<size_t>(cell)];
                }
            }
            float z = point.water_height;
            float alpha_factor = 1.0f;
            if (river_layer.bottom) {
                z = point.bottom_height +
                        u * (1.0f - u) * 0.1f;
            } else if (cell == 0 ||
                       cell + 1 == river_layer.cells) {
                alpha_factor = 0.0f;
            }
            const uint32_t alpha = static_cast<uint32_t>(
                    std::lround(
                            std::clamp(
                                    point.opacity *
                                            river_layer.opacity *
                                            alpha_factor,
                                    0.0f,
                                    1.0f) *
                            255.0f));
            mesh->vertices.push_back(TerrainVertex{
                    x,
                    y,
                    z,
                    u,
                    v,
                    (alpha << 24) | 0x00ffffffu});
        }
    };
    append_row(current, texture_v);
    append_row(next, next_texture_v);

    WorldObjectMesh::Layer& output =
            mesh->layers[river_layer.mesh_layer_index];
    for (int cell = 0;
         cell + 1 < river_layer.cells;
         ++cell) {
        const uint32_t current_left =
                vertex_base + static_cast<uint32_t>(cell);
        const uint32_t current_right = current_left + 1;
        const uint32_t next_left =
                vertex_base +
                static_cast<uint32_t>(river_layer.cells + cell);
        const uint32_t next_right = next_left + 1;
        output.triangle_indices.push_back(current_left);
        output.triangle_indices.push_back(next_left);
        output.triangle_indices.push_back(current_right);
        output.triangle_indices.push_back(current_right);
        output.triangle_indices.push_back(next_left);
        output.triangle_indices.push_back(next_right);
        g_river_triangle_count += 2;
    }
}

bool AppendRiverInstance(
        const NDb::SVSOInstance& river,
        const STerrainInfo& terrain_info,
        WorldObjectMesh* mesh,
        std::unordered_set<std::string>* texture_paths) {
    if (mesh == nullptr ||
        river.points.size() < 2 ||
        !river.pDescriptor) {
        return false;
    }
    const NDb::SVSODesc* base_descriptor =
            river.pDescriptor.GetPtr();
    if (base_descriptor == nullptr ||
        base_descriptor->GetTypeID() != NDb::SRiverDesc::typeID) {
        return false;
    }
    const NDb::SRiverDesc* descriptor =
            static_cast<const NDb::SRiverDesc*>(
                    base_descriptor);
    std::vector<RiverRenderLayer> layers;
    layers.reserve(3);
    const auto add_layer =
            [&](const NDb::SMaterial* material,
                const char* fallback_texture_name,
                int cells,
                float width_scale,
                float opacity,
                float tiling_step,
                float stream_speed,
                int water_layer_index,
                bool bottom) {
        RiverRenderLayer layer;
        if (!ConfigureRiverRenderLayer(
                    descriptor,
                    material,
                    fallback_texture_name,
                    cells,
                    width_scale,
                    opacity,
                    tiling_step,
                    stream_speed,
                    water_layer_index,
                    bottom,
                    mesh,
                    &layer)) {
            return;
        }
        if (texture_paths != nullptr) {
            texture_paths->insert(
                    mesh->layers[layer.mesh_layer_index].texture_path);
        }
        layers.push_back(layer);
    };
    add_layer(
            descriptor->pBottomMaterial.GetPtr(),
            "bottom",
            kLegacyRiverBottomCells,
            1.0f,
            1.0f,
            0.1f,
            0.0f,
            -1,
            true);
    for (size_t layer_index = 0;
         layer_index < descriptor->waterLayers.size() &&
         layer_index < 2;
         ++layer_index) {
        const NDb::SVSOLayerCenterDesc& source =
                descriptor->waterLayers[layer_index];
        const NDb::SMaterial* material =
                source.materials.empty()
                ? nullptr
                : source.materials.front().GetPtr();
        add_layer(
                material,
                layer_index == 0 ? "water" : "water2",
                source.nNumCells,
                layer_index == 0
                        ? 1.0f + kLegacyRiverWaterExpand
                        : 1.0f - kLegacyRiverWaterExpand,
                source.fCenterOpacity,
                source.fTilingStep,
                source.fStreamSpeed,
                static_cast<int>(layer_index),
                false);
    }
    if (layers.empty()) {
        return false;
    }

    LegacyRiverRandom river_random(LegacyRiverSeed(river));
    std::vector<RiverPointGeometry> points =
            BuildRiverPointGeometry(
                    river,
                    *descriptor,
                    terrain_info,
                    &river_random);
    if (points.size() < 2) {
        return false;
    }
    BuildRiverDisturbanceOffsets(
            *descriptor,
            &river_random,
            &points);
    std::vector<float> texture_v(layers.size(), 0.0f);
    for (size_t point_index = 0;
         point_index + 1 < points.size();
         ++point_index) {
        const RiverPointGeometry& current = points[point_index];
        const RiverPointGeometry& next = points[point_index + 1];
        const float dx = next.center_x - current.center_x;
        const float dy = next.center_y - current.center_y;
        const float segment_length = std::sqrt(dx * dx + dy * dy);
        if (!std::isfinite(segment_length) ||
            segment_length <= 0.001f) {
            continue;
        }
        for (size_t layer_index = 0;
             layer_index < layers.size();
             ++layer_index) {
            const RiverRenderLayer& layer = layers[layer_index];
            const float next_v =
                    texture_v[layer_index] +
                    (layer.bottom
                             ? segment_length /
                                       std::max(
                                               next.half_width * 2.0f,
                                               0.001f)
                             : layer.tiling_step);
            AppendRiverLayerSegment(
                    current,
                    next,
                    texture_v[layer_index],
                    next_v,
                    layer,
                    mesh);
            texture_v[layer_index] = next_v;
        }
    }
    g_river_point_count += points.size();
    return true;
}

void AppendTerrainRivers(
        const NDb::SMapInfo* map,
        const STerrainInfo& terrain_info,
        WorldObjectMesh* mesh) {
    g_river_instance_count = 0;
    g_river_point_count = 0;
    g_river_triangle_count = 0;
    g_river_texture_count = 0;
    g_river_texture_gpu_count = 0;
    g_river_disturbed_vertex_count = 0;
    g_river_animated_layer_count = 0;
    g_river_texture_logged = false;
    if (map == nullptr || mesh == nullptr) {
        return;
    }
    std::unordered_set<std::string> texture_paths;
    bool first_descriptor_logged = false;
    for (const NDb::SVSOInstance& river : map->rivers) {
        if (!first_descriptor_logged) {
            const NDb::SVSODesc* descriptor =
                    river.pDescriptor
                    ? river.pDescriptor.GetPtr()
                    : nullptr;
            std::ostringstream descriptor_report;
            descriptor_report
                    << "terrain_river_descriptor="
                    << (descriptor == nullptr
                                ? "<unavailable>"
                                : descriptor->GetDBID().ToString().c_str())
                    << "; type="
                    << (descriptor == nullptr
                                ? -1
                                : descriptor->GetTypeID())
                    << "; expected_type="
                    << NDb::SRiverDesc::typeID
                    << "; points=" << river.points.size()
                    << "; bottom_texture="
                    << FallbackRiverTexturePath(
                               descriptor,
                               "bottom")
                    << "; water_texture="
                    << FallbackRiverTexturePath(
                               descriptor,
                               "water")
                    << "; water2_texture="
                    << FallbackRiverTexturePath(
                               descriptor,
                               "water2");
            if (descriptor != nullptr &&
                descriptor->GetTypeID() ==
                        NDb::SRiverDesc::typeID) {
                const NDb::SRiverDesc* river_descriptor =
                        static_cast<const NDb::SRiverDesc*>(
                                descriptor);
                descriptor_report
                        << "; water_layers="
                        << river_descriptor->waterLayers.size();
            }
            PlatformRuntime::instance().log_info(
                    descriptor_report.str());
            first_descriptor_logged = true;
        }
        if (AppendRiverInstance(
                    river,
                    terrain_info,
                    mesh,
                    &texture_paths)) {
            ++g_river_instance_count;
        }
    }
    g_river_texture_count = texture_paths.size();
    g_river_animated_layer_count = static_cast<size_t>(
            std::count_if(
                    mesh->layers.begin(),
                    mesh->layers.end(),
                    [](const WorldObjectMesh::Layer& layer) {
                        return layer.texture_v_scroll_speed != 0.0f;
                    }));
    std::ostringstream report;
    report << "terrain_rivers="
           << (g_river_instance_count > 0 ? "ready" : "empty")
           << "; descriptors=" << map->rivers.size()
           << "; rendered=" << g_river_instance_count
           << "; points=" << g_river_point_count
           << "; triangles=" << g_river_triangle_count
           << "; textures=" << g_river_texture_count
           << "; disturbed_vertices="
           << g_river_disturbed_vertex_count
           << "; animated_layers="
           << g_river_animated_layer_count;
    PlatformRuntime::instance().log_info(report.str());
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
    // No incremental reserve: growing by exactly what each call needs turns
    // push_back's geometric growth into a full reallocation every time, which
    // is quadratic over a frame's worth of parts.
    for (uint32_t index = 0; index < index_count; ++index) {
        output->push_back(
                vertex_base +
                part.triangle_indices[first_index + index]);
    }
}

size_t ConvertedGeometryFrameIndex(
        const ConvertedGeometryPart& part,
        ConvertedAnimationVariant animation_variant,
        float animation_time_seconds) {
    if (part.animation_frames.size() <= 1 ||
        part.animation_duration_seconds <= 0.0f) {
        return 0;
    }
    const float animation_time =
            animation_variant == ConvertedAnimationVariant::Death
            ? std::min(
                      std::max(animation_time_seconds, 0.0f),
                      part.animation_duration_seconds)
            : std::fmod(
                      std::max(animation_time_seconds, 0.0f),
                      part.animation_duration_seconds);
    return std::min(
            static_cast<size_t>(
                    animation_time /
                    part.animation_duration_seconds *
                    part.animation_frames.size()),
            part.animation_frames.size() - 1);
}

const std::vector<ConvertedGeometryVertex>*
SelectConvertedGeometryVertices(
        const ConvertedGeometryPart& part,
        ConvertedAnimationVariant animation_variant,
        float animation_time_seconds) {
    if (part.animation_frames.size() <= 1 ||
        part.animation_duration_seconds <= 0.0f) {
        return &part.vertices;
    }
    return &part.animation_frames[ConvertedGeometryFrameIndex(
            part,
            animation_variant,
            animation_time_seconds)];
}

struct ProjectedShadowPoint {
    float x;
    float y;
};

// The projected silhouette only depends on the model, its animation frame and
// its heading: moving the unit just translates the hull. Rebuilding it (sort +
// convex hull over every vertex) for every unit every frame was the single
// most expensive thing in the frame, so the hull is cached in origin-relative
// space and the heading is quantised to keep the cache bounded.
constexpr size_t kProjectedShadowHeadingBuckets = 64;
constexpr size_t kProjectedShadowCacheLimit = 8192;

std::unordered_map<uint64_t, std::vector<ProjectedShadowPoint> >
        g_projected_shadow_hulls;

void MixProjectedShadowKey(uint64_t* key, uint64_t value) {
    *key ^= value + 0x9e3779b97f4a7c15ull + (*key << 6) + (*key >> 2);
}

float ProjectedShadowCross(
        const ProjectedShadowPoint& origin,
        const ProjectedShadowPoint& left,
        const ProjectedShadowPoint& right) {
    return (left.x - origin.x) * (right.y - origin.y) -
            (left.y - origin.y) * (right.x - origin.x);
}

void AppendProjectedShadowHull(
        WorldObjectMesh* mesh,
        const std::vector<ProjectedShadowPoint>& hull,
        float x,
        float y,
        float z) {
    if (hull.size() < 3) {
        return;
    }
    const uint32_t vertex_base =
            static_cast<uint32_t>(mesh->vertices.size());
    constexpr uint32_t kShadowArgb = 0x5811170du;
    const uint32_t shadow_abgr = ArgbToAbgr(kShadowArgb);
    for (const ProjectedShadowPoint& point : hull) {
        mesh->vertices.push_back(TerrainVertex{
                x + point.x,
                y + point.y,
                z + 0.08f,
                0.0f,
                0.0f,
                shadow_abgr});
    }
    WorldObjectMesh::Layer* shadow_layer =
            FindOrAddWorldObjectLayer(
                    mesh,
                    kProjectedShadowLayer);
    if (shadow_layer == nullptr) {
        return;
    }
    shadow_layer->alpha_blended = true;
    for (uint32_t index = 1;
         index + 1 < static_cast<uint32_t>(hull.size());
         ++index) {
        shadow_layer->triangle_indices.push_back(vertex_base);
        shadow_layer->triangle_indices.push_back(vertex_base + index);
        shadow_layer->triangle_indices.push_back(vertex_base + index + 1);
    }
}

void AppendProjectedGeometryShadow(
        WorldObjectMesh* mesh,
        const ConvertedGeometry& geometry,
        const GeometryBinding& binding,
        float x,
        float y,
        float z,
        float cosine,
        float sine,
        ConvertedAnimationVariant animation_variant,
        float animation_time_seconds) {
    if (mesh == nullptr) {
        return;
    }
    constexpr float kShadowProjectionX = 0.42f;
    constexpr float kShadowProjectionY = -0.28f;
    const float heading = std::atan2(sine, cosine);
    const size_t heading_bucket = static_cast<size_t>(
            ((static_cast<int>(std::lround(
                      heading / (2.0f * kPi) *
                      static_cast<float>(
                              kProjectedShadowHeadingBuckets))) %
              static_cast<int>(kProjectedShadowHeadingBuckets)) +
             static_cast<int>(kProjectedShadowHeadingBuckets)) %
            static_cast<int>(kProjectedShadowHeadingBuckets));
    uint64_t cache_key = 0xcbf29ce484222325ull;
    MixProjectedShadowKey(
            &cache_key,
            static_cast<uint64_t>(binding.geometry_record_id) + 1ull);
    MixProjectedShadowKey(
            &cache_key,
            static_cast<uint64_t>(animation_variant));
    MixProjectedShadowKey(&cache_key, heading_bucket);
    MixProjectedShadowKey(
            &cache_key,
            static_cast<uint64_t>(
                    std::lround(binding.geometry_scale * 4096.0f)));
    for (const ConvertedGeometryPart& part : geometry.parts) {
        MixProjectedShadowKey(
                &cache_key,
                ConvertedGeometryFrameIndex(
                        part,
                        animation_variant,
                        animation_time_seconds));
    }
    const auto cached = g_projected_shadow_hulls.find(cache_key);
    if (cached != g_projected_shadow_hulls.end()) {
        AppendProjectedShadowHull(mesh, cached->second, x, y, z);
        return;
    }

    const float bucket_angle =
            static_cast<float>(heading_bucket) * 2.0f * kPi /
            static_cast<float>(kProjectedShadowHeadingBuckets);
    const float bucket_cosine = std::cos(bucket_angle);
    const float bucket_sine = std::sin(bucket_angle);
    std::vector<ProjectedShadowPoint> points;
    for (const ConvertedGeometryPart& part : geometry.parts) {
        const std::vector<ConvertedGeometryVertex>* vertices =
                SelectConvertedGeometryVertices(
                        part,
                        animation_variant,
                        animation_time_seconds);
        points.reserve(points.size() + vertices->size());
        for (const ConvertedGeometryVertex& vertex : *vertices) {
            const float local_x =
                    binding.geometry_scale *
                    (bucket_cosine * vertex.x - bucket_sine * vertex.y);
            const float local_y =
                    binding.geometry_scale *
                    (bucket_sine * vertex.x + bucket_cosine * vertex.y);
            const float local_height = std::max(
                    binding.geometry_scale * vertex.z,
                    0.0f);
            points.push_back({
                    local_x + local_height * kShadowProjectionX,
                    local_y + local_height * kShadowProjectionY});
        }
    }
    if (points.size() < 3) {
        g_projected_shadow_hulls[cache_key];
        return;
    }
    std::sort(
            points.begin(),
            points.end(),
            [](const ProjectedShadowPoint& left,
               const ProjectedShadowPoint& right) {
                return left.x != right.x
                        ? left.x < right.x
                        : left.y < right.y;
            });
    points.erase(
            std::unique(
                    points.begin(),
                    points.end(),
                    [](const ProjectedShadowPoint& left,
                       const ProjectedShadowPoint& right) {
                        return std::abs(left.x - right.x) < 0.0001f &&
                                std::abs(left.y - right.y) < 0.0001f;
                    }),
            points.end());
    if (points.size() < 3) {
        g_projected_shadow_hulls[cache_key];
        return;
    }
    std::vector<ProjectedShadowPoint> hull(points.size() * 2);
    size_t hull_size = 0;
    for (const ProjectedShadowPoint& point : points) {
        while (hull_size >= 2 &&
               ProjectedShadowCross(
                       hull[hull_size - 2],
                       hull[hull_size - 1],
                       point) <= 0.0f) {
            --hull_size;
        }
        hull[hull_size++] = point;
    }
    const size_t lower_size = hull_size;
    for (size_t index = points.size() - 1; index > 0; --index) {
        const ProjectedShadowPoint& point = points[index - 1];
        while (hull_size > lower_size &&
               ProjectedShadowCross(
                       hull[hull_size - 2],
                       hull[hull_size - 1],
                       point) <= 0.0f) {
            --hull_size;
        }
        hull[hull_size++] = point;
    }
    if (hull_size <= 3) {
        g_projected_shadow_hulls[cache_key];
        return;
    }
    --hull_size;
    // hull was sized for the worst case; store only the points it kept, or
    // every cache entry drags the whole scratch allocation along with it.
    std::vector<ProjectedShadowPoint> compact(
            hull.begin(),
            hull.begin() + static_cast<std::ptrdiff_t>(hull_size));
    if (g_projected_shadow_hulls.size() >= kProjectedShadowCacheLimit) {
        g_projected_shadow_hulls.clear();
    }
    const std::vector<ProjectedShadowPoint>& stored =
            g_projected_shadow_hulls
                    .emplace(cache_key, std::move(compact))
                    .first->second;
    AppendProjectedShadowHull(mesh, stored, x, y, z);
}

void AppendAlphaMaskedGeometryShadow(
        WorldObjectMesh* mesh,
        const ConvertedGeometry& geometry,
        const GeometryBinding& binding,
        float x,
        float y,
        float z,
        float cosine,
        float sine,
        ConvertedAnimationVariant animation_variant,
        float animation_time_seconds) {
    if (mesh == nullptr || binding.texture_paths.empty()) {
        return;
    }
    constexpr float kShadowProjectionX = 0.42f;
    constexpr float kShadowProjectionY = -0.28f;
    constexpr uint32_t kShadowArgb = 0x3411170du;
    const uint32_t shadow_abgr = ArgbToAbgr(kShadowArgb);
    int material_base = 0;
    for (size_t part_index = 0;
         part_index < geometry.parts.size();
         ++part_index) {
        const ConvertedGeometryPart& part = geometry.parts[part_index];
        const std::vector<ConvertedGeometryVertex>* vertices =
                SelectConvertedGeometryVertices(
                        part,
                        animation_variant,
                        animation_time_seconds);
        const uint32_t vertex_base =
                static_cast<uint32_t>(mesh->vertices.size());
        for (const ConvertedGeometryVertex& vertex : *vertices) {
            const float local_x =
                    binding.geometry_scale *
                    (cosine * vertex.x - sine * vertex.y);
            const float local_y =
                    binding.geometry_scale *
                    (sine * vertex.x + cosine * vertex.y);
            const float local_height = std::max(
                    binding.geometry_scale * vertex.z,
                    0.0f);
            mesh->vertices.push_back(TerrainVertex{
                    x + local_x + local_height * kShadowProjectionX,
                    y + local_y + local_height * kShadowProjectionY,
                    z + 0.08f,
                    vertex.u,
                    vertex.v,
                    shadow_abgr});
        }

        if (binding.material_quantities.empty()) {
            const int material_index = std::min(
                    static_cast<int>(part_index),
                    static_cast<int>(binding.texture_paths.size()) - 1);
            WorldObjectMesh::Layer* layer =
                    FindOrAddWorldObjectLayer(
                            mesh,
                            binding.texture_paths[material_index],
                            true);
            if (layer != nullptr) {
                layer->alpha_blended = true;
                AppendPartIndices(
                        &layer->triangle_indices,
                        part,
                        vertex_base,
                        0,
                        static_cast<uint32_t>(
                                part.triangle_indices.size()));
            }
            continue;
        }

        const int material_count =
                part_index < binding.material_quantities.size()
                ? binding.material_quantities[part_index]
                : 0;
        for (const ConvertedGeometryGroup& group : part.groups) {
            const int material_index = material_count > 0
                    ? material_base +
                            std::min(
                                    static_cast<int>(
                                            group.material_index),
                                    material_count - 1)
                    : -1;
            if (material_index < 0 ||
                material_index >=
                        static_cast<int>(
                                binding.texture_paths.size())) {
                continue;
            }
            WorldObjectMesh::Layer* layer =
                    FindOrAddWorldObjectLayer(
                            mesh,
                            binding.texture_paths[material_index],
                            true);
            if (layer == nullptr) {
                continue;
            }
            layer->alpha_blended = true;
            AppendPartIndices(
                    &layer->triangle_indices,
                    part,
                    vertex_base,
                    group.first_index,
                    group.index_count);
        }
        material_base += material_count;
    }
}

void ResolveGeometryMaterialAlpha(
        const GeometryBinding& binding,
        int material_index,
        bool fallback_alpha_blended,
        bool fallback_alpha_tested,
        bool* alpha_blended,
        bool* alpha_tested) {
    *alpha_blended = fallback_alpha_blended;
    *alpha_tested = fallback_alpha_tested;
    if (material_index < 0 ||
        material_index >=
                static_cast<int>(
                        binding.material_alpha_modes.size())) {
        return;
    }
    switch (binding.material_alpha_modes[material_index]) {
        case GeometryMaterialAlphaMode::Opaque:
            *alpha_blended = false;
            *alpha_tested = false;
            break;
        case GeometryMaterialAlphaMode::Blend:
            *alpha_blended = true;
            *alpha_tested = false;
            break;
        case GeometryMaterialAlphaMode::Test:
            *alpha_blended = false;
            *alpha_tested = true;
            break;
        case GeometryMaterialAlphaMode::Inherit:
        default:
            break;
    }
}


// Vertices bound to a rotating platform bone (or any bone under it) are the
// turret/gun subtree the desktop client poses with a bone mutator.
std::vector<bool> ConvertedGeometryBoneSubtree(
        const ConvertedGeometryPart& part,
        const std::string& root_bone) {
    std::vector<bool> in_subtree(part.bones.size(), false);
    if (root_bone.empty() || part.bones.empty()) {
        return in_subtree;
    }
    for (size_t index = 0; index < part.bones.size(); ++index) {
        if (part.bones[index].name == root_bone) {
            in_subtree[index] = true;
        }
    }
    // Bones are stored parent-first, so one forward pass propagates.
    for (size_t index = 0; index < part.bones.size(); ++index) {
        const int32_t parent = part.bones[index].parent;
        if (parent >= 0 &&
            static_cast<size_t>(parent) < in_subtree.size() &&
            in_subtree[static_cast<size_t>(parent)]) {
            in_subtree[index] = true;
        }
    }
    return in_subtree;
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
        bool alpha_blended,
        bool alpha_tested,
        bool cast_projected_shadow,
        bool cast_alpha_masked_shadow,
        int frame_index,
        const std::string& turret_bone = std::string(),
        float turret_yaw = 0.0f,
        bool turret_aim_valid = false,
        float travelled_distance = 0.0f,
        const std::string& gun_bone = std::string(),
        float gun_pitch = 0.0f) {
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
    if (cast_projected_shadow && !g_shadows_disabled) {
        AppendProjectedGeometryShadow(
                mesh,
                *geometry,
                binding,
                x,
                y,
                z,
                cosine,
                sine,
                animation_variant,
                animation_time_seconds);
    }
    if (cast_alpha_masked_shadow && !g_shadows_disabled) {
        AppendAlphaMaskedGeometryShadow(
                mesh,
                *geometry,
                binding,
                x,
                y,
                z,
                cosine,
                sine,
                animation_variant,
                animation_time_seconds);
    }
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
                SelectConvertedGeometryVertices(
                        part,
                        animation_variant,
                        animation_time_seconds);
        const uint32_t vertex_base =
                static_cast<uint32_t>(mesh->vertices.size());
        // Pose the rotating platform before the hull heading is applied, so
        // the turret angle stays relative to the hull like the original.
        // Only a real sub-platform is posed. Many hulls name their root bone
        // as the rotate point, and turning the root would spin the whole
        // vehicle on top of the heading the hull already carries.
        size_t turret_bone_index = part.bones.size();
        for (size_t index = 0; index < part.bones.size(); ++index) {
            if (part.bones[index].name == turret_bone) {
                turret_bone_index = index;
                break;
            }
        }
        // Without a real aim from the AI the angles mean nothing, and posing
        // by them would counter-rotate the platform by the hull heading.
        bool pose_turret =
                turret_aim_valid &&
                !turret_bone.empty() &&
                part.vertex_bones.size() == vertices->size() &&
                turret_bone_index < part.bones.size() &&
                part.bones[turret_bone_index].parent >= 0;
        std::vector<bool> turret_subtree = pose_turret
                ? ConvertedGeometryBoneSubtree(part, turret_bone)
                : std::vector<bool>();
        // A platform that covers nearly the whole skeleton is the hull, not a
        // turret: those units aim by turning the vehicle, which the hull
        // heading already does. Posing them would swing the whole body.
        size_t subtree_size = 0;
        for (const bool member : turret_subtree) {
            subtree_size += member ? 1 : 0;
        }
        if (!part.bones.empty() &&
            subtree_size * 5 > part.bones.size() * 4) {
            pose_turret = false;
            turret_subtree.clear();
        }
        // The AI reports an absolute aim direction while the vertices are
        // still in model space, so the hull heading is taken back out.
        const float turret_local = turret_yaw - heading;
        if (pose_turret && !g_turret_pose_logged) {
            size_t posed = 0;
            for (const bool member : turret_subtree) {
                posed += member ? 1 : 0;
            }
            if (posed > 0) {
                std::ostringstream report;
                report << "mech_turret_pose=active"
                       << "; bone=" << turret_bone
                       << "; bones=" << part.bones.size()
                       << "; subtree=" << posed
                       << "/" << part.bones.size()
                       << "; yaw=" << turret_yaw
                       << "; local=" << turret_local;
                PlatformRuntime::instance().log_info(report.str());
                g_turret_pose_logged = true;
            }
        }
        float pivot_x = 0.0f;
        float pivot_y = 0.0f;
        if (pose_turret) {
            for (size_t index = 0; index < part.bones.size(); ++index) {
                if (part.bones[index].name == turret_bone) {
                    pivot_x = part.bones[index].pivot_x;
                    pivot_y = part.bones[index].pivot_y;
                    break;
                }
            }
        }
        const float turret_cosine = std::cos(turret_local);
        const float turret_sine = std::sin(turret_local);
        // The barrel elevates about the gun's own rotate point, in the
        // turret's rest frame; the platform yaw above then carries it round.
        size_t gun_bone_index = part.bones.size();
        if (pose_turret && !gun_bone.empty()) {
            for (size_t index = 0; index < part.bones.size(); ++index) {
                if (part.bones[index].name == gun_bone) {
                    gun_bone_index = index;
                    break;
                }
            }
        }
        const bool pose_gun =
                gun_bone_index < part.bones.size() &&
                part.bones[gun_bone_index].parent >= 0 &&
                gun_pitch != 0.0f;
        const std::vector<bool> gun_subtree = pose_gun
                ? ConvertedGeometryBoneSubtree(part, gun_bone)
                : std::vector<bool>();
        const float gun_pivot_y =
                pose_gun ? part.bones[gun_bone_index].pivot_y : 0.0f;
        const float gun_pivot_z =
                pose_gun ? part.bones[gun_bone_index].pivot_z : 0.0f;
        const float gun_cosine = std::cos(gun_pitch);
        const float gun_sine = std::sin(gun_pitch);
        // Road wheels turn with the ground the vehicle has covered, not with
        // time, so a stopped vehicle keeps its wheels still and a reversing
        // one turns them back.
        const bool roll_wheel =
                part.wheel_roll && travelled_distance != 0.0f;
        const float wheel_angle = roll_wheel
                ? -travelled_distance / part.wheel_radius
                : 0.0f;
        const float wheel_cosine = std::cos(wheel_angle);
        const float wheel_sine = std::sin(wheel_angle);
        if (roll_wheel) {
            ++g_wheel_roll_part_count;
            if (!g_wheel_roll_logged) {
                std::ostringstream report;
                report << "mech_wheel_roll=active"
                       << "; radius=" << part.wheel_radius
                       << "; pivot=" << part.wheel_pivot_y
                       << "," << part.wheel_pivot_z
                       << "; distance=" << travelled_distance
                       << "; angle=" << wheel_angle;
                PlatformRuntime::instance().log_info(report.str());
                g_wheel_roll_logged = true;
            }
        }
        for (size_t vertex_index = 0;
             vertex_index < vertices->size();
             ++vertex_index) {
            const ConvertedGeometryVertex& vertex =
                    (*vertices)[vertex_index];
            float local_x = vertex.x;
            float local_y = vertex.y;
            float local_z = vertex.z;
            if (roll_wheel) {
                const float offset_y = local_y - part.wheel_pivot_y;
                const float offset_z = local_z - part.wheel_pivot_z;
                local_y = part.wheel_pivot_y +
                        wheel_cosine * offset_y - wheel_sine * offset_z;
                local_z = part.wheel_pivot_z +
                        wheel_sine * offset_y + wheel_cosine * offset_z;
            }
            if (pose_gun) {
                const uint32_t bone = part.vertex_bones[vertex_index];
                if (bone < gun_subtree.size() && gun_subtree[bone]) {
                    const float offset_y = local_y - gun_pivot_y;
                    const float offset_z = local_z - gun_pivot_z;
                    local_y = gun_pivot_y +
                            gun_cosine * offset_y - gun_sine * offset_z;
                    local_z = gun_pivot_z +
                            gun_sine * offset_y + gun_cosine * offset_z;
                }
            }
            if (pose_turret) {
                const uint32_t bone = part.vertex_bones[vertex_index];
                if (bone < turret_subtree.size() && turret_subtree[bone]) {
                    const float offset_x = local_x - pivot_x;
                    const float offset_y = local_y - pivot_y;
                    local_x = pivot_x +
                            turret_cosine * offset_x -
                            turret_sine * offset_y;
                    local_y = pivot_y +
                            turret_sine * offset_x +
                            turret_cosine * offset_y;
                }
            }
            mesh->vertices.push_back(TerrainVertex{
                    x + binding.geometry_scale *
                            (cosine * local_x - sine * local_y),
                    y + binding.geometry_scale *
                            (sine * local_x + cosine * local_y),
                    z + binding.geometry_scale * local_z + 0.05f,
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
            bool material_alpha_blended = false;
            bool material_alpha_tested = false;
            ResolveGeometryMaterialAlpha(
                    binding,
                    material_index,
                    alpha_blended,
                    alpha_tested,
                    &material_alpha_blended,
                    &material_alpha_tested);
            WorldObjectMesh::Layer* layer =
                    FindOrAddMaterialWorldObjectLayer(
                            mesh,
                            texture_path,
                            material_alpha_blended,
                            material_alpha_tested);
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
            bool material_alpha_blended = false;
            bool material_alpha_tested = false;
            ResolveGeometryMaterialAlpha(
                    binding,
                    material_index,
                    alpha_blended,
                    alpha_tested,
                    &material_alpha_blended,
                    &material_alpha_tested);
            WorldObjectMesh::Layer* layer =
                    FindOrAddMaterialWorldObjectLayer(
                            mesh,
                            texture_path,
                            material_alpha_blended,
                            material_alpha_tested);
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
                false,
                false,
                true,
                false,
                -1,
                LegacyMechTurretBone(entity.rpg_stats_path_hash),
                entity.turret_yaw_radians,
                entity.turret_aim_valid != 0u,
                entity.travelled_distance,
                LegacyMechGunBone(entity.rpg_stats_path_hash),
                entity.turret_pitch_radians)) {
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
        const std::string& texture_path,
        bool additive_blended = false) {
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
    layer->alpha_blended = !additive_blended;
    layer->additive_blended = additive_blended;
    const uint32_t indices[] = {0, 1, 2, 0, 2, 3};
    for (uint32_t index : indices) {
        layer->triangle_indices.push_back(base + index);
    }
}

void AppendGroundFacingSprite(
        WorldObjectMesh* mesh,
        float x,
        float y,
        float z,
        float radius,
        uint32_t abgr,
        const std::string& texture_path) {
    if (mesh == nullptr || texture_path.empty() || radius <= 0.0f) {
        return;
    }
    const uint32_t base =
            static_cast<uint32_t>(mesh->vertices.size());
    mesh->vertices.push_back(TerrainVertex{
            x - radius,
            y - radius,
            z,
            0.0f,
            1.0f,
            abgr});
    mesh->vertices.push_back(TerrainVertex{
            x + radius,
            y - radius,
            z,
            1.0f,
            1.0f,
            abgr});
    mesh->vertices.push_back(TerrainVertex{
            x + radius,
            y + radius,
            z,
            1.0f,
            0.0f,
            abgr});
    mesh->vertices.push_back(TerrainVertex{
            x - radius,
            y + radius,
            z,
            0.0f,
            0.0f,
            abgr});
    WorldObjectMesh::Layer* layer =
            FindOrAddWorldObjectLayer(mesh, texture_path);
    if (layer == nullptr) {
        return;
    }
    layer->additive_blended = true;
    const uint32_t indices[] = {0, 1, 2, 0, 2, 3};
    for (uint32_t index : indices) {
        layer->triangle_indices.push_back(base + index);
    }
}

void AppendDescriptorEffectLights(
        WorldObjectMesh* mesh,
        const std::vector<AndroidEffectLight>& lights,
        float origin_x,
        float origin_y,
        float origin_z,
        uint32_t age_millis,
        uint32_t lifetime_millis) {
    if (mesh == nullptr || lights.empty() ||
        lifetime_millis == 0) {
        return;
    }
    const float age_seconds =
            static_cast<float>(age_millis) / 1000.0f;
    const size_t light_count =
            std::min<size_t>(lights.size(), 8);
    for (size_t light_index = 0;
         light_index < light_count;
         ++light_index) {
        const AndroidEffectLight& light = lights[light_index];
        const float speed =
                std::abs(light.speed) > 0.001f
                ? std::abs(light.speed)
                : 1.0f;
        const float local_seconds =
                age_seconds * speed -
                light.time_offset_seconds * speed;
        if (local_seconds < 0.0f) {
            continue;
        }
        const float cycle_seconds =
                light.end_cycle_seconds > 0.001f
                ? light.end_cycle_seconds
                : std::max(
                          static_cast<float>(lifetime_millis) /
                                  1000.0f,
                          0.1f);
        if (light.cycle_count > 0 &&
            local_seconds >=
                    cycle_seconds *
                            static_cast<float>(
                                    light.cycle_count)) {
            continue;
        }
        const float phase = std::clamp(
                std::fmod(local_seconds, cycle_seconds) /
                        cycle_seconds,
                0.0f,
                1.0f);
        const float fade_in = std::clamp(phase * 18.0f, 0.0f, 1.0f);
        const float envelope =
                fade_in * std::exp(-phase * 5.0f);
        const uint32_t alpha = static_cast<uint32_t>(
                std::lround(
                        180.0f *
                        std::clamp(envelope, 0.0f, 1.0f)));
        if (alpha == 0) {
            continue;
        }
        const float light_x = origin_x + light.offset_x;
        const float light_y = origin_y + light.offset_y;
        const float ground_z =
                TerrainMeshHeightAtLocked(light_x, light_y);
        const float elevation = std::max(
                origin_z + light.offset_z - ground_z,
                0.0f);
        const float radius =
                std::clamp(
                        11.0f * light.scale *
                                (0.75f + envelope * 0.35f) +
                                elevation * 0.25f,
                        3.5f,
                        30.0f);
        AppendGroundFacingSprite(
                mesh,
                light_x,
                light_y,
                ground_z + 0.12f,
                radius,
                ArgbToAbgr(
                        (alpha << 24) | 0x00ff8438u),
                kEffectLightTexture);
        ++g_effect_light_render_count;
    }
}

bool ParticlePathContains(
        const std::string& path,
        const char* needle) {
    if (needle == nullptr || *needle == '\0') {
        return false;
    }
    std::string lower_path(path);
    std::transform(
            lower_path.begin(),
            lower_path.end(),
            lower_path.begin(),
            [](unsigned char value) {
                return static_cast<char>(std::tolower(value));
            });
    return lower_path.find(needle) != std::string::npos;
}

void AppendDescriptorParticleEffect(
        WorldObjectMesh* mesh,
        const std::vector<AndroidParticleEmitter>& emitters,
        float origin_x,
        float origin_y,
        float origin_z,
        uint32_t age_millis,
        uint32_t lifetime_millis,
        int32_t seed) {
    if (mesh == nullptr || emitters.empty() ||
        lifetime_millis == 0) {
        return;
    }
    const float age_seconds =
            static_cast<float>(age_millis) / 1000.0f;
    const float lifetime_phase = std::clamp(
            static_cast<float>(age_millis) /
                    static_cast<float>(lifetime_millis),
            0.0f,
            1.0f);
    const size_t emitter_count =
            std::min<size_t>(emitters.size(), 16);
    for (size_t emitter_index = 0;
         emitter_index < emitter_count;
         ++emitter_index) {
        const AndroidParticleEmitter& emitter =
                emitters[emitter_index];
        if (emitter.textures.empty()) {
            continue;
        }
        const float speed =
                std::abs(emitter.speed) > 0.001f
                ? emitter.speed
                : 1.0f;
        const float local_seconds =
                age_seconds * speed -
                emitter.time_offset_seconds * speed;
        if (local_seconds < 0.0f) {
            continue;
        }
        const float cycle_seconds =
                emitter.end_cycle_seconds > 0.001f
                ? emitter.end_cycle_seconds
                : std::max(
                          static_cast<float>(lifetime_millis) /
                                  1000.0f,
                          0.1f);
        if (emitter.cycle_count > 0 &&
            local_seconds >=
                    cycle_seconds *
                            static_cast<float>(
                                    emitter.cycle_count)) {
            continue;
        }
        const float cycle_position =
                std::fmod(local_seconds, cycle_seconds);
        const float cycle_phase = std::clamp(
                cycle_position / cycle_seconds,
                0.0f,
                0.9999f);
        const AndroidParticleTexture& probe_texture =
                emitter.textures[static_cast<size_t>(
                        cycle_phase *
                        static_cast<float>(
                                emitter.textures.size()))];
        const bool smoke =
                ParticlePathContains(probe_texture.path, "smoke") ||
                ParticlePathContains(probe_texture.path, "dust") ||
                ParticlePathContains(probe_texture.path, "explosion2") ||
                ParticlePathContains(probe_texture.path, "explosion3");
        const bool fire =
                ParticlePathContains(probe_texture.path, "fire") ||
                ParticlePathContains(probe_texture.path, "flame");
        const bool flash =
                ParticlePathContains(probe_texture.path, "flash") ||
                ParticlePathContains(probe_texture.path, "shot");
        const int sprite_count = smoke ? 3 : fire ? 2 : 1;
        for (int sprite = 0;
             sprite < sprite_count;
             ++sprite) {
            const float sprite_phase = std::fmod(
                    cycle_phase +
                            static_cast<float>(sprite) /
                                    static_cast<float>(
                                            sprite_count),
                    1.0f);
            const size_t texture_index = std::min(
                    static_cast<size_t>(
                            sprite_phase *
                            static_cast<float>(
                                    emitter.textures.size())),
                    emitter.textures.size() - 1);
            const AndroidParticleTexture& texture =
                    emitter.textures[texture_index];
            const float texture_width =
                    static_cast<float>(
                            std::max(texture.width, 1));
            const float texture_height =
                    static_cast<float>(
                            std::max(texture.height, 1));
            const float aspect =
                    std::clamp(
                            texture_height / texture_width,
                            0.35f,
                            2.85f);
            float width = std::clamp(
                    texture_width / 32.0f *
                            std::max(emitter.scale, 0.05f),
                    0.8f,
                    12.0f);
            float height = std::clamp(
                    width * aspect,
                    0.8f,
                    16.0f);
            const float random_seed =
                    static_cast<float>(
                            (seed & 0xffff) +
                            static_cast<int>(emitter_index) * 97 +
                            sprite * 43);
            float x = origin_x + emitter.offset_x;
            float y = origin_y + emitter.offset_y;
            float z = origin_z + emitter.offset_z +
                    height * 0.5f;
            if (smoke) {
                const float drift =
                        (0.4f + sprite_phase * 1.8f) *
                        emitter.scale;
                x += std::sin(random_seed * 0.37f) * drift;
                y += std::cos(random_seed * 0.29f) * drift;
                z += sprite_phase *
                        (2.5f + 5.0f * emitter.scale);
                width *= 0.8f + sprite_phase * 0.8f;
                height *= 0.8f + sprite_phase * 0.8f;
            } else if (fire) {
                const float flicker =
                        0.9f +
                        0.12f *
                                std::sin(
                                        static_cast<float>(
                                                age_millis) *
                                                0.035f +
                                        random_seed);
                x += std::sin(random_seed) *
                        0.35f * emitter.scale;
                y += std::cos(random_seed) *
                        0.35f * emitter.scale;
                width *= flicker;
                height *= flicker;
            } else if (flash) {
                const float expansion =
                        0.75f + sprite_phase * 0.65f;
                width *= expansion;
                height *= expansion;
            }
            const float cycle_envelope = std::clamp(
                    std::min(
                            sprite_phase * 5.0f,
                            (1.0f - sprite_phase) * 5.0f),
                    0.0f,
                    1.0f);
            const float lifetime_envelope = std::clamp(
                    (1.0f - lifetime_phase) * 6.0f,
                    0.0f,
                    1.0f);
            const uint32_t alpha =
                    static_cast<uint32_t>(
                            std::lround(
                                    255.0f *
                                    cycle_envelope *
                                    lifetime_envelope));
            if (alpha == 0) {
                continue;
            }
            AppendCameraFacingSprite(
                    mesh,
                    x,
                    y,
                    z,
                    width,
                    height,
                    (alpha << 24) | 0x00ffffffu,
                    texture.path,
                    texture.additive);
        }
    }
}

void AppendSceneEffect(
        WorldObjectMesh* mesh,
        const AndroidSceneEffect& effect) {
    AppendDescriptorEffectLights(
            mesh,
            effect.lights,
            effect.x,
            effect.y,
            effect.z,
            effect.age_millis,
            effect.lifetime_millis);
    AppendDescriptorParticleEffect(
            mesh,
            effect.emitters,
            effect.x,
            effect.y,
            effect.z,
            effect.age_millis,
            effect.lifetime_millis,
            effect.victim_unit_id);
}

void AppendDestructionEffect(
        WorldObjectMesh* mesh,
        const AndroidDestructionEffect& effect) {
    if (mesh == nullptr || effect.lifetime_millis == 0) {
        return;
    }
    AppendDescriptorEffectLights(
            mesh,
            effect.lights,
            effect.x,
            effect.y,
            effect.z,
            effect.age_millis,
            effect.lifetime_millis);
    if (!effect.emitters.empty()) {
        AppendDescriptorParticleEffect(
                mesh,
                effect.emitters,
                effect.x,
                effect.y,
                effect.z,
                effect.age_millis,
                effect.lifetime_millis,
                effect.unit_id);
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
                type_id == NDb::SObjectRPGStats::typeID ||
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
        const std::string stats_path =
                stats->GetDBID().ToString().c_str();
        std::string normalized_stats_path = stats_path;
        std::replace(
                normalized_stats_path.begin(),
                normalized_stats_path.end(),
                '\\',
                '/');
        std::transform(
                normalized_stats_path.begin(),
                normalized_stats_path.end(),
                normalized_stats_path.begin(),
                [](unsigned char value) {
                    return static_cast<char>(std::tolower(value));
                });
        const bool flora =
                stats->eGameType == NDb::SGVOGT_FLORA ||
                normalized_stats_path.find("objects/flora/") !=
                        std::string::npos;
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
                    false,
                    flora,
                    !flora,
                    flora,
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
    AppendTerrainPrecipices(map, terrain_info, mesh);
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
    AppendTerrainRivers(map, terrain_info, mesh);
    AppendTerrainRoads(map, terrain_info, mesh);
    g_material_alpha_test_layer_count = 0;
    g_material_alpha_test_triangle_count = 0;
    g_material_alpha_blend_layer_count = 0;
    g_material_alpha_blend_triangle_count = 0;
    for (const WorldObjectMesh::Layer& layer : mesh->layers) {
        if (layer.alpha_tested && !layer.alpha_masked_shadow) {
            ++g_material_alpha_test_layer_count;
            g_material_alpha_test_triangle_count +=
                    layer.triangle_indices.size() / 3;
        }
        if (layer.alpha_blended &&
            !layer.additive_blended &&
            !layer.alpha_masked_shadow) {
            ++g_material_alpha_blend_layer_count;
            g_material_alpha_blend_triangle_count +=
                    layer.triangle_indices.size() / 3;
        }
    }
    std::ostringstream material_report;
    material_report
            << "world_material_alpha=ready"
            << "; test_layers="
            << g_material_alpha_test_layer_count
            << "; test_triangles="
            << g_material_alpha_test_triangle_count
            << "; blend_layers="
            << g_material_alpha_blend_layer_count
            << "; blend_triangles="
            << g_material_alpha_blend_triangle_count;
    PlatformRuntime::instance().log_info(material_report.str());
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

void ApplyStaticLayerTextureAnimation(WorldObjectMesh* mesh) {
    if (mesh == nullptr ||
        g_river_animated_layer_count == 0 ||
        g_animation_elapsed_seconds == 0.0f) {
        return;
    }
    std::vector<uint8_t> adjusted(mesh->vertices.size(), 0);
    for (const WorldObjectMesh::Layer& layer : mesh->layers) {
        if (layer.texture_v_scroll_speed == 0.0f) {
            continue;
        }
        const float offset =
                g_animation_elapsed_seconds *
                layer.texture_v_scroll_speed;
        for (uint32_t vertex_index : layer.triangle_indices) {
            if (vertex_index >= mesh->vertices.size() ||
                adjusted[vertex_index] != 0) {
                continue;
            }
            mesh->vertices[vertex_index].v -= offset;
            adjusted[vertex_index] = 1;
        }
    }
}

struct TickBudget {
    std::chrono::steady_clock::time_point window_start;
    uint64_t ticks = 0;
    uint64_t legacy_micros = 0;
    uint64_t mesh_micros = 0;
    uint64_t water_micros = 0;
    bool started = false;
};

TickBudget g_tick_budget;

uint64_t ElapsedMicros(
        std::chrono::steady_clock::time_point from,
        std::chrono::steady_clock::time_point to) {
    return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(to - from)
                    .count());
}

// One line every five seconds: enough to spot a regression, cheap enough to
// leave switched on.
void ReportTickBudgetLocked(std::chrono::steady_clock::time_point now) {
    if (!g_tick_budget.started) {
        g_tick_budget.started = true;
        g_tick_budget.window_start = now;
        return;
    }
    const uint64_t window_micros = ElapsedMicros(g_tick_budget.window_start, now);
    if (window_micros < 5000000ull) {
        return;
    }
    std::ostringstream report;
    report << "tick_budget=" << g_tick_budget.ticks << " ticks/"
           << (window_micros / 1000ull) << "ms"
           << "; legacy_ms=" << (g_tick_budget.legacy_micros / 1000ull)
           << "; mesh_ms=" << (g_tick_budget.mesh_micros / 1000ull)
           << "; water_ms=" << (g_tick_budget.water_micros / 1000ull)
           << "; static_verts=" << g_static_world_object_mesh.vertices.size()
           << "; dynamic_verts=" << g_world_object_mesh.vertices.size()
           << "; entities=" << g_dynamic_rendered_object_count
           << "; culled=" << g_culled_entity_count
           << "; shadow_cache=" << g_projected_shadow_hulls.size()
           << "; wheel_parts=" << g_wheel_roll_part_count
;
    PlatformRuntime::instance().log_info(report.str());
    g_wheel_roll_part_count = 0;
    g_tick_budget = TickBudget();
    g_tick_budget.started = true;
    g_tick_budget.window_start = now;
}

// Moves the scrolling-texture layers out of the mission's static mesh into a
// compact mesh of their own, so the bulk of the world can stay on the GPU
// untouched while only the rivers are re-uploaded each frame.
void SplitAnimatedStaticLayersLocked() {
    g_animated_static_world_object_mesh = WorldObjectMesh();
    WorldObjectMesh& source = g_static_world_object_mesh;
    WorldObjectMesh& animated = g_animated_static_world_object_mesh;
    std::vector<WorldObjectMesh::Layer> kept;
    kept.reserve(source.layers.size());
    std::vector<uint32_t> remap(source.vertices.size(), UINT32_MAX);
    for (WorldObjectMesh::Layer& layer : source.layers) {
        if (layer.texture_v_scroll_speed == 0.0f) {
            kept.push_back(std::move(layer));
            continue;
        }
        WorldObjectMesh::Layer moved;
        moved.texture_path = layer.texture_path;
        moved.texture_handle = layer.texture_handle;
        moved.alpha_blended = layer.alpha_blended;
        moved.additive_blended = layer.additive_blended;
        moved.alpha_tested = layer.alpha_tested;
        moved.alpha_masked_shadow = layer.alpha_masked_shadow;
        moved.depth_test_always = layer.depth_test_always;
        moved.texture_v_scroll_speed = layer.texture_v_scroll_speed;
        moved.triangle_indices.reserve(layer.triangle_indices.size());
        for (uint32_t vertex_index : layer.triangle_indices) {
            if (vertex_index >= source.vertices.size()) {
                continue;
            }
            uint32_t& mapped = remap[vertex_index];
            if (mapped == UINT32_MAX) {
                mapped = static_cast<uint32_t>(animated.vertices.size());
                animated.vertices.push_back(source.vertices[vertex_index]);
            }
            moved.triangle_indices.push_back(mapped);
        }
        animated.layers.push_back(std::move(moved));
    }
    source.layers = std::move(kept);
}

bool UploadStaticWorldObjectMeshLocked() {
    RefreshWorldObjectTextureHandles(&g_static_world_object_mesh);
    if (g_static_world_object_mesh.vertices.empty()) {
        return true;
    }
    return RenderBackend().set_static_world_object_mesh(
            g_static_world_object_mesh);
}

float CameraDegreesToRadians(float degrees);

struct VisibleWorldBounds {
    float min_x = -1.0e9f;
    float min_y = -1.0e9f;
    float max_x = 1.0e9f;
    float max_y = 1.0e9f;
    bool valid = false;
};

// The camera basis mirrors ScreenToTerrainIntersectionLocked so the culled
// rectangle matches what the renderer actually draws.
VisibleWorldBounds ComputeVisibleWorldBoundsLocked() {
    VisibleWorldBounds bounds;
    const uint32_t viewport_width = RenderBackend().width();
    const uint32_t viewport_height = RenderBackend().content_height();
    if (viewport_width == 0 || viewport_height == 0) {
        return bounds;
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
    const float aspect =
            static_cast<float>(viewport_width) /
            static_cast<float>(viewport_height);
    const float tangent =
            std::tan(
                    CameraDegreesToRadians(
                            g_camera.horizontal_fov_degrees * 0.5f)) /
            aspect;
    // Objects taller than the ground plane show up before their footprint
    // does, and their projected shadow reaches further still.
    const float margin = 48.0f;
    const float far_limit = g_camera.distance * 6.0f;
    float min_x = 1.0e9f;
    float min_y = 1.0e9f;
    float max_x = -1.0e9f;
    float max_y = -1.0e9f;
    const float corners[4][2] = {
            {-1.0f, -1.0f}, {1.0f, -1.0f}, {-1.0f, 1.0f}, {1.0f, 1.0f}};
    for (const float(&corner)[2] : corners) {
        const Vec3 ray = normalize(Vec3{
                forward.x + right.x * corner[0] * tangent * aspect +
                        camera_up.x * corner[1] * tangent,
                forward.y + right.y * corner[0] * tangent * aspect +
                        camera_up.y * corner[1] * tangent,
                forward.z + right.z * corner[0] * tangent * aspect +
                        camera_up.z * corner[1] * tangent});
        float distance = far_limit;
        if (ray.z < -0.0001f) {
            const float ground = (g_camera.target_z - eye.z) / ray.z;
            if (ground > 0.0f) {
                distance = std::min(ground, far_limit);
            }
        }
        const float point_x = eye.x + ray.x * distance;
        const float point_y = eye.y + ray.y * distance;
        min_x = std::min(min_x, point_x);
        min_y = std::min(min_y, point_y);
        max_x = std::max(max_x, point_x);
        max_y = std::max(max_y, point_y);
    }
    bounds.min_x = min_x - margin;
    bounds.min_y = min_y - margin;
    bounds.max_x = max_x + margin;
    bounds.max_y = max_y + margin;
    bounds.valid = true;
    return bounds;
}

// The options screen writes through the same global the desktop build uses,
// so the renderer just reads it back.
bool ReadShadowsDisabledOption() {
    const std::string value(
            NStr::ToMBCS(NGlobal::GetVar("gfx_noshadows", "0")).c_str());
    return value == "1" || value == "true";
}

bool RefreshDynamicWorldMeshLocked(bool force) {
    g_shadows_disabled = ReadShadowsDisabledOption();
    const Bk2PresentationSnapshotInfo info =
            bk2_presentation_snapshot_info();
    const std::vector<AndroidCombatEffect> combat_effects =
            CopyActiveAndroidCombatEffects();
    const std::vector<AndroidSceneEffect> scene_effects =
            CopyActiveAndroidSceneEffects();
    const std::vector<AndroidDestructionEffect> destruction_effects =
            CopyActiveAndroidDestructionEffects();
    const AndroidWarFogSnapshot war_fog =
            CopyAndroidWarFogSnapshot();
    if (!force &&
        info.generation == g_rendered_presentation_generation &&
        war_fog.generation == g_rendered_war_fog_generation &&
        g_animated_geometry_part_count == 0 &&
        g_river_animated_layer_count == 0 &&
        combat_effects.empty() &&
        g_active_combat_effect_count == 0 &&
        scene_effects.empty() &&
        g_active_scene_effect_count == 0 &&
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

    WorldObjectMesh combined = g_animated_static_world_object_mesh;
    ApplyStaticLayerTextureAnimation(&combined);
    combined.vertices.reserve(
            combined.vertices.size() + entities.size() * 24);
    combined.triangle_indices.reserve(
            combined.triangle_indices.size() + entities.size() * 108);
    g_dynamic_rendered_object_count = 0;
    g_active_unit_indicator_count = 0;
    const VisibleWorldBounds visible_bounds =
            ComputeVisibleWorldBoundsLocked();
    g_culled_entity_count = 0;
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
        // Nothing outside the camera rectangle reaches the screen, and
        // rebuilding its geometry every frame is what used to eat the frame
        // budget on a full map.
        if (visible_bounds.valid &&
            (entity.x < visible_bounds.min_x ||
             entity.x > visible_bounds.max_x ||
             entity.y < visible_bounds.min_y ||
             entity.y > visible_bounds.max_y)) {
            ++g_culled_entity_count;
            continue;
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
    const size_t previous_active_scene_effect_count =
            g_active_scene_effect_count;
    for (const AndroidSceneEffect& effect : scene_effects) {
        AppendSceneEffect(&combined, effect);
    }
    g_active_scene_effect_count = scene_effects.size();
    if (g_active_scene_effect_count > 0 &&
        previous_active_scene_effect_count == 0) {
        PlatformRuntime::instance().log_info(
                std::string("descriptor_scene_effect_render=active; id=") +
                scene_effects.front().descriptor_id +
                "; emitters=" +
                std::to_string(
                        scene_effects.front().emitters.size()) +
                "; lights=" +
                std::to_string(
                        scene_effects.front().lights.size()));
    } else if (
            g_active_scene_effect_count == 0 &&
            previous_active_scene_effect_count > 0) {
        PlatformRuntime::instance().log_info(
                "descriptor_scene_effect_render=cleared");
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
                std::string("destruction_effect_render=active; lights=") +
                std::to_string(
                        destruction_effects.front().lights.size()));
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

float CameraDegreesToRadians(float degrees) {
    return degrees * kPi / 180.0f;
}

void ConfigureCameraFromLegacyConstsLocked() {
    g_camera_limits = CameraRuntimeLimits();
    g_camera.yaw_radians =
            CameraDegreesToRadians(kDefaultCameraYawDegrees);
    g_camera.pitch_radians =
            CameraDegreesToRadians(kDefaultCameraPitchDegrees);
    g_camera.distance = kDefaultCameraDistance;
    g_camera.horizontal_fov_degrees =
            kDefaultCameraHorizontalFovDegrees;

    const NDb::SClientGameConsts* client_consts =
            NGameX::GetClientConsts();
    const NDb::SCameraLimits* camera_consts =
            client_consts == nullptr
            ? nullptr
            : client_consts->pCamera.GetPtr();
    bool loaded = false;
    if (camera_consts != nullptr) {
        const NDb::SCameraLimits::SCLLimit& distance =
                camera_consts->distanceLimit;
        if (std::isfinite(distance.fMin) &&
            std::isfinite(distance.fMax) &&
            std::isfinite(distance.fAve) &&
            distance.fMin > 0.0f &&
            distance.fMax >= distance.fMin) {
            g_camera_limits.min_distance = distance.fMin;
            g_camera_limits.max_distance = distance.fMax;
            g_camera_limits.default_distance = std::clamp(
                    distance.fAve,
                    distance.fMin,
                    distance.fMax);
            g_camera.distance =
                    g_camera_limits.default_distance;
            loaded = true;
        }
        if (std::isfinite(camera_consts->pitchLimit.fAve)) {
            g_camera.pitch_radians = CameraDegreesToRadians(
                    camera_consts->pitchLimit.fAve);
        }
        if (std::isfinite(camera_consts->yawLimit.fAve)) {
            g_camera.yaw_radians = CameraDegreesToRadians(
                    camera_consts->yawLimit.fAve);
        }
        if (std::isfinite(camera_consts->fFOV) &&
            camera_consts->fFOV > 1.0f &&
            camera_consts->fFOV < 179.0f) {
            g_camera.horizontal_fov_degrees =
                    camera_consts->fFOV;
        }
    }

    std::ostringstream report;
    report << "camera_limits=" << (loaded ? "legacy" : "fallback")
           << "; distance=" << g_camera_limits.min_distance
           << "," << g_camera_limits.default_distance
           << "," << g_camera_limits.max_distance
           << "; pitch=" << g_camera.pitch_radians
           << "; yaw=" << g_camera.yaw_radians
           << "; horizontal_fov="
           << g_camera.horizontal_fov_degrees;
    PlatformRuntime::instance().log_info(report.str());
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
    g_camera.distance = g_camera_limits.default_distance;

    std::ostringstream report;
    report << "camera_focus=player"
           << "; player=" << player
           << "; units=" << count
           << "; target=" << g_camera.target_x << "," << g_camera.target_y
           << "; distance=" << g_camera.distance
           << "; pitch=" << g_camera.pitch_radians
           << "; yaw=" << g_camera.yaw_radians
           << "; horizontal_fov="
           << g_camera.horizontal_fov_degrees;
    PlatformRuntime::instance().log_info(report.str());
    return true;
}

bool ApplyMissionCameraLocked(
        const NDb::SMapInfo* map,
        int player) {
    if (map == nullptr ||
        player < 0 ||
        player >= static_cast<int>(map->players.size())) {
        return false;
    }

    const NDb::SCameraPlacement& placement =
            map->players[player].camera;
    if (!std::isfinite(placement.vAnchor.x) ||
        !std::isfinite(placement.vAnchor.y) ||
        !std::isfinite(placement.vAnchor.z)) {
        return false;
    }

    g_camera.target_x = placement.vAnchor.x;
    g_camera.target_y = placement.vAnchor.y;
    g_camera.target_z = placement.vAnchor.z;
    if (!placement.bUseAnchorOnly &&
        std::isfinite(placement.fDist) &&
        std::isfinite(placement.fPitch) &&
        std::isfinite(placement.fYaw) &&
        placement.fDist > 0.0f) {
        g_camera.distance = placement.fDist;
        g_camera.pitch_radians =
                CameraDegreesToRadians(placement.fPitch);
        g_camera.yaw_radians =
                CameraDegreesToRadians(placement.fYaw);
    }

    std::ostringstream report;
    report << "camera_focus=mission"
           << "; player=" << player
           << "; anchor=" << g_camera.target_x << ","
           << g_camera.target_y << ","
           << g_camera.target_z
           << "; anchor_only="
           << (placement.bUseAnchorOnly ? "true" : "false")
           << "; distance=" << g_camera.distance
           << "; pitch=" << g_camera.pitch_radians
           << "; yaw=" << g_camera.yaw_radians
           << "; horizontal_fov="
           << g_camera.horizontal_fov_degrees;
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
    basis->aspect =
            static_cast<float>(viewport_width) /
            static_cast<float>(viewport_height);
    basis->tangent =
            std::tan(
                    CameraDegreesToRadians(
                            g_camera.horizontal_fov_degrees * 0.5f)) /
            basis->aspect;
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

size_t CountOwnUnitsLocked(int player) {
    const Bk2PresentationSnapshotInfo snapshot =
            bk2_presentation_snapshot_info();
    std::vector<Bk2PresentationEntity> entities(snapshot.entity_count);
    if (!entities.empty() &&
        bk2_presentation_copy_entities(
                entities.data(),
                entities.size()) != entities.size()) {
        return 0;
    }
    size_t count = 0;
    for (const Bk2PresentationEntity& entity : entities) {
        if (entity.player == player &&
            (entity.flags & BK2_PRESENTATION_ENTITY_ALIVE) != 0) {
            ++count;
        }
    }
    return count;
}

float NearestOwnUnitScreenDistanceLocked(
        float screen_x,
        float screen_y,
        uint32_t viewport_width,
        uint32_t viewport_height,
        int player,
        float* nearest_screen_x = nullptr,
        float* nearest_screen_y = nullptr) {
    ScreenProjectionBasis basis;
    if (!BuildScreenProjectionBasisLocked(
                viewport_width,
                viewport_height,
                &basis)) {
        return -1.0f;
    }
    const Bk2PresentationSnapshotInfo snapshot =
            bk2_presentation_snapshot_info();
    std::vector<Bk2PresentationEntity> entities(snapshot.entity_count);
    if (!entities.empty() &&
        bk2_presentation_copy_entities(
                entities.data(),
                entities.size()) != entities.size()) {
        return -1.0f;
    }
    float best = -1.0f;
    for (const Bk2PresentationEntity& entity : entities) {
        if (entity.player != player ||
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
        const float distance = std::sqrt(
                delta_x * delta_x + delta_y * delta_y);
        if (best < 0.0f || distance < best) {
            best = distance;
            if (nearest_screen_x != nullptr) {
                *nearest_screen_x = projected_x;
            }
            if (nearest_screen_y != nullptr) {
                *nearest_screen_y = projected_y;
            }
        }
    }
    return best;
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

bool ScreenToTerrainIntersectionLocked(
        float screen_x,
        float screen_y,
        uint32_t viewport_width,
        uint32_t viewport_height,
        bool require_inside_map,
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
    const float aspect =
            static_cast<float>(viewport_width) /
            static_cast<float>(viewport_height);
    const float tangent =
            std::tan(
                    CameraDegreesToRadians(
                            g_camera.horizontal_fov_degrees * 0.5f)) /
            aspect;
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
    if (require_inside_map &&
        (intersection_x < 0.0f ||
         intersection_y < 0.0f ||
         intersection_x > max_x ||
         intersection_y > max_y)) {
        return false;
    }
    *world_x = intersection_x;
    *world_y = intersection_y;
    return true;
}

bool ScreenToTerrainLocked(
        float screen_x,
        float screen_y,
        uint32_t viewport_width,
        uint32_t viewport_height,
        float* world_x,
        float* world_y) {
    return ScreenToTerrainIntersectionLocked(
            screen_x,
            screen_y,
            viewport_width,
            viewport_height,
            true,
            world_x,
            world_y);
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
    if (texture_path.empty() ||
        texture_path == kProjectedShadowLayer) {
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
    size_t road_texture_layers = 0;
    size_t road_texture_ready = 0;
    size_t river_texture_layers = 0;
    size_t river_texture_ready = 0;
    size_t crag_texture_layers = 0;
    size_t crag_texture_ready = 0;
    for (WorldObjectMesh::Layer& layer : mesh->layers) {
        layer.texture_handle =
                ModelTextureHandle(layer.texture_path);
        if (layer.texture_path.find("Terrain/roads/") == 0) {
            ++road_texture_layers;
            if (layer.texture_handle != UINT16_MAX) {
                ++road_texture_ready;
            }
        }
        // River bank precipices also resolve into Terrain/Water, so the
        // precipice pass reports its exact texture set instead of a path
        // prefix and the river gate keeps counting only water layers.
        const bool crag_layer =
                g_crag_texture_paths.count(layer.texture_path) != 0;
        if (crag_layer) {
            ++crag_texture_layers;
            if (layer.texture_handle != UINT16_MAX) {
                ++crag_texture_ready;
            }
        } else if (layer.texture_path.find("Terrain/Water/") == 0) {
            ++river_texture_layers;
            if (layer.texture_handle != UINT16_MAX) {
                ++river_texture_ready;
            }
        }
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
        if ((layer.alpha_blended ||
             layer.additive_blended) &&
            layer.texture_path.find(
                    "Scene/TexAndMats/All/Effects/") == 0 &&
            !g_descriptor_particle_texture_logged) {
            PlatformRuntime::instance().log_info(
                    std::string("descriptor_particle_texture=") +
                    (layer.texture_handle == UINT16_MAX
                             ? "unavailable"
                             : "ready") +
                    "; blend=" +
                    (layer.additive_blended
                             ? "additive"
                             : "alpha") +
                    "; path=" + layer.texture_path);
            g_descriptor_particle_texture_logged = true;
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
    if (road_texture_layers > 0) {
        g_road_texture_gpu_count = road_texture_ready;
        if (!g_road_texture_logged &&
            road_texture_ready == road_texture_layers) {
            std::ostringstream report;
            report << "terrain_road_textures_gpu=ready"
                   << "; ready=" << road_texture_ready
                   << "; total=" << road_texture_layers;
            PlatformRuntime::instance().log_info(report.str());
            g_road_texture_logged = true;
        }
    }
    if (river_texture_layers > 0) {
        g_river_texture_gpu_count = river_texture_ready;
        if (!g_river_texture_logged &&
            river_texture_ready == river_texture_layers) {
            std::ostringstream report;
            report << "terrain_river_textures_gpu=ready"
                   << "; ready=" << river_texture_ready
                   << "; total=" << river_texture_layers;
            PlatformRuntime::instance().log_info(report.str());
            g_river_texture_logged = true;
        }
    }
    if (crag_texture_layers > 0) {
        g_crag_texture_gpu_count = crag_texture_ready;
        if (!g_crag_texture_logged &&
            crag_texture_ready == crag_texture_layers) {
            std::ostringstream report;
            report << "terrain_crag_textures_gpu=ready"
                   << "; ready=" << crag_texture_ready
                   << "; total=" << crag_texture_layers;
            PlatformRuntime::instance().log_info(report.str());
            g_crag_texture_logged = true;
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

bool LoadMinimapTexture(const NDb::SMapInfo* map) {
    g_minimap_texture = 0;
    g_minimap_texture_path.clear();
    g_minimap_base_pixels.clear();
    g_minimap_base_width = 0;
    g_minimap_base_height = 0;
    if (map == nullptr || !map->pMiniMap || !map->pMiniMap->pTexture) {
        return false;
    }

    const NDb::STexture* texture_desc = map->pMiniMap->pTexture;
    CObj<NGfx::CTexture> texture;
    if (!LoadTextureImmediately(texture_desc, &texture) ||
        !CopyLegacyTextureArgb(
                texture,
                &g_minimap_base_pixels,
                &g_minimap_base_width,
                &g_minimap_base_height)) {
        g_minimap_base_pixels.clear();
        g_minimap_base_width = 0;
        g_minimap_base_height = 0;
        return false;
    }

    g_minimap_texture = texture;
    g_minimap_texture_path = texture_desc->szDestName.c_str();
    PlatformRuntime::instance().log_info(
            "minimap_texture=ready; path=" +
            g_minimap_texture_path +
            "; size=" +
            std::to_string(g_minimap_base_width) + "x" +
            std::to_string(g_minimap_base_height));
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
    if (!g_water_mesh.vertices.empty()) {
        g_water_mesh.texture_handle =
                ModelTextureHandle(g_water_mesh.texture_path);
        if (!RenderBackend().set_water_mesh(g_water_mesh)) {
            return false;
        }
        if (!g_water_texture_logged &&
            g_water_mesh.texture_handle != UINT16_MAX) {
            PlatformRuntime::instance().log_info(
                    std::string("water_texture=ready; path=") +
                    g_water_mesh.texture_path);
            g_water_texture_logged = true;
        }
    } else {
        RenderBackend().clear_water_mesh();
    }
    if (!UploadStaticWorldObjectMeshLocked()) {
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

// The mission sounds are spatialised against the shipped min/max distances,
// so the listener has to sit where the camera actually is rather than at the
// world origin.
void UpdateAudioListenerLocked() {
    if (!AudioBackend().is_initialized()) {
        return;
    }
    const float horizontal_distance =
            g_camera.distance * std::cos(g_camera.pitch_radians);
    const float eye_x =
            g_camera.target_x +
            std::sin(g_camera.yaw_radians) * horizontal_distance;
    const float eye_y =
            g_camera.target_y -
            std::cos(g_camera.yaw_radians) * horizontal_distance;
    const float eye_z =
            g_camera.target_z +
            std::sin(g_camera.pitch_radians) * g_camera.distance;
    AudioListener listener;
    listener.position[0] = eye_x;
    listener.position[1] = eye_y;
    listener.position[2] = eye_z;
    float forward_x = g_camera.target_x - eye_x;
    float forward_y = g_camera.target_y - eye_y;
    float forward_z = g_camera.target_z - eye_z;
    const float length = std::sqrt(
            forward_x * forward_x +
            forward_y * forward_y +
            forward_z * forward_z);
    if (length > 0.0001f) {
        forward_x /= length;
        forward_y /= length;
        forward_z /= length;
    }
    listener.forward[0] = forward_x;
    listener.forward[1] = forward_y;
    listener.forward[2] = forward_z;
    listener.up[0] = 0.0f;
    listener.up[1] = 0.0f;
    listener.up[2] = 1.0f;
    AudioBackend().update_listener(listener);
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

    ResetMissionHudNotifications();
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
    CarveTerrainRivers(map, &mesh);
    BuildTerrainLayers(map, terrain_info, &mesh);
    WaterMesh water_mesh;
    BuildWaterMesh(map, terrain_info, &water_mesh);
    WorldObjectMesh presentation_world_mesh;
    // Scene object shadows are baked into the static mesh, so the option has
    // to be read before it is built, not just before the first frame.
    g_shadows_disabled = ReadShadowsDisabledOption();
    BuildPresentationStaticWorldMesh(map, terrain_info, &presentation_world_mesh);
    LoadMinimapTexture(map);
    if (LoadTerrainLayerTextures(map, &mesh) == 0) {
        if (IsValid(g_minimap_texture)) {
            g_terrain_texture = g_minimap_texture;
            g_terrain_texture_path = g_minimap_texture_path;
        } else {
            LoadTerrainTexture(map);
        }
    }

    g_camera.target_x = mesh.center_x;
    g_camera.target_y = mesh.center_y;
    g_camera.target_z = mesh.center_z;
    ConfigureCameraFromLegacyConstsLocked();

    g_terrain_mesh = std::move(mesh);
    g_water_mesh = std::move(water_mesh);
    g_static_world_object_mesh = std::move(presentation_world_mesh);
    g_world_object_mesh = WorldObjectMesh();
    PublishPresentationMeshes(
            mission.state.mission_id,
            g_terrain_mesh,
            g_static_world_object_mesh);
    SplitAnimatedStaticLayersLocked();
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
    if (!ApplyMissionCameraLocked(map, 0)) {
        FocusCameraOnPlayerLocked(0);
    }
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
    const auto tick_started = std::chrono::steady_clock::now();
    UpdateAudioListenerLocked();
    TickLegacyGameRuntime(elapsed_millis);
    const auto legacy_done = std::chrono::steady_clock::now();
    g_animation_elapsed_seconds +=
            static_cast<float>(elapsed_millis) * 0.001f;
    if (g_animation_elapsed_seconds > 3600.0f) {
        g_animation_elapsed_seconds =
                std::fmod(g_animation_elapsed_seconds, 3600.0f);
    }
    if (!RefreshDynamicWorldMeshLocked(false)) {
        g_last_error = "dynamic_world_render_refresh_failed";
    }
    const auto mesh_done = std::chrono::steady_clock::now();
    if (!UpdateWaterAnimationLocked(false)) {
        g_last_error = "water_render_refresh_failed";
    }
    const auto tick_done = std::chrono::steady_clock::now();
    ++g_tick_budget.ticks;
    g_tick_budget.legacy_micros += ElapsedMicros(tick_started, legacy_done);
    g_tick_budget.mesh_micros += ElapsedMicros(legacy_done, mesh_done);
    g_tick_budget.water_micros += ElapsedMicros(mesh_done, tick_done);
    ReportTickBudgetLocked(tick_done);
}

void ShutdownSinglePlayerRuntime() {
    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    ShutdownLegacyGameRuntime();
    ResetMissionHudNotifications();
    RenderBackend().clear_terrain_mesh();
    RenderBackend().clear_water_mesh();
    RenderBackend().clear_world_object_mesh();
    g_terrain_texture = 0;
    g_minimap_texture = 0;
    g_terrain_layer_textures.clear();
    g_terrain_layer_texture_paths.clear();
    g_terrain_mesh = TerrainMesh();
    g_water_mesh = WaterMesh();
    g_water_base_vertices.clear();
    g_static_world_object_mesh = WorldObjectMesh();
    g_animated_static_world_object_mesh = WorldObjectMesh();
    g_world_object_mesh = WorldObjectMesh();
    bk2::presentation::Reset();
    g_ready = false;
    g_user_paused = false;
    g_mission_id.clear();
    g_map_path.clear();
    g_terrain_texture_path.clear();
    g_minimap_texture_path.clear();
    g_minimap_base_pixels.clear();
    g_minimap_base_width = 0;
    g_minimap_base_height = 0;
    g_terrain_layer_count = 0;
    g_terrain_layer_texture_count = 0;
    g_terrain_type_map.clear();
    g_terrain_type_colors.clear();
    g_terrain_type_width = 0;
    g_terrain_type_height = 0;
    g_water_mask_width = 0;
    g_water_mask_height = 0;
    g_water_mask_node_count = 0;
    g_water_rendered_node_count = 0;
    g_water_triangle_count = 0;
    g_water_wave_amplitude = 1.4f;
    g_water_wave_period = 0.3f;
    g_water_texture_tile_count = 6.0f;
    g_water_last_update_seconds = -1.0f;
    g_water_waves_enabled = true;
    g_water_texture_logged = false;
    g_road_instance_count = 0;
    g_road_segment_count = 0;
    g_road_triangle_count = 0;
    g_road_texture_count = 0;
    g_road_texture_gpu_count = 0;
    g_road_texture_logged = false;
    g_river_instance_count = 0;
    g_river_point_count = 0;
    g_river_triangle_count = 0;
    g_river_texture_count = 0;
    g_river_texture_gpu_count = 0;
    g_river_carved_vertex_count = 0;
    g_river_disturbed_vertex_count = 0;
    g_river_animated_layer_count = 0;
    g_river_texture_logged = false;
    g_crag_precipice_count = 0;
    g_river_bank_precipice_count = 0;
    g_river_bank_precipice_logged = false;
    g_crag_node_count = 0;
    g_crag_triangle_count = 0;
    g_crag_foot_count = 0;
    g_crag_foot_segment_count = 0;
    g_crag_foot_triangle_count = 0;
    g_crag_texture_count = 0;
    g_crag_texture_gpu_count = 0;
    g_crag_texture_paths.clear();
    g_crag_texture_logged = false;
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
    g_effect_light_render_count = 0;
    g_active_combat_effect_count = 0;
    g_active_scene_effect_count = 0;
    g_active_destruction_effect_count = 0;
    g_active_unit_indicator_count = 0;
    g_combat_effect_trace_texture_logged = false;
    g_muzzle_flash_texture_logged = false;
    g_descriptor_particle_texture_logged = false;
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
    g_projected_shadow_hulls.clear();
    g_stats_geometry_index.clear();
    g_stats_geometry_variants.clear();
    g_stats_geometry_index_loaded = false;
    g_model_textures.clear();
    g_model_texture_count = 0;
    g_material_alpha_test_layer_count = 0;
    g_material_alpha_test_triangle_count = 0;
    g_material_alpha_blend_layer_count = 0;
    g_material_alpha_blend_triangle_count = 0;
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

void PanSinglePlayerCameraDrag(
        float from_x,
        float from_y,
        float to_x,
        float to_y,
        uint32_t viewport_width,
        uint32_t viewport_height) {
    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    if (!g_ready) {
        return;
    }
    // Drag the ground itself: intersect both pointer positions with the
    // terrain and shift the camera by the difference, so the spot under the
    // finger stays under the finger at any zoom or pitch.
    float from_world_x = 0.0f;
    float from_world_y = 0.0f;
    float to_world_x = 0.0f;
    float to_world_y = 0.0f;
    if (!ScreenToTerrainIntersectionLocked(
                from_x,
                from_y,
                viewport_width,
                viewport_height,
                false,
                &from_world_x,
                &from_world_y) ||
        !ScreenToTerrainIntersectionLocked(
                to_x,
                to_y,
                viewport_width,
                viewport_height,
                false,
                &to_world_x,
                &to_world_y)) {
        return;
    }
    g_camera.target_x -= to_world_x - from_world_x;
    g_camera.target_y -= to_world_y - from_world_y;
    const float maximum_x =
            static_cast<float>(std::max(g_height_width - 1, 0)) *
            VIS_TILE_SIZE;
    const float maximum_y =
            static_cast<float>(std::max(g_height_height - 1, 0)) *
            VIS_TILE_SIZE;
    g_camera.target_x = std::clamp(g_camera.target_x, 0.0f, maximum_x);
    g_camera.target_y = std::clamp(g_camera.target_y, 0.0f, maximum_y);
    ApplyCameraLocked();
}

void PanSinglePlayerCamera(float delta_x_pixels, float delta_y_pixels) {
    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    if (!g_ready) {
        return;
    }
    const float scale =
            std::max(g_camera.distance, g_camera_limits.min_distance) *
            0.0015f;
    const float right_x = std::cos(g_camera.yaw_radians);
    const float right_y = std::sin(g_camera.yaw_radians);
    const float forward_x = -right_y;
    const float forward_y = right_x;
    g_camera.target_x -= right_x * delta_x_pixels * scale;
    g_camera.target_y -= right_y * delta_x_pixels * scale;
    g_camera.target_x += forward_x * delta_y_pixels * scale;
    g_camera.target_y += forward_y * delta_y_pixels * scale;
    const float maximum_x =
            static_cast<float>(std::max(g_height_width - 1, 0)) *
            VIS_TILE_SIZE;
    const float maximum_y =
            static_cast<float>(std::max(g_height_height - 1, 0)) *
            VIS_TILE_SIZE;
    g_camera.target_x = std::clamp(
            g_camera.target_x,
            0.0f,
            maximum_x);
    g_camera.target_y = std::clamp(
            g_camera.target_y,
            0.0f,
            maximum_y);
    ApplyCameraLocked();
}

bool CenterSinglePlayerCameraFromMinimap(
        float normalized_x,
        float normalized_y) {
    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    if (!g_ready ||
        !std::isfinite(normalized_x) ||
        !std::isfinite(normalized_y) ||
        g_height_width < 2 ||
        g_height_height < 2) {
        return false;
    }
    normalized_x = std::clamp(normalized_x, 0.0f, 1.0f);
    normalized_y = std::clamp(normalized_y, 0.0f, 1.0f);
    const float maximum_x =
            static_cast<float>(g_height_width - 1) * VIS_TILE_SIZE;
    const float maximum_y =
            static_cast<float>(g_height_height - 1) * VIS_TILE_SIZE;
    g_camera.target_x = normalized_x * maximum_x;
    g_camera.target_y = (1.0f - normalized_y) * maximum_y;
    g_camera.target_z = TerrainMeshHeightAtLocked(
            g_camera.target_x,
            g_camera.target_y);
    ApplyCameraLocked();

    std::ostringstream report;
    report << "camera_focus=minimap"
           << "; normalized=" << normalized_x << "," << normalized_y
           << "; target=" << g_camera.target_x << ","
           << g_camera.target_y << "," << g_camera.target_z;
    PlatformRuntime::instance().log_info(report.str());
    return true;
}

void ZoomSinglePlayerCamera(float scale) {
    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    if (!g_ready || scale <= 0.0f) {
        return;
    }
    g_camera.distance = std::clamp(
            g_camera.distance /
                    std::max(0.25f, std::min(scale, 4.0f)),
            g_camera_limits.min_distance,
            g_camera_limits.max_distance);
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
    // A fingertip covers far more of a phone screen than a mouse cursor does,
    // so the pick radius scales with the viewport instead of staying at the
    // desktop-sized constant.
    const float kEntityTapRadiusPixels = std::max(
            52.0f,
            static_cast<float>(viewport_width) * 0.03f);
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
            std::tan(
                    CameraDegreesToRadians(
                            g_camera.horizontal_fov_degrees * 0.5f)) /
            static_cast<float>(std::max(viewport_width, 1u));
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
        // A tap that neither selects nor orders means the player has no unit
        // under the finger and none selected. Report what was actually within
        // reach so touch tuning has something to work from.
        float nearest_screen_x = -1.0f;
        float nearest_screen_y = -1.0f;
        const float nearest_pixels = NearestOwnUnitScreenDistanceLocked(
                screen_x,
                screen_y,
                viewport_width,
                viewport_height,
                0,
                &nearest_screen_x,
                &nearest_screen_y);
        std::ostringstream report;
        report << "player_tap=no_action"
               << "; selected=" << SelectedLegacyUnitId()
               << "; own_units=" << CountOwnUnitsLocked(0)
               << "; nearest_own_pixels=" << nearest_pixels
               << "; nearest_own_screen=" << nearest_screen_x
               << "," << nearest_screen_y;
        PlatformRuntime::instance().log_info(report.str());
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
    size_t alpha_masked_shadow_layers = 0;
    size_t alpha_masked_shadow_triangles = 0;
    for (const WorldObjectMesh::Layer& layer :
         g_static_world_object_mesh.layers) {
        if (layer.alpha_masked_shadow) {
            ++alpha_masked_shadow_layers;
            alpha_masked_shadow_triangles +=
                    layer.triangle_indices.size() / 3;
        }
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
           << "; camera_target="
           << g_camera.target_x << "," << g_camera.target_y
           << "," << g_camera.target_z
           << "; camera_distance=" << g_camera.distance
           << "; camera_distance_limits="
           << g_camera_limits.min_distance << ","
           << g_camera_limits.max_distance
           << "; camera_pitch=" << g_camera.pitch_radians
           << "; camera_yaw=" << g_camera.yaw_radians
           << "; camera_horizontal_fov="
           << g_camera.horizontal_fov_degrees
           << "; heightfield=" << g_height_width << "x" << g_height_height
           << "; triangles=" << g_triangle_count
           << "; terrain_texture="
           << (g_terrain_texture_path.empty()
                       ? "<none>"
                       : g_terrain_texture_path)
           << "; minimap_texture="
           << (g_minimap_texture_path.empty()
                       ? "<none>"
                       : g_minimap_texture_path)
           << "; minimap_texture_size="
           << g_minimap_base_width << "x" << g_minimap_base_height
           << "; terrain_layers=" << g_terrain_layer_count
           << "; terrain_layer_textures=" << g_terrain_layer_texture_count
           << "; terrain_layer_triangles=" << terrain_layer_triangles
           << "; terrain_unassigned_triangles="
           << (g_triangle_count > terrain_layer_triangles
                       ? g_triangle_count - terrain_layer_triangles
                       : 0)
           << "; water_mask="
           << g_water_mask_width << "x" << g_water_mask_height
           << "; water_mask_nodes=" << g_water_mask_node_count
           << "; water_rendered_nodes="
           << g_water_rendered_node_count
           << "; water_triangles=" << g_water_triangle_count
           << "; water_texture="
           << (g_water_mesh.texture_path.empty()
                       ? "<none>"
                       : g_water_mesh.texture_path)
           << "; water_texture_gpu="
           << (g_water_mesh.texture_handle == UINT16_MAX
                       ? "not_ready"
                       : "ready")
           << "; water_waves="
           << (g_water_waves_enabled ? "descriptor" : "disabled")
           << "; water_wave_amplitude="
           << g_water_wave_amplitude
           << "; water_wave_period="
           << g_water_wave_period
           << "; terrain_river_instances="
           << g_river_instance_count
           << "; terrain_river_points="
           << g_river_point_count
           << "; terrain_river_triangles="
           << g_river_triangle_count
           << "; terrain_river_textures="
           << g_river_texture_count
           << "; terrain_river_textures_gpu="
           << g_river_texture_gpu_count
           << "; terrain_river_carved_vertices="
           << g_river_carved_vertex_count
           << "; terrain_river_disturbed_vertices="
           << g_river_disturbed_vertex_count
           << "; terrain_river_animated_layers="
           << g_river_animated_layer_count
           << "; terrain_road_instances="
           << g_road_instance_count
           << "; terrain_road_segments="
           << g_road_segment_count
           << "; terrain_road_triangles="
           << g_road_triangle_count
           << "; terrain_road_textures="
           << g_road_texture_count
           << "; terrain_road_textures_gpu="
           << g_road_texture_gpu_count
           << "; terrain_crag_precipices="
           << g_crag_precipice_count
           << "; terrain_river_bank_precipices="
           << g_river_bank_precipice_count
           << "; terrain_crag_nodes="
           << g_crag_node_count
           << "; terrain_crag_triangles="
           << g_crag_triangle_count
           << "; terrain_crag_foots="
           << g_crag_foot_count
           << "; terrain_crag_foot_segments="
           << g_crag_foot_segment_count
           << "; terrain_crag_foot_triangles="
           << g_crag_foot_triangle_count
           << "; terrain_crag_textures="
           << g_crag_texture_count
           << "; terrain_crag_textures_gpu="
           << g_crag_texture_gpu_count
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
           << "; alpha_masked_shadow_layers="
           << alpha_masked_shadow_layers
           << "; alpha_masked_shadow_triangles="
           << alpha_masked_shadow_triangles
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
           << "; material_alpha_test_layers="
           << g_material_alpha_test_layer_count
           << "; material_alpha_test_triangles="
           << g_material_alpha_test_triangle_count
           << "; material_alpha_blend_layers="
           << g_material_alpha_blend_layer_count
           << "; material_alpha_blend_triangles="
           << g_material_alpha_blend_triangle_count
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
           << "; effect_light_renders="
           << g_effect_light_render_count
           << "; active_combat_effects_rendered="
           << g_active_combat_effect_count
           << "; active_scene_effects_rendered="
           << g_active_scene_effect_count
           << "; active_destruction_effects_rendered="
           << g_active_destruction_effect_count
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
    const bool has_original_minimap =
            g_minimap_base_width > 0 &&
            g_minimap_base_height > 0 &&
            g_minimap_base_pixels.size() ==
                    static_cast<size_t>(
                            g_minimap_base_width *
                            g_minimap_base_height);
    const bool has_terrain_fallback =
            g_terrain_type_width > 0 &&
            g_terrain_type_height > 0 &&
            !g_terrain_type_map.empty();
    if (!g_ready ||
        width <= 0 ||
        height <= 0 ||
        width > 1024 ||
        height > 1024 ||
        (!has_original_minimap && !has_terrain_fallback)) {
        return {};
    }
    std::vector<int32_t> pixels(
            static_cast<size_t>(width) * height,
            static_cast<int32_t>(0xff1d3027u));
    const AndroidWarFogSnapshot war_fog =
            CopyAndroidWarFogSnapshot();
    const bool has_war_fog =
            war_fog.width >= 2 &&
            war_fog.height >= 2 &&
            war_fog.visibility.size() ==
                    static_cast<size_t>(
                            war_fog.width * war_fog.height);
    for (int y = 0; y < height; ++y) {
        const int source_y = has_terrain_fallback
                ? std::min(
                        g_terrain_type_height - 1,
                        (height - 1 - y) *
                                g_terrain_type_height /
                                height)
                : 0;
        const int minimap_y = has_original_minimap
                ? std::min(
                        g_minimap_base_height - 1,
                        y * g_minimap_base_height / height)
                : 0;
        for (int x = 0; x < width; ++x) {
            const int source_x = has_terrain_fallback
                    ? std::min(
                            g_terrain_type_width - 1,
                            x * g_terrain_type_width / width)
                    : 0;
            uint32_t color = 0xff1d3027u;
            bool has_color = false;
            if (has_original_minimap) {
                const int minimap_x = std::min(
                        g_minimap_base_width - 1,
                        x * g_minimap_base_width / width);
                color = 0xff000000u |
                        (g_minimap_base_pixels[
                                 static_cast<size_t>(
                                         minimap_y *
                                                 g_minimap_base_width +
                                         minimap_x)] &
                         0x00ffffffu);
                has_color = true;
            } else {
                const int type_index = g_terrain_type_map[
                        static_cast<size_t>(
                                source_y *
                                        g_terrain_type_width +
                                source_x)];
                if (type_index >= 0 &&
                    type_index <
                            static_cast<int>(
                                    g_terrain_type_colors.size())) {
                    color = 0xff000000u |
                            (g_terrain_type_colors[type_index] &
                             0x00ffffffu);
                    has_color = true;
                }
            }
            if (has_color) {
                if (has_war_fog) {
                    const int fog_x = std::clamp(
                            x * war_fog.width / width,
                            0,
                            war_fog.width - 1);
                    const int fog_y = std::clamp(
                            (height - 1 - y) *
                                    war_fog.height /
                                    height,
                            0,
                            war_fog.height - 1);
                    const float visibility = std::clamp(
                            static_cast<float>(
                                    war_fog.visibility[
                                            static_cast<size_t>(
                                                    fog_y *
                                                            war_fog.width +
                                                    fog_x)]) /
                                    static_cast<float>(
                                            std::max<int>(
                                                    war_fog.visibility_power,
                                                    1)),
                            0.0f,
                            1.0f);
                    const float brightness =
                            0.5f + visibility * 0.5f;
                    const uint32_t red = static_cast<uint32_t>(
                            std::lround(
                                    static_cast<float>(
                                            (color >> 16) & 0xffu) *
                                    brightness));
                    const uint32_t green = static_cast<uint32_t>(
                            std::lround(
                                    static_cast<float>(
                                            (color >> 8) & 0xffu) *
                                    brightness));
                    const uint32_t blue = static_cast<uint32_t>(
                            std::lround(
                                    static_cast<float>(
                                            color & 0xffu) *
                                    brightness));
                    color = 0xff000000u |
                            (red << 16) |
                            (green << 8) |
                            blue;
                }
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
            const bool selected =
                    (entity.flags &
                     BK2_PRESENTATION_ENTITY_SELECTED) != 0;
            const int radius =
                    (entity.flags & BK2_PRESENTATION_ENTITY_MECHANIZED) != 0
                    ? 2
                    : selected ? 2 : 1;
            const uint32_t color = selected
                    ? 0xffffdf58u
                    : entity.player == 0
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

    const uint32_t viewport_width = RenderBackend().width();
    const uint32_t viewport_height = RenderBackend().content_height();
    const float maximum_world_x =
            static_cast<float>(g_height_width - 1) * VIS_TILE_SIZE;
    const float maximum_world_y =
            static_cast<float>(g_height_height - 1) * VIS_TILE_SIZE;
    if (viewport_width > 0 &&
        viewport_height > 0 &&
        maximum_world_x > 0.0f &&
        maximum_world_y > 0.0f) {
        const float screen_x[4] = {
                0.0f,
                static_cast<float>(viewport_width),
                static_cast<float>(viewport_width),
                0.0f};
        const float screen_y[4] = {
                0.0f,
                0.0f,
                static_cast<float>(viewport_height),
                static_cast<float>(viewport_height)};
        int frame_x[4] = {};
        int frame_y[4] = {};
        bool has_viewport_frame = true;
        for (int index = 0; index < 4; ++index) {
            float world_x = 0.0f;
            float world_y = 0.0f;
            if (!ScreenToTerrainIntersectionLocked(
                        screen_x[index],
                        screen_y[index],
                        viewport_width,
                        viewport_height,
                        false,
                        &world_x,
                        &world_y)) {
                has_viewport_frame = false;
                break;
            }
            frame_x[index] = static_cast<int>(std::lround(
                    world_x / maximum_world_x *
                    static_cast<float>(width - 1)));
            frame_y[index] = static_cast<int>(std::lround(
                    (1.0f - world_y / maximum_world_y) *
                    static_cast<float>(height - 1)));
        }
        if (has_viewport_frame) {
            constexpr uint32_t kViewportFrameArgb = 0xffff80ffu;
            const auto draw_line =
                    [&pixels, width, height](
                            int x0,
                            int y0,
                            int x1,
                            int y1) {
                        const auto out_code =
                                [width, height](int x, int y) {
                                    int code = 0;
                                    if (x < 0) {
                                        code |= 1;
                                    } else if (x >= width) {
                                        code |= 2;
                                    }
                                    if (y < 0) {
                                        code |= 4;
                                    } else if (y >= height) {
                                        code |= 8;
                                    }
                                    return code;
                                };
                        bool visible = false;
                        for (int iteration = 0;
                             iteration < 8;
                             ++iteration) {
                            const int code0 = out_code(x0, y0);
                            const int code1 = out_code(x1, y1);
                            if ((code0 | code1) == 0) {
                                visible = true;
                                break;
                            }
                            if ((code0 & code1) != 0) {
                                break;
                            }
                            const int outside =
                                    code0 != 0 ? code0 : code1;
                            double clipped_x = 0.0;
                            double clipped_y = 0.0;
                            if ((outside & 8) != 0) {
                                clipped_y =
                                        static_cast<double>(height - 1);
                                clipped_x = x0 +
                                        static_cast<double>(x1 - x0) *
                                        (clipped_y - y0) /
                                        static_cast<double>(y1 - y0);
                            } else if ((outside & 4) != 0) {
                                clipped_y = 0.0;
                                clipped_x = x0 +
                                        static_cast<double>(x1 - x0) *
                                        (clipped_y - y0) /
                                        static_cast<double>(y1 - y0);
                            } else if ((outside & 2) != 0) {
                                clipped_x =
                                        static_cast<double>(width - 1);
                                clipped_y = y0 +
                                        static_cast<double>(y1 - y0) *
                                        (clipped_x - x0) /
                                        static_cast<double>(x1 - x0);
                            } else {
                                clipped_x = 0.0;
                                clipped_y = y0 +
                                        static_cast<double>(y1 - y0) *
                                        (clipped_x - x0) /
                                        static_cast<double>(x1 - x0);
                            }
                            if (outside == code0) {
                                x0 = static_cast<int>(
                                        std::lround(clipped_x));
                                y0 = static_cast<int>(
                                        std::lround(clipped_y));
                            } else {
                                x1 = static_cast<int>(
                                        std::lround(clipped_x));
                                y1 = static_cast<int>(
                                        std::lround(clipped_y));
                            }
                        }
                        if (!visible) {
                            return;
                        }
                        const int delta_x = std::abs(x1 - x0);
                        const int step_x = x0 < x1 ? 1 : -1;
                        const int delta_y = -std::abs(y1 - y0);
                        const int step_y = y0 < y1 ? 1 : -1;
                        int error = delta_x + delta_y;
                        for (;;) {
                            if (x0 >= 0 && x0 < width &&
                                y0 >= 0 && y0 < height) {
                                pixels[static_cast<size_t>(
                                        y0 * width + x0)] =
                                        static_cast<int32_t>(
                                                kViewportFrameArgb);
                            }
                            if (x0 == x1 && y0 == y1) {
                                break;
                            }
                            const int doubled_error = error * 2;
                            if (doubled_error >= delta_y) {
                                error += delta_y;
                                x0 += step_x;
                            }
                            if (doubled_error <= delta_x) {
                                error += delta_x;
                                y0 += step_y;
                            }
                        }
                    };
            for (int index = 0; index < 4; ++index) {
                const int next = (index + 1) % 4;
                draw_line(
                        frame_x[index],
                        frame_y[index],
                        frame_x[next],
                        frame_y[next]);
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
Java_com_nival_blitzkrieg2_NativeBridge_setMissionHudHeightPixels(
        JNIEnv*,
        jclass,
        jint height) {
    bk2::android::RenderBackend().set_bottom_inset(
            static_cast<uint32_t>(std::max(height, 0)));
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

extern "C" JNIEXPORT jboolean JNICALL
Java_com_nival_blitzkrieg2_NativeBridge_centerMissionCameraFromMinimap(
        JNIEnv*,
        jclass,
        jfloat normalized_x,
        jfloat normalized_y) {
    return bk2::android::CenterSinglePlayerCameraFromMinimap(
                   normalized_x,
                   normalized_y)
            ? JNI_TRUE
            : JNI_FALSE;
}
