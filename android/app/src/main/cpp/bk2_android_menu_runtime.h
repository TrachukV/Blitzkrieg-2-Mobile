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
    // Range of pressed-state quads built for this button.
    int pressed_quad_begin = 0;
    int pressed_quad_end = 0;
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

// Touch is mapped back through the original ScreenToVirtual transform and hit
// tested against the resolved button rects. A release inside the pressed
// button runs its descriptor reaction.
void PressOriginalMenu(
        float screen_x,
        float screen_y,
        uint32_t screen_width,
        uint32_t screen_height);
void CancelOriginalMenuPress();
// Returns the reaction id of the activated button, or an empty string.
std::string ReleaseOriginalMenu(
        float screen_x,
        float screen_y,
        uint32_t screen_width,
        uint32_t screen_height);

// Shows one of the screen's top-level panels and hides the others, which is
// what the shipped main_menu_init / single_player_submenu reactions do.
bool ShowOriginalMenuPanel(const std::string& panel_name);

bool IsOriginalMenuReady();
const std::vector<MenuWindowNode>& OriginalMenuNodes();
float OriginalMenuVirtualWidth();
float OriginalMenuVirtualHeight();
std::string OriginalMenuReport();

}  // namespace bk2::android
