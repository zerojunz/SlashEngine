#include "slash/command/lighting.hpp"

#include "slash/canvas/canvas.hpp"
#include "slash/command/session.hpp"
#include "slash/core/result.hpp"
#include "slash/core/text.hpp"
#include "slash/scene3d/scene3d.hpp"
#include "slash/world/components.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace slash::command {
namespace {

// A pixel is clipped when a channel is this bright, and crushed when its brightness is below
// kCrushed: both lose the detail lighting is there to show.
constexpr std::uint8_t kClipped = 254;
constexpr double kCrushed = 0.02;
// A light lights a pixel when taking it away changes the pixel's brightness by this much, two
// steps of an 8-bit channel.
constexpr std::int32_t kLitStep = 2 * 10000;
// What counts as a problem: a frame more than 5% clipped, more than 40% crushed, whose mean
// brightness is below 0.08 or above 0.8, or whose 5th to 95th percentile spans less than 0.1; a
// light that lights under 0.1% of the frame, or without which 2% of it would not be clipped.
constexpr double kMostClipped = 0.05;
constexpr double kMostCrushed = 0.4;
constexpr double kDarkest = 0.08;
constexpr double kBrightest = 0.8;
constexpr double kFlattest = 0.1;
constexpr double kLeastLit = 0.001;
constexpr double kMostClippedByOne = 0.02;
constexpr double kDefaultScale = 0.25;
constexpr std::size_t kMaxLights = 32;

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

// A share with four decimals, so replies stay short and the same everywhere.
double rounded(double value) {
    return std::round(value * 10000.0) / 10000.0;
}

// Each pixel's brightness from 0 to 1, Rec. 709's weights over the drawn (display) values, and
// whether a channel is clipped. `level` is the brightness in whole numbers (the weights times
// 10000 over the channels), so a light's change compares exactly on every machine.
struct Pixels {
    std::vector<std::int32_t> level;
    std::vector<double> brightness;
    std::vector<bool> clipped;
};

Pixels pixels_of(const render::Image& image) {
    Pixels out;
    const std::size_t count = static_cast<std::size_t>(image.width) * image.height;
    out.level.resize(count);
    out.brightness.resize(count);
    out.clipped.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint8_t r = image.pixels[i * 4];
        const std::uint8_t g = image.pixels[(i * 4) + 1];
        const std::uint8_t b = image.pixels[(i * 4) + 2];
        out.level[i] = (2126 * r) + (7152 * g) + (722 * b);
        out.brightness[i] = static_cast<double>(out.level[i]) / (10000.0 * 255.0);
        out.clipped[i] = r >= kClipped || g >= kClipped || b >= kClipped;
    }
    return out;
}

double percentile(std::vector<double> values, double at) {
    if (values.empty()) {
        return 0.0;
    }
    const auto index =
        static_cast<std::size_t>(std::floor(at * static_cast<double>(values.size() - 1) + 0.5));
    std::ranges::nth_element(values, values.begin() + static_cast<std::ptrdiff_t>(index));
    return values[index];
}

// A light the check takes away, by its component.
struct Light {
    entt::entity entity = entt::null;
    std::string id;
    std::string type;
    std::string bake_mode;
};

template <typename T>
void add_lights(world::World& world, std::string_view type, std::vector<Light>& out) {
    for (const entt::entity entity : world.ecs().view<T>()) {
        const T& light = world.ecs().get<T>(entity);
        if constexpr (std::is_same_v<T, world::PointLight2D> ||
                      std::is_same_v<T, world::DirectionalLight2D>) {
            if (!light.enabled || light.energy == 0.0F) {
                continue;
            }
            out.push_back({.entity = entity,
                           .id = std::string(world.id_of(entity)),
                           .type = std::string(type),
                           .bake_mode = ""});
        } else {
            if constexpr (std::is_same_v<T, world::DirectionalLight3D>) {
                if (light.sky_mode == "sky_only") {
                    continue; // lights only the sky
                }
            }
            if (light.light_energy == 0.0F) {
                continue;
            }
            out.push_back({.entity = entity,
                           .id = std::string(world.id_of(entity)),
                           .type = std::string(type),
                           .bake_mode = light.light_bake_mode});
        }
    }
}

