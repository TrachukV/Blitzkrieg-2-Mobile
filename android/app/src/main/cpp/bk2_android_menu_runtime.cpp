#include "bk2_android_menu_runtime.h"

#include "bk2_android_audio_backend.h"
#include "bk2_android_audio_decode.h"
#include "bk2_android_legacy_game_runtime.h"
#include "bk2_android_mission_runtime.h"
#include "bk2_android_vorbis_stream.h"
#include "bk2_android_platform.h"
#include "bk2_legacy_texture_probe.h"
#include "bk2_port_paths.h"
#include "bk2_render_backend.h"

#include "UI/stdafx.h"
#include "UI/DBUserInterface.h"
#include "UISpecificB2/DBUISpecificB2.h"
#include "3Dmotor/DBScene.h"
#include "3Dmotor/GfxBuffers.h"
// The engine modules alias the saver through their own Specific.h.
#ifndef CStructureSaver
#define CStructureSaver IBinSaver
#endif
#include "3Dmotor/FontFormat.h"
#include "3Dmotor/GTexture.h"
#include "GameX/GetConsts.h"
#include "Misc/StrProc.h"
#include "GameX/DBGameRoot.h"
#include "GameX/DBScenario.h"
#include "Stats_B2_M1/DBMapInfo.h"
#include "Stats_B2_M1/RPGStats.h"
#include "Stats_B2_M1/Vis2AI.h"
#include "GameX/dbgameoptions.h"
#include "System/GlobalVars.h"
#include "Sound/DBMusicSystem.h"
#include "Sound/DBSound.h"
#include "Sound/DBSoundDesc.h"
#include "System/GResource.h"
#include "System/VFSOperations.h"
#include "libdb/Database.h"
#include "libdb/Db.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <jni.h>
#include <map>
#include <mutex>
#include <cstdlib>
#include <memory>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>
#include <vector>

