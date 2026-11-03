#include "slash/ai/provider.hpp"
#include "slash/autopilot/agent.hpp"
#include "slash/autopilot/bridge.hpp"
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/input/input.hpp"
#include "slash/platform/file.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace json = slash::json;
namespace ai = slash::ai;
namespace command = slash::command;
namespace world = slash::world;
namespace autopilot = slash::autopilot;
namespace fs = std::filesystem;

namespace {

constexpr double kTick = 1.0 / 60.0;

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? std::string() : parsed.error().message));
    return std::move(parsed).value();
}

void write(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    REQUIRE(slash::platform::write_file(path.string(), text).ok());
}

std::string read(const fs::path& path) {
    auto text = slash::platform::read_file(path.string());
    REQUIRE_MESSAGE(text.ok(), path.string());
    return std::move(text).value();
}

// A player who walks right while D is held, and a HUD that counts more coins than there are.
fs::path write_project(std::string_view name) {
    const fs::path root = fs::absolute(name);
    fs::remove_all(root);
    write(root / "project.json",
          R"({"format": 1, "name": "Walker", "main_scene": "level.scene.json", "input": {
        "move_left": {"events": [{"physical_key": "A", "type": "key"}]},
        "move_right": {"events": [{"physical_key": "D", "type": "key"}]}}})");
    write(root / "level.scene.json", R"({"format": 1, "entities": {
        "player": {"components": {"Transform2D": {"position": [100, 200]}}},
        "hud": {"components": {"CanvasLayer": {"children": ["coins"]}}},
        "coins": {"components": {"Label": {"text": "Coins 4/3"}}}}})");
    return root;
}

// A model that answers from a script: each turn sees the request and gives the reply.
using Turn = std::function<slash::Result<ai::Response>(const ai::Request&)>;

class ScriptedModel final : public ai::Provider {
public:
    explicit ScriptedModel(std::vector<Turn> turns) : turns_(std::move(turns)) {}

    [[nodiscard]] const ai::Capabilities& capabilities() const override { return capabilities_; }

    slash::Result<ai::Response> complete(const ai::Request& request, const ai::StreamSink&,
                                         const ai::CancelFlag*) override {
        requests.push_back(request);
        if (turns_.empty()) {
            return slash::Error{.message = "the script ran out"};
        }
        Turn turn = std::move(turns_.front());
        turns_.erase(turns_.begin());
        return turn(request);
    }

    std::vector<ai::Request> requests;

private:
    std::vector<Turn> turns_;
    ai::Capabilities capabilities_;
};

ai::ToolCall tool(std::string id, std::string name, std::string_view arguments) {
    return {.id = std::move(id), .name = std::move(name), .arguments = parse(arguments)};
}

ai::Response calls(std::vector<ai::ToolCall> tools) {
    ai::Response response;
    response.message.role = ai::Role::Assistant;
    response.message.tool_calls = std::move(tools);
    response.stop = ai::StopReason::ToolCalls;
    response.usage = {.input_tokens = 100, .output_tokens = 10};
    response.model = "scripted";
    return response;
}

// The text of the tool result answering a call.
std::string result_of(const ai::Request& request, std::string_view id) {
    for (const ai::Message& message : request.messages) {
        if (message.role == ai::Role::Tool && message.tool_call_id == id) {
            return (message.is_error ? "error: " : "") + message.text;
        }
    }
    FAIL("no result for " << id);
    return {};
}

// A game run by the built-in agent, as slash-cli run --autopilot-agent runs it.
struct Game {
    world::ComponentRegistry components;
    command::Registry commands;
    std::unique_ptr<command::Session> session;
    std::shared_ptr<ScriptedModel> model;
    std::unique_ptr<autopilot::Bridge> bridge;

