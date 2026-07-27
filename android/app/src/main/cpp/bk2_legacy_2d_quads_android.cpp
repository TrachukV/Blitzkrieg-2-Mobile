#include <algorithm>
#include <cstdint>
#include <vector>

#include "3Dmotor/stdafx.h"

#include "Misc/2Darray.h"
#include "3Dmotor/GfxUtils.h"

#include "bk2_legacy_texture_probe.h"
#include "bk2_render_backend.h"

template<>
CObjectBase* CastToObjectBaseImpl<NGfx::CPixelShader>(
        NGfx::CPixelShader* shader,
        void*) {
    return reinterpret_cast<CObjectBase*>(shader);
}

template<>
NGfx::CPixelShader* CastToUserObjectImpl<NGfx::CPixelShader>(
        CObjectBase* object,
        NGfx::CPixelShader*,
        void*) {
    return reinterpret_cast<NGfx::CPixelShader*>(object);
}

template<>
CObjectBase* CastToObjectBaseImpl<NGfx::CVertexShader>(
        NGfx::CVertexShader* shader,
        void*) {
    return reinterpret_cast<CObjectBase*>(shader);
}

template<>
NGfx::CVertexShader* CastToUserObjectImpl<NGfx::CVertexShader>(
        CObjectBase* object,
        NGfx::CVertexShader*,
        void*) {
    return reinterpret_cast<NGfx::CVertexShader*>(object);
}

