// SlashEngine's own playtest agent: `slash-cli run --autopilot-run verify --autopilot-prompt "..."
// --autopilot-agent playtester` plays a run with a model from providers.json (Claude, GPT,
// Gemini, a local model through Ollama...) instead of Autopilot's agent service and its hosted
// decision model. The agent is an agent service inside the game: it speaks the bridge's protocol
// over an in-process connection, gives the model the bridge's requests as tools, pauses the game
// while the model thinks, and writes the report and exit code as Autopilot's runs do.
//
// The game is a Start button, then a walker who picks up coins under a HUD that counts them out of
// 3, though the level has 4. The model here is a stand-in, a few rules that read what the tools
// answer, so the example needs no key and plays the same every time; a real model gets the same
// requests and answers.

#include "slash/ai/provider.hpp"
#include "slash/autopilot/agent.hpp"
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
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace ai = slash::ai;
namespace autopilot = slash::autopilot;
namespace command = slash::command;
namespace json = slash::json;
namespace world = slash::world;

constexpr double kTick = 1.0 / 60.0;
constexpr float kWalkSpeed = 120.0F; // pixels per second

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

// The text of the last message of a role, or "".
std::string last_text(const ai::Request& request, ai::Role role) {
    std::string text;
    for (const ai::Message& message : request.messages) {
        if (message.role == role) {
            text = message.text;
        }
    }
    return text;
}

ai::ToolCall call(std::string_view name, std::string_view arguments) {
    static int next = 0;
    return {.id = "call_" + std::to_string(++next),
            .name = std::string(name),
            .arguments = json::parse(arguments).value()};
}

// A stand-in for a model: what a model would do with what it is shown, written as rules.
class RuleModel final : public ai::Provider {
public:
    [[nodiscard]] const ai::Capabilities& capabilities() const override { return capabilities_; }

    slash::Result<ai::Response> complete(const ai::Request& request, const ai::StreamSink& /*sink*/,
                                         const ai::CancelFlag* /*cancel*/) override {
        ++turn_;
        // What the model was shown last: the brief, or its tools' answers.
        const ai::Message& last = request.messages.back();
        if (last.role == ai::Role::User) {
            std::printf("turn %d sees the brief: %zu tools, a Start button on screen: %s\n", turn_,
                        request.tools.size(),
                        last.text.find(R"("id":"start")") != std::string::npos ? "yes" : "no");
        } else {
            std::printf("turn %d sees:\n", turn_);
            for (const ai::Message& message : request.messages) {
                if (message.role == ai::Role::Tool && seen_.insert(message.tool_call_id)) {
                    std::string text = message.text.substr(0, message.text.find("\n\n["));
                    for (std::size_t at = text.find("\n\n"); at != std::string::npos;
                         at = text.find("\n\n")) {
                        text.erase(at, 1);
                    }
                    for (std::size_t at = text.find('\n'); at != std::string::npos;
                         at = text.find('\n', at + 3)) {
                        text.insert(at + 1, "  ");
                    }
                    std::printf("  %s\n", text.c_str());
                }
            }
        }

        ai::Response response;
        response.message.role = ai::Role::Assistant;
        response.model = "rule-model";
        std::vector<ai::ToolCall>& calls = response.message.tool_calls;
        const std::string seen = last_text(request, ai::Role::Tool);
        if (turn_ == 1) {
            // The menu first, then walk right, as the controls say D does, and look.
            calls.push_back(call("ui_act", R"({"id": "start"})"));
            calls.push_back(call("key", R"({"key": "D", "mode": "hold", "seconds": 2})"));
            calls.push_back(call("look", "{}"));
        } else if (const std::size_t at = seen.find("shows 4/3"); at != std::string::npos) {
            calls.push_back(call("note", R"({"text": "D walks right; the level has 4 coins."})"));
            calls.push_back(call("report", R"({"outcome": "DENIED",
                "summary": "After the agent took all 4 coins, the HUD read Coins 4/3.",
                "evidence": ["At 2.0 s of play, after walking past the 4 coins, the HUD read Coins 4/3."]})"));
        } else {
            calls.push_back(call("wait", R"({"seconds": 1})"));
        }
        for (const ai::ToolCall& tool : calls) {
            std::printf("  > %s %s\n", tool.name.c_str(),
                        json::write(tool.arguments, json::Style::Compact).c_str());
        }
        return response;
    }

private:
    // Tool calls whose answers were printed.
    struct Seen {
        std::vector<std::string> ids;
        bool insert(const std::string& id) {
            for (const std::string& known : ids) {
                if (known == id) {
                    return false;
                }
            }
            ids.push_back(id);
            return true;
        }
    };

    ai::Capabilities capabilities_;
    int turn_ = 0;
    Seen seen_;
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

    // As `slash-cli run --headless --autopilot-run verify --autopilot-prompt "..."
    // --autopilot-agent playtester` makes it, with ai::open's provider for the role.
    autopilot::AgentOptions agent;
    agent.provider = std::make_shared<RuleModel>();
    agent.model = "rule-model";
    agent.wait = true; // headless: wait for each reply, so runs repeat
    autopilot::Options run;
    run.run = json::parse(R"({"kind": "verify", "claim": "The HUD counts every coin"})").value();
    run.report_dir = "report";
    run.clock = autopilot::game_clock(session.world(), kTick);
    autopilot::Bridge bridge(session, autopilot::agent_connector(std::move(agent)), std::move(run));

    // The game's loop: the bridge before each tick, which runs while the game plays.
    while (!bridge.finished()) {
        bridge.poll(kTick);
        if (session.playing()) {
            session.world().step(kTick);
        }
    }

    std::printf("exit code %d\n", bridge.exit_code());
    std::printf("%s", slash::platform::read_file("report/report.md").value().c_str());
    std::printf("notes: %s",
                slash::platform::read_file(".slash/autopilot/notes.md").value().c_str());
    return 0;
}
