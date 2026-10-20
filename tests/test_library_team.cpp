#include "slash/ai/config.hpp"
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/core/text.hpp"
#include "slash/library/commands.hpp"
#include "slash/library/library.hpp"
#include "slash/library/team.hpp"
#include "slash/platform/file.hpp"
#include "slash/render/image.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <SDL3/SDL_stdinc.h>
#include <doctest/doctest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace json = slash::json;
namespace command = slash::command;
namespace library = slash::library;
namespace fs = std::filesystem;

namespace {

json::Value parse(std::string_view text) {
    auto value = json::parse(text);
    REQUIRE_MESSAGE(value.ok(), std::string(text));
    return std::move(value).value();
}

void write(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    REQUIRE(slash::platform::write_file(path.string(), text).ok());
}

// An environment variable set for one test.
struct ScopedVariable {
    std::string name;
    ScopedVariable(std::string variable, const std::string& value) : name(std::move(variable)) {
        REQUIRE(SDL_setenv_unsafe(name.c_str(), value.c_str(), 1) == 0);
    }
    ScopedVariable(const ScopedVariable&) = delete;
    ScopedVariable& operator=(const ScopedVariable&) = delete;
    ScopedVariable(ScopedVariable&&) = delete;
    ScopedVariable& operator=(ScopedVariable&&) = delete;
    ~ScopedVariable() { SDL_unsetenv_unsafe(name.c_str()); }
};

// A teammate: a project of their own and the library on their machine, under root/name, with
// providers.json naming the team's library.
struct Teammate {
    fs::path root;
    slash::world::ComponentRegistry components;
    command::Registry commands;
    std::unique_ptr<command::Session> session;

    Teammate(const fs::path& base, std::string_view name, std::string_view providers,
             std::vector<command::Capability> capabilities = {command::Capability::Read,
                                                              command::Capability::Filesystem,
                                                              command::Capability::Network})
        : root(base / name) {
        write(root / "project" / "project.json", R"({"format": 1, "name": "Team test"})");
        if (!providers.empty()) {
            write(root / "project" / "providers.json", providers);
        }
        fs::create_directories(root / "library");
        slash::world::register_builtin_components(components);
        command::register_builtin_commands(commands);
        library::register_commands(commands);
        session = std::make_unique<command::Session>(
            components, commands,
            command::SessionOptions{.project_root = (root / "project").string(),
                                    .capabilities = std::move(capabilities),
                                    .capture_logs = false,
                                    .library_dir = (root / "library").string()});
    }

    [[nodiscard]] library::Library library() const {
        return library::Library((root / "library").string());
    }

    json::Value call(std::string_view name, std::string_view params) const {
        command::Reply reply = session->call(name, parse(params));
        REQUIRE_MESSAGE(reply.ok, reply.message);
        return reply.value;
    }

    command::Reply fail(std::string_view name, std::string_view params) const {
        command::Reply reply = session->call(name, parse(params));
        REQUIRE_FALSE(reply.ok);
        return reply;
    }
};

std::string team_providers(const fs::path& team) {
    json::Value providers;
    providers["library"]["team"] = team.generic_string();
    return json::write(providers);
}

std::string text_of(const json::Value& value, std::string_view key) {
    const json::Value* member = value.find(key);
    REQUIRE_MESSAGE(member != nullptr, std::string(key));
    return member->as_string();
}

} // namespace

