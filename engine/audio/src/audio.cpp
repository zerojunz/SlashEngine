// A world's sound: the "audio" system plays each AudioStreamPlayer, AudioStreamPlayer2D and
// AudioStreamPlayer3D through the world's Server, as Godot's AudioStreamPlayerInternal and
// _update_panning do, then mixes the tick's share of samples.

#include "slash/audio/audio.hpp"

#include "slash/audio/effects.hpp"
#include "slash/core/log.hpp"
#include "slash/core/math.hpp"
#include "slash/core/text.hpp"
#include "slash/physics2d/physics2d.hpp"
#include "slash/physics3d/physics3d.hpp"
#include "slash/ui/ui.hpp"
#include "slash/world/camera2d.hpp"
#include "slash/world/components.hpp"
#include "slash/world/tree2d.hpp"
#include "slash/world/tree3d.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace slash::audio {

namespace {

constexpr std::string_view kPlayer = "AudioStreamPlayer";
constexpr std::string_view kPlayer2D = "AudioStreamPlayer2D";
constexpr std::string_view kPlayer3D = "AudioStreamPlayer3D";

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
    std::string bus;   // where a new voice starts, at volume
    Frame volume{};
    std::vector<BusVolume> mix; // where its voices mix now
    float pitch = 1.0F;         // its voices' pitch, after doppler
    float highshelf = 0.0F;     // a 3D player's filter gain, linear; 0 for none
    float cutoff_hz = 0.0F;
    std::uint32_t plays = 0;
    std::map<std::string, json::Value> parameters; // as last given to its voices
    std::string clip; // the music clip it last said started, until its music stops
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

// Godot's VelocityTracker3D, counting ticks: where an entity was over its last four moves, each
// with the tick it was there, and how fast it went over at most the last fifth of a second.
struct VelocityTracker {
    struct Position {
        Vec3 position;
        std::uint64_t tick = 0;
    };
    std::array<Position, 4> history{};
    std::size_t length = 0;
    std::string mode;          // the doppler_tracking it follows
    world::Affine3D transform; // the entity's global transform when last seen
    bool seen = false;         // seen this tick

    void reset(Vec3 position, std::uint64_t tick) {
        history[0] = {position, tick};
        length = 1;
    }

    void update(Vec3 position, std::uint64_t tick) {
        if (length == 0 || history[0].tick != tick) { // in the same tick, the latest counts
            length = std::min(history.size(), length + 1);
            for (std::size_t i = length - 1; i > 0; --i) {
                history[i] = history[i - 1];
            }
        }
        history[0] = {position, tick};
    }

