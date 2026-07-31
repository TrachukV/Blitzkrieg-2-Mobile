#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct STerrainInfo;

namespace NDb {
struct SMapInfo;
struct STexture;
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

struct AndroidWarFogSnapshot {
    int width = 0;
    int height = 0;
    uint8_t visibility_power = 1;
    uint64_t generation = 0;
    std::vector<uint8_t> visibility;
};

struct AndroidParticleTexture {
    std::string path;
    int width = 0;
    int height = 0;
    bool additive = false;
};

struct AndroidParticleEmitter {
    float offset_x = 0.0f;
    float offset_y = 0.0f;
    float offset_z = 0.0f;
    float scale = 1.0f;
    float speed = 1.0f;
    float time_offset_seconds = 0.0f;
    float end_cycle_seconds = 0.0f;
    int cycle_count = 1;
    std::vector<AndroidParticleTexture> textures;
};

struct AndroidEffectLight {
    float offset_x = 0.0f;
    float offset_y = 0.0f;
    float offset_z = 0.0f;
    float scale = 1.0f;
    float speed = 1.0f;
    float time_offset_seconds = 0.0f;
    float end_cycle_seconds = 0.0f;
    int cycle_count = 1;
};

struct AndroidSceneEffect {
    int32_t victim_unit_id = -1;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    std::string descriptor_id;
    std::vector<AndroidParticleEmitter> emitters;
    std::vector<AndroidEffectLight> lights;
    uint32_t age_millis = 0;
    uint32_t lifetime_millis = 0;
};

struct AndroidAttachedEntityEffect {
    int32_t entity_id = -1;
    std::string descriptor_id;
    std::vector<std::string> locator_names;
    std::vector<AndroidParticleEmitter> emitters;
    std::vector<AndroidEffectLight> lights;
    uint32_t age_millis = 0;
    uint32_t lifetime_millis = 0;
};

struct AndroidDestructionEffect {
    int32_t unit_id = -1;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    std::string descriptor_id;
    std::vector<AndroidParticleEmitter> emitters;
    std::vector<AndroidEffectLight> lights;
    bool uses_fallback_recipe = false;
    uint32_t age_millis = 0;
    uint32_t lifetime_millis = 0;
};

struct LegacyMissionStatisticsPlayer {
    std::string name;
    std::string name_ref;
    std::string statistics_icon_ref;
    int units_lost = 0;
    int units_killed = 0;
    int reinforcements_called = 0;
};

struct LegacyMissionStatisticsReward {
    std::string name_ref;
    const NDb::STexture* icon_texture = nullptr;
    bool upgrade = false;
};

struct LegacyMissionStatisticsSnapshot {
    bool available = false;
    bool won = false;
    bool custom = false;
    int campaign_index = -1;
    int chapter_index = -1;
    int mission_time_seconds = 0;
    int campaign_time_seconds = 0;
    int experience_earned = 0;
    int campaign_experience_current = 0;
    int campaign_experience_next_level = 0;
    int campaign_experience_absolute = 0;
    int campaign_experience_max = 0;
    std::vector<LegacyMissionStatisticsPlayer> players;
    std::vector<LegacyMissionStatisticsReward> rewards;
};

bool InitializeLegacyGameRuntime(
        const NDb::SMapInfo* map,
        const STerrainInfo& terrain_info,
        int campaign_index,
        int chapter_index,
        int difficulty,
        std::string* error);
void TickLegacyGameRuntime(uint32_t elapsed_millis);
// Rotating platform bone of a mechanized unit's stats, empty when it has none.
const std::string& LegacyMechTurretBone(uint64_t stats_path_hash);
const std::string& LegacyMechGunBone(uint64_t stats_path_hash);
int SelectLegacyUnitNear(
        float world_x,
        float world_y,
        float max_radius,
        int player);
bool SelectLegacyUnit(int unit_id, int player);
int SelectLegacyUnits(
        const std::vector<int>& unit_ids,
        int player);
int SelectLegacyUnitsByTypeNear(
        int seed_unit_id,
        float max_radius,
        int player);
bool ActivateSelectedLegacyUnit(int unit_id);
bool MoveSelectedLegacyUnit(float world_x, float world_y);
bool PerformSelectedLegacyUnitPointAction(
        int user_action,
        float world_x,
        float world_y);
bool PerformSelectedLegacyUnitSegmentAction(
        int user_action,
        float start_world_x,
        float start_world_y,
        float end_world_x,
        float end_world_y);
bool CanSelectedLegacyUnitPerformAction(int user_action);
bool AttackSelectedLegacyUnit(int target_unit_id);
bool StopSelectedLegacyUnit();
bool PerformSelectedLegacyUnitAction(int user_action);
int SelectedLegacyUnitId();
int SelectedLegacyUnitCount();
std::string SelectedLegacyUnitHudStatus();
std::string SelectedLegacyUnitHudSnapshot();
void HandleLegacyInputEvent(const char* event_name);
const char* LegacyMissionOutcome();
LegacyMissionStatisticsSnapshot CopyLegacyMissionStatisticsSnapshot();
std::vector<AndroidCombatEffect> CopyActiveAndroidCombatEffects();
std::vector<AndroidSceneEffect> CopyActiveAndroidSceneEffects();
std::vector<AndroidAttachedEntityEffect>
CopyActiveAndroidAttachedEntityEffects();
std::vector<AndroidDestructionEffect> CopyActiveAndroidDestructionEffects();
AndroidWarFogSnapshot CopyAndroidWarFogSnapshot();
void ShutdownLegacyGameRuntime();
bool IsLegacyGameRuntimeReady();
std::string LegacyGameRuntimeReport();

}
