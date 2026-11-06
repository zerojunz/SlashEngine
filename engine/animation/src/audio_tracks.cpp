// Audio tracks, as Godot's AnimationMixer plays them (_blend_init, the TYPE_AUDIO cases of
// _blend_process and _blend_apply, and _clear_audio_streams), through the sound engine/audio sets
// on the world (world::AudioTrackSound).

#include "slash/world/audio_tracks.hpp"

#include "internal.hpp"

#include "slash/core/math.hpp"
#include "slash/world/components.hpp"

#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace slash::animation::detail {
namespace {

// A stream a key started, as Godot's PlayingAudioStreamInfo.
struct PlayingStream {
    std::int64_t id = 0;
    double start = 0.0;             // the animation's time when it started
    double length = 0.0;            // how long it may play, or 0 for to its end
    std::optional<float> volume_db; // as last set
};

// One audio track of one animation, as Godot's PlayingAudioTrackInfo.
struct TrackInfo {
    double length = 0.0;
    double time = 0.0;
    float volume = 0.0F;
    bool loop = false;
    bool backward = false;
    bool use_blend = true;
    std::map<std::size_t, PlayingStream> streams; // by key
};

// What each player and tree's audio tracks play: by the entity that plays them, then by
// animation and track.
using Owner = std::pair<entt::entity, Mixer>;
using Owners = std::map<Owner, std::map<std::pair<std::string, std::size_t>, TrackInfo>>;

Owners& owners_of(world::World& world) {
    auto& ctx = world.ecs().ctx();
    if (auto* owners = ctx.find<Owners>()) {
        return *owners;
    }
    return ctx.emplace<Owners>();
}

float volume_db(float linear) {
    // Godot's linear_to_db: ln(linear) * 20 / ln(10).
    constexpr float kDbPerNeper = 8.6858896380650365530225783783321F;
    return linear > 0.0F ? slash::ln(linear) * kDbPerNeper
                         : -std::numeric_limits<float>::infinity();
}

} // namespace

void begin_audio(world::World& world, entt::entity owner, Mixer mixer) {
    Owners& owners = owners_of(world);
    if (const auto found = owners.find({owner, mixer}); found != owners.end()) {
        for (auto& [name, info] : found->second) {
            info.volume = 0.0F;
        }
    }
}

Result<void> audio_track_tick(world::World& world, const AudioTrackTick& tick) {
    const Track& track = *tick.track;
    TrackInfo& info =
        owners_of(world)[{tick.owner, tick.mixer}][{tick.animation_name, tick.track_index}];
    info.length = tick.animation->length;
    info.time = tick.time;
    info.volume += tick.blend;
    info.loop = tick.animation->loop != LoopMode::None;
    info.backward = tick.backward;
    info.use_blend = track.use_blend;
    if (tick.key == nullptr) {
        return {};
    }
    const world::AudioTrackSound* sound = world::audio_track_sound(world);
    if (sound == nullptr || tick.on == entt::null || !world.valid(tick.on)) {
        return {};
    }
    const auto index = static_cast<std::size_t>(std::distance(track.keys.data(), tick.key));
    if (tick.seeked) {
        // Seeking starts the key's stream again, from the middle.
        if (const auto found = info.streams.find(index); found != info.streams.end()) {
            sound->stop(world, found->second.id);
            info.streams.erase(found);
        }
    }
    if (!tick.key->value.is_string()) {
        return {};
    }
    double start = tick.key->start_offset;
    if (tick.seeked) {
        start += tick.time - tick.key->time;
    }
    auto played = sound->play(world, tick.on, tick.key->value.as_string(), start);
    if (!played) {
        return played.error();
    }
    PlayingStream playing;
    playing.id = played.value().id;
    playing.start = tick.time;
    const double length = played.value().length;
    if (length != 0.0 && is_greater_approx(tick.key->end_offset, 0.0)) {
        playing.length = length - start - tick.key->end_offset; // forces an end
    }
    info.streams[index] = playing;
    return {};
}

void end_audio(world::World& world, entt::entity owner, Mixer mixer) {
    Owners& owners = owners_of(world);
    const auto found = owners.find({owner, mixer});
    if (found == owners.end()) {
        return;
    }
    const world::AudioTrackSound* sound = world::audio_track_sound(world);
    for (auto it = found->second.begin(); it != found->second.end();) {
        TrackInfo& info = it->second;
        const float db = volume_db(info.use_blend ? info.volume : 1.0F);
        for (auto stream = info.streams.begin(); stream != info.streams.end();) {
            PlayingStream& playing = stream->second;
            bool stop = sound == nullptr || !sound->playing(world, playing.id);
            if (!info.loop) {
                // Playback went back before the key.
                if (!info.backward ? is_less_approx(info.time, playing.start)
                                   : is_greater_approx(info.time, playing.start)) {
                    stop = true;
                }
            }
            if (is_greater_approx(playing.length, 0.0)) {
                double played = 0.0;
                if (!info.backward) {
                    played = is_greater_approx(playing.start, info.time)
                                 ? (info.length - playing.start) + info.time
                                 : info.time - playing.start;
                } else {
                    played = is_less_approx(playing.start, info.time)
                                 ? (info.length - info.time) + playing.start
                                 : playing.start - info.time;
                }
                if (is_greater_approx(played, playing.length)) {
                    stop = true;
                }
            }
            if (stop) {
                if (sound != nullptr) {
                    sound->stop(world, playing.id);
                }
                stream = info.streams.erase(stream);
                continue;
            }
            if (playing.volume_db != db) {
                sound->set_volume(world, playing.id, db);
                playing.volume_db = db;
            }
            ++stream;
        }
        it = info.streams.empty() ? found->second.erase(it) : std::next(it);
    }
    if (found->second.empty()) {
        owners.erase(found);
    }
}

void stop_audio(world::World& world, entt::entity owner, Mixer mixer) {
    auto* owners = world.ecs().ctx().find<Owners>();
    if (owners == nullptr) {
        return;
    }
    const auto found = owners->find({owner, mixer});
    if (found == owners->end()) {
        return;
    }
    if (const world::AudioTrackSound* sound = world::audio_track_sound(world)) {
        for (const auto& [name, info] : found->second) {
            for (const auto& [key, playing] : info.streams) {
                sound->stop(world, playing.id);
            }
        }
    }
    owners->erase(found);
}

void sweep_audio(world::World& world) {
    auto* owners = world.ecs().ctx().find<Owners>();
    if (owners == nullptr) {
        return;
    }
    std::vector<Owner> gone;
    for (const auto& [owner, infos] : *owners) {
        const auto [entity, mixer] = owner;
        if (!world.valid(entity) ||
            (mixer == Mixer::Player ? !world.ecs().all_of<world::AnimationPlayer>(entity)
                                    : !world.ecs().all_of<world::AnimationTree>(entity))) {
            gone.push_back(owner);
        }
    }
    for (const auto& [entity, mixer] : gone) {
        stop_audio(world, entity, mixer);
    }
}

void reset_audio(world::World& world) {
    if (auto* owners = world.ecs().ctx().find<Owners>()) {
        owners->clear();
    }
}

} // namespace slash::animation::detail
