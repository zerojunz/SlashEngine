// Godot's precision build option: an engine configured with -DSLASH_PRECISION=double (the
// "double" preset) keeps positions in doubles (slash::real_t), so places thousands of kilometers
// from the origin keep their millimeters. This program shows what a build's precision allows:
// the step between neighboring positions at growing distances, a crate dropped onto a floor ten
// thousand kilometers out, and the render origin the 3D world is drawn around.
//
// expected.txt holds a single-precision build's output, which CI checks; a double build prints
// the second set of numbers in README.md.

#include "slash/core/math.hpp"
#include "slash/physics3d/physics3d.hpp"
#include "slash/scene3d/scene3d.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <cmath>
#include <cstdio>
#include <limits>

namespace {

namespace physics3d = slash::physics3d;
namespace scene3d = slash::scene3d;
namespace world = slash::world;
using slash::real_t;
using slash::Vec3;

// Where a crate dropped 3 meters above a floor at `at`, aimed 0.3 meters east and 0.2 north of
// the floor's middle, comes to rest, relative to that middle.
Vec3 drop_crate(Vec3 at) {
    world::ComponentRegistry components;
    world::register_builtin_components(components);
    world::World world(components);
    physics3d::install(world, {});

    const entt::entity floor = world.create("floor").value();
    world.ecs().emplace<world::Transform3D>(floor).position = at + Vec3{0.0F, -0.5F, 0.0F};
    world.ecs().emplace<world::StaticBody3D>(floor);
    world.ecs().emplace<world::CollisionShape3D>(floor).size = {20.0F, 1.0F, 20.0F};

    const entt::entity crate = world.create("crate").value();
    world.ecs().emplace<world::Transform3D>(crate).position = at + Vec3{0.3F, 3.0F, -0.2F};
    world.ecs().emplace<world::RigidBody3D>(crate);
    world.ecs().emplace<world::CollisionShape3D>(crate).size = {1.0F, 1.0F, 1.0F};

    for (int tick = 0; tick < 240; ++tick) {
        world.step(1.0 / 60.0);
    }
    return world.ecs().get<world::Transform3D>(crate).position - at;
}

} // namespace

int main() {
    std::printf("precision: %s (positions are %zu-bit)\n",
                sizeof(real_t) == 8 ? "double" : "single", sizeof(real_t) * 8);

    // How far apart two neighboring positions are, at each distance from the origin.
    std::printf("\nstep between neighboring positions:\n");
    for (const double meters : {1.0e3, 1.0e5, 1.0e7}) {
        const auto at = static_cast<real_t>(meters);
        const real_t next = std::nextafter(at, std::numeric_limits<real_t>::infinity());
        std::printf("  %8.0f km: %.6g mm\n", meters / 1000.0,
                    static_cast<double>(next - at) * 1000.0);
    }

    // The same drop at home and ten thousand kilometers out.
    std::printf("\na crate dropped 0.3 m east and 0.2 m north of a floor's middle rests at:\n");
    for (const double meters : {0.0, 1.0e7}) {
        const auto out = static_cast<real_t>(meters);
        const Vec3 rest = drop_crate({out, 0.0F, -out});
        std::printf("  %8.0f km out: x %.3f, y %.3f, z %.3f\n", meters / 1000.0,
                    static_cast<double>(rest.x), static_cast<double>(rest.y),
                    static_cast<double>(rest.z));
    }

    // The 3D world draws around a render origin near the camera, so what reaches the GPU in floats
    // stays near the origin.
    const Vec3 eye{static_cast<real_t>(1.0e7) + 300.25F, 1.8F, -5000.0F};
    const Vec3 origin = scene3d::render_origin_for(eye);
    std::printf("\na camera at (%.2f, %.2f, %.2f) draws around (%.0f, %.0f, %.0f)\n",
                static_cast<double>(eye.x), static_cast<double>(eye.y), static_cast<double>(eye.z),
                static_cast<double>(origin.x), static_cast<double>(origin.y),
                static_cast<double>(origin.z));
    return 0;
}