// Every 3D and 2D light that shines and has an ID, in ID order.
std::vector<Light> shining_lights(world::World& world) {
    std::vector<Light> lights;
    add_lights<world::DirectionalLight3D>(world, "DirectionalLight3D", lights);
    add_lights<world::OmniLight3D>(world, "OmniLight3D", lights);
    add_lights<world::SpotLight3D>(world, "SpotLight3D", lights);
    add_lights<world::PointLight2D>(world, "PointLight2D", lights);
    add_lights<world::DirectionalLight2D>(world, "DirectionalLight2D", lights);
    std::erase_if(lights, [](const Light& light) { return light.id.empty(); });
    std::ranges::sort(lights, {}, &Light::id);
    return lights;
}

float& energy_of(world::World& world, const Light& light) {
    if (light.type == "DirectionalLight3D") {
        return world.ecs().get<world::DirectionalLight3D>(light.entity).light_energy;
    }
    if (light.type == "OmniLight3D") {
        return world.ecs().get<world::OmniLight3D>(light.entity).light_energy;
    }
    if (light.type == "PointLight2D") {
        return world.ecs().get<world::PointLight2D>(light.entity).energy;
    }
    if (light.type == "DirectionalLight2D") {
        return world.ecs().get<world::DirectionalLight2D>(light.entity).energy;
    }
    return world.ecs().get<world::SpotLight3D>(light.entity).light_energy;
}

// What light.tune scales: a 3D light's lux or lumens with physical light units, else its energy.
float& intensity_of(world::World& world, const Light& light, bool units) {
    if (units && light.type == "DirectionalLight3D") {
        return world.ecs().get<world::DirectionalLight3D>(light.entity).light_intensity_lux;
    }
    if (units && light.type == "OmniLight3D") {
        return world.ecs().get<world::OmniLight3D>(light.entity).light_intensity_lumens;
    }
    if (units && light.type == "SpotLight3D") {
        return world.ecs().get<world::SpotLight3D>(light.entity).light_intensity_lumens;
    }
    return energy_of(world, light);
}

std::string_view intensity_field(const Light& light, bool units) {
    if (light.type == "PointLight2D" || light.type == "DirectionalLight2D") {
        return "energy";
    }
    if (!units) {
        return "light_energy";
    }
    return light.type == "DirectionalLight3D" ? "light_intensity_lux" : "light_intensity_lumens";
}

// The lights named by ids (a JSON array at path), or a reply naming the first unknown one.
Result<std::vector<Light>> named_lights(const std::vector<Light>& lights, const json::Value& ids,
                                        std::string_view path) {
    std::vector<Light> named;
    const json::Array& list = ids.as_array();
    for (std::size_t i = 0; i < list.size(); ++i) {
        const std::string& id = list[i].as_string();
        const auto found = std::ranges::find(lights, id, &Light::id);
        if (found == lights.end()) {
            std::vector<std::string> known;
            known.reserve(lights.size());
            for (const Light& light : lights) {
                known.push_back(light.id);
            }
            std::string message = "no light that shines has the id \"" + id + "\"";
            if (const std::string_view hint = closest_match(id, known); !hint.empty()) {
                message += "; did you mean \"" + std::string(hint) + "\"?";
            }
            return make_error(ErrorCode::NotFound,
                              std::string(path) + "/" + std::to_string(i) + ": " + message);
        }
        if (std::ranges::find(named, id, &Light::id) == named.end()) {
            named.push_back(*found);
        }
    }
    std::ranges::sort(named, {}, &Light::id);
    return named;
}

// A failed lookup's "path: message" as a reply at that path.
Reply path_error(const Error& error) {
    const std::size_t colon = error.message.find(": ");
    return param_error(code::kNotFound, error.message.substr(0, colon),
                       error.message.substr(colon + 2));
}

// The frame's exposure: its mean brightness and the share of it clipped.
struct Exposure {
    double mean = 0.0;
    double clipped = 0.0;
};

