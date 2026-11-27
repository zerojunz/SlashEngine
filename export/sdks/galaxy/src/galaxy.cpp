// The galaxy package's games backend: the GOG player, achievements and leaderboards through the
// GOG Galaxy SDK, from the developer's own copy. Calls finish in listeners, which
// galaxy::api::ProcessData runs each tick on the game's thread.

#include "slash/services/services.hpp"

#include <cstdint>
#include <functional>
#include <galaxy/GalaxyApi.h>
#include <limits>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The game's Galaxy SDK credentials, from the GOG Developer Portal, given to the export in the
// environment (see the package's keys).
#ifndef SLASH_GALAXY_CLIENT_ID
#define SLASH_GALAXY_CLIENT_ID ""
#endif
#ifndef SLASH_GALAXY_CLIENT_SECRET
#define SLASH_GALAXY_CLIENT_SECRET ""
#endif

namespace slash_galaxy {
namespace {

namespace api = galaxy::api;
namespace services = slash::services;

constexpr std::string_view kNotRunning =
    "the GOG Galaxy SDK didn't start: export the game with GOG_GALAXY_CLIENT_ID and "
    "GOG_GALAXY_CLIENT_SECRET set, from the game's page in the GOG Developer Portal";
constexpr std::string_view kSignedOut = "sign the player in with GOG first";

// The error the SDK's last call left, as text; "" when it left none.
std::string last_error() {
    const auto* error = api::GetError();
    return error == nullptr ? std::string() : std::string(error->GetMsg());
}

// A listener the backend owns until its call is over.
class Listener {
public:
    Listener() = default;
    virtual ~Listener() = default;
    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;
    Listener(Listener&&) = delete;
    Listener& operator=(Listener&&) = delete;
    bool done = false;
};

class Auth final : public Listener, public api::IAuthListener {
public:
    explicit Auth(std::function<void(bool)> then) : then_(std::move(then)) {}
    void OnAuthSuccess() override { finish(true); }
    void OnAuthFailure(FailureReason failureReason) override {
        (void)failureReason;
        finish(false);
    }
    void OnAuthLost() override {}

private:
    void finish(bool ok) {
        if (!done) {
            done = true;
            then_(ok);
        }
    }
    std::function<void(bool)> then_;
};

class Retrieve final : public Listener, public api::IUserStatsAndAchievementsRetrieveListener {
public:
    explicit Retrieve(std::function<void(bool)> then) : then_(std::move(then)) {}
    void OnUserStatsAndAchievementsRetrieveSuccess(api::GalaxyID userID) override {
        (void)userID;
        done = true;
        then_(true);
    }
    void OnUserStatsAndAchievementsRetrieveFailure(api::GalaxyID userID,
                                                   FailureReason failureReason) override {
        (void)userID;
        (void)failureReason;
        done = true;
        then_(false);
    }

private:
    std::function<void(bool)> then_;
};

class Store final : public Listener, public api::IStatsAndAchievementsStoreListener {
public:
    explicit Store(std::function<void(bool)> then) : then_(std::move(then)) {}
    void OnUserStatsAndAchievementsStoreSuccess() override {
        done = true;
        then_(true);
    }
    void OnUserStatsAndAchievementsStoreFailure(FailureReason failureReason) override {
        (void)failureReason;
        done = true;
        then_(false);
    }

private:
    std::function<void(bool)> then_;
};

class Find final : public Listener, public api::ILeaderboardRetrieveListener {
public:
    explicit Find(std::function<void(bool)> then) : then_(std::move(then)) {}
    void OnLeaderboardRetrieveSuccess(const char* name) override {
        (void)name;
        done = true;
        then_(true);
    }
    void OnLeaderboardRetrieveFailure(const char* name, FailureReason failureReason) override {
        (void)name;
        (void)failureReason;
        done = true;
        then_(false);
    }

private:
    std::function<void(bool)> then_;
};

class Update final : public Listener, public api::ILeaderboardScoreUpdateListener {
public:
    explicit Update(std::function<void(bool)> then) : then_(std::move(then)) {}
    void OnLeaderboardScoreUpdateSuccess(const char* name, int32_t score, uint32_t oldRank,
                                         uint32_t newRank) override {
        (void)name;
        (void)score;
        (void)oldRank;
        (void)newRank;
        done = true;
        then_(true);
    }
    // A score no better than the player's best leaves the best, as Steam's KeepBest does.
    void OnLeaderboardScoreUpdateFailure(const char* name, int32_t score,
                                         FailureReason failureReason) override {
        (void)name;
        (void)score;
        done = true;
        then_(failureReason == FAILURE_REASON_NO_IMPROVEMENT);
    }

private:
    std::function<void(bool)> then_;
};

class Entries final : public Listener, public api::ILeaderboardEntriesRetrieveListener {
public:
    explicit Entries(std::function<void(bool, std::uint32_t)> then) : then_(std::move(then)) {}
    void OnLeaderboardEntriesRetrieveSuccess(const char* name, uint32_t entryCount) override {
        (void)name;
        done = true;
        then_(true, entryCount);
    }
    void OnLeaderboardEntriesRetrieveFailure(const char* name,
                                             FailureReason failureReason) override {
        (void)name;
        // A player with no score yet has no entry around them.
        done = true;
        then_(failureReason == FAILURE_REASON_NOT_FOUND, 0);
    }

private:
    std::function<void(bool, std::uint32_t)> then_;
};

class GogGames final : public services::GamesBackend {
public:
    GogGames() {
        api::Init(api::InitOptions(SLASH_GALAXY_CLIENT_ID, SLASH_GALAXY_CLIENT_SECRET));
        running_ = api::GetError() == nullptr;
    }
    ~GogGames() override {
        listeners_.clear();
        api::Shutdown();
    }
    GogGames(const GogGames&) = delete;
    GogGames& operator=(const GogGames&) = delete;
    GogGames(GogGames&&) = delete;
    GogGames& operator=(GogGames&&) = delete;

