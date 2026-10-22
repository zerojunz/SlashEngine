#include "slash/world/tree2d.hpp"

#include "slash/world/world.hpp"

#include <algorithm>
#include <cmath>
#include <ranges>
#include <unordered_set>

namespace slash::world {

namespace {

float dot(Vec2 a, Vec2 b) {
    return (a.x * b.x) + (a.y * b.y);
}

float length(Vec2 v) {
    return std::sqrt(dot(v, v));
}

// The entities with a Transform2D in the order they were made.
std::vector<entt::entity> made_order(const World& world) {
    std::vector<entt::entity> entities;
    for (const entt::entity entity : world.ecs().view<const Transform2D>()) {
        entities.push_back(entity);
    }
    std::ranges::sort(entities, {}, [](entt::entity entity) { return entt::to_entity(entity); });
    return entities;
}

// Whether any Transform2D lists children.
bool has_children(const World& world) {
    const auto view = world.ecs().view<const Transform2D>();
    return std::ranges::any_of(view, [&](entt::entity entity) {
        return !view.get<const Transform2D>(entity).children.empty();
    });
}

// A parent's global transform, then its child's own, as Godot multiplies their matrices. A
// parent that only turns, moves and scales evenly adds its rotation and scale to the child's
// instead, which is the same transform with no rounding in the angle.
Transform2D compose(const Transform2D& parent, const Transform2D& local) {
    if (parent.skew == 0.0F && parent.scale.x == parent.scale.y && parent.scale.x > 0.0F) {
        const float s = parent.scale.x;
        const Vec2 cs = cos_sin(parent.rotation);
        Transform2D out;
        out.position = {
            parent.position.x + (s * ((cs.x * local.position.x) - (cs.y * local.position.y))),
            parent.position.y + (s * ((cs.y * local.position.x) + (cs.x * local.position.y)))};
        out.rotation = parent.rotation + local.rotation;
        out.scale = {local.scale.x * s, local.scale.y * s};
        out.skew = local.skew;
        return out;
    }
    return parts(affine(parent) * affine(local));
}

} // namespace

Vec2 Affine2D::apply(Vec2 p) const {
    return {(x.x * p.x) + (y.x * p.y) + origin.x, (x.y * p.x) + (y.y * p.y) + origin.y};
}

Affine2D Affine2D::operator*(const Affine2D& o) const {
    const auto basis = [this](Vec2 v) {
        return Vec2{(x.x * v.x) + (y.x * v.y), (x.y * v.x) + (y.y * v.y)};
    };
    return {.x = basis(o.x), .y = basis(o.y), .origin = apply(o.origin)};
}

Affine2D Affine2D::inverse() const {
    const float det = (x.x * y.y) - (x.y * y.x);
    if (det == 0.0F || !std::isfinite(det)) {
        return *this;
    }
    const float inv = 1.0F / det;
    Affine2D out{.x = {y.y * inv, -x.y * inv}, .y = {-y.x * inv, x.x * inv}, .origin = {}};
    const Vec2 moved = out.apply(origin);
    out.origin = {-moved.x, -moved.y};
    return out;
}

Affine2D affine(const Transform2D& transform) {
    const Vec2 turn = cos_sin(transform.rotation);
    const Vec2 lean = transform.skew == 0.0F ? turn : cos_sin(transform.rotation + transform.skew);
    return {.x = {turn.x * transform.scale.x, turn.y * transform.scale.x},
            .y = {-lean.y * transform.scale.y, lean.x * transform.scale.y},
            .origin = transform.position};
}

Transform2D parts(const Affine2D& matrix) {
    Transform2D out;
    out.position = matrix.origin;
    out.rotation = atan2(matrix.x.y, matrix.x.x);
    const float det = (matrix.x.x * matrix.y.y) - (matrix.x.y * matrix.y.x);
    float sign = 0.0F;
    if (det > 0.0F) {
        sign = 1.0F;
    } else if (det < 0.0F) {
        sign = -1.0F;
    }
    const float x_length = length(matrix.x);
    const float y_length = length(matrix.y);
    out.scale = {x_length, sign * y_length};
    // acos(d) - pi / 2, as Godot's get_skew, is -asin(d).
    if (x_length > 0.0F && y_length > 0.0F && sign != 0.0F) {
        const float d =
            std::clamp(dot(matrix.x, matrix.y) * sign / (x_length * y_length), -1.0F, 1.0F);
        out.skew = -atan2(d, std::sqrt(1.0F - (d * d)));
    }
    return out;
}

Tree2D::Tree2D(const World& world) : world_(&world) {
    const auto& ecs = world.ecs();
    if (!has_children(world)) {
        return;
    }
    const std::vector<entt::entity> entities = made_order(world);
    for (const entt::entity entity : entities) {
        for (const entt::entity child : ecs.get<Transform2D>(entity).children) {
            if (child == entity || !world.valid(child) || !ecs.all_of<Transform2D>(child)) {
                continue;
            }
            parents_.try_emplace(child, entity);
        }
    }
    // A cycle is broken at its first entity in the order they were made, which becomes a root.
    for (const entt::entity entity : entities) {
        const auto link = parents_.find(entity);
        if (link == parents_.end()) {
            continue;
        }
        entt::entity up = link->second;
        for (std::size_t steps = 0; steps <= parents_.size() && up != entt::null; ++steps) {
            if (up == entity) {
                parents_.erase(link);
                break;
            }
            up = parent(up);
        }
    }
    // Draw keys: the root's place in the order entities were made, then the place in its tree.
    // As Godot, a top_level entity is an item of the canvas of its own, with the entities under
    // it: it draws after the rest of the tree it is in, top-level items in tree order, and
    // before the roots made after that tree's.
    struct Visit {
        entt::entity entity;
        std::size_t item; // the place in the tree of its top-level item, or 0 for the root's
    };
    std::vector<Visit> stack;
    std::vector<Visit> visits;
    for (const entt::entity root : entities) {
        if (parents_.contains(root) || ecs.get<Transform2D>(root).children.empty()) {
            continue;
        }
        visits.clear();
        stack.push_back({root, 0});
        while (!stack.empty()) {
            Visit visit = stack.back();
            stack.pop_back();
            if (visit.entity != root && ecs.get<Transform2D>(visit.entity).top_level) {
                visit.item = visits.size();
            }
            visits.push_back(visit);
            const auto& children = ecs.get<Transform2D>(visit.entity).children;
            for (const entt::entity child : std::views::reverse(children)) {
                if (const auto found = parents_.find(child);
                    found != parents_.end() && found->second == visit.entity) {
                    stack.push_back({child, visit.item});
                }
            }
        }
        std::ranges::stable_sort(visits, {}, &Visit::item);
        const std::uint64_t base = static_cast<std::uint64_t>(entt::to_entity(root)) << 32U;
        for (std::size_t i = 0; i < visits.size(); ++i) {
            keys_[visits[i].entity] = base + i;
        }
    }
}

std::uint64_t Tree2D::draw_key(entt::entity entity) const {
    const auto found = keys_.find(entity);
    return found != keys_.end() ? found->second
                                : static_cast<std::uint64_t>(entt::to_entity(entity)) << 32U;
}

entt::entity Tree2D::parent(entt::entity entity) const {
    const auto found = parents_.find(entity);
    return found != parents_.end() ? found->second : entt::entity{entt::null};
}

entt::entity Tree2D::transform_parent(entt::entity entity) const {
    const entt::entity up = parent(entity);
    if (up == entt::null || world_->ecs().get<Transform2D>(entity).top_level) {
        return entt::null;
    }
    return up;
}

Transform2D Tree2D::global(entt::entity entity) const {
    const auto* own = world_->valid(entity) ? world_->ecs().try_get<Transform2D>(entity) : nullptr;
    if (own == nullptr) {
        return {};
    }
    Transform2D local;
    local.position = own->position;
    local.rotation = own->rotation;
    local.scale = own->scale;
    local.skew = own->skew;
    const entt::entity up = own->top_level ? entt::entity{entt::null} : parent(entity);
    if (up == entt::null) {
        return local;
    }
    return compose(global(up), local);
}

Affine2D Tree2D::global_affine(entt::entity entity) const {
    return affine(global(entity));
}

void Tree2D::set_global_position(World& world, entt::entity entity, Vec2 position) const {
    auto& transform = world.ecs().get_or_emplace<Transform2D>(entity);
    const entt::entity up = transform_parent(entity);
    transform.position = up == entt::null ? position : global_affine(up).inverse().apply(position);
}

void Tree2D::set_global_rotation(World& world, entt::entity entity, float rotation) const {
    auto& transform = world.ecs().get_or_emplace<Transform2D>(entity);
    const entt::entity up = transform_parent(entity);
    transform.rotation = up == entt::null ? rotation : rotation - global(up).rotation;
}

namespace {

// What CanvasLayers' children get wrong: an entity there has no other parent and one layer, and
// layers don't hold each other. parents are the Transform2D children's.
void check_layers(const World& world, const std::unordered_map<entt::entity, entt::entity>& parents,
                  std::vector<TreeProblem>& problems) {
    const auto& ecs = world.ecs();
    std::vector<entt::entity> layers;
    for (const entt::entity entity : ecs.view<const CanvasLayer>()) {
        layers.push_back(entity);
    }
    if (layers.empty()) {
        return;
    }
    std::ranges::sort(layers, {}, [](entt::entity entity) { return entt::to_entity(entity); });
    std::unordered_map<entt::entity, entt::entity> control_parents;
    std::vector<entt::entity> controls;
    for (const entt::entity entity : ecs.view<const Control>()) {
        controls.push_back(entity);
    }
    std::ranges::sort(controls, {}, [](entt::entity entity) { return entt::to_entity(entity); });
    for (const entt::entity entity : controls) {
        for (const entt::entity child : ecs.get<const Control>(entity).children) {
            control_parents.try_emplace(child, entity);
        }
    }
    const auto quoted = [&](entt::entity entity) {
        return "\"" + std::string(world.id_of(entity)) + "\"";
    };
    std::unordered_map<entt::entity, entt::entity> held;
    for (const entt::entity layer : layers) {
        const auto& children = ecs.get<const CanvasLayer>(layer).children;
        for (std::size_t i = 0; i < children.size(); ++i) {
            const entt::entity child = children[i];
            const std::string path = "/entities/" + std::string(world.id_of(layer)) +
                                     "/components/CanvasLayer/children/" + std::to_string(i);
            const auto parent = parents.find(child);
            const auto control_parent = control_parents.find(child);
            if (child == entt::null || !world.valid(child)) {
                problems.push_back({path, "children can't be null"});
            } else if (child == layer) {
                problems.push_back({path, "a canvas layer can't hold itself"});
            } else if (ecs.all_of<CanvasLayer>(child)) {
                problems.push_back({path, quoted(child) +
                                              " is a canvas layer; canvas layers can't hold each "
                                              "other"});
            } else if (parent != parents.end()) {
                problems.push_back(
                    {path, quoted(child) + " is already a child of " + quoted(parent->second)});
            } else if (control_parent != control_parents.end()) {
                problems.push_back({path, quoted(child) + " is already a child of " +
                                              quoted(control_parent->second)});
            } else if (const auto [found, added] = held.try_emplace(child, layer); !added) {
                problems.push_back(
                    {path, quoted(child) + " is already in " + quoted(found->second)});
            }
        }
    }
}

} // namespace

namespace {

// What a tree of T's children gets wrong, T being Transform2D or Transform3D, named name: null,
// the entity itself or one without a T, a second parent, or a cycle. Returns each child's parent.
template <typename T>
std::unordered_map<entt::entity, entt::entity>
check_children(const World& world, const std::string& name, std::vector<TreeProblem>& problems) {
    std::unordered_map<entt::entity, entt::entity> parents;
    const auto& ecs = world.ecs();
    const auto view = ecs.view<const T>();
    if (std::ranges::none_of(view, [&](entt::entity entity) {
            return !view.template get<const T>(entity).children.empty();
        })) {
        return parents;
    }
    std::vector<entt::entity> entities(view.begin(), view.end());
    std::ranges::sort(entities, {}, [](entt::entity entity) { return entt::to_entity(entity); });
    const auto quoted = [&](entt::entity entity) {
        return "\"" + std::string(world.id_of(entity)) + "\"";
    };
    for (const entt::entity entity : entities) {
        const auto& children = ecs.get<T>(entity).children;
        for (std::size_t i = 0; i < children.size(); ++i) {
            const entt::entity child = children[i];
            const std::string path = "/entities/" + std::string(world.id_of(entity)) +
                                     "/components/" + name + "/children/" + std::to_string(i);
            if (child == entt::null || !world.valid(child)) {
                problems.push_back({path, "children can't be null"});
            } else if (child == entity) {
                problems.push_back({path, "an entity can't be its own child"});
            } else if (!ecs.all_of<T>(child)) {
                problems.push_back({path, quoted(child) + " has no " + name + " to place in " +
                                              quoted(entity) + "'s space"});
            } else if (const auto [found, added] = parents.try_emplace(child, entity); !added) {
                problems.push_back(
                    {path, quoted(child) + " is already a child of " + quoted(found->second)});
            }
        }
    }
    // Cycles: an entity found again going up from it, reported once for all its members.
    std::unordered_set<entt::entity> reported;
    for (const entt::entity entity : entities) {
        if (reported.contains(entity)) {
            continue;
        }
        std::vector<entt::entity> members{entity};
        entt::entity up = entity;
        for (std::size_t steps = 0; steps <= parents.size(); ++steps) {
            const auto found = parents.find(up);
            if (found == parents.end()) {
                members.clear();
                break;
            }
            up = found->second;
            if (up == entity) {
                break;
            }
            members.push_back(up);
        }
        if (up != entity || members.empty()) {
            continue;
        }
        reported.insert(members.begin(), members.end());
        const entt::entity parent = parents.at(entity);
        const auto& list = ecs.get<T>(parent).children;
        const auto at = std::ranges::find(list, entity) - list.begin();
        problems.push_back({"/entities/" + std::string(world.id_of(parent)) + "/components/" +
                                name + "/children/" + std::to_string(at),
                            quoted(entity) + " is an ancestor of " + quoted(parent) +
                                ", so it can't be its child"});
    }
    return parents;
}

} // namespace

std::vector<TreeProblem> check_tree(const World& world) {
    std::vector<TreeProblem> problems;
    const auto parents = check_children<Transform2D>(world, "Transform2D", problems);
    check_layers(world, parents, problems);
    (void)check_children<Transform3D>(world, "Transform3D", problems);
    return problems;
}

} // namespace slash::world
