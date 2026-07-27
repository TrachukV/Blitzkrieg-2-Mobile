#include "bk2_android_video_bridge.h"

#include "bk2_android_platform.h"
#include "bk2_port_paths.h"

#include "System/stdafx.h"
#include "System/Streams.h"
#include "System/VFS.h"
#include "System/VFSOperations.h"

#include <jni.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>
#include <vector>

namespace bk2::android {
namespace {

JavaVM* g_java_vm = nullptr;
jclass g_bridge_class = nullptr;
jmethodID g_play_method = nullptr;
jmethodID g_play_sequence_method = nullptr;

std::string JoinPath(const std::string& left, const std::string& right) {
    if (left.empty()) {
        return right;
    }
    if (right.empty()) {
        return left;
    }
    return left[left.size() - 1] == '/' ? left + right : left + "/" + right;
}

std::string LowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch);
    });
    return value;
}

bool EndsWithNoCase(const std::string& value, const char* suffix) {
    const std::string lower_value = LowerAscii(value);
    const std::string lower_suffix = LowerAscii(suffix);
    return lower_value.size() >= lower_suffix.size() &&
           lower_value.compare(lower_value.size() - lower_suffix.size(), lower_suffix.size(), lower_suffix) == 0;
}

bool HasKnownVideoExtension(const std::string& value) {
    return EndsWithNoCase(value, ".bik") || EndsWithNoCase(value, ".mp4") ||
           EndsWithNoCase(value, ".webm");
}

std::string Trim(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.erase(value.begin());
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.pop_back();
    }
    return value;
}

std::string NormalizeLegacyRef(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    const size_t xpointer = path.find('#');
    if (xpointer != std::string::npos) {
        path.resize(xpointer);
    }
    while (!path.empty() && path[0] == '/') {
        path.erase(0, 1);
    }
    return path;
}

std::string ToSlashPath(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    return path;
}

bool VfsFileExists(NVFS::IVFS* vfs, const std::string& ref) {
    if (vfs == nullptr || ref.empty()) {
        return false;
    }
    return vfs->DoesFileExist(ref.c_str());
}

std::string ParentPath(const std::string& ref) {
    const size_t slash = ref.find_last_of('/');
    return slash == std::string::npos ? std::string() : ref.substr(0, slash);
}

std::string CanonicalExistingVfsRef(std::string ref) {
    NVFS::IVFS* vfs = NVFS::GetMainVFS();
    ref = ToSlashPath(ref);
    if (VfsFileExists(vfs, ref)) {
        return ref;
    }

    const std::string folder = ParentPath(ref);
    if (vfs == nullptr || folder.empty()) {
        return ref;
    }

    vector<string> files;
    vfs->GetAllFileNames(&files, folder.c_str());
    const std::string wanted = LowerAscii(ref);
    for (vector<string>::const_iterator it = files.begin(); it != files.end(); ++it) {
        const std::string candidate = ToSlashPath(it->c_str());
        if (LowerAscii(candidate) == wanted) {
            return candidate;
        }
    }
    return ref;
}

std::string AndroidVideoRefForMovieFile(std::string source_ref) {
    std::string normalized = NormalizeLegacyRef(std::move(source_ref));
    if (normalized.empty()) {
        return {};
    }
    if (EndsWithNoCase(normalized, ".xml")) {
        return {};
    }
    if (EndsWithNoCase(normalized, ".mp4") || EndsWithNoCase(normalized, ".webm")) {
        return CanonicalExistingVfsRef(normalized);
    }
    if (EndsWithNoCase(normalized, ".bik")) {
        normalized.resize(normalized.size() - 4);
    } else if (!HasKnownVideoExtension(normalized)) {
        // Movie sequence XML uses extension-less FileName entries.
    } else {
        return {};
    }
    normalized += ".mp4";
    const std::string lower = LowerAscii(normalized);
    if (lower.find("movies/") != 0 && lower.find("ui/") != 0) {
        normalized = "Movies/" + normalized;
    }
    return CanonicalExistingVfsRef(normalized);
}

std::string ReadVfsTextFile(std::string ref) {
    NVFS::IVFS* vfs = NVFS::GetMainVFS();
    if (vfs == nullptr) {
        return {};
    }

    CDataStream* stream = vfs->OpenFile(NormalizeLegacyRef(std::move(ref)).c_str());
    if (stream == nullptr) {
        return {};
    }

    std::string text;
    if (stream->IsOk() && stream->CanRead() && stream->GetSize() > 0 && stream->GetBuffer() != nullptr) {
        text.assign(
                reinterpret_cast<const char*>(stream->GetBuffer()),
                static_cast<size_t>(stream->GetSize()));
    }
    delete stream;
    return text;
}

std::vector<std::string> ExtractMovieFileNamesFromXml(const std::string& xml) {
    std::vector<std::string> refs;
    const std::string lower = LowerAscii(xml);
    size_t search_from = 0;
    while (true) {
        const size_t open = lower.find("<filename>", search_from);
        if (open == std::string::npos) {
            break;
        }
        const size_t value_start = open + 10;
        const size_t close = lower.find("</filename>", value_start);
        if (close == std::string::npos) {
            break;
        }
        std::string value = Trim(xml.substr(value_start, close - value_start));
        if (!value.empty()) {
            refs.push_back(value);
        }
        search_from = close + 11;
    }
    return refs;
}

JNIEnv* GetEnv(bool* did_attach) {
    *did_attach = false;
    if (g_java_vm == nullptr) {
        return nullptr;
    }

    JNIEnv* env = nullptr;
    const jint get_env_result = g_java_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (get_env_result == JNI_OK) {
        return env;
    }
    if (get_env_result != JNI_EDETACHED) {
        return nullptr;
    }
    if (g_java_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
        return nullptr;
    }
    *did_attach = true;
    return env;
}

}  // namespace

