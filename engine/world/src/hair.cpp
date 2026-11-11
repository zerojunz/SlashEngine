// Hair's grooms, its strand solver and the colliders it keeps out of, shared by 2D and 3D hair.
#include "slash/world/hair.hpp"

#include "slash/asset/assets.hpp"
#include "slash/world/components.hpp"
#include "slash/world/tree2d.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <mutex>
#include <utility>

namespace slash::world {

namespace {

constexpr std::string_view kSchema = R"({
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "title": "SlashEngine groom",
  "description": "Hair or fur that a HairInstance3D grows over a model or a Hair2D along a line, after Unreal's Groom: how its strands grow, draw and move. Lengths are meters in 3D and pixels in 2D.",
  "type": "object",
  "additionalProperties": false,
  "required": ["format"],
  "properties": {
    "format": {"const": 1, "description": "Format version. Must be 1."},
    "space": {"enum": ["3d", "2d"], "description": "Whether it grows in a 3D or a 2D world. Default 3d."},
    "render": {"enum": ["strands", "cards", "shells"], "description": "How it draws: thin strands facing the camera, cards (a textured ribbon a guide) or fur shells (3D: layers over the surface). Default strands."},
    "guides": {"type": "integer", "minimum": 1, "maximum": 1024, "description": "Strands simulated; the rest follow the nearest. Default 32."},
    "density": {"type": "number", "minimum": 0, "description": "Strands drawn per square meter of the surface (3D) or per pixel of the root line (2D). Default 2000 (3D), 1 (2D)."},
    "max_strands": {"type": "integer", "minimum": 1, "maximum": 65536, "description": "Strands drawn at most. Default 4096."},
    "segments": {"type": "integer", "minimum": 1, "maximum": 32, "description": "Points along a strand after its root. Default 6."},
    "length": {"type": "number", "minimum": 0, "description": "A strand's length. Default 0.1 (3D), 24 (2D)."},
    "length_variation": {"type": "number", "minimum": 0, "maximum": 1, "description": "The share a strand may be shorter, at random. Default 0.2."},
    "length_map": {"type": "string", "description": "An image whose red scales the length over the surface's texture coordinates (3D) or along the root line (2D); 0 grows none there."},
    "density_map": {"type": "string", "description": "An image whose red is the chance a strand grows there."},
    "color_map": {"type": "string", "description": "An image whose color multiplies the strands' colors."},
    "root_color": {"type": "array", "items": {"type": "number", "minimum": 0}, "minItems": 3, "maxItems": 4, "description": "RGBA at the root. Default [0.2, 0.13, 0.08, 1]."},
    "tip_color": {"type": "array", "items": {"type": "number", "minimum": 0}, "minItems": 3, "maxItems": 4, "description": "RGBA at the tip. Default [0.45, 0.32, 0.2, 1]."},
    "width": {"type": "number", "exclusiveMinimum": 0, "description": "A strand's width at its root. Default 0.002 (3D), 1.5 (2D)."},
    "tip_width": {"type": "number", "minimum": 0, "description": "A strand's width at its tip. Default 0.0005 (3D), 0.5 (2D)."},
    "lift": {"type": "number", "minimum": 0, "maximum": 1, "description": "How far strands stand out along the surface's normal (1) rather than turn toward comb (0). Default 1."},
    "comb": {"type": "array", "items": {"type": "number"}, "minItems": 2, "maxItems": 3, "description": "The way strands are combed, in the model's space (3D) or the entity's (2D). Default [0, -1, 0] (3D), [0, 1] (2D): down."},
    "clumping": {"type": "number", "minimum": 0, "maximum": 1, "description": "How far strands gather toward their guide at the tip. Default 0.3."},
    "curl_radius": {"type": "number", "minimum": 0, "description": "How far strands curl about their way. Default 0."},
    "curl_turns": {"type": "number", "minimum": 0, "description": "How many times they curl along their length. Default 0."},
    "seed": {"type": "integer", "minimum": 0, "maximum": 4294967295, "description": "Picks where strands grow. Default 0."},
    "card_width": {"type": "number", "exclusiveMinimum": 0, "description": "Cards: each ribbon's width. Default 0.02 (3D), 6 (2D)."},
    "texture": {"type": "string", "description": "Cards: the ribbon's image, across its width and down its length, its alpha cutting it out."},
    "shell_count": {"type": "integer", "minimum": 1, "maximum": 64, "description": "Shells: layers of fur over the surface. Default 16."},
    "shell_tiling": {"type": "number", "exclusiveMinimum": 0, "description": "Shells: times the fur's pattern repeats over the texture coordinates. Default 8."},
    "roughness": {"type": "number", "minimum": 0, "maximum": 1, "description": "3D: as StandardMaterial3D's. Default 0.45."},
    "anisotropy": {"type": "number", "minimum": -1, "maximum": 1, "description": "3D: how far the highlight stretches across the strands, as StandardMaterial3D's anisotropy. Default 0.8."},
    "specular": {"type": "number", "minimum": 0, "maximum": 1, "description": "3D: as StandardMaterial3D's metallic_specular. Default 0.5."},
    "simulation": {
      "type": "object", "additionalProperties": false,
      "description": "How the guides move, stepped by physics.",
      "properties": {
        "enabled": {"type": "boolean", "description": "Whether they move at all; off, they keep their groomed shape on the body. Default true."},
        "stiffness": {"type": "number", "minimum": 0, "maximum": 1, "description": "The pull back toward the groomed shape at the root. Default 0.5."},
        "tip_stiffness": {"type": "number", "minimum": 0, "maximum": 1, "description": "And at the tip. Default 0.05."},
        "bend_stiffness": {"type": "number", "minimum": 0, "maximum": 1, "description": "How hard each keeps the bends it was groomed with. Default 0.2."},
        "damping": {"type": "number", "minimum": 0, "description": "Per second. Default 2."},
        "gravity_scale": {"type": "number", "description": "Default 1."},
        "drag_coefficient": {"type": "number", "minimum": 0, "maximum": 1, "description": "How hard WindSource3Ds' and WindSource2Ds' wind carries it. Default 1."},
        "point_radius": {"type": "number", "minimum": 0, "description": "How far each point keeps from the body's shapes. Default 0.002 (3D), 1 (2D)."},
        "precision": {"type": "integer", "minimum": 1, "maximum": 32, "description": "Small steps per physics step. Default 4."}
      }
    }
  }
})";

