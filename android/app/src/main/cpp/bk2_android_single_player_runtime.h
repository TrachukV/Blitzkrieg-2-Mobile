#pragma once

#include <cstdint>
#include <string>

namespace bk2::android {

bool InitializeSinglePlayerRuntime();
void RefreshSinglePlayerRenderResources();
void TickSinglePlayerRuntime(uint32_t elapsed_millis);
void ShutdownSinglePlayerRuntime();

void PanSinglePlayerCamera(float delta_x_pixels, float delta_y_pixels);
void ZoomSinglePlayerCamera(float scale);
void RotateSinglePlayerCamera(float delta_radians);
bool HandleSinglePlayerTap(
        float screen_x,
        float screen_y,
        uint32_t viewport_width,
        uint32_t viewport_height);
bool HandleSinglePlayerSelectionRect(
        float start_x,
        float start_y,
        float end_x,
        float end_y,
        uint32_t viewport_width,
        uint32_t viewport_height);
bool SetSinglePlayerTouchCommandMode(int mode);
int SinglePlayerTouchCommandMode();
bool ActivateSelectedSinglePlayerUnit(int unit_id);
bool StopSelectedSinglePlayerUnit();
bool PerformSelectedSinglePlayerUnitAction(int user_action);
void SetSinglePlayerPaused(bool paused);
bool IsSinglePlayerPaused();

bool IsSinglePlayerRuntimeReady();
std::string CurrentSinglePlayerMissionId();
std::string SelectedSinglePlayerUnitHudStatus();
std::string SelectedSinglePlayerUnitHudSnapshot();
std::string SinglePlayerRuntimeReport();

}  // namespace bk2::android
