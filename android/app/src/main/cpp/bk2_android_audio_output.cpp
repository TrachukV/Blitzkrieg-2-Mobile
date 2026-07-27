#include "bk2_android_audio_output.h"

#include "bk2_android_audio_backend.h"
#include "bk2_android_platform.h"

#include <jni.h>

#include <chrono>
#include <sstream>
#include <thread>

namespace bk2::android {
namespace {

const char* ApiName(oboe::AudioApi api) {
    switch (api) {
        case oboe::AudioApi::AAudio:
            return "AAudio";
        case oboe::AudioApi::OpenSLES:
            return "OpenSLES";
        case oboe::AudioApi::Unspecified:
            return "Unspecified";
    }
    return "Unknown";
}

const char* StateName(oboe::StreamState state) {
    switch (state) {
        case oboe::StreamState::Uninitialized:
            return "Uninitialized";
        case oboe::StreamState::Unknown:
            return "Unknown";
        case oboe::StreamState::Open:
            return "Open";
        case oboe::StreamState::Starting:
            return "Starting";
        case oboe::StreamState::Started:
            return "Started";
        case oboe::StreamState::Pausing:
            return "Pausing";
        case oboe::StreamState::Paused:
            return "Paused";
        case oboe::StreamState::Flushing:
            return "Flushing";
        case oboe::StreamState::Flushed:
            return "Flushed";
        case oboe::StreamState::Stopping:
            return "Stopping";
        case oboe::StreamState::Stopped:
            return "Stopped";
        case oboe::StreamState::Closing:
            return "Closing";
        case oboe::StreamState::Closed:
            return "Closed";
        case oboe::StreamState::Disconnected:
            return "Disconnected";
    }
    return "Unknown";
}

}  // namespace

bool AndroidAudioOutput::start() {
    desired_running_.store(true, std::memory_order_release);
    const std::lock_guard<std::mutex> lock(mutex_);
    if (stream_ != nullptr) {
        const oboe::StreamState state = stream_->getState();
        if (state == oboe::StreamState::Starting || state == oboe::StreamState::Started) {
            return true;
        }
        if (state == oboe::StreamState::Open || state == oboe::StreamState::Paused ||
            state == oboe::StreamState::Stopped || state == oboe::StreamState::Flushed) {
            const oboe::Result result = stream_->requestStart();
            if (result == oboe::Result::OK) {
                restart_requested_.store(false, std::memory_order_release);
                return true;
            }
            last_error_.store(static_cast<int32_t>(result), std::memory_order_release);
        }
        close_locked();
    }
    return open_and_start_locked();
}

void AndroidAudioOutput::pause() {
    desired_running_.store(false, std::memory_order_release);
    const std::lock_guard<std::mutex> lock(mutex_);
    if (stream_ == nullptr) {
        return;
    }
    const oboe::StreamState state = stream_->getState();
    if (state == oboe::StreamState::Starting || state == oboe::StreamState::Started) {
        const oboe::Result result = stream_->requestPause();
        if (result != oboe::Result::OK) {
            last_error_.store(static_cast<int32_t>(result), std::memory_order_release);
        }
    }
}

void AndroidAudioOutput::resume() {
    start();
}

void AndroidAudioOutput::shutdown() {
    desired_running_.store(false, std::memory_order_release);
    restart_requested_.store(false, std::memory_order_release);
    const std::lock_guard<std::mutex> lock(mutex_);
    close_locked();
}

void AndroidAudioOutput::service() {
    AudioBackend().collect_finished_channels();
    if (!desired_running_.load(std::memory_order_acquire) ||
        !restart_requested_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    const std::lock_guard<std::mutex> lock(mutex_);
    close_locked();
    open_and_start_locked();
}

bool AndroidAudioOutput::is_open() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return stream_ != nullptr && stream_->getState() != oboe::StreamState::Closed;
}

bool AndroidAudioOutput::is_started() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (stream_ == nullptr) {
        return false;
    }
    const oboe::StreamState state = stream_->getState();
    return state == oboe::StreamState::Starting || state == oboe::StreamState::Started;
}

int AndroidAudioOutput::sample_rate() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return stream_ == nullptr ? 0 : stream_->getSampleRate();
}

int AndroidAudioOutput::channel_count() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return stream_ == nullptr ? 0 : stream_->getChannelCount();
}

uint64_t AndroidAudioOutput::callback_count() const {
    return callback_count_.load(std::memory_order_relaxed);
}

uint64_t AndroidAudioOutput::rendered_frame_count() const {
    return rendered_frame_count_.load(std::memory_order_relaxed);
}

