#include "bk2_render_backend.h"

#include "bk2_android_platform.h"

#include <bgfx/bgfx.h>
#include <bgfx/embedded_shader.h>
#include <bx/math.h>

#include <sys/system_properties.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <sstream>
#include <vector>

#include "fs_debugdraw_fill.bin.h"
#include "fs_debugdraw_fill_texture.bin.h"
#include "vs_debugdraw_fill_mesh.bin.h"
#include "vs_debugdraw_fill_texture.bin.h"
#include "shaders/generated/fs_bk2_alpha_masked_shadow_essl.bin.h"
#include "shaders/generated/fs_bk2_alpha_masked_shadow_spv.bin.h"
#include "shaders/generated/fs_bk2_alpha_test_essl.bin.h"
#include "shaders/generated/fs_bk2_alpha_test_spv.bin.h"

namespace bk2::android {
namespace {

constexpr bgfx::ViewId kTerrainView = 0;
constexpr bgfx::ViewId kUiView = 1;
constexpr uint32_t kClearColor = 0x000000ff;

const bgfx::EmbeddedShader kRectShaders[] = {
        BGFX_EMBEDDED_SHADER(vs_debugdraw_fill_mesh),
        BGFX_EMBEDDED_SHADER(fs_debugdraw_fill),
        BGFX_EMBEDDED_SHADER(vs_debugdraw_fill_texture),
        BGFX_EMBEDDED_SHADER(fs_debugdraw_fill_texture),
        BGFX_EMBEDDED_SHADER_END()};

const bgfx::EmbeddedShader kAlphaTestShaders[] = {
        BGFX_EMBEDDED_SHADER(fs_bk2_alpha_test),
        BGFX_EMBEDDED_SHADER_END()};

const bgfx::EmbeddedShader kAlphaMaskedShadowShaders[] = {
        BGFX_EMBEDDED_SHADER(fs_bk2_alpha_masked_shadow),
        BGFX_EMBEDDED_SHADER_END()};

const float kIdentityMatrix[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f};

struct SolidRect {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    uint32_t argb = 0xffffffffu;
};

struct TexturedRect {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    uint16_t texture_handle = UINT16_MAX;
    float u0 = 0.0f;
    float v0 = 0.0f;
    float u1 = 1.0f;
    float v1 = 1.0f;
    uint32_t argb = 0xffffffffu;
};

struct RectVertex {
    float x;
    float y;
    float z;
};

struct TexturedRectVertex {
    float x;
    float y;
    float z;
    float u;
    float v;
    uint32_t abgr;
};

bgfx::VertexLayout BuildRectVertexLayout() {
    bgfx::VertexLayout layout;
    layout.begin()
            .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
            .end();
    return layout;
}

bgfx::VertexLayout BuildTexturedRectVertexLayout() {
    bgfx::VertexLayout layout;
    layout.begin()
            .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
            .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
            .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
            .end();
    return layout;
}

uint32_t ArgbToAbgr(uint32_t argb) {
    return (argb & 0xff00ff00u) |
           ((argb & 0x00ff0000u) >> 16) |
           ((argb & 0x000000ffu) << 16);
}

bool IsAndroidEmulator() {
    char value[PROP_VALUE_MAX] = {};
    if (__system_property_get("ro.kernel.qemu", value) > 0 &&
        std::strcmp(value, "1") == 0) {
        return true;
    }
    if (__system_property_get("ro.hardware", value) > 0 &&
        std::strstr(value, "ranchu") != nullptr) {
        return true;
    }
    return false;
}

bgfx::RendererType::Enum PreferredRenderer() {
    return IsAndroidEmulator()
            ? bgfx::RendererType::OpenGLES
            : bgfx::RendererType::Vulkan;
}

class BgfxRenderBackend final : public IRenderBackend {
public:
    ~BgfxRenderBackend() override {
        detach_window();
    }

