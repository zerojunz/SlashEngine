// The game side of Autopilot's protocol, after the Godot addon's playtest.gd and lib/link.gd,
// lib/keys.gd, lib/detectors.gd and lib/media.gd (Autopilot, Apache 2.0): the connection out to
// the agent service, the hello, requests and their answers (some a few ticks later), held keys,
// events (heartbeats, scene changes, log lines and the in-game detectors' findings), the picture
// and the run asked for at start, whose result ends the game.

#include "slash/autopilot/bridge.hpp"

#include "internal.hpp"

#include "slash/canvas/canvas.hpp"
#include "slash/command/session.hpp"
#include "slash/core/log.hpp"
#include "slash/core/text.hpp"
#include "slash/core/version.hpp"
#include "slash/input/input.hpp"
#include "slash/net/websocket.hpp"
#include "slash/platform/file.hpp"
#include "slash/platform/process.hpp"
#include "slash/render/image.hpp"
#include "slash/runtime/project.hpp"
#include "slash/ui/ui.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <ctime>
#include <limits>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <utility>

namespace slash::autopilot {

using detail::Answer;
using detail::Look;
using detail::r2;

// ---- Settings ----

namespace {

std::string home_folder() {
#ifdef _WIN32
    std::string home = platform::environment_variable("USERPROFILE");
    if (!home.empty()) {
        return home;
    }
#endif
    return platform::environment_variable("HOME");
}

std::string settings_file(std::string_view name) {
    const std::string folder = settings_folder();
    return folder.empty() ? std::string() : folder + "/" + std::string(name);
}

json::Value read_json(const std::string& path) {
    if (path.empty()) {
        return {};
    }
    Result<std::string> text = platform::read_file(path);
    if (!text.ok()) {
        return {};
    }
    Result<json::Value> doc = json::parse(text.value());
    return doc.ok() && doc.value().is_object() ? std::move(doc.value()) : json::Value();
}

bool local_url(std::string_view url) {
    return url.starts_with("ws://127.0.0.1:") || url.starts_with("ws://localhost:");
}

// The address an agent service announced in Autopilot/<name>, or "".
std::string announced_url(std::string_view name) {
    const json::Value said = read_json(settings_file(name));
    const json::Value* url = said.find("url");
    if (url == nullptr || !url->is_string() || !local_url(url->as_string())) {
        return "";
    }
    return url->as_string();
}

} // namespace

std::string settings_folder() {
#ifdef _WIN32
    std::string base = platform::environment_variable("LOCALAPPDATA");
    if (base.empty()) {
        const std::string home = home_folder();
        base = home.empty() ? std::string() : home + "/AppData/Local";
    }
#elif defined(__APPLE__)
    const std::string home = home_folder();
    const std::string base = home.empty() ? std::string() : home + "/Library/Application Support";
#else
    std::string base = platform::environment_variable("XDG_CONFIG_HOME");
    if (base.empty()) {
        const std::string home = home_folder();
        base = home.empty() ? std::string() : home + "/.config";
    }
#endif
    return base.empty() ? std::string() : base + "/Autopilot";
}

std::string find_url(std::string_view given) {
    if (!given.empty()) {
        return std::string(given);
    }
    if (std::string env = platform::environment_variable("AUTOPILOT_PLAYTEST_URL"); !env.empty()) {
        return env;
    }
    for (const std::string_view file : {"brain.json", "aura-brain.json"}) {
        if (std::string url = announced_url(file); !url.empty()) {
            return url;
        }
    }
    return std::string(kDefaultUrl);
}

std::string find_token(std::string_view given) {
    if (!given.empty()) {
        return std::string(given);
    }
    return platform::environment_variable("AUTOPILOT_PLAYTEST_TOKEN");
}

bool default_share() {
    const json::Value prefs = read_json(settings_file("preferences.json"));
    const json::Value* share = prefs.find("share_data");
    return share != nullptr && share->is_bool() && share->as_bool();
}

// ---- The WebSocket ----

namespace {

class WebSocketTransport final : public Transport {
public:
    explicit WebSocketTransport(std::string url) : url_(std::move(url)) {}

    net::WebSocketPeer& peer() { return peer_; }

    void poll() override {
        peer_.poll();
        while (peer_.available_packet_count() > 0) {
            Result<std::span<const std::uint8_t>> packet = peer_.get_packet();
            if (!packet.ok()) {
                break;
            }
            const std::span<const std::uint8_t> bytes = packet.value();
            inbox_.emplace_back(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        }
    }

    [[nodiscard]] State state() const override {
        switch (peer_.ready_state()) {
        case net::WebSocketState::Connecting:
            return State::Connecting;
        case net::WebSocketState::Open:
            return State::Open;
        default:
            return State::Closed;
        }
    }

    std::optional<std::string> receive() override {
        if (inbox_.empty()) {
            return std::nullopt;
        }
        std::string text = std::move(inbox_.front());
        inbox_.erase(inbox_.begin());
        return text;
    }

    bool send(std::string_view text) override { return peer_.send_text(text).ok(); }

    [[nodiscard]] std::string address() const override { return url_; }

    ~WebSocketTransport() override { peer_.close(); }
    WebSocketTransport(const WebSocketTransport&) = delete;
    WebSocketTransport& operator=(const WebSocketTransport&) = delete;

private:
    std::string url_;
    net::WebSocketPeer peer_;
    std::vector<std::string> inbox_;
};

} // namespace

Result<std::unique_ptr<Transport>> connect_websocket(std::string_view url, std::string_view token) {
    auto transport = std::make_unique<WebSocketTransport>(std::string(url));
    net::WebSocketPeer& peer = transport->peer();
    // As the Godot addon's: pictures and codex reads are large messages.
    peer.set_inbound_buffer_size(16 * 1024 * 1024);
    peer.set_outbound_buffer_size(64 * 1024 * 1024);
    peer.set_max_queued_packets(16384);
    if (!token.empty()) {
        peer.set_handshake_headers({"Authorization: Bearer " + std::string(token)});
    }
    if (Result<void> const started = peer.connect_to_url(url); !started.ok()) {
        return started.error();
    }
    return std::unique_ptr<Transport>(std::move(transport));
}

Connector websocket_connector(std::string url, std::string token) {
    return [url = std::move(url), token = std::move(token)]() {
        return connect_websocket(find_url(url), find_token(token));
    };
}

// ---- Keys (the addon's lib/keys.gd) ----

namespace {

using input::Key;

struct NamedKey {
    std::string_view name;
    Key key;
};

constexpr std::array<NamedKey, 66> kNamedKeys{{
    {"SpaceBar", Key::Space},
    {"Enter", Key::Enter},
    {"Escape", Key::Escape},
    {"Tab", Key::Tab},
    {"BackSpace", Key::Backspace},
    {"LeftShift", Key::Shift},
    {"RightShift", Key::Shift},
    {"LeftControl", Key::Ctrl},
    {"RightControl", Key::Ctrl},
    {"LeftAlt", Key::Alt},
    {"RightAlt", Key::Alt},
    {"LeftCommand", Key::Meta},
    {"RightCommand", Key::Meta},
    {"Up", Key::Up},
    {"Down", Key::Down},
    {"Left", Key::Left},
    {"Right", Key::Right},
    {"Delete", Key::Delete},
    {"Insert", Key::Insert},
    {"Home", Key::Home},
    {"End", Key::End},
    {"PageUp", Key::PageUp},
    {"PageDown", Key::PageDown},
    {"CapsLock", Key::CapsLock},
    {"Pause", Key::Pause},
    {"Zero", Key::Key0},
    {"One", Key::Key1},
    {"Two", Key::Key2},
    {"Three", Key::Key3},
    {"Four", Key::Key4},
    {"Five", Key::Key5},
    {"Six", Key::Key6},
    {"Seven", Key::Key7},
    {"Eight", Key::Key8},
    {"Nine", Key::Key9},
    {"NumPadZero", Key::Kp0},
    {"NumPadOne", Key::Kp1},
    {"NumPadTwo", Key::Kp2},
    {"NumPadThree", Key::Kp3},
    {"NumPadFour", Key::Kp4},
    {"NumPadFive", Key::Kp5},
    {"NumPadSix", Key::Kp6},
    {"NumPadSeven", Key::Kp7},
    {"NumPadEight", Key::Kp8},
    {"NumPadNine", Key::Kp9},
    {"Multiply", Key::KpMultiply},
    {"Add", Key::KpAdd},
    {"Subtract", Key::KpSubtract},
    {"Decimal", Key::KpPeriod},
    {"Divide", Key::KpDivide},
    {"Semicolon", Key::Semicolon},
    {"Equals", Key::Equal},
    {"Comma", Key::Comma},
    {"Hyphen", Key::Minus},
    {"Underscore", Key::UnderScore},
    {"Period", Key::Period},
    {"Slash", Key::Slash},
    {"Tilde", Key::QuoteLeft},
    {"LeftBracket", Key::BracketLeft},
    {"Backslash", Key::BackSlash},
    {"RightBracket", Key::BracketRight},
    {"Apostrophe", Key::Apostrophe},
    {"F1", Key::F1},
    {"F2", Key::F2},
    {"F3", Key::F3},
    {"F4", Key::F4},
}};

struct NamedButton {
    std::string_view name;
    input::MouseButton button;
};
constexpr std::array<NamedButton, 5> kMouseButtons{{
    {"LeftMouseButton", input::MouseButton::Left},
    {"RightMouseButton", input::MouseButton::Right},
    {"MiddleMouseButton", input::MouseButton::Middle},
    {"ThumbMouseButton", input::MouseButton::Xbutton1},
    {"ThumbMouseButton2", input::MouseButton::Xbutton2},
}};

struct NamedJoyButton {
    std::string_view name;
    input::JoyButton button;
};
constexpr std::array<NamedJoyButton, 14> kJoyButtons{{
    {"Gamepad_FaceButton_Bottom", input::JoyButton::A},
    {"Gamepad_FaceButton_Right", input::JoyButton::B},
    {"Gamepad_FaceButton_Left", input::JoyButton::X},
    {"Gamepad_FaceButton_Top", input::JoyButton::Y},
    {"Gamepad_LeftShoulder", input::JoyButton::LeftShoulder},
    {"Gamepad_RightShoulder", input::JoyButton::RightShoulder},
    {"Gamepad_Special_Left", input::JoyButton::Back},
    {"Gamepad_Special_Right", input::JoyButton::Start},
    {"Gamepad_LeftThumbstick", input::JoyButton::LeftStick},
    {"Gamepad_RightThumbstick", input::JoyButton::RightStick},
    {"Gamepad_DPad_Up", input::JoyButton::DpadUp},
    {"Gamepad_DPad_Down", input::JoyButton::DpadDown},
    {"Gamepad_DPad_Left", input::JoyButton::DpadLeft},
    {"Gamepad_DPad_Right", input::JoyButton::DpadRight},
}};

// Axis keys and their sign: Unreal's Y axes are positive up, SlashEngine's (Godot's) down.
struct NamedAxis {
    std::string_view name;
    input::JoyAxis axis;
    float sign;
};
constexpr std::array<NamedAxis, 6> kJoyAxes{{
    {"Gamepad_LeftX", input::JoyAxis::LeftX, 1.0F},
    {"Gamepad_LeftY", input::JoyAxis::LeftY, -1.0F},
    {"Gamepad_RightX", input::JoyAxis::RightX, 1.0F},
    {"Gamepad_RightY", input::JoyAxis::RightY, -1.0F},
    {"Gamepad_LeftTriggerAxis", input::JoyAxis::TriggerLeft, 1.0F},
    {"Gamepad_RightTriggerAxis", input::JoyAxis::TriggerRight, 1.0F},
}};

// What a key name presses.
struct Spec {
    enum class Kind : std::uint8_t {
        None,
        Key,
        MouseButton,
        Wheel,
        MouseAxis,
        WheelAxis,
        JoyButton,
        JoyAxis,
        JoyTrigger,
        Action,
    };
    Kind kind = Kind::None;
    Key key = Key::None;
    input::MouseButton button = input::MouseButton::None;
    input::JoyButton joy_button = input::JoyButton::A;
    input::JoyAxis axis = input::JoyAxis::LeftX;
    float sign = 1.0F;
    bool mouse_y = false;
    std::string action;

