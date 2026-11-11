// Flesh on a character's 3D model (slash/scene3d/flesh.hpp).
#include "slash/scene3d/flesh.hpp"

#include "slash/asset/assets.hpp"
#include "slash/scene3d/ik.hpp"
#include "slash/world/tree3d.hpp"

#include <algorithm>
#include <map>
#include <string_view>

namespace slash::scene3d {

namespace {

using world::Affine3D;

// What part of the body a rig's bone moves.
world::PartSide part_of_bone(std::string_view name) {
    using world::BodyPart;
    int side = -1;
    if (name.starts_with("Left")) {
        side = 0;
        name.remove_prefix(4);
    } else if (name.starts_with("Right")) {
        side = 1;
        name.remove_prefix(5);
    }
    const auto in_middle = [](BodyPart part) { return world::PartSide{.part = part, .side = -1}; };
    if (side < 0) {
        if (name == "Hips") {
            return in_middle(BodyPart::Pelvis);
        }
        if (name == "Spine") {
            return in_middle(BodyPart::Abdomen);
        }
        if (name == "Neck") {
            return in_middle(BodyPart::Neck);
        }
        if (name == "Head") {
            return in_middle(BodyPart::Head);
        }
        if (name == "Jaw") {
            return in_middle(BodyPart::Jaw);
        }
        return in_middle(BodyPart::Chest);
    }
    const std::array<std::pair<std::string_view, BodyPart>, 13> limbs{{
        {"Shoulder", BodyPart::Clavicle},
        {"UpperArm", BodyPart::UpperArm},
        {"LowerArm", BodyPart::LowerArm},
        {"Hand", BodyPart::Hand},
        {"Thumb", BodyPart::Hand},
        {"Index", BodyPart::Hand},
        {"Middle", BodyPart::Hand},
        {"Ring", BodyPart::Hand},
        {"Little", BodyPart::Hand},
        {"UpperLeg", BodyPart::UpperLeg},
        {"LowerLeg", BodyPart::LowerLeg},
        {"Foot", BodyPart::Foot},
        {"Toes", BodyPart::Foot},
    }};
    for (const auto& [prefix, part] : limbs) {
        if (name.starts_with(prefix)) {
            return {.part = part, .side = side};
        }
    }
    return in_middle(name == "Eye" ? BodyPart::Head : BodyPart::Chest);
}

struct BoundSurface {
    std::size_t part = 0;
    std::size_t surface = 0;
    world::FleshBinding binding;
};

struct Flesh {
    std::string path;
    std::shared_ptr<const Model> model;
    std::shared_ptr<const world::CharacterRecipe> recipe;
    world::Anatomy anatomy;
    world::FleshBody body;
    world::FleshState state;
    std::vector<int> nodes; // each of body.bones' node in the model
    std::vector<Affine3D> rests;
    std::vector<BoundSurface> surfaces;
    std::array<Vec3, 3> root{};
    int seen = 0;
    std::shared_ptr<const FleshPose> pose;
};

struct FleshWorld {
    std::map<entt::entity, Flesh> bodies;
};
using FleshHolder = std::shared_ptr<FleshWorld>;

void build(Flesh& flesh) {
    const Model& model = *flesh.model;
    const world::CharacterRecipe& recipe = *flesh.recipe;
    flesh.anatomy =
        world::make_anatomy(recipe, world::body_frames_3d(world::character_measures(recipe)));
    flesh.body = world::build_flesh(flesh.anatomy);
    flesh.state = {};
    const std::vector<Affine3D> rests = node_globals(model, rest_pose(model));
    flesh.nodes.clear();
    flesh.rests.clear();
    for (const std::string& bone : flesh.body.bones) {
        const int node = find_node(model, bone);
        flesh.nodes.push_back(node);
        flesh.rests.push_back(node >= 0 ? rests[static_cast<std::size_t>(node)] : Affine3D{});
    }
    // A form's own surfaces (::muscles) have no fat over them.
    const bool form = !asset::subresource_of(flesh.path).empty();
    const float fat = form ? 0.0F : recipe.body.fat;
    flesh.surfaces.clear();
    for (std::size_t p = 0; p < model.parts.size(); ++p) {
        const ModelPart& part = model.parts[p];
        if (part.skin < 0) {
            continue;
        }
        const ModelSkin& skin = model.skins[static_cast<std::size_t>(part.skin)];
        const ModelMesh& mesh = model.meshes[part.mesh];
        for (std::size_t i = 0; i < mesh.surfaces.size(); ++i) {
            const ModelSurface& surface = mesh.surfaces[i];
            if (surface.weights.size() != surface.vertices.size()) {
                continue;
            }
            std::vector<world::SkinPoint> points;
            points.reserve(surface.vertices.size());
            for (std::size_t v = 0; v < surface.vertices.size(); ++v) {
                const auto& at = surface.vertices[v].position;
                const std::size_t joint = surface.weights[v].joints[0];
                const std::size_t node = joint < skin.joints.size() ? skin.joints[joint] : 0;
                const std::string_view name =
                    node < model.nodes.size() ? std::string_view(model.nodes[node].name) : "";
                points.push_back({.rest = {at[0], at[1], at[2]}, .part = part_of_bone(name)});
            }
            world::FleshBinding binding = world::bind_skin(flesh.body, points, fat);
            if (!binding.entries.empty()) {
                flesh.surfaces.push_back({.part = p, .surface = i, .binding = std::move(binding)});
            }
        }
    }
}

void run(const world::World& world, entt::entity entity, Flesh& flesh, const world::Tree3D& tree,
         const ModelLoader& load, real_t seconds) {
    const Model& model = *flesh.model;
    std::vector<NodePose> pose = pose_of(
        model, clips(world, entity), world.ecs().try_get<const world::Skeleton3D>(entity), load);
    const Placer global = [&tree](entt::entity e) { return tree.global(e); };
    const Affine3D place = tree.global(entity);
    apply_modifiers(world, entity, model, place, global, pose);
    const std::vector<Affine3D> globals = node_globals(model, pose);
    std::vector<Affine3D> moves(flesh.nodes.size());
    for (std::size_t i = 0; i < flesh.nodes.size(); ++i) {
        const int node = flesh.nodes[i];
        if (node >= 0 && static_cast<std::size_t>(node) < globals.size()) {
            moves[i] = globals[static_cast<std::size_t>(node)] * flesh.rests[i].inverse();
        }
    }
    // The root's acceleration, felt the other way in the model's space.
    flesh.root[2] = flesh.root[1];
    flesh.root[1] = flesh.root[0];
    flesh.root[0] = place.origin;
    Vec3 push;
    if (flesh.seen >= 2 && seconds > 0.0F) {
        const Vec3 a = (flesh.root[0] - flesh.root[1] - (flesh.root[1] - flesh.root[2])) *
                       (1.0F / (seconds * seconds));
        Affine3D turn = place.inverse();
        turn.origin = {};
        push = turn.apply(a) * -1.0F;
    }
    flesh.seen = std::min(flesh.seen + 1, 2);
    const auto& settings = world.ecs().get<const world::Flesh3D>(entity);
    world::step_flesh(flesh.body, flesh.state,
                      {.moves = moves,
                       .acceleration = push,
                       .seconds = seconds,
                       .settings = world::flesh_settings(settings)});
    auto next = std::make_shared<FleshPose>();
    for (const BoundSurface& bound : flesh.surfaces) {
        FleshPose::Surface surface;
        surface.part = bound.part;
        surface.surface = bound.surface;
        world::skin_offsets(flesh.state, bound.binding, surface.now);
        if (flesh.pose != nullptr) {
            for (const FleshPose::Surface& was : flesh.pose->surfaces) {
                if (was.part == bound.part && was.surface == bound.surface) {
                    surface.before = was.now;
                }
            }
        }
        if (surface.before.size() != surface.now.size()) {
            surface.before = surface.now;
        }
        next->surfaces.push_back(std::move(surface));
    }
    flesh.pose = std::move(next);
}

} // namespace

void install_flesh(world::World& world, ModelLoader load) {
    if (world.ecs().ctx().find<FleshHolder>() != nullptr) {
        return;
    }
    auto held = std::make_shared<FleshWorld>();
    world.ecs().ctx().emplace<FleshHolder>(held);
    world.add_reset([](world::World& w) {
        if (auto* found = w.ecs().ctx().find<FleshHolder>(); found != nullptr) {
            (*found)->bodies.clear();
        }
    });
    world.add_system(
        "flesh_3d",
        [held, load = std::move(load)](world::World& w, double seconds) {
            std::vector<entt::entity> characters;
            for (const entt::entity entity :
                 w.ecs().view<const world::Flesh3D, const world::MeshInstance3D>()) {
                const auto& mesh = w.ecs().get<const world::MeshInstance3D>(entity).mesh;
                if (w.ecs().get<const world::Flesh3D>(entity).enabled &&
                    world::is_character_path(asset::file_of(mesh))) {
                    characters.push_back(entity);
                }
            }
            std::erase_if(held->bodies, [&](const auto& e) {
                return std::ranges::find(characters, e.first) == characters.end();
            });
            if (characters.empty()) {
                return;
            }
            const world::Tree3D tree(w);
            for (const entt::entity entity : characters) {
                const std::string& path = w.ecs().get<const world::MeshInstance3D>(entity).mesh;
                auto model = load(path);
                auto recipe = world::load_character(w, asset::file_of(path));
                if (!model || !recipe) {
                    held->bodies.erase(entity);
                    continue;
                }
                Flesh& flesh = held->bodies[entity];
                if (flesh.path != path || flesh.model != model.value() ||
                    flesh.recipe != recipe.value()) {
                    flesh = {};
                    flesh.path = path;
                    flesh.model = model.value();
                    flesh.recipe = recipe.value();
                    build(flesh);
                }
                run(w, entity, flesh, tree, load, static_cast<real_t>(seconds));
            }
        },
        {.doc = "Steps each Flesh3D's muscles from where its character model's posed bones are, "
                "for the renderer to move its skin by.",
         .priority = 957});
}

std::shared_ptr<const FleshPose> flesh_pose(const world::World& world, entt::entity entity) {
    const auto* held = world.ecs().ctx().find<FleshHolder>();
    if (held == nullptr) {
        return nullptr;
    }
    const auto found = (*held)->bodies.find(entity);
    return found == (*held)->bodies.end() ? nullptr : found->second.pose;
}

world::FleshView flesh_3d(const world::World& world, entt::entity entity) {
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

} // namespace slash::scene3d
