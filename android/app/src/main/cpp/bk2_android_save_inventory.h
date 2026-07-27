#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bk2::android {

struct SaveInventoryEntry {
    std::string title;
    std::string save_file;
    std::string info_file;
    std::int64_t save_size = 0;
    std::int64_t info_size = 0;
    std::int64_t modified_time = 0;
    bool has_save = false;
    bool has_info = false;
};

struct SaveInventory {
    std::string legacy_save_dir;
    std::string absolute_save_dir;
    bool save_dir_exists = false;
    bool save_dir_writable = false;
    int save_files = 0;
    int info_files = 0;
    int paired_entries = 0;
    int orphan_info_files = 0;
    int other_files = 0;
    std::int64_t newest_modified_time = 0;
    std::vector<SaveInventoryEntry> entries;
};

SaveInventory ScanSaveInventory();

}  // namespace bk2::android