    bool attach_window(ANativeWindow* window) override {
        if (window == nullptr) {
            last_error_ = "native window is null";
            return false;
        }
        if (ready_ && window_ == window) {
            resize(
                    static_cast<uint32_t>(ANativeWindow_getWidth(window)),
                    static_cast<uint32_t>(ANativeWindow_getHeight(window)));
            return true;
        }
        detach_window();

        window_ = window;
        ANativeWindow_acquire(window_);
        width_ = static_cast<uint32_t>(
                std::max(ANativeWindow_getWidth(window_), 1));
        height_ = static_cast<uint32_t>(
                std::max(ANativeWindow_getHeight(window_), 1));

        const bgfx::RendererType::Enum preferred_renderer = PreferredRenderer();
        bgfx::Init init;
        init.type = preferred_renderer;
        init.fallback = preferred_renderer == bgfx::RendererType::Vulkan;
        init.platformData.nwh = window_;
        init.resolution.width = width_;
        init.resolution.height = height_;
        init.resolution.reset = BGFX_RESET_VSYNC;
        init.debug = false;
        init.profile = false;

        if (!bgfx::init(init)) {
            last_error_ = "bgfx initialization failed";
            ANativeWindow_release(window_);
            window_ = nullptr;
            width_ = 0;
            height_ = 0;
            return false;
        }

        ready_ = true;
        frame_count_ = 0;
        submitted_primitives_ = 0;
        renderer_name_ = bgfx::getRendererName(bgfx::getRendererType());
        last_error_.clear();
        rect_layout_ = BuildRectVertexLayout();
        textured_rect_layout_ = BuildTexturedRectVertexLayout();
        terrain_layout_ = BuildTexturedRectVertexLayout();
        rect_uniform_ =
                bgfx::createUniform("u_params", bgfx::UniformType::Vec4, 4);
        texture_sampler_ =
                bgfx::createUniform("s_texColor", bgfx::UniformType::Sampler);
        rect_program_ = bgfx::createProgram(
                bgfx::createEmbeddedShader(
                        kRectShaders,
                        bgfx::getRendererType(),
                        "vs_debugdraw_fill_mesh"),
                bgfx::createEmbeddedShader(
                        kRectShaders,
                        bgfx::getRendererType(),
                        "fs_debugdraw_fill"),
                true);
        textured_rect_program_ = bgfx::createProgram(
                bgfx::createEmbeddedShader(
                        kRectShaders,
                        bgfx::getRendererType(),
                        "vs_debugdraw_fill_texture"),
                bgfx::createEmbeddedShader(
                        kRectShaders,
                        bgfx::getRendererType(),
                        "fs_debugdraw_fill_texture"),
                true);
        alpha_test_program_ = bgfx::createProgram(
                bgfx::createEmbeddedShader(
                        kRectShaders,
                        bgfx::getRendererType(),
                        "vs_debugdraw_fill_texture"),
                bgfx::createEmbeddedShader(
                        kAlphaTestShaders,
                        bgfx::getRendererType(),
                        "fs_bk2_alpha_test"),
                true);
        alpha_masked_shadow_program_ = bgfx::createProgram(
                bgfx::createEmbeddedShader(
                        kRectShaders,
                        bgfx::getRendererType(),
                        "vs_debugdraw_fill_texture"),
                bgfx::createEmbeddedShader(
                        kAlphaMaskedShadowShaders,
                        bgfx::getRendererType(),
                        "fs_bk2_alpha_masked_shadow"),
                true);
        if (!bgfx::isValid(rect_program_) ||
            !bgfx::isValid(textured_rect_program_) ||
            !bgfx::isValid(alpha_test_program_) ||
            !bgfx::isValid(alpha_masked_shadow_program_) ||
            !bgfx::isValid(rect_uniform_) ||
            !bgfx::isValid(texture_sampler_)) {
            last_error_ = "bgfx primitive shader initialization failed";
            detach_window();
            return false;
        }
        const uint32_t white_pixel = 0xffffffffu;
        white_texture_ = bgfx::createTexture2D(
                1,
                1,
                false,
                1,
                bgfx::TextureFormat::RGBA8,
                BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP,
                bgfx::copy(&white_pixel, sizeof(white_pixel)));
        if (!bgfx::isValid(white_texture_)) {
            last_error_ = "bgfx white texture initialization failed";
            detach_window();
            return false;
        }
        upload_terrain_mesh();
        upload_world_object_mesh();
        applied_bottom_inset_ = clamped_requested_bottom_inset();
        configure_view();
        PlatformRuntime::instance().log_info(
                std::string("bgfx renderer initialized: ") + renderer_name_);
        return true;
    }

    void detach_window() override {
        rect_queue_.clear();
        textured_rect_queue_.clear();
        destroy_terrain_buffers();
        destroy_world_object_buffers();
        if (bgfx::isValid(white_texture_)) {
            bgfx::destroy(white_texture_);
            white_texture_ = BGFX_INVALID_HANDLE;
        }
        if (bgfx::isValid(textured_rect_program_)) {
            bgfx::destroy(textured_rect_program_);
            textured_rect_program_ = BGFX_INVALID_HANDLE;
        }
        if (bgfx::isValid(alpha_test_program_)) {
            bgfx::destroy(alpha_test_program_);
            alpha_test_program_ = BGFX_INVALID_HANDLE;
        }
        if (bgfx::isValid(alpha_masked_shadow_program_)) {
            bgfx::destroy(alpha_masked_shadow_program_);
            alpha_masked_shadow_program_ = BGFX_INVALID_HANDLE;
        }
        if (bgfx::isValid(rect_program_)) {
            bgfx::destroy(rect_program_);
            rect_program_ = BGFX_INVALID_HANDLE;
        }
        if (bgfx::isValid(texture_sampler_)) {
            bgfx::destroy(texture_sampler_);
            texture_sampler_ = BGFX_INVALID_HANDLE;
        }
        if (bgfx::isValid(rect_uniform_)) {
            bgfx::destroy(rect_uniform_);
            rect_uniform_ = BGFX_INVALID_HANDLE;
        }
        if (ready_) {
            bgfx::shutdown();
            ready_ = false;
        }
        if (window_ != nullptr) {
            ANativeWindow_release(window_);
            window_ = nullptr;
        }
        width_ = 0;
        height_ = 0;
        applied_bottom_inset_ = 0;
        renderer_name_.clear();
    }