namespace bk2::android {
namespace {

// The shipped menu screens are authored against the original 1024x768
// interface surface; every placement value below is in that space.
constexpr float kVirtualScreenWidth = 1024.0f;
constexpr float kVirtualScreenHeight = 768.0f;
constexpr int kMaxMenuWindowDepth = 24;
constexpr size_t kMaxMenuWindowNodes = 512;
constexpr int kMaxCaptionCharacters = 120;
constexpr int kMaxBriefingCharacters = 4096;

// One submitted rectangle in the 1024x768 virtual screen space, with texture
// coordinates already normalized out of the descriptor's pixel maps.
struct MenuQuad {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    float u0 = 0.0f;
    float v0 = 0.0f;
    float u1 = 1.0f;
    float v1 = 1.0f;
    uint32_t argb = 0xffffffffu;
    int texture = -1;
};

// Four-corner equivalent of CRectLayout's UI quad. Most menu art remains
// axis-aligned MenuQuad geometry; chapter-map arrows need the original
// rotated segment vertices.
struct MenuTexturedQuad {
    float x0 = 0.0f;
    float y0 = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;
    float x2 = 0.0f;
    float y2 = 0.0f;
    float x3 = 0.0f;
    float y3 = 0.0f;
    float u0 = 0.0f;
    float v0 = 0.0f;
    float u1 = 1.0f;
    float v1 = 1.0f;
    uint32_t argb = 0xffffffffu;
    int texture = -1;
    NGfx::CTexture* direct_texture = nullptr;
};

std::vector<MenuQuad> g_quads;
std::vector<MenuTexturedQuad> g_textured_quads;
size_t g_textured_quad_insert_index = static_cast<size_t>(-1);
// A modal dialog is one branch of the authored screen graph, but the chapter
// map appends its generated frontline, route arrows, and mission markers
// after the whole tree has been walked. The modal's quad range is remembered
// so it can be lifted back over that dynamic content, the way the desktop
// draws a modal over the screen beneath it.
std::string g_modal_quad_path;
size_t g_modal_quad_begin = 0;
size_t g_modal_quad_end = 0;
CObj<NGfx::CTexture> g_chapter_potential_texture;
std::vector<MenuQuad> g_pressed_quads;
int g_pressed_button = -1;
std::vector<std::string> g_texture_paths;
std::vector<uint16_t> g_texture_handles;
std::map<std::string, CObj<NGfx::CTexture>> g_menu_textures;
bool g_render_logged = false;
bool g_menu_options_restored = false;
std::string g_menu_background_path;
uint16_t g_menu_background_texture = UINT16_MAX;
float g_menu_background_u = 1.0f;
float g_menu_background_v = 1.0f;
std::vector<MenuWindowNode> g_nodes;
std::string g_screen_ref;
std::string g_error;
size_t g_button_count = 0;
size_t g_texture_count = 0;
size_t g_caption_count = 0;
bool g_ready = false;
std::mutex g_menu_mutex;
std::map<std::string, std::string> g_text_cache;
// Visibility overrides applied by the menu's own navigation reactions.
std::map<std::string, bool> g_visibility_overrides;
// Runtime-built screens can contain duplicate child names in separate
// branches, so controller state also needs the full window path.
std::map<std::string, bool> g_path_visibility_overrides;
std::map<std::string, bool> g_path_enabled_overrides;
std::map<std::string, size_t> g_path_button_state_overrides;
std::map<std::string, std::string> g_caption_overrides;
// A window whose descriptor carries no name inherits its parent's path, so
// one path can resolve to several windows: the shipped composition dialog
// pairs each label with a thin rule that shares the label's name. The
// original binds text through one window handle, so a caption override stops
// at the first window that resolves to its path.
std::set<std::string> g_applied_caption_paths;
std::map<std::string, float> g_progress_overrides;
// IWindow::SetTexture equivalents: a controller swaps a window's art at
// runtime, keyed by the window name the original looks it up by.
std::map<std::string, const NDb::STexture*> g_texture_overrides;
std::map<std::string, const NDb::STexture*> g_path_texture_overrides;

// Template windows a screen's runtime content is cloned from, captured while
// the descriptor graph is walked.
struct MenuTemplate {
    const NDb::SWindow* window = nullptr;
    const NDb::SWindowShared* shared = nullptr;
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

std::map<std::string, MenuTemplate> g_templates;
int g_options_category = 0;
std::unique_ptr<AndroidVorbisStream> g_menu_music;
std::string g_menu_music_path;
std::string g_menu_music_error;
int g_menu_music_channel = -1;
DecodedPcmClip g_click_clip;
std::string g_click_sound_path;
std::string g_click_sound_error;
const char* const kMenuPanels[] = {"MainMenu", "SinglePlayerMenu"};

std::string ToStdString(const string& value) {
    return std::string(value.c_str());
}

std::string NormalizeResourcePath(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    while (!path.empty() && path.front() == '/') {
        path.erase(path.begin());
    }
    return path;
}

// Mirrors NText::GetText() for the UTF-16 text files the menu descriptors
// reference, converted to UTF-8 for the Android renderer.
std::string DecodeUtf16Text(
        const std::string& file_ref,
        int max_characters) {
    if (file_ref.empty() || NVFS::GetMainVFS() == nullptr) {
        return std::string();
    }
    CFileStream stream(NVFS::GetMainVFS(), string(file_ref.c_str()));
    std::string text;
    const int byte_count = stream.GetSize();
    if (!stream.IsOk() || byte_count < 2) {
        return text;
    }
    const unsigned char* bytes = stream.GetBuffer();
    if (bytes == nullptr) {
        return text;
    }

    bool little_endian = true;
    int offset = 0;
    if (bytes[0] == 0xff && bytes[1] == 0xfe) {
        offset = 2;
    } else if (bytes[0] == 0xfe && bytes[1] == 0xff) {
        little_endian = false;
        offset = 2;
    }

    int character_count = 0;
    while (offset + 1 < byte_count &&
           character_count < max_characters) {
        const std::uint16_t first = little_endian
                ? static_cast<std::uint16_t>(
                          bytes[offset] | (bytes[offset + 1] << 8))
                : static_cast<std::uint16_t>(
                          (bytes[offset] << 8) | bytes[offset + 1]);
        offset += 2;

        std::uint32_t code_point = first;
        if (first >= 0xd800 && first <= 0xdbff && offset + 1 < byte_count) {
            const std::uint16_t second = little_endian
                    ? static_cast<std::uint16_t>(
                              bytes[offset] | (bytes[offset + 1] << 8))
                    : static_cast<std::uint16_t>(
                              (bytes[offset] << 8) | bytes[offset + 1]);
            if (second >= 0xdc00 && second <= 0xdfff) {
                code_point = 0x10000u +
                        ((static_cast<std::uint32_t>(first - 0xd800) << 10) |
                         static_cast<std::uint32_t>(second - 0xdc00));
                offset += 2;
            }
        }
        if (code_point == 0) {
            break;
        }
        ++character_count;
        if (code_point < 0x80) {
            text.push_back(static_cast<char>(code_point));
        } else if (code_point < 0x800) {
            text.push_back(static_cast<char>(0xc0 | (code_point >> 6)));
            text.push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
        } else if (code_point < 0x10000) {
            text.push_back(static_cast<char>(0xe0 | (code_point >> 12)));
            text.push_back(
                    static_cast<char>(0x80 | ((code_point >> 6) & 0x3f)));
            text.push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
        } else {
            text.push_back(static_cast<char>(0xf0 | (code_point >> 18)));
            text.push_back(
                    static_cast<char>(0x80 | ((code_point >> 12) & 0x3f)));
            text.push_back(
                    static_cast<char>(0x80 | ((code_point >> 6) & 0x3f)));
            text.push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
        }
    }
    return text;
}

std::string LoadUtf16Text(const std::string& file_ref) {
    const std::map<std::string, std::string>::const_iterator cached =
            g_text_cache.find(file_ref);
    if (cached != g_text_cache.end()) {
        return cached->second;
    }
    const std::string text =
            DecodeUtf16Text(file_ref, kMaxCaptionCharacters);
    g_text_cache[file_ref] = text;
    return text;
}

std::string LoadUtf16BriefingText(const std::string& file_ref) {
    return DecodeUtf16Text(file_ref, kMaxBriefingCharacters);
}

// UI/Tools.cpp ApplyWindowAlign, reproduced so Android lays the shipped
// screens out exactly like the desktop client does.
void ApplyWindowAlign(
        NDb::EPositionAllign align,
        float parent_position,
        float parent_size,
        float child_position,
        float lower_margin,
        float upper_margin,
        float* size,
        float* screen_position) {
    switch (align) {
        case NDb::ERA_CENTER:
            *screen_position = parent_position +
                    static_cast<float>(static_cast<int>(
                            (parent_size - static_cast<int>(*size)) / 2)) +
                    child_position;
            break;
        case NDb::EPA_HIGH_END:
            *screen_position =
                    parent_position + parent_size - *size + child_position;
            break;
        case NDb::EPA_MARGIN:
            *screen_position = parent_position + lower_margin;
            *size = parent_size - lower_margin - upper_margin;
            break;
        case NDb::EPA_LOW_END:
        default:
            *screen_position = parent_position + child_position;
            break;
    }
}

// CWindow::InitByDesc merges every unset instance placement field with the
// shared descriptor before the window is positioned.
NDb::SWindowPlacement MergePlacement(
        const NDb::SWindowPlacement& instance,
        const NDb::SWindowShared* shared) {
    NDb::SWindowPlacement placement = instance;
    if (shared == nullptr) {
        return placement;
    }
    placement.position.Merge(shared->placement.position.Get());
    placement.lowerMargin.Merge(shared->placement.lowerMargin.Get());
    placement.upperMargin.Merge(shared->placement.upperMargin.Get());
    placement.size.Merge(shared->placement.size.Get());
    placement.verAllign.Merge(shared->placement.verAllign.Get());
    placement.horAllign.Merge(shared->placement.horAllign.Get());
    return placement;
}

std::string TexturePath(const NDb::STexture* texture) {
    if (texture == nullptr) {
        return std::string();
    }
    std::string path = NormalizeResourcePath(
            ToStdString(texture->szDestName));
    if (path.empty() || path.find('/') != std::string::npos) {
        return path;
    }
    std::string folder = NormalizeResourcePath(
            ToStdString(NDb::GetFolderName(texture->GetDBID())));
    while (!folder.empty() && folder.back() == '/') {
        folder.pop_back();
    }
    return folder.empty() ? path : folder + "/" + path;
}

std::string BackgroundTexturePath(const NDb::SBackground* background) {
    if (background == nullptr || !background->pTexture) {
        return std::string();
    }
    const NDb::STexture* texture = background->pTexture.GetPtr();
    if (texture == nullptr) {
        return std::string();
    }
    std::string path = NormalizeResourcePath(
            ToStdString(texture->szDestName));
    if (path.empty()) {
        return path;
    }
    if (path.find('/') == std::string::npos) {
        std::string folder = NormalizeResourcePath(
                ToStdString(NDb::GetFolderName(background->GetDBID())));
        while (!folder.empty() && folder.back() == '/') {
            folder.pop_back();
        }
        if (!folder.empty()) {
            path = folder + "/" + path;
        }
    }
    return path;
}

// Same immediate CFileTexture path the world renderer uses, so shipped menu
// DDS/TGA art reaches bgfx without the legacy resource loader threads.
uint16_t MenuTextureHandle(const std::string& texture_path) {
    if (texture_path.empty()) {
        return UINT16_MAX;
    }
    auto cached = g_menu_textures.find(texture_path);
    if (cached == g_menu_textures.end()) {
        CPtr<NDb::STexture> texture_desc = new NDb::STexture();
        NDb::CResourceHelper::SetDBID(
                texture_desc.GetPtr(),
                CDBID("Android/OriginalMenuTexture.xdb"));
        NDb::CResourceHelper::SetLoaded(texture_desc.GetPtr());
        texture_desc->szDestName = texture_path.c_str();
        texture_desc->eType = NDb::STexture::REGULAR;
        texture_desc->eAddrType = NDb::STexture::CLAMP;
        texture_desc->bInstantLoad = true;
        CObj<NGScene::CFileTexture> texture_node =
                new NGScene::CFileTexture();
        GUID uid;
        Zero(uid);
        texture_node->SetKey(NGScene::GetKey(texture_desc.GetPtr()), uid);
        CDGPtr<NGScene::CFileTexture> texture_ref(texture_node.GetPtr());
        texture_ref.Refresh();
        CObj<NGfx::CTexture> texture = texture_ref->GetValue();
        cached = g_menu_textures.emplace(texture_path, texture).first;
    }
    if (!IsValid(cached->second)) {
        return UINT16_MAX;
    }
    EnsureLegacyTextureMipChainUploaded(cached->second);
    return LegacyTextureHandleIndex(cached->second);
}

// One shipped bitmap font: its glyph metrics cache from Data/bin/fonts and the
// atlas texture its STexture descriptor points at.
struct MenuFont {
    CObj<CFontFormatInfo> metrics;
    int texture = -1;
    float texture_width = 1.0f;
    float texture_height = 1.0f;
    int height = 0;
};

std::map<std::string, MenuFont> g_fonts;

// The shipped font resources live at fixed paths and name themselves through
// SFont::szName, which is what the "<font face=...>" markup selects.
const char* const kMenuFontPaths[] = {
        "Fonts/Body/Font.xdb",
        "Fonts/Header1/Font.xdb",
        "Fonts/Header2/Font.xdb",
        "Fonts/Numeric/Font.xdb",
};

int TextureIndex(const std::string& path) {
    if (path.empty()) {
        return -1;
    }
    for (size_t index = 0; index < g_texture_paths.size(); ++index) {
        if (g_texture_paths[index] == path) {
            return static_cast<int>(index);
        }
    }
    g_texture_paths.push_back(path);
    return static_cast<int>(g_texture_paths.size() - 1);
}

// UI/Tools.cpp ApplyTextureAllign; the maps it produces are texture pixels.
void ApplyTextureAlign(
        NDb::EPositionAllign align,
        float width,
        float texture_width,
        float* map1,
        float* map2,
        float* position) {
    switch (align) {
        case NDb::EPA_HIGH_END:
            if (width < texture_width) {
                *map1 = texture_width - width;
                *map2 = texture_width;
            } else {
                *position += width - texture_width;
                *map1 = 0.0f;
                *map2 = texture_width;
            }
            break;
        case NDb::EPA_LOW_END:
        default:
            *map1 = 0.0f;
            *map2 = std::min(width, texture_width);
            break;
    }
}

// CBackgroundSimpleTexture::Visit draws one rect clipped to the texture size.
void AppendSimpleTextureQuads(
        const NDb::SBackgroundSimpleTexture* background,
        float x,
        float y,
        float width,
        float height,
        int texture,
        float texture_width,
        float texture_height) {
    if (background == nullptr ||
        width <= 0.0f ||
        height <= 0.0f ||
        texture_width <= 0.0f ||
        texture_height <= 0.0f) {
        return;
    }
    MenuQuad quad;
    quad.x = x;
    quad.y = y;
    quad.width = std::min(width, texture_width);
    quad.height = std::min(height, texture_height);
    quad.texture = texture;
    quad.argb = static_cast<uint32_t>(background->nColor);
    float map_x1 = 0.0f;
    float map_x2 = 0.0f;
    float map_y1 = 0.0f;
    float map_y2 = 0.0f;
    ApplyTextureAlign(
            background->eTextureX,
            width,
            texture_width,
            &map_x1,
            &map_x2,
            &quad.x);
    ApplyTextureAlign(
            background->eTextureY,
            height,
            texture_height,
            &map_y1,
            &map_y2,
            &quad.y);
    quad.u0 = map_x1 / texture_width;
    quad.u1 = map_x2 / texture_width;
    quad.v0 = map_y1 / texture_height;
    quad.v1 = map_y2 / texture_height;
    g_quads.push_back(quad);
}

// CBackgroundTiledTexture::DivideSubrects repeats one sub-rect over its
// destination band, clipping the final row/column's texture maps.
void DivideSubrect(
        const NDb::SSubRect& sub_rect,
        uint32_t argb,
        int texture,
        float texture_width,
        float texture_height) {
    if (sub_rect.ptSize.x <= 0.0f || sub_rect.ptSize.y <= 0.0f) {
        return;
    }
    const float map_width = sub_rect.rcMaps.x2 - sub_rect.rcMaps.x1;
    const float map_height = sub_rect.rcMaps.y2 - sub_rect.rcMaps.y1;
    for (float y = sub_rect.rcRect.y1;
         y < sub_rect.rcRect.y2;
         y += sub_rect.ptSize.y) {
        float size_y = sub_rect.ptSize.y;
        float map_y2 = sub_rect.rcMaps.y2;
        if (y + sub_rect.ptSize.y > sub_rect.rcRect.y2) {
            size_y = sub_rect.rcRect.y2 - y;
            map_y2 = sub_rect.rcMaps.y1 +
                    (size_y / sub_rect.ptSize.y) * map_height;
        }
        for (float x = sub_rect.rcRect.x1;
             x < sub_rect.rcRect.x2;
             x += sub_rect.ptSize.x) {
            float size_x = sub_rect.ptSize.x;
            float map_x2 = sub_rect.rcMaps.x2;
            if (x + sub_rect.ptSize.x > sub_rect.rcRect.x2) {
                size_x = sub_rect.rcRect.x2 - x;
                map_x2 = sub_rect.rcMaps.x1 +
                        (size_x / sub_rect.ptSize.x) * map_width;
            }
            MenuQuad quad;
            quad.x = x;
            quad.y = y;
            quad.width = size_x;
            quad.height = size_y;
            quad.u0 = sub_rect.rcMaps.x1 / texture_width;
            quad.u1 = map_x2 / texture_width;
            quad.v0 = sub_rect.rcMaps.y1 / texture_height;
            quad.v1 = map_y2 / texture_height;
            quad.argb = argb;
            quad.texture = texture;
            g_quads.push_back(quad);
        }
    }
}

// CBackgroundTiledTexture::InitBorderAndFill lays the nine bands out from the
// window rect and the corner sub-rect sizes.
void AppendTiledTextureQuads(
        const NDb::SBackgroundTiledTexture* background,
        float x,
        float y,
        float width,
        float height,
        int texture,
        float texture_width,
        float texture_height) {
    if (background == nullptr ||
        width <= 0.0f ||
        height <= 0.0f ||
        texture_width <= 0.0f ||
        texture_height <= 0.0f) {
        return;
    }
    const float left = x;
    const float top_edge = y;
    const float right = x + width;
    const float bottom = y + height;
    const float border_left = background->rL.ptSize.x;
    const float border_right = background->rR.ptSize.x;
    const float border_top = background->rT.ptSize.y;
    const float border_bottom = background->rB.ptSize.y;

    NDb::SSubRect left_top = background->rLT;
    NDb::SSubRect right_top = background->rRT;
    NDb::SSubRect left_bottom = background->rLB;
    NDb::SSubRect right_bottom = background->rRB;
    NDb::SSubRect top = background->rT;
    NDb::SSubRect bottom_band = background->rB;
    NDb::SSubRect left_band = background->rL;
    NDb::SSubRect right_band = background->rR;
    NDb::SSubRect fill = background->rF;
    left_top.rcRect.Set(left, top_edge, left + border_left, top_edge + border_top);
    right_top.rcRect.Set(right - border_right, top_edge, right, top_edge + border_top);
    left_bottom.rcRect.Set(left, bottom - border_bottom, left + border_left, bottom);
    right_bottom.rcRect.Set(
            right - border_right, bottom - border_bottom, right, bottom);
    top.rcRect.Set(
            left + border_left, top_edge, right - border_right, top_edge + border_top);
    bottom_band.rcRect.Set(
            left + border_left,
            bottom - border_bottom,
            right - border_right,
            bottom);
    left_band.rcRect.Set(
            left,
            top_edge + border_top,
            left + border_left,
            bottom - border_bottom);
    right_band.rcRect.Set(
            right - border_right,
            top_edge + border_top,
            right,
            bottom - border_bottom);
    fill.rcRect.Set(
            left + border_left,
            top_edge + border_top,
            right - border_right,
            bottom - border_bottom);

    const uint32_t argb = static_cast<uint32_t>(background->nColor);
    const NDb::SSubRect* order[] = {
            &left_top,   &top,         &right_top,
            &left_band,  &fill,        &right_band,
            &left_bottom, &bottom_band, &right_bottom};
    for (const NDb::SSubRect* sub_rect : order) {
        DivideSubrect(
                *sub_rect,
                argb,
                texture,
                texture_width,
                texture_height);
    }
}

void AppendBackgroundQuads(
        const NDb::SBackground* background,
        float x,
        float y,
        float width,
        float height,
        const NDb::STexture* texture_override = nullptr) {
    if (background == nullptr) {
        return;
    }
    const NDb::STexture* texture_desc = texture_override != nullptr
            ? texture_override
            : background->pTexture.GetPtr();
    if (texture_desc == nullptr) {
        return;
    }
    const int texture = texture_override != nullptr
            ? TextureIndex(TexturePath(texture_override))
            : TextureIndex(BackgroundTexturePath(background));
    if (texture < 0) {
        return;
    }
    const float texture_width =
            static_cast<float>(std::max(texture_desc->nWidth, 1));
    const float texture_height =
            static_cast<float>(std::max(texture_desc->nHeight, 1));
    switch (background->GetTypeID()) {
        case NDb::SBackgroundTiledTexture::typeID:
            AppendTiledTextureQuads(
                    static_cast<const NDb::SBackgroundTiledTexture*>(
                            background),
                    x,
                    y,
                    width,
                    height,
                    texture,
                    texture_width,
                    texture_height);
            break;
        case NDb::SBackgroundSimpleTexture::typeID:
            AppendSimpleTextureQuads(
                    static_cast<const NDb::SBackgroundSimpleTexture*>(
                            background),
                    x,
                    y,
                    width,
                    height,
                    texture,
                    texture_width,
                    texture_height);
            break;
        case NDb::SBackgroundSimpleScallingTexture::typeID: {
            // CBackgroundSimpleScallingTexture stretches a texel-space source
            // rect over the window. vSize is that rect: zero means the whole
            // texture, and a value past the texture size repeats it, which is
            // how the shipped grids are drawn from one small marker.
            const NDb::SBackgroundSimpleScallingTexture* scaling =
                    static_cast<
                            const NDb::SBackgroundSimpleScallingTexture*>(
                            background);
            MenuQuad quad;
            quad.x = x;
            quad.y = y;
            quad.width = width;
            quad.height = height;
            quad.argb = static_cast<uint32_t>(background->nColor);
            quad.texture = texture;
            quad.u1 = scaling->vSize.x == 0.0f
                    ? 1.0f
                    : scaling->vSize.x / texture_width;
            quad.v1 = scaling->vSize.y == 0.0f
                    ? 1.0f
                    : scaling->vSize.y / texture_height;
            g_quads.push_back(quad);
            break;
        }
        default: {
            // Any other variant stretches the whole texture over the rect.
            MenuQuad quad;
            quad.x = x;
            quad.y = y;
            quad.width = width;
            quad.height = height;
            quad.argb = static_cast<uint32_t>(background->nColor);
            quad.texture = texture;
            g_quads.push_back(quad);
            break;
        }
    }
}

// SUIGameConsts::buttonClickSound points at the UISPlaySound command whose
// complex sound descriptor names the shipped menu click wav.
std::string ResolveButtonClickSoundPath() {
    const NDb::SUIConstsB2* consts = NGameX::GetUIConsts();
    if (consts == nullptr || consts->buttonClickSound.commands.empty()) {
        return std::string();
    }
    const NDb::SUIStateBase* command =
            consts->buttonClickSound.commands[0].GetPtr();
    const NDb::SUISPlaySound* play_sound =
            dynamic_cast<const NDb::SUISPlaySound*>(command);
    if (play_sound == nullptr || !play_sound->pSoundToPlay.Get()) {
        return std::string();
    }
    const NDb::SComplexSoundDesc* complex =
            play_sound->pSoundToPlay.Get().GetPtr();
    if (complex == nullptr || complex->sounds.empty()) {
        return std::string();
    }
    const NDb::SSoundDesc* sound = complex->sounds[0].pPathName.GetPtr();
    if (sound == nullptr) {
        return std::string();
    }
    return NormalizeResourcePath(ToStdString(sound->szSoundPath));
}

void LoadButtonClickSound() {
    g_click_clip = DecodedPcmClip();
    g_click_sound_error.clear();
    g_click_sound_path = ResolveButtonClickSoundPath();
    if (g_click_sound_path.empty() || NVFS::GetMainVFS() == nullptr) {
        return;
    }
    CFileStream stream(
            NVFS::GetMainVFS(), string(g_click_sound_path.c_str()));
    const int byte_count = stream.GetSize();
    if (!stream.IsOk() || byte_count <= 0 ||
        stream.GetBuffer() == nullptr) {
        g_click_sound_error = "unreadable";
        return;
    }
    DecodeWavToPcm16(
            stream.GetBuffer(),
            static_cast<size_t>(byte_count),
            &g_click_clip,
            &g_click_sound_error);
}

// GameRoot -> MapMusic -> PlayList -> Composition -> MusicTrack names the
// shipped ogg the desktop menu streams.
std::string ResolveMenuMusicPath() {
    const NDb::SGameRoot* root = NGameX::GetGameRoot();
    if (root == nullptr || !root->pMainMenuMusic) {
        return std::string();
    }
    const NDb::SMapMusic* music = root->pMainMenuMusic.GetPtr();
    if (music == nullptr || music->playLists.empty() ||
        !music->playLists[0]) {
        return std::string();
    }
    const NDb::SPlayList* play_list = music->playLists[0].GetPtr();
    if (play_list == nullptr || play_list->stillOrder.empty() ||
        !play_list->stillOrder[0]) {
        return std::string();
    }
    const NDb::SComposition* composition =
            play_list->stillOrder[0].GetPtr();
    if (composition == nullptr || !composition->pTrack) {
        return std::string();
    }
    const NDb::SMusicTrack* track = composition->pTrack.GetPtr();
    if (track == nullptr) {
        return std::string();
    }
    return NormalizeResourcePath(ToStdString(track->szMusicFileName));
}

void StartMenuMusic() {
    g_menu_music_error.clear();
    g_menu_music_path = ResolveMenuMusicPath();
    if (g_menu_music_path.empty()) {
        g_menu_music_error = "unresolved";
        return;
    }
    // Open resolves the data-relative path itself; pre-resolving here would
    // hand it an absolute path it would prefix a second time.
    g_menu_music = AndroidVorbisStream::Open(
            g_menu_music_path, 0, 4, &g_menu_music_error);
    if (!g_menu_music) {
        return;
    }
    g_menu_music_channel =
            AudioBackend().play_stream(g_menu_music->pcm_source());
}

void StopMenuMusic() {
    if (g_menu_music_channel >= 0) {
        AudioBackend().stop(g_menu_music_channel);
        g_menu_music_channel = -1;
    }
    if (g_menu_music) {
        g_menu_music->stop();
        g_menu_music.reset();
    }
}

void LoadMenuFonts() {
    g_fonts.clear();
    for (const char* path : kMenuFontPaths) {
        const NDb::SFont* font_desc = NDb::Get<NDb::SFont>(CDBID(path));
        if (font_desc == nullptr || font_desc->szName.empty()) {
            continue;
        }
        // CFileFont::Recalc opens the cache under the "Fonts" resource root by
        // the descriptor uid; the metrics carry glyph rects and advances.
        NGScene::CResourceOpener file(
                "Fonts", NGScene::SResKey<int>(font_desc->uid, 0));
        if (!file.IsOk()) {
            continue;
        }
        MenuFont font;
        file->Add(1, &font.metrics);
        if (!IsValid(font.metrics) || font.metrics->GetHeight() <= 0) {
            continue;
        }
        font.height = font.metrics->GetHeight();
        const NDb::STexture* atlas = font_desc->pTexture
                ? font_desc->pTexture.GetPtr()
                : nullptr;
        if (atlas == nullptr) {
            continue;
        }
        std::string atlas_path =
                NormalizeResourcePath(ToStdString(atlas->szDestName));
        if (atlas_path.find('/') == std::string::npos) {
            std::string folder = NormalizeResourcePath(
                    ToStdString(NDb::GetFolderName(font_desc->GetDBID())));
            while (!folder.empty() && folder.back() == '/') {
                folder.pop_back();
            }
            if (!folder.empty()) {
                atlas_path = folder + "/" + atlas_path;
            }
        }
        font.texture = TextureIndex(atlas_path);
        font.texture_width = static_cast<float>(std::max(atlas->nWidth, 1));
        font.texture_height = static_cast<float>(std::max(atlas->nHeight, 1));
        g_fonts[ToStdString(font_desc->szName)] = font;
    }
}

// SUIConstsB2::tags maps a markup name to a file whose contents replace
// "<val name>", so the button style expands to its font, color and alignment.
std::string ExpandMarkupTags(const std::string& source, int depth) {
    if (depth > 4 || source.find("<val ") == std::string::npos) {
        return source;
    }
    const NDb::SUIConstsB2* consts = NGameX::GetUIConsts();
    if (consts == nullptr) {
        return source;
    }
    std::string expanded;
    size_t cursor = 0;
    while (cursor < source.size()) {
        const size_t open = source.find("<val ", cursor);
        if (open == std::string::npos) {
            expanded.append(source, cursor, std::string::npos);
            break;
        }
        const size_t close = source.find('>', open);
        if (close == std::string::npos) {
            expanded.append(source, cursor, std::string::npos);
            break;
        }
        expanded.append(source, cursor, open - cursor);
        const std::string name =
                source.substr(open + 5, close - open - 5);
        for (const NDb::SMLTag& tag : consts->tags) {
            if (ToStdString(tag.szName) != name) {
                continue;
            }
            expanded += ExpandMarkupTags(
                    LoadUtf16Text(NormalizeResourcePath(
                            ToStdString(tag.szTextFileRef))),
                    depth + 1);
            break;
        }
        cursor = close + 1;
    }
    return expanded;
}

struct MenuTextStyle {
    std::string face;
    uint32_t argb = 0xffffffffu;
    bool centered = false;
};

MenuTextStyle ParseTextStyle(const std::string& tags) {
    MenuTextStyle style;
    const std::string markup = ExpandMarkupTags(tags, 0);
    const size_t face = markup.find("face=");
    if (face != std::string::npos) {
        size_t end = face + 5;
        while (end < markup.size() &&
               markup[end] != ' ' &&
               markup[end] != '>') {
            ++end;
        }
        style.face = markup.substr(face + 5, end - face - 5);
    }
    const size_t color = markup.find("<color=");
    if (color != std::string::npos) {
        const size_t end = markup.find('>', color);
        if (end != std::string::npos) {
            const std::string value =
                    markup.substr(color + 7, end - color - 7);
            style.argb = static_cast<uint32_t>(
                    std::strtoul(value.c_str(), nullptr, 16));
            if (value.size() <= 6) {
                style.argb |= 0xff000000u;
            }
        }
    }
    style.centered = markup.find("<center>") != std::string::npos;
    return style;
}

std::vector<uint16_t> DecodeUtf8Characters(const std::string& text) {
    std::vector<uint16_t> characters;
    for (size_t index = 0; index < text.size();) {
        const unsigned char lead = static_cast<unsigned char>(text[index]);
        uint32_t code_point = lead;
        size_t length = 1;
        if ((lead & 0xe0) == 0xc0) {
            code_point = lead & 0x1fu;
            length = 2;
        } else if ((lead & 0xf0) == 0xe0) {
            code_point = lead & 0x0fu;
            length = 3;
        } else if ((lead & 0xf8) == 0xf0) {
            code_point = lead & 0x07u;
            length = 4;
        }
        for (size_t extra = 1; extra < length && index + extra < text.size();
             ++extra) {
            code_point = (code_point << 6) |
                    (static_cast<unsigned char>(text[index + extra]) & 0x3fu);
        }
        index += length;
        if (code_point <= 0xffff) {
            characters.push_back(static_cast<uint16_t>(code_point));
        }
    }
    return characters;
}

float MeasureTextWidth(
        const std::string& text,
        const CFontFormatInfo& metrics) {
    float total = 0.0f;
    uint16_t previous = 0;
    for (uint16_t character : DecodeUtf8Characters(text)) {
        const STFCharacter& glyph = metrics.GetChar(character);
        total += static_cast<float>(glyph.nBC) +
                static_cast<float>(metrics.GetKern(character, previous));
        previous = character;
    }
    return total;
}

// Lays a caption out with the shipped glyph metrics: nA is the pre-space,
// nBC the advance to the next character, plus the kerning pair correction.
void AppendTextQuads(
        const std::string& text,
        const std::string& tags,
        float x,
        float y,
        float width,
        float height,
        const std::string& face_override = std::string(),
        uint32_t argb_override = 0) {
    if (text.empty()) {
        return;
    }
    MenuTextStyle style = ParseTextStyle(tags);
    if (!face_override.empty()) {
        style.face = face_override;
    }
    if (argb_override != 0) {
        style.argb = argb_override;
    }
    const std::map<std::string, MenuFont>::const_iterator found =
            g_fonts.find(style.face.empty() ? "body" : style.face);
    if (found == g_fonts.end() || !found->second.metrics) {
        return;
    }
    const MenuFont& font = found->second;
    const CFontFormatInfo& metrics = *font.metrics;

    // The captions are UTF-8 here; the shipped fonts are single-plane, so a
    // direct decode to UTF-16 code units matches the original lookup.
    const std::vector<uint16_t> characters = DecodeUtf8Characters(text);
    const float total = MeasureTextWidth(text, metrics);

    float pen_x = style.centered
            ? x + (width - total) * 0.5f
            : x;
    const float pen_y =
            y + (height - static_cast<float>(font.height)) * 0.5f;
    uint16_t previous = 0;
    for (uint16_t character : characters) {
        const STFCharacter& glyph = metrics.GetChar(character);
        pen_x += static_cast<float>(metrics.GetKern(character, previous));
        const float glyph_width =
                static_cast<float>(glyph.x2 - glyph.x1);
        const float glyph_height =
                static_cast<float>(glyph.y2 - glyph.y1);
        if (glyph_width > 0.0f && glyph_height > 0.0f) {
            MenuQuad quad;
            quad.x = pen_x + static_cast<float>(glyph.nA);
            quad.y = pen_y;
            quad.width = glyph_width;
            quad.height = glyph_height;
            quad.u0 = static_cast<float>(glyph.x1) / font.texture_width;
            quad.u1 = static_cast<float>(glyph.x2) / font.texture_width;
            quad.v0 = static_cast<float>(glyph.y1) / font.texture_height;
            quad.v1 = static_cast<float>(glyph.y2) / font.texture_height;
            quad.argb = style.argb;
            quad.texture = font.texture;
            g_quads.push_back(quad);
        }
        pen_x += static_cast<float>(glyph.nBC);
        previous = character;
    }
}

void AppendWrappedTextQuads(
        const std::string& text,
        const std::string& tags,
        float x,
        float y,
        float width,
        float height,
        const std::string& face_override = std::string(),
        uint32_t argb_override = 0) {
    if (text.empty() || width <= 0.0f || height <= 0.0f) {
        return;
    }
    MenuTextStyle style = ParseTextStyle(tags);
    if (!face_override.empty()) {
        style.face = face_override;
    }
    const std::map<std::string, MenuFont>::const_iterator found =
            g_fonts.find(style.face.empty() ? "body" : style.face);
    if (found == g_fonts.end() || !found->second.metrics) {
        return;
    }
    const CFontFormatInfo& metrics = *found->second.metrics;
    const float line_height =
            static_cast<float>(found->second.height + 3);
    const int max_lines = std::max(
            1, static_cast<int>(height / line_height));

    std::vector<std::string> lines;
    size_t paragraph_begin = 0;
    while (paragraph_begin <= text.size()) {
        const size_t paragraph_end = text.find('\n', paragraph_begin);
        std::string paragraph = text.substr(
                paragraph_begin,
                paragraph_end == std::string::npos
                        ? std::string::npos
                        : paragraph_end - paragraph_begin);
        if (!paragraph.empty() && paragraph.back() == '\r') {
            paragraph.pop_back();
        }
        std::istringstream words(paragraph);
        std::string line;
        std::string word;
        while (words >> word) {
            const std::string candidate =
                    line.empty() ? word : line + " " + word;
            if (line.empty() ||
                MeasureTextWidth(candidate, metrics) <= width) {
                line = candidate;
            } else {
                lines.push_back(line);
                line = word;
            }
        }
        if (!line.empty()) {
            lines.push_back(line);
        } else if (paragraph.empty()) {
            lines.emplace_back();
        }
        if (paragraph_end == std::string::npos) {
            break;
        }
        paragraph_begin = paragraph_end + 1;
    }

    const int count = std::min(
            max_lines, static_cast<int>(lines.size()));
    for (int index = 0; index < count; ++index) {
        AppendTextQuads(
                lines[static_cast<size_t>(index)],
                tags,
                x,
                y + static_cast<float>(index) * line_height,
                width,
                line_height,
                face_override,
                argb_override);
    }
}

const NDb::SWindowShared* SharedOf(const NDb::SWindow* window) {
    if (window == nullptr || !window->pShared) {
        return nullptr;
    }
    return dynamic_cast<const NDb::SWindowShared*>(window->pShared.GetPtr());
}

std::string WindowTypeName(int type_id) {
    switch (type_id) {
        case NDb::SWindowScreen::typeID:
            return "WindowScreen";
        case NDb::SWindowSimple::typeID:
            return "WindowSimple";
        case NDb::SWindowMSButton::typeID:
            return "WindowMSButton";
        case NDb::SWindowTextView::typeID:
            return "WindowTextView";
        case NDb::SWindowMiniMap::typeID:
            return "WindowMiniMap";
        default:
            return "Window";
    }
}

// The shipped caption files are authored for the original markup text engine
// and carry style tags such as "<val button>" inline. Split them out so the
// Android view gets the visible characters and the requested style separately.
void SplitMarkupTags(
        const std::string& source,
        std::string* text,
        std::string* tags) {
    size_t cursor = 0;
    while (cursor < source.size()) {
        const size_t open = source.find('<', cursor);
        if (open == std::string::npos) {
            text->append(source, cursor, std::string::npos);
            return;
        }
        const size_t close = source.find('>', open);
        if (close == std::string::npos) {
            text->append(source, cursor, std::string::npos);
            return;
        }
        text->append(source, cursor, open - cursor);
        if (!tags->empty()) {
            tags->push_back(' ');
        }
        tags->append(source, open, close - open + 1);
        cursor = close + 1;
    }
}

// SWindow::pTextString points at a ForegroundTextString whose instance and
// shared file refs hold the shipped UTF-16 caption. CWindow::GetDBText()
// concatenates the format string in front of the caption; the format part is
// markup for the original text engine, so it is kept out of the visible text.
void ResolveWindowText(
        const NDb::SWindow* window,
        std::string* caption,
        std::string* text_format) {
    if (window == nullptr || !window->pTextString) {
        return;
    }
    const NDb::SForegroundTextString* text_string =
            window->pTextString.GetPtr();
    if (text_string == nullptr) {
        return;
    }
    if (text_string->pShared) {
        const NDb::SForegroundTextStringShared* shared =
                text_string->pShared.GetPtr();
        if (shared != nullptr && !shared->szFormatStringFileRef.empty()) {
            SplitMarkupTags(
                    LoadUtf16Text(NormalizeResourcePath(
                            ToStdString(shared->szFormatStringFileRef))),
                    caption,
                    text_format);
        }
    }
    if (!text_string->szTextStringFileRef.empty()) {
        SplitMarkupTags(
                LoadUtf16Text(NormalizeResourcePath(
                        ToStdString(text_string->szTextStringFileRef))),
                caption,
                text_format);
    }
}

// The first logical state's enter commands name the reaction that the
// desktop client runs when the button is released.
std::string ButtonAction(const NDb::SWindowMSButton* button) {
    if (button == nullptr || button->buttonStates.empty()) {
        return std::string();
    }
    const NDb::SUIStateSequence& sequence =
            button->buttonStates[0].commandsOnEnterState;
    if (sequence.commands.empty() || !sequence.commands[0]) {
        return std::string();
    }
    const NDb::SUIStateBase* command = sequence.commands[0].GetPtr();
    if (command == nullptr) {
        return std::string();
    }
    std::string reference =
            NormalizeResourcePath(ToStdString(command->GetDBID().ToString()));
    const size_t slash = reference.rfind('/');
    if (slash != std::string::npos) {
        reference.erase(0, slash + 1);
    }
    const size_t suffix = reference.rfind('_');
    if (suffix != std::string::npos) {
        reference.erase(suffix);
    }
    return reference;
}

void CollectWindow(
        const NDb::SWindow* window,
        float parent_x,
        float parent_y,
        float parent_width,
        float parent_height,
        int depth,
        bool parent_visible,
        const std::string& parent_path) {
    if (window == nullptr ||
        depth > kMaxMenuWindowDepth ||
        g_nodes.size() >= kMaxMenuWindowNodes) {
        return;
    }
    const NDb::SWindowShared* shared = SharedOf(window);
    NDb::SWindowPlacement placement =
            MergePlacement(window->placement, shared);

    float width = placement.size.Get().x;
    float height = placement.size.Get().y;
    float x = parent_x;
    float y = parent_y;
    ApplyWindowAlign(
            placement.horAllign.Get(),
            parent_x,
            parent_width,
            placement.position.Get().x,
            placement.lowerMargin.Get().x,
            placement.upperMargin.Get().x,
            &width,
            &x);
    ApplyWindowAlign(
            placement.verAllign.Get(),
            parent_y,
            parent_height,
            placement.position.Get().y,
            placement.lowerMargin.Get().y,
            placement.upperMargin.Get().y,
            &height,
            &y);

    MenuWindowNode node;
    node.name = ToStdString(window->szName);
    node.path = parent_path;
    if (!node.name.empty()) {
        if (!node.path.empty()) {
            node.path += "/";
        }
        node.path += node.name;
    }
    // Closes over every exit from this call, including the child walk, so a
    // modal branch reports the exact quad range it produced.
    struct ModalQuadRangeScope {
        bool active = false;
        explicit ModalQuadRangeScope(const std::string& path) {
            if (!g_modal_quad_path.empty() && path == g_modal_quad_path) {
                active = true;
                g_modal_quad_begin = g_quads.size();
            }
        }
        ~ModalQuadRangeScope() {
            if (active) {
                g_modal_quad_end = g_quads.size();
            }
        }
    } modal_quad_scope(node.path);
    node.type = WindowTypeName(window->GetTypeID());
    node.x = x;
    node.y = y;
    node.width = width;
    node.height = height;
    // CWindow::Reposition defers an invisible window and nothing under it is
    // drawn, so a hidden branch contributes no geometry.
    bool window_visible = window->bVisible;
    const std::map<std::string, bool>::const_iterator override =
            g_visibility_overrides.find(ToStdString(window->szName));
    if (override != g_visibility_overrides.end()) {
        window_visible = override->second;
    }
    const std::map<std::string, bool>::const_iterator path_override =
            g_path_visibility_overrides.find(node.path);
    if (path_override != g_path_visibility_overrides.end()) {
        window_visible = path_override->second;
    }
    const bool visible = parent_visible && window_visible;
    node.visible = visible;
    node.enabled = window->bEnabled;
    const std::map<std::string, bool>::const_iterator enabled_override =
            g_path_enabled_overrides.find(node.path);
    if (enabled_override != g_path_enabled_overrides.end()) {
        node.enabled = enabled_override->second;
    }
    node.depth = depth;
    ResolveWindowText(window, &node.caption, &node.text_format);
    if (shared != nullptr) {
        node.background_texture =
                BackgroundTexturePath(shared->pBackground.GetPtr());
        node.foreground_texture =
                BackgroundTexturePath(shared->pForeground.GetPtr());
    }

    if (window->GetTypeID() == NDb::SWindowTextView::typeID) {
        // Text views carry their own file ref plus a shared font name and
        // colour rather than a ForegroundTextString.
        const NDb::SWindowTextView* text_view =
                static_cast<const NDb::SWindowTextView*>(window);
        if (node.caption.empty() && !text_view->szTextFileRef.empty()) {
            SplitMarkupTags(
                    LoadUtf16Text(NormalizeResourcePath(
                            ToStdString(text_view->szTextFileRef))),
                    &node.caption,
                    &node.text_format);
        }
        const NDb::SWindowTextViewShared* text_shared =
                dynamic_cast<const NDb::SWindowTextViewShared*>(shared);
        if (text_shared != nullptr) {
            node.text_face = ToStdString(text_shared->szFontName);
            node.text_argb = static_cast<uint32_t>(text_shared->nColor);
        }
    }

    if (window->GetTypeID() == NDb::SWindowMSButton::typeID) {
        const NDb::SWindowMSButton* button =
                static_cast<const NDb::SWindowMSButton*>(window);
        node.button = true;
        node.action = ButtonAction(button);
        if (node.caption.empty() && !button->szTextFileRef.empty()) {
            SplitMarkupTags(
                    LoadUtf16Text(NormalizeResourcePath(
                            ToStdString(button->szTextFileRef))),
                    &node.caption,
                    &node.text_format);
        }
        const NDb::SWindowMSButtonShared* button_shared =
                dynamic_cast<const NDb::SWindowMSButtonShared*>(shared);
        if (button_shared != nullptr &&
            !button_shared->visualStates.empty()) {
            const NDb::SButtonVisualState& state =
                    button_shared->visualStates[0];
            const std::string normal =
                    BackgroundTexturePath(state.normal.pBackground.GetPtr());
            if (!normal.empty()) {
                node.background_texture = normal;
            }
            node.pushed_texture =
                    BackgroundTexturePath(state.pushed.pBackground.GetPtr());
        }
    }

    const std::map<std::string, std::string>::const_iterator caption_override =
            g_caption_overrides.find(node.path);
    if (caption_override != g_caption_overrides.end() &&
        g_applied_caption_paths.insert(node.path).second) {
        node.caption = caption_override->second;
    }

    // CWindow draws its own background, then its foreground, then children.
    // A button's state presentation goes on top of that background rather
    // than instead of it: the shared background is the plate, the state
    // carries the label, and replacing one with the other loses whichever
    // the button kept its caption in.
    if (visible && shared != nullptr) {
        const std::map<std::string, const NDb::STexture*>::const_iterator
                texture_override = g_texture_overrides.find(node.name);
        const std::map<std::string, const NDb::STexture*>::const_iterator
                path_texture_override =
                        g_path_texture_overrides.find(node.path);
        const NDb::STexture* resolved_texture_override =
                path_texture_override != g_path_texture_overrides.end()
                        ? path_texture_override->second
                        : (texture_override == g_texture_overrides.end()
                                   ? nullptr
                                   : texture_override->second);
        AppendBackgroundQuads(
                shared->pBackground.GetPtr(),
                x,
                y,
                width,
                height,
                resolved_texture_override);
        if (window->GetTypeID() == NDb::SWindowMiniMap::typeID &&
            resolved_texture_override != nullptr) {
            MenuQuad minimap;
            minimap.x = x;
            minimap.y = y;
            minimap.width = width;
            minimap.height = height;
            minimap.texture =
                    TextureIndex(TexturePath(resolved_texture_override));
            if (minimap.texture >= 0) {
                g_quads.push_back(minimap);
            }
        }
        if (window->GetTypeID() ==
                    NDb::SWindow3DControl::typeID &&
            resolved_texture_override != nullptr) {
            // The desktop dialog renders the selected DB model into this
            // control. Until the menu has its own render target, keep the
            // authored slot functional with that unit's shipped HUD icon.
            MenuQuad preview;
            preview.x = x;
            preview.y = y;
            preview.width = width;
            preview.height = height;
            preview.texture = TextureIndex(
                    TexturePath(resolved_texture_override));
            if (preview.texture >= 0) {
                g_quads.push_back(preview);
            }
        }
        if (window->GetTypeID() == NDb::SWindowProgressBar::typeID) {
            const NDb::SWindowProgressBar* progress_bar =
                    static_cast<const NDb::SWindowProgressBar*>(window);
            const NDb::SWindowProgressBarShared* progress_shared =
                    dynamic_cast<const NDb::SWindowProgressBarShared*>(
                            shared);
            if (progress_shared != nullptr) {
                float progress = progress_bar->fProgress;
                const std::map<std::string, float>::const_iterator
                        progress_override =
                                g_progress_overrides.find(node.path);
                if (progress_override != g_progress_overrides.end()) {
                    progress = progress_override->second;
                }
                progress = std::max(0.0f, std::min(progress, 1.0f));
                AppendBackgroundQuads(
                        progress_shared->pBackward.GetPtr(),
                        x,
                        y,
                        width,
                        height);
                if (progress > 0.0f) {
                    AppendBackgroundQuads(
                            progress_shared->pForward.GetPtr(),
                            x,
                            y,
                            width * progress,
                            height);
                }
            }
        }
        if (window->GetTypeID() == NDb::SWindowMSButton::typeID) {
            const NDb::SWindowMSButtonShared* button_shared =
                    dynamic_cast<const NDb::SWindowMSButtonShared*>(shared);
            if (button_shared != nullptr &&
                !button_shared->visualStates.empty()) {
                size_t visual_state_index = 0;
                const std::map<std::string, size_t>::const_iterator
                        visual_state_override =
                                g_path_button_state_overrides.find(node.path);
                if (visual_state_override !=
                    g_path_button_state_overrides.end()) {
                    visual_state_index = std::min(
                            visual_state_override->second,
                            static_cast<size_t>(
                                    button_shared->visualStates.size() -
                                    1));
                }
                const NDb::SButtonVisualState& visual_state =
                        button_shared->visualStates[visual_state_index];
                const NDb::SButtonVisualSubState& normal =
                        node.enabled
                                ? visual_state.normal
                                : visual_state.disabled;
                AppendBackgroundQuads(
                        normal.pBackground.GetPtr(), x, y, width, height);
                // A state draws its background and then its foreground, and
                // for the common menu buttons the label is the foreground:
                // the plate is shared art, the caption is per button.
                AppendBackgroundQuads(
                        normal.pForeground.GetPtr(), x, y, width, height);
            }
        }
        AppendTextQuads(
                node.caption,
                node.text_format,
                x,
                y,
                width,
                height,
                node.text_face,
                node.text_argb);
    }

    if (node.button && visible && shared != nullptr) {
        const NDb::SWindowMSButtonShared* button_shared =
                dynamic_cast<const NDb::SWindowMSButtonShared*>(shared);
        if (button_shared != nullptr &&
            !button_shared->visualStates.empty()) {
            // Build the pushed presentation once so a press can swap to it
            // without re-walking the descriptor graph.
            size_t visual_state_index = 0;
            const std::map<std::string, size_t>::const_iterator
                    visual_state_override =
                            g_path_button_state_overrides.find(node.path);
            if (visual_state_override !=
                g_path_button_state_overrides.end()) {
                visual_state_index = std::min(
                        visual_state_override->second,
                        static_cast<size_t>(
                                button_shared->visualStates.size() -
                                1));
            }
            const NDb::SButtonVisualState& visual_state =
                    button_shared->visualStates[visual_state_index];
            const size_t normal_end = g_quads.size();
            AppendBackgroundQuads(
                    shared->pBackground.GetPtr(), x, y, width, height);
            AppendBackgroundQuads(
                    visual_state.pushed.pBackground.GetPtr(),
                    x,
                    y,
                    width,
                    height);
            AppendBackgroundQuads(
                    visual_state.pushed.pForeground.GetPtr(),
                    x,
                    y,
                    width,
                    height);
            AppendTextQuads(
                    node.caption,
                    node.text_format,
                    x,
                    y,
                    width,
                    height,
                    node.text_face,
                    node.text_argb);
            node.pressed_quad_begin =
                    static_cast<int>(g_pressed_quads.size());
            g_pressed_quads.insert(
                    g_pressed_quads.end(),
                    g_quads.begin() + normal_end,
                    g_quads.end());
            node.pressed_quad_end = static_cast<int>(g_pressed_quads.size());
            g_quads.resize(normal_end);
        }
    }

    if (!node.name.empty() &&
        g_templates.find(node.name) == g_templates.end()) {
        MenuTemplate entry;
        entry.window = window;
        entry.shared = shared;
        entry.x = x;
        entry.y = y;
        entry.width = width;
        entry.height = height;
        g_templates[node.name] = entry;
    }

    if (node.button) {
        ++g_button_count;
    }
    if (!node.background_texture.empty()) {
        ++g_texture_count;
    }
    if (!node.caption.empty()) {
        ++g_caption_count;
    }
    g_nodes.push_back(node);

    if (shared == nullptr) {
        return;
    }
    // This chapter-map modal contains eight complete panel variants. Walking
    // every hidden sibling exhausts the screen's legacy-node budget before
    // the active composition panel is reached. The desktop window tree also
    // stops at an invisible parent; preserve other screens' hidden template
    // discovery until their runtime builders no longer depend on it.
    if (!visible &&
        node.path.compare(
                0,
                std::char_traits<char>::length(
                        "ReinfDescriptionBackground"),
                "ReinfDescriptionBackground") == 0) {
        return;
    }
    // CWindow keeps its children in a priority-sorted draw order and draws
    // background, then text, then children, and its foreground last.
    std::vector<const NDb::SWindow*> children;
    children.reserve(shared->children.size());
    for (const CDBPtr<NDb::SUIDesc>& child : shared->children) {
        if (!child) {
            continue;
        }
        const NDb::SWindow* child_window =
                dynamic_cast<const NDb::SWindow*>(child.GetPtr());
        if (child_window != nullptr) {
            children.push_back(child_window);
        }
    }
    std::stable_sort(
            children.begin(),
            children.end(),
            [](const NDb::SWindow* first, const NDb::SWindow* second) {
                return first->nPriority < second->nPriority;
            });
    for (const NDb::SWindow* child_window : children) {
        CollectWindow(
                child_window,
                x,
                y,
                width,
                height,
                depth + 1,
                visible,
                node.path);
    }
    if (visible) {
        AppendBackgroundQuads(
                shared->pForeground.GetPtr(), x, y, width, height);
    }
}

}  // namespace

namespace {

bool RebuildMenuScreenLocked(const std::string& screen_ref);
std::string ScreenLayoutReportLocked();

}  // namespace

namespace {

// The shipped menu is a set of sibling screens the buttons push and pop.
// Every entry below is a real WindowScreen descriptor in UI/Game/Menu.
struct MenuScreenRoute {
    const char* reaction;
    const char* screen_ref;
};

constexpr MenuScreenRoute kMenuScreenRoutes[] = {
        {"campaign_selection",
         "UI/Game/Menu/CampaignSelection2/"
         "CampaignSelection_WindowScreen.xdb"},
        {"chapter_map", "UI/Game/Menu/ChapterMap_WindowScreen.xdb"},
        {"options", "UI/Game/Menu/OptionsMenu_WindowScreen.xdb"},
        {"Credits", "UI/Game/Menu/Credits_WindowScreen.xdb"},
        {"Encyclopedia", "UI/Game/Menu/Encyclopedia_WindowScreen.xdb"},
        {"multiplayer", "UI/Game/Menu/Multiplayer_WindowScreen.xdb"},
        {"ProfileManager", "UI/Game/Menu/ProfileManager_WindowScreen.xdb"},
        {"CustomMission", "UI/Game/Menu/CustomMissions_WindowScreen.xdb"},
        {"Load", "UI/Game/Menu/SaveLoadMenu_WindowScreen.xdb"},
        {"load", "UI/Game/Menu/SaveLoadMenu_WindowScreen.xdb"},
};

std::vector<std::string> g_screen_stack;

}  // namespace

namespace {

int g_selected_campaign = 0;
int g_selected_chapter = 0;
int g_selected_mission = 0;
// The original combo presents [Easy, Normal, Hard, Very Hard]. Campaign data
// stores the four matching descriptors as [Normal, Hard, Very Hard, Easy],
// and StartCampaign remaps the visible index before starting the tracker.
int g_selected_difficulty = 1;

enum ChapterTargetState {
    kChapterTargetDisabled = 0,
    kChapterTargetEnabled = 1,
    kChapterTargetRecommended = 2,
    kChapterTargetCompleted = 3,
};

std::vector<ChapterTargetState> g_chapter_target_states;
int g_chapter_selection_campaign = -1;
int g_chapter_selection_chapter = -1;
bool g_chapter_runtime_state_allowed = false;
bool g_chapter_frontline_animation_requested = false;
int g_chapter_frontline_animation_mission = -1;
int g_chapter_calls_available = 0;
int g_chapter_calls_for_selected_mission = 0;
std::map<int, const NDb::SReinforcement*>
        g_chapter_reinforcements;
bool g_chapter_roller_selection_requested = false;
int g_chapter_roller_chapter_from = 0;
int g_chapter_roller_mission_from = 0;

enum class ChapterReinfDialogMode {
    kNone,
    kComposition,
};

ChapterReinfDialogMode g_chapter_reinf_dialog_mode =
        ChapterReinfDialogMode::kNone;
int g_chapter_reinf_dialog_type = -1;
int g_chapter_reinf_dialog_unit = 0;

// The original WindowPlayer widgets play this Bink strip at 30 fps. Android
// stages an atlas derived losslessly from all 1,440 source frames so the
// original per-digit frame ranges can be reproduced without a Bink decoder.
constexpr const char* kChapterRollerAtlasPath =
        "UI/chaptermap/number18x33_atlas.dds";
constexpr int kChapterRollerFrameWidth = 18;
constexpr int kChapterRollerFrameHeight = 33;
constexpr int kChapterRollerAtlasColumns = 40;
constexpr int kChapterRollerAtlasRows = 36;
constexpr int kChapterRollerFramesPerDigit = 36;
constexpr int kChapterRollerFps = 30;
constexpr uint64_t kChapterRollerDelayMillis = 50;

enum class ChapterRollerReason {
    kStatic,
    kEntry,
    kSelection,
    kPostWin,
};

const char* ChapterRollerReasonName(ChapterRollerReason reason) {
    switch (reason) {
        case ChapterRollerReason::kEntry:
            return "entry";
        case ChapterRollerReason::kSelection:
            return "selection";
        case ChapterRollerReason::kPostWin:
            return "post_win";
        case ChapterRollerReason::kStatic:
        default:
            return "static";
    }
}

struct ChapterRollerTransition {
    bool prepared = false;
    ChapterRollerReason reason = ChapterRollerReason::kStatic;
    int chapter_from = 0;
    int chapter_to = 0;
    int mission_from = 0;
    int mission_to = 0;
    uint64_t delay_millis = 0;
};

struct ChapterRollerDigitPlayback {
    size_t quad_index = static_cast<size_t>(-1);
    int frame_start = 0;
    int frame_end = 0;
    int frame_skip = 0;
};

struct ChapterRollerAnimationState {
    std::vector<ChapterRollerDigitPlayback> digits;
    uint64_t elapsed_millis = 0;
    uint64_t delay_millis = 0;
    uint64_t last_tick_millis = 0;
    int chapter_from = 0;
    int chapter_to = 0;
    int mission_from = 0;
    int mission_to = 0;
    bool post_win = false;
    bool started = false;
};

ChapterRollerTransition g_chapter_roller_transition;
std::unique_ptr<ChapterRollerAnimationState>
        g_chapter_roller_animation;

constexpr const char* kCampaignSelectionScreenRef =
        "UI/Game/Menu/CampaignSelection2/"
        "CampaignSelection_WindowScreen.xdb";

std::string JoinHostPath(const std::string& left, const std::string& right) {
    if (left.empty()) {
        return right;
    }
    if (left.back() == '/') {
        return left + right;
    }
    return left + "/" + right;
}

// The shell's game loop polls this and hands control to the mission runtime
// without tearing the process down.
bool g_mission_launch_requested = false;

// Writes the shell's mission selection the same way MissionSelectActivity
// does, then asks Java to restart the activity out of menu mode.
bool RequestCampaignLaunch(
        int campaign_index,
        int mission_index) {
    const PortPaths paths = GetPortPaths();
    std::vector<std::string> targets;
    if (!paths.external_files_dir.empty()) {
        targets.push_back(JoinHostPath(
                paths.external_files_dir, "selected_mission.txt"));
    }
    if (!paths.files_dir.empty()) {
        targets.push_back(
                JoinHostPath(paths.files_dir, "selected_mission.txt"));
    }
    if (targets.empty()) {
        return false;
    }
    int runtime_difficulty = g_selected_difficulty;
    const NDb::SGameRoot* root = NGameX::GetGameRoot();
    if (root != nullptr &&
        campaign_index >= 0 &&
        static_cast<size_t>(campaign_index) < root->campaigns.size()) {
        const NDb::SCampaign* campaign =
                root->campaigns[static_cast<size_t>(campaign_index)].GetPtr();
        if (campaign != nullptr && campaign->difficultyLevels.size() == 4) {
            runtime_difficulty =
                    g_selected_difficulty == 0
                            ? 3
                            : g_selected_difficulty - 1;
        }
    }
    bool written = false;
    for (const std::string& target : targets) {
        std::ofstream file(target.c_str(), std::ios::trunc);
        if (!file.is_open()) {
            continue;
        }
        file << "campaign=" << campaign_index << "\n";
        file << "chapter=" << g_selected_chapter << "\n";
        file << "mission=" << mission_index << "\n";
        file << "difficulty=" << runtime_difficulty << "\n";
        written = true;
    }
    if (!written) {
        return false;
    }
    std::ostringstream report;
    report << "original_menu_launch=campaign; index=" << campaign_index
           << "; chapter=" << g_selected_chapter
           << "; mission=" << mission_index
           << "; difficulty_ui=" << g_selected_difficulty
           << "; difficulty_db=" << runtime_difficulty;
    PlatformRuntime::instance().log_info(report.str());
    g_mission_launch_requested = true;
    return true;
}

}  // namespace

namespace {

// Advances one option to its next shipped state, mirroring what the desktop
// options screen writes when its control changes.
// The quality preset is not a row of its own: the shipped entry lists the
// variables it drives, one value per step, and CommandSetQuality writes them
// all. Without this the phone-relevant levers -- shadows, grass, LOD, tree
// shadows -- never move, because they have no row on the screen.
// NGlobal keeps option values in memory only, so a phone that swaps the app
// out loses every setting. They are mirrored into the save directory and read
// back before the first screen is built.
std::string MenuOptionsStorePath() {
    return JoinHostPath(GetPortPaths().save_root(), "options.cfg");
}

void ForEachOptionVariable(
        const std::function<void(const std::string&)>& visit) {
    const NDb::SGameRoot* root = NGameX::GetGameRoot();
    if (root == nullptr || !root->pGameOptions) {
        return;
    }
    const NDb::SOptionSystem* options = root->pGameOptions.GetPtr();
    if (options == nullptr) {
        return;
    }
    for (const NDb::SOptionSystem::SOptionsCategory& category :
         options->categories) {
        for (const NDb::SOptionSystem::SOptionsCategory::SOptionEntry& entry :
             category.options) {
            visit(ToStdString(entry.szProgName));
            for (const NDb::SOptionSystem::SOptionsCategory::SOptionEntry::
                         SSliderSingleValue& slider : entry.sliderValues) {
                visit(ToStdString(slider.szProgName));
            }
        }
    }
}

void SaveMenuOptions() {
    std::ofstream output(MenuOptionsStorePath(), std::ios::trunc);
    if (!output.is_open()) {
        return;
    }
    size_t written = 0;
    ForEachOptionVariable([&output, &written](const std::string& name) {
        const std::string value(
                NStr::ToMBCS(NGlobal::GetVar(string(name.c_str()), ""))
                        .c_str());
        if (value.empty()) {
            return;
        }
        output << name << '=' << value << '\n';
        ++written;
    });
    std::ostringstream report;
    report << "original_menu_options_saved=" << written;
    PlatformRuntime::instance().log_info(report.str());
}

void LoadMenuOptions() {
    std::ifstream input(MenuOptionsStorePath());
    if (!input.is_open()) {
        return;
    }
    std::string line;
    size_t restored = 0;
    while (std::getline(input, line)) {
        const size_t separator = line.find('=');
        if (separator == std::string::npos || separator == 0) {
            continue;
        }
        NGlobal::SetVar(
                string(line.substr(0, separator).c_str()),
                line.substr(separator + 1).c_str());
        ++restored;
    }
    std::ostringstream report;
    report << "original_menu_options_restored=" << restored;
    PlatformRuntime::instance().log_info(report.str());
}

size_t ApplyOptionSliderValues(
        const NDb::SOptionSystem::SOptionsCategory::SOptionEntry& entry,
        const std::string& value) {
    if (entry.sliderValues.empty() ||
        entry.sliderValues[0].values.empty()) {
        return 0;
    }
    const int step_count =
            static_cast<int>(entry.sliderValues[0].values.size());
    const float parameter = static_cast<float>(std::atof(value.c_str()));
    int step = static_cast<int>(std::lround(
            parameter * static_cast<float>(step_count) - 0.5f));
    step = std::max(0, std::min(step, step_count - 1));
    size_t applied = 0;
    for (const NDb::SOptionSystem::SOptionsCategory::SOptionEntry::
                 SSliderSingleValue& slider : entry.sliderValues) {
        if (static_cast<size_t>(step) >= slider.values.size()) {
            continue;
        }
        NGlobal::SetVar(slider.szProgName, slider.values[step].c_str());
        ++applied;
    }
    return applied;
}

bool CycleOptionValue(const std::string& program_name) {
    const NDb::SGameRoot* root = NGameX::GetGameRoot();
    if (root == nullptr || !root->pGameOptions) {
        return false;
    }
    const NDb::SOptionSystem* options = root->pGameOptions.GetPtr();
    if (options == nullptr) {
        return false;
    }
    for (const NDb::SOptionSystem::SOptionsCategory& category :
         options->categories) {
        for (const NDb::SOptionSystem::SOptionsCategory::SOptionEntry& entry :
             category.options) {
            if (ToStdString(entry.szProgName) != program_name) {
                continue;
            }
            const std::string current = std::string(
                    NStr::ToMBCS(NGlobal::GetVar(
                            entry.szProgName,
                            entry.szDefaultValue)).c_str());
            std::string next;
            if (!entry.states.empty()) {
                size_t index = 0;
                for (size_t i = 0; i < entry.states.size(); ++i) {
                    if (ToStdString(entry.states[i].szValue) == current) {
                        index = i + 1;
                        break;
                    }
                }
                next = ToStdString(
                        entry.states[index % entry.states.size()].szValue);
            } else if (entry.eEditorType ==
                       NDb::SOptionSystem::SOptionsCategory::SOptionEntry::
                               OPTION_EDITOR_SLIDER) {
                // Sliders step in quarters so a tap is still useful on touch.
                const float value =
                        static_cast<float>(std::atof(current.c_str()));
                const float stepped = value + 0.25f > 1.0f
                        ? 0.0f
                        : value + 0.25f;
                std::ostringstream formatted;
                formatted << stepped;
                next = formatted.str();
            } else {
                next = current == "1" ? "0" : "1";
            }
            NGlobal::SetVar(entry.szProgName, next.c_str());
            const size_t driven = ApplyOptionSliderValues(entry, next);
            std::ostringstream report;
            report << "original_menu_option=" << program_name
                   << "; from=" << current << "; to=" << next
                   << "; driven=" << driven;
            PlatformRuntime::instance().log_info(report.str());
            SaveMenuOptions();
            return true;
        }
    }
    return false;
}

}  // namespace

const NDb::SChapter* SelectedChapter();
bool LoadOriginalMissionBriefingScreen(int mission_index);

bool ConsumeMenuMissionLaunchRequest() {
    const bool requested = g_mission_launch_requested;
    g_mission_launch_requested = false;
    return requested;
}

bool RunOriginalMenuReaction(const std::string& reaction) {
    if (reaction.empty()) {
        return false;
    }
    // The campaign selection screen picks with static buttons and starts with
    // Play / Continue, both of which run the chapter_map reaction.
    if (reaction.size() == 10 &&
        reaction.compare(0, 8, "campaign") == 0 &&
        reaction[8] == '0') {
        const int index = reaction[9] - '1';
        if (index >= 0 && index < 8) {
            g_selected_campaign = index;
            g_selected_chapter = 0;
            g_selected_mission = 0;
            g_chapter_selection_campaign = -1;
            g_chapter_selection_chapter = -1;
            g_chapter_runtime_state_allowed = false;
            g_chapter_reinf_dialog_mode =
                    ChapterReinfDialogMode::kNone;
            std::ostringstream report;
            report << "original_menu_campaign=" << index;
            PlatformRuntime::instance().log_info(report.str());
            std::string screen_ref;
            {
                std::lock_guard<std::mutex> guard(g_menu_mutex);
                screen_ref = g_screen_ref;
            }
            if (screen_ref == kCampaignSelectionScreenRef) {
                return LoadOriginalMenuScreen(screen_ref);
            }
            return true;
        }
    }
    std::string current_screen;
    {
        std::lock_guard<std::mutex> guard(g_menu_mutex);
        current_screen = g_screen_ref;
    }
    if ((reaction == "Play" || reaction == "menu_play") &&
        current_screen ==
                "UI/Game/Menu/MissionBriefing/"
                "MissionBriefing_WindowScreen.xdb") {
        return RequestCampaignLaunch(
                g_selected_campaign,
                g_selected_mission);
    }
    if ((reaction == "Back" || reaction == "menu_back") &&
        current_screen ==
                "UI/Game/Menu/MissionBriefing/"
                "MissionBriefing_WindowScreen.xdb") {
        return LoadOriginalMenuScreen(
                "UI/Game/Menu/ChapterMap_WindowScreen.xdb");
    }
    if (reaction == "difficulty_cycle" &&
        current_screen == kCampaignSelectionScreenRef) {
        const NDb::SGameRoot* root = NGameX::GetGameRoot();
        if (root == nullptr ||
            g_selected_campaign < 0 ||
            static_cast<size_t>(g_selected_campaign) >=
                    root->campaigns.size()) {
            return false;
        }
        const NDb::SCampaign* campaign =
                root->campaigns[
                        static_cast<size_t>(g_selected_campaign)].GetPtr();
        if (campaign == nullptr || campaign->difficultyLevels.empty()) {
            return false;
        }
        g_selected_difficulty =
                (g_selected_difficulty + 1) %
                static_cast<int>(campaign->difficultyLevels.size());
        return LoadOriginalMenuScreen(current_screen);
    }
    // Campaign intros are not decodable on Android yet. Until the original
    // movie path is restored, continue to the exact selected campaign's map
    // instead of dropping the Play reaction.
    if ((reaction == "Play" || reaction == "menu_play") &&
        current_screen == kCampaignSelectionScreenRef) {
        g_chapter_selection_campaign = -1;
        g_chapter_selection_chapter = -1;
        g_chapter_runtime_state_allowed = false;
        g_chapter_reinf_dialog_mode =
                ChapterReinfDialogMode::kNone;
        return RunOriginalMenuReaction("chapter_map");
    }
    if (reaction.compare(
                0,
                23,
                "show_reinf_composition_") == 0 &&
        current_screen ==
                "UI/Game/Menu/ChapterMap_WindowScreen.xdb") {
        const int reinforcement_type =
                std::atoi(reaction.c_str() + 23);
        if (g_chapter_reinforcements.find(
                    reinforcement_type) ==
            g_chapter_reinforcements.end()) {
            return false;
        }
        g_chapter_reinf_dialog_mode =
                ChapterReinfDialogMode::kComposition;
        g_chapter_reinf_dialog_type = reinforcement_type;
        g_chapter_reinf_dialog_unit = 0;
        return LoadOriginalMenuScreen(current_screen);
    }
    if (reaction == "close_reinf_composition" &&
        current_screen ==
                "UI/Game/Menu/ChapterMap_WindowScreen.xdb") {
        g_chapter_reinf_dialog_mode =
                ChapterReinfDialogMode::kNone;
        return LoadOriginalMenuScreen(current_screen);
    }
    if (reaction.compare(
                0,
                30,
                "select_reinf_composition_unit_") == 0 &&
        current_screen ==
                "UI/Game/Menu/ChapterMap_WindowScreen.xdb" &&
        g_chapter_reinf_dialog_mode ==
                ChapterReinfDialogMode::kComposition) {
        const int unit_index =
                std::atoi(reaction.c_str() + 30);
        if (unit_index < 0 || unit_index >= 4) {
            return false;
        }
        g_chapter_reinf_dialog_unit = unit_index;
        return LoadOriginalMenuScreen(current_screen);
    }
    if (reaction.compare(0, 15, "select_mission_") == 0) {
        const int mission_index =
                std::atoi(reaction.c_str() + 15);
        const NDb::SChapter* chapter = SelectedChapter();
        if (chapter == nullptr ||
            mission_index < 0 ||
            static_cast<size_t>(mission_index) >=
                    chapter->missionPath.size() ||
            static_cast<size_t>(mission_index) >=
                    g_chapter_target_states.size() ||
            g_chapter_target_states[
                    static_cast<size_t>(mission_index)] ==
                    kChapterTargetCompleted) {
            return false;
        }
        g_chapter_roller_selection_requested = true;
        g_chapter_roller_chapter_from = std::max(
                0,
                g_chapter_calls_available -
                        g_chapter_calls_for_selected_mission);
        g_chapter_roller_mission_from =
                std::max(0, g_chapter_calls_for_selected_mission);
        g_chapter_reinf_dialog_mode =
                ChapterReinfDialogMode::kNone;
        g_selected_mission = mission_index;
        return LoadOriginalMenuScreen(current_screen);
    }
    // The chapter map is a screen of its own between picking a campaign and
    // the mission.
    if (reaction == "play") {
        if (g_selected_mission < 0 ||
            static_cast<size_t>(g_selected_mission) >=
                    g_chapter_target_states.size() ||
            (g_chapter_target_states[
                     static_cast<size_t>(g_selected_mission)] !=
                     kChapterTargetEnabled &&
             g_chapter_target_states[
                     static_cast<size_t>(g_selected_mission)] !=
                     kChapterTargetRecommended)) {
            return false;
        }
        return LoadOriginalMissionBriefingScreen(
                g_selected_mission);
    }
    if (reaction.compare(0, 13, "play_mission_") == 0) {
        const int mission_index =
                std::atoi(reaction.c_str() + 13);
        if (mission_index < 0) {
            return false;
        }
        if (static_cast<size_t>(mission_index) >=
                    g_chapter_target_states.size() ||
            (g_chapter_target_states[
                     static_cast<size_t>(mission_index)] !=
                     kChapterTargetEnabled &&
             g_chapter_target_states[
                     static_cast<size_t>(mission_index)] !=
                     kChapterTargetRecommended)) {
            return false;
        }
        return LoadOriginalMissionBriefingScreen(mission_index);
    }
    // The statistics screen's DB command file names are what ButtonAction()
    // sees, while the desktop controller receives the reaction string inside
    // those files. Accept both forms so the shipped controls remain usable.
    if (reaction == "restart_mission" ||
        reaction == "RestartMission") {
        g_mission_launch_requested = true;
        return true;
    }
    if (reaction == "exit_to_chapter" ||
        reaction == "ExitToChapter" ||
        reaction == "menu_next" ||
        reaction == "Next") {
        g_chapter_runtime_state_allowed = true;
        g_chapter_frontline_animation_requested = true;
        return LoadOriginalMenuScreen(
                "UI/Game/Menu/ChapterMap_WindowScreen.xdb");
    }
    if (reaction == "exit_to_main_menu" ||
        reaction == "ExitToMainMenu") {
        return LoadOriginalMenuScreen(
                "UI/Game/Menu/MainMenu_WindowScreen.xdb");
    }
    // Tapping an option row advances it to its next shipped state and stores
    // the value through the same global variable the desktop screen uses.
    if (reaction.compare(0, 13, "option_cycle_") == 0) {
        const std::string program_name = reaction.substr(13);
        std::string screen_ref;
        {
            std::lock_guard<std::mutex> guard(g_menu_mutex);
            screen_ref = g_screen_ref;
        }
        if (!CycleOptionValue(program_name)) {
            return false;
        }
        std::lock_guard<std::mutex> guard(g_menu_mutex);
        return RebuildMenuScreenLocked(screen_ref);
    }
    // Options category tabs re-fill the option list for that category.
    if (reaction.compare(0, 16, "option_category_") == 0) {
        const int index = std::atoi(reaction.c_str() + 16);
        std::string screen_ref;
        {
            std::lock_guard<std::mutex> guard(g_menu_mutex);
            g_options_category = index;
            screen_ref = g_screen_ref;
        }
        std::lock_guard<std::mutex> guard(g_menu_mutex);
        return RebuildMenuScreenLocked(screen_ref);
    }
    // Panel swaps stay inside the current screen.
    if (reaction == "single_player") {
        return ShowOriginalMenuPanel("SinglePlayerMenu");
    }
    if (reaction == "single_player_back") {
        return ShowOriginalMenuPanel("MainMenu");
    }
    for (const MenuScreenRoute& route : kMenuScreenRoutes) {
        if (reaction != route.reaction) {
            continue;
        }
        std::string previous;
        {
            std::lock_guard<std::mutex> guard(g_menu_mutex);
            previous = g_screen_ref;
        }
        if (!LoadOriginalMenuScreen(route.screen_ref)) {
            // Keep the caller on a working screen if the target fails.
            LoadOriginalMenuScreen(previous);
            return false;
        }
        g_screen_stack.push_back(previous);
        {
            std::lock_guard<std::mutex> guard(g_menu_mutex);
            PlatformRuntime::instance().log_info(
                    ScreenLayoutReportLocked());
        }
        return true;
    }
    // Every shipped back button ends in "back"; pop the pushed screen.
    if (reaction.size() >= 4 &&
        reaction.compare(reaction.size() - 4, 4, "back") == 0 &&
        !g_screen_stack.empty()) {
        const std::string previous = g_screen_stack.back();
        g_screen_stack.pop_back();
        return LoadOriginalMenuScreen(previous);
    }
    return false;
}

bool ShowOriginalMenuPanel(const std::string& panel_name) {
    std::lock_guard<std::mutex> guard(g_menu_mutex);
    if (!g_ready) {
        return false;
    }
    bool known = false;
    for (const char* panel : kMenuPanels) {
        const bool selected = panel_name == panel;
        known = known || selected;
        g_visibility_overrides[panel] = selected;
    }
    if (!known) {
        return false;
    }
    const std::string screen_ref = g_screen_ref;
    return RebuildMenuScreenLocked(screen_ref);
}

// Some screens ship windows marked visible that their controller hides the
// moment it initialises. The chapter map is the clear case: the reinforcement
// description is four stacked variants, all visible in the descriptor, and
// CInterfaceChapterMapMenu::Init hides the panel outright until the player
// picks a reinforcement. Without a controller the port drew all four at once.
struct ScreenInitHiddenWindow {
    const char* screen_ref;
    const char* window_name;
};

constexpr ScreenInitHiddenWindow kScreenInitHiddenWindows[] = {
        {"UI/Game/Menu/ChapterMap_WindowScreen.xdb",
         "ReinfDescriptionBackground"},
};

void ApplyScreenInitVisibilityLocked(const std::string& screen_ref) {
    for (const ScreenInitHiddenWindow& hidden : kScreenInitHiddenWindows) {
        if (screen_ref == hidden.screen_ref) {
            g_visibility_overrides[hidden.window_name] = false;
        }
    }
}

void ApplyCampaignSelectionBindingsLocked(const std::string& screen_ref) {
    if (screen_ref != kCampaignSelectionScreenRef) {
        return;
    }
    const NDb::SGameRoot* root = NGameX::GetGameRoot();
    const size_t campaign_count =
            root == nullptr
                    ? 0
                    : std::min<size_t>(3, root->campaigns.size());
    if (campaign_count == 0) {
        g_selected_campaign = 0;
    } else if (g_selected_campaign < 0 ||
               static_cast<size_t>(g_selected_campaign) >= campaign_count) {
        g_selected_campaign = 0;
    }

    if (root != nullptr && root->pInterfacesBackground) {
        g_path_texture_overrides["Main"] =
                root->pInterfacesBackground.GetPtr();
    }

    for (size_t slot = 0; slot < 5; ++slot) {
        const std::string panel =
                "Main/CampaignPanel" + std::to_string(slot + 1);
        const bool available = slot < campaign_count;
        g_path_visibility_overrides[panel] = available;
        if (!available || root == nullptr) {
            continue;
        }
        const NDb::SCampaign* campaign =
                root->campaigns[slot].GetPtr();
        if (campaign == nullptr) {
            g_path_visibility_overrides[panel] = false;
            continue;
        }
        g_path_visibility_overrides[panel + "/SelectCampaignBtn"] =
                static_cast<int>(slot) != g_selected_campaign;
        g_caption_overrides[panel + "/CampaignNameView"] =
                LoadUtf16Text(NormalizeResourcePath(
                        ToStdString(campaign->szLocalizedNameFileRef)));
        // The controller pushes the description into the scroll container;
        // suppress the descriptor's placeholder before the wrapped runtime
        // copy is appended after layout.
        g_caption_overrides[panel + "/DescCont/DescView"] =
                std::string();
        if (campaign->pTextureNotStarted) {
            g_path_texture_overrides[panel + "/Flag"] =
                    campaign->pTextureNotStarted.GetPtr();
        }
    }

    const NDb::SCampaign* selected =
            root != nullptr && campaign_count > 0
                    ? root->campaigns[
                              static_cast<size_t>(g_selected_campaign)]
                              .GetPtr()
                    : nullptr;
    const int difficulty_count =
            selected == nullptr
                    ? 0
                    : static_cast<int>(selected->difficultyLevels.size());
    if (difficulty_count > 0) {
        g_selected_difficulty = std::max(
                0,
                std::min(g_selected_difficulty, difficulty_count - 1));
    } else {
        g_selected_difficulty = 0;
        g_path_visibility_overrides["Main/BottomPanel/Difficulty"] = false;
    }
}

// The chapter the player is about to fight. Campaign progress is not tracked
// yet, so this is the campaign's first chapter, which is what a new campaign
// opens on.
const NDb::SChapter* SelectedChapter() {
    const NDb::SGameRoot* root = NGameX::GetGameRoot();
    if (root == nullptr ||
        g_selected_campaign < 0 ||
        static_cast<size_t>(g_selected_campaign) >= root->campaigns.size()) {
        return nullptr;
    }
    const NDb::SCampaign* campaign =
            root->campaigns[g_selected_campaign].GetPtr();
    if (campaign == nullptr || campaign->chapters.empty()) {
        return nullptr;
    }
    const int chapter_index =
            std::max(0, std::min(
                    g_selected_chapter,
                    static_cast<int>(campaign->chapters.size()) - 1));
    return campaign->chapters[chapter_index].GetPtr();
}

const NDb::SMapInfo* SelectedMission() {
    const NDb::SChapter* chapter = SelectedChapter();
    if (chapter == nullptr ||
        g_selected_mission < 0 ||
        static_cast<size_t>(g_selected_mission) >=
                chapter->missionPath.size()) {
        return nullptr;
    }
    return chapter->missionPath[
            static_cast<size_t>(g_selected_mission)].pMap.GetPtr();
}

bool ContainsResourceId(
        const std::vector<std::string>& resource_ids,
        const std::string& resource_id) {
    return std::find(
                   resource_ids.begin(),
                   resource_ids.end(),
                   resource_id) != resource_ids.end();
}

std::vector<int> InitialChapterReinforcementTypes(
        const NDb::SChapter* chapter) {
    std::vector<int> types;
    if (chapter == nullptr ||
        chapter->basePlayerReinforcements.empty()) {
        return types;
    }
    for (const CDBPtr<NDb::SReinforcement>& reinforcement_ptr :
         chapter->basePlayerReinforcements[0].reinforcements) {
        const NDb::SReinforcement* reinforcement =
                reinforcement_ptr.GetPtr();
        if (reinforcement != nullptr &&
            std::find(
                    types.begin(),
                    types.end(),
                    static_cast<int>(reinforcement->eType)) ==
                    types.end()) {
            types.push_back(static_cast<int>(reinforcement->eType));
        }
    }
    return types;
}

bool InitialMissionReinforcementsSatisfied(
        const NDb::SChapter* chapter,
        const NDb::SMapInfo* mission,
        const std::vector<int>& reinforcement_types) {
    if (chapter == nullptr ||
        mission == nullptr ||
        chapter->bUseMapReinforcements ||
        mission->players.empty() ||
        mission->players[0].reinforcementTypes.empty()) {
        return true;
    }
    for (const CDBPtr<NDb::SReinforcement>& reinforcement_ptr :
         mission->players[0].reinforcementTypes) {
        const NDb::SReinforcement* reinforcement =
                reinforcement_ptr.GetPtr();
        if (reinforcement != nullptr &&
            std::find(
                    reinforcement_types.begin(),
                    reinforcement_types.end(),
                    static_cast<int>(reinforcement->eType)) !=
                    reinforcement_types.end()) {
            return true;
        }
    }
    return false;
}

const NDb::SReinfButton* ReinfButtonForType(int reinforcement_type) {
    const NDb::SUIConstsB2* ui_consts = NGameX::GetUIConsts();
    if (ui_consts == nullptr) {
        return nullptr;
    }
    for (const NDb::SReinfButton& button : ui_consts->reinfButtons) {
        if (static_cast<int>(button.eType) == reinforcement_type) {
            return &button;
        }
    }
    return nullptr;
}

std::string ReinfButtonName(const NDb::SReinfButton* button) {
    return button != nullptr && button->pButton
            ? ToStdString(button->pButton->szName)
            : std::string();
}

const NDb::STexture* ReinfIconTexture(
        const NDb::SReinforcement* reinforcement,
        const NDb::SReinfButton* button,
        bool disabled) {
    if (reinforcement != nullptr &&
        reinforcement->pIconTexture &&
        !disabled) {
        return reinforcement->pIconTexture.GetPtr();
    }
    if (button == nullptr) {
        return nullptr;
    }
    if (disabled && button->pTextureDisabled) {
        return button->pTextureDisabled.GetPtr();
    }
    return button->pTexture.GetPtr();
}

void HideReinfButtonDetails(const std::string& button_path) {
    g_path_visibility_overrides[button_path + "/Icon"] = false;
    g_path_visibility_overrides[button_path + "/IconDisabled"] = false;
    g_path_visibility_overrides[button_path + "/UnitNumber"] = false;
    g_path_visibility_overrides[button_path + "/Unknown"] = false;
    g_path_visibility_overrides[button_path + "/Icon/XPLevel"] = false;
    g_path_visibility_overrides[button_path + "/Icon/XPBar"] = false;
    g_path_visibility_overrides[button_path + "/Icon/XPBarBg"] = false;
    g_path_visibility_overrides[button_path + "/Icon/BadWeather"] = false;
}

void BindChapterReinforcement(
        int reinforcement_type,
        const NDb::SReinforcement* reinforcement,
        int state) {
    const NDb::SReinfButton* button =
            ReinfButtonForType(reinforcement_type);
    const std::string button_name = ReinfButtonName(button);
    if (button_name.empty()) {
        return;
    }
    const std::string button_path =
            "ChapterMapMain/ChapterMapRight/ReinforcementGrid/" +
            button_name;
    HideReinfButtonDetails(button_path);
    g_path_enabled_overrides[button_path] = state == 2;
    if (state == 2) {
        g_path_button_state_overrides[button_path] = 0;
        g_path_visibility_overrides[button_path + "/Icon"] = true;
        const NDb::STexture* texture =
                ReinfIconTexture(reinforcement, button, false);
        if (texture != nullptr) {
            g_path_texture_overrides[button_path + "/Icon"] = texture;
        }
    } else if (state == 1) {
        g_path_button_state_overrides[button_path] = 1;
        g_path_visibility_overrides[button_path + "/Icon"] = true;
        g_path_visibility_overrides[button_path + "/Unknown"] = true;
        const NDb::STexture* texture =
                ReinfIconTexture(nullptr, button, true);
        if (texture != nullptr) {
            g_path_texture_overrides[button_path + "/Icon"] = texture;
        }
    } else {
        g_path_button_state_overrides[button_path] = 2;
    }
}

struct ChapterReinfDialogUnit {
    const NDb::SHPObjectRPGStats* stats = nullptr;
    int count = 0;
};

std::vector<ChapterReinfDialogUnit> ChapterReinfDialogUnits(
        const NDb::SReinforcement* reinforcement) {
    std::vector<ChapterReinfDialogUnit> units;
    if (reinforcement == nullptr) {
        return units;
    }
    for (const NDb::SReinforcementEntry& entry :
         reinforcement->entries) {
        const NDb::SHPObjectRPGStats* stats =
                entry.pMechUnit
                        ? static_cast<
                                  const NDb::SHPObjectRPGStats*>(
                                  entry.pMechUnit.GetPtr())
                        : static_cast<
                                  const NDb::SHPObjectRPGStats*>(
                                  entry.pSquad.GetPtr());
        if (stats == nullptr) {
            continue;
        }
        std::vector<ChapterReinfDialogUnit>::iterator existing =
                std::find_if(
                        units.begin(),
                        units.end(),
                        [stats](const ChapterReinfDialogUnit& unit) {
                            return unit.stats == stats;
                        });
        if (existing == units.end()) {
            ChapterReinfDialogUnit unit;
            unit.stats = stats;
            unit.count = 1;
            units.push_back(unit);
        } else {
            ++existing->count;
        }
    }
    return units;
}

void HideChapterReinfDialogPanelsLocked() {
    constexpr const char* kDialogRoot =
            "ReinfDescriptionBackground";
    const char* const panels[] = {
            "ReinfDesc1Unit",
            "ReinfDesc1Reinf",
            "ReinfDescMultiReinf",
            "ReinfDescUpgrade",
            "UpgradePanel",
            "CompositionPanel",
            "MissionDesc",
            "ChapterDesc",
    };
    for (const char* panel : panels) {
        g_path_visibility_overrides[
                std::string(kDialogRoot) + "/" + panel] = false;
    }
}

void ApplyChapterReinfCompositionBindingsLocked() {
    HideChapterReinfDialogPanelsLocked();
    if (g_chapter_reinf_dialog_mode !=
        ChapterReinfDialogMode::kComposition) {
        return;
    }
    const std::map<
            int,
            const NDb::SReinforcement*>::const_iterator reinforcement_it =
            g_chapter_reinforcements.find(
                    g_chapter_reinf_dialog_type);
    if (reinforcement_it == g_chapter_reinforcements.end() ||
        reinforcement_it->second == nullptr) {
        g_chapter_reinf_dialog_mode =
                ChapterReinfDialogMode::kNone;
        return;
    }
    const NDb::SReinforcement* reinforcement =
            reinforcement_it->second;
    const std::vector<ChapterReinfDialogUnit> units =
            ChapterReinfDialogUnits(reinforcement);
    if (units.empty()) {
        g_chapter_reinf_dialog_unit = 0;
    } else {
        g_chapter_reinf_dialog_unit = std::clamp(
                g_chapter_reinf_dialog_unit,
                0,
                static_cast<int>(
                        std::min<size_t>(units.size(), 4) - 1));
    }

    constexpr const char* kDialogRoot =
            "ReinfDescriptionBackground";
    constexpr const char* kPanel =
            "ReinfDescriptionBackground/CompositionPanel";
    constexpr const char* kDescription =
            "ReinfDescriptionBackground/CompositionPanel";
    g_visibility_overrides[kDialogRoot] = true;
    g_path_visibility_overrides[kPanel] = true;

    g_caption_overrides[
            std::string(kDescription) + "/ReinfNameView"] =
            LoadUtf16Text(NormalizeResourcePath(
                    ToStdString(
                            reinforcement
                                    ->szLocalizedNameFileRef)));
    if (reinforcement->pIconTexture) {
        g_path_texture_overrides[
                std::string(kDescription) +
                "/ReinfIcon"] =
                reinforcement->pIconTexture.GetPtr();
    }

    constexpr size_t kCompositionSlots = 4;
    for (size_t index = 0; index < kCompositionSlots; ++index) {
        const std::string unit_path =
                std::string(kDescription) + "/Unit" +
                std::to_string(index + 1);
        const std::string sub_panel =
                unit_path;
        const std::string unit_button =
                sub_panel + "/UnitBtn";
        const std::string appearance =
                unit_button + "/UnitAppearance";
        const std::string control =
                sub_panel + "/3DControl";
        const bool populated = index < units.size();
        g_path_enabled_overrides[unit_button] = populated;
        g_path_enabled_overrides[
                sub_panel + "/UnitCountBtn"] = populated;
        g_path_visibility_overrides[appearance] = populated;
        g_path_visibility_overrides[control] = populated;
        g_path_visibility_overrides[
                sub_panel + "/UnitUnknown"] = false;
        g_path_visibility_overrides[
                sub_panel + "/UnitNone"] = !populated;
        g_path_visibility_overrides[
                sub_panel + "/UnitSelection"] =
                populated &&
                static_cast<int>(index) ==
                        g_chapter_reinf_dialog_unit;
        const bool show_count =
                populated && units[index].count > 1;
        g_path_visibility_overrides[
                sub_panel + "/UnitCountBtn/UnitCount"] =
                show_count;
        if (!populated) {
            continue;
        }
        const NDb::SHPObjectRPGStats* stats =
                units[index].stats;
        g_caption_overrides[
                appearance + "/HPView"] =
                std::to_string(static_cast<int>(
                        std::round(std::max(
                                0.0f,
                                stats->fMaxHP))));
        g_progress_overrides[appearance + "/HPBar"] = 1.0f;
        g_caption_overrides[
                sub_panel +
                "/UnitCountBtn/UnitCount/UnitCountView"] =
                show_count
                        ? std::to_string(units[index].count)
                        : std::string();
        if (stats->pIconTexture) {
            g_path_texture_overrides[control] =
                    stats->pIconTexture.GetPtr();
        }
        const char* const armor_views[] = {
                "ArmorFrontView",
                "ArmorLeftView",
                "ArmorRightView",
                "ArmorTopView",
                "ArmorBackView",
        };
        for (const char* armor_view : armor_views) {
            g_path_visibility_overrides[
                    control + "/" + armor_view] = false;
        }
    }

    const std::string info =
            std::string(kDescription);
    for (int weapon = 1; weapon <= 4; ++weapon) {
        g_path_visibility_overrides[
                info + "/UnitWeapon" +
                std::to_string(weapon)] = false;
    }
    g_path_visibility_overrides[
            std::string(kDescription) +
            "/UnitSupplyLabel"] = false;
    g_path_visibility_overrides[
            std::string(kDescription) +
            "/UnitSupplyView"] = false;
    if (!units.empty()) {
        const NDb::SHPObjectRPGStats* selected =
                units[static_cast<size_t>(
                        g_chapter_reinf_dialog_unit)].stats;
        g_caption_overrides[info + "/UnitName"] =
                LoadUtf16Text(NormalizeResourcePath(
                        ToStdString(
                                selected
                                        ->szLocalizedNameFileRef)));
    } else {
        g_caption_overrides[info + "/UnitName"] = std::string();
    }

    std::ostringstream report;
    report << "original_menu_chapter_reinf_composition=ready"
           << "; type=" << g_chapter_reinf_dialog_type
           << "; reinforcement="
           << ToStdString(reinforcement->GetDBID().ToString())
           << "; entries=" << reinforcement->entries.size()
           << "; unit_types=" << units.size()
           << "; selected=" << g_chapter_reinf_dialog_unit
           << "; preview=icon_fallback";
    PlatformRuntime::instance().log_info(report.str());
}

void ApplyChapterMapBindingsLocked(const std::string& screen_ref) {
    if (screen_ref != "UI/Game/Menu/ChapterMap_WindowScreen.xdb") {
        return;
    }
    const bool frontline_animation_requested =
            g_chapter_frontline_animation_requested;
    const bool roller_selection_requested =
            g_chapter_roller_selection_requested;
    const int roller_chapter_from =
            g_chapter_roller_chapter_from;
    const int roller_mission_from =
            g_chapter_roller_mission_from;
    g_chapter_frontline_animation_requested = false;
    g_chapter_frontline_animation_mission = -1;
    g_chapter_roller_selection_requested = false;
    g_chapter_roller_transition = ChapterRollerTransition();
    g_chapter_reinforcements.clear();
    const NDb::SGameRoot* root = NGameX::GetGameRoot();
    const NDb::SChapter* chapter = SelectedChapter();
    if (root == nullptr ||
        chapter == nullptr ||
        chapter->missionPath.empty() ||
        g_selected_campaign < 0 ||
        static_cast<size_t>(g_selected_campaign) >= root->campaigns.size()) {
        g_chapter_target_states.clear();
        return;
    }
    const NDb::SCampaign* campaign =
            root->campaigns[static_cast<size_t>(g_selected_campaign)]
                    .GetPtr();
    if (campaign == nullptr) {
        g_chapter_target_states.clear();
        return;
    }

    const MissionRuntimeState runtime_state = GetMissionRuntimeState();
    const bool use_runtime_state =
            g_chapter_runtime_state_allowed &&
            runtime_state.campaign_index == g_selected_campaign &&
            runtime_state.chapter_index == g_selected_chapter &&
            runtime_state.chapter_active;
    const bool animate_last_win =
            frontline_animation_requested &&
            use_runtime_state &&
            runtime_state.mission_won &&
            !runtime_state.mission_id.empty();

    g_chapter_target_states.assign(
            chapter->missionPath.size(),
            kChapterTargetDisabled);
    const std::vector<int> initial_reinforcement_types =
            InitialChapterReinforcementTypes(chapter);
    for (size_t index = 0; index < chapter->missionPath.size(); ++index) {
        const NDb::SMapInfo* mission =
                chapter->missionPath[index].pMap.GetPtr();
        if (mission == nullptr) {
            continue;
        }
        const std::string mission_id =
                ToStdString(mission->GetDBID().ToString());
        if (use_runtime_state &&
            (ContainsResourceId(
                     runtime_state.completed_mission_ids,
                     mission_id) ||
             ContainsResourceId(
                     runtime_state.won_mission_ids,
                     mission_id))) {
            g_chapter_target_states[index] =
                    kChapterTargetCompleted;
            if (animate_last_win &&
                runtime_state.mission_id == mission_id) {
                g_chapter_frontline_animation_mission =
                        static_cast<int>(index);
            }
        } else if (use_runtime_state &&
                   ContainsResourceId(
                           runtime_state.enabled_mission_ids,
                           mission_id)) {
            g_chapter_target_states[index] =
                    kChapterTargetEnabled;
        } else if (!use_runtime_state &&
                   chapter->missionPath[index].nMissionsToEnable <= 0 &&
                   InitialMissionReinforcementsSatisfied(
                           chapter,
                           mission,
                           initial_reinforcement_types)) {
            g_chapter_target_states[index] =
                    kChapterTargetEnabled;
        }
    }

    int recommended_target = -1;
    int recommended_order = 0;
    for (size_t index = 1; index < chapter->missionPath.size(); ++index) {
        if (g_chapter_target_states[index] !=
            kChapterTargetEnabled) {
            continue;
        }
        const int order = chapter->missionPath[index].nRecommendedOrder;
        if (recommended_target < 0 || order < recommended_order) {
            recommended_target = static_cast<int>(index);
            recommended_order = order;
        }
    }
    if (recommended_target < 0) {
        for (size_t index = 0; index < g_chapter_target_states.size();
             ++index) {
            if (g_chapter_target_states[index] ==
                kChapterTargetEnabled) {
                recommended_target = static_cast<int>(index);
                break;
            }
        }
    }
    if (recommended_target >= 0) {
        g_chapter_target_states[
                static_cast<size_t>(recommended_target)] =
                kChapterTargetRecommended;
    }

    const bool chapter_changed =
            g_chapter_selection_campaign != g_selected_campaign ||
            g_chapter_selection_chapter != g_selected_chapter;
    const bool selected_invalid =
            g_selected_mission < 0 ||
            static_cast<size_t>(g_selected_mission) >=
                    g_chapter_target_states.size() ||
            g_chapter_target_states[
                    static_cast<size_t>(g_selected_mission)] ==
                    kChapterTargetCompleted;
    if (chapter_changed || selected_invalid) {
        g_selected_mission =
                recommended_target >= 0 ? recommended_target : 0;
    }
    g_chapter_selection_campaign = g_selected_campaign;
    g_chapter_selection_chapter = g_selected_chapter;

    g_caption_overrides[
            "ChapterMapMain/ChapterMapLeft/ChapterName"] =
            LoadUtf16Text(NormalizeResourcePath(
                    ToStdString(chapter->szLocalizedNameFileRef)));

    const NDb::SMissionEnableInfo& selected =
            chapter->missionPath[
                    static_cast<size_t>(g_selected_mission)];
    const NDb::SMapInfo* selected_map = selected.pMap.GetPtr();
    g_caption_overrides[
            "ChapterMapMain/ChapterMapRight/MissionName"] =
            selected_map == nullptr
                    ? std::string()
                    : LoadUtf16Text(NormalizeResourcePath(
                              ToStdString(
                                      selected_map
                                              ->szLocalizedNameFileRef)));

    const ChapterTargetState selected_state =
            g_chapter_target_states[
                    static_cast<size_t>(g_selected_mission)];
    const bool mission_playable =
            selected_state == kChapterTargetEnabled ||
            selected_state == kChapterTargetRecommended;
    g_path_visibility_overrides[
            "ChapterMapMain/ChapterMapRight/MissionEnabledLight"] =
            mission_playable;
    g_path_enabled_overrides["ChapterMapMain/PlayButton"] =
            mission_playable;

    const bool final_mission = g_selected_mission == 0;
    g_path_visibility_overrides[
            "ChapterMapMain/ChapterMapRight/FinalBonus"] =
            final_mission;
    g_path_visibility_overrides[
            "ChapterMapMain/ChapterMapRight/BonusGrid"] =
            !final_mission;
    if (final_mission && campaign->pTextureChapterFinishBonus) {
        g_path_texture_overrides[
                "ChapterMapMain/ChapterMapRight/FinalBonus"] =
                campaign->pTextureChapterFinishBonus.GetPtr();
    }

    const NDb::SUIConstsB2* ui_consts = NGameX::GetUIConsts();
    if (ui_consts != nullptr) {
        for (const NDb::SReinfButton& button :
             ui_consts->reinfButtons) {
            const std::string button_name = ReinfButtonName(&button);
            if (button_name.empty()) {
                continue;
            }
            const std::string button_path =
                    "ChapterMapMain/ChapterMapRight/"
                    "ReinforcementGrid/" +
                    button_name;
            HideReinfButtonDetails(button_path);
            g_path_enabled_overrides[button_path] = false;
            g_path_button_state_overrides[button_path] = 2;
        }
    }

    if (use_runtime_state) {
        for (const MissionReinforcementState& reinforcement_state :
             runtime_state.chapter_reinforcements) {
            const NDb::SReinforcement* reinforcement =
                    reinforcement_state.dbid.empty()
                            ? nullptr
                            : NDb::Get<NDb::SReinforcement>(
                                      CDBID(reinforcement_state.dbid.c_str()));
            BindChapterReinforcement(
                    reinforcement_state.type,
                    reinforcement,
                    reinforcement_state.state);
            if (reinforcement != nullptr &&
                reinforcement_state.state == 2) {
                g_chapter_reinforcements[
                        reinforcement_state.type] =
                        reinforcement;
            }
        }
        g_chapter_calls_available = std::max(
                0,
                runtime_state.chapter_reinforcement_calls_left);
    } else {
        if (!chapter->basePlayerReinforcements.empty()) {
            for (const CDBPtr<NDb::SReinforcement>& reinforcement_ptr :
                 chapter->basePlayerReinforcements[0].reinforcements) {
                const NDb::SReinforcement* reinforcement =
                        reinforcement_ptr.GetPtr();
                if (reinforcement == nullptr) {
                    continue;
                }
                BindChapterReinforcement(
                        static_cast<int>(reinforcement->eType),
                        reinforcement,
                        2);
                g_chapter_reinforcements[
                        static_cast<int>(reinforcement->eType)] =
                        reinforcement;
            }
        }
        g_chapter_calls_available =
                std::max(0, chapter->nReinforcementCalls);
    }
    g_chapter_calls_for_selected_mission = std::min(
            std::max(0, selected.nRecommendedCalls),
            g_chapter_calls_available);

    const int chapter_calls_for_selection = std::max(
            0,
            g_chapter_calls_available -
                    g_chapter_calls_for_selected_mission);
    if (animate_last_win) {
        // StepLocal restarts the chapter counter when the frontline begins,
        // rolling the saved pre-mission value to the new chapter total.
        g_chapter_roller_transition.prepared = true;
        g_chapter_roller_transition.reason =
                ChapterRollerReason::kPostWin;
        g_chapter_roller_transition.chapter_from = std::max(
                0,
                runtime_state.chapter_reinforcement_calls_old);
        g_chapter_roller_transition.chapter_to =
                g_chapter_calls_available;
        g_chapter_roller_transition.mission_from =
                g_chapter_calls_for_selected_mission;
        g_chapter_roller_transition.mission_to =
                g_chapter_calls_for_selected_mission;
        g_chapter_roller_transition.delay_millis =
                kChapterRollerDelayMillis;
    } else if (roller_selection_requested) {
        // SelectTarget rolls both counters in opposite directions while
        // preserving their sum at the chapter's available call count.
        g_chapter_roller_transition.prepared = true;
        g_chapter_roller_transition.reason =
                ChapterRollerReason::kSelection;
        g_chapter_roller_transition.chapter_from =
                roller_chapter_from;
        g_chapter_roller_transition.chapter_to =
                chapter_calls_for_selection;
        g_chapter_roller_transition.mission_from =
                roller_mission_from;
        g_chapter_roller_transition.mission_to =
                g_chapter_calls_for_selected_mission;
    } else if (chapter_changed) {
        // InitRoller starts every digit at zero before InitMissions selects
        // the recommended target.
        g_chapter_roller_transition.prepared = true;
        g_chapter_roller_transition.reason =
                ChapterRollerReason::kEntry;
        g_chapter_roller_transition.chapter_to =
                chapter_calls_for_selection;
        g_chapter_roller_transition.mission_to =
                g_chapter_calls_for_selected_mission;
    }

    for (size_t slot = 0; slot < 4; ++slot) {
        const std::string button_path =
                "ChapterMapMain/ChapterMapRight/BonusGrid/"
                "ButtonBonus0" +
                std::to_string(slot + 1);
        HideReinfButtonDetails(button_path);
        g_path_button_state_overrides[button_path] = 2;
        if (final_mission || slot >= selected.reward.size()) {
            continue;
        }
        const NDb::SChapterBonus* bonus =
                selected.reward[slot].GetPtr();
        if (bonus == nullptr ||
            bonus->eBonusType != NDb::CBT_REINF_CHANGE ||
            bonus->bApplyToEnemy ||
            !bonus->pReinforcementSet) {
            continue;
        }
        const NDb::SReinforcement* reinforcement =
                bonus->pReinforcementSet.GetPtr();
        const NDb::SReinfButton* button =
                ReinfButtonForType(
                        static_cast<int>(reinforcement->eType));
        const NDb::STexture* texture =
                ReinfIconTexture(reinforcement, button, false);
        g_path_button_state_overrides[button_path] = 0;
        g_path_visibility_overrides[button_path + "/Icon"] = true;
        if (texture != nullptr) {
            g_path_texture_overrides[button_path + "/Icon"] = texture;
        }
    }

    ApplyChapterReinfCompositionBindingsLocked();

    std::ostringstream report;
    report << "original_menu_chapter_state="
           << (use_runtime_state ? "tracker" : "new_campaign")
           << "; campaign=" << g_selected_campaign
           << "; chapter=" << g_selected_chapter
           << "; selected=" << g_selected_mission
           << "; recommended=" << recommended_target
           << "; calls=" << g_chapter_calls_available
           << "; mission_calls="
           << g_chapter_calls_for_selected_mission;
    for (size_t index = 0;
         index < g_chapter_target_states.size();
         ++index) {
        report << "; target" << index << "="
               << static_cast<int>(g_chapter_target_states[index]);
    }
    PlatformRuntime::instance().log_info(report.str());
}

// CInterfaceChapterMapMenu sets the map artwork on the window it looks up as
// "ChapterMap"; the descriptor ships that window with no texture of its own.
// CInterfaceMissionBackground sits behind every menu screen. It shows a live
// map when the root names one, and the shipped picture otherwise; a phone has
// no business running a mission behind a menu, so the picture is what this
// draws.
void ResolveMenuBackgroundLocked() {
    g_menu_background_path.clear();
    const NDb::SGameRoot* root = NGameX::GetGameRoot();
    if (root == nullptr || !root->mainMenuBackground.pPicture) {
        return;
    }
    const NDb::STexture* picture = root->mainMenuBackground.pPicture.GetPtr();
    g_menu_background_path = TexturePath(picture);
    if (g_menu_background_path.empty()) {
        return;
    }
    // The picture is the shipped 1024x768 menu art stored in a texture padded
    // out to a power of two, so drawing the whole texture would show the
    // padding as a black band. The art covers the virtual screen exactly, at
    // one texel per virtual pixel.
    const float texture_width =
            static_cast<float>(std::max(picture->nWidth, 1));
    const float texture_height =
            static_cast<float>(std::max(picture->nHeight, 1));
    g_menu_background_u =
            std::min(kVirtualScreenWidth / texture_width, 1.0f);
    g_menu_background_v =
            std::min(kVirtualScreenHeight / texture_height, 1.0f);
    std::ostringstream report;
    report << "original_menu_background=" << g_menu_background_path
           << "; texture=" << texture_width << "x" << texture_height
           << "; uv=" << g_menu_background_u << ","
           << g_menu_background_v;
    PlatformRuntime::instance().log_info(report.str());
}

void ApplyScreenInitTexturesLocked(const std::string& screen_ref) {
    if (screen_ref != "UI/Game/Menu/ChapterMap_WindowScreen.xdb") {
        return;
    }
    const NDb::SChapter* chapter = SelectedChapter();
    if (chapter == nullptr || !chapter->pMapPicture) {
        return;
    }
    const NDb::STexture* picture = chapter->pMapPicture.GetPtr();
    if (picture == nullptr) {
        return;
    }
    g_texture_overrides["ChapterMap"] = picture;
    std::ostringstream report;
    report << "original_menu_chapter_map=" << TexturePath(picture)
           << "; campaign=" << g_selected_campaign
           << "; missions=" << chapter->missionPath.size();
    PlatformRuntime::instance().log_info(report.str());
}

bool LoadOriginalMissionBriefingScreen(int mission_index) {
    const NDb::SChapter* chapter = SelectedChapter();
    if (chapter == nullptr ||
        mission_index < 0 ||
        static_cast<size_t>(mission_index) >=
                chapter->missionPath.size()) {
        return false;
    }
    const NDb::SMapInfo* mission =
            chapter->missionPath[
                    static_cast<size_t>(mission_index)].pMap.GetPtr();
    if (mission == nullptr) {
        return false;
    }

    constexpr const char* kScreenRef =
            "UI/Game/Menu/MissionBriefing/"
            "MissionBriefing_WindowScreen.xdb";
    std::lock_guard<std::mutex> guard(g_menu_mutex);
    g_selected_mission = mission_index;
    g_chapter_reinf_dialog_mode =
            ChapterReinfDialogMode::kNone;
    g_visibility_overrides.clear();
    g_path_visibility_overrides.clear();
    g_path_enabled_overrides.clear();
    g_path_button_state_overrides.clear();
    g_caption_overrides.clear();
    g_progress_overrides.clear();
    g_texture_overrides.clear();
    g_path_texture_overrides.clear();

    std::string mission_name = LoadUtf16BriefingText(
            NormalizeResourcePath(
                    ToStdString(mission->szLocalizedNameFileRef)));
    if (mission_name.empty()) {
        mission_name = LoadUtf16Text(
                "UI/Game/Menu/MissionBriefing/DefaultHeader.txt");
    }
    g_caption_overrides["Main/TopPanel/HeaderView"] =
            mission_name;
    if (mission->pMiniMap && mission->pMiniMap->pTexture) {
        g_path_texture_overrides[
                "Main/RightTopPanel/Minimap"] =
                mission->pMiniMap->pTexture.GetPtr();
    }
    ResolveMenuBackgroundLocked();
    g_options_category = 0;
    const bool loaded = RebuildMenuScreenLocked(kScreenRef);
    if (loaded) {
        PlatformRuntime::instance().log_info(
                ScreenLayoutReportLocked());
    }
    return loaded;
}

bool LoadOriginalMenuScreen(const std::string& screen_ref) {
    std::lock_guard<std::mutex> guard(g_menu_mutex);
    if (screen_ref !=
        "UI/Game/Menu/ChapterMap_WindowScreen.xdb") {
        g_chapter_reinf_dialog_mode =
                ChapterReinfDialogMode::kNone;
    }
    g_visibility_overrides.clear();
    g_path_visibility_overrides.clear();
    g_path_enabled_overrides.clear();
    g_path_button_state_overrides.clear();
    g_caption_overrides.clear();
    g_progress_overrides.clear();
    g_texture_overrides.clear();
    g_path_texture_overrides.clear();
    ApplyScreenInitVisibilityLocked(screen_ref);
    ApplyScreenInitTexturesLocked(screen_ref);
    ApplyCampaignSelectionBindingsLocked(screen_ref);
    ApplyChapterMapBindingsLocked(screen_ref);
    ResolveMenuBackgroundLocked();
    g_options_category = 0;
    return RebuildMenuScreenLocked(screen_ref);
}

std::string FormatMissionStatisticsTime(int seconds) {
    seconds = std::max(seconds, 0);
    std::ostringstream text;
    const int hours = seconds / 3600;
    const int minutes = (seconds / 60) % 60;
    if (hours > 0) {
        text << hours << "h : ";
    }
    if (hours > 0 || minutes > 0) {
        text << minutes << "m : ";
    }
    text << (seconds % 60) << "s";
    return text.str();
}

bool LoadOriginalMissionStatisticsScreen() {
    const LegacyMissionStatisticsSnapshot statistics =
            CopyLegacyMissionStatisticsSnapshot();
    if (!statistics.available) {
        PlatformRuntime::instance().log_warn(
                "original_mission_statistics=failed; error=no_snapshot");
        return false;
    }

    constexpr const char* kScreenRef =
            "UI/Game/Menu/SingleStatictics2/"
            "SingleStatistics2_WindowScreen.xdb";
    std::lock_guard<std::mutex> guard(g_menu_mutex);
    g_visibility_overrides.clear();
    g_path_visibility_overrides.clear();
    g_path_enabled_overrides.clear();
    g_path_button_state_overrides.clear();
    g_caption_overrides.clear();
    g_progress_overrides.clear();
    g_texture_overrides.clear();
    g_path_texture_overrides.clear();
    g_screen_stack.clear();

    g_path_visibility_overrides["Main/InfoPanel/MissionSuccessLabel"] =
            statistics.won;
    g_path_visibility_overrides["Main/InfoPanel/MissionFailedLabel"] =
            !statistics.won;
    g_path_visibility_overrides["Main/BottomPanel/NextBtn"] =
            statistics.won;
    g_path_visibility_overrides["Main/BottomPanel/RestartMissionBtn"] =
            !statistics.won;
    g_path_visibility_overrides["Main/BottomPanel/ExitToMainMenuBtn"] =
            !statistics.won && statistics.custom;
    g_path_visibility_overrides["Main/BottomPanel/ExitToChapterBtn"] =
            !statistics.won && !statistics.custom;
    // The generic save browser is not yet wired to resume the finished
    // legacy mission, so exposing Load here would lead to a dead end.
    g_path_visibility_overrides["Main/BottomPanel/LoadBtn"] = false;

    g_path_visibility_overrides["MedalDlg"] = false;
    g_path_visibility_overrides["NewRankDlg"] = false;
    g_path_visibility_overrides["ReinfDescriptionBackground"] = false;
    g_path_visibility_overrides["DlgBg"] = false;
    g_path_visibility_overrides["BlackBg"] = false;

    g_path_visibility_overrides["Main/InfoPanel/CareerExpNAView"] =
            statistics.custom;
    g_path_visibility_overrides["Main/InfoPanel/MissionExpNAView"] =
            statistics.custom;
    g_path_visibility_overrides["Main/InfoPanel/CareerProgress"] =
            !statistics.custom;
    g_path_visibility_overrides["Main/InfoPanel/ExpProgress"] =
            !statistics.custom;
    g_path_visibility_overrides["Main/RewardPanel/NewReinfLabel"] =
            !statistics.custom;

    for (int index = 0; index < 3; ++index) {
        const std::string line =
                "Line0" + std::to_string(index + 1);
        const std::string player_path =
                "Main/InfoPanel/" + line;
        const bool has_player =
                static_cast<size_t>(index) < statistics.players.size();
        g_path_visibility_overrides[player_path] = has_player;
        if (!has_player) {
            continue;
        }
        const LegacyMissionStatisticsPlayer& player =
                statistics.players[static_cast<size_t>(index)];
        std::string player_name;
        if (!player.name_ref.empty()) {
            player_name = LoadUtf16Text(
                    NormalizeResourcePath(player.name_ref));
        }
        if (player_name.empty()) {
            player_name = player.name;
        }
        g_caption_overrides[player_path + "/NameView"] = player_name;
        g_caption_overrides[player_path + "/UnitsLostView"] =
                std::to_string(player.units_lost);
        g_caption_overrides[player_path + "/UnitsKilledView"] =
                std::to_string(player.units_killed);
        g_caption_overrides[player_path + "/ResevedView"] =
                std::to_string(player.reinforcements_called);
        if (!player.statistics_icon_ref.empty()) {
            const NDb::STexture* icon =
                    NDb::Get<NDb::STexture>(
                            CDBID(player.statistics_icon_ref.c_str()));
            if (icon != nullptr) {
                g_path_texture_overrides[player_path + "/Flag"] =
                        icon;
            }
        }
    }
    const std::string reward_text_root =
            "UI/Game/Menu/SingleStatictics2/ReinfLine/";
    const std::string new_reinforcement_prefix =
            LoadUtf16Text(
                    reward_text_root + "PrefixNewReinf.txt");
    const std::string upgrade_prefix =
            LoadUtf16Text(
                    reward_text_root + "PrefixUpgrade.txt");
    for (int index = 0; index < 4; ++index) {
        const std::string reward_path =
                "Main/RewardPanel/Line0" +
                std::to_string(index + 1);
        const bool has_reward =
                static_cast<size_t>(index) <
                statistics.rewards.size();
        g_path_visibility_overrides[reward_path] = has_reward;
        if (!has_reward) {
            continue;
        }
        const LegacyMissionStatisticsReward& reward =
                statistics.rewards[static_cast<size_t>(index)];
        std::string name =
                LoadUtf16Text(
                        NormalizeResourcePath(reward.name_ref));
        g_caption_overrides[reward_path + "/BlockBtn/NameView"] =
                (reward.upgrade ? upgrade_prefix
                                : new_reinforcement_prefix) +
                name;
        if (reward.icon_texture != nullptr) {
            g_path_texture_overrides[
                    reward_path + "/BlockBtn/Icon"] =
                    reward.icon_texture;
        }
    }

    g_caption_overrides["Main/InfoPanel/MissionTimeView"] =
            FormatMissionStatisticsTime(
                    statistics.mission_time_seconds);
    g_caption_overrides["Main/InfoPanel/TotalCampaignTimeView"] =
            FormatMissionStatisticsTime(
                    statistics.custom
                            ? statistics.mission_time_seconds
                            : statistics.campaign_time_seconds);
    const int rank_start = std::max(
            0,
            statistics.campaign_experience_current -
                    statistics.experience_earned);
    const int rank_span = std::max(
            0,
            statistics.campaign_experience_next_level - rank_start);
    const int rank_progress = std::max(
            0,
            statistics.campaign_experience_current - rank_start);
    g_caption_overrides["Main/InfoPanel/ExpForNextRankView"] =
            std::to_string(rank_progress) + "/" +
            std::to_string(rank_span);
    // One shipped descriptor accidentally points its visible label at the
    // two-line tooltip file; the desktop text widget displays only the first
    // line, while the Android glyph path otherwise draws both on one row.
    g_caption_overrides["Main/InfoPanel/UnitsLostLabel"] =
            "Units Lost";

    const float rank_current =
            statistics.campaign_experience_next_level > 0
                    ? static_cast<float>(
                              statistics.campaign_experience_current) /
                              static_cast<float>(
                                      statistics
                                              .campaign_experience_next_level)
                    : 0.0f;
    const float rank_previous =
            statistics.campaign_experience_next_level > 0
                    ? static_cast<float>(std::max(
                              0,
                              statistics.campaign_experience_current -
                                      statistics.experience_earned)) /
                              static_cast<float>(
                                      statistics
                                              .campaign_experience_next_level)
                    : 0.0f;
    const float career_current =
            statistics.campaign_experience_max > 0
                    ? static_cast<float>(
                              statistics.campaign_experience_absolute) /
                              static_cast<float>(
                                      statistics.campaign_experience_max)
                    : 0.0f;
    const float career_previous =
            statistics.campaign_experience_max > 0
                    ? static_cast<float>(std::max(
                              0,
                              statistics.campaign_experience_absolute -
                                      statistics.experience_earned)) /
                              static_cast<float>(
                                      statistics.campaign_experience_max)
                    : 0.0f;
    g_progress_overrides["Main/InfoPanel/CareerProgress"] =
            career_previous;
    g_progress_overrides[
            "Main/InfoPanel/CareerProgress/NewCareerProgress"] =
            career_current;
    g_progress_overrides["Main/InfoPanel/ExpProgress"] =
            rank_previous;
    g_progress_overrides[
            "Main/InfoPanel/ExpProgress/NewExpProgress"] =
            rank_current;

    if (statistics.campaign_index >= 0) {
        g_selected_campaign = statistics.campaign_index;
    }
    if (statistics.chapter_index >= 0) {
        g_selected_chapter = statistics.chapter_index;
    }
    ResolveMenuBackgroundLocked();
    g_options_category = 0;
    const bool loaded = RebuildMenuScreenLocked(kScreenRef);
    std::ostringstream report;
    report << "original_mission_statistics="
           << (loaded ? "ready" : "failed")
           << "; won=" << (statistics.won ? "true" : "false")
           << "; custom=" << (statistics.custom ? "true" : "false")
           << "; campaign=" << statistics.campaign_index
           << "; chapter=" << statistics.chapter_index
           << "; players=" << statistics.players.size()
           << "; rewards=" << statistics.rewards.size()
           << "; mission_time=" << statistics.mission_time_seconds
           << "; campaign_time=" << statistics.campaign_time_seconds
           << "; experience=" << statistics.experience_earned;
    if (!loaded) {
        report << "; error=" << g_error;
        PlatformRuntime::instance().log_warn(report.str());
    } else {
        PlatformRuntime::instance().log_info(report.str());
    }
    return loaded;
}

namespace {

// Options that cannot mean anything on a phone: the surface size is fixed by
// the device, the Android renderer has no 16-bit or fixed-function path, the
// shadow-map and anisotropic settings drive desktop-only code, and there is no
// keyboard to rebind or mouse to invert.
bool IsDesktopOnlyOption(const std::string& program_name) {
    static const char* const kDesktopOnly[] = {
            "gfx_resolution",
            "gfx_16bit_mode",
            "gfx_tnl_mode",
            "gfx_anisotropic_filter",
            "gfx_depth_tex_resolution",
            "mission_buttons_bind_section",
            "game_camera_mouse_pitch_invert",
            "game_camera_mouse_zoom_invert",
            "game_camera_mouse_sensetivity",
    };
    for (const char* name : kDesktopOnly) {
        if (program_name == name) {
            return true;
        }
    }
    return false;
}

// CInterfaceOptionsMenu::FillScreen builds this screen at runtime by cloning
// template windows, so the same construction is reproduced here from the
// shipped SOptionSystem data.
void PopulateOptionsScreenLocked() {
    const std::map<std::string, MenuTemplate>::const_iterator button =
            g_templates.find("CategoryButton");
    const std::map<std::string, MenuTemplate>::const_iterator offset =
            g_templates.find("CategoryButtonOffset");
    const std::map<std::string, MenuTemplate>::const_iterator list =
            g_templates.find("OptionList");
    if (button == g_templates.end() ||
        offset == g_templates.end() ||
        list == g_templates.end()) {
        PlatformRuntime::instance().log_info(
                "original_menu_options=no_templates");
        return;
    }
    const NDb::SGameRoot* root = NGameX::GetGameRoot();
    if (root == nullptr || !root->pGameOptions) {
        PlatformRuntime::instance().log_info(
                "original_menu_options=no_game_options");
        return;
    }
    const NDb::SOptionSystem* options = root->pGameOptions.GetPtr();
    if (options == nullptr) {
        PlatformRuntime::instance().log_info(
                "original_menu_options=options_unloaded");
        return;
    }
    // The desktop screen derives its category step from the offset template.
    const float step_x = offset->second.x - button->second.x;
    const float step_y = offset->second.y - button->second.y;
    constexpr int kOptionModeMask = 0x07;

    std::vector<int> visible_categories;
    for (size_t index = 0; index < options->categories.size(); ++index) {
        for (const NDb::SOptionSystem::SOptionsCategory::SOptionEntry& entry :
             options->categories[index].options) {
            if ((entry.nModeFlags & kOptionModeMask) &&
                !IsDesktopOnlyOption(ToStdString(entry.szProgName))) {
                visible_categories.push_back(static_cast<int>(index));
                break;
            }
        }
    }
    if (visible_categories.empty()) {
        PlatformRuntime::instance().log_info(
                std::string("original_menu_options=no_categories; total=") +
                std::to_string(options->categories.size()));
        return;
    }
    {
        std::ostringstream report;
        report << "original_menu_options=ready; categories="
               << visible_categories.size()
               << "/" << options->categories.size()
               << "; selected=" << g_options_category;
        PlatformRuntime::instance().log_info(report.str());
    }
    g_options_category = std::clamp(
            g_options_category,
            0,
            static_cast<int>(visible_categories.size()) - 1);

    for (size_t slot = 0; slot < visible_categories.size(); ++slot) {
        const NDb::SOptionSystem::SOptionsCategory& category =
                options->categories[
                        static_cast<size_t>(visible_categories[slot])];
        const float x =
                button->second.x + step_x * static_cast<float>(slot);
        const float y =
                button->second.y + step_y * static_cast<float>(slot);
        MenuWindowNode node;
        node.name = "OptionCategory" + std::to_string(slot);
        node.type = "WindowMSButton";
        node.x = x;
        node.y = y;
        node.width = button->second.width;
        node.height = button->second.height;
        node.visible = true;
        node.enabled = true;
        node.button = true;
        node.action = "option_category_" + std::to_string(slot);
        SplitMarkupTags(
                LoadUtf16Text(NormalizeResourcePath(
                        ToStdString(category.szNameFileRef))),
                &node.caption,
                &node.text_format);

        const NDb::SWindowMSButtonShared* button_shared =
                dynamic_cast<const NDb::SWindowMSButtonShared*>(
                        button->second.shared);
        if (button_shared != nullptr &&
            !button_shared->visualStates.empty()) {
            const bool selected =
                    static_cast<int>(slot) == g_options_category;
            const NDb::SButtonVisualState& state =
                    button_shared->visualStates[0];
            AppendBackgroundQuads(
                    selected && state.pushed.pBackground
                            ? state.pushed.pBackground.GetPtr()
                            : state.normal.pBackground.GetPtr(),
                    x,
                    y,
                    node.width,
                    node.height);
            const size_t pressed_begin = g_quads.size();
            AppendBackgroundQuads(
                    state.pushed.pBackground.GetPtr(),
                    x,
                    y,
                    node.width,
                    node.height);
            AppendTextQuads(
                    node.caption,
                    node.text_format,
                    x,
                    y,
                    node.width,
                    node.height);
            node.pressed_quad_begin =
                    static_cast<int>(g_pressed_quads.size());
            g_pressed_quads.insert(
                    g_pressed_quads.end(),
                    g_quads.begin() + pressed_begin,
                    g_quads.end());
            node.pressed_quad_end =
                    static_cast<int>(g_pressed_quads.size());
            g_quads.resize(pressed_begin);
        }
        AppendTextQuads(
                node.caption,
                node.text_format,
                x,
                y,
                node.width,
                node.height);
        ++g_button_count;
        g_nodes.push_back(node);
    }

    // Rows of the selected category, laid out down the option list panel.
    const NDb::SOptionSystem::SOptionsCategory& category =
            options->categories[static_cast<size_t>(
                    visible_categories[
                            static_cast<size_t>(g_options_category)])];
    const char* const kRowTemplates[] = {
            "EditLineTemplate",
            "CheckBoxTemplate",
            "SliderTemplate",
            "MultichoiceTemplate",
            "EditNumberTemplate"};
    float row_y = list->second.y + 12.0f;
    for (const NDb::SOptionSystem::SOptionsCategory::SOptionEntry& entry :
         category.options) {
        if (!(entry.nModeFlags & kOptionModeMask) ||
            IsDesktopOnlyOption(ToStdString(entry.szProgName))) {
            continue;
        }
        const size_t editor = static_cast<size_t>(entry.eEditorType);
        const std::map<std::string, MenuTemplate>::const_iterator row =
                g_templates.find(
                        editor < 5 ? kRowTemplates[editor]
                                   : kRowTemplates[0]);
        if (row == g_templates.end()) {
            continue;
        }
        const float row_height = row->second.height;
        if (row_y + row_height >
            list->second.y + list->second.height) {
            break;
        }
        // Keep the template's internal name/control split, shifted onto
        // this row.
        const float shift_y = row_y - row->second.y;
        if (row->second.shared != nullptr) {
            AppendBackgroundQuads(
                    row->second.shared->pBackground.GetPtr(),
                    row->second.x,
                    row_y,
                    row->second.width,
                    row_height);
        }

        std::string name;
        std::string name_format;
        SplitMarkupTags(
                LoadUtf16Text(NormalizeResourcePath(
                        ToStdString(entry.szNameFileRef))),
                &name,
                &name_format);
        const std::map<std::string, MenuTemplate>::const_iterator label =
                g_templates.find("OptionName");
        const std::map<std::string, MenuTemplate>::const_iterator control =
                g_templates.find("OptionControl");
        if (label != g_templates.end()) {
            AppendTextQuads(
                    name,
                    name_format,
                    label->second.x,
                    label->second.y + shift_y,
                    label->second.width,
                    label->second.height,
                    "body");
        }

        // The live value comes from the same global variable the desktop
        // options screen reads.
        const std::string value = std::string(
                NStr::ToMBCS(NGlobal::GetVar(
                        entry.szProgName,
                        entry.szDefaultValue)).c_str());
        std::string shown = value;
        for (const NDb::SOptionSystem::SOptionsCategory::SOptionEntry::
                     SOptionEntryState& state : entry.states) {
            if (ToStdString(state.szValue) != value) {
                continue;
            }
            std::string state_text;
            std::string state_format;
            SplitMarkupTags(
                    LoadUtf16Text(NormalizeResourcePath(
                            ToStdString(state.szNameFileRef))),
                    &state_text,
                    &state_format);
            if (!state_text.empty()) {
                shown = state_text;
            }
            break;
        }
        if (control != g_templates.end()) {
            AppendTextQuads(
                    shown,
                    name_format,
                    control->second.x,
                    control->second.y + shift_y,
                    control->second.width,
                    control->second.height,
                    "body");
        }
        // The row is a touch target: tapping it advances the option to its
        // next shipped state, or flips a checkbox.
        MenuWindowNode node;
        node.name = ToStdString(entry.szProgName);
        node.type = "OptionRow";
        node.x = row->second.x;
        node.y = row_y;
        node.width = row->second.width;
        node.height = row_height;
        node.visible = true;
        node.enabled = true;
        node.button = true;
        node.action = "option_cycle_" + node.name;
        node.pressed_quad_begin = static_cast<int>(g_pressed_quads.size());
        node.pressed_quad_end = node.pressed_quad_begin + 1;
        // Reuse the row background as the pressed presentation so the hit
        // test accepts the row; the visual stays the shipped one.
        if (row->second.shared != nullptr) {
            const size_t begin = g_quads.size();
            AppendBackgroundQuads(
                    row->second.shared->pBackground.GetPtr(),
                    row->second.x,
                    row_y,
                    row->second.width,
                    row_height);
            g_pressed_quads.insert(
                    g_pressed_quads.end(),
                    g_quads.begin() + begin,
                    g_quads.end());
            node.pressed_quad_end =
                    static_cast<int>(g_pressed_quads.size());
            g_quads.resize(begin);
        }
        ++g_button_count;
        g_nodes.push_back(node);

        row_y += row_height + 4.0f;
    }
}

struct ChapterPotentialNode {
    float x = 0.0f;
    float y = 0.0f;
    float end_x = 0.0f;
    float end_y = 0.0f;
    float value = 0.0f;
};

struct ChapterPotentialAnimationState {
    int width = 0;
    int height = 0;
    int source_width = 0;
    int source_height = 0;
    int mission_index = -1;
    float strike_x = 0.0f;
    float strike_y = 0.0f;
    float value_from = 0.0f;
    float value_to = 0.0f;
    uint64_t elapsed_millis = 0;
    uint64_t delay_millis = 50;
    uint64_t last_tick_millis = 0;
    uint64_t last_upload_millis = 0;
    bool started = false;
    NGfx::SPixel8888 border;
    std::vector<NGfx::SPixel8888> mask;
    std::vector<NGfx::SPixel8888> territory_colour;
    std::vector<ChapterPotentialNode> nodes;
    std::vector<float> base_potential;
    std::vector<float> animated_weight;
};

std::unique_ptr<ChapterPotentialAnimationState>
        g_chapter_potential_animation;

float ChapterPotentialNodeWeight(
        const ChapterPotentialNode& node,
        float x,
        float y) {
    float along =
            node.end_x * (x - node.x) +
            node.end_y * (y - node.y);
    const float end_length_squared =
            node.end_x * node.end_x +
            node.end_y * node.end_y;
    if (end_length_squared > 0.0001f) {
        along = std::clamp(
                along / end_length_squared,
                0.0f,
                1.0f);
    } else {
        along = 0.0f;
    }
    const float dx = node.x + node.end_x * along - x;
    const float dy = node.y + node.end_y * along - y;
    return 1.0f / (1.0f + dx * dx + dy * dy);
}

std::string ResolveChapterLayerPath(
        const NDb::SChapter* chapter,
        const string& file_ref) {
    if (chapter == nullptr || file_ref.empty()) {
        return std::string();
    }
    const std::string raw = ToStdString(file_ref);
    const bool root_relative =
            !raw.empty() && (raw.front() == '/' || raw.front() == '\\');
    const std::string normalized = NormalizeResourcePath(raw);
    if (root_relative || normalized.empty()) {
        return normalized;
    }
    std::string folder = NormalizeResourcePath(ToStdString(
            NDb::GetFolderName(chapter->GetDBID())));
    while (!folder.empty() && folder.back() == '/') {
        folder.pop_back();
    }
    if (!folder.empty() &&
        normalized.size() > folder.size() &&
        normalized.compare(0, folder.size(), folder) == 0 &&
        normalized[folder.size()] == '/') {
        return normalized;
    }
    return folder.empty() ? normalized : folder + "/" + normalized;
}

bool LoadChapterTga(
        const std::string& path,
        CArray2D<NGfx::SPixel8888>* pixels) {
    if (path.empty() ||
        pixels == nullptr ||
        NVFS::GetMainVFS() == nullptr) {
        return false;
    }
    CFileStream stream(NVFS::GetMainVFS(), string(path.c_str()));
    const int byte_count = stream.GetSize();
    const BYTE* bytes = stream.GetBuffer();
    if (!stream.IsOk() || byte_count < 18 || bytes == nullptr) {
        return false;
    }
    const int id_length = bytes[0];
    const int color_map_type = bytes[1];
    const int image_type = bytes[2];
    const int width = bytes[12] | (bytes[13] << 8);
    const int height = bytes[14] | (bytes[15] << 8);
    const int pixel_depth = bytes[16];
    const int descriptor = bytes[17];
    if (color_map_type != 0 ||
        (image_type != 2 && image_type != 10) ||
        (pixel_depth != 24 && pixel_depth != 32) ||
        width <= 0 ||
        height <= 0) {
        return false;
    }
    const int bytes_per_pixel = pixel_depth / 8;
    size_t offset = static_cast<size_t>(18 + id_length);
    const size_t total_pixels =
            static_cast<size_t>(width) * height;
    pixels->SetSizes(width, height);
    size_t decoded_pixels = 0;
    const auto decode_pixel = [&](const BYTE* source) {
        const size_t file_y =
                decoded_pixels / static_cast<size_t>(width);
        const size_t file_x =
                decoded_pixels % static_cast<size_t>(width);
        const int output_x = (descriptor & 0x10) != 0
                ? width - 1 - static_cast<int>(file_x)
                : static_cast<int>(file_x);
        const int output_y = (descriptor & 0x20) != 0
                ? static_cast<int>(file_y)
                : height - 1 - static_cast<int>(file_y);
        (*pixels)[output_y][output_x] = NGfx::SPixel8888(
                source[2],
                source[1],
                source[0],
                bytes_per_pixel == 4 ? source[3] : 0xff);
        ++decoded_pixels;
    };
    if (image_type == 2) {
        const size_t required =
                total_pixels * static_cast<size_t>(bytes_per_pixel);
        if (offset > static_cast<size_t>(byte_count) ||
            required > static_cast<size_t>(byte_count) - offset) {
            pixels->Clear();
            return false;
        }
        while (decoded_pixels < total_pixels) {
            decode_pixel(bytes + offset);
            offset += static_cast<size_t>(bytes_per_pixel);
        }
        return true;
    }

    while (decoded_pixels < total_pixels &&
           offset < static_cast<size_t>(byte_count)) {
        const BYTE packet = bytes[offset++];
        const size_t count =
                static_cast<size_t>((packet & 0x7f) + 1);
        if (count > total_pixels - decoded_pixels) {
            pixels->Clear();
            return false;
        }
        if ((packet & 0x80) != 0) {
            if (offset + static_cast<size_t>(bytes_per_pixel) >
                static_cast<size_t>(byte_count)) {
                pixels->Clear();
                return false;
            }
            const BYTE* source = bytes + offset;
            offset += static_cast<size_t>(bytes_per_pixel);
            for (size_t index = 0; index < count; ++index) {
                decode_pixel(source);
            }
        } else {
            const size_t packet_bytes =
                    count * static_cast<size_t>(bytes_per_pixel);
            if (offset + packet_bytes >
                static_cast<size_t>(byte_count)) {
                pixels->Clear();
                return false;
            }
            for (size_t index = 0; index < count; ++index) {
                decode_pixel(
                        bytes + offset +
                        index * static_cast<size_t>(bytes_per_pixel));
            }
            offset += packet_bytes;
        }
    }
    if (decoded_pixels != total_pixels) {
        pixels->Clear();
        return false;
    }
    return true;
}

NGfx::SPixel8888 BlendChapterPixel(
        const NGfx::SPixel8888& source,
        const NGfx::SPixel8888& destination) {
    const float source_alpha =
            static_cast<float>(source.a) / 255.0f;
    const float destination_alpha =
            static_cast<float>(destination.a) / 255.0f;
    const float output_alpha =
            source_alpha +
            destination_alpha * (1.0f - source_alpha);
    if (output_alpha <= 0.0001f) {
        return NGfx::SPixel8888(0, 0, 0, 0);
    }
    const auto channel = [&](BYTE source_value, BYTE destination_value) {
        const float premultiplied =
                static_cast<float>(source_value) * source_alpha +
                static_cast<float>(destination_value) *
                        destination_alpha *
                        (1.0f - source_alpha);
        return static_cast<BYTE>(std::clamp(
                premultiplied / output_alpha,
                0.0f,
                255.0f));
    };
    return NGfx::SPixel8888(
            channel(source.r, destination.r),
            channel(source.g, destination.g),
            channel(source.b, destination.b),
            static_cast<BYTE>(std::clamp(
                    output_alpha * 255.0f,
                    0.0f,
                    255.0f)));
}

float ChapterPotentialAt(
        const ChapterPotentialAnimationState& state,
        int x,
        int y,
        float animated_value) {
    if (x >= 0 && y >= 0 && x < state.width && y < state.height) {
        const size_t index =
                static_cast<size_t>(y) * state.width + x;
        return state.base_potential[index] +
                (state.mission_index >= 0
                         ? animated_value *
                                   state.animated_weight[index]
                         : 0.0f);
    }

    float value = 0.0f;
    if (x >= 0 &&
        y >= 0 &&
        x < state.source_width &&
        y < state.source_height) {
        const NGfx::SPixel8888& mask_pixel =
                state.mask[
                        static_cast<size_t>(y) *
                                state.source_width +
                        x];
        if (mask_pixel.a != 0xff && mask_pixel.a != 0x00) {
            value =
                    (static_cast<float>(mask_pixel.a) - 127.0f) *
                    0.00001f;
        }
        const float x_gradient =
                state.strike_x *
                (static_cast<float>(
                         x + x - state.source_width) /
                 static_cast<float>(state.source_width));
        const float y_gradient =
                -state.strike_y *
                (static_cast<float>(
                         y + y - state.source_height) /
                 static_cast<float>(state.source_height));
        value += (x_gradient + y_gradient) * 0.007f;
    }
    for (size_t index = 0; index < state.nodes.size(); ++index) {
        const ChapterPotentialNode& node = state.nodes[index];
        const float node_value =
                static_cast<int>(index) == state.mission_index
                        ? animated_value
                        : node.value;
        value += node_value *
                ChapterPotentialNodeWeight(
                        node,
                        static_cast<float>(x),
                        static_cast<float>(y));
    }
    return value;
}

void BuildChapterPotentialBase(
        ChapterPotentialAnimationState* state,
        float strike_x,
        float strike_y) {
    if (state == nullptr) {
        return;
    }
    state->strike_x = strike_x;
    state->strike_y = strike_y;
    const size_t pixel_count =
            static_cast<size_t>(state->width) * state->height;
    state->base_potential.assign(pixel_count, 0.0f);
    if (state->mission_index >= 0) {
        state->animated_weight.assign(pixel_count, 0.0f);
    } else {
        state->animated_weight.clear();
    }
    for (int y = 0; y < state->height; ++y) {
        for (int x = 0; x < state->width; ++x) {
            const size_t pixel_index =
                    static_cast<size_t>(y) * state->width + x;
            const NGfx::SPixel8888& mask_pixel =
                    state->mask[
                            static_cast<size_t>(y) *
                                    state->source_width +
                            x];
            float value = 0.0f;
            if (mask_pixel.a != 0xff && mask_pixel.a != 0x00) {
                value =
                        (static_cast<float>(mask_pixel.a) - 127.0f) *
                        0.00001f;
            }
            const float x_gradient =
                    strike_x *
                    (static_cast<float>(
                             x + x - state->source_width) /
                     static_cast<float>(state->source_width));
            const float y_gradient =
                    -strike_y *
                    (static_cast<float>(
                             y + y - state->source_height) /
                     static_cast<float>(state->source_height));
            value += (x_gradient + y_gradient) * 0.007f;

            for (size_t node_index = 0;
                 node_index < state->nodes.size();
                 ++node_index) {
                const float weight = ChapterPotentialNodeWeight(
                        state->nodes[node_index],
                        static_cast<float>(x),
                        static_cast<float>(y));
                if (static_cast<int>(node_index) ==
                    state->mission_index) {
                    state->animated_weight[pixel_index] = weight;
                } else {
                    value +=
                            state->nodes[node_index].value * weight;
                }
            }
            state->base_potential[pixel_index] = value;
        }
    }
}

std::vector<NGfx::SPixel8888> RasterizeChapterPotential(
        const ChapterPotentialAnimationState& state,
        float animated_value,
        size_t* territory_pixels,
        size_t* border_pixels) {
    std::vector<NGfx::SPixel8888> overlay(
            static_cast<size_t>(state.width) * state.height,
            NGfx::SPixel8888(0, 0, 0, 0));
    size_t territory_count = 0;
    for (int y = 0; y < state.height; ++y) {
        for (int x = 0; x < state.width; ++x) {
            const size_t source_index =
                    static_cast<size_t>(y) *
                            state.source_width +
                    x;
            const NGfx::SPixel8888& mask_pixel =
                    state.mask[source_index];
            if (mask_pixel.a == 0xff ||
                ChapterPotentialAt(
                        state,
                        x,
                        y,
                        animated_value) > 0.0f) {
                continue;
            }
            NGfx::SPixel8888 pixel =
                    state.territory_colour[source_index];
            pixel.a = 0xff;
            overlay[static_cast<size_t>(y) * state.width + x] =
                    pixel;
            ++territory_count;
        }
    }

    std::vector<NGfx::SPixel8888> lines(
            static_cast<size_t>(state.width) * state.height,
            NGfx::SPixel8888(0, 0, 0, 0));
    constexpr int kGridSize = 10;
    constexpr float kBorderRadius = 5.0f;
    constexpr float kBlurPart = 0.3f;
    const auto draw_circle = [&](int center_x, int center_y) {
        if (center_x < 0 ||
            center_y < 0 ||
            center_x >= state.width ||
            center_y >= state.height ||
            state.mask[
                    static_cast<size_t>(center_y) *
                            state.source_width +
                    center_x].a == 0xff) {
            return;
        }
        const int first_x =
                static_cast<int>(center_x - kBorderRadius);
        const int last_x =
                static_cast<int>(center_x + kBorderRadius);
        const int first_y =
                static_cast<int>(center_y - kBorderRadius);
        const int last_y =
                static_cast<int>(center_y + kBorderRadius);
        for (int x = first_x; x < last_x; ++x) {
            for (int y = first_y; y < last_y; ++y) {
                if (x < 0 ||
                    y < 0 ||
                    x >= state.width ||
                    y >= state.height) {
                    continue;
                }
                const float horizontal =
                        std::fabs(
                                static_cast<float>(
                                        x - center_x));
                const float vertical =
                        std::fabs(
                                static_cast<float>(
                                        y - center_y));
                const float distance = std::min(
                        std::max(horizontal, vertical),
                        (horizontal + vertical) * 0.666f);
                if (distance >= kBorderRadius) {
                    continue;
                }
                BYTE alpha = state.border.a;
                if (distance > kBorderRadius * kBlurPart) {
                    alpha = static_cast<BYTE>(
                            static_cast<float>(alpha) *
                            (kBorderRadius - distance) /
                            (kBorderRadius *
                             (1.0f - kBlurPart)));
                }
                NGfx::SPixel8888& pixel =
                        lines[
                                static_cast<size_t>(y) *
                                        state.width +
                                x];
                const DWORD old_alpha = pixel.a;
                pixel = state.border;
                pixel.a =
                        std::max<DWORD>(old_alpha, alpha);
            }
        }
    };
    const auto draw_line =
            [&](float x0, float y0, float x1, float y1) {
                const float dx = x1 - x0;
                const float dy = y1 - y0;
                const int steps = std::max(
                        1,
                        static_cast<int>(std::ceil(
                                std::max(
                                        std::fabs(dx),
                                        std::fabs(dy)))));
                for (int step = 0; step <= steps; ++step) {
                    const float part =
                            static_cast<float>(step) /
                            static_cast<float>(steps);
                    draw_circle(
                            static_cast<int>(std::lround(
                                    x0 + dx * part)),
                            static_cast<int>(std::lround(
                                    y0 + dy * part)));
                }
            };

    for (int y = 0; y < state.height; y += kGridSize) {
        for (int x = 0; x < state.width; x += kGridSize) {
            const float values[4] = {
                    ChapterPotentialAt(
                            state,
                            x + kGridSize,
                            y + kGridSize,
                            animated_value),
                    ChapterPotentialAt(
                            state,
                            x + kGridSize,
                            y,
                            animated_value),
                    ChapterPotentialAt(
                            state,
                            x,
                            y,
                            animated_value),
                    ChapterPotentialAt(
                            state,
                            x,
                            y + kGridSize,
                            animated_value),
            };
            BYTE type = 0;
            for (float value : values) {
                type = static_cast<BYTE>(
                        (type << 1) |
                        (value > 0.0f ? 1 : 0));
            }
            const auto part = [&](int first, int second) {
                const float denominator =
                        values[first] - values[second];
                if (std::fabs(denominator) <= 0.000001f) {
                    return kGridSize * 0.5f;
                }
                return kGridSize *
                        std::fabs(
                                values[first] / denominator);
            };
            switch (type) {
                case 1:
                case 14:
                    draw_line(
                            static_cast<float>(x),
                            y + part(2, 3),
                            x + part(3, 0),
                            static_cast<float>(y + kGridSize));
                    break;
                case 2:
                case 13:
                    draw_line(
                            static_cast<float>(x),
                            y + part(2, 3),
                            x + part(2, 1),
                            static_cast<float>(y));
                    break;
                case 3:
                case 12:
                    draw_line(
                            x + part(2, 1),
                            static_cast<float>(y),
                            x + part(3, 0),
                            static_cast<float>(y + kGridSize));
                    break;
                case 4:
                case 11:
                    draw_line(
                            x + part(2, 1),
                            static_cast<float>(y),
                            static_cast<float>(x + kGridSize),
                            y + part(1, 0));
                    break;
                case 5:
                case 10:
                    draw_line(
                            static_cast<float>(x),
                            y + part(2, 3),
                            x + part(3, 0),
                            static_cast<float>(y + kGridSize));
                    draw_line(
                            x + part(2, 1),
                            static_cast<float>(y),
                            static_cast<float>(x + kGridSize),
                            y + part(1, 0));
                    break;
                case 6:
                case 9:
                    draw_line(
                            static_cast<float>(x),
                            y + part(2, 3),
                            static_cast<float>(x + kGridSize),
                            y + part(1, 0));
                    break;
                case 7:
                case 8:
                    draw_line(
                            x + part(3, 0),
                            static_cast<float>(y + kGridSize),
                            static_cast<float>(x + kGridSize),
                            y + part(1, 0));
                    break;
                default:
                    break;
            }
        }
    }

    size_t border_count = 0;
    for (size_t index = 0; index < overlay.size(); ++index) {
        if (lines[index].a == 0) {
            continue;
        }
        overlay[index] =
                BlendChapterPixel(lines[index], overlay[index]);
        ++border_count;
    }
    if (territory_pixels != nullptr) {
        *territory_pixels = territory_count;
    }
    if (border_pixels != nullptr) {
        *border_pixels = border_count;
    }
    return overlay;
}

void UploadChapterPotential(
        const std::vector<NGfx::SPixel8888>& overlay,
        int width,
        int height) {
    if (!IsValid(g_chapter_potential_texture) ||
        overlay.size() <
                static_cast<size_t>(width) * height) {
        return;
    }
    NGfx::CTextureLock<NGfx::SPixel8888> lock(
            g_chapter_potential_texture,
            0,
            NGfx::INPLACE);
    for (int y = 0; y < height; ++y) {
        std::copy_n(
                overlay.data() +
                        static_cast<size_t>(y) * width,
                width,
                lock[y]);
    }
}

// CWindowPotentialLines builds two generated layers over the chapter picture:
// a differently coloured negative-potential territory and its blurred zero
// contour. Generate the same layers into one Android texture; selected mission
// arrows and target markers are submitted afterward.
void AppendChapterMapPotential(
        const NDb::SChapter* chapter,
        const NDb::SMapInfo* details,
        const MenuTemplate& map_window,
        float details_scale_x,
        float details_scale_y) {
    g_chapter_potential_texture = nullptr;
    g_chapter_potential_animation.reset();
    if (chapter == nullptr ||
        details == nullptr ||
        map_window.width <= 1.0f ||
        map_window.height <= 1.0f ||
        details_scale_x <= 0.0f ||
        details_scale_y <= 0.0f) {
        return;
    }

    const std::string mask_path = ResolveChapterLayerPath(
            chapter,
            chapter->szSeaNoiseMask);
    const std::string colour_path = ResolveChapterLayerPath(
            chapter,
            chapter->szDifferentColourMap);
    CArray2D<NGfx::SPixel8888> mask;
    CArray2D<NGfx::SPixel8888> territory_colour;
    if (!LoadChapterTga(mask_path, &mask) ||
        !LoadChapterTga(colour_path, &territory_colour)) {
        PlatformRuntime::instance().log_info(
                "original_menu_chapter_frontline=not_ready; "
                "error=layer_tga_unreadable; mask=" +
                mask_path + "; colour=" + colour_path);
        return;
    }

    const int width = std::max(
            1,
            std::min(
                    static_cast<int>(std::lround(map_window.width)),
                    std::min(mask.GetSizeX(), territory_colour.GetSizeX())));
    const int height = std::max(
            1,
            std::min(
                    static_cast<int>(std::lround(map_window.height)),
                    std::min(mask.GetSizeY(), territory_colour.GetSizeY())));
    if (width <= 1 || height <= 1) {
        return;
    }

    std::unique_ptr<ChapterPotentialAnimationState> animation_state =
            std::make_unique<ChapterPotentialAnimationState>();
    animation_state->width = width;
    animation_state->height = height;
    animation_state->source_width =
            std::min(mask.GetSizeX(), territory_colour.GetSizeX());
    animation_state->source_height =
            std::min(mask.GetSizeY(), territory_colour.GetSizeY());

    std::vector<ChapterPotentialNode> nodes;
    nodes.reserve(chapter->missionPath.size());
    size_t completed_nodes = 0;
    for (size_t mission_index = 0;
         mission_index < chapter->missionPath.size();
         ++mission_index) {
        const NDb::SMissionEnableInfo& mission =
                chapter->missionPath[mission_index];
        ChapterPotentialNode node;
        node.x = mission.vPlaceOnChapterMap.x;
        node.y = mission.vPlaceOnChapterMap.y;
        for (const NDb::SMapObjectInfo& object : details->objects) {
            if (object.nPlayer == static_cast<int>(mission_index)) {
                node.x = object.vPos.y * details_scale_x;
                node.y = object.vPos.x * details_scale_y;
                break;
            }
        }
        node.end_x = mission.vEndOffset.x;
        node.end_y = mission.vEndOffset.y;
        const bool completed =
                mission_index < g_chapter_target_states.size() &&
                g_chapter_target_states[mission_index] ==
                        kChapterTargetCompleted;
        node.value = completed
                ? mission.fPotentialComplete
                : mission.fPotentialIncomplete;
        if (completed) {
            ++completed_nodes;
        }
        if (completed &&
            static_cast<int>(mission_index) ==
                    g_chapter_frontline_animation_mission) {
            animation_state->mission_index =
                    static_cast<int>(mission_index);
            animation_state->value_from =
                    mission.fPotentialIncomplete;
            animation_state->value_to =
                    mission.fPotentialComplete;
            node.value = animation_state->value_from;
        }
        nodes.push_back(node);
    }

    float strike_x = 0.0f;
    float strike_y = 0.0f;
    for (const NDb::SMapObjectInfo& object : details->objects) {
        if (object.nPlayer == 0) {
            const float radians =
                    chapter->fMainStrikeAngle *
                    3.14159265358979323846f / 180.0f;
            strike_x = std::cos(radians) * chapter->fMainStrikePower;
            strike_y = std::sin(radians) * chapter->fMainStrikePower;
            break;
        }
    }
    if (animation_state->mission_index >= 0) {
        animation_state->mask.resize(
                static_cast<size_t>(
                        animation_state->source_width) *
                animation_state->source_height);
        animation_state->territory_colour.resize(
                animation_state->mask.size());
        for (int y = 0;
             y < animation_state->source_height;
             ++y) {
            for (int x = 0;
                 x < animation_state->source_width;
                 ++x) {
                const size_t index =
                        static_cast<size_t>(y) *
                                animation_state->source_width +
                        x;
                animation_state->mask[index] = mask[y][x];
                animation_state->territory_colour[index] =
                        territory_colour[y][x];
            }
        }
        animation_state->border = NGfx::SPixel8888(
                static_cast<DWORD>(
                        chapter->nPositiveColour));
        animation_state->nodes = nodes;
        BuildChapterPotentialBase(
                animation_state.get(),
                strike_x,
                strike_y);
    }

    const auto potential_at = [&](int x, int y) {
        float value = 0.0f;
        if (x >= 0 &&
            y >= 0 &&
            x < mask.GetSizeX() &&
            y < mask.GetSizeY()) {
            const NGfx::SPixel8888& mask_pixel = mask[y][x];
            if (mask_pixel.a != 0xff && mask_pixel.a != 0x00) {
                value =
                        (static_cast<float>(mask_pixel.a) - 127.0f) *
                        0.00001f;
            }
            const float x_gradient =
                    strike_x *
                    (static_cast<float>(x + x - mask.GetSizeX()) /
                     static_cast<float>(mask.GetSizeX()));
            const float y_gradient =
                    -strike_y *
                    (static_cast<float>(y + y - mask.GetSizeY()) /
                     static_cast<float>(mask.GetSizeY()));
            value += (x_gradient + y_gradient) * 0.007f;
        }
        for (const ChapterPotentialNode& node : nodes) {
            float along =
                    node.end_x * (static_cast<float>(x) - node.x) +
                    node.end_y * (static_cast<float>(y) - node.y);
            const float end_length_squared =
                    node.end_x * node.end_x +
                    node.end_y * node.end_y;
            if (end_length_squared > 0.0001f) {
                along = std::clamp(
                        along / end_length_squared,
                        0.0f,
                        1.0f);
            } else {
                along = 0.0f;
            }
            const float dx =
                    node.x + node.end_x * along -
                    static_cast<float>(x);
            const float dy =
                    node.y + node.end_y * along -
                    static_cast<float>(y);
            value += node.value / (1.0f + dx * dx + dy * dy);
        }
        return value;
    };

    std::vector<NGfx::SPixel8888> overlay(
            static_cast<size_t>(width) * height,
            NGfx::SPixel8888(0, 0, 0, 0));
    size_t territory_pixels = 0;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const NGfx::SPixel8888& mask_pixel = mask[y][x];
            if (mask_pixel.a == 0xff || potential_at(x, y) > 0.0f) {
                continue;
            }
            NGfx::SPixel8888 pixel = territory_colour[y][x];
            pixel.a = 0xff;
            overlay[static_cast<size_t>(y) * width + x] = pixel;
            ++territory_pixels;
        }
    }

