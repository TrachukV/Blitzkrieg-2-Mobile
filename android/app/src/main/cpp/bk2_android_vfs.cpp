#include "System/stdafx.h"

#include "System/Streams.h"
#include "System/VFS.h"
#include "System/VFSOperations.h"

#include "bk2_android_vfs.h"
#include "bk2_port_paths.h"

#include <algorithm>
#include <cerrno>
#include <dirent.h>
#include <mutex>
#include <set>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

std::mutex g_vfs_mutex;
bool g_vfs_initialized = false;

std::string NormalizePath(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    while (path.size() >= 2 && path[1] == ':') {
        path.erase(0, 2);
    }
    while (!path.empty() && path[0] == '/') {
        path.erase(0, 1);
    }
    return path;
}

std::string JoinPath(const std::string& left, const std::string& right) {
    if (left.empty()) {
        return right;
    }
    if (right.empty()) {
        return left;
    }
    return left[left.size() - 1] == '/' ? left + right : left + "/" + right;
}

bool IsRegularFile(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool IsDirectory(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool EnsureDirectory(const std::string& directory) {
    if (directory.empty()) {
        return true;
    }

    std::string current;
    size_t position = 0;
    if (directory[0] == '/') {
        current = "/";
        position = 1;
    }

    while (position <= directory.size()) {
        const size_t slash = directory.find('/', position);
        const std::string part = directory.substr(position, slash - position);
        if (!part.empty()) {
            current = current == "/" ? current + part : JoinPath(current, part);
            if (mkdir(current.c_str(), 0775) != 0 && errno != EEXIST) {
                return false;
            }
        }
        if (slash == std::string::npos) {
            break;
        }
        position = slash + 1;
    }
    return true;
}

bool EnsureParentDirectory(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos || EnsureDirectory(path.substr(0, slash));
}

std::string ToLegacyPath(std::string path) {
    std::replace(path.begin(), path.end(), '/', '\\');
    return path;
}

std::vector<std::string> BuildRoots() {
    const bk2::android::PortPaths paths = bk2::android::GetPortPaths();
    std::vector<std::string> roots;
    if (!paths.data_root().empty()) {
        roots.push_back(JoinPath(paths.data_root(), "Overlay"));
        roots.push_back(JoinPath(paths.data_root(), "Overlay/Data"));
        roots.push_back(paths.data_root());
        roots.push_back(JoinPath(paths.data_root(), "Data"));
    }
    if (!paths.external_files_dir.empty()) {
        roots.push_back(paths.external_files_dir);
    }
    if (!paths.files_dir.empty()) {
        roots.push_back(paths.files_dir);
    }
    if (!paths.save_root().empty()) {
        roots.push_back(paths.save_root());
    }
    roots.push_back(".");
    return roots;
}

std::vector<std::string> BuildCandidates(const std::string& path) {
    const std::string normalized = NormalizePath(path);
    std::vector<std::string> candidates;
    if (!path.empty() && path[0] == '/') {
        candidates.push_back(path);
    }
    for (const std::string& root : BuildRoots()) {
        candidates.push_back(JoinPath(root, normalized));
    }
    candidates.push_back(normalized);
    return candidates;
}

std::string ResolveExistingFile(const std::string& path) {
    const std::vector<std::string> candidates = BuildCandidates(path);
    for (const std::string& candidate : candidates) {
        if (IsRegularFile(candidate)) {
            return candidate;
        }
    }
    return {};
}

std::string ResolveWriteFile(const std::string& path) {
    const bk2::android::PortPaths paths = bk2::android::GetPortPaths();
    const std::string normalized = NormalizePath(path);
    if (normalized == "Profiles" || normalized.compare(0, 9, "Profiles/") == 0) {
        if (!paths.files_dir.empty()) {
            return JoinPath(paths.files_dir, normalized);
        }
    }
    if (!paths.save_root().empty()) {
        return JoinPath(paths.save_root(), normalized);
    }
    if (!paths.files_dir.empty()) {
        return JoinPath(paths.files_dir, normalized);
    }
    return normalized;
}

void CollectFiles(
        const std::string& absoluteRoot,
        const std::string& absoluteDir,
        const std::string& relativePrefix,
        std::set<std::string>* out) {
    DIR* dir = opendir(absoluteDir.c_str());
    if (dir == nullptr) {
        return;
    }

    while (dirent* entry = readdir(dir)) {
        const char* name = entry->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
            continue;
        }

        const std::string absolutePath = JoinPath(absoluteDir, name);
        const std::string relativePath =
                relativePrefix.empty() ? std::string(name) : JoinPath(relativePrefix, name);
        if (IsDirectory(absolutePath)) {
            CollectFiles(absoluteRoot, absolutePath, relativePath, out);
        } else if (IsRegularFile(absolutePath)) {
            out->insert(ToLegacyPath(relativePath));
        }
    }

    closedir(dir);
}

class CAndroidVFS : public NVFS::IVFS {
    OBJECT_NOCOPY_METHODS(CAndroidVFS)
public:
    CAndroidVFS() {}

    CDataStream* OpenFile(const string& path) override {
        const std::string resolved = ResolveExistingFile(path.c_str());
        if (resolved.empty()) {
            return 0;
        }
        CFileStream* stream = new CFileStream(resolved.c_str(), CFileStream::WIN_READ_ONLY);
        if (!stream->IsOk()) {
            delete stream;
            return 0;
        }
        return stream;
    }

    bool DoesFileExist(const string& path) override {
        return !ResolveExistingFile(path.c_str()).empty();
    }

    bool GetFileStats(NVFS::SFileStats* stats, const string& path) override {
        if (stats == 0) {
            return false;
        }
        const std::string resolved = ResolveExistingFile(path.c_str());
        if (resolved.empty()) {
            return false;
        }
        struct stat st;
        if (stat(resolved.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
            return false;
        }
        stats->pszName = 0;
        stats->nSize = static_cast<int>(st.st_size);
        stats->dwAccess = 0;
        stats->ctime = DOSToWin32DateTime(st.st_ctime);
        stats->mtime = DOSToWin32DateTime(st.st_mtime);
        stats->atime = DOSToWin32DateTime(st.st_atime);
        return true;
    }

    void GetAllFileNames(vector<string>* fileNames, const string& folder) override {
        if (fileNames == 0) {
            return;
        }
        std::set<std::string> found;
        const std::string normalizedFolder = NormalizePath(folder.c_str());
        for (const std::string& root : BuildRoots()) {
            const std::string absoluteDir = JoinPath(root, normalizedFolder);
            if (IsDirectory(absoluteDir)) {
                CollectFiles(root, absoluteDir, normalizedFolder, &found);
            }
        }
        fileNames->clear();
        fileNames->reserve(found.size());
        for (const std::string& item : found) {
            fileNames->push_back(item.c_str());
        }
    }
};

class CAndroidFileCreator : public NVFS::IFileCreator {
    OBJECT_NOCOPY_METHODS(CAndroidFileCreator)
public:
    CAndroidFileCreator() {}

    CDataStream* CreateFile(const string& path) override {
        const std::string resolved = ResolveWriteFile(path.c_str());
        if (!EnsureParentDirectory(resolved)) {
            return 0;
        }
        CFileStream* stream = new CFileStream(resolved.c_str(), CFileStream::WIN_CREATE);
        if (!stream->IsOk()) {
            delete stream;
            return 0;
        }
        return stream;
    }

    bool RemoveFile(const string& path) override {
        const std::string resolved = ResolveWriteFile(path.c_str());
        return unlink(resolved.c_str()) == 0 || errno == ENOENT;
    }
};

}  // namespace

namespace bk2::android {

bool InitializeLegacyVfs() {
    std::lock_guard<std::mutex> lock(g_vfs_mutex);
    if (g_vfs_initialized && NVFS::GetMainVFS() != 0 && NVFS::GetMainFileCreator() != 0) {
        return true;
    }

    NVFS::SetMainVFS(new CAndroidVFS());
    NVFS::SetMainFileCreator(new CAndroidFileCreator());
    g_vfs_initialized = NVFS::GetMainVFS() != 0 && NVFS::GetMainFileCreator() != 0;
    return g_vfs_initialized;
}

void ShutdownLegacyVfs() {
    std::lock_guard<std::mutex> lock(g_vfs_mutex);
    if (!g_vfs_initialized) {
        return;
    }

    NVFS::SetMainFileCreator(0);
    NVFS::SetMainVFS(0);
    g_vfs_initialized = false;
}

bool IsLegacyVfsInitialized() {
    std::lock_guard<std::mutex> lock(g_vfs_mutex);
    return g_vfs_initialized && NVFS::GetMainVFS() != 0 && NVFS::GetMainFileCreator() != 0;
}

}  // namespace bk2::android
