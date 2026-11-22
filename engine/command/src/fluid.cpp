// fluid.place, fluid.pour, fluid.state, fluid.check and fluid.preview, over .fluid.json recipes
// (slash/world/fluid.hpp): put a body of liquid or gas in a scene, pour into it, read what its
// particles are doing, and check recipes on a stage of their own.
#include "slash/world/fluid.hpp"

#include "procedural.hpp"

#include "slash/canvas/canvas.hpp"
#include "slash/command/session.hpp"
#include "slash/core/text.hpp"
#include "slash/render/image.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slash::command {

namespace {

constexpr double kTick = 1.0 / 60.0;

json::Value property(std::string_view type, std::string_view description) {
    json::Value schema;
    schema["type"] = type;
    schema["description"] = description;
    return schema;
}

json::Value point_property(std::string_view description) {
    json::Value schema = property("array", description);
    schema["items"]["type"] = "number";
    schema["minItems"] = 2;
    schema["maxItems"] = 3;
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

Reply unknown_id(const world::World& world, std::string_view id, const std::string& path) {
    std::string message = "no entity has the id " + quoted(id);
    if (const std::string_view hint = closest_match(id, world.ids()); !hint.empty()) {
        message += "; did you mean " + quoted(hint) + "?";
    }
    return param_error(code::kInvalidParams, path, message);
}

double rounded(double v) {
    return std::round(v * 1000.0) / 1000.0;
}

json::Value vector_value(Vec3f v, bool flat) {
    json::Array out{rounded(v.x), rounded(v.y)};
    if (!flat) {
        out.emplace_back(rounded(v.z));
    }
    return out;
}

// The file's name without its folder and suffix: "liquids/water.fluid.json" is "water".
std::string stem_of(std::string_view path) {
    if (const std::size_t slash = path.rfind('/'); slash != std::string_view::npos) {
        path.remove_prefix(slash + 1);
    }
    if (path.ends_with(world::kFluidSuffix)) {
        path.remove_suffix(world::kFluidSuffix.size());
    }
    std::string out;
    for (const char c : path) {
        const bool word =
            (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        out.push_back(word ? c : '_');
    }
    return out.empty() ? "fluid" : out;
}

std::string free_id(const world::World& world, const std::string& name) {
    if (world.find(name) == entt::null) {
        return name;
    }
    for (int n = 2;; ++n) {
        std::string id = name + "_" + std::to_string(n);
        if (world.find(id) == entt::null) {
            return id;
        }
    }
}

Result<std::shared_ptr<const world::FluidRecipe>>
recipe_at(Session& session, const std::string& path, const json::Value& overrides = {}) {
    if (!world::is_fluid_path(path)) {
        return make_error(ErrorCode::InvalidArgument, quoted(path) + " is not a .fluid.json file");
    }
    auto text = session.read_project_file(path);
    if (!text) {
        return text.error();
    }
    if (!text.value()) {
        return make_error(ErrorCode::NotFound, quoted(path) + " doesn't exist");
    }
    return world::load_fluid(session.world(), path, overrides);
}

// Reads [x, y] or [x, y, z] at a param into out, or returns the reply to fail with.
std::optional<Reply> read_point(const json::Value* given, bool flat, const std::string& path,
                                Vec3& out) {
    if (given == nullptr) {
        return std::nullopt;
    }
    const json::Array& a = given->as_array();
    if (a.size() != (flat ? 2U : 3U)) {
        return param_error(code::kInvalidParams, path,
                           flat ? "a 2d fluid takes [x, y] pixels"
                                : "a 3d fluid takes [x, y, z] meters");
    }
    out = {static_cast<real_t>(a[0].as_double()), static_cast<real_t>(a[1].as_double()),
           flat ? 0.0F : static_cast<real_t>(a[2].as_double())};
    return std::nullopt;
}

// ---- fluid.place ----

Reply fluid_place(Session& session, const json::Value& params) {
    world::World& world = session.world();
    auto& ecs = world.ecs();
    const std::string& path = params.find("recipe")->as_string();
    json::Value overrides;
    if (const json::Value* given = params.find("overrides"); given != nullptr) {
        overrides = *given;
    }
    auto loaded = recipe_at(session, path, overrides);
    if (!loaded) {
        return param_error(code::kInvalidParams, "/recipe", loaded.error().message);
    }
    const world::FluidRecipe& recipe = *loaded.value();
    const bool flat = recipe.flat;
    Vec3 at;
    Vec3 fill;
    Vec3 bounds;
    Vec3 velocity;
    if (auto failed = read_point(params.find("at"), flat, "/at", at); failed) {
        return *failed;
    }
    if (auto failed = read_point(params.find("fill"), flat, "/fill", fill); failed) {
        return *failed;
    }
    if (auto failed = read_point(params.find("bounds"), flat, "/bounds", bounds); failed) {
        return *failed;
    }
    if (auto failed = read_point(params.find("velocity"), flat, "/velocity", velocity); failed) {
        return *failed;
    }
    entt::entity parent = entt::null;
    if (const json::Value* given = params.find("parent"); given != nullptr) {
        parent = world.find(given->as_string());
        if (parent == entt::null) {
            return unknown_id(world, given->as_string(), "/parent");
        }
        if (flat ? !ecs.all_of<world::Transform2D>(parent)
                 : !ecs.all_of<world::Transform3D>(parent)) {
            return param_error(code::kInvalidParams, "/parent",
                               quoted(given->as_string()) + " has no " +
                                   (flat ? "Transform2D" : "Transform3D"));
        }
    }
    std::string id;
    if (const json::Value* given = params.find("id"); given != nullptr) {
        id = given->as_string();
        if (world.find(id) != entt::null) {
            return param_error(code::kInvalidParams, "/id", quoted(id) + " is taken");
        }
    } else {
        id = free_id(world, stem_of(path));
    }
    auto made = world.create(id);
    if (!made) {
        return param_error(code::kInvalidParams, "/id", made.error().message);
    }
    const entt::entity entity = made.value();
    // How many particles the fill makes, as the system lays them out.
    const real_t spacing = 2.0F * recipe.radius;
    const auto along = [&](real_t extent) {
        return extent > 0.0F ? static_cast<std::size_t>(std::floor(extent / spacing)) : 0U;
    };
    std::size_t filled = along(fill.x) * along(fill.y) * (flat ? 1U : along(fill.z));
    filled = std::min({filled, recipe.max_particles, world::kMaxFluidParticles});
    if (flat) {
        world::Transform2D place;
        place.position = {at.x, at.y};
        ecs.emplace<world::Transform2D>(entity, place);
        world::Fluid2D fluid;
        fluid.recipe = path;
        fluid.overrides = overrides;
        fluid.fill_size = {fill.x, fill.y};
        fluid.fill_velocity = {velocity.x, velocity.y};
        fluid.bounds_size = {bounds.x, bounds.y};
        ecs.emplace<world::Fluid2D>(entity, std::move(fluid));
        if (parent != entt::null) {
            ecs.get<world::Transform2D>(parent).children.push_back(entity);
        }
    } else {
        world::Transform3D place;
        place.position = at;
        ecs.emplace<world::Transform3D>(entity, place);
        world::Fluid3D fluid;
        fluid.recipe = path;
        fluid.overrides = overrides;
        fluid.fill_size = fill;
        fluid.fill_velocity = velocity;
        fluid.bounds_size = bounds;
        ecs.emplace<world::Fluid3D>(entity, std::move(fluid));
        if (parent != entt::null) {
            ecs.get<world::Transform3D>(parent).children.push_back(entity);
        }
    }
    json::Value result;
    result["id"] = id;
    result["kind"] = recipe.gas ? "gas" : "liquid";
    result["space"] = flat ? "2d" : "3d";
    result["particles"] = filled;
    result["volume"] = rounded(static_cast<double>(filled) * world::fluid_particle_volume(recipe));
    return Reply::success(std::move(result));
}

// ---- fluid.pour ----

Reply fluid_pour(Session& session, const json::Value& params) {
    world::World& world = session.world();
    auto& ecs = world.ecs();
    const std::string& fluid_id = params.find("fluid")->as_string();
    const entt::entity fluid = world.find(fluid_id);
    if (fluid == entt::null) {
        return unknown_id(world, fluid_id, "/fluid");
    }
    const bool flat = ecs.all_of<world::Fluid2D>(fluid);
    if (!flat && !ecs.all_of<world::Fluid3D>(fluid)) {
        return param_error(code::kInvalidParams, "/fluid",
                           quoted(fluid_id) + " has no Fluid3D or Fluid2D to pour into");
    }
    Vec3 at;
    if (auto failed = read_point(params.find("at"), flat, "/at", at); failed) {
        return *failed;
    }
    Vec3 direction = flat ? Vec3{0.0F, 1.0F, 0.0F} : Vec3{0.0F, -1.0F, 0.0F};
    if (auto failed = read_point(params.find("direction"), flat, "/direction", direction); failed) {
        return *failed;
    }
    std::string id;
    if (const json::Value* given = params.find("id"); given != nullptr) {
        id = given->as_string();
        if (world.find(id) != entt::null) {
            return param_error(code::kInvalidParams, "/id", quoted(id) + " is taken");
        }
    } else {
        id = free_id(world, fluid_id + "_pour");
    }
    auto made = world.create(id);
    if (!made) {
        return param_error(code::kInvalidParams, "/id", made.error().message);
    }
    const entt::entity entity = made.value();
    const auto number = [&](std::string_view name, float fallback) {
        const json::Value* given = params.find(name);
        return given != nullptr ? static_cast<float>(given->as_double()) : fallback;
    };
    const auto fill = [&](world::FluidEmitter& emitter) {
        emitter.fluid = fluid_id;
        emitter.rate = number("rate", emitter.rate);
        emitter.speed = number("speed", emitter.speed);
        emitter.spread = number("spread", emitter.spread);
        emitter.radius = number("radius", emitter.radius);
        emitter.temperature = number("temperature", emitter.temperature);
        emitter.smoke = number("smoke", emitter.smoke);
        emitter.fuel = number("fuel", emitter.fuel);
    };
    if (flat) {
        world::Transform2D place;
        place.position = {at.x, at.y};
        ecs.emplace<world::Transform2D>(entity, place);
        world::FluidEmitter2D emitter;
        fill(emitter);
        emitter.direction = {direction.x, direction.y};
        ecs.emplace<world::FluidEmitter2D>(entity, std::move(emitter));
    } else {
        world::Transform3D place;
        place.position = at;
        ecs.emplace<world::Transform3D>(entity, place);
        world::FluidEmitter3D emitter;
        fill(emitter);
        emitter.direction = direction;
        ecs.emplace<world::FluidEmitter3D>(entity, std::move(emitter));
    }
    json::Value result;
    result["id"] = id;
    return Reply::success(std::move(result));
}

// ---- fluid.state ----

json::Value fluid_summary(const world::World& world, entt::entity entity, bool flat,
                          const std::string& recipe_path, const json::Value& overrides,
                          std::size_t points) {
    json::Value out;
    out["recipe"] = recipe_path;
    auto loaded = world::load_fluid(world, recipe_path, overrides);
    if (!loaded) {
        out["error"] = loaded.error().message;
        return out;
    }
    const world::FluidRecipe& recipe = *loaded.value();
    out["kind"] = recipe.gas ? "gas" : "liquid";
    const world::FluidParticles* particles = world::fluid_particles(world, entity);
    const std::size_t n = particles != nullptr ? particles->size() : 0;
    out["particles"] = n;
    out["volume"] = rounded(static_cast<double>(n) * world::fluid_particle_volume(recipe));
    if (n == 0) {
        return out;
    }
    Vec3f low = particles->position[0];
    Vec3f high = low;
    Vec3f middle;
    double speed_sum = 0.0;
    float fastest = 0.0F;
    double density = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const Vec3f p = particles->position[i];
        low = {std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z)};
        high = {std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z)};
        middle = middle + p;
        const Vec3f v = particles->velocity[i];
        const float speed = std::sqrt((v.x * v.x) + (v.y * v.y) + (v.z * v.z));
        speed_sum += speed;
        fastest = std::max(fastest, speed);
        density += particles->density[i];
    }
    const auto count = static_cast<double>(n);
    const float scale = 1.0F / static_cast<float>(n);
    out["bounds"] = json::Array{vector_value(low, flat), vector_value(high, flat)};
    out["middle"] = vector_value(middle * scale, flat);
    out["speed"] = rounded(speed_sum / count);
    out["fastest"] = rounded(fastest);
    if (!recipe.gas) {
        out["density"] = rounded(density / count);
    } else {
        double temperature = 0.0;
        float hottest = -1e30F;
        double smoke = 0.0;
        double fuel = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            temperature += particles->temperature[i];
            hottest = std::max(hottest, particles->temperature[i]);
            smoke += particles->smoke[i];
            fuel += particles->fuel[i];
        }
        out["temperature"] = rounded(temperature / count);
        out["hottest"] = rounded(hottest);
        out["smoke"] = rounded(smoke / count);
        out["fuel"] = rounded(fuel);
    }
    out["contacts"] = particles->contacts;
    out["work"] = particles->work;
    if (points > 0) {
        // Evenly through the particles, so the sample covers the whole body.
        json::Array sample;
        const std::size_t taken = std::min(points, n);
        for (std::size_t k = 0; k < taken; ++k) {
            sample.emplace_back(vector_value(particles->position[k * n / taken], flat));
        }
        out["points"] = std::move(sample);
    }
    return out;
}