    std::vector<NGfx::SPixel8888> lines(
            static_cast<size_t>(width) * height,
            NGfx::SPixel8888(0, 0, 0, 0));
    const NGfx::SPixel8888 border(
            static_cast<DWORD>(chapter->nPositiveColour));
    constexpr int kGridSize = 10;
    constexpr float kBorderRadius = 5.0f;
    constexpr float kBlurPart = 0.3f;
    const auto draw_circle = [&](int center_x, int center_y) {
        if (center_x < 0 ||
            center_y < 0 ||
            center_x >= width ||
            center_y >= height ||
            mask[center_y][center_x].a == 0xff) {
            return;
        }
        const int first_x =
                static_cast<int>(center_x - kBorderRadius);
        const int last_x =
                static_cast<int>(center_x + kBorderRadius);
        const int first_y =
                static_cast<int>(center_y - kBorderRadius);
        const int last_y =
                static_cast<int>(center_y + kBorderRadius);
        for (int x = first_x; x < last_x; ++x) {
            for (int y = first_y; y < last_y; ++y) {
                if (x < 0 || y < 0 || x >= width || y >= height) {
                    continue;
                }
                const float horizontal =
                        std::fabs(static_cast<float>(x - center_x));
                const float vertical =
                        std::fabs(static_cast<float>(y - center_y));
                const float distance = std::min(
                        std::max(horizontal, vertical),
                        (horizontal + vertical) * 0.666f);
                if (distance >= kBorderRadius) {
                    continue;
                }
                BYTE alpha = border.a;
                if (distance > kBorderRadius * kBlurPart) {
                    alpha = static_cast<BYTE>(
                            static_cast<float>(alpha) *
                            (kBorderRadius - distance) /
                            (kBorderRadius * (1.0f - kBlurPart)));
                }
                NGfx::SPixel8888& pixel =
                        lines[static_cast<size_t>(y) * width + x];
                const DWORD old_alpha = pixel.a;
                pixel = border;
                pixel.a = std::max<DWORD>(old_alpha, alpha);
            }
        }
    };
    const auto draw_line =
            [&](float x0, float y0, float x1, float y1) {
                const float dx = x1 - x0;
                const float dy = y1 - y0;
                const int steps = std::max(
                        1,
                        static_cast<int>(std::ceil(
                                std::max(std::fabs(dx), std::fabs(dy)))));
                for (int step = 0; step <= steps; ++step) {
                    const float part =
                            static_cast<float>(step) /
                            static_cast<float>(steps);
                    draw_circle(
                            static_cast<int>(std::lround(x0 + dx * part)),
                            static_cast<int>(std::lround(y0 + dy * part)));
                }
            };

