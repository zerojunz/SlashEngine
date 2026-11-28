#include "slash/asset/assets.hpp"
#include "slash/canvas/canvas.hpp"
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/render/device.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/skeleton2d.hpp"
#include "slash/world/tree2d.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace slash;

namespace {

constexpr float kQuarterTurn = std::numbers::pi_v<float> / 2.0F;

json::Value parse(std::string_view text) {
    auto value = json::parse(text);
    REQUIRE_MESSAGE(value.ok(), text);
    return std::move(value).value();
}

// An arm: a skeleton at (100, 100) with an upper bone and a lower bone 50 pixels along it, and
// a strip of skin 100 pixels long and 20 wide over both, beside the bones under the skeleton.
struct Arm {
    world::ComponentRegistry components;
    world::World world{components};
    entt::entity skeleton;
    entt::entity upper;
    entt::entity lower;
    entt::entity skin;

    Arm() {
        world::register_builtin_components(components);
        skin = create("skin", R"({"Transform2D": {}, "Polygon2D": {"polygon":
            [[0, -10], [50, -10], [100, -10], [100, 10], [50, 10], [0, 10]]}})");
        lower = create("lower", R"({"Transform2D": {"position": [50, 0]},
            "Bone2D": {"rest_position": [50, 0], "length": 50}})");
        upper = create("upper", R"({"Transform2D": {"children": ["lower"]}, "Bone2D": {}})");
        skeleton = create("arm", R"({"Transform2D": {"position": [100, 100],
            "children": ["upper", "skin"]}, "Skeleton2D": {}})");
    }

    entt::entity create(std::string_view id, std::string_view components_json) {
        const entt::entity entity = world.create(id).value();
        const json::Value values = parse(components_json);
        for (const auto& [name, fields] : values.as_object()) {
            std::string path;
            const auto assigned = components.find(name)->assign(world, entity, fields, path);
            REQUIRE_MESSAGE(assigned.ok(), name, path, ": ",
                            assigned.ok() ? "" : assigned.error().message);
        }
        return entity;
    }

    void weigh(std::vector<world::BoneWeights2D> weights) {
        world::Polygon2D& polygon = world.ecs().get<world::Polygon2D>(skin);
        polygon.skeleton = skeleton;
        polygon.bone_weights = std::move(weights);
    }

    [[nodiscard]] std::optional<world::Skin2D> skinned() const {
        const world::Polygon2D& polygon = world.ecs().get<const world::Polygon2D>(skin);
        return world::skin_2d(world, world::Tree2D(world), skin, polygon.polygon);
    }
    // The skinned points, or none and a problem saying it isn't skinned.
    [[nodiscard]] world::Skin2D bent() const {
        return skinned().value_or(
            world::Skin2D{.points = {}, .path = {}, .problem = "not skinned"});
    }
};

void check_near(Vec2 got, Vec2 expected) {
    INFO("got [", got.x, ", ", got.y, "], expected [", expected.x, ", ", expected.y, "]");
    CHECK(std::abs(static_cast<double>(got.x - expected.x)) < 1e-3);
    CHECK(std::abs(static_cast<double>(got.y - expected.y)) < 1e-3);
}

} // namespace

TEST_CASE("a skinned Polygon2D's points move as its bones move from rest") {
    Arm arm;
    // Not skinned: no skeleton, then no weights.
    CHECK_FALSE(arm.skinned().has_value());
    arm.weigh({});
    CHECK_FALSE(arm.skinned().has_value());

    // The first three points follow the lower bone, the last three the upper.
    arm.weigh({{.bone = arm.lower, .weights = {0, 0.5F, 1, 1, 0.5F}},
               {.bone = arm.upper, .weights = {1, 0.5F, 0, 0, 0.5F, 1}}});
    world::Skin2D skin = arm.bent();
    CHECK(skin.problem.empty());
    REQUIRE(skin.points.size() == 6);
    // At rest nothing moves.
    check_near(skin.points[2], {100, -10});
    check_near(skin.points[5], {0, 10});

    // The lower bone turns a quarter clockwise about the elbow at (50, 0).
    arm.world.ecs().get<world::Transform2D>(arm.lower).rotation = kQuarterTurn;
    skin = arm.bent();
    REQUIRE(skin.points.size() == 6);
    check_near(skin.points[0], {0, -10});
    check_near(skin.points[1], {55, -5}); // halfway between (50, -10) and (60, 0)
    check_near(skin.points[2], {60, 50});
    check_near(skin.points[3], {40, 50});
    check_near(skin.points[4], {45, 5});
    check_near(skin.points[5], {0, 10});

    // Moving the skeleton, which carries the skin with it, moves nothing more.
    arm.world.ecs().get<world::Transform2D>(arm.skeleton).position = {-300, 40};
    arm.world.ecs().get<world::Transform2D>(arm.skeleton).rotation = 1.0F;
    skin = arm.bent();
    REQUIRE(skin.points.size() == 6);
    check_near(skin.points[2], {60, 50});

    // The upper bone moves the whole arm, the lower bone with it.
    arm.world.ecs().get<world::Transform2D>(arm.upper).position = {0, 20};
    skin = arm.bent();
    REQUIRE(skin.points.size() == 6);
    check_near(skin.points[2], {60, 70});
    check_near(skin.points[5], {0, 30});

    // Showing the rest leaves the points where they are.
    arm.world.ecs().get<world::Skeleton2D>(arm.skeleton).show_rest_only = true;
    CHECK_FALSE(arm.skinned().has_value());
}

