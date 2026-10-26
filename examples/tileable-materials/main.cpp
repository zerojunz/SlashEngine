// A tileable material made once and shared: a cobblestone image generated and made to tile, a
// normal and a roughness map worked out from it, and two model recipes that name the material
// instead of carrying textures of their own. In the library both recipes name the one material,
// and copying either into a project brings the material and its maps along.
//
// The built-in test generator draws the stones, unevenly lit, so the example needs no key.

#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/library/commands.hpp"
#include "slash/platform/file.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <cstdio>
#include <string>
#include <string_view>

namespace {

namespace command = slash::command;
namespace json = slash::json;

// Prints the call as an agent would make it, then the reply.
json::Value call(command::Session& session, std::string_view name, std::string_view params) {
    const json::Value value = json::parse(params).value();
    std::printf("%.*s %s\n", static_cast<int>(name.size()), name.data(),
                json::write(value, json::Style::Compact).c_str());
    const command::Reply reply = session.call(name, value);
    std::puts(json::write(reply.ok ? reply.value : json::Value(reply.message), json::Style::Compact)
                  .c_str());
    return reply.ok ? reply.value : json::Value();
}

void show(const char* path) {
    std::printf("%s:\n%s", path, slash::platform::read_file(path).value().c_str());
}

} // namespace

int main() {
    slash::world::ComponentRegistry components;
    slash::world::register_builtin_components(components);
    command::Registry commands;
    command::register_builtin_commands(commands);
    slash::library::register_commands(commands);

    // The library would be ~/.slash/library; the example keeps its own.
    command::SessionOptions options;
    options.capture_logs = false;
    options.library_dir = "library";
    options.project_root = "game";
    command::Session game(components, commands, options);

    // The material: its image doesn't tile as drawn (image_seam), so it is made to (seam), and
    // the maps are copied beside the material file.
    const json::Value cobble =
        call(game, "asset.generate",
             R"({"type": "material", "prompt": "grey cobblestones", "generator": "test",
                 "width": 128, "tags": ["stone"],
                 "path": "materials/cobble.standard_material.json"})");
    show("game/materials/cobble.standard_material.json");

    // Two recipes name it: the wall as it is, the steps with its texture tiling twice as often
    // and a mossy variant tinted green. Both draw the same three textures.
    (void)slash::platform::create_directories("game/props");
    (void)slash::platform::write_file("game/props/wall.model.json", R"({
  "format": 1,
  "materials": {"cobble": "materials/cobble.standard_material.json"},
  "parts": [{"name": "wall", "shape": "box", "size": [3, 1.2, 0.4], "material": "cobble"}]
})");
    (void)slash::platform::write_file("game/props/steps.model.json", R"({
  "format": 1,
  "materials": {"cobble": {"extends": "materials/cobble.standard_material.json",
                           "uv1_scale": [2, 2, 2]}},
  "parts": [{"name": "step", "shape": "box", "size": [1.2, 0.2, 0.4], "material": "cobble",
             "array": {"count": 3, "position": [0, 0.2, -0.3]}}],
  "variants": {"mossy": {"materials": {"cobble": {"albedo_color": [0.6, 0.8, 0.5, 1]}}}}
})");
    call(game, "model.inspect", R"({"path": "props/steps.model.json::mossy"})");

    // In the library, both name the material by its ID, which lists them as its users.
    call(game, "asset.add",
         R"({"path": "props/wall.model.json", "description": "a cobblestone wall",
             "license": "CC0-1.0"})");
    call(game, "asset.add",
         R"({"path": "props/steps.model.json", "description": "cobblestone steps",
             "license": "CC0-1.0"})");
    call(game, "asset.info", R"({"id": ")" + cobble.find("id")->as_string() + R"("})");

    // A material is made of an image: a sound is refused.
    const json::Value beep =
        call(game, "asset.generate", R"({"type": "sound", "prompt": "beep", "generator": "test"})");
    call(game, "asset.generate",
         R"({"type": "material", "prompt": "beep", "image": ")" + beep.find("id")->as_string() +
             R"("})");
    return 0;
}