    for (int y = 0; y < height; y += kGridSize) {
        for (int x = 0; x < width; x += kGridSize) {
            const float values[4] = {
                    potential_at(x + kGridSize, y + kGridSize),
                    potential_at(x + kGridSize, y),
                    potential_at(x, y),
                    potential_at(x, y + kGridSize),
            };
            BYTE type = 0;
            for (float value : values) {
                type = static_cast<BYTE>((type << 1) |
                        (value > 0.0f ? 1 : 0));
            }
            const auto part = [&](int first, int second) {
                const float denominator =
                        values[first] - values[second];
                if (std::fabs(denominator) <= 0.000001f) {
                    return kGridSize * 0.5f;
                }
                return kGridSize *
                        std::fabs(values[first] / denominator);
            };
            switch (type) {
                case 1:
                case 14:
                    draw_line(
                            static_cast<float>(x),
                            y + part(2, 3),
                            x + part(3, 0),
                            static_cast<float>(y + kGridSize));
                    break;
                case 2:
                case 13:
                    draw_line(
                            static_cast<float>(x),
                            y + part(2, 3),
                            x + part(2, 1),
                            static_cast<float>(y));
                    break;
                case 3:
                case 12:
                    draw_line(
                            x + part(2, 1),
                            static_cast<float>(y),
                            x + part(3, 0),
                            static_cast<float>(y + kGridSize));
                    break;
                case 4:
                case 11:
                    draw_line(
                            x + part(2, 1),
                            static_cast<float>(y),
                            static_cast<float>(x + kGridSize),
                            y + part(1, 0));
                    break;
                case 5:
                case 10:
                    draw_line(
                            static_cast<float>(x),
                            y + part(2, 3),
                            x + part(3, 0),
                            static_cast<float>(y + kGridSize));
                    draw_line(
                            x + part(2, 1),
                            static_cast<float>(y),
                            static_cast<float>(x + kGridSize),
                            y + part(1, 0));
                    break;
                case 6:
                case 9:
                    draw_line(
                            static_cast<float>(x),
                            y + part(2, 3),
                            static_cast<float>(x + kGridSize),
                            y + part(1, 0));
                    break;
                case 7:
                case 8:
                    draw_line(
                            x + part(3, 0),
                            static_cast<float>(y + kGridSize),
                            static_cast<float>(x + kGridSize),
                            y + part(1, 0));
                    break;
                default:
                    break;
            }
        }
    }

