#include "bk2_android_vorbis_stream.h"

#include "bk2_android_audio_backend.h"
#include "bk2_android_platform.h"
#include "bk2_port_paths.h"

#include <jni.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <sstream>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace bk2::android {
namespace {

constexpr int64_t kCodecTimeoutUs = 10000;
constexpr int32_t kPcmEncoding16Bit = 2;

bool IsRegularFile(const std::string& path) {
    struct stat status {};
    return stat(path.c_str(), &status) == 0 && S_ISREG(status.st_mode);
}

std::string NormalizeRelativePath(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    while (!path.empty() && path.front() == '/') {
        path.erase(path.begin());
    }
    while (path.rfind("./", 0) == 0) {
        path.erase(0, 2);
    }
    if (path.rfind("Data/", 0) == 0) {
        path.erase(0, 5);
    }
    return path;
}

std::string JoinPath(const std::string& left, const std::string& right) {
    if (left.empty()) {
        return right;
    }
    return left.back() == '/' ? left + right : left + "/" + right;
}

const char* StateName(VorbisStreamState state) {
    switch (state) {
        case VorbisStreamState::Starting:
            return "starting";
        case VorbisStreamState::Decoding:
            return "decoding";
        case VorbisStreamState::Completed:
            return "completed";
        case VorbisStreamState::Failed:
            return "failed";
        case VorbisStreamState::Stopped:
            return "stopped";
    }
    return "unknown";
}

bool ReadFormatInt32(AMediaFormat* format, const char* key, int32_t* value) {
    return format != nullptr && key != nullptr && value != nullptr &&
            AMediaFormat_getInt32(format, key, value);
}

bool WriteFramesBlocking(const std::shared_ptr<PcmStreamBuffer>& source,
                         const int16_t* samples,
                         size_t frame_count,
                         const std::atomic<bool>& stop_requested) {
    size_t written = 0;
    while (written < frame_count) {
        if (stop_requested.load(std::memory_order_acquire) || source->is_cancelled()) {
            return false;
        }
        const size_t count = source->write_stereo(
                samples + (written * 2), frame_count - written);
        if (count == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        written += count;
    }
    return true;
}

}  // namespace

std::string ResolveAndroidAudioPath(const std::string& path) {
    if (path.empty()) {
        return {};
    }
    if (IsRegularFile(path)) {
        return path;
    }

    const std::string relative = NormalizeRelativePath(path);
    const std::string data_root = GetPortPaths().data_root();
    const std::string direct = JoinPath(data_root, relative);
    if (IsRegularFile(direct)) {
        return direct;
    }
    const std::string nested = JoinPath(JoinPath(data_root, "Data"), relative);
    return IsRegularFile(nested) ? nested : direct;
}

bool ReadAndroidAudioMetadata(const std::string& path,
                              AndroidAudioMetadata* metadata,
                              std::string* error) {
    if (metadata == nullptr) {
        if (error != nullptr) {
            *error = "metadata output is null";
        }
        return false;
    }

    const std::string resolved_path = ResolveAndroidAudioPath(path);
    if (!IsRegularFile(resolved_path)) {
        if (error != nullptr) {
            *error = "audio file does not exist: " + resolved_path;
        }
        return false;
    }

    AMediaExtractor* extractor = AMediaExtractor_new();
    if (extractor == nullptr) {
        if (error != nullptr) {
            *error = "AMediaExtractor_new failed";
        }
        return false;
    }

    const int file = open(resolved_path.c_str(), O_RDONLY | O_CLOEXEC);
    struct stat file_status {};
    const bool file_ready = file >= 0 && fstat(file, &file_status) == 0;
    const media_status_t source_status = file_ready
            ? AMediaExtractor_setDataSourceFd(
                      extractor, file, 0, static_cast<off64_t>(file_status.st_size))
            : AMEDIA_ERROR_UNKNOWN;
    if (file >= 0) {
        close(file);
    }
    if (source_status != AMEDIA_OK) {
        AMediaExtractor_delete(extractor);
        if (error != nullptr) {
            *error = "AMediaExtractor_setDataSourceFd failed: " + resolved_path;
        }
        return false;
    }

    bool found = false;
    const size_t track_count = AMediaExtractor_getTrackCount(extractor);
    for (size_t track = 0; track < track_count; ++track) {
        AMediaFormat* format = AMediaExtractor_getTrackFormat(extractor, track);
        const char* mime = nullptr;
        if (format != nullptr &&
            AMediaFormat_getString(format, AMEDIAFORMAT_KEY_MIME, &mime) &&
            mime != nullptr && std::strncmp(mime, "audio/", 6) == 0) {
            int32_t sample_rate = 0;
            int32_t channel_count = 0;
            int64_t duration_us = 0;
            ReadFormatInt32(format, AMEDIAFORMAT_KEY_SAMPLE_RATE, &sample_rate);
            ReadFormatInt32(format, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &channel_count);
            AMediaFormat_getInt64(format, AMEDIAFORMAT_KEY_DURATION, &duration_us);
            if (sample_rate > 0 && channel_count > 0) {
                metadata->sample_rate = sample_rate;
                metadata->channel_count = channel_count;
                metadata->duration_ms =
                        duration_us > 0 ? static_cast<uint64_t>(duration_us / 1000) : 0;
                found = true;
            }
        }
        if (format != nullptr) {
            AMediaFormat_delete(format);
        }
        if (found) {
            break;
        }
    }
    AMediaExtractor_delete(extractor);

    if (!found) {
        if (error != nullptr) {
            *error = "no supported audio track: " + resolved_path;
        }
        return false;
    }
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

AndroidVorbisStream::AndroidVorbisStream() = default;

std::unique_ptr<AndroidVorbisStream> AndroidVorbisStream::Open(
        const std::string& path,
        uint64_t start_time_ms,
        size_t ring_seconds,
        std::string* error) {
    const std::string resolved_path = ResolveAndroidAudioPath(path);
    if (!IsRegularFile(resolved_path)) {
        if (error != nullptr) {
            *error = "OGG file does not exist: " + resolved_path;
        }
        return nullptr;
    }

    AMediaExtractor* extractor = AMediaExtractor_new();
    if (extractor == nullptr) {
        if (error != nullptr) {
            *error = "AMediaExtractor_new failed";
        }
        return nullptr;
    }
    const int file = open(resolved_path.c_str(), O_RDONLY | O_CLOEXEC);
    struct stat file_status {};
    const bool file_ready = file >= 0 && fstat(file, &file_status) == 0;
    const media_status_t data_source_status = file_ready
            ? AMediaExtractor_setDataSourceFd(
                      extractor, file, 0, static_cast<off64_t>(file_status.st_size))
            : AMEDIA_ERROR_UNKNOWN;
    if (file >= 0) {
        close(file);
    }
    if (data_source_status != AMEDIA_OK) {
        AMediaExtractor_delete(extractor);
        if (error != nullptr) {
            *error = "AMediaExtractor_setDataSourceFd failed: " + resolved_path;
        }
        return nullptr;
    }

    AMediaFormat* selected_format = nullptr;
    const char* selected_mime = nullptr;
    const size_t track_count = AMediaExtractor_getTrackCount(extractor);
    for (size_t track = 0; track < track_count; ++track) {
        AMediaFormat* format = AMediaExtractor_getTrackFormat(extractor, track);
        const char* mime = nullptr;
        if (format != nullptr && AMediaFormat_getString(
                    format, AMEDIAFORMAT_KEY_MIME, &mime) &&
            mime != nullptr && std::strncmp(mime, "audio/", 6) == 0) {
            if (AMediaExtractor_selectTrack(extractor, track) == AMEDIA_OK) {
                selected_format = format;
                selected_mime = mime;
                break;
            }
        }
        if (format != nullptr) {
            AMediaFormat_delete(format);
        }
    }
    if (selected_format == nullptr || selected_mime == nullptr) {
        AMediaExtractor_delete(extractor);
        if (error != nullptr) {
            *error = "no audio track in OGG file";
        }
        return nullptr;
    }

    int32_t sample_rate = 0;
    int32_t channel_count = 0;
    int64_t duration_us = 0;
    ReadFormatInt32(selected_format, AMEDIAFORMAT_KEY_SAMPLE_RATE, &sample_rate);
    ReadFormatInt32(selected_format, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &channel_count);
    AMediaFormat_getInt64(selected_format, AMEDIAFORMAT_KEY_DURATION, &duration_us);
    if (sample_rate <= 0 || channel_count <= 0) {
        AMediaFormat_delete(selected_format);
        AMediaExtractor_delete(extractor);
        if (error != nullptr) {
            *error = "invalid audio track format";
        }
        return nullptr;
    }

    AMediaCodec* codec = AMediaCodec_createDecoderByType(selected_mime);
    if (codec == nullptr ||
        AMediaCodec_configure(codec, selected_format, nullptr, nullptr, 0) != AMEDIA_OK ||
        AMediaCodec_start(codec) != AMEDIA_OK) {
        if (codec != nullptr) {
            AMediaCodec_delete(codec);
        }
        AMediaFormat_delete(selected_format);
        AMediaExtractor_delete(extractor);
        if (error != nullptr) {
            *error = std::string("cannot start decoder for ") + selected_mime;
        }
        return nullptr;
    }
    AMediaFormat_delete(selected_format);

    const uint64_t start_time_us = start_time_ms * 1000;
    if (start_time_us > 0) {
        AMediaExtractor_seekTo(
                extractor, static_cast<int64_t>(start_time_us),
                AMEDIAEXTRACTOR_SEEK_PREVIOUS_SYNC);
    }

    auto stream = std::unique_ptr<AndroidVorbisStream>(new AndroidVorbisStream());
    stream->sample_rate_ = sample_rate;
    stream->source_channel_count_ = channel_count;
    stream->duration_ms_ = duration_us > 0
            ? static_cast<uint64_t>(duration_us / 1000)
            : 0;
    stream->pcm_source_ = std::make_shared<PcmStreamBuffer>(
            sample_rate,
            static_cast<size_t>(sample_rate) * std::max<size_t>(ring_seconds, 1));
    AndroidVorbisStream* raw_stream = stream.get();
    stream->decode_thread_ = std::thread(
            [raw_stream, extractor, codec, start_time_us]() {
                raw_stream->decode_loop(extractor, codec, start_time_us);
            });
    if (error != nullptr) {
        error->clear();
    }
    return stream;
}

AndroidVorbisStream::~AndroidVorbisStream() {
    stop();
}

std::shared_ptr<PcmStreamBuffer> AndroidVorbisStream::pcm_source() const {
    return pcm_source_;
}

int AndroidVorbisStream::sample_rate() const {
    return sample_rate_;
}

int AndroidVorbisStream::source_channel_count() const {
    return source_channel_count_;
}

uint64_t AndroidVorbisStream::duration_ms() const {
    return duration_ms_;
}

uint64_t AndroidVorbisStream::decoded_frame_count() const {
    return decoded_frame_count_.load(std::memory_order_acquire);
}

VorbisStreamState AndroidVorbisStream::state() const {
    return static_cast<VorbisStreamState>(state_.load(std::memory_order_acquire));
}

std::string AndroidVorbisStream::error() const {
    const std::lock_guard<std::mutex> lock(error_mutex_);
    return error_;
}

void AndroidVorbisStream::stop() {
    stop_requested_.store(true, std::memory_order_release);
    if (pcm_source_ != nullptr) {
        pcm_source_->cancel();
    }
    if (decode_thread_.joinable()) {
        decode_thread_.join();
    }
}

void AndroidVorbisStream::decode_loop(
        AMediaExtractor* extractor,
        AMediaCodec* codec,
        uint64_t start_time_us) {
    state_.store(static_cast<int>(VorbisStreamState::Decoding), std::memory_order_release);
    bool input_complete = false;
    bool output_complete = false;
    int output_channels = source_channel_count_;
    int output_sample_rate = sample_rate_;
    int output_pcm_encoding = kPcmEncoding16Bit;
    std::vector<int16_t> stereo_samples;

    while (!stop_requested_.load(std::memory_order_acquire) && !output_complete) {
        if (!input_complete) {
            const ssize_t input_index = AMediaCodec_dequeueInputBuffer(codec, 0);
            if (input_index >= 0) {
                size_t input_capacity = 0;
                uint8_t* input_buffer = AMediaCodec_getInputBuffer(
                        codec, static_cast<size_t>(input_index), &input_capacity);
                const ssize_t sample_size = input_buffer == nullptr
                        ? -1
                        : AMediaExtractor_readSampleData(
                                  extractor, input_buffer, input_capacity);
                if (sample_size < 0) {
                    AMediaCodec_queueInputBuffer(
                            codec, static_cast<size_t>(input_index), 0, 0, 0,
                            AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
                    input_complete = true;
                } else {
                    const int64_t sample_time = AMediaExtractor_getSampleTime(extractor);
                    AMediaCodec_queueInputBuffer(
                            codec,
                            static_cast<size_t>(input_index),
                            0,
                            static_cast<size_t>(sample_size),
                            sample_time >= 0 ? static_cast<uint64_t>(sample_time) : 0,
                            0);
                    AMediaExtractor_advance(extractor);
                }
            }
        }

        AMediaCodecBufferInfo info {};
        const ssize_t output_index =
                AMediaCodec_dequeueOutputBuffer(codec, &info, kCodecTimeoutUs);
        if (output_index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat* output_format = AMediaCodec_getOutputFormat(codec);
            ReadFormatInt32(
                    output_format, AMEDIAFORMAT_KEY_SAMPLE_RATE, &output_sample_rate);
            ReadFormatInt32(
                    output_format, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &output_channels);
            ReadFormatInt32(output_format, "pcm-encoding", &output_pcm_encoding);
            AMediaFormat_delete(output_format);
            if (output_sample_rate != sample_rate_ ||
                output_channels <= 0 || output_channels > 2 ||
                output_pcm_encoding != kPcmEncoding16Bit) {
                set_error("unsupported decoder PCM output format");
                break;
            }
            continue;
        }
        if (output_index < 0) {
            continue;
        }

        size_t output_capacity = 0;
        uint8_t* output_buffer = AMediaCodec_getOutputBuffer(
                codec, static_cast<size_t>(output_index), &output_capacity);
        if (output_buffer != nullptr && info.size > 0 &&
            static_cast<size_t>(info.offset + info.size) <= output_capacity) {
            const int16_t* pcm = reinterpret_cast<const int16_t*>(
                    output_buffer + info.offset);
            size_t frame_count =
                    static_cast<size_t>(info.size) /
                    (static_cast<size_t>(output_channels) * sizeof(int16_t));
            size_t skip_frames = 0;
            if (start_time_us > 0 &&
                static_cast<uint64_t>(std::max<int64_t>(info.presentationTimeUs, 0)) <
                        start_time_us) {
                const uint64_t delta_us =
                        start_time_us -
                        static_cast<uint64_t>(std::max<int64_t>(
                                info.presentationTimeUs, 0));
                skip_frames = std::min<size_t>(
                        frame_count,
                        static_cast<size_t>(
                                (delta_us * static_cast<uint64_t>(sample_rate_)) /
                                1000000));
            }
            pcm += skip_frames * static_cast<size_t>(output_channels);
            frame_count -= skip_frames;

            stereo_samples.resize(frame_count * 2);
            if (output_channels == 1) {
                for (size_t frame = 0; frame < frame_count; ++frame) {
                    stereo_samples[frame * 2] = pcm[frame];
                    stereo_samples[(frame * 2) + 1] = pcm[frame];
                }
            } else if (frame_count > 0) {
                std::memcpy(
                        stereo_samples.data(),
                        pcm,
                        frame_count * 2 * sizeof(int16_t));
            }
            if (frame_count > 0 &&
                !WriteFramesBlocking(
                        pcm_source_, stereo_samples.data(), frame_count,
                        stop_requested_)) {
                break;
            }
            decoded_frame_count_.fetch_add(frame_count, std::memory_order_release);
        }

        output_complete =
                (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
        AMediaCodec_releaseOutputBuffer(
                codec, static_cast<size_t>(output_index), false);
    }

    AMediaCodec_stop(codec);
    AMediaCodec_delete(codec);
    AMediaExtractor_delete(extractor);

    if (!error().empty()) {
        pcm_source_->cancel();
        state_.store(static_cast<int>(VorbisStreamState::Failed), std::memory_order_release);
    } else if (stop_requested_.load(std::memory_order_acquire)) {
        pcm_source_->cancel();
        state_.store(static_cast<int>(VorbisStreamState::Stopped), std::memory_order_release);
    } else {
        pcm_source_->mark_end_of_stream();
        state_.store(static_cast<int>(VorbisStreamState::Completed), std::memory_order_release);
    }
}

void AndroidVorbisStream::set_error(const std::string& error) {
    const std::lock_guard<std::mutex> lock(error_mutex_);
    error_ = error;
}

std::string RunMusicStreamingProbe() {
    const std::string path = JoinPath(GetPortPaths().data_root(), "Music/Intro_Main.ogg");
    std::string open_error;
    auto stream = AndroidVorbisStream::Open(path, 0, 2, &open_error);
    if (stream == nullptr) {
        return "music_stream=failed; music_error=" + open_error;
    }
    if (!AudioBackend().is_initialized()) {
        AudioBackend().init(stream->sample_rate(), 96);
    }
    if (AudioBackend().mix_rate() != stream->sample_rate()) {
        stream->stop();
        return "music_stream=failed; music_error=mix_rate_mismatch";
    }

    const auto prebuffer_deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (stream->pcm_source()->available_frames() < 4096 &&
           stream->state() != VorbisStreamState::Failed &&
           std::chrono::steady_clock::now() < prebuffer_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    const int channel = AudioBackend().play_stream(stream->pcm_source());
    if (channel >= 0) {
        AudioBackend().set_volume(channel, 0.05f);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const uint64_t consumed_frames = stream->pcm_source()->consumed_frames();
    const uint64_t decoded_frames = stream->decoded_frame_count();
    const uint64_t underruns = stream->pcm_source()->underrun_count();
    const VorbisStreamState stream_state = stream->state();
    const std::string stream_error = stream->error();
    AudioBackend().stop(channel);
    stream->stop();

    std::ostringstream report;
    report << "music_stream="
           << (channel >= 0 && consumed_frames > 0 ? "probed" : "failed")
           << "; music_path=" << path
           << "; music_channel=" << channel
           << "; music_sample_rate=" << stream->sample_rate()
           << "; music_source_channels=" << stream->source_channel_count()
           << "; music_duration_ms=" << stream->duration_ms()
           << "; music_decoded_frames=" << decoded_frames
           << "; music_consumed_frames=" << consumed_frames
           << "; music_underruns=" << underruns
           << "; music_decoder_state=" << StateName(stream_state);
    if (!stream_error.empty()) {
        report << "; music_error=" << stream_error;
    }
    const std::string text = report.str();
    PlatformRuntime::instance().log_info(text);
    return text;
}

}  // namespace bk2::android

extern "C" JNIEXPORT jstring JNICALL
Java_com_nival_blitzkrieg2_NativeBridge_runMusicStreamingProbe(JNIEnv* env, jclass) {
    const std::string report = bk2::android::RunMusicStreamingProbe();
    return env->NewStringUTF(report.c_str());
}
