#include "unity_import.hpp"

#include "common.hpp"

#include "slash/canvas/debug_draw.hpp"
#include "slash/canvas/tilemap.hpp"
#include "slash/core/json.hpp"
#include "slash/importer/unity_yaml.hpp"
#include "slash/platform/file.hpp"
#include "slash/runtime/project.hpp"
#include "slash/storyboard/storyboard.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/scene.hpp"
#include "slash/world/world.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <numbers>
#include <numeric>
#include <optional>
#include <set>
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
constexpr int kAudioListener = 81;
constexpr int kAudioSource = 82;
constexpr int kGuiLayer = 92;
constexpr int kMonoBehaviour = 114;
constexpr int kFlareLayer = 124;
constexpr int kSpriteRenderer = 212;
constexpr int kRectTransform = 224;
constexpr int kPlatformEffector2D = 251;
constexpr int kPrefabInstance = 1001;
constexpr int kTilemapCollider2D = 19719996;
constexpr int kGrid = 156049354;
constexpr int kTilemapRenderer = 483693784;
constexpr int kTilemap = 1839735485;
// The script of Unity's own Tile class, which tile assets name; other tiles are scripted.
constexpr std::string_view kTileScriptGuid = "0000000000000000e000000000000000";
constexpr std::int64_t kTileScriptId = 13312;

constexpr std::string_view kUiWhy = "Unity UI converts in a later import (on the roadmap)";
constexpr std::string_view kEventWhy =
    "not needed: SlashEngine's gui system sends input to controls itself";

