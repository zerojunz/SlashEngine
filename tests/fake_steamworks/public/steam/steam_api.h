#pragma once

// A stand-in for the Steamworks SDK's steam_api.h, so the steamworks package's glue code compiles
// and runs in the tests without Valve's SDK, which sits behind a Steamworks login. It declares
// only what the glue calls, with the names and shapes Valve's public documentation gives, and
// plays Steam from fake_steamworks::state: the tests set the player, achievements and
// leaderboards there, and read back what the glue did.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <vector>

using uint8 = std::uint8_t;
using int32 = std::int32_t;
using uint32 = std::uint32_t;
using uint64 = std::uint64_t;
using HSteamPipe = int32;
using HSteamUser = int32;
using SteamAPICall_t = uint64;
using SteamLeaderboard_t = uint64;
using SteamLeaderboardEntries_t = uint64;
using UGCHandle_t = uint64;

constexpr SteamAPICall_t k_uAPICallInvalid = 0;

class CSteamID {
public:
    CSteamID() = default;
    explicit CSteamID(uint64 id) : id_(id) {}
    [[nodiscard]] uint64 ConvertToUint64() const { return id_; }

private:
    uint64 id_ = 0;
};

enum ELeaderboardDataRequest : int {
    k_ELeaderboardDataRequestGlobal = 0,
    k_ELeaderboardDataRequestGlobalAroundUser = 1,
    k_ELeaderboardDataRequestFriends = 2,
};

enum ELeaderboardUploadScoreMethod : int {
    k_ELeaderboardUploadScoreMethodNone = 0,
    k_ELeaderboardUploadScoreMethodKeepBest = 1,
    k_ELeaderboardUploadScoreMethodForceUpdate = 2,
};

struct LeaderboardEntry_t {
    CSteamID m_steamIDUser;
    int32 m_nGlobalRank = 0;
    int32 m_nScore = 0;
    int32 m_cDetails = 0;
    UGCHandle_t m_hUGC = 0;
};

struct CallbackMsg_t {
    HSteamUser m_hSteamUser = 0;
    int m_iCallback = 0;
    uint8* m_pubParam = nullptr;
    int m_cubParam = 0;
};

struct SteamAPICallCompleted_t {
    static constexpr int k_iCallback = 703;
    SteamAPICall_t m_hAsyncCall = 0;
    int m_iCallback = 0;
    uint32 m_cubParam = 0;
};

struct GameOverlayActivated_t {
    static constexpr int k_iCallback = 331;
    uint8 m_bActive = 0;
};

struct LeaderboardFindResult_t {
    static constexpr int k_iCallback = 1104;
    SteamLeaderboard_t m_hSteamLeaderboard = 0;
    uint8 m_bLeaderboardFound = 0;
};

struct LeaderboardScoresDownloaded_t {
    static constexpr int k_iCallback = 1105;
    SteamLeaderboard_t m_hSteamLeaderboard = 0;
    SteamLeaderboardEntries_t m_hSteamLeaderboardEntries = 0;
    int m_cEntryCount = 0;
};

struct LeaderboardScoreUploaded_t {
    static constexpr int k_iCallback = 1106;
    uint8 m_bSuccess = 0;
    SteamLeaderboard_t m_hSteamLeaderboard = 0;
    int32 m_nScore = 0;
    uint8 m_bScoreChanged = 0;
    int m_nGlobalRankNew = 0;
    int m_nGlobalRankPrevious = 0;
};