    [[nodiscard]] bool is_axis() const {
        return kind == Kind::MouseAxis || kind == Kind::JoyAxis || kind == Kind::WheelAxis;
    }
};

Spec spec_of(const world::World& world, std::string_view name) {
    Spec spec;
    if (name.empty()) {
        return spec;
    }
    if (name.size() == 1 && std::isalpha(static_cast<unsigned char>(name[0])) != 0) {
        spec.kind = Spec::Kind::Key;
        spec.key = static_cast<Key>(static_cast<int>(Key::A) +
                                    (std::toupper(static_cast<unsigned char>(name[0])) - 'A'));
        return spec;
    }
    for (const NamedKey& named : kNamedKeys) {
        if (named.name == name) {
            spec.kind = Spec::Kind::Key;
            spec.key = named.key;
            return spec;
        }
    }
    if (name.size() >= 2 && name.size() <= 3 && name[0] == 'F') {
        int n = 0;
        for (const char c : name.substr(1)) {
            n = std::isdigit(static_cast<unsigned char>(c)) != 0 ? (n * 10) + (c - '0') : -100;
        }
        if (n >= 1 && n <= 12) {
            spec.kind = Spec::Kind::Key;
            spec.key = static_cast<Key>(static_cast<int>(Key::F1) + n - 1);
            return spec;
        }
    }
    for (const NamedButton& named : kMouseButtons) {
        if (named.name == name) {
            spec.kind = Spec::Kind::MouseButton;
            spec.button = named.button;
            return spec;
        }
    }
    if (name == "MouseScrollUp" || name == "MouseScrollDown") {
        spec.kind = Spec::Kind::Wheel;
        spec.button =
            name == "MouseScrollUp" ? input::MouseButton::WheelUp : input::MouseButton::WheelDown;
        return spec;
    }
    if (name == "MouseX" || name == "MouseY") {
        spec.kind = Spec::Kind::MouseAxis;
        spec.mouse_y = name == "MouseY";
        return spec;
    }
    if (name == "MouseWheelAxis") {
        spec.kind = Spec::Kind::WheelAxis;
        return spec;
    }
    for (const NamedJoyButton& named : kJoyButtons) {
        if (named.name == name) {
            spec.kind = Spec::Kind::JoyButton;
            spec.joy_button = named.button;
            return spec;
        }
    }
    for (const NamedAxis& named : kJoyAxes) {
        if (named.name == name) {
            spec.kind = Spec::Kind::JoyAxis;
            spec.axis = named.axis;
            spec.sign = named.sign;
            return spec;
        }
    }
    if (name == "Gamepad_LeftTrigger" || name == "Gamepad_RightTrigger") {
        spec.kind = Spec::Kind::JoyTrigger;
        spec.axis = name == "Gamepad_LeftTrigger" ? input::JoyAxis::TriggerLeft
                                                  : input::JoyAxis::TriggerRight;
        return spec;
    }
    if (const input::Map* map = input::map(world); map != nullptr && map->has_action(name)) {
        spec.kind = Spec::Kind::Action;
        spec.action = std::string(name);
        return spec;
    }
    if (const std::optional<Key> key = input::parse_key(name); key && *key != Key::None) {
        spec.kind = Spec::Kind::Key;
        spec.key = *key;
    }
    return spec;
}

// The Unreal name of a binding, or "" when it has none.
std::string unreal_name(const input::Event& binding) {
    switch (binding.type) {
    case input::EventType::Key: {
        const Key key = binding.key != Key::None ? binding.key : binding.physical_key;
        for (const NamedKey& named : kNamedKeys) {
            if (named.key == key && !named.name.starts_with("Right")) {
                return std::string(named.name);
            }
        }
        if (key >= Key::F1 && key <= Key::F12) {
            return "F" + std::to_string(static_cast<int>(key) - static_cast<int>(Key::F1) + 1);
        }
        return std::string(input::key_name(key));
    }
    case input::EventType::MouseButton:
        for (const NamedButton& named : kMouseButtons) {
            if (named.button == binding.button) {
                return std::string(named.name);
            }
        }
        if (binding.button == input::MouseButton::WheelUp) {
            return "MouseScrollUp";
        }
        if (binding.button == input::MouseButton::WheelDown) {
            return "MouseScrollDown";
        }
        return "";
    case input::EventType::JoypadButton:
        for (const NamedJoyButton& named : kJoyButtons) {
            if (named.button == binding.joy_button) {
                return std::string(named.name);
            }
        }
        return "";
    case input::EventType::JoypadMotion:
        for (const NamedAxis& named : kJoyAxes) {
            if (named.axis == binding.axis) {
                return std::string(named.name);
            }
        }
        return "";
    default:
        return "";
    }
}

// A binding in words, as the addon's godot_keys are Godot's InputEvent::as_text.
std::string engine_name(const input::Event& binding) {
    switch (binding.type) {
    case input::EventType::Key:
        if (binding.key == input::Key::None) {
            return std::string(input::key_name(binding.physical_key)) + " (Physical)";
        }
        return std::string(input::key_name(binding.key));
    case input::EventType::MouseButton:
        return "Mouse " + std::string(input::mouse_button_name(binding.button));
    case input::EventType::JoypadButton:
        return "Joypad Button " + std::string(input::joy_button_name(binding.joy_button));
    case input::EventType::JoypadMotion:
        return "Joypad Motion " + std::string(input::joy_axis_name(binding.axis)) +
               (binding.axis_value < 0.0F ? " -" : " +");
    default:
        return std::string(input::type_name(binding.type));
    }
}

// Whether an action is one of the built-in ui_* actions as the engine defines it.
bool is_default_ui(const input::Map& defaults, std::string_view name, const input::Action& action) {
    if (!name.starts_with("ui_")) {
        return false;
    }
    const input::Action* fallback = defaults.find(name);
    return fallback != nullptr && fallback->events == action.events;
}

json::Array controls(const world::World& world) {
    json::Array out;
    const input::Map* map = input::map(world);
    if (map == nullptr) {
        return out;
    }
    const input::Map defaults = input::Map::with_defaults();
    for (const auto& [name, action] : map->actions()) {
        if (is_default_ui(defaults, name, action)) {
            continue;
        }
        json::Array keys;
        json::Array engine;
        bool digital = true;
        for (const input::Event& binding : action.events) {
            const std::string unreal = unreal_name(binding);
            if (!unreal.empty() &&
                std::find(keys.begin(), keys.end(), json::Value(unreal)) == keys.end()) {
                keys.emplace_back(unreal);
            }
            engine.emplace_back(engine_name(binding));
            if (binding.type == input::EventType::JoypadMotion) {
                digital = false;
            }
        }
        json::Value control;
        control["name"] = name;
        control["action"] = name;
        control["keys"] = std::move(keys);
        control["godot_keys"] = std::move(engine);
        control["digital"] = digital;
        out.push_back(std::move(control));
    }
    return out;
}

// The action a key is bound to, preferring the game's own over ui_* ones; "" when none.
std::string action_for_key(const world::World& world, Key key) {
    const input::Map* map = input::map(world);
    if (map == nullptr) {
        return "";
    }
    std::string found;
    for (const auto& [name, action] : map->actions()) {
        for (const input::Event& binding : action.events) {
            if (binding.type == input::EventType::Key &&
                (binding.key == key || binding.physical_key == key)) {
                if (!name.starts_with("ui_")) {
                    return name;
                }
                if (found.empty()) {
                    found = name;
                }
            }
        }
    }
    return found;
}

double number_arg(const json::Value& args, std::string_view key, double fallback) {
    const json::Value* found = args.find(key);
    return found != nullptr && found->is_number() ? found->as_double() : fallback;
}

std::string string_arg(const json::Value& args, std::string_view key,
                       std::string_view fallback = {}) {
    const json::Value* found = args.find(key);
    if (found == nullptr || found->is_null()) {
        return std::string(fallback);
    }
    if (found->is_string()) {
        return found->as_string();
    }
    return json::write(*found, json::Style::Compact);
}

bool bool_arg(const json::Value& args, std::string_view key, bool fallback) {
    const json::Value* found = args.find(key);
    return found != nullptr && found->is_bool() ? found->as_bool() : fallback;
}

// Replaces runs of digits with '#', as the addon's _normalize, for signatures.
std::string normalized(std::string_view text) {
    std::string out;
    bool digits = false;
    for (const char c : text) {
        if (std::isdigit(static_cast<unsigned char>(c)) != 0) {
            if (!digits) {
                out += '#';
            }
            digits = true;
        } else {
            out += c;
            digits = false;
        }
        if (out.size() >= 160) {
            break;
        }
    }
    return out;
}

std::string utc_stamp() {
    const std::time_t now = std::time(nullptr);
    std::tm parts{};
#ifdef _WIN32
    gmtime_s(&parts, &now);
#else
    gmtime_r(&now, &parts);
#endif
    std::array<char, 32> buffer{};
    const std::size_t n = std::strftime(buffer.data(), buffer.size(), "%Y%m%d-%H%M%S", &parts);
    return {buffer.data(), n};
}

double unix_time() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

// ---- The bridge ----

struct Bridge::State {
    static constexpr double kTapSeconds = 0.05;
    static constexpr std::uint64_t kTapFrames = 2;