    [[nodiscard]] std::string_view name() const override { return "gog"; }

    void start(const std::shared_ptr<services::Reports>& reports) override { reports_ = reports; }

    void poll() override {
        if (!running_) {
            return;
        }
        api::ProcessData();
        std::erase_if(listeners_,
                      [](const std::unique_ptr<Listener>& listener) { return listener->done; });
    }

    // The GOG Galaxy client signs the player in: the game must be started from it, or with it
    // running and signed in.
    void sign_in(bool interactive) override {
        (void)interactive;
        if (!running_) {
            reports_->sign_in_failed(kNotRunning);
            return;
        }
        api::User()->SignInGalaxy(
            false, 15, own<Auth>([this](bool ok) {
                if (!ok) {
                    reports_->sign_in_failed("GOG couldn't sign the player in: start the game from "
                                             "GOG Galaxy, signed in");
                    return;
                }
                signed_in_ = true;
                services::Player player;
                player.id = std::to_string(api::User()->GetGalaxyID().ToUint64());
                player.name = api::Friends()->GetPersonaName();
                reports_->signed_in(std::move(player));
            }));
    }

    // GOG's achievements are named by their API keys, which are the game's names.
    void load_achievements(std::span<const services::AchievementUnit> units) override {
        if (!ready("", &services::Reports::achievement_failed)) {
            return;
        }
        std::vector<services::AchievementUnit> wanted(units.begin(), units.end());
        api::Stats()->RequestUserStatsAndAchievements(
            api::GalaxyID(), own<Retrieve>([this, wanted = std::move(wanted)](bool ok) {
                if (!ok) {
                    reports_->achievement_failed("", "GOG couldn't load the player's achievements");
                    return;
                }
                reports_->achievements(read_progress(wanted));
            }));
    }

    void unlock(const services::AchievementUnit& unit) override {
        if (!ready(unit.achievement, &services::Reports::achievement_failed)) {
            return;
        }
        api::Stats()->SetAchievement(unit.achievement.c_str());
        if (const std::string error = last_error(); !error.empty()) {
            reports_->achievement_failed(unit.achievement,
                                         missing("achievement", unit.achievement));
            return;
        }
        store(unit.achievement);
    }