namespace fake_steamworks {

struct Player {
    uint64 id = 0;
    std::string name;
    bool friend_of_player = false;
};

struct Leaderboard {
    std::string name;
    std::vector<std::pair<uint64, int32>> scores; // by player, best first
};

// What the fake Steam knows and what the glue did.
struct State {
    bool running = true; // whether SteamAPI_Init finds Steam
    bool initialized = false;
    uint64 player = 76561197960287930ULL;
    std::map<uint64, Player> players;
    std::map<std::string, bool> achievements; // by API name: unlocked
    std::vector<std::string> stored;          // "<name>" set, "<name> <cur>/<max>" progress
    std::vector<Leaderboard> leaderboards;    // handles are positions + 1
    bool refuse_uploads = false;
    std::vector<std::string> overlays;                        // dialogs ActivateGameOverlay opened
    std::deque<std::pair<int, std::vector<uint8>>> callbacks; // posted callbacks
    std::map<SteamAPICall_t, std::pair<int, std::vector<uint8>>> results; // finished calls
    SteamAPICall_t next_call = 1;
    std::map<SteamLeaderboardEntries_t, std::vector<LeaderboardEntry_t>> downloads;
    std::vector<uint8> last; // the callback GetNextCallback handed out
};

inline State state;

template <typename T>
void post(const T& value) {
    std::vector<uint8> bytes(sizeof(T));
    std::memcpy(bytes.data(), &value, sizeof(T));
    state.callbacks.emplace_back(T::k_iCallback, std::move(bytes));
}

// Finishes an API call with result, as Steam does a frame later.
template <typename T>
SteamAPICall_t finish(const T& result) {
    const SteamAPICall_t call = state.next_call++;
    std::vector<uint8> bytes(sizeof(T));
    std::memcpy(bytes.data(), &result, sizeof(T));
    state.results[call] = {T::k_iCallback, std::move(bytes)};
    SteamAPICallCompleted_t done;
    done.m_hAsyncCall = call;
    done.m_iCallback = T::k_iCallback;
    done.m_cubParam = sizeof(T);
    post(done);
    return call;
}

inline void reset() {
    state = State{};
}

} // namespace fake_steamworks

class ISteamUser {
public:
    [[nodiscard]] CSteamID GetSteamID() { return CSteamID(fake_steamworks::state.player); }
};

class ISteamFriends {
public:
    const char* GetPersonaName() {
        return fake_steamworks::state.players[fake_steamworks::state.player].name.c_str();
    }
    const char* GetFriendPersonaName(CSteamID id) {
        return fake_steamworks::state.players[id.ConvertToUint64()].name.c_str();
    }
    void ActivateGameOverlay(const char* dialog) {
        fake_steamworks::state.overlays.emplace_back(dialog);
        GameOverlayActivated_t shown;
        shown.m_bActive = 1;
        fake_steamworks::post(shown);
        GameOverlayActivated_t closed;
        fake_steamworks::post(closed);
    }
};

class ISteamUserStats {
public:
    bool GetAchievement(const char* name, bool* achieved) {
        const auto it = fake_steamworks::state.achievements.find(name);
        if (it == fake_steamworks::state.achievements.end()) {
            return false;
        }
        *achieved = it->second;
        return true;
    }
    bool SetAchievement(const char* name) {
        const auto it = fake_steamworks::state.achievements.find(name);
        if (it == fake_steamworks::state.achievements.end()) {
            return false;
        }
        it->second = true;
        fake_steamworks::state.stored.emplace_back(name);
        return true;
    }
    bool IndicateAchievementProgress(const char* name, uint32 current, uint32 max) {
        if (!fake_steamworks::state.achievements.contains(name)) {
            return false;
        }
        fake_steamworks::state.stored.push_back(std::string(name) + " " + std::to_string(current) +
                                                "/" + std::to_string(max));
        return true;
    }
    bool StoreStats() { return true; }

