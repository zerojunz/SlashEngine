#include "slash/autopilot/bridge.hpp"
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/core/log.hpp"
#include "slash/gameplay/host.hpp"
#include "slash/input/input.hpp"
#include "slash/net/websocket.hpp"
#include "slash/platform/file.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/game_module.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace json = slash::json;
namespace command = slash::command;
namespace world = slash::world;
namespace autopilot = slash::autopilot;
namespace fs = std::filesystem;

namespace autopilot_game {

struct Walker {
    float speed = 120.0F;
};
SLASH_COMPONENT(Walker, speed)

// Walks right while move_right is held and left while move_left is.
void walk(world::World& w, double seconds) {
    for (const entt::entity e : w.ecs().view<world::Transform2D, Walker>()) {
        const float axis = slash::input::get_axis(w, "move_left", "move_right");
        w.ecs().get<world::Transform2D>(e).position.x +=
            axis * w.ecs().get<Walker>(e).speed * static_cast<float>(seconds);
    }
}

void module(world::GameModule& game) {
    game.add_component<Walker>().set_doc("Walks left and right.");
    game.add_system("walk", walk);
}

} // namespace autopilot_game

namespace {

constexpr double kTick = 1.0 / 60.0;

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? std::string() : parsed.error().message));
    return std::move(parsed).value();
}

std::string compact(const json::Value& value) {
    return json::write(value, json::Style::Compact);
}

void write(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    REQUIRE(slash::platform::write_file(path.string(), text).ok());
}

// A walker, a HUD that counts more coins than there are and a start button.
fs::path write_project(std::string_view name) {
    const fs::path root = fs::absolute(name);
    fs::remove_all(root);
    write(root / "project.json",
          R"({"format": 1, "name": "Walker", "main_scene": "level.scene.json",
        "gameplay": {"sources": ["src"]}, "input": {
        "move_left": {"events": [{"physical_key": "A", "type": "key"}]},
        "move_right": {"events": [{"physical_key": "D", "type": "key"}]},
        "jump": {"events": [{"physical_key": "Space", "type": "key"}]}}})");
    write(root / "level.scene.json", R"({"format": 1, "entities": {
        "player": {"components": {"Transform2D": {"position": [100, 200]}, "Walker": {}}},
        "hud": {"components": {"CanvasLayer": {"children": ["coins"]}}},
        "coins": {"components": {"Label": {"text": "Coins 4/3"}}},
        "start": {"components": {"Button": {"text": "Start"},
            "Control": {"offset_left": 200, "offset_top": 100, "offset_right": 300,
                        "offset_bottom": 140}}},
        "volume": {"components": {"Slider": {"value": 50},
            "Control": {"offset_left": 200, "offset_top": 160, "offset_right": 300,
                        "offset_bottom": 176}}}}})");
    write(root / "src/walker.cpp", "// The walker's code lives in the test.\n");
    write(root / "README.md", "Not Autopilot's README.\n");
    write(root / "build/skip.json", "{}");
    return root;
}

// The other end of a fake connection: what the agent service sends and what it got.
struct Link {
    slash::autopilot::Transport::State state = slash::autopilot::Transport::State::Open;
    std::vector<std::string> to_game;
    std::vector<json::Value> from_game;
};

class FakeTransport final : public autopilot::Transport {
public:
    explicit FakeTransport(std::shared_ptr<Link> link) : link_(std::move(link)) {}
    void poll() override {}
    [[nodiscard]] State state() const override { return link_->state; }
    std::optional<std::string> receive() override {
        if (link_->to_game.empty()) {
            return std::nullopt;
        }
        std::string text = std::move(link_->to_game.front());
        link_->to_game.erase(link_->to_game.begin());
        return text;
    }
    bool send(std::string_view text) override {
        link_->from_game.push_back(parse(text));
        return true;
    }
    [[nodiscard]] std::string address() const override { return "fake://brain"; }

private:
    std::shared_ptr<Link> link_;
};