    void resize(uint32_t width, uint32_t height) override {
        width = std::max(width, 1u);
        height = std::max(height, 1u);
        if (!ready_ || (width_ == width && height_ == height)) {
            return;
        }
        width_ = width;
        height_ = height;
        bgfx::reset(width_, height_, BGFX_RESET_VSYNC);
        applied_bottom_inset_ = clamped_requested_bottom_inset();
        configure_view();
    }

    void queue_solid_rect(
            float x,
            float y,
            float width,
            float height,
            uint32_t argb) override {
        if (!ready_ || width == 0.0f || height == 0.0f) {
            return;
        }
        SolidRect rect;
        rect.x = x;
        rect.y = y;
        rect.width = width;
        rect.height = height;
        rect.argb = argb;
        rect_queue_.push_back(rect);
    }

    void queue_textured_rect(
            float x,
            float y,
            float width,
            float height,
            uint16_t texture_handle,
            float u0,
            float v0,
            float u1,
            float v1,
            uint32_t argb) override {
        if (!ready_ || width == 0.0f || height == 0.0f ||
            texture_handle == UINT16_MAX) {
            return;
        }
        TexturedRect rect;
        rect.x = x;
        rect.y = y;
        rect.width = width;
        rect.height = height;
        rect.texture_handle = texture_handle;
        rect.u0 = u0;
        rect.v0 = v0;
        rect.u1 = u1;
        rect.v1 = v1;
        rect.argb = argb;
        textured_rect_queue_.push_back(rect);
    }

    bool set_terrain_mesh(const TerrainMesh& mesh) override {
        terrain_mesh_ = mesh;
        if (!ready_) {
            return !terrain_mesh_.vertices.empty();
        }
        return upload_terrain_mesh();
    }

    void clear_terrain_mesh() override {
        terrain_mesh_ = TerrainMesh();
        destroy_terrain_buffers();
    }

    bool set_world_object_mesh(const WorldObjectMesh& mesh) override {
        world_object_mesh_ = mesh;
        if (!ready_) {
            return !world_object_mesh_.vertices.empty();
        }
        return upload_world_object_mesh();
    }

    void clear_world_object_mesh() override {
        world_object_mesh_ = WorldObjectMesh();
        destroy_world_object_buffers();
    }

    void set_terrain_camera(const TerrainCamera& camera) override {
        terrain_camera_ = camera;
    }

    void set_bottom_inset(uint32_t pixels) override {
        requested_bottom_inset_.store(
                pixels,
                std::memory_order_release);
    }

    void render_frame() override {
        if (!ready_) {
            return;
        }
        apply_requested_bottom_inset();
        submit_terrain();
        submit_world_objects();
        submit_rects();
        submit_textured_rects();
        bgfx::touch(kTerrainView);
        bgfx::touch(kUiView);
        bgfx::frame();
        ++frame_count_;
    }

    bool is_ready() const override {
        return ready_;
    }

    uint32_t width() const override {
        return width_;
    }

    uint32_t height() const override {
        return height_;
    }

    uint32_t content_height() const override {
        const uint32_t inset = std::min(
                requested_bottom_inset_.load(
                        std::memory_order_acquire),
                height_ > 1 ? height_ - 1 : 0);
        return std::max(height_ - inset, 1u);
    }

    uint64_t frame_count() const override {
        return frame_count_;
    }

    uint64_t submitted_primitives() const override {
        return submitted_primitives_;
    }

    const std::string& renderer_name() const override {
        return renderer_name_;
    }

    const std::string& last_error() const override {
        return last_error_;
    }

private:
    uint32_t clamped_requested_bottom_inset() const {
        return std::min(
                requested_bottom_inset_.load(
                        std::memory_order_acquire),
                height_ > 1 ? height_ - 1 : 0);
    }

    uint32_t rendered_content_height() const {
        return std::max(height_ - applied_bottom_inset_, 1u);
    }

    void apply_requested_bottom_inset() {
        const uint32_t requested =
                clamped_requested_bottom_inset();
        if (requested == applied_bottom_inset_) {
            return;
        }
        applied_bottom_inset_ = requested;
        configure_view();
        PlatformRuntime::instance().log_info(
                std::string("render_viewport=mission; size=") +
                std::to_string(width_) + "x" +
                std::to_string(rendered_content_height()) +
                "; bottom_inset=" +
                std::to_string(applied_bottom_inset_));
    }

