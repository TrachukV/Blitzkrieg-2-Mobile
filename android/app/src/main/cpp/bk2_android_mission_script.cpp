#include "bk2_android_mission_script.h"

#include "bk2_android_mission_runtime.h"

#include "GameX/stdafx.h"
#include "AILogic/DBAIConsts.h"
#include "GameX/GetConsts.h"
#include "Script/Script.h"
#include "Script/ScriptWrapper.h"
#include "Stats_B2_M1/DBMapInfo.h"
#include "Stats_B2_M1/RPGStats.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <sstream>
#include <vector>

namespace bk2::android {
namespace {

constexpr uint32_t kScriptSegmentMillis = 200;

struct ScriptObjectState {
    int unique_id = 0;
    int script_id = -1;
    int player = 0;
    float x = 0.0f;
    float y = 0.0f;
    float hp = 0.0f;
    bool alive = false;
};

CObj<IScriptWrapper> g_script;
const NDb::SMapInfo* g_map = nullptr;
std::vector<ScriptObjectState> g_objects;
std::string g_script_path;
std::string g_outcome = "running";
uint32_t g_segment_accumulator = 0;
uint64_t g_segment_count = 0;
int g_common_script_count = 0;
int g_objective_events = 0;
int g_runtime_errors = 0;

bool EqualsIgnoreCase(const string& left, const char* right) {
    if (right == nullptr || left.size() != std::strlen(right)) {
        return false;
    }
    for (size_t i = 0; i < left.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(left[i])) !=
            std::tolower(static_cast<unsigned char>(right[i]))) {
            return false;
        }
    }
    return true;
}

bool IsUnitObject(const NDb::SMapObjectInfo& object) {
    const NDb::SHPObjectRPGStats* stats = object.pObject.GetPtrNoLoad();
    if (stats == nullptr) {
        return false;
    }
    const int type_id = stats->GetTypeID();
    return type_id == NDb::SMechUnitRPGStats::typeID ||
           type_id == NDb::SSquadRPGStats::typeID ||
           type_id == NDb::SInfantryRPGStats::typeID;
}

void AddScriptObjects(const vector<NDb::SMapObjectInfo>& objects) {
    for (size_t i = 0; i < objects.size(); ++i) {
        const NDb::SMapObjectInfo& object = objects[i];
        if (!IsUnitObject(object)) {
            continue;
        }
        ScriptObjectState state;
        state.unique_id = object.link.nLinkID >= 0
                ? object.link.nLinkID
                : static_cast<int>(g_objects.size()) + 1;
        state.script_id = object.nScriptID;
        state.player = object.nPlayer;
        state.x = object.vPos.x;
        state.y = object.vPos.y;
        state.hp = object.fHP;
        state.alive = object.fHP > 0.0f;
        g_objects.push_back(state);
    }
}

const NDb::SScriptArea* FindArea(const char* name) {
    if (g_map == nullptr || name == nullptr) {
        return nullptr;
    }
    for (size_t i = 0; i < g_map->scriptAreas.size(); ++i) {
        if (EqualsIgnoreCase(g_map->scriptAreas[i].szName, name)) {
            return &g_map->scriptAreas[i];
        }
    }
    return nullptr;
}

bool IsInsideArea(const ScriptObjectState& object, const NDb::SScriptArea& area) {
    const float dx = object.x - area.vCenter.x;
    const float dy = object.y - area.vCenter.y;
    if (area.eType == NDb::EAT_CIRCLE) {
        return dx * dx + dy * dy <= area.fR * area.fR;
    }
    return std::abs(dx) <= area.vAABBHalfSize.x &&
           std::abs(dy) <= area.vAABBHalfSize.y;
}

int CountUnitsInArea(int player, const char* area_name) {
    const NDb::SScriptArea* area = FindArea(area_name);
    if (area == nullptr) {
        return 0;
    }
    int count = 0;
    for (const ScriptObjectState& object : g_objects) {
        if (object.alive && object.player == player &&
            IsInsideArea(object, *area)) {
            ++count;
        }
    }
    return count;
}

ScriptObjectState* FindObject(int unique_id) {
    for (ScriptObjectState& object : g_objects) {
        if (object.unique_id == unique_id) {
            return &object;
        }
    }
    return nullptr;
}

int LuaNoOp(lua_State*) {
    return 0;
}

int LuaZero(lua_State* state) {
    lua_pushnumber(state, 0.0);
    return 1;
}

int LuaRandomInt(lua_State* state) {
    const int upper = lua_gettop(state) > 0
            ? std::max(0, static_cast<int>(lua_tonumber(state, 1)))
            : 0;
    lua_pushnumber(state, upper > 0 ? std::rand() % upper : 0);
    return 1;
}

