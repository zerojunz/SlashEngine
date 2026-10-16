// Parent transforms: entities placed in another's space, as Godot's Node2D children.
//
// The scene is a boat in a 320x180 window. Its Transform2D lists a deck (a static body), a mast
// and a cannon as children, and the mast lists a flag, so they all move, turn and draw with the
// boat. A rock made after the boat draws over it. The agent reads where the flag is, sails and
// rocks the boat, lifts it over the rock, drops a barrel into it, tries to make the tree a loop
// and sinks the boat. Games ship PNG files made in an image editor; this example draws its
// textures in code, so the repository holds no images.

#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/platform/file.hpp"
#include "slash/render/image.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>

namespace {

namespace command = slash::command;
namespace json = slash::json;
namespace render = slash::render;
namespace world = slash::world;

// Writes an opaque texture in one color.
void write_texture(const std::string& path, std::uint32_t width, std::uint32_t height,
                   std::array<std::uint8_t, 3> color) {
    render::Image image{.width = width, .height = height, .pixels = {}};
    image.pixels.resize(std::size_t{width} * height * 4);
    for (std::size_t at = 0; at < image.pixels.size(); at += 4) {
        image.pixels[at] = color[0];
        image.pixels[at + 1] = color[1];
        image.pixels[at + 2] = color[2];
        image.pixels[at + 3] = 255;
    }
    if (!slash::platform::write_file(path, render::encode_png(image).value()).ok()) {
        std::fprintf(stderr, "can't write %s\n", path.c_str());
        std::exit(1);
    }
}

struct Agent {
    world::ComponentRegistry components;
    command::Registry commands;
    std::unique_ptr<command::Session> session;

    Agent() {
        world::register_builtin_components(components);
        command::register_builtin_commands(commands);
        command::SessionOptions options;
        options.capture_logs = false;
        session = std::make_unique<command::Session>(components, commands, options);
    }

    // Prints the call as an agent would make it, then the reply.
    void call(std::string_view name, std::string_view params) const {
        const json::Value value = json::parse(params).value();
        std::printf("%.*s %s\n", static_cast<int>(name.size()), name.data(),
                    json::write(value, json::Style::Compact).c_str());
        command::Reply reply = session->call(name, value);
        const json::Value out = reply.ok ? std::move(reply.value) : json::Value(reply.message);
        std::puts(json::write(out, json::Style::Compact).c_str());
    }
};

} // namespace

int main() {
    write_texture("boat.png", 48, 12, {150, 90, 50});
    write_texture("mast.png", 2, 24, {90, 60, 40});
    write_texture("flag.png", 10, 6, {220, 40, 40});
    write_texture("cannon.png", 12, 4, {40, 40, 50});
    write_texture("rock.png", 16, 16, {120, 120, 120});
    write_texture("barrel.png", 8, 8, {170, 120, 60});

    const Agent agent;
    agent.call("scene.open", R"({"path": "main.scene.json"})");

    std::puts("-- the flag is in the mast's space, and the mast in the boat's");
    agent.call("entity.get", R"({"id": "flag", "components": ["Transform2D"]})");

    std::puts("-- the boat draws, then its children in their order, then the rock made after it");
    const std::string_view drawn = R"({"ids": ["boat", "mast", "flag", "cannon", "rock"]})";
    agent.call("frame.summary", drawn);

    std::puts("-- the boat sails right and rocks; everything in it goes along");
    agent.call("component.set", R"({"id": "boat", "components": {
        "Transform2D": {"position": [160, 120], "rotation": 0.2}}})");
    agent.call("entity.get", R"({"id": "flag", "components": ["Transform2D"]})");
    agent.call("frame.summary", drawn);

    std::puts("-- the boat's z_index lifts its children over the rock too");
    agent.call("component.set", R"({"id": "boat", "components": {"Sprite2D": {"z_index": 1}}})");
    agent.call("frame.summary", drawn);

    std::puts("-- the deck is a body in the boat: a ray down finds it where the boat is now");
    agent.call("physics.raycast", R"({"from": [160, 0], "to": [160, 180]})");

    std::puts("-- a barrel dropped into the boat falls in the world and lands on the deck");
    agent.call("batch", R"({"commands": [
        {"command": "entity.create", "params": {"id": "barrel", "components": {
            "Transform2D": {"position": [0, -40]}, "Sprite2D": {"texture": "barrel.png"},
            "RigidBody2D": {}, "CollisionShape2D": {"size": [8, 8]}}}},
        {"command": "component.set", "params": {"id": "boat", "components": {
            "Transform2D": {"children": ["deck", "mast", "cannon", "barrel"]}}}}]})");
    agent.call("step", R"({"ticks": 90})");
    agent.call("entity.get", R"({"id": "barrel", "components": ["Transform2D"]})");

    std::puts("-- each child has one parent and is never its own ancestor");
    agent.call("component.set",
               R"({"id": "flag", "components": {"Transform2D": {"children": ["boat"]}}})");

    std::puts("-- the boat sinks with everything in it");
    agent.call("entity.destroy", R"({"id": "boat"})");
    agent.call("world.query", "{}");
    return 0;
}
