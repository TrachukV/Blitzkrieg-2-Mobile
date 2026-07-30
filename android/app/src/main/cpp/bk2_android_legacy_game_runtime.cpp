#include "bk2_android_legacy_game_runtime.h"

#include "bk2_android_mission_runtime.h"
#include "bk2_android_audio_backend.h"
#include "bk2_android_audio_decode.h"
#include "bk2_android_platform.h"
#include "bk2_presentation_internal.h"

#include "GameX/stdafx.h"
#include "AILogic/Specific.h"
#include "AILogic/AIConsts.h"
#include "AILogic/B2AI.h"
#include "AILogic/CreateAI.h"
#include "AILogic/GlobeUpdater.h"
#include "AILogic/GroupLogic.h"
#include "AILogic/AIMap.h"
#include "AILogic/AIUnit.h"
#include "AILogic/NewUpdater.h"
#include "AILogic/Soldier.h"
#include "AILogic/Statistics.h"
#include "AILogic/UnitStates.h"
#include "AILogic/UnitsIterators.h"
#include "B2_M1_Terrain/DBTerrain.h"
#include "B2_M1_World/MissionObjectiveStates.h"
#include "Common_RTS_AI/AIClasses.h"
#include "Common_RTS_AI/aiobjectbase.h"
#include "Common_RTS_AI/CommonPathFinder.h"
#include "Common_RTS_AI/Pathfinders.h"
#include "Common_RTS_AI/StaticMapHeights.h"
#include "3Dmotor/DBScene.h"
#include "GameX/DBGameRoot.h"
#include "Sound/DBSound.h"
#include "Sound/DBSoundDesc.h"
#include "System/GlobalVars.h"
#include "System/VFSOperations.h"
#include "Misc/StrProc.h"
#include "GameX/GetConsts.h"
#include "GameX/ScenarioTracker.h"
#include "Main/GameTimer.h"
#include "SceneB2/TerrainInfo.h"
#include "Stats_B2_M1/DBMapInfo.h"
#include "Stats_B2_M1/DBVisObj.h"
#include "Stats_B2_M1/RPGStats.h"
#include "Stats_B2_M1/ActionsRemap.h"
#include "Stats_B2_M1/AIUpdates.h"
#include "Stats_B2_M1/DBNotifications.h"
#include "Stats_B2_M1/FeedBackUpdates.h"
#include "Stats_B2_M1/AnimationType.h"
#include "Stats_B2_M1/TerraAIObserver.h"
#include "Stats_B2_M1/Vis2AI.h"
#include "System/VFSOperations.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <sstream>
#include <unordered_map>
#include <vector>

extern "C" {
extern int bk2_android_ai_debug_load_objects;
extern int bk2_android_ai_debug_load_candidates;
extern int bk2_android_ai_debug_reinforcement_deferred;
extern int bk2_android_ai_debug_add_calls;
extern int bk2_android_ai_debug_add_success;
extern int bk2_android_ai_debug_add_failed;
extern int bk2_android_ai_debug_empty_stats;
extern int bk2_android_ai_debug_bare_infantry;
extern int bk2_android_ai_debug_bad_visual;
extern int bk2_android_ai_debug_outside_map;
extern int bk2_android_ai_debug_player_missing;
extern int bk2_android_ai_debug_unit_case;
extern int bk2_android_ai_debug_squad_case;
extern int bk2_android_ai_debug_other_case;
}

extern CGroupLogic theGroupLogic;
extern CStatistics theStatistics;

