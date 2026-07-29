#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace NGfx {
class CTexture;
}  // namespace NGfx

namespace bk2::android {

std::string RunLegacyTextureProbe();
void QueueLegacyTextureSmokeOverlay(float x, float y, float width, float height);
void QueueLegacy2DQuadsSmokeOverlay(float x, float y, float width, float height);
void ReleaseLegacyTextureGpuResources();
void EnsureLegacyTextureUploaded(NGfx::CTexture* texture, int level);
void EnsureLegacyTextureMipChainUploaded(NGfx::CTexture* texture);
void ConfigureLegacyTerrainTexture(NGfx::CTexture* texture);
void ConfigureLegacyLuminanceAlphaTexture(NGfx::CTexture* texture);
uint16_t LegacyTextureHandleIndex(NGfx::CTexture* texture);
NGfx::CTexture* LegacyTextureSmokeTexture();
bool CopyLegacyTextureArgb(
        NGfx::CTexture* texture,
        std::vector<uint32_t>* pixels,
        int* width,
        int* height);

}  // namespace bk2::android
