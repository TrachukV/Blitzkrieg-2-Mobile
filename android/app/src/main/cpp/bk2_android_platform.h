#pragma once

#include <atomic>
#include <cstdint>
#include <string_view>

namespace bk2::android {

enum class LifecycleState {
    Started,
    WindowReady,
    Focused,
    Paused,
    Stopped,
    Destroying,
};

class PlatformRuntime final {
public:
    static PlatformRuntime& instance();

    void set_lifecycle_state(LifecycleState state);
    LifecycleState lifecycle_state() const;

    uint64_t monotonic_millis() const;
    uint64_t monotonic_micros() const;
    void sleep_millis(uint32_t millis) const;
    void sleep_micros(uint64_t micros) const;

    void log_info(std::string_view message) const;
    void log_warn(std::string_view message) const;
    void show_message(std::string_view title, std::string_view message) const;

private:
    std::atomic<int> lifecycle_state_{static_cast<int>(LifecycleState::Started)};
};

}  // namespace bk2::android
