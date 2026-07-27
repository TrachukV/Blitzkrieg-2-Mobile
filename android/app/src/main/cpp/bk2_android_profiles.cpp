#include "Main/stdafx.h"

#include "Main/Profiles.h"
#include "Misc/StrProc.h"

#include "bk2_port_paths.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

const char* kLegacyProfileRoot = "Profiles\\";
const char* kDefaultProfileDirName = "default_profile";

std::string JoinPath(const std::string& left, const std::string& right) {
    if (left.empty()) {
        return right;
    }
    if (right.empty()) {
        return left;
    }
    return left[left.size() - 1] == '/' ? left + right : left + "/" + right;
}

std::string NormalizeSeparators(std::string path) {
    for (std::string::iterator it = path.begin(); it != path.end(); ++it) {
        if (*it == '\\') {
            *it = '/';
        }
    }
    return path;
}

std::string AbsoluteProfilesRoot() {
    const bk2::android::PortPaths paths = bk2::android::GetPortPaths();
    if (!paths.files_dir.empty()) {
        return JoinPath(paths.files_dir, "Profiles");
    }
    return "Profiles";
}

std::string AbsoluteFromLegacyProfilePath(const std::string& legacyPath) {
    std::string normalized = NormalizeSeparators(legacyPath);
    while (!normalized.empty() && normalized[0] == '/') {
        normalized.erase(0, 1);
    }
    const bk2::android::PortPaths paths = bk2::android::GetPortPaths();
    if (!paths.files_dir.empty()) {
        return JoinPath(paths.files_dir, normalized);
    }
    return normalized;
}

