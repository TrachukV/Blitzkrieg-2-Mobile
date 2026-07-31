#include "bk2_android_platform.h"

#include <android/log.h>

#include <chrono>
#include <thread>

namespace bk2::android {
namespace {

constexpr const char* kLogTag = "Blitzkrieg2";

void Log(android_LogPriority priority, std::string_view message) {
    __android_log_print(priority, kLogTag, "%.*s", static_cast<int>(message.size()), message.data());
}

}  // namespace

PlatformRuntime& PlatformRuntime::instance() {
    static PlatformRuntime runtime;
    return runtime;
}

void PlatformRuntime::set_lifecycle_state(LifecycleState state) {
    lifecycle_state_.store(static_cast<int>(state), std::memory_order_relaxed);
}

LifecycleState PlatformRuntime::lifecycle_state() const {
    return static_cast<LifecycleState>(lifecycle_state_.load(std::memory_order_relaxed));
}

uint64_t PlatformRuntime::monotonic_millis() const {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

uint64_t PlatformRuntime::monotonic_micros() const {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

void PlatformRuntime::sleep_millis(uint32_t millis) const {
    std::this_thread::sleep_for(std::chrono::milliseconds(millis));
}

void PlatformRuntime::sleep_micros(uint64_t micros) const {
    if (micros == 0) {
        return;
    }
    std::this_thread::sleep_for(std::chrono::microseconds(micros));
}

void PlatformRuntime::log_info(std::string_view message) const {
    Log(ANDROID_LOG_INFO, message);
}

void PlatformRuntime::log_warn(std::string_view message) const {
    Log(ANDROID_LOG_WARN, message);
}

void PlatformRuntime::show_message(std::string_view title, std::string_view message) const {
    Log(ANDROID_LOG_WARN, title);
    Log(ANDROID_LOG_WARN, message);
}

}  // namespace bk2::android
