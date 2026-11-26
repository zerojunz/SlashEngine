#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/exporter/commands.hpp"
#include "slash/exporter/presets.hpp"
#include "slash/exporter/publish.hpp"
#include "slash/platform/file.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <SDL3/SDL_stdinc.h>
#include <doctest/doctest.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace command = slash::command;
namespace exporter = slash::exporter;
namespace json = slash::json;

namespace {

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? std::string() : parsed.error().message));
    return std::move(parsed).value();
}

bool contains(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

// Sets an environment variable, or unsets it for an empty value, and puts it back on scope exit.
class Variable {
public:
    Variable(std::string name, const std::string& value) : name_(std::move(name)) {
        if (const char* old = SDL_getenv_unsafe(name_.c_str())) {
            old_ = old;
        }
        set(value);
    }
    ~Variable() { set(old_.value_or("")); }
    Variable(const Variable&) = delete;
    Variable& operator=(const Variable&) = delete;
    Variable(Variable&&) = delete;
    Variable& operator=(Variable&&) = delete;

    void set(const std::string& value) const {
        if (value.empty()) {
            SDL_unsetenv_unsafe(name_.c_str());
        } else {
            SDL_setenv_unsafe(name_.c_str(), value.c_str(), 1);
        }
    }

private:
    std::string name_;
    std::optional<std::string> old_;
};

std::string read(const std::filesystem::path& path) {
    auto text = slash::platform::read_file(path.string());
    return text ? text.value() : std::string();
}

// A game with four presets, one of them for iOS, which itch.io doesn't take.
void write_game(const std::string& root, std::string_view stores) {
    std::filesystem::remove_all(root);
    REQUIRE(slash::platform::create_directories(root).ok());
    REQUIRE(slash::platform::write_file(root + "/project.json", R"({"format": 1,
        "name": "Space Game", "main_scene": "main.scene.json", "version": "1.2.0"})")
                .ok());
    REQUIRE(slash::platform::write_file(root + "/main.scene.json", R"({"format": 1,
        "entities": {}})")
                .ok());
    REQUIRE(slash::platform::write_file(root + "/export_presets.json",
                                        std::string(R"({"format": 1, "presets": {
        "mac-release": {"platform": "macos"},
        "pc": {"platform": "windows", "output": "dist/pc"},
        "pi": {"platform": "linux", "linux": {"arch": "arm64"}},
        "phone": {"platform": "ios"}}, "stores": )") +
                                            std::string(stores) + "}")
                .ok());
}

struct Engine {
    slash::world::ComponentRegistry components;
    command::Registry commands;
    std::unique_ptr<command::Session> session;

    explicit Engine(command::SessionOptions options) {
        slash::world::register_builtin_components(components);
        command::register_builtin_commands(commands);
        exporter::register_export_commands(commands);
        options.capture_logs = false;
        session = std::make_unique<command::Session>(components, commands, std::move(options));
    }
};

std::vector<std::string> field(const json::Value& builds, std::string_view key) {
    std::vector<std::string> out;
    for (const json::Value& build : builds.as_array()) {
        out.push_back(build.find(key)->as_string());
    }
    return out;
}

} // namespace

TEST_CASE("export presets say where stores take each build") {
    CHECK(exporter::itch_channel("macos") == "mac");
    CHECK(exporter::itch_channel("linux", "arm64") == "linux-arm64");
    CHECK(exporter::itch_channel("web") == "html5");
    CHECK(exporter::itch_channel("ios").empty());

    const json::Value good = parse(R"({"format": 1, "presets": {"pc": {"platform": "windows"}},
        "stores": {"itch": {"project": "player/space-game",
                            "channels": {"pc": "windows-beta", "web": "html5"}}}})");
    CHECK(exporter::validate_presets(good).empty());
    const exporter::Stores stores = exporter::parse_stores(good);
    REQUIRE(stores.itch.has_value());
    CHECK(stores.itch->project == "player/space-game");
    CHECK(stores.itch->channels.at("pc") == "windows-beta");

    // Each channel names a preset or a platform itch.io takes; the project is "user/game".
    const json::Value bad = parse(R"({"format": 1, "presets": {"phone": {"platform": "ios"}},
        "stores": {"itch": {"project": "player/space-game",
                            "channels": {"phnoe": "x", "phone": "iphone", "ios": "ios", "web": "html 5"}}}})");
    std::vector<std::string> paths;
    for (const auto& error : exporter::validate_presets(bad)) {
        paths.push_back(error.path + " " + error.message);
    }
    CHECK(paths == std::vector<std::string>{
                       "/stores/itch/channels/ios itch.io doesn't take iOS games",
                       "/stores/itch/channels/phnoe no preset or platform called \"phnoe\"",
                       "/stores/itch/channels/phone itch.io doesn't take iOS games",
                       "/stores/itch/channels/web channel names are letters, digits, '.', '_' "
                       "or '-'"});
    CHECK_FALSE(exporter::validate_presets(parse(R"({"format": 1, "presets": {},
        "stores": {"itch": {"project": "https://player.itch.io/space-game"}}})"))
                    .empty());
    CHECK_FALSE(exporter::validate_presets(parse(R"({"format": 1, "presets": {},
        "stores": {"steem": {}}})"))
                    .empty());
}

