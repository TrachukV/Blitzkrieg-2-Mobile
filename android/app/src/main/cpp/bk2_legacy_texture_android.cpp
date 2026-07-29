#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include <sys/stat.h>

#if defined(BK2_BGFX_RENDERER_ENABLED)
#include <bgfx/bgfx.h>
#endif

#include "3Dmotor/stdafx.h"

#include "Misc/2Darray.h"
#include "3Dmotor/GfxBuffers.h"
#include "3Dmotor/GTexture.h"
#include "Image/DDS.h"
#include "libdb/Database.h"

#include "bk2_legacy_texture_probe.h"
#include "bk2_port_paths.h"
#include "bk2_render_backend.h"

namespace NGfx {

struct SAndroidTextureLevel {
    int width = 1;
    int height = 1;
    int stride = 0;
    int storage_rows = 1;
    std::vector<uint8_t> bytes;
};

std::vector<INew2DTexAllocCallback*> g_texture_alloc_callbacks;
std::vector<CTexture*> g_live_textures;

int PositiveSize(int value) {
    return value > 0 ? value : 1;
}

int MipSize(int size, int level) {
    const int shifted = size >> level;
    return shifted > 0 ? shifted : 1;
}

int PixelBlockWidth(int pixel_id) {
    switch (pixel_id) {
        case SPixelDXT1::ID:
        case SPixelDXT2::ID:
        case SPixelDXT3::ID:
        case SPixelDXT4::ID:
        case SPixelDXT5::ID:
            return 4;
        default:
            return 1;
    }
}

int PixelBlockHeight(int pixel_id) {
    switch (pixel_id) {
        case SPixelDXT1::ID:
        case SPixelDXT2::ID:
        case SPixelDXT3::ID:
        case SPixelDXT4::ID:
        case SPixelDXT5::ID:
            return 4;
        default:
            return 1;
    }
}

int PixelBlockBytes(int pixel_id) {
    switch (pixel_id) {
        case SPixel8888::ID:
            return 4;
        case SPixel4444::ID:
        case SPixel565::ID:
        case SPixel1555::ID:
            return 2;
        case SPixelFloat::ID:
            return 4;
        case SPixelFFFF::ID:
            return 16;
        case SPixelDXT1::ID:
            return 8;
        case SPixelDXT2::ID:
        case SPixelDXT3::ID:
        case SPixelDXT4::ID:
        case SPixelDXT5::ID:
            return 16;
        default:
            return 4;
    }
}

int StorageColumns(int pixel_id, int width) {
    const int block_width = PixelBlockWidth(pixel_id);
    return (PositiveSize(width) + block_width - 1) / block_width;
}

int StorageRows(int pixel_id, int height) {
    const int block_height = PixelBlockHeight(pixel_id);
    return (PositiveSize(height) + block_height - 1) / block_height;
}

int LevelStride(int pixel_id, int width) {
    return StorageColumns(pixel_id, width) * PixelBlockBytes(pixel_id);
}

bool CanUploadAsRgba8(int pixel_id) {
    return pixel_id == SPixel8888::ID ||
           pixel_id == SPixel4444::ID ||
           pixel_id == SPixel565::ID ||
           pixel_id == SPixel1555::ID ||
           pixel_id == SPixelDXT1::ID ||
           pixel_id == SPixelDXT2::ID ||
           pixel_id == SPixelDXT3::ID ||
           pixel_id == SPixelDXT4::ID ||
           pixel_id == SPixelDXT5::ID;
}

bool IsDxtFormat(int pixel_id) {
    return pixel_id == SPixelDXT1::ID ||
           pixel_id == SPixelDXT2::ID ||
           pixel_id == SPixelDXT3::ID ||
           pixel_id == SPixelDXT4::ID ||
           pixel_id == SPixelDXT5::ID;
}

uint8_t Expand4(uint32_t value) {
    return static_cast<uint8_t>((value & 0x0f) * 17);
}

uint8_t Expand5(uint32_t value) {
    return static_cast<uint8_t>(((value & 0x1f) * 255 + 15) / 31);
}

uint8_t Expand6(uint32_t value) {
    return static_cast<uint8_t>(((value & 0x3f) * 255 + 31) / 63);
}

uint16_t ReadLE16(const uint8_t* data) {
    return static_cast<uint16_t>(data[0]) |
           (static_cast<uint16_t>(data[1]) << 8);
}

uint32_t ReadLE32(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) |
           (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) |
           (static_cast<uint32_t>(data[3]) << 24);
}

uint64_t ReadLE64(const uint8_t* data) {
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(data[i]) << (i * 8);
    }
    return value;
}

SPixel8888 DecodeRgb565(uint16_t value) {
    return SPixel8888(
            Expand5((value >> 11) & 0x1f),
            Expand6((value >> 5) & 0x3f),
            Expand5(value & 0x1f),
            0xff);
}

uint8_t MixByte(uint8_t first, int first_weight, uint8_t second, int second_weight, int divisor) {
    return static_cast<uint8_t>(
            (static_cast<int>(first) * first_weight +
             static_cast<int>(second) * second_weight) /
            divisor);
}

void BuildDxtColorPalette(
        uint16_t color0,
        uint16_t color1,
        bool force_four_color,
        SPixel8888* palette) {
    palette[0] = DecodeRgb565(color0);
    palette[1] = DecodeRgb565(color1);
    if (force_four_color || color0 > color1) {
        palette[2] = SPixel8888(
                MixByte(palette[0].r, 2, palette[1].r, 1, 3),
                MixByte(palette[0].g, 2, palette[1].g, 1, 3),
                MixByte(palette[0].b, 2, palette[1].b, 1, 3),
                0xff);
        palette[3] = SPixel8888(
                MixByte(palette[0].r, 1, palette[1].r, 2, 3),
                MixByte(palette[0].g, 1, palette[1].g, 2, 3),
                MixByte(palette[0].b, 1, palette[1].b, 2, 3),
                0xff);
    } else {
        palette[2] = SPixel8888(
                MixByte(palette[0].r, 1, palette[1].r, 1, 2),
                MixByte(palette[0].g, 1, palette[1].g, 1, 2),
                MixByte(palette[0].b, 1, palette[1].b, 1, 2),
                0xff);
        palette[3] = SPixel8888(0, 0, 0, 0);
    }
}

void DecodeDxt3Alpha(const uint8_t* block, uint8_t* alpha) {
    const uint64_t bits = ReadLE64(block);
    for (int i = 0; i < 16; ++i) {
        alpha[i] = Expand4(static_cast<uint32_t>((bits >> (i * 4)) & 0x0f));
    }
}

void DecodeDxt5Alpha(const uint8_t* block, uint8_t* alpha) {
    uint8_t palette[8];
    palette[0] = block[0];
    palette[1] = block[1];
    if (palette[0] > palette[1]) {
        palette[2] = MixByte(palette[0], 6, palette[1], 1, 7);
        palette[3] = MixByte(palette[0], 5, palette[1], 2, 7);
        palette[4] = MixByte(palette[0], 4, palette[1], 3, 7);
        palette[5] = MixByte(palette[0], 3, palette[1], 4, 7);
        palette[6] = MixByte(palette[0], 2, palette[1], 5, 7);
        palette[7] = MixByte(palette[0], 1, palette[1], 6, 7);
    } else {
        palette[2] = MixByte(palette[0], 4, palette[1], 1, 5);
        palette[3] = MixByte(palette[0], 3, palette[1], 2, 5);
        palette[4] = MixByte(palette[0], 2, palette[1], 3, 5);
        palette[5] = MixByte(palette[0], 1, palette[1], 4, 5);
        palette[6] = 0;
        palette[7] = 255;
    }

    uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) {
        bits |= static_cast<uint64_t>(block[2 + i]) << (i * 8);
    }
    for (int i = 0; i < 16; ++i) {
        alpha[i] = palette[(bits >> (i * 3)) & 0x07];
    }
}

