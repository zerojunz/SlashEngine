#include "unity_import.hpp"

#include "common.hpp"
#include "skeletons.hpp"

#include "slash/animation/animation.hpp"
#include "slash/animation/tree.hpp"
#include "slash/audio/audio.hpp"
#include "slash/canvas/debug_draw.hpp"
#include "slash/canvas/tilemap.hpp"
#include "slash/core/assert.hpp"
#include "slash/core/json.hpp"
#include "slash/core/math.hpp"
#include "slash/importer/unity_build.hpp"
#include "slash/importer/unity_yaml.hpp"
#include "slash/platform/file.hpp"
#include "slash/render/image.hpp"
#include "slash/runtime/project.hpp"
#include "slash/scene3d/model.hpp"
#include "slash/scene3d/skeleton.hpp"
#include "slash/scene3d/terrain.hpp"
#include "slash/storyboard/storyboard.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/scene.hpp"
#include "slash/world/tree2d.hpp"
#include "slash/world/world.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <numbers>
#include <numeric>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace slash::importer {
namespace {

using unity::Node;
using unity::Object;
using unity::ObjectRef;

constexpr std::string_view kReportFile = "import/unity.json";
constexpr int kMaxInstanceDepth = 16;
// An object an instance brings has the ID (instance ID ^ its ID in the prefab) & kIdMask.
constexpr std::int64_t kIdMask = 0x7FFFFFFFFFFFFFFF;
// The file ID of a texture's sprite when it holds one sprite.
constexpr std::int64_t kSingleSprite = 21300000;
// Unity's friction and bounciness when no physics material sets them.
constexpr double kDefaultFriction = 0.4;
constexpr int kMaxZIndex = 4096;
// The weight of an unweighted curve tangent, and how often a curved stretch of a clip is keyed.
constexpr double kUnweighted = 1.0 / 3.0;
constexpr double kSampleStep = 1.0 / 60.0;

// Unity's class IDs for the objects this reads.
constexpr int kGameObject = 1;
constexpr int kTransform = 4;
constexpr int kCamera = 20;
constexpr int kRigidbody2D = 50;
constexpr int kCircleCollider2D = 58;
constexpr int kPolygonCollider2D = 60;
constexpr int kBoxCollider2D = 61;
constexpr int kCompositeCollider2D = 66;
constexpr int kEdgeCollider2D = 68;
constexpr int kCapsuleCollider2D = 70;
constexpr int kAnimationClip = 74;
constexpr int kAudioListener = 81;
constexpr int kAudioSource = 82;
constexpr int kAnimatorController = 91;
constexpr int kGuiLayer = 92;
constexpr int kAnimator = 95;
constexpr int kMonoBehaviour = 114;
constexpr int kFlareLayer = 124;
constexpr int kBlendTree = 206;
constexpr int kSpriteRenderer = 212;
constexpr int kAnimatorOverrideController = 221;
constexpr int kCanvasRenderer = 222;
constexpr int kCanvas = 223;
constexpr int kRectTransform = 224;
constexpr int kCanvasGroup = 225;
constexpr int kSpringJoint2D = 231;
constexpr int kHingeJoint2D = 233;
constexpr int kSliderJoint2D = 234;
constexpr int kAudioMixerController = 241;
constexpr int kAudioMixerGroup = 243;
constexpr int kAudioMixerEffect = 244;
constexpr int kAudioMixerSnapshot = 245;
constexpr int kPlatformEffector2D = 251;
constexpr int kPrefabInstance = 1001;
constexpr int kTilemapCollider2D = 19719996;
constexpr int kGrid = 156049354;
constexpr int kTilemapRenderer = 483693784;
constexpr int kTilemap = 1839735485;
// 3D, which converts in scenes with more 3D components than 2D ones.
constexpr int kMeshRenderer = 23;
constexpr int kMeshFilter = 33;
constexpr int kRigidbody = 54;
constexpr int kMeshCollider = 64;
constexpr int kBoxCollider = 65;
constexpr int kLight = 108;
constexpr int kSphereCollider = 135;
constexpr int kCapsuleCollider = 136;
constexpr int kSkinnedMeshRenderer = 137;
constexpr int kCharacterController = 143;
constexpr int kTerrainCollider = 154;
constexpr int kLodGroup = 205;
constexpr int kTerrain = 218;
// Not one of Unity's: an instance of a model file, which the import makes for a PrefabInstance of
// one.
constexpr int kModelInstance = -1;
constexpr std::int64_t kModelComponent = 0x4D4F44454C; // its ID in the instance: "MODEL"
// The script of Unity's own Tile class, which tile assets name; other tiles are scripted.
constexpr std::string_view kTileScriptGuid = "0000000000000000e000000000000000";
constexpr std::int64_t kTileScriptId = 13312;
constexpr std::int64_t kMaxTileFrames = 256; // a tile set's limit
// Unity's built-in resources: its default font (Arial) and the sprites its default UI controls
// use, by file ID.
constexpr std::string_view kBuiltinExtraGuid = "0000000000000000e000000000000000";
constexpr std::string_view kBuiltinSprites = "0000000000000000f000000000000000";
constexpr std::int64_t kCheckmarkSprite = 10901;
constexpr std::int64_t kUiSprite = 10905;
constexpr std::int64_t kBackgroundSprite = 10907;
constexpr std::int64_t kInputFieldSprite = 10911;
constexpr std::int64_t kKnobSprite = 10913;
constexpr std::int64_t kMaskSprite = 10917;
// Unity's built-in meshes, in its default resources (kBuiltinExtraGuid), and its default material
// and skybox, in its extra resources (kBuiltinSprites).
constexpr std::int64_t kCubeMesh = 10202;
constexpr std::int64_t kCylinderMesh = 10206;
constexpr std::int64_t kSphereMesh = 10207;
constexpr std::int64_t kCapsuleMesh = 10208;
constexpr std::int64_t kPlaneMesh = 10209;
constexpr std::int64_t kQuadMesh = 10210;
constexpr std::int64_t kDefaultMaterial = 10303;
constexpr std::int64_t kDefaultSkybox = 10304;
constexpr std::int64_t kSpecularShader = 45; // Standard (Specular setup)
// URP's shaders, whose GUIDs are the same in every project.
constexpr std::string_view kUrpLit = "933532a4fcc9baf4fa0491de14d08ed7";
constexpr std::string_view kUrpSimpleLit = "8d2bb70cbf9db8d4da26e15b26e74248";
constexpr std::string_view kUrpUnlit = "650dd9526735d5b46b79224bc6e94025";
// The IDs a model's top object and its Transform have in the model: since Unity 2019.3, then
// before it.
constexpr std::array<std::int64_t, 2> kModelRootObjects{919132149155446097, 100000};
constexpr std::array<std::int64_t, 2> kModelRootTransforms{-8679921383154817045, 400000};
// The ID of the Animator Unity puts on the top object of a model it imports with a rig.
constexpr std::array<std::int64_t, 2> kModelRootAnimators{5866666021909216657, 9500000};
// Unity's 3D friction when no physics material sets it.
constexpr double kDefaultFriction3D = 0.6;

constexpr std::string_view kUiWhy = "a UI component outside a Canvas; put it under one";
constexpr std::string_view kLostWhy =
    "its values didn't survive the game's build: the build's scripts couldn't be read";
constexpr std::string_view kEventWhy =
    "not needed: SlashEngine's gui system sends input to controls itself";
constexpr std::string_view k3DWhy = "a 3D component in a 2D scene; a scene with more 3D "
                                    "components than 2D ones converts in 3D";
constexpr std::string_view k2DWhy = "a 2D component in a 3D scene; a scene with more 2D "
                                    "components than 3D ones converts in 2D";

// Components of Unity's own packages, by their scripts' GUIDs, which are the same in every
// project. UI ones convert on objects under a Canvas; why says why one doesn't.
struct KnownScript {
    std::string_view guid;
    std::string_view name;
    std::string_view why;
    bool ui = false;
    bool no_values = false; // it serializes nothing, or may not, in some Unity versions
};
constexpr std::array kKnownScripts{
    KnownScript{"fe87c0e1cc204ed48ad3b37840f39efc", "Image", "", true},
    KnownScript{"1344c3c82d62a2a41a3576d8abb8e3ea", "RawImage", "", true},
    KnownScript{"4e29b1a8efbd4b44bb3f3716e73f07ff", "Button", "", true},
    KnownScript{"5f7201a12d95ffc409449d95f23cf332", "Text", "", true},
    KnownScript{"9085046f02f69544eb97fd06b6048fe2", "Toggle", "", true},
    KnownScript{"2fafe2cfe61f6974895a912c3755e8f1", "ToggleGroup", "", true},
    KnownScript{"67db9e8f0e2ae9c40bc1e2b64352a6b4", "Slider", "", true},
    KnownScript{"2a4db7a114972834c8e4117be1d82ba3", "Scrollbar", "", true},
    KnownScript{"1aa08ab6e0800fa44ae55d278d1423e3", "ScrollRect", "", true},
    KnownScript{"0d0b652f32a2cc243917e4028fa0f046", "Dropdown", "", true},
    KnownScript{"d199490a83bb2b844b9695cbf13b01ef", "InputField", "", true},
    KnownScript{"31a19414c41e5ae4aae2af33fee712f6", "Mask", "", true},
    KnownScript{"3312d7739989d2b4e91e6319e9a96d76", "RectMask2D", "", true, true},
    KnownScript{"30649d3a9faa99c48a7b1166b86bf2a0", "HorizontalLayoutGroup", "", true},
    KnownScript{"59f8146938fff824cb5fd77236b75775", "VerticalLayoutGroup", "", true},
    KnownScript{"8a8695521f0d02e499659fee002a26c2", "GridLayoutGroup", "", true},
    KnownScript{"3245ec927659c4140ac4f8d17403cc18", "ContentSizeFitter", "", true},
    KnownScript{"86710e43de46f6f4bac7c8e50813a599", "AspectRatioFitter", "", true},
    KnownScript{"306cc8c2b49d7114eaa3623786fc2126", "LayoutElement", "", true},
    KnownScript{"0cd44c1031e13a943bb63640046fad76", "CanvasScaler", "", true},
    KnownScript{"e19747de3f5aca642ab2be37e372fb86", "Outline", "", true},
    KnownScript{"cfabb0440166ab443bba8876756fdfa9", "Shadow", "", true},
    KnownScript{"7a98125502f715b4b83cfb77b434e436", "Selectable",
                "a plain Selectable becomes a control that takes no input", true},
    KnownScript{"f4688fdb7df04437aeb418b961361dc5", "TextMeshProUGUI", "", true},
    KnownScript{"7b743370ac3e4ec2a1668f5455a8ef8a", "TMP_Dropdown", "", true},
    KnownScript{"2da0c512f12947e489f739169773d7ca", "TMP_InputField", "", true},
    KnownScript{"d0b148fe25e99eb48b9724523833bab1", "EventTrigger",
                "event triggers are gameplay code: it reads the control's events", true},
    KnownScript{"dc42784cf147c0c48a680349fa168899", "GraphicRaycaster", "", true},
    KnownScript{"9541d86e2fd84c1d9990edf0852d74ab", "TextMeshPro",
                "text in the world isn't converted yet; use a Label in a Control"},
    KnownScript{"76c392e42b5098c458856cdf6ecaaaa1", "EventSystem", kEventWhy},
    KnownScript{"4f231c4fb786f3946a6b90b886c48677", "StandaloneInputModule", kEventWhy},
    KnownScript{"1511ccae7919cfc46b603b9b337fdc94", "BaseInput", kEventWhy, false, true},
    KnownScript{"56666c5a40171f54783dd416a44f42bf", "Physics2DRaycaster", kEventWhy},
    KnownScript{"c49b4cc203aa6414fae5c798d1d0e7d6", "PhysicsRaycaster", kEventWhy},
    // URP's 2D lights and shadow casters convert on objects outside a Canvas.
    KnownScript{"073797afb82c5a1438f328866b10b3f0", "Light2D",
                "2D lights shade sprites and tile maps, not controls"},
    KnownScript{"7db70e0ea77f5ac47a8f4565a9406397", "ShadowCaster2D", "controls cast no shadows"},
    KnownScript{"38f48a8d66b2adc46887f23a3a88ac01", "CompositeShadowCaster2D",
                "controls cast no shadows", false, true},
};

// Keys every MonoBehaviour has, which aren't a script's own values.
bool standard_key(std::string_view key) {
    static constexpr std::array<std::string_view, 10> kKeys{"m_ObjectHideFlags",
                                                            "m_CorrespondingSourceObject",
                                                            "m_PrefabInstance",
                                                            "m_PrefabAsset",
                                                            "m_GameObject",
                                                            "m_Enabled",
                                                            "m_EditorHideFlags",
                                                            "m_Script",
                                                            "m_Name",
                                                            "m_EditorClassIdentifier"};
    return contains(kKeys, key);
}

// Rounds pixel values made from Unity's units, so 0.64 * 100 is 64 rather than 64.00000000000001.
double tidy(double value) {
    const double rounded = std::round(value * 10000.0) / 10000.0;
    return rounded == 0.0 ? 0.0 : rounded;
}

double tidy_angle(double radians) {
    const double rounded = std::round(radians * 1e6) / 1e6;
    return rounded == 0.0 ? 0.0 : rounded;
}

json::Value pair(double x, double y) {
    return json::Array{tidy(x), tidy(y)};
}

std::optional<std::pair<int, int>> bmp_size(std::string_view bytes) {
    constexpr std::size_t kHeader = 26;
    if (bytes.size() < kHeader || bytes.substr(0, 2) != "BM") {
        return std::nullopt;
    }
    const auto read = [&](std::size_t at) {
        std::uint32_t value = 0;
        for (std::size_t i = 4; i-- > 0;) {
            value = (value << 8U) | static_cast<unsigned char>(bytes[at + i]);
        }
        return static_cast<std::int32_t>(value);
    };
    return std::pair{read(18), std::abs(read(22))};
}

// Where a sprite's pivot is, from its bottom left corner as a fraction of its size, by Unity's
// SpriteAlignment.
std::pair<double, double> alignment_pivot(std::int64_t alignment, const Node* custom) {
    static constexpr std::array<std::pair<double, double>, 9> kPivots{{{0.5, 0.5},
                                                                       {0.0, 1.0},
                                                                       {0.5, 1.0},
                                                                       {1.0, 1.0},
                                                                       {0.0, 0.5},
                                                                       {1.0, 0.5},
                                                                       {0.0, 0.0},
                                                                       {0.5, 0.0},
                                                                       {1.0, 0.0}}};
    if (alignment >= 0 && alignment < 9) {
        return kPivots.at(static_cast<std::size_t>(alignment));
    }
    if (custom != nullptr) {
        return {custom->number_at("x", 0.5), custom->number_at("y", 0.5)};
    }
    return {0.5, 0.5};
}

Node ref_node(const ObjectRef& ref) {
    Node node;
    node.kind = Node::Kind::Map;
    Node id;
    id.text = std::to_string(ref.file_id);
    node.entries.push_back({"fileID", std::move(id)});
    if (!ref.guid.empty()) {
        Node guid;
        guid.text = ref.guid;
        node.entries.push_back({"guid", std::move(guid)});
        Node type;
        type.text = std::to_string(ref.type);
        node.entries.push_back({"type", std::move(type)});
    }
    return node;
}

Node scalar_node(std::string text) {
    Node node;
    node.text = std::move(text);
    return node;
}

Node seq_node() {
    Node node;
    node.kind = Node::Kind::Seq;
    return node;
}

Node map_node(std::vector<std::pair<std::string, Node>> entries) {
    Node node;
    node.kind = Node::Kind::Map;
    for (auto& [key, value] : entries) {
        node.entries.push_back({std::move(key), std::move(value)});
    }
    return node;
}

// Sets a mapping's value at a key, adding the key if it has none.
void set_node(Node& map, std::string_view key, Node value) {
    if (Node* existing = map.get(key)) {
        *existing = std::move(value);
    } else {
        map.entries.push_back({std::string(key), std::move(value)});
    }
}

// Calls change on every reference to an object of the same file under node.
void for_each_local_ref(Node& node, const std::function<std::int64_t(std::int64_t)>& change) {
    if (node.is_map()) {
        const Node* guid = node.get("guid");
        if (Node* file_id = node.get("fileID");
            file_id != nullptr && (guid == nullptr || guid->string().empty())) {
            const std::int64_t id = file_id->integer().value_or(0);
            if (id != 0) {
                file_id->text = std::to_string(change(id));
            }
            return;
        }
        for (auto& entry : node.entries) {
            for_each_local_ref(entry.value, change);
        }
    } else if (node.is_seq()) {
        for (Node& item : node.items) {
            for_each_local_ref(item, change);
        }
    }
}

// Sets the value at one of a prefab instance's property paths, such as "m_LocalPosition.x" or
// "m_Points.m_Paths.Array.data[0].Array.data[2].y".
void set_property(Node& body, std::string_view path, std::string_view value,
                  const std::optional<ObjectRef>& reference) {
    Node* node = &body;
    const std::vector<std::string_view> parts = split_path(path, '.');
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const std::string_view part = parts[i];
        if (part == "Array" && i + 1 < parts.size()) {
            if (!node->is_seq()) {
                *node = Node{};
                node->kind = Node::Kind::Seq;
            }
            const std::string_view next = parts[i + 1];
            if (next == "size") {
                const auto size = parse_int(value);
                if (size && *size >= 0 && *size < 1000000) {
                    node->items.resize(static_cast<std::size_t>(*size));
                }
                return;
            }
            if (!next.starts_with("data[") || !next.ends_with("]")) {
                return;
            }
            const auto index = parse_int(next.substr(5, next.size() - 6));
            if (!index || *index < 0 || *index >= 1000000) {
                return;
            }
            const auto at = static_cast<std::size_t>(*index);
            if (at >= node->items.size()) {
                node->items.resize(at + 1);
            }
            node = &node->items[at];
            ++i;
            continue;
        }
        if (!node->is_map()) {
            *node = Node{};
            node->kind = Node::Kind::Map;
        }
        Node* child = node->get(part);
        if (child == nullptr) {
            node->entries.push_back({std::string(part), Node{}});
            child = &node->entries.back().value;
        }
        node = child;
    }
    if (reference && (!reference->null() || !reference->guid.empty())) {
        *node = ref_node(*reference);
    } else if (node->is_map() && node->get("fileID") != nullptr) {
        *node = ref_node(ObjectRef{});
    } else {
        *node = Node{};
        node->text = value;
    }
}

// The bits of a 32-bit mask written as eight hex digits of little-endian bytes, as Unity writes
// Physics2DSettings' m_LayerCollisionMatrix.
std::optional<std::uint32_t> hex_word(std::string_view text) {
    if (text.size() != 8) {
        return std::nullopt;
    }
    std::uint32_t value = 0;
    for (std::size_t byte = 0; byte < 4; ++byte) {
        std::uint32_t bits = 0;
        for (std::size_t digit = 0; digit < 2; ++digit) {
            const char c = text[(byte * 2) + digit];
            std::uint32_t nibble = 0;
            if (c >= '0' && c <= '9') {
                nibble = static_cast<std::uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                nibble = static_cast<std::uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                nibble = static_cast<std::uint32_t>(c - 'A' + 10);
            } else {
                return std::nullopt;
            }
            bits = (bits << 4U) | nibble;
        }
        value |= bits << (8U * byte);
    }
    return value;
}

// xxHash64 with a seed of 0, from its specification (XXH64 in Cyan4973's xxHash).
std::uint64_t xxh64(std::string_view data) {
    constexpr std::uint64_t kP1 = 11400714785074694791ULL;
    constexpr std::uint64_t kP2 = 14029467366897019727ULL;
    constexpr std::uint64_t kP3 = 1609587929392839161ULL;
    constexpr std::uint64_t kP4 = 9650029242287828579ULL;
    constexpr std::uint64_t kP5 = 2870177450012600261ULL;
    const auto rotl = [](std::uint64_t x, unsigned r) { return (x << r) | (x >> (64U - r)); };
    const auto read = [&](std::size_t at, std::size_t bytes) {
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < bytes; ++i) {
            value |= std::uint64_t{static_cast<unsigned char>(data[at + i])} << (8U * i);
        }
        return value;
    };
    const auto mix = [&](std::uint64_t acc, std::uint64_t input) {
        return rotl(acc + (input * kP2), 31) * kP1;
    };
    const auto merge = [&](std::uint64_t acc, std::uint64_t value) {
        return ((acc ^ mix(0, value)) * kP1) + kP4;
    };
    const std::size_t size = data.size();
    std::size_t at = 0;
    std::uint64_t h = 0;
    if (size >= 32) {
        std::uint64_t v1 = kP1 + kP2;
        std::uint64_t v2 = kP2;
        std::uint64_t v3 = 0;
        std::uint64_t v4 = 0 - kP1;
        for (; at + 32 <= size; at += 32) {
            v1 = mix(v1, read(at, 8));
            v2 = mix(v2, read(at + 8, 8));
            v3 = mix(v3, read(at + 16, 8));
            v4 = mix(v4, read(at + 24, 8));
        }
        h = rotl(v1, 1) + rotl(v2, 7) + rotl(v3, 12) + rotl(v4, 18);
        h = merge(merge(merge(merge(h, v1), v2), v3), v4);
    } else {
        h = kP5;
    }
    h += size;
    for (; at + 8 <= size; at += 8) {
        h = (rotl(h ^ mix(0, read(at, 8)), 27) * kP1) + kP4;
    }
    if (at + 4 <= size) {
        h = (rotl(h ^ (read(at, 4) * kP1), 23) * kP2) + kP3;
        at += 4;
    }
    for (; at < size; ++at) {
        h = rotl(h ^ (read(at, 1) * kP5), 11) * kP1;
    }
    h ^= h >> 33U;
    h *= kP2;
    h ^= h >> 29U;
    h *= kP3;
    h ^= h >> 32U;
    return h;
}

// The ID Unity gives an object an importer adds to an asset, as a model's mesh: xxHash64 of
// "Type:<its class>-><its name><index>", read as signed, index counting objects of the same
// class and name from 0.
std::int64_t import_id(std::string_view type, std::string_view name, int index) {
    const std::string key =
        "Type:" + std::string(type) + "->" + std::string(name) + std::to_string(index);
    return static_cast<std::int64_t>(xxh64(key));
}

// --- Input -------------------------------------------------------------------------------------

json::Value key_binding(std::string_view key, bool physical) {
    json::Value binding;
    binding["type"] = "key";
    binding[physical ? "physical_key" : "key"] = key;
    return binding;
}

json::Value mouse_binding(std::string_view button) {
    json::Value binding;
    binding["type"] = "mouse_button";
    binding["button"] = button;
    return binding;
}

json::Value pad_button(std::string_view button) {
    json::Value binding;
    binding["type"] = "joypad_button";
    binding["button"] = button;
    return binding;
}

json::Value pad_motion(std::string_view axis, int value) {
    json::Value binding;
    binding["type"] = "joypad_motion";
    binding["axis"] = axis;
    binding["axis_value"] = value;
    return binding;
}

// The Godot key names project.json takes for keys both input systems name the same way.
std::string common_key(std::string_view name) {
    if (name.size() == 1 && name[0] >= 'a' && name[0] <= 'z') {
        return {static_cast<char>(name[0] - 'a' + 'A')};
    }
    if (name.size() == 1 && name[0] >= '0' && name[0] <= '9') {
        return std::string(name);
    }
    if (name.size() >= 2 && name.size() <= 3 && name[0] == 'f') {
        if (const auto number = parse_int(name.substr(1));
            number && *number >= 1 && *number <= 24) {
            return "F" + std::to_string(*number);
        }
    }
    return {};
}

// A key or button of the old Input Manager's axes, by the names it takes ("left", "space",
// "mouse 0", "joystick button 1", "[5]").
json::Value input_manager_binding(std::string_view name, std::string& why) {
    if (const std::string key = common_key(name); !key.empty()) {
        return key_binding(key, false);
    }
    static const std::map<std::string_view, std::string_view> kKeys{
        {"up", "Up"},
        {"down", "Down"},
        {"left", "Left"},
        {"right", "Right"},
        {"space", "Space"},
        {"return", "Enter"},
        {"enter", "Kp Enter"},
        {"escape", "Escape"},
        {"backspace", "Backspace"},
        {"tab", "Tab"},
        {"delete", "Delete"},
        {"insert", "Insert"},
        {"home", "Home"},
        {"end", "End"},
        {"page up", "PageUp"},
        {"page down", "PageDown"},
        {"left shift", "Shift"},
        {"right shift", "Shift"},
        {"left ctrl", "Ctrl"},
        {"right ctrl", "Ctrl"},
        {"left alt", "Alt"},
        {"right alt", "Alt"},
        {"left cmd", "Meta"},
        {"right cmd", "Meta"},
        {"left super", "Meta"},
        {"right super", "Meta"},
        {"left windows", "Meta"},
        {"right windows", "Meta"},
        {"caps lock", "CapsLock"},
        {"numlock", "NumLock"},
        {"scroll lock", "ScrollLock"},
        {"pause", "Pause"},
        {"print screen", "Print"},
        {"menu", "Menu"},
        {"`", "QuoteLeft"},
        {"-", "Minus"},
        {"=", "Equal"},
        {"[", "BracketLeft"},
        {"]", "BracketRight"},
        {";", "Semicolon"},
        {"'", "Apostrophe"},
        {",", "Comma"},
        {".", "Period"},
        {"/", "Slash"},
        {"\\", "BackSlash"},
        {"[+]", "Kp Add"},
        {"[-]", "Kp Subtract"},
        {"[*]", "Kp Multiply"},
        {"[/]", "Kp Divide"},
        {"[.]", "Kp Period"},
    };
    if (const auto it = kKeys.find(name); it != kKeys.end()) {
        return key_binding(it->second, false);
    }
    if (name.size() == 3 && name[0] == '[' && name[2] == ']' && name[1] >= '0' && name[1] <= '9') {
        return key_binding("Kp " + std::string(1, name[1]), false);
    }
    if (name.starts_with("mouse ")) {
        static constexpr std::array<std::string_view, 5> kButtons{"left", "right", "middle",
                                                                  "xbutton1", "xbutton2"};
        const auto index = parse_int(name.substr(6));
        if (index && *index >= 0 && *index < 5) {
            return mouse_binding(kButtons.at(static_cast<std::size_t>(*index)));
        }
    }
    if (name.starts_with("joystick ")) {
        // "joystick button 0", or "joystick 2 button 0" for the second gamepad.
        std::string_view rest = name.substr(9);
        std::int64_t device = -1;
        if (const auto space = rest.find(' '); space != std::string_view::npos && rest[0] != 'b') {
            device = parse_int(rest.substr(0, space)).value_or(0) - 1;
            rest = rest.substr(space + 1);
        }
        static constexpr std::array<std::string_view, 10> kButtons{
            "a",    "b",     "x",          "y",          "left_shoulder", "right_shoulder",
            "back", "start", "left_stick", "right_stick"};
        if (rest.starts_with("button ")) {
            const auto index = parse_int(rest.substr(7));
            if (index && *index >= 0 && *index < 10) {
                json::Value binding = pad_button(kButtons.at(static_cast<std::size_t>(*index)));
                if (device >= 0) {
                    binding["device"] = device;
                }
                return binding;
            }
        }
    }
    why = "\"" + std::string(name) + "\"";
    return nullptr;
}

// A control of the Input System, by its path ("<Keyboard>/space", "<Gamepad>/leftStick/up"),
// as a binding, or null with why.
json::Value control_binding(std::string_view path, std::string& why) {
    std::string device;
    std::string control;
    if (path.starts_with("<")) {
        const auto close = path.find('>');
        if (close != std::string_view::npos) {
            device = std::string(path.substr(1, close - 1));
            control = std::string(path.substr(std::min(close + 2, path.size())));
        }
    }
    if (const auto brace = device.find('{'); brace != std::string::npos) {
        device.resize(brace);
    }
    why = "\"" + std::string(path) + "\"";
    if (device == "Keyboard") {
        if (const std::string key = common_key(control); !key.empty()) {
            return key_binding(key, true);
        }
        static const std::map<std::string_view, std::string_view> kKeys{
            {"space", "Space"},
            {"enter", "Enter"},
            {"tab", "Tab"},
            {"backquote", "QuoteLeft"},
            {"quote", "Apostrophe"},
            {"semicolon", "Semicolon"},
            {"comma", "Comma"},
            {"period", "Period"},
            {"slash", "Slash"},
            {"backslash", "BackSlash"},
            {"leftBracket", "BracketLeft"},
            {"rightBracket", "BracketRight"},
            {"minus", "Minus"},
            {"equals", "Equal"},
            {"upArrow", "Up"},
            {"downArrow", "Down"},
            {"leftArrow", "Left"},
            {"rightArrow", "Right"},
            {"escape", "Escape"},
            {"backspace", "Backspace"},
            {"shift", "Shift"},
            {"leftShift", "Shift"},
            {"rightShift", "Shift"},
            {"ctrl", "Ctrl"},
            {"leftCtrl", "Ctrl"},
            {"rightCtrl", "Ctrl"},
            {"alt", "Alt"},
            {"leftAlt", "Alt"},
            {"rightAlt", "Alt"},
            {"leftMeta", "Meta"},
            {"rightMeta", "Meta"},
            {"leftWindows", "Meta"},
            {"rightWindows", "Meta"},
            {"leftCommand", "Meta"},
            {"rightCommand", "Meta"},
            {"leftApple", "Meta"},
            {"rightApple", "Meta"},
            {"capsLock", "CapsLock"},
            {"numLock", "NumLock"},
            {"scrollLock", "ScrollLock"},
            {"printScreen", "Print"},
            {"pause", "Pause"},
            {"insert", "Insert"},
            {"delete", "Delete"},
            {"home", "Home"},
            {"end", "End"},
            {"pageUp", "PageUp"},
            {"pageDown", "PageDown"},
            {"contextMenu", "Menu"},
            {"numpadEnter", "Kp Enter"},
            {"numpadDivide", "Kp Divide"},
            {"numpadMultiply", "Kp Multiply"},
            {"numpadPlus", "Kp Add"},
            {"numpadMinus", "Kp Subtract"},
            {"numpadPeriod", "Kp Period"},
        };
        if (const auto it = kKeys.find(control); it != kKeys.end()) {
            return key_binding(it->second, true);
        }
        if (control.size() == 7 && control.starts_with("numpad") && control[6] >= '0' &&
            control[6] <= '9') {
            return key_binding("Kp " + std::string(1, control[6]), true);
        }
        return nullptr;
    }
    if (device == "Mouse") {
        static const std::map<std::string_view, std::string_view> kButtons{
            {"leftButton", "left"},         {"rightButton", "right"},
            {"middleButton", "middle"},     {"backButton", "xbutton1"},
            {"forwardButton", "xbutton2"},  {"scroll/up", "wheel_up"},
            {"scroll/down", "wheel_down"},  {"scroll/left", "wheel_left"},
            {"scroll/right", "wheel_right"}};
        if (const auto it = kButtons.find(control); it != kButtons.end()) {
            return mouse_binding(it->second);
        }
        why += ": read the mouse's position and motion in code";
        return nullptr;
    }
    if (device == "Touchscreen" || device == "Pointer" || device == "Pen") {
        // Taps and presses: touches press the left mouse button, as Godot emulates the mouse.
        if (control.ends_with("press") || control.ends_with("tap") || control == "tip") {
            return mouse_binding("left");
        }
        why += ": read touches in code";
        return nullptr;
    }
    static constexpr std::array<std::string_view, 7> kPads{"Gamepad",
                                                           "XInputController",
                                                           "DualShockGamepad",
                                                           "DualSenseGamepadHID",
                                                           "SwitchProControllerHID",
                                                           "DualShock4GamepadHID",
                                                           "XboxOneGamepad"};
    if (contains(kPads, std::string_view(device))) {
        static const std::map<std::string_view, std::string_view> kButtons{
            {"buttonSouth", "a"},
            {"a", "a"},
            {"cross", "a"},
            {"buttonEast", "b"},
            {"b", "b"},
            {"circle", "b"},
            {"buttonWest", "x"},
            {"x", "x"},
            {"square", "x"},
            {"buttonNorth", "y"},
            {"y", "y"},
            {"triangle", "y"},
            {"start", "start"},
            {"select", "back"},
            {"leftShoulder", "left_shoulder"},
            {"rightShoulder", "right_shoulder"},
            {"leftStickPress", "left_stick"},
            {"rightStickPress", "right_stick"},
            {"dpad/up", "dpad_up"},
            {"dpad/down", "dpad_down"},
            {"dpad/left", "dpad_left"},
            {"dpad/right", "dpad_right"}};
        if (const auto it = kButtons.find(control); it != kButtons.end()) {
            return pad_button(it->second);
        }
        static const std::map<std::string_view, std::pair<std::string_view, int>> kMotions{
            {"leftTrigger", {"trigger_left", 1}}, {"rightTrigger", {"trigger_right", 1}},
            {"leftStick/up", {"left_y", -1}},     {"leftStick/down", {"left_y", 1}},
            {"leftStick/left", {"left_x", -1}},   {"leftStick/right", {"left_x", 1}},
            {"rightStick/up", {"right_y", -1}},   {"rightStick/down", {"right_y", 1}},
            {"rightStick/left", {"right_x", -1}}, {"rightStick/right", {"right_x", 1}}};
        if (const auto it = kMotions.find(control); it != kMotions.end()) {
            return pad_motion(it->second.first, it->second.second);
        }
        return nullptr;
    }
    if (device.empty()) {
        why += ": bindings by usage or wildcard name no single control";
    } else {
        why += ": " + device + " bindings";
    }
    return nullptr;
}

// --- Scripts -----------------------------------------------------------------------------------

std::string trim_text(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r");
    return std::string(text.substr(first, last - first + 1));
}

bool is_word_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// What a C# file declares, from its lines: its classes, serialized fields and methods.
struct ScriptOutline {
    std::vector<std::string> classes;
    std::vector<std::string> fields;
    std::vector<std::string> methods;
};

ScriptOutline outline_csharp(std::string_view text) {
    ScriptOutline outline;
    bool in_comment = false;
    bool serialize_next = false;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = std::min(text.find('\n', start), text.size());
        std::string line = trim_text(text.substr(start, end - start));
        start = end + 1;
        if (in_comment) {
            const auto close = line.find("*/");
            if (close == std::string::npos) {
                continue;
            }
            line = trim_text(line.substr(close + 2));
            in_comment = false;
        }
        if (const auto open = line.find("/*"); open != std::string::npos) {
            in_comment = line.find("*/", open) == std::string::npos;
            line = trim_text(line.substr(0, open));
        }
        if (const auto comment = line.find("//"); comment != std::string::npos) {
            line = trim_text(line.substr(0, comment));
        }
        if (line.empty()) {
            continue;
        }
        bool serialized = serialize_next;
        serialize_next = false;
        // Attributes in brackets before the declaration.
        while (line.starts_with("[")) {
            const auto close = line.find(']');
            if (close == std::string::npos) {
                break;
            }
            const std::string attribute = line.substr(1, close - 1);
            if (attribute.starts_with("SerializeField")) {
                serialized = true;
            }
            line = trim_text(line.substr(close + 1));
        }
        if (line.empty()) {
            serialize_next = serialized;
            continue;
        }
        if (const auto at = line.find("class ");
            at != std::string::npos && (at == 0 || line[at - 1] == ' ')) {
            std::string declaration = trim_text(line.substr(at + 6));
            if (const auto brace = declaration.find('{'); brace != std::string::npos) {
                declaration = trim_text(declaration.substr(0, brace));
            }
            if (const auto where = declaration.find(" where "); where != std::string::npos) {
                declaration.resize(where);
            }
            outline.classes.push_back(std::move(declaration));
            continue;
        }
        const auto paren = line.find('(');
        const auto equals = line.find('=');
        const bool statement = line.starts_with("if ") || line.starts_with("if(") ||
                               line.starts_with("for") || line.starts_with("while") ||
                               line.starts_with("return") || line.starts_with("using ") ||
                               line.starts_with("namespace ") || line.starts_with("switch") ||
                               line.starts_with("new ") || line.starts_with("else") ||
                               line.starts_with("case ") || line.starts_with("var ");
        if (statement) {
            continue;
        }
        if (line.back() == ';' && (paren == std::string::npos || equals < paren)) {
            const bool is_public = line.starts_with("public ");
            const bool modifier = line.find("static ") != std::string::npos ||
                                  line.find("const ") != std::string::npos ||
                                  line.find("readonly ") != std::string::npos ||
                                  line.find("event ") != std::string::npos ||
                                  line.find(" get;") != std::string::npos;
            if ((is_public || serialized) && !modifier) {
                std::string field = line.substr(0, line.size() - 1);
                for (const std::string_view prefix :
                     {"public ", "private ", "protected ", "internal "}) {
                    if (field.starts_with(prefix)) {
                        field = field.substr(prefix.size());
                    }
                }
                outline.fields.push_back(trim_text(field));
            }
            continue;
        }
        if (paren != std::string::npos && paren > 0 &&
            (equals == std::string::npos || equals > paren) && line.back() != ';') {
            std::size_t name_end = paren;
            while (name_end > 0 && line[name_end - 1] == ' ') {
                --name_end;
            }
            std::size_t name_start = name_end;
            while (name_start > 0 && is_word_char(line[name_start - 1])) {
                --name_start;
            }
            // A method has a return type or modifier before its name.
            if (name_start == 0 || name_start == name_end) {
                continue;
            }
            std::string name = line.substr(name_start, name_end - name_start);
            if (!contains(outline.methods, name)) {
                outline.methods.push_back(std::move(name));
            }
        }
    }
    return outline;
}

// --- The importer ------------------------------------------------------------------------------

class UnityImporter {
public:
    UnityImporter(std::string source, std::string prefix, bool copy_assets)
        : source_(std::move(source)), prefix_(std::move(prefix)), copy_assets_(copy_assets) {
        world::register_builtin_components(components_);
    }

    Result<ImportPlan> run(const Existing& existing) {
        auto version = platform::read_file(source_ + "ProjectSettings/ProjectVersion.txt");
        if (!version) {
            return Error{.code = ErrorCode::NotFound,
                         .message = "no ProjectSettings/ProjectVersion.txt in " + source_ +
                                    ": not a Unity project"};
        }
        if (auto parsed = unity::parse_document(version.value()); parsed) {
            unity_version_ = parsed.value().string_at("m_EditorVersion");
        }
        const std::vector<std::string_view> numbers = split_path(unity_version_, '.');
        const std::int64_t major = numbers.empty() ? 0 : parse_int(numbers[0]).value_or(0);
        const std::int64_t minor = numbers.size() < 2 ? 0 : parse_int(numbers[1]).value_or(0);
        if (major != 0 && (major < 2018 || (major == 2018 && minor < 3))) {
            return Error{.code = ErrorCode::InvalidArgument,
                         .message = "a project from Unity " + unity_version_ +
                                    ": prefabs before 2018.3 are saved another way; open it in "
                                    "Unity 2018.3 or later and save it there first"};
        }
        auto listed = platform::list_files(
            source_ + "Assets/", [](std::string_view folder) { return !folder.ends_with("~/"); });
        if (!listed) {
            return listed.error();
        }
        for (const std::string& file : listed.value()) {
            files_.push_back("Assets/" + file);
        }
        read_metas();
        read_settings();
        skeletons_.emplace(existing);
        for (const std::string& file : files_) {
            take_file(file);
        }
        convert_mixers();
        count_dimensions();
        for (const std::string& file : files_) {
            const std::string extension = extension_of(file);
            if (extension == ".prefab" || extension == ".unity") {
                (void)convert_file(file, 0);
            }
        }
        for (const TileSetOut& set : tile_sets_) {
            write_tile_set(set); // with the physics a later tile map gave it
        }
        convert_unused_animation();
        json::Value project = convert_project(existing.project);
        convert_input(project);
        write_project(std::move(project));
        write_tasks(existing.work);
        write_skeletons();
        write_report();
        return std::move(plan_);
    }

private:
    // --- Files and assets ---

    struct Asset {
        std::string path; // relative to the Unity project's folder, such as "Assets/a.png"
        Node meta;
    };

    void add_file(std::string path, std::string contents) {
        plan_.files.emplace_back(std::move(path), std::move(contents));
    }

    [[nodiscard]] std::string destination(std::string_view path) const {
        return prefix_ + std::string(path);
    }

    // Keeps only what validates, reporting the rest, so one bad value can't stop a file.
    template <typename Validate>
    void keep_valid(json::Value& doc, const std::string& path, Validate validate) {
        for (int pass = 0; pass < 64; ++pass) {
            const std::vector<json::ValidationError> errors = validate(doc);
            if (errors.empty()) {
                return;
            }
            std::vector<std::string> removed;
            for (const json::ValidationError& error : errors) {
                problems_.add("invalid value", path + ": " + error.path, error.message);
                if (pointer_within(removed, error.path)) {
                    continue;
                }
                if (!remove_pointer(doc, error.path)) {
                    return;
                }
                removed.push_back(error.path);
            }
        }
    }

    // Learns each asset's GUID from its .meta file.
    void read_metas() {
        const std::set<std::string> listed(files_.begin(), files_.end());
        for (const std::string& file : files_) {
            if (extension_of(file) != ".meta") {
                continue;
            }
            const std::string asset = file.substr(0, file.size() - 5);
            auto text = platform::read_file(source_ + file);
            if (!text) {
                continue;
            }
            auto meta = unity::parse_document(text.value());
            if (!meta) {
                problems_.add("unreadable file", file, meta.error().message);
                continue;
            }
            const std::string guid = meta.value().string_at("guid");
            if (guid.empty()) {
                continue;
            }
            assets_by_guid_[guid] = Asset{.path = asset, .meta = std::move(meta.value())};
        }
    }

    [[nodiscard]] const Asset* asset_of(const std::string& guid) const {
        const auto it = assets_by_guid_.find(guid);
        return it != assets_by_guid_.end() ? &it->second : nullptr;
    }

    // An object of a project settings file, by its type.
    std::optional<Node> settings(std::string_view file, std::string_view type) {
        auto text = platform::read_file(source_ + "ProjectSettings/" + std::string(file));
        if (!text) {
            return std::nullopt;
        }
        auto objects = unity::parse_file(text.value());
        if (!objects) {
            problems_.add("unreadable file", "ProjectSettings/" + std::string(file),
                          objects.error().message);
            return std::nullopt;
        }
        for (Object& object : objects.value()) {
            if (object.type == type) {
                return std::move(object.body);
            }
        }
        return std::nullopt;
    }

    void read_settings() {
        player_ = settings("ProjectSettings.asset", "PlayerSettings").value_or(Node{});
        if (const Node* width = player_.get("defaultScreenWidth");
            width != nullptr && width->number()) {
            window_width_ = *width->number();
        }
        if (const Node* height = player_.get("defaultScreenHeight");
            height != nullptr && height->number()) {
            window_height_ = *height->number();
        }
        // The units: the pixels per unit most of the project's sprites use. Sprites saved as
        // assets of their own, as projects recovered from games have them, carry theirs; the
        // textures they draw from keep the default.
        std::map<double, int> ppus;
        std::set<std::string> drawn_from;
        for (const auto& [guid, asset] : assets_by_guid_) {
            if (extension_of(asset.path) != ".asset") {
                continue;
            }
            const auto text = platform::read_file(source_ + asset.path);
            if (!text || text.value().find("\nSprite:") == std::string::npos) {
                continue;
            }
            const auto objects = unity::parse_file(text.value());
            for (const Object& object : objects ? objects.value() : std::vector<Object>{}) {
                if (object.type == "Sprite") {
                    ++ppus[object.body.number_at("m_PixelsToUnits", 100.0)];
                    if (const auto texture = object_ref_at(object.body, "m_RD.texture")) {
                        drawn_from.insert(texture->guid);
                    }
                }
            }
        }
        for (const auto& [guid, asset] : assets_by_guid_) {
            const Node* importer = asset.meta.get("TextureImporter");
            const bool drawn = drawn_from.contains(guid);
            if (importer != nullptr && (importer->integer_at("textureType", 0) == 8 || drawn)) {
                if (!drawn) {
                    ++ppus[importer->number_at("spritePixelsToUnits", 100.0)];
                }
                const std::int64_t filter = importer->integer_at("textureSettings.filterMode", 1);
                ++(filter == 0 ? nearest_textures_ : linear_textures_);
            }
        }
        int best = 0;
        for (const auto& [ppu, count] : ppus) {
            if (count > best && ppu > 0.0) {
                ppu_ = ppu;
                best = count;
            }
        }
        if (const auto physics = settings("Physics2DSettings.asset", "Physics2DSettings")) {
            physics_ = *physics;
            const std::string matrix = physics->string_at("m_LayerCollisionMatrix");
            for (std::size_t layer = 0; layer < 32 && (layer + 1) * 8 <= matrix.size(); ++layer) {
                if (const auto bits = hex_word(std::string_view(matrix).substr(layer * 8, 8))) {
                    layer_masks_.at(layer) = *bits;
                }
            }
            if (const auto material = unity::object_ref_at(*physics, "m_DefaultMaterial");
                material && !material->guid.empty()) {
                default_material_ = physics_material(material->guid);
            }
        }
        if (const auto physics = settings("DynamicsManager.asset", "PhysicsManager")) {
            physics3d_ = *physics;
            const std::string matrix = physics->string_at("m_LayerCollisionMatrix");
            for (std::size_t layer = 0; layer < 32 && (layer + 1) * 8 <= matrix.size(); ++layer) {
                if (const auto bits = hex_word(std::string_view(matrix).substr(layer * 8, 8))) {
                    layer_masks_3d_.at(layer) = *bits;
                }
            }
            if (const auto material = unity::object_ref_at(*physics, "m_DefaultMaterial");
                material && !material->guid.empty()) {
                default_material_3d_ = physics_material_3d(material->guid);
            }
        }
        if (const auto tags = settings("TagManager.asset", "TagManager")) {
            if (const Node* layers = tags->get("layers"); layers != nullptr) {
                for (std::size_t i = 0; i < layers->items.size() && i < 32; ++i) {
                    if (!layers->items[i].string().empty()) {
                        layer_names_[i] = std::string(layers->items[i].string());
                    }
                }
            }
            if (const Node* sorting = tags->get("m_SortingLayers"); sorting != nullptr) {
                for (const Node& layer : sorting->items) {
                    sorting_layers_.emplace_back(layer.integer_at("uniqueID", 0),
                                                 layer.string_at("name"));
                }
            }
        }
        for (std::size_t i = 0; i < sorting_layers_.size(); ++i) {
            if (sorting_layers_[i].first == 0) {
                default_sorting_ = static_cast<int>(i);
            }
        }
        if (sorting_layers_.size() > 1) {
            sorting_step_ =
                std::min(1000, (2 * kMaxZIndex) / static_cast<int>(sorting_layers_.size()));
        }
        if (const auto build = settings("EditorBuildSettings.asset", "EditorBuildSettings")) {
            if (const Node* scenes = build->get("m_Scenes"); scenes != nullptr) {
                for (const Node& scene : scenes->items) {
                    const std::string path = scene.string_at("path");
                    if (scene.flag_at("enabled", true) && !path.empty() &&
                        std::ranges::binary_search(files_, path)) {
                        main_source_ = path;
                        break;
                    }
                }
            }
        }
        if (main_source_.empty()) {
            for (const std::string& file : files_) {
                if (extension_of(file) == ".unity") {
                    main_source_ = file;
                    problems_.add("main scene", "ProjectSettings/EditorBuildSettings.asset",
                                  "it lists no scene, so the first one, " + file +
                                      ", is the main scene");
                    break;
                }
            }
        }
    }

    // Copies an asset, or notes one that can't come along.
    void take_file(const std::string& file) {
        const std::string extension = extension_of(file);
        if (extension == ".meta" || extension == ".unity" || extension == ".prefab" ||
            extension == ".anim" || extension == ".controller" ||
            extension == ".overridecontroller" || extension == ".mixer" || extension == ".mat" ||
            extension == ".physicMaterial" || extension == ".physicsMaterial") {
            return; // converted with what uses them
        }
        const bool editor = file.find("/Editor/") != std::string::npos;
        if (extension == ".cs") {
            if (editor) {
                return; // editor tools, not part of the game
            }
            if (copy_assets_) {
                copy(file);
            }
            return;
        }
        if (is_texture_file(extension) || is_sound_file(extension) || is_font_file(extension) ||
            is_model_file(extension) || is_data_file(extension) || extension == ".mtl") {
            if (!is_data_file(extension) && extension != ".mtl") {
                ++assets_;
            }
            if (copy_assets_) {
                copy(file);
            }
            return;
        }
        static const std::map<std::string_view, std::string_view> kWhys{
            {".shader", "a game's own shaders are on the roadmap"},
            {".shadergraph", "a game's own shaders are on the roadmap"},
            {".dll", "compiled plugins don't load; closed SDKs come in as SDK packages (on the "
                     "roadmap)"},
            {".so", "compiled plugins don't load; closed SDKs come in as SDK packages (on the "
                    "roadmap)"},
            {".aar", "compiled plugins don't load; closed SDKs come in as SDK packages (on the "
                     "roadmap)"},
            {".spriteatlas", "not needed: sprites draw from their own textures"},
            {".spriteatlasv2", "not needed: sprites draw from their own textures"},
            {".rendertexture", "render textures aren't supported yet"},
            {".lighting",
             "baked lighting settings don't convert; lights light the scene as it runs"},
            {".playable", "Timeline isn't supported; animate with AnimationPlayer"},
        };
        if (const auto it = kWhys.find(extension); it != kWhys.end()) {
            problems_.add("file type " + extension, file, std::string(it->second));
            return;
        }
        if (const std::string why = unsupported_asset(extension); !why.empty()) {
            problems_.add("file type " + extension, file, why);
        }
    }

    void copy(const std::string& file) {
        if (auto bytes = platform::read_file(source_ + file); bytes) {
            add_file(destination(file), std::move(bytes.value()));
        } else {
            problems_.add("unreadable file", file, bytes.error().message);
        }
    }

    // --- Textures and sprites ---

    using Outline = std::vector<std::array<double, 2>>;

    struct Sprite {
        std::string texture;            // its texture's path in the destination
        std::string name;               // the sprite's, or the texture's for a single sprite
        std::array<double, 4> region{}; // x, y, width and height in pixels, from the top left
        bool whole = true;              // the whole texture
        double width = 0.0;
        double height = 0.0;
        double pivot_x = 0.5; // from the bottom left, as a fraction of the size, as Unity's
        double pivot_y = 0.5;
        double ppu = 100.0;
        std::array<double, 4> border{}; // pixels kept at their size when sliced: left, bottom,
                                        // right and top, as Unity's x, y, z and w
        std::string filter;
        std::string normal_map; // its texture's normal map's path in the destination, if any
        // Its physics shape: outlines in pixels around the middle of its rect, y up.
        std::vector<Outline> shape;
        bool fallback_shape = true; // Unity makes one from the alpha when it has none
    };

    struct Texture {
        bool ok = false;
        std::string why;
        std::string path; // in the Unity project
        int width = 0;
        int height = 0;
        std::map<std::int64_t, Sprite> sprites; // by file ID
        Sprite base;                            // the settings its sprites start from
    };

    static std::vector<Outline> outlines(const Node* node) {
        std::vector<Outline> out;
        for (const Node& path : node != nullptr ? node->items : std::vector<Node>{}) {
            Outline outline;
            for (const Node& p : path.items) {
                outline.push_back({p.number_at("x"), p.number_at("y")});
            }
            if (!outline.empty()) {
                out.push_back(std::move(outline));
            }
        }
        return out;
    }

    const Texture& texture(const std::string& guid) {
        auto [it, inserted] = textures_.try_emplace(guid);
        Texture& out = it->second;
        if (!inserted) {
            return out;
        }
        const Asset* asset = asset_of(guid);
        if (asset == nullptr) {
            out.why = "a texture from outside the project's Assets folder";
            return out;
        }
        const std::string extension = extension_of(asset->path);
        if (extension == ".asset") {
            sprite_asset(out, *asset);
            return out;
        }
        if (!is_texture_file(extension)) {
            const std::string why = unsupported_asset(extension);
            out.why = asset->path + ": " + (why.empty() ? "not a texture" : why);
            return out;
        }
        auto bytes = platform::read_file(source_ + asset->path);
        std::optional<std::pair<int, int>> size;
        if (bytes && extension == ".png") {
            size = png_size(bytes.value());
        } else if (bytes && extension == ".bmp") {
            size = bmp_size(bytes.value());
        } else if (bytes) {
            if (const auto image = render::decode_image(bytes.value())) {
                size = std::pair{static_cast<int>(image.value().width),
                                 static_cast<int>(image.value().height)};
            }
        }
        if (!size) {
            out.why = asset->path + ": unreadable texture";
            return out;
        }
        const auto [width, height] = *size;
        out.path = asset->path;
        out.width = width;
        out.height = height;
        const Node empty;
        const Node* importer = asset->meta.get("TextureImporter");
        if (importer == nullptr) {
            importer = &empty;
        }
        Sprite base;
        base.texture = destination(asset->path);
        base.ppu = importer->number_at("spritePixelsToUnits", 100.0);
        if (base.ppu <= 0.0) {
            base.ppu = 100.0;
        }
        base.filter =
            importer->integer_at("textureSettings.filterMode", 1) == 0 ? "nearest" : "linear";
        base.fallback_shape = importer->flag_at("spriteGenerateFallbackPhysicsShape", true);
        // A secondary texture named _NormalMap is the sprites' normal map, laid out as the
        // texture, as Godot's CanvasTexture takes one.
        if (const Node* secondary = importer->path("spriteSheet.secondaryTextures")) {
            for (const Node& entry : secondary->items) {
                const auto ref = ref_at(entry, "texture");
                const Asset* map = ref && !ref->null() ? asset_of(ref->guid) : nullptr;
                if (entry.string_at("name") == "_NormalMap" && map != nullptr &&
                    is_texture_file(extension_of(map->path))) {
                    base.normal_map = destination(map->path);
                }
            }
        }
        out.base = base;
        const Node* sheet = importer->path("spriteSheet.sprites");
        if (importer->integer_at("spriteMode", 1) == 2 && sheet != nullptr) {
            // Each sprite's file ID: its internalID, or (before Unity 2019.3) the ID its name has
            // in fileIDToRecycleName.
            std::map<std::string, std::int64_t> ids;
            if (const Node* table = importer->get("internalIDToNameTable")) {
                for (const Node& entry : table->items) {
                    if (const Node* first = entry.get("first");
                        first != nullptr && first->is_map()) {
                        for (const auto& [type, id] : first->entries) {
                            ids[entry.string_at("second")] = id.integer().value_or(0);
                        }
                    }
                }
            }
            if (const Node* table = importer->get("fileIDToRecycleName")) {
                for (const auto& [id, name] : table->entries) {
                    ids[std::string(name.string())] = parse_int(id).value_or(0);
                }
            }
            for (std::size_t i = 0; i < sheet->items.size(); ++i) {
                const Node& entry = sheet->items[i];
                Sprite sprite = base;
                sprite.name = entry.string_at("name");
                sprite.shape = outlines(entry.get("physicsShape"));
                const double x = entry.number_at("rect.x");
                const double y = entry.number_at("rect.y");
                sprite.width = entry.number_at("rect.width");
                sprite.height = entry.number_at("rect.height");
                // Unity's rects go up from the bottom of the texture.
                sprite.region = {x, static_cast<double>(height) - y - sprite.height, sprite.width,
                                 sprite.height};
                sprite.whole = false;
                sprite.border = {entry.number_at("border.x"), entry.number_at("border.y"),
                                 entry.number_at("border.z"), entry.number_at("border.w")};
                std::tie(sprite.pivot_x, sprite.pivot_y) =
                    alignment_pivot(entry.integer_at("alignment", 0), entry.get("pivot"));
                std::int64_t id = entry.integer_at("internalID", 0);
                if (id == 0) {
                    const auto named = ids.find(entry.string_at("name"));
                    id = named != ids.end() ? named->second
                                            : kSingleSprite + (2 * static_cast<std::int64_t>(i));
                }
                out.sprites[id] = std::move(sprite);
            }
        } else {
            Sprite sprite = base;
            sprite.name = file_stem(asset->path);
            sprite.shape = outlines(importer->path("spriteSheet.physicsShape"));
            sprite.width = width;
            sprite.height = height;
            sprite.border = {
                importer->number_at("spriteBorder.x"), importer->number_at("spriteBorder.y"),
                importer->number_at("spriteBorder.z"), importer->number_at("spriteBorder.w")};
            std::tie(sprite.pivot_x, sprite.pivot_y) =
                alignment_pivot(importer->integer_at("alignment", 0), importer->get("spritePivot"));
            out.sprites[kSingleSprite] = std::move(sprite);
        }
        out.ok = true;
        return out;
    }

    // A sprite saved as an asset of its own, a .asset file holding a Sprite, as projects
    // recovered from games have them: its rect, pivot and physics shape on the texture its render
    // data draws from.
    void sprite_asset(Texture& out, const Asset& asset) {
        auto text = platform::read_file(source_ + asset.path);
        auto objects =
            text ? unity::parse_file(text.value()) : Result<std::vector<Object>>(text.error());
        const Object* found = nullptr;
        if (objects) {
            const auto sprite = std::ranges::find(objects.value(), "Sprite", &Object::type);
            found = sprite != objects.value().end() ? &*sprite : nullptr;
        }
        if (found == nullptr) {
            out.why = asset.path + ": not a texture or a sprite";
            return;
        }
        const Node& body = found->body;
        const auto ref = object_ref_at(body, "m_RD.texture");
        if (!ref || ref->null() || ref->guid.empty()) {
            out.why = asset.path + ": a sprite with no texture";
            return;
        }
        const Texture& source = texture(ref->guid);
        if (!source.ok) {
            out.why = asset.path + ": " + source.why;
            return;
        }
        // Unity's SpriteSettings: packed, packing mode (0 tight) and rotation in an atlas.
        const std::int64_t settings = body.integer_at("m_RD.settingsRaw", 0);
        if ((settings & 1) != 0 && ((settings >> 2) & 0xF) != 0) {
            out.why = asset.path + ": a sprite turned in its atlas";
            return;
        }
        if ((settings & 1) != 0 && ((settings >> 1) & 1) == 0) {
            problems_.add("tightly packed sprite", asset.path,
                          "packed to its outline in an atlas, so its neighbors may show at its "
                          "edges");
        }
        Sprite sprite = source.base;
        sprite.name = body.string_at("m_Name");
        const double rect_width = body.number_at("m_Rect.width");
        const double rect_height = body.number_at("m_Rect.height");
        // Where its pixels are on the texture, which is an atlas's when it was packed into one,
        // and how far that is inside its rect when transparent edges were trimmed.
        const double x = body.number_at("m_RD.textureRect.x", body.number_at("m_Rect.x"));
        const double y = body.number_at("m_RD.textureRect.y", body.number_at("m_Rect.y"));
        sprite.width = body.number_at("m_RD.textureRect.width", rect_width);
        sprite.height = body.number_at("m_RD.textureRect.height", rect_height);
        if (sprite.width <= 0.0 || sprite.height <= 0.0) {
            sprite.width = rect_width;
            sprite.height = rect_height;
        }
        const double offset_x = body.number_at("m_RD.textureRectOffset.x");
        const double offset_y = body.number_at("m_RD.textureRectOffset.y");
        sprite.region = {x, static_cast<double>(source.height) - y - sprite.height, sprite.width,
                         sprite.height};
        sprite.whole =
            x == 0.0 && y == 0.0 && sprite.width == source.width && sprite.height == source.height;
        sprite.ppu = body.number_at("m_PixelsToUnits", 100.0);
        if (sprite.ppu <= 0.0) {
            sprite.ppu = 100.0;
        }
        sprite.border = {body.number_at("m_Border.x"), body.number_at("m_Border.y"),
                         body.number_at("m_Border.z"), body.number_at("m_Border.w")};
        const double pivot_x = (body.number_at("m_Pivot.x", 0.5) * rect_width) - offset_x;
        const double pivot_y = (body.number_at("m_Pivot.y", 0.5) * rect_height) - offset_y;
        sprite.pivot_x = pivot_x / sprite.width;
        sprite.pivot_y = pivot_y / sprite.height;
        // Its physics shape is in units around the pivot; the sprite's outlines are in pixels
        // around the middle of its rect.
        sprite.shape.clear();
        for (Outline outline : outlines(body.get("m_PhysicsShape"))) {
            for (auto& point : outline) {
                point = {tidy((point[0] * sprite.ppu) + pivot_x - (sprite.width / 2.0)),
                         tidy((point[1] * sprite.ppu) + pivot_y - (sprite.height / 2.0))};
            }
            sprite.shape.push_back(std::move(outline));
        }
        out.path = source.path;
        out.width = source.width;
        out.height = source.height;
        out.sprites[found->file_id] = std::move(sprite);
        out.ok = true;
    }

    const Sprite* sprite(const ObjectRef& ref, std::string& why) {
        const Texture& found = texture(ref.guid);
        if (!found.ok) {
            why = found.why;
            return nullptr;
        }
        const auto it = found.sprites.find(ref.file_id);
        if (it == found.sprites.end()) {
            why = "a sprite its texture's import settings don't list";
            return nullptr;
        }
        return &it->second;
    }

    // --- Physics materials ---

    struct Material {
        double friction = kDefaultFriction;
        double bounce = 0.0;
    };

    Material physics_material(const std::string& guid) {
        if (const auto it = materials_.find(guid); it != materials_.end()) {
            return it->second;
        }
        Material material = default_material_;
        if (const Asset* asset = asset_of(guid); asset != nullptr) {
            if (auto text = platform::read_file(source_ + asset->path); text) {
                if (auto objects = unity::parse_file(text.value()); objects) {
                    for (const Object& object : objects.value()) {
                        if (object.type == "PhysicsMaterial2D") {
                            // Box2D clamps bounciness to 1, and so does SlashEngine.
                            material.friction =
                                std::max(0.0, object.body.number_at("friction", kDefaultFriction));
                            material.bounce =
                                std::clamp(object.body.number_at("bounciness", 0.0), 0.0, 1.0);
                        }
                    }
                }
            }
        }
        materials_[guid] = material;
        return material;
    }

    // --- Audio mixers ---

    // A .mixer file: its objects by ID and its start snapshot's values.
    struct MixerFile {
        std::string path;
        std::string guid;
        std::string name;
        std::vector<Object> objects;
        std::map<std::int64_t, const Object*> by_id;
        const Object* controller = nullptr;
        std::map<std::string, double> values; // by parameter GUID
        int groups = 0;

        [[nodiscard]] const Object* find(std::int64_t id, int class_id) const {
            const auto it = by_id.find(id);
            return it != by_id.end() && it->second->class_id == class_id ? it->second : nullptr;
        }
        [[nodiscard]] double value(const std::string& parameter, double fallback) const {
            const auto it = values.find(parameter);
            return it != values.end() ? it->second : fallback;
        }
    };

    // A mixer group as the bus it becomes.
    struct GroupBus {
        const MixerFile* mixer = nullptr;
        const Object* group = nullptr;
        std::string bus;
        std::string send;
    };

    // Unity's built-in effects' settings when a snapshot leaves them out, by effect and by
    // setting name lowercased without spaces or dashes.
    static double effect_default(std::string_view effect, std::string_view setting) {
        static const std::map<std::pair<std::string_view, std::string_view>, double> kDefaults{
            {{"Lowpass", "cutofffreq"}, 5000.0},
            {{"Lowpass", "resonance"}, 1.0},
            {{"Pitch Shifter", "pitch"}, 1.0},
            {{"Pitch Shifter", "fftsize"}, 1024.0},
            {{"Pitch Shifter", "overlap"}, 4.0},
            {{"Highpass", "cutofffreq"}, 5000.0},
            {{"Highpass", "resonance"}, 1.0},
            {{"Lowpass Simple", "cutofffreq"}, 5000.0},
            {{"Highpass Simple", "cutofffreq"}, 5000.0},
            {{"Echo", "delay"}, 500.0},
            {{"Echo", "decay"}, 50.0},
            {{"Echo", "drymix"}, 100.0},
            {{"Echo", "wetmix"}, 100.0},
            {{"Distortion", "level"}, 0.5},
            {{"Chorus", "drymix"}, 0.5},
            {{"Chorus", "wetmixtap1"}, 0.5},
            {{"Chorus", "wetmixtap2"}, 0.5},
            {{"Chorus", "wetmixtap3"}, 0.5},
            {{"Chorus", "delay"}, 40.0},
            {{"Chorus", "rate"}, 0.8},
            {{"Chorus", "depth"}, 0.03},
            {{"Compressor", "threshold"}, 0.0},
            {{"Compressor", "attack"}, 50.0},
            {{"Compressor", "release"}, 50.0},
            {{"Compressor", "makeupgain"}, 0.0},
            {{"Duck Volume", "threshold"}, -10.0},
            {{"Duck Volume", "ratio"}, 2.0},
            {{"Duck Volume", "attacktime"}, 100.0},
            {{"Duck Volume", "releasetime"}, 100.0},
            {{"Duck Volume", "makeupgain"}, 0.0},
            {{"SFX Reverb", "drylevel"}, 0.0},
            {{"SFX Reverb", "room"}, 0.0},
            {{"SFX Reverb", "decaytime"}, 1.0},
            {{"SFX Reverb", "decayhfratio"}, 0.5},
            {{"SFX Reverb", "reflectdelay"}, 0.02},
            {{"SFX Reverb", "reverb"}, 0.0},
            {{"SFX Reverb", "reverbdelay"}, 0.04},
        };
        const auto it = kDefaults.find({effect, setting});
        return it != kDefaults.end() ? it->second : 0.0;
    }

    static std::string setting_key(std::string_view name) {
        std::string key;
        for (const char c : name) {
            if (std::isalnum(static_cast<unsigned char>(c)) != 0) {
                key += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
        }
        return key;
    }

    static double db_to_linear(double db) { return std::pow(10.0, db / 20.0); }

    // A list's items, or none for a missing node.
    static const std::vector<Node>& items_of(const Node* node) {
        static const std::vector<Node> kNone;
        return node != nullptr ? node->items : kNone;
    }

    // One of Unity's mixer effects as a bus effect, or nullopt with why (empty when the effect
    // is part of the bus rather than one of its effects).
    std::optional<json::Value> mixer_effect(const MixerFile& mixer, const Node& body,
                                            const std::string& where,
                                            const std::vector<std::string>& sidechains,
                                            std::string& why) {
        const std::string name = body.string_at("m_EffectName");
        std::map<std::string, double> settings;
        if (const Node* parameters = body.get("m_Parameters"); parameters != nullptr) {
            for (const Node& parameter : parameters->items) {
                const std::string guid = parameter.string_at("m_GUID");
                if (const auto it = mixer.values.find(guid); it != mixer.values.end()) {
                    settings[setting_key(parameter.string_at("m_ParameterName"))] = it->second;
                }
            }
        }
        const auto setting = [&](std::string_view key) {
            const auto it = settings.find(std::string(key));
            return it != settings.end() ? it->second : effect_default(name, key);
        };
        json::Value out;
        if (name == "Lowpass" || name == "Highpass" || name == "Lowpass Simple" ||
            name == "Highpass Simple") {
            out["type"] = name.starts_with("Low") ? "low_pass_filter" : "high_pass_filter";
            out["cutoff_hz"] = tidy(std::clamp(setting("cutofffreq"), 1.0, 20500.0));
            if (!name.ends_with("Simple")) {
                const double resonance = setting("resonance");
                if (resonance > 1.0) {
                    problems_.add("filter resonance above 1", where,
                                  "a filter's resonance goes up to 1, as Godot's");
                }
                out["resonance"] = tidy(std::clamp(resonance, 0.0, 1.0));
            }
        } else if (name == "Echo") {
            // Echoes that each repeat the last one, decay quieter.
            const double delay = setting("delay");
            const double decay = std::clamp(setting("decay") / 100.0, 0.0, 1.0);
            if (delay > 1500.0) {
                problems_.add("echo delay above 1500 ms", where,
                              "a delay's echoes come at most 1500 ms apart, as Godot's");
            }
            out["type"] = "delay";
            out["dry"] = tidy(std::clamp(setting("drymix") / 100.0, 0.0, 1.0));
            out["tap2_active"] = false;
            if (decay > 0.0) {
                out["tap1_active"] = false;
                out["feedback_active"] = true;
                out["feedback_delay_ms"] = tidy(std::clamp(delay, 0.0, 1500.0));
                out["feedback_level_db"] = tidy(std::max(20.0 * std::log10(decay), -60.0));
            } else {
                const double wet = std::clamp(setting("wetmix") / 100.0, 0.0, 1.0);
                out["tap1_delay_ms"] = tidy(std::clamp(delay, 0.0, 1500.0));
                out["tap1_level_db"] =
                    wet > 0.0 ? tidy(std::max(20.0 * std::log10(wet), -60.0)) : -60.0;
                out["tap1_pan"] = 0.0;
            }
        } else if (name == "Distortion") {
            // Unity's distortion is the waveshaper Godot's waveshape mode is.
            out["type"] = "distortion";
            out["mode"] = "waveshape";
            out["drive"] = tidy(std::clamp(setting("level"), 0.0, 1.0));
            out["keep_hf_hz"] = 20500.0;
        } else if (name == "Chorus") {
            const double delay = std::clamp(setting("delay"), 0.0, 50.0);
            json::Array voices;
            for (const std::string_view tap : {"wetmixtap1", "wetmixtap2", "wetmixtap3"}) {
                json::Value voice;
                voice["delay_ms"] = tidy(delay);
                voice["rate_hz"] = tidy(std::clamp(setting("rate"), 0.1, 20.0));
                voice["depth_ms"] =
                    tidy(std::clamp(std::clamp(setting("depth"), 0.0, 1.0) * delay, 0.0, 20.0));
                const double level = std::clamp(setting(tap), 0.0, 1.0);
                voice["level_db"] =
                    level > 0.0 ? tidy(std::max(20.0 * std::log10(level), -60.0)) : -60.0;
                voice["pan"] = 0.0;
                voices.push_back(std::move(voice));
            }
            out["type"] = "chorus";
            out["dry"] = tidy(std::clamp(setting("drymix"), 0.0, 1.0));
            out["wet"] = 1.0;
            out["voice_count"] = 3;
            out["voices"] = std::move(voices);
        } else if (name == "Compressor" || name == "Duck Volume") {
            const bool duck = name == "Duck Volume";
            if (duck && sidechains.empty()) {
                why = "no group sends to it, so it does nothing";
                return std::nullopt;
            }
            const double attack = setting(duck ? "attacktime" : "attack");
            if (attack > 2.0) {
                problems_.add("compressor attack above 2 ms", where,
                              "a compressor's attack_us goes up to 2000, as Godot's");
            }
            // The Duck Volume's ratio may be written as a percentage.
            double ratio = duck ? setting("ratio") : 2.5;
            if (ratio > 10.0) {
                ratio /= 100.0;
            }
            out["type"] = "compressor";
            out["threshold"] = tidy(std::clamp(setting("threshold"), -60.0, 0.0));
            out["ratio"] = tidy(std::clamp(ratio, 1.0, 48.0));
            if (const double gain = setting("makeupgain"); gain != 0.0) {
                out["gain"] = tidy(std::clamp(gain, -20.0, 20.0));
            }
            out["attack_us"] = tidy(std::clamp(attack * 1000.0, 20.0, 2000.0));
            out["release_ms"] =
                tidy(std::clamp(setting(duck ? "releasetime" : "release"), 20.0, 2000.0));
            if (duck) {
                out["sidechain"] = sidechains.front();
                if (sidechains.size() > 1) {
                    problems_.add("Duck Volume with sends from several groups", where,
                                  "the first group that sends to it drives it");
                }
            }
        } else if (name == "SFX Reverb") {
            // Freeverb's combs, about 1378 samples long at 44.1 kHz, take decay time T when each
            // pass keeps 10^(-3 * 0.03125 / T) of the sound; Godot's room_size sets that from
            // 0.7 to 0.98.
            const double decay = std::clamp(setting("decaytime"), 0.1, 20.0);
            const double feedback = std::pow(10.0, -3.0 * 0.03125 / decay);
            out["type"] = "reverb";
            out["room_size"] = tidy(std::clamp((feedback - 0.7) / 0.28, 0.0, 1.0));
            out["damping"] = tidy(std::clamp(1.0 - setting("decayhfratio"), 0.0, 1.0));
            out["dry"] = tidy(std::clamp(db_to_linear(setting("drylevel") / 100.0), 0.0, 1.0));
            out["wet"] = tidy(
                std::clamp(db_to_linear((setting("room") + setting("reverb")) / 100.0), 0.0, 1.0));
            out["predelay_msec"] = tidy(std::clamp(
                (setting("reflectdelay") + setting("reverbdelay")) * 1000.0, 20.0, 500.0));
            out["predelay_feedback"] = 0.0;
        } else if (name == "Pitch Shifter") {
            // Both are Bernsee's phase vocoder: Unity's overlap is Godot's oversampling, from 4.
            const double overlap = setting("overlap");
            if (overlap < 4.0) {
                problems_.add("pitch shifter overlap below 4", where,
                              "a pitch shift oversamples at least 4 times, as Godot's");
            }
            std::int64_t fft_size = 256;
            while (fft_size < 4096 && static_cast<double>(fft_size) < setting("fftsize")) {
                fft_size *= 2;
            }
            out["type"] = "pitch_shift";
            out["pitch_scale"] = tidy(std::clamp(setting("pitch"), 0.01, 16.0));
            out["fft_size"] = fft_size;
            out["oversampling"] =
                static_cast<std::int64_t>(std::clamp(std::round(overlap), 4.0, 32.0));
        } else if (name == "Attenuation" || name == "Send" || name == "Receive") {
            why.clear();
            return std::nullopt;
        } else {
            static const std::map<std::string_view, std::string_view> kWhys{
                {"Flange", "Godot's effects have no flanger"},
                {"Normalize", "Godot's effects have no normalizer"},
                {"ParamEQ", "Godot's EQs have fixed bands; one movable band doesn't convert"},
            };
            const auto it = kWhys.find(name);
            why = it != kWhys.end() ? std::string(it->second) : "an audio plugin's effect";
            return std::nullopt;
        }
        if (body.flag_at("m_Bypass")) {
            out["enabled"] = false;
        }
        if (body.flag_at("m_EnableWetMix") &&
            std::abs(mixer.value(body.string_at("m_MixLevel"), 0.0)) > 0.01) {
            problems_.add("effect wet level", where, "effects run fully wet");
        }
        return out;
    }

    // Reads the project's .mixer files and writes them as one .bus_layout.json file: the mixer
    // most groups go through gives Master, each group becomes a bus sending to its parent's, and
    // the start snapshot gives volumes and effect settings.
    void convert_mixers() {
        std::map<std::string, std::string> guid_of; // by path
        for (const auto& [guid, asset] : assets_by_guid_) {
            guid_of[asset.path] = guid;
        }
        std::deque<MixerFile> mixers;
        for (const std::string& file : files_) {
            if (extension_of(file) != ".mixer") {
                continue;
            }
            auto text = platform::read_file(source_ + file);
            auto objects =
                text ? unity::parse_file(text.value()) : Result<std::vector<Object>>(text.error());
            if (!objects) {
                problems_.add("unreadable file", file, objects.error().message);
                continue;
            }
            MixerFile& mixer = mixers.emplace_back();
            mixer.path = file;
            mixer.guid = guid_of[file];
            mixer.objects = std::move(objects.value());
            for (const Object& object : mixer.objects) {
                mixer.by_id[object.file_id] = &object;
                if (object.class_id == kAudioMixerController && mixer.controller == nullptr) {
                    mixer.controller = &object;
                }
                mixer.groups += object.class_id == kAudioMixerGroup ? 1 : 0;
            }
            if (mixer.controller == nullptr) {
                problems_.add("mixer without an AudioMixerController", file);
                mixers.pop_back();
                continue;
            }
            const Node& body = mixer.controller->body;
            mixer.name = body.string_at("m_Name");
            if (mixer.name.empty()) {
                mixer.name = std::string(file_stem(file));
            }
            const auto start = ref_at(body, "m_StartSnapshot");
            const Object* snapshot =
                start ? mixer.find(start->file_id, kAudioMixerSnapshot) : nullptr;
            if (snapshot != nullptr) {
                if (const Node* values = snapshot->body.get("m_FloatValues"); values != nullptr) {
                    for (const auto& [guid, value] : values->entries) {
                        if (const auto number = value.number()) {
                            mixer.values[guid] = *number;
                        }
                    }
                }
            }
            if (const Node* snapshots = body.get("m_Snapshots");
                snapshots != nullptr && snapshots->items.size() > 1) {
                std::string others;
                for (const Node& item : snapshots->items) {
                    const auto ref = unity::object_ref(item);
                    const Object* other =
                        ref ? mixer.find(ref->file_id, kAudioMixerSnapshot) : nullptr;
                    if (other != nullptr && other != snapshot) {
                        others.append(others.empty() ? "" : ", ")
                            .append(other->body.string_at("m_Name"));
                    }
                }
                problems_.add("mixer snapshots", file,
                              "buses take the start snapshot's values; " + others +
                                  " don't convert: gameplay code can set the buses instead");
            }
            if (const Node* exposed = body.get("m_ExposedParameters");
                exposed != nullptr && !exposed->items.empty()) {
                std::string names;
                for (const Node& item : exposed->items) {
                    names.append(names.empty() ? "" : ", ").append(item.string_at("name"));
                }
                problems_.add("exposed mixer parameters", file,
                              names + ": scripts that set them set buses with audio.set_bus or "
                                      "gameplay code instead");
            }
        }
        if (mixers.empty()) {
            return;
        }
        // Master comes from the mixer that outputs nowhere else and has the most groups.
        MixerFile* primary = nullptr;
        for (MixerFile& mixer : mixers) {
            const auto output = ref_at(mixer.controller->body, "m_OutputGroup");
            if ((!output || output->null()) &&
                (primary == nullptr || mixer.groups > primary->groups)) {
                primary = &mixer;
            }
        }
        if (primary == nullptr) {
            primary = &mixers.front();
        }
        std::vector<GroupBus> buses;
        std::set<std::string> taken{"Master"};
        std::map<std::pair<const MixerFile*, std::int64_t>, std::size_t> bus_of; // group: bus
        json::Value groups = json::Object{};
        std::function<void(const MixerFile&, std::int64_t, const std::string&, std::string,
                           const std::string&)>
            walk = [&](const MixerFile& mixer, std::int64_t id, const std::string& send,
                       std::string name, const std::string& above) {
                const Object* group = mixer.find(id, kAudioMixerGroup);
                if (group == nullptr || bus_of.contains({&mixer, id})) {
                    return;
                }
                const std::string own = group->body.string_at("m_Name");
                if (name.empty()) {
                    name = own.empty() ? std::string("Bus") : own;
                    for (int n = 2; !taken.insert(name).second; ++n) {
                        name = (own.empty() ? std::string("Bus") : own) + " " + std::to_string(n);
                    }
                }
                const std::string path = above.empty() ? own : above + "/" + own;
                groups[mixer.path][path] = name;
                mixer_buses_[mixer.guid][id] = name;
                bus_of[{&mixer, id}] = buses.size();
                buses.push_back(
                    GroupBus{.mixer = &mixer, .group = group, .bus = name, .send = send});
                if (const Node* children = group->body.get("m_Children"); children != nullptr) {
                    for (const Node& child : children->items) {
                        if (const auto ref = unity::object_ref(child)) {
                            walk(mixer, ref->file_id, name, {}, path);
                        }
                    }
                }
            };
        const auto master_of = [](const MixerFile& mixer) {
            const auto ref = ref_at(mixer.controller->body, "m_MasterGroup");
            return ref ? ref->file_id : 0;
        };
        walk(*primary, master_of(*primary), {}, "Master", {});
        // The other mixers' Masters send to the group each outputs to, once it has a bus.
        std::vector<MixerFile*> waiting;
        for (MixerFile& mixer : mixers) {
            if (&mixer != primary) {
                waiting.push_back(&mixer);
            }
        }
        for (bool progress = true; progress && !waiting.empty();) {
            progress = false;
            for (auto it = waiting.begin(); it != waiting.end();) {
                const MixerFile& mixer = **it;
                const auto output = ref_at(mixer.controller->body, "m_OutputGroup");
                std::string send = "Master";
                if (output && !output->null()) {
                    const auto target = std::ranges::find_if(
                        mixers, [&](const MixerFile& other) { return other.guid == output->guid; });
                    const bool pending = target != mixers.end() &&
                                         std::ranges::find(waiting, &*target) != waiting.end();
                    if (pending) {
                        ++it;
                        continue;
                    }
                    if (const auto bus = target != mixers.end()
                                             ? bus_of.find({&*target, output->file_id})
                                             : bus_of.end();
                        bus != bus_of.end()) {
                        send = buses[bus->second].bus;
                    } else {
                        problems_.add("mixer output group", mixer.path,
                                      "it outputs to a group that didn't convert, so it sends to "
                                      "Master");
                    }
                }
                std::string name = mixer.name;
                for (int n = 2; !taken.insert(name).second; ++n) {
                    name = mixer.name + " " + std::to_string(n);
                }
                walk(mixer, master_of(mixer), send, name, {});
                it = waiting.erase(it);
                progress = true;
            }
        }
        for (const MixerFile* mixer : waiting) { // outputs that loop back
            std::string name = mixer->name;
            for (int n = 2; !taken.insert(name).second; ++n) {
                name = mixer->name + " " + std::to_string(n);
            }
            walk(*mixer, master_of(*mixer), "Master", name, {});
        }
        // Sends that drive a Duck Volume become its compressor's sidechain; others are reported.
        std::map<const Object*, std::vector<std::string>> sidechains;
        for (const GroupBus& bus : buses) {
            for (const Node& item : items_of(bus.group->body.get("m_Effects"))) {
                const auto ref = unity::object_ref(item);
                const Object* effect =
                    ref ? bus.mixer->find(ref->file_id, kAudioMixerEffect) : nullptr;
                if (effect == nullptr || effect->body.string_at("m_EffectName") != "Send" ||
                    bus.mixer->value(effect->body.string_at("m_MixLevel"), -80.0) <= -79.99) {
                    continue; // a send at -80 dB sends nothing
                }
                const auto target = ref_at(effect->body, "m_SendTarget");
                const Object* to =
                    target ? bus.mixer->find(target->file_id, kAudioMixerEffect) : nullptr;
                if (to != nullptr && to->body.string_at("m_EffectName") == "Duck Volume") {
                    sidechains[to].push_back(bus.bus);
                } else {
                    problems_.add("mixer send", bus.mixer->path + ": " + bus.bus,
                                  "each bus sends to one bus, so sends to a Receive don't "
                                  "convert");
                }
            }
        }
        json::Array list;
        for (const GroupBus& bus : buses) {
            const Node& body = bus.group->body;
            const std::string where = bus.mixer->path + ": " + bus.bus;
            json::Value out;
            out["name"] = bus.bus;
            const double volume = bus.mixer->value(body.string_at("m_Volume"), 0.0);
            if (volume != 0.0) {
                out["volume_db"] = tidy(std::clamp(volume, -80.0, 24.0));
            }
            if (std::abs(bus.mixer->value(body.string_at("m_Pitch"), 100.0) - 100.0) > 0.01) {
                problems_.add("mixer group pitch", where, "buses have no pitch");
            }
            if (body.flag_at("m_Solo")) {
                out["solo"] = true;
            }
            if (body.flag_at("m_Mute")) {
                out["mute"] = true;
            }
            if (body.flag_at("m_BypassEffects")) {
                out["bypass_effects"] = true;
            }
            if (!bus.send.empty() && bus.send != "Master") {
                out["send"] = bus.send;
            }
            bool after_fader = false;
            for (const Node& item : items_of(body.get("m_Effects"))) {
                const auto ref = unity::object_ref(item);
                const Object* effect =
                    ref ? bus.mixer->find(ref->file_id, kAudioMixerEffect) : nullptr;
                if (effect == nullptr) {
                    continue;
                }
                if (effect->body.string_at("m_EffectName") == "Attenuation") {
                    after_fader = true;
                    continue;
                }
                std::string why;
                const auto found = sidechains.find(effect);
                auto converted = mixer_effect(
                    *bus.mixer, effect->body, where,
                    found != sidechains.end() ? found->second : std::vector<std::string>{}, why);
                if (!converted) {
                    if (!why.empty()) {
                        problems_.add("mixer effect " + effect->body.string_at("m_EffectName"),
                                      where, why);
                    }
                    continue;
                }
                if (after_fader && volume != 0.0) {
                    problems_.add("effect after the fader", where,
                                  "a bus runs its effects before its volume");
                }
                out["effects"].push_back(std::move(*converted));
            }
            list.push_back(std::move(out));
        }
        json::Value doc;
        doc["format"] = 1;
        doc["buses"] = std::move(list);
        bus_layout_ =
            destination(without_extension(primary->path) + std::string(audio::kBusLayoutSuffix));
        keep_valid(doc, bus_layout_,
                   [](const json::Value& value) { return audio::validate_bus_layout(value); });
        add_file(bus_layout_, json::write(doc) + "\n");
        bus_groups_ = std::move(groups);
    }

    // --- Scenes and prefabs, with the prefabs they instance ---

    struct Component {
        std::int64_t id = 0;
        int class_id = 0;
        std::string type;
        Node body;
    };

    // What a UI object's layout group or scroll view sets on it.
    struct UiPlacement {
        bool placed = false;    // in a container
        bool vertical = false;  // a column's
        bool from_rect = false; // a scroll view's content: at least its rect's size
        bool hidden = false;    // a toggle's checkmark, while off
        std::string size_h = "fill";
        std::string size_v = "fill";
        std::array<double, 2> minimum{-1.0, -1.0}; // -1 for none
    };

    struct GameObject {
        std::int64_t id = 0;
        Node body;
        std::vector<Component> components;
        std::int64_t transform = 0; // its Transform's ID
        std::int64_t father = 0;    // its parent's Transform's ID
        int parent = -1;
        std::vector<int> children;
        int instance = -1;     // the instance that brought it, or -1 for the file's own
        std::string source_id; // brought: its entity ID in the instanced prefab's scene
        std::map<std::string, std::string> source_added; // and the entities made for it there
        std::string entity;                              // its entity ID in this file's scene
        std::map<std::string, std::string> added; // entities made for it, by what they are for
        bool removed = false;
        // A node of the model the object above it draws, by its name there: no entity of its own
        // unless something hangs on it, when it becomes one that follows the node (attached).
        std::string node;
        // UI: whether it becomes a control, the control that draws it when it is part of one,
        // its rect's size in pixels and its canvas's reference pixels per unit.
        bool ui = false;
        std::string folded;
        std::array<double, 2> rect{};
        double ui_ppu = 100.0;
        UiPlacement placement;
    };

    struct Expanded;

    // A model file instanced as a prefab: one object of the file's own, its top object's and
    // Transform's IDs in the model, and theirs here.
    struct ModelInstance {
        std::int64_t id = 0;
        std::string guid;
        std::int64_t root_object = 0;
        std::int64_t root_transform = 0;
        std::int64_t object = 0;
        std::int64_t transform = 0;
        // Its nodes' objects and Transforms: their IDs in the model, and theirs here.
        std::map<std::int64_t, std::int64_t> parts;
    };

    struct Instance {
        std::int64_t id = 0;
        const Expanded* prefab = nullptr;
        int root = -1;
        std::set<std::string> brings; // the prefab's entities other than its root, by their IDs
    };

    // A scene or prefab with the prefabs it instances expanded into it: every object, with the
    // IDs Unity gives an instance's objects.
    struct Expanded {
        std::string path;
        std::vector<GameObject> objects;
        std::vector<Instance> instances;
        std::vector<ModelInstance> models;
        std::map<std::int64_t, std::int64_t> aliases; // a stripped object's ID: what it stands for
        std::map<std::int64_t, int> by_id;            // GameObjects by ID
        std::map<std::int64_t, std::pair<int, int>> components; // by ID: object, component
        std::vector<int> roots;
        int root = -1;
        std::string scene;              // the converted scene
        std::set<std::string> entities; // every entity its scene holds
        std::set<std::string> used;     // the scene's own entity IDs
        bool finished = false;
        bool failed = false;
        bool three_d = false; // converted as a 3D scene
        Node render_settings; // a scene's RenderSettings: its sky, ambient light and fog

        [[nodiscard]] std::int64_t resolve(std::int64_t id) const {
            const auto it = aliases.find(id);
            return it != aliases.end() ? it->second : id;
        }
    };

    static std::int64_t remap(std::int64_t instance, std::int64_t id) {
        return id == 0 ? 0 : (instance ^ id) & kIdMask;
    }

    static std::optional<ObjectRef> ref_at(const Node& node, std::string_view dotted) {
        return unity::object_ref_at(node, dotted);
    }

    static bool is_own(const Object& object) {
        const auto instance = ref_at(object.body, "m_PrefabInstance");
        return !object.stripped && (!instance || instance->null());
    }

    // Converts a scene or prefab once, with the prefabs it instances first. Returns null if it
    // can't be read.
    Expanded* convert_file(const std::string& path, int depth) {
        if (const auto done = expanded_.find(path); done != expanded_.end()) {
            if (!done->second->finished) {
                problems_.add("prefab that instances itself", path);
                return nullptr;
            }
            return done->second->failed ? nullptr : done->second.get();
        }
        auto owned = std::make_unique<Expanded>();
        Expanded& file = *owned;
        expanded_[path] = std::move(owned);
        file.path = path;
        const auto fail = [&](const std::string& what, const std::string& why) {
            problems_.add(what, path, why);
            file.failed = true;
            file.finished = true;
            return nullptr;
        };
        if (depth > kMaxInstanceDepth) {
            return fail("prefabs nested too deep", "more than 16 levels of prefabs in prefabs");
        }
        auto text = platform::read_file(source_ + path);
        if (!text) {
            return fail("unreadable file", text.error().message);
        }
        auto objects = unity::parse_file(text.value());
        if (!objects) {
            return fail("unreadable file", objects.error().message);
        }
        for (const Object& object : objects.value()) {
            if (object.class_id == kPrefabInstance && object.type != "PrefabInstance") {
                return fail("prefab from before Unity 2018.3",
                            "open the project in Unity 2018.3 or later and save it there first");
            }
        }
        expand(file, objects.value(), depth);
        file.three_d = is_3d(file);
        if (extension_of(path) == ".prefab") {
            if (file.roots.empty()) {
                return fail("prefab with no objects", "");
            }
            file.root = file.roots.front();
        }
        assign_entities(file);
        write_scene(file);
        file.finished = true;
        return &file;
    }

    void expand(Expanded& file, const std::vector<Object>& objects, int depth) {
        for (const Object& object : objects) {
            if (object.class_id == kGameObject && is_own(object)) {
                file.by_id[object.file_id] = static_cast<int>(file.objects.size());
                GameObject go;
                go.id = object.file_id;
                go.body = object.body;
                file.objects.push_back(std::move(go));
            }
        }
        for (const Object& object : objects) {
            if (object.class_id == kPrefabInstance) {
                instantiate(file, object, depth);
            }
        }
        // A stripped object stands for an object an instance brought.
        std::set<std::int64_t> fathers; // the Transforms the file's own objects are under
        for (const Object& object : objects) {
            if (object.class_id == kTransform && is_own(object)) {
                fathers.insert(object.body.integer_at("m_Father.fileID", 0));
            }
        }
        for (const Object& object : objects) {
            const auto instance = ref_at(object.body, "m_PrefabInstance");
            const auto source = ref_at(object.body, "m_CorrespondingSourceObject");
            if (!object.stripped || !instance || !source) {
                continue;
            }
            for (const Instance& known : file.instances) {
                if (known.id == instance->file_id) {
                    file.aliases[object.file_id] =
                        remap(known.id, known.prefab->resolve(source->file_id));
                }
            }
            for (const ModelInstance& model : file.models) {
                if (model.id != instance->file_id) {
                    continue;
                }
                // A node's object or Transform stands for its own; what is under one hangs on
                // the node. One the model hasn't stands for the model's top object.
                if (const auto part = model.parts.find(source->file_id);
                    part != model.parts.end()) {
                    file.aliases[object.file_id] = part->second;
                } else if (source->file_id == model.root_object || object.class_id == kGameObject) {
                    file.aliases[object.file_id] = model.object;
                } else if (source->file_id == model.root_transform ||
                           object.class_id == kTransform) {
                    file.aliases[object.file_id] = model.transform;
                    if (source->file_id != model.root_transform &&
                        fathers.contains(object.file_id)) {
                        problems_.add("object under a node the model hasn't",
                                      file.path + ": instance of a model",
                                      "it goes under the model's top object");
                    }
                }
            }
        }
        std::vector<std::int64_t> root_order;
        for (const Object& object : objects) {
            if (object.type == "RenderSettings") {
                file.render_settings = object.body;
                continue;
            }
            if (object.type == "SceneRoots") {
                if (const Node* roots = object.body.get("m_Roots")) {
                    for (const Node& root : roots->items) {
                        if (const auto ref = unity::object_ref(root)) {
                            root_order.push_back(ref->file_id);
                        }
                    }
                }
                continue;
            }
            if (!is_own(object) || object.class_id == kGameObject ||
                object.class_id == kPrefabInstance) {
                continue;
            }
            const auto owner = ref_at(object.body, "m_GameObject");
            if (!owner || owner->null()) {
                continue; // a scene's settings
            }
            const auto at = file.by_id.find(file.resolve(owner->file_id));
            if (at == file.by_id.end()) {
                problems_.add("component of a missing object", file.path + ": " + object.type);
                continue;
            }
            file.objects[static_cast<std::size_t>(at->second)].components.push_back(
                Component{.id = object.file_id,
                          .class_id = object.class_id,
                          .type = object.type,
                          .body = object.body});
        }
        // References to stripped objects name what they stand for.
        const auto resolve = [&](std::int64_t id) { return file.resolve(id); };
        for (GameObject& go : file.objects) {
            for_each_local_ref(go.body, resolve);
            for (Component& component : go.components) {
                for_each_local_ref(component.body, resolve);
            }
        }
        // Roots name a stripped Transform or, since Unity 2022, an instance itself.
        for (std::int64_t& id : root_order) {
            id = file.resolve(id);
            for (const Instance& instance : file.instances) {
                if (instance.id == id) {
                    id = file.objects[static_cast<std::size_t>(instance.root)].transform;
                }
            }
            for (const ModelInstance& model : file.models) {
                if (model.id == id) {
                    id = model.transform;
                }
            }
        }
        build_hierarchy(file, root_order);
        fold_models(file);
    }

    void instantiate(Expanded& file, const Object& object, int depth) {
        const std::string where = file.path + ": prefab instance";
        const auto source = ref_at(object.body, "m_SourcePrefab");
        const Asset* asset = source ? asset_of(source->guid) : nullptr;
        if (asset == nullptr) {
            problems_.add("instance of a missing prefab", where,
                          "its prefab isn't in the project's Assets folder");
            return;
        }
        if (is_model_file(extension_of(asset->path))) {
            instantiate_model(file, object, *asset);
            return;
        }
        if (extension_of(asset->path) != ".prefab") {
            const std::string why = unsupported_asset(extension_of(asset->path));
            problems_.add("instance of " + extension_of(asset->path), where + " of " + asset->path,
                          why.empty() ? "only prefabs can be instanced" : why);
            return;
        }
        Expanded* prefab = convert_file(asset->path, depth + 1);
        if (prefab == nullptr || prefab->root < 0) {
            problems_.add("instance of a prefab that didn't convert", where, asset->path);
            return;
        }
        const std::int64_t id = object.file_id;
        const int base = static_cast<int>(file.objects.size());
        const int instance_index = static_cast<int>(file.instances.size());
        const auto change = [&](std::int64_t local) { return remap(id, local); };
        std::map<std::int64_t, Node*> bodies;
        for (const GameObject& go : prefab->objects) {
            GameObject copy = go;
            copy.id = remap(id, go.id);
            copy.transform = remap(id, go.transform);
            copy.parent = go.parent >= 0 ? go.parent + base : -1;
            for (int& child : copy.children) {
                child += base;
            }
            copy.instance = instance_index;
            copy.source_id = go.entity;
            copy.source_added = go.added;
            copy.entity.clear();
            copy.added.clear();
            for_each_local_ref(copy.body, change);
            for (Component& component : copy.components) {
                component.id = remap(id, component.id);
                for_each_local_ref(component.body, change);
            }
            file.by_id[copy.id] = static_cast<int>(file.objects.size());
            file.objects.push_back(std::move(copy));
        }
        for (auto i = static_cast<std::size_t>(base); i < file.objects.size(); ++i) {
            bodies[file.objects[i].id] = &file.objects[i].body;
            for (Component& component : file.objects[i].components) {
                bodies[component.id] = &component.body;
            }
        }
        Instance instance;
        instance.id = id;
        instance.prefab = prefab;
        instance.root = base + prefab->root;
        const std::string& source_root =
            prefab->objects[static_cast<std::size_t>(prefab->root)].entity;
        for (const std::string& entity : prefab->entities) {
            if (entity != source_root) {
                instance.brings.insert(entity);
            }
        }
        file.instances.push_back(std::move(instance));
        // The instance's root goes under the transform the instance names, which its Transform
        // then says, so files that instance this one keep it there.
        GameObject& root =
            file.objects[static_cast<std::size_t>(base) + static_cast<std::size_t>(prefab->root)];
        const auto parent = ref_at(object.body, "m_Modification.m_TransformParent");
        ObjectRef father;
        father.file_id = parent ? parent->file_id : 0;
        for (Component& component : root.components) {
            if (Node* field = component.body.get("m_Father");
                field != nullptr && component.id == root.transform) {
                *field = ref_node(father);
            }
        }
        const Node* modifications = object.body.path("m_Modification.m_Modifications");
        for (const Node& modification :
             modifications != nullptr ? modifications->items : std::vector<Node>{}) {
            const auto target = ref_at(modification, "target");
            if (!target) {
                continue;
            }
            const auto body = bodies.find(remap(id, prefab->resolve(target->file_id)));
            if (body == bodies.end()) {
                problems_.add("override of a missing object", where);
                continue;
            }
            set_property(*body->second, modification.string_at("propertyPath"),
                         modification.string_at("value"), ref_at(modification, "objectReference"));
        }
        if (const Node* removed = object.body.path("m_Modification.m_RemovedComponents")) {
            for (const Node& item : removed->items) {
                const auto ref = unity::object_ref(item);
                if (!ref) {
                    continue;
                }
                const std::int64_t component = remap(id, prefab->resolve(ref->file_id));
                for (auto i = static_cast<std::size_t>(base); i < file.objects.size(); ++i) {
                    std::erase_if(file.objects[i].components,
                                  [&](const Component& c) { return c.id == component; });
                }
            }
        }
        if (const Node* removed = object.body.path("m_Modification.m_RemovedGameObjects")) {
            for (const Node& item : removed->items) {
                const auto ref = unity::object_ref(item);
                if (!ref) {
                    continue;
                }
                if (const auto at = file.by_id.find(remap(id, prefab->resolve(ref->file_id)));
                    at != file.by_id.end()) {
                    mark_removed(file, at->second);
                }
            }
        }
    }

    // A model file instanced as a prefab becomes one object of the file's own, with a Transform
    // and a ModelInstance naming the model, which a MeshInstance3D draws whole. Each of the
    // model's nodes gets an object too, with the Transform Unity gives it, so overrides and
    // objects under it find it: it poses the model's node, and becomes an entity only when
    // something hangs on it.
    void instantiate_model(Expanded& file, const Object& object, const Asset& asset) {
        const std::string where = file.path + ": instance of " + asset.path;
        ModelInstance model;
        model.id = object.file_id;
        model.guid = asset.meta.string_at("guid");
        const Node* modifications = object.body.path("m_Modification.m_Modifications");
        const std::vector<Node> none;
        const std::vector<Node>& changes = modifications != nullptr ? modifications->items : none;
        // Its top object and Transform: the IDs Unity gives them, else the ones overrides name.
        std::set<std::int64_t> objects;
        std::set<std::int64_t> transforms;
        for (const Node& change : changes) {
            const auto target = ref_at(change, "target");
            if (!target || target->guid != model.guid) {
                continue;
            }
            const std::string path = change.string_at("propertyPath");
            if (path.starts_with("m_Local") || path == "m_RootOrder") {
                transforms.insert(target->file_id);
            } else if (path == "m_Name" || path == "m_IsActive" || path == "m_Layer" ||
                       path == "m_TagString") {
                objects.insert(target->file_id);
            }
        }
        const auto root = [&](const std::array<std::int64_t, 2>& known,
                              const std::set<std::int64_t>& named, std::string_view type) {
            for (const std::int64_t id : known) {
                if (named.contains(id)) {
                    return id;
                }
            }
            // A model's meta names its objects' IDs, its top one "//RootNode".
            if (const Node* table = asset.meta.path("ModelImporter.internalIDToNameTable")) {
                for (const Node& entry : table->items) {
                    const Node* first = entry.get("first");
                    if (entry.string_at("second") != "//RootNode" || first == nullptr) {
                        continue;
                    }
                    if (const Node* id = first->get(type); id != nullptr && id->integer()) {
                        return *id->integer();
                    }
                }
            }
            return named.size() == 1 ? *named.begin() : known.front();
        };
        model.root_object = root(kModelRootObjects, objects, "1");
        model.root_transform = root(kModelRootTransforms, transforms, "4");
        std::set<std::int64_t> animators(kModelRootAnimators.begin(), kModelRootAnimators.end());
        animators.insert(root(kModelRootAnimators, {}, "95"));
        // The Animator a scene gives the model's top object; no type for none.
        Component animator;
        model.object = remap(model.id, model.root_object);
        model.transform = remap(model.id, model.root_transform);

        GameObject go;
        go.id = model.object;
        go.body = map_node(
            {{"m_Name", scalar_node(file_stem(asset.path))}, {"m_IsActive", scalar_node("1")}});
        Component transform{.id = model.transform,
                            .class_id = kTransform,
                            .type = "Transform",
                            .body = map_node({{"m_Children", seq_node()}})};
        Component instance{.id = remap(model.id, kModelComponent),
                           .class_id = kModelInstance,
                           .type = "ModelInstance",
                           .body = map_node({{"path", scalar_node(asset.path)},
                                             {"guid", scalar_node(model.guid)}})};
        ObjectRef father;
        if (const auto parent = ref_at(object.body, "m_Modification.m_TransformParent")) {
            father.file_id = parent->file_id;
        }
        // The model's nodes, under the top object; a top node Unity makes the top object places
        // it.
        const scene3d::Model* loaded =
            extension_of(asset.path) != ".blend" ? model_file(asset, where) : nullptr;
        std::vector<GameObject> parts;
        std::map<std::int64_t, Node*> bodies; // by ID in the model
        if (loaded != nullptr) {
            const double scale = asset.meta.number_at("ModelImporter.meshes.globalScale", 1.0);
            const int top = collapsed_top(asset, *loaded);
            if (top >= 0) {
                transform.body = unity_place(loaded->nodes[static_cast<std::size_t>(top)], scale);
                set_node(transform.body, "m_Children", seq_node());
            }
            const NodeIds ids = node_ids(asset, *loaded);
            parts.reserve(loaded->nodes.size());
            for (std::size_t i = 0; i < loaded->nodes.size(); ++i) {
                const scene3d::ModelNode& node = loaded->nodes[i];
                if (static_cast<int>(i) == top) {
                    continue;
                }
                GameObject part;
                part.id = remap(model.id, ids.objects[i]);
                part.node = node.name;
                part.body = map_node({{"m_Name", scalar_node(file_name_of(node))},
                                      {"m_IsActive", scalar_node("1")}});
                Component place{.id = remap(model.id, ids.transforms[i]),
                                .class_id = kTransform,
                                .type = "Transform",
                                .body = unity_place(node, scale)};
                ObjectRef up;
                up.file_id =
                    node.parent < 0 || node.parent == top
                        ? model.transform
                        : remap(model.id, ids.transforms[static_cast<std::size_t>(node.parent)]);
                set_node(place.body, "m_Father", ref_node(up));
                part.components.push_back(std::move(place));
                model.parts[ids.objects[i]] = part.id;
                model.parts[ids.transforms[i]] = part.components.front().id;
                parts.push_back(std::move(part));
                bodies[ids.objects[i]] = &parts.back().body;
                bodies[ids.transforms[i]] = &parts.back().components.front().body;
            }
        }
        set_node(transform.body, "m_Father", ref_node(father));
        Node materials = seq_node();
        for (const Node& change : changes) {
            const auto target = ref_at(change, "target");
            if (!target) {
                continue;
            }
            const std::string path = change.string_at("propertyPath");
            const auto reference = ref_at(change, "objectReference");
            if (path.starts_with("m_Materials.Array.data[")) {
                if (reference && !reference->null()) {
                    materials.items.push_back(ref_node(*reference));
                }
            } else if (target->file_id == model.root_object) {
                set_property(go.body, path, change.string_at("value"), reference);
            } else if (target->file_id == model.root_transform) {
                set_property(transform.body, path, change.string_at("value"), reference);
            } else if (animators.contains(target->file_id)) {
                // Its Animator, as a scene sets its controller.
                if (animator.type.empty()) {
                    animator = Component{.id = remap(model.id, target->file_id),
                                         .class_id = kAnimator,
                                         .type = "Animator",
                                         .body = map_node({{"m_Enabled", scalar_node("1")}})};
                }
                set_property(animator.body, path, change.string_at("value"), reference);
            } else if (const auto part = bodies.find(target->file_id); part != bodies.end()) {
                set_property(*part->second, path, change.string_at("value"), reference);
            } else {
                problems_.add("change to a model's inner object", where,
                              "a model draws as its file has it; " + path + " isn't changed");
            }
        }
        // Materials the model's import settings put in place of its own.
        add_external_materials(asset, materials);
        set_node(instance.body, "m_Materials", std::move(materials));
        if (object.body.path("m_Modification.m_RemovedComponents") != nullptr &&
            !object.body.path("m_Modification.m_RemovedComponents")->items.empty()) {
            problems_.add("component removed from a model's inner object", where,
                          "a model draws as its file has it");
        }
        go.components.push_back(std::move(transform));
        go.components.push_back(std::move(instance));
        if (!animator.type.empty()) {
            go.components.push_back(std::move(animator));
        }
        file.by_id[go.id] = static_cast<int>(file.objects.size());
        file.objects.push_back(std::move(go));
        for (GameObject& part : parts) {
            file.by_id[part.id] = static_cast<int>(file.objects.size());
            file.objects.push_back(std::move(part));
        }
        file.models.push_back(std::move(model));
    }

    // Materials a model's import settings put in place of its own.
    static void add_external_materials(const Asset& asset, Node& materials) {
        if (const Node* remapped = asset.meta.path("ModelImporter.externalObjects")) {
            for (const Node& entry : remapped->items) {
                const auto reference = ref_at(entry, "second");
                if (entry.string_at("first.type") == "UnityEngine:Material" && reference &&
                    !reference->null()) {
                    materials.items.push_back(ref_node(*reference));
                }
            }
        }
    }

    // A node's name in its model's file, by which Unity names its object.
    static const std::string& file_name_of(const scene3d::ModelNode& node) {
        return node.file_name.empty() ? node.name : node.file_name;
    }

    // A node's place in its parent as Unity has it, in a Transform's fields: mirrored in x, and
    // moved as far as the model's import scale says.
    static Node unity_place(const scene3d::ModelNode& node, double scale) {
        const auto number = [](double value) {
            return scalar_node(json::write(json::Value(tidy_number(value)), json::Style::Compact));
        };
        const auto xyz = [&](double x, double y, double z) {
            return map_node({{"x", number(x)}, {"y", number(y)}, {"z", number(z)}});
        };
        const Vec3& p = node.position;
        const Quat& q = node.rotation;
        Node rotation = xyz(q.x, -q.y, -q.z);
        rotation.entries.push_back({"w", number(q.w)});
        return map_node({{"m_LocalPosition", xyz(-p.x * scale, p.y * scale, p.z * scale)},
                         {"m_LocalRotation", std::move(rotation)},
                         {"m_LocalScale", xyz(node.scale.x, node.scale.y, node.scale.z)}});
    }

    // The node Unity makes a model's top object: its one top node, unless its import settings
    // preserve its hierarchy; -1 when the top object is Unity's own.
    static int collapsed_top(const Asset& asset, const scene3d::Model& model) {
        if (asset.meta.flag_at("ModelImporter.meshes.preserveHierarchy", false)) {
            return -1;
        }
        int top = -1;
        for (std::size_t i = 0; i < model.nodes.size(); ++i) {
            if (model.nodes[i].parent < 0) {
                if (top >= 0) {
                    return -1;
                }
                top = static_cast<int>(i);
            }
        }
        return top;
    }

    // The IDs Unity gives each node's object and Transform in a model: those the import settings
    // name (fileIDToRecycleName before Unity 2019.3, internalIDToNameTable since), else the hash
    // of the node's name, counting nodes of the same name, as for its meshes.
    struct NodeIds {
        std::vector<std::int64_t> objects;
        std::vector<std::int64_t> transforms;
    };

    static NodeIds node_ids(const Asset& asset, const scene3d::Model& model) {
        NodeIds ids;
        const std::size_t count = model.nodes.size();
        ids.objects.resize(count);
        ids.transforms.resize(count);
        std::map<std::string, int, std::less<>> seen;
        for (std::size_t i = 0; i < count; ++i) {
            const std::string& name = file_name_of(model.nodes[i]);
            const int n = seen[name]++;
            ids.objects[i] = import_id("GameObject", name, n);
            ids.transforms[i] = import_id("Transform", name, n);
        }
        // An ID the settings give a name goes to the first node of that name not given one yet.
        std::set<std::size_t> objects_named;
        std::set<std::size_t> transforms_named;
        const auto name_id = [&](int class_id, std::string_view name, std::int64_t id) {
            std::vector<std::int64_t>& out = class_id == kGameObject ? ids.objects : ids.transforms;
            std::set<std::size_t>& named =
                class_id == kGameObject ? objects_named : transforms_named;
            for (std::size_t i = 0; i < count; ++i) {
                if (!named.contains(i) && file_name_of(model.nodes[i]) == name) {
                    out[i] = id;
                    named.insert(i);
                    return;
                }
            }
        };
        if (const Node* table = asset.meta.path("ModelImporter.fileIDToRecycleName")) {
            for (const auto& [key, name] : table->entries) {
                const auto id = parse_int(key);
                if (id && (*id / 100000 == kGameObject || *id / 100000 == kTransform)) {
                    name_id(static_cast<int>(*id / 100000), name.string(), *id);
                }
            }
        }
        if (const Node* table = asset.meta.path("ModelImporter.internalIDToNameTable")) {
            for (const Node& entry : table->items) {
                const Node* first = entry.get("first");
                for (const int class_id : {kGameObject, kTransform}) {
                    const Node* id =
                        first != nullptr ? first->get(std::to_string(class_id)) : nullptr;
                    const std::optional<std::int64_t> value =
                        id != nullptr ? id->integer() : std::nullopt;
                    if (value) {
                        name_id(class_id, entry.string_at("second"), *value);
                    }
                }
            }
        }
        return ids;
    }

    // Whether an object is named as a model's node is.
    static bool names_node(const GameObject& go, const scene3d::ModelNode& node) {
        return go.body.string_at("m_Name") == file_name_of(node);
    }

    // Unpacked instances of models: objects named as a model's nodes, one with a
    // SkinnedMeshRenderer drawing one of its meshes. Each folds back into an instance of the
    // model, which draws whole: its top object gets a ModelInstance and the others become its
    // nodes, posing them where they are.
    void fold_models(Expanded& file) {
        for (std::size_t i = 0; i < file.objects.size(); ++i) {
            const GameObject& go = file.objects[i];
            if (go.instance >= 0 || go.removed || !go.node.empty()) {
                continue;
            }
            ObjectRef mesh;
            for (const Component& component : go.components) {
                if (component.class_id == kSkinnedMeshRenderer) {
                    mesh = ref_at(component.body, "m_Mesh").value_or(ObjectRef{});
                    break;
                }
            }
            const Asset* asset = !mesh.null() && !mesh.guid.empty() ? asset_of(mesh.guid) : nullptr;
            if (asset != nullptr && is_model_file(extension_of(asset->path)) &&
                extension_of(asset->path) != ".blend") {
                fold_model(file, static_cast<int>(i), *asset, mesh.file_id);
            }
        }
    }

    void fold_model(Expanded& file, int index, const Asset& asset, std::int64_t mesh_id) {
        const std::string where = file.path + ": " + object_path(file, index);
        const scene3d::Model* model = model_file(asset, where);
        if (model == nullptr) {
            return;
        }
        const std::optional<std::size_t> mesh = model_mesh(asset, *model, mesh_id);
        const auto part = std::ranges::find_if(model->parts, [&](const scene3d::ModelPart& p) {
            return mesh && p.mesh == *mesh && p.node_index >= 0;
        });
        if (part == model->parts.end()) {
            problems_.add("skinned mesh from a model", where,
                          asset.path + " has no mesh with the ID " + std::to_string(mesh_id));
            return;
        }
        // Up from the object as many objects as its node is deep: the instance's top object.
        const auto node_at = [&](int n) -> const scene3d::ModelNode& {
            return model->nodes[static_cast<std::size_t>(n)];
        };
        const int top = collapsed_top(asset, *model);
        int at = index;
        for (int n = part->node_index; n != top; n = node_at(n).parent) {
            if (at < 0 || !names_node(file.objects[static_cast<std::size_t>(at)], node_at(n))) {
                problems_.add("skinned mesh whose objects aren't its model's", where,
                              "the objects above it aren't named as " + asset.path +
                                  "'s nodes are, so it isn't drawn; instance the model");
                return;
            }
            at = file.objects[static_cast<std::size_t>(at)].parent;
            if (node_at(n).parent < 0) {
                break;
            }
        }
        if (at < 0) {
            problems_.add("skinned mesh whose objects aren't its model's", where,
                          "its model's top object isn't there, so it isn't drawn; instance the "
                          "model");
            return;
        }
        GameObject& root = file.objects[static_cast<std::size_t>(at)];
        if (!root.node.empty() || root.instance >= 0 || !go_model_guid(root).empty()) {
            return;
        }
        // The objects under it named as the nodes under its node, with the materials their
        // renderers draw in place of the model's own.
        const std::string guid = asset.meta.string_at("guid");
        Node materials = seq_node();
        std::vector<bool> found(model->nodes.size());
        const std::function<void(int, int)> match = [&](int object, int node) {
            for (const int child : file.objects[static_cast<std::size_t>(object)].children) {
                GameObject& c = file.objects[static_cast<std::size_t>(child)];
                if (c.removed || c.instance >= 0) {
                    continue;
                }
                for (std::size_t k = 0; k < model->nodes.size(); ++k) {
                    if (found[k] || static_cast<int>(k) == top || model->nodes[k].parent != node ||
                        !names_node(c, model->nodes[k])) {
                        continue;
                    }
                    found[k] = true;
                    c.node = model->nodes[k].name;
                    for (const Component& component : c.components) {
                        const Node* used = component.class_id == kSkinnedMeshRenderer ||
                                                   component.class_id == kMeshRenderer
                                               ? component.body.get("m_Materials")
                                               : nullptr;
                        for (const Node& item :
                             used != nullptr ? used->items : std::vector<Node>{}) {
                            if (const auto ref = unity::object_ref(item);
                                ref && !ref->null() && !ref->guid.empty() && ref->guid != guid) {
                                materials.items.push_back(ref_node(*ref));
                            }
                        }
                    }
                    match(child, static_cast<int>(k));
                    break;
                }
            }
        };
        match(at, top);
        for (std::size_t k = 0; k < model->nodes.size(); ++k) {
            if (!found[k] && static_cast<int>(k) != top) {
                problems_.add("node of a model the scene removed", where,
                              model->nodes[k].name + " still draws, since the model draws whole");
            }
        }
        add_external_materials(asset, materials);
        root.components.push_back(
            Component{.id = remap(root.id, kModelComponent),
                      .class_id = kModelInstance,
                      .type = "ModelInstance",
                      .body = map_node({{"path", scalar_node(asset.path)},
                                        {"guid", scalar_node(guid)},
                                        {"m_Materials", std::move(materials)}})});
    }

    // Whether a model's node has something on it, so that it becomes an entity following the
    // node: a component the model doesn't make, or an object under it that isn't the model's.
    static bool attached(const Expanded& file, int index) {
        const GameObject& go = file.objects[static_cast<std::size_t>(index)];
        if (go.node.empty() || go.removed) {
            return false;
        }
        return std::ranges::any_of(go.components,
                                   [](const Component& c) { return !model_made(c.class_id); }) ||
               std::ranges::any_of(go.children, [&](int child) {
                   const GameObject& c = file.objects[static_cast<std::size_t>(child)];
                   return c.node.empty() && !c.removed;
               });
    }

    // The components Unity's model importer gives a model's nodes, which draw with the model.
    static bool model_made(int class_id) {
        return class_id == kTransform || class_id == kMeshFilter || class_id == kMeshRenderer ||
               class_id == kSkinnedMeshRenderer;
    }

    static void mark_removed(Expanded& file, int index) {
        GameObject& go = file.objects[static_cast<std::size_t>(index)];
        go.removed = true;
        for (const int child : go.children) {
            mark_removed(file, child);
        }
    }

    // Each object's parent and children from their Transforms, and the file's top objects.
    void build_hierarchy(Expanded& file, const std::vector<std::int64_t>& root_order) {
        std::map<std::int64_t, int> by_transform;
        for (std::size_t i = 0; i < file.objects.size(); ++i) {
            GameObject& go = file.objects[i];
            for (const Component& component : go.components) {
                if (component.class_id == kTransform || component.class_id == kRectTransform) {
                    go.transform = component.id;
                    go.father = component.body.integer_at("m_Father.fileID", 0);
                    break;
                }
            }
            go.father = file.resolve(go.father);
            by_transform[go.transform] = static_cast<int>(i);
        }
        file.components.clear();
        for (std::size_t i = 0; i < file.objects.size(); ++i) {
            GameObject& go = file.objects[i];
            go.children.clear();
            go.parent = -1;
            for (std::size_t c = 0; c < go.components.size(); ++c) {
                file.components[go.components[c].id] = {static_cast<int>(i), static_cast<int>(c)};
            }
        }
        std::vector<int> tops;
        for (std::size_t i = 0; i < file.objects.size(); ++i) {
            GameObject& go = file.objects[i];
            const auto parent = by_transform.find(go.father);
            if (go.father != 0 && parent != by_transform.end() &&
                parent->second != static_cast<int>(i)) {
                go.parent = parent->second;
                file.objects[static_cast<std::size_t>(parent->second)].children.push_back(
                    static_cast<int>(i));
            } else {
                if (go.father != 0) {
                    problems_.add("object whose parent is missing",
                                  file.path + ": " + go.body.string_at("m_Name"));
                }
                tops.push_back(static_cast<int>(i));
            }
        }
        // Children in the order their parent's Transform lists them.
        for (GameObject& go : file.objects) {
            const Component* transform = transform_of(go);
            if (transform == nullptr) {
                continue;
            }
            std::vector<std::int64_t> listed;
            if (const Node* children = transform->body.get("m_Children")) {
                for (const Node& child : children->items) {
                    listed.push_back(file.resolve(child.integer_at("fileID", 0)));
                }
            }
            const auto position = [&](int child) {
                const auto it = std::ranges::find(
                    listed, file.objects[static_cast<std::size_t>(child)].transform);
                return std::distance(listed.begin(), it);
            };
            std::ranges::stable_sort(go.children,
                                     [&](int a, int b) { return position(a) < position(b); });
        }
        const auto order = [&](int index) -> std::int64_t {
            const GameObject& go = file.objects[static_cast<std::size_t>(index)];
            if (!root_order.empty()) {
                const auto it = std::ranges::find(root_order, go.transform);
                return std::distance(root_order.begin(), it);
            }
            const Component* transform = transform_of(go);
            return transform != nullptr ? transform->body.integer_at("m_RootOrder", 0) : 0;
        };
        std::ranges::stable_sort(tops, [&](int a, int b) { return order(a) < order(b); });
        file.roots.clear();
        for (const int top : tops) {
            if (!file.objects[static_cast<std::size_t>(top)].removed) {
                file.roots.push_back(top);
            }
        }
        // Removed objects still get entity IDs, to write their nulls.
        for (const int top : tops) {
            if (file.objects[static_cast<std::size_t>(top)].removed) {
                file.roots.push_back(top);
            }
        }
    }

    static const Component* transform_of(const GameObject& go) {
        for (const Component& component : go.components) {
            if (component.id == go.transform) {
                return &component;
            }
        }
        return nullptr;
    }

    static std::string take(std::set<std::string>& used, std::string id) {
        if (id.size() > 64) {
            id.resize(64);
        }
        if (used.insert(id).second) {
            return id;
        }
        for (int n = 2;; ++n) {
            std::string candidate = id.substr(0, 60) + "_" + std::to_string(n);
            if (used.insert(candidate).second) {
                return candidate;
            }
        }
    }

    void assign_entities(Expanded& file) {
        std::function<void(int)> visit = [&](int index) {
            GameObject& go = file.objects[static_cast<std::size_t>(index)];
            const Instance* instance =
                go.instance >= 0 ? &file.instances[static_cast<std::size_t>(go.instance)] : nullptr;
            if (!go.node.empty() && !attached(file, index)) {
                go.entity.clear(); // a model's node, which its model draws
            } else if (instance != nullptr && instance->root != index && !go.source_id.empty()) {
                go.entity = file.objects[static_cast<std::size_t>(instance->root)].entity + "/" +
                            go.source_id;
            } else {
                const std::string base = snake_case(go.body.string_at("m_Name"));
                int up = go.parent; // the nearest object above it with an entity
                while (up >= 0 && file.objects[static_cast<std::size_t>(up)].entity.empty()) {
                    up = file.objects[static_cast<std::size_t>(up)].parent;
                }
                if (!file.used.contains(base) || up < 0) {
                    go.entity = take(file.used, base);
                } else {
                    const std::string& parent = file.objects[static_cast<std::size_t>(up)].entity;
                    go.entity = take(file.used, snake_case(parent) + "_" + base);
                }
            }
            for (const int child : go.children) {
                visit(child);
            }
        };
        for (const int root : file.roots) {
            visit(root);
        }
    }

    // The path of names from the top, for reports.
    static std::string object_path(const Expanded& file, int index) {
        std::vector<std::string> names;
        for (int at = index; at >= 0; at = file.objects[static_cast<std::size_t>(at)].parent) {
            names.push_back(file.objects[static_cast<std::size_t>(at)].body.string_at("m_Name"));
            if (names.size() > 256) {
                break;
            }
        }
        std::ranges::reverse(names);
        return join(names, "/");
    }

    static bool active(const Expanded& file, int index) {
        for (int at = index; at >= 0; at = file.objects[static_cast<std::size_t>(at)].parent) {
            if (!file.objects[static_cast<std::size_t>(at)].body.flag_at("m_IsActive", true)) {
                return false;
            }
        }
        return true;
    }

    // --- Converting objects ---

    // A 3D object's place in its parent's space, in Godot's axes: Unity's mirrored in x.
    struct Place3D {
        std::array<double, 3> position{};
        Quaternion rotation{0.0, 0.0, 0.0, 1.0};
        std::array<double, 3> scale{1.0, 1.0, 1.0};
        bool visible = true;
    };

    // What one object becomes: its components, and entities made for it by what they are for.
    // An Animator converts once every object has its entity, since its tracks name objects
    // under it.
    struct Converted {
        json::Object components;
        std::vector<std::pair<std::string, json::Object>> added;
        const Component* animator = nullptr;
        std::string children_to; // the added entity that holds the object's children, if one
        Vec2 child_shift;        // moves the object's children by minus this
        bool collisions = false; // a script takes collision messages, so its body reports them
        // 3D: its place, written once its parent is known to be turned or not, and whether it is
        // turned half a turn about y, as a camera, light or quad is.
        std::optional<Place3D> place;
        bool turned = false;
    };

    struct Context {
        Expanded* file = nullptr;
        int index = -1;
        std::string where;
        bool active = true;
    };

    Converted convert_object(const Context& ctx) {
        Converted out;
        const GameObject& go = ctx.file->objects[static_cast<std::size_t>(ctx.index)];
        if (!ctx.active) {
            problems_.add("inactive object", ctx.where,
                          "converted hidden, with its shapes disabled; there is no inactive "
                          "state, so its other components still run");
        }
        if (go.ui) {
            convert_ui_object(ctx, out);
            return out;
        }
        if (ctx.file->three_d) {
            convert_object_3d(ctx, out);
            return out;
        }
        if (const Component* transform = transform_of(go)) {
            out.components["Transform2D"] = transform_2d(transform->body, ctx.where);
        } else {
            out.components["Transform2D"] = json::Object{};
        }
        std::vector<const Component*> colliders;
        const Component* rigidbody = nullptr;
        const Component* effector = nullptr;
        const Component* composite = nullptr;
        std::vector<const Component*> found_joints;
        TileMapParts tiles;
        for (const Component& component : go.components) {
            switch (component.class_id) {
            case kTransform:
            case kRectTransform:
            case kGuiLayer:
            case kFlareLayer:
                break;
            case kSpriteRenderer:
                sprite_renderer(component, ctx, out);
                break;
            case kCamera:
                camera(component, ctx, go, out);
                break;
            case kAudioListener: {
                json::Value listener;
                listener["current"] = component.body.flag_at("m_Enabled", true) && ctx.active;
                out.components["AudioListener2D"] = std::move(listener);
                break;
            }
            case kAudioSource:
                audio_source(component, ctx, out);
                break;
            case kRigidbody2D:
                rigidbody = &component;
                break;
            case kBoxCollider2D:
            case kCircleCollider2D:
            case kCapsuleCollider2D:
            case kPolygonCollider2D:
            case kEdgeCollider2D:
                colliders.push_back(&component);
                break;
            case kCompositeCollider2D:
                composite = &component;
                break;
            case kHingeJoint2D:
            case kSpringJoint2D:
            case kSliderJoint2D:
                found_joints.push_back(&component);
                break;
            case kGrid:
                break; // read by the tile maps under it
            case kTilemap:
                tiles.tilemap = &component;
                break;
            case kTilemapRenderer:
                tiles.renderer = &component;
                break;
            case kTilemapCollider2D:
                tiles.collider = &component;
                break;
            case kPlatformEffector2D:
                effector = &component;
                break;
            case kMonoBehaviour: {
                // A UI component here is outside a Canvas, which mono_behaviour reports.
                if (const KnownScript* lost = lost_script(component);
                    lost != nullptr && !lost->ui) {
                    problems_.add("component " + std::string(lost->name), ctx.where,
                                  std::string(kLostWhy));
                    break;
                }
                const KnownScript* known = known_script(component);
                const std::string_view name = known != nullptr ? known->name : std::string_view();
                if (name == "Light2D") {
                    light_2d(component, ctx, out);
                } else if (name == "ShadowCaster2D") {
                    shadow_caster(component, ctx, out);
                } else if (name != "CompositeShadowCaster2D") {
                    // A composite caster groups the casters under it, which cast on their own.
                    mono_behaviour(component, ctx, out);
                }
                break;
            }
            case kAnimator:
                out.animator = &component;
                break;
            case kModelInstance:
                problems_.add("model instance", ctx.where, std::string(k3DWhy));
                break;
            default:
                problems_.add("component " + component.type, ctx.where,
                              component_why(component.type));
                break;
            }
        }
        if (tiles.collider != nullptr && tiles.tilemap == nullptr) {
            problems_.add("TilemapCollider2D without a Tilemap", ctx.where);
            tiles.collider = nullptr;
        }
        if (tiles.tilemap != nullptr) {
            tiles.effector = effector;
            tiles.composite = composite;
            if (tiles.collider != nullptr && colliders.empty()) {
                tiles.rigidbody = std::exchange(rigidbody, nullptr); // the layer is the body
            }
            tile_map(ctx, tiles, out);
        }
        if (composite != nullptr && tiles.collider == nullptr) {
            problems_.add("component CompositeCollider2D", ctx.where,
                          "the colliders it merges stay separate shapes");
        }
        physics(ctx, rigidbody, colliders, effector, out);
        joints(ctx, found_joints, rigidbody, out);
        // A body only reports what it touches when asked to, as Godot's RigidBody2D does.
        if (const auto body = out.components.find("RigidBody2D");
            out.collisions && body != out.components.end()) {
            body->second["contact_monitor"] = true;
        }
        return out;
    }

    // A UI object: a control, with its other components where they convert.
    void convert_ui_object(const Context& ctx, Converted& out) {
        const GameObject& go = ctx.file->objects[static_cast<std::size_t>(ctx.index)];
        UiParts parts;
        for (const Component& component : go.components) {
            switch (component.class_id) {
            case kRectTransform:
            case kCanvasRenderer:
                break;
            case kCanvas:
                parts.canvas = &component;
                break;
            case kCanvasGroup:
                parts.group = &component;
                break;
            case kAudioSource:
                audio_source(component, ctx, out);
                break;
            case kAnimator:
                out.animator = &component;
                break;
            case kMonoBehaviour: {
                if (const KnownScript* lost = lost_script(component)) {
                    problems_.add("component " + std::string(lost->name), ctx.where,
                                  std::string(kLostWhy));
                    break;
                }
                const KnownScript* known = known_script(component);
                if (known == nullptr || !known->ui) {
                    mono_behaviour(component, ctx, out);
                } else if (!known->why.empty()) {
                    problems_.add("component " + std::string(known->name), ctx.where,
                                  std::string(known->why));
                } else if (known->name == "Outline" || known->name == "Shadow") {
                    parts.effects.push_back(&component);
                } else {
                    parts.scripts.try_emplace(std::string(known->name), &component);
                }
                break;
            }
            default:
                problems_.add("component " + component.type + " on a UI object", ctx.where,
                              "UI objects become controls, which don't take it");
                break;
            }
        }
        ui_object(ctx, parts, out);
        // A 2D player plays where its entity is; a control has no Transform2D, so it plays as
        // a plain one.
        if (const auto it = out.components.find("AudioStreamPlayer2D");
            it != out.components.end()) {
            json::Value player = std::move(it->second);
            out.components.erase(it);
            player.as_object().erase("max_distance");
            player.as_object().erase("attenuation");
            player.as_object().erase("panning_strength");
            out.components["AudioStreamPlayer"] = std::move(player);
            problems_.add("spatial sound on a UI object", ctx.where, "it plays as a plain one");
        }
    }

    static std::string component_why(std::string_view type) {
        if (type == "Canvas" || type == "CanvasRenderer" || type == "CanvasGroup") {
            return "a Canvas's part on an object without a RectTransform";
        }
        if (type == "Animation") {
            return "the legacy Animation component doesn't convert; an Animator's controller does";
        }
        if (type == "SortingGroup") {
            return "sprites under it keep their own z_index";
        }
        if (type == "ParticleSystem" || type == "ParticleSystemRenderer") {
            return "Unity particle systems don't convert; CPUParticles2D is Godot's";
        }
        if (type == "MeshRenderer" || type == "MeshFilter" || type == "SkinnedMeshRenderer" ||
            type == "Light" || type == "BoxCollider" || type == "SphereCollider" ||
            type == "CapsuleCollider" || type == "MeshCollider" || type == "Rigidbody" ||
            type == "CharacterController" || type == "LODGroup") {
            return std::string(k3DWhy);
        }
        if (type == "Terrain" || type == "TerrainCollider") {
            return std::string(k3DWhy);
        }
        if (type == "HingeJoint" || type == "FixedJoint" || type == "SpringJoint" ||
            type == "ConfigurableJoint" || type == "CharacterJoint" || type == "WheelCollider") {
            return "3D joints and wheels convert in a later import";
        }
        if (type == "NavMeshAgent" || type == "NavMeshObstacle" || type == "OffMeshLink") {
            return "navigation converts in a later import";
        }
        if (type == "ReflectionProbe" || type == "LightProbeGroup" ||
            type == "LightProbeProxyVolume") {
            return "probes don't convert; lit surfaces take their light from the sky and lights";
        }
        if (type == "Skybox") {
            return "a camera's own skybox doesn't convert; the scene's skybox is the sky";
        }
        if (type == "LineRenderer" || type == "TrailRenderer") {
            return "line and trail renderers don't convert";
        }
        if (type.find("Joint2D") != std::string_view::npos) {
            return "Godot's joints are PinJoint2D, DampedSpringJoint2D and GrooveJoint2D, which "
                   "HingeJoint2D, SpringJoint2D and SliderJoint2D become";
        }
        return {};
    }

    // Unity's angles go counterclockwise with y up; SlashEngine's clockwise with y down.
    static double z_angle(const Node& transform) {
        const double qx = transform.number_at("m_LocalRotation.x");
        const double qy = transform.number_at("m_LocalRotation.y");
        const double qz = transform.number_at("m_LocalRotation.z");
        const double qw = transform.number_at("m_LocalRotation.w", 1.0);
        return -std::atan2(2.0 * ((qw * qz) + (qx * qy)), 1.0 - (2.0 * ((qy * qy) + (qz * qz))));
    }

    json::Value transform_2d(const Node& transform, const std::string& where) {
        json::Value out;
        out["position"] = pair(transform.number_at("m_LocalPosition.x") * ppu_,
                               -transform.number_at("m_LocalPosition.y") * ppu_);
        if (std::abs(transform.number_at("m_LocalRotation.x")) > 1e-6 ||
            std::abs(transform.number_at("m_LocalRotation.y")) > 1e-6) {
            problems_.add("rotation about x or y", where,
                          "2D objects turn about z only; the rest is dropped");
        }
        out["rotation"] = tidy_angle(z_angle(transform));
        out["scale"] = json::Array{transform.number_at("m_LocalScale.x", 1.0),
                                   transform.number_at("m_LocalScale.y", 1.0)};
        return out;
    }

    // The same as a Transform2D, for working out where things are.
    [[nodiscard]] world::Transform2D local_2d(const Node& transform) const {
        world::Transform2D out;
        out.position = {static_cast<float>(transform.number_at("m_LocalPosition.x") * ppu_),
                        static_cast<float>(-transform.number_at("m_LocalPosition.y") * ppu_)};
        out.rotation = static_cast<float>(z_angle(transform));
        out.scale = {static_cast<float>(transform.number_at("m_LocalScale.x", 1.0)),
                     static_cast<float>(transform.number_at("m_LocalScale.y", 1.0))};
        return out;
    }

    int z_index(const Node& renderer, const std::string& where) {
        const std::int64_t layer = renderer.integer_at("m_SortingLayerID", 0);
        const std::int64_t order = renderer.integer_at("m_SortingOrder", 0);
        int rank = 0;
        for (std::size_t i = 0; i < sorting_layers_.size(); ++i) {
            if (sorting_layers_[i].first == layer) {
                rank = static_cast<int>(i) - default_sorting_;
            }
        }
        const std::int64_t limit =
            sorting_layers_.size() > 1 ? (sorting_step_ / 2) - 1 : kMaxZIndex;
        const std::int64_t clamped = std::clamp(order, -limit, limit);
        if (clamped != order) {
            problems_.add("sorting order out of range", where,
                          "z_index holds an order in layer from " + std::to_string(-limit) +
                              " to " + std::to_string(limit));
        }
        return static_cast<int>(std::clamp<std::int64_t>(
            (rank * std::int64_t{sorting_step_}) + clamped, -kMaxZIndex, kMaxZIndex));
    }

    void sprite_renderer(const Component& component, const Context& ctx, Converted& out) {
        const Node& body = component.body;
        json::Value sprite = json::Object{};
        const Sprite* found = nullptr;
        if (const auto ref = ref_at(body, "m_Sprite"); ref && !ref->null()) {
            std::string why;
            found = this->sprite(*ref, why);
            if (found == nullptr) {
                problems_.add("sprite", ctx.where, why);
            }
        }
        const bool flip_x = body.flag_at("m_FlipX");
        const bool flip_y = body.flag_at("m_FlipY");
        if (found != nullptr) {
            sprite["texture"] = found->texture;
            if (!found->normal_map.empty()) {
                sprite["normal_texture"] = found->normal_map;
            }
            if (!found->whole) {
                sprite["region"] = json::Array{found->region[0], found->region[1], found->region[2],
                                               found->region[3]};
            }
            // Unity places a sprite by its pivot and flips it about the pivot; Sprite2D centers
            // it, then moves it by offset, and flips it in place.
            double x = (0.5 - found->pivot_x) * found->width;
            double y = (found->pivot_y - 0.5) * found->height;
            if (flip_x) {
                x = -x;
            }
            if (flip_y) {
                y = -y;
            }
            sprite["offset"] = pair(x, y);
            const std::string filter = nearest_textures_ > linear_textures_ ? "nearest" : "linear";
            if (found->filter != filter) {
                sprite["texture_filter"] = found->filter;
            }
        }
        sprite["flip_h"] = flip_x;
        sprite["flip_v"] = flip_y;
        if (const Node* color = body.get("m_Color")) {
            sprite["modulate"] =
                json::Array{color->number_at("r", 1.0), color->number_at("g", 1.0),
                            color->number_at("b", 1.0), color->number_at("a", 1.0)};
        }
        sprite["z_index"] = z_index(body, ctx.where);
        sprite["visible"] = body.flag_at("m_Enabled", true) && ctx.active;
        if (body.integer_at("m_DrawMode", 0) != 0) {
            problems_.add("sliced or tiled sprite", ctx.where,
                          "drawn as a simple sprite; 9-slicing is NinePatchRect's, in controls");
        }
        if (const Node* materials = body.get("m_Materials");
            materials != nullptr && !materials->items.empty()) {
            if (const auto material = unity::object_ref(materials->items.front());
                material && asset_of(material->guid) != nullptr) {
                problems_.add("material", ctx.where,
                              "materials and shaders don't convert; the sprite draws with the "
                              "default material");
            }
        }
        if (body.integer_at("m_MaskInteraction", 0) != 0) {
            problems_.add("sprite mask interaction", ctx.where, "sprite masks aren't supported");
        }
        // Sprite2D passes its z_index, visibility and color to the entities under it, as Godot's
        // CanvasItem does, and Unity's SpriteRenderer doesn't; so an object with children draws its
        // sprite on a child entity of its own, as does a sprite drawn at another scale than the
        // project's.
        const GameObject& go = ctx.file->objects[static_cast<std::size_t>(ctx.index)];
        const bool scaled = found != nullptr && std::abs(found->ppu - ppu_) > 1e-6;
        const bool parent = std::ranges::any_of(go.children, [&](int child) {
            return !ctx.file->objects[static_cast<std::size_t>(child)].removed;
        });
        if (scaled || parent) {
            json::Object added;
            json::Value transform = json::Object{};
            if (scaled) {
                const double scale = ppu_ / found->ppu;
                transform["scale"] = json::Array{tidy(scale), tidy(scale)};
            }
            added["Transform2D"] = std::move(transform);
            added["Sprite2D"] = std::move(sprite);
            out.added.emplace_back("sprite", std::move(added));
            return;
        }
        out.components["Sprite2D"] = std::move(sprite);
    }

    void camera(const Component& component, const Context& ctx, const GameObject& go,
                Converted& out) {
        const Node& body = component.body;
        if (!body.flag_at("orthographic")) {
            problems_.add("perspective camera", ctx.where, std::string(k3DWhy));
            return;
        }
        const double size = body.number_at("orthographic size", 5.0);
        const double zoom = size > 0.0 ? window_height_ / (2.0 * size * ppu_) : 1.0;
        json::Value camera;
        camera["enabled"] = body.flag_at("m_Enabled", true) && ctx.active;
        camera["zoom"] = json::Array{tidy(zoom), tidy(zoom)};
        out.components["Camera2D"] = std::move(camera);
        const Node* rect = body.get("m_NormalizedViewPortRect");
        if (rect != nullptr &&
            (rect->number_at("x") != 0.0 || rect->number_at("y") != 0.0 ||
             rect->number_at("width", 1.0) != 1.0 || rect->number_at("height", 1.0) != 1.0)) {
            problems_.add("camera viewport rect", ctx.where, "every camera fills the window");
        }
        if (const auto target = ref_at(body, "m_TargetTexture"); target && !target->null()) {
            problems_.add("camera render texture", ctx.where,
                          "render textures aren't supported yet");
        }
        if (go.body.string_at("m_TagString") == "MainCamera" && !scene_clear_color_) {
            if (const Node* color = body.get("m_BackGroundColor")) {
                scene_clear_color_ = json::Array{color->number_at("r"), color->number_at("g"),
                                                 color->number_at("b"), 1.0};
            }
        }
    }

    void audio_source(const Component& component, const Context& ctx, Converted& out,
                      bool three_d = false) {
        const Node& body = component.body;
        json::Value player;
        const auto clip = ref_at(body, "m_audioClip");
        if (clip && !clip->null()) {
            const Asset* asset = asset_of(clip->guid);
            const std::string extension = asset != nullptr ? extension_of(asset->path) : "";
            if (asset == nullptr || !is_sound_file(extension)) {
                const std::string why = asset != nullptr ? unsupported_asset(extension) : "";
                problems_.add("audio clip", ctx.where,
                              asset == nullptr
                                  ? "a clip from outside the project's Assets folder"
                                  : asset->path + ": " + (why.empty() ? "not a sound file" : why));
            } else if (body.flag_at("Loop")) {
                player["stream"] = loop_stream(destination(asset->path), extension);
            } else {
                player["stream"] = destination(asset->path);
            }
        }
        const double volume = body.number_at("m_Volume", 1.0);
        const bool mute = body.flag_at("Mute");
        player["volume_db"] = mute || volume <= 0.0 ? -80.0 : tidy(20.0 * std::log10(volume));
        const double pitch = body.number_at("m_Pitch", 1.0);
        if (pitch <= 0.0) {
            problems_.add("audio pitch at or below 0", ctx.where, "plays at pitch 1");
        } else {
            player["pitch_scale"] = pitch;
        }
        player["autoplay"] =
            body.flag_at("m_PlayOnAwake", true) && body.flag_at("m_Enabled", true) && ctx.active;
        const double spatial = body.number_at("panLevelCustomCurve.m_Curve.0.value", 0.0);
        std::string kind = "AudioStreamPlayer";
        if (spatial > 0.0 && three_d) {
            // Unity's logarithmic rolloff is Godot's inverse distance from MinDistance, never
            // louder than at it.
            kind = "AudioStreamPlayer3D";
            player["unit_size"] = body.number_at("MinDistance", 1.0);
            player["max_db"] = 0.0;
            player["max_distance"] = body.number_at("MaxDistance", 500.0);
            if (body.integer_at("rolloffMode", 0) != 0) {
                problems_.add("audio rolloff", ctx.where,
                              "sounds fade with the inverse of their distance, as Unity's "
                              "logarithmic rolloff");
            }
        } else if (spatial > 0.0) {
            kind = "AudioStreamPlayer2D";
            player["max_distance"] = tidy(body.number_at("MaxDistance", 500.0) * ppu_);
            if (body.integer_at("rolloffMode", 0) != 1) {
                problems_.add("audio rolloff", ctx.where,
                              "sounds fade linearly to max_distance (attenuation 1)");
            }
        }
        out.components[kind] = std::move(player);
        if (const auto group = ref_at(body, "OutputAudioMixerGroup"); group && !group->null()) {
            const std::string* bus = nullptr;
            if (const auto mixer = mixer_buses_.find(group->guid); mixer != mixer_buses_.end()) {
                if (const auto it = mixer->second.find(group->file_id); it != mixer->second.end()) {
                    bus = &it->second;
                }
            }
            if (bus == nullptr) {
                problems_.add("audio mixer group", ctx.where,
                              "a group of a mixer that didn't convert; the sound plays on Master");
            } else if (*bus != "Master") {
                out.components[kind]["bus"] = *bus;
            }
        }
    }

    // A .audio.json file that loops a sound, written once per sound.
    std::string loop_stream(const std::string& path, const std::string& extension) {
        const std::string loop = without_extension(path) + ".loop.audio.json";
        if (loops_written_.insert(loop).second) {
            json::Value stream;
            stream["format"] = 1;
            stream["source"] = path;
            if (extension == ".wav") {
                stream["loop_mode"] = "forward";
            } else {
                stream["loop"] = true;
            }
            add_file(loop, json::write(stream) + "\n");
        }
        return loop;
    }

    // The scripts' skeletons, made once the project's files are listed.
    Skeletons& skeletons() {
        SLASH_ASSERT(skeletons_.has_value(), "the skeletons are made before they are used");
        return *skeletons_;
    }

    // The skeleton of a project script, read the first time an object uses it, or nullptr when
    // it can't be read.
    Skeleton* skeleton_of(const std::string& path) {
        if (Skeleton* found = skeletons().find(path); found != nullptr) {
            return found;
        }
        if (unreadable_scripts_.contains(path)) {
            return nullptr;
        }
        auto text = platform::read_file(source_ + path);
        if (!text) {
            unreadable_scripts_.insert(path);
            return nullptr;
        }
        return &skeletons().add(
            path, read_csharp(destination(path), text.value(), ScriptSource::UnityCsharp),
            components_);
    }

    // A script's component on an object's entity: the values its serialized fields have, as the
    // skeleton's fields take them.
    static json::Value script_component(Script& script, const json::Value& values) {
        json::Value component = json::Object{};
        for (ScriptField& field : script.fields) {
            const json::Value* value = values.find(field.source_name);
            if (!field.exported || value == nullptr) {
                continue;
            }
            retype(field, *value);
            json::Value converted = field_value(field.type, field.comment, *value);
            if (!converted.is_null()) {
                component[field.name] = std::move(converted);
            }
        }
        return component;
    }

    // A field the skeleton took for an object's entity, as what scenes say it holds: the path
    // of an asset or a prefab, a value of its own (a [Serializable] class) or a number (an enum).
    static void retype(ScriptField& field, const json::Value& value) {
        if (field.kind == FieldKind::Value) {
            return;
        }
        const bool list = field.kind == FieldKind::Entities;
        const json::Value* sample = &value;
        if (list) {
            const auto& items = value.is_array() ? value.as_array() : json::Array{};
            const auto set = std::ranges::find_if(
                items, [](const json::Value& item) { return !item.is_null(); });
            if (set == items.end()) {
                return;
            }
            sample = &*set;
        }
        std::string type;
        std::string comment = field.comment;
        if (sample->is_string() && (sample->as_string().find('.') != std::string::npos ||
                                    sample->as_string().starts_with("unity:"))) {
            type = "std::string";
            comment += list ? ": their paths" : ": its path";
        } else if (sample->is_object() || sample->is_array()) {
            type = "slash::json::Value";
        } else if (sample->is_int()) {
            type = "int";
        } else {
            return;
        }
        field.type = list && type != "slash::json::Value" ? "std::vector<" + type + ">" : type;
        field.comment = std::move(comment);
        field.kind = FieldKind::Value;
        field.init.clear();
    }

    // A value from a Unity scene as the JSON of a skeleton's field of a C++ type: vectors and
    // colors as lists, flags as booleans, objects as their entities' IDs.
    static json::Value field_value(const std::string& type, const std::string& comment,
                                   const json::Value& value) {
        if (type.starts_with("std::vector<")) {
            const std::string item = type.substr(12, type.size() - 13);
            json::Array items;
            if (value.is_array()) {
                for (const json::Value& entry : value.as_array()) {
                    json::Value converted = field_value(item, comment, entry);
                    if (!converted.is_null() || item == "entt::entity") {
                        items.push_back(std::move(converted));
                    }
                }
            }
            return items;
        }
        if (type == "bool" && value.is_number()) {
            return value.as_double() != 0.0;
        }
        if ((type == "float" || type == "double") && value.is_number()) {
            return value.as_double(); // YAML writes 4 for 4.0
        }
        if (type == "entt::entity") {
            if (!value.is_string()) {
                return nullptr;
            }
            std::string id = value.as_string();
            if (const std::size_t hash = id.find('#'); hash != std::string::npos) {
                id.resize(hash); // the object a component is on
            }
            return id;
        }
        if (type == "std::string" && (value.is_number() || value.is_bool())) {
            return json::write(value); // YAML wrote the text without quotes
        }
        if (type == "int" && value.is_object()) {
            const json::Value* bits = value.find("m_Bits"); // a LayerMask
            return bits != nullptr ? *bits : json::Value(nullptr);
        }
        static const std::map<std::string, std::size_t, std::less<>> kSizes{{"slash::Vec2", 2},
                                                                            {"slash::Vec2i", 2},
                                                                            {"slash::Vec3", 3},
                                                                            {"slash::Vec4", 4},
                                                                            {"slash::Quat", 4}};
        if (const auto size = kSizes.find(type); size != kSizes.end() && value.is_object()) {
            static constexpr std::array<std::array<std::string_view, 4>, 3> kKeys{
                {{"x", "y", "z", "w"}, {"r", "g", "b", "a"}, {"x", "y", "width", "height"}}};
            const double scale = comment == "Color32" ? 255.0 : 1.0;
            for (const auto& keys : kKeys) {
                json::Array out;
                for (std::size_t i = 0; i < size->second; ++i) {
                    const json::Value* part = value.find(keys.at(i));
                    if (part == nullptr || !part->is_number()) {
                        break;
                    }
                    out.emplace_back(type == "slash::Vec2i"
                                         ? json::Value(part->as_int())
                                         : json::Value(part->as_double() / scale));
                }
                if (out.size() == size->second) {
                    return out;
                }
            }
            return nullptr;
        }
        return value;
    }

    void mono_behaviour(const Component& component, const Context& ctx, Converted& out) {
        const Node& body = component.body;
        const auto script = ref_at(body, "m_Script");
        const Asset* asset = script ? asset_of(script->guid) : nullptr;
        if (asset != nullptr && extension_of(asset->path) == ".cs") {
            json::Value values = json::Object{};
            for (const auto& [key, value] : body.entries) {
                if (!standard_key(key)) {
                    values[key] = convert_value(value, *ctx.file);
                }
            }
            if (Skeleton* skeleton = skeleton_of(asset->path);
                skeleton != nullptr && skeleton->script.node) {
                Script& code = skeleton->script;
                if (out.components.contains(code.component)) {
                    problems_.add("two of one script on an object", ctx.where,
                                  "the entity has one " + code.component +
                                      ", with the first one's values");
                } else {
                    out.components[code.component] = script_component(code, values);
                }
                out.collisions = out.collisions ||
                                 std::ranges::any_of(code.functions, [](const ScriptFunction& f) {
                                     return f.source_name == "OnCollisionEnter2D" ||
                                            f.source_name == "OnCollisionExit2D" ||
                                            f.source_name == "OnCollisionEnter" ||
                                            f.source_name == "OnCollisionExit";
                                 });
            }
            ScriptRecord& record = scripts_[asset->path];
            record.uses.push_back(
                ScriptUse{.scene = ctx.file->scene,
                          .entity = ctx.file->objects[static_cast<std::size_t>(ctx.index)].entity,
                          .values = std::move(values),
                          .enabled = body.flag_at("m_Enabled", true)});
            return;
        }
        if (script) {
            for (const KnownScript& known : kKnownScripts) {
                if (known.guid == script->guid) {
                    problems_.add("component " + std::string(known.name), ctx.where,
                                  std::string(known.ui ? kUiWhy : known.why));
                    return;
                }
            }
        }
        // Unity 6 writes a package script's class as "Assembly::Namespace.Class".
        std::string name = body.string_at("m_EditorClassIdentifier");
        if (const auto colons = name.rfind("::"); colons != std::string::npos) {
            name = name.substr(colons + 2);
        }
        if (const auto dot = name.rfind('.'); dot != std::string::npos) {
            name = name.substr(dot + 1);
        }
        if (!name.empty()) {
            problems_.add("component " + name, ctx.where, "a script from a Unity package");
            return;
        }
        problems_.add("script from a package or DLL", ctx.where,
                      "its script (GUID " + (script ? script->guid : std::string()) +
                          ") isn't in the project's Assets folder");
    }

    // A script's serialized value as JSON: numbers and strings as they read, references to
    // objects as entity IDs and to assets as their paths.
    json::Value convert_value(const Node& node, const Expanded& file) {
        if (node.is_seq()) {
            json::Array items;
            for (const Node& item : node.items) {
                items.push_back(convert_value(item, file));
            }
            return items;
        }
        if (node.is_map()) {
            if (const auto ref = unity::object_ref(node)) {
                if (ref->null() && ref->guid.empty()) {
                    return nullptr;
                }
                if (ref->guid.empty()) {
                    return entity_ref(file, ref->file_id);
                }
                if (const Asset* asset = asset_of(ref->guid)) {
                    const std::string extension = extension_of(asset->path);
                    if (extension == ".prefab" || extension == ".unity") {
                        return destination(without_extension(asset->path) +
                                           std::string(world::kSceneSuffix));
                    }
                    return destination(asset->path);
                }
                return "unity:" + ref->guid;
            }
            json::Value out = json::Object{};
            for (const auto& [key, value] : node.entries) {
                out[key] = convert_value(value, file);
            }
            return out;
        }
        if (const auto integer = parse_int(node.text)) {
            return *integer;
        }
        if (const auto number = node.number();
            number && std::isfinite(*number) && node.text.find_first_of("0123456789") == 0) {
            return *number;
        }
        if (const auto number = node.number();
            number && std::isfinite(*number) && node.text.size() > 1 && node.text[0] == '-') {
            return *number;
        }
        return node.text;
    }

    // An object of the file as its entity ID, "entity#Component" for a component other than a
    // Transform, or null.
    static json::Value entity_ref(const Expanded& file, std::int64_t id) {
        if (const auto go = file.by_id.find(id); go != file.by_id.end()) {
            return file.objects[static_cast<std::size_t>(go->second)].entity;
        }
        if (const auto at = file.components.find(id); at != file.components.end()) {
            const GameObject& go = file.objects[static_cast<std::size_t>(at->second.first)];
            const Component& component = go.components[static_cast<std::size_t>(at->second.second)];
            if (component.class_id == kTransform || component.class_id == kRectTransform) {
                return go.entity;
            }
            return go.entity + "#" + component.type;
        }
        return nullptr;
    }

    // --- Physics ---

    struct Piece {
        std::string component; // "CollisionShape2D" or "CollisionPolygon2D"
        json::Value fields;
        bool trigger = false;
    };

    [[nodiscard]] json::Value point(const Node& p, double offset_x, double offset_y) const {
        return pair((p.number_at("x") + offset_x) * ppu_, -(p.number_at("y") + offset_y) * ppu_);
    }

    std::vector<Piece> collider_pieces(const Component& collider, const Context& ctx,
                                       bool one_way) {
        const Node& body = collider.body;
        const double ox = body.number_at("m_Offset.x");
        const double oy = body.number_at("m_Offset.y");
        const bool disabled = !body.flag_at("m_Enabled", true) || !ctx.active;
        const bool trigger = body.flag_at("m_IsTrigger");
        std::vector<Piece> pieces;
        const auto shape = [&](json::Value fields) {
            fields["position"] = pair(ox * ppu_, -oy * ppu_);
            pieces.push_back(Piece{
                .component = "CollisionShape2D", .fields = std::move(fields), .trigger = trigger});
        };
        switch (collider.class_id) {
        case kBoxCollider2D: {
            json::Value fields;
            fields["shape"] = "rectangle";
            fields["size"] = pair(std::abs(body.number_at("m_Size.x", 1.0)) * ppu_,
                                  std::abs(body.number_at("m_Size.y", 1.0)) * ppu_);
            if (body.number_at("m_EdgeRadius") > 0.0) {
                problems_.add("BoxCollider2D edge radius", ctx.where, "the corners stay square");
            }
            shape(std::move(fields));
            break;
        }
        case kCircleCollider2D: {
            json::Value fields;
            fields["shape"] = "circle";
            fields["radius"] = tidy(std::abs(body.number_at("m_Radius", 0.5)) * ppu_);
            shape(std::move(fields));
            break;
        }
        case kCapsuleCollider2D: {
            const double width = std::abs(body.number_at("m_Size.x", 0.5)) * ppu_;
            const double height = std::abs(body.number_at("m_Size.y", 1.0)) * ppu_;
            const bool horizontal = body.integer_at("m_Direction", 0) == 1;
            json::Value fields;
            fields["shape"] = "capsule";
            fields["radius"] = tidy((horizontal ? height : width) / 2.0);
            fields["height"] = tidy(std::max(width, height));
            if (horizontal) {
                fields["rotation"] = tidy_angle(std::numbers::pi / 2.0);
            }
            shape(std::move(fields));
            break;
        }
        case kPolygonCollider2D: {
            const Node* paths = body.path("m_Points.m_Paths");
            for (const Node& path : paths != nullptr ? paths->items : std::vector<Node>{}) {
                json::Array polygon;
                for (const Node& p : path.items) {
                    polygon.push_back(point(p, ox, oy));
                }
                if (polygon.size() < 3) {
                    continue;
                }
                json::Value fields;
                fields["polygon"] = std::move(polygon);
                pieces.push_back(Piece{.component = "CollisionPolygon2D",
                                       .fields = std::move(fields),
                                       .trigger = trigger});
            }
            break;
        }
        case kEdgeCollider2D: {
            const Node* points = body.get("m_Points");
            const std::vector<Node> none;
            const std::vector<Node>& list = points != nullptr ? points->items : none;
            for (std::size_t i = 0; i + 1 < list.size(); ++i) {
                json::Value fields;
                fields["shape"] = "segment";
                fields["a"] = point(list[i], ox, oy);
                fields["b"] = point(list[i + 1], ox, oy);
                fields["position"] = pair(0.0, 0.0);
                pieces.push_back(Piece{.component = "CollisionShape2D",
                                       .fields = std::move(fields),
                                       .trigger = trigger});
            }
            if (body.number_at("m_EdgeRadius") > 0.0) {
                problems_.add("EdgeCollider2D edge radius", ctx.where, "the edges stay thin");
            }
            break;
        }
        default:
            break;
        }
        for (Piece& piece : pieces) {
            if (disabled) {
                piece.fields["disabled"] = true;
            }
            if (one_way && body.flag_at("m_UsedByEffector")) {
                piece.fields["one_way_collision"] = true;
            }
        }
        return pieces;
    }

    // The nearest object above that has a Rigidbody2D, and how many levels up it is.
    [[nodiscard]] std::pair<int, int> body_above(const Expanded& file, int index) const {
        int levels = 0;
        for (int at = file.objects[static_cast<std::size_t>(index)].parent; at >= 0;
             at = file.objects[static_cast<std::size_t>(at)].parent) {
            ++levels;
            for (const Component& component :
                 file.objects[static_cast<std::size_t>(at)].components) {
                if (component.class_id == kRigidbody2D) {
                    return {at, levels};
                }
            }
            if (levels > 256) {
                break;
            }
        }
        return {-1, 0};
    }

    void physics(const Context& ctx, const Component* rigidbody,
                 const std::vector<const Component*>& colliders, const Component* effector,
                 Converted& out) {
        if (rigidbody == nullptr && colliders.empty()) {
            return;
        }
        const GameObject& go = ctx.file->objects[static_cast<std::size_t>(ctx.index)];
        const std::int64_t layer =
            std::clamp<std::int64_t>(go.body.integer_at("m_Layer", 0), 0, 31);
        const std::uint32_t layer_bit = 1U << static_cast<std::uint32_t>(layer);
        const std::uint32_t mask = layer_masks_.at(static_cast<std::size_t>(layer));
        const bool one_way = effector != nullptr && effector->body.flag_at("m_UseOneWay", true);
        std::vector<Piece> solid;
        std::vector<Piece> triggers;
        Material material = default_material_;
        for (const Component* collider : colliders) {
            for (Piece& piece : collider_pieces(*collider, ctx, one_way)) {
                (piece.trigger ? triggers : solid).push_back(std::move(piece));
            }
            if (const auto ref = ref_at(collider->body, "m_Material"); ref && !ref->guid.empty()) {
                material = physics_material(ref->guid);
            }
        }
        if (rigidbody != nullptr) {
            const auto has_material = [](const Component* collider) {
                const auto own = ref_at(collider->body, "m_Material");
                return own && !own->guid.empty();
            };
            if (const auto ref = ref_at(rigidbody->body, "m_Material");
                ref && !ref->guid.empty() && std::ranges::none_of(colliders, has_material)) {
                material = physics_material(ref->guid);
            }
        }
        const auto body_fields = [&](json::Value& fields, bool with_material) {
            fields["collision_layer"] = std::int64_t{layer_bit};
            fields["collision_mask"] = std::int64_t{mask};
            if (with_material) {
                fields["friction"] = material.friction;
                fields["bounce"] = material.bounce;
            }
        };
        // The body this object becomes, if any.
        std::string body_type;
        if (rigidbody != nullptr) {
            const Node& rb = rigidbody->body;
            const std::int64_t kind = rb.integer_at("m_BodyType", 0);
            json::Value fields;
            body_fields(fields, true);
            if (kind == 0) {
                body_type = "RigidBody2D";
                if (rb.flag_at("m_UseAutoMass")) {
                    problems_.add("Rigidbody2D auto mass", ctx.where,
                                  "the body keeps the mass it had");
                }
                fields["mass"] = rb.number_at("m_Mass", 1.0);
                fields["gravity_scale"] = rb.number_at("m_GravityScale", 1.0);
                const Node* linear = rb.get("m_LinearDamping");
                const Node* angular = rb.get("m_AngularDamping");
                fields["linear_damp"] = linear != nullptr ? linear->number().value_or(0.0)
                                                          : rb.number_at("m_LinearDrag");
                fields["angular_damp"] = angular != nullptr ? angular->number().value_or(0.05)
                                                            : rb.number_at("m_AngularDrag", 0.05);
                const std::int64_t constraints = rb.integer_at("m_Constraints", 0);
                fields["lock_rotation"] = (constraints & 4) != 0;
                if ((constraints & 3) != 0) {
                    problems_.add("Rigidbody2D frozen position", ctx.where,
                                  "bodies can't freeze x or y; gameplay code can hold them");
                }
                if (rb.integer_at("m_CollisionDetection", 0) == 1) {
                    fields["continuous_cd"] = "cast_shape";
                }
                const std::int64_t sleeping = rb.integer_at("m_SleepingMode", 1);
                fields["can_sleep"] = sleeping != 0;
                fields["sleeping"] = sleeping == 2;
            } else if (kind == 1) {
                body_type = "AnimatableBody2D";
            } else {
                body_type = "StaticBody2D";
            }
            if (!rb.flag_at("m_Simulated", true)) {
                problems_.add("Rigidbody2D not simulated", ctx.where,
                              "the body still takes part in physics");
            }
            out.components[body_type] = std::move(fields);
        } else if (!solid.empty()) {
            const auto [above, levels] = body_above(*ctx.file, ctx.index);
            if (above < 0) {
                body_type = "StaticBody2D"; // a collider with no Rigidbody2D is static
            } else if (levels > 1) {
                body_type = "AnimatableBody2D";
                problems_.add("collider two or more levels under its Rigidbody2D", ctx.where,
                              "it becomes an AnimatableBody2D that follows its parent; bodies "
                              "take shapes from their own children only");
            }
            if (!body_type.empty()) {
                json::Value fields;
                body_fields(fields, true);
                out.components[body_type] = std::move(fields);
            }
        } else if (!triggers.empty()) {
            body_type = "Area2D";
            json::Value fields;
            body_fields(fields, false);
            out.components[body_type] = std::move(fields);
            solid = std::move(triggers);
            triggers.clear();
        }
        // The first shape and the first polygon on the object, the rest on child entities.
        const bool has_body = !body_type.empty();
        bool shape_taken = false;
        bool polygon_taken = false;
        int extra = 1;
        for (Piece& piece : solid) {
            bool& taken = piece.component == "CollisionShape2D" ? shape_taken : polygon_taken;
            if (!taken) {
                taken = true;
                out.components[piece.component] = std::move(piece.fields);
                continue;
            }
            if (!has_body) {
                problems_.add("more colliders than an object without a Rigidbody2D can hold",
                              ctx.where,
                              "its parent's body takes one shape and one polygon from it; the "
                              "rest don't attach");
                continue;
            }
            json::Object added;
            added["Transform2D"] = json::Object{};
            added[piece.component] = std::move(piece.fields);
            out.added.emplace_back("shape_" + std::to_string(++extra), std::move(added));
        }
        // Triggers on a body: an Area2D child entity, as Godot keeps areas apart from bodies.
        int trigger_count = 0;
        for (Piece& piece : triggers) {
            json::Object added;
            added["Transform2D"] = json::Object{};
            json::Value area;
            body_fields(area, false);
            added["Area2D"] = std::move(area);
            added[piece.component] = std::move(piece.fields);
            ++trigger_count;
            out.added.emplace_back(trigger_count == 1 ? std::string("trigger")
                                                      : "trigger_" + std::to_string(trigger_count),
                                   std::move(added));
        }
    }

    using Color = std::array<double, 4>;

    // --- 2D lights ---

    // URP's 2D lights shade as Godot's do with a CanvasModulate for the global light and a
    // PointLight2D in add mode for each other light: what they light gets its color times the
    // ambient color plus each light's color, energy and texture there. Each light's shape and
    // falloff become its texture, white with the light's strength in its alpha.
    static constexpr std::uint32_t kLightTextureSize = 256;

    // How strong a 2D light is at an attenuation t, from 1 where it is full to 0 where it stops,
    // as URP's falloff curve: t raised to a power the falloff strength sets, 1 at the default
    // strength of 0.5. Higher strengths dim the light sooner, softening its edge.
    static double falloff(double t, double strength) {
        const double base = 0.25 + (std::clamp(strength, 0.0, 1.0) / 2.0);
        if (base == 0.5) {
            return std::clamp(t, 0.0, 1.0); // linear, and the same everywhere without libm
        }
        return std::pow(std::clamp(t, 0.0, 1.0), std::log(1.0 - base) / std::log(base));
    }

    // Writes a light texture once: white, each pixel's alpha what alpha gives for the pixel's
    // middle.
    void add_light_texture(const std::string& path, std::uint32_t width, std::uint32_t height,
                           const std::function<double(double x, double y)>& alpha) {
        if (!light_textures_.insert(path).second) {
            return;
        }
        render::Image image{.width = width, .height = height, .pixels = {}};
        image.pixels.reserve(std::size_t{width} * height * 4);
        for (std::uint32_t y = 0; y < height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                const double value =
                    alpha(static_cast<double>(x) + 0.5, static_cast<double>(y) + 0.5);
                image.pixels.insert(
                    image.pixels.end(),
                    {255, 255, 255,
                     static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0, 1.0) * 255.0))});
            }
        }
        if (auto png = render::encode_png(image)) {
            add_file(path, std::move(png).value());
        }
    }

    // A spot light's texture, its outer radius half the texture's size: full inside the inner
    // radius (a share of the outer) and the cone's inner angle, gone past the outer radius and
    // angle, the cone pointing up as Unity's points along the object's y axis.
    std::string spot_texture(double inner, double inner_angle, double outer_angle,
                             double strength) {
        const auto percent = [](double value) {
            return static_cast<int>(std::lround(std::clamp(value, 0.0, 1.0) * 100.0));
        };
        const int inner_percent = percent(inner);
        const int strength_percent = percent(strength);
        const auto degrees = [](double value) {
            return static_cast<int>(std::lround(std::clamp(value, 0.0, 360.0)));
        };
        const int cone_out = degrees(outer_angle);
        const int cone_in = std::min(degrees(inner_angle), cone_out);
        const bool cone = cone_out < 360;
        std::string name =
            "spot_" + std::to_string(inner_percent) + "_" + std::to_string(strength_percent);
        if (cone) {
            name.append("_")
                .append(std::to_string(cone_in))
                .append("_")
                .append(std::to_string(cone_out));
        }
        const std::string path = destination("Assets/lights/" + name + ".png");
        const double radius_in = inner_percent / 100.0;
        const double half_in = cone_in / 2.0;
        const double half_out = cone_out / 2.0;
        const double soft = strength_percent / 100.0;
        constexpr double kHalf = kLightTextureSize / 2.0;
        add_light_texture(path, kLightTextureSize, kLightTextureSize, [&](double x, double y) {
            const double dx = (x - kHalf) / kHalf;
            const double dy = (y - kHalf) / kHalf;
            const double distance = std::sqrt((dx * dx) + (dy * dy));
            double t = 0.0;
            if (radius_in >= 1.0) {
                t = distance <= 1.0 ? 1.0 : 0.0;
            } else {
                t = (1.0 - distance) / (1.0 - radius_in);
            }
            if (cone) {
                const double angle = distance > 0.0
                                         ? std::acos(std::clamp(-dy / distance, -1.0, 1.0)) *
                                               180.0 / std::numbers::pi
                                         : 0.0;
                if (half_out <= half_in) {
                    t *= angle <= half_out ? 1.0 : 0.0;
                } else {
                    t *= std::clamp((half_out - angle) / (half_out - half_in), 0.0, 1.0);
                }
            }
            return falloff(t, soft);
        });
        return path;
    }

    struct LightTexture {
        std::string path;
        double scale = 1.0;             // the texture's pixels per light pixel
        std::array<double, 2> middle{}; // the middle of what it covers, in pixels
    };

    // A freeform light's texture: full inside its polygon (in pixels), fading over falloff_size
    // pixels outside it. Named by a hash of its shape, so a prefab's light and its instances' share
    // one.
    LightTexture freeform_texture(const std::vector<std::array<double, 2>>& polygon,
                                  double falloff_size, double strength) {
        double left = std::numeric_limits<double>::max();
        double top = left;
        double right = std::numeric_limits<double>::lowest();
        double bottom = right;
        std::uint64_t hash = 14695981039346656037ULL;
        const auto mix = [&hash](double value) {
            const std::string text = json::write(json::Value(tidy(value)));
            for (const char c : text) {
                hash = (hash ^ static_cast<unsigned char>(c)) * 1099511628211ULL;
            }
            hash = (hash ^ ',') * 1099511628211ULL;
        };
        for (const auto& [x, y] : polygon) {
            left = std::min(left, x);
            right = std::max(right, x);
            top = std::min(top, y);
            bottom = std::max(bottom, y);
            mix(x);
            mix(y);
        }
        mix(falloff_size);
        mix(strength);
        left -= falloff_size;
        top -= falloff_size;
        right += falloff_size;
        bottom += falloff_size;
        const double scale =
            std::max({right - left, bottom - top, 1.0}) / static_cast<double>(kLightTextureSize);
        const auto pixels = [scale](double size) {
            return static_cast<std::uint32_t>(std::max(1L, std::lround(size / scale)));
        };
        const std::uint32_t width = pixels(right - left);
        const std::uint32_t height = pixels(bottom - top);
        std::string hex;
        for (int shift = 60; shift >= 32; shift -= 4) {
            hex.push_back("0123456789abcdef"[(hash >> static_cast<unsigned>(shift)) & 0xFU]);
        }
        LightTexture out{.path = destination("Assets/lights/freeform_" + hex + ".png"),
                         .scale = scale,
                         .middle = {left + (width * scale / 2.0), top + (height * scale / 2.0)}};
        add_light_texture(out.path, width, height, [&](double x, double y) {
            const double px = left + (x * scale);
            const double py = top + (y * scale);
            bool inside = false;
            double nearest = std::numeric_limits<double>::max();
            for (std::size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
                const auto& [ax, ay] = polygon[i];
                const auto& [bx, by] = polygon[j];
                if ((ay > py) != (by > py) && px < ax + ((bx - ax) * (py - ay) / (by - ay))) {
                    inside = !inside;
                }
                const double ex = bx - ax;
                const double ey = by - ay;
                const double length = (ex * ex) + (ey * ey);
                const double along =
                    length > 0.0
                        ? std::clamp((((px - ax) * ex) + ((py - ay) * ey)) / length, 0.0, 1.0)
                        : 0.0;
                const double cx = ax + (ex * along) - px;
                const double cy = ay + (ey * along) - py;
                nearest = std::min(nearest, std::sqrt((cx * cx) + (cy * cy)));
            }
            if (inside) {
                return 1.0;
            }
            if (falloff_size <= 0.0) {
                return 0.0;
            }
            return falloff(1.0 - (nearest / falloff_size), strength);
        });
        return out;
    }

    // A sprite light's texture: the sprite's own texture, or its part of a sheet cut out once.
    std::optional<std::string> cookie_texture(const ObjectRef& ref, const Sprite& sprite,
                                              const std::string& where) {
        if (sprite.whole) {
            return sprite.texture;
        }
        auto [it, added] = cookies_.try_emplace(std::pair{ref.guid, ref.file_id});
        if (!added) {
            return it->second.empty() ? std::nullopt : std::optional(it->second);
        }
        const Texture& sheet = texture(ref.guid);
        auto bytes = platform::read_file(source_ + sheet.path);
        auto image =
            bytes ? render::decode_image(bytes.value()) : Result<render::Image>(bytes.error());
        if (!image) {
            problems_.add("light sprite", where, sheet.path + ": " + image.error().message);
            return std::nullopt;
        }
        const render::Image& whole = image.value();
        const auto at = [](double value, std::uint32_t limit) {
            return static_cast<std::uint32_t>(
                std::clamp(std::lround(value), 0L, static_cast<long>(limit)));
        };
        const std::uint32_t x0 = at(sprite.region[0], whole.width);
        const std::uint32_t y0 = at(sprite.region[1], whole.height);
        const std::uint32_t x1 = at(sprite.region[0] + sprite.region[2], whole.width);
        const std::uint32_t y1 = at(sprite.region[1] + sprite.region[3], whole.height);
        if (x1 <= x0 || y1 <= y0) {
            problems_.add("light sprite", where,
                          sheet.path + ": the sprite is outside its texture");
            return std::nullopt;
        }
        render::Image part{.width = x1 - x0, .height = y1 - y0, .pixels = {}};
        for (std::uint32_t y = y0; y < y1; ++y) {
            const auto row = whole.pixels.begin() +
                             static_cast<std::ptrdiff_t>(((std::size_t{y} * whole.width) + x0) * 4);
            part.pixels.insert(part.pixels.end(), row,
                               row + static_cast<std::ptrdiff_t>(std::size_t{part.width} * 4));
        }
        const std::string name =
            take(cookie_names_, sprite.name.empty() ? "sprite" : snake_case(sprite.name));
        std::string path = destination("Assets/lights/" + name + ".png");
        if (auto png = render::encode_png(part)) {
            add_file(path, std::move(png).value());
            it->second = path;
            return path;
        }
        return std::nullopt;
    }

    // The sorting layers a light names, by their ranks from the default layer; every layer when
    // it names none in a list it doesn't have.
    std::vector<int> sorting_ranks(const Node* layers) const {
        std::vector<std::uint32_t> ids;
        if (layers != nullptr && layers->is_seq()) {
            for (const Node& item : layers->items) {
                ids.push_back(static_cast<std::uint32_t>(item.integer().value_or(0)));
            }
        } else if (layers != nullptr) {
            const std::string_view text = layers->string();
            for (std::size_t at = 0; at + 8 <= text.size(); at += 8) {
                if (const auto word = hex_word(text.substr(at, 8))) {
                    ids.push_back(*word);
                }
            }
        }
        std::vector<int> ranks;
        if (sorting_layers_.empty()) {
            if (layers == nullptr || contains(ids, 0U)) {
                ranks.push_back(0);
            }
            return ranks;
        }
        for (std::size_t i = 0; i < sorting_layers_.size(); ++i) {
            if (layers == nullptr ||
                contains(ids, static_cast<std::uint32_t>(sorting_layers_[i].first))) {
                ranks.push_back(static_cast<int>(i) - default_sorting_);
            }
        }
        return ranks;
    }

    // The z indices a light's sorting layers' sprites have, or nullopt when it lights none.
    std::optional<std::pair<int, int>> light_z_range(const Node& body, const std::string& where) {
        const std::vector<int> ranks = sorting_ranks(body.get("m_ApplyToSortingLayers"));
        if (ranks.empty()) {
            return std::nullopt;
        }
        if (ranks.back() - ranks.front() + 1 != static_cast<int>(ranks.size())) {
            problems_.add("Light 2D on sorting layers apart", where,
                          "its z range takes in the sorting layers between them too");
        }
        const int limit = sorting_layers_.size() > 1 ? (sorting_step_ / 2) - 1 : kMaxZIndex;
        return std::pair{
            std::clamp((ranks.front() * sorting_step_) - limit, -kMaxZIndex, kMaxZIndex),
            std::clamp((ranks.back() * sorting_step_) + limit, -kMaxZIndex, kMaxZIndex)};
    }

    // A Light2D of URP's 2D renderer: a global light as the scene's CanvasModulate, a spot,
    // freeform or sprite light as a PointLight2D through a texture of its shape.
    void light_2d(const Component& component, const Context& ctx, Converted& out) {
        const Node& body = component.body;
        // Lights saved before Unity versioned them keep a few settings in older fields.
        const std::int64_t version = body.integer_at("m_ComponentVersion", 0);
        const std::int64_t type = body.integer_at("m_LightType", 3);
        bool enabled = body.flag_at("m_Enabled", true) && ctx.active;
        const Color color = color_at(body, "m_Color");
        const double intensity = std::max(0.0, body.number_at("m_Intensity", 1.0));
        if (type == 4) {
            global_light(body, color, intensity, enabled, ctx, out);
            return;
        }
        const double strength = body.number_at("m_FalloffIntensity", 0.5);
        json::Value light;
        if (type == 3) {
            const double outer =
                std::max(0.0, body.number_at("m_PointLightOuterRadius", 1.0)) * ppu_;
            if (outer <= 0.0) {
                problems_.add("Spot Light 2D with no radius", ctx.where, "it lights nothing");
                return;
            }
            const double inner =
                std::clamp(body.number_at("m_PointLightInnerRadius") * ppu_, 0.0, outer);
            light["texture"] =
                spot_texture(inner / outer, body.number_at("m_PointLightInnerAngle", 360.0),
                             body.number_at("m_PointLightOuterAngle", 360.0), strength);
            light["texture_scale"] = tidy(2.0 * outer / kLightTextureSize);
            const auto cookie = ref_at(body, "m_LightCookieSprite");
            const auto old_cookie =
                version < 4 ? ref_at(body, "m_DeprecatedPointLightCookieSprite") : std::nullopt;
            if ((cookie && !cookie->null()) || (old_cookie && !old_cookie->null())) {
                problems_.add("Spot Light 2D cookie", ctx.where,
                              "the light shines without its cookie sprite");
            }
        } else if (type == 1) {
            std::vector<std::array<double, 2>> polygon;
            if (const Node* path = body.get("m_ShapePath"); path != nullptr) {
                for (const Node& p : path->items) {
                    polygon.push_back(
                        {tidy(p.number_at("x") * ppu_), tidy(-p.number_at("y") * ppu_)});
                }
            }
            if (polygon.size() < 3) {
                problems_.add("Freeform Light 2D with fewer than three points", ctx.where,
                              "it lights nothing");
                return;
            }
            if (body.number_at("m_ShapeLightFalloffOffset.x") != 0.0 ||
                body.number_at("m_ShapeLightFalloffOffset.y") != 0.0) {
                problems_.add("Freeform Light 2D falloff offset", ctx.where,
                              "the falloff spreads evenly around the shape");
            }
            const LightTexture made = freeform_texture(
                polygon, std::max(0.0, body.number_at("m_ShapeLightFalloffSize", 0.5)) * ppu_,
                strength);
            light["texture"] = made.path;
            light["texture_scale"] = tidy(made.scale);
            light["offset"] = pair(made.middle[0], made.middle[1]);
        } else if (type == 2) {
            const auto ref = ref_at(body, "m_LightCookieSprite");
            if (!ref || ref->null()) {
                problems_.add("Sprite Light 2D with no sprite", ctx.where, "it lights nothing");
                return;
            }
            std::string why;
            const Sprite* found = sprite(*ref, why);
            const auto path =
                found != nullptr ? cookie_texture(*ref, *found, ctx.where) : std::nullopt;
            if (found == nullptr) {
                problems_.add("light sprite", ctx.where, why);
            }
            if (!path) {
                return;
            }
            // Unity draws the sprite at its size in units, its pivot on the light.
            const double scale = ppu_ / found->ppu;
            light["texture"] = *path;
            light["texture_scale"] = tidy(scale);
            light["offset"] = pair((0.5 - found->pivot_x) * found->width * scale,
                                   -(0.5 - found->pivot_y) * found->height * scale);
        } else {
            problems_.add(
                type == 0 ? "Parametric Light 2D" : "Light 2D of type " + std::to_string(type),
                ctx.where,
                type == 0 ? "Unity replaced parametric lights with freeform ones; make it "
                            "Freeform in Unity and import again"
                          : "Spot, Freeform, Sprite and Global lights convert");
            return;
        }
        const auto z_range = light_z_range(body, ctx.where);
        if (!z_range) {
            problems_.add("Light 2D on no sorting layer", ctx.where, "it lights nothing");
            enabled = false;
        } else {
            light["range_z_min"] = z_range->first;
            light["range_z_max"] = z_range->second;
        }
        if (!enabled) {
            light["enabled"] = false;
        }
        light["color"] = color_json({std::clamp(color[0], 0.0, 1.0), std::clamp(color[1], 0.0, 1.0),
                                     std::clamp(color[2], 0.0, 1.0), 1.0});
        light["energy"] = tidy(intensity);
        const bool normals = version == 0 ? body.flag_at("m_UseNormalMap")
                                          : body.integer_at("m_NormalMapQuality", 2) != 2;
        if (normals) {
            light["height"] =
                tidy(std::max(0.0, body.number_at("m_NormalMapDistance", 3.0)) * ppu_);
        }
        const double shadow_strength = body.number_at("m_ShadowIntensity", 0.75);
        if (version == 0 ? shadow_strength > 0.0 : body.flag_at("m_ShadowsEnabled")) {
            light["shadow_enabled"] = true;
            if (shadow_strength < 0.995) {
                problems_.add("Light 2D shadow strength below 1", ctx.where,
                              "Godot's 2D shadows block all of a light");
            }
            const double softness =
                version < 2 ? 0.0 : std::clamp(body.number_at("m_ShadowSoftness", 0.3), 0.0, 1.0);
            if (softness > 0.0) {
                light["shadow_filter"] = "pcf13";
                light["shadow_filter_smooth"] = tidy(softness * 10.0);
            }
        }
        if (version == 0 ? body.number_at("m_LightVolumeIntensity") > 0.0
                         : body.flag_at("m_LightVolumeEnabled")) {
            problems_.add("volumetric Light 2D", ctx.where,
                          "Godot's 2D lights light what they fall on, not the air");
        }
        if (body.integer_at("m_BlendStyleIndex", 0) != 0) {
            problems_.add("Light 2D blend style", ctx.where,
                          "lights add to what they light, as URP's first blend style, Multiply, "
                          "does");
        }
        if (version == 0 ? body.flag_at("m_AlphaBlendOnOverlap")
                         : body.integer_at("m_OverlapOperation", 0) == 1) {
            problems_.add("Light 2D alpha blend overlap", ctx.where, "overlapping lights add up");
        }
        if (enabled) {
            scene_lit_ = true;
        }
        if (out.components.find("PointLight2D") == out.components.end()) {
            out.components["PointLight2D"] = std::move(light);
            return;
        }
        // An entity has one light; more on the object go on entities under it.
        const auto lights = std::ranges::count_if(
            out.added, [](const auto& added) { return added.first.starts_with("light_"); });
        json::Object added;
        added["Transform2D"] = json::Object{};
        added["PointLight2D"] = std::move(light);
        out.added.emplace_back("light_" + std::to_string(lights + 2), std::move(added));
    }

    // A global light lights everything the same, as the scene's CanvasModulate does, up to white.
    void global_light(const Node& body, const Color& color, double intensity, bool enabled,
                      const Context& ctx, Converted& out) {
        if (!enabled) {
            problems_.add("disabled Global Light 2D", ctx.where,
                          "converted as no global light; gameplay code can add a CanvasModulate");
            return;
        }
        if (scene_global_) {
            problems_.add("second Global Light 2D", ctx.where,
                          "a scene has one CanvasModulate, the first global light's");
            return;
        }
        scene_global_ = true;
        if (sorting_layers_.size() > 1 &&
            sorting_ranks(body.get("m_ApplyToSortingLayers")).size() < sorting_layers_.size()) {
            problems_.add("Global Light 2D on some sorting layers", ctx.where,
                          "CanvasModulate tints all of them");
        }
        Color ambient{0.0, 0.0, 0.0, 1.0};
        bool brighter = false;
        for (std::size_t i = 0; i < 3; ++i) {
            const double value = color[i] * intensity;
            brighter = brighter || value > 1.0 + 1e-6;
            ambient[i] = std::clamp(value, 0.0, 1.0);
        }
        if (brighter) {
            problems_.add("Global Light 2D brighter than white", ctx.where,
                          "CanvasModulate lights things up to their own colors");
        }
        json::Value modulate;
        modulate["color"] = color_json(ambient);
        out.components["CanvasModulate"] = std::move(modulate);
    }

    // A ShadowCaster2D as Godot's LightOccluder2D: its own shape, or the shape of the collider or
    // sprite it takes its shape from.
    void shadow_caster(const Component& component, const Context& ctx, Converted& out) {
        const Node& body = component.body;
        if (!body.flag_at("m_Enabled", true)) {
            problems_.add("disabled ShadowCaster2D", ctx.where,
                          "converted as no occluder, since occluders can't be turned off");
            return;
        }
        if (const Node* option = body.get("m_CastingOption"); option != nullptr) {
            const std::int64_t casting = option->integer().value_or(1);
            if (casting == 3) {
                return; // casts no shadow
            }
            if (casting == 0) {
                problems_.add("ShadowCaster2D that shadows only itself", ctx.where,
                              "Godot's occluders shadow what is behind them");
                return;
            }
        } else if (!body.flag_at("m_CastsShadows", true)) {
            return;
        }
        std::vector<std::pair<Outline, bool>> outlines; // in pixels, y down; closed or not
        const std::int64_t source = body.integer_at("m_ShadowCastingSource", -1);
        if (source == 0) {
            return;
        }
        if (source == 2) {
            const Component* provider = component_at(*ctx.file, body, "m_ShadowShape2DComponent");
            if (provider != nullptr) {
                outlines = provider_outlines(*provider, ctx.where);
            } else {
                problems_.add("ShadowCaster2D shape provider", ctx.where,
                              "it names no component; the occluder takes the caster's own shape");
            }
        }
        if (outlines.empty()) {
            Outline points;
            if (const Node* path = body.get("m_ShapePath"); path != nullptr) {
                for (const Node& p : path->items) {
                    points.push_back({p.number_at("x") * ppu_, -p.number_at("y") * ppu_});
                }
            }
            if (points.size() >= 2) {
                outlines.emplace_back(std::move(points), true);
            }
        }
        if (outlines.empty()) {
            problems_.add("ShadowCaster2D with no shape", ctx.where, "it casts no shadow");
            return;
        }
        int extra = 0;
        for (auto& [points, closed] : outlines) {
            json::Array polygon;
            for (const auto& [x, y] : points) {
                polygon.push_back(pair(x, y));
            }
            json::Value occluder;
            occluder["polygon"] = std::move(polygon);
            if (!closed) {
                occluder["closed"] = false;
            }
            if (out.components.find("LightOccluder2D") == out.components.end()) {
                out.components["LightOccluder2D"] = std::move(occluder);
                continue;
            }
            json::Object added;
            added["Transform2D"] = json::Object{};
            added["LightOccluder2D"] = std::move(occluder);
            out.added.emplace_back("occluder_" + std::to_string(extra++ + 2), std::move(added));
        }
    }

    // The outlines of the collider or sprite a shadow caster takes its shape from, in pixels with
    // y down, each closed or not.
    std::vector<std::pair<Outline, bool>> provider_outlines(const Component& provider,
                                                            const std::string& where) {
        const Node& body = provider.body;
        const double ox = body.number_at("m_Offset.x");
        const double oy = body.number_at("m_Offset.y");
        const auto point = [&](double x, double y) {
            return std::array<double, 2>{(x + ox) * ppu_, -(y + oy) * ppu_};
        };
        // A circle's or capsule's round end, from one angle to another, in radians.
        const auto arc = [&](Outline& points, double cx, double cy, double radius, double from,
                             double to, int steps) {
            for (int i = 0; i <= steps; ++i) {
                const double angle = from + ((to - from) * i / steps);
                const Vec2 cs = cos_sin(static_cast<float>(angle));
                points.push_back(point(cx + (radius * cs.x), cy + (radius * cs.y)));
            }
        };
        std::vector<std::pair<Outline, bool>> out;
        switch (provider.class_id) {
        case kBoxCollider2D: {
            const double x = std::abs(body.number_at("m_Size.x", 1.0)) / 2.0;
            const double y = std::abs(body.number_at("m_Size.y", 1.0)) / 2.0;
            out.emplace_back(Outline{point(-x, y), point(x, y), point(x, -y), point(-x, -y)}, true);
            break;
        }
        case kCircleCollider2D: {
            Outline points;
            arc(points, 0.0, 0.0, std::abs(body.number_at("m_Radius", 0.5)), 0.0,
                2.0 * std::numbers::pi * 23.0 / 24.0, 23);
            out.emplace_back(std::move(points), true);
            break;
        }
        case kCapsuleCollider2D: {
            const double width = std::abs(body.number_at("m_Size.x", 0.5));
            const double height = std::abs(body.number_at("m_Size.y", 1.0));
            Outline points;
            if (body.integer_at("m_Direction", 0) == 1) {
                const double radius = height / 2.0;
                const double reach = std::max(0.0, (width / 2.0) - radius);
                arc(points, reach, 0.0, radius, -std::numbers::pi / 2.0, std::numbers::pi / 2.0, 8);
                arc(points, -reach, 0.0, radius, std::numbers::pi / 2.0,
                    3.0 * std::numbers::pi / 2.0, 8);
            } else {
                const double radius = width / 2.0;
                const double reach = std::max(0.0, (height / 2.0) - radius);
                arc(points, 0.0, reach, radius, 0.0, std::numbers::pi, 8);
                arc(points, 0.0, -reach, radius, std::numbers::pi, 2.0 * std::numbers::pi, 8);
            }
            out.emplace_back(std::move(points), true);
            break;
        }
        case kPolygonCollider2D:
        case kEdgeCollider2D: {
            const bool edge = provider.class_id == kEdgeCollider2D;
            std::vector<const Node*> paths;
            if (edge) {
                paths.push_back(body.get("m_Points"));
            } else if (const Node* all = body.path("m_Points.m_Paths"); all != nullptr) {
                for (const Node& path : all->items) {
                    paths.push_back(&path);
                }
            }
            for (const Node* path : paths) {
                Outline points;
                for (const Node& p : path != nullptr ? path->items : std::vector<Node>{}) {
                    points.push_back(point(p.number_at("x"), p.number_at("y")));
                }
                if (points.size() >= (edge ? 2U : 3U)) {
                    out.emplace_back(std::move(points), !edge);
                }
            }
            break;
        }
        case kSpriteRenderer: {
            std::string why;
            const auto ref = ref_at(body, "m_Sprite");
            const Sprite* found = ref && !ref->null() ? sprite(*ref, why) : nullptr;
            if (found == nullptr) {
                problems_.add("ShadowCaster2D shape from a sprite", where,
                              why.empty() ? "the SpriteRenderer has no sprite" : why);
                break;
            }
            // Its physics shape, around the middle of its rect; or the rect.
            const double scale = ppu_ / found->ppu;
            const double mx = (0.5 - found->pivot_x) * found->width;
            const double my = (0.5 - found->pivot_y) * found->height;
            const double flip_x = body.flag_at("m_FlipX") ? -1.0 : 1.0;
            const double flip_y = body.flag_at("m_FlipY") ? -1.0 : 1.0;
            std::vector<Outline> shapes = found->shape;
            if (shapes.empty()) {
                const double x = found->width / 2.0;
                const double y = found->height / 2.0;
                shapes.push_back({{-x, y}, {x, y}, {x, -y}, {-x, -y}});
                problems_.add("ShadowCaster2D shape from a sprite with no physics shape", where,
                              "the occluder is the sprite's rectangle");
            }
            for (const Outline& shape : shapes) {
                Outline points;
                for (const auto& [x, y] : shape) {
                    points.push_back({(x + mx) * scale * flip_x, -(y + my) * scale * flip_y});
                }
                out.emplace_back(std::move(points), true);
            }
            break;
        }
        default:
            problems_.add("ShadowCaster2D shape from " + provider.type, where,
                          "colliders' and sprites' shapes convert; the occluder takes the "
                          "caster's own shape");
            break;
        }
        return out;
    }

    // --- UI ---

    static Color color_at(const Node& body, std::string_view key, Color fallback = {1, 1, 1, 1}) {
        const Node* node = body.path(key);
        if (node == nullptr || !node->is_map()) {
            return fallback;
        }
        return {node->number_at("r", fallback[0]), node->number_at("g", fallback[1]),
                node->number_at("b", fallback[2]), node->number_at("a", fallback[3])};
    }

    // Two colors multiplied, the second scaled, as Unity tints a graphic; each part at most 1.
    static Color tinted(const Color& color, const Color& tint, double scale = 1.0) {
        Color out{};
        for (std::size_t i = 0; i < 4; ++i) {
            out[i] = std::clamp(color[i] * tint[i] * scale, 0.0, 1.0);
        }
        return out;
    }

    static double number_in(const json::Value& value, std::string_view key) {
        const json::Value* found = value.is_object() ? value.find(key) : nullptr;
        return found != nullptr && found->is_number() ? found->as_double() : 0.0;
    }

    static json::Value color_json(const Color& color) {
        return json::Array{tidy(color[0]), tidy(color[1]), tidy(color[2]), tidy(color[3])};
    }

    // The known package script a component is, or nullptr.
    static const KnownScript* package_script(const Component& component) {
        if (component.class_id != kMonoBehaviour) {
            return nullptr;
        }
        const auto script = ref_at(component.body, "m_Script");
        if (!script) {
            return nullptr;
        }
        for (const KnownScript& known : kKnownScripts) {
            if (known.guid == script->guid) {
                return &known;
            }
        }
        return nullptr;
    }

    // The known package script of a component that has lost its values, or nullptr. A project
    // recovered from a build whose scripts couldn't be read (an IL2CPP one, often) keeps only the
    // values every MonoBehaviour has; Unity writes the rest for every script that has any.
    static const KnownScript* lost_script(const Component& component) {
        const KnownScript* known = package_script(component);
        if (known == nullptr || known->no_values) {
            return nullptr;
        }
        for (const auto& [key, value] : component.body.entries) {
            if (!standard_key(key)) {
                return nullptr;
            }
        }
        return known;
    }

    // The known package script a component is, or nullptr, also when its values are lost.
    static const KnownScript* known_script(const Component& component) {
        return lost_script(component) == nullptr ? package_script(component) : nullptr;
    }

    // The object a local reference names, through the component it names, or -1.
    static int object_at(const Expanded& file, const Node& body, std::string_view key) {
        const auto ref = ref_at(body, key);
        if (!ref || ref->null() || !ref->guid.empty()) {
            return -1;
        }
        const std::int64_t id = file.resolve(ref->file_id);
        if (const auto at = file.components.find(id); at != file.components.end()) {
            return at->second.first;
        }
        if (const auto go = file.by_id.find(id); go != file.by_id.end()) {
            return go->second;
        }
        return -1;
    }

    // A component a local reference names, or nullptr.
    static const Component* component_at(const Expanded& file, const Node& body,
                                         std::string_view key) {
        const auto ref = ref_at(body, key);
        if (!ref || ref->null() || !ref->guid.empty()) {
            return nullptr;
        }
        const auto at = file.components.find(file.resolve(ref->file_id));
        if (at == file.components.end()) {
            return nullptr;
        }
        return &file.objects[static_cast<std::size_t>(at->second.first)]
                    .components[static_cast<std::size_t>(at->second.second)];
    }

    // An object's known package script of a name, or nullptr.
    static const Component* script_of(const GameObject& go, std::string_view name) {
        for (const Component& component : go.components) {
            if (const KnownScript* known = known_script(component);
                known != nullptr && known->name == name) {
                return &component;
            }
        }
        return nullptr;
    }

    // Which objects are UI: those with a RectTransform under a Canvas, and the objects of a UI
    // prefab, whose top object has a RectTransform and UI components under it.
    void mark_ui(Expanded& file) {
        const auto rect = [&](const GameObject& go) {
            const Component* transform = transform_of(go);
            return transform != nullptr && transform->class_id == kRectTransform;
        };
        std::function<bool(int)> has_ui = [&](int index) {
            const GameObject& go = file.objects[static_cast<std::size_t>(index)];
            const bool own = std::ranges::any_of(go.components, [](const Component& component) {
                const KnownScript* known = known_script(component);
                return component.class_id == kCanvas || (known != nullptr && known->ui);
            });
            return own || std::ranges::any_of(go.children, has_ui);
        };
        std::function<void(int, bool)> visit = [&](int index, bool parent_ui) {
            GameObject& go = file.objects[static_cast<std::size_t>(index)];
            // An instance's objects come from its prefab's, converted on their own before.
            go.folded.clear();
            go.rect = {};
            go.ui_ppu = 100.0;
            go.placement = {};
            const bool canvas = std::ranges::any_of(
                go.components, [](const Component& c) { return c.class_id == kCanvas; });
            go.ui = rect(go) && (canvas || parent_ui || (go.parent < 0 && has_ui(index)));
            for (const int child : go.children) {
                visit(child, go.ui);
            }
        };
        for (const int root : file.roots) {
            visit(root, false);
        }
    }

    // Marks an object and the objects under it as drawn by a control above, which they become
    // part of; the objects under them that aren't are lifted into that control's children.
    static void fold(Expanded& file, int index, const std::string& into, bool subtree) {
        if (index < 0) {
            return;
        }
        GameObject& go = file.objects[static_cast<std::size_t>(index)];
        go.folded = into;
        if (subtree) {
            for (const int child : go.children) {
                fold(file, child, into, true);
            }
        }
    }

    // Whether an object is under another, at any depth.
    static bool descends(const Expanded& file, int index, int ancestor) {
        for (int at = index >= 0 ? file.objects[static_cast<std::size_t>(index)].parent : -1;
             at >= 0; at = file.objects[static_cast<std::size_t>(at)].parent) {
            if (at == ancestor) {
                return true;
            }
        }
        return false;
    }

    // Whether an object is only a rect, with nothing of its own to draw or do.
    static bool bare(const GameObject& go) {
        return std::ranges::all_of(go.components, [](const Component& component) {
            return component.class_id == kRectTransform || component.class_id == kCanvasRenderer;
        });
    }

    // The screen space canvas's scale: what its CanvasScaler gives the project's window.
    double canvas_scale(const Component* scaler, const std::string& where) {
        if (scaler == nullptr) {
            return 1.0;
        }
        const Node& body = scaler->body;
        switch (body.integer_at("m_UiScaleMode", 0)) {
        case 0:
            return std::max(body.number_at("m_ScaleFactor", 1.0), 0.01);
        case 1: {
            const double width = std::max(body.number_at("m_ReferenceResolution.x", 800.0), 1.0);
            const double height = std::max(body.number_at("m_ReferenceResolution.y", 600.0), 1.0);
            const double across = window_width_ / width;
            const double down = window_height_ / height;
            double scale = 1.0;
            switch (body.integer_at("m_ScreenMatchMode", 0)) {
            case 1: // expand
                scale = std::min(across, down);
                break;
            case 2: // shrink
                scale = std::max(across, down);
                break;
            default: { // match width or height, in log space as Unity does
                const double match = std::clamp(body.number_at("m_MatchWidthOrHeight"), 0.0, 1.0);
                scale = std::pow(2.0, std::lerp(std::log2(across), std::log2(down), match));
                break;
            }
            }
            if (std::abs(scale - 1.0) > 1e-6) {
                problems_.add("CanvasScaler", where,
                              "scaled for the project's " + format_size(window_width_) + "x" +
                                  format_size(window_height_) +
                                  " window; at other sizes the UI keeps that scale");
            }
            return scale;
        }
        default:
            problems_.add("CanvasScaler", where,
                          "constant physical size isn't converted: the UI is drawn at 1:1");
            return 1.0;
        }
    }

    static std::string format_size(double value) {
        return std::to_string(static_cast<std::int64_t>(std::llround(value)));
    }

    // Unity's RectTransform as Control's anchors and offsets in its parent's rect; returns the
    // rect's size. Unity's rects go up from the bottom left and Control's down from the top left.
    std::array<double, 2> rect_control(const Node& rect, std::array<double, 2> parent,
                                       json::Value& control) {
        const double ax0 = rect.number_at("m_AnchorMin.x", 0.5);
        const double ay0 = rect.number_at("m_AnchorMin.y", 0.5);
        const double ax1 = rect.number_at("m_AnchorMax.x", 0.5);
        const double ay1 = rect.number_at("m_AnchorMax.y", 0.5);
        const double px = rect.number_at("m_AnchoredPosition.x");
        const double py = rect.number_at("m_AnchoredPosition.y");
        const double sx = rect.number_at("m_SizeDelta.x", 100.0);
        const double sy = rect.number_at("m_SizeDelta.y", 100.0);
        const double vx = rect.number_at("m_Pivot.x", 0.5);
        const double vy = rect.number_at("m_Pivot.y", 0.5);
        control["anchor_left"] = tidy(ax0);
        control["anchor_top"] = tidy(1.0 - ay1);
        control["anchor_right"] = tidy(ax1);
        control["anchor_bottom"] = tidy(1.0 - ay0);
        control["offset_left"] = tidy(px - (sx * vx));
        control["offset_right"] = tidy(px + (sx * (1.0 - vx)));
        control["offset_top"] = tidy(-(py + (sy * (1.0 - vy))));
        control["offset_bottom"] = tidy(-(py - (sy * vy)));
        // A rect smaller than its content grows from its pivot.
        const auto grow = [](double pivot) {
            if (pivot < 0.25) {
                return "end";
            }
            return pivot > 0.75 ? "begin" : "both";
        };
        control["grow_horizontal"] = grow(vx);
        control["grow_vertical"] = grow(1.0 - vy);
        const std::array<double, 2> size{(parent[0] * (ax1 - ax0)) + sx,
                                         (parent[1] * (ay1 - ay0)) + sy};
        const double angle = z_angle(rect);
        const double scale_x = rect.number_at("m_LocalScale.x", 1.0);
        const double scale_y = rect.number_at("m_LocalScale.y", 1.0);
        if (angle != 0.0 || scale_x != 1.0 || scale_y != 1.0) {
            control["rotation"] = tidy_angle(angle);
            control["scale"] = pair(scale_x, scale_y);
            control["pivot_offset"] = pair(vx * size[0], (1.0 - vy) * size[1]);
        }
        return size;
    }

    // Unity's rich text tags, which Label shows as text, taken out; <br> becomes a line break.
    static std::string plain_text(std::string_view text, bool& had_tags) {
        static constexpr std::array<std::string_view, 34> kTags{
            "b",         "i",      "u",      "s",           "color",       "size",      "material",
            "quad",      "font",   "sup",    "sub",         "mark",        "link",      "align",
            "alpha",     "cspace", "indent", "line-height", "line-indent", "lowercase", "uppercase",
            "smallcaps", "margin", "mspace", "noparse",     "nobr",        "page",      "pos",
            "rotate",    "space",  "sprite", "style",       "voffset",     "width"};
        std::string out;
        std::size_t at = 0;
        while (at < text.size()) {
            const std::size_t open = text.find('<', at);
            if (open == std::string_view::npos) {
                out.append(text.substr(at));
                break;
            }
            out.append(text.substr(at, open - at));
            const std::size_t close = text.find('>', open);
            if (close == std::string_view::npos) {
                out.append(text.substr(open));
                break;
            }
            std::string_view tag = text.substr(open + 1, close - open - 1);
            if (tag.starts_with('/')) {
                tag.remove_prefix(1);
            }
            const std::string_view name = tag.substr(0, tag.find_first_of("= "));
            std::string lower;
            for (const char c : name) {
                lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            if (lower == "br") {
                out += '\n';
                had_tags = true;
            } else if (name.starts_with('#') || contains(kTags, std::string_view(lower))) {
                had_tags = true;
            } else {
                out.append(text.substr(open, close - open + 1));
            }
            at = close + 1;
        }
        return out;
    }

    // A font Unity's text names: a .ttf or .otf file of the project, or "" for the built-in one.
    std::string font_path(const std::optional<ObjectRef>& ref, const std::string& where) {
        if (!ref || ref->null() || ref->guid == kBuiltinExtraGuid) {
            return {};
        }
        const Asset* asset = asset_of(ref->guid);
        if (asset != nullptr) {
            const std::string extension = extension_of(asset->path);
            if (extension == ".ttf" || extension == ".otf") {
                return destination(asset->path);
            }
            if (extension == ".asset") {
                return tmp_font(*asset, where);
            }
        }
        problems_.add("font", where, "a font from outside the project; the default font draws it");
        return {};
    }

    // A TextMesh Pro font asset's source font.
    std::string tmp_font(const Asset& asset, const std::string& where) {
        if (const auto known = tmp_fonts_.find(asset.path); known != tmp_fonts_.end()) {
            return known->second;
        }
        std::string guid;
        if (auto text = platform::read_file(source_ + asset.path); text) {
            if (auto objects = unity::parse_file(text.value()); objects) {
                for (const Object& object : objects.value()) {
                    if (const auto ref = ref_at(object.body, "m_SourceFontFile");
                        ref && !ref->guid.empty()) {
                        guid = ref->guid;
                    } else if (std::string named = object.body.string_at("m_SourceFontFileGUID");
                               !named.empty()) {
                        guid = std::move(named);
                    } else if (std::string created =
                                   object.body.string_at("m_CreationSettings.sourceFontFileGUID");
                               !created.empty()) {
                        guid = std::move(created);
                    }
                    if (!guid.empty()) {
                        break;
                    }
                }
            }
        }
        std::string path;
        if (const Asset* font = guid.empty() ? nullptr : asset_of(guid); font != nullptr) {
            path = destination(font->path);
        } else {
            problems_.add("TextMesh Pro font without its font file", where,
                          asset.path + ": the default font draws it");
        }
        tmp_fonts_[asset.path] = path;
        return path;
    }

    // Text as a Label, with its font, size and color as the control's theme overrides.
    struct TextLook {
        std::string text;
        std::string font;
        Color color{1, 1, 1, 1};
        int size = 14;
        std::string horizontal = "left";
        std::string vertical = "top";
        bool wrap = true;
    };

    // Unity's legacy Text.
    TextLook legacy_text(const Node& body, const std::string& where, json::Value& label) {
        TextLook look;
        bool tags = false;
        look.text = body.flag_at("m_FontData.m_RichText", true)
                        ? plain_text(body.string_at("m_Text"), tags)
                        : body.string_at("m_Text");
        look.font = font_path(ref_at(body, "m_FontData.m_Font"), where);
        look.color = color_at(body, "m_Color", {0.196, 0.196, 0.196, 1.0});
        look.size = static_cast<int>(body.integer_at("m_FontData.m_FontSize", 14));
        const auto alignment = static_cast<std::size_t>(
            std::clamp<std::int64_t>(body.integer_at("m_FontData.m_Alignment", 0), 0, 8));
        look.horizontal = std::array{"left", "center", "right"}[alignment % 3];
        look.vertical = std::array{"top", "center", "bottom"}[alignment / 3];
        look.wrap = body.integer_at("m_FontData.m_HorizontalOverflow", 0) == 0;
        if (body.integer_at("m_FontData.m_VerticalOverflow", 0) == 0) {
            label["clip_text"] = true;
        }
        if (tags) {
            problems_.add("rich text", where, "Labels show plain text; the tags are taken out");
        }
        if (const std::int64_t style = body.integer_at("m_FontData.m_FontStyle", 0); style != 0) {
            problems_.add("bold or italic text", where,
                          "drawn in the font's regular style; a bold or italic font file draws it");
        }
        if (body.flag_at("m_FontData.m_BestFit")) {
            problems_.add("best fit text", where, "drawn at its font size");
        }
        if (const double spacing = body.number_at("m_FontData.m_LineSpacing", 1.0);
            std::abs(spacing - 1.0) > 1e-6) {
            label["line_spacing"] =
                static_cast<std::int64_t>(std::lround((spacing - 1.0) * look.size));
        }
        return look;
    }

    // A TextMesh Pro text.
    TextLook tmp_text(const Node& body, const std::string& where, json::Value& label,
                      json::Value& control) {
        TextLook look;
        bool tags = false;
        look.text = body.flag_at("m_isRichText", true) ? plain_text(body.string_at("m_text"), tags)
                                                       : body.string_at("m_text");
        look.font = font_path(ref_at(body, "m_fontAsset"), where);
        look.color = body.get("m_fontColor") != nullptr ? color_at(body, "m_fontColor")
                                                        : color_at(body, "m_Color");
        look.size = static_cast<int>(std::lround(body.number_at("m_fontSize", 36.0)));
        std::int64_t horizontal = body.integer_at("m_HorizontalAlignment", 0);
        std::int64_t vertical = body.integer_at("m_VerticalAlignment", 0);
        if (horizontal == 0 || vertical == 0) { // before TextMesh Pro 3, one field for both
            const std::int64_t both = body.integer_at("m_textAlignment", 257);
            horizontal = both & 0xFF;
            vertical = both & 0xFF00;
        }
        // Left, center, right, justified, flush and geometry center; top, middle, bottom,
        // baseline, midline and capline.
        static const std::map<std::int64_t, std::string_view> kHorizontal{
            {2, "center"}, {4, "right"}, {8, "fill"}, {16, "fill"}, {32, "center"}};
        static const std::map<std::int64_t, std::string_view> kVertical{
            {512, "center"}, {1024, "bottom"}, {4096, "center"}};
        const auto across = kHorizontal.find(horizontal);
        look.horizontal = across != kHorizontal.end() ? across->second : "left";
        const auto down = kVertical.find(vertical);
        look.vertical = down != kVertical.end() ? down->second : "top";
        // TextMesh Pro 4's modes: no wrap, normal, preserve whitespace, preserve it without wrap.
        if (body.get("m_TextWrappingMode") != nullptr) {
            const std::int64_t mode = body.integer_at("m_TextWrappingMode");
            look.wrap = mode == 1 || mode == 2;
        } else {
            look.wrap = body.flag_at("m_enableWordWrapping", true);
        }
        switch (body.integer_at("m_overflowMode", 0)) {
        case 0:
            break;
        case 1:
            label["text_overrun_behavior"] = "trim_ellipsis";
            label["clip_text"] = true;
            break;
        case 2:
        case 3:
            label["clip_text"] = true;
            break;
        default:
            problems_.add("TextMesh Pro overflow mode", where,
                          "scroll, page and linked overflow aren't converted; the text overflows");
            break;
        }
        const std::int64_t style = body.integer_at("m_fontStyle", 0);
        if ((style & 16) != 0) {
            label["uppercase"] = true;
        }
        if ((style & ~std::int64_t{16}) != 0) {
            problems_.add("TextMesh Pro font style", where,
                          "bold, italic, underline, strikethrough, small caps and lowercase "
                          "styles aren't converted");
        }
        if (body.flag_at("m_enableAutoSizing")) {
            problems_.add("auto-sized text", where, "drawn at the size it last had");
        }
        if (body.number_at("m_characterSpacing") != 0.0 || body.number_at("m_wordSpacing") != 0.0 ||
            body.number_at("m_paragraphSpacing") != 0.0) {
            problems_.add("text spacing", where,
                          "character, word and paragraph spacing aren't converted");
        }
        if (const double spacing = body.number_at("m_lineSpacing"); spacing != 0.0) {
            label["line_spacing"] =
                static_cast<std::int64_t>(std::lround(spacing * 0.01 * look.size));
        }
        if (body.flag_at("m_isRightToLeft")) {
            problems_.add("right-to-left TextMesh Pro text", where,
                          "Labels order right-to-left text themselves");
        }
        // Margins move the text in from the rect's edges.
        const std::array<std::pair<const char*, double>, 4> margins{{
            {"offset_left", body.number_at("m_margin.x")},
            {"offset_top", body.number_at("m_margin.y")},
            {"offset_right", -body.number_at("m_margin.z")},
            {"offset_bottom", -body.number_at("m_margin.w")},
        }};
        for (const auto& [key, by] : margins) {
            if (by != 0.0) {
                control[key] = tidy(number_in(control, key) + by);
            }
        }
        if (tags) {
            problems_.add("rich text", where, "Labels show plain text; the tags are taken out");
        }
        return look;
    }

    // Writes text's look on a control: its Label's fields and theme overrides.
    static void apply_text(const TextLook& look, json::Value& label, json::Value& control,
                           std::string_view color_item = "font_color") {
        label["text"] = look.text;
        label["horizontal_alignment"] = look.horizontal;
        label["vertical_alignment"] = look.vertical;
        if (look.wrap) {
            label["autowrap_mode"] = "word_smart";
        }
        control["theme_override_font_sizes"]["font_size"] = std::int64_t{look.size};
        control["theme_override_colors"][std::string(color_item)] = color_json(look.color);
        if (!look.font.empty()) {
            control["theme_override_fonts"]["font"] = look.font;
        }
        if (label.find("line_spacing") != nullptr) {
            control["theme_override_constants"]["line_spacing"] = label["line_spacing"];
            label.as_object().erase("line_spacing");
        }
    }

    // How an Image draws: nothing, a flat rect, or its sprite.
    struct ImageLook {
        enum class Kind : std::uint8_t { None, Color, Rounded, Texture } kind = Kind::None;
        Color color{1, 1, 1, 1};
        const Sprite* sprite = nullptr;
        int type = 0; // Unity's: simple, sliced, tiled or filled
        bool fill_center = true;
        bool preserve_aspect = false;
        int radius = 0;
    };

    ImageLook image_look(const Node& body, const std::string& where, std::string_view sprite_key,
                         double canvas_ppu) {
        ImageLook look;
        look.color = color_at(body, "m_Color");
        look.type = static_cast<int>(body.integer_at("m_Type", 0));
        look.fill_center = body.flag_at("m_FillCenter", true);
        look.preserve_aspect = body.flag_at("m_PreserveAspect");
        const auto ref = ref_at(body, sprite_key);
        if (!ref || ref->null()) {
            look.kind = ImageLook::Kind::Color;
            return look;
        }
        if (ref->guid == kBuiltinSprites) {
            // Unity's built-in sprites aren't open source; its rounded ones draw as Godot's
            // rounded style boxes.
            switch (ref->file_id) {
            case kUiSprite:
            case kBackgroundSprite:
            case kInputFieldSprite:
            case kMaskSprite:
                look.kind = ImageLook::Kind::Rounded;
                look.radius = 3;
                return look;
            case kKnobSprite:
                look.kind = ImageLook::Kind::Rounded;
                look.radius = 64;
                return look;
            default:
                problems_.add("built-in UI sprite", where,
                              "Unity's checkmark and dropdown arrow sprites aren't converted");
                return look;
            }
        }
        std::string why;
        look.sprite = sprite(*ref, why);
        if (look.sprite == nullptr) {
            problems_.add("sprite", where, why);
            return look;
        }
        look.kind = ImageLook::Kind::Texture;
        if (look.type == 3) {
            problems_.add("filled Image", where, "drawn whole: fills aren't converted");
            look.type = 0;
        }
        if ((look.type == 1 || look.type == 2) &&
            std::ranges::any_of(look.sprite->border, [](double side) { return side > 0.0; })) {
            const double scale =
                canvas_ppu / (look.sprite->ppu *
                              std::max(body.number_at("m_PixelsPerUnitMultiplier", 1.0), 0.01));
            if (std::abs(scale - 1.0) > 0.01) {
                problems_.add("sliced sprite's corner size", where,
                              "its corners draw at the texture's size; Unity draws them at " +
                                  json::write(json::Value(tidy(scale))) + " times that");
            }
        } else if (look.type == 1) {
            look.type = 0; // a sliced sprite with no border draws as a simple one
        }
        if (const auto material = ref_at(body, "m_Material");
            material && !material->null() && asset_of(material->guid) != nullptr) {
            problems_.add("UI material", where, "materials and shaders don't convert");
        }
        return look;
    }

    // An Image's look as a style box, tinted.
    static json::Value image_style(const ImageLook& look, const Color& tint,
                                   const Sprite* swapped = nullptr) {
        json::Value style;
        const Color color = tinted(look.color, tint);
        const Sprite* sprite = swapped != nullptr ? swapped : look.sprite;
        if (look.kind == ImageLook::Kind::Texture && sprite != nullptr) {
            style["type"] = "texture";
            style["texture"] = sprite->texture;
            if (!sprite->whole) {
                style["region_rect"] = json::Array{sprite->region[0], sprite->region[1],
                                                   sprite->region[2], sprite->region[3]};
            }
            if (look.type == 1 || look.type == 2) {
                // Unity's borders are left, bottom, right and top.
                style["texture_margin_left"] = sprite->border[0];
                style["texture_margin_top"] = sprite->border[3];
                style["texture_margin_right"] = sprite->border[2];
                style["texture_margin_bottom"] = sprite->border[1];
                if (!look.fill_center) {
                    style["draw_center"] = false;
                }
                if (look.type == 2) {
                    style["axis_stretch_horizontal"] = "tile";
                    style["axis_stretch_vertical"] = "tile";
                }
            }
            if (color != Color{1, 1, 1, 1}) {
                style["modulate_color"] = color_json(color);
            }
            return style;
        }
        if (look.kind == ImageLook::Kind::None) {
            style["type"] = "empty";
            return style;
        }
        style["type"] = "flat";
        style["bg_color"] = color_json(color);
        if (look.radius > 0) {
            style["corner_radius"] = look.radius;
        }
        return style;
    }

    // An Image drawn on its own: a color, a rounded panel, a texture or a nine-patch.
    void draw_image(const ImageLook& look, json::Value& control, json::Object& components) const {
        switch (look.kind) {
        case ImageLook::Kind::None:
            return;
        case ImageLook::Kind::Color: {
            json::Value rect;
            rect["color"] = color_json(look.color);
            components["ColorRect"] = std::move(rect);
            return;
        }
        case ImageLook::Kind::Rounded:
            components["Panel"] = json::Object{};
            control["theme_override_styles"]["panel"] = image_style(look, {1, 1, 1, 1});
            return;
        case ImageLook::Kind::Texture:
            break;
        }
        const Sprite& sprite = *look.sprite;
        if (look.type == 0 && sprite.whole) {
            json::Value rect;
            rect["texture"] = sprite.texture;
            rect["expand_mode"] = "ignore_size";
            rect["stretch_mode"] = look.preserve_aspect ? "keep_aspect_centered" : "scale";
            components["TextureRect"] = std::move(rect);
        } else {
            json::Value patch;
            patch["texture"] = sprite.texture;
            if (!sprite.whole) {
                patch["region_rect"] = json::Array{sprite.region[0], sprite.region[1],
                                                   sprite.region[2], sprite.region[3]};
            }
            if (look.type == 1 || look.type == 2) {
                patch["patch_margin_left"] =
                    static_cast<std::int64_t>(std::lround(sprite.border[0]));
                patch["patch_margin_top"] =
                    static_cast<std::int64_t>(std::lround(sprite.border[3]));
                patch["patch_margin_right"] =
                    static_cast<std::int64_t>(std::lround(sprite.border[2]));
                patch["patch_margin_bottom"] =
                    static_cast<std::int64_t>(std::lround(sprite.border[1]));
                patch["draw_center"] = look.fill_center;
                if (look.type == 2) {
                    patch["axis_stretch_horizontal"] = "tile";
                    patch["axis_stretch_vertical"] = "tile";
                }
            }
            components["NinePatchRect"] = std::move(patch);
        }
        if (look.color != Color{1, 1, 1, 1}) {
            control["self_modulate"] = color_json(look.color);
        }
        const std::string filter = nearest_textures_ > linear_textures_ ? "nearest" : "linear";
        if (sprite.filter != filter) {
            control["texture_filter"] = sprite.filter;
        }
    }

    // A Selectable's look per state (normal, hover, pressed, disabled and focus) as style boxes:
    // Unity's color tint or sprite swap on its target graphic. Focus draws over the others, as
    // Godot's; it is the selected look, or nothing when that is the normal one.
    std::array<json::Value, 5> selectable_styles(const Node& body, const ImageLook& look,
                                                 const std::string& where) {
        const double multiplier = body.number_at("m_Colors.m_ColorMultiplier", 1.0);
        const Color white{1, 1, 1, 1};
        const auto state = [&](std::string_view key) {
            return tinted(white, color_at(body, "m_Colors." + std::string(key)), multiplier);
        };
        const auto swapped = [&](std::string_view key) -> const Sprite* {
            const auto ref = ref_at(body, "m_SpriteState." + std::string(key));
            if (!ref || ref->null() || ref->guid.empty()) {
                return nullptr;
            }
            std::string why;
            const Sprite* found = sprite(*ref, why);
            if (found == nullptr) {
                problems_.add("sprite", where, why);
            }
            return found;
        };
        json::Value empty;
        empty["type"] = "empty";
        switch (body.integer_at("m_Transition", 1)) {
        case 1: {
            const Color normal = state("m_NormalColor");
            const Color selected =
                body.get("m_Colors") != nullptr && body.path("m_Colors.m_SelectedColor") != nullptr
                    ? state("m_SelectedColor")
                    : normal;
            return {image_style(look, normal), image_style(look, state("m_HighlightedColor")),
                    image_style(look, state("m_PressedColor")),
                    image_style(look, state("m_DisabledColor")),
                    selected != normal ? image_style(look, selected) : empty};
        }
        case 2: {
            const Sprite* selected = swapped("m_SelectedSprite");
            return {image_style(look, white),
                    image_style(look, white, swapped("m_HighlightedSprite")),
                    image_style(look, white, swapped("m_PressedSprite")),
                    image_style(look, white, swapped("m_DisabledSprite")),
                    selected != nullptr ? image_style(look, white, selected) : empty};
        }
        default:
            if (body.integer_at("m_Transition", 1) == 3) {
                problems_.add("animation transition", where,
                              "states draw alike: an Animator's transition isn't converted");
            }
            return {image_style(look, white), image_style(look, white), image_style(look, white),
                    image_style(look, white), empty};
        }
    }

    // Sets a control's style boxes for the states, by the names its type gives them.
    static void set_styles(json::Value& control, const std::array<json::Value, 5>& states,
                           const std::array<std::string_view, 5>& names) {
        for (std::size_t i = 0; i < 5; ++i) {
            if (!names[i].empty()) {
                control["theme_override_styles"][std::string(names[i])] = states[i];
            }
        }
    }

    // Unity's navigation as focus: none takes focus only from clicks; explicit names the next
    // control each way.
    static void navigation(const Node& body, const Expanded& file, json::Value& control) {
        const std::int64_t mode = body.integer_at("m_Navigation.m_Mode", 3);
        if (mode == 0) {
            control["focus_mode"] = "click";
        } else if (mode == 4) {
            const std::array<std::pair<const char*, const char*>, 4> sides{{
                {"m_Navigation.m_SelectOnUp", "focus_neighbor_top"},
                {"m_Navigation.m_SelectOnDown", "focus_neighbor_bottom"},
                {"m_Navigation.m_SelectOnLeft", "focus_neighbor_left"},
                {"m_Navigation.m_SelectOnRight", "focus_neighbor_right"},
            }};
            for (const auto& [key, field] : sides) {
                if (const int other = object_at(file, body, key); other >= 0) {
                    control[field] = file.objects[static_cast<std::size_t>(other)].entity;
                }
            }
        }
    }

    // A UI event's persistent calls: a line in the task of the project script each calls, or a
    // report of a call on Unity's own components.
    void ui_calls(const Node& body, std::string_view key, std::string_view event,
                  const Context& ctx) {
        const Node* calls = body.path(std::string(key) + ".m_PersistentCalls.m_Calls");
        const GameObject& go = ctx.file->objects[static_cast<std::size_t>(ctx.index)];
        for (const Node& call : items_of(calls)) {
            const std::string method = call.string_at("m_MethodName");
            const Component* target = component_at(*ctx.file, call, "m_Target");
            const Asset* script = nullptr;
            if (target != nullptr && target->class_id == kMonoBehaviour) {
                const auto ref = ref_at(target->body, "m_Script");
                script = ref ? asset_of(ref->guid) : nullptr;
            }
            std::string argument;
            switch (call.integer_at("m_Mode", 1)) {
            case 2:
                if (const Node* object = call.path("m_Arguments.m_ObjectArgument")) {
                    argument = json::write(convert_value(*object, *ctx.file));
                }
                break;
            case 3:
                argument = std::to_string(call.integer_at("m_Arguments.m_IntArgument"));
                break;
            case 4:
                argument = json::write(json::Value(call.number_at("m_Arguments.m_FloatArgument")));
                break;
            case 5:
                argument = json::write(json::Value(call.string_at("m_Arguments.m_StringArgument")));
                break;
            case 6:
                argument = call.flag_at("m_Arguments.m_BoolArgument") ? "true" : "false";
                break;
            default:
                break;
            }
            std::string text = go.entity;
            text.append(" in ")
                .append(ctx.file->scene)
                .append(" calls ")
                .append(method)
                .append("(")
                .append(argument)
                .append(") on ")
                .append(event);
            if (script != nullptr && extension_of(script->path) == ".cs") {
                ScriptRecord& record = scripts_[script->path];
                record.calls.push_back(text);
                if (const int to = object_at(*ctx.file, call, "m_Target"); to >= 0) {
                    record.connections.push_back(
                        {.scene = ctx.file->scene,
                         .signal = std::string(event),
                         .from = go.entity,
                         .to = ctx.file->objects[static_cast<std::size_t>(to)].entity,
                         .method = method,
                         .argument = argument});
                }
            } else {
                std::string why(event);
                why.append(" calls ")
                    .append(target != nullptr ? target->type : std::string("an object"))
                    .append(".")
                    .append(method)
                    .append(": gameplay code makes the call on the control's ")
                    .append(event)
                    .append(" event");
                problems_.add("UI event call", ctx.where, why);
            }
        }
    }

    // The parts of a UI object, by the known script names of its components.
    struct UiParts {
        const Component* canvas = nullptr;
        const Component* group = nullptr; // CanvasGroup
        std::map<std::string, const Component*, std::less<>> scripts;
        std::vector<const Component*> effects; // Outline and Shadow
        [[nodiscard]] const Component* get(std::string_view name) const {
            const auto it = scripts.find(name);
            return it != scripts.end() ? it->second : nullptr;
        }
    };

    // A UI object as a control: its RectTransform as Control's anchors and offsets, its graphic
    // as what the control draws, and its selectable, layout and mask as Godot's.
    void ui_object(const Context& ctx, const UiParts& parts, Converted& out) {
        Expanded& file = *ctx.file;
        GameObject& go = file.objects[static_cast<std::size_t>(ctx.index)];
        json::Value control = json::Object{};
        const Component* transform = transform_of(go);
        const GameObject* parent =
            go.parent >= 0 ? &file.objects[static_cast<std::size_t>(go.parent)] : nullptr;
        const bool root = parent == nullptr || !parent->ui;
        if (parent != nullptr && parent->ui) {
            go.ui_ppu = parent->ui_ppu;
        }
        if (parts.canvas != nullptr && root) {
            const Node& canvas = parts.canvas->body;
            const Component* scaler = parts.get("CanvasScaler");
            if (scaler != nullptr) {
                go.ui_ppu = scaler->body.number_at("m_ReferencePixelsPerUnit", 100.0);
            }
            if (canvas.integer_at("m_RenderMode", 0) == 2) {
                problems_.add("World Space canvas", ctx.where,
                              "drawn over the screen, at its own size, like the others");
                go.rect = rect_control(transform->body, {window_width_, window_height_}, control);
            } else {
                const double scale = canvas_scale(scaler, ctx.where);
                if (std::abs(scale - 1.0) <= 1e-6) {
                    control["anchor_right"] = 1.0;
                    control["anchor_bottom"] = 1.0;
                } else {
                    control["offset_right"] = tidy(window_width_ / scale);
                    control["offset_bottom"] = tidy(window_height_ / scale);
                    control["scale"] = pair(scale, scale);
                }
                go.rect = {window_width_ / scale, window_height_ / scale};
            }
            if (const std::int64_t order = canvas.integer_at("m_SortingOrder", 0); order != 0) {
                control["z_index"] = order;
            }
        } else {
            go.rect = rect_control(transform->body,
                                   root ? std::array{window_width_, window_height_} : parent->rect,
                                   control);
            if (parts.canvas != nullptr && parts.canvas->body.flag_at("m_OverrideSorting")) {
                control["z_index"] = parts.canvas->body.integer_at("m_SortingOrder", 0);
            }
        }
        if (parts.canvas != nullptr && !parts.canvas->body.flag_at("m_Enabled", true)) {
            control["visible"] = false;
        }
        if (!go.body.flag_at("m_IsActive", true) || go.placement.hidden) {
            control["visible"] = false;
        }
        if (parts.group != nullptr) {
            const Node& group = parts.group->body;
            if (const double alpha = group.number_at("m_Alpha", 1.0); alpha < 1.0) {
                control["modulate"] = json::Array{1.0, 1.0, 1.0, tidy(alpha)};
            }
            if (!group.flag_at("m_Interactable", true)) {
                problems_.add("CanvasGroup that isn't interactable", ctx.where,
                              "the controls under it still take input; disable them instead");
            }
            if (!group.flag_at("m_BlocksRaycasts", true)) {
                control["mouse_filter"] = "ignore";
            }
        }
        // What a layout group above set.
        if (go.placement.placed) {
            control["size_flags_horizontal"] = go.placement.size_h;
            control["size_flags_vertical"] = go.placement.size_v;
            if (go.placement.minimum[0] >= 0.0 || go.placement.minimum[1] >= 0.0) {
                control["custom_minimum_size"] = pair(std::max(go.placement.minimum[0], 0.0),
                                                      std::max(go.placement.minimum[1], 0.0));
            }
        }
        json::Object& components = out.components;
        // The graphic: an Image, RawImage or text, unless a selectable draws it as its styles.
        const Component* image = parts.get("Image");
        const Component* raw = parts.get("RawImage");
        const Component* text = parts.get("Text");
        const Component* tmp = parts.get("TextMeshProUGUI");
        const Component* mask = parts.get("Mask");
        const auto enabled = [](const Component* c) {
            return c != nullptr && c->body.flag_at("m_Enabled", true);
        };
        const Component* selectable = nullptr;
        for (const std::string_view name :
             {"Button", "Toggle", "Slider", "Scrollbar", "InputField", "TMP_InputField", "Dropdown",
              "TMP_Dropdown", "ScrollRect"}) {
            if (const Component* found = parts.get(name); enabled(found)) {
                selectable = found;
                break;
            }
        }
        const std::string selectable_name =
            selectable != nullptr ? std::string(known_script(*selectable)->name) : std::string();
        ImageLook look;
        if (enabled(image)) {
            look = image_look(image->body, ctx.where, "m_Sprite", go.ui_ppu);
        }
        // Whether the selectable's target graphic is this object's Image, which it then draws.
        bool image_drawn = false;
        if (selectable != nullptr && selectable_name != "ScrollRect" && enabled(image)) {
            const Component* target = component_at(file, selectable->body, "m_TargetGraphic");
            image_drawn = target == image;
        }
        // A Mask that doesn't show its graphic draws nothing of its own.
        const bool hidden_by_mask = enabled(mask) && !mask->body.flag_at("m_ShowMaskGraphic", true);
        if (enabled(image) && !image_drawn && !hidden_by_mask) {
            draw_image(look, control, components);
        }
        if (enabled(raw)) {
            if (const auto ref = ref_at(raw->body, "m_Texture"); ref && !ref->null()) {
                const Texture& found = texture(ref->guid);
                if (found.ok) {
                    json::Value rect;
                    rect["texture"] = destination(found.path);
                    rect["expand_mode"] = "ignore_size";
                    components["TextureRect"] = std::move(rect);
                    const Color color = color_at(raw->body, "m_Color");
                    if (color != Color{1, 1, 1, 1}) {
                        control["self_modulate"] = color_json(color);
                    }
                    const Node& uv = raw->body;
                    if (uv.number_at("m_UVRect.x") != 0.0 || uv.number_at("m_UVRect.y") != 0.0 ||
                        uv.number_at("m_UVRect.width", 1.0) != 1.0 ||
                        uv.number_at("m_UVRect.height", 1.0) != 1.0) {
                        problems_.add("RawImage UV rect", ctx.where, "the whole texture draws");
                    }
                } else {
                    problems_.add("texture", ctx.where, found.why);
                }
            } else {
                json::Value rect;
                rect["color"] = color_json(color_at(raw->body, "m_Color"));
                components["ColorRect"] = std::move(rect);
            }
        }
        if (enabled(text) || enabled(tmp)) {
            json::Value label = json::Object{};
            const TextLook look_text = enabled(text)
                                           ? legacy_text(text->body, ctx.where, label)
                                           : tmp_text(tmp->body, ctx.where, label, control);
            apply_text(look_text, label, control);
            for (const Component* effect : parts.effects) {
                const Node& body = effect->body;
                if (!body.flag_at("m_Enabled", true)) {
                    continue;
                }
                const Color color = color_at(body, "m_EffectColor", {0, 0, 0, 0.5});
                const double dx = body.number_at("m_EffectDistance.x", 1.0);
                const double dy = body.number_at("m_EffectDistance.y", -1.0);
                if (known_script(*effect)->name == "Outline") {
                    // Godot's outline strokes a quarter of outline_size each side of the edge.
                    control["theme_override_colors"]["font_outline_color"] = color_json(color);
                    control["theme_override_constants"]["outline_size"] = static_cast<std::int64_t>(
                        std::lround(4.0 * std::max(std::abs(dx), std::abs(dy))));
                } else {
                    control["theme_override_colors"]["font_shadow_color"] = color_json(color);
                    control["theme_override_constants"]["shadow_offset_x"] =
                        static_cast<std::int64_t>(std::lround(dx));
                    control["theme_override_constants"]["shadow_offset_y"] =
                        static_cast<std::int64_t>(std::lround(-dy));
                }
            }
            if (!(text != nullptr ? text : tmp)->body.flag_at("m_RaycastTarget", true)) {
                control["mouse_filter"] = "ignore";
            }
            components["Label"] = std::move(label);
        } else if (!parts.effects.empty()) {
            problems_.add("Outline or Shadow on an image", ctx.where, "only text takes them");
        }
        // A graphic that takes the pointer passes it on to the controls it is part of, as
        // Unity sends a click to the button above what was clicked.
        const Component* graphic = enabled(raw) ? raw : nullptr;
        if (enabled(image) && !image_drawn) {
            graphic = image;
        }
        if (graphic != nullptr && control.find("mouse_filter") == nullptr) {
            control["mouse_filter"] =
                graphic->body.flag_at("m_RaycastTarget", true) ? "pass" : "ignore";
        }
        if (selectable_name == "Button") {
            ui_button(ctx, *selectable, look, image_drawn, control, out);
        } else if (selectable_name == "Toggle") {
            ui_toggle(ctx, *selectable, look, image_drawn, control, out);
        } else if (selectable_name == "Slider") {
            ui_slider(ctx, *selectable, control, out);
        } else if (selectable_name == "Scrollbar") {
            ui_scrollbar(ctx, *selectable, look, image_drawn, control, out);
        } else if (selectable_name == "InputField" || selectable_name == "TMP_InputField") {
            ui_input(ctx, *selectable, selectable_name == "TMP_InputField", look, image_drawn,
                     control, out);
        } else if (selectable_name == "ScrollRect") {
            ui_scroll(ctx, *selectable, out);
        } else if (selectable_name == "Dropdown" || selectable_name == "TMP_Dropdown") {
            ui_dropdown(ctx, *selectable, selectable_name == "TMP_Dropdown", look, image_drawn,
                        control, out);
        }
        if ((enabled(mask) || enabled(parts.get("RectMask2D")))) {
            control["clip_contents"] = true;
        }
        ui_layout(ctx, parts, control, out);
        out.components["Control"] = std::move(control);
    }

    void ui_button(const Context& ctx, const Component& button, const ImageLook& look,
                   bool image_drawn, json::Value& control, Converted& out) {
        const Node& body = button.body;
        json::Value fields = json::Object{};
        if (image_drawn) {
            const auto states = selectable_styles(body, look, ctx.where);
            set_styles(control, states, {"normal", "hover", "pressed", "disabled", "focus"});
            control["theme_override_styles"]["hover_pressed"] = states[2];
        } else {
            fields["flat"] = true;
            if (body.integer_at("m_Transition", 1) != 0 &&
                component_at(*ctx.file, body, "m_TargetGraphic") != nullptr) {
                problems_.add("button tint on another object", ctx.where,
                              "the graphic it tints draws as it starts");
            }
        }
        if (!body.flag_at("m_Interactable", true)) {
            fields["disabled"] = true;
        }
        navigation(body, *ctx.file, control);
        ui_calls(body, "m_OnClick", "pressed", ctx);
        out.components["Button"] = std::move(fields);
    }

    void ui_toggle(const Context& ctx, const Component& toggle, const ImageLook& look,
                   bool image_drawn, json::Value& control, Converted& out) {
        Expanded& file = *ctx.file;
        const Node& body = toggle.body;
        json::Value fields = json::Object{};
        fields["button_pressed"] = body.flag_at("m_IsOn");
        if (!body.flag_at("m_Interactable", true)) {
            fields["disabled"] = true;
        }
        if (const int group = object_at(file, body, "m_Group"); group >= 0) {
            fields["button_group"] = file.objects[static_cast<std::size_t>(group)].entity;
            if (const Component* found =
                    script_of(file.objects[static_cast<std::size_t>(group)], "ToggleGroup");
                found != nullptr && found->body.flag_at("m_AllowSwitchOff")) {
                problems_.add("ToggleGroup that allows switching off", ctx.where,
                              "one button of a group stays pressed");
            }
        }
        navigation(body, file, control);
        ui_calls(body, "onValueChanged", "toggled", ctx);
        // Unity's default toggle, its built-in box and checkmark, is Godot's CheckBox.
        const int graphic = object_at(file, body, "graphic");
        const int target = object_at(file, body, "m_TargetGraphic");
        const auto builtin = [&](int index, std::int64_t id) {
            if (index < 0) {
                return false;
            }
            const Component* image =
                script_of(file.objects[static_cast<std::size_t>(index)], "Image");
            const auto ref = image != nullptr ? ref_at(image->body, "m_Sprite") : std::nullopt;
            return ref && ref->guid == kBuiltinSprites && ref->file_id == id;
        };
        if (builtin(graphic, kCheckmarkSprite) &&
            (builtin(target, kUiSprite) || builtin(target, kBackgroundSprite)) &&
            descends(file, graphic, ctx.index) && descends(file, target, ctx.index)) {
            const std::string into = file.objects[static_cast<std::size_t>(ctx.index)].entity;
            fold(file, target, into, false);
            fold(file, graphic, into, false);
            out.components["CheckBox"] = std::move(fields);
            return;
        }
        fields["toggle_mode"] = true;
        if (image_drawn) {
            // A toggle's background tints while hovered and pressed, not while it is on.
            const auto states = selectable_styles(body, look, ctx.where);
            set_styles(control, states, {"normal", "hover", "", "disabled", "focus"});
            control["theme_override_styles"]["pressed"] = states[0];
            control["theme_override_styles"]["hover_pressed"] = states[1];
        } else {
            fields["flat"] = true;
        }
        if (graphic >= 0) {
            file.objects[static_cast<std::size_t>(graphic)].placement.hidden =
                !body.flag_at("m_IsOn");
            problems_.add("Toggle with its own sprites", ctx.where,
                          "its checkmark shows as it starts: show and hide it from gameplay code "
                          "on the toggled event");
        }
        out.components["Button"] = std::move(fields);
    }

    // The objects between a part and the selectable it belongs to, when they hold nothing else.
    static void fold_part(Expanded& file, int part, int owner, const std::string& into) {
        if (!descends(file, part, owner)) {
            return;
        }
        for (int at = part; at >= 0 && at != owner;
             at = file.objects[static_cast<std::size_t>(at)].parent) {
            if (at != part && !bare(file.objects[static_cast<std::size_t>(at)])) {
                break;
            }
            fold(file, at, into, at == part);
        }
    }

    // A part's Image as a style box, or nullopt.
    std::optional<json::Value> part_style(const Expanded& file, int part, const std::string& where,
                                          double canvas_ppu, const Color& tint = {1, 1, 1, 1}) {
        if (part < 0) {
            return std::nullopt;
        }
        const Component* image = script_of(file.objects[static_cast<std::size_t>(part)], "Image");
        if (image == nullptr || !image->body.flag_at("m_Enabled", true)) {
            return std::nullopt;
        }
        return image_style(image_look(image->body, where, "m_Sprite", canvas_ppu), tint);
    }

    void ui_slider(const Context& ctx, const Component& slider, json::Value& control,
                   Converted& out) {
        Expanded& file = *ctx.file;
        GameObject& go = file.objects[static_cast<std::size_t>(ctx.index)];
        const Node& body = slider.body;
        json::Value fields = json::Object{};
        fields["min_value"] = body.number_at("m_MinValue");
        fields["max_value"] = body.number_at("m_MaxValue", 1.0);
        fields["value"] = body.number_at("m_Value");
        fields["step"] = body.flag_at("m_WholeNumbers") ? 1.0 : 0.0;
        const std::int64_t direction = body.integer_at("m_Direction", 0);
        if (direction >= 2) {
            fields["vertical"] = true;
        }
        if (direction == 1 || direction == 3) {
            problems_.add("reversed Slider", ctx.where,
                          "it fills left to right, or bottom to top, as Godot's");
        }
        if (!body.flag_at("m_Interactable", true)) {
            fields["editable"] = false;
        }
        navigation(body, file, control);
        ui_calls(body, "m_OnValueChanged", "value_changed", ctx);
        // Its parts become its theme: the background its slider style, the fill its grabber
        // area and the handle its grabber icon.
        const int fill = object_at(file, body, "m_FillRect");
        const int handle = object_at(file, body, "m_HandleRect");
        int background = -1;
        for (const int child : go.children) {
            const GameObject& c = file.objects[static_cast<std::size_t>(child)];
            if (c.body.string_at("m_Name") == "Background" && script_of(c, "Image") != nullptr) {
                background = child;
            }
        }
        // Godot draws the track as thick as its style's minimum size: the background's
        // thickness, as content margins either side.
        const bool vertical = direction >= 2;
        const auto thick = [&](json::Value style) {
            if (background >= 0) {
                const double half = child_size(file, background, go.rect)[vertical ? 0 : 1] / 2.0;
                style[vertical ? "content_margin_left" : "content_margin_top"] = tidy(half);
                style[vertical ? "content_margin_right" : "content_margin_bottom"] = tidy(half);
            }
            return style;
        };
        json::Value& styles = control["theme_override_styles"];
        if (auto style = part_style(file, background, ctx.where, go.ui_ppu)) {
            styles["slider"] = thick(std::move(*style));
            fold(file, background, go.entity, true);
        }
        if (auto style = part_style(file, fill, ctx.where, go.ui_ppu)) {
            styles["grabber_area"] = thick(*style);
            styles["grabber_area_highlight"] = thick(std::move(*style));
        }
        if (handle >= 0) {
            const Component* image =
                script_of(file.objects[static_cast<std::size_t>(handle)], "Image");
            const auto ref = image != nullptr ? ref_at(image->body, "m_Sprite") : std::nullopt;
            if (ref && !ref->null() && ref->guid != kBuiltinSprites) {
                std::string why;
                const Sprite* found = sprite(*ref, why);
                if (found != nullptr && found->whole) {
                    control["theme_override_icons"]["grabber"] = found->texture;
                    control["theme_override_icons"]["grabber_highlight"] = found->texture;
                } else {
                    problems_.add("Slider handle", ctx.where,
                                  found != nullptr
                                      ? "a sprite cut from a sheet; the default grabber draws"
                                      : why);
                }
            }
        }
        fold_part(file, fill, ctx.index, go.entity);
        fold_part(file, handle, ctx.index, go.entity);
        out.components["Slider"] = std::move(fields);
    }

    void ui_scrollbar(const Context& ctx, const Component& bar, const ImageLook& look,
                      bool image_drawn, json::Value& control, Converted& out) {
        Expanded& file = *ctx.file;
        const GameObject& go = file.objects[static_cast<std::size_t>(ctx.index)];
        const Node& body = bar.body;
        json::Value fields = json::Object{};
        const double size = std::clamp(body.number_at("m_Size", 0.2), 0.0, 1.0);
        const double value = std::clamp(body.number_at("m_Value"), 0.0, 1.0);
        const std::int64_t direction = body.integer_at("m_Direction", 0);
        // Unity's value goes from 0 to 1 whatever the size; Godot's from min to max - page. A
        // vertical bar's 1 is at the top, Godot's 0.
        const bool reversed = direction == 1 || direction == 2;
        fields["max_value"] = 1.0;
        fields["page"] = tidy(size);
        fields["value"] = tidy((reversed ? 1.0 - value : value) * (1.0 - size));
        if (direction >= 2) {
            fields["vertical"] = true;
        }
        if (direction == 1) {
            problems_.add("reversed Scrollbar", ctx.where, "it scrolls left to right");
        }
        if (const std::int64_t steps = body.integer_at("m_NumberOfSteps", 0); steps > 1) {
            fields["step"] = tidy((1.0 - size) / static_cast<double>(steps - 1));
        }
        navigation(body, file, control);
        ui_calls(body, "m_OnValueChanged", "value_changed", ctx);
        if (image_drawn) {
            control["theme_override_styles"]["scroll"] = image_style(look, {1, 1, 1, 1});
        }
        const int handle = object_at(file, body, "m_HandleRect");
        if (handle >= 0) {
            const Component* image =
                script_of(file.objects[static_cast<std::size_t>(handle)], "Image");
            if (image != nullptr) {
                const ImageLook handle_look =
                    image_look(image->body, ctx.where, "m_Sprite", go.ui_ppu);
                set_styles(control, selectable_styles(body, handle_look, ctx.where),
                           {"grabber", "grabber_highlight", "grabber_pressed", "", ""});
            }
        }
        fold_part(file, handle, ctx.index, go.entity);
        out.components["ScrollBar"] = std::move(fields);
    }

    void ui_input(const Context& ctx, const Component& input, bool tmp, const ImageLook& look,
                  bool image_drawn, json::Value& control, Converted& out) {
        Expanded& file = *ctx.file;
        const GameObject& go = file.objects[static_cast<std::size_t>(ctx.index)];
        const Node& body = input.body;
        const bool lines = body.integer_at("m_LineType", 0) == 2;
        json::Value fields = json::Object{};
        fields["text"] = body.string_at("m_Text");
        if (!body.flag_at("m_Interactable", true) || body.flag_at("m_ReadOnly")) {
            fields["editable"] = false;
        }
        const int text_object = object_at(file, body, "m_TextComponent");
        const int placeholder = object_at(file, body, "m_Placeholder");
        // The text's look is the field's font, size and color.
        if (text_object >= 0) {
            const GameObject& t = file.objects[static_cast<std::size_t>(text_object)];
            json::Value label = json::Object{};
            json::Value ignored = json::Object{};
            if (const Component* legacy = script_of(t, "Text")) {
                const TextLook text_look = legacy_text(legacy->body, ctx.where, label);
                apply_text(text_look, label, control);
                if (!lines) {
                    fields["alignment"] = text_look.horizontal;
                }
            } else if (const Component* pro = script_of(t, "TextMeshProUGUI")) {
                const TextLook text_look = tmp_text(pro->body, ctx.where, label, ignored);
                apply_text(text_look, label, control);
                if (!lines) {
                    fields["alignment"] = text_look.horizontal;
                }
            }
        }
        if (placeholder >= 0) {
            const GameObject& p = file.objects[static_cast<std::size_t>(placeholder)];
            json::Value label = json::Object{};
            json::Value ignored = json::Object{};
            std::optional<TextLook> found;
            if (const Component* legacy = script_of(p, "Text")) {
                found = legacy_text(legacy->body, ctx.where, label);
            } else if (const Component* pro = script_of(p, "TextMeshProUGUI")) {
                found = tmp_text(pro->body, ctx.where, label, ignored);
            }
            if (found) {
                fields["placeholder_text"] = found->text;
                control["theme_override_colors"]["font_placeholder_color"] =
                    color_json(found->color);
            }
        }
        if (!lines) {
            if (const std::int64_t limit = body.integer_at("m_CharacterLimit", 0); limit > 0) {
                fields["max_length"] = limit;
            }
            const std::int64_t content = body.integer_at("m_ContentType", 0);
            if (content == 7 || content == 8 || body.integer_at("m_InputType", 0) == 2) {
                fields["secret"] = true;
            }
            static const std::map<std::int64_t, std::string_view> kContent{{2, "number"},
                                                                           {3, "number_decimal"},
                                                                           {6, "email_address"},
                                                                           {7, "password"},
                                                                           {8, "number"}};
            static const std::map<std::int64_t, std::string_view> kKeyboards{
                {2, "number"}, {3, "url"},           {4, "number"},
                {5, "phone"},  {7, "email_address"}, {8, "number_decimal"}};
            if (const auto it = kContent.find(content); it != kContent.end()) {
                fields["virtual_keyboard_type"] = std::string(it->second);
            } else if (const auto keyboard = kKeyboards.find(body.integer_at("m_KeyboardType", 0));
                       keyboard != kKeyboards.end()) {
                fields["virtual_keyboard_type"] = std::string(keyboard->second);
            }
            if (content == 2 || content == 3 || content == 4 || content == 5 || content == 6 ||
                body.integer_at("m_CharacterValidation", 0) != 0) {
                problems_.add("input validation", ctx.where,
                              "any text can be typed; gameplay code checks it on text_changed");
            }
        } else if (body.integer_at("m_CharacterLimit", 0) > 0) {
            problems_.add("character limit on several lines", ctx.where,
                          "a TextEdit takes text of any length");
        }
        if (const double rate = body.number_at("m_CaretBlinkRate", 0.85); rate > 0.0) {
            fields["caret_blink"] = true;
            fields["caret_blink_interval"] = tidy(0.5 / rate);
        }
        // Its styles, with the text's insets as their content margins so the text sits where
        // Unity's does; with no image, nothing draws behind the text, as in Unity.
        std::array<json::Value, 5> states;
        if (image_drawn) {
            states = selectable_styles(body, look, ctx.where);
        } else {
            for (json::Value& state : states) {
                state["type"] = "empty";
            }
        }
        if (const auto inset = text_inset(file, text_object, ctx.index, go.rect)) {
            const std::array<const char*, 4> sides{"content_margin_left", "content_margin_top",
                                                   "content_margin_right", "content_margin_bottom"};
            for (json::Value& state : states) {
                for (std::size_t i = 0; i < 4; ++i) {
                    state[sides[i]] = tidy(std::max((*inset)[i], 0.0));
                }
            }
        }
        set_styles(control, states, {"normal", "", "", "read_only", "focus"});
        navigation(body, file, control);
        ui_calls(body, "m_OnValueChanged", "text_changed", ctx);
        ui_calls(body, "m_OnEndEdit", "text_submitted", ctx);
        ui_calls(body, "m_OnSubmit", "text_submitted", ctx);
        // Its text, placeholder and the area holding them are part of the field.
        fold_part(file, text_object, ctx.index, go.entity);
        fold_part(file, placeholder, ctx.index, go.entity);
        if (tmp) {
            const int viewport = object_at(file, body, "m_TextViewport");
            if (descends(file, viewport, ctx.index)) {
                fold(file, viewport, go.entity, true);
            }
            if (const int bar = object_at(file, body, "m_VerticalScrollbar"); bar >= 0) {
                problems_.add("input field scroll bar", ctx.where, "a TextEdit draws its own");
                if (descends(file, bar, ctx.index)) {
                    fold(file, bar, go.entity, true);
                }
            }
        }
        out.components[lines ? "TextEdit" : "LineEdit"] = std::move(fields);
    }

    // A Dropdown or TMP_Dropdown is Godot's OptionButton: its options and the one shown, its
    // background's styles and its caption's look. Its caption, arrow and template are part of
    // it; its list draws with the PopupMenu theme.
    void ui_dropdown(const Context& ctx, const Component& dropdown, bool tmp, const ImageLook& look,
                     bool image_drawn, json::Value& control, Converted& out) {
        Expanded& file = *ctx.file;
        const GameObject& go = file.objects[static_cast<std::size_t>(ctx.index)];
        const Node& body = dropdown.body;
        json::Value fields = json::Object{};
        json::Array items;
        if (const Node* options = body.path("m_Options.m_Options");
            options != nullptr && options->is_seq()) {
            for (const Node& option : options->items) {
                json::Value item = json::Object{};
                if (std::string text = option.string_at("m_Text"); !text.empty()) {
                    item["text"] = std::move(text);
                }
                if (const auto ref = ref_at(option, "m_Image"); ref && !ref->null()) {
                    std::string why;
                    const Sprite* found = sprite(*ref, why);
                    if (found != nullptr && found->whole) {
                        item["icon"] = found->texture;
                    } else {
                        problems_.add("Dropdown option image", ctx.where,
                                      found != nullptr
                                          ? "a sprite cut from a sheet; the option shows none"
                                          : why);
                    }
                }
                items.push_back(std::move(item));
            }
        }
        // Unity keeps the value in the options' range.
        const auto count = static_cast<std::int64_t>(items.size());
        fields["selected"] =
            count > 0 ? std::clamp(body.integer_at("m_Value", 0), std::int64_t{0}, count - 1)
                      : std::int64_t{-1};
        fields["items"] = std::move(items);
        if (!body.flag_at("m_Interactable", true)) {
            fields["disabled"] = true;
        }
        // The caption's look is the button's font, size and color.
        const int caption = object_at(file, body, "m_CaptionText");
        if (caption >= 0) {
            const GameObject& c = file.objects[static_cast<std::size_t>(caption)];
            json::Value label = json::Object{};
            json::Value ignored = json::Object{};
            std::optional<TextLook> text_look;
            if (const Component* legacy = script_of(c, "Text")) {
                text_look = legacy_text(legacy->body, ctx.where, label);
            } else if (const Component* pro = script_of(c, "TextMeshProUGUI")) {
                text_look = tmp_text(pro->body, ctx.where, label, ignored);
            }
            if (text_look) {
                apply_text(*text_look, label, control);
                fields["alignment"] = text_look->horizontal;
            }
        }
        if (image_drawn) {
            const auto states = selectable_styles(body, look, ctx.where);
            set_styles(control, states, {"normal", "hover", "pressed", "disabled", "focus"});
            control["theme_override_styles"]["hover_pressed"] = states[2];
        } else {
            fields["flat"] = true;
        }
        navigation(body, file, control);
        ui_calls(body, "m_OnValueChanged", "item_selected", ctx);
        // Its caption and caption image show the option picked, its arrow is OptionButton's
        // and its template is the list it opens.
        fold_part(file, caption, ctx.index, go.entity);
        fold_part(file, object_at(file, body, "m_CaptionImage"), ctx.index, go.entity);
        if (tmp) {
            fold_part(file, object_at(file, body, "m_Placeholder"), ctx.index, go.entity);
        }
        if (const int list = object_at(file, body, "m_Template"); descends(file, list, ctx.index)) {
            fold(file, list, go.entity, true);
            problems_.add("Dropdown template", ctx.where,
                          "its list draws with the PopupMenu theme, not the template's look");
        }
        for (const int child : go.children) {
            const GameObject& c = file.objects[static_cast<std::size_t>(child)];
            const Component* image = script_of(c, "Image");
            if (c.body.string_at("m_Name") != "Arrow" || image == nullptr || !c.children.empty()) {
                continue;
            }
            // A sprite of the project's own replaces the arrow; Unity's built-in one is
            // OptionButton's.
            if (const auto ref = ref_at(image->body, "m_Sprite");
                ref && !ref->null() && ref->guid != kBuiltinSprites) {
                std::string why;
                if (const Sprite* found = sprite(*ref, why); found != nullptr && found->whole) {
                    control["theme_override_icons"]["arrow"] = found->texture;
                }
            }
            fold(file, child, go.entity, true);
        }
        out.components["OptionButton"] = std::move(fields);
    }

    void ui_scroll(const Context& ctx, const Component& scroll, Converted& out) {
        Expanded& file = *ctx.file;
        const GameObject& go = file.objects[static_cast<std::size_t>(ctx.index)];
        const Node& body = scroll.body;
        json::Value fields = json::Object{};
        const auto mode = [&](bool on, std::string_view bar, std::string_view visibility) {
            if (!on) {
                return "disabled";
            }
            if (object_at(file, body, bar) < 0) {
                return "never";
            }
            return body.integer_at(visibility, 0) == 0 ? "always" : "auto";
        };
        fields["horizontal_scroll_mode"] =
            mode(body.flag_at("m_Horizontal", true), "m_HorizontalScrollbar",
                 "m_HorizontalScrollbarVisibility");
        fields["vertical_scroll_mode"] =
            mode(body.flag_at("m_Vertical", true), "m_VerticalScrollbar",
                 "m_VerticalScrollbarVisibility");
        ui_calls(body, "m_OnValueChanged", "value_changed", ctx);
        // The viewport is the container's own rect, and its scroll bars its own; the content
        // becomes its child.
        for (const char* bar : {"m_HorizontalScrollbar", "m_VerticalScrollbar"}) {
            if (const int index = object_at(file, body, bar); descends(file, index, ctx.index)) {
                fold(file, index, go.entity, true);
            }
        }
        const int content = object_at(file, body, "m_Content");
        const int viewport = object_at(file, body, "m_Viewport");
        if (descends(file, viewport, ctx.index) && viewport != content) {
            fold(file, viewport, go.entity, false);
        }
        if (descends(file, content, ctx.index)) {
            GameObject& c = file.objects[static_cast<std::size_t>(content)];
            c.placement.placed = true;
            c.placement.size_h = body.flag_at("m_Horizontal", true) ? "fill" : "expand_fill";
            c.placement.size_v = body.flag_at("m_Vertical", true) ? "fill" : "expand_fill";
            c.placement.minimum = {-1.0, -1.0};
            c.placement.from_rect = true;
        }
        out.components["ScrollContainer"] = std::move(fields);
    }

    // A layout group as a container, a LayoutElement as minimum sizes and size flags, and a
    // ContentSizeFitter as a rect that grows to its content.
    void ui_layout(const Context& ctx, const UiParts& parts, json::Value& control, Converted& out) {
        Expanded& file = *ctx.file;
        GameObject& go = file.objects[static_cast<std::size_t>(ctx.index)];
        const auto enabled = [](const Component* c) {
            return c != nullptr && c->body.flag_at("m_Enabled", true);
        };
        // A content's minimum size from its rect, when its scroll view places it.
        if (go.placement.from_rect && !enabled(parts.get("ContentSizeFitter"))) {
            control["custom_minimum_size"] = pair(tidy(go.rect[0]), tidy(go.rect[1]));
        }
        if (const Component* element = parts.get("LayoutElement"); enabled(element)) {
            const Node& body = element->body;
            if (body.flag_at("m_IgnoreLayout")) {
                problems_.add("LayoutElement that ignores layout", ctx.where,
                              "its container places it with the others");
            }
            if (go.placement.placed) {
                std::array<double, 2> minimum = go.placement.minimum;
                const std::array<std::pair<const char*, const char*>, 2> keys{
                    {{"m_MinWidth", "m_PreferredWidth"}, {"m_MinHeight", "m_PreferredHeight"}}};
                for (std::size_t axis = 0; axis < 2; ++axis) {
                    const double size = std::max(body.number_at(keys[axis].first, -1.0),
                                                 body.number_at(keys[axis].second, -1.0));
                    if (size >= 0.0) {
                        minimum[axis] = size;
                    }
                }
                if (minimum[0] >= 0.0 || minimum[1] >= 0.0) {
                    control["custom_minimum_size"] =
                        pair(std::max(minimum[0], 0.0), std::max(minimum[1], 0.0));
                }
                const double flexible_w = body.number_at("m_FlexibleWidth", -1.0);
                const double flexible_h = body.number_at("m_FlexibleHeight", -1.0);
                if (flexible_w > 0.0) {
                    control["size_flags_horizontal"] = "expand_fill";
                }
                if (flexible_h > 0.0) {
                    control["size_flags_vertical"] = "expand_fill";
                }
                const double ratio = go.placement.vertical ? flexible_h : flexible_w;
                if (ratio > 0.0 && std::abs(ratio - 1.0) > 1e-6) {
                    control["size_flags_stretch_ratio"] = tidy(ratio);
                }
            }
        }
        if (const Component* fitter = parts.get("ContentSizeFitter"); enabled(fitter)) {
            // Godot's controls are at least their content's minimum size: a rect of no size at
            // the pivot grows to it.
            const Node& rect = transform_of(go)->body;
            const double vx = rect.number_at("m_Pivot.x", 0.5);
            const double vy = rect.number_at("m_Pivot.y", 0.5);
            if (fitter->body.integer_at("m_HorizontalFit", 0) != 0) {
                const double left = number_in(control, "offset_left");
                const double right = number_in(control, "offset_right");
                const double at = left + ((right - left) * vx);
                control["offset_left"] = tidy(at);
                control["offset_right"] = tidy(at);
            }
            if (fitter->body.integer_at("m_VerticalFit", 0) != 0) {
                const double top = number_in(control, "offset_top");
                const double bottom = number_in(control, "offset_bottom");
                const double at = top + ((bottom - top) * (1.0 - vy));
                control["offset_top"] = tidy(at);
                control["offset_bottom"] = tidy(at);
            }
        }
        if (enabled(parts.get("AspectRatioFitter"))) {
            problems_.add("component AspectRatioFitter", ctx.where, "the control keeps its size");
        }
        const Component* horizontal = parts.get("HorizontalLayoutGroup");
        const Component* vertical = parts.get("VerticalLayoutGroup");
        const Component* grid = parts.get("GridLayoutGroup");
        const Component* group = nullptr;
        for (const Component* each : {horizontal, vertical, grid}) { // Unity allows one
            if (group == nullptr && enabled(each)) {
                group = each;
            }
        }
        if (group == nullptr) {
            return;
        }
        const Node& body = group->body;
        const std::array<double, 4> padding{
            body.number_at("m_Padding.m_Left"), body.number_at("m_Padding.m_Top"),
            body.number_at("m_Padding.m_Right"), body.number_at("m_Padding.m_Bottom")};
        const auto alignment = static_cast<std::size_t>(
            std::clamp<std::int64_t>(body.integer_at("m_ChildAlignment", 0), 0, 8));
        const std::array<std::string_view, 3> ends{"begin", "center", "end"};
        const std::array<std::string_view, 3> shrinks{"shrink_begin", "shrink_center",
                                                      "shrink_end"};
        json::Value container;
        std::string type;
        std::vector<int> children;
        for (const int child : go.children) {
            const GameObject& c = file.objects[static_cast<std::size_t>(child)];
            if (!c.removed && c.ui && c.folded.empty()) {
                children.push_back(child);
            }
        }
        if (group != grid) {
            const bool down = group == vertical;
            type = "BoxContainer";
            if (down) {
                container["vertical"] = true;
            }
            container["alignment"] = std::string(down ? ends[alignment / 3] : ends[alignment % 3]);
            const bool control_w = body.flag_at("m_ChildControlWidth", true);
            const bool control_h = body.flag_at("m_ChildControlHeight", true);
            const bool expand_w = body.flag_at("m_ChildForceExpandWidth", true);
            const bool expand_h = body.flag_at("m_ChildForceExpandHeight", true);
            if (body.flag_at("m_ReverseArrangement")) {
                problems_.add("reversed layout group", ctx.where, "its children go in order");
            }
            const std::int64_t spacing = std::lround(body.number_at("m_Spacing"));
            for (const int child : children) {
                GameObject& c = file.objects[static_cast<std::size_t>(child)];
                c.placement.placed = true;
                c.placement.vertical = down;
                const std::array<double, 2> size = child_size(file, child, go.rect);
                // Along the row: expanding shares the space left; across: filling takes all of
                // it, else the child keeps its size, placed as the alignment says.
                const auto along = [&](bool expand, bool control_size, std::size_t axis) {
                    if (!control_size) {
                        c.placement.minimum[axis] = size[axis];
                    }
                    return expand ? std::string("expand_fill") : std::string("fill");
                };
                const auto across = [&](bool expand, bool control_size, std::size_t axis,
                                        std::size_t part) {
                    if (control_size && expand) {
                        return std::string("fill");
                    }
                    if (!control_size) {
                        c.placement.minimum[axis] = size[axis];
                    }
                    return std::string(shrinks[part]);
                };
                if (down) {
                    c.placement.size_v = along(expand_h, control_h, 1);
                    c.placement.size_h = across(expand_w, control_w, 0, alignment % 3);
                } else {
                    c.placement.size_h = along(expand_w, control_w, 0);
                    c.placement.size_v = across(expand_h, control_h, 1, alignment / 3);
                }
            }
            if (spacing != 0) {
                container["theme_override_separation"] = spacing;
            }
        } else {
            type = "GridContainer";
            const double cell_w = body.number_at("m_CellSize.x", 100.0);
            const double cell_h = body.number_at("m_CellSize.y", 100.0);
            const double gap_x = body.number_at("m_Spacing.x");
            const double gap_y = body.number_at("m_Spacing.y");
            std::int64_t columns = 1;
            const std::int64_t count =
                std::max<std::int64_t>(body.integer_at("m_ConstraintCount", 2), 1);
            switch (body.integer_at("m_Constraint", 0)) {
            case 1:
                columns = count;
                break;
            case 2:
                columns = std::max<std::int64_t>(
                    (static_cast<std::int64_t>(children.size()) + count - 1) / count, 1);
                break;
            default:
                columns = std::max<std::int64_t>(
                    static_cast<std::int64_t>(std::floor(
                        (go.rect[0] - padding[0] - padding[2] + gap_x) / (cell_w + gap_x))),
                    1);
                break;
            }
            container["columns"] = columns;
            if (body.integer_at("m_StartCorner", 0) != 0 ||
                body.integer_at("m_StartAxis", 0) != 0 || alignment != 0) {
                problems_.add("grid layout corner, axis or alignment", ctx.where,
                              "a GridContainer fills rows from the top left");
            }
            for (const int child : children) {
                GameObject& c = file.objects[static_cast<std::size_t>(child)];
                c.placement.placed = true;
                c.placement.size_h = "fill";
                c.placement.size_v = "fill";
                c.placement.minimum = {cell_w, cell_h};
            }
            if (std::lround(gap_x) != 0) {
                container["theme_override_h_separation"] = std::lround(gap_x);
            }
            if (std::lround(gap_y) != 0) {
                container["theme_override_v_separation"] = std::lround(gap_y);
            }
        }
        // Separations are theme constants of the control that holds the container.
        const auto take_constants = [&](json::Value& on) {
            for (const auto& [key, name] : std::array<std::pair<const char*, const char*>, 3>{
                     {{"theme_override_separation", "separation"},
                      {"theme_override_h_separation", "h_separation"},
                      {"theme_override_v_separation", "v_separation"}}}) {
                if (const json::Value* value = container.find(key)) {
                    on["theme_override_constants"][name] = *value;
                    container.as_object().erase(key);
                }
            }
        };
        if (std::ranges::all_of(padding, [](double side) { return side == 0.0; })) {
            take_constants(control);
            out.components[type] = std::move(container);
            return;
        }
        // Padding: a MarginContainer whose child entity is the container.
        out.components["MarginContainer"] = json::Object{};
        const std::array<const char*, 4> sides{"margin_left", "margin_top", "margin_right",
                                               "margin_bottom"};
        for (std::size_t i = 0; i < 4; ++i) {
            control["theme_override_constants"][sides[i]] = std::lround(padding[i]);
        }
        json::Value inner = json::Object{};
        take_constants(inner);
        json::Object added;
        added[type] = std::move(container);
        added["Control"] = std::move(inner);
        out.added.emplace_back("layout", std::move(added));
        out.children_to = "layout";
    }

    // How far a field's text is in from each side of the field (left, top, right, bottom):
    // the offsets of the rects from it up to the field, each stretched over its parent, and a
    // TextMesh Pro text's margins; nullopt if one isn't stretched.
    std::optional<std::array<double, 4>> text_inset(const Expanded& file, int part, int owner,
                                                    std::array<double, 2> owner_rect) {
        if (part < 0 || part == owner) {
            return std::nullopt;
        }
        std::array<double, 4> inset{};
        for (int at = part; at != owner; at = file.objects[static_cast<std::size_t>(at)].parent) {
            if (at < 0) {
                return std::nullopt;
            }
            const Component* transform = transform_of(file.objects[static_cast<std::size_t>(at)]);
            if (transform == nullptr || transform->class_id != kRectTransform) {
                return std::nullopt;
            }
            const Node& rect = transform->body;
            if (rect.number_at("m_AnchorMin.x", 0.5) != 0.0 ||
                rect.number_at("m_AnchorMin.y", 0.5) != 0.0 ||
                rect.number_at("m_AnchorMax.x", 0.5) != 1.0 ||
                rect.number_at("m_AnchorMax.y", 0.5) != 1.0) {
                return std::nullopt;
            }
            json::Value control;
            rect_control(rect, owner_rect, control);
            inset[0] += number_in(control, "offset_left");
            inset[1] += number_in(control, "offset_top");
            inset[2] -= number_in(control, "offset_right");
            inset[3] -= number_in(control, "offset_bottom");
        }
        if (const Component* pro =
                script_of(file.objects[static_cast<std::size_t>(part)], "TextMeshProUGUI")) {
            inset[0] += pro->body.number_at("m_margin.x");
            inset[1] += pro->body.number_at("m_margin.y");
            inset[2] += pro->body.number_at("m_margin.z");
            inset[3] += pro->body.number_at("m_margin.w");
        }
        return inset;
    }

    // A child's rect size in a parent's rect of the given size.
    std::array<double, 2> child_size(const Expanded& file, int child,
                                     std::array<double, 2> parent) {
        const Component* transform = transform_of(file.objects[static_cast<std::size_t>(child)]);
        if (transform == nullptr) {
            return {0.0, 0.0};
        }
        json::Value ignored;
        return rect_control(transform->body, parent, ignored);
    }

    // --- Joints ---

    // Where an object's own points are in its scene, in pixels: its transform after its parents'.
    [[nodiscard]] world::Affine2D placed(const Expanded& file, int index) const {
        world::Affine2D out;
        for (int at = index, depth = 0; at >= 0 && depth < 256;
             at = file.objects[static_cast<std::size_t>(at)].parent, ++depth) {
            if (const Component* transform =
                    transform_of(file.objects[static_cast<std::size_t>(at)])) {
                out = world::affine(local_2d(transform->body)) * out;
            }
        }
        return out;
    }

    // A point written in Unity's units, in pixels with y down.
    [[nodiscard]] Vec2 units_at(const Node& node, const std::string& key) const {
        return {static_cast<float>(node.number_at(key + ".x") * ppu_),
                static_cast<float>(-node.number_at(key + ".y") * ppu_)};
    }

    // Sets a joint entity's Transform2D, in its object's space, so the joint's y axis runs from
    // origin to origin + span in the scene; returns how many of the joint's own pixels a scene
    // pixel along it is.
    static double aim(json::Value& transform, const world::Affine2D& object, Vec2 origin,
                      Vec2 span) {
        const world::Affine2D inverse = object.inverse();
        const Vec2 start = inverse.apply(origin);
        const double length = std::hypot(span.x, span.y);
        const Vec2 unit = length > 0.0 ? span * static_cast<float>(1.0 / length) : Vec2{0.0F, 1.0F};
        const Vec2 along = inverse.apply(origin + unit) - start;
        transform["position"] = pair(start.x, start.y);
        transform["rotation"] = tidy_angle(std::atan2(-along.x, along.y));
        return std::hypot(along.x, along.y);
    }

    static double mass_of(const GameObject& go) {
        for (const Component& component : go.components) {
            if (component.class_id == kRigidbody2D) {
                return component.body.integer_at("m_BodyType", 0) == 0
                           ? std::max(component.body.number_at("m_Mass", 1.0), 0.0)
                           : 0.0;
            }
        }
        return 0.0;
    }

    // Unity's HingeJoint2D, SpringJoint2D and SliderJoint2D as Godot's PinJoint2D,
    // DampedSpringJoint2D and GrooveJoint2D, each on an entity under the joint's object. The
    // joint joins the connected body (or the world) as node_a to the object's own body as
    // node_b, so Unity's joint angles keep their sign with y down.
    void joints(const Context& ctx, const std::vector<const Component*>& found,
                const Component* rigidbody, Converted& out) {
        const Expanded& file = *ctx.file;
        const GameObject& go = file.objects[static_cast<std::size_t>(ctx.index)];
        std::map<std::string, int> counts;
        for (const Component* joint : found) {
            const Node& body = joint->body;
            if (!body.flag_at("m_Enabled", true)) {
                problems_.add("disabled " + joint->type, ctx.where,
                              "joints can't be disabled, so it isn't converted");
                continue;
            }
            if (rigidbody == nullptr) {
                problems_.add(joint->type + " without a Rigidbody2D", ctx.where);
                continue;
            }
            const world::Affine2D object = placed(file, ctx.index);
            const Vec2 local_anchor = units_at(body, "m_Anchor");
            const Vec2 anchor = object.apply(local_anchor);
            json::Value fields;
            fields["node_a"] = nullptr;
            int other = -1;
            if (const auto ref = ref_at(body, "m_ConnectedRigidBody"); ref && !ref->null()) {
                if (const auto at = file.components.find(file.resolve(ref->file_id));
                    at != file.components.end()) {
                    other = at->second.first;
                    fields["node_a"] = file.objects[static_cast<std::size_t>(other)].entity;
                } else {
                    problems_.add("joint to a missing body", ctx.where,
                                  "it joins the body to the world");
                }
            }
            fields["node_b"] = go.entity;
            Vec2 connected = anchor;
            if (!body.flag_at("m_AutoConfigureConnectedAnchor")) {
                const Vec2 local = units_at(body, "m_ConnectedAnchor");
                connected = other >= 0 ? placed(file, other).apply(local) : local;
            }
            if (body.flag_at("m_EnableCollision")) {
                fields["disable_collision"] = false;
            }
            constexpr double kForever = std::numeric_limits<double>::infinity();
            if (std::isfinite(body.number_at("m_BreakForce", kForever)) ||
                std::isfinite(body.number_at("m_BreakTorque", kForever))) {
                problems_.add("breakable joint", ctx.where,
                              "joints don't break; gameplay code can destroy one");
            }
            json::Value transform;
            std::string component;
            std::string purpose;
            if (joint->class_id == kHingeJoint2D) {
                component = "PinJoint2D";
                purpose = "hinge";
                transform["position"] = pair(local_anchor.x, local_anchor.y);
                if (std::hypot(anchor.x - connected.x, anchor.y - connected.y) > 0.5) {
                    problems_.add("joint anchors apart", ctx.where,
                                  "it pins the bodies where its anchor is; Unity pulls the "
                                  "connected anchor to it");
                }
                if (body.flag_at("m_UseLimits")) {
                    const double lower =
                        body.number_at("m_AngleLimits.m_LowerAngle") * std::numbers::pi / 180.0;
                    const double upper =
                        body.number_at("m_AngleLimits.m_UpperAngle") * std::numbers::pi / 180.0;
                    if (lower > 0.0 || upper < 0.0) {
                        problems_.add("hinge limits on one side of where it starts", ctx.where,
                                      "limits keep the angle it starts at between them, as "
                                      "Godot's");
                    }
                    fields["angular_limit_enabled"] = true;
                    fields["angular_limit_lower"] = tidy_angle(std::clamp(lower, -3.1, 0.0));
                    fields["angular_limit_upper"] = tidy_angle(std::clamp(upper, 0.0, 3.1));
                }
                if (body.flag_at("m_UseMotor")) {
                    fields["motor_enabled"] = true;
                    fields["motor_target_velocity"] = tidy_angle(
                        body.number_at("m_Motor.m_MotorSpeed") * std::numbers::pi / 180.0);
                }
            } else if (joint->class_id == kSpringJoint2D) {
                component = "DampedSpringJoint2D";
                purpose = "spring";
                // From the connected anchor to the object's.
                const double scale = aim(transform, object, connected, anchor - connected);
                const double length =
                    std::hypot(anchor.x - connected.x, anchor.y - connected.y) * scale;
                fields["length"] = tidy(length);
                if (!body.flag_at("m_AutoConfigureDistance")) {
                    fields["rest_length"] =
                        tidy(std::max(body.number_at("m_Distance") * ppu_, 0.01));
                }
                // Unity's frequency and damping ratio, as the stiffness and damping that give
                // them for the bodies' masses.
                const double mass_a =
                    other >= 0 ? mass_of(file.objects[static_cast<std::size_t>(other)]) : 0.0;
                const double mass_b = mass_of(go);
                const double mass =
                    std::max(mass_a > 0.0 && mass_b > 0.0 ? mass_a * mass_b / (mass_a + mass_b)
                                                          : std::max(mass_a, mass_b),
                             1e-3);
                double frequency = body.number_at("m_Frequency", 1.0);
                if (frequency <= 0.0) {
                    problems_.add("spring with frequency 0", ctx.where,
                                  "a stiff spring at 15 Hz stands in for Unity's rigid one");
                    frequency = 15.0;
                }
                const double omega = 2.0 * std::numbers::pi * frequency;
                const double stiffness = mass * omega * omega;
                fields["stiffness"] = tidy(stiffness);
                fields["damping"] =
                    tidy(2.0 * std::clamp(body.number_at("m_DampingRatio"), 0.0, 1.0) *
                         std::sqrt(stiffness * mass));
            } else {
                component = "GrooveJoint2D";
                purpose = "slider";
                // Unity's angle is counterclockwise from x with y up.
                const double angle = body.number_at("m_Angle") * std::numbers::pi / 180.0;
                const Vec2 axis{static_cast<float>(std::cos(angle)),
                                static_cast<float>(-std::sin(angle))};
                const Vec2 offset = anchor - connected;
                const double at = (offset.x * axis.x) + (offset.y * axis.y);
                double lower = at - 10000.0;
                double upper = at + 10000.0;
                if (body.flag_at("m_UseLimits")) {
                    lower = body.number_at("m_TranslationLimits.m_LowerTranslation") * ppu_;
                    upper = body.number_at("m_TranslationLimits.m_UpperTranslation") * ppu_;
                    if (lower > upper) {
                        std::swap(lower, upper);
                    }
                } else {
                    problems_.add("SliderJoint2D without limits", ctx.where,
                                  "it slides along a groove 20000 pixels long");
                }
                const Vec2 start = connected + (axis * static_cast<float>(lower));
                const double scale =
                    aim(transform, object, start, axis * static_cast<float>(upper - lower));
                fields["length"] = tidy((upper - lower) * scale);
                fields["initial_offset"] = tidy(std::clamp(at - lower, 0.0, upper - lower) * scale);
                if ((rigidbody->body.integer_at("m_Constraints", 0) & 4) == 0) {
                    problems_.add("SliderJoint2D", ctx.where,
                                  "the body turns freely along the groove, as on Godot's "
                                  "GrooveJoint2D; freezing its rotation keeps it straight");
                }
                if (body.flag_at("m_UseMotor")) {
                    problems_.add("SliderJoint2D motor", ctx.where, "a GrooveJoint2D has no motor");
                }
            }
            const int count = ++counts[purpose];
            json::Object added;
            added["Transform2D"] = std::move(transform);
            added[component] = std::move(fields);
            out.added.emplace_back(count == 1 ? purpose : purpose + "_" + std::to_string(count),
                                   std::move(added));
        }
    }

    // --- Tile maps ---

    // The components of a Tilemap's object that make up a tile map layer.
    struct TileMapParts {
        const Component* tilemap = nullptr;
        const Component* renderer = nullptr;
        const Component* collider = nullptr;
        const Component* rigidbody = nullptr;
        const Component* effector = nullptr;
        const Component* composite = nullptr;
    };

    // What a cell gives its tile besides the sprite: Unity's collider type (0 none, 1 the
    // sprite's physics shape, 2 the cell), a color, and where its tile map's anchor puts the
    // sprite's pivot, in pixels from the middle of the cell with y down.
    struct TileLook {
        std::int64_t collider = 1;
        std::array<double, 4> color{1.0, 1.0, 1.0, 1.0};
        std::array<double, 2> shift{};

        auto operator<=>(const TileLook&) const = default;
    };

    // A tile of a tile set the import writes: one sprite, and an alternative for each other look
    // cells give it, as Godot's alternative tiles.
    struct TileOut {
        const Sprite* sprite = nullptr;
        std::string source;
        std::array<std::int64_t, 2> atlas{};
        std::string name;
        TileLook base;
        std::map<TileLook, int> alternatives;
        // Polygons by collider type and shift.
        std::map<std::pair<std::int64_t, std::array<double, 2>>, json::Array> collision;
        // An AnimatedTile's frames, as Godot lays them out after the tile: how many, how many to
        // a row (0: all in one) and the texture cells between them; each lasts duration seconds.
        int frames = 1;
        int columns = 0;
        std::array<std::int64_t, 2> separation{};
        double duration = 1.0;
        bool still = false; // shown by a Tile too, which doesn't animate in Unity
    };

    // What an AnimatedTile (from Unity's 2D Tilemap Extras) plays: its sprites in order, at a
    // speed in frames per second times the tile map's animation frame rate.
    struct AnimatedTileAsset {
        std::vector<ObjectRef> sprites;
        double min_speed = 1.0;
        double max_speed = 1.0;
        double start_time = 0.0;
        std::int64_t start_frame = 0;
        std::int64_t flags = 0;
    };

    // The physics a tile map's collider gives its tile set's physics layer.
    struct TilePhysics {
        std::int64_t layer = 1;
        std::int64_t mask = 1;
        double friction = 1.0;
        double bounce = 0.0;
        bool one_way = false;

        auto operator<=>(const TilePhysics&) const = default;
    };

    // A grid of one texture's sprites of one size, as an atlas source cuts it; an empty source
    // means they don't fall on one, so each sprite gets a source of its own.
    struct TileGrid {
        std::string source;
        std::array<std::int64_t, 2> step{};
        std::array<std::int64_t, 2> margins{};
    };

    // A .tileset.json file the import writes: the tiles of the tile maps whose cells are its size
    // in pixels and whose colliders have its physics, by sprite.
    struct TileSetOut {
        std::string path;
        std::array<std::int64_t, 2> cell{};
        canvas::TileShape shape = canvas::TileShape::Square;
        canvas::TileLayout layout = canvas::TileLayout::Stacked;
        canvas::TileOffsetAxis axis = canvas::TileOffsetAxis::Horizontal;
        std::optional<TilePhysics> physics;
        std::map<std::string, json::Value> sources; // atlas sources by name, without their tiles
        std::map<std::string, TileGrid> grids;      // by "<guid> <width>x<height>"
        std::map<std::pair<std::string, std::int64_t>, TileOut> tiles; // by texture GUID, file ID
        std::set<std::string> names;                                   // the tiles'
    };

    // How a Grid lays out its cells, as a tile set's shape: Unity's cell (x, y) is cell(x, y)
    // here, and the tile map moves by offset, in its own pixels, so each cell's center is where
    // Unity's is.
    struct GridPlan {
        std::int64_t layout = 0; // Unity's: 0 rectangle, 1 hexagon, 2 isometric, 3 z as y
        bool swap = false;       // a YXZ swizzle: the cells' x runs up the y axis, their y along x
        double width = 1.0;      // the cell size along the cells' x and y, in whole pixels
        double height = 1.0;
        canvas::TileSet set; // the shape, layout, offset axis and tile size
        Vec2 offset;

        // Where Unity centers a cell, in the tile map's pixels with y down.
        [[nodiscard]] std::array<double, 2> center(int x, int y) const {
            const auto cx = static_cast<double>(x);
            const auto cy = static_cast<double>(y);
            std::array<double, 2> at{}; // in Unity's units, y up, before the swizzle
            if (layout == 0) {
                at = {(cx + 0.5) * width, (cy + 0.5) * height};
            } else if (layout == 1) {
                // A hexagon's corner is its center; odd rows sit half a cell right, and rows
                // overlap by a quarter.
                at = {(cx + (y % 2 != 0 ? 0.5 : 0.0)) * width, 0.75 * cy * height};
            } else {
                // A diamond's corner is its bottom one; x runs up to the right, y up to the left.
                at = {(cx - cy) * width / 2.0, (cx + cy + 1.0) * height / 2.0};
            }
            if (swap) {
                std::swap(at[0], at[1]);
            }
            return {at[0], -at[1]};
        }

        // Where a tile anchor, in fractions of a cell from its corner, is from the cell's center,
        // in pixels with y down. A hexagon's is its center.
        [[nodiscard]] std::array<double, 2> shift(double x, double y) const {
            std::array<double, 2> at{};
            if (layout == 0) {
                at = {(x - 0.5) * width, (y - 0.5) * height};
            } else if (layout != 1) {
                at = {(x - y) * width / 2.0, (x + y - 1.0) * height / 2.0};
            }
            if (swap) {
                std::swap(at[0], at[1]);
            }
            return {at[0] == 0.0 ? 0.0 : at[0], at[1] == 0.0 ? 0.0 : -at[1]};
        }

        // The cell here for Unity's cell: rows go down, and a swizzled grid's columns are its y.
        // A swizzled diamond's x still runs up to the right, and its y down to the right.
        [[nodiscard]] Vec2i cell(int x, int y) const {
            if (swap && layout >= 2) {
                return {x, y};
            }
            return swap ? Vec2i{y, -x - 1} : Vec2i{x, -y - 1};
        }
    };

    std::optional<GridPlan> grid_plan(const Node& grid, const std::string& where) {
        GridPlan plan;
        plan.layout = grid.integer_at("m_CellLayout", 0);
        const std::int64_t swizzle = grid.integer_at("m_CellSwizzle", 0);
        if (plan.layout < 0 || plan.layout > 3) {
            problems_.add("grid cell layout", where, "this tile map isn't converted");
            return std::nullopt;
        }
        if (swizzle != 0 && swizzle != 2) {
            problems_.add("grid cell swizzle", where,
                          "tile maps lie in the xy plane; this one isn't converted");
            return std::nullopt;
        }
        plan.swap = swizzle == 2;
        if (grid.number_at("m_CellGap.x") != 0.0 || grid.number_at("m_CellGap.y") != 0.0) {
            problems_.add("grid cell gap", where, "cells touch");
        }
        const double cell_x = std::abs(grid.number_at("m_CellSize.x", 1.0)) * ppu_;
        const double cell_y = std::abs(grid.number_at("m_CellSize.y", 1.0)) * ppu_;
        const std::array<std::int64_t, 2> whole{std::max<std::int64_t>(std::llround(cell_x), 1),
                                                std::max<std::int64_t>(std::llround(cell_y), 1)};
        if (std::abs(cell_x - static_cast<double>(whole[0])) > 1e-6 ||
            std::abs(cell_y - static_cast<double>(whole[1])) > 1e-6) {
            problems_.add("grid cell size between whole pixels", where,
                          "tiles are whole pixels; the size is rounded");
        }
        constexpr std::int64_t kMaxPixels = 16384; // a tile set's largest tile size
        const auto x = static_cast<int>(std::min(whole[0], kMaxPixels));
        const auto y = static_cast<int>(std::min(whole[1], kMaxPixels));
        plan.width = static_cast<double>(x);
        plan.height = static_cast<double>(y);
        plan.set.tile_size = plan.swap ? Vec2i{y, x} : Vec2i{x, y};
        if (plan.layout == 1) {
            // Unity's odd rows (columns, swizzled) sit over; Godot's stacked_offset layout moves
            // its even ones, and rows here have the other parity.
            plan.set.shape = canvas::TileShape::Hexagon;
            plan.set.layout = canvas::TileLayout::StackedOffset;
            plan.set.offset_axis =
                plan.swap ? canvas::TileOffsetAxis::Vertical : canvas::TileOffsetAxis::Horizontal;
        } else if (plan.layout >= 2) {
            plan.set.shape = canvas::TileShape::Isometric;
            plan.set.layout = canvas::TileLayout::DiamondRight;
        }
        const Vec2 ours = canvas::map_to_local(plan.set, plan.cell(0, 0));
        const auto unity = plan.center(0, 0);
        plan.offset = {static_cast<float>(unity[0] - static_cast<double>(ours.x)),
                       static_cast<float>(unity[1] - static_cast<double>(ours.y))};
        return plan;
    }

    TileSetOut& tile_set_for(const GridPlan& plan, const std::optional<TilePhysics>& physics) {
        const std::array<std::int64_t, 2> cell{plan.set.tile_size.x, plan.set.tile_size.y};
        for (TileSetOut& set : tile_sets_) {
            if (set.cell == cell && set.shape == plan.set.shape && set.layout == plan.set.layout &&
                set.axis == plan.set.offset_axis &&
                (!physics || !set.physics || *set.physics == *physics)) {
                if (physics && !set.physics) {
                    set.physics = physics;
                }
                return set;
            }
        }
        TileSetOut& set = tile_sets_.emplace_back();
        set.cell = cell;
        set.shape = plan.set.shape;
        set.layout = plan.set.layout;
        set.axis = plan.set.offset_axis;
        set.physics = physics;
        return set;
    }

    // A source's name, unique in its tile set.
    static std::string source_name(const TileSetOut& set, std::string name) {
        if (name.empty()) {
            name = "source";
        }
        std::string candidate = name;
        for (int n = 2; set.sources.contains(candidate); ++n) {
            candidate = name + "_" + std::to_string(n);
        }
        return candidate;
    }

    // The atlas source holding a sprite, and the sprite's column and row in it.
    std::pair<std::string, std::array<std::int64_t, 2>> tile_source(TileSetOut& set,
                                                                    const std::string& guid,
                                                                    const Sprite& sprite,
                                                                    const std::string& where) {
        const Texture& sheet = texture(guid);
        const auto rect_of = [](const Sprite& s) {
            return s.whole ? std::array<double, 4>{0.0, 0.0, s.width, s.height} : s.region;
        };
        const auto whole = [](double v) { return std::abs(v - std::round(v)) < 1e-6; };
        const std::array<double, 4> rect = rect_of(sprite);
        const std::int64_t width = std::llround(rect[2]);
        const std::int64_t height = std::llround(rect[3]);
        const std::string key = guid + " " + std::to_string(width) + "x" + std::to_string(height);
        auto grid = set.grids.find(key);
        if (grid == set.grids.end()) {
            // Sprites sliced by Unity's grid fall on one: a step between columns and rows (the
            // gap between neighbors seen most), and a margin before the first. Sprites off it get
            // sources of their own.
            std::vector<std::array<std::int64_t, 2>> corners;
            for (const auto& [id, other] : sheet.sprites) {
                const std::array<double, 4> r = rect_of(other);
                if (std::llround(r[2]) == width && std::llround(r[3]) == height && whole(r[0]) &&
                    whole(r[1])) {
                    corners.push_back({std::llround(r[0]), std::llround(r[1])});
                }
            }
            TileGrid out;
            const auto axis = [&](std::size_t i, std::int64_t size) {
                std::set<std::int64_t> values;
                for (const auto& corner : corners) {
                    values.insert(corner.at(i));
                }
                std::map<std::int64_t, int> gaps;
                for (auto it = values.begin(); std::next(it) != values.end(); ++it) {
                    if (const std::int64_t gap = *std::next(it) - *it; gap >= size) {
                        ++gaps[gap];
                    }
                }
                std::int64_t step = size;
                int seen = 0;
                for (const auto& [gap, count] : gaps) {
                    if (count > seen) {
                        step = gap;
                        seen = count;
                    }
                }
                out.step.at(i) = step;
                out.margins.at(i) = values.empty() ? 0 : *values.begin() % step;
            };
            if (!corners.empty() && width > 0 && height > 0 && whole(rect[2]) && whole(rect[3])) {
                axis(0, width);
                axis(1, height);
                json::Value source;
                source["texture"] = sprite.texture;
                if (!sprite.normal_map.empty()) {
                    source["normal_texture"] = sprite.normal_map;
                }
                if (width != set.cell[0] || height != set.cell[1]) {
                    source["region_size"] = json::Array{width, height};
                }
                if (out.margins[0] != 0 || out.margins[1] != 0) {
                    source["margins"] = json::Array{out.margins[0], out.margins[1]};
                }
                if (out.step[0] != width || out.step[1] != height) {
                    source["separation"] = json::Array{out.step[0] - width, out.step[1] - height};
                }
                std::string name = snake_case(file_stem(sheet.path));
                if (std::ranges::any_of(set.grids, [&](const auto& entry) {
                        return entry.first.starts_with(guid + " ");
                    })) {
                    name += "_" + std::to_string(width) + "x" + std::to_string(height);
                }
                out.source = source_name(set, name);
                set.sources[out.source] = std::move(source);
            }
            grid = set.grids.emplace(key, std::move(out)).first;
        }
        const TileGrid& found = grid->second;
        const std::int64_t left = std::llround(rect[0]);
        const std::int64_t top = std::llround(rect[1]);
        if (!found.source.empty() && whole(rect[0]) && whole(rect[1]) &&
            (left - found.margins[0]) % found.step[0] == 0 &&
            (top - found.margins[1]) % found.step[1] == 0) {
            return {found.source,
                    {(left - found.margins[0]) / found.step[0],
                     (top - found.margins[1]) / found.step[1]}};
        }
        // A source of its own, its margins at the sprite.
        if (!whole(rect[0]) || !whole(rect[1]) || !whole(rect[2]) || !whole(rect[3])) {
            problems_.add("tile sprite rect between pixels", where,
                          "its tile takes the nearest whole pixels");
        }
        json::Value source;
        source["texture"] = sprite.texture;
        if (!sprite.normal_map.empty()) {
            source["normal_texture"] = sprite.normal_map;
        }
        if (width != set.cell[0] || height != set.cell[1]) {
            source["region_size"] =
                json::Array{std::max<std::int64_t>(width, 1), std::max<std::int64_t>(height, 1)};
        }
        source["margins"] = json::Array{std::llround(rect[0]), std::llround(rect[1])};
        const std::string name =
            source_name(set, snake_case(file_stem(sheet.path)) + "_" + snake_case(sprite.name));
        set.sources[name] = std::move(source);
        return {name, {0, 0}};
    }

    // Where a sprite's pivot is from its middle, in its pixels with y down.
    static std::array<double, 2> pivot_offset(const Sprite& sprite) {
        return {(sprite.pivot_x - 0.5) * sprite.width, (0.5 - sprite.pivot_y) * sprite.height};
    }

    // Where a tile's texture is from its cell's center, as a texture_origin: Unity puts the
    // sprite's pivot at the tile anchor.
    static std::array<double, 2> texture_origin(const Sprite& sprite, const TileLook& look) {
        const auto [x, y] = pivot_offset(sprite);
        return {tidy(x - look.shift[0]), tidy(y - look.shift[1])};
    }

    // A tile's collision polygons for a look, in pixels around its cell's center, y down: the
    // sprite's physics shape where Unity puts it, or the cell's outline.
    json::Array tile_polygons(const TileSetOut& set, const Sprite& sprite, const TileLook& look,
                              const std::string& where) {
        json::Array out;
        std::vector<Outline> shapes;
        if (look.collider == 2) {
            const double x = static_cast<double>(set.cell[0]) / 2.0;
            const double y = static_cast<double>(set.cell[1]) / 2.0;
            if (set.shape == canvas::TileShape::Isometric) {
                shapes.push_back({{0.0, -y}, {x, 0.0}, {0.0, y}, {-x, 0.0}});
            } else if (set.shape == canvas::TileShape::Hexagon &&
                       set.axis == canvas::TileOffsetAxis::Horizontal) {
                shapes.push_back({{0.0, -y},
                                  {x, -y / 2.0},
                                  {x, y / 2.0},
                                  {0.0, y},
                                  {-x, y / 2.0},
                                  {-x, -y / 2.0}});
            } else if (set.shape == canvas::TileShape::Hexagon) {
                shapes.push_back({{-x, 0.0},
                                  {-x / 2.0, -y},
                                  {x / 2.0, -y},
                                  {x, 0.0},
                                  {x / 2.0, y},
                                  {-x / 2.0, y}});
            } else {
                shapes.push_back({{-x, -y}, {x, -y}, {x, y}, {-x, y}});
            }
        } else if (look.collider == 1) {
            // Unity's outlines are around the sprite's middle with y up; the sprite's middle is
            // its pivot's offset from the anchor.
            const double scale = ppu_ / sprite.ppu;
            const std::array<double, 2> pivot = pivot_offset(sprite);
            const auto place = [&](const Outline& outline) {
                Outline placed;
                for (const auto& [x, y] : outline) {
                    placed.push_back({look.shift[0] + ((x - pivot[0]) * scale),
                                      look.shift[1] + ((-y - pivot[1]) * scale)});
                }
                return placed;
            };
            for (const Outline& outline : sprite.shape) {
                shapes.push_back(place(outline));
            }
            if (shapes.empty() && sprite.fallback_shape) {
                problems_.add("tile physics shape from the sprite's alpha", where,
                              "Unity makes it from the pixels; the tile's polygon is the sprite's "
                              "rectangle");
                const double x = sprite.width / 2.0;
                const double y = sprite.height / 2.0;
                shapes.push_back(place({{-x, y}, {x, y}, {x, -y}, {-x, -y}}));
            }
        }
        for (const Outline& outline : shapes) {
            std::vector<Vec2> points;
            json::Array polygon;
            for (const auto& [x, y] : outline) {
                points.push_back({static_cast<float>(x), static_cast<float>(y)});
                polygon.push_back(pair(tidy(x), tidy(y)));
            }
            if (points.size() < 3 || canvas::triangulate(points).empty()) {
                problems_.add("tile physics shape with no area or crossing itself", where,
                              "left out");
                continue;
            }
            json::Value item;
            item["points"] = std::move(polygon);
            out.push_back(std::move(item));
        }
        return out;
    }

    // The tile, and its alternative, that a cell showing this sprite with this look has.
    std::pair<TileOut*, int> tile_for(TileSetOut& set, const ObjectRef& ref, const Sprite& sprite,
                                      const TileLook& look, const std::string& where) {
        auto [it, added] = set.tiles.try_emplace({ref.guid, ref.file_id});
        TileOut& tile = it->second;
        if (added) {
            if (set.path.empty()) {
                // Tile sets hold every tile map's tiles, whatever textures they come from.
                const auto number = std::ranges::count_if(
                    tile_sets_, [](const TileSetOut& other) { return !other.path.empty(); });
                set.path =
                    destination("Assets/tiles" +
                                (number == 0 ? std::string() : "_" + std::to_string(number + 1)) +
                                std::string(canvas::kTileSetSuffix));
            }
            tile.sprite = &sprite;
            std::tie(tile.source, tile.atlas) = tile_source(set, ref.guid, sprite, where);
            tile.name = take(set.names, sprite.name.empty() ? "tile" : snake_case(sprite.name));
            tile.base = TileLook{.collider = look.collider, .shift = look.shift};
            if (std::abs(sprite.ppu - ppu_) > 1e-6) {
                problems_.add("tile sprite with other pixels per unit", where,
                              "a tile draws at its size in pixels, unscaled");
            }
        }
        if (!tile.collision.contains({look.collider, look.shift})) {
            tile.collision[{look.collider, look.shift}] = tile_polygons(set, sprite, look, where);
            const auto origin = texture_origin(sprite, look);
            if (origin[0] != std::round(origin[0]) || origin[1] != std::round(origin[1])) {
                problems_.add("tile sprite pivot between whole pixels", where,
                              "a tile's texture_origin is whole pixels; it draws up to half a "
                              "pixel off");
            }
        }
        if (look == tile.base) {
            return {&tile, 0};
        }
        const auto [alternative, inserted] =
            tile.alternatives.try_emplace(look, static_cast<int>(tile.alternatives.size()) + 1);
        return {&tile, alternative->second};
    }

    [[nodiscard]] json::Value tile_set_doc(const TileSetOut& set) const {
        json::Value doc;
        doc["format"] = 1;
        if (set.cell != std::array<std::int64_t, 2>{16, 16}) {
            doc["tile_size"] = json::Array{set.cell[0], set.cell[1]};
        }
        if (set.shape != canvas::TileShape::Square) {
            doc["tile_shape"] = canvas::kTileShapeNames.at(static_cast<std::size_t>(set.shape));
            doc["tile_layout"] = canvas::kTileLayoutNames.at(static_cast<std::size_t>(set.layout));
            if (set.axis != canvas::TileOffsetAxis::Horizontal) {
                doc["tile_offset_axis"] =
                    canvas::kTileOffsetAxisNames.at(static_cast<std::size_t>(set.axis));
            }
        }
        json::Value sources = json::Object{};
        for (const auto& [name, source] : set.sources) {
            sources[name] = source;
        }
        bool polygons = false;
        const auto collision = [&](const TileOut& tile, const TileLook& look) {
            const auto found = tile.collision.find({look.collider, look.shift});
            json::Array out = found != tile.collision.end() ? found->second : json::Array{};
            for (json::Value& polygon : out) {
                if (set.physics && set.physics->one_way) {
                    polygon["one_way"] = true;
                }
            }
            polygons = polygons || !out.empty();
            return out;
        };
        for (const auto& [key, tile] : set.tiles) {
            json::Value out;
            // Unity's sprite pivots and tile anchors, as each tile's texture origin.
            const auto origin = [&](const TileLook& look, json::Value& to) {
                const auto [x, y] = texture_origin(*tile.sprite, look);
                if (std::llround(x) != 0 || std::llround(y) != 0) {
                    to["texture_origin"] = json::Array{std::llround(x), std::llround(y)};
                }
            };
            out["name"] = tile.name;
            origin(tile.base, out);
            if (tile.frames > 1) {
                out["animation_frames"] =
                    json::Array(static_cast<std::size_t>(tile.frames), json::Value(tile.duration));
                if (tile.columns != 0) {
                    out["animation_columns"] = tile.columns;
                }
                if (tile.separation != std::array<std::int64_t, 2>{}) {
                    out["animation_separation"] =
                        json::Array{tile.separation[0], tile.separation[1]};
                }
            }
            if (json::Array base = collision(tile, tile.base); !base.empty()) {
                out["collision"] = std::move(base);
            }
            for (const auto& [look, number] : tile.alternatives) {
                json::Value alternative = json::Object{};
                if (look.color != std::array<double, 4>{1.0, 1.0, 1.0, 1.0}) {
                    alternative["modulate"] =
                        json::Array{look.color[0], look.color[1], look.color[2], look.color[3]};
                }
                origin(look, alternative);
                if (json::Array own = collision(tile, look); !own.empty()) {
                    alternative["collision"] = std::move(own);
                }
                out["alternatives"][std::to_string(number)] = std::move(alternative);
            }
            sources[tile.source]["tiles"][std::to_string(tile.atlas[0]) + "," +
                                          std::to_string(tile.atlas[1])] = std::move(out);
        }
        if (polygons) {
            // Only what differs from a physics layer's defaults.
            json::Value layer = json::Object{};
            const TilePhysics physics = set.physics.value_or(TilePhysics{});
            if (physics.layer != 1) {
                layer["collision_layer"] = physics.layer;
            }
            if (physics.mask != 1) {
                layer["collision_mask"] = physics.mask;
            }
            if (physics.friction != 1.0) {
                layer["friction"] = physics.friction;
            }
            if (physics.bounce != 0.0) {
                layer["bounce"] = physics.bounce;
            }
            doc["physics_layers"] = json::Array{std::move(layer)};
        }
        doc["sources"] = std::move(sources);
        return doc;
    }

    // Writes a tile set as it is now, replacing what an earlier tile map wrote.
    void write_tile_set(const TileSetOut& set) {
        if (set.path.empty()) {
            return;
        }
        const std::string text = json::write(tile_set_doc(set)) + "\n";
        for (auto& [planned, contents] : plan_.files) {
            if (planned == set.path) {
                contents = text;
                return;
            }
        }
        add_file(set.path, text);
    }

    // A cell's matrix as its tile's turns, as Godot's cell transform flags: flip_h, flip_v and
    // transpose, the axes swapped before the flips. nullopt for a matrix that scales, skews or
    // turns by other than quarter turns.
    static std::optional<std::array<bool, 3>> tile_turns(const Node* matrix) {
        if (matrix == nullptr) {
            return std::array<bool, 3>{};
        }
        // Unity's y points up and SlashEngine's down.
        const std::array<double, 4> m{matrix->number_at("e00", 1.0), -matrix->number_at("e01"),
                                      -matrix->number_at("e10"), matrix->number_at("e11", 1.0)};
        for (int bits = 0; bits < 8; ++bits) {
            const bool flip_h = (bits & 1) != 0;
            const bool flip_v = (bits & 2) != 0;
            const bool transpose = (bits & 4) != 0;
            const double h = flip_h ? -1.0 : 1.0;
            const double v = flip_v ? -1.0 : 1.0;
            const std::array<double, 4> t = transpose ? std::array<double, 4>{0.0, h, v, 0.0}
                                                      : std::array<double, 4>{h, 0.0, 0.0, v};
            bool same = true;
            for (std::size_t i = 0; i < 4; ++i) {
                same = same && std::abs(m.at(i) - t.at(i)) < 1e-3;
            }
            if (same) {
                return std::array<bool, 3>{flip_h, flip_v, transpose};
            }
        }
        return std::nullopt;
    }

    // A tile asset's object, or nullptr if it can't be read.
    const Object* tile_object(const ObjectRef& ref) {
        auto [file, added] = tile_files_.try_emplace(ref.guid);
        if (added) {
            if (const Asset* asset = asset_of(ref.guid); asset != nullptr) {
                if (auto text = platform::read_file(source_ + asset->path); text) {
                    if (auto objects = unity::parse_file(text.value()); objects) {
                        file->second = std::move(objects.value());
                    }
                }
            }
        }
        for (const Object& object : file->second) {
            if (object.file_id == ref.file_id) {
                return &object;
            }
        }
        return nullptr;
    }

    // Whether a tile asset is one of Unity's own Tiles; rule tiles, animated tiles and other
    // scripted tiles aren't.
    bool plain_tile(const ObjectRef& ref) {
        const Object* object = tile_object(ref);
        if (object == nullptr) {
            return true;
        }
        const auto script = ref_at(object->body, "m_Script");
        return script && script->guid == kTileScriptGuid && script->file_id == kTileScriptId;
    }

    // A tile asset's animation, if it is an AnimatedTile: known by its m_AnimatedSprites.
    std::optional<AnimatedTileAsset> animated_tile(const ObjectRef& ref) {
        const Object* object = tile_object(ref);
        const Node* sprites = object != nullptr ? object->body.get("m_AnimatedSprites") : nullptr;
        if (sprites == nullptr) {
            return std::nullopt;
        }
        AnimatedTileAsset out;
        for (const Node& item : sprites->items) {
            if (const auto sprite = unity::object_ref(item); sprite && !sprite->null()) {
                out.sprites.push_back(*sprite);
            }
        }
        if (out.sprites.empty()) {
            return std::nullopt;
        }
        const Node& body = object->body;
        out.min_speed = body.number_at("m_MinSpeed", 1.0);
        out.max_speed = body.number_at("m_MaxSpeed", out.min_speed);
        out.start_time = body.number_at("m_AnimationStartTime");
        out.start_frame = body.integer_at("m_AnimationStartFrame");
        out.flags = body.integer_at("m_TileAnimationFlags");
        return out;
    }

    // How frames at these texture cells follow one another as Godot's do: the frames per row
    // (0: one row) and the cells between them; nullopt if they don't.
    static std::optional<std::pair<int, std::array<std::int64_t, 2>>>
    frame_layout(const std::vector<std::array<std::int64_t, 2>>& cells) {
        const std::array<std::int64_t, 2> first = cells.front();
        int columns = 0;
        for (std::size_t i = 1; i < cells.size(); ++i) {
            if (cells[i][1] != first[1]) {
                columns = static_cast<int>(i);
                break;
            }
        }
        std::array<std::int64_t, 2> gap{};
        if (cells.size() > 1 && columns != 1) {
            gap[0] = cells[1][0] - first[0] - 1;
        }
        if (columns > 0) {
            gap[1] = cells[static_cast<std::size_t>(columns)][1] - first[1] - 1;
        }
        if (gap[0] < 0 || gap[1] < 0) {
            return std::nullopt;
        }
        for (std::size_t i = 0; i < cells.size(); ++i) {
            const auto frame = static_cast<std::int64_t>(i);
            const std::int64_t column = columns > 0 ? frame % columns : frame;
            const std::int64_t row = columns > 0 ? frame / columns : 0;
            if (cells[i] != std::array<std::int64_t, 2>{first[0] + (column * (gap[0] + 1)),
                                                        first[1] + (row * (gap[1] + 1))}) {
                return std::nullopt;
            }
        }
        return std::pair{columns, gap};
    }

    // Gives a tile an AnimatedTile's frames, if they lie in the tile's source as Godot's do.
    void animate(TileSetOut& set, TileOut& tile, const AnimatedTileAsset& animation,
                 double frame_rate, const std::string& where) {
        std::vector<std::array<std::int64_t, 2>> cells;
        for (const ObjectRef& ref : animation.sprites) {
            std::string why;
            const Sprite* frame = sprite(ref, why);
            if (frame == nullptr) {
                problems_.add("tile sprite", where, why);
                return;
            }
            // The texture first: tile_source would add a source for another one.
            const auto [source, atlas] =
                ref.guid == animation.sprites.front().guid
                    ? tile_source(set, ref.guid, *frame, where)
                    : std::pair<std::string, std::array<std::int64_t, 2>>{};
            if (source != tile.source) {
                problems_.add("animated tiles whose sprites aren't on one texture's grid", where,
                              "they show their first frame");
                return;
            }
            cells.push_back(atlas);
        }
        const auto layout = frame_layout(cells);
        if (!layout) {
            problems_.add("animated tiles whose sprites aren't in order along the texture's grid",
                          where,
                          "frames follow each other along rows, as Godot's; they show their "
                          "first frame");
            return;
        }
        if (animation.min_speed != animation.max_speed) {
            problems_.add("animated tiles with a random speed", where,
                          "they play at the middle of their speed range");
        }
        if (animation.start_time != 0.0 || animation.start_frame != 0) {
            problems_.add("animated tile start times", where,
                          "every cell starts on the first frame when the game does");
        }
        if (animation.flags != 0) {
            problems_.add("animated tile flags", where, "they loop, and keep playing");
        }
        const double fps = (animation.min_speed + animation.max_speed) / 2.0 * frame_rate;
        if (fps <= 0.0) {
            problems_.add("animated tiles that don't play", where, "they show their first frame");
            return;
        }
        const auto frames = static_cast<int>(
            std::min<std::size_t>(cells.size(), static_cast<std::size_t>(kMaxTileFrames)));
        if (tile.frames > 1 && (tile.frames != frames || tile.duration != 1.0 / fps)) {
            problems_.add("a sprite animated two ways", where,
                          "it plays the first animation everywhere");
            return;
        }
        tile.frames = frames;
        tile.columns = layout->first;
        tile.separation = layout->second;
        tile.duration = 1.0 / fps;
    }

    // Takes the animation off tiles whose frames cover texture cells other tiles or their frames
    // do, as Godot's has_room_for_tile forbids.
    void settle_animations(TileSetOut& set, const std::string& where) {
        std::set<std::pair<std::string, std::array<std::int64_t, 2>>> covered;
        for (const auto& [key, tile] : set.tiles) {
            covered.insert({tile.source, tile.atlas});
        }
        for (auto& [key, tile] : set.tiles) {
            if (tile.frames > 1 && tile.still) {
                problems_.add("sprites shown both by animated tiles and still ones", where,
                              "they play wherever they show");
                tile.still = false;
            }
            std::vector<std::pair<std::string, std::array<std::int64_t, 2>>> cells;
            for (int frame = 1; frame < tile.frames; ++frame) {
                const std::int64_t column = tile.columns > 0 ? frame % tile.columns : frame;
                const std::int64_t row = tile.columns > 0 ? frame / tile.columns : 0;
                cells.emplace_back(
                    tile.source,
                    std::array<std::int64_t, 2>{tile.atlas[0] + (column * (tile.separation[0] + 1)),
                                                tile.atlas[1] + (row * (tile.separation[1] + 1))});
            }
            if (std::ranges::any_of(cells,
                                    [&](const auto& cell) { return covered.contains(cell); })) {
                problems_.add("animated tiles whose frames other tiles show", where,
                              "they show their first frame");
                tile.frames = 1;
                continue;
            }
            covered.insert(cells.begin(), cells.end());
        }
    }

    void tile_map(const Context& ctx, const TileMapParts& parts, Converted& out) {
        const GameObject& go = ctx.file->objects[static_cast<std::size_t>(ctx.index)];
        // The Grid is on the tile map's parent.
        const Node* grid = nullptr;
        if (go.parent >= 0) {
            for (const Component& component :
                 ctx.file->objects[static_cast<std::size_t>(go.parent)].components) {
                if (component.class_id == kGrid) {
                    grid = &component.body;
                }
            }
        }
        const Node none;
        if (grid == nullptr) {
            problems_.add("Tilemap without a Grid above it", ctx.where, "its cells are 1 unit");
            grid = &none;
        }
        const std::optional<GridPlan> plan = grid_plan(*grid, ctx.where);
        if (!plan) {
            return;
        }
        const Node& body = parts.tilemap->body;
        // Unity puts each sprite's pivot at the tile anchor; a texture origin puts it there.
        const double anchor_x = body.number_at("m_TileAnchor.x", 0.5);
        const double anchor_y = body.number_at("m_TileAnchor.y", 0.5);
        if (plan->layout == 1 && (anchor_x != 0.0 || anchor_y != 0.0)) {
            problems_.add("tile anchor off a hexagon's center", ctx.where,
                          "tiles anchor at their cell's center, as Unity's (0, 0)");
        }
        const std::array<double, 2> shift = plan->shift(anchor_x, anchor_y);
        if (body.integer_at("m_TileOrientation", 0) != 0) {
            problems_.add("tile orientation", ctx.where, "tiles lie in the xy plane");
        }

        // The collider's physics, for the tile set's physics layer.
        std::optional<TilePhysics> physics;
        bool solid = false;
        if (parts.collider != nullptr) {
            const Node& collider = parts.collider->body;
            solid = collider.flag_at("m_Enabled", true) && ctx.active;
            if (collider.flag_at("m_IsTrigger")) {
                problems_.add("trigger TilemapCollider2D", ctx.where,
                              "tile map layers are solid or not; it collides with nothing");
                solid = false;
            }
            if (collider.number_at("m_Offset.x") != 0.0 ||
                collider.number_at("m_Offset.y") != 0.0) {
                problems_.add("TilemapCollider2D offset", ctx.where,
                              "the tiles' polygons stay on their tiles");
            }
            const std::int64_t layer =
                std::clamp<std::int64_t>(go.body.integer_at("m_Layer", 0), 0, 31);
            Material material = default_material_;
            if (const auto ref = ref_at(collider, "m_Material"); ref && !ref->guid.empty()) {
                material = physics_material(ref->guid);
            } else if (parts.rigidbody != nullptr) {
                if (const auto own = ref_at(parts.rigidbody->body, "m_Material");
                    own && !own->guid.empty()) {
                    material = physics_material(own->guid);
                }
            }
            physics =
                TilePhysics{.layer = std::int64_t{1} << layer,
                            .mask = std::int64_t{layer_masks_.at(static_cast<std::size_t>(layer))},
                            .friction = material.friction,
                            .bounce = material.bounce,
                            .one_way = parts.effector != nullptr &&
                                       parts.effector->body.flag_at("m_UseOneWay", true) &&
                                       (collider.flag_at("m_UsedByEffector") ||
                                        (parts.composite != nullptr &&
                                         parts.composite->body.flag_at("m_UsedByEffector")))};
            if (parts.rigidbody != nullptr &&
                parts.rigidbody->body.integer_at("m_BodyType", 0) != 2) {
                problems_.add("tile map moved by a Rigidbody2D", ctx.where,
                              "tile map layers are static bodies; it stays where it is");
            }
        }
        TileSetOut& set = tile_set_for(*plan, physics);

        // Each cell's tile, from the arrays the cells index.
        const auto entry = [&](std::string_view array, std::int64_t index) -> const Node* {
            const Node* list = body.get(array);
            if (list == nullptr || index < 0 ||
                static_cast<std::size_t>(index) >= list->items.size()) {
                return nullptr;
            }
            return list->items[static_cast<std::size_t>(index)].get("m_Data");
        };
        struct Placed {
            const TileOut* tile = nullptr;
            int alternative = 0;
            std::array<bool, 3> turns{};
        };
        std::map<std::pair<std::int64_t, std::int64_t>, Placed> placed;
        std::string filter;
        const Node* tiles = body.get("m_Tiles");
        for (const Node& item : tiles != nullptr ? tiles->items : std::vector<Node>{}) {
            const Node* data = item.get("second");
            if (data == nullptr) {
                continue;
            }
            if (item.integer_at("first.z", 0) != 0) {
                problems_.add("tiles above or below z 0", ctx.where,
                              "a layer holds one tile per cell; the last one wins");
            }
            const auto asset = [&]() -> std::optional<ObjectRef> {
                const Node* ref = entry("m_TileAssetArray", data->integer_at("m_TileIndex", -1));
                return ref != nullptr ? unity::object_ref(*ref) : std::nullopt;
            }();
            // An AnimatedTile shows its first sprite and plays the rest; other scripted tiles
            // keep the sprite each cell shows.
            std::optional<AnimatedTileAsset> animation;
            if (asset && !asset->null() && !plain_tile(*asset)) {
                animation = animated_tile(*asset);
                if (!animation) {
                    problems_.add("scripted tiles (rule tiles and others)", ctx.where,
                                  "each cell keeps the sprite it shows");
                }
            }
            std::optional<ObjectRef> ref;
            if (animation) {
                ref = animation->sprites.front();
            } else if (const Node* sprite_ref =
                           entry("m_TileSpriteArray", data->integer_at("m_TileSpriteIndex", -1))) {
                ref = unity::object_ref(*sprite_ref);
            }
            if (!ref || ref->null()) {
                problems_.add("tiles without a sprite", ctx.where,
                              "left out, with any collision they had");
                continue;
            }
            std::string why;
            const Sprite* sprite = this->sprite(*ref, why);
            if (sprite == nullptr) {
                problems_.add("tile sprite", ctx.where, why);
                continue;
            }
            TileLook look{.shift = shift};
            look.collider = static_cast<std::int64_t>(
                (static_cast<std::uint64_t>(data->integer_at("m_AllTileFlags", 0)) >> 30U) & 3U);
            if (const Node* color =
                    entry("m_TileColorArray", data->integer_at("m_TileColorIndex", -1))) {
                look.color = {color->number_at("r", 1.0), color->number_at("g", 1.0),
                              color->number_at("b", 1.0), color->number_at("a", 1.0)};
            }
            const Node* matrix =
                entry("m_TileMatrixArray", data->integer_at("m_TileMatrixIndex", -1));
            auto turns = tile_turns(matrix);
            if (!turns) {
                problems_.add("tiles scaled or turned other than by quarter turns", ctx.where,
                              "drawn unturned");
                turns = std::array<bool, 3>{};
            }
            if (std::ranges::any_of(*turns, std::identity{}) && shift != std::array<double, 2>{}) {
                problems_.add("turned tiles with the tile anchor off the cell's center", ctx.where,
                              "they turn about the cell's center");
            }
            if (matrix != nullptr &&
                (matrix->number_at("e03") != 0.0 || matrix->number_at("e13") != 0.0)) {
                problems_.add("tile offsets", ctx.where, "tiles draw centered on their cells");
            }
            const auto [tile, alternative] = tile_for(set, *ref, *sprite, look, ctx.where);
            if (animation) {
                animate(set, *tile, *animation, body.number_at("m_AnimationFrameRate", 1.0),
                        ctx.where);
            } else {
                tile->still = true;
            }
            if (filter.empty()) {
                filter = sprite->filter;
            }
            const Vec2i at = plan->cell(static_cast<int>(item.integer_at("first.x", 0)),
                                        static_cast<int>(item.integer_at("first.y", 0)));
            placed[{at.x, at.y}] =
                Placed{.tile = tile, .alternative = alternative, .turns = *turns};
            if (asset && !asset->null()) {
                if (const Asset* file = asset_of(asset->guid); file != nullptr) {
                    tile_assets_.try_emplace(
                        destination(file->path),
                        std::pair{set.path, alternative == 0
                                                ? tile->name
                                                : tile->name + ":" + std::to_string(alternative)});
                }
            }
        }

        json::Value layer = json::Object{};
        if (!set.path.empty()) {
            layer["tile_set"] = set.path;
            settle_animations(set, ctx.where);
            const json::Value doc = tile_set_doc(set);
            auto parsed = canvas::parse_tileset(doc);
            if (!parsed) {
                problems_.add("tile map cells", ctx.where, parsed.error().message);
            } else {
                canvas::Cells cells;
                for (const auto& [at, cell_tile] : placed) {
                    cells[{static_cast<int>(at.first), static_cast<int>(at.second)}] =
                        canvas::TileRef{.source = cell_tile.tile->source,
                                        .atlas = {static_cast<int>(cell_tile.tile->atlas[0]),
                                                  static_cast<int>(cell_tile.tile->atlas[1])},
                                        .alternative = cell_tile.alternative,
                                        .flip_h = cell_tile.turns[0],
                                        .flip_v = cell_tile.turns[1],
                                        .transpose = cell_tile.turns[2]};
                }
                world::TileMapLayer encoded;
                canvas::encode_cells(cells, parsed.value(), encoded);
                layer["origin"] = json::Array{encoded.origin.x, encoded.origin.y};
                json::Object legend;
                for (const auto& [key, text] : encoded.legend) {
                    legend.emplace(key, text);
                }
                layer["legend"] = std::move(legend);
                json::Array rows;
                for (const std::string& row : encoded.rows) {
                    rows.emplace_back(row);
                }
                layer["rows"] = std::move(rows);
            }
            write_tile_set(set);
        }
        if (parts.renderer != nullptr) {
            const Node& renderer = parts.renderer->body;
            layer["z_index"] = z_index(renderer, ctx.where);
            layer["visible"] = renderer.flag_at("m_Enabled", true) && ctx.active;
            if (const Node* materials = renderer.get("m_Materials");
                materials != nullptr && !materials->items.empty()) {
                if (const auto material = unity::object_ref(materials->items.front());
                    material && asset_of(material->guid) != nullptr) {
                    problems_.add("material", ctx.where,
                                  "materials and shaders don't convert; the tiles draw with the "
                                  "default material");
                }
            }
            if (renderer.integer_at("m_MaskInteraction", 0) != 0) {
                problems_.add("sprite mask interaction", ctx.where,
                              "sprite masks aren't supported");
            }
        } else {
            layer["visible"] = false; // a Tilemap draws through its TilemapRenderer
        }
        if (const Node* color = body.get("m_Color")) {
            layer["modulate"] = json::Array{color->number_at("r", 1.0), color->number_at("g", 1.0),
                                            color->number_at("b", 1.0), color->number_at("a", 1.0)};
        }
        const std::string project_filter =
            nearest_textures_ > linear_textures_ ? "nearest" : "linear";
        if (!filter.empty() && filter != project_filter) {
            layer["texture_filter"] = filter;
        }
        layer["collision_enabled"] = solid;
        out.components["TileMapLayer"] = std::move(layer);
        if (plan->offset != Vec2{}) {
            // Unity's cells sit off the tile set's: the layer moves to put them where they are in
            // Unity, and its children move back.
            world::Transform2D local;
            if (const Component* transform = transform_of(go)) {
                local = local_2d(transform->body);
            }
            const Vec2 moved = world::affine(local).apply(plan->offset);
            out.components["Transform2D"]["position"] = pair(moved.x, moved.y);
            out.child_shift = plan->offset;
        }
    }

    // --- Animation ---

    // A key of a Unity curve: a value with Hermite tangents, which may be weighted.
    struct CurveKey {
        double time = 0.0;
        double value = 0.0;
        double in_slope = 0.0;
        double out_slope = 0.0;
        double in_weight = kUnweighted;
        double out_weight = kUnweighted;
        std::int64_t weighted = 0; // 1: the in tangent's weight counts, 2: the out tangent's
    };
    using Curve = std::vector<CurveKey>;

    // A curve's keys, or one part of a vector curve's ("x", "y", "z" or "w").
    static Curve read_curve(const Node* curve, std::string_view part) {
        Curve out;
        const Node* keys = curve != nullptr ? curve->get("m_Curve") : nullptr;
        for (const Node& key : keys != nullptr ? keys->items : std::vector<Node>{}) {
            const auto at = [&](std::string_view name, double fallback) {
                const Node* node = key.get(name);
                if (node != nullptr && !part.empty()) {
                    node = node->get(part);
                }
                const auto value = node != nullptr ? node->number() : std::nullopt;
                return value.value_or(fallback);
            };
            const double time = key.number_at("time");
            if (!out.empty() && time <= out.back().time) {
                continue;
            }
            out.push_back(CurveKey{.time = time,
                                   .value = at("value", 0.0),
                                   .in_slope = at("inSlope", 0.0),
                                   .out_slope = at("outSlope", 0.0),
                                   .in_weight = at("inWeight", kUnweighted),
                                   .out_weight = at("outWeight", kUnweighted),
                                   .weighted = key.integer_at("weightedMode", 0)});
        }
        return out;
    }

    enum class Segment : std::uint8_t { Linear, Stepped, Curved };

    // The weights of a segment's tangents, as Unity uses them.
    static std::pair<double, double> weights(const CurveKey& a, const CurveKey& b) {
        return {(a.weighted & 2) != 0 ? std::clamp(a.out_weight, 0.0, 1.0) : kUnweighted,
                (b.weighted & 1) != 0 ? std::clamp(b.in_weight, 0.0, 1.0) : kUnweighted};
    }

    static Segment segment(const CurveKey& a, const CurveKey& b) {
        if (!std::isfinite(a.out_slope) || !std::isfinite(b.in_slope)) {
            return Segment::Stepped;
        }
        const double span = b.time - a.time;
        const double slope = span > 0.0 ? (b.value - a.value) / span : 0.0;
        const double tolerance = 1e-4 * std::max(1.0, std::abs(slope));
        return std::abs(a.out_slope - slope) <= tolerance &&
                       std::abs(b.in_slope - slope) <= tolerance
                   ? Segment::Linear
                   : Segment::Curved;
    }

    // A segment's value at a time between its keys: Unity's Hermite curve, or with weighted
    // tangents its Bezier curve in time and value.
    static double evaluate(const CurveKey& a, const CurveKey& b, double time) {
        const double span = b.time - a.time;
        if (span <= 0.0 || segment(a, b) == Segment::Stepped) {
            return time >= b.time ? b.value : a.value;
        }
        const auto [w0, w1] = weights(a, b);
        const double s = std::clamp((time - a.time) / span, 0.0, 1.0);
        if (std::abs(w0 - kUnweighted) > 1e-6 || std::abs(w1 - kUnweighted) > 1e-6) {
            const auto bezier = [](double p0, double p1, double p2, double p3, double u) {
                const double v = 1.0 - u;
                return (v * v * v * p0) + (3.0 * v * v * u * p1) + (3.0 * v * u * u * p2) +
                       (u * u * u * p3);
            };
            const double x1 = a.time + (w0 * span);
            const double x2 = b.time - (w1 * span);
            double low = 0.0;
            double high = 1.0;
            for (int i = 0; i < 48; ++i) {
                const double middle = (low + high) / 2.0;
                (bezier(a.time, x1, x2, b.time, middle) < time ? low : high) = middle;
            }
            const double u = (low + high) / 2.0;
            return bezier(a.value, a.value + (w0 * span * a.out_slope),
                          b.value - (w1 * span * b.in_slope), b.value, u);
        }
        const double s2 = s * s;
        const double s3 = s2 * s;
        return ((2.0 * s3 - 3.0 * s2 + 1.0) * a.value) +
               ((s3 - 2.0 * s2 + s) * span * a.out_slope) + ((-2.0 * s3 + 3.0 * s2) * b.value) +
               ((s3 - s2) * span * b.in_slope);
    }

    // A curve's value at any time: its first key's before it and its last's after it.
    static double evaluate(const Curve& curve, double time) {
        if (curve.empty()) {
            return 0.0;
        }
        if (time <= curve.front().time) {
            return curve.front().value;
        }
        for (std::size_t i = 0; i + 1 < curve.size(); ++i) {
            if (time < curve[i + 1].time) {
                return evaluate(curve[i], curve[i + 1], time);
            }
        }
        return curve.back().value;
    }

    // The part of a clip that plays: from its start time to its stop time.
    struct ClipTime {
        double start = 0.0;
        double stop = 1.0;

        [[nodiscard]] double length() const { return stop - start; }
        // A time in the clip as a time in its animation.
        [[nodiscard]] double at(double time) const {
            return std::clamp(time - start, 0.0, length());
        }
    };

    // The times a track keys a curve at, and whether each holds its value to the next: every
    // key, and between keys a curved segment joins, a key each 60th of a second, since
    // SlashEngine's curves aren't Unity's.
    struct Sample {
        double time = 0.0; // in the clip
        bool hold = false;
    };

    static std::vector<Sample> samples(const std::vector<const Curve*>& curves,
                                       const ClipTime& clip, bool discrete) {
        std::set<double> times;
        std::set<double> holds;
        for (const Curve* curve : curves) {
            for (std::size_t i = 0; i < curve->size(); ++i) {
                const CurveKey& key = (*curve)[i];
                times.insert(key.time);
                if (i + 1 == curve->size() || discrete) {
                    continue;
                }
                const CurveKey& next = (*curve)[i + 1];
                const Segment kind = segment(key, next);
                if (kind == Segment::Stepped) {
                    holds.insert(key.time);
                } else if (kind == Segment::Curved) {
                    const double count = ((next.time - key.time) / kSampleStep) - 0.5;
                    for (int n = 1; n < count; ++n) {
                        times.insert(key.time + (static_cast<double>(n) * kSampleStep));
                    }
                }
            }
        }
        // Only the clip's part, with its ends where keys run past them.
        std::vector<Sample> out;
        const auto inside = [&](double t) { return t >= clip.start && t <= clip.stop; };
        if (!times.empty() && *times.begin() < clip.start) {
            out.push_back({.time = clip.start, .hold = false});
        }
        for (const double t : times) {
            if (inside(t)) {
                out.push_back({.time = t, .hold = holds.contains(t)});
            }
        }
        if (!times.empty() && *times.rbegin() > clip.stop) {
            out.push_back({.time = clip.stop, .hold = false});
        }
        return out;
    }

    // A value track's keys: the value curves give at each sample, made a field's value by
    // convert, at the times the clip plays them. Keys that land on one time keep the last.
    static json::Array track_keys(const std::vector<Sample>& at, const ClipTime& clip,
                                  const std::function<json::Value(double)>& value_at) {
        json::Array keys;
        double last = -1.0;
        for (const Sample& sample : at) {
            const double time = tidy(clip.at(sample.time));
            json::Value key;
            key["time"] = time;
            key["value"] = value_at(sample.time);
            if (sample.hold) {
                key["transition"] = 0;
            }
            if (time <= last && !keys.empty()) {
                keys.back() = std::move(key);
            } else {
                keys.push_back(std::move(key));
            }
            last = time;
        }
        return keys;
    }

    // Samples with more between them wherever the value turns more than a little from one to the
    // next, by the turn distance gives between two times in radians: Unity interpolates a
    // rotation's parts and SlashEngine turns the shorter way, which agree over small turns.
    static std::vector<Sample>
    dense_samples(const std::vector<Sample>& at,
                  const std::function<double(double, double)>& distance) {
        constexpr double kMostTurn = 0.1; // about 6 degrees
        constexpr int kMostSteps = 360;
        std::vector<Sample> out;
        for (std::size_t i = 0; i < at.size(); ++i) {
            out.push_back(at[i]);
            if (i + 1 == at.size() || at[i].hold) {
                continue;
            }
            const double from = at[i].time;
            const double to = at[i + 1].time;
            const int steps =
                std::min(static_cast<int>(std::ceil(distance(from, to) / kMostTurn)), kMostSteps);
            for (int n = 1; n < steps; ++n) {
                const double part = static_cast<double>(n) / static_cast<double>(steps);
                out.push_back({.time = from + ((to - from) * part), .hold = false});
            }
        }
        return out;
    }

    // A node track's keys, which hold no value to the next key as a value track's may: a key that
    // held one is followed by its value again just before the next key.
    static void unhold(json::Value& track) {
        constexpr double kBefore = 0.0001;
        const json::Array& in = track.find("keys")->as_array();
        json::Array keys;
        for (std::size_t i = 0; i < in.size(); ++i) {
            json::Value key = in[i];
            const bool hold = key.find("transition") != nullptr;
            if (hold) {
                key.as_object().erase("transition");
            }
            keys.push_back(key);
            if (!hold || i + 1 == in.size()) {
                continue;
            }
            const double before = tidy(in[i + 1].find("time")->as_double() - kBefore);
            if (before > key.find("time")->as_double()) {
                key["time"] = before;
                keys.push_back(std::move(key));
            }
        }
        track["keys"] = std::move(keys);
    }

    // An animation of a clip, before a player binds its targets: tracks name the object they
    // animate by its path from the Animator's object ("" for that object, "Body/Arm" below it),
    // and the entity drawing an object's sprite as "<path>#sprite".
    struct ClipOut {
        bool ok = false;
        std::string name;
        json::Value animation;                              // its length, loop and tracks
        std::vector<std::pair<std::string, double>> events; // functions and times, for tasks
        bool offsets = false;    // whether its sprites' pivots are off their middles
        bool retargeted = false; // a humanoid clip of another model, made for the one playing it
    };

    // The class names of components clips animate, for reports.
    static std::string class_name(std::int64_t class_id) {
        static const std::map<std::int64_t, std::string_view> kNames{
            {kGameObject, "GameObject"},
            {kTransform, "Transform"},
            {kCamera, "Camera"},
            {kRigidbody2D, "Rigidbody2D"},
            {kCircleCollider2D, "CircleCollider2D"},
            {kPolygonCollider2D, "PolygonCollider2D"},
            {kBoxCollider2D, "BoxCollider2D"},
            {kEdgeCollider2D, "EdgeCollider2D"},
            {kCapsuleCollider2D, "CapsuleCollider2D"},
            {kAudioSource, "AudioSource"},
            {kAnimator, "Animator"},
            {kLight, "Light"},
            {kMeshRenderer, "MeshRenderer"},
            {kMonoBehaviour, "MonoBehaviour"},
            {kSpriteRenderer, "SpriteRenderer"},
            {kRectTransform, "RectTransform"},
        };
        const auto it = kNames.find(class_id);
        return it != kNames.end() ? std::string(it->second)
                                  : "component class " + std::to_string(class_id);
    }

    // How a clip's curves move what they animate: a 2D object's Transform2D and sprites, a 3D
    // object's Transform3D, or, below an Animator on a model's instance, the model's nodes.
    enum class ClipSpace : std::uint8_t { Flat, Objects, Model };

    const ClipOut& clip(const ObjectRef& ref, const std::string& where, ClipSpace space) {
        static constexpr std::array<std::string_view, 3> kSpaces{"", ":3d", ":model"};
        // A model's clips are made for the model whose instance plays them.
        const std::string key = ref.guid + ":" + std::to_string(ref.file_id) +
                                std::string(kSpaces.at(static_cast<std::size_t>(space))) +
                                (space == ClipSpace::Model ? ":" + clip_target_ : std::string());
        auto [it, inserted] = clips_.try_emplace(key);
        ClipOut& out = it->second;
        if (!inserted) {
            return out;
        }
        const Asset* asset = asset_of(ref.guid);
        if (asset == nullptr) {
            problems_.add("animation clip", where,
                          "a clip from outside the project's Assets folder");
            return out;
        }
        if (space != ClipSpace::Flat && is_model_file(extension_of(asset->path))) {
            if (space == ClipSpace::Model) {
                model_clip(ref, *asset, where, out);
            } else {
                problems_.add("animation clip of a model", where,
                              asset->path + ": a model's clips move its nodes, so they play on "
                                            "an Animator on the model's instance");
            }
            return out;
        }
        if (extension_of(asset->path) != ".anim") {
            const std::string why = unsupported_asset(extension_of(asset->path));
            problems_.add("animation clip", where,
                          asset->path + ": " + (why.empty() ? "not an .anim file" : why));
            return out;
        }
        auto text = platform::read_file(source_ + asset->path);
        auto objects =
            text ? unity::parse_file(text.value()) : Result<std::vector<Object>>(text.error());
        if (!objects) {
            problems_.add("unreadable file", asset->path, objects.error().message);
            return out;
        }
        const Object* found = nullptr;
        for (const Object& object : objects.value()) {
            if (object.class_id == kAnimationClip &&
                (found == nullptr || object.file_id == ref.file_id)) {
                found = &object;
            }
        }
        if (found == nullptr) {
            problems_.add("animation clip", asset->path, "no AnimationClip in the file");
            return out;
        }
        convert_clip(found->body, asset->path, space, out);
        out.ok = true;
        return out;
    }

    void convert_clip(const Node& body, const std::string& where, ClipSpace space, ClipOut& out) {
        out.name = body.string_at("m_Name");
        ClipTime time;
        time.start = body.number_at("m_AnimationClipSettings.m_StartTime");
        time.stop = body.number_at("m_AnimationClipSettings.m_StopTime");
        bool loop = body.flag_at("m_AnimationClipSettings.m_LoopTime");
        if (body.flag_at("m_Legacy")) {
            // Legacy clips loop by their wrap mode: 2 loops, 4 ping-pongs.
            const std::int64_t wrap = body.integer_at("m_WrapMode", 0);
            loop = wrap == 2;
            if (wrap == 4) {
                out.animation["loop"] = "pingpong";
            }
        }
        if (loop) {
            out.animation["loop"] = "linear";
        }

        // Every track, by its target and path, so a property set twice keeps the first.
        std::map<std::pair<std::string, std::string>, json::Value> tracks;
        double last_key = 0.0;
        const auto add = [&](const std::string& target, const std::string& path,
                             json::Value track) {
            if (const json::Value* keys = track.find("keys");
                keys == nullptr || keys->as_array().empty()) {
                return;
            }
            if (!target.empty()) {
                track["target"] = target;
            }
            track["path"] = path;
            tracks.try_emplace({target, path}, std::move(track));
        };
        // Clip times, once the clip's stop time is known.
        std::vector<std::function<void()>> pending;
        const auto float_track = [&](const std::string& target, const std::string& path,
                                     Curve values, std::function<json::Value(double)> to_field,
                                     bool discrete) {
            if (values.empty()) {
                return;
            }
            last_key = std::max(last_key, values.back().time);
            pending.emplace_back([&, target, path, curve = std::move(values),
                                  convert = std::move(to_field), discrete] {
                json::Value track;
                track["keys"] = track_keys(samples({&curve}, time, discrete), time,
                                           [&](double t) { return convert(evaluate(curve, t)); });
                add(target, path, std::move(track));
            });
        };
        const auto scaled = [](double scale) {
            return [scale](double value) -> json::Value { return tidy(value * scale); };
        };
        const auto angle = [](double degrees) -> json::Value {
            return tidy_angle(-degrees * std::numbers::pi / 180.0);
        };
        const auto plain = [](double value) -> json::Value { return tidy(value); };
        const auto flag = [](double value) -> json::Value { return value > 0.5; };

        const auto list = [&](std::string_view key) -> const std::vector<Node>& {
            static const std::vector<Node> kNone;
            const Node* node = body.get(key);
            return node != nullptr ? node->items : kNone;
        };
        // A 2D clip's curves, then a 3D one's.
        const auto flat = [&](std::string_view key) -> const std::vector<Node>& {
            static const std::vector<Node> kNone;
            return space == ClipSpace::Flat ? list(key) : kNone;
        };
        const auto solid = [&](std::string_view key) -> const std::vector<Node>& {
            static const std::vector<Node> kNone;
            return space != ClipSpace::Flat ? list(key) : kNone;
        };
        for (const Node& item : flat("m_PositionCurves")) {
            const std::string target = item.string_at("path");
            const Node* curve = item.get("curve");
            float_track(target, "Transform2D/position/x", read_curve(curve, "x"), scaled(ppu_),
                        false);
            float_track(target, "Transform2D/position/y", read_curve(curve, "y"), scaled(-ppu_),
                        false);
        }
        for (const Node& item : flat("m_ScaleCurves")) {
            const std::string target = item.string_at("path");
            const Node* curve = item.get("curve");
            float_track(target, "Transform2D/scale/x", read_curve(curve, "x"), plain, false);
            float_track(target, "Transform2D/scale/y", read_curve(curve, "y"), plain, false);
        }
        const auto tilted = [&](const Curve& curve) {
            return std::ranges::any_of(curve, [](const CurveKey& key) { return key.value != 0.0; });
        };
        for (const Node& item : flat("m_EulerCurves")) {
            const std::string target = item.string_at("path");
            const Node* curve = item.get("curve");
            if (tilted(read_curve(curve, "x")) || tilted(read_curve(curve, "y"))) {
                problems_.add("animated rotation about x or y", where,
                              "2D objects turn about z only; the rest is dropped");
            }
            float_track(target, "Transform2D/rotation", read_curve(curve, "z"), angle, false);
        }
        for (const Node& item : flat("m_RotationCurves")) {
            // Quaternions: each part interpolates on its own, as Unity does, then the turn about
            // z is taken from them, at every key and sample.
            const std::string target = item.string_at("path");
            const Node* curve = item.get("curve");
            auto parts = std::make_shared<std::array<Curve, 4>>(
                std::array<Curve, 4>{read_curve(curve, "x"), read_curve(curve, "y"),
                                     read_curve(curve, "z"), read_curve(curve, "w")});
            if (tilted((*parts)[0]) || tilted((*parts)[1])) {
                problems_.add("animated rotation about x or y", where,
                              "2D objects turn about z only; the rest is dropped");
            }
            if ((*parts)[3].empty()) {
                continue;
            }
            last_key = std::max(last_key, (*parts)[3].back().time);
            pending.emplace_back([&, target, parts] {
                const Curve& qx = (*parts)[0];
                const Curve& qy = (*parts)[1];
                const Curve& qz = (*parts)[2];
                const Curve& qw = (*parts)[3];
                json::Value track;
                track["keys"] =
                    track_keys(samples({&qx, &qy, &qz, &qw}, time, false), time, [&](double t) {
                        const double x = evaluate(qx, t);
                        const double y = evaluate(qy, t);
                        const double z = evaluate(qz, t);
                        const double w = evaluate(qw, t);
                        const double turn = std::atan2(2.0 * ((w * z) + (x * y)),
                                                       1.0 - (2.0 * ((y * y) + (z * z))));
                        return json::Value(tidy_angle(-turn));
                    });
                track["interpolation"] = "linear_angle";
                add(target, "Transform2D/rotation", std::move(track));
            });
        }
        for (const Node& item : flat("m_FloatCurves")) {
            const std::string target = item.string_at("path");
            const std::string sprite_target = target + "#sprite";
            const std::string attribute = item.string_at("attribute");
            const std::int64_t class_id = item.integer_at("classID", 0);
            Curve curve = read_curve(item.get("curve"), {});
            if (class_id == kTransform) {
                if (attribute == "m_LocalPosition.x") {
                    float_track(target, "Transform2D/position/x", std::move(curve), scaled(ppu_),
                                false);
                } else if (attribute == "m_LocalPosition.y") {
                    float_track(target, "Transform2D/position/y", std::move(curve), scaled(-ppu_),
                                false);
                } else if (attribute == "m_LocalScale.x" || attribute == "m_LocalScale.y") {
                    float_track(target, "Transform2D/scale/" + attribute.substr(13),
                                std::move(curve), plain, false);
                } else if (attribute == "localEulerAnglesRaw.z" ||
                           attribute == "localEulerAngles.z") {
                    float_track(target, "Transform2D/rotation", std::move(curve), angle, false);
                } else if (attribute.ends_with(".z") && attribute.starts_with("m_Local")) {
                    // Depth: SlashEngine orders by z_index.
                } else if (tilted(curve)) {
                    problems_.add("animated rotation about x or y", where,
                                  "2D objects turn about z only; the rest is dropped");
                }
                continue;
            }
            if (class_id == kSpriteRenderer) {
                if (attribute.starts_with("m_Color.") && attribute.size() == 9) {
                    float_track(sprite_target, "Sprite2D/modulate/" + attribute.substr(8),
                                std::move(curve), plain, false);
                    continue;
                }
                const std::map<std::string, std::string, std::less<>> kFlags{
                    {"m_Enabled", "visible"}, {"m_FlipX", "flip_h"}, {"m_FlipY", "flip_v"}};
                if (const auto it = kFlags.find(attribute); it != kFlags.end()) {
                    float_track(sprite_target, "Sprite2D/" + it->second, std::move(curve), flag,
                                true);
                    continue;
                }
            }
            if (class_id == kGameObject && attribute == "m_IsActive") {
                problems_.add("animated m_IsActive", where,
                              "shows and hides the object's sprites and those under it; its "
                              "other components still run");
                float_track(target, "Sprite2D/visible", std::move(curve), flag, true);
                continue;
            }
            if (class_id == kMonoBehaviour) {
                problems_.add("animated script value",
                              std::string(where).append(": ").append(attribute),
                              "set it from gameplay code; the script's storyboard task says what "
                              "it does");
                continue;
            }
            problems_.add("animated " + class_name(class_id) + "." + attribute, where,
                          "no SlashEngine field it maps to");
        }
        for (const Node& item : flat("m_PPtrCurves")) {
            const std::string target = item.string_at("path");
            const std::string attribute = item.string_at("attribute");
            const std::int64_t class_id = item.integer_at("classID", 0);
            if (class_id != kSpriteRenderer || attribute != "m_Sprite") {
                problems_.add("animated " + class_name(class_id) + "." + attribute, where,
                              "no SlashEngine field it maps to");
                continue;
            }
            // Each key's sprite as the texture, region and offset its Sprite2D takes.
            struct Frame {
                double time = 0.0;
                json::Value texture;
                json::Value region;
                json::Value offset;
            };
            auto frames = std::make_shared<std::vector<Frame>>();
            const Node* keys = item.get("curve");
            for (const Node& key : keys != nullptr ? keys->items : std::vector<Node>{}) {
                Frame frame{.time = key.number_at("time"),
                            .texture = "",
                            .region = json::Array{0.0, 0.0, 0.0, 0.0},
                            .offset = pair(0.0, 0.0)};
                if (const auto ref = ref_at(key, "value"); ref && !ref->null()) {
                    std::string why;
                    if (const Sprite* found = sprite(*ref, why)) {
                        frame.texture = found->texture;
                        if (!found->whole) {
                            frame.region = json::Array{found->region[0], found->region[1],
                                                       found->region[2], found->region[3]};
                        }
                        const double x = (0.5 - found->pivot_x) * found->width;
                        const double y = (found->pivot_y - 0.5) * found->height;
                        out.offsets = out.offsets || x != 0.0 || y != 0.0;
                        frame.offset = pair(x, y);
                        if (std::abs(found->ppu - ppu_) > 1e-6) {
                            problems_.add("animated sprite with other pixels per unit", where,
                                          "drawn at its size in pixels, unscaled");
                        }
                    } else {
                        problems_.add("animated sprite", where, why);
                    }
                }
                if (!frames->empty() && frame.time <= frames->back().time) {
                    continue;
                }
                last_key = std::max(last_key, frame.time);
                frames->push_back(std::move(frame));
            }
            pending.emplace_back([&, target, frames] {
                for (const std::string_view field : {"texture", "region", "offset"}) {
                    json::Array out_keys;
                    for (std::size_t i = 0; i < frames->size(); ++i) {
                        const Frame& frame = (*frames)[i];
                        // The frame showing at the clip's start counts from there.
                        const bool next_inside =
                            i + 1 < frames->size() && (*frames)[i + 1].time <= time.start;
                        if (next_inside || frame.time > time.stop) {
                            continue;
                        }
                        json::Value key;
                        key["time"] = tidy(time.at(frame.time));
                        if (field == "texture") {
                            key["value"] = frame.texture;
                        } else {
                            key["value"] = field == "region" ? frame.region : frame.offset;
                        }
                        if (!out_keys.empty() && out_keys.back().find("time")->as_double() >=
                                                     key.find("time")->as_double()) {
                            out_keys.back() = std::move(key);
                        } else {
                            out_keys.push_back(std::move(key));
                        }
                    }
                    json::Value track;
                    track["keys"] = std::move(out_keys);
                    if (field != "texture") {
                        track["update"] = "discrete";
                    }
                    add(target + "#sprite", "Sprite2D/" + std::string(field), std::move(track));
                }
            });
        }

        // 3D: what moves an object's Transform3D, mirrored in x as the scene is: positions' x
        // negated, and turns about y and z backwards, so Unity's Euler angles (applied z, x, then
        // y, as Godot's YXZ ones) are [x, -y, -z]. On a model's instance, a path below the
        // Animator's object names a node of the model, which a node track moves.
        const auto on_node = [&](const std::string& target) {
            return space == ClipSpace::Model && !target.empty();
        };
        const auto add_3d = [&](const std::string& target, const std::string& property,
                                json::Value track) {
            if (!on_node(target)) {
                add(target, "Transform3D/" + property, std::move(track));
                return;
            }
            if (const json::Value* keys = track.find("keys");
                keys == nullptr || keys->as_array().empty()) {
                return;
            }
            // A node turns the shorter way from key to key, and holds no key's value to the next.
            track.as_object().erase("interpolation");
            unhold(track);
            const auto slash = target.rfind('/');
            const std::string node = slash == std::string::npos ? target : target.substr(slash + 1);
            track["node"] = node;
            track["property"] = property;
            tracks.try_emplace({"#" + node, property}, std::move(track));
        };
        // A track of [x, y, z] values from three curves, keyed where any of them is. A node's
        // turns are sampled closely, since Unity turns each Euler angle on its own.
        using Vector = std::function<json::Value(const std::array<double, 3>&)>;
        const auto vector_track = [&](const std::string& target, const std::string& property,
                                      const Node* curve, Vector value, bool turns) {
            auto parts = std::make_shared<std::array<Curve, 3>>(std::array<Curve, 3>{
                read_curve(curve, "x"), read_curve(curve, "y"), read_curve(curve, "z")});
            if (std::ranges::all_of(*parts, [](const Curve& part) { return part.empty(); })) {
                return;
            }
            for (const Curve& part : *parts) {
                if (!part.empty()) {
                    last_key = std::max(last_key, part.back().time);
                }
            }
            pending.emplace_back([&, target, property, parts, value = std::move(value), turns] {
                const auto at = [&](double t) {
                    return std::array<double, 3>{evaluate((*parts)[0], t), evaluate((*parts)[1], t),
                                                 evaluate((*parts)[2], t)};
                };
                std::vector<Sample> times =
                    samples({parts->data(), &(*parts)[1], &(*parts)[2]}, time, false);
                if (turns && on_node(target)) {
                    times = dense_samples(times, [&](double from, double to) {
                        const std::array<double, 3> a = at(from);
                        const std::array<double, 3> b = at(to);
                        double most = 0.0;
                        for (std::size_t i = 0; i < 3; ++i) {
                            most = std::max(most, std::abs(b.at(i) - a.at(i)));
                        }
                        return most * std::numbers::pi / 180.0;
                    });
                }
                json::Value track;
                track["keys"] = track_keys(times, time, [&](double t) { return value(at(t)); });
                add_3d(target, property, std::move(track));
            });
        };
        const auto mirrored = [](const std::array<double, 3>& v) -> json::Value {
            return json::Array{tidy(-v[0]), tidy(v[1]), tidy(v[2])};
        };
        const auto as_is = [](const std::array<double, 3>& v) -> json::Value {
            return json::Array{tidy(v[0]), tidy(v[1]), tidy(v[2])};
        };
        const auto euler = [](const std::array<double, 3>& v) -> json::Value {
            constexpr double kRadians = std::numbers::pi / 180.0;
            return json::Array{tidy_angle(v[0] * kRadians), tidy_angle(-v[1] * kRadians),
                               tidy_angle(-v[2] * kRadians)};
        };
        for (const Node& item : solid("m_PositionCurves")) {
            vector_track(item.string_at("path"), "position", item.get("curve"), mirrored, false);
        }
        for (const Node& item : solid("m_ScaleCurves")) {
            vector_track(item.string_at("path"), "scale", item.get("curve"), as_is, false);
        }
        for (const Node& item : solid("m_EulerCurves")) {
            vector_track(item.string_at("path"), "rotation", item.get("curve"), euler, true);
        }
        for (const Node& item : solid("m_RotationCurves")) {
            // Quaternions, each part interpolated on its own as Unity does, mirrored, then as
            // Euler angles turning the shorter way, keyed often enough to follow the turn.
            const std::string target = item.string_at("path");
            const Node* curve = item.get("curve");
            auto parts = std::make_shared<std::array<Curve, 4>>(
                std::array<Curve, 4>{read_curve(curve, "x"), read_curve(curve, "y"),
                                     read_curve(curve, "z"), read_curve(curve, "w")});
            if ((*parts)[3].empty()) {
                continue;
            }
            last_key = std::max(last_key, (*parts)[3].back().time);
            pending.emplace_back([&, target, parts] {
                const auto turn = [&](double t) {
                    Quaternion q{evaluate((*parts)[0], t), -evaluate((*parts)[1], t),
                                 -evaluate((*parts)[2], t), evaluate((*parts)[3], t)};
                    const double length =
                        std::sqrt((q[0] * q[0]) + (q[1] * q[1]) + (q[2] * q[2]) + (q[3] * q[3]));
                    for (double& part : q) {
                        part = length > 0.0 ? part / length : 0.0;
                    }
                    return q;
                };
                const std::vector<Sample> times = dense_samples(
                    samples({parts->data(), &(*parts)[1], &(*parts)[2], &(*parts)[3]}, time, false),
                    [&](double from, double to) {
                        const Quaternion a = turn(from);
                        const Quaternion b = turn(to);
                        const double dot =
                            std::abs((a[0] * b[0]) + (a[1] * b[1]) + (a[2] * b[2]) + (a[3] * b[3]));
                        return 2.0 * std::acos(std::min(dot, 1.0));
                    });
                json::Value track;
                track["keys"] = track_keys(times, time, [&](double t) {
                    const std::array<double, 3> angles = quaternion_euler(turn(t));
                    return json::Value(json::Array{angles[0], angles[1], angles[2]});
                });
                track["interpolation"] = "linear_angle";
                add_3d(target, "rotation", std::move(track));
            });
        }
        const auto radians = [](double sign) {
            return [sign](double degrees) -> json::Value {
                return tidy_angle(sign * degrees * std::numbers::pi / 180.0);
            };
        };
        bool animator_values = false;
        for (const Node& item : solid("m_FloatCurves")) {
            const std::string target = item.string_at("path");
            const std::string attribute = item.string_at("attribute");
            const std::int64_t class_id = item.integer_at("classID", 0);
            Curve curve = read_curve(item.get("curve"), {});
            if (on_node(target) && (class_id == kTransform || class_id == kGameObject)) {
                problems_.add("animated " + class_name(class_id) + "." + attribute +
                                  " of a model's node",
                              where, "node tracks move a node's whole position, rotation or scale");
                continue;
            }
            if (class_id == kTransform) {
                const auto dot = attribute.rfind('.');
                const std::string field = attribute.substr(0, dot);
                const std::string axis = dot == std::string::npos ? "" : attribute.substr(dot + 1);
                if (axis != "x" && axis != "y" && axis != "z") {
                    problems_.add("animated Transform." + attribute, where,
                                  "no SlashEngine field it maps to");
                } else if (field == "m_LocalPosition") {
                    float_track(target, "Transform3D/position/" + axis, std::move(curve),
                                scaled(axis == "x" ? -1.0 : 1.0), false);
                } else if (field == "m_LocalScale") {
                    float_track(target, "Transform3D/scale/" + axis, std::move(curve), plain,
                                false);
                } else if (field.starts_with("localEulerAngles")) {
                    float_track(target, "Transform3D/rotation/" + axis, std::move(curve),
                                radians(axis == "x" ? 1.0 : -1.0), false);
                } else {
                    problems_.add("animated Transform." + attribute, where,
                                  "a rotation's parts turn together; key its Euler angles");
                }
                continue;
            }
            if (class_id == kGameObject && attribute == "m_IsActive") {
                problems_.add("animated m_IsActive", where,
                              "shows and hides the object and those under it; their other "
                              "components still run");
                float_track(target, "Transform3D/visible", std::move(curve), flag, true);
                continue;
            }
            if (class_id == kAnimator) {
                animator_values = true;
                continue;
            }
            if (class_id == kMonoBehaviour) {
                problems_.add("animated script value",
                              std::string(where).append(": ").append(attribute),
                              "set it from gameplay code; the script's storyboard task says what "
                              "it does");
                continue;
            }
            problems_.add("animated " + class_name(class_id) + "." + attribute, where,
                          "no SlashEngine field it maps to");
        }
        if (animator_values) {
            problems_.add("animated Animator values", where,
                          "humanoid muscle curves and Animator parameter curves don't convert");
        }
        for (const Node& item : solid("m_PPtrCurves")) {
            problems_.add("animated " + class_name(item.integer_at("classID", 0)) + "." +
                              item.string_at("attribute"),
                          where, "no SlashEngine field it maps to");
        }

        if (time.stop <= time.start) {
            time.stop = last_key > time.start ? last_key : time.start + kSampleStep;
        }
        out.animation["length"] = tidy(time.length());
        for (const auto& make : pending) {
            make();
        }
        json::Array converted;
        for (auto& [key, track] : tracks) {
            converted.push_back(std::move(track));
        }
        add_events(
            list("m_Events"),
            [&](double at) -> std::optional<double> {
                if (at < time.start || at > time.stop) {
                    return std::nullopt;
                }
                return time.at(at);
            },
            converted, out);
        out.animation["tracks"] = std::move(converted);
    }

    // Events call a method of the object's scripts in Unity; here, gameplay code reads the event
    // the track emits. when gives an event's time in the animation, if it is in it.
    void add_events(const std::vector<Node>& items,
                    const std::function<std::optional<double>(double)>& when,
                    json::Array& converted, ClipOut& out) {
        std::map<std::string, json::Array> events;
        for (const Node& event : items) {
            const std::string function = event.string_at("functionName");
            const std::optional<double> at = when(event.number_at("time"));
            if (function.empty() || !at) {
                continue;
            }
            json::Value key;
            key["time"] = tidy(*at);
            json::Value data = json::Object{};
            if (const double number = event.number_at("floatParameter"); number != 0.0) {
                data["float"] = number;
            }
            if (const std::int64_t integer = event.integer_at("intParameter"); integer != 0) {
                data["int"] = integer;
            }
            if (const std::string text = event.string_at("data"); !text.empty()) {
                data["string"] = text;
            }
            if (const auto ref = ref_at(event, "objectReferenceParameter"); ref && !ref->null()) {
                const Asset* referenced = asset_of(ref->guid);
                data["object"] = referenced != nullptr ? json::Value(destination(referenced->path))
                                                       : json::Value("unity:" + ref->guid);
            }
            if (!data.as_object().empty()) {
                key["data"] = std::move(data);
            }
            json::Array& keys = events[function];
            if (!keys.empty() &&
                keys.back().find("time")->as_double() >= key.find("time")->as_double()) {
                continue;
            }
            keys.push_back(std::move(key));
            out.events.emplace_back(function, *at);
        }
        for (auto& [function, keys] : events) {
            json::Value track;
            track["event"] = function;
            track["keys"] = std::move(keys);
            converted.push_back(std::move(track));
        }
    }

    // --- Humanoid animation ---

    // A model's Avatar, as its import settings describe it: for a humanoid model, its node for
    // each human bone it maps ("Hips", "LeftUpperLeg"), and those bones' rotations in their
    // parents in the T-pose the Avatar keeps, mirrored back into the model's axes; for a generic
    // one, the node its root motion comes from, if it names one.
    struct Avatar {
        std::map<std::string, int> bones; // the model's node, by human name
        std::map<int, Quaternion> pose;   // a human bone's T-pose rotation, by node
        int motion_node = -1;
        [[nodiscard]] bool humanoid() const { return bones.contains("Hips"); }
    };

    // The node of a model a Unity name names: as the model names it, or with the characters
    // Godot's importer changes made "_".
    static int node_named(const scene3d::Model& model, std::string name) {
        if (const int found = scene3d::find_node(model, name); found >= 0) {
            return found;
        }
        for (char& c : name) {
            if (std::string_view(".:@/\"%").find(c) != std::string_view::npos) {
                c = '_';
            }
        }
        return scene3d::find_node(model, name);
    }

    const Avatar* avatar_of(const Asset& asset, const std::string& where) {
        auto [it, inserted] = avatars_.try_emplace(asset.path);
        std::optional<Avatar>& cached = it->second;
        if (!inserted) {
            return cached ? &*cached : nullptr;
        }
        const scene3d::Model* model = model_file(asset, where);
        if (model == nullptr) {
            return nullptr;
        }
        Avatar avatar;
        // An Avatar copied from another model's describes the same bones by the same names.
        constexpr int kHuman = 3;
        constexpr int kCopied = 2;
        constexpr int kMostCopies = 4;
        const Asset* described = &asset;
        for (int copies = 0; copies < kMostCopies &&
                             described->meta.integer_at("ModelImporter.avatarSetup", 1) == kCopied;
             ++copies) {
            const auto source = ref_at(described->meta, "ModelImporter.sourceAvatar");
            const Asset* next = source && !source->null() ? asset_of(source->guid) : nullptr;
            if (next == nullptr) {
                break;
            }
            described = next;
        }
        if (asset.meta.integer_at("ModelImporter.animationType", 2) == kHuman) {
            if (const Node* human = described->meta.path("ModelImporter.humanDescription.human")) {
                for (const Node& bone : human->items) {
                    if (const int node = node_named(*model, bone.string_at("boneName"));
                        node >= 0) {
                        avatar.bones[bone.string_at("humanName")] = node;
                    }
                }
            }
            std::set<int> human_nodes;
            for (const auto& [human, node] : avatar.bones) {
                human_nodes.insert(node);
            }
            if (const Node* skeleton =
                    described->meta.path("ModelImporter.humanDescription.skeleton")) {
                for (const Node& bone : skeleton->items) {
                    const int node = node_named(*model, bone.string_at("name"));
                    const Node* rotation = bone.get("rotation");
                    if (node < 0 || rotation == nullptr || !human_nodes.contains(node)) {
                        continue;
                    }
                    avatar.pose[node] =
                        normalized({rotation->number_at("x", 0.0), -rotation->number_at("y", 0.0),
                                    -rotation->number_at("z", 0.0), rotation->number_at("w", 1.0)});
                }
            }
        }
        std::string motion = asset.meta.string_at("ModelImporter.motionNodeName");
        if (const auto slash = motion.find_last_of("/\\"); slash != std::string::npos) {
            motion = motion.substr(slash + 1);
        }
        if (!motion.empty() && motion.front() != '<') {
            avatar.motion_node = node_named(*model, motion);
        }
        return &cached.emplace(std::move(avatar));
    }

    static Quaternion normalized(const Quaternion& q) {
        const double length =
            std::sqrt((q[0] * q[0]) + (q[1] * q[1]) + (q[2] * q[2]) + (q[3] * q[3]));
        if (length <= 0.0) {
            return {0.0, 0.0, 0.0, 1.0};
        }
        return {q[0] / length, q[1] / length, q[2] / length, q[3] / length};
    }

    static Quaternion inverse(const Quaternion& q) { return {-q[0], -q[1], -q[2], q[3]}; }

    // v turned by q.
    static std::array<double, 3> rotated(const Quaternion& q, const std::array<double, 3>& v) {
        const Quaternion turned = multiply(multiply(q, {v[0], v[1], v[2], 0.0}), inverse(q));
        return {turned[0], turned[1], turned[2]};
    }

    // The angle between two rotations, in radians, from the turn between them, which keeps small
    // angles exact as acos wouldn't.
    static double angle_between(const Quaternion& a, const Quaternion& b) {
        const Quaternion turn = multiply(inverse(a), b);
        return 2.0 * std::atan2(std::hypot(turn[0], turn[1], turn[2]), std::abs(turn[3]));
    }

    // The rotation weight of the way from a to b, the shorter way, as Godot's slerp.
    static Quaternion slerp(const Quaternion& a, Quaternion b, double weight) {
        double dot = (a[0] * b[0]) + (a[1] * b[1]) + (a[2] * b[2]) + (a[3] * b[3]);
        if (dot < 0.0) {
            dot = -dot;
            for (double& part : b) {
                part = -part;
            }
        }
        double from = 1.0 - weight;
        double to = weight;
        constexpr double kNearlyParallel = 0.9999;
        if (dot < kNearlyParallel) {
            const double omega = std::acos(dot);
            const double sine = std::sin(omega);
            from = std::sin(from * omega) / sine;
            to = std::sin(weight * omega) / sine;
        }
        return normalized({(from * a[0]) + (to * b[0]), (from * a[1]) + (to * b[1]),
                           (from * a[2]) + (to * b[2]), (from * a[3]) + (to * b[3])});
    }

    // A model's nodes posed: each one's rotation, position and scale in its parent, and once
    // placed, in the model's space.
    struct Pose {
        std::vector<Quaternion> rotations;
        std::vector<std::array<double, 3>> positions;
        std::vector<std::array<double, 3>> scales;
    };

    static Pose rest_pose(const scene3d::Model& model) {
        Pose pose;
        for (const scene3d::ModelNode& node : model.nodes) {
            pose.rotations.push_back(
                {node.rotation.x, node.rotation.y, node.rotation.z, node.rotation.w});
            pose.positions.push_back({node.position.x, node.position.y, node.position.z});
            pose.scales.push_back({node.scale.x, node.scale.y, node.scale.z});
        }
        return pose;
    }

    // Each node's parent before it.
    static std::vector<std::size_t> parents_first(const scene3d::Model& model) {
        std::vector<std::size_t> order;
        std::vector<bool> placed(model.nodes.size(), false);
        while (order.size() < model.nodes.size()) {
            const std::size_t before = order.size();
            for (std::size_t i = 0; i < model.nodes.size(); ++i) {
                const int parent = model.nodes[i].parent;
                if (!placed[i] && (parent < 0 || placed[static_cast<std::size_t>(parent)])) {
                    placed[i] = true;
                    order.push_back(i);
                }
            }
            if (order.size() == before) {
                break; // a cycle, which no loaded model has
            }
        }
        return order;
    }

    // A pose in the model's space. Where turns names a node's rotation in the model's space, the
    // node takes it, and its rotation in its parent is set to match.
    static Pose placed(const scene3d::Model& model, const std::vector<std::size_t>& order,
                       Pose& local, const std::map<std::size_t, Quaternion>& turns = {}) {
        Pose out = local;
        for (const std::size_t i : order) {
            const int parent = model.nodes[i].parent;
            if (parent < 0) {
                if (const auto turn = turns.find(i); turn != turns.end()) {
                    out.rotations[i] = turn->second;
                    local.rotations[i] = turn->second;
                }
                continue;
            }
            const auto p = static_cast<std::size_t>(parent);
            const std::array<double, 3>& scale = out.scales[p];
            const std::array<double, 3>& at = local.positions[i];
            const std::array<double, 3> offset =
                rotated(out.rotations[p], {at[0] * scale[0], at[1] * scale[1], at[2] * scale[2]});
            out.positions[i] = {out.positions[p][0] + offset[0], out.positions[p][1] + offset[1],
                                out.positions[p][2] + offset[2]};
            out.scales[i] = {scale[0] * local.scales[i][0], scale[1] * local.scales[i][1],
                             scale[2] * local.scales[i][2]};
            if (const auto turn = turns.find(i); turn != turns.end()) {
                out.rotations[i] = turn->second;
                local.rotations[i] = multiply(inverse(out.rotations[p]), turn->second);
            } else {
                out.rotations[i] = multiply(out.rotations[p], local.rotations[i]);
            }
        }
        return out;
    }

    // The keys worth keeping of values at times: the first and last, and between them only those
    // that interpolating from the last kept key to the next doesn't reach within a tolerance,
    // with a key at least every 64 frames.
    static std::vector<std::size_t>
    kept_keys(std::size_t count,
              const std::function<double(std::size_t, std::size_t, std::size_t)>& error) {
        constexpr double kTolerance = 1e-4;
        constexpr std::size_t kLongest = 64;
        std::vector<std::size_t> kept{0};
        std::size_t from = 0;
        for (std::size_t to = 2; to < count; ++to) {
            bool fits = to - from <= kLongest;
            for (std::size_t middle = from + 1; fits && middle < to; ++middle) {
                fits = error(from, to, middle) <= kTolerance;
            }
            if (!fits) {
                kept.push_back(to - 1);
                from = to - 1;
            }
        }
        if (count > 1) {
            kept.push_back(count - 1);
        }
        return kept;
    }

    // A humanoid take of one model retargeted onto another, as Unity's Avatars let any humanoid
    // model play it: each of the target's human bones turns from its T-pose as the source's turns
    // from its own, in the models' space, and the hips move as far for the target's hips height.
    // Its node tracks, keyed every frame from start to stop and thinned where keys add nothing;
    // human bones the source lacks hold their T-pose.
    json::Array retarget(const scene3d::Model& source, const Avatar& from,
                         const scene3d::ModelAnimation& take, double start, double stop,
                         const scene3d::Model& target, const Avatar& to) {
        const std::vector<std::size_t> source_order = parents_first(source);
        const std::vector<std::size_t> target_order = parents_first(target);
        const Pose source_rest = rest_pose(source);
        Pose source_t_pose = source_rest;
        for (const auto& [node, rotation] : from.pose) {
            source_t_pose.rotations[static_cast<std::size_t>(node)] = rotation;
        }
        const Pose source_zero = placed(source, source_order, source_t_pose);
        const Pose target_rest = rest_pose(target);
        Pose target_t_pose = target_rest;
        for (const auto& [node, rotation] : to.pose) {
            target_t_pose.rotations[static_cast<std::size_t>(node)] = rotation;
        }
        const Pose target_zero = placed(target, target_order, target_t_pose);

        std::vector<std::pair<std::size_t, std::size_t>> pairs; // target node, source node
        std::set<std::size_t> held;                             // human bones the source lacks
        for (const auto& [human, node] : to.bones) {
            const auto found = from.bones.find(human);
            if (found != from.bones.end()) {
                pairs.emplace_back(node, found->second);
            } else {
                held.insert(static_cast<std::size_t>(node));
            }
        }
        const auto hips_target = static_cast<std::size_t>(to.bones.at("Hips"));
        const auto hips_source = static_cast<std::size_t>(from.bones.at("Hips"));
        constexpr double kLowest = 1e-6;
        const double source_height = source_zero.positions[hips_source][1];
        const double target_height = target_zero.positions[hips_target][1];
        const double ratio = source_height > kLowest && target_height > kLowest
                                 ? target_height / source_height
                                 : 1.0;

        constexpr double kRate = 30.0;
        const double rate = take.frame_rate > 0.0 ? take.frame_rate : kRate;
        const auto frames =
            static_cast<std::size_t>(std::max(1L, std::lround((stop - start) * rate)));
        std::vector<double> times;
        std::map<std::size_t, std::vector<Quaternion>> turns; // by target node
        std::vector<std::array<double, 3>> hips;
        for (std::size_t frame = 0; frame <= frames; ++frame) {
            const double t =
                start + ((stop - start) * static_cast<double>(frame) / static_cast<double>(frames));
            Pose source_local = source_rest;
            for (const scene3d::NodeTrack& track : take.tracks) {
                const std::array<float, 4> value =
                    scene3d::sample_track(track, t, take.length, false);
                switch (track.property) {
                case scene3d::NodeProperty::Position:
                    source_local.positions[track.node] = {value[0], value[1], value[2]};
                    break;
                case scene3d::NodeProperty::Rotation:
                    source_local.rotations[track.node] =
                        normalized({value[0], value[1], value[2], value[3]});
                    break;
                case scene3d::NodeProperty::Scale:
                    source_local.scales[track.node] = {value[0], value[1], value[2]};
                    break;
                }
            }
            const Pose source_now = placed(source, source_order, source_local);
            std::map<std::size_t, Quaternion> globals;
            for (const auto& [target_node, source_node] : pairs) {
                const Quaternion change = multiply(source_now.rotations[source_node],
                                                   inverse(source_zero.rotations[source_node]));
                globals[target_node] =
                    normalized(multiply(change, target_zero.rotations[target_node]));
            }
            Pose target_local = target_rest;
            for (const std::size_t node : held) {
                target_local.rotations[node] = target_t_pose.rotations[node];
            }
            const Pose target_now = placed(target, target_order, target_local, globals);
            times.push_back(t - start);
            for (const auto& [human, node] : to.bones) {
                turns[static_cast<std::size_t>(node)].push_back(
                    target_local.rotations[static_cast<std::size_t>(node)]);
            }
            // The hips move as far as the source's, for the target's height, in its parent.
            std::array<double, 3> hips_at = target_zero.positions[hips_target];
            for (std::size_t i = 0; i < 3; ++i) {
                hips_at.at(i) += (source_now.positions[hips_source].at(i) -
                                  source_zero.positions[hips_source].at(i)) *
                                 ratio;
            }
            const int parent = target.nodes[hips_target].parent;
            if (parent >= 0) {
                const auto p = static_cast<std::size_t>(parent);
                std::array<double, 3> offset = rotated(inverse(target_now.rotations[p]),
                                                       {hips_at[0] - target_now.positions[p][0],
                                                        hips_at[1] - target_now.positions[p][1],
                                                        hips_at[2] - target_now.positions[p][2]});
                for (std::size_t i = 0; i < 3; ++i) {
                    const double scale = target_now.scales[p].at(i);
                    offset.at(i) = std::abs(scale) > kLowest ? offset.at(i) / scale : offset.at(i);
                }
                hips_at = offset;
            }
            hips.push_back(hips_at);
        }

        json::Array tracks;
        constexpr double kStill = 1e-5;
        const auto key_time = [&](std::size_t i) { return tidy(times[i]); };
        for (const auto& [node, values] : turns) {
            const Quaternion& rest = target_rest.rotations[node];
            if (std::ranges::all_of(
                    values, [&](const Quaternion& q) { return angle_between(q, rest) < kStill; })) {
                continue;
            }
            json::Array keys;
            for (const std::size_t i :
                 kept_keys(values.size(), [&](std::size_t a, std::size_t b, std::size_t m) {
                     const double weight = (times[m] - times[a]) / (times[b] - times[a]);
                     return angle_between(slerp(values[a], values[b], weight), values[m]);
                 })) {
                const std::array<double, 3> angles = quaternion_euler(values[i]);
                json::Value key;
                key["time"] = key_time(i);
                key["value"] = json::Array{angles[0], angles[1], angles[2]};
                keys.push_back(std::move(key));
            }
            json::Value track;
            track["node"] = target.nodes[node].name;
            track["property"] = "rotation";
            track["keys"] = std::move(keys);
            tracks.push_back(std::move(track));
        }
        const std::array<double, 3>& rest = target_rest.positions[hips_target];
        if (std::ranges::any_of(hips, [&](const std::array<double, 3>& at) {
                return std::abs(at[0] - rest[0]) + std::abs(at[1] - rest[1]) +
                           std::abs(at[2] - rest[2]) >=
                       kStill;
            })) {
            json::Array keys;
            for (const std::size_t i :
                 kept_keys(hips.size(), [&](std::size_t a, std::size_t b, std::size_t m) {
                     const double weight = (times[m] - times[a]) / (times[b] - times[a]);
                     double error = 0.0;
                     for (std::size_t axis = 0; axis < 3; ++axis) {
                         const double between =
                             hips[a].at(axis) + ((hips[b].at(axis) - hips[a].at(axis)) * weight);
                         error = std::max(error, std::abs(between - hips[m].at(axis)));
                     }
                     return error;
                 })) {
                json::Value key;
                key["time"] = key_time(i);
                key["value"] = json::Array{tidy(hips[i][0]), tidy(hips[i][1]), tidy(hips[i][2])};
                keys.push_back(std::move(key));
            }
            json::Value track;
            track["node"] = target.nodes[hips_target].name;
            track["property"] = "position";
            track["keys"] = std::move(keys);
            tracks.insert(tracks.begin(), std::move(track));
        }
        return tracks;
    }

    // Whether a humanoid take of one model plays on another as it is: the same human bones by
    // the same names, in the same T-pose, at rest the same way, and the hips as high.
    static bool same_rig(const scene3d::Model& source, const Avatar& from,
                         const scene3d::Model& target, const Avatar& to) {
        constexpr double kClose = 1e-4;
        if (from.bones.size() != to.bones.size()) {
            return false;
        }
        return std::ranges::all_of(to.bones, [&](const auto& bone) {
            const int node = bone.second;
            const auto found = from.bones.find(bone.first);
            if (found == from.bones.end()) {
                return false;
            }
            const scene3d::ModelNode& a = source.nodes[static_cast<std::size_t>(found->second)];
            const scene3d::ModelNode& b = target.nodes[static_cast<std::size_t>(node)];
            const Quaternion rest_a{a.rotation.x, a.rotation.y, a.rotation.z, a.rotation.w};
            const Quaternion rest_b{b.rotation.x, b.rotation.y, b.rotation.z, b.rotation.w};
            const auto pose_a = from.pose.find(found->second);
            const auto pose_b = to.pose.find(node);
            const Quaternion t_a = pose_a != from.pose.end() ? pose_a->second : rest_a;
            const Quaternion t_b = pose_b != to.pose.end() ? pose_b->second : rest_b;
            return a.name == b.name && angle_between(rest_a, rest_b) <= kClose &&
                   angle_between(t_a, t_b) <= kClose &&
                   std::abs(a.position.x - b.position.x) + std::abs(a.position.y - b.position.y) +
                           std::abs(a.position.z - b.position.z) <=
                       kClose;
        });
    }

    // A clip of a model, as its import settings make it of one of the model's takes. A clip's ID
    // names it in the settings' tables, else it is Unity's ID for the clip of each take's name.
    struct ModelClip {
        std::string name;
        std::string take;
        bool loop = false;
        std::optional<std::array<double, 2>> frames; // the clip's first and last frame
        const Node* events = nullptr;                // at times from 0 to 1 through the clip
    };

    static std::optional<ModelClip>
    model_clip_settings(const Asset& asset, const scene3d::Model& model, std::int64_t file_id) {
        std::string named;
        if (const Node* table = asset.meta.path("ModelImporter.internalIDToNameTable")) {
            for (const Node& entry : table->items) {
                const Node* first = entry.get("first");
                if (const Node* id = first != nullptr ? first->get("74") : nullptr;
                    id != nullptr && id->integer() == file_id) {
                    named = entry.string_at("second");
                }
            }
        }
        if (const Node* table = asset.meta.path("ModelImporter.fileIDToRecycleName")) {
            for (const auto& [id, name] : table->entries) {
                if (parse_int(id) == file_id) {
                    named = name.string();
                }
            }
        }
        const Node* clips = asset.meta.path("ModelImporter.animations.clipAnimations");
        const std::vector<Node> none;
        const std::vector<Node>& settings = clips != nullptr ? clips->items : none;
        for (const Node& clip : settings) {
            if (named.empty() && clip.integer_at("internalID", 0) == file_id) {
                named = clip.string_at("name");
            }
        }
        if (named.empty()) {
            std::map<std::string, int, std::less<>> seen;
            for (const scene3d::ModelAnimation& animation : model.animations) {
                if (import_id("AnimationClip", animation.name, seen[animation.name]++) == file_id) {
                    named = animation.name;
                }
            }
        }
        if (named.empty()) {
            return std::nullopt;
        }
        ModelClip out{
            .name = named, .take = named, .loop = false, .frames = std::nullopt, .events = nullptr};
        for (const Node& clip : settings) {
            if (clip.string_at("name") != named) {
                continue;
            }
            if (std::string take = clip.string_at("takeName"); !take.empty()) {
                out.take = std::move(take);
            }
            out.loop = clip.flag_at("loopTime");
            if (clip.get("firstFrame") != nullptr && clip.get("lastFrame") != nullptr) {
                out.frames = std::array<double, 2>{clip.number_at("firstFrame"),
                                                   clip.number_at("lastFrame")};
            }
            out.events = clip.get("events");
        }
        return out;
    }

    // A clip of a model: its take's animation, played "from" the model, with the loop and events
    // its import settings give it, cut short where the clip ends before the take does.
    void model_clip(const ObjectRef& ref, const Asset& asset, const std::string& where,
                    ClipOut& out) {
        if (extension_of(asset.path) == ".blend") {
            problems_.add("animation clip of a .blend file", where,
                          asset.path + ": its takes are read through Blender as the game runs; "
                                       "export the model as FBX or glTF");
            return;
        }
        const scene3d::Model* model = model_file(asset, where);
        if (model == nullptr) {
            return;
        }
        const std::optional<ModelClip> found = model_clip_settings(asset, *model, ref.file_id);
        if (!found) {
            problems_.add("animation clip", where,
                          asset.path + ": no clip of the model has ID " +
                              std::to_string(ref.file_id));
            return;
        }
        const ModelClip& settings = *found;
        // The model names its takes as Godot names animations, with some characters made "_".
        const auto plain = [](std::string name) {
            for (char& c : name) {
                if (std::string_view(".:@/\"%,[").find(c) != std::string_view::npos) {
                    c = '_';
                }
            }
            return name;
        };
        const auto take =
            std::ranges::find_if(model->animations, [&](const scene3d::ModelAnimation& animation) {
                return animation.name == settings.take || animation.name == plain(settings.take);
            });
        if (take == model->animations.end()) {
            problems_.add("animation clip", where,
                          asset.path + ": the model has no take \"" + settings.take + "\"");
            return;
        }
        out.name = settings.name;
        out.animation["loop"] = settings.loop ? "linear" : "none";
        constexpr double kClose = 0.001;
        double start = 0.0;
        double stop = take->length;
        double first_frame = 0.0;
        if (settings.frames && take->frame_rate > 0.0) {
            first_frame = (*settings.frames)[0];
            start = std::clamp(first_frame / take->frame_rate, 0.0, take->length);
            stop = std::clamp((*settings.frames)[1] / take->frame_rate, start, take->length);
            if (stop - start < kClose) {
                stop = take->length;
            }
        }
        // A humanoid take of another model plays on this one retargeted through both Avatars;
        // any other plays from its model, moving the nodes of the same names.
        const Asset* target_asset = clip_target_.empty() ? nullptr : asset_of(clip_target_);
        const scene3d::Model* target = target_asset != nullptr && target_asset->path != asset.path
                                           ? model_file(*target_asset, where)
                                           : nullptr;
        const Avatar* from = target != nullptr ? avatar_of(asset, where) : nullptr;
        const Avatar* to = target != nullptr ? avatar_of(*target_asset, where) : nullptr;
        json::Array tracks;
        double length = take->length;
        if (from != nullptr && to != nullptr && from->humanoid() && to->humanoid() &&
            !same_rig(*model, *from, *target, *to)) {
            tracks = retarget(*model, *from, *take, start, stop, *target, *to);
            length = stop - start;
            out.retargeted = true;
        } else {
            if (target != nullptr) {
                const std::vector<int> map = scene3d::node_map(*target, *model);
                if (!take->tracks.empty() &&
                    std::ranges::none_of(take->tracks, [&](const scene3d::NodeTrack& track) {
                        return map[track.node] >= 0;
                    })) {
                    problems_.add("animation clip of another model", where,
                                  settings.name + " moves nodes of " + asset.path + " that " +
                                      target_asset->path + " hasn't");
                }
            }
            out.animation["from"]["model"] = destination(asset.path);
            out.animation["from"]["animation"] = take->name;
            if (start > kClose) {
                std::ostringstream frame;
                frame << first_frame;
                problems_.add("animation clip of part of a model's take", where,
                              settings.name + " plays " + take->name +
                                  " from its start; Unity starts it at frame " + frame.str());
            } else if (stop < length - kClose) {
                length = stop;
            }
        }
        out.animation["length"] = tidy(length);
        const std::vector<Node> none;
        add_events(
            settings.events != nullptr ? settings.events->items : none,
            [&](double at) -> std::optional<double> { return std::clamp(at, 0.0, 1.0) * length; },
            tracks, out);
        if (!tracks.empty()) {
            out.animation["tracks"] = std::move(tracks);
        }
        out.ok = true;
    }

    // A clip as one state plays it: faster or slower by its speed.
    static json::Value at_speed(const json::Value& animation, double speed) {
        json::Value out = animation;
        if (std::abs(speed - 1.0) < 1e-9) {
            return out;
        }
        out["length"] = tidy(animation.find("length")->as_double() / speed);
        if (out.find("tracks") == nullptr) {
            return out;
        }
        for (json::Value& track : out["tracks"].as_array()) {
            for (json::Value& key : track["keys"].as_array()) {
                key["time"] = tidy(key.find("time")->as_double() / speed);
            }
        }
        return out;
    }

    // An Animator controller as converted: an .anim.json file with an animation per state of its
    // base layer, the state it starts in, and the target names its tracks use.
    struct LibraryOut {
        std::string path; // "" if it didn't convert
        std::string first;
        std::set<std::string> targets;
        bool offsets = false;
        std::string task_text;          // its state machine, for a storyboard task; "" with none
        std::vector<std::string> users; // "<entity> in <scene>"
        std::vector<std::string> scenes;
        std::string tree;        // the base layer's state machine as a .animtree.json file, or ""
        json::Object parameters; // the tree's parameters that start other than at their defaults
        ClipSpace space = ClipSpace::Flat; // what its clips converted for
        std::string model;                 // for a model's instance, the model's GUID
        bool retargeted = false;           // whether some clip is retargeted to that model
        std::string motion_tree; // a tree playing its first animation, for root motion, or ""
    };

    // The objects of a controller file, by file ID.
    struct ControllerFile {
        std::string path;
        std::map<std::int64_t, const Object*> by_id;
        std::vector<Object> objects;
    };

    // A controller's base layer: its states through its sub-state machines and what the
    // library made of them.
    struct LayerParts {
        const ControllerFile* file = nullptr;
        const Object* controller = nullptr;
        const Object* root = nullptr; // the layer's state machine
        std::vector<const Object*> states;
        std::vector<const Object*> machines;
        std::map<std::int64_t, std::string> names;               // animations, by state's ID
        std::map<std::int64_t, std::string> state_names;         // by state's ID
        std::map<std::int64_t, const Object*> machine_of;        // by state's ID
        std::map<std::int64_t, const Object*> parent_of;         // by state machine's ID
        std::map<std::int64_t, std::array<double, 2>> positions; // by state's ID
        std::map<std::string, std::pair<double, bool>> clips;    // length and loop, by animation
        std::map<std::int64_t, json::Value> nodes;               // blend spaces, by state's ID
        // The parameters each blend space reads, by state's ID: its parameter path under the
        // state ("" or "0/"), and its X and, in 2D, Y parameter.
        struct Blend {
            std::string path;
            std::string x;
            std::string y;
        };
        std::map<std::int64_t, std::vector<Blend>> blends;
    };

    std::optional<ControllerFile> read_controller(const std::string& guid,
                                                  const std::string& where) {
        const Asset* asset = asset_of(guid);
        if (asset == nullptr) {
            problems_.add("Animator controller", where,
                          "a controller from outside the project's Assets folder");
            return std::nullopt;
        }
        auto text = platform::read_file(source_ + asset->path);
        auto objects =
            text ? unity::parse_file(text.value()) : Result<std::vector<Object>>(text.error());
        if (!objects) {
            problems_.add("unreadable file", asset->path, objects.error().message);
            return std::nullopt;
        }
        ControllerFile file;
        file.path = asset->path;
        file.objects = std::move(objects).value();
        for (const Object& object : file.objects) {
            file.by_id[object.file_id] = &object;
        }
        return file;
    }

    static std::string_view space_name(ClipSpace space) {
        switch (space) {
        case ClipSpace::Flat:
            return "2D objects";
        case ClipSpace::Objects:
            return "3D objects";
        case ClipSpace::Model:
            return "a model's instance";
        }
        return "";
    }

    LibraryOut& library(const ObjectRef& ref, const std::string& where, ClipSpace space) {
        auto [it, inserted] = libraries_.try_emplace(ref.guid);
        LibraryOut& out = it->second;
        if (!inserted) {
            if (out.space != space && !out.path.empty()) {
                problems_.add("Animator controller played by different kinds of objects", where,
                              "its clips converted for " + std::string(space_name(out.space)) +
                                  ", so its tracks may not move this one as in Unity");
            } else if (out.retargeted && out.model != clip_target_) {
                const Asset* model = asset_of(out.model);
                problems_.add("Animator controller played by different humanoid models", where,
                              "its humanoid clips are retargeted to " +
                                  (model != nullptr ? model->path : out.model) +
                                  ", so they may not fit this model");
            }
            return out;
        }
        out.space = space;
        out.model = space == ClipSpace::Model ? clip_target_ : std::string();
        auto file = read_controller(ref.guid, where);
        if (!file) {
            return out;
        }
        // An override controller is its controller's states with some clips swapped.
        std::map<std::string, ObjectRef> overrides;
        std::optional<ControllerFile> base;
        for (const Object& object : file->objects) {
            if (object.class_id != kAnimatorOverrideController) {
                continue;
            }
            const Node* clips = object.body.get("m_Clips");
            for (const Node& item : clips != nullptr ? clips->items : std::vector<Node>{}) {
                const auto original = ref_at(item, "m_OriginalClip");
                const auto replacement = ref_at(item, "m_OverrideClip");
                if (original && replacement && !replacement->null()) {
                    overrides[original->guid + ":" + std::to_string(original->file_id)] =
                        *replacement;
                }
            }
            const auto controller = ref_at(object.body, "m_Controller");
            if (!controller || controller->null()) {
                problems_.add("Animator override controller", file->path,
                              "it overrides no controller");
                return out;
            }
            base = read_controller(controller->guid, file->path);
            if (!base) {
                return out;
            }
        }
        const ControllerFile& states_file = base ? *base : *file;
        const Object* controller = nullptr;
        for (const Object& object : states_file.objects) {
            if (object.class_id == kAnimatorController) {
                controller = &object;
            }
        }
        if (controller == nullptr) {
            problems_.add("Animator controller", file->path, "no AnimatorController in the file");
            return out;
        }
        const Node* layers = controller->body.get("m_AnimatorLayers");
        if (layers == nullptr || layers->items.empty()) {
            problems_.add("Animator controller", file->path, "no layers");
            return out;
        }
        if (layers->items.size() > 1) {
            problems_.add("Animator layers other than the base layer", file->path,
                          "the base layer's states convert; the layers over it don't");
        }
        const auto object_at = [&](const Node& node, std::string_view key) -> const Object* {
            const auto local = ref_at(node, key);
            if (!local || local->null()) {
                return nullptr;
            }
            const auto found = states_file.by_id.find(local->file_id);
            return found != states_file.by_id.end() ? found->second : nullptr;
        };
        const Object* machine = object_at(layers->items.front(), "m_StateMachine");
        if (machine == nullptr) {
            problems_.add("Animator controller", file->path, "its base layer has no state machine");
            return out;
        }

        // Every state of the base layer, through its sub-state machines.
        LayerParts layer;
        layer.file = &states_file;
        layer.controller = controller;
        layer.root = machine;
        std::vector<const Object*>& states = layer.states;
        std::vector<const Object*>& machines = layer.machines;
        std::function<void(const Object&, int)> collect = [&](const Object& at, int depth) {
            machines.push_back(&at);
            if (const Node* children = at.body.get("m_ChildStates")) {
                for (const Node& child : children->items) {
                    if (const Object* state = object_at(child, "m_State")) {
                        states.push_back(state);
                        layer.machine_of[state->file_id] = &at;
                        if (child.get("m_Position") != nullptr) {
                            layer.positions[state->file_id] = {child.number_at("m_Position.x"),
                                                               child.number_at("m_Position.y")};
                        }
                    }
                }
            }
            if (const Node* children = at.body.get("m_ChildStateMachines")) {
                for (const Node& child : children->items) {
                    if (const Object* sub = object_at(child, "m_StateMachine");
                        sub != nullptr && depth < kMaxInstanceDepth) {
                        layer.parent_of[sub->file_id] = &at;
                        collect(*sub, depth + 1);
                    }
                }
            }
        };
        collect(*machine, 0);

        // A controller and its override controller may share a name: the second gets a number.
        const std::string base_path = without_extension(file->path);
        std::string stem = base_path;
        for (int n = 2; !library_paths_.insert(stem).second; ++n) {
            stem = base_path + "_" + std::to_string(n);
        }
        out.path = destination(stem + std::string(animation::kAnimationSuffix));
        json::Object animations;
        std::map<std::int64_t, std::string> names; // by state's file ID
        std::set<std::string> taken;
        std::vector<std::string> events;
        const std::string state_where = file->path + ": state ";
        // Adds a clip's animation at a speed and returns its name, "" when the clip doesn't
        // convert: a state's under the state's name, a blend tree's under its clip's, or the
        // state's and its clip's when another animation has that, unless the same animation is
        // there already.
        const AddAnimation add_animation = [&](ObjectRef motion, double speed,
                                               const std::string& name, const std::string& prefix,
                                               const std::string& here) {
            if (const auto swapped =
                    overrides.find(motion.guid + ":" + std::to_string(motion.file_id));
                swapped != overrides.end()) {
                motion = swapped->second;
            }
            const ClipOut& converted = clip(motion, here, space);
            if (!converted.ok) {
                return std::string();
            }
            out.retargeted = out.retargeted || converted.retargeted;
            // A model's take plays at its own speed.
            const bool from_model = converted.animation.find("from") != nullptr;
            const double rate = from_model ? 1.0 : speed;
            if (from_model && std::abs(speed - 1.0) > 1e-9) {
                problems_.add("state speed on a model's clip", here,
                              "it plays at the speed of the model's take");
            }
            json::Value animation = at_speed(converted.animation, rate);
            std::string chosen = name;
            if (const Asset* asset = asset_of(motion.guid); asset != nullptr && chosen.empty()) {
                chosen = is_model_file(extension_of(asset->path)) ? converted.name
                                                                  : file_stem(asset->path);
            }
            if (!prefix.empty()) {
                for (const auto& [existing, value] : animations) {
                    if (value == animation) {
                        return existing;
                    }
                }
                if (taken.contains(chosen)) {
                    chosen.insert(0, prefix + " ");
                }
                chosen = take(taken, chosen);
            }
            out.offsets = out.offsets || converted.offsets;
            for (const auto& [function, at] : converted.events) {
                std::ostringstream seconds;
                seconds << tidy(at / rate);
                events.push_back(std::string(function)
                                     .append(" at ")
                                     .append(seconds.str())
                                     .append(" s in ")
                                     .append(chosen));
            }
            if (const json::Value* tracks = animation.find("tracks")) {
                for (const json::Value& track : tracks->as_array()) {
                    if (const json::Value* target = track.find("target")) {
                        out.targets.insert(target->as_string());
                    }
                }
            }
            animations.emplace(chosen, std::move(animation));
            return chosen;
        };
        // States playing clips first, under their own names, then blend trees, whose clips the
        // states may play already.
        std::map<std::int64_t, std::string> reserved;
        for (const Object* state : states) {
            reserved[state->file_id] = take(taken, state->body.string_at("m_Name"));
        }
        for (const bool blend_trees : {false, true}) {
            for (const Object* state : states) {
                const std::optional<ObjectRef> motion = ref_at(state->body, "m_Motion");
                if (!motion || motion->null() || motion->guid.empty() != blend_trees) {
                    continue;
                }
                const std::string& state_name = reserved.at(state->file_id);
                const std::string here = state_where + state_name;
                double speed = state->body.number_at("m_Speed", 1.0);
                if (speed <= 0.0) {
                    problems_.add("state speed at or below 0", here, "plays forward at speed 1");
                    speed = 1.0;
                }
                if (state->body.flag_at("m_SpeedParameterActive")) {
                    problems_.add("state speed parameter", here,
                                  "set the AnimationPlayer's speed_scale from gameplay code");
                }
                if (state->body.number_at("m_CycleOffset") != 0.0 ||
                    state->body.flag_at("m_CycleOffsetParameterActive")) {
                    problems_.add("state cycle offset", here, "plays from the start");
                }
                if (state->body.flag_at("m_Mirror") ||
                    state->body.flag_at("m_MirrorParameterActive")) {
                    problems_.add("mirrored state", here, "mirroring is for humanoid 3D animation");
                }
                if (!blend_trees) {
                    if (!add_animation(*motion, speed, state_name, "", here).empty()) {
                        names[state->file_id] = state_name;
                    }
                    continue;
                }
                // A blend tree is a blend space over its clips' animations.
                std::vector<LayerParts::Blend> blends;
                std::string first;
                const std::optional<json::Value> node =
                    blend_node(states_file, motion->file_id, speed, state_name, here, "", 0,
                               add_animation, animations, blends, first);
                if (!node) {
                    continue;
                }
                names[state->file_id] = first;
                if (node->find("type")->as_string() != "Animation") {
                    layer.nodes[state->file_id] = *node;
                    layer.blends[state->file_id] = std::move(blends);
                }
            }
        }
        if (const auto first = ref_at(machine->body, "m_DefaultState"); first) {
            if (const auto name = names.find(first->file_id);
                name != names.end() && animations.contains(name->second)) {
                out.first = name->second;
            }
        }
        for (const auto& [name, animation] : animations) {
            const json::Value* loop = animation.find("loop");
            layer.clips[name] = {animation.find("length")->as_double(),
                                 loop != nullptr && loop->as_string() != "none"};
        }
        json::Value doc;
        doc["format"] = 1;
        doc["animations"] = std::move(animations);
        keep_valid(doc, out.path, [this](const json::Value& value) {
            return animation::validate_animation_library(value, &components_);
        });
        add_file(out.path, json::write(doc) + "\n");
        ++animation_libraries_;
        layer.names = names;
        layer.state_names = reserved;
        std::vector<std::string> described;
        std::vector<std::string> positions;
        out.tree = layer_tree(layer, stem, file->path, described, positions, out.parameters);
        if (out.tree.empty()) {
            out.task_text =
                state_machine_text(states_file, controller->body, names, machines, events);
        } else {
            out.task_text = out.tree + " plays the state machine with an AnimationTree.";
            if (!described.empty()) {
                out.task_text += " Gameplay code sets its parameters, as Unity's SetFloat, "
                                 "SetInteger, SetBool and SetTrigger did: " +
                                 join(described, "; ") + ".";
            }
            if (!positions.empty()) {
                out.task_text += " Unity's blend trees read their parameters themselves; set "
                                 "each blend position with them: " +
                                 join(positions, "; ") + ".";
            }
            if (!events.empty()) {
                out.task_text += " Animation events: " + join(events, ", ") + ".";
            }
        }
        return out;
    }

    using AddAnimation = std::function<std::string(ObjectRef, double, const std::string&,
                                                   const std::string&, const std::string&)>;

    // A blend tree as a blend space over its motions, as Godot's BlendSpace1D and BlendSpace2D:
    // each an Animation node naming the animation add makes of its clip, or a blend space of its
    // own. Null when no motion converts, and the motion's node when only one does. first is the
    // first clip's animation; blends gets each blend space's parameters, under path.
    std::optional<json::Value> blend_node(const ControllerFile& file, std::int64_t id, double speed,
                                          const std::string& state, const std::string& here,
                                          const std::string& path, int depth,
                                          const AddAnimation& add, const json::Object& animations,
                                          std::vector<LayerParts::Blend>& blends,
                                          std::string& first) {
        const auto found = file.by_id.find(id);
        if (found == file.by_id.end() || found->second->class_id != kBlendTree ||
            depth > kMaxInstanceDepth) {
            problems_.add("blend tree", here, "a blend tree missing from the controller");
            return std::nullopt;
        }
        const Node& tree = found->second->body;
        const Node* children = tree.get("m_Childs");
        const std::vector<Node> none;
        const std::vector<Node>& items = children != nullptr ? children->items : none;
        const std::int64_t type = tree.integer_at("m_BlendType");
        struct Point {
            std::array<double, 2> position{};
            json::Value node;
            std::string path; // its blend spaces' parameters' path
        };
        std::vector<Point> points;
        const auto motion_node = [&](const Node& child,
                                     const std::string& child_path) -> std::optional<json::Value> {
            const auto motion = ref_at(child, "m_Motion");
            if (!motion || motion->null()) {
                return std::nullopt;
            }
            double scale = child.number_at("m_TimeScale", 1.0);
            if (scale <= 0.0) {
                problems_.add("blend tree motion time scale at or below 0", here,
                              "plays forward at speed 1");
                scale = 1.0;
            }
            if (child.number_at("m_CycleOffset") != 0.0) {
                problems_.add("blend tree motion cycle offset", here, "plays from the start");
            }
            if (child.flag_at("m_Mirror")) {
                problems_.add("mirrored state", here, "mirroring is for humanoid 3D animation");
            }
            if (motion->guid.empty()) {
                return blend_node(file, motion->file_id, speed * scale, state, here, child_path,
                                  depth + 1, add, animations, blends, first);
            }
            const std::string name = add(*motion, speed * scale, "", state, here);
            if (name.empty()) {
                return std::nullopt;
            }
            if (first.empty()) {
                first = name;
            }
            json::Value node;
            node["type"] = "Animation";
            node["animation"] = name;
            return node;
        };
        if (type == 4) {
            problems_.add("Animator direct blend tree", here,
                          "the state plays the tree's first motion");
            for (const Node& child : items) {
                if (auto node = motion_node(child, path)) {
                    return node;
                }
            }
            return std::nullopt;
        }
        if (type < 0 || type > 4) {
            problems_.add("blend tree", here, "a blend type Unity doesn't have");
            return std::nullopt;
        }
        const bool two = type != 0;
        for (const Node& child : items) {
            const std::string child_path = path + std::to_string(points.size()) + "/";
            if (auto node = motion_node(child, child_path)) {
                const std::array<double, 2> at =
                    two ? std::array<double, 2>{child.number_at("m_Position.x"),
                                                child.number_at("m_Position.y")}
                        : std::array<double, 2>{child.number_at("m_Threshold"), 0.0};
                points.push_back({.position = at, .node = std::move(*node), .path = child_path});
            }
        }
        if (points.empty()) {
            return std::nullopt;
        }
        if (points.size() == 1) {
            // The motion's node stands for the tree, its parameters a level up.
            for (LayerParts::Blend& blend : blends) {
                if (blend.path.starts_with(points.front().path)) {
                    blend.path = path + blend.path.substr(points.front().path.size());
                }
            }
            return std::move(points.front().node);
        }
        // In 2D, motions all in one row or column blend in 1D along it, and ones on a slant
        // can't make triangles, so the closest plays.
        std::string x = tree.string_at("m_BlendParameter");
        std::string y = two ? tree.string_at("m_BlendParameterY") : std::string();
        int axis = 0; // the position's part a 1D space reads
        bool discrete = false;
        if (two) {
            bool same_x = true;
            bool same_y = true;
            bool line = true;
            const std::array<double, 2>& a = points.front().position;
            for (const Point& point : points) {
                same_x = same_x && std::abs(point.position[0] - a[0]) < 1e-6;
                same_y = same_y && std::abs(point.position[1] - a[1]) < 1e-6;
            }
            for (std::size_t i = 1; i + 1 < points.size() && line; ++i) {
                for (std::size_t j = i + 1; j < points.size() && line; ++j) {
                    const std::array<double, 2>& b = points[i].position;
                    const std::array<double, 2>& c = points[j].position;
                    line = std::abs(((b[0] - a[0]) * (c[1] - a[1])) -
                                    ((b[1] - a[1]) * (c[0] - a[0]))) < 1e-6;
                }
            }
            if (same_y && !same_x) {
                y.clear();
            } else if (same_x && !same_y) {
                axis = 1;
                x = y;
                y.clear();
            } else if (line) {
                discrete = true;
                problems_.add("Animator 2D blend tree on a slanted line", here,
                              "its motions make no triangles to blend in; the closest one plays");
            } else {
                problems_.add("Animator 2D blend tree", here,
                              "the blend space blends the three motions around the position, in "
                              "triangles as Godot's; Unity weighs every motion by gradient bands");
            }
        }
        const bool plane = !y.empty() || discrete;
        json::Value out;
        out["type"] = plane ? "BlendSpace2D" : "BlendSpace1D";
        json::Array list;
        std::array<double, 2> low{-1.0, -1.0};
        std::array<double, 2> high{1.0, 1.0};
        for (Point& point : points) {
            json::Value entry;
            if (plane) {
                entry["position"] = pair(point.position[0], point.position[1]);
            } else {
                entry["position"] = tidy(point.position.at(static_cast<std::size_t>(axis)));
            }
            entry["node"] = std::move(point.node);
            list.push_back(std::move(entry));
            for (std::size_t i = 0; i < 2; ++i) {
                const double at = plane ? point.position.at(i)
                                        : point.position.at(static_cast<std::size_t>(axis));
                low.at(i) = std::min(low.at(i), at);
                high.at(i) = std::max(high.at(i), at);
            }
        }
        out["points"] = std::move(list);
        if (low[0] < -1.0 || (plane && low[1] < -1.0)) {
            out["min_space"] = plane ? pair(low[0], low[1]) : json::Value(tidy(low[0]));
        }
        if (high[0] > 1.0 || (plane && high[1] > 1.0)) {
            out["max_space"] = plane ? pair(high[0], high[1]) : json::Value(tidy(high[0]));
        }
        if (discrete) {
            out["blend_mode"] = "discrete";
        }
        // Unity plays every motion, in step; a blend space keeps them all playing with sync.
        out["sync"] = true;
        std::set<double> lengths;
        for (const json::Value& point : out.find("points")->as_array()) {
            const json::Value& node = *point.find("node");
            if (const json::Value* animation = node.find("animation")) {
                if (const auto it = animations.find(animation->as_string());
                    it != animations.end()) {
                    lengths.insert(it->second.find("length")->as_double());
                }
            }
        }
        if (lengths.size() > 1) {
            problems_.add("Animator blend tree motions of different lengths", here,
                          "Unity stretches them to one cycle as it blends; each plays at its own "
                          "length");
        }
        blends.push_back({.path = path, .x = x, .y = y});
        return out;
    }

    // A name for a parameter as the tree's variable: letters, digits and "_", not one of an
    // expression's own words.
    static std::string variable_name(std::string_view name) {
        std::string out;
        for (const char c : name) {
            const bool word = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                              (c >= '0' && c <= '9') || c == '_';
            out += word ? c : '_';
        }
        if (out.empty() || (out[0] >= '0' && out[0] <= '9')) {
            out.insert(0, "_");
        }
        static constexpr std::array<std::string_view, 12> kWords{
            "true", "false", "null", "and", "or", "not", "in", "self", "PI", "TAU", "INF", "NAN"};
        if (contains(kWords, std::string_view(out))) {
            out += "_";
        }
        return out;
    }

    // A controller's base layer as a .animtree.json state machine over its library, played as
    // Unity plays it: sub-state machines flattened into their states, Any State's transitions
    // copied to every state, and conditions as advance expressions of the parameters, which
    // become the tree's variables and triggers. Its path, or "" when no transition converts.
    std::string layer_tree(const LayerParts& layer, const std::string& stem,
                           const std::string& where, std::vector<std::string>& described,
                           std::vector<std::string>& positions, json::Object& initial) {
        const ControllerFile& file = *layer.file;
        const auto object = [&](const std::optional<ObjectRef>& ref) -> const Object* {
            if (!ref || ref->null()) {
                return nullptr;
            }
            const auto found = file.by_id.find(ref->file_id);
            return found != file.by_id.end() ? found->second : nullptr;
        };
        const auto listed = [&](const Node& owner, std::string_view key) {
            std::vector<const Object*> out;
            if (const Node* list = owner.get(key)) {
                for (const Node& item : list->items) {
                    if (const Object* found = object(unity::object_ref(item))) {
                        out.push_back(found);
                    }
                }
            }
            return out;
        };

        // Parameters become variables, triggers triggers, under names expressions can read.
        json::Object variables;
        json::Array triggers;
        std::map<std::string, std::pair<std::string, std::int64_t>> parameters; // name and type
        std::set<std::string> used{
            "travel",           "start",        "next", "stop", "playback", "current_length",
            "current_position", "current_delta"};
        const std::string in_controller = where + ": ";
        if (const Node* list = layer.controller->body.get("m_AnimatorParameters")) {
            for (const Node& parameter : list->items) {
                const std::string name = parameter.string_at("m_Name");
                const std::int64_t type = parameter.integer_at("m_Type", 1);
                const std::string id = take(used, variable_name(name));
                if (id != name) {
                    problems_.add("Animator parameter name", in_controller + name,
                                  "the tree's variable is " + id);
                }
                parameters[name] = {id, type};
                std::string text = name;
                if (id != name) {
                    text.append(" as ").append(id);
                }
                if (type == 1) {
                    variables[id] = parameter.number_at("m_DefaultFloat");
                    text += " (float, " + json::write(variables[id], json::Style::Compact) + ")";
                } else if (type == 3) {
                    variables[id] = parameter.integer_at("m_DefaultInt");
                    text += " (int, " + json::write(variables[id], json::Style::Compact) + ")";
                } else if (type == 4) {
                    variables[id] = parameter.flag_at("m_DefaultBool");
                    text += " (bool, " + json::write(variables[id], json::Style::Compact) + ")";
                } else if (type == 9) {
                    triggers.emplace_back(id);
                    text += " (trigger)";
                } else {
                    problems_.add("Animator parameter type", in_controller + name);
                    continue;
                }
                described.push_back(std::move(text));
            }
        }
        // A transition's conditions as parts of an expression, all of which must hold; none if
        // one names no parameter.
        const auto conditions =
            [&](const Node& transition,
                const std::string& at) -> std::optional<std::vector<std::string>> {
            std::vector<std::string> parts;
            const Node* list = transition.get("m_Conditions");
            for (const Node& condition : list != nullptr ? list->items : std::vector<Node>{}) {
                const std::string name = condition.string_at("m_ConditionEvent");
                const auto parameter = parameters.find(name);
                if (parameter == parameters.end()) {
                    problems_.add("Animator condition", at, "no parameter is named " + name);
                    return std::nullopt;
                }
                const std::string& id = parameter->second.first;
                const double threshold = condition.number_at("m_EventTreshold");
                const std::string value =
                    parameter->second.second == 3
                        ? std::to_string(std::llround(threshold))
                        : json::write(json::Value(tidy(threshold)), json::Style::Compact);
                const auto compared = [&](std::string_view op) {
                    return std::string(id).append(op).append(value);
                };
                switch (condition.integer_at("m_ConditionMode", 1)) {
                case 1: // true, or a trigger set
                    parts.push_back(id);
                    break;
                case 2:
                    parts.push_back("not " + id);
                    break;
                case 3:
                    parts.push_back(compared(" > "));
                    break;
                case 4:
                    parts.push_back(compared(" < "));
                    break;
                case 6:
                    parts.push_back(compared(" == "));
                    break;
                case 7:
                    parts.push_back(compared(" != "));
                    break;
                default:
                    problems_.add("Animator condition", at, "a mode Unity doesn't have");
                    return std::nullopt;
                }
            }
            return parts;
        };

        // Where a transition leads, as states with the conditions on the way: a state, a
        // sub-state machine's entry, or its own machine's exit.
        struct Leg {
            std::int64_t state = 0;
            std::vector<std::string> when;
        };
        std::function<std::vector<Leg>(const Object&, int)> enter;
        std::function<std::vector<Leg>(const Object&, int)> leave;
        const auto leads_to = [&](const Node& transition, const Object& machine,
                                  int depth) -> std::vector<Leg> {
            if (const Object* state = object(ref_at(transition, "m_DstState"))) {
                return {Leg{.state = state->file_id, .when = {}}};
            }
            if (const Object* sub = object(ref_at(transition, "m_DstStateMachine"))) {
                return enter(*sub, depth + 1);
            }
            if (transition.flag_at("m_IsExit")) {
                return leave(machine, depth + 1);
            }
            return {};
        };
        // Through a machine's entry: its transitions that hold, else its default state.
        enter = [&](const Object& machine, int depth) {
            std::vector<Leg> legs;
            if (depth > kMaxInstanceDepth) {
                return legs;
            }
            for (const Object* transition : listed(machine.body, "m_EntryTransitions")) {
                const auto when = conditions(transition->body, where + ": entry");
                if (transition->body.flag_at("m_Mute") || !when) {
                    continue;
                }
                for (Leg& leg : leads_to(transition->body, machine, depth)) {
                    leg.when.insert(leg.when.begin(), when->begin(), when->end());
                    legs.push_back(std::move(leg));
                }
            }
            if (const Object* first = object(ref_at(machine.body, "m_DefaultState"))) {
                legs.push_back({.state = first->file_id, .when = {}});
            }
            return legs;
        };
        // Through a machine's exit: its parent's transitions from it that hold, else the
        // parent's entry. The layer's own machine starts over at its entry.
        leave = [&](const Object& machine, int depth) {
            std::vector<Leg> legs;
            const auto parent = layer.parent_of.find(machine.file_id);
            if (depth > kMaxInstanceDepth) {
                return legs;
            }
            if (parent == layer.parent_of.end()) {
                return enter(*layer.root, depth);
            }
            const Object& outer = *parent->second;
            const Node* list = outer.body.get("m_StateMachineTransitions");
            for (const Node& item : list != nullptr ? list->items : std::vector<Node>{}) {
                const auto from = ref_at(item, "first");
                const Node* second = item.get("second");
                if (!from || from->file_id != machine.file_id || second == nullptr) {
                    continue;
                }
                for (const Object* transition : listed(item, "second")) {
                    const auto when = conditions(transition->body, where + ": exit");
                    if (transition->body.flag_at("m_Mute") || !when) {
                        continue;
                    }
                    for (Leg& leg : leads_to(transition->body, outer, depth)) {
                        leg.when.insert(leg.when.begin(), when->begin(), when->end());
                        legs.push_back(std::move(leg));
                    }
                }
            }
            for (Leg& leg : enter(outer, depth + 1)) {
                legs.push_back(std::move(leg));
            }
            return legs;
        };

        // A state for each one whose animation converted, named apart from Start and End.
        std::set<std::string> taken{"Start", "End"};
        std::map<std::int64_t, std::string> state_name;
        json::Object states;
        for (const Object* state : layer.states) {
            const auto animation = layer.names.find(state->file_id);
            if (animation == layer.names.end() || !layer.clips.contains(animation->second)) {
                continue;
            }
            std::string id = layer.state_names.at(state->file_id);
            std::ranges::replace(id, '/', '_');
            id = take(taken, id);
            state_name[state->file_id] = id;
            json::Value entry;
            if (const auto blended = layer.nodes.find(state->file_id);
                blended != layer.nodes.end()) {
                entry["node"] = blended->second;
            } else {
                entry["node"]["type"] = "Animation";
                entry["node"]["animation"] = animation->second;
            }
            if (const auto at = layer.positions.find(state->file_id); at != layer.positions.end()) {
                entry["position"] = pair(at->second[0], at->second[1]);
            }
            states.emplace(id, std::move(entry));
        }

        // Transitions in the order Unity tries them, which priorities keep: one state machine
        // transition joins Unity's between the same two states.
        json::Array transitions;
        std::map<std::pair<std::string, std::string>, std::size_t> index_of;
        const auto add = [&](const std::string& from, const std::string& to, json::Value out,
                             const std::vector<std::string>& when, const std::string& at) {
            if (from == to) {
                problems_.add(
                    "Animator transition to its own state", at,
                    "a state machine's states don't go to themselves; the state plays on");
                return;
            }
            const std::string expression = join(when, " and ");
            const auto [it, added] = index_of.try_emplace({from, to}, transitions.size());
            if (!added) {
                json::Value& kept = transitions[it->second];
                if (const json::Value* before = kept.find("advance_expression")) {
                    if (expression.empty()) {
                        kept.as_object().erase("advance_expression");
                    } else {
                        kept["advance_expression"] =
                            "(" + before->as_string() + ") or (" + expression + ")";
                    }
                }
                problems_.add("Animator transitions between the same states", at,
                              "one transition takes either's conditions, with the first's timing");
                return;
            }
            out["from"] = from;
            out["to"] = to;
            if (!expression.empty()) {
                out["advance_expression"] = expression;
            }
            transitions.push_back(std::move(out));
        };

        int start_priority = 0;
        for (const Leg& leg : enter(*layer.root, 0)) {
            const auto to = state_name.find(leg.state);
            if (to == state_name.end()) {
                problems_.add("Animator entry", where, "to a state whose animation didn't convert");
                continue;
            }
            json::Value out;
            out["advance_mode"] = "auto";
            out["priority"] = start_priority++;
            add("Start", to->second, std::move(out), leg.when, where + ": entry");
        }
        const std::size_t entries = transitions.size();

        // Any State's transitions, from every machine of the layer, which Unity tries first.
        std::vector<const Object*> any;
        for (const Object* machine : layer.machines) {
            for (const Object* transition : listed(machine->body, "m_AnyStateTransitions")) {
                any.push_back(transition);
            }
        }
        for (const Object* state : layer.states) {
            const auto from = state_name.find(state->file_id);
            const auto machine = layer.machine_of.find(state->file_id);
            if (from == state_name.end() || machine == layer.machine_of.end()) {
                continue;
            }
            const std::vector<const Object*> own = listed(state->body, "m_Transitions");
            const bool solo = std::ranges::any_of(own, [](const Object* transition) {
                return transition->body.flag_at("m_Solo") && !transition->body.flag_at("m_Mute");
            });
            const std::pair<double, bool>& clip = layer.clips.at(layer.names.at(state->file_id));
            const double length = clip.first;
            const bool loops = clip.second;
            int priority = 0;
            const auto convert = [&](const Node& body, bool from_any) {
                if (body.flag_at("m_Mute") || (solo && !from_any && !body.flag_at("m_Solo"))) {
                    return;
                }
                const std::string at = in_controller + (from_any ? "Any State" : from->second);
                const auto when = conditions(body, at);
                if (!when) {
                    return;
                }
                const bool exit_time = body.flag_at("m_HasExitTime");
                if (when->empty() && !exit_time) {
                    problems_.add("Animator transition with no conditions or exit time", at,
                                  "Unity never takes it");
                    return;
                }
                double duration = body.number_at("m_TransitionDuration");
                if (!body.flag_at("m_HasFixedDuration", true)) {
                    duration *= length;
                }
                json::Value out;
                out["advance_mode"] = "auto";
                if (duration > 0.0) {
                    out["xfade_time"] = tidy(duration);
                }
                if (exit_time) {
                    out["switch_mode"] = "at_end";
                    if (loops) {
                        out["break_loop_at_end"] = true;
                    }
                    const double starts = body.number_at("m_ExitTime", 1.0) * length;
                    if (std::abs(starts - (length - duration)) > 1e-3) {
                        problems_.add("Animator exit time", at,
                                      "the transition starts when the state has its cross-fade "
                                      "left, rather than at its exit time");
                    }
                }
                if (body.number_at("m_TransitionOffset") != 0.0) {
                    problems_.add("Animator transition offset", at,
                                  "the next state starts from its start");
                }
                if (body.integer_at("m_InterruptionSource") != 0) {
                    problems_.add("Animator transition interruption source", at,
                                  "nothing interrupts a cross-fade; the next transition waits "
                                  "for it to end");
                }
                for (const Leg& leg : leads_to(body, *machine->second, 0)) {
                    const auto to = state_name.find(leg.state);
                    if (to == state_name.end()) {
                        problems_.add("Animator transition", at,
                                      "to a state whose animation didn't convert");
                        continue;
                    }
                    if (from_any && to->second == from->second &&
                        !body.flag_at("m_CanTransitionToSelf", true)) {
                        continue;
                    }
                    std::vector<std::string> parts = *when;
                    parts.insert(parts.end(), leg.when.begin(), leg.when.end());
                    json::Value taken_out = out;
                    taken_out["priority"] = priority++;
                    add(from->second, to->second, std::move(taken_out), parts, at);
                }
            };
            for (const Object* transition : any) {
                convert(transition->body, true);
            }
            for (const Object* transition : own) {
                convert(transition->body, false);
            }
        }
        if (transitions.size() == entries && layer.nodes.empty()) {
            return {};
        }

        // Unity's blend trees read their parameters; a blend space's position is set with them.
        for (const auto& [state, blends] : layer.blends) {
            const auto name = state_name.find(state);
            if (name == state_name.end()) {
                continue;
            }
            for (const LayerParts::Blend& blend : blends) {
                const std::string key = name->second + "/" + blend.path + "blend_position";
                std::vector<std::string> ids;
                json::Array starts;
                bool moved = false;
                for (const std::string* parameter : {&blend.x, &blend.y}) {
                    if (parameter == &blend.y && blend.y.empty()) {
                        continue;
                    }
                    const auto found = parameters.find(*parameter);
                    if (found == parameters.end()) {
                        problems_.add("Animator blend parameter", in_controller + name->second,
                                      "no parameter is named " + *parameter);
                        ids.emplace_back("0");
                        starts.emplace_back(0.0);
                        continue;
                    }
                    ids.push_back(found->second.first);
                    const auto value = variables.find(found->second.first);
                    const double start = value != variables.end() && value->second.is_number()
                                             ? value->second.as_double()
                                             : 0.0;
                    moved = moved || start != 0.0;
                    starts.emplace_back(start);
                }
                positions.push_back(key + " to " +
                                    (ids.size() == 1 ? ids.front() : "[" + join(ids, ", ") + "]"));
                if (moved) {
                    initial[key] = starts.size() == 1 ? starts.front() : json::Value(starts);
                }
            }
        }

        json::Value doc;
        doc["format"] = 1;
        if (!variables.empty()) {
            doc["variables"] = std::move(variables);
        }
        if (!triggers.empty()) {
            doc["triggers"] = std::move(triggers);
        }
        doc["root"]["type"] = "StateMachine";
        doc["root"]["states"] = std::move(states);
        doc["root"]["transitions"] = std::move(transitions);
        const std::string path = destination(stem + std::string(animation::kTreeSuffix));
        keep_valid(doc, path,
                   [](const json::Value& value) { return animation::validate_tree(value); });
        add_file(path, json::write(doc) + "\n");
        ++animation_trees_;
        return path;
    }

    // What a controller's state machine does, in words, for the storyboard task that ports it:
    // its states, parameters and transitions.
    std::string state_machine_text(const ControllerFile& file, const Node& controller,
                                   const std::map<std::int64_t, std::string>& names,
                                   const std::vector<const Object*>& machines,
                                   const std::vector<std::string>& events) {
        static constexpr std::array<std::string_view, 10> kTypes{"", "float", "", "int", "bool",
                                                                 "", "",      "", "",    "trigger"};
        std::vector<std::string> parameters;
        std::map<std::string, std::int64_t> parameter_types;
        if (const Node* list = controller.get("m_AnimatorParameters")) {
            for (const Node& parameter : list->items) {
                const std::string name = parameter.string_at("m_Name");
                const std::int64_t type = parameter.integer_at("m_Type", 1);
                parameter_types[name] = type;
                std::string text = name;
                if (type >= 0 && type < static_cast<std::int64_t>(kTypes.size()) &&
                    !kTypes.at(static_cast<std::size_t>(type)).empty()) {
                    text += " (" + std::string(kTypes.at(static_cast<std::size_t>(type)));
                    std::ostringstream value;
                    if (type == 1) {
                        value << parameter.number_at("m_DefaultFloat");
                    } else if (type == 3) {
                        value << parameter.integer_at("m_DefaultInt");
                    } else if (type == 4) {
                        value << (parameter.flag_at("m_DefaultBool") ? "true" : "false");
                    }
                    text += value.str().empty() ? ")" : ", " + value.str() + ")";
                }
                parameters.push_back(std::move(text));
            }
        }
        const auto condition_text = [&](const Node& condition) {
            std::string text = condition.string_at("m_ConditionEvent");
            std::ostringstream threshold;
            threshold << condition.number_at("m_EventTreshold");
            switch (condition.integer_at("m_ConditionMode", 1)) {
            case 2:
                text.insert(0, "not ");
                break;
            case 3:
                text.append(" > ").append(threshold.str());
                break;
            case 4:
                text.append(" < ").append(threshold.str());
                break;
            case 6:
                text.append(" == ").append(threshold.str());
                break;
            case 7:
                text.append(" != ").append(threshold.str());
                break;
            default: // 1: true, or a trigger set
                break;
            }
            return text;
        };
        std::vector<std::string> transitions;
        bool blended = false;
        const auto describe = [&](const std::string& from, const Node& list_owner,
                                  std::string_view key) {
            const Node* list = list_owner.get(key);
            for (const Node& item : list != nullptr ? list->items : std::vector<Node>{}) {
                const auto ref = unity::object_ref(item);
                const auto found = ref ? file.by_id.find(ref->file_id) : file.by_id.end();
                if (found == file.by_id.end()) {
                    continue;
                }
                const Node& transition = found->second->body;
                std::string to = "exit";
                if (const auto to_state = ref_at(transition, "m_DstState");
                    to_state && !to_state->null()) {
                    const auto name = names.find(to_state->file_id);
                    to = name != names.end() ? name->second : "a missing state";
                } else if (const auto sub = ref_at(transition, "m_DstStateMachine");
                           sub && !sub->null()) {
                    const auto machine = file.by_id.find(sub->file_id);
                    to = machine != file.by_id.end()
                             ? "the " + machine->second->body.string_at("m_Name") + " state machine"
                             : "a missing state machine";
                }
                std::vector<std::string> when;
                if (const Node* conditions = transition.get("m_Conditions")) {
                    for (const Node& condition : conditions->items) {
                        when.push_back(condition_text(condition));
                    }
                }
                if (transition.flag_at("m_HasExitTime")) {
                    std::ostringstream through;
                    through << transition.number_at("m_ExitTime", 1.0);
                    when.push_back("its animation reaches exit time " + through.str());
                }
                blended = blended || transition.number_at("m_TransitionDuration") > 0.0;
                std::string text = from;
                text.append(" to ").append(to);
                if (!when.empty()) {
                    text += " when " + join(when, " and ");
                }
                transitions.push_back(std::move(text));
            }
        };
        for (const Object* machine : machines) {
            if (const Node* children = machine->body.get("m_ChildStates")) {
                for (const Node& child : children->items) {
                    const auto state = ref_at(child, "m_State");
                    const auto found = state ? file.by_id.find(state->file_id) : file.by_id.end();
                    const auto name = state ? names.find(state->file_id) : names.end();
                    if (found != file.by_id.end() && name != names.end()) {
                        describe(name->second, found->second->body, "m_Transitions");
                    }
                }
            }
            describe("any state", machine->body, "m_AnyStateTransitions");
            describe("entry", machine->body, "m_EntryTransitions");
        }
        if (parameters.empty() && transitions.empty()) {
            return {};
        }
        std::vector<std::string> state_names;
        state_names.reserve(names.size());
        for (const auto& [id, name] : names) {
            state_names.push_back(name);
        }
        std::string text =
            "States, each an animation in the library: " + join(state_names, ", ") + ".";
        if (!parameters.empty()) {
            text += " Parameters: " + join(parameters, "; ") + ".";
        }
        if (!transitions.empty()) {
            text += " Transitions: " + join(transitions, "; ") + ".";
        }
        if (text.find("exit time") != std::string::npos) {
            text += " Exit times count plays of the state's animation: 1 is its end.";
        }
        if (blended) {
            text += " Unity blends between states as it moves; an AnimationPlayer switches at "
                    "once.";
        }
        if (!events.empty()) {
            text += " Animation events: " + join(events, ", ") + ".";
        }
        return text;
    }

    // The component an Animator becomes, with each target its tracks name bound to the entity
    // under the object at that path: an AnimationTree playing its controller's state machine,
    // or, with no transitions, an AnimationPlayer starting in its first state.
    std::optional<std::pair<std::string, json::Value>>
    animator_component(const Expanded& file, int index, const Component& component,
                       bool object_active, const std::string& where) {
        const Node& body = component.body;
        const auto controller = ref_at(body, "m_Controller");
        if (!controller || controller->null()) {
            return std::nullopt;
        }
        const GameObject& go = file.objects[static_cast<std::size_t>(index)];
        const ClipSpace space = clip_space(file, go);
        const Asset* model = space == ClipSpace::Model ? model_of(go) : nullptr;
        clip_target_ = clip_target(go, space);
        LibraryOut& converted = library(*controller, where, space);
        if (converted.path.empty()) {
            return std::nullopt;
        }
        converted.users.push_back(go.entity + " in " + file.scene);
        if (!contains(converted.scenes, file.scene)) {
            converted.scenes.push_back(file.scene);
        }
        // Root motion on a model's instance comes from its hips, or the node its import settings
        // name, which a tree takes out of the pose for gameplay code to move the object by.
        std::string root_node;
        if (body.flag_at("m_ApplyRootMotion")) {
            root_node = model != nullptr && !converted.first.empty()
                            ? root_motion_node(*model, where)
                            : std::string();
            if (root_node.empty()) {
                problems_.add("Animator root motion", where,
                              file.three_d
                                  ? "the animation moves the object's Transform3D and its "
                                    "model's nodes in place, not the object by its root"
                                  : "the animation moves the object's Transform2D directly");
            } else {
                problems_.add("Animator root motion", where,
                              "the AnimationTree takes the " + root_node +
                                  " node's motion out of the pose into root_motion_position and "
                                  "root_motion_rotation, as Godot's does; gameplay code moves "
                                  "the object by them, as Unity's Animator did");
            }
        }
        json::Value player;
        player["library"] = converted.path;
        if (!converted.first.empty()) {
            player["animation"] = converted.first;
        }
        player["playing"] =
            !converted.first.empty() && body.flag_at("m_Enabled", true) && object_active;
        json::Object targets;
        for (const std::string& name : converted.targets) {
            const auto hash = name.rfind('#');
            const std::string path = hash == std::string::npos ? name : name.substr(0, hash);
            const int at = object_at(file, index, path);
            if (at < 0) {
                problems_.add("animated object missing",
                              std::string(where).append(": ").append(path),
                              "no object at that path under the Animator; its tracks don't play");
                targets.emplace(name, nullptr);
                continue;
            }
            const GameObject& target = file.objects[static_cast<std::size_t>(at)];
            const auto sprite_entity = target.added.find("sprite");
            const bool sprite_child =
                hash != std::string::npos && sprite_entity != target.added.end();
            targets.emplace(name, sprite_child ? sprite_entity->second : target.entity);
            if (hash != std::string::npos && converted.offsets) {
                for (const Component& renderer : target.components) {
                    if (renderer.class_id == kSpriteRenderer &&
                        (renderer.body.flag_at("m_FlipX") || renderer.body.flag_at("m_FlipY"))) {
                        problems_.add("flipped sprite with animated pivots", where,
                                      "its animation's sprite offsets don't flip with it");
                    }
                }
            }
        }
        if (!converted.tree.empty() || !root_node.empty()) {
            json::Value tree;
            tree["tree_root"] = converted.tree.empty() ? motion_tree(converted) : converted.tree;
            tree["libraries"][""] = converted.path;
            tree["deterministic"] = false;
            if (!body.flag_at("m_Enabled", true) || !object_active) {
                tree["active"] = false;
            }
            if (!targets.empty()) {
                tree["targets"] = std::move(targets);
            }
            if (!converted.parameters.empty()) {
                tree["parameters"] = converted.parameters;
            }
            if (!root_node.empty()) {
                tree["root_motion_track"] = root_node;
            }
            return std::pair{std::string("AnimationTree"), std::move(tree)};
        }
        if (!targets.empty()) {
            player["targets"] = std::move(targets);
        }
        return std::pair{std::string("AnimationPlayer"), std::move(player)};
    }

    // What an Animator's clips convert for: by its scene, and in 3D by whether its object is a
    // model's instance.
    static ClipSpace clip_space(const Expanded& file, const GameObject& go) {
        if (!file.three_d) {
            return ClipSpace::Flat;
        }
        const bool model = std::ranges::any_of(
            go.components, [](const Component& c) { return c.class_id == kModelInstance; });
        return model ? ClipSpace::Model : ClipSpace::Objects;
    }

    // The model an object is an instance of, or null.
    [[nodiscard]] const Asset* model_of(const GameObject& go) const {
        const std::string guid = go_model_guid(go);
        return guid.empty() ? nullptr : asset_of(guid);
    }

    // The model whose instance plays an Animator's clips, which a model's clips are made for.
    [[nodiscard]] std::string clip_target(const GameObject& go, ClipSpace space) const {
        return space == ClipSpace::Model && model_of(go) != nullptr ? go_model_guid(go)
                                                                    : std::string();
    }

    static std::string go_model_guid(const GameObject& go) {
        for (const Component& component : go.components) {
            if (component.class_id == kModelInstance) {
                return component.body.string_at("guid");
            }
        }
        return {};
    }

    // The node of a model whose motion is an Animator's root motion: a humanoid model's hips, or
    // the node a generic model's import settings name; "" for neither.
    std::string root_motion_node(const Asset& model, const std::string& where) {
        const Avatar* avatar = avatar_of(model, where);
        const scene3d::Model* loaded = model_file(model, where);
        if (avatar == nullptr || loaded == nullptr) {
            return {};
        }
        const int node = avatar->humanoid() ? avatar->bones.at("Hips") : avatar->motion_node;
        return node >= 0 ? loaded->nodes[static_cast<std::size_t>(node)].name : std::string();
    }

    // A tree playing a library's first animation, for root motion where its controller has no
    // transitions to make one.
    const std::string& motion_tree(LibraryOut& library) {
        if (!library.motion_tree.empty()) {
            return library.motion_tree;
        }
        json::Value doc;
        doc["format"] = 1;
        doc["root"]["type"] = "Animation";
        doc["root"]["animation"] = library.first;
        library.motion_tree =
            library.path.substr(0, library.path.size() - animation::kAnimationSuffix.size()) +
            std::string(animation::kTreeSuffix);
        keep_valid(doc, library.motion_tree,
                   [](const json::Value& value) { return animation::validate_tree(value); });
        add_file(library.motion_tree, json::write(doc) + "\n");
        ++animation_trees_;
        return library.motion_tree;
    }

    // The object at a path of names below an object ("" for the object itself), or -1.
    static int object_at(const Expanded& file, int index, std::string_view path) {
        int at = index;
        for (const std::string_view part :
             path.empty() ? std::vector<std::string_view>{} : split_path(path)) {
            const GameObject& parent = file.objects[static_cast<std::size_t>(at)];
            const auto child = std::ranges::find_if(parent.children, [&](int c) {
                const GameObject& candidate = file.objects[static_cast<std::size_t>(c)];
                return !candidate.removed && candidate.body.string_at("m_Name") == part;
            });
            if (child == parent.children.end()) {
                return -1;
            }
            at = *child;
        }
        return at;
    }

    // The 3D objects a scene's Animators move and those above them, which keep Unity's place
    // rather than turning half a turn for a camera or light on them, so their tracks move them as
    // in Unity; their cameras and lights go on entities under them.
    void mark_animated(const Expanded& file) {
        const auto mark = [&](int at) {
            while (at >= 0 && animated_.insert(at).second) {
                at = file.objects[static_cast<std::size_t>(at)].parent;
            }
        };
        for (std::size_t index = 0; index < file.objects.size(); ++index) {
            const GameObject& go = file.objects[index];
            for (const Component& component : go.components) {
                const auto controller = ref_at(component.body, "m_Controller");
                if (go.removed || component.class_id != kAnimator || !controller ||
                    controller->null()) {
                    continue;
                }
                const int self = static_cast<int>(index);
                const ClipSpace space = clip_space(file, go);
                clip_target_ = clip_target(go, space);
                const LibraryOut& converted =
                    library(*controller, file.path + ": " + object_path(file, self), space);
                mark(self);
                for (const std::string& name : converted.targets) {
                    mark(object_at(file, self, name.substr(0, name.rfind('#'))));
                }
            }
        }
    }

    // Controllers no scene's Animator uses convert too, since gameplay code may set them; clips
    // no controller plays are reported.
    void convert_unused_animation() {
        std::map<std::string, std::string> by_path; // GUIDs
        for (const auto& [guid, asset] : assets_by_guid_) {
            by_path[asset.path] = guid;
        }
        for (const auto& [path, guid] : by_path) {
            const std::string extension = extension_of(path);
            if ((extension == ".controller" || extension == ".overridecontroller") &&
                !libraries_.contains(guid)) {
                (void)library(ObjectRef{.file_id = 0, .guid = guid}, path,
                              project_3d_ ? ClipSpace::Objects : ClipSpace::Flat);
            }
        }
        std::set<std::string> played;
        for (const auto& entry : clips_) {
            played.insert(entry.first.substr(0, entry.first.find(':')));
        }
        for (const auto& [path, guid] : by_path) {
            if (extension_of(path) == ".anim" && !played.contains(guid)) {
                problems_.add("animation clip no Animator controller plays", path,
                              "clips convert as the states of the controllers that play them");
            }
        }
    }

    // --- Writing scenes ---

    struct ScriptUse {
        std::string scene;
        std::string entity;
        json::Value values;
        bool enabled = true;
    };

    struct ScriptRecord {
        std::vector<ScriptUse> uses;
        std::vector<std::string> calls;            // UI events that call it
        std::vector<ScriptConnection> connections; // the same, for its skeleton
    };

    // The entity made for an object: the prefab's own for an object an instance brought, when the
    // prefab made one (source is then its ID in the prefab's scene), else a new one of this
    // scene's.
    std::string added_id(Expanded& file, GameObject& go, const std::string& purpose,
                         std::string& source) {
        if (go.instance >= 0) {
            const Instance& instance = file.instances[static_cast<std::size_t>(go.instance)];
            const auto found = go.source_added.find(purpose);
            if (found != go.source_added.end() && instance.brings.contains(found->second)) {
                source = found->second;
                return file.objects[static_cast<std::size_t>(instance.root)].entity + "/" +
                       found->second;
            }
        }
        source.clear();
        std::string base = go.entity;
        std::ranges::replace(base, '/', '_');
        return take(file.used, base + "_" + purpose);
    }

    // The components each entity of a converted scene has, with its instances expanded.
    const std::map<std::string, std::set<std::string>>& scene_components(const std::string& scene) {
        auto [it, inserted] = scene_components_.try_emplace(scene);
        if (!inserted) {
            return it->second;
        }
        const world::FileReader reader = planned_files();
        auto text = reader(scene);
        auto parsed = text ? json::parse(text.value()) : Result<json::Value>(text.error());
        if (!parsed) {
            return it->second;
        }
        world::World loaded(components_);
        loaded.set_file_reader(reader);
        if (!world::load_scene(loaded, parsed.value())) {
            return it->second;
        }
        for (const std::string_view id : loaded.ids()) {
            const entt::entity entity = loaded.find(id);
            std::set<std::string>& names = it->second[std::string(id)];
            for (const auto& type : components_.types()) {
                if (type->has(loaded, entity)) {
                    names.insert(type->name());
                }
            }
        }
        return it->second;
    }

    // An entity an instance brings, written whole: its components' fields that weren't converted
    // as their defaults, and null for each component its prefab's entity has that it doesn't, so
    // nothing of the prefab's stays that this scene's object doesn't have.
    void complete(json::Object& components, const Instance& instance, const std::string& source) {
        for (auto& [name, fields] : components) {
            const world::ComponentType* type = components_.find(name);
            if (type == nullptr || !fields.is_object() || !type->defaults().is_object()) {
                continue;
            }
            for (const auto& [field, value] : type->defaults().as_object()) {
                if (fields.find(field) == nullptr) {
                    fields[field] = value;
                }
            }
        }
        const auto& entities = scene_components(instance.prefab->scene);
        if (const auto it = entities.find(source); it != entities.end()) {
            for (const std::string& name : it->second) {
                if (!components.contains(name)) {
                    components[name] = nullptr;
                }
            }
        }
    }

    void write_scene(Expanded& file) {
        file.scene = destination(without_extension(file.path) + std::string(world::kSceneSuffix));
        scene_clear_color_.reset();
        scene_lit_ = false;
        scene_global_ = false;
        scene_camera_.reset();
        lod_hidden_.clear();
        animated_.clear();
        if (file.three_d) {
            ++three_d_files_;
            hide_lod_levels(file);
            mark_animated(file);
        }
        json::Value record;
        record["scene"] = file.scene;
        json::Object entities;
        std::vector<int> order; // every object, each before the objects under it
        std::function<void(int)> collect = [&](int index) {
            order.push_back(index);
            for (const int child : file.objects[static_cast<std::size_t>(index)].children) {
                collect(child);
            }
        };
        for (const int root : file.roots) {
            collect(root);
        }
        mark_ui(file);
        // First each object's components and the entities made for it, so an Animator can then
        // bind its tracks to the entities under it.
        struct Done {
            Converted converted;
            std::vector<std::string> sources; // of its added entities: their IDs in the prefab
        };
        std::vector<std::optional<Done>> done(file.objects.size());
        for (const int index : order) {
            GameObject& go = file.objects[static_cast<std::size_t>(index)];
            if (go.removed || (!go.node.empty() && go.entity.empty())) {
                continue;
            }
            if (!go.folded.empty()) {
                // Part of a control above; its rect still places what is lifted out of it.
                if (const Component* transform = transform_of(go);
                    go.ui && transform != nullptr && go.parent >= 0) {
                    const GameObject& parent = file.objects[static_cast<std::size_t>(go.parent)];
                    json::Value ignored;
                    go.rect = rect_control(transform->body, parent.rect, ignored);
                    go.ui_ppu = parent.ui_ppu;
                }
                continue;
            }
            const Context ctx{.file = &file,
                              .index = index,
                              .where = file.path + ": " + object_path(file, index),
                              .active = active(file, index)};
            Done& object = done[static_cast<std::size_t>(index)].emplace();
            object.converted = convert_object(ctx);
            for (const auto& [purpose, components] : object.converted.added) {
                std::string source;
                go.added[purpose] = added_id(file, go, purpose, source);
                object.sources.push_back(std::move(source));
            }
        }
        // A tile map layer moved to put its cells where Unity's are moves its children back.
        for (const int index : order) {
            const auto& parent = done[static_cast<std::size_t>(index)];
            if (!parent || parent->converted.child_shift == Vec2{}) {
                continue;
            }
            const Vec2 shift = parent->converted.child_shift;
            for (const int child : file.objects[static_cast<std::size_t>(index)].children) {
                auto& object = done[static_cast<std::size_t>(child)];
                if (!object || file.objects[static_cast<std::size_t>(child)].ui) {
                    continue;
                }
                json::Value& transform = object->converted.components["Transform2D"];
                const json::Value* position = transform.find("position");
                const double x = position != nullptr ? position->as_array()[0].as_double() : 0.0;
                const double y = position != nullptr ? position->as_array()[1].as_double() : 0.0;
                transform["position"] =
                    pair(x - static_cast<double>(shift.x), y - static_cast<double>(shift.y));
            }
        }
        for (const int index : order) {
            GameObject& go = file.objects[static_cast<std::size_t>(index)];
            if (!go.node.empty() && go.entity.empty()) {
                continue; // a model's node, which its model draws
            }
            if (!go.folded.empty()) {
                record["entities"][object_path(file, index)] = go.folded;
                continue;
            }
            record["entities"][object_path(file, index)] = go.entity;
            if (go.removed) {
                entities[go.entity] = nullptr;
                continue;
            }
            Done& object = *done[static_cast<std::size_t>(index)];
            Converted& converted = object.converted;
            if (converted.animator != nullptr) {
                const std::string where = file.path + ": " + object_path(file, index);
                if (auto made = animator_component(file, index, *converted.animator,
                                                   active(file, index), where)) {
                    // A model imported at a scale draws on an entity of its own, whose nodes the
                    // animation moves.
                    const auto drawn = std::ranges::find_if(
                        converted.added, [](const auto& added) { return added.first == "model"; });
                    json::Object& drawing =
                        drawn != converted.added.end() ? drawn->second : converted.components;
                    leave_out_animated(drawing, made->second, where);
                    if (drawn != converted.added.end()) {
                        problems_.add("Animator on a model imported at a scale", where,
                                      "it plays on the entity that draws the model, so tracks of "
                                      "the object's own Transform move that entity");
                    }
                    drawing[made->first] = std::move(made->second);
                }
            }
            const Instance* instance =
                go.instance >= 0 ? &file.instances[static_cast<std::size_t>(go.instance)] : nullptr;
            json::Value entity;
            if (converted.place) {
                // Under a turned parent, an object turns back.
                Place3D place = *converted.place;
                const std::size_t parent = static_cast<std::size_t>(std::max(go.parent, 0));
                if (go.parent >= 0 && done[parent] && done[parent]->converted.turned) {
                    place.position = {-place.position[0], place.position[1], -place.position[2]};
                    place.rotation = multiply(kHalfTurn, place.rotation);
                }
                if (converted.turned) {
                    place.rotation = multiply(place.rotation, kHalfTurn);
                }
                converted.components["Transform3D"] = transform_3d(place);
            }
            // A control's children are controls, listed in its Control; other objects' children
            // in their Transform2D or Transform3D. A canvas under a world object is a root of the
            // UI.
            const char* placed = file.three_d ? "Transform3D" : "Transform2D";
            json::Value& transform = converted.components[go.ui ? "Control" : placed];
            std::vector<std::string> children;
            std::function<void(int)> lift = [&](int at) {
                for (const int child : file.objects[static_cast<std::size_t>(at)].children) {
                    const GameObject& c = file.objects[static_cast<std::size_t>(child)];
                    if (c.removed || !c.node.empty()) {
                        continue;
                    }
                    if (!c.folded.empty()) {
                        lift(child);
                    } else if (c.ui == go.ui) {
                        children.push_back(c.entity);
                    } else if (go.ui) {
                        problems_.add("object under a UI object",
                                      file.path + ": " + object_path(file, child),
                                      "placed in the world, apart from the controls");
                    }
                }
            };
            lift(index);
            // A model's nodes something hangs on follow them under the entity that draws it.
            std::vector<std::string> attachments;
            std::function<void(int)> nodes = [&](int at) {
                for (const int child : file.objects[static_cast<std::size_t>(at)].children) {
                    const GameObject& c = file.objects[static_cast<std::size_t>(child)];
                    if (!c.removed && !c.node.empty()) {
                        if (!c.entity.empty()) {
                            attachments.push_back(c.entity);
                        }
                        nodes(child);
                    }
                }
            };
            if (go.node.empty()) {
                nodes(index);
            }
            json::Value* holder = &transform;
            for (std::size_t i = 0; i < converted.added.size(); ++i) {
                auto& [purpose, components] = converted.added[i];
                const std::string& id = go.added[purpose];
                transform["children"].push_back(id);
                if (!object.sources[i].empty()) {
                    complete(components, *instance, object.sources[i]);
                }
                json::Value child;
                child["components"] = std::move(components);
                entities[id] = std::move(child);
                if (purpose == converted.children_to) {
                    holder = &entities[id]["components"]["Control"];
                }
            }
            for (const std::string& child : children) {
                (*holder)["children"].push_back(child);
            }
            if (!attachments.empty()) {
                json::Value* drawing = &transform;
                if (const auto model = go.added.find("model"); model != go.added.end()) {
                    drawing = &entities[model->second]["components"]["Transform3D"];
                    problems_.add("object on a node of a model imported at a scale",
                                  file.path + ": " + object_path(file, index),
                                  "it follows the node on the entity that draws the model, so it "
                                  "is scaled as the model is");
                }
                for (const std::string& attachment : attachments) {
                    (*drawing)["children"].push_back(attachment);
                }
            }
            if (instance != nullptr) {
                complete(converted.components, *instance, go.source_id);
            }
            entity["components"] = std::move(converted.components);
            if (instance != nullptr && instance->root == index) {
                entity["extends"] = instance->prefab->scene;
            }
            if (const std::string tag = go.body.string_at("m_TagString");
                !tag.empty() && tag != "Untagged") {
                tags_[tag].push_back(file.scene + "#" + go.entity);
            }
            entities[go.entity] = std::move(entity);
        }
        // Entities an instance brings that nothing here makes any more are left out.
        for (const Instance& instance : file.instances) {
            const std::string& root = file.objects[static_cast<std::size_t>(instance.root)].entity;
            for (const std::string& brought : instance.brings) {
                const std::string id = std::string(root).append("/").append(brought);
                if (!entities.contains(id)) {
                    entities[id] = nullptr;
                }
            }
        }
        if (file.three_d && extension_of(file.path) == ".unity") {
            environment_3d(file, entities);
        }
        // URP's 2D lights leave what no light reaches dark unless a global light lights it.
        if (scene_lit_ && !scene_global_ && extension_of(file.path) == ".unity") {
            const std::string id = take(file.used, "ambient");
            json::Value ambient;
            ambient["components"]["CanvasModulate"]["color"] = json::Array{0.0, 0.0, 0.0, 1.0};
            entities[id] = std::move(ambient);
            problems_.add("2D lights without a Global Light 2D", file.path,
                          "URP leaves what no light reaches dark, so entity \"" + id +
                              "\" has a black CanvasModulate");
        }
        for (const auto& [id, entity] : entities) {
            if (!entity.is_null()) {
                file.entities.insert(id);
            }
        }
        json::Value converted;
        converted["format"] = 1;
        converted["entities"] = std::move(entities);
        if (file.root >= 0) {
            converted["root"] = file.objects[static_cast<std::size_t>(file.root)].entity;
        }
        const world::FileReader reader = planned_files();
        for (int pass = 0; pass < 64; ++pass) {
            auto errors = world::validate_scene(components_, converted, reader);
            if (errors.empty()) {
                errors = animation::check_players(converted, reader);
            }
            if (errors.empty()) {
                break;
            }
            std::vector<std::string> removed;
            for (const json::ValidationError& error : errors) {
                problems_.add("invalid value", file.path + ": " + error.path, error.message);
                if (pointer_within(removed, error.path)) {
                    continue;
                }
                if (!remove_pointer(converted, error.path)) {
                    converted["entities"] = json::Object{};
                    break;
                }
                removed.push_back(error.path);
            }
        }
        auto text = world::format_scene(components_, json::write(converted), reader);
        add_file(file.scene, text ? text.value() : json::write(converted) + "\n");
        if (scene_clear_color_) {
            clear_colors_[file.path] = *scene_clear_color_;
        }
        report_scenes_[file.path] = std::move(record);
        ++(extension_of(file.path) == ".prefab" ? prefabs_ : scenes_);
    }

    // --- 3D ---

    // Unity's axes are left-handed and Godot's right-handed: a 3D scene converts mirrored in x,
    // which keeps every picture as Unity draws it. Unity's cameras, lights and quads face down
    // their +z, Godot's down -z, so they turn half a turn about y.
    static constexpr Quaternion kHalfTurn{0.0, 1.0, 0.0, 0.0};

    // a, then b in a's space: the Hamilton product a * b, as Godot's Quaternion multiplies.
    static Quaternion multiply(const Quaternion& a, const Quaternion& b) {
        const auto [ax, ay, az, aw] = a;
        const auto [bx, by, bz, bw] = b;
        return {(aw * bx) + (ax * bw) + (ay * bz) - (az * by),
                (aw * by) + (ay * bw) + (az * bx) - (ax * bz),
                (aw * bz) + (az * bw) + (ax * by) - (ay * bx),
                (aw * bw) - (ax * bx) - (ay * by) - (az * bz)};
    }

    static json::Value triple(const std::array<double, 3>& v) {
        return json::Array{tidy_number(v[0]), tidy_number(v[1]), tidy_number(v[2])};
    }

    static json::Value rgba(const Node& color) {
        return json::Array{color.number_at("r", 1.0), color.number_at("g", 1.0),
                           color.number_at("b", 1.0), color.number_at("a", 1.0)};
    }

    static json::Value half_turn() { return json::Array{0.0, tidy_number(std::numbers::pi), 0.0}; }

    // 3 for one of Unity's 3D components, 2 for a 2D one, 0 for either.
    static int dimension_of(int class_id, bool orthographic) {
        switch (class_id) {
        case kMeshRenderer:
        case kSkinnedMeshRenderer:
        case kLight:
        case kRigidbody:
        case kBoxCollider:
        case kSphereCollider:
        case kCapsuleCollider:
        case kMeshCollider:
        case kCharacterController:
        case kTerrain:
        case kTerrainCollider:
        case kModelInstance:
            return 3;
        case kSpriteRenderer:
        case kRigidbody2D:
        case kBoxCollider2D:
        case kCircleCollider2D:
        case kCapsuleCollider2D:
        case kPolygonCollider2D:
        case kEdgeCollider2D:
        case kTilemap:
            return 2;
        case kCamera:
            return orthographic ? 2 : 3;
        default:
            return 0;
        }
    }

    // Whether a scene or prefab converts in 3D: it has more of Unity's 3D components than its 2D
    // ones, or as many and the project has more.
    [[nodiscard]] bool is_3d(const Expanded& file) const {
        int three = 0;
        int two = 0;
        for (const GameObject& go : file.objects) {
            if (go.removed) {
                continue;
            }
            for (const Component& component : go.components) {
                const int dimension =
                    dimension_of(component.class_id, component.body.flag_at("orthographic"));
                three += dimension == 3 ? 1 : 0;
                two += dimension == 2 ? 1 : 0;
            }
        }
        return three > two || (three == two && project_3d_);
    }

    // Whether the project's scenes and prefabs have more 3D components than 2D ones, counted by
    // the class IDs their objects start with.
    void count_dimensions() {
        int three = 0;
        int two = 0;
        for (const std::string& file : files_) {
            const std::string extension = extension_of(file);
            if (extension != ".unity" && extension != ".prefab") {
                continue;
            }
            const auto text = platform::read_file(source_ + file);
            if (!text) {
                continue;
            }
            const std::string_view view = text.value();
            constexpr std::string_view kStart = "--- !u!";
            for (std::size_t at = view.find(kStart); at != std::string_view::npos;
                 at = view.find(kStart, at + 1)) {
                const std::size_t begin = at + kStart.size();
                const std::size_t end = view.find(' ', begin);
                const auto id = parse_int(view.substr(begin, end - begin));
                const int dimension =
                    id && *id != kCamera ? dimension_of(static_cast<int>(*id), false) : 0;
                three += dimension == 3 ? 1 : 0;
                two += dimension == 2 ? 1 : 0;
            }
        }
        project_3d_ = three > two;
    }

    // An object's place from its Transform, mirrored in x, which turns its rotations the other way
    // about y and z.
    static Place3D place_of(const Node& transform) {
        Place3D place;
        place.position = {-transform.number_at("m_LocalPosition.x"),
                          transform.number_at("m_LocalPosition.y"),
                          transform.number_at("m_LocalPosition.z")};
        place.rotation = {transform.number_at("m_LocalRotation.x"),
                          -transform.number_at("m_LocalRotation.y"),
                          -transform.number_at("m_LocalRotation.z"),
                          transform.number_at("m_LocalRotation.w", 1.0)};
        place.scale = {transform.number_at("m_LocalScale.x", 1.0),
                       transform.number_at("m_LocalScale.y", 1.0),
                       transform.number_at("m_LocalScale.z", 1.0)};
        return place;
    }

    static json::Value transform_3d(const Place3D& place) {
        json::Value out = json::Object{};
        std::array<double, 3> position{};
        std::array<double, 3> scale{};
        for (std::size_t i = 0; i < 3; ++i) {
            position.at(i) = tidy_number(place.position.at(i));
            scale.at(i) = tidy_number(place.scale.at(i));
        }
        if (position != std::array<double, 3>{}) {
            out["position"] = triple(position);
        }
        if (const std::array<double, 3> rotation = quaternion_euler(place.rotation);
            rotation != std::array<double, 3>{}) {
            out["rotation"] = triple(rotation);
        }
        if (scale != std::array<double, 3>{1.0, 1.0, 1.0}) {
            out["scale"] = triple(scale);
        }
        if (!place.visible) {
            out["visible"] = false;
        }
        return out;
    }

    // The parts of a 3D object that convert together.
    struct Parts3D {
        const Component* filter = nullptr;
        const Component* renderer = nullptr;
        const Component* model = nullptr;
        const Component* camera = nullptr;
        const Component* light = nullptr;
        const Component* listener = nullptr;
        const Component* rigidbody = nullptr;
        const Component* controller = nullptr;
        const Component* terrain = nullptr;
        const Component* terrain_collider = nullptr;
        std::vector<const Component*> colliders;
    };

    // Parts that turn half a turn, by what they are for, with their components.
    using Turned = std::vector<std::pair<std::string, json::Object>>;

    void convert_object_3d(const Context& ctx, Converted& out) {
        const GameObject& go = ctx.file->objects[static_cast<std::size_t>(ctx.index)];
        Place3D place;
        if (const Component* transform = transform_of(go)) {
            place = place_of(transform->body);
        }
        place.visible = go.body.flag_at("m_IsActive", true);
        if (!go.node.empty()) {
            // A model's node with something on it follows the node, from where it is posed.
            out.components["BoneAttachment3D"]["bone_name"] = go.node;
            if (const auto posed = node_place(*ctx.file, ctx.index, ctx.where)) {
                place = *posed;
            }
            place.visible = go.body.flag_at("m_IsActive", true);
        }
        Parts3D parts;
        for (const Component& component : go.components) {
            if (!go.node.empty() && model_made(component.class_id)) {
                continue; // drawn with the model
            }
            switch (component.class_id) {
            case kTransform:
            case kRectTransform:
            case kGuiLayer:
            case kFlareLayer:
            case kLodGroup: // read before the scene's objects convert
                break;
            case kMeshFilter:
                parts.filter = &component;
                break;
            case kMeshRenderer:
                parts.renderer = &component;
                break;
            case kModelInstance:
                parts.model = &component;
                break;
            case kCamera:
                parts.camera = &component;
                break;
            case kLight:
                parts.light = &component;
                break;
            case kAudioListener:
                parts.listener = &component;
                break;
            case kAudioSource:
                audio_source(component, ctx, out, true);
                break;
            case kRigidbody:
                parts.rigidbody = &component;
                break;
            case kBoxCollider:
            case kSphereCollider:
            case kCapsuleCollider:
            case kMeshCollider:
                parts.colliders.push_back(&component);
                break;
            case kCharacterController:
                parts.controller = &component;
                break;
            case kTerrain:
                parts.terrain = &component;
                break;
            case kTerrainCollider:
                parts.terrain_collider = &component;
                break;
            case kSkinnedMeshRenderer:
                problems_.add("component SkinnedMeshRenderer", ctx.where,
                              "a skinned mesh draws with the model it is part of; instance the "
                              "model");
                break;
            case kAnimator:
                out.animator = &component;
                break;
            case kSpriteRenderer:
            case kRigidbody2D:
            case kBoxCollider2D:
            case kCircleCollider2D:
            case kCapsuleCollider2D:
            case kPolygonCollider2D:
            case kEdgeCollider2D:
            case kCompositeCollider2D:
            case kHingeJoint2D:
            case kSpringJoint2D:
            case kSliderJoint2D:
            case kGrid:
            case kTilemap:
            case kTilemapRenderer:
            case kTilemapCollider2D:
            case kPlatformEffector2D:
                problems_.add("component " + component.type, ctx.where, std::string(k2DWhy));
                break;
            case kMonoBehaviour: {
                if (const KnownScript* lost = lost_script(component);
                    lost != nullptr && !lost->ui) {
                    problems_.add("component " + std::string(lost->name), ctx.where,
                                  std::string(kLostWhy));
                    break;
                }
                const KnownScript* known = known_script(component);
                const std::string_view name = known != nullptr ? known->name : std::string_view();
                if (name == "Light2D" || name == "ShadowCaster2D" ||
                    name == "CompositeShadowCaster2D") {
                    problems_.add("component " + std::string(name), ctx.where, std::string(k2DWhy));
                } else {
                    mono_behaviour(component, ctx, out);
                }
                break;
            }
            default:
                problems_.add("component " + component.type, ctx.where,
                              component_why(component.type));
                break;
            }
        }
        Turned turned;
        mesh_3d(ctx, parts, out, turned);
        if (parts.camera != nullptr) {
            turned.emplace_back("camera", camera_3d(*parts.camera, ctx, go));
        }
        if (parts.light != nullptr) {
            light_3d(*parts.light, ctx, out, turned);
        }
        if (parts.listener != nullptr) {
            json::Object listener;
            listener["AudioListener3D"]["current"] =
                parts.listener->body.flag_at("m_Enabled", true) && ctx.active;
            turned.emplace_back("listener", std::move(listener));
        }
        physics_3d(ctx, parts, out);
        terrain_3d(ctx, parts, place, out);
        if (const auto body = out.components.find("RigidBody3D");
            out.collisions && body != out.components.end()) {
            body->second["contact_monitor"] = true;
        }
        place_turned(out, turned, animated_.contains(ctx.index) || !go.node.empty());
        out.place = place;
    }

    // Parts that turn go on the object, which turns with them, when nothing else on it faces a
    // way and no animation moves it; otherwise each goes on an entity of its own under it,
    // turned half a turn.
    static void place_turned(Converted& out, Turned& turned, bool animated) {
        if (turned.empty()) {
            return;
        }
        static constexpr std::array<std::string_view, 7> kFacing{
            "MeshInstance3D", "CollisionShape3D", "StaticBody3D", "AnimatableBody3D",
            "RigidBody3D",    "CharacterBody3D",  "Area3D"};
        const bool facing = std::ranges::any_of(kFacing, [&](std::string_view name) {
            return out.components.contains(std::string(name));
        });
        if (facing || animated || !out.added.empty()) {
            for (auto& [purpose, components] : turned) {
                components["Transform3D"]["rotation"] = half_turn();
                out.added.emplace_back(purpose, std::move(components));
            }
            return;
        }
        for (auto& [purpose, components] : turned) {
            for (auto& [name, fields] : components) {
                out.components[name] = std::move(fields);
            }
        }
        out.turned = true;
    }

    // Renderers in a LOD group's levels past its first don't draw: only its most detailed level
    // does.
    void hide_lod_levels(const Expanded& file) {
        for (const GameObject& go : file.objects) {
            for (const Component& component : go.components) {
                const Node* lods = component.body.get("m_LODs");
                if (component.class_id != kLodGroup || lods == nullptr) {
                    continue;
                }
                std::set<std::int64_t> first;
                std::set<std::int64_t> rest;
                for (std::size_t level = 0; level < lods->items.size(); ++level) {
                    const Node* renderers = lods->items[level].get("renderers");
                    for (const Node& entry :
                         renderers != nullptr ? renderers->items : std::vector<Node>{}) {
                        if (const auto ref = ref_at(entry, "renderer"); ref && !ref->null()) {
                            (level == 0 ? first : rest).insert(file.resolve(ref->file_id));
                        }
                    }
                }
                for (const std::int64_t id : rest) {
                    if (!first.contains(id)) {
                        lod_hidden_.insert(id);
                    }
                }
            }
        }
    }

    // Render layers from an object's layer: Unity has 32, Godot 20.
    json::Value render_layers(std::int64_t layer, const std::string& where) {
        if (layer < 0 || layer >= 20) {
            problems_.add("object on a layer past 19", where,
                          "there are 20 render layers; it is on the first");
            return std::int64_t{1};
        }
        return std::int64_t{1} << layer;
    }

    // --- Terrains ---

    // What a TerrainData asset gives an HTerrain: its maps, written once, its scale and its
    // ground textures, and how wide it is, which the terrain moves by as x mirrors.
    struct TerrainFields {
        json::Value fields;
        double width = 0.0;
    };

    // A Terrain as an HTerrain on an entity of its own under the object, moved its width along -x:
    // Unity's terrains reach toward +x from their object, so mirrored they reach toward -x, and
    // their columns are flipped. Its TerrainCollider, if it has one, is the HTerrain's collision.
    void terrain_3d(const Context& ctx, const Parts3D& parts, const Place3D& place,
                    Converted& out) {
        if (parts.terrain == nullptr) {
            if (parts.terrain_collider != nullptr) {
                problems_.add("TerrainCollider without a Terrain", ctx.where,
                              "an HTerrain draws and collides together; put the Terrain beside it");
            }
            return;
        }
        const Node& terrain = parts.terrain->body;
        const ObjectRef data = ref_at(terrain, "m_TerrainData").value_or(ObjectRef{});
        const Asset* asset = data.guid.empty() ? nullptr : asset_of(data.guid);
        if (asset == nullptr) {
            problems_.add("Terrain", ctx.where,
                          "its TerrainData isn't an asset in the project's Assets folder");
            return;
        }
        const std::string key = data.guid + ":" + std::to_string(data.file_id);
        auto [it, inserted] = terrains_.try_emplace(key);
        if (inserted) {
            it->second = terrain_data_fields(*asset, data.file_id);
        }
        const std::optional<TerrainFields>& found = it->second;
        if (!found) {
            return;
        }
        const TerrainFields& converted = *found;
        json::Object components;
        json::Value hterrain = converted.fields;
        const std::int64_t shadows = terrain.integer_at("m_ShadowCastingMode", 2);
        static constexpr std::array<std::string_view, 4> kShadows{"off", "on", "double_sided",
                                                                  "shadows_only"};
        if (shadows >= 0 && shadows < 4 && shadows != 1) {
            hterrain["cast_shadow"] = kShadows.at(static_cast<std::size_t>(shadows));
        }
        const GameObject& go = ctx.file->objects[static_cast<std::size_t>(ctx.index)];
        const std::int64_t layer =
            std::clamp<std::int64_t>(go.body.integer_at("m_Layer", 0), 0, 31);
        if (json::Value layers = render_layers(layer, ctx.where); layers.as_int() != 1) {
            hterrain["render_layers"] = std::move(layers);
        }
        const Component* collider = parts.terrain_collider;
        if (collider != nullptr && collider->body.flag_at("m_Enabled", true)) {
            if (const auto own = ref_at(collider->body, "m_TerrainData");
                own && (own->guid != data.guid || own->file_id != data.file_id)) {
                problems_.add("TerrainCollider of another TerrainData", ctx.where,
                              "the HTerrain collides where it draws");
            }
            const std::uint32_t layer_bit = 1U << static_cast<std::uint32_t>(layer);
            if (layer_bit != 1U) {
                hterrain["collision_layer"] = std::int64_t{layer_bit};
            }
            if (const std::uint32_t mask = layer_masks_3d_.at(static_cast<std::size_t>(layer));
                mask != 1U) {
                hterrain["collision_mask"] = std::int64_t{mask};
            }
            Material material = default_material_3d_;
            if (const auto ref = ref_at(collider->body, "m_Material"); ref && !ref->guid.empty()) {
                material = physics_material_3d(ref->guid);
            }
            hterrain["friction"] = material.friction;
            if (material.bounce != 0.0) {
                hterrain["bounce"] = material.bounce;
            }
            if (parts.rigidbody != nullptr) {
                problems_.add("TerrainCollider on a Rigidbody", ctx.where,
                              "an HTerrain collides as static ground");
            }
        } else {
            hterrain["collision_enabled"] = false;
        }
        json::Value transform;
        transform["position"] = json::Array{tidy_number(-converted.width), 0.0, 0.0};
        if (!terrain.flag_at("m_Enabled", true) || !terrain.flag_at("m_DrawHeightmap", true)) {
            transform["visible"] = false;
        }
        if (quaternion_euler(place.rotation) != std::array<double, 3>{} ||
            place.scale != std::array<double, 3>{1.0, 1.0, 1.0}) {
            problems_.add("Terrain on a turned or scaled object", ctx.where,
                          "Unity draws terrains unturned and unscaled; the HTerrain turns and "
                          "scales with its object");
        }
        if (const auto material = ref_at(terrain, "m_MaterialTemplate");
            material && !material->guid.empty() && asset_of(material->guid) != nullptr) {
            problems_.add("Terrain material", ctx.where,
                          "an HTerrain shades as HTerrain's Classic4 shader");
        }
        components["Transform3D"] = std::move(transform);
        components["HTerrain"] = std::move(hterrain);
        out.added.emplace_back("terrain", std::move(components));
    }

    // Unity's heights are 16-bit numbers from 0 to this, standing for 0 to the terrain's height.
    static constexpr double kMaxTerrainHeight = 32766.0;

    // The bytes a hex string stands for, as Unity writes arrays of numbers, or nullopt when it
    // isn't one.
    static std::optional<std::vector<std::uint8_t>> hex_bytes(std::string_view text) {
        std::vector<std::uint8_t> out;
        out.reserve(text.size() / 2);
        int high = -1;
        for (const char c : text) {
            int nibble = -1;
            if (c >= '0' && c <= '9') {
                nibble = c - '0';
            } else if (c >= 'a' && c <= 'f') {
                nibble = c - 'a' + 10;
            } else if (c >= 'A' && c <= 'F') {
                nibble = c - 'A' + 10;
            } else if (c == ' ' || c == '\n') {
                continue; // where a long line was wrapped
            } else {
                return std::nullopt;
            }
            if (high < 0) {
                high = nibble;
            } else {
                out.push_back(static_cast<std::uint8_t>((high << 4) | nibble));
                high = -1;
            }
        }
        if (high >= 0) {
            return std::nullopt;
        }
        return out;
    }

    // A grid of values a power of two plus one across, made one finer: values halfway between
    // two are their mean, and ones in the middle of four the mean of the four.
    template <typename T, typename Mix>
    static std::vector<T> refine_grid(const std::vector<T>& grid, int size, const Mix& mix) {
        const int finer = (size * 2) - 1;
        std::vector<T> out(static_cast<std::size_t>(finer) * static_cast<std::size_t>(finer));
        const auto at = [&](int x, int z) {
            return grid[(static_cast<std::size_t>(z) * static_cast<std::size_t>(size)) +
                        static_cast<std::size_t>(x)];
        };
        for (int z = 0; z < finer; ++z) {
            for (int x = 0; x < finer; ++x) {
                const int x0 = x / 2;
                const int z0 = z / 2;
                const int x1 = std::min(x0 + (x % 2), size - 1);
                const int z1 = std::min(z0 + (z % 2), size - 1);
                out[(static_cast<std::size_t>(z) * static_cast<std::size_t>(finer)) +
                    static_cast<std::size_t>(x)] =
                    mix(at(x0, z0), at(x1, z0), at(x0, z1), at(x1, z1));
            }
        }
        return out;
    }

    // The fields a TerrainData asset gives an HTerrain, its maps written beside it.
    std::optional<TerrainFields> terrain_data_fields(const Asset& asset, std::int64_t file_id) {
        const std::string& where = asset.path;
        auto text = platform::read_file(source_ + asset.path);
        auto objects =
            text ? unity::parse_file(text.value()) : Result<std::vector<Object>>(text.error());
        if (!objects) {
            problems_.add("unreadable file", where, objects.error().message);
            return std::nullopt;
        }
        const Object* data = nullptr;
        std::map<std::int64_t, const Object*> by_id;
        for (const Object& object : objects.value()) {
            by_id[object.file_id] = &object;
            if (object.type == "TerrainData" && (data == nullptr || object.file_id == file_id)) {
                data = &object;
            }
        }
        if (data == nullptr) {
            problems_.add("Terrain", where, "the asset holds no TerrainData");
            return std::nullopt;
        }
        const Node& body = data->body;
        const Node* map = body.get("m_Heightmap");
        const int unity_size =
            map != nullptr ? static_cast<int>(map->integer_at("m_Resolution")) : 0;
        const auto bytes = map != nullptr ? hex_bytes(map->string_at("m_Heights")) : std::nullopt;
        const std::size_t count =
            static_cast<std::size_t>(unity_size) * static_cast<std::size_t>(unity_size);
        if (unity_size < 2 || !bytes || bytes->size() != count * 2) {
            problems_.add("unreadable file", where,
                          "its heightmap isn't m_Resolution by m_Resolution 16-bit heights");
            return std::nullopt;
        }
        const double height = map->number_at("m_Scale.y", 600.0);
        // Rows run along +z, as the HTerrain's do; columns are flipped, as x mirrors.
        std::vector<float> heights(count);
        for (int z = 0; z < unity_size; ++z) {
            for (int x = 0; x < unity_size; ++x) {
                const std::size_t from =
                    ((static_cast<std::size_t>(z) * static_cast<std::size_t>(unity_size)) +
                     static_cast<std::size_t>(unity_size - 1 - x)) *
                    2;
                const auto value =
                    static_cast<std::int16_t>(static_cast<std::uint16_t>((*bytes)[from]) |
                                              static_cast<std::uint16_t>((*bytes)[from + 1] << 8U));
                heights[(static_cast<std::size_t>(z) * static_cast<std::size_t>(unity_size)) +
                        static_cast<std::size_t>(x)] =
                    static_cast<float>(
                        std::clamp(static_cast<double>(value), 0.0, kMaxTerrainHeight) /
                        kMaxTerrainHeight * height);
            }
        }
        // Holes: Unity's are cells, an HTerrain's heights. A height is a hole where every cell
        // around it is one.
        std::vector<std::uint8_t> holes;
        if (const auto cells = hex_bytes(map->string_at("m_Holes"));
            cells && cells->size() == static_cast<std::size_t>(unity_size - 1) *
                                          static_cast<std::size_t>(unity_size - 1)) {
            const int cell_size = unity_size - 1;
            const auto solid = [&](int x, int z) {
                return x < 0 || z < 0 || x >= cell_size || z >= cell_size ||
                       (*cells)[(static_cast<std::size_t>(z) *
                                 static_cast<std::size_t>(cell_size)) +
                                static_cast<std::size_t>(x)] != 0;
            };
            bool any = false;
            holes.assign(count, 0);
            for (int z = 0; z < unity_size; ++z) {
                for (int x = 0; x < unity_size; ++x) {
                    bool hole = true;
                    bool touched = false;
                    for (int dz = -1; dz <= 0; ++dz) {
                        for (int dx = -1; dx <= 0; ++dx) {
                            const int cx = x + dx;
                            const int cz = z + dz;
                            if (cx < 0 || cz < 0 || cx >= cell_size || cz >= cell_size) {
                                continue;
                            }
                            touched = true;
                            hole = hole && !solid(cx, cz);
                        }
                    }
                    if (hole && touched) {
                        holes[(static_cast<std::size_t>(z) * static_cast<std::size_t>(unity_size)) +
                              static_cast<std::size_t>(unity_size - 1 - x)] = 1;
                        any = true;
                    }
                }
            }
            bool cut = false;
            for (int i = 0; i < cell_size * cell_size && !cut; ++i) {
                cut = (*cells)[static_cast<std::size_t>(i)] == 0;
            }
            if (cut) {
                problems_.add("Terrain holes", where,
                              "a height is a hole where every cell around it is one, so holes "
                              "come in half a cell at their edges and one a cell across closes");
            }
            if (!any) {
                holes.clear();
            }
        }
        // An HTerrain is 65 to 4097 heights across; Unity's smallest, 33, is made one finer.
        int size = unity_size;
        double cell = 1.0;
        while (size < 65) {
            heights = refine_grid(heights, size, [](float a, float b, float c, float d) {
                return (a + b + c + d) / 4.0F;
            });
            if (!holes.empty()) {
                holes =
                    refine_grid(holes, size,
                                [](std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d) {
                                    return static_cast<std::uint8_t>(a & b & c & d);
                                });
            }
            size = (size * 2) - 1;
            cell /= 2.0;
        }
        if (size != unity_size) {
            problems_.add("Terrain heightmap resolution", where,
                          "an HTerrain is 65 to 4097 heights across; this one's " +
                              std::to_string(unity_size) + " are made " + std::to_string(size));
        }
        TerrainFields out;
        json::Value& fields = out.fields;
        const std::string stem = destination(without_extension(asset.path));
        float low = std::numeric_limits<float>::max();
        float high = std::numeric_limits<float>::lowest();
        for (const float h : heights) {
            low = std::min(low, h);
            high = std::max(high, h);
        }
        Vec2 range{std::floor(low), std::ceil(high)};
        if (range.y <= range.x) {
            range.y = range.x + 1.0F;
        }
        auto png = render::encode_gray16_png(scene3d::encode_heights(heights, size, range));
        if (!png) {
            problems_.add("unreadable file", where, png.error().message);
            return std::nullopt;
        }
        add_file(stem + std::string(scene3d::kHeightmapSuffix), std::move(png).value());
        fields["heightmap"] = stem + std::string(scene3d::kHeightmapSuffix);
        fields["resolution"] = size;
        fields["height_range"] =
            json::Array{static_cast<double>(range.x), static_cast<double>(range.y)};
        const double cell_x = map->number_at("m_Scale.x", 1.0) * cell;
        const double cell_z = map->number_at("m_Scale.z", 1.0) * cell;
        fields["map_scale"] = json::Array{tidy_number(cell_x), 1.0, tidy_number(cell_z)};
        out.width = cell_x * (size - 1);
        if (!holes.empty()) {
            render::Image color{
                .width = static_cast<std::uint32_t>(size),
                .height = static_cast<std::uint32_t>(size),
                .pixels = std::vector<std::uint8_t>(
                    static_cast<std::size_t>(size) * static_cast<std::size_t>(size) * 4, 255),
                .block_format = render::TextureFormat::Rgba8,
                .blocks = {}};
            for (std::size_t i = 0; i < holes.size(); ++i) {
                if (holes[i] != 0) {
                    color.pixels[(i * 4) + 3] = 0;
                }
            }
            if (auto written = render::encode_png(color)) {
                add_file(stem + ".color.png", std::move(written).value());
                fields["colormap"] = stem + ".color.png";
            }
        }
        terrain_layers(*data, by_id, cell_x, size, stem, where, fields);
        if (const Node* details = body.get("m_DetailDatabase")) {
            const Node* trees = details->get("m_TreeInstances");
            if (trees != nullptr && !trees->items.empty()) {
                problems_.add("Terrain trees", where,
                              "trees on terrains don't convert; place them as model instances");
            }
            const Node* grass = details->get("m_DetailPrototypes");
            if (grass != nullptr && !grass->items.empty()) {
                problems_.add("Terrain details", where,
                              "grass and other details on terrains don't convert");
            }
        }
        return out;
    }

    // A TerrainData's first four terrain layers as the HTerrain's textures, and its first alpha
    // map, their weights, as its splat map.
    void terrain_layers(const Object& data, const std::map<std::int64_t, const Object*>& by_id,
                        double cell, int size, const std::string& stem, const std::string& where,
                        json::Value& fields) {
        const Node* splats = data.body.get("m_SplatDatabase");
        if (splats == nullptr) {
            return;
        }
        if (const Node* old = splats->get("m_Splats"); old != nullptr && !old->items.empty()) {
            problems_.add("Terrain splat prototypes", where,
                          "Unity 2018.3 made them terrain layers; open the project in it to "
                          "convert them, then import again");
        }
        const Node* layers = splats->get("m_TerrainLayers");
        json::Array textures;
        for (const Node& item : layers != nullptr ? layers->items : std::vector<Node>{}) {
            if (textures.size() == 4) {
                problems_.add("Terrain layers past the fourth", where,
                              "an HTerrain mixes 4 textures, as HTerrain's Classic4 shader; the "
                              "others are left out");
                break;
            }
            const auto ref = unity::object_ref(item);
            textures.push_back(ref && !ref->guid.empty() ? terrain_layer(ref->guid, cell)
                                                         : json::Value(json::Object{}));
        }
        if (textures.empty()) {
            return;
        }
        const std::size_t layer_count = textures.size();
        fields["textures"] = std::move(textures);
        // Unity weighs layers by their alpha maps; there is no bump in its textures to blend by.
        fields["depth_blending"] = false;
        if (layer_count < 2) {
            return; // the first texture covers the terrain
        }
        const Node* alphas = splats->get("m_AlphaTextures");
        const auto first = alphas != nullptr && !alphas->items.empty()
                               ? unity::object_ref(alphas->items.front())
                               : std::nullopt;
        const auto found = first ? by_id.find(first->file_id) : by_id.end();
        if (found == by_id.end() || found->second->type != "Texture2D") {
            problems_.add("Terrain alpha map", where,
                          "it has no alpha map in its asset, so the first texture covers it");
            return;
        }
        const Node& texture = found->second->body;
        const auto width = static_cast<std::size_t>(texture.integer_at("m_Width"));
        const auto height = static_cast<std::size_t>(texture.integer_at("m_Height"));
        const std::int64_t format = texture.integer_at("m_TextureFormat");
        const auto texels = hex_bytes(texture.string_at("_typelessdata"));
        // RGBA32 (4), ARGB32 (5) and BGRA32 (14), each a byte a channel.
        static constexpr std::array<std::array<std::size_t, 4>, 3> kOrders{
            {{0, 1, 2, 3}, {1, 2, 3, 0}, {2, 1, 0, 3}}};
        std::size_t order = 0;
        if (format == 5) {
            order = 1;
        } else if (format == 14) {
            order = 2;
        }
        if ((format != 4 && format != 5 && format != 14) || width == 0 || height == 0 || !texels ||
            texels->size() < width * height * 4) {
            problems_.add("Terrain alpha map", where,
                          "it isn't an uncompressed texture of 8-bit channels, so the first "
                          "texture covers the terrain");
            return;
        }
        // Each height takes the weights where it is on the alpha map, whose texels sit at
        // their centers and whose rows run along +z from the bottom, as the heights' do;
        // columns are flipped, as x mirrors.
        const auto texel = [&](std::size_t x, std::size_t z, std::size_t channel) {
            return static_cast<double>(
                (*texels)[(((z * width) + x) * 4) + kOrders.at(order).at(channel)]);
        };
        render::Image splat{.width = static_cast<std::uint32_t>(size),
                            .height = static_cast<std::uint32_t>(size),
                            .pixels = {},
                            .block_format = render::TextureFormat::Rgba8,
                            .blocks = {}};
        splat.pixels.resize(static_cast<std::size_t>(size) * static_cast<std::size_t>(size) * 4);
        for (int z = 0; z < size; ++z) {
            for (int x = 0; x < size; ++x) {
                const double u = static_cast<double>(size - 1 - x) / (size - 1);
                const double v = static_cast<double>(z) / (size - 1);
                const double fx = std::clamp((u * static_cast<double>(width)) - 0.5, 0.0,
                                             static_cast<double>(width - 1));
                const double fz = std::clamp((v * static_cast<double>(height)) - 0.5, 0.0,
                                             static_cast<double>(height - 1));
                const auto x0 = static_cast<std::size_t>(fx);
                const auto z0 = static_cast<std::size_t>(fz);
                const std::size_t x1 = std::min(x0 + 1, width - 1);
                const std::size_t z1 = std::min(z0 + 1, height - 1);
                const double tx = fx - static_cast<double>(x0);
                const double tz = fz - static_cast<double>(z0);
                for (std::size_t channel = 0; channel < 4; ++channel) {
                    const double top =
                        std::lerp(texel(x0, z0, channel), texel(x1, z0, channel), tx);
                    const double bottom =
                        std::lerp(texel(x0, z1, channel), texel(x1, z1, channel), tx);
                    splat.pixels[((static_cast<std::size_t>(z) * static_cast<std::size_t>(size) +
                                   static_cast<std::size_t>(x)) *
                                  4) +
                                 channel] =
                        static_cast<std::uint8_t>(std::lround(std::lerp(top, bottom, tz)));
                }
            }
        }
        auto png = render::encode_png(splat);
        if (!png) {
            problems_.add("unreadable file", where, png.error().message);
            return;
        }
        add_file(stem + ".splat.png", std::move(png).value());
        fields["splatmap"] = stem + ".splat.png";
    }

    // A TerrainLayer as an HTerrain texture: its diffuse texture, its normal map and how many
    // cells one repeat of them covers.
    json::Value terrain_layer(const std::string& guid, double cell) {
        json::Value out = json::Object{};
        const Asset* asset = asset_of(guid);
        auto text = asset != nullptr ? platform::read_file(source_ + asset->path)
                                     : Result<std::string>(Error{.code = ErrorCode::NotFound,
                                                                 .message = "not in the project"});
        auto objects =
            text ? unity::parse_file(text.value()) : Result<std::vector<Object>>(text.error());
        const Object* layer = nullptr;
        if (objects) {
            for (const Object& object : objects.value()) {
                if (object.type == "TerrainLayer") {
                    layer = &object;
                }
            }
        }
        const std::string where = asset != nullptr ? asset->path : "TerrainLayer " + guid;
        if (layer == nullptr) {
            problems_.add("Terrain layer", where,
                          "not a terrain layer in the project's Assets folder; its texture is "
                          "white");
            return out;
        }
        const Node& body = layer->body;
        const auto texture = [&](std::string_view key) -> std::string {
            const auto ref = ref_at(body, key);
            if (!ref || ref->null()) {
                return {};
            }
            const Asset* file = asset_of(ref->guid);
            if (file == nullptr || !is_texture_file(extension_of(file->path))) {
                problems_.add("Terrain layer texture", where + ": " + std::string(key),
                              file == nullptr ? "a texture from outside the project's Assets folder"
                                              : file->path + ": not a texture");
                return {};
            }
            return destination(file->path);
        };
        if (std::string albedo = texture("m_DiffuseTexture"); !albedo.empty()) {
            out["albedo_bump"] = std::move(albedo);
        }
        if (std::string normal = texture("m_NormalMapTexture"); !normal.empty()) {
            out["normal_roughness"] = std::move(normal);
        }
        const double tile_x = body.number_at("m_TileSize.x", 15.0);
        const double tile_y = body.number_at("m_TileSize.y", 15.0);
        out["uv_scale"] = tidy_number(tile_x / cell);
        if (std::abs(tile_x - tile_y) > 1e-9) {
            problems_.add("Terrain layer tile size", where,
                          "its textures repeat as often both ways, by its x");
        }
        if (body.number_at("m_TileOffset.x") != 0.0 || body.number_at("m_TileOffset.y") != 0.0) {
            problems_.add("Terrain layer tile offset", where, "its textures start at the corner");
        }
        const auto mask = ref_at(body, "m_MaskMapTexture");
        if (body.number_at("m_Smoothness") != 0.0 || body.number_at("m_Metallic") != 0.0 ||
            (mask && !mask->null()) || body.number_at("m_NormalScale", 1.0) != 1.0) {
            problems_.add("Terrain layer surface", where,
                          "an HTerrain's textures take roughness from their normal maps' alpha; "
                          "smoothness, metallic, mask maps and normal strength don't convert");
        }
        return out;
    }

    // --- Meshes picked out of model files ---

    // A model file, read as the engine reads it, for the meshes picked out of it; null, reported,
    // for one that doesn't load.
    const scene3d::Model* model_file(const Asset& asset, const std::string& where) {
        auto [it, inserted] = models_.try_emplace(asset.path);
        std::optional<scene3d::Model>& model = it->second;
        if (!inserted) {
            return model ? &*model : nullptr;
        }
        if (extension_of(asset.path) == ".blend") {
            problems_.add("mesh picked out of a .blend file", where,
                          "a .blend file's meshes are read through Blender as the game runs; "
                          "instance the model itself, or export it as FBX or glTF");
            return nullptr;
        }
        const scene3d::ModelReader read = [this](std::string_view path) {
            return platform::read_file(source_ + std::string(path));
        };
        auto bytes = read(asset.path);
        if (!bytes) {
            problems_.add("unreadable model", where, asset.path + ": " + bytes.error().message);
            return nullptr;
        }
        auto loaded = scene3d::load_model(bytes.value(), asset.path, read);
        if (!loaded) {
            problems_.add("model that doesn't load", where,
                          asset.path + ": " + loaded.error().message);
            return nullptr;
        }
        model = std::move(loaded).value();
        return &*model;
    }

    // The mesh of a model a file ID names: by the name the model's import settings give that ID,
    // else by Unity's ID for a mesh of each name the model has (the name of the node that places
    // it, then its own).
    static std::optional<std::size_t> model_mesh(const Asset& asset, const scene3d::Model& model,
                                                 std::int64_t file_id) {
        std::string named;
        if (const Node* table = asset.meta.path("ModelImporter.internalIDToNameTable")) {
            for (const Node& entry : table->items) {
                const Node* first = entry.get("first");
                if (const Node* id = first != nullptr ? first->get("43") : nullptr;
                    id != nullptr && id->integer() == file_id) {
                    named = entry.string_at("second");
                }
            }
        }
        if (const Node* table = asset.meta.path("ModelImporter.fileIDToRecycleName")) {
            for (const auto& [id, name] : table->entries) {
                if (parse_int(id) == file_id) {
                    named = name.string();
                }
            }
        }
        std::map<std::string, int, std::less<>> seen; // how many meshes had each name before
        for (std::size_t i = 0; i < model.meshes.size(); ++i) {
            std::vector<std::string> names;
            for (const scene3d::ModelPart& part : model.parts) {
                if (part.mesh == i) {
                    names.push_back(part.node);
                }
            }
            names.push_back(model.meshes[i].name);
            for (const std::string& name : names) {
                if (name.empty()) {
                    continue;
                }
                if (name == named || import_id("Mesh", name, seen[name]) == file_id) {
                    return i;
                }
            }
            for (const std::string& name : names) {
                ++seen[name];
            }
        }
        return std::nullopt;
    }

    // A name made safe for a file's: letters, digits, '-' and '_'.
    static std::string file_name_part(std::string_view name) {
        std::string out;
        for (const char c : name) {
            out += std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-' ? c : '_';
        }
        return out.empty() ? "mesh" : out;
    }

    struct PickedMesh {
        std::string path;                    // the glTF file written for it
        std::optional<json::Value> material; // the entity's StandardMaterial3D, for one material
    };

    // A mesh picked out of a model file, as a MeshFilter and its renderer's materials draw it:
    // written as a glTF file of that mesh alone, at the MeshInstance3D's origin and sized by the
    // model's import scale. One material for all its parts goes on the entity, keeping the file
    // for every user of the mesh; several are written into a file of their own. Unity draws only
    // as many of the mesh's parts as there are materials.
    std::optional<PickedMesh> picked_mesh(const Asset& asset, std::int64_t file_id,
                                          const Node* materials, const std::string& where) {
        const scene3d::Model* model = model_file(asset, where);
        if (model == nullptr) {
            return std::nullopt;
        }
        const std::optional<std::size_t> index = model_mesh(asset, *model, file_id);
        if (!index) {
            problems_.add("mesh from a model", where,
                          asset.path + " has no mesh with the ID " + std::to_string(file_id));
            return std::nullopt;
        }
        const scene3d::ModelMesh& mesh = model->meshes[*index];
        std::vector<std::optional<ObjectRef>> refs;
        if (materials != nullptr) {
            for (const Node& item : materials->items) {
                const auto ref = unity::object_ref(item);
                refs.push_back(ref && !ref->null() ? ref : std::nullopt);
            }
        }
        std::size_t kept = mesh.surfaces.size();
        if (!refs.empty()) {
            kept = std::min(kept, refs.size());
            if (refs.size() > mesh.surfaces.size()) {
                problems_.add("more materials than the mesh has parts", where,
                              "each part draws once, with its own material");
            }
        }
        // One material for every part drawn goes on the entity.
        PickedMesh picked;
        bool one = !refs.empty() && refs.front().has_value();
        for (std::size_t i = 1; i < kept && one; ++i) {
            one = refs[i].has_value() && refs[i]->guid == refs.front()->guid &&
                  refs[i]->file_id == refs.front()->file_id;
        }
        const json::Value* shared = one ? surface_material(*refs.front(), where) : nullptr;
        if (shared != nullptr) {
            picked.material = *shared;
        }
        // The file: the mesh's parts that draw, with the model's materials or the renderer's.
        scene3d::Model out;
        out.images = model->images;
        for (scene3d::ModelImage& image : out.images) {
            if (!image.path.empty()) {
                image.path = destination(image.path);
            }
        }
        std::string key = asset.path + "\n" + std::to_string(*index) + "\n" + std::to_string(kept);
        scene3d::ModelMesh part{.name = mesh.name, .surfaces = {}};
        std::map<int, int> own; // the model's materials, by their index there
        world::World scratch(components_);
        for (std::size_t i = 0; i < kept; ++i) {
            scene3d::ModelSurface surface = mesh.surfaces[i];
            const json::Value* given = shared == nullptr && i < refs.size() && refs[i]
                                           ? surface_material(*refs[i], where)
                                           : nullptr;
            if (given != nullptr) {
                scene3d::ModelMaterial material;
                const Asset* source = asset_of(refs[i]->guid);
                material.name = source != nullptr ? file_stem(source->path) : "Default-Material";
                std::string at;
                if (world::from_json(scratch, *given, material.standard, at)) {
                    surface.material = static_cast<int>(out.materials.size());
                    out.materials.push_back(std::move(material));
                    key += "\n" + refs[i]->guid;
                    part.surfaces.push_back(std::move(surface));
                    continue;
                }
            }
            if (surface.material >= 0) {
                const auto [found, added] =
                    own.try_emplace(surface.material, static_cast<int>(out.materials.size()));
                if (added) {
                    out.materials.push_back(
                        model->materials[static_cast<std::size_t>(surface.material)]);
                }
                surface.material = found->second;
            }
            key += "\n-";
            part.surfaces.push_back(std::move(surface));
        }
        if (const double scale = asset.meta.number_at("ModelImporter.meshes.globalScale", 1.0);
            scale != 1.0) {
            for (scene3d::ModelSurface& surface : part.surfaces) {
                for (render::Vertex3D& v : surface.vertices) {
                    for (float& c : v.position) {
                        c = static_cast<float>(c * scale);
                    }
                }
            }
        }
        const std::string name = !mesh.name.empty() ? mesh.name : "Mesh" + std::to_string(*index);
        out.meshes.push_back(std::move(part));
        out.parts.push_back({.node = name, .mesh = 0, .transform = {}, .node_index = -1});
        if (const auto it = picked_.find(key); it != picked_.end()) {
            picked.path = it->second;
            return picked;
        }
        const std::string extension = extension_of(asset.path);
        const std::string base =
            destination(asset.path.substr(0, asset.path.size() - extension.size())) + "_" +
            file_name_part(name);
        std::string path = base + ".gltf";
        for (int n = 2; picked_paths_.contains(path); ++n) {
            path = base + "_" + std::to_string(n) + ".gltf";
        }
        auto written = scene3d::write_gltf(out, path);
        if (!written) {
            problems_.add("mesh from a model", where, asset.path + ": " + written.error().message);
            return std::nullopt;
        }
        add_file(path, std::move(written).value());
        picked_paths_.insert(path);
        picked_.emplace(key, path);
        picked.path = path;
        return picked;
    }

    // The triangles of a mesh picked out of a model, sized by the model's import scale.
    std::optional<std::vector<std::array<float, 3>>>
    model_triangles(const Asset& asset, std::int64_t file_id, const std::string& where) {
        const scene3d::Model* model = model_file(asset, where);
        if (model == nullptr) {
            return std::nullopt;
        }
        const std::optional<std::size_t> index = model_mesh(asset, *model, file_id);
        if (!index) {
            problems_.add("mesh from a model", where,
                          asset.path + " has no mesh with the ID " + std::to_string(file_id));
            return std::nullopt;
        }
        const auto scale =
            static_cast<float>(asset.meta.number_at("ModelImporter.meshes.globalScale", 1.0));
        std::vector<std::array<float, 3>> corners;
        for (const scene3d::ModelSurface& surface : model->meshes[*index].surfaces) {
            for (const std::uint32_t i : surface.indices) {
                const std::array<float, 3>& p = surface.vertices[i].position;
                corners.push_back({p[0] * scale, p[1] * scale, p[2] * scale});
            }
        }
        return corners;
    }

    // A MeshFilter and its MeshRenderer, or a model instance: a MeshInstance3D with its mesh and
    // material.
    void mesh_3d(const Context& ctx, const Parts3D& parts, Converted& out, Turned& turned) {
        const GameObject& go = ctx.file->objects[static_cast<std::size_t>(ctx.index)];
        if (parts.model != nullptr) {
            model_3d(ctx, *parts.model, out);
        }
        if (parts.renderer == nullptr) {
            return; // a MeshFilter alone draws nothing
        }
        const Node& renderer = parts.renderer->body;
        if (lod_hidden_.contains(parts.renderer->id)) {
            problems_.add("LOD level past the first", ctx.where,
                          "only the most detailed level of a LOD group draws");
            return;
        }
        if (parts.filter == nullptr) {
            problems_.add("MeshRenderer without a MeshFilter", ctx.where, "it has no mesh to draw");
            return;
        }
        const auto ref = ref_at(parts.filter->body, "m_Mesh");
        if (!ref || ref->null()) {
            return;
        }
        json::Object mesh;
        bool quad = false;
        std::optional<PickedMesh> picked;
        if (ref->guid == kBuiltinExtraGuid) {
            switch (ref->file_id) {
            case kCubeMesh:
                mesh["BoxMesh"] = json::Object{};
                break;
            case kCylinderMesh:
                mesh["CylinderMesh"] = json::Object{};
                break;
            case kSphereMesh:
                mesh["SphereMesh"] = json::Object{};
                break;
            case kCapsuleMesh:
                mesh["CapsuleMesh"] = json::Object{};
                break;
            case kPlaneMesh:
                mesh["PlaneMesh"]["size"] = json::Array{10.0, 10.0};
                break;
            case kQuadMesh:
                mesh["QuadMesh"] = json::Object{};
                quad = true;
                break;
            default:
                problems_.add("built-in mesh", ctx.where,
                              "Unity's cube, sphere, capsule, cylinder, plane and quad convert");
                return;
            }
        } else {
            const Asset* asset = ref->guid.empty() ? nullptr : asset_of(ref->guid);
            if (ref->guid.empty()) {
                problems_.add("mesh made in the scene", ctx.where,
                              "meshes saved in a scene, as ProBuilder saves them, don't convert "
                              "yet");
            } else if (asset != nullptr && is_model_file(extension_of(asset->path))) {
                picked = picked_mesh(*asset, ref->file_id, renderer.get("m_Materials"), ctx.where);
            } else {
                problems_.add("mesh asset", ctx.where,
                              asset != nullptr
                                  ? asset->path + ": meshes saved as Unity assets don't convert"
                                  : "a mesh from outside the project's Assets folder");
            }
            if (!picked) {
                return;
            }
        }
        json::Value instance = json::Object{};
        if (picked) {
            instance["mesh"] = picked->path;
        }
        if (const std::int64_t layer = go.body.integer_at("m_Layer", 0); layer != 0) {
            instance["layers"] = render_layers(layer, ctx.where);
        }
        static constexpr std::array<std::string_view, 4> kShadows{"off", "on", "double_sided",
                                                                  "shadows_only"};
        if (const std::int64_t shadows = renderer.integer_at("m_CastShadows", 1);
            shadows != 1 && shadows >= 0 && shadows < 4) {
            instance["cast_shadow"] = std::string(kShadows.at(static_cast<std::size_t>(shadows)));
        }
        mesh["MeshInstance3D"] = std::move(instance);
        if (picked) {
            if (picked->material) {
                mesh["StandardMaterial3D"] = *std::move(picked->material);
            }
        } else if (const Node* materials = renderer.get("m_Materials");
                   materials != nullptr && !materials->items.empty()) {
            if (const auto material = unity::object_ref(materials->items.front());
                material && !material->null()) {
                if (const json::Value* surface = surface_material(*material, ctx.where)) {
                    mesh["StandardMaterial3D"] = *surface;
                }
            }
            if (materials->items.size() > 1) {
                problems_.add("more materials than the mesh has parts", ctx.where,
                              "the mesh draws once, with its first material");
            }
        }
        if (!renderer.flag_at("m_ReceiveShadows", true)) {
            problems_.add("renderer that receives no shadows", ctx.where, "it receives them");
        }
        if (!renderer.flag_at("m_Enabled", true)) {
            // Hidden on an entity of its own, which gameplay code can show.
            mesh["Transform3D"]["visible"] = false;
            if (quad) {
                mesh["Transform3D"]["rotation"] = half_turn();
            }
            out.added.emplace_back("mesh", std::move(mesh));
            return;
        }
        if (quad) {
            turned.emplace_back("mesh", std::move(mesh));
            return;
        }
        for (auto& [name, fields] : mesh) {
            out.components[name] = std::move(fields);
        }
    }

    // A model instance: a MeshInstance3D drawing the model file whole, with a material in place of
    // its own when the instance or the model's import settings put one in place of all of them.
    void model_3d(const Context& ctx, const Component& model, Converted& out) {
        const GameObject& go = ctx.file->objects[static_cast<std::size_t>(ctx.index)];
        const Asset* asset = asset_of(model.body.string_at("guid"));
        json::Object parts;
        json::Value instance;
        instance["mesh"] = destination(model.body.string_at("path"));
        if (const std::int64_t layer = go.body.integer_at("m_Layer", 0); layer != 0) {
            instance["layers"] = render_layers(layer, ctx.where);
        }
        parts["MeshInstance3D"] = std::move(instance);
        std::set<std::pair<std::string, std::int64_t>> named;
        std::optional<ObjectRef> material;
        if (const Node* materials = model.body.get("m_Materials")) {
            for (const Node& item : materials->items) {
                if (const auto ref = unity::object_ref(item); ref && !ref->null()) {
                    named.emplace(ref->guid, ref->file_id);
                    material = ref;
                }
            }
        }
        if (named.size() == 1 && material) {
            if (const json::Value* surface = surface_material(*material, ctx.where)) {
                parts["StandardMaterial3D"] = *surface;
            }
        } else if (named.size() > 1) {
            problems_.add("several materials in place of a model's own", ctx.where,
                          "the model draws with its own materials");
        }
        const Node empty;
        const Node& meta = asset != nullptr ? asset->meta : empty;
        if (!meta.flag_at("ModelImporter.meshes.useFileScale", true)) {
            problems_.add("model imported without its file's units", ctx.where,
                          "it is sized as its file's units say");
        }
        // Its nodes as the scene poses them.
        const scene3d::Model* loaded = asset != nullptr && extension_of(asset->path) != ".blend"
                                           ? model_file(*asset, ctx.where)
                                           : nullptr;
        if (loaded != nullptr) {
            ModelPose pose = model_pose(*ctx.file, ctx.index, *asset, *loaded, ctx.where, true);
            if (!pose.json.as_object().empty()) {
                parts["Skeleton3D"] = std::move(pose.json);
            }
        }
        // The import's scale sizes the model, not the objects under its instance.
        if (const double scale = meta.number_at("ModelImporter.meshes.globalScale", 1.0);
            scale != 1.0) {
            parts["Transform3D"]["scale"] = triple({scale, scale, scale});
            out.added.emplace_back("model", std::move(parts));
            return;
        }
        for (auto& [name, fields] : parts) {
            out.components[name] = std::move(fields);
        }
    }

    // How a scene poses a model's nodes: each whose object is placed other than the model has
    // it, and a top node Unity made the model's top object, whose place the object has. As
    // Skeleton3D's fields and as the component itself.
    struct ModelPose {
        json::Value json = json::Object{}; // NOLINT(readability-redundant-member-init)
        world::Skeleton3D skeleton;
    };

    ModelPose model_pose(const Expanded& file, int index, const Asset& asset,
                         const scene3d::Model& model, const std::string& where, bool report) {
        ModelPose out;
        const double scale = asset.meta.number_at("ModelImporter.meshes.globalScale", 1.0);
        const auto set = [&](const scene3d::ModelNode& node, const Place3D& place) {
            const std::array<double, 3> position{
                place.position[0] / scale, place.position[1] / scale, place.position[2] / scale};
            const auto differs = [](const std::array<double, 3>& a, const Vec3& b) {
                const std::array<double, 3> c{b.x, b.y, b.z};
                for (std::size_t i = 0; i < 3; ++i) {
                    if (std::abs(a.at(i) - c.at(i)) > 1e-5 * std::max(1.0, std::abs(c.at(i)))) {
                        return true;
                    }
                }
                return false;
            };
            const auto vec = [](const std::array<double, 3>& v) {
                return Vec3{static_cast<float>(v[0]), static_cast<float>(v[1]),
                            static_cast<float>(v[2])};
            };
            if (differs(position, node.position)) {
                out.json["bone_positions"][node.name] = triple(position);
                out.skeleton.bone_positions[node.name] = vec(position);
            }
            const Quat& q = node.rotation;
            if (angle_between(place.rotation, {q.x, q.y, q.z, q.w}) > 1e-4) {
                const std::array<double, 3> euler = quaternion_euler(place.rotation);
                out.json["bone_rotations"][node.name] = triple(euler);
                out.skeleton.bone_rotations[node.name] = vec(euler);
            }
            if (differs(place.scale, node.scale)) {
                out.json["bone_scales"][node.name] = triple(place.scale);
                out.skeleton.bone_scales[node.name] = vec(place.scale);
            }
        };
        if (const int top = collapsed_top(asset, model); top >= 0) {
            set(model.nodes[static_cast<std::size_t>(top)], Place3D{});
        }
        const std::function<void(int)> walk = [&](int at) {
            for (const int child : file.objects[static_cast<std::size_t>(at)].children) {
                const GameObject& c = file.objects[static_cast<std::size_t>(child)];
                if (c.removed || c.node.empty()) {
                    continue;
                }
                const int n = scene3d::find_node(model, c.node);
                const Component* transform = transform_of(c);
                if (n >= 0 && transform != nullptr) {
                    set(model.nodes[static_cast<std::size_t>(n)], place_of(transform->body));
                }
                const bool hidden =
                    !c.body.flag_at("m_IsActive", true) ||
                    std::ranges::any_of(c.components, [](const Component& component) {
                        return (component.class_id == kSkinnedMeshRenderer ||
                                component.class_id == kMeshRenderer) &&
                               !component.body.flag_at("m_Enabled", true);
                    });
                if (hidden && report) {
                    problems_.add("hidden node of a model",
                                  where + "/" + c.body.string_at("m_Name"),
                                  "it draws, since the model draws whole");
                }
                walk(child);
            }
        };
        walk(index);
        return out;
    }

    // Where a model's node an entity follows is posed, in the space of the entity that draws the
    // model.
    std::optional<Place3D> node_place(const Expanded& file, int index, const std::string& where) {
        int root = index;
        while (root >= 0 && !file.objects[static_cast<std::size_t>(root)].node.empty()) {
            root = file.objects[static_cast<std::size_t>(root)].parent;
        }
        const Asset* asset =
            root >= 0 ? model_of(file.objects[static_cast<std::size_t>(root)]) : nullptr;
        const scene3d::Model* model = asset != nullptr && extension_of(asset->path) != ".blend"
                                          ? model_file(*asset, where)
                                          : nullptr;
        if (model == nullptr) {
            return std::nullopt;
        }
        const int node =
            scene3d::find_node(*model, file.objects[static_cast<std::size_t>(index)].node);
        if (node < 0) {
            return std::nullopt;
        }
        std::vector<scene3d::NodePose> pose = scene3d::rest_pose(*model);
        scene3d::apply_skeleton(
            *model, model_pose(file, root, *asset, *model, where, false).skeleton, pose);
        const scene3d::NodePose at =
            scene3d::decompose(scene3d::node_globals(*model, pose)[static_cast<std::size_t>(node)]);
        Place3D place;
        place.position = {at.position.x, at.position.y, at.position.z};
        place.rotation = {at.rotation.x, at.rotation.y, at.rotation.z, at.rotation.w};
        place.scale = {at.scale.x, at.scale.y, at.scale.z};
        return place;
    }

    // Unity's Animator writes the nodes its clips move each frame, over the scene's pose, so a
    // Skeleton3D, which a model's animations don't move, leaves those parts of the pose out.
    void leave_out_animated(json::Object& drawing, const json::Value& player,
                            const std::string& where) {
        const auto skeleton = drawing.find("Skeleton3D");
        if (skeleton == drawing.end()) {
            return;
        }
        for (const auto& [node, property] : animated_nodes(player, where)) {
            const std::string field = "bone_" + property + "s";
            if (json::Value* bones = skeleton->second.find(field); bones != nullptr) {
                bones->as_object().erase(node);
                if (bones->as_object().empty()) {
                    skeleton->second.as_object().erase(field);
                }
            }
        }
        if (skeleton->second.as_object().empty()) {
            drawing.erase(skeleton);
        }
    }

    // The node properties ("position", "rotation", "scale") an AnimationPlayer's or
    // AnimationTree's libraries move: their node tracks', and those of the model animations they
    // play from.
    std::set<std::pair<std::string, std::string>> animated_nodes(const json::Value& player,
                                                                 const std::string& where) {
        std::vector<std::string> paths;
        if (const json::Value* library = player.find("library");
            library != nullptr && library->is_string()) {
            paths.push_back(library->as_string());
        }
        if (const json::Value* libraries = player.find("libraries");
            libraries != nullptr && libraries->is_object()) {
            for (const auto& [name, library] : libraries->as_object()) {
                if (library.is_string()) {
                    paths.push_back(library.as_string());
                }
            }
        }
        static constexpr std::array<std::string_view, 3> kProperties{"position", "rotation",
                                                                     "scale"};
        std::set<std::pair<std::string, std::string>> out;
        const world::FileReader reader = planned_files();
        for (const std::string& path : paths) {
            auto text = reader(path);
            auto parsed = text ? json::parse(text.value()) : Result<json::Value>(text.error());
            const json::Value* animations = parsed ? parsed.value().find("animations") : nullptr;
            if (animations == nullptr || !animations->is_object()) {
                continue;
            }
            for (const auto& [name, animation] : animations->as_object()) {
                const json::Value* tracks = animation.find("tracks");
                for (const json::Value& track :
                     tracks != nullptr && tracks->is_array() ? tracks->as_array() : json::Array{}) {
                    const json::Value* node = track.find("node");
                    const json::Value* property = track.find("property");
                    if (node != nullptr && node->is_string() && property != nullptr &&
                        property->is_string()) {
                        out.emplace(node->as_string(), property->as_string());
                    }
                }
                const json::Value* from = animation.find("from");
                const json::Value* model_path = from != nullptr ? from->find("model") : nullptr;
                const json::Value* take = from != nullptr ? from->find("animation") : nullptr;
                if (model_path == nullptr || !model_path->is_string() || take == nullptr ||
                    !take->is_string()) {
                    continue;
                }
                const Asset* asset = model_at(model_path->as_string());
                const scene3d::Model* model =
                    asset != nullptr ? model_file(*asset, where) : nullptr;
                for (const scene3d::ModelAnimation& played :
                     model != nullptr ? model->animations
                                      : std::vector<scene3d::ModelAnimation>{}) {
                    if (played.name != take->as_string()) {
                        continue;
                    }
                    for (const scene3d::NodeTrack& track : played.tracks) {
                        out.emplace(model->nodes[track.node].name,
                                    kProperties.at(static_cast<std::size_t>(track.property)));
                    }
                }
            }
        }
        return out;
    }

    // The model file converted to a project path.
    [[nodiscard]] const Asset* model_at(const std::string& converted) const {
        for (const auto& [guid, asset] : assets_by_guid_) {
            if (is_model_file(extension_of(asset.path)) && destination(asset.path) == converted) {
                return &asset;
            }
        }
        return nullptr;
    }

    // The scene's background is its main camera's, else its first enabled camera's.
    struct SceneCamera {
        bool main = false;
        std::int64_t clear = 1; // Unity's clear flags: 1 skybox, 2 solid color, 3 depth, 4 nothing
        json::Value color;
    };

    json::Object camera_3d(const Component& component, const Context& ctx, const GameObject& go) {
        const Node& body = component.body;
        json::Value camera = json::Object{};
        if (body.flag_at("orthographic")) {
            camera["projection"] = "orthogonal";
            camera["size"] = tidy_number(2.0 * body.number_at("orthographic size", 5.0));
        } else {
            camera["fov"] = body.number_at("field of view", 60.0);
        }
        camera["near"] = body.number_at("near clip plane", 0.3);
        camera["far"] = body.number_at("far clip plane", 1000.0);
        const bool enabled = body.flag_at("m_Enabled", true) && ctx.active;
        const bool main = go.body.string_at("m_TagString") == "MainCamera";
        if (main && enabled) {
            camera["current"] = true;
        }
        if (const auto mask =
                static_cast<std::uint32_t>(body.integer_at("m_CullingMask.m_Bits", 0xFFFFFFFF)) &
                0xFFFFFU;
            mask != 0xFFFFFU) {
            camera["cull_mask"] = std::int64_t{mask};
        }
        camera_problems(body, ctx);
        if (enabled && (!scene_camera_ || (main && !scene_camera_->main))) {
            const Node* color = body.get("m_BackGroundColor");
            scene_camera_ =
                SceneCamera{.main = main,
                            .clear = body.integer_at("m_ClearFlags", 1),
                            .color = color != nullptr
                                         ? json::Array{color->number_at("r"), color->number_at("g"),
                                                       color->number_at("b"), 1.0}
                                         : json::Array{0.0, 0.0, 0.0, 1.0}};
        }
        json::Object out;
        out["Camera3D"] = std::move(camera);
        return out;
    }

    void camera_problems(const Node& body, const Context& ctx) {
        const Node* rect = body.get("m_NormalizedViewPortRect");
        if (rect != nullptr &&
            (rect->number_at("x") != 0.0 || rect->number_at("y") != 0.0 ||
             rect->number_at("width", 1.0) != 1.0 || rect->number_at("height", 1.0) != 1.0)) {
            problems_.add("camera viewport rect", ctx.where, "every camera fills the window");
        }
        if (const auto target = ref_at(body, "m_TargetTexture"); target && !target->null()) {
            problems_.add("camera render texture", ctx.where,
                          "render textures aren't supported yet");
        }
    }

    void light_3d(const Component& component, const Context& ctx, Converted& out, Turned& turned) {
        const Node& body = component.body;
        const std::int64_t type = body.integer_at("m_Type", 2);
        json::Value light = json::Object{};
        std::string kind;
        switch (type) {
        case 0:
            kind = "SpotLight3D";
            light["spot_range"] = body.number_at("m_Range", 10.0);
            light["spot_angle"] = tidy_number(body.number_at("m_SpotAngle", 30.0) / 2.0);
            break;
        case 1:
            kind = "DirectionalLight3D";
            break;
        case 2:
            kind = "OmniLight3D";
            light["omni_range"] = body.number_at("m_Range", 10.0);
            break;
        default:
            problems_.add("area light", ctx.where,
                          "area lights are baked in Unity; there are none");
            return;
        }
        if (const Node* color = body.get("m_Color")) {
            light["light_color"] =
                json::Array{color->number_at("r", 1.0), color->number_at("g", 1.0),
                            color->number_at("b", 1.0), 1.0};
        }
        light["light_energy"] = body.number_at("m_Intensity", 1.0);
        if (body.integer_at("m_Shadows.m_Type", 0) != 0) {
            light["shadow_enabled"] = true;
            light["shadow_opacity"] = body.number_at("m_Shadows.m_Strength", 1.0);
        }
        if (const auto mask =
                static_cast<std::uint32_t>(body.integer_at("m_CullingMask.m_Bits", 0xFFFFFFFF));
            mask != 0xFFFFFFFFU) {
            light["light_cull_mask"] = std::int64_t{mask};
        }
        if (body.integer_at("m_Lightmapping", 4) == 2) {
            problems_.add("baked light", ctx.where,
                          "it lights the scene as it runs, as every light here does");
        }
        if (body.flag_at("m_UseColorTemperature")) {
            problems_.add("light color temperature", ctx.where, "the light has its color alone");
        }
        if (const auto cookie = ref_at(body, "m_Cookie"); cookie && !cookie->null()) {
            problems_.add("light cookie", ctx.where, "lights have no cookies");
        }
        json::Object parts;
        parts[kind] = std::move(light);
        const bool turns = type != 2; // a point light shines all round
        if (!body.flag_at("m_Enabled", true)) {
            parts["Transform3D"]["visible"] = false;
            if (turns) {
                parts["Transform3D"]["rotation"] = half_turn();
            }
            out.added.emplace_back("light", std::move(parts));
        } else if (turns) {
            turned.emplace_back("light", std::move(parts));
        } else {
            for (auto& [name, fields] : parts) {
                out.components[name] = std::move(fields);
            }
        }
    }

    // --- 3D physics ---

    // A collider's shape, or nullopt for one that doesn't convert.
    std::optional<json::Value> collider_shape(const Component& collider, const Context& ctx) {
        const Node& body = collider.body;
        json::Value shape = json::Object{};
        const auto center = [&] {
            const std::array<double, 3> at{-body.number_at("m_Center.x"),
                                           body.number_at("m_Center.y"),
                                           body.number_at("m_Center.z")};
            if (tidy_number(at[0]) != 0.0 || tidy_number(at[1]) != 0.0 ||
                tidy_number(at[2]) != 0.0) {
                shape["position"] = triple(at);
            }
        };
        const auto capsule = [&](double radius, double height, std::int64_t direction) {
            shape["shape"] = "capsule";
            shape["radius"] = std::abs(radius);
            shape["height"] = std::max(std::abs(height), 2.0 * std::abs(radius));
            const double quarter = tidy_number(std::numbers::pi / 2.0);
            if (direction == 0) {
                shape["rotation"] = json::Array{0.0, 0.0, quarter};
            } else if (direction == 2) {
                shape["rotation"] = json::Array{quarter, 0.0, 0.0};
            }
        };
        switch (collider.class_id) {
        case kBoxCollider:
            shape["size"] = triple({std::abs(body.number_at("m_Size.x", 1.0)),
                                    std::abs(body.number_at("m_Size.y", 1.0)),
                                    std::abs(body.number_at("m_Size.z", 1.0))});
            center();
            break;
        case kSphereCollider:
            shape["shape"] = "sphere";
            shape["radius"] = std::abs(body.number_at("m_Radius", 0.5));
            center();
            break;
        case kCapsuleCollider:
            capsule(body.number_at("m_Radius", 0.5), body.number_at("m_Height", 2.0),
                    body.integer_at("m_Direction", 1));
            center();
            break;
        default: { // a MeshCollider: Unity's built-in meshes become shapes, a model's its faces
            const auto mesh = ref_at(body, "m_Mesh");
            const Asset* asset = mesh && !mesh->guid.empty() ? asset_of(mesh->guid) : nullptr;
            if (mesh && asset != nullptr && is_model_file(extension_of(asset->path))) {
                const auto corners = model_triangles(*asset, mesh->file_id, ctx.where);
                if (!corners) {
                    return std::nullopt;
                }
                // Convex, its points' hull, as Unity's convex colliders; else its triangles.
                const bool convex = body.flag_at("m_Convex");
                std::set<std::array<double, 3>> seen;
                json::Array points;
                for (const std::array<float, 3>& p : *corners) {
                    const std::array<double, 3> at{tidy_number(p[0]), tidy_number(p[1]),
                                                   tidy_number(p[2])};
                    if (!convex || seen.insert(at).second) {
                        points.emplace_back(json::Array{at[0], at[1], at[2]});
                    }
                }
                shape["shape"] = convex ? "convex_polygon" : "concave_polygon";
                shape[convex ? "points" : "faces"] = std::move(points);
                break;
            }
            if (!mesh || mesh->guid != kBuiltinExtraGuid) {
                problems_.add("MeshCollider of a saved mesh", ctx.where,
                              "mesh colliders of Unity's built-in meshes and of models' meshes "
                              "convert; meshes saved as Unity assets don't");
                return std::nullopt;
            }
            // A plane and a quad are one-sided in Unity; here both their sides collide.
            const auto faces = [&](double half, bool upright) {
                const auto corner = [&](double a, double b) {
                    return upright ? json::Array{a, b, 0.0} : json::Array{a, 0.0, b};
                };
                shape["shape"] = "concave_polygon";
                shape["faces"] =
                    json::Array{corner(-half, -half), corner(half, -half), corner(half, half),
                                corner(-half, -half), corner(half, half),  corner(-half, half)};
                shape["backface_collision"] = true;
            };
            switch (mesh->file_id) {
            case kCubeMesh:
                break;
            case kSphereMesh:
                shape["shape"] = "sphere";
                shape["radius"] = 0.5;
                break;
            case kCapsuleMesh:
                capsule(0.5, 2.0, 1);
                break;
            case kCylinderMesh:
                shape["shape"] = "cylinder";
                break;
            case kPlaneMesh:
                faces(5.0, false);
                break;
            case kQuadMesh:
                faces(0.5, true);
                break;
            default:
                return std::nullopt;
            }
            break;
        }
        }
        if (!body.flag_at("m_Enabled", true) || !ctx.active) {
            shape["disabled"] = true;
        }
        return shape;
    }

    // A 3D physics material: dynamic friction and bounciness.
    Material physics_material_3d(const std::string& guid) {
        if (const auto it = materials_3d_.find(guid); it != materials_3d_.end()) {
            return it->second;
        }
        Material material = default_material_3d_;
        if (const Asset* asset = asset_of(guid); asset != nullptr) {
            if (auto text = platform::read_file(source_ + asset->path); text) {
                if (auto objects = unity::parse_file(text.value()); objects) {
                    for (const Object& object : objects.value()) {
                        if (object.type == "PhysicMaterial" || object.type == "PhysicsMaterial") {
                            material.friction = std::max(
                                0.0, object.body.number_at("dynamicFriction", kDefaultFriction3D));
                            material.bounce =
                                std::clamp(object.body.number_at("bounciness", 0.0), 0.0, 1.0);
                        }
                    }
                }
            }
        }
        materials_3d_[guid] = material;
        return material;
    }

    // The nearest object above that has a component of a class, and how many levels up it is.
    [[nodiscard]] static std::pair<int, int> above_with(const Expanded& file, int index,
                                                        int class_id) {
        int levels = 0;
        for (int at = file.objects[static_cast<std::size_t>(index)].parent; at >= 0;
             at = file.objects[static_cast<std::size_t>(at)].parent) {
            ++levels;
            for (const Component& component :
                 file.objects[static_cast<std::size_t>(at)].components) {
                if (component.class_id == class_id) {
                    return {at, levels};
                }
            }
            if (levels > 256) {
                break;
            }
        }
        return {-1, 0};
    }

    void physics_3d(const Context& ctx, const Parts3D& parts, Converted& out) {
        if (parts.rigidbody == nullptr && parts.controller == nullptr && parts.colliders.empty()) {
            return;
        }
        const GameObject& go = ctx.file->objects[static_cast<std::size_t>(ctx.index)];
        const std::int64_t layer =
            std::clamp<std::int64_t>(go.body.integer_at("m_Layer", 0), 0, 31);
        const std::uint32_t layer_bit = 1U << static_cast<std::uint32_t>(layer);
        const std::uint32_t mask = layer_masks_3d_.at(static_cast<std::size_t>(layer));
        std::vector<json::Value> solid;
        std::vector<json::Value> triggers;
        Material material = default_material_3d_;
        for (const Component* collider : parts.colliders) {
            auto shape = collider_shape(*collider, ctx);
            if (!shape) {
                continue;
            }
            (collider->body.flag_at("m_IsTrigger") ? triggers : solid).push_back(std::move(*shape));
            if (const auto ref = ref_at(collider->body, "m_Material"); ref && !ref->guid.empty()) {
                material = physics_material_3d(ref->guid);
            }
        }
        const auto body_fields = [&](json::Value& fields, bool with_material) {
            fields["collision_layer"] = std::int64_t{layer_bit};
            fields["collision_mask"] = std::int64_t{mask};
            if (with_material) {
                fields["friction"] = material.friction;
                fields["bounce"] = material.bounce;
            }
        };
        std::string body_type;
        if (parts.controller != nullptr) {
            // A CharacterController is a capsule moved by gameplay code: a CharacterBody3D.
            const Node& controller = parts.controller->body;
            body_type = "CharacterBody3D";
            json::Value fields;
            body_fields(fields, false);
            fields["floor_max_angle"] =
                tidy_number(controller.number_at("m_SlopeLimit", 45.0) * std::numbers::pi / 180.0);
            fields["safe_margin"] = controller.number_at("m_SkinWidth", 0.08);
            out.components[body_type] = std::move(fields);
            const double radius = std::abs(controller.number_at("m_Radius", 0.5));
            json::Value capsule;
            capsule["shape"] = "capsule";
            capsule["radius"] = radius;
            capsule["height"] =
                std::max(std::abs(controller.number_at("m_Height", 2.0)), 2.0 * radius);
            const std::array<double, 3> at{-controller.number_at("m_Center.x"),
                                           controller.number_at("m_Center.y"),
                                           controller.number_at("m_Center.z")};
            if (at != std::array<double, 3>{}) {
                capsule["position"] = triple(at);
            }
            solid.insert(solid.begin(), std::move(capsule));
            if (controller.number_at("m_StepOffset", 0.3) > 0.0) {
                problems_.add("CharacterController step offset", ctx.where,
                              "a CharacterBody3D doesn't climb steps on its own; gameplay code "
                              "can");
            }
            if (parts.rigidbody != nullptr) {
                problems_.add("Rigidbody beside a CharacterController", ctx.where,
                              "the object is a CharacterBody3D");
            }
        } else if (parts.rigidbody != nullptr) {
            const Node& rb = parts.rigidbody->body;
            body_type = "RigidBody3D";
            json::Value fields;
            body_fields(fields, true);
            fields["mass"] = rb.number_at("m_Mass", 1.0);
            if (!rb.flag_at("m_UseGravity", true)) {
                fields["gravity_scale"] = 0.0;
            }
            const Node* linear = rb.get("m_LinearDamping");
            const Node* angular = rb.get("m_AngularDamping");
            fields["linear_damp"] =
                linear != nullptr ? linear->number().value_or(0.0) : rb.number_at("m_Drag", 0.0);
            fields["angular_damp"] = angular != nullptr ? angular->number().value_or(0.05)
                                                        : rb.number_at("m_AngularDrag", 0.05);
            if (rb.flag_at("m_IsKinematic")) {
                fields["freeze"] = true;
                fields["freeze_mode"] = "kinematic";
            }
            const std::int64_t constraints = rb.integer_at("m_Constraints", 0);
            static constexpr std::array<std::pair<std::int64_t, std::string_view>, 6> kLocks{
                {{2, "axis_lock_linear_x"},
                 {4, "axis_lock_linear_y"},
                 {8, "axis_lock_linear_z"},
                 {16, "axis_lock_angular_x"},
                 {32, "axis_lock_angular_y"},
                 {64, "axis_lock_angular_z"}}};
            for (const auto& [bit, field] : kLocks) {
                if ((constraints & bit) != 0) {
                    fields[std::string(field)] = true;
                }
            }
            if (rb.integer_at("m_CollisionDetection", 0) != 0) {
                fields["continuous_cd"] = true;
            }
            out.components[body_type] = std::move(fields);
        } else if (!solid.empty()) {
            const auto [above, levels] = above_with(*ctx.file, ctx.index, kRigidbody);
            if (above < 0) {
                body_type = "StaticBody3D"; // a collider with no Rigidbody is static
            } else if (levels > 1) {
                body_type = "AnimatableBody3D";
                problems_.add("collider two or more levels under its Rigidbody", ctx.where,
                              "it becomes an AnimatableBody3D that follows its parent; bodies "
                              "take shapes from their own children only");
            }
            if (!body_type.empty()) {
                json::Value fields;
                body_fields(fields, true);
                out.components[body_type] = std::move(fields);
            }
        } else if (!triggers.empty()) {
            body_type = "Area3D";
            json::Value fields;
            body_fields(fields, false);
            out.components[body_type] = std::move(fields);
            solid = std::move(triggers);
            triggers.clear();
        }
        // One shape on the object, the rest on child entities.
        const bool has_body = !body_type.empty();
        int extra = 1;
        for (std::size_t i = 0; i < solid.size(); ++i) {
            if (i == 0) {
                out.components["CollisionShape3D"] = std::move(solid[i]);
                continue;
            }
            if (!has_body) {
                problems_.add("more colliders than an object without a Rigidbody can hold",
                              ctx.where,
                              "its parent's body takes one shape from it; the rest don't attach");
                continue;
            }
            json::Object added;
            added["Transform3D"] = json::Object{};
            added["CollisionShape3D"] = std::move(solid[i]);
            out.added.emplace_back("shape_" + std::to_string(++extra), std::move(added));
        }
        // Triggers on a body: an Area3D child entity, as Godot keeps areas apart from bodies.
        int trigger_count = 0;
        for (json::Value& shape : triggers) {
            json::Object added;
            added["Transform3D"] = json::Object{};
            json::Value area;
            body_fields(area, false);
            added["Area3D"] = std::move(area);
            added["CollisionShape3D"] = std::move(shape);
            ++trigger_count;
            out.added.emplace_back(trigger_count == 1 ? std::string("trigger")
                                                      : "trigger_" + std::to_string(trigger_count),
                                   std::move(added));
        }
    }

    // --- 3D materials ---

    // A .mat file's properties, by name.
    struct MaterialFile {
        std::string path;
        ObjectRef shader;
        std::map<std::string, Node> textures; // each with m_Texture, m_Scale and m_Offset
        std::map<std::string, double> numbers;
        std::map<std::string, Node> colors;
        std::set<std::string> keywords;

        [[nodiscard]] double number(const std::string& name, double fallback) const {
            const auto it = numbers.find(name);
            return it != numbers.end() ? it->second : fallback;
        }
        [[nodiscard]] std::optional<ObjectRef> texture(const std::string& name) const {
            const auto it = textures.find(name);
            if (it == textures.end()) {
                return std::nullopt;
            }
            auto ref = unity::object_ref_at(it->second, "m_Texture");
            return ref && !ref->null() ? ref : std::nullopt;
        }
    };

    std::optional<MaterialFile> read_material(const std::string& guid) {
        const Asset* asset = asset_of(guid);
        if (asset == nullptr || extension_of(asset->path) != ".mat") {
            return std::nullopt;
        }
        const auto text = platform::read_file(source_ + asset->path);
        const auto objects =
            text ? unity::parse_file(text.value()) : Result<std::vector<Object>>(text.error());
        if (!objects) {
            return std::nullopt;
        }
        for (const Object& object : objects.value()) {
            if (object.type != "Material") {
                continue;
            }
            MaterialFile file;
            file.path = asset->path;
            const Node& body = object.body;
            if (const auto shader = ref_at(body, "m_Shader")) {
                file.shader = *shader;
            }
            // Each list holds one-entry mappings: a property's name and its value.
            const auto each = [&](std::string_view list, const auto& take) {
                if (const Node* items = body.path(list)) {
                    for (const Node& item : items->items) {
                        for (const auto& [name, value] : item.entries) {
                            take(name, value);
                        }
                    }
                }
            };
            each("m_SavedProperties.m_TexEnvs",
                 [&](const std::string& name, const Node& value) { file.textures[name] = value; });
            const auto number = [&](const std::string& name, const Node& value) {
                file.numbers[name] = value.number().value_or(0.0);
            };
            each("m_SavedProperties.m_Floats", number);
            each("m_SavedProperties.m_Ints", number);
            each("m_SavedProperties.m_Colors",
                 [&](const std::string& name, const Node& value) { file.colors[name] = value; });
            // Kept in a local: the keywords are views into it.
            const std::string keywords = body.string_at("m_ShaderKeywords");
            for (const std::string_view keyword : split_path(keywords, ' ')) {
                if (!keyword.empty()) {
                    file.keywords.emplace(keyword);
                }
            }
            if (const Node* valid = body.get("m_ValidKeywords")) {
                for (const Node& keyword : valid->items) {
                    file.keywords.emplace(keyword.string());
                }
            }
            return file;
        }
        return std::nullopt;
    }

    // A material's StandardMaterial3D, converted once, or null when it doesn't convert.
    const json::Value* surface_material(const ObjectRef& ref, const std::string& where) {
        if (ref.guid == kBuiltinSprites) {
            if (ref.file_id == kDefaultMaterial) {
                return &default_surface_; // Standard, white, smoothness 0.5
            }
            problems_.add("built-in material", where,
                          "of Unity's materials, Default-Material converts");
            return nullptr;
        }
        auto [it, inserted] = surfaces_.try_emplace(ref.guid);
        std::optional<json::Value>& surface = it->second;
        if (inserted) {
            surface = convert_surface(ref.guid, where);
        }
        return surface ? &*surface : nullptr;
    }

    // A texture of a material, by its path here, or "" when it has none.
    std::string material_texture(const MaterialFile& file, const std::string& name) {
        const auto ref = file.texture(name);
        if (!ref) {
            return {};
        }
        const Asset* asset = asset_of(ref->guid);
        if (asset == nullptr || !is_texture_file(extension_of(asset->path))) {
            const std::string why =
                asset != nullptr ? unsupported_asset(extension_of(asset->path)) : "";
            problems_.add("material texture", file.path + ": " + name,
                          asset == nullptr
                              ? "a texture from outside the project's Assets folder"
                              : asset->path + ": " + (why.empty() ? "not a texture" : why));
            return {};
        }
        return destination(asset->path);
    }

    // Unity's Standard and URP Lit properties as a StandardMaterial3D's fields.
    std::optional<json::Value> convert_surface(const std::string& guid, const std::string& where) {
        auto read = read_material(guid);
        if (!read) {
            problems_.add("material", where,
                          "a material that isn't a readable .mat file in the project's Assets "
                          "folder");
            return std::nullopt;
        }
        const MaterialFile& file = *read;
        const std::string& at = file.path;
        const std::string& shader = file.shader.guid;
        const bool urp = shader == kUrpLit || shader == kUrpSimpleLit || shader == kUrpUnlit;
        const bool standard = shader == kBuiltinSprites;
        if (!urp && !standard) {
            problems_.add("material with a shader of its own", at,
                          "converted from the properties it shares with Unity's Standard "
                          "shader");
        }
        json::Value material = json::Object{};
        const auto color =
            file.colors.find(file.colors.contains("_BaseColor") ? "_BaseColor" : "_Color");
        if (color != file.colors.end()) {
            material["albedo_color"] = rgba(color->second);
        }
        const std::string base = file.textures.contains("_BaseMap") ? "_BaseMap" : "_MainTex";
        if (std::string texture = material_texture(file, base); !texture.empty()) {
            material["albedo_texture"] = std::move(texture);
            const Node& env = file.textures.at(base);
            const double sx = env.number_at("m_Scale.x", 1.0);
            const double sy = env.number_at("m_Scale.y", 1.0);
            const double ox = env.number_at("m_Offset.x");
            const double oy = env.number_at("m_Offset.y");
            if (sx != 1.0 || sy != 1.0) {
                material["uv1_scale"] = triple({sx, sy, 1.0});
            }
            // Unity's texture coordinates start at the bottom, Godot's at the top.
            if (const double y = 1.0 - sy - oy; ox != 0.0 || tidy_number(y) != 0.0) {
                material["uv1_offset"] = triple({ox, y, 0.0});
            }
            if (const auto ref = file.texture(base)) {
                texture_settings(ref->guid, at, material);
            }
        }
        material["metallic"] = file.number("_Metallic", 0.0);
        const double smoothness = file.number(urp ? "_Smoothness" : "_Glossiness", 0.5);
        material["roughness"] = tidy_number(1.0 - smoothness);
        if (std::string map = material_texture(file, "_MetallicGlossMap"); !map.empty()) {
            material["metallic_texture"] = std::move(map);
            material["metallic"] = 1.0;
            const double scale = urp ? smoothness : file.number("_GlossMapScale", 1.0);
            material["roughness"] = tidy_number(1.0 - scale);
            problems_.add("smoothness in a metallic map", at,
                          "roughness is 1 minus its smoothness scale over the whole surface");
        }
        if ((standard && file.shader.file_id == kSpecularShader) ||
            (urp && file.number("_WorkflowMode", 1.0) == 0.0)) {
            problems_.add("specular workflow material", at,
                          "converted as a metallic one, with metallic 0");
            material["metallic"] = 0.0;
        }
        if (std::string normal = material_texture(file, "_BumpMap"); !normal.empty()) {
            material["normal_enabled"] = true;
            material["normal_texture"] = std::move(normal);
            material["normal_scale"] = file.number("_BumpScale", 1.0);
        }
        if (file.keywords.contains("_EMISSION")) {
            // An HDR color: its brightest channel past 1 is its energy.
            material["emission_enabled"] = true;
            if (const auto emission = file.colors.find("_EmissionColor");
                emission != file.colors.end()) {
                std::array<double, 3> rgb{emission->second.number_at("r"),
                                          emission->second.number_at("g"),
                                          emission->second.number_at("b")};
                const double energy = std::max({rgb[0], rgb[1], rgb[2]});
                if (energy > 1.0) {
                    for (double& channel : rgb) {
                        channel /= energy;
                    }
                    material["emission_energy_multiplier"] = tidy_number(energy);
                }
                material["emission"] =
                    json::Array{tidy_number(rgb[0]), tidy_number(rgb[1]), tidy_number(rgb[2]), 1.0};
            }
            if (std::string map = material_texture(file, "_EmissionMap"); !map.empty()) {
                material["emission_texture"] = std::move(map);
            }
        }
        static constexpr std::array<std::pair<std::string_view, std::string_view>, 4> kMaps{
            {{"_OcclusionMap", "occlusion map"},
             {"_ParallaxMap", "height map"},
             {"_DetailAlbedoMap", "detail maps"},
             {"_DetailNormalMap", "detail maps"}}};
        for (const auto& [name, what] : kMaps) {
            if (file.texture(std::string(name))) {
                problems_.add("material " + std::string(what), at,
                              "StandardMaterial3D here has none; the material draws without it");
            }
        }
        if (urp) {
            if (file.number("_Surface", 0.0) == 1.0) {
                material["transparency"] = "alpha";
                const double blend = file.number("_Blend", 0.0);
                if (blend == 2.0) {
                    material["blend_mode"] = "add";
                } else if (blend == 3.0) {
                    material["blend_mode"] = "mul";
                }
            } else if (file.number("_AlphaClip", 0.0) == 1.0) {
                material["transparency"] = "alpha_scissor";
                material["alpha_scissor_threshold"] = file.number("_Cutoff", 0.5);
            }
            if (shader == kUrpUnlit) {
                material["shading_mode"] = "unshaded";
            }
        } else if (const double mode = file.number("_Mode", 0.0); mode == 1.0) {
            material["transparency"] = "alpha_scissor";
            material["alpha_scissor_threshold"] = file.number("_Cutoff", 0.5);
        } else if (mode == 2.0 || mode == 3.0) {
            material["transparency"] = "alpha";
        }
        if (const double cull = file.number("_Cull", 2.0); cull == 0.0) {
            material["cull_mode"] = "disabled";
        } else if (cull == 1.0) {
            material["cull_mode"] = "front";
        }
        return material;
    }

    // How a material's textures are filtered and repeat, from its main texture's import
    // settings.
    void texture_settings(const std::string& guid, const std::string& where,
                          json::Value& material) {
        const Asset* asset = asset_of(guid);
        const Node* importer = asset != nullptr ? asset->meta.get("TextureImporter") : nullptr;
        if (importer == nullptr) {
            return;
        }
        std::string filter =
            importer->integer_at("textureSettings.filterMode", 1) == 0 ? "nearest" : "linear";
        if (importer->flag_at("mipmaps.enableMipMap", true)) {
            filter += "_with_mipmaps";
        }
        if (filter != "linear_with_mipmaps") {
            material["texture_filter"] = filter;
        }
        std::int64_t wrap = importer->integer_at("textureSettings.wrapU", -1);
        if (wrap < 0) {
            wrap = importer->integer_at("textureSettings.wrapMode", 0);
        }
        if (wrap == 1) {
            material["texture_repeat"] = false;
        } else if (wrap == 2 || wrap == 3) {
            problems_.add("mirrored texture wrapping", where, "the texture repeats");
        }
    }

    // --- The 3D environment ---

    // A skybox material as the sky's component and its fields, or nullopt.
    std::optional<std::pair<std::string, json::Value>> sky_material(const ObjectRef& ref,
                                                                    const std::string& where) {
        // Unity's procedural sky scatters the sun's light through the air in HDR. Godot's physical
        // sky does too, but is dim without HDR, so it becomes a ProceduralSkyMaterial's gradient
        // in the colors Unity's draws with its default settings: its tint scales the sky's colors
        // and its exposure, 1.3 by default, the sky's energy.
        const auto procedural = [&](const MaterialFile* file) {
            json::Value sky = json::Object{};
            const auto number = [&](const char* name, double fallback) {
                return file != nullptr ? file->number(name, fallback) : fallback;
            };
            const auto color = [&](const char* name) -> const Node* {
                if (file == nullptr) {
                    return nullptr;
                }
                const auto found = file->colors.find(name);
                return found != file->colors.end() ? &found->second : nullptr;
            };
            static constexpr std::array<double, 3> kTop{0.29, 0.45, 0.75};
            static constexpr std::array<double, 3> kHorizon{0.72, 0.77, 0.84};
            const Node* tint = color("_SkyTint");
            const std::array<double, 3> scale{
                tint != nullptr ? tint->number_at("r", 0.5) / 0.5 : 1.0,
                tint != nullptr ? tint->number_at("g", 0.5) / 0.5 : 1.0,
                tint != nullptr ? tint->number_at("b", 0.5) / 0.5 : 1.0};
            const auto tinted = [&](const std::array<double, 3>& rgb) {
                json::Array out;
                for (std::size_t i = 0; i < 3; ++i) {
                    out.emplace_back(tidy_number(std::min(1.0, rgb.at(i) * scale.at(i))));
                }
                out.emplace_back(1.0);
                return json::Value(std::move(out));
            };
            sky["sky_top_color"] = tinted(kTop);
            sky["sky_horizon_color"] = tinted(kHorizon);
            sky["ground_horizon_color"] = tinted(kHorizon);
            if (const Node* ground = color("_GroundColor")) {
                json::Value bottom = rgba(*ground);
                bottom.as_array()[3] = 1.0;
                sky["ground_bottom_color"] = std::move(bottom);
            } else {
                sky["ground_bottom_color"] = json::Array{0.369, 0.349, 0.341, 1.0};
            }
            sky["energy_multiplier"] = tidy_number(number("_Exposure", 1.3) / 1.3);
            problems_.add("procedural skybox", where,
                          "approximated by ProceduralSkyMaterial's gradient, tinted as Unity's "
                          "sky is");
            if (number("_AtmosphereThickness", 1.0) != 1.0) {
                problems_.add("skybox atmosphere thickness", where,
                              "the sky has the colors of the default thickness");
            }
            return std::pair<std::string, json::Value>{"ProceduralSkyMaterial", std::move(sky)};
        };
        if (ref.guid == kBuiltinSprites && ref.file_id == kDefaultSkybox) {
            return procedural(nullptr);
        }
        const auto file = read_material(ref.guid);
        if (!file) {
            problems_.add("skybox material", where,
                          "a material that isn't a readable .mat file in the project's Assets "
                          "folder");
            return std::nullopt;
        }
        if (file->numbers.contains("_AtmosphereThickness") || file->colors.contains("_SkyTint")) {
            return procedural(&*file);
        }
        if (file->textures.contains("_MainTex") && file->numbers.contains("_Mapping")) {
            // A panoramic sky. Unity's longitude turns from x towards z and Godot's from -z
            // towards x; in mirrored axes they agree a quarter turn apart.
            if (file->number("_Mapping", 1.0) != 1.0 || file->number("_ImageType", 0.0) != 0.0) {
                problems_.add("panoramic skybox layout", file->path,
                              "only 360-degree latitude-longitude panoramas convert");
                return std::nullopt;
            }
            std::string panorama = material_texture(*file, "_MainTex");
            if (panorama.empty()) {
                return std::nullopt;
            }
            json::Value sky = json::Object{};
            sky["panorama"] = std::move(panorama);
            sky["energy_multiplier"] = file->number("_Exposure", 1.0);
            double turn = (-file->number("_Rotation", 0.0) * std::numbers::pi / 180.0) -
                          (std::numbers::pi / 2.0);
            turn = std::remainder(turn, 2.0 * std::numbers::pi);
            sky_rotation_ = tidy_number(turn);
            return std::pair<std::string, json::Value>{"PanoramaSkyMaterial", std::move(sky)};
        }
        problems_.add("cubemap skybox", file->path,
                      "procedural and panoramic skyboxes convert; the background is the camera's "
                      "color");
        return std::nullopt;
    }

    // The scene's sky, ambient light and fog, from its RenderSettings and the background its main
    // camera clears to, on an entity of its own.
    void environment_3d(Expanded& file, json::Object& entities) {
        const Node& settings = file.render_settings;
        const std::string where = file.path + ": RenderSettings";
        json::Value environment = json::Object{};
        json::Object components;
        sky_rotation_ = 0.0;
        std::optional<std::pair<std::string, json::Value>> sky;
        if (const auto ref = ref_at(settings, "m_SkyboxMaterial"); ref && !ref->null()) {
            sky = sky_material(*ref, where);
        }
        const std::int64_t clear = scene_camera_ ? scene_camera_->clear : 1;
        if (clear == 1 && sky) {
            environment["background_mode"] = "sky";
        } else if (scene_camera_ && (clear == 1 || clear == 2)) {
            environment["background_mode"] = "color";
            environment["background_color"] = scene_camera_->color;
        } else if (scene_camera_) {
            problems_.add("camera that clears only depth or nothing", file.path,
                          "the background is the project's clear color");
        }
        const auto color = [&](std::string_view key) {
            const Node* value = settings.get(key);
            return value != nullptr ? json::Array{value->number_at("r"), value->number_at("g"),
                                                  value->number_at("b"), 1.0}
                                    : json::Array{0.0, 0.0, 0.0, 1.0};
        };
        switch (settings.integer_at("m_AmbientMode", 0)) {
        case 1:
            environment["ambient_light_source"] = "color";
            environment["ambient_light_color"] = color("m_AmbientEquatorColor");
            problems_.add("gradient ambient light", where,
                          "one color lights the scene all round: the equator's");
            break;
        case 3:
            environment["ambient_light_source"] = "color";
            environment["ambient_light_color"] = color("m_AmbientSkyColor");
            break;
        default:
            if (settings.integer_at("m_AmbientMode", 0) != 0) {
                problems_.add("custom ambient light", where, "the sky lights the scene");
            }
            if (sky) {
                environment["ambient_light_source"] = "sky";
                environment["ambient_light_energy"] = settings.number_at("m_AmbientIntensity", 1.0);
            } else {
                environment["ambient_light_source"] = "color";
                environment["ambient_light_color"] = color("m_AmbientSkyColor");
            }
            break;
        }
        if (sky) {
            environment["reflected_light_source"] = "sky";
            if (sky_rotation_ != 0.0) {
                environment["sky_rotation"] = json::Array{0.0, sky_rotation_, 0.0};
            }
        }
        if (settings.flag_at("m_Fog")) {
            environment["fog_enabled"] = true;
            environment["fog_light_color"] = color("m_FogColor");
            environment["fog_sky_affect"] = 0.0; // Unity's fog leaves the sky alone
            const std::int64_t mode = settings.integer_at("m_FogMode", 3);
            if (mode == 1) {
                environment["fog_mode"] = "depth";
                environment["fog_density"] = 1.0;
                environment["fog_depth_begin"] = settings.number_at("m_LinearFogStart", 0.0);
                environment["fog_depth_end"] = settings.number_at("m_LinearFogEnd", 300.0);
                problems_.add("linear fog", where,
                              "depth fog thickens from its start to its end along a smooth curve, "
                              "not a straight line");
            } else {
                environment["fog_density"] = settings.number_at("m_FogDensity", 0.01);
                if (mode == 3) {
                    problems_.add("exponential squared fog", where,
                                  "it thickens as Unity's exponential fog does");
                }
            }
        }
        components["WorldEnvironment"] = std::move(environment);
        if (sky) {
            components[sky->first] = std::move(sky->second);
        }
        json::Value entity;
        entity["components"] = std::move(components);
        entities[take(file.used, "environment")] = std::move(entity);
    }

    // Reads the files this import has written so far, for checking scenes that instance them.
    [[nodiscard]] world::FileReader planned_files() const {
        return [this](std::string_view path) -> Result<std::string> {
            for (const auto& [planned, contents] : plan_.files) {
                if (planned == path) {
                    return contents;
                }
            }
            // Files the project keeps as they are, such as models, from the Unity project.
            if (path.starts_with(prefix_) && is_model_file(extension_of(path))) {
                return platform::read_file(source_ + std::string(path.substr(prefix_.size())));
            }
            return make_error(ErrorCode::NotFound, "not converted");
        };
    }

    // --- The project ---

    json::Value convert_project(const std::optional<std::string>& existing) {
        std::string name = player_.string_at("productName");
        if (name.empty()) {
            name = "Imported Unity project";
        }
        project_name_ = name;
        if (!main_source_.empty()) {
            main_scene_ =
                destination(without_extension(main_source_) + std::string(world::kSceneSuffix));
        }
        json::Value project = runtime::new_project(name, main_scene_);
        if (existing) {
            if (auto parsed = json::parse(*existing); parsed && parsed.value().is_object()) {
                json::Value merged = std::move(parsed.value());
                merged["name"] = name;
                if (!main_scene_.empty()) {
                    merged["main_scene"] = main_scene_;
                }
                project = std::move(merged);
            }
        }
        const std::string version = player_.string_at("bundleVersion");
        const std::vector<std::string_view> parts = split_path(version, '.');
        bool numeric = !parts.empty() && parts.size() <= 3;
        for (const std::string_view part : parts) {
            numeric = numeric && parse_int(part).has_value() && part.front() != '-';
        }
        if (numeric) {
            std::string full = version;
            for (std::size_t i = parts.size(); i < 3; ++i) {
                full += ".0";
            }
            project["version"] = full;
        }
        project["window"]["width"] = static_cast<std::int64_t>(window_width_);
        project["window"]["height"] = static_cast<std::int64_t>(window_height_);
        if (const auto time = settings("TimeManager.asset", "TimeManager")) {
            double step = 0.0;
            if (const Node* fixed = time->get("Fixed Timestep")) {
                if (fixed->is_map()) {
                    // Since Unity 2022: a count of ticks at a rate.
                    const double count = fixed->number_at("m_Count");
                    const double numerator = fixed->number_at("m_Rate.m_Numerator");
                    const double denominator = fixed->number_at("m_Rate.m_Denominator", 1.0);
                    step = numerator > 0.0 ? count * denominator / numerator : 0.0;
                } else {
                    step = fixed->number().value_or(0.0);
                }
            }
            if (step > 0.0) {
                project["tick_hz"] = std::round(1.0 / step * 1000.0) / 1000.0;
            }
        }
        const double gx = physics_.number_at("m_Gravity.x", 0.0);
        const double gy = physics_.number_at("m_Gravity.y", -9.81);
        const double gravity = std::hypot(gx, gy);
        project["physics_2d"]["gravity"] = tidy(gravity * ppu_);
        if (gravity > 0.0) {
            project["physics_2d"]["gravity_vector"] =
                json::Array{tidy(gx / gravity), tidy(-gy / gravity)};
        }
        // Unity has no damping besides each body's own.
        project["physics_2d"]["linear_damp"] = 0.0;
        project["physics_2d"]["angular_damp"] = 0.0;
        if (three_d_files_ > 0) {
            // Mirrored in x, as the scenes are.
            const double x = physics3d_.number_at("m_Gravity.x", 0.0);
            const double y = physics3d_.number_at("m_Gravity.y", -9.81);
            const double z = physics3d_.number_at("m_Gravity.z", 0.0);
            const double length = std::sqrt((x * x) + (y * y) + (z * z));
            project["physics_3d"]["gravity"] = tidy(length);
            if (length > 0.0) {
                project["physics_3d"]["gravity_vector"] =
                    json::Array{tidy(-x / length), tidy(y / length), tidy(z / length)};
            }
            project["physics_3d"]["linear_damp"] = 0.0;
            project["physics_3d"]["angular_damp"] = 0.0;
        }
        if (const auto color = clear_colors_.find(main_source_); color != clear_colors_.end()) {
            project["rendering"]["clear_color"] = color->second;
        }
        if (nearest_textures_ + linear_textures_ > 0) {
            project["rendering"]["texture_filter"] =
                nearest_textures_ > linear_textures_ ? "nearest" : "linear";
        }
        if (!bus_layout_.empty()) {
            project["audio"]["bus_layout"] = bus_layout_;
        }
        if (const auto quality = settings("QualitySettings.asset", "QualitySettings")) {
            const std::int64_t current = quality->integer_at("m_CurrentQuality", 0);
            if (const Node* levels = quality->get("m_QualitySettings");
                levels != nullptr && current >= 0 &&
                static_cast<std::size_t>(current) < levels->items.size()) {
                const std::int64_t vsync =
                    levels->items[static_cast<std::size_t>(current)].integer_at("vSyncCount", 1);
                project["window"]["vsync_mode"] = vsync == 0 ? "disabled" : "enabled";
            }
        }
        return project;
    }

    void write_project(json::Value project) {
        // The scripts' C++ skeletons are the game's code.
        if (!skeletons().empty()) {
            json::Value& gameplay = project["gameplay"];
            json::Value* sources = gameplay.is_object() ? gameplay.find("sources") : nullptr;
            if (sources == nullptr || !sources->is_array()) {
                gameplay = json::Object{};
                gameplay["sources"] = json::Array{skeletons().folder()};
            } else if (!contains(sources->as_array(), json::Value(skeletons().folder()))) {
                sources->as_array().emplace_back(skeletons().folder());
            }
        }
        // Keep only what validates, so a bad setting can't stop the rest.
        for (int pass = 0; pass < 32; ++pass) {
            const auto errors = runtime::validate_project(project);
            if (errors.empty()) {
                break;
            }
            bool reset = false;
            for (const json::ValidationError& error : errors) {
                problems_.add("setting " + error.path, "ProjectSettings", error.message);
                if (!remove_pointer(project, error.path)) {
                    project = runtime::new_project(project_name_, main_scene_);
                    reset = true;
                    break;
                }
            }
            if (reset) {
                break;
            }
        }
        add_file("project.json", json::write(project) + "\n");
    }

    // --- Input ---

    static void bind(json::Value& project, const std::string& action, json::Value binding) {
        json::Value& events = project["input"][action]["events"];
        if (events.is_null()) {
            events = json::Array{};
        }
        if (!contains(events.as_array(), binding)) {
            events.push_back(std::move(binding));
        }
    }

    void convert_input(json::Value& project) {
        // 0: the old Input Manager, 1: the Input System package, 2: both.
        const std::int64_t handler = player_.integer_at("activeInputHandler", 0);
        if (handler != 1) {
            convert_input_manager(project);
        }
        for (const std::string& file : files_) {
            if (extension_of(file) == ".inputactions") {
                convert_input_actions(file, project);
            }
        }
        if (const json::Value* input = project.find("input");
            input != nullptr && input->is_object()) {
            input_actions_ = static_cast<int>(input->as_object().size());
        }
    }

    void convert_input_manager(json::Value& project) {
        const auto manager = settings("InputManager.asset", "InputManager");
        const Node* axes = manager ? manager->get("m_Axes") : nullptr;
        if (axes == nullptr) {
            return;
        }
        const std::string where = "ProjectSettings/InputManager.asset";
        // An axis splits into name/negative and name/positive when any entry of that name goes
        // both ways.
        std::set<std::string> split;
        for (const Node& axis : axes->items) {
            if (!axis.string_at("negativeButton").empty() ||
                !axis.string_at("altNegativeButton").empty() || axis.integer_at("type", 0) == 2) {
                split.insert(axis.string_at("m_Name"));
            }
        }
        for (const Node& axis : axes->items) {
            const std::string name = axis.string_at("m_Name");
            const std::int64_t type = axis.integer_at("type", 0);
            if (name.empty()) {
                continue;
            }
            const bool both = split.contains(name);
            const std::string negative = both ? name + "/negative" : name;
            const std::string positive = both ? name + "/positive" : name;
            if (type == 1) {
                problems_.add("input axis " + name, where,
                              "mouse movement axes: read the mouse's motion in code");
                continue;
            }
            if (type == 2) {
                static constexpr std::array<std::string_view, 5> kAxes{"left_x", "left_y", "",
                                                                       "right_x", "right_y"};
                const std::int64_t index = axis.integer_at("axis", 0);
                if (index < 0 || index >= 5 || kAxes.at(static_cast<std::size_t>(index)).empty()) {
                    problems_.add("input axis " + name, where,
                                  "joystick axis " + std::to_string(index + 1));
                    continue;
                }
                // Unity's y axes point down; inverted, up is positive.
                const int sign = axis.flag_at("invert") ? -1 : 1;
                const std::string_view stick = kAxes.at(static_cast<std::size_t>(index));
                bind(project, negative, pad_motion(stick, -sign));
                bind(project, positive, pad_motion(stick, sign));
                continue;
            }
            for (const auto& [key, action] :
                 {std::pair{"negativeButton", negative}, std::pair{"altNegativeButton", negative},
                  std::pair{"positiveButton", positive},
                  std::pair{"altPositiveButton", positive}}) {
                const std::string button = axis.string_at(key);
                if (button.empty()) {
                    continue;
                }
                std::string why;
                json::Value binding = input_manager_binding(button, why);
                if (binding.is_null()) {
                    problems_.add("input button", std::string(where).append(": ").append(name),
                                  why);
                    continue;
                }
                bind(project, action, std::move(binding));
            }
        }
    }

    void convert_input_actions(const std::string& file, json::Value& project) {
        auto text = platform::read_file(source_ + file);
        auto parsed = text ? json::parse(text.value()) : Result<json::Value>(text.error());
        if (!parsed || !parsed.value().is_object()) {
            problems_.add("unreadable file", file,
                          parsed ? "not a JSON object" : parsed.error().message);
            return;
        }
        const json::Value* maps = parsed.value().find("maps");
        if (maps == nullptr || !maps->is_array()) {
            return;
        }
        const auto string_of = [](const json::Value& object, std::string_view key) {
            const json::Value* value = object.find(key);
            return value != nullptr && value->is_string() ? value->as_string() : std::string();
        };
        const auto flag_of = [](const json::Value& object, std::string_view key) {
            const json::Value* value = object.find(key);
            return value != nullptr && value->is_bool() && value->as_bool();
        };
        for (const json::Value& map : maps->as_array()) {
            if (!map.is_object()) {
                continue;
            }
            const std::string map_name = string_of(map, "name");
            // Each action's kind: "vector" (up, down, left, right), "axis" (negative, positive)
            // or "button".
            std::map<std::string, std::string> kinds;
            if (const json::Value* actions = map.find("actions");
                actions != nullptr && actions->is_array()) {
                for (const json::Value& action : actions->as_array()) {
                    if (!action.is_object()) {
                        continue;
                    }
                    const std::string control = string_of(action, "expectedControlType");
                    const std::string type = string_of(action, "type");
                    std::string kind = "button";
                    if (type != "Button" &&
                        (control == "Vector2" || control == "Dpad" || control == "Stick")) {
                        kind = "vector";
                    } else if (type != "Button" && control == "Axis") {
                        kind = "axis";
                    }
                    kinds[string_of(action, "name")] = kind;
                }
            }
            const json::Value* bindings = map.find("bindings");
            if (bindings == nullptr || !bindings->is_array()) {
                continue;
            }
            std::string composite;     // the composite whose parts follow
            std::string modifier_part; // a ButtonWithOneModifier's modifier key
            for (const json::Value& binding : bindings->as_array()) {
                if (!binding.is_object()) {
                    continue;
                }
                const std::string action = string_of(binding, "action");
                const std::string path = string_of(binding, "path");
                const std::string base = std::string(map_name).append("/").append(action);
                const std::string where = std::string(file).append(": ").append(base);
                const std::string kind = kinds.contains(action) ? kinds[action] : "button";
                if (flag_of(binding, "isComposite")) {
                    composite = path;
                    if (path != "2DVector" && path != "Dpad" && path != "1DAxis" &&
                        path != "Axis") {
                        problems_.add("input composite " + path, where,
                                      "only 2D vector and 1D axis composites convert");
                    }
                    continue;
                }
                if (const std::string interactions = string_of(binding, "interactions");
                    !interactions.empty()) {
                    problems_.add("input interaction", where,
                                  interactions + ": gameplay code times holds and taps");
                }
                const std::string processors = string_of(binding, "processors");
                if (processors.find("invert") != std::string::npos) {
                    problems_.add("input processor", where, processors);
                }
                std::string target = base;
                if (flag_of(binding, "isPartOfComposite")) {
                    const std::string part = lowercase(string_of(binding, "name"));
                    if (composite == "2DVector" || composite == "Dpad") {
                        if (part != "up" && part != "down" && part != "left" && part != "right") {
                            continue;
                        }
                        target = std::string(base).append("/").append(part);
                    } else if (composite == "1DAxis" || composite == "Axis") {
                        if (part != "negative" && part != "positive") {
                            continue;
                        }
                        target = std::string(base).append("/").append(part);
                    } else {
                        continue;
                    }
                } else {
                    composite.clear();
                    modifier_part.clear();
                    if (kind == "vector") {
                        bind_vector(project, base, path, where);
                        continue;
                    }
                    if (kind == "axis") {
                        problems_.add("input binding", where,
                                      "\"" + path +
                                          "\": bind an axis action through a 1D axis "
                                          "composite");
                        continue;
                    }
                }
                std::string why;
                json::Value converted = control_binding(path, why);
                if (converted.is_null()) {
                    problems_.add("input binding", where, why);
                    continue;
                }
                bind(project, target, std::move(converted));
            }
        }
    }

    // A stick or a D-pad bound whole to a vector action: its four directions.
    void bind_vector(json::Value& project, const std::string& base, const std::string& path,
                     const std::string& where) {
        static constexpr std::array<std::string_view, 4> kParts{"up", "down", "left", "right"};
        bool any = false;
        for (const std::string_view part : kParts) {
            std::string why;
            json::Value converted = control_binding(path + "/" + std::string(part), why);
            if (!converted.is_null()) {
                bind(project, base + "/" + std::string(part), std::move(converted));
                any = true;
            }
        }
        if (!any) {
            problems_.add("input binding", where,
                          "\"" + path +
                              "\": only gamepad sticks and D-pads bind whole to a "
                              "vector");
        }
    }

    // --- Tasks and the report ---

    void write_tasks(const std::optional<std::string>& existing) {
        std::vector<PortTask> tasks;
        for (const auto& [script, record] : scripts_) {
            PortTask task;
            task.link = destination(script);
            task.name = "Port " + script + " to C++";
            task.summary = "C# from Unity: write what it does as C++ components and systems.";
            task.engine = "unity";
            std::string text;
            if (auto source = platform::read_file(source_ + script); source) {
                const ScriptOutline outline = outline_csharp(source.value());
                if (!outline.classes.empty()) {
                    text += "Class " + join(outline.classes, "; ") + ".";
                }
                if (!outline.fields.empty()) {
                    text += " Fields: " + join(outline.fields, "; ") + ".";
                }
                if (!outline.methods.empty()) {
                    text += " Methods: " + join(outline.methods, ", ") + ".";
                }
            }
            std::vector<std::string> uses;
            for (const ScriptUse& use : record.uses) {
                uses.push_back(use.entity + " in " + use.scene);
                task.links.push_back(use.scene);
            }
            if (!uses.empty()) {
                text += " Used by " + join(uses, ", ") + ".";
            }
            if (!record.calls.empty()) {
                text += " UI events call it: " + join(record.calls, "; ") + ".";
            }
            if (const Skeleton* skeleton = skeletons().find(script); skeleton != nullptr) {
                const Script& code = skeleton->script;
                text += " Its C++ skeleton is " + skeleton->code;
                text += code.node ? ": the " + code.component + " component and the " +
                                        code.system + " system."
                                  : ": the " + code.component + " struct.";
                task.links.push_back(skeleton->code);
            }
            text += " The values each entity's component set and the entity ID of every object "
                    "are in " +
                    std::string(kReportFile) + ".";
            task.text = trim_text(text);
            tasks.push_back(std::move(task));
        }
        std::vector<std::string> controller_tasks; // controller GUIDs, in task order
        for (const auto& [guid, controller] : libraries_) {
            if (controller.path.empty() || controller.task_text.empty()) {
                continue;
            }
            PortTask task;
            task.link = controller.path;
            const Asset* asset = asset_of(guid);
            task.name = "Port the state machine of " + (asset != nullptr ? asset->path : guid);
            task.summary =
                controller.tree.empty()
                    ? "Unity Animator controller: pick its animations from gameplay code."
                    : "Unity Animator controller: set its parameters from gameplay code.";
            task.engine = "unity";
            std::string text = controller.task_text;
            if (!controller.users.empty()) {
                text += " Played by " + join(controller.users, ", ") + ".";
            }
            task.text = trim_text(text);
            if (!controller.tree.empty()) {
                task.links.push_back(controller.tree);
            }
            task.links.insert(task.links.end(), controller.scenes.begin(), controller.scenes.end());
            tasks.push_back(std::move(task));
            controller_tasks.push_back(guid);
        }
        PortTasks written = write_port_tasks(existing, tasks);
        std::size_t index = 0;
        for (const auto& [script, record] : scripts_) {
            task_of_[script] = written.ids[index++];
        }
        for (const std::string& guid : controller_tasks) {
            task_of_[guid] = written.ids[index++];
        }
        if (!written.file.empty()) {
            add_file(storyboard::file_path("work"), std::move(written.file));
        }
    }

    // Writes each script's skeleton but those the destination has, the file registering them
    // all and, when the game's code has none, a game module calling it.
    void write_skeletons() {
        for (auto& [script, entry] : skeletons().all()) {
            entry.script.task = task_of_[script];
            entry.script.connections = scripts_[script].connections;
        }
        module_ = skeletons().write(
            [this](std::string path, std::string text) {
                add_file(std::move(path), std::move(text));
            },
            problems_);
    }

    void write_report() {
        json::Value report;
        report["format"] = 1;
        report["engine"] = "unity";
        report["unity_version"] = unity_version_;
        report["pixels_per_unit"] = ppu_;
        report["scenes"] = json::Object{};
        for (auto& [path, record] : report_scenes_) {
            report["scenes"][path] = std::move(record);
        }
        json::Value scripts = json::Object{};
        json::Array summary;
        for (const auto& [script, record] : scripts_) {
            json::Value item;
            item["task"] = task_of_[script];
            json::Array uses;
            for (const ScriptUse& use : record.uses) {
                json::Value entry;
                entry["scene"] = use.scene;
                entry["entity"] = use.entity;
                if (!use.enabled) {
                    entry["enabled"] = false;
                }
                if (!use.values.as_object().empty()) {
                    entry["values"] = use.values;
                }
                uses.push_back(std::move(entry));
            }
            item["used_by"] = std::move(uses);
            if (!record.calls.empty()) {
                json::Array calls;
                for (const std::string& call : record.calls) {
                    calls.emplace_back(call);
                }
                item["ui_events"] = std::move(calls);
            }
            json::Value brief;
            brief["script"] = destination(script);
            brief["task"] = task_of_[script];
            brief["entities"] = static_cast<std::int64_t>(record.uses.size());
            if (const Skeleton* skeleton = skeletons().find(script); skeleton != nullptr) {
                for (json::Value* entry : {&item, &brief}) {
                    (*entry)["code"] = skeleton->code;
                    (*entry)["component"] = skeleton->script.component;
                    if (skeleton->kept) {
                        (*entry)["kept"] = true;
                    }
                }
                if (skeleton->script.node) {
                    item["system"] = skeleton->script.system;
                }
            }
            scripts[destination(script)] = item;
            summary.push_back(std::move(brief));
        }
        report["scripts"] = std::move(scripts);
        if (!tags_.empty()) {
            json::Value tags = json::Object{};
            for (const auto& [tag, members] : tags_) {
                json::Array list;
                for (const std::string& member : members) {
                    list.emplace_back(member);
                }
                tags[tag] = std::move(list);
            }
            report["tags"] = std::move(tags);
        }
        json::Value layers = json::Object{};
        for (const auto& [index, name] : layer_names_) {
            layers[name] = std::int64_t{1} << index;
        }
        report["physics_layers"] = std::move(layers);
        if (sorting_layers_.size() > 1) {
            json::Value sorting = json::Object{};
            for (std::size_t i = 0; i < sorting_layers_.size(); ++i) {
                sorting[sorting_layers_[i].second] =
                    (static_cast<int>(i) - default_sorting_) * sorting_step_;
            }
            report["sorting_layers"] = std::move(sorting);
        }
        if (!tile_assets_.empty()) {
            json::Value tiles = json::Object{};
            for (const auto& [asset, use] : tile_assets_) {
                json::Value item;
                item["tile_set"] = use.first;
                item["tile"] = use.second;
                tiles[asset] = std::move(item);
            }
            report["tile_assets"] = std::move(tiles);
        }
        json::Value animation = json::Object{};
        for (const auto& [guid, controller] : libraries_) {
            if (controller.path.empty()) {
                continue;
            }
            json::Value item;
            const Asset* asset = asset_of(guid);
            item["controller"] = asset != nullptr ? asset->path : guid;
            if (!controller.first.empty()) {
                item["first"] = controller.first;
            }
            if (!controller.tree.empty()) {
                item["tree"] = controller.tree;
            }
            json::Array users;
            for (const std::string& user : controller.users) {
                users.emplace_back(user);
            }
            item["used_by"] = std::move(users);
            if (const auto task = task_of_.find(guid); task != task_of_.end()) {
                item["task"] = task->second;
            }
            animation[controller.path] = std::move(item);
        }
        if (!animation.as_object().empty()) {
            report["animation_libraries"] = std::move(animation);
        }
        if (!bus_layout_.empty()) {
            report["bus_layout"]["path"] = bus_layout_;
            report["bus_layout"]["groups"] = bus_groups_;
        }
        json::Array unsupported = problems_.to_json();
        report["unsupported"] = unsupported;
        add_file(std::string(kReportFile), json::write(report) + "\n");

        json::Value& out = plan_.report;
        out["engine"] = "unity";
        out["project"] = project_name_;
        out["unity_version"] = unity_version_;
        if (!main_scene_.empty()) {
            out["main_scene"] = main_scene_;
        }
        out["pixels_per_unit"] = ppu_;
        out["converted"]["scenes"] = scenes_;
        out["converted"]["prefabs"] = prefabs_;
        out["converted"]["tile_sets"] = static_cast<std::int64_t>(std::ranges::count_if(
            tile_sets_, [](const TileSetOut& set) { return !set.path.empty(); }));
        out["converted"]["animation_libraries"] = animation_libraries_;
        out["converted"]["animation_trees"] = animation_trees_;
        out["converted"]["bus_layouts"] = bus_layout_.empty() ? 0 : 1;
        out["converted"]["input_actions"] = input_actions_;
        out["converted"]["assets"] = assets_;
        out["scripts"] = std::move(summary);
        if (!module_.empty()) {
            out["module"] = module_;
        }
        out["unsupported"] = std::move(unsupported);
        out["report"] = kReportFile;
    }

    std::string source_;
    std::string prefix_;
    bool copy_assets_ = false;
    world::ComponentRegistry components_;
    std::vector<std::string> files_; // sorted, relative to source_
    std::string unity_version_;
    std::map<std::string, Asset> assets_by_guid_;
    std::map<std::string, Texture> textures_;
    std::map<std::string, Material> materials_;
    Material default_material_;
    std::map<std::string, std::unique_ptr<Expanded>> expanded_;
    Node player_;
    Node physics_;
    double ppu_ = 100.0;
    double window_width_ = 1024.0;
    double window_height_ = 768.0;
    int nearest_textures_ = 0;
    int linear_textures_ = 0;
    std::array<std::uint32_t, 32> layer_masks_ = [] {
        std::array<std::uint32_t, 32> masks{};
        masks.fill(0xFFFFFFFFU);
        return masks;
    }();
    std::map<std::size_t, std::string> layer_names_;
    std::vector<std::pair<std::int64_t, std::string>> sorting_layers_; // unique ID, name
    int default_sorting_ = 0;
    int sorting_step_ = 0;
    std::string main_source_;
    std::optional<json::Value> scene_clear_color_;
    // The scene being converted has an enabled 2D light, and its global light.
    bool scene_lit_ = false;
    bool scene_global_ = false;
    std::set<std::string> light_textures_; // light textures written
    // Light textures cut from sprite sheets, by the sprite's texture GUID and file ID.
    std::map<std::pair<std::string, std::int64_t>, std::string> cookies_;
    std::set<std::string> cookie_names_;
    std::map<std::string, json::Value> clear_colors_; // by scene
    std::set<std::string> loops_written_;
    std::map<std::string, ScriptRecord> scripts_; // by source path
    std::optional<Skeletons> skeletons_;          // by source path
    std::set<std::string> unreadable_scripts_;
    std::string module_;                           // the game module written, or ""
    std::map<std::string, std::string> tmp_fonts_; // TextMesh Pro font assets' fonts, by path
    std::map<std::string, std::string> task_of_;
    std::map<std::string, std::vector<std::string>> tags_;
    std::map<std::string, json::Value> report_scenes_;
    std::map<std::string, std::map<std::string, std::set<std::string>>> scene_components_;
    Problems problems_;
    ImportPlan plan_;
    std::string project_name_;
    std::string main_scene_;
    int scenes_ = 0;
    int prefabs_ = 0;
    int input_actions_ = 0;
    int assets_ = 0;
    std::deque<TileSetOut> tile_sets_;
    std::map<std::string, std::vector<Object>> tile_files_;                  // by tile asset GUID
    std::map<std::string, std::pair<std::string, std::string>> tile_assets_; // tile set, tile
    std::map<std::string, ClipOut> clips_;        // by "<GUID>:<file ID>"
    std::map<std::string, LibraryOut> libraries_; // by controller GUID
    std::set<std::string> library_paths_;
    int animation_libraries_ = 0;
    int animation_trees_ = 0;
    std::map<std::string, std::map<std::int64_t, std::string>> mixer_buses_; // by mixer GUID
    std::string bus_layout_;
    json::Value bus_groups_; // the buses' names, by mixer and group
    // 3D: whether the project has more 3D components than 2D ones, the scene's main camera, the
    // LOD levels it hides, and materials, physics and the sky's turn as converted.
    bool project_3d_ = false;
    std::optional<SceneCamera> scene_camera_;
    std::set<std::int64_t> lod_hidden_;
    std::set<int> animated_; // the scene's objects Animators move, and those above them
    std::map<std::string, std::optional<json::Value>> surfaces_; // by material GUID
    json::Value default_surface_ = json::Object{{"roughness", 0.5}};
    std::map<std::string, Material> materials_3d_;
    std::map<std::string, std::optional<TerrainFields>> terrains_; // by TerrainData GUID:file ID
    Material default_material_3d_{kDefaultFriction3D, 0.0};
    Node physics3d_;
    std::array<std::uint32_t, 32> layer_masks_3d_ = [] {
        std::array<std::uint32_t, 32> masks{};
        masks.fill(0xFFFFFFFFU);
        return masks;
    }();
    double sky_rotation_ = 0.0;
    int three_d_files_ = 0;
    // Model files read for meshes picked out of them, by path; nullopt for one that doesn't load.
    std::map<std::string, std::optional<scene3d::Model>> models_;
    // Models' Avatars, by the model's path; nullopt for a model with none that loads.
    std::map<std::string, std::optional<Avatar>> avatars_;
    // The GUID of the model whose instance plays the clips being converted for a model, or "".
    std::string clip_target_;
    // glTF files written for meshes picked out of models: by model, mesh and materials, and all.
    std::map<std::string, std::string> picked_;
    std::set<std::string> picked_paths_;
};

} // namespace

Result<ImportPlan> plan_unity(const std::string& source, const std::string& prefix,
                              bool copy_assets, const Existing& existing) {
    UnityImporter importer(source, prefix, copy_assets);
    return importer.run(existing);
}

std::string_view unity::package_script_guid(std::string_view space, std::string_view name) {
    // uGUI's scripts are in UnityEngine.UI and UnityEngine.EventSystems, TextMesh Pro's in TMPro,
    // and URP's 2D lights in UnityEngine.Rendering.Universal (in its Experimental namespace
    // before URP 11).
    static constexpr std::array<std::string_view, 6> kEvents{
        "EventSystem",  "StandaloneInputModule", "BaseInput",
        "EventTrigger", "Physics2DRaycaster",    "PhysicsRaycaster"};
    static constexpr std::array<std::string_view, 3> kLights{"Light2D", "ShadowCaster2D",
                                                             "CompositeShadowCaster2D"};
    bool known_space = false;
    if (name.starts_with("TMP_") || name.starts_with("TextMeshPro")) {
        known_space = space == "TMPro";
    } else if (contains(kEvents, name)) {
        known_space = space == "UnityEngine.EventSystems";
    } else if (contains(kLights, name)) {
        known_space = space == "UnityEngine.Rendering.Universal" ||
                      space == "UnityEngine.Experimental.Rendering.Universal";
    } else {
        known_space = space == "UnityEngine.UI";
    }
    if (!known_space) {
        return {};
    }
    for (const KnownScript& known : kKnownScripts) {
        if (known.name == name) {
            return known.guid;
        }
    }
    return {};
}

} // namespace slash::importer
