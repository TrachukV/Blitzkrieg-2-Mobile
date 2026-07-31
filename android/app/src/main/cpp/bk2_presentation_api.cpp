#include "bk2_presentation_internal.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <utility>

namespace {

struct PresentationState {
    uint64_t generation = 0;
    std::string mission_id;
    std::vector<Bk2PresentationVertex> terrain_vertices;
    std::vector<uint32_t> terrain_triangle_indices;
    std::vector<Bk2PresentationVertex> world_vertices;
    std::vector<uint32_t> world_triangle_indices;
    std::vector<Bk2PresentationEntity> entities;
    float center_x = 0.0f;
    float center_y = 0.0f;
    float center_z = 0.0f;
    float world_size = 0.0f;
};

std::mutex g_presentation_mutex;
PresentationState g_presentation;

template <typename T>
size_t CopyVector(
        const std::vector<T>& source,
        T* output,
        size_t capacity) {
    if (output == nullptr || capacity == 0) {
        return 0;
    }
    const size_t count = std::min(source.size(), capacity);
    std::copy_n(source.data(), count, output);
    return count;
}

void WriteJsonString(std::ostream& output, const std::string& value) {
    output << '"';
    for (const unsigned char ch : value) {
        switch (ch) {
            case '"':
                output << "\\\"";
                break;
            case '\\':
                output << "\\\\";
                break;
            case '\b':
                output << "\\b";
                break;
            case '\f':
                output << "\\f";
                break;
            case '\n':
                output << "\\n";
                break;
            case '\r':
                output << "\\r";
                break;
            case '\t':
                output << "\\t";
                break;
            default:
                if (ch < 0x20) {
                    output << "\\u"
                           << std::hex << std::setw(4) << std::setfill('0')
                           << static_cast<int>(ch)
                           << std::dec << std::setfill(' ');
                } else {
                    output << static_cast<char>(ch);
                }
                break;
        }
    }
    output << '"';
}

void WriteVertexArray(
        std::ostream& output,
        const std::vector<Bk2PresentationVertex>& vertices) {
    output << '[';
    for (size_t index = 0; index < vertices.size(); ++index) {
        if (index != 0) {
            output << ',';
        }
        const Bk2PresentationVertex& vertex = vertices[index];
        output << '['
               << vertex.x << ',' << vertex.y << ',' << vertex.z << ','
               << vertex.u << ',' << vertex.v << ',' << vertex.abgr
               << ']';
    }
    output << ']';
}

void WriteIndexArray(
        std::ostream& output,
        const std::vector<uint32_t>& indices) {
    output << '[';
    for (size_t index = 0; index < indices.size(); ++index) {
        if (index != 0) {
            output << ',';
        }
        output << indices[index];
    }
    output << ']';
}

}  // namespace

namespace bk2::presentation {

void Reset() {
    std::lock_guard<std::mutex> lock(g_presentation_mutex);
    g_presentation = PresentationState();
}

void PublishMission(std::string mission_id) {
    std::lock_guard<std::mutex> lock(g_presentation_mutex);
    g_presentation.mission_id = std::move(mission_id);
    ++g_presentation.generation;
}

void PublishTerrain(
        std::vector<Bk2PresentationVertex> vertices,
        std::vector<uint32_t> triangle_indices,
        float center_x,
        float center_y,
        float center_z,
        float world_size) {
    std::lock_guard<std::mutex> lock(g_presentation_mutex);
    g_presentation.terrain_vertices = std::move(vertices);
    g_presentation.terrain_triangle_indices = std::move(triangle_indices);
    g_presentation.center_x = center_x;
    g_presentation.center_y = center_y;
    g_presentation.center_z = center_z;
    g_presentation.world_size = world_size;
    ++g_presentation.generation;
}

void PublishWorld(
        std::vector<Bk2PresentationVertex> vertices,
        std::vector<uint32_t> triangle_indices) {
    std::lock_guard<std::mutex> lock(g_presentation_mutex);
    g_presentation.world_vertices = std::move(vertices);
    g_presentation.world_triangle_indices = std::move(triangle_indices);
    ++g_presentation.generation;
}

void PublishEntities(std::vector<Bk2PresentationEntity> entities) {
    std::lock_guard<std::mutex> lock(g_presentation_mutex);
    g_presentation.entities = std::move(entities);
    ++g_presentation.generation;
}

}  // namespace bk2::presentation

extern "C" uint32_t bk2_presentation_api_version(void) {
    return BK2_PRESENTATION_API_VERSION;
}