void RegisterTexture(CTexture* texture) {
    g_live_textures.push_back(texture);
}

void UnregisterTexture(CTexture* texture) {
    g_live_textures.erase(
            std::remove(g_live_textures.begin(), g_live_textures.end(), texture),
            g_live_textures.end());
}

void NotifyNew2DTextureAllocated() {
    for (size_t i = 0; i < g_texture_alloc_callbacks.size(); ++i) {
        if (g_texture_alloc_callbacks[i] != 0) {
            g_texture_alloc_callbacks[i]->NewTextureWasAllocated();
        }
    }
}

int LinearElementStride(int format_id) {
    switch (format_id) {
        case S3DTriangle::ID:
            return sizeof(S3DTriangle);
        case SGeomVecT2C1::ID:
            return sizeof(SGeomVecT2C1);
        case SGeomVecFull::ID:
            return sizeof(SGeomVecFull);
        default:
            return 1;
    }
}

class CAndroidLinearBuffer : public ILinearBuffer {
public:
    CAndroidLinearBuffer()
            : format_id_(0), size_(0), buffer_size_(0), stride_(1) {
    }

    CAndroidLinearBuffer(int format_id, int size, EBufferUsage)
            : format_id_(format_id),
              size_(PositiveSize(size)),
              buffer_size_(PositiveSize(size)),
              stride_(LinearElementStride(format_id)) {
        bytes_.resize(static_cast<size_t>(buffer_size_) * stride_);
    }

    int GetFormatID() const override {
        return format_id_;
    }

    void SetSize(int size) override {
        size_ = size > 0 ? size : 0;
        if (size_ > buffer_size_) {
            buffer_size_ = size_;
            bytes_.resize(static_cast<size_t>(buffer_size_) * stride_);
        }
    }

    int GetSize() const override {
        return size_;
    }

    int GetBufSize() const override {
        return buffer_size_;
    }

    void* Lock() override {
        if (bytes_.empty()) {
            return 0;
        }
        return &bytes_[0];
    }

    void Unlock() override {
    }

protected:
    OBJECT_NOCOPY_METHODS(CAndroidLinearBuffer);

private:
    int format_id_;
    int size_;
    int buffer_size_;
    int stride_;
    std::vector<uint8_t> bytes_;
};

class CTexture : public I2DBuffer {
public:
    CTexture()
            : size_x_(1),
              size_y_(1),
              mip_levels_(1),
              pixel_id_(SPixel8888::ID),
              usage_(REGULAR),
              wrap_(CLAMP),
              frame_mru_(0),
              uploaded_levels_(0),
              force_opaque_(false),
              derive_alpha_from_luminance_(false),
              force_base_level_only_(false) {
#if defined(BK2_BGFX_RENDERER_ENABLED)
        texture_handle_ = BGFX_INVALID_HANDLE;
#endif
        RebuildStorage();
        RegisterTexture(this);
    }

    CTexture(
            int size_x,
            int size_y,
            int mip_levels,
            int pixel_id,
            ETextureUsage usage,
            EWrap wrap)
            : size_x_(PositiveSize(size_x)),
              size_y_(PositiveSize(size_y)),
              mip_levels_(PositiveSize(mip_levels)),
              pixel_id_(pixel_id),
              usage_(usage),
              wrap_(wrap),
              frame_mru_(0),
              uploaded_levels_(0),
              force_opaque_(false),
              derive_alpha_from_luminance_(false),
              force_base_level_only_(false) {
#if defined(BK2_BGFX_RENDERER_ENABLED)
        texture_handle_ = BGFX_INVALID_HANDLE;
#endif
        RebuildStorage();
        RegisterTexture(this);
    }

    int GetPixelID() override {
        return pixel_id_;
    }

    I2DBufferLock* Lock(int level, EAccess access) override;

    int GetSizeX() const override {
        return size_x_;
    }

    int GetSizeY() const override {
        return size_y_;
    }

    int GetNumMipLevels() const override {
        return mip_levels_;
    }

    int GetFrameMRU() const override {
        return frame_mru_;
    }

    void UserTouch() override {
        ++frame_mru_;
    }

    void Touch() {
        UserTouch();
    }

    int GetStride(int level) const {
        if (level < 0 || level >= static_cast<int>(levels_.size())) {
            return 0;
        }
        return levels_[level].stride;
    }

    size_t CpuStorageBytes() const {
        size_t total = 0;
        for (size_t i = 0; i < levels_.size(); ++i) {
            total += levels_[i].bytes.size();
        }
        return total;
    }

    int UploadedLevelCount() const {
        return uploaded_levels_;
    }

    bool HasGpuHandle() const {
#if defined(BK2_BGFX_RENDERER_ENABLED)
        return bgfx::isValid(texture_handle_);
#else
        return false;
#endif
    }

    uint16_t NativeTextureHandleIndex() const {
#if defined(BK2_BGFX_RENDERER_ENABLED)
        return bgfx::isValid(texture_handle_) ? texture_handle_.idx : UINT16_MAX;
#else
        return UINT16_MAX;
#endif
    }

    void ReleaseGpuTexture() {
        DestroyGpuTexture();
    }

    void ConfigureTerrainSampling() {
        if (force_opaque_ && force_base_level_only_) {
            return;
        }
        force_opaque_ = true;
        force_base_level_only_ = true;
        DestroyGpuTexture();
        UploadLevel(0);
    }

    void ConfigureLuminanceAlphaSampling() {
        if (derive_alpha_from_luminance_) {
            return;
        }
        derive_alpha_from_luminance_ = true;
        DestroyGpuTexture();
        UploadMipChain();
    }

    uint8_t* MutableLevelBytes(int level) {
        if (level < 0 || level >= static_cast<int>(levels_.size())) {
            return 0;
        }
        if (levels_[level].bytes.empty()) {
            return 0;
        }
        return &levels_[level].bytes[0];
    }

    const SAndroidTextureLevel* Level(int level) const {
        if (level < 0 || level >= static_cast<int>(levels_.size())) {
            return 0;
        }
        return &levels_[level];
    }

    void UploadLevel(int level) {
        if (level < 0 || level >= static_cast<int>(levels_.size())) {
            return;
        }
        if (force_base_level_only_ && level != 0) {
            return;
        }
        if (!CanUploadAsRgba8(pixel_id_)) {
            return;
        }
#if defined(BK2_BGFX_RENDERER_ENABLED)
        if (!bk2::android::RenderBackend().is_ready()) {
            return;
        }
        if (!EnsureGpuTexture()) {
            return;
        }

        const SAndroidTextureLevel& src = levels_[level];
        std::vector<uint8_t> rgba;
        ConvertLevelToRgba8(src, &rgba);
        if (rgba.empty()) {
            return;
        }

        const bgfx::Memory* memory =
                bgfx::copy(&rgba[0], static_cast<uint32_t>(rgba.size()));
        bgfx::updateTexture2D(
                texture_handle_,
                0,
                static_cast<uint8_t>(level),
                0,
                0,
                static_cast<uint16_t>(src.width),
                static_cast<uint16_t>(src.height),
                memory,
                static_cast<uint16_t>(src.width * 4));
        if (level + 1 > uploaded_levels_) {
            uploaded_levels_ = level + 1;
        }
#endif
    }