namespace bk2::android {
namespace {

constexpr float kPackedHeightScale = 0.01f;

CPtr<IScenarioTracker> g_scenario_tracker;
CPtr<ITerraAIObserver> g_terrain_observer;
CPtr<IGameTimer> g_game_timer;
IAILogic* g_ai_logic = nullptr;
bool g_ready = false;
uint64_t g_timer_millis = 1;
uint64_t g_segment_count = 0;
int g_unit_count = 0;
int g_terrain_type_count = 0;
int g_terrain_grid_width = 0;
int g_terrain_grid_height = 0;
int g_terrain_feature_count = 0;
int g_map_player_count = 0;
int g_diplomacy_count = 0;
int g_start_unit_object_count = 0;
int g_ai_unit_total_count = 0;
int g_ai_unit_alive_count = 0;
int g_ai_unit_ref_valid_count = 0;
int g_normalized_rpg_stats_count = 0;
int g_normalized_unit_stats_count = 0;
int g_normalized_squad_stats_count = 0;
int g_normalize_skipped_no_visual_count = 0;
int g_selected_unit_id = -1;
std::vector<int> g_selected_unit_ids;
int g_attack_target_unit_id = -1;
bool g_android_command_group_registered = false;
WORD g_android_command_group = 0;
uint64_t g_player_move_command_count = 0;
uint64_t g_player_attack_command_count = 0;
uint64_t g_player_stop_command_count = 0;
uint64_t g_client_update_count = 0;
uint64_t g_objective_update_count = 0;
uint64_t g_hud_notification_count = 0;
struct TimedCombatEffect {
    AndroidCombatEffect effect;
    uint64_t created_millis = 0;
    uint64_t expires_millis = 0;
};
std::vector<TimedCombatEffect> g_combat_effects;
struct TimedSceneEffect {
    AndroidSceneEffect effect;
    uint64_t created_millis = 0;
    uint64_t expires_millis = 0;
};
std::vector<TimedSceneEffect> g_scene_effects;
struct TimedDestructionEffect {
    AndroidDestructionEffect effect;
    uint64_t created_millis = 0;
    uint64_t expires_millis = 0;
    bool exact_animation_recipe = false;
};
std::vector<TimedDestructionEffect> g_destruction_effects;
struct UnitDeathEffectCandidates {
    std::string smoke_descriptor_id;
    std::vector<AndroidParticleEmitter> smoke_emitters;
    std::vector<AndroidEffectLight> smoke_lights;
    uint32_t smoke_lifetime_millis = 0;
    std::string fatality_descriptor_id;
    std::vector<AndroidParticleEmitter> fatality_emitters;
    std::vector<AndroidEffectLight> fatality_lights;
    uint32_t fatality_lifetime_millis = 0;
};
std::unordered_map<int32_t, UnitDeathEffectCandidates>
        g_unit_death_effect_candidates;
uint64_t g_infantry_shot_effect_count = 0;
uint64_t g_mechanized_shot_effect_count = 0;
uint64_t g_mechanized_destruction_effect_count = 0;
uint64_t g_descriptor_scene_effect_count = 0;
uint64_t g_descriptor_particle_emitter_count = 0;
uint64_t g_descriptor_particle_texture_count = 0;
uint64_t g_descriptor_effect_light_count = 0;
uint64_t g_forwarded_unit_kill_count = 0;
uint64_t g_forwarded_unit_kill_error_count = 0;
struct PresentationCorpse {
    Bk2PresentationEntity entity{};
    uint64_t expires_millis = 0;
};
std::unordered_map<int32_t, Bk2PresentationEntity>
        g_last_presentation_entities;
std::unordered_map<int32_t, PresentationCorpse> g_presentation_corpses;
uint64_t g_presentation_death_count = 0;
AndroidWarFogSnapshot g_war_fog_snapshot;
bool g_war_fog_first_update = true;
enum LegacyMissionOutcomeValue {
    kLegacyMissionRunning = 0,
    kLegacyMissionWon = 1,
    kLegacyMissionLost = 2,
    kLegacyMissionProgressionError = 3,
};
std::atomic<int> g_mission_outcome{kLegacyMissionRunning};
std::atomic<int> g_pending_mission_outcome{kLegacyMissionRunning};
std::set<std::string> g_missing_unit_payload_refs;
std::set<std::string> g_missing_squad_payload_refs;
std::string g_stage = "not_started";

CUserActions LegacyUnitActions(CAIUnit* unit);

bool IsSelectedLegacyUnitId(int unit_id) {
    return std::find(
            g_selected_unit_ids.begin(),
            g_selected_unit_ids.end(),
            unit_id) != g_selected_unit_ids.end();
}

bool IsAllUnitsSelectionAction(int user_action) {
    return user_action == NDb::USER_ACTION_MOVE ||
            user_action == NDb::USER_ACTION_ATTACK ||
            user_action == NDb::USER_ACTION_ROTATE ||
            user_action == NDb::USER_ACTION_ENTRENCH_SELF ||
            user_action == NDb::USER_ACTION_STAND_GROUND ||
            user_action == NDb::USER_ACTION_STOP;
}

bool IsLegacyUnitAction(CAIUnit* unit, int user_action) {
    return unit != nullptr &&
            user_action >= 0 &&
            user_action <= 127 &&
            LegacyUnitActions(unit).HasAction(user_action);
}

CUserActions LegacyUnitActions(CAIUnit* unit) {
    CUserActions actions;
    if (unit == nullptr) {
        return actions;
    }
    const NDb::SUnitBaseRPGStats* stats = unit->GetStats();
    if (stats == nullptr || stats->GetActions() == nullptr) {
        return actions;
    }
    actions = stats->GetActions()->availUserActions;
    actions |= stats->GetActions()->availUserExposures;
    for (int command = 0; command < 128; ++command) {
        if (!stats->GetActions()->availCommands.GetData(command) &&
            !stats->GetActions()->availExposures.GetData(command)) {
            continue;
        }
        const NDb::EUserAction action =
                GetActionByCommand(static_cast<EActionCommand>(command));
        if (action != NDb::USER_ACTION_UNKNOWN) {
            actions.SetAction(static_cast<int>(action));
        }
    }
    // SUnitActions::ToAIUnits adds these common controls during the desktop
    // PostLoad path. Some mobile-loaded mission references skip that pass, so
    // derive the same visible controls without mutating shared DB resources.
    actions.SetAction(NDb::USER_ACTION_STOP);
    actions.SetAction(NDb::USER_ACTION_ATTACK);
    if (unit->CanMove()) {
        actions.SetAction(NDb::USER_ACTION_MOVE);
    }
    const int ability_count = std::min(
            static_cast<int>(
                    stats->GetActions()->specialAbilities.size()),
            unit->GetAbilityLevel());
    for (int index = 0; index < ability_count; ++index) {
        const NDb::SUnitSpecialAblityDesc* ability =
                stats->GetActions()->specialAbilities[index];
        if (ability == nullptr) {
            continue;
        }
        const NDb::EUserAction action =
                GetActionByAbility(ability->eName);
        if (action != NDb::USER_ACTION_UNKNOWN) {
            actions.SetAction(static_cast<int>(action));
        }
    }
    return actions;
}

CUserActions SelectedLegacyActions(CAIUnit* active_unit) {
    CUserActions actions = LegacyUnitActions(active_unit);
    CUserActions common_actions;
    for (int unit_id : g_selected_unit_ids) {
        CAIUnit* unit = CAIUnit::GetUnitByUniqueID(unit_id);
        if (unit == nullptr ||
            !unit->IsAlive() ||
            !unit->IsSelectable() ||
            unit->GetPlayer() != 0) {
            continue;
        }
        const CUserActions unit_actions = LegacyUnitActions(unit);
        for (int action : {
                     static_cast<int>(NDb::USER_ACTION_MOVE),
                     static_cast<int>(NDb::USER_ACTION_ATTACK),
                     static_cast<int>(NDb::USER_ACTION_STOP)}) {
            if (unit_actions.HasAction(action)) {
                common_actions.SetAction(action);
            }
        }
    }
    actions |= common_actions;
    return actions;
}

bool AnySelectedLegacyUnitCanMove() {
    for (int unit_id : g_selected_unit_ids) {
        CAIUnit* unit = CAIUnit::GetUnitByUniqueID(unit_id);
        if (unit != nullptr &&
            unit->IsAlive() &&
            unit->IsSelectable() &&
            unit->GetPlayer() == 0 &&
            unit->CanMove()) {
            return true;
        }
    }
    return false;
}

// CSelector::DoGroupCommand registers the group, issues the command and
// unregisters straight away. Leaving the group alive keeps CGroupLogic
// managing its subgroup shifts between orders, which makes units drift.
void ReleaseSelectedLegacyCommandGroup() {
    if (!g_android_command_group_registered) {
        return;
    }
    theGroupLogic.UnregisterGroup(g_android_command_group);
    g_android_command_group_registered = false;
}

bool RegisterSelectedLegacyCommandGroup(
        int user_action,
        bool movable_only,
        WORD* group,
        size_t* unit_count) {
    if (!g_ready || group == nullptr || unit_count == nullptr) {
        return false;
    }
    vector<int> command_units;
    command_units.reserve(g_selected_unit_ids.size());
    CAIUnit* active_unit =
            CAIUnit::GetUnitByUniqueID(g_selected_unit_id);
    const bool all_units_action =
            IsAllUnitsSelectionAction(user_action);
    for (int unit_id : g_selected_unit_ids) {
        CAIUnit* unit = CAIUnit::GetUnitByUniqueID(unit_id);
        if (unit == nullptr ||
            !unit->IsAlive() ||
            !unit->IsSelectable() ||
            unit->GetPlayer() != 0 ||
            (movable_only && !unit->CanMove()) ||
            (!all_units_action &&
             active_unit != nullptr &&
             unit->GetStats() != active_unit->GetStats()) ||
            (!all_units_action &&
             user_action != NDb::USER_ACTION_UNKNOWN &&
             !IsLegacyUnitAction(unit, user_action))) {
            continue;
        }
        command_units.push_back(unit_id);
    }
    if (command_units.empty()) {
        if (active_unit != nullptr) {
            std::ostringstream report;
            report << "player_command_group=empty"
                   << "; action=" << user_action
                   << "; alive=" << (active_unit->IsAlive() ? 1 : 0)
                   << "; selectable="
                   << (active_unit->IsSelectable() ? 1 : 0)
                   << "; player=" << static_cast<int>(active_unit->GetPlayer())
                   << "; can_move=" << (active_unit->CanMove() ? 1 : 0)
                   << "; has_action="
                   << (IsLegacyUnitAction(active_unit, user_action) ? 1 : 0)
                   << "; all_units=" << (all_units_action ? 1 : 0);
            PlatformRuntime::instance().log_info(report.str());
        }
        return false;
    }
    if (g_android_command_group_registered) {
        theGroupLogic.UnregisterGroup(g_android_command_group);
        g_android_command_group_registered = false;
    }
    g_android_command_group = theGroupLogic.GenerateGroupNumber();
    theGroupLogic.RegisterGroup(
            command_units,
            g_android_command_group);
    g_android_command_group_registered = true;
    *group = g_android_command_group;
    *unit_count = command_units.size();
    return true;
}

bool UpdateWarFogSnapshot() {
    if (g_ai_logic == nullptr) {
        return false;
    }
    CArray2D<BYTE>* fog = nullptr;
    const bool complete = g_ai_logic->GetMiniMapWarForInfo(
            &fog,
            g_war_fog_first_update);
    if (!complete || fog == nullptr ||
        fog->GetSizeX() < 2 || fog->GetSizeY() < 2) {
        return false;
    }
    g_war_fog_first_update = false;
    AndroidWarFogSnapshot next;
    next.width = fog->GetSizeX();
    next.height = fog->GetSizeY();
    next.visibility_power = static_cast<uint8_t>(
            std::max(g_ai_logic->VIS_POWER(), 1));
    next.visibility.resize(
            static_cast<size_t>(next.width * next.height));
    size_t visible_cells = 0;
    for (int y = 0; y < next.height; ++y) {
        for (int x = 0; x < next.width; ++x) {
            const uint8_t value = (*fog)[y][x];
            next.visibility[
                    static_cast<size_t>(y * next.width + x)] = value;
            if (value >= next.visibility_power) {
                ++visible_cells;
            }
        }
    }
    if (next.width == g_war_fog_snapshot.width &&
        next.height == g_war_fog_snapshot.height &&
        next.visibility_power == g_war_fog_snapshot.visibility_power &&
        next.visibility == g_war_fog_snapshot.visibility) {
        return true;
    }
    next.generation = g_war_fog_snapshot.generation + 1;
    const bool first_snapshot = g_war_fog_snapshot.generation == 0;
    g_war_fog_snapshot = std::move(next);
    if (first_snapshot) {
        PlatformRuntime::instance().log_info(
                std::string("war_fog=ready; size=") +
                std::to_string(g_war_fog_snapshot.width) + "x" +
                std::to_string(g_war_fog_snapshot.height) +
                "; power=" +
                std::to_string(g_war_fog_snapshot.visibility_power) +
                "; visible_cells=" +
                std::to_string(visible_cells));
    }
    return true;
}

uint64_t StatsPathHash(const NDb::SHPObjectRPGStats* stats) {
    if (stats == nullptr) {
        return 0;
    }
    std::string path(stats->GetDBID().ToString().c_str());
    const size_t xpointer = path.find('#');
    if (xpointer != std::string::npos) {
        path.resize(xpointer);
    }
    while (!path.empty() && (path[0] == '/' || path[0] == '\\')) {
        path.erase(path.begin());
    }
    uint64_t result = 0xcbf29ce484222325ull;
    for (char character : path) {
        unsigned char byte = static_cast<unsigned char>(
                character == '\\' ? '/' : character);
        if (byte >= 'A' && byte <= 'Z') {
            byte = static_cast<unsigned char>(byte - 'A' + 'a');
        }
        result ^= byte;
        result *= 0x100000001b3ull;
    }
    return result;
}

int GeometryRecordId(const NDb::SHPObjectRPGStats* stats) {
    if (stats == nullptr) {
        return -1;
    }
    const NDb::SVisObj* visual = stats->pvisualObject.GetPtr();
    if (visual == nullptr) {
        return -1;
    }
    const NDb::SModel* fallback = nullptr;
    const NDb::SModel* summer = nullptr;
    for (const NDb::SVisObj::SSingleObj& candidate : visual->models) {
        const NDb::SModel* model = candidate.pModel.GetPtr();
        if (model == nullptr || model->pGeometry.IsEmpty()) {
            continue;
        }
        if (fallback == nullptr) {
            fallback = model;
        }
        if (candidate.eSeason == NDb::SEASON_ASIA) {
            return model->pGeometry->GetRecordID();
        }
        if (candidate.eSeason == NDb::SEASON_SUMMER) {
            summer = model;
        }
    }
    const NDb::SModel* model = summer == nullptr ? fallback : summer;
    return model == nullptr ? -1 : model->pGeometry->GetRecordID();
}

void SetStage(const char* stage) {
    g_stage = stage;
    PlatformRuntime::instance().log_info(
            std::string("legacy_game_stage=") + stage);
}

int HeightWidth(const STerrainInfo& info) {
    return info.heights.GetSizeX() > 0
            ? info.heights.GetSizeX()
            : info.optimizedHeights.GetSizeX();
}

int HeightHeight(const STerrainInfo& info) {
    return info.heights.GetSizeY() > 0
            ? info.heights.GetSizeY()
            : info.optimizedHeights.GetSizeY();
}

float FullVisualHeight(const STerrainInfo& info, int x, int y) {
    float height = info.heights.GetSizeX() > 0
            ? info.heights[y][x]
            : static_cast<float>(info.optimizedHeights[y][x]) *
                    kPackedHeightScale;
    if (info.addHeights.GetSizeX() == HeightWidth(info) &&
        info.addHeights.GetSizeY() == HeightHeight(info)) {
        height += info.addHeights[y][x];
    } else if (
            info.optimizedAddHeights.GetSizeX() == HeightWidth(info) &&
            info.optimizedAddHeights.GetSizeY() == HeightHeight(info)) {
        height += static_cast<float>(info.optimizedAddHeights[y][x]) *
                kPackedHeightScale;
    }
    return height;
}

bool IsSeaTile(const STerrainInfo& info, int x, int y) {
    if (!info.seaMask.IsEmpty() &&
        x < info.seaMask.GetSizeX() &&
        y < info.seaMask.GetSizeY()) {
        return info.seaMask[y][x] != 0;
    }
    if (!info.optimizedSeaMask.IsEmpty() &&
        x < info.optimizedSeaMask.GetSizeX() &&
        y < info.optimizedSeaMask.GetSizeY()) {
        return info.optimizedSeaMask.GetData(x, y) != 0;
    }
    return false;
}

int RequiredTerrainTypeCount(const STerrainInfo& info) {
    int count = 1;
    for (int y = 0; y < info.tileTerraMap.GetSizeY(); ++y) {
        for (int x = 0; x < info.tileTerraMap.GetSizeX(); ++x) {
            count = std::max(
                    count,
                    static_cast<int>(info.tileTerraMap[y][x]) + 1);
        }
    }
    return count;
}

NDb::STerrainAIProperties DefaultTerrainAIProperties() {
    NDb::STerrainAIProperties properties;
    properties.fPassability = 1.0f;
    properties.nAIClass = EAC_TERRAIN;
    properties.nAIPassabilityClass = EAC_TERRAIN;
    properties.bCanEntrench = true;
    properties.nSoilType = 0;
    return properties;
}

bool IsStartUnitObject(const NDb::SMapObjectInfo& object) {
    const NDb::SHPObjectRPGStats* stats = object.pObject.GetPtrNoLoad();
    if (stats == nullptr) {
        return false;
    }
    const int type_id = stats->GetTypeID();
    return type_id == NDb::SMechUnitRPGStats::typeID ||
           type_id == NDb::SSquadRPGStats::typeID ||
           type_id == NDb::SInfantryRPGStats::typeID;
}

bool IsUnitStats(const NDb::SHPObjectRPGStats* stats) {
    if (stats == nullptr) {
        return false;
    }
    const int type_id = stats->GetTypeID();
    return type_id == NDb::SMechUnitRPGStats::typeID ||
           type_id == NDb::SInfantryRPGStats::typeID;
}

bool IsSquadStats(const NDb::SHPObjectRPGStats* stats) {
    return stats != nullptr && stats->GetTypeID() == NDb::SSquadRPGStats::typeID;
}

std::string NormalizeLegacyResourcePath(std::string path) {
    const size_t xpointer = path.find('#');
    if (xpointer != std::string::npos) {
        path.resize(xpointer);
    }
    std::replace(path.begin(), path.end(), '\\', '/');
    while (!path.empty() && path[0] == '/') {
        path.erase(0, 1);
    }
    return path;
}

bool VfsFileExists(const std::string& path) {
    NVFS::IVFS* vfs = NVFS::GetMainVFS();
    if (vfs == nullptr) {
        return false;
    }
    const string legacy_path(path.c_str());
    if (vfs->DoesFileExist(legacy_path)) {
        return true;
    }
    const std::string normalized = NormalizeLegacyResourcePath(path);
    if (normalized == path) {
        return false;
    }
    const string normalized_path(normalized.c_str());
    return vfs->DoesFileExist(normalized_path);
}

bool HasDbPayload(const NDb::SHPObjectRPGStats* stats) {
    if (stats == nullptr) {
        return false;
    }
    return VfsFileExists(std::string(stats->GetDBID().ToString().c_str()));
}

void RecordMissingPayload(const NDb::SHPObjectRPGStats* stats) {
    if (stats == nullptr) {
        return;
    }
    const std::string dbid = NormalizeLegacyResourcePath(
            std::string(stats->GetDBID().ToString().c_str()));
    if (IsUnitStats(stats)) {
        g_missing_unit_payload_refs.insert(dbid);
    } else if (IsSquadStats(stats)) {
        g_missing_squad_payload_refs.insert(dbid);
    }
}

std::string JoinRefSamples(const std::set<std::string>& refs, size_t limit) {
    std::ostringstream stream;
    size_t count = 0;
    for (const std::string& ref : refs) {
        if (count >= limit) {
            stream << ",...";
            break;
        }
        if (count != 0) {
            stream << ",";
        }
        stream << ref;
        ++count;
    }
    return stream.str();
}

bool HasRequiredUnitVisual(const NDb::SHPObjectRPGStats* stats) {
    return !IsUnitStats(stats) || !stats->pvisualObject.IsEmpty();
}

void NormalizeMapObjectRPGStats(
        const NDb::SMapObjectInfo& object,
        std::set<const NDb::SHPObjectRPGStats*>* seen) {
    const NDb::SHPObjectRPGStats* raw_stats = object.pObject.GetPtrNoLoad();
    if (raw_stats != nullptr &&
        (IsUnitStats(raw_stats) || IsSquadStats(raw_stats))) {
        if (seen->find(raw_stats) != seen->end()) {
            return;
        }
        if (!HasDbPayload(raw_stats)) {
            seen->insert(raw_stats);
            RecordMissingPayload(raw_stats);
            return;
        }
    }

    const NDb::SHPObjectRPGStats* loaded_stats = object.pObject.GetPtr();
    if (loaded_stats == nullptr || seen->find(loaded_stats) != seen->end()) {
        return;
    }
    seen->insert(loaded_stats);

    if (!IsUnitStats(loaded_stats) && !IsSquadStats(loaded_stats)) {
        return;
    }
    if (!HasDbPayload(loaded_stats)) {
        RecordMissingPayload(loaded_stats);
        return;
    }
    if (!HasRequiredUnitVisual(loaded_stats)) {
        ++g_normalize_skipped_no_visual_count;
        return;
    }

    NDb::SHPObjectRPGStats* stats =
            const_cast<NDb::SHPObjectRPGStats*>(loaded_stats);
    stats->PostLoad(false);
    ++g_normalized_rpg_stats_count;
    if (IsUnitStats(stats)) {
        ++g_normalized_unit_stats_count;
    } else if (IsSquadStats(stats)) {
        ++g_normalized_squad_stats_count;
    }
}

void NormalizeMapRPGStats(const NDb::SMapInfo* map) {
    g_normalized_rpg_stats_count = 0;
    g_normalized_unit_stats_count = 0;
    g_normalized_squad_stats_count = 0;
    g_normalize_skipped_no_visual_count = 0;
    g_missing_unit_payload_refs.clear();
    g_missing_squad_payload_refs.clear();
    if (map == nullptr) {
        return;
    }

    std::set<const NDb::SHPObjectRPGStats*> seen;
    for (const NDb::SMapObjectInfo& object : map->objects) {
        NormalizeMapObjectRPGStats(object, &seen);
    }
    for (const NDb::SMapObjectInfo& object : map->scenarioObjects) {
        NormalizeMapObjectRPGStats(object, &seen);
    }
}

int CountStartUnitObjects(const NDb::SMapInfo* map) {
    int count = 0;
    if (map == nullptr) {
        return count;
    }
    for (const NDb::SMapObjectInfo& object : map->objects) {
        if (IsStartUnitObject(object)) {
            ++count;
        }
    }
    for (const NDb::SMapObjectInfo& object : map->scenarioObjects) {
        if (IsStartUnitObject(object)) {
            ++count;
        }
    }
    return count;
}

void CountAIUnits() {
    g_ai_unit_total_count = 0;
    g_ai_unit_alive_count = 0;
    g_ai_unit_ref_valid_count = 0;
    for (CGlobalIter iter(0, ANY_PARTY); !iter.IsFinished(); iter.Iterate()) {
        CAIUnit* unit = *iter;
        if (unit == nullptr) {
            continue;
        }
        ++g_ai_unit_total_count;
        if (unit->IsAlive()) {
            ++g_ai_unit_alive_count;
        }
        if (unit->IsRefValid()) {
            ++g_ai_unit_ref_valid_count;
        }
    }
}

const char* MissionOutcomeName(int outcome) {
    switch (outcome) {
        case kLegacyMissionWon:
            return "won";
        case kLegacyMissionLost:
            return "lost";
        case kLegacyMissionProgressionError:
            return "progression_error";
        default:
            return "running";
    }
}

void FinalizePendingMissionOutcome() {
    const int pending = g_pending_mission_outcome.exchange(
            kLegacyMissionRunning);
    if (pending == kLegacyMissionRunning ||
        g_mission_outcome.load() != kLegacyMissionRunning) {
        return;
    }

    MissionRuntimeResult progression;
    if (pending == kLegacyMissionWon) {
        if (g_scenario_tracker != nullptr &&
            !g_scenario_tracker->IsMissionWon()) {
            g_scenario_tracker->MissionWin();
        }
        progression = MarkMissionWon();
        if (progression.ok) {
            const MissionRuntimeResult autosave =
                    SaveMissionRuntimeCheckpoint("android_autosave");
            PlatformRuntime::instance().log_info(
                    autosave.ok
                            ? "mission_autosave=saved"
                            : std::string("mission_autosave=failed; error=") +
                                      autosave.error);
        }
    } else {
        if (g_scenario_tracker != nullptr) {
            g_scenario_tracker->MissionCancel();
        }
        progression = CancelMission();
    }

    const int outcome = progression.ok
            ? pending
            : kLegacyMissionProgressionError;
    g_mission_outcome.store(outcome);
    PlatformRuntime::instance().log_info(
            std::string("mission_outcome=") +
            MissionOutcomeName(outcome) +
            (progression.ok
                    ? ""
                    : "; progression_error=" + progression.error));
}

std::string NormalizeParticleTexturePath(
        const NDb::STexture* texture) {
    if (texture == nullptr) {
        return {};
    }
    std::string path = texture->szDestName.c_str();
    std::replace(path.begin(), path.end(), '\\', '/');
    while (!path.empty() && path.front() == '/') {
        path.erase(path.begin());
    }
    if (path.find('/') != std::string::npos) {
        return path;
    }
    std::string descriptor = texture->GetDBID().ToString().c_str();
    const size_t xpointer = descriptor.find('#');
    if (xpointer != std::string::npos) {
        descriptor.resize(xpointer);
    }
    std::replace(descriptor.begin(), descriptor.end(), '\\', '/');
    while (!descriptor.empty() && descriptor.front() == '/') {
        descriptor.erase(descriptor.begin());
    }
    const size_t slash = descriptor.rfind('/');
    if (slash == std::string::npos) {
        return path;
    }
    return descriptor.substr(0, slash + 1) + path;
}

uint32_t AppendSceneEffectRecipe(
        const NDb::SEffect* scene_effect,
        std::vector<AndroidParticleEmitter>* emitters,
        std::vector<AndroidEffectLight>* lights) {
    if (scene_effect == nullptr ||
        emitters == nullptr ||
        lights == nullptr) {
        return 0;
    }
    float lifetime_seconds =
            std::max(scene_effect->fDuration, 0.1f);
    for (const CDBPtr<NDb::SParticleInstance>& instance_ref :
         scene_effect->instances) {
        const NDb::SParticleInstance* instance =
                instance_ref.GetPtr();
        if (instance == nullptr || instance->textures.empty()) {
            continue;
        }
        AndroidParticleEmitter emitter;
        emitter.offset_x = AI2Vis(instance->vPosition.x);
        emitter.offset_y = AI2Vis(instance->vPosition.y);
        emitter.offset_z = AI2Vis(instance->vPosition.z);
        emitter.scale =
                std::isfinite(instance->fScale)
                ? std::clamp(instance->fScale, 0.05f, 20.0f)
                : 1.0f;
        emitter.speed =
                std::isfinite(instance->fSpeed) &&
                        std::abs(instance->fSpeed) > 0.001f
                ? instance->fSpeed
                : 1.0f;
        emitter.time_offset_seconds =
                std::isfinite(instance->fOffset)
                ? instance->fOffset
                : 0.0f;
        emitter.end_cycle_seconds =
                std::isfinite(instance->fEndCycle)
                ? std::max(instance->fEndCycle, 0.0f)
                : 0.0f;
        emitter.cycle_count = instance->nCycleCount;
        emitter.textures.reserve(instance->textures.size());
        for (const CDBPtr<NDb::STexture>& texture_ref :
             instance->textures) {
            const NDb::STexture* texture = texture_ref.GetPtr();
            if (texture == nullptr) {
                continue;
            }
            AndroidParticleTexture snapshot;
            snapshot.path = NormalizeParticleTexturePath(texture);
            if (snapshot.path.empty()) {
                continue;
            }
            snapshot.width = std::max(texture->nWidth, 1);
            snapshot.height = std::max(texture->nHeight, 1);
            snapshot.additive =
                    texture->eConversionType ==
                    NDb::CONVERT_TRANSPARENT_ADD;
            emitter.textures.push_back(std::move(snapshot));
        }
        if (emitter.textures.empty()) {
            continue;
        }
        const float cycle_seconds =
                emitter.end_cycle_seconds > 0.0f
                ? emitter.end_cycle_seconds
                : std::max(scene_effect->fDuration, 0.1f);
        const float emitter_lifetime =
                emitter.cycle_count == 0
                ? 10.0f
                : std::max(emitter.time_offset_seconds, 0.0f) +
                        cycle_seconds *
                                static_cast<float>(
                                        std::max(
                                                emitter.cycle_count,
                                                1));
        lifetime_seconds =
                std::max(lifetime_seconds, emitter_lifetime);
        g_descriptor_particle_texture_count +=
                emitter.textures.size();
        emitters->push_back(std::move(emitter));
        ++g_descriptor_particle_emitter_count;
    }
    for (const CDBPtr<NDb::SLightInstance>& instance_ref :
         scene_effect->lights) {
        const NDb::SLightInstance* instance =
                instance_ref.GetPtr();
        if (instance == nullptr) {
            continue;
        }
        AndroidEffectLight light;
        light.offset_x = AI2Vis(instance->vPosition.x);
        light.offset_y = AI2Vis(instance->vPosition.y);
        light.offset_z = AI2Vis(instance->vPosition.z);
        light.scale =
                std::isfinite(instance->fScale)
                ? std::clamp(instance->fScale, 0.05f, 20.0f)
                : 1.0f;
        light.speed =
                std::isfinite(instance->fSpeed) &&
                        std::abs(instance->fSpeed) > 0.001f
                ? instance->fSpeed
                : 1.0f;
        light.time_offset_seconds =
                std::isfinite(instance->fOffset)
                ? instance->fOffset
                : 0.0f;
        light.end_cycle_seconds =
                std::isfinite(instance->fEndCycle)
                ? std::max(instance->fEndCycle, 0.0f)
                : 0.0f;
        light.cycle_count = instance->nCycleCount;
        const float cycle_seconds =
                light.end_cycle_seconds > 0.0f
                ? light.end_cycle_seconds
                : std::max(scene_effect->fDuration, 0.1f);
        const float light_lifetime =
                light.cycle_count == 0
                ? 10.0f
                : std::max(light.time_offset_seconds, 0.0f) +
                        cycle_seconds *
                                static_cast<float>(
                                        std::max(
                                                light.cycle_count,
                                                1));
        lifetime_seconds =
                std::max(lifetime_seconds, light_lifetime);
        lights->push_back(light);
        ++g_descriptor_effect_light_count;
    }
    return static_cast<uint32_t>(
            std::lround(
                    std::clamp(lifetime_seconds, 0.1f, 12.0f) *
                    1000.0f));
}

uint32_t BuildComplexEffectRecipe(
        const NDb::SComplexEffect* complex_effect,
        std::string* descriptor_id,
        std::vector<AndroidParticleEmitter>* emitters,
        std::vector<AndroidEffectLight>* lights) {
    if (complex_effect == nullptr ||
        descriptor_id == nullptr ||
        emitters == nullptr ||
        lights == nullptr) {
        return 0;
    }
    *descriptor_id =
            complex_effect->GetDBID().ToString().c_str();
    return AppendSceneEffectRecipe(
            complex_effect->GetSceneEffect(),
            emitters,
            lights);
}

UnitDeathEffectCandidates BuildUnitDeathEffectCandidates(
        const NDb::SUnitBaseRPGStats* stats) {
    UnitDeathEffectCandidates result;
    if (stats == nullptr ||
        stats->GetTypeID() != NDb::SMechUnitRPGStats::typeID) {
        return result;
    }
    const NDb::SMechUnitRPGStats* mechanized =
            static_cast<const NDb::SMechUnitRPGStats*>(stats);
    result.smoke_lifetime_millis = BuildComplexEffectRecipe(
            mechanized->pEffectSmoke.GetPtr(),
            &result.smoke_descriptor_id,
            &result.smoke_emitters,
            &result.smoke_lights);
    result.fatality_lifetime_millis = BuildComplexEffectRecipe(
            mechanized->pEffectFatality.GetPtr(),
            &result.fatality_descriptor_id,
            &result.fatality_emitters,
            &result.fatality_lights);
    return result;
}

const NDb::SComplexEffect* ResolveHitEffect(
        const SAINotifyHitInfo& info) {
    const NDb::SWeaponRPGStats* weapon = info.pWeapon.GetPtr();
    if (weapon == nullptr ||
        info.wShell >= weapon->shells.size()) {
        return nullptr;
    }
    const NDb::SWeaponRPGStats::SShell& shell =
            weapon->shells[info.wShell];
    switch (info.eHitType) {
        case SAINotifyHitInfo::EHT_HIT:
            return shell.pEffectHitDirect.GetPtr();
        case SAINotifyHitInfo::EHT_MISS:
            return shell.pEffectHitMiss.GetPtr();
        case SAINotifyHitInfo::EHT_REFLECT:
            return shell.pEffectHitReflect.GetPtr();
        case SAINotifyHitInfo::EHT_GROUND:
            return shell.pEffectHitGround.GetPtr();
        case SAINotifyHitInfo::EHT_WATER:
            return shell.pEffectHitWater.GetPtr();
        case SAINotifyHitInfo::EHT_AIR:
            return shell.pEffectHitAir.GetPtr();
        case SAINotifyHitInfo::EHT_NONE:
        default:
            return nullptr;
    }
}

// Mission sound. The effect descriptors the port already resolves for hits and
// explosions carry the shipped SComplexSoundDesc, and the audio backend
// already attenuates by distance, so the descriptor's own fMinDist/fMaxDist
// go straight through.
std::unordered_map<std::string, DecodedPcmClip> g_mission_sound_clips;
size_t g_mission_sound_played = 0;
size_t g_mission_sound_missing = 0;
std::unordered_map<std::string, bool> g_mission_sound_logged_paths;
uint32_t g_mission_sound_random = 0x2545f491u;

float MissionSfxVolume() {
    const std::string value(
            NStr::ToMBCS(NGlobal::GetVar(string("Sound.SFXVolume"), "0.5"))
                    .c_str());
    const float volume = static_cast<float>(std::atof(value.c_str()));
    return std::max(0.0f, std::min(volume, 1.0f));
}

uint32_t NextMissionSoundRandom() {
    g_mission_sound_random ^= g_mission_sound_random << 13;
    g_mission_sound_random ^= g_mission_sound_random >> 17;
    g_mission_sound_random ^= g_mission_sound_random << 5;
    return g_mission_sound_random;
}

const DecodedPcmClip* LoadMissionSound(const std::string& path) {
    const std::unordered_map<std::string, DecodedPcmClip>::const_iterator
            cached = g_mission_sound_clips.find(path);
    if (cached != g_mission_sound_clips.end()) {
        return cached->second.samples.empty() ? nullptr : &cached->second;
    }
    DecodedPcmClip clip;
    if (NVFS::GetMainVFS() != nullptr) {
        CFileStream stream(NVFS::GetMainVFS(), string(path.c_str()));
        const int byte_count = stream.GetSize();
        if (stream.IsOk() && byte_count > 0 &&
            stream.GetBuffer() != nullptr) {
            std::string error;
            DecodeWavToPcm16(
                    stream.GetBuffer(),
                    static_cast<size_t>(byte_count),
                    &clip,
                    &error);
        }
    }
    if (clip.samples.empty()) {
        ++g_mission_sound_missing;
    }
    const DecodedPcmClip& stored =
            g_mission_sound_clips.emplace(path, std::move(clip))
                    .first->second;
    return stored.samples.empty() ? nullptr : &stored;
}

void PlayComplexSound(
        const NDb::SComplexSoundDesc* sound,
        const CVec3& ai_position) {
    if (sound == nullptr || sound->sounds.empty()) {
        return;
    }
    const float volume = MissionSfxVolume();
    if (volume <= 0.0f) {
        return;
    }
    // The shipped entries carry a weight each; pick one of them the way a
    // varied set is meant to be heard rather than always the first.
    float total = 0.0f;
    for (const NDb::SComplexSoundDesc::SSoundStats& entry : sound->sounds) {
        total += std::max(entry.fProbability, 0.0f);
    }
    const NDb::SComplexSoundDesc::SSoundStats* chosen = &sound->sounds[0];
    if (total > 0.0f) {
        float ticket =
                static_cast<float>(NextMissionSoundRandom() & 0xffffffu) /
                static_cast<float>(0x1000000u) * total;
        for (const NDb::SComplexSoundDesc::SSoundStats& entry :
             sound->sounds) {
            ticket -= std::max(entry.fProbability, 0.0f);
            if (ticket <= 0.0f) {
                chosen = &entry;
                break;
            }
        }
    }
    if (!chosen->pPathName) {
        return;
    }
    const NDb::SSoundDesc* desc = chosen->pPathName.GetPtr();
    if (desc == nullptr) {
        return;
    }
    const std::string path(desc->szSoundPath.c_str());
    if (path.empty()) {
        return;
    }
    const DecodedPcmClip* clip = LoadMissionSound(path);
    if (clip == nullptr) {
        return;
    }
    const int channel = AudioBackend().play(clip->view(), false, 0);
    if (channel < 0) {
        return;
    }
    AudioBackend().set_volume(channel, volume);
    const float position[3] = {
            AI2Vis(ai_position.x),
            AI2Vis(ai_position.y),
            AI2Vis(ai_position.z)};
    const float velocity[3] = {0.0f, 0.0f, 0.0f};
    AudioBackend().set_spatial_position(
            channel,
            position,
            velocity,
            std::max(chosen->fMinDist, 0.1f),
            std::max(chosen->fMaxDist, chosen->fMinDist + 0.1f));
    ++g_mission_sound_played;
    if (g_mission_sound_logged_paths.size() < 6 &&
        g_mission_sound_logged_paths.emplace(path, true).second) {
        std::ostringstream report;
        report << "mission_sound=active; path=" << path
               << "; frames=" << clip->frame_count()
               << "; rate=" << clip->sample_rate
               << "; volume=" << volume
               << "; min=" << chosen->fMinDist
               << "; max=" << chosen->fMaxDist;
        PlatformRuntime::instance().log_info(report.str());
    }
}

void CaptureDescriptorSceneEffect(
        const NDb::SComplexEffect* complex_effect,
        int32_t victim_unit_id,
        const CVec3& position) {
    if (complex_effect != nullptr) {
        PlayComplexSound(complex_effect->pSoundEffect.GetPtr(), position);
    }
    TimedSceneEffect timed;
    timed.effect.victim_unit_id = victim_unit_id;
    timed.effect.x = AI2Vis(position.x);
    timed.effect.y = AI2Vis(position.y);
    timed.effect.z = AI2Vis(position.z);
    timed.effect.lifetime_millis = BuildComplexEffectRecipe(
            complex_effect,
            &timed.effect.descriptor_id,
            &timed.effect.emitters,
            &timed.effect.lights);
    if ((timed.effect.emitters.empty() &&
         timed.effect.lights.empty()) ||
        timed.effect.lifetime_millis == 0) {
        return;
    }
    timed.created_millis = g_timer_millis;
    timed.expires_millis =
            g_timer_millis + timed.effect.lifetime_millis;
    g_scene_effects.push_back(std::move(timed));
    ++g_descriptor_scene_effect_count;
    if (g_descriptor_scene_effect_count == 1) {
        const AndroidSceneEffect& effect =
                g_scene_effects.back().effect;
        PlatformRuntime::instance().log_info(
                std::string("descriptor_scene_effect=") +
                effect.descriptor_id +
                "; emitters=" +
                std::to_string(effect.emitters.size()) +
                "; lights=" +
                std::to_string(effect.lights.size()));
    }
}

void CapturePresentationCorpse(
        int32_t unit_id,
        const CVec3& position,
        const NDb::SUnitBaseRPGStats* stats = nullptr,
        int die_animation_param =
                std::numeric_limits<int>::min()) {
    const auto previous = g_last_presentation_entities.find(unit_id);
    if (previous == g_last_presentation_entities.end()) {
        return;
    }
    const bool infantry =
            (previous->second.flags &
             BK2_PRESENTATION_ENTITY_INFANTRY) != 0;
    const bool mechanized =
            (previous->second.flags &
             BK2_PRESENTATION_ENTITY_MECHANIZED) != 0;
    if ((!infantry && !mechanized) ||
        (previous->second.flags &
         BK2_PRESENTATION_ENTITY_FORMATION) != 0) {
        return;
    }
    const bool new_corpse =
            g_presentation_corpses.find(unit_id) ==
            g_presentation_corpses.end();
    if (new_corpse) {
        PresentationCorpse corpse;
        corpse.entity = previous->second;
        corpse.entity.flags &=
                ~(BK2_PRESENTATION_ENTITY_ALIVE |
                  BK2_PRESENTATION_ENTITY_SELECTABLE |
                  BK2_PRESENTATION_ENTITY_MOVABLE |
                  BK2_PRESENTATION_ENTITY_SELECTED |
                  BK2_PRESENTATION_ENTITY_TARGETED |
                  BK2_PRESENTATION_ENTITY_MOVING |
                  BK2_PRESENTATION_ENTITY_ATTACKING);
        corpse.entity.flags |= BK2_PRESENTATION_ENTITY_DEAD;
        corpse.entity.hit_points = 0.0f;
        corpse.entity.x = AI2Vis(position.x);
        corpse.entity.y = AI2Vis(position.y);
        corpse.entity.z = AI2Vis(position.z);
        corpse.expires_millis = g_timer_millis + 10000;
        g_presentation_corpses[unit_id] = corpse;
        ++g_presentation_death_count;
        PlatformRuntime::instance().log_info(
                std::string(
                        mechanized
                                ? "mechanized_death_presentation="
                                : "infantry_death_presentation=") +
                std::to_string(unit_id));
    }
    if (mechanized) {
        if (stats != nullptr) {
            g_unit_death_effect_candidates[unit_id] =
                    BuildUnitDeathEffectCandidates(stats);
        }
        const auto candidates =
                g_unit_death_effect_candidates.find(unit_id);
        const bool exact_animation =
                die_animation_param !=
                std::numeric_limits<int>::min();
        bool fatality = false;
        if (exact_animation && die_animation_param != -1) {
            const NDb::EAnimationType animation =
                    static_cast<NDb::EAnimationType>(
                            (die_animation_param >> 16) &
                            0x00000fff);
            fatality =
                    animation ==
                    NDb::ANIMATION_DEATH_FATALITY;
        }
        auto existing = std::find_if(
                g_destruction_effects.begin(),
                g_destruction_effects.end(),
                [unit_id](const TimedDestructionEffect& effect) {
                    return effect.effect.unit_id == unit_id;
                });
        const bool new_effect =
                existing == g_destruction_effects.end();
        if (new_effect ||
            (exact_animation &&
             !existing->exact_animation_recipe)) {
            TimedDestructionEffect destruction;
            destruction.effect.unit_id = unit_id;
            destruction.effect.x =
                    g_presentation_corpses[unit_id].entity.x;
            destruction.effect.y =
                    g_presentation_corpses[unit_id].entity.y;
            destruction.effect.z =
                    g_presentation_corpses[unit_id].entity.z;
            destruction.effect.lifetime_millis = 10000u;
            destruction.exact_animation_recipe =
                    exact_animation;
            if (candidates !=
                g_unit_death_effect_candidates.end()) {
                const UnitDeathEffectCandidates& recipe =
                        candidates->second;
                const bool use_fatality =
                        fatality &&
                        !recipe.fatality_emitters.empty();
                destruction.effect.descriptor_id =
                        use_fatality
                        ? recipe.fatality_descriptor_id
                        : recipe.smoke_descriptor_id;
                destruction.effect.emitters =
                        use_fatality
                        ? recipe.fatality_emitters
                        : recipe.smoke_emitters;
                destruction.effect.lights =
                        use_fatality
                        ? recipe.fatality_lights
                        : recipe.smoke_lights;
                const uint32_t recipe_lifetime =
                        use_fatality
                        ? recipe.fatality_lifetime_millis
                        : recipe.smoke_lifetime_millis;
                if (recipe_lifetime > 0) {
                    destruction.effect.lifetime_millis =
                            recipe_lifetime;
                }
            }
            destruction.effect.uses_fallback_recipe =
                    destruction.effect.emitters.empty();
            destruction.created_millis = g_timer_millis;
            destruction.expires_millis =
                    g_timer_millis +
                    destruction.effect.lifetime_millis;
            if (new_effect) {
                g_destruction_effects.push_back(
                        std::move(destruction));
                ++g_mechanized_destruction_effect_count;
            } else {
                *existing = std::move(destruction);
            }
            if (g_mechanized_destruction_effect_count == 1) {
                const AndroidDestructionEffect& effect =
                        new_effect
                        ? g_destruction_effects.back().effect
                        : existing->effect;
                PlatformRuntime::instance().log_info(
                        std::string(
                                "destruction_effect=mechanized; unit=") +
                        std::to_string(unit_id) +
                        "; descriptor=" +
                        (effect.descriptor_id.empty()
                                 ? "fallback"
                                 : effect.descriptor_id) +
                        "; emitters=" +
                        std::to_string(effect.emitters.size()) +
                        "; lights=" +
                        std::to_string(effect.lights.size()));
            }
        }
    }
}

const NDb::SWeaponRPGStats* ResolveMechShotWeapon(
        int unit_id,
        int platform_index,
        int gun_index) {
    CAIUnit* unit = CAIUnit::GetUnitByUniqueID(unit_id);
    if (unit == nullptr) {
        return nullptr;
    }
    const NDb::SMechUnitRPGStats* mech_stats =
            dynamic_cast<const NDb::SMechUnitRPGStats*>(unit->GetStats());
    if (mech_stats == nullptr ||
        platform_index < 0 ||
        static_cast<size_t>(platform_index) >= mech_stats->platforms.size()) {
        return nullptr;
    }
    const NDb::SMechUnitRPGStats::SPlatform& platform =
            mech_stats->platforms[platform_index];
    if (gun_index < 0 ||
        static_cast<size_t>(gun_index) >= platform.guns.size()) {
        return nullptr;
    }
    return platform.guns[gun_index].pWeapon.GetPtr();
}

// Gun fire carries its own complex effect on the shell, the same record the
// desktop build plays the muzzle report from.
const NDb::SComplexSoundDesc* ResolveGunFireSound(
        const NDb::SWeaponRPGStats* weapon,
        int shell_index) {
    if (weapon == nullptr ||
        shell_index < 0 ||
        static_cast<size_t>(shell_index) >= weapon->shells.size()) {
        return nullptr;
    }
    const NDb::SComplexEffect* effect =
            weapon->shells[shell_index].pEffectGunFire.GetPtr();
    return effect == nullptr ? nullptr : effect->pSoundEffect.GetPtr();
}

void CaptureCombatEffect(
        int32_t source_unit_id,
        const CVec3& destination,
        AndroidCombatEffectType type) {
    CAIUnit* source = CAIUnit::GetUnitByUniqueID(source_unit_id);
    if (source == nullptr) {
        return;
    }
    const CVec3& source_center = source->GetCenter();
    TimedCombatEffect timed;
    timed.effect.type = type;
    timed.effect.source_unit_id = source_unit_id;
    timed.effect.source_x = AI2Vis(source_center.x);
    timed.effect.source_y = AI2Vis(source_center.y);
    timed.effect.source_z = AI2Vis(source->GetVisZ());
    timed.effect.destination_x = AI2Vis(destination.x);
    timed.effect.destination_y = AI2Vis(destination.y);
    timed.effect.destination_z = AI2Vis(destination.z);
    timed.effect.lifetime_millis =
            type == AndroidCombatEffectType::InfantryShot ? 180u : 260u;
    timed.created_millis = g_timer_millis;
    timed.expires_millis =
            g_timer_millis + timed.effect.lifetime_millis;
    g_combat_effects.push_back(timed);

    uint64_t* counter =
            type == AndroidCombatEffectType::InfantryShot
            ? &g_infantry_shot_effect_count
            : &g_mechanized_shot_effect_count;
    ++*counter;
    if (*counter == 1) {
        PlatformRuntime::instance().log_info(
                std::string("combat_effect=") +
                (type == AndroidCombatEffectType::InfantryShot
                         ? "infantry_shot"
                         : "mechanized_shot") +
                "; source=" + std::to_string(source_unit_id));
    }
}

void PruneExpiredCombatEffects() {
    g_combat_effects.erase(
            std::remove_if(
                    g_combat_effects.begin(),
                    g_combat_effects.end(),
                    [](const TimedCombatEffect& effect) {
                        return effect.expires_millis <= g_timer_millis;
                    }),
            g_combat_effects.end());
    g_scene_effects.erase(
            std::remove_if(
                    g_scene_effects.begin(),
                    g_scene_effects.end(),
                    [](const TimedSceneEffect& effect) {
                        return effect.expires_millis <= g_timer_millis;
                    }),
            g_scene_effects.end());
    g_destruction_effects.erase(
            std::remove_if(
                    g_destruction_effects.begin(),
                    g_destruction_effects.end(),
                    [](const TimedDestructionEffect& effect) {
                        return effect.expires_millis <= g_timer_millis;
                    }),
            g_destruction_effects.end());
}

const char* LegacyNotificationTextFallback(
        NDb::ENotificationType type) {
    switch (type) {
        case NDb::NTF_OBJECTIVE_RECEIVED:
            return "/Other/Text/Game/Mission/Notifications/"
                   "Objective_Received/Text.txt";
        case NDb::NTF_OBJECTIVE_COMPLETED:
            return "/Other/Text/Game/Mission/Notifications/"
                   "Objective_Completed/Text.txt";
        case NDb::NTF_OBJECTIVE_FAILED:
            return "/Other/Text/Game/Mission/Notifications/"
                   "Objective_Failed/Text.txt";
        case NDb::NTF_REINFORCEMENT_ARRIVED:
            return "/Other/Text/Game/Mission/Notifications/"
                   "ReinfArrived/Text.txt";
        default:
            return "";
    }
}

bool QueueLegacyHudNotification(
        NDb::ENotificationType type,
        const std::string& suffix_file_ref = std::string()) {
    const NDb::SGameRoot* root = NGameX::GetGameRoot();
    std::string text_ref;
    if (root != nullptr) {
        for (const CDBPtr<NDb::SNotification>& notification :
             root->notifications) {
            if (!notification || notification->eType != type) {
                continue;
            }
            text_ref = notification->szTextFileRef.c_str();
            break;
        }
    }
    if (text_ref.empty()) {
        text_ref = LegacyNotificationTextFallback(type);
    }
    if (text_ref.empty()) {
        PlatformRuntime::instance().log_warn(
                std::string("mission_hud_notification_missing=") +
                std::to_string(static_cast<int>(type)) +
                "; error=no_text_ref");
        return false;
    }
    if (!QueueMissionHudNotification(
                text_ref,
                5000,
                suffix_file_ref)) {
        PlatformRuntime::instance().log_warn(
                std::string("mission_hud_notification_failed=") +
                std::to_string(static_cast<int>(type)) +
                "; text_ref=" + text_ref);
        return false;
    }
    ++g_hud_notification_count;
    PlatformRuntime::instance().log_info(
            std::string("mission_hud_notification=") +
            std::to_string(static_cast<int>(type)) +
            "; text_ref=" + text_ref);
    return true;
}

// Turret aim the AI last commanded for a unit's platform, interpolated toward
// its end time the way CMOUnitMechanical::AIUpdateTurretTurn animates the bone.
struct AndroidTurretAim {
    float yaw_radians = 0.0f;
    float pitch_radians = 0.0f;
    // The two axes arrive in separate notifications. Posing a yaw that never
    // arrived would swing the turret to world north.
    bool has_yaw = false;
    bool has_pitch = false;
};

// The aim arrives per platform, and a unit can carry a dozen of them, so the
// key has to say which one turned.
std::unordered_map<uint64_t, AndroidTurretAim> g_turret_aim;

uint64_t TurretAimKey(int unit_id, int platform) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(unit_id)) << 16) |
            static_cast<uint64_t>(static_cast<uint16_t>(platform));
}
// Rotating platform bone per unit stats record, cached while entities are
// published so the renderer can pose that subtree.
// Keyed by the stats path hash: GetRecordID() is -1 for these runtime stats,
// so every unit would otherwise collapse onto one entry.
std::unordered_map<uint64_t, std::string> g_turret_bones;
std::unordered_map<uint64_t, std::string> g_gun_bones;
std::unordered_map<uint64_t, int> g_turret_platforms;
size_t g_turret_update_count = 0;
// Distance each unit has covered, accumulated from its own centre so wheels
// roll at the speed the simulation actually moves the vehicle.
struct AndroidUnitOdometer {
    float x = 0.0f;
    float y = 0.0f;
    float distance = 0.0f;
    bool seeded = false;
};