    struct Pending {
        // Runs each poll until it answers; true when done.
        std::function<bool(double now)> tick;
    };
    struct Held {
        Spec spec;
        float value = 1.0F;
        bool rate = false;
        double renewed = 0.0;
        double until = 0.0; // game time a timed hold lets go at; 0 for a lease
    };
    struct LogLine {
        log::Level level = log::Level::Error;
        std::string line;
    };

    command::Session& session;
    Connector connect;
    Options options;
    std::unique_ptr<Transport> transport;

    // The connection.
    bool want = true;
    bool connected = false;
    bool welcomed = false;
    bool active = false;
    bool run_asked = false;
    double backoff = 1.0;
    double next_try = 0.0;
    double deadline = 0.0;
    std::string problem;
    std::string role;

    // The run.
    bool finished = false;
    int exit_code = 0;
    std::string report_dir;
    json::Value result;

    // The game.
    std::string project_name;
    std::string launch_scene;
    std::string scene;
    double game_time = 0.0;
    std::uint64_t last_world_tick = 0;
    double last_tick_seconds = 1.0 / 60.0;
    double last_real = -1.0;
    double scene_started = 0.0;
    std::uint64_t frames = 0;
    double fps = 0.0;
    std::uint64_t fps_frames = 0;
    double fps_at = 0.0;
    double heartbeat_at = -1.0;

    std::vector<json::Value> events;
    std::vector<Pending> pending;
    std::map<std::string, Held> held;
    double lease = 1.0;
    std::set<std::string> pressed_actions;

    // The detectors.
    log::Sink previous_sink;
    std::mutex log_mutex;
    std::vector<LogLine> log_lines;
    bool detectors_on = true;
    double long_fall_seconds = 8.0;
    double hitch_seconds = 0.75;
    double hud_every = 0.25;
    double scan_every = 0.5;
    double out_of_world_margin = 50.0;
    std::map<std::string, int> counts;
    double falling = 0.0;
    double hud_at = 0.0;
    double scan_at = 0.0;
    std::optional<float> kill_y;
    std::set<std::string> flagged;

    // The picture.
    bool recording = false;
    double record_fps = 5.0;
    int record_max_dim = 960;
    int record_quality = 75;
    std::int64_t frame_n = 0;
    std::int64_t frames_sent = 0;
    double next_frame_at = 0.0;

    State(command::Session& s, Connector c, Options o)
        : session(s), connect(std::move(c)), options(std::move(o)) {}

    [[nodiscard]] double now() const { return options.clock(); }

    [[nodiscard]] json::Value t_value() const { return r2(game_time); }

    [[nodiscard]] bool asks_run() const { return options.run.is_object(); }

    // ---- Sending ----

    void send(const json::Value& frame) const {
        if (transport != nullptr && connected) {
            transport->send(json::write(frame, json::Style::Compact));
        }
    }

    void respond(const json::Value& id, json::Value value) const {
        json::Value frame;
        frame["type"] = "response";
        frame["id"] = id;
        frame["ok"] = true;
        frame["result"] = std::move(value);
        frame["t"] = t_value();
        send(frame);
    }

    void respond_error(const json::Value& id, const std::string& error, int status = 400) const {
        json::Value frame;
        frame["type"] = "response";
        frame["id"] = id;
        frame["ok"] = false;
        frame["status"] = status;
        frame["error"] = error;
        frame["t"] = t_value();
        send(frame);
    }

    void emit(json::Value event) {
        if (active) {
            events.push_back(std::move(event));
        }
    }

    void flush_events() {
        if (!events.empty() && active) {
            json::Value frame;
            frame["type"] = "events";
            json::Array batch;
            batch.reserve(events.size());
            for (json::Value& event : events) {
                batch.push_back(std::move(event));
            }
            events.clear();
            frame["events"] = std::move(batch);
            frame["t"] = t_value();
            send(frame);
        }
    }

    [[nodiscard]] json::Value hello() const {
        json::Value h;
        h["type"] = "hello";
        h["protocol"] = kProtocol;
        h["plugin_version"] = std::string(slash::version());
        h["engine"] = "godot";
        h["engine_version"] = "SlashEngine " + std::string(slash::version());
        h["project"] = project_name;
        h["host"] = "game";
        h["features"] =
            options.media ? json::Array{"timed_hold", "media"} : json::Array{"timed_hold"};
        h["share"] = options.share;
        if (asks_run() && !run_asked) {
            h["run"] = options.run;
        }
        return h;
    }

    // ---- The connection ----

    void schedule() {
        transport.reset();
        connected = false;
        role.clear();
        if (want) {
            next_try = now() + backoff;
            backoff = std::min(backoff * 2.0, 15.0);
        }
    }

    void poll_link(double t) {
        connect_or_read(t);
        if (asks_run() && !finished && !welcomed && !run_asked && t > deadline) {
            fail_run("no welcome from the agent service within " +
                     std::to_string(static_cast<int>(options.connect_timeout)) + " s (" + problem +
                     ")");
        }
    }

    void connect_or_read(double t) {
        if (transport == nullptr) {
            if (!want || t < next_try) {
                return;
            }
            Result<std::unique_ptr<Transport>> made = connect();
            if (!made.ok()) {
                problem = "Can't connect: " + made.error().message;
                schedule();
                return;
            }
            transport = std::move(made.value());
            next_try = std::numeric_limits<double>::infinity();
        }
        transport->poll();
        const Transport::State state = transport->state();
        if (state == Transport::State::Open && !connected) {
            connected = true;
            problem.clear();
            send(hello());
        }
        // Every frame that came in, whatever the state now: a result and the close can come in
        // one poll.
        while (std::optional<std::string> text = transport->receive()) {
            Result<json::Value> frame = json::parse(*text);
            if (!frame.ok() || !frame.value().is_object()) {
                continue; // (a server's binary hello, or anything that isn't a frame)
            }
            handle_frame(frame.value(), t);
            if (finished && !options.stay_open) {
                return;
            }
        }
        if (transport != nullptr && state == Transport::State::Closed) {
            const bool was = connected;
            const std::string address = transport->address();
            if (!was) {
                problem = "Can't reach the agent service at " + address;
            } else if (problem.empty()) {
                problem = "the connection to " + address + " closed";
            }
            schedule();
            closed(was, t);
        }
    }

    void closed(bool was_connected, double t) {
        const bool had_welcome = welcomed;
        active = false;
        welcomed = false;
        release_all();
        recording = false;
        if (finished) {
            return;
        }
        if (run_asked && !options.stay_open) {
            fail_run("the connection to the agent service closed before the run's result");
        } else if (asks_run() && was_connected && !had_welcome && !options.stay_open) {
            fail_run("the agent service closed the connection without taking the run (" + problem +
                     ")");
        } else if (was_connected) {
            deadline = t + options.connect_timeout;
        }
    }