    size_t border_pixels = 0;
    for (size_t index = 0; index < overlay.size(); ++index) {
        if (lines[index].a == 0) {
            continue;
        }
        overlay[index] = BlendChapterPixel(lines[index], overlay[index]);
        ++border_pixels;
    }

    g_chapter_potential_texture = NGfx::MakeTexture(
            width,
            height,
            1,
            NGfx::SPixel8888::ID,
            NGfx::DYNAMIC_TEXTURE,
            NGfx::CLAMP);
    if (!IsValid(g_chapter_potential_texture)) {
        return;
    }
    {
        NGfx::CTextureLock<NGfx::SPixel8888> lock(
                g_chapter_potential_texture,
                0,
                NGfx::INPLACE);
        for (int y = 0; y < height; ++y) {
            std::copy_n(
                    overlay.data() + static_cast<size_t>(y) * width,
                    width,
                    lock[y]);
        }
    }

    MenuTexturedQuad quad;
    quad.x0 = map_window.x;
    quad.y0 = map_window.y;
    quad.x1 = map_window.x;
    quad.y1 = map_window.y + map_window.height;
    quad.x2 = map_window.x + map_window.width;
    quad.y2 = map_window.y + map_window.height;
    quad.x3 = map_window.x + map_window.width;
    quad.y3 = map_window.y;
    quad.direct_texture = g_chapter_potential_texture.GetPtr();
    g_textured_quads.push_back(quad);

