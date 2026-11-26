// The steamworks package's games backend (export/sdks/steamworks/src/steamworks.cpp), compiled
// against tests/fake_steamworks, a stand-in for Valve's SDK that plays Steam.

#include "slash/services/services.hpp"

#include <doctest/doctest.h>

#include <memory>
#include <steam/steam_api.h>
#include <string>
#include <vector>

namespace slash_steamworks {
void register_backends(slash::services::Backends& backends);
} // namespace slash_steamworks

namespace services = slash::services;
using Kind = services::Report::Kind;

namespace {

constexpr std::uint64_t kRival = 76561197960287931ULL;
constexpr std::uint64_t kStranger = 76561197960287932ULL;

// The package's backend as a game starts it, with the reports of each tick.
struct Steam {
    std::shared_ptr<services::Reports> reports = std::make_shared<services::Reports>();
    services::Backends backends;

    Steam() {
        slash_steamworks::register_backends(backends);
        REQUIRE(backends.games != nullptr);
        backends.games->start(reports);
    }
    services::GamesBackend& games() const { return *backends.games; }
    std::vector<services::Report> tick() const {
        backends.games->poll();
        return reports->take();
    }
};

void play_steam() {
    fake_steamworks::reset();
    auto& state = fake_steamworks::state;
    state.players[state.player] = {state.player, "Player", false};
    state.players[kRival] = {kRival, "Rival", true};
    state.players[kStranger] = {kStranger, "Stranger", false};
}

} // namespace

TEST_CASE("steamworks: the Steam player, achievements and the overlay") {
    play_steam();
    auto& state = fake_steamworks::state;
    state.achievements = {{"FIRST_LAP", true}, {"TEN_LAPS", false}};
    {
        const Steam steam;
        CHECK(state.initialized);
        CHECK(steam.games().name() == "steam");
        steam.games().sign_in(false);
        std::vector<services::Report> reports = steam.tick();
        REQUIRE(reports.size() == 1);
        CHECK(reports[0].kind == Kind::SignedIn);
        CHECK(reports[0].player == services::Player{"76561197960287930", "Player"});

        // Achievements go by their names, which are their Steam API names.
        const std::vector<services::AchievementUnit> units{
            {"FIRST_LAP", "", 0}, {"TEN_LAPS", "", 10}, {"NO_SUCH", "", 0}};
        steam.games().load_achievements(units);
        reports = steam.tick();
        REQUIRE(reports.size() == 2);
        CHECK(reports[0].kind == Kind::AchievementFailed);
        CHECK(reports[0].name == "NO_SUCH");
        CHECK(reports[0].error.find("API name \"NO_SUCH\"") != std::string::npos);
        CHECK(reports[1].kind == Kind::AchievementsLoaded);
        CHECK(reports[1].achievements ==
              std::vector<services::AchievementProgress>{{"FIRST_LAP", 0, true}});

        // Steps show as progress until they reach the achievement's, which unlocks it.
        steam.games().set_steps(units[1], 4);
        steam.games().set_steps(units[1], 10);
        CHECK(state.stored == std::vector<std::string>{"TEN_LAPS 4/10", "TEN_LAPS"});
        CHECK(state.achievements["TEN_LAPS"]);
        steam.games().unlock(units[2]);
        reports = steam.tick();
        REQUIRE(reports.size() == 1);
        CHECK(reports[0].kind == Kind::AchievementFailed);

        // The overlay's screens close when the player closes the overlay.
        steam.games().show_achievements();
        steam.games().show_leaderboards(nullptr);
        reports = steam.tick();
        CHECK(state.overlays == std::vector<std::string>{"Achievements", "Stats"});
        REQUIRE_FALSE(reports.empty());
        CHECK(reports[0].kind == Kind::GamesUiClosed);
    }
    CHECK_FALSE(state.initialized);

    // Without Steam, every call fails, saying why.
    state.running = false;
    const Steam offline;
    offline.games().sign_in(true);
    offline.games().submit_score({"best_lap", "", services::SortOrder::HighToLow}, 1);
    const std::vector<services::Report> reports = offline.tick();
    REQUIRE(reports.size() == 2);
    CHECK(reports[0].kind == Kind::SignInFailed);
    CHECK(reports[0].error.find("Steam isn't running") != std::string::npos);
    CHECK(reports[1].kind == Kind::ScoreFailed);
}

TEST_CASE("steamworks: leaderboards submit and load through Steam's calls") {
    play_steam();
    auto& state = fake_steamworks::state;
    state.leaderboards = {{"best_lap", {{kStranger, 900}, {kRival, 800}}}};
    const Steam steam;
    const services::LeaderboardUnit best{"best_lap", "", services::SortOrder::HighToLow};

    // Finding the leaderboard and uploading finish a tick later, in Steam's call results.
    steam.games().submit_score(best, 850);
    std::vector<services::Report> reports = steam.tick();
    REQUIRE(reports.size() == 1);
    CHECK(reports[0].kind == Kind::ScoreSubmitted);
    CHECK(reports[0].name == "best_lap");
    CHECK(reports[0].score == 850);

    steam.games().load_scores(best, {services::TimeSpan::AllTime, services::Collection::Global, 2});
    reports = steam.tick();
    REQUIRE(reports.size() == 1);
    CHECK(reports[0].kind == Kind::ScoresLoaded);
    const services::Scores& top = reports[0].scores;
    REQUIRE(top.entries.size() == 2);
    CHECK(top.entries[0] == services::Score{1, std::to_string(kStranger), "Stranger", 900});
    CHECK(top.entries[1].name == "Player");
    REQUIRE(top.player.has_value());
    CHECK(top.player->rank == 2);
    CHECK(top.total == 3);

    // The player's own score comes with the best, even when it isn't among them.
    steam.games().load_scores(best, {services::TimeSpan::AllTime, services::Collection::Global, 1});
    reports = steam.tick();
    REQUIRE(reports.size() == 1);
    REQUIRE(reports[0].scores.entries.size() == 1);
    REQUIRE(reports[0].scores.player.has_value());
    CHECK(reports[0].scores.player->value == 850);

    steam.games().load_scores(best,
                              {services::TimeSpan::AllTime, services::Collection::Friends, 10});
    reports = steam.tick();
    REQUIRE(reports.size() == 1);
    std::vector<std::string> names;
    for (const services::Score& score : reports[0].scores.entries) {
        names.push_back(score.name + " " + std::to_string(score.rank));
    }
    CHECK(names == std::vector<std::string>{"Player 2", "Rival 3"});

    // What Steam can't do.
    steam.games().load_scores(best, {services::TimeSpan::Week, services::Collection::Global, 10});
    steam.games().submit_score(best, std::int64_t{1} << 40);
    steam.games().submit_score({"no_such", "", services::SortOrder::HighToLow}, 1);
    state.refuse_uploads = true;
    steam.games().submit_score(best, 990);
    reports = steam.tick();
    std::vector<std::string> errors;
    for (const services::Report& report : reports) {
        errors.push_back(report.name + ": " + report.error);
    }
    CHECK(errors ==
          std::vector<std::string>{
              "best_lap: Steam leaderboards keep all-time scores only",
              "best_lap: Steam leaderboards hold 32-bit scores",
              "no_such: Steam has no leaderboard with the API name \"no_such\"; give it the "
              "game's name in Steamworks",
              "best_lap: Steam refused the score"});
}