    void handle_frame(const json::Value& frame, double t) {
        const std::string type = string_arg(frame, "type");
        if (type == "request") {
            handle_request(frame);
        } else if (type == "welcome") {
            backoff = 1.0;
            role = string_arg(frame, "role");
            active = true;
            welcomed = true;
            if (asks_run()) {
                run_asked = true;
            }
            log::info("autopilot: connected", {{"role", role}});
        } else if (type == "status") {
            log::info("autopilot: " + string_arg(frame, "text"));
        } else if (type == "finding") {
            const json::Value* finding = frame.find("finding");
            log::info("autopilot: finding",
                      {{"summary", finding != nullptr ? string_arg(*finding, "summary") : ""}});
        } else if (type == "result") {
            handle_result(frame);
        } else if (type == "error") {
            problem = "the agent service sent an error: " + string_arg(frame, "message");
            log::info("autopilot: " + problem);
            want = false; // as the addon: an error frame ends the connecting
            if (transport != nullptr) {
                const bool was = connected;
                schedule();
                closed(was, t);
            }
            if (asks_run() && !options.stay_open && !finished) {
                fail_run(problem);
            }
        }
    }

    // ---- The run's end ----

    std::string write_report(const json::Value& run_result, const json::Value* files) const {
        std::string dir = options.report_dir;
        if (dir.empty()) {
            dir = session.options().project_root + "/.slash/autopilot/" + utc_stamp();
        }
        (void)platform::create_directories(dir);
        if (files != nullptr && files->is_object()) {
            for (const auto& [name, text] : files->as_object()) {
                std::string clean = name;
                if (const std::size_t slash = clean.find_last_of("/\\");
                    slash != std::string::npos) {
                    clean = clean.substr(slash + 1);
                }
                if (clean.empty() || clean == "." || clean == "..") {
                    continue;
                }
                (void)platform::write_file(
                    dir + "/" += clean,
                    text.is_string() ? text.as_string() : json::write(text, json::Style::Pretty));
            }
        }
        (void)platform::write_file(dir + "/result.json",
                                   json::write(run_result, json::Style::Pretty) + "\n");
        return dir;
    }

    void fail_run(const std::string& message) {
        log::error("autopilot: " + message);
        json::Value run_result;
        run_result["kind"] = string_arg(options.run, "kind");
        run_result["error"] = message;
        run_result["exit_code"] = 1;
        report_dir = write_report(run_result, nullptr);
        result = std::move(run_result);
        finish(1);
    }

    void handle_result(const json::Value& frame) {
        json::Value run_result = json::Object{};
        if (const json::Value* given = frame.find("result");
            given != nullptr && given->is_object()) {
            run_result = *given;
        }
        report_dir = write_report(run_result, frame.find("files"));
        log::info("autopilot: run finished", {{"report", report_dir}});
        int code = 0;
        if (const json::Value* exit = run_result.find("exit_code");
            exit != nullptr && exit->is_number()) {
            code = static_cast<int>(exit->as_int());
        } else if (const json::Value* verdict = run_result.find("verdict"); verdict != nullptr) {
            const std::string said = verdict->is_string() ? verdict->as_string() : "";
            code = 3;
            if (said == "CONFIRMED") {
                code = 0;
            } else if (said == "DENIED") {
                code = 2;
            }
        } else if (const json::Value* bugs = run_result.find("bugs");
                   bugs != nullptr && bugs->is_array() && !bugs->as_array().empty()) {
            code = 2;
        }
        result = std::move(run_result);
        if (asks_run() && !options.stay_open) {
            finish(code);
        }
    }

    void finish(int code) {
        if (finished) {
            return;
        }
        finished = true;
        exit_code = code;
        release_all();
        want = false;
        if (transport != nullptr) {
            transport.reset();
            connected = false;
        }
    }

    // ---- Input ----

    world::World& world() { return session.world(); }

    [[nodiscard]] Vec2 crosshair() const { return ui::settings(session.world()).viewport * 0.5F; }

    void send_key(const Spec& spec, bool pressed, float value = 1.0F) {
        world::World& w = world();
        input::Event ev;
        switch (spec.kind) {
        case Spec::Kind::Key: {
            ev.type = input::EventType::Key;
            ev.key = spec.key;
            ev.physical_key = spec.key;
            ev.pressed = pressed;
            if (pressed && spec.key >= Key::A && spec.key <= Key::Z) {
                ev.unicode = static_cast<char32_t>(
                    'a' + (static_cast<int>(spec.key) - static_cast<int>(Key::A)));
            } else if (pressed && spec.key >= Key::Key0 && spec.key <= Key::Key9) {
                ev.unicode = static_cast<char32_t>(
                    '0' + (static_cast<int>(spec.key) - static_cast<int>(Key::Key0)));
            } else if (pressed && spec.key == Key::Space) {
                ev.unicode = U' ';
            }
            break;
        }
        case Spec::Kind::MouseButton:
        case Spec::Kind::Wheel:
            ev.type = input::EventType::MouseButton;
            ev.button = spec.button;
            ev.pressed = pressed;
            ev.position = crosshair();
            if (spec.kind == Spec::Kind::Wheel) {
                ev.factor = std::abs(value);
            }
            break;
        case Spec::Kind::MouseAxis:
            ev.type = input::EventType::MouseMotion;
            ev.relative = spec.mouse_y ? Vec2{0.0F, -value} : Vec2{value, 0.0F};
            ev.position = crosshair();
            break;
        case Spec::Kind::WheelAxis:
            ev.type = input::EventType::MouseButton;
            ev.button = value > 0.0F ? input::MouseButton::WheelUp : input::MouseButton::WheelDown;
            ev.pressed = pressed;
            ev.factor = std::abs(value);
            ev.position = crosshair();
            break;
        case Spec::Kind::JoyButton:
            ev.type = input::EventType::JoypadButton;
            ev.joy_button = spec.joy_button;
            ev.pressed = pressed;
            break;
        case Spec::Kind::JoyAxis:
            ev.type = input::EventType::JoypadMotion;
            ev.axis = spec.axis;
            ev.axis_value = pressed ? std::clamp(value * spec.sign, -1.0F, 1.0F) : 0.0F;
            break;
        case Spec::Kind::JoyTrigger:
            ev.type = input::EventType::JoypadMotion;
            ev.axis = spec.axis;
            ev.axis_value = pressed ? 1.0F : 0.0F;
            break;
        case Spec::Kind::Action:
            ev.type = input::EventType::Action;
            ev.action = spec.action;
            ev.pressed = pressed;
            ev.strength = pressed ? std::clamp(std::abs(value), 0.0F, 1.0F) : 0.0F;
            break;
        case Spec::Kind::None:
            return;
        }
        input::push(w, ev);
        if (spec.kind == Spec::Kind::Wheel && pressed) {
            ev.pressed = false;
            input::push(w, ev);
        }
    }

    void release_all() {
        for (auto& [name, h] : held) {
            if (!h.spec.is_axis() || h.spec.kind == Spec::Kind::JoyAxis) {
                send_key(h.spec, false);
            }
        }
        held.clear();
        const input::Map* map = input::map(world());
        for (const std::string& action : pressed_actions) {
            if (map != nullptr && map->has_action(action)) {
                input::action_release(world(), action);
            }
        }
        pressed_actions.clear();
    }

    void tick_keys(double t, double dt) {
        for (auto it = held.begin(); it != held.end();) {
            Held const& h = it->second;
            const bool expired = h.until > 0.0 ? game_time >= h.until : t - h.renewed > lease;
            if (expired) {
                if (!h.spec.is_axis() || h.spec.kind == Spec::Kind::JoyAxis) {
                    send_key(h.spec, false);
                }
                it = held.erase(it);
                continue;
            }
            if (h.spec.kind == Spec::Kind::MouseAxis || h.spec.kind == Spec::Kind::WheelAxis) {
                send_key(h.spec, true, h.rate ? h.value * static_cast<float>(dt) : h.value);
            } else if (h.spec.kind == Spec::Kind::JoyAxis) {
                send_key(h.spec, true, h.value);
            }
            ++it;
        }
    }

    // A tap's answer: after kTapSeconds and kTapFrames polls, let go and answer.
    void after_tap(const json::Value& id, std::function<void()> release) {
        const double started = now();
        const std::uint64_t frame = frames;
        pending.push_back({[this, id, started, frame, release = std::move(release)](double t) {
            if (t - started < kTapSeconds || frames < frame + kTapFrames) {
                return false;
            }
            release();
            respond(id, json::Object{});
            return true;
        }});
    }