    void configure_view() {
        bgfx::setViewRect(
                kTerrainView,
                0,
                0,
                static_cast<uint16_t>(std::min(width_, 65535u)),
                static_cast<uint16_t>(
                        std::min(
                                rendered_content_height(),
                                65535u)));
        bgfx::setViewClear(
                kTerrainView,
                BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH,
                kClearColor,
                1.0f,
                0);
        bgfx::setViewRect(
                kUiView,
                0,
                0,
                static_cast<uint16_t>(std::min(width_, 65535u)),
                static_cast<uint16_t>(std::min(height_, 65535u)));
        bgfx::setViewClear(kUiView, BGFX_CLEAR_NONE);
        bgfx::setViewTransform(kUiView, kIdentityMatrix, kIdentityMatrix);
    }

    void destroy_terrain_buffers() {
        for (bgfx::VertexBufferHandle handle :
             terrain_layer_vertex_buffers_) {
            if (bgfx::isValid(handle)) {
                bgfx::destroy(handle);
            }
        }
        terrain_layer_vertex_buffers_.clear();
        for (bgfx::IndexBufferHandle handle :
             terrain_layer_index_buffers_) {
            if (bgfx::isValid(handle)) {
                bgfx::destroy(handle);
            }
        }
        terrain_layer_index_buffers_.clear();
        if (bgfx::isValid(terrain_line_index_buffer_)) {
            bgfx::destroy(terrain_line_index_buffer_);
            terrain_line_index_buffer_ = BGFX_INVALID_HANDLE;
        }
        if (bgfx::isValid(terrain_index_buffer_)) {
            bgfx::destroy(terrain_index_buffer_);
            terrain_index_buffer_ = BGFX_INVALID_HANDLE;
        }
        if (bgfx::isValid(terrain_vertex_buffer_)) {
            bgfx::destroy(terrain_vertex_buffer_);
            terrain_vertex_buffer_ = BGFX_INVALID_HANDLE;
        }
    }

    void destroy_world_object_buffers() {
        for (bgfx::IndexBufferHandle handle :
             world_object_layer_index_buffers_) {
            if (bgfx::isValid(handle)) {
                bgfx::destroy(handle);
            }
        }
        world_object_layer_index_buffers_.clear();
        if (bgfx::isValid(world_object_index_buffer_)) {
            bgfx::destroy(world_object_index_buffer_);
            world_object_index_buffer_ = BGFX_INVALID_HANDLE;
        }
        if (bgfx::isValid(world_object_vertex_buffer_)) {
            bgfx::destroy(world_object_vertex_buffer_);
            world_object_vertex_buffer_ = BGFX_INVALID_HANDLE;
        }
    }

    bool upload_terrain_mesh() {
        destroy_terrain_buffers();
        if (!ready_ || terrain_mesh_.vertices.empty() ||
            terrain_mesh_.triangle_indices.empty()) {
            return false;
        }

        terrain_vertex_buffer_ = bgfx::createVertexBuffer(
                bgfx::copy(
                        terrain_mesh_.vertices.data(),
                        static_cast<uint32_t>(
                                terrain_mesh_.vertices.size() *
                                sizeof(TerrainVertex))),
                terrain_layout_);
        terrain_index_buffer_ = bgfx::createIndexBuffer(
                bgfx::copy(
                        terrain_mesh_.triangle_indices.data(),
                        static_cast<uint32_t>(
                                terrain_mesh_.triangle_indices.size() *
                                sizeof(uint32_t))),
                BGFX_BUFFER_INDEX32);
        if (!terrain_mesh_.line_indices.empty()) {
            terrain_line_index_buffer_ = bgfx::createIndexBuffer(
                    bgfx::copy(
                            terrain_mesh_.line_indices.data(),
                            static_cast<uint32_t>(
                                    terrain_mesh_.line_indices.size() *
                                    sizeof(uint32_t))),
                    BGFX_BUFFER_INDEX32);
        }
        terrain_layer_vertex_buffers_.reserve(terrain_mesh_.layers.size());
        terrain_layer_index_buffers_.reserve(terrain_mesh_.layers.size());
        for (const TerrainLayer& layer : terrain_mesh_.layers) {
            bgfx::VertexBufferHandle vertex_handle =
                    bgfx::createVertexBuffer(
                            bgfx::copy(
                                    layer.vertices.data(),
                                    static_cast<uint32_t>(
                                            layer.vertices.size() *
                                            sizeof(TerrainVertex))),
                            terrain_layout_);
            bgfx::IndexBufferHandle handle = bgfx::createIndexBuffer(
                    bgfx::copy(
                            layer.triangle_indices.data(),
                        static_cast<uint32_t>(
                                layer.triangle_indices.size() *
                                sizeof(uint32_t))),
                    BGFX_BUFFER_INDEX32);
            terrain_layer_vertex_buffers_.push_back(vertex_handle);
            terrain_layer_index_buffers_.push_back(handle);
        }
        if (!bgfx::isValid(terrain_vertex_buffer_) ||
            !bgfx::isValid(terrain_index_buffer_)) {
            last_error_ = "terrain GPU buffer creation failed";
            destroy_terrain_buffers();
            return false;
        }
        return true;
    }

