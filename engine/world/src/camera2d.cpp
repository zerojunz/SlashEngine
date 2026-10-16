#include "slash/world/camera2d.hpp"

#include "slash/world/world.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <numbers>
#include <set>
#include <string>
#include <utility>

namespace slash::world {
namespace {

constexpr float kTau = 2.0F * std::numbers::pi_v<float>;

// Godot's Math::angle_difference and Math::lerp_angle.
float angle_difference(float from, float to) {
    const float difference = std::fmod(to - from, kTau);
    return std::fmod(2.0F * difference, kTau) - difference;
}

float lerp_angle(float from, float to, float weight) {
    return from + (angle_difference(from, to) * weight);
}

// Godot's Vector2::rotated.
Vec2 rotated(Vec2 v, float angle) {
    const Vec2 cs = cos_sin(angle);
    return {(v.x * cs.x) - (v.y * cs.y), (v.x * cs.y) + (v.y * cs.x)};
}

// Godot's Vector2::normalize.
Vec2 normalized(Vec2 v) {
    const float length_squared = (v.x * v.x) + (v.y * v.y);
    if (length_squared == 0.0F) {
        return v;
    }
    const float length = std::sqrt(length_squared);
    return {v.x / length, v.y / length};
}

// A limit plus another, as Godot adds the two ints, without overflowing.
float sum(int a, int b) {
    return static_cast<float>(std::int64_t{a} + std::int64_t{b});
}

// What a camera's state is kept under: its ID, which outlives the entity handles that restoring
// a copy of the world (a failed command, a dry run, undo) hands out anew.
std::string key_of(const World& world, entt::entity entity) {
    const std::string_view id = world.id_of(entity);
    return id.empty() ? "#" + std::to_string(entt::to_integral(entity)) : std::string(id);
}

// Every camera's state, kept with the world.
struct Cameras {
    std::function<Vec2(const World&)> screen_size;
    std::map<std::string, CameraState, std::less<>> states;

