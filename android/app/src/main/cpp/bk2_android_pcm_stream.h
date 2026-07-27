#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace bk2::android {

enum class PcmStreamReadResult {
    Frame,
    Underrun,
    EndOfStream,
};

class PcmStreamBuffer final {
public:
    PcmStreamBuffer(int sample_rate, size_t capacity_frames);

    int sample_rate() const;
    size_t capacity_frames() const;
    size_t available_frames() const;
    size_t writable_frames() const;
    uint64_t consumed_frames() const;
    uint64_t underrun_count() const;

    size_t write_stereo(const int16_t* samples, size_t frame_count);
    PcmStreamReadResult read_stereo(int16_t* left, int16_t* right);
    void mark_end_of_stream();
    void cancel();
    bool is_cancelled() const;
    bool is_end_of_stream() const;

private:
    int sample_rate_;
    size_t capacity_frames_;
    std::vector<int16_t> samples_;
    std::atomic<uint64_t> read_frame_{0};
    std::atomic<uint64_t> write_frame_{0};
    std::atomic<uint64_t> underrun_count_{0};
    std::atomic<bool> end_of_stream_{false};
    std::atomic<bool> cancelled_{false};
};

}  // namespace bk2::android