TEST_CASE("a skin's problems are named and its bad bones left out") {
    Arm arm;
    const entt::entity stray = arm.create("stray", R"({"Transform2D": {}, "Bone2D": {}})");
    arm.world.ecs().get<world::Transform2D>(stray).position = {500, 500};
    arm.weigh({{.bone = stray, .weights = {1, 1, 1, 1, 1, 1}},
               {.bone = arm.lower, .weights = {1, 1, 1, 1, 1, 1}}});
    arm.world.ecs().get<world::Transform2D>(arm.lower).position = {60, 0};
    world::Skin2D skin = arm.bent();
    REQUIRE(skin.points.size() == 6);
    CHECK(skin.path == "/components/Polygon2D/bone_weights/0/bone");
    CHECK(skin.problem == R"("stray" is not a Bone2D of "arm")");
    check_near(skin.points[0], {10, -10});

    arm.world.ecs().get<world::Polygon2D>(arm.skin).skeleton = arm.lower;
    skin = arm.bent();
    REQUIRE(skin.points.size() == 6);
    CHECK(skin.path == "/components/Polygon2D/skeleton");
    CHECK(skin.problem == R"("lower" has no Skeleton2D)");
    check_near(skin.points[0], {0, -10});
}

TEST_CASE("skeleton bones list parents first, and auto weights favor the nearest bone") {
    Arm arm;
    const world::Tree2D tree(arm.world);
    const std::vector<world::SkeletonBone2D> bones =
        world::skeleton_bones_2d(arm.world, tree, arm.skeleton);
    REQUIRE(bones.size() == 2);
    CHECK(bones[0].entity == arm.upper);
    CHECK(bones[0].parent == entt::entity{entt::null});
    CHECK(bones[1].entity == arm.lower);
    CHECK(bones[1].parent == arm.upper);
    // The upper bone (length 0) reaches to the lower; the lower is 50 long.
    check_near(bones[0].tip, {50, 0});
    check_near(bones[1].tip, {100, 0});

    const std::vector<world::BoneWeights2D> weights =
        world::auto_weights_2d(arm.world, tree, arm.skeleton, arm.skin);
    REQUIRE(weights.size() == 2);
    CHECK(weights[0].bone == arm.upper);
    CHECK(weights[1].bone == arm.lower);
    for (std::size_t point = 0; point < 6; ++point) {
        CHECK(weights[0].weights[point] + weights[1].weights[point] ==
              doctest::Approx(1.0F).epsilon(0.002));
    }
    CHECK(weights[0].weights[0] > 0.99F);
    CHECK(weights[0].weights[1] == doctest::Approx(0.5F));
    CHECK(weights[1].weights[2] > 0.99F);
    CHECK(weights[1].weights[3] > 0.99F);

    // One bone a point: each point wholly to its nearest.
    const std::vector<world::BoneWeights2D> single =
        world::auto_weights_2d(arm.world, tree, arm.skeleton, arm.skin, 1);
    REQUIRE(single.size() == 2);
    CHECK(single[0].weights == std::vector<float>{1, 1, 0, 0, 1, 1});
    CHECK(single[1].weights == std::vector<float>{0, 0, 1, 1, 0, 0});

    // Rest takes the pose.
    arm.world.ecs().get<world::Transform2D>(arm.lower).rotation = 0.25F;
    CHECK(world::set_rest_2d(arm.world, arm.skeleton) == 2);
    CHECK(arm.world.ecs().get<world::Bone2D>(arm.lower).rest_rotation == 0.25F);
}

