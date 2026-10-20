#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/exporter/commands.hpp"
#include "slash/platform/file.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <SDL3/SDL_stdinc.h>
#include <doctest/doctest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace json = slash::json;
namespace command = slash::command;
namespace fs = std::filesystem;

namespace {

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? std::string() : parsed.error().message));
    return std::move(parsed).value();
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

struct Engine {
    slash::world::ComponentRegistry components;
    command::Registry commands;
    std::unique_ptr<command::Session> session;

    explicit Engine(const std::string& project) {
        slash::world::register_builtin_components(components);
        command::register_builtin_commands(commands);
        slash::exporter::register_api_commands(commands);
        session = std::make_unique<command::Session>(
            components, commands, command::SessionOptions{.project_root = project});
    }

    [[nodiscard]] json::Value search(std::string_view params) const {
        command::Reply reply = session->call("api.search", parse(params));
        REQUIRE_MESSAGE(reply.ok, reply.message);
        return std::move(reply.value);
    }
};

// A small engine: its public headers, a detail namespace and private members api.search leaves
// out, and examples, one of which doesn't include the header it seems to call.
void write_engine(const fs::path& engine) {
    write(engine / "export" / "CMakeLists.txt", "");
    write(engine / "engine" / "world" / "include" / "slash" / "world" / "world.hpp",
          R"cpp(#pragma once
namespace slash::world {
// The simulation state.
class World {
public:
    // The entity with this ID.
    int find(std::string_view id) const;
};
} // namespace slash::world
)cpp");
    write(engine / "engine" / "audio" / "include" / "slash" / "audio" / "audio.hpp",
          R"cpp(#pragma once
#include "slash/world/world.hpp"

namespace slash::audio {
// Plays a sound file on an entity.
void play_sound(world::World& world, std::string_view id, std::string_view path);

// How a sound loops.
struct LoopSettings {
    bool loop = false; // Starts again at the end.
    float offset = 0.0F;

private:
    int cache_ = 0;
};

class Mixer {
public:
    // Sets a bus's volume in decibels.
    void set_volume(std::string_view bus, float db);
    void set_volume(int bus, float db);

private:
    void mix();
};

namespace detail {
void hidden_helper();
} // namespace detail
} // namespace slash::audio

// Names a sound for the mixer.
#define SLASH_SOUND(name) name
)cpp");
    write(engine / "examples" / "steps" / "main.cpp", R"cpp(#include "slash/audio/audio.hpp"

int main() {
    slash::world::World world;
    // play_sound(world, "comment", "x.wav");
    const char* text = "play_sound(world)";
    slash::audio::play_sound(world, "hero", "sounds/step.wav");
    slash::audio::Mixer mixer;
    mixer.set_volume("Master", -6.0F);
    settings.find("loop");
    world.find("hero");
}
)cpp");
    write(engine / "examples" / "unrelated" / "main.cpp", R"cpp(#include "other.hpp"

int main() { play_sound(w, "a", "b"); }
)cpp");
}

void write_project(const fs::path& project) {
    write(project / "project.json",
          R"({"format": 1, "name": "Jumper", "gameplay": {"sources": ["src"]}})");
    write(project / "src" / "player.hpp", R"cpp(#pragma once

namespace game {
// The one the keys move.
struct Player {
    int lives = 3;
    // Jumps if on the ground.
    void jump();
};
} // namespace game
)cpp");
    write(project / "src" / "player.cpp", R"cpp(#include "player.hpp"

namespace game {
void Player::jump() {}
} // namespace game
)cpp");
    write(project / "src" / "game.cpp", R"cpp(#include "player.hpp"

void tick(game::Player& player) {
    player.jump();
}
)cpp");
}

} // namespace

