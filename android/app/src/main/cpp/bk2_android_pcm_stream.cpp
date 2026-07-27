#include "bk2_android_pcm_stream.h"

#include <algorithm>
#include <cstring>

namespace bk2::android {

PcmStreamBuffer::PcmStreamBuffer(int sample_rate, size_t capacity_frames)
    : sample_rate_(sample_rate > 0 ? sample_rate : 44100),
      capacity_frames_(std::max<size_t>(capacity_frames, 1)),
      samples_(capacity_frames_ * 2, 0) {
}

int PcmStreamBuffer::sample_rate() const {
    return sample_rate_;
}

size_t PcmStreamBuffer::capacity_frames() const {
    return capacity_frames_;
}

size_t PcmStreamBuffer::available_frames() const {
    const uint64_t read = read_frame_.load(std::memory_order_acquire);
    const uint64_t write = write_frame_.load(std::memory_order_acquire);
    return static_cast<size_t>(std::min<uint64_t>(write - read, capacity_frames_));
}

size_t PcmStreamBuffer::writable_frames() const {
    return capacity_frames_ - available_frames();
}

uint64_t PcmStreamBuffer::consumed_frames() const {
    return read_frame_.load(std::memory_order_acquire);
}

uint64_t PcmStreamBuffer::underrun_count() const {
    return underrun_count_.load(std::memory_order_relaxed);
}

size_t PcmStreamBuffer::write_stereo(const int16_t* samples, size_t frame_count) {
    if (samples == nullptr || frame_count == 0 ||
        cancelled_.load(std::memory_order_acquire)) {
        return 0;
    }

    const uint64_t write = write_frame_.load(std::memory_order_relaxed);
    const uint64_t read = read_frame_.load(std::memory_order_acquire);
    const size_t available = static_cast<size_t>(
            std::min<uint64_t>(write - read, capacity_frames_));
    const size_t frames_to_write =
            std::min(frame_count, capacity_frames_ - available);
    if (frames_to_write == 0) {
        return 0;
    }

    const size_t write_index = static_cast<size_t>(write % capacity_frames_);
    const size_t first_frames =
            std::min(frames_to_write, capacity_frames_ - write_index);
    std::memcpy(
            &samples_[write_index * 2],
            samples,
            first_frames * 2 * sizeof(int16_t));
    const size_t second_frames = frames_to_write - first_frames;
    if (second_frames > 0) {
        std::memcpy(
                samples_.data(),
                samples + (first_frames * 2),
                second_frames * 2 * sizeof(int16_t));
    }
    write_frame_.store(write + frames_to_write, std::memory_order_release);
    return frames_to_write;
}

PcmStreamReadResult PcmStreamBuffer::read_stereo(int16_t* left, int16_t* right) {
    if (left == nullptr || right == nullptr) {
        return PcmStreamReadResult::EndOfStream;
    }

    const uint64_t read = read_frame_.load(std::memory_order_relaxed);
    const uint64_t write = write_frame_.load(std::memory_order_acquire);
    if (read == write) {
        *left = 0;
        *right = 0;
        if (end_of_stream_.load(std::memory_order_acquire) ||
            cancelled_.load(std::memory_order_acquire)) {
            return PcmStreamReadResult::EndOfStream;
        }
        underrun_count_.fetch_add(1, std::memory_order_relaxed);
        return PcmStreamReadResult::Underrun;
    }

    const size_t read_index = static_cast<size_t>(read % capacity_frames_) * 2;
    *left = samples_[read_index];
    *right = samples_[read_index + 1];
    read_frame_.store(read + 1, std::memory_order_release);
    return PcmStreamReadResult::Frame;
}

void PcmStreamBuffer::mark_end_of_stream() {
    end_of_stream_.store(true, std::memory_order_release);
}

void PcmStreamBuffer::cancel() {
    cancelled_.store(true, std::memory_order_release);
    end_of_stream_.store(true, std::memory_order_release);
}

bool PcmStreamBuffer::is_cancelled() const {
    return cancelled_.load(std::memory_order_acquire);
}

bool PcmStreamBuffer::is_end_of_stream() const {
    return end_of_stream_.load(std::memory_order_acquire) && available_frames() == 0;
}

}  // namespace bk2::android
