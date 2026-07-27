#pragma once

#include <cstdint>
#include <string>

namespace NDb {
struct SMapInfo;
}

namespace bk2::android {

bool InitializeMissionScriptRuntime(
        const NDb::SMapInfo* map,
        std::string* error);
void TickMissionScriptRuntime(uint32_t elapsed_millis);
void ShutdownMissionScriptRuntime();
std::string MissionScriptRuntimeReport();

}  // namespace bk2::android
