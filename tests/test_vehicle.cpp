// Vehicles: .vehicle.json recipes, the drivetrain, the models and 2D side views they make, the
// vehicles systems that drive them in 3D and 2D, and the vehicle.* commands.
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/platform/file.hpp"
#include "slash/scene3d/model.hpp"
#include "slash/scene3d/vehicle.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/vehicle.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace command = slash::command;
namespace json = slash::json;
namespace scene3d = slash::scene3d;
namespace world = slash::world;

namespace {

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE_MESSAGE(parsed.ok(), text);
    return std::move(parsed).value();
}

constexpr std::string_view kCar = R"({"format": 1, "name": "Hatchback", "kind": "car",
    "body": {"color": [0.1, 0.3, 0.8]},
    "physics": {"differential": "front"},
    "design": {"top_speed": [25, 70], "acceleration": [3, 25]}})";
constexpr std::string_view kBike = R"({"format": 1, "name": "Scooter", "kind": "bike"})";
constexpr std::string_view kTruck = R"({"format": 1, "name": "Hauler", "kind": "truck"})";
constexpr std::string_view kTank = R"({"format": 1, "name": "Tank", "kind": "tracked"})";
constexpr std::string_view kBoat = R"({"format": 1, "name": "Skiff", "kind": "boat"})";
constexpr std::string_view kPlane = R"({"format": 1, "name": "Glider", "kind": "aircraft"})";
constexpr std::string_view kRacer = R"({"format": 1, "extends": "vehicles/car.vehicle.json",
    "name": "Racer", "physics": {"mass": 900, "engine": {"max_torque": 450}}})";

const std::map<std::string, std::string_view> kFiles{
    {"vehicles/car.vehicle.json", kCar},     {"vehicles/bike.vehicle.json", kBike},
    {"vehicles/truck.vehicle.json", kTruck}, {"vehicles/tank.vehicle.json", kTank},
    {"vehicles/boat.vehicle.json", kBoat},   {"vehicles/plane.vehicle.json", kPlane},
    {"vehicles/racer.vehicle.json", kRacer}};

constexpr std::string_view kGround = R"({"format": 1, "entities": {
    "camera": {"components": {"Transform3D": {"position": [0, 4, 20]},
                              "Camera3D": {"current": true}}},
    "ground": {"components": {"Transform3D": {"position": [0, -0.5, 0]},
                              "StaticBody3D": {},
                              "CollisionShape3D": {"shape": "box", "size": [400, 1, 400]}}}}})";

struct Project {
    std::string root;
    world::ComponentRegistry components;
    command::Registry commands;
    std::unique_ptr<command::Session> session;

    Project(std::string name, std::string_view scene = kGround) : root(std::move(name)) {
        std::filesystem::remove_all(root);
        for (const auto& [file, text] : kFiles) {
            const std::string path = root + "/" + file;
            REQUIRE(slash::platform::create_directories(
                        std::filesystem::path(path).parent_path().string())
                        .ok());
            REQUIRE(slash::platform::write_file(path, text).ok());
        }
        REQUIRE(slash::platform::write_file(root + "/main.scene.json", scene).ok());
        world::register_builtin_components(components);
        command::register_builtin_commands(commands);
        command::SessionOptions options;
        options.project_root = root;
        options.capture_logs = false;
        session = std::make_unique<command::Session>(components, commands, std::move(options));
        ok("scene.open", R"({"path": "main.scene.json"})");
    }
    Project(const Project&) = delete;
    Project& operator=(const Project&) = delete;
    Project(Project&&) = delete;
    Project& operator=(Project&&) = delete;
    ~Project() {
        session.reset();
        std::filesystem::remove_all(root);
    }

    json::Value ok(std::string_view name, std::string_view params = "{}") {
        command::Reply reply = session->call(name, parse(params));
        REQUIRE_MESSAGE(reply.ok, reply.message);
        return std::move(reply.value);
    }

    command::Reply call(std::string_view name, std::string_view params) {
        return session->call(name, parse(params));
    }

    world::World& world() { return session->world(); }

    template <typename T>
    T& get(std::string_view id) {
        const entt::entity e = world().find(id);
        REQUIRE_MESSAGE((e != entt::null), id);
        REQUIRE_MESSAGE(world().ecs().all_of<T>(e), id);
        return world().ecs().get<T>(e);
    }
};