    bool upload_world_object_mesh() {
        destroy_world_object_buffers();
        if (!ready_ || world_object_mesh_.vertices.empty() ||
            (world_object_mesh_.triangle_indices.empty() &&
             world_object_mesh_.layers.empty())) {
            return false;
        }

        world_object_vertex_buffer_ = bgfx::createVertexBuffer(
                bgfx::copy(
                        world_object_mesh_.vertices.data(),
                        static_cast<uint32_t>(
                                world_object_mesh_.vertices.size() *
                                sizeof(TerrainVertex))),
                terrain_layout_);
        if (!world_object_mesh_.triangle_indices.empty()) {
            world_object_index_buffer_ = bgfx::createIndexBuffer(
                    bgfx::copy(
                            world_object_mesh_.triangle_indices.data(),
                            static_cast<uint32_t>(
                                    world_object_mesh_.triangle_indices.size() *
                                    sizeof(uint32_t))),
                    BGFX_BUFFER_INDEX32);
        }
        world_object_layer_index_buffers_.reserve(
                world_object_mesh_.layers.size());
        bool has_index_buffer = bgfx::isValid(world_object_index_buffer_);
        for (const WorldObjectMesh::Layer& layer :
             world_object_mesh_.layers) {
            bgfx::IndexBufferHandle handle = BGFX_INVALID_HANDLE;
            if (!layer.triangle_indices.empty()) {
                handle = bgfx::createIndexBuffer(
                        bgfx::copy(
                                layer.triangle_indices.data(),
                                static_cast<uint32_t>(
                                        layer.triangle_indices.size() *
                                        sizeof(uint32_t))),
                        BGFX_BUFFER_INDEX32);
                has_index_buffer = has_index_buffer || bgfx::isValid(handle);
            }
            world_object_layer_index_buffers_.push_back(handle);
        }
        if (!bgfx::isValid(world_object_vertex_buffer_) ||
            !has_index_buffer) {
            last_error_ = "world object GPU buffer creation failed";
            destroy_world_object_buffers();
            return false;
        }
        return true;
    }

    void set_fill_color(uint32_t argb) {
        const float color[4][4] = {
                {0.0f, 0.0f, 1.0f, 0.0f},
                {0.0f, 0.0f, 0.0f, 0.0f},
                {0.0f, 0.0f, 0.0f, 0.0f},
                {
                        static_cast<float>((argb >> 16) & 0xff) / 255.0f,
                        static_cast<float>((argb >> 8) & 0xff) / 255.0f,
                        static_cast<float>(argb & 0xff) / 255.0f,
                        static_cast<float>((argb >> 24) & 0xff) / 255.0f,
                }};
        bgfx::setUniform(rect_uniform_, color, 4);
    }

