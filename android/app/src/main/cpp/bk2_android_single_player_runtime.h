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

bool IsSinglePlayerRuntimeReady();
std::string SinglePlayerRuntimeReport();

}  // namespace bk2::android
