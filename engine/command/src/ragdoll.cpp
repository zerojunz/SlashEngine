#include "slash/command/ragdoll.hpp"

#include "slash/asset/assets.hpp"
#include "slash/command/session.hpp"
#include "slash/core/text.hpp"
#include "slash/physics2d/ragdoll.hpp"
#include "slash/scene3d/model.hpp"
#include "slash/scene3d/ragdoll.hpp"
#include "slash/world/components.hpp"
#include "slash/world/ragdoll.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

// ragdoll.generate, ragdoll.hit and ragdoll.tune, over slash/scene3d/ragdoll.hpp (Ragdoll3D) and
// slash/physics2d/ragdoll.hpp (Ragdoll2D).
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

double rounded(double value) {
    return std::round(value * 1000.0) / 1000.0;
}

json::Value vec(Vec3 v) {
    return json::Array{rounded(v.x), rounded(v.y), rounded(v.z)};
}

// The entity an "id" names, or a reply saying why there is none.
Result<entt::entity> entity_of(Session& session, const json::Value& params) {
    const std::string& id = params.find("id")->as_string();
    const entt::entity entity = session.world().find(id);
    if (entity == entt::null) {
        std::string message = "no entity has the id " + quoted(id);
        if (const std::string_view hint = closest_match(id, session.world().ids()); !hint.empty()) {
            message += "; did you mean " + quoted(hint) + "?";
        }
        return make_error(ErrorCode::NotFound, std::move(message));
    }
    return entity;
}

// A bone's settings as a profile has them, after defaults and overrides.
json::Value bone_value(const world::RagdollBone& bone) {
    json::Value out;
    out["shape"] = bone.shape;
    if (bone.shape == "box" || bone.shape == "rectangle") {
        out["size"] = vec(bone.size);
    } else {
        out["radius"] = rounded(bone.radius);
    }
    if (bone.shape == "capsule") {
        out["height"] = rounded(bone.height);
    }
    out["mass"] = rounded(bone.mass);
    out["joint"] = bone.joint;
    if (bone.joint == "cone") {
        out["swing_span"] = rounded(bone.swing_span);
        out["twist_span"] = rounded(bone.twist_span);
    } else if (bone.joint == "hinge") {
        out["lower"] = rounded(bone.lower);
        out["upper"] = rounded(bone.upper);
    }
    out["drive"] = bone.drive;
    out["stiffness"] = rounded(bone.stiffness);
    out["damping"] = rounded(bone.damping);
    out["pin"] = rounded(bone.pin);
    out["pin_damping"] = rounded(bone.pin_damping);
    out["strength"] = rounded(bone.strength);
    return out;
}

// Writes a generated profile where params' out says (or default_out) and stages its undo.
Reply write_profile(Session& session, const json::Value& params, const json::Value& file,
                    const std::string& default_out, json::Value& result, std::string& out) {
    out = default_out;
    if (const json::Value* given = params.find("out"); given != nullptr) {
        out = given->as_string();
    }
    if (!world::is_ragdoll_path(out)) {
        return param_error(code::kInvalidParams, "/out",
                           quoted(out) + " must end in .ragdoll.json");
    }
    auto before = session.read_project_file(out);
    if (!before) {
        return Reply::failure(before.error());
    }
    std::string after = json::write(file);
    result["out"] = out;
    json::Array names;
    for (const auto& [name, bone] : file.find("bones")->as_object()) {
        names.emplace_back(name);
    }
    result["bones"] = std::move(names);
    if (before.value() != after) {
        if (auto staged = session.stage_file(out, after); !staged) {
            return Reply::failure(staged.error());
        }
        session.add_undo_file(out, std::move(before).value(), std::move(after));
    }
    return Reply::success({});
}

template <typename Options>
Options options_of(const json::Value& params) {
    Options options;
    const auto number = [&](std::string_view key, float& into) {
        if (const json::Value* value = params.find(key); value != nullptr) {
            into = static_cast<float>(value->as_double());
        }
    };
    number("mass", options.mass);
    number("stiffness", options.stiffness);
    number("damping", options.damping);
    number("pin", options.pin);
    number("pin_damping", options.pin_damping);
    number("min_length", options.min_length);
    return options;
}