    void submit_terrain() {
        if (!bgfx::isValid(terrain_vertex_buffer_) ||
            !bgfx::isValid(terrain_index_buffer_) ||
            !bgfx::isValid(rect_program_) ||
            !bgfx::isValid(rect_uniform_)) {
            return;
        }

        const float horizontal_distance =
                terrain_camera_.distance *
                std::cos(terrain_camera_.pitch_radians);
        const bx::Vec3 eye = {
                terrain_camera_.target_x +
                        std::sin(terrain_camera_.yaw_radians) *
                                horizontal_distance,
                terrain_camera_.target_y -
                        std::cos(terrain_camera_.yaw_radians) *
                                horizontal_distance,
                terrain_camera_.target_z +
                        std::sin(terrain_camera_.pitch_radians) *
                                terrain_camera_.distance};
        const bx::Vec3 target = {
                terrain_camera_.target_x,
                terrain_camera_.target_y,
                terrain_camera_.target_z};
        const bx::Vec3 up = {0.0f, 0.0f, 1.0f};
        float view[16];
        float projection[16];
        bx::mtxLookAt(view, eye, target, up);
        const float aspect =
                static_cast<float>(width_) /
                static_cast<float>(rendered_content_height());
        const float horizontal_half_fov =
                bx::toRad(terrain_camera_.horizontal_fov_degrees * 0.5f);
        const float vertical_fov_degrees =
                bx::toDeg(
                        2.0f *
                        std::atan(
                                std::tan(horizontal_half_fov) /
                                aspect));
        bx::mtxProj(
                projection,
                vertical_fov_degrees,
                aspect,
                0.5f,
                std::max(terrain_mesh_.world_size * 8.0f, 1000.0f),
                bgfx::getCaps()->homogeneousDepth);
        bgfx::setViewTransform(kTerrainView, view, projection);

        const uint64_t terrain_state =
                BGFX_STATE_WRITE_RGB |
                BGFX_STATE_WRITE_A |
                BGFX_STATE_WRITE_Z |
                BGFX_STATE_DEPTH_TEST_LEQUAL |
                BGFX_STATE_MSAA |
                BGFX_STATE_BLEND_ALPHA;
        const bool has_layers =
                !terrain_mesh_.layers.empty() &&
                terrain_layer_vertex_buffers_.size() ==
                        terrain_mesh_.layers.size() &&
                terrain_layer_index_buffers_.size() ==
                        terrain_mesh_.layers.size();
        if (has_layers) {
            for (size_t index = 0;
                 index < terrain_mesh_.layers.size();
                 ++index) {
                const TerrainLayer& layer = terrain_mesh_.layers[index];
                const bgfx::VertexBufferHandle vertex_buffer =
                        terrain_layer_vertex_buffers_[index];
                const bgfx::IndexBufferHandle index_buffer =
                        terrain_layer_index_buffers_[index];
                if (!bgfx::isValid(vertex_buffer) ||
                    !bgfx::isValid(index_buffer)) {
                    continue;
                }
                bgfx::setTransform(kIdentityMatrix);
                bgfx::setVertexBuffer(0, vertex_buffer);
                bgfx::setIndexBuffer(index_buffer);
                bgfx::setState(terrain_state);
                const bgfx::TextureHandle texture = {
                        layer.texture_handle};
                if (layer.texture_handle != UINT16_MAX &&
                    bgfx::isValid(texture) &&
                    bgfx::isValid(textured_rect_program_) &&
                    bgfx::isValid(texture_sampler_)) {
                    bgfx::setTexture(0, texture_sampler_, texture);
                    bgfx::submit(
                            kTerrainView,
                            textured_rect_program_);
                } else {
                    set_fill_color(layer.fallback_argb);
                    bgfx::submit(kTerrainView, rect_program_);
                }
                ++submitted_primitives_;
            }
        } else {
            bgfx::setTransform(kIdentityMatrix);
            bgfx::setVertexBuffer(0, terrain_vertex_buffer_);
            bgfx::setIndexBuffer(terrain_index_buffer_);
            bgfx::setState(terrain_state);
            if (terrain_mesh_.texture_handle != UINT16_MAX &&
                bgfx::isValid(textured_rect_program_) &&
                bgfx::isValid(texture_sampler_)) {
                const bgfx::TextureHandle texture = {
                        terrain_mesh_.texture_handle};
                bgfx::setTexture(0, texture_sampler_, texture);
                bgfx::submit(
                        kTerrainView,
                        textured_rect_program_);
            } else {
                set_fill_color(0xff61764fu);
                bgfx::submit(kTerrainView, rect_program_);
            }
            ++submitted_primitives_;
        }

        if (!has_layers &&
            bgfx::isValid(terrain_line_index_buffer_)) {
            bgfx::setTransform(kIdentityMatrix);
            set_fill_color(0x906f8560u);
            bgfx::setVertexBuffer(0, terrain_vertex_buffer_);
            bgfx::setIndexBuffer(terrain_line_index_buffer_);
            bgfx::setState(
                    BGFX_STATE_WRITE_RGB |
                    BGFX_STATE_WRITE_A |
                    BGFX_STATE_DEPTH_TEST_LEQUAL |
                    BGFX_STATE_BLEND_ALPHA |
                    BGFX_STATE_PT_LINES |
                    BGFX_STATE_MSAA);
            bgfx::submit(kTerrainView, rect_program_);
            ++submitted_primitives_;
        }
    }