std::unordered_map<int, AndroidUnitOdometer> g_unit_odometers;
size_t g_turret_bone_log_count = 0;

// AI angles are a 16-bit turn; the desktop converts with AI2VisRad.
float AiAngleToRadians(WORD angle) {
    constexpr float kTwoPi = 6.28318530717958647692f;
    return static_cast<float>(angle) * kTwoPi / 65536.0f;
}

const std::string& LegacyMechTurretBone(uint64_t stats_path_hash);
const std::string& LegacyMechGunBone(uint64_t stats_path_hash);
int LegacyMechTurretPlatform(uint64_t stats_path_hash);
int SelectMechTurretPlatform(const NDb::SMechUnitRPGStats* mech_stats);

void DrainLegacyClientUpdates() {
    if (g_ai_logic == nullptr) {
        return;
    }
    g_ai_logic->PrepareUpdates();
    while (CPtr<CObjectBase> update = g_ai_logic->GetUpdate()) {
        ++g_client_update_count;
        SAIDeadUnitUpdate* dead =
                dynamic_cast<SAIDeadUnitUpdate*>(update.GetPtr());
        if (dead != nullptr) {
            const SAINotifyPlacement& placement = dead->placement;
            CapturePresentationCorpse(
                    dead->nDeadObj,
                    placement.bNewFormat
                            ? placement.vPlacement
                            : CVec3(
                                      placement.center.x,
                                      placement.center.y,
                                      placement.z),
                    nullptr,
                    dead->dieAnimation.nParam);
            continue;
        }
        SAITurretUpdate* turret =
                dynamic_cast<SAITurretUpdate*>(update.GetPtr());
        if (turret != nullptr) {
            ++g_turret_update_count;
            AndroidTurretAim& aim = g_turret_aim[TurretAimKey(
                    turret->info.nObjUniqueID,
                    turret->info.nPlantform)];
            const float angle = AiAngleToRadians(turret->info.wAngle);
            if (turret->eUpdateType == ACTION_NOTIFY_TURRET_VERT_TURN) {
                aim.pitch_radians = angle;
                aim.has_pitch = true;
            } else {
                aim.yaw_radians = angle;
                aim.has_yaw = true;
            }
            continue;
        }
        SAIHitUpdate* hit =
                dynamic_cast<SAIHitUpdate*>(update.GetPtr());
        if (hit != nullptr) {
            CaptureDescriptorSceneEffect(
                    ResolveHitEffect(hit->info),
                    hit->info.nVictimUniqueID,
                    hit->info.explCoord);
            continue;
        }
        SAIInfantryShotUpdate* infantry_shot =
                dynamic_cast<SAIInfantryShotUpdate*>(update.GetPtr());
        if (infantry_shot != nullptr) {
            CaptureCombatEffect(
                    infantry_shot->info.nObjUniqueID,
                    infantry_shot->info.vDestPos,
                    AndroidCombatEffectType::InfantryShot);
            PlayComplexSound(
                    ResolveGunFireSound(
                            infantry_shot->info.pWeapon.GetPtr(),
                            infantry_shot->info.cShell),
                    infantry_shot->info.vDestPos);
            continue;
        }
        SAIMechShotUpdate* mechanized_shot =
                dynamic_cast<SAIMechShotUpdate*>(update.GetPtr());
        if (mechanized_shot != nullptr) {
            CaptureCombatEffect(
                    mechanized_shot->info.nObjUniqueID,
                    mechanized_shot->info.vDestPos,
                    AndroidCombatEffectType::MechanizedShot);
            // A mech shot names the platform and gun rather than the weapon,
            // so the weapon comes back through the unit's own stats.
            PlayComplexSound(
                    ResolveGunFireSound(
                            ResolveMechShotWeapon(
                                    mechanized_shot->info.nObjUniqueID,
                                    mechanized_shot->info.cPlatform,
                                    mechanized_shot->info.cGun),
                            mechanized_shot->info.cShell),
                    mechanized_shot->info.vDestPos);
            continue;
        }
        SAIFeedbackUpdate* feedback =
                dynamic_cast<SAIFeedbackUpdate*>(update.GetPtr());
        if (feedback == nullptr) {
            continue;
        }
        if (feedback->info.feedBackType ==
            EFB_REINFORCEMENT_CENTER_LOCAL_PLAYER) {
            QueueLegacyHudNotification(
                    NDb::NTF_REINFORCEMENT_ARRIVED);
            continue;
        }
        if (feedback->info.feedBackType != EFB_OBJECTIVE_CHANGED) {
            continue;
        }

        const int objective = feedback->info.nParam >> 8;
        const int state = feedback->info.nParam & 0xff;
        if (state < EMOS_MIN || state > EMOS_MAX) {
            continue;
        }
        if (g_scenario_tracker != nullptr &&
            objective >= 0 &&
            objective < g_scenario_tracker->GetObjectiveCount()) {
            g_scenario_tracker->SetObjectiveState(
                    objective,
                    static_cast<EMissionObjectiveState>(state));
        }
        const MissionRuntimeResult result =
                SetMissionObjectiveState(objective, state);
        if (result.ok) {
            ++g_objective_update_count;
            std::string objective_text_ref;
            if (objective >= 0 &&
                objective <
                        static_cast<int>(
                                result.state.objectives.size())) {
                const MissionObjectiveState& objective_state =
                        result.state.objectives[objective];
                objective_text_ref =
                        objective_state.header_ref.empty()
                        ? objective_state.description_ref
                        : objective_state.header_ref;
            }
            if (state == EMOS_RECEIVED) {
                QueueLegacyHudNotification(
                        NDb::NTF_OBJECTIVE_RECEIVED,
                        objective_text_ref);
            } else if (state == EMOS_COMPLETED) {
                QueueLegacyHudNotification(
                        NDb::NTF_OBJECTIVE_COMPLETED,
                        objective_text_ref);
            } else if (state == EMOS_FAILED) {
                QueueLegacyHudNotification(
                        NDb::NTF_OBJECTIVE_FAILED,
                        objective_text_ref);
            }
            PlatformRuntime::instance().log_info(
                    std::string("mission_objective_update=") +
                    std::to_string(objective) +
                    "; state=" + std::to_string(state));
        } else {
            PlatformRuntime::instance().log_warn(
                    std::string("mission_objective_update_failed=") +
                    std::to_string(objective) +
                    "; state=" + std::to_string(state) +
                    "; error=" + result.error);
        }
    }
}

