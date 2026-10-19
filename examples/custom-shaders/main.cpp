// A game's own shaders, as Godot's canvas_item shaders: a .material.json file's "shader" names an
// .hlsl file with a fragment() function, a vertex() function or both, which read and write
// Godot's built-ins (COLOR, UV, VERTEX, TIME and the rest) and the material's params by name.
//
// main.scene.json has two sprites: the crate burns away with dissolve.material.json, whose
// shader discards the pixels where its noise is under amount, and the flag sways with
// wave.material.json, whose vertex() moves the flag's top corners with the time. This program
// writes their textures, checks colors with material.eval and frame.capture as an agent would,
// then edits a shader, with a mistake and without, and sees it reload.

#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/platform/file.hpp"
#include "slash/render/device.hpp"
#include "slash/render/image.hpp"
#include "slash/shader_compiler/shader_compiler.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace {

namespace command = slash::command;
namespace json = slash::json;
namespace render = slash::render;
namespace world = slash::world;

// Stops with the error if a call fails, so the output never hides one.
void check(const slash::Result<void>& result) {
    if (!result) {
        std::fprintf(stderr, "%s\n", result.error().message.c_str());
        std::exit(1);
    }
}

// A 16x16 PNG of two colored halves, left and right. Games ship PNG files made in an image
// editor or by a generator; these are made here so the example has no binary file.
void write_png(const char* path, std::array<std::uint8_t, 4> left,
               std::array<std::uint8_t, 4> right) {
    constexpr std::uint32_t kSize = 16;
    render::Image image{.width = kSize, .height = kSize, .pixels = {}};
    image.pixels.resize(std::size_t{kSize} * kSize * 4);
    for (std::uint32_t y = 0; y < kSize; ++y) {
        for (std::uint32_t x = 0; x < kSize; ++x) {
            const std::size_t at = ((std::size_t{y} * kSize) + x) * 4;
            for (std::size_t channel = 0; channel < 4; ++channel) {
                image.pixels[at + channel] = (x < kSize / 2 ? left : right).at(channel);
            }
        }
    }
    check(slash::platform::write_file(path, render::encode_png(image).value()));
    std::printf("wrote %s\n", path);
}

// Prints the call as an agent would make it, then the reply.
void call(command::Session& session, std::string_view name, std::string_view params) {
    const json::Value value = json::parse(params).value();
    std::printf("%.*s %s\n", static_cast<int>(name.size()), name.data(),
                json::write(value, json::Style::Compact).c_str());
    const command::Reply reply = session.call(name, value);
    std::puts(json::write(reply.ok ? reply.value : json::Value(reply.message), json::Style::Compact)
                  .c_str());
}

} // namespace

int main() {
    // The shader tools compile a game's shaders: slash-cli and slash-mcp install the compiler, as
    // this program does. Games can't compile them, so exports write each material's shader
    // compiled for the platform into the material file.
    slash::shader_compiler::install();

    write_png("crate.png", {160, 96, 48, 255}, {120, 72, 40, 255});
    write_png("flag.png", {255, 255, 255, 255}, {255, 255, 255, 255});

    world::ComponentRegistry components;
    world::register_builtin_components(components);
    command::Registry commands;
    command::register_builtin_commands(commands);
    command::SessionOptions options;
    options.capture_logs = false;
    command::Session session(components, commands, options);

    call(session, "scene.open", R"({"path": "main.scene.json"})");

    // material.eval runs the shader's fragment() on the CPU at texture coordinates, as the GPU
    // runs it. The crate's material_params set amount to 0.5: some of its cells are burned away
    // (discarded pixels come back as [0, 0, 0, 0]), some glow at the edge, the rest are whole.
    call(session, "material.eval",
         R"({"id": "crate", "uv": [[0.06, 0.06], [0.44, 0.06], [0.31, 0.31], [0.56, 0.06]]})");
    // With amount 0 nothing burns.
    call(session, "material.eval",
         R"({"id": "crate", "uv": [[0.06, 0.06], [0.44, 0.06]], "params": {"amount": 0}})");

    // frame.capture draws the window on the CPU, vertex() and all. At 0 seconds the flag hangs
    // straight; half a second later its top has swayed 8 pixels to the right.
    call(session, "frame.capture", R"({"points": [[70, 17], [84, 17], [84, 31]], "time": 0})");
    call(session, "frame.capture", R"({"points": [[70, 17], [84, 17], [84, 31]], "time": 0.5})");

    // Shaders reload when their files change. A mistake is reported with its file and line, and
    // the sprite is left out until it is fixed.
    check(slash::platform::write_file("shaders/wave.hlsl", R"(void fragment() {
    COLOR *= tnt;
}
)"));
    call(session, "material.eval", R"({"id": "flag", "uv": [[0.5, 0.5]]})");
    check(slash::platform::write_file("shaders/wave.hlsl", R"(void fragment() {
    COLOR.rgb *= tint.rgb * 0.5;
}
)"));
    call(session, "material.eval", R"({"id": "flag", "uv": [[0.5, 0.5]]})");
    call(session, "frame.capture", R"({"points": [[72, 24]], "time": 0.5})");
    return 0;
}