world::VehicleRecipe recipe_of(std::string_view text) {
    auto recipe = world::parse_vehicle(parse(text), nullptr);
    REQUIRE_MESSAGE(recipe.ok(), recipe.error().message);
    return std::move(recipe).value();
}

double speed_of(const slash::Vec3& v) {
    return std::sqrt(static_cast<double>((v.x * v.x) + (v.y * v.y) + (v.z * v.z)));
}

} // namespace

TEST_CASE("vehicle recipes fill in each kind's wheels, seats and physics") {
    const world::VehicleRecipe car = recipe_of(kCar);
    CHECK(car.kind == "car");
    CHECK(car.wheels.size() == 4);
    CHECK(std::ranges::count_if(car.wheels, [](const auto& w) { return w.steer; }) == 2);
    // A front differential drives the front wheels.
    for (const world::VehicleWheel& wheel : car.wheels) {
        CHECK(wheel.drive == (wheel.position.z > 0.0F));
    }
    CHECK(std::ranges::count_if(car.seats, [](const auto& s) { return s.driver; }) == 1);
    CHECK(recipe_of(kBike).wheels.size() == 2);
    CHECK(recipe_of(kTruck).wheels.size() == 6);
    CHECK(recipe_of(kTank).wheels.size() == 10);
    CHECK(recipe_of(kBoat).wheels.empty());
    const world::VehicleRecipe plane = recipe_of(kPlane);
    CHECK(plane.wheels.size() == 3);
    CHECK(plane.wing_span > plane.body.length);
    CHECK(plane.physics.engine.thrust > 0.0F);
}

TEST_CASE("vehicle recipes are checked") {
    const auto errors = [](std::string_view text) {
        std::vector<std::string> paths;
        for (const auto& error : world::validate_vehicle(parse(text))) {
            paths.push_back(error.path);
        }
        return paths;
    };
    CHECK(errors(kCar).empty());
    const auto has = [](const std::vector<std::string>& paths, std::string_view path) {
        return std::ranges::find(paths, path) != paths.end();
    };
    CHECK(has(errors(R"({"format": 1, "kind": "boat",
        "wheels": [{"position": [0, 0.3, 1]}]})"),
              "/wheels"));
    CHECK(has(errors(R"({"format": 1, "kind": "car",
        "seats": [{"name": "a", "position": [0, 1, 0]}]})"),
              "/seats"));
    CHECK(has(errors(R"({"format": 1, "physics": {"gears": {"ratios": [2, 3]}}})"),
              "/physics/gears/ratios/1"));
    CHECK(!errors(R"({"format": 1, "kind": "hovercraft"})").empty());
}

TEST_CASE("the drivetrain shifts up, limits revs, reverses and eases the steering") {
    const world::VehicleRecipe car = recipe_of(kCar);
    world::DriveState state;
    const world::DriveInput full{.throttle = 1.0F, .steer = 1.0F};
    // From a standstill it pulls in first gear.
    world::DriveOutput out =
        world::drive_step(car.physics, state, full, 0.0F, 0.0F, 0.35F, 1.0F / 60.0F);
    CHECK(state.gear == 1);
    CHECK(out.force > 1000.0F);
    // The wheels turn toward the steer at steer_speed.
    CHECK(out.steer == doctest::Approx(car.physics.steer_speed / 60.0F));
    // Spinning fast, it shifts up and has no torque while it shifts.
    out = world::drive_step(car.physics, state, full, 1600.0F, 20.0F, 0.35F, 1.0F / 60.0F);
    CHECK(state.gear == 2);
    out = world::drive_step(car.physics, state, full, 1600.0F, 20.0F, 0.35F, 1.0F / 60.0F);
    CHECK(out.force == 0.0F);
    // Throttle back while rolling forward brakes; once stopped it takes reverse.
    world::DriveState rolling;
    out = world::drive_step(car.physics, rolling, {.throttle = -1.0F}, 300.0F, 5.0F, 0.35F,
                            1.0F / 60.0F);
    CHECK(out.brake > 0.0F);
    CHECK(out.force == 0.0F);
    out = world::drive_step(car.physics, rolling, {.throttle = -1.0F}, 0.0F, 0.0F, 0.35F,
                            1.0F / 60.0F);
    CHECK(rolling.gear == -1);
    CHECK(out.force < 0.0F);
    // The torque curve's peak.
    CHECK(world::torque_share(car.physics.engine, 0.5F) == doctest::Approx(1.0F));
}