TEST_CASE("api.search finds the engine's and the project's C++ API with examples that use it") {
    const fs::path root = fs::absolute("api_search_test");
    fs::remove_all(root);
    write_engine(root / "engine");
    write_project(root / "game");
    const ScopedVariable engine_dir("SLASH_ENGINE_DIR", (root / "engine").string());
    const Engine engine((root / "game").string());

    // Words for what it does find it, with its declaration and a verified example; comments,
    // strings and files that don't include its header aren't examples.
    CHECK(engine.search(R"({"query": "play a sound", "limit": 1})") == parse(R"j({"results": [{
        "name": "slash::audio::play_sound", "kind": "function",
        "signature": "void play_sound(world::World& world, std::string_view id, std::string_view path)",
        "doc": "Plays a sound file on an entity.",
        "at": "engine/audio/include/slash/audio/audio.hpp:6",
        "include": "slash/audio/audio.hpp",
        "examples": [{"at": "examples/steps/main.cpp:7",
                      "code": "slash::audio::play_sound(world, \"hero\", \"sounds/step.wav\");"}]
}]
})j"));

    // Overloads come together. A member's example calls it on something named like its class.
    const json::Value volume = engine.search(R"({"query": "Mixer::set_volume", "limit": 1})");
    CHECK(volume.find("results")->as_array().at(0) == parse(R"j({
        "name": "slash::audio::Mixer::set_volume", "kind": "function",
        "signature": "void set_volume(std::string_view bus, float db)",
        "overloads": ["void set_volume(int bus, float db)"],
        "doc": "Sets a bus's volume in decibels.",
        "at": "engine/audio/include/slash/audio/audio.hpp:20",
        "include": "slash/audio/audio.hpp",
        "examples": [{"at": "examples/steps/main.cpp:9",
                      "code": "mixer.set_volume(\"Master\", -6.0F);"}]
    })j"));
    const json::Value find = engine.search(R"({"query": "World::find", "limit": 1})");
    CHECK(*find.find("results")->as_array().at(0).find("examples") ==
          parse(R"([{"at": "examples/steps/main.cpp:11", "code": "world.find(\"hero\");"}])"));

    // A type lists its public fields and member functions.
    const json::Value mixer = engine.search(R"({"query": "LoopSettings", "limit": 1})");
    CHECK(mixer == parse(R"({"results": [{"name": "slash::audio::LoopSettings", "kind": "struct",
        "signature": "struct LoopSettings", "doc": "How a sound loops.",
        "at": "engine/audio/include/slash/audio/audio.hpp:9", "include": "slash/audio/audio.hpp",
        "fields": ["bool loop = false // Starts again at the end.", "float offset = 0.0F"]}]})"));
    CHECK(*engine.search(R"({"query": "Mixer", "kind": "type", "limit": 1})")
               .find("results")
               ->as_array()
               .at(0)
               .find("members") == parse(R"(["set_volume"])"));

    // Private members and detail namespaces are not the engine's API.
    CHECK(engine.search(R"({"query": "mix", "kind": "function"})")
              .find("results")
              ->as_array()
              .empty());
    CHECK(engine.search(R"({"query": "hidden_helper"})").find("results")->as_array().empty());

    // Macros, with their parameters.
    CHECK(engine.search(R"({"query": "SLASH_SOUND", "limit": 1, "examples": 0})") ==
          parse(R"j({"results": [{"name": "SLASH_SOUND", "kind": "macro",
              "signature": "SLASH_SOUND(name)", "doc": "Names a sound for the mixer.",
              "at": "engine/audio/include/slash/audio/audio.hpp:33",
              "include": "slash/audio/audio.hpp"
    }]
    })j"));

    // The project's own code: a function declared in a header and defined in a source file,
    // with the project's lines that call it.
    CHECK(engine.search(R"({"query": "jump", "in": ["project"]})") == parse(R"j({"results": [{
        "name": "game::Player::jump", "kind": "function", "signature": "void jump()",
        "doc": "Jumps if on the ground.", "at": "src/player.hpp:8", "defined": "src/player.cpp:4",
        "examples": [{"at": "src/game.cpp:4", "code": "player.jump();"}]
    }]
    })j"));
    CHECK(engine.search(R"({"query": "Player", "in": ["project"], "kind": "type"})") ==
          parse(R"j({"results": [{"name": "game::Player", "kind": "struct",
              "signature": "struct Player", "doc": "The one the keys move.",
              "at": "src/player.hpp:5", "fields": ["int lives = 3"], "members": ["jump"],
              "examples": [{"at": "src/game.cpp:3", "code": "void tick(game::Player& player) {"},
                           {"at": "src/player.cpp:4", "code": "void Player::jump() {}"}]}]})j"));
    CHECK(engine.search(R"({"query": "jump", "in": ["engine"]})")
              .find("results")
              ->as_array()
              .empty());

    // Nothing found: the closest name.
    CHECK(engine.search(R"({"query": "Mixxer"})") ==
          parse(R"({"did_you_mean": "Mixer", "results": []})"));

    // Params are checked like any command's.
    const command::Reply bad = engine.session->call("api.search", parse(R"({"query": ""})"));
    CHECK_FALSE(bad.ok);
}

TEST_CASE("api.search reads this engine's headers and examples") {
    const fs::path project = fs::absolute("api_search_engine");
    fs::remove_all(project);
    write(project / "project.json", R"({"format": 1, "name": "Empty"})");
    const Engine engine(project.string());
    const json::Value found = engine.search(R"({"query": "World::add_system", "limit": 1})");
    const json::Value& result = found.find("results")->as_array().at(0);
    CHECK(result.find("name")->as_string() == "slash::world::World::add_system");
    CHECK(result.find("include")->as_string() == "slash/world/world.hpp");
    REQUIRE(result.find("examples") != nullptr);
    CHECK(
        result.find("examples")->as_array().at(0).find("at")->as_string().starts_with("examples/"));
}