    std::optional<Answer> key_op(const json::Value& id, const json::Value& args) {
        const std::string mode = string_arg(args, "mode", "press");
        if (const double given = number_arg(args, "lease", 0.0); given > 0.0) {
            lease = std::clamp(given, 0.2, 10.0);
        }
        if (mode == "release_all") {
            release_all();
            return Answer::success(json::Object{});
        }
        const std::string name = string_arg(args, "key");
        const Spec spec = spec_of(world(), name);
        if (spec.kind == Spec::Kind::None) {
            return Answer::failure("Unknown key " + name);
        }
        const auto value = static_cast<float>(number_arg(args, "value", 1.0));
        const bool rate = bool_arg(args, "rate", false);
        if (mode == "release") {
            if (const auto found = held.find(name); found != held.end()) {
                if (!spec.is_axis() || spec.kind == Spec::Kind::JoyAxis) {
                    send_key(spec, false);
                }
                held.erase(found);
            }
            return Answer::success(json::Object{});
        }
        if (mode == "hold") {
            auto found = held.find(name);
            if (found == held.end()) {
                found = held.emplace(name, Held{.spec = spec}).first;
                if (!spec.is_axis()) {
                    send_key(spec, true, value);
                }
            } else if (spec.kind == Spec::Kind::Action && found->second.value != value) {
                send_key(spec, true, value);
            }
            Held& h = found->second;
            h.value = value;
            h.rate = rate;
            h.renewed = now();
            const double seconds = number_arg(args, "seconds", 0.0);
            h.until = seconds > 0.0 ? game_time + seconds : 0.0;
            return Answer::success(json::Object{});
        }
        if (spec.is_axis()) {
            send_key(spec, true, value);
            if (spec.kind == Spec::Kind::JoyAxis) {
                send_key(spec, false);
            }
            return Answer::success(json::Object{});
        }
        send_key(spec, true, value);
        after_tap(id, [this, spec, name]() {
            if (!held.contains(name)) {
                send_key(spec, false);
            }
        });
        return std::nullopt;
    }

    std::optional<Answer> input_op(const json::Value& id, const json::Value& args) {
        const std::string action = string_arg(args, "action");
        const input::Map* map = input::map(world());
        if (map == nullptr || !map->has_action(action)) {
            return Answer::failure("No input action named " + action);
        }
        const std::string mode = string_arg(args, "mode", "press");
        float strength = 1.0F;
        if (const json::Value* v = args.find("value"); v != nullptr) {
            if (v->is_object()) {
                const json::Value* x = v->find("X");
                if (x == nullptr) {
                    x = v->find("x");
                }
                strength = x != nullptr && x->is_number()
                               ? std::abs(static_cast<float>(x->as_double()))
                               : 1.0F;
            } else if (v->is_number()) {
                strength = std::abs(static_cast<float>(v->as_double()));
            }
        }
        Spec spec;
        spec.kind = Spec::Kind::Action;
        spec.action = action;
        if (mode == "release") {
            send_key(spec, false);
            return Answer::success(json::Object{});
        }
        send_key(spec, true, strength);
        if (mode == "hold") {
            return Answer::success(json::Object{});
        }
        after_tap(id, [this, spec]() { send_key(spec, false); });
        return std::nullopt;
    }

    // The actions W, S, A and D are bound to (the arrow keys when those aren't), "" where none.
    std::array<std::string, 4> move_actions() {
        constexpr std::array<std::pair<Key, Key>, 4> kPairs{{
            {Key::W, Key::Up},
            {Key::S, Key::Down},
            {Key::A, Key::Left},
            {Key::D, Key::Right},
        }};
        std::array<std::string, 4> out;
        for (std::size_t i = 0; i < 4; ++i) {
            out[i] = action_for_key(world(), kPairs[i].first);
            if (out[i].empty()) {
                out[i] = action_for_key(world(), kPairs[i].second);
            }
        }
        return out;
    }

    void drive(const std::array<float, 4>& strengths, const std::array<std::string, 4>& actions,
               std::array<bool, 4>& keys_down) {
        constexpr std::array<Key, 4> kKeys{Key::W, Key::S, Key::A, Key::D};
        for (std::size_t i = 0; i < 4; ++i) {
            const float s = std::max(0.0F, strengths[i]);
            const std::string& action = actions[i];
            if (!action.empty()) {
                if (s > 0.01F) {
                    input::action_press(world(), action, s);
                    pressed_actions.insert(action);
                } else if (pressed_actions.contains(action)) {
                    input::action_release(world(), action);
                    pressed_actions.erase(action);
                }
            } else {
                const bool down = s > 0.38F;
                if (down != keys_down[i]) {
                    keys_down[i] = down;
                    Spec spec;
                    spec.kind = Spec::Kind::Key;
                    spec.key = kKeys[i];
                    send_key(spec, down);
                }
            }
        }
    }

    std::optional<Answer> move_op(const json::Value& id, const json::Value& args) {
        std::string player_id;
        bool is3d = false;
        Vec3 start;
        {
            const Look look(session);
            const entt::entity player = look.player();
            if (player == entt::null) {
                return Answer::failure("No player");
            }
            player_id = look.id(player);
            is3d = look.is3d(player);
            if (is3d) {
                start = look.position3(player);
            } else {
                const Vec2 p = look.position2(player);
                start = {p.x, 0.0F, p.y};
            }
        }
        json::Value direction = json::Object{};
        if (const json::Value* d = args.find("direction"); d != nullptr && d->is_object()) {
            direction = *d;
        }
        const auto scale = static_cast<float>(std::clamp(number_arg(args, "scale", 1.0), 0.0, 1.0));
        const double seconds = number_arg(args, "seconds", 0.35);
        const std::array<std::string, 4> actions = move_actions();
        const double started = now();
        auto keys_down = std::make_shared<std::array<bool, 4>>();
        pending.push_back({[this, id, player_id, is3d, start, direction, scale, seconds, actions,
                            started, keys_down](double t) {
            const Look look(session);
            const entt::entity p = look.resolve(player_id);
            const bool alive = p != entt::null;
            if (alive && t - started < seconds) {
                float f = 0.0F;
                float r = 0.0F;
                if (is3d) {
                    Vec3 wish = detail::from_kit_dir(&direction);
                    wish.y = 0.0F;
                    wish = detail::normalized(wish);
                    const entt::entity cam = look.camera3d();
                    const world::Affine3D basis = look.global3(cam != entt::null ? cam : p);
                    Vec3 fwd = basis.z * -1.0F;
                    fwd.y = 0.0F;
                    Vec3 right = basis.x;
                    right.y = 0.0F;
                    f = detail::dot(wish, detail::normalized(fwd)) * scale;
                    r = detail::dot(wish, detail::normalized(right)) * scale;
                } else {
                    r = std::clamp(static_cast<float>(number_arg(direction, "X", 0.0)), -1.0F,
                                   1.0F) *
                        scale;
                    f = std::clamp(static_cast<float>(number_arg(direction, "Z", 0.0)), -1.0F,
                                   1.0F) *
                        scale;
                }
                drive({f, -f, -r, r}, actions, *keys_down);
                return false;
            }
            drive({0.0F, 0.0F, 0.0F, 0.0F}, actions, *keys_down);
            Vec3 end = start;
            if (alive) {
                if (is3d) {
                    end = look.position3(p);
                } else {
                    const Vec2 q = look.position2(p);
                    end = {q.x, 0.0F, q.y};
                }
            }
            const float dx = end.x - start.x;
            const float dz = end.z - start.z;
            const double moved = std::sqrt((dx * dx) + (dz * dz)) * (is3d ? detail::kScale : 1.0F);
            json::Value out;
            out["moved"] = std::round(moved * 10.0) / 10.0;
            respond(id, std::move(out));
            return true;
        }});
        return std::nullopt;
    }

    std::optional<Answer> watch_op(const json::Value& id, const json::Value& args) {
        auto watch = std::make_shared<detail::Watch>();
        {
            const Look look(session);
            *watch = detail::watch_start(look, args);
            detail::watch_tick(look, *watch, game_time);
        }
        const double seconds = number_arg(args, "seconds", 0.6);
        const double started = now();
        pending.push_back({[this, id, watch, seconds, started](double t) {
            const Look look(session);
            detail::watch_tick(look, *watch, game_time);
            if (t - started < seconds) {
                return false;
            }
            respond(id, detail::watch_finish(*watch));
            return true;
        }});
        return std::nullopt;
    }

    // ---- The session ----

    // A scene by project path, or by its file's name ("level" or "level.scene.json").
    [[nodiscard]] std::string find_scene(const std::string& name) const {
        const std::string& root = session.options().project_root;
        if (name.ends_with(".scene.json") && platform::exists(root + "/" + name)) {
            return name;
        }
        std::string wanted = name;
        if (const std::size_t slash = wanted.find_last_of('/'); slash != std::string::npos) {
            wanted = wanted.substr(slash + 1);
        }
        if (!wanted.ends_with(".scene.json")) {
            wanted += ".scene.json";
        }
        Result<std::vector<std::string>> files = platform::list_files(
            root, [](std::string_view folder) { return !folder.starts_with("build"); });
        if (!files.ok()) {
            return "";
        }
        for (const std::string& file : files.value()) {
            const std::size_t slash = file.find_last_of('/');
            const std::string base = slash == std::string::npos ? file : file.substr(slash + 1);
            if (base == wanted) {
                return file;
            }
        }
        return "";
    }

    std::optional<Answer> session_op(const json::Value& id, const json::Value& args) {
        const std::string action = string_arg(args, "action", "status");
        if (action != "restart") {
            json::Value out;
            out["running"] = !session.scene_path().empty() || !session.world().ids().empty();
            return Answer::success(std::move(out));
        }
        std::string target;
        if (const std::string map = string_arg(args, "map"); !map.empty()) {
            target = find_scene(map);
            if (target.empty()) {
                return Answer::failure("No scene " + map);
            }
        } else {
            target = !launch_scene.empty() ? launch_scene : session.scene_path();
        }
        if (target.empty()) {
            return Answer::failure("No scene to restart");
        }
        release_all();
        json::Value params;
        params["path"] = target;
        if (command::Reply const opened = session.call("scene.open", params); !opened.ok) {
            return Answer::failure("Couldn't open " + target + ": " + opened.message, 500);
        }
        session.set_playing(true);
        scene_changed();
        const std::uint64_t frame = frames;
        pending.push_back({[this, id, target, frame](double /*t*/) {
            if (frames < frame + 2) {
                return false;
            }
            json::Value out;
            out["running"] = true;
            out["scene"] = target;
            respond(id, std::move(out));
            return true;
        }});
        return std::nullopt;
    }

