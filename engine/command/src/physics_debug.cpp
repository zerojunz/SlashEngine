#include "slash/command/physics_debug.hpp"

#include "slash/command/session.hpp"
#include "slash/core/text.hpp"
#include "slash/world/components.hpp"
#include "slash/world/physics_cache.hpp"
#include "slash/world/physics_debug.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// physics.debug, what physics did each step as Chaos's Visual Debugger shows it, and
// physics.cache, simulations recorded as animations as Chaos Cache records them.
namespace slash::command {
namespace {

json::Value property(std::string_view type, std::string_view description) {
    json::Value schema;
    schema["type"] = type;
    schema["description"] = description;
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

// To a thousandth, without a minus sign on zero.
double rounded(double value) {
    return (std::round(value * 1000.0) / 1000.0) + 0.0;
}

// A place or direction: [x, y] from a 2D step, [x, y, z] from a 3D one.
json::Value vec(Vec3 v, int dimensions) {
    if (dimensions == 2) {
        return json::Array{rounded(v.x), rounded(v.y)};
    }
    return json::Array{rounded(v.x), rounded(v.y), rounded(v.z)};
}

json::Value contact_value(const world::DebugContact& contact, int dimensions) {
    json::Value out;
    out["a"] = contact.a;
    out["b"] = contact.b;
    out["point"] = vec(contact.point, dimensions);
    out["normal"] = vec(contact.normal, dimensions);
    out["depth"] = rounded(contact.depth);
    out["speed"] = rounded(contact.speed);
    if (dimensions == 2) {
        out["impulse"] = rounded(contact.impulse);
    }
    return out;
}

json::Value joint_value(const world::DebugJoint& joint) {
    json::Value out;
    out["id"] = joint.id;
    out["kind"] = joint.kind;
    out["a"] = joint.a;
    if (!joint.b.empty()) {
        out["b"] = joint.b;
    }
    out["force"] = rounded(joint.force);
    out["stretch"] = rounded(joint.stretch);
    return out;
}

json::Value field_value(const world::DebugField& field, int dimensions) {
    json::Value out;
    out["body"] = field.body;
    if (field.force != Vec3{}) {
        out["force"] = vec(field.force, dimensions);
    }
    if (field.velocity) {
        out["velocity"] = vec(*field.velocity, dimensions);
    }
    if (field.strain > 0.0F) {
        out["strain"] = rounded(field.strain);
    }
    if (field.anchored) {
        out["anchored"] = true;
    }
    if (field.killed) {
        out["killed"] = true;
    }
    return out;
}

Reply physics_debug(Session& session, const json::Value& params) {
    world::World& world = session.world();
    if (const json::Value* record = params.find("record"); record != nullptr) {
        const auto keep = static_cast<std::size_t>(record->as_int());
        world::record_physics(world, keep);
        json::Value result;
        result["recording"] = static_cast<std::int64_t>(keep);
        return Reply::success(std::move(result));
    }
    const world::PhysicsDebug* debug = world::physics_debug(world);
    if (debug == nullptr) {
        return Reply::failure(code::kInvalidArgument,
                              R"(physics isn't recording: start with {"record": 120}, then step)");
    }
    std::string only;
    int dimensions = 0; // the entity's space's, or 0 for both
    if (const json::Value* id = params.find("id"); id != nullptr) {
        only = id->as_string();
        const entt::entity entity = world.find(only);
        if (entity != entt::null && world.ecs().all_of<world::Transform3D>(entity)) {
            dimensions = 3;
        } else if (entity != entt::null && world.ecs().all_of<world::Transform2D>(entity)) {
            dimensions = 2;
        }
        if (entity == entt::null) {
            std::string message = "no entity has the id " + quoted(only);
            if (const std::string_view hint = closest_match(only, world.ids()); !hint.empty()) {
                message += "; did you mean " + quoted(hint) + "?";
            }
            return param_error(code::kNotFound, "/id", message);
        }
    }
    std::uint64_t first = 0;
    std::uint64_t last = UINT64_MAX;
    std::size_t newest = 1;
    if (const json::Value* ticks = params.find("ticks"); ticks != nullptr) {
        first = static_cast<std::uint64_t>(ticks->as_array()[0].as_int());
        last = static_cast<std::uint64_t>(ticks->as_array()[1].as_int());
        newest = debug->steps().size();
    }
    if (const json::Value* count = params.find("last"); count != nullptr) {
        newest = static_cast<std::size_t>(count->as_int());
    }
    const auto touches = [&](std::string_view a, std::string_view b) {
        return only.empty() || a == only || b == only;
    };
    // The steps of the newest ticks asked for, oldest first.
    std::vector<const world::PhysicsDebugStep*> picked;
    std::set<std::uint64_t> ticks_seen;
    for (const world::PhysicsDebugStep& step : std::views::reverse(debug->steps())) {
        if (step.tick < first || step.tick > last ||
            (dimensions != 0 && step.dimensions != dimensions)) {
            continue;
        }
        if (!ticks_seen.contains(step.tick) && ticks_seen.size() == newest) {
            continue;
        }
        ticks_seen.insert(step.tick);
        picked.push_back(&step);
    }
    std::ranges::reverse(picked);
    json::Array steps;
    for (const world::PhysicsDebugStep* step : picked) {
        json::Value out;
        out["tick"] = static_cast<std::int64_t>(step->tick);
        out["dimensions"] = step->dimensions;
        out["awake"] = step->awake;
        out["sleeping"] = step->sleeping;
        json::Array contacts;
        for (const world::DebugContact& contact : step->contacts) {
            if (touches(contact.a, contact.b)) {
                contacts.push_back(contact_value(contact, step->dimensions));
            }
        }
        json::Array joints;
        for (const world::DebugJoint& joint : step->joints) {
            if (touches(joint.a, joint.b) || joint.id == only) {
                joints.push_back(joint_value(joint));
            }
        }
        json::Array fields;
        for (const world::DebugField& field : step->fields) {
            if (touches(field.body, field.body)) {
                fields.push_back(field_value(field, step->dimensions));
            }
        }
        if (!contacts.empty()) {
            out["contacts"] = std::move(contacts);
        }
        if (!joints.empty()) {
            out["joints"] = std::move(joints);
        }
        if (!fields.empty()) {
            out["fields"] = std::move(fields);
        }
        steps.push_back(std::move(out));
    }
    json::Value result;
    result["recording"] = static_cast<std::int64_t>(debug->keep());
    result["steps"] = std::move(steps);
    return Reply::success(std::move(result));
}

// ---- physics.cache

constexpr std::string_view kAnimSuffix = ".anim.json";
constexpr double kTwoPi = 6.283185307179586;

double fine(double value) {
    return (std::round(value * 10000.0) / 10000.0) + 0.0;
}

// A track's values at a key: up to three numbers.
struct CacheKey {
    double time = 0.0;
    std::array<double, 3> value{};
};

// The keys left when a straight line between their neighbors passes within tolerance of the
// others (Ramer, Douglas and Peucker, by the largest difference of any number); one key when the
// track never moves.
std::vector<CacheKey> reduced(const std::vector<CacheKey>& keys, std::size_t size,
                              double tolerance) {
    const auto off = [&](const CacheKey& key, const std::array<double, 3>& line) {
        double most = 0.0;
        for (std::size_t i = 0; i < size; ++i) {
            most = std::max(most, std::abs(key.value[i] - line[i]));
        }
        return most;
    };
    if (keys.size() <= 1 || std::ranges::all_of(keys, [&](const CacheKey& key) {
            return off(key, keys.front().value) <= tolerance;
        })) {
        return keys.empty() ? keys : std::vector<CacheKey>{keys.front()};
    }
    std::vector<bool> keep(keys.size(), false);
    keep.front() = true;
    keep.back() = true;
    std::vector<std::pair<std::size_t, std::size_t>> spans{{0, keys.size() - 1}};
    while (!spans.empty()) {
        const auto [a, b] = spans.back();
        spans.pop_back();
        double worst = tolerance;
        std::size_t at = 0;
        for (std::size_t i = a + 1; i < b; ++i) {
            const double t = (keys[i].time - keys[a].time) / (keys[b].time - keys[a].time);
            std::array<double, 3> line{};
            for (std::size_t n = 0; n < size; ++n) {
                line[n] = keys[a].value[n] + ((keys[b].value[n] - keys[a].value[n]) * t);
            }
            if (const double away = off(keys[i], line); away > worst) {
                worst = away;
                at = i;
            }
        }
        if (at != 0) {
            keep[at] = true;
            spans.emplace_back(a, at);
            spans.emplace_back(at, b);
        }
    }
    std::vector<CacheKey> out;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (keep[i]) {
            out.push_back(keys[i]);
        }
    }
    return out;
}

json::Value key_value(const CacheKey& key, std::size_t size) {
    if (size == 1) {
        return fine(key.value[0]);
    }
    json::Array numbers;
    for (std::size_t i = 0; i < size; ++i) {
        numbers.emplace_back(fine(key.value[i]));
    }
    return numbers;
}

json::Value value_track(const std::string& target, const std::string& path,
                        const std::vector<CacheKey>& keys, std::size_t size) {
    json::Array out;
    for (const CacheKey& key : keys) {
        json::Value entry;
        entry["time"] = fine(key.time);
        entry["value"] = key_value(key, size);
        out.push_back(std::move(entry));
    }
    json::Value track;
    track["keys"] = std::move(out);
    track["path"] = path;
    track["target"] = target;
    return track;
}

// A body's recording as tracks: its place and turn, keyed where they bend, and frozen as a
// kinematic body from the start, so physics carries what it hits instead of moving it.
void add_tracks(json::Array& tracks, const std::string& id, const world::CacheTrack& track,
                double tolerance, std::size_t& keys) {
    const bool flat = track.dimensions == 2;
    std::vector<CacheKey> places;
    std::vector<CacheKey> turns;
    std::array<double, 3> last{};
    for (std::size_t i = 0; i < track.samples.size(); ++i) {
        const world::CacheSample& sample = track.samples[i];
        const CacheKey place{.time = sample.time,
                             .value = {sample.position.x, sample.position.y, sample.position.z}};
        CacheKey turn{.time = sample.time,
                      .value = {sample.rotation.x, sample.rotation.y, sample.rotation.z}};
        // Angles go the short way from the last key, so they turn as the body did.
        for (std::size_t n = 0; n < 3; ++n) {
            if (i > 0) {
                turn.value[n] += kTwoPi * std::round((last[n] - turn.value[n]) / kTwoPi);
            }
            last[n] = turn.value[n];
        }
        places.push_back(place);
        turns.push_back(turn);
    }
    const std::string kind = flat ? "2D" : "3D";
    const std::size_t place_size = flat ? 2 : 3;
    const std::size_t turn_size = flat ? 1 : 3;
    places = reduced(places, place_size, tolerance);
    turns = reduced(turns, turn_size, tolerance);
    keys += places.size() + turns.size();
    tracks.push_back(value_track(id, "Transform" + kind + "/position", places, place_size));
    tracks.push_back(value_track(id, "Transform" + kind + "/rotation", turns, turn_size));
    json::Value frozen;
    frozen["time"] = 0;
    frozen["value"] = true;
    json::Value kinematic;
    kinematic["time"] = 0;
    kinematic["value"] = "kinematic";
    json::Value freeze;
    freeze["keys"] = json::Array{std::move(frozen)};
    freeze["path"] = "RigidBody" + kind + "/freeze";
    freeze["target"] = id;
    json::Value mode;
    mode["keys"] = json::Array{std::move(kinematic)};
    mode["path"] = "RigidBody" + kind + "/freeze_mode";
    mode["target"] = id;
    tracks.push_back(std::move(mode));
    tracks.push_back(std::move(freeze));
    keys += 2;
}

Reply cache_record(Session& session, const json::Value& ids) {
    world::World& world = session.world();
    std::set<entt::entity> only;
    const json::Array& names = ids.as_array();
    for (std::size_t i = 0; i < names.size(); ++i) {
        const std::string& id = names[i].as_string();
        const entt::entity entity = world.find(id);
        const std::string path = "/record/" + std::to_string(i);
        if (entity == entt::null) {
            std::string message = "no entity has the id " + quoted(id);
            if (const std::string_view hint = closest_match(id, world.ids()); !hint.empty()) {
                message += "; did you mean " + quoted(hint) + "?";
            }
            return param_error(code::kNotFound, path, message);
        }
        if (!world.ecs().any_of<world::RigidBody3D, world::RigidBody2D>(entity)) {
            return param_error(code::kInvalidParams, path,
                               quoted(id) + " has no RigidBody3D or RigidBody2D");
        }
        only.insert(entity);
    }
    world::PhysicsCache& cache = world::start_physics_cache(world, only);
    json::Array recording;
    const entt::registry& ecs = world.ecs();
    for (const auto [entity, rigid, transform] :
         ecs.view<const world::RigidBody3D, const world::Transform3D>().each()) {
        if (cache.records(entity) && !world.id_of(entity).empty()) {
            cache.add_start(std::string(world.id_of(entity)), 3, transform.position,
                            transform.rotation);
        }
    }
    for (const auto [entity, rigid, transform] :
         ecs.view<const world::RigidBody2D, const world::Transform2D>().each()) {
        if (cache.records(entity) && !world.id_of(entity).empty()) {
            cache.add_start(std::string(world.id_of(entity)), 2,
                            {transform.position.x, transform.position.y, 0.0F},
                            {transform.rotation, 0.0F, 0.0F});
        }
    }
    for (const auto& [id, track] : cache.tracks()) {
        recording.emplace_back(id);
    }
    json::Value result;
    result["recording"] = std::move(recording);
    return Reply::success(std::move(result));
}

Reply cache_save(Session& session, const json::Value& params) {
    world::World& world = session.world();
    world::PhysicsCache* cache = world::physics_cache(world);
    if (cache == nullptr) {
        return Reply::failure(code::kInvalidArgument,
                              R"(nothing is recording: start with {"record": []}, then step)");
    }
    const std::string out = params.find("save")->as_string();
    if (!out.ends_with(kAnimSuffix)) {
        return param_error(code::kInvalidParams, "/save", quoted(out) + " must end in .anim.json");
    }
    std::string name = "cache";
    if (const json::Value* given = params.find("name"); given != nullptr) {
        name = given->as_string();
    }
    double tolerance = 0.001;
    if (const json::Value* given = params.find("tolerance"); given != nullptr) {
        tolerance = given->as_double();
    }
    entt::entity player = entt::null;
    if (const json::Value* given = params.find("player"); given != nullptr) {
        player = world.find(given->as_string());
        if (player == entt::null) {
            std::string message = "no entity has the id " + quoted(given->as_string());
            if (const std::string_view hint = closest_match(given->as_string(), world.ids());
                !hint.empty()) {
                message += "; did you mean " + quoted(hint) + "?";
            }
            return param_error(code::kNotFound, "/player", message);
        }
    }
    json::Array tracks;
    json::Array bodies;
    std::size_t keys = 0;
    double length = 0.0;
    for (const auto& [id, track] : cache->tracks()) {
        if (track.samples.empty()) {
            continue;
        }
        add_tracks(tracks, id, track, tolerance, keys);
        bodies.emplace_back(id);
        length = std::max(length, track.samples.back().time);
    }
    if (tracks.empty()) {
        return Reply::failure(code::kInvalidArgument,
                              "the recording has no bodies: record rigid bodies, then step");
    }
    json::Value animation;
    animation["length"] = std::max(fine(length), 0.001);
    animation["tracks"] = std::move(tracks);
    json::Value file;
    file["animations"][name] = std::move(animation);
    file["format"] = 1;
    auto before = session.read_project_file(out);
    if (!before) {
        return Reply::failure(before.error());
    }
    std::string after = json::write(file);
    if (before.value() != after) {
        if (auto staged = session.stage_file(out, after); !staged) {
            return Reply::failure(staged.error());
        }
        session.add_undo_file(out, std::move(before).value(), std::move(after));
    }
    json::Value result;
    result["out"] = out;
    result["animation"] = name;
    result["length"] = fine(length);
    result["bodies"] = bodies;
    result["keys"] = static_cast<std::int64_t>(keys);
    if (player != entt::null) {
        auto& ecs = world.ecs();
        world::AnimationPlayer animation_player = ecs.all_of<world::AnimationPlayer>(player)
                                                      ? ecs.get<world::AnimationPlayer>(player)
                                                      : world::AnimationPlayer{};
        animation_player.library = out;
        animation_player.animation = name;
        animation_player.playing = false;
        animation_player.time = 0.0;
        for (const json::Value& id : bodies) {
            animation_player.targets[id.as_string()] = world.find(id.as_string());
        }
        ecs.emplace_or_replace<world::AnimationPlayer>(player, std::move(animation_player));
        result["player"] = std::string(world.id_of(player));
    }
    if (!session.dry_running()) {
        world::stop_physics_cache(world);
    }
    return Reply::success(std::move(result));
}

Reply physics_cache(Session& session, const json::Value& params) {
    const json::Value* record = params.find("record");
    const json::Value* save = params.find("save");
    const bool stop = params.find("stop") != nullptr && params.find("stop")->as_bool();
    if (static_cast<int>(record != nullptr) + static_cast<int>(save != nullptr) +
            static_cast<int>(stop) !=
        1) {
        return Reply::failure(code::kInvalidParams,
                              "give one of record (start), save (write it) or stop (drop it)");
    }
    if (record != nullptr) {
        return cache_record(session, *record);
    }
    if (save != nullptr) {
        return cache_save(session, params);
    }
    world::stop_physics_cache(session.world());
    json::Value result;
    result["recording"] = json::Array{};
    return Reply::success(std::move(result));
}

} // namespace

void register_physics_debug_commands(Registry& registry) {
    using enum Capability;
    json::Value record = property("integer", "Start recording afresh, keeping the last this many "
                                             "steps; 0 stops and forgets.");
    record["minimum"] = 0;
    record["maximum"] = 3600;
    json::Value ticks = property("array", "Only steps from tick first to last, [first, last].");
    ticks["items"]["type"] = "integer";
    ticks["items"]["minimum"] = 0;
    ticks["minItems"] = 2;
    ticks["maxItems"] = 2;
    json::Value last = property("integer", "Only the newest this many ticks. Default 1, or all "
                                           "with ticks.");
    last["minimum"] = 1;
    last["maximum"] = 3600;
    json::Value id = property("string", "Only what touches this entity.");
    id["minLength"] = 1;
    json::Value params;
    params["type"] = "object";
    params["additionalProperties"] = false;
    params["properties"] = json::Object{
        {"record", std::move(record)},
        {"ticks", std::move(ticks)},
        {"last", std::move(last)},
        {"id", std::move(id)},
    };
    registry.add({
        .name = "physics.debug",
        .description =
            "Physics' visual debugger. record starts keeping each step; then returns steps: "
            "contacts (bodies, point, normal, depth, closing speed), joints (force, stretch), "
            "fields' effect on each body, and bodies awake and asleep.",
        .capabilities = {Read},
        .effect = Effect::Read,
        .params = std::move(params),
        .handler = physics_debug,
    });
    json::Value cache_ids = property("array", "Start recording these rigid bodies' places "
                                              "each step; [] records every one.");
    cache_ids["items"]["type"] = "string";
    cache_ids["maxItems"] = 1024;
    json::Value cache_save = property("string", "Stop and write the recording to this "
                                                ".anim.json file.");
    cache_save["minLength"] = 1;
    json::Value cache_name = property("string", "The animation's name. Default \"cache\".");
    cache_name["minLength"] = 1;
    json::Value cache_tolerance =
        property("number", "Keys within this of a straight line are dropped. Default 0.001.");
    cache_tolerance["minimum"] = 0;
    json::Value cache_player = property("string", "With save: an entity whose AnimationPlayer "
                                                  "(made if missing) gets the file and targets.");
    cache_player["minLength"] = 1;
    json::Value cache_params;
    cache_params["type"] = "object";
    cache_params["additionalProperties"] = false;
    cache_params["properties"] = json::Object{
        {"record", std::move(cache_ids)},
        {"save", std::move(cache_save)},
        {"name", std::move(cache_name)},
        {"tolerance", std::move(cache_tolerance)},
        {"player", std::move(cache_player)},
        {"stop", property("boolean", "Stop and drop the recording.")},
    };
    registry.add({
        .name = "physics.cache",
        .description = "Records a simulation to play back the same every time, as Chaos "
                       "Cache: record starts, save writes the bodies' places as a .anim.json "
                       "animation (frozen kinematic while it plays) for an AnimationPlayer or "
                       "cinematic.",
        .capabilities = {Read, Edit},
        .effect = Effect::Edit,
        .params = std::move(cache_params),
        .handler = physics_cache,
    });
}

} // namespace slash::command
