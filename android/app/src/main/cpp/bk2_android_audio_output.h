#pragma once

#include <oboe/Oboe.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace bk2::android {

class AndroidAudioOutput final : public oboe::AudioStreamDataCallback,
                                 public oboe::AudioStreamErrorCallback {
public:
    bool start();
    void pause();
    void resume();
    void shutdown();
    void service();

    bool is_open() const;
    bool is_started() const;
    int sample_rate() const;
    int channel_count() const;
    uint64_t callback_count() const;
    uint64_t rendered_frame_count() const;
    std::string diagnostic_report() const;

    oboe::DataCallbackResult onAudioReady(
            oboe::AudioStream* stream, void* audio_data, int32_t num_frames) override;
    void onErrorAfterClose(oboe::AudioStream* stream, oboe::Result error) override;

private:
    bool open_and_start_locked();
    void close_locked();

    mutable std::mutex mutex_;
    std::shared_ptr<oboe::AudioStream> stream_;
    std::atomic<bool> desired_running_{false};
    std::atomic<bool> restart_requested_{false};
    std::atomic<int32_t> last_error_{static_cast<int32_t>(oboe::Result::OK)};
    std::atomic<uint64_t> callback_count_{0};
    std::atomic<uint64_t> rendered_frame_count_{0};
};

AndroidAudioOutput& AudioOutput();

}  // namespace bk2::android
