#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bk2::android {

// Resolved node of an original UI screen graph. Rects are in the original
// 1024x768 virtual screen space that the shipped descriptors are authored in.
struct MenuWindowNode {
    std::string name;
    std::string type;
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    bool visible = true;
    bool enabled = true;
    bool button = false;
    std::string background_texture;
    std::string foreground_texture;
    std::string pushed_texture;
    std::string caption;
    // Leading markup tags of the original format string, e.g. "<val button>",
    // which select the shipped text style rather than visible characters.
    std::string text_format;
    std::string action;
    int depth = 0;
};

// Loads the original main menu screen descriptor graph through the Android
// game database and resolves it with the desktop placement rules.
bool LoadOriginalMenuScreen(const std::string& screen_ref);
void ShutdownOriginalMenuRuntime();

// Submits the resolved screen through the bgfx 2D path. The original client
// lays screens out in a fixed 1024x768 virtual space and then scales X and Y
// independently to the active resolution, so the same mapping is used here.
void RenderOriginalMenu(uint32_t screen_width, uint32_t screen_height);
void ReleaseOriginalMenuGpuResources();

bool IsOriginalMenuReady();
const std::vector<MenuWindowNode>& OriginalMenuNodes();
float OriginalMenuVirtualWidth();
float OriginalMenuVirtualHeight();
std::string OriginalMenuReport();

}  // namespace bk2::android
