// Flesh on a Character2D's cut-out (slash/world/flesh.hpp): the flesh_2d system steps its
// muscles from where its bones' entities are and moves their Polygon2D points as they are drawn.
#include "slash/world/flesh.hpp"
#include "slash/world/tree2d.hpp"

#include <algorithm>
#include <map>
#include <memory>
#include <numbers>

namespace slash::world {

namespace {

constexpr std::string_view kAnatomySuffix = "_anatomy";

std::string part_entity_id(std::string_view owner, std::string_view bone) {
    std::string id(owner);
    id += '_';
    id += bone;
    if (id.size() > 64) {
        id.resize(64);
    }
    return id;
}

// What part of the body a cut-out bone's shapes are on.
PartSide part_of_cutout(std::string_view bone) {
    int side = -1;
    if (bone.ends_with("_front")) {
        side = 1;
    } else if (bone.ends_with("_back")) {
        side = 0;
    }
    const std::array<std::pair<std::string_view, BodyPart>, 13> names{{
        {"pelvis", BodyPart::Pelvis},
        {"chest", BodyPart::Chest},
        {"head", BodyPart::Head},
        {"eye", BodyPart::Head},
        {"jaw", BodyPart::Jaw},
        {"upper_arm", BodyPart::UpperArm},
        {"lower_arm", BodyPart::LowerArm},
        {"hand", BodyPart::Hand},
        {"fingers", BodyPart::Hand},
        {"index", BodyPart::Hand},
        {"thumb", BodyPart::Hand},
        {"upper_leg", BodyPart::UpperLeg},
        {"lower_leg", BodyPart::LowerLeg},
    }};
    for (const auto& [name, part] : names) {
        if (bone.starts_with(name)) {
            return {.part = part, .side = is_sided(part) ? side : -1};
        }
    }
    if (bone.starts_with("foot")) {
        return {.part = BodyPart::Foot, .side = side};
    }
    return {.part = BodyPart::Chest, .side = -1};
}

Affine3D to_3d(const Affine2D& a) {
    Affine3D out;
    out.x = {a.x.x, a.x.y, 0.0F};
    out.y = {a.y.x, a.y.y, 0.0F};
    out.z = {0.0F, 0.0F, 1.0F};
    out.origin = {a.origin.x, a.origin.y, 0.0F};
    return out;
}

// A point of the anatomy view: a muscle's flesh point, or a bone's point carried by its bone.
struct ViewPoint {
    int flesh = -1;
    int bone = -1; // into Flesh::bones
    Vec3 rest;
};

// A part's skin bound to flesh: the entity whose Polygon2D it is and its points' binding.
struct BoundPart {
    std::string bone;
    FleshBinding binding;
};

struct Flesh {
    std::string key;
    std::shared_ptr<const CharacterRecipe> recipe;
    Anatomy anatomy;
    FleshBody body;
    FleshState state;
    std::vector<std::string> bones; // every cut-out bone, with its rest place in meters
    std::vector<Vec2> rests;
    std::vector<int> body_bones; // FleshBody::bones into bones
    std::vector<BoundPart> parts;
    std::vector<ViewPoint> view_points;
    std::array<Vec2, 3> root{}; // the root's place this tick and the two before
    int seen = 0;
};

struct FleshWorld {
    std::map<entt::entity, Flesh> bodies;
    std::map<entt::entity, std::vector<Vec2>> offsets;
};
using FleshHolder = std::shared_ptr<FleshWorld>;

// A capsule's outline, sides of eight round each end.
std::vector<Vec3> capsule_outline(const AnatomyPiece& piece) {
    std::vector<Vec3> out;
    const Vec3 a = piece.a;
    const Vec3 b = piece.b;
    Vec2 along{b.x - a.x, b.y - a.y};
    const real_t span = std::sqrt((along.x * along.x) + (along.y * along.y));
    along = span > 0.0F ? along * (1.0F / span) : Vec2{0.0F, 1.0F};
    const real_t start = slash::atan2(static_cast<float>(along.y), static_cast<float>(along.x));
    constexpr int kSides = 8;
    for (const int end : {1, 0}) {
        const Vec3 c = end == 1 ? b : a;
        for (int i = 0; i <= kSides; ++i) {
            const real_t angle = start - (std::numbers::pi_v<real_t> * 0.5F) +
                                 (std::numbers::pi_v<real_t> * static_cast<real_t>(i) / kSides) +
                                 (end == 0 ? std::numbers::pi_v<real_t> : 0.0F);
            const Vec2f cs = cos_sin(static_cast<float>(angle));
            out.push_back({c.x + (cs.x * piece.radius), c.y + (cs.y * piece.radius), 0.0F});
        }
    }
    return out;
}

void build(World& world, entt::entity entity, Flesh& flesh, const Character2D& character,
           const Flesh2D& settings) {
    const CharacterRecipe& recipe = *flesh.recipe;
    const BodyFrames frames = body_frames_2d(character_measures(recipe));
    flesh.anatomy = make_anatomy(recipe, frames);
    flesh.body = build_flesh(flesh.anatomy);
    flesh.state = {};
    const std::vector<CutoutBone> cutout = character_cutout(recipe);
    flesh.bones.clear();
    flesh.rests.clear();
    for (const CutoutBone& bone : cutout) {
        const Vec2 parent =
            bone.parent < 0 ? Vec2{} : flesh.rests[static_cast<std::size_t>(bone.parent)];
        flesh.bones.push_back(bone.name);
        flesh.rests.push_back(parent + bone.position);
    }
    const auto slot = [&](const std::string& name) {
        const auto found = std::ranges::find(flesh.bones, name);
        return found == flesh.bones.end() ? -1 : static_cast<int>(found - flesh.bones.begin());
    };
    flesh.body_bones.clear();
    for (const std::string& name : flesh.body.bones) {
        flesh.body_bones.push_back(slot(name));
    }
    // Each part's skin, bound where its shapes are at rest.
    flesh.parts.clear();
    for (std::size_t i = 0; i < cutout.size(); ++i) {
        std::vector<SkinPoint> skin;
        const PartSide part = part_of_cutout(cutout[i].name);
        for (const CutoutShape& shape : cutout[i].shapes) {
            for (const Vec2 point : shape.points) {
                const Vec2 at = flesh.rests[i] + point;
                skin.push_back({.rest = {at.x, at.y, 0.0F}, .part = part});
            }
        }
        flesh.parts.push_back(
            {.bone = cutout[i].name, .binding = bind_skin(flesh.body, skin, recipe.body.fat)});
    }
    // The anatomy view: far side, the middle, then the near side; bones under muscles.
    flesh.view_points.clear();
    const std::string owner(world.id_of(entity));
    const entt::entity view = world.find(owner + std::string(kAnatomySuffix));
    if (settings.view == "skin") {
        if (view != entt::null && world.ecs().all_of<Polygon2D>(view)) {
            world.ecs().remove<Polygon2D>(view);
        }
        return;
    }
    Polygon2D polygon;
    polygon.z_index = 3;
    polygon.z_as_relative = true;
    const real_t ppm = character.pixels_per_meter;
    const real_t facing = character.flip_h ? -1.0F : 1.0F;
    const auto add_outline = [&](const std::vector<ViewPoint>& points, Vec4 color) {
        std::vector<int> indices;
        for (const ViewPoint& p : points) {
            indices.push_back(static_cast<int>(polygon.polygon.size()));
            polygon.polygon.push_back({p.rest.x * ppm * facing, -p.rest.y * ppm});
            polygon.vertex_colors.push_back(color);
            flesh.view_points.push_back(p);
        }
        polygon.polygons.push_back(std::move(indices));
    };
    // Far side first, then the middle, then the near side.
    const auto layer = [](int side) {
        if (side < 0) {
            return 1;
        }
        return side == 0 ? 0 : 2;
    };
    for (int at = 0; at < 3; ++at) {
        for (const AnatomyBone& bone : flesh.anatomy.bones) {
            if (layer(bone.side) != at) {
                continue;
            }
            const int moved_by = slot(bone.bone);
            for (const AnatomyPiece& piece : bone.pieces) {
                std::vector<ViewPoint> points;
                for (const Vec3 p : capsule_outline(piece)) {
                    points.push_back({.flesh = -1, .bone = moved_by, .rest = p});
                }
                const float shade = at == 0 ? 0.8F : 1.0F;
                add_outline(points, {0.92F * shade, 0.89F * shade, 0.8F * shade, 1.0F});
            }
        }
        if (settings.view != "muscles") {
            continue;
        }
        for (const FleshMuscle& m : flesh.body.muscles) {
            const bool middle = !is_sided(m.origin.part) && !is_sided(m.insertion.part);
            int side = m.origin.side >= 0 ? m.origin.side : m.insertion.side;
            if (middle) {
                side = -1;
            }
            if (layer(side) != at) {
                continue;
            }
            std::vector<ViewPoint> points;
            for (std::size_t k = 0; k < m.rings; ++k) {
                const auto i = static_cast<int>(m.first_point + (k * m.ring_points) + 1);
                points.push_back({.flesh = i,
                                  .bone = -1,
                                  .rest = flesh.body.points[static_cast<std::size_t>(i)].rest});
            }
            for (std::size_t k = m.rings; k-- > 0;) {
                const auto i = static_cast<int>(m.first_point + (k * m.ring_points) + 2);
                points.push_back({.flesh = i,
                                  .bone = -1,
                                  .rest = flesh.body.points[static_cast<std::size_t>(i)].rest});
            }
            const float shade = at == 0 ? 0.75F : 1.0F;
            add_outline(points, {0.72F * shade, 0.2F * shade, 0.18F * shade, 1.0F});
        }
    }
    entt::entity shown = view;
    if (shown == entt::null) {
        auto made = world.create(owner + std::string(kAnatomySuffix));
        if (!made) {
            return;
        }
        shown = made.value();
        world.ecs().emplace<Transform2D>(shown);
        if (auto* parent = world.ecs().try_get<Transform2D>(entity); parent != nullptr) {
            parent->children.push_back(shown);
        }
    }
    world.ecs().emplace_or_replace<Polygon2D>(shown, std::move(polygon));
}

std::string key_of(const Character2D& character, const Flesh2D& settings) {
    return character.recipe + "/" + std::to_string(character.pixels_per_meter) +
           (character.flip_h ? "/flip" : "") + "/" + settings.view;
}

void run(World& world, entt::entity entity, Flesh& flesh, FleshWorld& all, const Tree2D& tree,
         real_t seconds) {
    const Character2D& character = world.ecs().get<const Character2D>(entity);
    const Flesh2D& settings = world.ecs().get<const Flesh2D>(entity);
    const real_t k = character.pixels_per_meter;
    const real_t facing = character.flip_h ? -1.0F : 1.0F;
    const std::string owner(world.id_of(entity));
    const Affine2D root = tree.global_affine(entity);
    const Affine2D from_root = root.inverse();
    // Each cut-out bone's move from its rest, in meters in the character's space.
    Affine3D to_pixels;
    to_pixels.x = {k * facing, 0.0F, 0.0F};
    to_pixels.y = {0.0F, -k, 0.0F};
    Affine3D to_meters;
    to_meters.x = {1.0F / (k * facing), 0.0F, 0.0F};
    to_meters.y = {0.0F, -1.0F / k, 0.0F};
    std::vector<Affine3D> moves(flesh.bones.size());
    std::vector<Affine2D> locals(flesh.bones.size());
    for (std::size_t i = 0; i < flesh.bones.size(); ++i) {
        const entt::entity part = world.find(part_entity_id(owner, flesh.bones[i]));
        if (part == entt::null || !world.ecs().all_of<Transform2D>(part)) {
            continue;
        }
        locals[i] = from_root * tree.global_affine(part);
        Affine2D rest;
        rest.origin = {flesh.rests[i].x * k * facing, -flesh.rests[i].y * k};
        moves[i] = to_meters * to_3d(locals[i] * rest.inverse()) * to_pixels;
    }
    std::vector<Affine3D> body_moves;
    body_moves.reserve(flesh.body_bones.size());
    for (const int slot : flesh.body_bones) {
        body_moves.push_back(slot >= 0 ? moves[static_cast<std::size_t>(slot)] : Affine3D{});
    }
    // The root's acceleration, felt the other way in its own space.
    flesh.root[2] = flesh.root[1];
    flesh.root[1] = flesh.root[0];
    flesh.root[0] = root.origin;
    Vec3 push;
    if (flesh.seen >= 2 && seconds > 0.0F) {
        const Vec2 a = (flesh.root[0] - flesh.root[1] - (flesh.root[1] - flesh.root[2])) *
                       (1.0F / (seconds * seconds));
        Affine2D turn = from_root;
        turn.origin = {};
        const Vec2 local = turn.apply(a);
        push = {-local.x / (k * facing), local.y / k, 0.0F};
    }
    flesh.seen = std::min(flesh.seen + 1, 2);
    step_flesh(flesh.body, flesh.state,
               {.moves = body_moves,
                .acceleration = push,
                .seconds = seconds,
                .settings = flesh_settings(settings)});
    // Each part's points moved, in its own space.
    std::vector<Vec3> offsets;
    for (std::size_t i = 0; i < flesh.parts.size(); ++i) {
        const entt::entity part = world.find(part_entity_id(owner, flesh.parts[i].bone));
        if (part == entt::null) {
            continue;
        }
        skin_offsets(flesh.state, flesh.parts[i].binding, offsets);
        Affine2D turn = locals[i].inverse();
        turn.origin = {};
        std::vector<Vec2>& out = all.offsets[part];
        out.resize(offsets.size());
        for (std::size_t p = 0; p < offsets.size(); ++p) {
            out[p] = turn.apply({offsets[p].x * k * facing, -offsets[p].y * k});
        }
    }
    if (!flesh.view_points.empty()) {
        const entt::entity view = world.find(owner + std::string(kAnatomySuffix));
        if (view != entt::null) {
            std::vector<Vec2>& out = all.offsets[view];
            out.resize(flesh.view_points.size());
            for (std::size_t p = 0; p < flesh.view_points.size(); ++p) {
                const ViewPoint& at = flesh.view_points[p];
                Vec3 moved = at.rest;
                if (at.flesh >= 0 &&
                    static_cast<std::size_t>(at.flesh) < flesh.state.positions.size()) {
                    moved = flesh.state.positions[static_cast<std::size_t>(at.flesh)];
                } else if (at.bone >= 0) {
                    moved = moves[static_cast<std::size_t>(at.bone)].apply(at.rest);
                }
                const Vec3 d = moved - at.rest;
                out[p] = {d.x * k * facing, -d.y * k};
            }
        }
    }
}

} // namespace

void add_flesh_2d_system(World& world) {
    if (world.ecs().ctx().find<FleshHolder>() != nullptr) {
        return;
    }
    auto held = std::make_shared<FleshWorld>();
    world.ecs().ctx().emplace<FleshHolder>(held);
    world.add_reset([](World& w) {
        if (auto* found = w.ecs().ctx().find<FleshHolder>(); found != nullptr) {
            (*found)->bodies.clear();
            (*found)->offsets.clear();
        }
    });
    world.add_system(
        "flesh_2d",
        [held](World& w, double seconds) {
            std::vector<entt::entity> characters;
            for (const entt::entity entity : w.ecs().view<const Character2D, const Flesh2D>()) {
                if (w.ecs().get<const Flesh2D>(entity).enabled) {
                    characters.push_back(entity);
                }
            }
            std::erase_if(held->bodies, [&](const auto& e) {
                return std::ranges::find(characters, e.first) == characters.end();
            });
            held->offsets.clear();
            if (characters.empty()) {
                return;
            }
            const Tree2D tree(w);
            for (const entt::entity entity : characters) {
                const Character2D& character = w.ecs().get<const Character2D>(entity);
                const Flesh2D& settings = w.ecs().get<const Flesh2D>(entity);
                auto recipe = load_character(w, character.recipe);
                if (!recipe) {
                    continue;
                }
                Flesh& flesh = held->bodies[entity];
                const std::string key = key_of(character, settings);
                if (flesh.key != key || flesh.recipe != recipe.value()) {
                    flesh = {};
                    flesh.key = key;
                    flesh.recipe = recipe.value();
                    build(w, entity, flesh, character, settings);
                }
                run(w, entity, flesh, *held, tree, static_cast<real_t>(seconds));
            }
        },
        {.doc = "Steps each Character2D's Flesh2D: its muscles from where its cut-out's bones "
                "are, moving its parts' points as they are drawn.",
         .priority = 957});
}

const std::vector<Vec2>* flesh_offsets_2d(const World& world, entt::entity entity) {
    const auto* held = world.ecs().ctx().find<FleshHolder>();
    if (held == nullptr) {
        return nullptr;
    }
    const auto found = (*held)->offsets.find(entity);
    return found == (*held)->offsets.end() ? nullptr : &found->second;
}

FleshView flesh_2d(const World& world, entt::entity entity) {
    const auto* held = world.ecs().ctx().find<FleshHolder>();
    if (held == nullptr) {
        return {};
    }
    const auto found = (*held)->bodies.find(entity);
    if (found == (*held)->bodies.end()) {
        return {};
    }
    return {.body = &found->second.body,
            .state = &found->second.state,
            .anatomy = &found->second.anatomy};
}

} // namespace slash::world