json::Value make_schema() {
    return json::parse(kSchema).value();
}

void read(const json::Value& doc, std::string_view key, float& out) {
    if (const json::Value* value = doc.find(key); value != nullptr) {
        out = static_cast<float>(value->as_double());
    }
}

void read(const json::Value& doc, std::string_view key, int& out) {
    if (const json::Value* value = doc.find(key); value != nullptr) {
        out = static_cast<int>(value->as_int());
    }
}

void read(const json::Value& doc, std::string_view key, bool& out) {
    if (const json::Value* value = doc.find(key); value != nullptr) {
        out = value->as_bool();
    }
}

void read(const json::Value& doc, std::string_view key, std::string& out) {
    if (const json::Value* value = doc.find(key); value != nullptr) {
        out = value->as_string();
    }
}

void read(const json::Value& doc, std::string_view key, Vec4& out) {
    if (const json::Value* value = doc.find(key); value != nullptr) {
        const json::Array& items = value->as_array();
        out = {static_cast<float>(items[0].as_double()), static_cast<float>(items[1].as_double()),
               static_cast<float>(items[2].as_double()),
               items.size() > 3 ? static_cast<float>(items[3].as_double()) : 1.0F};
    }
}

StrandCollider capsule(Vec3 a, Vec3 b, real_t radius) {
    StrandCollider out;
    out.a = a;
    out.b = b;
    out.radius = radius;
    return out;
}

StrandCollider box(const Affine3D& place, Vec3 half) {
    StrandCollider out;
    out.box = true;
    out.place = place;
    out.half = half;
    return out;
}