Reply fluid_state(Session& session, const json::Value& params) {
    const world::World& world = session.world();
    const auto& ecs = world.ecs();
    std::vector<std::string> wanted;
    if (const json::Value* ids = params.find("ids"); ids != nullptr) {
        for (std::size_t i = 0; i < ids->as_array().size(); ++i) {
            const std::string& id = ids->as_array()[i].as_string();
            const entt::entity found = world.find(id);
            if (found == entt::null) {
                return unknown_id(world, id, "/ids/" + std::to_string(i));
            }
            if (!ecs.any_of<world::Fluid3D, world::Fluid2D>(found)) {
                return param_error(code::kInvalidParams, "/ids/" + std::to_string(i),
                                   quoted(id) + " has no Fluid3D or Fluid2D");
            }
            wanted.push_back(id);
        }
    }
    const std::size_t points = params.find("points") != nullptr
                                   ? static_cast<std::size_t>(params.find("points")->as_int())
                                   : 0U;
    json::Value fluids = json::Object{};
    const auto included = [&](std::string_view id) {
        return wanted.empty() || std::ranges::find(wanted, id) != wanted.end();
    };
    for (const auto [entity, fluid] : ecs.view<const world::Fluid3D>().each()) {
        const std::string id(world.id_of(entity));
        if (!id.empty() && included(id)) {
            fluids[id] = fluid_summary(world, entity, false, fluid.recipe, fluid.overrides, points);
        }
    }
    for (const auto [entity, fluid] : ecs.view<const world::Fluid2D>().each()) {
        const std::string id(world.id_of(entity));
        if (!id.empty() && included(id)) {
            fluids[id] = fluid_summary(world, entity, true, fluid.recipe, fluid.overrides, points);
        }
    }
    json::Value result;
    result["fluids"] = std::move(fluids);
    return Reply::success(std::move(result));
}

