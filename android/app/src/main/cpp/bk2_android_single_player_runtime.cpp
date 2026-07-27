#include "bk2_android_single_player_runtime.h"

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
#include "Stats_B2_M1/Vis2AI.h"
#include "System/BinSaver.h"
#include "System/VFSOperations.h"
#include "libdb/Db.h"

#include <algorithm>
#include <cmath>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <mutex>
#include <sstream>
#include <vector>

namespace bk2::android {
namespace {

constexpr float kPackedHeightScale = 0.01f;
constexpr float kMinCameraDistance = 24.0f;

std::mutex g_runtime_mutex;
bool g_ready = false;
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
TerrainCamera g_camera;
TerrainMesh g_terrain_mesh;
WorldObjectMesh g_static_world_object_mesh;
WorldObjectMesh g_world_object_mesh;
CObj<NGfx::CTexture> g_terrain_texture;
std::string g_terrain_texture_path;

struct MissionLaunchOverride {
    bool present = false;
    bool tutorial = false;
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
    if (!launch.mission_id.empty()) {
        return StartDirectMissionState(launch.mission_id, launch.difficulty);
    }
    if (launch.tutorial) {
        return StartTutorialMissionState(launch.tutorial_index, launch.difficulty);
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
                                    static_cast<float>(width - 1),
                            1.0f -
                                    static_cast<float>(y) /
                                            static_cast<float>(height - 1),
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

void AppendEntityModel(
        WorldObjectMesh* mesh,
        const Bk2PresentationEntity& entity,
        uint32_t abgr,
        bool selected) {
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
        AppendObjectMarker(
                mesh,
                x,
                y,
                z,
                scenario_objects ? 1.25f : 0.85f,
                scenario_objects ? 5.0f : 3.5f,
                ObjectColor(object.nPlayer, scenario_objects));
        if (count_rendered) {
            ++g_rendered_object_count;
        }
    }
}

void BuildWorldObjectMesh(
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
            true,
            true,
            true,
            mesh);
    AppendMapObjects(
            map->scenarioObjects,
            terrain_info,
            true,
            true,
            true,
            true,
            mesh);
}

void BuildPresentationStaticWorldMesh(
        const NDb::SMapInfo* map,
        const STerrainInfo& terrain_info,
        WorldObjectMesh* mesh) {
    if (map == nullptr || mesh == nullptr) {
        return;
    }
    const size_t total = map->objects.size() + map->scenarioObjects.size();
    mesh->vertices.reserve(total * 5);
    mesh->triangle_indices.reserve(total * 18);
    AppendMapObjects(
            map->objects,
            terrain_info,
            false,
            false,
            false,
            false,
            mesh);
    AppendMapObjects(
            map->scenarioObjects,
            terrain_info,
            true,
            false,
            false,
            false,
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
    if (!force &&
        info.generation == g_rendered_presentation_generation) {
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
    for (const Bk2PresentationEntity& entity : entities) {
        if ((entity.flags & BK2_PRESENTATION_ENTITY_ALIVE) == 0) {
            continue;
        }
        const bool selected =
                (entity.flags & BK2_PRESENTATION_ENTITY_SELECTED) != 0;
        AppendEntityModel(
                &combined,
                entity,
                selected ? ArgbToAbgr(0xffffe066u)
                         : ObjectColor(entity.player, false),
                selected);
        ++g_dynamic_rendered_object_count;
    }
    g_world_object_mesh = std::move(combined);
    g_rendered_presentation_generation = info.generation;
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
    const float formation_span = std::max(max_x - min_x, max_y - min_y);
    g_camera.distance = std::clamp(
            formation_span * 2.8f + 55.0f,
            110.0f,
            std::max(g_terrain_mesh.world_size * 0.6f, 110.0f));

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
    const Vec3 right = normalize(cross(forward, world_up));
    const Vec3 camera_up = normalize(cross(right, forward));
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

bool LoadTerrainTexture(const NDb::SMapInfo* map) {
    if (map == nullptr || !map->pMiniMap || !map->pMiniMap->pTexture) {
        return false;
    }

    const NDb::STexture* texture_desc = map->pMiniMap->pTexture;
    CObj<NGScene::CFileTexture> texture_node =
            new NGScene::CFileTexture();
    GUID uid;
    Zero(uid);
    texture_node->SetKey(NGScene::GetKey(texture_desc), uid);
    CDGPtr<NGScene::CFileTexture> texture_ref(texture_node.GetPtr());
    texture_ref.Refresh();
    g_terrain_texture = texture_ref->GetValue();
    if (!IsValid(g_terrain_texture)) {
        return false;
    }

    g_terrain_texture_path = texture_desc->szDestName.c_str();
    return true;
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
    if (!RenderBackend().set_terrain_mesh(g_terrain_mesh)) {
        return false;
    }
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
    WorldObjectMesh world_object_mesh;
    BuildWorldObjectMesh(map, terrain_info, &world_object_mesh);
    WorldObjectMesh presentation_world_mesh;
    BuildPresentationStaticWorldMesh(map, terrain_info, &presentation_world_mesh);
    LoadTerrainTexture(map);

    g_camera.target_x = mesh.center_x;
    g_camera.target_y = mesh.center_y;
    g_camera.target_z = mesh.center_z;
    g_camera.yaw_radians = 0.72f;
    g_camera.pitch_radians = 0.82f;
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
    if (!g_ready) {
        return;
    }
    TickLegacyGameRuntime(elapsed_millis);
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
    g_terrain_mesh = TerrainMesh();
    g_static_world_object_mesh = WorldObjectMesh();
    g_world_object_mesh = WorldObjectMesh();
    bk2::presentation::Reset();
    g_ready = false;
    g_mission_id.clear();
    g_map_path.clear();
    g_terrain_texture_path.clear();
    g_height_width = 0;
    g_height_height = 0;
    g_triangle_count = 0;
    g_map_object_count = 0;
    g_scenario_object_count = 0;
    g_rendered_object_count = 0;
    g_dynamic_rendered_object_count = 0;
    g_presentation_snapshot_written = false;
    g_rendered_presentation_generation = 0;
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
    g_camera.distance = std::max(
            kMinCameraDistance,
            g_camera.distance / std::max(0.25f, std::min(scale, 4.0f)));
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

bool HandleSinglePlayerTap(
        float screen_x,
        float screen_y,
        uint32_t viewport_width,
        uint32_t viewport_height) {
    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    if (!g_ready) {
        return false;
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

bool IsSinglePlayerRuntimeReady() {
    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    return g_ready;
}

std::string SinglePlayerRuntimeReport() {
    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    std::ostringstream report;
    report << "single_player_runtime=" << (g_ready ? "ready" : "not_ready")
           << "; mission=" << (g_mission_id.empty() ? "<none>" : g_mission_id)
           << "; map=" << (g_map_path.empty() ? "<none>" : g_map_path)
           << "; heightfield=" << g_height_width << "x" << g_height_height
           << "; triangles=" << g_triangle_count
           << "; terrain_texture="
           << (g_terrain_texture_path.empty()
                       ? "<none>"
                       : g_terrain_texture_path)
           << "; texture_gpu="
           << (g_terrain_mesh.texture_handle == UINT16_MAX
                       ? "not_ready"
                       : "ready")
           << "; map_objects=" << g_map_object_count
           << "; scenario_objects=" << g_scenario_object_count
           << "; rendered_objects=" << g_rendered_object_count
           << "; dynamic_rendered_objects="
           << g_dynamic_rendered_object_count
           << "; presentation_snapshot="
           << (g_presentation_snapshot_written ? "written" : "not_written")
           << "; " << LegacyGameRuntimeReport();
    if (!g_last_error.empty()) {
        report << "; error=" << g_last_error;
    }
    return report.str();
}

}  // namespace bk2::android