real_t dot(Vec3 a, Vec3 b) {
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

real_t length(Vec3 v) {
    return std::sqrt(dot(v, v));
}

// The share of a pull one of n small steps makes, so that n steps together make stiffness of it.
float per_step(float stiffness, int steps) {
    const float s = std::clamp(stiffness, 0.0F, 1.0F);
    if (s >= 1.0F) {
        return 1.0F;
    }
    if (s <= 0.0F) {
        return 0.0F;
    }
    return 1.0F - slash::exp(slash::ln(1.0F - s) / static_cast<float>(steps));
}

// Moves two points toward a length, each by its share.
void hold(Vec3& a, Vec3& b, bool a_free, real_t rest, float stiffness) {
    const real_t w = a_free ? 2.0F : 1.0F;
    const Vec3 d = b - a;
    const real_t at = length(d);
    if (at <= 0.0F) {
        return;
    }
    const Vec3 fix = d * (((at - rest) / (at * w)) * stiffness);
    if (a_free) {
        a = a + fix;
    }
    b = b - fix;
}

// The point of a segment nearest p.
Vec3 nearest_on(Vec3 a, Vec3 b, Vec3 p) {
    const Vec3 ab = b - a;
    const real_t span = dot(ab, ab);
    if (span <= 0.0F) {
        return a;
    }
    return a + (ab * std::clamp<real_t>(dot(p - a, ab) / span, 0.0F, 1.0F));
}

// Pushes a point out of a collider to margin beyond its surface.
void keep_out(Vec3& point, const StrandCollider& collider, real_t margin) {
    if (!collider.box) {
        const Vec3 near = nearest_on(collider.a, collider.b, point);
        const Vec3 away = point - near;
        const real_t d = length(away);
        const real_t reach = collider.radius + margin;
        if (d >= reach) {
            return;
        }
        // At the very middle there is no way out; go up.
        point = d > 0.0F ? near + (away * (reach / d)) : near + Vec3{0.0F, reach, 0.0F};
        return;
    }
    const Affine3D undo = collider.place.inverse();
    const Vec3 local = undo.apply(point);
    const Vec3 half{collider.half.x + margin, collider.half.y + margin, collider.half.z + margin};
    if (std::abs(local.x) >= half.x || std::abs(local.y) >= half.y || std::abs(local.z) >= half.z) {
        return;
    }
    // Out through the nearest face.
    const real_t dx = half.x - std::abs(local.x);
    const real_t dy = half.y - std::abs(local.y);
    const real_t dz = half.z - std::abs(local.z);
    Vec3 out = local;
    if (dx <= dy && dx <= dz) {
        out.x = local.x < 0.0F ? -half.x : half.x;
    } else if (dy <= dz) {
        out.y = local.y < 0.0F ? -half.y : half.y;
    } else {
        out.z = local.z < 0.0F ? -half.z : half.z;
    }
    point = collider.place.apply(out);
}

template <typename Holder>
const HairStrands* strands_from(const World& world, entt::entity e) {
    const auto* holder = world.ecs().ctx().find<Holder>();
    if (holder == nullptr || !holder->source) {
        return nullptr;
    }
    return holder->source(world, e);
}

// Parsed grooms by path, kept while their file's version stays, and Hair2Ds' growths by key.
struct GroomCache {
    std::mutex mutex;
    std::map<std::string, LoadedGroom, std::less<>> grooms;
    std::map<entt::entity, HairGrown2D> grown_2d;
    std::map<std::string, std::shared_ptr<const void>, std::less<>> grown_3d;
};

struct HairImageReaderHolder {
    HairImageReader reader;
};
using GroomCacheHolder = std::shared_ptr<GroomCache>;

struct HairSource3DHolder {
    HairSource source;
};

struct HairSource2DHolder {
    HairSource source;
};

// Whether an entity, or the nearest above it in its tree with a body, is a character's body.
template <typename Character, typename Tree>
bool on_character(const World& world, const Tree& tree, entt::entity entity) {
    for (entt::entity at = entity; at != entt::null; at = tree.parent(at)) {
        if (world.ecs().all_of<Character>(at)) {
            return true;
        }
        if (at != entity) {
            return false;
        }
    }
    return false;
}

// Whether an entity is root or under it in its tree.
template <typename Tree>
bool under(const Tree& tree, entt::entity entity, entt::entity root) {
    for (entt::entity at = entity; at != entt::null; at = tree.parent(at)) {
        if (at == root) {
            return true;
        }
    }
    return false;
}

} // namespace

bool is_groom_path(std::string_view path) {
    return path.ends_with(kGroomSuffix);
}

json::Value groom_schema() {
    static const json::Value schema = make_schema();
    return schema;
}

std::vector<json::ValidationError> validate_groom(const json::Value& doc) {
    std::vector<json::ValidationError> errors = json::validate(doc, groom_schema());
    if (!errors.empty()) {
        return errors;
    }
    const json::Value* space = doc.find("space");
    const bool flat = space != nullptr && space->as_string() == "2d";
    if (const json::Value* render = doc.find("render");
        flat && render != nullptr && render->as_string() == "shells") {
        errors.push_back({"/render", "2D hair draws as strands or cards; shells need a surface"});
    }
    const json::Value* width = doc.find("width");
    const json::Value* tip = doc.find("tip_width");
    if (width != nullptr && tip != nullptr && tip->as_double() > width->as_double()) {
        errors.push_back({"/tip_width", "expected a tip no wider than the root's width"});
    }
    return errors;
}

