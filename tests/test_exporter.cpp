#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/exporter/commands.hpp"
#include "slash/exporter/export.hpp"
#include "slash/exporter/presets.hpp"
#include "slash/platform/file.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace json = slash::json;
namespace command = slash::command;
namespace exporter = slash::exporter;

namespace {

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? std::string() : parsed.error().message));
    return std::move(parsed).value();
}

// The message of the error at path, or "" if there is none.
std::string error_at(std::string_view presets, std::string_view path) {
    for (const auto& error : exporter::validate_presets(parse(presets))) {
        if (error.path == path) {
            return error.message;
        }
    }
    return {};
}

bool contains(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
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

} // namespace

TEST_CASE("export presets parse with defaults") {
    auto presets = exporter::parse_presets(parse(R"({
        "format": 1,
        "presets": {
            "mac": {"platform": "macos"},
            "win": {"platform": "windows", "output": "dist/win", "debug": true,
                    "windows": {"sign": true, "timestamp_url": ""}}
        }
    })"));
    REQUIRE(presets.ok());
    const exporter::Preset& mac = presets.value().at("mac");
    CHECK(mac.platform == "macos");
    CHECK(mac.output.empty());
    CHECK_FALSE(mac.debug);
    CHECK(mac.macos.codesign_identity == "-");
    CHECK_FALSE(mac.macos.notarize);
    const exporter::Preset& win = presets.value().at("win");
    CHECK(win.output == "dist/win");
    CHECK(win.debug);
    CHECK(win.windows.sign);
    CHECK(win.windows.timestamp_url.empty());
}

TEST_CASE("export presets report every error with its path") {
    CHECK(contains(error_at(R"({"format": 1, "presets": {"a": {"platform": "macso"}}})",
                            "/presets/a/platform"),
                   "did you mean \"macos\""));
    // Secrets belong in environment variables, and the error says so.
    CHECK(contains(error_at(R"({"format": 1, "presets": {"w": {"platform": "windows",
                                "windows": {"sign": true, "password": "hunter2"}}}})",
                            "/presets/w/windows/password"),
                   "environment variables"));
    CHECK(
        contains(error_at(R"({"format": 1, "presets": {"a": {"platform": "web", "outptu": "x"}}})",
                          "/presets/a/outptu"),
                 "did you mean \"output\""));
    CHECK(contains(error_at(R"({"presets": {}})", ""), "\"format\""));

    // Rules beyond the schema.
    CHECK(contains(error_at(R"({"format": 1, "presets": {"a": {"platform": "linux",
                                "output": "../elsewhere"}}})",
                            "/presets/a/output"),
                   "inside the project"));
    CHECK(contains(error_at(R"({"format": 1, "presets": {"w": {"platform": "windows",
                                "macos": {"notarize": false}}}})",
                            "/presets/w/macos"),
                   "only a macos preset"));
    CHECK(contains(error_at(R"({"format": 1, "presets": {"m": {"platform": "macos",
                                "macos": {"notarize": true}}}})",
                            "/presets/m/macos/notarize"),
                   "Developer ID"));
    CHECK(contains(error_at(R"({"format": 1, "presets": {"m": {"platform": "macos",
                                "macos": {"entitlements": "/etc/app.plist"}}}})",
                            "/presets/m/macos/entitlements"),
                   "inside the project"));
    CHECK(contains(error_at(R"({"format": 1, "presets": {"w": {"platform": "windows",
                                "windows": {"timestamp_url": "ftp://example.com"}}}})",
                            "/presets/w/windows/timestamp_url"),
                   "http"));
    CHECK(contains(error_at(R"({"format": 1, "presets": {"bad name": {"platform": "web"}}})",
                            "/presets/bad name"),
                   "letters, digits"));
    CHECK(contains(error_at(R"({"format": 1, "presets": {"linux": {"platform": "web"}}})",
                            "/presets/linux/platform"),
                   "named after a platform"));

    CHECK(exporter::validate_presets(parse(R"json({"format": 1, "presets": {
        "store": {"platform": "macos", "output": "dist/mac",
                  "macos": {"codesign_identity": "Developer ID Application: Example (ABCDE12345)",
                            "entitlements": "macos/game.entitlements", "notarize": true}}
    }})json"))
              .empty());
}

