#include "slash/command/face.hpp"

#include "slash/canvas/canvas.hpp"
#include "slash/command/session.hpp"
#include "slash/core/math.hpp"
#include "slash/core/text.hpp"
#include "slash/platform/file.hpp"
#include "slash/procedural/texture.hpp"
#include "slash/render/image.hpp"
#include "slash/storyboard/storyboard.hpp"
#include "slash/world/character.hpp"
#include "slash/world/face.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <numbers>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// face.check, face.generate and face.portrait, over .face.json files and the faces of
// .character.json recipes (slash/world/face.hpp). Built with -ffp-contract=off, so measures and
// generated faces are the same everywhere.
namespace slash::command {
namespace {

// Faces whose surfaces are this far apart where they differ most, in percent of the head's
// size, tell two faces apart where no face_rules entry asks more.
constexpr real_t kDefaultShape = 0.5F;
constexpr float kPi = std::numbers::pi_v<float>;
constexpr std::string_view kRulesKind = "face_rules/";
// The rays a face's front is measured along: latitudes from under the chin to the forehead and
// longitudes from ear to ear.
constexpr std::size_t kLatitudes = 30;
constexpr std::size_t kLongitudes = 33;
constexpr float kLatitudeLow = -75.0F; // degrees
constexpr float kLatitudeSpan = 140.0F;
constexpr float kLongitudeLow = -80.0F;
constexpr float kLongitudeSpan = 160.0F;
// The rows the side profile's front is measured along, from under the chin to the crown.
constexpr std::size_t kProfileRows = 96;

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

double rounded(double value, double places = 1000.0) {
    return std::round(value * places) / places;
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

json::Value failure(const std::string& file, std::string path, std::string message) {
    json::Value item;
    item["file"] = file;
    item["path"] = std::move(path);
    item["message"] = std::move(message);
    return item;
}

// Project files as faces read them: a missing one is an error.
world::FileReader reader_of(Session& session) {
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

const json::Value* field(const json::Value& value, std::string_view name) {
    return value.is_object() ? value.find(name) : nullptr;
}

std::string text_of(const json::Value& value, std::string_view name) {
    const json::Value* found = field(value, name);
    return found != nullptr && found->is_string() ? found->as_string() : std::string();
}

// ---- Measuring a face ----

Vec3 cross(Vec3 a, Vec3 b) {
    return {(a.y * b.z) - (a.z * b.y), (a.z * b.x) - (a.x * b.z), (a.x * b.y) - (a.y * b.x)};
}

real_t dot(Vec3 a, Vec3 b) {
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

// The directions a face's front is measured along, in the head's own proportions (a unit
// sphere for an ellipsoid head): from 75 degrees under it to 65 above, 80 to either side.
const std::vector<Vec3>& ray_directions() {
    static const std::vector<Vec3> directions = [] {
        std::vector<Vec3> out;
        for (std::size_t i = 0; i < kLatitudes; ++i) {
            const float lat = (kLatitudeLow + (kLatitudeSpan * static_cast<float>(i) /
                                               static_cast<float>(kLatitudes - 1))) *
                              (kPi / 180.0F);
            for (std::size_t j = 0; j < kLongitudes; ++j) {
                const float lon = (kLongitudeLow + (kLongitudeSpan * static_cast<float>(j) /
                                                    static_cast<float>(kLongitudes - 1))) *
                                  (kPi / 180.0F);
                const Vec2f up = cos_sin(lat);
                const Vec2f round = cos_sin(lon);
                out.push_back({up.x * round.y, up.y, up.x * round.x});
            }
        }
        return out;
    }();
    return directions;
}

// What face.check and face.generate compare: how far the head's surface is from its center
// along each of ray_directions (in its half sizes, -1 where no skin is), and how far forward its
// side profile's front reaches on each row (in its half depth, -1 where it doesn't reach).
struct Measures {
    std::vector<real_t> depth;
    std::vector<real_t> profile;
};

struct Angles {
    float lon_low = 0.0F;
    float lon_high = 0.0F;
    float lat_low = 0.0F;
    float lat_high = 0.0F;
};

std::vector<real_t> surface_depths(const world::SculptedHead& sculpt, Vec3 radii) {
    std::vector<Vec3> places;
    places.reserve(sculpt.positions.size());
    for (const Vec3 p : sculpt.positions) {
        places.push_back({p.x / radii.x, p.y / radii.y, p.z / radii.z});
    }
    // Each skin triangle's span of longitudes and latitudes, to skip the rest quickly; those
    // round the back, which no ray reaches, are left out.
    struct Triangle {
        std::array<std::uint32_t, 3> corners;
        Angles span;
    };
    std::vector<Triangle> triangles;
    for (std::size_t t = 0; t * 3 + 2 < sculpt.indices.size(); ++t) {
        const world::FaceArea area = sculpt.areas.at(t);
        if (area == world::FaceArea::Mouth || area == world::FaceArea::Hidden) {
            continue;
        }
        Triangle triangle{
            .corners = {sculpt.indices[t * 3], sculpt.indices[(t * 3) + 1],
                        sculpt.indices[(t * 3) + 2]},
            .span = {.lon_low = 10.0F, .lon_high = -10.0F, .lat_low = 10.0F, .lat_high = -10.0F}};
        bool front = true;
        for (const std::uint32_t corner : triangle.corners) {
            const Vec3f p = to_float(places.at(corner));
            if (p.z < -0.2F) {
                front = false;
                break;
            }
            const float lon = slash::atan2(p.x, p.z);
            const float lat = slash::atan2(p.y, std::sqrt((p.x * p.x) + (p.z * p.z)));
            triangle.span.lon_low = std::min(triangle.span.lon_low, lon);
            triangle.span.lon_high = std::max(triangle.span.lon_high, lon);
            triangle.span.lat_low = std::min(triangle.span.lat_low, lat);
            triangle.span.lat_high = std::max(triangle.span.lat_high, lat);
        }
        if (front) {
            triangles.push_back(triangle);
        }
    }
    constexpr float kMargin = 0.02F;
    std::vector<real_t> out;
    out.reserve(ray_directions().size());
    for (std::size_t r = 0; r < ray_directions().size(); ++r) {
        const Vec3 d = ray_directions()[r];
        const float lon = static_cast<float>(r % kLongitudes) / static_cast<float>(kLongitudes - 1);
        const std::size_t row = r / kLongitudes;
        const float lat = static_cast<float>(row) / static_cast<float>(kLatitudes - 1);
        const float ray_lon = ((kLongitudeLow + (kLongitudeSpan * lon)) * kPi) / 180.0F;
        const float ray_lat = ((kLatitudeLow + (kLatitudeSpan * lat)) * kPi) / 180.0F;
        real_t nearest = -1.0F;
        for (const Triangle& triangle : triangles) {
            if (ray_lon < triangle.span.lon_low - kMargin ||
                ray_lon > triangle.span.lon_high + kMargin ||
                ray_lat < triangle.span.lat_low - kMargin ||
                ray_lat > triangle.span.lat_high + kMargin) {
                continue;
            }
            const Vec3 a = places[triangle.corners[0]];
            const Vec3 e1 = places[triangle.corners[1]] - a;
            const Vec3 e2 = places[triangle.corners[2]] - a;
            const Vec3 p = cross(d, e2);
            const real_t det = dot(e1, p);
            if (std::abs(det) < 1e-9F) {
                continue;
            }
            const Vec3 s{-a.x, -a.y, -a.z};
            const real_t u = dot(s, p) / det;
            if (u < 0.0F || u > 1.0F) {
                continue;
            }
            const Vec3 q = cross(s, e1);
            const real_t v = dot(d, q) / det;
            if (v < 0.0F || u + v > 1.0F) {
                continue;
            }
            const real_t t = dot(e2, q) / det;
            if (t > 0.0F && (nearest < 0.0F || t < nearest)) {
                nearest = t;
            }
        }
        out.push_back(nearest);
    }
    return out;
}

// How far forward the profile reaches on each row: its outline's furthest crossing.
std::vector<real_t> profile_front(const world::FaceProfile& profile, Vec3 radii) {
    std::vector<real_t> out(kProfileRows, -1.0F);
    const std::vector<Vec2>& points = profile.points;
    for (std::size_t row = 0; row < kProfileRows; ++row) {
        const real_t y = radii.y * (-1.6F + (3.0F * (static_cast<real_t>(row) + 0.5F) /
                                             static_cast<real_t>(kProfileRows)));
        for (std::size_t i = 0, j = points.size() - 1; i < points.size(); j = i++) {
            const Vec2 a = points[i];
            const Vec2 b = points[j];
            if ((a.y > y) != (b.y > y)) {
                const real_t x = a.x + ((b.x - a.x) * (y - a.y) / (b.y - a.y));
                out[row] = std::max(out[row], x / radii.z);
            }
        }
    }
    return out;
}

Measures measure(const world::CharacterFace& face) {
    const world::HeadShape head;
    Measures out;
    out.depth = surface_depths(world::sculpt_head(face, head), head.radii);
    const world::FaceProfile profile = world::face_profile(face, head);
    if (profile.points.size() >= 3) {
        out.profile = profile_front(profile, head.radii);
    }
    return out;
}

struct Apart {
    real_t shape = 0.0F;   // percent of the head's size
    real_t profile = 0.0F; // percent of the head's depth
};

// Where two measures differ most: the mean of the largest tenth of their differences, in
// percent, so a nose or a chin counts as much as a whole face's width. Places only one reaches
// are left out.
real_t most_apart(const std::vector<real_t>& a, const std::vector<real_t>& b) {
    std::vector<real_t> differences;
    for (std::size_t i = 0; i < a.size() && i < b.size(); ++i) {
        if (a[i] >= 0.0F && b[i] >= 0.0F) {
            differences.push_back(std::abs(a[i] - b[i]));
        }
    }
    if (differences.empty()) {
        return 0.0F;
    }
    std::ranges::sort(differences, std::greater<>());
    const std::size_t top = std::max<std::size_t>(1, differences.size() / 10);
    real_t sum = 0.0F;
    for (std::size_t i = 0; i < top; ++i) {
        sum += differences[i];
    }
    return 100.0F * sum / static_cast<real_t>(top);
}

Apart apart(const Measures& a, const Measures& b) {
    return {.shape = most_apart(a.depth, b.depth), .profile = most_apart(a.profile, b.profile)};
}

// ---- The storyboard's face rules ----

struct Rules {
    std::string id;
    // A copy: the storyboard it was read from is gone once the rules are loaded.
    json::Value entry;
    real_t shape = kDefaultShape;
    real_t profile = 0.0F;
    std::vector<std::string> styles;
    std::optional<std::vector<std::string>> marks;
    std::optional<std::int64_t> max_marks;
    std::map<std::string, std::pair<float, float>, std::less<>> controls;
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

std::vector<std::string> texts_of(const json::Value& entry, std::string_view name) {
    std::vector<std::string> out;
    if (const json::Value* list = entry.find(name); list != nullptr && list->is_array()) {
        for (const json::Value& item : list->as_array()) {
            if (item.is_string()) {
                out.push_back(item.as_string());
            }
        }
    }
    return out;
}

struct Storyboard {
    std::vector<Rules> rules;
    std::vector<json::Value> problems; // in the rules themselves
};

Result<Storyboard> load_rules(Session& session) {
    auto loaded = storyboard::load(
        [&session](std::string_view path) { return session.read_project_file(path); });
    if (!loaded) {
        return loaded.error();
    }
    Storyboard out;
    const json::Object& entries = loaded.value().entries;
    std::vector<std::string_view> names;
    for (const world::FaceControl& control : world::face_controls()) {
        names.push_back(control.name);
    }
    for (const auto& [id, entry] : entries) {
        if (!std::string_view(id).starts_with(kRulesKind)) {
            continue;
        }
        Rules rules;
        rules.id = id;
        rules.entry = entry;
        if (const json::Value* value = entry.find("shape_difference");
            value != nullptr && value->is_number()) {
            rules.shape = static_cast<real_t>(value->as_double());
        }
        if (const json::Value* value = entry.find("profile_difference");
            value != nullptr && value->is_number()) {
            rules.profile = static_cast<real_t>(value->as_double());
        }
        rules.styles = texts_of(entry, "styles");
        if (entry.find("marks") != nullptr) {
            rules.marks = texts_of(entry, "marks");
        }
        if (const json::Value* most = entry.find("max_marks"); most != nullptr && most->is_int()) {
            rules.max_marks = most->as_int();
        }
        if (const json::Value* controls = entry.find("controls");
            controls != nullptr && controls->is_object()) {
            for (const auto& [name, range] : controls->as_object()) {
                if (!world::face_control_index(name)) {
                    std::string message = quoted(name) + " isn't a sculpt control";
                    if (const std::string_view hint = closest_match(name, names); !hint.empty()) {
                        message += "; did you mean " + quoted(hint) + "?";
                    }
                    std::string pointer = "/entries/";
                    pointer += id;
                    pointer += "/controls/";
                    pointer += name;
                    out.problems.push_back(
                        failure("storyboard/design.json", pointer, std::move(message)));
                    continue;
                }
                if (range.is_array() && range.as_array().size() == 2 &&
                    range.as_array()[0].is_number() && range.as_array()[1].is_number()) {
                    rules.controls[name] = {static_cast<float>(range.as_array()[0].as_double()),
                                            static_cast<float>(range.as_array()[1].as_double())};
                }
            }
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
        out.rules.push_back(std::move(rules));
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

bool in_palette(const Rules& rules, Vec4 color) {
    const auto byte = [](float v) {
        return static_cast<int>(std::lround(std::clamp(v, 0.0F, 1.0F) * 255.0F));
    };
    const std::array<int, 3> rgb{byte(color.x), byte(color.y), byte(color.z)};
    return std::ranges::any_of(rules.palette, [&](const std::array<int, 3>& allowed) {
        return std::abs(allowed[0] - rgb[0]) <= rules.tolerance &&
               std::abs(allowed[1] - rgb[1]) <= rules.tolerance &&
               std::abs(allowed[2] - rgb[2]) <= rules.tolerance;
    });
}

// ---- The cast ----

// A face in the project: a .face.json file, or a character recipe's face. pointer is where the
// face's fields are in its file ("" for a face file, "/face" for a recipe).
struct Face {
    std::string path;
    std::string pointer;
    world::CharacterFace face;
    Measures measures;
};

// Every .face.json and .character.json file in the project, build/, .slash/ and storyboard/
// aside.
Result<std::vector<std::string>> face_files(Session& session) {
    auto listed = platform::list_files(session.options().project_root, [](std::string_view folder) {
        return folder != "build/" && folder != "storyboard/" && folder != ".slash/";
    });
    if (!listed) {
        return listed.error();
    }
    std::vector<std::string> out;
    for (std::string& path : listed.value()) {
        if (world::is_face_path(path) || world::is_character_path(path)) {
            out.push_back(std::move(path));
        }
    }
    return out;
}

// The faces a face's fields name: what it extends or comes from and what it blends.
void named_faces(const json::Value& face, std::set<std::string, std::less<>>& named) {
    for (const std::string_view key : {"extends", "from"}) {
        if (const std::string name = text_of(face, key); !name.empty()) {
            named.insert(name);
        }
    }
    if (const json::Value* blend = field(face, "blend"); blend != nullptr && blend->is_array()) {
        for (const json::Value& item : blend->as_array()) {
            if (const std::string name = text_of(item, "face"); !name.empty()) {
                named.insert(name);
            }
        }
    }
}

// Whether a recipe's face is designed (from a face file, or with controls, carves or blends of
// its own), rather than a generated character's few sizes, which character.check tells apart.
bool designed(const json::Value& face) {
    return std::ranges::any_of(
        std::array<std::string_view, 5>{"from", "controls", "carves", "blend", "style"},
        [&](std::string_view key) { return field(face, key) != nullptr; });
}

struct Cast {
    std::vector<Face> faces;
    std::vector<json::Value> problems; // of face files that don't read
};

// The project's faces: every designed one but the presets other faces name (whose fields those
// faces take), so a character made from a face file isn't measured against it.
Cast read_cast(Session& session, const std::vector<std::string>& files, bool with_presets) {
    const world::FileReader read = reader_of(session);
    struct Read {
        std::string path;
        std::string pointer;
        json::Value doc;
    };
    std::vector<Read> found;
    std::set<std::string, std::less<>> named;
    Cast out;
    for (const std::string& path : files) {
        auto text = read(path);
        if (!text) {
            continue;
        }
        auto doc = json::parse(text.value());
        if (!doc) {
            if (world::is_face_path(path)) {
                out.problems.push_back(failure(path, "", doc.error().message));
            }
            continue;
        }
        if (world::is_face_path(path)) {
            for (const json::ValidationError& problem : world::validate_face(doc.value())) {
                out.problems.push_back(failure(path, problem.path, problem.message));
            }
            named_faces(doc.value(), named);
            found.push_back({.path = path, .pointer = "", .doc = std::move(doc).value()});
            continue;
        }
        // A recipe: its face as its extends chain leaves it.
        auto resolved = world::resolve_character(doc.value(), read);
        if (!resolved) {
            continue;
        }
        const json::Value* face = field(resolved.value(), "face");
        if (face == nullptr || !designed(*face)) {
            continue;
        }
        named_faces(*face, named);
        found.push_back({.path = path, .pointer = "/face", .doc = std::move(doc).value()});
    }
    for (Read& item : found) {
        if (!with_presets && item.pointer.empty() && named.contains(item.path)) {
            continue;
        }
        world::CharacterFace face;
        if (item.pointer.empty()) {
            auto parsed = world::parse_face(item.doc, read);
            if (!parsed) {
                out.problems.push_back(failure(item.path, "", parsed.error().message));
                continue;
            }
            face = std::move(parsed).value();
        } else {
            auto recipe = world::parse_character(item.doc, read);
            if (!recipe) {
                continue; // character.check reports it
            }
            face = std::move(recipe.value().face);
        }
        Face entry{.path = std::move(item.path),
                   .pointer = std::move(item.pointer),
                   .face = std::move(face),
                   .measures = {}};
        entry.measures = measure(entry.face);
        out.faces.push_back(std::move(entry));
    }
    return out;
}

// ---- face.check ----

void check_rules(const Face& face, const Rules& rules, std::vector<json::Value>& errors) {
    const world::CharacterFace& f = face.face;
    if (!rules.styles.empty() && std::ranges::find(rules.styles, f.style) == rules.styles.end()) {
        std::string allowed;
        for (const std::string& style : rules.styles) {
            allowed += (allowed.empty() ? "" : ", ") + style;
        }
        errors.push_back(failure(face.path, face.pointer + "/style",
                                 "is " + f.style + "; " + rules.id + " asks " + allowed));
    }
    for (const auto& [name, range] : rules.controls) {
        const float value = f.control(name);
        if (value < range.first - 1e-4F || value > range.second + 1e-4F) {
            errors.push_back(failure(face.path, face.pointer + "/controls/" + name,
                                     "is " + number_text(value) + "; " + rules.id + " keeps it " +
                                         number_text(range.first) + " to " +
                                         number_text(range.second)));
        }
    }
    if (rules.max_marks && static_cast<std::int64_t>(f.marks.size()) > *rules.max_marks) {
        errors.push_back(failure(face.path, face.pointer + "/marks",
                                 std::to_string(f.marks.size()) + " marks; " + rules.id +
                                     " allows " + std::to_string(*rules.max_marks)));
    }
    for (std::size_t i = 0; i < f.marks.size(); ++i) {
        if (rules.marks && std::ranges::find(*rules.marks, f.marks[i].kind) == rules.marks->end()) {
            errors.push_back(failure(
                face.path, face.pointer + "/marks/" + std::to_string(i) + "/kind",
                quoted(f.marks[i].kind) + " marks aren't among those " + rules.id + " allows"));
        }
    }
    if (rules.palette.empty()) {
        return;
    }
    const auto check = [&](Vec4 color, std::string pointer) {
        if (!in_palette(rules, color)) {
            errors.push_back(failure(face.path, face.pointer + std::move(pointer),
                                     "isn't a color of " + rules.palette_id + ", which " +
                                         rules.id + " keeps faces to"));
        }
    };
    check(f.eyes, "/eyes");
    check(f.lips, "/lips");
    if (f.brows.color.w > 0.0F) {
        check(f.brows.color, "/brows/color");
    }
    for (std::size_t i = 0; i < f.marks.size(); ++i) {
        if (f.marks[i].color.w > 0.0F) {
            check(f.marks[i].color, "/marks/" + std::to_string(i) + "/color");
        }
    }
}

// What the rules covering a path ask: the most of each, and the rule that asks it.
struct Asked {
    real_t shape = kDefaultShape;
    real_t profile = 0.0F;
    std::string by = "the default";
};

Asked asked(const std::vector<const Rules*>& covering) {
    Asked out;
    for (const Rules* rule : covering) {
        if (rule->shape >= out.shape || rule->profile > out.profile) {
            out.by = rule->id;
        }
        out.shape = std::max(out.shape, rule->shape);
        out.profile = std::max(out.profile, rule->profile);
    }
    return out;
}

Reply face_check(Session& session, const json::Value& params) {
    auto files = face_files(session);
    if (!files) {
        return Reply::failure(files.error());
    }
    std::vector<std::string> checked;
    if (const json::Value* paths = params.find("paths"); paths != nullptr) {
        for (const json::Value& path : paths->as_array()) {
            const std::string& name = path.as_string();
            if (!world::is_face_path(name) && !world::is_character_path(name)) {
                return param_error(code::kInvalidParams, "/paths",
                                   quoted(name) + " is neither a .face.json nor a "
                                                  ".character.json file");
            }
            checked.push_back(name);
            if (std::ranges::find(files.value(), name) == files.value().end()) {
                files.value().push_back(name);
            }
        }
    }
    const bool all = checked.empty();
    const auto mine = [&](const std::string& path) {
        return all || std::ranges::find(checked, path) != checked.end();
    };
    auto rules = load_rules(session);
    if (!rules) {
        return Reply::failure(rules.error());
    }
    Cast cast = read_cast(session, files.value(), false);
    std::vector<json::Value> errors;
    if (all) {
        errors = rules.value().problems;
    }
    for (json::Value& problem : cast.problems) {
        if (mine(problem.find("file")->as_string())) {
            errors.push_back(std::move(problem));
        }
    }
    const bool measures = params.find("measures") != nullptr && params.find("measures")->as_bool();
    json::Array pairs;
    std::vector<Face>& faces = cast.faces;
    for (std::size_t a = 0; a < faces.size(); ++a) {
        if (!mine(faces[a].path)) {
            continue;
        }
        const std::vector<const Rules*> covering = rules_for(rules.value().rules, faces[a].path);
        for (const Rules* rule : covering) {
            check_rules(faces[a], *rule, errors);
        }
        const Asked asks = asked(covering);
        for (std::size_t b = 0; b < faces.size(); ++b) {
            // Each pair once: on the checked face, the later one when both are.
            if (a == b || (mine(faces[b].path) && faces[b].path < faces[a].path)) {
                continue;
            }
            const Apart measured = apart(faces[a].measures, faces[b].measures);
            if (measures) {
                json::Value pair;
                pair["a"] = faces[a].path;
                pair["b"] = faces[b].path;
                pair["shape"] = rounded(measured.shape, 100.0);
                pair["profile"] = rounded(measured.profile, 100.0);
                pairs.push_back(std::move(pair));
            }
            if (measured.shape < asks.shape) {
                errors.push_back(failure(
                    faces[a].path, faces[a].pointer,
                    "its face is too like " + faces[b].path + "'s: " + number_text(measured.shape) +
                        "% apart, and " + asks.by + " asks " + number_text(asks.shape) +
                        "%; change its sculpt controls or carves"));
            }
            if (asks.profile > 0.0F && measured.profile < asks.profile) {
                errors.push_back(
                    failure(faces[a].path, faces[a].pointer,
                            "its profile is too like " + faces[b].path +
                                "'s: " + number_text(measured.profile) + "% apart, and " + asks.by +
                                " asks " + number_text(asks.profile) + "%" +
                                "; change its nose, brow, lips or chin, which the side shows"));
            }
        }
    }
    json::Value result;
    result["faces"] = static_cast<std::int64_t>(faces.size());
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

// ---- face.generate ----

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
};

std::uint64_t hash_text(std::string_view text) {
    std::uint64_t h = 1469598103934665603ULL;
    for (const char c : text) {
        h = (h ^ static_cast<unsigned char>(c)) * 1099511628211ULL;
    }
    return h;
}

// What a brief asks of a face: control values it leans toward (those it names fixed more
// tightly), and the rest of the face.
struct Wish {
    std::map<std::string, float, std::less<>> controls;
    std::optional<float> wrinkles;
    std::optional<float> blotches;
    std::optional<std::string> beard;
    std::optional<std::string> brows;
    std::optional<std::string> style;
    std::vector<std::string> marks;
    std::optional<float> asymmetry;
    std::vector<std::string> understood;
    std::vector<std::string> ignored;

    void lean(std::string_view control, float value) {
        auto [it, added] = controls.emplace(std::string(control), value);
        if (!added) {
            it->second = std::clamp(it->second + value, -1.0F, 1.0F);
        }
    }
};

struct Lean {
    std::string_view control;
    float value;
};

// A word that says something of the whole face, and what it leans the face toward.
struct Trait {
    std::string_view word;
    std::array<Lean, 6> leans;
    float wrinkles = -1.0F;
};

// The traits a brief may name. Unused leans have an empty control.
const std::vector<Trait>& traits() {
    static const std::vector<Trait> list = {
        {.word = "old",
         .leans = {{{"cheeks.hollow", 0.35F},
                    {"eyes.hooded", 0.5F},
                    {"ears.size", 0.25F},
                    {"nose.length", 0.25F},
                    {"lips.upper", -0.3F},
                    {"mouth.corners", -0.2F}}},
         .wrinkles = 0.75F},
        {.word = "young",
         .leans = {{{"cheeks.fullness", 0.4F},
                    {"eyes.size", 0.3F},
                    {"nose.length", -0.3F},
                    {"chin.length", -0.2F},
                    {"jaw.angle", -0.2F},
                    {"", 0.0F}}},
         .wrinkles = 0.0F},
        {.word = "child",
         .leans = {{{"cheeks.fullness", 0.7F},
                    {"eyes.size", 0.6F},
                    {"nose.length", -0.6F},
                    {"chin.length", -0.5F},
                    {"face.length", -0.5F},
                    {"jaw.width", -0.3F}}},
         .wrinkles = 0.0F},
        {.word = "stern",
         .leans = {{{"brow.ridge", 0.45F},
                    {"brow.height", -0.35F},
                    {"mouth.corners", -0.45F},
                    {"jaw.angle", 0.25F},
                    {"lips.upper", -0.2F},
                    {"", 0.0F}}}},
        {.word = "kind",
         .leans = {{{"mouth.corners", 0.45F},
                    {"cheeks.fullness", 0.25F},
                    {"eyes.openness", 0.2F},
                    {"brow.height", 0.15F},
                    {"jaw.angle", -0.2F},
                    {"", 0.0F}}}},
        {.word = "sly",
         .leans = {{{"eyes.tilt", 0.45F},
                    {"eyes.openness", -0.35F},
                    {"mouth.corners", 0.2F},
                    {"chin.length", 0.25F},
                    {"brow.tilt", 0.4F},
                    {"", 0.0F}}}},
        {.word = "tired",
         .leans = {{{"eyes.hooded", 0.55F},
                    {"eyes.openness", -0.35F},
                    {"mouth.corners", -0.25F},
                    {"cheeks.hollow", 0.2F},
                    {"", 0.0F},
                    {"", 0.0F}}},
         .wrinkles = 0.3F},
        {.word = "tough",
         .leans = {{{"jaw.width", 0.5F},
                    {"jaw.angle", 0.55F},
                    {"brow.ridge", 0.5F},
                    {"nose.width", 0.3F},
                    {"neck.width", 0.45F},
                    {"chin.width", 0.3F}}}},
        {.word = "noble",
         .leans = {{{"cheeks.bones", 0.5F},
                    {"nose.bridge", 0.45F},
                    {"jaw.angle", 0.2F},
                    {"face.length", 0.25F},
                    {"chin.protrusion", 0.2F},
                    {"", 0.0F}}}},
        {.word = "gaunt",
         .leans = {{{"cheeks.hollow", 0.65F},
                    {"cheeks.fullness", -0.5F},
                    {"face.width", -0.3F},
                    {"cheeks.bones", 0.35F},
                    {"eyes.depth", 0.35F},
                    {"", 0.0F}}}},
        {.word = "round",
         .leans = {{{"cheeks.fullness", 0.7F},
                    {"face.width", 0.45F},
                    {"jaw.angle", -0.45F},
                    {"chin.length", -0.3F},
                    {"neck.width", 0.3F},
                    {"", 0.0F}}}},
        {.word = "weathered",
         .leans = {{{"eyes.hooded", 0.25F},
                    {"cheeks.hollow", 0.2F},
                    {"", 0.0F},
                    {"", 0.0F},
                    {"", 0.0F},
                    {"", 0.0F}}},
         .wrinkles = 0.45F},
    };
    return list;
}

// Other words for the traits.
const std::map<std::string_view, std::string_view>& trait_words() {
    static const std::map<std::string_view, std::string_view> words = {
        {"old", "old"},       {"elderly", "old"},         {"aged", "old"},
        {"ancient", "old"},   {"grizzled", "old"},        {"elder", "old"},
        {"young", "young"},   {"youthful", "young"},      {"teen", "young"},
        {"teenage", "young"}, {"boyish", "young"},        {"girlish", "young"},
        {"child", "child"},   {"kid", "child"},           {"childlike", "child"},
        {"stern", "stern"},   {"severe", "stern"},        {"grim", "stern"},
        {"serious", "stern"}, {"harsh", "stern"},         {"scowling", "stern"},
        {"kind", "kind"},     {"gentle", "kind"},         {"friendly", "kind"},
        {"warm", "kind"},     {"cheerful", "kind"},       {"jolly", "kind"},
        {"sly", "sly"},       {"cunning", "sly"},         {"shifty", "sly"},
        {"sneaky", "sly"},    {"tired", "tired"},         {"weary", "tired"},
        {"sleepy", "tired"},  {"tough", "tough"},         {"rugged", "tough"},
        {"brutish", "tough"}, {"brawny", "tough"},        {"warrior", "tough"},
        {"soldier", "tough"}, {"fighter", "tough"},       {"noble", "noble"},
        {"regal", "noble"},   {"aristocratic", "noble"},  {"elegant", "noble"},
        {"gaunt", "gaunt"},   {"haggard", "gaunt"},       {"skinny", "gaunt"},
        {"round", "round"},   {"chubby", "round"},        {"plump", "round"},
        {"portly", "round"},  {"weathered", "weathered"}, {"wrinkled", "weathered"},
    };
    return words;
}

// How a modifier changes the feature after it: by its size, its width, or another way.
enum class Change : std::uint8_t {
    Bigger,
    Smaller,
    Wider,
    Narrower,
    Higher,
    Lower,
    Sharper,
    Softer,
    Fuller,
    Thinner,
    Deeper
};

const std::map<std::string_view, Change>& modifiers() {
    static const std::map<std::string_view, Change> words = {
        {"big", Change::Bigger},       {"large", Change::Bigger},   {"long", Change::Bigger},
        {"prominent", Change::Bigger}, {"huge", Change::Bigger},    {"small", Change::Smaller},
        {"little", Change::Smaller},   {"tiny", Change::Smaller},   {"short", Change::Smaller},
        {"wide", Change::Wider},       {"broad", Change::Wider},    {"narrow", Change::Narrower},
        {"high", Change::Higher},      {"low", Change::Lower},      {"sharp", Change::Sharper},
        {"angular", Change::Sharper},  {"strong", Change::Sharper}, {"square", Change::Sharper},
        {"defined", Change::Sharper},  {"heavy", Change::Sharper},  {"soft", Change::Softer},
        {"weak", Change::Softer},      {"rounded", Change::Softer}, {"full", Change::Fuller},
        {"plump", Change::Fuller},     {"thick", Change::Fuller},   {"thin", Change::Thinner},
        {"deep", Change::Deeper},      {"sunken", Change::Deeper},  {"hollow", Change::Deeper},
    };
    return words;
}

// What a modifier does to a feature: the controls it leans, or none where it says nothing.
std::vector<Lean> feature_leans(std::string_view feature, Change change) {
    const float sign =
        (change == Change::Smaller || change == Change::Narrower || change == Change::Lower ||
         change == Change::Softer || change == Change::Thinner)
            ? -1.0F
            : 1.0F;
    const bool size = change == Change::Bigger || change == Change::Smaller;
    const bool width = change == Change::Wider || change == Change::Narrower;
    if (feature == "nose") {
        if (width) {
            return {{"nose.width", 0.6F * sign}};
        }
        if (change == Change::Higher || change == Change::Sharper) {
            return {{"nose.bridge", 0.6F}};
        }
        return {{"nose.length", 0.6F * sign}, {"nose.tip", 0.3F * sign}};
    }
    if (feature == "eyes") {
        if (width) {
            return {{"eyes.spacing", 0.6F * sign}};
        }
        if (change == Change::Deeper) {
            return {{"eyes.depth", 0.7F}};
        }
        if (change == Change::Higher || change == Change::Lower) {
            return {{"eyes.height", 0.5F * sign}};
        }
        return {{"eyes.size", 0.6F * sign}, {"eyes.openness", 0.3F * sign}};
    }
    if (feature == "lips") {
        return {{"lips.upper", 0.6F * sign}, {"lips.lower", 0.6F * sign}};
    }
    if (feature == "mouth") {
        if (change == Change::Higher || change == Change::Lower) {
            return {{"mouth.height", 0.5F * sign}};
        }
        return {{"mouth.width", 0.6F * sign}};
    }
    if (feature == "jaw") {
        if (size || width) {
            return {{"jaw.width", 0.6F * sign}};
        }
        return {{"jaw.width", 0.4F * sign}, {"jaw.angle", 0.6F * sign}};
    }
    if (feature == "chin") {
        if (width) {
            return {{"chin.width", 0.6F * sign}};
        }
        if (change == Change::Sharper || change == Change::Softer) {
            return {{"chin.protrusion", 0.6F * sign}};
        }
        return {{"chin.length", 0.6F * sign}};
    }
    if (feature == "cheekbones") {
        return {{"cheeks.bones", 0.6F * sign}};
    }
    if (feature == "cheeks") {
        if (change == Change::Deeper) {
            return {{"cheeks.hollow", 0.7F}};
        }
        return {{"cheeks.fullness", 0.6F * sign}};
    }
    if (feature == "brow") {
        if (change == Change::Higher || change == Change::Lower) {
            return {{"brow.height", 0.6F * sign}};
        }
        return {{"brow.ridge", 0.6F * sign}};
    }
    if (feature == "forehead") {
        return {{"forehead.height", 0.6F * sign}};
    }
    if (feature == "ears") {
        return {{"ears.size", 0.6F * sign}};
    }
    if (feature == "face") {
        if (width) {
            return {{"face.width", 0.6F * sign}};
        }
        return {{"face.length", 0.6F * sign}};
    }
    if (feature == "head") {
        return {{"head.width", 0.4F * sign}, {"head.height", 0.4F * sign}};
    }
    if (feature == "neck") {
        return {{"neck.width", 0.6F * sign}};
    }
    return {};
}

const std::map<std::string_view, std::string_view>& feature_words() {
    static const std::map<std::string_view, std::string_view> words = {
        {"nose", "nose"},     {"eyes", "eyes"},
        {"eye", "eyes"},      {"lips", "lips"},
        {"lip", "lips"},      {"mouth", "mouth"},
        {"jaw", "jaw"},       {"jawline", "jaw"},
        {"chin", "chin"},     {"cheekbones", "cheekbones"},
        {"cheeks", "cheeks"}, {"brow", "brow"},
        {"brows", "brow"},    {"forehead", "forehead"},
        {"ears", "ears"},     {"face", "face"},
        {"head", "head"},     {"neck", "neck"},
    };
    return words;
}

// Words that name a whole feature's shape by themselves.
const std::map<std::string_view, std::vector<Lean>>& shape_words() {
    static const std::map<std::string_view, std::vector<Lean>> words = {
        {"hooked", {{"nose.hook", 0.75F}, {"nose.length", 0.3F}}},
        {"aquiline", {{"nose.hook", 0.6F}, {"nose.bridge", 0.5F}}},
        {"roman", {{"nose.hook", 0.5F}, {"nose.bridge", 0.6F}}},
        {"button", {{"nose.upturn", 0.6F}, {"nose.length", -0.45F}, {"nose.tip", 0.3F}}},
        {"upturned", {{"nose.upturn", 0.7F}}},
        {"snub", {{"nose.upturn", 0.6F}, {"nose.length", -0.4F}}},
        {"cleft", {{"chin.cleft", 0.8F}}},
        {"dimpled", {{"chin.cleft", 0.5F}}},
        {"hooded", {{"eyes.hooded", 0.75F}}},
        {"almond", {{"eyes.tilt", 0.4F}, {"eyes.openness", -0.2F}}},
        {"protruding", {{"ears.angle", 0.7F}}},
        {"pointed", {{"chin.width", -0.5F}, {"chin.length", 0.3F}}},
        {"bulbous", {{"nose.tip", 0.8F}, {"nose.width", 0.3F}}},
        {"flat", {{"nose.bridge", -0.6F}, {"nose.width", 0.3F}}},
        {"crooked", {{"nose.hook", 0.3F}}},
        {"smiling", {{"mouth.corners", 0.6F}}},
        {"frowning", {{"mouth.corners", -0.6F}}},
        {"wide-set", {{"eyes.spacing", 0.6F}}},
        {"close-set", {{"eyes.spacing", -0.6F}}},
        {"deep-set", {{"eyes.depth", 0.7F}}},
        {"high-cheekboned", {{"cheeks.bones", 0.6F}}},
        {"square-jawed", {{"jaw.width", 0.5F}, {"jaw.angle", 0.6F}}},
        {"lantern-jawed", {{"jaw.width", 0.4F}, {"chin.length", 0.6F}}},
    };
    return words;
}

// Marks, beards, brows and styles a brief may name.
struct Extra {
    std::string_view word;
    std::string_view mark;
    std::string_view beard;
    std::string_view brows;
    std::string_view style;
};

const std::vector<Extra>& extras() {
    static const std::vector<Extra> list = {
        {.word = "scar", .mark = "scar", .beard = "", .brows = "", .style = ""},
        {.word = "scarred", .mark = "scar", .beard = "", .brows = "", .style = ""},
        {.word = "freckles", .mark = "freckles", .beard = "", .brows = "", .style = ""},
        {.word = "freckled", .mark = "freckles", .beard = "", .brows = "", .style = ""},
        {.word = "mole", .mark = "mole", .beard = "", .brows = "", .style = ""},
        {.word = "tattoo", .mark = "tattoo", .beard = "", .brows = "", .style = ""},
        {.word = "tattooed", .mark = "tattoo", .beard = "", .brows = "", .style = ""},
        {.word = "warpaint", .mark = "paint", .beard = "", .brows = "", .style = ""},
        {.word = "painted", .mark = "paint", .beard = "", .brows = "", .style = ""},
        {.word = "burn", .mark = "burn", .beard = "", .brows = "", .style = ""},
        {.word = "burned", .mark = "burn", .beard = "", .brows = "", .style = ""},
        {.word = "bruise", .mark = "bruise", .beard = "", .brows = "", .style = ""},
        {.word = "bruised", .mark = "bruise", .beard = "", .brows = "", .style = ""},
        {.word = "wound", .mark = "wound", .beard = "", .brows = "", .style = ""},
        {.word = "wounded", .mark = "wound", .beard = "", .brows = "", .style = ""},
        {.word = "birthmark", .mark = "birthmark", .beard = "", .brows = "", .style = ""},
        {.word = "rash", .mark = "rash", .beard = "", .brows = "", .style = ""},
        {.word = "beard", .mark = "", .beard = "short", .brows = "", .style = ""},
        {.word = "bearded", .mark = "", .beard = "short", .brows = "", .style = ""},
        {.word = "stubble", .mark = "", .beard = "stubble", .brows = "", .style = ""},
        {.word = "stubbled", .mark = "", .beard = "stubble", .brows = "", .style = ""},
        {.word = "unshaven", .mark = "", .beard = "stubble", .brows = "", .style = ""},
        {.word = "clean-shaven", .mark = "", .beard = "none", .brows = "", .style = ""},
        {.word = "beardless", .mark = "", .beard = "none", .brows = "", .style = ""},
        {.word = "arched", .mark = "", .beard = "", .brows = "arched", .style = ""},
        {.word = "bushy", .mark = "", .beard = "", .brows = "straight", .style = ""},
        {.word = "toon", .mark = "", .beard = "", .brows = "", .style = "toon"},
        {.word = "cartoon", .mark = "", .beard = "", .brows = "", .style = "toon"},
        {.word = "cartoony", .mark = "", .beard = "", .brows = "", .style = "toon"},
        {.word = "stylized", .mark = "", .beard = "", .brows = "", .style = "stylized"},
        {.word = "painterly", .mark = "", .beard = "", .brows = "", .style = "painterly"},
        {.word = "low-poly", .mark = "", .beard = "", .brows = "", .style = "low_poly"},
        {.word = "realistic", .mark = "", .beard = "", .brows = "", .style = "realistic"},
    };
    return list;
}

// Words a brief may hold that say nothing of a face.
bool filler(std::string_view word) {
    static constexpr std::array<std::string_view, 32> kWords{
        "a",     "an",     "the",      "and",      "with",   "of",   "in",      "on",
        "her",   "his",    "their",    "its",      "who",    "is",   "has",     "very",
        "quite", "rather", "slightly", "somewhat", "but",    "to",   "from",    "by",
        "for",   "or",     "man",      "woman",    "person", "face", "looking", "one"};
    return std::ranges::find(kWords, word) != kWords.end();
}

// A brief, read as words: traits, modifiers before features, shapes, marks, beards and styles.
Wish read_brief(std::string_view brief) {
    std::vector<std::string> words;
    std::string word;
    const auto flush = [&] {
        while (!word.empty() && word.back() == '-') {
            word.pop_back();
        }
        if (!word.empty()) {
            words.push_back(std::move(word));
        }
        word.clear();
    };
    for (const char c : brief) {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || (c == '-' && !word.empty())) {
            word += c;
        } else if (c >= 'A' && c <= 'Z') {
            word += static_cast<char>(c - 'A' + 'a');
        } else {
            flush();
        }
    }
    flush();
    // "low poly" and "clean shaven" as one word.
    for (std::size_t i = 0; i + 1 < words.size(); ++i) {
        if ((words[i] == "low" && words[i + 1] == "poly") ||
            (words[i] == "clean" && words[i + 1] == "shaven")) {
            words[i] += "-" + words[i + 1];
            words.erase(words.begin() + static_cast<std::ptrdiff_t>(i) + 1);
        }
    }
    Wish wish;
    std::optional<Change> pending;
    std::string pending_word;
    for (const std::string& w : words) {
        if (const auto feature = feature_words().find(w); feature != feature_words().end()) {
            if (pending) {
                for (const Lean& lean : feature_leans(feature->second, *pending)) {
                    wish.lean(lean.control, lean.value);
                }
                std::string phrase = pending_word;
                phrase += " ";
                phrase += w;
                wish.understood.push_back(std::move(phrase));
                pending.reset();
            }
            continue;
        }
        if (const auto modifier = modifiers().find(w); modifier != modifiers().end()) {
            if (pending) {
                wish.ignored.push_back(pending_word);
            }
            pending = modifier->second;
            pending_word = w;
            // "round" and "plump" are traits of the whole face unless a feature follows.
            continue;
        }
        if (pending) {
            const auto trait = trait_words().find(pending_word);
            if (trait != trait_words().end()) {
                for (const Trait& t : traits()) {
                    if (t.word == trait->second) {
                        for (const Lean& lean : t.leans) {
                            if (!lean.control.empty()) {
                                wish.lean(lean.control, lean.value);
                            }
                        }
                    }
                }
                wish.understood.push_back(pending_word);
            } else {
                wish.ignored.push_back(pending_word);
            }
            pending.reset();
        }
        if (const auto trait = trait_words().find(w); trait != trait_words().end()) {
            for (const Trait& t : traits()) {
                if (t.word != trait->second) {
                    continue;
                }
                for (const Lean& lean : t.leans) {
                    if (!lean.control.empty()) {
                        wish.lean(lean.control, lean.value);
                    }
                }
                if (t.wrinkles >= 0.0F) {
                    wish.wrinkles = std::max(wish.wrinkles.value_or(0.0F), t.wrinkles);
                    if (t.wrinkles > 0.4F) {
                        wish.blotches = 0.6F;
                    }
                }
            }
            wish.understood.push_back(w);
            continue;
        }
        if (const auto shape = shape_words().find(w); shape != shape_words().end()) {
            for (const Lean& lean : shape->second) {
                wish.lean(lean.control, lean.value);
            }
            wish.understood.push_back(w);
            continue;
        }
        const auto extra =
            std::ranges::find_if(extras(), [&](const Extra& e) { return e.word == w; });
        if (extra != extras().end()) {
            if (!extra->mark.empty()) {
                wish.marks.emplace_back(extra->mark);
            }
            if (!extra->beard.empty()) {
                wish.beard = std::string(extra->beard);
            }
            if (!extra->brows.empty()) {
                wish.brows = std::string(extra->brows);
            }
            if (!extra->style.empty()) {
                wish.style = std::string(extra->style);
            }
            wish.understood.push_back(w);
            continue;
        }
        if (w == "symmetrical" || w == "symmetric") {
            wish.asymmetry = 0.0F;
            wish.understood.push_back(w);
            continue;
        }
        if (w == "asymmetrical" || w == "asymmetric" || w == "lopsided") {
            wish.asymmetry = 0.6F;
            wish.understood.push_back(w);
            continue;
        }
        if (!filler(w)) {
            wish.ignored.push_back(w);
        }
    }
    if (pending) {
        if (trait_words().contains(pending_word)) {
            for (const Trait& t : traits()) {
                if (t.word == trait_words().at(pending_word)) {
                    for (const Lean& lean : t.leans) {
                        if (!lean.control.empty()) {
                            wish.lean(lean.control, lean.value);
                        }
                    }
                }
            }
            wish.understood.push_back(pending_word);
        } else {
            wish.ignored.push_back(pending_word);
        }
    }
    return wish;
}

// Where a kind of mark goes on a generated face, and its own look.
world::FaceMark mark_for(std::string_view kind, Random& random) {
    world::FaceMark mark;
    mark.kind = std::string(kind);
    mark.seed = static_cast<std::uint32_t>(random.next() & 0xFFFFFFFFU);
    const bool left = random.unit() < 0.5F;
    const auto side = [&](std::string_view base) {
        return std::string(base) + (left ? "_left" : "_right");
    };
    if (kind == "scar") {
        static constexpr std::array<std::string_view, 3> kAt{"eye", "cheek", "brow"};
        mark.at = side(kAt.at(random.pick(kAt.size())));
        mark.angle = static_cast<float>(rounded(random.between(50.0F, 110.0F), 1.0));
        mark.length = static_cast<float>(rounded(random.between(0.15F, 0.35F), 100.0));
        mark.pattern = random.unit() < 0.4F ? "stitched" : "";
        mark.age = static_cast<float>(rounded(random.between(0.3F, 0.9F), 100.0));
    } else if (kind == "freckles") {
        mark.at = "nose_bridge";
        mark.offset = {0.0F, -0.05F};
        mark.mirror = false;
        mark.amount = static_cast<float>(rounded(random.between(0.5F, 0.9F), 100.0));
    } else if (kind == "mole") {
        static constexpr std::array<std::string_view, 3> kAt{"mouth_corner", "cheek", "cheekbone"};
        mark.at = side(kAt.at(random.pick(kAt.size())));
        mark.offset = {static_cast<float>(rounded(random.between(-0.04F, 0.04F), 100.0)),
                       static_cast<float>(rounded(random.between(-0.04F, 0.04F), 100.0))};
    } else if (kind == "tattoo") {
        static constexpr std::array<std::string_view, 5> kPatterns{"lines", "dots", "star",
                                                                   "spiral", "teardrop"};
        mark.at = side("cheekbone");
        mark.pattern = std::string(kPatterns.at(random.pick(kPatterns.size())));
    } else if (kind == "paint") {
        static constexpr std::array<std::string_view, 4> kPatterns{"band", "stripes", "chevron",
                                                                   "mask"};
        mark.at = "nose_bridge";
        mark.pattern = std::string(kPatterns.at(random.pick(kPatterns.size())));
        mark.mirror = mark.pattern == "stripes" || mark.pattern == "chevron";
        if (mark.mirror) {
            mark.at = "cheek_right";
        }
    } else if (kind == "wound" || kind == "burn" || kind == "bruise" || kind == "rash" ||
               kind == "birthmark") {
        static constexpr std::array<std::string_view, 3> kAt{"cheek", "cheekbone", "jaw"};
        mark.at = side(kAt.at(random.pick(kAt.size())));
        mark.age = static_cast<float>(rounded(random.between(0.0F, 0.6F), 100.0));
    }
    return mark;
}

// A candidate face for a wish: each control near what the wish leans it to (anywhere for the
// rest), within the rules' limits.
world::CharacterFace candidate(const Wish& wish, const std::vector<const Rules*>& covering,
                               const std::string& style, Random& random,
                               const std::map<std::string, float, std::less<>>& fixed) {
    world::CharacterFace face;
    face.style = style;
    face.controls.assign(world::face_controls().size(), 0.0F);
    for (std::size_t i = 0; i < world::face_controls().size(); ++i) {
        const std::string_view name = world::face_controls()[i].name;
        float value = 0.0F;
        if (const auto given = fixed.find(name); given != fixed.end()) {
            value = given->second;
        } else if (const auto lean = wish.controls.find(name); lean != wish.controls.end()) {
            value = lean->second + random.between(-0.15F, 0.15F);
        } else {
            // The rest vary a little, and those that hardly would are left at 0.
            value = random.between(-0.3F, 0.3F);
            if (std::abs(value) < 0.1F) {
                value = 0.0F;
            }
        }
        float low = -1.0F;
        float high = 1.0F;
        for (const Rules* rule : covering) {
            if (const auto range = rule->controls.find(name); range != rule->controls.end()) {
                low = std::max(low, range->second.first);
                high = std::min(high, range->second.second);
            }
        }
        value = std::clamp(value, low, std::max(low, high));
        face.controls[i] = static_cast<float>(rounded(value, 100.0));
    }
    if (wish.wrinkles) {
        face.skin.wrinkles = *wish.wrinkles;
    }
    if (wish.blotches) {
        face.skin.blotches = *wish.blotches;
    }
    if (wish.beard) {
        face.beard = *wish.beard;
    }
    if (wish.brows) {
        face.brows.shape = *wish.brows;
    } else {
        static constexpr std::array<std::string_view, 4> kShapes{"soft", "straight", "arched",
                                                                 "angled"};
        face.brows.shape = std::string(kShapes.at(random.pick(kShapes.size())));
    }
    face.brows.thickness = static_cast<float>(rounded(random.between(0.3F, 0.8F), 100.0));
    face.asymmetry = wish.asymmetry.value_or(
        style == "realistic" ? static_cast<float>(rounded(random.between(0.0F, 0.3F), 100.0))
                             : 0.0F);
    face.seed = static_cast<std::uint32_t>(random.next() & 0xFFFFU);
    for (const std::string& kind : wish.marks) {
        face.marks.push_back(mark_for(kind, random));
    }
    return face;
}

// A face as a .face.json file: what differs from the defaults.
json::Value face_json(const world::CharacterFace& face, std::string_view name) {
    json::Value out;
    out["format"] = 1;
    out["name"] = name;
    if (face.style != "realistic") {
        out["style"] = face.style;
    }
    json::Value controls = json::Object{};
    for (std::size_t i = 0; i < face.controls.size(); ++i) {
        if (face.controls[i] != 0.0F) {
            controls[world::face_controls()[i].name] = rounded(face.controls[i], 100.0);
        }
    }
    if (!controls.as_object().empty()) {
        out["controls"] = std::move(controls);
    }
    json::Value& brows = out["brows"];
    brows["shape"] = face.brows.shape;
    brows["thickness"] = rounded(face.brows.thickness, 100.0);
    if (face.beard != "none") {
        out["beard"] = face.beard;
    }
    json::Value skin = json::Object{};
    if (face.skin.wrinkles > 0.0F) {
        skin["wrinkles"] = rounded(face.skin.wrinkles, 100.0);
    }
    if (face.skin.blotches >= 0.0F) {
        skin["blotches"] = rounded(face.skin.blotches, 100.0);
    }
    if (!skin.as_object().empty()) {
        out["skin"] = std::move(skin);
    }
    if (face.asymmetry > 0.0F) {
        out["asymmetry"] = rounded(face.asymmetry, 100.0);
        out["seed"] = static_cast<std::int64_t>(face.seed);
    }
    if (!face.marks.empty()) {
        json::Array marks;
        for (const world::FaceMark& mark : face.marks) {
            json::Value item;
            item["kind"] = mark.kind;
            item["at"] = mark.at;
            if (mark.offset.x != 0.0F || mark.offset.y != 0.0F) {
                item["offset"] =
                    json::Array{rounded(mark.offset.x, 100.0), rounded(mark.offset.y, 100.0)};
            }
            if (mark.length > 0.0F) {
                item["length"] = rounded(mark.length, 100.0);
            }
            if (mark.angle != 0.0F) {
                item["angle"] = rounded(mark.angle, 1.0);
            }
            if (mark.amount != 1.0F) {
                item["amount"] = rounded(mark.amount, 100.0);
            }
            if (mark.age > 0.0F) {
                item["age"] = rounded(mark.age, 100.0);
            }
            if (!mark.pattern.empty()) {
                item["pattern"] = mark.pattern;
            }
            if (mark.mirror) {
                item["mirror"] = true;
            }
            if (mark.seed != 0) {
                item["seed"] = static_cast<std::int64_t>(mark.seed);
            }
            marks.push_back(std::move(item));
        }
        out["marks"] = std::move(marks);
    }
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
    return out.empty() ? std::string("face") : out;
}

json::Array text_list(const std::vector<std::string>& items) {
    json::Array out;
    for (const std::string& item : items) {
        out.emplace_back(item);
    }
    return out;
}

Reply face_generate(Session& session, const json::Value& params) {
    const std::string& name = params.find("name")->as_string();
    std::string out = "faces/" + slug(name) + std::string(world::kFaceSuffix);
    if (const json::Value* given = params.find("out"); given != nullptr) {
        out = given->as_string();
    }
    if (!world::is_face_path(out)) {
        return param_error(code::kInvalidParams, "/out",
                           quoted(out) + " must end in " + std::string(world::kFaceSuffix));
    }
    std::map<std::string, float, std::less<>> fixed;
    if (const json::Value* controls = params.find("controls"); controls != nullptr) {
        std::vector<std::string_view> names;
        for (const world::FaceControl& control : world::face_controls()) {
            names.push_back(control.name);
        }
        for (const auto& [control, value] : controls->as_object()) {
            if (!world::face_control_index(control)) {
                std::string message = quoted(control) + " isn't a sculpt control";
                if (const std::string_view hint = closest_match(control, names); !hint.empty()) {
                    message += "; did you mean " + quoted(hint) + "?";
                }
                return param_error(code::kInvalidParams, "/controls/" + control, message);
            }
            fixed[control] = static_cast<float>(value.as_double());
        }
    }
    const std::string brief =
        params.find("brief") != nullptr ? params.find("brief")->as_string() : std::string();
    const Wish wish = read_brief(brief);
    auto files = face_files(session);
    if (!files) {
        return Reply::failure(files.error());
    }
    auto rules = load_rules(session);
    if (!rules) {
        return Reply::failure(rules.error());
    }
    const std::vector<const Rules*> covering = rules_for(rules.value().rules, out);
    // The style: asked for, else the brief's, else the first the rules allow.
    std::string style = "realistic";
    if (const json::Value* given = params.find("style"); given != nullptr) {
        style = given->as_string();
    } else if (wish.style) {
        style = *wish.style;
    } else {
        for (const Rules* rule : covering) {
            if (!rule->styles.empty()) {
                style = rule->styles.front();
                break;
            }
        }
    }
    // Marks the rules don't allow are left out.
    Wish allowed = wish;
    std::erase_if(allowed.marks, [&](const std::string& kind) {
        return std::ranges::any_of(covering, [&](const Rules* rule) {
            return rule->marks && std::ranges::find(*rule->marks, kind) == rule->marks->end();
        });
    });
    for (const Rules* rule : covering) {
        if (rule->max_marks && static_cast<std::int64_t>(allowed.marks.size()) > *rule->max_marks) {
            allowed.marks.resize(
                static_cast<std::size_t>(std::max<std::int64_t>(0, *rule->max_marks)));
        }
    }
    std::vector<std::string> files_but_out;
    for (const std::string& path : files.value()) {
        if (path != out) {
            files_but_out.push_back(path);
        }
    }
    const Cast cast = read_cast(session, files_but_out, false);
    const Asked asks = asked(covering);
    const std::uint64_t seed = params.find("seed") != nullptr
                                   ? static_cast<std::uint64_t>(params.find("seed")->as_int())
                                   : 0U;
    const std::int64_t count =
        params.find("candidates") != nullptr ? params.find("candidates")->as_int() : 12;
    Random random{hash_text(name) ^ hash_text(brief) ^ (seed * 0x9e3779b97f4a7c15ULL)};
    std::optional<world::CharacterFace> best;
    real_t best_score = -1.0F;
    std::size_t best_nearest = 0;
    Apart best_apart;
    for (std::int64_t k = 0; k < count; ++k) {
        world::CharacterFace face = candidate(allowed, covering, style, random, fixed);
        const Measures measures = measure(face);
        // How far it is from the nearest other face, in shares of what the rules ask.
        real_t score = cast.faces.empty() ? 0.0F : 1e9F;
        std::size_t nearest = 0;
        Apart nearest_apart;
        for (std::size_t o = 0; o < cast.faces.size(); ++o) {
            const Apart measured = apart(measures, cast.faces[o].measures);
            real_t s = measured.shape / asks.shape;
            if (asks.profile > 0.0F) {
                s = std::min(s, measured.profile / asks.profile);
            }
            if (s < score) {
                score = s;
                nearest = o;
                nearest_apart = measured;
            }
        }
        // Past what the rules ask, the one nearest the brief: the first.
        if (score > best_score && (best_score < 1.0F || !best)) {
            best_score = score;
            best = std::move(face);
            best_nearest = nearest;
            best_apart = nearest_apart;
        }
    }
    if (!best) {
        return Reply::failure(code::kInternalError, "no candidate was made");
    }
    auto before = session.read_project_file(out);
    if (!before) {
        return Reply::failure(before.error());
    }
    std::string after = json::write(face_json(*best, name));
    if (before.value() != after) {
        if (auto staged = session.stage_file(out, after); !staged) {
            return Reply::failure(staged.error());
        }
        session.add_undo_file(out, std::move(before).value(), std::move(after));
    }
    json::Value result;
    result["out"] = out;
    result["style"] = best->style;
    if (!wish.understood.empty()) {
        result["understood"] = text_list(wish.understood);
    }
    if (!wish.ignored.empty()) {
        result["ignored"] = text_list(wish.ignored);
    }
    if (allowed.marks.size() < wish.marks.size()) {
        result["marks_left_out"] =
            static_cast<std::int64_t>(wish.marks.size() - allowed.marks.size());
    }
    if (!cast.faces.empty()) {
        json::Value nearest;
        nearest["path"] = cast.faces[best_nearest].path;
        nearest["shape"] = rounded(best_apart.shape, 100.0);
        nearest["profile"] = rounded(best_apart.profile, 100.0);
        result["nearest"] = std::move(nearest);
        if (best_score < 1.0F) {
            result["warning"] = "no candidate was as different from " +
                                cast.faces[best_nearest].path +
                                " as the rules ask; try another seed or more candidates";
        }
    }
    return Reply::success(std::move(result));
}

// ---- face.portrait ----

SessionOptions stage_options(const Session& host) {
    SessionOptions options = host.options();
    options.capture_logs = false;
    options.write_files = false;
    std::erase(options.capabilities, Capability::Build);
    return options;
}

Result<void> called(const Reply& reply, std::string_view what) {
    if (reply.ok) {
        return {};
    }
    return make_error(ErrorCode::InvalidArgument, std::string(what) + ": " + reply.message);
}

json::Value vec3(Vec3 v) {
    return json::Array{static_cast<double>(v.x), static_cast<double>(v.y),
                       static_cast<double>(v.z)};
}

// Euler angles (YXZ, radians) that turn a camera or light at from to look at to.
Vec3 looking(Vec3 from, Vec3 to) {
    const Vec3f d = to_float(to - from);
    const float across = std::sqrt((d.x * d.x) + (d.z * d.z));
    return {slash::atan2(d.y, across), slash::atan2(-d.x, -d.z), 0.0F};
}

Result<void> create(Session& stage, std::string_view id, json::Value components) {
    json::Value params;
    params["id"] = id;
    params["components"] = std::move(components);
    return called(stage.call("entity.create", params), "making the portrait's stage");
}

// The 3D stage: the head framed by a long lens from the front, at three quarters or from the
// side, lit by a key light from the camera's side and a dimmer fill.
Result<void> stage_3d(Session& stage, const std::string& recipe, const world::HeadShape& head,
                      std::string_view view, const json::Value& expressions, Vec4 background) {
    const Vec3 c = head.center;
    const Vec3 r = head.radii;
    // Turned toward the face's left (+x), which the 2D profile shows too.
    float turn = 0.6F;
    if (view == "front") {
        turn = 0.0F;
    } else if (view == "side") {
        turn = kPi / 2.0F;
    }
    const Vec2f round = cos_sin(turn);
    // A 30 degree lens, the head and neck filling about four fifths of the frame.
    const real_t reach = (r.y * 1.55F) / 0.2679F;
    const Vec3 target{c.x, c.y - (r.y * 0.12F), c.z};
    const Vec3 eye{c.x + (round.y * reach), c.y + (r.y * 0.1F), c.z + (round.x * reach)};
    if (auto made = create(
            stage, "camera",
            json::Object{{"Camera3D", json::Object{{"current", true}, {"fov", 30.0}}},
                         {"Transform3D", json::Object{{"position", vec3(eye)},
                                                      {"rotation", vec3(looking(eye, target))}}}});
        !made) {
        return made;
    }
    const Vec3 key_from{c.x + (round.y * 0.6F) + 0.5F, c.y + 0.7F, c.z + (round.x * 0.6F) + 0.4F};
    const Vec3 fill_from{c.x - 0.7F, c.y + 0.1F, c.z + 0.5F};
    if (auto made = create(
            stage, "key",
            json::Object{{"DirectionalLight3D", json::Object{{"light_energy", 1.1}}},
                         {"Transform3D", json::Object{{"rotation", vec3(looking(key_from, c))}}}});
        !made) {
        return made;
    }
    if (auto made = create(
            stage, "fill",
            json::Object{{"DirectionalLight3D", json::Object{{"light_energy", 0.35}}},
                         {"Transform3D", json::Object{{"rotation", vec3(looking(fill_from, c))}}}});
        !made) {
        return made;
    }
    if (auto made = create(
            stage, "environment",
            json::Object{
                {"WorldEnvironment",
                 json::Object{
                     {"background_mode", "color"},
                     {"background_color", json::Array{static_cast<double>(background.x),
                                                      static_cast<double>(background.y),
                                                      static_cast<double>(background.z), 1.0}},
                     {"ambient_light_source", "color"},
                     {"ambient_light_color", json::Array{0.35, 0.35, 0.38, 1.0}}}}});
        !made) {
        return made;
    }
    json::Value mesh = json::Object{{"mesh", recipe}};
    if (!expressions.as_object().empty()) {
        mesh["blend_shapes"] = expressions;
    }
    return create(
        stage, "subject",
        json::Object{{"MeshInstance3D", std::move(mesh)}, {"Transform3D", json::Object{}}});
}

// The 2D stage: the recipe's side-on cut-out, scaled so its head fills the frame, over a sprite
// of the background's color.
Result<void> stage_2d(Session& stage, const std::string& recipe, const world::HeadShape& head,
                      const json::Value& expressions, Vec4 background, Vec2 size) {
    render::Image white;
    white.width = 4;
    white.height = 4;
    white.pixels.assign(std::size_t{4} * 4 * 4, 255);
    auto texture = procedural::make_texture(stage.world(), "portrait_background", std::move(white));
    if (!texture) {
        return texture.error();
    }
    if (auto made = create(
            stage, "background",
            json::Object{
                {"Sprite2D",
                 json::Object{{"texture", texture.value()},
                              {"centered", false},
                              {"z_index", -100},
                              {"modulate", json::Array{static_cast<double>(background.x),
                                                       static_cast<double>(background.y),
                                                       static_cast<double>(background.z), 1.0}}}},
                {"Transform2D",
                 json::Object{{"scale", json::Array{static_cast<double>(size.x) / 4.0,
                                                    static_cast<double>(size.y) / 4.0}}}}});
        !made) {
        return made;
    }
    // The head fills about seven tenths of the frame's height, in its middle.
    const real_t ppm = (size.y * 0.7F) / (head.radii.y * 2.0F);
    json::Value place;
    place["recipe"] = recipe;
    place["id"] = "subject";
    place["at"] = json::Array{static_cast<double>((size.x * 0.5F) - (head.center.z * ppm)),
                              static_cast<double>((size.y * 0.5F) + (head.center.y * ppm))};
    place["pixels_per_meter"] = static_cast<double>(ppm);
    if (auto placed = called(stage.call("character.place", place), "placing the cut-out");
        !placed) {
        return placed;
    }
    if (!expressions.as_object().empty()) {
        json::Value set;
        set["id"] = "subject";
        set["components"]["Character2D"]["expressions"] = expressions;
        if (auto done = called(stage.call("component.set", set), "setting its expressions");
            !done) {
            return done;
        }
    }
    json::Value step;
    step["ticks"] = 1;
    return called(stage.call("step", step), "stepping the cut-out");
}

Reply face_portrait(Session& session, const json::Value& params) {
    const std::string& path = params.find("face")->as_string();
    const bool face_file = world::is_face_path(path);
    if (!face_file && !world::is_character_path(path)) {
        return param_error(code::kInvalidParams, "/face",
                           quoted(path) + " is neither a .face.json nor a .character.json file");
    }
    const world::FileReader read = reader_of(session);
    auto text = read(path);
    if (!text) {
        return param_error(code::kNotFound, "/face", text.error().message);
    }
    // A face file is drawn on a recipe of its own: the face on an average body, bald.
    std::string recipe_path = path;
    std::string recipe_text = text.value();
    if (face_file) {
        auto doc = json::parse(text.value());
        if (!doc) {
            return param_error(code::kInvalidParams, "/face", doc.error().message);
        }
        if (const auto problems = world::validate_face(doc.value()); !problems.empty()) {
            return param_error(code::kInvalidParams, "/face",
                               quoted(path) + problems.front().path + ": " +
                                   problems.front().message);
        }
        json::Value recipe;
        recipe["format"] = 1;
        recipe["name"] = "Portrait";
        recipe["face"]["from"] = path;
        recipe["hair"]["style"] = "none";
        recipe["clothing"] =
            json::Array{json::Object{{"kind", "shirt"}, {"color", json::Array{0.3, 0.32, 0.36}}}};
        recipe_text = json::write(recipe);
        recipe_path = ".slash/portrait/" + slug(path) + std::string(world::kCharacterSuffix);
    }
    auto doc = json::parse(recipe_text);
    if (!doc) {
        return param_error(code::kInvalidParams, "/face", doc.error().message);
    }
    const world::FileReader staged_read = [&](std::string_view name) -> Result<std::string> {
        if (name == recipe_path) {
            return recipe_text;
        }
        return read(name);
    };
    auto recipe = world::parse_character(doc.value(), staged_read);
    if (!recipe) {
        return param_error(code::kInvalidParams, "/face", recipe.error().message);
    }
    const world::HeadShape head = world::character_head(world::character_measures(recipe.value()));
    json::Value expressions = json::Object{};
    if (const json::Value* given = params.find("expressions"); given != nullptr) {
        for (const auto& [shape, weight] : given->as_object()) {
            if (!world::face_expression_index(shape)) {
                std::string message = quoted(shape) + " isn't one of ARKit's expression shapes";
                if (const std::string_view hint =
                        closest_match(shape, world::face_expression_names());
                    !hint.empty()) {
                    message += "; did you mean " + quoted(hint) + "?";
                }
                return param_error(code::kInvalidParams, "/expressions/" + shape, message);
            }
            expressions[shape] = weight;
        }
    }
    std::array<std::uint32_t, 2> size{256, 256};
    if (const json::Value* given = params.find("size"); given != nullptr) {
        size = {static_cast<std::uint32_t>(given->as_array()[0].as_int()),
                static_cast<std::uint32_t>(given->as_array()[1].as_int())};
    }
    Vec4 background{0.22F, 0.23F, 0.26F, 1.0F};
    if (const json::Value* given = params.find("background"); given != nullptr) {
        const json::Array& c = given->as_array();
        background = {static_cast<float>(c[0].as_double()), static_cast<float>(c[1].as_double()),
                      static_cast<float>(c[2].as_double()), 1.0F};
    }
    const std::string view =
        params.find("view") != nullptr ? params.find("view")->as_string() : "three_quarter";
    std::string out = "portraits/" + slug(path.substr(path.rfind('/') + 1)) + ".png";
    if (const json::Value* given = params.find("out"); given != nullptr) {
        out = given->as_string();
    }
    if (!out.ends_with(".png")) {
        return param_error(code::kInvalidParams, "/out", quoted(out) + " must end in .png");
    }
    auto full = session.resolve_path(out);
    if (!full) {
        return param_error(code::kInvalidParams, "/out", full.error().message);
    }
    SessionOptions options = stage_options(session);
    if (face_file) {
        options.files[recipe_path] = recipe_text;
    }
    Session stage(session.components(), session.commands(), options);
    const Vec2 viewport{static_cast<real_t>(size[0]), static_cast<real_t>(size[1])};
    const Result<void> built =
        view == "cutout" ? stage_2d(stage, recipe_path, head, expressions, background, viewport)
                         : stage_3d(stage, recipe_path, head, view, expressions, background);
    if (!built) {
        return Reply::failure(built.error());
    }
    canvas::CaptureOptions capture;
    capture.viewport = viewport;
    auto captured = canvas::capture(stage.world(), stage.assets(), capture);
    if (!captured) {
        return Reply::failure(captured.error());
    }
    auto png = render::encode_png(captured.value().image);
    if (!png) {
        return Reply::failure(png.error());
    }
    if (auto written = session.write_file(full.value(), png.value()); !written) {
        return Reply::failure(written.error());
    }
    json::Value result;
    result["out"] = out;
    result["width"] = static_cast<std::int64_t>(captured.value().image.width);
    result["height"] = static_cast<std::int64_t>(captured.value().image.height);
    return Reply::success(std::move(result));
}

} // namespace

void register_face_commands(Registry& registry) {
    using enum Capability;
    json::Value paths = property("array", "Faces to check, against every other. Default all.");
    paths["items"] = text_property("A .face.json or .character.json file.");
    registry.add({
        .name = "face.check",
        .description = "Checks the project's faces (.face.json files and characters' designed "
                       "faces, the presets others take theirs from aside) against the "
                       "storyboard's face_rules: sculpts far enough from every other face's "
                       "(and side profiles, for 2D), styles, sculpt control limits, marks and "
                       "palette. Returns errors with their file and JSON Pointer.",
        .capabilities = {Read, Filesystem},
        .effect = Effect::Read,
        .params = schema_of({{"paths", std::move(paths)},
                             {"measures", property("boolean", "Also return how far apart each "
                                                              "pair is. Default false.")}},
                            {}),
        .handler = face_check,
    });
    json::Value style = property("string", "How it is sculpted. Default the brief's, else the "
                                           "face_rules' first, else realistic.");
    style["enum"] = json::Array{"realistic", "stylized", "toon", "painterly", "low_poly"};
    json::Value controls = property("object", "Sculpt controls to keep as given, each -1 to 1.");
    controls["additionalProperties"] =
        json::Object{{"type", "number"}, {"minimum", -1}, {"maximum", 1}};
    json::Value seed = property("integer", "Picks other candidates. Default 0.");
    seed["minimum"] = 0;
    json::Value candidates = property("integer", "How many to make and choose from. Default 12.");
    candidates["minimum"] = 1;
    candidates["maximum"] = 64;
    registry.add({
        .name = "face.generate",
        .description = "Writes a .face.json face from a brief (\"a stern old soldier with a scar "
                       "and a hooked nose\"): makes candidates leaning its sculpt controls, skin, "
                       "brows, beard and marks as the brief's words say, within the storyboard's "
                       "face_rules, and keeps the one least like the project's other faces. "
                       "Returns the file, the words it understood and ignored and its nearest "
                       "face.",
        .capabilities = {Read, Edit, Filesystem},
        .effect = Effect::Edit,
        .params = schema_of({{"name", text_property("Whose face.")},
                             {"brief", property("string", "What it looks like, in words.")},
                             {"out", text_property("The file to write. Default "
                                                   "faces/<name>.face.json.")},
                             {"style", std::move(style)},
                             {"controls", std::move(controls)},
                             {"seed", std::move(seed)},
                             {"candidates", std::move(candidates)}},
                            {"name"}),
        .handler = face_generate,
    });
    json::Value view = property("string", "front, three_quarter (default) or side in 3D; cutout "
                                          "for the 2D cut-out's side-on head.");
    view["enum"] = json::Array{"front", "three_quarter", "side", "cutout"};
    json::Value size = property("array", "[width, height] in pixels. Default [256, 256].");
    size["items"] = json::Object{{"type", "integer"}, {"minimum", 16}, {"maximum", 2048}};
    size["minItems"] = 2;
    size["maxItems"] = 2;
    json::Value background = property("array", "[r, g, b], 0 to 1.");
    background["items"] = json::Object{{"type", "number"}, {"minimum", 0}, {"maximum", 1}};
    background["minItems"] = 3;
    background["maxItems"] = 3;
    json::Value expressions =
        property("object", "ARKit expression shapes by name, 0 to 1 (\"mouthSmileLeft\").");
    expressions["additionalProperties"] =
        json::Object{{"type", "number"}, {"minimum", 0}, {"maximum", 1}};
    registry.add({
        .name = "face.portrait",
        .description = "Draws a face's portrait into a PNG, as dialogue boxes and menus show "
                       "them: a .face.json file on a bald average head, or a character "
                       "recipe as it is, in 3D from the front, three quarters or the side, or "
                       "as its 2D cut-out, with expressions. Returns the file and its size.",
        .capabilities = {Read, Filesystem},
        .effect = Effect::Read,
        .params = schema_of({{"face", text_property("A .face.json or .character.json file.")},
                             {"out", text_property("The PNG to write. Default "
                                                   "portraits/<file>.png.")},
                             {"view", std::move(view)},
                             {"size", std::move(size)},
                             {"background", std::move(background)},
                             {"expressions", std::move(expressions)}},
                            {"face"}),
        .handler = face_portrait,
    });
}

} // namespace slash::command