Result<Groom> parse_groom(const json::Value& doc) {
    const std::vector<json::ValidationError> errors = validate_groom(doc);
    if (!errors.empty()) {
        return make_error(ErrorCode::InvalidArgument,
                          (errors.front().path.empty() ? "" : errors.front().path + ": ") +
                              errors.front().message);
    }
    Groom groom;
    read(doc, "space", groom.space);
    if (groom.space == "2d") {
        groom.density = 1.0F;
        groom.length = 24.0F;
        groom.width = 1.5F;
        groom.tip_width = 0.5F;
        groom.comb = {0.0F, 1.0F, 0.0F};
        groom.card_width = 6.0F;
        groom.point_radius = 1.0F;
    }
    read(doc, "render", groom.render);
    read(doc, "guides", groom.guides);
    read(doc, "density", groom.density);
    read(doc, "max_strands", groom.max_strands);
    read(doc, "segments", groom.segments);
    read(doc, "length", groom.length);
    read(doc, "length_variation", groom.length_variation);
    read(doc, "length_map", groom.length_map);
    read(doc, "density_map", groom.density_map);
    read(doc, "color_map", groom.color_map);
    read(doc, "root_color", groom.root_color);
    read(doc, "tip_color", groom.tip_color);
    read(doc, "width", groom.width);
    read(doc, "tip_width", groom.tip_width);
    groom.tip_width = std::min(groom.tip_width, groom.width);
    read(doc, "lift", groom.lift);
    if (const json::Value* comb = doc.find("comb"); comb != nullptr) {
        const json::Array& items = comb->as_array();
        groom.comb = {static_cast<real_t>(items[0].as_double()),
                      static_cast<real_t>(items[1].as_double()),
                      items.size() > 2 ? static_cast<real_t>(items[2].as_double()) : 0.0F};
    }
    read(doc, "clumping", groom.clumping);
    read(doc, "curl_radius", groom.curl_radius);
    read(doc, "curl_turns", groom.curl_turns);
    if (const json::Value* seed = doc.find("seed"); seed != nullptr) {
        groom.seed = static_cast<std::uint32_t>(seed->as_int());
    }
    read(doc, "card_width", groom.card_width);
    read(doc, "texture", groom.texture);
    read(doc, "shell_count", groom.shell_count);
    read(doc, "shell_tiling", groom.shell_tiling);
    read(doc, "roughness", groom.roughness);
    read(doc, "anisotropy", groom.anisotropy);
    read(doc, "specular", groom.specular);
    if (const json::Value* simulation = doc.find("simulation"); simulation != nullptr) {
        read(*simulation, "enabled", groom.simulate);
        read(*simulation, "stiffness", groom.stiffness);
        read(*simulation, "tip_stiffness", groom.tip_stiffness);
        read(*simulation, "bend_stiffness", groom.bend_stiffness);
        read(*simulation, "damping", groom.damping);
        read(*simulation, "gravity_scale", groom.gravity_scale);
        read(*simulation, "drag_coefficient", groom.drag_coefficient);
        read(*simulation, "point_radius", groom.point_radius);
        read(*simulation, "precision", groom.precision);
    }
    return groom;
}

void install_hair(World& world) {
    if (world.ecs().ctx().find<GroomCacheHolder>() == nullptr) {
        world.ecs().ctx().emplace<GroomCacheHolder>(std::make_shared<GroomCache>());
    }
}

Result<LoadedGroom> load_groom(const World& world, const std::string& path) {
    if (!is_groom_path(path)) {
        return make_error(ErrorCode::InvalidArgument, "\"" + path + "\" is not a .groom.json file");
    }
    asset::Assets* assets = asset::assets_of(world.ecs());
    if (assets == nullptr) {
        const FileReader& reader = world.file_reader();
        if (!reader) {
            return make_error(ErrorCode::PlatformFailure,
                              "the world has no files to read grooms from");
        }
        auto bytes = reader(path);
        if (!bytes) {
            return bytes.error();
        }
        auto doc = json::parse(bytes.value());
        if (!doc) {
            return doc.error();
        }
        auto groom = parse_groom(doc.value());
        if (!groom) {
            return groom.error();
        }
        return LoadedGroom{.groom = std::make_shared<const Groom>(std::move(groom).value())};
    }
    auto doc = assets->load<json::Value>(path);
    if (!doc) {
        return doc.error();
    }
    const std::uint64_t version = doc.value().version();
    const auto* holder = world.ecs().ctx().find<GroomCacheHolder>();
    GroomCache* cache = holder != nullptr ? holder->get() : nullptr;
    std::unique_lock<std::mutex> lock;
    if (cache != nullptr) {
        lock = std::unique_lock<std::mutex>(cache->mutex);
        if (const auto found = cache->grooms.find(path);
            found != cache->grooms.end() && found->second.version == version) {
            return found->second;
        }
    }
    auto groom = parse_groom(*doc.value());
    if (!groom) {
        return groom.error();
    }
    LoadedGroom loaded{.groom = std::make_shared<const Groom>(std::move(groom).value()),
                       .version = version};
    if (cache != nullptr) {
        cache->grooms[path] = loaded;
    }
    return loaded;
}