std::string AndroidAudioOutput::diagnostic_report() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream report;
    report << "audio_device=" << (stream_ != nullptr ? "open" : "closed")
           << "; desired_running="
           << (desired_running_.load(std::memory_order_acquire) ? "true" : "false")
           << "; restart_pending="
           << (restart_requested_.load(std::memory_order_acquire) ? "true" : "false")
           << "; callbacks=" << callback_count()
           << "; rendered_frames=" << rendered_frame_count()
           << "; last_error="
           << oboe::convertToText(static_cast<oboe::Result>(
                      last_error_.load(std::memory_order_acquire)));
    if (stream_ != nullptr) {
        report << "; api=" << ApiName(stream_->getAudioApi())
               << "; state=" << StateName(stream_->getState())
               << "; sample_rate=" << stream_->getSampleRate()
               << "; channels=" << stream_->getChannelCount()
               << "; frames_per_burst=" << stream_->getFramesPerBurst()
               << "; buffer_capacity=" << stream_->getBufferCapacityInFrames();
    }
    return report.str();
}

oboe::DataCallbackResult AndroidAudioOutput::onAudioReady(
        oboe::AudioStream* stream, void* audio_data, int32_t num_frames) {
    if (audio_data == nullptr || num_frames <= 0 || stream == nullptr) {
        return oboe::DataCallbackResult::Stop;
    }
    if (stream->getFormat() != oboe::AudioFormat::I16 ||
        stream->getChannelCount() != 2) {
        return oboe::DataCallbackResult::Stop;
    }

    AudioBackend().mix_interleaved_stereo(
            static_cast<int16_t*>(audio_data), static_cast<size_t>(num_frames));
    callback_count_.fetch_add(1, std::memory_order_relaxed);
    rendered_frame_count_.fetch_add(
            static_cast<uint64_t>(num_frames), std::memory_order_relaxed);
    return oboe::DataCallbackResult::Continue;
}

void AndroidAudioOutput::onErrorAfterClose(oboe::AudioStream* stream, oboe::Result error) {
    (void)stream;
    last_error_.store(static_cast<int32_t>(error), std::memory_order_release);
    restart_requested_.store(true, std::memory_order_release);
    PlatformRuntime::instance().log_warn(
            std::string("Android audio stream closed with error: ") + oboe::convertToText(error));
}

bool AndroidAudioOutput::open_and_start_locked() {
    oboe::AudioStreamBuilder builder;
    builder.setDirection(oboe::Direction::Output);
    builder.setPerformanceMode(oboe::PerformanceMode::LowLatency);
    builder.setSharingMode(oboe::SharingMode::Exclusive);
    builder.setFormat(oboe::AudioFormat::I16);
    builder.setChannelCount(2);
    builder.setSampleRate(AudioBackend().mix_rate());
    builder.setFormatConversionAllowed(true);
    builder.setChannelConversionAllowed(true);
    builder.setSampleRateConversionQuality(oboe::SampleRateConversionQuality::Medium);
    builder.setDataCallback(this);
    builder.setErrorCallback(this);

    std::shared_ptr<oboe::AudioStream> new_stream;
    oboe::Result result = builder.openStream(new_stream);
    if (result != oboe::Result::OK || new_stream == nullptr) {
        last_error_.store(static_cast<int32_t>(result), std::memory_order_release);
        PlatformRuntime::instance().log_warn(
                std::string("Cannot open Android audio output: ") + oboe::convertToText(result));
        return false;
    }

    stream_ = std::move(new_stream);
    result = stream_->requestStart();
    if (result != oboe::Result::OK) {
        last_error_.store(static_cast<int32_t>(result), std::memory_order_release);
        PlatformRuntime::instance().log_warn(
                std::string("Cannot start Android audio output: ") + oboe::convertToText(result));
        close_locked();
        return false;
    }

    last_error_.store(static_cast<int32_t>(oboe::Result::OK), std::memory_order_release);
    restart_requested_.store(false, std::memory_order_release);
    std::ostringstream report;
    report << "Android audio output started; api=" << ApiName(stream_->getAudioApi())
           << "; sample_rate=" << stream_->getSampleRate()
           << "; channels=" << stream_->getChannelCount()
           << "; frames_per_burst=" << stream_->getFramesPerBurst();
    PlatformRuntime::instance().log_info(report.str());
    return true;
}

void AndroidAudioOutput::close_locked() {
    if (stream_ == nullptr) {
        return;
    }
    stream_->requestStop();
    stream_->close();
    stream_.reset();
}

AndroidAudioOutput& AudioOutput() {
    static AndroidAudioOutput output;
    return output;
}

std::string RunAudioDeviceProbe() {
    if (!AudioBackend().is_initialized()) {
        AudioBackend().init(44100, 96);
    }
    const bool started = AudioOutput().start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::string report = AudioOutput().diagnostic_report();
    report += std::string("; start_result=") + (started ? "true" : "false");
    PlatformRuntime::instance().log_info(report);
    return report;
}

}  // namespace bk2::android

extern "C" JNIEXPORT jstring JNICALL
Java_com_nival_blitzkrieg2_NativeBridge_runAudioDeviceProbe(JNIEnv* env, jclass) {
    const std::string report = bk2::android::RunAudioDeviceProbe();
    return env->NewStringUTF(report.c_str());
}
