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
constexpr std::array<FieldAlias, 0> kFields{};

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
    for (const FieldAlias& alias : kFields) {
        if (alias.component == component && alias.old_name == field) {
            return alias.name;
        }
    }
    return field;
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

} // namespace slash::world
