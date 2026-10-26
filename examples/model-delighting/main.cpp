// Scanned and generated models come with the light they were captured under painted into their
// albedo textures: the top of a scanned crate is bright, its underside dark, and in a game's own
// lights it looks lit twice. model.cleanup's "delight" takes that light back out, as Unity's
// De-Lighting tool does for scans: it works out how much light reached each texel from the
// model's own shape (the way each surface faces and how much of the sky its corners see), fits
// the light that best explains the texture, and divides it out, keeping the albedo's pattern.
//
// The crate here is a cube whose six faces share one texture, a gray checker lit from above and
// to the right. Before, its top is nearly twice as bright as its underside; after, every face
// matches within a code, the checker is kept and the reply names the light it found.

#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/core/math.hpp"
#include "slash/platform/file.hpp"
#include "slash/render/image.hpp"
#include "slash/scene3d/model.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace {

namespace command = slash::command;
namespace json = slash::json;
namespace render = slash::render;

// Prints the call as an agent would make it, then the reply.
json::Value call(command::Session& session, std::string_view name, std::string_view params) {
    const json::Value value = json::parse(params).value();
    std::printf("%.*s %s\n", static_cast<int>(name.size()), name.data(),
                json::write(value, json::Style::Compact).c_str());
    const command::Reply reply = session.call(name, value);
    const json::Value shown = reply.ok ? reply.value : json::Value(reply.message);
    std::puts(json::write(shown, json::Style::Compact).c_str());
    return shown;
}

struct Face {
    const char* name;
    std::array<float, 3> normal;
};

// Each face samples a 32 by 32 cell of a 96 by 64 texture: three cells across, two down.
constexpr std::array<Face, 6> kFaces{{{"right", {1, 0, 0}},
                                      {"left", {-1, 0, 0}},
                                      {"top", {0, 1, 0}},
                                      {"bottom", {0, -1, 0}},
                                      {"front", {0, 0, 1}},
                                      {"back", {0, 0, -1}}}};

std::string number(float value) {
    std::array<char, 32> text{};
    std::snprintf(text.data(), text.size(), "%.4f", static_cast<double>(value));
    return text.data();
}

// A linear value as an sRGB color code, with the engine's own exp and ln so the bytes are the same
// everywhere.
std::uint8_t srgb_code(float linear) {
    const float c = linear <= 0.0031308F ? linear * 12.92F
                                         : (1.055F * slash::exp(slash::ln(linear) / 2.4F)) - 0.055F;
    return static_cast<std::uint8_t>(std::floor((c * 255.0F) + 0.5F));
}

// The scan: a cube one meter across and its texture, the checker lit by an ambient part and
// light from above and to the right, in linear light as a camera captures it.
void write_scan() {
    const std::array<float, 3> sun{0.3F, 1.0F, 0.2F};
    const float sun_length = std::sqrt((sun[0] * sun[0]) + (sun[1] * sun[1]) + (sun[2] * sun[2]));
    render::Image texture;
    texture.width = 96;
    texture.height = 64;
    texture.pixels.assign(static_cast<std::size_t>(96) * 64 * 4, 255);
    std::string obj = "mtllib crate.mtl\nusemtl Paint\n";
    for (std::size_t f = 0; f < kFaces.size(); ++f) {
        const auto& n = kFaces[f].normal;
        // Two axes across the face, so its corners go round it.
        std::array<float, 3> a{n[1] != 0 ? 1.0F : 0.0F, n[1] == 0 ? 1.0F : 0.0F, 0.0F};
        if (n[2] != 0) {
            a = {1.0F, 0.0F, 0.0F};
        }
        const std::array<float, 3> b{(n[1] * a[2]) - (n[2] * a[1]), (n[2] * a[0]) - (n[0] * a[2]),
                                     (n[0] * a[1]) - (n[1] * a[0])};
        const float u0 = static_cast<float>(f % 3) / 3.0F;
        const float v0 = f < 3 ? 0.0F : 0.5F;
        const std::array<std::array<float, 2>, 4> corners{{{-1, -1}, {1, -1}, {1, 1}, {-1, 1}}};
        for (const auto& c : corners) {
            obj += "v " + number((n[0] + (a[0] * c[0]) + (b[0] * c[1])) * 0.5F) + " " +
                   number((n[1] + (a[1] * c[0]) + (b[1] * c[1])) * 0.5F) + " " +
                   number((n[2] + (a[2] * c[0]) + (b[2] * c[1])) * 0.5F) + "\n";
            // OBJ's v runs up the image, so the texture's rows are turned over.
            obj += "vt " + number(u0 + ((c[0] + 1.0F) / 6.0F)) + " " +
                   number(1.0F - (v0 + ((c[1] + 1.0F) / 4.0F))) + "\n";
        }
        obj += "vn " + number(n[0]) + " " + number(n[1]) + " " + number(n[2]) + "\n";
        const std::size_t base = (f * 4) + 1;
        const auto corner = [&](std::size_t k) {
            return std::to_string(base + k) + "/" + std::to_string(base + k) + "/" +
                   std::to_string(f + 1);
        };
        obj += "f " + corner(0) + " " + corner(1) + " " + corner(2) + " " + corner(3) + "\n";
        const float lit =
            0.6F + (0.35F * ((n[0] * sun[0]) + (n[1] * sun[1]) + (n[2] * sun[2])) / sun_length);
        for (std::uint32_t y = 0; y < 32; ++y) {
            for (std::uint32_t x = 0; x < 32; ++x) {
                const float albedo = ((x / 4) + (y / 4)) % 2 == 0 ? 0.55F : 0.4F;
                const std::uint8_t code = srgb_code(albedo * lit);
                const std::size_t at =
                    ((static_cast<std::size_t>(((f / 3) * 32) + y) * 96) + ((f % 3) * 32) + x) * 4;
                texture.pixels[at] = code;
                texture.pixels[at + 1] = code;
                texture.pixels[at + 2] = code;
            }
        }
    }
    (void)slash::platform::create_directories("source");
    (void)slash::platform::write_file("source/crate.obj", obj);
    (void)slash::platform::write_file("source/crate.mtl",
                                      "newmtl Paint\nKd 1 1 1\nmap_Kd crate.png\n");
    (void)slash::platform::write_file("source/crate.png", render::encode_png(texture).value());
}