TEST_CASE("publish plans each build's channel and push") {
    const std::string root = "publish_plan_project";
    write_game(root, R"({"itch": {"project": "player/space-game"}})");
    exporter::PublishOptions options;
    options.store = "itch";
    options.project = root;

    // Without channels, every preset itch.io takes goes on its platform's channel.
    exporter::PublishResult plan = exporter::publish_game(options);
    REQUIRE_MESSAGE(plan.ok, json::write(plan.json));
    CHECK(plan.json.find("version")->as_string() == "1.2.0");
    CHECK_FALSE(plan.json.find("upload")->as_bool());
    const json::Value& builds = *plan.json.find("builds");
    CHECK(field(builds, "preset") == std::vector<std::string>{"mac-release", "pc", "pi"});
    CHECK(field(builds, "channel") == std::vector<std::string>{"mac", "windows", "linux-arm64"});
    CHECK(field(builds, "output") ==
          std::vector<std::string>{"build/export/mac-release", "dist/pc", "build/export/pi"});
    CHECK(*builds.as_array()[1].find("command") ==
          parse(R"(["butler", "push", "dist/pc", "player/space-game:windows", "--userversion",
                    "1.2.0", "--if-changed"])"));

    // Named presets and platforms, with their own channels where the file gives them.
    write_game(root, R"({"itch": {"project": "player/space-game",
                                  "channels": {"pc": "windows-beta", "web": "html5"}}})");
    plan = exporter::publish_game(options);
    REQUIRE(plan.ok);
    CHECK(field(*plan.json.find("builds"), "target") ==
          std::vector<std::string>{"player/space-game:windows-beta", "player/space-game:html5"});
    options.targets = {"mac-release", "pc"};
    options.version = "1.2.1-beta";
    plan = exporter::publish_game(options);
    REQUIRE(plan.ok);
    CHECK(field(*plan.json.find("builds"), "channel") ==
          std::vector<std::string>{"mac", "windows-beta"});
    CHECK(plan.json.find("version")->as_string() == "1.2.1-beta");

    // What the plan can't publish.
    options.targets = {"phone"};
    plan = exporter::publish_game(options);
    REQUIRE_FALSE(plan.ok);
    CHECK(contains(json::write(plan.json), "which itch.io doesn't take"));
    options.targets = {"mac-relase"};
    CHECK(contains(json::write(exporter::publish_game(options).json),
                   "did you mean \\\"mac-release\\\""));
    options.targets = {"pc", "windows"};
    write_game(root, R"({"itch": {"project": "player/space-game"}})");
    CHECK(contains(json::write(exporter::publish_game(options).json),
                   "two builds go to the itch.io channel \\\"windows\\\""));
    options.targets.clear();
    options.store = "ich";
    plan = exporter::publish_game(options);
    CHECK(plan.json.find("error")->find("step")->as_string() == "options");
    CHECK(contains(json::write(plan.json), "did you mean \\\"itch\\\""));
    write_game(root, "{}");
    options.store = "itch";
    CHECK(contains(json::write(exporter::publish_game(options).json), "has no stores.itch"));
    std::filesystem::remove_all(root);
}