int LuaGetDifficulty(lua_State* state) {
    lua_pushnumber(state, GetMissionRuntimeState().difficulty);
    return 1;
}

int LuaSetGlobalInt(lua_State* state) {
    if (lua_gettop(state) >= 2) {
        const char* name = lua_tostring(state, 1);
        if (name != nullptr) {
            NGlobal::SetVar(
                    name,
                    static_cast<int>(lua_tonumber(state, 2)),
                    STORAGE_SAVE);
        }
    }
    return 0;
}

int LuaGetGlobalInt(lua_State* state) {
    const char* name =
            lua_gettop(state) > 0 ? lua_tostring(state, 1) : nullptr;
    const int fallback = lua_gettop(state) > 1
            ? static_cast<int>(lua_tonumber(state, 2))
            : 0;
    const int value = name != nullptr
            ? static_cast<int>(NGlobal::GetVar(name, fallback))
            : fallback;
    lua_pushnumber(state, value);
    return 1;
}

int LuaObjectiveChanged(lua_State* state) {
    if (lua_gettop(state) >= 2) {
        const int objective = static_cast<int>(lua_tonumber(state, 1));
        const int value = static_cast<int>(lua_tonumber(state, 2));
        const MissionRuntimeResult result =
                SetMissionObjectiveState(objective, value);
        if (result.ok) {
            ++g_objective_events;
        } else {
            ++g_runtime_errors;
        }
    }
    return 0;
}

int LuaWin(lua_State* state) {
    const int winning_party = lua_gettop(state) > 0
            ? static_cast<int>(lua_tonumber(state, 1))
            : 0;
    if (winning_party == 0) {
        const MissionRuntimeResult result = MarkMissionWon();
        g_outcome = result.ok ? "won" : "win_error";
        if (!result.ok) {
            ++g_runtime_errors;
        }
    } else {
        g_outcome = "lost";
    }
    return 0;
}

int LuaLoose(lua_State*) {
    g_outcome = "lost";
    return 0;
}

int LuaCountUnitsInArea(lua_State* state) {
    const int player = lua_gettop(state) > 0
            ? static_cast<int>(lua_tonumber(state, 1))
            : -1;
    const char* area =
            lua_gettop(state) > 1 ? lua_tostring(state, 2) : nullptr;
    lua_pushnumber(state, CountUnitsInArea(player, area));
    return 1;
}

int LuaIsSomeUnitInArea(lua_State* state) {
    const int player = lua_gettop(state) > 0
            ? static_cast<int>(lua_tonumber(state, 1))
            : -1;
    const char* area =
            lua_gettop(state) > 1 ? lua_tostring(state, 2) : nullptr;
    lua_pushnumber(state, CountUnitsInArea(player, area) > 0 ? 1 : 0);
    return 1;
}

int LuaIsSomePlayerUnit(lua_State* state) {
    const int player = lua_gettop(state) > 0
            ? static_cast<int>(lua_tonumber(state, 1))
            : -1;
    int count = 0;
    for (const ScriptObjectState& object : g_objects) {
        if (object.alive && object.player == player) {
            ++count;
        }
    }
    lua_pushnumber(state, count);
    return 1;
}

int LuaIsSomeBodyAlive(lua_State* state) {
    const int player = lua_gettop(state) > 0
            ? static_cast<int>(lua_tonumber(state, 1))
            : -1;
    const int script_id = lua_gettop(state) > 1
            ? static_cast<int>(lua_tonumber(state, 2))
            : -1;
    int count = 0;
    for (const ScriptObjectState& object : g_objects) {
        if (object.alive && object.player == player &&
            object.script_id == script_id) {
            ++count;
        }
    }
    lua_pushnumber(state, count);
    return 1;
}

int LuaGetObjectList(lua_State* state) {
    const int script_id = lua_gettop(state) > 0
            ? static_cast<int>(lua_tonumber(state, 1))
            : -1;
    int count = 0;
    for (const ScriptObjectState& object : g_objects) {
        if (object.alive && object.script_id == script_id) {
            lua_pushnumber(state, object.unique_id);
            ++count;
        }
    }
    return count;
}

int LuaIsAlive(lua_State* state) {
    const int unique_id = lua_gettop(state) > 0
            ? static_cast<int>(lua_tonumber(state, 1))
            : 0;
    const ScriptObjectState* object = FindObject(unique_id);
    lua_pushnumber(state, object != nullptr && object->alive ? 1 : 0);
    return 1;
}

int LuaDamageObject(lua_State* state) {
    if (lua_gettop(state) >= 2) {
        const int unique_id = static_cast<int>(lua_tonumber(state, 1));
        const float damage = static_cast<float>(lua_tonumber(state, 2));
        ScriptObjectState* object = FindObject(unique_id);
        if (object != nullptr && object->alive) {
            object->hp -= damage * 0.01f;
            object->alive = object->hp > 0.0f;
        }
    }
    return 0;
}