Exposure exposure_of(const Pixels& pixels) {
    Exposure out;
    if (pixels.brightness.empty()) {
        return out;
    }
    std::size_t clipped = 0;
    for (std::size_t i = 0; i < pixels.brightness.size(); ++i) {
        out.mean += pixels.brightness[i];
        clipped += pixels.clipped[i] ? 1U : 0U;
    }
    const auto count = static_cast<double>(pixels.brightness.size());
    out.mean /= count;
    out.clipped = static_cast<double>(clipped) / count;
    return out;
}

// The frame drawn and read as pixels, or why not; it must draw 3D meshes or have 2D lights.
Result<Pixels> draw_frame(Session& session, const canvas::CaptureOptions& options, bool lights_2d) {
    auto drawn = canvas::capture(session.world(), session.assets(), options);
    if (!drawn) {
        return drawn.error();
    }
    if (drawn.value().meshes == 0 && !lights_2d) {
        return make_error(ErrorCode::InvalidArgument,
                          "the frame draws no 3D meshes and has no 2D lights; lighting looks at "
                          "a 3D world through its current Camera3D or a 2D one's lights");
    }
    return pixels_of(drawn.value().image);
}

bool has_2d(const std::vector<Light>& lights) {
    return std::ranges::any_of(lights, [](const Light& light) {
        return light.type == "PointLight2D" || light.type == "DirectionalLight2D";
    });
}

json::Value problem(std::string_view kind, std::string_view id, std::string message) {
    json::Value out;
    out["kind"] = kind;
    if (!id.empty()) {
        out["id"] = id;
    }
    out["message"] = std::move(message);
    return out;
}

std::string percent(double share) {
    const double value = std::round(share * 1000.0) / 10.0;
    std::string text = std::to_string(value);
    text.erase(text.find_last_not_of('0') + 1);
    if (text.back() == '.') {
        text.pop_back();
    }
    return text + "%";
}

