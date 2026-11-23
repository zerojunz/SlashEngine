#include "slash/world/aliases.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace slash::world {
namespace {

// Sorted by old name.
constexpr std::array kComponents = std::to_array<NameAlias>({
    {"AnimatableBody2D", "MovingBody2D"},
    {"AnimatableBody3D", "MovingBody3D"},
    {"AnimatedSprite2D", "Flipbook2D"},
    {"AnimationPlayer", "Animator"},
    {"AnimationTree", "AnimationGraph"},
    {"Area2D", "Zone2D"},
    {"Area3D", "Zone3D"},
    {"AudioListener2D", "Listener2D"},
    {"AudioListener3D", "Listener3D"},
    {"AudioStreamPlayer", "SoundPlayer"},
    {"AudioStreamPlayer2D", "SoundPlayer2D"},
    {"AudioStreamPlayer3D", "SoundPlayer3D"},
    {"BoneAttachment3D", "BoneSocket3D"},
    {"BoxContainer", "StackBox"},
    {"BoxMesh", "BoxGeometry"},
    {"CCDIK3D", "CcdIK3D"},
    {"CPUParticles2D", "Emitter2D"},
    {"CPUParticles3D", "Emitter3D"},
    {"CSGBox3D", "SolidBox3D"},
    {"CSGCombiner3D", "SolidGroup3D"},
    {"CSGCylinder3D", "SolidCylinder3D"},
    {"CSGMesh3D", "SolidMesh3D"},
    {"CSGPolygon3D", "SolidExtrusion3D"},
    {"CSGSphere3D", "SolidSphere3D"},
    {"CSGTorus3D", "SolidTorus3D"},
    {"CameraAttributesPractical", "CameraLens"},
    {"CanvasLayer", "ScreenLayer"},
    {"CanvasModulate", "AmbientTint2D"},
    {"CapsuleMesh", "CapsuleGeometry"},
    {"CenterContainer", "CenterBox"},
    {"CharacterBody2D", "CharacterMover2D"},
    {"CharacterBody3D", "CharacterMover3D"},
    {"CheckButton", "ToggleSwitch"},
    {"CodeEdit", "CodeEditor"},
    {"CodeHighlighter", "SyntaxColors"},
    {"CollisionPolygon2D", "PolygonCollider2D"},
    {"CollisionShape2D", "Collider2D"},
    {"CollisionShape3D", "Collider3D"},
    {"ColorRect", "ColorFill"},
    {"ConeTwistJoint3D", "ConeJoint3D"},
    {"Control", "Widget"},
    {"CopyTransformModifier3D", "BoneCopy3D"},
    {"CylinderMesh", "CylinderGeometry"},
    {"DampedSpringJoint2D", "SpringJoint2D"},
    {"DirectionalLight2D", "SunLight2D"},
    {"DirectionalLight3D", "SunLight3D"},
    {"FABRIK3D", "FabrikIK3D"},
    {"FlowContainer", "FlowBox"},
    {"FogMaterial", "FogVolumeLook"},
    {"GPUParticles2D", "GpuEmitter2D"},
    {"GPUParticles3D", "GpuEmitter3D"},
    {"GPUParticlesAttractorBox3D", "ParticleAttractorBox3D"},
    {"GPUParticlesAttractorSphere3D", "ParticleAttractorSphere3D"},
    {"GPUParticlesAttractorVectorField3D", "ParticleFlowField3D"},
    {"GPUParticlesCollisionBox3D", "ParticleColliderBox3D"},
    {"GPUParticlesCollisionHeightField3D", "ParticleColliderHeight3D"},
    {"GPUParticlesCollisionSDF3D", "ParticleColliderSdf3D"},
    {"GPUParticlesCollisionSphere3D", "ParticleColliderSphere3D"},
    {"GaussianSplat3D", "Splat3D"},
    {"Generic6DOFJoint3D", "SixAxisJoint3D"},
    {"GridContainer", "GridBox"},
    {"GridMap", "BlockMap3D"},
    {"GrooveJoint2D", "TrackJoint2D"},
    {"HTerrain", "Terrain3D"},
    {"HairInstance3D", "Hair3D"},
    {"LightOccluder2D", "Occluder2D"},
    {"LightmapGI", "BakedLighting3D"},
    {"LightmapProbe", "LightProbe3D"},
    {"LineEdit", "TextField"},
    {"LookAtModifier3D", "BoneLookAt3D"},
    {"MarginContainer", "MarginBox"},
    {"MeshInstance3D", "Model3D"},
    {"MultiMeshInstance3D", "ModelInstances3D"},
    {"MultiplayerSpawner", "NetSpawner"},
    {"MultiplayerSynchronizer", "NetSync"},
    {"NavigationAgent2D", "NavAgent2D"},
    {"NavigationAgent3D", "NavAgent3D"},
    {"NavigationLink2D", "NavLink2D"},
    {"NavigationLink3D", "NavLink3D"},
    {"NavigationObstacle2D", "NavObstacle2D"},
    {"NavigationObstacle3D", "NavObstacle3D"},
    {"NavigationRegion2D", "NavArea2D"},
    {"NavigationRegion3D", "NavArea3D"},
    {"NinePatchRect", "NineSlice"},
    {"OccluderInstance3D", "Occluder3D"},
    {"OmniLight3D", "PointLight3D"},
    {"OptionButton", "Dropdown"},
    {"PanelContainer", "PanelBox"},
    {"PanoramaSkyMaterial", "SkyPanorama"},
    {"Path2D", "Spline2D"},
    {"Path3D", "Spline3D"},
    {"PathFollow2D", "SplineFollower2D"},
    {"PathFollow3D", "SplineFollower3D"},
    {"PhysicalBone3D", "SimulatedBone3D"},
    {"PhysicalBoneSimulator3D", "BoneSimulation3D"},
    {"PhysicalSkyMaterial", "SkyScattering"},
    {"PlaneMesh", "PlaneGeometry"},
    {"PopupMenu", "PopupList"},
    {"PrismMesh", "PrismGeometry"},
    {"ProceduralSkyMaterial", "SkyGradient"},
    {"QuadMesh", "QuadGeometry"},
    {"ScrollContainer", "ScrollBox"},
    {"SliderJoint3D", "SlideJoint3D"},
    {"SphereMesh", "SphereGeometry"},
    {"StandardMaterial3D", "Surface3D"},
    {"TextEdit", "TextArea"},
    {"TextureButton", "ImageButton"},
    {"TextureRect", "Picture"},
    {"TileMapLayer", "TileLayer2D"},
    {"TorusMesh", "TorusGeometry"},
    {"VehicleBody3D", "WheeledBody3D"},
    {"VehicleWheel3D", "Wheel3D"},
    {"VideoStreamPlayer", "Video"},
    {"VoxelGI", "VoxelLighting3D"},
    {"WorldEnvironment", "Environment3D"},
    {"XRFaceModifier3D", "XRFace3D"},
});

// Sorted by component, then old name.
constexpr std::array kFields = std::to_array<FieldAlias>({
    {"AnimationGraph", "tree_root", "root"},
    {"BoneLookAt3D", "origin_external_node", "origin_entity"},
    {"BoneLookAt3D", "target_node", "target"},
    {"BoneSocket3D", "external_skeleton", "skeleton_entity"},
    {"Button", "action_mode", "press_on"},
    {"Button", "autowrap_mode", "wrap"},
    {"Button", "button_pressed", "pressed"},
    {"Button", "keep_pressed_outside", "hold_outside"},
    {"Button", "text_overrun_behavior", "overflow"},
    {"Button", "toggle_mode", "toggles"},
    {"Camera3D", "cull_mask", "layer_filter"},
    {"CharacterMover2D", "motion_mode", "movement"},
    {"CharacterMover2D", "up_direction", "up"},
    {"CharacterMover3D", "motion_mode", "movement"},
    {"CharacterMover3D", "up_direction", "up"},
    {"CheckBox", "action_mode", "press_on"},
    {"CheckBox", "autowrap_mode", "wrap"},
    {"CheckBox", "button_pressed", "pressed"},
    {"CheckBox", "keep_pressed_outside", "hold_outside"},
    {"CheckBox", "text_overrun_behavior", "overflow"},
    {"CheckBox", "toggle_mode", "toggles"},
    {"Cloth2D", "light_mask", "light_layers"},
    {"Cloth2D", "texture_filter", "filter"},
    {"Cloth2D", "z_as_relative", "sort_order_relative"},
    {"Cloth2D", "z_index", "sort_order"},
    {"ConeJoint3D", "node_a", "body_a"},
    {"ConeJoint3D", "node_b", "body_b"},
    {"Decal", "cull_mask", "layer_filter"},
    {"Decal", "modulate", "tint"},
    {"Dropdown", "action_mode", "press_on"},
    {"Dropdown", "autowrap_mode", "wrap"},
    {"Dropdown", "button_pressed", "pressed"},
    {"Dropdown", "keep_pressed_outside", "hold_outside"},
    {"Dropdown", "text_overrun_behavior", "overflow"},
    {"Dropdown", "toggle_mode", "toggles"},
    {"Emitter2D", "explosiveness", "burst"},
    {"Emitter2D", "hframes", "columns"},
    {"Emitter2D", "light_mask", "light_layers"},
    {"Emitter2D", "local_coords", "local_space"},
    {"Emitter2D", "texture_filter", "filter"},
    {"Emitter2D", "vframes", "rows"},
    {"Emitter2D", "z_as_relative", "sort_order_relative"},
    {"Emitter2D", "z_index", "sort_order"},
    {"Emitter3D", "explosiveness", "burst"},
    {"Emitter3D", "local_coords", "local_space"},
    {"Environment3D", "sdfgi_bounce_feedback", "field_lighting_bounce_feedback"},
    {"Environment3D", "sdfgi_cascades", "field_lighting_cascades"},
    {"Environment3D", "sdfgi_enabled", "field_lighting_enabled"},
    {"Environment3D", "sdfgi_energy", "field_lighting_energy"},
    {"Environment3D", "sdfgi_min_cell_size", "field_lighting_min_cell_size"},
    {"Environment3D", "sdfgi_normal_bias", "field_lighting_normal_bias"},
    {"Environment3D", "sdfgi_probe_bias", "field_lighting_probe_bias"},
    {"Environment3D", "sdfgi_read_sky_light", "field_lighting_read_sky_light"},
    {"Environment3D", "sdfgi_use_occlusion", "field_lighting_use_occlusion"},
    {"Environment3D", "sdfgi_y_scale", "field_lighting_y_scale"},
    {"Fluid2D", "light_mask", "light_layers"},
    {"Fluid2D", "modulate", "tint"},
    {"Fluid2D", "z_as_relative", "sort_order_relative"},
    {"Fluid2D", "z_index", "sort_order"},
    {"Foliage2D", "light_mask", "light_layers"},
    {"Foliage2D", "modulate", "tint"},
    {"Foliage2D", "texture_filter", "filter"},
    {"Foliage2D", "z_as_relative", "sort_order_relative"},
    {"Foliage2D", "z_index", "sort_order"},
    {"GpuEmitter2D", "collision_base_size", "collision_size"},
    {"GpuEmitter2D", "explosiveness", "burst"},
    {"GpuEmitter2D", "fixed_fps", "fixed_rate"},
    {"GpuEmitter2D", "fract_delta", "smooth_emission"},
    {"GpuEmitter2D", "hframes", "columns"},
    {"GpuEmitter2D", "light_mask", "light_layers"},
    {"GpuEmitter2D", "local_coords", "local_space"},
    {"GpuEmitter2D", "texture_filter", "filter"},
    {"GpuEmitter2D", "vframes", "rows"},
    {"GpuEmitter2D", "z_as_relative", "sort_order_relative"},
    {"GpuEmitter2D", "z_index", "sort_order"},
    {"GpuEmitter3D", "collision_base_size", "collision_size"},
    {"GpuEmitter3D", "draw_pass_1", "mesh_1"},
    {"GpuEmitter3D", "draw_pass_2", "mesh_2"},
    {"GpuEmitter3D", "draw_pass_3", "mesh_3"},
    {"GpuEmitter3D", "draw_pass_4", "mesh_4"},
    {"GpuEmitter3D", "draw_passes", "mesh_count"},
    {"GpuEmitter3D", "explosiveness", "burst"},
    {"GpuEmitter3D", "fixed_fps", "fixed_rate"},
    {"GpuEmitter3D", "fract_delta", "smooth_emission"},
    {"GpuEmitter3D", "local_coords", "local_space"},
    {"Hair2D", "light_mask", "light_layers"},
    {"Hair2D", "modulate", "tint"},
    {"Hair2D", "texture_filter", "filter"},
    {"Hair2D", "z_as_relative", "sort_order_relative"},
    {"Hair2D", "z_index", "sort_order"},
    {"HingeJoint3D", "node_a", "body_a"},
    {"HingeJoint3D", "node_b", "body_b"},
    {"ImageButton", "action_mode", "press_on"},
    {"ImageButton", "button_pressed", "pressed"},
    {"ImageButton", "ignore_texture_size", "ignore_image_size"},
    {"ImageButton", "keep_pressed_outside", "hold_outside"},
    {"ImageButton", "toggle_mode", "toggles"},
    {"Label", "autowrap_mode", "wrap"},
    {"Label", "text_overrun_behavior", "overflow"},
    {"MenuButton", "action_mode", "press_on"},
    {"MenuButton", "autowrap_mode", "wrap"},
    {"MenuButton", "button_pressed", "pressed"},
    {"MenuButton", "keep_pressed_outside", "hold_outside"},
    {"MenuButton", "text_overrun_behavior", "overflow"},
    {"MenuButton", "toggle_mode", "toggles"},
    {"Model3D", "gi_lightmap_texel_scale", "lightmap_texel_scale"},
    {"Model3D", "gi_mode", "lighting_mode"},
    {"Model3D", "ignore_occlusion_culling", "skip_occlusion"},
    {"Model3D", "material_override", "material"},
    {"MovingBody2D", "physics_material_override", "physics_material"},
    {"MovingBody3D", "physics_material_override", "physics_material"},
    {"NineSlice", "axis_stretch_horizontal", "stretch_horizontal"},
    {"NineSlice", "axis_stretch_vertical", "stretch_vertical"},
    {"NineSlice", "draw_center", "fill_center"},
    {"NineSlice", "patch_margin_bottom", "border_bottom"},
    {"NineSlice", "patch_margin_left", "border_left"},
    {"NineSlice", "patch_margin_right", "border_right"},
    {"NineSlice", "patch_margin_top", "border_top"},
    {"NineSlice", "region_rect", "region"},
    {"Occluder2D", "occluder_light_mask", "light_layers"},
    {"ParticleAttractorBox3D", "cull_mask", "layer_filter"},
    {"ParticleAttractorSphere3D", "cull_mask", "layer_filter"},
    {"ParticleColliderBox3D", "cull_mask", "layer_filter"},
    {"ParticleColliderHeight3D", "cull_mask", "layer_filter"},
    {"ParticleColliderSdf3D", "cull_mask", "layer_filter"},
    {"ParticleColliderSphere3D", "cull_mask", "layer_filter"},
    {"ParticleFlowField3D", "cull_mask", "layer_filter"},
    {"PinJoint2D", "node_a", "body_a"},
    {"PinJoint2D", "node_b", "body_b"},
    {"PinJoint3D", "node_a", "body_a"},
    {"PinJoint3D", "node_b", "body_b"},
    {"PointLight2D", "range_item_cull_mask", "lit_layers"},
    {"PointLight2D", "shadow_item_cull_mask", "shadow_layers"},
    {"PointLight3D", "light_cull_mask", "lit_layers"},
    {"Polygon2D", "light_mask", "light_layers"},
    {"Polygon2D", "modulate", "tint"},
    {"Polygon2D", "texture_filter", "filter"},
    {"Polygon2D", "z_as_relative", "sort_order_relative"},
    {"Polygon2D", "z_index", "sort_order"},
    {"Reflection2D", "z_as_relative", "sort_order_relative"},
    {"Reflection2D", "z_index", "sort_order"},
    {"ReflectionProbe", "cull_mask", "layer_filter"},
    {"RigidBody2D", "contact_monitor", "report_contacts"},
    {"RigidBody2D", "freeze", "frozen"},
    {"RigidBody2D", "freeze_mode", "frozen_mode"},
    {"RigidBody2D", "physics_material_override", "physics_material"},
    {"RigidBody3D", "contact_monitor", "report_contacts"},
    {"RigidBody3D", "freeze", "frozen"},
    {"RigidBody3D", "freeze_mode", "frozen_mode"},
    {"RigidBody3D", "physics_material_override", "physics_material"},
    {"SimulatedBone3D", "physics_material_override", "physics_material"},
    {"SixAxisJoint3D", "node_a", "body_a"},
    {"SixAxisJoint3D", "node_b", "body_b"},
    {"SkyGradient", "sky_cover_modulate", "sky_cover_tint"},
    {"SlideJoint3D", "node_a", "body_a"},
    {"SlideJoint3D", "node_b", "body_b"},
    {"SolidExtrusion3D", "path_node", "path"},
    {"SoundPlayer", "pitch_scale", "pitch"},
    {"SoundPlayer2D", "area_mask", "zone_mask"},
    {"SoundPlayer2D", "pitch_scale", "pitch"},
    {"SoundPlayer3D", "area_mask", "zone_mask"},
    {"SoundPlayer3D", "max_db", "max_volume_db"},
    {"SoundPlayer3D", "pitch_scale", "pitch"},
    {"SoundPlayer3D", "unit_size", "reference_distance"},
    {"SpotLight3D", "light_cull_mask", "lit_layers"},
    {"SpringJoint2D", "node_a", "body_a"},
    {"SpringJoint2D", "node_b", "body_b"},
    {"Sprite2D", "hframes", "columns"},
    {"Sprite2D", "light_mask", "light_layers"},
    {"Sprite2D", "modulate", "tint"},
    {"Sprite2D", "texture_filter", "filter"},
    {"Sprite2D", "vframes", "rows"},
    {"Sprite2D", "z_as_relative", "sort_order_relative"},
    {"Sprite2D", "z_index", "sort_order"},
    {"StaticBody2D", "physics_material_override", "physics_material"},
    {"StaticBody3D", "physics_material_override", "physics_material"},
    {"SunLight2D", "range_item_cull_mask", "lit_layers"},
    {"SunLight2D", "shadow_item_cull_mask", "shadow_layers"},
    {"SunLight3D", "light_cull_mask", "lit_layers"},
    {"Surface3D", "billboard_mode", "billboard"},
    {"Surface3D", "shading_mode", "shading"},
    {"Surface3D", "subsurf_scatter_enabled", "subsurface_enabled"},
    {"Surface3D", "subsurf_scatter_skin_mode", "subsurface_skin_mode"},
    {"Surface3D", "subsurf_scatter_strength", "subsurface_strength"},
    {"Surface3D", "subsurf_scatter_transmittance_boost", "subsurface_transmittance_boost"},
    {"Surface3D", "subsurf_scatter_transmittance_color", "subsurface_transmittance_color"},
    {"Surface3D", "subsurf_scatter_transmittance_depth", "subsurface_transmittance_depth"},
    {"Surface3D", "subsurf_scatter_transmittance_enabled", "subsurface_transmittance_enabled"},
    {"Surface3D", "texture_filter", "filter"},
    {"Surface3D", "texture_repeat", "repeat"},
    {"Surface3D", "uv1_offset", "uv_offset"},
    {"Surface3D", "uv1_scale", "uv_scale"},
    {"TextArea", "autowrap_mode", "wrap"},
    {"TileLayer2D", "light_mask", "light_layers"},
    {"TileLayer2D", "modulate", "tint"},
    {"TileLayer2D", "texture_filter", "filter"},
    {"TileLayer2D", "z_as_relative", "sort_order_relative"},
    {"TileLayer2D", "z_index", "sort_order"},
    {"ToggleSwitch", "action_mode", "press_on"},
    {"ToggleSwitch", "autowrap_mode", "wrap"},
    {"ToggleSwitch", "button_pressed", "pressed"},
    {"ToggleSwitch", "keep_pressed_outside", "hold_outside"},
    {"ToggleSwitch", "text_overrun_behavior", "overflow"},
    {"ToggleSwitch", "toggle_mode", "toggles"},
    {"TrackJoint2D", "node_a", "body_a"},
    {"TrackJoint2D", "node_b", "body_b"},
    {"Water2D", "light_mask", "light_layers"},
    {"Water2D", "modulate", "tint"},
    {"Water2D", "z_as_relative", "sort_order_relative"},
    {"Water2D", "z_index", "sort_order"},
    {"Widget", "clip_contents", "clip_children"},
    {"Widget", "custom_minimum_size", "min_size"},
    {"Widget", "focus_neighbor_bottom", "focus_bottom"},
    {"Widget", "focus_neighbor_left", "focus_left"},
    {"Widget", "focus_neighbor_right", "focus_right"},
    {"Widget", "focus_neighbor_top", "focus_top"},
    {"Widget", "modulate", "tint"},
    {"Widget", "mouse_filter", "pointer_mode"},
    {"Widget", "pivot_offset", "pivot"},
    {"Widget", "self_modulate", "own_tint"},
    {"Widget", "size_flags_horizontal", "sizing_horizontal"},
    {"Widget", "size_flags_stretch_ratio", "stretch_ratio"},
    {"Widget", "size_flags_vertical", "sizing_vertical"},
    {"Widget", "texture_filter", "filter"},
    {"Widget", "theme_override_colors", "theme_colors"},
    {"Widget", "theme_override_constants", "theme_constants"},
    {"Widget", "theme_override_font_sizes", "theme_font_sizes"},
    {"Widget", "theme_override_fonts", "theme_fonts"},
    {"Widget", "theme_override_icons", "theme_icons"},
    {"Widget", "theme_override_styles", "theme_styles"},
    {"Widget", "z_as_relative", "sort_order_relative"},
    {"Widget", "z_index", "sort_order"},
    {"Zone2D", "angular_damp_space_override", "angular_damp_mode"},
    {"Zone2D", "audio_bus_override", "bus_override"},
    {"Zone2D", "gravity_space_override", "gravity_mode"},
    {"Zone2D", "linear_damp_space_override", "linear_damp_mode"},
    {"Zone2D", "monitorable", "detectable"},
    {"Zone2D", "monitoring", "detects"},
    {"Zone3D", "angular_damp_space_override", "angular_damp_mode"},
    {"Zone3D", "audio_bus_override", "bus_override"},
    {"Zone3D", "gravity_space_override", "gravity_mode"},
    {"Zone3D", "linear_damp_space_override", "linear_damp_mode"},
    {"Zone3D", "monitorable", "detectable"},
    {"Zone3D", "monitoring", "detects"},
});

// Moves `from`'s value to `to` in an object, unless `to` already holds one, and drops `from`.
void move_key(json::Object& object, const std::string& from, std::string_view to) {
    const auto it = object.find(from);
    if (object.find(to) == object.end()) {
        object.emplace(std::string(to), std::move(it->second));
    }
    object.erase(object.find(from));
}

// Replaces old field names in one component's object of fields.
void upgrade_fields(std::string_view component, json::Value& fields, const std::string& path,
                    std::vector<Renamed>& renamed) {
    if (!fields.is_object()) {
        return;
    }
    json::Object& object = fields.as_object();
    std::vector<std::pair<std::string, std::string_view>> moves;
    for (const auto& [key, unused] : object) {
        if (const std::string_view name = current_field_name(component, key); name != key) {
            moves.emplace_back(key, name);
        }
    }
    for (const auto& [from, to] : moves) {
        std::string at = path;
        at += '/';
        at += from;
        renamed.push_back({std::move(at), from, std::string(to)});
        move_key(object, from, to);
    }
}

} // namespace

