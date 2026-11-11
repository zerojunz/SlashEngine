#pragma once

// Vehicles as recipes, after Unreal's Chaos Vehicles (Godot has only VehicleBody3D): a
// .vehicle.json file says what a car, bike, truck, tracked vehicle, boat or aircraft looks like
// (its body, seats, doors and wheels, tracks, hull or wings) and its own physics to tune: mass
// and center of mass, the engine's torque curve, gears, the differential, steering, brakes,
// suspension, tires, buoyancy and lift. slash/scene3d/vehicle.hpp builds its 3D model; the
// vehicles systems of physics3d and physics2d (slash/physics3d/vehicle_tool.hpp,
// slash/physics2d/vehicle_tool.hpp) keep each Vehicle3D's and Vehicle2D's bodies and wheels and
// drive them with the drivetrain here.

#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/math.hpp"
#include "slash/core/result.hpp"
#include "slash/world/character.hpp"
#include "slash/world/world.hpp"

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slash::world {

inline constexpr std::string_view kVehicleSuffix = ".vehicle.json";

// Whether a path names a .vehicle.json file.
[[nodiscard]] bool is_vehicle_path(std::string_view path);

// The body, in meters, facing +z with y up and the ground at 0: a box from clearance up to
// height, a cabin on it, its colors, and a model file drawn in its place when given.
struct VehicleBody {
    float length = 4.2F;
    float width = 1.8F;
    float height = 1.4F;
    float clearance = 0.3F; // from the ground to the bottom of the body
    float cabin = 0.5F;     // the cabin's share of the length; 0 for none
    Vec4 color{0.7F, 0.1F, 0.1F, 1.0F};
    Vec4 trim{0.12F, 0.12F, 0.12F, 1.0F}; // wheels, tracks, windows' frames
    std::string model;                    // a model file drawn as the body
    bool draw = true; // false: only its parts draw, though it collides as its box still
};

// A part an agent builds a vehicle from, on top of (or, with body.draw off, in place of) the
// body its kind makes: one of Godot's primitive shapes a meter across (a "box", a "cylinder"
// standing along y, a "sphere" or a "prism" with its ridge along z and its point up), stretched
// to size, turned by rotation (radians, Godot's YXZ order) and put at position in the vehicle's
// space; or a model file placed the same way, which draws in 3D only. Its material is the
// body's paint, trim or glass, or one of its own color; one that collides adds its turned box to
// the vehicle's collision.
struct VehiclePart {
    std::string name;
    std::string shape = "box";
    std::string model;
    Vec3 position;
    Vec3 size{1.0F, 1.0F, 1.0F};
    Vec3 rotation;
    std::string material = "paint";
    std::optional<Vec4> color;
    bool collide = false;
};

// A seat: where whoever sits in it is, in the vehicle's space; the driver's seat drives it, and
// one with a door has a door beside it.
struct VehicleSeat {
    std::string name;
    Vec3 position;
    bool driver = false;
    bool door = false;
};

// A wheel: where its middle is at rest, in the vehicle's space; whether it steers, is driven and
// is held by the handbrake.
struct VehicleWheel {
    Vec3 position;
    float radius = 0.35F;
    float width = 0.25F;
    bool steer = false;
    bool drive = false;
    bool handbrake = false;
};

// The engine: torque in newton-meters at each share of max_rpm, by a curve of [share, share of
// max_torque] points.
struct VehicleEngine {
    float max_torque = 300.0F;
    float max_rpm = 6500.0F;
    float idle_rpm = 900.0F;
    std::vector<std::pair<float, float>> torque_curve{{0.0F, 0.6F}, {0.5F, 1.0F}, {1.0F, 0.75F}};
    float thrust = 0.0F; // newtons a boat's propeller or an aircraft's engines push at full
};

struct VehicleGears {
    std::vector<float> ratios{3.5F, 2.2F, 1.5F, 1.1F, 0.9F};
    float reverse = 3.2F;
    float final_drive = 3.7F;
    float shift_up = 0.85F;   // the share of max_rpm it shifts up at
    float shift_down = 0.35F; // and down at
    float shift_time = 0.25F; // seconds without torque while it shifts
    float efficiency = 0.9F;
};

// How it handles: the differential ("rear", "front" or "all", by front_share), steering, brakes,
// suspension, tires, and drag through the air (as a share of a car's).
struct VehiclePhysics {
    float mass = 1200.0F;
    Vec3 center_of_mass{0.0F, 0.45F, 0.0F};
    VehicleEngine engine;
    VehicleGears gears;
    std::string differential = "rear";
    float front_share = 0.5F;
    float max_steer = 0.6F;      // radians
    float steer_speed = 2.0F;    // radians a second the wheels turn
    float steer_at_speed = 0.5F; // how much less they turn at 30 meters a second
    float brake_force = 4000.0F; // newtons each wheel
    float handbrake_force = 6000.0F;
    float suspension_stiffness = 40.0F; // Godot's VehicleWheel3D units
    float suspension_damping = 2.5F;
    float suspension_relaxation = 2.8F;
    float suspension_travel = 0.2F;    // meters
    float suspension_rest = 0.15F;     // meters
    float suspension_max_force = 0.0F; // 0: four times its share of the weight
    float tire_friction = 10.5F;
    float roll_influence = 0.1F;
    float drag = 0.35F;       // air drag coefficient over its front's area
    float balance = 0.0F;     // how hard it keeps upright (bikes)
    float water_level = 0.0F; // boats: where the water's surface is, meters (2D: pixels) up
    float draft = 0.5F;       // boats: how deep the hull sits at rest
    float water_drag = 2.0F;  // boats
    float rudder = 1.5F;      // boats: how hard it turns
    float lift = 1.2F;        // aircraft: the wings' lift coefficient
    float control = 1.0F;     // aircraft: how hard its surfaces turn it
};

// What it does to characters and creatures it hits: damage a kilogram-meter a second of impulse,
// and the impulse that knocks a ragdoll down.
struct VehicleDamage {
    float per_impulse = 0.02F;
    float knockdown = 400.0F;
    float min_speed = 1.5F; // meters a second slower than this hurts no one
};

// What its design asks of a test drive, each a [min, max] range (0 for no bound): top speed in
// meters a second, seconds from 0 to 100 km/h, meters to stop from 100 km/h (or its top speed,
// when lower) and the turning circle's radius at 5 meters a second.
struct VehicleDesign {
    std::array<float, 2> top_speed{0.0F, 0.0F};
    std::array<float, 2> acceleration{0.0F, 0.0F};
    std::array<float, 2> braking_distance{0.0F, 0.0F};
    std::array<float, 2> turning_radius{0.0F, 0.0F};
};

struct VehicleRecipe {
    std::string name;
    std::string kind = "car"; // "car", "bike", "truck", "tracked", "boat" or "aircraft"
    VehicleBody body;
    std::vector<VehicleSeat> seats;
    std::vector<VehicleWheel> wheels;
    std::vector<VehiclePart> parts;
    float wing_span = 0.0F;  // aircraft
    float wing_chord = 0.0F; // aircraft
    VehiclePhysics physics;
    VehicleDamage damage;
    VehicleDesign design;
};

// The JSON Schema of .vehicle.json files.
[[nodiscard]] json::Value vehicle_schema();

// Every problem with a recipe's JSON, after its schema: no driver's seat, wheels on a boat, a
// curve out of order, gears out of order, a center of mass outside the body.
[[nodiscard]] std::vector<json::ValidationError> validate_vehicle(const json::Value& doc);

// The recipe a .vehicle.json file's JSON makes, its "extends" chain read with read_file. What
// is left out follows the kind: a car's four wheels, a bike's two, a truck's six, a tracked
// vehicle's road wheels, an aircraft's landing gear, its seats, and its physics' defaults.
[[nodiscard]] Result<VehicleRecipe> parse_vehicle(const json::Value& source,
                                                  const FileReader& read_file);
// The JSON a file's "extends" chain merges to, without "extends".
[[nodiscard]] Result<json::Value> resolve_vehicle(const json::Value& doc,
                                                  const FileReader& read_file);
// The recipe with overrides (a merge patch over its "physics", "damage" and "design", as
// vehicle.tune writes) applied; refused when they don't fit the schema.
[[nodiscard]] Result<VehicleRecipe> tuned_vehicle(const json::Value& resolved,
                                                  const json::Value& overrides);

// Whether it rolls on wheels (cars, bikes, trucks, tracked vehicles, aircraft on the ground).
[[nodiscard]] bool vehicle_rolls(const VehicleRecipe& recipe);

// A recipe a world's assets (or file reader) read, kept until one of its files changes.
[[nodiscard]] Result<std::shared_ptr<const VehicleRecipe>> load_vehicle(const World& world,
                                                                        std::string_view path);
// Its JSON, its "extends" chain merged, read the same way.
[[nodiscard]] Result<json::Value> load_vehicle_json(const World& world, std::string_view path);
// Gives a world the cache load_vehicle keeps recipes in; until then each load reads again.
void prepare_vehicles(World& world);

// What drives it this tick: throttle (-1 reverse to 1), brake and handbrake (0 to 1), steer (-1
// right to 1 left) and pitch (-1 down to 1 up, aircraft).
struct DriveInput {
    float throttle = 0.0F;
    float brake = 0.0F;
    float handbrake = 0.0F;
    float steer = 0.0F;
    float pitch = 0.0F;
};

// What the drivetrain gives the wheels: the force all driven wheels push with together (newtons,
// negative backward), each wheel's brake force and the handbrake's, and the steered wheels'
// angle (radians, positive left).
struct DriveOutput {
    float force = 0.0F;
    float brake = 0.0F;
    float handbrake = 0.0F;
    float steer = 0.0F;
};

// One tick of the drivetrain, as Chaos Vehicles' simple transmission: the engine's rpm from the
// driven wheels' (rpm, its share of max_rpm) is held between idle and max, the automatic gearbox
// shifts up and down (no torque while it shifts), the torque curve, gear and final drive give the
// wheels' force through their radius, throttle against the way it rolls brakes, and the wheels
// turn toward the steer at steer_speed, less at speed. gear: -1 reverse, 0 neutral, 1 on;
// speed: meters a second forward.
struct DriveState {
    int gear = 1;
    float rpm = 0.0F;
    float shift_timer = 0.0F;
    float steer = 0.0F;
};
[[nodiscard]] DriveOutput drive_step(const VehiclePhysics& physics, DriveState& state,
                                     const DriveInput& input, float wheel_rpm, float speed,
                                     float wheel_radius, float tick_seconds);

// The torque curve's share of max_torque at a share of max_rpm.
[[nodiscard]] float torque_share(const VehicleEngine& engine, float rpm_share);

// The parts of its 2D side view, in meters with y up and its nose toward +x (z of the recipe):
// the body's polygons (body, cabin, hull, wings) and each wheel's middle and radius on the near
// side (one a z).
struct VehicleSideView {
    struct Piece {
        std::string name;
        std::vector<Vec2> points;
        Vec4 color;
    };
    // The body's pieces (body, cabin, hull, frame, wing, tail, track), then each part made of a
    // shape, named by the part, as the outline of the shape turned and seen from the side.
    std::vector<Piece> body;
    std::vector<std::pair<Vec2, float>> wheels; // middle, radius
    std::vector<std::size_t> wheel_of;          // the recipe's wheel each side wheel is
};
[[nodiscard]] VehicleSideView vehicle_side_view(const VehicleRecipe& recipe);

} // namespace slash::world
