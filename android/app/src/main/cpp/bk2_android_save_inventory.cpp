#include "bk2_android_save_inventory.h"

#include "Main/stdafx.h"
#include "Main/Profiles.h"

#include "bk2_port_paths.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <dirent.h>
#include <map>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace bk2::android {
namespace {

std::string JoinPath(const std::string& left, const std::string& right) {
    if (left.empty()) {
        return right;
    }
    if (right.empty()) {
        return left;
    }
    return left[left.size() - 1] == '/' ? left + right : left + "/" + right;
}

std::string NormalizeLegacyRelative(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    while (!path.empty() && path[0] == '/') {
        path.erase(0, 1);
    }
    return path;
}

std::string ToLowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch);
    });
    return value;
}

bool EndsWithNoCase(const std::string& value, const char* suffix) {
    const std::string lower_value = ToLowerAscii(value);
    const std::string lower_suffix = ToLowerAscii(suffix);
    return lower_value.size() >= lower_suffix.size() &&
           lower_value.compare(lower_value.size() - lower_suffix.size(), lower_suffix.size(), lower_suffix) == 0;
}

std::string RemoveExtension(const std::string& file_name) {
    const size_t dot = file_name.find_last_of('.');
    return dot == std::string::npos ? file_name : file_name.substr(0, dot);
}

bool IsRegularFile(const std::string& path, struct stat* out) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        return false;
    }
    if (out != nullptr) {
        *out = st;
    }
    return true;
}

bool IsDirectory(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool IsWritableDirectory(const std::string& path) {
    const std::string probe_path = JoinPath(path, ".bk2_write_probe");
    FILE* file = fopen(probe_path.c_str(), "wb");
    if (file == nullptr) {
        return false;
    }
    fclose(file);
    unlink(probe_path.c_str());
    return true;
}

std::string AbsoluteFromLegacyRelative(const std::string& legacy_path) {
    const PortPaths paths = GetPortPaths();
    const std::string normalized = NormalizeLegacyRelative(legacy_path);
    if (!paths.files_dir.empty()) {
        return JoinPath(paths.files_dir, normalized);
    }
    return normalized;
}

void UpdateEntryFromSave(
        SaveInventoryEntry* entry,
        const std::string& file_name,
        const struct stat& st) {
    entry->save_file = file_name;
    entry->save_size = static_cast<std::int64_t>(st.st_size);
    entry->modified_time = std::max(entry->modified_time, static_cast<std::int64_t>(st.st_mtime));
    entry->has_save = true;
}

void UpdateEntryFromInfo(
        SaveInventoryEntry* entry,
        const std::string& file_name,
        const struct stat& st) {
    entry->info_file = file_name;
    entry->info_size = static_cast<std::int64_t>(st.st_size);
    entry->modified_time = std::max(entry->modified_time, static_cast<std::int64_t>(st.st_mtime));
    entry->has_info = true;
}

}  // namespace

SaveInventory ScanSaveInventory() {
    const string legacy_profile_dir = NProfile::GetCurrentProfileDir();
    SaveInventory inventory;
    inventory.legacy_save_dir = std::string((legacy_profile_dir + "Saves\\").c_str());
    inventory.absolute_save_dir = AbsoluteFromLegacyRelative(inventory.legacy_save_dir);
    inventory.save_dir_exists = IsDirectory(inventory.absolute_save_dir);
    inventory.save_dir_writable = inventory.save_dir_exists && IsWritableDirectory(inventory.absolute_save_dir);

    DIR* dir = opendir(inventory.absolute_save_dir.c_str());
    if (dir == nullptr) {
        return inventory;
    }

    std::map<std::string, SaveInventoryEntry> entries;
    while (dirent* entry = readdir(dir)) {
        const char* raw_name = entry->d_name;
        if (std::string(raw_name) == "." || std::string(raw_name) == "..") {
            continue;
        }

        const std::string file_name(raw_name);
        const std::string full_path = JoinPath(inventory.absolute_save_dir, file_name);
        struct stat st;
        if (!IsRegularFile(full_path, &st)) {
            continue;
        }

        if (EndsWithNoCase(file_name, ".sav")) {
            ++inventory.save_files;
            SaveInventoryEntry& save_entry = entries[RemoveExtension(file_name)];
            save_entry.title = RemoveExtension(file_name);
            UpdateEntryFromSave(&save_entry, file_name, st);
            inventory.newest_modified_time =
                    std::max(inventory.newest_modified_time, static_cast<std::int64_t>(st.st_mtime));
        } else if (EndsWithNoCase(file_name, ".sfo")) {
            ++inventory.info_files;
            SaveInventoryEntry& save_entry = entries[RemoveExtension(file_name)];
            save_entry.title = RemoveExtension(file_name);
            UpdateEntryFromInfo(&save_entry, file_name, st);
            inventory.newest_modified_time =
                    std::max(inventory.newest_modified_time, static_cast<std::int64_t>(st.st_mtime));
        } else {
            ++inventory.other_files;
        }
    }
    closedir(dir);

    inventory.entries.reserve(entries.size());
    for (std::map<std::string, SaveInventoryEntry>::const_iterator it = entries.begin();
         it != entries.end();
         ++it) {
        if (it->second.has_save && it->second.has_info) {
            ++inventory.paired_entries;
        } else if (!it->second.has_save && it->second.has_info) {
            ++inventory.orphan_info_files;
        }
        inventory.entries.push_back(it->second);
    }

    std::sort(inventory.entries.begin(), inventory.entries.end(), [](const SaveInventoryEntry& left, const SaveInventoryEntry& right) {
        if (left.modified_time != right.modified_time) {
            return left.modified_time > right.modified_time;
        }
        return left.title < right.title;
    });
    return inventory;
}

}  // namespace bk2::android