void step_strands(Strands& strands, int points, const std::vector<Vec3>& targets,
                  const StrandStep& step) {
    const auto n = static_cast<std::size_t>(std::max(points, 1));
    const std::size_t count = targets.size();
    if (strands.points != points || strands.current.size() != count || step.seconds <= 0.0F) {
        strands.points = points;
        strands.current = targets;
        strands.previous = targets;
        strands.targets = targets;
        strands.velocity.assign(count, Vec3{});
        return;
    }
    const int steps = std::clamp(step.steps, 1, 32);
    const real_t h = step.seconds / static_cast<float>(steps);
    const float bend = per_step(step.bend_stiffness, steps);
    // How fast a point takes up the wind's speed at a drag coefficient of 1, per second.
    constexpr float kWindRate = 6.0F;
    // How much of follow-the-leader's correction is taken back out of the velocity, as Müller's
    // dynamic follow-the-leader damps the strand's stretching.
    constexpr float kLeaderDamping = 0.9F;
    std::vector<Vec3> place(count);
    std::vector<Vec3> start(count);
    std::vector<Vec3> fixes(count);
    std::vector<float> pull(n);
    for (std::size_t k = 0; k < n; ++k) {
        const float t = n > 1 ? static_cast<float>(k) / static_cast<float>(n - 1) : 0.0F;
        pull[k] = per_step(step.stiffness + ((step.tip_stiffness - step.stiffness) * t), steps);
    }
    std::vector<Vec3>& x = strands.current;
    std::vector<Vec3>& v = strands.velocity;
    strands.previous = x;
    for (int s = 0; s < steps; ++s) {
        const real_t f = static_cast<float>(s + 1) / static_cast<float>(steps);
        for (std::size_t i = 0; i < count; ++i) {
            place[i] = strands.targets[i] + ((targets[i] - strands.targets[i]) * f);
            start[i] = x[i];
            fixes[i] = Vec3{};
        }
        for (std::size_t first = 0; first + n <= count; first += n) {
            x[first] = place[first];
            v[first] = Vec3{};
            for (std::size_t k = 1; k < n; ++k) {
                const std::size_t i = first + k;
                Vec3 accelerate = step.gravity;
                if (step.wind && step.drag_coefficient > 0.0F) {
                    accelerate = accelerate +
                                 ((step.wind(x[i]) - v[i]) * (step.drag_coefficient * kWindRate));
                }
                if (step.force) {
                    accelerate = accelerate + step.force(x[i]);
                }
                v[i] = v[i] + (accelerate * h);
                v[i] = v[i] * std::max<real_t>(0.0F, 1.0F - (step.damping * h));
                x[i] = x[i] + (v[i] * h);
                // Back toward the groomed shape, less so toward the tip.
                x[i] = x[i] + ((place[i] - x[i]) * pull[k]);
            }
            // Bends: each point and the one two along keep their groomed distance.
            for (std::size_t k = 2; k < n && bend > 0.0F; ++k) {
                const std::size_t i = first + k;
                hold(x[i - 2], x[i], k > 2, length(place[i] - place[i - 2]), bend);
            }
            // Lengths, follow-the-leader: each point at its segment's length from the one before.
            for (std::size_t k = 1; k < n; ++k) {
                const std::size_t i = first + k;
                const Vec3 d = x[i] - x[i - 1];
                const real_t at = length(d);
                const real_t rest = length(place[i] - place[i - 1]);
                const Vec3 to =
                    at > 0.0F ? x[i - 1] + (d * (rest / at)) : x[i - 1] + (place[i] - place[i - 1]);
                fixes[i] = to - x[i];
                x[i] = to;
            }
            for (std::size_t k = 1; k < n; ++k) {
                for (const StrandCollider& collider : step.colliders) {
                    keep_out(x[first + k], collider, step.point_radius);
                }
            }
            for (std::size_t k = 1; k < n; ++k) {
                const std::size_t i = first + k;
                v[i] = (x[i] - start[i]) * (1.0F / h);
                if (k + 1 < n) {
                    v[i] = v[i] - (fixes[i + 1] * (kLeaderDamping / h));
                }
            }
        }
    }
    strands.targets = targets;
}

std::vector<StrandCollider> strand_colliders_3d(const World& world, const Tree3D& tree,
                                                entt::entity root) {
    std::vector<StrandCollider> out;
    if (root == entt::null) {
        return out;
    }
    for (const auto [entity, own] : world.ecs().view<const CollisionShape3D>().each()) {
        const auto* shape = &own;
        if (shape->disabled || !under(tree, entity, root) ||
            on_character<CharacterBody3D>(world, tree, entity)) {
            continue;
        }
        Transform3D local;
        local.position = shape->position;
        local.rotation = shape->rotation;
        const Affine3D place = tree.global(entity) * affine(local);
        // The shape's scale, from its place's axes: spheres and capsules take the largest.
        const real_t scale =
            std::max({length(place.x), length(place.y), length(place.z), real_t{0.0F}});
        if (shape->shape == "sphere") {
            out.push_back(capsule(place.origin, place.origin, shape->radius * scale));
        } else if (shape->shape == "capsule") {
            const real_t half = std::max(0.0F, (shape->height * 0.5F) - shape->radius);
            out.push_back(capsule(place.apply({0.0F, -half, 0.0F}), place.apply({0.0F, half, 0.0F}),
                                  shape->radius * scale));
        } else if (shape->shape == "box") {
            out.push_back(box(place, shape->size * 0.5F));
        }
    }
    return out;
}