    [[nodiscard]] Vec3 velocity(std::uint64_t now, double tick_seconds) const {
        constexpr double kMaxTime = 1 / 5.0; // the longest it averages over
        Vec3 distance_accum;
        double time_accum = 0.0;
        const double base_time =
            length > 0 ? static_cast<double>(now - history[0].tick) * tick_seconds : 0.0;
        for (std::size_t i = 0; i + 1 < length; ++i) {
            const double delta =
                static_cast<double>(history[i].tick - history[i + 1].tick) * tick_seconds;
            if (base_time + time_accum + delta > kMaxTime) {
                break;
            }
            distance_accum = distance_accum + (history[i].position - history[i + 1].position);
            time_accum += delta;
        }
        if (time_accum == 0.0) {
            return {};
        }
        const auto time = static_cast<float>(time_accum);
        return {distance_accum.x / time, distance_accum.y / time, distance_accum.z / time};
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
    // Doppler: how fast 3D players, listeners and cameras that track it move, by
    // "<kind>/<entity ID>", and the ticks run.
    std::map<std::string, VelocityTracker, std::less<>> trackers;
    std::uint64_t ticks = 0;
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

    // The buses' recent sound kept for capture, as items, and the buffers mixing uses. Sounds
    // are assets, counted with them.
    [[nodiscard]] world::MemoryPart memory() const {
        world::MemoryPart part{"audio", 0, {}, {}};
        for (std::size_t i = 0; i < history.size(); ++i) {
            const std::uint64_t bytes = history[i].frames.capacity() * sizeof(Frame);
            const std::string name = server != nullptr && i < server->layout().buses.size()
                                         ? server->layout().buses[i].name
                                         : std::to_string(i);
            part.items.push_back({"history " + name, bytes, history[i].frames.size()});
            part.bytes += bytes;
        }
        part.bytes += (out.capacity() + input_frames.capacity()) * sizeof(Frame);
        part.counts = {{"buses", history.size()},
                       {"players", players.size()},
                       {"streams", streams.size() + played.size()}};
        return part;
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

// Where 3D sounds are heard from: the current AudioListener3D, else the current Camera3D.
struct Listener3D {
    bool found = false;
    world::Affine3D transform; // after its parents'
    Vec3 velocity;             // how fast it moves, when it tracks that for doppler
};

// What placing players needs this tick.
struct Hearing {
    const world::Tree2D* tree2d = nullptr;
    Listener listener2d;
    Vec2 screen;
    const world::Tree3D* tree3d = nullptr;
    Listener3D listener3d;
    bool areas3d = false; // whether the world has an Area3D a 3D sound could be in
    double tick_seconds = 0.0;
};

// Godot's Viewport::get_audio_listener_3d, falling back to its camera: the first current
// AudioListener3D by ID, else the first by ID (the first listener in a viewport is current), else
// the current Camera3D.
Listener3D find_listener_3d(const State& state, const world::World& world,
                            const world::Tree3D& tree, double tick_seconds) {
    const auto& ecs = world.ecs();
    std::vector<std::pair<std::string, entt::entity>> listeners;
    for (const entt::entity entity : ecs.view<world::AudioListener3D>()) {
        listeners.emplace_back(key_of(world, "", entity), entity);
    }
    std::ranges::sort(listeners);
    entt::entity node = entt::null;
    for (const auto& [key, entity] : listeners) {
        if (ecs.get<world::AudioListener3D>(entity).current) {
            node = entity;
            break;
        }
    }
    if (node == entt::null && !listeners.empty()) {
        node = listeners.front().second;
    }
    std::string_view kind = "AudioListener3D";
    if (node == entt::null) {
        node = world::current_camera(world, tree);
        kind = "Camera3D";
    }
    Listener3D listener;
    if (node == entt::null) {
        return listener;
    }
    listener.found = true;
    listener.transform = tree.global(node);
    if (const auto tracker = state.trackers.find(key_of(world, kind, node));
        tracker != state.trackers.end()) {
        listener.velocity = tracker->second.velocity(state.ticks, tick_seconds);
    }
    return listener;
}

// Moves the velocity trackers of the entities with a T that track doppler on to where they are,
// as Godot's NOTIFICATION_TRANSFORM_CHANGED does; set_doppler_tracking's reset is a tracker made
// anew.
template <typename T>
void track(State& state, const world::World& world, const world::Tree3D& tree,
           std::string_view kind) {
    const auto& ecs = world.ecs();
    for (const entt::entity entity : ecs.view<T>()) {
        const std::string& mode = ecs.get<T>(entity).doppler_tracking;
        if (mode != "idle_step" && mode != "physics_step") {
            continue;
        }
        const world::Affine3D global = tree.global(entity);
        auto [found, added] = state.trackers.try_emplace(key_of(world, kind, entity));
        VelocityTracker& tracker = found->second;
        tracker.seen = true;
        if (added || tracker.mode != mode) {
            tracker.mode = mode;
            tracker.reset(global.origin, state.ticks);
        } else if (!(global == tracker.transform)) {
            tracker.update(global.origin, state.ticks);
        }
        tracker.transform = global;
    }
}

// A bus by name as Godot's Area3D::get_audio_bus_name gives it: Master when there is no such bus.
std::string known_bus(const Server& server, const std::string& name) {
    return server.bus_index(name) >= 0 ? name : std::string("Master");
}

float dot3(Vec3 a, Vec3 b) {
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

float length3(Vec3 v) {
    return std::sqrt(dot3(v, v));
}

// Godot's Vector3::normalized: zero stays zero.
Vec3 unit(Vec3 v) {
    const float length_sq = dot3(v, v);
    if (length_sq == 0.0F) {
        return {};
    }
    const float length = std::sqrt(length_sq);
    return {v.x / length, v.y / length, v.z / length};
}

Frame scaled(Frame frame, float s) {
    return {frame.left * s, frame.right * s};
}

// Godot's AudioFrame::lerp.
Frame lerp(Frame a, Frame b, float t) {
    return {a.left + (t * (b.left - a.left)), a.right + (t * (b.right - a.right))};
}

// Godot's AudioStreamPlayer3D::_get_attenuation_db: the player's volume at a distance from the
// listener, in decibels, at most max_db. Godot works it in doubles.
float attenuation_db(const world::AudioStreamPlayer3D& player, float distance) {
    constexpr double kCmpEpsilon = 0.00001;
    // Godot's Math::linear_to_db for a double.
    const auto to_db = [](double linear) {
        return std::log(linear) * 8.6858896380650365530225783783321;
    };
    float att = 0.0F;
    if (player.attenuation_model == "inverse_distance") {
        att = static_cast<float>(to_db(1.0 / ((distance / player.unit_size) + kCmpEpsilon)));
    } else if (player.attenuation_model == "inverse_square_distance") {
        float d = distance / player.unit_size;
        d *= d;
        att = static_cast<float>(to_db(1.0 / (d + kCmpEpsilon)));
    } else if (player.attenuation_model == "logarithmic") {
        att = static_cast<float>(-20 * std::log((distance / player.unit_size) + kCmpEpsilon));
    }
    att += player.volume_db;
    if (att > player.max_db) {
        att = player.max_db;
    }
    return att;
}

// Godot's AudioStreamPlayer3D::_calc_output_vol_stereo: each side's volume is the cosine of half
// the angle from the sound to that side, ignoring height, scaled so the louder side is at most
// 1 / (1 - panning_strength) times the other.
Frame output_vol_stereo(Vec3 source_dir, float panning_strength) {
    const double flatrad = std::sqrt(
        static_cast<double>((source_dir.x * source_dir.x) + (source_dir.z * source_dir.z)));
    const double g = std::clamp((1.0 - panning_strength) * (1.0 - panning_strength), 0.0, 1.0);
    const double f = (1.0 - g) / (1.0 + g);
    const double cosx = std::clamp(source_dir.x / (flatrad == 0.0 ? 1.0 : flatrad), -1.0, 1.0);
    const double fcosx = cosx * f;
    return {static_cast<float>(std::sqrt((-fcosx + 1.0) / 2.0)),
            static_cast<float>(std::sqrt((fcosx + 1.0) / 2.0))};
}

// Godot's AudioStreamPlayer3D::_calc_reverb_vol in stereo: the send to an area's reverb bus,
// from the sound's direction or, with uniformity, from the area's nearest point around the
// listener.
Frame reverb_vol(const world::AudioStreamPlayer3D& player, const world::Area3D& area,
                 Vec3 listener_area_pos, Frame direct_path_vol) {
    const float uniformity = area.reverb_bus_uniformity;
    const float area_send = area.reverb_bus_amount;
    if (uniformity <= 0.0F) {
        return scaled(direct_path_vol, area_send);
    }
    const float distance = length3(listener_area_pos);
    const float attenuation = db_to_linear(attenuation_db(player, distance));
    // The share of the sound from each speaker if all were driven alike: half each in stereo.
    const Frame center_frame{0.5F, 0.5F};
    Frame reverb = center_frame;
    if (attenuation < 1.0F) {
        // Pan the uniform sound.
        Vec3 rev_pos = listener_area_pos;
        rev_pos.y = 0;
        rev_pos = unit(rev_pos);
        const auto c = static_cast<float>((rev_pos.x * 0.5) + 0.5);
        reverb = lerp({1.0F - c, c}, center_frame, attenuation);
    }
    return scaled(lerp(direct_path_vol, scaled(reverb, attenuation), uniformity), area_send);
}

// Godot's AudioStreamPlayer3D::_get_overriding_area: the first area by ID under the player whose
// layer is in its area_mask and which overrides the bus or has a reverb bus. Godot takes the
// areas in the physics server's order; ID order is the same on every run.
entt::entity overriding_area(world::World& world, const world::AudioStreamPlayer3D& player,
                             Vec3 position) {
    constexpr std::size_t kMaxIntersectAreas = 32; // Godot's MAX_INTERSECT_AREAS
    const physics3d::QueryFilter filter{.collision_mask = player.area_mask,
                                        .collide_with_bodies = false,
                                        .collide_with_areas = true,
                                        .exclude = {}};
    for (const entt::entity found :
         physics3d::overlap_point(world, position, filter, kMaxIntersectAreas)) {
        const auto* area = world.ecs().try_get<world::Area3D>(found);
        if (area != nullptr && (area->audio_bus_override || area->reverb_bus_enabled)) {
            return found;
        }
    }
    return entt::null;
}

float pitch_of(float pitch_scale) {
    return pitch_scale > 0.0F ? pitch_scale : 1.0F;
}

// Where a player's voices start and mix this tick, at what volume and pitch.
void place(State& /*state*/, world::World& /*world*/, const Hearing& /*hearing*/,
           const world::AudioStreamPlayer& player, entt::entity /*entity*/, PlayerState& out) {
    const float volume = db_to_linear(player.volume_db);
    out.bus = player.bus;
    out.volume = {volume, volume};
    out.mix.assign(1, {out.bus, out.volume});
    out.pitch = pitch_of(player.pitch_scale);
}

void place(State& state, world::World& world, const Hearing& hearing,
           const world::AudioStreamPlayer2D& player, entt::entity entity, PlayerState& out) {
    const Vec2 position = hearing.tree2d->global(entity).position;
    out.volume = volume_2d(player, position, hearing.listener2d, hearing.screen,
                           state.settings.panning_strength_2d);
    // Godot's AudioStreamPlayer2D::_get_actual_bus: the first area by ID under the player whose
    // layer is in its area_mask and which overrides the bus, else its own bus. Godot takes the
    // areas in the physics server's order; ID order is the same on every run.
    out.bus = player.bus;
    if (player.area_mask != 0) {
        constexpr std::size_t kMaxIntersectAreas = 32; // Godot's MAX_INTERSECT_AREAS
        const physics2d::QueryFilter filter{.collision_mask = player.area_mask,
                                            .collide_with_bodies = false,
                                            .collide_with_areas = true,
                                            .exclude = {}};
        for (const entt::entity found :
             physics2d::overlap_point(world, position, filter, kMaxIntersectAreas)) {
            const auto* area = world.ecs().try_get<world::Area2D>(found);
            if (area != nullptr && area->audio_bus_override) {
                out.bus = area->audio_bus_name;
                break;
            }
        }
    }
    out.mix.assign(1, {out.bus, out.volume});
    out.pitch = pitch_of(player.pitch_scale);
}

// Godot's AudioStreamPlayer3D::_update_panning, for one listener, in stereo.
void place(State& state, world::World& world, const Hearing& hearing,
           const world::AudioStreamPlayer3D& player, entt::entity entity, PlayerState& out) {
    const float pitch_scale = pitch_of(player.pitch_scale);
    if (!out.known) {
        out.pitch = pitch_scale;
    }
    const world::Affine3D global = hearing.tree3d->global(entity);
    const Vec3 global_pos = global.origin;
    const entt::entity area_entity = hearing.areas3d && player.area_mask != 0
                                         ? overriding_area(world, player, global_pos)
                                         : entt::entity{entt::null};
    const world::Area3D* area =
        area_entity != entt::null ? &world.ecs().get<world::Area3D>(area_entity) : nullptr;
    // _get_actual_bus: where a voice starts.
    out.bus = area != nullptr && area->audio_bus_override && !area->reverb_bus_enabled
                  ? known_bus(*state.server, area->audio_bus_name)
                  : player.bus;
    out.volume = {};
    out.mix.clear();
    const Listener3D& listener = hearing.listener3d;
    if (!listener.found) {
        out.pitch = pitch_scale;
        return; // nothing hears it
    }
    const world::Affine3D listener_basis = world::orthonormalized(listener.transform);
    const Vec3 local_pos = listener_basis.inverse().apply(global_pos);
    const float dist = length3(local_pos);

    Vec3 listener_area_pos;
    const bool uniform_reverb =
        area != nullptr && area->reverb_bus_enabled && area->reverb_bus_uniformity > 0.0F;
    if (uniform_reverb) {
        const Vec3 area_sound_pos =
            physics3d::closest_point(world, area_entity, listener.transform.origin)
                .value_or(listener.transform.origin);
        listener_area_pos = listener.transform.inverse().apply(area_sound_pos);
    }
    if (player.max_distance > 0.0F) {
        float total_max = player.max_distance;
        if (uniform_reverb) {
            total_max = std::max(total_max, length3(listener_area_pos));
        }
        // As Godot's: an area's reverb reaching past max_distance silences it too.
        if (dist > total_max || total_max > player.max_distance) {
            return; // can't be heard; the filter and pitch stay as they were
        }
    }

    float multiplier = db_to_linear(attenuation_db(player, dist));
    if (player.max_distance > 0.0F) {
        multiplier =
            static_cast<float>(multiplier * std::max(0.0, 1.0 - (dist / player.max_distance)));
    }
    auto db_att = static_cast<float>((1.0 - std::min(1.0F, multiplier)) *
                                     static_cast<double>(player.attenuation_filter_db));
    if (player.emission_angle_enabled) {
        const Vec3 listener_to_pos = global_pos - listener.transform.origin;
        const float c = dot3(unit(listener_to_pos), unit(global.z)); // it's z negative
        const float angle = std::acos(std::clamp(c, -1.0F, 1.0F)) * 57.29577951308232F;
        if (angle > player.emission_angle_degrees) {
            db_att -= -player.emission_angle_filter_attenuation_db;
        }
    }
    out.highshelf = db_to_linear(db_att);
    out.cutoff_hz = player.attenuation_filter_cutoff_hz;

    const Frame output = scaled(
        output_vol_stereo(local_pos, state.settings.panning_strength_3d * player.panning_strength),
        multiplier);
    out.volume = output;
    // Godot's bus map: a bus named twice keeps the last volume, in the first one's place.
    const auto set_bus = [&](const std::string& bus, Frame volume) {
        const auto found = std::ranges::find(out.mix, bus, &BusVolume::bus);
        if (found != out.mix.end()) {
            found->volume = volume;
        } else {
            out.mix.push_back({bus, volume});
        }
    };
    if (area != nullptr) {
        // As Godot's: with a reverb bus and no bus override, it plays only through the reverb.
        if (area->audio_bus_override) {
            set_bus(known_bus(*state.server, area->audio_bus_name), output);
        }
        if (area->reverb_bus_enabled) {
            set_bus(known_bus(*state.server, area->reverb_bus_name),
                    reverb_vol(player, *area, listener_area_pos, output));
        }
    } else {
        set_bus(player.bus, output);
    }

    out.pitch = pitch_scale;
    if (player.doppler_tracking != "disabled") {
        const auto tracker = state.trackers.find(key_of(world, kPlayer3D, entity));
        const Vec3 linear_velocity =
            tracker != state.trackers.end()
                ? tracker->second.velocity(state.ticks, hearing.tick_seconds)
                : Vec3{};
        const Vec3 relative = linear_velocity - listener.velocity;
        const Vec3 local_velocity{dot3(listener_basis.x, relative),
                                  dot3(listener_basis.y, relative),
                                  dot3(listener_basis.z, relative)};
        if (!(local_velocity == Vec3{})) {
            const float approaching = dot3(unit(local_pos), unit(local_velocity));
            const float velocity = length3(local_velocity);
            constexpr float kSpeedOfSound = 343.0F;
            const float doppler_pitch_scale =
                pitch_scale * kSpeedOfSound / (kSpeedOfSound + (velocity * approaching));
            out.pitch = std::clamp(doppler_pitch_scale, 1 / 8.0F, 8.0F); // avoid crazy stuff
        }
    }
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
    const Server::PlaybackId id =
        state.server->start(std::move(playback), player.bus, player.volume,
                            std::max(from_seconds, 0.0), player.pitch, component.stream_paused);
    if (player.highshelf != 0.0F) {
        state.server->set_highshelf(id, player.highshelf, player.cutoff_hz);
    }
    player.voices.push_back(id);
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
void reconcile(State& state, world::World& world, const Hearing& hearing, std::string_view kind,
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
        place(state, world, hearing, component, entity, player);

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
            state.server->set_bus_volumes(id, player.mix);
            state.server->set_pitch_scale(id, player.pitch);
            state.server->set_highshelf(id, player.highshelf, player.cutoff_hz);
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

// A clip a player's music started, as a kClipChanged world event.
void emit_clip_change(world::World& world, const PlayerState& player, ClipChange change) {
    constexpr double kMilliseconds = 1000.0;
    json::Value data;
    data["component"] = player.kind;
    data["clip"] = std::move(change.clip);
    if (!change.from.empty()) {
        data["from"] = std::move(change.from);
    }
    if (change.transition >= 0) {
        data["transition"] = change.transition;
    }
    if (change.filler) {
        data["filler"] = true;
    }
    if (change.automatic) {
        data["automatic"] = true;
    }
    if (const double waited = std::round(change.waited * kMilliseconds) / kMilliseconds;
        waited > 0.0) {
        data["waited"] = waited;
    }
    world.emit(std::string(kClipChanged), player.id, std::move(data));
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
    const world::Tree2D tree2d(world);
    const world::Tree3D tree3d(world);
    ++state.ticks;
    for (auto& [key, tracker] : state.trackers) {
        tracker.seen = false;
    }
    track<world::AudioStreamPlayer3D>(state, world, tree3d, kPlayer3D);
    track<world::AudioListener3D>(state, world, tree3d, "AudioListener3D");
    track<world::Camera3D>(state, world, tree3d, "Camera3D");
    std::erase_if(state.trackers, [](const auto& entry) { return !entry.second.seen; });
    Hearing hearing;
    hearing.tree2d = &tree2d;
    hearing.screen = ui::settings(world).viewport;
    hearing.listener2d = find_listener(world, tree2d, hearing.screen);
    hearing.tree3d = &tree3d;
    hearing.tick_seconds = tick_seconds;
    if (!world.ecs().view<world::AudioStreamPlayer3D>().empty()) {
        hearing.listener3d = find_listener_3d(state, world, tree3d, tick_seconds);
        hearing.areas3d = !world.ecs().view<world::Area3D>().empty();
    }
    reconcile<world::AudioStreamPlayer>(state, world, hearing, kPlayer, requests);
    reconcile<world::AudioStreamPlayer2D>(state, world, hearing, kPlayer2D, requests);
    reconcile<world::AudioStreamPlayer3D>(state, world, hearing, kPlayer3D, requests);
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
        } else if (player.kind == kPlayer2D) {
            follow_clip<world::AudioStreamPlayer2D>(state, world, player);
        } else {
            follow_clip<world::AudioStreamPlayer3D>(state, world, player);
        }
    }
    std::vector<ClipChange> changes;
    for (auto& [key, player] : state.players) {
        for (const Server::PlaybackId id : player.voices) {
            if (Playback* playback = state.server->playback(id); playback != nullptr) {
                playback->take_clip_changes(changes);
            }
        }
        for (ClipChange& change : changes) {
            player.clip = change.clip;
            emit_clip_change(world, player, std::move(change));
        }
        changes.clear();
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
            } else if (player.kind == kPlayer2D) {
                if (auto* component =
                        world.ecs().try_get<world::AudioStreamPlayer2D>(player.entity)) {
                    component->playing = false;
                }
            } else if (auto* component =
                           world.ecs().try_get<world::AudioStreamPlayer3D>(player.entity)) {
                component->playing = false;
            }
        }
        json::Value data;
        data["component"] = player.kind;
        world.emit("finished", player.id, std::move(data));
    }
    // Music that stopped, at its end or asked to, says so with no clip.
    for (auto& [key, player] : state.players) {
        if (player.voices.empty() && !player.clip.empty()) {
            ClipChange stopped;
            stopped.from = std::exchange(player.clip, {});
            emit_clip_change(world, player, std::move(stopped));
        }
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
    properties["panning_strength_3d"] =
        number_setting("How far AudioStreamPlayer3D sounds pan left and right, as Godot's "
                       "audio/general/3d_panning_strength.",
                       0, 4, Settings{}.panning_strength_3d);
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
    if (const json::Value* panning = settings.find("panning_strength_3d"); panning != nullptr) {
        out.panning_strength_3d = static_cast<float>(panning->as_double());
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
        [](std::string_view text, std::string_view path) { return decode_sound(text, path); },
        [](const Sound& sound) {
            return sizeof(Sound) + (sound.samples.capacity() * sizeof(float)) +
                   sound.data.capacity();
        });
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
        state->trackers.clear();
        state->played.clear();
        state->requests.clear();
        state->recordings.clear();
        state->input_ask.reset();
        state->input_stream.clear();
        state->input_playback.reset();
        state->input_loaded = {};
        state->apply_settings();
    });
    world.add_memory_source("audio",
                            [state](const world::World& /*world*/) { return state->memory(); });
    world.add_system(
        "audio", [state](world::World& w, double tick_seconds) { run(*state, w, tick_seconds); },
        {.doc = "Plays each AudioStreamPlayer, AudioStreamPlayer2D and AudioStreamPlayer3D and "
                "mixes the tick's sound through the bus layout.",
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

namespace {

bool has_player(const world::World& world, std::string_view kind, entt::entity entity) {
    const auto& ecs = world.ecs();
    if (kind == kPlayer) {
        return ecs.all_of<world::AudioStreamPlayer>(entity);
    }
    if (kind == kPlayer2D) {
        return ecs.all_of<world::AudioStreamPlayer2D>(entity);
    }
    return ecs.all_of<world::AudioStreamPlayer3D>(entity);
}

} // namespace

void play(world::World& world, entt::entity entity, double from_seconds) {
    State* state = state_of(world);
    if (state == nullptr) {
        return;
    }
    for (const std::string_view kind : {kPlayer, kPlayer2D, kPlayer3D}) {
        if (has_player(world, kind, entity)) {
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
    for (const std::string_view kind : {kPlayer, kPlayer2D, kPlayer3D}) {
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
    const auto* player_3d = world.ecs().try_get<world::AudioStreamPlayer3D>(entity);
    if (player == nullptr && player_2d == nullptr && player_3d == nullptr) {
        return make_error(ErrorCode::InvalidArgument,
                          "it has no AudioStreamPlayer, AudioStreamPlayer2D or "
                          "AudioStreamPlayer3D");
    }
    const auto own_stream = [&]() -> const std::string& {
        if (player != nullptr) {
            return player->stream;
        }
        return player_2d != nullptr ? player_2d->stream : player_3d->stream;
    };
    const std::string& own = own_stream();
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
    for (const std::string_view kind : {kPlayer, kPlayer2D, kPlayer3D}) {
        if (has_player(world, kind, entity)) {
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
    for (const std::string_view kind : {kPlayer, kPlayer2D, kPlayer3D}) {
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
    for (const std::string_view kind : {kPlayer, kPlayer2D, kPlayer3D}) {
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
        if (player.kind == kPlayer3D) {
            // Where its voices mix now: the first bus, then sends such as an area's reverb.
            info.volume = {};
            if (!player.mix.empty()) {
                info.bus = state->server->bus_index(player.mix.front().bus) >= 0
                               ? player.mix.front().bus
                               : "Master";
                info.volume = player.mix.front().volume;
                for (std::size_t i = 1; i < player.mix.size(); ++i) {
                    info.sends.push_back(player.mix[i]);
                }
            }
            info.pitch_scale = player.pitch;
            info.filter_db = player.highshelf > 0.0F ? linear_to_db(player.highshelf) : 0.0F;
        }
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
