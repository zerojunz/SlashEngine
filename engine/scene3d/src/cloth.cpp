// Character cloth's model side, as Unreal's Chaos Cloth lays out a skeletal mesh's clothing
// section: the surfaces it names, their skin weights and where the skin puts them.
#include "slash/scene3d/cloth.hpp"

#include "slash/asset/assets.hpp"
#include "slash/core/text.hpp"
#include "slash/scene3d/ik.hpp"
#include "slash/scene3d/skeleton.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <string_view>

namespace slash::scene3d {

namespace {

constexpr std::string_view kSectionField = "/components/Cloth3D/section";

// The model an entity's MeshInstance3D draws and its version, through the world's assets, else
// read with its file reader.
struct Loaded {
    std::shared_ptr<const Model> model;
    std::uint64_t version = 0;
};

Result<Loaded> model_of(const world::World& world, const std::string& path) {
    if (asset::Assets* assets = asset::assets_of(world.ecs()); assets != nullptr) {
        auto found = assets->load<Model>(path);
        if (!found) {
            return found.error();
        }
        return Loaded{.model = found.value().share(), .version = found.value().version()};
    }
    const world::FileReader& reader = world.file_reader();
    if (!reader) {
        return make_error(ErrorCode::PlatformFailure, "the world has no files to read models from");
    }
    auto bytes = reader(std::string(asset::file_of(path)));
    if (!bytes) {
        return bytes.error();
    }
    auto model = load_model(bytes.value(), path, reader);
    if (!model) {
        return model.error();
    }
    return Loaded{.model = std::make_shared<const Model>(std::move(model).value())};
}

ModelLoader loader_of(const world::World& world) {
    return [&world](const std::string& path) -> Result<std::shared_ptr<const Model>> {
        auto loaded = model_of(world, path);
        if (!loaded) {
            return loaded.error();
        }
        return loaded.value().model;
    };
}

const std::string* model_path(const world::World& world, entt::entity entity) {
    const auto* instance = world.ecs().try_get<const world::MeshInstance3D>(entity);
    if (instance == nullptr || instance->mesh.empty() || !is_model_path(instance->mesh)) {
        return nullptr;
    }
    return &instance->mesh;
}

Vec3 to_vec(const std::array<float, 3>& value) {
    return {value[0], value[1], value[2]};
}

real_t dot(Vec3 a, Vec3 b) {
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

} // namespace

std::string cloth_key(const world::World& world, entt::entity entity) {
    const auto* cloth = world.ecs().try_get<const world::Cloth3D>(entity);
    const std::string* path = model_path(world, entity);
    if (cloth == nullptr || path == nullptr) {
        return {};
    }
    std::uint64_t version = 0;
    if (asset::Assets* assets = asset::assets_of(world.ecs()); assets != nullptr) {
        auto found = assets->load<Model>(*path);
        if (!found) {
            return {};
        }
        version = found.value().version();
    }
    return *path + "@" + std::to_string(version) + "#" + cloth->section;
}

Result<std::shared_ptr<const ClothSection>> cloth_section(const world::World& world,
                                                          entt::entity entity) {
    const auto* cloth = world.ecs().try_get<const world::Cloth3D>(entity);
    if (cloth == nullptr) {
        return make_error(ErrorCode::InvalidArgument, "has no Cloth3D");
    }
    if (model_path(world, entity) == nullptr) {
        return make_error(ErrorCode::InvalidArgument,
                          "cloth needs a MeshInstance3D drawing a model file");
    }
    return model_section(world, entity, cloth->section);
}

Result<std::shared_ptr<const ClothSection>>
model_section(const world::World& world, entt::entity entity, const std::string& name) {
    const std::string* path = model_path(world, entity);
    if (path == nullptr) {
        return make_error(ErrorCode::InvalidArgument,
                          "needs a MeshInstance3D drawing a model file");
    }
    auto loaded = model_of(world, *path);
    if (!loaded) {
        return make_error(loaded.error().code,
                          "\"" + *path + "\" can't be read: " + loaded.error().message);
    }
    const Model& model = *loaded.value().model;
    int material = -1;
    if (!name.empty()) {
        std::vector<std::string> names;
        for (std::size_t i = 0; i < model.materials.size(); ++i) {
            names.push_back(model.materials[i].name);
            if (model.materials[i].name == name && material < 0) {
                material = static_cast<int>(i);
            }
        }
        if (material < 0) {
            std::string message = "the model has no material \"" + name + "\"";
            if (const std::string_view close = closest_match(name, names); !close.empty()) {
                message += "; did you mean \"" + std::string(close) + "\"?";
            }
            return make_error(ErrorCode::InvalidArgument, std::move(message));
        }
    }

    auto section = std::make_shared<ClothSection>();
    section->model = loaded.value().model;
    section->key = *path + "@" + std::to_string(loaded.value().version) + "#" + name;
    const std::vector<world::Affine3D> rest_globals = node_globals(model, rest_pose(model));
    std::map<std::pair<int, std::size_t>, std::uint32_t> joint_ids;
    const auto joint_id = [&](int skin, std::size_t index,
                              const world::Affine3D& rest) -> std::uint32_t {
        const auto [found, added] = joint_ids.try_emplace(
            {skin, index}, static_cast<std::uint32_t>(section->joints.size()));
        if (added) {
            section->joints.push_back({.skin = skin, .index = index});
            section->rest.push_back(rest);
        }
        return found->second;
    };
    std::map<int, std::vector<world::Affine3D>> rest_joints;
    for (std::size_t p = 0; p < model.parts.size(); ++p) {
        const ModelPart& part = model.parts[p];
        if (part.mesh >= model.meshes.size()) {
            continue;
        }
        const ModelMesh& mesh = model.meshes[part.mesh];
        for (std::size_t i = 0; i < mesh.surfaces.size(); ++i) {
            const ModelSurface& surface = mesh.surfaces[i];
            if ((material >= 0 && surface.material != material) || surface.indices.empty()) {
                continue;
            }
            const bool skinned = part.skin >= 0 &&
                                 static_cast<std::size_t>(part.skin) < model.skins.size() &&
                                 surface.weights.size() == surface.vertices.size();
            const std::vector<world::Affine3D>* skin_rest = nullptr;
            if (skinned) {
                auto [found, added] = rest_joints.try_emplace(part.skin);
                if (added) {
                    found->second = joint_transforms(
                        model.skins[static_cast<std::size_t>(part.skin)], rest_globals);
                }
                skin_rest = &found->second;
            }
            const world::Affine3D part_rest =
                part.node_index >= 0 &&
                        static_cast<std::size_t>(part.node_index) < rest_globals.size()
                    ? rest_globals[static_cast<std::size_t>(part.node_index)]
                    : part.transform;
            const std::size_t first = section->positions.size();
            for (std::size_t v = 0; v < surface.vertices.size(); ++v) {
                const render::Vertex3D& vertex = surface.vertices[v];
                const Vec3 local = to_vec(vertex.position);
                std::array<std::pair<std::uint32_t, float>, 4> weights{};
                Vec3 at;
                if (skinned) {
                    // The vertex's four heaviest joints, as Jolt skins at most four.
                    const VertexWeights& w = surface.weights[v];
                    std::vector<std::pair<float, std::size_t>> order;
                    for (std::size_t k = 0; k < w.weights.size(); ++k) {
                        if (w.weights.at(k) > 0.0F && w.joints.at(k) < skin_rest->size()) {
                            order.emplace_back(w.weights.at(k), w.joints.at(k));
                        }
                    }
                    std::ranges::stable_sort(
                        order, [](const auto& a, const auto& b) { return a.first > b.first; });
                    float total = 0.0F;
                    for (std::size_t k = 0; k < order.size() && k < 4; ++k) {
                        total += order[k].first;
                    }
                    for (std::size_t k = 0; k < order.size() && k < 4; ++k) {
                        const std::size_t joint = order[k].second;
                        const world::Affine3D& rest = (*skin_rest)[joint];
                        weights.at(k) = {joint_id(part.skin, joint, rest), order[k].first / total};
                        at = at + rest.apply(local) * (order[k].first / total);
                    }
                    if (order.empty()) {
                        weights[0] = {joint_id(-1, p, part_rest), 1.0F};
                        at = part_rest.apply(local);
                    }
                } else {
                    weights[0] = {joint_id(-1, p, part_rest), 1.0F};
                    at = part_rest.apply(local);
                }
                section->positions.push_back(at);
                // The normal as the vertex's heaviest joint (or its part) turns it at rest.
                const world::Affine3D& carrier = section->rest[weights[0].first];
                Vec3 normal = carrier.basis(to_vec(vertex.normal));
                const real_t size = std::sqrt(dot(normal, normal));
                normal = size > 0.0F ? normal * (1.0F / size) : Vec3{0.0F, 1.0F, 0.0F};
                section->normals.push_back(normal);
                section->uvs.push_back(Vec2{vertex.uv[0], vertex.uv[1]});
                section->colors.push_back({static_cast<float>(vertex.rgba[0]) / 255.0F,
                                           static_cast<float>(vertex.rgba[1]) / 255.0F,
                                           static_cast<float>(vertex.rgba[2]) / 255.0F,
                                           static_cast<float>(vertex.rgba[3]) / 255.0F});
                section->weights.push_back(weights);
            }
            const std::size_t first_triangle = section->indices.size() / 3;
            for (const std::uint32_t index : surface.indices) {
                section->indices.push_back(static_cast<std::uint32_t>(first) + index);
            }
            section->pieces.push_back({.part = p,
                                       .surface = i,
                                       .first = first,
                                       .count = surface.vertices.size(),
                                       .first_triangle = first_triangle});
        }
    }
    if (section->indices.size() < 3) {
        return make_error(ErrorCode::InvalidArgument,
                          name.empty() ? std::string("the model has no triangles")
                                       : "the model draws no triangles with \"" + name + "\"");
    }
    return std::shared_ptr<const ClothSection>(std::move(section));
}

std::vector<world::Affine3D> cloth_joints(const world::World& world, const world::Tree3D& tree,
                                          entt::entity entity, const ClothSection& section) {
    const Model& model = *section.model;
    std::vector<world::Affine3D> globals;
    if (!model.nodes.empty()) {
        const ModelLoader load = loader_of(world);
        const auto& ecs = world.ecs();
        std::vector<NodePose> pose = pose_of(model, clips(world, entity),
                                             ecs.try_get<const world::Skeleton3D>(entity), load);
        const Placer global = [&tree](entt::entity e) { return tree.global(e); };
        apply_modifiers(world, entity, model, tree.global(entity), global, pose);
        globals = node_globals(model, pose);
    }
    std::map<int, std::vector<world::Affine3D>> skins;
    std::vector<world::Affine3D> out;
    out.reserve(section.joints.size());
    for (const ClothSection::Joint& joint : section.joints) {
        if (joint.skin >= 0) {
            auto [found, added] = skins.try_emplace(joint.skin);
            if (added) {
                found->second =
                    joint_transforms(model.skins[static_cast<std::size_t>(joint.skin)], globals);
            }
            out.push_back(joint.index < found->second.size() ? found->second[joint.index]
                                                             : world::Affine3D{});
            continue;
        }
        const ModelPart& part = model.parts[joint.index];
        const bool posed =
            part.node_index >= 0 && static_cast<std::size_t>(part.node_index) < globals.size();
        out.push_back(posed ? globals[static_cast<std::size_t>(part.node_index)] : part.transform);
    }
    return out;
}

std::vector<float> cloth_shares(const world::Cloth3D& cloth, const ClothSection& section) {
    std::vector<float> out(section.positions.size(), 1.0F);
    if (cloth.max_distance_map == "gradient") {
        if (dot(cloth.gradient, cloth.gradient) <= 0.0F || out.empty()) {
            return out;
        }
        std::vector<real_t> along(out.size());
        real_t low = 0.0F;
        real_t high = 0.0F;
        for (std::size_t i = 0; i < out.size(); ++i) {
            along[i] = dot(section.positions[i], cloth.gradient);
            low = i == 0 ? along[i] : std::min(low, along[i]);
            high = i == 0 ? along[i] : std::max(high, along[i]);
        }
        for (std::size_t i = 0; i < out.size(); ++i) {
            out[i] = high > low ? static_cast<float>((along[i] - low) / (high - low)) : 1.0F;
        }
    } else if (cloth.max_distance_map == "vertex_color") {
        std::size_t channel = 0;
        if (cloth.vertex_color_channel == "g") {
            channel = 1;
        } else if (cloth.vertex_color_channel == "b") {
            channel = 2;
        } else if (cloth.vertex_color_channel == "a") {
            channel = 3;
        }
        for (std::size_t i = 0; i < out.size(); ++i) {
            out[i] = section.colors[i].at(channel);
        }
    }
    return out;
}

std::vector<Problem> check_cloths(const world::World& world) {
    std::vector<Problem> problems;
    const auto& ecs = world.ecs();
    for (const entt::entity entity : ecs.view<const world::Cloth3D>()) {
        if (model_path(world, entity) == nullptr) {
            problems.push_back({entity, "/components/Cloth3D",
                                "cloth needs a MeshInstance3D drawing a model file; it is "
                                "ignored until it has one"});
            continue;
        }
        auto section = cloth_section(world, entity);
        if (!section) {
            problems.push_back({entity, std::string(kSectionField), section.error().message});
        }
    }
    return problems;
}

} // namespace slash::scene3d