    std::ostringstream report;
    report << "original_menu_chapter_frontline=ready"
           << "; size=" << width << "x" << height
           << "; nodes=" << nodes.size()
           << "; completed=" << completed_nodes
           << "; territory_pixels=" << territory_pixels
           << "; border_pixels=" << border_pixels
           << "; animation_mission="
           << animation_state->mission_index
           << "; animation_from="
           << animation_state->value_from
           << "; animation_to="
           << animation_state->value_to
           << "; mask=" << mask_path
           << "; colour=" << colour_path;
    PlatformRuntime::instance().log_info(report.str());

    if (animation_state->mission_index >= 0 &&
        std::fabs(
                animation_state->value_to -
                animation_state->value_from) > 0.000001f) {
        animation_state->last_tick_millis =
                PlatformRuntime::instance().monotonic_millis();
        g_chapter_potential_animation =
                std::move(animation_state);
    }
}

void UpdateChapterMapPotentialAnimationLocked() {
    if (!g_chapter_potential_animation ||
        !IsValid(g_chapter_potential_texture)) {
        return;
    }
    ChapterPotentialAnimationState& state =
            *g_chapter_potential_animation;
    const uint64_t now =
            PlatformRuntime::instance().monotonic_millis();
    if (PlatformRuntime::instance().lifecycle_state() !=
        LifecycleState::Focused) {
        state.last_tick_millis = now;
        return;
    }
    const uint64_t delta = std::min<uint64_t>(
            now - state.last_tick_millis,
            100);
    state.last_tick_millis = now;
    uint64_t animation_delta = delta;
    if (state.delay_millis > 0) {
        const uint64_t consumed =
                std::min(state.delay_millis, animation_delta);
        state.delay_millis -= consumed;
        animation_delta -= consumed;
        if (state.delay_millis > 0) {
            return;
        }
    }
    state.elapsed_millis = std::min<uint64_t>(
            5000,
            state.elapsed_millis + animation_delta);
    if (state.elapsed_millis < 5000 &&
        state.elapsed_millis - state.last_upload_millis < 50) {
        return;
    }
    state.last_upload_millis = state.elapsed_millis;
    const float progress =
            static_cast<float>(state.elapsed_millis) / 5000.0f;
    const float value =
            state.value_from +
            (state.value_to - state.value_from) * progress;
    const std::vector<NGfx::SPixel8888> overlay =
            RasterizeChapterPotential(
                    state,
                    value,
                    nullptr,
                    nullptr);
    UploadChapterPotential(
            overlay,
            state.width,
            state.height);
    if (!state.started) {
        state.started = true;
        std::ostringstream report;
        report << "original_menu_chapter_frontline_animation=started"
               << "; mission=" << state.mission_index
               << "; duration_ms=5000"
               << "; from=" << state.value_from
               << "; to=" << state.value_to;
        PlatformRuntime::instance().log_info(report.str());
    }
    if (state.elapsed_millis >= 5000) {
        PlatformRuntime::instance().log_info(
                "original_menu_chapter_frontline_animation=completed; "
                "mission=" +
                std::to_string(state.mission_index) +
                "; duration_ms=5000");
        g_chapter_potential_animation.reset();
    }
}

// CWindowPotentialLines::DrawArrows maps every road assigned to the selected
// mission into a strip of rotated quads. The arrow texture is consumed exactly
// once along the complete polyline, preserving its authored head and tail.
void AppendChapterMapArrows(
        const NDb::SChapter* chapter,
        const NDb::SMapInfo* details,
        const MenuTemplate& map_window,
        float details_scale_x,
        float details_scale_y) {
    if (chapter == nullptr ||
        details == nullptr ||
        g_selected_mission < 0 ||
        details_scale_x <= 0.0f ||
        details_scale_y <= 0.0f) {
        return;
    }

    constexpr const char* kArrowTexturePaths[] = {
            "UI/chaptermap/arrows/arrow_own.dds",
            "UI/chaptermap/arrows/arrow_enemy.dds",
            "UI/chaptermap/arrows/defence_own.dds",
            "UI/chaptermap/arrows/defence_enemy.dds",
    };
    constexpr float kArrowTextureWidths[] = {
            64.0f, 64.0f, 128.0f, 128.0f};
    constexpr float kArrowTextureHeights[] = {
            128.0f, 128.0f, 256.0f, 256.0f};

    size_t arrow_count = 0;
    size_t segment_count = 0;
    size_t dependent_count = 0;
    size_t fallback_texture_count = 0;
    for (const NDb::SVSOInstance& road : details->roads) {
        if (road.nCMArrowMission != g_selected_mission ||
            road.nCMArrowType < 0 ||
            road.nCMArrowType >= 4 ||
            road.points.size() < 2) {
            continue;
        }
        const size_t texture_index =
                static_cast<size_t>(road.nCMArrowType);
        const NDb::STexture* texture_desc =
                texture_index < chapter->arrowTextures.size()
                        ? chapter->arrowTextures[texture_index].GetPtr()
                        : nullptr;
        const bool texture_descriptor_valid =
                texture_desc != nullptr &&
                !texture_desc->IsRefInvalid();
        std::string texture_path;
        if (texture_descriptor_valid) {
            texture_path = TexturePath(texture_desc);
        }
        if (texture_path.empty()) {
            // The public sparse source omits the four descriptor XDB files,
            // so package their exact final DDS payloads directly.
            texture_path = kArrowTexturePaths[texture_index];
            ++fallback_texture_count;
        }
        const int texture = TextureIndex(texture_path);
        if (texture < 0) {
            continue;
        }
        const float texture_width =
                texture_descriptor_valid && texture_desc->nWidth > 0
                        ? static_cast<float>(texture_desc->nWidth)
                        : kArrowTextureWidths[texture_index];
        const float texture_height =
                texture_descriptor_valid && texture_desc->nHeight > 0
                        ? static_cast<float>(texture_desc->nHeight)
                        : kArrowTextureHeights[texture_index];
        const float texture_length = texture_height - 4.0f;
        if (texture_width <= 1.5f || texture_length <= 0.5f) {
            continue;
        }

        std::vector<float> segment_lengths(
                road.points.size() - 1,
                0.0f);
        std::vector<float> point_x(road.points.size(), 0.0f);
        std::vector<float> point_y(road.points.size(), 0.0f);
        for (size_t point = 0; point < road.points.size(); ++point) {
            // SChapterMapMenuHelper::Map2Screen swaps the map axes.
            point_x[point] =
                    road.points[point].vPos.y * details_scale_x;
            point_y[point] =
                    road.points[point].vPos.x * details_scale_y;
        }
        float arrow_length = 0.0f;
        for (size_t segment = 0;
             segment < segment_lengths.size();
             ++segment) {
            const float dx = point_x[segment + 1] - point_x[segment];
            const float dy = point_y[segment + 1] - point_y[segment];
            segment_lengths[segment] =
                    std::sqrt(dx * dx + dy * dy);
            arrow_length += segment_lengths[segment];
        }
        if (arrow_length <= 0.0001f) {
            continue;
        }

        uint32_t argb = 0xffffffffu;
        if (g_selected_mission == 0 &&
            road.nCMArrowMission2 > 0 &&
            (static_cast<size_t>(road.nCMArrowMission2) >=
                     g_chapter_target_states.size() ||
             g_chapter_target_states[
                     static_cast<size_t>(road.nCMArrowMission2)] !=
                     kChapterTargetCompleted)) {
            argb = 0x40ffffffu;
            ++dependent_count;
        }

        const float half_width =
                road.points[0].fWidth / 8.0f;
        if (half_width <= 0.0f) {
            continue;
        }
        const float texture_scale = texture_length / arrow_length;
        const float u0 = 0.5f / texture_width;
        const float u1 = (texture_width - 1.5f) / texture_width;
        float texture_y = 0.5f;
        float positive_start_x = 0.0f;
        float positive_start_y = 0.0f;
        float negative_start_x = 0.0f;
        float negative_start_y = 0.0f;
        float positive_end_x = 0.0f;
        float positive_end_y = 0.0f;
        float negative_end_x = 0.0f;
        float negative_end_y = 0.0f;
        size_t arrow_segments = 0;
        for (size_t segment = 0;
             segment < segment_lengths.size();
             ++segment) {
            const float length = segment_lengths[segment];
            if (length <= 0.0001f) {
                continue;
            }
            const float dx = point_x[segment + 1] - point_x[segment];
            const float dy = point_y[segment + 1] - point_y[segment];
            const float perpendicular_x = (dy / length) * half_width;
            const float perpendicular_y = (-dx / length) * half_width;
            if (arrow_segments == 0) {
                positive_start_x =
                        point_x[segment] + perpendicular_x;
                positive_start_y =
                        point_y[segment] + perpendicular_y;
                negative_start_x =
                        point_x[segment] - perpendicular_x;
                negative_start_y =
                        point_y[segment] - perpendicular_y;
            } else {
                positive_start_x = positive_end_x;
                positive_start_y = positive_end_y;
                negative_start_x = negative_end_x;
                negative_start_y = negative_end_y;
            }
            positive_end_x =
                    point_x[segment + 1] + perpendicular_x;
            positive_end_y =
                    point_y[segment + 1] + perpendicular_y;
            negative_end_x =
                    point_x[segment + 1] - perpendicular_x;
            negative_end_y =
                    point_y[segment + 1] - perpendicular_y;

            const float segment_texture_length =
                    length * texture_scale;
            MenuTexturedQuad quad;
            quad.x0 = map_window.x + negative_start_x;
            quad.y0 = map_window.y + negative_start_y;
            quad.x1 = map_window.x + negative_end_x;
            quad.y1 = map_window.y + negative_end_y;
            quad.x2 = map_window.x + positive_end_x;
            quad.y2 = map_window.y + positive_end_y;
            quad.x3 = map_window.x + positive_start_x;
            quad.y3 = map_window.y + positive_start_y;
            quad.u0 = u0;
            quad.v0 = std::min(texture_y, texture_length) /
                    texture_height;
            quad.u1 = u1;
            quad.v1 = std::min(
                              texture_y + segment_texture_length,
                              texture_length) /
                    texture_height;
            quad.argb = argb;
            quad.texture = texture;
            g_textured_quads.push_back(quad);
            texture_y += segment_texture_length;
            ++arrow_segments;
            ++segment_count;
        }
        if (arrow_segments > 0) {
            ++arrow_count;
        }
    }

    std::ostringstream report;
    report << "original_menu_chapter_arrows=" << arrow_count
           << "; segments=" << segment_count
           << "; mission=" << g_selected_mission
           << "; dependent=" << dependent_count
           << "; fallback_textures=" << fallback_texture_count;
    PlatformRuntime::instance().log_info(report.str());
}

void SetChapterRollerFrame(MenuQuad* quad, int frame) {
    if (quad == nullptr) {
        return;
    }
    const int frame_count =
            kChapterRollerAtlasColumns *
            kChapterRollerAtlasRows;
    frame = std::clamp(frame, 0, frame_count - 1);
    const int column = frame % kChapterRollerAtlasColumns;
    const int row = frame / kChapterRollerAtlasColumns;
    const float atlas_width = static_cast<float>(
            kChapterRollerAtlasColumns *
            kChapterRollerFrameWidth);
    const float atlas_height = static_cast<float>(
            kChapterRollerAtlasRows *
            kChapterRollerFrameHeight);
    quad->u0 = static_cast<float>(
                       column * kChapterRollerFrameWidth) /
            atlas_width;
    quad->v0 = static_cast<float>(
                       row * kChapterRollerFrameHeight) /
            atlas_height;
    quad->u1 = static_cast<float>(
                       (column + 1) *
                       kChapterRollerFrameWidth) /
            atlas_width;
    quad->v1 = static_cast<float>(
                       (row + 1) *
                       kChapterRollerFrameHeight) /
            atlas_height;
}

ChapterRollerDigitPlayback ChapterRollerDigitFrames(
        int start,
        int end,
        int decimal_place) {
    ChapterRollerDigitPlayback playback;
    int divisor = 1;
    for (int place = 0; place < decimal_place; ++place) {
        divisor *= 10;
    }
    int digit_start = (std::max(start, 0) / divisor) % 10;
    int digit_end = (std::max(end, 0) / divisor) % 10;
    const bool forward = start <= end;
    if (!forward) {
        std::swap(digit_start, digit_end);
    }

    int frame_start = digit_start + 1;
    int frame_end = digit_end + 1;
    if (digit_start > digit_end) {
        frame_end = digit_end + 11;
    }
    if (!forward) {
        frame_start = 40 - frame_start;
        frame_end = 40 - frame_end;
        std::swap(frame_start, frame_end);
    }
    playback.frame_start =
            frame_start * kChapterRollerFramesPerDigit;
    playback.frame_end =
            frame_end * kChapterRollerFramesPerDigit;
    const int duration_seconds =
            (playback.frame_end - playback.frame_start) /
                    kChapterRollerFps +
            1;
    playback.frame_skip = duration_seconds <= 2
            ? 0
            : duration_seconds / 2;
    return playback;
}

void AppendChapterRollerDigit(
        const char* window_name,
        int decimal_place,
        int start,
        int end,
        int texture,
        ChapterRollerAnimationState* animation) {
    const std::map<std::string, MenuTemplate>::const_iterator digit =
            g_templates.find(window_name);
    if (digit == g_templates.end()) {
        return;
    }
    ChapterRollerDigitPlayback playback =
            ChapterRollerDigitFrames(
                    start,
                    end,
                    decimal_place);
    MenuQuad quad;
    quad.x = digit->second.x;
    quad.y = digit->second.y;
    quad.width = digit->second.width;
    quad.height = digit->second.height;
    quad.texture = texture;
    SetChapterRollerFrame(&quad, playback.frame_start);
    playback.quad_index = g_quads.size();
    g_quads.push_back(quad);
    if (animation != nullptr &&
        playback.frame_start != playback.frame_end) {
        animation->digits.push_back(playback);
    }
}

// CWindowPlayer's number strip contains four directional passes through the
// digits. PlayRollerAnim selects a frame range independently for every decimal
// place and caps long transitions with frame skipping. Use those exact ranges
// for the three chapter digits and two selected-mission digits.
void AppendChapterRollers() {
    const int chapter_target = g_chapter_roller_transition.prepared
            ? g_chapter_roller_transition.chapter_to
            : std::max(
                      0,
                      g_chapter_calls_available -
                              g_chapter_calls_for_selected_mission);
    const int mission_target = g_chapter_roller_transition.prepared
            ? g_chapter_roller_transition.mission_to
            : std::max(0, g_chapter_calls_for_selected_mission);
    const int chapter_start = g_chapter_roller_transition.prepared
            ? g_chapter_roller_transition.chapter_from
            : chapter_target;
    const int mission_start = g_chapter_roller_transition.prepared
            ? g_chapter_roller_transition.mission_from
            : mission_target;
    const int texture = TextureIndex(kChapterRollerAtlasPath);

    std::unique_ptr<ChapterRollerAnimationState> animation;
    if (g_chapter_roller_transition.prepared) {
        animation = std::make_unique<
                ChapterRollerAnimationState>();
        animation->delay_millis =
                g_chapter_roller_transition.delay_millis;
        animation->last_tick_millis =
                PlatformRuntime::instance().monotonic_millis();
        animation->chapter_from = chapter_start;
        animation->chapter_to = chapter_target;
        animation->mission_from = mission_start;
        animation->mission_to = mission_target;
        animation->post_win =
                g_chapter_roller_transition.reason ==
                ChapterRollerReason::kPostWin;
    }

    AppendChapterRollerDigit(
            "ReinfQty3",
            2,
            chapter_start,
            chapter_target,
            texture,
            animation.get());
    AppendChapterRollerDigit(
            "ReinfQty2",
            1,
            chapter_start,
            chapter_target,
            texture,
            animation.get());
    AppendChapterRollerDigit(
            "ReinfQty1",
            0,
            chapter_start,
            chapter_target,
            texture,
            animation.get());
    AppendChapterRollerDigit(
            "ReinfMission1",
            1,
            mission_start,
            mission_target,
            texture,
            animation.get());
    AppendChapterRollerDigit(
            "ReinfMission2",
            0,
            mission_start,
            mission_target,
            texture,
            animation.get());

    if (animation && !animation->digits.empty()) {
        g_chapter_roller_animation = std::move(animation);
    } else {
        g_chapter_roller_animation.reset();
    }
    std::ostringstream report;
    report << "original_menu_chapter_rollers=ready"
           << "; atlas=" << kChapterRollerAtlasPath
           << "; chapter=" << chapter_start
           << "->" << chapter_target
           << "; mission=" << mission_start
           << "->" << mission_target
           << "; animated_digits="
           << (g_chapter_roller_animation
                       ? g_chapter_roller_animation->digits.size()
                       : 0)
           << "; reason="
           << ChapterRollerReasonName(
                      g_chapter_roller_transition.reason);
    PlatformRuntime::instance().log_info(report.str());
    g_chapter_roller_transition =
            ChapterRollerTransition();
}

void UpdateChapterRollerAnimationLocked() {
    if (!g_chapter_roller_animation) {
        return;
    }
    ChapterRollerAnimationState& state =
            *g_chapter_roller_animation;
    const uint64_t now =
            PlatformRuntime::instance().monotonic_millis();
    if (PlatformRuntime::instance().lifecycle_state() !=
        LifecycleState::Focused) {
        state.last_tick_millis = now;
        return;
    }
    if (now >= state.last_tick_millis) {
        state.elapsed_millis += now - state.last_tick_millis;
    }
    state.last_tick_millis = now;
    if (state.elapsed_millis < state.delay_millis) {
        return;
    }
    const uint64_t playback_millis =
            state.elapsed_millis - state.delay_millis;
    if (!state.started) {
        state.started = true;
        std::ostringstream report;
        report << "original_menu_chapter_roller_animation=started"
               << "; chapter=" << state.chapter_from
               << "->" << state.chapter_to
               << "; mission=" << state.mission_from
               << "->" << state.mission_to
               << "; post_win="
               << (state.post_win ? "true" : "false");
        PlatformRuntime::instance().log_info(report.str());
    }

    const uint64_t displayed_frames =
            playback_millis * kChapterRollerFps / 1000;
    bool completed = true;
    for (const ChapterRollerDigitPlayback& digit :
         state.digits) {
        if (digit.quad_index >= g_quads.size()) {
            continue;
        }
        const int frame_step = std::max(
                1,
                digit.frame_skip + 1);
        const uint64_t advanced =
                displayed_frames *
                static_cast<uint64_t>(frame_step);
        const int frame = static_cast<int>(std::min<uint64_t>(
                static_cast<uint64_t>(digit.frame_end),
                static_cast<uint64_t>(digit.frame_start) +
                        advanced));
        SetChapterRollerFrame(
                &g_quads[digit.quad_index],
                frame);
        completed = completed && frame >= digit.frame_end;
    }
    if (!completed) {
        return;
    }
    std::ostringstream report;
    report << "original_menu_chapter_roller_animation=completed"
           << "; chapter=" << state.chapter_to
           << "; mission=" << state.mission_to;
    PlatformRuntime::instance().log_info(report.str());
    g_chapter_roller_animation.reset();
}

void BindChapterReinfCompositionActionsLocked() {
    size_t branch_actions = 0;
    size_t unit_actions = 0;
    for (MenuWindowNode& node : g_nodes) {
        for (const auto& reinforcement :
             g_chapter_reinforcements) {
            const NDb::SReinfButton* button =
                    ReinfButtonForType(reinforcement.first);
            const std::string button_name =
                    ReinfButtonName(button);
            if (!button_name.empty() &&
                node.path ==
                        "ChapterMapMain/ChapterMapRight/"
                        "ReinforcementGrid/" +
                                button_name) {
                node.action =
                        "show_reinf_composition_" +
                        std::to_string(reinforcement.first);
                ++branch_actions;
            }
        }
        if (node.button &&
            node.path ==
                    "ReinfDescriptionBackground/CompositionPanel") {
            node.action = "close_reinf_composition";
            continue;
        }
        if (!node.button ||
            (node.name != "UnitBtn" &&
             node.name != "UnitCountBtn")) {
            continue;
        }
        constexpr const char* kUnitPrefix =
                "ReinfDescriptionBackground/CompositionPanel/"
                "Unit";
        if (node.path.compare(
                    0,
                    std::char_traits<char>::length(kUnitPrefix),
                    kUnitPrefix) != 0) {
            continue;
        }
        const size_t digit_offset =
                std::char_traits<char>::length(kUnitPrefix);
        if (digit_offset >= node.path.size() ||
            node.path[digit_offset] < '1' ||
            node.path[digit_offset] > '4') {
            continue;
        }
        if (node.path.find("/UnitBtn") ==
                    std::string::npos &&
            node.path.find("/UnitCountBtn") ==
                    std::string::npos) {
            continue;
        }
        node.action =
                "select_reinf_composition_unit_" +
                std::to_string(
                        node.path[digit_offset] - '1');
        ++unit_actions;
    }
    std::ostringstream report;
    report << "original_menu_chapter_reinf_actions=ready"
           << "; branches=" << branch_actions
           << "; unit_buttons=" << unit_actions
           << "; dialog="
           << (g_chapter_reinf_dialog_mode ==
                       ChapterReinfDialogMode::kComposition
                       ? "composition"
                       : "none");
    PlatformRuntime::instance().log_info(report.str());
}

// InitMissions clones a target button onto the chapter map for each mission
// in the chapter's path. Position comes from the details map when the chapter
// ships one, and from the mission's own PlaceOnChapterMap otherwise, which is
// the same fallback SChapterMapMenuHelper uses.
void PopulateChapterMapLocked() {
    const NDb::SChapter* chapter = SelectedChapter();
    if (chapter == nullptr || chapter->missionPath.empty()) {
        return;
    }
    const std::map<std::string, MenuTemplate>::const_iterator map_window =
            g_templates.find("ChapterMap");
    const std::map<std::string, MenuTemplate>::const_iterator target =
            g_templates.find("ChapterMapTarget");
    const std::map<std::string, MenuTemplate>::const_iterator target_big =
            g_templates.find("ChapterMapTargetBig");
    if (map_window == g_templates.end() || target == g_templates.end()) {
        return;
    }
    float details_scale_x = 0.0f;
    float details_scale_y = 0.0f;
    const NDb::SMapInfo* details = chapter->pDetailsMap.GetPtr();
    if (details != nullptr &&
        details->nNumPatchesX > 0 &&
        details->nNumPatchesY > 0) {
        details_scale_x = map_window->second.width /
                static_cast<float>(
                        details->nNumPatchesX * AI_TILE_SIZE *
                        AI_TILES_IN_PATCH);
        details_scale_y = map_window->second.height /
                static_cast<float>(
                        details->nNumPatchesY * AI_TILE_SIZE *
                        AI_TILES_IN_PATCH);
    }
    g_textured_quad_insert_index = g_quads.size();
    AppendChapterMapPotential(
            chapter,
            details,
            map_window->second,
            details_scale_x,
            details_scale_y);
    AppendChapterMapArrows(
            chapter,
            details,
            map_window->second,
            details_scale_x,
            details_scale_y);
    size_t placed = 0;
    for (size_t index = 0; index < chapter->missionPath.size(); ++index) {
        const NDb::SMissionEnableInfo& mission = chapter->missionPath[index];
        float position_x = mission.vPlaceOnChapterMap.x;
        float position_y = mission.vPlaceOnChapterMap.y;
        if (details != nullptr && details_scale_x > 0.0f) {
            for (size_t object = 0; object < details->objects.size();
                 ++object) {
                if (details->objects[object].nPlayer !=
                    static_cast<int>(index)) {
                    continue;
                }
                // Map2Screen swaps the axes.
                position_x =
                        details->objects[object].vPos.y * details_scale_x;
                position_y =
                        details->objects[object].vPos.x * details_scale_y;
                break;
            }
        }
        // The first entry of the path is the chapter's main target and uses
        // the larger marker.
        const MenuTemplate& marker =
                index == 0 && target_big != g_templates.end()
                        ? target_big->second
                        : target->second;
        if (marker.shared == nullptr) {
            continue;
        }
        const float x = map_window->second.x + position_x -
                marker.width * 0.5f;
        const float y = map_window->second.y + position_y -
                marker.height * 0.5f;
        MenuWindowNode node;
        node.name = "Target" + std::to_string(index);
        node.type = "ChapterMapTarget";
        node.x = x;
        node.y = y;
        node.width = marker.width;
        node.height = marker.height;
        node.visible = true;
        const ChapterTargetState target_state =
                index < g_chapter_target_states.size()
                        ? g_chapter_target_states[index]
                        : kChapterTargetDisabled;
        const bool selected =
                static_cast<int>(index) == g_selected_mission;
        node.enabled = target_state != kChapterTargetCompleted;
        node.button = true;
        node.action =
                "select_mission_" + std::to_string(index);
        node.pressed_quad_begin = static_cast<int>(g_pressed_quads.size());
        node.pressed_quad_end = node.pressed_quad_begin;
        const NDb::SWindowMSButtonShared* marker_shared =
                dynamic_cast<const NDb::SWindowMSButtonShared*>(
                        marker.shared);
        size_t visual_state_index = 0;
        if (target_state == kChapterTargetCompleted) {
            visual_state_index = 4;
        } else if (target_state == kChapterTargetDisabled) {
            visual_state_index = selected ? 3 : 2;
        } else if (target_state == kChapterTargetRecommended) {
            visual_state_index = selected ? 6 : 5;
        } else {
            visual_state_index = selected ? 1 : 0;
        }
        if (marker_shared != nullptr &&
            visual_state_index < marker_shared->visualStates.size()) {
            const NDb::SButtonVisualState& visual_state =
                    marker_shared->visualStates[visual_state_index];
            AppendBackgroundQuads(
                    marker.shared->pBackground.GetPtr(),
                    x,
                    y,
                    marker.width,
                    marker.height);
            AppendBackgroundQuads(
                    visual_state.normal.pBackground.GetPtr(),
                    x,
                    y,
                    marker.width,
                    marker.height);
            AppendBackgroundQuads(
                    visual_state.normal.pForeground.GetPtr(),
                    x,
                    y,
                    marker.width,
                    marker.height);
            if (node.enabled) {
                const size_t pressed_begin = g_quads.size();
                AppendBackgroundQuads(
                        marker.shared->pBackground.GetPtr(),
                        x,
                        y,
                        marker.width,
                        marker.height);
                AppendBackgroundQuads(
                        visual_state.pushed.pBackground.GetPtr(),
                        x,
                        y,
                        marker.width,
                        marker.height);
                AppendBackgroundQuads(
                        visual_state.pushed.pForeground.GetPtr(),
                        x,
                        y,
                        marker.width,
                        marker.height);
                node.pressed_quad_begin =
                        static_cast<int>(g_pressed_quads.size());
                g_pressed_quads.insert(
                        g_pressed_quads.end(),
                        g_quads.begin() + pressed_begin,
                        g_quads.end());
                node.pressed_quad_end =
                        static_cast<int>(g_pressed_quads.size());
                g_quads.resize(pressed_begin);
            }
        }
        ++g_button_count;
        g_nodes.push_back(node);
        ++placed;
    }

    AppendChapterRollers();
    BindChapterReinfCompositionActionsLocked();

    std::ostringstream report;
    report << "original_menu_chapter_targets=" << placed
           << "; details_map=" << (details != nullptr ? "yes" : "no")
           << "; map_rect=" << map_window->second.x << ","
           << map_window->second.y << "+" << map_window->second.width << "x"
           << map_window->second.height;
    for (const MenuWindowNode& node : g_nodes) {
        if (node.type != "ChapterMapTarget") {
            continue;
        }
        report << "; " << node.name << "="
               << node.x << "," << node.y << "+"
               << node.width << "x" << node.height
               << " state="
               << static_cast<int>(
                          g_chapter_target_states[
                                  static_cast<size_t>(
                                          std::atoi(
                                                  node.name.c_str() +
                                                  6))])
               << (node.name ==
                                   "Target" +
                                           std::to_string(
                                                   g_selected_mission)
                           ? " selected"
                           : "");
    }
    PlatformRuntime::instance().log_info(report.str());
}

// CInterfaceCampaignSelectionMenu builds this screen from the three campaigns
// in GameRoot. The descriptors only contain reusable panel shells, so bind
// their descriptions and controller-generated actions after exact layout is
// known. The phone uses a tap-to-cycle difficulty control in the authored
// combo rectangle; the values and StartCampaign index remap stay original.
void PopulateCampaignSelectionLocked() {
    const NDb::SGameRoot* root = NGameX::GetGameRoot();
    if (root == nullptr) {
        return;
    }
    const size_t campaign_count =
            std::min<size_t>(3, root->campaigns.size());
    for (size_t slot = 0; slot < campaign_count; ++slot) {
        const NDb::SCampaign* campaign =
                root->campaigns[slot].GetPtr();
        if (campaign == nullptr) {
            continue;
        }
        const std::string panel =
                "Main/CampaignPanel" + std::to_string(slot + 1);
        const std::string container_path = panel + "/DescCont";
        const std::string view_path = container_path + "/DescView";
        MenuWindowNode* select_button = nullptr;
        const MenuWindowNode* container = nullptr;
        const MenuWindowNode* view = nullptr;
        for (MenuWindowNode& node : g_nodes) {
            if (node.path == panel + "/SelectCampaignBtn") {
                select_button = &node;
            } else if (node.path == container_path) {
                container = &node;
            } else if (node.path == view_path) {
                view = &node;
            }
        }
        if (select_button != nullptr) {
            select_button->action =
                    "campaign0" + std::to_string(slot + 1);
        }
        if (container == nullptr || !container->visible) {
            continue;
        }
        std::string description;
        std::string description_format;
        SplitMarkupTags(
                LoadUtf16BriefingText(NormalizeResourcePath(
                        ToStdString(campaign->szLocalizedDescFileRef))),
                &description,
                &description_format);
        std::string merged_format =
                view == nullptr ? std::string() : view->text_format;
        if (!description_format.empty()) {
            if (!merged_format.empty()) {
                merged_format.push_back(' ');
            }
            merged_format += description_format;
        }
        const float inset = 8.0f;
        const float scroll_bar_reserve = 20.0f;
        AppendWrappedTextQuads(
                description,
                merged_format,
                container->x + inset,
                container->y + inset,
                std::max(
                        1.0f,
                        container->width - inset * 2.0f -
                                scroll_bar_reserve),
                std::max(1.0f, container->height - inset * 2.0f),
                view == nullptr ? std::string() : view->text_face,
                view == nullptr ? 0u : view->text_argb);
    }

    MenuWindowNode* play = nullptr;
    MenuWindowNode* back = nullptr;
    MenuWindowNode* difficulty = nullptr;
    for (MenuWindowNode& node : g_nodes) {
        if (node.path == "Main/BottomPanel/PlayBtn") {
            play = &node;
        } else if (node.path == "Main/BottomPanel/BackBtn") {
            back = &node;
        } else if (node.path == "Main/BottomPanel/Difficulty") {
            difficulty = &node;
        }
    }
    if (play != nullptr) {
        play->action = "menu_play";
    }
    if (back != nullptr) {
        back->action = "menu_back";
    }

    std::string difficulty_name;
    const NDb::SCampaign* selected =
            g_selected_campaign >= 0 &&
                    static_cast<size_t>(g_selected_campaign) <
                            campaign_count
                    ? root->campaigns[
                              static_cast<size_t>(g_selected_campaign)]
                              .GetPtr()
                    : nullptr;
    if (selected != nullptr && !selected->difficultyLevels.empty()) {
        size_t db_index = static_cast<size_t>(g_selected_difficulty);
        if (selected->difficultyLevels.size() == 4) {
            db_index =
                    g_selected_difficulty == 0
                            ? 3
                            : static_cast<size_t>(
                                      g_selected_difficulty - 1);
        }
        if (db_index < selected->difficultyLevels.size() &&
            selected->difficultyLevels[db_index]) {
            const NDb::SDifficultyLevel* level =
                    selected->difficultyLevels[db_index].GetPtr();
            difficulty_name =
                    LoadUtf16Text(NormalizeResourcePath(
                            ToStdString(level->szLocalizedNameFileRef)));
        }
    }

    const std::map<std::string, MenuTemplate>::const_iterator combo =
            g_templates.find("Difficulty");
    if (difficulty != nullptr && difficulty->visible &&
        combo != g_templates.end() && combo->second.shared != nullptr) {
        const NDb::SWindowComboBoxShared* combo_shared =
                dynamic_cast<const NDb::SWindowComboBoxShared*>(
                        combo->second.shared);
        const NDb::SWindowMSButton* icon =
                combo_shared == nullptr
                        ? nullptr
                        : combo_shared->pIcon.GetPtr();
        const NDb::SWindowMSButtonShared* icon_shared =
                icon == nullptr
                        ? nullptr
                        : dynamic_cast<const NDb::SWindowMSButtonShared*>(
                                  SharedOf(icon));
        if (icon_shared != nullptr &&
            !icon_shared->visualStates.empty()) {
            AppendBackgroundQuads(
                    icon_shared->visualStates[0].normal.pBackground.GetPtr(),
                    difficulty->x - 1.0f,
                    difficulty->y,
                    difficulty->width,
                    difficulty->height);
            difficulty->pressed_quad_begin =
                    static_cast<int>(g_pressed_quads.size());
            const size_t pressed_begin = g_quads.size();
            AppendBackgroundQuads(
                    icon_shared->visualStates[0].pushed.pBackground.GetPtr(),
                    difficulty->x - 1.0f,
                    difficulty->y,
                    difficulty->width,
                    difficulty->height);
            g_pressed_quads.insert(
                    g_pressed_quads.end(),
                    g_quads.begin() + pressed_begin,
                    g_quads.end());
            difficulty->pressed_quad_end =
                    static_cast<int>(g_pressed_quads.size());
            g_quads.resize(pressed_begin);
        }
        AppendTextQuads(
                difficulty_name,
                LoadUtf16Text(
                        "UI/Game/Common/Templates/TopLeft/"
                        "CourierBold_17pt_Color_ffcecfce.txt"),
                difficulty->x + 9.0f,
                difficulty->y + 9.0f,
                84.0f,
                22.0f,
                std::string(),
                0u);
        difficulty->button = true;
        difficulty->enabled = !difficulty_name.empty();
        difficulty->action = "difficulty_cycle";
        ++g_button_count;
    }

    std::ostringstream report;
    report << "original_menu_campaign_selection=ready"
           << "; campaigns=" << campaign_count
           << "; selected=" << g_selected_campaign
           << "; difficulty_ui=" << g_selected_difficulty
           << "; difficulty=" << difficulty_name;
    PlatformRuntime::instance().log_info(report.str());
}

// CInterfaceMissionBriefing pushes the current map's two text resources into
// scrollable containers and assigns its authored minimap texture. Android does
// not link that legacy controller, so reproduce its data binding after the
// shipped descriptor graph has supplied the exact panel geometry and fonts.
void PopulateMissionBriefingLocked() {
    const NDb::SMapInfo* mission = SelectedMission();
    if (mission == nullptr) {
        return;
    }

    std::string mission_text;
    std::string mission_format;
    SplitMarkupTags(
            LoadUtf16BriefingText(NormalizeResourcePath(
                    ToStdString(mission->szLoadingDescriptionFileRef))),
            &mission_text,
            &mission_format);
    std::string objectives_text;
    std::string objectives_format;
    SplitMarkupTags(
            LoadUtf16BriefingText(NormalizeResourcePath(
                    ToStdString(mission->szLocalizedDescriptionFileRef))),
            &objectives_text,
            &objectives_format);

    const auto append_to_container =
            [](const char* container_name,
               const char* view_name,
               const std::string& text,
               const std::string& format) {
                const std::map<std::string, MenuTemplate>::const_iterator
                        container = g_templates.find(container_name);
                if (container == g_templates.end() || text.empty()) {
                    return;
                }
                const MenuWindowNode* view = nullptr;
                for (const MenuWindowNode& node : g_nodes) {
                    if (node.name == view_name) {
                        view = &node;
                        break;
                    }
                }
                std::string merged_format =
                        view == nullptr ? std::string()
                                        : view->text_format;
                if (!format.empty()) {
                    if (!merged_format.empty()) {
                        merged_format.push_back(' ');
                    }
                    merged_format += format;
                }
                const float inset = 8.0f;
                const float scroll_bar_reserve = 24.0f;
                AppendWrappedTextQuads(
                        text,
                        merged_format,
                        container->second.x + inset,
                        container->second.y + inset,
                        std::max(
                                1.0f,
                                container->second.width -
                                        inset * 2.0f -
                                        scroll_bar_reserve),
                        std::max(
                                1.0f,
                                container->second.height - inset * 2.0f),
                        view == nullptr ? std::string()
                                        : view->text_face,
                        view == nullptr ? 0u : view->text_argb);
            };
    append_to_container(
            "MissionDescCont",
            "MissionDescView",
            mission_text,
            mission_format);
    append_to_container(
            "ObjectivesListCont",
            "ObjectivesListView",
            objectives_text,
            objectives_format);

    const NDb::STexture* minimap =
            mission->pMiniMap && mission->pMiniMap->pTexture
                    ? mission->pMiniMap->pTexture.GetPtr()
                    : nullptr;
    std::ostringstream report;
    report << "original_mission_briefing=ready"
           << "; mission="
           << ToStdString(mission->GetDBID().ToString())
           << "; loading_bytes=" << mission_text.size()
           << "; objective_bytes=" << objectives_text.size()
           << "; minimap="
           << (minimap == nullptr ? "<none>" : TexturePath(minimap));
    PlatformRuntime::instance().log_info(report.str());
}

// Moves the modal branch behind nothing: the chapter map's generated layers
// and mission markers are appended after the screen graph, so the dialog has
// to be rotated back to the end of the draw list.
void LiftModalQuadsLocked() {
    if (g_modal_quad_end <= g_modal_quad_begin ||
        g_modal_quad_end > g_quads.size() ||
        g_modal_quad_end == g_quads.size()) {
        return;
    }
    const size_t moved = g_modal_quad_end - g_modal_quad_begin;
    std::rotate(
            g_quads.begin() + static_cast<std::ptrdiff_t>(g_modal_quad_begin),
            g_quads.begin() + static_cast<std::ptrdiff_t>(g_modal_quad_end),
            g_quads.end());
    if (g_textured_quad_insert_index != static_cast<size_t>(-1) &&
        g_textured_quad_insert_index >= g_modal_quad_end) {
        g_textured_quad_insert_index -= moved;
    }
    g_modal_quad_begin = g_quads.size() - moved;
    g_modal_quad_end = g_quads.size();
}

bool RebuildMenuScreenLocked(const std::string& screen_ref) {
    g_nodes.clear();
    g_quads.clear();
    g_textured_quads.clear();
    g_textured_quad_insert_index = static_cast<size_t>(-1);
    g_applied_caption_paths.clear();
    g_modal_quad_path =
            g_chapter_reinf_dialog_mode != ChapterReinfDialogMode::kNone &&
                    screen_ref == "UI/Game/Menu/ChapterMap_WindowScreen.xdb"
                    ? std::string("ReinfDescriptionBackground")
                    : std::string();
    g_modal_quad_begin = 0;
    g_modal_quad_end = 0;
    g_chapter_potential_texture = nullptr;
    g_chapter_potential_animation.reset();
    g_chapter_roller_animation.reset();
    g_texture_paths.clear();
    g_pressed_quads.clear();
    g_pressed_button = -1;
    g_templates.clear();
    g_texture_handles.clear();
    g_fonts.clear();
    g_render_logged = false;
    g_text_cache.clear();
    g_button_count = 0;
    g_texture_count = 0;
    g_caption_count = 0;
    g_ready = false;
    g_error.clear();
    g_screen_ref = screen_ref;

    if (screen_ref.empty()) {
        g_error = "empty_screen_ref";
        return false;
    }
    const NDb::SWindowScreen* screen =
            NDb::Get<NDb::SWindowScreen>(CDBID(screen_ref.c_str()));
    if (screen == nullptr) {
        g_error = "screen_not_found";
        return false;
    }

    LoadMenuFonts();
    LoadButtonClickSound();
    if (!g_menu_options_restored) {
        g_menu_options_restored = true;
        LoadMenuOptions();
    }
    if (!g_menu_music) {
        StartMenuMusic();
    }
    CollectWindow(
            screen,
            0.0f,
            0.0f,
            kVirtualScreenWidth,
            kVirtualScreenHeight,
            0,
            true,
            std::string());
    if (screen_ref.find("OptionsMenu") != std::string::npos) {
        PopulateOptionsScreenLocked();
    }
    if (screen_ref == kCampaignSelectionScreenRef) {
        PopulateCampaignSelectionLocked();
    }
    if (screen_ref == "UI/Game/Menu/ChapterMap_WindowScreen.xdb") {
        PopulateChapterMapLocked();
    }
    if (screen_ref ==
        "UI/Game/Menu/MissionBriefing/"
        "MissionBriefing_WindowScreen.xdb") {
        PopulateMissionBriefingLocked();
    }
    LiftModalQuadsLocked();
    g_ready = g_nodes.size() > 1;
    if (!g_ready) {
        g_error = "empty_screen_graph";
    }
    return g_ready;
}

// Names and rects of the windows a screen resolved, so a screen's template
// windows can be located while its runtime content is being ported.
std::string ScreenLayoutReportLocked() {
    std::ostringstream report;
    report << "original_menu_layout=" << g_screen_ref
           << "; windows=" << g_nodes.size();
    for (const MenuWindowNode& node : g_nodes) {
        if (node.name.empty()) {
            continue;
        }
        report << "; " << node.name << "=" << node.x << "," << node.y
               << " " << node.width << "x" << node.height
               << (node.visible ? "" : " hidden");
    }
    return report.str();
}

}  // namespace