    // GOG counts progress in a stat: the steps go to the stat named like the achievement, and
    // reaching the achievement's unlock it.
    void set_steps(const services::AchievementUnit& unit, int steps) override {
        if (!ready(unit.achievement, &services::Reports::achievement_failed)) {
            return;
        }
        api::Stats()->SetStatInt(unit.achievement.c_str(), steps);
        if (const std::string error = last_error(); !error.empty()) {
            reports_->achievement_failed(unit.achievement,
                                         "GOG has no stat \"" + unit.achievement +
                                             "\" counting the achievement's steps; add one named "
                                             "like it in the Developer Portal");
            return;
        }
        if (steps >= unit.steps) {
            unlock(unit);
        } else {
            store(unit.achievement);
        }
    }

    void submit_score(const services::LeaderboardUnit& unit, std::int64_t score) override {
        if (!ready(unit.leaderboard, &services::Reports::score_failed)) {
            return;
        }
        if (score < std::numeric_limits<std::int32_t>::min() ||
            score > std::numeric_limits<std::int32_t>::max()) {
            reports_->score_failed(unit.leaderboard, "GOG leaderboards hold 32-bit scores");
            return;
        }
        const std::string leaderboard = unit.leaderboard;
        find(leaderboard, [this, leaderboard, score](bool found) {
            if (!found) {
                reports_->score_failed(leaderboard, missing("leaderboard", leaderboard));
                return;
            }
            api::Stats()->SetLeaderboardScore(
                leaderboard.c_str(), static_cast<std::int32_t>(score), false,
                own<Update>([this, leaderboard, score](bool ok) {
                    if (ok) {
                        reports_->score_submitted(leaderboard, score);
                    } else {
                        reports_->score_failed(leaderboard, "GOG refused the score");
                    }
                }));
        });
    }

    void load_scores(const services::LeaderboardUnit& unit,
                     const services::ScoreQuery& query) override {
        if (!ready(unit.leaderboard, &services::Reports::scores_failed)) {
            return;
        }
        if (query.time != services::TimeSpan::AllTime) {
            reports_->scores_failed(unit.leaderboard, "GOG leaderboards keep all-time scores only");
            return;
        }
        if (query.collection != services::Collection::Global) {
            reports_->scores_failed(unit.leaderboard, "GOG leaderboards load global ranks only");
            return;
        }
        const std::string leaderboard = unit.leaderboard;
        const int count = query.count;
        find(leaderboard, [this, leaderboard, count](bool found) {
            if (!found) {
                reports_->scores_failed(leaderboard, missing("leaderboard", leaderboard));
                return;
            }
            api::Stats()->RequestLeaderboardEntriesGlobal(
                leaderboard.c_str(), 0, static_cast<std::uint32_t>(count - 1),
                own<Entries>([this, leaderboard](bool ok, std::uint32_t loaded) {
                    if (!ok) {
                        reports_->scores_failed(leaderboard, "GOG couldn't load the scores");
                        return;
                    }
                    services::Scores scores;
                    scores.total = api::Stats()->GetLeaderboardEntryCount(leaderboard.c_str());
                    scores.entries = read_entries(loaded);
                    load_own(leaderboard, std::move(scores));
                }));
        });
    }

    // GOG's overlay has no achievements or leaderboards screens: games draw their own.
    void show_achievements() override { reports_->games_ui_closed(); }

    void show_leaderboards(const services::LeaderboardUnit* unit) override {
        (void)unit;
        reports_->games_ui_closed();
    }

private:
    template <typename Kind, typename Then>
    Kind* own(Then then) {
        auto listener = std::make_unique<Kind>(std::move(then));
        Kind* raw = listener.get();
        listeners_.push_back(std::move(listener));
        return raw;
    }

    bool ready(std::string_view name,
               void (services::Reports::*failed)(std::string_view, std::string_view)) {
        if (!running_) {
            ((*reports_).*failed)(name, kNotRunning);
            return false;
        }
        if (!signed_in_) {
            ((*reports_).*failed)(name, kSignedOut);
            return false;
        }
        return true;
    }