TEST_CASE("the canvas draws a skinned polygon bent and names a bad skin") {
    Arm arm;
    asset::Assets assets{asset::FileSource{
        .read = [](std::string_view /*path*/) -> Result<std::string> {
            return make_error(ErrorCode::NotFound, "no files");
        },
        .stamp = [](std::string_view /*path*/) -> std::optional<asset::FileStamp> {
            return std::nullopt;
        }}};
    canvas::install(arm.world, assets);
    const auto device = render::create_null_device();
    canvas::CanvasRenderer renderer(*device, assets);
    auto target = device->create_texture(
        {.width = 400, .height = 300, .filter = render::Filter::Linear, .render_target = true});
    REQUIRE(target.ok());
    canvas::FrameContents contents;
    canvas::CanvasOptions options;
    options.target = target.value();
    options.contents = &contents;
    // The width and height of what the polygon drew.
    const auto drawn_size = [&] {
        REQUIRE(renderer.draw(arm.world, options).ok());
        const auto item = std::ranges::find(contents.items, canvas::FrameItem::Kind::Polygon,
                                            &canvas::FrameItem::kind);
        REQUIRE(item != contents.items.end());
        float low_x = std::numeric_limits<float>::max();
        float low_y = low_x;
        float high_x = std::numeric_limits<float>::lowest();
        float high_y = high_x;
        for (const canvas::FramePiece& piece : item->pieces) {
            for (const render::Vertex2D& vertex : piece.vertices) {
                low_x = std::min(low_x, vertex.x);
                high_x = std::max(high_x, vertex.x);
                low_y = std::min(low_y, vertex.y);
                high_y = std::max(high_y, vertex.y);
            }
        }
        return Vec2{high_x - low_x, high_y - low_y};
    };
    arm.weigh(world::auto_weights_2d(arm.world, world::Tree2D(arm.world), arm.skeleton, arm.skin));
    const Vec2 rest = drawn_size();
    CHECK(rest.y / rest.x == doctest::Approx(0.2).epsilon(0.01));
    arm.world.ecs().get<world::Transform2D>(arm.lower).rotation = kQuarterTurn;
    const Vec2 bent = drawn_size();
    CHECK(bent.x / rest.x == doctest::Approx(0.6).epsilon(0.01));
    CHECK(bent.y / rest.x == doctest::Approx(0.6).epsilon(0.01));
    CHECK(contents.problems.empty());

    arm.world.ecs().get<world::Polygon2D>(arm.skin).skeleton = arm.upper;
    drawn_size();
    REQUIRE(contents.problems.size() == 1);
    CHECK(contents.problems[0].entity == arm.skin);
    CHECK(contents.problems[0].path == "/components/Polygon2D/skeleton");
}

TEST_CASE("skeleton.bind weights polygons to a Skeleton2D and skeleton.get lists its bones") {
    world::ComponentRegistry components;
    world::register_builtin_components(components);
    command::Registry commands;
    command::register_builtin_commands(commands);
    command::Session session(components, commands, command::SessionOptions{});
    const auto call = [&](std::string_view name, std::string_view params) {
        return session.call(name, parse(params));
    };
    const auto create = [&](std::string_view params) {
        const command::Reply reply = call("entity.create", params);
        REQUIRE_MESSAGE(reply.ok, reply.message);
    };
    create(R"({"id": "skin", "components": {"Transform2D": {}, "Polygon2D": {"polygon":
        [[0, -10], [50, -10], [100, -10], [100, 10], [50, 10], [0, 10]]}}})");
    create(R"({"id": "lower", "components": {"Transform2D": {"position": [50, 0]},
        "Bone2D": {"length": 50}}})");
    create(R"({"id": "upper", "components": {"Transform2D": {"children": ["lower"]},
        "Bone2D": {}}})");
    create(R"({"id": "arm", "components": {"Transform2D": {"children": ["upper", "skin"]},
        "Skeleton2D": {}}})");

    command::Reply reply = call("skeleton.bind", R"({"id": "arm", "polygons": ["skin"]})");
    REQUIRE_MESSAGE(reply.ok, reply.message);
    CHECK(reply.value.find("rested")->as_int() == 2);
    const json::Value& bound = reply.value.find("polygons")->as_array()[0];
    CHECK(bound.find("id")->as_string() == "skin");
    CHECK(bound.find("points")->as_int() == 6);
    CHECK(bound.find("bones")->as_array() == json::Array{"upper", "lower"});
    const auto& lower = session.world().ecs().get<world::Bone2D>(session.world().find("lower"));
    CHECK(lower.rest_position.x == 50.0F);
    const auto& polygon = session.world().ecs().get<world::Polygon2D>(session.world().find("skin"));
    CHECK(polygon.skeleton == session.world().find("arm"));
    REQUIRE(polygon.bone_weights.size() == 2);

    reply = call("skeleton.get", R"({"id": "arm", "bones": ["lower"]})");
    REQUIRE_MESSAGE(reply.ok, reply.message);
    const json::Array& bones = reply.value.find("bones")->as_array();
    REQUIRE(bones.size() == 2);
    CHECK(bones[0].find("id")->as_string() == "upper");
    CHECK(bones[0].find("rest") == nullptr);
    CHECK(bones[1].find("parent")->as_string() == "upper");
    CHECK(bones[1].find("rest")->find("position")->as_array() == json::Array{50.0, 0.0});
    CHECK(bones[1].find("tip")->as_array() == json::Array{100.0, 0.0});
    CHECK(reply.value.find("polygons")->as_array() == json::Array{"skin"});

    // Binding again with the polygons it bends, without new weights.
    reply = call("skeleton.bind", R"({"id": "arm", "rest": false, "weights": false})");
    REQUIRE_MESSAGE(reply.ok, reply.message);
    CHECK(reply.value.find("rested") == nullptr);
    CHECK(reply.value.find("polygons")->as_array().size() == 1);

    reply = call("skeleton.bind", R"({"id": "upper"})");
    CHECK_FALSE(reply.ok);
    CHECK(reply.message == R"(/id: "upper" has no Skeleton2D and Transform2D)");
    reply = call("skeleton.bind", R"({"id": "arm", "polygons": ["lower"]})");
    CHECK_FALSE(reply.ok);
    CHECK(reply.message == R"(/polygons/0: "lower" has no Polygon2D)");
}
