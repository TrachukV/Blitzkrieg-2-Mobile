#include "System/stdafx.h"

#include "System/Streams.h"
#include "System/VFS.h"

#include "bk2_port_paths.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

std::string NormalizeLegacyPath(const std::string& path) {
    std::string normalized = path;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    while (normalized.size() >= 2 && normalized[1] == ':') {
        normalized.erase(0, 2);
    }
    while (!normalized.empty() && normalized[0] == '/') {
        normalized.erase(0, 1);
    }
    return normalized;
}

bool IsAbsolutePath(const std::string& path) {
    return !path.empty() && path[0] == '/';
}

bool StartsWithPath(const std::string& path, const char* prefix) {
    const std::string needle(prefix);
    return path == needle || path.compare(0, needle.size() + 1, needle + "/") == 0;
}

std::string JoinPath(const std::string& left, const std::string& right) {
    if (left.empty()) {
        return right;
    }
    if (right.empty()) {
        return left;
    }
    if (left[left.size() - 1] == '/') {
        return left + right;
    }
    return left + "/" + right;
}

bool FileExists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::vector<std::string> BuildReadCandidates(const std::string& requested) {
    const std::string normalized = NormalizeLegacyPath(requested);
    if (IsAbsolutePath(requested)) {
        return {requested};
    }

    const bk2::android::PortPaths paths = bk2::android::GetPortPaths();
    std::vector<std::string> candidates;
    if (!paths.external_files_dir.empty()) {
        candidates.push_back(JoinPath(paths.external_files_dir, normalized));
    }
    if (!paths.files_dir.empty()) {
        candidates.push_back(JoinPath(paths.files_dir, normalized));
    }
    if (!paths.data_root().empty()) {
        candidates.push_back(JoinPath(paths.data_root(), normalized));
    }
    if (StartsWithPath(normalized, "Profiles") && !paths.files_dir.empty()) {
        candidates.push_back(JoinPath(paths.files_dir, normalized));
    } else if (!paths.save_root().empty()) {
        candidates.push_back(JoinPath(paths.save_root(), normalized));
    }
    candidates.push_back(normalized);
    return candidates;
}

std::string BuildWritePath(const std::string& requested) {
    const std::string normalized = NormalizeLegacyPath(requested);
    if (IsAbsolutePath(requested)) {
        return requested;
    }

    const bk2::android::PortPaths paths = bk2::android::GetPortPaths();
    if (StartsWithPath(normalized, "Profiles") && !paths.files_dir.empty()) {
        return JoinPath(paths.files_dir, normalized);
    }
    if (!paths.save_root().empty()) {
        return JoinPath(paths.save_root(), normalized);
    }
    if (!paths.files_dir.empty()) {
        return JoinPath(paths.files_dir, normalized);
    }
    return normalized;
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
    if (slash == std::string::npos) {
        return true;
    }
    return EnsureDirectory(path.substr(0, slash));
}

class CAndroidFileBackedStream : public CMemoryStream {
    std::string fileName;
    bool writeOnFlush;

public:
    CAndroidFileBackedStream(const std::string& path, bool writeMode)
        : fileName(path), writeOnFlush(writeMode) {
        if (writeMode) {
            SetSizeDiscard(0);
            if (!EnsureParentDirectory(fileName)) {
                SetBroken();
            }
            return;
        }

        FILE* file = fopen(fileName.c_str(), "rb");
        if (file == 0) {
            SetBroken();
            return;
        }

        if (fseek(file, 0, SEEK_END) != 0) {
            fclose(file);
            SetBroken();
            return;
        }
        const long fileSize = ftell(file);
        if (fileSize < 0 || fseek(file, 0, SEEK_SET) != 0) {
            fclose(file);
            SetBroken();
            return;
        }

        SetSizeDiscard(static_cast<int>(fileSize));
        if (fileSize > 0) {
            const size_t read = fread(GetBufferForWrite(), 1, static_cast<size_t>(fileSize), file);
            if (read != static_cast<size_t>(fileSize)) {
                SetBroken();
            }
        }
        fclose(file);
        Seek(0);
    }

    ~CAndroidFileBackedStream() override {
        Flush();
    }

    void Flush() override {
        if (!writeOnFlush || !IsOk()) {
            return;
        }
        if (!EnsureParentDirectory(fileName)) {
            SetBroken();
            return;
        }
        FILE* file = fopen(fileName.c_str(), "wb");
        if (file == 0) {
            SetBroken();
            return;
        }
        const int size = GetSize();
        if (size > 0) {
            const size_t written = fwrite(GetBuffer(), 1, static_cast<size_t>(size), file);
            if (written != static_cast<size_t>(size)) {
                SetBroken();
            }
        }
        if (fclose(file) != 0) {
            SetBroken();
        }
    }
};

}  // namespace

void CDataStream::SetBuffer(unsigned char* buffer, int bufferSize, int position, int size, int flags) {
    data.pBuffer = buffer;
    data.pBufferEnd = data.pBuffer + bufferSize;
    data.pCurrent = data.pBuffer + position;
    data.pFileEnd = data.pBuffer + size;
    data.nFlags = flags;
}

bool CDataStream::FixupBuf(int oldSize) {
    if ((data.nFlags & F_Broken) || !CanWrite()) {
        unsigned char* oldFileEnd = data.pBuffer + oldSize;
        data.pCurrent = oldFileEnd;
        data.pFileEnd = oldFileEnd;
        SetBroken();
        return false;
    }

    const int currentSize = data.pCurrent - data.pBuffer;
    int newBufferSize = Min(currentSize + 65536 * 16, currentSize * 2);
    newBufferSize = Max(newBufferSize, 4096);
    newBufferSize = (newBufferSize + 4095) & ~4095;
    AllocBuf(oldSize, newBufferSize);
    return IsOk();
}