    void UploadMipChain() {
        if (force_base_level_only_) {
            if (uploaded_levels_ < 1) {
                UploadLevel(0);
            }
            return;
        }
        if (uploaded_levels_ >= mip_levels_) {
            return;
        }
        for (int level = 0; level < mip_levels_; ++level) {
            UploadLevel(level);
        }
    }

    bool CopyLevelAs8888(CArray2D<SPixel8888>* result, int level) const {
        if (result == 0) {
            return false;
        }
        const SAndroidTextureLevel* src = Level(level);
        if (src == 0 || !CanUploadAsRgba8(pixel_id_)) {
            result->Clear();
            return false;
        }
        std::vector<uint8_t> rgba;
        ConvertLevelToRgba8(*src, &rgba);
        if (rgba.empty()) {
            result->Clear();
            return false;
        }
        result->SetSizes(src->width, src->height);
        for (int y = 0; y < src->height; ++y) {
            for (int x = 0; x < src->width; ++x) {
                const size_t offset =
                        (static_cast<size_t>(y) * src->width + x) * 4;
                (*result)[y][x] = SPixel8888(
                        rgba[offset + 0],
                        rgba[offset + 1],
                        rgba[offset + 2],
                        rgba[offset + 3]);
            }
        }
        return true;
    }

    bool CopyLevelFrom(const CTexture* source, int level) {
        if (source == 0 || level < 0 ||
            level >= static_cast<int>(levels_.size()) ||
            level >= static_cast<int>(source->levels_.size())) {
            return false;
        }
        if (pixel_id_ != source->pixel_id_ ||
            levels_[level].width != source->levels_[level].width ||
            levels_[level].height != source->levels_[level].height ||
            levels_[level].stride != source->levels_[level].stride) {
            return false;
        }
        levels_[level].bytes = source->levels_[level].bytes;
        UploadLevel(level);
        return true;
    }

protected:
    ~CTexture() override {
        DestroyGpuTexture();
        UnregisterTexture(this);
    }

    OBJECT_NOCOPY_METHODS(CTexture);

private:
    void RebuildStorage() {
        levels_.clear();
        levels_.resize(static_cast<size_t>(mip_levels_));
        for (int level = 0; level < mip_levels_; ++level) {
            SAndroidTextureLevel& dst = levels_[level];
            dst.width = MipSize(size_x_, level);
            dst.height = MipSize(size_y_, level);
            dst.stride = LevelStride(pixel_id_, dst.width);
            dst.storage_rows = StorageRows(pixel_id_, dst.height);
            dst.bytes.assign(
                    static_cast<size_t>(dst.stride) * dst.storage_rows,
                    0);
        }
    }

    SPixel8888 ReadPixelAs8888(const uint8_t* row, int x) const {
        switch (pixel_id_) {
            case SPixel8888::ID: {
                const uint8_t* pixel = row + x * 4;
                return SPixel8888(pixel[2], pixel[1], pixel[0], pixel[3]);
            }
            case SPixel4444::ID: {
                const uint16_t value =
                        static_cast<uint16_t>(row[x * 2]) |
                        (static_cast<uint16_t>(row[x * 2 + 1]) << 8);
                return SPixel8888(
                        Expand4((value >> 8) & 0x0f),
                        Expand4((value >> 4) & 0x0f),
                        Expand4(value & 0x0f),
                        Expand4((value >> 12) & 0x0f));
            }
            case SPixel565::ID: {
                const uint16_t value =
                        static_cast<uint16_t>(row[x * 2]) |
                        (static_cast<uint16_t>(row[x * 2 + 1]) << 8);
                return SPixel8888(
                        Expand5((value >> 11) & 0x1f),
                        Expand6((value >> 5) & 0x3f),
                        Expand5(value & 0x1f),
                        0xff);
            }
            case SPixel1555::ID: {
                const uint16_t value =
                        static_cast<uint16_t>(row[x * 2]) |
                        (static_cast<uint16_t>(row[x * 2 + 1]) << 8);
                return SPixel8888(
                        Expand5((value >> 10) & 0x1f),
                        Expand5((value >> 5) & 0x1f),
                        Expand5(value & 0x1f),
                        (value & 0x8000) != 0 ? 0xff : 0x00);
            }
            default:
                return SPixel8888(0, 0, 0, 0);
        }
    }

    void ConvertLevelToRgba8(
            const SAndroidTextureLevel& src,
            std::vector<uint8_t>* rgba) const {
        rgba->assign(static_cast<size_t>(src.width) * src.height * 4, 0);
        if (IsDxtFormat(pixel_id_)) {
            DecodeDxtLevelToRgba8(src, rgba);
        } else {
            for (int y = 0; y < src.height; ++y) {
                const uint8_t* row =
                        &src.bytes[static_cast<size_t>(y) * src.stride];
                for (int x = 0; x < src.width; ++x) {
                    const SPixel8888 pixel = ReadPixelAs8888(row, x);
                    const size_t offset =
                            (static_cast<size_t>(y) * src.width + x) * 4;
                    (*rgba)[offset + 0] = static_cast<uint8_t>(pixel.r);
                    (*rgba)[offset + 1] = static_cast<uint8_t>(pixel.g);
                    (*rgba)[offset + 2] = static_cast<uint8_t>(pixel.b);
                    (*rgba)[offset + 3] = static_cast<uint8_t>(pixel.a);
                }
            }
        }
        if (derive_alpha_from_luminance_) {
            for (size_t offset = 0; offset < rgba->size(); offset += 4) {
                (*rgba)[offset + 3] = std::max(
                        (*rgba)[offset + 0],
                        std::max(
                                (*rgba)[offset + 1],
                                (*rgba)[offset + 2]));
            }
        } else if (force_opaque_) {
            for (size_t offset = 3; offset < rgba->size(); offset += 4) {
                (*rgba)[offset] = 0xff;
            }
        }
    }

