#include "slash/command/vehicle.hpp"

#include "slash/command/session.hpp"
#include "slash/core/text.hpp"
#include "slash/physics2d/vehicle_tool.hpp"
#include "slash/physics3d/vehicle_tool.hpp"
#include "slash/world/components.hpp"
#include "slash/world/ragdoll.hpp"
#include "slash/world/tree2d.hpp"
#include "slash/world/tree3d.hpp"
#include "slash/world/vehicle.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// vehicle.place, vehicle.control, vehicle.tune, vehicle.check, vehicle.design and
// vehicle.state, over
// .vehicle.json recipes (slash/world/vehicle.hpp) and the vehicles systems that drive them
// (slash/physics3d/vehicle_tool.hpp, slash/physics2d/vehicle_tool.hpp).
namespace slash::command {
namespace {

constexpr double kTick = 1.0 / 60.0;
constexpr float kHundredKmh = 27.7778F; // meters a second

json::Value property(std::string_view type, std::string_view description) {
    json::Value schema;
    schema["type"] = type;
    schema["description"] = description;
    return schema;
}

json::Value text_property(std::string_view description) {
    json::Value out = property("string", description);
    out["minLength"] = 1;
    return out;
}

json::Value unit_property(std::string_view description, double low) {
    json::Value out = property("number", description);
    out["minimum"] = low;
    out["maximum"] = 1;
    return out;
}

json::Value schema_of(json::Object properties, json::Array required) {
    json::Value schema;
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["properties"] = std::move(properties);
    if (!required.empty()) {
        schema["required"] = std::move(required);
    }
    return schema;
}

Reply param_error(int error_code, std::string_view path, const std::string& message) {
    json::Value data;
    data["path"] = path;
    return Reply::failure(error_code, std::string(path) + ": " + message, std::move(data));
}

std::string quoted(std::string_view name) {
    return "\"" + std::string(name) + "\"";
}

double rounded(double value, double places = 100.0) {
    return std::round(value * places) / places;
}

Reply unknown_entity(const world::World& world, std::string_view id, std::string_view path) {
    std::string message = "no entity has the id " + quoted(id);
    if (const std::string_view hint = closest_match(id, world.ids()); !hint.empty()) {
        message += "; did you mean " + quoted(hint) + "?";
    }
    return param_error(code::kNotFound, path, message);
}

Result<std::shared_ptr<const world::VehicleRecipe>> recipe_param(const world::World& world,
                                                                 const std::string& path) {
    if (!world::is_vehicle_path(path)) {
        return make_error(ErrorCode::InvalidArgument,
                          quoted(path) + " must end in " + std::string(world::kVehicleSuffix));
    }
    auto recipe = world::load_vehicle(world, path);
    if (!recipe) {
        std::string message = recipe.error().message;
        if (!message.starts_with(quoted(path))) {
            message = quoted(path) + ": " + message;
        }
        return make_error(ErrorCode::InvalidArgument, message);
    }
    return recipe;
}

// A vehicle named by a param: its entity and whether it is 2D.
struct Found {
    entt::entity entity{entt::null};
    bool flat = false;
};

std::optional<Found> vehicle_param(const world::World& world, const std::string& id) {
    const entt::entity entity = world.find(id);
    if (entity == entt::null) {
        return std::nullopt;
    }
    if (world.ecs().all_of<world::Vehicle3D>(entity)) {
        return Found{.entity = entity, .flat = false};
    }
    if (world.ecs().all_of<world::Vehicle2D>(entity)) {
        return Found{.entity = entity, .flat = true};
    }
    return std::nullopt;
}

Reply not_a_vehicle(const world::World& world, const std::string& id, std::string_view path) {
    if (world.find(id) == entt::null) {
        return unknown_entity(world, id, path);
    }
    return param_error(code::kInvalidParams, path, quoted(id) + " has no Vehicle3D or Vehicle2D");
}

json::Value vec3(Vec3 v) {
    return json::Array{rounded(v.x), rounded(v.y), rounded(v.z)};
}

json::Value vec2(Vec2 v) {
    return json::Array{rounded(v.x), rounded(v.y)};
}

// ---- vehicle.place ----

Reply vehicle_place(Session& session, const json::Value& params) {
    world::World& world = session.world();
    const std::string path = params.find("recipe")->as_string();
    auto recipe = recipe_param(world, path);
    if (!recipe) {
        return param_error(code::kInvalidParams, "/recipe", recipe.error().message);
    }
    const std::string id = params.find("id")->as_string();
    if (world.find(id) != entt::null) {
        return param_error(code::kInvalidParams, "/id", quoted(id) + " already exists");
    }
    entt::entity parent{entt::null};
    if (const json::Value* given = params.find("parent"); given != nullptr) {
        parent = world.find(given->as_string());
        if (parent == entt::null) {
            return unknown_entity(world, given->as_string(), "/parent");
        }
    }
    auto created = world.create(id);
    if (!created) {
        return param_error(code::kInvalidParams, "/id", created.error().message);
    }
    const entt::entity entity = created.value();
    auto& ecs = world.ecs();
    const json::Array& at = params.find("at")->as_array();
    const bool flat = at.size() == 2;
    json::Array components;
    if (flat) {
        world::Transform2D transform;
        transform.position = {static_cast<real_t>(at[0].as_double()),
                              static_cast<real_t>(at[1].as_double())};
        ecs.emplace<world::Transform2D>(entity, transform);
        world::Vehicle2D vehicle;
        vehicle.recipe = path;
        if (const json::Value* ppm = params.find("pixels_per_meter"); ppm != nullptr) {
            vehicle.pixels_per_meter = static_cast<float>(ppm->as_double());
        }
        if (const json::Value* flip = params.find("flip_h"); flip != nullptr) {
            vehicle.flip_h = flip->as_bool();
        }
        ecs.emplace<world::Vehicle2D>(entity, vehicle);
        world::RigidBody2D body;
        body.mass = recipe.value()->physics.mass;
        ecs.emplace<world::RigidBody2D>(entity, body);
        if (parent != entt::null) {
            if (auto* tree = ecs.try_get<world::Transform2D>(parent); tree != nullptr) {
                tree->children.push_back(entity);
            }
        }
        components = {"Transform2D", "Vehicle2D", "RigidBody2D"};
    } else {
        world::Transform3D transform;
        transform.position = {static_cast<real_t>(at[0].as_double()),
                              static_cast<real_t>(at[1].as_double()),
                              static_cast<real_t>(at[2].as_double())};
        if (const json::Value* yaw = params.find("yaw"); yaw != nullptr) {
            transform.rotation.y = static_cast<real_t>(yaw->as_double());
        }
        ecs.emplace<world::Transform3D>(entity, transform);
        world::MeshInstance3D instance;
        instance.mesh = path;
        ecs.emplace<world::MeshInstance3D>(entity, instance);
        world::RigidBody3D body;
        body.mass = recipe.value()->physics.mass;
        ecs.emplace<world::RigidBody3D>(entity, body);
        world::Vehicle3D vehicle;
        vehicle.recipe = path;
        ecs.emplace<world::Vehicle3D>(entity, vehicle);
        if (parent != entt::null) {
            if (auto* tree = ecs.try_get<world::Transform3D>(parent); tree != nullptr) {
                tree->children.push_back(entity);
            }
        }
        components = {"Transform3D", "MeshInstance3D", "RigidBody3D", "Vehicle3D"};
    }
    const world::VehicleRecipe& made = *recipe.value();
    json::Value result;
    result["id"] = id;
    result["components"] = std::move(components);
    result["kind"] = made.kind;
    json::Array seats;
    for (const world::VehicleSeat& seat : made.seats) {
        seats.emplace_back(seat.name);
    }
    result["seats"] = std::move(seats);
    result["wheels"] = static_cast<std::int64_t>(made.wheels.size());
    return Reply::success(std::move(result));
}

// ---- vehicle.control ----

Reply vehicle_control(Session& session, const json::Value& params) {
    world::World& world = session.world();
    const std::string id = params.find("vehicle")->as_string();
    const std::optional<Found> found = vehicle_param(world, id);
    if (!found) {
        return not_a_vehicle(world, id, "/vehicle");
    }
    const std::string action = params.find("action")->as_string();
    json::Value result;
    result["vehicle"] = id;
    if (action == "enter" || action == "exit") {
        const json::Value* rider_id = params.find("rider");
        if (rider_id == nullptr) {
            return param_error(code::kInvalidParams, "/rider", action + " needs a rider");
        }
        const entt::entity rider = world.find(rider_id->as_string());
        if (rider == entt::null) {
            return unknown_entity(world, rider_id->as_string(), "/rider");
        }
        if (action == "enter") {
            const json::Value* seat = params.find("seat");
            const std::string seat_name = seat != nullptr ? seat->as_string() : std::string{};
            auto entered = found->flat
                               ? physics2d::enter_vehicle(world, rider, found->entity, seat_name)
                               : physics3d::enter_vehicle(world, rider, found->entity, seat_name);
            if (!entered) {
                return param_error(code::kInvalidParams, seat != nullptr ? "/seat" : "/rider",
                                   entered.error().message);
            }
            result["seat"] = entered.value();
        } else {
            const auto* ride = world.ecs().try_get<world::VehicleRider>(rider);
            if (ride == nullptr || ride->vehicle != found->entity) {
                return param_error(code::kInvalidParams, "/rider",
                                   quoted(rider_id->as_string()) + " isn't in " + quoted(id));
            }
            auto left = found->flat ? physics2d::exit_vehicle(world, rider)
                                    : physics3d::exit_vehicle(world, rider);
            if (!left) {
                return param_error(code::kInvalidParams, "/rider", left.error().message);
            }
            result["exited"] = true;
        }
        return Reply::success(std::move(result));
    }
    // drive: the controls given, the rest as they were.
    auto& ecs = world.ecs();
    world::VehicleControls controls = found->flat
                                          ? ecs.get<world::Vehicle2D>(found->entity).controls
                                          : ecs.get<world::Vehicle3D>(found->entity).controls;
    const auto read = [&](std::string_view key, float& into) {
        if (const json::Value* value = params.find(key); value != nullptr) {
            into = static_cast<float>(value->as_double());
        }
    };
    read("throttle", controls.throttle);
    read("brake", controls.brake);
    read("handbrake", controls.handbrake);
    read("steer", controls.steer);
    read("pitch", controls.pitch);
    if (found->flat) {
        ecs.patch<world::Vehicle2D>(found->entity,
                                    [&](world::Vehicle2D& v) { v.controls = controls; });
    } else {
        ecs.patch<world::Vehicle3D>(found->entity,
                                    [&](world::Vehicle3D& v) { v.controls = controls; });
    }
    json::Value now;
    now["throttle"] = rounded(controls.throttle);
    now["brake"] = rounded(controls.brake);
    now["handbrake"] = rounded(controls.handbrake);
    now["steer"] = rounded(controls.steer);
    now["pitch"] = rounded(controls.pitch);
    result["controls"] = std::move(now);
    return Reply::success(std::move(result));
}

// ---- vehicle.tune ----

Reply vehicle_tune(Session& session, const json::Value& params) {
    world::World& world = session.world();
    const std::string id = params.find("vehicle")->as_string();
    const std::optional<Found> found = vehicle_param(world, id);
    if (!found) {
        return not_a_vehicle(world, id, "/vehicle");
    }
    auto& ecs = world.ecs();
    json::Value overrides = found->flat ? ecs.get<world::Vehicle2D>(found->entity).overrides
                                        : ecs.get<world::Vehicle3D>(found->entity).overrides;
    const json::Value* reset = params.find("reset");
    if (reset != nullptr && reset->as_bool()) {
        overrides = json::Value{};
    }
    json::Value patch = json::Object{};
    for (const std::string_view section : {"physics", "damage", "design"}) {
        if (const json::Value* given = params.find(section); given != nullptr) {
            patch[section] = *given;
        }
    }
    if (!overrides.is_object()) {
        overrides = json::Object{};
    }
    overrides = world::merge_patch(overrides, patch);
    const std::string& path = found->flat ? ecs.get<world::Vehicle2D>(found->entity).recipe
                                          : ecs.get<world::Vehicle3D>(found->entity).recipe;
    auto resolved = world::load_vehicle_json(world, path);
    if (!resolved) {
        return param_error(code::kInvalidParams, "/vehicle", resolved.error().message);
    }
    // Checked as the recipe with them would be.
    const json::Value merged = world::merge_patch(resolved.value(), overrides);
    if (auto errors = world::validate_vehicle(merged); !errors.empty()) {
        return param_error(code::kInvalidParams, errors.front().path, errors.front().message);
    }
    auto tuned = world::tuned_vehicle(resolved.value(), overrides);
    if (!tuned) {
        return param_error(code::kInvalidParams, "", tuned.error().message);
    }
    if (overrides.is_object() && overrides.as_object().empty()) {
        overrides = json::Value{};
    }
    if (found->flat) {
        ecs.patch<world::Vehicle2D>(found->entity,
                                    [&](world::Vehicle2D& v) { v.overrides = overrides; });
    } else {
        ecs.patch<world::Vehicle3D>(found->entity,
                                    [&](world::Vehicle3D& v) { v.overrides = overrides; });
    }
    const world::VehiclePhysics& p = tuned.value().physics;
    json::Value result;
    result["vehicle"] = id;
    result["overrides"] = overrides;
    json::Value physics;
    physics["mass"] = rounded(p.mass);
    physics["max_torque"] = rounded(p.engine.max_torque);
    physics["thrust"] = rounded(p.engine.thrust);
    physics["max_steer"] = rounded(p.max_steer);
    physics["brake_force"] = rounded(p.brake_force);
    physics["tire_friction"] = rounded(p.tire_friction);
    result["physics"] = std::move(physics);
    return Reply::success(std::move(result));
}

// ---- vehicle.check ----

// A test drive's measures, in meters and seconds; nullopt where it never got there.
struct Measures {
    std::optional<float> top_speed;
    std::optional<float> acceleration;
    std::optional<float> braking_distance;
    std::optional<float> turning_radius;
};

// A test drive's world: flat ground (or open water for a boat) and the vehicle on it.
struct Drive {
    std::unique_ptr<Session> session;
    entt::entity vehicle{entt::null};
    bool flat = false;
    float ppm = 50.0F;

