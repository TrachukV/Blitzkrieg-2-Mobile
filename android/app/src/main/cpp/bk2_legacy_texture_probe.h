#pragma once

#include <cstdint>
#include <string>

namespace NGfx {
class CTexture;
}  // namespace NGfx

namespace bk2::android {

std::string RunLegacyTextureProbe();
void QueueLegacyTextureSmokeOverlay(float x, float y, float width, float height);
void QueueLegacy2DQuadsSmokeOverlay(float x, float y, float width, float height);
void ReleaseLegacyTextureGpuResources();
void EnsureLegacyTextureUploaded(NGfx::CTexture* texture, int level);
void ConfigureLegacyTerrainTexture(NGfx::CTexture* texture);
uint16_t LegacyTextureHandleIndex(NGfx::CTexture* texture);
NGfx::CTexture* LegacyTextureSmokeTexture();

}  // namespace bk2::android