void PublishPresentationEntities() {
    std::vector<Bk2PresentationEntity> entities;
    entities.reserve(
            static_cast<size_t>(std::max(g_ai_unit_total_count, 0)) +
            g_presentation_corpses.size());
    for (CGlobalIter iter(0, ANY_PARTY); !iter.IsFinished(); iter.Iterate()) {
        CAIUnit* unit = *iter;
        if (unit == nullptr) {
            continue;
        }
        const BYTE player_party = theDipl.GetMyParty();
        if (unit->GetParty() != player_party &&
            !unit->IsVisible(player_party)) {
            continue;
        }
        uint32_t flags = 0;
        if (unit->IsAlive()) {
            flags |= BK2_PRESENTATION_ENTITY_ALIVE;
        }
        if (unit->IsSelectable()) {
            flags |= BK2_PRESENTATION_ENTITY_SELECTABLE;
        }
        if (unit->CanMove()) {
            flags |= BK2_PRESENTATION_ENTITY_MOVABLE;
        }
        if (IsSelectedLegacyUnitId(unit->GetUniqueIdQU())) {
            flags |= BK2_PRESENTATION_ENTITY_SELECTED;
        }
        if (unit->GetUniqueIdQU() == g_attack_target_unit_id) {
            flags |= BK2_PRESENTATION_ENTITY_TARGETED;
        }
        if (unit->IsMech()) {
            flags |= BK2_PRESENTATION_ENTITY_MECHANIZED;
        }
        if (unit->IsInfantry()) {
            flags |= BK2_PRESENTATION_ENTITY_INFANTRY;
        }
        if (unit->IsFormation()) {
            flags |= BK2_PRESENTATION_ENTITY_FORMATION;
        }
        const CSoldier* soldier = dynamic_cast<const CSoldier*>(unit);
        if (soldier != nullptr && soldier->IsLying()) {
            flags |= BK2_PRESENTATION_ENTITY_LYING;
        }
        IUnitState* state = unit->GetState();
        const EUnitStateNames state_name =
                state == nullptr ? EUSN_ERROR : state->GetName();
        if ((state != nullptr && state->IsAttackingState()) ||
            (IsSelectedLegacyUnitId(unit->GetUniqueIdQU()) &&
             g_attack_target_unit_id >= 0)) {
            flags |= BK2_PRESENTATION_ENTITY_ATTACKING;
        }
        if (state_name == EUSN_MOVE ||
            state_name == EUSN_MOVE_BY_FORMATION ||
            state_name == EUSN_MOVE_TO_GRID ||
            state_name == EUSN_MOVE_TO_RESUPPLY_CELL ||
            state_name == EUSN_PATROL ||
            state_name == EUSN_PLANE_PATROL) {
            flags |= BK2_PRESENTATION_ENTITY_MOVING;
        }
        const CVec3& center = unit->GetCenter();
        const NDb::SUnitBaseRPGStats* stats = unit->GetStats();
        const int turret_platform = LegacyMechTurretPlatform(
                StatsPathHash(unit->GetStats()));
        const std::unordered_map<uint64_t, AndroidTurretAim>::const_iterator
                aim = turret_platform < 0
                ? g_turret_aim.end()
                : g_turret_aim.find(TurretAimKey(
                          unit->GetUniqueIdQU(),
                          turret_platform));
        AndroidUnitOdometer& odometer =
                g_unit_odometers[unit->GetUniqueIdQU()];
        if (odometer.seeded) {
            const float step_x = center.x - odometer.x;
            const float step_y = center.y - odometer.y;
            odometer.distance +=
                    std::sqrt(step_x * step_x + step_y * step_y);
        }
        odometer.x = center.x;
        odometer.y = center.y;
        odometer.seeded = true;
        const uint64_t stats_key = StatsPathHash(stats);
        if (stats != nullptr &&
            g_turret_bones.find(stats_key) == g_turret_bones.end()) {
            const NDb::SMechUnitRPGStats* mech_stats =
                    dynamic_cast<const NDb::SMechUnitRPGStats*>(stats);
            // Platform 0 is the hull: its rotate point is the skeleton root
            // and turning it would swing the whole vehicle. The turret is the
            // first platform that names a different bone.
            const int platform = SelectMechTurretPlatform(mech_stats);
            g_turret_platforms[stats_key] = platform;
            const std::string bone = platform >= 0
                    ? std::string(mech_stats->platforms[platform]
                                          .szRotatePoint.c_str())
                    : std::string();
            g_turret_bones[stats_key] = bone;
            // The gun carries its own rotate point: that is the pivot the
            // barrel elevates about, separate from the platform's yaw pivot.
            const std::string gun_bone =
                    platform >= 0 &&
                            !mech_stats->platforms[platform].guns.empty()
                            ? std::string(mech_stats->platforms[platform]
                                                  .guns[0]
                                                  .szRotatePoint.c_str())
                            : std::string();
            g_gun_bones[stats_key] = gun_bone;
            if (!bone.empty() && g_turret_bone_log_count < 4) {
                std::ostringstream report;
                report << "mech_turret_bone=" << bone
                       << "; platform=" << platform
                       << "; stats_key=" << stats_key
                       << "; platforms="
                       << (mech_stats == nullptr
                                   ? 0
                                   : mech_stats->platforms.size())
                       << "; gun_bone=" << gun_bone
                       << "; turret_updates=" << g_turret_update_count;
            }
        }
        Bk2PresentationEntity entity{
                unit->GetUniqueIdQU(),
                static_cast<int32_t>(unit->GetPlayer()),
                flags,
                AI2Vis(center.x),
                AI2Vis(center.y),
                AI2Vis(unit->GetVisZ()),
                unit->GetDir(),
                unit->GetHitPoints(),
                stats == nullptr ? 1.0f : stats->fMaxHP,
                StatsPathHash(stats),
                stats == nullptr ? -1 : stats->GetRecordID(),
                GeometryRecordId(stats),
                stats == nullptr ? 1.0f : stats->fSelectionScale,
                aim == g_turret_aim.end() ? 0.0f : aim->second.yaw_radians,
                aim == g_turret_aim.end() || !aim->second.has_pitch
                        ? 0.0f
                        : aim->second.pitch_radians,
                aim != g_turret_aim.end() && aim->second.has_yaw ? 1u : 0u,
                odometer.distance};
        entities.push_back(entity);
        if ((entity.flags & BK2_PRESENTATION_ENTITY_ALIVE) != 0) {
            g_last_presentation_entities[entity.id] = entity;
        }
    }
    for (auto corpse = g_presentation_corpses.begin();
         corpse != g_presentation_corpses.end();) {
        if (corpse->second.expires_millis <= g_timer_millis) {
            corpse = g_presentation_corpses.erase(corpse);
            continue;
        }
        entities.push_back(corpse->second.entity);
        ++corpse;
    }
    bk2::presentation::PublishEntities(std::move(entities));
}

