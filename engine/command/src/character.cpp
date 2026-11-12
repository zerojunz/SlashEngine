#include "slash/command/character.hpp"

#include "slash/command/session.hpp"
#include "slash/core/text.hpp"
#include "slash/platform/file.hpp"
#include "slash/scene3d/character.hpp"
#include "slash/storyboard/storyboard.hpp"
#include "slash/world/character.hpp"
#include "slash/world/components.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// character.generate, character.check and character.place, over .character.json recipes
// (slash/world/character.hpp) and the models they build (slash/scene3d/character.hpp). Built with
// -ffp-contract=off, so a generated recipe is the same everywhere.
namespace slash::command {
namespace {

// Outlines this far apart, and colors this far apart, tell two characters apart where no
// character_rules entry asks more.
constexpr float kDefaultSilhouette = 0.05F;
constexpr float kDefaultColor = 5.0F;
constexpr std::string_view kRulesKind = "character_rules/";

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

double rounded(double value, double places = 1000.0) {
    return std::round(value * places) / places;
}

json::Value color_value(Vec4 color) {
    json::Array out{rounded(color.x), rounded(color.y), rounded(color.z)};
    if (color.w < 1.0F) {
        out.emplace_back(rounded(color.w));
    }
    return out;
}

json::Value failure(const std::string& file, std::string path, std::string message) {
    json::Value item;
    item["file"] = file;
    item["path"] = std::move(path);
    item["message"] = std::move(message);
    return item;
}

// Project files as a model reads them: a missing one is an error.
scene3d::ModelReader reader_of(Session& session) {
    return [&session](std::string_view path) -> Result<std::string> {
        auto text = session.read_project_file(path);
        if (!text) {
            return text.error();
        }
        if (!text.value()) {
            return make_error(ErrorCode::NotFound, quoted(path) + " doesn't exist");
        }
        return std::move(*text.value());
    };
}

// What character.check and character.generate compare: a recipe's outlines, front and side,
// and its main colors with the share of the model each covers.
struct Look {
    std::string path;
    world::CharacterRecipe recipe;
    scene3d::Silhouette front;
    scene3d::Silhouette side;
    std::vector<std::pair<Vec4, float>> colors;
};

real_t triangle_area(const std::array<float, 3>& a, const std::array<float, 3>& b,
                     const std::array<float, 3>& c) {
    const Vec3 u{b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    const Vec3 v{c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    const Vec3 n{(u.y * v.z) - (u.z * v.y), (u.z * v.x) - (u.x * v.z), (u.x * v.y) - (u.y * v.x)};
    return std::sqrt((n.x * n.x) + (n.y * n.y) + (n.z * n.z)) * 0.5F;
}

Result<Look> look_of(std::string path, world::CharacterRecipe recipe,
                     const scene3d::ModelReader& read) {
    auto model = scene3d::build_character(recipe, read);
    if (!model) {
        return model.error();
    }
    Look out;
    out.path = std::move(path);
    out.recipe = std::move(recipe);
    out.front = scene3d::silhouette(model.value(), false);
    out.side = scene3d::silhouette(model.value(), true);
    // Each material's color by the area it covers; equal colors add up.
    std::vector<std::pair<Vec4, real_t>> areas;
    real_t total = 0.0F;
    for (const scene3d::ModelMesh& mesh : model.value().meshes) {
        for (const scene3d::ModelSurface& surface : mesh.surfaces) {
            if (surface.material < 0) {
                continue;
            }
            const Vec4 color = model.value()
                                   .materials.at(static_cast<std::size_t>(surface.material))
                                   .standard.albedo_color;
            real_t area = 0.0F;
            for (std::size_t i = 0; i + 2 < surface.indices.size(); i += 3) {
                area += triangle_area(surface.vertices.at(surface.indices[i]).position,
                                      surface.vertices.at(surface.indices[i + 1]).position,
                                      surface.vertices.at(surface.indices[i + 2]).position);
            }
            total += area;
            const auto same = std::ranges::find_if(
                areas, [&](const auto& counted) { return counted.first == color; });
            if (same == areas.end()) {
                areas.emplace_back(color, area);
            } else {
                same->second += area;
            }
        }
    }
    std::ranges::stable_sort(areas,
                             [](const auto& a, const auto& b) { return a.second > b.second; });
    // The main colors: those covering a twentieth of it or more, at most six.
    for (const auto& [color, area] : areas) {
        const float share = total > 0.0F ? area / total : 0.0F;
        if (out.colors.size() == 6 || (share < 0.05F && !out.colors.empty())) {
            break;
        }
        out.colors.emplace_back(color, share);
    }
    return out;
}

// How far a's main colors are from b's: each one's distance to b's nearest, by its share.
float colors_from(const Look& a, const Look& b) {
    float sum = 0.0F;
    float shares = 0.0F;
    for (const auto& [color, share] : a.colors) {
        float nearest = 1e9F;
        for (const auto& other : b.colors) {
            nearest = std::min(nearest, world::color_difference(color, other.first));
        }
        sum += nearest * share;
        shares += share;
    }
    return shares > 0.0F ? sum / shares : 0.0F;
}

struct Apart {
    float silhouette = 0.0F;
    float color = 0.0F;
};

// How far apart two characters look: their outlines' difference, front or side, whichever is
// more, and their colors', whichever way is more.
Apart apart(const Look& a, const Look& b) {
    return {.silhouette = std::max(scene3d::silhouette_difference(a.front, b.front),
                                   scene3d::silhouette_difference(a.side, b.side)),
            .color = std::max(colors_from(a, b), colors_from(b, a))};
}

// A character_rules entry, read from the storyboard.
struct Rules {
    std::string id;
    // A copy: the storyboard it was read from is gone once the rules are loaded.
    json::Value entry;
    float silhouette = kDefaultSilhouette;
    float color = kDefaultColor;
    std::optional<double> min_height;
    std::optional<double> max_height;
    std::optional<std::int64_t> max_props;
    std::string palette_id;
    std::vector<std::array<int, 3>> palette;
    int tolerance = 0;
};

bool covers(const json::Value& entry, std::string_view path) {
    const json::Value* links = entry.find("links");
    if (links == nullptr || !links->is_array()) {
        return entry.find("refs") == nullptr;
    }
    return std::ranges::any_of(links->as_array(), [&](const json::Value& link) {
        if (!link.is_string()) {
            return false;
        }
        const std::string_view folder = link.as_string();
        return path.starts_with(folder) && (path.size() == folder.size() ||
                                            path[folder.size()] == '/' || folder.ends_with('/'));
    });
}

std::optional<std::array<int, 3>> hex_color(const json::Value& value) {
    if (!value.is_string() || value.as_string().size() != 7 || value.as_string()[0] != '#') {
        return std::nullopt;
    }
    std::array<int, 3> out{};
    for (std::size_t c = 0; c < 3; ++c) {
        int channel = 0;
        for (std::size_t k = 0; k < 2; ++k) {
            const char digit = value.as_string()[1 + (2 * c) + k];
            int v = 0;
            if (digit >= '0' && digit <= '9') {
                v = digit - '0';
            } else if (digit >= 'a' && digit <= 'f') {
                v = digit - 'a' + 10;
            } else if (digit >= 'A' && digit <= 'F') {
                v = digit - 'A' + 10;
            } else {
                return std::nullopt;
            }
            channel = (channel * 16) + v;
        }
        out[c] = channel;
    }
    return out;
}

Result<std::vector<Rules>> load_rules(Session& session) {
    auto loaded = storyboard::load(
        [&session](std::string_view path) { return session.read_project_file(path); });
    if (!loaded) {
        return loaded.error();
    }
    std::vector<Rules> out;
    const json::Object& entries = loaded.value().entries;
    for (const auto& [id, entry] : entries) {
        if (!std::string_view(id).starts_with(kRulesKind)) {
            continue;
        }
        Rules rules;
        rules.id = id;
        rules.entry = entry;
        const auto number = [&](std::string_view field) -> std::optional<double> {
            const json::Value* value = entry.find(field);
            if (value == nullptr || !value->is_number()) {
                return std::nullopt;
            }
            return value->as_double();
        };
        if (auto value = number("silhouette_difference")) {
            rules.silhouette = static_cast<float>(*value);
        }
        if (auto value = number("color_difference")) {
            rules.color = static_cast<float>(*value);
        }
        rules.min_height = number("min_height");
        rules.max_height = number("max_height");
        if (const json::Value* most = entry.find("max_props"); most != nullptr && most->is_int()) {
            rules.max_props = most->as_int();
        }
        if (const json::Value* palette = entry.find("palette");
            palette != nullptr && palette->is_string()) {
            rules.palette_id = palette->as_string();
            if (const auto found = entries.find(rules.palette_id); found != entries.end()) {
                if (const json::Value* colors = found->second.find("colors");
                    colors != nullptr && colors->is_object()) {
                    for (const auto& [name, color] : colors->as_object()) {
                        if (auto rgb = hex_color(color)) {
                            rules.palette.push_back(*rgb);
                        }
                    }
                }
                if (const json::Value* tolerance = found->second.find("tolerance");
                    tolerance != nullptr && tolerance->is_int()) {
                    rules.tolerance = static_cast<int>(tolerance->as_int());
                }
            }
        }
        out.push_back(std::move(rules));
    }
    return out;
}

std::vector<const Rules*> rules_for(const std::vector<Rules>& all, std::string_view path) {
    std::vector<const Rules*> out;
    for (const Rules& rules : all) {
        if (covers(rules.entry, path)) {
            out.push_back(&rules);
        }
    }
    return out;
}

std::array<int, 3> bytes_of(Vec4 color) {
    const auto byte = [](float v) {
        return static_cast<int>(std::lround(std::clamp(v, 0.0F, 1.0F) * 255.0F));
    };
    return {byte(color.x), byte(color.y), byte(color.z)};
}

bool in_palette(const Rules& rules, Vec4 color) {
    const std::array<int, 3> rgb = bytes_of(color);
    return std::ranges::any_of(rules.palette, [&](const std::array<int, 3>& allowed) {
        return std::abs(allowed[0] - rgb[0]) <= rules.tolerance &&
               std::abs(allowed[1] - rgb[1]) <= rules.tolerance &&
               std::abs(allowed[2] - rgb[2]) <= rules.tolerance;
    });
}

// Every .character.json file in the project, build/ and storyboard/ aside.
Result<std::vector<std::string>> character_files(Session& session) {
    auto listed = platform::list_files(session.options().project_root, [](std::string_view folder) {
        return folder != "build/" && folder != "storyboard/";
    });
    if (!listed) {
        return listed.error();
    }
    std::vector<std::string> out;
    for (std::string& path : listed.value()) {
        if (world::is_character_path(path)) {
            out.push_back(std::move(path));
        }
    }
    return out;
}

// A recipe file read, validated and parsed, or its problems.
Result<world::CharacterRecipe> read_recipe(const std::string& path,
                                           const scene3d::ModelReader& read,
                                           std::vector<json::Value>& errors) {
    auto text = read(path);
    if (!text) {
        errors.push_back(failure(path, "", text.error().message));
        return text.error();
    }
    const std::vector<json::ValidationError> problems =
        scene3d::check_character_model(text.value(), read);
    for (const json::ValidationError& problem : problems) {
        errors.push_back(failure(path, problem.path, problem.message));
    }
    if (!problems.empty()) {
        return make_error(ErrorCode::InvalidArgument, "the recipe has problems");
    }
    auto doc = json::parse(text.value());
    if (!doc) {
        return doc.error();
    }
    return world::parse_character(doc.value(), read);
}

std::string number_text(double value) {
    std::string text = std::to_string(rounded(value, 100.0));
    while (text.ends_with('0')) {
        text.pop_back();
    }
    if (text.ends_with('.')) {
        text.pop_back();
    }
    return text;
}

// ---- character.check ----

// A recipe's own problems under one set of rules: height, props and palette.
void check_rules(const Look& look, const Rules& rules, std::vector<json::Value>& errors) {
    const world::CharacterRecipe& recipe = look.recipe;
    const double height = recipe.body.height;
    if (rules.min_height && height < *rules.min_height) {
        errors.push_back(failure(look.path, "/body/height",
                                 "is " + number_text(height) + " m; " + rules.id + " asks " +
                                     number_text(*rules.min_height) + " m or more"));
    }
    if (rules.max_height && height > *rules.max_height) {
        errors.push_back(failure(look.path, "/body/height",
                                 "is " + number_text(height) + " m; " + rules.id + " asks " +
                                     number_text(*rules.max_height) + " m or less"));
    }
    if (rules.max_props && static_cast<std::int64_t>(recipe.props.size()) > *rules.max_props) {
        errors.push_back(failure(look.path, "/props",
                                 std::to_string(recipe.props.size()) + " props; " + rules.id +
                                     " allows " + std::to_string(*rules.max_props)));
    }
    if (rules.palette.empty()) {
        return;
    }
    const auto check = [&](Vec4 color, std::string pointer) {
        if (!in_palette(rules, color)) {
            errors.push_back(failure(look.path, std::move(pointer),
                                     "isn't a color of " + rules.palette_id + ", which " +
                                         rules.id + " keeps characters to"));
        }
    };
    check(recipe.body.skin, "/body/skin");
    if (recipe.hair.style != "none" || recipe.face.beard != "none") {
        check(recipe.hair.color, "/hair/color");
    }
    for (std::size_t i = 0; i < recipe.clothing.size(); ++i) {
        check(recipe.clothing[i].color, "/clothing/" + std::to_string(i) + "/color");
    }
}

Reply character_check(Session& session, const json::Value& params) {
    const scene3d::ModelReader read = reader_of(session);
    auto files = character_files(session);
    if (!files) {
        return Reply::failure(files.error());
    }
    std::vector<std::string> checked;
    if (const json::Value* paths = params.find("paths"); paths != nullptr) {
        for (const json::Value& path : paths->as_array()) {
            checked.push_back(path.as_string());
            if (std::ranges::find(files.value(), path.as_string()) == files.value().end()) {
                files.value().push_back(path.as_string());
            }
        }
    } else {
        checked = files.value();
    }
    auto rules = load_rules(session);
    if (!rules) {
        return Reply::failure(rules.error());
    }
    std::vector<json::Value> errors;
    std::vector<Look> looks;
    for (const std::string& path : files.value()) {
        const bool mine = std::ranges::find(checked, path) != checked.end();
        std::vector<json::Value> problems;
        auto recipe = read_recipe(path, read, problems);
        if (mine) {
            errors.insert(errors.end(), problems.begin(), problems.end());
        }
        if (!recipe) {
            continue;
        }
        auto look = look_of(path, std::move(recipe).value(), read);
        if (!look) {
            if (mine) {
                errors.push_back(failure(path, "", look.error().message));
            }
            continue;
        }
        looks.push_back(std::move(look).value());
    }
    const bool measures = params.find("measures") != nullptr && params.find("measures")->as_bool();
    json::Array pairs;
    for (std::size_t a = 0; a < looks.size(); ++a) {
        const bool a_mine = std::ranges::find(checked, looks[a].path) != checked.end();
        const std::vector<const Rules*> covering = rules_for(rules.value(), looks[a].path);
        if (a_mine) {
            for (const Rules* rule : covering) {
                check_rules(looks[a], *rule, errors);
            }
        }
        for (std::size_t b = 0; b < looks.size(); ++b) {
            if (a == b) {
                continue;
            }
            const bool b_mine = std::ranges::find(checked, looks[b].path) != checked.end();
            // Each pair once: on the checked file, the later one when both are.
            if (!a_mine || (b_mine && looks[b].path > looks[a].path)) {
                continue;
            }
            const Apart measured = apart(looks[a], looks[b]);
            if (measures) {
                json::Value pair;
                pair["a"] = looks[a].path;
                pair["b"] = looks[b].path;
                pair["silhouette"] = rounded(measured.silhouette);
                pair["color"] = rounded(measured.color, 100.0);
                pairs.push_back(std::move(pair));
            }
            float silhouette = kDefaultSilhouette;
            float color = kDefaultColor;
            std::string asks = "the default";
            for (const Rules* rule : covering) {
                if (rule->silhouette >= silhouette || rule->color >= color) {
                    asks = rule->id;
                }
                silhouette = std::max(silhouette, rule->silhouette);
                color = std::max(color, rule->color);
            }
            if (measured.silhouette < silhouette) {
                errors.push_back(failure(looks[a].path, "",
                                         "its outline is too like " + looks[b].path + "'s: " +
                                             number_text(measured.silhouette) + " apart, and " +
                                             asks + " asks " + number_text(silhouette) +
                                             "; change the build, height or clothing"));
            }
            if (measured.color < color) {
                errors.push_back(failure(looks[a].path, "",
                                         "its colors are too like " + looks[b].path +
                                             "'s: " + number_text(measured.color) + " apart, and " +
                                             asks + " asks " + number_text(color) +
                                             "; change the skin, hair or clothing colors"));
            }
        }
    }
    json::Value result;
    result["characters"] = static_cast<std::int64_t>(looks.size());
    json::Array list;
    for (json::Value& error : errors) {
        list.push_back(std::move(error));
    }
    result["errors"] = std::move(list);
    if (measures) {
        result["pairs"] = std::move(pairs);
    }
    return Reply::success(std::move(result));
}

// ---- character.generate ----

// Random numbers from a seed, the same everywhere: splitmix64.
struct Random {
    std::uint64_t state;

    std::uint64_t next() {
        state += 0x9e3779b97f4a7c15ULL;
        std::uint64_t z = state;
        z = (z ^ (z >> 30U)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27U)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31U);
    }
    // In [0, 1).
    float unit() { return static_cast<float>(next() >> 40U) / 16777216.0F; }
    float between(float low, float high) { return low + ((high - low) * unit()); }
    std::size_t pick(std::size_t count) {
        return static_cast<std::size_t>(next() % static_cast<std::uint64_t>(count));
    }
    bool chance(float p) { return unit() < p; }
};

std::uint64_t hash_text(std::string_view text) {
    std::uint64_t h = 1469598103934665603ULL;
    for (const char c : text) {
        h = (h ^ static_cast<unsigned char>(c)) * 1099511628211ULL;
    }
    return h;
}

constexpr std::array<Vec4, 7> kSkins{{{0.98F, 0.84F, 0.73F, 1.0F},
                                      {0.93F, 0.76F, 0.62F, 1.0F},
                                      {0.87F, 0.68F, 0.55F, 1.0F},
                                      {0.76F, 0.57F, 0.42F, 1.0F},
                                      {0.62F, 0.44F, 0.31F, 1.0F},
                                      {0.48F, 0.33F, 0.22F, 1.0F},
                                      {0.36F, 0.24F, 0.16F, 1.0F}}};
constexpr std::array<Vec4, 8> kHair{{{0.06F, 0.05F, 0.05F, 1.0F},
                                     {0.18F, 0.12F, 0.08F, 1.0F},
                                     {0.35F, 0.22F, 0.12F, 1.0F},
                                     {0.5F, 0.2F, 0.1F, 1.0F},
                                     {0.85F, 0.72F, 0.45F, 1.0F},
                                     {0.6F, 0.6F, 0.62F, 1.0F},
                                     {0.92F, 0.92F, 0.9F, 1.0F},
                                     {0.6F, 0.15F, 0.08F, 1.0F}}};
constexpr std::array<Vec4, 4> kEyes{{{0.3F, 0.22F, 0.12F, 1.0F},
                                     {0.25F, 0.4F, 0.6F, 1.0F},
                                     {0.3F, 0.45F, 0.25F, 1.0F},
                                     {0.45F, 0.35F, 0.15F, 1.0F}}};

// A color of hue h (0 to 1), saturation s and value v.
Vec4 from_hsv(float h, float s, float v) {
    const float scaled = h * 6.0F;
    const float sector = std::floor(scaled);
    const float f = scaled - sector;
    const float p = v * (1.0F - s);
    const float q = v * (1.0F - (s * f));
    const float t = v * (1.0F - (s * (1.0F - f)));
    switch (static_cast<int>(sector) % 6) {
    case 0:
        return {v, t, p, 1.0F};
    case 1:
        return {q, v, p, 1.0F};
    case 2:
        return {p, v, t, 1.0F};
    case 3:
        return {p, q, v, 1.0F};
    case 4:
        return {t, p, v, 1.0F};
    default:
        return {v, p, q, 1.0F};
    }
}

Vec4 rounded_color(Vec4 c) {
    return {static_cast<float>(rounded(c.x)), static_cast<float>(rounded(c.y)),
            static_cast<float>(rounded(c.z)), 1.0F};
}

// What a generated character's colors come from: a palette's, or any.
struct Colors {
    std::vector<Vec4> palette;

    [[nodiscard]] Vec4 skin(Random& random) const {
        const Vec4 tone = kSkins.at(random.pick(kSkins.size()));
        return palette.empty() ? tone : nearest(tone);
    }
    [[nodiscard]] Vec4 hair(Random& random) const {
        const Vec4 shade = kHair.at(random.pick(kHair.size()));
        return palette.empty() ? shade : nearest(shade);
    }
    [[nodiscard]] Vec4 cloth(Random& random) const {
        if (!palette.empty()) {
            return palette.at(random.pick(palette.size()));
        }
        // One draw a statement, so every compiler draws them in this order (last first, as the
        // first recipes were made).
        const float value = random.between(0.2F, 0.85F);
        const float saturation = random.between(0.25F, 0.8F);
        const float hue = random.unit();
        return rounded_color(from_hsv(hue, saturation, value));
    }
    [[nodiscard]] Vec4 nearest(Vec4 color) const {
        Vec4 best = palette.front();
        float distance = 1e9F;
        for (const Vec4 candidate : palette) {
            const float d = world::color_difference(color, candidate);
            if (d < distance) {
                distance = d;
                best = candidate;
            }
        }
        return best;
    }
};

world::Garment garment(std::string kind, Vec4 color, std::string material = "cloth",
                       float length = 1.0F, float loose = 0.0F) {
    world::Garment out;
    out.kind = std::move(kind);
    out.color = color;
    out.material = std::move(material);
    out.length = static_cast<float>(rounded(length, 100.0));
    out.loose = static_cast<float>(rounded(loose, 100.0));
    return out;
}

// The garments of an outfit, each drawn in its own statements: C++ leaves the order of a call's
// arguments to the compiler, so draws made as arguments would come out in another order on
// another compiler. They are drawn last first, the order the first recipes were made in.
struct Wardrobe {
    Random& random;
    const Colors& colors;
    std::vector<world::Garment> out;

    // Puts on `kind`: its looseness and length where their ranges are open, then `other` for
    // its material with chance `other_chance`, then its color.
    void wear(std::string kind, std::string material = "cloth",
              std::array<float, 2> length = {1.0F, 1.0F}, std::array<float, 2> loose = {0.0F, 0.0F},
              float other_chance = 0.0F, const char* other = nullptr) {
        const float o = loose[0] == loose[1] ? loose[0] : random.between(loose[0], loose[1]);
        const float l = length[0] == length[1] ? length[0] : random.between(length[0], length[1]);
        if (other != nullptr && random.chance(other_chance)) {
            material = other;
        }
        const Vec4 color = colors.cloth(random);
        out.push_back(garment(std::move(kind), color, std::move(material), l, o));
    }
};

// One of a few outfits, in the order garments go on.
std::vector<world::Garment> outfit(Random& random, const Colors& colors) {
    Wardrobe w{.random = random, .colors = colors, .out = {}};
    const std::size_t kind = random.pick(8);
    const float sleeves = random.between(0.2F, 1.0F);
    switch (kind) {
    case 0:
        w.wear("shirt", "cloth", {sleeves, sleeves});
        w.wear("pants");
        w.wear("shoes", "leather");
        break;
    case 1:
        w.wear("shirt", "cloth", {sleeves, sleeves});
        w.wear("pants");
        w.wear("boots", "leather", {0.3F, 1.0F});
        w.wear("jacket", "cloth", {1.0F, 1.0F}, {0.0F, 0.4F}, 0.5F, "leather");
        break;
    case 2:
        w.wear("dress", "cloth", {0.4F, 1.0F}, {0.1F, 0.6F}, 0.4F, "silk");
        w.wear("shoes", "leather");
        w.wear("belt", "leather");
        break;
    case 3:
        w.wear("shirt");
        w.wear("shoes", "leather");
        w.wear("robe", "cloth", {0.6F, 1.0F}, {0.2F, 0.7F});
        w.wear("belt", "leather");
        if (random.chance(0.5F)) {
            w.wear("hood");
        }
        break;
    case 4:
        w.wear("shirt");
        w.wear("pants");
        w.wear("boots", "metal", {0.5F, 1.0F});
        w.wear("gloves", "leather", {0.2F, 1.0F});
        w.wear("armor", "metal");
        if (random.chance(0.5F)) {
            w.wear("helmet", "metal");
        }
        break;
    case 5:
        w.wear("shirt");
        w.wear("pants");
        w.wear("boots", "leather", {0.3F, 1.0F});
        w.wear("coat", "cloth", {0.4F, 1.0F}, {0.1F, 0.5F}, 0.3F, "fur");
        w.wear("scarf", "cloth", {0.3F, 1.0F});
        if (random.chance(0.5F)) {
            w.wear("hat", "cloth", {0.0F, 1.0F});
        }
        break;
    case 6:
        w.wear("shirt", "cloth", {0.0F, 0.4F});
        w.wear("shorts", "cloth", {0.2F, 1.0F});
        w.wear("shoes", "leather");
        w.wear("hat", "cloth", {0.0F, 0.6F});
        break;
    default:
        w.wear("shirt", "cloth", {sleeves, sleeves});
        w.wear("skirt", "cloth", {0.3F, 0.9F}, {0.1F, 0.6F});
        w.wear("boots", "leather", {0.3F, 1.0F});
        w.wear("cape", "cloth", {0.5F, 1.0F});
        break;
    }
    return std::move(w.out);
}

world::CharacterRecipe candidate(Random& random, const json::Value& params, const Colors& colors,
                                 double min_height, double max_height) {
    world::CharacterRecipe recipe;
    recipe.name = params.find("name")->as_string();
    if (const json::Value* role = params.find("role"); role != nullptr) {
        recipe.role = role->as_string();
    }
    if (const json::Value* faction = params.find("faction"); faction != nullptr) {
        recipe.faction = faction->as_string();
    }
    const auto r2 = [](float v) { return static_cast<float>(rounded(v, 100.0)); };
    world::CharacterBody& body = recipe.body;
    body.height =
        r2(random.between(static_cast<float>(min_height), static_cast<float>(max_height)));
    body.build = r2(random.between(0.15F, 0.9F));
    body.head = r2(random.between(0.92F, 1.1F));
    body.neck = r2(random.between(0.9F, 1.1F));
    body.shoulders = r2(random.between(0.85F, 1.2F));
    body.hips = r2(random.between(0.85F, 1.15F));
    body.arms = r2(random.between(0.95F, 1.05F));
    body.legs = r2(random.between(0.92F, 1.08F));
    body.skin = colors.skin(random);
    world::CharacterFace& face = recipe.face;
    face.eyes = kEyes.at(random.pick(kEyes.size()));
    face.eye_size = r2(random.between(0.9F, 1.15F));
    face.eye_spacing = r2(random.between(0.9F, 1.1F));
    face.nose = r2(random.between(0.85F, 1.2F));
    face.ears = r2(random.between(0.9F, 1.1F));
    static constexpr std::array<std::string_view, 4> kBeards{"none", "stubble", "short", "long"};
    face.beard = random.chance(0.5F) ? "none" : std::string(kBeards.at(1 + random.pick(3)));
    static constexpr std::array<std::string_view, 6> kStyles{"none",     "short", "long",
                                                             "ponytail", "bun",   "mohawk"};
    recipe.hair.style = std::string(kStyles.at(random.pick(kStyles.size())));
    recipe.hair.color = colors.hair(random);
    recipe.hair.length = r2(random.between(0.2F, 0.9F));
    recipe.clothing = outfit(random, colors);
    return recipe;
}

// A recipe as a .character.json file.
json::Value recipe_json(const world::CharacterRecipe& recipe) {
    json::Value out;
    out["format"] = 1;
    out["name"] = recipe.name;
    out["role"] = recipe.role;
    if (!recipe.faction.empty()) {
        out["faction"] = recipe.faction;
    }
    const world::CharacterBody& body = recipe.body;
    json::Value& b = out["body"];
    b["height"] = rounded(body.height, 100.0);
    b["build"] = rounded(body.build, 100.0);
    b["head"] = rounded(body.head, 100.0);
    b["neck"] = rounded(body.neck, 100.0);
    b["shoulders"] = rounded(body.shoulders, 100.0);
    b["hips"] = rounded(body.hips, 100.0);
    b["arms"] = rounded(body.arms, 100.0);
    b["legs"] = rounded(body.legs, 100.0);
    b["skin"] = color_value(body.skin);
    const world::CharacterFace& face = recipe.face;
    json::Value& f = out["face"];
    f["eyes"] = color_value(face.eyes);
    f["eye_size"] = rounded(face.eye_size, 100.0);
    f["eye_spacing"] = rounded(face.eye_spacing, 100.0);
    f["nose"] = rounded(face.nose, 100.0);
    f["ears"] = rounded(face.ears, 100.0);
    f["lips"] = color_value(face.lips);
    f["beard"] = face.beard;
    json::Value& h = out["hair"];
    h["style"] = recipe.hair.style;
    h["color"] = color_value(recipe.hair.color);
    h["length"] = rounded(recipe.hair.length, 100.0);
    json::Array clothing;
    for (const world::Garment& garment : recipe.clothing) {
        json::Value item;
        item["kind"] = garment.kind;
        item["color"] = color_value(garment.color);
        if (garment.material != "cloth") {
            item["material"] = garment.material;
        }
        if (garment.length != 1.0F) {
            item["length"] = rounded(garment.length, 100.0);
        }
        if (garment.loose != 0.0F) {
            item["loose"] = rounded(garment.loose, 100.0);
        }
        if (garment.cloth) {
            item["cloth"] = true;
        }
        clothing.push_back(std::move(item));
    }
    out["clothing"] = std::move(clothing);
    return out;
}

std::string slug(std::string_view name) {
    std::string out;
    for (const char c : name) {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            out += c;
        } else if (c >= 'A' && c <= 'Z') {
            out += static_cast<char>(c - 'A' + 'a');
        } else if (!out.empty() && out.back() != '_') {
            out += '_';
        }
    }
    while (out.ends_with('_')) {
        out.pop_back();
    }
    return out.empty() ? std::string("character") : out;
}

Reply character_generate(Session& session, const json::Value& params) {
    const std::string& name = params.find("name")->as_string();
    std::string out = "characters/" + slug(name) + std::string(world::kCharacterSuffix);
    if (const json::Value* given = params.find("out"); given != nullptr) {
        out = given->as_string();
    }
    if (!world::is_character_path(out)) {
        return param_error(code::kInvalidParams, "/out",
                           quoted(out) + " must end in " + std::string(world::kCharacterSuffix));
    }
    const scene3d::ModelReader read = reader_of(session);
    auto files = character_files(session);
    if (!files) {
        return Reply::failure(files.error());
    }
    auto rules = load_rules(session);
    if (!rules) {
        return Reply::failure(rules.error());
    }
    // The others it must look unlike.
    std::vector<Look> others;
    for (const std::string& path : files.value()) {
        if (path == out) {
            continue;
        }
        std::vector<json::Value> ignored;
        auto recipe = read_recipe(path, read, ignored);
        if (!recipe) {
            continue;
        }
        if (auto look = look_of(path, std::move(recipe).value(), read); look) {
            others.push_back(std::move(look).value());
        }
    }
    // The rules that cover where it goes.
    float silhouette = kDefaultSilhouette;
    float color = kDefaultColor;
    const std::string role =
        params.find("role") != nullptr ? params.find("role")->as_string() : std::string("npc");
    double min_height = role == "player" ? 1.65 : 1.5;
    double max_height = role == "player" ? 1.85 : 1.95;
    Colors colors;
    for (const Rules* rule : rules_for(rules.value(), out)) {
        silhouette = std::max(silhouette, rule->silhouette);
        color = std::max(color, rule->color);
        if (rule->min_height) {
            min_height = *rule->min_height;
            max_height = std::max(max_height, min_height);
        }
        if (rule->max_height) {
            max_height = *rule->max_height;
            min_height = std::min(min_height, max_height);
        }
        for (const std::array<int, 3>& rgb : rule->palette) {
            colors.palette.push_back(rounded_color({static_cast<float>(rgb[0]) / 255.0F,
                                                    static_cast<float>(rgb[1]) / 255.0F,
                                                    static_cast<float>(rgb[2]) / 255.0F, 1.0F}));
        }
    }
    const std::uint64_t seed = params.find("seed") != nullptr
                                   ? static_cast<std::uint64_t>(params.find("seed")->as_int())
                                   : 0U;
    const std::int64_t count =
        params.find("candidates") != nullptr ? params.find("candidates")->as_int() : 24;
    Random random{hash_text(name) ^ (seed * 0x9e3779b97f4a7c15ULL)};
    std::optional<Look> best;
    float best_score = -1.0F;
    std::size_t best_nearest = 0;
    Apart best_apart;
    for (std::int64_t k = 0; k < count; ++k) {
        auto look = look_of(out, candidate(random, params, colors, min_height, max_height), read);
        if (!look) {
            return Reply::failure(look.error());
        }
        // How far it is from the nearest other, in shares of what the rules ask.
        float score = others.empty() ? 0.0F : 1e9F;
        std::size_t nearest = 0;
        Apart nearest_apart;
        for (std::size_t o = 0; o < others.size(); ++o) {
            const Apart measured = apart(look.value(), others[o]);
            const float s = std::min(measured.silhouette / silhouette, measured.color / color);
            if (s < score) {
                score = s;
                nearest = o;
                nearest_apart = measured;
            }
        }
        if (score > best_score) {
            best_score = score;
            best = std::move(look).value();
            best_nearest = nearest;
            best_apart = nearest_apart;
        }
    }
    if (!best) {
        return Reply::failure(code::kInternalError, "no candidate was made");
    }
    const json::Value file = recipe_json(best->recipe);
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
    result["height"] = rounded(best->recipe.body.height, 100.0);
    json::Array garments;
    for (const world::Garment& worn : best->recipe.clothing) {
        garments.emplace_back(worn.kind);
    }
    result["clothing"] = std::move(garments);
    if (!others.empty()) {
        json::Value nearest;
        nearest["path"] = others[best_nearest].path;
        nearest["silhouette"] = rounded(best_apart.silhouette);
        nearest["color"] = rounded(best_apart.color, 100.0);
        result["nearest"] = std::move(nearest);
        if (best_apart.silhouette < silhouette || best_apart.color < color) {
            result["warning"] = "no candidate was as different from " + others[best_nearest].path +
                                " as the rules ask; try another seed or more candidates";
        }
    }
    return Reply::success(std::move(result));
}

// ---- character.place ----

Reply character_place(Session& session, const json::Value& params) {
    world::World& world = session.world();
    const std::string& recipe_path = params.find("recipe")->as_string();
    if (!world::is_character_path(recipe_path)) {
        return param_error(code::kInvalidParams, "/recipe",
                           quoted(recipe_path) + " must end in " +
                               std::string(world::kCharacterSuffix));
    }
    const scene3d::ModelReader read = reader_of(session);
    std::vector<json::Value> problems;
    auto recipe = read_recipe(recipe_path, read, problems);
    if (!recipe) {
        std::string message = recipe.error().message;
        if (!problems.empty()) {
            const std::string& where = problems.front().find("path")->as_string();
            message = problems.front().find("message")->as_string();
            if (!where.empty()) {
                message = where + ": " + message;
            }
        }
        if (!message.starts_with(quoted(recipe_path))) {
            message = quoted(recipe_path) + ": " + message;
        }
        return param_error(code::kInvalidParams, "/recipe", message);
    }
    const std::string& id = params.find("id")->as_string();
    if (world.find(id) != entt::null) {
        return param_error(code::kInvalidParams, "/id", quoted(id) + " already exists");
    }
    const json::Array& at = params.find("at")->as_array();
    const bool flat = at.size() == 2;
    entt::entity parent{entt::null};
    if (const json::Value* given = params.find("parent"); given != nullptr) {
        parent = world.find(given->as_string());
        if (parent == entt::null) {
            return param_error(code::kNotFound, "/parent",
                               "no entity has the id " + quoted(given->as_string()));
        }
    }
    auto created = world.create(id);
    if (!created) {
        return param_error(code::kInvalidParams, "/id", created.error().message);
    }
    const entt::entity entity = created.value();
    auto& ecs = world.ecs();
    json::Value result;
    result["id"] = id;
    json::Array components;
    if (flat) {
        world::Transform2D transform;
        transform.position = {static_cast<real_t>(at[0].as_double()),
                              static_cast<real_t>(at[1].as_double())};
        ecs.emplace<world::Transform2D>(entity, transform);
        world::Character2D character;
        character.recipe = recipe_path;
        if (const json::Value* ppm = params.find("pixels_per_meter"); ppm != nullptr) {
            character.pixels_per_meter = static_cast<float>(ppm->as_double());
        }
        if (const json::Value* flip = params.find("flip_h"); flip != nullptr) {
            character.flip_h = flip->as_bool();
        }
        ecs.emplace<world::Character2D>(entity, character);
        components = {"Transform2D", "Character2D"};
        if (parent != entt::null) {
            if (auto* tree = ecs.try_get<world::Transform2D>(parent); tree != nullptr) {
                tree->children.push_back(entity);
            }
        }
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
        instance.mesh = recipe_path;
        ecs.emplace<world::MeshInstance3D>(entity, instance);
        components = {"Transform3D", "MeshInstance3D"};
        if (!recipe.value().hair.groom.empty()) {
            world::HairInstance3D hair;
            hair.groom = recipe.value().hair.groom;
            hair.section = "scalp";
            ecs.emplace<world::HairInstance3D>(entity, hair);
            components.emplace_back("HairInstance3D");
        }
        for (const world::Garment& worn : recipe.value().clothing) {
            if (worn.cloth) {
                world::Cloth3D cloth;
                cloth.section = worn.kind;
                ecs.emplace<world::Cloth3D>(entity, cloth);
                components.emplace_back("Cloth3D");
                break;
            }
        }
        if (parent != entt::null) {
            if (auto* tree = ecs.try_get<world::Transform3D>(parent); tree != nullptr) {
                tree->children.push_back(entity);
            }
        }
    }
    result["components"] = std::move(components);
    result["height"] = rounded(recipe.value().body.height, 100.0);
    return Reply::success(std::move(result));
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

json::Value text_property(std::string_view description) {
    json::Value out = property("string", description);
    out["minLength"] = 1;
    return out;
}

} // namespace

void register_character_commands(Registry& registry) {
    using enum Capability;
    json::Value role = property("string", R"("player" or "npc". Default npc.)");
    role["enum"] = json::Array{"player", "npc"};
    json::Value seed = property("integer", "Picks other candidates. Default 0.");
    seed["minimum"] = 0;
    json::Value candidates = property("integer", "How many to make and choose from. Default 24.");
    candidates["minimum"] = 1;
    candidates["maximum"] = 64;
    registry.add({
        .name = "character.generate",
        .description = "Writes a .character.json recipe for a new player or NPC: makes candidates "
                       "(build, face, hair, an outfit) within the storyboard's character_rules "
                       "(heights, palette) and keeps the one whose outline and colors are least "
                       "like the project's other characters. Returns the file, its nearest "
                       "other and how far apart they are.",
        .capabilities = {Read, Edit, Filesystem},
        .effect = Effect::Edit,
        .params = schema_of({{"name", text_property("Who it is.")},
                             {"role", std::move(role)},
                             {"faction", text_property("A storyboard faction's ID.")},
                             {"out", text_property("The file to write. Default "
                                                   "characters/<name>.character.json.")},
                             {"seed", std::move(seed)},
                             {"candidates", std::move(candidates)}},
                            {"name"}),
        .handler = character_generate,
    });
    json::Value paths = property("array", "Recipes to check, against every other. Default all.");
    paths["items"] = text_property("A .character.json file.");
    registry.add({
        .name = "character.check",
        .description = "Checks the project's .character.json recipes: each builds, and keeps the "
                       "storyboard's character_rules (outlines front or side and main colors "
                       "far enough from every other character's, height, palette, props). "
                       "Returns errors with their file and JSON Pointer.",
        .capabilities = {Read, Filesystem},
        .effect = Effect::Read,
        .params = schema_of({{"paths", std::move(paths)},
                             {"measures", property("boolean", "Also return how far apart each "
                                                              "pair is. Default false.")}},
                            {}),
        .handler = character_check,
    });
    json::Value at = property("array", "Where: [x, y] pixels puts a Character2D in 2D, "
                                       "[x, y, z] meters a model in 3D.");
    at["items"]["type"] = "number";
    at["minItems"] = 2;
    at["maxItems"] = 3;
    json::Value ppm = property("number", "2D: pixels a meter. Default 100.");
    ppm["exclusiveMinimum"] = 0;
    registry.add({
        .name = "character.place",
        .description = "Makes an entity of a .character.json recipe: in 3D a MeshInstance3D "
                       "drawing its rigged model, with a HairInstance3D on its scalp for a groom "
                       "and a Cloth3D on a cloth garment; in 2D a Character2D, whose cut-out's "
                       "bones the character_2d system keeps as <id>_<bone> children.",
        .capabilities = {Read, Edit},
        .effect = Effect::Edit,
        .params = schema_of({{"recipe", text_property("A .character.json file.")},
                             {"id", text_property("The new entity's ID.")},
                             {"at", std::move(at)},
                             {"parent", text_property("An entity to place it under.")},
                             {"yaw", property("number", "3D: radians it is turned about y.")},
                             {"pixels_per_meter", std::move(ppm)},
                             {"flip_h", property("boolean", "2D: face left.")}},
                            {"recipe", "id", "at"}),
        .handler = character_place,
    });
}

} // namespace slash::command