TEST_CASE("providers.json's library names the team's folder or server") {
    auto config = slash::ai::parse_config(parse(R"({"library": {"team": "https://library.example",
        "token_env": "STUDIO_LIBRARY_TOKEN", "timeout_seconds": 5}})"));
    REQUIRE(config.ok());
    REQUIRE(config.value().library.has_value());
    CHECK(config.value().library->team == "https://library.example");
    CHECK(config.value().library->token_env == "STUDIO_LIBRARY_TOKEN");
    CHECK(config.value().library->timeout_seconds == 5);
    CHECK_FALSE(slash::ai::check_config(parse(R"({"library": {"folder": "x"}})")).empty());

    const auto environment = [](const std::string& name) {
        return name == "STUDIO_LIBRARY_TOKEN" ? std::string("secret") : std::string();
    };
    library::TeamOptions options;
    options.environment = environment;
    auto team = library::open_team(config.value(), options);
    REQUIRE(team.ok());
    REQUIRE(team.value() != nullptr);
    CHECK(team.value()->where() == "https://library.example");

    // Without the token or the network capability, a server can't be used.
    options.environment = [](const std::string& /*name*/) { return std::string(); };
    CHECK(library::open_team(config.value(), options).error().message ==
          "the team's library server https://library.example needs its token in "
          "STUDIO_LIBRARY_TOKEN");
    options.environment = environment;
    options.network = false;
    CHECK(library::open_team(config.value(), options).error().message ==
          "the team's library server https://library.example needs the network capability");

    // SLASH_LIBRARY_TEAM wins; a relative folder is the project's, and must exist.
    const fs::path root = fs::absolute("team_open");
    fs::remove_all(root);
    fs::create_directories(root / "project" / "shared");
    options.project_root = (root / "project").string();
    options.environment = [](const std::string& name) {
        return name == "SLASH_LIBRARY_TEAM" ? std::string("shared") : std::string();
    };
    team = library::open_team(config.value(), options);
    REQUIRE(team.ok());
    CHECK(team.value()->where() == (root / "project" / "shared").generic_string());
    options.environment = [](const std::string& name) {
        return name == "SLASH_LIBRARY_TEAM" ? std::string("unmounted") : std::string();
    };
    CHECK(library::open_team(config.value(), options).error().message ==
          "the team's library folder \"" + (root / "project" / "unmounted").generic_string() +
              "\" doesn't exist; is the share mounted?");

    // Nothing named, no team.
    options.environment = [](const std::string& /*name*/) { return std::string(); };
    team = library::open_team(slash::ai::Config{}, options);
    REQUIRE(team.ok());
    CHECK(team.value() == nullptr);
}

