#include "bk2_android_audio_backend.h"
#include "bk2_android_audio_decode.h"
#include "bk2_android_platform.h"
#if defined(BK2_LEGACY_SOUND_RUNTIME_ENABLED)
#include "bk2_legacy_sound_probe.h"
#endif

#include <jni.h>

#include <array>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace bk2::android {
namespace {

size_t CountNonZeroSamples(const std::vector<int16_t>& samples) {
    size_t count = 0;
    for (int16_t sample : samples) {
        if (sample != 0) {
            ++count;
        }
    }
    return count;
}

bool AllSamplesZero(const std::vector<int16_t>& samples) {
    for (int16_t sample : samples) {
        if (sample != 0) {
            return false;
        }
    }
    return true;
}

std::vector<int16_t> MakeMonoPulse(size_t frame_count) {
    std::vector<int16_t> samples(frame_count);
    for (size_t i = 0; i < frame_count; ++i) {
        samples[i] = (i % 16) < 8 ? 12000 : -12000;
    }
    return samples;
}

std::vector<int16_t> MakeStereoPulse(size_t frame_count) {
    std::vector<int16_t> samples(frame_count * 2);
    for (size_t i = 0; i < frame_count; ++i) {
        samples[(i * 2)] = (i % 12) < 6 ? 6000 : -6000;
        samples[(i * 2) + 1] = (i % 20) < 10 ? 9000 : -9000;
    }
    return samples;
}

void AppendTag(std::vector<uint8_t>* bytes, const char tag[5]) {
    bytes->insert(bytes->end(), tag, tag + 4);
}

void AppendU16(std::vector<uint8_t>* bytes, uint16_t value) {
    bytes->push_back(static_cast<uint8_t>(value & 0xff));
    bytes->push_back(static_cast<uint8_t>((value >> 8) & 0xff));
}

void AppendS16(std::vector<uint8_t>* bytes, int16_t value) {
    AppendU16(bytes, static_cast<uint16_t>(value));
}

void AppendU32(std::vector<uint8_t>* bytes, uint32_t value) {
    bytes->push_back(static_cast<uint8_t>(value & 0xff));
    bytes->push_back(static_cast<uint8_t>((value >> 8) & 0xff));
    bytes->push_back(static_cast<uint8_t>((value >> 16) & 0xff));
    bytes->push_back(static_cast<uint8_t>((value >> 24) & 0xff));
}

std::vector<uint8_t> MakeMsAdpcmWav() {
    constexpr uint16_t kChannels = 1;
    constexpr uint32_t kSampleRate = 44100;
    constexpr uint16_t kBlockAlign = 11;
    constexpr uint16_t kSamplesPerBlock = 10;
    constexpr uint32_t kFmtSize = 50;
    constexpr uint32_t kDataSize = kBlockAlign;
    constexpr uint32_t kRiffSize = 4 + (8 + kFmtSize) + (8 + kDataSize + 1);
    constexpr std::array<std::array<int16_t, 2>, 7> kCoefficients = {{
            {{256, 0}},
            {{512, -256}},
            {{0, 0}},
            {{192, 64}},
            {{240, 0}},
            {{460, -208}},
            {{392, -232}},
    }};

    std::vector<uint8_t> bytes;
    bytes.reserve(kRiffSize + 8);
    AppendTag(&bytes, "RIFF");
    AppendU32(&bytes, kRiffSize);
    AppendTag(&bytes, "WAVE");
    AppendTag(&bytes, "fmt ");
    AppendU32(&bytes, kFmtSize);
    AppendU16(&bytes, 0x0002);
    AppendU16(&bytes, kChannels);
    AppendU32(&bytes, kSampleRate);
    AppendU32(&bytes, (kSampleRate * kBlockAlign) / kSamplesPerBlock);
    AppendU16(&bytes, kBlockAlign);
    AppendU16(&bytes, 4);
    AppendU16(&bytes, 32);
    AppendU16(&bytes, kSamplesPerBlock);
    AppendU16(&bytes, static_cast<uint16_t>(kCoefficients.size()));
    for (const auto& coefficient : kCoefficients) {
        AppendS16(&bytes, coefficient[0]);
        AppendS16(&bytes, coefficient[1]);
    }
    AppendTag(&bytes, "data");
    AppendU32(&bytes, kDataSize);
    bytes.push_back(0);
    AppendS16(&bytes, 16);
    AppendS16(&bytes, 1000);
    AppendS16(&bytes, 0);
    bytes.insert(bytes.end(), 4, 0);
    bytes.push_back(0);
    return bytes;
}

}  // namespace

