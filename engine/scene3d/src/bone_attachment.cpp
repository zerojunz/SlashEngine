#include "slash/scene3d/bone_attachment.hpp"

#include "slash/core/text.hpp"
#include "slash/world/components.hpp"

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace slash::scene3d {

entt::entity attachment_skeleton(const world::World& world, const world::Tree3D& tree,
                                 entt::entity attachment) {
    const auto& c = world.ecs().get<const world::BoneAttachment3D>(attachment);
    const entt::entity skeleton =
        c.external_skeleton != entt::null ? c.external_skeleton : tree.parent(attachment);
    return skeleton != entt::null && world.valid(skeleton) ? skeleton : entt::null;
}

void place_attachments(world::World& world, const ModelLoader& load) {
    auto& ecs = world.ecs();
    if (ecs.view<const world::BoneAttachment3D>().empty() || !load) {
        return;
    }
    const world::Tree3D tree(world);
    const Placer global = [&tree](entt::entity e) { return tree.global(e); };
    // Parents first, then any without a Transform3D in the order they were made.
    std::vector<entt::entity> attachments;
    for (const entt::entity entity : tree.order()) {
        if (ecs.all_of<world::BoneAttachment3D>(entity)) {
            attachments.push_back(entity);
        }
    }
    std::vector<entt::entity> rest;
    for (const entt::entity entity : ecs.view<const world::BoneAttachment3D>()) {
        if (!ecs.all_of<world::Transform3D>(entity)) {
            rest.push_back(entity);
        }
    }
    std::ranges::sort(rest, {}, [](entt::entity entity) { return entt::to_entity(entity); });
    attachments.insert(attachments.end(), rest.begin(), rest.end());

    // Each drawing entity's model and its nodes as posed, worked out once a tick.
    struct Posed {
        std::shared_ptr<const Model> model;
        std::vector<world::Affine3D> globals;
    };
    std::map<entt::entity, std::optional<Posed>> posed;
    const auto pose_for = [&](entt::entity skeleton) -> const Posed* {
        auto [at, added] = posed.try_emplace(skeleton);
        if (!added) {
            return at->second ? &*at->second : nullptr;
        }
        const auto* instance = ecs.try_get<const world::MeshInstance3D>(skeleton);
        if (instance == nullptr || instance->mesh.empty()) {
            return nullptr;
        }
        auto model = load(instance->mesh);
        if (!model || model.value()->nodes.empty()) {
            return nullptr;
        }
        const Model& m = *model.value();
        std::vector<NodePose> pose = pose_of(m, clips(world, skeleton),
                                             ecs.try_get<const world::Skeleton3D>(skeleton), load);
        apply_modifiers(world, skeleton, m, tree.global(skeleton), global, pose);
        at->second = Posed{.model = model.value(), .globals = node_globals(m, pose)};
        return &*at->second;
    };

    for (const entt::entity entity : attachments) {
        const entt::entity skeleton = attachment_skeleton(world, tree, entity);
        const Posed* found = skeleton != entt::null ? pose_for(skeleton) : nullptr;
        if (found == nullptr) {
            continue;
        }
        const int bone =
            find_node(*found->model, ecs.get<const world::BoneAttachment3D>(entity).bone_name);
        if (bone < 0) {
            continue;
        }
        const world::Affine3D target =
            tree.global(skeleton) * found->globals[static_cast<std::size_t>(bone)];
        const entt::entity up = tree.transform_parent(entity);
        const NodePose local =
            decompose(up == entt::null ? target : tree.global(up).inverse() * target);
        const Vec3 rotation = world::euler(world::rotation_basis(local.rotation));
        auto& transform = ecs.get_or_emplace<world::Transform3D>(entity);
        if (transform.position != local.position) {
            transform.position = local.position;
        }
        if (transform.rotation != rotation) {
            transform.rotation = rotation;
        }
        if (transform.scale != local.scale) {
            transform.scale = local.scale;
        }
    }
}

std::vector<ModifierProblem>
check_attachments(const world::World& world,
                  const std::function<const Model*(entt::entity)>& model_of) {
    std::vector<ModifierProblem> out;
    const auto& ecs = world.ecs();
    std::vector<entt::entity> attachments(ecs.view<const world::BoneAttachment3D>().begin(),
                                          ecs.view<const world::BoneAttachment3D>().end());
    // In the order entities were made, so reports read the same each time.
    std::ranges::sort(attachments, {}, [](entt::entity entity) { return entt::to_entity(entity); });
    const world::Tree3D tree(world);
    for (const entt::entity entity : attachments) {
        const auto& c = ecs.get<const world::BoneAttachment3D>(entity);
        const entt::entity skeleton = attachment_skeleton(world, tree, entity);
        const auto* instance =
            skeleton != entt::null ? ecs.try_get<const world::MeshInstance3D>(skeleton) : nullptr;
        if (instance == nullptr || instance->mesh.empty()) {
            out.push_back(
                {entity,
                 c.external_skeleton != entt::null
                     ? "/components/BoneAttachment3D/external_skeleton"
                     : "/components/BoneAttachment3D",
                 c.external_skeleton != entt::null
                     ? "follows an entity that draws no model file: name the entity whose "
                       "MeshInstance3D draws the model"
                     : "follows a bone, but its Transform3D parent draws no model file: put it "
                       "in the Transform3D children of the MeshInstance3D's entity, or name "
                       "that entity in external_skeleton"});
            continue;
        }
        const Model* model = model_of ? model_of(skeleton) : nullptr;
        if (model == nullptr) {
            continue;
        }
        const std::string path = "/components/BoneAttachment3D/bone_name";
        if (c.bone_name.empty()) {
            out.push_back({entity, path, "names no bone"});
            continue;
        }
        if (find_node(*model, c.bone_name) >= 0) {
            continue;
        }
        std::vector<std::string_view> names;
        names.reserve(model->nodes.size());
        for (const ModelNode& node : model->nodes) {
            names.emplace_back(node.name);
        }
        std::string message =
            "\"" + instance->mesh + "\" has no bone or node named \"" + c.bone_name + "\"";
        if (const std::string_view hint = closest_match(c.bone_name, names); !hint.empty()) {
            message += "; did you mean \"" + std::string(hint) + "\"?";
        }
        out.push_back({entity, path, std::move(message)});
    }
    return out;
}

} // namespace slash::scene3d