int LuaChangePlayer(lua_State* state) {
    if (lua_gettop(state) >= 2) {
        const int unique_id = static_cast<int>(lua_tonumber(state, 1));
        const int player = static_cast<int>(lua_tonumber(state, 2));
        ScriptObjectState* object = FindObject(unique_id);
        if (object != nullptr) {
            object->player = player;
        }
    }
    return 0;
}

int LuaObjectGetCoord(lua_State* state) {
    const int unique_id = lua_gettop(state) > 0
            ? static_cast<int>(lua_tonumber(state, 1))
            : 0;
    const ScriptObjectState* object = FindObject(unique_id);
    lua_pushnumber(state, object != nullptr ? object->x : 0.0);
    lua_pushnumber(state, object != nullptr ? object->y : 0.0);
    return 2;
}

int LuaGetReinforcementCallsLeft(lua_State* state) {
    const int player = lua_gettop(state) > 0
            ? static_cast<int>(lua_tonumber(state, 1))
            : 0;
    const MissionRuntimeState mission = GetMissionRuntimeState();
    lua_pushnumber(
            state,
            player == 0
                    ? mission.mission_reinforcement_calls_left
                    : mission.enemy_reinforcement_calls_left);
    return 1;
}

int LuaGetScriptAreaParams(lua_State* state) {
    const char* name =
            lua_gettop(state) > 0 ? lua_tostring(state, 1) : nullptr;
    const NDb::SScriptArea* area = FindArea(name);
    lua_pushnumber(state, area != nullptr ? area->vCenter.x : 0.0);
    lua_pushnumber(state, area != nullptr ? area->vCenter.y : 0.0);
    return 2;
}

int LuaGetGameTime(lua_State* state) {
    lua_pushnumber(
            state,
            static_cast<double>(g_segment_count * kScriptSegmentMillis) /
                    1000.0);
    return 1;
}

SRegFunction kMissionFunctions[] = {
        {"Cmd", LuaNoOp},
        {"GiveCommand", LuaNoOp},
        {"QCmd", LuaNoOp},
        {"GiveQCommand", LuaNoOp},
        {"UnitCmd", LuaNoOp},
        {"UnitQCmd", LuaNoOp},
        {"LandReinforcement", LuaNoOp},
        {"LandReinforcementFromMap", LuaNoOp},
        {"CallReinforcement", LuaNoOp},
        {"Trace", LuaNoOp},
        {"DisplayTrace", LuaNoOp},
        {"ChangeFormation", LuaNoOp},
        {"GetNUnitsInArea", LuaCountUnitsInArea},
        {"IsSomeUnitInArea", LuaIsSomeUnitInArea},
        {"IsSomePlayerUnit", LuaIsSomePlayerUnit},
        {"IsSomeBodyAlive", LuaIsSomeBodyAlive},
        {"GetObjectList", LuaGetObjectList},
        {"IsAlive", LuaIsAlive},
        {"DamageObject", LuaDamageObject},
        {"ChangePlayer", LuaChangePlayer},
        {"ObjectGetCoord", LuaObjectGetCoord},
        {"GetReinforcementCallsLeft", LuaGetReinforcementCallsLeft},
        {"GetScriptAreaParams", LuaGetScriptAreaParams},
        {"SetIGlobalVar", LuaSetGlobalInt},
        {"GetIGlobalVar", LuaGetGlobalInt},
        {"SetFGlobalVar", LuaNoOp},
        {"SetSGlobalVar", LuaNoOp},
        {"GetFGlobalVar", LuaZero},
        {"GetSGlobalVar", LuaZero},
        {"ObjectiveChanged", LuaObjectiveChanged},
        {"Win", LuaWin},
        {"Loose", LuaLoose},
        {"Draw", LuaNoOp},
        {"RandomInt", LuaRandomInt},
        {"RandomFloat", LuaRandomInt},
        {"GetDifficultyLevel", LuaGetDifficulty},
        {"GetGameTime", LuaGetGameTime},
        {"GetNUnitsInCircle", LuaZero},
        {"GetNUnitsInScriptGroup", LuaZero},
        {"GetNScriptUnitsInArea", LuaZero},
        {"GetNUnitsInParty", LuaZero},
        {"GetNUnitsInPartyUF", LuaZero},
        {"GetNUnitsInPlayerUF", LuaZero},
        {"GetNUnitsInSide", LuaZero},
        {"GetNUnitsOfType", LuaZero},
        {"IsUnitInArea", LuaZero},
        {"IsUnitNearScriptObject", LuaZero},
        {"IsSomeUnitInParty", LuaZero},
        {"GetObjectHPs", LuaZero},
        {"GetUnitState", LuaZero},
        {"GetAmmo", LuaZero},
        {"GetNAmmo", LuaZero},
        {"IsImmobilized", LuaZero},
        {"IsFollowing", LuaZero},
        {"IsStandGround", LuaZero},
        {"IsEntrenched", LuaZero},
        {"CheckMissionBonus", LuaZero},
        {"GetPlayersMask", LuaZero},
        {"IsPlayerPresent", LuaZero},
        {"GetObjectList", LuaGetObjectList},
        {"UnitRemove", LuaNoOp},
        {"GameMesage", LuaNoOp},
        {"ChangeWarFog", LuaNoOp},
        {"SwitchWeather", LuaNoOp},
        {"SwitchWeatherAutomatic", LuaNoOp},
        {"SetGameSpeed", LuaNoOp},
        {"StartSequenceWOMovieBorder", LuaNoOp},
        {"EndSequenceWOMovieBorder", LuaNoOp},
        {"ShowMovieBorder", LuaNoOp},
        {"HideMovieBorder", LuaNoOp},
        {"SCRunTime", LuaNoOp},
        {"SCRunSpeed", LuaNoOp},
        {"SCReset", LuaNoOp},
        {"SCStartMovie", LuaNoOp},
        {"SCStopMovie", LuaNoOp},
        {nullptr, nullptr},
};

