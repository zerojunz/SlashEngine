// HTTP from game code (slash/net/http.hpp): a game that reads its leaderboard from a web API and
// posts the player's score. Requests go out without blocking the tick; each reply comes back to
// its callback at the start of a later tick, on the world's thread, so the callback can touch the
// world freely.
//
// This program runs that game in a session as an agent does, with no server: net.http_fake
// answers the game's requests with canned replies, step runs the ticks, and net.http lists what
// the game asked and what came back. An exported game sends the same requests over the network.

#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/net/http.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <cstdio>
#include <string>
#include <string_view>

namespace {

namespace command = slash::command;
namespace json = slash::json;
namespace net = slash::net;
namespace world = slash::world;

constexpr std::string_view kApi = "https://scores.example.com/v1";

// The game's own state: whether it asked yet, and what it shows.
struct Board {
    bool asked = false;
    bool posted = false;
    std::string shown;
};
Board board;

// Reads the leaderboard once, then posts the player's score when it beats the last entry.
void leaderboard(world::World& game_world, double /*tick_seconds*/) {
    if (board.asked) {
        return;
    }
    board.asked = true;
    net::http_get(
        game_world, std::string(kApi) + "/scores?top=3",
        [](world::World& in_world, const net::HttpReply& reply) {
            if (!reply.ok()) {
                board.shown =
                    "leaderboard unavailable: " +
                    (reply.error.empty() ? "HTTP " + std::to_string(reply.status) : reply.error);
                return;
            }
            const auto scores = json::parse(reply.body);
            if (!scores || !scores.value().is_array()) {
                board.shown = "leaderboard unreadable";
                return;
            }
            board.shown.clear();
            for (const json::Value& entry : scores.value().as_array()) {
                board.shown += entry.find("name")->as_string() + " " +
                               std::to_string(entry.find("score")->as_int()) + "; ";
            }
            const std::int64_t lowest = scores.value().as_array().back().find("score")->as_int();
            constexpr std::int64_t kPlayerScore = 4200;
            if (kPlayerScore > lowest && !board.posted) {
                board.posted = true;
                json::Value score;
                score["name"] = "Player";
                score["score"] = kPlayerScore;
                net::http_post_json(in_world, std::string(kApi) + "/scores", score,
                                    [](world::World& /*world*/, const net::HttpReply& posted) {
                                        board.shown +=
                                            "posted: HTTP " + std::to_string(posted.status);
                                    });
            }
        });
}

// Prints the call as an agent would make it, then the reply.
void call(command::Session& session, std::string_view name, std::string_view params) {
    const json::Value value = json::parse(params).value();
    std::printf("%.*s %s\n", static_cast<int>(name.size()), name.data(),
                json::write(value, json::Style::Compact).c_str());
    const command::Reply reply = session.call(name, value);
    std::puts(json::write(reply.ok ? reply.value : json::Value(reply.message), json::Style::Compact)
                  .c_str());
}

} // namespace

int main() {
    world::ComponentRegistry components;
    world::register_builtin_components(components);
    command::Registry commands;
    command::register_builtin_commands(commands);
    command::SessionOptions options;
    options.project_root = ".";
    options.capture_logs = false;
    command::Session session(components, commands, options);
    session.world().add_system("leaderboard", leaderboard);
    call(session, "scene.open", R"({"path": "main.scene.json"})");

    // The agent stands in for the score server: the top three, and a 201 for a new score.
    call(session, "net.http_fake", R"({"url_prefix": "https://scores.example.com/v1/scores",
        "method": "GET", "body": [{"name": "Ada", "score": 9100},
        {"name": "Grace", "score": 5600}, {"name": "Linus", "score": 3900}]})");
    call(session, "net.http_fake", R"({"url_prefix": "https://scores.example.com/v1/scores",
        "method": "POST", "status": 201, "body": {"rank": 3}})");

    // Tick 0 asks; tick 1 hands the reply to the game, which posts; tick 2 hands that back.
    call(session, "step", R"({"ticks": 3})");
    std::printf("shown: %s\n", board.shown.c_str());
    call(session, "net.http", "{}");

    // The server going down is something the game must handle too.
    board = {};
    call(session, "net.http_fake",
         R"({"clear": true, "url_prefix": "https://scores.example.com/", "status": 503})");
    call(session, "step", R"({"ticks": 2})");
    std::printf("shown: %s\n", board.shown.c_str());
    return 0;
}