TEST_CASE("a team folder shares what teammates make, use and say about assets") {
    const fs::path root = fs::absolute("team_folder");
    fs::remove_all(root);
    const fs::path shared = root / "share";
    fs::create_directories(shared);
    const Teammate alice(root, "alice", team_providers(shared));
    const Teammate bob(root, "bob", team_providers(shared));
    const library::Library team(shared.string());

    // Alice's generated sprite goes to the team with the request that made it.
    const json::Value made = alice.call("asset.generate", R"({"prompt": "green slime sprite",
        "generator": "test", "width": 32, "tags": ["enemy"]})");
    CHECK(made.find("team_error") == nullptr);
    const std::string slime = text_of(made, "id");
    REQUIRE(team.get(slime).ok());
    const std::string full_slime = team.get(slime).value().id;
    CHECK_FALSE(team.requests_for(full_slime).empty());
    CHECK(team.requests_for(full_slime) == alice.library().requests_for(full_slime));

    // Bob asks for the same thing and gets hers with no model call; his library keeps a copy.
    const json::Value repeat = bob.call("asset.generate", R"({"prompt": "green slime sprite",
        "generator": "test", "width": 32, "tags": ["enemy"], "path": "art/slime.png"})");
    CHECK(text_of(repeat, "id") == slime);
    CHECK(repeat.find("reused") != nullptr);
    CHECK(repeat.find("team") != nullptr);
    CHECK(bob.library().get(slime).ok());
    CHECK(fs::exists(bob.root / "project" / "art" / "slime.png"));
    CHECK(bob.library().find_request(team.requests_for(full_slime).front()).has_value());

    // Alice's sound is found by Bob's search before it is on his machine, and asset.use brings it.
    alice.call("asset.generate", R"({"prompt": "coin chime", "type": "sound", "generator": "test",
        "seconds": 0.1})");
    const json::Value found = bob.call("asset.search", R"({"text": "coin chime"})");
    REQUIRE(found.find("assets")->as_array().size() == 1);
    const std::string coin = text_of(found.find("assets")->as_array()[0], "id");
    CHECK_FALSE(bob.library().get(coin).ok());
    bob.call("asset.use", R"({"id": ")" + coin + R"(", "path": "sfx/coin.wav"})");
    CHECK(bob.library().get(coin).ok());
    CHECK(fs::exists(bob.root / "project" / "sfx" / "coin.wav"));

    // An artist's file Alice adds is the team's too, with its license.
    slash::render::Image dot;
    dot.width = 2;
    dot.height = 1;
    dot.pixels = {255, 0, 0, 255, 0, 0, 255, 255};
    write(alice.root / "project" / "ui" / "dot.png", slash::render::encode_png(dot).value());
    const std::string added = text_of(alice.call("asset.add", R"({"path": "ui/dot.png",
        "description": "Red and blue dots", "license": "CC0-1.0", "author": "Mira"})"),
                                      "id");
    CHECK(team.get(added).value().license == "CC0-1.0");

    // Bob's verdict reaches the team, so Alice's search ranks by it and asset.info shows it.
    const json::Value judged = bob.call(
        "asset.feedback",
        R"({"id": ")" + slime + R"(", "verdict": "rejected", "reason": "too dark at night"})");
    CHECK(judged.find("rejected")->as_int() == 1);
    CHECK(team.get(slime).value().feedback.size() == 1);
    CHECK(alice.library().get(slime).value().feedback.empty());
    const json::Value ranked = alice.call("asset.search", R"({"text": "slime"})");
    CHECK(ranked.find("assets")->as_array()[0].find("rejected")->as_int() == 1);
    const json::Value info = alice.call("asset.info", R"({"id": ")" + slime + R"("})");
    CHECK(info.find("shared")->as_bool());
    CHECK(info.find("feedback")->as_array()[0].find("reason")->as_string() == "too dark at night");

    // A refined version goes to the team with the version it came from.
    const json::Value red =
        bob.call("asset.refine", R"({"id": ")" + slime + R"(", "prompt": "make it red"})");
    CHECK(team.get(text_of(red, "id")).value().parent == full_slime);

    // A teammate who generated before the team was set up shares with asset.sync.
    const Teammate carol(root, "carol", "");
    const json::Value early = carol.call("asset.generate", R"({"prompt": "blue gem",
        "generator": "test", "width": 8, "path": "art/gem.png"})");
    const std::string gem = text_of(early, "id");
    CHECK(early.find("team_error") == nullptr);
    CHECK_FALSE(team.get(gem).ok());
    CHECK(carol.fail("asset.sync", "{}").message.starts_with("no team library"));
    write(carol.root / "project" / "providers.json", team_providers(shared));
    const json::Value synced = carol.call("asset.sync", "{}");
    CHECK(synced ==
          parse(R"({"team": ")" + shared.generic_string() + R"(", "shared": [")" + gem + R"("]})"));
    CHECK(team.get(gem).ok());
    CHECK(carol.call("asset.sync", "{}").find("shared")->as_array().empty());
    // Bob's exact repeat of Carol's request finds it on the team.
    CHECK(bob.call("asset.generate", R"({"prompt": "blue gem", "generator": "test", "width": 8})")
              .find("team") != nullptr);

    // With the share gone, commands still work on the machine's library and say why the team
    // was left out.
    fs::remove_all(shared);
    const json::Value alone = alice.call("asset.search", R"({"text": "slime"})");
    CHECK(alone.find("assets")->as_array().size() == 1);
    CHECK(text_of(alone, "team_error") == "the team's library folder \"" + shared.generic_string() +
                                              "\" doesn't exist; is the share mounted?");
    CHECK(bob.fail("asset.info", R"({"id": "abcdef0123"})").message ==
          "/id: no asset with ID \"abcdef0123\" in the library, and the team's library failed: "
          "the team's library folder \"" +
              shared.generic_string() + "\" doesn't exist; is the share mounted?");
}