Reply lighting_check(Session& session, const json::Value& params) {
    world::World& world = session.world();
    const json::Value* scale_param = params.find("scale");
    const double scale = scale_param != nullptr ? scale_param->as_double() : kDefaultScale;
    const bool per_light = params.find("lights") == nullptr || params.find("lights")->as_bool();

    const std::vector<Light> all = shining_lights(world);
    std::vector<Light> lights = all;
    if (const json::Value* ids = params.find("ids"); ids != nullptr) {
        auto named = named_lights(all, *ids, "/ids");
        if (!named) {
            return path_error(named.error());
        }
        lights = std::move(named).value();
    }
    if (lights.size() > kMaxLights) {
        lights.resize(kMaxLights);
    }

    const canvas::CaptureOptions options{.scale = static_cast<float>(scale)};
    auto whole = canvas::capture(world, session.assets(), options);
    if (!whole) {
        return Reply::failure(whole.error());
    }
    if (whole.value().meshes == 0 && !has_2d(all)) {
        return Reply::failure(code::kInvalidParams,
                              "the frame draws no 3D meshes and has no 2D lights; lighting.check "
                              "looks at a 3D world through its current Camera3D or a 2D one's "
                              "lights");
    }
    const render::Image& image = whole.value().image;
    const Pixels base = pixels_of(image);
    const auto count = static_cast<double>(base.brightness.size());

    json::Value result;
    result["size"] = json::Array{image.width, image.height};
    double sum = 0.0;
    std::size_t clipped = 0;
    std::size_t crushed = 0;
    for (std::size_t i = 0; i < base.brightness.size(); ++i) {
        sum += base.brightness[i];
        clipped += base.clipped[i] ? 1U : 0U;
        crushed += base.brightness[i] < kCrushed ? 1U : 0U;
    }
    const double mean = sum / count;
    const double low = percentile(base.brightness, 0.05);
    const double high = percentile(base.brightness, 0.95);
    json::Value exposure;
    exposure["mean"] = rounded(mean);
    exposure["p5"] = rounded(low);
    exposure["p95"] = rounded(high);
    exposure["clipped"] = rounded(static_cast<double>(clipped) / count);
    exposure["crushed"] = rounded(static_cast<double>(crushed) / count);
    result["exposure"] = std::move(exposure);

    json::Array problems;
    if (static_cast<double>(clipped) / count > kMostClipped) {
        problems.push_back(problem("overexposed", "",
                                   percent(static_cast<double>(clipped) / count) +
                                       " of the frame is clipped to full brightness"));
    }
    if (static_cast<double>(crushed) / count > kMostCrushed) {
        problems.push_back(problem("crushed", "",
                                   percent(static_cast<double>(crushed) / count) +
                                       " of the frame is nearly black"));
    }
    if (mean < kDarkest) {
        problems.push_back(problem("dark", "",
                                   "the frame's mean brightness is " +
                                       json::write(json::Value(rounded(mean))) + ", below " +
                                       json::write(json::Value(kDarkest))));
    } else if (mean > kBrightest) {
        problems.push_back(problem("bright", "",
                                   "the frame's mean brightness is " +
                                       json::write(json::Value(rounded(mean))) + ", above " +
                                       json::write(json::Value(kBrightest))));
    }
    if (high - low < kFlattest) {
        problems.push_back(problem("flat", "",
                                   "the frame's brightness spans only " +
                                       json::write(json::Value(rounded(high - low))) +
                                       " from its 5th to its 95th percentile"));
    }

    if (per_light) {
        json::Array measured;
        for (const Light& light : lights) {
            float& energy = energy_of(world, light);
            const float kept = energy;
            energy = 0.0F;
            auto without = canvas::capture(world, session.assets(), options);
            energy_of(world, light) = kept;
            if (!without) {
                return Reply::failure(without.error());
            }
            const Pixels dark = pixels_of(without.value().image);
            std::size_t lit = 0;
            std::size_t clips = 0;
            double added = 0.0;
            for (std::size_t i = 0; i < base.brightness.size(); ++i) {
                const double change = base.brightness[i] - dark.brightness[i];
                lit += base.level[i] - dark.level[i] >= kLitStep ? 1U : 0U;
                added += std::max(change, 0.0);
                clips += base.clipped[i] && !dark.clipped[i] ? 1U : 0U;
            }
            const double lit_share = static_cast<double>(lit) / count;
            const double clip_share = static_cast<double>(clips) / count;
            json::Value entry;
            entry["id"] = light.id;
            entry["type"] = light.type;
            entry["lit"] = rounded(lit_share);
            entry["share"] = rounded(sum > 0.0 ? added / sum : 0.0);
            if (clips > 0) {
                entry["clips"] = rounded(clip_share);
            }
            if (light.bake_mode == "static") {
                entry["baked"] = true;
            }
            measured.push_back(std::move(entry));
            if (lit_share < kLeastLit && light.bake_mode != "static") {
                problems.push_back(problem("unlit", light.id,
                                           "\"" + light.id +
                                               "\" lights nothing in view: out of range, "
                                               "shadowed, culled or facing away"));
            }
            if (clip_share > kMostClippedByOne) {
                problems.push_back(problem("clips", light.id,
                                           "\"" + light.id + "\" pushes " + percent(clip_share) +
                                               " of the frame to full brightness"));
            }
        }
        result["lights"] = std::move(measured);
    }
    result["problems"] = std::move(problems);
    return Reply::success(std::move(result));
}

// A light's share of the frame's brightness: what it adds to the frame over the frame's sum.
Result<double> share_of(Session& session, const canvas::CaptureOptions& options, const Pixels& base,
                        const Light& light) {
    world::World& world = session.world();
    float& energy = energy_of(world, light);
    const float kept = energy;
    energy = 0.0F;
    auto without = canvas::capture(world, session.assets(), options);
    energy_of(world, light) = kept;
    if (!without) {
        return without.error();
    }
    const Pixels dark = pixels_of(without.value().image);
    double sum = 0.0;
    double added = 0.0;
    for (std::size_t i = 0; i < base.brightness.size(); ++i) {
        sum += base.brightness[i];
        added += std::max(base.brightness[i] - dark.brightness[i], 0.0);
    }
    return sum > 0.0 ? added / sum : 0.0;
}

