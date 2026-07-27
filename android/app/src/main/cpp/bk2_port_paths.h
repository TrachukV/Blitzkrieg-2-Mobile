#pragma once

#include <string>

namespace bk2::android {

struct PortPaths {
    std::string files_dir;
    std::string no_backup_dir;
    std::string external_files_dir;

    std::string data_root() const;
    std::string save_root() const;
    std::string log_root() const;
};

void SetPortPaths(PortPaths paths);
PortPaths GetPortPaths();

}  // namespace bk2::android