TEST_CASE("publish uploads exports through butler") {
    const std::string root = "publish_upload_project";
    write_game(root, R"({"itch": {"project": "player/space-game",
                                  "channels": {"mac-release": "mac", "pc": "windows"}}})");
    const std::filesystem::path log = std::filesystem::absolute(root + "-butler.log");
    std::filesystem::remove(log);
    Variable butler(std::string(exporter::kButlerPath), SLASH_FAKE_BUTLER);
    Variable key(std::string(exporter::kButlerApiKey), "");
    Variable record("FAKE_BUTLER_LOG", log.string());
    Variable refuse("FAKE_BUTLER_FAIL", "");
    exporter::PublishOptions options;
    options.store = "itch";
    options.project = root;
    options.upload = true;
    options.build = false;

    // butler's key comes from the environment, and is checked before anything is built.
    exporter::PublishResult result = exporter::publish_game(options);
    REQUIRE_FALSE(result.ok);
    CHECK(result.json.find("error")->find("step")->as_string() == "setup");
    CHECK(contains(result.json.find("error")->find("message")->as_string(), "BUTLER_API_KEY"));
    key.set("test-key");

    // With no export there, nothing is pushed.
    result = exporter::publish_game(options);
    REQUIRE_FALSE(result.ok);
    CHECK(contains(result.json.find("error")->find("message")->as_string(),
                   "there's no export of \"mac-release\""));
    CHECK(result.json.find("pushed") == nullptr);

    // The outputs earlier exports left go to their channels.
    REQUIRE(slash::platform::create_directories(root + "/build/export/mac-release/Space Game.app")
                .ok());
    REQUIRE(slash::platform::write_file(
                root + "/build/export/mac-release/Space Game.app/Info.plist", "<plist/>")
                .ok());
    REQUIRE(slash::platform::create_directories(root + "/dist/pc").ok());
    REQUIRE(slash::platform::write_file(root + "/dist/pc/Space Game.exe", "MZ").ok());
    REQUIRE(slash::platform::write_file(root + "/dist/pc/game.pck", "data").ok());
    result = exporter::publish_game(options);
    REQUIRE_MESSAGE(result.ok, json::write(result.json));
    CHECK(result.json.find("upload")->as_bool());
    const json::Array& builds = result.json.find("builds")->as_array();
    REQUIRE(builds.size() == 2);
    CHECK(contains(builds[1].find("push")->find("tail")->as_string(),
                   "For channel `player/space-game:windows`: pushing 2 files"));
    std::istringstream lines(read(log));
    std::string mac;
    std::string pc;
    std::getline(lines, mac);
    std::getline(lines, pc);
    CHECK(contains(mac, "build/export/mac-release player/space-game:mac --userversion 1.2.0 "
                        "--if-changed 1"));
    CHECK(contains(pc, "dist/pc player/space-game:windows --userversion 1.2.0 --if-changed 2"));

    // A push itch.io refuses stops the publish, saying which builds went up before it.
    options.targets = {"pc", "mac-release"};
    refuse.set("");
    std::filesystem::remove_all(root + "/build/export/mac-release");
    result = exporter::publish_game(options);
    REQUIRE_FALSE(result.ok);
    CHECK(*result.json.find("pushed") == parse(R"(["player/space-game:windows"])"));
    refuse.set("the project is in draft");
    options.targets = {"pc"};
    result = exporter::publish_game(options);
    REQUIRE_FALSE(result.ok);
    const json::Value& error = *result.json.find("error");
    CHECK(error.find("step")->as_string() == "push");
    CHECK(error.find("exit_code")->as_int() == 1);
    CHECK(contains(error.find("tail")->as_string(), "the project is in draft"));
    CHECK(std::filesystem::exists(root + "/.slash/publish/itch-pc.log"));
    std::filesystem::remove_all(root);
    std::filesystem::remove(log);
}

TEST_CASE("publish.run plans without asking and uploads only with the capabilities") {
    const std::string root = "publish_command_project";
    write_game(root, R"({"itch": {"project": "player/space-game"}})");
    command::SessionOptions options;
    options.project_root = root;
    options.capabilities = {command::Capability::Read, command::Capability::Filesystem};
    Engine engine(options);
    command::Reply reply =
        engine.session->call("publish.run", parse(R"({"store": "itch", "presets": ["pc"]})"));
    REQUIRE_MESSAGE(reply.ok, reply.message);
    CHECK(field(*reply.value.find("builds"), "target") ==
          std::vector<std::string>{"player/space-game:windows"});
    CHECK(*reply.value.find("needs") ==
          parse(R"(["butler on PATH or in SLASH_BUTLER_PATH", "BUTLER_API_KEY"])"));

    // An upload builds and sends the game where players see it.
    reply = engine.session->call("publish.run", parse(R"({"store": "itch", "upload": true})"));
    REQUIRE_FALSE(reply.ok);
    CHECK(reply.code == command::code::kPermissionDenied);
    CHECK(reply.data.find("capability")->as_string() == "build");

    reply =
        engine.session->call("publish.run", parse(R"({"store": "itch", "presets": ["phone"]})"));
    REQUIRE_FALSE(reply.ok);
    CHECK(reply.code == command::code::kInvalidArgument);
    CHECK(reply.data.find("step")->as_string() == "options");
    CHECK(engine.session->call("publish.run", parse(R"({"store": "steem"})")).code ==
          command::code::kInvalidParams);
    std::filesystem::remove_all(root);
}