std::span<const NameAlias> component_aliases() {
    return kComponents;
}

std::span<const FieldAlias> field_aliases() {
    return kFields;
}

std::string_view current_component_name(std::string_view name) {
    const std::span<const NameAlias> list = kComponents;
    const auto it = std::ranges::lower_bound(list, name, std::less<>{}, &NameAlias::old_name);
    return it != list.end() && it->old_name == name ? it->name : name;
}

std::string_view current_field_name(std::string_view component, std::string_view field) {
    const auto key = [](const FieldAlias& alias) {
        return std::pair{alias.component, alias.old_name};
    };
    const std::span<const FieldAlias> list = kFields;
    const auto it = std::ranges::lower_bound(list, std::pair{component, field}, std::less<>{}, key);
    return it != list.end() && it->component == component && it->old_name == field ? it->name
                                                                                   : field;
}

std::vector<Renamed> upgrade_components(json::Value& components, std::string_view path) {
    std::vector<Renamed> renamed;
    if (!components.is_object()) {
        return renamed;
    }
    json::Object& object = components.as_object();
    std::vector<std::pair<std::string, std::string_view>> moves;
    for (const auto& [key, unused] : object) {
        if (const std::string_view name = current_component_name(key); name != key) {
            moves.emplace_back(key, name);
        }
    }
    for (const auto& [from, to] : moves) {
        renamed.push_back({std::string(path) + "/" + from, from, std::string(to)});
        move_key(object, from, to);
    }
    for (auto& [name, fields] : object) {
        upgrade_fields(name, fields, std::string(path) + "/" + name, renamed);
    }
    return renamed;
}

