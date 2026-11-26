#pragma once

// A stand-in for the GOG Galaxy SDK's GalaxyApi.h, so the galaxy package's glue code compiles and
// runs in the tests without GOG's SDK, which comes from the GOG Developer Portal. It declares only
// what the glue calls, with the names and shapes GOG's public documentation gives, and plays GOG
// Galaxy from fake_galaxy::state: the tests set the player, achievements, stats and leaderboards
// there, and read back what the glue did. Listeners are called from ProcessData, as GOG's are.

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace galaxy::api {

class GalaxyID {
public:
    GalaxyID() = default;
    GalaxyID(std::uint64_t value) : value_(value) {} // NOLINT(google-explicit-constructor)
    [[nodiscard]] std::uint64_t ToUint64() const { return value_; }
    [[nodiscard]] bool IsValid() const { return value_ != 0; }

private:
    std::uint64_t value_ = 0;
};

class InitOptions {
public:
    InitOptions(const char* _clientID, const char* _clientSecret, const char* _configFilePath = ".")
        : clientID(_clientID), clientSecret(_clientSecret), configFilePath(_configFilePath) {}
    const char* clientID;
    const char* clientSecret;
    const char* configFilePath;
};

class IError {
public:
    virtual ~IError() = default;
    [[nodiscard]] virtual const char* GetName() const = 0;
    [[nodiscard]] virtual const char* GetMsg() const = 0;
};

class IGalaxyListener {
public:
    virtual ~IGalaxyListener() = default;
};

class IAuthListener : public IGalaxyListener {
public:
    enum FailureReason {
        FAILURE_REASON_UNDEFINED,
        FAILURE_REASON_GALAXY_SERVICE_NOT_AVAILABLE,
        FAILURE_REASON_GALAXY_SERVICE_NOT_SIGNED_IN,
        FAILURE_REASON_CONNECTION_FAILURE,
        FAILURE_REASON_NO_LICENSE,
        FAILURE_REASON_INVALID_CREDENTIALS,
        FAILURE_REASON_GALAXY_NOT_INITIALIZED,
        FAILURE_REASON_EXTERNAL_SERVICE_FAILURE
    };
    virtual void OnAuthSuccess() = 0;
    virtual void OnAuthFailure(FailureReason failureReason) = 0;
    virtual void OnAuthLost() = 0;
};

class IUserStatsAndAchievementsRetrieveListener : public IGalaxyListener {
public:
    enum FailureReason { FAILURE_REASON_UNDEFINED, FAILURE_REASON_CONNECTION_FAILURE };
    virtual void OnUserStatsAndAchievementsRetrieveSuccess(GalaxyID userID) = 0;
    virtual void OnUserStatsAndAchievementsRetrieveFailure(GalaxyID userID,
                                                           FailureReason failureReason) = 0;
};

class IStatsAndAchievementsStoreListener : public IGalaxyListener {
public:
    enum FailureReason { FAILURE_REASON_UNDEFINED, FAILURE_REASON_CONNECTION_FAILURE };
    virtual void OnUserStatsAndAchievementsStoreSuccess() = 0;
    virtual void OnUserStatsAndAchievementsStoreFailure(FailureReason failureReason) = 0;
};

class ILeaderboardRetrieveListener : public IGalaxyListener {
public:
    enum FailureReason { FAILURE_REASON_UNDEFINED, FAILURE_REASON_CONNECTION_FAILURE };
    virtual void OnLeaderboardRetrieveSuccess(const char* name) = 0;
    virtual void OnLeaderboardRetrieveFailure(const char* name, FailureReason failureReason) = 0;
};

class ILeaderboardScoreUpdateListener : public IGalaxyListener {
public:
    enum FailureReason {
        FAILURE_REASON_UNDEFINED,
        FAILURE_REASON_NO_IMPROVEMENT,
        FAILURE_REASON_CONNECTION_FAILURE
    };
    virtual void OnLeaderboardScoreUpdateSuccess(const char* name, int32_t score, uint32_t oldRank,
                                                 uint32_t newRank) = 0;
    virtual void OnLeaderboardScoreUpdateFailure(const char* name, int32_t score,
                                                 FailureReason failureReason) = 0;
};

class ILeaderboardEntriesRetrieveListener : public IGalaxyListener {
public:
    enum FailureReason {
        FAILURE_REASON_UNDEFINED,
        FAILURE_REASON_NOT_FOUND,
        FAILURE_REASON_CONNECTION_FAILURE
    };
    virtual void OnLeaderboardEntriesRetrieveSuccess(const char* name, uint32_t entryCount) = 0;
    virtual void OnLeaderboardEntriesRetrieveFailure(const char* name,
                                                     FailureReason failureReason) = 0;
};

class IUser {
public:
    virtual ~IUser() = default;
    virtual void SignInGalaxy(bool requireOnline = false, uint32_t timeout = 15,
                              IAuthListener* const listener = nullptr) = 0;
    virtual bool SignedIn() = 0;
    virtual GalaxyID GetGalaxyID() = 0;
};

class IFriends {
public:
    virtual ~IFriends() = default;
    virtual const char* GetPersonaName() = 0;
    virtual const char* GetFriendPersonaName(GalaxyID userID) = 0;
};

class IStats {
public:
    virtual ~IStats() = default;
    virtual void RequestUserStatsAndAchievements(
        GalaxyID userID = GalaxyID(),
        IUserStatsAndAchievementsRetrieveListener* const listener = nullptr) = 0;
    virtual int32_t GetStatInt(const char* name, GalaxyID userID = GalaxyID()) = 0;
    virtual void SetStatInt(const char* name, int32_t value) = 0;
    virtual void GetAchievement(const char* name, bool& unlocked, uint32_t& unlockTime,
                                GalaxyID userID = GalaxyID()) = 0;
    virtual void SetAchievement(const char* name) = 0;
    virtual void
    StoreStatsAndAchievements(IStatsAndAchievementsStoreListener* const listener = nullptr) = 0;
    virtual void FindLeaderboard(const char* name,
                                 ILeaderboardRetrieveListener* const listener = nullptr) = 0;
    virtual void SetLeaderboardScore(const char* name, int32_t score, bool forceUpdate = false,
                                     ILeaderboardScoreUpdateListener* const listener = nullptr) = 0;
    virtual void RequestLeaderboardEntriesGlobal(
        const char* name, uint32_t rangeStart, uint32_t rangeEnd,
        ILeaderboardEntriesRetrieveListener* const listener = nullptr) = 0;
    virtual void RequestLeaderboardEntriesAroundUser(
        const char* name, uint32_t countBefore, uint32_t countAfter, GalaxyID userID = GalaxyID(),
        ILeaderboardEntriesRetrieveListener* const listener = nullptr) = 0;
    virtual void GetRequestedLeaderboardEntry(uint32_t index, uint32_t& rank, int32_t& score,
                                              GalaxyID& userID) = 0;
    virtual uint32_t GetLeaderboardEntryCount(const char* name) = 0;
};

} // namespace galaxy::api