std::string RunAudioBackendProbe() {
    auto& backend = AudioBackend();
    const bool restore_initialized = backend.is_initialized();
    const int restore_mix_rate = restore_initialized ? backend.mix_rate() : 44100;
    const int restore_max_channels = restore_initialized ? backend.max_channels() : 96;
    backend.shutdown();
    const bool initialized = backend.init(48000, 4);

    const std::vector<uint8_t> adpcm_wav = MakeMsAdpcmWav();
    DecodedPcmClip decoded_adpcm;
    std::string decode_error;
    const bool adpcm_decoded = DecodeWavToPcm16(
            adpcm_wav.data(), adpcm_wav.size(), &decoded_adpcm, &decode_error);
    const size_t decoded_adpcm_non_zero = CountNonZeroSamples(decoded_adpcm.samples);

    std::vector<int16_t> mono = MakeMonoPulse(64);
    std::vector<int16_t> stereo = MakeStereoPulse(64);
    const AudioClipView mono_clip = adpcm_decoded ? decoded_adpcm.view() : AudioClipView{
            mono.data(), mono.size(), 1, 48000};

    AudioClipView stereo_clip;
    stereo_clip.samples = stereo.data();
    stereo_clip.frame_count = stereo.size() / 2;
    stereo_clip.channel_count = 2;
    stereo_clip.sample_rate = 24000;

    const int one_shot_channel = backend.play(mono_clip, false, 0);
    const int loop_channel = backend.play(stereo_clip, true, 0);
    backend.set_pan(one_shot_channel, -0.75f);
    backend.set_volume(one_shot_channel, 0.8f);
    backend.set_pan(loop_channel, 0.5f);
    backend.set_volume(loop_channel, 0.6f);

    std::vector<int16_t> mixed(160 * 2);
    const size_t mixed_frames = backend.mix_interleaved_stereo(mixed.data(), 160);
    const size_t non_zero_samples = CountNonZeroSamples(mixed);
    const bool one_shot_stopped = !backend.channel_state(one_shot_channel).has_value();
    const auto loop_state_after_mix = backend.channel_state(loop_channel);
    const uint32_t loop_position_after_mix =
            loop_state_after_mix.has_value() ? loop_state_after_mix->position_frames : 0;

    backend.set_paused(true);
    std::vector<int16_t> paused_buffer(32 * 2);
    const size_t paused_frames = backend.mix_interleaved_stereo(paused_buffer.data(), 32);
    const auto paused_state = backend.channel_state(loop_channel);
    const uint32_t paused_position =
            paused_state.has_value() ? paused_state->position_frames : 0;
    const bool pause_zeroed = AllSamplesZero(paused_buffer);
    const bool pause_held_position = paused_position == loop_position_after_mix;

    backend.set_paused(false);
    std::vector<int16_t> resumed_buffer(16 * 2);
    const size_t resumed_frames = backend.mix_interleaved_stereo(resumed_buffer.data(), 16);
    const auto resumed_state = backend.channel_state(loop_channel);
    const uint32_t resumed_position =
            resumed_state.has_value() ? resumed_state->position_frames : 0;
    const bool resume_advanced = resumed_state.has_value() &&
            resumed_position != loop_position_after_mix &&
            CountNonZeroSamples(resumed_buffer) > 0;

    backend.stop(loop_channel);
    const size_t active_after_stop = backend.active_channel_count();

#if defined(BK2_LEGACY_SOUND_RUNTIME_ENABLED)
    backend.shutdown();
    const std::string legacy_report = RunLegacySoundProbe();
#endif

    std::ostringstream report;
    report << "audio_backend=" << (initialized ? "probed" : "failed")
           << "; mix_rate=" << backend.mix_rate()
           << "; max_channels=" << backend.max_channels()
           << "; adpcm_decoded=" << (adpcm_decoded ? "true" : "false")
           << "; adpcm_format=" << decoded_adpcm.source_format_tag
           << "; adpcm_frames=" << decoded_adpcm.frame_count()
           << "; adpcm_nonzero_samples=" << decoded_adpcm_non_zero
           << "; mixed_frames=" << mixed_frames
           << "; nonzero_samples=" << non_zero_samples
           << "; oneshot_stopped=" << (one_shot_stopped ? "true" : "false")
           << "; loop_position_after_mix=" << loop_position_after_mix
           << "; paused_frames=" << paused_frames
           << "; pause_zeroed=" << (pause_zeroed ? "true" : "false")
           << "; pause_held_position=" << (pause_held_position ? "true" : "false")
           << "; resumed_frames=" << resumed_frames
           << "; resume_advanced=" << (resume_advanced ? "true" : "false")
           << "; active_channels_after_stop=" << active_after_stop;
#if defined(BK2_LEGACY_SOUND_RUNTIME_ENABLED)
    report << "; " << legacy_report;
#endif
    if (!decode_error.empty()) {
        report << "; adpcm_error=" << decode_error;
    }

    const std::string text = report.str();
    PlatformRuntime::instance().log_info(text);
    backend.shutdown();
    if (restore_initialized) {
        backend.init(restore_mix_rate, restore_max_channels);
    }
    return text;
}

}  // namespace bk2::android

extern "C" JNIEXPORT jstring JNICALL
Java_com_nival_blitzkrieg2_NativeBridge_runAudioBackendProbe(JNIEnv* env, jclass) {
    const std::string report = bk2::android::RunAudioBackendProbe();
    return env->NewStringUTF(report.c_str());
}