    Game(const fs::path& project, std::vector<Turn> turns, autopilot::AgentOptions agent = {},
         std::string_view run = R"({"kind": "verify", "claim": "The HUD counts every coin"})") {
        world::register_builtin_components(components);
        command::register_builtin_commands(commands);
        session = std::make_unique<command::Session>(
            components, commands,
            command::SessionOptions{.project_root = project.string(), .capture_logs = false});
        REQUIRE(session->call("scene.open", parse(R"({"path": "level.scene.json"})")).ok);
        model = std::make_shared<ScriptedModel>(std::move(turns));
        agent.provider = model;
        agent.model = "scripted";
        agent.project_root = project.string();
        agent.wait = true;
        autopilot::Options options;
        options.run = parse(run);
        options.report_dir = (project / "report").string();
        options.clock = autopilot::game_clock(session->world(), kTick);
        bridge = std::make_unique<autopilot::Bridge>(
            *session, autopilot::agent_connector(std::move(agent)), std::move(options));
    }

    // The game's loop until the run ends: the bridge before each tick, and ticks while playing.
    void play() {
        for (int frame = 0; frame < 100000 && !bridge->finished(); ++frame) {
            bridge->poll(kTick);
            if (session->playing()) {
                session->world().step(kTick);
            }
        }
        REQUIRE(bridge->finished());
    }
};

} // namespace

TEST_CASE("autopilot agent: a model plays a run through the bridge and reports") {
    const fs::path root = write_project("autopilot_agent_verify");
    std::vector<Turn> turns;
    // The first turn sees the brief: the request, the game and what is on screen.
    turns.emplace_back([](const ai::Request& request) {
        REQUIRE(request.messages.size() == 2);
        CHECK(request.messages[0].role == ai::Role::System);
        CHECK(request.messages[0].text.find("You are a playtester") != std::string::npos);
        const std::string& brief = request.messages[1].text;
        CHECK(brief.find("Request (verify): The HUD counts every coin") != std::string::npos);
        CHECK(brief.find("Coins 4/3") != std::string::npos);
        CHECK(brief.find("No notes from earlier runs") != std::string::npos);
        CHECK(request.tools.size() == 18);
        return calls({tool("a", "read_files", R"({"paths": ["level.scene.json"]})"),
                      tool("b", "key", R"({"key": "D", "mode": "hold", "seconds": 1})"),
                      tool("c", "look", "{}"), tool("d", "screenshot", "{}"),
                      tool("e", "rc", R"({"verb": "call", "body": {"functionName": "step"}})")});
    });
    // The second sees each tool's answer, the picture and what the detectors found.
    turns.emplace_back([](const ai::Request& request) {
        CHECK(result_of(request, "a").find("Coins 4/3") != std::string::npos);
        CHECK(result_of(request, "b") == R"({"held_seconds":1.0})");
        CHECK(result_of(request, "c").find("\"pose\"") != std::string::npos);
        CHECK(result_of(request, "d").find("attached after the results") != std::string::npos);
        CHECK(result_of(request, "e").starts_with("error: a write or a call needs why\n\n"));
        const ai::Message& pictures = request.messages.back();
        CHECK(pictures.role == ai::Role::User);
        REQUIRE(pictures.images.size() == 1);
        CHECK(pictures.images[0].media_type == "image/jpeg");
        const std::string last = result_of(request, "e");
        CHECK(last.find("The detectors found:\n- ") != std::string::npos);
        CHECK(last.find("(hud_fraction)") != std::string::npos);
        CHECK(last.find("[59 turns and ") != std::string::npos);
        return calls({tool("f", "note", R"({"text": "D walks the player right."})"),
                      tool("g", "rc", R"({"verb": "property", "why": "to read the HUD again",
                          "body": {"objectPath": "coins", "propertyName": "Label/text",
                                   "propertyValue": "Coins 4/4", "access": "WRITE_ACCESS"}})"),
                      tool("h", "report", R"({"outcome": "DENIED",
                          "summary": "The HUD reads Coins 4/3: it counts out of 3.",
                          "evidence": ["At 0.3 s the HUD read Coins 4/3."]})")});
    });
    Game game(root, std::move(turns));
    game.play();

    CHECK(game.model->requests.size() == 2);
    CHECK(game.bridge->exit_code() == 2);
    const json::Value& result = game.bridge->result();
    CHECK(result.find("kind")->as_string() == "verify");
    CHECK(result.find("verdict")->as_string() == "DENIED");
    CHECK(result.find("agent")->find("turns")->as_int() == 2);
    CHECK(result.find("agent")->find("input_tokens")->as_int() == 200);
    CHECK(result.find("detector_findings")->size() >= 1);
    CHECK(result.find("direct_changes")->size() == 1);
    // About a second of play: the hold, and the taps around it; none while the model thought.
    CHECK(result.find("agent")->find("play_seconds")->as_double() < 1.5);
    const std::string report = read(root / "report/report.md");
    CHECK(report.find("# Verify: The HUD counts every coin") != std::string::npos);
    CHECK(report.find("**Outcome: DENIED**") != std::string::npos);
    CHECK(report.find("## What the detectors found") != std::string::npos);
    CHECK(report.find("## Direct changes to the game") != std::string::npos);
    CHECK(report.find("Played by scripted in 2 turns") != std::string::npos);
    CHECK(read(root / ".slash/autopilot/notes.md") ==
          "# Notes from playtests\n\n- D walks the player right.\n");
    CHECK(read(root / "report/result.json").find("\"exit_code\": 2") != std::string::npos);
    fs::remove_all(root);
}