void RenderOriginalMenu(uint32_t screen_width, uint32_t screen_height) {
    std::lock_guard<std::mutex> guard(g_menu_mutex);
    if (!g_ready || screen_width == 0 || screen_height == 0) {
        return;
    }
    UpdateChapterMapPotentialAnimationLocked();
    UpdateChapterRollerAnimationLocked();
    // UI.cpp VirtualToScreenX/Y scale each axis independently, so the shipped
    // 1024x768 layout stretches to whatever resolution is active.
    const float scale_x =
            static_cast<float>(screen_width) / kVirtualScreenWidth;
    const float scale_y =
            static_cast<float>(screen_height) / kVirtualScreenHeight;
    if (g_texture_handles.size() != g_texture_paths.size()) {
        g_texture_handles.assign(g_texture_paths.size(), UINT16_MAX);
    }
    for (size_t index = 0; index < g_texture_paths.size(); ++index) {
        if (g_texture_handles[index] == UINT16_MAX) {
            g_texture_handles[index] = MenuTextureHandle(
                    g_texture_paths[index]);
        }
    }
    size_t submitted = 0;
    const auto submit = [&](const MenuQuad& quad) {
        if (quad.texture < 0 ||
            static_cast<size_t>(quad.texture) >= g_texture_handles.size()) {
            return;
        }
        const uint16_t handle = g_texture_handles[quad.texture];
        if (handle == UINT16_MAX) {
            return;
        }
        RenderBackend().queue_textured_rect(
                quad.x * scale_x,
                quad.y * scale_y,
                quad.width * scale_x,
                quad.height * scale_y,
                handle,
                quad.u0,
                quad.v0,
                quad.u1,
                quad.v1,
                quad.argb);
        ++submitted;
    };
    if (!g_menu_background_path.empty()) {
        if (g_menu_background_texture == UINT16_MAX) {
            g_menu_background_texture =
                    MenuTextureHandle(g_menu_background_path);
        }
        if (g_menu_background_texture != UINT16_MAX) {
            // The menu lays out inside the content area, but the background
            // belongs to the whole surface: anything it does not cover shows
            // as a black band.
            const float surface_width = std::max(
                    static_cast<float>(RenderBackend().width()),
                    static_cast<float>(screen_width));
            const float surface_height = std::max(
                    static_cast<float>(RenderBackend().height()),
                    static_cast<float>(screen_height));
            RenderBackend().queue_textured_rect(
                    0.0f,
                    0.0f,
                    surface_width,
                    surface_height,
                    g_menu_background_texture,
                    0.0f,
                    0.0f,
                    g_menu_background_u,
                    g_menu_background_v,
                    0xffffffffu);
            ++submitted;
        }
    }
    const auto submit_textured_quads = [&]() {
        for (const MenuTexturedQuad& quad : g_textured_quads) {
            uint16_t handle = UINT16_MAX;
            if (quad.direct_texture != nullptr) {
                EnsureLegacyTextureMipChainUploaded(quad.direct_texture);
                handle = LegacyTextureHandleIndex(quad.direct_texture);
            } else {
                if (quad.texture < 0 ||
                    static_cast<size_t>(quad.texture) >=
                            g_texture_handles.size()) {
                    continue;
                }
                handle = g_texture_handles[quad.texture];
            }
            if (handle == UINT16_MAX) {
                continue;
            }
            RenderBackend().queue_textured_quad(
                    quad.x0 * scale_x,
                    quad.y0 * scale_y,
                    quad.x1 * scale_x,
                    quad.y1 * scale_y,
                    quad.x2 * scale_x,
                    quad.y2 * scale_y,
                    quad.x3 * scale_x,
                    quad.y3 * scale_y,
                    handle,
                    quad.u0,
                    quad.v0,
                    quad.u1,
                    quad.v1,
                    quad.argb);
            ++submitted;
        }
    };
    bool textured_quads_submitted = false;
    for (size_t index = 0; index < g_quads.size(); ++index) {
        if (!textured_quads_submitted &&
            index == g_textured_quad_insert_index) {
            submit_textured_quads();
            textured_quads_submitted = true;
        }
        submit(g_quads[index]);
    }
    if (!textured_quads_submitted) {
        submit_textured_quads();
    }
    // A held button repaints in its pushed presentation on top of the normal
    // one, which is what the desktop button state swap looks like.
    if (g_pressed_button >= 0 &&
        static_cast<size_t>(g_pressed_button) < g_nodes.size()) {
        const MenuWindowNode& node =
                g_nodes[static_cast<size_t>(g_pressed_button)];
        for (int index = node.pressed_quad_begin;
             index < node.pressed_quad_end &&
             static_cast<size_t>(index) < g_pressed_quads.size();
             ++index) {
            submit(g_pressed_quads[static_cast<size_t>(index)]);
        }
    }
    if (!g_render_logged && submitted > 0) {
        std::ostringstream report;
        size_t ready_textures = 0;
        for (uint16_t handle : g_texture_handles) {
            if (handle != UINT16_MAX) {
                ++ready_textures;
            }
        }
        report << "original_menu_render=ready"
               << "; quads=" << g_quads.size()
               << "; textured_quads=" << g_textured_quads.size()
               << "; submitted=" << submitted
               << "; textures=" << ready_textures
               << "/" << g_texture_handles.size()
               << "; surface=" << screen_width << "x" << screen_height;
        PlatformRuntime::instance().log_info(report.str());
        g_render_logged = true;
    }
}