TEST_CASE("presets fall back to platform defaults and suggest names") {
    auto presets = exporter::parse_presets(
        parse(R"({"format": 1, "presets": {"mac-store": {"platform": "macos"}}})"));
    REQUIRE(presets.ok());
    auto named = exporter::find_preset(presets.value(), "mac-store");
    REQUIRE(named.ok());
    CHECK(named.value().platform == "macos");
    auto defaults = exporter::find_preset(presets.value(), "linux");
    REQUIRE(defaults.ok());
    CHECK(defaults.value().name == "linux");
    CHECK(defaults.value().platform == "linux");

    auto misspelled = exporter::find_preset(presets.value(), "mac-stor");
    REQUIRE_FALSE(misspelled.ok());
    CHECK(contains(misspelled.error().message, "did you mean \"mac-store\""));
    auto unknown = exporter::find_preset(presets.value(), "playstation");
    REQUIRE_FALSE(unknown.ok());
    CHECK(contains(unknown.error().message, "macos, windows, linux or web"));
}

TEST_CASE("presets load from the project folder") {
    const std::filesystem::path folder = "exporter_presets_project";
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);
    auto none = exporter::load_presets(folder.string());
    REQUIRE(none.ok());
    CHECK(none.value().empty());

    REQUIRE(slash::platform::write_file((folder / "export_presets.json").string(),
                                        R"({"format": 1, "presets": {"a": {"platform": "wbe"}}})")
                .ok());
    auto broken = exporter::load_presets(folder.string());
    REQUIRE_FALSE(broken.ok());
    CHECK(contains(broken.error().message, "export_presets.json /presets/a/platform"));

    // An export reports every preset error before it builds anything.
    REQUIRE(slash::platform::write_file(
                (folder / "project.json").string(),
                R"({"format": 1, "name": "Presets", "main_scene": "main.scene.json"})")
                .ok());
    exporter::ExportOptions options;
    options.target = "linux";
    options.project = folder.string();
    const exporter::ExportResult result = exporter::export_game(options);
    CHECK_FALSE(result.ok);
    const json::Value* error = result.json.find("error");
    REQUIRE(error != nullptr);
    CHECK(error->find("step")->as_string() == "validate");
    REQUIRE(error->find("errors") != nullptr);
    CHECK(error->find("errors")->as_array()[0].find("file")->as_string() == "export_presets.json");
    CHECK_FALSE(std::filesystem::exists(folder / ".slash"));
    std::filesystem::remove_all(folder);
}

TEST_CASE("export.run checks its request before building") {
    command::SessionOptions options;
    options.project_root = SLASH_TEST_DATA_DIR "/game";
    Engine engine(options);
    const command::Command* run = engine.commands.find("export.run");
    REQUIRE(run != nullptr);
    CHECK(run->capabilities == std::vector<command::Capability>{command::Capability::Filesystem,
                                                                command::Capability::Build});

    command::Reply reply = engine.session->call("export.run", parse(R"({"preset": "macso"})"));
    REQUIRE_FALSE(reply.ok);
    CHECK(reply.code == command::code::kInvalidArgument);
    CHECK(contains(reply.message, "did you mean \"macos\""));
    CHECK(reply.data.find("step")->as_string() == "options");

    reply = engine.session->call("export.run", parse(R"({"preset": "linux", "tset": true})"));
    REQUIRE_FALSE(reply.ok);
    CHECK(reply.code == command::code::kInvalidParams);

    // Building runs compilers, so it needs its own capability.
    options.capabilities = {command::Capability::Read, command::Capability::Filesystem};
    Engine limited(options);
    reply = limited.session->call("export.run", parse(R"({"preset": "linux"})"));
    REQUIRE_FALSE(reply.ok);
    CHECK(reply.code == command::code::kPermissionDenied);
    CHECK(contains(reply.message, "build"));
}
