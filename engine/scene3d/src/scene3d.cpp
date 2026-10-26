#include "slash/scene3d/scene3d.hpp"

#include "slash/core/json.hpp"
#include "slash/core/math.hpp"
#include "slash/core/text.hpp"
#include "slash/scene3d/bone_attachment.hpp"
#include "slash/scene3d/csg.hpp"
#include "slash/scene3d/gridmap.hpp"
#include "slash/scene3d/ik.hpp"
#include "slash/scene3d/lightmap.hpp"
#include "slash/scene3d/terrain.hpp"
#include "slash/scene3d/voxel_gi.hpp"
#include "slash/world/path.hpp"
#include "slash/world/serialize.hpp"
#include "slash/world/world.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace slash::scene3d {

namespace {

// A lit surface's triangles to the end of its unwrap.
constexpr std::uint32_t kAllIndices = std::numeric_limits<std::uint32_t>::max();

constexpr float kDegrees = 180.0F / std::numbers::pi_v<float>;
constexpr float kRadians = std::numbers::pi_v<float> / 180.0F;
// rendering/lightmapping/probe_capture/update_speed: how fast moving meshes' light from light
// probes follows them, a share per second.
constexpr double kProbeUpdateSpeed = 15.0;

Vec3 scaled(Vec3 v, float s) {
    return {v.x * s, v.y * s, v.z * s};
}

float dot(Vec3 a, Vec3 b) {
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

Vec3 normalized(Vec3 v) {
    const float length = std::sqrt(dot(v, v));
    return length == 0.0F ? v : scaled(v, 1.0F / length);
}

std::array<float, 3> rgb(const Vec4& color) {
    return {color.x, color.y, color.z};
}

// The entities with a component in the order they were made.
template <typename T>
std::vector<entt::entity> made(const world::World& world) {
    std::vector<entt::entity> out;
    for (const entt::entity entity : world.ecs().view<const T>()) {
        out.push_back(entity);
    }
    std::ranges::sort(out, {}, [](entt::entity entity) { return entt::to_entity(entity); });
    return out;
}

// The bounds of vertices, in their own space.
Bounds bounds_of(std::span<const render::Vertex3D> vertices) {
    if (vertices.empty()) {
        return {};
    }
    const auto& first = vertices.front().position;
    Bounds out{.min = {first[0], first[1], first[2]}, .max = {first[0], first[1], first[2]}};
    for (const render::Vertex3D& vertex : vertices) {
        const auto& p = vertex.position;
        out.min = {std::min(out.min.x, p[0]), std::min(out.min.y, p[1]), std::min(out.min.z, p[2])};
        out.max = {std::max(out.max.x, p[0]), std::max(out.max.y, p[1]), std::max(out.max.z, p[2])};
    }
    return out;
}

render::TextureChannel channel_of(const std::string& name) {
    if (name == "green") {
        return render::TextureChannel::Green;
    }
    if (name == "blue") {
        return render::TextureChannel::Blue;
    }
    if (name == "alpha") {
        return render::TextureChannel::Alpha;
    }
    if (name == "gray") {
        return render::TextureChannel::Gray;
    }
    return render::TextureChannel::Red;
}

// The light's own fields, which the three kinds share.
template <typename T>
render::Light3D light_base(const T& light, render::LightType type) {
    render::Light3D out;
    out.type = type;
    out.color = rgb(light.light_color);
    out.energy = light.light_negative ? -light.light_energy : light.light_energy;
    out.specular = light.light_specular;
    out.cull_mask = light.light_cull_mask;
    out.shadow = light.shadow_enabled;
    out.shadow_bias = light.shadow_bias;
    out.shadow_normal_bias = light.shadow_normal_bias;
    out.shadow_opacity = light.shadow_opacity;
    out.shadow_reverse_cull_face = light.shadow_reverse_cull_face;
    out.shadow_caster_mask = light.shadow_caster_mask;
    out.baked = light.light_bake_mode == "static";
    return out;
}

render::DirectionalShadowMode shadow_mode_of(const std::string& name) {
    if (name == "orthogonal") {
        return render::DirectionalShadowMode::Orthogonal;
    }
    if (name == "parallel_2_splits") {
        return render::DirectionalShadowMode::Parallel2Splits;
    }
    return render::DirectionalShadowMode::Parallel4Splits;
}

render::ShadowCasting casting_of(const std::string& name) {
    if (name == "off") {
        return render::ShadowCasting::Off;
    }
    if (name == "double_sided") {
        return render::ShadowCasting::DoubleSided;
    }
    if (name == "shadows_only") {
        return render::ShadowCasting::ShadowsOnly;
    }
    return render::ShadowCasting::On;
}

// A light that shines, the entity it is on and its place in tree order.
struct Shone {
    std::size_t place = 0;
    entt::entity entity = entt::null;
    render::Light3D light;
};

// Each light of type T that shines, with where it is.
template <typename T>
void add_lights(const world::World& world, const world::Tree3D& tree,
                const std::function<world::Affine3D(entt::entity)>& global,
                const std::unordered_map<entt::entity, std::size_t>& order,
                std::vector<Shone>& out) {
    for (const entt::entity entity : made<T>(world)) {
        if (!tree.visible(entity)) {
            continue;
        }
        const T& light = world.ecs().get<const T>(entity);
        render::Light3D shone;
        if constexpr (std::is_same_v<T, world::DirectionalLight3D>) {
            if (light.sky_mode == "sky_only") {
                continue;
            }
            shone = light_base(light, render::LightType::Directional);
            shone.shadow_mode = shadow_mode_of(light.directional_shadow_mode);
            shone.shadow_splits = {light.directional_shadow_split_1,
                                   light.directional_shadow_split_2,
                                   light.directional_shadow_split_3};
            shone.shadow_blend_splits = light.directional_shadow_blend_splits;
            shone.shadow_fade_start = light.directional_shadow_fade_start;
            shone.shadow_max_distance = light.directional_shadow_max_distance;
            shone.shadow_pancake_size = light.directional_shadow_pancake_size;
        } else if constexpr (std::is_same_v<T, world::OmniLight3D>) {
            shone = light_base(light, render::LightType::Omni);
            shone.range = light.omni_range;
            shone.attenuation = light.omni_attenuation;
        } else {
            shone = light_base(light, render::LightType::Spot);
            shone.range = light.spot_range;
            shone.attenuation = light.spot_attenuation;
            shone.spot_angle = light.spot_angle;
            shone.spot_attenuation = light.spot_angle_attenuation;
        }
        const world::Affine3D placed = global(entity);
        shone.position = {placed.origin.x, placed.origin.y, placed.origin.z};
        const Vec3 forward = normalized(scaled(placed.z, -1.0F));
        shone.direction = {forward.x, forward.y, forward.z};
        const Vec3 up = normalized(placed.y);
        shone.up = {up.x, up.y, up.z};
        const auto at = order.find(entity);
        out.push_back({.place = at != order.end() ? at->second : order.size(),
                       .entity = entity,
                       .light = shone});
    }
}

// Every light that shines, in tree order.
std::vector<Shone> shining(const world::World& world, const world::Tree3D& tree,
                           const std::function<world::Affine3D(entt::entity)>& global) {
    std::unordered_map<entt::entity, std::size_t> order;
    for (std::size_t i = 0; i < tree.order().size(); ++i) {
        order.emplace(tree.order()[i], i);
    }
    std::vector<Shone> found;
    add_lights<world::DirectionalLight3D>(world, tree, global, order, found);
    add_lights<world::OmniLight3D>(world, tree, global, order, found);
    add_lights<world::SpotLight3D>(world, tree, global, order, found);
    std::ranges::stable_sort(found, {}, &Shone::place);
    return found;
}

std::array<float, 4> rgba(const Vec4& color) {
    return {color.x, color.y, color.z, color.w};
}

// The sky material on a WorldEnvironment's entity as a frame's sky, as Godot's Sky: the first of
// ProceduralSkyMaterial, PhysicalSkyMaterial and PanoramaSkyMaterial it has, or a default
// ProceduralSkyMaterial, as Godot's new scenes have.
void add_sky(const world::World& world, entt::entity entity, Environment& out) {
    const auto& ecs = world.ecs();
    const auto* procedural = ecs.try_get<const world::ProceduralSkyMaterial>(entity);
    const auto* physical = ecs.try_get<const world::PhysicalSkyMaterial>(entity);
    const auto* panorama = ecs.try_get<const world::PanoramaSkyMaterial>(entity);
    const char* ignored = nullptr;
    if (procedural != nullptr && physical != nullptr) {
        ignored = "PhysicalSkyMaterial";
    } else if ((procedural != nullptr || physical != nullptr) && panorama != nullptr) {
        ignored = "PanoramaSkyMaterial";
    }
    if (ignored != nullptr) {
        out.problems.push_back({entity, std::string("/components/") + ignored,
                                "is a second sky material, which draws nothing: keep one of "
                                "ProceduralSkyMaterial, PhysicalSkyMaterial and "
                                "PanoramaSkyMaterial"});
    }
    render::Sky3D& sky = out.sky;
    if (procedural != nullptr || (physical == nullptr && panorama == nullptr)) {
        const world::ProceduralSkyMaterial m =
            procedural != nullptr ? *procedural : world::ProceduralSkyMaterial{};
        sky.kind = render::SkyKind::Procedural;
        render::ProceduralSky& p = sky.procedural;
        p.sky_top_color = rgb(m.sky_top_color);
        p.sky_horizon_color = rgb(m.sky_horizon_color);
        p.sky_curve = m.sky_curve;
        p.sky_energy_multiplier = m.sky_energy_multiplier;
        p.sky_cover_modulate = rgba(m.sky_cover_modulate);
        p.ground_bottom_color = rgb(m.ground_bottom_color);
        p.ground_horizon_color = rgb(m.ground_horizon_color);
        p.ground_curve = m.ground_curve;
        p.ground_energy_multiplier = m.ground_energy_multiplier;
        p.sun_angle_max = m.sun_angle_max;
        p.sun_curve = m.sun_curve;
        p.use_debanding = m.use_debanding;
        p.energy_multiplier = m.energy_multiplier;
        out.sky_texture = m.sky_cover;
        out.sky_texture_field = "/components/ProceduralSkyMaterial/sky_cover";
    } else if (physical != nullptr) {
        const world::PhysicalSkyMaterial& m = *physical;
        sky.kind = render::SkyKind::Physical;
        render::PhysicalSky& p = sky.physical;
        p.rayleigh_coefficient = m.rayleigh_coefficient;
        p.rayleigh_color = rgb(m.rayleigh_color);
        p.mie_coefficient = m.mie_coefficient;
        p.mie_eccentricity = m.mie_eccentricity;
        p.mie_color = rgb(m.mie_color);
        p.turbidity = m.turbidity;
        p.sun_disk_scale = m.sun_disk_scale;
        p.ground_color = rgb(m.ground_color);
        p.energy_multiplier = m.energy_multiplier;
        p.use_debanding = m.use_debanding;
        out.sky_texture = m.night_sky;
        out.sky_texture_field = "/components/PhysicalSkyMaterial/night_sky";
    } else {
        sky.kind = render::SkyKind::Panorama;
        sky.panorama.energy_multiplier = panorama->energy_multiplier;
        out.sky_texture = panorama->panorama;
        out.sky_filter = panorama->filter ? render::Filter::Linear : render::Filter::Nearest;
        out.sky_texture_field = "/components/PanoramaSkyMaterial/panorama";
    }
}

constexpr std::string_view kNoMesh = "has no mesh: set mesh to a .gltf, .glb, .obj, .fbx or .blend "
                                     "file, or add a BoxMesh, SphereMesh, "
                                     "CylinderMesh, "
                                     "CapsuleMesh, PlaneMesh, QuadMesh, PrismMesh or TorusMesh";
constexpr std::string_view kMeshField = "/components/MeshInstance3D/mesh";

// What's wrong with a MeshInstance3D's mesh path, without reading it; "" for nothing.
std::string mesh_path_problem(const world::World& world, entt::entity entity,
                              const std::string& path) {
    if (!is_project_path(path)) {
        return "expected a path inside the project folder, separated by '/', with no \".\" or "
               "\"..\" parts";
    }
    if (!is_model_path(path)) {
        return "expected a .gltf, .glb, .obj, .fbx or .blend file";
    }
    auto primitive = primitive_of(world, entity);
    if (!primitive) {
        return {};
    }
    const std::optional<Primitive>& found = primitive.value();
    if (found) {
        return "draws a model, so its " + std::string(kPrimitiveNames[found->index()]) +
               " is left out: remove one of them";
    }
    return {};
}

// The entity's primitive mesh when it can draw one; otherwise nullopt, with the problem.
std::optional<Primitive> drawable(const world::World& world, entt::entity entity,
                                  std::vector<Problem>& problems) {
    auto primitive = primitive_of(world, entity);
    if (!primitive) {
        problems.push_back({entity, "/components", primitive.error().message});
        return std::nullopt;
    }
    const std::optional<Primitive>& found = primitive.value();
    if (!found) {
        problems.push_back({entity, "/components/MeshInstance3D", std::string(kNoMesh)});
        return std::nullopt;
    }
    const Primitive& mesh = *found;
    if (auto checked = check_primitive(mesh); !checked) {
        problems.push_back({entity, "/components/" + std::string(kPrimitiveNames[mesh.index()]),
                            checked.error().message});
        return std::nullopt;
    }
    return mesh;
}

} // namespace

entt::entity current_camera(const world::World& world, const world::Tree3D& tree) {
    return world::current_camera(world, tree);
}

CameraView camera_view(const world::Camera3D& camera, const world::Affine3D& global, float aspect) {
    world::Affine3D eye = orthonormalized(global);
    eye.origin = eye.origin + scaled(eye.y, camera.v_offset) + scaled(eye.x, camera.h_offset);
    CameraView out;
    out.view = eye.inverse().matrix();
    const bool keep_width = camera.keep_aspect == "width";
    if (camera.projection == "orthogonal") {
        const float size = keep_width ? camera.size / aspect : camera.size;
        out.projection = render::orthogonal(size, aspect, camera.near, camera.far);
    } else {
        float fov_y = camera.fov;
        if (keep_width) {
            // Godot's Projection::get_fovy with 1 / aspect: the height's angle for a width's.
            const Vec2 half = cos_sin(camera.fov * 0.5F * kRadians);
            fov_y = atan2(half.y / half.x, aspect) * 2.0F * kDegrees;
        }
        out.projection = render::perspective(fov_y, aspect, camera.near, camera.far);
    }
    return out;
}

Environment world_environment(const world::World& world, render::Color clear) {
    Environment out;
    out.background = clear;
    const auto environments = made<world::WorldEnvironment>(world);
    if (environments.empty()) {
        return out;
    }
    const auto& env = world.ecs().get<const world::WorldEnvironment>(environments.front());
    render::Color background = clear;
    if (env.background_mode == "color") {
        background = {.r = env.background_color.x,
                      .g = env.background_color.y,
                      .b = env.background_color.z,
                      .a = env.background_color.w};
    }
    const float energy = env.background_energy_multiplier;
    out.background = {.r = background.r * energy,
                      .g = background.g * energy,
                      .b = background.b * energy,
                      .a = background.a};
    out.entity = environments.front();
    const bool sky = env.background_mode == "sky";
    // Godot's RasterizerSceneGLES3::_setup_environment: a sky's light (its radiance) lights
    // surfaces through ambient_light_color, by ambient_light_sky_contribution.
    const bool sky_ambient =
        env.ambient_light_source == "sky" || (env.ambient_light_source == "background" && sky);
    const bool sky_reflections =
        env.reflected_light_source == "sky" || (env.reflected_light_source == "background" && sky);
    if (env.ambient_light_source == "background" && !sky) {
        out.ambient = {background.r, background.g, background.b};
        out.ambient_energy = energy;
    } else if (env.ambient_light_source == "color" || sky_ambient) {
        out.ambient = rgb(env.ambient_light_color);
        out.ambient_energy = env.ambient_light_energy;
    }
    if (env.fog_enabled) {
        render::Fog3D& fog = out.fog;
        fog.enabled = true;
        fog.depth = env.fog_mode == "depth";
        fog.light_color = rgb(env.fog_light_color);
        fog.light_energy = env.fog_light_energy;
        fog.sun_scatter = env.fog_sun_scatter;
        fog.density = env.fog_density;
        fog.aerial_perspective = env.fog_aerial_perspective;
        fog.sky_affect = env.fog_sky_affect;
        fog.height = env.fog_height;
        fog.height_density = env.fog_height_density;
        fog.depth_curve = env.fog_depth_curve;
        fog.depth_begin = env.fog_depth_begin;
        fog.depth_end = env.fog_depth_end;
    }
    if (env.ssr_enabled) {
        out.ssr = {.enabled = true,
                   .max_steps = env.ssr_max_steps,
                   .fade_in = env.ssr_fade_in,
                   .fade_out = env.ssr_fade_out,
                   .depth_tolerance = env.ssr_depth_tolerance,
                   .settings = {}};
    }
    if (env.ssao_enabled) {
        out.ssao = {.enabled = true,
                    .radius = env.ssao_radius,
                    .intensity = env.ssao_intensity,
                    .power = env.ssao_power,
                    .detail = env.ssao_detail,
                    .horizon = env.ssao_horizon,
                    .sharpness = env.ssao_sharpness,
                    .light_affect = env.ssao_light_affect,
                    .ao_channel_affect = env.ssao_ao_channel_affect};
    }
    out.sky.energy = energy;
    if (sky || sky_ambient || sky_reflections) {
        add_sky(world, out.entity, out);
        out.sky_material = out.sky.kind;
        out.sky.custom_fov = env.sky_custom_fov;
        // The world to the sky's space: the inverse, so the transpose, of the sky's turn.
        const world::Affine3D turn = world::rotation_basis(world::quaternion(env.sky_rotation));
        const std::array<Vec3, 3> columns{turn.x, turn.y, turn.z};
        for (std::size_t row = 0; row < 3; ++row) {
            out.sky.orientation.at(row) = columns.at(row).x;
            out.sky.orientation.at(4 + row) = columns.at(row).y;
            out.sky.orientation.at(8 + row) = columns.at(row).z;
        }
    }
    if (sky_ambient || sky_reflections) {
        render::Radiance3D& radiance = out.radiance;
        radiance.kind = out.sky_material;
        radiance.ambient = sky_ambient;
        radiance.ambient_mix = env.ambient_light_sky_contribution;
        radiance.reflections = sky_reflections;
        const std::uint32_t size = env.sky_radiance_size;
        if (size >= 32 && size <= 2048 && std::has_single_bit(size)) {
            radiance.size = size;
        } else {
            out.problems.push_back({out.entity, "/components/WorldEnvironment/sky_radiance_size",
                                    "is 32, 64, 128, 256, 512, 1024 or 2048, not " +
                                        std::to_string(size) + "; drawing it at 256"});
        }
    }
    if (!sky) {
        out.sky.kind = render::SkyKind::None;
        if (env.fog_enabled) {
            // Fog over the background, as Godot draws it: a sky of the background's color,
            // which its energy multiplies twice, as Godot's fog sky material does.
            out.sky.kind = render::SkyKind::Color;
            out.sky.color = {out.background.r, out.background.g, out.background.b};
        }
    }
    for (std::size_t i = 0; i < render::kTonemapperNames.size(); ++i) {
        if (render::kTonemapperNames[i] == env.tonemap_mode) {
            out.post.tonemapper = static_cast<render::Tonemapper>(i);
        }
    }
    out.post.exposure = env.tonemap_exposure;
    // As Godot's Environment::_update_tonemap: AgX has a white of its own.
    out.post.white =
        out.post.tonemapper == render::Tonemapper::Agx ? env.tonemap_agx_white : env.tonemap_white;
    out.post.agx_contrast = env.tonemap_agx_contrast;
    out.post.glow = env.glow_enabled;
    out.post.glow_intensity = env.glow_intensity;
    if (env.glow_levels.size() != render::kMaxGlowLevels) {
        out.problems.push_back({out.entity, "/components/WorldEnvironment/glow_levels",
                                "has 7 levels, not " + std::to_string(env.glow_levels.size()) +
                                    "; the missing ones are 0 and the rest are left out"});
    }
    out.post.glow_levels = {};
    float sum = 0.0F;
    for (std::size_t i = 0; i < std::min(env.glow_levels.size(), render::kMaxGlowLevels); ++i) {
        out.post.glow_levels.at(i) = env.glow_levels[i];
        sum += env.glow_levels[i];
    }
    // As Godot's Environment::_update_glow. Where every level is 0 Godot divides by 0; here they
    // stay 0.
    if (env.glow_normalized && sum != 0.0F) {
        for (float& level : out.post.glow_levels) {
            level /= sum;
        }
    }
    out.post.glow_strength = env.glow_strength;
    out.post.glow_mix = env.glow_mix;
    for (std::size_t i = 0; i < render::kGlowBlendModeNames.size(); ++i) {
        if (render::kGlowBlendModeNames[i] == env.glow_blend_mode) {
            out.post.glow_blend_mode = static_cast<render::GlowBlendMode>(i);
        }
    }
    out.post.glow_bloom = env.glow_bloom;
    out.post.glow_hdr_threshold = env.glow_hdr_threshold;
    out.post.glow_hdr_scale = env.glow_hdr_scale;
    out.post.glow_hdr_luminance_cap = env.glow_hdr_luminance_cap;
    out.post.adjustments = env.adjustment_enabled;
    out.post.brightness = env.adjustment_brightness;
    out.post.contrast = env.adjustment_contrast;
    out.post.saturation = env.adjustment_saturation;
    out.color_correction = env.adjustment_color_correction;
    return out;
}

namespace {

// Versions of auto exposure measures, unique across renderers, since a device keeps one
// measure.
std::atomic<std::uint64_t> exposure_versions{0};

} // namespace

entt::entity camera_attributes(const world::World& world, entt::entity camera) {
    const auto& ecs = world.ecs();
    if (camera != entt::null && world.valid(camera) &&
        ecs.all_of<world::CameraAttributesPractical>(camera)) {
        return camera;
    }
    const auto environments = made<world::WorldEnvironment>(world);
    if (!environments.empty() &&
        ecs.all_of<world::CameraAttributesPractical>(environments.front())) {
        return environments.front();
    }
    return entt::null;
}

Environment environment(const world::World& world, render::Color clear, entt::entity camera) {
    Environment out = world_environment(world, clear);
    out.attributes = camera_attributes(world, camera);
    if (out.attributes == entt::null) {
        return out;
    }
    const auto& attributes =
        world.ecs().get<const world::CameraAttributesPractical>(out.attributes);
    // Godot's exposure normalization without physical light units is exposure_multiplier alone.
    // As its bg_energy_multiplier, it brightens the background and the sky, and so the sky's
    // light, but not ambient_light_color or fog; a fogged background's sky, which the background's
    // energy multiplies twice, takes it twice.
    out.exposure = attributes.exposure_multiplier;
    out.background.r *= out.exposure;
    out.background.g *= out.exposure;
    out.background.b *= out.exposure;
    out.sky.energy *= out.exposure;
    if (out.sky.kind == render::SkyKind::Color) {
        out.sky.color = {out.background.r, out.background.g, out.background.b};
    }
    // Depth of field, as Godot's CameraAttributesPractical::_update_dof_blur.
    out.post.dof_far = attributes.dof_blur_far_enabled;
    out.post.dof_far_distance = attributes.dof_blur_far_distance;
    out.post.dof_far_transition = attributes.dof_blur_far_transition;
    out.post.dof_near = attributes.dof_blur_near_enabled;
    out.post.dof_near_distance = attributes.dof_blur_near_distance;
    out.post.dof_near_transition = attributes.dof_blur_near_transition;
    out.post.dof_amount = attributes.dof_blur_amount;
    if (!attributes.auto_exposure_enabled) {
        return out;
    }
    if (!std::isfinite(attributes.exposure_sensitivity) ||
        attributes.exposure_sensitivity <= 0.0F) {
        out.problems.push_back({out.attributes,
                                "/components/CameraAttributesPractical/exposure_sensitivity",
                                "is above 0 for auto exposure; drawing without it"});
        return out;
    }
    // As Godot's CameraAttributesPractical::_update_auto_exposure, which turns sensitivities into
    // luminances.
    const float luminance = 0.125F / attributes.exposure_sensitivity;
    out.post.auto_exposure = true;
    out.post.auto_exposure_scale = attributes.auto_exposure_scale;
    out.post.auto_exposure_min = attributes.auto_exposure_min_sensitivity * luminance;
    out.post.auto_exposure_max = attributes.auto_exposure_max_sensitivity * luminance;
    return out;
}

std::vector<render::Light3D> lights(const world::World& world, const world::Tree3D& tree,
                                    const std::function<world::Affine3D(entt::entity)>& global) {
    std::vector<Shone> found = shining(world, tree, global);
    // Directional lights first, so Compatibility's first render::kMaxLights3D keep them all.
    std::ranges::stable_partition(found, [](const Shone& shone) {
        return shone.light.type == render::LightType::Directional;
    });
    std::vector<render::Light3D> out;
    std::size_t directional = 0;
    for (const Shone& shone : found) {
        const render::Light3D& light = shone.light;
        if (light.type == render::LightType::Directional) {
            if (directional == render::kMaxDirectionalLights3D) {
                continue;
            }
            ++directional;
        } else if (out.size() - directional == render::kMaxClusteredLights3D) {
            break;
        }
        out.push_back(light);
    }
    return out;
}

std::vector<render::SkyLight>
sky_lights(const world::World& world, const world::Tree3D& tree,
           const std::function<world::Affine3D(entt::entity)>& global) {
    std::unordered_map<entt::entity, std::size_t> order;
    for (std::size_t i = 0; i < tree.order().size(); ++i) {
        order.emplace(tree.order()[i], i);
    }
    std::vector<std::pair<std::size_t, render::SkyLight>> found;
    for (const entt::entity entity : made<world::DirectionalLight3D>(world)) {
        const auto& light = world.ecs().get<const world::DirectionalLight3D>(entity);
        if (!tree.visible(entity) || light.sky_mode == "light_only") {
            continue;
        }
        const Vec3 forward = normalized(scaled(global(entity).z, -1.0F));
        render::SkyLight sun;
        sun.direction = {forward.x, forward.y, forward.z};
        sun.color = rgb(light.light_color);
        sun.energy = light.light_negative ? -light.light_energy : light.light_energy;
        sun.angular_distance = light.light_angular_distance;
        const auto at = order.find(entity);
        found.emplace_back(at != order.end() ? at->second : order.size(), sun);
    }
    std::ranges::stable_sort(found, {}, &std::pair<std::size_t, render::SkyLight>::first);
    std::vector<render::SkyLight> out;
    for (auto& [place, sun] : found) {
        if (out.size() == render::kMaxSkyLights) {
            break;
        }
        out.push_back(sun);
    }
    return out;
}

std::vector<Problem> check_meshes(const world::World& world) {
    std::vector<Problem> problems;
    // A Skeleton3D's bone names, checked against the model its entity draws, if it loads.
    const auto check_bones = [&](entt::entity entity, const Model* model) {
        const auto* skeleton = world.ecs().try_get<const world::Skeleton3D>(entity);
        if (skeleton == nullptr) {
            return;
        }
        for (SkeletonProblem& problem : check_skeleton(*skeleton, model)) {
            problems.push_back(
                {entity, "/components/Skeleton3D" + problem.path, std::move(problem.message)});
        }
    };
    // Its skeleton modifiers' bones, chains, targets and names.
    const auto check_ik = [&](entt::entity entity, const Model* model) {
        for (ModifierProblem& problem : check_modifiers(world, entity, model)) {
            problems.push_back(
                {problem.entity, std::move(problem.path), std::move(problem.message)});
        }
    };
    // The entities bone attachments follow, and the models they draw, loaded for their checks.
    std::set<entt::entity> followed;
    {
        const world::Tree3D tree(world);
        for (const entt::entity entity : world.ecs().view<const world::BoneAttachment3D>()) {
            followed.insert(attachment_skeleton(world, tree, entity));
        }
    }
    std::map<entt::entity, Model> loaded;
    for (const entt::entity entity : made<world::MeshInstance3D>(world)) {
        const std::string& path = world.ecs().get<const world::MeshInstance3D>(entity).mesh;
        if (path.empty()) {
            (void)drawable(world, entity, problems);
            check_bones(entity, nullptr);
            check_ik(entity, nullptr);
            continue;
        }
        if (std::string problem = mesh_path_problem(world, entity, path); !problem.empty()) {
            problems.push_back({entity, std::string(kMeshField), std::move(problem)});
            continue;
        }
        const world::FileReader& reader = world.file_reader();
        if (!reader) {
            continue;
        }
        auto bytes = reader(path);
        if (!bytes) {
            problems.push_back({entity, std::string(kMeshField),
                                "\"" + path + "\" could not be read: " + bytes.error().message});
            continue;
        }
        const std::vector<json::ValidationError> errors = check_model(bytes.value(), path, reader);
        for (const json::ValidationError& error : errors) {
            problems.push_back({entity, std::string(kMeshField),
                                "\"" + path + "\"" +
                                    (error.path.empty() ? "" : " at " + error.path) + ": " +
                                    error.message});
        }
        if (errors.empty() && (world.ecs().all_of<world::Skeleton3D>(entity) ||
                               has_modifiers(world, entity) || followed.contains(entity))) {
            if (auto model = load_model(bytes.value(), path, reader); model) {
                check_bones(entity, &model.value());
                check_ik(entity, &model.value());
                loaded.emplace(entity, std::move(model.value()));
            }
        }
    }
    for (ModifierProblem& problem : check_stray_modifiers(world)) {
        problems.push_back({problem.entity, std::move(problem.path), std::move(problem.message)});
    }
    for (ModifierProblem& problem : check_attachments(world, [&](entt::entity entity) {
             const auto found = loaded.find(entity);
             return found != loaded.end() ? &found->second : nullptr;
         })) {
        problems.push_back({problem.entity, std::move(problem.path), std::move(problem.message)});
    }
    for (const entt::entity entity : made<world::Skeleton3D>(world)) {
        if (!world.ecs().all_of<world::MeshInstance3D>(entity)) {
            check_bones(entity, nullptr);
        }
    }
    // A primitive mesh draws only on an entity with a MeshInstance3D, but two are always wrong.
    std::vector<entt::entity> others;
    if (const auto* entities = world.ecs().storage<entt::entity>(); entities != nullptr) {
        for (const entt::entity entity : *entities) {
            if (world.ecs().valid(entity) && !world.ecs().all_of<world::MeshInstance3D>(entity)) {
                others.push_back(entity);
            }
        }
    }
    std::ranges::sort(others, {}, [](entt::entity entity) { return entt::to_entity(entity); });
    for (const entt::entity entity : others) {
        if (auto primitive = primitive_of(world, entity); !primitive) {
            problems.push_back({entity, "/components", primitive.error().message});
        }
    }
    for (Problem& problem : check_csg(world)) {
        problems.push_back(std::move(problem));
    }
    for (world::PathProblem& problem : world::check_paths(world)) {
        problems.push_back({problem.entity, std::move(problem.path), std::move(problem.message)});
    }
    for (Problem& problem : check_gridmaps(world)) {
        problems.push_back(std::move(problem));
    }
    for (Problem& problem : check_terrains(world)) {
        problems.push_back(std::move(problem));
    }
    for (Problem& problem : check_lightmaps(world)) {
        problems.push_back(std::move(problem));
    }
    for (Problem& problem : check_voxel_gis(world)) {
        problems.push_back(std::move(problem));
    }
    return problems;
}

std::vector<json::ValidationError> check_meshes(const world::ComponentRegistry& components,
                                                const json::Value& scene,
                                                const world::FileReader& reader) {
    std::vector<json::ValidationError> errors;
    world::World world(components);
    world.set_file_reader(reader);
    if (!world::load_scene(world, scene)) {
        return errors;
    }
    for (Problem& problem : check_meshes(world)) {
        errors.push_back(
            {"/entities/" + json::pointer_escape(world.id_of(problem.entity)) + problem.path,
             std::move(problem.message)});
    }
    return errors;
}

render::Material3D material(const world::StandardMaterial3D& standard,
                            const TextureSource& textures, std::vector<Problem>& problems,
                            entt::entity entity) {
    render::Material3D out;
    const bool nearest = standard.texture_filter.starts_with("nearest");
    const render::Filter filter = nearest ? render::Filter::Nearest : render::Filter::Linear;
    const auto texture = [&](const std::string& path,
                             const char* field) -> render::MaterialTexture {
        if (path.empty()) {
            return {};
        }
        auto made = textures(path, filter);
        if (!made) {
            problems.push_back({entity, std::string("/components/StandardMaterial3D/") + field,
                                made.error().message});
            return {};
        }
        return {.texture = made.value().id, .repeat = standard.texture_repeat};
    };
    out.albedo = {standard.albedo_color.x, standard.albedo_color.y, standard.albedo_color.z,
                  standard.albedo_color.w};
    out.albedo_texture = texture(standard.albedo_texture, "albedo_texture");
    out.vertex_color_use_as_albedo = standard.vertex_color_use_as_albedo;
    out.metallic = standard.metallic;
    out.metallic_texture = texture(standard.metallic_texture, "metallic_texture");
    out.metallic_channel = channel_of(standard.metallic_texture_channel);
    out.roughness = standard.roughness;
    out.roughness_texture = texture(standard.roughness_texture, "roughness_texture");
    out.roughness_channel = channel_of(standard.roughness_texture_channel);
    out.specular = standard.metallic_specular;
    if (standard.emission_enabled) {
        out.emission = rgb(standard.emission);
        out.emission_energy = standard.emission_energy_multiplier;
        out.emission_texture = texture(standard.emission_texture, "emission_texture");
    }
    if (standard.normal_enabled) {
        out.normal_texture = texture(standard.normal_texture, "normal_texture");
        out.normal_scale = standard.normal_scale;
    }
    out.unshaded = standard.shading_mode == "unshaded";
    if (standard.transparency == "alpha") {
        out.transparency = render::Transparency::Alpha;
    } else if (standard.transparency == "alpha_scissor") {
        out.transparency = render::Transparency::AlphaScissor;
    }
    out.alpha_scissor_threshold = standard.alpha_scissor_threshold;
    out.blend = render::parse_blend_mode(standard.blend_mode).value_or(render::BlendMode::Mix);
    if (standard.cull_mode == "front") {
        out.cull = render::CullMode::Front;
    } else if (standard.cull_mode == "disabled") {
        out.cull = render::CullMode::Disabled;
    }
    out.uv1_scale = {standard.uv1_scale.x, standard.uv1_scale.y};
    out.uv1_offset = {standard.uv1_offset.x, standard.uv1_offset.y};
    out.disable_fog = standard.disable_fog;
    return out;
}

Renderer::Renderer(render::RenderDevice& device, TextureSource textures, ModelSource models,
                   LightmapSource lightmaps, VoxelGISource voxel_gis)
    : device_(&device), textures_(std::move(textures)), models_(std::move(models)),
      lightmaps_(std::move(lightmaps)), voxel_gi_source_(std::move(voxel_gis)) {}

Renderer::~Renderer() {
    for (const auto& [key, shared] : shared_) {
        device_->destroy_mesh(shared.id);
    }
    for (const auto& [key, skinned] : skinned_) {
        device_->destroy_mesh(skinned.id);
    }
    for (const auto& [key, texture] : embedded_) {
        device_->destroy_texture(texture.id);
    }
    destroy_lightmaps(false);
    destroy_voxel_gis();
}

void Renderer::destroy_lightmaps(bool unused_only) {
    for (auto it = kept_lightmaps_.begin(); it != kept_lightmaps_.end();) {
        if (unused_only && it->second.used == frame_) {
            ++it;
            continue;
        }
        if (it->second.texture != render::TextureId::None) {
            device_->destroy_texture(it->second.texture);
        }
        it = kept_lightmaps_.erase(it);
    }
    std::erase_if(mesh_hashes_,
                  [&](const auto& entry) { return !unused_only || entry.second.used != frame_; });
}

void Renderer::find_lightmaps(const world::World& world, float exposure,
                              const std::function<world::Affine3D(entt::entity)>& global) {
    lit_.clear();
    probe_sources_.clear();
    for (const entt::entity entity : made<world::LightmapGI>(world)) {
        const auto& gi = world.ecs().get<const world::LightmapGI>(entity);
        if (gi.light_data.empty()) {
            continue;
        }
        const std::string field = "/components/LightmapGI/light_data";
        if (!lightmaps_) {
            problems_.push_back({entity, field, "lightmaps can't load here"});
            continue;
        }
        auto loaded = lightmaps_(gi.light_data);
        if (!loaded) {
            problems_.push_back({entity, field, loaded.error().message});
            continue;
        }
        KeptLightmap& kept = kept_lightmaps_[gi.light_data];
        if (kept.source.data != loaded.value().data ||
            kept.source.texels != loaded.value().texels) {
            if (kept.texture != render::TextureId::None) {
                device_->destroy_texture(kept.texture);
            }
            kept = KeptLightmap{.source = loaded.value(),
                                .texture = render::TextureId::None,
                                .problem = {},
                                .version = ++lightmap_versions_,
                                .used = 0};
            const LightmapData& data = *kept.source.data;
            const render::HdrImage& texels = *kept.source.texels;
            if (texels.width != data.width || texels.height != data.height * data.slices) {
                kept.problem = "its texture \"" + data.texture + "\" is " +
                               std::to_string(texels.width) + "x" + std::to_string(texels.height) +
                               ", not the " + std::to_string(data.width) + "x" +
                               std::to_string(data.height * data.slices) +
                               " its slices take: bake it again";
            } else {
                // Half floats, as Godot keeps lightmaps.
                std::vector<std::uint8_t> bytes(std::size_t{texels.width} * texels.height * 8);
                for (std::size_t i = 0; i < std::size_t{texels.width} * texels.height; ++i) {
                    const std::array<std::uint16_t, 4> half{
                        render::float_to_half(texels.rgb[i * 3]),
                        render::float_to_half(texels.rgb[(i * 3) + 1]),
                        render::float_to_half(texels.rgb[(i * 3) + 2]),
                        render::float_to_half(1.0F)};
                    for (std::size_t c = 0; c < 4; ++c) {
                        bytes[(i * 8) + (c * 2)] = static_cast<std::uint8_t>(half.at(c) & 0xFFU);
                        bytes[(i * 8) + (c * 2) + 1] = static_cast<std::uint8_t>(half.at(c) >> 8U);
                    }
                }
                auto made = device_->create_texture({.width = texels.width,
                                                     .height = texels.height,
                                                     .filter = render::Filter::Linear,
                                                     .render_target = false,
                                                     .format = render::TextureFormat::Rgba16F},
                                                    bytes);
                if (!made) {
                    kept.problem = "its texture \"" + data.texture + "\": " + made.error().message;
                } else {
                    kept.texture = made.value();
                }
            }
        }
        kept.used = frame_;
        if (!kept.problem.empty()) {
            problems_.push_back({entity, field, kept.problem});
            continue;
        }
        const LightmapData& data = *kept.source.data;
        if (!data.probes.points.empty()) {
            probe_sources_.push_back({.data = kept.source.data,
                                      .transform = global(entity),
                                      .exposure = exposure / data.baked_exposure});
        }
        const auto slices = static_cast<float>(std::max(data.slices, 1U));
        for (const LightmapUser& user : data.users) {
            const entt::entity lit = world.find(user.entity);
            if (lit == entt::null) {
                problems_.push_back(
                    {entity, field,
                     "lights \"" + user.entity + "\", which isn't in the world: bake it again"});
                continue;
            }
            const auto unwrap = data.meshes.find(user.mesh);
            if (unwrap == data.meshes.end()) {
                continue;
            }
            // The slices lie one under another in the texture.
            Lit place{
                .data = kept.source.data,
                .unwrap = &unwrap->second,
                .mesh = user.mesh,
                .key = gi.light_data + "@" + std::to_string(kept.version),
                .shading = {.texture = kept.texture,
                            .uv_rect = {user.uv_rect[0],
                                        (static_cast<float>(user.slice) + user.uv_rect[1]) / slices,
                                        user.uv_rect[2], user.uv_rect[3] / slices},
                            .exposure = exposure / data.baked_exposure,
                            .bicubic = true},
                .reported = false};
            lit_.emplace(std::pair{lit, user.part}, std::move(place));
        }
    }
}

render::LightmapCapture Renderer::capture(entt::entity entity, const Vec3& center) {
    render::LightmapCapture out;
    if (probe_sources_.empty()) {
        return out;
    }
    std::vector<LightmapCaptureSource> sources;
    sources.reserve(probe_sources_.size());
    for (const ProbeSource& source : probe_sources_) {
        sources.push_back({.probes = &source.data->probes,
                           .transform = source.transform,
                           .exposure = source.exposure});
    }
    const std::optional<LightmapSh> taken =
        lightmap_capture(sources, {center.x, center.y, center.z});
    if (!taken) {
        captured_.erase(entity);
        return out;
    }
    const LightmapSh& target = *taken;
    // Godot eases a mesh's light by its lightmap_probe_update_speed, 15, times the frame's time;
    // the first frame, and after game time goes back, it is set at once.
    auto [found, made] = captured_.try_emplace(entity);
    Captured& captured = found->second;
    if (made || captured.used + 1 != frame_ || capture_time_ < captured.time) {
        captured.sh = target;
    } else {
        const auto weight =
            static_cast<float>(std::min(1.0, kProbeUpdateSpeed * (capture_time_ - captured.time)));
        for (std::size_t j = 0; j < 9; ++j) {
            for (std::size_t c = 0; c < 3; ++c) {
                float& value = captured.sh.at(j).at(c);
                value += (target.at(j).at(c) - value) * weight;
            }
        }
    }
    captured.time = capture_time_;
    captured.used = frame_;
    out.enabled = true;
    out.sh = captured.sh;
    return out;
}

const render::LightmapShading* Renderer::lit_surface(entt::entity entity, std::uint32_t part,
                                                     const LitSource& source, std::uint32_t first,
                                                     std::uint32_t count, render::MeshId& mesh) {
    const auto found = lit_.find({entity, part});
    if (found == lit_.end()) {
        return nullptr;
    }
    Lit& lit = found->second;
    std::vector<render::Vertex3D> vertices;
    std::vector<std::uint32_t> indices;
    bool made = false;
    MeshHash& hash = mesh_hashes_[source.key];
    if (hash.hash.empty()) {
        source.make(vertices, indices);
        made = true;
        hash.hash = lightmap_mesh_hash(vertices, indices);
    }
    hash.used = frame_;
    if (hash.hash != lit.mesh) {
        if (!lit.reported) {
            lit.reported = true;
            problems_.push_back({entity, "/components/MeshInstance3D",
                                 "has changed since its LightmapGI baked it, so it is drawn "
                                 "without its lightmap: bake it again"});
        }
        return nullptr;
    }
    const std::string key = "lightmap:" + lit.key + ":" + lit.mesh + ":" + std::to_string(first) +
                            "+" + std::to_string(count);
    Shared& shared = shared_[key];
    if (shared.id == render::MeshId::None) {
        if (!made) {
            source.make(vertices, indices);
        }
        // The unwrap's vertices of these triangles, with their lightmap UVs.
        const LightmapUnwrap& unwrap = *lit.unwrap;
        if (count == kAllIndices) {
            count = static_cast<std::uint32_t>(unwrap.indices.size()) -
                    std::min(first, static_cast<std::uint32_t>(unwrap.indices.size()));
        }
        std::vector<render::Vertex3D> lit_vertices;
        std::vector<std::uint32_t> lit_indices;
        std::unordered_map<std::uint32_t, std::uint32_t> remap;
        bool fits = std::size_t{first} + count <= unwrap.indices.size();
        for (std::uint32_t i = first; fits && i < first + count; ++i) {
            const std::uint32_t v = unwrap.indices[i];
            const auto [at, added] =
                remap.try_emplace(v, static_cast<std::uint32_t>(lit_vertices.size()));
            if (added) {
                if (unwrap.xref[v] >= vertices.size()) {
                    fits = false;
                    break;
                }
                render::Vertex3D vertex = vertices[unwrap.xref[v]];
                vertex.uv2 = unwrap.uv2[v];
                lit_vertices.push_back(vertex);
            }
            lit_indices.push_back(at->second);
        }
        auto created = fits ? device_->create_mesh(lit_vertices, lit_indices)
                            : Result<render::MeshId>(make_error(
                                  ErrorCode::InvalidArgument,
                                  "its lightmap's unwrap doesn't fit its mesh: bake it again"));
        if (!created) {
            shared_.erase(key);
            problems_.push_back({entity, "/components/MeshInstance3D", created.error().message});
            return nullptr;
        }
        shared.id = created.value();
    }
    shared.used = frame_;
    mesh = shared.id;
    return &lit.shading;
}

void Renderer::before_tick(const world::World& world) {
    before_.clear();
    const auto view = world.ecs().view<const world::Transform3D>();
    for (const entt::entity entity : view) {
        const auto& transform = view.get<const world::Transform3D>(entity);
        before_.emplace(entity, Local{.position = transform.position,
                                      .rotation = world::quaternion(transform.rotation),
                                      .scale = transform.scale});
    }
    before_poses_.clear();
    for (const entt::entity entity : world.ecs().view<const world::MeshInstance3D>()) {
        if (world.ecs().get<const world::MeshInstance3D>(entity).mesh.empty()) {
            continue;
        }
        PoseKey key{.clips = clips(world, entity), .skeleton = std::nullopt};
        if (const auto* skeleton = world.ecs().try_get<const world::Skeleton3D>(entity);
            skeleton != nullptr) {
            key.skeleton = *skeleton;
        }
        if (!key.clips.empty() || key.skeleton) {
            before_poses_.emplace(entity, std::move(key));
        }
    }
}

Result<render::MeshId> Renderer::mesh_for(const world::World& world, entt::entity entity,
                                          const Primitive& primitive) {
    auto cached = cached_.find(entity);
    if (cached == cached_.end() || !(cached->second.primitive == primitive)) {
        // Meshes of equal settings share one, keyed by the component's name and fields.
        const std::string key =
            std::string(kPrimitiveNames[primitive.index()]) +
            std::visit(
                [&](const auto& mesh) {
                    return json::write(world::to_json(world, mesh), json::Style::Compact);
                },
                primitive);
        cached = cached_.insert_or_assign(entity, Cached{.primitive = primitive, .key = key}).first;
    }
    Shared& shared = shared_[cached->second.key];
    if (shared.id == render::MeshId::None) {
        auto data = make_mesh(primitive);
        if (!data) {
            shared_.erase(cached->second.key);
            return data.error();
        }
        auto made = device_->create_mesh(data.value().vertices, data.value().indices);
        if (!made) {
            shared_.erase(cached->second.key);
            return made.error();
        }
        shared.id = made.value();
        shared.box = bounds_of(data.value().vertices);
        shared.center = scaled(shared.box.min + shared.box.max, 0.5F);
    }
    shared.used = frame_;
    return shared.id;
}

Result<render::MeshId> Renderer::kept_mesh(const std::string& key,
                                           const std::vector<render::Vertex3D>& vertices,
                                           const std::vector<std::uint32_t>& indices) {
    Shared& shared = shared_[key];
    if (shared.id == render::MeshId::None) {
        auto made = device_->create_mesh(vertices, indices);
        if (!made) {
            shared_.erase(key);
            return made.error();
        }
        shared.id = made.value();
        shared.box = bounds_of(vertices);
    }
    shared.used = frame_;
    return shared.id;
}

Result<render::MeshId> Renderer::skinned_mesh(const std::string& key, const ModelSurface& surface,
                                              const std::vector<world::Affine3D>& joints) {
    Skinned& skinned = skinned_[key];
    skinned.used = frame_;
    if (skinned.id != render::MeshId::None && skinned.joints == joints) {
        return skinned.id;
    }
    skin_vertices(surface, joints, skinned_vertices_);
    if (skinned.id == render::MeshId::None) {
        auto made = device_->create_mesh(skinned_vertices_, surface.indices);
        if (!made) {
            skinned_.erase(key);
            return made.error();
        }
        skinned.id = made.value();
    } else if (auto updated = device_->update_mesh(skinned.id, skinned_vertices_); !updated) {
        return updated.error();
    }
    skinned.joints = joints;
    skinned.box = bounds_of(skinned_vertices_);
    return skinned.id;
}

render::MaterialTexture Renderer::model_texture(const std::string& path, const SourceModel& source,
                                                int image,
                                                const world::StandardMaterial3D& standard,
                                                entt::entity entity, const std::string& field) {
    if (image < 0 || static_cast<std::size_t>(image) >= source.model->images.size()) {
        return {};
    }
    const bool nearest = standard.texture_filter.starts_with("nearest");
    const render::Filter filter = nearest ? render::Filter::Nearest : render::Filter::Linear;
    const ModelImage& found = source.model->images[static_cast<std::size_t>(image)];
    if (!found.path.empty()) {
        auto made = textures_(found.path, filter);
        if (!made) {
            problems_.push_back(
                {entity, field, "\"" + path + "\"'s texture: " + made.error().message});
            return {};
        }
        return {.texture = made.value().id, .repeat = standard.texture_repeat};
    }
    const std::string key = path + "@" + std::to_string(source.version) + "#" +
                            std::to_string(image) + (nearest ? "n" : "l");
    Embedded& embedded = embedded_[key];
    if (embedded.id == render::TextureId::None) {
        auto made = device_->create_texture({.width = found.image.width,
                                             .height = found.image.height,
                                             .filter = filter,
                                             .render_target = false,
                                             .format = render::TextureFormat::Rgba8},
                                            found.image.pixels);
        if (!made) {
            embedded_.erase(key);
            problems_.push_back({entity, field,
                                 "\"" + path + "\"'s image " + std::to_string(image) + ": " +
                                     made.error().message});
            return {};
        }
        embedded.id = made.value();
    }
    embedded.used = frame_;
    return {.texture = embedded.id, .repeat = standard.texture_repeat};
}

void Renderer::add_model(const world::World& world, entt::entity entity,
                         const world::MeshInstance3D& instance, const world::Affine3D& place,
                         const std::function<world::Affine3D(entt::entity)>& global,
                         render::ShadowCasting casting, float interpolation) {
    if (std::string problem = mesh_path_problem(world, entity, instance.mesh); !problem.empty()) {
        problems_.push_back({entity, std::string(kMeshField), std::move(problem)});
        return;
    }
    if (!models_) {
        problems_.push_back({entity, std::string(kMeshField), "models can't load here"});
        return;
    }
    auto source = models_(instance.mesh);
    if (!source) {
        problems_.push_back({entity, std::string(kMeshField), source.error().message});
        return;
    }
    const Model& model = *source.value().model;
    // A StandardMaterial3D on the entity takes the place of every surface's, as Godot's
    // material_override.
    std::optional<render::Material3D> override;
    if (const auto* standard = world.ecs().try_get<world::StandardMaterial3D>(entity);
        standard != nullptr) {
        override = material(*standard, textures_, problems_, entity);
    }
    std::vector<std::optional<render::Material3D>> materials(model.materials.size());
    const std::string prefix =
        "model:" + instance.mesh + "@" + std::to_string(source.value().version) + "#";

    // A model with a skin, an animation playing, a Skeleton3D or skeleton modifiers is posed: its
    // parts go where its nodes are posed, and its skins move their surfaces' vertices.
    PoseKey now{.clips = clips(world, entity), .skeleton = std::nullopt};
    if (const auto* skeleton = world.ecs().try_get<const world::Skeleton3D>(entity);
        skeleton != nullptr) {
        now.skeleton = *skeleton;
    }
    std::vector<world::Affine3D> globals;
    const bool modified = has_modifiers(world, entity);
    if (!model.nodes.empty() &&
        (!model.skins.empty() || !now.clips.empty() || now.skeleton || modified)) {
        const ModelLoader load =
            [this](const std::string& path) -> Result<std::shared_ptr<const Model>> {
            auto loaded = models_(path);
            if (!loaded) {
                return loaded.error();
            }
            return loaded.value().model;
        };
        const auto skeleton_of = [](const std::optional<world::Skeleton3D>& skeleton) {
            return skeleton.has_value() ? &skeleton.value() : nullptr;
        };
        std::vector<NodePose> pose = pose_of(model, now.clips, skeleton_of(now.skeleton), load);
        // Part of the way from the pose before the last tick, as Transform3Ds are.
        if (const auto was = before_poses_.find(entity);
            interpolation < 1.0F && was != before_poses_.end() && !(was->second == now)) {
            const std::vector<NodePose> before =
                pose_of(model, was->second.clips, skeleton_of(was->second.skeleton), load);
            pose = blend_poses(before, pose, interpolation);
        }
        // Modifiers turn bones after that, toward targets where they are drawn.
        if (modified) {
            apply_modifiers(world, entity, model, place, global, pose);
        }
        globals = node_globals(model, pose);
    }
    const std::string skin_prefix =
        "skin:" + std::to_string(entt::to_integral(entity)) + ":" + prefix;
    // A moving mesh takes the light probes' light at its bounds' middle, as Godot's.
    render::LightmapCapture captured;
    if (instance.gi_mode == "dynamic") {
        auto center = model_centers_.find(prefix);
        if (center == model_centers_.end()) {
            const Bounds box = bounds(model);
            center =
                model_centers_.emplace(prefix, std::pair{scaled(box.min + box.max, 0.5F), 0}).first;
        }
        center->second.second = frame_;
        captured = capture(entity, place.apply(center->second.first));
    }
    for (std::size_t p = 0; p < model.parts.size(); ++p) {
        const ModelPart& part = model.parts[p];
        const bool posed = !globals.empty() && part.node_index >= 0 &&
                           static_cast<std::size_t>(part.node_index) < globals.size();
        const std::array<float, 16> transform =
            (place * (posed ? globals[static_cast<std::size_t>(part.node_index)] : part.transform))
                .matrix();
        std::vector<world::Affine3D> joints;
        if (!globals.empty() && part.skin >= 0) {
            joints = joint_transforms(model.skins[static_cast<std::size_t>(part.skin)], globals);
        }
        const ModelMesh& mesh = model.meshes[part.mesh];
        // A part's surfaces were unwrapped together for its lightmap, one after another.
        const LitSource lit_source{.key = prefix + "part" + std::to_string(part.mesh),
                                   .make = [&mesh](std::vector<render::Vertex3D>& vertices,
                                                   std::vector<std::uint32_t>& indices) {
                                       for (const ModelSurface& surface : mesh.surfaces) {
                                           const auto base =
                                               static_cast<std::uint32_t>(vertices.size());
                                           vertices.insert(vertices.end(), surface.vertices.begin(),
                                                           surface.vertices.end());
                                           for (const std::uint32_t index : surface.indices) {
                                               indices.push_back(base + index);
                                           }
                                       }
                                   }};
        std::uint32_t first_index = 0;
        for (std::size_t i = 0; i < mesh.surfaces.size(); ++i) {
            const ModelSurface& surface = mesh.surfaces[i];
            const std::uint32_t first = first_index;
            first_index += static_cast<std::uint32_t>(surface.indices.size());
            // A skin places its surfaces by its joints, leaving out its part's node.
            const bool skinned = !joints.empty() && !surface.weights.empty();
            auto made =
                skinned ? skinned_mesh(skin_prefix + std::to_string(p) + "/" + std::to_string(i),
                                       surface, joints)
                        : kept_mesh(prefix + std::to_string(part.mesh) + "/" + std::to_string(i),
                                    surface.vertices, surface.indices);
            if (!made) {
                problems_.push_back({entity, std::string(kMeshField), made.error().message});
                continue;
            }
            Draw draw;
            draw.mesh = made.value();
            draw.transform = skinned ? place.matrix() : transform;
            if (override) {
                draw.material = *override;
            } else if (surface.material >= 0) {
                const auto index = static_cast<std::size_t>(surface.material);
                std::optional<render::Material3D>& made_material = materials[index];
                if (!made_material) {
                    const ModelMaterial& from = model.materials[index];
                    render::Material3D out = material(from.standard, textures_, problems_, entity);
                    const auto texture = [&](int image) {
                        return model_texture(instance.mesh, source.value(), image, from.standard,
                                             entity, std::string(kMeshField));
                    };
                    out.albedo_texture = texture(from.albedo_image);
                    out.metallic_texture = texture(from.metallic_image);
                    out.roughness_texture = from.roughness_image == from.metallic_image
                                                ? out.metallic_texture
                                                : texture(from.roughness_image);
                    if (from.standard.normal_enabled) {
                        out.normal_texture = texture(from.normal_image);
                    }
                    if (from.standard.emission_enabled) {
                        out.emission_texture = texture(from.emission_image);
                    }
                    made_material = out;
                }
                draw.material = *made_material;
            }
            render::MeshId lit = render::MeshId::None;
            if (instance.gi_mode != "static" || skinned || part.skin >= 0) {
                // Moving meshes aren't baked; dynamic ones take the light probes'.
                draw.material.capture = captured;
            } else if (const render::LightmapShading* lightmap =
                           lit_surface(entity, static_cast<std::uint32_t>(p), lit_source, first,
                                       static_cast<std::uint32_t>(surface.indices.size()), lit)) {
                draw.mesh = lit;
                draw.material.lightmap = *lightmap;
            }
            draw.layers = instance.layers;
            draw.shadows = casting;
            draws_.push_back(draw);
        }
    }
}

render::Material3D Renderer::surface_material(const SurfaceMaterial& from, entt::entity entity,
                                              const std::string& model_field) {
    if (from.standard) {
        return material(*from.standard, textures_, problems_, entity);
    }
    if (from.model == nullptr || from.model_material < 0 ||
        static_cast<std::size_t>(from.model_material) >= from.model->materials.size()) {
        return {};
    }
    // A model's own material, its images found as a MeshInstance3D's are.
    const ModelMaterial& model_material =
        from.model->materials[static_cast<std::size_t>(from.model_material)];
    render::Material3D out = material(model_material.standard, textures_, problems_, from.entity);
    // Its images are kept by the model's version where models load here.
    SourceModel source{.model = from.model, .version = 0};
    if (models_) {
        if (auto loaded = models_(from.model_path); loaded && loaded.value().model == from.model) {
            source.version = loaded.value().version;
        }
    }
    const auto texture = [&](int image) {
        return model_texture(from.model_path, source, image, model_material.standard, from.entity,
                             model_field);
    };
    out.albedo_texture = texture(model_material.albedo_image);
    out.metallic_texture = texture(model_material.metallic_image);
    out.roughness_texture = model_material.roughness_image == model_material.metallic_image
                                ? out.metallic_texture
                                : texture(model_material.roughness_image);
    if (model_material.standard.normal_enabled) {
        out.normal_texture = texture(model_material.normal_image);
    }
    if (model_material.standard.emission_enabled) {
        out.emission_texture = texture(model_material.emission_image);
    }
    return out;
}

void Renderer::add_csg(const world::World& world, const world::Tree3D& tree, entt::entity root,
                       const world::Affine3D& place, std::uint32_t cull_mask) {
    const std::optional<CsgShape> shape = csg_shape_of(world, root);
    if (!shape) {
        return;
    }
    const CsgSettings settings = csg_settings(*shape);
    render::ShadowCasting casting = casting_of(settings.cast_shadow);
    if ((settings.layers & cull_mask) == 0) {
        if (casting == render::ShadowCasting::Off) {
            return;
        }
        casting = render::ShadowCasting::ShadowsOnly;
    }
    const std::shared_ptr<const CsgMesh> mesh = csg_mesh(world, tree, root);
    problems_.insert(problems_.end(), mesh->problems.begin(), mesh->problems.end());
    const std::array<float, 16> transform = place.matrix();
    const std::string prefix = "csg:" + std::to_string(mesh->version) + "/";
    for (std::size_t i = 0; i < mesh->surfaces.size(); ++i) {
        const MeshSurface& surface = mesh->surfaces[i];
        auto made = kept_mesh(prefix + std::to_string(i), surface.vertices, surface.indices);
        if (!made) {
            problems_.push_back({root, "/components", made.error().message});
            continue;
        }
        Draw draw;
        draw.mesh = made.value();
        draw.transform = transform;
        draw.material = surface_material(surface.material, root, "/components/CSGMesh3D/mesh");
        draw.layers = settings.layers;
        draw.shadows = casting;
        draws_.push_back(draw);
    }
}

void Renderer::add_gridmap(const world::World& world, entt::entity entity,
                           const world::Affine3D& place, std::uint32_t cull_mask) {
    // An octant's MultiMesh instances are on the first render layer, as Godot makes them.
    constexpr std::uint32_t kLayers = 1;
    const std::shared_ptr<const GridMesh> mesh = grid_mesh(world, entity);
    problems_.insert(problems_.end(), mesh->problems.begin(), mesh->problems.end());
    const std::array<float, 16> transform = place.matrix();
    for (const std::shared_ptr<const GridOctant>& octant : mesh->octants) {
        const std::string prefix = "grid:" + std::to_string(octant->version) + "/";
        for (std::size_t i = 0; i < octant->surfaces.size(); ++i) {
            const GridSurface& surface = octant->surfaces[i];
            render::ShadowCasting casting = casting_of(surface.cast_shadow);
            if ((kLayers & cull_mask) == 0) {
                if (casting == render::ShadowCasting::Off) {
                    continue;
                }
                casting = render::ShadowCasting::ShadowsOnly;
            }
            auto made =
                kept_mesh(prefix + std::to_string(i), surface.mesh.vertices, surface.mesh.indices);
            if (!made) {
                problems_.push_back({entity, "/components/GridMap", made.error().message});
                continue;
            }
            Draw draw;
            draw.mesh = made.value();
            draw.transform = transform;
            draw.material =
                surface_material(surface.mesh.material, entity, "/components/GridMap/mesh_library");
            draw.layers = kLayers;
            draw.shadows = casting;
            draws_.push_back(draw);
        }
    }
}

namespace {

// Where a camera's forward ray first meets a terrain, in its cell space, as HTerrain's
// cell_raycast finds it for orthogonal cameras: stepping half a cell at a time up to far * 1.2.
std::optional<Vec3> terrain_hit(const TerrainData& data, const world::Affine3D& to_cells,
                                const world::Affine3D& camera, float far, float step) {
    constexpr int kMaxSteps = 1 << 14;
    const Vec3 forward = normalized(scaled(camera.z, -1.0F));
    const float limit = far * 1.2F;
    step = std::max(step, limit / static_cast<float>(kMaxSteps));
    if (!(step > 0.0F)) {
        return std::nullopt;
    }
    const auto edge = static_cast<float>(data.resolution - 1);
    const auto steps = static_cast<int>(limit / step);
    for (int i = 0; i <= steps; ++i) {
        const float t = static_cast<float>(i) * step;
        const Vec3 cell = to_cells.apply(camera.origin + scaled(forward, t));
        if (cell.x < 0.0F || cell.z < 0.0F || cell.x > edge || cell.z > edge) {
            continue;
        }
        const float h = data.interpolated_height(cell.x, cell.z);
        if (cell.y <= h) {
            return Vec3{cell.x, h, cell.z};
        }
    }
    return std::nullopt;
}

} // namespace

render::MaterialTexture Renderer::kept_texture(const std::string& key, const render::Image& image) {
    Embedded& embedded = embedded_[key];
    if (embedded.id == render::TextureId::None) {
        auto made = device_->create_texture({.width = image.width,
                                             .height = image.height,
                                             .filter = render::Filter::Linear,
                                             .render_target = false,
                                             .format = render::TextureFormat::Rgba8},
                                            image.pixels);
        if (!made) {
            embedded_.erase(key);
            return {};
        }
        embedded.id = made.value();
    }
    embedded.used = frame_;
    return {.texture = embedded.id, .repeat = false};
}

void Renderer::add_terrain(const world::World& world, entt::entity entity,
                           const world::Affine3D& place, const world::Affine3D& camera,
                           const world::Camera3D& lens) {
    const auto& terrain = world.ecs().get<const world::HTerrain>(entity);
    const std::shared_ptr<const TerrainData> data = terrain_data(world, entity);
    problems_.insert(problems_.end(), data->problems.begin(), data->problems.end());
    if (data->empty()) {
        return;
    }
    const int chunk_size = terrain.chunk_size;
    if (chunk_size != 16 && chunk_size != 32 && chunk_size != 64) {
        problems_.push_back({entity, "/components/HTerrain/chunk_size",
                             "is 16, 32 or 64, not " + std::to_string(chunk_size)});
        return;
    }
    render::ShadowCasting casting = casting_of(terrain.cast_shadow);
    if ((terrain.render_layers & lens.cull_mask) == 0) {
        if (casting == render::ShadowCasting::Off) {
            return;
        }
        casting = render::ShadowCasting::ShadowsOnly;
    }
    const world::Affine3D cells = terrain_transform(terrain, data->resolution, place);
    const world::Affine3D to_cells = cells.inverse();
    // The viewer in cell space, 1 unit a cell; orthogonal cameras stand where they look at the
    // terrain, as their distance says nothing of what they show.
    Vec3 viewer = to_cells.apply(camera.origin);
    if (lens.projection == "orthogonal") {
        const float step =
            0.5F * std::min(std::abs(terrain.map_scale.x), std::abs(terrain.map_scale.z));
        if (const std::optional<Vec3> hit = terrain_hit(*data, to_cells, camera, lens.far, step);
            hit) {
            viewer = *hit;
        }
    }

    render::Material3D material;
    material.specular = terrain.specular;
    render::TerrainShading& shading = material.terrain;
    shading.enabled = true;
    shading.resolution = static_cast<std::uint32_t>(data->resolution);
    shading.depth_blending = terrain.depth_blending;
    const auto map = [&](const std::string& path, bool present, const char* field,
                         const char* fallback, std::array<std::uint8_t, 4> rgba) {
        if (present && textures_) {
            auto made = textures_(path, render::Filter::Linear);
            if (made) {
                return render::MaterialTexture{.texture = made.value().id, .repeat = false};
            }
            problems_.push_back(
                {entity, std::string("/components/HTerrain/") + field, made.error().message});
        }
        render::Image pixel;
        pixel.width = 1;
        pixel.height = 1;
        pixel.pixels = {rgba[0], rgba[1], rgba[2], rgba[3]};
        return kept_texture(fallback, pixel);
    };
    shading.colormap = map(terrain.colormap, data->colormap != nullptr, "colormap", "terrain:white",
                           {255, 255, 255, 255});
    shading.splatmap = map(terrain.splatmap, data->splatmap != nullptr, "splatmap", "terrain:first",
                           {255, 0, 0, 0});
    const std::string normals_key = "terrain-normals:" + std::to_string(data->version) + "@" +
                                    std::to_string(terrain.map_scale.x) + "," +
                                    std::to_string(terrain.map_scale.y) + "," +
                                    std::to_string(terrain.map_scale.z);
    if (const auto found = embedded_.find(normals_key); found != embedded_.end()) {
        found->second.used = frame_;
        shading.normalmap = {.texture = found->second.id, .repeat = false};
    } else {
        shading.normalmap = kept_texture(normals_key, terrain_normals(*data, terrain.map_scale));
    }
    for (std::size_t i = 0; i < terrain.textures.size() && i < shading.layers.size(); ++i) {
        const world::TerrainTexture& ground = terrain.textures[i];
        render::TerrainLayer& layer = shading.layers.at(i);
        const std::string at = "/components/HTerrain/textures/" + std::to_string(i) + "/";
        const auto texture = [&](const std::string& path, const char* field) {
            if (path.empty() || !textures_) {
                return render::MaterialTexture{};
            }
            auto made = textures_(path, render::Filter::Linear);
            if (!made) {
                problems_.push_back({entity, at + field, made.error().message});
                return render::MaterialTexture{};
            }
            return render::MaterialTexture{.texture = made.value().id, .repeat = true};
        };
        layer.albedo_bump = texture(ground.albedo_bump, "albedo_bump");
        layer.normal_roughness = texture(ground.normal_roughness, "normal_roughness");
        layer.uv_scale = ground.uv_scale;
        layer.colormap_opacity = ground.colormap_opacity;
    }

    const std::array<float, 16> transform = cells.matrix();
    const std::string prefix =
        "terrain:" + std::to_string(data->version) + "/" + std::to_string(chunk_size) + "/";
    for (const TerrainChunk& chunk : terrain_chunks(*data, chunk_size, terrain.lod_scale, viewer)) {
        const std::string key = prefix + std::to_string(chunk.x) + "," + std::to_string(chunk.z) +
                                "," + std::to_string(chunk.lod) + "," + std::to_string(chunk.seams);
        const auto found = shared_.find(key);
        auto made = found != shared_.end() && found->second.id != render::MeshId::None
                        ? kept_mesh(key, {}, {})
                        : kept_mesh(key, terrain_chunk_vertices(*data, chunk_size, chunk),
                                    terrain_indices(chunk_size, chunk_size, chunk.seams));
        if (!made) {
            problems_.push_back({entity, "/components/HTerrain", made.error().message});
            return;
        }
        Draw draw;
        draw.mesh = made.value();
        draw.transform = transform;
        draw.material = material;
        draw.layers = terrain.render_layers;
        draw.shadows = casting;
        draws_.push_back(draw);
    }
}

namespace {

// rendering/global_illumination/voxel_gi: the most lights Godot injects into a VoxelGI.
constexpr std::size_t kMaxVoxelGILights = 32;

// The bounds of a box under a column-major transform.
Bounds placed_bounds(const Bounds& box, const std::array<float, 16>& m) {
    Bounds out{.min = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                       std::numeric_limits<float>::max()},
               .max = {std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
                       std::numeric_limits<float>::lowest()}};
    for (int corner = 0; corner < 8; ++corner) {
        const float x = (corner & 1) != 0 ? box.max.x : box.min.x;
        const float y = (corner & 2) != 0 ? box.max.y : box.min.y;
        const float z = (corner & 4) != 0 ? box.max.z : box.min.z;
        const Vec3 p{(m[0] * x) + (m[4] * y) + (m[8] * z) + m[12],
                     (m[1] * x) + (m[5] * y) + (m[9] * z) + m[13],
                     (m[2] * x) + (m[6] * y) + (m[10] * z) + m[14]};
        out.min = {std::min(out.min.x, p.x), std::min(out.min.y, p.y), std::min(out.min.z, p.z)};
        out.max = {std::max(out.max.x, p.x), std::max(out.max.y, p.y), std::max(out.max.z, p.z)};
    }
    return out;
}

// Whether two boxes overlap, as Godot's AABB::intersects.
bool touches(const Bounds& a, const Bounds& b) {
    return a.min.x < b.max.x && b.min.x < a.max.x && a.min.y < b.max.y && b.min.y < a.max.y &&
           a.min.z < b.max.z && b.min.z < a.max.z;
}

// The bounds of an omni or spot light's reach, as Godot's LightStorage::light_get_aabb: a cube
// its range across for an omni light, the box round its cone for a spot light (a cube for a cone
// of 90 degrees or more, whose box Godot's tangent turns inside out).
Bounds light_bounds(const render::Light3D& light) {
    const Vec3 at{light.position[0], light.position[1], light.position[2]};
    const float range = light.range;
    if (light.type == render::LightType::Omni || light.spot_angle >= 90.0F) {
        return {.min = {at.x - range, at.y - range, at.z - range},
                .max = {at.x + range, at.y + range, at.z + range}};
    }
    const float size = std::tan(light.spot_angle * kRadians) * range;
    const Vec3 back = normalized({-light.direction[0], -light.direction[1], -light.direction[2]});
    const Vec3 up = normalized({light.up[0], light.up[1], light.up[2]});
    const Vec3 right{(up.y * back.z) - (up.z * back.y), (up.z * back.x) - (up.x * back.z),
                     (up.x * back.y) - (up.y * back.x)};
    const std::array<float, 16> place{right.x, right.y, right.z, 0.0F, up.x, up.y, up.z, 0.0F,
                                      back.x,  back.y,  back.z,  0.0F, at.x, at.y, at.z, 1.0F};
    return placed_bounds({.min = {-size, -size, -range}, .max = {size, size, 0.0F}}, place);
}

// What a light's component says of its indirect light: light_indirect_energy and bake_mode.
std::pair<float, std::string_view> indirect_of(const world::World& world, const Shone& shone) {
    const auto& ecs = world.ecs();
    if (shone.light.type == render::LightType::Directional) {
        const auto& light = ecs.get<const world::DirectionalLight3D>(shone.entity);
        return {light.light_indirect_energy, light.light_bake_mode};
    }
    if (shone.light.type == render::LightType::Omni) {
        const auto& light = ecs.get<const world::OmniLight3D>(shone.entity);
        return {light.light_indirect_energy, light.light_bake_mode};
    }
    const auto& light = ecs.get<const world::SpotLight3D>(shone.entity);
    return {light.light_indirect_energy, light.light_bake_mode};
}

// The lights a VoxelGI of data, placed at place, whose box in the world is box, injects, as Godot
// pairs them: the omni and spot lights whose bake_mode isn't "disabled" and whose reach touches
// its box, then every directional light, at most kMaxVoxelGILights.
std::vector<VoxelGILight> voxel_gi_lights(const world::World& world, std::span<const Shone> shone,
                                          const VoxelGIData& data, const world::Affine3D& place,
                                          const Bounds& box) {
    std::vector<VoxelGILight> out;
    const auto add = [&](const Shone& light) {
        if (out.size() < kMaxVoxelGILights) {
            out.push_back(
                voxel_gi_light(light.light, indirect_of(world, light).first, data, place));
        }
    };
    for (const Shone& light : shone) {
        if (light.light.type != render::LightType::Directional &&
            indirect_of(world, light).second != "disabled" &&
            touches(light_bounds(light.light), box)) {
            add(light);
        }
    }
    for (const Shone& light : shone) {
        if (light.light.type == render::LightType::Directional) {
            add(light);
        }
    }
    return out;
}

} // namespace