// A game with the bridge, driven by a scripted agent service on a fake clock.
struct Game {
    world::ComponentRegistry components;
    command::Registry commands;
    std::unique_ptr<slash::gameplay::Host> host;
    std::unique_ptr<command::Session> session;
    std::vector<std::shared_ptr<Link>> links; // one per connection the bridge made
    bool refuse = false;                      // connections fail to start
    double now = 0.0;
    std::unique_ptr<autopilot::Bridge> bridge;
    int next_id = 1;

    explicit Game(const fs::path& project, autopilot::Options options = {}) {
        world::register_builtin_components(components);
        command::register_builtin_commands(commands);
        host = std::make_unique<slash::gameplay::Host>(components, autopilot_game::module);
        session = std::make_unique<command::Session>(
            components, commands,
            command::SessionOptions{
                .project_root = project.string(), .capture_logs = false, .gameplay = host.get()});
        REQUIRE(session->call("scene.open", parse(R"({"path": "level.scene.json"})")).ok);
        if (options.report_dir.empty()) {
            options.report_dir = (project / "report").string();
        }
        options.clock = [this] { return now; };
        bridge = std::make_unique<autopilot::Bridge>(
            *session,
            [this]() -> slash::Result<std::unique_ptr<autopilot::Transport>> {
                if (refuse) {
                    return slash::Error{.message = "refused"};
                }
                links.push_back(std::make_shared<Link>());
                return std::unique_ptr<autopilot::Transport>(
                    std::make_unique<FakeTransport>(links.back()));
            },
            std::move(options));
    }

    Link& link() {
        REQUIRE_FALSE(links.empty());
        return *links.back();
    }

    // One frame: the bridge, then a tick of the world.
    void frame() {
        bridge->poll(kTick);
        REQUIRE(
            session->call("step", parse(R"({"ticks": 1, "tick_seconds": 0.016666666666666666})"))
                .ok);
        now += kTick;
    }

    void frames(int n) {
        for (int i = 0; i < n; ++i) {
            frame();
        }
    }

    void say(const json::Value& frame) { link().to_game.push_back(compact(frame)); }

    // Connects and is welcomed; returns the hello.
    json::Value welcome() {
        frame();
        REQUIRE(link().from_game.size() == 1);
        json::Value hello = link().from_game.front();
        link().from_game.clear();
        say(parse(R"({"type": "welcome", "protocol": 1, "role": "game"})"));
        frame();
        REQUIRE(bridge->welcomed());
        return hello;
    }

    // The frames of a type the game sent since the last call, taken out.
    std::vector<json::Value> take(std::string_view type) {
        std::vector<json::Value> out;
        std::vector<json::Value>& sent = link().from_game;
        for (auto it = sent.begin(); it != sent.end();) {
            if (it->find("type")->as_string() == type) {
                out.push_back(std::move(*it));
                it = sent.erase(it);
            } else {
                ++it;
            }
        }
        return out;
    }

    // Sends a request and waits up to `limit` frames for its answer.
    json::Value ask(std::string_view op, std::string_view args = "{}", int limit = 120) {
        const int id = next_id++;
        json::Value request;
        request["type"] = "request";
        request["id"] = id;
        request["op"] = op;
        request["args"] = parse(args);
        say(request);
        for (int i = 0; i < limit; ++i) {
            frame();
            for (json::Value& response : take("response")) {
                if (response.find("id")->as_int() == id) {
                    return response;
                }
            }
        }
        FAIL("no answer to " << op);
        return {};
    }

    json::Value ok(std::string_view op, std::string_view args = "{}", int limit = 120) {
        json::Value response = ask(op, args, limit);
        REQUIRE_MESSAGE(response.find("ok")->as_bool(), compact(response));
        return *response.find("result");
    }

    std::string error(std::string_view op, std::string_view args = "{}") {
        json::Value response = ask(op, args);
        REQUIRE_FALSE(response.find("ok")->as_bool());
        return response.find("error")->as_string();
    }

    float player_x() {
        return session->world()
            .ecs()
            .get<world::Transform2D>(session->world().find("player"))
            .position.x;
    }
};

std::string read(const fs::path& path) {
    auto text = slash::platform::read_file(path.string());
    REQUIRE(text.ok());
    return text.value();
}

} // namespace