void CDataStream::ReadOverflow(void* destination, int size) {
    if (data.pCurrent != data.pFileEnd) {
        const int remaining = data.pFileEnd - data.pCurrent;
        Read(destination, remaining);
        Read(static_cast<char*>(destination) + remaining, size - remaining);
    } else {
        SetBroken();
        memset(destination, 0, size);
    }
}

void CDataStream::ReadString(string& result, int maxSize) {
    int size = 0;
    Read(&size, 1);
    if (size & 1) {
        Read(reinterpret_cast<char*>(&size) + 1, 3);
    }
    size >>= 1;

    if ((maxSize > 0 && size > maxSize) || data.pCurrent + size > data.pFileEnd) {
        result = "";
        data.pCurrent = data.pFileEnd;
        SetBroken();
        return;
    }

    result.assign(reinterpret_cast<const char*>(data.pCurrent), size);
    data.pCurrent += size;
}

void CDataStream::WriteString(const string& value) {
    int size = value.size();
    int encodedSize = size >= 128 ? size * 2 + 1 : size * 2;
    Write(&encodedSize, size >= 128 ? 4 : 1);
    Write(value.data(), size);
}

void CDataStream::ReadTo(CDataStream* destination, unsigned int size) {
    destination->SetSize(0);
    destination->SetSize(size);
    Read(destination->GetBufferForWrite(), size);
    destination->Seek(0);
}

void CDataStream::WriteFrom(CDataStream& source) {
    source.Seek(0);
    Write(source.GetBuffer(), source.GetSize());
}

void CMemoryStream::AllocBuf(int oldFileSize, int size) {
    unsigned char* buffer = new unsigned char[size];
    unsigned char* previous = GetBufferPtr();
    const int oldPosition = GetPosition();
    const int oldSize = GetSize();
    const int transfer = Min(size, oldFileSize);
    if (transfer != 0 && buffer != 0 && previous != 0) {
        memcpy(buffer, previous, transfer);
    }
    delete[] previous;

    if (buffer == 0) {
        SetBuffer(0, 0, 0, 0, GetFlags() | F_Broken);
    } else {
        SetBuffer(buffer, size, Min(oldPosition, size), Min(oldSize, size), GetFlags());
    }
}

CMemoryStream::~CMemoryStream() {
    delete[] GetBufferPtr();
}

CMemoryStream::CMemoryStream(const CMemoryStream& source) : CDataStream(0) {
    CopyMemoryStream(source);
}

CMemoryStream& CMemoryStream::operator=(const CMemoryStream& source) {
    if (this != &source) {
        delete[] GetBufferPtr();
        CopyMemoryStream(source);
    }
    return *this;
}

void CMemoryStream::SetSizeDiscard(int size) {
    delete[] GetBufferPtr();
    unsigned char* buffer = new unsigned char[size];
    SetBuffer(buffer, size, 0, size, F_CanRW);
}

void CMemoryStream::CopyMemoryStream(const CMemoryStream& source) {
    const int bufferSize = source.GetBufferSize();
    unsigned char* buffer = new unsigned char[bufferSize];
    SetBuffer(buffer, bufferSize, source.GetPosition(), source.GetSize(), source.GetFlags());
    if (bufferSize != 0) {
        memcpy(GetBufferPtr(), source.GetBufferPtr(), bufferSize);
    }
}

CFileStream::CFileStream(NVFS::IVFS* vfs, const string& fileName)
    : CDataStream(F_Broken), pStream(0) {
    if (vfs != 0) {
        pStream = vfs->OpenFile(fileName);
        if (pStream != 0) {
            SyncWith(*pStream);
        }
    }
}

CFileStream::CFileStream(NVFS::IFileCreator* fileCreator, const string& fileName)
    : CDataStream(F_Broken), pStream(0) {
    if (fileCreator != 0) {
        pStream = fileCreator->CreateFile(fileName);
        if (pStream != 0) {
            SyncWith(*pStream);
        }
    }
}

CFileStream::CFileStream(const string& fileName, const EWinMode mode)
    : CDataStream(F_Broken), pStream(0) {
    if (mode == WIN_READ_ONLY) {
        const std::vector<std::string> candidates = BuildReadCandidates(fileName.c_str());
        for (size_t i = 0; i < candidates.size(); ++i) {
            if (FileExists(candidates[i])) {
                pStream = new CAndroidFileBackedStream(candidates[i], false);
                break;
            }
        }
        if (pStream == 0) {
            pStream = new CAndroidFileBackedStream(fileName.c_str(), false);
        }
    } else {
        pStream = new CAndroidFileBackedStream(BuildWritePath(fileName.c_str()), true);
    }

    if (pStream != 0) {
        SyncWith(*pStream);
    }
}

CFileStream::~CFileStream() {
    if (pStream != 0) {
        pStream->SyncWith(*this);
        delete pStream;
    }
}

void CFileStream::AllocBuf(int oldFileSize, int size) {
    if (pStream != 0) {
        pStream->SyncWith(*this);
        pStream->AllocBuf(oldFileSize, size);
        SyncWith(*pStream);
    }
}

void CFileStream::Flush() {
    if (pStream != 0) {
        pStream->SyncWith(*this);
        pStream->Flush();
        SyncWith(*pStream);
    }
}