    [[nodiscard]] world::World& world() const { return session->world(); }

    void step() const { world().step(kTick); }

    void controls(const world::VehicleControls& c) const {
        auto& ecs = world().ecs();
        if (flat) {
            ecs.patch<world::Vehicle2D>(vehicle, [&](world::Vehicle2D& v) { v.controls = c; });
        } else {
            ecs.patch<world::Vehicle3D>(vehicle, [&](world::Vehicle3D& v) { v.controls = c; });
        }
    }

    [[nodiscard]] float speed() const {
        auto& ecs = world().ecs();
        return flat ? ecs.get<world::Vehicle2D>(vehicle).drive.speed
                    : ecs.get<world::Vehicle3D>(vehicle).drive.speed;
    }

    // Where it is, in meters along the ground.
    [[nodiscard]] Vec3 place() const {
        if (flat) {
            const Vec2 p = world::Tree2D(world()).global_affine(vehicle).origin;
            return {p.x / ppm, -p.y / ppm, 0.0F};
        }
        return world::Tree3D(world()).global(vehicle).origin;
    }
};

SessionOptions drive_options(const Session& host) {
    SessionOptions options = host.options();
    options.capture_logs = false;
    options.write_files = false;
    std::erase(options.capabilities, Capability::Build);
    std::erase(options.capabilities, Capability::Network);
    return options;
}

// files are read in place of the disk's, as a recipe not yet written.
Drive make_drive(Session& host, const std::string& recipe, const json::Value& overrides, bool flat,
                 float ppm, bool boat,
                 const std::map<std::string, std::string, std::less<>>& files = {}) {
    Drive drive;
    SessionOptions options = drive_options(host);
    for (const auto& [path, text] : files) {
        options.files.insert_or_assign(path, text);
    }
    drive.session =
        std::make_unique<Session>(host.components(), host.commands(), std::move(options));
    drive.flat = flat;
    drive.ppm = ppm;
    world::World& world = drive.world();
    auto& ecs = world.ecs();
    if (!boat) {
        const entt::entity ground = world.create();
        if (flat) {
            world::Transform2D place;
            place.position = {0.0F, 500.0F};
            ecs.emplace<world::Transform2D>(ground, place);
            ecs.emplace<world::StaticBody2D>(ground);
            world::CollisionShape2D shape;
            shape.size = {2000000.0F, 1000.0F};
            ecs.emplace<world::CollisionShape2D>(ground, shape);
        } else {
            world::Transform3D place;
            // 40 kilometers ahead, so a fast car never runs off it.
            place.position = {0.0F, -0.5F, 19000.0F};
            ecs.emplace<world::Transform3D>(ground, place);
            ecs.emplace<world::StaticBody3D>(ground);
            world::CollisionShape3D shape;
            shape.shape = "box";
            shape.size = {2000.0F, 1.0F, 40000.0F};
            ecs.emplace<world::CollisionShape3D>(ground, shape);
        }
    }
    auto made = world.create("test_vehicle");
    drive.vehicle = made ? made.value() : world.create();
    if (flat) {
        ecs.emplace<world::Transform2D>(drive.vehicle);
        world::Vehicle2D vehicle;
        vehicle.recipe = recipe;
        vehicle.overrides = overrides;
        vehicle.pixels_per_meter = ppm;
        ecs.emplace<world::Vehicle2D>(drive.vehicle, vehicle);
    } else {
        world::Transform3D place;
        place.position = {0.0F, boat ? 0.0F : 0.05F, 0.0F};
        ecs.emplace<world::Transform3D>(drive.vehicle, place);
        world::MeshInstance3D instance;
        instance.mesh = recipe;
        ecs.emplace<world::MeshInstance3D>(drive.vehicle, instance);
        world::Vehicle3D vehicle;
        vehicle.recipe = recipe;
        vehicle.overrides = overrides;
        ecs.emplace<world::Vehicle3D>(drive.vehicle, vehicle);
    }
    // It settles before it drives.
    for (int i = 0; i < 60; ++i) {
        drive.step();
    }
    return drive;
}

real_t distance(Vec3 a, Vec3 b) {
    const Vec3 d = a - b;
    return std::sqrt((d.x * d.x) + (d.y * d.y) + (d.z * d.z));
}

// Full throttle from a standstill to 100 km/h (the acceleration), then full brake to a stop
// (the braking distance), then full throttle again until it stops gaining (the top speed). A
// vehicle whose top speed is under 100 km/h brakes from nine tenths of it.
void straight_run(const Drive& drive, Measures& out) {
    constexpr int kLimit = 60 * 60; // a minute a phase
    drive.controls({.throttle = 1.0F});
    float top = 0.0F;
    int ticks = 0;
    float best = 0.0F;
    int flat_ticks = 0;
    // Up to 100 km/h, or until it stops gaining.
    for (; ticks < kLimit; ++ticks) {
        drive.step();
        const float speed = drive.speed();
        top = std::max(top, speed);
        if (speed >= kHundredKmh) {
            out.acceleration = static_cast<float>(ticks + 1) * static_cast<float>(kTick);
            break;
        }
        if (speed > best + 0.02F) {
            best = speed;
            flat_ticks = 0;
        } else if (++flat_ticks > 180) {
            break;
        }
    }
    const float from = std::min(kHundredKmh, top * 0.9F);
    // Down to where braking starts, if it went past it.
    if (from > 0.5F) {
        for (int i = 0; i < kLimit && drive.speed() > from; ++i) {
            drive.controls({});
            drive.step();
        }
        const Vec3 start = drive.place();
        drive.controls({.brake = 1.0F});
        for (int i = 0; i < kLimit; ++i) {
            drive.step();
            if (std::abs(drive.speed()) < 0.1F) {
                out.braking_distance = static_cast<float>(distance(drive.place(), start));
                break;
            }
        }
    }
    drive.controls({.throttle = 1.0F});
    best = 0.0F;
    flat_ticks = 0;
    for (int i = 0; i < kLimit * 2; ++i) {
        drive.step();
        const float speed = drive.speed();
        top = std::max(top, speed);
        if (speed > best + 0.01F) {
            best = speed;
            flat_ticks = 0;
        } else if (++flat_ticks > 240) {
            break;
        }
    }
    out.top_speed = top;
}

// At 5 meters a second with the steering full over, how wide a circle it turns.
void turning_run(const Drive& drive, Measures& out) {
    constexpr float kTarget = 5.0F;
    std::vector<Vec3> path;
    for (int i = 0; i < 60 * 30; ++i) {
        const float speed = drive.speed();
        world::VehicleControls c{.steer = 1.0F};
        if (speed < kTarget) {
            c.throttle = std::clamp((kTarget - speed) * 0.5F, 0.1F, 1.0F);
        } else {
            c.brake = std::clamp((speed - kTarget) * 0.2F, 0.0F, 1.0F);
        }
        drive.controls(c);
        drive.step();
        if (i >= 60 * 10 && i % 10 == 0) {
            path.push_back(drive.place());
        }
    }
    if (path.size() < 8) {
        return;
    }
    // The circle's middle is the average of a whole number of laps' points; near enough, the
    // middle of their bounds.
    Vec3 low = path.front();
    Vec3 high = path.front();
    for (const Vec3 p : path) {
        low = {std::min(low.x, p.x), 0.0F, std::min(low.z, p.z)};
        high = {std::max(high.x, p.x), 0.0F, std::max(high.z, p.z)};
    }
    const real_t width = high.x - low.x;
    const real_t depth = high.z - low.z;
    // A path that never came round is no circle: half the way across it went.
    out.turning_radius = static_cast<float>(std::max(width, depth) * 0.5F);
}

json::Value measure(std::optional<float> value, const std::array<float, 2>& range,
                    std::string_view name, json::Array& problems) {
    json::Value out;
    out["value"] = value ? json::Value(rounded(*value)) : json::Value{};
    const bool bounded = range[0] > 0.0F || range[1] > 0.0F;
    if (bounded) {
        out["range"] = json::Array{rounded(range[0]), rounded(range[1])};
    }
    bool ok = true;
    if (bounded && !value) {
        ok = false;
        problems.emplace_back(std::string(name) + " couldn't be measured: it never got there");
    } else if (value) {
        if (range[0] > 0.0F && *value < range[0]) {
            ok = false;
            problems.emplace_back(std::string(name) + " " + std::to_string(rounded(*value)) +
                                  " is under " + std::to_string(rounded(range[0])));
        }
        if (range[1] > 0.0F && *value > range[1]) {
            ok = false;
            problems.emplace_back(std::string(name) + " " + std::to_string(rounded(*value)) +
                                  " is over " + std::to_string(rounded(range[1])));
        }
    }
    out["ok"] = ok;
    return out;
}

Reply vehicle_check(Session& session, const json::Value& params) {
    world::World& world = session.world();
    std::string path;
    json::Value overrides;
    bool flat = false;
    float ppm = 50.0F;
    if (const json::Value* id = params.find("vehicle"); id != nullptr) {
        const std::optional<Found> found = vehicle_param(world, id->as_string());
        if (!found) {
            return not_a_vehicle(world, id->as_string(), "/vehicle");
        }
        flat = found->flat;
        if (flat) {
            const auto& v = world.ecs().get<world::Vehicle2D>(found->entity);
            path = v.recipe;
            overrides = v.overrides;
            ppm = v.pixels_per_meter;
        } else {
            const auto& v = world.ecs().get<world::Vehicle3D>(found->entity);
            path = v.recipe;
            overrides = v.overrides;
        }
    } else if (const json::Value* recipe = params.find("recipe"); recipe != nullptr) {
        path = recipe->as_string();
        if (const json::Value* space = params.find("space"); space != nullptr) {
            flat = space->as_string() == "2d";
        }
    } else {
        return param_error(code::kInvalidParams, "", "give a vehicle or a recipe");
    }
    auto loaded = recipe_param(world, path);
    if (!loaded) {
        return param_error(code::kInvalidParams, "/recipe", loaded.error().message);
    }
    auto resolved = world::load_vehicle_json(world, path);
    if (!resolved) {
        return param_error(code::kInvalidParams, "/recipe", resolved.error().message);
    }
    auto tuned = world::tuned_vehicle(resolved.value(), overrides);
    if (!tuned) {
        return param_error(code::kInvalidParams, "/vehicle", tuned.error().message);
    }
    const world::VehicleRecipe& recipe = tuned.value();
    const bool boat = recipe.kind == "boat";
    Measures measured;
    {
        const Drive drive = make_drive(session, path, overrides, flat, ppm, boat);
        straight_run(drive, measured);
    }
    if (!flat) {
        const Drive drive = make_drive(session, path, overrides, flat, ppm, boat);
        turning_run(drive, measured);
    }
    json::Array problems;
    json::Value result;
    result["recipe"] = path;
    result["space"] = flat ? "2d" : "3d";
    json::Value measures;
    measures["top_speed"] =
        measure(measured.top_speed, recipe.design.top_speed, "top_speed", problems);
    measures["acceleration"] =
        measure(measured.acceleration, recipe.design.acceleration, "acceleration", problems);
    measures["braking_distance"] = measure(
        measured.braking_distance, recipe.design.braking_distance, "braking_distance", problems);
    if (!flat) {
        measures["turning_radius"] = measure(measured.turning_radius, recipe.design.turning_radius,
                                             "turning_radius", problems);
    }
    result["measures"] = std::move(measures);
    result["passed"] = problems.empty();
    result["problems"] = std::move(problems);
    return Reply::success(std::move(result));
}

// ---- vehicle.design ----

constexpr std::array<std::string_view, 4> kMeasures{"top_speed", "acceleration", "braking_distance",
                                                    "turning_radius"};

std::optional<float> measured_of(const Measures& m, std::string_view name) {
    if (name == "top_speed") {
        return m.top_speed;
    }
    if (name == "acceleration") {
        return m.acceleration;
    }
    if (name == "braking_distance") {
        return m.braking_distance;
    }
    return m.turning_radius;
}

// The recipe's physics value at keys (under "physics"), set to value.
void set_physics(json::Value& recipe, std::initializer_list<std::string_view> keys, float value) {
    json::Value* at = &recipe["physics"];
    for (const std::string_view key : keys) {
        at = &(*at)[key];
    }
    *at = rounded(value, 1000.0);
}

// What vehicle.design changed, by the path of each value under the recipe's physics.
struct Fitted {
    json::Value& recipe;
    json::Value changes;

