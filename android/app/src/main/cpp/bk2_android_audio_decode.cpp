#include "bk2_android_audio_decode.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace bk2::android {
namespace {

constexpr uint16_t kWaveFormatPcm = 0x0001;
constexpr uint16_t kWaveFormatMsAdpcm = 0x0002;
constexpr uint16_t kWaveFormatIeeeFloat = 0x0003;
constexpr uint16_t kWaveFormatImaAdpcm = 0x0011;

// IMA/DVI ADPCM step and index tables, as used by the shipped menu and UI
// sounds.
constexpr std::array<int, 89> kImaStepTable = {
        7,     8,     9,     10,    11,    12,    13,    14,    16,    17,
        19,    21,    23,    25,    28,    31,    34,    37,    41,    45,
        50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
        130,   143,   157,   173,   190,   209,   230,   253,   279,   307,
        337,   371,   408,   449,   494,   544,   598,   658,   724,   796,
        876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
        2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,
        5894,  6484,  7132,  7845,  8630,  9493,  10442, 11487, 12635, 13899,
        15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

constexpr std::array<int, 16> kImaIndexTable = {
        -1, -1, -1, -1, 2, 4, 6, 8,
        -1, -1, -1, -1, 2, 4, 6, 8};

constexpr std::array<int, 16> kMsAdpcmAdaptation = {
        230, 230, 230, 230, 307, 409, 512, 614,
        768, 614, 512, 409, 307, 230, 230, 230};

constexpr std::array<std::array<int16_t, 2>, 7> kDefaultMsAdpcmCoefficients = {{
        {{256, 0}},
        {{512, -256}},
        {{0, 0}},
        {{192, 64}},
        {{240, 0}},
        {{460, -208}},
        {{392, -232}},
}};

struct WavFormat {
    uint16_t format_tag = 0;
    uint16_t channels = 0;
    uint32_t sample_rate = 0;
    uint32_t byte_rate = 0;
    uint16_t block_align = 0;
    uint16_t bits_per_sample = 0;
    uint16_t samples_per_block = 0;
    std::vector<std::array<int16_t, 2>> coefficients;
};

uint16_t ReadU16(const uint8_t* data) {
    return static_cast<uint16_t>(data[0]) |
           static_cast<uint16_t>(data[1] << 8);
}

int16_t ReadS16(const uint8_t* data) {
    return static_cast<int16_t>(ReadU16(data));
}

uint32_t ReadU32(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) |
           (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) |
           (static_cast<uint32_t>(data[3]) << 24);
}

bool HasTag(const uint8_t* data, const char tag[4]) {
    return std::memcmp(data, tag, 4) == 0;
}

void SetError(std::string* error, const char* text) {
    if (error != nullptr) {
        *error = text;
    }
}

int16_t ClampToPcm16(int32_t sample) {
    if (sample > std::numeric_limits<int16_t>::max()) {
        return std::numeric_limits<int16_t>::max();
    }
    if (sample < std::numeric_limits<int16_t>::min()) {
        return std::numeric_limits<int16_t>::min();
    }
    return static_cast<int16_t>(sample);
}

int16_t ConvertPcmSample(const uint8_t* data, uint16_t bits_per_sample) {
    switch (bits_per_sample) {
        case 8:
            return static_cast<int16_t>((static_cast<int>(data[0]) - 128) << 8);
        case 16:
            return ReadS16(data);
        case 24: {
            int32_t value = static_cast<int32_t>(data[0]) |
                    (static_cast<int32_t>(data[1]) << 8) |
                    (static_cast<int32_t>(data[2]) << 16);
            if ((value & 0x00800000) != 0) {
                value |= static_cast<int32_t>(0xff000000);
            }
            return static_cast<int16_t>(value >> 8);
        }
        case 32: {
            const int32_t value = static_cast<int32_t>(ReadU32(data));
            return static_cast<int16_t>(value >> 16);
        }
        default:
            return 0;
    }
}

int16_t ConvertFloatSample(const uint8_t* data) {
    float value = 0.0f;
    std::memcpy(&value, data, sizeof(value));
    value = std::clamp(value, -1.0f, 1.0f);
    return static_cast<int16_t>(std::lround(value * 32767.0f));
}

uint8_t ReadNibble(const uint8_t* data, size_t data_size, size_t* nibble_index) {
    const size_t byte_index = *nibble_index / 2;
    if (byte_index >= data_size) {
        return 0;
    }
    const uint8_t value = data[byte_index];
    const bool high = (*nibble_index % 2) == 0;
    ++(*nibble_index);
    return high ? static_cast<uint8_t>(value >> 4) : static_cast<uint8_t>(value & 0x0f);
}

uint16_t DerivedMsAdpcmSamplesPerBlock(const WavFormat& fmt) {
    if (fmt.channels == 0 || fmt.block_align <= fmt.channels * 7) {
        return 0;
    }
    return static_cast<uint16_t>(((fmt.block_align - (fmt.channels * 7)) * 2) /
                                 fmt.channels + 2);
}

bool DecodePcmData(const WavFormat& fmt,
                   const std::vector<uint8_t>& data,
                   DecodedPcmClip* decoded,
                   std::string* error) {
    if (fmt.channels == 0 || fmt.sample_rate == 0 || fmt.bits_per_sample == 0) {
        SetError(error, "WAV fmt chunk has invalid PCM parameters");
        return false;
    }
    if (fmt.format_tag == kWaveFormatPcm &&
        fmt.bits_per_sample != 8 &&
        fmt.bits_per_sample != 16 &&
        fmt.bits_per_sample != 24 &&
        fmt.bits_per_sample != 32) {
        SetError(error, "unsupported PCM bit depth");
        return false;
    }
    if (fmt.format_tag == kWaveFormatIeeeFloat && fmt.bits_per_sample != 32) {
        SetError(error, "unsupported float WAV bit depth");
        return false;
    }

    const size_t bytes_per_sample = fmt.bits_per_sample / 8;
    size_t sample_count = data.size() / bytes_per_sample;
    sample_count -= sample_count % fmt.channels;
    decoded->samples.clear();
    decoded->samples.reserve(sample_count);
    for (size_t i = 0; i < sample_count; ++i) {
        const uint8_t* sample_data = data.data() + (i * bytes_per_sample);
        decoded->samples.push_back(fmt.format_tag == kWaveFormatIeeeFloat
                ? ConvertFloatSample(sample_data)
                : ConvertPcmSample(sample_data, fmt.bits_per_sample));
    }
    return true;
}

bool DecodeMsAdpcmBlock(const WavFormat& fmt,
                        const uint8_t* block,
                        size_t block_size,
                        DecodedPcmClip* decoded) {
    const size_t channels = fmt.channels;
    const size_t header_size = channels * 7;
    if (channels == 0 || channels > 2 || block_size < header_size) {
        return false;
    }

    const uint16_t samples_per_block = fmt.samples_per_block != 0
            ? fmt.samples_per_block
            : DerivedMsAdpcmSamplesPerBlock(fmt);
    if (samples_per_block < 2) {
        return false;
    }

    std::array<uint8_t, 2> predictor = {0, 0};
    std::array<int32_t, 2> delta = {0, 0};
    std::array<int32_t, 2> sample1 = {0, 0};
    std::array<int32_t, 2> sample2 = {0, 0};

    size_t offset = 0;
    for (size_t ch = 0; ch < channels; ++ch) {
        predictor[ch] = block[offset++];
    }
    for (size_t ch = 0; ch < channels; ++ch) {
        delta[ch] = std::max<int32_t>(ReadS16(block + offset), 16);
        offset += 2;
    }
    for (size_t ch = 0; ch < channels; ++ch) {
        sample1[ch] = ReadS16(block + offset);
        offset += 2;
    }
    for (size_t ch = 0; ch < channels; ++ch) {
        sample2[ch] = ReadS16(block + offset);
        offset += 2;
    }

    for (size_t ch = 0; ch < channels; ++ch) {
        if (predictor[ch] >= fmt.coefficients.size()) {
            predictor[ch] = 0;
        }
    }

    decoded->samples.reserve(decoded->samples.size() +
                             static_cast<size_t>(samples_per_block) * channels);
    for (size_t ch = 0; ch < channels; ++ch) {
        decoded->samples.push_back(ClampToPcm16(sample2[ch]));
    }
    for (size_t ch = 0; ch < channels; ++ch) {
        decoded->samples.push_back(ClampToPcm16(sample1[ch]));
    }

    size_t nibble_index = 0;
    const uint8_t* nibble_data = block + offset;
    const size_t nibble_data_size = block_size - offset;
    const size_t decodable_frames = std::min<size_t>(
            samples_per_block,
            2 + ((nibble_data_size * 2) / channels));
    for (size_t frame = 2; frame < decodable_frames; ++frame) {
        for (size_t ch = 0; ch < channels; ++ch) {
            const uint8_t nibble = ReadNibble(nibble_data, nibble_data_size, &nibble_index);
            const int signed_nibble = (nibble & 0x08) != 0
                    ? static_cast<int>(nibble) - 16
                    : static_cast<int>(nibble);
            const auto& coeff = fmt.coefficients[predictor[ch]];
            const int32_t predicted =
                    ((sample1[ch] * coeff[0]) + (sample2[ch] * coeff[1])) / 256 +
                    signed_nibble * delta[ch];
            const int16_t pcm = ClampToPcm16(predicted);
            decoded->samples.push_back(pcm);
            sample2[ch] = sample1[ch];
            sample1[ch] = pcm;
            delta[ch] = std::max<int32_t>(
                    16,
                    (delta[ch] * kMsAdpcmAdaptation[nibble]) / 256);
        }
    }
    return true;
}

bool DecodeMsAdpcmData(const WavFormat& fmt,
                       const std::vector<uint8_t>& data,
                       DecodedPcmClip* decoded,
                       std::string* error) {
    if (fmt.channels == 0 || fmt.channels > 2 || fmt.sample_rate == 0 ||
        fmt.block_align == 0) {
        SetError(error, "WAV fmt chunk has invalid MS ADPCM parameters");
        return false;
    }

    decoded->samples.clear();
    size_t offset = 0;
    while (offset < data.size()) {
        const size_t block_size = std::min<size_t>(fmt.block_align, data.size() - offset);
        if (!DecodeMsAdpcmBlock(fmt, data.data() + offset, block_size, decoded)) {
            SetError(error, "failed to decode MS ADPCM block");
            return false;
        }
        offset += block_size;
    }
    return true;
}

bool ParseFmtChunk(const uint8_t* data, size_t size, WavFormat* fmt, std::string* error) {
    if (size < 16) {
        SetError(error, "WAV fmt chunk is too small");
        return false;
    }

    fmt->format_tag = ReadU16(data);
    fmt->channels = ReadU16(data + 2);
    fmt->sample_rate = ReadU32(data + 4);
    fmt->byte_rate = ReadU32(data + 8);
    fmt->block_align = ReadU16(data + 12);
    fmt->bits_per_sample = ReadU16(data + 14);
    fmt->coefficients.clear();

    if (fmt->format_tag == kWaveFormatImaAdpcm) {
        if (size >= 20) {
            fmt->samples_per_block = ReadU16(data + 18);
        }
    }

    if (fmt->format_tag == kWaveFormatMsAdpcm) {
        if (size < 22) {
            SetError(error, "MS ADPCM fmt chunk is too small");
            return false;
        }
        fmt->samples_per_block = ReadU16(data + 18);
        const uint16_t coefficient_count = ReadU16(data + 20);
        size_t offset = 22;
        for (uint16_t i = 0; i < coefficient_count && offset + 4 <= size; ++i) {
            fmt->coefficients.push_back({ReadS16(data + offset), ReadS16(data + offset + 2)});
            offset += 4;
        }
        if (fmt->coefficients.empty()) {
            fmt->coefficients.assign(kDefaultMsAdpcmCoefficients.begin(),
                                     kDefaultMsAdpcmCoefficients.end());
        }
    }

    return true;
}

// Each IMA ADPCM block starts with a four byte per-channel preamble holding
// the initial predictor and step index, followed by interleaved nibbles that
// are grouped per channel in four byte words.
int16_t DecodeImaNibble(uint8_t nibble, int* predictor, int* step_index) {
    const int step = kImaStepTable[static_cast<size_t>(*step_index)];
    // Single-truncation form: the magnitude bits scale the step once, which
    // is what reference IMA decoders produce.
    int difference = ((2 * (nibble & 7) + 1) * step) >> 3;
    if (nibble & 8) {
        *predictor -= difference;
    } else {
        *predictor += difference;
    }
    *predictor = std::clamp(*predictor, -32768, 32767);
    *step_index = std::clamp(
            *step_index + kImaIndexTable[nibble], 0, 88);
    return static_cast<int16_t>(*predictor);
}

bool DecodeImaAdpcmData(const WavFormat& fmt,
                        const std::vector<uint8_t>& audio_data,
                        DecodedPcmClip* decoded,
                        std::string* error) {
    const size_t channels = fmt.channels;
    if (channels == 0 || channels > 2) {
        SetError(error, "unsupported IMA ADPCM channel count");
        return false;
    }
    const size_t block_align = fmt.block_align;
    const size_t preamble = 4 * channels;
    if (block_align <= preamble) {
        SetError(error, "IMA ADPCM block align is too small");
        return false;
    }
    const size_t samples_per_block = fmt.samples_per_block > 0
            ? static_cast<size_t>(fmt.samples_per_block)
            : 1 + (block_align - preamble) * 2 / channels;

    std::vector<std::vector<int16_t>> channel_samples(channels);
    for (size_t block = 0; block + block_align <= audio_data.size();
         block += block_align) {
        const uint8_t* data = audio_data.data() + block;
        for (std::vector<int16_t>& samples : channel_samples) {
            samples.clear();
            samples.reserve(samples_per_block);
        }

        std::vector<int> predictor(channels, 0);
        std::vector<int> step_index(channels, 0);
        for (size_t channel = 0; channel < channels; ++channel) {
            predictor[channel] = ReadS16(data + channel * 4);
            step_index[channel] = std::clamp(
                    static_cast<int>(data[channel * 4 + 2]), 0, 88);
            // The preamble already carries the block's first sample.
            channel_samples[channel].push_back(
                    static_cast<int16_t>(predictor[channel]));
        }

        // Nibbles are grouped into four byte words, one word per channel.
        size_t offset = preamble;
        while (offset + 4 * channels <= block_align) {
            for (size_t channel = 0; channel < channels; ++channel) {
                const uint8_t* word = data + offset + channel * 4;
                for (size_t byte = 0; byte < 4; ++byte) {
                    channel_samples[channel].push_back(DecodeImaNibble(
                            word[byte] & 0x0fu,
                            &predictor[channel],
                            &step_index[channel]));
                    channel_samples[channel].push_back(DecodeImaNibble(
                            (word[byte] >> 4) & 0x0fu,
                            &predictor[channel],
                            &step_index[channel]));
                }
            }
            offset += 4 * channels;
        }

        size_t frames = samples_per_block;
        for (const std::vector<int16_t>& samples : channel_samples) {
            frames = std::min(frames, samples.size());
        }
        for (size_t frame = 0; frame < frames; ++frame) {
            for (size_t channel = 0; channel < channels; ++channel) {
                decoded->samples.push_back(channel_samples[channel][frame]);
            }
        }
    }
    return true;
}

}  // namespace

