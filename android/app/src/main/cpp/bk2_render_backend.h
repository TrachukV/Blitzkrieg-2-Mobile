#pragma once

#include <android/native_window.h>

#include <cstdint>
#include <string>
#include <vector>

namespace bk2::android {

struct TerrainVertex {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float u = 0.0f;
    float v = 0.0f;
    uint32_t abgr = 0xffffffffu;
};

struct TerrainLayer {
    std::vector<TerrainVertex> vertices;
    std::vector<uint32_t> triangle_indices;
    int terrain_type_index = -1;
    uint16_t texture_handle = UINT16_MAX;
    uint32_t fallback_argb = 0xff61764fu;
};

struct TerrainMesh {
    std::vector<TerrainVertex> vertices;
    std::vector<uint32_t> triangle_indices;
    std::vector<uint32_t> line_indices;
    std::vector<TerrainLayer> layers;
    float center_x = 0.0f;
    float center_y = 0.0f;
    float center_z = 0.0f;
    float world_size = 1.0f;
    uint16_t texture_handle = UINT16_MAX;
};

struct WorldObjectMesh {
    struct Layer {
        std::vector<uint32_t> triangle_indices;
        std::string texture_path;
        uint16_t texture_handle = UINT16_MAX;
        bool alpha_blended = false;
    };

    std::vector<TerrainVertex> vertices;
    std::vector<uint32_t> triangle_indices;
    std::vector<Layer> layers;
};

struct TerrainCamera {
    float target_x = 0.0f;
    float target_y = 0.0f;
    float target_z = 0.0f;
    float yaw_radians = 0.0f;
    float pitch_radians = 0.85f;
    float distance = 100.0f;
};

class IRenderBackend {
public:
    virtual ~IRenderBackend() = default;

    virtual bool attach_window(ANativeWindow* window) = 0;
    virtual void detach_window() = 0;
    virtual void resize(uint32_t width, uint32_t height) = 0;
    virtual void queue_solid_rect(
            float x,
            float y,
            float width,
            float height,
            uint32_t argb) = 0;
    virtual void queue_textured_rect(
            float x,
            float y,
            float width,
            float height,
            uint16_t texture_handle,
            float u0,
            float v0,
            float u1,
            float v1,
            uint32_t argb) = 0;
    virtual bool set_terrain_mesh(const TerrainMesh& mesh) = 0;
    virtual void clear_terrain_mesh() = 0;
    virtual bool set_world_object_mesh(const WorldObjectMesh& mesh) = 0;
    virtual void clear_world_object_mesh() = 0;
    virtual void set_terrain_camera(const TerrainCamera& camera) = 0;
    virtual void render_frame() = 0;

    virtual bool is_ready() const = 0;
    virtual uint32_t width() const = 0;
    virtual uint32_t height() const = 0;
    virtual uint64_t frame_count() const = 0;
    virtual uint64_t submitted_primitives() const = 0;
    virtual const std::string& renderer_name() const = 0;
    virtual const std::string& last_error() const = 0;
};

IRenderBackend& RenderBackend();
std::string RenderBackendDiagnosticReport();

bool InitializeLegacyGfx(ANativeWindow* window);
void ResizeLegacyGfx(uint32_t width, uint32_t height);
void RenderLegacyGfxFrame();
void ShutdownLegacyGfx();

}  // namespace bk2::android