// Each face's mean color code away from its edges, and its light and dark checks'.
void print_faces(const char* title, const render::Image& image) {
    std::printf("%s\n", title);
    for (std::size_t f = 0; f < kFaces.size(); ++f) {
        std::array<std::uint32_t, 2> sum{};
        std::array<std::uint32_t, 2> count{};
        for (std::uint32_t y = 4; y < 28; ++y) {
            for (std::uint32_t x = 4; x < 28; ++x) {
                const std::size_t at =
                    ((static_cast<std::size_t>(((f / 3) * 32) + y) * 96) + ((f % 3) * 32) + x) * 4;
                const std::size_t check = ((x / 4) + (y / 4)) % 2;
                sum[check] += image.pixels[at];
                ++count[check];
            }
        }
        std::printf("  %-6s mean %3u  light %3u  dark %3u\n", kFaces[f].name,
                    (sum[0] + sum[1]) / (count[0] + count[1]), sum[0] / count[0],
                    sum[1] / count[1]);
    }
}

render::Image read_png(const std::string& path) {
    return render::decode_image(slash::platform::read_file(path).value()).value();
}

// The albedo texture a .glb file holds inside it.
render::Image albedo_of(const std::string& path) {
    const std::string glb = slash::platform::read_file(path).value();
    const auto model = slash::scene3d::load_model(glb, path, [](std::string_view file) {
        return slash::platform::read_file(std::string(file));
    });
    const auto& material = model.value().materials[0];
    return model.value().images[static_cast<std::size_t>(material.albedo_image)].image;
}

} // namespace

int main() {
    slash::world::ComponentRegistry components;
    slash::world::register_builtin_components(components);
    command::Registry commands;
    command::register_builtin_commands(commands);
    command::SessionOptions options;
    options.capture_logs = false;
    command::Session session(components, commands, options);

    write_scan();
    print_faces("source/crate.png, the scan's texture:", read_png("source/crate.png"));

    call(session, "model.cleanup",
         R"({"path": "source/crate.obj", "out": "props/crate.glb", "delight": 1})");

    print_faces("props/crate.glb's texture, delit:", albedo_of("props/crate.glb"));

    // Half strength takes out half the light, for a texture whose shading is partly the artist's.
    call(session, "model.cleanup",
         R"({"path": "source/crate.obj", "out": "props/crate_half.glb", "delight": 0.5})");
    print_faces("props/crate_half.glb's texture:", albedo_of("props/crate_half.glb"));

    // A texture that isn't there is named, at the param that needed it.
    (void)slash::platform::write_file("source/crate.mtl",
                                      "newmtl Paint\nKd 1 1 1\nmap_Kd missing.png\n");
    call(session, "model.cleanup",
         R"({"path": "source/crate.obj", "out": "props/crate.glb", "delight": 1})");
    return 0;
}