std::vector<StrandCollider> strand_colliders_2d(const World& world, const Tree2D& tree,
                                                entt::entity root) {
    std::vector<StrandCollider> out;
    if (root == entt::null) {
        return out;
    }
    for (const auto [entity, own] : world.ecs().view<const CollisionShape2D>().each()) {
        const auto* shape = &own;
        if (shape->disabled || !under(tree, entity, root) ||
            on_character<CharacterBody2D>(world, tree, entity)) {
            continue;
        }
        const Affine2D global = tree.global_affine(entity);
        Transform2D local_transform;
        local_transform.position = shape->position;
        local_transform.rotation = shape->rotation;
        const Affine2D local = affine(local_transform);
        const Affine2D place = global * local;
        const auto lift = [](Vec2 p) { return Vec3{p.x, p.y, 0.0F}; };
        const real_t scale = std::max(std::sqrt((place.x.x * place.x.x) + (place.x.y * place.x.y)),
                                      std::sqrt((place.y.x * place.y.x) + (place.y.y * place.y.y)));
        if (shape->shape == "circle") {
            const Vec3 at = lift(place.apply({}));
            out.push_back(capsule(at, at, shape->radius * scale));
        } else if (shape->shape == "capsule") {
            const real_t half = std::max(0.0F, (shape->height * 0.5F) - shape->radius);
            out.push_back(capsule(lift(place.apply({0.0F, -half})), lift(place.apply({0.0F, half})),
                                  shape->radius * scale));
        } else if (shape->shape == "rectangle") {
            Affine3D flat;
            flat.x = lift(place.x);
            flat.y = lift(place.y);
            flat.origin = lift(place.origin);
            // Deep along z, so points in the plane are inside it wherever x and y say.
            out.push_back(box(flat, {shape->size.x * 0.5F, shape->size.y * 0.5F, 1.0e6F}));
        }
    }
    return out;
}

entt::entity tree_root_3d(const Tree3D& tree, entt::entity entity) {
    entt::entity at = entity;
    while (at != entt::null && tree.parent(at) != entt::null) {
        at = tree.parent(at);
    }
    return at;
}

entt::entity tree_root_2d(const Tree2D& tree, entt::entity entity) {
    entt::entity at = entity;
    while (at != entt::null && tree.parent(at) != entt::null) {
        at = tree.parent(at);
    }
    return at;
}

void set_hair_source_3d(World& world, HairSource source) {
    world.ecs().ctx().insert_or_assign(HairSource3DHolder{std::move(source)});
}

void set_hair_source_2d(World& world, HairSource source) {
    world.ecs().ctx().insert_or_assign(HairSource2DHolder{std::move(source)});
}

const HairStrands* hair_strands_3d(const World& world, entt::entity entity) {
    return strands_from<HairSource3DHolder>(world, entity);
}

const HairStrands* hair_strands_2d(const World& world, entt::entity entity) {
    return strands_from<HairSource2DHolder>(world, entity);
}

std::uint32_t hair_hash(std::uint32_t value) {
    // lowbias32, Chris Wellons's integer hash.
    value ^= value >> 16U;
    value *= 0x7feb352dU;
    value ^= value >> 15U;
    value *= 0x846ca68bU;
    value ^= value >> 16U;
    return value;
}

float hair_unit(std::uint32_t hash) {
    return static_cast<float>(hash >> 8U) * (1.0F / 16777216.0F);
}

Vec4 HairMap::at(float u, float v) const {
    if (empty() || height == 0) {
        return {1.0F, 1.0F, 1.0F, 1.0F};
    }
    // The texel under the point, wrapping as a repeating texture does.
    const float fu = u - std::floor(u);
    const float fv = v - std::floor(v);
    const auto x = std::min(static_cast<std::uint32_t>(fu * static_cast<float>(width)), width - 1);
    const auto y =
        std::min(static_cast<std::uint32_t>(fv * static_cast<float>(height)), height - 1);
    const std::uint8_t* texel = pixels + ((static_cast<std::size_t>(y) * width + x) * 4);
    return {static_cast<float>(texel[0]) / 255.0F, static_cast<float>(texel[1]) / 255.0F,
            static_cast<float>(texel[2]) / 255.0F, static_cast<float>(texel[3]) / 255.0F};
}