// A 2D profile from the Transform2D tree under an entity, set as its Ragdoll2D's.
Reply ragdoll_generate_2d(Session& session, const json::Value& params, entt::entity entity) {
    world::World& world = session.world();
    auto made =
        physics2d::generate_ragdoll(world, entity, options_of<physics2d::RagdollOptions>(params));
    if (!made) {
        return param_error(code::kInvalidParams, "/id", made.error().message);
    }
    std::string id(world.id_of(entity));
    std::ranges::replace(id, '/', '_');
    json::Value result;
    std::string out;
    if (Reply written = write_profile(session, params, made.value(),
                                      "ragdolls/" + id + ".ragdoll.json", result, out);
        !written.ok) {
        return written;
    }
    auto& ragdoll = world.ecs().get_or_emplace<world::Ragdoll2D>(entity);
    if (ragdoll.profile != out) {
        ragdoll.profile = out;
    }
    return Reply::success(std::move(result));
}

Reply ragdoll_generate(Session& session, const json::Value& params) {
    world::World& world = session.world();
    entt::entity entity{entt::null};
    std::string model_path;
    if (params.find("id") != nullptr) {
        auto found = entity_of(session, params);
        if (!found) {
            return param_error(code::kNotFound, "/id", found.error().message);
        }
        entity = found.value();
        const auto* instance = world.ecs().try_get<const world::MeshInstance3D>(entity);
        if ((instance == nullptr || instance->mesh.empty()) && params.find("model") == nullptr &&
            world.ecs().any_of<world::Transform2D, world::Ragdoll2D>(entity)) {
            return ragdoll_generate_2d(session, params, entity);
        }
        if (instance == nullptr || instance->mesh.empty()) {
            return param_error(code::kInvalidParams, "/id",
                               quoted(params.find("id")->as_string()) +
                                   " has no MeshInstance3D drawing a model");
        }
        model_path = instance->mesh;
    }
    if (const json::Value* model = params.find("model"); model != nullptr) {
        model_path = model->as_string();
    }
    if (model_path.empty()) {
        return Reply::failure(code::kInvalidParams, "give id (an entity drawing a model) or model");
    }
    auto loaded = session.assets().load<scene3d::Model>(model_path);
    if (!loaded) {
        return param_error(code::kInvalidParams, params.find("model") != nullptr ? "/model" : "/id",
                           quoted(model_path) + " can't be loaded: " + loaded.error().message);
    }
    auto made =
        scene3d::generate_ragdoll(*loaded.value(), options_of<scene3d::RagdollOptions>(params));
    if (!made) {
        return param_error(code::kInvalidParams, params.find("model") != nullptr ? "/model" : "/id",
                           made.error().message);
    }
    json::Value file = std::move(made).value();
    file["model"] = model_path;
    const std::string_view file_path = asset::file_of(model_path);
    const std::size_t dot = file_path.rfind('.');
    json::Value result;
    std::string out;
    if (Reply written =
            write_profile(session, params, file,
                          std::string(file_path.substr(0, dot)) + ".ragdoll.json", result, out);
        !written.ok) {
        return written;
    }
    result["humanoid"] = file.find("balance") != nullptr;
    if (entity != entt::null) {
        auto& ragdoll = world.ecs().get_or_emplace<world::Ragdoll3D>(entity);
        if (ragdoll.profile != out) {
            ragdoll.profile = out;
        }
    }
    return Reply::success(std::move(result));
}

