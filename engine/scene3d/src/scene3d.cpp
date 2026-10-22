#include "slash/scene3d/scene3d.hpp"

#include "slash/core/json.hpp"
#include "slash/core/math.hpp"
#include "slash/core/text.hpp"
#include "slash/scene3d/ik.hpp"
#include "slash/world/serialize.hpp"
#include "slash/world/world.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <functional>
#include <numbers>
#include <ranges>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace slash::scene3d {

namespace {

constexpr float kDegrees = 180.0F / std::numbers::pi_v<float>;
constexpr float kRadians = std::numbers::pi_v<float> / 180.0F;

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

// Godot's Basis::orthonormalize: Gram-Schmidt on the columns, x first.
world::Affine3D orthonormalized(world::Affine3D t) {
    t.x = normalized(t.x);
    t.y = normalized(t.y - scaled(t.x, dot(t.x, t.y)));
    t.z = normalized(t.z - scaled(t.x, dot(t.x, t.z)) - scaled(t.y, dot(t.y, t.z)));
    return t;
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

// Each light of type T that shines, with where it is, keyed by its place in tree order.
template <typename T>
void add_lights(const world::World& world, const world::Tree3D& tree,
                const std::function<world::Affine3D(entt::entity)>& global,
                const std::unordered_map<entt::entity, std::size_t>& order,
                std::vector<std::pair<std::size_t, render::Light3D>>& out) {
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
        out.emplace_back(at != order.end() ? at->second : order.size(), shone);
    }
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
    const auto& ecs = world.ecs();
    entt::entity first = entt::null;
    for (const entt::entity entity : tree.order()) {
        if (const auto* camera = ecs.try_get<world::Camera3D>(entity); camera != nullptr) {
            if (camera->current) {
                return entity;
            }
            if (first == entt::null) {
                first = entity;
            }
        }
    }
    // Cameras without a Transform3D come after those in the tree, in the order they were made.
    for (const entt::entity entity : made<world::Camera3D>(world)) {
        if (ecs.all_of<world::Transform3D>(entity)) {
            continue;
        }
        if (ecs.get<const world::Camera3D>(entity).current) {
            return entity;
        }
        if (first == entt::null) {
            first = entity;
        }
    }
    return first;
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

Environment environment(const world::World& world, render::Color clear) {
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
    const auto* mode = std::ranges::find(render::kTonemapperNames, env.tonemap_mode);
    if (mode != render::kTonemapperNames.end()) {
        out.post.tonemapper =
            static_cast<render::Tonemapper>(std::distance(render::kTonemapperNames.begin(), mode));
    }
    out.post.exposure = env.tonemap_exposure;
    out.post.white = env.tonemap_white;
    out.post.glow = env.glow_enabled;
    out.post.glow_intensity = env.glow_intensity;
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

std::vector<render::Light3D> lights(const world::World& world, const world::Tree3D& tree,
                                    const std::function<world::Affine3D(entt::entity)>& global) {
    std::unordered_map<entt::entity, std::size_t> order;
    for (std::size_t i = 0; i < tree.order().size(); ++i) {
        order.emplace(tree.order()[i], i);
    }
    std::vector<std::pair<std::size_t, render::Light3D>> found;
    add_lights<world::DirectionalLight3D>(world, tree, global, order, found);
    add_lights<world::OmniLight3D>(world, tree, global, order, found);
    add_lights<world::SpotLight3D>(world, tree, global, order, found);
    std::ranges::stable_sort(found, {}, &std::pair<std::size_t, render::Light3D>::first);
    std::vector<render::Light3D> out;
    for (auto& [place, light] : found) {
        if (out.size() == render::kMaxLights3D) {
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
        if (errors.empty() &&
            (world.ecs().all_of<world::Skeleton3D>(entity) || has_modifiers(world, entity))) {
            if (auto model = load_model(bytes.value(), path, reader); model) {
                check_bones(entity, &model.value());
                check_ik(entity, &model.value());
            }
        }
    }
    for (ModifierProblem& problem : check_stray_modifiers(world)) {
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

Renderer::Renderer(render::RenderDevice& device, TextureSource textures, ModelSource models)
    : device_(&device), textures_(std::move(textures)), models_(std::move(models)) {}

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
    }
    shared.used = frame_;
    return shared.id;
}

Result<render::MeshId> Renderer::model_mesh(const std::string& key, const ModelSurface& surface) {
    Shared& shared = shared_[key];
    if (shared.id == render::MeshId::None) {
        auto made = device_->create_mesh(surface.vertices, surface.indices);
        if (!made) {
            shared_.erase(key);
            return made.error();
        }
        shared.id = made.value();
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
    return skinned.id;
}

render::MaterialTexture Renderer::model_texture(const std::string& path, const SourceModel& source,
                                                int image,
                                                const world::StandardMaterial3D& standard,
                                                entt::entity entity) {
    if (image < 0 || static_cast<std::size_t>(image) >= source.model->images.size()) {
        return {};
    }
    const bool nearest = standard.texture_filter.starts_with("nearest");
    const render::Filter filter = nearest ? render::Filter::Nearest : render::Filter::Linear;
    const ModelImage& found = source.model->images[static_cast<std::size_t>(image)];
    if (!found.path.empty()) {
        auto made = textures_(found.path, filter);
        if (!made) {
            problems_.push_back({entity, std::string(kMeshField),
                                 "\"" + path + "\"'s texture: " + made.error().message});
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
            problems_.push_back({entity, std::string(kMeshField),
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
        for (std::size_t i = 0; i < mesh.surfaces.size(); ++i) {
            const ModelSurface& surface = mesh.surfaces[i];
            // A skin places its surfaces by its joints, leaving out its part's node.
            const bool skinned = !joints.empty() && !surface.weights.empty();
            auto made =
                skinned ? skinned_mesh(skin_prefix + std::to_string(p) + "/" + std::to_string(i),
                                       surface, joints)
                        : model_mesh(prefix + std::to_string(part.mesh) + "/" + std::to_string(i),
                                     surface);
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
                                             entity);
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
            draw.layers = instance.layers;
            draw.shadows = casting;
            draws_.push_back(draw);
        }
    }
}

std::optional<Frame3D> Renderer::prepare(const world::World& world, float width, float height,
                                         double interpolation, render::Color clear,
                                         const render::ShadowSettings& shadows) {
    ++frame_;
    draws_.clear();
    problems_.clear();
    lights_ = 0;
    const auto& ecs = world.ecs();
    const world::Tree3D tree(world);
    const entt::entity camera = current_camera(world, tree);
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
    const Environment env = environment(world, clear);
    frame.clear = env.background;
    frame.scene.ambient = env.ambient;
    frame.scene.ambient_energy = env.ambient_energy;
    frame.scene.post = env.post;
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
    frame.scene.shadows = shadows;
    lights_ = frame.scene.lights.size();

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
        draw.layers = instance.layers;
        draw.shadows = casting;
        draws_.push_back(draw);
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
    return frame;
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
