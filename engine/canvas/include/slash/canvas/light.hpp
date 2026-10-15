#pragma once

#include "slash/core/result.hpp"
#include "slash/render/device.hpp"
#include "slash/render/light.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

// 2D lights in a world, as Godot's: PointLight2D, DirectionalLight2D, LightOccluder2D and
// CanvasModulate made into what the render device shades draws with (render::Lighting). Shadows
// are cast on the CPU into one row of render::kShadowMapWidth texels per shadowed light: a point
// light's row goes round it from -pi, holding the nearest occluder's distance at each angle, and a
// directional light's goes across its direction, holding the nearest occluder's depth along it.
namespace slash::canvas {

// The size of the light a PointLight2D with no texture gives, in pixels.
inline constexpr float kDefaultLightSize = 256.0F;
// That light's image: white, its alpha fading from 1 in the middle to 0 at the edge.
[[nodiscard]] const render::Image& default_light_image();

// A light ready to draw.
struct SceneLight {
    entt::entity entity = entt::null;
    std::string texture; // its texture's project path; "" for the default light
    render::Light light; // pack_light_atlas sets its atlas_rect
};

// What a world's lights make.
struct SceneLighting {
    // The world has an enabled light or a CanvasModulate, so its draws are shaded.
    bool lit = false;
    std::array<float, 4> ambient{1.0F, 1.0F, 1.0F, 1.0F};
    // Enabled lights in the order their entities were made, up to render::kMaxLights.
    std::vector<SceneLight> lights;
    // The shadow map: one row per light whose shadow_row is set, RGBA.
    std::uint32_t shadow_rows = 0;
    std::vector<std::uint8_t> shadow_pixels;
};

// Why a light, occluder or CanvasModulate was left out.
struct LightProblem {
    entt::entity entity = entt::null;
    std::string message;
};

// A light texture's image, by project path.
using LightImage = std::function<Result<const render::Image*>(const std::string& path)>;
// Where an entity is drawn, given its Transform2D: the renderer moves it part of the way from
// the last tick.
using TransformOf =
    std::function<world::Transform2D(entt::entity entity, const world::Transform2D& current)>;

// An occluder edge, in the frame's coordinates, from a to b.
struct OccluderEdge {
    std::array<float, 2> a{};
    std::array<float, 2> b{};
};

// Shadow rows from earlier frames, reused while their light and occluders stay put.
struct ShadowCache {
    struct Row {
        std::vector<float> inputs;
        render::Light light; // with the shadow fields cast_shadows set
        std::vector<std::array<std::uint8_t, 4>> texels;
        bool used = false;
    };
    std::unordered_map<entt::entity, Row> rows;
};

// The world's lighting. A light that can't shine, such as one whose texture doesn't load, is left
// out with a problem. transform_of and cache may be empty.
[[nodiscard]] SceneLighting collect_lighting(const world::World& world, const LightImage& images,
                                             const TransformOf& transform_of,
                                             std::vector<LightProblem>& problems,
                                             ShadowCache* cache = nullptr);

// A light's shadow row from the edges that occlude it, as render::encode_shadow_depth texels.
// A point light's position and shadow_far must be set; a directional light gets its
// shadow_origin, shadow_span, shadow_far and shadow_reach from the edges and max_distance.
[[nodiscard]] std::vector<std::array<std::uint8_t, 4>>
cast_shadows(render::Light& light, std::span<const OccluderEdge> edges, float max_distance);

// Packs the lights' textures into one image and sets each light's atlas_rect to its place.
[[nodiscard]] Result<render::Image> pack_light_atlas(std::vector<SceneLight>& lights,
                                                     const LightImage& images);

} // namespace slash::canvas