TEST_CASE("autopilot agent: notes from earlier runs are in the brief") {
    const fs::path root = write_project("autopilot_agent_notes");
    write(root / ".slash/autopilot/notes.md", "# Notes from playtests\n\n- Coins are gold.\n");
    std::vector<Turn> turns;
    turns.emplace_back([](const ai::Request& request) {
        CHECK(request.messages[1].text.find("- Coins are gold.") != std::string::npos);
        return calls({tool("a", "report", R"({"outcome": "explored", "summary": "Fine.",
            "evidence": []})")});
    });
    Game game(root, std::move(turns), {}, R"({"kind": "explore", "minutes": 1})");
    game.play();
    CHECK(game.bridge->exit_code() == 0);
    CHECK(game.bridge->result().find("kind")->as_string() == "explore");
    fs::remove_all(root);
}

TEST_CASE("autopilot agent: turns and play time run out") {
    const fs::path root = write_project("autopilot_agent_limits");
    std::vector<Turn> turns;
    for (int i = 0; i < 3; ++i) {
        turns.emplace_back([i](const ai::Request& request) {
            if (i == 1) {
                CHECK_FALSE(result_of(request, "w0").starts_with("error: "));
            }
            if (i == 2) {
                // The last turn may only report, and play time is up.
                REQUIRE(request.tools.size() == 1);
                CHECK(request.tools[0].name == "report");
                CHECK(result_of(request, "w1").find("Call report now.") != std::string::npos);
                ai::Response response;
                response.message.role = ai::Role::Assistant;
                response.message.text = "I give up.";
                return slash::Result<ai::Response>(std::move(response));
            }
            return slash::Result<ai::Response>(
                calls({tool("w" + std::to_string(i), "wait", R"({"seconds": 1})")}));
        });
    }
    autopilot::AgentOptions agent;
    agent.max_turns = 3;
    agent.play_seconds = 1.5;
    Game game(root, std::move(turns), agent);
    game.play();
    CHECK(game.model->requests.size() == 3);
    CHECK(game.bridge->exit_code() == 3);
    const json::Value& result = game.bridge->result();
    CHECK(result.find("verdict")->as_string() == "INCONCLUSIVE");
    CHECK(result.find("summary")->as_string().starts_with("No report: the agent stopped"));
    CHECK(result.find("agent")->find("play_seconds")->as_double() ==
          doctest::Approx(1.5).epsilon(0.1));
    fs::remove_all(root);
}

TEST_CASE("autopilot agent: a model that fails ends the run with an error") {
    const fs::path root = write_project("autopilot_agent_error");
    std::vector<Turn> turns;
    turns.emplace_back([](const ai::Request&) -> slash::Result<ai::Response> {
        return slash::Error{.message = "no key"};
    });
    Game game(root, std::move(turns));
    game.play();
    CHECK(game.bridge->exit_code() == 1);
    CHECK(game.bridge->result().find("error")->as_string() == "the model's reply failed: no key");
    fs::remove_all(root);
}
