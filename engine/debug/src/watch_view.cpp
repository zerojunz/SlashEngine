#include "slash/debug/watch_view.hpp"

#include "slash/core/assert.hpp"
#include "slash/navigation/navigation.hpp"
#include "slash/scene3d/scene3d.hpp"
#include "slash/ui/ui.hpp"
#include "slash/world/components.hpp"
#include "slash/world/scene.hpp"
#include "slash/world/tree2d.hpp"
#include "slash/world/tree3d.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

namespace slash::debug {
namespace {

// The bar along the top: the session and what the agent did last. Its controls take no input.
constexpr std::string_view kBar = R"({
  "format": 1,
  "entities": {
    "bar": {"components": {
      "Control": {"children": ["status"], "anchor_right": 1, "mouse_filter": "ignore",
                  "theme_override_styles": {"panel": {"bg_color": [0.06, 0.07, 0.09, 0.85],
                                                      "content_margin": 6}}},
      "PanelContainer": {}}},
    "status": {"components": {
      "Control": {"mouse_filter": "ignore"},
      "Label": {"text_overrun_behavior": "trim_ellipsis"}}}
  }
})";

// Outlines' colors: made, then changed.
constexpr Vec4 kAddedColor{0.30F, 0.85F, 0.45F, 1.0F};
constexpr Vec4 kChangedColor{1.0F, 0.72F, 0.20F, 1.0F};
// Pixels an outline keeps round what it outlines, and the square round an entity with nothing
// drawn, such as an empty 2D entity or a 3D one with no geometry.
constexpr float kMargin = 3.0F;
constexpr float kMarker = 12.0F;
// Touches kept, newest last; older ones fade out early.
constexpr std::size_t kMaxTouches = 64;

struct Rect {
    float min_x = std::numeric_limits<float>::max();
    float min_y = std::numeric_limits<float>::max();
    float max_x = std::numeric_limits<float>::lowest();
    float max_y = std::numeric_limits<float>::lowest();

    void add(float x, float y) {
        min_x = std::min(min_x, x);
        min_y = std::min(min_y, y);
        max_x = std::max(max_x, x);
        max_y = std::max(max_y, y);
    }
    [[nodiscard]] bool empty() const { return min_x > max_x; }
};

// Whether entity is ancestor or ancestor itself, by a tree's parent links.
template <typename Tree>
bool under(const Tree& tree, entt::entity entity, entt::entity ancestor) {
    for (entt::entity at = entity; at != entt::null; at = tree.parent(at)) {
        if (at == ancestor) {
            return true;
        }
    }
    return false;
}

// A world point on the screen through a camera's view and projection (column-major), or nothing
// for a point behind the camera.
std::optional<std::array<float, 2>> project(const scene3d::CameraView& view, Vec3 point,
                                            Vec2 viewport) {
    const std::array<float, 4> p{static_cast<float>(point.x), static_cast<float>(point.y),
                                 static_cast<float>(point.z), 1.0F};
    std::array<float, 4> eye{};
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t col = 0; col < 4; ++col) {
            eye[row] += view.view[(col * 4) + row] * p[col];
        }
    }
    std::array<float, 4> clip{};
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t col = 0; col < 4; ++col) {
            clip[row] += view.projection[(col * 4) + row] * eye[col];
        }
    }
    if (!(clip[3] > 1.0e-4F)) {
        return std::nullopt;
    }
    const float x = clip[0] / clip[3];
    const float y = clip[1] / clip[3];
    return std::array<float, 2>{(x + 1.0F) * 0.5F * viewport.x, (1.0F - y) * 0.5F * viewport.y};
}

json::Value outline_style(Vec4 color) {
    json::Value style;
    style["draw_center"] = false;
    style["border_width"] = 2;
    style["border_color"] = json::Array{color.x, color.y, color.z, color.w};
    return style;
}

json::Value name_style(Vec4 color) {
    json::Value style;
    style["bg_color"] = json::Array{color.x * 0.35F, color.y * 0.35F, color.z * 0.35F, 0.9F};
    style["content_margin"] = 2;
    return style;
}

} // namespace

WatchView::WatchView(asset::Assets& assets) : world_(components_) {
    world::register_builtin_components(components_);
    // The bar's text is ASCII and IDs, in the engine's own font.
    ui::Settings settings;
    settings.allow_system_fallback = false;
    ui::install(world_, assets, settings);
    auto bar = json::parse(kBar);
    SLASH_ASSERT(bar.ok(), "the watch view's bar parses");
    const auto loaded = world::load_scene(world_, bar.value());
    SLASH_ASSERT(loaded.ok(), "the watch view's bar loads");
}

