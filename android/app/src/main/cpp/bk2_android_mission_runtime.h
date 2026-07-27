#pragma once

#include <string>
#include <vector>

namespace bk2::android {

struct MissionObjectiveState {
    std::string dbid;
    std::string header_ref;
    std::string briefing_ref;
    std::string description_ref;
    int state = 0;
    int map_positions = 0;
    int experience = 0;
    bool primary = false;
};

struct MissionMedalState {
    std::string dbid;
    std::string name_ref;
    std::string description_ref;
    int source = 0;
};

struct MissionReinforcementState {
    int type = -1;
    int state = 0;
    bool from_previous_chapter = false;
    std::string dbid;
    std::string name_ref;
    std::string description_ref;
};

struct MissionReinforcementProgressState {
    int type = -1;
    int xp = 0;
    int level = 0;
    int next_level_xp = 0;
    int favorite_count = 0;
    bool max_level = false;
};

struct MissionLeaderState {
    int reinforcement_type = -1;
    int leader_index = -1;
    int rank = 0;
    int xp = 0;
    int xp_debt = 0;
    int next_rank_xp = 0;
    int units_killed = 0;
    int units_lost = 0;
    int stored_rank = 0;
    int stored_xp = 0;
    int stored_xp_debt = 0;
    int stored_units_killed = 0;
    int stored_units_lost = 0;
    bool assigned = false;
    std::string name_ref;
    std::string rank_name_ref;
};

enum MissionStatisticKind {
    kMissionStatisticTime = 0,
    kMissionStatisticCampaignTime = 1,
    kMissionStatisticExpEarned = 2,
    kMissionStatisticCampaignExpCurrent = 3,
    kMissionStatisticCampaignExpNextLevel = 4,
    kMissionStatisticUnitsLost = 5,
    kMissionStatisticUnitsKilled = 6,
    kMissionStatisticKeyBuildingsCaptured = 7,
    kMissionStatisticReinforcementsCalled = 8,
    kMissionStatisticScore = 9,
    kMissionStatisticUnitsLostPrice = 10,
    kMissionStatisticUnitsKilledPrice = 11,
    kMissionStatisticEnemyUnitsMaxPrice = 12,
    kMissionStatisticCampaignUnitsLost = 13,
    kMissionStatisticCampaignUnitsKilled = 14,
    kMissionStatisticCampaignMissionsPassed = 15,
    kMissionStatisticTacticalEfficiency = 16,
    kMissionStatisticStrategicEfficiency = 17,
};

struct MissionRuntimeState {
    bool active = false;
    bool mission_active = false;
    bool campaign_active = false;
    bool chapter_active = false;
    bool campaign_finished = false;
    bool chapter_finished = false;
    bool tutorial = false;
    bool custom = false;
    int campaign_index = -1;
    int chapter_index = -1;
    int mission_index = -1;
    int difficulty = 0;
    int recommended_calls = 0;
    int mission_enable_type = 0;
    int mission_type = 0;
    int campaign_chapter_count = 0;
    int chapter_mission_count = 0;
    int completed_mission_count = 0;
    int enabled_mission_count = 0;
    int missions_to_enable_count = 0;
    int continued_mission_starts = 0;
    int player_count = 0;
    int objective_count = 0;
    int waiting_objective_count = 0;
    int received_objective_count = 0;
    int completed_objective_count = 0;
    int failed_objective_count = 0;
    int primary_objective_count = 0;
    int script_movie_sequences = 0;
    int script_camera_placements = 0;
    int chapter_reinforcement_calls_left = 0;
    int chapter_reinforcement_calls_old = 0;
    int mission_reinforcement_calls_left = 0;
    int enemy_reinforcement_calls_left = 0;
    int reinforcement_calls_used = 0;
    int main_enemy_player = -1;
    int player_xp = 0;
    int player_xp_added = 0;
    int player_rank_index = -1;
    int player_rank_promotions = 0;
    int player_rank_promotions_added = 0;
    int campaign_exp_current = 0;
    int campaign_exp_next_level = 0;
    int mission_time_seconds = 0;
    int campaign_time_seconds = 0;
    int mission_exp_earned = 0;
    int mission_units_lost = 0;
    int mission_units_killed = 0;
    int mission_units_lost_price = 0;
    int mission_units_killed_price = 0;
    int mission_key_buildings_captured = 0;
    int mission_reinforcements_called = 0;
    int mission_enemy_units_max_price = 0;
    int mission_score = 0;
    int mission_kill_events = 0;
    int mission_price_kill_events = 0;
    int kill_matrix_player_count = 0;
    int last_kill_player = -1;
    int last_killed_player = -1;
    int last_kill_reinforcement_type = -1;
    int last_killed_reinforcement_type = -1;
    int last_kill_exp_price = 0;
    bool last_kill_leveled_up = false;
    int campaign_units_lost = 0;
    int campaign_units_killed = 0;
    int campaign_missions_passed = 0;
    int tactical_efficiency = 0;
    int strategic_efficiency = 0;
    int reward_bonus_reinforcements = 0;
    int reward_disabled_reinforcements = 0;
    int reward_added_calls = 0;
    int chapter_reinforcement_inventory_count = 0;
    int chapter_reinforcements_enabled = 0;
    int chapter_reinforcements_not_enabled = 0;
    int chapter_reinforcements_disabled = 0;
    int chapter_reinforcements_from_previous = 0;
    int old_chapter_reinforcement_inventory_count = 0;
    int reinforcement_progress_count = 0;
    int reinforcement_max_level = 3;
    int favorite_reinforcement_type = -1;
    int favorite_reinforcement_count = 0;
    int leader_rank_count = 0;
    int leader_pool_count = 0;
    int free_leader_count = 0;
    int assigned_leader_count = 0;
    int mission_medals_awarded = 0;
    int medal_kills_given = 0;
    int medal_tactics_given = 0;
    int medal_economy_given = 0;
    int medal_munchkin_given = 0;
    bool medal_munchkin_blocked_by_reinforcement_xp = false;
    bool use_map_reinforcements = false;
    bool only_recommended_reinforcement_calls = false;
    bool mission_won = false;
    bool mission_cancelled = false;
    bool started_from_existing_campaign_state = false;
    std::string campaign_id;
    std::string chapter_id;
    std::string mission_id;
    std::string campaign_script_ref;
    std::string chapter_script_ref;
    std::string map_data_ref;
    std::string map_script_ref;
    std::string intro_movie_ref;
    std::string player_rank_id;
    std::string player_rank_name_ref;
    std::string new_player_rank_id;
    std::string new_player_rank_name_ref;
    std::vector<std::string> won_mission_ids;
    std::vector<std::string> enabled_mission_ids;
    std::vector<std::string> completed_mission_ids;
    std::vector<std::string> reward_reinforcement_ids;
    std::vector<int> current_player_reinforcement_types;
    std::vector<int> reward_reinforcement_types;
    std::vector<int> reward_disabled_reinforcement_types;
    std::vector<MissionReinforcementState> chapter_reinforcements;
    std::vector<MissionReinforcementState> old_chapter_reinforcements;
    std::vector<MissionReinforcementProgressState> reinforcement_progress;
    std::vector<MissionLeaderState> leaders;
    std::vector<int> free_leader_indices;
    std::vector<int> player_sides;
    std::vector<int> kill_matrix;
    std::vector<int> price_kill_matrix;
    std::vector<MissionMedalState> mission_medals;
    std::vector<MissionObjectiveState> objectives;
};

struct MissionRuntimeResult {
    bool ok = false;
    std::string error;
    MissionRuntimeState state;
};

MissionRuntimeResult StartCampaignMissionState(
        int campaign_index,
        int chapter_index,
        int mission_index,
        int difficulty);

MissionRuntimeResult StartDirectMissionState(const std::string& mission_id, int difficulty);
MissionRuntimeResult StartTutorialMissionState(int tutorial_index, int difficulty);
MissionRuntimeResult StartFirstCampaignMissionState();
MissionRuntimeResult StartCurrentCampaignMissionState(int mission_index, int difficulty);
MissionRuntimeResult StartFirstEnabledCampaignMissionState(int difficulty);
MissionRuntimeResult SetMissionObjectiveState(int objective_index, int state);
MissionRuntimeResult AddPlayerXp(int xp);
MissionRuntimeResult SetMissionStatistic(int statistic_kind, int value);
MissionRuntimeResult AddMissionStatistic(int statistic_kind, int delta);
MissionRuntimeResult AssignLeaderToReinforcement(int reinforcement_type, int free_leader_slot);
MissionRuntimeResult GiveReinforcementXp(int reinforcement_type, int xp);
MissionRuntimeResult MarkFavoriteReinforcement(int reinforcement_type);
MissionRuntimeResult RegisterUnitKill(
        int player,
        int unit_type,
        int reinforcement_type,
        int killed_player,
        int killed_unit_type,
        int killed_reinforcement_type,
        int exp_price,
        bool infantry_kill);
MissionRuntimeResult MarkMissionWon();
MissionRuntimeResult CancelMission();
MissionRuntimeResult AdvanceToNextChapter();
MissionRuntimeResult DecreaseReinforcementCallsLeft(int player, int calls);
MissionRuntimeResult IncreaseReinforcementCallsLeft(int player, int calls);
MissionRuntimeResult RegisterReinforcementCall(int player);
std::string SerializeMissionRuntimeState(const MissionRuntimeState& state);
MissionRuntimeResult RestoreMissionRuntimeState(const std::string& checkpoint);
MissionRuntimeResult SaveMissionRuntimeCheckpoint(const std::string& slot_name);
MissionRuntimeResult LoadMissionRuntimeCheckpoint(const std::string& slot_name);
MissionRuntimeState GetMissionRuntimeState();
void ResetMissionRuntimeState();
std::string DescribeMissionRuntimeState(const MissionRuntimeState& state);
std::string RunFirstCampaignMissionProgressionProbe();
std::string RunMissionCheckpointProbe();

}  // namespace bk2::android
