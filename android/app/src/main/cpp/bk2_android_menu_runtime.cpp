#include "bk2_android_menu_runtime.h"

#include "bk2_android_platform.h"

#include "UI/stdafx.h"
#include "UI/DBUserInterface.h"
#include "3Dmotor/DBScene.h"
#include "System/VFSOperations.h"
#include "libdb/Db.h"

#include <algorithm>
#include <cstdint>
#include <jni.h>
#include <map>
#include <mutex>
#include <sstream>

namespace bk2::android {
namespace {

// The shipped menu screens are authored against the original 1024x768
// interface surface; every placement value below is in that space.
constexpr float kVirtualScreenWidth = 1024.0f;
constexpr float kVirtualScreenHeight = 768.0f;
constexpr int kMaxMenuWindowDepth = 24;
constexpr size_t kMaxMenuWindowNodes = 512;
constexpr int kMaxCaptionCharacters = 120;

std::vector<MenuWindowNode> g_nodes;
std::string g_screen_ref;
std::string g_error;
size_t g_button_count = 0;
size_t g_texture_count = 0;
size_t g_caption_count = 0;
bool g_ready = false;
std::mutex g_menu_mutex;
std::map<std::string, std::string> g_text_cache;

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
        int depth) {
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
    node.visible = window->bVisible;
    node.enabled = window->bEnabled;
    node.depth = depth;
    ResolveWindowText(window, &node.caption, &node.text_format);
    if (shared != nullptr) {
        node.background_texture =
                BackgroundTexturePath(shared->pBackground.GetPtr());
        node.foreground_texture =
                BackgroundTexturePath(shared->pForeground.GetPtr());
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
    for (const CDBPtr<NDb::SUIDesc>& child : shared->children) {
        if (!child) {
            continue;
        }
        const NDb::SWindow* child_window =
                dynamic_cast<const NDb::SWindow*>(child.GetPtr());
        if (child_window == nullptr) {
            continue;
        }
        CollectWindow(child_window, x, y, width, height, depth + 1);
    }
}

}  // namespace

bool LoadOriginalMenuScreen(const std::string& screen_ref) {
    std::lock_guard<std::mutex> guard(g_menu_mutex);
    g_nodes.clear();
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

    CollectWindow(
            screen,
            0.0f,
            0.0f,
            kVirtualScreenWidth,
            kVirtualScreenHeight,
            0);
    g_ready = g_nodes.size() > 1;
    if (!g_ready) {
        g_error = "empty_screen_graph";
    }
    return g_ready;
}

void ShutdownOriginalMenuRuntime() {
    std::lock_guard<std::mutex> guard(g_menu_mutex);
    g_nodes.clear();
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
           << "; captions=" << g_caption_count;
    for (const MenuWindowNode& node : g_nodes) {
        if (!node.button) {
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
