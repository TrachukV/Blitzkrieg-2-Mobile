#include "bk2_android_audio_backend.h"

#include "bk2_android_platform.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace bk2::android {
namespace {

constexpr size_t kMixScratchFrames = 4096;

uint32_t ClampFramePosition(double frame_position) {
    if (frame_position <= 0.0) {
        return 0;
    }
    constexpr double kMaxPosition =
            static_cast<double>(std::numeric_limits<uint32_t>::max());
    return static_cast<uint32_t>(std::min(frame_position, kMaxPosition));
}

int16_t ClampToInt16(int32_t sample) {
    if (sample > std::numeric_limits<int16_t>::max()) {
        return std::numeric_limits<int16_t>::max();
    }
    if (sample < std::numeric_limits<int16_t>::min()) {
        return std::numeric_limits<int16_t>::min();
    }
    return static_cast<int16_t>(sample);
}

int32_t ScaledSample(int16_t sample, float gain) {
    const float scaled = static_cast<float>(sample) * gain;
    return static_cast<int32_t>(std::lround(scaled));
}

int16_t ClipSample(const AudioClipView& clip, size_t frame, int channel) {
    if (clip.samples == nullptr || frame >= clip.frame_count || clip.channel_count <= 0) {
        return 0;
    }
    const int source_channel = clip.channel_count == 1
            ? 0
            : std::min(channel, clip.channel_count - 1);
    return clip.samples[(frame * static_cast<size_t>(clip.channel_count)) +
                        static_cast<size_t>(source_channel)];
}

double WrappedPosition(double source_position, size_t frame_count) {
    if (frame_count == 0) {
        return 0.0;
    }
    const double length = static_cast<double>(frame_count);
    source_position = std::fmod(source_position, length);
    if (source_position < 0.0) {
        source_position += length;
    }
    return source_position;
}

float Dot3(const float left[3], const float right[3]) {
    return (left[0] * right[0]) + (left[1] * right[1]) + (left[2] * right[2]);
}

float Length3(const float value[3]) {
    return std::sqrt(Dot3(value, value));
}

void Normalize3(float value[3]) {
    const float length = Length3(value);
    if (length <= 0.000001f) {
        return;
    }
    value[0] /= length;
    value[1] /= length;
    value[2] /= length;
}

void Cross3(const float left[3], const float right[3], float result[3]) {
    result[0] = (left[1] * right[2]) - (left[2] * right[1]);
    result[1] = (left[2] * right[0]) - (left[0] * right[2]);
    result[2] = (left[0] * right[1]) - (left[1] * right[0]);
}

struct SpatialGains {
    float attenuation = 1.0f;
    float pan = 0.0f;
};

SpatialGains CalculateSpatialGains(const AudioChannelState& state,
                                   const AudioListener& listener,
                                   float distance_factor,
                                   float rolloff_factor) {
    if (!state.spatialized) {
        return {};
    }

    float offset[3] = {
            state.position[0] - listener.position[0],
            state.position[1] - listener.position[1],
            state.position[2] - listener.position[2],
    };
    const float distance = Length3(offset) * std::max(distance_factor, 0.000001f);
    const float minimum = std::max(state.min_distance, 0.0f);
    const float maximum = std::max(state.max_distance, minimum + 0.000001f);

    SpatialGains gains;
    if (distance > minimum) {
        const float normalized = std::clamp(
                (distance - minimum) / (maximum - minimum), 0.0f, 1.0f);
        const float exponent = std::max(rolloff_factor, 0.0f);
        gains.attenuation = exponent == 0.0f
                ? 1.0f
                : std::pow(1.0f - normalized, exponent);
    }

    if (distance > 0.000001f) {
        Normalize3(offset);
        float right[3];
        Cross3(listener.up, listener.forward, right);
        Normalize3(right);
        gains.pan = std::clamp(Dot3(offset, right), -1.0f, 1.0f);
    }
    return gains;
}

}  // namespace

