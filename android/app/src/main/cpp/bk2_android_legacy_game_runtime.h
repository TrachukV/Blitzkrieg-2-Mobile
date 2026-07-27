#pragma once

#include <cstdint>
#include <string>

struct STerrainInfo;

namespace NDb {
struct SMapInfo;
}

namespace bk2::android {

bool InitializeLegacyGameRuntime(
        const NDb::SMapInfo* map,
        const STerrainInfo& terrain_info,
        int campaign_index,
        int chapter_index,
        int difficulty,
        std::string* error);
void TickLegacyGameRuntime(uint32_t elapsed_millis);
void ShutdownLegacyGameRuntime();
bool IsLegacyGameRuntimeReady();
std::string LegacyGameRuntimeReport();

}