// How far one step of light.tune may scale a light's energy, and how close it must come.
constexpr double kLeastStep = 0.25;
constexpr double kMostStep = 4.0;
constexpr double kClipBack = 0.85;
constexpr double kTolerance = 0.02;
// Drawn brightness grows about as energy to a power: 1/2.2 (the display's gamma) for 3D lights,
// which shade in linear light, and 1 for 2D ones, which add to drawn colors. Each step measures
// the power anew from what the last step did, within these bounds.
constexpr double kGamma3D = 1.0 / 2.2;
constexpr double kLeastPower = 0.2;
constexpr double kMostPower = 1.5;
constexpr int kDefaultIterations = 6;
constexpr double kDefaultMean = 0.4;
constexpr double kDefaultClipped = 0.02;

double step_for(double want, double have, double power) {
    if (have <= 0.0) {
        return kMostStep;
    }
    return std::clamp(std::pow(want / have, 1.0 / power), kLeastStep, kMostStep);
}

// The power a step of `factor` showed, taking the mean from `was` to `now`, or `power` when the
// step was too small to tell.
double measured_power(double factor, double was, double now, double power) {
    if (std::abs(std::log(factor)) < 0.01 || was <= 0.0 || now <= 0.0) {
        return power;
    }
    const double seen = std::log(now / was) / std::log(factor);
    return seen > 0.0 ? std::clamp(seen, kLeastPower, kMostPower) : power;
}

double three_places(float value) {
    return std::round(static_cast<double>(value) * 1000.0) / 1000.0;
}

json::Value exposure_json(const Exposure& exposure) {
    json::Value out;
    out["mean"] = rounded(exposure.mean);
    out["clipped"] = rounded(exposure.clipped);
    return out;
}