extern "C" Bk2PresentationSnapshotInfo bk2_presentation_snapshot_info(void) {
    std::lock_guard<std::mutex> lock(g_presentation_mutex);
    Bk2PresentationSnapshotInfo info = {};
    info.api_version = BK2_PRESENTATION_API_VERSION;
    info.generation = g_presentation.generation;
    info.terrain_vertex_count = g_presentation.terrain_vertices.size();
    info.terrain_triangle_index_count =
            g_presentation.terrain_triangle_indices.size();
    info.world_vertex_count = g_presentation.world_vertices.size();
    info.world_triangle_index_count =
            g_presentation.world_triangle_indices.size();
    info.entity_count = g_presentation.entities.size();
    info.center_x = g_presentation.center_x;
    info.center_y = g_presentation.center_y;
    info.center_z = g_presentation.center_z;
    info.world_size = g_presentation.world_size;
    std::strncpy(
            info.mission_id,
            g_presentation.mission_id.c_str(),
            BK2_PRESENTATION_MISSION_ID_CAPACITY - 1);
    return info;
}

extern "C" size_t bk2_presentation_copy_terrain_vertices(
        Bk2PresentationVertex* output,
        size_t capacity) {
    std::lock_guard<std::mutex> lock(g_presentation_mutex);
    return CopyVector(g_presentation.terrain_vertices, output, capacity);
}

extern "C" size_t bk2_presentation_copy_terrain_triangle_indices(
        uint32_t* output,
        size_t capacity) {
    std::lock_guard<std::mutex> lock(g_presentation_mutex);
    return CopyVector(g_presentation.terrain_triangle_indices, output, capacity);
}

extern "C" size_t bk2_presentation_copy_world_vertices(
        Bk2PresentationVertex* output,
        size_t capacity) {
    std::lock_guard<std::mutex> lock(g_presentation_mutex);
    return CopyVector(g_presentation.world_vertices, output, capacity);
}

extern "C" size_t bk2_presentation_copy_world_triangle_indices(
        uint32_t* output,
        size_t capacity) {
    std::lock_guard<std::mutex> lock(g_presentation_mutex);
    return CopyVector(g_presentation.world_triangle_indices, output, capacity);
}

extern "C" size_t bk2_presentation_copy_entities(
        Bk2PresentationEntity* output,
        size_t capacity) {
    std::lock_guard<std::mutex> lock(g_presentation_mutex);
    return CopyVector(g_presentation.entities, output, capacity);
}

extern "C" int bk2_presentation_write_json(const char* path) {
    if (path == nullptr || path[0] == '\0') {
        return 0;
    }
    std::lock_guard<std::mutex> lock(g_presentation_mutex);
    std::ofstream output(path, std::ios::out | std::ios::trunc);
    if (!output.is_open()) {
        return 0;
    }
    output << std::setprecision(9);
    output << "{\"api_version\":" << BK2_PRESENTATION_API_VERSION
           << ",\"generation\":" << g_presentation.generation
           << ",\"mission_id\":";
    WriteJsonString(output, g_presentation.mission_id);
    output << ",\"center\":["
           << g_presentation.center_x << ','
           << g_presentation.center_y << ','
           << g_presentation.center_z << ']'
           << ",\"world_size\":" << g_presentation.world_size
           << ",\"terrain\":{\"vertices\":";
    WriteVertexArray(output, g_presentation.terrain_vertices);
    output << ",\"indices\":";
    WriteIndexArray(output, g_presentation.terrain_triangle_indices);
    output << "},\"world\":{\"vertices\":";
    WriteVertexArray(output, g_presentation.world_vertices);
    output << ",\"indices\":";
    WriteIndexArray(output, g_presentation.world_triangle_indices);
    output << "},\"entities\":[";
    for (size_t index = 0; index < g_presentation.entities.size(); ++index) {
        if (index != 0) {
            output << ',';
        }
        const Bk2PresentationEntity& entity = g_presentation.entities[index];
        output << "{\"id\":" << entity.id
               << ",\"player\":" << entity.player
               << ",\"flags\":" << entity.flags
               << ",\"position\":["
               << entity.x << ',' << entity.y << ',' << entity.z << ']'
               << ",\"heading\":" << entity.heading_radians
               << ",\"hit_points\":" << entity.hit_points
               << ",\"max_hit_points\":" << entity.max_hit_points
               << ",\"rpg_stats_path_hash\":" << entity.rpg_stats_path_hash
               << ",\"rpg_stats_record_id\":" << entity.rpg_stats_record_id
               << ",\"geometry_record_id\":" << entity.geometry_record_id
               << ",\"visual_scale\":" << entity.visual_scale
               << ",\"root_tilt_axis\":["
               << entity.root_tilt_axis_x << ','
               << entity.root_tilt_axis_y << ']'
               << ",\"root_tilt\":" << entity.root_tilt_radians
               << ",\"animation_type\":" << entity.animation_type
               << ",\"animation_elapsed_seconds\":"
               << entity.animation_elapsed_seconds
               << '}';
    }
    output << "]}";
    output.flush();
    return output.good() ? 1 : 0;
}