    void submit_world_objects() {
        if (!bgfx::isValid(world_object_vertex_buffer_) ||
            !bgfx::isValid(textured_rect_program_) ||
            !bgfx::isValid(texture_sampler_) ||
            !bgfx::isValid(white_texture_)) {
            return;
        }

        const uint64_t state =
                BGFX_STATE_WRITE_RGB |
                BGFX_STATE_WRITE_A |
                BGFX_STATE_WRITE_Z |
                BGFX_STATE_DEPTH_TEST_LESS |
                BGFX_STATE_MSAA;
        if (bgfx::isValid(world_object_index_buffer_)) {
            bgfx::setTransform(kIdentityMatrix);
            bgfx::setTexture(0, texture_sampler_, white_texture_);
            bgfx::setVertexBuffer(0, world_object_vertex_buffer_);
            bgfx::setIndexBuffer(world_object_index_buffer_);
            bgfx::setState(state);
            bgfx::submit(kTerrainView, textured_rect_program_);
            ++submitted_primitives_;
        }
        const size_t layer_count = std::min(
                world_object_mesh_.layers.size(),
                world_object_layer_index_buffers_.size());
        for (size_t index = 0; index < layer_count; ++index) {
            const bgfx::IndexBufferHandle index_buffer =
                    world_object_layer_index_buffers_[index];
            if (!bgfx::isValid(index_buffer)) {
                continue;
            }
            bgfx::TextureHandle texture = white_texture_;
            const uint16_t texture_index =
                    world_object_mesh_.layers[index].texture_handle;
            if (texture_index != UINT16_MAX) {
                const bgfx::TextureHandle candidate = {texture_index};
                if (bgfx::isValid(candidate)) {
                    texture = candidate;
                }
            }
            bgfx::setTransform(kIdentityMatrix);
            bgfx::setTexture(0, texture_sampler_, texture);
            bgfx::setVertexBuffer(0, world_object_vertex_buffer_);
            bgfx::setIndexBuffer(index_buffer);
            const WorldObjectMesh::Layer& layer =
                    world_object_mesh_.layers[index];
            uint64_t layer_state = layer.depth_test_always
                    ? BGFX_STATE_WRITE_RGB |
                            BGFX_STATE_WRITE_A |
                            BGFX_STATE_MSAA
                    : state;
            if (layer.additive_blended) {
                layer_state &= ~BGFX_STATE_WRITE_Z;
                layer_state |= BGFX_STATE_BLEND_ADD;
            } else if (layer.alpha_blended) {
                layer_state &= ~BGFX_STATE_WRITE_Z;
                layer_state |= BGFX_STATE_BLEND_ALPHA;
            }
            const bgfx::ProgramHandle program =
                    layer.alpha_masked_shadow
                    ? alpha_masked_shadow_program_
                    : layer.alpha_tested
                            ? alpha_test_program_
                            : textured_rect_program_;
            if (layer.alpha_tested || layer.alpha_masked_shadow) {
                layer_state |= BGFX_STATE_ALPHA_REF(120);
            }
            bgfx::setState(layer_state);
            bgfx::submit(kTerrainView, program);
            ++submitted_primitives_;
        }
    }

    void submit_rects() {
        if (rect_queue_.empty() ||
            !bgfx::isValid(rect_program_) ||
            !bgfx::isValid(rect_uniform_)) {
            rect_queue_.clear();
            return;
        }

        const uint16_t indices[6] = {0, 1, 2, 0, 2, 3};
        for (const SolidRect& rect : rect_queue_) {
            bgfx::TransientVertexBuffer tvb;
            bgfx::TransientIndexBuffer tib;
            if (!bgfx::allocTransientBuffers(
                        &tvb,
                        rect_layout_,
                        4,
                        &tib,
                        6)) {
                break;
            }

            const float x0 =
                    (rect.x / static_cast<float>(width_)) * 2.0f - 1.0f;
            const float x1 =
                    ((rect.x + rect.width) / static_cast<float>(width_)) *
                            2.0f -
                    1.0f;
            const float y0 =
                    1.0f - (rect.y / static_cast<float>(height_)) * 2.0f;
            const float y1 =
                    1.0f -
                    ((rect.y + rect.height) / static_cast<float>(height_)) *
                            2.0f;

            RectVertex* vertices = reinterpret_cast<RectVertex*>(tvb.data);
            vertices[0] = {x0, y0, 0.0f};
            vertices[1] = {x0, y1, 0.0f};
            vertices[2] = {x1, y1, 0.0f};
            vertices[3] = {x1, y0, 0.0f};
            std::memcpy(tib.data, indices, sizeof(indices));

            const float color[4][4] = {
                    {0.0f, 0.0f, 1.0f, 0.0f},
                    {0.0f, 0.0f, 0.0f, 0.0f},
                    {0.0f, 0.0f, 0.0f, 0.0f},
                    {
                            static_cast<float>((rect.argb >> 16) & 0xff) /
                                    255.0f,
                            static_cast<float>((rect.argb >> 8) & 0xff) /
                                    255.0f,
                            static_cast<float>(rect.argb & 0xff) / 255.0f,
                            static_cast<float>((rect.argb >> 24) & 0xff) /
                                    255.0f,
                    }};

            bgfx::setViewTransform(kUiView, kIdentityMatrix, kIdentityMatrix);
            bgfx::setTransform(kIdentityMatrix);
            bgfx::setUniform(rect_uniform_, color, 4);
            bgfx::setVertexBuffer(0, &tvb);
            bgfx::setIndexBuffer(&tib);
            uint64_t state =
                    BGFX_STATE_WRITE_RGB |
                    BGFX_STATE_WRITE_A |
                    BGFX_STATE_MSAA;
            if (((rect.argb >> 24) & 0xff) < 0xff) {
                state |= BGFX_STATE_BLEND_ALPHA;
            }
            bgfx::setState(state);
            bgfx::submit(kUiView, rect_program_);
            ++submitted_primitives_;
        }
        rect_queue_.clear();
    }