Reply light_tune(Session& session, const json::Value& params) {
    world::World& world = session.world();
    const json::Value* scale_param = params.find("scale");
    const double scale = scale_param != nullptr ? scale_param->as_double() : kDefaultScale;
    const json::Value* mean_param = params.find("mean");
    const double want_mean = mean_param != nullptr ? mean_param->as_double() : kDefaultMean;
    const json::Value* clipped_param = params.find("clipped");
    const double want_clipped =
        clipped_param != nullptr ? clipped_param->as_double() : kDefaultClipped;
    const json::Value* iterations_param = params.find("iterations");
    const int iterations = iterations_param != nullptr
                               ? static_cast<int>(iterations_param->as_int())
                               : kDefaultIterations;

    const std::vector<Light> all = shining_lights(world);
    std::vector<Light> lights = all;
    if (const json::Value* ids = params.find("ids"); ids != nullptr) {
        auto named = named_lights(all, *ids, "/ids");
        if (!named) {
            return path_error(named.error());
        }
        lights = std::move(named).value();
    }
    std::vector<std::pair<Light, double>> shares;
    if (const json::Value* wanted = params.find("shares"); wanted != nullptr) {
        for (const auto& [id, share] : wanted->as_object()) {
            auto named = named_lights(lights, json::Value(json::Array{id}), "/shares");
            if (!named) {
                const Error& error = named.error();
                const std::size_t colon = error.message.find(": ");
                return param_error(code::kNotFound, "/shares/" + id,
                                   error.message.substr(colon + 2));
            }
            shares.emplace_back(named.value().front(), share.as_double());
        }
    }
    if (lights.empty()) {
        return Reply::failure(code::kInvalidParams, "no light with an ID shines, so none can be "
                                                    "tuned");
    }
    if (lights.size() > kMaxLights) {
        lights.resize(kMaxLights);
    }

    const bool units = scene3d::physical_light_units(world);
    std::vector<float> from;
    from.reserve(lights.size());
    for (const Light& light : lights) {
        from.push_back(intensity_of(world, light, units));
    }

    const canvas::CaptureOptions options{.scale = static_cast<float>(scale)};
    const bool lights_2d = has_2d(all);
    Exposure before;
    Exposure after;
    std::vector<double> measured(shares.size(), 0.0);
    bool converged = false;
    int steps = 0;
    // The power brightness grows by: measured from each step after the first.
    double power = std::ranges::all_of(lights,
                                       [](const Light& light) {
                                           return light.type == "PointLight2D" ||
                                                  light.type == "DirectionalLight2D";
                                       })
                       ? 1.0
                       : kGamma3D;
    // Every light's energy is its first times `total`, and a light with a share target's times
    // its own weight too. `low` and `high` bracket `total`: the largest seen too dark and the
    // smallest seen too bright or clipping (0 and infinity until seen). `best` is the total and
    // weights whose frame came closest without clipping past the limit.
    double total = 1.0;
    std::vector<double> weights(shares.size(), 1.0);
    double low = 0.0;
    double high = std::numeric_limits<double>::infinity();
    double last_total = 1.0;
    double last_mean = 0.0;
    struct Best {
        double total = 1.0;
        std::vector<double> weights;
        double miss = std::numeric_limits<double>::infinity();
        bool clips = true;
    } best;
    auto apply = [&](double scale_by, const std::vector<double>& weighted) {
        for (std::size_t i = 0; i < lights.size(); ++i) {
            double factor = scale_by;
            for (std::size_t j = 0; j < shares.size(); ++j) {
                if (shares[j].first.id == lights[i].id) {
                    factor *= weighted[j];
                }
            }
            intensity_of(world, lights[i], units) = static_cast<float>(
                std::round(static_cast<double>(from[i]) * factor * 1000.0) / 1000.0);
        }
    };
    auto measure = [&]() -> Result<Pixels> {
        auto drawn = draw_frame(session, options, lights_2d);
        if (!drawn) {
            return drawn;
        }
        after = exposure_of(drawn.value());
        for (std::size_t i = 0; i < shares.size(); ++i) {
            auto share = share_of(session, options, drawn.value(), shares[i].first);
            if (!share) {
                return share.error();
            }
            measured[i] = share.value();
        }
        return drawn;
    };
    for (;; ++steps) {
        auto drawn = measure();
        if (!drawn) {
            return Reply::failure(code::kInvalidParams, drawn.error().message);
        }
        if (steps == 0) {
            before = after;
        } else if (shares.empty()) {
            power = measured_power(total / last_total, last_mean, after.mean, power);
        }
        bool shares_met = true;
        for (std::size_t i = 0; i < shares.size(); ++i) {
            shares_met = shares_met && std::abs(measured[i] - shares[i].second) <= kTolerance;
        }
        const bool clips = after.clipped > want_clipped + 1e-9;
        converged = std::abs(after.mean - want_mean) <= kTolerance && !clips && shares_met;
        const double miss = std::abs(after.mean - want_mean);
        if ((best.clips && !clips) || (best.clips == clips && miss < best.miss)) {
            best = {.total = total, .weights = weights, .miss = miss, .clips = clips};
        }
        if (clips || after.mean > want_mean + kTolerance) {
            high = std::min(high, total);
        } else if (after.mean < want_mean - kTolerance) {
            low = std::max(low, total);
        }
        const bool bracketed = low > 0.0 && high < std::numeric_limits<double>::infinity();
        if (converged || steps == iterations || (bracketed && high / low < 1.01)) {
            break;
        }
        double next = total * step_for(want_mean, after.mean, power);
        if (clips) {
            next = std::min(next, total * kClipBack);
        }
        if (bracketed && (next <= low || next >= high)) {
            next = std::sqrt(low * high);
        }
        last_total = total;
        last_mean = after.mean;
        total = next;
        for (std::size_t i = 0; i < shares.size(); ++i) {
            const double step = step_for(shares[i].second, measured[i], power);
            weights[i] *= step;
            if (std::abs(step - 1.0) > kTolerance) {
                // The lights' mix changed, so what bracketed the total no longer does.
                low = 0.0;
                high = std::numeric_limits<double>::infinity();
            }
        }
        apply(total, weights);
    }
    if (!converged && (best.total != total || best.weights != weights)) {
        // Put back the closest frame that kept within the clipping limit.
        apply(best.total, best.weights);
        if (auto drawn = measure(); !drawn) {
            return Reply::failure(code::kInvalidParams, drawn.error().message);
        }
    }

    json::Value result;
    json::Array changes;
    for (std::size_t i = 0; i < lights.size(); ++i) {
        const float to = intensity_of(world, lights[i], units);
        if (to == from[i]) {
            continue;
        }
        json::Value change;
        change["id"] = lights[i].id;
        change["type"] = lights[i].type;
        change["field"] = intensity_field(lights[i], units);
        change["from"] = three_places(from[i]);
        change["to"] = three_places(to);
        changes.push_back(std::move(change));
    }
    result["changes"] = std::move(changes);
    result["before"] = exposure_json(before);
    json::Value end = exposure_json(after);
    if (!shares.empty()) {
        json::Object got;
        for (std::size_t i = 0; i < shares.size(); ++i) {
            got.emplace(shares[i].first.id, rounded(measured[i]));
        }
        end["shares"] = std::move(got);
    }
    result["after"] = std::move(end);
    result["iterations"] = steps;
    result["converged"] = converged;
    return Reply::success(std::move(result));
}

} // namespace

