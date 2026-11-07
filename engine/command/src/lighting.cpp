#include "slash/command/lighting.hpp"

#include "slash/canvas/canvas.hpp"
#include "slash/command/session.hpp"
#include "slash/core/text.hpp"
#include "slash/world/components.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
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
constexpr double kLitStep = 2.0 / 255.0;
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
// whether a channel is clipped.
struct Pixels {
    std::vector<double> brightness;
    std::vector<bool> clipped;
};

Pixels pixels_of(const render::Image& image) {
    Pixels out;
    const std::size_t count = static_cast<std::size_t>(image.width) * image.height;
    out.brightness.resize(count);
    out.clipped.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint8_t r = image.pixels[i * 4];
        const std::uint8_t g = image.pixels[(i * 4) + 1];
        const std::uint8_t b = image.pixels[(i * 4) + 2];
        out.brightness[i] = ((0.2126 * r) + (0.7152 * g) + (0.0722 * b)) / 255.0;
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

float& energy_of(world::World& world, const Light& light) {
    if (light.type == "DirectionalLight3D") {
        return world.ecs().get<world::DirectionalLight3D>(light.entity).light_energy;
    }
    if (light.type == "OmniLight3D") {
        return world.ecs().get<world::OmniLight3D>(light.entity).light_energy;
    }
    return world.ecs().get<world::SpotLight3D>(light.entity).light_energy;
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

    std::vector<Light> lights;
    add_lights<world::DirectionalLight3D>(world, "DirectionalLight3D", lights);
    add_lights<world::OmniLight3D>(world, "OmniLight3D", lights);
    add_lights<world::SpotLight3D>(world, "SpotLight3D", lights);
    std::erase_if(lights, [](const Light& light) { return light.id.empty(); });
    if (const json::Value* ids = params.find("ids"); ids != nullptr) {
        std::vector<Light> named;
        const json::Array& list = ids->as_array();
        for (std::size_t i = 0; i < list.size(); ++i) {
            const std::string& id = list[i].as_string();
            const auto found = std::ranges::find(lights, id, &Light::id);
            if (found == lights.end()) {
                std::vector<std::string> known;
                known.reserve(lights.size());
                for (const Light& light : lights) {
                    known.push_back(light.id);
                }
                std::string message = "no 3D light that shines has the id \"" + id + "\"";
                if (const std::string_view hint = closest_match(id, known); !hint.empty()) {
                    message += "; did you mean \"" + std::string(hint) + "\"?";
                }
                return param_error(code::kNotFound, "/ids/" + std::to_string(i), message);
            }
            named.push_back(*found);
        }
        lights = std::move(named);
    }
    std::ranges::sort(lights, {}, &Light::id);
    if (lights.size() > kMaxLights) {
        lights.resize(kMaxLights);
    }

    const canvas::CaptureOptions options{.scale = static_cast<float>(scale)};
    auto whole = canvas::capture(world, session.assets(), options);
    if (!whole) {
        return Reply::failure(whole.error());
    }
    if (whole.value().meshes == 0) {
        return Reply::failure(code::kInvalidParams,
                              "the frame draws no 3D meshes; lighting.check looks at a 3D world "
                              "through its current Camera3D");
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
        clipped += base.clipped[i] ? 1 : 0;
        crushed += base.brightness[i] < kCrushed ? 1 : 0;
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
                lit += change >= kLitStep ? 1 : 0;
                added += std::max(change, 0.0);
                clips += base.clipped[i] && !dark.clipped[i] ? 1 : 0;
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

} // namespace

void register_lighting_commands(Registry& registry) {
    json::Value scale = property("number", "Image pixels per window pixel. Default 0.25.");
    scale["minimum"] = 0.05;
    scale["maximum"] = 1;
    json::Value ids = property("array", "The lights to measure, by entity ID. Default: every 3D "
                                        "light that shines (at most 32).");
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
        .description = "Draws the 3D world through its camera and reports its exposure, what "
                       "each light lights and problems: clipped, crushed, dark, bright or flat "
                       "frames, lights that light nothing in view or push it into clipping.",
        .capabilities = {Capability::Read},
        .effect = Effect::Read,
        .params = std::move(schema),
        .handler = lighting_check,
    });
}

} // namespace slash::command
