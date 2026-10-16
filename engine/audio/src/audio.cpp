// A world's sound: the "audio" system plays each AudioStreamPlayer and AudioStreamPlayer2D
// through the world's Server, as Godot's AudioStreamPlayerInternal and _update_panning do, then
// mixes the tick's share of samples.

#include "slash/audio/audio.hpp"

#include "slash/core/log.hpp"
#include "slash/core/math.hpp"
#include "slash/core/text.hpp"
#include "slash/ui/ui.hpp"
#include "slash/world/components.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>
#include <utility>

namespace slash::audio {

namespace {

constexpr std::string_view kPlayer = "AudioStreamPlayer";
constexpr std::string_view kPlayer2D = "AudioStreamPlayer2D";

// What the audio system keeps for one player component, by kind and entity ID, which outlive the
// entity handles that restoring a copy of the world hands out anew.
struct PlayerState {
    std::string kind;
    std::string id;
    entt::entity entity = entt::null;
    std::string stream_path; // the stream its voices play
    std::string error;
    std::vector<Server::PlaybackId> voices; // oldest first
    bool known = false;                     // seen in an earlier tick
    bool playing = false;                   // the playing field as last seen or set
    bool paused = false;
    bool seen = false; // seen this tick
    std::string bus;
    Frame volume{};
    std::uint32_t plays = 0;
};

using Request = Snapshot::Request;

// A bus's last seconds of output, in a ring.
struct History {
    std::vector<Frame> frames;
    std::size_t next = 0;
    std::size_t filled = 0;

    void push(std::span<const Frame> in, std::size_t silent) {
        if (frames.empty()) {
            return;
        }
        const std::size_t count = in.empty() ? silent : in.size();
        for (std::size_t i = 0; i < count; ++i) {
            frames[next] = in.empty() ? Frame{} : in[i];
            next = (next + 1) % frames.size();
        }
        filled = std::min(frames.size(), filled + count);
    }

    // The last count frames, oldest first.
    [[nodiscard]] std::vector<Frame> last(std::size_t count) const {
        count = std::min(count, filled);
        std::vector<Frame> out(count);
        const std::size_t start =
            (next + frames.size() - count) % std::max<std::size_t>(frames.size(), 1);
        for (std::size_t i = 0; i < count; ++i) {
            out[i] = frames[(start + i) % frames.size()];
        }
        return out;
    }
};

struct State {
    Settings settings;
    asset::Assets* assets = nullptr;
    std::unique_ptr<Server> server;
    asset::Handle<BusLayout> layout;
    std::uint64_t layout_version = 0;
    std::string layout_error;
    std::map<std::string, PlayerState, std::less<>> players;
    std::map<std::string, LoadedStream, std::less<>> streams;
    std::vector<Request> requests;
    Sink sink;
    double carry = 0.0;
    std::vector<Frame> out;
    std::vector<History> history; // per bus

    void reset_history() {
        history.assign(server->layout().buses.size(), History{});
        const auto frames = static_cast<std::size_t>(settings.history_seconds * server->mix_rate());
        for (History& bus : history) {
            bus.frames.resize(frames);
        }
    }

    void apply_settings() {
        if (!server || server->mix_rate() != settings.mix_rate) {
            server = std::make_unique<Server>(std::max(settings.mix_rate, 8000));
            for (auto& [key, player] : players) {
                player.voices.clear();
            }
            carry = 0.0;
        }
        layout = {};
        layout_version = 0;
        layout_error.clear();
        server->set_layout(default_bus_layout());
        refresh_layout();
        reset_history();
        server->set_bus_listener(
            settings.history_seconds > 0
                ? Server::BusListener([this](int bus, std::span<const Frame> frames) {
                      if (bus >= 0 && static_cast<std::size_t>(bus) < history.size()) {
                          history[static_cast<std::size_t>(bus)].push(frames, Server::kMixStep);
                      }
                  })
                : Server::BusListener{});
    }

