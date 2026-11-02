// Autopilot playtests: Autopilot (github.com/ramenvr/autopilot) is an agent that plays a game to
// explore it for bugs, check a claim about it or reach a goal in it. Its agent service listens
// on a WebSocket and engine plugins connect out to it; slash::autopilot::Bridge is SlashEngine's,
// and `slash-cli run --autopilot-run verify --autopilot-prompt "..."` plays a project with it.
//
// The game is a title screen's Start button, then a walker who picks up coins with a HUD that
// counts them out of 3, though the level has 4. The agent service here is a script on an
// in-process connection and a clock of its own, so the run is the same every time: it reads the
// game's context, clicks Start, holds D to walk, reads the screen, gets the bridge's finding
// about the HUD and sends its verdict, which the bridge writes as the run's report and turns
// into the exit code.

#include "slash/autopilot/bridge.hpp"
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/input/input.hpp"
#include "slash/platform/file.hpp"
#include "slash/ui/ui.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace autopilot = slash::autopilot;
namespace command = slash::command;
namespace json = slash::json;
namespace world = slash::world;

constexpr double kTick = 1.0 / 60.0;
constexpr float kWalkSpeed = 120.0F; // pixels per second

std::string compact(const json::Value& value) {
    return json::write(value, json::Style::Compact);
}

// The game: Start hides the menu and lets the walker walk; walking past a coin takes it.
void add_systems(world::World& w) {
    w.add_system("coin_walk",
                 [](world::World& at, double seconds) {
                     const entt::entity start = at.find("start");
                     auto& menu = at.ecs().get<world::Control>(start);
                     if (slash::ui::was_pressed(at, start)) {
                         menu.visible = false;
                     }
                     if (menu.visible) {
                         return;
                     }
                     auto& player = at.ecs().get<world::Transform2D>(at.find("player"));
                     player.position.x += slash::input::get_axis(at, "move_left", "move_right") *
                                          kWalkSpeed * static_cast<float>(seconds);
                     int taken = 0;
                     for (const char* id : {"coin_1", "coin_2", "coin_3", "coin_4"}) {
                         const entt::entity coin = at.find(id);
                         if (coin == entt::null) {
                             ++taken;
                         } else if (player.position.x >=
                                    at.ecs().get<world::Transform2D>(coin).position.x) {
                             at.destroy(coin);
                             ++taken;
                         }
                     }
                     // The bug: the HUD counts out of 3, but the level has 4 coins.
                     at.ecs().get<world::Label>(at.find("coins")).text =
                         "Coins " + std::to_string(taken) + "/3";
                 },
                 {.doc = "Starts the game, walks the player and counts the coins taken."});
}

// The agent service's end of the connection.
struct Link {
    std::vector<std::string> to_game;
    std::vector<json::Value> from_game;
};

class Connection final : public autopilot::Transport {
public:
    explicit Connection(Link& link) : link_(link) {}
    void poll() override {}
    [[nodiscard]] State state() const override { return State::Open; }
    std::optional<std::string> receive() override {
        if (link_.to_game.empty()) {
            return std::nullopt;
        }
        std::string text = std::move(link_.to_game.front());
        link_.to_game.erase(link_.to_game.begin());
        return text;
    }
    bool send(std::string_view text) override {
        link_.from_game.push_back(json::parse(text).value());
        return true;
    }
    [[nodiscard]] std::string address() const override { return "in-process"; }

private:
    Link& link_;
};

// A scripted agent service: it asks, the game's loop runs, and the bridge answers.
struct AgentService {
    command::Session& session;
    Link link;
    double now = 0.0;
    std::unique_ptr<autopilot::Bridge> bridge;
    std::vector<json::Value> findings;
    int next_id = 1;

    AgentService(command::Session& s, json::Value run) : session(s) {
        autopilot::Options options;
        options.run = std::move(run);
        options.report_dir = "report";
        options.clock = [this] { return now; };
        bridge = std::make_unique<autopilot::Bridge>(
            session,
            [this]() -> slash::Result<std::unique_ptr<autopilot::Transport>> {
                return std::unique_ptr<autopilot::Transport>(std::make_unique<Connection>(link));
            },
            std::move(options));
    }

