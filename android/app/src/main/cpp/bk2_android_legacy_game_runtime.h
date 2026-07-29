#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct STerrainInfo;

namespace NDb {
struct SMapInfo;
}

namespace bk2::android {

enum class AndroidCombatEffectType {
    InfantryShot,
    MechanizedShot,
};

struct AndroidCombatEffect {
    AndroidCombatEffectType type = AndroidCombatEffectType::InfantryShot;
    int32_t source_unit_id = -1;
    float source_x = 0.0f;
    float source_y = 0.0f;
    float source_z = 0.0f;
    float destination_x = 0.0f;
    float destination_y = 0.0f;
    float destination_z = 0.0f;
    uint32_t age_millis = 0;
    uint32_t lifetime_millis = 0;
};

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
std::vector<AndroidCombatEffect> CopyActiveAndroidCombatEffects();
void ShutdownLegacyGameRuntime();
bool IsLegacyGameRuntimeReady();
std::string LegacyGameRuntimeReport();

}