// ---- fluid.check and fluid.preview ----

SessionOptions stage_options(const Session& host) {
    SessionOptions options = host.options();
    options.capture_logs = false;
    options.write_files = false;
    std::erase(options.capabilities, Capability::Build);
    std::erase(options.capabilities, Capability::Network);
    return options;
}

// Sets a recipe alone on a stage: a liquid in a tank from a block, a gas pouring from a vent.
// Returns the fluid's entity.
entt::entity build_stage(world::World& world, const std::string& path,
                         const world::FluidRecipe& recipe) {
    auto& ecs = world.ecs();
    const bool flat = recipe.flat;
    const float r = recipe.radius;
    // A tank twelve particles wide, a block eight high in it; a gas pours for the whole run.
    const float wide = 24.0F * r;
    const entt::entity tank = world.create("stage_fluid").value();
    const entt::entity vent = recipe.gas ? world.create("stage_vent").value() : entt::null;
    if (flat) {
        ecs.emplace<world::Transform2D>(tank, world::Transform2D{});
        world::Fluid2D fluid;
        fluid.recipe = path;
        if (!recipe.gas) {
            fluid.fill_size = {wide * 0.8F, 16.0F * r};
            fluid.bounds_size = {wide, 64.0F * r};
        }
        ecs.emplace<world::Fluid2D>(tank, std::move(fluid));
        if (vent != entt::null) {
            ecs.emplace<world::Transform2D>(vent, world::Transform2D{});
            world::FluidEmitter2D emitter;
            emitter.fluid = "stage_fluid";
            emitter.direction = {0.0F, -1.0F};
            emitter.temperature = recipe.ignition + 50.0F;
            emitter.fuel = 0.5F;
            emitter.rate = 120.0F;
            ecs.emplace<world::FluidEmitter2D>(vent, std::move(emitter));
        }
    } else {
        ecs.emplace<world::Transform3D>(tank, world::Transform3D{});
        world::Fluid3D fluid;
        fluid.recipe = path;
        if (!recipe.gas) {
            fluid.fill_size = {wide * 0.8F, 16.0F * r, wide * 0.8F};
            fluid.bounds_size = {wide, 64.0F * r, wide};
        }
        ecs.emplace<world::Fluid3D>(tank, std::move(fluid));
        if (vent != entt::null) {
            ecs.emplace<world::Transform3D>(vent, world::Transform3D{});
            world::FluidEmitter3D emitter;
            emitter.fluid = "stage_fluid";
            emitter.direction = {0.0F, 1.0F, 0.0F};
            emitter.speed = 0.5F;
            emitter.radius = 4.0F * r;
            emitter.temperature = recipe.ignition + 50.0F;
            emitter.fuel = 0.5F;
            emitter.rate = 300.0F;
            ecs.emplace<world::FluidEmitter3D>(vent, std::move(emitter));
        }
    }
    return tank;
}

