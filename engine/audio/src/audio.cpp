// A world's sound: the "audio" system plays each AudioStreamPlayer and AudioStreamPlayer2D
// through the world's Server, as Godot's AudioStreamPlayerInternal and _update_panning do, then
// mixes the tick's share of samples.

#include "slash/audio/audio.hpp"

#include "slash/audio/effects.hpp"
#include "slash/core/log.hpp"
#include "slash/core/math.hpp"
#include "slash/core/text.hpp"
#include "slash/physics2d/physics2d.hpp"
#include "slash/ui/ui.hpp"
#include "slash/world/camera2d.hpp"
#include "slash/world/components.hpp"
#include "slash/world/tree2d.hpp"

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
    std::map<std::string, json::Value> parameters; // as last given to its voices
};

using Request = Snapshot::Request;

// The parameter interactive music reports back, as Godot's get_parameter does.
constexpr std::string_view kSwitchToClip = "switch_to_clip";

double level_db(float linear) {
    return linear > 0.0F ? std::max(static_cast<double>(linear_to_db(linear)), -200.0) : -200.0;
}

// Whether a stream is a microphone, which plays only as a player's own stream.
bool is_microphone(const LoadedStream& loaded) {
    return loaded.sounds.empty() && loaded.files.size() == 1 &&
           loaded.files.front()->kind == StreamFile::Kind::Microphone;
}

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
    // Streams play_stream started on polyphonic players, kept while one plays.
    std::map<std::string, LoadedStream, std::less<>> played;
    std::vector<Request> requests;
    std::vector<Snapshot::Recording> recordings;
    Sink sink;
    double carry = 0.0;
    std::vector<Frame> out;
    std::vector<History> history; // per bus

    // Microphone input: the device, tried once each time input turns on, and the stream agents
    // play in its place.
    InputDevice* input_device = nullptr;
    bool device_tried = false;
    bool device_started = false;
    std::string device_error;
    std::optional<std::string> input_ask; // set_input_stream's, for the next tick
    std::string input_stream;
    LoadedStream input_loaded;
    std::unique_ptr<Playback> input_playback; // until its stream ends
    std::vector<Frame> input_frames;
    float input_peak_db = -200.0F;

    void stop_device() {
        if (device_started) {
            input_device->stop();
        }
        device_tried = false;
        device_started = false;
    }

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
        server->set_input_enabled(settings.enable_input);
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
    std::shared_ptr<const Stream>
    stream_for(const std::string& path, std::string& error,
               std::map<std::string, LoadedStream, std::less<>>& in) const {
        const auto found = in.find(path);
        if (found != in.end() && !found->second.stale()) {
            return found->second.stream;
        }
        auto loaded = load_stream(*assets, path);
        if (!loaded) {
            error = loaded.error().message;
            return found != in.end() ? found->second.stream : nullptr;
        }
        auto& entry = in[path];
        entry = std::move(loaded).value();
        return entry.stream;
    }
    std::shared_ptr<const Stream> stream_for(const std::string& path, std::string& error) {
        return stream_for(path, error, streams);
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

// Where 2D sounds are heard from: the first current AudioListener2D by ID, and the current
// Camera2D's view.
struct Listener {
    bool found = false;
    Vec2 position;
    float rotation = 0.0F;
    world::Affine2D canvas; // world to screen pixels, as Godot's canvas transform
    Vec2 scale{1.0F, 1.0F}; // the canvas transform's scale
};

Listener find_listener(const world::World& world, const world::Tree2D& tree, Vec2 screen) {
    Listener listener;
    if (const world::CameraView view = world::camera_view(world, tree, screen);
        view.camera != entt::null) {
        listener.canvas = view.canvas;
        listener.scale = world::parts(view.canvas).scale;
    }
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
        // Where it is after its parents.
        const world::Transform2D placed = tree.global(entity);
        listener.position = placed.position;
        listener.rotation = placed.rotation;
    }
    return listener;
}

