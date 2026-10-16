#include "slash/canvas/light.hpp"

#include "slash/world/tree2d.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numbers>
#include <optional>
#include <string_view>
#include <utility>

namespace slash::canvas {
namespace {

using Point = std::array<float, 2>;

constexpr float kTwoPi = 2.0F * std::numbers::pi_v<float>;
constexpr auto kColumns = static_cast<std::int32_t>(render::kShadowMapWidth);
constexpr float kNone = std::numeric_limits<float>::infinity();
// The default light's image size.
constexpr std::uint32_t kDefaultImageSize = 128;
// Godot's 20 render layers.
constexpr std::uint32_t kMaskBits = (1U << 20U) - 1U;

float cross(Point u, Point v) {
    return (u[0] * v[1]) - (u[1] * v[0]);
}

Point minus(Point u, Point v) {
    return {u[0] - v[0], u[1] - v[1]};
}

float length(Point u) {
    return std::sqrt((u[0] * u[0]) + (u[1] * u[1]));
}

// The distance from p to the segment from a to b.
float distance_to(Point p, const OccluderEdge& edge) {
    const Point e = minus(edge.b, edge.a);
    const float size = (e[0] * e[0]) + (e[1] * e[1]);
    float t = 0.0F;
    if (size > 0.0F) {
        t = std::clamp((((p[0] - edge.a[0]) * e[0]) + ((p[1] - edge.a[1]) * e[1])) / size, 0.0F,
                       1.0F);
    }
    return length(minus(p, {edge.a[0] + (e[0] * t), edge.a[1] + (e[1] * t)}));
}

Point place(const world::Transform2D& transform, float x, float y) {
    const float sx = x * transform.scale.x;
    const float sy = y * transform.scale.y;
    const float cos_r = std::cos(transform.rotation);
    const float sin_r = std::sin(transform.rotation);
    // The y axis's angle: skew leans it from square.
    const float cos_k =
        transform.skew == 0.0F ? cos_r : std::cos(transform.rotation + transform.skew);
    const float sin_k =
        transform.skew == 0.0F ? sin_r : std::sin(transform.rotation + transform.skew);
    return {transform.position.x + (sx * cos_r) - (sy * sin_k),
            transform.position.y + (sx * sin_r) + (sy * cos_k)};
}

std::uint32_t mask_of(int mask) {
    return static_cast<std::uint32_t>(std::max(mask, 0)) & kMaskBits;
}

std::optional<render::BlendMode> light_blend(std::string_view name) {
    if (name == "add") {
        return render::BlendMode::Add;
    }
    if (name == "sub") {
        return render::BlendMode::Sub;
    }
    if (name == "mix") {
        return render::BlendMode::Mix;
    }
    return std::nullopt;
}

enum class Cull : std::uint8_t { Disabled, Clockwise, CounterClockwise };

struct Occluder {
    std::vector<OccluderEdge> edges;
    std::uint32_t mask = 1;
    Cull cull = Cull::Disabled;
};

// The fields lights of both kinds share.
template <typename LightComponent>
std::optional<std::string> fill_common(const LightComponent& component, render::Light& light) {
    const auto blend = light_blend(component.blend_mode);
    if (!blend) {
        return "blend_mode \"" + component.blend_mode + "\" is not add, sub or mix";
    }
    const auto filter = render::parse_shadow_filter(component.shadow_filter);
    if (!filter) {
        return "shadow_filter \"" + component.shadow_filter + "\" is not none, pcf5 or pcf13";
    }
    light.blend = *blend;
    light.color = {component.color.x * component.energy, component.color.y * component.energy,
                   component.color.z * component.energy, component.color.w};
    light.item_cull_mask = mask_of(component.range_item_cull_mask);
    light.z_min = component.range_z_min;
    light.z_max = component.range_z_max;
    light.shadow_filter = *filter;
    light.shadow_smooth = std::max(component.shadow_filter_smooth, 0.0F);
    light.shadow_color = {component.shadow_color.x, component.shadow_color.y,
                          component.shadow_color.z, component.shadow_color.w};
    return std::nullopt;
}

// The edges that cast shadows from a light: from occluders its mask takes, facing it as their
// cull modes allow, and for a point light, within its reach.
std::vector<OccluderEdge> edges_for(const render::Light& light, std::uint32_t shadow_mask,
                                    std::span<const Occluder> occluders) {
    std::vector<OccluderEdge> edges;
    const bool point = light.kind == render::LightKind::Point;
    for (const Occluder& occluder : occluders) {
        if ((occluder.mask & shadow_mask) == 0) {
            continue;
        }
        for (const OccluderEdge& edge : occluder.edges) {
            // Positive turns clockwise on screen, with y down.
            float turn = 0.0F;
            if (point) {
                turn = cross(minus(edge.a, light.position), minus(edge.b, light.position));
            } else {
                turn = cross(light.position, minus(edge.b, edge.a));
            }
            if ((occluder.cull == Cull::Clockwise && turn > 0.0F) ||
                (occluder.cull == Cull::CounterClockwise && turn < 0.0F)) {
                continue;
            }
            if (point && distance_to(light.position, edge) > light.shadow_far) {
                continue;
            }
            edges.push_back(edge);
        }
    }
    return edges;
}

} // namespace

const render::Image& default_light_image() {
    static const render::Image image = [] {
        render::Image made{.width = kDefaultImageSize, .height = kDefaultImageSize, .pixels = {}};
        made.pixels.reserve(std::size_t{kDefaultImageSize} * kDefaultImageSize * 4);
        constexpr float kHalf = static_cast<float>(kDefaultImageSize) / 2.0F;
        for (std::uint32_t y = 0; y < kDefaultImageSize; ++y) {
            for (std::uint32_t x = 0; x < kDefaultImageSize; ++x) {
                const float dx = (static_cast<float>(x) + 0.5F - kHalf) / kHalf;
                const float dy = (static_cast<float>(y) + 0.5F - kHalf) / kHalf;
                const float fade = std::max(0.0F, 1.0F - std::sqrt((dx * dx) + (dy * dy)));
                const auto alpha = static_cast<std::uint8_t>(std::lround(fade * fade * 255.0F));
                made.pixels.insert(made.pixels.end(), {255, 255, 255, alpha});
            }
        }
        return made;
    }();
    return image;
}

std::vector<std::array<std::uint8_t, 4>>
cast_shadows(render::Light& light, std::span<const OccluderEdge> edges, float max_distance) {
    std::vector<float> nearest(render::kShadowMapWidth, kNone);
    float scale = light.shadow_far;
    if (light.kind == render::LightKind::Point) {
        const Point center = light.position;
        for (const OccluderEdge& edge : edges) {
            // An edge through the light hides nothing it can be seen from.
            if (distance_to(center, edge) < 1.0e-4F) {
                continue;
            }
            const Point da = minus(edge.a, center);
            const Point db = minus(edge.b, center);
            const Point e = minus(edge.b, edge.a);
            const float start = ((std::atan2(da[1], da[0]) / kTwoPi) + 0.5F) * kColumns;
            const float sweep =
                std::atan2(cross(da, db), (da[0] * db[0]) + (da[1] * db[1])) / kTwoPi * kColumns;
            const auto first =
                static_cast<std::int32_t>(std::floor(std::min(start, start + sweep)));
            const auto last = static_cast<std::int32_t>(std::floor(std::max(start, start + sweep)));
            for (std::int32_t k = first; k <= last; ++k) {
                const std::int32_t column = ((k % kColumns) + kColumns) % kColumns;
                const float angle =
                    (((static_cast<float>(column) + 0.5F) / kColumns) - 0.5F) * kTwoPi;
                const Point ray{std::cos(angle), std::sin(angle)};
                // Where the ray through the column's middle meets the edge, or the nearer end.
                const float denominator = cross(ray, e);
                float s = length(da) <= length(db) ? 0.0F : 1.0F;
                if (std::abs(denominator) > 1.0e-12F) {
                    s = std::clamp(cross(da, ray) / denominator, 0.0F, 1.0F);
                }
                const float distance = length({da[0] + (e[0] * s), da[1] + (e[1] * s)});
                auto& stored = nearest.at(static_cast<std::size_t>(column));
                stored = std::min(stored, distance);
            }
        }
    } else {
        const Point along = light.position;
        const Point across{-along[1], along[0]};
        const auto project = [&](Point p) {
            return Point{(p[0] * across[0]) + (p[1] * across[1]),
                         (p[0] * along[0]) + (p[1] * along[1])};
        };
        float s_min = kNone;
        float s_max = -kNone;
        float t_min = kNone;
        float t_max = -kNone;
        for (const OccluderEdge& edge : edges) {
            for (const Point p : {project(edge.a), project(edge.b)}) {
                s_min = std::min(s_min, p[0]);
                s_max = std::max(s_max, p[0]);
                t_min = std::min(t_min, p[1]);
                t_max = std::max(t_max, p[1]);
            }
        }
        if (edges.empty()) {
            s_min = s_max = t_min = t_max = 0.0F;
        }
        // A pixel of room each side, so edges at the ends land inside the row.
        s_min -= 1.0F;
        s_max += 1.0F;
        const float span = s_max - s_min;
        scale = std::max((t_max - t_min) + std::max(max_distance, 0.0F), 1.0F);
        light.shadow_origin = {s_min, t_min};
        light.shadow_span = span;
        light.shadow_far = scale;
        light.shadow_reach = std::max(max_distance, 0.0F) / scale;
        for (const OccluderEdge& edge : edges) {
            const Point a = project(edge.a);
            const Point b = project(edge.b);
            const float ca = (a[0] - s_min) / span * kColumns;
            const float cb = (b[0] - s_min) / span * kColumns;
            const auto first = std::max(static_cast<std::int32_t>(std::floor(std::min(ca, cb))), 0);
            const auto last =
                std::min(static_cast<std::int32_t>(std::floor(std::max(ca, cb))), kColumns - 1);
            for (std::int32_t column = first; column <= last; ++column) {
                const float s = s_min + ((static_cast<float>(column) + 0.5F) / kColumns * span);
                float t = std::min(a[1], b[1]);
                if (std::abs(b[0] - a[0]) > 1.0e-6F) {
                    const float f = std::clamp((s - a[0]) / (b[0] - a[0]), 0.0F, 1.0F);
                    t = a[1] + ((b[1] - a[1]) * f);
                }
                auto& stored = nearest.at(static_cast<std::size_t>(column));
                stored = std::min(stored, t - t_min);
            }
        }
    }
    std::vector<std::array<std::uint8_t, 4>> texels;
    texels.reserve(nearest.size());
    for (const float distance : nearest) {
        texels.push_back(distance == kNone ? render::kNoShadow
                                           : render::encode_shadow_depth(distance / scale));
    }
    return texels;
}

SceneLighting collect_lighting(const world::World& world, const LightImage& images,
                               const TransformOf& transform_of, std::vector<LightProblem>& problems,
                               ShadowCache* cache) {
    SceneLighting out;
    const auto& ecs = world.ecs();
    const world::Tree2D tree(world);
    const auto transform = [&](entt::entity entity) {
        world::Transform2D placed;
        if (const auto* current = ecs.try_get<world::Transform2D>(entity); current != nullptr) {
            placed = transform_of ? transform_of(entity, *current) : tree.global(entity);
        }
        return placed;
    };
    // In the order their entities were made, as sprites draw.
    const auto in_order = [](auto view) {
        std::vector<entt::entity> entities(view.begin(), view.end());
        std::ranges::sort(entities, {}, [](entt::entity e) { return entt::to_entity(e); });
        return entities;
    };

    const std::vector<entt::entity> modulates = in_order(ecs.view<const world::CanvasModulate>());
    for (const entt::entity entity : modulates) {
        if (entity != modulates.front()) {
            problems.push_back({entity, "a scene has one CanvasModulate, and \"" +
                                            std::string(world.id_of(modulates.front())) +
                                            "\" is the one used"});
            continue;
        }
        const Vec4& color = ecs.get<world::CanvasModulate>(entity).color;
        out.ambient = {color.x, color.y, color.z, color.w};
        out.lit = true;
    }

    std::vector<Occluder> occluders;
    for (const entt::entity entity : in_order(ecs.view<const world::LightOccluder2D>())) {
        const auto& component = ecs.get<world::LightOccluder2D>(entity);
        Occluder occluder;
        occluder.mask = mask_of(component.occluder_light_mask);
        if (component.cull_mode == "clockwise") {
            occluder.cull = Cull::Clockwise;
        } else if (component.cull_mode == "counter_clockwise") {
            occluder.cull = Cull::CounterClockwise;
        } else if (component.cull_mode != "disabled") {
            problems.push_back({entity, "cull_mode \"" + component.cull_mode +
                                            "\" is not disabled, clockwise or counter_clockwise"});
            continue;
        }
        const world::Transform2D placed = transform(entity);
        std::vector<Point> points;
        points.reserve(component.polygon.size());
        for (const Vec2& p : component.polygon) {
            points.push_back(place(placed, p.x, p.y));
        }
        for (std::size_t i = 0; i + 1 < points.size(); ++i) {
            occluder.edges.push_back({points[i], points[i + 1]});
        }
        if (component.closed && points.size() > 2) {
            occluder.edges.push_back({points.back(), points.front()});
        }
        occluders.push_back(std::move(occluder));
    }

    struct Shadowed {
        std::size_t light = 0;
        std::uint32_t mask = 0;
        float max_distance = 0.0F;
    };
    std::vector<Shadowed> shadowed;
    std::vector<entt::entity> lights;
    for (const entt::entity entity : ecs.view<const world::PointLight2D>()) {
        lights.push_back(entity);
    }
    for (const entt::entity entity : ecs.view<const world::DirectionalLight2D>()) {
        lights.push_back(entity);
    }
    std::ranges::sort(lights, {}, [](entt::entity e) { return entt::to_entity(e); });
    for (const entt::entity entity : lights) {
        SceneLight scene_light{.entity = entity, .texture = {}, .light = {}};
        render::Light& light = scene_light.light;
        const world::Transform2D placed = transform(entity);
        std::optional<std::string> problem;
        if (const auto* point = ecs.try_get<world::PointLight2D>(entity); point != nullptr) {
            if (!point->enabled) {
                continue;
            }
            out.lit = true;
            problem = fill_common(*point, light);
            float width = kDefaultLightSize;
            float height = kDefaultLightSize;
            if (!problem && !point->texture.empty()) {
                auto image = images(point->texture);
                if (!image) {
                    problem = point->texture + ": " + image.error().message;
                } else {
                    width = static_cast<float>(image.value()->width);
                    height = static_cast<float>(image.value()->height);
                }
            }
            width *= point->texture_scale;
            height *= point->texture_scale;
            const float sx = placed.scale.x;
            const float sy = placed.scale.y;
            if (!problem && (!(width > 0.0F) || !(height > 0.0F) || sx == 0.0F || sy == 0.0F)) {
                problem = "its texture_scale or Transform2D scale is 0, so it lights nothing";
            }
            if (problem) {
                problems.push_back({entity, *problem});
                continue;
            }
            scene_light.texture = point->texture;
            light.kind = render::LightKind::Point;
            light.position = {placed.position.x, placed.position.y};
            light.height = point->height;
            // World to the light's own space, then to its texture: u = (local x - left) / width.
            const float cos_r = std::cos(placed.rotation);
            const float sin_r = std::sin(placed.rotation);
            const float left = point->offset.x - (width / 2.0F);
            const float top = point->offset.y - (height / 2.0F);
            const float px = placed.position.x;
            const float py = placed.position.y;
            light.texture_transform = {
                cos_r / (sx * width),
                sin_r / (sx * width),
                ((-(cos_r * px) - (sin_r * py)) / (sx * width)) - (left / width),
                -sin_r / (sy * height),
                cos_r / (sy * height),
                (((sin_r * px) - (cos_r * py)) / (sy * height)) - (top / height)};
            float far = 0.0F;
            for (const auto& [cx, cy] :
                 {std::pair{left, top}, std::pair{left + width, top}, std::pair{left, top + height},
                  std::pair{left + width, top + height}}) {
                far = std::max(far, length(minus(place(placed, cx, cy), light.position)));
            }
            light.shadow_far = std::max(far, 1.0F);
            if (point->shadow_enabled) {
                shadowed.push_back({.light = out.lights.size(),
                                    .mask = mask_of(point->shadow_item_cull_mask),
                                    .max_distance = 0.0F});
            }
        } else {
            const auto& sun = ecs.get<world::DirectionalLight2D>(entity);
            if (!sun.enabled) {
                continue;
            }
            out.lit = true;
            problem = fill_common(sun, light);
            if (problem) {
                problems.push_back({entity, *problem});
                continue;
            }
            light.kind = render::LightKind::Directional;
            // Down the screen, turned clockwise by the rotation.
            light.position = {-std::sin(placed.rotation), std::cos(placed.rotation)};
            light.height = std::clamp(sun.height, 0.0F, 1.0F);
            if (sun.shadow_enabled) {
                shadowed.push_back({.light = out.lights.size(),
                                    .mask = mask_of(sun.shadow_item_cull_mask),
                                    .max_distance = sun.max_distance});
            }
        }
        if (out.lights.size() == render::kMaxLights) {
            problems.push_back({entity, "a frame takes up to " +
                                            std::to_string(render::kMaxLights) +
                                            " lights, so this one is left out"});
            if (!shadowed.empty() && shadowed.back().light == out.lights.size()) {
                shadowed.pop_back();
            }
            continue;
        }
        out.lights.push_back(std::move(scene_light));
    }

    if (cache != nullptr) {
        for (auto& [entity, row] : cache->rows) {
            row.used = false;
        }
    }
    for (const Shadowed& entry : shadowed) {
        SceneLight& scene_light = out.lights.at(entry.light);
        render::Light& light = scene_light.light;
        const std::vector<OccluderEdge> edges = edges_for(light, entry.mask, occluders);
        if (edges.empty()) {
            continue;
        }
        std::vector<float> inputs{static_cast<float>(light.kind), light.position[0],
                                  light.position[1], light.shadow_far, entry.max_distance};
        for (const OccluderEdge& edge : edges) {
            inputs.insert(inputs.end(), {edge.a[0], edge.a[1], edge.b[0], edge.b[1]});
        }
        std::vector<std::array<std::uint8_t, 4>> texels;
        ShadowCache::Row* row = nullptr;
        if (cache != nullptr) {
            row = &cache->rows[scene_light.entity];
            row->used = true;
        }
        if (row != nullptr && row->inputs == inputs) {
            texels = row->texels;
            light.shadow_origin = row->light.shadow_origin;
            light.shadow_span = row->light.shadow_span;
            light.shadow_far = row->light.shadow_far;
            light.shadow_reach = row->light.shadow_reach;
        } else {
            texels = cast_shadows(light, edges, entry.max_distance);
            if (row != nullptr) {
                row->inputs = std::move(inputs);
                row->light = light;
                row->texels = texels;
            }
        }
        light.shadow_row = static_cast<std::int32_t>(out.shadow_rows++);
        for (const auto& texel : texels) {
            out.shadow_pixels.insert(out.shadow_pixels.end(), texel.begin(), texel.end());
        }
    }
    if (cache != nullptr) {
        std::erase_if(cache->rows, [](const auto& entry) { return !entry.second.used; });
    }
    return out;
}

Result<render::Image> pack_light_atlas(std::vector<SceneLight>& lights, const LightImage& images) {
    struct Placed {
        const render::Image* image = nullptr;
        std::uint32_t x = 0;
        std::uint32_t y = 0;
    };
    std::map<std::string, Placed> placed;
    std::vector<std::string> order;
    for (const SceneLight& light : lights) {
        if (light.light.kind != render::LightKind::Point || placed.contains(light.texture)) {
            continue;
        }
        Placed entry;
        if (light.texture.empty()) {
            entry.image = &default_light_image();
        } else {
            auto image = images(light.texture);
            if (!image) {
                return make_error(image.error().code, light.texture + ": " + image.error().message);
            }
            entry.image = image.value();
        }
        placed.emplace(light.texture, entry);
        order.push_back(light.texture);
    }
    // Shelves, tallest first, a pixel apart.
    std::ranges::stable_sort(order, std::greater<>(), [&](const std::string& path) {
        return placed.at(path).image->height;
    });
    std::uint64_t area = 0;
    std::uint32_t widest = 1;
    for (const std::string& path : order) {
        const render::Image& image = *placed.at(path).image;
        area += std::uint64_t{image.width + 1} * (image.height + 1);
        widest = std::max(widest, image.width);
    }
    std::uint32_t width = 1;
    while (width < widest || std::uint64_t{width} * width < area) {
        width *= 2;
    }
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t shelf = 0;
    for (const std::string& path : order) {
        Placed& entry = placed.at(path);
        if (x + entry.image->width > width) {
            x = 0;
            y += shelf + 1;
            shelf = 0;
        }
        entry.x = x;
        entry.y = y;
        x += entry.image->width + 1;
        shelf = std::max(shelf, entry.image->height);
    }
    const std::uint32_t height = std::max(y + shelf, 1U);
    constexpr std::uint32_t kMaxSize = 16384;
    if (width > kMaxSize || height > kMaxSize) {
        return make_error(ErrorCode::InvalidArgument,
                          "the lights' textures don't fit in one 16384x16384 atlas");
    }
    render::Image atlas{.width = width, .height = height, .pixels = {}};
    atlas.pixels.assign(std::size_t{width} * height * 4, 0);
    for (const auto& [path, entry] : placed) {
        const render::Image& image = *entry.image;
        for (std::uint32_t row = 0; row < image.height; ++row) {
            std::copy_n(image.pixels.data() + (std::size_t{row} * image.width * 4),
                        std::size_t{image.width} * 4,
                        atlas.pixels.data() +
                            ((((std::size_t{entry.y} + row) * width) + entry.x) * 4));
        }
    }
    // Texel middles at the edges, so filtering never reads a neighbor.
    const auto w = static_cast<float>(width);
    const auto h = static_cast<float>(height);
    for (SceneLight& light : lights) {
        if (light.light.kind != render::LightKind::Point) {
            continue;
        }
        const Placed& entry = placed.at(light.texture);
        light.light.atlas_rect = {(static_cast<float>(entry.x) + 0.5F) / w,
                                  (static_cast<float>(entry.y) + 0.5F) / h,
                                  static_cast<float>(entry.image->width - 1) / w,
                                  static_cast<float>(entry.image->height - 1) / h};
    }
    return atlas;
}

} // namespace slash::canvas