WatchView::~WatchView() = default;

void WatchView::touch(std::string_view id, bool added, double seconds) {
    if (id.empty()) {
        return;
    }
    // An entity touched again moves to the end, colored by the latest touch.
    if (const auto it = std::ranges::find(touches_, id, &Touch::id); it != touches_.end()) {
        touches_.erase(it);
    }
    touches_.push_back({std::string(id), added, seconds});
    if (touches_.size() > kMaxTouches) {
        touches_.erase(touches_.begin());
    }
}

void WatchView::touch(const json::Value& line, double seconds) {
    const json::Value* changes = line.find("changes");
    if (changes == nullptr || !changes->is_object()) {
        return;
    }
    if (const json::Value* added = changes->find("added"); added != nullptr && added->is_array()) {
        for (const json::Value& id : added->as_array()) {
            if (id.is_string()) {
                touch(id.as_string(), true, seconds);
            }
        }
    }
    if (const json::Value* changed = changes->find("changed");
        changed != nullptr && changed->is_object()) {
        for (const auto& [id, unused] : changed->as_object()) {
            touch(id, false, seconds);
        }
    }
    if (const json::Value* removed = changes->find("removed");
        removed != nullptr && removed->is_array()) {
        for (const json::Value& id : removed->as_array()) {
            if (id.is_string()) {
                std::erase_if(touches_, [&](const Touch& t) { return t.id == id.as_string(); });
            }
        }
    }
}

void WatchView::set_status(std::string text) {
    status_ = std::move(text);
}

void WatchView::update(const world::World& watched, asset::Assets& assets,
                       const canvas::FrameContents& contents, Vec2 viewport, double seconds,
                       bool world_changed) {
    std::erase_if(touches_, [&](const Touch& t) {
        return seconds - t.seconds >= kOutlineHold + kOutlineFade;
    });
    outlines_.clear();
    if (world_changed) {
        boxes_.clear();
    }
    bool any_3d = false;
    for (const Touch& t : touches_) {
        const entt::entity entity = watched.find(t.id);
        any_3d =
            any_3d || (entity != entt::null && watched.ecs().all_of<world::Transform3D>(entity));
    }
    if (any_3d && boxes_.empty()) {
        for (const navigation::EntityGeometry& own :
             navigation::world_geometry(watched, assets, navigation::NavigationMesh{})) {
            const std::vector<float>& points = own.geometry.vertices;
            if (points.size() < 3) {
                continue;
            }
            Box3 box{.min = {points[0], points[1], points[2]},
                     .max = {points[0], points[1], points[2]}};
            for (std::size_t i = 3; i + 2 < points.size(); i += 3) {
                box.min = {std::min<real_t>(box.min.x, points[i]),
                           std::min<real_t>(box.min.y, points[i + 1]),
                           std::min<real_t>(box.min.z, points[i + 2])};
                box.max = {std::max<real_t>(box.max.x, points[i]),
                           std::max<real_t>(box.max.y, points[i + 1]),
                           std::max<real_t>(box.max.z, points[i + 2])};
            }
            boxes_[own.entity] = box;
        }
    }

    const world::Tree2D tree2d(watched);
    std::optional<world::Tree3D> tree3d;
    std::optional<scene3d::CameraView> camera;
    if (any_3d) {
        tree3d.emplace(watched);
        const entt::entity current = scene3d::current_camera(watched, *tree3d);
        if (current != entt::null && viewport.y > 0.0F) {
            camera = scene3d::camera_view(watched.ecs().get<world::Camera3D>(current),
                                          tree3d->global(current), viewport.x / viewport.y);
        }
    }

    for (const Touch& t : touches_) {
        const entt::entity entity = watched.find(t.id);
        if (entity == entt::null) {
            continue;
        }
        Rect rect;
        if (watched.ecs().all_of<world::Transform3D>(entity)) {
            if (!camera) {
                continue;
            }
            const auto add = [&](Vec3 point) {
                if (const auto at = project(*camera, point, viewport)) {
                    rect.add((*at)[0], (*at)[1]);
                }
            };
            for (const auto& [owner, box] : boxes_) {
                if (!under(*tree3d, owner, entity)) {
                    continue;
                }
                for (int corner = 0; corner < 8; ++corner) {
                    add({(corner & 1) != 0 ? box.max.x : box.min.x,
                         (corner & 2) != 0 ? box.max.y : box.min.y,
                         (corner & 4) != 0 ? box.max.z : box.min.z});
                }
            }
            if (rect.empty()) {
                if (const auto at = project(*camera, tree3d->global(entity).origin, viewport)) {
                    rect.add((*at)[0] - (kMarker / 2), (*at)[1] - (kMarker / 2));
                    rect.add((*at)[0] + (kMarker / 2), (*at)[1] + (kMarker / 2));
                }
            }
        } else {
            for (const canvas::FrameItem& item : contents.items) {
                if (item.entity == entt::null || !under(tree2d, item.entity, entity)) {
                    continue;
                }
                for (const canvas::FramePiece& piece : item.pieces) {
                    for (const render::Vertex2D& vertex : piece.vertices) {
                        rect.add(vertex.x, vertex.y);
                    }
                }
            }
            if (rect.empty() && watched.ecs().all_of<world::Transform2D>(entity)) {
                const Vec2 origin = tree2d.global(entity).position;
                const Vec2 at = canvas::to_screen(watched, origin, entity);
                rect.add(at.x - (kMarker / 2), at.y - (kMarker / 2));
                rect.add(at.x + (kMarker / 2), at.y + (kMarker / 2));
            }
        }
        if (rect.empty()) {
            continue;
        }
        const double age = seconds - t.seconds;
        const double alpha =
            age <= kOutlineHold ? 1.0 : 1.0 - ((age - kOutlineHold) / kOutlineFade);
        outlines_.push_back({.id = t.id,
                             .added = t.added,
                             .x = rect.min_x - kMargin,
                             .y = rect.min_y - kMargin,
                             .width = rect.max_x - rect.min_x + (2 * kMargin),
                             .height = rect.max_y - rect.min_y + (2 * kMargin),
                             .alpha = static_cast<float>(std::clamp(alpha, 0.0, 1.0))});
    }
    lay_out(viewport);
}