// Runs a recipe alone for seconds on its stage and says what came of it.
json::Value stage_run(Session& session, const std::string& path, const world::FluidRecipe& recipe,
                      double seconds) {
    Session stage(session.components(), session.commands(), stage_options(session));
    world::World& world = stage.world();
    const bool flat = recipe.flat;
    const float r = recipe.radius;
    const entt::entity tank = build_stage(world, path, recipe);
    const int ticks = static_cast<int>(std::ceil(seconds / kTick));
    std::size_t start = 0;
    std::size_t most_work = 0;
    std::size_t most_particles = 0;
    for (int t = 0; t < ticks; ++t) {
        world.step(kTick);
        const world::FluidParticles* particles = world::fluid_particles(world, tank);
        if (particles == nullptr) {
            continue;
        }
        if (t == 0) {
            start = particles->size();
        }
        most_work = std::max(most_work, particles->work);
        most_particles = std::max(most_particles, particles->size());
    }
    json::Value out = fluid_summary(world, tank, flat, path, {}, 0);
    out.as_object().erase("recipe");
    out["seconds"] = rounded(seconds);
    out["most_particles"] = most_particles;
    out["most_work"] = most_work;
    if (!recipe.gas) {
        out["kept"] =
            start == 0 ? 1.0
                       : rounded(static_cast<double>(world::fluid_particles(world, tank)->size()) /
                                 static_cast<double>(start));
        // Settled: slower than a particle's width a tenth of a second.
        const json::Value* fastest = out.find("fastest");
        out["settled"] = fastest != nullptr && fastest->as_double() < 20.0 * r;
    }
    return out;
}