bool InitializeScenarioTracker(
        const NDb::SMapInfo* map,
        int campaign_index,
        int chapter_index,
        int difficulty,
        std::string* error) {
    IScenarioTracker* tracker = CreateScenarioTracker();
    if (tracker == nullptr) {
        *error = "scenario_tracker_create_failed";
        return false;
    }
    g_scenario_tracker = tracker;
    NSingleton::RegisterSingleton(
            tracker,
            IScenarioTracker::tidTypeID);
    NSingleton::RegisterSingleton(
            tracker,
            IAIScenarioTracker::tidTypeID);

    const NDb::SGameRoot* root = NGameX::GetGameRoot();
    const NDb::SCampaign* campaign = nullptr;
    if (root != nullptr &&
        campaign_index >= 0 &&
        campaign_index < static_cast<int>(root->campaigns.size())) {
        campaign = root->campaigns[campaign_index].GetPtr();
    }

    if (campaign == nullptr) {
        tracker->CustomMissionStart(map, difficulty, false);
        return true;
    }

    tracker->CampaignStart(campaign, difficulty, false, false);
    const int last_chapter = std::min(
            chapter_index,
            static_cast<int>(campaign->chapters.size()) - 1);
    for (int index = 0; index <= last_chapter; ++index) {
        tracker->NextChapter();
    }
    tracker->MissionStart(map);
    return true;
}

bool FeedTerrainObserver(
        const NDb::SMapInfo* map,
        const STerrainInfo& info,
        ITerraAIObserver* observer,
        std::string* error) {
    if (map->pTerraSet.IsEmpty()) {
        *error = "ai_terrain_set_missing";
        return false;
    }

    const int database_type_count =
            static_cast<int>(map->pTerraSet->terraTypes.size());
    const int required_type_count =
            std::max(database_type_count, RequiredTerrainTypeCount(info));
    vector<NDb::STerrainAIProperties> properties;
    properties.reserve(required_type_count);
    for (int index = 0; index < required_type_count; ++index) {
        if (index < database_type_count &&
            map->pTerraSet->terraTypes[index]) {
            properties.push_back(
                    map->pTerraSet->terraTypes[index]->aIProperty);
        } else {
            properties.push_back(DefaultTerrainAIProperties());
        }
    }
    if (properties.empty()) {
        *error = "ai_terrain_types_missing";
        return false;
    }
    observer->SetTerraTypes(properties);
    g_terrain_type_count = static_cast<int>(properties.size());

    const int map_width = map->nNumPatchesX * AI_TILES_IN_PATCH;
    const int map_height = map->nNumPatchesY * AI_TILES_IN_PATCH;
    g_terrain_grid_width = std::min(
            info.tileTerraMap.GetSizeX(),
            map_width / 2);
    g_terrain_grid_height = std::min(
            info.tileTerraMap.GetSizeY(),
            map_height / 2);
    if (g_terrain_grid_width <= 0 || g_terrain_grid_height <= 0) {
        *error = "ai_terrain_type_grid_missing";
        return false;
    }

    CArray2D<BYTE> terrain_types(
            g_terrain_grid_width,
            g_terrain_grid_height);
    for (int y = 0; y < g_terrain_grid_height; ++y) {
        for (int x = 0; x < g_terrain_grid_width; ++x) {
            terrain_types[y][x] = IsSeaTile(info, x, y)
                    ? 0xff
                    : info.tileTerraMap[y][x];
        }
    }
    observer->UpdateTypes(
            0,
            0,
            g_terrain_grid_width,
            g_terrain_grid_height,
            terrain_types);

    const int height_width = HeightWidth(info);
    const int height_height = HeightHeight(info);
    if (height_width <= 0 || height_height <= 0) {
        *error = "ai_terrain_heights_missing";
        return false;
    }
    CArray2D<float> heights(height_width, height_height);
    for (int y = 0; y < height_height; ++y) {
        for (int x = 0; x < height_width; ++x) {
            heights[y][x] = std::max(
                    Vis2AI(FullVisualHeight(info, x, y)),
                    0.0f);
        }
    }
    observer->UpdateHeights(
            0,
            0,
            height_width,
            height_height,
            heights);

    g_terrain_feature_count = 0;
    for (const NDb::SVSOInstance& road : map->roads) {
        if (road.pDescriptor && road.points.size() >= 2) {
            observer->AddRoad(&road);
            ++g_terrain_feature_count;
        }
    }
    for (const NDb::SVSOInstance& river : map->rivers) {
        if (river.pDescriptor && river.points.size() >= 2) {
            observer->AddRiver(&river);
            ++g_terrain_feature_count;
        }
    }
    for (const NDb::SVSOInstance& crag : map->crags) {
        if (crag.pDescriptor && crag.points.size() >= 2) {
            observer->AddCrag(&crag);
            ++g_terrain_feature_count;
        }
    }
    if (map->bHasCoast &&
        map->coast.pDescriptor &&
        map->coast.points.size() >= 2) {
        observer->AddWaterLine(&map->coast, false);
        ++g_terrain_feature_count;
    }
    for (const NDb::SVSOInstance& lake : map->lakes) {
        if (lake.pDescriptor && lake.points.size() >= 2) {
            observer->AddWaterLine(&lake, true);
            ++g_terrain_feature_count;
        }
    }
    observer->FinalizeUpdates();
    return true;
}

int LegacyMechTurretPlatform(uint64_t stats_path_hash) {
    const std::unordered_map<uint64_t, int>::const_iterator found =
            g_turret_platforms.find(stats_path_hash);
    return found == g_turret_platforms.end() ? -1 : found->second;
}