namespace NGfx {
namespace {

float RectWidth(const CTRect<float>& rect) {
    return rect.x2 - rect.x1;
}

float RectHeight(const CTRect<float>& rect) {
    return rect.y2 - rect.y1;
}

uint32_t AverageColor(const SPixel8888* colors) {
    if (colors == 0) {
        return 0xffffffffu;
    }
    uint32_t r = 0;
    uint32_t g = 0;
    uint32_t b = 0;
    uint32_t a = 0;
    for (int i = 0; i < 4; ++i) {
        r += colors[i].r;
        g += colors[i].g;
        b += colors[i].b;
        a += colors[i].a;
    }
    r /= 4;
    g /= 4;
    b /= 4;
    a /= 4;
    return (a << 24) | (r << 16) | (g << 8) | b;
}

CTRect<float> BoundsFromQuad(const CVec2* positions) {
    CTRect<float> rect;
    rect.x1 = positions[0].x;
    rect.x2 = positions[0].x;
    rect.y1 = positions[0].y;
    rect.y2 = positions[0].y;
    for (int i = 1; i < 4; ++i) {
        rect.x1 = Min(rect.x1, positions[i].x);
        rect.x2 = Max(rect.x2, positions[i].x);
        rect.y1 = Min(rect.y1, positions[i].y);
        rect.y2 = Max(rect.y2, positions[i].y);
    }
    return rect;
}

void QueueTextureRect(
        const CTRect<float>& target,
        CTexture* texture,
        const CTRect<float>& source,
        uint32_t argb) {
    STexturePlaceInfo place;
    CTexture* container = GetTextureContainer(texture, &place);
    if (!IsValid(container)) {
        return;
    }

    bk2::android::EnsureLegacyTextureUploaded(container, 0);
    const uint16_t texture_handle =
            bk2::android::LegacyTextureHandleIndex(container);
    if (texture_handle == UINT16_MAX) {
        return;
    }

    const float holder_width = static_cast<float>(Max(place.size.x, 1));
    const float holder_height = static_cast<float>(Max(place.size.y, 1));
    const float u0 =
            (static_cast<float>(place.place.x1) + source.x1) / holder_width;
    const float v0 =
            (static_cast<float>(place.place.y1) + source.y1) / holder_height;
    const float u1 =
            (static_cast<float>(place.place.x1) + source.x2) / holder_width;
    const float v1 =
            (static_cast<float>(place.place.y1) + source.y2) / holder_height;

    bk2::android::RenderBackend().queue_textured_rect(
            target.x1,
            target.y1,
            RectWidth(target),
            RectHeight(target),
            texture_handle,
            u0,
            v0,
            u1,
            v1,
            argb);
}

void QueueSolidRect(const CTRect<float>& target, uint32_t argb) {
    bk2::android::RenderBackend().queue_solid_rect(
            target.x1,
            target.y1,
            RectWidth(target),
            RectHeight(target),
            argb);
}

}  // namespace

EVideoCard GetVideoCard() {
    return VC_DEFAULT;
}

EHardwareLevel GetHardwareLevel() {
    return HL_TNL_DEVICE;
}

bool IsTnLDevice() {
    return true;
}

CRenderContext::CRenderContext()
        : alpha(COMBINE_NONE),
          stencil(STENCIL_NONE),
          colorWrite(COLORWRITE_ALL),
          depth(DEPTH_NONE),
          cull(CULL_NONE),
          fog(FOG_NONE),
          targetMode(RTM_SCREEN),
          nMipLevel(0),
          nRegister(0),
          pPixelShader(0),
          nVertexShader(0),
          pOutstandingStream(0) {
}

CRenderContext::~CRenderContext() {
}

void CRenderContext::SetTransform(const SFBTransform& value) {
    transform = value;
}

void CRenderContext::SetAlphaCombine(EAlphaCombineMode mode) {
    alpha = mode;
}

void CRenderContext::SetStencil(const SStencilMode& mode) {
    stencil = mode;
}

void CRenderContext::SetDepth(EDepthMode mode) {
    depth = mode;
}

void CRenderContext::SetCulling(ECullMode mode) {
    cull = mode;
}

void CRenderContext::SetColorWrite(EColorWriteMask mode) {
    colorWrite = mode;
}

void CRenderContext::SetFogParams(const SFogParams& mode) {
    fogParams = mode;
}

void CRenderContext::SetFog(EFogMode mode) {
    fog = mode;
}

void CRenderContext::SetScreenRT() {
    targetMode = RTM_SCREEN;
    pTarget = 0;
    pCubeTarget = 0;
}

void CRenderContext::SetTextureRT(CTexture* texture, int mip_level) {
    targetMode = RTM_TEXTURE;
    pTarget = texture;
    nMipLevel = mip_level;
}

void CRenderContext::SetCubeTextureRT(
        CCubeTexture* texture,
        EFace,
        int mip_level) {
    targetMode = RTM_CUBETEXTURE;
    pCubeTarget = texture;
    nMipLevel = mip_level;
}

void CRenderContext::SetVirtualRT() {
    targetMode = RTM_REGISTERS;
}

void CRenderContext::SetRegister(int render_register) {
    targetMode = RTM_REGISTERS;
    nRegister = render_register;
}

void CRenderContext::ClearBuffers(DWORD) {
}

void CRenderContext::ClearTarget(DWORD) {
}

void CRenderContext::ClearZBuffer() {
}

void CRenderContext::SetPixelShader(const string&) {
}

void CRenderContext::SetPixelShader(const SPShader&) {
}

void CRenderContext::SetVertexShader(const string&) {
}

void CRenderContext::SetVertexShader(const SVShader&) {
}

void CRenderContext::SetVertexShader(ETnLVS shader) {
    nVertexShader = shader;
}

bool CRenderContext::SetShader(const SHLSLShader&) {
    return false;
}

void CRenderContext::SetVSConst(int, const CVec4*, int) const {
}

void CRenderContext::SetVSConst(int, const CVec3&) const {
}

void CRenderContext::SetTnlVertexColor(const CVec4&) {
}

void CRenderContext::SetTnlTexTransform(const SHMatrix&) {
}

void CRenderContext::SetPSConst(int, const CVec4*, int) {
}

void CRenderContext::SetPSConst(int, const CVec3&) {
}

void CRenderContext::SetAlphaRef(int) {
}

void CRenderContext::SetTexture(int, CTexture*, EFilterMode) {
}

void CRenderContext::SetTexture(int, CCubeTexture*) {
}

void CRenderContext::Use() const {
}

void CRenderContext::DrawPrimitive(CGeometry*, CTriList*, int, int) {
}

void CRenderContext::AddPrimitive(CGeometry*, CTriList*) {
}

void CRenderContext::AddPrimitive(CGeometry*, const STriangleList&) {
}

void CRenderContext::AddPrimitive(CGeometry*, const STriangleList*, int, unsigned) {
}

void CRenderContext::Flush() {
}

void CRenderContext::AddLineStrip(CGeometry*, const unsigned short*, int) {
}

void CRenderContext::ApplyRenderTarget() const {
}

void CRenderContext::StartStream(CGeometry*) {
}

void CRenderContext::CheckStream(CGeometry*) {
}

CTexture* GetRegisterTexture(int) {
    return 0;
}

CTexture* GetDepthRegisterTexture() {
    return 0;
}

bool CopyScreenToRegister(int) {
    return false;
}

void CopyScreenToTexture(CTexture*) {
}

IQuery* CreateOcclusionQuery() {
    return 0;
}

void SetWireframe(EWireframe) {
}

void GetRegisterSize(CTRect<float>* result) {
    if (result != 0) {
        result->Set(0.0f, 0.0f, 0.0f, 0.0f);
    }
}

bool IsNVidiaNP2Bug() {
    return false;
}

bool DoesSupportOcclusionQueries() {
    return false;
}

void SetDithering(EDithering) {
}

C2DQuadsRenderer::C2DQuadsRenderer(
        const CRenderContext& render_context,
        const CVec2& size,
        int depth_mode)
        : dm(depth_mode), rc(render_context), pLock(0) {
    SetupRC(size);
}

void C2DQuadsRenderer::SetTarget(
        const CRenderContext& render_context,
        const CVec2& size,
        int depth_mode) {
    Flush();
    dm = depth_mode;
    rc = render_context;
    SetupRC(size);
}

void C2DQuadsRenderer::SetTarget(CTexture*, const CVec2& size, int depth_mode) {
    Flush();
    dm = depth_mode;
    SetupRC(size);
}

void C2DQuadsRenderer::SetTarget(const CVec2& size, int depth_mode) {
    Flush();
    dm = depth_mode;
    SetupRC(size);
}

void C2DQuadsRenderer::SetupRC(const CVec2&) {
}

S2DRectInfoLock* C2DQuadsRenderer::GetRectInfoLock(
        CTexture*,
        const STexturePlaceInfo&) {
    return 0;
}

void C2DQuadsRenderer::FillRect(
        const CVec2* positions,
        const SPixel8888* colors,
        const CTRect<float>& source,
        float,
        float,
        float,
        CTexture* texture,
        const STexturePlaceInfo&) {
    const CTRect<float> target = BoundsFromQuad(positions);
    const uint32_t argb = AverageColor(colors);
    if ((dm & QRM_RENDER_MASK) == QRM_SOLID || !IsValid(texture)) {
        QueueSolidRect(target, argb);
    } else {
        QueueTextureRect(target, texture, source, argb);
    }
}

void C2DQuadsRenderer::AddRect(
        const CTRect<float>& target,
        CTexture* texture,
        const CTRect<float>& source,
        SPixel8888 color,
        float) {
    if (RectWidth(target) == 0.0f || RectHeight(target) == 0.0f) {
        return;
    }
    if ((dm & QRM_RENDER_MASK) == QRM_SOLID || !IsValid(texture)) {
        QueueSolidRect(target, color.dwColor);
    } else {
        QueueTextureRect(target, texture, source, color.dwColor);
    }
}

void C2DQuadsRenderer::AddRect(
        const CVec2* positions,
        const SPixel8888* colors,
        CTexture* texture,
        const CTRect<float>& source,
        float) {
    if (positions == 0) {
        return;
    }
    const CTRect<float> target = BoundsFromQuad(positions);
    const uint32_t argb = AverageColor(colors);
    if ((dm & QRM_RENDER_MASK) == QRM_SOLID || !IsValid(texture)) {
        QueueSolidRect(target, argb);
    } else {
        QueueTextureRect(target, texture, source, argb);
    }
}

void C2DQuadsRenderer::Flush() {
}

void MakeQuadTriList(int rects, STriangleList* result) {
    static std::vector<STriangle> triangles;
    const int clamped_rects = Max(0, Min(rects, N_MAX_RECTANGLES));
    triangles.resize(static_cast<size_t>(clamped_rects) * 2);
    for (int i = 0; i < clamped_rects; ++i) {
        const WORD start = static_cast<WORD>(i * 4);
        triangles[static_cast<size_t>(i) * 2 + 0] =
                STriangle(start + 0, start + 1, start + 2);
        triangles[static_cast<size_t>(i) * 2 + 1] =
                STriangle(start + 0, start + 2, start + 3);
    }
    if (result != 0) {
        *result = STriangleList(
                triangles.empty() ? 0 : &triangles[0],
                clamped_rects * 2,
                0);
    }
}

void CopyTexture(
        const CRenderContext& render_context,
        const CVec2& target_viewport,
        const CTRect<float>& target,
        CTexture* texture,
        const CTRect<float>& source,
        const CVec4& color,
        I2DEffect*) {
    SPixel8888 packed;
    packed.dwColor = GetDWORDColor(color);
    C2DQuadsRenderer renderer(render_context, target_viewport, QRM_DEPTH_NONE);
    renderer.AddRect(target, texture, source, packed);
}

void ShowTexture(CTexture* texture, float magnification) {
    if (!IsValid(texture)) {
        return;
    }
    const float scale = magnification == 0.0f ? 1.0f : magnification;
    CDynamicCast<I2DBuffer> buffer(texture);
    CTRect<float> rect(
            0.0f,
            0.0f,
            buffer->GetSizeX() * scale,
            buffer->GetSizeY() * scale);
    C2DQuadsRenderer renderer;
    renderer.SetTarget(CVec2(rect.x2, rect.y2), QRM_NOCOLOR);
    renderer.AddRect(rect, texture, CTRect<float>(0.0f, 0.0f, rect.x2, rect.y2));
}

void ShowTexture(CRenderContext* render_context, CTexture* texture, const CVec2& size) {
    if (render_context == 0 || !IsValid(texture)) {
        return;
    }
    CDynamicCast<I2DBuffer> buffer(texture);
    CTRect<float> source(
            0.0f,
            0.0f,
            static_cast<float>(buffer->GetSizeX()),
            static_cast<float>(buffer->GetSizeY()));
    CTRect<float> target(0.0f, 0.0f, size.x, size.y);
    C2DQuadsRenderer renderer(*render_context, size, QRM_NOCOLOR);
    renderer.AddRect(target, texture, source);
}

void CShowAlphaEffect::SetEffect(CRenderContext*, CTexture*, float, float) {
}

void CLinearToGammaEffect::SetEffect(CRenderContext*, CTexture*, float, float) {
}

void CShadow16toAlphaEffect::SetEffect(CRenderContext*, CTexture*, float, float) {
}

void CCopyShadowsAndCloudsEffect::SetEffect(CRenderContext*, CTexture*, float, float) {
}

}  // namespace NGfx

namespace bk2::android {

void QueueLegacy2DQuadsSmokeOverlay(float x, float y, float width, float height) {
    NGfx::CTexture* texture = LegacyTextureSmokeTexture();
    if (!IsValid(texture)) {
        return;
    }

    CDynamicCast<NGfx::I2DBuffer> buffer(texture);
    if (!IsValid(buffer)) {
        return;
    }

    NGfx::C2DQuadsRenderer renderer;
    renderer.SetTarget(
            CVec2(
                    static_cast<float>(RenderBackend().width()),
                    static_cast<float>(RenderBackend().height())),
            NGfx::QRM_NOCOLOR | NGfx::QRM_DEPTH_NONE);
    renderer.AddRect(
            CTRect<float>(x, y, x + width, y + height),
            texture,
            CTRect<float>(
                    0.0f,
                    0.0f,
                    static_cast<float>(buffer->GetSizeX()),
                    static_cast<float>(buffer->GetSizeY())),
            NGfx::SPixel8888(255, 255, 255, 255));
}

}  // namespace bk2::android