HairGrowth2D grow_hair_2d(const Groom& groom, std::span<const Vec2> line, const HairMap& length_map,
                          const HairMap& density_map, const HairMap& color_map) {
    HairGrowth2D out;
    if (line.size() < 2) {
        return out;
    }
    std::vector<real_t> starts(line.size(), 0.0F);
    for (std::size_t i = 1; i < line.size(); ++i) {
        const Vec2 d = line[i] - line[i - 1];
        starts[i] = starts[i - 1] + std::sqrt((d.x * d.x) + (d.y * d.y));
    }
    const real_t total = starts.back();
    if (total <= 0.0F) {
        return out;
    }
    // Where a distance along the line is, and the line's normal there: its way turned a quarter
    // to the left on screen (up for a line drawn left to right).
    const auto at = [&](real_t along, Vec2& point, Vec2& normal) {
        std::size_t i = 1;
        while (i + 1 < line.size() && starts[i] < along) {
            ++i;
        }
        const Vec2 d = line[i] - line[i - 1];
        const real_t span = starts[i] - starts[i - 1];
        const real_t t =
            span > 0.0F ? std::clamp<real_t>((along - starts[i - 1]) / span, 0, 1) : 0.0F;
        point = line[i - 1] + (d * t);
        const real_t size = std::sqrt((d.x * d.x) + (d.y * d.y));
        normal = size > 0.0F ? Vec2{d.y / size, -d.x / size} : Vec2{0.0F, -1.0F};
    };
    const int segments = std::clamp(groom.segments, 1, 32);
    const Vec2 comb{groom.comb.x, groom.comb.y};
    const real_t comb_size = std::sqrt((comb.x * comb.x) + (comb.y * comb.y));
    const int guides = std::clamp(groom.guides, 1, 1024);
    for (int g = 0; g < guides; ++g) {
        HairGuide2D guide;
        guide.along = total * (static_cast<float>(g) + 0.5F) / static_cast<float>(guides);
        at(guide.along, guide.root, guide.normal);
        const auto u = static_cast<float>(guide.along / total);
        const real_t size = groom.length * length_map.at(u, 0.5F).x;
        guide.rest.push_back(guide.root);
        for (int k = 1; k <= segments; ++k) {
            const real_t turn = comb_size > 0.0F
                                    ? (1.0F - std::clamp(groom.lift, 0.0F, 1.0F)) *
                                          static_cast<float>(k) / static_cast<float>(segments)
                                    : 0.0F;
            Vec2 way = (guide.normal * (1.0F - turn)) +
                       (comb_size > 0.0F ? comb * (turn / comb_size) : Vec2{});
            const real_t way_size = std::sqrt((way.x * way.x) + (way.y * way.y));
            way = way_size > 0.0F ? way * (1.0F / way_size) : guide.normal;
            guide.rest.push_back(guide.rest.back() + (way * (size / static_cast<float>(segments))));
        }
        out.guides.push_back(std::move(guide));
    }
    const auto wanted = static_cast<std::size_t>(std::clamp<real_t>(
        std::round(groom.density * total), 0.0F, static_cast<float>(groom.max_strands)));
    for (std::size_t i = 0; i < wanted; ++i) {
        const std::uint32_t hash = hair_hash(groom.seed ^ hair_hash(static_cast<std::uint32_t>(i)));
        HairStrand2D strand;
        strand.along =
            total * (static_cast<float>(i) + hair_unit(hash)) / static_cast<float>(wanted);
        const auto u = static_cast<float>(strand.along / total);
        if (!density_map.empty() && hair_unit(hair_hash(hash + 1U)) >= density_map.at(u, 0.5F).x) {
            continue;
        }
        Vec2 normal;
        at(strand.along, strand.root, normal);
        strand.guide = static_cast<std::uint32_t>(
            std::clamp<real_t>(std::floor(strand.along / total * static_cast<float>(guides)), 0.0F,
                               static_cast<float>(guides - 1)));
        const HairGuide2D& guide = out.guides[strand.guide];
        const float guide_size = length_map.at(static_cast<float>(guide.along / total), 0.5F).x;
        const float own_size = length_map.at(u, 0.5F).x;
        if (own_size <= 0.0F || guide_size <= 0.0F) {
            continue;
        }
        strand.length =
            (own_size / guide_size) * (1.0F - (std::clamp(groom.length_variation, 0.0F, 1.0F) *
                                               hair_unit(hair_hash(hash + 2U))));
        strand.phase = 6.2831853F * hair_unit(hair_hash(hash + 3U));
        strand.clump = std::clamp(groom.clumping, 0.0F, 1.0F) *
                       (0.5F + (0.5F * hair_unit(hair_hash(hash + 4U))));
        strand.color = color_map.at(u, 0.5F);
        out.strands.push_back(strand);
    }
    return out;
}

void set_hair_image_reader(World& world, HairImageReader reader) {
    world.ecs().ctx().insert_or_assign(HairImageReaderHolder{std::move(reader)});
}

Result<HairImage> read_hair_image(const World& world, const std::string& path) {
    if (path.empty()) {
        return HairImage{};
    }
    const auto* holder = world.ecs().ctx().find<HairImageReaderHolder>();
    if (holder == nullptr || !holder->reader) {
        return make_error(ErrorCode::PlatformFailure, "images can't be read here");
    }
    auto image = holder->reader(path);
    if (!image) {
        return make_error(image.error().code,
                          "\"" + path + "\" can't be read: " + image.error().message);
    }
    return image;
}

