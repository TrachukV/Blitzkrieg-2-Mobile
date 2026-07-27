#include "bk2_android_database.h"

#include "bk2_android_platform.h"

#include <mutex>
#include <string>

#if defined(BK2_LEGACY_CORE_SOURCES_ENABLED)
#include "libdb/stdafx.h"

#include "System/VFSOperations.h"
#include "libdb/Db.h"
#endif

namespace bk2::android {
namespace {

#if defined(BK2_LEGACY_CORE_SOURCES_ENABLED)
bool g_database_open = false;
std::mutex g_database_mutex;

void LogDataProbe(NVFS::IVFS* vfs) {
    auto& platform = PlatformRuntime::instance();
    const bool has_types = vfs != nullptr && vfs->DoesFileExist("types.xml");
    const bool has_index = vfs != nullptr && vfs->DoesFileExist("index.bin");

    platform.log_info(std::string("Legacy DB probe: types.xml=") + (has_types ? "present" : "missing")
            + ", index.bin=" + (has_index ? "present" : "missing"));
}
#endif

}  // namespace

bool InitializeLegacyDatabase() {
    auto& platform = PlatformRuntime::instance();

#if defined(BK2_LEGACY_CORE_SOURCES_ENABLED)
    std::lock_guard<std::mutex> lock(g_database_mutex);
    if (g_database_open) {
        return true;
    }

    NVFS::IVFS* vfs = NVFS::GetMainVFS();
    NVFS::IFileCreator* file_creator = NVFS::GetMainFileCreator();
    if (vfs == nullptr || file_creator == nullptr) {
        platform.log_warn("Legacy GameDatabase open skipped: Android VFS is not initialized.");
        return false;
    }

    LogDataProbe(vfs);
    const bool opened = NDb::OpenDatabase(vfs, file_creator, NDb::DATABASE_MODE_GAME);
    if (!opened) {
        NDb::CloseDatabase();
        platform.log_warn("Legacy GameDatabase open failed. Check DataAndroid/types.xml and index.bin.");
        return false;
    }

    NDb::SetLoadDepth(1);
    g_database_open = true;
    platform.log_info("Legacy GameDatabase opened in single-player mode.");
    return true;
#else
    platform.log_warn("Legacy GameDatabase open skipped: legacy core sources are disabled.");
    return false;
#endif
}

void ShutdownLegacyDatabase() {
#if defined(BK2_LEGACY_CORE_SOURCES_ENABLED)
    std::lock_guard<std::mutex> lock(g_database_mutex);
    if (g_database_open) {
        NDb::CloseDatabase();
        g_database_open = false;
        PlatformRuntime::instance().log_info("Legacy GameDatabase closed.");
    }
#endif
}

bool IsLegacyDatabaseOpen() {
#if defined(BK2_LEGACY_CORE_SOURCES_ENABLED)
    std::lock_guard<std::mutex> lock(g_database_mutex);
    return g_database_open;
#else
    return false;
#endif
}

}  // namespace bk2::android