namespace fake_galaxy {

namespace api = galaxy::api;

struct Entry {
    std::uint64_t user = 0;
    std::string name;
    std::int32_t score = 0;
};

// What the fake Galaxy holds. The tests set it up, then read back what the glue did.
struct State {
    // Set up by the tests.
    bool configured = true;     // Init works (the package's keys were set)
    bool galaxy_running = true; // the GOG Galaxy client runs, signed in
    std::uint64_t player = 46988745391538100ULL;
    std::string name = "Player";
    std::map<std::string, bool> achievements;               // unlocked, by API key
    std::map<std::string, std::int32_t> stats;              // by name
    std::map<std::string, std::vector<Entry>> leaderboards; // best first

    // Seen by the tests.
    bool initialized = false;
    std::vector<std::string> stored;  // what each StoreStatsAndAchievements stored
    std::vector<std::string> pending; // stats and achievements set since the last store

    // Inside the fake.
    std::deque<std::function<void()>> queue;
    std::vector<Entry> requested;
    std::string error;
    bool signed_in = false;
};

inline State state;

inline void reset() {
    state = State{};
}

class Error final : public api::IError {
public:
    [[nodiscard]] const char* GetName() const override { return "InvalidArgumentError"; }
    [[nodiscard]] const char* GetMsg() const override { return state.error.c_str(); }
};

inline Error error;

inline void later(std::function<void()> callback) {
    state.queue.push_back(std::move(callback));
}

class User final : public api::IUser {
public:
    void SignInGalaxy(bool, uint32_t, api::IAuthListener* const listener) override {
        state.error.clear();
        later([listener] {
            if (state.galaxy_running) {
                state.signed_in = true;
                listener->OnAuthSuccess();
            } else {
                listener->OnAuthFailure(
                    api::IAuthListener::FAILURE_REASON_GALAXY_SERVICE_NOT_AVAILABLE);
            }
        });
    }
    bool SignedIn() override { return state.signed_in; }
    api::GalaxyID GetGalaxyID() override { return {state.player}; }
};

class Friends final : public api::IFriends {
public:
    const char* GetPersonaName() override { return state.name.c_str(); }
    const char* GetFriendPersonaName(api::GalaxyID userID) override {
        for (const auto& [board, entries] : state.leaderboards) {
            for (const Entry& entry : entries) {
                if (entry.user == userID.ToUint64()) {
                    return entry.name.c_str();
                }
            }
        }
        return "";
    }
};

class Stats final : public api::IStats {
public:
    void RequestUserStatsAndAchievements(
        api::GalaxyID userID,
        api::IUserStatsAndAchievementsRetrieveListener* const listener) override {
        state.error.clear();
        later([userID, listener] { listener->OnUserStatsAndAchievementsRetrieveSuccess(userID); });
    }
    int32_t GetStatInt(const char* name, api::GalaxyID) override {
        const auto it = state.stats.find(name);
        state.error = it == state.stats.end() ? "no such stat" : "";
        return it == state.stats.end() ? 0 : it->second;
    }
    void SetStatInt(const char* name, int32_t value) override {
        const auto it = state.stats.find(name);
        state.error = it == state.stats.end() ? "no such stat" : "";
        if (it != state.stats.end()) {
            it->second = value;
            state.pending.push_back(std::string(name) + " " + std::to_string(value));
        }
    }
    void GetAchievement(const char* name, bool& unlocked, uint32_t& unlockTime,
                        api::GalaxyID) override {
        const auto it = state.achievements.find(name);
        state.error = it == state.achievements.end() ? "no such achievement" : "";
        unlocked = it != state.achievements.end() && it->second;
        unlockTime = unlocked ? 1764000000U : 0U;
    }
    void SetAchievement(const char* name) override {
        const auto it = state.achievements.find(name);
        state.error = it == state.achievements.end() ? "no such achievement" : "";
        if (it != state.achievements.end()) {
            it->second = true;
            state.pending.emplace_back(name);
        }
    }
    void
    StoreStatsAndAchievements(api::IStatsAndAchievementsStoreListener* const listener) override {
        state.error.clear();
        std::string stored;
        for (const std::string& item : state.pending) {
            stored += (stored.empty() ? "" : ", ") + item;
        }
        state.pending.clear();
        state.stored.push_back(stored);
        later([listener] { listener->OnUserStatsAndAchievementsStoreSuccess(); });
    }
    void FindLeaderboard(const char* name,
                         api::ILeaderboardRetrieveListener* const listener) override {
        state.error.clear();
        const std::string board = name;
        later([board, listener] {
            if (state.leaderboards.contains(board)) {
                listener->OnLeaderboardRetrieveSuccess(board.c_str());
            } else {
                listener->OnLeaderboardRetrieveFailure(
                    board.c_str(), api::ILeaderboardRetrieveListener::FAILURE_REASON_UNDEFINED);
            }
        });
    }
    void SetLeaderboardScore(const char* name, int32_t score, bool,
                             api::ILeaderboardScoreUpdateListener* const listener) override {
        state.error.clear();
        const std::string board = name;
        later([board, score, listener] {
            std::vector<Entry>& entries = state.leaderboards[board];
            for (Entry& entry : entries) {
                if (entry.user == state.player) {
                    if (entry.score >= score) {
                        listener->OnLeaderboardScoreUpdateFailure(
                            board.c_str(), score,
                            api::ILeaderboardScoreUpdateListener::FAILURE_REASON_NO_IMPROVEMENT);
                        return;
                    }
                    entry.score = score;
                    listener->OnLeaderboardScoreUpdateSuccess(board.c_str(), score, 0, 0);
                    return;
                }
            }
            entries.push_back({state.player, state.name, score});
            listener->OnLeaderboardScoreUpdateSuccess(board.c_str(), score, 0, 0);
        });
    }
    void RequestLeaderboardEntriesGlobal(
        const char* name, uint32_t rangeStart, uint32_t rangeEnd,
        api::ILeaderboardEntriesRetrieveListener* const listener) override {
        state.error.clear();
        const std::string board = name;
        later([board, rangeStart, rangeEnd, listener] {
            state.requested.clear();
            const std::vector<Entry>& entries = state.leaderboards[board];
            for (uint32_t i = rangeStart; i <= rangeEnd && i < entries.size(); ++i) {
                state.requested.push_back(entries[i]);
            }
            listener->OnLeaderboardEntriesRetrieveSuccess(
                board.c_str(), static_cast<uint32_t>(state.requested.size()));
        });
    }
    void RequestLeaderboardEntriesAroundUser(
        const char* name, uint32_t, uint32_t, api::GalaxyID,
        api::ILeaderboardEntriesRetrieveListener* const listener) override {
        state.error.clear();
        const std::string board = name;
        later([board, listener] {
            state.requested.clear();
            for (const Entry& entry : state.leaderboards[board]) {
                if (entry.user == state.player) {
                    state.requested.push_back(entry);
                }
            }
            if (state.requested.empty()) {
                listener->OnLeaderboardEntriesRetrieveFailure(
                    board.c_str(),
                    api::ILeaderboardEntriesRetrieveListener::FAILURE_REASON_NOT_FOUND);
            } else {
                listener->OnLeaderboardEntriesRetrieveSuccess(board.c_str(), 1);
            }
        });
    }
    void GetRequestedLeaderboardEntry(uint32_t index, uint32_t& rank, int32_t& score,
                                      api::GalaxyID& userID) override {
        state.error = index < state.requested.size() ? "" : "no such entry";
        if (index >= state.requested.size()) {
            return;
        }
        const Entry& entry = state.requested[index];
        // Ranks as the whole leaderboard has them.
        const std::vector<Entry>& all = state.leaderboards.begin()->second;
        rank = 0;
        for (std::size_t i = 0; i < all.size(); ++i) {
            if (all[i].user == entry.user) {
                rank = static_cast<uint32_t>(i + 1);
            }
        }
        score = entry.score;
        userID = api::GalaxyID(entry.user);
    }
    uint32_t GetLeaderboardEntryCount(const char* name) override {
        state.error.clear();
        return static_cast<uint32_t>(state.leaderboards[name].size());
    }
};

inline User user;
inline Friends friends;
inline Stats stats;

} // namespace fake_galaxy

namespace galaxy::api {

inline void Init(const InitOptions& initOptions) {
    (void)initOptions;
    fake_galaxy::state.error = fake_galaxy::state.configured ? "" : "no client id";
    fake_galaxy::state.initialized = fake_galaxy::state.error.empty();
}

inline void Shutdown() {
    fake_galaxy::state.initialized = false;
}

inline void ProcessData() {
    auto queue = std::move(fake_galaxy::state.queue);
    fake_galaxy::state.queue.clear();
    for (auto& callback : queue) {
        callback();
    }
}

inline const IError* GetError() {
    return fake_galaxy::state.error.empty() ? nullptr : &fake_galaxy::error;
}

inline IUser* User() {
    return &fake_galaxy::user;
}
inline IFriends* Friends() {
    return &fake_galaxy::friends;
}
inline IStats* Stats() {
    return &fake_galaxy::stats;
}

} // namespace galaxy::api
