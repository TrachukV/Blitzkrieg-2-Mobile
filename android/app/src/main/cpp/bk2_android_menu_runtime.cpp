#include "bk2_android_menu_runtime.h"

#include "bk2_android_audio_backend.h"
#include "bk2_android_audio_decode.h"
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
#include "GameX/dbgameoptions.h"
#include "System/GlobalVars.h"
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
#include <fstream>
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

// Mirrors NText::GetText() for the UTF-16 caption files the menu descriptors
// reference, converted to UTF-8 for the Android view layer.
std::string LoadUtf16Text(const std::string& file_ref) {
    if (file_ref.empty() || NVFS::GetMainVFS() == nullptr) {
        return std::string();
    }
    const std::map<std::string, std::string>::const_iterator cached =
            g_text_cache.find(file_ref);
    if (cached != g_text_cache.end()) {
        return cached->second;
    }

    CFileStream stream(NVFS::GetMainVFS(), string(file_ref.c_str()));
    std::string text;
    const int byte_count = stream.GetSize();
    if (!stream.IsOk() || byte_count < 2) {
        g_text_cache[file_ref] = text;
        return text;
    }
    const unsigned char* bytes = stream.GetBuffer();
    if (bytes == nullptr) {
        g_text_cache[file_ref] = text;
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
           character_count < kMaxCaptionCharacters) {
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
    g_text_cache[file_ref] = text;
    return text;
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
        float height) {
    if (background == nullptr || !background->pTexture) {
        return;
    }
    const NDb::STexture* texture_desc = background->pTexture.GetPtr();
    if (texture_desc == nullptr) {
        return;
    }
    const int texture = TextureIndex(BackgroundTexturePath(background));
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
        default: {
            // BackgroundSimpleScallingTexture and any other variant stretch
            // the whole texture over the window rect.
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

    float total = 0.0f;
    uint16_t previous = 0;
    for (uint16_t character : characters) {
        const STFCharacter& glyph = metrics.GetChar(character);
        total += static_cast<float>(glyph.nBC) +
                static_cast<float>(metrics.GetKern(character, previous));
        previous = character;
    }

    float pen_x = style.centered
            ? x + (width - total) * 0.5f
            : x;
    const float pen_y =
            y + (height - static_cast<float>(font.height)) * 0.5f;
    previous = 0;
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
        bool parent_visible) {
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

    // CWindow draws its own background, then its foreground, then children.
    if (visible && shared != nullptr) {
        const NDb::SBackground* background = shared->pBackground.GetPtr();
        if (window->GetTypeID() == NDb::SWindowMSButton::typeID) {
            const NDb::SWindowMSButtonShared* button_shared =
                    dynamic_cast<const NDb::SWindowMSButtonShared*>(shared);
            if (button_shared != nullptr &&
                !button_shared->visualStates.empty() &&
                button_shared->visualStates[0].normal.pBackground) {
                background = button_shared->visualStates[0]
                                     .normal.pBackground.GetPtr();
            }
        }
        AppendBackgroundQuads(background, x, y, width, height);
        AppendTextQuads(
                node.caption, node.text_format, x, y, width, height);
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
                    button_shared->visualStates[0].pushed.pBackground.GetPtr(),
                    x,
                    y,
                    width,
                    height);
            AppendTextQuads(
                    node.caption, node.text_format, x, y, width, height);
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
                child_window, x, y, width, height, depth + 1, visible);
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
         "UI/Game/Menu/CampaignSelection_WindowScreen.xdb"},
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
bool RequestCampaignLaunch(int campaign_index) {
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
    bool written = false;
    for (const std::string& target : targets) {
        std::ofstream file(target.c_str(), std::ios::trunc);
        if (!file.is_open()) {
            continue;
        }
        file << "campaign=" << campaign_index << "\n";
        file << "chapter=0\n";
        file << "mission=0\n";
        written = true;
    }
    if (!written) {
        return false;
    }
    std::ostringstream report;
    report << "original_menu_launch=campaign; index=" << campaign_index;
    PlatformRuntime::instance().log_info(report.str());
    g_mission_launch_requested = true;
    return true;
}

}  // namespace

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
            std::ostringstream report;
            report << "original_menu_campaign=" << index;
            PlatformRuntime::instance().log_info(report.str());
            return true;
        }
    }
    if (reaction == "chapter_map") {
        return RequestCampaignLaunch(g_selected_campaign);
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

bool LoadOriginalMenuScreen(const std::string& screen_ref) {
    std::lock_guard<std::mutex> guard(g_menu_mutex);
    g_visibility_overrides.clear();
    g_options_category = 0;
    return RebuildMenuScreenLocked(screen_ref);
}

namespace {

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
            if (entry.nModeFlags & kOptionModeMask) {
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
        if (!(entry.nModeFlags & kOptionModeMask)) {
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
        row_y += row_height + 4.0f;
    }
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
    CollectWindow(
            screen,
            0.0f,
            0.0f,
            kVirtualScreenWidth,
            kVirtualScreenHeight,
            0,
            true);
    if (screen_ref.find("OptionsMenu") != std::string::npos) {
        PopulateOptionsScreenLocked();
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
        if (!node.button || !node.visible || !node.enabled ||
            node.pressed_quad_end <= node.pressed_quad_begin) {
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
           << (g_click_sound_error.empty() ? "<none>" : g_click_sound_error);
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