    void scene_changed() {
        scene = session.scene_path();
        game_time = 0.0;
        last_world_tick = session.world().tick();
        scene_started = now();
        kill_y.reset();
        flagged.clear();
        falling = 0.0;
        emit(json::Value(json::Object{{"kind", "session"}, {"t", t_value()}, {"running", true}}));
    }

    json::Value context() {
        const Look look(session);
        const entt::entity player = look.player();
        entt::entity camera = look.camera3d();
        if (camera == entt::null) {
            camera = look.camera2d();
        }
        json::Value out;
        out["world"] = session.scene_path();
        out["scene"] = session.scene_path();
        out["character"] = player != entt::null ? look.id(player) : std::string();
        out["controller"] = "";
        out["input_subsystem"] = "";
        out["camera_manager"] = camera != entt::null ? look.id(camera) : std::string();
        out["camera"] = camera != entt::null ? look.id(camera) : std::string();
        out["dimension"] = look.dimension();
        out["game_name"] = project_name;
        out["host"] = "game";
        out["engine"] = "godot";
        out["session_running"] = true;
        out["paused"] = !session.playing();
        out["game_time"] = r2(game_time);
        out["project_dir"] = session.options().project_root;
        out["controls"] = controls(session.world());
        return out;
    }

    // ---- UI ----

    std::optional<Answer> ui_act(const json::Value& id, const json::Value& args) {
        std::string control_id = string_arg(args, "id");
        const std::string action = string_arg(args, "action", "click");
        world::World& w = world();
        // An entity ID, as ui lists them; a screen key ("<owner>.<id>") or a path ("/<id>")
        // works too.
        entt::entity control = w.find(control_id);
        if (control == entt::null && control_id.starts_with('/')) {
            control = w.find(std::string_view(control_id).substr(1));
        }
        if (control == entt::null) {
            if (const std::size_t dot = control_id.find('.'); dot != std::string::npos) {
                control = w.find(std::string_view(control_id).substr(dot + 1));
            }
        }
        const std::string asked = control_id;
        if (control != entt::null) {
            control_id = std::string(w.id_of(control));
        }
        const std::optional<Vec2> at =
            control == entt::null ? std::nullopt : detail::control_center(session, control);
        if (!at) {
            return Answer::failure("no element " + asked + " on screen");
        }
        const auto& ecs = w.ecs();
        bool disabled = false;
        for (const world::ComponentType* type :
             {w.components().find("Button"), w.components().find("CheckBox"),
              w.components().find("CheckButton"), w.components().find("OptionButton"),
              w.components().find("MenuButton"), w.components().find("TextureButton")}) {
            if (type != nullptr && type->has(w, control)) {
                const json::Value values = type->get(w, control);
                disabled = bool_arg(values, "disabled", false);
            }
        }
        if (disabled) {
            return Answer::failure(control_id + " is disabled");
        }
        const auto set = [&](std::string_view component, json::Value fields) -> Answer {
            json::Value components;
            components[component] = std::move(fields);
            json::Value params;
            params["id"] = control_id;
            params["components"] = std::move(components);
            if (command::Reply const reply = session.call("component.set", params); !reply.ok) {
                return Answer::failure(reply.message);
            }
            return Answer::success(json::Object{});
        };
        if (action == "click") {
            if (const auto* option = ecs.try_get<world::OptionButton>(control); option != nullptr) {
                json::Value params;
                params["id"] = control_id;
                if (const json::Value* wanted = args.find("option");
                    wanted != nullptr && !wanted->is_null()) {
                    params["text"] = wanted->is_string()
                                         ? wanted->as_string()
                                         : json::write(*wanted, json::Style::Compact);
                } else {
                    const int count = static_cast<int>(option->items.size());
                    params["index"] = count > 0 ? (option->selected + 1) % count : 0;
                }
                if (command::Reply const reply = session.call("ui.select", params); !reply.ok) {
                    return Answer::failure(control_id + " has no option " +
                                           string_arg(args, "option"));
                }
                return Answer::success(json::Value(json::Object{{"via", "select"}}));
            }
            push_click(*at, input::MouseButton::Left, false);
            if (!ecs.any_of<world::Button, world::CheckBox, world::CheckButton, world::MenuButton,
                            world::TextureButton>(control)) {
                return Answer::success(json::Value(json::Object{{"via", "mouse"}}));
            }
            // As the addon: the click, else the button pressed as its signal would be.
            const std::uint64_t frame = frames;
            pending.push_back({[this, id, control_id, frame](double /*t*/) {
                for (const ui::UiEvent& event : ui::events(session.world())) {
                    if (event.id == control_id &&
                        (event.kind == "pressed" || event.kind == "toggled")) {
                        respond(id, json::Value(json::Object{{"via", "mouse"}}));
                        return true;
                    }
                }
                if (frames < frame + 3) {
                    return false;
                }
                json::Value params;
                params["id"] = control_id;
                (void)session.call("ui.press", params);
                respond(id, json::Value(json::Object{{"via", "signal"}}));
                return true;
            }});
            return std::nullopt;
        }
        if (action == "right_click") {
            push_click(*at, input::MouseButton::Right, false);
            return Answer::success(json::Value(json::Object{{"via", "mouse"}}));
        }
        if (action == "double_click") {
            push_click(*at, input::MouseButton::Left, true);
            return Answer::success(json::Value(json::Object{{"via", "mouse"}}));
        }
        if (action == "set") {
            const json::Value* value = args.find("value");
            const json::Value* text = args.find("text");
            if (const auto* slider = ecs.try_get<world::Slider>(control); slider != nullptr) {
                return set("Slider", json::Value(json::Object{
                                         {"value", number_arg(args, "value", slider->value)}}));
            }
            std::string typed = string_arg(args, "value");
            if (text != nullptr && text->is_string()) {
                typed = text->as_string();
            } else if (value != nullptr && value->is_string()) {
                typed = value->as_string();
            }
            if (ecs.all_of<world::LineEdit>(control)) {
                return set("LineEdit", json::Value(json::Object{{"text", typed}}));
            }
            if (ecs.all_of<world::TextEdit>(control)) {
                return set("TextEdit", json::Value(json::Object{{"text", typed}}));
            }
            for (const std::string_view type : {"CheckBox", "CheckButton", "Button"}) {
                const world::ComponentType* component = w.components().find(type);
                if (component != nullptr && component->has(w, control) &&
                    bool_arg(component->get(w, control), "toggle_mode", false)) {
                    return set(type, json::Value(json::Object{
                                         {"button_pressed", bool_arg(args, "value", true)}}));
                }
            }
            return Answer::failure(control_id + " takes no value");
        }
        if (action == "drag") {
            json::Value params;
            params["from"] = control_id;
            params["to"] = string_arg(args, "to");
            if (command::Reply const reply = session.call("ui.drag", params); !reply.ok) {
                return Answer::failure(reply.message);
            }
            return Answer::success(json::Object{});
        }
        return Answer::failure("unknown action " + action);
    }

    void push_click(Vec2 at, input::MouseButton button, bool double_click) {
        world::World& w = world();
        input::Event motion;
        motion.type = input::EventType::MouseMotion;
        motion.position = at;
        input::push(w, motion);
        for (int i = 0; i < (double_click ? 2 : 1); ++i) {
            input::Event press;
            press.type = input::EventType::MouseButton;
            press.button = button;
            press.position = at;
            press.pressed = true;
            press.double_click = double_click && i == 1;
            input::push(w, press);
            press.pressed = false;
            press.double_click = false;
            input::push(w, press);
        }
    }

    // ---- The picture ----

    [[nodiscard]] std::string no_picture() const {
        if (!options.media) {
            return "pictures are off for this game (--autopilot-no-media)";
        }
        return "";
    }

    // The frame drawn on the software device, its longer side at most max_dim, as a JPEG.
    Result<std::pair<render::Image, std::string>> picture(int max_dim, int quality) {
        const Vec2 viewport = ui::settings(session.world()).viewport;
        const float longer = std::max(viewport.x, viewport.y);
        canvas::CaptureOptions capture_options;
        capture_options.scale =
            longer > static_cast<float>(max_dim) ? static_cast<float>(max_dim) / longer : 1.0F;
        Result<canvas::Capture> drawn =
            canvas::capture(session.world(), session.assets(), capture_options);
        if (!drawn.ok()) {
            return drawn.error();
        }
        render::Image image = std::move(drawn.value().image);
        Result<std::string> jpeg = render::encode_jpeg(image, quality);
        if (!jpeg.ok()) {
            return jpeg.error();
        }
        return std::pair<render::Image, std::string>{std::move(image), std::move(jpeg.value())};
    }