TEST_CASE("vehicle models have a body, seats, doors and collision") {
    const scene3d::Model car = scene3d::build_vehicle(recipe_of(kCar));
    REQUIRE(!car.meshes.empty());
    CHECK(car.collision.size() == 1);
    const auto node = [&](std::string_view name) {
        return std::ranges::any_of(car.nodes, [&](const auto& n) { return n.name == name; });
    };
    CHECK(node("seat_driver"));
    CHECK(node("door_driver"));
    const scene3d::Model plane = scene3d::build_vehicle(recipe_of(kPlane));
    CHECK(plane.collision.size() == 2); // the body and the wings
    // 2D side views: one wheel per axle.
    const world::VehicleSideView side = world::vehicle_side_view(recipe_of(kTruck));
    CHECK(side.wheels.size() == 3);
    CHECK(!side.body.empty());
}

TEST_CASE("a 3D car drives, steers, brakes and carries its driver") {
    Project project("vehicle_drive_3d");
    project.ok("entity.create", R"({"id": "car", "components": {
        "Transform3D": {"position": [0, 0.1, 0]},
        "Vehicle3D": {"recipe": "vehicles/car.vehicle.json"}}})");
    project.ok("entity.create", R"({"id": "dana", "components": {
        "Transform3D": {"position": [3, 0, 0]},
        "CharacterBody3D": {},
        "CollisionShape3D": {"shape": "capsule", "radius": 0.3, "height": 1.8}}})");
    project.ok("step", R"({"ticks": 60})");
    // It settles on its wheels.
    CHECK((project.world().find("car_wheel_0") != entt::null));
    const auto& body = project.get<world::RigidBody3D>("car");
    CHECK(body.mass == doctest::Approx(1200.0F));
    CHECK(body.center_of_mass_mode == "custom");
    const float rest = project.get<world::Transform3D>("car").position.y;
    CHECK(std::abs(rest - 0.1F) < 0.3F);

    project.get<world::Vehicle3D>("car").controls.throttle = 1.0F;
    project.ok("step", R"({"ticks": 240})");
    const auto& car = project.get<world::Vehicle3D>("car");
    CHECK(car.drive.speed > 10.0F);
    CHECK(car.drive.gear >= 2);
    CHECK(project.get<world::Transform3D>("car").position.z > 15.0F);

    project.get<world::Vehicle3D>("car").controls = {.brake = 1.0F};
    project.ok("step", R"({"ticks": 300})");
    CHECK(std::abs(project.get<world::Vehicle3D>("car").drive.speed) < 0.5F);

    // Turning left (toward +x) at speed.
    const float x0 = project.get<world::Transform3D>("car").position.x;
    project.get<world::Vehicle3D>("car").controls = {.throttle = 0.6F, .steer = 1.0F};
    project.ok("step", R"({"ticks": 120})");
    CHECK(project.get<world::Transform3D>("car").position.x > x0 + 1.0F);
}

TEST_CASE("boats float and planes fly") {
    Project project("vehicle_float_3d", R"({"format": 1, "entities": {}})");
    project.ok("entity.create", R"({"id": "boat", "components": {
        "Transform3D": {"position": [0, 1, 0]},
        "Vehicle3D": {"recipe": "vehicles/boat.vehicle.json"}}})");
    project.ok("step", R"({"ticks": 300})");
    const float y = project.get<world::Transform3D>("boat").position.y;
    // It floats with its hull at about its draft under the water.
    CHECK(y > -0.9F);
    CHECK(y < 0.2F);
    project.get<world::Vehicle3D>("boat").controls.throttle = 1.0F;
    project.ok("step", R"({"ticks": 300})");
    CHECK(project.get<world::Transform3D>("boat").position.z > 10.0F);
    CHECK(project.get<world::Transform3D>("boat").position.y > -1.0F);

    project.ok("entity.create", R"({"id": "plane", "components": {
        "Transform3D": {"position": [0, 100, 0]},
        "RigidBody3D": {"linear_velocity": [0, 0, 50]},
        "Vehicle3D": {"recipe": "vehicles/plane.vehicle.json",
                      "controls": {"throttle": 1}}}})");
    project.ok("step", R"({"ticks": 180})");
    const auto& plane = project.get<world::RigidBody3D>("plane");
    // Its wings hold it up: it falls far less than it would without them.
    CHECK(project.get<world::Transform3D>("plane").position.y > 70.0F);
    CHECK(speed_of(plane.linear_velocity) > 30.0);
}

