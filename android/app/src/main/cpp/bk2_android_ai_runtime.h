#pragma once

#include <string>

struct STerrainInfo;

namespace NDb {
struct SMapInfo;
}

namespace bk2::android {

bool InitializeAIRuntime(
        const NDb::SMapInfo* map,
        const STerrainInfo& terrain_info,
        std::string* error);
void ShutdownAIRuntime();
std::string AIRuntimeReport();

}