bool AndroidAudioBackend::init(int mix_rate, int max_channels) {
    const std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [_, state] : channels_) {
        if (state.stream != nullptr) {
            state.stream->cancel();
        }
    }
    mix_rate_ = mix_rate > 0 ? mix_rate : 44100;
    max_channels_ = max_channels > 0 ? max_channels : 64;
    channels_.clear();
    mix_scratch_.assign(kMixScratchFrames * 2, 0);
    next_channel_id_ = 1;
    paused_ = false;
    initialized_ = true;
    PlatformRuntime::instance().log_info(
            "Android audio backend initialized; PCM mixer is ready.");
    return true;
}

void AndroidAudioBackend::shutdown() {
    const std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [_, state] : channels_) {
        if (state.stream != nullptr) {
            state.stream->cancel();
        }
    }
    channels_.clear();
    mix_scratch_.clear();
    initialized_ = false;
    paused_ = false;
}

bool AndroidAudioBackend::is_initialized() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return initialized_;
}

int AndroidAudioBackend::mix_rate() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return mix_rate_;
}

int AndroidAudioBackend::max_channels() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return max_channels_;
}

size_t AndroidAudioBackend::active_channel_count() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    size_t count = 0;
    for (const auto& [_, state] : channels_) {
        if (!state.finished) {
            ++count;
        }
    }
    return count;
}

int AndroidAudioBackend::play(AudioClipView clip, bool looped, uint32_t start_frame) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_ || clip.samples == nullptr || clip.frame_count == 0 || clip.channel_count <= 0) {
        return -1;
    }
    for (auto it = channels_.begin(); it != channels_.end();) {
        if (it->second.finished) {
            it = channels_.erase(it);
        } else {
            ++it;
        }
    }
    if (static_cast<int>(channels_.size()) >= max_channels_) {
        return -1;
    }

    const int channel_id = next_channel_id_++;
    AudioChannelState state;
    state.id = channel_id;
    state.looped = looped;
    const size_t start = looped
            ? (static_cast<size_t>(start_frame) % clip.frame_count)
            : std::min(static_cast<size_t>(start_frame), clip.frame_count);
    state.source_frame_position = static_cast<double>(start);
    state.position_frames = ClampFramePosition(state.source_frame_position);
    state.clip = clip;
    channels_.emplace(channel_id, state);
    return channel_id;
}

int AndroidAudioBackend::play_stream(std::shared_ptr<PcmStreamBuffer> stream) {
    if (stream == nullptr) {
        return -1;
    }
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_ || stream->sample_rate() != mix_rate_) {
        return -1;
    }
    for (auto it = channels_.begin(); it != channels_.end();) {
        if (it->second.finished) {
            it = channels_.erase(it);
        } else {
            ++it;
        }
    }
    if (static_cast<int>(channels_.size()) >= max_channels_) {
        return -1;
    }

    const int channel_id = next_channel_id_++;
    AudioChannelState state;
    state.id = channel_id;
    state.stream = std::move(stream);
    channels_.emplace(channel_id, std::move(state));
    return channel_id;
}

void AndroidAudioBackend::stop(int channel_id) {
    const std::lock_guard<std::mutex> lock(mutex_);
    auto it = channels_.find(channel_id);
    if (it != channels_.end()) {
        if (it->second.stream != nullptr) {
            it->second.stream->cancel();
        }
        channels_.erase(it);
    }
}

void AndroidAudioBackend::stop_all() {
    const std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [_, state] : channels_) {
        if (state.stream != nullptr) {
            state.stream->cancel();
        }
    }
    channels_.clear();
}

void AndroidAudioBackend::collect_finished_channels() {
    const std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = channels_.begin(); it != channels_.end();) {
        if (it->second.finished) {
            it = channels_.erase(it);
        } else {
            ++it;
        }
    }
}

void AndroidAudioBackend::set_paused(bool paused) {
    const std::lock_guard<std::mutex> lock(mutex_);
    paused_ = paused;
}

bool AndroidAudioBackend::is_paused() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return paused_;
}

void AndroidAudioBackend::set_channel_paused(int channel_id, bool paused) {
    const std::lock_guard<std::mutex> lock(mutex_);
    auto it = channels_.find(channel_id);
    if (it != channels_.end()) {
        it->second.paused = paused;
    }
}

