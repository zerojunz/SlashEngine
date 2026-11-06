// Godot's precision build option: engines configured with SLASH_PRECISION=double keep positions
// in doubles (slash::real_t), and draw the 3D world around a render origin near the camera.

#include "slash/asset/assets.hpp"
#include "slash/canvas/canvas.hpp"
#include "slash/core/json.hpp"
#include "slash/core/math.hpp"
#include "slash/render/image.hpp"
#include "slash/scene3d/scene3d.hpp"
#include "slash/ui/ui.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

namespace asset = slash::asset;
namespace canvas = slash::canvas;
namespace json = slash::json;
namespace scene3d = slash::scene3d;
namespace world = slash::world;
using slash::real_t;
using slash::Vec3;

namespace {

constexpr bool kDouble = std::is_same_v<real_t, double>;

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE(parsed.ok());
    return std::move(parsed).value();
}

// A world with the built-in components and the canvas asset types, in a viewport of 64x64.
struct Scene {
    asset::Assets assets{{
        .read = [](std::string_view) -> slash::Result<std::string> {
            return slash::make_error(slash::ErrorCode::NotFound, "no files");
        },
        .stamp = [](std::string_view) -> std::optional<asset::FileStamp> { return std::nullopt; },
    }};
    world::ComponentRegistry components;
    world::World world{components};

    Scene() {
        world::register_builtin_components(components);
        canvas::install(world, assets);
        slash::ui::Settings ui;
        ui.viewport = {64.0F, 64.0F};
        slash::ui::set_settings(world, ui);
    }

    entt::entity create(std::string_view id, std::string_view components_json) {
        const entt::entity entity = world.create(id).value();
        const json::Value values = parse(components_json);
        for (const auto& [name, fields] : values.as_object()) {
            std::string path;
            const auto assigned = components.find(name)->assign(world, entity, fields, path);
            REQUIRE_MESSAGE(assigned.ok(), (assigned.ok() ? "" : assigned.error().message));
        }
        return entity;
    }

    // Moves every entity with a Transform3D by offset.
    void move_all(Vec3 offset) {
        for (const auto [entity, transform] : world.ecs().view<world::Transform3D>().each()) {
            transform.position = transform.position + offset;
        }
    }
};

} // namespace

TEST_CASE("positions are real_t: doubles in double-precision builds, as Godot's") {
#if defined(SLASH_REAL_T_IS_DOUBLE)
    CHECK(kDouble);
#else
    CHECK_FALSE(kDouble);
#endif
    CHECK(std::is_same_v<decltype(Vec3::x), real_t>);
    CHECK(std::is_same_v<decltype(slash::Vec2::x), real_t>);
    CHECK(std::is_same_v<decltype(slash::Quat::w), real_t>);
    // What the render device takes stays in floats.
    CHECK(std::is_same_v<decltype(slash::Vec3f::x), float>);
    CHECK(std::is_same_v<decltype(slash::Vec2f::x), float>);
    CHECK(sizeof(slash::Vec3f) == 3 * sizeof(float));

    // A place ten thousand kilometers out keeps its centimeters through a scene file only in
    // doubles: a float there is a whole meter apart from the next.
    Scene scene;
    const entt::entity probe =
        scene.create("probe", R"({"Transform3D": {"position": [10000000.25, 0, -0.5]}})");
    const json::Value saved =
        scene.components.find("Transform3D")->save(scene.world, probe).as_object().at("position");
    const double x = saved.as_array().at(0).as_double();
    if constexpr (kDouble) {
        CHECK(x == 10000000.25);
    } else {
        CHECK(x == 10000000.0);
    }
    CHECK(saved.as_array().at(2).as_double() == -0.5);
}

TEST_CASE("the render origin follows the camera by whole kilometers in double-precision builds") {
    const Vec3 eye{1536.0, -100.0, 5000000.5};
    if constexpr (kDouble) {
        CHECK(scene3d::render_origin_for(eye) == Vec3{2048.0, 0.0, 5000192.0});
        CHECK(scene3d::render_origin_for({511.0, -513.0, 0.0}) == Vec3{0.0, -1024.0, 0.0});
    } else {
        CHECK(scene3d::render_origin_for(eye) == Vec3{});
    }
    CHECK(scene3d::render_origin_for({}) == Vec3{});
}

TEST_CASE("a world far from the origin draws as it does near it, in double-precision builds") {
    // A lit, shadowed and fogged room seen from inside, with an omni light and a decal-free floor;
    // drawn again moved a billion meters out (a whole number of render origin grid cells, so its
    // places relative to the render origin are the same bits).
    const auto make = [](Scene& scene) {
        scene.create("camera",
                     R"({"Transform3D": {"position": [0, 1, 4]}, "Camera3D": {"far": 100}})");
        scene.create("env", R"({"WorldEnvironment": {"background_mode": "color",
            "background_color": [0.1, 0.2, 0.4, 1], "fog_enabled": true, "fog_density": 0.02,
            "fog_height": 0.5, "fog_height_density": 0.3}})");
        scene.create("floor", R"({"Transform3D": {"scale": [6, 0.2, 6]}, "MeshInstance3D": {},
                                  "BoxMesh": {}})");
        scene.create("cube", R"({"Transform3D": {"position": [0.3, 0.6, 0],
            "rotation": [0, 0.5, 0]}, "MeshInstance3D": {}, "BoxMesh": {},
            "StandardMaterial3D": {"albedo_color": [0.9, 0.4, 0.2, 1]}})");
        scene.create("lamp", R"({"Transform3D": {"position": [-1, 2, 1]},
                                 "OmniLight3D": {"shadow_enabled": true, "omni_range": 6}})");
        scene.create("sun", R"({"Transform3D": {"rotation": [-0.8, 0.4, 0]},
                                "DirectionalLight3D": {"light_energy": 0.4}})");
    };
    Scene near;
    make(near);
    Scene far;
    make(far);
    const real_t out = kDouble ? 1024.0 * 1024.0 * 1024.0 : 0.0;
    far.move_all({out, 0.0F, -out});

    const auto drawn_near = canvas::capture(near.world, near.assets);
    const auto drawn_far = canvas::capture(far.world, far.assets);
    REQUIRE(drawn_near.ok());
    REQUIRE(drawn_far.ok());
    CHECK(drawn_near.value().meshes == 2);
    CHECK(drawn_far.value().image.pixels == drawn_near.value().image.pixels);
}