struct Renderer::KeptVoxelGI {
    std::shared_ptr<const VoxelGIData> data;
    VoxelGIOctree octree;
    // What its light was injected with: none yet while lit is false.
    bool lit = false;
    std::vector<VoxelGILight> lights;
    VoxelGISettings settings;
    VoxelGIAtlas atlas;
    std::string problem;       // why its light can't be traced
    std::uint64_t version = 0; // raised each time its light changes
};

void Renderer::destroy_voxel_gis() {
    if (voxel_atlas_ != render::TextureId::None) {
        device_->destroy_texture(voxel_atlas_);
    }
    voxel_atlas_ = render::TextureId::None;
    voxel_atlas_width_ = 0;
    voxel_atlas_height_ = 0;
    voxel_atlas_key_.clear();
}

void Renderer::trace_voxel_gis(const world::World& world, const world::Tree3D& tree,
                               const std::function<world::Affine3D(entt::entity)>& global,
                               const Environment& env) {
    const auto& ecs = world.ecs();
    // The VoxelGIs whose light is traced this frame, in the order they were made.
    struct Traced {
        entt::entity entity = entt::null;
        const KeptVoxelGI* kept = nullptr;
        const world::VoxelGI* component = nullptr;
        world::Affine3D place;
        Bounds box; // in the world
    };
    std::vector<Traced> traced;
    std::vector<entt::entity> seen;
    std::optional<std::vector<Shone>> shone;
    for (const entt::entity entity : made<world::VoxelGI>(world)) {
        const auto& gi = ecs.get<const world::VoxelGI>(entity);
        if (gi.data.empty() || !tree.visible(entity)) {
            continue;
        }
        const std::string field = "/components/VoxelGI/data";
        if (!voxel_gi_source_) {
            problems_.push_back({entity, field, "VoxelGIs can't load here"});
            continue;
        }
        auto loaded = voxel_gi_source_(gi.data);
        if (!loaded) {
            problems_.push_back({entity, field, loaded.error().message});
            continue;
        }
        seen.push_back(entity);
        std::unique_ptr<KeptVoxelGI>& kept = voxel_gis_[entity];
        if (!kept) {
            kept = std::make_unique<KeptVoxelGI>();
        }
        if (kept->data != loaded.value()) {
            kept->data = loaded.value();
            kept->octree = voxel_gi_octree(*kept->data, &world.pool());
            kept->lit = false;
        }
        const VoxelGIData& data = *kept->data;
        const world::Affine3D place = global(entity);
        const Vec3 low{data.bounds_position[0], data.bounds_position[1], data.bounds_position[2]};
        const Bounds box = placed_bounds(
            {.min = low,
             .max = low + Vec3{data.bounds_size[0], data.bounds_size[1], data.bounds_size[2]}},
            place.matrix());
        if (!shone) {
            shone = shining(world, tree, global);
        }
        std::vector<VoxelGILight> lights = voxel_gi_lights(world, *shone, data, place, box);
        const VoxelGISettings settings{.dynamic_range = gi.dynamic_range,
                                       .propagation = gi.propagation,
                                       .use_two_bounces = gi.use_two_bounces};
        // The lights are injected again when they or the settings change, as Godot's light
        // cache and voxel_gi_needs_update.
        if (!kept->lit || lights != kept->lights || settings != kept->settings) {
            const VoxelGITexture texture =
                light_voxel_gi(data, kept->octree, lights, settings, &world.pool());
            auto atlas = voxel_gi_atlas(texture);
            kept->lit = true;
            kept->lights = std::move(lights);
            kept->settings = settings;
            kept->version = ++voxel_gi_versions_;
            kept->problem = atlas ? std::string() : atlas.error().message;
            kept->atlas = atlas ? std::move(atlas).value() : VoxelGIAtlas{};
        }
        if (!kept->problem.empty()) {
            problems_.push_back({entity, field, kept->problem});
            continue;
        }
        traced.push_back(
            {.entity = entity, .kept = kept.get(), .component = &gi, .place = place, .box = box});
    }
    std::erase_if(voxel_gis_, [&](const auto& entry) {
        return std::ranges::find(seen, entry.first) == seen.end();
    });

    // Their atlases one under another in one atlas, which a mesh's two VoxelGIs share.
    std::vector<std::uint32_t> tops;
    std::vector<std::pair<entt::entity, std::uint64_t>> key;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    for (auto it = traced.begin(); it != traced.end();) {
        const render::Image& image = it->kept->atlas.image;
        if (height + image.height > render::kMaxTextureSize) {
            problems_.push_back({it->entity, "/components/VoxelGI/subdiv",
                                 "leaves its light no room in the atlas it shares with the "
                                 "VoxelGIs made before it: lower its subdiv, or theirs"});
            it = traced.erase(it);
            continue;
        }
        tops.push_back(height);
        key.emplace_back(it->entity, it->kept->version);
        height += image.height;
        width = std::max(width, image.width);
        ++it;
    }
    if (traced.empty()) {
        destroy_voxel_gis();
        return;
    }
    if (key != voxel_atlas_key_) {
        std::vector<std::uint8_t> pixels(std::size_t{width} * height * 4, 0);
        for (std::size_t i = 0; i < traced.size(); ++i) {
            const render::Image& image = traced[i].kept->atlas.image;
            const std::size_t row = std::size_t{image.width} * 4;
            for (std::uint32_t y = 0; y < image.height; ++y) {
                std::copy_n(image.pixels.begin() + static_cast<std::ptrdiff_t>(y * row), row,
                            pixels.begin() +
                                static_cast<std::ptrdiff_t>((std::size_t{tops[i]} + y) *
                                                            std::size_t{width} * 4));
            }
        }
        Result<void> made;
        if (voxel_atlas_ != render::TextureId::None && width == voxel_atlas_width_ &&
            height == voxel_atlas_height_) {
            made = device_->update_texture(voxel_atlas_, pixels);
        } else {
            destroy_voxel_gis();
            auto created = device_->create_texture(
                {.width = width, .height = height, .filter = render::Filter::Linear}, pixels);
            if (created) {
                voxel_atlas_ = created.value();
                voxel_atlas_width_ = width;
                voxel_atlas_height_ = height;
            } else {
                made = created.error();
            }
        }
        if (!made) {
            problems_.push_back({traced.front().entity, "/components/VoxelGI/data",
                                 "its light's atlas: " + made.error().message});
            destroy_voxel_gis();
            return;
        }
        voxel_atlas_key_ = std::move(key);
    }

    // Each draw traces the VoxelGIs its bounds touch, at most two in the order they were made, as
    // Godot pairs them (MAX_INSTANCE_PAIRS); lightmapped, captured, terrain and shadow-only draws
    // take none.
    std::vector<render::VoxelGIProbe> probes;
    probes.reserve(traced.size());
    for (std::size_t i = 0; i < traced.size(); ++i) {
        const Traced& t = traced[i];
        const float normalization =
            env.attributes != entt::null ? env.exposure / t.kept->data->baked_exposure : 1.0F;
        render::VoxelGIProbe probe =
            voxel_gi_probe(*t.component, *t.kept->data, t.kept->atlas, t.place, normalization);
        for (std::uint32_t level = 0; level < probe.levels; ++level) {
            probe.placement.at(level)[1] += tops[i];
        }
        probes.push_back(probe);
    }
    std::unordered_map<render::MeshId, Bounds> boxes;
    for (const auto& [mesh_key, shared] : shared_) {
        if (shared.used == frame_) {
            boxes.emplace(shared.id, shared.box);
        }
    }
    for (const auto& [mesh_key, skinned] : skinned_) {
        if (skinned.used == frame_) {
            boxes.emplace(skinned.id, skinned.box);
        }
    }
    for (Draw& draw : draws_) {
        const render::Material3D& material = draw.material;
        if (draw.shadows == render::ShadowCasting::ShadowsOnly || material.terrain.enabled ||
            material.lightmap.texture != render::TextureId::None || material.capture.enabled) {
            continue;
        }
        const auto box = boxes.find(draw.mesh);
        if (box == boxes.end()) {
            continue;
        }
        const Bounds placed = placed_bounds(box->second, draw.transform);
        render::VoxelGIShading shading{.atlas = voxel_atlas_, .probes = {}, .count = 0};
        for (std::size_t i = 0; i < traced.size() && shading.count < shading.probes.size(); ++i) {
            if (touches(placed, traced[i].box)) {
                shading.probes.at(shading.count++) = probes[i];
            }
        }
        if (shading.count > 0) {
            draw.material.voxel_gi = shading;
        }
    }
}