    void set(std::initializer_list<std::string_view> keys, float value) {
        set_physics(recipe, keys, value);
        std::string name;
        for (const std::string_view key : keys) {
            name += name.empty() ? "" : "/";
            name += key;
        }
        changes[name] = rounded(value, 1000.0);
    }
};

// The fastest a wheeled vehicle goes in its top gear at its rev limit, meters a second.
float rev_limit(const world::VehicleRecipe& recipe) {
    const world::VehiclePhysics& p = recipe.physics;
    if (recipe.wheels.empty() || p.gears.ratios.empty()) {
        return 0.0F;
    }
    const float gear = p.gears.ratios.back() * p.gears.final_drive;
    if (gear <= 0.0F) {
        return 0.0F;
    }
    constexpr float kRpmToRadians = 2.0F * std::numbers::pi_v<float> / 60.0F;
    return p.engine.max_rpm * kRpmToRadians * recipe.wheels.front().radius / gear;
}

// A final drive whose top gear reaches speed at nine tenths of the rev limit, if the one it
// has doesn't.
void reach_top_speed(const world::VehicleRecipe& recipe, float speed, Fitted& fitted) {
    const float limit = rev_limit(recipe);
    if (limit <= 0.0F || limit >= speed * 1.1F) {
        return;
    }
    fitted.set({"gears", "final_drive"},
               recipe.physics.gears.final_drive * limit / (speed * 1.15F));
}

// Moves the physics toward the targets, from how far each measure is off: torque (or thrust)
// for acceleration, drag for top speed, brake force for braking and steering (or the rudder)
// for the turning circle.
void fit_physics(const world::VehicleRecipe& recipe, const Measures& measured,
                 const std::map<std::string, float, std::less<>>& targets, Fitted& fitted) {
    const world::VehiclePhysics& p = recipe.physics;
    const bool boat = recipe.kind == "boat";
    const bool thrust = boat || recipe.kind == "aircraft";
    // How much more the engine pushes after this fit: acceleration comes first (targets are by
    // name), and the top speed's drag takes it into account.
    float power = 1.0F;
    for (const auto& [name, target] : targets) {
        const std::optional<float> value = measured_of(measured, name);
        // How far off: past 1 too slow, too far or too wide.
        const float off = value ? *value / target : 2.0F;
        if (std::abs(off - 1.0F) <= 0.1F) {
            continue;
        }
        if (name == "acceleration") {
            const float scale = std::clamp(off, 0.5F, 2.0F);
            power = scale;
            if (thrust) {
                fitted.set({"engine", "thrust"}, std::max(p.engine.thrust, 100.0F) * scale);
            } else {
                fitted.set({"engine", "max_torque"}, p.engine.max_torque * scale);
            }
        } else if (name == "top_speed") {
            if (!value) {
                continue;
            }
            const float ratio = *value / target;
            if (boat) {
                // Water drag takes the propeller's thrust as the square of speed.
                fitted.set({"water", "drag"},
                           p.water_drag * std::clamp(ratio * ratio, 0.25F, 4.0F) * power);
            } else {
                // Air drag takes the engine's power as the cube of speed (a thrust's as its
                // square).
                const float drag = thrust ? ratio * ratio : ratio * ratio * ratio;
                fitted.set({"drag"}, std::clamp(p.drag * std::clamp(drag, 0.125F, 8.0F) * power,
                                                0.01F, 5.0F));
                if (ratio < 1.0F && !thrust) {
                    reach_top_speed(recipe, target, fitted);
                }
            }
        } else if (name == "braking_distance") {
            if (!boat) {
                fitted.set({"brakes", "force"},
                           std::max(p.brake_force, 100.0F) * std::clamp(off, 0.5F, 2.0F));
            }
        } else if (name == "turning_radius") {
            if (boat) {
                fitted.set({"water", "rudder"}, p.rudder * std::clamp(off, 0.5F, 2.0F));
            } else if (recipe.kind == "tracked") {
                fitted.set({"steering", "max_angle"},
                           std::clamp(p.max_steer * std::clamp(off, 0.5F, 2.0F), 0.05F, 1.5F));
            } else {
                // The circle's radius goes as one over the tangent of the wheels' angle.
                const Vec2f turn = cos_sin(p.max_steer);
                const float tangent = (turn.y / turn.x) * std::clamp(off, 0.5F, 2.0F);
                fitted.set({"steering", "max_angle"},
                           std::clamp(slash::atan2(tangent, 1.0F), 0.05F, 1.0F));
            }
        }
    }
}

Reply vehicle_design(Session& session, const json::Value& params) {
    world::World& world = session.world();
    const std::string path = params.find("path")->as_string();
    if (!path.ends_with(".vehicle.json")) {
        return param_error(code::kInvalidParams, "/path", "isn't a .vehicle.json file");
    }
    json::Value recipe = *params.find("recipe");
    if (const auto errors = world::validate_vehicle(recipe); !errors.empty()) {
        return param_error(code::kInvalidParams, "/recipe" + errors.front().path,
                           errors.front().message);
    }
    bool flat = false;
    if (const json::Value* space = params.find("space"); space != nullptr) {
        flat = space->as_string() == "2d";
    }
    float ppm = 50.0F;
    if (const json::Value* given = params.find("pixels_per_meter"); given != nullptr) {
        ppm = static_cast<float>(given->as_double());
    }
    int iterations = 4;
    if (const json::Value* given = params.find("iterations"); given != nullptr) {
        iterations = static_cast<int>(given->as_int());
    }
    std::map<std::string, float, std::less<>> targets;
    if (const json::Value* given = params.find("targets"); given != nullptr) {
        for (const std::string_view name : kMeasures) {
            if (const json::Value* t = given->find(name); t != nullptr) {
                if (flat && name == "turning_radius") {
                    return param_error(code::kInvalidParams, "/targets/turning_radius",
                                       "a 2D vehicle doesn't turn");
                }
                targets.emplace(std::string(name), static_cast<float>(t->as_double()));
            }
        }
    }
    const auto parse = [&](const json::Value& doc) {
        return world::parse_vehicle(doc, world.file_reader());
    };
    auto parsed = parse(recipe);
    if (!parsed) {
        return param_error(code::kInvalidParams, "/recipe", parsed.error().message);
    }
    const bool boat = parsed.value().kind == "boat";
    if (boat && targets.contains("braking_distance")) {
        return param_error(code::kInvalidParams, "/targets/braking_distance",
                           "a boat has no brakes");
    }
    Fitted fitted{.recipe = recipe, .changes = json::Object{}};
    if (const auto top = targets.find("top_speed"); top != targets.end() &&
                                                    vehicle_rolls(parsed.value()) &&
                                                    parsed.value().kind != "aircraft") {
        reach_top_speed(parsed.value(), top->second, fitted);
    }
    const bool straight = std::ranges::any_of(
        targets, [](const auto& entry) { return entry.first != "turning_radius"; });
    Measures measured;
    int used = 0;
    for (int i = 0; i < iterations; ++i) {
        auto current = parse(recipe);
        if (!current) {
            return param_error(code::kInvalidParams, "/recipe", current.error().message);
        }
        const std::map<std::string, std::string, std::less<>> files{{path, json::write(recipe)}};
        measured = {};
        if (straight || targets.empty()) {
            const Drive drive = make_drive(session, path, {}, flat, ppm, boat, files);
            straight_run(drive, measured);
        }
        if (!flat && (targets.contains("turning_radius") || targets.empty())) {
            const Drive drive = make_drive(session, path, {}, flat, ppm, boat, files);
            turning_run(drive, measured);
        }
        ++used;
        const bool close = std::ranges::all_of(targets, [&](const auto& entry) {
            const std::optional<float> value = measured_of(measured, entry.first);
            return value && std::abs((*value / entry.second) - 1.0F) <= 0.1F;
        });
        if (close || i + 1 == iterations) {
            break;
        }
        fit_physics(current.value(), measured, targets, fitted);
    }
    // The design it was fitted to, so vehicle.check holds it there.
    json::Array problems;
    json::Value measures = json::Object{};
    for (const auto& [name, target] : targets) {
        recipe["design"][name] = json::Array{rounded(target * 0.85F), rounded(target * 1.15F)};
        const std::array<float, 2> range{target * 0.85F, target * 1.15F};
        json::Value m = measure(measured_of(measured, name), range, name, problems);
        m.as_object().erase("range");
        m["target"] = rounded(target);
        measures[name] = std::move(m);
    }
    if (targets.empty()) {
        for (const std::string_view name : kMeasures) {
            if (const std::optional<float> value = measured_of(measured, name)) {
                measures[name]["value"] = rounded(*value);
            }
        }
    }
    auto before = session.read_project_file(path);
    if (!before) {
        return Reply::failure(before.error());
    }
    std::string after = json::write(recipe);
    if (before.value() != after) {
        if (auto staged = session.stage_file(path, after); !staged) {
            return Reply::failure(staged.error());
        }
        session.add_undo_file(path, std::move(before).value(), std::move(after));
    }
    json::Value result;
    result["path"] = path;
    result["kind"] = parsed.value().kind;
    result["measures"] = std::move(measures);
    result["fitted"] = std::move(fitted.changes);
    result["test_drives"] = used;
    result["passed"] = problems.empty();
    if (!problems.empty()) {
        result["problems"] = std::move(problems);
    }
    return Reply::success(std::move(result));
}

// ---- vehicle.state ----

json::Value state_of(const world::World& world, const Found& found) {
    const auto& ecs = world.ecs();
    json::Value out;
    out["id"] = std::string(world.id_of(found.entity));
    const world::VehicleControls& controls = found.flat
                                                 ? ecs.get<world::Vehicle2D>(found.entity).controls
                                                 : ecs.get<world::Vehicle3D>(found.entity).controls;
    const world::VehicleDrive& drive = found.flat ? ecs.get<world::Vehicle2D>(found.entity).drive
                                                  : ecs.get<world::Vehicle3D>(found.entity).drive;
    out["recipe"] = found.flat ? ecs.get<world::Vehicle2D>(found.entity).recipe
                               : ecs.get<world::Vehicle3D>(found.entity).recipe;
    out["gear"] = static_cast<std::int64_t>(drive.gear);
    out["rpm"] = rounded(drive.rpm, 1.0);
    out["speed"] = rounded(drive.speed);
    out["steer"] = rounded(drive.steer, 1000.0);
    json::Value c;
    c["throttle"] = rounded(controls.throttle);
    c["brake"] = rounded(controls.brake);
    c["steer"] = rounded(controls.steer);
    out["controls"] = std::move(c);
    if (found.flat) {
        out["position"] = vec2(world::Tree2D(world).global_affine(found.entity).origin);
    } else {
        out["position"] = vec3(world::Tree3D(world).global(found.entity).origin);
    }
    json::Array riders;
    const auto view = ecs.view<const world::VehicleRider>();
    std::vector<entt::entity> all(view.begin(), view.end());
    std::reverse(all.begin(), all.end());
    for (const entt::entity rider : all) {
        const auto& ride = ecs.get<const world::VehicleRider>(rider);
        if (ride.vehicle == found.entity) {
            json::Value r;
            r["id"] = std::string(world.id_of(rider));
            r["seat"] = ride.seat;
            riders.push_back(std::move(r));
        }
    }
    out["riders"] = std::move(riders);
    return out;
}

Reply vehicle_state(Session& session, const json::Value& params) {
    const world::World& world = session.world();
    json::Array vehicles;
    if (const json::Value* id = params.find("vehicle"); id != nullptr) {
        const std::optional<Found> found = vehicle_param(world, id->as_string());
        if (!found) {
            return not_a_vehicle(world, id->as_string(), "/vehicle");
        }
        vehicles.push_back(state_of(world, *found));
    } else {
        for (const std::string_view each : world.ids()) {
            if (const std::optional<Found> found = vehicle_param(world, std::string(each))) {
                vehicles.push_back(state_of(world, *found));
            }
        }
    }
    json::Value result;
    result["vehicles"] = std::move(vehicles);
    return Reply::success(std::move(result));
}

json::Value point_property(std::string_view description) {
    json::Value at = property("array", description);
    at["items"]["type"] = "number";
    at["minItems"] = 2;
    at["maxItems"] = 3;
    return at;
}

json::Value section_property(std::string_view description) {
    json::Value out = property("object", description);
    return out;
}

} // namespace

void register_vehicle_commands(Registry& registry) {
    using enum Capability;
    json::Value ppm = property("number", "2D: pixels a meter. Default 50.");
    ppm["exclusiveMinimum"] = 0;
    registry.add({
        .name = "vehicle.place",
        .description = "Makes a vehicle of a .vehicle.json recipe: in 3D a RigidBody3D with a "
                       "MeshInstance3D drawing its model and a Vehicle3D; in 2D a RigidBody2D "
                       "and a Vehicle2D. The vehicles systems then build its wheels and drive "
                       "it. Returns its kind, seats and wheel count.",
        .capabilities = {Read, Edit},
        .effect = Effect::Edit,
        .params = schema_of({{"recipe", text_property("A .vehicle.json file.")},
                             {"id", text_property("The new entity's ID.")},
                             {"at", point_property("Where: [x, y] pixels in 2D, [x, y, z] "
                                                   "meters in 3D, on the ground.")},
                             {"parent", text_property("An entity to place it under.")},
                             {"yaw", property("number", "3D: radians it is turned about y.")},
                             {"pixels_per_meter", ppm},
                             {"flip_h", property("boolean", "2D: face -x.")}},
                            {"recipe", "id", "at"}),
        .handler = vehicle_place,
    });
    json::Value action = property("string", "drive (sets the controls given), enter (rider gets "
                                            "in, in seat or the first free) or exit.");
    action["enum"] = json::Array{"drive", "enter", "exit"};
    json::Value steer = unit_property("drive: -1 right to 1 left.", -1);
    json::Value pitch = unit_property("drive: aircraft, -1 nose down to 1 up.", -1);
    registry.add({
        .name = "vehicle.control",
        .description = "Drives a vehicle (throttle, brake, handbrake, steer, pitch; the rest "
                       "stay) or gets a rider in or out of its seats, as gameplay code does.",
        .capabilities = {Read, Edit},
        .effect = Effect::Edit,
        .params = schema_of({{"vehicle", text_property("The vehicle's ID.")},
                             {"action", std::move(action)},
                             {"throttle", unit_property("drive: -1 reverse to 1.", -1)},
                             {"brake", unit_property("drive: 0 to 1.", 0)},
                             {"handbrake", unit_property("drive: 0 to 1.", 0)},
                             {"steer", std::move(steer)},
                             {"pitch", std::move(pitch)},
                             {"rider", text_property("enter, exit: who.")},
                             {"seat", text_property("enter: the seat's name.")}},
                            {"vehicle", "action"}),
        .handler = vehicle_control,
    });
    json::Value reset = property("boolean", "Drop the overrides before these.");
    registry.add({
        .name = "vehicle.tune",
        .description = "Tunes one vehicle while it runs: physics, damage and design are merge "
                       "patches over its recipe's (as in .vehicle.json), kept in its overrides. "
                       "Refused when the result doesn't fit the schema.",
        .capabilities = {Read, Edit},
        .effect = Effect::Edit,
        .params = schema_of({{"vehicle", text_property("The vehicle's ID.")},
                             {"physics", section_property("Over its physics.")},
                             {"damage", section_property("Over its damage.")},
                             {"design", section_property("Over its design ranges.")},
                             {"reset", std::move(reset)}},
                            {"vehicle"}),
        .handler = vehicle_tune,
    });
    json::Value space = property("string", "recipe: drive it in 3d (default) or 2d.");
    space["enum"] = json::Array{"3d", "2d"};
    registry.add({
        .name = "vehicle.check",
        .description = "Test-drives a vehicle (or recipe) apart from the game, on flat ground or "
                       "open water, and holds it to its design: top speed, 0 to 100 km/h, "
                       "braking distance from 100 km/h and (3D) turning radius at 5 m/s. Returns "
                       "the measures and what is out of range.",
        .capabilities = {Read},
        .effect = Effect::Read,
        .params = schema_of({{"vehicle", text_property("A vehicle's ID; its overrides count.")},
                             {"recipe", text_property("Or a .vehicle.json file.")},
                             {"space", std::move(space)}},
                            {}),
        .handler = vehicle_check,
    });
    json::Value targets = property("object", "What to fit it to: top_speed (m/s), acceleration "
                                             "(s to 100 km/h), braking_distance (m from 100 "
                                             "km/h) and turning_radius (m at 5 m/s, 3D).");
    for (const std::string_view name : kMeasures) {
        json::Value t = property("number", "The measure wanted.");
        t["exclusiveMinimum"] = 0;
        targets["properties"][name] = std::move(t);
    }
    targets["additionalProperties"] = false;
    json::Value recipe = property("object", "The recipe, as a .vehicle.json file holds it: "
                                            "kind, body, parts, seats, wheels, physics and "
                                            "extends.");
    json::Value iterations = property("integer", "Test drives at most. Default 4.");
    iterations["minimum"] = 1;
    iterations["maximum"] = 8;
    json::Value design_space = property("string", "Test-drive it in 3d (default) or 2d.");
    design_space["enum"] = json::Array{"3d", "2d"};
    json::Value design_ppm = property("number", "2D: pixels a meter. Default 50.");
    design_ppm["exclusiveMinimum"] = 0;
    registry.add({
        .name = "vehicle.design",
        .description = "Writes a .vehicle.json recipe an agent designed (its body and parts, "
                       "wheels, seats and physics) after test-driving it apart from the game and "
                       "fitting its torque, drag, brakes and steering to the targets, which "
                       "become its design ranges. Returns the measures and what it changed.",
        .capabilities = {Read, Edit, Filesystem},
        .effect = Effect::Edit,
        .params = schema_of({{"path", text_property("The file to write.")},
                             {"recipe", std::move(recipe)},
                             {"targets", std::move(targets)},
                             {"space", std::move(design_space)},
                             {"pixels_per_meter", std::move(design_ppm)},
                             {"iterations", std::move(iterations)}},
                            {"path", "recipe"}),
        .handler = vehicle_design,
    });
    registry.add({
        .name = "vehicle.state",
        .description = "What vehicles are doing: gear, rpm, speed, steering, controls, position "
                       "and riders, of one or every vehicle by ID.",
        .capabilities = {Read},
        .effect = Effect::Read,
        .params = schema_of({{"vehicle", text_property("One vehicle's ID. Default all.")}}, {}),
        .handler = vehicle_state,
    });
}

} // namespace slash::command