    void DecodeDxtLevelToRgba8(
            const SAndroidTextureLevel& src,
            std::vector<uint8_t>* rgba) const {
        const int block_bytes = PixelBlockBytes(pixel_id_);
        const int block_rows = StorageRows(pixel_id_, src.height);
        const int block_columns = StorageColumns(pixel_id_, src.width);
        for (int block_y = 0; block_y < block_rows; ++block_y) {
            for (int block_x = 0; block_x < block_columns; ++block_x) {
                const size_t block_offset =
                        static_cast<size_t>(block_y) * src.stride +
                        static_cast<size_t>(block_x) * block_bytes;
                if (block_offset + block_bytes > src.bytes.size()) {
                    continue;
                }
                const uint8_t* block = &src.bytes[block_offset];
                const uint8_t* color_block = block;
                uint8_t alpha[16];
                for (int i = 0; i < 16; ++i) {
                    alpha[i] = 255;
                }
                bool force_four_color = false;
                if (pixel_id_ == SPixelDXT2::ID || pixel_id_ == SPixelDXT3::ID) {
                    DecodeDxt3Alpha(block, alpha);
                    color_block = block + 8;
                    force_four_color = true;
                } else if (pixel_id_ == SPixelDXT4::ID || pixel_id_ == SPixelDXT5::ID) {
                    DecodeDxt5Alpha(block, alpha);
                    color_block = block + 8;
                    force_four_color = true;
                }

                SPixel8888 palette[4];
                BuildDxtColorPalette(
                        ReadLE16(color_block),
                        ReadLE16(color_block + 2),
                        force_four_color,
                        palette);
                const uint32_t color_bits = ReadLE32(color_block + 4);
                for (int py = 0; py < 4; ++py) {
                    for (int px = 0; px < 4; ++px) {
                        const int src_index = py * 4 + px;
                        const int dst_x = block_x * 4 + px;
                        const int dst_y = block_y * 4 + py;
                        if (dst_x >= src.width || dst_y >= src.height) {
                            continue;
                        }
                        const int color_index =
                                (color_bits >> (src_index * 2)) & 0x03;
                        SPixel8888 pixel = palette[color_index];
                        if (force_four_color) {
                            pixel.a = alpha[src_index];
                        }
                        const size_t dst_offset =
                                (static_cast<size_t>(dst_y) * src.width + dst_x) * 4;
                        (*rgba)[dst_offset + 0] = static_cast<uint8_t>(pixel.r);
                        (*rgba)[dst_offset + 1] = static_cast<uint8_t>(pixel.g);
                        (*rgba)[dst_offset + 2] = static_cast<uint8_t>(pixel.b);
                        (*rgba)[dst_offset + 3] = static_cast<uint8_t>(pixel.a);
                    }
                }
            }
        }
    }

    bool EnsureGpuTexture() {
#if defined(BK2_BGFX_RENDERER_ENABLED)
        if (bgfx::isValid(texture_handle_)) {
            return true;
        }
        if (!CanUploadAsRgba8(pixel_id_)) {
            return false;
        }
        uint64_t flags = BGFX_TEXTURE_NONE;
        if ((wrap_ & WRAP_X) == 0) {
            flags |= BGFX_SAMPLER_U_CLAMP;
        }
        if ((wrap_ & WRAP_Y) == 0) {
            flags |= BGFX_SAMPLER_V_CLAMP;
        }
        texture_handle_ = bgfx::createTexture2D(
                static_cast<uint16_t>(size_x_),
                static_cast<uint16_t>(size_y_),
                mip_levels_ > 1 && !force_base_level_only_,
                1,
                bgfx::TextureFormat::RGBA8,
                flags);
        return bgfx::isValid(texture_handle_);
#else
        return false;
#endif
    }

    void DestroyGpuTexture() {
#if defined(BK2_BGFX_RENDERER_ENABLED)
        if (bgfx::isValid(texture_handle_)) {
            if (bk2::android::RenderBackend().is_ready()) {
                bgfx::destroy(texture_handle_);
            }
            texture_handle_ = BGFX_INVALID_HANDLE;
        }
#endif
        uploaded_levels_ = 0;
    }

    int size_x_;
    int size_y_;
    int mip_levels_;
    int pixel_id_;
    ETextureUsage usage_;
    EWrap wrap_;
    int frame_mru_;
    int uploaded_levels_;
    bool force_opaque_;
    bool derive_alpha_from_luminance_;
    bool force_base_level_only_;
    std::vector<SAndroidTextureLevel> levels_;
#if defined(BK2_BGFX_RENDERER_ENABLED)
    bgfx::TextureHandle texture_handle_;
#endif
};

class CAndroidTextureLock : public I2DBufferLock {
public:
    CAndroidTextureLock(CTexture* texture, int level, EAccess access)
            : texture_(texture),
              level_(level),
              access_(access),
              buffer_(0),
              stride_(0) {
        if (IsValid(texture_)) {
            buffer_ = texture_->MutableLevelBytes(level_);
            stride_ = texture_->GetStride(level_);
        }
    }

    ~CAndroidTextureLock() override {
        if (IsValid(texture_) &&
            access_ != READONLY &&
            access_ != INPLACE_READONLY) {
            texture_->UploadLevel(level_);
        }
    }

    void* GetBuffer() override {
        return buffer_;
    }

    int GetStride() override {
        return stride_;
    }

private:
    CPtr<CTexture> texture_;
    int level_;
    EAccess access_;
    void* buffer_;
    int stride_;
};

class CCubeTexture : public ICubeBuffer {
public:
    CCubeTexture()
            : size_(1), mip_levels_(1), pixel_id_(SPixel8888::ID) {
        RebuildStorage();
    }

    CCubeTexture(int size, int mip_levels, int pixel_id)
            : size_(PositiveSize(size)),
              mip_levels_(PositiveSize(mip_levels)),
              pixel_id_(pixel_id) {
        RebuildStorage();
    }

    int GetPixelID() override {
        return pixel_id_;
    }

    I2DBufferLock* Lock(EFace face, int level, EAccess access) override;

    int GetSize() const override {
        return size_;
    }

    int GetNumMipLevels() const override {
        return mip_levels_;
    }

    uint8_t* MutableLevelBytes(EFace face, int level) {
        const int face_index = static_cast<int>(face);
        if (face_index < 0 || face_index >= 6 ||
            level < 0 || level >= mip_levels_) {
            return 0;
        }
        SAndroidTextureLevel& storage =
                faces_[face_index][static_cast<size_t>(level)];
        if (storage.bytes.empty()) {
            return 0;
        }
        return &storage.bytes[0];
    }

    int GetStride(int level) const {
        if (level < 0 || level >= mip_levels_) {
            return 0;
        }
        return faces_[0][static_cast<size_t>(level)].stride;
    }

protected:
    OBJECT_NOCOPY_METHODS(CCubeTexture);

private:
    void RebuildStorage() {
        for (int face = 0; face < 6; ++face) {
            faces_[face].clear();
            faces_[face].resize(static_cast<size_t>(mip_levels_));
            for (int level = 0; level < mip_levels_; ++level) {
                SAndroidTextureLevel& dst =
                        faces_[face][static_cast<size_t>(level)];
                dst.width = MipSize(size_, level);
                dst.height = MipSize(size_, level);
                dst.stride = LevelStride(pixel_id_, dst.width);
                dst.storage_rows = StorageRows(pixel_id_, dst.height);
                dst.bytes.assign(
                        static_cast<size_t>(dst.stride) * dst.storage_rows,
                        0);
            }
        }
    }

    int size_;
    int mip_levels_;
    int pixel_id_;
    std::vector<SAndroidTextureLevel> faces_[6];
};

class CAndroidCubeTextureLock : public I2DBufferLock {
public:
    CAndroidCubeTextureLock(
            CCubeTexture* texture,
            EFace face,
            int level,
            EAccess)
            : texture_(texture),
              face_(face),
              level_(level),
              buffer_(0),
              stride_(0) {
        if (IsValid(texture_)) {
            buffer_ = texture_->MutableLevelBytes(face_, level_);
            stride_ = texture_->GetStride(level_);
        }
    }

    void* GetBuffer() override {
        return buffer_;
    }

    int GetStride() override {
        return stride_;
    }

private:
    CPtr<CCubeTexture> texture_;
    EFace face_;
    int level_;
    void* buffer_;
    int stride_;
};

CObj<CTexture> g_texture_cache;
CObj<CTexture> g_transparent_texture_cache;
CObj<CTexture> g_linear_buffer_mru;
CObj<CTexture> g_probe_texture;