TEST_CASE("autopilot: hello and the probes the agent service sends every plugin") {
    autopilot::Options options;
    options.run = parse(R"({"kind": "verify", "claim": "The start button starts the game"})");
    Game game(write_project("autopilot_probes"), std::move(options));
    const json::Value hello = game.welcome();
    CHECK(hello.find("protocol")->as_int() == 1);
    CHECK(hello.find("engine")->as_string() == "godot");
    CHECK(hello.find("host")->as_string() == "game");
    CHECK(hello.find("project")->as_string() == "Walker");
    CHECK(compact(*hello.find("features")) == R"(["timed_hold","media"])");
    CHECK_FALSE(hello.find("share")->as_bool());
    CHECK(compact(*hello.find("run")) ==
          R"({"claim":"The start button starts the game","kind":"verify"})");

    // The agent service checks each op it needs answers anything but "unknown op".
    for (const auto& [op, args] : std::vector<std::pair<std::string, std::string>>{
             {"context", "{}"},
             {"session", R"({"action": "status"})"},
             {"ping", "{}"},
             {"rc", R"({"verb": "probe"})"},
             {"screen", "{}"},
             {"view", R"({"max_count": 1})"},
             {"ui", "{}"},
             {"watch", R"({"objects": [], "seconds": 0.01})"},
             {"key", R"({"key": "", "mode": "press"})"},
             {"input", R"({"action": ""})"},
             {"find", R"({"limit": 1})"},
             {"transforms", R"({"objects": []})"},
             {"pose", "{}"},
             {"movement", "{}"}}) {
        CAPTURE(op);
        const json::Value response = game.ask(op, args);
        if (!response.find("ok")->as_bool()) {
            CHECK_FALSE(response.find("error")->as_string().starts_with("unknown op"));
        }
    }
    CHECK(game.error("teleport") == "unknown op teleport");

    const json::Value context = game.ok("context");
    CHECK(context.find("character")->as_string() == "player");
    CHECK(context.find("dimension")->as_string() == "2d");
    CHECK(context.find("game_name")->as_string() == "Walker");
    CHECK(context.find("scene")->as_string() == "level.scene.json");

    // Where the player is, in the kit frame: 2D x stays X and y goes up as Z.
    const json::Value pose = game.ok("pose");
    CHECK(pose.find("path")->as_string() == "player");
    CHECK(compact(*pose.find("at")) == R"({"X":100.0,"Y":0.0,"Z":-200.0})");
}

TEST_CASE("autopilot: keys, actions and moves reach the game as input") {
    Game game(write_project("autopilot_input"));
    game.welcome();
    const float start = game.player_x();

    // A tap holds the key for 0.05 s and two frames before the answer.
    game.ok("key", R"({"key": "D", "mode": "press"})");
    const float tapped = game.player_x();
    CHECK(tapped > start);

    // A held key walks until it is let go.
    game.ok("key", R"({"key": "D", "mode": "hold"})");
    game.frames(30);
    CHECK(game.player_x() > tapped + 50.0F);
    game.ok("key", R"({"key": "D", "mode": "release"})");
    const float stopped = game.player_x();
    game.frames(10);
    CHECK(game.player_x() == doctest::Approx(stopped));

    // A hold that isn't renewed ends with its lease.
    game.ok("key", R"({"key": "A", "mode": "hold", "lease": 0.5})");
    game.frames(60);
    const float leased = game.player_x();
    game.frames(10);
    CHECK(game.player_x() == doctest::Approx(leased));
    CHECK(leased < stopped);

    // Actions by name, and a move by the WASD actions: X is right in 2D.
    game.ok("input", R"({"action": "move_right", "mode": "hold"})");
    game.frames(10);
    game.ok("input", R"({"action": "move_right", "mode": "release"})");
    CHECK(game.player_x() > leased);
    CHECK(game.error("input", R"({"action": "fly"})") == "No input action named fly");
    CHECK(game.error("key", R"({"key": "NoSuchKey"})") == "Unknown key NoSuchKey");

    const float before = game.player_x();
    const json::Value moved = game.ok("move", R"({"direction": {"X": 1, "Y": 0, "Z": 0},
        "seconds": 0.5})");
    CHECK(moved.find("moved")->as_double() > 40.0);
    CHECK(game.player_x() > before + 40.0F);

    // The controls the game binds, by Unreal's key names.
    const json::Value context = game.ok("context");
    CHECK(compact(*context.find("controls")).find("move_right") != std::string::npos);
}