    std::optional<Answer> record_op(const json::Value& args) {
        const std::string action = string_arg(args, "action");
        if (action == "stop") {
            json::Value out;
            out["recording"] = false;
            out["frames"] = recording ? frames_sent : 0;
            recording = false;
            return Answer::success(std::move(out));
        }
        if (action != "start") {
            return Answer::failure("record wants action start or stop");
        }
        if (const std::string why = no_picture(); !why.empty()) {
            return Answer::failure("no picture: " + why, 409);
        }
        const double f = number_arg(args, "fps", 5.0);
        record_fps = f > 0.0 ? std::clamp(f, 0.1, 60.0) : 5.0;
        record_max_dim =
            static_cast<int>(std::clamp(number_arg(args, "max_dim", 960.0), 16.0, 8192.0));
        record_quality =
            static_cast<int>(std::clamp(number_arg(args, "quality", 75.0), 1.0, 100.0));
        recording = true;
        frame_n = 0;
        frames_sent = 0;
        next_frame_at = 0.0;
        const Vec2 viewport = ui::settings(session.world()).viewport;
        const float longer = std::max(viewport.x, viewport.y);
        const float scale = longer > static_cast<float>(record_max_dim)
                                ? static_cast<float>(record_max_dim) / longer
                                : 1.0F;
        json::Value out;
        out["recording"] = true;
        out["width"] = std::max(1L, std::lround(viewport.x * scale));
        out["height"] = std::max(1L, std::lround(viewport.y * scale));
        return Answer::success(std::move(out));
    }

    std::optional<Answer> capture_op(const json::Value& id, const json::Value& args) {
        if (const std::string why = no_picture(); !why.empty()) {
            return Answer::failure("no picture: " + why, 409);
        }
        const int max_dim =
            static_cast<int>(std::clamp(number_arg(args, "max_dim", 1920.0), 16.0, 8192.0));
        const int quality =
            static_cast<int>(std::clamp(number_arg(args, "quality", 90.0), 1.0, 100.0));
        Result<std::pair<render::Image, std::string>> taken = picture(max_dim, quality);
        if (!taken.ok()) {
            return Answer::failure("no picture: " + taken.error().message, 409);
        }
        json::Value out;
        out["width"] = static_cast<std::int64_t>(taken.value().first.width);
        out["height"] = static_cast<std::int64_t>(taken.value().first.height);
        out["t"] = t_value();
        out["jpeg"] = base64_encode(taken.value().second);
        respond(id, std::move(out));
        return std::nullopt;
    }

    void tick_media(double t) {
        if (!recording || !session.playing()) {
            return;
        }
        if (t < next_frame_at) {
            return;
        }
        const double period = 1.0 / record_fps;
        next_frame_at =
            next_frame_at > 0.0 && t - next_frame_at < period ? next_frame_at + period : t + period;
        const std::int64_t n = frame_n++;
        Result<std::pair<render::Image, std::string>> taken =
            picture(record_max_dim, record_quality);
        if (!taken.ok()) {
            return; // (a gap in n)
        }
        json::Value frame;
        frame["type"] = "frame";
        frame["n"] = n;
        frame["t"] = t_value();
        frame["wall"] = unix_time();
        frame["width"] = static_cast<std::int64_t>(taken.value().first.width);
        frame["height"] = static_cast<std::int64_t>(taken.value().first.height);
        frame["jpeg"] = base64_encode(taken.value().second);
        send(frame);
        ++frames_sent;
    }

    // ---- Detectors (the addon's lib/detectors.gd) ----

    void install_log_watch() {
        previous_sink = log::sink();
        log::set_sink([this](log::Level level, std::string_view line) {
            if (level >= log::Level::Warn &&
                line.find(R"("msg":"autopilot: )") == std::string_view::npos) {
                const std::scoped_lock lock(log_mutex);
                if (log_lines.size() < 1000) {
                    log_lines.push_back({level, std::string(line)});
                }
            }
            if (previous_sink) {
                previous_sink(level, line);
            }
        });
    }

    void configure_detectors(const json::Value& args) {
        detectors_on = bool_arg(args, "enabled", detectors_on);
        long_fall_seconds = number_arg(args, "long_fall_seconds", long_fall_seconds);
        hitch_seconds = number_arg(args, "hitch_seconds", hitch_seconds);
        hud_every = number_arg(args, "hud_every", hud_every);
        scan_every = number_arg(args, "scan_every", scan_every);
        out_of_world_margin = number_arg(args, "out_of_world_margin", out_of_world_margin);
    }

    void finding(json::Value f) {
        const std::string signature = string_arg(f, "signature");
        const int n = ++counts[signature];
        if (n != 1 && n != 10 && n != 100 && n != 1000) {
            return;
        }
        f["t"] = t_value();
        f["count"] = n;
        f["source"] = "engine";
        json::Value event;
        event["kind"] = "finding";
        event["t"] = t_value();
        event["finding"] = std::move(f);
        emit(std::move(event));
    }

    void tick_detectors(double t, double real_dt) {
        std::vector<LogLine> lines;
        {
            const std::scoped_lock lock(log_mutex);
            lines.swap(log_lines);
        }
        for (const LogLine& entry : lines) {
            std::string message = entry.line;
            std::string where;
            if (Result<json::Value> doc = json::parse(entry.line);
                doc.ok() && doc.value().is_object()) {
                message = string_arg(doc.value(), "msg");
                for (const auto& [key, value] : doc.value().as_object()) {
                    if (key == "level" || key == "msg") {
                        continue;
                    }
                    where += (where.empty() ? "" : ", ") + key + "=" +
                             (value.is_string() ? value.as_string()
                                                : json::write(value, json::Style::Compact));
                }
            }
            const bool error = entry.level == log::Level::Error;
            std::string text = message;
            if (!where.empty()) {
                text += " (" + where + ")";
            }
            json::Value event;
            event["kind"] = "log";
            event["t"] = t_value();
            event["verbosity"] = error ? "Error" : "Warning";
            event["category"] = "Engine";
            event["message"] = text.substr(0, 2000);
            emit(std::move(event));
            if (error && detectors_on) {
                json::Value f;
                f["detector"] = "log_error";
                f["severity"] = "error";
                f["summary"] = "Engine error: " + message.substr(0, 300);
                f["signature"] = "log_error:" + normalized(message);
                f["details"] = json::Value(
                    json::Object{{"message", message}, {"where", where}, {"category", "Engine"}});
                finding(std::move(f));
            }
        }
        if (!detectors_on) {
            return;
        }
        const bool paused = !session.playing();
        const bool loading = t - scene_started < 3.0;
        if (!paused && !loading && real_dt > hitch_seconds) {
            std::array<char, 32> seconds{};
            (void)std::snprintf(seconds.data(), seconds.size(), "%.2f", real_dt);
            json::Value f;
            f["detector"] = "hitch";
            f["severity"] = "warning";
            f["summary"] = "A frame took " + std::string(seconds.data()) + " s";
            f["signature"] = "hitch";
            f["details"] = json::Value(json::Object{{"seconds", r2(real_dt)}});
            finding(std::move(f));
        }
        const bool scan_now = t - scan_at >= scan_every && !loading;
        const bool hud_now = t - hud_at >= hud_every;
        if (!scan_now && !hud_now && falling == 0.0 && !needs_fall_check()) {
            return;
        }
        long_fall(real_dt, paused);
        if (hud_now) {
            hud_at = t;
            hud_fraction();
        }
        if (scan_now) {
            scan_at = t;
            scan();
        }
    }

    bool needs_fall_check() {
        const auto& ecs = session.world().ecs();
        return !ecs.view<world::CharacterBody3D>().empty() ||
               !ecs.view<world::CharacterBody2D>().empty();
    }

    void long_fall(double real_dt, bool paused) {
        const Look look(session);
        const entt::entity p = look.player();
        if (p == entt::null) {
            return;
        }
        const auto& ecs = session.world().ecs();
        bool falling_now = false;
        if (const auto* body = ecs.try_get<world::CharacterBody3D>(p); body != nullptr) {
            falling_now = !body->on_floor && body->velocity.y < 0.0F;
        } else if (const auto* body2 = ecs.try_get<world::CharacterBody2D>(p); body2 != nullptr) {
            falling_now = !body2->on_floor && body2->velocity.y > 0.0F;
        } else {
            return;
        }
        if (paused) {
            return;
        }
        falling = falling_now ? falling + real_dt : 0.0;
        if (falling > long_fall_seconds) {
            falling = -1e9; // once per fall
            const std::string name = look.name(p);
            json::Value f;
            f["detector"] = "long_fall";
            f["severity"] = "error";
            f["summary"] = name + " kept falling for " +
                           std::to_string(static_cast<int>(long_fall_seconds)) + " s";
            f["signature"] = "long_fall:" + name;
            f["actor"] = name;
            f["where"] =
                look.is3d(p) ? detail::kit(look.position3(p)) : detail::kit2d(look.position2(p));
            finding(std::move(f));
        }
    }

    void hud_fraction() {
        const json::Value shown = detail::screen(session, json::Object{});
        const json::Value* texts = shown.find("texts");
        if (texts == nullptr || !texts->is_object()) {
            return;
        }
        static const std::regex kFraction(R"((\d+)\s*/\s*(\d+))");
        for (const auto& [key, value] : texts->as_object()) {
            if (!value.is_string()) {
                continue;
            }
            const std::string& text = value.as_string();
            for (auto it = std::sregex_iterator(text.begin(), text.end(), kFraction);
                 it != std::sregex_iterator(); ++it) {
                const std::string a_text = (*it)[1].str();
                const std::string b_text = (*it)[2].str();
                if (a_text.size() > 9 || b_text.size() > 9) {
                    continue;
                }
                const long a = std::strtol(a_text.c_str(), nullptr, 10);
                const long b = std::strtol(b_text.c_str(), nullptr, 10);
                if (b > 0 && a > b) {
                    json::Value f;
                    f["detector"] = "hud_fraction";
                    f["severity"] = "error";
                    std::string summary = "HUD text " + key;
                    summary += " shows " + a_text;
                    summary += "/" + b_text;
                    summary += ": more than its stated maximum";
                    f["summary"] = std::move(summary);
                    f["signature"] = "hud_fraction:" + key;
                    f["details"] = json::Value(json::Object{{"key", key},
                                                            {"text", text},
                                                            {"value", static_cast<std::int64_t>(a)},
                                                            {"max", static_cast<std::int64_t>(b)}});
                    finding(std::move(f));
                }
            }
        }
    }

