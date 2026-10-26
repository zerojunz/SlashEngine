#include "unreal_import.hpp"

#include "common.hpp"

#include "slash/animation/animation.hpp"
#include "slash/importer/unreal.hpp"
#include "slash/platform/file.hpp"
#include "slash/runtime/project.hpp"
#include "slash/scene3d/collision.hpp"
#include "slash/scene3d/model.hpp"
#include "slash/storyboard/storyboard.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/scene.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <numbers>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace slash::importer {
namespace {

namespace fs = std::filesystem;

constexpr std::string_view kReportFile = "import/unreal.json";
// Instances an instanced static mesh (foliage, say) brings as entities of their own, at most.
constexpr std::size_t kMaxInstances = 2048;

using V3 = std::array<double, 3>;
using M3 = std::array<V3, 3>; // rows

constexpr M3 kIdentity{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};

M3 multiply(const M3& a, const M3& b) {
    M3 out{};
    for (std::size_t i = 0; i < 3; ++i) {
        for (std::size_t j = 0; j < 3; ++j) {
            for (std::size_t k = 0; k < 3; ++k) {
                out[i][j] += a[i][k] * b[k][j];
            }
        }
    }
    return out;
}

M3 transpose(const M3& m) {
    M3 out{};
    for (std::size_t i = 0; i < 3; ++i) {
        for (std::size_t j = 0; j < 3; ++j) {
            out[i][j] = m[j][i];
        }
    }
    return out;
}

V3 transform_point(const M3& m, const V3& v) {
    V3 out{};
    for (std::size_t i = 0; i < 3; ++i) {
        out[i] = (m[i][0] * v[0]) + (m[i][1] * v[1]) + (m[i][2] * v[2]);
    }
    return out;
}

double determinant(const M3& m) {
    return (m[0][0] * ((m[1][1] * m[2][2]) - (m[1][2] * m[2][1]))) -
           (m[0][1] * ((m[1][0] * m[2][2]) - (m[1][2] * m[2][0]))) +
           (m[0][2] * ((m[1][0] * m[2][1]) - (m[1][1] * m[2][0])));
}

// A unit quaternion's rotation, acting on column vectors, as FQuat::RotateVector turns them.
M3 quaternion_matrix(const V3& xyz, double w) {
    const double x = xyz[0];
    const double y = xyz[1];
    const double z = xyz[2];
    const double length = std::sqrt((x * x) + (y * y) + (z * z) + (w * w));
    const double s = length > 0.0 ? 2.0 / (length * length) : 0.0;
    return {{{1.0 - (s * ((y * y) + (z * z))), s * ((x * y) - (w * z)), s * ((x * z) + (w * y))},
             {s * ((x * y) + (w * z)), 1.0 - (s * ((x * x) + (z * z))), s * ((y * z) - (w * x))},
             {s * ((x * z) - (w * y)), s * ((y * z) + (w * x)), 1.0 - (s * ((x * x) + (y * y)))}}};
}

double tidy(double value) {
    const double rounded = std::round(value * 1e6) / 1e6;
    return rounded == 0.0 ? 0.0 : rounded;
}

json::Array triple(const V3& v) {
    return json::Array{tidy(v[0]), tidy(v[1]), tidy(v[2])};
}

// --- JSON reading ---

const json::Value* member(const json::Value& value, std::string_view key) {
    return value.is_object() ? value.find(key) : nullptr;
}

double number(const json::Value& value, std::string_view key, double fallback = 0.0) {
    const json::Value* found = member(value, key);
    return found != nullptr && found->is_number() ? found->as_double() : fallback;
}

bool flag(const json::Value& value, std::string_view key, bool fallback = false) {
    const json::Value* found = member(value, key);
    return found != nullptr && found->is_bool() ? found->as_bool() : fallback;
}

std::string text(const json::Value& value, std::string_view key) {
    const json::Value* found = member(value, key);
    return found != nullptr && found->is_string() ? found->as_string() : std::string();
}

V3 vec(const json::Value& value, std::string_view key, V3 fallback = {}) {
    const json::Value* found = member(value, key);
    if (found == nullptr || !found->is_array() || found->as_array().size() < 3) {
        return fallback;
    }
    V3 out{};
    for (std::size_t i = 0; i < 3; ++i) {
        const json::Value& item = found->as_array()[i];
        out[i] = item.is_number() ? item.as_double() : fallback[i];
    }
    return out;
}

const json::Array& items(const json::Value& value, std::string_view key) {
    static const json::Array kNone;
    const json::Value* found = member(value, key);
    return found != nullptr && found->is_array() ? found->as_array() : kNone;
}

// --- Axes ---

// Where Unreal's axes go in the converted world: axis i of the world is sign[i] times Unreal's
// axis[i], and lengths are multiplied by scale. Epic's glTF exporter writes meshes so, and the
// world follows its meshes, so they need no turn of their own.
struct Axes {
    std::array<std::size_t, 3> axis{0, 2, 1};
    V3 sign{1, 1, 1};
    double scale = 0.01;

    [[nodiscard]] M3 matrix() const {
        M3 m{};
        for (std::size_t i = 0; i < 3; ++i) {
            m[i][axis[i]] = sign[i];
        }
        return m;
    }