TEST_CASE("autopilot: rc reads and writes components and calls commands") {
    Game game(write_project("autopilot_rc"));
    game.welcome();
    CHECK(compact(game.ok("rc", R"({"verb": "property", "body": {"objectPath": "/root/player",
        "propertyName": "Transform2D/position/x"}})")) == R"({"Transform2D/position/x":100.0})");
    game.ok("rc", R"({"verb": "property", "body": {"objectPath": "player",
        "propertyName": "Walker:speed", "propertyValue": {"Walker:speed": 30},
        "access": "WRITE_ACCESS"}})");
    CHECK(game.session->world()
              .ecs()
              .get<autopilot_game::Walker>(game.session->world().find("player"))
              .speed == 30.0F);
    CHECK(game.error("rc", R"({"verb": "property", "body": {"objectPath": "ghost"}})") ==
          "Object ghost does not exist");
    CHECK(game.error("rc", R"({"verb": "property", "body": {"objectPath": "player",
        "propertyName": "Health"}})") == "Property Health does not exist on object player");

    const json::Value called = game.ok("rc", R"({"verb": "call", "body": {
        "objectPath": "player", "functionName": "entity.get",
        "parameters": {"components": ["Walker"]}}})");
    CHECK(compact(*called.find("ReturnValue")) ==
          R"({"components":{"Walker":{"speed":30.0}},"id":"player"})");

    const json::Value described =
        game.ok("rc", R"({"verb": "describe", "body": {"objectPath": "player"}})");
    CHECK(described.find("Name")->as_string() == "player");
    CHECK(compact(*described.find("Properties")).find("Walker/speed") != std::string::npos);
    CHECK(game.error("rc", R"({"verb": "teleport", "body": {}})") == "unknown rc verb teleport");
}