TEST_CASE("a 2D car rolls on sprung wheels and a 2D boat floats") {
    Project project("vehicle_drive_2d", R"({"format": 1, "entities": {
        "ground": {"components": {"Transform2D": {"position": [0, 500]},
                                  "StaticBody2D": {},
                                  "CollisionShape2D": {"size": [100000, 1000]}}}}})");
    project.ok("vehicle.place", R"({"recipe": "vehicles/car.vehicle.json", "id": "car",
                                    "at": [0, -20]})");
    project.ok("step", R"({"ticks": 90})");
    // Its pieces, wheels and joints.
    CHECK((project.world().find("car_body") != entt::null));
    CHECK((project.world().find("car_collision") != entt::null));
    CHECK((project.world().find("car_wheel_1") != entt::null));
    CHECK((project.world().find("car_wheel_0_groove") != entt::null));
    CHECK((project.world().find("car_wheel_0_spring") != entt::null));
    CHECK(project.get<world::Polygon2D>("car_wheel_0").polygon.size() == 20);
    // It rests on its wheels a little above the ground's top at y = 0.
    const float rest = project.get<world::Transform2D>("car").position.y;
    CHECK(rest < 5.0F);
    CHECK(rest > -40.0F);
    project.ok("vehicle.control", R"({"vehicle": "car", "action": "drive", "throttle": 1})");
    project.ok("step", R"({"ticks": 240})");
    const json::Value state = project.ok("vehicle.state", R"({"vehicle": "car"})");
    const json::Value& car = state.find("vehicles")->as_array().front();
    CHECK(car.find("speed")->as_double() > 8.0);
    CHECK(project.get<world::Transform2D>("car").position.x > 500.0F);

    project.ok("vehicle.place", R"({"recipe": "vehicles/boat.vehicle.json", "id": "boat",
                                    "at": [-3000, 200]})");
    // No ground under the boat: it floats on water at y = 0.
    project.world().destroy(project.world().find("ground"));
    project.ok("step", R"({"ticks": 300})");
    const float y = project.get<world::Transform2D>("boat").position.y;
    CHECK(y > -10.0F);
    CHECK(y < 60.0F);
}