bool IsDirectory(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool IsRegularFile(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool EnsureDirectory(const std::string& directory) {
    if (directory.empty() || IsDirectory(directory)) {
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

bool RemoveTree(const std::string& path) {
    DIR* dir = opendir(path.c_str());
    if (dir == 0) {
        return unlink(path.c_str()) == 0 || errno == ENOENT;
    }

    while (dirent* entry = readdir(dir)) {
        const char* name = entry->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
            continue;
        }
        const std::string child = JoinPath(path, name);
        if (IsDirectory(child)) {
            RemoveTree(child);
        } else {
            unlink(child.c_str());
        }
    }
    closedir(dir);
    return rmdir(path.c_str()) == 0 || errno == ENOENT;
}

bool IsDefaultProfileName(const std::string& name) {
    std::string lowered = name;
    for (std::string::iterator it = lowered.begin(); it != lowered.end(); ++it) {
        if (*it >= 'A' && *it <= 'Z') {
            *it = static_cast<char>(*it - 'A' + 'a');
        }
    }
    return lowered == kDefaultProfileDirName;
}

bool IsPortableProfileDirName(const std::string& name) {
    if (name.empty() || name.size() > 128 || IsDefaultProfileName(name)) {
        return false;
    }
    for (std::string::const_iterator it = name.begin(); it != name.end(); ++it) {
        const unsigned char ch = static_cast<unsigned char>(*it);
        if (ch < 33 || ch == '<' || ch == '>' || ch == ':' || ch == '"' ||
            ch == '/' || ch == '\\' || ch == '|' || ch == '*' || ch == '?') {
            return false;
        }
    }
    return true;
}

std::string HashProfileName(const wstring& name) {
    unsigned int hash = 2166136261u;
    for (wstring::const_iterator it = name.begin(); it != name.end(); ++it) {
        hash ^= static_cast<unsigned int>(*it);
        hash *= 16777619u;
    }
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "profile_%08x", hash);
    return buffer;
}

std::string ProfileDirNameFor(const wstring& name) {
    const string legacyAscii = NStr::ToMBCS(name);
    std::string ascii(legacyAscii.c_str());
    return IsPortableProfileDirName(ascii) ? ascii : HashProfileName(name);
}

std::string LegacyProfileDirFor(const wstring& name) {
    return std::string(kLegacyProfileRoot) + ProfileDirNameFor(name) + "\\";
}

std::string LegacyDefaultProfileDir() {
    return std::string(kLegacyProfileRoot) + kDefaultProfileDirName + "\\";
}

void CopyConfigIfMissing(const std::string& srcLegacy, const std::string& dstLegacy) {
    if (IsRegularFile(AbsoluteFromLegacyProfilePath(dstLegacy))) {
        return;
    }

    CFileStream src(srcLegacy.c_str(), CFileStream::WIN_READ_ONLY);
    if (!src.IsOk() || src.GetSize() <= 0) {
        return;
    }

    CFileStream dst(dstLegacy.c_str(), CFileStream::WIN_CREATE);
    if (!dst.IsOk()) {
        return;
    }
    dst.Write(src.GetBuffer(), src.GetSize());
}

void EnsureMinimalConfig(const std::string& legacyPath, const char* contents) {
    if (IsRegularFile(AbsoluteFromLegacyProfilePath(legacyPath))) {
        return;
    }
    CFileStream dst(legacyPath.c_str(), CFileStream::WIN_CREATE);
    if (!dst.IsOk() || contents == 0) {
        return;
    }
    dst.Write(contents, strlen(contents));
}

wstring ReadStoredProfileName(const std::string& dirName) {
    const std::string namePath = JoinPath(JoinPath(AbsoluteProfilesRoot(), dirName), "name.txt");
    FILE* file = fopen(namePath.c_str(), "rb");
    if (file == 0) {
        return NStr::ToUnicode(string(dirName.c_str()));
    }

    char buffer[512];
    const size_t read = fread(buffer, 1, sizeof(buffer) - 1, file);
    fclose(file);
    buffer[read] = 0;
    if (read == 0) {
        return NStr::ToUnicode(string(dirName.c_str()));
    }
    return NStr::ToUnicode(string(buffer));
}

void WriteStoredProfileName(const std::string& legacyDir, const wstring& name) {
    const std::string absoluteDir = AbsoluteFromLegacyProfilePath(legacyDir);
    EnsureDirectory(absoluteDir);
    const std::string namePath = JoinPath(absoluteDir, "name.txt");
    FILE* file = fopen(namePath.c_str(), "wb");
    if (file == 0) {
        return;
    }
    const string legacyUtf8Name = NStr::ToMBCS(name);
    const std::string utf8Name(legacyUtf8Name.c_str());
    fwrite(utf8Name.data(), 1, utf8Name.size(), file);
    fclose(file);
}

void EnsureProfileScaffold(const std::string& legacyDir, const wstring* displayName) {
    EnsureDirectory(AbsoluteProfilesRoot());
    EnsureDirectory(AbsoluteFromLegacyProfilePath(legacyDir));
    EnsureDirectory(AbsoluteFromLegacyProfilePath(legacyDir + "Saves\\"));

    CopyConfigIfMissing(LegacyDefaultProfileDir() + "user.cfg", legacyDir + "user.cfg");
    CopyConfigIfMissing(LegacyDefaultProfileDir() + "input.cfg", legacyDir + "input.cfg");
    EnsureMinimalConfig(std::string(kLegacyProfileRoot) + "global.cfg", "setvar profile_name = default\n");
    EnsureMinimalConfig(LegacyDefaultProfileDir() + "user.cfg", "");
    EnsureMinimalConfig(LegacyDefaultProfileDir() + "input.cfg", "");
    EnsureMinimalConfig(legacyDir + "user.cfg", "");
    EnsureMinimalConfig(legacyDir + "input.cfg", "");

    const string legacyDisplayName = displayName != 0 ? NStr::ToMBCS(*displayName) : string();
    if (displayName != 0 && ProfileDirNameFor(*displayName) != std::string(legacyDisplayName.c_str())) {
        WriteStoredProfileName(legacyDir, *displayName);
    }
}

void LoadUserConfig(const std::string& legacyProfileDir) {
    NGlobal::LoadConfig((legacyProfileDir + "user.cfg").c_str(), STORAGE_USER);
    NGlobal::LoadConfig((legacyProfileDir + "input.cfg").c_str());
}

void RemoveProfileCmd(const string&, const vector<wstring>& params, void*) {
    if (params.size() == 1) {
        NProfile::RemoveProfile(params[0]);
    } else {
        csSystem << "Usage : remove_profile <user name>" << endl;
    }
}

void ChangeProfileCmd(const string&, const vector<wstring>& params, void*) {
    if (params.size() == 1) {
        NProfile::ChangeProfile(params[0]);
    } else {
        csSystem << "Usage: change_profile <user name>" << endl;
    }
}

}  // namespace

namespace NProfile {

void LoadProfile() {
    EnsureDirectory(AbsoluteProfilesRoot());
    EnsureMinimalConfig(std::string(kLegacyProfileRoot) + "global.cfg", "setvar profile_name = default\n");
    NGlobal::LoadConfig((std::string(kLegacyProfileRoot) + "global.cfg").c_str(), STORAGE_GLOBAL);

    const wstring profileName = GetCurrentProfileName();
    const std::string profileDir = LegacyProfileDirFor(profileName);
    EnsureProfileScaffold(profileDir, &profileName);
    LoadUserConfig(profileDir);
}

void SaveProfile() {
    const wstring profileName = GetCurrentProfileName();
    const std::string profileDir = LegacyProfileDirFor(profileName);
    EnsureProfileScaffold(profileDir, &profileName);
    NGlobal::SaveAllVars(
            (std::string(kLegacyProfileRoot) + "global.cfg").c_str(),
            (profileDir + "user.cfg").c_str());
}

bool AddProfile(const wstring& name) {
    const std::string profileDir = LegacyProfileDirFor(name);
    EnsureProfileScaffold(profileDir, &name);
    return true;
}

void ChangeProfile(const wstring& profile) {
    SaveProfile();
    NGlobal::SetVar("profile_name", profile);
    NGlobal::ResetVarsToDefault(STORAGE_USER);
    const std::string profileDir = LegacyProfileDirFor(profile);
    EnsureProfileScaffold(profileDir, &profile);
    LoadUserConfig(profileDir);
}

bool RemoveProfile(const wstring& profile) {
    if (profile == GetCurrentProfileName()) {
        return false;
    }
    const std::string profileDir = LegacyProfileDirFor(profile);
    return RemoveTree(AbsoluteFromLegacyProfilePath(profileDir));
}

void ResetToDefault() {
    NGlobal::ResetVarsToDefault(STORAGE_USER);
    const std::string defaultDir = LegacyDefaultProfileDir();
    EnsureProfileScaffold(defaultDir, 0);
    LoadUserConfig(defaultDir);
    SaveProfile();
}

void GetAllProfiles(vector<wstring>* result) {
    if (result == 0) {
        return;
    }
    result->clear();

    EnsureDirectory(AbsoluteProfilesRoot());
    DIR* dir = opendir(AbsoluteProfilesRoot().c_str());
    if (dir == 0) {
        return;
    }

    while (dirent* entry = readdir(dir)) {
        const char* name = entry->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0 || IsDefaultProfileName(name)) {
            continue;
        }
        if (IsDirectory(JoinPath(AbsoluteProfilesRoot(), name))) {
            result->push_back(ReadStoredProfileName(name));
        }
    }
    closedir(dir);
}

wstring GetCurrentProfileName() {
    return NGlobal::GetVar("profile_name", "default");
}

string GetCurrentProfileDir() {
    const wstring profileName = GetCurrentProfileName();
    const std::string profileDir = LegacyProfileDirFor(profileName);
    EnsureProfileScaffold(profileDir, &profileName);
    return profileDir.c_str();
}

}  // namespace NProfile

START_REGISTER(AndroidProfiles)
REGISTER_CMD("remove_profile", RemoveProfileCmd);
REGISTER_CMD("change_profile", ChangeProfileCmd);
REGISTER_VAR("profile_name", 0, "default", STORAGE_GLOBAL)
FINISH_REGISTER