CTexture* EnsureCacheTexture(CObj<CTexture>* cache, int size, ETextureUsage usage) {
    if (!IsValid(*cache)) {
        *cache = new CTexture(size, size, 1, SPixel8888::ID, usage, CLAMP);
    }
    return cache->GetPtr();
}

CTexture* EnsureProbeTexture() {
    if (IsValid(g_probe_texture)) {
        return g_probe_texture.GetPtr();
    }

    g_probe_texture =
            MakeTexture(4, 4, 1, SPixel8888::ID, REGULAR, CLAMP);
    if (!IsValid(g_probe_texture)) {
        return 0;
    }

    CTextureLock<SPixel8888> lock(g_probe_texture, 0, INPLACE);
    for (int y = 0; y < lock.GetSizeY(); ++y) {
        for (int x = 0; x < lock.GetSizeX(); ++x) {
            const bool bright = ((x + y) & 1) == 0;
            lock[y][x] = bright
                    ? SPixel8888(216, 178, 76, 255)
                    : SPixel8888(79, 163, 122, 255);
        }
    }
    return g_probe_texture.GetPtr();
}

bool WriteProbeDds(const std::string& path, std::string* error) {
    SDDSFileHeader header;
    header.header.dwHeaderFlags =
            DDS_HEADER_FLAGS_TEXTURE | DDS_HEADER_FLAGS_PITCH;
    header.header.dwHeight = 4;
    header.header.dwWidth = 4;
    header.header.dwPitchOrLinearSize = 4 * sizeof(SPixel8888);
    header.header.dwDepth = 0;
    header.header.dwMipMapCount = 1;
    header.header.ddspf = DDSPF_A8R8G8B8;
    header.header.dwSurfaceFlags = DDS_SURFACE_FLAGS_TEXTURE;
    header.header.dwCubemapFlags = 0;

    FILE* file = std::fopen(path.c_str(), "wb");
    if (file == 0) {
        if (error != 0) {
            *error = std::string("open_failed_errno_") + std::to_string(errno);
        }
        return false;
    }

    const bool header_written =
            std::fwrite(&header, 1, sizeof(header), file) == sizeof(header);
    bool pixels_written = true;
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            const bool bright = ((x ^ y) & 1) == 0;
            const SPixel8888 pixel = bright
                    ? SPixel8888(192, 64, 48, 255)
                    : SPixel8888(40, 120, 200, 255);
            pixels_written =
                    pixels_written &&
                    std::fwrite(&pixel, 1, sizeof(pixel), file) ==
                            sizeof(pixel);
        }
    }
    const int close_result = std::fclose(file);
    if (!header_written || !pixels_written || close_result != 0) {
        if (error != 0) {
            *error = "write_failed";
        }
        return false;
    }
    return true;
}

void FillProbeDxtColorBlock(int block_index, uint8_t* block) {
    const uint16_t color_pairs[4][2] = {
            {0xf800, 0x07e0},  // red, green
            {0x001f, 0xffe0},  // blue, yellow
            {0xffff, 0x0000},  // white, black
            {0xf81f, 0x07ff},  // magenta, cyan
    };
    uint32_t color_indices = 0;
    for (int i = 0; i < 16; ++i) {
        color_indices |=
                static_cast<uint32_t>((i + block_index) & 0x03) << (i * 2);
    }
    const uint16_t color0 = color_pairs[block_index & 0x03][0];
    const uint16_t color1 = color_pairs[block_index & 0x03][1];
    block[0] = static_cast<uint8_t>(color0 & 0xff);
    block[1] = static_cast<uint8_t>(color0 >> 8);
    block[2] = static_cast<uint8_t>(color1 & 0xff);
    block[3] = static_cast<uint8_t>(color1 >> 8);
    block[4] = static_cast<uint8_t>(color_indices & 0xff);
    block[5] = static_cast<uint8_t>((color_indices >> 8) & 0xff);
    block[6] = static_cast<uint8_t>((color_indices >> 16) & 0xff);
    block[7] = static_cast<uint8_t>((color_indices >> 24) & 0xff);
}

bool WriteProbeDxtDds(const std::string& path, int pixel_id, std::string* error) {
    SDDSFileHeader header;
    header.header.dwHeaderFlags =
            DDS_HEADER_FLAGS_TEXTURE | DDS_HEADER_FLAGS_LINEARSIZE;
    header.header.dwHeight = 8;
    header.header.dwWidth = 8;
    header.header.dwPitchOrLinearSize =
            PixelBlockBytes(pixel_id) * StorageColumns(pixel_id, 8) * StorageRows(pixel_id, 8);
    header.header.dwDepth = 0;
    header.header.dwMipMapCount = 1;
    switch (pixel_id) {
        case SPixelDXT1::ID:
            header.header.ddspf = DDSPF_DXT1;
            break;
        case SPixelDXT3::ID:
            header.header.ddspf = DDSPF_DXT3;
            break;
        case SPixelDXT5::ID:
            header.header.ddspf = DDSPF_DXT5;
            break;
        default:
            if (error != 0) {
                *error = "unsupported_dxt_probe_format";
            }
            return false;
    }
    header.header.dwSurfaceFlags = DDS_SURFACE_FLAGS_TEXTURE;
    header.header.dwCubemapFlags = 0;

    FILE* file = std::fopen(path.c_str(), "wb");
    if (file == 0) {
        if (error != 0) {
            *error = std::string("open_failed_errno_") + std::to_string(errno);
        }
        return false;
    }

    const bool header_written =
            std::fwrite(&header, 1, sizeof(header), file) == sizeof(header);

    bool blocks_written = true;
    for (int block_index = 0; block_index < 4; ++block_index) {
        uint8_t block[16];
        int block_size = 8;
        if (pixel_id == SPixelDXT1::ID) {
            FillProbeDxtColorBlock(block_index, block);
        } else if (pixel_id == SPixelDXT3::ID) {
            uint64_t alpha_bits = 0;
            for (int i = 0; i < 16; ++i) {
                alpha_bits |=
                        static_cast<uint64_t>((i + block_index) & 0x0f) << (i * 4);
            }
            for (int i = 0; i < 8; ++i) {
                block[i] = static_cast<uint8_t>((alpha_bits >> (i * 8)) & 0xff);
            }
            FillProbeDxtColorBlock(block_index, block + 8);
            block_size = 16;
        } else {
            block[0] = 255;
            block[1] = 32;
            uint64_t alpha_indices = 0;
            for (int i = 0; i < 16; ++i) {
                alpha_indices |=
                        static_cast<uint64_t>((i + block_index) & 0x07) << (i * 3);
            }
            for (int i = 0; i < 6; ++i) {
                block[2 + i] =
                        static_cast<uint8_t>((alpha_indices >> (i * 8)) & 0xff);
            }
            FillProbeDxtColorBlock(block_index, block + 8);
            block_size = 16;
        }
        blocks_written =
                blocks_written &&
                std::fwrite(block, 1, block_size, file) ==
                        static_cast<size_t>(block_size);
    }
    const int close_result = std::fclose(file);
    if (!header_written || !blocks_written || close_result != 0) {
        if (error != 0) {
            *error = "write_failed";
        }
        return false;
    }
    return true;
}

INew2DTexAllocCallback::INew2DTexAllocCallback() {
    g_texture_alloc_callbacks.push_back(this);
}