size_t DecodedPcmClip::frame_count() const {
    if (channel_count <= 0) {
        return 0;
    }
    return samples.size() / static_cast<size_t>(channel_count);
}

AudioClipView DecodedPcmClip::view() const {
    AudioClipView clip;
    clip.samples = samples.empty() ? nullptr : samples.data();
    clip.frame_count = frame_count();
    clip.channel_count = channel_count;
    clip.sample_rate = sample_rate;
    return clip;
}

bool DecodeWavToPcm16(const uint8_t* data,
                      size_t size,
                      DecodedPcmClip* decoded,
                      std::string* error) {
    if (decoded == nullptr) {
        SetError(error, "decoded output is null");
        return false;
    }
    decoded->samples.clear();
    decoded->channel_count = 0;
    decoded->sample_rate = 0;
    decoded->source_format_tag = 0;

    if (data == nullptr || size < 12) {
        SetError(error, "WAV data is empty or too small");
        return false;
    }
    if (!HasTag(data, "RIFF") || !HasTag(data + 8, "WAVE")) {
        SetError(error, "file is not a RIFF/WAVE stream");
        return false;
    }

    WavFormat fmt;
    std::vector<uint8_t> audio_data;
    bool have_fmt = false;
    bool have_data = false;

    size_t offset = 12;
    while (offset + 8 <= size) {
        const uint8_t* chunk = data + offset;
        const uint32_t chunk_size = ReadU32(chunk + 4);
        const size_t chunk_data_offset = offset + 8;
        if (chunk_data_offset > size || chunk_size > size - chunk_data_offset) {
            SetError(error, "WAV chunk size exceeds file size");
            return false;
        }

        if (HasTag(chunk, "fmt ")) {
            if (!ParseFmtChunk(data + chunk_data_offset, chunk_size, &fmt, error)) {
                return false;
            }
            have_fmt = true;
        } else if (HasTag(chunk, "data")) {
            audio_data.insert(audio_data.end(),
                              data + chunk_data_offset,
                              data + chunk_data_offset + chunk_size);
            have_data = true;
        }

        offset = chunk_data_offset + chunk_size + (chunk_size & 1u);
    }

    if (!have_fmt || !have_data) {
        SetError(error, "WAV stream is missing fmt or data chunk");
        return false;
    }

    decoded->channel_count = fmt.channels;
    decoded->sample_rate = static_cast<int>(fmt.sample_rate);
    decoded->source_format_tag = fmt.format_tag;

    bool ok = false;
    if (fmt.format_tag == kWaveFormatPcm || fmt.format_tag == kWaveFormatIeeeFloat) {
        ok = DecodePcmData(fmt, audio_data, decoded, error);
    } else if (fmt.format_tag == kWaveFormatMsAdpcm) {
        ok = DecodeMsAdpcmData(fmt, audio_data, decoded, error);
    } else if (fmt.format_tag == kWaveFormatImaAdpcm) {
        ok = DecodeImaAdpcmData(fmt, audio_data, decoded, error);
    } else {
        SetError(error, "unsupported WAV format tag");
        return false;
    }

    if (!ok) {
        return false;
    }
    if (decoded->samples.empty() || decoded->frame_count() == 0) {
        SetError(error, "decoded WAV produced no PCM frames");
        return false;
    }
    return true;
}

}  // namespace bk2::android
