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
#include "Stats_B2_M1/DBVisObj.h"
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

const ConvertedGeometry* LoadConvertedGeometry(int record_id) {
    const auto loaded = g_converted_geometries.find(record_id);
    if (loaded != g_converted_geometries.end()) {
        return &loaded->second;
    }
    if (record_id < 0 ||
        g_missing_converted_geometries.find(record_id) !=
                g_missing_converted_geometries.end()) {
        return nullptr;
    }

    const std::string path = JoinHostPath(
            GetPortPaths().data_root(),
            "Converted/Geometries/" + std::to_string(record_id) + ".bk2mesh");
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
        g_missing_converted_geometries.insert(record_id);
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
            g_missing_converted_geometries.insert(record_id);
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
                g_missing_converted_geometries.insert(record_id);
                return nullptr;
            }
        }
        part.vertices = part.animation_frames.front();
        part.triangle_indices.resize(index_count);
        if (!ReadExact(
                    &input,
                    part.triangle_indices.data(),
                    static_cast<size_t>(index_count) * sizeof(uint32_t))) {
            g_missing_converted_geometries.insert(record_id);
            return nullptr;
        }
        for (uint32_t index : part.triangle_indices) {
            if (index >= vertex_count) {
                g_missing_converted_geometries.insert(record_id);
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
                    g_missing_converted_geometries.insert(record_id);
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

    auto inserted = g_converted_geometries.emplace(
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
            LoadConvertedGeometry(binding.geometry_record_id);
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
            const float animation_time = std::fmod(
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

void AppendEntityModel(
        WorldObjectMesh* mesh,
        const Bk2PresentationEntity& entity,
        uint32_t abgr,
        bool selected) {
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
                g_animation_elapsed_seconds,
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
            true,
            false,
            mesh);
    AppendMapObjects(
            map->scenarioObjects,
            terrain_info,
            true,
            false,
            true,
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
        info.generation == g_rendered_presentation_generation &&
        g_animated_geometry_part_count == 0) {
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
        const bool targeted =
                (entity.flags & BK2_PRESENTATION_ENTITY_TARGETED) != 0;
        AppendEntityModel(
                &combined,
                entity,
                selected ? ArgbToAbgr(0xffffe066u)
                         : targeted ? ArgbToAbgr(0xffff8a3du)
                         : ObjectColor(entity.player, false),
                selected || targeted);
        ++g_dynamic_rendered_object_count;
    }
    g_world_object_mesh = std::move(combined);
    RefreshWorldObjectTextureHandles(&g_world_object_mesh);
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

int FindEntityNearScreenLocked(
        float screen_x,
        float screen_y,
        uint32_t viewport_width,
        uint32_t viewport_height,
        int player,
        bool invert_player_match,
        float radius_pixels) {
    if (viewport_width == 0 ||
        viewport_height == 0 ||
        radius_pixels <= 0.0f) {
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
    const auto dot = [](const Vec3& left, const Vec3& right) {
        return left.x * right.x + left.y * right.y + left.z * right.z;
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
    const Vec3 right = normalize(cross(forward, Vec3{0.0f, 0.0f, 1.0f}));
    const Vec3 camera_up = normalize(cross(right, forward));
    const float tangent =
            std::tan(48.0f * 0.5f * 3.14159265358979323846f / 180.0f);
    const float aspect =
            static_cast<float>(viewport_width) /
            static_cast<float>(viewport_height);
    const float max_distance_squared = radius_pixels * radius_pixels;
    float best_distance_squared = std::numeric_limits<float>::max();
    int best_id = -1;
    for (const Bk2PresentationEntity& entity : entities) {
        const bool player_matches = entity.player == player;
        if ((invert_player_match ? player_matches : !player_matches) ||
            (entity.flags & BK2_PRESENTATION_ENTITY_ALIVE) == 0) {
            continue;
        }
        const Vec3 relative = {
                entity.x - eye.x,
                entity.y - eye.y,
                entity.z + 1.0f - eye.z};
        const float depth = dot(relative, forward);
        if (depth <= 0.001f) {
            continue;
        }
        const float ndc_x =
                dot(relative, right) / (depth * tangent * aspect);
        const float ndc_y =
                dot(relative, camera_up) / (depth * tangent);
        const float projected_x =
                (ndc_x + 1.0f) * 0.5f * viewport_width;
        const float projected_y =
                (1.0f - ndc_y) * 0.5f * viewport_height;
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
    EnsureLegacyTextureUploaded(cached->second, 0);
    return LegacyTextureHandleIndex(cached->second);
}

void RefreshWorldObjectTextureHandles(WorldObjectMesh* mesh) {
    if (mesh == nullptr) {
        return;
    }
    for (WorldObjectMesh::Layer& layer : mesh->layers) {
        layer.texture_handle =
                ModelTextureHandle(layer.texture_path);
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
    WorldObjectMesh world_object_mesh;
    BuildWorldObjectMesh(map, terrain_info, &world_object_mesh);
    WorldObjectMesh presentation_world_mesh;
    BuildPresentationStaticWorldMesh(map, terrain_info, &presentation_world_mesh);
    if (LoadTerrainLayerTextures(map, &mesh) == 0) {
        LoadTerrainTexture(map);
    }

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
    g_converted_geometries.clear();
    g_missing_converted_geometries.clear();
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
    constexpr float kEntityTapRadiusPixels = 52.0f;
    const int friendly_unit = FindEntityNearScreenLocked(
            screen_x,
            screen_y,
            viewport_width,
            viewport_height,
            0,
            false,
            kEntityTapRadiusPixels);
    if (friendly_unit >= 0 && SelectLegacyUnit(friendly_unit, 0)) {
        std::ostringstream report;
        report << "player_tap=select_screen"
               << "; unit=" << friendly_unit
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

bool IsSinglePlayerRuntimeReady() {
    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    return g_ready;
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
    report
           << "; presentation_snapshot="
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