TEST_CASE("characters get in, ride, get out and are hit by vehicles") {
    Project project("vehicle_riders_3d");
    project.ok("vehicle.place", R"({"recipe": "vehicles/car.vehicle.json", "id": "car",
                                    "at": [0, 0.05, 0]})");
    project.ok("entity.create", R"({"id": "dana", "components": {
        "Transform3D": {"position": [3, 0, 0]},
        "CharacterBody3D": {"collision_layer": 2},
        "CollisionShape3D": {"shape": "capsule", "radius": 0.3, "height": 1.8}}})");
    project.ok("step", R"({"ticks": 30})");
    const json::Value entered =
        project.ok("vehicle.control", R"({"vehicle": "car", "action": "enter", "rider": "dana"})");
    CHECK(entered.find("seat")->as_string() == "driver");
    CHECK(project.get<world::CharacterBody3D>("dana").collision_layer == 0);
    CHECK(project.get<world::VehicleRider>("dana").seat == "driver");
    // A second rider takes the next seat; the driver's is taken.
    project.ok("entity.create", R"({"id": "sam", "components": {
        "Transform3D": {"position": [-3, 0, 0]}}})");
    CHECK(!project
               .call("vehicle.control", R"({"vehicle": "car", "action": "enter",
                                              "rider": "sam", "seat": "driver"})")
               .ok);
    project.ok("vehicle.control", R"({"vehicle": "car", "action": "enter", "rider": "sam"})");
    project.ok("vehicle.control", R"({"vehicle": "car", "action": "drive", "throttle": 1})");
    project.ok("step", R"({"ticks": 120})");
    // She rides along in her seat.
    const float car_z = project.get<world::Transform3D>("car").position.z;
    CHECK(car_z > 5.0F);
    CHECK(std::abs(project.get<world::Transform3D>("dana").position.z - car_z) < 2.0F);
    project.ok("vehicle.control", R"({"vehicle": "car", "action": "drive", "throttle": 0,
                                      "brake": 1})");
    project.ok("step", R"({"ticks": 240})");
    project.ok("vehicle.control", R"({"vehicle": "car", "action": "exit", "rider": "dana"})");
    CHECK(project.get<world::CharacterBody3D>("dana").collision_layer == 2);
    CHECK(!project.world().ecs().all_of<world::VehicleRider>(project.world().find("dana")));

    // Someone in its way is hit, pushed and hurt.
    const float ahead = project.get<world::Transform3D>("car").position.z + 12.0F;
    project.ok("entity.create", R"({"id": "walker", "components": {
        "Transform3D": {"position": [0, 0.9, )" +
                                    std::to_string(ahead) + R"(]},
        "CharacterBody3D": {"motion_mode": "floating"},
        "CollisionShape3D": {"shape": "capsule", "radius": 0.3, "height": 1.8}}})");
    project.ok("events.subscribe", R"({"kinds": ["vehicle_hit"]})");
    project.ok("vehicle.control", R"({"vehicle": "car", "action": "drive", "throttle": 1,
                                      "brake": 0})");
    project.ok("step", R"({"ticks": 180})");
    const json::Value read = project.ok("events.read", R"({"subscription": "events_1"})");
    REQUIRE(!read.find("events")->as_array().empty());
    const json::Value& hit = read.find("events")->as_array().front();
    CHECK(hit.find("data")->find("target")->as_string() == "walker");
    CHECK(hit.find("data")->find("damage")->as_double() > 0.0);
}

TEST_CASE("vehicle.tune changes one vehicle and vehicle.check holds it to its design") {
    Project project("vehicle_tune");
    project.ok("vehicle.place", R"({"recipe": "vehicles/car.vehicle.json", "id": "car",
                                    "at": [0, 0.05, 0]})");
    const json::Value tuned = project.ok(
        "vehicle.tune",
        R"({"vehicle": "car", "physics": {"mass": 1500, "engine": {"max_torque": 500}}})");
    CHECK(tuned.find("physics")->find("mass")->as_double() == doctest::Approx(1500.0));
    project.ok("step", R"({"ticks": 2})");
    CHECK(project.get<world::RigidBody3D>("car").mass == doctest::Approx(1500.0F));
    // A patch that breaks the recipe is refused and changes nothing.
    const command::Reply bad = project.call(
        "vehicle.tune", R"({"vehicle": "car", "physics": {"gears": {"ratios": [1, 2]}}})");
    CHECK(!bad.ok);
    CHECK(bad.message.find("/physics/gears/ratios/1") != std::string::npos);
    project.ok("vehicle.tune", R"({"vehicle": "car", "reset": true})");
    CHECK(project.get<world::Vehicle3D>("car").overrides.is_null());

    const json::Value checked = project.ok("vehicle.check", R"({"vehicle": "car"})");
    const json::Value& measures = *checked.find("measures");
    CHECK(measures.find("top_speed")->find("value")->as_double() > 25.0);
    CHECK(measures.find("acceleration")->find("value")->as_double() > 3.0);
    CHECK(measures.find("braking_distance")->find("value")->as_double() > 5.0);
    CHECK(measures.find("turning_radius")->find("value")->as_double() > 2.0);
    CHECK(checked.find("passed")->as_bool());
    // Detuned past its design, it fails.
    project.ok("vehicle.tune", R"({"vehicle": "car", "physics": {"engine": {"max_torque": 40}}})");
    const json::Value slow = project.ok("vehicle.check", R"({"vehicle": "car"})");
    CHECK(!slow.find("passed")->as_bool());
    CHECK(!slow.find("problems")->as_array().empty());
}