TEST_CASE("autopilot: the screen's texts and controls, and working them") {
    Game game(write_project("autopilot_ui"));
    game.welcome();
    const json::Value screen = game.ok("screen");
    CHECK(compact(*screen.find("texts")) == R"({"Screen.start":"Start","hud.coins":"Coins 4/3"})");

    const json::Value ui = game.ok("ui");
    const json::Array& elements = ui.find("elements")->as_array();
    REQUIRE(elements.size() == 2);
    CHECK(compact(elements[0]) ==
          compact(parse(R"({"class": "Button", "enabled": true, "id": "start", "kind": "button",
              "label": "Start", "name": "start", "owner": "Screen", "path": "start",
              "screen": {"x": 250, "y": 120}})")));
    CHECK(elements[1].find("kind")->as_string() == "slider");

    // A click goes in as the mouse; the button says it was pressed.
    CHECK(compact(game.ok("ui_act", R"({"id": "Screen.start"})")) == R"({"via":"mouse"})");
    game.ok("ui_act", R"({"id": "volume", "action": "set", "value": 80})");
    CHECK(game.session->world()
              .ecs()
              .get<world::Slider>(game.session->world().find("volume"))
              .value == 80.0);
    CHECK(game.error("ui_act", R"({"id": "coins_label"})") == "no element coins_label on screen");
}

TEST_CASE("autopilot: detectors report a HUD over its maximum and engine errors once") {
    Game game(write_project("autopilot_detectors"));
    game.welcome();
    game.frames(5);
    slash::log::error("the save file is gone", {{"slot", 2}});
    game.frames(70);
    std::vector<json::Value> findings;
    for (const json::Value& batch : game.take("events")) {
        for (const json::Value& event : batch.find("events")->as_array()) {
            if (event.find("kind")->as_string() == "finding") {
                findings.push_back(*event.find("finding"));
            }
        }
    }
    REQUIRE(findings.size() == 2);
    std::string seen;
    for (const json::Value& finding : findings) {
        seen += finding.find("detector")->as_string() + ": " +
                finding.find("summary")->as_string() + "\n";
    }
    CHECK(seen.find("hud_fraction: HUD text hud.coins shows 4/3: more than its stated maximum\n") !=
          std::string::npos);
    CHECK(seen.find("log_error: Engine error: the save file is gone\n") != std::string::npos);
}

TEST_CASE("autopilot: the codex serves the project's files and nothing outside it") {
    Game game(write_project("autopilot_codex"));
    game.welcome();
    const json::Value manifest = game.ok("codex_manifest");
    std::vector<std::string> paths;
    for (const json::Value& file : manifest.find("files")->as_array()) {
        paths.push_back(file.find("path")->as_string());
    }
    CHECK(paths == std::vector<std::string>{".godot/autopilot_codex/project/README.md",
                                            ".godot/autopilot_codex/level.scene.json",
                                            ".godot/autopilot_codex/project.json",
                                            ".godot/autopilot_codex/src/walker.cpp",
                                            ".godot/autopilot_codex/summary.txt"});

    const json::Value read = game.ok("codex_read", R"({"paths": [
        ".godot/autopilot_codex/src/walker.cpp", ".godot/autopilot_codex/summary.txt",
        ".godot/autopilot_codex/../secret.json", ".godot/autopilot_codex/build/skip.json",
        ".godot/autopilot_codex/README.md", "project.json"]})");
    const json::Value& files = *read.find("files");
    CHECK(files.find(".godot/autopilot_codex/src/walker.cpp")->as_string() ==
          "// The walker's code lives in the test.\n");
    const std::string summary = files.find(".godot/autopilot_codex/summary.txt")->as_string();
    CHECK(summary.find("Main scene: level.scene.json") != std::string::npos);
    CHECK(summary.find("move_right: D") != std::string::npos);
    CHECK(summary.find("    player (Transform2D, Walker)") != std::string::npos);
    CHECK(compact(*read.find("missing")) ==
          R"([".godot/autopilot_codex/../secret.json",".godot/autopilot_codex/build/skip.json",)"
          R"(".godot/autopilot_codex/README.md","project.json"])");
}

TEST_CASE("autopilot: a run's result writes its report and gives the exit code") {
    const fs::path project = write_project("autopilot_result");
    autopilot::Options options;
    options.run = parse(R"({"kind": "verify", "claim": "Coins count up to 3"})");
    Game game(project, std::move(options));
    game.welcome();
    game.say(parse(R"({"type": "result", "result": {"kind": "verify", "verdict": "DENIED",
        "summary": "The HUD shows 4/3"}, "files": {"report.md": "# Denied\n",
        "../escape.md": "kept as escape.md"}})"));
    game.frame();
    CHECK(game.bridge->finished());
    CHECK(game.bridge->exit_code() == 2);
    CHECK(game.bridge->report_dir() == (project / "report").string());
    CHECK(read(project / "report/report.md") == "# Denied\n");
    CHECK(read(project / "report/escape.md") == "kept as escape.md");
    CHECK_FALSE(fs::exists(project / "escape.md"));
    CHECK(parse(read(project / "report/result.json")).find("verdict")->as_string() == "DENIED");
}

