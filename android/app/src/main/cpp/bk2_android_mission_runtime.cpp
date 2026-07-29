#include "bk2_android_mission_runtime.h"

#include "bk2_android_database.h"
#include "bk2_android_legacy_game_runtime.h"
#include "bk2_android_platform.h"
#include "bk2_android_vfs.h"
#include "bk2_port_paths.h"

#include "System/stdafx.h"
#include "AILogic/DBAIConsts.h"
#include "B2_M1_World/MissionObjectiveStates.h"
#include "GameX/DBGameRoot.h"
#include "GameX/GetConsts.h"
#include "Main/Profiles.h"
#include "Stats_B2_M1/DBMapInfo.h"
#include "System/Streams.h"
#include "System/VFSOperations.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <jni.h>
#include <limits>
#include <map>
#include <mutex>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace bk2::android {
namespace {

std::mutex g_mission_mutex;
MissionRuntimeState g_state;
std::mutex g_text_cache_mutex;
std::map<std::string, std::string> g_text_cache;
std::mutex g_hud_notification_mutex;

struct MissionHudNotification {
    std::string text;
    uint64_t expires_at_millis = 0;
};

std::deque<MissionHudNotification> g_hud_notifications;

constexpr uint32_t kMissionHudNotificationVisibleMillis = 5000;
constexpr size_t kMaxQueuedMissionHudNotifications = 16;

struct MissionLocation {
    int campaign_index = -1;
    int chapter_index = -1;
    int mission_index = -1;
    bool has_objectives = false;
    bool has_rewards = false;
};

enum MissionReinforcementStateValue {
    kReinforcementDisabled = 0,
    kReinforcementNotEnabled = 1,
    kReinforcementEnabled = 2,
};

constexpr int kMaxReinforcementXpLevel = 3;
constexpr int kMissionCheckpointVersion = 1;

std::string ToStdString(const string& value) {
    return value.c_str();
}

void AppendUtf8(std::uint32_t code_point, std::string* out) {
    if (code_point <= 0x7f) {
        out->push_back(static_cast<char>(code_point));
    } else if (code_point <= 0x7ff) {
        out->push_back(static_cast<char>(0xc0 | (code_point >> 6)));
        out->push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
    } else if (code_point <= 0xffff) {
        out->push_back(static_cast<char>(0xe0 | (code_point >> 12)));
        out->push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3f)));
        out->push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
    } else if (code_point <= 0x10ffff) {
        out->push_back(static_cast<char>(0xf0 | (code_point >> 18)));
        out->push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3f)));
        out->push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3f)));
        out->push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
    }
}

std::string LoadUtf16Text(const std::string& file_ref) {
    if (file_ref.empty() || NVFS::GetMainVFS() == nullptr) {
        return std::string();
    }

    std::lock_guard<std::mutex> cache_guard(g_text_cache_mutex);
    const std::map<std::string, std::string>::const_iterator cached =
            g_text_cache.find(file_ref);
    if (cached != g_text_cache.end()) {
        return cached->second;
    }

    CFileStream stream(NVFS::GetMainVFS(), string(file_ref.c_str()));
    const int byte_count = stream.GetSize();
    std::string text;
    if (!stream.IsOk() || byte_count < 2) {
        g_text_cache[file_ref] = text;
        return text;
    }
    const unsigned char* bytes = stream.GetBuffer();
    if (bytes == nullptr) {
        g_text_cache[file_ref] = text;
        return text;
    }

    bool little_endian = true;
    int offset = 0;
    if (bytes[0] == 0xff && bytes[1] == 0xfe) {
        offset = 2;
    } else if (bytes[0] == 0xfe && bytes[1] == 0xff) {
        little_endian = false;
        offset = 2;
    }

    bool pending_space = false;
    int character_count = 0;
    while (offset + 1 < byte_count && character_count < 120) {
        const std::uint16_t first = little_endian
                ? static_cast<std::uint16_t>(bytes[offset] | (bytes[offset + 1] << 8))
                : static_cast<std::uint16_t>((bytes[offset] << 8) | bytes[offset + 1]);
        offset += 2;

        std::uint32_t code_point = first;
        if (first >= 0xd800 && first <= 0xdbff && offset + 1 < byte_count) {
            const std::uint16_t second = little_endian
                    ? static_cast<std::uint16_t>(bytes[offset] | (bytes[offset + 1] << 8))
                    : static_cast<std::uint16_t>((bytes[offset] << 8) | bytes[offset + 1]);
            if (second >= 0xdc00 && second <= 0xdfff) {
                code_point = 0x10000 +
                        ((static_cast<std::uint32_t>(first) - 0xd800) << 10) +
                        (static_cast<std::uint32_t>(second) - 0xdc00);
                offset += 2;
            }
        }

        if (code_point == 0 || code_point == 0xfeff) {
            continue;
        }
        if (code_point == '\r' || code_point == '\n' || code_point == '\t' ||
            code_point == ' ') {
            pending_space = !text.empty();
            continue;
        }
        if (pending_space) {
            text.push_back(' ');
            pending_space = false;
        }
        AppendUtf8(code_point, &text);
        ++character_count;
    }

    g_text_cache[file_ref] = text;
    return text;
}

std::string JoinPath(const std::string& left, const std::string& right) {
    if (left.empty()) {
        return right;
    }
    if (right.empty()) {
        return left;
    }
    return left[left.size() - 1] == '/' ? left + right : left + "/" + right;
}

std::string NormalizeLegacyRelative(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    while (!path.empty() && path[0] == '/') {
        path.erase(0, 1);
    }
    return path;
}

std::string AbsoluteFromLegacyRelative(const std::string& legacy_path) {
    const PortPaths paths = GetPortPaths();
    const std::string normalized = NormalizeLegacyRelative(legacy_path);
    if (!paths.files_dir.empty()) {
        return JoinPath(paths.files_dir, normalized);
    }
    return normalized;
}

bool IsDirectory(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool EnsureDirectory(const std::string& directory) {
    if (directory.empty() || IsDirectory(directory)) {
        return true;
    }

    std::string current;
    size_t position = 0;
    if (directory[0] == '/') {
        current = "/";
        position = 1;
    }

    while (position <= directory.size()) {
        const size_t slash = directory.find('/', position);
        const std::string part = directory.substr(position, slash - position);
        if (!part.empty()) {
            current = current == "/" ? current + part : JoinPath(current, part);
            if (mkdir(current.c_str(), 0775) != 0 && errno != EEXIST) {
                return false;
            }
        }
        if (slash == std::string::npos) {
            break;
        }
        position = slash + 1;
    }
    return true;
}

std::string SanitizeCheckpointSlot(std::string slot_name) {
    if (slot_name.empty()) {
        slot_name = "android_autosave";
    }
    for (std::string::iterator it = slot_name.begin(); it != slot_name.end(); ++it) {
        const unsigned char ch = static_cast<unsigned char>(*it);
        if (!(ch >= 'a' && ch <= 'z') &&
            !(ch >= 'A' && ch <= 'Z') &&
            !(ch >= '0' && ch <= '9') &&
            ch != '_' && ch != '-' && ch != '.') {
            *it = '_';
        }
    }
    if (slot_name.find('.') == std::string::npos) {
        slot_name += ".bk2checkpoint";
    }
    return slot_name;
}

std::string CurrentCheckpointPath(const std::string& slot_name) {
    const std::string legacy_profile_dir = std::string(NProfile::GetCurrentProfileDir().c_str());
    const std::string legacy_save_dir = legacy_profile_dir + "Saves\\";
    return JoinPath(AbsoluteFromLegacyRelative(legacy_save_dir), SanitizeCheckpointSlot(slot_name));
}

std::int64_t FileSize(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 ? static_cast<std::int64_t>(st.st_size) : -1;
}

std::string HexEncode(const std::string& value) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(value.size() * 2);
    for (std::string::const_iterator it = value.begin(); it != value.end(); ++it) {
        const unsigned char ch = static_cast<unsigned char>(*it);
        out.push_back(kHex[ch >> 4]);
        out.push_back(kHex[ch & 0x0f]);
    }
    return out;
}

int HexValue(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

bool HexDecode(const std::string& value, std::string* out) {
    if (out == nullptr || value.size() % 2 != 0) {
        return false;
    }
    out->clear();
    out->reserve(value.size() / 2);
    for (size_t i = 0; i < value.size(); i += 2) {
        const int high = HexValue(value[i]);
        const int low = HexValue(value[i + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        out->push_back(static_cast<char>((high << 4) | low));
    }
    return true;
}

bool ParseInt(const std::string& value, int* out) {
    if (out == nullptr || value.empty()) {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const long parsed = strtol(value.c_str(), &end, 10);
    if (errno != 0 || end == value.c_str() || *end != 0) {
        return false;
    }
    *out = static_cast<int>(parsed);
    return true;
}

bool ParseBool(const std::string& value, bool* out) {
    if (out == nullptr) {
        return false;
    }
    if (value == "1") {
        *out = true;
        return true;
    }
    if (value == "0") {
        *out = false;
        return true;
    }
    return false;
}

std::vector<std::string> SplitPreserveEmpty(const std::string& value, char separator) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= value.size()) {
        const size_t pos = value.find(separator, start);
        if (pos == std::string::npos) {
            out.push_back(value.substr(start));
            break;
        }
        out.push_back(value.substr(start, pos - start));
        start = pos + 1;
    }
    return out;
}

void AppendCheckpointValue(std::ostringstream& out, const char* key, int value) {
    out << key << "=" << value << "\n";
}

void AppendCheckpointValue(std::ostringstream& out, const char* key, bool value) {
    out << key << "=" << (value ? 1 : 0) << "\n";
}

void AppendCheckpointString(std::ostringstream& out, const char* key, const std::string& value) {
    out << key << "=" << HexEncode(value) << "\n";
}

std::string EncodeIntVector(const std::vector<int>& values) {
    std::ostringstream out;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            out << ",";
        }
        out << values[i];
    }
    return out.str();
}

std::string EncodeStringVector(const std::vector<std::string>& values) {
    std::ostringstream out;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            out << ",";
        }
        out << HexEncode(values[i]);
    }
    return out.str();
}

void AppendCheckpointRaw(std::ostringstream& out, const char* key, const std::string& value) {
    out << key << "=" << value << "\n";
}