std::vector<Renamed> upgrade_scene(json::Value& scene) {
    std::vector<Renamed> renamed;
    if (!scene.is_object()) {
        return renamed;
    }
    if (json::Value* components = scene.find("components"); components != nullptr) {
        renamed = upgrade_components(*components, "/components");
    }
    json::Value* entities = scene.find("entities");
    if (entities == nullptr || !entities->is_object()) {
        return renamed;
    }
    for (auto& [id, entry] : entities->as_object()) {
        if (json::Value* components = entry.is_object() ? entry.find("components") : nullptr;
            components != nullptr) {
            std::vector<Renamed> more = upgrade_components(
                *components, "/entities/" + json::pointer_escape(id) + "/components");
            renamed.insert(renamed.end(), std::make_move_iterator(more.begin()),
                           std::make_move_iterator(more.end()));
        }
    }
    return renamed;
}

bool has_old_names(const json::Value& scene) {
    const auto old_in = [](const json::Value* components) {
        if (components == nullptr || !components->is_object()) {
            return false;
        }
        return std::ranges::any_of(components->as_object(), [](const auto& component) {
            const std::string_view name = current_component_name(component.first);
            if (name != component.first) {
                return true;
            }
            return component.second.is_object() &&
                   std::ranges::any_of(component.second.as_object(), [&](const auto& field) {
                       return current_field_name(name, field.first) != field.first;
                   });
        });
    };
    if (!scene.is_object()) {
        return false;
    }
    if (old_in(scene.find("components"))) {
        return true;
    }
    const json::Value* entities = scene.find("entities");
    return entities != nullptr && entities->is_object() &&
           std::ranges::any_of(entities->as_object(), [&](const auto& entry) {
               return entry.second.is_object() && old_in(entry.second.find("components"));
           });
}

