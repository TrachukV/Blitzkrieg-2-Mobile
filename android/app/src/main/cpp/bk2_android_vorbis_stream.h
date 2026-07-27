#pragma once

#include "bk2_android_pcm_stream.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

struct AMediaCodec;
struct AMediaExtractor;

namespace bk2::android {

enum class VorbisStreamState {
    Starting,
    Decoding,
    Completed,
    Failed,
    Stopped,
};

struct AndroidAudioMetadata {
    int sample_rate = 0;
    int channel_count = 0;
    uint64_t duration_ms = 0;
};

class AndroidVorbisStream final {
public:
    static std::unique_ptr<AndroidVorbisStream> Open(
            const std::string& path,
            uint64_t start_time_ms,
            size_t ring_seconds,
            std::string* error);

    ~AndroidVorbisStream();

    AndroidVorbisStream(const AndroidVorbisStream&) = delete;
    AndroidVorbisStream& operator=(const AndroidVorbisStream&) = delete;

    std::shared_ptr<PcmStreamBuffer> pcm_source() const;
    int sample_rate() const;
    int source_channel_count() const;
    uint64_t duration_ms() const;
    uint64_t decoded_frame_count() const;
    VorbisStreamState state() const;
    std::string error() const;
    void stop();

private:
    AndroidVorbisStream();
    void decode_loop(AMediaExtractor* extractor,
                     AMediaCodec* codec,
                     uint64_t start_time_us);
    void set_error(const std::string& error);

    std::shared_ptr<PcmStreamBuffer> pcm_source_;
    int sample_rate_ = 0;
    int source_channel_count_ = 0;
    uint64_t duration_ms_ = 0;
    std::atomic<uint64_t> decoded_frame_count_{0};
    std::atomic<int> state_{static_cast<int>(VorbisStreamState::Starting)};
    std::atomic<bool> stop_requested_{false};
    mutable std::mutex error_mutex_;
    std::string error_;
    std::thread decode_thread_;
};

std::string ResolveAndroidAudioPath(const std::string& path);
bool ReadAndroidAudioMetadata(const std::string& path,
                              AndroidAudioMetadata* metadata,
                              std::string* error);
std::string RunMusicStreamingProbe();

}  // namespace bk2::android