    static std::string missing(std::string_view what, std::string_view name) {
        return "GOG has no " + std::string(what) + " with the API key \"" + std::string(name) +
               "\"; give it the game's name in the Developer Portal";
    }

    void store(const std::string& achievement) {
        api::Stats()->StoreStatsAndAchievements(own<Store>([this, achievement](bool ok) {
            if (!ok) {
                reports_->achievement_failed(achievement, "GOG couldn't store the progress");
            }
        }));
    }

    std::vector<services::AchievementProgress>
    read_progress(const std::vector<services::AchievementUnit>& wanted) {
        std::vector<services::AchievementProgress> progress;
        for (const services::AchievementUnit& unit : wanted) {
            bool unlocked = false;
            std::uint32_t when = 0;
            api::Stats()->GetAchievement(unit.achievement.c_str(), unlocked, when);
            if (!last_error().empty()) {
                reports_->achievement_failed(unit.achievement,
                                             missing("achievement", unit.achievement));
                continue;
            }
            services::AchievementProgress done;
            done.achievement = unit.achievement;
            done.unlocked = unlocked;
            if (unlocked) {
                done.steps = unit.steps;
            } else if (unit.steps > 0) {
                const std::int32_t steps = api::Stats()->GetStatInt(unit.achievement.c_str());
                done.steps = last_error().empty() ? steps : 0;
            }
            if (done.unlocked || done.steps > 0) {
                progress.push_back(std::move(done));
            }
        }
        return progress;
    }

    // The entries the last request loaded.
    static std::vector<services::Score> read_entries(std::uint32_t loaded) {
        std::vector<services::Score> entries;
        for (std::uint32_t i = 0; i < loaded; ++i) {
            std::uint32_t rank = 0;
            std::int32_t value = 0;
            api::GalaxyID user;
            api::Stats()->GetRequestedLeaderboardEntry(i, rank, value, user);
            if (!last_error().empty()) {
                continue;
            }
            services::Score score;
            score.rank = rank;
            score.player = std::to_string(user.ToUint64());
            const char* name = api::Friends()->GetFriendPersonaName(user);
            score.name = name == nullptr ? "" : name;
            score.value = value;
            entries.push_back(std::move(score));
        }
        return entries;
    }

    // The player's own entry, then the scores.
    void load_own(const std::string& leaderboard, services::Scores scores) {
        const std::string you = std::to_string(api::User()->GetGalaxyID().ToUint64());
        for (const services::Score& score : scores.entries) {
            if (score.player == you) {
                scores.player = score;
                reports_->scores(leaderboard, std::move(scores));
                return;
            }
        }
        api::Stats()->RequestLeaderboardEntriesAroundUser(
            leaderboard.c_str(), 0, 0, api::GalaxyID(),
            own<Entries>([this, leaderboard,
                          scores = std::move(scores)](bool ok, std::uint32_t loaded) mutable {
                if (ok && loaded > 0) {
                    std::vector<services::Score> mine = read_entries(loaded);
                    if (!mine.empty()) {
                        scores.player = mine.front();
                    }
                }
                reports_->scores(leaderboard, std::move(scores));
            }));
    }

    // Finds a leaderboard once, then calls then with whether GOG has it.
    void find(const std::string& leaderboard, std::function<void(bool)> then) {
        if (found_.contains(leaderboard)) {
            then(true);
            return;
        }
        api::Stats()->FindLeaderboard(
            leaderboard.c_str(), own<Find>([this, leaderboard, then = std::move(then)](bool ok) {
                if (ok) {
                    found_.insert(leaderboard);
                }
                then(ok);
            }));
    }

    bool running_ = false;
    bool signed_in_ = false;
    std::shared_ptr<services::Reports> reports_;
    std::vector<std::unique_ptr<Listener>> listeners_;
    std::set<std::string, std::less<>> found_;
};

} // namespace

// The package's register function: GOG Galaxy is the game's games backend.
void register_backends(slash::services::Backends& backends) {
    backends.games = std::make_unique<GogGames>();
}

} // namespace slash_galaxy