    void step(World& world, double tick_seconds) {
        const auto& ecs = world.ecs();
        std::set<std::string, std::less<>> seen;
        for (const entt::entity entity : ecs.view<Camera2D>()) {
            seen.insert(key_of(world, entity));
        }
        std::erase_if(states, [&](const auto& entry) { return !seen.contains(entry.first); });
        const Tree2D tree(world);
        const entt::entity camera = current_camera(world, tree);
        if (camera == entt::null) {
            return;
        }
        // Only the current camera moves on, as Godot updates only the current one's scroll.
        (void)camera_transform(ecs.get<Camera2D>(camera), states[key_of(world, camera)],
                               tree.global(camera), screen_size(world),
                               static_cast<float>(tick_seconds));
    }
};

} // namespace

Affine2D camera_transform(const Camera2D& camera, CameraState& state, const Transform2D& global,
                          Vec2 screen_size, float delta) {
    // Godot's Camera2D::get_camera_transform, line for line.
    const Vec2 zoom_scale{camera.zoom.x != 0.0F ? 1.0F / camera.zoom.x : 1.0F,
                          camera.zoom.y != 0.0F ? 1.0F / camera.zoom.y : 1.0F};
    const bool drag_center = camera.anchor_mode != "fixed_top_left";
    const auto left = static_cast<float>(camera.limit_left);
    const auto top = static_cast<float>(camera.limit_top);
    const auto right = static_cast<float>(camera.limit_right);
    const auto bottom = static_cast<float>(camera.limit_bottom);
    const Vec2 new_camera_pos = global.position;
    const Vec2 size{screen_size.x * zoom_scale.x, screen_size.y * zoom_scale.y};
    const Vec2 half_screen =
        drag_center ? Vec2{screen_size.x * 0.5F * zoom_scale.x, screen_size.y * 0.5F * zoom_scale.y}
                    : Vec2{};
    Vec2& camera_pos = state.camera_pos;
    Vec2 ret_camera_pos;

    if (!state.first) {
        if (drag_center) {
            // Godot works out the margins in double.
            const double sx = static_cast<double>(screen_size.x) * 0.5;
            const double sy = static_cast<double>(screen_size.y) * 0.5;
            if (camera.drag_horizontal_enabled &&
                camera.drag_horizontal_offset == state.drag_horizontal_offset) {
                camera_pos.x = static_cast<float>(std::min<double>(
                    camera_pos.x,
                    new_camera_pos.x + (sx * zoom_scale.x * camera.drag_left_margin)));
                camera_pos.x = static_cast<float>(std::max<double>(
                    camera_pos.x,
                    new_camera_pos.x - (sx * zoom_scale.x * camera.drag_right_margin)));
            } else {
                const float margin = camera.drag_horizontal_offset < 0.0F ? camera.drag_right_margin
                                                                          : camera.drag_left_margin;
                camera_pos.x = static_cast<float>(new_camera_pos.x +
                                                  (sx * margin * camera.drag_horizontal_offset));
                state.drag_horizontal_offset = camera.drag_horizontal_offset;
            }
            if (camera.drag_vertical_enabled &&
                camera.drag_vertical_offset == state.drag_vertical_offset) {
                camera_pos.y = static_cast<float>(std::min<double>(
                    camera_pos.y, new_camera_pos.y + (sy * zoom_scale.y * camera.drag_top_margin)));
                camera_pos.y = static_cast<float>(std::max<double>(
                    camera_pos.y,
                    new_camera_pos.y - (sy * zoom_scale.y * camera.drag_bottom_margin)));
            } else {
                const float margin = camera.drag_vertical_offset < 0.0F ? camera.drag_bottom_margin
                                                                        : camera.drag_top_margin;
                camera_pos.y = static_cast<float>(new_camera_pos.y +
                                                  (sy * margin * camera.drag_vertical_offset));
                state.drag_vertical_offset = camera.drag_vertical_offset;
            }
        } else {
            camera_pos = new_camera_pos;
        }

        if (camera.limit_enabled && camera.limit_smoothed) {
            const Vec2 position{camera_pos.x - half_screen.x, camera_pos.y - half_screen.y};
            if (left > right - size.x) {
                // Narrower than the limits: centered between them.
                camera_pos.x -= position.x + ((size.x - right - left) / 2.0F);
            } else if (position.x < left) {
                camera_pos.x -= position.x - left;
            } else if (position.x + size.x > right) {
                camera_pos.x -= position.x + size.x - right;
            }
            if (top > bottom - size.y) {
                camera_pos.y -= position.y + ((size.y - bottom - top) / 2.0F);
            } else if (position.y < top) {
                camera_pos.y -= position.y - top;
            } else if (position.y + size.y > bottom) {
                camera_pos.y -= position.y + size.y - bottom;
            }
        }

        if (camera.position_smoothing_enabled) {
            const float c = camera.position_smoothing_speed * delta;
            Vec2& smoothed = state.smoothed_camera_pos;
            smoothed = {((camera_pos.x - smoothed.x) * c) + smoothed.x,
                        ((camera_pos.y - smoothed.y) * c) + smoothed.y};
            ret_camera_pos = smoothed;
        } else {
            ret_camera_pos = state.smoothed_camera_pos = camera_pos;
        }
    } else {
        ret_camera_pos = state.smoothed_camera_pos = camera_pos = new_camera_pos;
        state.first = false;
    }

    Vec2 screen_offset = half_screen;
    if (!camera.ignore_rotation) {
        if (camera.rotation_smoothing_enabled) {
            state.camera_angle = lerp_angle(state.camera_angle, global.rotation,
                                            camera.rotation_smoothing_speed * delta);
        } else {
            state.camera_angle = global.rotation;
        }
        screen_offset = rotated(screen_offset, state.camera_angle);
    }

    Vec2 position{ret_camera_pos.x - screen_offset.x, ret_camera_pos.y - screen_offset.y};
    if (camera.limit_enabled && (!camera.position_smoothing_enabled || !camera.limit_smoothed)) {
        // The corner opposite the position, rotation included: as far again past the camera
        // for drag_center, as Godot; Godot's formula takes the camera for the screen's center,
        // so a fixed_top_left screen's corner is its size away.
        Vec2 corner = camera.ignore_rotation ? size : rotated(size, state.camera_angle);
        if (drag_center) {
            corner = {2.0F * (ret_camera_pos.x - position.x),
                      2.0F * (ret_camera_pos.y - position.y)};
        }
        const Vec2 bottom_right{position.x + corner.x, position.y + corner.y};
        const Vec2 across{bottom_right.x - position.x, bottom_right.y - position.y};
        if (left > right - across.x) {
            position.x = (sum(camera.limit_left, camera.limit_right) - across.x) / 2.0F;
        } else if (position.x < left) {
            position.x = left;
        } else if (bottom_right.x > right) {
            position.x = right - across.x;
        }
        if (top > bottom - across.y) {
            position.y = (sum(camera.limit_top, camera.limit_bottom) - across.y) / 2.0F;
        } else if (position.y < top) {
            position.y = top;
        } else if (bottom_right.y > bottom) {
            position.y = bottom - across.y;
        }
    }
    if (camera.offset.x != 0.0F || camera.offset.y != 0.0F) {
        position = {position.x + camera.offset.x, position.y + camera.offset.y};
    }

    // Godot's Transform2D: scale_basis(zoom_scale), set_rotation(camera_angle), set_origin.
    Affine2D xform{.x = {zoom_scale.x, 0.0F}, .y = {0.0F, zoom_scale.y}, .origin = position};
    if (!camera.ignore_rotation) {
        const float sign = zoom_scale.x * zoom_scale.y < 0.0F ? -1.0F : 1.0F;
        const Vec2 scale{std::abs(zoom_scale.x), sign * std::abs(zoom_scale.y)};
        const Vec2 cs = cos_sin(state.camera_angle);
        const Vec2 x = normalized({cs.x, cs.y});
        const Vec2 y = normalized({-cs.y, cs.x});
        xform.x = {x.x * scale.x, x.y * scale.x};
        xform.y = {y.x * scale.y, y.y * scale.y};
    }
    return xform.inverse();
}

entt::entity current_camera(const World& world, const Tree2D& tree) {
    entt::entity best = entt::null;
    std::uint64_t best_key = std::numeric_limits<std::uint64_t>::max();
    for (const auto [entity, camera, transform] :
         world.ecs().view<const Camera2D, const Transform2D>().each()) {
        if (!camera.enabled) {
            continue;
        }
        if (const std::uint64_t key = tree.draw_key(entity); best == entt::null || key < best_key) {
            best = entity;
            best_key = key;
        }
    }
    return best;
}

CameraView camera_view(const World& world, const Tree2D& tree, Vec2 screen_size) {
    CameraView view;
    view.camera = current_camera(world, tree);
    if (view.camera == entt::null) {
        return view;
    }
    CameraState state;
    if (const auto* cameras = world.ecs().ctx().find<std::shared_ptr<Cameras>>();
        cameras != nullptr) {
        if (const auto kept = (*cameras)->states.find(key_of(world, view.camera));
            kept != (*cameras)->states.end()) {
            state = kept->second;
        }
    }
    view.canvas = camera_transform(world.ecs().get<Camera2D>(view.camera), state,
                                   tree.global(view.camera), screen_size, 0.0F);
    return view;
}

void add_camera_system(World& world, std::function<Vec2(const World&)> screen_size) {
    if (world.ecs().ctx().contains<std::shared_ptr<Cameras>>()) {
        return;
    }
    auto cameras = std::make_shared<Cameras>();
    cameras->screen_size = std::move(screen_size);
    world.ecs().ctx().emplace<std::shared_ptr<Cameras>>(cameras);
    world.add_reset([cameras](World& /*world*/) { cameras->states.clear(); });
    world.add_system(
        "camera_2d", [cameras](World& w, double tick_seconds) { cameras->step(w, tick_seconds); },
        {.doc = "Moves the current Camera2D's drag margins and smoothing on by the tick.",
         .priority = 1050});
}

void reset_camera_smoothing(World& world, entt::entity camera) {
    const auto* cameras = world.ecs().ctx().find<std::shared_ptr<Cameras>>();
    const auto* component = world.ecs().try_get<Camera2D>(camera);
    if (cameras == nullptr || component == nullptr) {
        return;
    }
    const auto kept = (*cameras)->states.find(key_of(world, camera));
    if (kept == (*cameras)->states.end() || kept->second.first) {
        return; // its first update puts it where its entity is anyway
    }
    CameraState& state = kept->second;
    const Tree2D tree(world);
    (void)camera_transform(*component, state, tree.global(camera), (*cameras)->screen_size(world),
                           0.0F);
    state.smoothed_camera_pos = state.camera_pos;
}

} // namespace slash::world