    SteamAPICall_t FindLeaderboard(const char* name) {
        LeaderboardFindResult_t found;
        const auto& boards = fake_steamworks::state.leaderboards;
        for (std::size_t i = 0; i < boards.size(); ++i) {
            if (boards[i].name == name) {
                found.m_hSteamLeaderboard = i + 1;
                found.m_bLeaderboardFound = 1;
            }
        }
        return fake_steamworks::finish(found);
    }
    int GetLeaderboardEntryCount(SteamLeaderboard_t board) {
        return static_cast<int>(fake_steamworks::state.leaderboards[board - 1].scores.size());
    }
    SteamAPICall_t UploadLeaderboardScore(SteamLeaderboard_t board,
                                          ELeaderboardUploadScoreMethod method, int32 score,
                                          const int32* details, int detail_count) {
        (void)details;
        (void)detail_count;
        LeaderboardScoreUploaded_t uploaded;
        uploaded.m_hSteamLeaderboard = board;
        uploaded.m_nScore = score;
        if (!fake_steamworks::state.refuse_uploads &&
            method == k_ELeaderboardUploadScoreMethodKeepBest) {
            uploaded.m_bSuccess = 1;
            auto& scores = fake_steamworks::state.leaderboards[board - 1].scores;
            scores.emplace_back(fake_steamworks::state.player, score);
            std::stable_sort(scores.begin(), scores.end(),
                             [](const auto& a, const auto& b) { return a.second > b.second; });
        }
        return fake_steamworks::finish(uploaded);
    }
    SteamAPICall_t DownloadLeaderboardEntries(SteamLeaderboard_t board,
                                              ELeaderboardDataRequest request, int start, int end) {
        const auto& scores = fake_steamworks::state.leaderboards[board - 1].scores;
        std::vector<LeaderboardEntry_t> entries;
        for (std::size_t i = 0; i < scores.size(); ++i) {
            const auto rank = static_cast<int>(i) + 1;
            const uint64 id = scores[i].first;
            const bool you = id == fake_steamworks::state.player;
            const bool keep = request == k_ELeaderboardDataRequestGlobal
                                  ? rank >= start && rank <= end
                              : request == k_ELeaderboardDataRequestFriends
                                  ? you || fake_steamworks::state.players[id].friend_of_player
                                  : you;
            if (keep) {
                LeaderboardEntry_t entry;
                entry.m_steamIDUser = CSteamID(id);
                entry.m_nGlobalRank = rank;
                entry.m_nScore = scores[i].second;
                entries.push_back(entry);
            }
        }
        LeaderboardScoresDownloaded_t downloaded;
        downloaded.m_hSteamLeaderboard = board;
        downloaded.m_hSteamLeaderboardEntries = fake_steamworks::state.next_call + 1000;
        downloaded.m_cEntryCount = static_cast<int>(entries.size());
        fake_steamworks::state.downloads[downloaded.m_hSteamLeaderboardEntries] =
            std::move(entries);
        return fake_steamworks::finish(downloaded);
    }
    bool GetDownloadedLeaderboardEntry(SteamLeaderboardEntries_t entries, int index,
                                       LeaderboardEntry_t* entry, int32* details, int details_max) {
        (void)details;
        (void)details_max;
        const auto& list = fake_steamworks::state.downloads[entries];
        if (index < 0 || static_cast<std::size_t>(index) >= list.size()) {
            return false;
        }
        *entry = list[static_cast<std::size_t>(index)];
        return true;
    }
};

inline ISteamUser* SteamUser() {
    static ISteamUser user;
    return &user;
}
inline ISteamFriends* SteamFriends() {
    static ISteamFriends friends;
    return &friends;
}
inline ISteamUserStats* SteamUserStats() {
    static ISteamUserStats stats;
    return &stats;
}

inline bool SteamAPI_Init() {
    fake_steamworks::state.initialized = fake_steamworks::state.running;
    return fake_steamworks::state.initialized;
}
inline void SteamAPI_Shutdown() {
    fake_steamworks::state.initialized = false;
}
inline HSteamPipe SteamAPI_GetHSteamPipe() {
    return 1;
}
inline void SteamAPI_ManualDispatch_Init() {}
inline void SteamAPI_ManualDispatch_RunFrame(HSteamPipe pipe) {
    (void)pipe;
}
inline bool SteamAPI_ManualDispatch_GetNextCallback(HSteamPipe pipe, CallbackMsg_t* message) {
    (void)pipe;
    auto& callbacks = fake_steamworks::state.callbacks;
    if (callbacks.empty()) {
        return false;
    }
    fake_steamworks::state.last = std::move(callbacks.front().second);
    message->m_iCallback = callbacks.front().first;
    message->m_pubParam = fake_steamworks::state.last.data();
    message->m_cubParam = static_cast<int>(fake_steamworks::state.last.size());
    callbacks.pop_front();
    return true;
}
inline void SteamAPI_ManualDispatch_FreeLastCallback(HSteamPipe pipe) {
    (void)pipe;
}
inline bool SteamAPI_ManualDispatch_GetAPICallResult(HSteamPipe pipe, SteamAPICall_t call,
                                                     void* result, int size, int expected,
                                                     bool* failed) {
    (void)pipe;
    const auto it = fake_steamworks::state.results.find(call);
    if (it == fake_steamworks::state.results.end() || it->second.first != expected ||
        static_cast<std::size_t>(size) != it->second.second.size()) {
        *failed = true;
        return false;
    }
    std::memcpy(result, it->second.second.data(), it->second.second.size());
    *failed = false;
    fake_steamworks::state.results.erase(it);
    return true;
}