    // The game's loop: the bridge before each tick, as slash-cli run polls it.
    void frame() {
        bridge->poll(kTick);
        session.world().step(kTick);
        now += kTick;
        for (auto it = link.from_game.begin(); it != link.from_game.end();) {
            if (it->find("type")->as_string() != "events") {
                ++it;
                continue;
            }
            for (const json::Value& event : it->find("events")->as_array()) {
                if (event.find("kind")->as_string() == "finding") {
                    findings.push_back(*event.find("finding"));
                }
            }
            it = link.from_game.erase(it);
        }
    }

    void say(std::string_view frame_text) { link.to_game.emplace_back(frame_text); }

    // Sends a request and runs the game until its answer comes.
    json::Value ask(std::string_view op, std::string_view args) {
        const int id = next_id++;
        json::Value request;
        request["type"] = "request";
        request["id"] = id;
        request["op"] = op;
        request["args"] = json::parse(args).value();
        std::printf("> %.*s %s\n", static_cast<int>(op.size()), op.data(),
                    compact(*request.find("args")).c_str());
        say(compact(request));
        for (int i = 0; i < 600; ++i) {
            frame();
            for (auto it = link.from_game.begin(); it != link.from_game.end(); ++it) {
                if (it->find("type")->as_string() == "response" && it->find("id")->as_int() == id) {
                    json::Value response = std::move(*it);
                    link.from_game.erase(it);
                    return response.find("ok")->as_bool() ? *response.find("result")
                                                          : *response.find("error");
                }
            }
        }
        return {"no answer"};
    }
};

} // namespace

int main() {
    world::ComponentRegistry components;
    command::Registry commands;
    world::register_builtin_components(components);
    command::register_builtin_commands(commands);
    command::SessionOptions options;
    options.capture_logs = false;
    command::Session session(components, commands, options);
    if (!session.call("scene.open", json::parse(R"({"path": "main.scene.json"})").value()).ok) {
        return 1;
    }
    add_systems(session.world());

    // As `slash-cli run --autopilot-run verify --autopilot-prompt "..."` asks.
    AgentService service(
        session,
        json::parse(R"({"kind": "verify", "claim": "The HUD counts every coin the player takes"})")
            .value());
    service.frame();
    const json::Value& hello = service.link.from_game.front();
    std::printf("hello: engine %s, project %s, run %s\n", hello.find("engine")->as_string().c_str(),
                hello.find("project")->as_string().c_str(), compact(*hello.find("run")).c_str());
    service.link.from_game.clear();
    service.say(R"({"type": "welcome", "protocol": 1, "role": "game"})");
    service.frame();

    // What the game is: the player, the dimension and the keys it binds.
    const json::Value context = service.ask("context", "{}");
    std::printf("  character %s, %s, controls %s\n", context.find("character")->as_string().c_str(),
                context.find("dimension")->as_string().c_str(),
                compact(*context.find("controls")).c_str());

    // The title screen's controls, then a click on Start.
    std::printf("  %s\n", compact(service.ask("ui", "{}")).c_str());
    std::printf("  %s\n", compact(service.ask("ui_act", R"({"id": "start"})")).c_str());

    // Walk right for two seconds of game time, then see where the player got to.
    std::printf("  %s\n", compact(service.ask("key", R"({"key": "D", "mode": "hold",
        "seconds": 2})"))
                              .c_str());
    for (int i = 0; i < 150; ++i) {
        service.frame();
    }
    std::printf("  %s\n", compact(*service.ask("pose", "{}").find("at")).c_str());
    std::printf("  %s\n", compact(service.ask("screen", "{}")).c_str());

    // What the bridge's detectors found on their own.
    for (const json::Value& finding : service.findings) {
        std::printf("finding: %s (%s)\n", finding.find("summary")->as_string().c_str(),
                    finding.find("detector")->as_string().c_str());
    }

    // The verdict and the report, as the agent service sends them at the end of a run.
    service.say(R"({"type": "result", "result": {"kind": "verify", "verdict": "DENIED",
        "summary": "After four coins the HUD reads Coins 4/3."},
        "files": {"report.md": "# Denied\n\nThe level has 4 coins; the HUD counts out of 3.\n"}})");
    service.frame();
    std::printf("finished %s, exit code %d, report in %s\n",
                service.bridge->finished() ? "yes" : "no", service.bridge->exit_code(),
                service.bridge->report_dir().c_str());
    std::printf("report.md: %s", slash::platform::read_file("report/report.md").value().c_str());
    return 0;
}