std::optional<Frame3D> Renderer::prepare(const world::World& world, float width, float height,
                                         double interpolation, render::Color clear,
                                         const render::ShadowSettings& shadows, double time,
                                         const render::DepthOfFieldSettings& depth_of_field,
                                         const render::SsaoSettings& ssao,
                                         const render::SsrSettings& ssr,
                                         const render::ReflectionAtlasSettings& reflection_atlas,
                                         render::RenderMethod method, bool voxel_gi_high_quality) {
    ++frame_;
    draws_.clear();
    problems_.clear();
    lights_ = 0;
    const auto& ecs = world.ecs();
    const world::Tree3D tree(world);
    const entt::entity camera = scene3d::current_camera(world, tree);
    if (camera == entt::null) {
        // Meshes and images kept for a world that has none now go.
        for (auto it = shared_.begin(); it != shared_.end();) {
            device_->destroy_mesh(it->second.id);
            it = shared_.erase(it);
        }
        for (const auto& [key, texture] : embedded_) {
            device_->destroy_texture(texture.id);
        }
        embedded_.clear();
        cached_.clear();
        for (const auto& [key, skinned] : skinned_) {
            device_->destroy_mesh(skinned.id);
        }
        skinned_.clear();
        lit_.clear();
        destroy_lightmaps(false);
        probe_sources_.clear();
        captured_.clear();
        model_centers_.clear();
        voxel_gis_.clear();
        destroy_voxel_gis();
        exposure_version_ = 0;
        return std::nullopt;
    }

    // Where entities are part of the way between the last two ticks.
    world::Tree3D::Locals locals;
    const auto t = static_cast<float>(std::clamp(interpolation, 0.0, 1.0));
    if (t < 1.0F) {
        for (const auto& [entity, was] : before_) {
            const auto* now =
                world.valid(entity) ? ecs.try_get<world::Transform3D>(entity) : nullptr;
            if (now == nullptr) {
                continue;
            }
            const Quat rotation = world::quaternion(now->rotation);
            if (was.position == now->position && was.rotation == rotation &&
                was.scale == now->scale) {
                continue;
            }
            world::Affine3D local = world::rotation_basis(world::nlerp(was.rotation, rotation, t));
            const Vec3 scale = was.scale + scaled(now->scale - was.scale, t);
            local.x = scaled(local.x, scale.x);
            local.y = scaled(local.y, scale.y);
            local.z = scaled(local.z, scale.z);
            local.origin = was.position + scaled(now->position - was.position, t);
            locals.emplace(entity, local);
        }
    }
    const auto global = [&](entt::entity entity) { return tree.global(entity, locals); };

    Frame3D frame;
    frame.camera = camera;
    const auto& lens = ecs.get<const world::Camera3D>(camera);
    const float aspect = width > 0.0F && height > 0.0F ? width / height : 1.0F;
    const CameraView view = camera_view(lens, global(camera), aspect);
    frame.scene.camera = view.view;
    frame.scene.projection = view.projection;
    const Environment env = environment(world, clear, camera);
    frame.clear = env.background;
    frame.scene.ambient = env.ambient;
    frame.scene.ambient_energy = env.ambient_energy;
    frame.scene.post = env.post;
    if (env.post.auto_exposure) {
        // As Godot's auto exposure version: the brightness is measured afresh when auto exposure
        // turns on or follows other attributes, and here when game time goes back too, as after a
        // restore; otherwise it moves toward the frame's by auto_exposure_speed per second.
        if (exposure_version_ == 0 || env.attributes != exposure_entity_ || time < exposure_time_) {
            exposure_version_ = ++exposure_versions;
            exposure_entity_ = env.attributes;
            exposure_time_ = time;
        }
        const float speed =
            ecs.get<const world::CameraAttributesPractical>(env.attributes).auto_exposure_speed;
        frame.scene.post.auto_exposure_adjust = speed * static_cast<float>(time - exposure_time_);
        frame.scene.post.auto_exposure_version = exposure_version_;
        exposure_time_ = time;
    } else {
        exposure_version_ = 0;
    }
    frame.scene.post.depth_of_field = depth_of_field;
    frame.scene.ssao = env.ssao;
    frame.scene.ssao.settings = ssao;
    frame.scene.ssr = env.ssr;
    frame.scene.ssr.settings = ssr;
    if (depth_of_field.use_jitter) {
        // Godot seeds the jitter at random each frame; the game time keeps frames repeatable.
        frame.scene.post.dof_jitter_seed = static_cast<float>(time - std::floor(time)) * 1000.0F;
    }
    if (env.post.adjustments && !env.color_correction.empty()) {
        // Sampled smoothly, as Godot samples its LUTs.
        auto lut = textures_(env.color_correction, render::Filter::Linear);
        const char* const field = "/components/WorldEnvironment/adjustment_color_correction";
        if (!lut) {
            problems_.push_back({env.entity, field, lut.error().message});
        } else if (lut.value().height != 1 &&
                   lut.value().width != lut.value().height * lut.value().height) {
            problems_.push_back(
                {env.entity, field,
                 "a color correction texture is 1 pixel tall, or as wide as its height squared, "
                 "not " +
                     std::to_string(lut.value().width) + "x" + std::to_string(lut.value().height)});
        } else {
            frame.scene.post.color_correction = lut.value().id;
        }
    }
    frame.scene.sky = env.sky;
    frame.scene.fog = env.fog;
    frame.scene.radiance = env.radiance;
    problems_.insert(problems_.end(), env.problems.begin(), env.problems.end());
    if (env.sky.kind != render::SkyKind::None || env.radiance.kind != render::SkyKind::None) {
        frame.scene.sky.lights = sky_lights(world, tree, global);
        for (render::SkyLight& sun : frame.scene.sky.lights) {
            sun.energy *= env.exposure;
        }
    }
    if (!env.sky_texture.empty()) {
        auto texture = textures_(env.sky_texture, env.sky_filter);
        if (!texture) {
            problems_.push_back({env.entity, env.sky_texture_field, texture.error().message});
        } else if (env.sky_material == render::SkyKind::Procedural) {
            frame.scene.sky.procedural.sky_cover = texture.value().id;
        } else if (env.sky_material == render::SkyKind::Physical) {
            frame.scene.sky.physical.night_sky = texture.value().id;
        } else {
            frame.scene.sky.panorama.panorama = texture.value().id;
        }
    }
    frame.scene.lights = scene3d::lights(world, tree, global);
    for (render::Light3D& light : frame.scene.lights) {
        light.energy *= env.exposure;
    }
    frame.scene.shadows = shadows;
    frame.scene.voxel_gi_high_quality = voxel_gi_high_quality;
    lights_ = frame.scene.lights.size();
    capture_time_ = time;
    find_lightmaps(world, env.exposure, global);

    for (const entt::entity entity : made<world::MeshInstance3D>(world)) {
        const auto& instance = ecs.get<const world::MeshInstance3D>(entity);
        if (!tree.visible(entity)) {
            continue;
        }
        // A mesh the camera doesn't see still casts, as Godot's: it draws only into shadow maps.
        render::ShadowCasting casting = casting_of(instance.cast_shadow);
        if ((instance.layers & lens.cull_mask) == 0) {
            if (casting == render::ShadowCasting::Off) {
                continue;
            }
            casting = render::ShadowCasting::ShadowsOnly;
        }
        if (!instance.mesh.empty()) {
            add_model(world, entity, instance, global(entity), global, casting, t);
            continue;
        }
        const std::optional<Primitive> primitive = drawable(world, entity, problems_);
        if (!primitive) {
            continue;
        }
        auto mesh = mesh_for(world, entity, *primitive);
        if (!mesh) {
            const std::string name(kPrimitiveNames[primitive->index()]);
            problems_.push_back({entity, "/components/" + name, mesh.error().message});
            continue;
        }
        Draw draw;
        draw.mesh = mesh.value();
        draw.transform = global(entity).matrix();
        if (const auto* standard = ecs.try_get<world::StandardMaterial3D>(entity);
            standard != nullptr) {
            draw.material = material(*standard, textures_, problems_, entity);
        }
        if (instance.gi_mode == "dynamic") {
            const Shared& shared = shared_.at(cached_.at(entity).key);
            draw.material.capture = capture(entity, global(entity).apply(shared.center));
        } else if (instance.gi_mode == "static") {
            const LitSource source{.key = "primitive:" + cached_.at(entity).key,
                                   .make = [&primitive](std::vector<render::Vertex3D>& vertices,
                                                        std::vector<std::uint32_t>& indices) {
                                       if (auto data = make_mesh(*primitive)) {
                                           vertices = std::move(data.value().vertices);
                                           indices = std::move(data.value().indices);
                                       }
                                   }};
            render::MeshId lit = render::MeshId::None;
            if (const render::LightmapShading* lightmap =
                    lit_surface(entity, 0, source, 0, kAllIndices, lit)) {
                draw.mesh = lit;
                draw.material.lightmap = *lightmap;
            }
        }
        draw.layers = instance.layers;
        draw.shadows = casting;
        draws_.push_back(draw);
    }
    // CSG roots draw their results; the shapes under them draw nothing of their own.
    for (const entt::entity root : csg_roots(world, tree)) {
        if (tree.visible(root)) {
            add_csg(world, tree, root, global(root), lens.cull_mask);
        }
    }
    // GridMaps draw their octants.
    for (const entt::entity entity : made<world::GridMap>(world)) {
        if (tree.visible(entity)) {
            add_gridmap(world, entity, global(entity), lens.cull_mask);
        }
    }
    // Terrains draw the chunks their quad trees settle on for the camera.
    for (const entt::entity entity : made<world::HTerrain>(world)) {
        if (tree.visible(entity)) {
            add_terrain(world, entity, global(entity), global(camera), lens);
        }
    }
    if (method == render::RenderMethod::ForwardPlus) {
        trace_voxel_gis(world, tree, global, env);
    } else {
        voxel_gis_.clear();
        destroy_voxel_gis();
    }
    if (env.exposure != 1.0F) {
        for (Draw& draw : draws_) {
            draw.material.emission_energy *= env.exposure;
        }
    }
    // Meshes no entity drew this frame go, and so do the entities' records of them.
    for (auto it = shared_.begin(); it != shared_.end();) {
        if (it->second.used != frame_) {
            device_->destroy_mesh(it->second.id);
            it = shared_.erase(it);
        } else {
            ++it;
        }
    }
    std::erase_if(cached_, [&](const auto& entry) { return !shared_.contains(entry.second.key); });
    for (auto it = skinned_.begin(); it != skinned_.end();) {
        if (it->second.used != frame_) {
            device_->destroy_mesh(it->second.id);
            it = skinned_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = embedded_.begin(); it != embedded_.end();) {
        if (it->second.used != frame_) {
            device_->destroy_texture(it->second.id);
            it = embedded_.erase(it);
        } else {
            ++it;
        }
    }
    destroy_lightmaps(true);
    std::erase_if(captured_, [&](const auto& entry) { return entry.second.used != frame_; });
    std::erase_if(model_centers_, [&](const auto& entry) { return entry.second.second != frame_; });
    if (auto reflected = reflect(world, frame, clear, reflection_atlas, method); !reflected) {
        problems_.push_back(
            {.entity = camera, .path = "ReflectionProbe", .message = reflected.error().message});
    }
    return frame;
}

namespace {

render::ReflectionAmbient reflection_ambient(const std::string& mode) {
    if (mode == "color") {
        return render::ReflectionAmbient::Color;
    }
    return mode == "disabled" ? render::ReflectionAmbient::Disabled
                              : render::ReflectionAmbient::Environment;
}

} // namespace

Result<void> Renderer::reflect(const world::World& world, Frame3D& frame, render::Color clear,
                               const render::ReflectionAtlasSettings& settings,
                               render::RenderMethod method) {
    const auto& ecs = world.ecs();
    const world::Tree3D tree(world);
    // The probes, largest first as Godot sorts them, then in the order they were made.
    struct Found {
        entt::entity entity;
        const world::ReflectionProbe* probe;
        world::Affine3D place;
        float extent;
    };
    std::vector<Found> found;
    if (method == render::RenderMethod::ForwardPlus) {
        for (const auto [entity, probe] : ecs.view<const world::ReflectionProbe>().each()) {
            if (!ecs.all_of<world::Transform3D>(entity) || !tree.visible(entity)) {
                continue;
            }
            const Vec3 half = scaled(probe.size, 0.5F);
            found.push_back(
                {.entity = entity,
                 .probe = &probe,
                 .place = orthonormalized(tree.global(entity)),
                 .extent = std::sqrt((half.x * half.x) + (half.y * half.y) + (half.z * half.z))});
        }
    }
    std::ranges::sort(found, [](const Found& a, const Found& b) {
        return a.extent != b.extent ? a.extent > b.extent
                                    : entt::to_integral(a.entity) < entt::to_integral(b.entity);
    });
    const std::uint32_t most = std::min(settings.reflection_count, render::kMaxReflectionCubes);
    const auto wanted = static_cast<std::uint32_t>(std::min<std::size_t>(found.size(), most));
    const std::uint32_t cubes = wanted == 0 ? 0 : std::min(std::bit_ceil(wanted), most);
    if (cubes != atlas_cubes_ || (cubes > 0 && settings.reflection_size != atlas_size_)) {
        if (atlas_ != render::TextureId::None) {
            device_->destroy_texture(atlas_);
        }
        atlas_ = render::TextureId::None;
        atlas_cubes_ = 0;
        probe_slots_.clear();
        if (cubes > 0) {
            auto made = device_->create_texture({.width = settings.reflection_size,
                                                 .height = settings.reflection_size,
                                                 .cubes = cubes},
                                                {});
            if (!made) {
                return made.error();
            }
            atlas_ = made.value();
            atlas_size_ = settings.reflection_size;
            atlas_cubes_ = cubes;
        }
    }
    // Probes no longer drawn free their cubes; new ones take the first free cube.
    std::erase_if(probe_slots_, [&](const auto& entry) {
        return std::ranges::none_of(found, [&](const Found& f) { return f.entity == entry.first; });
    });
    std::vector<bool> taken(atlas_cubes_, false);
    for (const auto& [entity, slot] : probe_slots_) {
        taken.at(slot.cube) = true;
    }
    for (const Found& f : found) {
        if (probe_slots_.contains(f.entity)) {
            continue;
        }
        const auto free = std::ranges::find(taken, false);
        if (free == taken.end()) {
            continue; // past the atlas's cubes: no reflection
        }
        *free = true;
        probe_slots_[f.entity] = {
            .cube = static_cast<std::uint32_t>(std::distance(taken.begin(), free)), .drawn = {}};
    }

    render::Reflections3D& reflections = frame.scene.reflections;
    reflections = {.atlas = atlas_, .probes = {}, .exposure = 1.0F};
    for (const Found& f : found) {
        const auto slot = probe_slots_.find(f.entity);
        if (slot == probe_slots_.end()) {
            continue;
        }
        const world::ReflectionProbe& p = *f.probe;
        const std::array<float, 16> transform = f.place.matrix();
        const ProbeKey key{.transform = transform,
                           .size = p.size,
                           .origin_offset = p.origin_offset,
                           .max_distance = p.max_distance,
                           .cull_mask = p.cull_mask,
                           .interior = p.interior,
                           .enable_shadows = p.enable_shadows};
        if (p.update_mode == "always" || slot->second.drawn != key) {
            // Godot's _render_reflection_probe_step: the world from origin_offset through each
            // face, linear and unprocessed, with no SSAO, SSR or probes.
            render::Scene3D scene = frame.scene;
            scene.method = render::RenderMethod::ForwardPlus;
            scene.ssao.enabled = false;
            scene.ssr.enabled = false;
            scene.reflections = {};
            scene.post = {};
            for (render::Light3D& light : scene.lights) {
                light.shadow = light.shadow && p.enable_shadows;
            }
            render::Color background = frame.clear;
            if (p.interior) {
                scene.sky = {};
                scene.fog = {};
                scene.radiance = {};
                scene.ambient = {0.0F, 0.0F, 0.0F};
                background = clear;
            }
            const std::array<float, 3> size{p.size.x, p.size.y, p.size.z};
            const std::array<float, 3> offset{p.origin_offset.x, p.origin_offset.y,
                                              p.origin_offset.z};
            for (std::uint32_t face = 0; face < 6; ++face) {
                scene.camera = render::probe_face_view(transform, offset, face);
                scene.projection =
                    render::probe_face_projection(size, offset, p.max_distance, face);
                scene.capture =
                    render::ProbeCapture{.atlas = atlas_, .cube = slot->second.cube, .face = face};
                render::Frame capture;
                capture.clear = background;
                capture.scene = scene;
                auto began = device_->begin_frame(capture);
                if (!began) {
                    return began.error();
                }
                for (const Draw& draw : draws_) {
                    if ((draw.layers & p.cull_mask) == 0) {
                        continue;
                    }
                    if (auto drawn = device_->draw_mesh(draw.mesh, draw.transform, draw.material,
                                                        draw.layers, draw.shadows);
                        !drawn) {
                        (void)device_->end_frame();
                        return drawn;
                    }
                }
                if (auto ended = device_->end_frame(); !ended) {
                    return ended;
                }
            }
            slot->second.drawn = key;
        }
        reflections.probes.push_back(
            {.transform = transform,
             .size = {p.size.x, p.size.y, p.size.z},
             .origin_offset = {p.origin_offset.x, p.origin_offset.y, p.origin_offset.z},
             .intensity = p.intensity,
             .blend_distance = p.blend_distance,
             .box_projection = p.box_projection,
             .reflection_mask = p.reflection_mask,
             .ambient_mode = reflection_ambient(p.ambient_mode),
             .ambient_color = {p.ambient_color.x, p.ambient_color.y, p.ambient_color.z},
             .ambient_color_energy = p.ambient_color_energy,
             .cube = slot->second.cube});
    }
    return {};
}

Result<void> Renderer::draw() {
    for (const Draw& draw : draws_) {
        if (auto drawn = device_->draw_mesh(draw.mesh, draw.transform, draw.material, draw.layers,
                                            draw.shadows);
            !drawn) {
            return drawn;
        }
    }
    return {};
}

} // namespace slash::scene3d