json::Value recipe_check(Session& session, const std::string& path, double seconds, bool stage) {
    json::Value out;
    auto text = session.read_project_file(path);
    if (!text) {
        out["error"] = text.error().message;
        return out;
    }
    const std::optional<std::string>& body = text.value();
    if (!body) {
        out["error"] = quoted(path) + " doesn't exist";
        return out;
    }
    auto doc = json::parse(*body);
    if (!doc) {
        out["error"] = doc.error().message;
        return out;
    }
    json::Array errors;
    for (const json::ValidationError& error : world::validate_fluid(doc.value())) {
        errors.emplace_back(error.path + ": " + error.message);
    }
    if (!errors.empty()) {
        out["errors"] = std::move(errors);
        return out;
    }
    auto loaded = world::load_fluid(session.world(), path);
    if (!loaded) {
        out["error"] = loaded.error().message;
        return out;
    }
    const world::FluidRecipe& recipe = *loaded.value();
    out["kind"] = recipe.gas ? "gas" : "liquid";
    out["space"] = recipe.flat ? "2d" : "3d";
    if (!stage) {
        return out;
    }
    json::Value run = stage_run(session, path, recipe, seconds);
    json::Array over;
    if (recipe.budget_particles > 0 &&
        static_cast<std::size_t>(run["most_particles"].as_int()) > recipe.budget_particles) {
        over.emplace_back("particles");
    }
    if (recipe.budget_work > 0 &&
        static_cast<std::size_t>(run["most_work"].as_int()) > recipe.budget_work) {
        over.emplace_back("work");
    }
    out["stage"] = std::move(run);
    if (!over.empty()) {
        out["over_budget"] = std::move(over);
    }
    return out;
}

Reply fluid_check(Session& session, const json::Value& params) {
    const world::World& world = session.world();
    const auto& ecs = world.ecs();
    const double seconds =
        params.find("seconds") != nullptr ? params.find("seconds")->as_double() : 2.0;
    const bool stage = params.find("stage") == nullptr || params.find("stage")->as_bool();
    std::vector<std::string> paths;
    if (const json::Value* given = params.find("recipe"); given != nullptr) {
        if (!world::is_fluid_path(given->as_string())) {
            return param_error(code::kInvalidParams, "/recipe",
                               quoted(given->as_string()) + " is not a .fluid.json file");
        }
        paths.push_back(given->as_string());
    }
    // The world's fluids, emitters and sinks: what can't run or names nothing.
    json::Array problems;
    std::vector<std::pair<std::string, bool>> fluids;
    const auto problem = [&](std::string_view id, std::string message) {
        json::Value p;
        p["id"] = id;
        p["error"] = std::move(message);
        problems.emplace_back(std::move(p));
    };
    const auto check_fluid = [&](entt::entity entity, const std::string& recipe,
                                 const json::Value& overrides, bool flat) {
        const std::string id(world.id_of(entity));
        fluids.emplace_back(id, flat);
        if (params.find("recipe") == nullptr && std::ranges::find(paths, recipe) == paths.end() &&
            world::is_fluid_path(recipe)) {
            paths.push_back(recipe);
        }
        auto loaded = world::load_fluid(world, recipe, overrides);
        if (!loaded) {
            problem(id, loaded.error().message);
        } else if (loaded.value()->flat != flat) {
            problem(id, quoted(recipe) + " is a " + (flat ? "3d" : "2d") + " fluid, which a " +
                            (flat ? "Fluid2D" : "Fluid3D") + " can't step");
        } else if (const world::FluidParticles* particles = world::fluid_particles(world, entity);
                   particles != nullptr) {
            const world::FluidRecipe& r = *loaded.value();
            if (r.budget_particles > 0 && particles->size() > r.budget_particles) {
                problem(id, "holds " + std::to_string(particles->size()) +
                                " particles, past its budget of " +
                                std::to_string(r.budget_particles));
            }
            if (r.budget_work > 0 && particles->work > r.budget_work) {
                problem(id, "took " + std::to_string(particles->work) +
                                " neighbor visits last tick, past its budget of " +
                                std::to_string(r.budget_work));
            }
        }
    };
    for (const auto [entity, fluid] : ecs.view<const world::Fluid3D>().each()) {
        check_fluid(entity, fluid.recipe, fluid.overrides, false);
    }
    for (const auto [entity, fluid] : ecs.view<const world::Fluid2D>().each()) {
        check_fluid(entity, fluid.recipe, fluid.overrides, true);
    }
    const auto check_named = [&](entt::entity entity, const std::string& fluid, bool flat,
                                 bool may_be_empty) {
        if (fluid.empty() && may_be_empty) {
            return;
        }
        const auto found = std::ranges::find_if(
            fluids, [&](const std::pair<std::string, bool>& f) { return f.first == fluid; });
        if (found == fluids.end()) {
            std::vector<std::string> names;
            names.reserve(fluids.size());
            for (const auto& [name, f] : fluids) {
                names.push_back(name);
            }
            std::string message = "names " + quoted(fluid) + ", which is no fluid";
            if (const std::string_view hint = closest_match(fluid, names); !hint.empty()) {
                message += "; did you mean " + quoted(hint) + "?";
            }
            problem(world.id_of(entity), std::move(message));
        } else if (found->second != flat) {
            problem(world.id_of(entity), std::string("names a ") + (found->second ? "2D" : "3D") +
                                             " fluid from " + (flat ? "2D" : "3D"));
        }
    };
    for (const auto [entity, e] : ecs.view<const world::FluidEmitter3D>().each()) {
        check_named(entity, e.fluid, false, false);
    }
    for (const auto [entity, e] : ecs.view<const world::FluidEmitter2D>().each()) {
        check_named(entity, e.fluid, true, false);
    }
    for (const auto [entity, s] : ecs.view<const world::FluidSink3D>().each()) {
        check_named(entity, s.fluid, false, true);
    }
    for (const auto [entity, s] : ecs.view<const world::FluidSink2D>().each()) {
        check_named(entity, s.fluid, true, true);
    }
    json::Value recipes = json::Object{};
    for (const std::string& path : paths) {
        recipes[path] = recipe_check(session, path, seconds, stage);
    }
    json::Value result;
    result["recipes"] = std::move(recipes);
    if (!problems.empty()) {
        result["problems"] = std::move(problems);
    }
    return Reply::success(std::move(result));
}

} // namespace