TEST_CASE("export presets say which Steam depot takes each build") {
    CHECK(exporter::steam_takes("windows"));
    CHECK(exporter::steam_takes("linux"));
    CHECK_FALSE(exporter::steam_takes("web"));

    const json::Value good = parse(R"({"format": 1, "presets": {"pc": {"platform": "windows"}},
        "stores": {"steam": {"app": "480", "depots": {"pc": "481", "macos": "482"},
                             "branch": "beta"}}})");
    CHECK(exporter::validate_presets(good).empty());
    const exporter::Stores stores = exporter::parse_stores(good);
    REQUIRE(stores.steam.has_value());
    CHECK(stores.steam->app == "480");
    CHECK(stores.steam->depots.at("macos") == "482");
    CHECK(stores.steam->branch == "beta");

    // Ids are digits, each depot names a desktop preset or platform, and builds can't be set live
    // on the default branch from here.
    const json::Value bad = parse(R"({"format": 1, "presets": {"phone": {"platform": "ios"}},
        "stores": {"steam": {"app": "app-480", "branch": "default",
                             "depots": {"pc": "481", "windows": "x1", "phone": "483",
                                        "web": "484"}}}})");
    std::vector<std::string> paths;
    for (const auto& error : exporter::validate_presets(bad)) {
        paths.push_back(error.path + " " + error.message);
    }
    CHECK(paths ==
          std::vector<std::string>{
              "/stores/steam/app expected the app id's digits, from Steamworks",
              "/stores/steam/depots/pc no preset or platform called \"pc\"",
              "/stores/steam/depots/phone Steam takes Windows, macOS and Linux builds, not ios "
              "ones",
              "/stores/steam/depots/web Steam takes Windows, macOS and Linux builds, not web ones",
              "/stores/steam/depots/windows expected the depot id's digits, from Steamworks",
              "/stores/steam/branch Steam sets the default branch live only from Steamworks: "
              "leave branch out and set the build live there"});
    CHECK_FALSE(exporter::validate_presets(parse(R"({"format": 1, "presets": {},
        "stores": {"steam": {"app": "480"}}})"))
                    .empty());
}

TEST_CASE("publish plans one Steam app build for every depot") {
    const std::string root = "publish_steam_plan_project";
    write_game(root, R"({"steam": {"app": "480", "branch": "beta",
        "depots": {"mac-release": "481", "pc": "482", "pi": "483"}}})");
    exporter::PublishOptions options;
    options.store = "steam";
    options.project = root;

    exporter::PublishResult plan = exporter::publish_game(options);
    REQUIRE_MESSAGE(plan.ok, json::write(plan.json));
    CHECK(plan.json.find("app")->as_string() == "480");
    CHECK(plan.json.find("branch")->as_string() == "beta");
    const json::Value& builds = *plan.json.find("builds");
    CHECK(field(builds, "preset") == std::vector<std::string>{"mac-release", "pc", "pi"});
    CHECK(field(builds, "depot") == std::vector<std::string>{"481", "482", "483"});
    CHECK(field(builds, "output") ==
          std::vector<std::string>{"build/export/mac-release", "dist/pc", "build/export/pi"});
    CHECK(plan.json.find("script")->find("path")->as_string() ==
          ".slash/publish/steam/app_build_480.vdf");
    const std::string script = plan.json.find("script")->find("text")->as_string();
    CHECK(contains(script, "\"AppID\" \"480\""));
    CHECK(contains(script, "\"Desc\" \"1.2.0\""));
    CHECK(contains(script, "\"SetLive\" \"beta\""));
    CHECK(contains(script, "\"482\"\n\t\t{\n\t\t\t\"FileMapping\"\n\t\t\t{\n\t\t\t\t\"LocalPath\" "
                           "\"dist/pc/*\""));
    CHECK(*plan.json.find("command") ==
          parse(R"(["steamcmd", "+login", "$STEAM_USERNAME", "+run_app_build",
                    ".slash/publish/steam/app_build_480.vdf", "+quit"])"));
    CHECK(*plan.json.find("needs") ==
          parse(R"(["steamcmd on PATH or in SLASH_STEAMCMD_PATH", "STEAM_USERNAME"])"));

    // Named presets take their depot by name, else by their platform's.
    write_game(root, R"({"steam": {"app": "480", "depots": {"windows": "482"}}})");
    options.targets = {"pc"};
    plan = exporter::publish_game(options);
    REQUIRE(plan.ok);
    CHECK(field(*plan.json.find("builds"), "depot") == std::vector<std::string>{"482"});
    CHECK(plan.json.find("branch") == nullptr);

    // What the plan can't publish.
    options.targets = {"phone"};
    CHECK(contains(json::write(exporter::publish_game(options).json), "which Steam doesn't take"));
    options.targets = {"pc", "windows"};
    CHECK(contains(json::write(exporter::publish_game(options).json),
                   "two builds go to Steam depot 482"));
    options.targets = {"mac-release"};
    CHECK(contains(json::write(exporter::publish_game(options).json),
                   "\\\"mac-release\\\" has no Steam depot"));
    write_game(root, R"({"itch": {"project": "player/space-game"}})");
    options.targets.clear();
    CHECK(contains(json::write(exporter::publish_game(options).json), "has no stores.steam"));
    std::filesystem::remove_all(root);
}