    [[nodiscard]] std::string describe() const {
        std::string out;
        for (std::size_t i = 0; i < 3; ++i) {
            out += i == 0 ? "" : ", ";
            out += std::string(1, static_cast<char>('x' + i)) + " = " + (sign[i] < 0 ? "-" : "") +
                   std::string(1, static_cast<char>('x' + axis[i]));
        }
        return out;
    }
};

// Every way of mapping Unreal's left-handed axes to right-handed ones, the default first.
std::vector<Axes> candidate_axes() {
    std::vector<Axes> out{Axes{}};
    std::array<std::size_t, 3> order{0, 1, 2};
    do {
        for (int signs = 0; signs < 8; ++signs) {
            Axes axes;
            axes.axis = order;
            for (std::size_t i = 0; i < 3; ++i) {
                axes.sign[i] = (signs & (1 << i)) != 0 ? -1.0 : 1.0;
            }
            if (determinant(axes.matrix()) < 0.0 &&
                (axes.axis != out.front().axis || axes.sign != out.front().sign)) {
                out.push_back(axes);
            }
        }
    } while (std::ranges::next_permutation(order).found);
    return out;
}

// --- Input ---

struct KeyName {
    std::string_view unreal;
    std::string_view godot;
};

constexpr std::array<KeyName, 66> kKeys{{
    {"SpaceBar", "Space"},
    {"Enter", "Enter"},
    {"Escape", "Escape"},
    {"Tab", "Tab"},
    {"BackSpace", "Backspace"},
    {"LeftShift", "Shift"},
    {"RightShift", "Shift"},
    {"LeftControl", "Ctrl"},
    {"RightControl", "Ctrl"},
    {"LeftAlt", "Alt"},
    {"RightAlt", "Alt"},
    {"LeftCommand", "Meta"},
    {"RightCommand", "Meta"},
    {"Up", "Up"},
    {"Down", "Down"},
    {"Left", "Left"},
    {"Right", "Right"},
    {"Insert", "Insert"},
    {"Delete", "Delete"},
    {"Home", "Home"},
    {"End", "End"},
    {"PageUp", "PageUp"},
    {"PageDown", "PageDown"},
    {"CapsLock", "CapsLock"},
    {"NumLock", "NumLock"},
    {"ScrollLock", "ScrollLock"},
    {"Pause", "Pause"},
    {"Zero", "0"},
    {"One", "1"},
    {"Two", "2"},
    {"Three", "3"},
    {"Four", "4"},
    {"Five", "5"},
    {"Six", "6"},
    {"Seven", "7"},
    {"Eight", "8"},
    {"Nine", "9"},
    {"NumPadZero", "Kp 0"},
    {"NumPadOne", "Kp 1"},
    {"NumPadTwo", "Kp 2"},
    {"NumPadThree", "Kp 3"},
    {"NumPadFour", "Kp 4"},
    {"NumPadFive", "Kp 5"},
    {"NumPadSix", "Kp 6"},
    {"NumPadSeven", "Kp 7"},
    {"NumPadEight", "Kp 8"},
    {"NumPadNine", "Kp 9"},
    {"Add", "Kp Add"},
    {"Subtract", "Kp Subtract"},
    {"Multiply", "Kp Multiply"},
    {"Divide", "Kp Divide"},
    {"Decimal", "Kp Period"},
    {"Semicolon", "Semicolon"},
    {"Equals", "Equal"},
    {"Comma", "Comma"},
    {"Hyphen", "Minus"},
    {"Period", "Period"},
    {"Slash", "Slash"},
    {"Tilde", "QuoteLeft"},
    {"LeftBracket", "BracketLeft"},
    {"Backslash", "BackSlash"},
    {"RightBracket", "BracketRight"},
    {"Apostrophe", "Apostrophe"},
    {"Underscore", "UnderScore"},
    {"Colon", "Colon"},
    {"Ampersand", "Ampersand"},
}};

constexpr std::array<KeyName, 7> kMouseButtons{{{"LeftMouseButton", "left"},
                                                {"RightMouseButton", "right"},
                                                {"MiddleMouseButton", "middle"},
                                                {"ThumbMouseButton", "xbutton1"},
                                                {"ThumbMouseButton2", "xbutton2"},
                                                {"MouseScrollUp", "wheel_up"},
                                                {"MouseScrollDown", "wheel_down"}}};

constexpr std::array<KeyName, 16> kPadButtons{{{"Gamepad_FaceButton_Bottom", "a"},
                                               {"Gamepad_FaceButton_Right", "b"},
                                               {"Gamepad_FaceButton_Left", "x"},
                                               {"Gamepad_FaceButton_Top", "y"},
                                               {"Gamepad_LeftShoulder", "left_shoulder"},
                                               {"Gamepad_RightShoulder", "right_shoulder"},
                                               {"Gamepad_Special_Left", "back"},
                                               {"Gamepad_Special_Right", "start"},
                                               {"Gamepad_LeftThumbstick", "left_stick"},
                                               {"Gamepad_RightThumbstick", "right_stick"},
                                               {"Gamepad_DPad_Up", "dpad_up"},
                                               {"Gamepad_DPad_Down", "dpad_down"},
                                               {"Gamepad_DPad_Left", "dpad_left"},
                                               {"Gamepad_DPad_Right", "dpad_right"},
                                               {"Gamepad_Back", "back"},
                                               {"Gamepad_Start", "start"}}};

json::Value key_event(std::string_view key) {
    json::Value binding;
    binding["type"] = "key";
    binding["key"] = key;
    return binding;
}

json::Value mouse_event(std::string_view button) {
    json::Value binding;
    binding["type"] = "mouse_button";
    binding["button"] = button;
    return binding;
}

json::Value pad_event(std::string_view button) {
    json::Value binding;
    binding["type"] = "joypad_button";
    binding["button"] = button;
    return binding;
}

json::Value motion_event(std::string_view axis, int value) {
    json::Value binding;
    binding["type"] = "joypad_motion";
    binding["axis"] = axis;
    binding["axis_value"] = value;
    return binding;
}

// What an Unreal key gives an input action: each way it can push, as Unreal's value (x for a key
// or a one-axis stick, x and y for a two-axis one, y up), and the binding that pushes that way.
struct Push {
    V3 value;
    json::Value binding;
};

std::vector<Push> pushes_of(std::string_view key) {
    std::vector<Push> out;
    const auto press = [&](json::Value binding) {
        out.push_back({.value = {1, 0, 0}, .binding = std::move(binding)});
    };
    if (key.size() == 1 && ((key[0] >= 'A' && key[0] <= 'Z') || (key[0] >= '0' && key[0] <= '9'))) {
        press(key_event(key));
        return out;
    }
    if (key.size() >= 2 && key.size() <= 3 && key[0] == 'F' && parse_int(key.substr(1))) {
        press(key_event(key));
        return out;
    }
    for (const auto& [unreal, godot] : kKeys) {
        if (unreal == key) {
            press(key_event(godot));
            return out;
        }
    }
    for (const auto& [unreal, godot] : kMouseButtons) {
        if (unreal == key) {
            press(mouse_event(godot));
            return out;
        }
    }
    for (const auto& [unreal, godot] : kPadButtons) {
        if (unreal == key) {
            press(pad_event(godot));
            return out;
        }
    }
    // Triggers press as far as they are pulled.
    if (key == "Gamepad_LeftTrigger" || key == "Gamepad_LeftTriggerAxis") {
        press(motion_event("trigger_left", 1));
        return out;
    }
    if (key == "Gamepad_RightTrigger" || key == "Gamepad_RightTriggerAxis") {
        press(motion_event("trigger_right", 1));
        return out;
    }
    // A stick pushed one way, as a button; Godot's sticks have y down.
    for (const std::string_view side : {std::string_view("Left"), std::string_view("Right")}) {
        const std::string prefix = "Gamepad_" + std::string(side) + "Stick_";
        if (!key.starts_with(prefix)) {
            continue;
        }
        const std::string x = side == "Left" ? "left_x" : "right_x";
        const std::string y = side == "Left" ? "left_y" : "right_y";
        const std::string_view way = key.substr(prefix.size());
        if (way == "Up") {
            press(motion_event(y, -1));
        } else if (way == "Down") {
            press(motion_event(y, 1));
        } else if (way == "Left") {
            press(motion_event(x, -1));
        } else if (way == "Right") {
            press(motion_event(x, 1));
        }
        return out;
    }
    // Sticks as axes: x as Unreal's x, y flipped to Godot's.
    const auto stick = [&](std::string_view x_axis, std::string_view y_axis, bool two) {
        out.push_back({.value = {1, 0, 0}, .binding = motion_event(x_axis, 1)});
        out.push_back({.value = {-1, 0, 0}, .binding = motion_event(x_axis, -1)});
        if (two) {
            out.push_back({.value = {0, 1, 0}, .binding = motion_event(y_axis, -1)});
            out.push_back({.value = {0, -1, 0}, .binding = motion_event(y_axis, 1)});
        }
    };
    if (key == "Gamepad_LeftX") {
        stick("left_x", "", false);
    } else if (key == "Gamepad_RightX") {
        stick("right_x", "", false);
    } else if (key == "Gamepad_LeftY") {
        out.push_back({.value = {1, 0, 0}, .binding = motion_event("left_y", -1)});
        out.push_back({.value = {-1, 0, 0}, .binding = motion_event("left_y", 1)});
    } else if (key == "Gamepad_RightY") {
        out.push_back({.value = {1, 0, 0}, .binding = motion_event("right_y", -1)});
        out.push_back({.value = {-1, 0, 0}, .binding = motion_event("right_y", 1)});
    } else if (key == "Gamepad_Left2D") {
        stick("left_x", "left_y", true);
    } else if (key == "Gamepad_Right2D") {
        stick("right_x", "right_y", true);
    }
    return out;
}

// --- C++ classes ---

struct NativeClass {
    std::string name;   // as declared: "AMyCharacter"
    std::string parent; // "ACharacter"
    std::string header; // its path in the Unreal project: "Source/MyGame/MyCharacter.h"
    std::string module; // "MyGame"
    std::vector<std::string> properties;
    std::vector<std::string> functions;
};

bool is_word(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// The identifier the declaration after a UPROPERTY or UFUNCTION macro names: the last word before
// its ';', '=', '{' or '('.
std::string declared_name(std::string_view source, std::size_t from, bool function) {
    // Skip the macro's own parentheses.
    std::size_t at = source.find('(', from);
    int depth = 0;
    for (; at < source.size(); ++at) {
        if (source[at] == '(') {
            ++depth;
        } else if (source[at] == ')' && --depth == 0) {
            ++at;
            break;
        }
    }
    const std::size_t end = function ? source.find('(', at) : source.find_first_of(";={", at);
    if (end == std::string_view::npos) {
        return {};
    }
    std::size_t last = end;
    while (last > at && !is_word(source[last - 1])) {
        --last;
    }
    std::size_t first = last;
    while (first > at && is_word(source[first - 1])) {
        --first;
    }
    return std::string(source.substr(first, last - first));
}

// The UCLASSes a header declares, read as text.
std::vector<NativeClass> native_classes(std::string_view source, const std::string& header,
                                        const std::string& module) {
    std::vector<NativeClass> out;
    for (std::size_t at = source.find("UCLASS("); at != std::string_view::npos;
         at = source.find("UCLASS(", at + 1)) {
        const std::size_t keyword = source.find("class ", at);
        const std::size_t body = source.find('{', keyword);
        if (keyword == std::string_view::npos || body == std::string_view::npos) {
            break;
        }
        // "class MYGAME_API AMyCharacter : public ACharacter"
        std::vector<std::string> words;
        std::string word;
        for (std::size_t i = keyword + 6; i <= body; ++i) {
            if (i < body && is_word(source[i])) {
                word += source[i];
            } else if (!word.empty()) {
                words.push_back(std::move(word));
                word.clear();
            }
        }
        NativeClass cls{.name = {},
                        .parent = {},
                        .header = header,
                        .module = module,
                        .properties = {},
                        .functions = {}};
        const std::string_view head = source.substr(keyword, body - keyword);
        const std::size_t colon = head.find(':');
        for (const auto& name : words) {
            if (name.ends_with("_API") || name == "final") {
                continue;
            }
            if (cls.name.empty()) {
                cls.name = name;
            } else if (colon != std::string_view::npos && name != "public" && name != "protected" &&
                       name != "private") {
                cls.parent = name;
                break;
            }
        }
        // Its members up to the next UCLASS.
        const std::size_t next = source.find("UCLASS(", body);
        const std::string_view members = source.substr(
            body, next == std::string_view::npos ? std::string_view::npos : next - body);
        for (const auto& [macro, list, function] :
             {std::tuple{std::string_view("UPROPERTY("), &cls.properties, false},
              std::tuple{std::string_view("UFUNCTION("), &cls.functions, true}}) {
            for (std::size_t m = members.find(macro); m != std::string_view::npos;
                 m = members.find(macro, m + 1)) {
                std::string name = declared_name(members, m, function);
                if (!name.empty() && !contains(*list, name)) {
                    list->push_back(std::move(name));
                }
            }
        }
        if (!cls.name.empty()) {
            out.push_back(std::move(cls));
        }
        at = body;
    }
    return out;
}

// "/Script/MyGame.MyCharacter" for AMyCharacter in MyGame, as Unreal names classes it reflects.
std::string script_path(const NativeClass& cls) {
    std::string name = cls.name;
    if (name.size() > 1 && (name[0] == 'A' || name[0] == 'U') && name[1] >= 'A' && name[1] <= 'Z') {
        name.erase(0, 1);
    }
    return "/Script/" + cls.module + "." + name;
}

// --- The importer ---

// Actors that hold no game content: the level's settings, the editor's and navigation's own.
constexpr std::array<std::string_view, 16> kSkippedActors{"WorldSettings",
                                                          "DefaultPhysicsVolume",
                                                          "GameplayDebuggerPlayerManager",
                                                          "AbstractNavData",
                                                          "RecastNavMesh",
                                                          "NavigationData",
                                                          "LevelBounds",
                                                          "WorldDataLayers",
                                                          "WorldPartitionMiniMap",
                                                          "LandscapeGizmoActiveActor",
                                                          "ChaosDebugDrawActor",
                                                          "GameNetworkManager",
                                                          "LevelScriptActor",
                                                          "NavigationSystemModuleConfig",
                                                          "WorldPartitionReplay",
                                                          "SceneCapture2DHelper"};

constexpr std::array<std::string_view, 6> kTriggerProfiles{
    "Trigger", "OverlapAll", "OverlapAllDynamic", "OverlapOnlyPawn", "UI", "Spectator"};

struct MeshOut {
    std::string path; // the .glb file in the destination
    bool skeletal = false;
    bool collides = false;  // a component that blocks draws it
    bool simulated = false; // a component that simulates physics draws it
    std::optional<scene3d::Model> model;
    std::string bytes;
};

struct AnimationOut {
    std::string library; // the .glb file in the destination
    std::string name;    // its animation's name in that file
    double length = 0.0;
};

// The last part of an Unreal package's path: /Game/Props/M_Wood is M_Wood.
[[nodiscard]] std::string package_stem(std::string_view package) {
    const std::size_t slash = package.rfind('/');
    return std::string(slash == std::string_view::npos ? package : package.substr(slash + 1));
}

class UnrealImporter {
public:
    UnrealImporter(fs::path project, std::string prefix, bool copy_assets, fs::path work,
                   std::string editor)
        : project_(std::move(project)), root_(project_.parent_path()), prefix_(std::move(prefix)),
          copy_assets_(copy_assets), work_(std::move(work)), editor_option_(std::move(editor)) {
        world::register_builtin_components(components_);
    }

    Result<ImportPlan> run(const Existing& existing) {
        read_configs();
        auto editor = unreal::find_editor(project_, editor_option_);
        if (!editor) {
            return editor.error();
        }
        editor_ = editor.value();
        auto exported = unreal::run_export(editor_, project_, work_);
        if (!exported) {
            return exported.error();
        }
        export_ = std::move(exported.value());
        for (const json::Value& entry : items(export_, "problems")) {
            problems_.add(text(entry, "what"), text(entry, "where"), text(entry, "why"));
        }
        read_meshes();
        detect_axes();
        mark_mesh_uses();
        write_meshes();
        write_sounds();
        read_materials();
        write_animations();
        for (const json::Value& level : items(export_, "levels")) {
            convert_level(level);
        }
        json::Value project = convert_project(existing.project);
        convert_input(project);
        write_project(std::move(project));
        write_tasks(existing.work);
        write_report();
        return std::move(plan_);
    }

private:
    // --- Paths ---

    void add_file(std::string path, std::string contents) {
        plan_.files.emplace_back(std::move(path), std::move(contents));
    }

    // The project path of an Unreal package, with extension: /Game/X to Content/X, /Engine/X to
    // Engine/Content/X, and a plugin's /<Plugin>/X to Plugins/<Plugin>/Content/X.
    [[nodiscard]] std::string content_path(std::string_view package,
                                           std::string_view extension) const {
        std::string_view rest = package;
        if (rest.starts_with('/')) {
            rest.remove_prefix(1);
        }
        const std::size_t slash = rest.find('/');
        const std::string_view mount = rest.substr(0, slash);
        const std::string_view inside =
            slash == std::string_view::npos ? std::string_view() : rest.substr(slash + 1);
        std::string folder;
        if (mount == "Game") {
            folder = "Content/";
        } else if (mount == "Engine") {
            folder = "Engine/Content/";
        } else {
            folder = "Plugins/" + std::string(mount) + "/Content/";
        }
        return prefix_ + folder + std::string(inside) + std::string(extension);
    }

    [[nodiscard]] std::string scene_path(std::string_view level) const {
        return content_path(level, world::kSceneSuffix);
    }

    // --- Settings ---

    void read_configs() {
        const auto read = [&](std::string_view name) {
            auto text = platform::read_file((root_ / "Config" / name).string());
            return text ? unreal::parse_config(text.value()) : unreal::Config{};
        };
        engine_ini_ = read("DefaultEngine.ini");
        game_ini_ = read("DefaultGame.ini");
        input_ini_ = read("DefaultInput.ini");
    }

    // --- Meshes ---

    void read_meshes() {
        const json::Value* meshes = member(export_, "meshes");
        if (meshes == nullptr || !meshes->is_object()) {
            return;
        }
        for (const auto& [package, entry] : meshes->as_object()) {
            MeshOut out;
            out.skeletal = flag(entry, "skeletal");
            const std::string file = text(entry, "file");
            if (file.empty()) {
                continue; // reported by the export
            }
            out.path = content_path(package, ".glb");
            auto bytes = platform::read_file((work_ / file).string());
            if (!bytes) {
                problems_.add("mesh the editor exported but left no file", package,
                              bytes.error().message);
                continue;
            }
            out.bytes = std::move(bytes.value());
            auto model = scene3d::load_model(out.bytes, out.path, no_files());
            if (!model) {
                problems_.add("mesh glTF file that doesn't load", package, model.error().message);
                continue;
            }
            out.model = std::move(model.value());
            meshes_[package] = std::move(out);
        }
    }

    [[nodiscard]] static scene3d::ModelReader no_files() {
        return [](std::string_view path) -> Result<std::string> {
            return make_error(ErrorCode::NotFound, std::string(path) + " isn't in the .glb file");
        };
    }

    // Finds where the glTF exporter put Unreal's axes, by matching each mesh's bounds in its file
    // against the bounds Unreal gave for it; the default unless the meshes say otherwise.
    void detect_axes() {
        const std::vector<Axes> candidates = candidate_axes();
        std::vector<int> votes(candidates.size(), 0);
        std::vector<double> scales;
        const json::Value* meshes = member(export_, "meshes");
        for (const auto& [package, out] : meshes_) {
            const json::Value* entry = meshes != nullptr ? meshes->find(package) : nullptr;
            const json::Value* bounds = entry != nullptr ? member(*entry, "bounds") : nullptr;
            if (bounds == nullptr || !out.model) {
                continue;
            }
            const scene3d::Bounds box = scene3d::bounds(*out.model);
            const V3 size{box.max.x - box.min.x, box.max.y - box.min.y, box.max.z - box.min.z};
            const V3 center{(box.max.x + box.min.x) / 2.0, (box.max.y + box.min.y) / 2.0,
                            (box.max.z + box.min.z) / 2.0};
            const V3 origin = vec(*bounds, "origin");
            const V3 extent = vec(*bounds, "extent");
            const double largest = std::max({extent[0], extent[1], extent[2]});
            const double file_largest = std::max({size[0], size[1], size[2]});
            if (largest <= 0.0 || file_largest <= 0.0) {
                continue;
            }
            const double scale = file_largest / (2.0 * largest);
            scales.push_back(scale);
            const double tolerance = (0.02 * file_largest) + 1e-4;
            for (std::size_t c = 0; c < candidates.size(); ++c) {
                const Axes& axes = candidates[c];
                bool fits = true;
                for (std::size_t i = 0; i < 3 && fits; ++i) {
                    const double want_size = 2.0 * extent[axes.axis[i]] * scale;
                    const double want_center = axes.sign[i] * origin[axes.axis[i]] * scale;
                    fits = std::abs(want_size - size[i]) <= tolerance &&
                           std::abs(want_center - center[i]) <= tolerance;
                }
                votes[c] += fits ? 1 : 0;
            }
        }
        std::size_t best = 0;
        for (std::size_t c = 1; c < candidates.size(); ++c) {
            if (votes[c] > votes[best]) {
                best = c;
            }
        }
        axes_ = candidates[best];
        if (!scales.empty()) {
            std::ranges::sort(scales);
            axes_.scale = scales[scales.size() / 2];
            // Round to what an export scale is written as, so 0.0099998 is 0.01.
            const double digits = std::pow(10.0, std::floor(std::log10(axes_.scale)) - 3.0);
            axes_.scale = std::round(axes_.scale / digits) * digits;
        }
        axes_matrix_ = axes_.matrix();
    }

    [[nodiscard]] static bool blocks(const json::Value& comp) {
        const std::string collision = text(comp, "collision");
        if (collision == "no_collision" || collision.empty()) {
            return false;
        }
        return !contains(kTriggerProfiles, text(comp, "profile"));
    }

    void mark_mesh_uses() {
        for (const json::Value& level : items(export_, "levels")) {
            for (const json::Value& actor : items(level, "actors")) {
                for (const json::Value& comp : items(actor, "components")) {
                    const auto found = meshes_.find(text(comp, "mesh"));
                    if (found == meshes_.end()) {
                        continue;
                    }
                    found->second.collides = found->second.collides || blocks(comp);
                    found->second.simulated = found->second.simulated || flag(comp, "simulate");
                }
            }
        }
    }

    // How Unreal collides with a mesh, as the shapes generate_collision makes: its triangles when
    // it uses its complex collision as simple, else hulls standing for its simple shapes.
    std::string collision_kind(const std::string& package, const MeshOut& mesh) {
        const json::Value* meshes = member(export_, "meshes");
        const json::Value* entry = meshes != nullptr ? meshes->find(package) : nullptr;
        if (entry == nullptr || mesh.skeletal) {
            return {};
        }
        const std::string complexity = text(*entry, "collision_complexity");
        const double simple = number(*entry, "simple_collision");
        const double convex = number(*entry, "convex_collision");
        if (complexity.find("complex_as_simple") != std::string::npos && !mesh.simulated) {
            return "trimesh";
        }
        if (simple <= 0.0) {
            problems_.add("mesh with no simple collision", package,
                          "Unreal's bodies and characters pass through it, and so do these");
            return {};
        }
        if (simple > 1.0 || convex > 1.0) {
            problems_.add("simple collision of several shapes", package,
                          "convex hulls split from the mesh stand for them");
            return "decompose";
        }
        problems_.add("simple collision shape", package, "the mesh's convex hull stands for it");
        return "convex";
    }

    void write_meshes() {
        for (auto& [package, mesh] : meshes_) {
            const std::string kind = mesh.collides ? collision_kind(package, mesh) : std::string();
            if (!kind.empty() && mesh.model.has_value()) {
                if (auto made = scene3d::generate_collision(*mesh.model, kind); !made) {
                    problems_.add("mesh collision that couldn't be made", package,
                                  made.error().message);
                } else if (auto glb = scene3d::write_glb(*mesh.model, mesh.path, no_files()); glb) {
                    mesh.bytes = std::move(glb.value());
                } else {
                    problems_.add("mesh collision that couldn't be written", package,
                                  glb.error().message);
                }
            }
            add_file(mesh.path, std::move(mesh.bytes));
            mesh.bytes.clear();
            ++converted_meshes_;
        }
    }

    // --- Materials ---

    // The materials components draw meshes with in place of the meshes' own: each was exported on
    // a mesh of its own, and its file's first material is it.
    void read_materials() {
        const json::Value* materials = member(export_, "materials");
        if (materials == nullptr || !materials->is_object()) {
            return;
        }
        for (const auto& [package, entry] : materials->as_object()) {
            const std::string file = text(entry, "file");
            if (file.empty()) {
                continue; // reported by the export
            }
            auto bytes = platform::read_file((work_ / file).string());
            if (!bytes) {
                problems_.add("material the editor exported but left no file", package,
                              bytes.error().message);
                continue;
            }
            auto model =
                scene3d::load_model(bytes.value(), content_path(package, ".glb"), no_files());
            if (!model) {
                problems_.add("material glTF file that doesn't load", package,
                              model.error().message);
                continue;
            }
            if (model.value().materials.empty()) {
                problems_.add("material glTF file that doesn't load", package,
                              "it holds no material");
                continue;
            }
            materials_[package] = std::move(model.value());
        }
    }

    // The model material a mesh's material slot draws with: the one named as the slot's material,
    // else, when the file has one material a slot, the slot's own; -1 for none.
    [[nodiscard]] static int slot_material(const scene3d::Model& model,
                                           const std::vector<std::string>& slots,
                                           std::size_t slot) {
        if (slot < slots.size() && !slots[slot].empty()) {
            const std::string name = package_stem(slots[slot]);
            for (std::size_t i = 0; i < model.materials.size(); ++i) {
                if (model.materials[i].name == name) {
                    return static_cast<int>(i);
                }
            }
        }
        if ((slots.empty() || slots.size() == model.materials.size()) &&
            slot < model.materials.size()) {
            return static_cast<int>(slot);
        }
        return -1;
    }

    static void replace_material(scene3d::Model& model, int target, const scene3d::Model& from) {
        scene3d::ModelMaterial material = from.materials.front();
        const int offset = static_cast<int>(model.images.size());
        for (int* image :
             {&material.albedo_image, &material.metallic_image, &material.roughness_image,
              &material.normal_image, &material.emission_image}) {
            if (*image >= 0) {
                *image += offset;
            }
        }
        model.images.insert(model.images.end(), from.images.begin(), from.images.end());
        model.materials[static_cast<std::size_t>(target)] = std::move(material);
    }

    // FNV-1a, for names that must be short and the same on every run.
    [[nodiscard]] static std::string short_hash(std::string_view text) {
        std::uint64_t hash = 14695981039346656037ULL;
        for (const char c : text) {
            hash = (hash ^ static_cast<unsigned char>(c)) * 1099511628211ULL;
        }
        static constexpr std::string_view kDigits = "0123456789abcdef";
        std::string out;
        for (int shift = 60; shift >= 32; shift -= 4) {
            out += kDigits[(hash >> static_cast<unsigned>(shift)) & 0xFU];
        }
        return out;
    }

    // The mesh a component draws, with its override materials in place of the mesh's own in a
    // .glb file of its own beside the mesh's (a MeshInstance3D has no per-surface materials, as
    // Godot's has no surface overrides in a scene file); the mesh's own file when none applies.
    std::string mesh_with_overrides(const std::string& package, const MeshOut& mesh,
                                    const json::Value& comp, const std::string& where) {
        const json::Value* overrides = member(comp, "override_materials");
        if (overrides == nullptr || !overrides->is_array() || !mesh.model) {
            return mesh.path;
        }
        const json::Value* meshes = member(export_, "meshes");
        const json::Value* entry = meshes != nullptr ? meshes->find(package) : nullptr;
        std::vector<std::string> slots;
        if (entry != nullptr) {
            for (const json::Value& slot : items(*entry, "materials")) {
                slots.push_back(slot.is_string() ? slot.as_string() : std::string());
            }
        }
        scene3d::Model model = *mesh.model;
        std::string key = package;
        std::vector<std::string> names;
        const json::Array& list = overrides->as_array();
        for (std::size_t slot = 0; slot < list.size(); ++slot) {
            const std::string material = list[slot].is_string() ? list[slot].as_string() : "";
            const auto found = materials_.find(material);
            if (material.empty() || found == materials_.end()) {
                continue; // the slot keeps its own, or the export reported why
            }
            const int target = slot_material(model, slots, slot);
            if (target < 0) {
                problems_.add("material override of a slot the mesh's file lacks", where,
                              "slot " + std::to_string(slot) + " keeps the mesh's material");
                continue;
            }
            replace_material(model, target, found->second);
            names.push_back(package_stem(material));
            key += "|" + std::to_string(slot) + "=" + material;
        }
        if (names.empty()) {
            return mesh.path;
        }
        if (const auto known = variants_.find(key); known != variants_.end()) {
            return known->second;
        }
        const std::string base = mesh.path.substr(0, mesh.path.size() - 4); // less ".glb"
        std::string path = base + "_" + join(names, "_") + ".glb";
        if (path.size() - base.size() > 64 || variant_paths_.contains(path)) {
            path = base + "_" + short_hash(key) + ".glb";
        }
        auto glb = scene3d::write_glb(model, path, no_files());
        if (!glb) {
            problems_.add("mesh with override materials that couldn't be written", where,
                          glb.error().message);
            return mesh.path;
        }
        add_file(path, std::move(glb.value()));
        variants_[key] = path;
        variant_paths_.insert(path);
        ++converted_variants_;
        return path;
    }

    // --- Animations ---

    // Animation sequences, each exported on its skeleton, written where Unreal keeps them so an
    // AnimationPlayer plays them on a skeletal mesh by its bones' names.
    void write_animations() {
        const json::Value* animations = member(export_, "animations");
        if (animations == nullptr || !animations->is_object()) {
            return;
        }
        for (const auto& [package, entry] : animations->as_object()) {
            const std::string file = text(entry, "file");
            if (file.empty()) {
                continue; // reported by the export
            }
            auto bytes = platform::read_file((work_ / file).string());
            if (!bytes) {
                problems_.add("animation the editor exported but left no file", package,
                              bytes.error().message);
                continue;
            }
            AnimationOut out;
            out.library = content_path(package, ".glb");
            auto model = scene3d::load_model(bytes.value(), out.library, no_files());
            if (!model) {
                problems_.add("animation glTF file that doesn't load", package,
                              model.error().message);
                continue;
            }
            if (model.value().animations.empty()) {
                problems_.add("animation glTF file that doesn't load", package,
                              "it holds no animation");
                continue;
            }
            out.name = model.value().animations.front().name;
            out.length = model.value().animations.front().length;
            add_file(out.library, std::move(bytes.value()));
            animations_[package] = std::move(out);
            ++converted_animations_;
        }
    }

    // --- Sounds ---

    void write_sounds() {
        const json::Value* sounds = member(export_, "sounds");
        if (sounds == nullptr || !sounds->is_object()) {
            return;
        }
        for (const auto& [package, entry] : sounds->as_object()) {
            const std::string file = text(entry, "file");
            if (file.empty()) {
                continue;
            }
            auto bytes = platform::read_file((work_ / file).string());
            if (!bytes) {
                continue;
            }
            const std::string wav = content_path(package, ".wav");
            add_file(wav, std::move(bytes.value()));
            std::string stream = wav;
            if (flag(entry, "looping")) {
                json::Value settings;
                settings["source"] = wav;
                settings["loop_mode"] = "forward";
                stream = content_path(package, ".audio.json");
                add_file(stream, json::write(settings) + "\n");
            }
            sounds_[package] = stream;
            ++converted_sounds_;
        }
    }

    // --- Transforms ---

    // The turn a camera or light needs: Unreal's look down their +x with +z up, Godot's down
    // their -z with +y up.
    [[nodiscard]] M3 facing() const {
        const M3& m = axes_matrix_;
        M3 out{};
        for (std::size_t i = 0; i < 3; ++i) {
            out[i] = {m[i][1], m[i][2], -m[i][0]};
        }
        return out;
    }

    // An Unreal transform ({location, rotation, scale}) as a Transform3D, under a parent turned
    // by parent_turn (a turn of its own made for a camera or light) and itself turned by turn.
    [[nodiscard]] json::Value transform_3d(const json::Value& transform, const M3& parent_turn,
                                           const M3& turn, V3 offset = {}) const {
        const M3& m = axes_matrix_;
        const M3 mt = transpose(m);
        V3 location = vec(transform, "location");
        for (std::size_t i = 0; i < 3; ++i) {
            location[i] += offset[i];
        }
        const json::Value* rotation = member(transform, "rotation");
        V3 q{};
        double w = 1.0;
        if (rotation != nullptr && rotation->is_array() && rotation->as_array().size() == 4) {
            const json::Array& r = rotation->as_array();
            q = {r[0].as_double(), r[1].as_double(), r[2].as_double()};
            w = r[3].as_double();
        }
        const V3 scale = vec(transform, "scale", {1, 1, 1});
        const M3 unturn = transpose(parent_turn);
        V3 position = transform_point(m, location);
        for (double& value : position) {
            value *= axes_.scale;
        }
        position = transform_point(unturn, position);
        M3 scaled = quaternion_matrix(q, w);
        for (std::size_t i = 0; i < 3; ++i) {
            for (std::size_t j = 0; j < 3; ++j) {
                scaled[i][j] *= scale[j];
            }
        }
        const M3 basis = multiply(multiply(unturn, multiply(multiply(m, scaled), mt)), turn);
        // Columns' lengths are the scale; the rest is the turn.
        V3 out_scale{};
        M3 turned = basis;
        for (std::size_t j = 0; j < 3; ++j) {
            const double length =
                std::sqrt((basis[0][j] * basis[0][j]) + (basis[1][j] * basis[1][j]) +
                          (basis[2][j] * basis[2][j]));
            out_scale[j] = length;
            for (std::size_t i = 0; i < 3; ++i) {
                turned[i][j] = length > 0.0 ? basis[i][j] / length : kIdentity[i][j];
            }
        }
        if (determinant(turned) < 0.0) {
            out_scale[0] = -out_scale[0];
            for (std::size_t i = 0; i < 3; ++i) {
                turned[i][0] = -turned[i][0];
            }
        }
        json::Value out = json::Object{};
        if (position != V3{}) {
            out["position"] = triple(position);
        }
        const std::array<double, 3> euler = basis_euler(turned);
        if (euler != std::array<double, 3>{}) {
            out["rotation"] = triple(euler);
        }
        if (std::abs(out_scale[0] - 1.0) > 1e-6 || std::abs(out_scale[1] - 1.0) > 1e-6 ||
            std::abs(out_scale[2] - 1.0) > 1e-6) {
            out["scale"] = triple(out_scale);
        }
        return out;
    }

    // A size along Unreal's axes (an extent, say) as one along the world's.
    [[nodiscard]] V3 size_of(const V3& size) const {
        V3 out{};
        for (std::size_t i = 0; i < 3; ++i) {
            out[i] = std::abs(size[axes_.axis[i]]) * axes_.scale;
        }
        return out;
    }

    // --- Levels ---

    struct Entity {
        json::Object components;
        std::vector<std::string> children;
        M3 turn = kIdentity;
        bool has_body = false;
    };

    struct Level {
        std::string path;  // the level's package
        std::string scene; // its scene file
        std::set<std::string> used;
        std::map<std::string, Entity> entities;
        std::vector<std::string> roots;
        // Where each actor's components went: actor name, then component name, to entity.
        std::map<std::string, std::map<std::string, std::string>> placed;
        std::map<std::string, std::string> actor_ids;
        json::Value record;
        json::Object environment;
        bool sky = false;
        bool camera = false;
        bool current_camera = false;
        std::string player_start;
        std::string possessed; // the first pawn a player possesses as the level starts
        double sky_light = -1.0;
        json::Array sky_color;
        // Entities left out when nothing but a transform is left of them: editor-only
        // components, and actors the environment took in.
        std::set<std::string> droppable;
    };

    static std::string take(std::set<std::string>& used, std::string id) {
        if (id.empty()) {
            id = "actor";
        }
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

    void convert_level(const json::Value& data) {
        Level level;
        level.path = text(data, "path");
        level.scene = scene_path(level.path);
        // Names first, so attachments to later actors resolve.
        for (const json::Value& actor : items(data, "actors")) {
            const std::string cls = text(actor, "class");
            if (skipped(actor, level)) {
                continue;
            }
            const std::string label =
                text(actor, "label").empty() ? text(actor, "name") : text(actor, "label");
            const std::string id = take(level.used, snake_case(label));
            level.actor_ids[text(actor, "name")] = id;
            auto& placed = level.placed[text(actor, "name")];
            const std::string root = text(actor, "root");
            for (const json::Value& comp : items(actor, "components")) {
                if (member(comp, "transform") == nullptr) {
                    continue; // not a scene component
                }
                const std::string name = text(comp, "name");
                placed[name] =
                    name == root ? id : take(level.used, id + "_" + snake_case(name, 40));
            }
            if (cls == "PlayerStart" && level.player_start.empty()) {
                level.player_start = id;
            }
            if (const std::string possess = text(actor, "auto_possess");
                !possess.empty() && possess != "disabled" && level.possessed.empty()) {
                level.possessed = id;
            }
        }
        for (const json::Value& actor : items(data, "actors")) {
            if (level.actor_ids.contains(text(actor, "name"))) {
                convert_actor(actor, level);
            }
        }
        add_camera(level);
        write_environment(level);
        write_scene(level, data);
    }

    // Whether an actor stays out of the scene: reported when it has content that doesn't convert.
    bool skipped(const json::Value& actor, Level& level) {
        const std::string cls = text(actor, "class");
        const std::string where = level.path + ": " + text(actor, "label");
        if (contains(kSkippedActors, cls)) {
            return true;
        }
        if (cls == "Brush") {
            // The level's builder brush is the editor's; any other is BSP geometry.
            if (text(actor, "label") != "Brush" && !text(actor, "name").starts_with("Brush")) {
                problems_.add("BSP brush geometry", where,
                              "convert it to a static mesh in Unreal (Create Static Mesh) first");
            }
            return true;
        }
        if (cls.starts_with("Landscape")) {
            problems_.add("landscape", where, "landscapes don't convert yet");
            return true;
        }
        if (cls.find("Niagara") != std::string::npos || cls == "Emitter") {
            problems_.add("particle system " + cls, where, "particles don't convert yet");
            return true;
        }
        return false;
    }

    std::string& entity_of(Level& level, const std::string& actor, const std::string& comp) {
        return level.placed[actor][comp];
    }

    void convert_actor(const json::Value& actor, Level& level) {
        const std::string name = text(actor, "name");
        const std::string id = level.actor_ids[name];
        const std::string where = level.path + ": " + text(actor, "label");
        const std::string cls = text(actor, "class");
        json::Value record;
        record["class"] = text(actor, "class_path");
        if (!items(actor, "tags").empty()) {
            record["tags"] = json::Array(items(actor, "tags"));
        }
        level.record["actors"][id] = std::move(record);
        if (member(actor, "bounds") != nullptr) {
            convert_volume(actor, level, id, where);
            return;
        }
        // The actor's own entity: its root component, or an empty one.
        const std::string root = text(actor, "root");
        std::map<std::string, const json::Value*> components;
        std::vector<const json::Value*> others;
        for (const json::Value& comp : items(actor, "components")) {
            if (member(comp, "transform") != nullptr) {
                components[text(comp, "name")] = &comp;
            } else {
                others.push_back(&comp);
            }
        }
        if (!components.contains(root)) {
            Entity& entity = level.entities[id];
            entity.components["Transform3D"] =
                transform_3d(*member(actor, "transform"), kIdentity, kIdentity);
            level.roots.push_back(id);
        }
        // Each scene component, parents first.
        std::set<std::string> done;
        const std::function<void(const std::string&)> place = [&](const std::string& comp_name) {
            if (done.contains(comp_name)) {
                return;
            }
            done.insert(comp_name);
            const json::Value& comp = *components[comp_name];
            const std::string parent = text(comp, "parent");
            const bool is_root = comp_name == root;
            if (!is_root && !parent.empty() && components.contains(parent)) {
                place(parent);
            }
            const std::string& entity_id = entity_of(level, name, comp_name);
            // Where it hangs: an attached actor's root under its parent's component, others under
            // their parent component, or the actor's root.
            std::string parent_entity;
            const json::Value* transform = member(comp, "transform");
            if (is_root) {
                const std::string parent_actor = text(actor, "parent");
                if (!parent_actor.empty() && level.actor_ids.contains(parent_actor)) {
                    const std::string socket = text(actor, "parent_component");
                    const auto& theirs = level.placed[parent_actor];
                    const auto found = theirs.find(socket);
                    parent_entity =
                        found != theirs.end() ? found->second : level.actor_ids[parent_actor];
                } else {
                    transform = member(actor, "transform");
                }
            } else {
                parent_entity = components.contains(parent) ? entity_of(level, name, parent) : id;
            }
            Entity& entity = level.entities[entity_id];
            const bool turns = member(comp, "camera") != nullptr ||
                               (member(comp, "light") != nullptr &&
                                text(*member(comp, "light"), "type") != "point" &&
                                text(*member(comp, "light"), "type") != "sky");
            if (turns) {
                entity.turn = facing();
            }
            const M3 parent_turn =
                parent_entity.empty() ? kIdentity : level.entities[parent_entity].turn;
            // A spring arm holds what hangs from it at the end of its arm.
            V3 offset{};
            if (!is_root && components.contains(parent)) {
                if (const json::Value* arm = member(*components[parent], "spring_arm")) {
                    const V3 socket = vec(*arm, "socket_offset");
                    offset = {socket[0] - number(*arm, "length", 300.0), socket[1], socket[2]};
                    problems_.add("spring arm", where,
                                  "what hangs from it stays at the end of its arm; it doesn't "
                                  "pull in at walls");
                }
            }
            json::Value placed = transform_3d(*transform, parent_turn, entity.turn, offset);
            if (!flag(comp, "visible", true) || (is_root && !flag(actor, "visible", true))) {
                placed["visible"] = false;
            }
            entity.components["Transform3D"] = std::move(placed);
            if (parent_entity.empty()) {
                level.roots.push_back(entity_id);
            } else {
                level.entities[parent_entity].children.push_back(entity_id);
            }
            convert_component(comp, actor, level, entity_id, where);
        };
        if (components.contains(root)) {
            place(root);
        }
        for (const auto& [comp_name, comp] : components) {
            place(comp_name);
        }
        for (const json::Value* comp : others) {
            convert_other(*comp, actor, level, id, where);
        }
        if (cls == "SkyAtmosphere" || cls == "SkyLight" || cls == "ExponentialHeightFog") {
            level.droppable.insert(id);
        }
        if (cls == "PlayerStart") {
            // Its capsule only shows where a player fits.
            level.entities[id].components.erase("CollisionShape3D");
            level.entities[id].components.erase("StaticBody3D");
        }
    }

    void convert_volume(const json::Value& actor, Level& level, const std::string& id,
                        const std::string& where) {
        const json::Value& bounds = *member(actor, "bounds");
        const std::string cls = text(actor, "class");
        Entity& entity = level.entities[id];
        json::Value place;
        place["location"] = triple(vec(bounds, "origin"));
        entity.components["Transform3D"] = transform_3d(place, kIdentity, kIdentity);
        level.roots.push_back(id);
        if (cls == "PostProcessVolume" || cls == "NavMeshBoundsVolume" ||
            cls == "LightmassImportanceVolume" || cls == "LevelStreamingVolume" ||
            cls == "CullDistanceVolume" || cls == "AudioVolume") {
            problems_.add(cls, where,
                          cls == "NavMeshBoundsVolume"
                              ? "bake navigation with a NavigationRegion3D and navigation.bake"
                              : "it has no counterpart; the entity marks where it was");
            return;
        }
        json::Value shape;
        shape["shape"] = "box";
        const V3 extent = vec(bounds, "extent");
        shape["size"] = triple(size_of({extent[0] * 2.0, extent[1] * 2.0, extent[2] * 2.0}));
        entity.components["CollisionShape3D"] = std::move(shape);
        if (cls == "BlockingVolume") {
            entity.components["StaticBody3D"] = json::Object{};
        } else {
            entity.components["Area3D"] = json::Object{};
            if (cls != "TriggerVolume" && cls != "PhysicsVolume") {
                problems_.add(cls, where, "an Area3D marks it; what it does is the game's code");
            }
        }
        const json::Value* rotation = member(*member(actor, "transform"), "rotation");
        if (rotation != nullptr && rotation->is_array() && rotation->as_array().size() == 4 &&
            std::abs(rotation->as_array()[3].as_double()) < 0.99999) {
            problems_.add("turned volume", where,
                          "its box is the volume's bounds in the world, not turned with it");
        }
    }

    // The body a shape or a mesh's collision joins: the nearest entity at or above it in the same
    // actor with one.
    [[nodiscard]] static bool body_above(Level& level, const std::string& actor_id,
                                         const std::string& entity_id) {
        if (level.entities[entity_id].has_body) {
            return true;
        }
        const Entity& root = level.entities[actor_id];
        return root.has_body && contains(root.children, entity_id);
    }

    void rigid_body(const json::Value& comp, Entity& entity) {
        json::Value body;
        if (const double mass = number(comp, "mass"); mass > 0.0) {
            body["mass"] = tidy(mass);
        }
        if (!flag(comp, "gravity", true)) {
            body["gravity_scale"] = 0.0;
        }
        body["linear_damp"] = tidy(number(comp, "linear_damping", 0.01));
        body["angular_damp"] = tidy(number(comp, "angular_damping", 0.0));
        body["angular_damp_mode"] = "replace";
        body["linear_damp_mode"] = "replace";
        entity.components["RigidBody3D"] = std::move(body);
        entity.has_body = true;
    }

    // A skeletal mesh component's animation, as Godot's AnimationPlayer playing it on the entity
    // that draws the mesh: a sequence played on its own node (Unreal's "Use Animation Asset")
    // plays from where the component left it, looping through an .anim.json library that takes it
    // from the sequence's file; an Animation Blueprint is left for its task.
    void convert_animation(const json::Value& comp, Entity& entity, const std::string& where) {
        const json::Value* animation = member(comp, "animation");
        if (animation == nullptr) {
            return;
        }
        const std::string mode = text(*animation, "mode");
        if (mode == "blueprint") {
            problems_.add("Animation Blueprint", where,
                          "the mesh stands in its bind pose until its task's AnimationTree "
                          "plays it");
            return;
        }
        if (mode != "single_node") {
            problems_.add("skeletal mesh animation mode " + mode, where,
                          "the mesh stands in its bind pose");
            return;
        }
        const auto found = animations_.find(text(*animation, "animation"));
        if (found == animations_.end()) {
            return; // none set, or the export reported why
        }
        const AnimationOut& out = found->second;
        json::Value player;
        player["animation"] = out.name;
        if (flag(*animation, "looping", true)) {
            const std::string library = out.library.substr(0, out.library.size() - 4) +
                                        std::string(animation::kAnimationSuffix);
            if (!looped_.contains(library)) {
                json::Value file;
                file["format"] = 1;
                json::Value& entry = file["animations"][out.name];
                entry["from"]["model"] = out.library;
                entry["from"]["animation"] = out.name;
                entry["loop"] = "linear";
                add_file(library, json::write(file) + "\n");
                looped_.insert(library);
            }
            player["library"] = library;
        } else {
            player["library"] = out.library;
        }
        if (!flag(*animation, "playing", true)) {
            player["playing"] = false;
        }
        if (const double rate = number(*animation, "play_rate", 1.0); rate != 1.0) {
            player["speed_scale"] = tidy(rate);
        }
        if (const double position = number(*animation, "position"); position > 0.0) {
            player["time"] = tidy(position);
        }
        entity.components["AnimationPlayer"] = std::move(player);
    }

    void convert_component(const json::Value& comp, const json::Value& actor, Level& level,
                           const std::string& id, const std::string& where) {
        Entity& entity = level.entities[id];
        const std::string actor_id = level.actor_ids[text(actor, "name")];
        const std::string cls = text(comp, "class");
        const bool character = has_character(actor) && id == actor_id;
        if (flag(comp, "simulate")) {
            rigid_body(comp, entity);
        }
        if (const std::string mesh = text(comp, "mesh"); !mesh.empty()) {
            const auto found = meshes_.find(mesh);
            if (found == meshes_.end()) {
                return; // reported when exported
            }
            json::Value instance;
            instance["mesh"] =
                mesh_with_overrides(mesh, found->second, comp, where + "." + text(comp, "name"));
            if (!flag(comp, "cast_shadow", true)) {
                instance["cast_shadow"] = "off";
            }
            if (found->second.skeletal) {
                convert_animation(comp, entity, where);
            }
            if (member(comp, "instances") != nullptr) {
                std::size_t count = 0;
                for (const json::Value& placed : items(comp, "instances")) {
                    if (++count > kMaxInstances) {
                        problems_.add("instances past " + std::to_string(kMaxInstances), where,
                                      "only the first come along");
                        break;
                    }
                    const std::string child = take(level.used, id + "_" + std::to_string(count));
                    Entity& one = level.entities[child];
                    one.components["Transform3D"] = transform_3d(placed, entity.turn, kIdentity);
                    one.components["MeshInstance3D"] = instance;
                    level.entities[id].children.push_back(child);
                }
            } else {
                entity.components["MeshInstance3D"] = std::move(instance);
            }
            if (found->second.collides && blocks(comp) && !flag(comp, "simulate") &&
                text(comp, "mobility") == "movable" && !body_above(level, actor_id, id)) {
                // Godot moves a static body that moves as an animatable one.
                entity.components["AnimatableBody3D"] = json::Object{};
                entity.has_body = true;
            }
            return;
        }
        if (const json::Value* shape = member(comp, "shape")) {
            convert_shape(comp, *shape, level, actor_id, id, character, where);
            return;
        }
        if (const json::Value* camera = member(comp, "camera")) {
            json::Value out;
            out["fov"] = tidy(number(*camera, "fov", 90.0));
            out["keep_aspect"] = "width"; // Unreal's field of view is across
            if (text(*camera, "projection") == "orthographic") {
                out["projection"] = "orthogonal";
                out["size"] = tidy(number(*camera, "ortho_width", 512.0) * axes_.scale);
            }
            const std::string possess = text(actor, "auto_possess");
            if (!level.current_camera && !possess.empty() && possess != "disabled") {
                out["current"] = true;
                level.current_camera = true;
            }
            entity.components["Camera3D"] = std::move(out);
            level.camera = true;
            return;
        }
        if (const json::Value* light = member(comp, "light")) {
            convert_light(*light, entity, level, where);
            return;
        }
        if (const json::Value* audio = member(comp, "audio")) {
            convert_audio(*audio, entity, where);
            return;
        }
        if (const json::Value* fog = member(comp, "fog")) {
            const V3 at =
                transform_point(axes_matrix_, vec(*member(comp, "transform"), "location"));
            level.environment["fog_enabled"] = true;
            level.environment["fog_density"] = tidy(number(*fog, "density", 0.02) / 10.0);
            const V3 color = vec(*fog, "color", {0.447, 0.638, 1.0});
            level.environment["fog_light_color"] =
                json::Array{tidy(color[0]), tidy(color[1]), tidy(color[2]), 1.0};
            level.environment["fog_height"] = tidy(at[1] * axes_.scale);
            level.environment["fog_height_density"] = tidy(number(*fog, "height_falloff", 0.2));
            if (flag(*fog, "volumetric")) {
                level.environment["volumetric_fog_enabled"] = true;
            }
            problems_.add("exponential height fog", where,
                          "Godot's height fog stands for it: as thick, thinning upward by the "
                          "same falloff");
            return;
        }
        if (member(comp, "sky_atmosphere") != nullptr) {
            level.sky = true;
            return;
        }
        if (cls == "ArrowComponent" || cls == "BillboardComponent" ||
            cls == "DrawFrustumComponent" || cls.ends_with("SpriteComponent")) {
            level.droppable.insert(id); // shows only in the editor
            return;
        }
        if (member(comp, "spring_arm") != nullptr || cls == "SceneComponent") {
            return; // places things
        }
        problems_.add("component " + cls, where, "it has no counterpart yet");
    }

    [[nodiscard]] static bool has_character(const json::Value& actor) {
        return std::ranges::any_of(items(actor, "components"), [](const json::Value& comp) {
            return member(comp, "character") != nullptr;
        });
    }

    void convert_shape(const json::Value& comp, const json::Value& shape, Level& level,
                       const std::string& actor_id, const std::string& id, bool character,
                       const std::string& where) {
        Entity& entity = level.entities[id];
        if (text(comp, "collision") == "no_collision" || text(comp, "collision").empty()) {
            return;
        }
        json::Value out;
        const std::string type = text(shape, "type");
        if (type == "box") {
            const V3 extent = vec(shape, "extent");
            out["shape"] = "box";
            out["size"] = triple(size_of({extent[0] * 2.0, extent[1] * 2.0, extent[2] * 2.0}));
        } else if (type == "sphere") {
            out["shape"] = "sphere";
            out["radius"] = tidy(number(shape, "radius") * axes_.scale);
        } else {
            out["shape"] = "capsule";
            out["radius"] = tidy(number(shape, "radius") * axes_.scale);
            out["height"] = tidy(2.0 * number(shape, "half_height") * axes_.scale);
            if (axes_.axis[1] != 2) {
                problems_.add("capsule", where, "it stands along y, not along Unreal's z");
            }
        }
        entity.components["CollisionShape3D"] = std::move(out);
        if (!blocks(comp)) {
            entity.components["Area3D"] = json::Object{};
            entity.has_body = true;
            return;
        }
        if (character) {
            entity.components["CharacterBody3D"] = json::Object{};
            entity.has_body = true;
            return;
        }
        if (entity.has_body || body_above(level, actor_id, id)) {
            return;
        }
        entity
            .components[text(comp, "mobility") == "movable" ? "AnimatableBody3D" : "StaticBody3D"] =
            json::Object{};
        entity.has_body = true;
    }

    void convert_other(const json::Value& comp, const json::Value& actor, Level& level,
                       const std::string& id, const std::string& where) {
        const std::string cls = text(comp, "class");
        if (const json::Value* movement = member(comp, "character")) {
            Entity& entity = level.entities[id];
            json::Value& body = entity.components["CharacterBody3D"];
            if (body.is_null()) {
                body = json::Object{};
            }
            body["floor_max_angle"] =
                tidy(number(*movement, "walkable_floor_angle", 44.765) * std::numbers::pi / 180.0);
            json::Value record;
            record["max_walk_speed"] =
                tidy(number(*movement, "max_walk_speed", 600.0) * axes_.scale);
            record["jump_velocity"] =
                tidy(number(*movement, "jump_z_velocity", 420.0) * axes_.scale);
            record["max_step_height"] =
                tidy(number(*movement, "max_step_height", 45.0) * axes_.scale);
            level.record["actors"][id]["character_movement"] = std::move(record);
            problems_.add("CharacterMovementComponent", where,
                          "a CharacterBody3D, moved by gameplay code; its speeds are in the "
                          "report");
            return;
        }
        (void)actor;
        // Components that only Unreal's editor or engine runs.
        static constexpr std::array<std::string_view, 6> kQuiet{
            "InputComponent",
            "PawnNoiseEmitterComponent",
            "NavigationInvokerComponent",
            "PhysicsHandleComponent",
            "AIPerceptionStimuliSourceComponent",
            "ChildActorComponent"};
        if (contains(kQuiet, cls)) {
            return;
        }
        problems_.add("component " + cls, where,
                      cls.ends_with("_C") ? "a Blueprint component: port it with its Blueprint"
                                          : "it has no counterpart yet");
    }

    void convert_light(const json::Value& light, Entity& entity, Level& level,
                       const std::string& where) {
        const std::string type = text(light, "type");
        const double intensity = number(light, "intensity", 1.0);
        if (type == "sky") {
            level.sky_light = intensity;
            const V3 color = vec(light, "color", {255, 255, 255});
            level.sky_color = json::Array{tidy(color[0] / 255.0), tidy(color[1] / 255.0),
                                          tidy(color[2] / 255.0), 1.0};
            return;
        }
        json::Value out;
        std::string kind;
        double energy = 1.0;
        if (type == "directional") {
            kind = "DirectionalLight3D";
            energy = intensity / 10.0; // Unreal's lux; its default sun of 10 lux is energy 1
            if (const double angle = number(light, "light_source_angle"); angle > 0.0) {
                out["light_angular_distance"] = tidy(angle);
            }
        } else {
            // Candelas: Unreal's default local light of 8 cd is energy 1.
            const std::string units = text(light, "units");
            double candelas = intensity;
            if (units == "unitless") {
                candelas = intensity / 625.0;
            } else if (units == "lumens") {
                candelas = intensity / (4.0 * std::numbers::pi);
            } else if (units == "ev") {
                candelas = std::pow(2.0, intensity - 3.0);
            }
            energy = candelas / 8.0;
            const double range = number(light, "attenuation_radius", 1000.0) * axes_.scale;
            if (type == "point") {
                kind = "OmniLight3D";
                out["omni_range"] = tidy(range);
            } else {
                kind = "SpotLight3D";
                out["spot_range"] = tidy(range);
                out["spot_angle"] =
                    tidy(type == "rect" ? 89.0
                                        : std::min(number(light, "outer_cone_angle", 44.0), 89.0));
                if (type == "rect") {
                    problems_.add("rect light", where, "a spot light shining over its face");
                }
            }
            if (const double size = number(light, "source_radius"); size > 0.0) {
                out["light_size"] = tidy(size * axes_.scale);
            }
        }
        out["light_energy"] = tidy(energy);
        const V3 color = vec(light, "color", {255, 255, 255});
        if (color != V3{255, 255, 255}) {
            out["light_color"] = json::Array{tidy(color[0] / 255.0), tidy(color[1] / 255.0),
                                             tidy(color[2] / 255.0), 1.0};
        }
        if (flag(light, "shadows", true)) {
            out["shadow_enabled"] = true;
        }
        if (member(light, "temperature") != nullptr) {
            problems_.add("light temperature", where, "the light has its color alone");
        }
        if (text(light, "mobility") == "static") {
            problems_.add("static light", where,
                          "baked in Unreal; it lights the scene as it runs here, or bake it into "
                          "a LightmapGI");
        }
        entity.components[kind] = std::move(out);
    }

    void convert_audio(const json::Value& audio, Entity& entity, const std::string& where) {
        const std::string sound = text(audio, "sound");
        const auto found = sounds_.find(sound);
        if (found == sounds_.end()) {
            if (!sound.empty()) {
                problems_.add("sound that didn't export", where, sound);
            }
            return;
        }
        json::Value out;
        out["stream"] = found->second;
        if (const double volume = number(audio, "volume", 1.0); volume != 1.0) {
            out["volume_db"] = tidy(volume > 0.0 ? 20.0 * std::log10(volume) : -80.0);
        }
        if (const double pitch = number(audio, "pitch", 1.0); pitch != 1.0) {
            out["pitch_scale"] = tidy(pitch);
        }
        if (flag(audio, "auto_activate", true)) {
            out["autoplay"] = true;
        }
        if (member(audio, "falloff_distance") != nullptr) {
            const double inner = number(audio, "inner_radius", 400.0) * axes_.scale;
            out["unit_size"] = tidy(std::max(inner, 1.0));
            out["max_distance"] =
                tidy(inner + (number(audio, "falloff_distance", 3600.0) * axes_.scale));
            entity.components["AudioStreamPlayer3D"] = std::move(out);
            problems_.add("linear sound attenuation", where,
                          "it fades as 1 / distance from its inner radius and stops at its "
                          "falloff's end");
            return;
        }
        // Without attenuation Unreal plays a sound everywhere at once.
        entity.components["AudioStreamPlayer"] = std::move(out);
    }

    // A level without a camera views from the eyes of the pawn the player possesses, or of the
    // one the game mode spawns at the player start: a camera 64 cm up, Unreal's BaseEyeHeight.
    void add_camera(Level& level) {
        const std::string& viewer = level.possessed.empty() ? level.player_start : level.possessed;
        if (level.camera || viewer.empty() || !level.entities.contains(viewer)) {
            return;
        }
        Entity& start = level.entities[viewer];
        const std::string id = take(level.used, viewer + "_camera");
        Entity& camera = level.entities[id];
        camera.turn = facing();
        json::Value place;
        place["location"] = json::Array{0.0, 0.0, 64.0};
        camera.components["Transform3D"] = transform_3d(place, start.turn, camera.turn);
        json::Value fields;
        fields["fov"] = 90.0;
        fields["keep_aspect"] = "width";
        fields["current"] = true;
        camera.components["Camera3D"] = std::move(fields);
        start.children.push_back(id);
        level.record["camera_added"] = id;
        level.camera = true;
    }

    void write_environment(Level& level) {
        if (level.sky) {
            level.environment["background_mode"] = "sky";
        }
        if (level.sky_light >= 0.0) {
            level.environment["ambient_light_source"] = level.sky ? "sky" : "color";
            if (!level.sky) {
                level.environment["ambient_light_color"] = level.sky_color;
            }
            level.environment["ambient_light_energy"] = tidy(level.sky_light);
            if (level.sky) {
                level.environment["reflected_light_source"] = "sky";
            }
        }
        if (level.environment.empty()) {
            return;
        }
        const std::string id = take(level.used, "environment");
        Entity& entity = level.entities[id];
        entity.components["WorldEnvironment"] = json::Value(level.environment);
        if (level.sky) {
            // Unreal's sky atmosphere scatters sunlight as Godot's physical sky does.
            entity.components["PhysicalSkyMaterial"] = json::Object{};
        }
        level.roots.push_back(id);
    }

    // Leaves out the droppable entities that hold nothing but a transform.
    static void drop_empty(Level& level) {
        for (const std::string& id : level.droppable) {
            const auto found = level.entities.find(id);
            if (found == level.entities.end() || !found->second.children.empty() ||
                found->second.components.size() != 1 ||
                !found->second.components.contains("Transform3D")) {
                continue;
            }
            level.entities.erase(found);
            std::erase(level.roots, id);
            for (auto& [other, entity] : level.entities) {
                std::erase(entity.children, id);
            }
            level.record["actors"].as_object().erase(id);
            std::erase_if(level.actor_ids, [&](const auto& pair) { return pair.second == id; });
        }
    }

    void write_scene(Level& level, const json::Value& data) {
        drop_empty(level);
        json::Object entities;
        for (auto& [id, entity] : level.entities) {
            if (!entity.children.empty()) {
                json::Value& transform = entity.components["Transform3D"];
                if (transform.is_null()) {
                    transform = json::Object{};
                }
                json::Array children;
                for (const std::string& child : entity.children) {
                    children.emplace_back(child);
                }
                transform["children"] = std::move(children);
            }
            json::Value out;
            out["components"] = json::Value(std::move(entity.components));
            entities[id] = std::move(out);
        }
        json::Value scene;
        scene["format"] = 1;
        scene["entities"] = json::Value(std::move(entities));
        const world::FileReader reader = planned_files();
        for (int pass = 0; pass < 64; ++pass) {
            const auto errors = world::validate_scene(components_, scene, reader);
            if (errors.empty()) {
                break;
            }
            std::vector<std::string> removed;
            for (const json::ValidationError& error : errors) {
                problems_.add("invalid value", level.scene + ": " + error.path, error.message);
                if (pointer_within(removed, error.path)) {
                    continue;
                }
                if (!remove_pointer(scene, error.path)) {
                    scene["entities"] = json::Object{};
                    break;
                }
                removed.push_back(error.path);
            }
        }
        auto formatted = world::format_scene(components_, json::write(scene), reader);
        add_file(level.scene, formatted ? formatted.value() : json::write(scene) + "\n");
        level.record["scene"] = level.scene;
        if (const double kill = number(data, "kill_z", -1048575.0); kill > -1048575.0) {
            level.record["kill_y"] = tidy(kill * axes_.scale);
        }
        if (const std::string mode = text(data, "game_mode"); !mode.empty()) {
            level.record["game_mode"] = mode;
        }
        json::Value ids = json::Object{};
        for (const auto& [name, id] : level.actor_ids) {
            ids[name] = id;
        }
        level.record["entities"] = std::move(ids);
        levels_[level.path] = std::move(level.record);
        scenes_.push_back(level.scene);
    }

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
        constexpr std::string_view kGeneral = "/Script/EngineSettings.GeneralProjectSettings";
        std::string name = game_ini_.value(kGeneral, "ProjectName");
        if (name.empty()) {
            name = text(*member(export_, "project"), "name");
        }
        if (name.empty()) {
            name = project_.stem().string();
        }
        project_name_ = name;
        std::string map =
            engine_ini_.value("/Script/EngineSettings.GameMapsSettings", "GameDefaultMap");
        map = map.substr(0, map.find('.'));
        if (!map.empty() && levels_.contains(map)) {
            main_scene_ = scene_path(map);
        } else if (!scenes_.empty()) {
            main_scene_ = scenes_.front();
            if (!map.empty()) {
                problems_.add("game default map", map,
                              "it wasn't exported; the first level is "
                              "the main scene");
            }
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
        const std::string version = game_ini_.value(kGeneral, "ProjectVersion");
        std::vector<std::string_view> parts = split_path(version, '.');
        if (parts.size() > 3) {
            parts.resize(3);
        }
        bool numeric = !parts.empty();
        for (const std::string_view part : parts) {
            numeric = numeric && parse_int(part).has_value() && part.front() != '-';
        }
        if (numeric) {
            std::string full;
            for (std::size_t i = 0; i < 3; ++i) {
                full += (i == 0 ? "" : ".") + (i < parts.size() ? std::string(parts[i]) : "0");
            }
            project["version"] = full;
        }
        const std::string gravity =
            engine_ini_.value("/Script/Engine.PhysicsSettings", "DefaultGravityZ");
        const double gravity_z = parse_double(gravity).value_or(-980.0);
        project["physics_3d"]["gravity"] = tidy(std::abs(gravity_z) * axes_.scale);
        return project;
    }

    static std::optional<double> parse_double(std::string_view text) {
        if (text.empty()) {
            return std::nullopt;
        }
        const std::string copy(text);
        char* end = nullptr;
        const double value = std::strtod(copy.c_str(), &end);
        return end == copy.c_str() + copy.size() ? std::optional<double>(value) : std::nullopt;
    }

    static void bind(json::Value& project, const std::string& action, json::Value binding) {
        json::Value& events = project["input"][action]["events"];
        if (events.is_null()) {
            events = json::Array{};
        }
        if (!contains(events.as_array(), binding) && events.as_array().size() < 32) {
            events.push_back(std::move(binding));
        }
    }

    void convert_input(json::Value& project) {
        constexpr std::string_view kInput = "/Script/Engine.InputSettings";
        for (const std::string& mapping : input_ini_.values(kInput, "ActionMappings")) {
            std::map<std::string, std::string> fields;
            for (auto& [key, value] : unreal::parse_struct(mapping)) {
                fields[key] = value;
            }
            const std::string& action = fields["ActionName"];
            const std::vector<Push> pushes = pushes_of(fields["Key"]);
            if (action.empty() || pushes.empty()) {
                problems_.add("input key " + fields["Key"], "Config/DefaultInput.ini",
                              "no binding presses it");
                continue;
            }
            json::Value binding = pushes.front().binding;
            if (binding.find("type")->as_string() == "key") {
                for (const auto& [flag_name, modifier] :
                     {std::pair{"bShift", "shift"}, std::pair{"bCtrl", "ctrl"},
                      std::pair{"bAlt", "alt"}, std::pair{"bCmd", "meta"}}) {
                    if (lowercase(fields[flag_name]) == "true") {
                        binding[modifier] = true;
                    }
                }
            }
            bind(project, action, std::move(binding));
        }
        for (const std::string& mapping : input_ini_.values(kInput, "AxisMappings")) {
            std::map<std::string, std::string> fields;
            for (auto& [key, value] : unreal::parse_struct(mapping)) {
                fields[key] = value;
            }
            const std::string& axis = fields["AxisName"];
            const double scale = parse_double(fields["Scale"]).value_or(1.0);
            const std::vector<Push> pushes = pushes_of(fields["Key"]);
            if (axis.empty() || pushes.empty() || scale == 0.0) {
                problems_.add("input key " + fields["Key"], "Config/DefaultInput.ini",
                              "no binding moves it");
                continue;
            }
            for (const Push& push : pushes) {
                const double way = push.value[0] * scale;
                if (way != 0.0) {
                    bind(project, axis + (way > 0.0 ? "/positive" : "/negative"), push.binding);
                }
            }
        }
        convert_enhanced_input(project);
        if (const json::Value* input = project.find("input");
            input != nullptr && input->is_object()) {
            input_actions_ = static_cast<int>(input->as_object().size());
        }
    }

    // Enhanced Input's mappings: each action by its asset's name, split by the way each key
    // pushes it once its modifiers have turned the key's value.
    void convert_enhanced_input(json::Value& project) {
        const json::Value* actions = member(export_, "input_actions");
        const json::Value* contexts = member(export_, "input_contexts");
        if (contexts == nullptr || !contexts->is_object()) {
            return;
        }
        for (const auto& [context, mappings] : contexts->as_object()) {
            for (const json::Value& mapping :
                 mappings.is_array() ? mappings.as_array() : json::Array{}) {
                const std::string action_path = text(mapping, "action");
                const std::string action = file_stem(action_path);
                const std::string key = text(mapping, "key");
                const json::Value* type_value =
                    actions != nullptr ? actions->find(action_path) : nullptr;
                const std::string type = type_value != nullptr && type_value->is_string()
                                             ? type_value->as_string()
                                             : "boolean";
                std::vector<Push> pushes = pushes_of(key);
                if (pushes.empty()) {
                    problems_.add("input key " + key, context, "no binding moves it");
                    continue;
                }
                for (Push& push : pushes) {
                    for (const json::Value& modifier : items(mapping, "modifiers")) {
                        apply_modifier(modifier, push.value, context);
                    }
                }
                for (const Push& push : pushes) {
                    const V3& v = push.value;
                    std::string name;
                    if (type == "boolean") {
                        name = v[0] > 0.0 || v[1] > 0.0 || v[2] > 0.0 ? action : "";
                    } else if (type == "axis1d") {
                        name = v[0] == 0.0 ? "" : action + (v[0] > 0.0 ? "/positive" : "/negative");
                    } else if (type == "axis2d") {
                        if (v[0] != 0.0) {
                            name = action + (v[0] > 0.0 ? "/right" : "/left");
                        } else if (v[1] != 0.0) {
                            name = action + (v[1] > 0.0 ? "/up" : "/down");
                        }
                    } else {
                        problems_.add("3D input action", action_path,
                                      "actions are buttons or axes of one or two dimensions");
                        break;
                    }
                    if (!name.empty()) {
                        bind(project, name, push.binding);
                    }
                }
            }
        }
    }

    void apply_modifier(const json::Value& modifier, V3& value, const std::string& where) {
        const std::string cls = text(modifier, "class");
        if (cls.ends_with("Negate")) {
            for (std::size_t i = 0; i < 3; ++i) {
                if (flag(modifier, std::string(1, static_cast<char>('x' + i)), true)) {
                    value[i] = -value[i];
                }
            }
        } else if (cls.ends_with("SwizzleAxis")) {
            const std::string order =
                text(modifier, "order").empty() ? "yxz" : text(modifier, "order");
            // Order "yxz" puts x where y was: each letter names the axis that lands there.
            V3 out{};
            for (std::size_t i = 0; i < 3 && i < order.size(); ++i) {
                const auto from = static_cast<std::size_t>(order[i] - 'x');
                if (from < 3) {
                    out[from] = value[i];
                }
            }
            value = out;
        } else if (!cls.ends_with("DeadZone") && !cls.ends_with("ResponseCurveExponential") &&
                   !cls.ends_with("Smooth")) {
            problems_.add("input modifier " + cls, where, "the binding moves its action as is");
        }
    }

    void write_project(json::Value project) {
        for (int pass = 0; pass < 32; ++pass) {
            const auto errors = runtime::validate_project(project);
            if (errors.empty()) {
                break;
            }
            bool reset = false;
            for (const json::ValidationError& error : errors) {
                problems_.add("setting " + error.path, "Config", error.message);
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

    // --- Port tasks ---

    // The actors of each level whose class is a class path or comes from a package.
    [[nodiscard]] std::vector<std::string> users_of(std::string_view class_path,
                                                    bool package) const {
        std::vector<std::string> out;
        for (const json::Value& level : items(export_, "levels")) {
            for (const json::Value& actor : items(level, "actors")) {
                const std::string path = text(actor, "class_path");
                if (package ? path.starts_with(std::string(class_path) + ".")
                            : path == class_path) {
                    out.push_back(text(actor, "label") + " in " + scene_path(text(level, "path")));
                }
            }
        }
        return out;
    }

    void write_tasks(const std::optional<std::string>& existing) {
        std::vector<PortTask> tasks;
        for (const json::Value& blueprint : items(export_, "blueprints")) {
            const std::string package = text(blueprint, "path");
            const std::string kind = text(blueprint, "kind");
            PortTask task;
            task.link = content_path(package, ".uasset");
            task.engine = "unreal";
            if (kind == "WidgetBlueprint") {
                task.name = "Port the widget " + package + " to controls";
                task.summary = "UMG widget Blueprint: rebuild it as Control entities.";
            } else if (kind == "AnimBlueprint") {
                task.name = "Port the animation Blueprint " + package;
                task.summary = "Animation Blueprint: rebuild it as an .animtree.json "
                               "AnimationTree.";
            } else {
                task.name = "Port the Blueprint " + package + " to C++";
                task.summary = "Unreal Blueprint: write what it does as C++ components and "
                               "systems.";
            }
            std::string text_out = "Blueprint of " +
                                   (text(blueprint, "parent").empty() ? std::string("an unknown "
                                                                                    "class")
                                                                      : text(blueprint, "parent")) +
                                   ".";
            const std::vector<std::string> users = users_of(package, true);
            if (!users.empty()) {
                text_out += " Placed as " + join(users, ", ") + ".";
            }
            text_out += " Open it in Unreal's editor to read its graphs; its components are on "
                        "the entities its actors became, listed in " +
                        std::string(kReportFile) + ".";
            task.text = std::move(text_out);
            for (const json::Value& level : items(export_, "levels")) {
                for (const json::Value& actor : items(level, "actors")) {
                    if (text(actor, "class_path").starts_with(package + ".") &&
                        !contains(task.links, scene_path(text(level, "path")))) {
                        task.links.push_back(scene_path(text(level, "path")));
                    }
                }
            }
            if (copy_assets_) {
                copy_source(content_path(package, ".uasset").substr(prefix_.size()));
            }
            tasks.push_back(std::move(task));
            ++blueprints_;
        }
        for (const NativeClass& cls : read_classes()) {
            PortTask task;
            task.link = prefix_ + cls.header;
            task.name = "Port the C++ class " + cls.name + " to SlashEngine";
            task.summary = "Unreal C++: write what it does as SlashEngine components and systems.";
            task.engine = "unreal";
            std::string body = "class " + cls.name +
                               (cls.parent.empty() ? std::string() : " : " + cls.parent) + ".";
            if (!cls.properties.empty()) {
                body += " Properties: " + join(cls.properties, ", ") + ".";
            }
            if (!cls.functions.empty()) {
                body += " Functions: " + join(cls.functions, ", ") + ".";
            }
            const std::vector<std::string> users = users_of(script_path(cls), false);
            if (!users.empty()) {
                body += " Placed as " + join(users, ", ") + ".";
            }
            task.text = std::move(body);
            tasks.push_back(std::move(task));
            classes_.push_back(cls.name);
        }
        PortTasks written = write_port_tasks(existing, tasks);
        for (std::size_t i = 0; i < tasks.size(); ++i) {
            json::Value item;
            item["source"] = tasks[i].link;
            item["task"] = written.ids[i];
            task_list_.push_back(std::move(item));
        }
        if (!written.file.empty()) {
            add_file(storyboard::file_path("work"), std::move(written.file));
        }
    }

    void copy_source(const std::string& relative) {
        if (auto bytes = platform::read_file((root_ / relative).string()); bytes) {
            add_file(prefix_ + relative, std::move(bytes.value()));
        }
    }

    std::vector<NativeClass> read_classes() {
        std::vector<NativeClass> out;
        std::error_code error;
        const fs::path source = root_ / "Source";
        if (!fs::is_directory(source, error)) {
            return out;
        }
        std::vector<std::string> files;
        for (fs::recursive_directory_iterator it(source, error), end; !error && it != end;
             it.increment(error)) {
            if (it->is_regular_file(error)) {
                files.push_back(it->path().lexically_relative(root_).generic_string());
            }
        }
        std::ranges::sort(files);
        for (const std::string& file : files) {
            const std::string extension = extension_of(file);
            const bool code = extension == ".h" || extension == ".cpp" || extension == ".cs";
            if (!code) {
                continue;
            }
            if (copy_assets_) {
                copy_source(file);
            }
            if (extension != ".h") {
                continue;
            }
            auto text_in = platform::read_file((root_ / file).string());
            if (!text_in) {
                continue;
            }
            const std::vector<std::string_view> parts = split_path(file);
            const std::string module = parts.size() > 2 ? std::string(parts[1]) : std::string();
            for (NativeClass& cls : native_classes(text_in.value(), file, module)) {
                out.push_back(std::move(cls));
            }
        }
        return out;
    }

    // --- The report ---

    void write_report() {
        json::Value report;
        report["format"] = 1;
        report["engine"] = "unreal";
        report["engine_version"] = text(export_, "engine_version");
        report["axes"] = axes_.describe();
        report["meters_per_unit"] = tidy(axes_.scale);
        json::Value levels = json::Object{};
        for (auto& [path, record] : levels_) {
            levels[path] = std::move(record);
        }
        report["levels"] = std::move(levels);
        json::Value meshes = json::Object{};
        for (const auto& [package, mesh] : meshes_) {
            meshes[package] = mesh.path;
        }
        report["meshes"] = std::move(meshes);
        json::Value sounds = json::Object{};
        for (const auto& [package, stream] : sounds_) {
            sounds[package] = stream;
        }
        report["sounds"] = std::move(sounds);
        if (!animations_.empty()) {
            json::Value animations = json::Object{};
            for (const auto& [package, animation] : animations_) {
                animations[package] = animation.library;
            }
            report["animations"] = std::move(animations);
        }
        report["tasks"] = task_list_;
        json::Array unsupported = problems_.to_json();
        report["unsupported"] = unsupported;
        add_file(std::string(kReportFile), json::write(report) + "\n");

        json::Value& out = plan_.report;
        out["engine"] = "unreal";
        out["project"] = project_name_;
        out["engine_version"] = text(export_, "engine_version");
        if (!main_scene_.empty()) {
            out["main_scene"] = main_scene_;
        }
        out["converted"]["scenes"] = static_cast<std::int64_t>(scenes_.size());
        out["converted"]["meshes"] = converted_meshes_;
        out["converted"]["sounds"] = converted_sounds_;
        out["converted"]["animations"] = converted_animations_;
        out["converted"]["material_variants"] = converted_variants_;
        out["converted"]["input_actions"] = input_actions_;
        out["converted"]["blueprints"] = blueprints_;
        out["converted"]["classes"] = static_cast<std::int64_t>(classes_.size());
        out["tasks"] = task_list_;
        out["unsupported"] = std::move(unsupported);
        out["report"] = kReportFile;
    }

    fs::path project_;
    fs::path root_;
    std::string prefix_;
    bool copy_assets_ = false;
    fs::path work_;
    std::string editor_option_;
    unreal::Editor editor_;
    json::Value export_;
    unreal::Config engine_ini_;
    unreal::Config game_ini_;
    unreal::Config input_ini_;
    world::ComponentRegistry components_;
    Axes axes_;
    M3 axes_matrix_ = Axes{}.matrix();
    std::map<std::string, MeshOut> meshes_;
    std::map<std::string, std::string> sounds_;       // package -> the stream players play
    std::map<std::string, scene3d::Model> materials_; // package -> a model drawing it
    std::map<std::string, std::string> variants_;     // mesh and overrides -> the .glb file
    std::set<std::string> variant_paths_;
    std::map<std::string, AnimationOut> animations_; // package -> its file
    std::set<std::string> looped_;                   // .anim.json files written
    std::map<std::string, json::Value> levels_;
    std::vector<std::string> scenes_;
    std::vector<std::string> classes_;
    json::Array task_list_;
    std::string project_name_;
    std::string main_scene_;
    int converted_meshes_ = 0;
    int converted_sounds_ = 0;
    int converted_animations_ = 0;
    int converted_variants_ = 0;
    int input_actions_ = 0;
    int blueprints_ = 0;
    Problems problems_;
    ImportPlan plan_;
};

} // namespace

Result<ImportPlan> plan_unreal(const fs::path& project, const std::string& prefix, bool copy_assets,
                               const Existing& existing, const fs::path& work,
                               const std::string& editor) {
    UnrealImporter importer(project, prefix, copy_assets, work, editor);
    return importer.run(existing);
}

} // namespace slash::importer