namespace {

// CWindowScreen::Pick maps the pointer back into the virtual screen before
// hit testing, so the same inverse of VirtualToScreen is used here.
int PickMenuButtonLocked(
        float screen_x,
        float screen_y,
        uint32_t screen_width,
        uint32_t screen_height) {
    if (screen_width == 0 || screen_height == 0) {
        return -1;
    }
    const float virtual_x = screen_x * kVirtualScreenWidth /
            static_cast<float>(screen_width);
    const float virtual_y = screen_y * kVirtualScreenHeight /
            static_cast<float>(screen_height);
    // Later children draw on top, so the last match wins.
    int picked = -1;
    const bool modal_reinforcement_dialog =
            g_chapter_reinf_dialog_mode !=
            ChapterReinfDialogMode::kNone;
    for (size_t index = 0; index < g_nodes.size(); ++index) {
        const MenuWindowNode& node = g_nodes[index];
        // A touch target is a button with an area. Requiring a pressed-state
        // visual too made every options row unpickable, because the shipped
        // row template draws no pressed art.
        if (!node.button || !node.visible || !node.enabled ||
            node.width <= 0.0f || node.height <= 0.0f) {
            continue;
        }
        if (modal_reinforcement_dialog &&
            node.path.compare(
                    0,
                    std::char_traits<char>::length(
                            "ReinfDescriptionBackground"),
                    "ReinfDescriptionBackground") != 0) {
            continue;
        }
        if (virtual_x >= node.x &&
            virtual_x <= node.x + node.width &&
            virtual_y >= node.y &&
            virtual_y <= node.y + node.height) {
            picked = static_cast<int>(index);
        }
    }
    return picked;
}

}  // namespace

void PressOriginalMenu(
        float screen_x,
        float screen_y,
        uint32_t screen_width,
        uint32_t screen_height) {
    std::lock_guard<std::mutex> guard(g_menu_mutex);
    if (!g_ready) {
        return;
    }
    g_pressed_button = PickMenuButtonLocked(
            screen_x, screen_y, screen_width, screen_height);
}

void CancelOriginalMenuPress() {
    std::lock_guard<std::mutex> guard(g_menu_mutex);
    g_pressed_button = -1;
}

std::string ReleaseOriginalMenu(
        float screen_x,
        float screen_y,
        uint32_t screen_width,
        uint32_t screen_height) {
    std::lock_guard<std::mutex> guard(g_menu_mutex);
    const int pressed = g_pressed_button;
    g_pressed_button = -1;
    if (!g_ready || pressed < 0) {
        return std::string();
    }
    // The shipped menu buttons trigger on release, matching BCST_ON_RELEASE.
    const int released = PickMenuButtonLocked(
            screen_x, screen_y, screen_width, screen_height);
    if (released != pressed) {
        return std::string();
    }
    const MenuWindowNode& node = g_nodes[static_cast<size_t>(pressed)];
    if (g_click_clip.frame_count() > 0) {
        AudioBackend().play(g_click_clip.view(), false, 0);
    }
    std::ostringstream report;
    report << "original_menu_action=" << node.action
           << "; button=" << (node.name.empty() ? node.caption : node.name);
    PlatformRuntime::instance().log_info(report.str());
    return node.action;
}

void ReleaseOriginalMenuGpuResources() {
    std::lock_guard<std::mutex> guard(g_menu_mutex);
    g_menu_textures.clear();
    g_texture_handles.clear();
    g_render_logged = false;
}

void ShutdownOriginalMenuRuntime() {
    std::lock_guard<std::mutex> guard(g_menu_mutex);
    StopMenuMusic();
    g_nodes.clear();
    g_quads.clear();
    g_textured_quads.clear();
    g_textured_quad_insert_index = static_cast<size_t>(-1);
    g_chapter_potential_texture = nullptr;
    g_chapter_potential_animation.reset();
    g_chapter_roller_animation.reset();
    g_chapter_roller_transition = ChapterRollerTransition();
    g_chapter_roller_selection_requested = false;
    g_chapter_reinforcements.clear();
    g_chapter_reinf_dialog_mode =
            ChapterReinfDialogMode::kNone;
    g_chapter_reinf_dialog_type = -1;
    g_chapter_reinf_dialog_unit = 0;
    g_texture_paths.clear();
    g_texture_handles.clear();
    g_pressed_quads.clear();
    g_pressed_button = -1;
    g_menu_textures.clear();
    g_fonts.clear();
    g_render_logged = false;
    g_text_cache.clear();
    g_visibility_overrides.clear();
    g_path_visibility_overrides.clear();
    g_path_enabled_overrides.clear();
    g_path_button_state_overrides.clear();
    g_caption_overrides.clear();
    g_progress_overrides.clear();
    g_texture_overrides.clear();
    g_path_texture_overrides.clear();
    g_screen_stack.clear();
    g_screen_ref.clear();
    g_error.clear();
    g_button_count = 0;
    g_texture_count = 0;
    g_caption_count = 0;
    g_ready = false;
}

bool IsOriginalMenuReady() {
    std::lock_guard<std::mutex> guard(g_menu_mutex);
    return g_ready;
}

const std::vector<MenuWindowNode>& OriginalMenuNodes() {
    return g_nodes;
}

float OriginalMenuVirtualWidth() {
    return kVirtualScreenWidth;
}

float OriginalMenuVirtualHeight() {
    return kVirtualScreenHeight;
}

std::string OriginalMenuReport() {
    std::lock_guard<std::mutex> guard(g_menu_mutex);
    std::ostringstream report;
    report << "original_menu=" << (g_ready ? "ready" : "unavailable")
           << "; screen=" << (g_screen_ref.empty() ? "<none>" : g_screen_ref)
           << "; error=" << (g_error.empty() ? "<none>" : g_error)
           << "; windows=" << g_nodes.size()
           << "; buttons=" << g_button_count
           << "; textures=" << g_texture_count
           << "; captions=" << g_caption_count
           << "; quads=" << g_quads.size()
           << "; texture_paths=" << g_texture_paths.size()
           << "; fonts=" << g_fonts.size()
           << "; click_sound="
           << (g_click_sound_path.empty() ? "<none>" : g_click_sound_path)
           << "; click_frames=" << g_click_clip.frame_count()
           << "; click_error="
           << (g_click_sound_error.empty() ? "<none>" : g_click_sound_error)
           << "; music="
           << (g_menu_music_path.empty() ? "<none>" : g_menu_music_path)
           << "; music_state="
           << (g_menu_music ? "streaming"
                            : (g_menu_music_error.empty()
                                       ? "idle"
                                       : g_menu_music_error));
    for (const MenuWindowNode& node : g_nodes) {
        if (!node.button || !node.visible) {
            continue;
        }
        report << "; button[" << node.name << "]="
               << node.x << "," << node.y << " "
               << node.width << "x" << node.height
               << " text=" << (node.caption.empty() ? "<none>" : node.caption)
               << " style="
               << (node.text_format.empty() ? "<none>" : node.text_format)
               << " action=" << (node.action.empty() ? "<none>" : node.action);
    }
    return report.str();
}

}  // namespace bk2::android

extern "C" JNIEXPORT jstring JNICALL
Java_com_nival_blitzkrieg2_NativeBridge_runOriginalMenuProbe(
        JNIEnv* env,
        jclass) {
    bk2::android::LoadOriginalMenuScreen(
            "UI/Game/Menu/MainMenu_WindowScreen.xdb");
    const std::string report = bk2::android::OriginalMenuReport();
    bk2::android::PlatformRuntime::instance().log_info(report);
    return env->NewStringUTF(report.c_str());
}
