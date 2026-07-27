#include "bk2_port_paths.h"

#include <jni.h>

#include <mutex>
#include <unistd.h>
#include <utility>

namespace bk2::android {
namespace {

std::mutex g_paths_mutex;
PortPaths g_paths;

bool IsReadableDataRoot(const std::string& root) {
    if (root.empty()) {
        return false;
    }
    return access((root + "/Data/types.xml").c_str(), R_OK) == 0 &&
           access((root + "/Data/index.bin").c_str(), R_OK) == 0;
}

}  // namespace

std::string PortPaths::data_root() const {
    const std::string internal_root =
            files_dir.empty() ? std::string() : files_dir + "/DataAndroid";
    const std::string external_root =
            external_files_dir.empty()
                    ? std::string()
                    : external_files_dir + "/DataAndroid";
    if (IsReadableDataRoot(internal_root)) {
        return internal_root;
    }
    if (IsReadableDataRoot(external_root)) {
        return external_root;
    }
    if (!internal_root.empty()) {
        return internal_root;
    }
    return external_root;
}

std::string PortPaths::save_root() const {
    return files_dir + "/Profiles";
}

std::string PortPaths::log_root() const {
    return no_backup_dir.empty() ? files_dir : no_backup_dir;
}

void SetPortPaths(PortPaths paths) {
    std::lock_guard<std::mutex> lock(g_paths_mutex);
    g_paths = std::move(paths);
}

PortPaths GetPortPaths() {
    std::lock_guard<std::mutex> lock(g_paths_mutex);
    return g_paths;
}

}  // namespace bk2::android

extern "C" JNIEXPORT void JNICALL
Java_com_nival_blitzkrieg2_NativeBridge_configurePaths(
    JNIEnv* env,
    jclass,
    jstring files_dir,
    jstring no_backup_dir,
    jstring external_files_dir) {
    auto to_string = [env](jstring value) -> std::string {
        if (value == nullptr) {
            return {};
        }
        const char* chars = env->GetStringUTFChars(value, nullptr);
        std::string out = chars == nullptr ? std::string() : std::string(chars);
        if (chars != nullptr) {
            env->ReleaseStringUTFChars(value, chars);
        }
        return out;
    };

    bk2::android::SetPortPaths({
        to_string(files_dir),
        to_string(no_backup_dir),
        to_string(external_files_dir),
    });
}