Result<HairGrown2D> hair_growth_2d(const World& world, entt::entity entity) {
    const auto* hair = world.ecs().try_get<const Hair2D>(entity);
    if (hair == nullptr) {
        return make_error(ErrorCode::InvalidArgument, "has no Hair2D");
    }
    if (hair->groom.empty()) {
        return make_error(ErrorCode::InvalidArgument, "names no groom: set groom to a 2D "
                                                      ".groom.json file");
    }
    auto loaded = load_groom(world, hair->groom);
    if (!loaded) {
        return make_error(loaded.error().code,
                          "\"" + hair->groom + "\" can't be read: " + loaded.error().message);
    }
    const Groom& groom = *loaded.value().groom;
    if (groom.space != "2d") {
        return make_error(ErrorCode::InvalidArgument,
                          "\"" + hair->groom + R"(" is a 3D groom; a Hair2D needs space "2d")");
    }
    std::array<HairImage, 3> maps;
    const std::array<const std::string*, 3> paths{&groom.length_map, &groom.density_map,
                                                  &groom.color_map};
    std::string key = hair->groom + "@" + std::to_string(loaded.value().version);
    for (std::size_t i = 0; i < maps.size(); ++i) {
        auto image = read_hair_image(world, *paths.at(i));
        if (!image) {
            return image.error();
        }
        maps.at(i) = std::move(image).value();
        key += "|" + *paths.at(i) + "@" + std::to_string(maps.at(i).version);
    }
    for (const Vec2 point : hair->root_line) {
        key += "|" + std::to_string(point.x) + "," + std::to_string(point.y);
    }
    const auto* holder = world.ecs().ctx().find<GroomCacheHolder>();
    GroomCache* cache = holder != nullptr ? holder->get() : nullptr;
    std::unique_lock<std::mutex> lock;
    if (cache != nullptr) {
        lock = std::unique_lock<std::mutex>(cache->mutex);
        if (const auto found = cache->grown_2d.find(entity);
            found != cache->grown_2d.end() && found->second.key == key) {
            return found->second;
        }
    }
    HairGrown2D grown{.groom = loaded.value().groom,
                      .growth = std::make_shared<const HairGrowth2D>(grow_hair_2d(
                          groom, hair->root_line, maps[0].map(), maps[1].map(), maps[2].map())),
                      .key = std::move(key)};
    if (cache != nullptr) {
        cache->grown_2d[entity] = grown;
    }
    return grown;
}

std::shared_ptr<const void> kept_hair_3d(const World& world, std::string_view key) {
    const auto* holder = world.ecs().ctx().find<GroomCacheHolder>();
    if (holder == nullptr || key.empty()) {
        return nullptr;
    }
    const std::lock_guard lock((*holder)->mutex);
    const auto found = (*holder)->grown_3d.find(key);
    return found != (*holder)->grown_3d.end() ? found->second : nullptr;
}

void keep_hair_3d(const World& world, std::string key, std::shared_ptr<const void> growth) {
    const auto* holder = world.ecs().ctx().find<GroomCacheHolder>();
    if (holder == nullptr || key.empty()) {
        return;
    }
    GroomCache& cache = **holder;
    const std::lock_guard lock(cache.mutex);
    // Growths nothing else holds any more go first.
    std::erase_if(cache.grown_3d, [](const auto& entry) { return entry.second.use_count() <= 1; });
    cache.grown_3d[std::move(key)] = std::move(growth);
}

void follow_guide(std::span<const Vec3> guide, Vec3 offset, float share_of_guide, float clump,
                  real_t curl_radius, float curl_turns, float phase, Vec3 side, bool flat,
                  std::vector<Vec3>& out) {
    out.clear();
    if (guide.empty()) {
        return;
    }
    const std::size_t n = guide.size();
    const float share = std::clamp(share_of_guide, 0.0F, 1.0F);
    for (std::size_t k = 0; k < n; ++k) {
        const float t = n > 1 ? static_cast<float>(k) / static_cast<float>(n - 1) : 0.0F;
        // The guide at this strand's share of its length.
        const float at = t * share * static_cast<float>(n - 1);
        const auto i = std::min(static_cast<std::size_t>(at), n - 1);
        const std::size_t j = std::min(i + 1, n - 1);
        const float f = at - static_cast<float>(i);
        const Vec3 on = guide[i] + ((guide[j] - guide[i]) * f);
        Vec3 point = on + (offset * (1.0F - (clump * t)));
        if (curl_radius > 0.0F && n > 1) {
            // Across the guide's way here: the curl turns about it.
            const Vec3 way = guide[std::min(i + 1, n - 1)] - guide[i == j ? i - 1 : i];
            const real_t way_size = length(way);
            if (way_size > 0.0F) {
                const Vec3 forward = way * (1.0F / way_size);
                Vec3 across = side - (forward * dot(side, forward));
                const real_t across_size = length(across);
                if (across_size > 0.0F) {
                    across = across * (1.0F / across_size);
                    const Vec3 up{(forward.y * across.z) - (forward.z * across.y),
                                  (forward.z * across.x) - (forward.x * across.z),
                                  (forward.x * across.y) - (forward.y * across.x)};
                    const Vec2f turn =
                        slash::cos_sin(phase + (6.2831853F * curl_turns * t * share));
                    const float c = turn.x;
                    const float s = turn.y;
                    // Grows in over the first part of the strand, so the root stays put.
                    const real_t reach = curl_radius * std::min(1.0F, t * 4.0F);
                    point = point + (across * (c * reach));
                    if (!flat) {
                        point = point + (up * (s * reach));
                    }
                }
            }
        }
        out.push_back(point);
    }
}

} // namespace slash::world