TEST_CASE("a library server shares assets with teammates who send its token") {
    const fs::path root = fs::absolute("team_server");
    fs::remove_all(root);
    auto server = library::TeamServer::start((root / "served").string(),
                                             {.host = "127.0.0.1", .port = 0, .token = "s3cret"});
    REQUIRE(server.ok());
    const std::string url = server.value()->url();
    CHECK(server.value()->token() == "s3cret");

    json::Value providers;
    providers["library"]["team"] = url;
    providers["library"]["token_env"] = "SLASH_TEST_LIBRARY_TOKEN";
    const ScopedVariable token("SLASH_TEST_LIBRARY_TOKEN", "s3cret");
    const Teammate alice(root, "alice", json::write(providers));
    const Teammate bob(root, "bob", json::write(providers));
    const library::Library served((root / "served").string());

    const json::Value made = alice.call("asset.generate", R"({"prompt": "wooden crate",
        "generator": "test", "width": 16, "tags": ["prop"]})");
    CHECK(made.find("team_error") == nullptr);
    const std::string crate = text_of(made, "id");
    REQUIRE(served.get(crate).ok());

    const json::Value repeat = bob.call("asset.generate", R"({"prompt": "wooden crate",
        "generator": "test", "width": 16, "tags": ["prop"]})");
    CHECK(text_of(repeat, "id") == crate);
    CHECK(repeat.find("team") != nullptr);
    CHECK(bob.library().read(bob.library().get(crate).value()).value() ==
          served.read(served.get(crate).value()).value());

    bob.call("asset.feedback", R"({"id": ")" + crate + R"(", "verdict": "accepted",
        "reason": "reads well"})");
    const json::Value info = alice.call("asset.info", R"({"id": ")" + crate + R"("})");
    CHECK(info.find("shared")->as_bool());
    CHECK(info.find("feedback")->as_array().size() == 1);
    // A check's result the same as its last is kept once.
    const library::Feedback check{.verdict = "accepted", .reason = "fits", .source = "asset.check"};
    auto team =
        library::open_team(slash::ai::load_config((alice.root / "project").string()).value(),
                           {.project_root = (alice.root / "project").string()});
    REQUIRE(team.ok());
    REQUIRE(team.value()->add_feedback(crate, check, true).ok());
    REQUIRE(team.value()->add_feedback(crate, check, true).ok());
    CHECK(served.get(crate).value().feedback.size() == 2);
    CHECK_FALSE(team.value()->add_feedback("0123456789ab", check, true).value().has_value());

    // A wrong token, or no network capability, leaves the team out and says why.
    const ScopedVariable wrong("SLASH_TEST_LIBRARY_TOKEN", "guess");
    const json::Value refused = alice.call("asset.search", R"({"text": "crate"})");
    CHECK(text_of(refused, "team_error").find("answered HTTP 401") != std::string::npos);
    const Teammate offline(root, "offline", json::write(providers),
                           {command::Capability::Read, command::Capability::Filesystem});
    CHECK(text_of(offline.call("asset.search", R"({"text": "crate"})"), "team_error") ==
          "the team's library server " + url + " needs the network capability");
    server.value()->stop();
}

TEST_CASE("a library server checks what teammates send it") {
    const fs::path root = fs::absolute("team_answers");
    fs::remove_all(root);
    fs::create_directories(root);
    const library::Library served(root.string());
    const auto ask = [&served](std::string_view op, std::string_view body) {
        return library::answer_team_request(served, op, parse(body));
    };
    CHECK(ask("list", "{}").status == 404);
    CHECK(ask("request", R"({"key": "abc"})").status == 400);
    CHECK(ask("get", R"({"id": "abcdef"})").body == parse(R"({"record": null})"));
    CHECK(ask("get", R"({"id": "xyz"})").status == 400);
    CHECK(ask("read", R"({"id": "abcdef"})").status == 404);
    CHECK(ask("feedback", R"({"id": "abcdef", "verdict": "maybe"})").status == 400);

    // Bytes the library can't read, or that aren't the format the record says, are refused.
    json::Value put;
    put["record"] =
        parse(R"({"id": "0000000000000000000000000000000000000000000000000000000000000000",
        "type": "image", "format": "png", "bytes": 9})");
    put["data"] = slash::base64_encode("not a png");
    CHECK(library::answer_team_request(served, "put", put).status == 400);
    put["data"] = "%%%";
    CHECK(library::answer_team_request(served, "put", put).status == 400);
    CHECK(ask("missing", R"({"ids": ["abc"]})").status == 400);
    CHECK(served.all().value().empty());
}