Reply ragdoll_hit(Session& session, const json::Value& params) {
    auto found = entity_of(session, params);
    if (!found) {
        return param_error(code::kNotFound, "/id", found.error().message);
    }
    const json::Array& items = params.find("impulse")->as_array();
    const auto at = [&](std::size_t i) {
        return static_cast<real_t>(i < items.size() ? items[i].as_double() : 0.0);
    };
    const bool flat = session.world().ecs().all_of<world::Ragdoll2D>(found.value());
    if (!flat && items.size() != 3) {
        return param_error(code::kInvalidParams, "/impulse",
                           "a Ragdoll3D takes an impulse of three numbers, [x, y, z]");
    }
    const std::string& bone = params.find("bone")->as_string();
    Result<physics2d::RagdollHitReport> hit = make_error(ErrorCode::InvalidArgument, "not hit");
    if (flat) {
        hit = physics2d::hit_ragdoll(session.world(), found.value(), bone, {at(0), at(1)});
    } else if (auto hit3d = scene3d::hit_ragdoll(session.world(), found.value(), bone,
                                                 Vec3{at(0), at(1), at(2)});
               hit3d) {
        hit = physics2d::RagdollHitReport{.state = std::move(hit3d.value().state),
                                          .weakness = std::move(hit3d.value().weakness),
                                          .knocked_down = hit3d.value().knocked_down};
    } else {
        hit = hit3d.error();
    }
    if (!hit) {
        return param_error(
            hit.error().code == ErrorCode::NotFound ? code::kNotFound : code::kInvalidParams,
            hit.error().code == ErrorCode::NotFound ? "/bone" : "/id", hit.error().message);
    }
    json::Value result;
    result["state"] = hit.value().state;
    json::Object weakness;
    for (const auto& [name, weak] : hit.value().weakness) {
        weakness[name] = rounded(weak);
    }
    result["weakness"] = std::move(weakness);
    if (hit.value().knocked_down) {
        result["knocked_down"] = true;
    }
    return Reply::success(std::move(result));
}

Reply ragdoll_tune(Session& session, const json::Value& params) {
    auto found = entity_of(session, params);
    if (!found) {
        return param_error(code::kNotFound, "/id", found.error().message);
    }
    world::World& world = session.world();
    auto* ragdoll3d = world.ecs().try_get<world::Ragdoll3D>(found.value());
    auto* ragdoll2d = world.ecs().try_get<world::Ragdoll2D>(found.value());
    if (ragdoll3d == nullptr && ragdoll2d == nullptr) {
        return param_error(code::kInvalidParams, "/id",
                           quoted(params.find("id")->as_string()) +
                               " has no Ragdoll3D or Ragdoll2D");
    }
    struct Fields {
        const std::string* profile;
        json::Value* overrides;
    };
    const Fields ragdoll = ragdoll3d != nullptr
                               ? Fields{&ragdoll3d->profile, &ragdoll3d->overrides}
                               : Fields{&ragdoll2d->profile, &ragdoll2d->overrides};
    const json::Value* patch = params.find("overrides");
    const bool replace = params.find("replace") != nullptr && params.find("replace")->as_bool();
    json::Value overrides = replace ? *patch : world::merge_patch(*ragdoll.overrides, *patch);
    if (overrides.is_object() && overrides.as_object().empty()) {
        overrides = json::Value{};
    }
    auto file = session.assets().load<json::Value>(*ragdoll.profile);
    if (!file) {
        return param_error(code::kInvalidParams, "/id",
                           quoted(*ragdoll.profile) + " can't be read: " + file.error().message);
    }
    auto profile = world::parse_ragdoll(*file.value(), overrides);
    if (!profile) {
        return param_error(code::kInvalidParams, "/overrides", profile.error().message);
    }
    if (!(*ragdoll.overrides == overrides)) {
        *ragdoll.overrides = overrides;
    } else {
        session.mark_unchanged();
    }
    json::Value result;
    result["overrides"] = overrides;
    // The bones the patch touched, as they now are.
    json::Object bones;
    const json::Value* touched = patch->find("bones");
    const bool all = patch->find("defaults") != nullptr;
    for (const auto& [name, bone] : profile.value().bones) {
        if (all || (touched != nullptr && touched->find(name) != nullptr)) {
            bones[name] = bone_value(bone);
        }
    }
    if (!bones.empty()) {
        result["bones"] = std::move(bones);
    }
    return Reply::success(std::move(result));
}

