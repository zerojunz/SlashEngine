#include "slash/scene3d/collision.hpp"

#include "model_internal.hpp"
#include "vhacd.hpp"

#include "slash/asset/assets.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slash::scene3d {

namespace {

// Godot's _teststr and _fixstr (resource_importer_scene.cpp): a name without the trailing
// digits, spaces and dots Blender adds to copies has the hint as "$hint" anywhere, or ends with
// "-hint" or "_hint", in any case.
std::size_t hinted_length(std::string_view name) {
    std::size_t length = name.size();
    while (length > 0) {
        const auto c = static_cast<unsigned char>(name[length - 1]);
        if (std::isdigit(c) == 0 && c > 32 && c != '.') {
            break;
        }
        --length;
    }
    return length;
}

std::string lowered(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

bool has_hint(std::string_view name, std::string_view hint) {
    const std::string what = lowered(name.substr(0, hinted_length(name)));
    const std::string hinted(hint);
    return what.find("$" + hinted) != std::string::npos || what.ends_with("-" + hinted) ||
           what.ends_with("_" + hinted);
}

std::string without_hint(std::string_view name, std::string_view hint) {
    const std::size_t length = hinted_length(name);
    std::string what(name.substr(0, length));
    const std::string_view end = name.substr(length);
    const std::string lower = lowered(what);
    const std::string hinted(hint);
    if (lower.find("$" + hinted) != std::string::npos) {
        // Godot's String::replace, which matches case.
        const std::string needle = "$" + hinted;
        for (std::size_t at = what.find(needle); at != std::string::npos; at = what.find(needle)) {
            what.erase(at, needle.size());
        }
        return what + std::string(end);
    }
    if (lower.ends_with("-" + hinted) || lower.ends_with("_" + hinted)) {
        return what.substr(0, what.size() - hint.size() - 1) + std::string(end);
    }
    return what;
}

enum class Hint : std::uint8_t { None, Trimesh, Convex, TrimeshOnly, ConvexOnly };

// The hint on a mesh's node, in the order _pre_fix_node tests them.
std::pair<Hint, std::string_view> hint_of(std::string_view name) {
    if (has_hint(name, "colonly")) {
        return {Hint::TrimeshOnly, "colonly"};
    }
    if (has_hint(name, "convcolonly")) {
        return {Hint::ConvexOnly, "convcolonly"};
    }
    if (has_hint(name, "col")) {
        return {Hint::Trimesh, "col"};
    }
    if (has_hint(name, "convcol")) {
        return {Hint::Convex, "convcol"};
    }
    return {Hint::None, ""};
}

Vec3 cross(Vec3 a, Vec3 b) {
    return {(a.y * b.z) - (a.z * b.y), (a.z * b.x) - (a.x * b.z), (a.x * b.y) - (a.y * b.x)};
}

float dot(Vec3 a, Vec3 b) {
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

// A hull's faces wound as Godot's: clockwise seen from outside, so each face's
// (b - a) x (c - a) points in, toward the middle of its corners.
void wind_inward(ModelCollision& hull) {
    if (hull.points.empty()) {
        return;
    }
    Vec3 middle;
    for (const Vec3 point : hull.points) {
        middle = middle + point;
    }
    middle = middle * (1.0F / static_cast<float>(hull.points.size()));
    for (std::size_t i = 0; i + 2 < hull.indices.size(); i += 3) {
        const Vec3 a = hull.points[hull.indices[i]];
        const Vec3 b = hull.points[hull.indices[i + 1]];
        const Vec3 c = hull.points[hull.indices[i + 2]];
        const Vec3 face = (a + b + c) * (1.0F / 3.0F);
        if (dot(cross(b - a, c - a), face - middle) > 0.0F) {
            std::swap(hull.indices[i + 1], hull.indices[i + 2]);
        }
    }
}

ModelCollision from_vhacd(const detail::VhacdHull& hull) {
    ModelCollision out;
    out.convex = true;
    out.points.reserve(hull.points.size() / 3);
    for (std::size_t i = 0; i + 2 < hull.points.size(); i += 3) {
        out.points.push_back({static_cast<float>(hull.points[i]),
                              static_cast<float>(hull.points[i + 1]),
                              static_cast<float>(hull.points[i + 2])});
    }
    out.indices = hull.triangles;
    wind_inward(out);
    return out;
}

std::vector<float> flat(std::span<const Vec3> points) {
    std::vector<float> out;
    out.reserve(points.size() * 3);
    for (const Vec3 point : points) {
        out.insert(out.end(), {point.x, point.y, point.z});
    }
    return out;
}

// A part's triangles in the model's space, appended to points and indices.
void add_part(const Model& model, const ModelPart& part, std::vector<Vec3>& points,
              std::vector<std::uint32_t>& indices) {
    if (part.mesh >= model.meshes.size()) {
        return;
    }
    for (const ModelSurface& surface : model.meshes[part.mesh].surfaces) {
        const auto base = static_cast<std::uint32_t>(points.size());
        for (const render::Vertex3D& vertex : surface.vertices) {
            points.push_back(part.transform.apply(position_of(vertex)));
        }
        for (const std::uint32_t index : surface.indices) {
            indices.push_back(base + index);
        }
    }
}

// Meshes no part draws any more, dropped, and parts' indices into the rest.
void drop_unused_meshes(Model& model) {
    std::vector<std::size_t> remap(model.meshes.size(), model.meshes.size());
    for (const ModelPart& part : model.parts) {
        if (part.mesh < remap.size()) {
            remap[part.mesh] = 0;
        }
    }
    std::vector<ModelMesh> kept;
    for (std::size_t i = 0; i < model.meshes.size(); ++i) {
        if (remap[i] == 0) {
            remap[i] = kept.size();
            kept.push_back(std::move(model.meshes[i]));
        }
    }
    for (ModelPart& part : model.parts) {
        if (part.mesh < remap.size()) {
            part.mesh = remap[part.mesh];
        }
    }
    model.meshes = std::move(kept);
}

struct CollisionCache {
    ModelLoader models;
    std::mutex mutex;
    struct Entry {
        std::weak_ptr<const Model> model;
        std::shared_ptr<const std::vector<world::CollisionShape3D>> shapes;
    };
    std::map<const Model*, Entry> entries;

    world::MemoryPart memory() {
        const std::scoped_lock lock(mutex);
        world::MemoryPart part{"model_collision", 0, {}, {}};
        std::uint64_t shapes = 0;
        for (const auto& [model, entry] : entries) {
            if (entry.shapes == nullptr) {
                continue;
            }
            for (const world::CollisionShape3D& shape : *entry.shapes) {
                part.bytes += sizeof(shape) +
                              ((shape.points.capacity() + shape.faces.capacity()) * sizeof(Vec3));
            }
            shapes += entry.shapes->size();
        }
        part.counts = {{"models", entries.size()}, {"shapes", shapes}};
        return part;
    }
};

CollisionCache* find_cache(const world::World& world) {
    const auto* found = world.ecs().ctx().find<std::shared_ptr<CollisionCache>>();
    return found != nullptr ? found->get() : nullptr;
}

std::shared_ptr<const std::vector<world::CollisionShape3D>> shapes_of(const Model& model) {
    if (model.collision.empty()) {
        return nullptr;
    }
    return std::make_shared<const std::vector<world::CollisionShape3D>>(collision_shapes(model));
}

} // namespace

void apply_import_hints(Model& model) {
    NameSet taken = node_names();
    for (const ModelNode& node : model.nodes) {
        taken.insert(node.name);
    }
    std::vector<bool> drawn(model.parts.size(), true);
    // A node placing several parts is renamed once.
    std::map<int, std::string> renamed;
    for (std::size_t i = 0; i < model.parts.size(); ++i) {
        ModelPart& part = model.parts[i];
        const auto [hint, word] = hint_of(part.node);
        if (hint == Hint::None) {
            continue;
        }
        std::string name;
        if (const auto found = renamed.find(part.node_index);
            part.node_index >= 0 && found != renamed.end()) {
            name = found->second;
        } else {
            taken.erase(part.node);
            name = unique_name(taken, without_hint(part.node, word));
            if (part.node_index >= 0 &&
                static_cast<std::size_t>(part.node_index) < model.nodes.size()) {
                model.nodes[static_cast<std::size_t>(part.node_index)].name = name;
                renamed.emplace(part.node_index, name);
            }
        }
        part.node = name;
        ModelCollision collision;
        add_part(model, part, collision.points, collision.indices);
        if (hint == Hint::Convex || hint == Hint::ConvexOnly) {
            collision = convex_hull(collision.points);
        }
        collision.node = name;
        collision.node_index = part.node_index;
        if (!collision.points.empty()) {
            model.collision.push_back(std::move(collision));
        }
        drawn[i] = hint == Hint::Trimesh || hint == Hint::Convex;
    }
    if (std::ranges::find(drawn, false) == drawn.end()) {
        return;
    }
    std::vector<ModelPart> parts;
    for (std::size_t i = 0; i < model.parts.size(); ++i) {
        if (drawn[i]) {
            parts.push_back(std::move(model.parts[i]));
        }
    }
    model.parts = std::move(parts);
    drop_unused_meshes(model);
}

ModelCollision convex_hull(std::span<const Vec3> points, std::size_t max_points) {
    const std::vector<float> values = flat(points);
    const detail::VhacdHull hull =
        detail::vhacd_hull(values, static_cast<std::uint32_t>(max_points));
    if (hull.triangles.empty()) {
        ModelCollision none;
        none.convex = true;
        return none;
    }
    return from_vhacd(hull);
}

std::vector<ModelCollision> convex_decompose(std::span<const Vec3> points,
                                             std::span<const std::uint32_t> indices,
                                             const DecomposeOptions& options) {
    const std::vector<float> values = flat(points);
    detail::VhacdSettings settings;
    settings.max_hulls = static_cast<std::uint32_t>(std::max<std::size_t>(options.max_hulls, 1));
    settings.max_points = static_cast<std::uint32_t>(std::max<std::size_t>(options.max_points, 4));
    settings.resolution = options.resolution;
    std::vector<ModelCollision> out;
    for (const detail::VhacdHull& hull : detail::vhacd_decompose(values, indices, settings)) {
        if (!hull.triangles.empty()) {
            out.push_back(from_vhacd(hull));
        }
    }
    return out;
}

Result<std::size_t> generate_collision(Model& model, std::string_view kind,
                                       const DecomposeOptions& options) {
    if (kind != "trimesh" && kind != "convex" && kind != "decompose") {
        return make_error(ErrorCode::InvalidArgument,
                          R"(collision is "trimesh", "convex" or "decompose", not ")" +
                              std::string(kind) + "\"");
    }
    ModelCollision whole;
    for (const ModelPart& part : model.parts) {
        add_part(model, part, whole.points, whole.indices);
    }
    if (whole.indices.size() < 3) {
        return make_error(ErrorCode::InvalidArgument, "the model has no triangles to collide with");
    }
    std::vector<ModelCollision> made;
    if (kind == "trimesh") {
        made.push_back(std::move(whole));
    } else if (kind == "convex") {
        made.push_back(convex_hull(whole.points, options.max_points));
    } else {
        made = convex_decompose(whole.points, whole.indices, options);
    }
    std::erase_if(made, [](const ModelCollision& shape) { return shape.points.empty(); });
    if (made.empty()) {
        return make_error(ErrorCode::InvalidArgument,
                          "the model's triangles span no volume, so they make no convex hull: use "
                          "\"trimesh\"");
    }
    for (std::size_t i = 0; i < made.size(); ++i) {
        made[i].node = made.size() == 1 ? "collision" : "collision" + std::to_string(i + 1);
    }
    model.collision = std::move(made);
    return model.collision.size();
}

std::vector<world::CollisionShape3D> collision_shapes(const Model& model) {
    std::vector<world::CollisionShape3D> out;
    out.reserve(model.collision.size());
    for (const ModelCollision& collision : model.collision) {
        world::CollisionShape3D shape;
        if (collision.convex) {
            shape.shape = "convex_polygon";
            shape.points = collision.points;
        } else {
            shape.shape = "concave_polygon";
            shape.faces.reserve(collision.indices.size());
            for (const std::uint32_t index : collision.indices) {
                if (index < collision.points.size()) {
                    shape.faces.push_back(collision.points[index]);
                }
            }
            shape.faces.resize(shape.faces.size() - (shape.faces.size() % 3));
        }
        out.push_back(std::move(shape));
    }
    return out;
}

std::shared_ptr<const std::vector<world::CollisionShape3D>> model_shapes(const world::World& world,
                                                                         entt::entity entity) {
    const auto* mesh = world.ecs().try_get<world::MeshInstance3D>(entity);
    if (mesh == nullptr || mesh->mesh.empty() || !is_model_path(mesh->mesh)) {
        return nullptr;
    }
    CollisionCache* cache = find_cache(world);
    if (cache == nullptr) {
        const world::FileReader& reader = world.file_reader();
        if (!reader) {
            return nullptr;
        }
        auto bytes = reader(asset::file_of(mesh->mesh));
        if (!bytes) {
            return nullptr;
        }
        auto model = load_model(bytes.value(), mesh->mesh, reader);
        return model ? shapes_of(model.value()) : nullptr;
    }
    auto model = cache->models(mesh->mesh);
    if (!model || model.value() == nullptr) {
        return nullptr;
    }
    const std::shared_ptr<const Model>& held = model.value();
    const std::scoped_lock lock(cache->mutex);
    if (const auto found = cache->entries.find(held.get()); found != cache->entries.end()) {
        if (found->second.model.lock() == held) {
            return found->second.shapes;
        }
    }
    // Models that are gone go with it.
    std::erase_if(cache->entries, [](const auto& entry) { return entry.second.model.expired(); });
    auto shapes = shapes_of(*held);
    cache->entries.insert_or_assign(held.get(), CollisionCache::Entry{held, shapes});
    return shapes;
}

void install_model_collision(world::World& world, ModelLoader models) {
    if (CollisionCache* cache = find_cache(world); cache != nullptr) {
        const std::scoped_lock lock(cache->mutex);
        cache->models = std::move(models);
        cache->entries.clear();
        return;
    }
    auto cache = std::make_shared<CollisionCache>();
    cache->models = std::move(models);
    world.ecs().ctx().emplace<std::shared_ptr<CollisionCache>>(cache);
    world.add_memory_source("model_collision",
                            [cache](const world::World& /*world*/) { return cache->memory(); });
}

std::vector<ModelBody> model_bodies(const world::World& world) {
    std::vector<ModelBody> bodies;
    for (const entt::entity entity : world.ecs().view<const world::MeshInstance3D>()) {
        if (auto shapes = model_shapes(world, entity); shapes != nullptr) {
            bodies.push_back({.entity = entity, .shapes = std::move(shapes)});
        }
    }
    return bodies;
}

} // namespace slash::scene3d
