// The eos package's games backend (export/sdks/eos/src/eos.cpp), compiled against
// tests/fake_eos, a stand-in for Epic's SDK that plays Epic Online Services.

#include "slash/services/services.hpp"

#include <doctest/doctest.h>

#include <eos_sdk.h>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace slash_eos {
void register_backends(slash::services::Backends& backends);
std::string exchange_code(std::span<const std::string> args);
} // namespace slash_eos

namespace services = slash::services;
using Kind = services::Report::Kind;

namespace {

// The package's backend as a game starts it, with the reports of each tick.
struct Epic {
    std::shared_ptr<services::Reports> reports = std::make_shared<services::Reports>();
    services::Backends backends;

    Epic() {
        slash_eos::register_backends(backends);
        REQUIRE(backends.games != nullptr);
        backends.games->start(reports);
    }
    services::GamesBackend& games() const { return *backends.games; }
    // Ticks until EOS has nothing left to call back, as frames would.
    std::vector<services::Report> tick() const {
        for (int i = 0; i < 8; ++i) {
            backends.games->poll();
        }
        return reports->take();
    }
};

} // namespace

TEST_CASE("eos: the launcher's exchange code signs the player in") {
    const std::vector<std::string> launched{
        "Game.exe",      "-AUTH_LOGIN=unused", "-AUTH_PASSWORD=code123", "-AUTH_TYPE=exchangecode",
        "-epicapp=game", "-EpicPortal"};
    CHECK(slash_eos::exchange_code(launched) == "code123");
    const std::vector<std::string> other{"Game.exe", "-AUTH_PASSWORD=code123",
                                         "-AUTH_TYPE=password"};
    CHECK(slash_eos::exchange_code(other).empty());
    CHECK(slash_eos::exchange_code(std::vector<std::string>{"Game.exe"}).empty());
}

TEST_CASE("eos: the Epic player, achievements and screens") {
    fake_eos::reset();
    auto& state = fake_eos::state;
    state.achievements = {{"FIRST_LAP", {0, true}}, {"TEN_LAPS", {0.3, false}}};
    state.stats = {{"TEN_LAPS", 3}};
    {
        const Epic epic;
        CHECK(state.platform);
        CHECK(epic.games().name() == "epic");

        // Calls before signing in say so.
        epic.games().unlock({"FIRST_LAP", "", 0});
        std::vector<services::Report> reports = epic.tick();
        REQUIRE(reports.size() == 1);
        CHECK(reports[0].error == "sign the player in with Epic first");

        // Outside the launcher, the login EOS kept signs the player in to the game's services.
        epic.games().sign_in(false);
        reports = epic.tick();
        REQUIRE(reports.size() == 1);
        CHECK(reports[0].kind == Kind::SignedIn);
        CHECK(reports[0].player == services::Player{"0002a4f1c8e7b5d3", "Player"});
        CHECK(state.logins == std::vector<std::string>{"PersistentAuth", "Connect id-token"});

        // Achievements go by their ids; progress shows as steps.
        const std::vector<services::AchievementUnit> units{
            {"FIRST_LAP", "", 0}, {"TEN_LAPS", "", 10}, {"NO_SUCH", "", 0}};
        epic.games().load_achievements(units);
        reports = epic.tick();
        REQUIRE(reports.size() == 2);
        CHECK(reports[0].kind == Kind::AchievementFailed);
        CHECK(reports[0].error.find("id \"NO_SUCH\"") != std::string::npos);
        CHECK(reports[1].kind == Kind::AchievementsLoaded);
        CHECK(reports[1].achievements == std::vector<services::AchievementProgress>{
                                             {"FIRST_LAP", 0, true}, {"TEN_LAPS", 3, false}});

        // Steps go to the stat named like the achievement, and all of them unlock it.
        epic.games().set_steps(units[1], 4);
        epic.games().set_steps(units[1], 10);
        CHECK(epic.tick().empty());
        CHECK(state.ingested == std::vector<std::string>{"TEN_LAPS 4", "TEN_LAPS 10"});
        CHECK(state.achievements["TEN_LAPS"].unlocked);
        epic.games().unlock(units[2]);
        epic.games().set_steps({"NO_STAT", "", 5}, 1);
        reports = epic.tick();
        REQUIRE(reports.size() == 2);
        CHECK(reports[0].kind == Kind::AchievementFailed);
        CHECK(reports[1].error.find("no stat \"NO_STAT\"") != std::string::npos);

        // EOS has no screens of its own, so they close at once.
        epic.games().show_achievements();
        reports = epic.tick();
        REQUIRE(reports.size() == 1);
        CHECK(reports[0].kind == Kind::GamesUiClosed);
    }
    CHECK_FALSE(state.platform);
    CHECK_FALSE(state.initialized);
}