const json::Value& current_names(const json::Value& scene, json::Value& copy) {
    if (!has_old_names(scene)) {
        return scene;
    }
    copy = scene;
    upgrade_scene(copy);
    return copy;
}

namespace {

bool is_old_component(const json::Value& value) {
    return value.is_string() && current_component_name(value.as_string()) != value.as_string();
}

bool is_name_list(std::string_view key) {
    return key == "components" || key == "with" || key == "without";
}

} // namespace

namespace {

// Whether one param names a component by an old name: a "components" object's keys, a
// "component" or a list of component names.
bool names_old_component(std::string_view key, const json::Value& value) {
    if (key == "components" && value.is_object()) {
        json::Value wrapped;
        wrapped["components"] = value;
        return has_old_names(wrapped);
    }
    if (key == "component") {
        return is_old_component(value);
    }
    return is_name_list(key) && value.is_array() &&
           std::ranges::any_of(value.as_array(), is_old_component);
}

} // namespace

bool params_have_old_names(const json::Value& params) {
    if (params.is_array()) {
        return std::ranges::any_of(params.as_array(), params_have_old_names);
    }
    if (!params.is_object()) {
        return false;
    }
    return std::ranges::any_of(params.as_object(), [](const auto& entry) {
        return names_old_component(entry.first, entry.second) ||
               params_have_old_names(entry.second);
    });
}

