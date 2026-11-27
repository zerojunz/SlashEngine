// The galaxy package's games backend (export/sdks/galaxy/src/galaxy.cpp), compiled against
// tests/fake_galaxy, a stand-in for GOG's SDK that plays GOG Galaxy.

#include "slash/services/services.hpp"

#include <doctest/doctest.h>

#include <galaxy/GalaxyApi.h>
#include <memory>
#include <string>
#include <vector>

namespace slash_galaxy {
void register_backends(slash::services::Backends& backends);
} // namespace slash_galaxy

namespace services = slash::services;
using Kind = services::Report::Kind;

namespace {

constexpr std::uint64_t kRival = 46988745391538101ULL;
constexpr std::uint64_t kStranger = 46988745391538102ULL;

// The package's backend as a game starts it, with the reports of each tick.
struct Gog {
    std::shared_ptr<services::Reports> reports = std::make_shared<services::Reports>();
    services::Backends backends;

    Gog() {
        slash_galaxy::register_backends(backends);
        REQUIRE(backends.games != nullptr);
        backends.games->start(reports);
    }
    services::GamesBackend& games() const { return *backends.games; }
    // Ticks until Galaxy has nothing left to call back, as frames would.
    std::vector<services::Report> tick() const {
        for (int i = 0; i < 8; ++i) {
            backends.games->poll();
        }
        return reports->take();
    }
};

} // namespace

TEST_CASE("galaxy: the GOG player and achievements") {
    fake_galaxy::reset();
    auto& state = fake_galaxy::state;
    state.achievements = {{"FIRST_LAP", true}, {"TEN_LAPS", false}};
    state.stats = {{"TEN_LAPS", 3}};
    {
        const Gog gog;
        CHECK(state.initialized);
        CHECK(gog.games().name() == "gog");
        gog.games().unlock({"FIRST_LAP", "", 0});
        std::vector<services::Report> reports = gog.tick();
        REQUIRE(reports.size() == 1);
        CHECK(reports[0].error == "sign the player in with GOG first");

        gog.games().sign_in(false);
        reports = gog.tick();
        REQUIRE(reports.size() == 1);
        CHECK(reports[0].kind == Kind::SignedIn);
        CHECK(reports[0].player == services::Player{"46988745391538100", "Player"});

        // Achievements go by their API keys; progress comes from the stat named like them.
        const std::vector<services::AchievementUnit> units{
            {"FIRST_LAP", "", 0}, {"TEN_LAPS", "", 10}, {"NO_SUCH", "", 0}};
        gog.games().load_achievements(units);
        reports = gog.tick();
        REQUIRE(reports.size() == 2);
        CHECK(reports[0].kind == Kind::AchievementFailed);
        CHECK(reports[0].error.find("API key \"NO_SUCH\"") != std::string::npos);
        CHECK(reports[1].kind == Kind::AchievementsLoaded);
        CHECK(reports[1].achievements == std::vector<services::AchievementProgress>{
                                             {"FIRST_LAP", 0, true}, {"TEN_LAPS", 3, false}});

        // Steps are stored in the stat, and all of them unlock the achievement.
        gog.games().set_steps(units[1], 4);
        gog.games().set_steps(units[1], 10);
        CHECK(gog.tick().empty());
        CHECK(state.stored == std::vector<std::string>{"TEN_LAPS 4", "TEN_LAPS 10, TEN_LAPS"});
        CHECK(state.achievements["TEN_LAPS"]);
        gog.games().unlock(units[2]);
        gog.games().set_steps({"NO_STAT", "", 5}, 1);
        reports = gog.tick();
        REQUIRE(reports.size() == 2);
        CHECK(reports[0].kind == Kind::AchievementFailed);
        CHECK(reports[1].error.find("no stat \"NO_STAT\"") != std::string::npos);

        gog.games().show_leaderboards(nullptr);
        reports = gog.tick();
        REQUIRE(reports.size() == 1);
        CHECK(reports[0].kind == Kind::GamesUiClosed);
    }
    CHECK_FALSE(state.initialized);

    // Without GOG Galaxy running, signing in fails; exported without the game's credentials,
    // the SDK doesn't start and every call says why.
    fake_galaxy::reset();
    state.galaxy_running = false;
    {
        const Gog gog;
        gog.games().sign_in(true);
        const std::vector<services::Report> reports = gog.tick();
        REQUIRE(reports.size() == 1);
        CHECK(reports[0].kind == Kind::SignInFailed);
        CHECK(reports[0].error.find("start the game from GOG Galaxy") != std::string::npos);
    }
    fake_galaxy::reset();
    state.configured = false;
    const Gog offline;
    offline.games().sign_in(true);
    offline.games().submit_score({"best_lap", "", services::SortOrder::HighToLow}, 1);
    const std::vector<services::Report> reports = offline.tick();
    REQUIRE(reports.size() == 2);
    CHECK(reports[0].error.find("GOG_GALAXY_CLIENT_ID") != std::string::npos);
    CHECK(reports[1].kind == Kind::ScoreFailed);
}

TEST_CASE("galaxy: leaderboards submit and load through GOG's listeners") {
    fake_galaxy::reset();
    auto& state = fake_galaxy::state;
    state.leaderboards = {{"best_lap", {{kStranger, "Stranger", 900}, {kRival, "Rival", 800}}}};
    const Gog gog;
    gog.games().sign_in(false);
    REQUIRE(gog.tick().size() == 1);
    const services::LeaderboardUnit best{"best_lap", "", services::SortOrder::HighToLow};

    // Finding the leaderboard and setting the score finish in later ticks; a score no better
    // than the best kept counts as submitted.
    gog.games().submit_score(best, 850);
    std::vector<services::Report> reports = gog.tick();
    REQUIRE(reports.size() == 1);
    CHECK(reports[0].kind == Kind::ScoreSubmitted);
    CHECK(reports[0].score == 850);
    gog.games().submit_score(best, 10);
    reports = gog.tick();
    REQUIRE(reports.size() == 1);
    CHECK(reports[0].kind == Kind::ScoreSubmitted);

    // The best, with the player's own even when it isn't among them.
    gog.games().load_scores(best, {services::TimeSpan::AllTime, services::Collection::Global, 1});
    reports = gog.tick();
    REQUIRE(reports.size() == 1);
    CHECK(reports[0].kind == Kind::ScoresLoaded);
    const services::Scores& top = reports[0].scores;
    REQUIRE(top.entries.size() == 1);
    CHECK(top.entries[0] == services::Score{1, std::to_string(kStranger), "Stranger", 900});
    REQUIRE(top.player.has_value());
    CHECK(top.player->value == 850);
    CHECK(top.total == 3);

    // What GOG can't do.
    gog.games().load_scores(best, {services::TimeSpan::Week, services::Collection::Global, 10});
    gog.games().load_scores(best, {services::TimeSpan::AllTime, services::Collection::Friends, 10});
    gog.games().submit_score(best, std::int64_t{1} << 40);
    gog.games().submit_score({"no_such", "", services::SortOrder::HighToLow}, 1);
    reports = gog.tick();
    std::vector<std::string> errors;
    for (const services::Report& report : reports) {
        errors.push_back(report.name + ": " + report.error);
    }
    CHECK(errors == std::vector<std::string>{
                        "best_lap: GOG leaderboards keep all-time scores only",
                        "best_lap: GOG leaderboards load global ranks only",
                        "best_lap: GOG leaderboards hold 32-bit scores",
                        "no_such: GOG has no leaderboard with the API key \"no_such\"; give it "
                        "the game's name in the Developer Portal"});
}