bool AndroidAudioBackend::is_channel_paused(int channel_id) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    auto it = channels_.find(channel_id);
    return it != channels_.end() && it->second.paused;
}

void AndroidAudioBackend::set_volume(int channel_id, float volume) {
    const std::lock_guard<std::mutex> lock(mutex_);
    auto it = channels_.find(channel_id);
    if (it != channels_.end()) {
        it->second.volume = std::clamp(volume, 0.0f, 1.0f);
    }
}

void AndroidAudioBackend::set_pan(int channel_id, float pan) {
    const std::lock_guard<std::mutex> lock(mutex_);
    auto it = channels_.find(channel_id);
    if (it != channels_.end()) {
        it->second.pan = std::clamp(pan, -1.0f, 1.0f);
    }
}

void AndroidAudioBackend::set_spatial_position(int channel_id,
                                               const float position[3],
                                               const float velocity[3],
                                               float min_distance,
                                               float max_distance) {
    if (position == nullptr || velocity == nullptr) {
        return;
    }
    const std::lock_guard<std::mutex> lock(mutex_);
    auto it = channels_.find(channel_id);
    if (it == channels_.end()) {
        return;
    }
    AudioChannelState& state = it->second;
    state.spatialized = true;
    std::copy(position, position + 3, state.position);
    std::copy(velocity, velocity + 3, state.velocity);
    state.min_distance = std::max(min_distance, 0.0f);
    state.max_distance = std::max(max_distance, state.min_distance);
}

void AndroidAudioBackend::clear_spatial_position(int channel_id) {
    const std::lock_guard<std::mutex> lock(mutex_);
    auto it = channels_.find(channel_id);
    if (it != channels_.end()) {
        it->second.spatialized = false;
    }
}

bool AndroidAudioBackend::set_current_position(int channel_id, uint32_t position_frames) {
    const std::lock_guard<std::mutex> lock(mutex_);
    auto it = channels_.find(channel_id);
    if (it == channels_.end() || it->second.stream != nullptr ||
        it->second.clip.frame_count == 0) {
        return false;
    }

    AudioChannelState& state = it->second;
    const size_t position = state.looped
            ? static_cast<size_t>(position_frames) % state.clip.frame_count
            : std::min(static_cast<size_t>(position_frames), state.clip.frame_count);
    state.source_frame_position = static_cast<double>(position);
    state.position_frames = ClampFramePosition(state.source_frame_position);
    return true;
}

uint32_t AndroidAudioBackend::current_position(int channel_id) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    auto it = channels_.find(channel_id);
    return it == channels_.end() ? 0 : it->second.position_frames;
}

bool AndroidAudioBackend::is_playing(int channel_id) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    auto it = channels_.find(channel_id);
    return initialized_ && it != channels_.end() && !it->second.finished;
}

void AndroidAudioBackend::update_listener(const AudioListener& listener) {
    const std::lock_guard<std::mutex> lock(mutex_);
    listener_ = listener;
}

void AndroidAudioBackend::set_distance_factor(float factor) {
    const std::lock_guard<std::mutex> lock(mutex_);
    distance_factor_ = std::max(factor, 0.000001f);
}

void AndroidAudioBackend::set_rolloff_factor(float factor) {
    const std::lock_guard<std::mutex> lock(mutex_);
    rolloff_factor_ = std::max(factor, 0.0f);
}