TEST_CASE("custom centers of mass move where bodies balance") {
    // A plank on a point: with its center of mass off to one side, it tips that way.
    Project project("vehicle_center_of_mass");
    project.ok("entity.create", R"({"id": "plank", "components": {
        "Transform3D": {"position": [0, 2, 0]},
        "RigidBody3D": {"center_of_mass_mode": "custom", "center_of_mass": [1.5, 0, 0]},
        "CollisionShape3D": {"shape": "box", "size": [4, 0.2, 1]}}})");
    project.ok("entity.create", R"({"id": "point", "components": {
        "Transform3D": {"position": [0, 1, 0]},
        "StaticBody3D": {},
        "CollisionShape3D": {"shape": "box", "size": [0.2, 0.2, 2]}}})");
    project.ok("step", R"({"ticks": 120})");
    // Tipped down on +x: turned negative about z.
    CHECK(project.get<world::Transform3D>("plank").rotation.z < -0.2F);

    Project flat("vehicle_center_of_mass_2d", R"({"format": 1, "entities": {}})");
    flat.ok("entity.create", R"({"id": "plank", "components": {
        "Transform2D": {"position": [0, 0]},
        "RigidBody2D": {"center_of_mass_mode": "custom", "center_of_mass": [80, 0]},
        "CollisionShape2D": {"size": [200, 10]}}})");
    flat.ok("entity.create", R"({"id": "point", "components": {
        "Transform2D": {"position": [0, 20]},
        "StaticBody2D": {},
        "CollisionShape2D": {"size": [10, 10]}}})");
    flat.ok("step", R"({"ticks": 120})");
    // y is down: tipping down on +x turns it clockwise, positive.
    CHECK(flat.get<world::Transform2D>("plank").rotation > 0.2F);
}

TEST_CASE("agents build vehicles of their own parts") {
    constexpr std::string_view kBuggy = R"({"format": 1, "name": "Buggy", "kind": "car",
        "body": {"draw": false},
        "parts": [
            {"name": "tub", "shape": "box", "position": [0, 0.6, 0], "size": [1.6, 0.4, 3.6],
             "collide": true},
            {"name": "nose", "shape": "prism", "position": [0, 0.6, 2.2], "size": [1.4, 0.4, 0.8],
             "rotation": [1.5708, 0, 0]},
            {"name": "roll_bar", "shape": "cylinder", "position": [0, 1.3, -0.6],
             "size": [0.1, 1.0, 0.1], "material": "trim"},
            {"name": "lamp", "shape": "sphere", "position": [0.6, 0.8, 1.8],
             "size": [0.25, 0.25, 0.25], "material": "lamp", "color": [1, 0.9, 0.5]},
            {"name": "sidecar", "model": "vehicles/bike.vehicle.json", "position": [1.4, 0, 0]}
        ]})";
    const json::Value doc = parse(kBuggy);
    CHECK(world::validate_vehicle(doc).empty());
    const auto read = [](std::string_view path) -> slash::Result<std::string> {
        const auto found = kFiles.find(std::string(path));
        if (found == kFiles.end()) {
            return slash::make_error(slash::ErrorCode::NotFound, std::string(path));
        }
        return std::string(found->second);
    };
    auto built = scene3d::load_vehicle_model(kBuggy, read);
    REQUIRE_MESSAGE(built.ok(), built.error().message);
    const scene3d::Model& buggy = built.value();
    // Drawn in the body's paint and trim and the lamp's own color; the body's box isn't drawn.
    const auto material = [&](std::string_view name) {
        return std::ranges::any_of(buggy.materials, [&](const auto& m) { return m.name == name; });
    };
    CHECK(material("paint"));
    CHECK(material("trim"));
    CHECK(material("lamp"));
    // The body's collision box, the tub's and the sidecar's own.
    CHECK(buggy.collision.size() == 3);
    CHECK(std::ranges::any_of(buggy.nodes, [](const auto& n) { return n.name == "seat_driver"; }));
    // The sidecar's model is placed beside it: its nodes come under the body.
    CHECK(buggy.parts.size() >= 2);
    // A missing model part fails to load and is reported where it is named.
    constexpr std::string_view kLost = R"({"format": 1, "kind": "car",
        "parts": [{"name": "crate", "model": "props/crate.glb"}]})";
    CHECK(!scene3d::load_vehicle_model(kLost, read).ok());
    const auto lost = scene3d::check_vehicle_model(kLost, read);
    CHECK(std::ranges::any_of(lost, [](const auto& e) { return e.path == "/parts/0/model"; }));

    // From the side, each shape part is a piece of its own; the body's box isn't drawn.
    const world::VehicleSideView side = world::vehicle_side_view(recipe_of(kBuggy));
    const auto piece = [&](std::string_view name) {
        return std::ranges::find_if(side.body, [&](const auto& p) { return p.name == name; });
    };
    CHECK(piece("body") == side.body.end());
    REQUIRE(piece("tub") != side.body.end());
    CHECK(piece("tub")->points.size() == 4);
    REQUIRE(piece("lamp") != side.body.end());
    CHECK(piece("lamp")->color.y == doctest::Approx(0.9F));
    CHECK(piece("sidecar") == side.body.end()); // models aren't drawn in 2D
    // The tub is 3.6 meters long, seen along z.
    float low = 100.0F;
    float high = -100.0F;
    for (const slash::Vec2 p : piece("tub")->points) {
        low = std::min(low, static_cast<float>(p.x));
        high = std::max(high, static_cast<float>(p.x));
    }
    CHECK(high - low == doctest::Approx(3.6F).epsilon(0.001));

    // Parts are checked: names taken twice or by the vehicle's own pieces, a part that is both a
    // shape and a model, and a color of its own named without one.
    const auto errors = world::validate_vehicle(parse(R"({"format": 1, "kind": "car", "parts": [
        {"name": "fin"}, {"name": "fin"}, {"name": "wheel_2"},
        {"name": "box", "shape": "box", "model": "a.glb"}, {"name": "chrome", "material": "chrome"}
    ]})"));
    const auto has = [&](std::string_view path) {
        return std::ranges::any_of(errors, [&](const auto& e) { return e.path == path; });
    };
    CHECK(has("/parts/1/name"));
    CHECK(has("/parts/2/name"));
    CHECK(has("/parts/3"));
    CHECK(has("/parts/4/material"));
}