TEST_CASE("publish uploads exports to Steam through steamcmd") {
    const std::string root = "publish_steam_upload_project";
    write_game(root, R"({"steam": {"app": "480", "depots": {"mac-release": "481", "pc": "482"}}})");
    const std::filesystem::path log = std::filesystem::absolute(root + "-steamcmd.log");
    std::filesystem::remove(log);
    Variable steamcmd(std::string(exporter::kSteamcmdPath), root + "/no-steamcmd");
    Variable user(std::string(exporter::kSteamUser), "");
    Variable record("FAKE_STEAMCMD_LOG", log.string());
    Variable refuse("FAKE_STEAMCMD_FAIL", "");
    exporter::PublishOptions options;
    options.store = "steam";
    options.project = root;
    options.upload = true;
    options.build = false;

    // steamcmd and the account come from the environment, before anything is built.
    exporter::PublishResult result = exporter::publish_game(options);
    REQUIRE_FALSE(result.ok);
    CHECK(result.json.find("error")->find("step")->as_string() == "setup");
    CHECK(contains(result.json.find("error")->find("message")->as_string(),
                   "SLASH_STEAMCMD_PATH names"));
    steamcmd.set(SLASH_FAKE_STEAMCMD);
    result = exporter::publish_game(options);
    CHECK(contains(result.json.find("error")->find("message")->as_string(), "STEAM_USERNAME"));
    user.set("builder");

    // With no export there, nothing is uploaded.
    result = exporter::publish_game(options);
    REQUIRE_FALSE(result.ok);
    CHECK(contains(result.json.find("error")->find("message")->as_string(),
                   "there's no export of \"mac-release\""));

    // The outputs earlier exports left go up in one app build.
    REQUIRE(slash::platform::create_directories(root + "/build/export/mac-release/Space Game.app")
                .ok());
    REQUIRE(slash::platform::write_file(
                root + "/build/export/mac-release/Space Game.app/Info.plist", "<plist/>")
                .ok());
    REQUIRE(slash::platform::create_directories(root + "/dist/pc").ok());
    REQUIRE(slash::platform::write_file(root + "/dist/pc/Space Game.exe", "MZ").ok());
    REQUIRE(slash::platform::write_file(root + "/dist/pc/game.pck", "data").ok());
    result = exporter::publish_game(options);
    REQUIRE_MESSAGE(result.ok, json::write(result.json));
    CHECK(result.json.find("push")->find("build_id")->as_string() == "7654321");
    CHECK(std::filesystem::exists(root + "/.slash/publish/steam/app_build_480.vdf"));
    std::string line;
    std::istringstream lines(read(log));
    std::getline(lines, line);
    CHECK(line == "builder 480 481 1 482 2");

    // A build Steam refuses fails the publish, with steamcmd's words.
    refuse.set("Invalid depot ID");
    result = exporter::publish_game(options);
    REQUIRE_FALSE(result.ok);
    const json::Value& error = *result.json.find("error");
    CHECK(error.find("step")->as_string() == "push");
    CHECK(contains(error.find("tail")->as_string(), "Invalid depot ID"));
    CHECK(std::filesystem::exists(root + "/.slash/publish/steam.log"));
    std::filesystem::remove_all(root);
    std::filesystem::remove(log);
}