size_t AndroidAudioBackend::mix_interleaved_stereo(int16_t* output, size_t frame_count) {
    if (output == nullptr || frame_count == 0) {
        return 0;
    }

    const std::lock_guard<std::mutex> lock(mutex_);
    std::fill(output, output + (frame_count * 2), 0);
    if (!initialized_ || paused_ || channels_.empty()) {
        return frame_count;
    }

    size_t processed_frames = 0;
    while (processed_frames < frame_count) {
        const size_t chunk_frames =
                std::min(kMixScratchFrames, frame_count - processed_frames);
        std::fill(
                mix_scratch_.begin(),
                mix_scratch_.begin() + static_cast<ptrdiff_t>(chunk_frames * 2),
                0);
        for (auto& [channel_id, state] : channels_) {
            if (state.finished || state.paused) {
                continue;
            }

            const SpatialGains spatial = CalculateSpatialGains(
                    state, listener_, distance_factor_, rolloff_factor_);
            const float pan = std::clamp(state.pan + spatial.pan, -1.0f, 1.0f);
            const float volume =
                    std::clamp(state.volume, 0.0f, 1.0f) * spatial.attenuation;
            const float left_gain = volume * (pan > 0.0f ? 1.0f - pan : 1.0f);
            const float right_gain = volume * (pan < 0.0f ? 1.0f + pan : 1.0f);

            if (state.stream != nullptr) {
                for (size_t out_frame = 0; out_frame < chunk_frames; ++out_frame) {
                    int16_t left = 0;
                    int16_t right = 0;
                    const PcmStreamReadResult result =
                            state.stream->read_stereo(&left, &right);
                    if (result == PcmStreamReadResult::EndOfStream) {
                        state.finished = true;
                        break;
                    }
                    if (result == PcmStreamReadResult::Underrun) {
                        continue;
                    }
                    const size_t out_index = out_frame * 2;
                    mix_scratch_[out_index] += ScaledSample(left, left_gain);
                    mix_scratch_[out_index + 1] += ScaledSample(right, right_gain);
                }
                state.source_frame_position =
                        static_cast<double>(state.stream->consumed_frames());
                state.position_frames = ClampFramePosition(state.source_frame_position);
                continue;
            }

            if (state.clip.samples == nullptr || state.clip.frame_count == 0 ||
                state.clip.channel_count <= 0) {
                state.finished = true;
                continue;
            }

            const int source_rate =
                    state.clip.sample_rate > 0 ? state.clip.sample_rate : mix_rate_;
            const double step =
                    static_cast<double>(source_rate) / static_cast<double>(mix_rate_);
            double source_position = state.source_frame_position;
            bool ended = false;

            for (size_t out_frame = 0; out_frame < chunk_frames; ++out_frame) {
                if (source_position >= static_cast<double>(state.clip.frame_count)) {
                    if (!state.looped) {
                        ended = true;
                        break;
                    }
                    source_position = WrappedPosition(
                            source_position, state.clip.frame_count);
                }

                const size_t source_frame = static_cast<size_t>(source_position);
                const int16_t left = ClipSample(state.clip, source_frame, 0);
                const int16_t right = state.clip.channel_count == 1
                        ? left
                        : ClipSample(state.clip, source_frame, 1);
                const size_t out_index = out_frame * 2;
                mix_scratch_[out_index] += ScaledSample(left, left_gain);
                mix_scratch_[out_index + 1] += ScaledSample(right, right_gain);
                source_position += step;
            }

            if (ended || (!state.looped &&
                          source_position >= static_cast<double>(state.clip.frame_count))) {
                state.finished = true;
                state.source_frame_position = static_cast<double>(state.clip.frame_count);
                state.position_frames = ClampFramePosition(state.source_frame_position);
                continue;
            }

            if (state.looped) {
                source_position = WrappedPosition(
                        source_position, state.clip.frame_count);
            }
            state.source_frame_position = source_position;
            state.position_frames = ClampFramePosition(source_position);
        }

        const size_t output_offset = processed_frames * 2;
        for (size_t i = 0; i < chunk_frames * 2; ++i) {
            output[output_offset + i] = ClampToInt16(mix_scratch_[i]);
        }
        processed_frames += chunk_frames;
    }

    return frame_count;
}

std::optional<AudioChannelState> AndroidAudioBackend::channel_state(int channel_id) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    auto it = channels_.find(channel_id);
    if (it == channels_.end() || it->second.finished) {
        return std::nullopt;
    }
    return it->second;
}

AndroidAudioBackend& AudioBackend() {
    static AndroidAudioBackend backend;
    return backend;
}

}  // namespace bk2::android
