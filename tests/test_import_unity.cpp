#include "slash/canvas/tilemap.hpp"
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/gameplay/host.hpp"
#include "slash/importer/commands.hpp"
#include "slash/importer/import.hpp"
#include "slash/importer/scripts.hpp"
#include "slash/importer/unity_yaml.hpp"
#include "slash/platform/file.hpp"
#include "slash/render/image.hpp"
#include "slash/runtime/gameplay.hpp"
#include "slash/runtime/project.hpp"
#include "slash/scene3d/model.hpp"
#include "slash/scene3d/terrain.hpp"
#include "slash/storyboard/storyboard.hpp"
#include "slash/text/text.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/scene.hpp"
#include "slash/world/world.hpp"

#include <SDL3/SDL_stdinc.h>
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <map>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace json = slash::json;
namespace command = slash::command;
namespace unity = slash::importer::unity;
namespace importer = slash::importer;
namespace canvas = slash::canvas;
namespace fs = std::filesystem;

namespace {

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? std::string() : parsed.error().message));
    return std::move(parsed).value();
}

void write(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    REQUIRE(slash::platform::write_file(path.string(), text).ok());
}

void write_png(const fs::path& path, std::uint32_t width, std::uint32_t height) {
    slash::render::Image image{.width = width, .height = height, .pixels = {}};
    image.pixels.assign(std::size_t{width} * height * 4, 255);
    write(path, slash::render::encode_png(image).value());
}

std::vector<unity::Object> objects_of(std::string_view text) {
    auto parsed = unity::parse_file(text);
    REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? std::string() : parsed.error().message));
    return std::move(parsed).value();
}

// The files a plan writes, by path.
std::map<std::string, std::string> files_of(const importer::ImportPlan& plan) {
    std::map<std::string, std::string> files;
    for (const auto& [path, contents] : plan.files) {
        files[path] = contents;
    }
    return files;
}

// The engine's components, and stand-ins for those of the C++ skeletons a plan writes, as the
// tools have them before the game's code is built.
void add_types(slash::world::ComponentRegistry& components,
               const std::map<std::string, std::string>& files) {
    slash::world::register_builtin_components(components);
    for (const auto& [path, contents] : files) {
        if (!path.ends_with(".cpp")) {
            continue;
        }
        for (slash::runtime::DeclaredComponent& declared : slash::runtime::declared_in(contents)) {
            if (components.find(declared.name) == nullptr) {
                components.add_stand_in(std::move(declared.name), std::move(declared.fields));
            }
        }
    }
}

// The components one entity of a planned scene ends up with, with what it extends and its
// overrides applied; null if the scene has no such entity.
json::Value entity(const std::map<std::string, std::string>& files, const std::string& scene,
                   std::string_view id) {
    REQUIRE_MESSAGE(files.contains(scene), scene);
    slash::world::ComponentRegistry components;
    add_types(components, files);
    slash::world::World world(components);
    world.set_file_reader([&files](std::string_view path) -> slash::Result<std::string> {
        const auto it = files.find(std::string(path));
        if (it == files.end()) {
            return slash::make_error(slash::ErrorCode::NotFound, "no such file");
        }
        return it->second;
    });
    const auto loaded = slash::world::load_scene(world, parse(files.at(scene)));
    REQUIRE_MESSAGE(loaded.ok(), (loaded.ok() ? std::string() : loaded.error().message));
    const entt::entity found = world.find(id);
    if (!world.valid(found)) {
        return nullptr;
    }
    json::Value result = json::Object{};
    for (const auto& type : components.types()) {
        if (type->has(world, found)) {
            result[type->name()] = type->save(world, found);
        }
    }
    return result;
}

// The report's unsupported entry for what, or null.
json::Value unsupported(const json::Value& report, std::string_view what) {
    for (const json::Value& item : report.find("unsupported")->as_array()) {
        if (item.find("what")->as_string() == what) {
            return item;
        }
    }
    return nullptr;
}

// --- Unity's files, written as Unity writes them ---

constexpr std::string_view kHeader = "%YAML 1.1\n%TAG !u! tag:unity3d.com,2011:\n";
constexpr std::string_view kSource = "  m_ObjectHideFlags: 0\n"
                                     "  m_CorrespondingSourceObject: {fileID: 0}\n"
                                     "  m_PrefabInstance: {fileID: 0}\n"
                                     "  m_PrefabAsset: {fileID: 0}\n";
constexpr std::int64_t kMask = 0x7FFFFFFFFFFFFFFF;

std::string id(std::int64_t value) {
    return std::to_string(value);
}

// The ID of an object an instance brings: (instance ^ its ID in the prefab) & kMask.
std::int64_t brought(std::int64_t instance, std::int64_t source) {
    return (instance ^ source) & kMask;
}

std::string ref(std::int64_t file_id, std::string_view guid = {}, int type = 3) {
    if (guid.empty()) {
        return "{fileID: " + id(file_id) + "}";
    }
    return "{fileID: " + id(file_id) + ", guid: " + std::string(guid) +
           ", type: " + std::to_string(type) + "}";
}

std::string game_object(std::int64_t object, std::string_view name,
                        std::initializer_list<std::int64_t> components, int layer = 0,
                        std::string_view more = "  m_TagString: Untagged\n  m_IsActive: 1\n") {
    std::string out = "--- !u!1 &" + id(object) + "\nGameObject:\n" + std::string(kSource) +
                      "  serializedVersion: 6\n  m_Component:\n";
    for (const std::int64_t component : components) {
        out += "  - component: {fileID: " + id(component) + "}\n";
    }
    return out + "  m_Layer: " + std::to_string(layer) + "\n  m_Name: " + std::string(name) + "\n" +
           std::string(more);
}

std::string transform(std::int64_t object, std::int64_t owner, std::string_view position,
                      std::int64_t father = 0, std::initializer_list<std::int64_t> children = {},
                      std::string_view more = "") {
    std::string out = "--- !u!4 &" + id(object) + "\nTransform:\n" + std::string(kSource) +
                      "  m_GameObject: {fileID: " + id(owner) + "}\n" +
                      "  m_LocalRotation: {x: 0, y: 0, z: 0, w: 1}\n" +
                      "  m_LocalPosition: " + std::string(position) + "\n" +
                      "  m_LocalScale: {x: 1, y: 1, z: 1}\n" + std::string(more);
    if (children.size() == 0) {
        out += "  m_Children: []\n";
    } else {
        out += "  m_Children:\n";
        for (const std::int64_t child : children) {
            out += "  - {fileID: " + id(child) + "}\n";
        }
    }
    return out + "  m_Father: {fileID: " + id(father) + "}\n";
}

std::string component(int class_id, std::string_view type, std::int64_t object, std::int64_t owner,
                      std::string_view fields) {
    return "--- !u!" + std::to_string(class_id) + " &" + id(object) + "\n" + std::string(type) +
           ":\n" + std::string(kSource) + "  m_GameObject: {fileID: " + id(owner) + "}\n" +
           "  m_Enabled: 1\n" + std::string(fields);
}

std::string modification(std::int64_t target, std::string_view guid, std::string_view path,
                         std::string_view value, std::string_view reference = "{fileID: 0}") {
    return "    - target: {fileID: " + id(target) + ", guid: " + std::string(guid) +
           ",\n        type: 3}\n      propertyPath: " + std::string(path) +
           "\n      value: " + std::string(value) +
           "\n      objectReference: " + std::string(reference) + "\n";
}

std::string instance(std::int64_t object, std::string_view guid, std::int64_t parent,
                     std::string_view modifications, std::string_view removed_components = " []",
                     std::string_view removed_objects = " []") {
    return "--- !u!1001 &" + id(object) +
           "\nPrefabInstance:\n  m_ObjectHideFlags: 0\n  serializedVersion: 2\n"
           "  m_Modification:\n    serializedVersion: 3\n    m_TransformParent: {fileID: " +
           id(parent) + "}\n    m_Modifications:\n" + std::string(modifications) +
           "    m_RemovedComponents:" + std::string(removed_components) +
           "\n    m_RemovedGameObjects:" + std::string(removed_objects) +
           "\n    m_AddedGameObjects: []\n    m_AddedComponents: []\n"
           "  m_SourcePrefab: {fileID: 100100000, guid: " +
           std::string(guid) + ", type: 3}\n";
}

std::string stripped(int class_id, std::string_view type, std::int64_t object, std::int64_t source,
                     std::string_view guid, std::int64_t instance_id) {
    return "--- !u!" + std::to_string(class_id) + " &" + id(object) + " stripped\n" +
           std::string(type) + ":\n  m_CorrespondingSourceObject: {fileID: " + id(source) +
           ", guid: " + std::string(guid) +
           ",\n    type: 3}\n  m_PrefabInstance: {fileID: " + id(instance_id) +
           "}\n  m_PrefabAsset: {fileID: 0}\n";
}

std::string sprite_renderer(std::int64_t object, std::int64_t owner, std::string_view sprite,
                            int order = 0, int flip_x = 0) {
    return component(
        212, "SpriteRenderer", object, owner,
        "  m_Materials:\n  - {fileID: 10754, guid: "
        "0000000000000000f000000000000000, type: 0}\n"
        "  m_SortingLayerID: 0\n  m_SortingLayer: 0\n  m_SortingOrder: " +
            std::to_string(order) + "\n  m_Sprite: " + std::string(sprite) +
            "\n  m_Color: {r: 1, g: 1, b: 1, a: 1}\n  m_FlipX: " + std::to_string(flip_x) +
            "\n  m_FlipY: 0\n  m_DrawMode: 0\n  m_MaskInteraction: 0\n");
}

std::string meta(std::string_view guid, std::string_view importer = "DefaultImporter",
                 std::string_view more = "") {
    return "fileFormatVersion: 2\nguid: " + std::string(guid) + "\n" + std::string(importer) +
           ":\n  externalObjects: {}\n" + std::string(more) + "  userData: \n";
}

std::string texture_meta(std::string_view guid, int ppu, std::string_view more) {
    return meta(guid, "TextureImporter",
                "  textureSettings:\n    serializedVersion: 2\n    filterMode: 0\n"
                "  textureType: 8\n  spritePixelsToUnits: " +
                    std::to_string(ppu) + "\n" + std::string(more));
}

constexpr std::string_view kHeroPng = "a1000000000000000000000000000001";
constexpr std::string_view kSheetPng = "a1000000000000000000000000000002";
constexpr std::string_view kHitOgg = "a1000000000000000000000000000003";
constexpr std::string_view kCrateCs = "a1000000000000000000000000000004";
constexpr std::string_view kCratePrefab = "a1000000000000000000000000000005";
constexpr std::string_view kStackPrefab = "a1000000000000000000000000000006";
constexpr std::string_view kBigCratePrefab = "a1000000000000000000000000000007";
constexpr std::string_view kBouncy = "a1000000000000000000000000000008";
constexpr std::string_view kImageCs = "fe87c0e1cc204ed48ad3b37840f39efc"; // uGUI's Image

// "Cave Run": project settings, sprites, a sound and a scene with a camera, bodies, colliders,
// a sound and things that don't convert, saved by Unity 2021.3 (no SceneRoots; m_RootOrder).
void write_settings(const fs::path& folder) {
    write(folder / "ProjectSettings/ProjectVersion.txt", "m_EditorVersion: 2021.3.5f1\n");
    write(folder / "ProjectSettings/ProjectSettings.asset",
          std::string(kHeader) +
              "--- !u!129 &1\nPlayerSettings:\n  m_ObjectHideFlags: 0\n  companyName: Test\n"
              "  productName: Cave Run\n  defaultScreenWidth: 800\n  defaultScreenHeight: 600\n"
              "  bundleVersion: 0.3\n  activeInputHandler: 0\n");
    // Layer 8, Enemies, doesn't collide with itself.
    std::string matrix;
    for (int layer = 0; layer < 32; ++layer) {
        matrix += layer == 8 ? "fffeffff" : "ffffffff";
    }
    write(folder / "ProjectSettings/Physics2DSettings.asset",
          std::string(kHeader) +
              "--- !u!19 &1\nPhysics2DSettings:\n  m_ObjectHideFlags: 0\n"
              "  m_Gravity: {x: 0, y: -20}\n  m_DefaultMaterial: {fileID: 0}\n"
              "  m_LayerCollisionMatrix: " +
              matrix + "\n");
    std::string layers = "  - Default\n  - TransparentFX\n  - Ignore Raycast\n  - \n  - Water\n"
                         "  - UI\n  - \n  - \n  - Enemies\n";
    for (int i = 9; i < 32; ++i) {
        layers += "  - \n";
    }
    write(folder / "ProjectSettings/TagManager.asset",
          std::string(kHeader) +
              "--- !u!78 &1\nTagManager:\n  serializedVersion: 2\n  tags: []\n"
              "  layers:\n" +
              layers + "  m_SortingLayers:\n  - name: Default\n    uniqueID: 0\n    locked: 0\n");
    // Unity 2022's fixed timestep: a count of ticks at a rate, here 1/50 s.
    write(folder / "ProjectSettings/TimeManager.asset",
          std::string(kHeader) +
              "--- !u!5 &1\nTimeManager:\n  m_ObjectHideFlags: 0\n  Fixed Timestep:\n"
              "    m_Count: 2822399\n    m_Rate:\n      m_Denominator: 1\n"
              "      m_Numerator: 141120000\n  m_TimeScale: 1\n");
    const auto axis = [](std::string_view name, std::string_view negative,
                         std::string_view positive, std::string_view alt_positive, int type,
                         int axis_index, int invert) {
        return "  - serializedVersion: 3\n    m_Name: " + std::string(name) +
               "\n    descriptiveName: \n    negativeButton: " + std::string(negative) +
               "\n    positiveButton: " + std::string(positive) +
               "\n    altNegativeButton: \n    altPositiveButton: " + std::string(alt_positive) +
               "\n    gravity: 3\n    dead: 0.001\n    sensitivity: 3\n    snap: 0\n"
               "    invert: " +
               std::to_string(invert) + "\n    type: " + std::to_string(type) +
               "\n    axis: " + std::to_string(axis_index) + "\n    joyNum: 0\n";
    };
    write(folder / "ProjectSettings/InputManager.asset",
          std::string(kHeader) +
              "--- !u!13 &1\nInputManager:\n  m_ObjectHideFlags: 0\n"
              "  serializedVersion: 2\n  m_Axes:\n" +
              axis("Fire1", "", "left ctrl", "mouse 0", 0, 0, 0) +
              axis("Vertical", "down", "up", "", 0, 0, 0) + axis("Vertical", "", "", "", 2, 1, 1) +
              axis("Dash", "", "joystick 2 button 4", "'[5]'", 0, 0, 0) +
              axis("Odd", "", "joystick 9 axis", "", 0, 0, 0));
    // An Input System actions file, which a project using only the Input Manager still converts.
    write(folder / "Assets/Controls.inputactions", R"({
    "name": "Controls",
    "maps": [
        {
            "name": "Ship",
            "actions": [
                {"name": "Thrust", "type": "Value", "expectedControlType": "Axis"},
                {"name": "Aim", "type": "Value", "expectedControlType": "Vector2"},
                {"name": "Fire", "type": "Button", "expectedControlType": "Button"}
            ],
            "bindings": [
                {"name": "1D Axis", "path": "1DAxis", "action": "Thrust", "isComposite": true,
                 "isPartOfComposite": false},
                {"name": "negative", "path": "<Keyboard>/s", "action": "Thrust",
                 "isComposite": false, "isPartOfComposite": true},
                {"name": "positive", "path": "<Keyboard>/w", "action": "Thrust",
                 "isComposite": false, "isPartOfComposite": true},
                {"name": "", "path": "<Gamepad>/rightStick", "action": "Aim",
                 "isComposite": false, "isPartOfComposite": false},
                {"name": "", "path": "<Mouse>/leftButton", "action": "Fire",
                 "isComposite": false, "isPartOfComposite": false},
                {"name": "", "path": "<Keyboard>/numpad0", "action": "Fire",
                 "interactions": "Tap", "isComposite": false, "isPartOfComposite": false},
                {"name": "", "path": "<Mouse>/delta", "action": "Fire",
                 "isComposite": false, "isPartOfComposite": false}
            ]
        }
    ]
}
)");
    write(folder / "Assets/Controls.inputactions.meta",
          meta("a1000000000000000000000000000009", "ScriptedImporter"));
}

void write_assets(const fs::path& folder) {
    write_png(folder / "Assets/Art/hero.png", 16, 24);
    write(folder / "Assets/Art/hero.png.meta",
          texture_meta(kHeroPng, 16, "  spriteMode: 1\n  alignment: 0\n"));
    // A sheet of two sprites, saved before Unity 2019.3 gave sprites internal IDs.
    write_png(folder / "Assets/Art/sheet.png", 32, 32);
    write(folder / "Assets/Art/sheet.png.meta",
          meta(kSheetPng, "TextureImporter",
               "  fileIDToRecycleName:\n    21300000: sheet_0\n    21300002: sheet_1\n"
               "  textureSettings:\n    filterMode: 1\n  textureType: 8\n  spriteMode: 2\n"
               "  spritePixelsToUnits: 32\n  spriteSheet:\n    serializedVersion: 2\n"
               "    sprites:\n    - serializedVersion: 2\n      name: sheet_0\n      rect:\n"
               "        serializedVersion: 2\n        x: 0\n        y: 16\n        width: 16\n"
               "        height: 16\n      alignment: 7\n      pivot: {x: 0.5, y: 0}\n"
               "    - serializedVersion: 2\n      name: sheet_1\n      rect:\n"
               "        serializedVersion: 2\n        x: 16\n        y: 0\n        width: 16\n"
               "        height: 8\n      alignment: 9\n      pivot: {x: 0, y: 1}\n"));
    write(folder / "Assets/Audio/hit.ogg", "OggS");
    write(folder / "Assets/Audio/hit.ogg.meta", meta(kHitOgg, "AudioImporter"));
    write(folder / "Assets/Bouncy.physicsMaterial2D",
          std::string(kHeader) + "--- !u!62 &6200000\nPhysicsMaterial2D:\n" + std::string(kSource) +
              "  m_Name: Bouncy\n  friction: 0.2\n  bounciness: 1.5\n");
    write(folder / "Assets/Bouncy.physicsMaterial2D.meta",
          meta(kBouncy, "NativeFormatImporter", "  mainObjectFileID: 6200000\n"));
    write(folder / "Assets/Art/logo.psd", "8BPS");
    write(folder / "Assets/Art/logo.psd.meta",
          meta("a100000000000000000000000000000a", "TextureImporter"));
    write(folder / "Assets/Editor/Tool.cs", "class Tool {}\n");
}

constexpr std::string_view kCollider = "  m_Density: 1\n  m_Material: {fileID: 0}\n"
                                       "  m_IsTrigger: 0\n  m_UsedByEffector: 0\n"
                                       "  m_UsedByComposite: 0\n  m_Offset: {x: 0, y: 0}\n";

void write_cave(const fs::path& folder) {
    std::string scene(kHeader);
    scene += "--- !u!104 &2\nRenderSettings:\n  m_ObjectHideFlags: 0\n  m_Fog: 0\n";
    // The camera: an orthographic size of 3.75 units at 16 pixels per unit shows 120 pixels of
    // the 600-pixel window's height, a zoom of 5.
    scene += game_object(10, "Main Camera", {11, 12, 13}, 0,
                         "  m_TagString: MainCamera\n  m_IsActive: 1\n");
    scene += transform(11, 10, "{x: 1, y: 2, z: -10}", 0, {}, "  m_RootOrder: 0\n");
    scene += component(20, "Camera", 12, 10,
                       "  m_BackGroundColor: {r: 0.1, g: 0.1, b: 0.2, a: 0}\n"
                       "  m_NormalizedViewPortRect:\n    serializedVersion: 2\n    x: 0\n"
                       "    y: 0\n    width: 1\n    height: 1\n  orthographic: 1\n"
                       "  orthographic size: 3.75\n  m_TargetTexture: {fileID: 0}\n");
    scene += component(81, "AudioListener", 13, 10, "");
    // The hero: turned a quarter turn counterclockwise, scaled, flipped, kinematic.
    scene += game_object(20, "Hero", {21, 22, 23, 24});
    scene += "--- !u!4 &21\nTransform:\n" + std::string(kSource) +
             "  m_GameObject: {fileID: 20}\n"
             "  m_LocalRotation: {x: 0, y: 0, z: 0.7071068, w: 0.7071068}\n"
             "  m_LocalPosition: {x: 1.5, y: -2, z: 0}\n  m_LocalScale: {x: 2, y: 1, z: 1}\n"
             "  m_Children: []\n  m_Father: {fileID: 0}\n  m_RootOrder: 1\n";
    scene += sprite_renderer(22, 20, ref(21300000, kHeroPng), 9000, 1);
    scene +=
        component(50, "Rigidbody2D", 23, 20, "  m_BodyType: 1\n  m_Simulated: 1\n  m_Mass: 1\n");
    scene += component(61, "BoxCollider2D", 24, 20,
                       "  m_Density: 1\n  m_Material: " + ref(6200000, kBouncy, 2) +
                           "\n  m_IsTrigger: 0\n  m_UsedByEffector: 0\n  m_Offset: {x: 0, y: "
                           "0.25}\n  m_Size: {x: 1, y: 1.5}\n  m_EdgeRadius: 0\n");
    // The bat: a dynamic body with a trigger, on the Enemies layer, with a wing under it.
    scene += game_object(30, "Bat", {31, 32, 33, 34, 35}, 8);
    scene += transform(31, 30, "{x: -3, y: 1, z: 0}", 0, {41}, "  m_RootOrder: 2\n");
    scene += sprite_renderer(32, 30, ref(21300000, kSheetPng));
    scene += component(50, "Rigidbody2D", 33, 30,
                       "  m_BodyType: 0\n  m_Simulated: 1\n  m_UseAutoMass: 0\n  m_Mass: 2\n"
                       "  m_LinearDrag: 0.5\n  m_AngularDrag: 0.05\n  m_GravityScale: 0\n"
                       "  m_SleepingMode: 0\n  m_CollisionDetection: 0\n  m_Constraints: 3\n");
    scene +=
        component(58, "CircleCollider2D", 34, 30, std::string(kCollider) + "  m_Radius: 0.5\n");
    scene += component(58, "CircleCollider2D", 35, 30,
                       "  m_Density: 1\n  m_Material: {fileID: 0}\n  m_IsTrigger: 1\n"
                       "  m_UsedByEffector: 0\n  m_Offset: {x: 0, y: 0}\n  m_Radius: 1\n");
    scene += game_object(40, "Wing", {41, 42, 43}, 8);
    scene += transform(41, 40, "{x: 0.5, y: 0, z: 0}", 31);
    scene += sprite_renderer(42, 40, ref(21300002, kSheetPng));
    scene += component(60, "PolygonCollider2D", 43, 40,
                       std::string(kCollider) + "  m_Points:\n    m_Paths:\n    - - {x: 0, y: 0}\n"
                                                "      - {x: 1, y: 0}\n      - {x: 0, y: 0.5}\n");
    // Spikes: an edge of two segments, with no Rigidbody2D, so static.
    scene += game_object(50, "Spikes", {51, 52});
    scene += transform(51, 50, "{x: 0, y: -3, z: 0}", 0, {}, "  m_RootOrder: 3\n");
    scene += component(68, "EdgeCollider2D", 52, 50,
                       std::string(kCollider) + "  m_EdgeRadius: 0\n  m_Points:\n"
                                                "  - {x: 0, y: 0}\n  - {x: 1, y: 0}\n"
                                                "  - {x: 2, y: 0.5}\n");
    // A torch, switched off, with a looping sound.
    scene += game_object(60, "Torch", {61, 62}, 0, "  m_TagString: Untagged\n  m_IsActive: 0\n");
    scene += transform(61, 60, "{x: 0, y: 0, z: 0}", 0, {}, "  m_RootOrder: 4\n");
    scene +=
        component(82, "AudioSource", 62, 60,
                  "  OutputAudioMixerGroup: {fileID: 0}\n  m_audioClip: " + ref(8300000, kHitOgg) +
                      "\n  m_PlayOnAwake: 1\n  m_Volume: 0.25\n  m_Pitch: 1.5\n  Loop: 1\n"
                      "  Mute: 0\n  MaxDistance: 500\n  rolloffMode: 0\n"
                      "  panLevelCustomCurve:\n    serializedVersion: 2\n    m_Curve:\n"
                      "    - serializedVersion: 3\n      time: 0\n      value: 0\n");
    // Two rocks of the same name, the second written first; m_RootOrder says which is which.
    scene += game_object(80, "Rock", {81});
    scene += transform(81, 80, "{x: 9, y: 0, z: 0}", 0, {}, "  m_RootOrder: 7\n");
    scene += game_object(70, "Rock", {71});
    scene += transform(71, 70, "{x: 8, y: 0, z: 0}", 0, {}, "  m_RootOrder: 6\n");
    // What doesn't convert: a 3D light, a uGUI Image and a package's script.
    scene += game_object(90, "Lamp", {91, 92, 93, 94});
    scene += transform(91, 90, "{x: 0, y: 0, z: 0}", 0, {}, "  m_RootOrder: 5\n");
    scene += component(108, "Light", 92, 90, "  m_Type: 2\n");
    scene += component(114, "MonoBehaviour", 93, 90,
                       "  m_Script: " + ref(11500000, kImageCs) +
                           "\n  m_Name: \n"
                           "  m_EditorClassIdentifier: \n");
    scene += component(114, "MonoBehaviour", 94, 90,
                       "  m_Script: {fileID: 11500000, guid: 5b3f9f1e2c8d4a7b9e0f1a2b3c4d5e6f, "
                       "type: 3}\n  m_Name: \n  m_EditorClassIdentifier: "
                       "Unity.Cinemachine::Unity.Cinemachine.CinemachineCamera\n");
    write(folder / "Assets/Scenes/Cave.unity", scene);
    write(folder / "Assets/Scenes/Cave.unity.meta", meta("a100000000000000000000000000000b"));
}

// Prefabs: a crate with a label and a script; a stack whose crate is an instance of the crate
// prefab; a big crate, a variant of the crate; and a yard scene that instances the stack,
// overriding, removing and adding to what it brings.
void write_prefabs(const fs::path& folder) {
    std::string crate(kHeader);
    crate += game_object(100, "Crate", {101, 102, 103, 104});
    crate += transform(101, 100, "{x: 0, y: 0, z: 0}", 0, {111});
    crate += sprite_renderer(102, 100, ref(21300000, kHeroPng));
    crate += component(61, "BoxCollider2D", 103, 100,
                       std::string(kCollider) + "  m_Size: {x: 1, y: 1}\n");
    crate += component(114, "MonoBehaviour", 104, 100,
                       "  m_Script: " + ref(11500000, kCrateCs) +
                           "\n  m_Name: \n  m_EditorClassIdentifier: \n  health: 3\n"
                           "  label: {fileID: 110}\n  body: {fileID: 103}\n  sound: " +
                           ref(8300000, kHitOgg) +
                           "\n  tint: {r: 1, g: 0.5, b: 0, a: 1}\n  loot:\n  - coin\n  - gem\n"
                           "  title: 'Crate: wooden'\n");
    crate += game_object(110, "Label", {111});
    crate += transform(111, 110, "{x: 0, y: 1, z: 0}", 101);
    write(folder / "Assets/Prefabs/Crate.prefab", crate);
    write(folder / "Assets/Prefabs/Crate.prefab.meta", meta(kCratePrefab, "PrefabImporter"));
    write(
        folder / "Assets/Scripts/Crate.cs",
        "using UnityEngine;\n\npublic class Crate : MonoBehaviour\n{\n    public int health = 3;\n"
        "    [SerializeField] private Transform label;\n    private int hits;\n\n"
        "    public void Hit(int amount)\n    {\n        health -= amount;\n    }\n}\n");
    write(folder / "Assets/Scripts/Crate.cs.meta", meta(kCrateCs, "MonoImporter"));

    std::string stack(kHeader);
    stack += game_object(200, "Stack", {201});
    stack += transform(201, 200, "{x: 0, y: 0, z: 0}", 0, {brought(300, 101)});
    stack += instance(300, kCratePrefab, 201,
                      modification(100, kCratePrefab, "m_Name", "Crate") +
                          modification(101, kCratePrefab, "m_LocalPosition.y", "1.5"));
    stack += stripped(4, "Transform", brought(300, 101), 101, kCratePrefab, 300);
    write(folder / "Assets/Prefabs/Stack.prefab", stack);
    write(folder / "Assets/Prefabs/Stack.prefab.meta", meta(kStackPrefab, "PrefabImporter"));

    std::string big(kHeader);
    big += instance(500, kCratePrefab, 0,
                    modification(100, kCratePrefab, "m_Name", "Big Crate") +
                        modification(101, kCratePrefab, "m_LocalScale.x", "2") +
                        modification(101, kCratePrefab, "m_LocalScale.y", "2") +
                        modification(104, kCratePrefab, "health", "10"));
    big += stripped(1, "GameObject", brought(500, 100), 100, kCratePrefab, 500);
    big += stripped(4, "Transform", brought(500, 101), 101, kCratePrefab, 500);
    write(folder / "Assets/Prefabs/Big Crate.prefab", big);
    write(folder / "Assets/Prefabs/Big Crate.prefab.meta", meta(kBigCratePrefab, "PrefabImporter"));

    // The yard: the stack moved, its crate tinted and without its collider or label, a flag
    // added under it, and a big crate. Unity 2022's SceneRoots name the roots.
    const std::int64_t crate_sprite = brought(300, 102);
    std::string yard(kHeader);
    yard += instance(1000, kStackPrefab, 0,
                     modification(201, kStackPrefab, "m_LocalPosition.x", "2") +
                         modification(crate_sprite, kStackPrefab, "m_Color.r", "0.5"),
                     "\n    - " + ref(brought(300, 103), kStackPrefab),
                     "\n    - " + ref(brought(300, 110), kStackPrefab));
    yard += stripped(4, "Transform", 1001, 201, kStackPrefab, 1000);
    yard += game_object(1100, "Flag", {1101, 1102});
    yard += transform(1101, 1100, "{x: 0, y: 3, z: 0}", 1001);
    yard += component(114, "MonoBehaviour", 1102, 1100,
                      "  m_Script: " + ref(11500000, kCrateCs) +
                          "\n  m_Name: \n  m_EditorClassIdentifier: \n  health: 1\n"
                          "  label: {fileID: 1001}\n  body: {fileID: 0}\n");
    yard += instance(1200, kBigCratePrefab, 0,
                     modification(brought(500, 101), kBigCratePrefab, "m_LocalPosition.x", "-4"));
    yard += stripped(4, "Transform", 1201, brought(500, 101), kBigCratePrefab, 1200);
    yard += "--- !u!1660057539 &9223372036854775807\nSceneRoots:\n  m_ObjectHideFlags: 0\n"
            "  m_Roots:\n  - {fileID: 1200}\n  - {fileID: 1000}\n";
    write(folder / "Assets/Scenes/Yard.unity", yard);
    write(folder / "Assets/Scenes/Yard.unity.meta", meta("a100000000000000000000000000000c"));
}

constexpr std::string_view kTilesPng = "a100000000000000000000000000000d";
constexpr std::string_view kTilesNormalPng = "a1000000000000000000000000000d01";
constexpr std::string_view kGroundTile = "a100000000000000000000000000000e";
constexpr std::string_view kSlopeTile = "a100000000000000000000000000000f";
constexpr std::string_view kRuleTile = "a1000000000000000000000000000010";

// A tile asset: Unity's own Tile, or a scripted tile of a package.
void write_tile(const fs::path& path, std::string_view guid, std::string_view script,
                std::int64_t sprite) {
    write(path,
          std::string(kHeader) + "--- !u!114 &11400000\nMonoBehaviour:\n" + std::string(kSource) +
              "  m_GameObject: {fileID: 0}\n  m_Enabled: 1\n  m_Script: " + std::string(script) +
              "\n  m_Name: " + path.stem().string() + "\n  m_Sprite: " + ref(sprite, kTilesPng) +
              "\n  m_ColliderType: 1\n");
    write(path.string() + ".meta",
          meta(guid, "NativeFormatImporter", "  mainObjectFileID: 11400000\n"));
}

// One of a Tilemap's cells: indexes into its arrays of tile assets, sprites, matrices and
// colors, and flags whose top two bits are the collider type.
std::string tile_cell(int x, int y, int asset, int sprite, int matrix, int color,
                      std::int64_t flags) {
    return "  - first: {x: " + std::to_string(x) + ", y: " + std::to_string(y) +
           ", z: 0}\n    second:\n      serializedVersion: 2\n      m_TileIndex: " +
           std::to_string(asset) + "\n      m_TileSpriteIndex: " + std::to_string(sprite) +
           "\n      m_TileMatrixIndex: " + std::to_string(matrix) +
           "\n      m_TileColorIndex: " + std::to_string(color) +
           "\n      m_TileObjectToInstantiateIndex: 65535\n      dummyAlignment: 0\n"
           "      m_AllTileFlags: " +
           id(flags) + "\n";
}

std::string tile_matrix(int e00, int e01, int e10, int e11) {
    return "  - m_RefCount: 1\n    m_Data:\n      e00: " + std::to_string(e00) +
           "\n      e01: " + std::to_string(e01) +
           "\n      e02: 0\n      e03: 0\n      e10: " + std::to_string(e10) +
           "\n      e11: " + std::to_string(e11) +
           "\n      e12: 0\n      e13: 0\n      e20: 0\n      e21: 0\n      e22: 1\n"
           "      e23: 0\n      e30: 0\n      e31: 0\n      e32: 0\n      e33: 1\n";
}

// "Level": a sheet of three tiles (a block, a slope with a physics shape and a plank), tile
// assets, and a scene with a rectangular tile map on a Grid.
void write_tiles(const fs::path& folder) {
    write_png(folder / "Assets/Art/tiles.png", 48, 16);
    const auto sprite = [](std::string_view name, int x, std::string_view shape, int id) {
        return "    - serializedVersion: 2\n      name: " + std::string(name) +
               "\n      rect:\n        serializedVersion: 2\n        x: " + std::to_string(x) +
               "\n        y: 0\n        width: 16\n        height: 16\n      alignment: 0\n"
               "      pivot: {x: 0.5, y: 0.5}\n      physicsShape:" +
               std::string(shape) + "\n      internalID: " + std::to_string(id) + "\n";
    };
    write(folder / "Assets/Art/tiles.png.meta",
          texture_meta(kTilesPng, 16,
                       "  spriteMode: 2\n  spriteGenerateFallbackPhysicsShape: 0\n"
                       "  spriteSheet:\n    serializedVersion: 2\n    sprites:\n" +
                           sprite("Ground", 0, " []", 1001) +
                           // Unity's physics shapes are in pixels around the sprite's middle,
                           // with y up: a slope rising to the right.
                           sprite("Slope", 16,
                                  "\n      - - {x: -8, y: -8}\n        - {x: 8, y: -8}\n"
                                  "        - {x: 8, y: 8}",
                                  1002) +
                           sprite("Plank", 32, " []", 1003) +
                           // The sheet's normal map, for 2D lights.
                           "    secondaryTextures:\n    - texture: " +
                           ref(2800000, kTilesNormalPng) + "\n      name: _NormalMap\n"));
    write_png(folder / "Assets/Art/tiles_n.png", 48, 16);
    write(folder / "Assets/Art/tiles_n.png.meta", texture_meta(kTilesNormalPng, 16, ""));
    constexpr std::string_view kTileScript =
        "{fileID: 13312, guid: 0000000000000000e000000000000000, type: 0}";
    write_tile(folder / "Assets/Tiles/Ground.asset", kGroundTile, kTileScript, 1001);
    write_tile(folder / "Assets/Tiles/Slope.asset", kSlopeTile, kTileScript, 1002);
    write_tile(folder / "Assets/Tiles/Rule.asset", kRuleTile,
               "{fileID: 11500000, guid: 9d1514134bc4fbd41bb739b1b9a49231, type: 3}", 1003);

    // Collider types: none, the sprite's physics shape, the cell.
    constexpr std::int64_t kNone = 1;
    constexpr std::int64_t kSprite = (std::int64_t{1} << 30) + 1;
    constexpr std::int64_t kCell = (std::int64_t{2} << 30) + 1;
    constexpr std::string_view kGrid = "  m_CellSize: {x: 1, y: 1, z: 0}\n"
                                       "  m_CellGap: {x: 0, y: 0, z: 0}\n";
    std::string scene(kHeader);
    scene += game_object(3000, "Grid", {3001, 3002});
    scene += transform(3001, 3000, "{x: 0, y: 0, z: 0}", 0, {3101});
    scene += component(156049354, "Grid", 3002, 3000,
                       std::string(kGrid) + "  m_CellLayout: 0\n  m_CellSwizzle: 0\n");
    scene += game_object(3100, "Ground", {3101, 3102, 3103, 3104});
    scene += transform(3101, 3100, "{x: 0, y: 0, z: 0}", 3001);
    // Unity's cells go up from y 0, each an index into the arrays below them.
    scene += component(
        1839735485, "Tilemap", 3102, 3100,
        "  m_Tiles:\n" + tile_cell(0, 0, 0, 0, 0, 0, kCell) + // a block
            tile_cell(1, 0, 0, 0, 0, 1, kCell) +              // tinted
            tile_cell(2, 0, 1, 1, 0, 0, kSprite) +            // a slope
            tile_cell(-1, 0, 1, 1, 1, 0, kSprite) +           // flipped
            tile_cell(2, 1, 1, 1, 2, 0, kSprite) +            // turned counterclockwise
            tile_cell(0, 1, 2, 2, 0, 0, kNone) +              // a rule tile's plank
            tile_cell(1, 1, 0, 0, 0, 0, kNone) +              // a block with no collider
            "  m_AnimatedTiles: {}\n  m_TileAssetArray:\n"
            "  - m_RefCount: 3\n    m_Data: " +
            ref(11400000, kGroundTile, 2) +
            "\n  - m_RefCount: 3\n    m_Data: " + ref(11400000, kSlopeTile, 2) +
            "\n  - m_RefCount: 1\n    m_Data: " + ref(11400000, kRuleTile, 2) +
            "\n  m_TileSpriteArray:\n  - m_RefCount: 3\n    m_Data: " + ref(1001, kTilesPng) +
            "\n  - m_RefCount: 3\n    m_Data: " + ref(1002, kTilesPng) +
            "\n  - m_RefCount: 1\n    m_Data: " + ref(1003, kTilesPng) +
            "\n  m_TileMatrixArray:\n" + tile_matrix(1, 0, 0, 1) + tile_matrix(-1, 0, 0, 1) +
            tile_matrix(0, -1, 1, 0) +
            "  m_TileColorArray:\n  - m_RefCount: 6\n"
            "    m_Data: {r: 1, g: 1, b: 1, a: 1}\n  - m_RefCount: 1\n"
            "    m_Data: {r: 1, g: 0.5, b: 0.5, a: 1}\n"
            "  m_Color: {r: 1, g: 1, b: 1, a: 1}\n"
            "  m_TileAnchor: {x: 0.5, y: 0.5, z: 0}\n  m_TileOrientation: 0\n");
    scene += component(483693784, "TilemapRenderer", 3103, 3100,
                       "  m_Materials:\n  - {fileID: 10754, guid: "
                       "0000000000000000f000000000000000, type: 0}\n"
                       "  m_SortingLayerID: 0\n  m_SortingOrder: 2\n  m_MaskInteraction: 0\n");
    scene += component(19719996, "TilemapCollider2D", 3104, 3100,
                       "  m_Density: 1\n  m_Material: " + ref(6200000, kBouncy, 2) +
                           "\n  m_IsTrigger: 0\n  m_UsedByEffector: 0\n"
                           "  m_Offset: {x: 0, y: 0}\n");
    write(folder / "Assets/Scenes/Level.unity", scene);
    write(folder / "Assets/Scenes/Level.unity.meta", meta("a1000000000000000000000000000011"));
}

constexpr std::string_view kIsoPng = "a1000000000000000000000000000e01";
constexpr std::string_view kHexPng = "a1000000000000000000000000000e02";
constexpr std::string_view kFlatPng = "a1000000000000000000000000000e03";
constexpr std::int64_t kWholeSprite = 21300000; // a texture's one sprite

// "Board": an isometric tile map of blocks that stand on their bottom corner, anchored at the
// cells' bottom corners as Unity's isometric tile maps are, with a marker under it; and tile
// maps of hexagons pointing up and, swizzled YXZ, flat on top.
void write_shaped_tiles(const fs::path& folder) {
    write_png(folder / "Assets/Art/iso.png", 32, 32);
    // The block's pivot is its bottom middle (alignment 7); its physics shape is all of it.
    write(folder / "Assets/Art/iso.png.meta",
          texture_meta(kIsoPng, 32,
                       "  spriteMode: 1\n  alignment: 7\n  spritePivot: {x: 0.5, y: 0.5}\n"
                       "  spriteSheet:\n    serializedVersion: 2\n    sprites: []\n"
                       "    physicsShape:\n    - - {x: -16, y: 16}\n      - {x: 16, y: 16}\n"
                       "      - {x: 16, y: -16}\n      - {x: -16, y: -16}\n"));
    write_png(folder / "Assets/Art/hex.png", 32, 36);
    write(folder / "Assets/Art/hex.png.meta", texture_meta(kHexPng, 32, "  spriteMode: 1\n"));
    write_png(folder / "Assets/Art/flat.png", 36, 32);
    write(folder / "Assets/Art/flat.png.meta", texture_meta(kFlatPng, 32, "  spriteMode: 1\n"));

    constexpr std::int64_t kNone = 1;
    constexpr std::int64_t kSprite = (std::int64_t{1} << 30) + 1;
    constexpr std::int64_t kCell = (std::int64_t{2} << 30) + 1;
    // A Grid object at the origin, and under it a Tilemap object with its renderer.
    const auto tile_map = [](std::int64_t grid, std::string_view name, std::string_view layout,
                             std::string_view cells, std::string_view sprite,
                             std::initializer_list<std::int64_t> children = {}) {
        const std::int64_t tiles = grid + 100;
        return game_object(grid, std::string(name) + " Grid", {grid + 1, grid + 2}) +
               transform(grid + 1, grid, "{x: 0, y: 0, z: 0}", 0, {tiles + 1}) +
               component(156049354, "Grid", grid + 2, grid, layout) +
               game_object(tiles, std::string(name) + " Tiles",
                           {tiles + 1, tiles + 2, tiles + 3, tiles + 4}) +
               (transform)(tiles + 1, tiles, "{x: 0, y: 0, z: 0}", grid + 1, children) +
               component(1839735485, "Tilemap", tiles + 2, tiles,
                         "  m_Tiles:\n" + std::string(cells) +
                             "  m_TileSpriteArray:\n  - m_RefCount: 1\n    m_Data: " +
                             ref(kWholeSprite, sprite) + "\n  m_TileMatrixArray:\n" +
                             tile_matrix(1, 0, 0, 1) + tile_matrix(-1, 0, 0, 1) +
                             "  m_TileAnchor: {x: 0, y: 0, z: 0}\n  m_TileOrientation: 0\n") +
               component(483693784, "TilemapRenderer", tiles + 3, tiles,
                         "  m_SortingLayerID: 0\n  m_SortingOrder: 0\n  m_MaskInteraction: 0\n") +
               component(19719996, "TilemapCollider2D", tiles + 4, tiles,
                         "  m_Density: 1\n  m_Material: {fileID: 0}\n  m_IsTrigger: 0\n"
                         "  m_UsedByEffector: 0\n  m_Offset: {x: 0, y: 0}\n");
    };
    std::string scene(kHeader);
    // Cells of 32x16 pixels; one block with the cell as its collider, one flipped.
    scene += tile_map(3000, "Iso",
                      "  m_CellSize: {x: 1, y: 0.5, z: 1}\n  m_CellGap: {x: 0, y: 0, z: 0}\n"
                      "  m_CellLayout: 2\n  m_CellSwizzle: 0\n",
                      tile_cell(0, 0, 0, 0, 0, 0, kSprite) + tile_cell(1, 0, 0, 0, 0, 0, kCell) +
                          tile_cell(0, 1, 0, 0, 1, 0, kSprite),
                      kIsoPng, {3201});
    scene += game_object(3200, "Marker", {3201});
    scene += transform(3201, 3200, "{x: 1, y: 1, z: 0}", 3101);
    // Hexagons 32 pixels wide and 36 high, and, swizzled, 36 wide and 32 high.
    scene += tile_map(4000, "Hex",
                      "  m_CellSize: {x: 1, y: 1.125, z: 1}\n  m_CellGap: {x: 0, y: 0, z: 0}\n"
                      "  m_CellLayout: 1\n  m_CellSwizzle: 0\n",
                      tile_cell(0, 0, 0, 0, 0, 0, kNone) + tile_cell(1, 1, 0, 0, 0, 0, kNone) +
                          tile_cell(-1, -1, 0, 0, 0, 0, kNone) + tile_cell(0, 1, 0, 0, 0, 0, kNone),
                      kHexPng);
    scene += tile_map(5000, "Flat",
                      "  m_CellSize: {x: 1, y: 1.125, z: 1}\n  m_CellGap: {x: 0, y: 0, z: 0}\n"
                      "  m_CellLayout: 1\n  m_CellSwizzle: 2\n",
                      tile_cell(0, 0, 0, 0, 0, 0, kNone) + tile_cell(1, 1, 0, 0, 0, 0, kNone) +
                          tile_cell(0, 1, 0, 0, 0, 0, kNone) + tile_cell(-1, -1, 0, 0, 0, 0, kNone),
                      kFlatPng);
    write(folder / "Assets/Scenes/Board.unity", scene);
    write(folder / "Assets/Scenes/Board.unity.meta", meta("a1000000000000000000000000000e04"));
}

constexpr std::string_view kPoolPng = "a1000000000000000000000000000f01";
constexpr std::string_view kSparePng = "a1000000000000000000000000000f0b";

// "Pool": a sheet of water, lava and torch frames and a rock, a sprite on its own, AnimatedTiles of
// Unity's 2D Tilemap Extras playing them, and two tile maps: one with frame rate 2, and one with
// smaller cells, so its own tile set, where the frames clash with still tiles.
void write_animated_tiles(const fs::path& folder) {
    write_png(folder / "Assets/Art/pool.png", 64, 48);
    // Unity's sprite rects go up from the bottom: row 0 is the top one, y 32.
    const auto sprite = [](std::string_view name, int column, int row, int id) {
        return "    - serializedVersion: 2\n      name: " + std::string(name) +
               "\n      rect:\n        serializedVersion: 2\n        x: " +
               std::to_string(column * 16) + "\n        y: " + std::to_string(32 - (row * 16)) +
               "\n        width: 16\n        height: 16\n      alignment: 0\n"
               "      pivot: {x: 0.5, y: 0.5}\n      internalID: " +
               std::to_string(id) + "\n";
    };
    write(folder / "Assets/Art/pool.png.meta",
          texture_meta(kPoolPng, 16,
                       "  spriteMode: 2\n  spriteSheet:\n    serializedVersion: 2\n"
                       "    sprites:\n" +
                           sprite("Water0", 0, 0, 2001) + sprite("Water1", 1, 0, 2002) +
                           sprite("Water2", 2, 0, 2003) + sprite("Rock", 3, 0, 2004) +
                           sprite("Lava0", 0, 1, 2005) + sprite("Lava1", 0, 2, 2006) +
                           sprite("Torch0", 2, 1, 2007) + sprite("Torch1", 3, 1, 2008) +
                           sprite("Torch2", 2, 2, 2009) + sprite("Torch3", 3, 2, 2010)));
    // A sprite on another texture.
    write_png(folder / "Assets/Art/spare.png", 16, 16);
    write(folder / "Assets/Art/spare.png.meta", texture_meta(kSparePng, 16, "  spriteMode: 1\n"));
    const auto animated = [&](std::string_view name, std::string_view guid,
                              std::initializer_list<std::string> sprites, std::string_view more) {
        std::string list;
        for (const std::string& frame : sprites) {
            list += "  - " + frame + "\n";
        }
        const fs::path path = folder / "Assets/Tiles" / (std::string(name) + ".asset");
        write(path, std::string(kHeader) + "--- !u!114 &11400000\nMonoBehaviour:\n" +
                        std::string(kSource) +
                        "  m_GameObject: {fileID: 0}\n  m_Enabled: 1\n  m_Script: " +
                        ref(11500000, "a1000000000000000000000000000f0f") +
                        "\n  m_Name: " + std::string(name) + "\n  m_AnimatedSprites:\n" + list +
                        std::string(more) + "  m_TileColliderType: 1\n");
        write(path.string() + ".meta",
              meta(guid, "NativeFormatImporter", "  mainObjectFileID: 11400000\n"));
    };
    constexpr std::string_view kStill = "{fileID: 13312, guid: 0000000000000000e000000000000000, "
                                        "type: 0}";
    // Water plays at 2 frames a second, and torches between 0.5 and 1.5; lava starts a second in.
    const auto pool = [](std::int64_t id) { return ref(id, kPoolPng); };
    animated("Water", "a1000000000000000000000000000f02", {pool(2001), pool(2002), pool(2003)},
             "  m_MinSpeed: 2\n  m_MaxSpeed: 2\n");
    animated("Torch", "a1000000000000000000000000000f03",
             {pool(2007), pool(2008), pool(2009), pool(2010)},
             "  m_MinSpeed: 0.5\n  m_MaxSpeed: 1.5\n");
    animated("Lava", "a1000000000000000000000000000f04", {pool(2005), pool(2006)},
             "  m_MinSpeed: 1\n  m_MaxSpeed: 1\n  m_AnimationStartTime: 1\n");
    // Frames that go back along the row, and frames on two textures, which Godot's can't play.
    animated("Jumble", "a1000000000000000000000000000f05", {pool(2004), pool(2001)}, "");
    animated("Elsewhere", "a1000000000000000000000000000f0a",
             {pool(2004), ref(kWholeSprite, kSparePng)}, "");
    write_tile(folder / "Assets/Tiles/Rock.asset", "a1000000000000000000000000000f06", kStill, 0);
    write_tile(folder / "Assets/Tiles/Foam.asset", "a1000000000000000000000000000f07", kStill, 0);
    write_tile(folder / "Assets/Tiles/Brazier.asset", "a1000000000000000000000000000f08", kStill,
               0);

    constexpr std::int64_t kNone = 1;
    const auto tile_map = [](std::int64_t grid, std::string_view name, std::string_view cell_size,
                             std::string_view cells, std::string_view assets,
                             std::string_view sprites, std::string_view more) {
        const std::int64_t tiles = grid + 100;
        return game_object(grid, std::string(name) + " Grid", {grid + 1, grid + 2}) +
               transform(grid + 1, grid, "{x: 0, y: 0, z: 0}", 0, {tiles + 1}) +
               component(156049354, "Grid", grid + 2, grid,
                         "  m_CellSize: " + std::string(cell_size) +
                             "\n  m_CellGap: {x: 0, y: 0, z: 0}\n  m_CellLayout: 0\n"
                             "  m_CellSwizzle: 0\n") +
               game_object(tiles, std::string(name) + " Tiles", {tiles + 1, tiles + 2, tiles + 3}) +
               transform(tiles + 1, tiles, "{x: 0, y: 0, z: 0}", grid + 1) +
               component(1839735485, "Tilemap", tiles + 2, tiles,
                         "  m_Tiles:\n" + std::string(cells) + "  m_TileAssetArray:\n" +
                             std::string(assets) + "  m_TileSpriteArray:\n" + std::string(sprites) +
                             "  m_TileMatrixArray:\n" + tile_matrix(1, 0, 0, 1) +
                             std::string(more) +
                             "  m_TileAnchor: {x: 0.5, y: 0.5, z: 0}\n"
                             "  m_TileOrientation: 0\n") +
               component(483693784, "TilemapRenderer", tiles + 3, tiles,
                         "  m_SortingLayerID: 0\n  m_SortingOrder: 0\n  m_MaskInteraction: 0\n");
    };
    const auto entries = [](std::initializer_list<std::string> refs) {
        std::string out;
        for (const std::string& item : refs) {
            out += "  - m_RefCount: 1\n    m_Data: " + item + "\n";
        }
        return out;
    };
    const auto asset = [](std::string_view guid) { return ref(11400000, guid, 2); };
    std::string scene(kHeader);
    // Unity saves each animated cell with the sprite it showed then; water's second frame.
    scene += tile_map(
        3000, "Pool", "{x: 1, y: 1, z: 0}",
        tile_cell(0, 0, 0, 0, 0, 0, kNone) + tile_cell(1, 0, 0, 0, 0, 0, kNone) +
            tile_cell(2, 0, 1, 1, 0, 0, kNone) + tile_cell(3, 0, 2, 2, 0, 0, kNone) +
            tile_cell(4, 0, 3, 3, 0, 0, kNone) + tile_cell(5, 0, 4, 3, 0, 0, kNone) +
            tile_cell(6, 0, 5, 3, 0, 0, kNone),
        entries(
            {asset("a1000000000000000000000000000f02"), asset("a1000000000000000000000000000f03"),
             asset("a1000000000000000000000000000f04"), asset("a1000000000000000000000000000f06"),
             asset("a1000000000000000000000000000f05"), asset("a1000000000000000000000000000f0a")}),
        entries(
            {ref(2002, kPoolPng), ref(2007, kPoolPng), ref(2005, kPoolPng), ref(2004, kPoolPng)}),
        "  m_AnimationFrameRate: 2\n");
    // Foam shows water's last frame and a brazier a torch's first, still.
    scene += tile_map(4000, "Edge", "{x: 0.5, y: 0.5, z: 0}",
                      tile_cell(0, 0, 0, 0, 0, 0, kNone) + tile_cell(1, 0, 1, 1, 0, 0, kNone) +
                          tile_cell(2, 0, 2, 2, 0, 0, kNone) + tile_cell(3, 0, 3, 2, 0, 0, kNone),
                      entries({asset("a1000000000000000000000000000f02"),
                               asset("a1000000000000000000000000000f07"),
                               asset("a1000000000000000000000000000f03"),
                               asset("a1000000000000000000000000000f08")}),
                      entries({ref(2001, kPoolPng), ref(2003, kPoolPng), ref(2007, kPoolPng)}), "");
    write(folder / "Assets/Scenes/Pool.unity", scene);
    write(folder / "Assets/Scenes/Pool.unity.meta", meta("a1000000000000000000000000000f09"));
}

constexpr std::string_view kFrogPng = "a1000000000000000000000000000012";
constexpr std::string_view kIdleAnim = "a1000000000000000000000000000013";
constexpr std::string_view kJumpAnim = "a1000000000000000000000000000014";
constexpr std::string_view kSitAnim = "a1000000000000000000000000000015";
constexpr std::string_view kFrogController = "a1000000000000000000000000000016";
constexpr std::string_view kToadController = "a1000000000000000000000000000017";
constexpr std::string_view kVectorWeight = "{x: 0.33333334, y: 0.33333334, z: 0.33333334}";

// A key of an animation curve: a number, or for a vector curve each part's.
std::string anim_key(std::string_view time, std::string_view value, std::string_view in,
                     std::string_view out, std::string_view weight = "0.33333334") {
    return "      - serializedVersion: 3\n        time: " + std::string(time) +
           "\n        value: " + std::string(value) + "\n        inSlope: " + std::string(in) +
           "\n        outSlope: " + std::string(out) +
           "\n        tangentMode: 0\n        weightedMode: 0\n        inWeight: " +
           std::string(weight) + "\n        outWeight: " + std::string(weight) + "\n";
}

// A curve of a clip, with what it animates.
std::string anim_curve(std::string_view keys, std::string_view binding) {
    return "  - curve:\n      serializedVersion: 2\n      m_Curve:\n" + std::string(keys) +
           "      m_PreInfinity: 2\n      m_PostInfinity: 2\n      m_RotationOrder: 4\n" +
           std::string(binding);
}

// A float curve's binding: an attribute of a component class on the object at path.
std::string anim_binding(std::string_view attribute, std::string_view path, int class_id,
                         std::string_view script = "{fileID: 0}") {
    return "    attribute: " + std::string(attribute) + "\n    path: " + std::string(path) +
           "\n    classID: " + std::to_string(class_id) + "\n    script: " + std::string(script) +
           "\n";
}

struct ClipCurves {
    std::string euler;
    std::string position;
    std::string floats;
    std::string sprites;
    std::string events;
};

void write_clip(const fs::path& path, std::string_view guid, std::string_view stop, bool loop,
                const ClipCurves& curves) {
    const auto listed = [](const std::string& items) {
        return items.empty() ? std::string(" []\n") : "\n" + items;
    };
    write(path, std::string(kHeader) + "--- !u!74 &7400000\nAnimationClip:\n" +
                    std::string(kSource) + "  m_Name: " + path.stem().string() +
                    "\n  serializedVersion: 7\n  m_Legacy: 0\n  m_Compressed: 0\n"
                    "  m_UseHighQualityCurve: 1\n  m_RotationCurves: []\n"
                    "  m_CompressedRotationCurves: []\n  m_EulerCurves:" +
                    listed(curves.euler) + "  m_PositionCurves:" + listed(curves.position) +
                    "  m_ScaleCurves: []\n  m_FloatCurves:" + listed(curves.floats) +
                    "  m_PPtrCurves:" + listed(curves.sprites) +
                    "  m_SampleRate: 12\n  m_WrapMode: 0\n  m_AnimationClipSettings:\n"
                    "    serializedVersion: 2\n    m_StartTime: 0\n    m_StopTime: " +
                    std::string(stop) + "\n    m_LoopTime: " + (loop ? "1" : "0") +
                    "\n  m_EditorCurves: []\n  m_Events:" + listed(curves.events));
    write(path.string() + ".meta",
          meta(guid, "NativeFormatImporter", "  mainObjectFileID: 7400000\n"));
}

std::string animator_state(std::int64_t object, std::string_view name, std::string_view speed,
                           std::string_view transitions, std::string_view motion) {
    return "--- !u!1102 &" + id(object) + "\nAnimatorState:\n  serializedVersion: 6\n" +
           std::string(kSource) + "  m_Name: " + std::string(name) +
           "\n  m_Speed: " + std::string(speed) +
           "\n  m_CycleOffset: 0\n  m_Transitions:" + std::string(transitions) +
           "\n  m_StateMachineBehaviours: []\n  m_WriteDefaultValues: 1\n  m_Mirror: 0\n"
           "  m_SpeedParameterActive: 0\n  m_MirrorParameterActive: 0\n"
           "  m_CycleOffsetParameterActive: 0\n  m_TimeParameterActive: 0\n  m_Motion: " +
           std::string(motion) + "\n  m_Tag: \n";
}

std::string animator_transition(std::int64_t object, std::string_view conditions,
                                std::int64_t destination, bool exit_time) {
    return "--- !u!1101 &" + id(object) + "\nAnimatorStateTransition:\n" + std::string(kSource) +
           "  m_Name: \n  m_Conditions:" + std::string(conditions) +
           "\n  m_DstStateMachine: {fileID: 0}\n  m_DstState: " + ref(destination) +
           "\n  m_Solo: 0\n  m_Mute: 0\n  m_IsExit: 0\n  serializedVersion: 3\n"
           "  m_TransitionDuration: 0.25\n  m_TransitionOffset: 0\n  m_ExitTime: 1\n"
           "  m_HasExitTime: " +
           (exit_time ? "1" : "0") + "\n  m_HasFixedDuration: 1\n";
}

std::string animator_condition(int mode, std::string_view parameter, std::string_view threshold) {
    return "\n  - m_ConditionMode: " + std::to_string(mode) +
           "\n    m_ConditionEvent: " + std::string(parameter) +
           "\n    m_EventTreshold: " + std::string(threshold);
}

std::string animator_parameter(std::string_view name, int type, std::string_view value) {
    return "  - m_Name: " + std::string(name) + "\n    m_Type: " + std::to_string(type) +
           "\n    m_DefaultFloat: " + (type == 1 ? std::string(value) : "0") +
           "\n    m_DefaultInt: 0\n    m_DefaultBool: " + (type == 4 ? std::string(value) : "0") +
           "\n    m_Controller: {fileID: 0}\n";
}

// "Frogs": a sheet of two frames, clips (idle: a bob, a fade, frames and an event; jump: a turn
// and a flip; sit), a controller with three states and a second layer, an override controller,
// and a scene with an Animator on each.
void write_frogs(const fs::path& folder) {
    const auto frame = [](std::string_view name, int x, int id) {
        return "    - serializedVersion: 2\n      name: " + std::string(name) +
               "\n      rect:\n        serializedVersion: 2\n        x: " + std::to_string(x) +
               "\n        y: 0\n        width: 16\n        height: 16\n      alignment: 7\n"
               "      pivot: {x: 0.5, y: 0}\n      internalID: " +
               std::to_string(id) + "\n";
    };
    write_png(folder / "Assets/Art/frog.png", 32, 16);
    write(folder / "Assets/Art/frog.png.meta",
          texture_meta(kFrogPng, 16,
                       "  spriteMode: 2\n  spriteSheet:\n    serializedVersion: 2\n    sprites:\n" +
                           frame("frog_0", 0, 1001) + frame("frog_1", 16, 1002)));

    // Idle bobs the body up a quarter unit and back, fades it with a curve and swaps its frame
    // halfway, where it croaks.
    ClipCurves idle;
    idle.position = anim_curve(anim_key("0", "{x: 0, y: 0.5, z: 0}", "{x: 0, y: 0.5, z: 0}",
                                        "{x: 0, y: 0.5, z: 0}", kVectorWeight) +
                                   anim_key("0.5", "{x: 0, y: 0.75, z: 0}", "{x: 0, y: 0.5, z: 0}",
                                            "{x: 0, y: -0.5, z: 0}", kVectorWeight) +
                                   anim_key("1", "{x: 0, y: 0.5, z: 0}", "{x: 0, y: -0.5, z: 0}",
                                            "{x: 0, y: -0.5, z: 0}", kVectorWeight),
                               "    path: Body\n");
    idle.floats = anim_curve(anim_key("0", "1", "0", "0") + anim_key("1", "0.5", "0", "0"),
                             anim_binding("m_Color.a", "Body", 212));
    idle.sprites = "  - curve:\n    - time: 0\n      value: " + ref(1001, kFrogPng) +
                   "\n    - time: 0.5\n      value: " + ref(1002, kFrogPng) +
                   "\n    attribute: m_Sprite\n    path: Body\n    classID: 212\n"
                   "    script: {fileID: 0}\n";
    idle.events = "  - time: 0.5\n    functionName: Croak\n    data: \n"
                  "    objectReferenceParameter: {fileID: 0}\n    floatParameter: 0\n"
                  "    intParameter: 2\n    messageOptions: 0\n";
    write_clip(folder / "Assets/Animations/Idle.anim", kIdleAnim, "1", true, idle);

    // Jump turns the frog a quarter turn counterclockwise and flips its body a quarter of the
    // way in, with stepped keys, and animates a script's value.
    ClipCurves jump;
    jump.euler = anim_curve(anim_key("0", "{x: 0, y: 0, z: 0}", "{x: 0, y: 0, z: 180}",
                                     "{x: 0, y: 0, z: 180}", kVectorWeight) +
                                anim_key("0.5", "{x: 0, y: 0, z: 90}", "{x: 0, y: 0, z: 180}",
                                         "{x: 0, y: 0, z: 180}", kVectorWeight),
                            "    path: \n");
    jump.floats = anim_curve(anim_key("0", "0", "Infinity", "Infinity") +
                                 anim_key("0.25", "1", "Infinity", "Infinity"),
                             anim_binding("m_FlipX", "Body", 212)) +
                  anim_curve(anim_key("0", "1", "0", "0"),
                             anim_binding("power", "", 114, ref(11500000, kCrateCs)));
    write_clip(folder / "Assets/Animations/Jump.anim", kJumpAnim, "0.5", false, jump);

    ClipCurves sit;
    sit.position = anim_curve(anim_key("0", "{x: 0, y: 0, z: 0}", "{x: 1, y: 0, z: 0}",
                                       "{x: 1, y: 0, z: 0}", kVectorWeight) +
                                  anim_key("1", "{x: 1, y: 0, z: 0}", "{x: 1, y: 0, z: 0}",
                                           "{x: 1, y: 0, z: 0}", kVectorWeight),
                              "    path: \n");
    write_clip(folder / "Assets/Animations/Sit.anim", kSitAnim, "1", true, sit);
    write_clip(folder / "Assets/Animations/Unused.anim", "a1000000000000000000000000000018", "1",
               false, {});

    // Idle goes to Jump on the Jump trigger and to Walk, a blend tree, when Speed passes 0.1;
    // Jump goes back to Idle at its end.
    const std::string idle_motion = ref(7400000, kIdleAnim, 2);
    std::string controller(kHeader);
    controller +=
        animator_transition(110100001, animator_condition(1, "Jump", "0"), 110200002, false);
    controller += animator_transition(110100002, " []", 110200001, true);
    controller +=
        animator_transition(110100003, animator_condition(3, "Speed", "0.1"), 110200003, false);
    controller += "--- !u!91 &9100000\nAnimatorController:\n" + std::string(kSource) +
                  "  m_Name: Frog\n  serializedVersion: 5\n  m_AnimatorParameters:\n" +
                  animator_parameter("Speed", 1, "0") + animator_parameter("Jump", 9, "0") +
                  animator_parameter("Grounded", 4, "1") +
                  "  m_AnimatorLayers:\n  - serializedVersion: 5\n    m_Name: Base Layer\n"
                  "    m_StateMachine: {fileID: 110700000}\n    m_Mask: {fileID: 0}\n"
                  "  - serializedVersion: 5\n    m_Name: Face\n"
                  "    m_StateMachine: {fileID: 110700001}\n    m_Mask: {fileID: 0}\n";
    controller += animator_state(110200001, "Idle", "1",
                                 "\n  - {fileID: 110100001}\n  - {fileID: 110100003}", idle_motion);
    controller += animator_state(110200002, "Jump", "2", "\n  - {fileID: 110100002}",
                                 ref(7400000, kJumpAnim, 2));
    controller += animator_state(110200003, "Walk", "1", " []", ref(20600000));
    controller += "--- !u!1107 &110700000\nAnimatorStateMachine:\n  serializedVersion: 6\n" +
                  std::string(kSource) +
                  "  m_Name: Base Layer\n  m_ChildStates:\n  - serializedVersion: 1\n"
                  "    m_State: {fileID: 110200001}\n  - serializedVersion: 1\n"
                  "    m_State: {fileID: 110200002}\n  - serializedVersion: 1\n"
                  "    m_State: {fileID: 110200003}\n  m_ChildStateMachines: []\n"
                  "  m_AnyStateTransitions: []\n  m_EntryTransitions: []\n"
                  "  m_DefaultState: {fileID: 110200001}\n";
    controller += "--- !u!1107 &110700001\nAnimatorStateMachine:\n  serializedVersion: 6\n" +
                  std::string(kSource) +
                  "  m_Name: Face\n  m_ChildStates: []\n  m_ChildStateMachines: []\n"
                  "  m_AnyStateTransitions: []\n  m_EntryTransitions: []\n"
                  "  m_DefaultState: {fileID: 0}\n";
    controller += "--- !u!206 &20600000\nBlendTree:\n" + std::string(kSource) +
                  "  m_Name: Blend Tree\n  m_Childs:\n  - serializedVersion: 2\n    m_Motion: " +
                  idle_motion + "\n    m_Threshold: 0\n  - serializedVersion: 2\n    m_Motion: " +
                  ref(7400000, kJumpAnim, 2) + "\n    m_Threshold: 1\n  m_BlendParameter: Speed\n";
    write(folder / "Assets/Animations/Frog.controller", controller);
    write(folder / "Assets/Animations/Frog.controller.meta",
          meta(kFrogController, "NativeFormatImporter", "  mainObjectFileID: 9100000\n"));

    // The toad's controller is the frog's, sitting instead of idling.
    write(folder / "Assets/Animations/Toad.overrideController",
          std::string(kHeader) + "--- !u!221 &22100000\nAnimatorOverrideController:\n" +
              std::string(kSource) +
              "  m_Name: Toad\n  m_Controller: " + ref(9100000, kFrogController, 2) +
              "\n  m_Clips:\n  - m_OriginalClip: " + idle_motion +
              "\n    m_OverrideClip: " + ref(7400000, kSitAnim, 2) + "\n");
    write(folder / "Assets/Animations/Toad.overrideController.meta",
          meta(kToadController, "NativeFormatImporter", "  mainObjectFileID: 22100000\n"));

    // The frog's body is a child it animates; the toad has none, and moves by root motion.
    std::string scene(kHeader);
    scene += game_object(4000, "Frog", {4001, 4002});
    scene += transform(4001, 4000, "{x: 0, y: 0, z: 0}", 0, {4101});
    scene +=
        component(95, "Animator", 4002, 4000,
                  "  m_Avatar: {fileID: 0}\n  m_Controller: " + ref(9100000, kFrogController, 2) +
                      "\n  m_CullingMode: 0\n  m_UpdateMode: 0\n  m_ApplyRootMotion: 0\n");
    scene += game_object(4100, "Body", {4101, 4102});
    scene += transform(4101, 4100, "{x: 0, y: 0.5, z: 0}", 4001);
    scene += sprite_renderer(4102, 4100, ref(1001, kFrogPng));
    scene += game_object(4200, "Toad", {4201, 4202});
    scene += transform(4201, 4200, "{x: 3, y: 0, z: 0}");
    scene +=
        component(95, "Animator", 4202, 4200,
                  "  m_Avatar: {fileID: 0}\n  m_Controller: " + ref(22100000, kToadController, 2) +
                      "\n  m_CullingMode: 0\n  m_UpdateMode: 0\n  m_ApplyRootMotion: 1\n");
    write(folder / "Assets/Scenes/Frogs.unity", scene);
    write(folder / "Assets/Scenes/Frogs.unity.meta", meta("a1000000000000000000000000000019"));
}

constexpr std::string_view kMainMixer = "a100000000000000000000000000001a";
constexpr std::string_view kUiMixer = "a100000000000000000000000000001b";

std::string
mixer_group(std::int64_t object, std::string_view name, std::string_view children,
            std::string_view volume, std::string_view effects,
            std::string_view flags = "  m_Mute: 0\n  m_Solo: 0\n  m_BypassEffects: 0\n") {
    return "--- !u!243 &" + id(object) + "\nAudioMixerGroupController:\n" + std::string(kSource) +
           "  m_Name: " + std::string(name) + "\n  m_AudioMixer: {fileID: 24100000}\n" +
           "  m_GroupID: 1000000000000000000000000000" + id(object % 10000) +
           "\n  m_Children:" + std::string(children) + "\n  m_Volume: " + std::string(volume) +
           "\n  m_Pitch: 3" + std::string(volume).substr(1) +
           "\n  m_Send: 00000000000000000000000000000000\n  m_Effects:\n" + std::string(effects) +
           "  m_UserColorIndex: 0\n" + std::string(flags);
}

// A mixer effect with its settings' GUIDs, "name:GUID" pairs.
std::string mixer_effect(std::int64_t object, std::string_view name,
                         std::initializer_list<std::string_view> settings,
                         std::string_view more = "  m_SendTarget: {fileID: 0}\n  m_Bypass: 0\n") {
    std::string out = "--- !u!244 &" + id(object) + "\nAudioMixerEffectController:\n" +
                      std::string(kSource) + "  m_Name: \n  m_EffectID: 40000000000000000000" +
                      id(object) + "\n  m_EffectName: " + std::string(name) +
                      "\n  m_MixLevel: 5000000000000000000000" + id(object) + "\n  m_Parameters:";
    if (settings.size() == 0) {
        out += " []";
    }
    for (const std::string_view setting : settings) {
        const auto colon = setting.find(':');
        out += "\n  - m_ParameterName: " + std::string(setting.substr(0, colon)) +
               "\n    m_GUID: " + std::string(setting.substr(colon + 1));
    }
    return out + "\n  m_EnableWetMix: 0\n" + std::string(more);
}

std::string mixer_controller(std::string_view name, std::string_view output,
                             std::string_view snapshots) {
    return std::string(kHeader) + "--- !u!241 &24100000\nAudioMixerController:\n" +
           std::string(kSource) + "  m_Name: " + std::string(name) +
           "\n  m_OutputGroup: " + std::string(output) +
           "\n  m_MasterGroup: {fileID: 24300002}\n  m_Snapshots:" + std::string(snapshots) +
           "\n  m_StartSnapshot: {fileID: 24500006}\n  m_ExposedParameters: []\n";
}

std::string mixer_snapshot(std::int64_t object, std::string_view name, std::string_view values) {
    return "--- !u!245 &" + id(object) + "\nAudioMixerSnapshotController:\n" +
           std::string(kSource) + "  m_Name: " + std::string(name) +
           "\n  m_AudioMixer: {fileID: 24100000}\n  m_FloatValues:" + std::string(values) +
           "\n  m_TransitionOverrides: {}\n";
}

std::string rigidbody(std::int64_t object, std::int64_t owner, int type, std::string_view mass) {
    return component(50, "Rigidbody2D", object, owner,
                     "  m_BodyType: " + std::to_string(type) +
                         "\n  m_Simulated: 1\n  m_UseAutoMass: 0\n  m_Mass: " + std::string(mass) +
                         "\n  m_LinearDrag: 0\n  m_AngularDrag: 0\n  m_GravityScale: 1\n"
                         "  m_SleepingMode: 1\n  m_CollisionDetection: 0\n  m_Constraints: 0\n");
}

std::string joint(int class_id, std::string_view type, std::int64_t object, std::int64_t owner,
                  std::string_view connected, std::string_view fields) {
    return component(class_id, type, object, owner,
                     "  m_EnableCollision: 0\n  m_ConnectedRigidBody: " + std::string(connected) +
                         "\n  m_BreakForce: Infinity\n  m_BreakTorque: Infinity\n" +
                         std::string(fields));
}

// "Mill": two audio mixers, the second sending into the first, sounds on their groups, and a
// scene of bodies on joints.
void write_mill(const fs::path& folder) {
    std::string main =
        mixer_controller("Main", "{fileID: 0}", "\n  - {fileID: 24500006}\n  - {fileID: 24500007}");
    main += mixer_group(24300002, "Master", "\n  - {fileID: 24300010}\n  - {fileID: 24300020}",
                        "2b000000000000000000000000000001",
                        "  - {fileID: 24400004}\n  - {fileID: 24400032}\n");
    // Music ducks under what SFX sends it, then a low pass after its fader.
    main += mixer_group(24300010, "Music", " []", "2b000000000000000000000000000002",
                        "  - {fileID: 24400012}\n  - {fileID: 24400011}\n  - {fileID: 24400013}\n");
    main +=
        mixer_group(24300020, "SFX", "\n  - {fileID: 24300030}", "2b000000000000000000000000000003",
                    "  - {fileID: 24400021}\n  - {fileID: 24400022}\n  - {fileID: 24400024}\n"
                    "  - {fileID: 24400025}\n  - {fileID: 24400026}\n  - {fileID: 24400027}\n"
                    "  - {fileID: 24400028}\n  - {fileID: 24400029}\n  - {fileID: 24400030}\n"
                    "  - {fileID: 24400031}\n  - {fileID: 24400033}\n");
    main += mixer_group(24300030, "Footsteps", " []", "2b000000000000000000000000000004",
                        "  - {fileID: 24400042}\n  - {fileID: 24400041}\n",
                        "  m_Mute: 1\n  m_Solo: 1\n  m_BypassEffects: 1\n");
    for (const std::int64_t attenuation : {24400004, 24400011, 24400021, 24400041}) {
        main += mixer_effect(attenuation, "Attenuation", {});
    }
    main += mixer_effect(24400012, "Duck Volume",
                         {"Threshold:6f000000000000000000000000000001",
                          "Ratio:6f000000000000000000000000000002",
                          "Attack Time:6f000000000000000000000000000003",
                          "Release Time:6f000000000000000000000000000004",
                          "Make-up Gain:6f000000000000000000000000000005"});
    main +=
        mixer_effect(24400013, "Lowpass Simple", {"Cutoff freq:6f000000000000000000000000000006"});
    main += mixer_effect(24400022, "Lowpass",
                         {"Cutoff freq:6f000000000000000000000000000007",
                          "Resonance:6f000000000000000000000000000008"});
    main += mixer_effect(
        24400024, "Echo",
        {"Delay:6f000000000000000000000000000009", "Decay:6f000000000000000000000000000010",
         "Drymix:6f000000000000000000000000000011", "Wetmix:6f000000000000000000000000000012"});
    main += mixer_effect(24400025, "Compressor",
                         {"Threshold:6f000000000000000000000000000013",
                          "Attack:6f000000000000000000000000000014",
                          "Release:6f000000000000000000000000000015",
                          "Make up gain:6f000000000000000000000000000016"});
    main += mixer_effect(
        24400026, "SFX Reverb",
        {"Dry Level:6f000000000000000000000000000017", "Room:6f000000000000000000000000000018",
         "Decay Time:6f000000000000000000000000000019", "Reverb:6f000000000000000000000000000020"});
    main += mixer_effect(24400027, "Distortion", {"Level:6f000000000000000000000000000021"},
                         "  m_SendTarget: {fileID: 0}\n  m_Bypass: 1\n");
    main += mixer_effect(24400028, "Chorus", {});
    main += mixer_effect(24400029, "Flange", {});
    // Sends: to the Duck Volume at -3 dB, to Master's Receive at -10 dB, and one left at -80.
    main +=
        mixer_effect(24400030, "Send", {}, "  m_SendTarget: {fileID: 24400012}\n  m_Bypass: 0\n");
    main +=
        mixer_effect(24400031, "Send", {}, "  m_SendTarget: {fileID: 24400032}\n  m_Bypass: 0\n");
    main +=
        mixer_effect(24400033, "Send", {}, "  m_SendTarget: {fileID: 24400032}\n  m_Bypass: 0\n");
    main += mixer_effect(24400032, "Receive", {});
    main += mixer_effect(24400042, "Pitch Shifter",
                         {"Pitch:6f000000000000000000000000000022",
                          "FFT size:6f000000000000000000000000000023",
                          "Overlap:6f000000000000000000000000000024"});
    main += mixer_snapshot(24500006, "Snapshot",
                           "\n    2b000000000000000000000000000002: -10"
                           "\n    3b000000000000000000000000000004: 50"
                           "\n    6f000000000000000000000000000001: -20"
                           "\n    6f000000000000000000000000000002: 3"
                           "\n    6f000000000000000000000000000003: 1"
                           "\n    6f000000000000000000000000000004: 200"
                           "\n    6f000000000000000000000000000007: 1200"
                           "\n    6f000000000000000000000000000008: 3"
                           "\n    6f000000000000000000000000000009: 300"
                           "\n    6f000000000000000000000000000010: 25"
                           "\n    6f000000000000000000000000000011: 80"
                           "\n    6f000000000000000000000000000013: -12"
                           "\n    6f000000000000000000000000000014: 50"
                           "\n    6f000000000000000000000000000015: 100"
                           "\n    6f000000000000000000000000000016: 3"
                           "\n    6f000000000000000000000000000017: -100"
                           "\n    6f000000000000000000000000000018: -600"
                           "\n    6f000000000000000000000000000019: 2"
                           "\n    6f000000000000000000000000000020: 200"
                           "\n    6f000000000000000000000000000021: 0.25"
                           "\n    6f000000000000000000000000000022: 1.5"
                           "\n    6f000000000000000000000000000023: 2000"
                           "\n    6f000000000000000000000000000024: 8"
                           "\n    500000000000000000000024400030: -3"
                           "\n    500000000000000000000024400031: -10");
    main += mixer_snapshot(24500007, "Quiet", "\n    2b000000000000000000000000000001: -20");
    write(folder / "Assets/Audio/Main.mixer", main);
    write(folder / "Assets/Audio/Main.mixer.meta", meta(kMainMixer, "NativeFormatImporter"));
    // The UI mixer plays into Main's SFX group; its Music group's name is taken.
    std::string ui =
        mixer_controller("UI", ref(24300020, kMainMixer, 2), "\n  - {fileID: 24500006}");
    ui += mixer_group(24300002, "Master", "\n  - {fileID: 24300010}",
                      "2b000000000000000000000000000001", "  - {fileID: 24400004}\n");
    ui += mixer_group(24300010, "Music", " []", "2b000000000000000000000000000002",
                      "  - {fileID: 24400011}\n");
    ui += mixer_effect(24400004, "Attenuation", {});
    ui += mixer_effect(24400011, "Attenuation", {});
    ui += mixer_snapshot(24500006, "Snapshot", "\n    2b000000000000000000000000000001: -3");
    write(folder / "Assets/Audio/UI.mixer", ui);
    write(folder / "Assets/Audio/UI.mixer.meta", meta(kUiMixer, "NativeFormatImporter"));

    std::string scene(kHeader);
    // The frame is static; the door turns on a hinge to it, driven down to its 90 degree limit.
    scene += game_object(5000, "Frame", {5001, 5002});
    scene += transform(5001, 5000, "{x: 0, y: 0, z: 0}", 0, {}, "  m_RootOrder: 0\n");
    scene += rigidbody(5002, 5000, 2, "1");
    scene += game_object(5100, "Door", {5101, 5102, 5103, 5104});
    scene += transform(5101, 5100, "{x: 2, y: 0, z: 0}", 0, {}, "  m_RootOrder: 1\n");
    scene += rigidbody(5102, 5100, 0, "2");
    scene += component(61, "BoxCollider2D", 5103, 5100,
                       std::string(kCollider) + "  m_Size: {x: 2, y: 0.25}\n");
    scene += joint(233, "HingeJoint2D", 5104, 5100, ref(5002),
                   "  m_AutoConfigureConnectedAnchor: 1\n  m_Anchor: {x: -1, y: 0}\n"
                   "  m_ConnectedAnchor: {x: 1, y: 0}\n  m_UseMotor: 1\n  m_Motor:\n"
                   "    m_MotorSpeed: 90\n    m_MaximumMotorForce: 10000\n  m_UseLimits: 1\n"
                   "  m_AngleLimits:\n    m_LowerAngle: -45\n    m_UpperAngle: 90\n");
    // A weight hangs on a spring from a point in the world, 3 units long at rest.
    scene += game_object(5200, "Weight", {5201, 5202, 5203, 5204});
    scene += transform(5201, 5200, "{x: 2, y: -4, z: 0}", 0, {}, "  m_RootOrder: 2\n");
    scene += rigidbody(5202, 5200, 0, "1");
    scene += component(61, "BoxCollider2D", 5203, 5200,
                       std::string(kCollider) + "  m_Size: {x: 0.5, y: 0.5}\n");
    scene += joint(231, "SpringJoint2D", 5204, 5200, ref(0),
                   "  m_AutoConfigureConnectedAnchor: 0\n  m_Anchor: {x: 0, y: 0.5}\n"
                   "  m_ConnectedAnchor: {x: 2, y: 0}\n  m_AutoConfigureDistance: 0\n"
                   "  m_Distance: 3\n  m_DampingRatio: 0.5\n  m_Frequency: 2\n");
    // A bead slides down a groove 30 degrees up from x, from (4, 0) 4 units along.
    scene += game_object(5300, "Bead", {5301, 5302, 5303, 5304});
    scene += transform(5301, 5300, "{x: 4.8660254, y: 0.5, z: 0}", 0, {}, "  m_RootOrder: 3\n");
    scene += rigidbody(5302, 5300, 0, "1");
    scene += component(58, "CircleCollider2D", 5303, 5300,
                       std::string(kCollider) + "  m_Radius: 0.25\n");
    scene += joint(234, "SliderJoint2D", 5304, 5300, ref(0),
                   "  m_AutoConfigureConnectedAnchor: 0\n  m_Anchor: {x: 0, y: 0}\n"
                   "  m_ConnectedAnchor: {x: 4, y: 0}\n  m_AutoConfigureAngle: 0\n"
                   "  m_Angle: 30\n  m_UseMotor: 0\n  m_UseLimits: 1\n  m_TranslationLimits:\n"
                   "    m_LowerTranslation: 0\n    m_UpperTranslation: 4\n");
    // What doesn't convert: a fixed joint, a disabled hinge and a hinge that breaks.
    scene += game_object(5400, "Gate", {5401, 5402, 5403, 5404, 5405});
    scene += transform(5401, 5400, "{x: 10, y: 0, z: 0}", 0, {}, "  m_RootOrder: 4\n");
    scene += rigidbody(5402, 5400, 1, "1");
    scene += joint(255, "FixedJoint2D", 5403, 5400, ref(0), "");
    scene +=
        "--- !u!233 &5404\nHingeJoint2D:\n" + std::string(kSource) +
        "  m_GameObject: {fileID: 5400}\n  m_Enabled: 0\n  m_ConnectedRigidBody: {fileID: 0}\n";
    scene += component(233, "HingeJoint2D", 5405, 5400,
                       "  m_ConnectedRigidBody: {fileID: 0}\n  m_BreakForce: 100\n"
                       "  m_AutoConfigureConnectedAnchor: 1\n");
    // Sounds on a group of each mixer, and on a group of a mixer the project doesn't have.
    const auto sound = [&](std::int64_t object, std::string_view name, std::string_view group,
                           int order) {
        scene += game_object(object, name, {object + 1, object + 2});
        scene += transform(object + 1, object, "{x: 0, y: 0, z: 0}", 0, {},
                           "  m_RootOrder: " + std::to_string(order) + "\n");
        scene += component(82, "AudioSource", object + 2, object,
                           "  OutputAudioMixerGroup: " + std::string(group) +
                               "\n  m_audioClip: {fileID: 0}\n  m_PlayOnAwake: 0\n"
                               "  m_Volume: 1\n  m_Pitch: 1\n  Loop: 0\n  Mute: 0\n");
    };
    sound(5600, "Steps", ref(24300030, kMainMixer, 2), 5);
    sound(5700, "Click", ref(24300010, kUiMixer, 2), 6);
    sound(5800, "Lost", ref(24300010, "a100000000000000000000000000001c", 2), 7);
    write(folder / "Assets/Scenes/Mill.unity", scene);
    write(folder / "Assets/Scenes/Mill.unity.meta", meta("a100000000000000000000000000001d"));
}

// uGUI's and TextMesh Pro's scripts, whose GUIDs are the same in every project.
constexpr std::string_view kRawImageCs = "1344c3c82d62a2a41a3576d8abb8e3ea";
constexpr std::string_view kButtonCs = "4e29b1a8efbd4b44bb3f3716e73f07ff";
constexpr std::string_view kTextCs = "5f7201a12d95ffc409449d95f23cf332";
constexpr std::string_view kToggleCs = "9085046f02f69544eb97fd06b6048fe2";
constexpr std::string_view kSliderCs = "67db9e8f0e2ae9c40bc1e2b64352a6b4";
constexpr std::string_view kScrollbarCs = "2a4db7a114972834c8e4117be1d82ba3";
constexpr std::string_view kScrollRectCs = "1aa08ab6e0800fa44ae55d278d1423e3";
constexpr std::string_view kMaskCs = "31a19414c41e5ae4aae2af33fee712f6";
constexpr std::string_view kRectMaskCs = "3312d7739989d2b4e91e6319e9a96d76";
constexpr std::string_view kVerticalLayoutCs = "59f8146938fff824cb5fd77236b75775";
constexpr std::string_view kGridLayoutCs = "8a8695521f0d02e499659fee002a26c2";
constexpr std::string_view kFitterCs = "3245ec927659c4140ac4f8d17403cc18";
constexpr std::string_view kScalerCs = "0cd44c1031e13a943bb63640046fad76";
constexpr std::string_view kOutlineCs = "e19747de3f5aca642ab2be37e372fb86";
constexpr std::string_view kRaycasterCs = "dc42784cf147c0c48a680349fa168899";
constexpr std::string_view kTmpTextCs = "f4688fdb7df04437aeb418b961361dc5";
constexpr std::string_view kTmpInputCs = "2da0c512f12947e489f739169773d7ca";
constexpr std::string_view kEventSystemCs = "76c392e42b5098c458856cdf6ecaaaa1";
constexpr std::string_view kBuiltin = "0000000000000000f000000000000000";
constexpr std::string_view kMenuCs = "a1000000000000000000000000000020";
constexpr std::string_view kButtonPng = "a1000000000000000000000000000021";
constexpr std::string_view kIconPng = "a1000000000000000000000000000022";
constexpr std::string_view kPondTtf = "a1000000000000000000000000000023";
constexpr std::string_view kPondSdf = "a1000000000000000000000000000024";
constexpr std::string_view kBadgePrefab = "a1000000000000000000000000000026";

// A RectTransform: anchors, position, size and pivot as "x, y" pairs.
std::string rect_transform(std::int64_t object, std::int64_t owner, std::string_view anchor_min,
                           std::string_view anchor_max, std::string_view position,
                           std::string_view size, std::string_view pivot, std::int64_t father,
                           std::initializer_list<std::int64_t> children = {},
                           std::string_view scale = "1, y: 1") {
    std::string out = "--- !u!224 &" + id(object) + "\nRectTransform:\n" + std::string(kSource) +
                      "  m_GameObject: {fileID: " + id(owner) + "}\n" +
                      "  m_LocalRotation: {x: 0, y: 0, z: 0, w: 1}\n" +
                      "  m_LocalPosition: {x: 0, y: 0, z: 0}\n" +
                      "  m_LocalScale: {x: " + std::string(scale) + ", z: 1}\n";
    if (children.size() == 0) {
        out += "  m_Children: []\n";
    } else {
        out += "  m_Children:\n";
        for (const std::int64_t child : children) {
            out += "  - {fileID: " + id(child) + "}\n";
        }
    }
    const auto xy = [](std::string_view pair) {
        const auto comma = pair.find(',');
        return "{x: " + std::string(pair.substr(0, comma)) +
               ", y:" + std::string(pair.substr(comma + 1)) + "}";
    };
    return out + "  m_Father: {fileID: " + id(father) + "}\n  m_AnchorMin: " + xy(anchor_min) +
           "\n  m_AnchorMax: " + xy(anchor_max) + "\n  m_AnchoredPosition: " + xy(position) +
           "\n  m_SizeDelta: " + xy(size) + "\n  m_Pivot: " + xy(pivot) + "\n";
}

// A UI object: a GameObject on the UI layer with a RectTransform and a CanvasRenderer.
std::string ui_object(std::int64_t object, std::string_view name,
                      std::initializer_list<std::int64_t> components, bool active = true) {
    return game_object(object, name, components, 5,
                       active ? "  m_TagString: Untagged\n  m_IsActive: 1\n"
                              : "  m_TagString: Untagged\n  m_IsActive: 0\n") +
           "--- !u!222 &" + id(object + 9) + "\nCanvasRenderer:\n" + std::string(kSource) +
           "  m_GameObject: {fileID: " + id(object) + "}\n  m_CullTransparentMesh: 1\n";
}

std::string script(std::int64_t object, std::int64_t owner, std::string_view guid,
                   std::string_view fields) {
    return component(114, "MonoBehaviour", object, owner,
                     "  m_EditorHideFlags: 0\n  m_Script: " + ref(11500000, guid) +
                         "\n  m_Name: \n  m_EditorClassIdentifier: \n" + std::string(fields));
}

std::string image(std::int64_t object, std::int64_t owner, std::string_view sprite,
                  std::string_view color = "{r: 1, g: 1, b: 1, a: 1}", int type = 0) {
    return script(object, owner, kImageCs,
                  "  m_Material: {fileID: 0}\n  m_Color: " + std::string(color) +
                      "\n  m_RaycastTarget: 1\n  m_Sprite: " + std::string(sprite) +
                      "\n  m_Type: " + std::to_string(type) +
                      "\n  m_PreserveAspect: 0\n  m_FillCenter: 1\n"
                      "  m_PixelsPerUnitMultiplier: 1\n");
}

std::string legacy_text(std::int64_t object, std::int64_t owner, std::string_view text, int size,
                        int alignment) {
    return script(object, owner, kTextCs,
                  "  m_Material: {fileID: 0}\n  m_Color: {r: 0.19607843, g: 0.19607843, b: "
                  "0.19607843, a: 1}\n  m_RaycastTarget: 1\n  m_FontData:\n"
                  "    m_Font: {fileID: 10102, guid: 0000000000000000e000000000000000, type: 0}\n"
                  "    m_FontSize: " +
                      std::to_string(size) +
                      "\n    m_FontStyle: 0\n    m_BestFit: 0\n    m_MinSize: 10\n"
                      "    m_MaxSize: 40\n    m_Alignment: " +
                      std::to_string(alignment) +
                      "\n    m_AlignByGeometry: 0\n    m_RichText: 1\n"
                      "    m_HorizontalOverflow: 0\n    m_VerticalOverflow: 1\n"
                      "    m_LineSpacing: 1\n  m_Text: " +
                      std::string(text) + "\n");
}

std::string tmp_text(std::int64_t object, std::int64_t owner, std::string_view text,
                     std::string_view color, std::string_view more) {
    return script(object, owner, kTmpTextCs,
                  "  m_Material: {fileID: 0}\n  m_Color: {r: 1, g: 1, b: 1, a: 1}\n"
                  "  m_RaycastTarget: 1\n  m_text: " +
                      std::string(text) + "\n  m_isRightToLeft: 0\n  m_fontColor: " +
                      std::string(color) + "\n" + std::string(more));
}

// A persistent call of a UI event to a method of the Menu script.
std::string ui_call(std::string_view event, std::string_view method, int mode = 1) {
    return "  " + std::string(event) +
           ":\n    m_PersistentCalls:\n      m_Calls:\n      - m_Target: {fileID: 1005}\n"
           "        m_TargetAssemblyTypeName: Menu, Assembly-CSharp\n        m_MethodName: " +
           std::string(method) + "\n        m_Mode: " + std::to_string(mode) +
           "\n        m_Arguments:\n          m_ObjectArgument: {fileID: 0}\n"
           "          m_IntArgument: 0\n          m_FloatArgument: 0\n"
           "          m_StringArgument: \n          m_BoolArgument: 0\n        m_CallState: 2\n";
}

constexpr std::string_view kColors = "  m_Colors:\n    m_NormalColor: {r: 1, g: 1, b: 1, a: 1}\n"
                                     "    m_HighlightedColor: {r: 0.9, g: 0.9, b: 0.9, a: 1}\n"
                                     "    m_PressedColor: {r: 0.78, g: 0.78, b: 0.78, a: 1}\n"
                                     "    m_SelectedColor: {r: 0.96, g: 0.96, b: 0.96, a: 1}\n"
                                     "    m_DisabledColor: {r: 0.78, g: 0.78, b: 0.78, a: 0.5}\n"
                                     "    m_ColorMultiplier: 1\n    m_FadeDuration: 0.1\n";

// The fields every Selectable has, with automatic navigation and a color tint.
std::string selectable(std::int64_t target) {
    return "  m_Navigation:\n    m_Mode: 3\n    m_WrapAround: 0\n  m_Transition: 1\n" +
           std::string(kColors) + "  m_Interactable: 1\n  m_TargetGraphic: {fileID: " + id(target) +
           "}\n";
}

// "Pond Hop"'s menu: a canvas scaled from 400x300 to the 800x600 window, holding a panel, a
// TextMesh Pro title in a font asset, a sliced button, the default toggle, a slider, a TextMesh
// Pro input field, a scroll view of a vertical layout, a grid, a raw image and a hidden panel.
void write_menu(const fs::path& folder) {
    write(
        folder / "Assets/Scripts/Menu.cs",
        "using UnityEngine;\n\npublic class Menu : MonoBehaviour\n{\n"
        "    public void StartGame() {}\n    public void SetSound(bool on) {}\n"
        "    public void SetName(string name) {}\n    public void SetVolume(float volume) {}\n}\n");
    write(folder / "Assets/Scripts/Menu.cs.meta", meta(kMenuCs, "MonoImporter"));
    write_png(folder / "Assets/UI/button.png", 48, 48);
    write(folder / "Assets/UI/button.png.meta",
          texture_meta(kButtonPng, 100,
                       "  spriteMode: 1\n  spriteBorder: {x: 12, y: 12, z: 12, w: 12}\n"));
    write_png(folder / "Assets/UI/icon.png", 16, 16);
    write(folder / "Assets/UI/icon.png.meta", texture_meta(kIconPng, 100, "  spriteMode: 1\n"));
    const std::vector<std::uint8_t> font =
        slash::text::box_font(U"Pond HopPlaySoundMiraYour nameLilyReedOptions");
    write(folder / "Assets/Fonts/Pond.ttf", std::string(font.begin(), font.end()));
    write(folder / "Assets/Fonts/Pond.ttf.meta", meta(kPondTtf, "TrueTypeFontImporter"));
    write(folder / "Assets/Fonts/Pond SDF.asset",
          std::string(kHeader) + "--- !u!114 &11400000\nMonoBehaviour:\n" + std::string(kSource) +
              "  m_GameObject: {fileID: 0}\n  m_Enabled: 1\n"
              "  m_Script: {fileID: 11500000, guid: 71c1514a6bd24e1e882cebbe1904ce04, type: 3}\n"
              "  m_Name: Pond SDF\n  m_Version: 1.1.0\n"
              "  m_SourceFontFile: {fileID: 12800000, guid: " +
              std::string(kPondTtf) + ", type: 3}\n");
    write(folder / "Assets/Fonts/Pond SDF.asset.meta",
          meta(kPondSdf, "NativeFormatImporter", "  mainObjectFileID: 11400000\n"));

    // A UI prefab: a red badge with a count.
    std::string badge(kHeader);
    badge += ui_object(100, "Badge", {101, 109, 102});
    badge +=
        rect_transform(101, 100, "0.5, 0.5", "0.5, 0.5", "0, 0", "40, 40", "0.5, 0.5", 0, {111});
    badge += image(102, 100, ref(10905, kBuiltin, 0), "{r: 1, g: 0, b: 0, a: 1}", 1);
    badge += ui_object(110, "Count", {111, 119, 112});
    badge += rect_transform(111, 110, "0, 0", "1, 1", "0, 0", "0, 0", "0.5, 0.5", 101);
    badge += legacy_text(112, 110, "3", 14, 4);
    write(folder / "Assets/UI/Badge.prefab", badge);
    write(folder / "Assets/UI/Badge.prefab.meta", meta(kBadgePrefab, "PrefabImporter"));

    std::string scene(kHeader);
    // The canvas, with the menu's script.
    scene += ui_object(1000, "Canvas", {1001, 1009, 1002, 1003, 1004, 1005});
    scene += rect_transform(
        1001, 1000, "0, 0", "0, 0", "0, 0", "0, 0", "0, 0", 0,
        {1101, 1201, 1301, 1401, 1501, 1601, 1701, 1801, 1901, 2001, brought(2100, 101)});
    scene += component(223, "Canvas", 1002, 1000,
                       "  serializedVersion: 3\n  m_RenderMode: 0\n  m_Camera: {fileID: 0}\n"
                       "  m_PixelPerfect: 0\n  m_OverrideSorting: 0\n  m_SortingOrder: 0\n");
    scene += script(1003, 1000, kScalerCs,
                    "  m_UiScaleMode: 1\n  m_ReferencePixelsPerUnit: 100\n  m_ScaleFactor: 1\n"
                    "  m_ReferenceResolution: {x: 400, y: 300}\n  m_ScreenMatchMode: 0\n"
                    "  m_MatchWidthOrHeight: 0.5\n");
    scene += script(1004, 1000, kRaycasterCs, "  m_IgnoreReversedGraphics: 1\n");
    scene += script(1005, 1000, kMenuCs, "");
    // A dark panel over the whole canvas, in Unity's rounded sprite.
    scene += ui_object(1100, "Panel", {1101, 1109, 1102});
    scene += rect_transform(1101, 1100, "0, 0", "1, 1", "0, 0", "0, 0", "0.5, 0.5", 1001);
    scene += image(1102, 1100, ref(10905, kBuiltin, 0), "{r: 0, g: 0, b: 0, a: 0.5}", 1);
    // The title, at the top, in a TextMesh Pro font asset, with rich text and an outline.
    scene += ui_object(1200, "Title", {1201, 1209, 1202, 1203});
    scene += rect_transform(1201, 1200, "0.5, 1", "0.5, 1", "0, -10", "300, 60", "0.5, 1", 1001);
    scene += tmp_text(1202, 1200, "<b>Pond</b> Hop", "{r: 1, g: 0.9, b: 0.2, a: 1}",
                      "  m_fontAsset: " + ref(11400000, kPondSdf, 2) +
                          "\n  m_fontSize: 32\n  m_fontStyle: 0\n  m_textAlignment: 514\n"
                          "  m_enableWordWrapping: 0\n  m_overflowMode: 0\n"
                          "  m_margin: {x: 0, y: 0, z: 0, w: 0}\n");
    scene += script(1203, 1200, kOutlineCs,
                    "  m_EffectColor: {r: 0, g: 0, b: 0, a: 1}\n"
                    "  m_EffectDistance: {x: 2, y: -2}\n  m_UseGraphicAlpha: 1\n");
    // A button of a sliced sprite, which calls Menu.StartGame, with its label.
    scene += ui_object(1300, "Play", {1301, 1309, 1302, 1303});
    scene += rect_transform(1301, 1300, "0.5, 0.5", "0.5, 0.5", "0, 20", "160, 40", "0.5, 0.5",
                            1001, {1311});
    scene += image(1302, 1300, ref(21300000, kButtonPng), "{r: 1, g: 1, b: 1, a: 1}", 1);
    scene += script(1303, 1300, kButtonCs, selectable(1302) + ui_call("m_OnClick", "StartGame"));
    scene += ui_object(1310, "Text", {1311, 1319, 1312});
    scene += rect_transform(1311, 1310, "0, 0", "1, 1", "0, 0", "0, 0", "0.5, 0.5", 1301);
    scene += legacy_text(1312, 1310, "Play", 16, 4);
    // Unity's default toggle: a background, a checkmark in it and a label.
    scene += ui_object(1400, "Sound", {1401, 1409, 1402});
    scene += rect_transform(1401, 1400, "0.5, 0.5", "0.5, 0.5", "0, -20", "160, 20", "0.5, 0.5",
                            1001, {1411, 1431});
    scene += script(1402, 1400, kToggleCs,
                    selectable(1412) +
                        "  toggleTransition: 1\n  graphic: {fileID: 1422}\n"
                        "  m_Group: {fileID: 0}\n" +
                        ui_call("onValueChanged", "SetSound", 0) + "  m_IsOn: 1\n");
    scene += ui_object(1410, "Background", {1411, 1419, 1412});
    scene +=
        rect_transform(1411, 1410, "0, 1", "0, 1", "10, -10", "20, 20", "0.5, 0.5", 1401, {1421});
    scene += image(1412, 1410, ref(10905, kBuiltin, 0), "{r: 1, g: 1, b: 1, a: 1}", 1);
    scene += ui_object(1420, "Checkmark", {1421, 1429, 1422});
    scene += rect_transform(1421, 1420, "0.5, 0.5", "0.5, 0.5", "0, 0", "20, 20", "0.5, 0.5", 1411);
    scene += image(1422, 1420, ref(10901, kBuiltin, 0));
    scene += ui_object(1430, "Label", {1431, 1439, 1432});
    scene += rect_transform(1431, 1430, "0, 0", "1, 1", "9, -0.5", "-28, -3", "0.5, 0.5", 1401);
    scene += legacy_text(1432, 1430, "Sound", 14, 0);
    // A slider of whole numbers from 0 to 10: a background, a fill and a handle.
    scene += ui_object(1500, "Volume", {1501, 1509, 1502});
    scene += rect_transform(1501, 1500, "0.5, 0.5", "0.5, 0.5", "0, -50", "160, 20", "0.5, 0.5",
                            1001, {1511, 1531, 1551});
    scene += script(1502, 1500, kSliderCs,
                    selectable(1542) +
                        "  m_FillRect: {fileID: 1521}\n  m_HandleRect: {fileID: 1541}\n"
                        "  m_Direction: 0\n  m_MinValue: 0\n  m_MaxValue: 10\n"
                        "  m_WholeNumbers: 1\n  m_Value: 7\n" +
                        ui_call("m_OnValueChanged", "SetVolume", 0));
    scene += ui_object(1510, "Background", {1511, 1519, 1512});
    scene += rect_transform(1511, 1510, "0, 0.25", "1, 0.75", "0, 0", "0, 0", "0.5, 0.5", 1501);
    scene += image(1512, 1510, ref(10907, kBuiltin, 0), "{r: 1, g: 1, b: 1, a: 1}", 1);
    scene += ui_object(1530, "Fill Area", {1531, 1539});
    scene += rect_transform(1531, 1530, "0, 0.25", "1, 0.75", "-5, 0", "-20, 0", "0.5, 0.5", 1501,
                            {1521});
    scene += ui_object(1520, "Fill", {1521, 1529, 1522});
    scene += rect_transform(1521, 1520, "0, 0", "0.7, 1", "0, 0", "10, 0", "0.5, 0.5", 1531);
    scene += image(1522, 1520, ref(10905, kBuiltin, 0), "{r: 0.2, g: 0.8, b: 0.4, a: 1}", 1);
    scene += ui_object(1550, "Handle Slide Area", {1551, 1559});
    scene += rect_transform(1551, 1550, "0, 0", "1, 1", "0, 0", "-20, 0", "0.5, 0.5", 1501, {1541});
    scene += ui_object(1540, "Handle", {1541, 1549, 1542});
    scene += rect_transform(1541, 1540, "0.7, 0", "0.7, 1", "0, 0", "20, 0", "0.5, 0.5", 1551);
    scene += image(1542, 1540, ref(10913, kBuiltin, 0));
    // A TextMesh Pro input field: a text area holding the placeholder and the text.
    scene += ui_object(1600, "Name", {1601, 1609, 1602, 1603});
    scene += rect_transform(1601, 1600, "0.5, 0.5", "0.5, 0.5", "0, -80", "160, 30", "0.5, 0.5",
                            1001, {1611});
    scene += image(1602, 1600, ref(10911, kBuiltin, 0), "{r: 1, g: 1, b: 1, a: 1}", 1);
    scene += script(1603, 1600, kTmpInputCs,
                    selectable(1602) +
                        "  m_TextViewport: {fileID: 1611}\n  m_TextComponent: {fileID: 1632}\n"
                        "  m_Placeholder: {fileID: 1622}\n  m_VerticalScrollbar: {fileID: 0}\n"
                        "  m_ContentType: 0\n  m_InputType: 0\n  m_KeyboardType: 0\n"
                        "  m_LineType: 0\n  m_CharacterValidation: 0\n  m_CharacterLimit: 12\n" +
                        ui_call("m_OnEndEdit", "SetName", 0) +
                        "  m_CaretBlinkRate: 0.85\n  m_Text: Mira\n  m_ReadOnly: 0\n");
    scene += ui_object(1610, "Text Area", {1611, 1619, 1612});
    scene += rect_transform(1611, 1610, "0, 0", "1, 1", "0, -0.5", "-20, -13", "0.5, 0.5", 1601,
                            {1621, 1631});
    scene += script(1612, 1610, kRectMaskCs, "  m_Padding: {x: -8, y: -5, z: -8, w: -5}\n");
    scene += ui_object(1620, "Placeholder", {1621, 1629, 1622});
    scene += rect_transform(1621, 1620, "0, 0", "1, 1", "0, 0", "0, 0", "0.5, 0.5", 1611);
    scene += tmp_text(1622, 1620, "Your name", "{r: 0.2, g: 0.2, b: 0.2, a: 0.5}",
                      "  m_fontAsset: " + ref(11400000, kPondSdf, 2) +
                          "\n  m_fontSize: 14\n  m_fontStyle: 0\n  m_HorizontalAlignment: 1\n"
                          "  m_VerticalAlignment: 512\n  m_TextWrappingMode: 0\n");
    scene += ui_object(1630, "Text", {1631, 1639, 1632});
    scene += rect_transform(1631, 1630, "0, 0", "1, 1", "0, 0", "0, 0", "0.5, 0.5", 1611);
    scene += tmp_text(1632, 1630, "Mira", "{r: 0.2, g: 0.2, b: 0.2, a: 1}",
                      "  m_fontAsset: " + ref(11400000, kPondSdf, 2) +
                          "\n  m_fontSize: 14\n  m_fontStyle: 0\n  m_HorizontalAlignment: 1\n"
                          "  m_VerticalAlignment: 512\n  m_TextWrappingMode: 0\n");
    // A scroll view down the right edge: a masked viewport, a content that a vertical layout
    // group lays out and a fitter sizes, and a scroll bar that hides when it isn't needed.
    scene += ui_object(1700, "List", {1701, 1709, 1702, 1703});
    scene += rect_transform(1701, 1700, "1, 0", "1, 1", "-10, 0", "120, -20", "1, 0.5", 1001,
                            {1711, 1751});
    scene += image(1702, 1700, ref(10907, kBuiltin, 0), "{r: 1, g: 1, b: 1, a: 0.4}", 1);
    scene += script(1703, 1700, kScrollRectCs,
                    "  m_Content: {fileID: 1721}\n  m_Horizontal: 0\n  m_Vertical: 1\n"
                    "  m_MovementType: 1\n  m_Elasticity: 0.1\n  m_Inertia: 1\n"
                    "  m_ScrollSensitivity: 1\n  m_Viewport: {fileID: 1711}\n"
                    "  m_HorizontalScrollbar: {fileID: 0}\n"
                    "  m_VerticalScrollbar: {fileID: 1753}\n"
                    "  m_HorizontalScrollbarVisibility: 2\n  m_VerticalScrollbarVisibility: 2\n");
    scene += ui_object(1710, "Viewport", {1711, 1719, 1712, 1713});
    scene += rect_transform(1711, 1710, "0, 0", "1, 1", "0, 0", "-17, 0", "0, 1", 1701, {1721});
    scene += image(1712, 1710, ref(10917, kBuiltin, 0), "{r: 1, g: 1, b: 1, a: 1}", 1);
    scene += script(1713, 1710, kMaskCs, "  m_ShowMaskGraphic: 0\n");
    scene += ui_object(1720, "Content", {1721, 1729, 1722, 1723});
    scene +=
        rect_transform(1721, 1720, "0, 1", "1, 1", "0, 0", "0, 300", "0, 1", 1711, {1731, 1741});
    scene += script(1722, 1720, kVerticalLayoutCs,
                    "  m_Padding:\n    m_Left: 4\n    m_Right: 4\n    m_Top: 4\n    m_Bottom: 4\n"
                    "  m_ChildAlignment: 0\n  m_Spacing: 2\n  m_ChildForceExpandWidth: 1\n"
                    "  m_ChildForceExpandHeight: 0\n  m_ChildControlWidth: 1\n"
                    "  m_ChildControlHeight: 0\n  m_ChildScaleWidth: 0\n"
                    "  m_ChildScaleHeight: 0\n  m_ReverseArrangement: 0\n");
    scene += script(1723, 1720, kFitterCs, "  m_HorizontalFit: 0\n  m_VerticalFit: 2\n");
    for (const auto& [object, name] :
         std::initializer_list<std::pair<std::int64_t, std::string_view>>{{1730, "Lily"},
                                                                          {1740, "Reed"}}) {
        scene += ui_object(object, name, {object + 1, object + 9, object + 2});
        scene +=
            rect_transform(object + 1, object, "0, 1", "0, 1", "0, 0", "100, 24", "0.5, 0.5", 1721);
        scene += legacy_text(object + 2, object, name, 14, 3);
    }
    scene += ui_object(1750, "Scrollbar Vertical", {1751, 1759, 1752, 1753});
    scene += rect_transform(1751, 1750, "1, 0", "1, 1", "0, 0", "20, 0", "1, 1", 1701, {1761});
    scene += image(1752, 1750, ref(10907, kBuiltin, 0), "{r: 1, g: 1, b: 1, a: 1}", 1);
    scene +=
        script(1753, 1750, kScrollbarCs,
               selectable(1772) + "  m_HandleRect: {fileID: 1771}\n  m_Direction: 2\n  m_Value: 1\n"
                                  "  m_Size: 1\n  m_NumberOfSteps: 0\n");
    scene += ui_object(1760, "Sliding Area", {1761, 1769});
    scene +=
        rect_transform(1761, 1760, "0, 0", "1, 1", "0, 0", "-20, -20", "0.5, 0.5", 1751, {1771});
    scene += ui_object(1770, "Handle", {1771, 1779, 1772});
    scene += rect_transform(1771, 1770, "0, 0", "1, 1", "0, 0", "20, 20", "0.5, 0.5", 1761);
    scene += image(1772, 1770, ref(10905, kBuiltin, 0), "{r: 1, g: 1, b: 1, a: 1}", 1);
    // A grid of two columns of icons in the bottom left corner.
    scene += ui_object(1800, "Grid", {1801, 1809, 1802});
    scene += rect_transform(1801, 1800, "0, 0", "0, 0", "10, 10", "100, 100", "0, 0", 1001,
                            {1811, 1821, 1831});
    scene += script(1802, 1800, kGridLayoutCs,
                    "  m_Padding:\n    m_Left: 0\n    m_Right: 0\n    m_Top: 0\n    m_Bottom: 0\n"
                    "  m_ChildAlignment: 0\n  m_StartCorner: 0\n  m_StartAxis: 0\n"
                    "  m_CellSize: {x: 32, y: 32}\n  m_Spacing: {x: 4, y: 4}\n"
                    "  m_Constraint: 1\n  m_ConstraintCount: 2\n");
    for (const auto& [object, name] :
         std::initializer_list<std::pair<std::int64_t, std::string_view>>{
             {1810, "Gem"}, {1820, "Key"}, {1830, "Map"}}) {
        scene += ui_object(object, name, {object + 1, object + 9, object + 2});
        scene +=
            rect_transform(object + 1, object, "0, 1", "0, 1", "0, 0", "32, 32", "0.5, 0.5", 1801);
        scene += image(object + 2, object, ref(21300000, kIconPng));
    }
    // A raw image at the top left, scaled up by half.
    scene += ui_object(1900, "Logo", {1901, 1909, 1902});
    scene += rect_transform(1901, 1900, "0, 1", "0, 1", "8, -8", "64, 32", "0, 1", 1001, {},
                            "1.5, y: 1.5");
    scene += script(1902, 1900, kRawImageCs,
                    "  m_Material: {fileID: 0}\n  m_Color: {r: 1, g: 1, b: 1, a: 1}\n"
                    "  m_RaycastTarget: 0\n  m_Texture: " +
                        ref(2800000, kIconPng) +
                        "\n  m_UVRect:\n    serializedVersion: 2\n    x: 0\n    y: 0\n"
                        "    width: 1\n    height: 1\n");
    // A hidden panel of options, faded by a CanvasGroup.
    scene += ui_object(2000, "Options", {2001, 2009, 2002, 2003}, false);
    scene +=
        rect_transform(2001, 2000, "0.5, 0.5", "0.5, 0.5", "0, 0", "200, 150", "0.5, 0.5", 1001);
    scene += image(2002, 2000, "{fileID: 0}", "{r: 0.1, g: 0.2, b: 0.3, a: 1}");
    scene += component(225, "CanvasGroup", 2003, 2000,
                       "  m_Alpha: 0.5\n  m_Interactable: 1\n  m_BlocksRaycasts: 1\n"
                       "  m_IgnoreParentGroups: 0\n");
    // The badge, moved to the top right of the canvas.
    scene += instance(2100, kBadgePrefab, 1001,
                      modification(100, kBadgePrefab, "m_Name", "Badge") +
                          modification(101, kBadgePrefab, "m_AnchoredPosition.x", "150") +
                          modification(101, kBadgePrefab, "m_AnchoredPosition.y", "120"));
    scene += stripped(224, "RectTransform", brought(2100, 101), 101, kBadgePrefab, 2100);
    // A sign in the world with a canvas of its own in world space.
    scene += game_object(2200, "Sign", {2201});
    scene += transform(2201, 2200, "{x: 3, y: 1, z: 0}", 0, {2211});
    scene += ui_object(2210, "Board", {2211, 2219, 2212, 2213});
    scene += rect_transform(2211, 2210, "0.5, 0.5", "0.5, 0.5", "0, 0", "80, 30", "0.5, 0.5", 2201);
    scene += component(223, "Canvas", 2212, 2210,
                       "  serializedVersion: 3\n  m_RenderMode: 2\n  m_Camera: {fileID: 0}\n"
                       "  m_PixelPerfect: 0\n  m_OverrideSorting: 0\n  m_SortingOrder: 0\n");
    scene += legacy_text(2213, 2210, "Exit", 14, 4);
    // The event system, which the gui system stands in for.
    scene += game_object(3000, "EventSystem", {3001, 3002});
    scene += transform(3001, 3000, "{x: 0, y: 0, z: 0}");
    scene += script(3002, 3000, kEventSystemCs,
                    "  m_FirstSelected: {fileID: 0}\n  m_sendNavigationEvents: 1\n");
    scene += "--- !u!1660057539 &9223372036854775807\nSceneRoots:\n  m_ObjectHideFlags: 0\n"
             "  m_Roots:\n  - {fileID: 1001}\n  - {fileID: 2201}\n  - {fileID: 3001}\n";
    write(folder / "Assets/Scenes/Menu.unity", scene);
    write(folder / "Assets/Scenes/Menu.unity.meta", meta("a1000000000000000000000000000025"));
}

constexpr std::string_view kDropdownCs = "0d0b652f32a2cc243917e4028fa0f046";
constexpr std::string_view kTmpDropdownCs = "7b743370ac3e4ec2a1668f5455a8ef8a";

// An options screen beside the menu: a canvas holding Unity's default dropdown, which calls
// Menu.SetQuality, and a TextMesh Pro dropdown that can't be used, with a placeholder and an
// arrow of the project's own.
void write_options(const fs::path& folder) {
    std::string scene(kHeader);
    scene += ui_object(5000, "Canvas", {5001, 5009, 5002, 1005});
    scene += rect_transform(5001, 5000, "0, 0", "0, 0", "0, 0", "0, 0", "0, 0", 0, {5101, 5201});
    scene += component(223, "Canvas", 5002, 5000,
                       "  serializedVersion: 3\n  m_RenderMode: 0\n  m_Camera: {fileID: 0}\n"
                       "  m_PixelPerfect: 0\n  m_OverrideSorting: 0\n  m_SortingOrder: 0\n");
    scene += script(1005, 5000, kMenuCs, "");
    const auto options =
        [](std::initializer_list<std::pair<std::string_view, std::string_view>> entries) {
            std::string out = "  m_Options:\n    m_Options:\n";
            for (const auto& [text, sprite] : entries) {
                out += "    - m_Text: " + std::string(text) +
                       "\n      m_Image: " + std::string(sprite) + "\n";
            }
            return out;
        };
    // The default dropdown: a caption, Unity's arrow and a hidden template for its list.
    scene += ui_object(5100, "Quality", {5101, 5109, 5102, 5103});
    scene += rect_transform(5101, 5100, "0.5, 0.5", "0.5, 0.5", "0, 40", "160, 30", "0.5, 0.5",
                            5001, {5111, 5121, 5131});
    scene += image(5102, 5100, ref(10905, kBuiltin, 0), "{r: 1, g: 1, b: 1, a: 1}", 1);
    scene += script(5103, 5100, kDropdownCs,
                    selectable(5102) +
                        "  m_Template: {fileID: 5131}\n  m_CaptionText: {fileID: 5112}\n"
                        "  m_CaptionImage: {fileID: 0}\n  m_ItemText: {fileID: 0}\n"
                        "  m_ItemImage: {fileID: 0}\n  m_Value: 1\n" +
                        options({{"Low", "{fileID: 0}"}, {"High", ref(21300000, kIconPng)}}) +
                        ui_call("m_OnValueChanged", "SetQuality", 0));
    scene += ui_object(5110, "Label", {5111, 5119, 5112});
    scene += rect_transform(5111, 5110, "0, 0", "1, 1", "-7.5, -0.5", "-35, -13", "0.5, 0.5", 5101);
    scene += legacy_text(5112, 5110, "High", 14, 3);
    scene += ui_object(5120, "Arrow", {5121, 5129, 5122});
    scene += rect_transform(5121, 5120, "1, 0.5", "1, 0.5", "-15, 0", "20, 20", "0.5, 0.5", 5101);
    scene += image(5122, 5120, ref(10915, kBuiltin, 0));
    scene += ui_object(5130, "Template", {5131, 5139, 5132}, false);
    scene += rect_transform(5131, 5130, "0, 0", "1, 0", "0, 2", "0, 150", "0.5, 1", 5101);
    scene += image(5132, 5130, ref(10905, kBuiltin, 0), "{r: 1, g: 1, b: 1, a: 1}", 1);
    // The TextMesh Pro dropdown, its value past its options, drawn in the project's sprites.
    std::string locked = selectable(5202);
    locked.replace(locked.find("m_Interactable: 1"), 17, "m_Interactable: 0");
    scene += ui_object(5200, "Mode", {5201, 5209, 5202, 5203});
    scene += rect_transform(5201, 5200, "0.5, 0.5", "0.5, 0.5", "0, -10", "160, 30", "0.5, 0.5",
                            5001, {5211, 5221, 5231, 5241});
    scene += image(5202, 5200, ref(21300000, kButtonPng), "{r: 1, g: 1, b: 1, a: 1}", 1);
    scene += script(5203, 5200, kTmpDropdownCs,
                    locked +
                        "  m_Template: {fileID: 5241}\n  m_CaptionText: {fileID: 5212}\n"
                        "  m_CaptionImage: {fileID: 0}\n  m_Placeholder: {fileID: 5222}\n"
                        "  m_ItemText: {fileID: 0}\n  m_ItemImage: {fileID: 0}\n  m_Value: 5\n" +
                        options({{"Easy", "{fileID: 0}"}, {"Hard", "{fileID: 0}"}}));
    scene += ui_object(5210, "Label", {5211, 5219, 5212});
    scene += rect_transform(5211, 5210, "0, 0", "1, 1", "-7.5, -0.5", "-35, -13", "0.5, 0.5", 5201);
    scene += tmp_text(5212, 5210, "Hard", "{r: 0.2, g: 0.2, b: 0.2, a: 1}",
                      "  m_fontSize: 14\n  m_fontStyle: 0\n  m_HorizontalAlignment: 1\n"
                      "  m_VerticalAlignment: 512\n  m_TextWrappingMode: 0\n");
    scene += ui_object(5220, "Placeholder", {5221, 5229, 5222}, false);
    scene += rect_transform(5221, 5220, "0, 0", "1, 1", "-7.5, -0.5", "-35, -13", "0.5, 0.5", 5201);
    scene += tmp_text(5222, 5220, "Pick one", "{r: 0.2, g: 0.2, b: 0.2, a: 0.5}",
                      "  m_fontSize: 14\n  m_fontStyle: 2\n  m_HorizontalAlignment: 1\n"
                      "  m_VerticalAlignment: 512\n  m_TextWrappingMode: 0\n");
    scene += ui_object(5230, "Arrow", {5231, 5239, 5232});
    scene += rect_transform(5231, 5230, "1, 0.5", "1, 0.5", "-15, 0", "20, 20", "0.5, 0.5", 5201);
    scene += image(5232, 5230, ref(21300000, kIconPng));
    scene += ui_object(5240, "Template", {5241, 5249, 5242}, false);
    scene += rect_transform(5241, 5240, "0, 0", "1, 0", "0, 2", "0, 150", "0.5, 1", 5201);
    scene += image(5242, 5240, ref(10905, kBuiltin, 0), "{r: 1, g: 1, b: 1, a: 1}", 1);
    scene += "--- !u!1660057539 &9223372036854775807\nSceneRoots:\n  m_ObjectHideFlags: 0\n"
             "  m_Roots:\n  - {fileID: 5001}\n";
    write(folder / "Assets/Scenes/Options.unity", scene);
    write(folder / "Assets/Scenes/Options.unity.meta", meta("a1000000000000000000000000000026"));
}

constexpr std::string_view kLight2DCs = "073797afb82c5a1438f328866b10b3f0";
constexpr std::string_view kShadowCasterCs = "7db70e0ea77f5ac47a8f4565a9406397";
constexpr std::string_view kCompositeCasterCs = "38f48a8d66b2adc46887f23a3a88ac01";
constexpr std::string_view kGlowPng = "a3000000000000000000000000000001";
constexpr std::string_view kGlowNormalPng = "a3000000000000000000000000000002";

// A Light2D of URP's 2D renderer, saved by URP 12 and later, lighting the sorting layers whose
// IDs layers lists as Unity writes them: eight hex digits each, little-endian.
std::string light_2d(std::int64_t object, std::int64_t owner, int type, std::string_view fields,
                     std::string_view layers = "00000000") {
    return script(object, owner, kLight2DCs,
                  "  m_ComponentVersion: 4\n  m_LightType: " + std::to_string(type) +
                      "\n  m_ApplyToSortingLayers: " + std::string(layers) + "\n" +
                      std::string(fields));
}

std::string shadow_caster(std::int64_t object, std::int64_t owner, std::string_view fields) {
    return script(object, owner, kShadowCasterCs,
                  "  m_ComponentVersion: 6\n  m_HasRenderer: 0\n  m_UseRendererSilhouette: 1\n" +
                      std::string(fields));
}

// A lit scene: a global light, spot lights (one a cone, two on one object), a freeform window,
// a sprite light cut from a sheet, a parametric light, shadow casters with their own shape and
// a collider's, and a sprite with a normal map; on three sorting layers. And a scene with a light
// and no global light.
void write_lights(const fs::path& folder) {
    // The project's tags, with a sorting layer behind the default one and one in front.
    const fs::path tags = folder / "ProjectSettings/TagManager.asset";
    auto tag_manager = slash::platform::read_file(tags.string());
    REQUIRE(tag_manager.ok());
    std::string sorted = std::move(tag_manager).value();
    sorted.replace(sorted.find("  m_SortingLayers:"), std::string::npos,
                   "  m_SortingLayers:\n  - name: Back\n    uniqueID: 111\n    locked: 0\n"
                   "  - name: Default\n    uniqueID: 0\n    locked: 0\n"
                   "  - name: Front\n    uniqueID: 222\n    locked: 0\n");
    write(tags, sorted);
    write_png(folder / "Assets/Art/glow.png", 16, 16);
    write_png(folder / "Assets/Art/glow_n.png", 16, 16);
    write(folder / "Assets/Art/glow.png.meta",
          texture_meta(kGlowPng, 16,
                       "  spriteMode: 1\n  alignment: 0\n  spriteSheet:\n"
                       "    serializedVersion: 2\n    sprites: []\n    secondaryTextures:\n"
                       "    - texture: " +
                           ref(2800000, kGlowNormalPng) + "\n      name: _NormalMap\n"));
    write(folder / "Assets/Art/glow_n.png.meta", texture_meta(kGlowNormalPng, 16, ""));

    const std::string spot = "  m_PointLightInnerRadius: 1\n  m_PointLightOuterRadius: 4\n"
                             "  m_PointLightInnerAngle: 360\n  m_PointLightOuterAngle: 360\n";
    std::string scene(kHeader);
    scene += game_object(100, "Global Light", {101, 102});
    scene += transform(101, 100, "{x: 0, y: 0, z: 0}");
    scene += light_2d(102, 100, 4, "  m_Color: {r: 0.5, g: 0.5, b: 1, a: 1}\n  m_Intensity: 0.8\n",
                      "6f00000000000000de000000");
    // A warm torch on the default layer, its shadows soft and its normal maps used.
    scene += game_object(110, "Torch", {111, 112});
    scene += transform(111, 110, "{x: 2, y: 1, z: 0}");
    scene += light_2d(112, 110, 3,
                      spot + "  m_FalloffIntensity: 0.5\n  m_Color: {r: 1, g: 0.8, b: 0.6, a: 1}\n"
                             "  m_Intensity: 1.5\n  m_ShadowsEnabled: 1\n  m_ShadowIntensity: 1\n"
                             "  m_ShadowSoftness: 0.3\n  m_NormalMapQuality: 1\n"
                             "  m_NormalMapDistance: 2\n");
    // A cone lighting the front layer, volumetric and in another blend style, and a second light
    // on the same object.
    scene += game_object(120, "Beam", {121, 122, 123});
    scene += transform(121, 120, "{x: 6, y: 0, z: 0}");
    scene += light_2d(122, 120, 3,
                      "  m_PointLightInnerRadius: 0\n  m_PointLightOuterRadius: 2\n"
                      "  m_PointLightInnerAngle: 30\n  m_PointLightOuterAngle: 60\n"
                      "  m_FalloffIntensity: 1\n  m_ShadowsEnabled: 0\n  m_LightVolumeEnabled: 1\n"
                      "  m_BlendStyleIndex: 1\n",
                      "de000000");
    scene += light_2d(123, 120, 3, "  m_NormalMapQuality: 2\n  m_ShadowsEnabled: 0\n");
    // A freeform window, a square two units wide to the right of its object, on the back and
    // front layers but not between, with faint shadows.
    scene += game_object(130, "Window", {131, 132});
    scene += transform(131, 130, "{x: -6, y: -3, z: 0}");
    scene += light_2d(132, 130, 1,
                      "  m_FalloffIntensity: 0.5\n  m_ShapeLightFalloffSize: 0.5\n"
                      "  m_ShadowsEnabled: 1\n  m_ShadowIntensity: 0.5\n"
                      "  m_ShapePath:\n  - {x: 0, y: -1, z: 0}\n  - {x: 2, y: -1, z: 0}\n"
                      "  - {x: 2, y: 1, z: 0}\n  - {x: 0, y: 1, z: 0}\n",
                      "6f000000de000000");
    // A sprite light in the shape of a sheet's sprite, on every layer.
    scene += game_object(140, "Lamp", {141, 142});
    scene += transform(141, 140, "{x: -10, y: 5, z: 0}");
    scene +=
        light_2d(142, 140, 2,
                 "  m_LightCookieSprite: " + ref(21300002, kSheetPng) + "\n  m_ShadowsEnabled: 0\n",
                 "6f00000000000000de000000");
    scene += game_object(150, "Old", {151, 152});
    scene += transform(151, 150, "{x: 0, y: 10, z: 0}");
    scene += light_2d(152, 150, 0, "  m_ShapeLightParametricSides: 5\n");
    // A crate casting a square shadow, its sprite normal mapped.
    scene += game_object(160, "Crate", {161, 162, 163});
    scene += transform(161, 160, "{x: 3, y: -2, z: 0}");
    scene += sprite_renderer(162, 160, ref(21300000, kGlowPng));
    scene += shadow_caster(163, 160,
                           "  m_CastsShadows: 1\n  m_SelfShadows: 0\n  m_ShadowCastingSource: 1\n"
                           "  m_CastingOption: 1\n  m_ShapePath:\n  - {x: -0.5, y: -0.5, z: 0}\n"
                           "  - {x: 0.5, y: -0.5, z: 0}\n  - {x: 0.5, y: 0.5, z: 0}\n"
                           "  - {x: -0.5, y: 0.5, z: 0}\n");
    // A box casting the shape of its collider, offset from its middle.
    scene += game_object(170, "Box", {171, 172, 173});
    scene += transform(171, 170, "{x: 8, y: -2, z: 0}");
    scene += component(
        61, "BoxCollider2D", 172, 170,
        std::string(kCollider).replace(kCollider.find("{x: 0, y: 0}"), 12, "{x: 0.5, y: 0}") +
            "  m_Size: {x: 2, y: 1}\n  m_EdgeRadius: 0\n");
    scene += shadow_caster(173, 170,
                           "  m_ShadowCastingSource: 2\n  m_CastingOption: 1\n"
                           "  m_ShadowShape2DComponent: {fileID: 172}\n  m_ShapePath: []\n");
    // A caster that shadows only itself, and a composite caster.
    scene += game_object(180, "Ghost", {181, 182});
    scene += transform(181, 180, "{x: 0, y: -6, z: 0}");
    scene += shadow_caster(182, 180,
                           "  m_ShadowCastingSource: 1\n  m_CastingOption: 0\n  m_ShapePath:\n"
                           "  - {x: 0, y: 0, z: 0}\n  - {x: 1, y: 0, z: 0}\n"
                           "  - {x: 0, y: 1, z: 0}\n");
    scene += game_object(190, "Group", {191, 192});
    scene += transform(191, 190, "{x: 0, y: 0, z: 0}");
    scene += script(192, 190, kCompositeCasterCs, "");
    scene += "--- !u!1660057539 &9223372036854775807\nSceneRoots:\n  m_ObjectHideFlags: 0\n"
             "  m_Roots:\n  - {fileID: 101}\n  - {fileID: 111}\n  - {fileID: 121}\n"
             "  - {fileID: 131}\n  - {fileID: 141}\n  - {fileID: 151}\n  - {fileID: 161}\n"
             "  - {fileID: 171}\n  - {fileID: 181}\n  - {fileID: 191}\n";
    write(folder / "Assets/Scenes/Lights.unity", scene);
    write(folder / "Assets/Scenes/Lights.unity.meta", meta("a3000000000000000000000000000003"));

    // Saved before URP versioned its lights: shadows on by their strength, normal maps by a flag.
    std::string night(kHeader);
    night += game_object(200, "Moon", {201, 202});
    night += transform(201, 200, "{x: 0, y: 0, z: 0}");
    night += script(202, 200, kLight2DCs,
                    "  m_LightType: 3\n  m_ApplyToSortingLayers: 00000000\n"
                    "  m_ShadowIntensity: 0\n  m_UseNormalMap: 1\n");
    write(folder / "Assets/Scenes/Night.unity", night);
    write(folder / "Assets/Scenes/Night.unity.meta", meta("a3000000000000000000000000000004"));
}

// --- A 3D project: "Hall", saved by Unity 2022.3 ---

constexpr std::string_view kBuiltinExtra = "0000000000000000e000000000000000";
constexpr std::string_view kBuiltinSprites = "0000000000000000f000000000000000";
constexpr std::string_view kWoodPng = "a4000000000000000000000000000001";
constexpr std::string_view kWoodMat = "a4000000000000000000000000000002";
constexpr std::string_view kStoneMat = "a4000000000000000000000000000003";
constexpr std::string_view kTreeGlb = "a4000000000000000000000000000004";
constexpr std::string_view kStepsOgg = "a4000000000000000000000000000005";
constexpr std::string_view kTreeRoot = "-8679921383154817045"; // a model's top Transform
constexpr std::string_view kTreeObject = "919132149155446097"; // and its top GameObject

std::string transform_3d(std::int64_t object, std::int64_t owner, std::string_view position,
                         std::string_view rotation, std::string_view scale, std::int64_t father = 0,
                         std::initializer_list<std::int64_t> children = {}) {
    std::string out = "--- !u!4 &" + id(object) + "\nTransform:\n" + std::string(kSource) +
                      "  m_GameObject: {fileID: " + id(owner) + "}\n  serializedVersion: 2\n" +
                      "  m_LocalRotation: " + std::string(rotation) + "\n" +
                      "  m_LocalPosition: " + std::string(position) + "\n" +
                      "  m_LocalScale: " + std::string(scale) + "\n";
    if (children.size() == 0) {
        out += "  m_Children: []\n";
    } else {
        out += "  m_Children:\n";
        for (const std::int64_t child : children) {
            out += "  - {fileID: " + id(child) + "}\n";
        }
    }
    return out + "  m_Father: {fileID: " + id(father) + "}\n";
}

// A change an instance of the tree model makes to one of its objects, by its ID in the model.
std::string model_change(std::string_view target, std::string_view path, std::string_view value,
                         std::string_view reference = "{fileID: 0}") {
    return "    - target: {fileID: " + std::string(target) + ", guid: " + std::string(kTreeGlb) +
           ",\n        type: 3}\n      propertyPath: " + std::string(path) +
           "\n      value: " + std::string(value) +
           "\n      objectReference: " + std::string(reference) + "\n";
}

constexpr std::string_view kNoTurn = "{x: 0, y: 0, z: 0, w: 1}";
constexpr std::string_view kOne = "{x: 1, y: 1, z: 1}";

std::string mesh_filter(std::int64_t object, std::int64_t owner, int mesh) {
    return "--- !u!33 &" + id(object) + "\nMeshFilter:\n" + std::string(kSource) +
           "  m_GameObject: {fileID: " + id(owner) + "}\n  m_Mesh: " + ref(mesh, kBuiltinExtra, 0) +
           "\n";
}

std::string mesh_renderer(std::int64_t object, std::int64_t owner, std::string_view material) {
    return component(23, "MeshRenderer", object, owner,
                     "  m_CastShadows: 1\n  m_ReceiveShadows: 1\n  m_Materials:\n  - " +
                         std::string(material) + "\n");
}

std::string material_file(std::string_view name, std::string_view shader, std::string_view keywords,
                          std::string_view textures, std::string_view floats,
                          std::string_view colors) {
    return std::string(kHeader) + "--- !u!21 &2100000\nMaterial:\n  serializedVersion: 8\n" +
           std::string(kSource) + "  m_Name: " + std::string(name) +
           "\n  m_Shader: " + std::string(shader) +
           "\n  m_ShaderKeywords: " + std::string(keywords) +
           "\n  m_SavedProperties:\n    serializedVersion: 3\n    m_TexEnvs:" +
           std::string(textures) + "\n    m_Floats:" + std::string(floats) +
           "\n    m_Colors:" + std::string(colors) + "\n";
}

void write_hall(const fs::path& folder) {
    write(folder / "ProjectSettings/ProjectVersion.txt", "m_EditorVersion: 2022.3.20f1\n");
    write(folder / "ProjectSettings/ProjectSettings.asset",
          std::string(kHeader) + "--- !u!129 &1\nPlayerSettings:\n  productName: Hall\n");
    std::string matrix;
    for (int layer = 0; layer < 32; ++layer) {
        matrix += "ffffffff";
    }
    write(folder / "ProjectSettings/DynamicsManager.asset",
          std::string(kHeader) + "--- !u!55 &1\nPhysicsManager:\n  m_ObjectHideFlags: 0\n" +
              "  serializedVersion: 13\n  m_Gravity: {x: 0, y: -9.81, z: 0}\n" +
              "  m_DefaultMaterial: {fileID: 0}\n  m_LayerCollisionMatrix: " + matrix + "\n");

    write_png(folder / "Assets/Art/wood.png", 8, 8);
    write(folder / "Assets/Art/wood.png.meta",
          meta(kWoodPng, "TextureImporter",
               "  mipmaps:\n    enableMipMap: 1\n  textureSettings:\n    serializedVersion: 2\n"
               "    filterMode: 0\n    wrapU: 0\n  textureType: 0\n"));
    // Unity's Standard shader: a texture tiled twice, smoothness and an HDR emission color.
    write(folder / "Assets/Materials/Wood.mat",
          material_file("Wood", ref(46, kBuiltinSprites, 0), "_EMISSION",
                        "\n    - _MainTex:\n        m_Texture: " + ref(2800000, kWoodPng) +
                            "\n        m_Scale: {x: 2, y: 2}\n        m_Offset: {x: 0, y: 0}\n"
                            "    - _BumpMap:\n        m_Texture: {fileID: 0}\n"
                            "        m_Scale: {x: 1, y: 1}\n        m_Offset: {x: 0, y: 0}",
                        "\n    - _Glossiness: 0.2\n    - _Metallic: 0\n    - _Mode: 0",
                        "\n    - _Color: {r: 0.8, g: 0.6, b: 0.4, a: 1}\n"
                        "    - _EmissionColor: {r: 2, g: 1, b: 0, a: 1}"));
    write(folder / "Assets/Materials/Wood.mat.meta",
          meta(kWoodMat, "NativeFormatImporter", "  mainObjectFileID: 2100000\n"));
    // URP's Lit shader, both sides drawn.
    write(folder / "Assets/Materials/Stone.mat",
          material_file("Stone", ref(4800000, "933532a4fcc9baf4fa0491de14d08ed7"), "", " []",
                        "\n    - _Smoothness: 0.1\n    - _Surface: 0\n    - _Cull: 0",
                        "\n    - _BaseColor: {r: 0.5, g: 0.5, b: 0.5, a: 1}"));
    write(folder / "Assets/Materials/Stone.mat.meta",
          meta(kStoneMat, "NativeFormatImporter", "  mainObjectFileID: 2100000\n"));
    write(folder / "Assets/Models/Tree.glb", "glTF");
    write(folder / "Assets/Models/Tree.glb.meta",
          meta(kTreeGlb, "ModelImporter", "  meshes:\n    globalScale: 1\n    useFileScale: 1\n"));
    write(folder / "Assets/Audio/steps.ogg", "OggS");
    write(folder / "Assets/Audio/steps.ogg.meta", meta(kStepsOgg, "AudioImporter"));
    // A prefab with only a Transform converts as the project does: in 3D.
    write(folder / "Assets/Prefabs/Spawn.prefab", std::string(kHeader) +
                                                      game_object(500, "Spawn", {501}) +
                                                      transform(501, 500, "{x: 1, y: 0, z: 0}"));
    write(folder / "Assets/Prefabs/Spawn.prefab.meta", meta("a4000000000000000000000000000006"));

    const std::string default_material = ref(10303, kBuiltinSprites, 0);
    std::string scene(kHeader);
    scene += "--- !u!104 &2\nRenderSettings:\n  m_ObjectHideFlags: 0\n  serializedVersion: 9\n"
             "  m_Fog: 1\n  m_FogColor: {r: 0.6, g: 0.7, b: 0.8, a: 1}\n  m_FogMode: 2\n"
             "  m_FogDensity: 0.02\n  m_LinearFogStart: 0\n  m_LinearFogEnd: 300\n"
             "  m_AmbientSkyColor: {r: 0.212, g: 0.227, b: 0.259, a: 1}\n"
             "  m_AmbientIntensity: 1\n  m_AmbientMode: 0\n  m_SkyboxMaterial: " +
             ref(10304, kBuiltinSprites, 0) + "\n";
    // The camera, at -10 looking along +z, with a cube held in front of it.
    scene += game_object(10, "Main Camera", {11, 12, 13}, 0,
                         "  m_TagString: MainCamera\n  m_IsActive: 1\n");
    scene += transform_3d(11, 10, "{x: 1, y: 2, z: -10}", kNoTurn, kOne, 0, {16});
    scene += component(20, "Camera", 12, 10,
                       "  m_ClearFlags: 1\n  m_BackGroundColor: {r: 0.2, g: 0.3, b: 0.5, a: 0}\n"
                       "  m_NormalizedViewPortRect:\n    serializedVersion: 2\n    x: 0\n"
                       "    y: 0\n    width: 1\n    height: 1\n  near clip plane: 0.1\n"
                       "  far clip plane: 500\n  field of view: 50\n  orthographic: 0\n"
                       "  orthographic size: 5\n  m_CullingMask:\n    serializedVersion: 2\n"
                       "    m_Bits: 4294967295\n  m_TargetTexture: {fileID: 0}\n");
    scene += component(81, "AudioListener", 13, 10, "");
    scene += game_object(15, "Gun", {16, 17, 18});
    scene += transform_3d(16, 15, "{x: 0.5, y: -0.5, z: 1}", kNoTurn, kOne, 11);
    scene += mesh_filter(17, 15, 10202);
    scene += mesh_renderer(18, 15, default_material);
    // The sun: Unity's Euler angles (50, -30, 0), soft shadows.
    scene += game_object(30, "Sun", {31, 32});
    scene += transform_3d(31, 30, "{x: 0, y: 3, z: 0}",
                          "{x: 0.40821788, y: -0.23456968, z: 0.10938163, w: 0.8754261}", kOne);
    scene += component(108, "Light", 32, 30,
                       "  serializedVersion: 10\n  m_Type: 1\n"
                       "  m_Color: {r: 1, g: 0.95, b: 0.85, a: 1}\n  m_Intensity: 1.2\n"
                       "  m_Range: 10\n  m_SpotAngle: 30\n  m_Shadows:\n    m_Type: 2\n"
                       "    m_Strength: 0.8\n  m_Cookie: {fileID: 0}\n  m_Lightmapping: 4\n"
                       "  m_CullingMask:\n    serializedVersion: 2\n    m_Bits: 4294967295\n");
    // A crate turned 30 degrees, wooden, falling, with a label on its front.
    scene += game_object(40, "Crate", {41, 42, 43, 44, 45});
    scene += transform_3d(41, 40, "{x: 3, y: 0.5, z: 2}",
                          "{x: 0, y: 0.25881904, z: 0, w: 0.9659258}", kOne, 0, {47});
    scene += mesh_filter(42, 40, 10202);
    scene += mesh_renderer(43, 40, ref(2100000, kWoodMat, 2));
    scene += component(65, "BoxCollider", 44, 40,
                       "  m_Material: {fileID: 0}\n  m_IsTrigger: 0\n  serializedVersion: 3\n"
                       "  m_Size: {x: 1, y: 1, z: 1}\n  m_Center: {x: 0, y: 0, z: 0}\n");
    scene += "--- !u!54 &45\nRigidbody:\n" + std::string(kSource) +
             "  m_GameObject: {fileID: 40}\n  serializedVersion: 4\n  m_Mass: 2\n"
             "  m_Drag: 0.1\n  m_AngularDrag: 0.05\n  m_UseGravity: 1\n  m_IsKinematic: 0\n"
             "  m_Interpolate: 0\n  m_Constraints: 80\n  m_CollisionDetection: 0\n";
    scene += game_object(46, "Label", {47, 48, 49});
    scene += transform_3d(47, 46, "{x: 0, y: 0, z: -0.51}", kNoTurn, kOne, 41);
    scene += mesh_filter(48, 46, 10210);
    scene += mesh_renderer(49, 46, default_material);
    // The floor: a plane twice its size, of stone, its mesh its collider.
    scene += game_object(50, "Floor", {51, 52, 53, 54});
    scene += transform_3d(51, 50, "{x: 0, y: 0, z: 0}", kNoTurn, "{x: 2, y: 1, z: 2}");
    scene += mesh_filter(52, 50, 10209);
    scene += mesh_renderer(53, 50, ref(2100000, kStoneMat, 2));
    scene += component(64, "MeshCollider", 54, 50,
                       "  m_Material: {fileID: 0}\n  m_IsTrigger: 0\n  serializedVersion: 5\n"
                       "  m_Convex: 0\n  m_Mesh: " +
                           ref(10209, kBuiltinExtra, 0) + "\n");
    // A lamp, and a spot light switched off.
    scene += game_object(60, "Lamp", {61, 62});
    scene += transform_3d(61, 60, "{x: 0, y: 3, z: 0}", kNoTurn, kOne);
    scene += component(108, "Light", 62, 60,
                       "  m_Type: 2\n  m_Color: {r: 1, g: 0.8, b: 0.6, a: 1}\n"
                       "  m_Intensity: 2\n  m_Range: 8\n  m_Shadows:\n    m_Type: 0\n");
    scene += game_object(65, "Spot", {66, 67});
    scene += transform_3d(66, 65, "{x: 2, y: 4, z: 0}", kNoTurn, kOne);
    scene += "--- !u!108 &67\nLight:\n" + std::string(kSource) +
             "  m_GameObject: {fileID: 65}\n  m_Enabled: 0\n  m_Type: 0\n"
             "  m_Color: {r: 1, g: 1, b: 1, a: 1}\n  m_Intensity: 1\n  m_Range: 12\n"
             "  m_SpotAngle: 40\n";
    // The player: a character controller with footsteps around it.
    scene += game_object(70, "Player", {71, 72, 73});
    scene += transform_3d(71, 70, "{x: 0, y: 0, z: -3}", kNoTurn, kOne);
    scene += component(143, "CharacterController", 72, 70,
                       "  m_Material: {fileID: 0}\n  m_IsTrigger: 0\n  serializedVersion: 3\n"
                       "  m_Height: 1.8\n  m_Radius: 0.4\n  m_SlopeLimit: 50\n"
                       "  m_StepOffset: 0.3\n  m_SkinWidth: 0.05\n"
                       "  m_MinMoveDistance: 0.001\n  m_Center: {x: 0, y: 0.9, z: 0}\n");
    scene += component(
        82, "AudioSource", 73, 70,
        "  OutputAudioMixerGroup: {fileID: 0}\n  m_audioClip: " + ref(8300000, kStepsOgg) +
            "\n  m_PlayOnAwake: 1\n  m_Volume: 1\n  m_Pitch: 1\n  Loop: 0\n"
            "  Mute: 0\n  MinDistance: 2\n  MaxDistance: 30\n  rolloffMode: 1\n"
            "  panLevelCustomCurve:\n    serializedVersion: 2\n    m_Curve:\n"
            "    - serializedVersion: 3\n      time: 0\n      value: 1\n");
    // A tree model, its materials replaced with stone, and a bird perched in it.
    scene += instance(100, kTreeGlb, 0,
                      model_change(kTreeRoot, "m_LocalPosition.x", "-4") +
                          model_change(kTreeRoot, "m_LocalPosition.z", "6") +
                          model_change(kTreeObject, "m_Name", "Oak") +
                          model_change("-7511558181221131132", "m_Materials.Array.data[0]", "",
                                       ref(2100000, kStoneMat, 2)));
    scene += "--- !u!4 &101 stripped\nTransform:\n  m_CorrespondingSourceObject: {fileID: " +
             std::string(kTreeRoot) + ", guid: " + std::string(kTreeGlb) +
             ",\n    type: 3}\n  m_PrefabInstance: {fileID: 100}\n  m_PrefabAsset: {fileID: 0}\n";
    scene += game_object(105, "Bird", {106, 107, 108, 109});
    scene += transform_3d(106, 105, "{x: 1, y: 3, z: 0}", kNoTurn, kOne, 101);
    scene += mesh_filter(107, 105, 10207);
    scene += mesh_renderer(108, 105, default_material);
    scene += component(135, "SphereCollider", 109, 105,
                       "  m_Material: {fileID: 0}\n  m_IsTrigger: 1\n  serializedVersion: 3\n"
                       "  m_Radius: 0.25\n  m_Center: {x: 0, y: 0, z: 0}\n");
    // A rock with two levels of detail: only the first draws.
    scene += game_object(110, "Rock", {111, 112});
    scene += transform_3d(111, 110, "{x: 5, y: 0, z: 0}", kNoTurn, kOne, 0, {121, 131});
    scene += "--- !u!205 &112\nLODGroup:\n" + std::string(kSource) +
             "  m_GameObject: {fileID: 110}\n  serializedVersion: 2\n  m_Size: 1\n"
             "  m_LODs:\n  - screenRelativeHeight: 0.6\n    renderers:\n"
             "    - renderer: {fileID: 123}\n  - screenRelativeHeight: 0.1\n    renderers:\n"
             "    - renderer: {fileID: 133}\n  m_Enabled: 1\n";
    scene += game_object(120, "Rock_LOD0", {121, 122, 123});
    scene += transform_3d(121, 120, "{x: 0, y: 0, z: 0}", kNoTurn, kOne, 111);
    scene += mesh_filter(122, 120, 10207);
    scene += mesh_renderer(123, 120, default_material);
    scene += game_object(130, "Rock_LOD1", {131, 132, 133});
    scene += transform_3d(131, 130, "{x: 0, y: 0, z: 0}", kNoTurn, kOne, 111);
    scene += mesh_filter(132, 130, 10202);
    scene += mesh_renderer(133, 130, default_material);
    // A sprite in a 3D scene doesn't convert.
    scene += game_object(140, "Sign", {141, 142});
    scene += transform_3d(141, 140, "{x: 0, y: 1, z: 4}", kNoTurn, kOne);
    scene += sprite_renderer(142, 140, ref(21300000, kWoodPng));
    scene += "--- !u!1660057539 &9223372036854775807\nSceneRoots:\n  m_ObjectHideFlags: 0\n"
             "  m_Roots:\n  - {fileID: 11}\n  - {fileID: 31}\n  - {fileID: 41}\n"
             "  - {fileID: 51}\n  - {fileID: 61}\n  - {fileID: 66}\n  - {fileID: 71}\n"
             "  - {fileID: 100}\n  - {fileID: 111}\n  - {fileID: 141}\n";
    write(folder / "Assets/Scenes/Hall.unity", scene);
    write(folder / "Assets/Scenes/Hall.unity.meta", meta("a4000000000000000000000000000007"));
}

fs::path make_project(const fs::path& folder) {
    fs::remove_all(folder);
    write_settings(folder);
    write_assets(folder);
    write_cave(folder);
    write_prefabs(folder);
    return folder;
}

// --- A 3D project: "Yard", with meshes picked out of a model ---

constexpr std::string_view kPropsObj = "a5000000000000000000000000000001";
// Unity's IDs for the model's meshes: the barrel's is xxHash64 of
// "Type:Mesh->Barrel_Oak_Banded_LowPoly0", as Unity names a model's objects since 2019.3; the
// lid's an ID from before, which the model's import settings name.
constexpr std::int64_t kBarrelMesh = -8458169340473882849;
constexpr std::int64_t kLidMesh = 4300002;

std::string picked_filter(std::int64_t object, std::int64_t owner, std::int64_t mesh) {
    return "--- !u!33 &" + id(object) + "\nMeshFilter:\n" + std::string(kSource) +
           "  m_GameObject: {fileID: " + id(owner) + "}\n  m_Mesh: " + ref(mesh, kPropsObj) + "\n";
}

std::string mesh_collider(std::int64_t object, std::int64_t owner, std::int64_t mesh, int convex) {
    return component(64, "MeshCollider", object, owner,
                     "  m_Material: {fileID: 0}\n  m_IsTrigger: 0\n  serializedVersion: 5\n"
                     "  m_Convex: " +
                         std::to_string(convex) + "\n  m_Mesh: " + ref(mesh, kPropsObj) + "\n");
}

std::string renderer_of(std::int64_t object, std::int64_t owner,
                        std::initializer_list<std::string_view> materials) {
    std::string list;
    for (const std::string_view material : materials) {
        list += "  - " + std::string(material) + "\n";
    }
    return component(23, "MeshRenderer", object, owner,
                     "  m_CastShadows: 1\n  m_ReceiveShadows: 1\n  m_Materials:\n" + list);
}

void write_yard(const fs::path& folder) {
    write(folder / "ProjectSettings/ProjectVersion.txt", "m_EditorVersion: 2022.3.20f1\n");
    write(folder / "ProjectSettings/ProjectSettings.asset",
          std::string(kHeader) + "--- !u!129 &1\nPlayerSettings:\n  productName: Yard\n");
    write(folder / "Assets/Materials/Wood.mat",
          material_file("Wood", ref(46, kBuiltinSprites, 0), "", " []",
                        "\n    - _Glossiness: 0.2\n    - _Metallic: 0\n    - _Mode: 0",
                        "\n    - _Color: {r: 0.8, g: 0.6, b: 0.4, a: 1}"));
    write(folder / "Assets/Materials/Wood.mat.meta",
          meta(kWoodMat, "NativeFormatImporter", "  mainObjectFileID: 2100000\n"));
    write(folder / "Assets/Materials/Stone.mat",
          material_file("Stone", ref(4800000, "933532a4fcc9baf4fa0491de14d08ed7"), "", " []",
                        "\n    - _Smoothness: 0.1\n    - _Surface: 0",
                        "\n    - _BaseColor: {r: 0.5, g: 0.5, b: 0.5, a: 1}"));
    write(folder / "Assets/Materials/Stone.mat.meta",
          meta(kStoneMat, "NativeFormatImporter", "  mainObjectFileID: 2100000\n"));
    // The model: a barrel's side, and a lid of oak banded with iron, two materials.
    write(folder / "Assets/Models/Props.obj", "mtllib Props.mtl\n"
                                              "o Barrel_Oak_Banded_LowPoly\n"
                                              "v -1 0 1\nv 1 0 1\nv 1 2 1\nv -1 2 1\n"
                                              "vn 0 0 1\nusemtl Oak\nf 1//1 2//1 3//1 4//1\n"
                                              "o Lid\n"
                                              "v -1 2 1\nv 1 2 1\nv 1 2 -1\nv -1 2 -1\n"
                                              "v -1 2.1 1\nv 1 2.1 1\nv 1 2.1 -1\nv -1 2.1 -1\n"
                                              "vn 0 1 0\nusemtl Oak\nf 5//2 6//2 7//2 8//2\n"
                                              "usemtl Iron\nf 9//2 10//2 11//2 12//2\n");
    write(folder / "Assets/Models/Props.mtl",
          "newmtl Oak\nKd 0.6 0.4 0.2\nnewmtl Iron\nKd 0.3 0.3 0.3\n");
    write(folder / "Assets/Models/Props.obj.meta",
          meta(kPropsObj, "ModelImporter",
               "  serializedVersion: 19\n  fileIDToRecycleName:\n    100000: //RootNode\n"
               "    4300002: Lid\n  meshes:\n    globalScale: 0.5\n    useFileScale: 1\n"));

    const std::string wood = ref(2100000, kWoodMat, 2);
    const std::string stone = ref(2100000, kStoneMat, 2);
    std::string scene(kHeader);
    // A barrel of wood, a convex collider of its mesh, and one of stone beside it.
    scene += game_object(10, "Barrel", {11, 12, 13, 14});
    scene += transform_3d(11, 10, "{x: 2, y: 0, z: 0}", kNoTurn, kOne);
    scene += picked_filter(12, 10, kBarrelMesh);
    scene += renderer_of(13, 10, {wood});
    scene += mesh_collider(14, 10, kBarrelMesh, 1);
    scene += game_object(20, "Barrel 2", {21, 22, 23});
    scene += transform_3d(21, 20, "{x: 4, y: 0, z: 0}", kNoTurn, kOne);
    scene += picked_filter(22, 20, kBarrelMesh);
    scene += renderer_of(23, 20, {stone});
    // The lid in wood and stone, its triangles its collider; and with one material, which draws
    // its first part alone.
    scene += game_object(30, "Lid", {31, 32, 33, 34});
    scene += transform_3d(31, 30, "{x: 2, y: 0, z: 0}", kNoTurn, kOne);
    scene += picked_filter(32, 30, kLidMesh);
    scene += renderer_of(33, 30, {wood, stone});
    scene += mesh_collider(34, 30, kLidMesh, 0);
    scene += game_object(40, "Half Lid", {41, 42, 43});
    scene += transform_3d(41, 40, "{x: 4, y: 0, z: 0}", kNoTurn, kOne);
    scene += picked_filter(42, 40, kLidMesh);
    scene += renderer_of(43, 40, {stone});
    // A mesh the model doesn't have.
    scene += game_object(50, "Lost", {51, 52, 53});
    scene += transform_3d(51, 50, "{x: 0, y: 0, z: 0}", kNoTurn, kOne);
    scene += picked_filter(52, 50, 12345);
    scene += renderer_of(53, 50, {wood});
    write(folder / "Assets/Scenes/Yard.unity", scene);
    write(folder / "Assets/Scenes/Yard.unity.meta", meta("a5000000000000000000000000000002"));
}

importer::ImportPlan plan_inside(const fs::path& project) {
    auto plan = importer::plan_import(
        {.source = (project / "unity").string(), .destination = project.string()});
    REQUIRE_MESSAGE(plan.ok(), (plan.ok() ? std::string() : plan.error().message));
    return std::move(plan).value();
}

} // namespace

TEST_CASE("Unity YAML parses as Unity writes it") {
    const auto objects = objects_of(R"(%YAML 1.1
%TAG !u! tag:unity3d.com,2011:
--- !u!1 &100
GameObject:
  m_Component:
  - component: {fileID: 101}
  - component: {fileID: 102}
  m_Name: Long name that goes on
    across a line
  m_Tag: 'it''s quoted'
  m_Empty:
  m_List: []
--- !u!4 &101 stripped
Transform:
  m_CorrespondingSourceObject: {fileID: 4000012345, guid: 0123456789abcdef0123456789abcdef,
    type: 3}
  orthographic size: 5
  m_Curve:
    m_Points:
    - - {x: 0, y: 1}
      - {x: 2, y: -3.5}
    - - {x: 4, y: 5}
  m_Note: |-
    two
    lines
)");
    REQUIRE(objects.size() == 2);
    const unity::Object& go = objects[0];
    CHECK(go.class_id == 1);
    CHECK(go.file_id == 100);
    CHECK(go.type == "GameObject");
    CHECK_FALSE(go.stripped);
    CHECK(go.body.path("m_Component.1.component.fileID")->integer() == 102);
    CHECK(go.body.string_at("m_Name") == "Long name that goes on across a line");
    CHECK(go.body.string_at("m_Tag") == "it's quoted");
    CHECK(go.body.string_at("m_Empty").empty());
    CHECK(go.body.get("m_List")->is_seq());
    CHECK(go.body.get("m_Missing") == nullptr);

    const unity::Object& transform = objects[1];
    CHECK(transform.stripped);
    const auto source = unity::object_ref_at(transform.body, "m_CorrespondingSourceObject");
    REQUIRE(source);
    CHECK(source->file_id == 4000012345);
    CHECK(source->guid == "0123456789abcdef0123456789abcdef");
    CHECK(source->type == 3);
    CHECK(transform.body.number_at("orthographic size") == 5.0);
    CHECK(transform.body.number_at("m_Curve.m_Points.0.1.y") == -3.5);
    CHECK(transform.body.path("m_Curve.m_Points.1")->items.size() == 1);
    CHECK(transform.body.string_at("m_Note") == "two\nlines");

    // .meta files are one plain document; text before the first document is an error.
    auto meta = unity::parse_document("fileFormatVersion: 2\nguid: abc\nTextureImporter:\n"
                                      "  spriteSheet:\n    sprites: []\n");
    REQUIRE(meta.ok());
    CHECK(meta.value().string_at("guid") == "abc");
    CHECK(meta.value().path("TextureImporter.spriteSheet.sprites")->is_seq());
    CHECK_FALSE(unity::parse_file("m_Name: stray\n--- !u!1 &1\nGameObject:\n").ok());
}

TEST_CASE("importing a Unity project converts its settings and input") {
    const fs::path project = "import_unity_settings";
    make_project(project / "unity");
    const importer::ImportPlan plan = plan_inside(project);
    const auto files = files_of(plan);
    const json::Value settings = parse(files.at("project.json"));
    CHECK(slash::runtime::validate_project(settings).empty());
    CHECK(settings.find("name")->as_string() == "Cave Run");
    CHECK(settings.find("version")->as_string() == "0.3.0");
    CHECK(*settings.find("window") == parse(R"({"width": 800, "height": 600})"));
    CHECK(settings.find("tick_hz")->as_double() == 50.0);
    // No scene in the build settings: the first scene is the main one.
    CHECK(settings.find("main_scene")->as_string() == "unity/Assets/Scenes/Cave.scene.json");
    // Units are the pixels per unit most sprites use (16 and 32 tie; the smaller wins), and
    // gravity is scaled by them, pointing down.
    CHECK(plan.report.find("pixels_per_unit")->as_double() == 16.0);
    CHECK(*settings.find("physics_2d") ==
          parse(R"({"gravity": 320.0, "gravity_vector": [0.0, 1.0], "linear_damp": 0.0,
                    "angular_damp": 0.0})"));
    // As many textures filter as pixel art as smoothly, so the default, linear, stays.
    CHECK(*settings.find("rendering") ==
          parse(R"({"clear_color": [0.1, 0.1, 0.2, 1.0], "texture_filter": "linear"})"));

    // The Input Manager's axes: buttons, keys of every kind, and a stick's axis.
    const json::Value& input = *settings.find("input");
    CHECK(*input.find("Fire1") == parse(R"({"events": [{"type": "key", "key": "Ctrl"},
                               {"type": "mouse_button", "button": "left"}]})"));
    CHECK(*input.find("Vertical/negative") == parse(R"({"events": [{"type": "key", "key": "Down"},
                               {"type": "joypad_motion", "axis": "left_y", "axis_value": 1}]})"));
    CHECK(*input.find("Vertical/positive") == parse(R"({"events": [{"type": "key", "key": "Up"},
                               {"type": "joypad_motion", "axis": "left_y", "axis_value": -1}]})"));
    CHECK(*input.find("Dash") ==
          parse(R"({"events": [{"type": "joypad_button", "button": "left_shoulder", "device": 1},
                               {"type": "key", "key": "Kp 5"}]})"));
    CHECK(input.find("Odd") == nullptr);
    // The Input System's actions: a 1D axis, a stick bound to a vector, buttons.
    CHECK(*input.find("Ship/Thrust/negative") ==
          parse(R"({"events": [{"type": "key", "physical_key": "S"}]})"));
    CHECK(*input.find("Ship/Aim/left") ==
          parse(R"({"events": [{"type": "joypad_motion", "axis": "right_x", "axis_value": -1}]})"));
    CHECK(*input.find("Ship/Fire") ==
          parse(R"({"events": [{"type": "mouse_button", "button": "left"},
                               {"type": "key", "physical_key": "Kp 0"}]})"));

    const json::Value report = parse(files.at("import/unity.json"));
    CHECK(unsupported(report, "input button").find("why")->as_string() == "\"joystick 9 axis\"");
    CHECK(unsupported(report, "input interaction").find("why")->as_string() ==
          "Tap: gameplay code times holds and taps");
    CHECK(unsupported(report, "input binding").find("why")->as_string() ==
          "\"<Mouse>/delta\": read the mouse's position and motion in code");
    CHECK(unsupported(report, "main scene").find("where")->as_string() ==
          "ProjectSettings/EditorBuildSettings.asset");
    CHECK(unsupported(report, "file type .psd").find("why")->as_string() ==
          "textures are PNG, JPEG or BMP files; save it as PNG");
    CHECK(*report.find("physics_layers") ==
          parse(R"({"Default": 1, "TransparentFX": 2, "Ignore Raycast": 4, "Water": 16,
                    "UI": 32, "Enemies": 256})"));
    // Editor scripts aren't part of the game, so they get no task.
    CHECK(report.find("scripts")->find("unity/Assets/Editor/Tool.cs") == nullptr);
}

TEST_CASE("importing a Unity scene converts cameras, sprites, bodies and sounds in pixels") {
    const fs::path project = "import_unity_scene";
    make_project(project / "unity");
    const importer::ImportPlan plan = plan_inside(project);
    const auto files = files_of(plan);
    const std::string cave = "unity/Assets/Scenes/Cave.scene.json";
    REQUIRE(files.contains(cave));
    slash::world::ComponentRegistry components;
    slash::world::register_builtin_components(components);
    CHECK(slash::world::validate_scene(components, parse(files.at(cave))).empty());

    const json::Value camera = entity(files, cave, "main_camera");
    CHECK(*camera.find("Transform2D") == parse(R"({"position": [16.0, -32.0]})"));
    CHECK(*camera.find("Camera2D") == parse(R"({"zoom": [5.0, 5.0]})"));
    CHECK(*camera.find("AudioListener2D") == parse(R"({"current": true})"));

    // Unity turns counterclockwise with y up; SlashEngine clockwise with y down.
    const json::Value hero = entity(files, cave, "hero");
    CHECK(*hero.find("Transform2D") ==
          parse(R"({"position": [24.0, 32.0], "rotation": -1.570796, "scale": [2.0, 1.0]})"));
    // Flipped about its pivot, its order clamped to the z_index range, filtered as its texture's
    // import settings say.
    CHECK(*hero.find("Sprite2D") ==
          parse(R"({"texture": "unity/Assets/Art/hero.png", "flip_h": true, "z_index": 4096,
                    "texture_filter": "nearest"})"));
    // Unity's colliders meet every layer by default; bounciness is at most 1.
    CHECK(*hero.find("AnimatableBody2D") ==
          parse(R"({"friction": 0.2, "bounce": 1.0, "collision_mask": 4294967295})"));
    CHECK(*hero.find("CollisionShape2D") ==
          parse(R"({"size": [16.0, 24.0], "position": [0.0, -4.0]})"));

    // A body with children draws its sprite on a child entity, so they don't take its z_index,
    // visibility and color; triggers on a body go on an Area2D of their own.
    const json::Value bat = entity(files, cave, "bat");
    CHECK(*bat.find("Transform2D") ==
          parse(R"({"position": [-48.0, -16.0], "children": ["bat_sprite", "bat_trigger",
                                                             "wing"]})"));
    CHECK(bat.find("Sprite2D") == nullptr);
    CHECK(*bat.find("RigidBody2D") ==
          parse(R"({"mass": 2.0, "gravity_scale": 0.0, "linear_damp": 0.5,
                    "angular_damp": 0.05, "can_sleep": false, "friction": 0.4,
                    "collision_layer": 256, "collision_mask": 4294967039})"));
    CHECK(*bat.find("CollisionShape2D") == parse(R"({"shape": "circle", "radius": 8.0})"));
    // Sheet sprites: regions from the top left, pivots as offsets, and scaled from the sheet's
    // 32 pixels per unit to the project's 16.
    const json::Value bat_sprite = entity(files, cave, "bat_sprite");
    CHECK(*bat_sprite.find("Sprite2D") ==
          parse(R"({"texture": "unity/Assets/Art/sheet.png", "region": [0.0, 0.0, 16.0, 16.0],
                    "offset": [0.0, -8.0]})"));
    CHECK(*bat_sprite.find("Transform2D") == parse(R"({"scale": [0.5, 0.5]})"));
    const json::Value trigger = entity(files, cave, "bat_trigger");
    CHECK(*trigger.find("Area2D") ==
          parse(R"({"collision_layer": 256, "collision_mask": 4294967039})"));
    CHECK(*trigger.find("CollisionShape2D") == parse(R"({"shape": "circle", "radius": 16.0})"));
    // The wing's polygon belongs to the bat's body, its parent.
    const json::Value wing = entity(files, cave, "wing");
    CHECK(wing.find("RigidBody2D") == nullptr);
    CHECK(*wing.find("CollisionPolygon2D") ==
          parse(R"({"polygon": [[0.0, 0.0], [16.0, 0.0], [0.0, -8.0]]})"));
    CHECK(wing.find("Sprite2D") == nullptr);
    CHECK(*entity(files, cave, "wing_sprite").find("Sprite2D") ==
          parse(R"({"texture": "unity/Assets/Art/sheet.png", "region": [16.0, 24.0, 16.0, 8.0],
                    "offset": [8.0, 4.0]})"));

    // An edge of two segments: one on the body, the other on a child entity.
    const json::Value spikes = entity(files, cave, "spikes");
    CHECK(spikes.find("StaticBody2D") != nullptr);
    CHECK(*spikes.find("CollisionShape2D") == parse(R"({"shape": "segment", "b": [16.0, 0.0]})"));
    CHECK(*entity(files, cave, "spikes_shape_2").find("CollisionShape2D") ==
          parse(R"({"shape": "segment", "a": [16.0, 0.0], "b": [32.0, -8.0]})"));

    // An inactive object's sound doesn't play; looping it takes an .audio.json file.
    const json::Value torch = entity(files, cave, "torch");
    CHECK(*torch.find("AudioStreamPlayer") ==
          parse(R"({"stream": "unity/Assets/Audio/hit.loop.audio.json", "volume_db": -12.0412,
                    "pitch_scale": 1.5})"));
    CHECK(parse(files.at("unity/Assets/Audio/hit.loop.audio.json")) ==
          parse(R"({"format": 1, "source": "unity/Assets/Audio/hit.ogg", "loop": true})"));

    // Objects of the same name are numbered in the hierarchy's order.
    CHECK(*entity(files, cave, "rock").find("Transform2D") ==
          parse(R"({"position": [128.0, 0.0]})"));
    CHECK(*entity(files, cave, "rock_2").find("Transform2D") ==
          parse(R"({"position": [144.0, 0.0]})"));

    const json::Value report = parse(files.at("import/unity.json"));
    CHECK(unsupported(report, "component Light").find("why")->as_string() ==
          "a 3D component in a 2D scene; a scene with more 3D components than 2D ones converts in "
          "3D");
    CHECK(unsupported(report, "component Image").find("why")->as_string() ==
          "a UI component outside a Canvas; put it under one");
    CHECK(unsupported(report, "component CinemachineCamera").find("why")->as_string() ==
          "a script from a Unity package");
    CHECK(unsupported(report, "Rigidbody2D frozen position").find("where")->as_string() ==
          "Assets/Scenes/Cave.unity: Bat");
    CHECK(unsupported(report, "sorting order out of range") != nullptr);
    CHECK(unsupported(report, "inactive object").find("where")->as_string() ==
          "Assets/Scenes/Cave.unity: Torch");
    CHECK(report.find("scenes")
              ->find("Assets/Scenes/Cave.unity")
              ->find("entities")
              ->find("Bat/Wing")
              ->as_string() == "wing");
}

TEST_CASE("Unity prefab instances, nested prefabs and variants stay instances") {
    const fs::path project = "import_unity_prefabs";
    make_project(project / "unity");
    const importer::ImportPlan plan = plan_inside(project);
    const auto files = files_of(plan);
    const std::string crate = "unity/Assets/Prefabs/Crate.scene.json";
    const std::string stack = "unity/Assets/Prefabs/Stack.scene.json";
    const std::string big = "unity/Assets/Prefabs/Big Crate.scene.json";
    const std::string yard = "unity/Assets/Scenes/Yard.scene.json";
    CHECK(plan.report.find("converted")->find("prefabs")->as_int() == 3);

    // The crate prefab: a scene with a root.
    const json::Value crate_file = parse(files.at(crate));
    CHECK(crate_file.find("root")->as_string() == "crate");
    CHECK(entity(files, crate, "crate").find("StaticBody2D") != nullptr);
    CHECK(*entity(files, crate, "crate_sprite").find("Sprite2D") ==
          parse(R"({"texture": "unity/Assets/Art/hero.png", "texture_filter": "nearest"})"));

    // The stack holds an instance of the crate, moved up: only the overrides are written. Until
    // the game's code is built, its script's component holds the label's ID as written, so the
    // instance names its own label.
    const json::Value stack_file = parse(files.at(stack));
    CHECK(*stack_file.find("entities")->find("crate") ==
          parse(R"({"extends": "unity/Assets/Prefabs/Crate.scene.json",
                    "components": {"Transform2D": {"position": [0.0, -24.0]},
                                   "Crate": {"label": "crate/label"}}})"));
    CHECK(entity(files, stack, "crate/label") != nullptr);

    // A variant extends the prefab it varies.
    const json::Value big_file = parse(files.at(big));
    CHECK(big_file.find("root")->as_string() == "big_crate");
    CHECK(*big_file.find("entities")->find("big_crate") ==
          parse(R"({"extends": "unity/Assets/Prefabs/Crate.scene.json",
                    "components": {"Transform2D": {"scale": [2.0, 2.0]},
                                   "Crate": {"health": 10, "label": "big_crate/label"}}})"));

    // The yard moves the stack, tints its crate and takes away the crate's collider and label;
    // with no children left, the crate draws its own sprite. The flag added under the stack is
    // the yard's own entity.
    const json::Value stack_entity = entity(files, yard, "stack");
    CHECK(*stack_entity.find("Transform2D") ==
          parse(R"({"position": [32.0, 0.0], "children": ["stack/crate", "flag"]})"));
    const json::Value nested = entity(files, yard, "stack/crate");
    CHECK(nested.find("StaticBody2D") == nullptr);
    CHECK(nested.find("CollisionShape2D") == nullptr);
    CHECK(*nested.find("Sprite2D") ==
          parse(R"({"texture": "unity/Assets/Art/hero.png", "modulate": [0.5, 1.0, 1.0, 1.0],
                    "texture_filter": "nearest"})"));
    CHECK(*nested.find("Transform2D") == parse(R"({"position": [0.0, -24.0]})"));
    CHECK(entity(files, yard, "stack/crate/label").is_null());
    CHECK(entity(files, yard, "stack/crate/crate_sprite").is_null());
    CHECK(*entity(files, yard, "flag").find("Transform2D") ==
          parse(R"({"position": [0.0, -48.0]})"));
    // The big crate's root is itself an instance of the crate, so what the crate brings is
    // "big_crate/crate_sprite" in the variant, and that behind the yard's instance's ID here.
    CHECK(*entity(files, yard, "big_crate").find("Transform2D") ==
          parse(R"({"position": [-64.0, 0.0], "scale": [2.0, 2.0],
                    "children": ["big_crate/big_crate/crate_sprite",
                                 "big_crate/big_crate/label"]})"));
    const json::Value yard_file = parse(files.at(yard));
    CHECK(yard_file.find("entities")->find("stack/crate/label")->is_null());

    // The values scripts were given, references as entity IDs and paths.
    const json::Value report = parse(files.at("import/unity.json"));
    const json::Value& uses =
        *report.find("scripts")->find("unity/Assets/Scripts/Crate.cs")->find("used_by");
    CHECK(uses.as_array().front() ==
          parse(R"({"scene": "unity/Assets/Prefabs/Crate.scene.json", "entity": "crate",
                    "values": {"health": 3, "label": "label", "body": "crate#BoxCollider2D",
                               "sound": "unity/Assets/Audio/hit.ogg",
                               "tint": {"r": 1, "g": 0.5, "b": 0, "a": 1},
                               "loot": ["coin", "gem"], "title": "Crate: wooden"}})"));
    bool flag_seen = false;
    bool big_seen = false;
    for (const json::Value& use : uses.as_array()) {
        if (use.find("entity")->as_string() == "flag") {
            flag_seen = true;
            CHECK(*use.find("values") == parse(R"({"health": 1, "label": "stack", "body": null})"));
        }
        if (use.find("scene")->as_string() == big) {
            big_seen = true;
            CHECK(use.find("values")->find("health")->as_int() == 10);
        }
    }
    CHECK(flag_seen);
    CHECK(big_seen);
}

TEST_CASE("importing a Unity project turns each script into a storyboard task") {
    const fs::path project = "import_unity_scripts";
    make_project(project / "unity");
    const importer::ImportPlan plan = plan_inside(project);
    const auto files = files_of(plan);
    const json::Value work = parse(files.at("storyboard/work.json"));
    CHECK(slash::storyboard::validate_file("work", work).empty());
    const json::Value& tasks = *work.find("entries");
    REQUIRE(tasks.size() == 1);
    const json::Value& task = *tasks.find("task/port-crate");
    CHECK(task.find("name")->as_string() == "Port Assets/Scripts/Crate.cs to C++");
    CHECK(*task.find("tags") == parse(R"(["unity", "import"])"));
    CHECK(task.find("links")->as_array().front().as_string() == "unity/Assets/Scripts/Crate.cs");
    const std::string& text = task.find("text")->as_string();
    CHECK(text.find("Class Crate : MonoBehaviour.") != std::string::npos);
    CHECK(text.find("Fields: int health = 3; Transform label.") != std::string::npos);
    CHECK(text.find("Methods: Hit.") != std::string::npos);
    CHECK(text.find("flag in unity/Assets/Scenes/Yard.scene.json") != std::string::npos);
    CHECK(text.find("Its C++ skeleton is src/imported/crate.cpp: the Crate component and the "
                    "crate system.") != std::string::npos);
    CHECK(*plan.report.find("scripts") ==
          parse(R"([{"script": "unity/Assets/Scripts/Crate.cs", "task": "task/port-crate",
                     "entities": 6, "code": "src/imported/crate.cpp", "component": "Crate"}])"));

    // The script is a C++ skeleton in the gameplay sources, registered by scripts.cpp, which a
    // game module calls; its serialized fields are on the entities that had it, references as
    // entity IDs and fields it doesn't have left out.
    CHECK(*parse(files.at("project.json")).find("gameplay") == parse(R"({"sources": ["src"]})"));
    const std::string& skeleton = files.at("src/imported/crate.cpp");
    CHECK(skeleton.find("struct Crate {\n    int health = 3;\n    entt::entity label = entt::null; "
                        "// Transform\n    int hits{};\n};\nSLASH_COMPONENT(Crate, health, label, "
                        "hits)") != std::string::npos);
    CHECK(skeleton.find("void hit(World& /*world*/, entt::entity /*entity*/, Crate& /*self*/, int "
                        "/*amount*/) {\n    // health -= amount;\n}") != std::string::npos);
    CHECK(files.at("src/imported/scripts.cpp").find("    register_crate(game);\n") !=
          std::string::npos);
    CHECK(files.at("src/game.cpp") == importer::module_source());
    CHECK(plan.report.find("module")->as_string() == "src/game.cpp");
    CHECK(*entity(files, "unity/Assets/Prefabs/Crate.scene.json", "crate").find("Crate") ==
          parse(R"({"health": 3, "label": "label"})"));
    CHECK(*entity(files, "unity/Assets/Scenes/Yard.scene.json", "flag").find("Crate") ==
          parse(R"({"health": 1, "label": "stack"})"));
}

TEST_CASE("a Unity C# script reads into a C++ skeleton that calls its messages as Unity does") {
    const std::string text = R"(using System.Collections;
using System.Collections.Generic;
using UnityEngine;
using UnityEngine.Events;

namespace Pond {

/// <summary>A frog that hops between lily pads.</summary>
[RequireComponent(typeof(Rigidbody2D))]
public class Frog : MonoBehaviour
{
    public enum Mood { Calm, Hungry = 4 }
    public const int MaxLives = 3;
    static int count;

    [Tooltip("Pixels a second.")]
    [Range(0f, 10f)] public float speed = 2.5f;
    [SerializeField] private int lives = MaxLives;
    public Color tint = Color.yellow;
    public Vector2 home = new Vector2(1, -2);
    public List<GameObject> pads = new List<GameObject>();
    [SerializeField] Transform tongue;
    public AudioClip croak;
    public UnityEvent<int> onHop;
    public string title = "Frog", other = "x";
    private float timer;
    [field: SerializeField] public int Score { get; private set; }
    public float Half => speed / 2;
    [System.NonSerialized] public int hidden;

    void Awake()
    {
        count++;
    }

    void Update()
    {
        timer += Time.deltaTime;
        if (timer > 1f) Hop(1);
    }

    public void Hop(int times)
    {
        onHop.Invoke(times);
    }

    void OnTriggerEnter2D(Collider2D other)
    {
        lives--;
    }

    void OnCollisionEnter2D(Collision2D collision) { }

    IEnumerator Blink()
    {
        yield return new WaitForSeconds(0.5f);
    }
}

}
)";
    importer::Script script =
        importer::read_csharp("unity/Assets/Frog.cs", text, importer::ScriptSource::UnityCsharp);
    CHECK(script.source == importer::ScriptSource::UnityCsharp);
    CHECK(script.extends == "MonoBehaviour");
    CHECK(script.class_name == "Frog");
    CHECK(script.component == "Frog");
    CHECK(script.ns == "frog");
    CHECK(script.doc == "A frog that hops between lily pads.");
    CHECK(script.node);
    const auto field = [&](std::string_view name) -> const importer::ScriptField& {
        const auto found = std::ranges::find(script.fields, name, &importer::ScriptField::name);
        REQUIRE_MESSAGE(found != script.fields.end(), name);
        return *found;
    };
    const auto has = [&](std::string_view name) {
        return std::ranges::find(script.fields, name, &importer::ScriptField::name) !=
               script.fields.end();
    };
    CHECK(field("speed").init == " = 2.5F");
    CHECK(field("speed").doc == "Pixels a second.");
    CHECK(field("speed").min == 0.0);
    CHECK(field("speed").max == 10.0);
    CHECK(field("speed").exported);
    CHECK(field("lives").exported);
    CHECK(field("lives").init == " = kMaxLives");
    CHECK(field("tint").type == "slash::Vec4");
    CHECK(field("tint").init == "{1.0F, 0.921569F, 0.0156863F, 1.0F}");
    CHECK(field("home").type == "slash::Vec2");
    CHECK(field("home").init == "{1.0F, -2.0F}");
    CHECK(field("pads").type == "std::vector<entt::entity>");
    CHECK(field("pads").kind == importer::FieldKind::Entities);
    CHECK(field("tongue").kind == importer::FieldKind::Entity);
    CHECK(field("croak").type == "std::string");
    CHECK(field("title").init == R"( = "Frog")");
    CHECK(field("other").init == R"( = "x")");
    CHECK_FALSE(field("timer").exported);
    CHECK(field("score").exported);
    CHECK(field("score").source_name == "<Score>k__BackingField");
    CHECK_FALSE(has("half"));              // computed, so nothing to keep
    CHECK_FALSE(has("count"));             // static
    CHECK_FALSE(field("hidden").exported); // [NonSerialized]: state, not saved
    CHECK_FALSE(has("on_hop"));            // an event
    REQUIRE(script.signals.size() == 1);
    CHECK(script.signals[0].name == "on_hop");
    REQUIRE(script.constants.size() == 1);
    CHECK(script.constants[0].name == "kMaxLives");
    REQUIRE(script.enums.size() == 1);
    CHECK(script.enums[0].values == std::vector<std::string>{"kCalm", "kHungry = 4"});
    const auto function = [&](std::string_view name) -> const importer::ScriptFunction& {
        const auto found =
            std::ranges::find(script.functions, name, &importer::ScriptFunction::source_name);
        REQUIRE_MESSAGE(found != script.functions.end(), name);
        return *found;
    };
    CHECK(function("Update").params.at(0).name == "delta");
    CHECK(function("Update").body.front() == "timer += Time.deltaTime;");
    CHECK(function("Hop").name == "hop");
    CHECK(function("Hop").params.at(0).type == "int");

    script.connections.push_back({.scene = "unity/Assets/Pond.scene.json",
                                  .signal = "pressed",
                                  .from = "button",
                                  .to = "frog",
                                  .method = "Hop",
                                  .argument = "2"});
    const std::string code = importer::skeleton(script);
    MESSAGE(code);
    for (const std::string_view part : {
             std::string_view("SLASH_COMPONENT(Frog, "),
             std::string_view("void emit_on_hop(World& world, entt::entity entity, int "),
             std::string_view("on_trigger_enter2d(world, entity, *self, other);"),
             std::string_view("on_collision_enter2d(world, entity, *self, other);"),
             std::string_view("ecs.all_of<slash::world::Area2D>(first)"),
             std::string_view("update(world, entity, self, tick_seconds);"),
             std::string_view("hop(world, entity, *self, 2);"),
             std::string_view("void hop(World& /*world*/, entt::entity /*entity*/, Frog& /*self*/, "
                              "int /*times*/) {"),
             std::string_view("// timer += Time.deltaTime;"),
             std::string_view(".describe(\"speed\", {.doc = \"Pixels a second.\", .min = 0.0, "
                              ".max = 10.0})"),
         }) {
        CHECK_MESSAGE(code.find(part) != std::string::npos, part);
    }
    // A ScriptableObject is a reflected struct with no system.
    const importer::Script data = importer::read_csharp(
        "Stats.cs", "public class Stats : ScriptableObject { public int power = 2; }",
        importer::ScriptSource::UnityCsharp);
    CHECK_FALSE(data.node);
    CHECK(importer::skeleton(data).find("SLASH_REFLECT(Stats, power)") != std::string::npos);
}

TEST_CASE("a Unity project outside the destination is copied in, and runs") {
    const fs::path source = make_project("import_unity_outside_source");
    const fs::path destination = "import_unity_outside_destination";
    fs::remove_all(destination);
    auto result =
        importer::import_project({.source = source.string(), .destination = destination.string()});
    REQUIRE_MESSAGE(result.ok(), (result.ok() ? std::string() : result.error().message));
    CHECK(result.value().report.find("engine")->as_string() == "unity");
    CHECK(result.value().report.find("main_scene")->as_string() == "Assets/Scenes/Cave.scene.json");
    CHECK(fs::exists(destination / "Assets/Art/hero.png"));
    CHECK(fs::exists(destination / "Assets/Scripts/Crate.cs"));
    CHECK_FALSE(fs::exists(destination / "Assets/Editor/Tool.cs"));
    CHECK_FALSE(fs::exists(destination / "Assets/Scenes/Cave.unity"));
    CHECK(slash::runtime::load_project(destination.string()).ok());

    slash::world::ComponentRegistry components;
    slash::world::register_builtin_components(components);
    command::Registry commands;
    command::register_builtin_commands(commands);
    // The scripts' components are stand-ins until the game's code is built.
    slash::gameplay::Host code(components, slash::gameplay::library_path(destination.string()));
    slash::runtime::add_stand_ins(code, destination.string());
    command::SessionOptions options;
    options.project_root = destination.string();
    options.capture_logs = false;
    options.gameplay = &code;
    command::Session session(components, commands, options);
    for (const std::string_view scene :
         {"Assets/Scenes/Cave.scene.json", "Assets/Scenes/Yard.scene.json"}) {
        json::Value params;
        params["path"] = scene;
        const command::Reply opened = session.call("scene.open", params);
        CHECK_MESSAGE(opened.ok, opened.message);
        CHECK(session.call("step", parse(R"({"ticks": 5})")).ok);
    }
}

TEST_CASE("only Unity projects saved by 2018.3 or later import") {
    const fs::path folder = "import_unity_errors";
    fs::remove_all(folder);
    fs::create_directories(folder / "empty/Assets");
    auto none = importer::plan_import(
        {.source = (folder / "empty").string(), .destination = folder.string()});
    REQUIRE_FALSE(none.ok());
    CHECK(none.error().message.find("Unity") != std::string::npos);

    write(folder / "old/ProjectSettings/ProjectVersion.txt", "m_EditorVersion: 2017.4.40f1\n");
    fs::create_directories(folder / "old/Assets");
    auto old = importer::plan_import(
        {.source = (folder / "old").string(), .destination = folder.string()});
    REQUIRE_FALSE(old.ok());
    CHECK(old.error().message.find("2018.3") != std::string::npos);
}

TEST_CASE("project.import converts a Unity project in the session's project") {
    const fs::path project = "import_unity_command";
    fs::remove_all(project);
    make_project(project / "unity");
    slash::world::ComponentRegistry components;
    slash::world::register_builtin_components(components);
    command::Registry commands;
    command::register_builtin_commands(commands);
    importer::register_import_commands(commands);
    // The game's code, which the import writes: until it is built, its components are stand-ins.
    slash::gameplay::Host code(components, slash::gameplay::library_path(project.string()));
    command::SessionOptions options;
    options.project_root = project.string();
    options.capture_logs = false;
    options.gameplay = &code;
    command::Session session(components, commands, options);

    const command::Reply done = session.call("project.import", parse(R"({"source": "unity"})"));
    REQUIRE_MESSAGE(done.ok, done.message);
    CHECK(done.value.find("engine")->as_string() == "unity");
    CHECK(fs::exists(project / "project.json"));
    CHECK(
        session.call("scene.open", parse(R"({"path": "unity/Assets/Scenes/Yard.scene.json"})")).ok);
    CHECK(session.call("storyboard.check", parse("{}")).value.find("valid")->as_bool());
}

TEST_CASE("Unity tile maps become tile map layers, their sprites a tile set") {
    const fs::path project = "import_unity_tiles";
    fs::remove_all(project);
    make_project(project / "unity");
    write_tiles(project / "unity");
    auto imported = importer::import_project(
        {.source = (project / "unity").string(), .destination = project.string()});
    REQUIRE_MESSAGE(imported.ok(), (imported.ok() ? std::string() : imported.error().message));
    const auto files = files_of(imported.value());
    const std::string level = "unity/Assets/Scenes/Level.scene.json";
    const std::string tiles = "unity/Assets/tiles.tileset.json";
    CHECK(imported.value().report.find("converted")->find("tile_sets")->as_int() == 1);

    // The sheet's sprites fall on a grid, so they are one atlas source. A cell's collider type
    // and color pick an alternative of its tile: the tinted block is alternative 1, the block
    // with no collider alternative 2. The collider's physics material is the physics layer's.
    CHECK(parse(files.at(tiles)) == parse(R"({
        "format": 1,
        "physics_layers": [{"collision_mask": 4294967295, "friction": 0.2, "bounce": 1.0}],
        "sources": {"tiles": {"texture": "unity/Assets/Art/tiles.png",
                              "normal_texture": "unity/Assets/Art/tiles_n.png", "tiles": {
            "0,0": {"name": "ground",
                    "collision": [{"points": [[-8.0, -8.0], [8.0, -8.0], [8.0, 8.0],
                                              [-8.0, 8.0]]}],
                    "alternatives": {
                        "1": {"modulate": [1.0, 0.5, 0.5, 1.0],
                              "collision": [{"points": [[-8.0, -8.0], [8.0, -8.0], [8.0, 8.0],
                                                        [-8.0, 8.0]]}]},
                        "2": {}}},
            "1,0": {"name": "slope",
                    "collision": [{"points": [[-8.0, 8.0], [8.0, 8.0], [8.0, -8.0]]}]},
            "2,0": {"name": "plank"}}}}
    })"));

    // Unity's cell (x, y) is (x, -y - 1) here, rows going down; a flip and a quarter turn
    // counterclockwise are the cells' turns.
    const json::Value ground = entity(files, level, "ground");
    const json::Value& layer = *ground.find("TileMapLayer");
    CHECK(layer.find("tile_set")->as_string() == tiles);
    CHECK(*layer.find("origin") == parse("[-1, -2]"));
    CHECK(layer.find("z_index")->as_int() == 2);
    CHECK(layer.find("collision_enabled") == nullptr);
    std::map<std::pair<int, int>, std::string> cells;
    const json::Value& rows = *layer.find("rows");
    for (std::size_t row = 0; row < rows.size(); ++row) {
        const std::string& keys = rows.as_array()[row].as_string();
        for (std::size_t column = 0; column < keys.size(); ++column) {
            if (keys[column] != '.') {
                cells[{static_cast<int>(column) - 1, static_cast<int>(row) - 2}] =
                    layer.find("legend")->find(std::string(1, keys[column]))->as_string();
            }
        }
    }
    CHECK(cells == std::map<std::pair<int, int>, std::string>{{{0, -1}, "ground"},
                                                              {{1, -1}, "ground:1"},
                                                              {{2, -1}, "slope"},
                                                              {{-1, -1}, "slope:h"},
                                                              {{2, -2}, "slope:vt"},
                                                              {{0, -2}, "plank"},
                                                              {{1, -2}, "ground:2"}});
    CHECK(*entity(files, level, "grid").find("Transform2D") ==
          parse(R"({"children": ["ground"]})"));

    const json::Value report = parse(files.at("import/unity.json"));
    CHECK(
        *report.find("tile_assets") ==
        parse(R"({"unity/Assets/Tiles/Ground.asset": {"tile_set": "unity/Assets/tiles.tileset.json",
                                                        "tile": "ground"},
                    "unity/Assets/Tiles/Slope.asset": {"tile_set": "unity/Assets/tiles.tileset.json",
                                                       "tile": "slope"},
                    "unity/Assets/Tiles/Rule.asset": {"tile_set": "unity/Assets/tiles.tileset.json",
                                                      "tile": "plank"}})"));
    CHECK(
        unsupported(report, "scripted tiles (rule tiles and others)").find("where")->as_string() ==
        "Assets/Scenes/Level.unity: Grid/Ground");

    // The tiles are solid where their polygons are: a ray down through the plank, which has
    // none, stops on the block under it, and one through the flipped slope on its face.
    slash::world::ComponentRegistry components;
    slash::world::register_builtin_components(components);
    command::Registry commands;
    command::register_builtin_commands(commands);
    command::SessionOptions options;
    options.project_root = project.string();
    options.capture_logs = false;
    command::Session session(components, commands, options);
    const command::Reply opened =
        session.call("scene.open", parse(R"({"path": "unity/Assets/Scenes/Level.scene.json"})"));
    REQUIRE_MESSAGE(opened.ok, opened.message);
    REQUIRE(session.call("step", parse(R"({"ticks": 1})")).ok);
    const command::Reply block =
        session.call("physics.raycast", parse(R"({"from": [8, -40], "to": [8, 40]})"));
    REQUIRE_MESSAGE(block.ok, block.message);
    CHECK(*block.value.find("hit")->find("cell") == parse("[0, -1]"));
    CHECK(block.value.find("hit")->find("point")->as_array()[1].as_double() ==
          doctest::Approx(-16.0));
    const command::Reply slope =
        session.call("physics.raycast", parse(R"({"from": [-4, -40], "to": [-4, 40]})"));
    CHECK(*slope.value.find("hit")->find("cell") == parse("[-1, -1]"));
    CHECK(slope.value.find("hit")->find("point")->as_array()[1].as_double() ==
          doctest::Approx(-4.0).epsilon(0.01));
}

TEST_CASE("Unity's isometric and hexagonal tile maps keep their cells where Unity puts them") {
    const fs::path project = "import_unity_shaped_tiles";
    fs::remove_all(project);
    make_project(project / "unity");
    write_shaped_tiles(project / "unity");
    auto imported = importer::import_project(
        {.source = (project / "unity").string(), .destination = project.string()});
    REQUIRE_MESSAGE(imported.ok(), (imported.ok() ? std::string() : imported.error().message));
    const auto files = files_of(imported.value());
    const std::string board = "unity/Assets/Scenes/Board.scene.json";

    // Each layer's cells by key, and where their centers are in the layer's parent: the tile
    // set's place for each cell, moved with the layer.
    struct Layer {
        canvas::TileSet set;
        std::map<std::pair<int, int>, std::string> cells;
        std::vector<std::array<float, 2>> centers;
        json::Value position;
    };
    const auto layer_of = [&](std::string_view id) {
        const json::Value components = entity(files, board, id);
        const json::Value& layer = *components.find("TileMapLayer");
        Layer out;
        const std::string& path = layer.find("tile_set")->as_string();
        REQUIRE_MESSAGE(files.contains(path), path);
        auto set = canvas::parse_tileset(parse(files.at(path)));
        REQUIRE_MESSAGE(set.ok(), (set.ok() ? std::string() : set.error().message));
        out.set = std::move(set).value();
        out.position = *components.find("Transform2D")->find("position");
        const auto& origin = layer.find("origin")->as_array();
        const json::Value& rows = *layer.find("rows");
        for (std::size_t row = 0; row < rows.size(); ++row) {
            const std::string& keys = rows.as_array()[row].as_string();
            for (std::size_t column = 0; column < keys.size(); ++column) {
                if (keys[column] == '.') {
                    continue;
                }
                const int x = static_cast<int>(origin[0].as_int()) + static_cast<int>(column);
                const int y = static_cast<int>(origin[1].as_int()) + static_cast<int>(row);
                out.cells[{x, y}] =
                    layer.find("legend")->find(std::string(1, keys[column]))->as_string();
                const slash::Vec2 center = canvas::map_to_local(out.set, {x, y});
                out.centers.push_back(
                    {center.x + static_cast<float>(out.position.as_array()[0].as_double()),
                     center.y + static_cast<float>(out.position.as_array()[1].as_double())});
            }
        }
        std::ranges::sort(out.centers);
        return out;
    };
    // Where Unity centers cells, in pixels with y down, sorted.
    const auto unity = [](std::vector<std::array<float, 2>> centers) {
        std::ranges::sort(centers);
        return centers;
    };

    // Isometric: Unity's cell (x, y) is (x, -y - 1) in Godot's diamond_right layout, where
    // Unity centers it at ((x - y) * 16, -(x + y + 1) * 8); the layer moves up 8 pixels to
    // put it there, and its marker moves back.
    const Layer iso = layer_of("iso_tiles");
    CHECK(iso.set.shape == canvas::TileShape::Isometric);
    CHECK(iso.set.layout == canvas::TileLayout::DiamondRight);
    CHECK(iso.set.tile_size == slash::Vec2i{32, 16});
    CHECK(iso.position == parse("[0, -8]"));
    CHECK(iso.cells == std::map<std::pair<int, int>, std::string>{
                           {{0, -1}, "iso"}, {{1, -1}, "iso:1"}, {{0, -2}, "iso:h"}});
    CHECK(iso.centers == unity({{0, -8}, {16, -16}, {-16, -16}}));
    CHECK(*entity(files, board, "marker").find("Transform2D")->find("position") ==
          parse("[32, -24]"));
    // The block's bottom middle is at the anchor, the cell's bottom corner, 8 pixels under its
    // center: its texture draws 8 pixels up. Its physics shape moves with it; the cell's outline
    // is a diamond.
    const json::Value iso_set = parse(files.at("unity/Assets/tiles.tileset.json"));
    CHECK(*iso_set.find("sources")->find("iso")->find("tiles")->find("0,0") == parse(R"({
        "name": "iso", "texture_origin": [0, 8],
        "collision": [{"points": [[-16.0, -24.0], [16.0, -24.0], [16.0, 8.0], [-16.0, 8.0]]}],
        "alternatives": {"1": {"texture_origin": [0, 8],
                               "collision": [{"points": [[0.0, -8.0], [16.0, 0.0], [0.0, 8.0],
                                                         [-16.0, 0.0]]}]}}})"));
    const canvas::TileLook look =
        canvas::tile_look(iso.set, canvas::TileRef{.source = "iso", .atlas = {0, 0}});
    CHECK(look.position == slash::Vec2{-16.0F, -24.0F});
    CHECK(look.size == slash::Vec2{32.0F, 32.0F});

    // Hexagons pointing up: odd rows sit half a cell right, as in Godot's stacked_offset layout
    // once rows go down.
    const Layer hex = layer_of("hex_tiles");
    CHECK(hex.set.shape == canvas::TileShape::Hexagon);
    CHECK(hex.set.layout == canvas::TileLayout::StackedOffset);
    CHECK(hex.set.offset_axis == canvas::TileOffsetAxis::Horizontal);
    CHECK(hex.set.tile_size == slash::Vec2i{32, 36});
    CHECK(hex.position == parse("[-16, 9]"));
    CHECK(hex.centers == unity({{0, 0}, {48, -27}, {-16, 27}, {16, -27}}));

    // Swizzled flat hexagons: Unity's cell x runs up the columns and y along them.
    const Layer flat = layer_of("flat_tiles");
    CHECK(flat.set.shape == canvas::TileShape::Hexagon);
    CHECK(flat.set.offset_axis == canvas::TileOffsetAxis::Vertical);
    CHECK(flat.set.tile_size == slash::Vec2i{36, 32});
    CHECK(flat.position == parse("[-18, 0]"));
    CHECK(flat.centers == unity({{0, 0}, {27, -48}, {27, -16}, {-27, 16}}));
    CHECK(flat.cells.contains({1, -2}));

    const json::Value report = parse(files.at("import/unity.json"));
    CHECK(imported.value().report.find("converted")->find("tile_sets")->as_int() == 3);
    CHECK(unsupported(report, "turned tiles with the tile anchor off the cell's center")
              .find("where")
              ->as_string() == "Assets/Scenes/Board.unity: Iso Grid/Iso Tiles");
    CHECK(unsupported(report, "tile anchor off a hexagon's center").is_null());
}

TEST_CASE("Unity's AnimatedTiles become animated tiles, their frames along the sheet's grid") {
    const fs::path project = "import_unity_animated_tiles";
    fs::remove_all(project);
    make_project(project / "unity");
    write_animated_tiles(project / "unity");
    auto imported = importer::import_project(
        {.source = (project / "unity").string(), .destination = project.string()});
    REQUIRE_MESSAGE(imported.ok(), (imported.ok() ? std::string() : imported.error().message));
    const auto files = files_of(imported.value());
    CHECK(parse(files.at("unity/Assets/tiles.tileset.json")).find("sources")->size() == 1);
    const auto tiles_of = [&](const std::string& path) {
        return *parse(files.at(path)).find("sources")->find("pool")->find("tiles");
    };

    // Each frame lasts a second over the AnimatedTile's speed (the middle of its range) times
    // the tile map's frame rate. Water's frames follow along a row, the torch's wrap after two
    // columns and the lava's go down one. Tiles whose frames go back, or are on two textures,
    // show their first, and the other texture isn't a source.
    CHECK(tiles_of("unity/Assets/tiles.tileset.json") == parse(R"({
        "0,0": {"name": "water0", "animation_frames": [0.25, 0.25, 0.25]},
        "2,1": {"name": "torch0", "animation_frames": [0.5, 0.5, 0.5, 0.5],
                "animation_columns": 2},
        "0,1": {"name": "lava0", "animation_frames": [0.5, 0.5], "animation_columns": 1},
        "3,0": {"name": "rock"}
    })"));
    // Cells show an AnimatedTile's first frame, whichever Unity saved them showing.
    const json::Value pool = entity(files, "unity/Assets/Scenes/Pool.scene.json", "pool_tiles");
    const json::Value& layer = *pool.find("TileMapLayer");
    CHECK(*layer.find("rows") == parse(R"(["wwtlrrr"])"));
    CHECK(*layer.find("legend") ==
          parse(R"({"l": "lava0", "r": "rock", "t": "torch0", "w": "water0"})"));

    // In the smaller cells' tile set, foam shows a frame of the water, which stops; the brazier
    // shows the torch still, which plays there too, at the frame rate of 1.
    CHECK(tiles_of("unity/Assets/tiles_2.tileset.json") == parse(R"({
        "0,0": {"name": "water0"},
        "2,0": {"name": "water2"},
        "2,1": {"name": "torch0", "animation_frames": [1.0, 1.0, 1.0, 1.0],
                "animation_columns": 2}
    })"));

    const json::Value report = parse(files.at("import/unity.json"));
    const auto where = [&](std::string_view what) {
        const json::Value item = unsupported(report, what);
        return item.is_null() ? std::string() : item.find("where")->as_string();
    };
    CHECK(where("animated tiles with a random speed") ==
          "Assets/Scenes/Pool.unity: Pool Grid/Pool Tiles");
    CHECK(where("animated tile start times") == "Assets/Scenes/Pool.unity: Pool Grid/Pool Tiles");
    CHECK(where("animated tiles whose sprites aren't in order along the texture's grid") ==
          "Assets/Scenes/Pool.unity: Pool Grid/Pool Tiles");
    CHECK(where("animated tiles whose sprites aren't on one texture's grid") ==
          "Assets/Scenes/Pool.unity: Pool Grid/Pool Tiles");
    CHECK(where("animated tiles whose frames other tiles show") ==
          "Assets/Scenes/Pool.unity: Edge Grid/Edge Tiles");
    CHECK(where("sprites shown both by animated tiles and still ones") ==
          "Assets/Scenes/Pool.unity: Edge Grid/Edge Tiles");
    CHECK(where("scripted tiles (rule tiles and others)").empty());
}

TEST_CASE("Unity Animator controllers become animation libraries, their states animations") {
    const fs::path project = "import_unity_animation";
    fs::remove_all(project);
    make_project(project / "unity");
    write_frogs(project / "unity");
    auto imported = importer::import_project(
        {.source = (project / "unity").string(), .destination = project.string()});
    REQUIRE_MESSAGE(imported.ok(), (imported.ok() ? std::string() : imported.error().message));
    const json::Value& plan_report = imported.value().report;
    CHECK(plan_report.find("converted")->find("animation_libraries")->as_int() == 2);
    CHECK(plan_report.find("converted")->find("animation_trees")->as_int() == 2);
    const auto files = files_of(imported.value());
    const std::string frogs = "unity/Assets/Scenes/Frogs.scene.json";
    const std::string frog_library = "unity/Assets/Animations/Frog.anim.json";
    const std::string toad_library = "unity/Assets/Animations/Toad.anim.json";

    // A library per controller, an animation per state of its base layer. Tracks name the
    // objects they animate by their paths from the Animator's object, and the entity drawing an
    // object's sprite as "<path>#sprite"; units become pixels, y down, and angles clockwise.
    const json::Value library = parse(files.at(frog_library));
    const json::Value& animations = *library.find("animations");
    CHECK(animations.size() == 3);
    json::Value idle = *animations.find("Idle");
    CHECK(idle.find("length")->as_double() == 1.0);
    CHECK(idle.find("loop")->as_string() == "linear");
    json::Array& tracks = idle["tracks"].as_array();
    REQUIRE(tracks.size() == 7);
    // A curved stretch is keyed every 60th of a second, so the fade keeps Unity's curve.
    const json::Value fade = tracks[2];
    CHECK(fade.find("target")->as_string() == "Body#sprite");
    CHECK(fade.find("path")->as_string() == "Sprite2D/modulate/a");
    const json::Array& fade_keys = fade.find("keys")->as_array();
    CHECK(fade_keys.size() == 61);
    CHECK(fade_keys[30].find("time")->as_double() == doctest::Approx(0.5));
    CHECK(fade_keys[30].find("value")->as_double() == doctest::Approx(0.75));
    tracks.erase(tracks.begin() + 2);
    CHECK(json::Value(tracks) == parse(R"([
        {"target": "Body", "path": "Transform2D/position/x",
         "keys": [{"time": 0.0, "value": 0.0}, {"time": 0.5, "value": 0.0},
                  {"time": 1.0, "value": 0.0}]},
        {"target": "Body", "path": "Transform2D/position/y",
         "keys": [{"time": 0.0, "value": -8.0}, {"time": 0.5, "value": -12.0},
                  {"time": 1.0, "value": -8.0}]},
        {"target": "Body#sprite", "path": "Sprite2D/offset", "update": "discrete",
         "keys": [{"time": 0.0, "value": [0.0, -8.0]}, {"time": 0.5, "value": [0.0, -8.0]}]},
        {"target": "Body#sprite", "path": "Sprite2D/region", "update": "discrete",
         "keys": [{"time": 0.0, "value": [0.0, 0.0, 16.0, 16.0]},
                  {"time": 0.5, "value": [16.0, 0.0, 16.0, 16.0]}]},
        {"target": "Body#sprite", "path": "Sprite2D/texture",
         "keys": [{"time": 0.0, "value": "unity/Assets/Art/frog.png"},
                  {"time": 0.5, "value": "unity/Assets/Art/frog.png"}]},
        {"event": "Croak", "keys": [{"time": 0.5, "data": {"int": 2}}]}
    ])"));
    // Jump's state plays at twice the speed; its stepped keys hold.
    CHECK(*animations.find("Jump") == parse(R"({"length": 0.25, "tracks": [
        {"path": "Transform2D/rotation",
         "keys": [{"time": 0.0, "value": 0.0}, {"time": 0.25, "value": -1.570796}]},
        {"target": "Body#sprite", "path": "Sprite2D/flip_h",
         "keys": [{"time": 0.0, "value": false}, {"time": 0.125, "value": true}]}
    ]})"));
    // Walk's blend tree blends Idle with the jump, which at Walk's speed is an animation of its
    // own, its name put after the state's.
    CHECK(*animations.find("Walk Jump") == parse(R"({"length": 0.5, "tracks": [
        {"path": "Transform2D/rotation",
         "keys": [{"time": 0.0, "value": 0.0}, {"time": 0.5, "value": -1.570796}]},
        {"target": "Body#sprite", "path": "Sprite2D/flip_h",
         "keys": [{"time": 0.0, "value": false}, {"time": 0.25, "value": true}]}
    ]})"));

    // An override controller is its controller's states with its clips swapped.
    const json::Value toad = parse(files.at(toad_library));
    CHECK(*toad.find("animations")->find("Idle") == parse(R"({"length": 1.0, "loop": "linear",
        "tracks": [
        {"path": "Transform2D/position/x",
         "keys": [{"time": 0.0, "value": 0.0}, {"time": 1.0, "value": 16.0}]},
        {"path": "Transform2D/position/y",
         "keys": [{"time": 0.0, "value": 0.0}, {"time": 1.0, "value": 0.0}]}
    ]})"));
    CHECK(*toad.find("animations")->find("Jump") == *animations.find("Jump"));

    // The base layer's state machine is a tree over the library: Speed, Jump and Grounded are
    // its variables and trigger, the transitions' conditions advance expressions, and Jump's
    // exit time, the end of the state, ends its cross-fade there.
    const std::string frog_tree = "unity/Assets/Animations/Frog.animtree.json";
    CHECK(parse(files.at(frog_tree)) == parse(R"({"format": 1,
        "variables": {"Speed": 0.0, "Grounded": true}, "triggers": ["Jump"],
        "root": {"type": "StateMachine",
            "states": {"Idle": {"node": {"type": "Animation", "animation": "Idle"}},
                       "Jump": {"node": {"type": "Animation", "animation": "Jump"}},
                       "Walk": {"node": {"type": "BlendSpace1D", "sync": true, "points": [
                           {"position": 0.0, "node": {"type": "Animation", "animation": "Idle"}},
                           {"position": 1.0,
                            "node": {"type": "Animation", "animation": "Walk Jump"}}]}}},
            "transitions": [
                {"from": "Start", "to": "Idle", "advance_mode": "auto", "priority": 0},
                {"from": "Idle", "to": "Jump", "advance_mode": "auto", "priority": 0,
                 "xfade_time": 0.25, "advance_expression": "Jump"},
                {"from": "Idle", "to": "Walk", "advance_mode": "auto", "priority": 1,
                 "xfade_time": 0.25, "advance_expression": "Speed > 0.1"},
                {"from": "Jump", "to": "Idle", "advance_mode": "auto", "priority": 0,
                 "xfade_time": 0.25, "switch_mode": "at_end"}]}})"));
    // The toad's tree is the frog's, over its own library.
    CHECK(parse(files.at("unity/Assets/Animations/Toad.animtree.json")) ==
          parse(files.at(frog_tree)));

    // Each Animator is an AnimationTree playing its controller's tree, with the targets bound to
    // the entities under it; the toad has no body.
    const json::Value frog_tree_player = *entity(files, frogs, "frog").find("AnimationTree");
    CHECK(frog_tree_player == parse(R"({"tree_root": "unity/Assets/Animations/Frog.animtree.json",
        "libraries": {"": "unity/Assets/Animations/Frog.anim.json"}, "deterministic": false,
        "targets": {"Body": "body", "Body#sprite": "body"}})"));
    const json::Value toad_tree_player = *entity(files, frogs, "toad").find("AnimationTree");
    CHECK(*toad_tree_player.find("libraries") == parse(R"({"": ")" + toad_library + "\"}"));
    CHECK(*toad_tree_player.find("targets") == parse(R"({"Body#sprite": null})"));

    // Gameplay code sets the tree's parameters, which a storyboard task per controller lists.
    const json::Value work = parse(files.at("storyboard/work.json"));
    CHECK(slash::storyboard::validate_file("work", work).empty());
    const json::Value& task = *work.find("entries")->find("task/port-frog-anim");
    CHECK(task.find("name")->as_string() ==
          "Port the state machine of Assets/Animations/Frog.controller");
    CHECK(task.find("summary")->as_string() ==
          "Unity Animator controller: set its parameters from gameplay code.");
    CHECK(*task.find("links") == parse(R"(["unity/Assets/Animations/Frog.anim.json",
                                           "unity/Assets/Animations/Frog.animtree.json",
                                           "unity/Assets/Scenes/Frogs.scene.json"])"));
    CHECK(task.find("text")->as_string() ==
          "unity/Assets/Animations/Frog.animtree.json plays the state machine with an "
          "AnimationTree. Gameplay code sets its parameters, as Unity's SetFloat, SetInteger, "
          "SetBool and SetTrigger did: Speed (float, 0.0); Jump (trigger); Grounded (bool, "
          "true). Unity's blend trees read their parameters themselves; set each blend "
          "position with them: Walk/blend_position to Speed. Animation events: Croak at 0.5 s in "
          "Idle. Played by frog in unity/Assets/Scenes/Frogs.scene.json.");

    const json::Value report = parse(files.at("import/unity.json"));
    CHECK(*report.find("animation_libraries")->find(frog_library) ==
          parse(R"({"controller": "Assets/Animations/Frog.controller", "first": "Idle",
                    "tree": "unity/Assets/Animations/Frog.animtree.json",
                    "used_by": ["frog in unity/Assets/Scenes/Frogs.scene.json"],
                    "task": "task/port-frog-anim"})"));
    for (const auto& [what, where] : std::initializer_list<std::pair<std::string, std::string>>{
             {"Animator layers other than the base layer", "Assets/Animations/Frog.controller"},
             {"Animator exit time", "Assets/Animations/Frog.controller: Jump"},
             {"Animator blend tree motions of different lengths",
              "Assets/Animations/Frog.controller: state Walk"},
             {"animated script value", "Assets/Animations/Jump.anim: power"},
             {"animated object missing", "Assets/Scenes/Frogs.unity: Toad: Body"},
             {"Animator root motion", "Assets/Scenes/Frogs.unity: Toad"},
             {"animation clip no Animator controller plays", "Assets/Animations/Unused.anim"}}) {
        const json::Value item = unsupported(report, what);
        REQUIRE_MESSAGE(!item.is_null(), what);
        CHECK(item.find("where")->as_string() == where);
    }

    // It plays: 34 ticks in (the tree spends its first seeking to its states' starts, as
    // Godot's does), the body is on its way down from the top of its bob, faded, on its second
    // frame, and has croaked.
    slash::world::ComponentRegistry components;
    slash::world::register_builtin_components(components);
    command::Registry commands;
    command::register_builtin_commands(commands);
    command::SessionOptions options;
    options.project_root = project.string();
    options.capture_logs = false;
    command::Session session(components, commands, options);
    const command::Reply opened =
        session.call("scene.open", parse(R"({"path": ")" + frogs + "\"}"));
    REQUIRE_MESSAGE(opened.ok, opened.message);
    REQUIRE(session.call("events.subscribe", parse(R"({"kinds": ["Croak"]})")).ok);
    REQUIRE(session.call("step", parse(R"({"ticks": 34})")).ok);
    const slash::world::World& world = session.world();
    const entt::entity body = world.find("body");
    const auto& moved = world.ecs().get<slash::world::Transform2D>(body);
    CHECK(moved.position.x == doctest::Approx(0.0));
    CHECK(moved.position.y == doctest::Approx(-11.6));
    const auto& drawn = world.ecs().get<slash::world::Sprite2D>(body);
    CHECK(drawn.region == slash::Vec4{16.0F, 0.0F, 16.0F, 16.0F});
    CHECK(drawn.modulate.w == doctest::Approx(0.7126).epsilon(0.002));
    // The toad sits, moving right a unit a second.
    CHECK(world.ecs().get<slash::world::Transform2D>(world.find("toad")).position.x ==
          doctest::Approx(8.8));
    const json::Value croaks =
        session.call("events.read", parse(R"({"subscription": "events_1"})")).value;
    REQUIRE(croaks.find("events")->size() == 1);
    CHECK(croaks.find("events")->as_array()[0].find("id")->as_string() == "frog");
    CHECK(*croaks.find("events")->as_array()[0].find("data") == parse(R"({"int": 2})"));

    // SetTrigger("Jump"): the frog cross-fades into its jump, which alone turns it, and the
    // trigger goes back to false once taken.
    const command::Reply jumped = session.call(
        "animation.set_parameters", parse(R"({"id": "frog", "parameters": {"Jump": true}})"));
    REQUIRE_MESSAGE(jumped.ok, jumped.message);
    REQUIRE(session.call("step", parse(R"({"ticks": 9})")).ok);
    const json::Value state = session.call("animation.tree", parse(R"({"id": "frog"})")).value;
    CHECK(state.find("parameters")->find("Jump")->find("value")->as_bool() == false);
    const json::Value& playback = *state.find("state")->find("playback");
    CHECK(playback.find("current")->as_string() == "Jump");
    CHECK(playback.find("fading_from")->as_string() == "Idle");
    CHECK(world.ecs().get<slash::world::Transform2D>(world.find("frog")).rotation ==
          doctest::Approx(-1.570796 * 7.0 / 15.0));
}

namespace {

// An Animator transition with the fields the knight's controller varies.
struct KnightTransition {
    std::int64_t object = 0;
    std::string conditions = " []";
    std::int64_t state = 0;
    std::int64_t machine = 0;
    bool exit = false;
    bool exit_time = false;
    std::string exit_at = "0.75";
    std::string duration = "0.1";
    bool fixed = true;
    bool mute = false;
    bool solo = false;
    bool to_self = true;
};

std::string knight_transition(const KnightTransition& t) {
    return "--- !u!1101 &" + id(t.object) + "\nAnimatorStateTransition:\n" + std::string(kSource) +
           "  m_Name: \n  m_Conditions:" + t.conditions +
           "\n  m_DstStateMachine: " + ref(t.machine) + "\n  m_DstState: " + ref(t.state) +
           "\n  m_Solo: " + (t.solo ? "1" : "0") + "\n  m_Mute: " + (t.mute ? "1" : "0") +
           "\n  m_IsExit: " + (t.exit ? "1" : "0") +
           "\n  serializedVersion: 3\n  m_TransitionDuration: " + t.duration +
           "\n  m_TransitionOffset: 0\n  m_ExitTime: " + t.exit_at +
           "\n  m_HasExitTime: " + (t.exit_time ? "1" : "0") +
           "\n  m_HasFixedDuration: " + (t.fixed ? "1" : "0") +
           "\n  m_InterruptionSource: 0\n  m_OrderedInterruption: 1\n"
           "  m_CanTransitionToSelf: " +
           (t.to_self ? "1" : "0") + "\n";
}

// An entry or state machine transition, which has no timing.
std::string knight_entry(std::int64_t object, std::string_view conditions, std::int64_t state,
                         std::int64_t machine) {
    return "--- !u!1109 &" + id(object) + "\nAnimatorTransition:\n" + std::string(kSource) +
           "  m_Name: \n  m_Conditions:" + std::string(conditions) +
           "\n  m_DstStateMachine: " + ref(machine) + "\n  m_DstState: " + ref(state) +
           "\n  m_Solo: 0\n  m_Mute: 0\n  m_IsExit: 0\n  serializedVersion: 1\n";
}

std::string knight_machine(std::int64_t object, std::string_view name,
                           std::initializer_list<std::pair<std::int64_t, int>> states,
                           std::string_view machines, std::string_view any,
                           std::string_view entries, std::string_view machine_transitions,
                           std::int64_t first) {
    std::string out = "--- !u!1107 &" + id(object) +
                      "\nAnimatorStateMachine:\n  serializedVersion: 6\n" + std::string(kSource) +
                      "  m_Name: " + std::string(name) + "\n  m_ChildStates:\n";
    for (const auto& [state, x] : states) {
        out += "  - serializedVersion: 1\n    m_State: " + ref(state) +
               "\n    m_Position: {x: " + std::to_string(x) + ", y: 120, z: 0}\n";
    }
    return out + "  m_ChildStateMachines:" + std::string(machines) +
           "\n  m_AnyStateTransitions:" + std::string(any) +
           "\n  m_EntryTransitions:" + std::string(entries) +
           "\n  m_StateMachineTransitions:" + std::string(machine_transitions) +
           "\n  m_DefaultState: " + ref(first) + "\n";
}

} // namespace

TEST_CASE("Unity Animator state machines become AnimationTree state machines, as Unity plays "
          "them") {
    const fs::path project = "import_unity_state_machines";
    fs::remove_all(project);
    make_project(project / "unity");
    const fs::path folder = project / "unity";
    // Idle and Block loop over a second, Run over half of one; Swing plays once over 0.4.
    const auto clip = [&](std::string_view name, std::string_view guid, std::string_view stop,
                          bool loop, std::string_view x) {
        ClipCurves curves;
        curves.position =
            anim_curve(anim_key("0", "{x: " + std::string(x) + ", y: 0, z: 0}",
                                "{x: 0, y: 0, z: 0}", "{x: 0, y: 0, z: 0}", kVectorWeight),
                       "    path: \n");
        write_clip(folder / ("Assets/Animations/" + std::string(name) + ".anim"), guid, stop, loop,
                   curves);
    };
    clip("Idle", "a2000000000000000000000000000001", "1", true, "1");
    clip("Run", "a2000000000000000000000000000002", "0.5", true, "2");
    clip("Swing", "a2000000000000000000000000000003", "0.4", false, "3");
    clip("Block", "a2000000000000000000000000000004", "1", true, "4");
    const auto motion = [](std::string_view guid) { return ref(7400000, guid, 2); };

    // The root machine has Idle and Run and the Combat sub-state machine, Swing and Block;
    // Combat's entry picks Block when the knight has been hit three times, and Swing leaves it
    // through its exit, to Run when running, else through the root's entry.
    std::string controller(kHeader);
    controller += "--- !u!91 &9100000\nAnimatorController:\n" + std::string(kSource) +
                  "  m_Name: Knight\n  serializedVersion: 5\n  m_AnimatorParameters:\n" +
                  animator_parameter("Is Grounded", 4, "1") + animator_parameter("Hits", 3, "0") +
                  animator_parameter("Attack", 9, "0") + animator_parameter("Speed", 1, "0") +
                  "  m_AnimatorLayers:\n  - serializedVersion: 5\n    m_Name: Base Layer\n"
                  "    m_StateMachine: {fileID: 110700001}\n    m_Mask: {fileID: 0}\n";
    controller += knight_machine(
        110700001, "Base Layer", {{110200001, 0}, {110200002, 300}},
        "\n  - serializedVersion: 1\n    m_StateMachine: {fileID: 110700002}\n"
        "    m_Position: {x: 600, y: 120, z: 0}",
        "\n  - {fileID: 110100009}", "\n  - {fileID: 110900001}",
        "\n  - first: {fileID: 110700002}\n    second:\n    - {fileID: 110900003}", 110200001);
    controller += knight_machine(110700002, "Combat", {{110200003, 0}, {110200004, 300}}, " []",
                                 " []", "\n  - {fileID: 110900002}", " []", 110200003);
    controller += knight_entry(110900001, animator_condition(1, "Attack", "0"), 0, 110700002);
    controller += knight_entry(110900002, animator_condition(6, "Hits", "3"), 110200004, 0);
    controller += knight_entry(110900003, animator_condition(3, "Speed", "0.5"), 110200002, 0);
    controller +=
        animator_state(110200001, "Idle", "1", "\n  - {fileID: 110100001}\n  - {fileID: 110100002}",
                       motion("a2000000000000000000000000000001"));
    controller += animator_state(
        110200002, "Run", "1",
        "\n  - {fileID: 110100003}\n  - {fileID: 110100004}\n  - {fileID: 110100005}",
        motion("a2000000000000000000000000000002"));
    controller += animator_state(110200003, "Swing", "1", "\n  - {fileID: 110100006}",
                                 motion("a2000000000000000000000000000003"));
    controller += animator_state(110200004, "Block", "1", "\n  - {fileID: 110100007}",
                                 motion("a2000000000000000000000000000004"));
    // Idle runs past half speed, cross-fading over a quarter of itself, or when it leaves the
    // ground.
    controller += knight_transition({.object = 110100001,
                                     .conditions = animator_condition(3, "Speed", "0.5"),
                                     .state = 110200002,
                                     .duration = "0.25",
                                     .fixed = false});
    controller += knight_transition({.object = 110100002,
                                     .conditions = animator_condition(2, "Is Grounded", "0"),
                                     .state = 110200002});
    // Run's muted way back to Idle doesn't count, and its solo attack mutes the one at its end.
    controller += knight_transition({.object = 110100003,
                                     .conditions = animator_condition(4, "Speed", "0.5"),
                                     .state = 110200001,
                                     .mute = true});
    controller += knight_transition({.object = 110100004,
                                     .conditions = animator_condition(1, "Attack", "0"),
                                     .machine = 110700002,
                                     .solo = true});
    controller += knight_transition({.object = 110100005, .state = 110200001, .exit_time = true});
    // Swing leaves Combat at Unity's default exit time, as its cross-fade ends with it.
    controller += knight_transition(
        {.object = 110100006, .exit = true, .exit_time = true, .duration = "0.25", .fixed = false});
    // Block's way to Swing has no conditions and no exit time, so Unity never takes it.
    controller += knight_transition({.object = 110100007, .state = 110200003});
    // From any state, more than four hits block, but not from Block itself.
    controller += knight_transition({.object = 110100009,
                                     .conditions = animator_condition(3, "Hits", "4"),
                                     .state = 110200004,
                                     .to_self = false});
    write(folder / "Assets/Animations/Knight.controller", controller);
    write(folder / "Assets/Animations/Knight.controller.meta",
          meta("a2000000000000000000000000000010", "NativeFormatImporter",
               "  mainObjectFileID: 9100000\n"));

    auto imported =
        importer::import_project({.source = folder.string(), .destination = project.string()});
    REQUIRE_MESSAGE(imported.ok(), (imported.ok() ? std::string() : imported.error().message));
    CHECK(imported.value().report.find("converted")->find("animation_trees")->as_int() == 1);
    const auto files = files_of(imported.value());
    const json::Value tree = parse(files.at("unity/Assets/Animations/Knight.animtree.json"));

    // Parameters are the tree's variables and triggers, under names an expression reads.
    CHECK(*tree.find("variables") == parse(R"({"Hits": 0, "Is_Grounded": true, "Speed": 0.0})"));
    CHECK(*tree.find("triggers") == parse(R"(["Attack"])"));
    // Combat's states join the layer's, each where Unity drew it in its own machine.
    CHECK(*tree.find("root")->find("states") == parse(R"({
        "Block": {"node": {"type": "Animation", "animation": "Block"}, "position": [300, 120]},
        "Idle": {"node": {"type": "Animation", "animation": "Idle"}, "position": [0, 120]},
        "Run": {"node": {"type": "Animation", "animation": "Run"}, "position": [300, 120]},
        "Swing": {"node": {"type": "Animation", "animation": "Swing"}, "position": [0, 120]}
    })"));
    // Start goes through the entries: into Combat on Attack, to Block there after three hits,
    // else Idle. Any State's transition comes first from every state but Block. Idle's two ways
    // to Run are one; Run's muted way back is gone and its solo attack mutes its exit time.
    // Swing leaves Combat as it ends, to Run when running, else back through the entries; a way
    // back into Swing itself and Block's way out, which has neither conditions nor exit time,
    // are dropped.
    CHECK(*tree.find("root")->find("transitions") == parse(R"json([
        {"from": "Start", "to": "Block", "advance_mode": "auto", "priority": 0,
         "advance_expression": "Attack and Hits == 3"},
        {"from": "Start", "to": "Swing", "advance_mode": "auto", "priority": 1,
         "advance_expression": "Attack"},
        {"from": "Start", "to": "Idle", "advance_mode": "auto", "priority": 2},
        {"from": "Idle", "to": "Block", "advance_mode": "auto", "priority": 0,
         "xfade_time": 0.1, "advance_expression": "Hits > 4"},
        {"from": "Idle", "to": "Run", "advance_mode": "auto", "priority": 1,
         "xfade_time": 0.25, "advance_expression": "(Speed > 0.5) or (not Is_Grounded)"},
        {"from": "Run", "to": "Block", "advance_mode": "auto", "priority": 0,
         "xfade_time": 0.1, "advance_expression": "(Hits > 4) or (Attack and Hits == 3)"},
        {"from": "Run", "to": "Swing", "advance_mode": "auto", "priority": 2,
         "xfade_time": 0.1, "advance_expression": "Attack"},
        {"from": "Swing", "to": "Block", "advance_mode": "auto", "priority": 0,
         "xfade_time": 0.1, "advance_expression": "(Hits > 4) or (Attack and Hits == 3)"},
        {"from": "Swing", "to": "Run", "advance_mode": "auto", "priority": 1,
         "xfade_time": 0.1, "switch_mode": "at_end", "advance_expression": "Speed > 0.5"},
        {"from": "Swing", "to": "Idle", "advance_mode": "auto", "priority": 4,
         "xfade_time": 0.1, "switch_mode": "at_end"}
    ])json"));

    // The task says what to set from gameplay code, and links the tree.
    const json::Value work = parse(files.at("storyboard/work.json"));
    const json::Value& task = *work.find("entries")->find("task/port-knight-anim");
    CHECK(task.find("summary")->as_string() ==
          "Unity Animator controller: set its parameters from gameplay code.");
    CHECK(*task.find("links") == parse(R"(["unity/Assets/Animations/Knight.anim.json",
                                           "unity/Assets/Animations/Knight.animtree.json"])"));
    CHECK(task.find("text")->as_string() ==
          "unity/Assets/Animations/Knight.animtree.json plays the state machine with an "
          "AnimationTree. Gameplay code sets its parameters, as Unity's SetFloat, SetInteger, "
          "SetBool and SetTrigger did: Is Grounded as Is_Grounded (bool, true); Hits (int, 0); "
          "Attack (trigger); Speed (float, 0.0).");

    const json::Value report = parse(files.at("import/unity.json"));
    CHECK(report.find("animation_libraries")
              ->find("unity/Assets/Animations/Knight.anim.json")
              ->find("tree")
              ->as_string() == "unity/Assets/Animations/Knight.animtree.json");
    const std::string controller_path = "Assets/Animations/Knight.controller: ";
    for (const auto& [what, where, count] :
         std::initializer_list<std::tuple<std::string, std::string, int>>{
             {"Animator parameter name", "Is Grounded", 1},
             {"Animator transitions between the same states", "Idle", 3},
             {"Animator transition to its own state", "Swing", 1},
             {"Animator transition with no conditions or exit time", "Block", 1}}) {
        const json::Value item = unsupported(report, what);
        REQUIRE_MESSAGE(!item.is_null(), what);
        CHECK(item.find("where")->as_string() == controller_path + where);
        CHECK(item.find("count")->as_int() == count);
    }
    CHECK(unsupported(report, "Animator exit time").is_null());
}

namespace {

std::string blend_tree(std::int64_t object, int type, std::string_view x, std::string_view y,
                       std::string_view children) {
    return "--- !u!206 &" + id(object) + "\nBlendTree:\n" + std::string(kSource) +
           "  m_Name: Blend Tree\n  m_Childs:" + std::string(children) +
           "\n  m_BlendParameter: " + std::string(x) + "\n  m_BlendParameterY: " + std::string(y) +
           "\n  m_MinThreshold: 0\n  m_MaxThreshold: 1\n  m_UseAutomaticThresholds: 0\n"
           "  m_NormalizedBlendValues: 0\n  m_BlendType: " +
           std::to_string(type) + "\n";
}

std::string blend_child(std::string_view motion, std::string_view threshold, std::string_view x,
                        std::string_view y, std::string_view scale = "1") {
    return "\n  - serializedVersion: 2\n    m_Motion: " + std::string(motion) +
           "\n    m_Threshold: " + std::string(threshold) +
           "\n    m_Position: {x: " + std::string(x) + ", y: " + std::string(y) +
           "}\n    m_TimeScale: " + std::string(scale) +
           "\n    m_CycleOffset: 0\n    m_DirectBlendParameter: Speed\n    m_Mirror: 0";
}

} // namespace

TEST_CASE("Unity blend trees become blend spaces, nested and collapsed as Unity lays them out") {
    const fs::path project = "import_unity_blend_trees";
    fs::remove_all(project);
    make_project(project / "unity");
    const fs::path folder = project / "unity";
    // Each clip holds the hero at its own x, so what plays shows in where the hero is.
    const auto clip = [&](std::string_view name, std::string_view guid, std::string_view stop,
                          std::string_view x) {
        ClipCurves curves;
        curves.position =
            anim_curve(anim_key("0", "{x: " + std::string(x) + ", y: 0, z: 0}",
                                "{x: 0, y: 0, z: 0}", "{x: 0, y: 0, z: 0}", kVectorWeight),
                       "    path: \n");
        write_clip(folder / ("Assets/Animations/" + std::string(name) + ".anim"), guid, stop, true,
                   curves);
    };
    clip("Idle", "a3000000000000000000000000000001", "1", "0");
    clip("Left", "a3000000000000000000000000000002", "1", "-1");
    clip("Right", "a3000000000000000000000000000003", "1", "1");
    clip("Back", "a3000000000000000000000000000004", "1", "-2");
    clip("Walk", "a3000000000000000000000000000005", "1", "2");
    clip("Run", "a3000000000000000000000000000006", "0.5", "4");
    const auto motion = [](char n) {
        return ref(7400000, std::string("a300000000000000000000000000000") + n, 2);
    };

    // Locomotion blends five motions on MoveX and MoveY, the one ahead a tree of its own that
    // runs on Speed, the run at twice its speed. Strafe's motions are in a row, Climb's in a
    // column and Dash's on a slant, and Mix is a direct blend tree.
    std::string controller(kHeader);
    controller += "--- !u!91 &9100000\nAnimatorController:\n" + std::string(kSource) +
                  "  m_Name: Hero\n  serializedVersion: 5\n  m_AnimatorParameters:\n" +
                  animator_parameter("MoveX", 1, "0") + animator_parameter("MoveY", 1, "0.5") +
                  animator_parameter("Speed", 1, "0") +
                  "  m_AnimatorLayers:\n  - serializedVersion: 5\n    m_Name: Base Layer\n"
                  "    m_StateMachine: {fileID: 110700001}\n    m_Mask: {fileID: 0}\n";
    controller += knight_machine(
        110700001, "Base Layer",
        {{110200001, 0}, {110200002, 300}, {110200003, 600}, {110200004, 900}, {110200005, 1200}},
        " []", " []", " []", " []", 110200001);
    controller += animator_state(110200001, "Locomotion", "1", " []", ref(20600001));
    controller += animator_state(110200002, "Strafe", "1", " []", ref(20600003));
    controller += animator_state(110200003, "Mix", "1", " []", ref(20600004));
    controller += animator_state(110200004, "Climb", "1", " []", ref(20600005));
    controller += animator_state(110200005, "Dash", "1", " []", ref(20600006));
    controller += blend_tree(
        20600001, 3, "MoveX", "MoveY",
        blend_child(motion('1'), "0", "0", "0") + blend_child(motion('2'), "0", "-1", "0") +
            blend_child(motion('3'), "0", "1", "0") + blend_child(motion('4'), "0", "0", "-1") +
            blend_child(ref(20600002), "0", "0", "1"));
    controller += blend_tree(20600002, 0, "Speed", "Speed",
                             blend_child(motion('5'), "0", "0", "0") +
                                 blend_child(motion('6'), "2", "0", "0", "2"));
    controller += blend_tree(20600003, 1, "MoveX", "MoveY",
                             blend_child(motion('2'), "0", "-1", "0") +
                                 blend_child(motion('1'), "0", "0", "0") +
                                 blend_child(motion('3'), "0", "2", "0"));
    controller += blend_tree(20600004, 4, "Speed", "Speed",
                             blend_child(motion('2'), "0", "0", "0") +
                                 blend_child(motion('3'), "0", "0", "0"));
    controller += blend_tree(20600005, 2, "MoveX", "MoveY",
                             blend_child(motion('4'), "0", "0", "-1") +
                                 blend_child(motion('1'), "0", "0", "0") +
                                 blend_child(motion('5'), "0", "0", "1"));
    controller += blend_tree(20600006, 3, "MoveX", "MoveY",
                             blend_child(motion('1'), "0", "0", "0") +
                                 blend_child(motion('3'), "0", "1", "1") +
                                 blend_child(motion('5'), "0", "2", "2"));
    write(folder / "Assets/Animations/Hero.controller", controller);
    write(folder / "Assets/Animations/Hero.controller.meta",
          meta("a3000000000000000000000000000010", "NativeFormatImporter",
               "  mainObjectFileID: 9100000\n"));
    std::string scene(kHeader);
    scene += game_object(5000, "Hero", {5001, 5002});
    scene += transform(5001, 5000, "{x: 0, y: 0, z: 0}");
    scene += component(95, "Animator", 5002, 5000,
                       "  m_Avatar: {fileID: 0}\n  m_Controller: " +
                           ref(9100000, "a3000000000000000000000000000010", 2) +
                           "\n  m_CullingMode: 0\n  m_UpdateMode: 0\n  m_ApplyRootMotion: 0\n");
    write(folder / "Assets/Scenes/Arena.unity", scene);
    write(folder / "Assets/Scenes/Arena.unity.meta", meta("a3000000000000000000000000000011"));

    auto imported =
        importer::import_project({.source = folder.string(), .destination = project.string()});
    REQUIRE_MESSAGE(imported.ok(), (imported.ok() ? std::string() : imported.error().message));
    const auto files = files_of(imported.value());
    const json::Value tree = parse(files.at("unity/Assets/Animations/Hero.animtree.json"));
    const json::Value library = parse(files.at("unity/Assets/Animations/Hero.anim.json"));

    // An animation per clip, named after it, the run's at its time scale.
    const json::Value& animations = *library.find("animations");
    CHECK(animations.size() == 6);
    CHECK(animations.find("Run")->find("length")->as_double() == 0.25);
    // Locomotion is a 2D blend space whose point ahead is a 1D one; Strafe's row is 1D along
    // MoveX, reaching 2; Mix plays its first motion. With no transitions, Start goes to the
    // default state.
    const auto point = [](const json::Value& position, std::string_view animation) {
        json::Value out;
        out["position"] = position;
        out["node"]["type"] = "Animation";
        out["node"]["animation"] = animation;
        return out;
    };
    const auto xy = [](double x, double y) { return json::Value(json::Array{x, y}); };
    json::Value ahead;
    ahead["type"] = "BlendSpace1D";
    ahead["points"] = json::Array{point(json::Value(0.0), "Walk"), point(json::Value(2.0), "Run")};
    ahead["max_space"] = 2.0;
    ahead["sync"] = true;
    json::Value locomotion;
    locomotion["type"] = "BlendSpace2D";
    locomotion["points"] = json::Array{point(xy(0, 0), "Idle"), point(xy(-1, 0), "Left"),
                                       point(xy(1, 0), "Right"), point(xy(0, -1), "Back")};
    json::Value ahead_point;
    ahead_point["position"] = xy(0, 1);
    ahead_point["node"] = ahead;
    locomotion["points"].as_array().push_back(std::move(ahead_point));
    locomotion["sync"] = true;
    const json::Value& states = *tree.find("root")->find("states");
    CHECK(*states.find("Locomotion")->find("node") == locomotion);
    json::Value strafe;
    strafe["type"] = "BlendSpace1D";
    strafe["points"] =
        json::Array{point(json::Value(-1.0), "Left"), point(json::Value(0.0), "Idle"),
                    point(json::Value(2.0), "Right")};
    strafe["max_space"] = 2.0;
    strafe["sync"] = true;
    CHECK(*states.find("Strafe")->find("node") == strafe);
    CHECK(*states.find("Mix")->find("node") ==
          parse(R"({"type": "Animation", "animation": "Left"})"));
    // Climb's column is 1D along MoveY; Dash's slant makes no triangles, so the closest plays.
    CHECK(*states.find("Climb")->find("node") == parse(R"({"type": "BlendSpace1D", "points": [
        {"position": -1.0, "node": {"type": "Animation", "animation": "Back"}},
        {"position": 0.0, "node": {"type": "Animation", "animation": "Idle"}},
        {"position": 1.0, "node": {"type": "Animation", "animation": "Walk"}}], "sync": true})"));
    CHECK(*states.find("Dash")->find("node") == parse(R"({"type": "BlendSpace2D", "points": [
        {"position": [0.0, 0.0], "node": {"type": "Animation", "animation": "Idle"}},
        {"position": [1.0, 1.0], "node": {"type": "Animation", "animation": "Right"}},
        {"position": [2.0, 2.0], "node": {"type": "Animation", "animation": "Walk"}}],
        "max_space": [2.0, 2.0], "blend_mode": "discrete", "sync": true})"));
    CHECK(
        *tree.find("root")->find("transitions") ==
        parse(R"([{"from": "Start", "to": "Locomotion", "advance_mode": "auto", "priority": 0}])"));

    // The task says which blend position follows which parameter, and the tree starts where
    // MoveY's default puts Locomotion.
    const json::Value work = parse(files.at("storyboard/work.json"));
    CHECK(work.find("entries")
              ->find("task/port-hero-anim")
              ->find("text")
              ->as_string()
              .find("Unity's blend trees read their parameters themselves; set each blend position "
                    "with them: Locomotion/4/blend_position to Speed; Locomotion/blend_position to "
                    "[MoveX, MoveY]; Strafe/blend_position to MoveX; Climb/blend_position to "
                    "MoveY; Dash/blend_position to [MoveX, MoveY].") != std::string::npos);
    const std::string arena = "unity/Assets/Scenes/Arena.scene.json";
    CHECK(*entity(files, arena, "hero").find("AnimationTree")->find("parameters") ==
          parse(R"({"Locomotion/blend_position": [0.0, 0.5], "Climb/blend_position": 0.5,
                    "Dash/blend_position": [0.0, 0.5]})"));
    const json::Value report = parse(files.at("import/unity.json"));
    const std::string controller_path = "Assets/Animations/Hero.controller: state ";
    for (const auto& [what, where] : std::initializer_list<std::pair<std::string, std::string>>{
             {"Animator 2D blend tree", "Locomotion"},
             {"Animator 2D blend tree on a slanted line", "Dash"},
             {"Animator blend tree motions of different lengths", "Locomotion"},
             {"Animator direct blend tree", "Mix"}}) {
        const json::Value item = unsupported(report, what);
        REQUIRE_MESSAGE(!item.is_null(), what);
        CHECK(item.find("where")->as_string() == controller_path + where);
    }

    // It blends: halfway up to the walk at first, then halfway right, then all the way ahead
    // at a run.
    slash::world::ComponentRegistry components;
    slash::world::register_builtin_components(components);
    command::Registry commands;
    command::register_builtin_commands(commands);
    command::SessionOptions options;
    options.project_root = project.string();
    options.capture_logs = false;
    command::Session session(components, commands, options);
    const command::Reply opened =
        session.call("scene.open", parse(R"({"path": ")" + arena + "\"}"));
    REQUIRE_MESSAGE(opened.ok, opened.message);
    const slash::world::World& world = session.world();
    const auto hero_x = [&] {
        return world.ecs().get<slash::world::Transform2D>(world.find("hero")).position.x;
    };
    REQUIRE(session.call("step", parse(R"({"ticks": 3})")).ok);
    CHECK(hero_x() == doctest::Approx(16.0));
    const auto blend = [&](std::string_view parameters) {
        const command::Reply set =
            session.call("animation.set_parameters",
                         parse(R"({"id": "hero", "parameters": )" + std::string(parameters) + "}"));
        REQUIRE_MESSAGE(set.ok, set.message);
        REQUIRE(session.call("step", parse(R"({"ticks": 2})")).ok);
    };
    blend(R"({"Locomotion/blend_position": [0.5, 0]})");
    CHECK(hero_x() == doctest::Approx(8.0));
    blend(R"({"Locomotion/blend_position": [0, 1], "Locomotion/4/blend_position": 2})");
    CHECK(hero_x() == doctest::Approx(64.0));
}

TEST_CASE("Unity audio mixers become a bus layout and 2D joints Godot's joints") {
    const fs::path project = "import_unity_mill";
    fs::remove_all(project);
    make_project(project / "unity");
    write_mill(project / "unity");
    auto imported = importer::import_project(
        {.source = (project / "unity").string(), .destination = project.string()});
    REQUIRE_MESSAGE(imported.ok(), (imported.ok() ? std::string() : imported.error().message));
    CHECK(imported.value().report.find("converted")->find("bus_layouts")->as_int() == 1);
    const auto files = files_of(imported.value());
    const std::string layout = "unity/Assets/Audio/Main.bus_layout.json";
    const std::string mill = "unity/Assets/Scenes/Mill.scene.json";

    // Every group becomes a bus sending to its parent's, Main's Master the project's Master and
    // UI's the bus "UI" sending to the group it outputs to. The start snapshot gives volumes and
    // effect settings; a send to a Duck Volume makes its compressor's sidechain.
    CHECK(parse(files.at("project.json")).find("audio")->find("bus_layout")->as_string() == layout);
    CHECK(parse(files.at(layout)) == parse(R"({"format": 1, "buses": [
        {"name": "Master"},
        {"name": "Music", "volume_db": -10.0, "effects": [
            {"type": "compressor", "threshold": -20.0, "ratio": 3.0, "attack_us": 1000.0,
             "release_ms": 200.0, "sidechain": "SFX"},
            {"type": "low_pass_filter", "cutoff_hz": 5000.0}]},
        {"name": "SFX", "effects": [
            {"type": "low_pass_filter", "cutoff_hz": 1200.0, "resonance": 1.0},
            {"type": "delay", "dry": 0.8, "tap1_active": false, "tap2_active": false,
             "feedback_active": true, "feedback_delay_ms": 300.0,
             "feedback_level_db": -12.0412},
            {"type": "compressor", "threshold": -12.0, "ratio": 2.5, "gain": 3.0,
             "attack_us": 2000.0, "release_ms": 100.0},
            {"type": "reverb", "room_size": 0.706, "damping": 0.5, "dry": 0.8913, "wet": 0.631,
             "predelay_msec": 60.0, "predelay_feedback": 0.0},
            {"type": "distortion", "mode": "waveshape", "drive": 0.25, "keep_hf_hz": 20500.0,
             "enabled": false},
            {"type": "chorus", "dry": 0.5, "wet": 1.0, "voice_count": 3, "voices": [
                {"delay_ms": 40.0, "rate_hz": 0.8, "depth_ms": 1.2, "level_db": -6.0206,
                 "pan": 0.0},
                {"delay_ms": 40.0, "rate_hz": 0.8, "depth_ms": 1.2, "level_db": -6.0206,
                 "pan": 0.0},
                {"delay_ms": 40.0, "rate_hz": 0.8, "depth_ms": 1.2, "level_db": -6.0206,
                 "pan": 0.0}]}]},
        {"name": "Footsteps", "send": "SFX", "solo": true, "mute": true, "bypass_effects": true,
         "effects": [{"type": "pitch_shift", "pitch_scale": 1.5, "fft_size": 2048,
                      "oversampling": 8}]},
        {"name": "UI", "send": "SFX", "volume_db": -3.0},
        {"name": "Music 2", "send": "UI"}
    ]})"));
    const json::Value report = parse(files.at("import/unity.json"));
    CHECK(*report.find("bus_layout") == parse(R"({
        "path": "unity/Assets/Audio/Main.bus_layout.json",
        "groups": {
            "Assets/Audio/Main.mixer": {"Master": "Master", "Master/Music": "Music",
                                        "Master/SFX": "SFX", "Master/SFX/Footsteps": "Footsteps"},
            "Assets/Audio/UI.mixer": {"Master": "UI", "Master/Music": "Music 2"}}})"));

    // Sounds play on their group's bus.
    CHECK(entity(files, mill, "steps")["AudioStreamPlayer"]["bus"].as_string() == "Footsteps");
    CHECK(entity(files, mill, "click")["AudioStreamPlayer"]["bus"].as_string() == "Music 2");
    CHECK(entity(files, mill, "lost")["AudioStreamPlayer"].find("bus") == nullptr); // Master

    // Each joint is an entity under its object, joining the connected body (or the world) as
    // node_a to the object's body as node_b. Unity's angles keep their sign; the spring's
    // frequency and damping ratio become stiffness and damping for the weight's mass.
    json::Value hinge = entity(files, mill, "door_hinge");
    CHECK(hinge["Transform2D"]["position"] == parse("[-16.0, 0.0]"));
    json::Value& pin = hinge["PinJoint2D"];
    CHECK(pin["node_a"].as_string() == "frame");
    CHECK(pin["node_b"].as_string() == "door");
    CHECK(pin["angular_limit_enabled"].as_bool());
    CHECK(pin["angular_limit_lower"].as_double() == doctest::Approx(-0.785398));
    CHECK(pin["angular_limit_upper"].as_double() == doctest::Approx(1.570796));
    CHECK(pin["motor_enabled"].as_bool());
    CHECK(pin["motor_target_velocity"].as_double() == doctest::Approx(1.570796));
    json::Value spring = entity(files, mill, "weight_spring");
    CHECK(spring["Transform2D"]["position"] == parse("[0.0, -64.0]"));
    CHECK(spring["Transform2D"].find("rotation") == nullptr);
    json::Value& damped = spring["DampedSpringJoint2D"];
    CHECK(damped["node_a"].is_null());
    CHECK(damped["node_b"].as_string() == "weight");
    CHECK(damped["length"].as_double() == doctest::Approx(56.0));
    CHECK(damped["rest_length"].as_double() == doctest::Approx(48.0));
    CHECK(damped["stiffness"].as_double() == doctest::Approx(157.9137));
    CHECK(damped["damping"].as_double() == doctest::Approx(12.5664));
    json::Value slider = entity(files, mill, "bead_slider");
    CHECK(slider["Transform2D"]["position"] == parse("[-13.8564, 8.0]"));
    CHECK(slider["Transform2D"]["rotation"].as_double() == doctest::Approx(-2.094395));
    CHECK(slider["GrooveJoint2D"]["length"].as_double() == doctest::Approx(64.0));
    CHECK(slider["GrooveJoint2D"]["initial_offset"].as_double() == doctest::Approx(16.0));
    CHECK(entity(files, mill, "gate_hinge")["PinJoint2D"]["node_b"].as_string() == "gate");

    for (const auto& [what, where] :
         std::initializer_list<std::pair<std::string_view, std::string_view>>{
             {"mixer snapshots", "Assets/Audio/Main.mixer"},
             {"filter resonance above 1", "Assets/Audio/Main.mixer: SFX"},
             {"compressor attack above 2 ms", "Assets/Audio/Main.mixer: SFX"},
             {"mixer effect Flange", "Assets/Audio/Main.mixer: SFX"},
             {"mixer send", "Assets/Audio/Main.mixer: SFX"},
             {"effect after the fader", "Assets/Audio/Main.mixer: Music"},
             {"mixer group pitch", "Assets/Audio/Main.mixer: Footsteps"},
             {"audio mixer group", "Assets/Scenes/Mill.unity: Lost"},
             {"SliderJoint2D", "Assets/Scenes/Mill.unity: Bead"},
             {"component FixedJoint2D", "Assets/Scenes/Mill.unity: Gate"},
             {"disabled HingeJoint2D", "Assets/Scenes/Mill.unity: Gate"},
             {"breakable joint", "Assets/Scenes/Mill.unity: Gate"}}) {
        const json::Value item = unsupported(report, what);
        REQUIRE_MESSAGE(!item.is_null(), what);
        CHECK(item.find("where")->as_string() == where);
    }
    CHECK(unsupported(report, "mixer send").find("count")->as_int() == 1); // not the silent one
    CHECK(unsupported(report, "mixer snapshots").find("why")->as_string().find("Quiet") !=
          std::string::npos);

    // It runs: the door swings down to its limit, the weight settles 48 + 320 / 157.9 pixels
    // under the spring's top, and the bead slides down the groove to its end.
    slash::world::ComponentRegistry components;
    slash::world::register_builtin_components(components);
    command::Registry commands;
    command::register_builtin_commands(commands);
    command::SessionOptions options;
    options.project_root = project.string();
    options.capture_logs = false;
    command::Session session(components, commands, options);
    const command::Reply opened = session.call("scene.open", parse(R"({"path": ")" + mill + "\"}"));
    REQUIRE_MESSAGE(opened.ok, opened.message);
    REQUIRE(session.call("step", parse(R"({"ticks": 300})")).ok);
    const slash::world::World& world = session.world();
    const auto placed = [&](std::string_view id) {
        return world.ecs().get<slash::world::Transform2D>(world.find(id));
    };
    CHECK(placed("door").rotation == doctest::Approx(1.5708).epsilon(0.02));
    CHECK(placed("weight").position.x == doctest::Approx(32.0).epsilon(0.001));
    CHECK(placed("weight").position.y == doctest::Approx(58.03).epsilon(0.002));
    CHECK(placed("bead").position.x == doctest::Approx(64.0).epsilon(0.005));
    CHECK(placed("bead").position.y == doctest::Approx(0.0).epsilon(0.5));
}

TEST_CASE("Unity UI becomes controls: canvases, text, images, widgets and layout groups") {
    const fs::path project = "import_unity_menu";
    fs::remove_all(project);
    make_project(project / "unity");
    write_menu(project / "unity");
    auto imported = importer::import_project(
        {.source = (project / "unity").string(), .destination = project.string()});
    REQUIRE_MESSAGE(imported.ok(), (imported.ok() ? std::string() : imported.error().message));
    const auto files = files_of(imported.value());
    const std::string menu = "unity/Assets/Scenes/Menu.scene.json";

    // The canvas is a control of the window's size at the scale its CanvasScaler gives the
    // 800x600 window: 400x300 at twice the size. The event system needs nothing.
    json::Value canvas = entity(files, menu, "canvas");
    CHECK(canvas["Control"]["offset_right"].as_double() == 400.0);
    CHECK(canvas["Control"]["offset_bottom"].as_double() == 300.0);
    CHECK(canvas["Control"]["scale"] == parse("[2.0, 2.0]"));
    CHECK(canvas["Control"]["children"] ==
          parse(R"(["panel", "title", "play", "sound", "volume", "name", "list", "grid",
                    "logo", "options", "badge"])"));
    CHECK(entity(files, menu, "event_system").find("Control") == nullptr);

    // A rect's anchors, position, size and pivot become anchors, offsets and grow directions;
    // TextMesh Pro text is a Label in its font asset's font, its outline the theme's.
    json::Value title = entity(files, menu, "title");
    CHECK(title["Control"]["anchor_left"].as_double() == 0.5);
    CHECK(title["Control"]["anchor_right"].as_double() == 0.5);
    CHECK(title["Control"]["offset_left"].as_double() == -150.0);
    CHECK(title["Control"]["offset_top"].as_double() == 10.0);
    CHECK(title["Control"]["offset_right"].as_double() == 150.0);
    CHECK(title["Control"]["offset_bottom"].as_double() == 70.0);
    CHECK(title["Control"].find("anchor_top") == nullptr); // 0, as Unity's top anchor of 1
    CHECK(title["Control"]["grow_horizontal"].as_string() == "both");
    CHECK(title["Control"]["theme_override_fonts"] ==
          parse(R"({"font": "unity/Assets/Fonts/Pond.ttf"})"));
    CHECK(title["Control"]["theme_override_constants"] == parse(R"({"outline_size": 8})"));
    CHECK(title["Control"]["theme_override_colors"] == parse(R"({"font_color": [1.0, 0.9, 0.2, 1.0],
                    "font_outline_color": [0.0, 0.0, 0.0, 1.0]})"));
    CHECK(title["Label"]["text"].as_string() == "Pond Hop");
    CHECK(title["Label"]["horizontal_alignment"].as_string() == "center");
    CHECK(title["Label"]["vertical_alignment"].as_string() == "center");
    CHECK(title["Label"].find("autowrap_mode") == nullptr); // off, as word wrapping is

    // A button draws its target graphic, a sliced sprite, tinted per state; its label is
    // a Label of Unity's legacy Text in the default font.
    json::Value play = entity(files, menu, "play");
    REQUIRE(play.find("Button") != nullptr);
    json::Value& styles = play["Control"]["theme_override_styles"];
    CHECK(styles["normal"] == parse(R"({"type": "texture", "texture": "unity/Assets/UI/button.png",
        "texture_margin_left": 12.0, "texture_margin_top": 12.0, "texture_margin_right": 12.0,
        "texture_margin_bottom": 12.0})"));
    CHECK(styles["hover"]["modulate_color"] == parse("[0.9, 0.9, 0.9, 1.0]"));
    CHECK(styles["pressed"]["modulate_color"] == parse("[0.78, 0.78, 0.78, 1.0]"));
    CHECK(styles["hover_pressed"] == styles["pressed"]);
    CHECK(styles["disabled"]["modulate_color"] == parse("[0.78, 0.78, 0.78, 0.5]"));
    CHECK(styles["focus"]["modulate_color"] == parse("[0.96, 0.96, 0.96, 1.0]"));
    CHECK(play["Control"]["children"] == parse(R"(["text"])"));
    json::Value text = entity(files, menu, "text");
    CHECK(text["Label"]["text"].as_string() == "Play");
    CHECK(text["Control"]["theme_override_font_sizes"] == parse(R"({"font_size": 16})"));
    CHECK(text["Control"].find("theme_override_fonts") == nullptr); // Arial: the default font

    // Unity's default toggle is a check box; its background and checkmark are part of it.
    json::Value sound = entity(files, menu, "sound");
    CHECK(sound["CheckBox"]["button_pressed"].as_bool());
    CHECK(sound["Control"]["children"] == parse(R"(["label"])"));
    const json::Value report = parse(files.at("import/unity.json"));
    const json::Value& objects =
        *report.find("scenes")->find("Assets/Scenes/Menu.unity")->find("entities");
    CHECK(objects.find("Canvas/Sound/Background/Checkmark")->as_string() == "sound");

    // A slider takes its background and fill as its track's styles, as thick as the background.
    json::Value volume = entity(files, menu, "volume");
    CHECK(volume["Slider"]["max_value"].as_double() == 10.0);
    CHECK(volume["Slider"]["value"].as_double() == 7.0);
    CHECK(volume["Slider"].find("step") == nullptr); // 1, whole numbers
    CHECK(volume["Control"]["theme_override_styles"]["slider"] ==
          parse(R"({"type": "flat", "bg_color": [1.0, 1.0, 1.0, 1.0], "corner_radius": 3,
                    "content_margin_top": 5.0, "content_margin_bottom": 5.0})"));
    CHECK(volume["Control"]["theme_override_styles"]["grabber_area"]["bg_color"] ==
          parse("[0.2, 0.8, 0.4, 1.0]"));
    CHECK(objects.find("Canvas/Volume/Handle Slide Area/Handle")->as_string() == "volume");

    // A TextMesh Pro input field is a LineEdit; its text area's insets are its styles' content
    // margins.
    json::Value name = entity(files, menu, "name");
    CHECK(name["LineEdit"]["text"].as_string() == "Mira");
    CHECK(name["LineEdit"]["placeholder_text"].as_string() == "Your name");
    CHECK(name["LineEdit"]["max_length"].as_int() == 12);
    CHECK(name["LineEdit"]["caret_blink"].as_bool());
    CHECK(name["Control"]["theme_override_colors"]["font_placeholder_color"] ==
          parse("[0.2, 0.2, 0.2, 0.5]"));
    CHECK(name["Control"]["theme_override_styles"]["normal"] ==
          parse(R"({"type": "flat", "bg_color": [1.0, 1.0, 1.0, 1.0], "corner_radius": 3,
                    "content_margin_left": 10.0, "content_margin_top": 7.0,
                    "content_margin_right": 10.0, "content_margin_bottom": 6.0})"));
    CHECK(objects.find("Canvas/Name/Text Area/Text")->as_string() == "name");

    // A scroll view is a ScrollContainer holding its content; a layout group with padding is a
    // MarginContainer around a BoxContainer, and its children take their sizes as minimums.
    json::Value list = entity(files, menu, "list");
    CHECK(list["ScrollContainer"]["horizontal_scroll_mode"].as_string() == "disabled");
    CHECK(list["ScrollContainer"].find("vertical_scroll_mode") == nullptr); // auto
    CHECK(list["Control"]["children"] == parse(R"(["content"])"));
    json::Value content = entity(files, menu, "content");
    REQUIRE(content.find("MarginContainer") != nullptr);
    CHECK(content["Control"]["children"] == parse(R"(["content_layout"])"));
    CHECK(content["Control"]["size_flags_horizontal"].as_string() == "expand_fill");
    CHECK(content["Control"]["theme_override_constants"] ==
          parse(R"({"margin_left": 4, "margin_top": 4, "margin_right": 4, "margin_bottom": 4})"));
    json::Value layout = entity(files, menu, "content_layout");
    CHECK(layout["BoxContainer"]["vertical"].as_bool());
    CHECK(layout["Control"]["theme_override_constants"] == parse(R"({"separation": 2})"));
    CHECK(layout["Control"]["children"] == parse(R"(["lily", "reed"])"));
    CHECK(entity(files, menu, "lily")["Control"]["custom_minimum_size"] == parse("[0.0, 24.0]"));

    // A grid layout group is a GridContainer of cells as big as its cell size.
    json::Value grid = entity(files, menu, "grid");
    CHECK(grid["GridContainer"]["columns"].as_int() == 2);
    CHECK(grid["Control"]["theme_override_constants"] ==
          parse(R"({"h_separation": 4, "v_separation": 4})"));
    json::Value gem = entity(files, menu, "gem");
    CHECK(gem["Control"]["custom_minimum_size"] == parse("[32.0, 32.0]"));
    CHECK(gem["TextureRect"]["texture"].as_string() == "unity/Assets/UI/icon.png");

    // A raw image is a TextureRect, scaled; an inactive object is hidden, faded by its group.
    json::Value logo = entity(files, menu, "logo");
    CHECK(logo["Control"]["scale"] == parse("[1.5, 1.5]"));
    CHECK(logo["Control"]["mouse_filter"].as_string() == "ignore");
    json::Value options = entity(files, menu, "options");
    CHECK_FALSE(options["Control"]["visible"].as_bool());
    CHECK(options["Control"]["modulate"] == parse("[1.0, 1.0, 1.0, 0.5]"));
    CHECK(options["ColorRect"]["color"] == parse("[0.1, 0.2, 0.3, 1.0]"));

    // A UI prefab is a scene of controls, which its instances extend; a canvas in the world is
    // a control of its own.
    const std::string badge = "unity/Assets/UI/Badge.scene.json";
    json::Value badge_root = entity(files, badge, "badge");
    CHECK(badge_root["Control"]["offset_left"].as_double() == -20.0);
    CHECK(badge_root["Control"]["children"] == parse(R"(["count"])"));
    CHECK(badge_root["Control"]["theme_override_styles"]["panel"]["bg_color"] ==
          parse("[1.0, 0.0, 0.0, 1.0]"));
    CHECK(entity(files, badge, "count")["Label"]["text"].as_string() == "3");
    const json::Value menu_scene = parse(files.at(menu));
    const json::Value& instanced = *menu_scene.find("entities")->find("badge");
    CHECK(instanced.find("extends")->as_string() == badge);
    CHECK(*instanced.find("components") ==
          parse(R"({"Control": {"offset_left": 130.0, "offset_top": -140.0,
                                "offset_right": 170.0, "offset_bottom": -100.0}})"));
    json::Value board = entity(files, menu, "board");
    CHECK(board["Label"]["text"].as_string() == "Exit");
    CHECK(entity(files, menu, "sign")["Transform2D"].find("children") == nullptr);

    // UI events' calls go in the task of the script they call.
    const json::Value work = parse(files.at("storyboard/work.json"));
    const std::string& task =
        work.find("entries")->find("task/port-menu")->find("text")->as_string();
    CHECK(task.find("play in unity/Assets/Scenes/Menu.scene.json calls StartGame() on pressed") !=
          std::string::npos);
    CHECK(task.find("name in unity/Assets/Scenes/Menu.scene.json calls SetName() on "
                    "text_submitted") != std::string::npos);
    // The menu's skeleton calls the script's methods on those events: the play button's with
    // nothing, as Unity does, and the others with the event that carries the value.
    const std::string& skeleton = files.at("src/imported/menu.cpp");
    CHECK(skeleton.find("        if (event.kind == \"pressed\" && event.id == \"play\") {\n"
                        "            const entt::entity entity = world.find(\"canvas\");\n"
                        "            if (ecs.all_of<Menu>(entity)) {\n"
                        "                start_game(world, entity);\n") != std::string::npos);
    CHECK(skeleton.find("set_volume(world, entity, event);") != std::string::npos);
    CHECK(skeleton.find("// run() calls it when volume emits value_changed. Unity passed float "
                        "volume: read it from\n// event.data.") != std::string::npos);
    for (const auto& [what, where] :
         std::initializer_list<std::pair<std::string_view, std::string_view>>{
             {"CanvasScaler", "Assets/Scenes/Menu.unity: Canvas"},
             {"rich text", "Assets/Scenes/Menu.unity: Canvas/Title"},
             {"World Space canvas", "Assets/Scenes/Menu.unity: Sign/Board"},
             {"component EventSystem", "Assets/Scenes/Menu.unity: EventSystem"}}) {
        const json::Value item = unsupported(report, what);
        REQUIRE_MESSAGE(!item.is_null(), what);
        CHECK(item.find("where")->as_string() == where);
    }

    // It lays out as Unity places it, at twice the size.
    slash::world::ComponentRegistry components;
    add_types(components, files);
    command::Registry commands;
    command::register_builtin_commands(commands);
    command::SessionOptions options_;
    options_.project_root = project.string();
    options_.capture_logs = false;
    command::Session session(components, commands, options_);
    const command::Reply opened = session.call("scene.open", parse(R"({"path": ")" + menu + "\"}"));
    REQUIRE_MESSAGE(opened.ok, opened.message);
    const command::Reply laid =
        session.call("ui.layout", parse(R"({"ids": ["title", "play", "volume", "name", "list",
                                                     "lily", "key", "logo"]})"));
    REQUIRE_MESSAGE(laid.ok, laid.message);
    CHECK(laid.value.find("problems") == nullptr);
    std::map<std::string, json::Value> rects;
    for (const json::Value& control : laid.value.find("controls")->as_array()) {
        rects[control.find("id")->as_string()] = *control.find("rect");
    }
    CHECK(rects["title"] == parse("[100, 20, 600, 120]"));
    CHECK(rects["play"] == parse("[240, 220, 320, 80]"));
    CHECK(rects["volume"] == parse("[240, 380, 320, 40]"));
    CHECK(rects["name"] == parse("[240, 430, 320, 60]"));
    CHECK(rects["list"] == parse("[540, 20, 240, 560]"));
    CHECK(rects["lily"] == parse("[548, 28, 224, 48]"));
    CHECK(rects["key"] == parse("[92, 380, 64, 64]"));
    CHECK(rects["logo"] == parse("[16, 16, 192, 96]"));
}

TEST_CASE("Unity dropdowns become option buttons, their captions, arrows and lists part of them") {
    const fs::path project = "import_unity_options";
    fs::remove_all(project);
    make_project(project / "unity");
    write_menu(project / "unity");
    write_options(project / "unity");
    auto imported = importer::import_project(
        {.source = (project / "unity").string(), .destination = project.string()});
    REQUIRE_MESSAGE(imported.ok(), (imported.ok() ? std::string() : imported.error().message));
    const auto files = files_of(imported.value());
    const std::string scene = "unity/Assets/Scenes/Options.scene.json";

    // Its options, the one shown, its background's styles and its caption's look; the caption,
    // the arrow and the template are part of it.
    json::Value quality = entity(files, scene, "quality");
    CHECK(quality["OptionButton"] == parse(R"({"selected": 1, "items": [{"text": "Low"},
        {"text": "High", "icon": "unity/Assets/UI/icon.png"}]})"));
    CHECK(quality["Control"].find("children") == nullptr);
    // Unity's rounded sprite draws as a flat style box.
    CHECK(quality["Control"]["theme_override_styles"]["normal"]["type"].as_string() == "flat");
    CHECK(quality["Control"]["theme_override_font_sizes"] == parse(R"({"font_size": 14})"));
    CHECK(quality["Control"].find("theme_override_icons") == nullptr); // Unity's arrow
    const json::Value report = parse(files.at("import/unity.json"));
    const json::Value& objects =
        *report.find("scenes")->find("Assets/Scenes/Options.unity")->find("entities");
    CHECK(objects.find("Canvas/Quality/Template")->as_string() == "quality");
    CHECK(unsupported(report, "Dropdown template").find("where")->as_string() ==
          "Assets/Scenes/Options.unity: Canvas/Quality");

    // A value past the options is the last; one that can't be used is disabled, and an arrow
    // of the project's own is the button's arrow icon.
    json::Value mode = entity(files, scene, "mode");
    CHECK(mode["OptionButton"] == parse(R"({"selected": 1, "disabled": true,
        "items": [{"text": "Easy"}, {"text": "Hard"}]})"));
    CHECK(mode["Control"]["theme_override_icons"] ==
          parse(R"({"arrow": "unity/Assets/UI/icon.png"})"));
    CHECK(mode["Control"]["theme_override_styles"]["normal"]["texture"].as_string() ==
          "unity/Assets/UI/button.png");
    CHECK(objects.find("Canvas/Mode/Placeholder")->as_string() == "mode");

    // Picking an option calls the script's method, in its task.
    const json::Value work = parse(files.at("storyboard/work.json"));
    const std::string& task =
        work.find("entries")->find("task/port-menu")->find("text")->as_string();
    CHECK(task.find("quality in " + scene + " calls SetQuality() on item_selected") !=
          std::string::npos);

    // It opens its list as an OptionButton does.
    slash::world::ComponentRegistry components;
    add_types(components, files);
    command::Registry commands;
    command::register_builtin_commands(commands);
    command::SessionOptions options;
    options.project_root = project.string();
    options.capture_logs = false;
    command::Session session(components, commands, options);
    const command::Reply opened =
        session.call("scene.open", parse(R"({"path": ")" + scene + "\"}"));
    REQUIRE_MESSAGE(opened.ok, opened.message);
    const command::Reply popup = session.call("ui.popup", parse(R"({"id": "quality"})"));
    REQUIRE_MESSAGE(popup.ok, popup.message);
    const command::Reply laid = session.call("ui.layout", parse(R"({"ids": ["quality"]})"));
    REQUIRE_MESSAGE(laid.ok, laid.message);
    CHECK(laid.value.find("problems") == nullptr);
    const json::Value& list = laid.value.find("popups")->as_array().at(0);
    CHECK(list.find("focused_item")->as_int() == 1);
    CHECK(list.find("items")->as_array().size() == 2);
}

TEST_CASE("Unity's 2D lights become Godot's lights, its shadow casters occluders") {
    const fs::path project = "import_unity_lights";
    fs::remove_all(project);
    make_project(project / "unity");
    write_lights(project / "unity");
    const auto files = files_of(plan_inside(project));
    const std::string lights = "unity/Assets/Scenes/Lights.scene.json";
    const auto image = [&files](const std::string& path) {
        REQUIRE_MESSAGE(files.contains(path), path);
        auto decoded = slash::render::decode_image(files.at(path));
        REQUIRE(decoded.ok());
        return std::move(decoded).value();
    };
    const auto alpha = [](const slash::render::Image& made, std::uint32_t x, std::uint32_t y) {
        return static_cast<int>(made.pixels[(((std::size_t{y} * made.width) + x) * 4) + 3]);
    };

    // The global light is the scene's CanvasModulate: its color times its intensity.
    CHECK(entity(files, lights, "global_light")["CanvasModulate"]["color"] ==
          parse("[0.4, 0.4, 0.8, 1.0]"));

    // A spot light is a PointLight2D through a texture of its falloff, as wide as the light: full
    // inside the inner radius, a quarter of the outer, then fading linearly at the default
    // falloff strength. The default sorting layer's z indices, normal maps and soft shadows.
    json::Value torch = entity(files, lights, "torch")["PointLight2D"];
    CHECK(torch["texture"].as_string() == "unity/Assets/lights/spot_25_50.png");
    CHECK(torch["texture_scale"].as_double() == 0.5); // 128 pixels across, from 256
    CHECK(torch["color"] == parse("[1.0, 0.8, 0.6, 1.0]"));
    CHECK(torch["energy"].as_double() == 1.5);
    CHECK(torch["height"].as_double() == 32.0);
    CHECK(torch["range_z_min"].as_int() == -499);
    CHECK(torch["range_z_max"].as_int() == 499);
    CHECK(torch["shadow_enabled"].as_bool());
    CHECK(torch["shadow_filter"].as_string() == "pcf13");
    CHECK(torch["shadow_filter_smooth"].as_double() == 3.0);
    const slash::render::Image spot = image("unity/Assets/lights/spot_25_50.png");
    CHECK(spot.width == 256);
    CHECK(alpha(spot, 128, 128) == 255);
    CHECK(alpha(spot, 128 + 31, 128) == 255); // inside the inner radius
    CHECK(alpha(spot, 128 + 80, 128) == doctest::Approx(128).epsilon(0.02)); // halfway out
    CHECK(alpha(spot, 255, 128) <= 1);                                       // the edge
    CHECK(alpha(spot, 0, 0) == 0);

    // A cone points up the texture, as Unity's points along the object's y axis; its strongest
    // falloff dims it soon. Volumetric lights and other blend styles are reported.
    json::Value beam = entity(files, lights, "beam")["PointLight2D"];
    CHECK(beam["texture"].as_string() == "unity/Assets/lights/spot_0_100_30_60.png");
    CHECK(beam["range_z_min"].as_int() == 501);
    CHECK(beam["range_z_max"].as_int() == 1499);
    CHECK(beam.find("shadow_enabled") == nullptr);
    CHECK(beam.find("height") == nullptr); // URP's default: normal maps off
    const slash::render::Image cone = image("unity/Assets/lights/spot_0_100_30_60.png");
    CHECK(alpha(cone, 128, 120) > 150); // just above the middle
    CHECK(alpha(cone, 128, 136) == 0);  // below it, behind the light
    CHECK(alpha(cone, 160, 128) == 0);  // beside it, outside the cone
    // The object's second light is on an entity under it.
    CHECK(entity(files, lights, "beam_light_2")["PointLight2D"]["texture"].as_string() ==
          "unity/Assets/lights/spot_0_50.png");
    CHECK(entity(files, lights, "beam")["Transform2D"]["children"] == parse(R"(["beam_light_2"])"));

    // A freeform light's texture covers its polygon and falloff, full inside and fading outside.
    json::Value window = entity(files, lights, "window")["PointLight2D"];
    const std::string window_texture = window["texture"].as_string();
    CHECK(window_texture.starts_with("unity/Assets/lights/freeform_"));
    CHECK(window["texture_scale"].as_double() == 0.1875); // 48 pixels across
    CHECK(window["offset"] == parse("[16.0, 0.0]"));      // the middle of the shape
    CHECK(window["range_z_min"].as_int() == -1499);       // the back and front layers, and between
    CHECK(window["range_z_max"].as_int() == 1499);
    const slash::render::Image freeform = image(window_texture);
    CHECK(alpha(freeform, 128, 128) == 255);
    CHECK(alpha(freeform, 21, 128) == doctest::Approx(128).epsilon(0.02)); // halfway out
    CHECK(alpha(freeform, 0, 0) == 0);

    // A sprite light shines through the sprite's part of its sheet, cut out, at the sprite's size
    // in units with its pivot on the light.
    json::Value lamp = entity(files, lights, "lamp")["PointLight2D"];
    CHECK(lamp["texture"].as_string() == "unity/Assets/lights/sheet_1.png");
    CHECK(lamp["texture_scale"].as_double() == 0.5);
    CHECK(lamp["offset"] == parse("[4.0, 2.0]"));
    const slash::render::Image cookie = image("unity/Assets/lights/sheet_1.png");
    CHECK(cookie.width == 16);
    CHECK(cookie.height == 8);
    CHECK(entity(files, lights, "old").find("PointLight2D") == nullptr);

    // Shadow casters are occluders: their own shape, or the collider's they take it from. Sprites
    // take their texture's normal map.
    CHECK(entity(files, lights, "crate")["LightOccluder2D"]["polygon"] ==
          parse("[[-8.0, 8.0], [8.0, 8.0], [8.0, -8.0], [-8.0, -8.0]]"));
    CHECK(entity(files, lights, "crate")["Sprite2D"]["normal_texture"].as_string() ==
          "unity/Assets/Art/glow_n.png");
    CHECK(entity(files, lights, "box")["LightOccluder2D"]["polygon"] ==
          parse("[[-8.0, -8.0], [24.0, -8.0], [24.0, 8.0], [-8.0, 8.0]]"));
    CHECK(entity(files, lights, "ghost").find("LightOccluder2D") == nullptr);
    CHECK(entity(files, lights, "group").find("LightOccluder2D") == nullptr);
    CHECK(entity(files, lights, "ambient").is_null()); // the global light lights the rest

    const json::Value report = parse(files.at("import/unity.json"));
    for (const std::string_view what :
         {"volumetric Light 2D", "Light 2D blend style", "Light 2D shadow strength below 1",
          "Light 2D on sorting layers apart", "Parametric Light 2D",
          "ShadowCaster2D that shadows only itself"}) {
        CHECK_MESSAGE(unsupported(report, what) != nullptr, what);
    }
    CHECK(unsupported(report, "component CompositeShadowCaster2D") == nullptr);
    CHECK(unsupported(report, "Light 2D shadow strength below 1").find("where")->as_string() ==
          "Assets/Scenes/Lights.unity: Window");

    // A scene whose lights have no global light to fill in is dark where they don't reach. Lights
    // saved before URP versioned them take shadows from their strength, normal maps from a flag.
    const std::string night = "unity/Assets/Scenes/Night.scene.json";
    CHECK(entity(files, night, "ambient")["CanvasModulate"]["color"] ==
          parse("[0.0, 0.0, 0.0, 1.0]"));
    json::Value moon = entity(files, night, "moon")["PointLight2D"];
    CHECK(moon.find("shadow_enabled") == nullptr);
    CHECK(moon["height"].as_double() == 48.0);
    CHECK(unsupported(report, "2D lights without a Global Light 2D") != nullptr);

    // In a session, the torch lights a white pixel under it with its color on top of the ambient.
    REQUIRE(importer::import_project(
                {.source = (project / "unity").string(), .destination = project.string()})
                .ok());
    slash::world::ComponentRegistry components;
    slash::world::register_builtin_components(components);
    command::Registry commands;
    command::register_builtin_commands(commands);
    command::SessionOptions options;
    options.project_root = project.string();
    options.capture_logs = false;
    command::Session session(components, commands, options);
    json::Value params;
    params["path"] = lights;
    const command::Reply opened = session.call("scene.open", params);
    REQUIRE_MESSAGE(opened.ok, opened.message);
    const command::Reply sampled =
        session.call("light.sample", parse(R"({"points": [[32, -16]]})"));
    REQUIRE_MESSAGE(sampled.ok, sampled.message);
    CHECK(*sampled.value.find("ambient") == parse("[0.4, 0.4, 0.8]"));
    const json::Value& point = sampled.value.find("points")->as_array()[0];
    CHECK(*point.find("color") == parse("[1.9, 1.6, 1.7]")); // as URP adds them
    CHECK(point.find("lights")->as_array()[0].find("id")->as_string() == "torch");
}

namespace {

// The GUIDs AssetRipper gives the scripts it writes, which aren't the ones Unity's packages have.
constexpr std::string_view kRipHop = "b2000000000000000000000000000001";
constexpr std::string_view kRipImage = "b2000000000000000000000000000002";
constexpr std::string_view kRipMask = "b2000000000000000000000000000003";
constexpr std::string_view kRipEvents = "b2000000000000000000000000000004";
constexpr std::string_view kRipModule = "b2000000000000000000000000000005";
constexpr std::string_view kRipSheet = "b2000000000000000000000000000006";
constexpr std::string_view kRipCoin = "b2000000000000000000000000000007";
constexpr std::string_view kRipTrimmed = "b2000000000000000000000000000008";
constexpr std::string_view kRipTurned = "b2000000000000000000000000000009";
constexpr std::string_view kRipTight = "b200000000000000000000000000000a";

// A sprite saved as an asset of its own, as AssetRipper writes a build's sprites: its rect on
// the texture ("x, y, width, height", from the bottom left), where its pixels are after
// trimming, its pivot, pixels per unit, packing settings and physics shape in units.
std::string sprite_asset(std::string_view name, std::string_view rect,
                         std::string_view texture_rect, std::string_view offset,
                         std::string_view pivot, int ppu, int settings,
                         std::string_view shape = "  m_PhysicsShape: []\n") {
    const auto rect_of = [](std::string_view text, std::string_view indent) {
        std::array<std::string, 4> parts;
        std::size_t part = 0;
        for (const char c : text) {
            if (c == ',') {
                ++part;
            } else if (c != ' ') {
                parts.at(part) += c;
            }
        }
        const std::string in(indent);
        return in + "serializedVersion: 2\n" + in + "x: " + parts[0] + "\n" + in +
               "y: " + parts[1] + "\n" + in + "width: " + parts[2] + "\n" + in +
               "height: " + parts[3] + "\n";
    };
    return std::string(kHeader) + "--- !u!213 &21300000\nSprite:\n" + std::string(kSource) +
           "  m_Name: " + std::string(name) + "\n  m_Rect:\n" + rect_of(rect, "    ") +
           "  m_Offset: {x: 0, y: 0}\n  m_Border: {x: 0, y: 0, z: 0, w: 0}\n"
           "  m_PixelsToUnits: " +
           std::to_string(ppu) + "\n  m_Pivot: " + std::string(pivot) +
           "\n  m_Extrude: 1\n  m_RD:\n    serializedVersion: 3\n    texture: " +
           ref(2800000, kRipSheet) + "\n    alphaTexture: {fileID: 0}\n    textureRect:\n" +
           rect_of(texture_rect, "      ") + "    textureRectOffset: " + std::string(offset) +
           "\n    settingsRaw: " + std::to_string(settings) + "\n" + std::string(shape);
}

// "Hopper" as AssetRipper rebuilds its project from an IL2CPP build: the scripts of Unity's
// packages decompiled under Assets/Scripts/<assembly>/ with GUIDs of AssetRipper's own, the
// game's script a stand-in whose code didn't come back, sprites as assets of their own, and a
// scene whose UI components kept their values or, read without the build's scripts, lost them.
void write_ripped(const fs::path& folder) {
    fs::remove_all(folder);
    write(
        folder / "ProjectSettings/ProjectVersion.txt",
        "m_EditorVersion: 2022.3.20f1\nm_EditorVersionWithRevision: 2022.3.20f1 (61c2feb0970d)\n");
    write(folder / "ProjectSettings/ProjectSettings.asset",
          std::string(kHeader) +
              "--- !u!129 &1\nPlayerSettings:\n  m_ObjectHideFlags: 0\n  companyName: Test\n"
              "  productName: Hopper\n  defaultScreenWidth: 320\n  defaultScreenHeight: 180\n");
    const auto script_file = [&](std::string_view path, std::string_view guid,
                                 std::string_view code) {
        write(folder / "Assets/Scripts" / path, code);
        write(folder / "Assets/Scripts" / (std::string(path) + ".meta"),
              meta(guid, "MonoImporter"));
    };
    script_file("Assembly-CSharp/Hop.cs", kRipHop,
                "using UnityEngine;\n\npublic class Hop : MonoBehaviour\n{\n\t/*\n\tDummy class. "
                "This could have happened for several reasons:\n\t*/\n}\n");
    script_file("UnityEngine.UI/UnityEngine/UI/Image.cs", kRipImage,
                "namespace UnityEngine.UI\n{\n\tpublic class Image : MaskableGraphic {}\n}\n");
    script_file("UnityEngine.UI/UnityEngine/UI/RectMask2D.cs", kRipMask,
                "namespace UnityEngine.UI\n{\n\tpublic class RectMask2D : UIBehaviour {}\n}\n");
    script_file("UnityEngine.UI/UnityEngine/EventSystems/EventSystem.cs", kRipEvents,
                "namespace UnityEngine.EventSystems\n{\n\tpublic class EventSystem : "
                "UIBehaviour {}\n}\n");
    script_file("Unity.InputSystem/UnityEngine/InputSystem/UI/InputSystemUIInputModule.cs",
                kRipModule,
                "namespace UnityEngine.InputSystem.UI\n{\n\tpublic class InputSystemUIInputModule "
                ": BaseInputModule {}\n}\n");
    write(folder / "Assets/Scripts/UnityEngine.UI.meta", meta("b20000000000000000000000000000f1"));

    write_png(folder / "Assets/Texture2D/sheet.png", 64, 32);
    write(folder / "Assets/Texture2D/sheet.png.meta",
          texture_meta(kRipSheet, 100, "  spriteMode: 1\n"));
    const auto sprite_file = [&](std::string_view name, std::string_view guid,
                                 const std::string& text) {
        write(folder / "Assets/Sprite" / (std::string(name) + ".asset"), text);
        write(folder / "Assets/Sprite" / (std::string(name) + ".asset.meta"),
              meta(guid, "NativeFormatImporter", "  mainObjectFileID: 21300000\n"));
    };
    // The coin, the sheet's top right square, with a square physics shape.
    sprite_file("coin", kRipCoin,
                sprite_asset("coin", "48, 16, 16, 16", "48, 16, 16, 16", "{x: 0, y: 0}",
                             "{x: 0.5, y: 0.5}", 16, 64,
                             "  m_PhysicsShape:\n  - - {x: -0.5, y: 0.5}\n"
                             "    - {x: -0.5, y: -0.5}\n    - {x: 0.5, y: -0.5}\n"
                             "    - {x: 0.5, y: 0.5}\n"));
    // A 32x16 sprite standing on its bottom middle, its 4 transparent columns on each side
    // trimmed off.
    sprite_file("trimmed", kRipTrimmed,
                sprite_asset("trimmed", "0, 0, 32, 16", "4, 0, 24, 16", "{x: 4, y: 0}",
                             "{x: 0.5, y: 0}", 16, 64));
    // Packed into an atlas turned, and packed tightly.
    sprite_file("turned", kRipTurned,
                sprite_asset("turned", "0, 16, 16, 16", "0, 16, 16, 16", "{x: 0, y: 0}",
                             "{x: 0.5, y: 0.5}", 16, 1 | (1 << 2)));
    sprite_file("tight", kRipTight,
                sprite_asset("tight", "16, 16, 16, 16", "16, 16, 16, 16", "{x: 0, y: 0}",
                             "{x: 0.5, y: 0.5}", 16, 1));

    std::string scene(kHeader);
    const auto sprite_object = [&](std::int64_t object, std::string_view name,
                                   std::string_view guid, std::string_view position) {
        scene += game_object(object, name, {object + 1, object + 2});
        scene += transform(object + 1, object, position);
        scene += sprite_renderer(object + 2, object, ref(21300000, guid, 2));
    };
    sprite_object(100, "Coin", kRipCoin, "{x: 0, y: 0, z: 0}");
    sprite_object(110, "Ledge", kRipTrimmed, "{x: 2, y: 0, z: 0}");
    sprite_object(120, "Turned", kRipTurned, "{x: 4, y: 0, z: 0}");
    sprite_object(130, "Tight", kRipTight, "{x: 6, y: 0, z: 0}");
    // The player, with the game's script.
    scene += game_object(200, "Player", {201, 202});
    scene += transform(201, 200, "{x: 0, y: 2, z: 0}");
    scene += script(202, 200, kRipHop, "  jumpHeight: 3\n");
    // A tile map of coins, whose collider takes the coin's physics shape.
    scene += game_object(300, "Grid", {301, 302});
    scene += transform(301, 300, "{x: 0, y: 0, z: 0}", 0, {311});
    scene += component(156049354, "Grid", 302, 300,
                       "  m_CellSize: {x: 1, y: 1, z: 0}\n  m_CellGap: {x: 0, y: 0, z: 0}\n"
                       "  m_CellLayout: 0\n  m_CellSwizzle: 0\n");
    scene += game_object(310, "Coins", {311, 312, 313, 314});
    scene += transform(311, 310, "{x: 0, y: 0, z: 0}", 301);
    scene += component(
        1839735485, "Tilemap", 312, 310,
        // Collider type 1 (the sprite's physics shape) in the flags' top bits.
        "  m_Tiles:\n" + tile_cell(0, 0, 0, 0, 0, 0, (std::int64_t{1} << 30) | 1) +
            "  m_TileSpriteArray:\n  - m_RefCount: 1\n    m_Data: " + ref(21300000, kRipCoin, 2) +
            "\n  m_TileMatrixArray:\n" + tile_matrix(1, 0, 0, 1) +
            "  m_TileAnchor: {x: 0.5, y: 0.5, z: 0}\n  m_TileOrientation: 0\n");
    scene += component(483693784, "TilemapRenderer", 313, 310,
                       "  m_SortingLayerID: 0\n  m_SortingOrder: 0\n  m_MaskInteraction: 0\n");
    scene += component(19719996, "TilemapCollider2D", 314, 310,
                       "  m_Density: 1\n  m_Material: {fileID: 0}\n  m_IsTrigger: 0\n"
                       "  m_UsedByEffector: 0\n  m_Offset: {x: 0, y: 0}\n");
    // The HUD: a panel whose Image kept its values (a Mono build's, or one bundled with its
    // type tree), one whose Image lost them, and a RectMask2D, which has none to lose.
    scene += ui_object(1000, "Hud", {1001, 1009, 1002});
    scene +=
        rect_transform(1001, 1000, "0, 0", "0, 0", "0, 0", "0, 0", "0, 0", 0, {1101, 1201, 1301});
    scene += component(223, "Canvas", 1002, 1000,
                       "  serializedVersion: 3\n  m_RenderMode: 0\n  m_Camera: {fileID: 0}\n"
                       "  m_PixelPerfect: 0\n  m_OverrideSorting: 0\n  m_SortingOrder: 0\n");
    scene += ui_object(1100, "Panel", {1101, 1109, 1102});
    scene += rect_transform(1101, 1100, "0, 1", "0, 1", "40, -20", "80, 40", "0.5, 0.5", 1001);
    scene += script(1102, 1100, kRipImage,
                    "  m_Material: {fileID: 0}\n  m_Color: {r: 0, g: 0, b: 1, a: 1}\n"
                    "  m_RaycastTarget: 1\n  m_Sprite: {fileID: 0}\n  m_Type: 0\n"
                    "  m_PreserveAspect: 0\n  m_FillCenter: 1\n");
    scene += ui_object(1200, "Lost", {1201, 1209, 1202});
    scene += rect_transform(1201, 1200, "1, 1", "1, 1", "-40, -20", "80, 40", "0.5, 0.5", 1001);
    scene += script(1202, 1200, kRipImage, "");
    scene += ui_object(1300, "Clip", {1301, 1309, 1302});
    scene += rect_transform(1301, 1300, "0, 0", "1, 1", "0, 0", "0, 0", "0.5, 0.5", 1001);
    scene += script(1302, 1300, kRipMask, "");
    // The event system, with the Input System's module.
    scene += game_object(1400, "EventSystem", {1401, 1402, 1403});
    scene += transform(1401, 1400, "{x: 0, y: 0, z: 0}");
    scene += script(1402, 1400, kRipEvents, "  m_FirstSelected: {fileID: 0}\n");
    scene += script(1403, 1400, kRipModule, "  m_MoveRepeatDelay: 0.5\n");
    write(folder / "Assets/Scenes/Main.unity", scene);
    write(folder / "Assets/Scenes/Main.unity.meta", meta("b20000000000000000000000000000f2"));
}

// An IL2CPP build for Linux: what find_build looks for. The stand-in AssetRipper exports the
// folder FAKE_ASSETRIPPER_PROJECT names, whatever the build holds.
void write_build(const fs::path& folder) {
    write(folder / "Hopper.x86_64", "");
    write(folder / "GameAssembly.so", "");
    write(folder / "UnityPlayer.so", "");
    write(folder / "Hopper_Data/globalgamemanagers", "");
    write(folder / "Hopper_Data/il2cpp_data/Metadata/global-metadata.dat", "");
}

// Sets an environment variable for the length of a test.
struct ScopedVariable {
    std::string name;
    ScopedVariable(std::string variable, const std::string& value) : name(std::move(variable)) {
        REQUIRE(SDL_setenv_unsafe(name.c_str(), value.c_str(), 1) == 0);
    }
    ScopedVariable(const ScopedVariable&) = delete;
    ScopedVariable& operator=(const ScopedVariable&) = delete;
    ScopedVariable(ScopedVariable&&) = delete;
    ScopedVariable& operator=(ScopedVariable&&) = delete;
    ~ScopedVariable() { SDL_unsetenv_unsafe(name.c_str()); }
};

} // namespace

TEST_CASE("a Unity build imports through AssetRipper, its package scripts and sprites fixed") {
    const fs::path project = fs::absolute("import_unity_build");
    fs::remove_all(project);
    write_ripped(project / "ripped");
    write_build(project / "Hopper");
    const ScopedVariable exported("FAKE_ASSETRIPPER_PROJECT", (project / "ripped").string());
    auto plan = importer::plan_import({.source = (project / "Hopper").string(),
                                       .destination = project.string(),
                                       .unpack_to = {},
                                       .assetripper = SLASH_FAKE_ASSETRIPPER});
    REQUIRE_MESSAGE(plan.ok(), (plan.ok() ? std::string() : plan.error().message));
    const json::Value& report = plan.value().report;
    CHECK(*report.find("unpacked") == parse(R"({
        "assetripper": "2.0.0",
        "build": {"backend": "il2cpp", "platform": "linux", "unity": "2022.3.20f1"},
        "files": 7, "log": ".slash/import/unity/assetripper.log", "package_scripts": 3,
        "prefabs": 0, "scenes": 1, "scripts": 1,
        "unsupported": [{"count": 1, "what": "script without its code",
                         "where": "Assets/Scripts/Assembly-CSharp/Hop.cs",
                         "why": "the build's code couldn't be read back (IL2CPP builds keep it compiled), so only the script's name and the objects using it came back"}]})"));

    // The package scripts' references now name Unity's GUIDs, and the scripts of Unity's own
    // assemblies are gone; a script the importer doesn't know is named as Unity 6 names one.
    const fs::path ripped = project / ".slash/import/unity/export/ExportedProject";
    const std::string main =
        slash::platform::read_file((ripped / "Assets/Scenes/Main.unity").string()).value();
    CHECK(main.find(std::string(kImageCs)) != std::string::npos);
    CHECK(main.find(std::string(kRipImage)) == std::string::npos);
    CHECK(main.find(std::string(kRipModule)) != std::string::npos);
    CHECK(main.find("m_EditorClassIdentifier: "
                    "Unity.InputSystem::UnityEngine.InputSystem.UI.InputSystemUIInputModule\n") !=
          std::string::npos);
    CHECK_FALSE(fs::exists(ripped / "Assets/Scripts/UnityEngine.UI"));
    CHECK_FALSE(fs::exists(ripped / "Assets/Scripts/UnityEngine.UI.meta"));
    CHECK_FALSE(fs::exists(ripped / "Assets/Scripts/Unity.InputSystem"));
    CHECK(fs::exists(ripped / "Assets/Scripts/Assembly-CSharp/Hop.cs"));
    CHECK(slash::platform::read_file((project / ".slash/import/unity/assetripper.log").string())
              .value()
              .starts_with("LoadFolder " + (project / "Hopper").string() + "\n"));

    // The project converts as one made in Unity would, its assets copied in.
    const auto files = files_of(plan.value());
    const std::string main_scene = "Assets/Scenes/Main.scene.json";
    CHECK(files.contains("Assets/Texture2D/sheet.png"));
    CHECK(files.contains("Assets/Scripts/Assembly-CSharp/Hop.cs"));
    CHECK_FALSE(files.contains("Assets/Scripts/UnityEngine.UI/UnityEngine/UI/Image.cs"));
    CHECK(*entity(files, main_scene, "coin").find("Sprite2D") ==
          parse(R"({"texture": "Assets/Texture2D/sheet.png", "region": [48.0, 0.0, 16.0, 16.0]})"));
    // Trimmed: its pixels start 4 in, and its pivot is still its rect's bottom middle.
    CHECK(*entity(files, main_scene, "ledge").find("Sprite2D") ==
          parse(R"({"texture": "Assets/Texture2D/sheet.png", "region": [4.0, 16.0, 24.0, 16.0],
                    "offset": [0.0, -8.0]})"));
    CHECK(*entity(files, main_scene, "turned").find("Sprite2D") == parse("{}"));
    const json::Value full = parse(files.at("import/unity.json"));
    CHECK(unsupported(full, "tightly packed sprite").find("why")->as_string() ==
          "packed to its outline in an atlas, so its neighbors may show at its edges");
    CHECK(unsupported(full, "sprite").find("why")->as_string() ==
          "Assets/Sprite/turned.asset: a sprite turned in its atlas");
    // The coin tile's collision is the coin's physics shape, in pixels.
    const json::Value tiles = parse(files.at("Assets/tiles.tileset.json"));
    CHECK(*tiles.find("sources")->find("sheet")->find("tiles")->find("3,0")->find("collision") ==
          parse(R"([{"points": [[-8.0, -8.0], [-8.0, 8.0], [8.0, 8.0], [8.0, -8.0]]}])"));
    // UI: the panel's Image converts, the one read without the build's scripts is reported,
    // and the RectMask2D, which keeps no values, clips.
    CHECK(*entity(files, main_scene, "panel").find("ColorRect") ==
          parse(R"({"color": [0.0, 0.0, 1.0, 1.0]})"));
    CHECK(unsupported(full, "component Image").find("why")->as_string() ==
          "its values didn't survive the game's build: the build's scripts couldn't be read");
    CHECK(entity(files, main_scene, "clip").find("Control")->find("clip_contents")->as_bool());
    CHECK(unsupported(full, "component InputSystemUIInputModule").find("why")->as_string() ==
          "a script from a Unity package");
    CHECK(*report.find("scripts") ==
          parse(R"([{"entities": 1, "script": "Assets/Scripts/Assembly-CSharp/Hop.cs",
                     "task": "task/port-hop", "code": "src/imported/hop.cpp",
                     "component": "Hop"}])"));

    // With unpack_to, the whole Unity project comes along, and the import uses it there.
    auto unpacked = importer::plan_import({.source = (project / "Hopper").string(),
                                           .destination = project.string(),
                                           .unpack_to = "unity",
                                           .assetripper = SLASH_FAKE_ASSETRIPPER});
    REQUIRE(unpacked.ok());
    const auto unpacked_files = files_of(unpacked.value());
    CHECK(unpacked_files.contains("unity/ProjectSettings/ProjectVersion.txt"));
    CHECK(unpacked_files.contains("unity/Assets/Scenes/Main.unity"));
    CHECK(unpacked_files.contains("unity/Assets/Scenes/Main.scene.json"));
    CHECK(unpacked.value().report.find("unpacked")->find("folder")->as_string() == "unity");
    CHECK(entity(unpacked_files, "unity/Assets/Scenes/Main.scene.json", "coin")
              .find("Sprite2D")
              ->find("texture")
              ->as_string() == "unity/Assets/Texture2D/sheet.png");
    fs::remove_all(project);
}

TEST_CASE("project.import unpacks a Unity build only where it may run AssetRipper") {
    const fs::path project = fs::absolute("import_unity_build_command");
    fs::remove_all(project);
    write_ripped(project / "ripped");
    write_build(project / "builds/Hopper");
    const ScopedVariable exported("FAKE_ASSETRIPPER_PROJECT", (project / "ripped").string());
    const ScopedVariable program("SLASH_ASSETRIPPER", SLASH_FAKE_ASSETRIPPER);
    slash::world::ComponentRegistry components;
    slash::world::register_builtin_components(components);
    slash::gameplay::Host code(components, slash::gameplay::library_path(project.string()));
    const json::Value params = parse(R"({"source": "builds/Hopper"})");

    // A session without the build capability.
    command::Registry commands;
    command::register_builtin_commands(commands);
    importer::register_import_commands(commands);
    command::SessionOptions options;
    options.project_root = project.string();
    options.capture_logs = false;
    options.capabilities = {command::Capability::Read, command::Capability::Filesystem};
    command::Session limited(components, commands, options);
    const command::Reply refused = limited.call("project.import", params);
    CHECK_FALSE(refused.ok);
    CHECK(refused.code == command::code::kPermissionDenied);
    CHECK(refused.message == "unpacking a Unity build runs AssetRipper, which needs the \"build\" "
                             "capability, which this session does not grant");
    CHECK(refused.data.find("capability")->as_string() == "build");

    // A running game's commands, which don't unpack builds.
    command::Registry running;
    command::register_builtin_commands(running);
    importer::register_import_commands(running, {.unpack_builds = false});
    options.capabilities = command::SessionOptions{}.capabilities;
    options.gameplay = &code;
    command::Session game(components, running, options);
    const command::Reply stalled = game.call("project.import", params);
    CHECK_FALSE(stalled.ok);
    CHECK(stalled.message ==
          "a running game doesn't unpack Unity builds, which takes minutes; use slash-cli import");

    command::Session session(components, commands, options);
    const command::Reply done = session.call("project.import", params);
    REQUIRE_MESSAGE(done.ok, done.message);
    CHECK(done.value.find("unpacked")->find("build")->find("platform")->as_string() == "linux");
    CHECK(fs::exists(project / "Assets/Scenes/Main.scene.json"));
    CHECK(fs::exists(project / "Assets/Texture2D/sheet.png"));
    CHECK(session.call("scene.open", parse(R"({"path": "Assets/Scenes/Main.scene.json"})")).ok);
    fs::remove_all(project);
}

TEST_CASE("Unity 3D scenes convert mirrored in x: meshes, materials, cameras, lights and bodies") {
    const fs::path project = "import_unity_3d";
    fs::remove_all(project);
    write_hall(project / "unity");
    const auto files = files_of(plan_inside(project));
    const std::string hall = "unity/Assets/Scenes/Hall.scene.json";
    REQUIRE(files.contains(hall));
    slash::world::ComponentRegistry components;
    slash::world::register_builtin_components(components);
    CHECK(slash::world::validate_scene(components, parse(files.at(hall))).empty());

    // Unity's axes are left-handed and Godot's right-handed, so x is mirrored: positions flip in
    // x and rotations turn the other way about y and z. The crate turned 30 degrees about y in
    // Unity turns -30 here.
    const json::Value crate = entity(files, hall, "crate");
    CHECK(*crate.find("Transform3D") == parse(R"({"position": [-3.0, 0.5, 2.0],
        "rotation": [0.0, -0.523599, 0.0], "children": ["label"]})"));
    CHECK(*crate.find("BoxMesh") == parse("{}"));
    CHECK(*crate.find("MeshInstance3D") == parse("{}"));
    // The Standard shader's color, texture (tiled twice, its offset from the top), smoothness and
    // HDR emission, filtered as the texture's import settings say.
    CHECK(*crate.find("StandardMaterial3D") == parse(R"({"albedo_color": [0.8, 0.6, 0.4, 1.0],
                    "albedo_texture": "unity/Assets/Art/wood.png", "uv1_scale": [2.0, 2.0, 1.0],
                    "uv1_offset": [0.0, -1.0, 0.0], "roughness": 0.8, "emission_enabled": true,
                    "emission": [1.0, 0.5, 0.0, 1.0], "emission_energy_multiplier": 2.0,
                    "texture_filter": "nearest_with_mipmaps"})"));
    // Unity's default friction, its colliders meeting every layer, frozen rotations as locks.
    CHECK(*crate.find("RigidBody3D") ==
          parse(R"({"mass": 2.0, "linear_damp": 0.1, "angular_damp": 0.05, "friction": 0.6,
                    "collision_mask": 4294967295, "axis_lock_angular_x": true,
                    "axis_lock_angular_z": true})"));
    CHECK(*crate.find("CollisionShape3D") == parse("{}")); // a box a meter across

    // Unity's cameras, lights and quads face along +z, Godot's along -z: they turn half a turn
    // about y, and what is under a turned object turns back.
    const json::Value camera = entity(files, hall, "main_camera");
    CHECK(*camera.find("Transform3D") == parse(R"({"position": [-1.0, 2.0, -10.0],
        "rotation": [0.0, 3.141593, 0.0], "children": ["gun"]})"));
    CHECK(*camera.find("Camera3D") ==
          parse(R"({"fov": 50.0, "near": 0.1, "far": 500.0, "current": true})"));
    CHECK(*camera.find("AudioListener3D") == parse(R"({"current": true})"));
    const json::Value gun = entity(files, hall, "gun");
    CHECK(*gun.find("Transform3D") ==
          parse(R"({"position": [0.5, -0.5, -1.0], "rotation": [0.0, 3.141593, 0.0]})"));
    CHECK(*gun.find("StandardMaterial3D") == parse(R"({"roughness": 0.5})")); // Default-Material
    const json::Value label = entity(files, hall, "label");
    CHECK(*label.find("Transform3D") ==
          parse(R"({"position": [0.0, 0.0, -0.51], "rotation": [0.0, 3.141593, 0.0]})"));
    CHECK(label.find("QuadMesh") != nullptr);
    // The sun's Euler angles (50, -30, 0) shine the same way here.
    const json::Value sun = entity(files, hall, "sun");
    CHECK(*sun.find("Transform3D") ==
          parse(R"({"position": [0.0, 3.0, 0.0], "rotation": [-0.872665, -2.617994, 0.0]})"));
    CHECK(*sun.find("DirectionalLight3D") ==
          parse(R"({"light_color": [1.0, 0.95, 0.85, 1.0], "light_energy": 1.2,
                    "shadow_enabled": true, "shadow_opacity": 0.8})"));
    // A point light shines all round, so it doesn't turn. A switched-off light waits hidden on
    // an entity of its own; a spot light's angle is half Unity's.
    CHECK(
        *entity(files, hall, "lamp").find("OmniLight3D") ==
        parse(R"({"omni_range": 8.0, "light_energy": 2.0, "light_color": [1.0, 0.8, 0.6, 1.0]})"));
    const json::Value spot = entity(files, hall, "spot_light");
    CHECK(*spot.find("Transform3D") ==
          parse(R"({"rotation": [0.0, 3.141593, 0.0], "visible": false})"));
    CHECK(*spot.find("SpotLight3D") == parse(R"({"spot_range": 12.0, "spot_angle": 20.0})"));

    // A plane's mesh collider is its two triangles, solid from both sides; a collider with no
    // Rigidbody is static. URP's Lit shader converts as the Standard one does.
    const json::Value floor = entity(files, hall, "floor");
    CHECK(*floor.find("PlaneMesh") == parse(R"({"size": [10.0, 10.0]})"));
    CHECK(*floor.find("Transform3D") == parse(R"({"scale": [2.0, 1.0, 2.0]})"));
    CHECK(*floor.find("StandardMaterial3D") ==
          parse(R"({"albedo_color": [0.5, 0.5, 0.5, 1.0], "roughness": 0.9,
                    "cull_mode": "disabled"})"));
    CHECK(*floor.find("StaticBody3D") ==
          parse(R"({"friction": 0.6, "collision_mask": 4294967295})"));
    const json::Value& faces = *floor.find("CollisionShape3D");
    CHECK(faces.find("shape")->as_string() == "concave_polygon");
    CHECK(faces.find("faces")->as_array().size() == 6);
    CHECK(faces.find("backface_collision")->as_bool());

    // A CharacterController is a CharacterBody3D with its capsule; a sound all in 3D an
    // AudioStreamPlayer3D, as loud as Unity's up to its MinDistance.
    const json::Value player = entity(files, hall, "player");
    CHECK(*player.find("CharacterBody3D") ==
          parse(R"({"collision_mask": 4294967295, "floor_max_angle": 0.872665,
                    "safe_margin": 0.05})"));
    CHECK(*player.find("CollisionShape3D") ==
          parse(R"({"shape": "capsule", "radius": 0.4, "height": 1.8,
                    "position": [0.0, 0.9, 0.0]})"));
    CHECK(*player.find("AudioStreamPlayer3D") ==
          parse(R"({"stream": "unity/Assets/Audio/steps.ogg", "autoplay": true,
                    "unit_size": 2.0, "max_db": 0.0, "max_distance": 30.0})"));

    // A model instance draws its file whole, with the material the instance puts in place of its
    // own; what is under its top object stays under it. A trigger with no body is an Area3D.
    const json::Value oak = entity(files, hall, "oak");
    CHECK(*oak.find("Transform3D") ==
          parse(R"({"position": [4.0, 0.0, 6.0], "children": ["bird"]})"));
    CHECK(*oak.find("MeshInstance3D") == parse(R"({"mesh": "unity/Assets/Models/Tree.glb"})"));
    CHECK(*oak.find("StandardMaterial3D") == *floor.find("StandardMaterial3D"));
    const json::Value bird = entity(files, hall, "bird");
    CHECK(*bird.find("Transform3D") == parse(R"({"position": [-1.0, 3.0, 0.0]})"));
    CHECK(*bird.find("Area3D") == parse(R"({"collision_mask": 4294967295})"));
    CHECK(*bird.find("CollisionShape3D") == parse(R"({"shape": "sphere", "radius": 0.25})"));

    // Only a LOD group's first level draws.
    CHECK(entity(files, hall, "rock_lod0").find("SphereMesh") != nullptr);
    CHECK(entity(files, hall, "rock_lod1").find("MeshInstance3D") == nullptr);

    // The scene's sky, ambient light and fog: Unity's procedural Default-Skybox becomes a
    // procedural sky in its colors; Unity's fog leaves the sky alone.
    const json::Value environment = entity(files, hall, "environment");
    CHECK(*environment.find("WorldEnvironment") ==
          parse(R"({"background_mode": "sky", "ambient_light_source": "sky",
                    "reflected_light_source": "sky", "fog_enabled": true,
                    "fog_light_color": [0.6, 0.7, 0.8, 1.0], "fog_density": 0.02,
                    "fog_sky_affect": 0.0})"));
    REQUIRE(environment.find("ProceduralSkyMaterial") != nullptr);
    CHECK(*environment.find("ProceduralSkyMaterial") ==
          parse(R"({"sky_top_color": [0.29, 0.45, 0.75, 1.0],
                    "sky_horizon_color": [0.72, 0.77, 0.84, 1.0],
                    "ground_horizon_color": [0.72, 0.77, 0.84, 1.0],
                    "ground_bottom_color": [0.369, 0.349, 0.341, 1.0]})"));

    // The project's 3D gravity, mirrored as the scenes are. A prefab with neither 2D nor 3D
    // components converts as most of the project does.
    const json::Value settings = parse(files.at("project.json"));
    CHECK(*settings.find("physics_3d") ==
          parse(R"({"gravity": 9.81, "gravity_vector": [0.0, -1.0, 0.0],
        "linear_damp": 0.0, "angular_damp": 0.0})"));
    CHECK(*entity(files, "unity/Assets/Prefabs/Spawn.scene.json", "spawn").find("Transform3D") ==
          parse(R"({"position": [-1.0, 0.0, 0.0]})"));

    const json::Value report = parse(files.at("import/unity.json"));
    CHECK(unsupported(report, "component SpriteRenderer").find("why")->as_string() ==
          "a 2D component in a 3D scene; a scene with more 2D components than 3D ones converts in "
          "2D");
    CHECK(unsupported(report, "LOD level past the first").find("where")->as_string() ==
          "Assets/Scenes/Hall.unity: Rock/Rock_LOD1");
    CHECK(!unsupported(report, "CharacterController step offset").is_null());
    CHECK(!unsupported(report, "procedural skybox").is_null());
    CHECK(!unsupported(report, "audio rolloff").is_null());
}

TEST_CASE("Unity meshes picked out of a model become glTF files of their own") {
    const fs::path project = "import_unity_picked";
    fs::remove_all(project);
    write_yard(project / "unity");
    const auto files = files_of(plan_inside(project));
    const std::string yard = "unity/Assets/Scenes/Yard.scene.json";
    REQUIRE(files.contains(yard));
    const auto read = [&files](std::string_view path) -> slash::Result<std::string> {
        const auto it = files.find(std::string(path));
        if (it == files.end()) {
            return slash::make_error(slash::ErrorCode::NotFound, "no such file");
        }
        return it->second;
    };
    const auto model = [&](const std::string& path) {
        REQUIRE_MESSAGE(files.contains(path), path);
        auto loaded = slash::scene3d::load_model(files.at(path), path, read);
        REQUIRE_MESSAGE(loaded.ok(), (loaded.ok() ? std::string() : loaded.error().message));
        return std::move(loaded).value();
    };

    // A MeshFilter's mesh of a model is written alone, at the entity's origin and sized by the
    // model's import scale; one material for it goes on the entity, so both barrels share it.
    const std::string barrel_file = "unity/Assets/Models/Props_Barrel_Oak_Banded_LowPoly.gltf";
    const json::Value barrel = entity(files, yard, "barrel");
    CHECK(*barrel.find("MeshInstance3D") == json::Object{{"mesh", barrel_file}});
    CHECK(*barrel.find("StandardMaterial3D") ==
          parse(R"({"albedo_color": [0.8, 0.6, 0.4, 1.0], "roughness": 0.8})"));
    const json::Value barrel_2 = entity(files, yard, "barrel_2");
    CHECK(*barrel_2.find("MeshInstance3D") == json::Object{{"mesh", barrel_file}});
    CHECK(barrel_2.find("StandardMaterial3D")->find("albedo_color") != nullptr);
    const slash::scene3d::Model side = model(barrel_file);
    REQUIRE(side.meshes.size() == 1);
    CHECK(side.meshes[0].name == "Barrel_Oak_Banded_LowPoly");
    REQUIRE(side.meshes[0].surfaces.size() == 1);
    CHECK(side.parts.size() == 1);
    const slash::scene3d::Bounds box = slash::scene3d::bounds(side);
    CHECK(box.min.x == doctest::Approx(-0.5));
    CHECK(box.max.y == doctest::Approx(1.0));
    CHECK(box.max.z == doctest::Approx(0.5));
    // Its convex mesh collider is the hull of its corners, sized as it is.
    const json::Value hull = *barrel.find("CollisionShape3D");
    CHECK(hull.find("shape")->as_string() == "convex_polygon");
    CHECK(hull.find("points")->size() == 4);
    CHECK(hull.find("points")->as_array().front() == parse("[-0.5, 0.0, 0.5]"));
    CHECK(barrel.find("StaticBody3D") != nullptr);

    // Two materials for the lid's two parts are written into its file, in place of the model's.
    const json::Value lid = entity(files, yard, "lid");
    CHECK(*lid.find("MeshInstance3D") ==
          json::Object{{"mesh", "unity/Assets/Models/Props_Lid.gltf"}});
    CHECK(lid.find("StandardMaterial3D") == nullptr);
    const slash::scene3d::Model banded = model("unity/Assets/Models/Props_Lid.gltf");
    REQUIRE(banded.meshes.at(0).surfaces.size() == 2);
    REQUIRE(banded.materials.size() == 2);
    CHECK(banded.materials[0].name == "Wood");
    CHECK(banded.materials[0].standard.albedo_color.x == doctest::Approx(0.8));
    CHECK(banded.materials[0].standard.roughness == doctest::Approx(0.8));
    CHECK(banded.materials[1].name == "Stone");
    CHECK(banded.materials[1].standard.albedo_color.x == doctest::Approx(0.5));
    // Its mesh collider is its triangles, both parts' of them.
    const json::Value faces = *lid.find("CollisionShape3D");
    CHECK(faces.find("shape")->as_string() == "concave_polygon");
    CHECK(faces.find("faces")->size() == 12);

    // With one material, Unity draws the lid's first part alone, with the model's own material.
    const json::Value half = entity(files, yard, "half_lid");
    CHECK(*half.find("MeshInstance3D") ==
          json::Object{{"mesh", "unity/Assets/Models/Props_Lid_2.gltf"}});
    CHECK(half.find("StandardMaterial3D")->find("albedo_color") != nullptr);
    const slash::scene3d::Model top = model("unity/Assets/Models/Props_Lid_2.gltf");
    REQUIRE(top.meshes.at(0).surfaces.size() == 1);
    REQUIRE(top.materials.size() == 1);
    CHECK(top.materials[0].name == "Oak");

    // A mesh the model doesn't have is reported, and the object kept without one.
    const json::Value report = parse(files.at("import/unity.json"));
    const json::Value lost = unsupported(report, "mesh from a model");
    REQUIRE(!lost.is_null());
    CHECK(lost.find("why")->as_string() == "Assets/Models/Props.obj has no mesh with the ID 12345");
    CHECK(entity(files, yard, "lost").find("MeshInstance3D") == nullptr);
}

namespace {

// Bytes as Unity writes arrays of numbers in text: two hex digits each.
std::string hex_of(const std::vector<std::uint8_t>& bytes) {
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const std::uint8_t byte : bytes) {
        out.push_back("0123456789abcdef"[byte >> 4U]);
        out.push_back("0123456789abcdef"[byte & 0xFU]);
    }
    return out;
}

// A Unity height at a column and row of the hills: rising 100 a column along +x, 10 a row.
std::int16_t hill_height(int x, int z) {
    return static_cast<std::int16_t>((x * 100) + (z * 10));
}

std::string terrain_layer_file(std::string_view name, std::string_view diffuse,
                               std::string_view normal, std::string_view tile,
                               std::string_view smoothness) {
    return std::string(kHeader) + "--- !u!1953259897 &8574412962073106934\nTerrainLayer:\n" +
           std::string(kSource) + "  m_Name: " + std::string(name) +
           "\n  m_DiffuseTexture: " + std::string(diffuse) +
           "\n  m_NormalMapTexture: " + std::string(normal) +
           "\n  m_MaskMapTexture: {fileID: 0}\n  m_TileSize: " + std::string(tile) +
           "\n  m_TileOffset: {x: 0, y: 0}\n  m_Specular: {r: 0, g: 0, b: 0, a: 0}\n"
           "  m_Metallic: 0\n  m_Smoothness: " +
           std::string(smoothness) + "\n  m_NormalScale: 1\n";
}

// Hills: a terrain of 65 by 65 heights, 2 m apart along x and 3 along z, 100 m high, with grass,
// rock and sand painted by a 4 by 4 alpha map, a hole three cells across and a tree.
void write_hills(const fs::path& folder) {
    write(folder / "ProjectSettings/ProjectVersion.txt", "m_EditorVersion: 2022.3.20f1\n");
    std::string matrix;
    for (int layer = 0; layer < 32; ++layer) {
        matrix += "ffffffff";
    }
    write(folder / "ProjectSettings/DynamicsManager.asset",
          std::string(kHeader) + "--- !u!55 &1\nPhysicsManager:\n  m_ObjectHideFlags: 0\n" +
              "  serializedVersion: 13\n  m_Gravity: {x: 0, y: -9.81, z: 0}\n" +
              "  m_DefaultMaterial: {fileID: 0}\n  m_LayerCollisionMatrix: " + matrix + "\n");
    const std::array<std::pair<std::string_view, std::string_view>, 4> textures{
        {{"grass", "a6000000000000000000000000000001"},
         {"rock", "a6000000000000000000000000000002"},
         {"rock_normal", "a6000000000000000000000000000003"},
         {"sand", "a6000000000000000000000000000004"}}};
    for (const auto& [name, guid] : textures) {
        write_png(folder / ("Assets/Terrain/" + std::string(name) + ".png"), 4, 4);
        write(folder / ("Assets/Terrain/" + std::string(name) + ".png.meta"),
              meta(guid, "TextureImporter", "  textureType: 0\n"));
    }
    const auto texture = [](std::string_view guid) { return ref(2800000, guid, 3); };
    write(folder / "Assets/Terrain/Grass.terrainlayer",
          terrain_layer_file("Grass", texture("a6000000000000000000000000000001"), "{fileID: 0}",
                             "{x: 10, y: 10}", "0"));
    write(folder / "Assets/Terrain/Rock.terrainlayer",
          terrain_layer_file("Rock", texture("a6000000000000000000000000000002"),
                             texture("a6000000000000000000000000000003"), "{x: 8, y: 8}", "0.5"));
    write(folder / "Assets/Terrain/Sand.terrainlayer",
          terrain_layer_file("Sand", texture("a6000000000000000000000000000004"), "{fileID: 0}",
                             "{x: 6, y: 4}", "0"));
    const std::array<std::string_view, 3> layers{"a6000000000000000000000000000011",
                                                 "a6000000000000000000000000000012",
                                                 "a6000000000000000000000000000013"};
    const std::array<std::string_view, 3> layer_files{"Grass", "Rock", "Sand"};
    for (std::size_t i = 0; i < layers.size(); ++i) {
        write(folder / ("Assets/Terrain/" + std::string(layer_files.at(i)) + ".terrainlayer.meta"),
              meta(layers.at(i), "NativeFormatImporter",
                   "  mainObjectFileID: 8574412962073106934\n"));
    }

    constexpr int kSize = 65;
    std::vector<std::uint8_t> heights;
    for (int z = 0; z < kSize; ++z) {
        for (int x = 0; x < kSize; ++x) {
            const auto value = static_cast<std::uint16_t>(hill_height(x, z));
            heights.push_back(static_cast<std::uint8_t>(value & 0xFFU));
            heights.push_back(static_cast<std::uint8_t>(value >> 8U));
        }
    }
    // Cells 10 to 12 along x and 20 to 22 along z are cut.
    std::vector<std::uint8_t> holes;
    for (int z = 0; z < kSize - 1; ++z) {
        for (int x = 0; x < kSize - 1; ++x) {
            holes.push_back(x >= 10 && x <= 12 && z >= 20 && z <= 22 ? 0 : 1);
        }
    }
    // The alpha map's texel at column x and row z (from the bottom, along +z): grass along the
    // first column, rock along the last, sand in between rows.
    std::vector<std::uint8_t> alpha;
    for (int z = 0; z < 4; ++z) {
        for (int x = 0; x < 4; ++x) {
            const std::array<std::uint8_t, 4> texel =
                x == 0 ? std::array<std::uint8_t, 4>{255, 0, 0, 0}
                       : (x == 3 ? std::array<std::uint8_t, 4>{0, 255, 0, 0}
                                 : std::array<std::uint8_t, 4>{
                                       0, static_cast<std::uint8_t>(z * 80),
                                       static_cast<std::uint8_t>(255 - (z * 80)), 0});
            alpha.insert(alpha.end(), texel.begin(), texel.end());
        }
    }
    std::string data(kHeader);
    data += "--- !u!156 &15600000\nTerrainData:\n" + std::string(kSource) +
            "  m_Name: Hills\n  serializedVersion: 6\n  m_SplatDatabase:\n"
            "    serializedVersion: 2\n    m_TerrainLayers:\n";
    for (const std::string_view guid : layers) {
        data += "    - " + ref(8574412962073106934, guid, 2) + "\n";
    }
    data += "    m_AlphaTextures:\n    - {fileID: 2800001}\n    m_AlphamapResolution: 4\n"
            "    m_BaseMapResolution: 16\n  m_DetailDatabase:\n    serializedVersion: 3\n"
            "    m_Patches: []\n    m_DetailPrototypes: []\n    m_TreeInstances:\n"
            "    - position: {x: 0.5, y: 0, z: 0.5}\n      widthScale: 1\n      heightScale: 1\n"
            "      prototypeIndex: 0\n    m_TreePrototypes: []\n"
            "  m_Heightmap:\n    serializedVersion: 6\n    m_Holes: " +
            hex_of(holes) + "\n    m_Heights: " + hex_of(heights) +
            "\n    m_Resolution: 65\n    m_Levels: 2\n    m_Scale: {x: 2, y: 100, z: 3}\n";
    data += "--- !u!28 &2800001\nTexture2D:\n" + std::string(kSource) +
            "  m_Name: SplatAlpha 0\n  serializedVersion: 13\n  m_Width: 4\n  m_Height: 4\n"
            "  m_CompleteImageSize: 64\n  m_TextureFormat: 4\n  m_MipCount: 1\n"
            "  image data: 64\n  _typelessdata: " +
            hex_of(alpha) + "\n";
    write(folder / "Assets/Terrain/Hills.asset", data);
    write(folder / "Assets/Terrain/Hills.asset.meta",
          meta("a6000000000000000000000000000020", "NativeFormatImporter",
               "  mainObjectFileID: 15600000\n"));
    write(folder / "Assets/Terrain/Mud.physicMaterial",
          std::string(kHeader) + "--- !u!134 &13400000\nPhysicMaterial:\n" + std::string(kSource) +
              "  m_Name: Mud\n  dynamicFriction: 0.4\n  staticFriction: 0.4\n"
              "  bounciness: 0.25\n");
    write(folder / "Assets/Terrain/Mud.physicMaterial.meta",
          meta("a6000000000000000000000000000021", "NativeFormatImporter",
               "  mainObjectFileID: 13400000\n"));

    // The terrain object 10 m along +x and 20 m along +z, with its collider.
    const std::string terrain_data = ref(15600000, "a6000000000000000000000000000020", 2);
    std::string scene(kHeader);
    scene += game_object(10, "Hills", {11, 12, 13});
    scene += transform_3d(11, 10, "{x: 10, y: 0, z: 20}", kNoTurn, kOne);
    scene += component(218, "Terrain", 12, 10,
                       "  serializedVersion: 6\n  m_TerrainData: " + terrain_data +
                           "\n  m_TreeDistance: 5000\n  m_HeightmapPixelError: 5\n"
                           "  m_ShadowCastingMode: 1\n  m_DrawHeightmap: 1\n"
                           "  m_MaterialTemplate: {fileID: 10652, guid: "
                           "0000000000000000f000000000000000, type: 0}\n");
    scene += component(154, "TerrainCollider", 13, 10,
                       "  m_Material: " + ref(13400000, "a6000000000000000000000000000021", 2) +
                           "\n  serializedVersion: 2\n  m_TerrainData: " + terrain_data +
                           "\n  m_EnableTreeColliders: 1\n");
    write(folder / "Assets/Scenes/Hills.unity", scene);
    write(folder / "Assets/Scenes/Hills.unity.meta", meta("a6000000000000000000000000000030"));
}

} // namespace

TEST_CASE("Unity terrains become HTerrains: heights, layers, alpha maps and holes, mirrored in x") {
    const fs::path project = "import_unity_terrain";
    fs::remove_all(project);
    write_hills(project / "unity");
    auto imported = importer::import_project(
        {.source = (project / "unity").string(), .destination = project.string()});
    REQUIRE_MESSAGE(imported.ok(), (imported.ok() ? std::string() : imported.error().message));
    const auto files = files_of(imported.value());
    const std::string scene = "unity/Assets/Scenes/Hills.scene.json";
    REQUIRE(files.contains(scene));
    slash::world::ComponentRegistry components;
    slash::world::register_builtin_components(components);
    CHECK(slash::world::validate_scene(components, parse(files.at(scene))).empty());

    // The terrain reaches toward +x from its object in Unity, so toward -x mirrored: its entity
    // moves back its width, 128 m. Its collider's material is the HTerrain's; its layers' tiles
    // are cells, 2 m each.
    CHECK(*entity(files, scene, "hills").find("Transform3D") ==
          parse(R"({"position": [-10.0, 0.0, 20.0], "children": ["hills_terrain"]})"));
    const json::Value terrain = entity(files, scene, "hills_terrain");
    CHECK(*terrain.find("Transform3D") == parse(R"({"position": [-128.0, 0.0, 0.0]})"));
    CHECK(*terrain.find("HTerrain") == parse(R"({
        "heightmap": "unity/Assets/Terrain/Hills.height.png",
        "colormap": "unity/Assets/Terrain/Hills.color.png",
        "splatmap": "unity/Assets/Terrain/Hills.splat.png",
        "resolution": 65, "height_range": [0.0, 22.0], "map_scale": [2.0, 1.0, 3.0],
        "textures": [{"albedo_bump": "unity/Assets/Terrain/grass.png", "uv_scale": 5.0},
                     {"albedo_bump": "unity/Assets/Terrain/rock.png",
                      "normal_roughness": "unity/Assets/Terrain/rock_normal.png", "uv_scale": 4.0},
                     {"albedo_bump": "unity/Assets/Terrain/sand.png", "uv_scale": 3.0}],
        "depth_blending": false, "collision_mask": 4294967295, "friction": 0.4,
        "bounce": 0.25})"));

    // Heights: Unity's 0 to 32766 are 0 to 100 m; columns are flipped, as x mirrors.
    const auto heights =
        slash::render::decode_gray16(files.at("unity/Assets/Terrain/Hills.height.png"));
    REQUIRE(heights.ok());
    const std::vector<float> meters =
        slash::scene3d::decode_heights(heights.value(), slash::Vec2{0.0F, 22.0F});
    for (const auto& [x, z] :
         std::initializer_list<std::pair<int, int>>{{0, 0}, {64, 64}, {5, 40}}) {
        const double expected = hill_height(64 - x, z) / 32766.0 * 100.0;
        CHECK(std::abs(meters[static_cast<std::size_t>((z * 65) + x)] - expected) <=
              22.0 / 65535.0);
    }
    // The alpha map's weights at each height, its columns flipped too.
    const auto splat =
        slash::render::decode_image(files.at("unity/Assets/Terrain/Hills.splat.png"));
    REQUIRE(splat.ok());
    const auto weights = [&](int x, int z) {
        const std::size_t at = static_cast<std::size_t>((z * 65) + x) * 4;
        return std::array<int, 4>{splat.value().pixels[at], splat.value().pixels[at + 1],
                                  splat.value().pixels[at + 2], splat.value().pixels[at + 3]};
    };
    CHECK(weights(64, 0) == std::array<int, 4>{255, 0, 0, 0}); // Unity's corner at x 0, z 0
    CHECK(weights(0, 64) == std::array<int, 4>{0, 255, 0, 0});
    CHECK(weights(32, 32) == std::array<int, 4>{0, 120, 135, 0}); // between four sand texels
    // Holes: the heights inside the cut cells, at Unity's columns 11 and 12, rows 21 and 22.
    const auto color =
        slash::render::decode_image(files.at("unity/Assets/Terrain/Hills.color.png"));
    REQUIRE(color.ok());
    int cut = 0;
    for (int z = 0; z < 65; ++z) {
        for (int x = 0; x < 65; ++x) {
            const bool hole =
                color.value().pixels[(static_cast<std::size_t>((z * 65) + x) * 4) + 3] < 128;
            cut += hole ? 1 : 0;
            if (hole) {
                CHECK((x == 52 || x == 53));
                CHECK((z == 21 || z == 22));
            }
        }
    }
    CHECK(cut == 4);

    const json::Value report = parse(files.at("import/unity.json"));
    for (const auto& [what, where] : std::initializer_list<std::pair<std::string, std::string>>{
             {"Terrain holes", "Assets/Terrain/Hills.asset"},
             {"Terrain trees", "Assets/Terrain/Hills.asset"},
             {"Terrain layer surface", "Assets/Terrain/Rock.terrainlayer"},
             {"Terrain layer tile size", "Assets/Terrain/Sand.terrainlayer"}}) {
        const json::Value item = unsupported(report, what);
        REQUIRE_MESSAGE(!item.is_null(), what);
        CHECK(item.find("where")->as_string() == where);
    }
    CHECK(unsupported(report, "component Terrain").is_null());
    CHECK(unsupported(report, "component TerrainCollider").is_null());

    // The ground is where Unity has it, mirrored: Unity's height at column 5 and row 40, at x
    // 10 + 2 * 5 and z 20 + 3 * 40, is at x -20 here.
    command::Registry commands;
    command::register_builtin_commands(commands);
    command::SessionOptions options;
    options.project_root = project.string();
    options.capture_logs = false;
    command::Session session(components, commands, options);
    const command::Reply opened =
        session.call("scene.open", parse(R"({"path": ")" + scene + "\"}"));
    REQUIRE_MESSAGE(opened.ok, opened.message);
    const command::Reply ground = session.call(
        "terrain.get", parse(R"({"id": "hills_terrain", "points": [[-20, 140]], "global": true})"));
    REQUIRE_MESSAGE(ground.ok, ground.message);
    const json::Value& position = *ground.value.find("points")->as_array()[0].find("position");
    CHECK(position.as_array()[1].as_double() ==
          doctest::Approx(hill_height(5, 40) / 32766.0 * 100.0).epsilon(0.001));
}