int SelectMechTurretPlatform(const NDb::SMechUnitRPGStats* mech_stats) {
    if (mech_stats == nullptr || mech_stats->platforms.size() < 2) {
        return -1;
    }
    const std::string hull(mech_stats->platforms[0].szRotatePoint.c_str());
    for (size_t index = 1; index < mech_stats->platforms.size(); ++index) {
        const std::string bone(
                mech_stats->platforms[index].szRotatePoint.c_str());
        if (!bone.empty() && bone != hull) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

void ResetReportState() {
    g_ready = false;
    g_ai_logic = nullptr;
    g_timer_millis = 1;
    g_segment_count = 0;
    g_unit_count = 0;
    g_terrain_type_count = 0;
    g_terrain_grid_width = 0;
    g_terrain_grid_height = 0;
    g_terrain_feature_count = 0;
    g_map_player_count = 0;
    g_diplomacy_count = 0;
    g_start_unit_object_count = 0;
    g_ai_unit_total_count = 0;
    g_ai_unit_alive_count = 0;
    g_ai_unit_ref_valid_count = 0;
    g_normalized_rpg_stats_count = 0;
    g_normalized_unit_stats_count = 0;
    g_normalized_squad_stats_count = 0;
    g_normalize_skipped_no_visual_count = 0;
    g_missing_unit_payload_refs.clear();
    g_missing_squad_payload_refs.clear();
    g_client_update_count = 0;
    g_objective_update_count = 0;
    g_hud_notification_count = 0;
    g_combat_effects.clear();
    g_scene_effects.clear();
    g_destruction_effects.clear();
    g_unit_death_effect_candidates.clear();
    g_infantry_shot_effect_count = 0;
    g_mechanized_shot_effect_count = 0;
    g_mechanized_destruction_effect_count = 0;
    g_descriptor_scene_effect_count = 0;
    g_descriptor_particle_emitter_count = 0;
    g_descriptor_particle_texture_count = 0;
    g_descriptor_effect_light_count = 0;
    g_forwarded_unit_kill_count = 0;
    g_forwarded_unit_kill_error_count = 0;
    g_last_presentation_entities.clear();
    g_presentation_corpses.clear();
    g_turret_aim.clear();
    g_turret_bones.clear();
    g_gun_bones.clear();
    g_turret_platforms.clear();
    g_turret_update_count = 0;
    g_unit_odometers.clear();
    g_turret_bone_log_count = 0;
    g_mission_sound_clips.clear();
    g_mission_sound_played = 0;
    g_mission_sound_missing = 0;
    g_mission_sound_logged_paths.clear();
    g_presentation_death_count = 0;
    g_mission_outcome.store(kLegacyMissionRunning);
    g_pending_mission_outcome.store(kLegacyMissionRunning);
    g_stage = "not_started";
}

}

void Bk2AndroidOnUnitDead(CCommonUnit* unit) {
    if (unit == nullptr) {
        return;
    }
    bk2::android::CapturePresentationCorpse(
            unit->GetUniqueIdQU(),
            unit->GetCenter(),
            dynamic_cast<const NDb::SUnitBaseRPGStats*>(
                    unit->GetStats()));
}

void Bk2AndroidOnUnitKilled(
        int player,
        int killed_player,
        float experience_price,
        int killer_reinforcement_type,
        int killed_reinforcement_type,
        bool infantry_kill) {
    const int rounded_experience_price = static_cast<int>(
            std::lround(std::max(0.0f, experience_price)));
    const MissionRuntimeResult result = RegisterUnitKill(
            player,
            -1,
            killer_reinforcement_type,
            killed_player,
            -1,
            killed_reinforcement_type,
            rounded_experience_price,
            infantry_kill);
    if (result.ok) {
        ++g_forwarded_unit_kill_count;
        if (g_forwarded_unit_kill_count == 1) {
            PlatformRuntime::instance().log_info(
                    std::string("campaign_kill_forwarded=true; player=") +
                    std::to_string(player) +
                    "; killed_player=" +
                    std::to_string(killed_player) +
                    "; experience=" +
                    std::to_string(rounded_experience_price) +
                    "; mission_kills=" +
                    std::to_string(result.state.mission_kill_events) +
                    "; campaign_units_killed=" +
                    std::to_string(result.state.campaign_units_killed) +
                    "; player_xp_added=" +
                    std::to_string(result.state.player_xp_added));
        }
        return;
    }

    ++g_forwarded_unit_kill_error_count;
    if (g_forwarded_unit_kill_error_count == 1) {
        PlatformRuntime::instance().log_info(
                std::string("campaign_kill_forwarded=false; error=") +
                result.error);
    }
}

bool InitializeLegacyGameRuntime(
        const NDb::SMapInfo* map,
        const STerrainInfo& terrain_info,
        int campaign_index,
        int chapter_index,
        int difficulty,
        std::string* error) {
    ShutdownLegacyGameRuntime();
    if (map == nullptr) {
        *error = "legacy_game_map_missing";
        return false;
    }
    const NDb::SAIGameConsts* constants = NGameX::GetAIConsts();
    if (constants == nullptr) {
        *error = "legacy_game_ai_constants_missing";
        return false;
    }
    g_map_player_count = static_cast<int>(map->players.size());
    g_diplomacy_count = static_cast<int>(map->diplomacies.size());

    SetStage("normalize_rpg_stats");
    NormalizeMapRPGStats(map);

    g_start_unit_object_count = CountStartUnitObjects(map);

    SetStage("register_timer");
    IGameTimer* timer = CreateGameTimer(SAIConsts::AI_SEGMENT_DURATION);
    g_game_timer = timer;
    NSingleton::RegisterSingleton(timer, IGameTimer::tidTypeID);
    timer->Reset(g_timer_millis);

    SetStage("register_pathfinder");
    RegisterPathfinderSingleton();

    SetStage("scenario_tracker");
    if (!InitializeScenarioTracker(
                map,
                campaign_index,
                chapter_index,
                difficulty,
                error)) {
        ShutdownLegacyGameRuntime();
        return false;
    }

    SetStage("create_ai");
    CreateAI();
    g_ai_logic = Singleton<IAILogic>();
    if (g_ai_logic == nullptr) {
        *error = "legacy_game_ai_create_failed";
        ShutdownLegacyGameRuntime();
        return false;
    }

    SetStage("ai_init");
    g_ai_logic->Init(
            nullptr,
            map,
            constants,
            g_scenario_tracker);

    SetStage("terrain_observer");
    const int map_width = map->nNumPatchesX * AI_TILES_IN_PATCH;
    const int map_height = map->nNumPatchesY * AI_TILES_IN_PATCH;
    g_terrain_observer =
            g_ai_logic->CreateTerraAIObserver(map_width, map_height);
    if (!g_terrain_observer) {
        *error = "legacy_game_terrain_observer_create_failed";
        ShutdownLegacyGameRuntime();
        return false;
    }
    if (!FeedTerrainObserver(
                map,
                terrain_info,
                g_terrain_observer,
                error)) {
        ShutdownLegacyGameRuntime();
        return false;
    }

    SetStage("load_units_and_scripts");
    g_ai_logic->SetMyDiplomacyInfo(
            g_scenario_tracker->GetPlayerSide(0),
            0);
    g_ai_logic->InitAfterMapLoad(map);

    SetStage("post_map_load");
    g_ai_logic->PostMapLoad();
    g_ai_logic->Resume();
    UpdateWarFogSnapshot();

    g_unit_count = 0;
    const int players_to_count = static_cast<int>(map->diplomacies.size());
    for (int player = 0; player < players_to_count; ++player) {
        g_unit_count += g_ai_logic->GetUnitCount(player);
    }
    CountAIUnits();
    PublishPresentationEntities();
    g_ready = g_ai_logic->IsMissionLoaded();
    SetStage(g_ready ? "ready" : "mission_not_loaded");
    if (!g_ready) {
        *error = "legacy_game_mission_not_loaded";
        ShutdownLegacyGameRuntime();
        return false;
    }
    return true;
}

const std::string& LegacyMechTurretBone(uint64_t stats_path_hash) {
    static const std::string kNone;
    const std::unordered_map<uint64_t, std::string>::const_iterator found =
            g_turret_bones.find(stats_path_hash);
    return found == g_turret_bones.end() ? kNone : found->second;
}

const std::string& LegacyMechGunBone(uint64_t stats_path_hash) {
    static const std::string kNone;
    const std::unordered_map<uint64_t, std::string>::const_iterator found =
            g_gun_bones.find(stats_path_hash);
    return found == g_gun_bones.end() ? kNone : found->second;
}

// A unit that dies while selected used to stay selected forever, and every
// order the player then gave was silently dropped by the group builder.
void PruneDeadSelectedLegacyUnits() {
    if (g_selected_unit_ids.empty()) {
        return;
    }
    std::vector<int> alive;
    alive.reserve(g_selected_unit_ids.size());
    for (int unit_id : g_selected_unit_ids) {
        CAIUnit* unit = CAIUnit::GetUnitByUniqueID(unit_id);
        if (unit != nullptr && unit->IsAlive() && unit->IsSelectable()) {
            alive.push_back(unit_id);
        }
    }
    if (alive.size() == g_selected_unit_ids.size()) {
        return;
    }
    g_selected_unit_ids = std::move(alive);
    if (g_selected_unit_ids.empty()) {
        g_selected_unit_id = -1;
        g_attack_target_unit_id = -1;
        if (g_android_command_group_registered) {
            theGroupLogic.UnregisterGroup(g_android_command_group);
            g_android_command_group_registered = false;
        }
        PlatformRuntime::instance().log_info(
                "player_selection=cleared; reason=units_dead");
        return;
    }
    if (std::find(
                g_selected_unit_ids.begin(),
                g_selected_unit_ids.end(),
                g_selected_unit_id) == g_selected_unit_ids.end()) {
        g_selected_unit_id = g_selected_unit_ids.front();
    }
}

void TickLegacyGameRuntime(uint32_t elapsed_millis) {
    if (!g_ready || g_ai_logic == nullptr || !g_game_timer) {
        return;
    }
    if (g_mission_outcome.load() != kLegacyMissionRunning) {
        return;
    }
    g_timer_millis += elapsed_millis;
    g_game_timer->Update(g_timer_millis);
    bool advanced = false;
    while (g_game_timer->NextSegment()) {
        g_ai_logic->Segment();
        ++g_segment_count;
        advanced = true;
    }
    UpdateWarFogSnapshot();
    DrainLegacyClientUpdates();
    PruneExpiredCombatEffects();
    FinalizePendingMissionOutcome();
    if (g_mission_outcome.load() != kLegacyMissionRunning) {
        CountAIUnits();
        PublishPresentationEntities();
        return;
    }
    if (advanced) {
        PruneDeadSelectedLegacyUnits();
        CountAIUnits();
        PublishPresentationEntities();
    }
}

int SelectLegacyUnitNear(
        float world_x,
        float world_y,
        float max_radius,
        int player) {
    if (!g_ready || max_radius <= 0.0f) {
        return -1;
    }
    const float max_distance_squared = max_radius * max_radius;
    float best_distance_squared = std::numeric_limits<float>::max();
    int best_id = -1;
    for (CGlobalIter iter(0, ANY_PARTY); !iter.IsFinished(); iter.Iterate()) {
        CAIUnit* unit = *iter;
        if (unit == nullptr ||
            !unit->IsAlive() ||
            !unit->IsSelectable() ||
            static_cast<int>(unit->GetPlayer()) != player) {
            continue;
        }
        const CVec3& center = unit->GetCenter();
        const float delta_x = AI2Vis(center.x) - world_x;
        const float delta_y = AI2Vis(center.y) - world_y;
        const float distance_squared =
                delta_x * delta_x + delta_y * delta_y;
        if (distance_squared <= max_distance_squared &&
            distance_squared < best_distance_squared) {
            best_distance_squared = distance_squared;
            best_id = unit->GetUniqueIdQU();
        }
    }
    if (best_id < 0) {
        return -1;
    }
    return SelectLegacyUnit(best_id, player) ? best_id : -1;
}

bool SelectLegacyUnit(int unit_id, int player) {
    if (!g_ready || unit_id < 0) {
        return false;
    }
    CAIUnit* unit = CAIUnit::GetUnitByUniqueID(unit_id);
    if (unit == nullptr ||
        !unit->IsAlive() ||
        !unit->IsSelectable() ||
        static_cast<int>(unit->GetPlayer()) != player) {
        return false;
    }
    g_selected_unit_id = unit_id;
    g_selected_unit_ids.clear();
    g_selected_unit_ids.push_back(unit_id);
    g_attack_target_unit_id = -1;
    PublishPresentationEntities();
    PlatformRuntime::instance().log_info(
            std::string("player_unit_selected=") +
            std::to_string(g_selected_unit_id));
    return true;
}

int SelectLegacyUnits(
        const std::vector<int>& unit_ids,
        int player) {
    if (!g_ready || unit_ids.empty()) {
        return 0;
    }
    constexpr size_t kMaximumMobileSelection = 12;
    std::vector<int> selected_ids;
    selected_ids.reserve(
            std::min(unit_ids.size(), kMaximumMobileSelection));
    for (int unit_id : unit_ids) {
        CAIUnit* unit = CAIUnit::GetUnitByUniqueID(unit_id);
        if (unit == nullptr ||
            !unit->IsAlive() ||
            !unit->IsSelectable() ||
            static_cast<int>(unit->GetPlayer()) != player) {
            continue;
        }
        const int selected_id = unit->GetUniqueIdQU();
        if (std::find(
                    selected_ids.begin(),
                    selected_ids.end(),
                    selected_id) != selected_ids.end()) {
            continue;
        }
        selected_ids.push_back(selected_id);
        if (selected_ids.size() >= kMaximumMobileSelection) {
            break;
        }
    }
    if (selected_ids.empty()) {
        return 0;
    }
    g_selected_unit_ids = std::move(selected_ids);
    g_selected_unit_id = g_selected_unit_ids.front();
    g_attack_target_unit_id = -1;
    PublishPresentationEntities();
    PlatformRuntime::instance().log_info(
            std::string("player_units_selected=") +
            std::to_string(g_selected_unit_ids.size()) +
            "; active=" + std::to_string(g_selected_unit_id));
    return static_cast<int>(g_selected_unit_ids.size());
}

int SelectLegacyUnitsByTypeNear(
        int seed_unit_id,
        float max_radius,
        int player) {
    if (!g_ready || seed_unit_id < 0 || max_radius <= 0.0f) {
        return 0;
    }
    CAIUnit* seed = CAIUnit::GetUnitByUniqueID(seed_unit_id);
    if (seed == nullptr ||
        !seed->IsAlive() ||
        !seed->IsSelectable() ||
        static_cast<int>(seed->GetPlayer()) != player ||
        seed->GetStats() == nullptr) {
        return 0;
    }
    struct Candidate {
        float distance_squared;
        int unit_id;
    };
    std::vector<Candidate> candidates;
    const CVec3& seed_center = seed->GetCenter();
    const float max_distance_squared = max_radius * max_radius;
    for (CGlobalIter iter(0, ANY_PARTY);
         !iter.IsFinished();
         iter.Iterate()) {
        CAIUnit* unit = *iter;
        if (unit == nullptr ||
            unit == seed ||
            !unit->IsAlive() ||
            !unit->IsSelectable() ||
            static_cast<int>(unit->GetPlayer()) != player ||
            unit->GetStats() != seed->GetStats() ||
            unit->IsFormation() != seed->IsFormation() ||
            unit->IsMech() != seed->IsMech() ||
            unit->IsInfantry() != seed->IsInfantry()) {
            continue;
        }
        const CVec3& center = unit->GetCenter();
        const float delta_x = AI2Vis(center.x - seed_center.x);
        const float delta_y = AI2Vis(center.y - seed_center.y);
        const float distance_squared =
                delta_x * delta_x + delta_y * delta_y;
        if (distance_squared <= max_distance_squared) {
            candidates.push_back(
                    Candidate{
                            distance_squared,
                            unit->GetUniqueIdQU()});
        }
    }
    std::sort(
            candidates.begin(),
            candidates.end(),
            [](const Candidate& left, const Candidate& right) {
                if (left.distance_squared != right.distance_squared) {
                    return left.distance_squared <
                            right.distance_squared;
                }
                return left.unit_id < right.unit_id;
            });
    constexpr size_t kMaximumMobileSelection = 12;
    g_selected_unit_id = seed_unit_id;
    g_selected_unit_ids.clear();
    g_selected_unit_ids.push_back(seed_unit_id);
    for (const Candidate& candidate : candidates) {
        if (g_selected_unit_ids.size() >=
            kMaximumMobileSelection) {
            break;
        }
        g_selected_unit_ids.push_back(candidate.unit_id);
    }
    g_attack_target_unit_id = -1;
    PublishPresentationEntities();
    PlatformRuntime::instance().log_info(
            std::string("player_units_selected_by_type=") +
            std::to_string(g_selected_unit_ids.size()) +
            "; seed=" + std::to_string(seed_unit_id) +
            "; radius=" + std::to_string(max_radius));
    return static_cast<int>(g_selected_unit_ids.size());
}

bool ActivateSelectedLegacyUnit(int unit_id) {
    if (!g_ready || unit_id < 0) {
        return false;
    }
    auto selected = std::find(
            g_selected_unit_ids.begin(),
            g_selected_unit_ids.end(),
            unit_id);
    if (selected == g_selected_unit_ids.end()) {
        return false;
    }
    CAIUnit* unit = CAIUnit::GetUnitByUniqueID(unit_id);
    if (unit == nullptr ||
        !unit->IsAlive() ||
        !unit->IsSelectable() ||
        unit->GetPlayer() != 0) {
        return false;
    }
    std::rotate(
            g_selected_unit_ids.begin(),
            selected,
            selected + 1);
    g_selected_unit_id = unit_id;
    PublishPresentationEntities();
    PlatformRuntime::instance().log_info(
            std::string("player_active_selected_unit=") +
            std::to_string(unit_id) +
            "; selection=" +
            std::to_string(g_selected_unit_ids.size()));
    return true;
}

bool MoveSelectedLegacyUnit(float world_x, float world_y) {
    WORD group = 0;
    size_t unit_count = 0;
    if (!RegisterSelectedLegacyCommandGroup(
                NDb::USER_ACTION_MOVE,
                true,
                &group,
                &unit_count)) {
        return false;
    }
    CVec2 target;
    Vis2AI(&target, world_x, world_y);
    SAIUnitCmd command(ACTION_COMMAND_MOVE_TO, target);
    command.bFromAI = false;
    theGroupLogic.GroupCommand(command, group, false);
    ReleaseSelectedLegacyCommandGroup();
    g_attack_target_unit_id = -1;
    ++g_player_move_command_count;
    PlatformRuntime::instance().log_info(
            std::string("player_move_command=") +
            std::to_string(g_selected_unit_id) +
            "; units=" + std::to_string(unit_count) +
            "; target=" + std::to_string(world_x) +
            "," + std::to_string(world_y));
    return true;
}

bool PerformSelectedLegacyUnitPointAction(
        int user_action,
        float world_x,
        float world_y) {
    if (!g_ready || g_selected_unit_id < 0) {
        return false;
    }
    EActionCommand command_type;
    bool place_in_queue = false;
    int command_number = 0;
    switch (user_action) {
        case NDb::USER_ACTION_ROTATE:
            command_type = ACTION_COMMAND_ROTATE_TO;
            break;
        case NDb::USER_ACTION_ENGINEER_PLACE_MINES:
            command_type = ACTION_COMMAND_PLACEMINE;
            place_in_queue = true;
            command_number = 1;
            break;
        case NDb::USER_ACTION_ENGINEER_CLEAR_MINES:
            command_type = ACTION_COMMAND_CLEARMINE;
            break;
        case NDb::USER_ACTION_SPYGLASS:
            command_type = ACTION_COMMAND_USE_SPYGLASS;
            break;
        default:
            return false;
    }

    CVec2 target;
    Vis2AI(&target, world_x, world_y);
    SAIUnitCmd command(command_type, target);
    command.bFromAI = false;
    command.nNumber = command_number;
    WORD group = 0;
    size_t unit_count = 0;
    if (!RegisterSelectedLegacyCommandGroup(
                user_action,
                false,
                &group,
                &unit_count)) {
        return false;
    }
    theGroupLogic.GroupCommand(command, group, place_in_queue);
    ReleaseSelectedLegacyCommandGroup();
    g_attack_target_unit_id = -1;
    PublishPresentationEntities();
    PlatformRuntime::instance().log_info(
            std::string("player_unit_point_action=") +
            std::to_string(user_action) +
            "; unit=" + std::to_string(g_selected_unit_id) +
            "; units=" + std::to_string(unit_count) +
            "; target=" + std::to_string(world_x) +
            "," + std::to_string(world_y));
    return true;
}

bool PerformSelectedLegacyUnitSegmentAction(
        int user_action,
        float start_world_x,
        float start_world_y,
        float end_world_x,
        float end_world_y) {
    if (!g_ready ||
        g_selected_unit_id < 0 ||
        user_action !=
                NDb::USER_ACTION_ENGINEER_BUILD_ENTRENCHMENT) {
        return false;
    }
    CVec2 start_target;
    CVec2 end_target;
    Vis2AI(&start_target, start_world_x, start_world_y);
    Vis2AI(&end_target, end_world_x, end_world_y);

    SAIUnitCmd begin_command(
            ACTION_COMMAND_ENTRENCH_BEGIN,
            start_target);
    begin_command.bFromAI = false;
    begin_command.nNumber = 1;

    SAIUnitCmd end_command(
            ACTION_COMMAND_ENTRENCH_END,
            end_target);
    end_command.bFromAI = false;
    end_command.nNumber = 1;
    WORD group = 0;
    size_t unit_count = 0;
    if (!RegisterSelectedLegacyCommandGroup(
                user_action,
                false,
                &group,
                &unit_count)) {
        return false;
    }
    theGroupLogic.GroupCommand(begin_command, group, false);
    theGroupLogic.GroupCommand(end_command, group, true);
    ReleaseSelectedLegacyCommandGroup();

    g_attack_target_unit_id = -1;
    PublishPresentationEntities();
    PlatformRuntime::instance().log_info(
            std::string("player_unit_segment_action=") +
            std::to_string(user_action) +
            "; unit=" + std::to_string(g_selected_unit_id) +
            "; units=" + std::to_string(unit_count) +
            "; start=" + std::to_string(start_world_x) +
            "," + std::to_string(start_world_y) +
            "; end=" + std::to_string(end_world_x) +
            "," + std::to_string(end_world_y));
    return true;
}

bool CanSelectedLegacyUnitPerformAction(int user_action) {
    if (!g_ready || g_selected_unit_id < 0) {
        return false;
    }
    CAIUnit* unit = CAIUnit::GetUnitByUniqueID(g_selected_unit_id);
    return unit != nullptr &&
            unit->IsAlive() &&
            unit->IsSelectable() &&
            unit->GetPlayer() == 0 &&
            IsLegacyUnitAction(unit, user_action);
}

bool AttackSelectedLegacyUnit(int target_unit_id) {
    if (!g_ready || g_selected_unit_id < 0 || target_unit_id < 0) {
        return false;
    }
    CAIUnit* attacker = CAIUnit::GetUnitByUniqueID(g_selected_unit_id);
    if (attacker == nullptr || !attacker->IsAlive()) {
        return false;
    }
    CAIUnit* target = CAIUnit::GetUnitByUniqueID(target_unit_id);
    if (target == nullptr ||
        !target->IsAlive() ||
        target->GetPlayer() == attacker->GetPlayer()) {
        return false;
    }

    SAIUnitCmd command(
            ACTION_COMMAND_ATTACK_UNIT,
            target->GetUniqueId());
    command.bFromAI = false;
    WORD group = 0;
    size_t unit_count = 0;
    if (!RegisterSelectedLegacyCommandGroup(
                NDb::USER_ACTION_ATTACK,
                false,
                &group,
                &unit_count)) {
        return false;
    }
    theGroupLogic.GroupCommand(command, group, false);
    ReleaseSelectedLegacyCommandGroup();
    g_attack_target_unit_id = target->GetUniqueIdQU();
    ++g_player_attack_command_count;
    PublishPresentationEntities();
    PlatformRuntime::instance().log_info(
            std::string("player_attack_command=") +
            std::to_string(g_selected_unit_id) +
            "; units=" + std::to_string(unit_count) +
            "; target=" + std::to_string(target->GetUniqueIdQU()));
    return true;
}

bool StopSelectedLegacyUnit() {
    WORD group = 0;
    size_t unit_count = 0;
    if (!RegisterSelectedLegacyCommandGroup(
                NDb::USER_ACTION_STOP,
                false,
                &group,
                &unit_count)) {
        return false;
    }
    SAIUnitCmd command(ACTION_COMMAND_STOP);
    command.bFromAI = false;
    theGroupLogic.GroupCommand(command, group, false);
    ReleaseSelectedLegacyCommandGroup();
    g_attack_target_unit_id = -1;
    ++g_player_stop_command_count;
    PublishPresentationEntities();
    PlatformRuntime::instance().log_info(
            std::string("player_stop_command=") +
            std::to_string(g_selected_unit_id) +
            "; units=" + std::to_string(unit_count));
    return true;
}

bool PerformSelectedLegacyUnitAction(int user_action) {
    if (!g_ready || g_selected_unit_id < 0) {
        return false;
    }
    if (user_action == NDb::USER_ACTION_STOP) {
        return StopSelectedLegacyUnit();
    }
    EActionCommand command_type;
    switch (user_action) {
        case NDb::USER_ACTION_ENTRENCH_SELF:
            command_type = ACTION_COMMAND_ENTRENCH_SELF;
            break;
        case NDb::USER_ACTION_STAND_GROUND:
            command_type = ACTION_COMMAND_STAND_GROUND;
            break;
        default:
            return false;
    }

    SAIUnitCmd command(command_type);
    command.bFromAI = false;
    WORD group = 0;
    size_t unit_count = 0;
    if (!RegisterSelectedLegacyCommandGroup(
                user_action,
                false,
                &group,
                &unit_count)) {
        return false;
    }
    theGroupLogic.GroupCommand(command, group, false);
    ReleaseSelectedLegacyCommandGroup();
    g_attack_target_unit_id = -1;
    PublishPresentationEntities();
    PlatformRuntime::instance().log_info(
            std::string("player_unit_action=") +
            std::to_string(user_action) +
            "; unit=" + std::to_string(g_selected_unit_id) +
            "; units=" + std::to_string(unit_count));
    return true;
}

int SelectedLegacyUnitId() {
    return g_selected_unit_id;
}

int SelectedLegacyUnitCount() {
    int count = 0;
    for (int unit_id : g_selected_unit_ids) {
        CAIUnit* unit = CAIUnit::GetUnitByUniqueID(unit_id);
        if (unit != nullptr &&
            unit->IsAlive() &&
            unit->IsSelectable() &&
            unit->GetPlayer() == 0) {
            ++count;
        }
    }
    return count;
}

std::string SelectedLegacyUnitHudStatus() {
    if (!g_ready || g_selected_unit_id < 0) {
        return std::string();
    }
    CAIUnit* unit = CAIUnit::GetUnitByUniqueID(g_selected_unit_id);
    if (unit == nullptr || !unit->IsAlive()) {
        return std::string();
    }
    const NDb::SUnitBaseRPGStats* stats = unit->GetStats();
    if (stats == nullptr ||
        !std::isfinite(unit->GetHitPoints()) ||
        !std::isfinite(stats->fMaxHP) ||
        stats->fMaxHP <= 0.0f) {
        return std::string();
    }
    std::ostringstream text;
    text << "Selected unit HP: "
         << static_cast<int>(std::round(
                    std::max(unit->GetHitPoints(), 0.0f)))
         << " / "
         << static_cast<int>(std::round(stats->fMaxHP));
    return text.str();
}

std::string SelectedLegacyUnitHudSnapshot() {
    if (!g_ready || g_selected_unit_id < 0) {
        return std::string();
    }
    CAIUnit* unit = CAIUnit::GetUnitByUniqueID(g_selected_unit_id);
    if (unit == nullptr || !unit->IsAlive()) {
        return std::string();
    }
    const NDb::SUnitBaseRPGStats* stats = unit->GetStats();
    if (stats == nullptr ||
        !std::isfinite(unit->GetHitPoints()) ||
        !std::isfinite(stats->fMaxHP) ||
        stats->fMaxHP <= 0.0f) {
        return std::string();
    }
    std::ostringstream snapshot;
    snapshot << "id=" << g_selected_unit_id
             << ";kind=" << (unit->IsMech() ? "tank" : "soldier")
             << ";hp=" << static_cast<int>(std::round(
                    std::max(unit->GetHitPoints(), 0.0f)))
             << ";max_hp="
             << static_cast<int>(std::round(stats->fMaxHP));
    snapshot << ";selection_count=" << SelectedLegacyUnitCount()
             << ";members=";
    bool first_member = true;
    for (int selected_id : g_selected_unit_ids) {
        CAIUnit* selected =
                CAIUnit::GetUnitByUniqueID(selected_id);
        if (selected == nullptr ||
            !selected->IsAlive() ||
            !selected->IsSelectable() ||
            selected->GetPlayer() != 0 ||
            selected->GetStats() == nullptr ||
            !std::isfinite(selected->GetHitPoints()) ||
            !std::isfinite(selected->GetStats()->fMaxHP) ||
            selected->GetStats()->fMaxHP <= 0.0f) {
            continue;
        }
        if (!first_member) {
            snapshot << ",";
        }
        snapshot << selected_id
                 << ":"
                 << (selected->IsMech() ? "tank" : "soldier")
                 << ":"
                 << static_cast<int>(std::round(
                            std::max(
                                    selected->GetHitPoints(),
                                    0.0f)))
                 << ":"
                 << static_cast<int>(std::round(
                            selected->GetStats()->fMaxHP));
        first_member = false;
    }
    const CUserActions actions = SelectedLegacyActions(unit);
    snapshot << ";actions=";
    bool first_action = true;
    for (int action = 1; action <= 127; ++action) {
        if (!actions.HasAction(action)) {
            continue;
        }
        if (!first_action) {
            snapshot << ",";
        }
        snapshot << action;
        first_action = false;
    }
    snapshot << ";enabled=";
    bool first_enabled = true;
    for (const int action : {
                 static_cast<int>(NDb::USER_ACTION_MOVE),
                 static_cast<int>(NDb::USER_ACTION_ATTACK),
                 static_cast<int>(NDb::USER_ACTION_ROTATE),
                 static_cast<int>(NDb::USER_ACTION_ENTRENCH_SELF),
                 static_cast<int>(NDb::USER_ACTION_STAND_GROUND),
                 static_cast<int>(
                         NDb::USER_ACTION_ENGINEER_PLACE_MINES),
                 static_cast<int>(
                         NDb::USER_ACTION_ENGINEER_CLEAR_MINES),
                 static_cast<int>(
                         NDb::USER_ACTION_ENGINEER_BUILD_ENTRENCHMENT),
                 static_cast<int>(NDb::USER_ACTION_STOP),
                 static_cast<int>(NDb::USER_ACTION_SPYGLASS)}) {
        if (!actions.HasAction(action) ||
            (action == NDb::USER_ACTION_MOVE &&
             !AnySelectedLegacyUnitCanMove())) {
            continue;
        }
        if (!first_enabled) {
            snapshot << ",";
        }
        snapshot << action;
        first_enabled = false;
    }
    snapshot << ";tiers=";
    const int ability_count = std::min(
            static_cast<int>(
                    stats->GetActions()->specialAbilities.size()),
            unit->GetAbilityLevel());
    bool first_tier = true;
    for (int tier = 0; tier < ability_count; ++tier) {
        const NDb::SUnitSpecialAblityDesc* ability =
                stats->GetActions()->specialAbilities[tier];
        if (ability == nullptr) {
            continue;
        }
        const NDb::EUserAction action =
                GetActionByAbility(ability->eName);
        if (action == NDb::USER_ACTION_UNKNOWN) {
            continue;
        }
        if (!first_tier) {
            snapshot << ",";
        }
        snapshot << static_cast<int>(action) << ":" << tier;
        first_tier = false;
    }
    return snapshot.str();
}

void HandleLegacyInputEvent(const char* event_name) {
    if (event_name == nullptr ||
        g_mission_outcome.load() != kLegacyMissionRunning) {
        return;
    }
    if (std::strcmp(event_name, "local_win") == 0) {
        g_pending_mission_outcome.store(kLegacyMissionWon);
    } else if (std::strcmp(event_name, "local_loose") == 0) {
        g_pending_mission_outcome.store(kLegacyMissionLost);
#if !defined(NDEBUG)
    } else if (std::strcmp(
                       event_name,
                       "debug_reinforcement_notification") == 0) {
        QueueLegacyHudNotification(
                NDb::NTF_REINFORCEMENT_ARRIVED);
    } else if (std::strcmp(event_name, "debug_force_combat") == 0) {
        CAIUnit* attacker = nullptr;
        CAIUnit* target = nullptr;
        float best_distance_squared =
                std::numeric_limits<float>::max();
        for (CGlobalIter attacker_iter(0, ANY_PARTY);
             !attacker_iter.IsFinished();
             attacker_iter.Iterate()) {
            CAIUnit* candidate = *attacker_iter;
            if (candidate == nullptr ||
                !candidate->IsAlive() ||
                !candidate->IsSelectable() ||
                candidate->GetPlayer() != 0) {
                continue;
            }
            for (CGlobalIter target_iter(0, ANY_PARTY);
                 !target_iter.IsFinished();
                 target_iter.Iterate()) {
                CAIUnit* enemy = *target_iter;
                if (enemy == nullptr ||
                    !enemy->IsAlive() ||
                    enemy->GetPlayer() == candidate->GetPlayer()) {
                    continue;
                }
                const CVec2 delta =
                        enemy->GetCenterPlain() -
                        candidate->GetCenterPlain();
                const float distance_squared =
                        delta.x * delta.x + delta.y * delta.y;
                if (distance_squared < best_distance_squared) {
                    best_distance_squared = distance_squared;
                    attacker = candidate;
                    target = enemy;
                }
            }
        }
        if (attacker != nullptr && target != nullptr &&
            SelectLegacyUnit(attacker->GetUniqueIdQU(), 0) &&
            AttackSelectedLegacyUnit(target->GetUniqueIdQU())) {
            PlatformRuntime::instance().log_info(
                    std::string("debug_force_combat=") +
                    std::to_string(attacker->GetUniqueIdQU()) +
                    "; target=" +
                    std::to_string(target->GetUniqueIdQU()));
        }
    } else if (std::strcmp(event_name, "debug_combat_effect") == 0 &&
               g_selected_unit_id >= 0 &&
               g_attack_target_unit_id >= 0) {
        CAIUnit* attacker =
                CAIUnit::GetUnitByUniqueID(g_selected_unit_id);
        CAIUnit* target =
                CAIUnit::GetUnitByUniqueID(g_attack_target_unit_id);
        if (attacker != nullptr && target != nullptr) {
            CaptureCombatEffect(
                    attacker->GetUniqueIdQU(),
                    target->GetCenter(),
                    attacker->IsMech()
                            ? AndroidCombatEffectType::MechanizedShot
                            : AndroidCombatEffectType::InfantryShot);
            PlatformRuntime::instance().log_info(
                    std::string("debug_combat_effect=") +
                    std::to_string(attacker->GetUniqueIdQU()) +
                    "; target=" +
                    std::to_string(target->GetUniqueIdQU()));
        }
    } else if (std::strcmp(event_name, "debug_kill_attack_target") == 0 &&
               g_attack_target_unit_id >= 0) {
        CAIUnit* target =
                CAIUnit::GetUnitByUniqueID(g_attack_target_unit_id);
        if (target != nullptr &&
            (!target->IsInfantry() || target->IsFormation())) {
            const int target_player = target->GetPlayer();
            target = nullptr;
            for (CGlobalIter iter(0, ANY_PARTY);
                 !iter.IsFinished();
                 iter.Iterate()) {
                CAIUnit* candidate = *iter;
                if (candidate != nullptr &&
                    candidate->IsAlive() &&
                    candidate->IsInfantry() &&
                    !candidate->IsFormation() &&
                    candidate->GetPlayer() == target_player) {
                    target = candidate;
                    break;
                }
            }
        }
        if (target != nullptr && target->IsAlive()) {
            CAIUnit* attacker =
                    CAIUnit::GetUnitByUniqueID(g_selected_unit_id);
            PlatformRuntime::instance().log_info(
                    std::string("debug_kill_attack_target=") +
                    std::to_string(target->GetUniqueIdQU()));
            if (attacker != nullptr &&
                attacker->IsAlive() &&
                attacker->GetPlayer() != target->GetPlayer() &&
                target->GetStats() != nullptr) {
                theStatistics.UnitKilled(
                        attacker->GetPlayer(),
                        target->GetPlayer(),
                        target->GetStats()->fExpPrice,
                        attacker->GetReinforcementType(),
                        target->GetReinforcementType(),
                        target->IsInfantry());
            }
            target->Die(false, target->GetHitPoints() + 1.0f);
        }
    } else if (std::strcmp(event_name, "debug_toggle_lying") == 0) {
        CSoldier* soldier = nullptr;
        if (g_selected_unit_id >= 0) {
            soldier = dynamic_cast<CSoldier*>(
                    CAIUnit::GetUnitByUniqueID(g_selected_unit_id));
        }
        if (soldier == nullptr) {
            for (CGlobalIter iter(0, ANY_PARTY);
                 !iter.IsFinished();
                 iter.Iterate()) {
                CSoldier* candidate = dynamic_cast<CSoldier*>(*iter);
                if (candidate != nullptr &&
                    candidate->IsAlive() &&
                    candidate->GetPlayer() == 0) {
                    soldier = candidate;
                    break;
                }
            }
        }
        if (soldier != nullptr) {
            if (soldier->IsLying()) {
                soldier->StandUp();
            } else {
                soldier->LieDownForce();
            }
            PublishPresentationEntities();
            PlatformRuntime::instance().log_info(
                    std::string("debug_toggle_lying=") +
                    std::to_string(soldier->GetUniqueIdQU()) +
                    "; lying=" +
                    (soldier->IsLying() ? "true" : "false"));
        }
    } else if (std::strcmp(event_name, "debug_kill_mechanized") == 0) {
        CAIUnit* target = nullptr;
        for (CGlobalIter iter(0, ANY_PARTY);
             !iter.IsFinished();
             iter.Iterate()) {
            CAIUnit* candidate = *iter;
            if (candidate != nullptr &&
                candidate->IsAlive() &&
                candidate->IsMech() &&
                candidate->IsVisible(theDipl.GetMyParty())) {
                target = candidate;
                break;
            }
        }
        if (target != nullptr) {
            PlatformRuntime::instance().log_info(
                    std::string("debug_kill_mechanized=") +
                    std::to_string(target->GetUniqueIdQU()));
            target->Die(
                    true,
                    target->GetHitPoints() + 1.0f);
        }
#endif
    }
}

const char* LegacyMissionOutcome() {
    return MissionOutcomeName(g_mission_outcome.load());
}

std::vector<AndroidCombatEffect> CopyActiveAndroidCombatEffects() {
    std::vector<AndroidCombatEffect> result;
    result.reserve(g_combat_effects.size());
    for (const TimedCombatEffect& timed : g_combat_effects) {
        AndroidCombatEffect effect = timed.effect;
        effect.age_millis = static_cast<uint32_t>(
                std::min(
                        g_timer_millis - timed.created_millis,
                        static_cast<uint64_t>(
                                effect.lifetime_millis)));
        result.push_back(effect);
    }
    return result;
}

std::vector<AndroidSceneEffect> CopyActiveAndroidSceneEffects() {
    std::vector<AndroidSceneEffect> result;
    result.reserve(g_scene_effects.size());
    for (const TimedSceneEffect& timed : g_scene_effects) {
        AndroidSceneEffect effect = timed.effect;
        effect.age_millis = static_cast<uint32_t>(
                std::min(
                        g_timer_millis - timed.created_millis,
                        static_cast<uint64_t>(
                                effect.lifetime_millis)));
        result.push_back(std::move(effect));
    }
    return result;
}

std::vector<AndroidDestructionEffect>
CopyActiveAndroidDestructionEffects() {
    std::vector<AndroidDestructionEffect> result;
    result.reserve(g_destruction_effects.size());
    for (const TimedDestructionEffect& timed : g_destruction_effects) {
        AndroidDestructionEffect effect = timed.effect;
        effect.age_millis = static_cast<uint32_t>(
                std::min(
                        g_timer_millis - timed.created_millis,
                        static_cast<uint64_t>(
                                effect.lifetime_millis)));
        result.push_back(effect);
    }
    return result;
}

AndroidWarFogSnapshot CopyAndroidWarFogSnapshot() {
    return g_war_fog_snapshot;
}

void ShutdownLegacyGameRuntime() {
    if (g_android_command_group_registered) {
        theGroupLogic.UnregisterGroup(g_android_command_group);
        g_android_command_group_registered = false;
        g_android_command_group = 0;
    }
    if (g_ai_logic != nullptr) {
        g_ai_logic->Suspend();
        g_ai_logic->ClearAI();
    }
    g_terrain_observer = nullptr;

    NSingleton::UnRegisterSingleton(CUpdates2Globe::tidTypeID);
    NSingleton::UnRegisterSingleton(ICommonB2M1AI::tidTypeID);
    NSingleton::UnRegisterSingleton(IAILogic::tidTypeID);
    NSingleton::UnRegisterSingleton(IAIScenarioTracker::tidTypeID);
    NSingleton::UnRegisterSingleton(IScenarioTracker::tidTypeID);
    NSingleton::UnRegisterSingleton(CCommonPathFinder::tidTypeID);
    NSingleton::UnRegisterSingleton(IGameTimer::tidTypeID);

    g_scenario_tracker = nullptr;
    g_game_timer = nullptr;
    SetAIMap(nullptr);
    ResetReportState();
    g_selected_unit_id = -1;
    g_selected_unit_ids.clear();
    g_attack_target_unit_id = -1;
    g_player_move_command_count = 0;
    g_player_attack_command_count = 0;
    g_player_stop_command_count = 0;
    g_client_update_count = 0;
    g_objective_update_count = 0;
    g_hud_notification_count = 0;
    g_last_presentation_entities.clear();
    g_presentation_corpses.clear();
    g_presentation_death_count = 0;
    g_war_fog_snapshot = AndroidWarFogSnapshot();
    g_war_fog_first_update = true;
    g_combat_effects.clear();
    g_scene_effects.clear();
    g_destruction_effects.clear();
    g_unit_death_effect_candidates.clear();
    g_infantry_shot_effect_count = 0;
    g_mechanized_shot_effect_count = 0;
    g_mechanized_destruction_effect_count = 0;
    g_descriptor_scene_effect_count = 0;
    g_descriptor_particle_emitter_count = 0;
    g_descriptor_particle_texture_count = 0;
    g_descriptor_effect_light_count = 0;
    g_forwarded_unit_kill_count = 0;
    g_forwarded_unit_kill_error_count = 0;
    g_mission_outcome.store(kLegacyMissionRunning);
    g_pending_mission_outcome.store(kLegacyMissionRunning);
}

bool IsLegacyGameRuntimeReady() {
    return g_ready;
}

std::string LegacyGameRuntimeReport() {
    std::ostringstream report;
    report << "legacy_game=" << (g_ready ? "ready" : "not_ready")
           << "; stage=" << g_stage
           << "; units=" << g_unit_count
           << "; segments=" << g_segment_count
           << "; missing_unit_payload_sample="
           << JoinRefSamples(g_missing_unit_payload_refs, 2)
           << "; missing_squad_payload_sample="
           << JoinRefSamples(g_missing_squad_payload_refs, 2)
           << "; map_players=" << g_map_player_count
           << "; diplomacies=" << g_diplomacy_count
           << "; start_unit_objects=" << g_start_unit_object_count
           << "; ai_units_total=" << g_ai_unit_total_count
           << "; ai_units_alive=" << g_ai_unit_alive_count
           << "; ai_units_ref_valid=" << g_ai_unit_ref_valid_count
           << "; selected_unit=" << g_selected_unit_id
           << "; attack_target_unit=" << g_attack_target_unit_id
           << "; mission_outcome="
           << MissionOutcomeName(g_mission_outcome.load())
           << "; player_move_commands=" << g_player_move_command_count
           << "; player_attack_commands=" << g_player_attack_command_count
           << "; player_stop_commands=" << g_player_stop_command_count
           << "; mission_sounds_played=" << g_mission_sound_played
           << "; mission_sounds_missing=" << g_mission_sound_missing
           << "; mission_sound_clips=" << g_mission_sound_clips.size()
           << "; client_updates=" << g_client_update_count
           << "; objective_updates=" << g_objective_update_count
           << "; hud_notifications=" << g_hud_notification_count
           << "; infantry_shot_effects="
           << g_infantry_shot_effect_count
           << "; mechanized_shot_effects="
           << g_mechanized_shot_effect_count
           << "; active_combat_effects=" << g_combat_effects.size()
           << "; descriptor_scene_effects="
           << g_descriptor_scene_effect_count
           << "; active_scene_effects=" << g_scene_effects.size()
           << "; descriptor_particle_emitters="
           << g_descriptor_particle_emitter_count
           << "; descriptor_particle_textures="
           << g_descriptor_particle_texture_count
           << "; descriptor_effect_lights="
           << g_descriptor_effect_light_count
           << "; mechanized_destruction_effects="
           << g_mechanized_destruction_effect_count
           << "; active_destruction_effects="
           << g_destruction_effects.size()
           << "; forwarded_unit_kills="
           << g_forwarded_unit_kill_count
           << "; forwarded_unit_kill_errors="
           << g_forwarded_unit_kill_error_count
           << "; presented_deaths=" << g_presentation_death_count
           << "; active_corpses=" << g_presentation_corpses.size()
           << "; war_fog=" << g_war_fog_snapshot.width
           << "x" << g_war_fog_snapshot.height
           << "; war_fog_generation="
           << g_war_fog_snapshot.generation
           << "; normalized_rpg_stats=" << g_normalized_rpg_stats_count
           << "; normalized_unit_stats=" << g_normalized_unit_stats_count
           << "; normalized_squad_stats=" << g_normalized_squad_stats_count
           << "; normalize_skipped_no_visual=" << g_normalize_skipped_no_visual_count
           << "; missing_unit_payload=" << g_missing_unit_payload_refs.size()
           << "; missing_squad_payload=" << g_missing_squad_payload_refs.size()
           << "; ai_load_objects=" << bk2_android_ai_debug_load_objects
           << "; ai_load_candidates=" << bk2_android_ai_debug_load_candidates
           << "; ai_reinforcement_deferred=" << bk2_android_ai_debug_reinforcement_deferred
           << "; ai_add_calls=" << bk2_android_ai_debug_add_calls
           << "; ai_add_success=" << bk2_android_ai_debug_add_success
           << "; ai_add_failed=" << bk2_android_ai_debug_add_failed
           << "; ai_empty_stats=" << bk2_android_ai_debug_empty_stats
           << "; ai_bare_infantry=" << bk2_android_ai_debug_bare_infantry
           << "; ai_bad_visual=" << bk2_android_ai_debug_bad_visual
           << "; ai_outside_map=" << bk2_android_ai_debug_outside_map
           << "; ai_player_missing=" << bk2_android_ai_debug_player_missing
           << "; ai_unit_case=" << bk2_android_ai_debug_unit_case
           << "; ai_squad_case=" << bk2_android_ai_debug_squad_case
           << "; ai_other_case=" << bk2_android_ai_debug_other_case
           << "; terrain_types=" << g_terrain_type_count
           << "; terrain_grid=" << g_terrain_grid_width
           << "x" << g_terrain_grid_height
           << "; terrain_features=" << g_terrain_feature_count;
    return report.str();
}

}