// Components of Unity's own packages, by their scripts' GUIDs, which are the same in every
// project.
struct KnownScript {
    std::string_view guid;
    std::string_view name;
    std::string_view why;
};
constexpr std::array kKnownScripts{
    KnownScript{"fe87c0e1cc204ed48ad3b37840f39efc", "Image", kUiWhy},
    KnownScript{"1344c3c82d62a2a41a3576d8abb8e3ea", "RawImage", kUiWhy},
    KnownScript{"4e29b1a8efbd4b44bb3f3716e73f07ff", "Button", kUiWhy},
    KnownScript{"5f7201a12d95ffc409449d95f23cf332", "Text", kUiWhy},
    KnownScript{"9085046f02f69544eb97fd06b6048fe2", "Toggle", kUiWhy},
    KnownScript{"2fafe2cfe61f6974895a912c3755e8f1", "ToggleGroup", kUiWhy},
    KnownScript{"67db9e8f0e2ae9c40bc1e2b64352a6b4", "Slider", kUiWhy},
    KnownScript{"2a4db7a114972834c8e4117be1d82ba3", "Scrollbar", kUiWhy},
    KnownScript{"1aa08ab6e0800fa44ae55d278d1423e3", "ScrollRect", kUiWhy},
    KnownScript{"0d0b652f32a2cc243917e4028fa0f046", "Dropdown", kUiWhy},
    KnownScript{"d199490a83bb2b844b9695cbf13b01ef", "InputField", kUiWhy},
    KnownScript{"31a19414c41e5ae4aae2af33fee712f6", "Mask", kUiWhy},
    KnownScript{"3312d7739989d2b4e91e6319e9a96d76", "RectMask2D", kUiWhy},
    KnownScript{"30649d3a9faa99c48a7b1166b86bf2a0", "HorizontalLayoutGroup", kUiWhy},
    KnownScript{"59f8146938fff824cb5fd77236b75775", "VerticalLayoutGroup", kUiWhy},
    KnownScript{"8a8695521f0d02e499659fee002a26c2", "GridLayoutGroup", kUiWhy},
    KnownScript{"3245ec927659c4140ac4f8d17403cc18", "ContentSizeFitter", kUiWhy},
    KnownScript{"86710e43de46f6f4bac7c8e50813a599", "AspectRatioFitter", kUiWhy},
    KnownScript{"306cc8c2b49d7114eaa3623786fc2126", "LayoutElement", kUiWhy},
    KnownScript{"0cd44c1031e13a943bb63640046fad76", "CanvasScaler", kUiWhy},
    KnownScript{"e19747de3f5aca642ab2be37e372fb86", "Outline", kUiWhy},
    KnownScript{"cfabb0440166ab443bba8876756fdfa9", "Shadow", kUiWhy},
    KnownScript{"7a98125502f715b4b83cfb77b434e436", "Selectable", kUiWhy},
    KnownScript{"f4688fdb7df04437aeb418b961361dc5", "TextMeshProUGUI", kUiWhy},
    KnownScript{"7b743370ac3e4ec2a1668f5455a8ef8a", "TMP_Dropdown", kUiWhy},
    KnownScript{"2da0c512f12947e489f739169773d7ca", "TMP_InputField", kUiWhy},
    KnownScript{"d0b148fe25e99eb48b9724523833bab1", "EventTrigger", kUiWhy},
    KnownScript{"9541d86e2fd84c1d9990edf0852d74ab", "TextMeshPro",
                "text in the world isn't converted yet; use a Label in a Control"},
    KnownScript{"76c392e42b5098c458856cdf6ecaaaa1", "EventSystem", kEventWhy},
    KnownScript{"4f231c4fb786f3946a6b90b886c48677", "StandaloneInputModule", kEventWhy},
    KnownScript{"dc42784cf147c0c48a680349fa168899", "GraphicRaycaster", kEventWhy},
    KnownScript{"1511ccae7919cfc46b603b9b337fdc94", "BaseInput", kEventWhy},
    KnownScript{"56666c5a40171f54783dd416a44f42bf", "Physics2DRaycaster", kEventWhy},
    KnownScript{"c49b4cc203aa6414fae5c798d1d0e7d6", "PhysicsRaycaster", kEventWhy},
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

    Result<ImportPlan> run(const std::optional<std::string>& existing_project,
                           const std::optional<std::string>& existing_work) {
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
        for (const std::string& file : files_) {
            take_file(file);
        }
        for (const std::string& file : files_) {
            const std::string extension = extension_of(file);
            if (extension == ".prefab" || extension == ".unity") {
                (void)convert_file(file, 0);
            }
        }
        for (const TileSetOut& set : tile_sets_) {
            write_tile_set(set); // with the physics a later tile map gave it
        }
        json::Value project = convert_project(existing_project);
        convert_input(project);
        write_project(std::move(project));
        write_tasks(existing_work);
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
        // The units: the pixels per unit most of the project's sprites use.
        std::map<double, int> ppus;
        for (const auto& [guid, asset] : assets_by_guid_) {
            const Node* importer = asset.meta.get("TextureImporter");
            if (importer != nullptr && importer->integer_at("textureType", 0) == 8) {
                ++ppus[importer->number_at("spritePixelsToUnits", 100.0)];
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
        if (extension == ".meta" || extension == ".unity" || extension == ".prefab") {
            return;
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
            is_data_file(extension)) {
            if (!is_data_file(extension)) {
                ++assets_;
            }
            if (copy_assets_) {
                copy(file);
            }
            return;
        }
        static const std::map<std::string_view, std::string_view> kWhys{
            {".mat", "materials and shaders don't convert; sprites draw with the default "
                     "material"},
            {".shader", "a game's own shaders are on the roadmap"},
            {".shadergraph", "a game's own shaders are on the roadmap"},
            {".anim", "Unity animation clips convert in a later import (on the roadmap)"},
            {".controller", "Animator controllers convert in a later import (on the roadmap)"},
            {".overridecontroller",
             "Animator controllers convert in a later import (on the roadmap)"},
            {".mixer", "audio mixers convert in a later import (on the roadmap)"},
            {".dll", "compiled plugins don't load; closed SDKs come in as SDK packages (on the "
                     "roadmap)"},
            {".so", "compiled plugins don't load; closed SDKs come in as SDK packages (on the "
                    "roadmap)"},
            {".aar", "compiled plugins don't load; closed SDKs come in as SDK packages (on the "
                     "roadmap)"},
            {".spriteatlas", "not needed: sprites draw from their own textures"},
            {".spriteatlasv2", "not needed: sprites draw from their own textures"},
            {".rendertexture", "render textures aren't supported yet"},
            {".lighting", "3D lighting comes with milestone 7"},
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
        std::string filter;
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
        if (!is_texture_file(extension)) {
            const std::string why = unsupported_asset(extension);
            out.why = asset->path + ": " + (why.empty() ? "not a texture" : why);
            return out;
        }
        auto bytes = platform::read_file(source_ + asset->path);
        std::optional<std::pair<int, int>> size;
        if (bytes) {
            size = extension == ".png" ? png_size(bytes.value()) : bmp_size(bytes.value());
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
            std::tie(sprite.pivot_x, sprite.pivot_y) =
                alignment_pivot(importer->integer_at("alignment", 0), importer->get("spritePivot"));
            out.sprites[kSingleSprite] = std::move(sprite);
        }
        out.ok = true;
        return out;
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

    // --- Scenes and prefabs, with the prefabs they instance ---

    struct Component {
        std::int64_t id = 0;
        int class_id = 0;
        std::string type;
        Node body;
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
    };

    struct Expanded;

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
        }
        std::vector<std::int64_t> root_order;
        for (const Object& object : objects) {
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
        }
        build_hierarchy(file, root_order);
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
            if (instance != nullptr && instance->root != index) {
                go.entity = file.objects[static_cast<std::size_t>(instance->root)].entity + "/" +
                            go.source_id;
            } else {
                const std::string base = snake_case(go.body.string_at("m_Name"));
                if (!file.used.contains(base) || go.parent < 0) {
                    go.entity = take(file.used, base);
                } else {
                    const std::string& parent =
                        file.objects[static_cast<std::size_t>(go.parent)].entity;
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

    // What one object becomes: its components, and entities made for it by what they are for.
    struct Converted {
        json::Object components;
        std::vector<std::pair<std::string, json::Object>> added;
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
        if (const Component* transform = transform_of(go)) {
            out.components["Transform2D"] = transform_2d(transform->body, ctx.where);
        } else {
            out.components["Transform2D"] = json::Object{};
        }
        if (!ctx.active) {
            problems_.add("inactive object", ctx.where,
                          "converted hidden, with its shapes disabled; there is no inactive "
                          "state, so its other components still run");
        }
        std::vector<const Component*> colliders;
        const Component* rigidbody = nullptr;
        const Component* effector = nullptr;
        const Component* composite = nullptr;
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
            case kMonoBehaviour:
                mono_behaviour(component, ctx);
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
        return out;
    }

    static std::string component_why(std::string_view type) {
        if (type == "Canvas" || type == "CanvasRenderer" || type == "CanvasGroup") {
            return std::string(kUiWhy);
        }
        if (type == "Animator" || type == "Animation") {
            return "Unity animation converts in a later import (on the roadmap)";
        }
        if (type == "SortingGroup") {
            return "sprites under it keep their own z_index";
        }
        if (type == "ParticleSystem" || type == "ParticleSystemRenderer") {
            return "Unity particle systems don't convert; CPUParticles2D is Godot's";
        }
        if (type == "MeshRenderer" || type == "MeshFilter" || type == "SkinnedMeshRenderer" ||
            type == "Light" || type == "BoxCollider" || type == "SphereCollider" ||
            type == "CapsuleCollider" || type == "MeshCollider" || type == "Rigidbody") {
            return "3D comes with milestone 7";
        }
        if (type.find("Joint2D") != std::string_view::npos) {
            return "2D joints from Unity convert in a later import";
        }
        return {};
    }

    json::Value transform_2d(const Node& transform, const std::string& where) {
        json::Value out;
        out["position"] = pair(transform.number_at("m_LocalPosition.x") * ppu_,
                               -transform.number_at("m_LocalPosition.y") * ppu_);
        const double qx = transform.number_at("m_LocalRotation.x");
        const double qy = transform.number_at("m_LocalRotation.y");
        const double qz = transform.number_at("m_LocalRotation.z");
        const double qw = transform.number_at("m_LocalRotation.w", 1.0);
        if (std::abs(qx) > 1e-6 || std::abs(qy) > 1e-6) {
            problems_.add("rotation about x or y", where,
                          "2D objects turn about z only; the rest is dropped");
        }
        // Unity's angles go counterclockwise with y up; SlashEngine's clockwise with y down.
        const double angle =
            std::atan2(2.0 * ((qw * qz) + (qx * qy)), 1.0 - (2.0 * ((qy * qy) + (qz * qz))));
        out["rotation"] = tidy_angle(-angle);
        out["scale"] = json::Array{transform.number_at("m_LocalScale.x", 1.0),
                                   transform.number_at("m_LocalScale.y", 1.0)};
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
            problems_.add("perspective camera", ctx.where, "3D comes with milestone 7");
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

    void audio_source(const Component& component, const Context& ctx, Converted& out) {
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
        if (spatial > 0.0) {
            player["max_distance"] = tidy(body.number_at("MaxDistance", 500.0) * ppu_);
            if (body.integer_at("rolloffMode", 0) != 1) {
                problems_.add("audio rolloff", ctx.where,
                              "sounds fade linearly to max_distance (attenuation 1)");
            }
            out.components["AudioStreamPlayer2D"] = std::move(player);
        } else {
            out.components["AudioStreamPlayer"] = std::move(player);
        }
        if (const auto group = ref_at(body, "OutputAudioMixerGroup"); group && !group->null()) {
            problems_.add("audio mixer group", ctx.where,
                          "audio mixers convert in a later import; the sound plays on Master");
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

    void mono_behaviour(const Component& component, const Context& ctx) {
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
                                  std::string(known.why));
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
    // sprite's physics shape, 2 the cell) and a color.
    struct TileLook {
        std::int64_t collider = 1;
        std::array<double, 4> color{1.0, 1.0, 1.0, 1.0};

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
        std::map<std::int64_t, json::Array> collision; // polygons by collider type
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
        std::optional<TilePhysics> physics;
        std::map<std::string, json::Value> sources; // atlas sources by name, without their tiles
        std::map<std::string, TileGrid> grids;      // by "<guid> <width>x<height>"
        std::map<std::pair<std::string, std::int64_t>, TileOut> tiles; // by texture GUID, file ID
        std::set<std::string> names;                                   // the tiles'
    };

    TileSetOut& tile_set_for(std::array<std::int64_t, 2> cell,
                             const std::optional<TilePhysics>& physics) {
        for (TileSetOut& set : tile_sets_) {
            if (set.cell == cell && (!physics || !set.physics || *set.physics == *physics)) {
                if (physics && !set.physics) {
                    set.physics = physics;
                }
                return set;
            }
        }
        TileSetOut& set = tile_sets_.emplace_back();
        set.cell = cell;
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

    // A tile's collision polygons for a Unity collider type, in pixels around its middle.
    json::Array tile_polygons(const TileSetOut& set, const Sprite& sprite, std::int64_t collider,
                              const std::string& where) {
        json::Array out;
        std::vector<Outline> shapes;
        if (collider == 2) {
            const double x = static_cast<double>(set.cell[0]) / 2.0;
            const double y = static_cast<double>(set.cell[1]) / 2.0;
            shapes.push_back({{-x, y}, {x, y}, {x, -y}, {-x, -y}});
        } else if (collider == 1) {
            const double scale = ppu_ / sprite.ppu;
            for (const Outline& outline : sprite.shape) {
                Outline scaled;
                for (const auto& [x, y] : outline) {
                    scaled.push_back({x * scale, y * scale});
                }
                shapes.push_back(std::move(scaled));
            }
            if (shapes.empty() && sprite.fallback_shape) {
                problems_.add("tile physics shape from the sprite's alpha", where,
                              "Unity makes it from the pixels; the tile's polygon is the sprite's "
                              "rectangle");
                const double x = sprite.width * scale / 2.0;
                const double y = sprite.height * scale / 2.0;
                shapes.push_back({{-x, y}, {x, y}, {x, -y}, {-x, -y}});
            }
        }
        for (const Outline& outline : shapes) {
            std::vector<Vec2> points;
            json::Array polygon;
            for (const auto& [x, y] : outline) {
                // Unity's y points up.
                points.push_back({static_cast<float>(x), static_cast<float>(-y)});
                polygon.push_back(pair(tidy(x), tidy(-y)));
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
    std::pair<const TileOut*, int> tile_for(TileSetOut& set, const ObjectRef& ref,
                                            const Sprite& sprite, const TileLook& look,
                                            const std::string& where) {
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
            tile.base = TileLook{.collider = look.collider};
            if (std::abs(sprite.pivot_x - 0.5) > 1e-6 || std::abs(sprite.pivot_y - 0.5) > 1e-6) {
                problems_.add("tile sprite pivot off its middle", where,
                              "tiles draw centered on their cells");
            }
            if (std::abs(sprite.ppu - ppu_) > 1e-6) {
                problems_.add("tile sprite with other pixels per unit", where,
                              "a tile draws at its size in pixels, unscaled");
            }
        }
        if (!tile.collision.contains(look.collider)) {
            tile.collision[look.collider] = tile_polygons(set, sprite, look.collider, where);
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
        json::Value sources = json::Object{};
        for (const auto& [name, source] : set.sources) {
            sources[name] = source;
        }
        bool polygons = false;
        const auto collision = [&](const TileOut& tile, const TileLook& look) {
            const auto found = tile.collision.find(look.collider);
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
            out["name"] = tile.name;
            if (json::Array base = collision(tile, tile.base); !base.empty()) {
                out["collision"] = std::move(base);
            }
            for (const auto& [look, number] : tile.alternatives) {
                json::Value alternative = json::Object{};
                if (look.color != std::array<double, 4>{1.0, 1.0, 1.0, 1.0}) {
                    alternative["modulate"] =
                        json::Array{look.color[0], look.color[1], look.color[2], look.color[3]};
                }
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

    // Whether a tile asset is one of Unity's own Tiles; rule tiles, animated tiles and other
    // scripted tiles aren't.
    bool plain_tile(const ObjectRef& ref) {
        if (const auto known = plain_tiles_.find(ref.guid); known != plain_tiles_.end()) {
            return known->second;
        }
        bool plain = true;
        if (const Asset* asset = asset_of(ref.guid); asset != nullptr) {
            if (auto text = platform::read_file(source_ + asset->path); text) {
                if (auto objects = unity::parse_file(text.value()); objects) {
                    for (const Object& object : objects.value()) {
                        if (object.file_id == ref.file_id) {
                            const auto script = ref_at(object.body, "m_Script");
                            plain = script && script->guid == kTileScriptGuid &&
                                    script->file_id == kTileScriptId;
                        }
                    }
                }
            }
        }
        plain_tiles_[ref.guid] = plain;
        return plain;
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
        if (grid->integer_at("m_CellLayout", 0) != 0) {
            problems_.add("isometric or hexagonal tile map", ctx.where,
                          "only rectangular grids convert; isometric and hexagonal tile maps are "
                          "on the roadmap");
            return;
        }
        if (grid->integer_at("m_CellSwizzle", 0) != 0) {
            problems_.add("grid cell swizzle", ctx.where,
                          "tile maps lie in the xy plane; this one isn't converted");
            return;
        }
        if (grid->number_at("m_CellGap.x") != 0.0 || grid->number_at("m_CellGap.y") != 0.0) {
            problems_.add("grid cell gap", ctx.where, "cells touch");
        }
        const double cell_x = std::abs(grid->number_at("m_CellSize.x", 1.0)) * ppu_;
        const double cell_y = std::abs(grid->number_at("m_CellSize.y", 1.0)) * ppu_;
        const std::array<std::int64_t, 2> cell{std::max<std::int64_t>(std::llround(cell_x), 1),
                                               std::max<std::int64_t>(std::llround(cell_y), 1)};
        if (std::abs(cell_x - static_cast<double>(cell[0])) > 1e-6 ||
            std::abs(cell_y - static_cast<double>(cell[1])) > 1e-6) {
            problems_.add("grid cell size between whole pixels", ctx.where,
                          "tiles are whole pixels; the size is rounded");
        }
        const Node& body = parts.tilemap->body;
        if (std::abs(body.number_at("m_TileAnchor.x", 0.5) - 0.5) > 1e-6 ||
            std::abs(body.number_at("m_TileAnchor.y", 0.5) - 0.5) > 1e-6) {
            problems_.add("tile anchor off the middle of the cell", ctx.where,
                          "tiles draw centered on their cells");
        }
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
        TileSetOut& set = tile_set_for(cell, physics);

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
            if (asset && !asset->null() && !plain_tile(*asset)) {
                problems_.add("scripted tiles (rule tiles, animated tiles)", ctx.where,
                              "each cell keeps the sprite it shows");
            }
            const Node* sprite_ref =
                entry("m_TileSpriteArray", data->integer_at("m_TileSpriteIndex", -1));
            const auto ref = sprite_ref != nullptr ? unity::object_ref(*sprite_ref) : std::nullopt;
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
            TileLook look;
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
            if (matrix != nullptr &&
                (matrix->number_at("e03") != 0.0 || matrix->number_at("e13") != 0.0)) {
                problems_.add("tile offsets", ctx.where, "tiles draw centered on their cells");
            }
            const auto [tile, alternative] = tile_for(set, *ref, *sprite, look, ctx.where);
            if (filter.empty()) {
                filter = sprite->filter;
            }
            // Unity's cells go up from y 0; rows go down.
            const std::int64_t x = item.integer_at("first.x", 0);
            const std::int64_t y = item.integer_at("first.y", 0);
            placed[{x, -y - 1}] = Placed{.tile = tile, .alternative = alternative, .turns = *turns};
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
        json::Value record;
        record["scene"] = file.scene;
        json::Object entities;
        std::function<void(int)> visit = [&](int index) {
            GameObject& go = file.objects[static_cast<std::size_t>(index)];
            const std::string where = file.path + ": " + object_path(file, index);
            record["entities"][object_path(file, index)] = go.entity;
            if (go.removed) {
                entities[go.entity] = nullptr;
                for (const int child : go.children) {
                    visit(child);
                }
                return;
            }
            const Context ctx{
                .file = &file, .index = index, .where = where, .active = active(file, index)};
            Converted converted = convert_object(ctx);
            const Instance* instance =
                go.instance >= 0 ? &file.instances[static_cast<std::size_t>(go.instance)] : nullptr;
            json::Value entity;
            json::Value& transform = converted.components["Transform2D"];
            for (auto& [purpose, components] : converted.added) {
                std::string source;
                const std::string id = added_id(file, go, purpose, source);
                go.added[purpose] = id;
                transform["children"].push_back(id);
                if (!source.empty()) {
                    complete(components, *instance, source);
                }
                json::Value child;
                child["components"] = std::move(components);
                entities[id] = std::move(child);
            }
            for (const int child : go.children) {
                const GameObject& c = file.objects[static_cast<std::size_t>(child)];
                if (!c.removed) {
                    transform["children"].push_back(c.entity);
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
            for (const int child : go.children) {
                visit(child);
            }
        };
        for (const int root : file.roots) {
            visit(root);
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
            const auto errors = world::validate_scene(components_, converted, reader);
            if (errors.empty()) {
                break;
            }
            for (const json::ValidationError& error : errors) {
                problems_.add("invalid value", file.path + ": " + error.path, error.message);
                if (!remove_pointer(converted, error.path)) {
                    converted["entities"] = json::Object{};
                    break;
                }
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

    // Reads the files this import has written so far, for checking scenes that instance them.
    [[nodiscard]] world::FileReader planned_files() const {
        return [this](std::string_view path) -> Result<std::string> {
            for (const auto& [planned, contents] : plan_.files) {
                if (planned == path) {
                    return contents;
                }
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
        if (const auto color = clear_colors_.find(main_source_); color != clear_colors_.end()) {
            project["rendering"]["clear_color"] = color->second;
        }
        if (nearest_textures_ + linear_textures_ > 0) {
            project["rendering"]["texture_filter"] =
                nearest_textures_ > linear_textures_ ? "nearest" : "linear";
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
            text += " The values each entity's component set and the entity ID of every object "
                    "are in " +
                    std::string(kReportFile) + ".";
            task.text = trim_text(text);
            tasks.push_back(std::move(task));
        }
        PortTasks written = write_port_tasks(existing, tasks);
        std::size_t index = 0;
        for (const auto& [script, record] : scripts_) {
            task_of_[script] = written.ids[index++];
        }
        if (!written.file.empty()) {
            add_file(storyboard::file_path("work"), std::move(written.file));
        }
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
            scripts[destination(script)] = item;
            json::Value brief;
            brief["script"] = destination(script);
            brief["task"] = task_of_[script];
            brief["entities"] = static_cast<std::int64_t>(record.uses.size());
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
        out["converted"]["input_actions"] = input_actions_;
        out["converted"]["assets"] = assets_;
        out["scripts"] = std::move(summary);
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
    std::map<std::string, json::Value> clear_colors_; // by scene
    std::set<std::string> loops_written_;
    std::map<std::string, ScriptRecord> scripts_; // by source path
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
    std::map<std::string, bool> plain_tiles_;                                // by tile asset GUID
    std::map<std::string, std::pair<std::string, std::string>> tile_assets_; // tile set, tile
};

} // namespace

Result<ImportPlan> plan_unity(const std::string& source, const std::string& prefix,
                              bool copy_assets, const std::optional<std::string>& existing_project,
                              const std::optional<std::string>& existing_work) {
    UnityImporter importer(source, prefix, copy_assets);
    return importer.run(existing_project, existing_work);
}

} // namespace slash::importer
