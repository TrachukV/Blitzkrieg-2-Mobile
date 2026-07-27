#pragma once

#include "bk2_android_audio_backend.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bk2::android {

struct DecodedPcmClip {
    std::vector<int16_t> samples;
    int channel_count = 0;
    int sample_rate = 0;
    uint16_t source_format_tag = 0;

    size_t frame_count() const;
    AudioClipView view() const;
};

bool DecodeWavToPcm16(const uint8_t* data,
                      size_t size,
                      DecodedPcmClip* decoded,
                      std::string* error);

}  // namespace bk2::android