bool upgrade_params(json::Value& params) {
    bool any = false;
    if (params.is_array()) {
        for (json::Value& item : params.as_array()) {
            any = upgrade_params(item) || any;
        }
        return any;
    }
    if (!params.is_object()) {
        return false;
    }
    for (auto& [key, value] : params.as_object()) {
        if (key == "components" && value.is_object()) {
            any = !upgrade_components(value).empty() || any;
        } else if (key == "component" && is_old_component(value)) {
            value = json::Value(std::string(current_component_name(value.as_string())));
            any = true;
        } else if (is_name_list(key) && value.is_array()) {
            for (json::Value& item : value.as_array()) {
                if (is_old_component(item)) {
                    item = json::Value(std::string(current_component_name(item.as_string())));
                    any = true;
                }
            }
        }
        any = upgrade_params(value) || any;
    }
    return any;
}

std::string current_field_path(std::string_view path) {
    const std::size_t slash = path.find('/');
    const std::string_view component = current_component_name(path.substr(0, slash));
    if (slash == std::string_view::npos) {
        return std::string(component);
    }
    const std::string_view rest = path.substr(slash + 1);
    const std::size_t next = rest.find('/');
    std::string out(component);
    out += '/';
    out += current_field_name(component, rest.substr(0, next));
    if (next != std::string_view::npos) {
        out += rest.substr(next);
    }
    return out;
}

