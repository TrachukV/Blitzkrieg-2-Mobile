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
int SelectLegacyUnitNear(
        float world_x,
        float world_y,
        float max_radius,
        int player);
bool SelectLegacyUnit(int unit_id, int player);
bool MoveSelectedLegacyUnit(float world_x, float world_y);
bool AttackSelectedLegacyUnit(int target_unit_id);
int SelectedLegacyUnitId();
void HandleLegacyInputEvent(const char* event_name);
const char* LegacyMissionOutcome();
void ShutdownLegacyGameRuntime();
bool IsLegacyGameRuntimeReady();
std::string LegacyGameRuntimeReport();

}