// Godot's AudioStreamPlayer2D::_update_panning, for one viewport.
Frame volume_2d(const world::AudioStreamPlayer2D& player, Vec2 global_pos, const Listener& listener,
                Vec2 screen_size, float global_panning_strength) {
    Vec2 listener_in_global;
    Vec2 relative_to_listener;
    if (listener.found) {
        listener_in_global = listener.position;
        const Vec2 d{global_pos.x - listener.position.x, global_pos.y - listener.position.y};
        const Vec2 cs = cos_sin(-listener.rotation);
        relative_to_listener = {d.x * cs.x - d.y * cs.y, d.x * cs.y + d.y * cs.x};
        // As the default listener's, scaled with the canvas.
        relative_to_listener = {relative_to_listener.x * listener.scale.x,
                                relative_to_listener.y * listener.scale.y};
    } else {
        // The middle of the screen, wherever the camera puts it.
        const Vec2 middle{screen_size.x * 0.5F, screen_size.y * 0.5F};
        listener_in_global = listener.canvas.inverse().apply(middle);
        const Vec2 on_screen = listener.canvas.apply(global_pos);
        relative_to_listener = {on_screen.x - middle.x, on_screen.y - middle.y};
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

Frame volume_of(const world::AudioStreamPlayer& player, const world::Tree2D& /*tree*/,
                entt::entity /*entity*/, const Listener& /*listener*/, const Settings& /*settings*/,
                Vec2 /*screen*/) {
    const float volume = db_to_linear(player.volume_db);
    return {volume, volume};
}

Frame volume_of(const world::AudioStreamPlayer2D& player, const world::Tree2D& tree,
                entt::entity entity, const Listener& listener, const Settings& settings,
                Vec2 screen) {
    return volume_2d(player, tree.global(entity).position, listener, screen,
                     settings.panning_strength_2d);
}

const std::string& bus_of(world::World& /*world*/, const world::AudioStreamPlayer& player,
                          const world::Tree2D& /*tree*/, entt::entity /*entity*/) {
    return player.bus;
}

// Godot's AudioStreamPlayer2D::_get_actual_bus: the first area by ID under the player whose
// layer is in its area_mask and which overrides the bus, else its own bus. Godot takes the
// areas in the physics server's order; ID order is the same on every run.
const std::string& bus_of(world::World& world, const world::AudioStreamPlayer2D& player,
                          const world::Tree2D& tree, entt::entity entity) {
    if (player.area_mask == 0) {
        return player.bus;
    }
    constexpr std::size_t kMaxIntersectAreas = 32; // Godot's MAX_INTERSECT_AREAS
    const physics2d::QueryFilter filter{.collision_mask = player.area_mask,
                                        .collide_with_bodies = false,
                                        .collide_with_areas = true,
                                        .exclude = {}};
    for (const entt::entity found : physics2d::overlap_point(world, tree.global(entity).position,
                                                             filter, kMaxIntersectAreas)) {
        const auto* area = world.ecs().try_get<world::Area2D>(found);
        if (area != nullptr && area->audio_bus_override) {
            return area->audio_bus_name;
        }
    }
    return player.bus;
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
    if (stream->is_monophonic()) {
        // Godot's play_basic: a monophonic stream starts over.
        stop_all(*state.server, player);
    }
    auto playback = stream->instantiate(state.server->mix_rate(), seed_of(key, ++player.plays));
    // As Godot's play_basic, a new playback gets every parameter before it starts.
    for (const auto& [name, value] : component.parameters) {
        playback->set_parameter(name, value);
    }
    const float pitch = component.pitch_scale > 0.0F ? component.pitch_scale : 1.0F;
    player.voices.push_back(state.server->start(std::move(playback), player.bus, player.volume,
                                                std::max(from_seconds, 0.0), pitch,
                                                component.stream_paused));
    // Godot's ensure_playback_limit.
    while (player.voices.size() > static_cast<std::size_t>(std::max(component.max_polyphony, 1))) {
        state.server->stop(player.voices.front());
        player.voices.erase(player.voices.begin());
    }
}

// play_stream's request: the stream plays on the player's newest voice, which starts first if
// the player isn't playing.
template <typename T>
void play_on(State& state, PlayerState& player, const T& component, std::string_view key,
             const Request& request) {
    if (player.voices.empty()) {
        start_voice(state, player, component, key, 0.0);
    }
    Playback* playback =
        player.voices.empty() ? nullptr : state.server->playback(player.voices.back());
    PolyphonicPlayback* polyphonic = playback != nullptr ? playback->polyphonic() : nullptr;
    if (polyphonic == nullptr) {
        log::warn("an audio player's stream isn't polyphonic, so it can't play another stream",
                  {{"player", std::string(key)}, {"stream", request.stream}});
        return;
    }
    std::string error;
    std::shared_ptr<const Stream> stream = state.stream_for(request.stream, error, state.played);
    if (stream == nullptr) {
        log::warn("a stream to play on a polyphonic player can't load",
                  {{"player", std::string(key)}, {"stream", request.stream}, {"error", error}});
        return;
    }
    if (polyphonic->play_stream(std::move(stream), request.from_seconds, request.volume_db,
                                request.pitch_scale) == PolyphonicPlayback::kInvalidId) {
        log::warn("a polyphonic player is playing as many streams as it can",
                  {{"player", std::string(key)}, {"polyphony", polyphonic->polyphony()}});
    }
}

template <typename T>
void reconcile(State& state, world::World& world, const world::Tree2D& tree, std::string_view kind,
               const Listener& listener, Vec2 screen,
               const std::map<std::string, std::vector<Request>>& requests) {
    auto& ecs = world.ecs();
    for (const entt::entity entity : ecs.view<T>()) {
        T& component = ecs.get<T>(entity);
        const std::string key = key_of(world, kind, entity);
        PlayerState& player = state.players[key];
        player.kind = kind;
        player.id = world.id_of(entity);
        player.entity = entity;
        player.seen = true;
        player.volume = volume_of(component, tree, entity, listener, state.settings, screen);
        player.bus = bus_of(world, component, tree, entity);
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
            state.server->set_bus_exclusive(id, player.bus, player.volume);
            state.server->set_pitch_scale(id, pitch);
        }
        if (component.stream_paused != player.paused) {
            player.paused = component.stream_paused;
            for (const Server::PlaybackId id : player.voices) {
                state.server->set_paused(id, player.paused);
            }
        }
        if (component.parameters != player.parameters) {
            // As Godot's AudioStreamPlayerInternal::set: each voice gets a changed parameter.
            for (const auto& [name, value] : component.parameters) {
                const auto was = player.parameters.find(name);
                if (was != player.parameters.end() && was->second == value) {
                    continue;
                }
                for (const Server::PlaybackId id : player.voices) {
                    if (Playback* playback = state.server->playback(id)) {
                        playback->set_parameter(name, value);
                    }
                }
            }
            player.parameters = component.parameters;
        }
        if (const auto asked = requests.find(key); asked != requests.end()) {
            // play() and stop() this tick decide, as calls after the field changed would.
            for (const Request& request : asked->second) {
                if (!request.stream.empty()) {
                    play_on(state, player, component, key, request);
                } else if (request.play) {
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

// A player's switch_to_clip follows the clip its newest voice is going to, so setting it to a
// clip the music left plays that clip again.
template <typename T>
void follow_clip(State& state, world::World& world, PlayerState& player) {
    T* component = world.ecs().try_get<T>(player.entity);
    if (component == nullptr || player.voices.empty()) {
        return;
    }
    const auto asked = component->parameters.find(std::string(kSwitchToClip));
    const Playback* playback = state.server->playback(player.voices.back());
    if (asked == component->parameters.end() || playback == nullptr) {
        return;
    }
    json::Value heading = playback->parameter(kSwitchToClip);
    if (heading.is_string() && heading != asked->second) {
        player.parameters[asked->first] = heading;
        asked->second = std::move(heading);
    }
}

// The tick's microphone input, pushed before the tick is mixed: the stream agents play into it,
// else what the device recorded, else silence. Input records only while it is on.
void feed_input(State& state, std::size_t frames) {
    Server& server = *state.server;
    if (state.input_ask) {
        state.input_stream = std::move(*state.input_ask);
        state.input_ask.reset();
        state.input_playback.reset();
        state.input_loaded = {};
        if (!state.input_stream.empty()) {
            if (auto loaded = load_stream(*state.assets, state.input_stream); loaded) {
                state.input_loaded = std::move(loaded).value();
                state.input_playback = state.input_loaded.stream->instantiate(
                    server.mix_rate(), seed_of(state.input_stream, 1));
                state.input_playback->start(0.0);
            } else {
                log::warn("the stream to play into the microphone can't load",
                          {{"stream", state.input_stream}, {"error", loaded.error().message}});
            }
        }
    }
    const bool on = server.input_active();
    const bool use_device = on && state.input_stream.empty() && state.input_device != nullptr;
    if (use_device && !state.device_tried) {
        // As Godot's input_start, once each time input turns on.
        state.device_tried = true;
        if (auto started = state.input_device->start(server.mix_rate()); started) {
            state.device_started = true;
            state.device_error.clear();
        } else {
            state.device_error = started.error().message;
            log::warn("the microphone can't record; input is silent",
                      {{"error", state.device_error}});
        }
    } else if (!use_device && state.device_tried) {
        state.stop_device();
    }
    state.input_peak_db = -200.0F;
    if (!on || frames == 0) {
        return;
    }
    state.input_frames.assign(frames, Frame{});
    if (state.input_playback) {
        const int count = static_cast<int>(frames);
        const int mixed = state.input_playback->mix(state.input_frames.data(), 1.0F, count);
        if (mixed < count) {
            std::fill(state.input_frames.begin() + std::max(mixed, 0), state.input_frames.end(),
                      Frame{});
            state.input_playback.reset();
        }
    } else if (state.device_started) {
        state.input_device->read(state.input_frames);
    }
    float peak = 0.0F;
    for (const Frame& frame : state.input_frames) {
        peak = std::max({peak, std::abs(frame.left), std::abs(frame.right)});
    }
    state.input_peak_db = static_cast<float>(level_db(peak));
    server.push_input(state.input_frames);
}

void run(State& state, world::World& world, double tick_seconds) {
    state.refresh_layout();

    std::map<std::string, std::vector<Request>> requests;
    for (Request& request : state.requests) {
        requests[request.player].push_back(std::move(request));
    }
    state.requests.clear();
    for (const Snapshot::Recording& asked : state.recordings) {
        if (RecordInstance* record = record_effect(*state.server, asked.bus, asked.effect)) {
            record->set_recording_active(asked.active);
        }
    }
    state.recordings.clear();

    for (auto& [key, player] : state.players) {
        player.seen = false;
    }
    const world::Tree2D tree(world);
    const Vec2 screen = ui::settings(world).viewport;
    const Listener listener = find_listener(world, tree, screen);
    reconcile<world::AudioStreamPlayer>(state, world, tree, kPlayer, listener, screen, requests);
    reconcile<world::AudioStreamPlayer2D>(state, world, tree, kPlayer2D, listener, screen,
                                          requests);
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
    feed_input(state, frames);
    state.out.resize(frames);
    state.server->mix(state.out);
    if (state.sink && frames > 0) {
        state.sink(state.out);
    }
    for (auto& [key, player] : state.players) {
        if (player.kind == kPlayer) {
            follow_clip<world::AudioStreamPlayer>(state, world, player);
        } else {
            follow_clip<world::AudioStreamPlayer2D>(state, world, player);
        }
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

    // Streams no player names are let go, and those played on polyphonic players once none
    // plays.
    std::erase_if(state.streams, [&](const auto& entry) {
        return std::ranges::none_of(state.players, [&](const auto& player) {
            return player.second.stream_path == entry.first;
        });
    });
    const bool polyphonic = std::ranges::any_of(state.players, [&](const auto& player) {
        return std::ranges::any_of(player.second.voices, [&](Server::PlaybackId id) {
            const Playback* playback = state.server->playback(id);
            return playback != nullptr && playback->polyphonic() != nullptr;
        });
    });
    if (!polyphonic) {
        state.played.clear();
    }
}

template <typename T>
Result<T> parse_json_file(std::string_view text, Result<T> (*parse)(const json::Value&)) {
    auto doc = json::parse(text);
    if (!doc) {
        return doc.error();
    }
    return parse(doc.value());
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
    properties["enable_input"]["type"] = "boolean";
    properties["enable_input"]["default"] = false;
    properties["enable_input"]["description"] =
        "Let AudioStreamMicrophone players record, as Godot's audio/driver/enable_input; exports "
        "then ask the player for the microphone.";
    properties["input_usage_description"]["type"] = "string";
    properties["input_usage_description"]["description"] =
        "Why the game records, shown when macOS and iOS ask the player for the microphone.";
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
    if (const json::Value* input = settings.find("enable_input"); input != nullptr) {
        out.enable_input = input->as_bool();
    }
    if (const json::Value* reason = settings.find("input_usage_description"); reason != nullptr) {
        out.input_usage_description = reason->as_string();
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
    // A new mixer: nothing plays, effects and the buses' recent sound start over.
    world.add_reset([state](world::World& /*world*/) {
        state->server.reset();
        state->players.clear();
        state->played.clear();
        state->requests.clear();
        state->recordings.clear();
        state->input_ask.reset();
        state->input_stream.clear();
        state->input_playback.reset();
        state->input_loaded = {};
        state->apply_settings();
    });
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

void set_input_device(world::World& world, InputDevice* device) {
    if (State* state = state_of(world); state != nullptr && state->input_device != device) {
        state->stop_device();
        state->device_error.clear();
        state->input_device = device;
    }
}

Result<void> set_input_stream(world::World& world, std::string_view stream) {
    State* state = state_of(world);
    if (state == nullptr) {
        return make_error(ErrorCode::InvalidArgument, "audio is not installed in this world");
    }
    if (!state->settings.enable_input) {
        return make_error(ErrorCode::InvalidArgument,
                          "microphone input is off; turn on audio.enable_input in project.json");
    }
    if (!stream.empty()) {
        auto loaded = load_stream(*state->assets, stream);
        if (!loaded) {
            return loaded.error();
        }
        if (is_microphone(loaded.value())) {
            return make_error(ErrorCode::InvalidArgument,
                              "\"" + std::string(stream) +
                                  "\" is a microphone; play a sound into the input instead");
        }
    }
    state->input_ask = std::string(stream);
    return {};
}

InputInfo input_info(const world::World& world) {
    InputInfo info;
    const State* state = state_of(world);
    if (state == nullptr) {
        return info;
    }
    info.enabled = state->settings.enable_input;
    info.active = state->server->input_active();
    const std::string& stream = state->input_ask ? *state->input_ask : state->input_stream;
    if (!stream.empty()) {
        info.source = stream;
    } else if (state->input_device != nullptr) {
        info.source = state->input_device->name();
    }
    info.error = state->device_error;
    info.peak_db = state->input_peak_db;
    return info;
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
            Request request;
            request.player = key_of(world, kind, entity);
            request.from_seconds = from_seconds;
            state->requests.push_back(std::move(request));
        }
    }
}

void stop(world::World& world, entt::entity entity) {
    State* state = state_of(world);
    if (state == nullptr) {
        return;
    }
    for (const std::string_view kind : {kPlayer, kPlayer2D}) {
        Request request;
        request.player = key_of(world, kind, entity);
        request.play = false;
        state->requests.push_back(std::move(request));
    }
}

Result<void> play_stream(world::World& world, entt::entity entity, std::string_view stream,
                         double from_seconds, float volume_db, float pitch_scale) {
    State* state = state_of(world);
    if (state == nullptr) {
        return make_error(ErrorCode::InvalidArgument, "audio is not installed in this world");
    }
    const auto* player = world.ecs().try_get<world::AudioStreamPlayer>(entity);
    const auto* player_2d = world.ecs().try_get<world::AudioStreamPlayer2D>(entity);
    if (player == nullptr && player_2d == nullptr) {
        return make_error(ErrorCode::InvalidArgument,
                          "it has no AudioStreamPlayer or AudioStreamPlayer2D");
    }
    const std::string& own = player != nullptr ? player->stream : player_2d->stream;
    bool polyphonic = false;
    if (own.ends_with(kStreamSuffix)) {
        const auto file = state->assets->load<StreamFile>(own);
        polyphonic = file && file.value()->kind == StreamFile::Kind::Polyphonic;
    }
    if (!polyphonic) {
        return make_error(ErrorCode::InvalidArgument,
                          "its stream \"" + own +
                              "\" isn't polyphonic; play_stream needs a .audio.json file with "
                              "\"polyphonic\"");
    }
    auto loaded = load_stream(*state->assets, stream);
    if (!loaded) {
        return loaded.error();
    }
    if (is_microphone(loaded.value())) {
        return make_error(ErrorCode::InvalidArgument,
                          "\"" + std::string(stream) +
                              "\" is a microphone, which plays only as a player's own stream");
    }
    for (const std::string_view kind : {kPlayer, kPlayer2D}) {
        if (kind == kPlayer ? player != nullptr : player_2d != nullptr) {
            Request request;
            request.player = key_of(world, kind, entity);
            request.stream = std::string(stream);
            request.from_seconds = from_seconds;
            request.volume_db = volume_db;
            request.pitch_scale = pitch_scale;
            state->requests.push_back(std::move(request));
        }
    }
    return {};
}

Playback* stream_playback(world::World& world, entt::entity entity) {
    State* state = state_of(world);
    if (state == nullptr) {
        return nullptr;
    }
    for (const std::string_view kind : {kPlayer, kPlayer2D}) {
        const auto found = state->players.find(key_of(world, kind, entity));
        if (found != state->players.end() && !found->second.voices.empty()) {
            return state->server->playback(found->second.voices.back());
        }
    }
    return nullptr;
}

void set_recording(world::World& world, int bus, int effect, bool active) {
    if (State* state = state_of(world)) {
        state->recordings.push_back({bus, effect, active});
    }
}

RecordingAsks recording_asks(const world::World& world, int bus, int effect) {
    RecordingAsks asks;
    if (const State* state = state_of(world)) {
        for (const Snapshot::Recording& asked : state->recordings) {
            if (asked.bus == bus && asked.effect == effect) {
                asks.active = asked.active;
                asks.restarts = asks.restarts || asked.active;
            }
        }
    }
    return asks;
}

Snapshot save(const world::World& world) {
    const State* state = state_of(world);
    return state != nullptr ? Snapshot{state->requests, state->recordings,
                                       state->server->layout().buses, state->input_ask}
                            : Snapshot{};
}

void restore(world::World& world, const Snapshot& snapshot) {
    State* state = state_of(world);
    if (state == nullptr) {
        return;
    }
    state->requests = snapshot.requests;
    state->recordings = snapshot.recordings;
    state->input_ask = snapshot.input_stream;
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
            if (const Playback* playback = state->server->playback(player.voices.back())) {
                info.clip = playback->clip();
                if (const GeneratorPlayback* generator = playback->generator()) {
                    info.generator =
                        PlayerInfo::Generator{generator->frames_available(), generator->skips()};
                }
                if (const PolyphonicPlayback* polyphonic = playback->polyphonic()) {
                    info.polyphonic = PlayerInfo::Polyphonic{polyphonic->streams_playing(),
                                                             polyphonic->polyphony()};
                }
            }
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