// A camera on a recipe's stage: a liquid's tank from in front and above, a gas's plume from
// in front; in 2D, the tank or the plume in the middle of the view.
void stage_camera(world::World& world, const world::FluidRecipe& recipe, Vec2 view) {
    auto& ecs = world.ecs();
    const float r = recipe.radius;
    const entt::entity camera = world.create("stage_camera").value();
    if (recipe.flat) {
        world::Transform2D place;
        // A liquid lies on its tank's floor, 32 radii down; a gas rises from the vent.
        place.position = recipe.gas ? Vec2{0.0F, -24.0F * r} : Vec2{0.0F, 20.0F * r};
        ecs.emplace<world::Transform2D>(camera, place);
        world::Camera2D lens;
        // The stage's 48 radii across fill most of the view.
        const float zoom = std::max(static_cast<float>(view.y) / (48.0F * r), 0.01F);
        lens.zoom = {zoom, zoom};
        lens.limit_enabled = false;
        ecs.emplace<world::Camera2D>(camera, lens);
        return;
    }
    world::Transform3D place;
    if (recipe.gas) {
        place.position = {0.0F, 30.0F * r, 90.0F * r};
    } else {
        place.position = {0.0F, -12.0F * r, 48.0F * r};
        place.rotation = {-0.45F, 0.0F, 0.0F};
    }
    ecs.emplace<world::Transform3D>(camera, place);
    world::Camera3D lens;
    lens.current = true;
    ecs.emplace<world::Camera3D>(camera, lens);
    const entt::entity sun = world.create("stage_sun").value();
    world::Transform3D sun_place;
    sun_place.rotation = {-0.9F, 0.5F, 0.0F};
    ecs.emplace<world::Transform3D>(sun, sun_place);
    ecs.emplace<world::SunLight3D>(sun);
}