INew2DTexAllocCallback::~INew2DTexAllocCallback() {
    g_texture_alloc_callbacks.erase(
            std::remove(
                    g_texture_alloc_callbacks.begin(),
                    g_texture_alloc_callbacks.end(),
                    this),
            g_texture_alloc_callbacks.end());
}

I2DBufferLock* CTexture::Lock(int level, EAccess access) {
    return new CAndroidTextureLock(this, level, access);
}

I2DBufferLock* CCubeTexture::Lock(EFace face, int level, EAccess access) {
    return new CAndroidCubeTextureLock(this, face, level, access);
}

ILinearBuffer* CreateBuffer(int format_id, int size, EBufferUsage usage) {
    return new CAndroidLinearBuffer(format_id, size, usage);
}

CTriList* MakeWrapper(CTriList* source, int) {
    return source;
}

CTexture* MakeTexture(
        int size_x,
        int size_y,
        int mip_levels,
        int pixel_id,
        ETextureUsage usage,
        EWrap wrap) {
    if (usage == TEXTURE_2D || usage == TRANSPARENT_TEXTURE) {
        NotifyNew2DTextureAllocated();
    }
    return new CTexture(size_x, size_y, mip_levels, pixel_id, usage, wrap);
}

CCubeTexture* MakeCubeTexture(
        int size,
        int mip_levels,
        int pixel_id,
        ETextureUsage usage) {
    (void)usage;
    return new CCubeTexture(size, mip_levels, pixel_id);
}

CTexture* GetTextureCache() {
    return EnsureCacheTexture(&g_texture_cache, 1024, TEXTURE_2D);
}

CTexture* GetTransparentTextureCache() {
    return EnsureCacheTexture(&g_transparent_texture_cache, 1024, TRANSPARENT_TEXTURE);
}

CTexture* GetTextureContainer(CTexture* texture, STexturePlaceInfo* place) {
    if (!IsValid(texture)) {
        return 0;
    }
    texture->Touch();
    if (place != 0) {
        place->place.x1 = 0;
        place->place.y1 = 0;
        place->place.x2 = texture->GetSizeX();
        place->place.y2 = texture->GetSizeY();
        place->size.x = texture->GetSizeX();
        place->size.y = texture->GetSizeY();
    }
    return texture;
}

CTexture* GetLinearBufferMRU(EBufferUsage) {
    return EnsureCacheTexture(&g_linear_buffer_mru, 1024, REGULAR);
}

bool HasSameContainer(CTexture* first, CTexture* second) {
    return first != 0 && first == second;
}

void GetRenderTargetData(CTexture* target, CTexture* source) {
    if (IsValid(target) && IsValid(source)) {
        target->CopyLevelFrom(source, 0);
    }
}

void GetRenderTargetData(CArray2D<SPixel8888>* result, CTexture* source) {
    if (!IsValid(source)) {
        if (result != 0) {
            result->Clear();
        }
        return;
    }
    source->CopyLevelAs8888(result, 0);
}

int CalcTouchedTextureSize() {
    size_t total = 0;
    for (size_t i = 0; i < g_live_textures.size(); ++i) {
        if (IsValid(g_live_textures[i])) {
            total += g_live_textures[i]->CpuStorageBytes();
        }
    }
    return static_cast<int>(total);
}

int CalcTouchedTextureSizeNotSetMip(int) {
    return CalcTouchedTextureSize();
}

int CalcTotalTextureSize(int* texture_count) {
    if (texture_count != 0) {
        *texture_count = static_cast<int>(g_live_textures.size());
    }
    return CalcTouchedTextureSize();
}

void SetLODToAllTextures(int) {
}

bool IsStaticGeometryThrashing() {
    return false;
}

bool IsDynamicGeometryThrashing() {
    return false;
}

bool Is2DTextureThrashing() {
    return false;
}

bool IsTransparentThrashing() {
    return false;
}

bool CanStreamGeometry() {
    return false;
}

bool Is16BitTextures() {
    return false;
}

void ReplaceTextureSurface(CTexture*, int, IDirect3DSurface9*) {
}

void FlushQueue() {
}

}  // namespace NGfx

BASIC_REGISTER_CLASS(NGfx::CTexture)
BASIC_REGISTER_CLASS(NGfx::CCubeTexture)