TEST_CASE("vehicle.design fits a recipe an agent wrote to its targets and writes it") {
    Project project("vehicle_design");
    const json::Value designed = project.ok("vehicle.design", R"({
        "path": "vehicles/van.vehicle.json",
        "recipe": {"format": 1, "name": "Van", "kind": "car",
                   "body": {"length": 4.8, "height": 2.0, "cabin": 0},
                   "parts": [{"name": "roof_rack", "shape": "box", "position": [0, 2.1, 0],
                              "size": [1.4, 0.1, 2.0], "material": "trim"}],
                   "physics": {"mass": 2000}},
        "targets": {"acceleration": 12, "top_speed": 38}})");
    CHECK(designed.find("passed")->as_bool());
    const json::Value& measures = *designed.find("measures");
    CHECK(measures.find("acceleration")->find("value")->as_double() ==
          doctest::Approx(12.0).epsilon(0.15));
    CHECK(measures.find("top_speed")->find("value")->as_double() ==
          doctest::Approx(38.0).epsilon(0.15));
    CHECK(designed.find("test_drives")->as_int() >= 1);
    CHECK(!designed.find("fitted")->as_object().empty());
    // The file holds what was fitted and the targets as its design, so vehicle.check passes it.
    auto text = slash::platform::read_file(project.root + "/vehicles/van.vehicle.json");
    REQUIRE(text.ok());
    const json::Value file = parse(text.value());
    CHECK(file.find("design")->find("acceleration")->as_array().size() == 2);
    CHECK(file.find("parts")->as_array().size() == 1);
    const json::Value checked =
        project.ok("vehicle.check", R"({"recipe": "vehicles/van.vehicle.json"})");
    CHECK(checked.find("passed")->as_bool());
    // A recipe that breaks the schema is refused where it breaks.
    const command::Reply bad = project.call("vehicle.design", R"({
        "path": "vehicles/bad.vehicle.json",
        "recipe": {"format": 1, "kind": "car", "parts": [{"name": "body"}]}})");
    CHECK(!bad.ok);
    CHECK(bad.message.find("/recipe/parts/0/name") != std::string::npos);
    // Undo takes the written file away again.
    project.ok("undo");
    CHECK(!std::filesystem::exists(project.root + "/vehicles/van.vehicle.json"));
}