Reply fluid_preview(Session& session, const json::Value& params) {
    const std::string& path = params.find("recipe")->as_string();
    const json::Value overrides =
        params.find("overrides") != nullptr ? *params.find("overrides") : json::Value{};
    auto loaded = recipe_at(session, path, overrides);
    if (!loaded) {
        return param_error(code::kInvalidParams, "/recipe", loaded.error().message);
    }
    const world::FluidRecipe& recipe = *loaded.value();
    std::vector<double> times;
    if (const json::Value* given = params.find("times"); given != nullptr) {
        for (const json::Value& t : given->as_array()) {
            times.push_back(t.as_double());
        }
    } else {
        times = {0.5, 1.5};
    }
    std::ranges::sort(times);
    times.erase(std::ranges::unique(times).begin(), times.end());
    if (times.empty() || times.back() > 60.0) {
        return param_error(code::kInvalidParams, "/times", "give times from 0 to 60 seconds");
    }
    const json::Value* capture = params.find("capture");
    std::string out;
    std::string full;
    double capture_at = 0.0;
    Vec2 size{320.0F, 240.0F};
    if (capture != nullptr) {
        out = capture->find("out") != nullptr ? capture->find("out")->as_string()
                                              : "previews/" + stem_of(path) + ".png";
        if (!out.ends_with(".png")) {
            return param_error(code::kInvalidParams, "/capture/out",
                               quoted(out) + " must end in .png");
        }
        auto resolved = session.resolve_path(out);
        if (!resolved) {
            return param_error(code::kInvalidParams, "/capture/out", resolved.error().message);
        }
        full = resolved.value();
        capture_at = capture->find("at")->as_double();
        if (const json::Value* given = capture->find("size"); given != nullptr) {
            size = {static_cast<real_t>(given->as_array()[0].as_double()),
                    static_cast<real_t>(given->as_array()[1].as_double())};
        }
    }
    Session stage(session.components(), session.commands(), stage_options(session));
    world::World& world = stage.world();
    const entt::entity tank = build_stage(world, path, recipe);
    if (!overrides.is_null()) {
        if (auto* f3 = world.ecs().try_get<world::Fluid3D>(tank); f3 != nullptr) {
            f3->overrides = overrides;
        }
        if (auto* f2 = world.ecs().try_get<world::Fluid2D>(tank); f2 != nullptr) {
            f2->overrides = overrides;
        }
    }
    stage_camera(world, recipe, size);
    json::Array samples;
    std::size_t next = 0;
    bool captured = capture == nullptr;
    json::Value result;
    const int last = static_cast<int>(std::ceil(std::max(times.back(), capture_at) / kTick));
    for (int tick = 0; tick <= last; ++tick) {
        world.step(kTick);
        const double t = static_cast<double>(tick + 1) * kTick;
        while (next < times.size() && times[next] <= t + 1e-9) {
            json::Value sample = fluid_summary(world, tank, recipe.flat, path, {}, 0);
            sample.as_object().erase("recipe");
            sample["t"] = rounded(times[next]);
            samples.emplace_back(std::move(sample));
            ++next;
        }
        if (!captured && capture_at <= t + 1e-9) {
            canvas::CaptureOptions options;
            options.viewport = size;
            auto image = canvas::capture(world, stage.assets(), options);
            if (!image) {
                return Reply::failure(image.error());
            }
            const render::Image& picture = image.value().image;
            // How much of the frame the fluid covers: pixels that differ from the clear color.
            std::size_t changed = 0;
            const std::size_t pixels = picture.pixels.size() / 4;
            for (std::size_t i = 0; i < pixels; ++i) {
                const std::size_t at = i * 4;
                if (picture.pixels[at] != picture.pixels[0] ||
                    picture.pixels[at + 1] != picture.pixels[1] ||
                    picture.pixels[at + 2] != picture.pixels[2]) {
                    ++changed;
                }
            }
            auto png = render::encode_png(picture);
            if (!png) {
                return Reply::failure(png.error());
            }
            if (auto written = session.write_file(full, png.value()); !written) {
                return Reply::failure(written.error());
            }
            json::Value shot;
            shot["path"] = out;
            shot["size"] = json::Array{static_cast<std::int64_t>(picture.width),
                                       static_cast<std::int64_t>(picture.height)};
            shot["covered"] =
                pixels == 0 ? 0.0
                            : rounded(static_cast<double>(changed) / static_cast<double>(pixels));
            result["capture"] = std::move(shot);
            captured = true;
        }
        if (next >= times.size() && captured) {
            break;
        }
    }
    result["recipe"] = path;
    result["samples"] = std::move(samples);
    return Reply::success(std::move(result));
}