std::string AndroidVideoRefForLegacyBink(std::string_view source_ref) {
    std::string normalized = NormalizeLegacyRef(std::string(source_ref));
    if (!EndsWithNoCase(normalized, ".bik")) {
        return {};
    }
    return AndroidVideoRefForMovieFile(std::move(normalized));
}

std::vector<std::string> AndroidVideoRefsForLegacyMovie(std::string_view source_ref) {
    const std::string normalized = NormalizeLegacyRef(std::string(source_ref));
    if (normalized.empty()) {
        return {};
    }

    if (!EndsWithNoCase(normalized, ".xml")) {
        const std::string ref = AndroidVideoRefForMovieFile(normalized);
        return ref.empty() ? std::vector<std::string>() : std::vector<std::string>(1, ref);
    }

    std::vector<std::string> refs;
    const std::vector<std::string> movie_files = ExtractMovieFileNamesFromXml(ReadVfsTextFile(normalized));
    for (std::vector<std::string>::const_iterator it = movie_files.begin(); it != movie_files.end(); ++it) {
        const std::string ref = AndroidVideoRefForMovieFile(*it);
        if (!ref.empty()) {
            refs.push_back(ref);
        }
    }
    return refs;
}

std::string AndroidVideoPathForLegacyBink(std::string_view source_ref) {
    const std::string ref = AndroidVideoRefForLegacyBink(source_ref);
    return ref.empty() ? std::string() : JoinPath(GetPortPaths().data_root(), ref);
}

std::vector<std::string> AndroidVideoPathsForLegacyMovie(std::string_view source_ref) {
    const std::vector<std::string> refs = AndroidVideoRefsForLegacyMovie(source_ref);
    std::vector<std::string> paths;
    paths.reserve(refs.size());
    for (std::vector<std::string>::const_iterator it = refs.begin(); it != refs.end(); ++it) {
        paths.push_back(JoinPath(GetPortPaths().data_root(), *it));
    }
    return paths;
}

void RequestFullscreenVideo(std::string_view video_path) {
    RequestFullscreenVideos(std::vector<std::string>(1, std::string(video_path)));
}

void RequestFullscreenVideos(const std::vector<std::string>& video_paths) {
    bool did_attach = false;
    JNIEnv* env = GetEnv(&did_attach);
    if (env == nullptr) {
        PlatformRuntime::instance().log_warn("Video bridge skipped: JNI environment is not available.");
        return;
    }

    if (g_bridge_class == nullptr || g_play_sequence_method == nullptr) {
        PlatformRuntime::instance().log_warn("Video bridge skipped: Java bridge is not initialized.");
        if (did_attach) {
            g_java_vm->DetachCurrentThread();
        }
        return;
    }

    jclass string_class = env->FindClass("java/lang/String");
    if (string_class == nullptr) {
        if (did_attach) {
            g_java_vm->DetachCurrentThread();
        }
        return;
    }

    jobjectArray java_paths = env->NewObjectArray(
            static_cast<jsize>(video_paths.size()),
            string_class,
            nullptr);
    env->DeleteLocalRef(string_class);
    if (java_paths == nullptr) {
        if (did_attach) {
            g_java_vm->DetachCurrentThread();
        }
        return;
    }

    for (size_t i = 0; i < video_paths.size(); ++i) {
        jstring java_path = env->NewStringUTF(video_paths[i].c_str());
        env->SetObjectArrayElement(java_paths, static_cast<jsize>(i), java_path);
        env->DeleteLocalRef(java_path);
    }
    env->CallStaticVoidMethod(g_bridge_class, g_play_sequence_method, java_paths);
    env->DeleteLocalRef(java_paths);

    if (did_attach) {
        g_java_vm->DetachCurrentThread();
    }
}

void RequestFullscreenVideoForLegacyBink(std::string_view source_ref) {
    const std::string path = AndroidVideoPathForLegacyBink(source_ref);
    if (path.empty()) {
        PlatformRuntime::instance().log_warn("Video bridge skipped: unsupported legacy video ref.");
        return;
    }
    RequestFullscreenVideo(path);
}

void RequestFullscreenVideoForLegacyMovie(std::string_view source_ref) {
    const std::vector<std::string> paths = AndroidVideoPathsForLegacyMovie(source_ref);
    if (paths.empty()) {
        PlatformRuntime::instance().log_warn("Video bridge skipped: unsupported legacy movie ref.");
        return;
    }
    RequestFullscreenVideos(paths);
}

void SetJavaVm(JavaVM* vm) {
    g_java_vm = vm;
}

bool CacheVideoBridge(JNIEnv* env) {
    jclass local_bridge_class = env->FindClass("com/nival/blitzkrieg2/NativeBridge");
    if (local_bridge_class == nullptr) {
        return false;
    }
    g_bridge_class = static_cast<jclass>(env->NewGlobalRef(local_bridge_class));
    env->DeleteLocalRef(local_bridge_class);
    if (g_bridge_class == nullptr) {
        return false;
    }

    g_play_method = env->GetStaticMethodID(g_bridge_class, "playFullscreenVideo", "(Ljava/lang/String;)V");
    g_play_sequence_method = env->GetStaticMethodID(g_bridge_class, "playFullscreenVideos", "([Ljava/lang/String;)V");
    return g_play_method != nullptr && g_play_sequence_method != nullptr;
}

}  // namespace bk2::android

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
    bk2::android::SetJavaVm(vm);
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK || env == nullptr) {
        return JNI_ERR;
    }
    if (!bk2::android::CacheVideoBridge(env)) {
        return JNI_ERR;
    }
    return JNI_VERSION_1_6;
}