    // Loads the bus layout, and again when its file changes.
    void refresh_layout() {
        if (settings.bus_layout.empty()) {
            return;
        }
        if (!layout) {
            auto loaded = assets->load<BusLayout>(settings.bus_layout);
            if (!loaded) {
                if (layout_error != loaded.error().message) {
                    layout_error = loaded.error().message;
                    log::warn("the bus layout can't load; playing through Master alone",
                              {{"path", settings.bus_layout}, {"error", layout_error}});
                }
                return;
            }
            layout = std::move(loaded).value();
            layout_error.clear();
        }
        if (layout.version() != layout_version) {
            server->set_layout(*layout);
            layout_version = layout.version();
            reset_history();
        }
    }

    // The stream at a path, loaded again if a file it was made from changed.
    std::shared_ptr<const Stream> stream_for(const std::string& path, std::string& error) {
        const auto found = streams.find(path);
        if (found != streams.end() && !found->second.stale()) {
            return found->second.stream;
        }
        auto loaded = load_stream(*assets, path);
        if (!loaded) {
            error = loaded.error().message;
            return found != streams.end() ? found->second.stream : nullptr;
        }
        auto& entry = streams[path];
        entry = std::move(loaded).value();
        return entry.stream;
    }
};

// Stops each of a player's voices, fading them out.
void stop_all(Server& server, PlayerState& player) {
    for (const Server::PlaybackId id : player.voices) {
        server.stop(id);
    }
    player.voices.clear();
}

using Handle = std::shared_ptr<State>;

State* state_of(world::World& world) {
    auto* found = world.ecs().ctx().find<Handle>();
    return found != nullptr ? found->get() : nullptr;
}

const State* state_of(const world::World& world) {
    const auto* found = world.ecs().ctx().find<Handle>();
    return found != nullptr ? found->get() : nullptr;
}

std::string key_of(const world::World& world, std::string_view kind, entt::entity entity) {
    const std::string_view id = world.id_of(entity);
    return std::string(kind) + "/" +
           (id.empty() ? "#" + std::to_string(entt::to_integral(entity)) : std::string(id));
}

// A play's randomizer seed: FNV-1a over the player's key, mixed with how many times it played.
std::uint32_t seed_of(std::string_view key, std::uint32_t plays) {
    std::uint32_t h = 2166136261U;
    for (const char c : key) {
        h = (h ^ static_cast<unsigned char>(c)) * 16777619U;
    }
    h ^= plays * 0x9E3779B9U;
    h ^= h >> 16U;
    h *= 0x7feb352dU;
    h ^= h >> 15U;
    return h;
}

// Where 2D sounds are heard from: the first current AudioListener2D by ID.
struct Listener {
    bool found = false;
    Vec2 position;
    float rotation = 0.0F;
};

Listener find_listener(const world::World& world) {
    Listener listener;
    std::string best;
    const auto& ecs = world.ecs();
    for (const entt::entity entity : ecs.view<world::AudioListener2D>()) {
        if (!ecs.get<world::AudioListener2D>(entity).current) {
            continue;
        }
        const std::string key = key_of(world, "", entity);
        if (listener.found && key >= best) {
            continue;
        }
        best = key;
        listener.found = true;
        const auto* transform = ecs.try_get<world::Transform2D>(entity);
        listener.position = transform != nullptr ? transform->position : Vec2{};
        listener.rotation = transform != nullptr ? transform->rotation : 0.0F;
    }
    return listener;
}

// Godot's AudioStreamPlayer2D::_update_panning, for one viewport with no camera.
Frame volume_2d(const world::AudioStreamPlayer2D& player, Vec2 global_pos, const Listener& listener,
                Vec2 screen_size, float global_panning_strength) {
    Vec2 listener_in_global;
    Vec2 relative_to_listener;
    if (listener.found) {
        listener_in_global = listener.position;
        const Vec2 d{global_pos.x - listener.position.x, global_pos.y - listener.position.y};
        const Vec2 cs = cos_sin(-listener.rotation);
        relative_to_listener = {d.x * cs.x - d.y * cs.y, d.x * cs.y + d.y * cs.x};
    } else {
        listener_in_global = {screen_size.x * 0.5F, screen_size.y * 0.5F};
        relative_to_listener = {global_pos.x - listener_in_global.x,
                                global_pos.y - listener_in_global.y};
    }
    const float dx = global_pos.x - listener_in_global.x;
    const float dy = global_pos.y - listener_in_global.y;
    const float dist = std::sqrt(dx * dx + dy * dy);
    const float max_distance = std::max(player.max_distance, 0.01F);
    if (dist > max_distance) {
        return {}; // can't be heard
    }
    float multiplier = std::pow(1.0F - dist / max_distance, player.attenuation);
    multiplier *= db_to_linear(player.volume_db);
    float pan = screen_size.x > 0.0F ? relative_to_listener.x / screen_size.x : 0.0F;
    // The panning doesn't reach (far) beyond the screen.
    pan = std::clamp(pan, -1.0F, 1.0F);
    // The 0.5 lets the project's 2D and 3D panning strengths both default to 1.
    pan *= player.panning_strength * global_panning_strength * 0.5F;
    pan = static_cast<float>(std::clamp(pan + 0.5, 0.0, 1.0));
    const auto l = static_cast<float>(1.0 - pan);
    const float r = pan;
    return {l * multiplier, r * multiplier};
}

Frame volume_of(const world::AudioStreamPlayer& player, const world::World& /*world*/,
                entt::entity /*entity*/, const Listener& /*listener*/, const Settings& /*settings*/,
                Vec2 /*screen*/) {
    const float volume = db_to_linear(player.volume_db);
    return {volume, volume};
}

Frame volume_of(const world::AudioStreamPlayer2D& player, const world::World& world,
                entt::entity entity, const Listener& listener, const Settings& settings,
                Vec2 screen) {
    const auto* transform = world.ecs().try_get<world::Transform2D>(entity);
    return volume_2d(player, transform != nullptr ? transform->position : Vec2{}, listener, screen,
                     settings.panning_strength_2d);
}

template <typename T>
void start_voice(State& state, PlayerState& player, const T& component, std::string_view key,
                 double from_seconds) {
    if (component.stream.empty()) {
        player.error.clear();
        return;
    }
    std::string error;
    const std::shared_ptr<const Stream> stream = state.stream_for(component.stream, error);
    if (error != player.error && !error.empty()) {
        log::warn("an audio player's stream can't load",
                  {{"player", std::string(key)}, {"error", error}});
    }
    player.error = error;
    if (stream == nullptr) {
        return;
    }
    auto playback = stream->instantiate(state.server->mix_rate(), seed_of(key, ++player.plays));
    const float pitch = component.pitch_scale > 0.0F ? component.pitch_scale : 1.0F;
    player.voices.push_back(state.server->start(std::move(playback), component.bus, player.volume,
                                                std::max(from_seconds, 0.0), pitch,
                                                component.stream_paused));
    // Godot's ensure_playback_limit.
    while (player.voices.size() > static_cast<std::size_t>(std::max(component.max_polyphony, 1))) {
        state.server->stop(player.voices.front());
        player.voices.erase(player.voices.begin());
    }
}

template <typename T>
void reconcile(State& state, world::World& world, std::string_view kind, const Listener& listener,
               Vec2 screen, const std::map<std::string, std::vector<Request>>& requests) {
    auto& ecs = world.ecs();
    for (const entt::entity entity : ecs.view<T>()) {
        T& component = ecs.get<T>(entity);
        const std::string key = key_of(world, kind, entity);
        PlayerState& player = state.players[key];
        player.kind = kind;
        player.id = world.id_of(entity);
        player.entity = entity;
        player.seen = true;
        player.volume = volume_of(component, world, entity, listener, state.settings, screen);
        player.bus = component.bus;
        const float pitch = component.pitch_scale > 0.0F ? component.pitch_scale : 1.0F;

        bool play_now = false;
        if (!player.known) {
            player.known = true;
            player.stream_path = component.stream;
            player.paused = component.stream_paused;
            play_now = component.autoplay || component.playing;
        } else if (component.stream != player.stream_path) {
            // As Godot's set_stream: the old stream stops.
            stop_all(*state.server, player);
            player.stream_path = component.stream;
            player.error.clear();
            play_now = component.playing;
        } else if (component.playing != player.playing) {
            if (component.playing) {
                play_now = true;
            } else {
                stop_all(*state.server, player);
            }
        }
        for (const Server::PlaybackId id : player.voices) {
            state.server->set_bus_exclusive(id, component.bus, player.volume);
            state.server->set_pitch_scale(id, pitch);
        }
        if (component.stream_paused != player.paused) {
            player.paused = component.stream_paused;
            for (const Server::PlaybackId id : player.voices) {
                state.server->set_paused(id, player.paused);
            }
        }
        if (const auto asked = requests.find(key); asked != requests.end()) {
            // play() and stop() this tick decide, as calls after the field changed would.
            for (const Request& request : asked->second) {
                if (request.play) {
                    start_voice(state, player, component, key, request.from_seconds);
                } else {
                    stop_all(*state.server, player);
                }
            }
        } else if (play_now) {
            start_voice(state, player, component, key, 0.0);
        }
        player.playing = !player.voices.empty();
        component.playing = player.playing;
    }
}

void run(State& state, world::World& world, double tick_seconds) {
    state.refresh_layout();

    std::map<std::string, std::vector<Request>> requests;
    for (Request& request : state.requests) {
        requests[request.player].push_back(std::move(request));
    }
    state.requests.clear();

    for (auto& [key, player] : state.players) {
        player.seen = false;
    }
    const Listener listener = find_listener(world);
    const Vec2 screen = ui::settings(world).viewport;
    reconcile<world::AudioStreamPlayer>(state, world, kPlayer, listener, screen, requests);
    reconcile<world::AudioStreamPlayer2D>(state, world, kPlayer2D, listener, screen, requests);
    // A player whose entity or component went away stops, as Godot's on NOTIFICATION_PREDELETE.
    std::erase_if(state.players, [&](auto& entry) {
        if (!entry.second.seen) {
            stop_all(*state.server, entry.second);
        }
        return !entry.second.seen;
    });

    // This tick's share of frames, carrying the fraction to the next.
    const double exact = state.carry + state.server->mix_rate() * std::max(tick_seconds, 0.0);
    const auto frames = static_cast<std::size_t>(std::floor(exact));
    state.carry = exact - static_cast<double>(frames);
    state.out.resize(frames);
    state.server->mix(state.out);
    if (state.sink && frames > 0) {
        state.sink(state.out);
    }

    // Voices that ended are dropped, as Godot's AudioStreamPlayerInternal::process.
    for (auto& [key, player] : state.players) {
        const auto ended = std::erase_if(player.voices, [&](Server::PlaybackId id) {
            return !state.server->is_active(id) && !state.server->is_paused(id);
        });
        if (ended == 0) {
            continue;
        }
        if (player.voices.empty()) {
            player.playing = false;
            if (player.kind == kPlayer) {
                if (auto* component =
                        world.ecs().try_get<world::AudioStreamPlayer>(player.entity)) {
                    component->playing = false;
                }
            } else if (auto* component =
                           world.ecs().try_get<world::AudioStreamPlayer2D>(player.entity)) {
                component->playing = false;
            }
        }
        json::Value data;
        data["component"] = player.kind;
        world.emit("finished", player.id, std::move(data));
    }

    // Streams no player names are let go.
    std::erase_if(state.streams, [&](const auto& entry) {
        return std::ranges::none_of(state.players, [&](const auto& player) {
            return player.second.stream_path == entry.first;
        });
    });
}

template <typename T>
Result<T> parse_json_file(std::string_view text, Result<T> (*parse)(const json::Value&)) {
    auto doc = json::parse(text);
    if (!doc) {
        return doc.error();
    }
    return parse(doc.value());
}

double level_db(float linear) {
    return linear > 0.0F ? std::max(static_cast<double>(linear_to_db(linear)), -200.0) : -200.0;
}

json::Value number_setting(std::string_view doc, double minimum, double maximum, double fallback) {
    json::Value value;
    value["type"] = "number";
    value["minimum"] = minimum;
    value["maximum"] = maximum;
    value["default"] = fallback;
    value["description"] = doc;
    return value;
}

} // namespace

json::Value settings_schema() {
    json::Value schema;
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["description"] = "Sound settings, as Godot's audio project settings.";
    json::Value& properties = schema["properties"];
    properties["bus_layout"]["type"] = "string";
    properties["bus_layout"]["description"] =
        "A .bus_layout.json file with the buses players mix into, as Godot's "
        "audio/buses/default_bus_layout; none plays everything through Master.";
    properties["mix_rate"]["type"] = "integer";
    properties["mix_rate"]["minimum"] = 8000;
    properties["mix_rate"]["maximum"] = 192000;
    properties["mix_rate"]["default"] = Settings{}.mix_rate;
    properties["mix_rate"]["description"] = "Frames mixed per second, as Godot's "
                                            "audio/driver/mix_rate.";
    properties["output_latency"] =
        number_setting("Milliseconds of sound queued ahead of the speakers, as Godot's "
                       "audio/driver/output_latency. More is steadier when frames run late.",
                       1, 500, Settings{}.output_latency);
    properties["panning_strength_2d"] =
        number_setting("How far AudioStreamPlayer2D sounds pan left and right, as Godot's "
                       "audio/general/2d_panning_strength.",
                       0, 4, Settings{}.panning_strength_2d);
    return schema;
}

std::vector<json::ValidationError> validate_settings(const json::Value& settings,
                                                     std::string_view at) {
    std::vector<json::ValidationError> errors = json::validate(settings, settings_schema());
    for (json::ValidationError& error : errors) {
        error.path = std::string(at) + error.path;
    }
    if (!errors.empty()) {
        return errors;
    }
    if (const json::Value* layout = settings.find("bus_layout");
        layout != nullptr && !layout->as_string().empty()) {
        if (!is_project_path(layout->as_string())) {
            errors.push_back({std::string(at) + "/bus_layout",
                              "expected a path inside the project folder, separated by '/', "
                              "with no \".\" or \"..\" parts"});
        } else if (!layout->as_string().ends_with(kBusLayoutSuffix)) {
            errors.push_back({std::string(at) + "/bus_layout", "expected a .bus_layout.json file"});
        }
    }
    return errors;
}

Settings parse_settings(const json::Value& settings) {
    Settings out;
    if (!settings.is_object()) {
        return out;
    }
    if (const json::Value* layout = settings.find("bus_layout"); layout != nullptr) {
        out.bus_layout = layout->as_string();
    }
    if (const json::Value* rate = settings.find("mix_rate"); rate != nullptr) {
        out.mix_rate = static_cast<int>(rate->as_int());
    }
    if (const json::Value* latency = settings.find("output_latency"); latency != nullptr) {
        out.output_latency = static_cast<float>(latency->as_double());
    }
    if (const json::Value* panning = settings.find("panning_strength_2d"); panning != nullptr) {
        out.panning_strength_2d = static_cast<float>(panning->as_double());
    }
    return out;
}

void add_asset_types(asset::Assets& assets) {
    assets.add_type<Sound>(
        "sound", {".wav", ".ogg", ".mp3"},
        [](std::string_view text, std::string_view path) { return decode_sound(text, path); });
    assets.add_type<StreamFile>("audio_stream", {std::string(kStreamSuffix)},
                                [](std::string_view text, std::string_view /*path*/) {
                                    return parse_json_file<StreamFile>(text, parse_stream_file);
                                });
    assets.add_type<BusLayout>("bus_layout", {std::string(kBusLayoutSuffix)},
                               [](std::string_view text, std::string_view /*path*/) {
                                   return parse_json_file<BusLayout>(text, parse_bus_layout);
                               });
}

void install(world::World& world, asset::Assets& assets, const Settings& settings) {
    add_asset_types(assets);
    if (State* existing = state_of(world); existing != nullptr) {
        existing->assets = &assets;
        set_settings(world, settings);
        return;
    }
    auto state = std::make_shared<State>();
    state->assets = &assets;
    state->settings = settings;
    state->apply_settings();
    world.ecs().ctx().emplace<Handle>(state);
    world.add_system(
        "audio", [state](world::World& w, double tick_seconds) { run(*state, w, tick_seconds); },
        {.doc = "Plays each AudioStreamPlayer and AudioStreamPlayer2D and mixes the tick's sound "
                "through the bus layout.",
         .priority = 1200});
}

void set_settings(world::World& world, const Settings& settings) {
    if (State* state = state_of(world); state != nullptr && !(state->settings == settings)) {
        state->settings = settings;
        state->apply_settings();
    }
}

Settings settings(const world::World& world) {
    const State* state = state_of(world);
    return state != nullptr ? state->settings : Settings{};
}

Server* server(world::World& world) {
    State* state = state_of(world);
    return state != nullptr ? state->server.get() : nullptr;
}

const Server* server(const world::World& world) {
    const State* state = state_of(world);
    return state != nullptr ? state->server.get() : nullptr;
}

void set_sink(world::World& world, Sink sink) {
    if (State* state = state_of(world); state != nullptr) {
        state->sink = std::move(sink);
    }
}

void play(world::World& world, entt::entity entity, double from_seconds) {
    State* state = state_of(world);
    if (state == nullptr) {
        return;
    }
    for (const std::string_view kind : {kPlayer, kPlayer2D}) {
        const bool has = kind == kPlayer ? world.ecs().all_of<world::AudioStreamPlayer>(entity)
                                         : world.ecs().all_of<world::AudioStreamPlayer2D>(entity);
        if (has) {
            state->requests.push_back({key_of(world, kind, entity), true, from_seconds});
        }
    }
}

void stop(world::World& world, entt::entity entity) {
    State* state = state_of(world);
    if (state == nullptr) {
        return;
    }
    for (const std::string_view kind : {kPlayer, kPlayer2D}) {
        state->requests.push_back({key_of(world, kind, entity), false, 0.0});
    }
}

Snapshot save(const world::World& world) {
    const State* state = state_of(world);
    return state != nullptr ? Snapshot{state->requests, state->server->layout().buses} : Snapshot{};
}

void restore(world::World& world, const Snapshot& snapshot) {
    State* state = state_of(world);
    if (state == nullptr) {
        return;
    }
    state->requests = snapshot.requests;
    Server& server = *state->server;
    const std::vector<BusSettings>& now = server.layout().buses;
    if (now.size() != snapshot.buses.size()) {
        return; // the layout was replaced, not changed
    }
    for (std::size_t i = 0; i < now.size(); ++i) {
        const BusSettings& was = snapshot.buses[i];
        const auto bus = static_cast<int>(i);
        if (now[i].volume_db != was.volume_db) {
            server.set_bus_volume_db(bus, was.volume_db);
        }
        if (now[i].mute != was.mute) {
            server.set_bus_mute(bus, was.mute);
        }
        if (now[i].solo != was.solo) {
            server.set_bus_solo(bus, was.solo);
        }
        if (now[i].bypass_effects != was.bypass_effects) {
            server.set_bus_bypass_effects(bus, was.bypass_effects);
        }
        for (std::size_t e = 0; e < now[i].effects.size() && e < was.effects.size(); ++e) {
            if (now[i].effects[e].enabled != was.effects[e].enabled) {
                server.set_bus_effect_enabled(bus, static_cast<int>(e), was.effects[e].enabled);
            }
        }
    }
}

double playback_position(const world::World& world, entt::entity entity) {
    const State* state = state_of(world);
    if (state == nullptr) {
        return 0.0;
    }
    for (const std::string_view kind : {kPlayer, kPlayer2D}) {
        const auto found = state->players.find(key_of(world, kind, entity));
        if (found != state->players.end() && !found->second.voices.empty()) {
            return state->server->position(found->second.voices.back());
        }
    }
    return 0.0;
}

std::vector<PlayerInfo> players(const world::World& world) {
    std::vector<PlayerInfo> out;
    const State* state = state_of(world);
    if (state == nullptr) {
        return out;
    }
    for (const auto& [key, player] : state->players) {
        if (player.id.empty()) {
            continue;
        }
        PlayerInfo info;
        info.id = player.id;
        info.kind = player.kind;
        info.stream = player.stream_path;
        info.bus = state->server->bus_index(player.bus) >= 0 ? player.bus : "Master";
        info.voices = static_cast<int>(player.voices.size());
        info.paused = player.paused && !player.voices.empty();
        if (!player.voices.empty()) {
            info.position = state->server->position(player.voices.back());
        }
        if (const auto found = state->streams.find(player.stream_path);
            found != state->streams.end()) {
            info.length = found->second.stream->length();
        }
        info.volume = player.volume;
        info.error = player.error;
        out.push_back(std::move(info));
    }
    std::ranges::sort(out, [](const PlayerInfo& a, const PlayerInfo& b) {
        return std::tie(a.id, a.kind) < std::tie(b.id, b.kind);
    });
    return out;
}

Result<Capture> capture(const world::World& world, std::string_view bus, double seconds, int slices,
                        bool frames) {
    const State* state = state_of(world);
    if (state == nullptr) {
        return make_error(ErrorCode::InvalidArgument, "audio is not installed in this world");
    }
    const int index = state->server->bus_index(bus);
    if (index < 0) {
        std::vector<std::string> names;
        for (const BusSettings& each : state->server->layout().buses) {
            names.push_back(each.name);
        }
        std::string message = "no bus is named \"" + std::string(bus) + "\"";
        if (const std::string_view hint = closest_match(bus, names); !hint.empty()) {
            message += "; did you mean \"" + std::string(hint) + "\"?";
        }
        return make_error(ErrorCode::NotFound, std::move(message));
    }
    const History& history = state->history[static_cast<std::size_t>(index)];
    const auto wanted =
        static_cast<std::size_t>(std::llround(std::max(seconds, 0.0) * state->server->mix_rate()));
    std::vector<Frame> kept = history.last(wanted);

    Capture out;
    out.seconds = static_cast<double>(kept.size()) / state->server->mix_rate();
    float peak = 0.0F;
    double sum = 0.0;
    for (const Frame& frame : kept) {
        peak = std::max({peak, std::abs(frame.left), std::abs(frame.right)});
        sum += static_cast<double>(frame.left) * frame.left +
               static_cast<double>(frame.right) * frame.right;
    }
    out.peak_db = static_cast<float>(level_db(peak));
    out.rms_db = kept.empty() ? -200.0F
                              : static_cast<float>(level_db(static_cast<float>(
                                    std::sqrt(sum / (2.0 * static_cast<double>(kept.size()))))));
    const auto count =
        std::min<std::size_t>(static_cast<std::size_t>(std::max(slices, 0)), kept.size());
    for (std::size_t s = 0; s < count; ++s) {
        const std::size_t begin = s * kept.size() / count;
        const std::size_t end = (s + 1) * kept.size() / count;
        float slice_peak = 0.0F;
        for (std::size_t i = begin; i < end; ++i) {
            slice_peak = std::max({slice_peak, std::abs(kept[i].left), std::abs(kept[i].right)});
        }
        out.slice_peaks_db.push_back(static_cast<float>(level_db(slice_peak)));
    }
    if (frames) {
        out.frames = std::move(kept);
    }
    return out;
}

} // namespace slash::audio