namespace bk2::android {

bool CopyLegacyTextureArgb(
        NGfx::CTexture* texture,
        std::vector<uint32_t>* pixels,
        int* width,
        int* height) {
    if (pixels == nullptr || width == nullptr || height == nullptr) {
        return false;
    }
    pixels->clear();
    *width = 0;
    *height = 0;
    if (!IsValid(texture)) {
        return false;
    }

    CArray2D<NGfx::SPixel8888> readback;
    NGfx::GetRenderTargetData(&readback, texture);
    if (readback.GetSizeX() <= 0 || readback.GetSizeY() <= 0) {
        return false;
    }

    *width = readback.GetSizeX();
    *height = readback.GetSizeY();
    pixels->resize(static_cast<size_t>(*width) * *height);
    for (int y = 0; y < *height; ++y) {
        for (int x = 0; x < *width; ++x) {
            (*pixels)[static_cast<size_t>(y * *width + x)] =
                    readback[y][x].dwColor;
        }
    }
    return true;
}

std::string RunLegacyTextureProbe() {
    std::ostringstream report;

    CObj<NGfx::CTexture> texture = NGfx::EnsureProbeTexture();
    if (!IsValid(texture)) {
        return "legacy_texture=failed; legacy_texture_error=allocation_failed";
    }

    const int stride = texture->GetStride(0);

    NGfx::STexturePlaceInfo place;
    NGfx::CTexture* container = NGfx::GetTextureContainer(texture, &place);

    CArray2D<NGfx::SPixel8888> readback;
    NGfx::GetRenderTargetData(&readback, texture);
    uint64_t checksum = 0;
    for (int y = 0; y < readback.GetSizeY(); ++y) {
        for (int x = 0; x < readback.GetSizeX(); ++x) {
            checksum += readback[y][x].dwColor;
        }
    }

    CObj<NGScene::CColorTexture> color_node =
            new NGScene::CColorTexture(CVec4(0.25f, 0.5f, 0.75f, 1.0f));
    CDGPtr<NGScene::CColorTexture> color_ref(color_node.GetPtr());
    const bool color_changed = color_ref.Refresh();
    NGfx::CTexture* color_texture = color_ref->GetValue();
    CArray2D<NGfx::SPixel8888> color_readback;
    NGfx::GetRenderTargetData(&color_readback, color_texture);
    uint64_t color_checksum = 0;
    for (int y = 0; y < color_readback.GetSizeY(); ++y) {
        for (int x = 0; x < color_readback.GetSizeX(); ++x) {
            color_checksum += color_readback[y][x].dwColor;
        }
    }

    CObj<NGScene::CFileTexture> checker_node = new NGScene::CFileTexture();
    checker_node->CreateChecker();
    CDGPtr<NGScene::CFileTexture> checker_ref(checker_node.GetPtr());
    checker_ref.Refresh();
    NGfx::CTexture* checker_texture = checker_ref->GetValue();
    CArray2D<NGfx::SPixel8888> checker_readback;
    NGfx::GetRenderTargetData(&checker_readback, checker_texture);
    uint64_t checker_checksum = 0;
    for (int y = 0; y < checker_readback.GetSizeY(); y += 16) {
        for (int x = 0; x < checker_readback.GetSizeX(); x += 16) {
            checker_checksum += checker_readback[y][x].dwColor;
        }
    }

    bool dds_write_ok = false;
    std::string dds_error;
    CObj<NGfx::CTexture> dds_texture;
    CArray2D<NGfx::SPixel8888> dds_readback;
    uint64_t dds_checksum = 0;
    bool dxt1_write_ok = false;
    std::string dxt1_error;
    CObj<NGfx::CTexture> dxt1_texture;
    CArray2D<NGfx::SPixel8888> dxt1_readback;
    uint64_t dxt1_checksum = 0;
    bool dxt3_write_ok = false;
    std::string dxt3_error;
    CObj<NGfx::CTexture> dxt3_texture;
    CArray2D<NGfx::SPixel8888> dxt3_readback;
    uint64_t dxt3_checksum = 0;
    bool dxt5_write_ok = false;
    std::string dxt5_error;
    CObj<NGfx::CTexture> dxt5_texture;
    CArray2D<NGfx::SPixel8888> dxt5_readback;
    uint64_t dxt5_checksum = 0;
    const bk2::android::PortPaths paths = bk2::android::GetPortPaths();
    const std::string data_root = paths.data_root();
    if (!data_root.empty()) {
        if (mkdir(data_root.c_str(), 0775) != 0 && errno != EEXIST) {
            dds_error = std::string("mkdir_failed_errno_") + std::to_string(errno);
            dxt1_error = dds_error;
            dxt3_error = dds_error;
            dxt5_error = dds_error;
        } else {
            const std::string dds_name = "AndroidTextureProbe.dds";
            dds_write_ok = NGfx::WriteProbeDds(data_root + "/" + dds_name, &dds_error);
            if (dds_write_ok) {
                CPtr<NDb::STexture> file_desc = new NDb::STexture();
                NDb::CResourceHelper::SetDBID(
                        file_desc.GetPtr(),
                        CDBID("Android/TextureProbe.xdb"));
                NDb::CResourceHelper::SetLoaded(file_desc.GetPtr());
                file_desc->szDestName = dds_name.c_str();
                file_desc->eType = NDb::STexture::TEXTURE_2D;
                file_desc->eAddrType = NDb::STexture::CLAMP;
                file_desc->eFormat = NDb::STexture::TF_8888;
                file_desc->nWidth = 4;
                file_desc->nHeight = 4;
                file_desc->nNMips = 1;
                file_desc->nAverageColor = 0xff804040;
                file_desc->bInstantLoad = true;
                file_desc->bIsDXT = false;

                CObj<NGScene::CFileTexture> dds_node = new NGScene::CFileTexture();
                GUID uid;
                Zero(uid);
                dds_node->SetKey(NGScene::GetKey(file_desc.GetPtr()), uid);
                CDGPtr<NGScene::CFileTexture> dds_ref(dds_node.GetPtr());
                dds_ref.Refresh();
                dds_texture = dds_ref->GetValue();
                NGfx::GetRenderTargetData(&dds_readback, dds_texture);
                for (int y = 0; y < dds_readback.GetSizeY(); ++y) {
                    for (int x = 0; x < dds_readback.GetSizeX(); ++x) {
                        dds_checksum += dds_readback[y][x].dwColor;
                    }
                }
            }

            const std::string dxt1_name = "AndroidTextureProbeDXT1.dds";
            dxt1_write_ok =
                    NGfx::WriteProbeDxtDds(
                            data_root + "/" + dxt1_name,
                            NGfx::SPixelDXT1::ID,
                            &dxt1_error);
            if (dxt1_write_ok) {
                CPtr<NDb::STexture> file_desc = new NDb::STexture();
                NDb::CResourceHelper::SetDBID(
                        file_desc.GetPtr(),
                        CDBID("Android/TextureProbeDXT1.xdb"));
                NDb::CResourceHelper::SetLoaded(file_desc.GetPtr());
                file_desc->szDestName = dxt1_name.c_str();
                file_desc->eType = NDb::STexture::TEXTURE_2D;
                file_desc->eAddrType = NDb::STexture::CLAMP;
                file_desc->eFormat = NDb::STexture::TF_DXT1;
                file_desc->nWidth = 8;
                file_desc->nHeight = 8;
                file_desc->nNMips = 1;
                file_desc->nAverageColor = 0xffff0000;
                file_desc->bInstantLoad = true;
                file_desc->bIsDXT = true;

                CObj<NGScene::CFileTexture> dxt1_node = new NGScene::CFileTexture();
                GUID uid;
                Zero(uid);
                dxt1_node->SetKey(NGScene::GetKey(file_desc.GetPtr()), uid);
                CDGPtr<NGScene::CFileTexture> dxt1_ref(dxt1_node.GetPtr());
                dxt1_ref.Refresh();
                dxt1_texture = dxt1_ref->GetValue();
                NGfx::GetRenderTargetData(&dxt1_readback, dxt1_texture);
                for (int y = 0; y < dxt1_readback.GetSizeY(); ++y) {
                    for (int x = 0; x < dxt1_readback.GetSizeX(); ++x) {
                        dxt1_checksum += dxt1_readback[y][x].dwColor;
                    }
                }
            }

            const std::string dxt3_name = "AndroidTextureProbeDXT3.dds";
            dxt3_write_ok =
                    NGfx::WriteProbeDxtDds(
                            data_root + "/" + dxt3_name,
                            NGfx::SPixelDXT3::ID,
                            &dxt3_error);
            if (dxt3_write_ok) {
                CPtr<NDb::STexture> file_desc = new NDb::STexture();
                NDb::CResourceHelper::SetDBID(
                        file_desc.GetPtr(),
                        CDBID("Android/TextureProbeDXT3.xdb"));
                NDb::CResourceHelper::SetLoaded(file_desc.GetPtr());
                file_desc->szDestName = dxt3_name.c_str();
                file_desc->eType = NDb::STexture::TEXTURE_2D;
                file_desc->eAddrType = NDb::STexture::CLAMP;
                file_desc->eFormat = NDb::STexture::TF_DXT3;
                file_desc->nWidth = 8;
                file_desc->nHeight = 8;
                file_desc->nNMips = 1;
                file_desc->nAverageColor = 0x80ff0000;
                file_desc->bInstantLoad = true;
                file_desc->bIsDXT = true;

                CObj<NGScene::CFileTexture> dxt3_node = new NGScene::CFileTexture();
                GUID uid;
                Zero(uid);
                dxt3_node->SetKey(NGScene::GetKey(file_desc.GetPtr()), uid);
                CDGPtr<NGScene::CFileTexture> dxt3_ref(dxt3_node.GetPtr());
                dxt3_ref.Refresh();
                dxt3_texture = dxt3_ref->GetValue();
                NGfx::GetRenderTargetData(&dxt3_readback, dxt3_texture);
                for (int y = 0; y < dxt3_readback.GetSizeY(); ++y) {
                    for (int x = 0; x < dxt3_readback.GetSizeX(); ++x) {
                        dxt3_checksum += dxt3_readback[y][x].dwColor;
                    }
                }
            }

            const std::string dxt5_name = "AndroidTextureProbeDXT5.dds";
            dxt5_write_ok =
                    NGfx::WriteProbeDxtDds(
                            data_root + "/" + dxt5_name,
                            NGfx::SPixelDXT5::ID,
                            &dxt5_error);
            if (dxt5_write_ok) {
                CPtr<NDb::STexture> file_desc = new NDb::STexture();
                NDb::CResourceHelper::SetDBID(
                        file_desc.GetPtr(),
                        CDBID("Android/TextureProbeDXT5.xdb"));
                NDb::CResourceHelper::SetLoaded(file_desc.GetPtr());
                file_desc->szDestName = dxt5_name.c_str();
                file_desc->eType = NDb::STexture::TEXTURE_2D;
                file_desc->eAddrType = NDb::STexture::CLAMP;
                file_desc->eFormat = NDb::STexture::TF_DXT3;
                file_desc->nWidth = 8;
                file_desc->nHeight = 8;
                file_desc->nNMips = 1;
                file_desc->nAverageColor = 0x80ff0000;
                file_desc->bInstantLoad = true;
                file_desc->bIsDXT = true;

                CObj<NGScene::CFileTexture> dxt5_node = new NGScene::CFileTexture();
                GUID uid;
                Zero(uid);
                dxt5_node->SetKey(NGScene::GetKey(file_desc.GetPtr()), uid);
                CDGPtr<NGScene::CFileTexture> dxt5_ref(dxt5_node.GetPtr());
                dxt5_ref.Refresh();
                dxt5_texture = dxt5_ref->GetValue();
                NGfx::GetRenderTargetData(&dxt5_readback, dxt5_texture);
                for (int y = 0; y < dxt5_readback.GetSizeY(); ++y) {
                    for (int x = 0; x < dxt5_readback.GetSizeX(); ++x) {
                        dxt5_checksum += dxt5_readback[y][x].dwColor;
                    }
                }
            }
        }
    } else {
        dds_error = "data_root_empty";
        dxt1_error = "data_root_empty";
        dxt3_error = "data_root_empty";
        dxt5_error = "data_root_empty";
    }

    report << "legacy_texture=probed"
           << "; size=" << texture->GetSizeX() << "x" << texture->GetSizeY()
           << "; mips=" << texture->GetNumMipLevels()
           << "; pixel=" << texture->GetPixelID()
           << "; stride=" << stride
           << "; storage=" << texture->CpuStorageBytes()
           << "; container_whole="
           << (container == texture.GetPtr() && place.IsWhole() ? "yes" : "no")
           << "; readback=" << readback.GetSizeX() << "x" << readback.GetSizeY()
           << "; checksum=" << checksum
           << "; gpu_handle=" << (texture->HasGpuHandle() ? "yes" : "no")
           << "; uploaded_levels=" << texture->UploadedLevelCount()
           << "; gtexture_color="
           << (IsValid(color_texture) ? "probed" : "failed")
           << "; gtexture_color_changed=" << (color_changed ? "true" : "false")
           << "; gtexture_color_size=" << color_readback.GetSizeX()
           << "x" << color_readback.GetSizeY()
           << "; gtexture_color_checksum=" << color_checksum
           << "; gtexture_checker="
           << (IsValid(checker_texture) ? "probed" : "failed")
           << "; gtexture_checker_size=" << checker_readback.GetSizeX()
           << "x" << checker_readback.GetSizeY()
           << "; gtexture_checker_sample_checksum=" << checker_checksum
           << "; gtexture_dds="
           << (IsValid(dds_texture) ? "probed" : "failed")
           << "; gtexture_dds_write=" << (dds_write_ok ? "true" : "false")
           << "; gtexture_dds_size=" << dds_readback.GetSizeX()
           << "x" << dds_readback.GetSizeY()
           << "; gtexture_dds_checksum=" << dds_checksum
           << "; gtexture_dxt1="
           << (IsValid(dxt1_texture) ? "probed" : "failed")
           << "; gtexture_dxt1_write=" << (dxt1_write_ok ? "true" : "false")
           << "; gtexture_dxt1_size=" << dxt1_readback.GetSizeX()
           << "x" << dxt1_readback.GetSizeY()
           << "; gtexture_dxt1_checksum=" << dxt1_checksum
           << "; gtexture_dxt3="
           << (IsValid(dxt3_texture) ? "probed" : "failed")
           << "; gtexture_dxt3_write=" << (dxt3_write_ok ? "true" : "false")
           << "; gtexture_dxt3_size=" << dxt3_readback.GetSizeX()
           << "x" << dxt3_readback.GetSizeY()
           << "; gtexture_dxt3_checksum=" << dxt3_checksum
           << "; gtexture_dxt5="
           << (IsValid(dxt5_texture) ? "probed" : "failed")
           << "; gtexture_dxt5_write=" << (dxt5_write_ok ? "true" : "false")
           << "; gtexture_dxt5_size=" << dxt5_readback.GetSizeX()
           << "x" << dxt5_readback.GetSizeY()
           << "; gtexture_dxt5_checksum=" << dxt5_checksum;
    if (!dds_error.empty()) {
        report << "; gtexture_dds_error=" << dds_error;
    }
    if (!dxt1_error.empty()) {
        report << "; gtexture_dxt1_error=" << dxt1_error;
    }
    if (!dxt3_error.empty()) {
        report << "; gtexture_dxt3_error=" << dxt3_error;
    }
    if (!dxt5_error.empty()) {
        report << "; gtexture_dxt5_error=" << dxt5_error;
    }
    return report.str();
}

void QueueLegacyTextureSmokeOverlay(float x, float y, float width, float height) {
    NGfx::CTexture* texture = LegacyTextureSmokeTexture();
    if (!IsValid(texture)) {
        return;
    }
    if (!texture->HasGpuHandle()) {
        texture->UploadLevel(0);
    }
    const uint16_t handle = texture->NativeTextureHandleIndex();
    if (handle == UINT16_MAX) {
        return;
    }
    RenderBackend().queue_textured_rect(
            x,
            y,
            width,
            height,
            handle,
            0.0f,
            0.0f,
            1.0f,
            1.0f,
            0xffffffffu);
}

NGfx::CTexture* LegacyTextureSmokeTexture() {
    return NGfx::EnsureProbeTexture();
}

void EnsureLegacyTextureUploaded(NGfx::CTexture* texture, int level) {
    if (IsValid(texture) && !texture->HasGpuHandle()) {
        texture->UploadLevel(level);
    }
}

void EnsureLegacyTextureMipChainUploaded(NGfx::CTexture* texture) {
    if (IsValid(texture)) {
        texture->UploadMipChain();
    }
}

void ConfigureLegacyTerrainTexture(NGfx::CTexture* texture) {
    if (IsValid(texture)) {
        texture->ConfigureTerrainSampling();
    }
}

void ConfigureLegacyLuminanceAlphaTexture(NGfx::CTexture* texture) {
    if (IsValid(texture)) {
        texture->ConfigureLuminanceAlphaSampling();
    }
}

uint16_t LegacyTextureHandleIndex(NGfx::CTexture* texture) {
    if (!IsValid(texture)) {
        return UINT16_MAX;
    }
    return texture->NativeTextureHandleIndex();
}

void ReleaseLegacyTextureGpuResources() {
    std::vector<NGfx::CTexture*> live_textures = NGfx::g_live_textures;
    for (size_t i = 0; i < live_textures.size(); ++i) {
        if (IsValid(live_textures[i])) {
            live_textures[i]->ReleaseGpuTexture();
        }
    }
}

}  // namespace bk2::android
