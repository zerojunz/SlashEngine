#include "animation_layers.hpp"

#include "slash/animation/animation.hpp"
#include "slash/core/text.hpp"
#include "slash/scene3d/model.hpp"
#include "slash/scene3d/skeleton.hpp"
#include "slash/world/ragdoll.hpp"
#include "slash/world/tree3d.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Tuning any model's animations as an agent would by hand in an animation editor: a .anim.json
// animation that comes from a model's animation takes a speed and layers over it
// (animation::AnimationLayer), which animation.edit writes and animation.preview and
// animation.tune measure, the gait from a humanoid's feet by forward kinematics. Built with
// -ffp-contract=off, so measures are the same everywhere.
namespace slash::command::detail {
namespace {

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

Reply param_error(int error_code, std::string_view path, const std::string& message) {
    json::Value data;
    data["path"] = path;
    return Reply::failure(error_code, std::string(path) + ": " + message, std::move(data));
}

std::string quoted(std::string_view name) {
    return "\"" + std::string(name) + "\"";
}

double rounded(double value, double places = 1000.0) {
    // Adding zero turns a rounded -0 into 0.
    return (std::round(value * places) / places) + 0.0;
}

// An error with the param it is about.
struct Problem {
    std::string path;
    std::string message;
    int code = code::kInvalidParams;
};

// A library file as it is: its text, or none yet.
struct LibraryFile {
    std::string path;
    std::optional<std::string> text;
    json::Value doc;
};

Result<LibraryFile> read_library(Session& session, const std::string& path, Problem& problem) {
    problem.path = "/library";
    if (!path.ends_with(".anim.json")) {
        return make_error(ErrorCode::InvalidArgument, quoted(path) + " must end in .anim.json");
    }
    LibraryFile out;
    out.path = path;
    auto text = session.read_project_file(path);
    if (!text) {
        return text.error();
    }
    out.text = std::move(text).value();
    if (!out.text) {
        out.doc["format"] = 1;
        out.doc["animations"] = json::Object{};
        return out;
    }
    auto doc = json::parse(*out.text);
    if (!doc) {
        return doc.error();
    }
    out.doc = std::move(doc).value();
    return out;
}

const json::Value* animation_entry(const json::Value& doc, std::string_view name) {
    const json::Value* animations = doc.find("animations");
    return animations != nullptr && animations->is_object() ? animations->find(name) : nullptr;
}

// The library with an edit's set, layer and remove_layer applied to one animation.
Result<json::Value> apply_edit(json::Value doc, const std::string& name, const json::Value& params,
                               Problem& problem) {
    const json::Value* found = animation_entry(doc, name);
    json::Value entry = found != nullptr ? *found : json::Value();
    if (const json::Value* set = params.find("set"); set != nullptr) {
        problem.path = "/set";
        if (set->is_null()) {
            doc["animations"].as_object().erase(name);
            return doc;
        }
        entry = world::merge_patch(entry.is_null() ? json::Value(json::Object{}) : entry, *set);
    }
    const bool layered = params.find("layer") != nullptr || params.find("remove_layer") != nullptr;
    if (layered && entry.is_null()) {
        problem.path = "/animation";
        return make_error(ErrorCode::NotFound,
                          quoted(name) + " is not in the library; make it with set first");
    }
    if (const json::Value* layer = params.find("layer"); layer != nullptr) {
        problem.path = "/layer";
        const std::string& layer_name = layer->find("name")->as_string();
        json::Value& layers = entry["layers"];
        if (!layers.is_array()) {
            layers = json::Array{};
        }
        json::Array& list = layers.as_array();
        const auto same = std::ranges::find_if(list, [&](const json::Value& each) {
            const json::Value* named = each.find("name");
            return named != nullptr && named->is_string() && named->as_string() == layer_name;
        });
        if (same != list.end()) {
            *same = world::merge_patch(*same, *layer);
        } else {
            list.push_back(world::merge_patch(json::Object{}, *layer));
        }
    }
    if (const json::Value* remove = params.find("remove_layer"); remove != nullptr) {
        problem.path = "/remove_layer";
        json::Value* layers = entry.find("layers");
        std::vector<std::string_view> names;
        bool removed = false;
        if (layers != nullptr && layers->is_array()) {
            json::Array& list = layers->as_array();
            for (auto it = list.begin(); it != list.end();) {
                const json::Value* named = it->find("name");
                if (named != nullptr && named->is_string() &&
                    named->as_string() == remove->as_string()) {
                    it = list.erase(it);
                    removed = true;
                } else {
                    if (named != nullptr && named->is_string()) {
                        names.emplace_back(named->as_string());
                    }
                    ++it;
                }
            }
            if (list.empty()) {
                entry.as_object().erase("layers");
            }
        }
        if (!removed) {
            std::string message = quoted(remove->as_string()) + " is not one of its layers";
            if (const std::string_view hint = closest_match(remove->as_string(), names);
                !hint.empty()) {
                message += "; did you mean " + quoted(hint) + "?";
            }
            return make_error(ErrorCode::NotFound, message);
        }
    }
    if (params.find("set") != nullptr || layered) {
        doc["animations"][name] = std::move(entry);
    }
    return doc;
}

// A library's animation ready to play: parsed, its model loaded and its layers baked.
struct Played {
    animation::Animation animation;
    asset::Handle<scene3d::Model> model;
};

Result<Played> resolve(Session& session, const json::Value& doc, const std::string& name,
                       Problem& problem) {
    const std::vector<json::ValidationError> errors =
        animation::validate_animation_library(doc, &session.components());
    if (!errors.empty()) {
        return make_error(
            ErrorCode::InvalidArgument,
            (errors.front().path.empty() ? std::string("root") : errors.front().path) + ": " +
                errors.front().message);
    }
    auto library = animation::parse_animation_library(doc);
    if (!library) {
        return library.error();
    }
    auto& animations = library.value().animations;
    const auto found = animations.find(name);
    if (found == animations.end()) {
        problem.path = "/animation";
        std::vector<std::string_view> names;
        names.reserve(animations.size());
        for (const auto& [each, unused] : animations) {
            names.emplace_back(each);
        }
        std::string message = quoted(name) + " is not in the library";
        if (const std::string_view hint = closest_match(name, names); !hint.empty()) {
            message += "; did you mean " + quoted(hint) + "?";
        }
        problem.code = code::kNotFound;
        return make_error(ErrorCode::NotFound, message);
    }
    Played out;
    out.animation = found->second;
    if (out.animation.from.model.empty()) {
        problem.path = "/animation";
        return make_error(ErrorCode::InvalidArgument,
                          quoted(name) + R"x( has no model animation to tune: give it "from" (a )x"
                                         R"x(model file and its animation) with set)x");
    }
    auto model = session.assets().load<scene3d::Model>(out.animation.from.model);
    if (!model) {
        return make_error(model.error().code, "/animations/" + json::pointer_escape(name) +
                                                  "/from/model: " + model.error().message);
    }
    out.model = std::move(model).value();
    if (auto resolved = animation::resolve_clip(out.animation, *out.model); !resolved) {
        const std::string& message = resolved.error().message;
        return make_error(
            resolved.error().code,
            "/animations/" + json::pointer_escape(name) + "/" +
                (message.starts_with("layers/") ? message : "from/animation: " + message));
    }
    return out;
}

// How a library's animation moves its model, sampled through its length.
struct Measure {
    double length = 0.0;
    bool loop = false;
    // Each moved bone's turn from rest about pitch, yaw and roll: least and most, in radians.
    std::map<std::string, std::array<std::array<float, 2>, 3>, std::less<>> ranges;
    // A humanoid's gait (LeftFoot and RightFoot): how far its feet sweep fore and aft (+z), how
    // fast they sweep back, and how fast its body moves.
    bool feet = false;
    double stride = 0.0;
    double foot_speed = 0.0;
    double move_speed = 0.0;
};

constexpr std::array<std::string_view, 3> kAxisNames{"pitch", "yaw", "roll"};
constexpr std::array<std::string_view, 2> kFeet{"LeftFoot", "RightFoot"};

Measure measure(const Played& played, int samples, std::optional<double> move_speed) {
    const scene3d::Model& model = *played.model;
    const scene3d::Model& source = animation::clip_source(played.animation, model);
    Measure out;
    out.length = played.animation.length;
    out.loop = played.animation.loop != animation::LoopMode::None;
    out.move_speed = move_speed.value_or(0.0);
    const auto clip = std::ranges::find(source.animations, played.animation.from.animation,
                                        &scene3d::ModelAnimation::name);
    if (clip == source.animations.end()) {
        return out;
    }
    const std::vector<int> map =
        &source == &model ? std::vector<int>{} : scene3d::node_map(model, source);
    std::vector<bool> moved(model.nodes.size(), false);
    for (const scene3d::NodeTrack& track : clip->tracks) {
        int node = static_cast<int>(track.node);
        if (!map.empty()) {
            node = track.node < map.size() ? map[track.node] : -1;
        }
        if (node >= 0 && static_cast<std::size_t>(node) < moved.size() &&
            track.property == scene3d::NodeProperty::Rotation) {
            moved[static_cast<std::size_t>(node)] = true;
        }
    }
    std::array<int, 2> feet{scene3d::find_node(model, kFeet[0]),
                            scene3d::find_node(model, kFeet[1])};
    out.feet = feet[0] >= 0 && feet[1] >= 0;
    std::vector<std::array<double, 2>> foot_z;
    for (int i = 0; i < samples; ++i) {
        const double time = out.length * static_cast<double>(i) / static_cast<double>(samples);
        std::vector<scene3d::NodePose> pose = scene3d::rest_pose(model);
        scene3d::blend_animation(model, source, *clip, time, 1.0F, map, pose);
        for (std::size_t n = 0; n < moved.size(); ++n) {
            if (!moved[n]) {
                continue;
            }
            const Quat turn =
                scene3d::multiply(scene3d::inverse(model.nodes[n].rotation), pose[n].rotation);
            const Vec3 e = world::euler(world::rotation_basis(scene3d::normalized(turn)));
            const std::array<float, 3> a{static_cast<float>(e.x), static_cast<float>(e.y),
                                         static_cast<float>(e.z)};
            auto [range, made] = out.ranges.try_emplace(model.nodes[n].name);
            for (std::size_t axis = 0; axis < 3; ++axis) {
                if (made || a.at(axis) < range->second.at(axis)[0]) {
                    range->second.at(axis)[0] = a.at(axis);
                }
                if (made || a.at(axis) > range->second.at(axis)[1]) {
                    range->second.at(axis)[1] = a.at(axis);
                }
            }
        }
        if (out.feet) {
            const std::vector<world::Affine3D> globals = scene3d::node_globals(model, pose);
            foot_z.push_back(
                {static_cast<double>(globals[static_cast<std::size_t>(feet[0])].origin.z),
                 static_cast<double>(globals[static_cast<std::size_t>(feet[1])].origin.z)});
        }
    }
    if (!out.feet || foot_z.empty() || out.length <= 0.0) {
        return out;
    }
    const double step = out.length / static_cast<double>(samples);
    for (std::size_t leg = 0; leg < 2; ++leg) {
        double low = foot_z[0].at(leg);
        double high = low;
        double back = 0.0;
        int backward = 0;
        for (std::size_t i = 0; i < foot_z.size(); ++i) {
            low = std::min(low, foot_z[i].at(leg));
            high = std::max(high, foot_z[i].at(leg));
            if (i + 1 == foot_z.size() && !out.loop) {
                break;
            }
            // It faces +z, so a planted foot moves toward -z under the body.
            const double dz = foot_z[(i + 1) % foot_z.size()].at(leg) - foot_z[i].at(leg);
            if (dz < 0.0) {
                back -= dz;
                ++backward;
            }
        }
        out.stride += (high - low) / 2.0;
        if (backward > 0) {
            out.foot_speed += back / (static_cast<double>(backward) * step) / 2.0;
        }
    }
    return out;
}

json::Value measure_json(const Measure& m, const std::vector<std::string>& only) {
    json::Value out;
    out["length"] = rounded(m.length);
    out["loop"] = m.loop;
    json::Value bones = json::Object{};
    for (const auto& [bone, axes] : m.ranges) {
        if (!only.empty() && std::ranges::find(only, bone) == only.end()) {
            continue;
        }
        json::Value each = json::Object{};
        for (std::size_t axis = 0; axis < 3; ++axis) {
            if (axes.at(axis)[1] - axes.at(axis)[0] > 1e-4F || std::abs(axes.at(axis)[0]) > 1e-4F) {
                each[kAxisNames.at(axis)] =
                    json::Array{rounded(axes.at(axis)[0]), rounded(axes.at(axis)[1])};
            }
        }
        if (!each.as_object().empty()) {
            bones[bone] = std::move(each);
        }
    }
    out["bones"] = std::move(bones);
    if (m.feet) {
        json::Value gait;
        gait["stride"] = rounded(m.stride);
        gait["foot_speed"] = rounded(m.foot_speed);
        gait["move_speed"] = rounded(m.move_speed);
        if (m.move_speed > 0.0) {
            gait["slide"] = rounded(m.move_speed - m.foot_speed);
        }
        out["gait"] = std::move(gait);
    }
    return out;
}

float swing_of(const Measure& m, std::string_view bone) {
    float most = 0.0F;
    if (const auto found = m.ranges.find(bone); found != m.ranges.end()) {
        for (const auto& axis : found->second) {
            most = std::max(most, axis[1] - axis[0]);
        }
    }
    return most;
}

std::vector<std::string> names_in(const json::Value* list) {
    std::vector<std::string> out;
    if (list != nullptr) {
        for (const json::Value& each : list->as_array()) {
            out.push_back(each.as_string());
        }
    }
    return out;
}

std::optional<double> number_in(const json::Value& params, std::string_view key) {
    if (const json::Value* given = params.find(key); given != nullptr) {
        return given->as_double();
    }
    return std::nullopt;
}

int samples_of(const json::Value& params) {
    const json::Value* given = params.find("samples");
    return given != nullptr ? static_cast<int>(given->as_int()) : 16;
}

Reply problem_reply(const Problem& problem, const Error& error) {
    return param_error(problem.code, problem.path, error.message);
}

// Writes a library's new text, with undo; nothing when it is the same.
Result<void> write_library(Session& session, const LibraryFile& file, const json::Value& doc) {
    std::string written = json::write(doc);
    if (file.text && written == *file.text) {
        session.mark_unchanged();
        return {};
    }
    if (auto staged = session.stage_file(file.path, written); !staged) {
        return staged.error();
    }
    session.add_undo_file(file.path, file.text, std::move(written));
    return {};
}

// The leg bones of a humanoid, which slide "swing" scales.
bool is_leg(std::string_view bone) {
    return bone.ends_with("UpperLeg") || bone.ends_with("LowerLeg") || bone.ends_with("Foot") ||
           bone.ends_with("Toes");
}

Reply edit(Session& session, const json::Value& params) {
    Problem problem;
    auto file = read_library(session, params.find("library")->as_string(), problem);
    if (!file) {
        return problem_reply(problem, file.error());
    }
    const std::string name = params.find("animation")->as_string();
    auto doc = apply_edit(file.value().doc, name, params, problem);
    if (!doc) {
        return problem_reply(problem, doc.error());
    }
    json::Value result;
    result["library"] = file.value().path;
    result["animation"] = name;
    const json::Value* entry = animation_entry(doc.value(), name);
    if (entry != nullptr) {
        auto played = resolve(session, doc.value(), name, problem);
        // A value track's or marker's animation needs no model; one from a model must play.
        if (!played && entry->find("from") != nullptr) {
            return problem_reply(problem, played.error());
        }
        if (!played) {
            const std::vector<json::ValidationError> errors =
                animation::validate_animation_library(doc.value(), &session.components());
            if (!errors.empty()) {
                return param_error(code::kInvalidParams, problem.path,
                                   errors.front().path + ": " + errors.front().message);
            }
        } else {
            result["length"] = rounded(played.value().animation.length);
        }
    }
    if (auto written = write_library(session, file.value(), doc.value()); !written) {
        return Reply::failure(written.error());
    }
    result["after"] = entry != nullptr ? *entry : json::Value();
    return Reply::success(std::move(result));
}

} // namespace

Reply library_preview(Session& session, const json::Value& params) {
    for (const std::string_view other : {"creature", "recipe", "motion", "filmstrip"}) {
        if (params.find(other) != nullptr) {
            return param_error(code::kInvalidParams, "/" + std::string(other),
                               "a library's animation is previewed by library alone, without " +
                                   std::string(other));
        }
    }
    Problem problem;
    auto file = read_library(session, params.find("library")->as_string(), problem);
    if (!file) {
        return problem_reply(problem, file.error());
    }
    if (!file.value().text) {
        return param_error(code::kNotFound, "/library",
                           quoted(file.value().path) + " doesn't exist");
    }
    const std::string name = params.find("animation")->as_string();
    auto doc = apply_edit(file.value().doc, name, params, problem);
    if (!doc) {
        return problem_reply(problem, doc.error());
    }
    auto played = resolve(session, doc.value(), name, problem);
    if (!played) {
        return problem_reply(problem, played.error());
    }
    json::Value result =
        measure_json(measure(played.value(), samples_of(params), number_in(params, "move_speed")),
                     names_in(params.find("bones")));
    result["library"] = file.value().path;
    return Reply::success(std::move(result));
}

Reply library_tune(Session& session, const json::Value& params) {
    for (const std::string_view other : {"creature", "recipe", "into"}) {
        if (params.find(other) != nullptr) {
            return param_error(code::kInvalidParams, "/" + std::string(other),
                               "a library's animation is tuned in its library, without " +
                                   std::string(other));
        }
    }
    Problem problem;
    auto file = read_library(session, params.find("library")->as_string(), problem);
    if (!file) {
        return problem_reply(problem, file.error());
    }
    const std::string name = params.find("animation")->as_string();
    const int samples = samples_of(params);
    const std::optional<double> move_speed = number_in(params, "move_speed");
    const std::vector<std::string> shown = names_in(params.find("bones"));
    json::Value result;
    if (auto before = resolve(session, file.value().doc, name, problem)) {
        result["before"] = measure_json(measure(before.value(), samples, move_speed), shown);
    } else {
        result["before"] = json::Value();
    }
    auto edited = apply_edit(file.value().doc, name, params, problem);
    if (!edited) {
        return problem_reply(problem, edited.error());
    }
    json::Value doc = std::move(edited).value();
    auto played = resolve(session, doc, name, problem);
    if (!played) {
        return problem_reply(problem, played.error());
    }
    // Goals go into a layer of their own: the one layer names, else "tune".
    std::string layer_name = "tune";
    if (const json::Value* layer = params.find("layer"); layer != nullptr) {
        layer_name = layer->find("name")->as_string();
    }
    json::Value& entry = doc["animations"][name];
    const auto tune_layer = [&]() -> json::Value& {
        json::Value& layers = entry["layers"];
        if (!layers.is_array()) {
            layers = json::Array{};
        }
        for (json::Value& each : layers.as_array()) {
            if (const json::Value* named = each.find("name");
                named != nullptr && named->as_string() == layer_name) {
                return each;
            }
        }
        json::Value made;
        made["name"] = layer_name;
        layers.as_array().push_back(std::move(made));
        return layers.as_array().back();
    };
    const auto scale_of = [&](std::string_view bone) {
        const json::Value& layer = tune_layer();
        const json::Value* bones = layer.find("bones");
        const json::Value* found = bones != nullptr ? bones->find(bone) : nullptr;
        const json::Value* scale = found != nullptr ? found->find("scale") : nullptr;
        return scale != nullptr ? scale->as_double() : 1.0;
    };
    const auto remeasure = [&]() -> Result<Measure> {
        auto now = resolve(session, doc, name, problem);
        if (!now) {
            return now.error();
        }
        return measure(now.value(), samples, move_speed);
    };
    if (const json::Value* ranges = params.find("ranges"); ranges != nullptr) {
        problem.path = "/ranges";
        const Measure start = measure(played.value(), samples, move_speed);
        for (const auto& [bone, want] : ranges->as_object()) {
            if (scene3d::find_node(*played.value().model, bone) < 0) {
                std::vector<std::string_view> names;
                names.reserve(start.ranges.size());
                for (const auto& [each, unused] : start.ranges) {
                    names.emplace_back(each);
                }
                std::string message = quoted(bone) + " is not a bone it moves";
                if (const std::string_view hint = closest_match(bone, names); !hint.empty()) {
                    message += "; did you mean " + quoted(hint) + "?";
                }
                return param_error(code::kInvalidParams, "/ranges/" + json::pointer_escape(bone),
                                   message);
            }
        }
        // A few rounds, as turns about several axes don't scale evenly.
        for (int round = 0; round < 4; ++round) {
            auto now = remeasure();
            if (!now) {
                return problem_reply(problem, now.error());
            }
            for (const auto& [bone, want] : ranges->as_object()) {
                const float swing = swing_of(now.value(), bone);
                if (swing <= 1e-5F) {
                    continue; // it doesn't turn; add a wave with layer instead
                }
                const double scale =
                    std::clamp(scale_of(bone) * want.as_double() / swing, 0.0, 4.0);
                tune_layer()["bones"][bone]["scale"] = rounded(scale, 10000.0);
            }
        }
    }
    if (const json::Value* slide = params.find("slide"); slide != nullptr) {
        problem.path = "/slide";
        const bool by_swing = slide->as_string() == "swing";
        for (int round = 0; round < (by_swing ? 8 : 3); ++round) {
            auto now = remeasure();
            if (!now) {
                return problem_reply(problem, now.error());
            }
            const Measure& m = now.value();
            if (!m.feet || m.move_speed <= 0.0 || m.foot_speed <= 1e-6) {
                return param_error(code::kInvalidParams, "/slide",
                                   !m.feet ? "its model has no LeftFoot and RightFoot to plant"
                                           : "its feet don't sweep, or it stands still; give "
                                             "move_speed");
            }
            const double ratio = m.move_speed / m.foot_speed;
            if (std::abs(ratio - 1.0) < 1e-4) {
                break;
            }
            if (by_swing) {
                for (const auto& [bone, unused] : m.ranges) {
                    if (is_leg(bone)) {
                        tune_layer()["bones"][bone]["scale"] =
                            rounded(std::clamp(scale_of(bone) * ratio, 0.0, 4.0), 10000.0);
                    }
                }
            } else {
                const json::Value* speed = entry.find("speed");
                const double now_speed = speed != nullptr ? speed->as_double() : 1.0;
                entry["speed"] = rounded(std::clamp(now_speed * ratio, 0.01, 10.0), 10000.0);
            }
        }
    }
    // A goal layer left with nothing in it is left out.
    if (json::Value* layers = entry.find("layers"); layers != nullptr) {
        std::erase_if(layers->as_array(),
                      [](const json::Value& each) { return each.as_object().size() == 1; });
        if (layers->as_array().empty()) {
            entry.as_object().erase("layers");
        }
    }
    auto after = remeasure();
    if (!after) {
        return problem_reply(problem, after.error());
    }
    if (auto written = write_library(session, file.value(), doc); !written) {
        return Reply::failure(written.error());
    }
    result["library"] = file.value().path;
    result["animation"] = name;
    result["change"] = entry;
    result["after"] = measure_json(after.value(), shown);
    return Reply::success(std::move(result));
}

void register_animation_edit(Registry& registry) {
    using enum Capability;
    json::Value set = property("object", "A JSON Merge Patch over the animation's entry "
                                         "(from, length, loop, speed, tracks, markers, "
                                         "layers); null removes the animation.");
    set["type"] = json::Array{"object", "null"};
    json::Value layer = property("object", "A layer merged by name into its layers (name, "
                                           "weight, amplitude, bones, add); null fields remove "
                                           "them.");
    layer["required"] = json::Array{"name"};
    layer["properties"]["name"] = text_property("The layer's name.");
    json::Value properties;
    properties["library"] = text_property("A .anim.json file; made if missing.");
    properties["animation"] = text_property("The animation's name.");
    properties["set"] = std::move(set);
    properties["layer"] = std::move(layer);
    properties["remove_layer"] = text_property("A layer's name to take out.");
    json::Value params;
    params["type"] = "object";
    params["additionalProperties"] = false;
    params["properties"] = std::move(properties);
    params["required"] = json::Array{"library", "animation"};
    registry.add({
        .name = "animation.edit",
        .description = "Changes an animation in a .anim.json library, with undo: set patches "
                       "its entry, layer adds or patches a layer of bone changes over the model "
                       "animation it comes from, remove_layer takes one out. Checks it plays.",
        .capabilities = {Read, Edit, Filesystem},
        .effect = Effect::Edit,
        .params = std::move(params),
        .handler = edit,
    });
}

} // namespace slash::command::detail
