// Gaussian splats: a captured boulder in a scene of meshes (research, software device only).
//
// Scenery captured from photos with 3D Gaussian Splatting (Postshot, Polycam, Luma or nerfstudio's
// splatfacto) comes as a .ply file of splats: each a small, see-through 3D Gaussian with a place,
// a size along three axes, a turn, an opacity and a color that changes with the side it is seen
// from (spherical harmonics). A GaussianSplat3D names such a file; the software device blends its
// splats over the opaque meshes, nearest first, and a mesh in front hides them.
//
// Games ship captures made with those tools; this example makes its boulder in code, so the
// repository holds no binary files: 1500 splats on a squashed sphere, gray stone below and moss
// on top, the moss wet so it shines toward the camera (a first-degree spherical harmonic). It
// opens a scene with a floor, a sun and a white post, puts the boulder behind the post, and asks
// frame.capture for colors: the moss, the stone, the post over the boulder and the sky. Then it
// turns the shine off with sh_degree 0, moves the boulder in front of the post, and fades it.
// Splats overlap many deep, so a capture stays solid until its opacity is low.

#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/core/math.hpp"
#include "slash/platform/file.hpp"
#include "slash/render/splats.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <cmath>
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

constexpr float kSh0 = 0.28209479177387814F;
constexpr int kSplats = 1500;
constexpr float kGoldenAngle = 2.39996323F;

// The boulder: splats over a sphere squashed to 0.7 by 0.45 by 0.6 meters, each 5 cm across,
// stone below and moss above, with a little of each one's color varied by a hash of its number.
render::GaussianSplats boulder() {
    render::GaussianSplats out;
    out.sh_degree = 1;
    for (int i = 0; i < kSplats; ++i) {
        const float y = 1.0F - ((2.0F * (static_cast<float>(i) + 0.5F)) / kSplats);
        const float ring = std::sqrt(1.0F - (y * y));
        const slash::Vec2f turn = slash::cos_sin(static_cast<float>(i) * kGoldenAngle);
        out.positions.push_back({0.7F * ring * turn.x, 0.45F * y, 0.6F * ring * turn.y});
        out.scales.push_back({0.05F, 0.05F, 0.05F});
        out.rotations.push_back({1.0F, 0.0F, 0.0F, 0.0F});
        out.opacities.push_back(0.95F);
        const std::uint32_t hash = static_cast<std::uint32_t>(i) * 2654435761U;
        const float vary = static_cast<float>((hash >> 16) & 255U) / 255.0F * 0.08F;
        const bool moss = y > 0.3F;
        const float red = (moss ? 0.30F : 0.52F) + vary;
        const float green = (moss ? 0.55F : 0.50F) + vary;
        const float blue = (moss ? 0.20F : 0.46F) + vary;
        // Degree 0, then degree 1's three coefficients: wet moss is brighter seen from +z, where
        // the camera is (the trainer's term is C1 * z * coefficient 2, z toward the splat).
        for (const float color : {red, green, blue}) {
            out.sh.push_back((color - 0.5F) / kSh0);
        }
        for (int k = 1; k < 4; ++k) {
            for (int channel = 0; channel < 3; ++channel) {
                out.sh.push_back(moss && k == 2 ? -0.4F : 0.0F);
            }
        }
    }
    return out;
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
    [[nodiscard]] json::Value call(std::string_view name, std::string_view params) const {
        const json::Value value = json::parse(params).value();
        std::printf("%.*s %s\n", static_cast<int>(name.size()), name.data(),
                    json::write(value, json::Style::Compact).c_str());
        command::Reply reply = session->call(name, value);
        json::Value out = reply.ok ? std::move(reply.value) : json::Value(reply.message);
        std::puts(json::write(out, json::Style::Compact).c_str());
        return out;
    }
};

// The moss, the stone, the post over the boulder's right side and the sky.
constexpr std::string_view kPoints = R"({"points": [[78, 40], [72, 52], [94, 48], [80, 10]]})";

} // namespace

int main() {
    const std::string file = render::write_splat_ply(boulder());
    if (!slash::platform::write_file("boulder.ply", file).ok()) {
        std::fputs("can't write boulder.ply\n", stderr);
        return 1;
    }
    std::printf("wrote boulder.ply: %d splats of degree 1, %zu bytes\n", kSplats, file.size());
    const Agent agent;
    (void)agent.call("scene.open", R"({"path": "main.scene.json"})");

    std::puts("-- the captured boulder behind the post: moss, stone, the post in front, the sky");
    (void)agent.call("entity.create", R"({"id": "boulder", "components": {
        "GaussianSplat3D": {"splats": "boulder.ply"},
        "Transform3D": {"position": [0, 0.5, 0]}}})");
    (void)agent.call("frame.capture", kPoints);

    std::puts("-- sh_degree 0 drops the wet moss's shine: the moss is duller, the stone the same");
    (void)agent.call("component.set", R"({"id": "boulder", "components": {
        "GaussianSplat3D": {"sh_degree": 0}}})");
    (void)agent.call("frame.capture", kPoints);

    std::puts("-- moved in front of the post, the boulder hides it; splats overlap many deep, so "
              "the post shows through only at a faint opacity of 0.05");
    (void)agent.call("component.set", R"({"id": "boulder", "components": {
        "Transform3D": {"position": [0, 0.5, 2]}}})");
    (void)agent.call("frame.capture", R"({"points": [[94, 50]]})");
    (void)agent.call("component.set", R"({"id": "boulder", "components": {
        "GaussianSplat3D": {"opacity": 0.05}}})");
    (void)agent.call("frame.capture", R"({"points": [[94, 50]]})");
    return 0;
}