void WatchView::lay_out(Vec2 viewport) {
    ui::set_viewport(world_, viewport);
    entt::registry& registry = world_.ecs();
    const entt::entity status = world_.find("status");
    registry.get<world::Label>(status).text = status_;
    registry.get<world::Control>(world_.find("bar")).visible = !status_.empty();

    // A box and a name tag per outline, made as more are needed and hidden when unused.
    while (shown_ < outlines_.size()) {
        const std::string box_id = "outline_" + std::to_string(shown_);
        const std::string name_id = box_id + "_name";
        const entt::entity box = world_.create(box_id).value();
        const entt::entity name = world_.create(name_id).value();
        world::Control box_control;
        box_control.mouse_filter = "ignore";
        registry.emplace<world::Control>(box, box_control);
        registry.emplace<world::Panel>(box);
        world::Control name_control;
        name_control.mouse_filter = "ignore";
        registry.emplace<world::Control>(name, name_control);
        registry.emplace<world::Label>(name);
        ++shown_;
    }
    for (std::size_t i = 0; i < shown_; ++i) {
        const std::string box_id = "outline_" + std::to_string(i);
        world::Control& box = registry.get<world::Control>(world_.find(box_id));
        const entt::entity name_entity = world_.find(box_id + "_name");
        world::Control& name = registry.get<world::Control>(name_entity);
        if (i >= outlines_.size()) {
            box.visible = false;
            name.visible = false;
            continue;
        }
        const Outline& outline = outlines_[i];
        const Vec4 color = outline.added ? kAddedColor : kChangedColor;
        box.visible = true;
        box.offset_left = outline.x;
        box.offset_top = outline.y;
        box.offset_right = outline.x + outline.width;
        box.offset_bottom = outline.y + outline.height;
        box.modulate = {1.0F, 1.0F, 1.0F, outline.alpha};
        box.theme_override_styles["panel"] = outline_style(color);
        // The name sits on the box's top edge, inside the window.
        name.visible = true;
        name.offset_left = std::max(outline.x, 0.0F);
        name.offset_top = std::max(outline.y - 18.0F, 0.0F);
        name.offset_right = name.offset_left;
        name.offset_bottom = name.offset_top;
        name.modulate = {1.0F, 1.0F, 1.0F, outline.alpha};
        name.theme_override_styles["normal"] = name_style(color);
        name.theme_override_font_sizes["font_size"] = 12;
        registry.get<world::Label>(name_entity).text = outline.id;
    }
}

} // namespace slash::debug
