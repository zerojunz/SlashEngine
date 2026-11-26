// The steamworks package's games backend: achievements, leaderboards and the overlay through
// Valve's Steamworks API, from the developer's own copy of the SDK. Calls finish through manual
// callback dispatch, polled each tick on the game's thread.

#include "slash/services/services.hpp"

#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <steam/steam_api.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slash_steamworks {
namespace {

namespace services = slash::services;

constexpr std::string_view kNotRunning =
    "Steam isn't running, or the game wasn't started from it; outside Steam, a steam_appid.txt "
    "with the app id beside the game lets it start";

// A Steam API call waiting for its result.
struct Pending {
    int callback = 0;
    std::size_t size = 0;
    std::function<void(const std::uint8_t* result, bool failed)> done;
};

class SteamGames final : public services::GamesBackend {
public:
    SteamGames() : running_(SteamAPI_Init()) {
        if (running_) {
            SteamAPI_ManualDispatch_Init();
            pipe_ = SteamAPI_GetHSteamPipe();
        }
    }
    ~SteamGames() override {
        if (running_) {
            SteamAPI_Shutdown();
        }
    }
    SteamGames(const SteamGames&) = delete;
    SteamGames& operator=(const SteamGames&) = delete;
    SteamGames(SteamGames&&) = delete;
    SteamGames& operator=(SteamGames&&) = delete;

    [[nodiscard]] std::string_view name() const override { return "steam"; }

    void start(const std::shared_ptr<services::Reports>& reports) override { reports_ = reports; }

    void poll() override {
        if (!running_) {
            return;
        }
        SteamAPI_ManualDispatch_RunFrame(pipe_);
        CallbackMsg_t message;
        while (SteamAPI_ManualDispatch_GetNextCallback(pipe_, &message)) {
            if (message.m_iCallback == SteamAPICallCompleted_t::k_iCallback) {
                SteamAPICallCompleted_t completed;
                std::memcpy(&completed, message.m_pubParam, sizeof(completed));
                finish(completed.m_hAsyncCall);
            } else if (message.m_iCallback == GameOverlayActivated_t::k_iCallback) {
                GameOverlayActivated_t overlay;
                std::memcpy(&overlay, message.m_pubParam, sizeof(overlay));
                if (overlay.m_bActive == 0 && showing_) {
                    showing_ = false;
                    reports_->games_ui_closed();
                }
            }
            SteamAPI_ManualDispatch_FreeLastCallback(pipe_);
        }
    }

    void sign_in(bool interactive) override {
        (void)interactive;
        if (!running_) {
            reports_->sign_in_failed(kNotRunning);
            return;
        }
        services::Player player;
        player.id = std::to_string(SteamUser()->GetSteamID().ConvertToUint64());
        player.name = SteamFriends()->GetPersonaName();
        reports_->signed_in(std::move(player));
    }

    // Steam's achievements are named by their API names, which are the game's names.
    void load_achievements(std::span<const services::AchievementUnit> units) override {
        if (!running_) {
            reports_->achievement_failed("", kNotRunning);
            return;
        }
        std::vector<services::AchievementProgress> progress;
        for (const services::AchievementUnit& unit : units) {
            bool achieved = false;
            if (!SteamUserStats()->GetAchievement(unit.achievement.c_str(), &achieved)) {
                reports_->achievement_failed(unit.achievement,
                                             missing("achievement", unit.achievement));
                continue;
            }
            if (achieved) {
                services::AchievementProgress done;
                done.achievement = unit.achievement;
                done.steps = unit.steps;
                done.unlocked = true;
                progress.push_back(std::move(done));
            }
        }
        reports_->achievements(std::move(progress));
    }

    void unlock(const services::AchievementUnit& unit) override {
        if (!running_) {
            reports_->achievement_failed(unit.achievement, kNotRunning);
            return;
        }
        if (!SteamUserStats()->SetAchievement(unit.achievement.c_str()) ||
            !SteamUserStats()->StoreStats()) {
            reports_->achievement_failed(unit.achievement,
                                         missing("achievement", unit.achievement));
        }
    }