TEST_CASE("autopilot: runs fail when the agent service can't be reached or goes away") {
    SUBCASE("no welcome in time") {
        autopilot::Options options;
        options.run = parse(R"({"kind": "explore", "minutes": 1})");
        options.connect_timeout = 2.0;
        Game game(write_project("autopilot_timeout"), std::move(options));
        game.refuse = true;
        for (int i = 0; i < 200 && !game.bridge->finished(); ++i) {
            game.frame();
        }
        CHECK(game.bridge->finished());
        CHECK(game.bridge->exit_code() == 1);
        CHECK(game.bridge->result().find("error")->as_string().starts_with(
            "no welcome from the agent service within 2 s"));
    }
    SUBCASE("closed before the result") {
        autopilot::Options options;
        options.run = parse(R"({"kind": "goal", "goal": "Reach the flag"})");
        Game game(write_project("autopilot_closed"), std::move(options));
        game.welcome();
        game.link().state = autopilot::Transport::State::Closed;
        game.frame();
        CHECK(game.bridge->finished());
        CHECK(game.bridge->exit_code() == 1);
    }
    SUBCASE("an error frame") {
        autopilot::Options options;
        options.run = parse(R"({"kind": "auto", "prompt": "Play"})");
        Game game(write_project("autopilot_error"), std::move(options));
        game.frame();
        game.say(parse(R"({"type": "error", "message": "protocol 2 needed"})"));
        game.frame();
        CHECK(game.bridge->finished());
        CHECK(game.bridge->exit_code() == 1);
        CHECK(game.bridge->problem() == "the agent service sent an error: protocol 2 needed");
    }
    SUBCASE("without a run, it reconnects with a backoff") {
        Game game(write_project("autopilot_reconnect"));
        game.welcome();
        game.link().state = autopilot::Transport::State::Closed;
        game.frame();
        CHECK_FALSE(game.bridge->finished());
        CHECK(game.links.size() == 1);
        game.frames(70); // a second later
        CHECK(game.links.size() == 2);
        CHECK(game.link().from_game.front().find("type")->as_string() == "hello");
    }
}

TEST_CASE("autopilot: the bridge talks to an agent service over a WebSocket") {
    if (!slash::net::websocket_server_supported()) {
        return;
    }
    const fs::path project = write_project("autopilot_websocket");
    slash::net::WebSocketMultiplayerPeer server;
    REQUIRE(server.create_server(0, "127.0.0.1"));
    const std::string url = "ws://127.0.0.1:" + std::to_string(server.local_port()) + "/v1/bridge";

    world::ComponentRegistry components;
    command::Registry commands;
    world::register_builtin_components(components);
    command::register_builtin_commands(commands);
    slash::gameplay::Host host(components, autopilot_game::module);
    command::Session session(components, commands,
                             command::SessionOptions{.project_root = project.string(),
                                                     .capture_logs = false,
                                                     .gameplay = &host});
    REQUIRE(session.call("scene.open", parse(R"({"path": "level.scene.json"})")).ok);
    autopilot::Options options;
    options.run = parse(R"({"kind": "verify", "claim": "It connects"})");
    options.report_dir = (project / "report").string();
    autopilot::Bridge bridge(session, autopilot::websocket_connector(url, "secret"),
                             std::move(options));

    // Polls both ends until the server has a text message from the game.
    int client = 0;
    const auto next_message = [&]() -> json::Value {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline) {
            bridge.poll(kTick);
            server.poll();
            for (const slash::net::PeerEvent& event : server.take_events()) {
                client = event.peer;
            }
            while (server.available_packet_count() > 0) {
                auto packet = server.get_packet();
                REQUIRE(packet.ok());
                const std::string text(packet.value().begin(), packet.value().end());
                auto frame = json::parse(text);
                if (frame.ok() && frame.value().is_object() &&
                    frame.value().find("type")->as_string() != "events") {
                    return frame.value();
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        FAIL("nothing from the game");
        return {};
    };
    const json::Value hello = next_message();
    CHECK(hello.find("type")->as_string() == "hello");
    REQUIRE(client != 0);
    REQUIRE(server.peer(client)->send_text(R"({"type": "welcome", "role": "game"})").ok());
    REQUIRE(server.peer(client)
                ->send_text(R"({"type": "request", "id": 7, "op": "ping", "args": {}})")
                .ok());
    const json::Value pong = next_message();
    CHECK(pong.find("id")->as_int() == 7);
    CHECK(pong.find("ok")->as_bool());
    CHECK(bridge.welcomed());
    REQUIRE(server.peer(client)
                ->send_text(R"({"type": "result", "result": {"verdict": "CONFIRMED"}})")
                .ok());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!bridge.finished() && std::chrono::steady_clock::now() < deadline) {
        bridge.poll(kTick);
        server.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(bridge.finished());
    CHECK(bridge.exit_code() == 0);
}
