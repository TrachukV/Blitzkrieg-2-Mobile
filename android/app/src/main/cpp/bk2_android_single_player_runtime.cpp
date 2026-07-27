#include "bk2_android_single_player_runtime.h"

#include "bk2_android_database.h"
#include "bk2_android_legacy_game_runtime.h"
#include "bk2_android_mission_runtime.h"
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
bool g_presentation_snapshot_written = false;
TerrainCamera g_camera;
TerrainMesh g_terrain_mesh;
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
            0xffd5d1c7u,
            0xffdf5b4fu,
            0xff4aa3dfu,
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

void AppendMapObjects(
        const vector<NDb::SMapObjectInfo>& objects,
        const STerrainInfo& terrain_info,
        bool scenario_objects,
        bool include_dynamic_units,
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
                type_id == NDb::SFenceRPGStats::typeID ||
                type_id == NDb::SMineRPGStats::typeID;
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
    AppendMapObjects(map->objects, terrain_info, false, true, true, mesh);
    AppendMapObjects(map->scenarioObjects, terrain_info, true, true, true, mesh);
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
    AppendMapObjects(map->objects, terrain_info, false, false, false, mesh);
    AppendMapObjects(map->scenarioObjects, terrain_info, true, false, false, mesh);
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
    g_world_object_mesh = std::move(world_object_mesh);
    PublishPresentationMeshes(
            mission.state.mission_id,
            g_terrain_mesh,
            presentation_world_mesh);
    if (!InitializeLegacyGameRuntime(
                map,
                terrain_info,
                mission.state.campaign_index,
                mission.state.chapter_index,
                mission.state.difficulty,
                &g_last_error)) {
        return false;
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
}

void ShutdownSinglePlayerRuntime() {
    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    ShutdownLegacyGameRuntime();
    RenderBackend().clear_terrain_mesh();
    RenderBackend().clear_world_object_mesh();
    g_terrain_texture = 0;
    g_terrain_mesh = TerrainMesh();
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
    g_presentation_snapshot_written = false;
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
           << "; presentation_snapshot="
           << (g_presentation_snapshot_written ? "written" : "not_written")
           << "; " << LegacyGameRuntimeReport();
    if (!g_last_error.empty()) {
        report << "; error=" << g_last_error;
    }
    return report.str();
}

}  // namespace bk2::android