    // Steam counts progress in stats of its own; the steps show as a progress notification until
    // they reach the achievement's, which unlocks it.
    void set_steps(const services::AchievementUnit& unit, int steps) override {
        if (steps >= unit.steps) {
            unlock(unit);
            return;
        }
        if (!running_) {
            reports_->achievement_failed(unit.achievement, kNotRunning);
            return;
        }
        if (!SteamUserStats()->IndicateAchievementProgress(
                unit.achievement.c_str(), static_cast<std::uint32_t>(steps),
                static_cast<std::uint32_t>(unit.steps))) {
            reports_->achievement_failed(unit.achievement,
                                         missing("achievement", unit.achievement));
        }
    }

    void submit_score(const services::LeaderboardUnit& unit, std::int64_t score) override {
        if (!running_) {
            reports_->score_failed(unit.leaderboard, kNotRunning);
            return;
        }
        if (score < std::numeric_limits<std::int32_t>::min() ||
            score > std::numeric_limits<std::int32_t>::max()) {
            reports_->score_failed(unit.leaderboard, "Steam leaderboards hold 32-bit scores");
            return;
        }
        const std::string leaderboard = unit.leaderboard;
        find(unit, [this, leaderboard, score](std::optional<SteamLeaderboard_t> board) {
            if (!board) {
                reports_->score_failed(leaderboard, missing("leaderboard", leaderboard));
                return;
            }
            expect<LeaderboardScoreUploaded_t>(
                SteamUserStats()->UploadLeaderboardScore(
                    *board, k_ELeaderboardUploadScoreMethodKeepBest,
                    static_cast<std::int32_t>(score), nullptr, 0),
                [this, leaderboard, score](const LeaderboardScoreUploaded_t& uploaded,
                                           bool failed) {
                    if (failed || uploaded.m_bSuccess == 0) {
                        reports_->score_failed(leaderboard, "Steam refused the score");
                    } else {
                        reports_->score_submitted(leaderboard, score);
                    }
                });
        });
    }

    void load_scores(const services::LeaderboardUnit& unit,
                     const services::ScoreQuery& query) override {
        if (!running_) {
            reports_->scores_failed(unit.leaderboard, kNotRunning);
            return;
        }
        if (query.time != services::TimeSpan::AllTime) {
            reports_->scores_failed(unit.leaderboard,
                                    "Steam leaderboards keep all-time scores only");
            return;
        }
        const std::string leaderboard = unit.leaderboard;
        find(unit, [this, leaderboard, query](std::optional<SteamLeaderboard_t> board) {
            if (!board) {
                reports_->scores_failed(leaderboard, missing("leaderboard", leaderboard));
                return;
            }
            const bool friends = query.collection == services::Collection::Friends;
            expect<LeaderboardScoresDownloaded_t>(
                SteamUserStats()->DownloadLeaderboardEntries(
                    *board,
                    friends ? k_ELeaderboardDataRequestFriends : k_ELeaderboardDataRequestGlobal, 1,
                    query.count),
                [this, leaderboard, query, board](const LeaderboardScoresDownloaded_t& downloaded,
                                                  bool failed) {
                    if (failed) {
                        reports_->scores_failed(leaderboard, "Steam couldn't load the scores");
                        return;
                    }
                    services::Scores scores;
                    scores.total = SteamUserStats()->GetLeaderboardEntryCount(*board);
                    read_entries(downloaded, query.count, scores);
                    if (scores.player || query.collection == services::Collection::Friends) {
                        reports_->scores(leaderboard, std::move(scores));
                        return;
                    }
                    // The player's own score, when it isn't among the best.
                    expect<LeaderboardScoresDownloaded_t>(
                        SteamUserStats()->DownloadLeaderboardEntries(
                            *board, k_ELeaderboardDataRequestGlobalAroundUser, 0, 0),
                        [this, leaderboard, scores](const LeaderboardScoresDownloaded_t& around,
                                                    bool around_failed) mutable {
                            if (!around_failed) {
                                services::Scores mine;
                                read_entries(around, 1, mine);
                                if (!mine.entries.empty()) {
                                    scores.player = mine.entries.front();
                                }
                            }
                            reports_->scores(leaderboard, std::move(scores));
                        });
                });
        });
    }

    void show_achievements() override { show("Achievements"); }