bool ReadCheckpointLines(const std::string& checkpoint, std::map<std::string, std::string>* values) {
    if (values == nullptr) {
        return false;
    }
    values->clear();
    size_t start = 0;
    while (start <= checkpoint.size()) {
        const size_t end = checkpoint.find('\n', start);
        std::string line = end == std::string::npos
                ? checkpoint.substr(start)
                : checkpoint.substr(start, end - start);
        if (!line.empty() && line[line.size() - 1] == '\r') {
            line.resize(line.size() - 1);
        }
        if (!line.empty()) {
            const size_t equals = line.find('=');
            if (equals == std::string::npos) {
                return false;
            }
            (*values)[line.substr(0, equals)] = line.substr(equals + 1);
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return true;
}

bool ReadIntField(const std::map<std::string, std::string>& values, const char* key, int* out) {
    const std::map<std::string, std::string>::const_iterator it = values.find(key);
    return it == values.end() ? true : ParseInt(it->second, out);
}

bool ReadBoolField(const std::map<std::string, std::string>& values, const char* key, bool* out) {
    const std::map<std::string, std::string>::const_iterator it = values.find(key);
    return it == values.end() ? true : ParseBool(it->second, out);
}

bool ReadStringField(const std::map<std::string, std::string>& values, const char* key, std::string* out) {
    const std::map<std::string, std::string>::const_iterator it = values.find(key);
    return it == values.end() ? true : HexDecode(it->second, out);
}

bool ReadIntVector(const std::map<std::string, std::string>& values, const char* key, std::vector<int>* out) {
    const std::map<std::string, std::string>::const_iterator it = values.find(key);
    if (it == values.end()) {
        return true;
    }
    out->clear();
    if (it->second.empty()) {
        return true;
    }
    const std::vector<std::string> items = SplitPreserveEmpty(it->second, ',');
    for (std::vector<std::string>::const_iterator item = items.begin(); item != items.end(); ++item) {
        int value = 0;
        if (!ParseInt(*item, &value)) {
            return false;
        }
        out->push_back(value);
    }
    return true;
}

bool ReadStringVector(const std::map<std::string, std::string>& values, const char* key, std::vector<std::string>* out) {
    const std::map<std::string, std::string>::const_iterator it = values.find(key);
    if (it == values.end()) {
        return true;
    }
    out->clear();
    if (it->second.empty()) {
        return true;
    }
    const std::vector<std::string> items = SplitPreserveEmpty(it->second, ',');
    for (std::vector<std::string>::const_iterator item = items.begin(); item != items.end(); ++item) {
        std::string value;
        if (!HexDecode(*item, &value)) {
            return false;
        }
        out->push_back(value);
    }
    return true;
}

std::string EncodeReinforcements(const std::vector<MissionReinforcementState>& values) {
    std::ostringstream out;
    for (size_t i = 0; i < values.size(); ++i) {
        const MissionReinforcementState& value = values[i];
        if (i != 0) {
            out << ";";
        }
        out << value.type << "|" << value.state << "|" << (value.from_previous_chapter ? 1 : 0)
            << "|" << HexEncode(value.dbid)
            << "|" << HexEncode(value.name_ref)
            << "|" << HexEncode(value.description_ref);
    }
    return out.str();
}

bool DecodeReinforcements(const std::map<std::string, std::string>& values, const char* key, std::vector<MissionReinforcementState>* out) {
    const std::map<std::string, std::string>::const_iterator it = values.find(key);
    if (it == values.end()) {
        return true;
    }
    out->clear();
    if (it->second.empty()) {
        return true;
    }
    const std::vector<std::string> records = SplitPreserveEmpty(it->second, ';');
    for (std::vector<std::string>::const_iterator record = records.begin(); record != records.end(); ++record) {
        const std::vector<std::string> fields = SplitPreserveEmpty(*record, '|');
        if (fields.size() != 6) {
            return false;
        }
        MissionReinforcementState value;
        if (!ParseInt(fields[0], &value.type) ||
            !ParseInt(fields[1], &value.state) ||
            !ParseBool(fields[2], &value.from_previous_chapter) ||
            !HexDecode(fields[3], &value.dbid) ||
            !HexDecode(fields[4], &value.name_ref) ||
            !HexDecode(fields[5], &value.description_ref)) {
            return false;
        }
        out->push_back(value);
    }
    return true;
}

std::string EncodeReinforcementProgress(const std::vector<MissionReinforcementProgressState>& values) {
    std::ostringstream out;
    for (size_t i = 0; i < values.size(); ++i) {
        const MissionReinforcementProgressState& value = values[i];
        if (i != 0) {
            out << ";";
        }
        out << value.type << "|" << value.xp << "|" << value.level << "|"
            << value.next_level_xp << "|" << value.favorite_count << "|"
            << (value.max_level ? 1 : 0);
    }
    return out.str();
}

bool DecodeReinforcementProgress(
        const std::map<std::string, std::string>& values,
        const char* key,
        std::vector<MissionReinforcementProgressState>* out) {
    const std::map<std::string, std::string>::const_iterator it = values.find(key);
    if (it == values.end()) {
        return true;
    }
    out->clear();
    if (it->second.empty()) {
        return true;
    }
    const std::vector<std::string> records = SplitPreserveEmpty(it->second, ';');
    for (std::vector<std::string>::const_iterator record = records.begin(); record != records.end(); ++record) {
        const std::vector<std::string> fields = SplitPreserveEmpty(*record, '|');
        if (fields.size() != 6) {
            return false;
        }
        MissionReinforcementProgressState value;
        if (!ParseInt(fields[0], &value.type) ||
            !ParseInt(fields[1], &value.xp) ||
            !ParseInt(fields[2], &value.level) ||
            !ParseInt(fields[3], &value.next_level_xp) ||
            !ParseInt(fields[4], &value.favorite_count) ||
            !ParseBool(fields[5], &value.max_level)) {
            return false;
        }
        out->push_back(value);
    }
    return true;
}

std::string EncodeLeaders(const std::vector<MissionLeaderState>& values) {
    std::ostringstream out;
    for (size_t i = 0; i < values.size(); ++i) {
        const MissionLeaderState& value = values[i];
        if (i != 0) {
            out << ";";
        }
        out << value.reinforcement_type << "|" << value.leader_index << "|"
            << value.rank << "|" << value.xp << "|" << value.xp_debt << "|"
            << value.next_rank_xp << "|" << value.units_killed << "|"
            << value.units_lost << "|" << value.stored_rank << "|"
            << value.stored_xp << "|" << value.stored_xp_debt << "|"
            << value.stored_units_killed << "|" << value.stored_units_lost << "|"
            << (value.assigned ? 1 : 0) << "|" << HexEncode(value.name_ref)
            << "|" << HexEncode(value.rank_name_ref);
    }
    return out.str();
}

bool DecodeLeaders(const std::map<std::string, std::string>& values, const char* key, std::vector<MissionLeaderState>* out) {
    const std::map<std::string, std::string>::const_iterator it = values.find(key);
    if (it == values.end()) {
        return true;
    }
    out->clear();
    if (it->second.empty()) {
        return true;
    }
    const std::vector<std::string> records = SplitPreserveEmpty(it->second, ';');
    for (std::vector<std::string>::const_iterator record = records.begin(); record != records.end(); ++record) {
        const std::vector<std::string> fields = SplitPreserveEmpty(*record, '|');
        if (fields.size() != 16) {
            return false;
        }
        MissionLeaderState value;
        if (!ParseInt(fields[0], &value.reinforcement_type) ||
            !ParseInt(fields[1], &value.leader_index) ||
            !ParseInt(fields[2], &value.rank) ||
            !ParseInt(fields[3], &value.xp) ||
            !ParseInt(fields[4], &value.xp_debt) ||
            !ParseInt(fields[5], &value.next_rank_xp) ||
            !ParseInt(fields[6], &value.units_killed) ||
            !ParseInt(fields[7], &value.units_lost) ||
            !ParseInt(fields[8], &value.stored_rank) ||
            !ParseInt(fields[9], &value.stored_xp) ||
            !ParseInt(fields[10], &value.stored_xp_debt) ||
            !ParseInt(fields[11], &value.stored_units_killed) ||
            !ParseInt(fields[12], &value.stored_units_lost) ||
            !ParseBool(fields[13], &value.assigned) ||
            !HexDecode(fields[14], &value.name_ref) ||
            !HexDecode(fields[15], &value.rank_name_ref)) {
            return false;
        }
        out->push_back(value);
    }
    return true;
}

std::string EncodeMedals(const std::vector<MissionMedalState>& values) {
    std::ostringstream out;
    for (size_t i = 0; i < values.size(); ++i) {
        const MissionMedalState& value = values[i];
        if (i != 0) {
            out << ";";
        }
        out << HexEncode(value.dbid) << "|" << HexEncode(value.name_ref) << "|"
            << HexEncode(value.description_ref) << "|" << value.source;
    }
    return out.str();
}

bool DecodeMedals(const std::map<std::string, std::string>& values, const char* key, std::vector<MissionMedalState>* out) {
    const std::map<std::string, std::string>::const_iterator it = values.find(key);
    if (it == values.end()) {
        return true;
    }
    out->clear();
    if (it->second.empty()) {
        return true;
    }
    const std::vector<std::string> records = SplitPreserveEmpty(it->second, ';');
    for (std::vector<std::string>::const_iterator record = records.begin(); record != records.end(); ++record) {
        const std::vector<std::string> fields = SplitPreserveEmpty(*record, '|');
        if (fields.size() != 4) {
            return false;
        }
        MissionMedalState value;
        if (!HexDecode(fields[0], &value.dbid) ||
            !HexDecode(fields[1], &value.name_ref) ||
            !HexDecode(fields[2], &value.description_ref) ||
            !ParseInt(fields[3], &value.source)) {
            return false;
        }
        out->push_back(value);
    }
    return true;
}

std::string EncodeObjectives(const std::vector<MissionObjectiveState>& values) {
    std::ostringstream out;
    for (size_t i = 0; i < values.size(); ++i) {
        const MissionObjectiveState& value = values[i];
        if (i != 0) {
            out << ";";
        }
        out << HexEncode(value.dbid) << "|" << HexEncode(value.header_ref) << "|"
            << HexEncode(value.briefing_ref) << "|" << HexEncode(value.description_ref)
            << "|" << value.state << "|" << value.map_positions << "|"
            << value.experience << "|" << (value.primary ? 1 : 0);
    }
    return out.str();
}

bool DecodeObjectives(const std::map<std::string, std::string>& values, const char* key, std::vector<MissionObjectiveState>* out) {
    const std::map<std::string, std::string>::const_iterator it = values.find(key);
    if (it == values.end()) {
        return true;
    }
    out->clear();
    if (it->second.empty()) {
        return true;
    }
    const std::vector<std::string> records = SplitPreserveEmpty(it->second, ';');
    for (std::vector<std::string>::const_iterator record = records.begin(); record != records.end(); ++record) {
        const std::vector<std::string> fields = SplitPreserveEmpty(*record, '|');
        if (fields.size() != 8) {
            return false;
        }
        MissionObjectiveState value;
        if (!HexDecode(fields[0], &value.dbid) ||
            !HexDecode(fields[1], &value.header_ref) ||
            !HexDecode(fields[2], &value.briefing_ref) ||
            !HexDecode(fields[3], &value.description_ref) ||
            !ParseInt(fields[4], &value.state) ||
            !ParseInt(fields[5], &value.map_positions) ||
            !ParseInt(fields[6], &value.experience) ||
            !ParseBool(fields[7], &value.primary)) {
            return false;
        }
        out->push_back(value);
    }
    return true;
}

std::string FileRef(const NFile::CFilePath& value) {
    return value.empty() ? std::string() : std::string(value.c_str());
}

std::string DbIdOf(const NDb::CResource* resource) {
    return resource != nullptr ? ToStdString(resource->GetDBID().ToString()) : std::string();
}

template <class T>
std::string DbIdOfPtr(const CDBPtr<T>& ptr) {
    return DbIdOf(ptr.GetBarePtrNoLoad());
}

bool EnsureDatabaseReady(MissionRuntimeResult* result) {
    if (!InitializeLegacyVfs()) {
        result->error = "legacy_vfs_failed";
        return false;
    }
    if (!InitializeLegacyDatabase()) {
        result->error = "legacy_database_closed";
        return false;
    }
    return true;
}

bool ContainsString(const std::vector<std::string>& values, const std::string& value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

bool ContainsInt(const std::vector<int>& values, int value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

void PushUniqueInt(std::vector<int>* values, int value) {
    if (values != nullptr && !ContainsInt(*values, value)) {
        values->push_back(value);
    }
}

MissionReinforcementState ReinforcementStateFromDb(
        const NDb::SReinforcement* reinforcement,
        int state,
        bool from_previous_chapter) {
    MissionReinforcementState result;
    result.type = reinforcement == nullptr ? -1 : reinforcement->eType;
    result.state = state;
    result.from_previous_chapter = from_previous_chapter;
    result.dbid = DbIdOf(reinforcement);
    if (reinforcement != nullptr) {
        result.name_ref = FileRef(reinforcement->szLocalizedNameFileRef);
        result.description_ref = FileRef(reinforcement->szLocalizedDescFileRef);
    }
    return result;
}

void EnsureChapterReinforcementInventory(MissionRuntimeState* state) {
    if (state->chapter_reinforcements.size() == NDb::_RT_NONE) {
        return;
    }

    std::vector<MissionReinforcementState> old = state->chapter_reinforcements;
    state->chapter_reinforcements.clear();
    state->chapter_reinforcements.reserve(NDb::_RT_NONE);
    for (int type = 0; type < NDb::_RT_NONE; ++type) {
        MissionReinforcementState slot;
        slot.type = type;
        slot.state = kReinforcementDisabled;
        state->chapter_reinforcements.push_back(slot);
    }
    for (std::vector<MissionReinforcementState>::const_iterator it = old.begin();
         it != old.end();
         ++it) {
        if (it->type >= 0 && it->type < state->chapter_reinforcements.size()) {
            state->chapter_reinforcements[it->type] = *it;
        }
    }
}

void RebuildChapterReinforcementCounters(MissionRuntimeState* state) {
    EnsureChapterReinforcementInventory(state);
    state->current_player_reinforcement_types.clear();
    state->chapter_reinforcement_inventory_count = state->chapter_reinforcements.size();
    state->chapter_reinforcements_enabled = 0;
    state->chapter_reinforcements_not_enabled = 0;
    state->chapter_reinforcements_disabled = 0;
    state->chapter_reinforcements_from_previous = 0;

    for (std::vector<MissionReinforcementState>::const_iterator it = state->chapter_reinforcements.begin();
         it != state->chapter_reinforcements.end();
         ++it) {
        if (it->from_previous_chapter) {
            ++state->chapter_reinforcements_from_previous;
        }
        switch (it->state) {
            case kReinforcementEnabled:
                ++state->chapter_reinforcements_enabled;
                PushUniqueInt(&state->current_player_reinforcement_types, it->type);
                break;
            case kReinforcementNotEnabled:
                ++state->chapter_reinforcements_not_enabled;
                break;
            case kReinforcementDisabled:
            default:
                ++state->chapter_reinforcements_disabled;
                break;
        }
    }
}

void SetChapterReinforcementState(
        MissionRuntimeState* state,
        const NDb::SReinforcement* reinforcement,
        int reinforcement_state,
        bool from_previous_chapter) {
    if (reinforcement == nullptr || reinforcement->eType < 0 || reinforcement->eType >= NDb::_RT_NONE) {
        return;
    }
    EnsureChapterReinforcementInventory(state);
    state->chapter_reinforcements[reinforcement->eType] =
            ReinforcementStateFromDb(reinforcement, reinforcement_state, from_previous_chapter);
}

bool IsSpecialMaxLevelReinforcementType(int type) {
    return type == NDb::RT_ENGINEERING || type == NDb::RT_RECON || type == NDb::RT_SUPER_WEAPON;
}

float ReinforcementXpForLevel(const NDb::SAIGameConsts* ai_consts, int type, int level) {
    if (ai_consts == nullptr || type < 0 || level < 0) {
        return 0.0f;
    }
    for (int i = 0; i < ai_consts->common.expLevels.size(); ++i) {
        const NDb::SAIExpLevel* levels = ai_consts->common.expLevels[i].GetPtr();
        if (levels == nullptr || levels->eDBType != type) {
            continue;
        }
        const int checked_level = std::min(level, levels->levels.size() - 1);
        if (checked_level < 0) {
            return 0.0f;
        }
        return levels->levels[checked_level].fExperience;
    }
    return 0.0f;
}

MissionLeaderState* FindLeaderForReinforcement(MissionRuntimeState* state, int type) {
    for (std::vector<MissionLeaderState>::iterator it = state->leaders.begin();
         it != state->leaders.end();
         ++it) {
        if (it->assigned && it->reinforcement_type == type) {
            return &(*it);
        }
    }
    return nullptr;
}

const MissionLeaderState* FindLeaderForReinforcement(const MissionRuntimeState* state, int type) {
    for (std::vector<MissionLeaderState>::const_iterator it = state->leaders.begin();
         it != state->leaders.end();
         ++it) {
        if (it->assigned && it->reinforcement_type == type) {
            return &(*it);
        }
    }
    return nullptr;
}

void RefreshLeaderRankInfo(const NDb::SCampaign* campaign, const NDb::SAIGameConsts* ai_consts, MissionLeaderState* leader) {
    if (leader == nullptr) {
        return;
    }
    leader->rank_name_ref.clear();
    leader->next_rank_xp = 0;
    if (campaign != nullptr && leader->rank >= 0 && leader->rank < campaign->leaderRanks.size()) {
        leader->rank_name_ref = FileRef(campaign->leaderRanks[leader->rank].szRankNameFileRef);
    }
    if (campaign != nullptr && leader->rank + 1 < campaign->leaderRanks.size()) {
        leader->next_rank_xp = static_cast<int>(
                ReinforcementXpForLevel(ai_consts, leader->reinforcement_type, leader->rank + 1));
    }
}

void RebuildReinforcementProgressCounters(const NDb::SCampaign* campaign, MissionRuntimeState* state) {
    const NDb::SAIGameConsts* ai_consts = NGameX::GetAIConsts();
    state->reinforcement_max_level = kMaxReinforcementXpLevel;
    if (state->reinforcement_progress.size() != NDb::_RT_NONE) {
        state->reinforcement_progress.clear();
        state->reinforcement_progress.reserve(NDb::_RT_NONE);
        for (int type = 0; type < NDb::_RT_NONE; ++type) {
            MissionReinforcementProgressState progress;
            progress.type = type;
            state->reinforcement_progress.push_back(progress);
        }
    }

    int favorite_count = 0;
    int favorite_type = -1;
    for (std::vector<MissionReinforcementProgressState>::iterator it = state->reinforcement_progress.begin();
         it != state->reinforcement_progress.end();
         ++it) {
        const MissionLeaderState* leader = FindLeaderForReinforcement(state, it->type);
        it->level = 0;
        it->xp = 0;
        it->next_level_xp = static_cast<int>(ReinforcementXpForLevel(ai_consts, it->type, 1));
        it->max_level = false;
        if (IsSpecialMaxLevelReinforcementType(it->type)) {
            it->level = kMaxReinforcementXpLevel;
            it->next_level_xp = 0;
            it->max_level = true;
        } else if (leader != nullptr) {
            it->level = leader->rank;
            it->xp = leader->xp;
            it->next_level_xp = leader->next_rank_xp;
            it->max_level = campaign != nullptr && leader->rank + 1 >= campaign->leaderRanks.size();
        }
        if (it->favorite_count > favorite_count) {
            favorite_count = it->favorite_count;
            favorite_type = it->type;
        }
    }
    state->reinforcement_progress_count = state->reinforcement_progress.size();
    state->favorite_reinforcement_type = favorite_type;
    state->favorite_reinforcement_count = favorite_count;
}

void RefreshLeaderCounters(const NDb::SCampaign* campaign, MissionRuntimeState* state) {
    const NDb::SAIGameConsts* ai_consts = NGameX::GetAIConsts();
    state->leader_rank_count = campaign == nullptr ? 0 : campaign->leaderRanks.size();
    state->leader_pool_count = campaign == nullptr ? 0 : campaign->leaders.size();
    state->free_leader_count = state->free_leader_indices.size();
    state->assigned_leader_count = 0;
    for (std::vector<MissionLeaderState>::iterator it = state->leaders.begin();
         it != state->leaders.end();
         ++it) {
        if (it->assigned) {
            ++state->assigned_leader_count;
            RefreshLeaderRankInfo(campaign, ai_consts, &(*it));
        }
    }
    RebuildReinforcementProgressCounters(campaign, state);
}

void InitializeLeaderProgress(const NDb::SCampaign* campaign, MissionRuntimeState* state) {
    state->leaders.clear();
    state->free_leader_indices.clear();
    state->reinforcement_progress.clear();
    state->reinforcement_progress.reserve(NDb::_RT_NONE);
    for (int type = 0; type < NDb::_RT_NONE; ++type) {
        MissionReinforcementProgressState progress;
        progress.type = type;
        progress.level = IsSpecialMaxLevelReinforcementType(type) ? kMaxReinforcementXpLevel : 0;
        progress.max_level = IsSpecialMaxLevelReinforcementType(type);
        state->reinforcement_progress.push_back(progress);
    }
    if (campaign != nullptr) {
        state->free_leader_indices.reserve(campaign->leaders.size());
        for (int i = 0; i < campaign->leaders.size(); ++i) {
            state->free_leader_indices.push_back(i);
        }
    }
    RefreshLeaderCounters(campaign, state);
}

void CommitLeaderProgress(MissionRuntimeState* state) {
    for (std::vector<MissionLeaderState>::iterator it = state->leaders.begin();
         it != state->leaders.end();
         ++it) {
        if (!it->assigned) {
            continue;
        }
        it->stored_rank = it->rank;
        it->stored_xp = it->xp;
        it->stored_xp_debt = it->xp_debt;
        it->stored_units_killed = it->units_killed;
        it->stored_units_lost = it->units_lost;
    }
}

void RestoreLeaderProgress(MissionRuntimeState* state) {
    for (std::vector<MissionLeaderState>::iterator it = state->leaders.begin();
         it != state->leaders.end();
         ++it) {
        if (!it->assigned) {
            continue;
        }
        it->rank = it->stored_rank;
        it->xp = it->stored_xp;
        it->xp_debt = it->stored_xp_debt;
        it->units_killed = it->stored_units_killed;
        it->units_lost = it->stored_units_lost;
    }
}

bool AddLeaderXpLocked(
        const NDb::SCampaign* campaign,
        const NDb::SAIGameConsts* ai_consts,
        MissionLeaderState* leader,
        float* xp) {
    if (campaign == nullptr || leader == nullptr || xp == nullptr) {
        return false;
    }

    bool leveled = false;
    while (leader->rank + 1 < campaign->leaderRanks.size() && *xp > 0.0f) {
        if (leader->xp_debt > 0) {
            const float penalty_coeff = ai_consts == nullptr ? 0.0f : ai_consts->common.fExpCommanderPenaltyCoeff;
            const float max_debt_payment = *xp * penalty_coeff;
            if (max_debt_payment >= leader->xp_debt) {
                *xp -= leader->xp_debt;
                leader->xp_debt = 0;
            } else {
                *xp -= max_debt_payment;
                leader->xp_debt = static_cast<int>(leader->xp_debt - max_debt_payment);
            }
        }

        leader->xp += static_cast<int>(*xp);
        *xp = 0.0f;
        const float required_xp = ReinforcementXpForLevel(ai_consts, leader->reinforcement_type, leader->rank + 1);
        const float extra_xp = static_cast<float>(leader->xp) - required_xp;
        if (extra_xp >= 0.0f) {
            *xp = extra_xp;
            leader->xp = static_cast<int>(required_xp);
            if (leader->rank + 1 < campaign->leaderRanks.size()) {
                ++leader->rank;
                leveled = true;
            }
        }
    }
    RefreshLeaderRankInfo(campaign, ai_consts, leader);
    return leveled;
}

void UpdateLeaderLossDebtLocked(
        const NDb::SAIGameConsts* ai_consts,
        MissionLeaderState* leader,
        int lost_unit_exp_price) {
    if (leader == nullptr || lost_unit_exp_price <= 0) {
        return;
    }
    const float penalty_coeff = ai_consts == nullptr ? 0.0f : ai_consts->common.fExpCommanderUnitPenaltyCoeff;
    ++leader->units_lost;
    leader->xp_debt += static_cast<int>(static_cast<float>(lost_unit_exp_price) * penalty_coeff);
}

void RestoreMissionState(const MissionRuntimeState& state) {
    std::lock_guard<std::mutex> lock(g_mission_mutex);
    g_state = state;
}

class ScopedMissionStateRestore {
public:
    explicit ScopedMissionStateRestore(MissionRuntimeState state) : state_(state) {}
    ~ScopedMissionStateRestore() { RestoreMissionState(state_); }

    ScopedMissionStateRestore(const ScopedMissionStateRestore&) = delete;
    ScopedMissionStateRestore& operator=(const ScopedMissionStateRestore&) = delete;

private:
    MissionRuntimeState state_;
};

int PlayerMapReinforcementCalls(const NDb::SMapInfo* map, int player) {
    if (map == nullptr || player < 0 || player >= map->players.size()) {
        return 0;
    }
    return map->players[player].nReinforcementCalls;
}

int FindMainEnemyPlayer(const NDb::SMapInfo* map) {
    if (map == nullptr) {
        return -1;
    }
    for (int i = 0; i < map->players.size(); ++i) {
        if (map->players[i].nDiplomacySide == 1) {
            return i;
        }
    }
    return -1;
}

std::vector<int> ChapterPlayerReinforcementTypes(const NDb::SChapter* chapter) {
    std::vector<int> types;
    if (chapter == nullptr || chapter->basePlayerReinforcements.empty()) {
        return types;
    }
    const NDb::SBaseReinforcements& reinforcements = chapter->basePlayerReinforcements[0];
    for (vector<CDBPtr<NDb::SReinforcement> >::const_iterator it = reinforcements.reinforcements.begin();
         it != reinforcements.reinforcements.end();
         ++it) {
        const NDb::SReinforcement* reinforcement = it->GetPtr();
        if (reinforcement != nullptr &&
            std::find(types.begin(), types.end(), reinforcement->eType) == types.end()) {
            types.push_back(reinforcement->eType);
        }
    }
    return types;
}

bool MissionReinforcementsSatisfied(
        const NDb::SMapInfo* map,
        const std::vector<int>& chapter_reinforcement_types,
        bool use_map_reinforcements) {
    if (map == nullptr || use_map_reinforcements) {
        return true;
    }
    if (map->players.empty() || map->players[0].reinforcementTypes.empty()) {
        return true;
    }
    for (vector<CDBPtr<NDb::SReinforcement> >::const_iterator it = map->players[0].reinforcementTypes.begin();
         it != map->players[0].reinforcementTypes.end();
         ++it) {
        const NDb::SReinforcement* reinforcement = it->GetPtr();
        if (reinforcement != nullptr &&
            std::find(chapter_reinforcement_types.begin(), chapter_reinforcement_types.end(), reinforcement->eType) !=
                    chapter_reinforcement_types.end()) {
            return true;
        }
    }
    return false;
}

void RefreshChapterProgress(const NDb::SChapter* chapter, MissionRuntimeState* state) {
    state->completed_mission_ids = state->won_mission_ids;
    state->completed_mission_count = state->completed_mission_ids.size();
    state->enabled_mission_ids.clear();
    state->enabled_mission_count = 0;
    state->chapter_mission_count = chapter == nullptr ? 0 : chapter->missionPath.size();
    state->missions_to_enable_count = 0;

    if (chapter == nullptr || chapter->missionPath.empty()) {
        return;
    }

    const int won_count = state->won_mission_ids.size();
    state->missions_to_enable_count = std::max(0, chapter->missionPath[0].nMissionsToEnable - won_count);
    std::vector<int> chapter_reinforcement_types = state->current_player_reinforcement_types;
    if (chapter_reinforcement_types.empty()) {
        chapter_reinforcement_types = ChapterPlayerReinforcementTypes(chapter);
    }
    for (int i = 0; i < chapter->missionPath.size(); ++i) {
        const NDb::SMapInfo* map = chapter->missionPath[i].pMap.GetPtr();
        if (map == nullptr) {
            continue;
        }
        const std::string mission_id = DbIdOf(map);
        if (ContainsString(state->won_mission_ids, mission_id)) {
            continue;
        }
        if (chapter->missionPath[i].nMissionsToEnable > won_count) {
            continue;
        }
        if (!MissionReinforcementsSatisfied(map, chapter_reinforcement_types, chapter->bUseMapReinforcements)) {
            continue;
        }
        state->enabled_mission_ids.push_back(mission_id);
    }
    state->enabled_mission_count = state->enabled_mission_ids.size();
}

void PopulateChapterReinforcementState(const NDb::SChapter* chapter, MissionRuntimeState* state) {
    EnsureChapterReinforcementInventory(state);
    for (std::vector<MissionReinforcementState>::iterator it = state->chapter_reinforcements.begin();
         it != state->chapter_reinforcements.end();
         ++it) {
        if (it->state == kReinforcementEnabled) {
            it->state = kReinforcementNotEnabled;
        }
        if (!it->dbid.empty()) {
            it->from_previous_chapter = true;
        }
    }

    if (chapter != nullptr && !chapter->basePlayerReinforcements.empty()) {
        const NDb::SBaseReinforcements& reinforcements = chapter->basePlayerReinforcements[0];
        for (vector<CDBPtr<NDb::SReinforcement> >::const_iterator it = reinforcements.reinforcements.begin();
             it != reinforcements.reinforcements.end();
             ++it) {
            SetChapterReinforcementState(state, it->GetPtr(), kReinforcementEnabled, false);
        }
    }

    if (chapter != nullptr) {
        for (int i = 0; i < chapter->missionPath.size(); ++i) {
            const NDb::SMissionEnableInfo& mission = chapter->missionPath[i];
            for (vector<CDBPtr<NDb::SChapterBonus> >::const_iterator it = mission.reward.begin();
                 it != mission.reward.end();
                 ++it) {
                const NDb::SChapterBonus* bonus = it->GetPtr();
                if (bonus == nullptr ||
                    bonus->eBonusType != NDb::CBT_REINF_CHANGE ||
                    bonus->bApplyToEnemy) {
                    continue;
                }
                const NDb::SReinforcement* reinforcement = bonus->pReinforcementSet.GetPtr();
                if (reinforcement == nullptr ||
                    reinforcement->eType < 0 ||
                    reinforcement->eType >= state->chapter_reinforcements.size()) {
                    continue;
                }
                if (state->chapter_reinforcements[reinforcement->eType].state != kReinforcementEnabled) {
                    SetChapterReinforcementState(state, reinforcement, kReinforcementNotEnabled, false);
                }
            }
        }
    }
    RebuildChapterReinforcementCounters(state);
}

bool FindCampaignMissionForProgressionProbe(const NDb::SGameRoot* game_root, MissionLocation* location) {
    if (game_root == nullptr || location == nullptr) {
        return false;
    }

    MissionLocation best;
    int best_score = -1;
    for (int campaign_index = 0; campaign_index < game_root->campaigns.size(); ++campaign_index) {
        const NDb::SCampaign* campaign = game_root->campaigns[campaign_index].GetPtr();
        if (campaign == nullptr) {
            continue;
        }
        for (int chapter_index = 0; chapter_index < campaign->chapters.size(); ++chapter_index) {
            const NDb::SChapter* chapter = campaign->chapters[chapter_index].GetPtr();
            if (chapter == nullptr) {
                continue;
            }
            for (int mission_index = 0; mission_index < chapter->missionPath.size(); ++mission_index) {
                const NDb::SMissionEnableInfo& mission = chapter->missionPath[mission_index];
                const NDb::SMapInfo* map = mission.pMap.GetPtr();
                if (map == nullptr) {
                    continue;
                }
                const bool has_objectives = !map->objectives.empty();
                const bool has_rewards = !mission.reward.empty();
                const int score = (has_rewards ? 4 : 0) + (has_objectives ? 2 : 0) + (mission_index != 0 ? 1 : 0);
                if (score > best_score) {
                    best_score = score;
                    best.campaign_index = campaign_index;
                    best.chapter_index = chapter_index;
                    best.mission_index = mission_index;
                    best.has_objectives = has_objectives;
                    best.has_rewards = has_rewards;
                }
                if (score == 7) {
                    *location = best;
                    return true;
                }
            }
        }
    }

    if (best_score < 0) {
        return false;
    }
    *location = best;
    return true;
}

int CalculateRankIndex(const NDb::SCampaign* campaign, int xp) {
    if (campaign == nullptr || campaign->rankExperiences.empty()) {
        return -1;
    }
    int chosen_rank = 0;
    for (int i = 1; i < campaign->rankExperiences.size(); ++i) {
        if (campaign->rankExperiences[i].fExperience > xp) {
            break;
        }
        chosen_rank = i;
    }
    return chosen_rank;
}

void PopulateRankInfo(const NDb::SCampaign* campaign, int rank_index, MissionRuntimeState* state) {
    state->player_rank_index = rank_index;
    state->player_rank_id.clear();
    state->player_rank_name_ref.clear();
    if (campaign == nullptr || rank_index < 0 || rank_index >= campaign->rankExperiences.size()) {
        return;
    }

    const NDb::SPlayerRank* rank = campaign->rankExperiences[rank_index].pRank.GetPtr();
    state->player_rank_id = DbIdOf(rank);
    if (rank != nullptr) {
        state->player_rank_name_ref = FileRef(rank->szRankNameFileRef);
    }
}

void RefreshCampaignExpProgress(const NDb::SCampaign* campaign, MissionRuntimeState* state) {
    const int xp = state->player_xp + state->player_xp_added;
    const int rank_index = CalculateRankIndex(campaign, xp);
    PopulateRankInfo(campaign, rank_index, state);
    state->campaign_exp_current = 0;
    state->campaign_exp_next_level = 0;
    if (campaign == nullptr || rank_index < 0 || rank_index >= campaign->rankExperiences.size()) {
        return;
    }

    const int rank_xp = static_cast<int>(campaign->rankExperiences[rank_index].fExperience);
    state->campaign_exp_current = std::max(0, xp - rank_xp);
    if (rank_index + 1 < campaign->rankExperiences.size()) {
        const int next_rank_xp = static_cast<int>(campaign->rankExperiences[rank_index + 1].fExperience);
        state->campaign_exp_next_level = std::max(0, next_rank_xp - rank_xp);
    }
}

void InitializeCampaignProgress(const NDb::SCampaign* campaign, MissionRuntimeState* state) {
    state->player_xp = 0;
    state->player_xp_added = 0;
    state->player_rank_promotions = 0;
    state->player_rank_promotions_added = 0;
    state->medal_kills_given = 0;
    state->medal_tactics_given = 0;
    state->medal_economy_given = 0;
    state->medal_munchkin_given = 0;
    state->mission_medals_awarded = 0;
    state->mission_medals.clear();
    if (campaign != nullptr && !campaign->rankExperiences.empty()) {
        state->player_rank_promotions = campaign->rankExperiences.front().nAddPromotion;
    }
    InitializeLeaderProgress(campaign, state);
    RefreshCampaignExpProgress(campaign, state);
}

int CalcScore(int units_lost, int units_killed, int lost_price, int killed_price, int reinforcements_called) {
    return ((killed_price / (lost_price + 1)) * (units_killed / (reinforcements_called + 1))) * 3;
}

void RefreshMissionScore(MissionRuntimeState* state) {
    state->mission_score = CalcScore(
            state->mission_units_lost,
            state->mission_units_killed,
            state->mission_units_lost_price,
            state->mission_units_killed_price,
            state->reinforcement_calls_used);
}

void EnsureKillMatrices(MissionRuntimeState* state) {
    const int players = std::max(0, state->player_count);
    const int size = players * players;
    if (state->kill_matrix_player_count == players &&
        state->kill_matrix.size() == size &&
        state->price_kill_matrix.size() == size) {
        return;
    }

    state->kill_matrix_player_count = players;
    state->kill_matrix.assign(size, 0);
    state->price_kill_matrix.assign(size, 0);
    state->mission_kill_events = 0;
    state->mission_price_kill_events = 0;
}

int KillMatrixIndex(const MissionRuntimeState& state, int player, int killed_player) {
    if (player < 0 || killed_player < 0 ||
        player >= state.kill_matrix_player_count ||
        killed_player >= state.kill_matrix_player_count) {
        return -1;
    }
    return player * state.kill_matrix_player_count + killed_player;
}

int PlayerSide(const MissionRuntimeState& state, int player) {
    if (player < 0 || player >= state.player_sides.size()) {
        return 2;
    }
    return state.player_sides[player];
}

void RecalculateLocalKillStatistics(MissionRuntimeState* state) {
    EnsureKillMatrices(state);
    int units_lost = 0;
    int units_killed = 0;
    int lost_price = 0;
    int killed_price = 0;

    const int local_player = 0;
    const int local_side = PlayerSide(*state, local_player);
    for (int player = 0; player < state->kill_matrix_player_count; ++player) {
        const int lost_index = KillMatrixIndex(*state, player, local_player);
        if (lost_index >= 0) {
            units_lost += state->kill_matrix[lost_index];
            lost_price += state->price_kill_matrix[lost_index];
        }

        if (player == local_player || PlayerSide(*state, player) == 2 || local_side == PlayerSide(*state, player)) {
            continue;
        }
        const int killed_index = KillMatrixIndex(*state, local_player, player);
        if (killed_index >= 0) {
            units_killed += state->kill_matrix[killed_index];
            killed_price += state->price_kill_matrix[killed_index];
        }
    }

    state->mission_units_lost = units_lost;
    state->mission_units_killed = units_killed;
    state->mission_units_lost_price = lost_price;
    state->mission_units_killed_price = killed_price;
    RefreshMissionScore(state);
}

bool SetStatisticLocked(
        MissionRuntimeState* state,
        const NDb::SCampaign* campaign,
        int statistic_kind,
        int value) {
    const int clamped_value = std::max(0, value);
    switch (statistic_kind) {
        case kMissionStatisticTime:
            state->mission_time_seconds = clamped_value;
            break;
        case kMissionStatisticCampaignTime:
            state->campaign_time_seconds = clamped_value;
            break;
        case kMissionStatisticExpEarned:
            state->player_xp_added = clamped_value;
            state->mission_exp_earned = clamped_value;
            RefreshCampaignExpProgress(campaign, state);
            break;
        case kMissionStatisticCampaignExpCurrent:
            state->campaign_exp_current = clamped_value;
            break;
        case kMissionStatisticCampaignExpNextLevel:
            state->campaign_exp_next_level = clamped_value;
            break;
        case kMissionStatisticUnitsLost:
            state->mission_units_lost = clamped_value;
            RefreshMissionScore(state);
            break;
        case kMissionStatisticUnitsKilled:
            state->mission_units_killed = clamped_value;
            RefreshMissionScore(state);
            break;
        case kMissionStatisticKeyBuildingsCaptured:
            state->mission_key_buildings_captured = clamped_value;
            break;
        case kMissionStatisticReinforcementsCalled:
            state->reinforcement_calls_used = clamped_value;
            state->mission_reinforcements_called = clamped_value;
            RefreshMissionScore(state);
            break;
        case kMissionStatisticScore:
            state->mission_score = clamped_value;
            break;
        case kMissionStatisticUnitsLostPrice:
            state->mission_units_lost_price = clamped_value;
            RefreshMissionScore(state);
            break;
        case kMissionStatisticUnitsKilledPrice:
            state->mission_units_killed_price = clamped_value;
            RefreshMissionScore(state);
            break;
        case kMissionStatisticEnemyUnitsMaxPrice:
            state->mission_enemy_units_max_price = clamped_value;
            break;
        case kMissionStatisticCampaignUnitsLost:
            state->campaign_units_lost = clamped_value;
            break;
        case kMissionStatisticCampaignUnitsKilled:
            state->campaign_units_killed = clamped_value;
            break;
        case kMissionStatisticCampaignMissionsPassed:
            state->campaign_missions_passed = clamped_value;
            break;
        case kMissionStatisticTacticalEfficiency:
            state->tactical_efficiency = clamped_value;
            break;
        case kMissionStatisticStrategicEfficiency:
            state->strategic_efficiency = clamped_value;
            break;
        default:
            return false;
    }
    return true;
}

void AddPlayerXpLocked(const NDb::SCampaign* campaign, MissionRuntimeState* state, int xp) {
    state->player_xp_added = std::max(0, state->player_xp_added + xp);
    state->mission_exp_earned = state->player_xp_added;
    state->mission_reinforcements_called = state->reinforcement_calls_used;
    RefreshCampaignExpProgress(campaign, state);
}

void RefreshWinStatisticsLocked(const NDb::SCampaign* campaign, MissionRuntimeState* state) {
    state->mission_exp_earned = state->player_xp_added;
    if (state->mission_won && !state->custom && !state->tutorial) {
        return;
    }
    if (!state->custom && !state->tutorial) {
        state->campaign_time_seconds += state->mission_time_seconds;
        ++state->campaign_missions_passed;
        state->campaign_units_killed += state->mission_units_killed;
        state->campaign_units_lost += state->mission_units_lost;
    }
    RefreshMissionScore(state);
    RefreshCampaignExpProgress(campaign, state);
}

void ResetMissionRewardSnapshot(MissionRuntimeState* state) {
    state->reward_bonus_reinforcements = 0;
    state->reward_disabled_reinforcements = 0;
    state->reward_added_calls = 0;
    state->reward_reinforcement_ids.clear();
    state->reward_reinforcement_types.clear();
    state->reward_disabled_reinforcement_types.clear();
    state->new_player_rank_id.clear();
    state->new_player_rank_name_ref.clear();
    state->player_rank_promotions_added = 0;
    state->mission_medals_awarded = 0;
    state->medal_munchkin_blocked_by_reinforcement_xp = false;
    state->old_chapter_reinforcement_inventory_count = 0;
    state->old_chapter_reinforcements.clear();
    state->mission_medals.clear();
}

void ApplyMissionBonusLocked(const NDb::SChapterBonus* bonus, MissionRuntimeState* state) {
    if (bonus == nullptr) {
        return;
    }

    switch (bonus->eBonusType) {
        case NDb::CBT_REINF_CHANGE: {
            if (bonus->bApplyToEnemy) {
                return;
            }
            const NDb::SReinforcement* reinforcement = bonus->pReinforcementSet.GetPtr();
            if (reinforcement == nullptr) {
                return;
            }
            SetChapterReinforcementState(state, reinforcement, kReinforcementEnabled, false);
            RebuildChapterReinforcementCounters(state);
            PushUniqueInt(&state->current_player_reinforcement_types, reinforcement->eType);
            PushUniqueInt(&state->reward_reinforcement_types, reinforcement->eType);
            state->reward_reinforcement_ids.push_back(DbIdOf(reinforcement));
            ++state->reward_bonus_reinforcements;
            break;
        }
        case NDb::CBT_REINF_DISABLE:
            if (bonus->bApplyToEnemy) {
                PushUniqueInt(&state->reward_disabled_reinforcement_types, bonus->eReinforcementType);
                ++state->reward_disabled_reinforcements;
            }
            break;
        case NDb::CBT_ADD_CALLS:
            if (!bonus->bApplyToEnemy) {
                const int added_calls = std::max(0, bonus->nNumberOfCalls);
                state->chapter_reinforcement_calls_left += added_calls;
                state->reward_added_calls += added_calls;
            }
            break;
    }
}

void ApplyMissionRewardsLocked(const NDb::SChapter* chapter, MissionRuntimeState* state) {
    ResetMissionRewardSnapshot(state);
    state->old_chapter_reinforcements = state->chapter_reinforcements;
    state->old_chapter_reinforcement_inventory_count = state->old_chapter_reinforcements.size();
    if (chapter == nullptr || state->mission_index < 0 || state->mission_index >= chapter->missionPath.size()) {
        return;
    }
    if (state->mission_index == 0) {
        return;
    }

    const NDb::SMissionEnableInfo& mission = chapter->missionPath[state->mission_index];
    for (vector<CDBPtr<NDb::SChapterBonus> >::const_iterator it = mission.reward.begin();
         it != mission.reward.end();
         ++it) {
        ApplyMissionBonusLocked(it->GetPtr(), state);
    }
}

void AddMissionMedalLocked(const NDb::SMedal* medal, int source, MissionRuntimeState* state) {
    if (medal == nullptr) {
        return;
    }

    MissionMedalState medal_state;
    medal_state.dbid = DbIdOf(medal);
    medal_state.name_ref = FileRef(medal->szLocalizedNameFileRef);
    medal_state.description_ref = FileRef(medal->szLocalizedDescFileRef);
    medal_state.source = source;
    state->mission_medals.push_back(medal_state);
    state->mission_medals_awarded = state->mission_medals.size();
}

void ApplyMissionMedalsLocked(const NDb::SCampaign* campaign, MissionRuntimeState* state) {
    state->mission_medals.clear();
    state->mission_medals_awarded = 0;
    state->medal_munchkin_blocked_by_reinforcement_xp = false;
    if (campaign == nullptr || state->tutorial || state->custom || state->chapter_index < 0) {
        return;
    }

    const int chapter_number = state->chapter_index + 1;
    bool medal_given = false;

    if (state->chapter_finished && campaign->medalsForChapter.size() >= chapter_number) {
        AddMissionMedalLocked(campaign->medalsForChapter[chapter_number - 1].pMedal.GetPtr(), 1, state);
        medal_given = state->mission_medals_awarded > 0;
    }

    if (!medal_given && campaign->medalsForKills.size() > state->medal_kills_given) {
        const NDb::SMedalConditions& conditions = campaign->medalsForKills[state->medal_kills_given];
        if (chapter_number >= conditions.nStartingChapter &&
            state->campaign_units_killed > conditions.fParameter) {
            AddMissionMedalLocked(conditions.pMedal.GetPtr(), 2, state);
            if (state->mission_medals_awarded > 0) {
                ++state->medal_kills_given;
                medal_given = true;
            }
        }
    }

    if (!medal_given && campaign->medalsForTactics.size() > state->medal_tactics_given) {
        const NDb::SMedalConditions& conditions = campaign->medalsForTactics[state->medal_tactics_given];
        const float units_lost = state->mission_units_lost == 0 ? 1.0f : static_cast<float>(state->mission_units_lost);
        const float ratio = static_cast<float>(state->mission_units_killed) / units_lost;
        if (chapter_number >= conditions.nStartingChapter && ratio > conditions.fParameter) {
            AddMissionMedalLocked(conditions.pMedal.GetPtr(), 3, state);
            if (state->mission_medals_awarded > 0) {
                ++state->medal_tactics_given;
                medal_given = true;
            }
        }
    }

    if (!medal_given && campaign->medalsForEconomy.size() > state->medal_economy_given) {
        const NDb::SMedalConditions& conditions = campaign->medalsForEconomy[state->medal_economy_given];
        float value = 0.0f;
        if (state->recommended_calls > 0) {
            value = static_cast<float>(
                    (state->recommended_calls - state->mission_reinforcements_called) * 100) /
                    static_cast<float>(state->recommended_calls);
        }
        if (chapter_number >= conditions.nStartingChapter && value > conditions.fParameter) {
            AddMissionMedalLocked(conditions.pMedal.GetPtr(), 4, state);
            if (state->mission_medals_awarded > 0) {
                ++state->medal_economy_given;
            }
        }
    }

    if (state->medal_munchkin_given == 0 && campaign->pMedalForMunchkinism.GetBarePtrNoLoad() != nullptr) {
        state->medal_munchkin_blocked_by_reinforcement_xp = true;
    }
}

void PopulateObjectives(const NDb::SMapInfo* map, MissionRuntimeState* state) {
    state->objectives.clear();
    state->waiting_objective_count = 0;
    state->received_objective_count = 0;
    state->completed_objective_count = 0;
    state->failed_objective_count = 0;
    state->primary_objective_count = 0;
    if (map == nullptr) {
        return;
    }
    state->objectives.reserve(map->objectives.size());
    for (vector<CDBPtr<NDb::SMissionObjective> >::const_iterator it = map->objectives.begin();
         it != map->objectives.end();
         ++it) {
        MissionObjectiveState objective;
        objective.state = EMOS_WAITING;
        ++state->waiting_objective_count;
        objective.dbid = DbIdOfPtr(*it);
        const NDb::SMissionObjective* db_objective = it->GetPtr();
        if (db_objective != nullptr) {
            objective.header_ref = FileRef(db_objective->szHeaderFileRef);
            objective.briefing_ref = FileRef(db_objective->szBriefingFileRef);
            objective.description_ref = FileRef(db_objective->szDescriptionFileRef);
            objective.map_positions = db_objective->mapPositions.size();
            objective.experience = db_objective->nExperience;
            objective.primary = db_objective->bIsPrimary;
            if (objective.primary) {
                ++state->primary_objective_count;
            }
        }
        state->objectives.push_back(objective);
    }
    state->objective_count = state->objectives.size();
}

void RecalculateObjectiveCounters(MissionRuntimeState* state) {
    state->waiting_objective_count = 0;
    state->received_objective_count = 0;
    state->completed_objective_count = 0;
    state->failed_objective_count = 0;
    state->primary_objective_count = 0;
    for (std::vector<MissionObjectiveState>::const_iterator it = state->objectives.begin();
         it != state->objectives.end();
         ++it) {
        if (it->primary) {
            ++state->primary_objective_count;
        }
        switch (it->state) {
            case EMOS_RECEIVED:
                ++state->received_objective_count;
                break;
            case EMOS_COMPLETED:
                ++state->completed_objective_count;
                break;
            case EMOS_FAILED:
                ++state->failed_objective_count;
                break;
            case EMOS_WAITING:
            default:
                ++state->waiting_objective_count;
                break;
        }
    }
    state->objective_count = state->objectives.size();
}

void PopulateMapSnapshot(const NDb::SMapInfo* map, MissionRuntimeState* state) {
    if (map == nullptr) {
        return;
    }
    state->mission_id = DbIdOf(map);
    state->map_data_ref = FileRef(map->szMapDesignerFileRef);
    state->map_script_ref = FileRef(map->szScriptFileRef);
    state->player_count = map->players.size();
    state->player_sides.clear();
    state->player_sides.reserve(map->players.size());
    for (int i = 0; i < map->players.size(); ++i) {
        state->player_sides.push_back(map->players[i].nDiplomacySide);
    }
    EnsureKillMatrices(state);
    state->script_movie_sequences = map->scriptMovies.scriptMovieSequences.size();
    state->script_camera_placements = map->scriptMovies.scriptCameraPlacements.size();
    PopulateObjectives(map, state);
}

void ClearMissionSnapshot(MissionRuntimeState* state) {
    state->mission_active = false;
    state->mission_index = -1;
    state->recommended_calls = 0;
    state->mission_enable_type = 0;
    state->mission_type = 0;
    state->player_count = 0;
    state->objective_count = 0;
    state->waiting_objective_count = 0;
    state->received_objective_count = 0;
    state->completed_objective_count = 0;
    state->failed_objective_count = 0;
    state->primary_objective_count = 0;
    state->script_movie_sequences = 0;
    state->script_camera_placements = 0;
    state->mission_reinforcement_calls_left = 0;
    state->enemy_reinforcement_calls_left = 0;
    state->reinforcement_calls_used = 0;
    state->main_enemy_player = -1;
    state->player_xp_added = 0;
    state->mission_time_seconds = 0;
    state->mission_exp_earned = 0;
    state->mission_units_lost = 0;
    state->mission_units_killed = 0;
    state->mission_units_lost_price = 0;
    state->mission_units_killed_price = 0;
    state->mission_key_buildings_captured = 0;
    state->mission_reinforcements_called = 0;
    state->mission_enemy_units_max_price = 0;
    state->mission_score = 0;
    state->mission_kill_events = 0;
    state->mission_price_kill_events = 0;
    state->kill_matrix_player_count = 0;
    state->last_kill_player = -1;
    state->last_killed_player = -1;
    state->last_kill_reinforcement_type = -1;
    state->last_killed_reinforcement_type = -1;
    state->last_kill_exp_price = 0;
    state->last_kill_leveled_up = false;
    state->use_map_reinforcements = false;
    state->only_recommended_reinforcement_calls = false;
    state->mission_won = false;
    state->mission_cancelled = false;
    state->started_from_existing_campaign_state = false;
    state->mission_id.clear();
    state->map_data_ref.clear();
    state->map_script_ref.clear();
    state->intro_movie_ref.clear();
    state->player_sides.clear();
    state->kill_matrix.clear();
    state->price_kill_matrix.clear();
    state->objectives.clear();
    ResetMissionRewardSnapshot(state);
}

MissionRuntimeResult StoreState(MissionRuntimeState state) {
    MissionRuntimeResult result;
    result.ok = true;
    state.active = true;
    {
        std::lock_guard<std::mutex> lock(g_mission_mutex);
        g_state = state;
        result.state = g_state;
    }
    return result;
}

MissionRuntimeResult ErrorResult(const char* error) {
    MissionRuntimeResult result;
    result.error = error == nullptr ? "" : error;
    return result;
}

MissionRuntimeResult ErrorResult(const std::string& error) {
    MissionRuntimeResult result;
    result.error = error;
    return result;
}

const NDb::SCampaign* CampaignByIndex(const NDb::SGameRoot* game_root, int campaign_index) {
    if (game_root == nullptr || campaign_index < 0 || campaign_index >= game_root->campaigns.size()) {
        return nullptr;
    }
    return game_root->campaigns[campaign_index].GetPtr();
}

const NDb::SChapter* ChapterByIndex(const NDb::SCampaign* campaign, int chapter_index) {
    if (campaign == nullptr || chapter_index < 0 || chapter_index >= campaign->chapters.size()) {
        return nullptr;
    }
    return campaign->chapters[chapter_index].GetPtr();
}

MissionRuntimeResult StartMissionInCurrentCampaignStateLocked(
        const NDb::SCampaign* campaign,
        const NDb::SChapter* chapter,
        int mission_index,
        int difficulty,
        bool continued) {
    if (campaign == nullptr) {
        return ErrorResult("campaign_missing");
    }
    if (chapter == nullptr) {
        return ErrorResult("chapter_missing");
    }
    if (mission_index < 0 || mission_index >= chapter->missionPath.size()) {
        return ErrorResult("mission_index_out_of_range");
    }

    const NDb::SMissionEnableInfo& mission = chapter->missionPath[mission_index];
    const NDb::SMapInfo* map = mission.pMap.GetPtr();
    if (map == nullptr) {
        return ErrorResult("mission_map_missing");
    }

    ClearMissionSnapshot(&g_state);
    g_state.active = true;
    g_state.mission_active = true;
    g_state.campaign_active = true;
    g_state.chapter_active = true;
    g_state.campaign_finished = false;
    g_state.chapter_finished = false;
    g_state.custom = false;
    g_state.tutorial = false;
    g_state.mission_won = false;
    g_state.mission_cancelled = false;
    g_state.mission_index = mission_index;
    g_state.difficulty = difficulty;
    g_state.campaign_chapter_count = campaign->chapters.size();
    g_state.chapter_mission_count = chapter->missionPath.size();
    g_state.recommended_calls = mission.nRecommendedCalls;
    g_state.mission_enable_type = mission.eMissionEnableType;
    g_state.mission_type = mission.eType;
    g_state.use_map_reinforcements = chapter->bUseMapReinforcements;
    g_state.started_from_existing_campaign_state = continued;
    if (continued) {
        ++g_state.continued_mission_starts;
    }
    g_state.intro_movie_ref = chapter->szIntroMovie.empty()
            ? FileRef(campaign->szIntroMovie)
            : FileRef(chapter->szIntroMovie);
    g_state.main_enemy_player = FindMainEnemyPlayer(map);
    g_state.enemy_reinforcement_calls_left =
            g_state.main_enemy_player >= 0 ? PlayerMapReinforcementCalls(map, g_state.main_enemy_player) : 0;
    g_state.mission_reinforcement_calls_left = g_state.use_map_reinforcements
            ? PlayerMapReinforcementCalls(map, 0)
            : std::max(0, std::min(g_state.chapter_reinforcement_calls_left, g_state.recommended_calls));
    g_state.only_recommended_reinforcement_calls =
            !chapter->missionPath.empty() && chapter->missionPath.front().pMap.GetPtr() != map;
    PopulateMapSnapshot(map, &g_state);
    RefreshChapterProgress(chapter, &g_state);
    RefreshCampaignExpProgress(campaign, &g_state);
    RefreshLeaderCounters(campaign, &g_state);

    MissionRuntimeResult result;
    result.ok = true;
    result.state = g_state;
    return result;
}

}  // namespace

MissionRuntimeResult StartCampaignMissionState(
        int campaign_index,
        int chapter_index,
        int mission_index,
        int difficulty) {
    MissionRuntimeResult result;
    if (!EnsureDatabaseReady(&result)) {
        return result;
    }

    const NDb::SGameRoot* game_root = NGameX::GetGameRoot();
    if (game_root == nullptr) {
        return ErrorResult("game_root_missing");
    }
    if (campaign_index < 0 || campaign_index >= game_root->campaigns.size()) {
        return ErrorResult("campaign_index_out_of_range");
    }

    const NDb::SCampaign* campaign = game_root->campaigns[campaign_index].GetPtr();
    if (campaign == nullptr) {
        return ErrorResult("campaign_missing");
    }
    if (chapter_index < 0 || chapter_index >= campaign->chapters.size()) {
        return ErrorResult("chapter_index_out_of_range");
    }

    const NDb::SChapter* chapter = campaign->chapters[chapter_index].GetPtr();
    if (chapter == nullptr) {
        return ErrorResult("chapter_missing");
    }
    if (mission_index < 0 || mission_index >= chapter->missionPath.size()) {
        return ErrorResult("mission_index_out_of_range");
    }

    const NDb::SMissionEnableInfo& mission = chapter->missionPath[mission_index];
    const NDb::SMapInfo* map = mission.pMap.GetPtr();
    if (map == nullptr) {
        return ErrorResult("mission_map_missing");
    }

    MissionRuntimeState state;
    state.active = true;
    state.mission_active = true;
    state.campaign_active = true;
    state.chapter_active = true;
    state.custom = false;
    state.tutorial = false;
    state.campaign_index = campaign_index;
    state.chapter_index = chapter_index;
    state.mission_index = mission_index;
    state.difficulty = difficulty;
    state.campaign_chapter_count = campaign->chapters.size();
    state.chapter_mission_count = chapter->missionPath.size();
    state.recommended_calls = mission.nRecommendedCalls;
    state.mission_enable_type = mission.eMissionEnableType;
    state.mission_type = mission.eType;
    state.chapter_reinforcement_calls_left = chapter->nReinforcementCalls;
    state.chapter_reinforcement_calls_old = 0;
    state.use_map_reinforcements = chapter->bUseMapReinforcements;
    state.campaign_id = DbIdOf(campaign);
    state.chapter_id = DbIdOf(chapter);
    state.campaign_script_ref = FileRef(campaign->szScriptFileRef);
    state.chapter_script_ref = FileRef(chapter->szScriptFileRef);
    state.intro_movie_ref = chapter->szIntroMovie.empty()
            ? FileRef(campaign->szIntroMovie)
            : FileRef(chapter->szIntroMovie);
    state.main_enemy_player = FindMainEnemyPlayer(map);
    state.enemy_reinforcement_calls_left =
            state.main_enemy_player >= 0 ? PlayerMapReinforcementCalls(map, state.main_enemy_player) : 0;
    state.mission_reinforcement_calls_left = state.use_map_reinforcements
            ? PlayerMapReinforcementCalls(map, 0)
            : std::max(0, std::min(state.chapter_reinforcement_calls_left, state.recommended_calls));
    state.only_recommended_reinforcement_calls =
            !chapter->missionPath.empty() && chapter->missionPath.front().pMap.GetPtr() != map;
    PopulateChapterReinforcementState(chapter, &state);
    InitializeCampaignProgress(campaign, &state);
    ResetMissionRewardSnapshot(&state);
    PopulateMapSnapshot(map, &state);
    RefreshChapterProgress(chapter, &state);
    return StoreState(state);
}

MissionRuntimeResult StartDirectMissionState(const std::string& mission_id, int difficulty) {
    MissionRuntimeResult result;
    if (!EnsureDatabaseReady(&result)) {
        return result;
    }
    if (mission_id.empty()) {
        return ErrorResult("mission_id_empty");
    }

    const NDb::SMapInfo* map =
            NDb::Get<NDb::SMapInfo>(CDBID(mission_id.c_str()));
    if (map == nullptr) {
        return ErrorResult("mission_map_missing");
    }

    const NDb::SGameRoot* game_root = NGameX::GetGameRoot();
    if (game_root != nullptr) {
        for (int campaign_index = 0;
             campaign_index < game_root->campaigns.size();
             ++campaign_index) {
            const NDb::SCampaign* campaign =
                    game_root->campaigns[campaign_index].GetPtr();
            if (campaign == nullptr) {
                continue;
            }
            for (int chapter_index = 0;
                 chapter_index < campaign->chapters.size();
                 ++chapter_index) {
                const NDb::SChapter* chapter =
                        campaign->chapters[chapter_index].GetPtr();
                if (chapter == nullptr) {
                    continue;
                }
                for (int mission_index = 0;
                     mission_index < chapter->missionPath.size();
                     ++mission_index) {
                    if (chapter->missionPath[mission_index].pMap.GetPtr() == map) {
                        return StartCampaignMissionState(
                                campaign_index,
                                chapter_index,
                                mission_index,
                                difficulty);
                    }
                }
            }
        }
        for (int tutorial_index = 0;
             tutorial_index < game_root->tutorialMaps.size();
             ++tutorial_index) {
            if (game_root->tutorialMaps[tutorial_index].pMapInfo.GetPtr() == map) {
                return StartTutorialMissionState(
                        tutorial_index,
                        difficulty);
            }
        }
    }

    MissionRuntimeState state;
    state.active = true;
    state.mission_active = true;
    state.campaign_active = false;
    state.chapter_active = false;
    state.custom = true;
    state.tutorial = false;
    state.campaign_index = -1;
    state.chapter_index = -1;
    state.mission_index = -1;
    state.difficulty = difficulty;
    state.use_map_reinforcements = true;
    state.mission_reinforcement_calls_left = PlayerMapReinforcementCalls(map, 0);
    state.main_enemy_player = FindMainEnemyPlayer(map);
    state.enemy_reinforcement_calls_left =
            state.main_enemy_player >= 0 ? PlayerMapReinforcementCalls(map, state.main_enemy_player) : 0;
    ResetMissionRewardSnapshot(&state);
    PopulateMapSnapshot(map, &state);
    return StoreState(state);
}

MissionRuntimeResult StartTutorialMissionState(int tutorial_index, int difficulty) {
    MissionRuntimeResult result;
    if (!EnsureDatabaseReady(&result)) {
        return result;
    }

    const NDb::SGameRoot* game_root = NGameX::GetGameRoot();
    if (game_root == nullptr) {
        return ErrorResult("game_root_missing");
    }
    if (tutorial_index < 0 || tutorial_index >= game_root->tutorialMaps.size()) {
        return ErrorResult("tutorial_index_out_of_range");
    }

    const NDb::SGameRoot::STutorialMap& tutorial = game_root->tutorialMaps[tutorial_index];
    const NDb::SMapInfo* map = tutorial.pMapInfo.GetPtr();
    if (map == nullptr) {
        return ErrorResult("tutorial_map_missing");
    }

    MissionRuntimeState state;
    state.active = true;
    state.mission_active = true;
    state.custom = false;
    state.tutorial = true;
    state.campaign_index = -1;
    state.chapter_index = -1;
    state.mission_index = tutorial_index;
    state.difficulty = difficulty;
    state.use_map_reinforcements = true;
    state.mission_reinforcement_calls_left = PlayerMapReinforcementCalls(map, 0);
    state.main_enemy_player = FindMainEnemyPlayer(map);
    state.enemy_reinforcement_calls_left =
            state.main_enemy_player >= 0 ? PlayerMapReinforcementCalls(map, state.main_enemy_player) : 0;
    state.chapter_script_ref = FileRef(tutorial.szDifficultyFileRef);
    ResetMissionRewardSnapshot(&state);
    PopulateMapSnapshot(map, &state);
    return StoreState(state);
}

MissionRuntimeResult StartFirstCampaignMissionState(
        int campaign_index,
        int difficulty) {
    MissionRuntimeResult result;
    if (!EnsureDatabaseReady(&result)) {
        return result;
    }

    const NDb::SGameRoot* game_root = NGameX::GetGameRoot();
    if (game_root == nullptr) {
        return ErrorResult("game_root_missing");
    }
    if (campaign_index < 0 ||
        campaign_index >= game_root->campaigns.size()) {
        return ErrorResult("campaign_index_out_of_range");
    }

    const NDb::SCampaign* campaign =
            game_root->campaigns[campaign_index].GetPtr();
    if (campaign == nullptr) {
        return ErrorResult("campaign_missing");
    }
    for (int chapter_index = 0;
         chapter_index < campaign->chapters.size();
         ++chapter_index) {
        const NDb::SChapter* chapter =
                campaign->chapters[chapter_index].GetPtr();
        if (chapter == nullptr) {
            continue;
        }
        const std::vector<int> reinforcement_types =
                ChapterPlayerReinforcementTypes(chapter);
        int first_enabled_mission = -1;
        int first_enabled_order = std::numeric_limits<int>::max();
        for (int mission_index = 0;
             mission_index < chapter->missionPath.size();
             ++mission_index) {
            const NDb::SMissionEnableInfo& mission =
                    chapter->missionPath[mission_index];
            const NDb::SMapInfo* map = mission.pMap.GetPtr();
            if (map == nullptr ||
                mission.nMissionsToEnable > 0 ||
                !MissionReinforcementsSatisfied(
                        map,
                        reinforcement_types,
                        chapter->bUseMapReinforcements)) {
                continue;
            }
            if (first_enabled_mission < 0 ||
                mission.nRecommendedOrder < first_enabled_order) {
                first_enabled_mission = mission_index;
                first_enabled_order = mission.nRecommendedOrder;
            }
        }
        if (first_enabled_mission >= 0) {
            return StartCampaignMissionState(
                    campaign_index,
                    chapter_index,
                    first_enabled_mission,
                    difficulty);
        }
    }
    return ErrorResult("no_enabled_campaign_missions");
}

MissionRuntimeResult StartFirstCampaignMissionState() {
    MissionRuntimeResult result;
    if (!EnsureDatabaseReady(&result)) {
        return result;
    }

    const NDb::SGameRoot* game_root = NGameX::GetGameRoot();
    if (game_root == nullptr) {
        return ErrorResult("game_root_missing");
    }
    for (int campaign_index = 0;
         campaign_index < game_root->campaigns.size();
         ++campaign_index) {
        result = StartFirstCampaignMissionState(campaign_index, 0);
        if (result.ok) {
            return result;
        }
    }
    return ErrorResult("no_enabled_campaign_missions");
}

MissionRuntimeResult StartCurrentCampaignMissionState(int mission_index, int difficulty) {
    MissionRuntimeResult result;
    if (!EnsureDatabaseReady(&result)) {
        return result;
    }
    const NDb::SGameRoot* game_root = NGameX::GetGameRoot();

    std::lock_guard<std::mutex> lock(g_mission_mutex);
    if (!g_state.active || !g_state.campaign_active || !g_state.chapter_active) {
        return ErrorResult("campaign_state_inactive");
    }
    if (g_state.mission_active) {
        return ErrorResult("mission_already_active");
    }
    const NDb::SCampaign* campaign = CampaignByIndex(game_root, g_state.campaign_index);
    const NDb::SChapter* chapter = ChapterByIndex(campaign, g_state.chapter_index);
    return StartMissionInCurrentCampaignStateLocked(campaign, chapter, mission_index, difficulty, true);
}

MissionRuntimeResult StartFirstEnabledCampaignMissionState(int difficulty) {
    MissionRuntimeResult result;
    if (!EnsureDatabaseReady(&result)) {
        return result;
    }
    const NDb::SGameRoot* game_root = NGameX::GetGameRoot();

    std::lock_guard<std::mutex> lock(g_mission_mutex);
    if (!g_state.active || !g_state.campaign_active || !g_state.chapter_active) {
        return ErrorResult("campaign_state_inactive");
    }
    if (g_state.mission_active) {
        return ErrorResult("mission_already_active");
    }

    const NDb::SCampaign* campaign = CampaignByIndex(game_root, g_state.campaign_index);
    const NDb::SChapter* chapter = ChapterByIndex(campaign, g_state.chapter_index);
    if (chapter == nullptr) {
        return ErrorResult("chapter_missing");
    }
    RefreshChapterProgress(chapter, &g_state);
    if (g_state.enabled_mission_ids.empty()) {
        return ErrorResult("no_enabled_missions");
    }

    for (int mission_index = 0; mission_index < chapter->missionPath.size(); ++mission_index) {
        const NDb::SMapInfo* map = chapter->missionPath[mission_index].pMap.GetPtr();
        if (map == nullptr) {
            continue;
        }
        if (ContainsString(g_state.enabled_mission_ids, DbIdOf(map))) {
            return StartMissionInCurrentCampaignStateLocked(campaign, chapter, mission_index, difficulty, true);
        }
    }
    return ErrorResult("enabled_mission_map_missing");
}

MissionRuntimeResult SetMissionObjectiveState(int objective_index, int state) {
    MissionRuntimeResult result;
    if (state < EMOS_MIN || state > EMOS_MAX) {
        return ErrorResult("objective_state_out_of_range");
    }
    if (!EnsureDatabaseReady(&result)) {
        return result;
    }
    const NDb::SGameRoot* game_root = NGameX::GetGameRoot();

    std::lock_guard<std::mutex> lock(g_mission_mutex);
    if (!g_state.active || !g_state.mission_active) {
        return ErrorResult("mission_state_inactive");
    }
    if (objective_index < 0 || objective_index >= g_state.objectives.size()) {
        return ErrorResult("objective_index_out_of_range");
    }
    const NDb::SCampaign* campaign = CampaignByIndex(game_root, g_state.campaign_index);
    const int old_state = g_state.objectives[objective_index].state;
    const int objective_xp = g_state.objectives[objective_index].experience;
    g_state.objectives[objective_index].state = state;
    if (old_state != EMOS_COMPLETED && state == EMOS_COMPLETED) {
        AddPlayerXpLocked(campaign, &g_state, objective_xp);
    } else if (old_state == EMOS_COMPLETED && state != EMOS_COMPLETED) {
        AddPlayerXpLocked(campaign, &g_state, -objective_xp);
    }
    RecalculateObjectiveCounters(&g_state);
    result.ok = true;
    result.state = g_state;
    return result;
}

MissionRuntimeResult AddPlayerXp(int xp) {
    MissionRuntimeResult result;
    if (!EnsureDatabaseReady(&result)) {
        return result;
    }
    const NDb::SGameRoot* game_root = NGameX::GetGameRoot();

    std::lock_guard<std::mutex> lock(g_mission_mutex);
    if (!g_state.active || !g_state.mission_active) {
        return ErrorResult("mission_state_inactive");
    }
    const NDb::SCampaign* campaign = CampaignByIndex(game_root, g_state.campaign_index);
    AddPlayerXpLocked(campaign, &g_state, xp);
    result.ok = true;
    result.state = g_state;
    return result;
}

MissionRuntimeResult SetMissionStatistic(int statistic_kind, int value) {
    MissionRuntimeResult result;
    if (!EnsureDatabaseReady(&result)) {
        return result;
    }
    const NDb::SGameRoot* game_root = NGameX::GetGameRoot();

    std::lock_guard<std::mutex> lock(g_mission_mutex);
    if (!g_state.active) {
        return ErrorResult("mission_state_inactive");
    }
    const NDb::SCampaign* campaign = CampaignByIndex(game_root, g_state.campaign_index);
    if (!SetStatisticLocked(&g_state, campaign, statistic_kind, value)) {
        return ErrorResult("mission_statistic_out_of_range");
    }
    result.ok = true;
    result.state = g_state;
    return result;
}

MissionRuntimeResult AddMissionStatistic(int statistic_kind, int delta) {
    MissionRuntimeState snapshot = GetMissionRuntimeState();
    int current_value = 0;
    switch (statistic_kind) {
        case kMissionStatisticTime:
            current_value = snapshot.mission_time_seconds;
            break;
        case kMissionStatisticCampaignTime:
            current_value = snapshot.campaign_time_seconds;
            break;
        case kMissionStatisticExpEarned:
            current_value = snapshot.player_xp_added;
            break;
        case kMissionStatisticCampaignExpCurrent:
            current_value = snapshot.campaign_exp_current;
            break;
        case kMissionStatisticCampaignExpNextLevel:
            current_value = snapshot.campaign_exp_next_level;
            break;
        case kMissionStatisticUnitsLost:
            current_value = snapshot.mission_units_lost;
            break;
        case kMissionStatisticUnitsKilled:
            current_value = snapshot.mission_units_killed;
            break;
        case kMissionStatisticKeyBuildingsCaptured:
            current_value = snapshot.mission_key_buildings_captured;
            break;
        case kMissionStatisticReinforcementsCalled:
            current_value = snapshot.reinforcement_calls_used;
            break;
        case kMissionStatisticScore:
            current_value = snapshot.mission_score;
            break;
        case kMissionStatisticUnitsLostPrice:
            current_value = snapshot.mission_units_lost_price;
            break;
        case kMissionStatisticUnitsKilledPrice:
            current_value = snapshot.mission_units_killed_price;
            break;
        case kMissionStatisticEnemyUnitsMaxPrice:
            current_value = snapshot.mission_enemy_units_max_price;
            break;
        case kMissionStatisticCampaignUnitsLost:
            current_value = snapshot.campaign_units_lost;
            break;
        case kMissionStatisticCampaignUnitsKilled:
            current_value = snapshot.campaign_units_killed;
            break;
        case kMissionStatisticCampaignMissionsPassed:
            current_value = snapshot.campaign_missions_passed;
            break;
        case kMissionStatisticTacticalEfficiency:
            current_value = snapshot.tactical_efficiency;
            break;
        case kMissionStatisticStrategicEfficiency:
            current_value = snapshot.strategic_efficiency;
            break;
        default:
            return ErrorResult("mission_statistic_out_of_range");
    }
    return SetMissionStatistic(statistic_kind, current_value + delta);
}

MissionRuntimeResult AssignLeaderToReinforcement(int reinforcement_type, int free_leader_slot) {
    MissionRuntimeResult result;
    if (!EnsureDatabaseReady(&result)) {
        return result;
    }
    const NDb::SGameRoot* game_root = NGameX::GetGameRoot();

    std::lock_guard<std::mutex> lock(g_mission_mutex);
    if (!g_state.active || !g_state.campaign_active) {
        return ErrorResult("campaign_state_inactive");
    }
    if (reinforcement_type < 0 || reinforcement_type >= NDb::_RT_NONE) {
        return ErrorResult("reinforcement_type_out_of_range");
    }
    if (FindLeaderForReinforcement(&g_state, reinforcement_type) != nullptr) {
        return ErrorResult("reinforcement_leader_already_assigned");
    }
    if (g_state.player_rank_promotions <= 0) {
        return ErrorResult("no_available_promotions");
    }
    const NDb::SCampaign* campaign = CampaignByIndex(game_root, g_state.campaign_index);
    if (campaign == nullptr) {
        return ErrorResult("campaign_missing");
    }
    if (g_state.free_leader_indices.empty()) {
        return ErrorResult("no_free_leaders");
    }

    const int free_slot = free_leader_slot < 0 ? 0 : free_leader_slot;
    if (free_slot < 0 || free_slot >= g_state.free_leader_indices.size()) {
        return ErrorResult("free_leader_slot_out_of_range");
    }
    const int leader_index = g_state.free_leader_indices[free_slot];
    if (leader_index < 0 || leader_index >= campaign->leaders.size()) {
        return ErrorResult("leader_index_out_of_range");
    }

    const NDb::SCampaign::SLeader& db_leader = campaign->leaders[leader_index];
    MissionLeaderState leader;
    leader.reinforcement_type = reinforcement_type;
    leader.leader_index = leader_index;
    leader.assigned = true;
    leader.rank = 0;
    leader.xp = 0;
    leader.xp_debt = 0;
    leader.units_killed = 0;
    leader.units_lost = 0;
    leader.stored_rank = 0;
    leader.stored_xp = 0;
    leader.stored_xp_debt = 0;
    leader.stored_units_killed = 0;
    leader.stored_units_lost = 0;
    leader.name_ref = FileRef(db_leader.szNameFileRef);
    RefreshLeaderRankInfo(campaign, NGameX::GetAIConsts(), &leader);

    g_state.leaders.push_back(leader);
    g_state.free_leader_indices.erase(g_state.free_leader_indices.begin() + free_slot);
    --g_state.player_rank_promotions;
    RefreshLeaderCounters(campaign, &g_state);
    result.ok = true;
    result.state = g_state;
    return result;
}

MissionRuntimeResult GiveReinforcementXp(int reinforcement_type, int xp) {
    MissionRuntimeResult result;
    if (!EnsureDatabaseReady(&result)) {
        return result;
    }
    const NDb::SGameRoot* game_root = NGameX::GetGameRoot();

    std::lock_guard<std::mutex> lock(g_mission_mutex);
    if (!g_state.active || !g_state.campaign_active) {
        return ErrorResult("campaign_state_inactive");
    }
    if (reinforcement_type < 0 || reinforcement_type >= NDb::_RT_NONE) {
        return ErrorResult("reinforcement_type_out_of_range");
    }
    if (xp <= 0) {
        result.ok = true;
        result.state = g_state;
        return result;
    }

    const NDb::SCampaign* campaign = CampaignByIndex(game_root, g_state.campaign_index);
    const NDb::SAIGameConsts* ai_consts = NGameX::GetAIConsts();
    MissionLeaderState* leader = FindLeaderForReinforcement(&g_state, reinforcement_type);
    if (leader != nullptr) {
        float rest_xp = static_cast<float>(xp);
        AddLeaderXpLocked(campaign, ai_consts, leader, &rest_xp);
        ++leader->units_killed;
        const float distribution_coeff = ai_consts == nullptr ? 0.0f : ai_consts->common.fExpCommanderDistributionCoeff;
        float shared_xp = rest_xp * distribution_coeff;
        while (shared_xp > 0.0f && campaign != nullptr) {
            int available_leaders = 0;
            for (std::vector<MissionLeaderState>::const_iterator it = g_state.leaders.begin();
                 it != g_state.leaders.end();
                 ++it) {
                if (it->assigned && it->rank + 1 < campaign->leaderRanks.size()) {
                    ++available_leaders;
                }
            }
            if (available_leaders == 0) {
                break;
            }
            const float part = shared_xp / static_cast<float>(available_leaders);
            shared_xp = 0.0f;
            for (std::vector<MissionLeaderState>::iterator it = g_state.leaders.begin();
                 it != g_state.leaders.end();
                 ++it) {
                if (!it->assigned || it->rank + 1 >= campaign->leaderRanks.size()) {
                    continue;
                }
                float leader_part = part;
                AddLeaderXpLocked(campaign, ai_consts, &(*it), &leader_part);
                shared_xp += leader_part;
            }
        }
    }
    RefreshLeaderCounters(campaign, &g_state);
    result.ok = true;
    result.state = g_state;
    return result;
}

MissionRuntimeResult MarkFavoriteReinforcement(int reinforcement_type) {
    MissionRuntimeResult result;
    std::lock_guard<std::mutex> lock(g_mission_mutex);
    if (!g_state.active || !g_state.campaign_active) {
        return ErrorResult("campaign_state_inactive");
    }
    if (reinforcement_type < 0 || reinforcement_type >= NDb::_RT_NONE) {
        return ErrorResult("reinforcement_type_out_of_range");
    }
    if (g_state.reinforcement_progress.size() != NDb::_RT_NONE) {
        RebuildReinforcementProgressCounters(nullptr, &g_state);
    }
    ++g_state.reinforcement_progress[reinforcement_type].favorite_count;
    const NDb::SGameRoot* game_root = NGameX::GetGameRoot();
    const NDb::SCampaign* campaign = CampaignByIndex(game_root, g_state.campaign_index);
    RefreshLeaderCounters(campaign, &g_state);
    result.ok = true;
    result.state = g_state;
    return result;
}

MissionRuntimeResult RegisterUnitKill(
        int player,
        int,
        int reinforcement_type,
        int killed_player,
        int,
        int killed_reinforcement_type,
        int exp_price,
        bool) {
    MissionRuntimeResult result;
    if (!EnsureDatabaseReady(&result)) {
        return result;
    }
    const NDb::SGameRoot* game_root = NGameX::GetGameRoot();

    std::lock_guard<std::mutex> lock(g_mission_mutex);
    if (!g_state.active || !g_state.mission_active) {
        return ErrorResult("mission_state_inactive");
    }
    EnsureKillMatrices(&g_state);
    if (player < 0 || player >= g_state.kill_matrix_player_count ||
        killed_player < 0 || killed_player >= g_state.kill_matrix_player_count) {
        return ErrorResult("kill_player_out_of_range");
    }

    const NDb::SCampaign* campaign = CampaignByIndex(game_root, g_state.campaign_index);
    const NDb::SAIGameConsts* ai_consts = NGameX::GetAIConsts();
    const int clamped_exp_price = std::max(0, exp_price);
    const int index = KillMatrixIndex(g_state, player, killed_player);
    if (index >= 0) {
        ++g_state.kill_matrix[index];
        g_state.price_kill_matrix[index] += clamped_exp_price;
        ++g_state.mission_kill_events;
        if (clamped_exp_price > 0) {
            ++g_state.mission_price_kill_events;
        }
    }

    g_state.last_kill_player = player;
    g_state.last_killed_player = killed_player;
    g_state.last_kill_reinforcement_type = reinforcement_type;
    g_state.last_killed_reinforcement_type = killed_reinforcement_type;
    g_state.last_kill_exp_price = clamped_exp_price;
    g_state.last_kill_leveled_up = false;

    if (g_state.campaign_active && killed_player == 0) {
        MissionLeaderState* killed_leader = FindLeaderForReinforcement(&g_state, killed_reinforcement_type);
        UpdateLeaderLossDebtLocked(ai_consts, killed_leader, clamped_exp_price);
    }

    if (reinforcement_type != NDb::_RT_NONE && player == 0) {
        AddPlayerXpLocked(campaign, &g_state, clamped_exp_price);
        MissionLeaderState* leader = FindLeaderForReinforcement(&g_state, reinforcement_type);
        if (leader != nullptr) {
            float rest_xp = static_cast<float>(clamped_exp_price);
            const bool leveled = AddLeaderXpLocked(campaign, ai_consts, leader, &rest_xp);
            g_state.last_kill_leveled_up = g_state.last_kill_leveled_up || leveled;
            ++leader->units_killed;

            const float distribution_coeff = ai_consts == nullptr ? 0.0f : ai_consts->common.fExpCommanderDistributionCoeff;
            float shared_xp = rest_xp * distribution_coeff;
            while (shared_xp > 0.0f && campaign != nullptr) {
                int available_leaders = 0;
                for (std::vector<MissionLeaderState>::const_iterator it = g_state.leaders.begin();
                     it != g_state.leaders.end();
                     ++it) {
                    if (it->assigned && it->rank + 1 < campaign->leaderRanks.size()) {
                        ++available_leaders;
                    }
                }
                if (available_leaders == 0) {
                    break;
                }

                const float xp_part = shared_xp / static_cast<float>(available_leaders);
                shared_xp = 0.0f;
                for (std::vector<MissionLeaderState>::iterator it = g_state.leaders.begin();
                     it != g_state.leaders.end();
                     ++it) {
                    if (!it->assigned || it->rank + 1 >= campaign->leaderRanks.size()) {
                        continue;
                    }
                    float leader_xp = xp_part;
                    const bool shared_leveled = AddLeaderXpLocked(campaign, ai_consts, &(*it), &leader_xp);
                    g_state.last_kill_leveled_up = g_state.last_kill_leveled_up || shared_leveled;
                    shared_xp += leader_xp;
                }
            }
        }
    }

    RecalculateLocalKillStatistics(&g_state);
    RefreshLeaderCounters(campaign, &g_state);
    result.ok = true;
    result.state = g_state;
    return result;
}

MissionRuntimeResult MarkMissionWon() {
    MissionRuntimeResult result;
    if (!EnsureDatabaseReady(&result)) {
        return result;
    }
    const NDb::SGameRoot* game_root = NGameX::GetGameRoot();

    std::lock_guard<std::mutex> lock(g_mission_mutex);
    if (!g_state.active || !g_state.mission_active) {
        return ErrorResult("mission_state_inactive");
    }
    const NDb::SCampaign* campaign = CampaignByIndex(game_root, g_state.campaign_index);
    const NDb::SChapter* chapter = ChapterByIndex(campaign, g_state.chapter_index);

    RefreshWinStatisticsLocked(campaign, &g_state);
    if (!g_state.mission_id.empty() && !ContainsString(g_state.won_mission_ids, g_state.mission_id)) {
        g_state.won_mission_ids.push_back(g_state.mission_id);
    }
    ApplyMissionRewardsLocked(chapter, &g_state);
    g_state.chapter_reinforcement_calls_old = g_state.chapter_reinforcement_calls_left;
    const int chapter_calls_used = std::max(
            0,
            g_state.recommended_calls - g_state.mission_reinforcement_calls_left);
    g_state.chapter_reinforcement_calls_left =
            std::max(0, g_state.chapter_reinforcement_calls_left - chapter_calls_used);

    const int old_rank_index = CalculateRankIndex(campaign, g_state.player_xp);
    g_state.player_xp += g_state.player_xp_added;
    g_state.player_xp_added = 0;
    const int new_rank_index = CalculateRankIndex(campaign, g_state.player_xp);
    if (old_rank_index != new_rank_index) {
        g_state.new_player_rank_id = g_state.player_rank_id;
        g_state.new_player_rank_name_ref = g_state.player_rank_name_ref;
    }
    if (campaign != nullptr) {
        for (int i = old_rank_index + 1; i <= new_rank_index && i < campaign->rankExperiences.size(); ++i) {
            if (i >= 0) {
                const int promotions = campaign->rankExperiences[i].nAddPromotion;
                g_state.player_rank_promotions += promotions;
                g_state.player_rank_promotions_added += promotions;
            }
        }
    }
    RefreshCampaignExpProgress(campaign, &g_state);
    if (old_rank_index != new_rank_index) {
        g_state.new_player_rank_id = g_state.player_rank_id;
        g_state.new_player_rank_name_ref = g_state.player_rank_name_ref;
    }

    g_state.mission_reinforcement_calls_left = 0;
    g_state.enemy_reinforcement_calls_left = 0;
    g_state.reinforcement_calls_used = 0;
    g_state.main_enemy_player = -1;
    g_state.mission_active = false;
    g_state.mission_won = true;
    g_state.mission_cancelled = false;

    if (chapter != nullptr) {
        RefreshChapterProgress(chapter, &g_state);
        if (g_state.mission_index == 0 || g_state.enabled_mission_count == 0) {
            g_state.chapter_finished = true;
            g_state.chapter_active = false;
        }
        ApplyMissionMedalsLocked(campaign, &g_state);
    }
    CommitLeaderProgress(&g_state);
    RefreshLeaderCounters(campaign, &g_state);
    result.ok = true;
    result.state = g_state;
    return result;
}

MissionRuntimeResult CancelMission() {
    MissionRuntimeResult result;
    if (!EnsureDatabaseReady(&result)) {
        return result;
    }
    const NDb::SGameRoot* game_root = NGameX::GetGameRoot();

    std::lock_guard<std::mutex> lock(g_mission_mutex);
    if (!g_state.active || !g_state.mission_active) {
        return ErrorResult("mission_state_inactive");
    }
    const NDb::SCampaign* campaign = CampaignByIndex(game_root, g_state.campaign_index);
    g_state.mission_active = false;
    g_state.mission_reinforcement_calls_left = 0;
    g_state.enemy_reinforcement_calls_left = 0;
    g_state.reinforcement_calls_used = 0;
    g_state.player_xp_added = 0;
    g_state.mission_exp_earned = 0;
    RestoreLeaderProgress(&g_state);
    RefreshCampaignExpProgress(campaign, &g_state);
    RefreshLeaderCounters(campaign, &g_state);
    g_state.main_enemy_player = -1;
    g_state.mission_cancelled = true;
    g_state.mission_won = false;
    result.ok = true;
    result.state = g_state;
    return result;
}

MissionRuntimeResult AdvanceToNextChapter() {
    MissionRuntimeResult result;
    if (!EnsureDatabaseReady(&result)) {
        return result;
    }
    const NDb::SGameRoot* game_root = NGameX::GetGameRoot();

    std::lock_guard<std::mutex> lock(g_mission_mutex);
    if (!g_state.active || !g_state.campaign_active) {
        return ErrorResult("campaign_state_inactive");
    }
    const NDb::SCampaign* campaign = CampaignByIndex(game_root, g_state.campaign_index);
    if (campaign == nullptr) {
        return ErrorResult("campaign_missing");
    }

    const int next_chapter_index = g_state.chapter_index < 0 ? 0 : g_state.chapter_index + 1;
    if (next_chapter_index >= campaign->chapters.size()) {
        ClearMissionSnapshot(&g_state);
        g_state.chapter_index = -1;
        g_state.chapter_id.clear();
        g_state.chapter_active = false;
        g_state.chapter_finished = true;
        g_state.campaign_active = false;
        g_state.campaign_finished = true;
        g_state.won_mission_ids.clear();
        g_state.completed_mission_ids.clear();
        g_state.enabled_mission_ids.clear();
        g_state.current_player_reinforcement_types.clear();
        g_state.chapter_reinforcements.clear();
        g_state.old_chapter_reinforcements.clear();
        g_state.reinforcement_progress.clear();
        g_state.leaders.clear();
        g_state.free_leader_indices.clear();
        g_state.chapter_reinforcement_inventory_count = 0;
        g_state.chapter_reinforcements_enabled = 0;
        g_state.chapter_reinforcements_not_enabled = 0;
        g_state.chapter_reinforcements_disabled = 0;
        g_state.chapter_reinforcements_from_previous = 0;
        g_state.old_chapter_reinforcement_inventory_count = 0;
        g_state.reinforcement_progress_count = 0;
        g_state.favorite_reinforcement_type = -1;
        g_state.favorite_reinforcement_count = 0;
        g_state.leader_rank_count = 0;
        g_state.leader_pool_count = 0;
        g_state.free_leader_count = 0;
        g_state.assigned_leader_count = 0;
        g_state.completed_mission_count = 0;
        g_state.enabled_mission_count = 0;
        result.ok = true;
        result.state = g_state;
        return result;
    }

    const NDb::SChapter* chapter = ChapterByIndex(campaign, next_chapter_index);
    if (chapter == nullptr) {
        return ErrorResult("chapter_missing");
    }
    ClearMissionSnapshot(&g_state);
    g_state.chapter_index = next_chapter_index;
    g_state.chapter_id = DbIdOf(chapter);
    g_state.chapter_script_ref = FileRef(chapter->szScriptFileRef);
    g_state.chapter_active = true;
    g_state.chapter_finished = false;
    g_state.chapter_reinforcement_calls_left = chapter->nReinforcementCalls;
    g_state.chapter_reinforcement_calls_old = 0;
    g_state.use_map_reinforcements = chapter->bUseMapReinforcements;
    g_state.won_mission_ids.clear();
    PopulateChapterReinforcementState(chapter, &g_state);
    ResetMissionRewardSnapshot(&g_state);
    RefreshChapterProgress(chapter, &g_state);
    result.ok = true;
    result.state = g_state;
    return result;
}

MissionRuntimeResult DecreaseReinforcementCallsLeft(int player, int calls) {
    MissionRuntimeResult result;
    std::lock_guard<std::mutex> lock(g_mission_mutex);
    if (!g_state.active || !g_state.mission_active) {
        return ErrorResult("mission_state_inactive");
    }

    const int actual_calls = calls == 0 ? 1 : calls;
    if (player == 0) {
        g_state.mission_reinforcement_calls_left =
                std::max(0, g_state.mission_reinforcement_calls_left - actual_calls);
        if (calls == 0) {
            ++g_state.reinforcement_calls_used;
            g_state.mission_reinforcements_called = g_state.reinforcement_calls_used;
            RefreshMissionScore(&g_state);
        }
    } else if (player == g_state.main_enemy_player) {
        g_state.enemy_reinforcement_calls_left =
                std::max(0, g_state.enemy_reinforcement_calls_left - actual_calls);
    }
    result.ok = true;
    result.state = g_state;
    return result;
}

MissionRuntimeResult IncreaseReinforcementCallsLeft(int player, int calls) {
    MissionRuntimeResult result;
    std::lock_guard<std::mutex> lock(g_mission_mutex);
    if (!g_state.active || !g_state.mission_active) {
        return ErrorResult("mission_state_inactive");
    }

    if (player == 0) {
        g_state.mission_reinforcement_calls_left += std::max(0, calls);
    } else if (player == g_state.main_enemy_player) {
        g_state.enemy_reinforcement_calls_left += std::max(0, calls);
    }
    result.ok = true;
    result.state = g_state;
    return result;
}

MissionRuntimeResult RegisterReinforcementCall(int player) {
    return DecreaseReinforcementCallsLeft(player, 0);
}

std::string SerializeMissionRuntimeState(const MissionRuntimeState& state) {
    std::ostringstream out;
    AppendCheckpointValue(out, "bk2_android_mission_checkpoint", kMissionCheckpointVersion);

#define WRITE_BOOL_FIELD(name) AppendCheckpointValue(out, #name, state.name)
#define WRITE_INT_FIELD(name) AppendCheckpointValue(out, #name, state.name)
#define WRITE_STRING_FIELD(name) AppendCheckpointString(out, #name, state.name)
#define WRITE_INT_VECTOR_FIELD(name) AppendCheckpointRaw(out, #name, EncodeIntVector(state.name))
#define WRITE_STRING_VECTOR_FIELD(name) AppendCheckpointRaw(out, #name, EncodeStringVector(state.name))

    WRITE_BOOL_FIELD(active);
    WRITE_BOOL_FIELD(mission_active);
    WRITE_BOOL_FIELD(campaign_active);
    WRITE_BOOL_FIELD(chapter_active);
    WRITE_BOOL_FIELD(campaign_finished);
    WRITE_BOOL_FIELD(chapter_finished);
    WRITE_BOOL_FIELD(tutorial);
    WRITE_BOOL_FIELD(custom);
    WRITE_INT_FIELD(campaign_index);
    WRITE_INT_FIELD(chapter_index);
    WRITE_INT_FIELD(mission_index);
    WRITE_INT_FIELD(difficulty);
    WRITE_INT_FIELD(recommended_calls);
    WRITE_INT_FIELD(mission_enable_type);
    WRITE_INT_FIELD(mission_type);
    WRITE_INT_FIELD(campaign_chapter_count);
    WRITE_INT_FIELD(chapter_mission_count);
    WRITE_INT_FIELD(completed_mission_count);
    WRITE_INT_FIELD(enabled_mission_count);
    WRITE_INT_FIELD(missions_to_enable_count);
    WRITE_INT_FIELD(continued_mission_starts);
    WRITE_INT_FIELD(player_count);
    WRITE_INT_FIELD(objective_count);
    WRITE_INT_FIELD(waiting_objective_count);
    WRITE_INT_FIELD(received_objective_count);
    WRITE_INT_FIELD(completed_objective_count);
    WRITE_INT_FIELD(failed_objective_count);
    WRITE_INT_FIELD(primary_objective_count);
    WRITE_INT_FIELD(script_movie_sequences);
    WRITE_INT_FIELD(script_camera_placements);
    WRITE_INT_FIELD(chapter_reinforcement_calls_left);
    WRITE_INT_FIELD(chapter_reinforcement_calls_old);
    WRITE_INT_FIELD(mission_reinforcement_calls_left);
    WRITE_INT_FIELD(enemy_reinforcement_calls_left);
    WRITE_INT_FIELD(reinforcement_calls_used);
    WRITE_INT_FIELD(main_enemy_player);
    WRITE_INT_FIELD(player_xp);
    WRITE_INT_FIELD(player_xp_added);
    WRITE_INT_FIELD(player_rank_index);
    WRITE_INT_FIELD(player_rank_promotions);
    WRITE_INT_FIELD(player_rank_promotions_added);
    WRITE_INT_FIELD(campaign_exp_current);
    WRITE_INT_FIELD(campaign_exp_next_level);
    WRITE_INT_FIELD(mission_time_seconds);
    WRITE_INT_FIELD(campaign_time_seconds);
    WRITE_INT_FIELD(mission_exp_earned);
    WRITE_INT_FIELD(mission_units_lost);
    WRITE_INT_FIELD(mission_units_killed);
    WRITE_INT_FIELD(mission_units_lost_price);
    WRITE_INT_FIELD(mission_units_killed_price);
    WRITE_INT_FIELD(mission_key_buildings_captured);
    WRITE_INT_FIELD(mission_reinforcements_called);
    WRITE_INT_FIELD(mission_enemy_units_max_price);
    WRITE_INT_FIELD(mission_score);
    WRITE_INT_FIELD(mission_kill_events);
    WRITE_INT_FIELD(mission_price_kill_events);
    WRITE_INT_FIELD(kill_matrix_player_count);
    WRITE_INT_FIELD(last_kill_player);
    WRITE_INT_FIELD(last_killed_player);
    WRITE_INT_FIELD(last_kill_reinforcement_type);
    WRITE_INT_FIELD(last_killed_reinforcement_type);
    WRITE_INT_FIELD(last_kill_exp_price);
    WRITE_BOOL_FIELD(last_kill_leveled_up);
    WRITE_INT_FIELD(campaign_units_lost);
    WRITE_INT_FIELD(campaign_units_killed);
    WRITE_INT_FIELD(campaign_missions_passed);
    WRITE_INT_FIELD(tactical_efficiency);
    WRITE_INT_FIELD(strategic_efficiency);
    WRITE_INT_FIELD(reward_bonus_reinforcements);
    WRITE_INT_FIELD(reward_disabled_reinforcements);
    WRITE_INT_FIELD(reward_added_calls);
    WRITE_INT_FIELD(chapter_reinforcement_inventory_count);
    WRITE_INT_FIELD(chapter_reinforcements_enabled);
    WRITE_INT_FIELD(chapter_reinforcements_not_enabled);
    WRITE_INT_FIELD(chapter_reinforcements_disabled);
    WRITE_INT_FIELD(chapter_reinforcements_from_previous);
    WRITE_INT_FIELD(old_chapter_reinforcement_inventory_count);
    WRITE_INT_FIELD(reinforcement_progress_count);
    WRITE_INT_FIELD(reinforcement_max_level);
    WRITE_INT_FIELD(favorite_reinforcement_type);
    WRITE_INT_FIELD(favorite_reinforcement_count);
    WRITE_INT_FIELD(leader_rank_count);
    WRITE_INT_FIELD(leader_pool_count);
    WRITE_INT_FIELD(free_leader_count);
    WRITE_INT_FIELD(assigned_leader_count);
    WRITE_INT_FIELD(mission_medals_awarded);
    WRITE_INT_FIELD(medal_kills_given);
    WRITE_INT_FIELD(medal_tactics_given);
    WRITE_INT_FIELD(medal_economy_given);
    WRITE_INT_FIELD(medal_munchkin_given);
    WRITE_BOOL_FIELD(medal_munchkin_blocked_by_reinforcement_xp);
    WRITE_BOOL_FIELD(use_map_reinforcements);
    WRITE_BOOL_FIELD(only_recommended_reinforcement_calls);
    WRITE_BOOL_FIELD(mission_won);
    WRITE_BOOL_FIELD(mission_cancelled);
    WRITE_BOOL_FIELD(started_from_existing_campaign_state);
    WRITE_STRING_FIELD(campaign_id);
    WRITE_STRING_FIELD(chapter_id);
    WRITE_STRING_FIELD(mission_id);
    WRITE_STRING_FIELD(campaign_script_ref);
    WRITE_STRING_FIELD(chapter_script_ref);
    WRITE_STRING_FIELD(map_data_ref);
    WRITE_STRING_FIELD(map_script_ref);
    WRITE_STRING_FIELD(intro_movie_ref);
    WRITE_STRING_FIELD(player_rank_id);
    WRITE_STRING_FIELD(player_rank_name_ref);
    WRITE_STRING_FIELD(new_player_rank_id);
    WRITE_STRING_FIELD(new_player_rank_name_ref);
    WRITE_STRING_VECTOR_FIELD(won_mission_ids);
    WRITE_STRING_VECTOR_FIELD(enabled_mission_ids);
    WRITE_STRING_VECTOR_FIELD(completed_mission_ids);
    WRITE_STRING_VECTOR_FIELD(reward_reinforcement_ids);
    WRITE_INT_VECTOR_FIELD(current_player_reinforcement_types);
    WRITE_INT_VECTOR_FIELD(reward_reinforcement_types);
    WRITE_INT_VECTOR_FIELD(reward_disabled_reinforcement_types);
    AppendCheckpointRaw(out, "chapter_reinforcements", EncodeReinforcements(state.chapter_reinforcements));
    AppendCheckpointRaw(out, "old_chapter_reinforcements", EncodeReinforcements(state.old_chapter_reinforcements));
    AppendCheckpointRaw(out, "reinforcement_progress", EncodeReinforcementProgress(state.reinforcement_progress));
    AppendCheckpointRaw(out, "leaders", EncodeLeaders(state.leaders));
    WRITE_INT_VECTOR_FIELD(free_leader_indices);
    WRITE_INT_VECTOR_FIELD(player_sides);
    WRITE_INT_VECTOR_FIELD(kill_matrix);
    WRITE_INT_VECTOR_FIELD(price_kill_matrix);
    AppendCheckpointRaw(out, "mission_medals", EncodeMedals(state.mission_medals));
    AppendCheckpointRaw(out, "objectives", EncodeObjectives(state.objectives));

#undef WRITE_STRING_VECTOR_FIELD
#undef WRITE_INT_VECTOR_FIELD
#undef WRITE_STRING_FIELD
#undef WRITE_INT_FIELD
#undef WRITE_BOOL_FIELD

    return out.str();
}

MissionRuntimeResult RestoreMissionRuntimeState(const std::string& checkpoint) {
    std::map<std::string, std::string> values;
    if (!ReadCheckpointLines(checkpoint, &values)) {
        return ErrorResult("checkpoint_parse_failed");
    }

    int version = 0;
    if (!ReadIntField(values, "bk2_android_mission_checkpoint", &version) ||
        version != kMissionCheckpointVersion) {
        return ErrorResult("checkpoint_unsupported_version");
    }

    MissionRuntimeState state;

#define READ_BOOL_FIELD(name) \
    if (!ReadBoolField(values, #name, &state.name)) { return ErrorResult(std::string("checkpoint_invalid_") + #name); }
#define READ_INT_FIELD(name) \
    if (!ReadIntField(values, #name, &state.name)) { return ErrorResult(std::string("checkpoint_invalid_") + #name); }
#define READ_STRING_FIELD(name) \
    if (!ReadStringField(values, #name, &state.name)) { return ErrorResult(std::string("checkpoint_invalid_") + #name); }
#define READ_INT_VECTOR_FIELD(name) \
    if (!ReadIntVector(values, #name, &state.name)) { return ErrorResult(std::string("checkpoint_invalid_") + #name); }
#define READ_STRING_VECTOR_FIELD(name) \
    if (!ReadStringVector(values, #name, &state.name)) { return ErrorResult(std::string("checkpoint_invalid_") + #name); }

    READ_BOOL_FIELD(active);
    READ_BOOL_FIELD(mission_active);
    READ_BOOL_FIELD(campaign_active);
    READ_BOOL_FIELD(chapter_active);
    READ_BOOL_FIELD(campaign_finished);
    READ_BOOL_FIELD(chapter_finished);
    READ_BOOL_FIELD(tutorial);
    READ_BOOL_FIELD(custom);
    READ_INT_FIELD(campaign_index);
    READ_INT_FIELD(chapter_index);
    READ_INT_FIELD(mission_index);
    READ_INT_FIELD(difficulty);
    READ_INT_FIELD(recommended_calls);
    READ_INT_FIELD(mission_enable_type);
    READ_INT_FIELD(mission_type);
    READ_INT_FIELD(campaign_chapter_count);
    READ_INT_FIELD(chapter_mission_count);
    READ_INT_FIELD(completed_mission_count);
    READ_INT_FIELD(enabled_mission_count);
    READ_INT_FIELD(missions_to_enable_count);
    READ_INT_FIELD(continued_mission_starts);
    READ_INT_FIELD(player_count);
    READ_INT_FIELD(objective_count);
    READ_INT_FIELD(waiting_objective_count);
    READ_INT_FIELD(received_objective_count);
    READ_INT_FIELD(completed_objective_count);
    READ_INT_FIELD(failed_objective_count);
    READ_INT_FIELD(primary_objective_count);
    READ_INT_FIELD(script_movie_sequences);
    READ_INT_FIELD(script_camera_placements);
    READ_INT_FIELD(chapter_reinforcement_calls_left);
    READ_INT_FIELD(chapter_reinforcement_calls_old);
    READ_INT_FIELD(mission_reinforcement_calls_left);
    READ_INT_FIELD(enemy_reinforcement_calls_left);
    READ_INT_FIELD(reinforcement_calls_used);
    READ_INT_FIELD(main_enemy_player);
    READ_INT_FIELD(player_xp);
    READ_INT_FIELD(player_xp_added);
    READ_INT_FIELD(player_rank_index);
    READ_INT_FIELD(player_rank_promotions);
    READ_INT_FIELD(player_rank_promotions_added);
    READ_INT_FIELD(campaign_exp_current);
    READ_INT_FIELD(campaign_exp_next_level);
    READ_INT_FIELD(mission_time_seconds);
    READ_INT_FIELD(campaign_time_seconds);
    READ_INT_FIELD(mission_exp_earned);
    READ_INT_FIELD(mission_units_lost);
    READ_INT_FIELD(mission_units_killed);
    READ_INT_FIELD(mission_units_lost_price);
    READ_INT_FIELD(mission_units_killed_price);
    READ_INT_FIELD(mission_key_buildings_captured);
    READ_INT_FIELD(mission_reinforcements_called);
    READ_INT_FIELD(mission_enemy_units_max_price);
    READ_INT_FIELD(mission_score);
    READ_INT_FIELD(mission_kill_events);
    READ_INT_FIELD(mission_price_kill_events);
    READ_INT_FIELD(kill_matrix_player_count);
    READ_INT_FIELD(last_kill_player);
    READ_INT_FIELD(last_killed_player);
    READ_INT_FIELD(last_kill_reinforcement_type);
    READ_INT_FIELD(last_killed_reinforcement_type);
    READ_INT_FIELD(last_kill_exp_price);
    READ_BOOL_FIELD(last_kill_leveled_up);
    READ_INT_FIELD(campaign_units_lost);
    READ_INT_FIELD(campaign_units_killed);
    READ_INT_FIELD(campaign_missions_passed);
    READ_INT_FIELD(tactical_efficiency);
    READ_INT_FIELD(strategic_efficiency);
    READ_INT_FIELD(reward_bonus_reinforcements);
    READ_INT_FIELD(reward_disabled_reinforcements);
    READ_INT_FIELD(reward_added_calls);
    READ_INT_FIELD(chapter_reinforcement_inventory_count);
    READ_INT_FIELD(chapter_reinforcements_enabled);
    READ_INT_FIELD(chapter_reinforcements_not_enabled);
    READ_INT_FIELD(chapter_reinforcements_disabled);
    READ_INT_FIELD(chapter_reinforcements_from_previous);
    READ_INT_FIELD(old_chapter_reinforcement_inventory_count);
    READ_INT_FIELD(reinforcement_progress_count);
    READ_INT_FIELD(reinforcement_max_level);
    READ_INT_FIELD(favorite_reinforcement_type);
    READ_INT_FIELD(favorite_reinforcement_count);
    READ_INT_FIELD(leader_rank_count);
    READ_INT_FIELD(leader_pool_count);
    READ_INT_FIELD(free_leader_count);
    READ_INT_FIELD(assigned_leader_count);
    READ_INT_FIELD(mission_medals_awarded);
    READ_INT_FIELD(medal_kills_given);
    READ_INT_FIELD(medal_tactics_given);
    READ_INT_FIELD(medal_economy_given);
    READ_INT_FIELD(medal_munchkin_given);
    READ_BOOL_FIELD(medal_munchkin_blocked_by_reinforcement_xp);
    READ_BOOL_FIELD(use_map_reinforcements);
    READ_BOOL_FIELD(only_recommended_reinforcement_calls);
    READ_BOOL_FIELD(mission_won);
    READ_BOOL_FIELD(mission_cancelled);
    READ_BOOL_FIELD(started_from_existing_campaign_state);
    READ_STRING_FIELD(campaign_id);
    READ_STRING_FIELD(chapter_id);
    READ_STRING_FIELD(mission_id);
    READ_STRING_FIELD(campaign_script_ref);
    READ_STRING_FIELD(chapter_script_ref);
    READ_STRING_FIELD(map_data_ref);
    READ_STRING_FIELD(map_script_ref);
    READ_STRING_FIELD(intro_movie_ref);
    READ_STRING_FIELD(player_rank_id);
    READ_STRING_FIELD(player_rank_name_ref);
    READ_STRING_FIELD(new_player_rank_id);
    READ_STRING_FIELD(new_player_rank_name_ref);
    READ_STRING_VECTOR_FIELD(won_mission_ids);
    READ_STRING_VECTOR_FIELD(enabled_mission_ids);
    READ_STRING_VECTOR_FIELD(completed_mission_ids);
    READ_STRING_VECTOR_FIELD(reward_reinforcement_ids);
    READ_INT_VECTOR_FIELD(current_player_reinforcement_types);
    READ_INT_VECTOR_FIELD(reward_reinforcement_types);
    READ_INT_VECTOR_FIELD(reward_disabled_reinforcement_types);
    if (!DecodeReinforcements(values, "chapter_reinforcements", &state.chapter_reinforcements)) {
        return ErrorResult("checkpoint_invalid_chapter_reinforcements");
    }
    if (!DecodeReinforcements(values, "old_chapter_reinforcements", &state.old_chapter_reinforcements)) {
        return ErrorResult("checkpoint_invalid_old_chapter_reinforcements");
    }
    if (!DecodeReinforcementProgress(values, "reinforcement_progress", &state.reinforcement_progress)) {
        return ErrorResult("checkpoint_invalid_reinforcement_progress");
    }
    if (!DecodeLeaders(values, "leaders", &state.leaders)) {
        return ErrorResult("checkpoint_invalid_leaders");
    }
    READ_INT_VECTOR_FIELD(free_leader_indices);
    READ_INT_VECTOR_FIELD(player_sides);
    READ_INT_VECTOR_FIELD(kill_matrix);
    READ_INT_VECTOR_FIELD(price_kill_matrix);
    if (!DecodeMedals(values, "mission_medals", &state.mission_medals)) {
        return ErrorResult("checkpoint_invalid_mission_medals");
    }
    if (!DecodeObjectives(values, "objectives", &state.objectives)) {
        return ErrorResult("checkpoint_invalid_objectives");
    }

#undef READ_STRING_VECTOR_FIELD
#undef READ_INT_VECTOR_FIELD
#undef READ_STRING_FIELD
#undef READ_INT_FIELD
#undef READ_BOOL_FIELD

    MissionRuntimeResult result;
    result.ok = true;
    {
        std::lock_guard<std::mutex> lock(g_mission_mutex);
        g_state = state;
        result.state = g_state;
    }
    return result;
}

MissionRuntimeResult SaveMissionRuntimeCheckpoint(const std::string& slot_name) {
    const MissionRuntimeState state = GetMissionRuntimeState();
    if (!state.active) {
        return ErrorResult("mission_state_inactive");
    }

    const std::string path = CurrentCheckpointPath(slot_name);
    const size_t slash = path.find_last_of('/');
    if (slash != std::string::npos && !EnsureDirectory(path.substr(0, slash))) {
        return ErrorResult("checkpoint_directory_failed");
    }

    const std::string checkpoint = SerializeMissionRuntimeState(state);
    FILE* file = fopen(path.c_str(), "wb");
    if (file == nullptr) {
        return ErrorResult("checkpoint_open_failed");
    }
    const size_t written = fwrite(checkpoint.data(), 1, checkpoint.size(), file);
    const int close_result = fclose(file);
    if (written != checkpoint.size() || close_result != 0) {
        return ErrorResult("checkpoint_write_failed");
    }

    MissionRuntimeResult result;
    result.ok = true;
    result.state = state;
    return result;
}

MissionRuntimeResult LoadMissionRuntimeCheckpoint(const std::string& slot_name) {
    const std::string path = CurrentCheckpointPath(slot_name);
    FILE* file = fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return ErrorResult("checkpoint_open_failed");
    }

    std::string checkpoint;
    char buffer[4096];
    while (true) {
        const size_t read = fread(buffer, 1, sizeof(buffer), file);
        if (read > 0) {
            checkpoint.append(buffer, read);
        }
        if (read < sizeof(buffer)) {
            if (ferror(file)) {
                fclose(file);
                return ErrorResult("checkpoint_read_failed");
            }
            break;
        }
    }
    fclose(file);
    return RestoreMissionRuntimeState(checkpoint);
}

MissionRuntimeState GetMissionRuntimeState() {
    std::lock_guard<std::mutex> lock(g_mission_mutex);
    return g_state;
}

bool QueueMissionHudNotification(
        const std::string& text_file_ref,
        uint32_t visible_millis,
        const std::string& suffix_file_ref) {
    std::string text = LoadUtf16Text(text_file_ref);
    const std::string suffix = LoadUtf16Text(suffix_file_ref);
    if (!text.empty() && !suffix.empty() &&
        text[text.size() - 1] != ' ') {
        text.push_back(' ');
    }
    text += suffix;
    if (text.empty()) {
        return false;
    }

    MissionHudNotification notification;
    notification.text = text;
    const uint32_t lifetime =
            visible_millis == 0
                    ? kMissionHudNotificationVisibleMillis
                    : visible_millis;
    notification.expires_at_millis =
            PlatformRuntime::instance().monotonic_millis() +
            lifetime;

    std::lock_guard<std::mutex> lock(g_hud_notification_mutex);
    if (g_hud_notifications.size() >=
        kMaxQueuedMissionHudNotifications) {
        g_hud_notifications.pop_front();
    }
    g_hud_notifications.push_back(std::move(notification));
    return true;
}

void ResetMissionHudNotifications() {
    std::lock_guard<std::mutex> lock(g_hud_notification_mutex);
    g_hud_notifications.clear();
}

std::string GetMissionHudHeadlineText() {
    {
        const uint64_t now =
                PlatformRuntime::instance().monotonic_millis();
        std::lock_guard<std::mutex> lock(g_hud_notification_mutex);
        g_hud_notifications.erase(
                std::remove_if(
                        g_hud_notifications.begin(),
                        g_hud_notifications.end(),
                        [now](const MissionHudNotification& notification) {
                            return notification.expires_at_millis <= now;
                        }),
                g_hud_notifications.end());
        if (!g_hud_notifications.empty()) {
            const size_t first =
                    g_hud_notifications.size() > 3
                            ? g_hud_notifications.size() - 3
                            : 0;
            std::string text;
            for (size_t index = first;
                 index < g_hud_notifications.size();
                 ++index) {
                if (!text.empty()) {
                    text.push_back('\n');
                }
                text += g_hud_notifications[index].text;
            }
            return text;
        }
    }

    const MissionRuntimeState state = GetMissionRuntimeState();
    const MissionObjectiveState* active = nullptr;
    for (std::vector<MissionObjectiveState>::const_iterator it =
                 state.objectives.begin();
         it != state.objectives.end();
         ++it) {
        if (it->state != EMOS_RECEIVED) {
            continue;
        }
        if (active == nullptr || (!active->primary && it->primary)) {
            active = &*it;
        }
    }
    if (active == nullptr) {
        return "";
    }
    std::string text = LoadUtf16Text(active->header_ref);
    if (text.empty()) {
        text = LoadUtf16Text(active->description_ref);
    }
    return text;
}

std::string GetMissionHudStatusText() {
    const MissionRuntimeState state = GetMissionRuntimeState();
    std::ostringstream text;
    if (!state.active) {
        text << "Objectives: loading";
        return text.str();
    }

    text << "Objectives: " << state.completed_objective_count
         << "/" << state.objective_count;
    if (state.received_objective_count > 0) {
        text << "   Active: " << state.received_objective_count;
    }
    if (state.failed_objective_count > 0) {
        text << "   Failed: " << state.failed_objective_count;
    }

    const MissionObjectiveState* active = nullptr;
    for (std::vector<MissionObjectiveState>::const_iterator it = state.objectives.begin();
         it != state.objectives.end();
         ++it) {
        if (it->state != EMOS_RECEIVED) {
            continue;
        }
        if (active == nullptr || (!active->primary && it->primary)) {
            active = &*it;
        }
    }
    if (active != nullptr) {
        std::string objective_text = LoadUtf16Text(active->header_ref);
        if (objective_text.empty()) {
            objective_text = LoadUtf16Text(active->description_ref);
        }
        if (!objective_text.empty()) {
            text << "\nCurrent: " << objective_text;
        }
    }
    return text.str();
}

void ResetMissionRuntimeState() {
    std::lock_guard<std::mutex> lock(g_mission_mutex);
    g_state = MissionRuntimeState();
}

std::string RunFirstCampaignMissionProgressionProbe() {
    MissionRuntimeResult ready;
    if (!EnsureDatabaseReady(&ready)) {
        return std::string("mission_progression=failed; mission_error=") + ready.error;
    }

    const NDb::SGameRoot* game_root = NGameX::GetGameRoot();
    if (game_root == nullptr) {
        return "mission_progression=failed; mission_error=game_root_missing";
    }

    MissionLocation location;
    if (!FindCampaignMissionForProgressionProbe(game_root, &location)) {
        return "mission_progression=failed; mission_error=no_campaign_missions";
    }

    const MissionRuntimeState previous_state = GetMissionRuntimeState();
    ScopedMissionStateRestore restore(previous_state);

    MissionRuntimeResult result = StartCampaignMissionState(
            location.campaign_index,
            location.chapter_index,
            location.mission_index,
            0);
    if (!result.ok) {
        return std::string("mission_progression=failed; mission_step=start; mission_error=") + result.error;
    }

    int objective_index = -1;
    for (int i = 0; i < result.state.objectives.size(); ++i) {
        if (result.state.objectives[i].experience > 0) {
            objective_index = i;
            break;
        }
    }
    if (objective_index < 0 && !result.state.objectives.empty()) {
        objective_index = 0;
    }

    bool objective_completed = false;
    if (objective_index >= 0) {
        result = SetMissionObjectiveState(objective_index, EMOS_COMPLETED);
        if (!result.ok) {
            return std::string("mission_progression=failed; mission_step=objective; mission_error=") + result.error;
        }
        objective_completed = true;
    } else {
        result = AddPlayerXp(10);
        if (!result.ok) {
            return std::string("mission_progression=failed; mission_step=add_xp; mission_error=") + result.error;
        }
    }

    const int statistic_steps[][2] = {
            {kMissionStatisticTime, 180},
    };
    for (int i = 0; i < sizeof(statistic_steps) / sizeof(statistic_steps[0]); ++i) {
        result = SetMissionStatistic(statistic_steps[i][0], statistic_steps[i][1]);
        if (!result.ok) {
            return std::string("mission_progression=failed; mission_step=statistics; mission_error=") + result.error;
        }
    }

    if (result.state.mission_reinforcement_calls_left > 0) {
        result = RegisterReinforcementCall(0);
        if (!result.ok) {
            return std::string("mission_progression=failed; mission_step=reinforcement; mission_error=") + result.error;
        }
    }

    bool leader_assigned = false;
    bool reinforcement_xp_given = false;
    bool kill_events_registered = false;
    int leader_reinforcement_type = -1;
    if (result.state.player_rank_promotions > 0 && result.state.free_leader_count > 0) {
        for (std::vector<MissionReinforcementState>::const_iterator it = result.state.chapter_reinforcements.begin();
             it != result.state.chapter_reinforcements.end();
             ++it) {
            if (it->state == kReinforcementEnabled && !IsSpecialMaxLevelReinforcementType(it->type)) {
                leader_reinforcement_type = it->type;
                break;
            }
        }
        if (leader_reinforcement_type < 0) {
            for (std::vector<MissionReinforcementProgressState>::const_iterator it =
                         result.state.reinforcement_progress.begin();
                 it != result.state.reinforcement_progress.end();
                 ++it) {
                if (!IsSpecialMaxLevelReinforcementType(it->type)) {
                    leader_reinforcement_type = it->type;
                    break;
                }
            }
        }
        if (leader_reinforcement_type >= 0) {
            result = AssignLeaderToReinforcement(leader_reinforcement_type, 0);
            if (!result.ok) {
                return std::string("mission_progression=failed; mission_step=assign_leader; mission_error=") +
                        result.error;
            }
            leader_assigned = true;
            result = MarkFavoriteReinforcement(leader_reinforcement_type);
            if (!result.ok) {
                return std::string("mission_progression=failed; mission_step=favorite_reinf; mission_error=") +
                        result.error;
            }
        }
    }

    int enemy_player = result.state.main_enemy_player >= 0 ? result.state.main_enemy_player : 1;
    if (enemy_player >= result.state.player_count) {
        enemy_player = -1;
    }
    if (leader_reinforcement_type < 0) {
        leader_reinforcement_type = 0;
    }
    if (enemy_player >= 0) {
        result = RegisterUnitKill(
                0,
                0,
                leader_reinforcement_type,
                enemy_player,
                0,
                NDb::RT_TANKS,
                700,
                false);
        if (!result.ok) {
            return std::string("mission_progression=failed; mission_step=kill_event; mission_error=") + result.error;
        }
        kill_events_registered = true;
        reinforcement_xp_given = leader_assigned;

        result = RegisterUnitKill(
                enemy_player,
                0,
                NDb::RT_TANKS,
                0,
                0,
                leader_reinforcement_type,
                100,
                false);
        if (!result.ok) {
            return std::string("mission_progression=failed; mission_step=loss_event; mission_error=") + result.error;
        }
    }

    result = MarkMissionWon();
    if (!result.ok) {
        return std::string("mission_progression=failed; mission_step=win; mission_error=") + result.error;
    }
    const int post_win_enabled_missions = result.state.enabled_mission_count;
    const int post_win_completed_missions = result.state.completed_mission_count;

    bool continued_mission_started = false;
    if (result.state.campaign_active && result.state.chapter_active && result.state.enabled_mission_count > 0) {
        result = StartFirstEnabledCampaignMissionState(0);
        if (!result.ok) {
            return std::string("mission_progression=failed; mission_step=continue_campaign; mission_error=") +
                    result.error;
        }
        continued_mission_started = true;
    }

    std::ostringstream out;
    out << "mission_progression=probed"
        << "; probe_campaign_index=" << location.campaign_index
        << "; probe_chapter_index=" << location.chapter_index
        << "; probe_mission_index=" << location.mission_index
        << "; probe_had_objectives=" << (location.has_objectives ? "true" : "false")
        << "; probe_objective_completed=" << (objective_completed ? "true" : "false")
        << "; probe_had_rewards=" << (location.has_rewards ? "true" : "false")
        << "; probe_leader_assigned=" << (leader_assigned ? "true" : "false")
        << "; probe_leader_reinf_type=" << leader_reinforcement_type
        << "; probe_kill_events_registered=" << (kill_events_registered ? "true" : "false")
        << "; probe_reinforcement_xp_given=" << (reinforcement_xp_given ? "true" : "false")
        << "; probe_post_win_enabled_missions=" << post_win_enabled_missions
        << "; probe_post_win_completed_missions=" << post_win_completed_missions
        << "; probe_continued_mission_started=" << (continued_mission_started ? "true" : "false")
        << "; " << DescribeMissionRuntimeState(result.state);
    return out.str();
}

std::string RunMissionCheckpointProbe() {
    MissionRuntimeResult ready;
    if (!EnsureDatabaseReady(&ready)) {
        return std::string("mission_checkpoint=failed; checkpoint_error=") + ready.error;
    }

    const NDb::SGameRoot* game_root = NGameX::GetGameRoot();
    if (game_root == nullptr) {
        return "mission_checkpoint=failed; checkpoint_error=game_root_missing";
    }

    MissionLocation location;
    if (!FindCampaignMissionForProgressionProbe(game_root, &location)) {
        return "mission_checkpoint=failed; checkpoint_error=no_campaign_missions";
    }

    const MissionRuntimeState previous_state = GetMissionRuntimeState();
    ScopedMissionStateRestore restore(previous_state);

    MissionRuntimeResult result = StartCampaignMissionState(
            location.campaign_index,
            location.chapter_index,
            location.mission_index,
            0);
    if (!result.ok) {
        return std::string("mission_checkpoint=failed; checkpoint_step=start; checkpoint_error=") + result.error;
    }

    result = AddPlayerXp(25);
    if (!result.ok) {
        return std::string("mission_checkpoint=failed; checkpoint_step=add_xp; checkpoint_error=") + result.error;
    }
    result = MarkMissionWon();
    if (!result.ok) {
        return std::string("mission_checkpoint=failed; checkpoint_step=win; checkpoint_error=") + result.error;
    }
    if (result.state.campaign_active && result.state.chapter_active && result.state.enabled_mission_count > 0) {
        result = StartFirstEnabledCampaignMissionState(0);
        if (!result.ok) {
            return std::string("mission_checkpoint=failed; checkpoint_step=continue_campaign; checkpoint_error=") +
                    result.error;
        }
    }

    const MissionRuntimeState saved_state = result.state;
    const char* kProbeSlot = "android_progression_probe";
    result = SaveMissionRuntimeCheckpoint(kProbeSlot);
    if (!result.ok) {
        return std::string("mission_checkpoint=failed; checkpoint_step=save; checkpoint_error=") + result.error;
    }

    ResetMissionRuntimeState();
    result = LoadMissionRuntimeCheckpoint(kProbeSlot);
    if (!result.ok) {
        return std::string("mission_checkpoint=failed; checkpoint_step=load; checkpoint_error=") + result.error;
    }

    const MissionRuntimeState loaded_state = result.state;
    const bool roundtrip_ok =
            loaded_state.active == saved_state.active &&
            loaded_state.mission_active == saved_state.mission_active &&
            loaded_state.campaign_index == saved_state.campaign_index &&
            loaded_state.chapter_index == saved_state.chapter_index &&
            loaded_state.mission_index == saved_state.mission_index &&
            loaded_state.player_xp == saved_state.player_xp &&
            loaded_state.campaign_exp_current == saved_state.campaign_exp_current &&
            loaded_state.chapter_reinforcement_calls_left == saved_state.chapter_reinforcement_calls_left &&
            loaded_state.continued_mission_starts == saved_state.continued_mission_starts &&
            loaded_state.mission_id == saved_state.mission_id &&
            loaded_state.won_mission_ids == saved_state.won_mission_ids &&
            loaded_state.enabled_mission_ids == saved_state.enabled_mission_ids &&
            loaded_state.chapter_reinforcements.size() == saved_state.chapter_reinforcements.size() &&
            loaded_state.reinforcement_progress.size() == saved_state.reinforcement_progress.size() &&
            loaded_state.leaders.size() == saved_state.leaders.size() &&
            loaded_state.objectives.size() == saved_state.objectives.size();
    const std::string path = CurrentCheckpointPath(kProbeSlot);

    std::ostringstream out;
    out << "mission_checkpoint=probed"
        << "; checkpoint_saved=true"
        << "; checkpoint_loaded=true"
        << "; checkpoint_roundtrip=" << (roundtrip_ok ? "true" : "false")
        << "; checkpoint_path=" << path
        << "; checkpoint_bytes=" << FileSize(path)
        << "; " << DescribeMissionRuntimeState(loaded_state);
    return out.str();
}

std::string DescribeMissionRuntimeState(const MissionRuntimeState& state) {
    std::ostringstream out;
    out << "mission_state=" << (state.active ? "active" : "inactive")
        << "; mission_active=" << (state.mission_active ? "true" : "false")
        << "; campaign_active=" << (state.campaign_active ? "true" : "false")
        << "; chapter_active=" << (state.chapter_active ? "true" : "false")
        << "; campaign_finished=" << (state.campaign_finished ? "true" : "false")
        << "; chapter_finished=" << (state.chapter_finished ? "true" : "false")
        << "; mission_tutorial=" << (state.tutorial ? "true" : "false")
        << "; mission_campaign_index=" << state.campaign_index
        << "; mission_chapter_index=" << state.chapter_index
        << "; mission_index=" << state.mission_index
        << "; campaign_chapters=" << state.campaign_chapter_count
        << "; chapter_missions=" << state.chapter_mission_count
        << "; chapter_completed_missions=" << state.completed_mission_count
        << "; chapter_enabled_missions=" << state.enabled_mission_count
        << "; chapter_missions_to_enable=" << state.missions_to_enable_count
        << "; continued_mission_starts=" << state.continued_mission_starts
        << "; started_from_existing_campaign_state="
        << (state.started_from_existing_campaign_state ? "true" : "false")
        << "; mission_id=" << (state.mission_id.empty() ? "<none>" : state.mission_id)
        << "; mission_campaign_id=" << (state.campaign_id.empty() ? "<none>" : state.campaign_id)
        << "; mission_chapter_id=" << (state.chapter_id.empty() ? "<none>" : state.chapter_id)
        << "; mission_map_ref=" << (state.map_data_ref.empty() ? "<none>" : state.map_data_ref)
        << "; mission_script_ref=" << (state.map_script_ref.empty() ? "<none>" : state.map_script_ref)
        << "; mission_campaign_script_ref=" << (state.campaign_script_ref.empty() ? "<none>" : state.campaign_script_ref)
        << "; mission_chapter_script_ref=" << (state.chapter_script_ref.empty() ? "<none>" : state.chapter_script_ref)
        << "; mission_intro_movie_ref=" << (state.intro_movie_ref.empty() ? "<none>" : state.intro_movie_ref)
        << "; mission_players=" << state.player_count
        << "; mission_objectives=" << state.objective_count
        << "; mission_objectives_waiting=" << state.waiting_objective_count
        << "; mission_objectives_received=" << state.received_objective_count
        << "; mission_objectives_completed=" << state.completed_objective_count
        << "; mission_objectives_failed=" << state.failed_objective_count
        << "; mission_primary_objectives=" << state.primary_objective_count
        << "; mission_script_movies=" << state.script_movie_sequences
        << "; mission_script_cameras=" << state.script_camera_placements
        << "; chapter_reinf_calls_left=" << state.chapter_reinforcement_calls_left
        << "; chapter_reinf_calls_old=" << state.chapter_reinforcement_calls_old
        << "; mission_reinf_calls_left=" << state.mission_reinforcement_calls_left
        << "; enemy_reinf_calls_left=" << state.enemy_reinforcement_calls_left
        << "; mission_reinf_calls_used=" << state.reinforcement_calls_used
        << "; mission_reinf_called_stat=" << state.mission_reinforcements_called
        << "; mission_main_enemy=" << state.main_enemy_player
        << "; use_map_reinforcements=" << (state.use_map_reinforcements ? "true" : "false")
        << "; only_recommended_reinforcements=" << (state.only_recommended_reinforcement_calls ? "true" : "false")
        << "; mission_recommended_calls=" << state.recommended_calls
        << "; player_xp=" << state.player_xp
        << "; player_xp_pending=" << state.player_xp_added
        << "; player_rank_index=" << state.player_rank_index
        << "; player_rank_id=" << (state.player_rank_id.empty() ? "<none>" : state.player_rank_id)
        << "; player_rank_name_ref=" << (state.player_rank_name_ref.empty() ? "<none>" : state.player_rank_name_ref)
        << "; new_player_rank_id=" << (state.new_player_rank_id.empty() ? "<none>" : state.new_player_rank_id)
        << "; player_promotions=" << state.player_rank_promotions
        << "; player_promotions_added=" << state.player_rank_promotions_added
        << "; campaign_exp_current=" << state.campaign_exp_current
        << "; campaign_exp_next_rank=" << state.campaign_exp_next_level
        << "; mission_time_seconds=" << state.mission_time_seconds
        << "; campaign_time_seconds=" << state.campaign_time_seconds
        << "; mission_exp_earned=" << state.mission_exp_earned
        << "; mission_units_lost=" << state.mission_units_lost
        << "; mission_units_killed=" << state.mission_units_killed
        << "; mission_units_lost_price=" << state.mission_units_lost_price
        << "; mission_units_killed_price=" << state.mission_units_killed_price
        << "; mission_kill_events=" << state.mission_kill_events
        << "; mission_price_kill_events=" << state.mission_price_kill_events
        << "; kill_matrix_players=" << state.kill_matrix_player_count
        << "; last_kill_player=" << state.last_kill_player
        << "; last_killed_player=" << state.last_killed_player
        << "; last_kill_reinf_type=" << state.last_kill_reinforcement_type
        << "; last_killed_reinf_type=" << state.last_killed_reinforcement_type
        << "; last_kill_exp_price=" << state.last_kill_exp_price
        << "; last_kill_leveled_up=" << (state.last_kill_leveled_up ? "true" : "false")
        << "; mission_key_buildings=" << state.mission_key_buildings_captured
        << "; mission_enemy_units_max_price=" << state.mission_enemy_units_max_price
        << "; mission_score=" << state.mission_score
        << "; campaign_units_lost=" << state.campaign_units_lost
        << "; campaign_units_killed=" << state.campaign_units_killed
        << "; campaign_missions_passed=" << state.campaign_missions_passed
        << "; tactical_efficiency=" << state.tactical_efficiency
        << "; strategic_efficiency=" << state.strategic_efficiency
        << "; chapter_player_reinf_types=" << state.current_player_reinforcement_types.size()
        << "; reward_bonus_reinforcements=" << state.reward_bonus_reinforcements
        << "; reward_disabled_reinforcements=" << state.reward_disabled_reinforcements
        << "; reward_added_calls=" << state.reward_added_calls
        << "; chapter_reinf_inventory=" << state.chapter_reinforcement_inventory_count
        << "; chapter_reinf_enabled=" << state.chapter_reinforcements_enabled
        << "; chapter_reinf_not_enabled=" << state.chapter_reinforcements_not_enabled
        << "; chapter_reinf_disabled=" << state.chapter_reinforcements_disabled
        << "; chapter_reinf_from_previous=" << state.chapter_reinforcements_from_previous
        << "; old_chapter_reinf_inventory=" << state.old_chapter_reinforcement_inventory_count
        << "; reinforcement_progress=" << state.reinforcement_progress_count
        << "; reinforcement_max_level=" << state.reinforcement_max_level
        << "; favorite_reinf_type=" << state.favorite_reinforcement_type
        << "; favorite_reinf_count=" << state.favorite_reinforcement_count
        << "; leader_ranks=" << state.leader_rank_count
        << "; leader_pool=" << state.leader_pool_count
        << "; free_leaders=" << state.free_leader_count
        << "; assigned_leaders=" << state.assigned_leader_count
        << "; mission_medals_awarded=" << state.mission_medals_awarded
        << "; medal_kills_given=" << state.medal_kills_given
        << "; medal_tactics_given=" << state.medal_tactics_given
        << "; medal_economy_given=" << state.medal_economy_given
        << "; medal_munchkin_given=" << state.medal_munchkin_given
        << "; medal_munchkin_blocked_by_reinf_xp="
        << (state.medal_munchkin_blocked_by_reinforcement_xp ? "true" : "false")
        << "; mission_won=" << (state.mission_won ? "true" : "false")
        << "; mission_cancelled=" << (state.mission_cancelled ? "true" : "false");
    return out.str();
}

}  // namespace bk2::android

extern "C" JNIEXPORT jstring JNICALL
Java_com_nival_blitzkrieg2_NativeBridge_startFirstCampaignMissionProbe(JNIEnv* env, jclass) {
    const bk2::android::MissionRuntimeResult result =
            bk2::android::StartFirstCampaignMissionState();
    std::string text = result.ok
            ? bk2::android::DescribeMissionRuntimeState(result.state)
            : std::string("mission_state=failed; mission_error=") + result.error;
    return env->NewStringUTF(text.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_nival_blitzkrieg2_NativeBridge_runMissionProgressionProbe(JNIEnv* env, jclass) {
    const std::string text = bk2::android::RunFirstCampaignMissionProgressionProbe();
    return env->NewStringUTF(text.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_nival_blitzkrieg2_NativeBridge_runMissionCheckpointProbe(JNIEnv* env, jclass) {
    const std::string text = bk2::android::RunMissionCheckpointProbe();
    return env->NewStringUTF(text.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_nival_blitzkrieg2_NativeBridge_getMissionOutcome(JNIEnv* env, jclass) {
    return env->NewStringUTF(bk2::android::LegacyMissionOutcome());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_nival_blitzkrieg2_NativeBridge_getMissionHudHeadline(JNIEnv* env, jclass) {
    const std::string text = bk2::android::GetMissionHudHeadlineText();
    return env->NewStringUTF(text.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_nival_blitzkrieg2_NativeBridge_getMissionHudStatus(JNIEnv* env, jclass) {
    const std::string text = bk2::android::GetMissionHudStatusText();
    return env->NewStringUTF(text.c_str());
}

extern "C" JNIEXPORT void JNICALL
Java_com_nival_blitzkrieg2_NativeBridge_forfeitMission(JNIEnv*, jclass) {
    bk2::android::HandleLegacyInputEvent("local_loose");
}