void register_lighting_commands(Registry& registry) {
    json::Value scale = property("number", "Image pixels per window pixel. Default 0.25.");
    scale["minimum"] = 0.05;
    scale["maximum"] = 1;
    json::Value ids = property("array", "The lights to measure, by entity ID. Default: every "
                                        "light, 3D or 2D, that shines (at most 32).");
    ids["items"]["type"] = "string";
    ids["maxItems"] = static_cast<std::int64_t>(kMaxLights);
    json::Value schema;
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["properties"] = json::Object{
        {"scale", std::move(scale)},
        {"lights", property("boolean", "Draw the frame again without each light, to measure "
                                       "what it lights. Default true.")},
        {"ids", std::move(ids)},
    };
    registry.add({
        .name = "lighting.check",
        .description =
            "Draws the world (3D through its camera, or 2D) and reports its exposure, what "
            "each light lights and problems: clipped, crushed, dark, bright or flat "
            "frames, lights that light nothing in view or push it into clipping.",
        .capabilities = {Capability::Read},
        .effect = Effect::Read,
        .params = std::move(schema),
        .handler = lighting_check,
    });

    json::Value tune_scale = property("number", "Image pixels per window pixel. Default 0.25.");
    tune_scale["minimum"] = 0.05;
    tune_scale["maximum"] = 1;
    json::Value tune_ids = property("array", "The lights to tune, by entity ID. Default: every "
                                             "light that shines (at most 32).");
    tune_ids["items"]["type"] = "string";
    tune_ids["maxItems"] = static_cast<std::int64_t>(kMaxLights);
    json::Value mean = property("number", "The frame's mean brightness to reach, 0 to 1. "
                                          "Default 0.4.");
    mean["minimum"] = 0.02;
    mean["maximum"] = 0.98;
    json::Value clipped = property("number", "The largest share of the frame that may clip. "
                                             "Default 0.02.");
    clipped["minimum"] = 0;
    clipped["maximum"] = 1;
    json::Value shares = property("object", "The share of the frame's brightness each named "
                                            "light should give, by entity ID.");
    shares["additionalProperties"] =
        json::Value(json::Object{{"type", "number"}, {"minimum", 0}, {"maximum", 1}});
    json::Value iterations = property("integer", "Most steps to take. Default 6.");
    iterations["minimum"] = 1;
    iterations["maximum"] = 16;
    json::Value tune;
    tune["type"] = "object";
    tune["additionalProperties"] = false;
    tune["properties"] = json::Object{
        {"mean", std::move(mean)},        {"clipped", std::move(clipped)},
        {"shares", std::move(shares)},    {"ids", std::move(tune_ids)},
        {"scale", std::move(tune_scale)}, {"iterations", std::move(iterations)},
    };
    registry.add({
        .name = "light.tune",
        .description = "Scales lights' energy (lux or lumens with physical light units), 3D or "
                       "2D, until the frame's mean brightness, "
                       "clipping and each named light's share reach their targets; returns "
                       "the changes and the exposure before and after.",
        .capabilities = {Capability::Edit},
        .effect = Effect::Edit,
        .params = std::move(tune),
        .handler = light_tune,
    });
}

} // namespace slash::command