void register_fluid_commands(Registry& registry) {
    {
        json::Value schema;
        schema["type"] = "object";
        schema["additionalProperties"] = false;
        schema["required"] = json::Array{"recipe"};
        json::Value& p = schema["properties"];
        p["recipe"] = property("string", "A .fluid.json file.");
        p["id"] = property("string", "The new entity's ID; default the file's name.");
        p["at"] = point_property("Where: [x, y, z] meters, or [x, y] pixels for a 2d fluid.");
        p["fill"] = point_property("The size of a block of it to start with, at rest; default "
                                   "none (emitters pour it).");
        p["bounds"] = point_property("The size of a box it stays in; default none.");
        p["velocity"] = point_property("The starting block's velocity.");
        p["parent"] = property("string", "An entity to place it under, moving with it.");
        p["overrides"] = property("object", "Fields over the recipe's, as a JSON Merge Patch.");
        registry.add({
            .name = "fluid.place",
            .description = "Places a .fluid.json liquid or gas as a new Fluid3D or Fluid2D, "
                           "with a block of it at rest and a box to keep it in.",
            .capabilities = {Capability::Edit},
            .effect = Effect::Edit,
            .params = std::move(schema),
            .handler = fluid_place,
        });
    }
    {
        json::Value schema;
        schema["type"] = "object";
        schema["additionalProperties"] = false;
        schema["required"] = json::Array{"fluid"};
        json::Value& p = schema["properties"];
        p["fluid"] = property("string", "The Fluid3D or Fluid2D entity to pour into.");
        p["id"] = property("string", "The new emitter's ID; default <fluid>_pour.");
        p["at"] = point_property("Where it pours from.");
        p["direction"] = point_property("The way it pours; default down.");
        p["rate"] = property("number", "Particles a second. Default 200.");
        p["speed"] = property("number", "Speed it leaves at. Default 2 m/s (120 px/s in 2D).");
        p["spread"] = property("number", "The most it turns from direction, in radians.");
        p["radius"] = property("number", "The mouth's radius.");
        p["temperature"] = property("number", "Gases: degrees it leaves at.");
        p["smoke"] = property("number", "Gases: smoke density it carries.");
        p["fuel"] = property("number", "Gases: fuel it carries, which burns above ignition.");
        registry.add({
            .name = "fluid.pour",
            .description = "Adds an emitter that pours into a fluid: a tap, hose, vent, chimney "
                           "or flame.",
            .capabilities = {Capability::Edit},
            .effect = Effect::Edit,
            .params = std::move(schema),
            .handler = fluid_pour,
        });
    }
    {
        json::Value schema;
        schema["type"] = "object";
        schema["additionalProperties"] = false;
        json::Value& p = schema["properties"];
        p["ids"] = property("array", "Fluids to report; default all.");
        p["ids"]["items"]["type"] = "string";
        p["points"] = property("integer", "Also return this many particles' places.");
        p["points"]["minimum"] = 0;
        p["points"]["maximum"] = 256;
        registry.add({
            .name = "fluid.state",
            .description = "Each fluid's particles, volume, bounds, middle, speeds, density (or "
                           "a gas's temperature, smoke and fuel), contacts and work.",
            .capabilities = {Capability::Read},
            .effect = Effect::Read,
            .params = std::move(schema),
            .handler = fluid_state,
        });
    }
    {
        json::Value schema;
        schema["type"] = "object";
        schema["additionalProperties"] = false;
        json::Value& p = schema["properties"];
        p["recipe"] = property("string", "A .fluid.json file; default every recipe the world's "
                                         "fluids name.");
        p["stage"] = property("boolean", "Run each recipe alone (a tank for a liquid, a vent "
                                         "for a gas). Default true.");
        p["seconds"] = property("number", "How long the stage runs. Default 2.");
        p["seconds"]["minimum"] = 0.1;
        p["seconds"]["maximum"] = 30;
        registry.add({
            .name = "fluid.check",
            .description = "Checks .fluid.json recipes (errors; on a stage: particles kept, "
                           "settling, density, rise, work against the budget) and the world's "
                           "fluids, emitters and sinks. $ slash-cli schema fluid.",
            .capabilities = {Capability::Read},
            .effect = Effect::Read,
            .params = std::move(schema),
            .handler = fluid_check,
        });
    }
    {
        json::Value schema;
        schema["type"] = "object";
        schema["additionalProperties"] = false;
        schema["required"] = json::Array{"recipe"};
        json::Value& p = schema["properties"];
        p["recipe"] = property("string", "A .fluid.json file.");
        p["overrides"] = property("object", "A merge patch over the recipe, to try a change.");
        p["times"] = property("array", "Seconds into the stage to sample; default 0.5 and 1.5.");
        p["times"]["items"]["type"] = "number";
        p["times"]["items"]["minimum"] = 0;
        json::Value& c = p["capture"];
        c = property("object", "Also draw it into a PNG at a time.");
        c["additionalProperties"] = false;
        c["required"] = json::Array{"at"};
        c["properties"]["at"] = property("number", "Seconds into the stage.");
        c["properties"]["at"]["minimum"] = 0;
        c["properties"]["out"] =
            property("string", "The PNG to write; default previews/<file>.png.");
        c["properties"]["size"] = point_property("[width, height] pixels; default [320, 240].");
        registry.add({
            .name = "fluid.preview",
            .description = "Runs a .fluid.json recipe alone on its stage (a liquid's tank, a "
                           "gas's vent) and returns what it does at times; optionally draws a "
                           "PNG, saying how much of the frame the fluid covers.",
            .capabilities = {Capability::Read, Capability::Filesystem},
            .effect = Effect::Read,
            .params = std::move(schema),
            .handler = fluid_preview,
        });
    }
}

} // namespace slash::command