bool RunScriptFile(IScriptWrapper* script, const char* path) {
    if (script == nullptr || path == nullptr || path[0] == 0) {
        return false;
    }
    return script->RunScriptFile(path) != 0;
}

}  // namespace

bool InitializeMissionScriptRuntime(
        const NDb::SMapInfo* map,
        std::string* error) {
    ShutdownMissionScriptRuntime();
    if (map == nullptr) {
        if (error != nullptr) {
            *error = "mission_script_map_missing";
        }
        return false;
    }

    g_map = map;
    AddScriptObjects(map->objects);
    AddScriptObjects(map->scenarioObjects);

    g_script = CreateScriptWrapper();
    if (!IsValid(g_script)) {
        if (error != nullptr) {
            *error = "mission_script_runtime_allocation_failed";
        }
        ShutdownMissionScriptRuntime();
        return false;
    }
    g_script->Init();
    g_script->AddRegFunctions(kMissionFunctions);

    const NDb::SAIGameConsts* ai_consts = NGameX::GetAIConsts();
    if (ai_consts == nullptr) {
        if (error != nullptr) {
            *error = "mission_script_ai_consts_missing";
        }
        ShutdownMissionScriptRuntime();
        return false;
    }
    for (size_t i = 0; i < ai_consts->commonScriptFileRefs.size(); ++i) {
        const char* path =
                ai_consts->commonScriptFileRefs[i].szScriptFileRef.c_str();
        if (!RunScriptFile(g_script, path)) {
            if (error != nullptr) {
                *error = std::string("common_script_load_failed:") + path;
            }
            ShutdownMissionScriptRuntime();
            return false;
        }
        ++g_common_script_count;
    }
    g_script->Segment();

    g_script_path = map->szScriptFileRef.c_str();
    if (g_script_path.empty() ||
        !RunScriptFile(g_script, g_script_path.c_str())) {
        if (error != nullptr) {
            *error = std::string("mission_script_load_failed:") +
                     g_script_path;
        }
        ShutdownMissionScriptRuntime();
        return false;
    }
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

void TickMissionScriptRuntime(uint32_t elapsed_millis) {
    if (!IsValid(g_script) || g_outcome != "running") {
        return;
    }
    g_segment_accumulator += elapsed_millis;
    while (g_segment_accumulator >= kScriptSegmentMillis) {
        g_segment_accumulator -= kScriptSegmentMillis;
        g_script->Segment();
        ++g_segment_count;
    }
}

void ShutdownMissionScriptRuntime() {
    g_script = 0;
    g_map = nullptr;
    g_objects.clear();
    g_script_path.clear();
    g_outcome = "running";
    g_segment_accumulator = 0;
    g_segment_count = 0;
    g_common_script_count = 0;
    g_objective_events = 0;
    g_runtime_errors = 0;
}

std::string MissionScriptRuntimeReport() {
    std::ostringstream report;
    report << "mission_script="
           << (IsValid(g_script) ? "ready" : "not_ready")
           << "; path=" << (g_script_path.empty() ? "<none>" : g_script_path)
           << "; common_scripts=" << g_common_script_count
           << "; unit_objects=" << g_objects.size()
           << "; segments=" << g_segment_count
           << "; objective_events=" << g_objective_events
           << "; outcome=" << g_outcome
           << "; errors=" << g_runtime_errors;
    return report.str();
}

}  // namespace bk2::android
