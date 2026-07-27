#include "3Dmotor/stdafx.h"

#include "3Dmotor/Gfx.h"
#include "Misc/2Darray.h"

#include "bk2_legacy_texture_probe.h"
#include "bk2_render_backend.h"

namespace NGfx {
namespace {

HWND g_window = 0;
SSystemInfo g_system_info = {};

}  // namespace

SRenderStats renderStats;

bool Init3D(HWND window) {
    g_window = window;
    return bk2::android::RenderBackend().attach_window(
            reinterpret_cast<ANativeWindow*>(window));
}

void Done3D() {
    bk2::android::RenderBackend().detach_window();
    g_window = 0;
}

HWND GetHWND() {
    return g_window;
}

bool Is3DActive() {
    return bk2::android::RenderBackend().is_ready();
}

void SetGamma(bool) {
}

void SetGammaRamp(const vector<NGfx::SPixel8888>&) {
}

bool SetMode(const SVideoMode& mode, const SRenderTargetsInfo&) {
    bk2::android::RenderBackend().resize(
            static_cast<uint32_t>(Max(mode.nXSize, 1)),
            static_cast<uint32_t>(Max(mode.nYSize, 1)));
    return bk2::android::RenderBackend().is_ready();
}

void GetModesList(list<SVideoMode>* result, int bits_per_pixel) {
    if (result == 0) {
        return;
    }
    result->clear();
    const bk2::android::IRenderBackend& backend =
            bk2::android::RenderBackend();
    result->push_back(SVideoMode(
            static_cast<int>(backend.width()),
            static_cast<int>(backend.height()),
            bits_per_pixel,
            FULL_SCREEN));
}

int GetMaxAnisotropicLevel() {
    return 1;
}

CVec2 GetScreenRect() {
    const bk2::android::IRenderBackend& backend =
            bk2::android::RenderBackend();
    return CVec2(
            static_cast<float>(backend.width()),
            static_cast<float>(backend.height()));
}

void Flip() {
    bk2::android::RenderBackend().render_frame();
    renderStats.Clear();
}

void MakeScreenShot(CArray2D<SPixel8888>* result, bool) {
    if (result != 0) {
        result->SetSizes(0, 0);
    }
}

void MakeScreenShotHQ(CArray2D<SPixel8888>* result, bool correct_gamma) {
    MakeScreenShot(result, correct_gamma);
}

void MakeFast32BitScreenShot(
        CArray2D<SPixel8888>* result, bool correct_gamma) {
    MakeScreenShot(result, correct_gamma);
}

void CheckBackBufferSize() {
}

int GetDeviceCreationID() {
    return Is3DActive() ? 1 : 0;
}

const SSystemInfo& GetSystemInfo() {
    return g_system_info;
}

bool Is16BitMode() {
    return false;
}

bool Is16BitDesktop() {
    return false;
}

bool IsDXTSupported() {
    return true;
}

bool Is8888FormatSupported() {
    return true;
}

const int GetAdapterToUse() {
    return 0;
}

void D3DASSERT(HRESULT, const char*, ...) {
}

}  // namespace NGfx

namespace bk2::android {

bool InitializeLegacyGfx(ANativeWindow* window) {
    return NGfx::Init3D(reinterpret_cast<HWND>(window));
}

void ResizeLegacyGfx(uint32_t width, uint32_t height) {
    NGfx::SRenderTargetsInfo render_targets;
    NGfx::SetMode(
            NGfx::SVideoMode(
                    static_cast<int>(width),
                    static_cast<int>(height),
                    32,
                    NGfx::FULL_SCREEN),
            render_targets);
}

void RenderLegacyGfxFrame() {
    NGfx::Flip();
}

void ShutdownLegacyGfx() {
#if defined(BK2_LEGACY_TEXTURE_RUNTIME_ENABLED)
    ReleaseLegacyTextureGpuResources();
#endif
    NGfx::Done3D();
}

}  // namespace bk2::android
