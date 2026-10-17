#include "slash/world/canvas_layer.hpp"

#include "slash/world/world.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

namespace slash::world {
namespace {

// Godot's Math::fposmod: x modulo y with y's sign.
float fposmod(float x, float y) {
    float value = std::fmod(x, y);
    if ((value < 0.0F && y > 0.0F) || (value > 0.0F && y < 0.0F)) {
        value += y;
    }
    return value + 0.0F;
}

// Godot's CLAMP.
float clamped(float value, float low, float high) {
    if (value < low) {
        return low;
    }
    return value > high ? high : value;
}

} // namespace

Affine2D canvas_layer_transform(const CanvasLayer& layer, const Affine2D& world, Vec2 screen_size) {
    // CanvasLayer::_update_xform: set_rotation_and_scale, then set_origin.
    Transform2D own;
    own.position = layer.offset;
    own.rotation = layer.rotation;
    own.scale = layer.scale;
    Affine2D out = affine(own);
    if (!layer.follow_viewport_enabled) {
        return out;
    }
    // _canvas_get_transform: the parent canvas's transform, then the layer's, then the parent
    // scale around the middle of the viewport.
    out = world * out;
    if (const float scale = layer.follow_viewport_scale; scale != 1.0F) {
        const Vec2 pivot = screen_size * 0.5F;
        out.x = out.x * scale;
        out.y = out.y * scale;
        out.origin = ((out.origin - pivot) * scale) + pivot;
    }
    return out;
}

Canvases::Canvases(const World& world, const Tree2D& tree, const CameraView& view,
                   Vec2 screen_size) {
    const Canvas scene{.layer = entt::null, .order = 0, .visible = true, .transform = view.canvas};
    const auto& ecs = world.ecs();
    const auto layers = ecs.view<const CanvasLayer>();
    if (layers.begin() == layers.end()) {
        canvases_.push_back(scene);
        return;
    }
    // The layers in the order their entities were made, then stacked by layer with the world
    // first among layer 0.
    std::vector<entt::entity> made(layers.begin(), layers.end());
    std::ranges::sort(made, {}, [](entt::entity entity) { return entt::to_entity(entity); });
    std::vector<entt::entity> stacked = made;
    std::ranges::stable_sort(
        stacked, {}, [&](entt::entity entity) { return ecs.get<const CanvasLayer>(entity).layer; });
    std::unordered_map<entt::entity, std::size_t> index;
    bool placed = false;
    for (const entt::entity entity : stacked) {
        const CanvasLayer& layer = ecs.get<const CanvasLayer>(entity);
        if (!placed && layer.layer >= 0) {
            world_ = canvases_.size();
            canvases_.push_back(scene);
            placed = true;
        }
        index.emplace(entity, canvases_.size());
        canvases_.push_back({.layer = entity,
                             .order = layer.layer,
                             .visible = layer.visible,
                             .transform = canvas_layer_transform(layer, view.canvas, screen_size)});
    }
    if (!placed) {
        world_ = canvases_.size();
        canvases_.push_back(scene);
    }
    for (const entt::entity entity : made) {
        for (const entt::entity child : ecs.get<const CanvasLayer>(entity).children) {
            if (child != entity && world.valid(child) && !ecs.all_of<CanvasLayer>(child)) {
                held_.try_emplace(child, index.at(entity));
            }
        }
    }
    if (held_.empty()) {
        return;
    }
    // Each entity's parent: through Transform2D children, else through Control children.
    for (const entt::entity entity : ecs.view<const Transform2D>()) {
        if (const entt::entity parent = tree.parent(entity); parent != entt::null) {
            parents_.emplace(entity, parent);
        }
    }
    std::vector<entt::entity> controls;
    for (const entt::entity entity : ecs.view<const Control>()) {
        controls.push_back(entity);
    }
    std::ranges::sort(controls, {}, [](entt::entity entity) { return entt::to_entity(entity); });
    for (const entt::entity entity : controls) {
        for (const entt::entity child : ecs.get<const Control>(entity).children) {
            if (child != entity) {
                parents_.try_emplace(child, entity);
            }
        }
    }
}

std::size_t Canvases::of(entt::entity entity) const {
    if (held_.empty()) {
        return world_;
    }
    // Up through the parents, as far as there are links, so a loop of Control children ends.
    entt::entity at = entity;
    for (std::size_t steps = 0; steps <= parents_.size() && at != entt::null; ++steps) {
        if (const auto found = held_.find(at); found != held_.end()) {
            return found->second;
        }
        const auto parent = parents_.find(at);
        at = parent != parents_.end() ? parent->second : entt::entity{entt::null};
    }
    return world_;
}

Canvases canvases(const World& world, const Tree2D& tree, Vec2 screen_size) {
    return {world, tree, camera_view(world, tree, screen_size), screen_size};
}

Affine2D screen_transform(const World& world, const Canvases& canvases, entt::entity entity) {
    const std::size_t index = canvases.of(entity);
    if (index == canvases.world_canvas() && entity != entt::null && world.valid(entity) &&
        world.ecs().all_of<Control>(entity)) {
        return {};
    }
    return canvases.all()[index].transform;
}

Vec2 parallax_screen_offset(const CameraView& view, Vec2 screen_size) {
    const Vec2 half = screen_size * 0.5F;
    return view.canvas.inverse().apply(half) - half;
}

Vec2 parallax_position(const Parallax2D& parallax, Vec2 scale, Vec2 screen_size) {
    // Godot's Parallax2D::_update_scroll, line for line.
    const Vec2 screen_offset = parallax.screen_offset;
    Vec2 scroll = screen_offset;
    if (parallax.limit_begin.x <= parallax.limit_end.x - screen_size.x) {
        scroll.x = clamped(scroll.x, parallax.limit_begin.x, parallax.limit_end.x - screen_size.x);
    }
    if (parallax.limit_begin.y <= parallax.limit_end.y - screen_size.y) {
        scroll.y = clamped(scroll.y, parallax.limit_begin.y, parallax.limit_end.y - screen_size.y);
    }
    scroll = {scroll.x * parallax.scroll_scale.x, scroll.y * parallax.scroll_scale.y};
    const Vec2 repeat{std::max(parallax.repeat_size.x, 0.0F),
                      std::max(parallax.repeat_size.y, 0.0F)};
    if (repeat.x != 0.0F) {
        const float mod = fposmod(scroll.x - parallax.scroll_offset.x, repeat.x * scale.x);
        scroll.x = screen_offset.x - mod;
    } else {
        scroll.x = screen_offset.x + parallax.scroll_offset.x - scroll.x;
    }
    if (repeat.y != 0.0F) {
        const float mod = fposmod(scroll.y - parallax.scroll_offset.y, repeat.y * scale.y);
        scroll.y = screen_offset.y - mod;
    } else {
        scroll.y = screen_offset.y + parallax.scroll_offset.y - scroll.y;
    }
    if (!parallax.follow_viewport) {
        scroll = scroll - screen_offset;
    }
    return scroll;
}

void add_parallax_system(World& world, std::function<Vec2(const World&)> screen_size) {
    world.add_system("parallax_2d",
                     [size_of = std::move(screen_size)](World& w, double tick_seconds) {
                         auto& ecs = w.ecs();
                         const auto parallaxes = ecs.view<Parallax2D, Transform2D>();
                         if (parallaxes.begin() == parallaxes.end()) {
                             return;
                         }
                         const Vec2 screen = size_of(w);
                         const Tree2D tree(w);
                         const CameraView view = camera_view(w, tree, screen);
                         const Vec2 camera_offset = parallax_screen_offset(view, screen);
                         const auto delta = static_cast<float>(tick_seconds);
                         for (const auto [entity, parallax, transform] : parallaxes.each()) {
                             // NOTIFICATION_INTERNAL_PROCESS, which runs only with both set.
                             const Vec2 repeat{std::max(parallax.repeat_size.x, 0.0F),
                                               std::max(parallax.repeat_size.y, 0.0F)};
                             if ((repeat.x != 0.0F || repeat.y != 0.0F) &&
                                 (parallax.autoscroll.x != 0.0F || parallax.autoscroll.y != 0.0F)) {
                                 Vec2 offset =
                                     parallax.scroll_offset + (parallax.autoscroll * delta);
                                 if (repeat.x != 0.0F) {
                                     offset.x = fposmod(offset.x, repeat.x);
                                 }
                                 if (repeat.y != 0.0F) {
                                     offset.y = fposmod(offset.y, repeat.y);
                                 }
                                 parallax.scroll_offset = offset;
                             }
                             // _camera_moved, which only the current camera calls.
                             if (view.camera != entt::null && !parallax.ignore_camera_scroll) {
                                 parallax.screen_offset = camera_offset;
                             }
                             transform.position =
                                 parallax_position(parallax, transform.scale, screen);
                         }
                     },
                     {.doc = "Scrolls each Parallax2D with the current camera and its autoscroll.",
                      .priority = 1060});
}

} // namespace slash::world