    void show_leaderboards(const services::LeaderboardUnit* unit) override {
        (void)unit;
        show("Stats");
    }

private:
    static std::string missing(std::string_view what, std::string_view name) {
        return "Steam has no " + std::string(what) + " with the API name \"" + std::string(name) +
               "\"; give it the game's name in Steamworks";
    }

    void show(const char* dialog) {
        if (!running_) {
            reports_->games_ui_closed();
            return;
        }
        showing_ = true;
        SteamFriends()->ActivateGameOverlay(dialog);
    }

    // Up to count entries of a download into scores, marking the player's.
    static void read_entries(const LeaderboardScoresDownloaded_t& downloaded, int count,
                             services::Scores& scores) {
        const std::uint64_t you = SteamUser()->GetSteamID().ConvertToUint64();
        for (int i = 0; i < downloaded.m_cEntryCount; ++i) {
            LeaderboardEntry_t entry;
            if (!SteamUserStats()->GetDownloadedLeaderboardEntry(
                    downloaded.m_hSteamLeaderboardEntries, i, &entry, nullptr, 0)) {
                continue;
            }
            services::Score score;
            score.rank = entry.m_nGlobalRank;
            score.player = std::to_string(entry.m_steamIDUser.ConvertToUint64());
            score.name = SteamFriends()->GetFriendPersonaName(entry.m_steamIDUser);
            score.value = entry.m_nScore;
            if (entry.m_steamIDUser.ConvertToUint64() == you) {
                scores.player = score;
            }
            if (static_cast<int>(scores.entries.size()) < count) {
                scores.entries.push_back(std::move(score));
            }
        }
    }

    // Finds a leaderboard by its API name, once, then hands its handle to then (or nothing when
    // Steam has none by that name).
    void find(const services::LeaderboardUnit& unit,
              std::function<void(std::optional<SteamLeaderboard_t>)> then) {
        if (const auto it = boards_.find(unit.leaderboard); it != boards_.end()) {
            then(it->second);
            return;
        }
        const std::string leaderboard = unit.leaderboard;
        expect<LeaderboardFindResult_t>(SteamUserStats()->FindLeaderboard(leaderboard.c_str()),
                                        [this, leaderboard, then = std::move(then)](
                                            const LeaderboardFindResult_t& found, bool failed) {
                                            if (failed || found.m_bLeaderboardFound == 0) {
                                                then(std::nullopt);
                                                return;
                                            }
                                            boards_[leaderboard] = found.m_hSteamLeaderboard;
                                            then(found.m_hSteamLeaderboard);
                                        });
    }

    // Calls done with call's result when Steam finishes it, or with failed set.
    template <typename Result>
    void expect(SteamAPICall_t call, std::function<void(const Result&, bool failed)> done) {
        auto handler = [done = std::move(done)](const std::uint8_t* data, bool failed) {
            Result result{};
            if (!failed) {
                std::memcpy(&result, data, sizeof(Result));
            }
            done(result, failed);
        };
        if (call == k_uAPICallInvalid) {
            handler(nullptr, true);
            return;
        }
        pending_[call] = Pending{Result::k_iCallback, sizeof(Result), std::move(handler)};
    }

    void finish(SteamAPICall_t call) {
        const auto it = pending_.find(call);
        if (it == pending_.end()) {
            return;
        }
        Pending pending = std::move(it->second);
        pending_.erase(it);
        std::vector<std::uint8_t> result(pending.size);
        bool failed = true;
        const bool got = SteamAPI_ManualDispatch_GetAPICallResult(
            pipe_, call, result.data(), static_cast<int>(result.size()), pending.callback, &failed);
        pending.done(result.data(), !got || failed);
    }

    bool running_ = false;
    HSteamPipe pipe_ = 0;
    bool showing_ = false;
    std::shared_ptr<services::Reports> reports_;
    std::map<SteamAPICall_t, Pending> pending_;
    std::map<std::string, SteamLeaderboard_t, std::less<>> boards_;
};

} // namespace

// The package's register function: Steam is the game's games backend.
void register_backends(slash::services::Backends& backends) {
    backends.games = std::make_unique<SteamGames>();
}

} // namespace slash_steamworks
