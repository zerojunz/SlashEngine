#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/importer/commands.hpp"
#include "slash/importer/import.hpp"
#include "slash/importer/unity_yaml.hpp"
#include "slash/platform/file.hpp"
#include "slash/render/image.hpp"
#include "slash/runtime/project.hpp"
#include "slash/storyboard/storyboard.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/scene.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace json = slash::json;
namespace command = slash::command;
namespace unity = slash::importer::unity;
namespace importer = slash::importer;
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

// The components one entity of a planned scene ends up with, with what it extends and its
// overrides applied; null if the scene has no such entity.
json::Value entity(const std::map<std::string, std::string>& files, const std::string& scene,
                   std::string_view id) {
    REQUIRE_MESSAGE(files.contains(scene), scene);
    slash::world::ComponentRegistry components;
    slash::world::register_builtin_components(components);
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
                       "  m_Script: {fileID: 11500000, guid: 073797afb82c5a1438f328866b10b3f0, "
                       "type: 3}\n  m_Name: \n  m_EditorClassIdentifier: "
                       "Unity.RenderPipelines.Universal.Runtime::UnityEngine.Rendering."
                       "Universal.Light2D\n");
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

fs::path make_project(const fs::path& folder) {
    fs::remove_all(folder);
    write_settings(folder);
    write_assets(folder);
    write_cave(folder);
    write_prefabs(folder);
    return folder;
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
          "textures are PNG or BMP files; save it as PNG");
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
          "3D comes with milestone 7");
    CHECK(unsupported(report, "component Image").find("why")->as_string() ==
          "Unity UI converts in a later import (on the roadmap)");
    CHECK(unsupported(report, "component Light2D").find("why")->as_string() ==
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

    // The stack holds an instance of the crate, moved up: only the override is written.
    const json::Value stack_file = parse(files.at(stack));
    CHECK(*stack_file.find("entities")->find("crate") ==
          parse(R"({"extends": "unity/Assets/Prefabs/Crate.scene.json",
                    "components": {"Transform2D": {"position": [0.0, -24.0]}}})"));
    CHECK(entity(files, stack, "crate/label") != nullptr);

    // A variant extends the prefab it varies.
    const json::Value big_file = parse(files.at(big));
    CHECK(big_file.find("root")->as_string() == "big_crate");
    CHECK(*big_file.find("entities")->find("big_crate") ==
          parse(R"({"extends": "unity/Assets/Prefabs/Crate.scene.json",
                    "components": {"Transform2D": {"scale": [2.0, 2.0]}}})"));

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
    CHECK(*plan.report.find("scripts") ==
          parse(R"([{"script": "unity/Assets/Scripts/Crate.cs", "task": "task/port-crate",
                     "entities": 6}])"));
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
    command::SessionOptions options;
    options.project_root = destination.string();
    options.capture_logs = false;
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
    command::SessionOptions options;
    options.project_root = project.string();
    options.capture_logs = false;
    command::Session session(components, commands, options);

    const command::Reply done = session.call("project.import", parse(R"({"source": "unity"})"));
    REQUIRE_MESSAGE(done.ok, done.message);
    CHECK(done.value.find("engine")->as_string() == "unity");
    CHECK(fs::exists(project / "project.json"));
    CHECK(
        session.call("scene.open", parse(R"({"path": "unity/Assets/Scenes/Yard.scene.json"})")).ok);
    CHECK(session.call("storyboard.check", parse("{}")).value.find("valid")->as_bool());
}