TEST_CASE("eos: signing in a new player, through the portal") {
    fake_eos::reset();
    auto& state = fake_eos::state;
    state.kept_login = false;
    state.new_player = true;
    {
        const Epic epic;
        // Without a kept login, a quiet sign-in fails; asked by the game, Epic's portal opens,
        // and a player new to the game gets a product user.
        epic.games().sign_in(false);
        std::vector<services::Report> reports = epic.tick();
        REQUIRE(reports.size() == 1);
        CHECK(reports[0].kind == Kind::SignInFailed);
        CHECK(reports[0].error.find("EOS_InvalidAuth") != std::string::npos);
        epic.games().sign_in(true);
        reports = epic.tick();
        REQUIRE(reports.size() == 1);
        CHECK(reports[0].kind == Kind::SignedIn);
        CHECK(state.logins == std::vector<std::string>{"PersistentAuth", "PersistentAuth",
                                                       "AccountPortal", "Connect id-token",
                                                       "CreateUser"});
    }

    // Exported without the game's ids, EOS doesn't start, and every call says why.
    fake_eos::reset();
    state.configured = false;
    const Epic offline;
    offline.games().sign_in(true);
    offline.games().submit_score({"best_lap", "", services::SortOrder::HighToLow}, 1);
    const std::vector<services::Report> reports = offline.tick();
    REQUIRE(reports.size() == 2);
    CHECK(reports[0].kind == Kind::SignInFailed);
    CHECK(reports[0].error.find("EOS_PRODUCT_ID") != std::string::npos);
    CHECK(reports[1].kind == Kind::ScoreFailed);
}

TEST_CASE("eos: leaderboards rank the stats scores go to") {
    fake_eos::reset();
    auto& state = fake_eos::state;
    state.stats = {{"best_lap", 0}};
    state.leaderboards = {{"best_lap",
                           {{"00000000rival", "Rival", 900},
                            {"0002a4f1c8e7b5d3", "Player", 850},
                            {"0000stranger", "Stranger", 800}}}};
    const Epic epic;
    epic.games().sign_in(false);
    REQUIRE(epic.tick().size() == 1);
    const services::LeaderboardUnit best{"best_lap", "", services::SortOrder::HighToLow};

    epic.games().submit_score(best, 850);
    std::vector<services::Report> reports = epic.tick();
    REQUIRE(reports.size() == 1);
    CHECK(reports[0].kind == Kind::ScoreSubmitted);
    CHECK(reports[0].score == 850);
    CHECK(state.stats["best_lap"] == 850);

    // The best, with the player's own even when it isn't among them.
    epic.games().load_scores(best, {services::TimeSpan::AllTime, services::Collection::Global, 1});
    reports = epic.tick();
    REQUIRE(reports.size() == 1);
    CHECK(reports[0].kind == Kind::ScoresLoaded);
    const services::Scores& top = reports[0].scores;
    REQUIRE(top.entries.size() == 1);
    CHECK(top.entries[0] == services::Score{1, "00000000rival", "Rival", 900});
    REQUIRE(top.player.has_value());
    CHECK(top.player->rank == 2);
    CHECK(top.total == 3);

    // What Epic can't do.
    epic.games().load_scores(best, {services::TimeSpan::Week, services::Collection::Global, 10});
    epic.games().load_scores(best,
                             {services::TimeSpan::AllTime, services::Collection::Friends, 10});
    epic.games().submit_score(best, std::int64_t{1} << 40);
    epic.games().submit_score({"no_such", "", services::SortOrder::HighToLow}, 1);
    epic.games().load_scores({"no_such", "", services::SortOrder::HighToLow}, {});
    reports = epic.tick();
    std::vector<std::string> errors;
    for (const services::Report& report : reports) {
        errors.push_back(report.name + ": " + report.error);
    }
    CHECK(errors ==
          std::vector<std::string>{
              "best_lap: Epic leaderboards keep the scores of the time window set in the Portal; "
              "load all_time",
              "best_lap: Epic leaderboards load global ranks only",
              "best_lap: Epic stats hold 32-bit scores",
              "no_such: Epic has no stat \"no_such\" for the leaderboard (EOS_NotFound); rank the "
              "leaderboard by a stat named like it in the Portal",
              "no_such: Epic has no leaderboard with the id \"no_such\" (EOS_NotFound); give it "
              "the game's name in the Portal"});
}
