#pragma once

#include <android/native_window.h>

#include <array>
#include <cstddef>
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
    float nx = 0.0f;
    float ny = 0.0f;
    float nz = 1.0f;
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
        bool additive_blended = false;
        bool alpha_tested = false;
        bool alpha_masked_shadow = false;
        bool depth_test_always = false;
        bool lighting_enabled = false;
        float texture_v_scroll_speed = 0.0f;
    };

    std::vector<TerrainVertex> vertices;
    std::vector<uint32_t> triangle_indices;
    std::vector<Layer> layers;
};

constexpr size_t kMaxGpuSkinBones = 48;

struct SkinnedVertex {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float u = 0.0f;
    float v = 0.0f;
    uint32_t abgr = 0xffffffffu;
    float nx = 0.0f;
    float ny = 0.0f;
    float nz = -1.0f;
    std::array<uint8_t, 4> bone_indices{};
    std::array<float, 4> bone_weights{};
};

// One draw owns one bone palette. The immutable bind-pose buffers are cached
// by geometry_key in the backend while transforms, material handles, and the
// selected animation frame can change on every presentation snapshot.
struct SkinnedWorldObject {
    struct Layer {
        uint32_t first_index = 0;
        uint32_t index_count = 0;
        std::string texture_path;
        uint16_t texture_handle = UINT16_MAX;
        bool alpha_blended = false;
        bool alpha_tested = false;
    };

    uint64_t geometry_key = 0;
    std::vector<SkinnedVertex> vertices;
    std::vector<uint32_t> triangle_indices;
    std::vector<Layer> layers;
    std::array<float, 16> world_transform{
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f};
    // Column-major matrices, 16 floats per bone.
    std::vector<float> bone_matrices;
};

struct SkinnedWorldObjectMesh {
    std::vector<SkinnedWorldObject> objects;
};

struct WaterMesh {
    std::vector<TerrainVertex> vertices;
    std::vector<uint32_t> triangle_indices;
    std::string texture_path;
    uint16_t texture_handle = UINT16_MAX;
};

struct TerrainCamera {
    float target_x = 0.0f;
    float target_y = 0.0f;
    float target_z = 0.0f;
    float yaw_radians = 0.0f;
    float pitch_radians = 0.85f;
    float distance = 100.0f;
    float horizontal_fov_degrees = 26.0f;
};

struct LegacyDirectionalLight {
    std::array<float, 3> direction{0.70710677f, 0.0f, -0.70710677f};
    std::array<float, 3> ambient_color{0.25f, 0.25f, 0.25f};
    std::array<float, 3> negative_ambient_color{0.25f, 0.25f, 0.25f};
    std::array<float, 3> light_color{0.5f, 0.5f, 0.5f};
    std::array<float, 3> shade_color{0.25f, 0.25f, 0.25f};
    bool enabled = false;
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
    virtual void queue_textured_quad(
            float x0,
            float y0,
            float x1,
            float y1,
            float x2,
            float y2,
            float x3,
            float y3,
            uint16_t texture_handle,
            float u0,
            float v0,
            float u1,
            float v1,
            uint32_t argb) = 0;
    virtual bool set_terrain_mesh(const TerrainMesh& mesh) = 0;
    virtual void clear_terrain_mesh() = 0;
    virtual bool set_water_mesh(const WaterMesh& mesh) = 0;
    virtual bool update_water_vertices(
            const std::vector<TerrainVertex>& vertices) = 0;
    virtual void clear_water_mesh() = 0;
    virtual bool set_world_object_mesh(const WorldObjectMesh& mesh) = 0;
    // Geometry that never changes during a mission. Kept in its own GPU
    // buffers so the per-frame path only rebuilds what actually moves.
    virtual bool set_static_world_object_mesh(
            const WorldObjectMesh& mesh) = 0;
    virtual bool set_skinned_world_object_mesh(
            const SkinnedWorldObjectMesh& mesh) = 0;
    // Bind-pose vertices and indices only have to travel to the backend once
    // per geometry_key; after that the GPU buffers are reused and a snapshot
    // can leave those arrays empty.
    virtual bool has_skinned_geometry(uint64_t geometry_key) const = 0;
    virtual void clear_world_object_mesh() = 0;
    virtual void set_legacy_directional_light(
            const LegacyDirectionalLight& light) = 0;
    virtual void set_legacy_terrain_light(
            const LegacyDirectionalLight& light) = 0;
    virtual void set_terrain_camera(const TerrainCamera& camera) = 0;
    virtual void set_bottom_inset(uint32_t pixels) = 0;
    virtual void render_frame() = 0;

    virtual bool is_ready() const = 0;
    virtual uint32_t width() const = 0;
    virtual uint32_t height() const = 0;
    virtual uint32_t content_height() const = 0;
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