    void submit_textured_rects() {
        if (textured_rect_queue_.empty() ||
            !bgfx::isValid(textured_rect_program_) ||
            !bgfx::isValid(texture_sampler_)) {
            textured_rect_queue_.clear();
            return;
        }

        const uint16_t indices[6] = {0, 1, 2, 0, 2, 3};
        for (const TexturedRect& rect : textured_rect_queue_) {
            bgfx::TransientVertexBuffer tvb;
            bgfx::TransientIndexBuffer tib;
            if (!bgfx::allocTransientBuffers(
                        &tvb,
                        textured_rect_layout_,
                        4,
                        &tib,
                        6)) {
                break;
            }

            const float x0 =
                    (rect.x / static_cast<float>(width_)) * 2.0f - 1.0f;
            const float x1 =
                    ((rect.x + rect.width) / static_cast<float>(width_)) *
                            2.0f -
                    1.0f;
            const float y0 =
                    1.0f - (rect.y / static_cast<float>(height_)) * 2.0f;
            const float y1 =
                    1.0f -
                    ((rect.y + rect.height) / static_cast<float>(height_)) *
                            2.0f;

            const uint32_t abgr = ArgbToAbgr(rect.argb);
            TexturedRectVertex* vertices =
                    reinterpret_cast<TexturedRectVertex*>(tvb.data);
            vertices[0] = {x0, y0, 0.0f, rect.u0, rect.v0, abgr};
            vertices[1] = {x0, y1, 0.0f, rect.u0, rect.v1, abgr};
            vertices[2] = {x1, y1, 0.0f, rect.u1, rect.v1, abgr};
            vertices[3] = {x1, y0, 0.0f, rect.u1, rect.v0, abgr};
            std::memcpy(tib.data, indices, sizeof(indices));

            bgfx::TextureHandle texture = {rect.texture_handle};
            bgfx::setViewTransform(kUiView, kIdentityMatrix, kIdentityMatrix);
            bgfx::setTransform(kIdentityMatrix);
            bgfx::setTexture(0, texture_sampler_, texture);
            bgfx::setVertexBuffer(0, &tvb);
            bgfx::setIndexBuffer(&tib);
            uint64_t state =
                    BGFX_STATE_WRITE_RGB |
                    BGFX_STATE_WRITE_A |
                    BGFX_STATE_MSAA |
                    BGFX_STATE_BLEND_ALPHA;
            bgfx::setState(state);
            bgfx::submit(kUiView, textured_rect_program_);
            ++submitted_primitives_;
        }
        textured_rect_queue_.clear();
    }

    ANativeWindow* window_ = nullptr;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    std::atomic<uint32_t> requested_bottom_inset_{0};
    uint32_t applied_bottom_inset_ = 0;
    uint64_t frame_count_ = 0;
    uint64_t submitted_primitives_ = 0;
    bool ready_ = false;
    bgfx::VertexLayout rect_layout_;
    bgfx::VertexLayout textured_rect_layout_;
    bgfx::VertexLayout terrain_layout_;
    bgfx::ProgramHandle rect_program_ = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle textured_rect_program_ = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle alpha_test_program_ = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle alpha_masked_shadow_program_ =
            BGFX_INVALID_HANDLE;
    bgfx::UniformHandle rect_uniform_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle texture_sampler_ = BGFX_INVALID_HANDLE;
    bgfx::VertexBufferHandle terrain_vertex_buffer_ = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle terrain_index_buffer_ = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle terrain_line_index_buffer_ = BGFX_INVALID_HANDLE;
    std::vector<bgfx::VertexBufferHandle>
            terrain_layer_vertex_buffers_;
    std::vector<bgfx::IndexBufferHandle>
            terrain_layer_index_buffers_;
    bgfx::VertexBufferHandle world_object_vertex_buffer_ =
            BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle world_object_index_buffer_ =
            BGFX_INVALID_HANDLE;
    std::vector<bgfx::IndexBufferHandle>
            world_object_layer_index_buffers_;
    bgfx::TextureHandle white_texture_ = BGFX_INVALID_HANDLE;
    TerrainMesh terrain_mesh_;
    WorldObjectMesh world_object_mesh_;
    TerrainCamera terrain_camera_;
    std::vector<SolidRect> rect_queue_;
    std::vector<TexturedRect> textured_rect_queue_;
    std::string renderer_name_;
    std::string last_error_;
};

}  // namespace

IRenderBackend& RenderBackend() {
    static BgfxRenderBackend backend;
    return backend;
}

std::string RenderBackendDiagnosticReport() {
    const IRenderBackend& backend = RenderBackend();
    std::ostringstream report;
    report << "render_backend="
           << (backend.is_ready() ? "ready" : "not_ready")
           << "; renderer=" << backend.renderer_name()
           << "; width=" << backend.width()
           << "; height=" << backend.height()
           << "; content_height=" << backend.content_height()
           << "; bottom_inset="
           << backend.height() - backend.content_height()
           << "; frames=" << backend.frame_count()
           << "; primitives=" << backend.submitted_primitives();
    if (!backend.last_error().empty()) {
        report << "; render_error=" << backend.last_error();
    }
    return report.str();
}

}  // namespace bk2::android