namespace {

// Deepest first, so a setting moves before the object holding it does.
constexpr std::array kSettings = std::to_array<SettingAlias>({
    {"rendering/anti_aliasing/quality/use_taa", "rendering/anti_aliasing/taa"},
    {"rendering/camera/depth_of_field/depth_of_field_bokeh_quality",
     "rendering/depth_of_field/bokeh_quality"},
    {"rendering/camera/depth_of_field/depth_of_field_bokeh_shape",
     "rendering/depth_of_field/bokeh_shape"},
    {"rendering/camera/depth_of_field/depth_of_field_use_jitter",
     "rendering/depth_of_field/jitter"},
    {"rendering/global_illumination/voxel_gi/quality", "rendering/voxel_lighting/quality"},
    {"rendering/lights_and_shadows/directional_shadow/size", "rendering/shadows/directional_size"},
    {"rendering/lights_and_shadows/directional_shadow/soft_shadow_filter_quality",
     "rendering/shadows/directional_softness"},
    {"rendering/lights_and_shadows/positional_shadow/atlas_size",
     "rendering/shadows/positional_atlas_size"},
    {"rendering/lights_and_shadows/positional_shadow/soft_shadow_filter_quality",
     "rendering/shadows/positional_softness"},
    {"rendering/mesh_lod/lod_change/threshold_pixels", "rendering/lod/threshold_pixels"},
    {"rendering/reflections/reflection_atlas/reflection_count",
     "rendering/reflections/probe_count"},
    {"rendering/reflections/reflection_atlas/reflection_size", "rendering/reflections/probe_size"},
    {"rendering/reflections/sky_reflections/ggx_samples", "rendering/reflections/sky_samples"},
    {"input_devices/pointing/emulate_mouse_from_touch", "pointer/mouse_from_touch"},
    {"input_devices/pointing/emulate_touch_from_mouse", "pointer/touch_from_mouse"},
    {"internationalization/pseudolocalization/use_pseudolocalization",
     "localization/pseudo/enabled"},
    {"internationalization/rendering/force_right_to_left_layout_direction",
     "localization/force_right_to_left"},
    {"internationalization/rendering/root_node_layout_direction", "localization/layout_direction"},
    {"rendering/environment/screen_space_reflection", "rendering/ssr"},
    {"rendering/environment/ssao", "rendering/ssao"},
    {"rendering/environment/subsurface_scattering", "rendering/subsurface"},
    {"rendering/environment/volumetric_fog", "rendering/volumetric_fog"},
    {"rendering/global_illumination/sdfgi", "rendering/field_lighting"},
    {"rendering/lights_and_shadows/use_physical_light_units", "rendering/physical_light_units"},
    {"rendering/occlusion_culling/occlusion_rays_per_thread",
     "rendering/occlusion_culling/rays_per_thread"},
    {"rendering/occlusion_culling/use_occlusion_culling", "rendering/occlusion_culling/enabled"},
    {"rendering/scaling/fsr_sharpness", "rendering/scaling/sharpness"},
    {"audio/channel_disable_threshold_db", "audio/silence_threshold_db"},
    {"audio/channel_disable_time", "audio/silence_seconds"},
    {"audio/enable_input", "audio/input_enabled"},
    {"gui/allow_system_fallback", "ui/system_font_fallback"},
    {"gui/button_shortcut_feedback_highlight_time", "ui/shortcut_flash_seconds"},
    {"gui/incremental_search_max_interval_msec", "ui/type_search_ms"},
    {"gui/text_edit_idle_detect_sec", "ui/text_edit_merge_seconds"},
    {"gui/text_edit_undo_stack_max_size", "ui/text_edit_undo_limit"},
    {"gui/tooltip_delay_sec", "ui/tooltip_delay_seconds"},
    {"gui/tooltip_position_offset", "ui/tooltip_offset"},
    {"internationalization/locale", "localization"},
    {"internationalization/pseudolocalization", "localization/pseudo"},
    {"navigation_2d/merge_rasterizer_cell_scale", "navigation_2d/merge_cell_scale"},
    {"navigation_2d/use_edge_connections", "navigation_2d/connect_edges"},
    {"navigation_3d/merge_rasterizer_cell_scale", "navigation_3d/merge_cell_scale"},
    {"navigation_3d/use_edge_connections", "navigation_3d/connect_edges"},
    {"physics_3d/areas_detect_static_bodies", "physics_3d/zones_detect_static_bodies"},
    {"rendering/lightmapping", "rendering/lightmaps"},
    {"rendering/rendering_method", "rendering/renderer"},
    {"window/vsync_mode", "window/vsync"},
    {"gui", "ui"},
});

// The next name in a path of names split by '/', taken off its front.
std::string_view next_name(std::string_view& path) {
    const std::size_t slash = path.find('/');
    const std::string_view name = path.substr(0, slash);
    path = slash == std::string_view::npos ? std::string_view{} : path.substr(slash + 1);
    return name;
}

// The value at a path of names, or null; `V` is json::Value or const json::Value.
template <typename V>
V* find_setting(V& root, std::string_view path) {
    V* at = &root;
    while (!path.empty()) {
        if (!at->is_object()) {
            return nullptr;
        }
        at = at->find(std::string(next_name(path)));
        if (at == nullptr) {
            return nullptr;
        }
    }
    return at;
}

// Takes the value at a path out of the objects holding it, dropping those it leaves empty.
json::Value take_setting(json::Value& root, std::string_view path) {
    const std::size_t slash = path.rfind('/');
    if (slash == std::string_view::npos) {
        json::Object& object = root.as_object();
        const auto it = object.find(std::string(path));
        json::Value value = std::move(it->second);
        object.erase(it);
        return value;
    }
    const std::string_view parent_path = path.substr(0, slash);
    // The value was found, so every object on the way is there.
    json::Value* parent = find_setting(root, parent_path);
    json::Object& object = parent->as_object();
    const auto it = object.find(std::string(path.substr(slash + 1)));
    json::Value value = std::move(it->second);
    object.erase(it);
    if (object.empty()) {
        take_setting(root, parent_path);
    }
    return value;
}

// Puts a value at a path, making the objects on the way; where a value is already there, an
// object takes the keys it lacks and anything else is kept.
void place_setting(json::Value& root, std::string_view path, json::Value value) {
    json::Value* at = &root;
    while (true) {
        const std::string name(next_name(path));
        json::Value* next = at->find(name);
        if (path.empty()) {
            if (next == nullptr) {
                (*at)[name] = std::move(value);
            } else if (next->is_object() && value.is_object()) {
                for (auto& [key, item] : value.as_object()) {
                    if (next->find(key) == nullptr) {
                        (*next)[key] = std::move(item);
                    }
                }
            }
            return;
        }
        if (next == nullptr || !next->is_object()) {
            (*at)[name] = json::Object{};
            next = at->find(name);
        }
        at = next;
    }
}

} // namespace

std::span<const SettingAlias> setting_aliases() {
    return kSettings;
}

std::vector<Renamed> upgrade_settings(json::Value& project) {
    std::vector<Renamed> renamed;
    if (!project.is_object()) {
        return renamed;
    }
    for (const SettingAlias& alias : kSettings) {
        if (find_setting(project, alias.old_path) == nullptr) {
            continue;
        }
        json::Value value = take_setting(project, alias.old_path);
        place_setting(project, alias.path, std::move(value));
        std::string at = "/";
        at += alias.old_path;
        renamed.push_back({std::move(at), std::string(alias.old_path), std::string(alias.path)});
    }
    return renamed;
}

bool has_old_settings(const json::Value& project) {
    return project.is_object() && std::ranges::any_of(kSettings, [&](const SettingAlias& alias) {
               return find_setting(project, alias.old_path) != nullptr;
           });
}

const json::Value& current_settings(const json::Value& project, json::Value& copy) {
    if (!has_old_settings(project)) {
        return project;
    }
    copy = project;
    upgrade_settings(copy);
    return copy;
}

} // namespace slash::world