json::Value id_property(std::string_view description) {
    json::Value id = property("string", description);
    id["minLength"] = 1;
    return id;
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

json::Value positive(std::string_view description) {
    json::Value out = property("number", description);
    out["exclusiveMinimum"] = 0;
    return out;
}

} // namespace

void register_ragdoll_commands(Registry& registry) {
    using enum Capability;
    json::Value model = property("string", "A skinned model file (3D). Or give id.");
    model["minLength"] = 1;
    json::Value out = property("string", "The .ragdoll.json file to write. Default: the model's "
                                         "path as .ragdoll.json; 2D: ragdolls/<id>.ragdoll.json.");
    out["minLength"] = 1;
    registry.add({
        .name = "ragdoll.generate",
        .description = "Writes a ragdoll profile fitted to a model's skeleton, as Unreal's physics "
                       "asset tool: capsules and spheres over its bones, masses by volume, joint "
                       "limits for each kind of joint and muscles. Given id, also sets the "
                       "entity's Ragdoll3D profile to it; an id with no model but a Transform2D "
                       "tree gets a 2D profile of its tree and a Ragdoll2D.",
        .capabilities = {Read, Edit, Filesystem},
        .effect = Effect::Edit,
        .params = schema_of(
            {
                {"id", id_property("An entity whose MeshInstance3D draws the model, or the root "
                                   "of a 2D character's Transform2D tree.")},
                {"model", std::move(model)},
                {"out", std::move(out)},
                {"mass", positive("Kilograms the bodies weigh together. Default 70.")},
                {"stiffness", positive("The muscles' stiffness. Default 400.")},
                {"damping", positive("The muscles' damping. Default 40.")},
                {"pin", positive("How hard muscles pin bodies to the pose's place. Default 200.")},
                {"pin_damping", positive("The pins' damping. Default 28.")},
                {"min_length", positive("Bones shorter than this get no body (not humanoids): a "
                                        "share of the skeleton's height, 2D pixels. Default "
                                        "4% of its height.")},
            },
            {}),
        .handler = ragdoll_generate,
    });
    json::Value impulse = property("array", "Kilogram-meters per second, [x, y, z], in the world; "
                                            "2D: kilogram-pixels per second, [x, y].");
    impulse["items"]["type"] = "number";
    impulse["minItems"] = 2;
    impulse["maxItems"] = 3;
    json::Value bone = property("string", "The bone hit, as the profile names it.");
    bone["minLength"] = 1;
    registry.add({
        .name = "ragdoll.hit",
        .description =
            "Hits a Ragdoll3D's or Ragdoll2D's bone with an impulse: its body takes it, its muscle "
            "and those under it weaken, an animated ragdoll reacts and a hit of the "
            "profile's knockdown makes it limp. Returns the state and weakness.",
        .capabilities = {Read, Edit},
        .effect = Effect::Edit,
        .params = schema_of({{"id", id_property("An entity with a Ragdoll3D or Ragdoll2D.")},
                             {"bone", std::move(bone)},
                             {"impulse", std::move(impulse)}},
                            {"id", "bone", "impulse"}),
        .handler = ragdoll_hit,
    });
    registry.add({
        .name = "ragdoll.tune",
        .description =
            "Tunes one Ragdoll3D or Ragdoll2D: merges a patch of profile settings into its "
            "overrides (null removes one), checks them against the profile, and "
            "returns the overrides and each bone the patch touched as it now is.",
        .capabilities = {Read, Edit},
        .effect = Effect::Edit,
        .params = schema_of(
            {{"id", id_property("An entity with a Ragdoll3D or Ragdoll2D.")},
             {"overrides", property("object", "Profile settings for this one, as in a "
                                              ".ragdoll.json file: {\"bones\": {\"Neck\": "
                                              "{\"stiffness\": 300}}, \"hit\": {...}}.")},
             {"replace", property("boolean", "Replace the overrides instead of merging into "
                                             "them. Default false.")}},
            {"id", "overrides"}),
        .handler = ragdoll_tune,
    });
}

} // namespace slash::command
