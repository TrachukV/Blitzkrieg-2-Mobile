#include "bk2_android_menu_runtime.h"

#include "bk2_android_audio_backend.h"
#include "bk2_android_audio_decode.h"
#include "bk2_android_legacy_game_runtime.h"
#include "bk2_android_vorbis_stream.h"
#include "bk2_android_platform.h"
#include "bk2_legacy_texture_probe.h"
#include "bk2_port_paths.h"
#include "bk2_render_backend.h"

#include "UI/stdafx.h"
#include "UI/DBUserInterface.h"
#include "UISpecificB2/DBUISpecificB2.h"
#include "3Dmotor/DBScene.h"
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
#include <cstdint>
#include <jni.h>
#include <map>
#include <mutex>
#include <cstdlib>
#include <memory>
#include <fstream>
#include <functional>
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

std::vector<MenuQuad> g_quads;
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
std::map<std::string, std::string> g_caption_overrides;
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
    if (caption_override != g_caption_overrides.end()) {
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
                const NDb::SButtonVisualSubState& normal =
                        button_shared->visualStates[0].normal;
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
            const size_t normal_end = g_quads.size();
            AppendBackgroundQuads(
                    shared->pBackground.GetPtr(), x, y, width, height);
            AppendBackgroundQuads(
                    button_shared->visualStates[0].pushed.pBackground.GetPtr(),
                    x,
                    y,
                    width,
                    height);
            AppendBackgroundQuads(
                    button_shared->visualStates[0].pushed.pForeground.GetPtr(),
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
        return RunOriginalMenuReaction("chapter_map");
    }
    // The chapter map is a screen of its own between picking a campaign and
    // the mission.
    if (reaction == "play") {
        return LoadOriginalMissionBriefingScreen(
                g_selected_mission);
    }
    if (reaction.compare(0, 13, "play_mission_") == 0) {
        const int mission_index =
                std::atoi(reaction.c_str() + 13);
        if (mission_index < 0) {
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
    g_visibility_overrides.clear();
    g_path_visibility_overrides.clear();
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
    g_visibility_overrides.clear();
    g_path_visibility_overrides.clear();
    g_caption_overrides.clear();
    g_progress_overrides.clear();
    g_texture_overrides.clear();
    g_path_texture_overrides.clear();
    ApplyScreenInitVisibilityLocked(screen_ref);
    ApplyScreenInitTexturesLocked(screen_ref);
    ApplyCampaignSelectionBindingsLocked(screen_ref);
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
        node.enabled = true;
        node.button = true;
        node.action =
                "play_mission_" + std::to_string(index);
        node.pressed_quad_begin = static_cast<int>(g_pressed_quads.size());
        node.pressed_quad_end = node.pressed_quad_begin;
        AppendBackgroundQuads(
                marker.shared->pBackground.GetPtr(),
                x,
                y,
                marker.width,
                marker.height);
        ++g_button_count;
        g_nodes.push_back(node);
        ++placed;
    }
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
               << node.width << "x" << node.height;
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

bool RebuildMenuScreenLocked(const std::string& screen_ref) {
    g_nodes.clear();
    g_quads.clear();
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
    for (const MenuQuad& quad : g_quads) {
        submit(quad);
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
    for (size_t index = 0; index < g_nodes.size(); ++index) {
        const MenuWindowNode& node = g_nodes[index];
        // A touch target is a button with an area. Requiring a pressed-state
        // visual too made every options row unpickable, because the shipped
        // row template draws no pressed art.
        if (!node.button || !node.visible || !node.enabled ||
            node.width <= 0.0f || node.height <= 0.0f) {
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