    void scan() {
        const Look look(session);
        const auto& ecs = session.world().ecs();
        if (!kill_y && look.camera3d() != entt::null) {
            float low = std::numeric_limits<float>::infinity();
            for (const entt::entity e : look.entities()) {
                const detail::Box3 box = look.mesh_bounds(e);
                if (!box.empty) {
                    low = std::min(low, box.low.y);
                }
            }
            kill_y = (std::isfinite(low) ? low : 0.0F) - static_cast<float>(out_of_world_margin);
        }
        int checked = 0;
        for (const entt::entity e : look.entities()) {
            if (++checked > 5000) {
                break;
            }
            const std::string id = look.id(e);
            if (id.empty() || flagged.contains(id)) {
                continue;
            }
            const std::string name = look.name(e);
            if (look.is3d(e)) {
                const world::Affine3D g = look.global3(e);
                const bool finite = std::isfinite(g.origin.x) && std::isfinite(g.origin.y) &&
                                    std::isfinite(g.origin.z) && std::isfinite(g.x.x) &&
                                    std::isfinite(g.y.y) && std::isfinite(g.z.z);
                if (!finite) {
                    flagged.insert(id);
                    nan_finding(name);
                } else if (kill_y && g.origin.y < *kill_y &&
                           ecs.any_of<world::RigidBody3D, world::CharacterBody3D>(e)) {
                    flagged.insert(id);
                    std::array<char, 32> below{};
                    (void)std::snprintf(below.data(), below.size(), "%.0f",
                                        static_cast<double>(*kill_y));
                    json::Value f;
                    f["detector"] = "out_of_world";
                    f["severity"] = "error";
                    f["summary"] =
                        name + " fell out of the world (below " + std::string(below.data()) + " m)";
                    f["signature"] = "out_of_world:" + name;
                    f["actor"] = name;
                    f["where"] = detail::kit(g.origin);
                    finding(std::move(f));
                }
            } else if (look.is2d(e)) {
                const Vec2 p = look.position2(e);
                if (!std::isfinite(p.x) || !std::isfinite(p.y)) {
                    flagged.insert(id);
                    nan_finding(name);
                }
            }
        }
    }

    void nan_finding(const std::string& name) {
        json::Value f;
        f["detector"] = "nan";
        f["severity"] = "error";
        f["summary"] = name + " has a NaN or infinite transform";
        f["signature"] = "nan:" + name;
        f["actor"] = name;
        finding(std::move(f));
    }

    // ---- Requests ----

    void handle_request(const json::Value& frame) {
        json::Value id;
        if (const json::Value* given = frame.find("id"); given != nullptr) {
            id = *given;
            if (id.is_double() && id.is_integral()) {
                id = id.as_int();
            }
        }
        const std::string op = string_arg(frame, "op");
        json::Value args = json::Object{};
        if (const json::Value* given = frame.find("args"); given != nullptr && given->is_object()) {
            args = *given;
        }
        const std::optional<Answer> answer = dispatch(id, op, args);
        if (!answer) {
            return; // answered later
        }
        if (answer->ok) {
            respond(id, answer->value);
        } else {
            respond_error(id, answer->error, answer->status);
        }
    }

    std::optional<Answer> dispatch(const json::Value& id, const std::string& op,
                                   const json::Value& args) {
        if (op == "rc") {
            return detail::rc(session, string_arg(args, "verb"), args.find("body"));
        }
        if (op == "context") {
            return Answer::success(context());
        }
        if (op == "session") {
            return session_op(id, args);
        }
        if (op == "pause") {
            session.set_playing(!bool_arg(args, "paused", false));
            return Answer::success(json::Value(json::Object{{"paused", !session.playing()}}));
        }
        if (op == "input") {
            return input_op(id, args);
        }
        if (op == "key") {
            return key_op(id, args);
        }
        if (op == "move") {
            return move_op(id, args);
        }
        if (op == "watch") {
            return watch_op(id, args);
        }
        if (op == "view") {
            Look look(session);
            return detail::view(look, args);
        }
        if (op == "screen") {
            return Answer::success(detail::screen(session, args));
        }
        if (op == "ui") {
            return Answer::success(
                json::Value(json::Object{{"elements", detail::ui_list(session)}}));
        }
        if (op == "ui_act") {
            return ui_act(id, args);
        }
        if (op == "timestep") {
            json::Value out = json::Object{};
            if (bool_arg(args, "fixed", false)) {
                out["note"] =
                    "SlashEngine games always tick at a fixed rate: " +
                    std::to_string(static_cast<int>(std::lround(1.0 / last_tick_seconds))) +
                    " ticks a second";
            }
            return Answer::success(std::move(out));
        }
        if (op == "detectors") {
            configure_detectors(args);
            return Answer::success(json::Object{});
        }
        if (op == "ping") {
            json::Value out;
            out["game_time"] = r2(game_time);
            out["wall_time"] = unix_time();
            return Answer::success(std::move(out));
        }
        if (op == "record") {
            return record_op(args);
        }
        if (op == "capture") {
            return capture_op(id, args);
        }
        if (op == "codex_manifest") {
            return Answer::success(detail::codex_manifest(session, project_name));
        }
        if (op == "codex_read") {
            return Answer::success(detail::codex_read(session, args));
        }
        if (op == "find") {
            const Look look(session);
            return Answer::success(detail::find(look, args));
        }
        if (op == "transforms") {
            const Look look(session);
            return Answer::success(detail::transforms(look, args));
        }
        if (op == "pose") {
            const Look look(session);
            return detail::pose(look);
        }
        if (op == "movement") {
            const Look look(session);
            return detail::movement(look);
        }
        return Answer::failure("unknown op " + op);
    }

    // ---- Each poll ----

    void poll(double tick_seconds) {
        const double t = now();
        const double real_dt = last_real >= 0.0 ? t - last_real : 0.0;
        last_real = t;
        ++frames;
        ++fps_frames;
        if (t - fps_at >= 1.0) {
            fps = static_cast<double>(fps_frames) / (t - fps_at);
            fps_frames = 0;
            fps_at = t;
        }
        // Game time: the ticks that ran since the last poll.
        const std::uint64_t ticks = session.world().tick();
        if (ticks > last_world_tick) {
            game_time += static_cast<double>(ticks - last_world_tick) * last_tick_seconds;
        }
        last_world_tick = ticks;
        last_tick_seconds = tick_seconds > 0.0 ? tick_seconds : last_tick_seconds;
        if (session.scene_path() != scene) {
            if (launch_scene.empty()) {
                launch_scene = session.scene_path();
            }
            release_all();
            scene_changed();
        }
        flush_events();
        if (!finished || options.stay_open) {
            poll_link(t);
        }
        if (!active) {
            events.clear();
            return;
        }
        tick_keys(t, real_dt);
        for (std::size_t i = 0; i < pending.size();) {
            // (a tick may add more pending answers, so the vector is indexed afresh)
            std::function<bool(double)> const tick = pending[i].tick;
            if (tick(t)) {
                pending.erase(pending.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                ++i;
            }
        }
        tick_media(t);
        tick_detectors(t, real_dt);
        if (heartbeat_at < 0.0 || t - heartbeat_at >= 1.0) {
            heartbeat_at = t;
            json::Value beat;
            beat["kind"] = "heartbeat";
            beat["t"] = t_value();
            beat["fps"] = r2(fps);
            beat["paused"] = !session.playing();
            emit(std::move(beat));
        }
        flush_events();
    }
};

Bridge::Bridge(command::Session& session, Connector connect, Options options) {
    if (!options.clock) {
        options.clock = [start = std::chrono::steady_clock::now()]() {
            return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        };
    }
    state_ = std::make_unique<State>(session, std::move(connect), std::move(options));
    State& s = *state_;
    Result<runtime::Project> project = runtime::load_project(session.options().project_root);
    s.project_name = project.ok() ? project.value().name : std::string();
    s.launch_scene = session.scene_path();
    s.scene = session.scene_path();
    s.last_world_tick = session.world().tick();
    s.deadline = s.now() + s.options.connect_timeout;
    s.scene_started = s.now();
    s.fps_at = s.now();
    s.install_log_watch();
}

Bridge::~Bridge() {
    state_->release_all();
    log::set_sink(state_->previous_sink);
}

void Bridge::poll(double tick_seconds) {
    state_->poll(tick_seconds);
}

bool Bridge::welcomed() const {
    return state_->welcomed;
}

bool Bridge::finished() const {
    return state_->finished;
}

int Bridge::exit_code() const {
    return state_->exit_code;
}

const std::string& Bridge::report_dir() const {
    return state_->report_dir;
}

const json::Value& Bridge::result() const {
    return state_->result;
}

const std::string& Bridge::problem() const {
    return state_->problem;
}

} // namespace slash::autopilot
