#pragma once

#include "bk2_android_pcm_stream.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace bk2::android {

struct AudioClipView {
    const int16_t* samples = nullptr;
    size_t frame_count = 0;
    int channel_count = 0;
    int sample_rate = 0;
};

struct AudioListener {
    float position[3] = {0.0f, 0.0f, 0.0f};
    float forward[3] = {0.0f, 0.0f, 1.0f};
    float up[3] = {0.0f, 1.0f, 0.0f};
};

struct AudioChannelState {
    int id = -1;
    bool looped = false;
    bool paused = false;
    bool finished = false;
    float volume = 1.0f;
    float pan = 0.0f;
    uint32_t position_frames = 0;
    double source_frame_position = 0.0;
    bool spatialized = false;
    float position[3] = {0.0f, 0.0f, 0.0f};
    float velocity[3] = {0.0f, 0.0f, 0.0f};
    float min_distance = 1.0f;
    float max_distance = 1000000000.0f;
    AudioClipView clip;
    std::shared_ptr<PcmStreamBuffer> stream;
};

class AndroidAudioBackend final {
public:
    bool init(int mix_rate, int max_channels);
    void shutdown();
    bool is_initialized() const;
    int mix_rate() const;
    int max_channels() const;
    size_t active_channel_count() const;

    int play(AudioClipView clip, bool looped, uint32_t start_frame);
    int play_stream(std::shared_ptr<PcmStreamBuffer> stream);
    void stop(int channel_id);
    void stop_all();
    void collect_finished_channels();
    void set_paused(bool paused);
    bool is_paused() const;
    void set_channel_paused(int channel_id, bool paused);
    bool is_channel_paused(int channel_id) const;

    void set_volume(int channel_id, float volume);
    void set_pan(int channel_id, float pan);
    void set_spatial_position(int channel_id,
                              const float position[3],
                              const float velocity[3],
                              float min_distance,
                              float max_distance);
    void clear_spatial_position(int channel_id);
    bool set_current_position(int channel_id, uint32_t position_frames);
    uint32_t current_position(int channel_id) const;
    bool is_playing(int channel_id) const;
    void update_listener(const AudioListener& listener);
    void set_distance_factor(float factor);
    void set_rolloff_factor(float factor);

    size_t mix_interleaved_stereo(int16_t* output, size_t frame_count);
    std::optional<AudioChannelState> channel_state(int channel_id) const;

private:
    mutable std::mutex mutex_;
    int mix_rate_ = 44100;
    int max_channels_ = 0;
    int next_channel_id_ = 1;
    bool initialized_ = false;
    bool paused_ = false;
    AudioListener listener_;
    float distance_factor_ = 1.0f;
    float rolloff_factor_ = 1.0f;
    std::unordered_map<int, AudioChannelState> channels_;
    std::vector<int32_t> mix_scratch_;
};

AndroidAudioBackend& AudioBackend();

}  // namespace bk2::android
