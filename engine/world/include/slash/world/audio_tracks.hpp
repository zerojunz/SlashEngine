#pragma once

#include "slash/core/result.hpp"
#include "slash/world/world.hpp"

#include <cstdint>
#include <entt/entity/entity.hpp>
#include <functional>
#include <string_view>

// Sound for animations' audio tracks, as Godot's AnimationMixer plays them through an
// AudioStreamPlayer's AudioStreamPlaybackPolyphonic: engine/audio, which plays sound, sets it on
// the worlds it installs into, and engine/animation, which doesn't depend on audio, plays tracks
// through it.
namespace slash::world {

// A stream an audio track started: its ID, and the stream's length in seconds (0 if unknown).
struct AudioTrackPlay {
    std::int64_t id = 0;
    double length = 0.0;
};

struct AudioTrackSound {
    // Starts a stream (a sound or a .audio.json file) on the entity's AudioStreamPlayer,
    // AudioStreamPlayer2D or AudioStreamPlayer3D, from from_seconds into it, when audio next
    // mixes. Fails if the entity has no player or the stream doesn't load.
    std::function<Result<AudioTrackPlay>(World& world, entt::entity player, std::string_view stream,
                                         double from_seconds)>
        play;
    // Stops one, fading it out over the next mix.
    std::function<void(World& world, std::int64_t id)> stop;
    // Whether one plays still: started or about to, and not ended or stopped.
    std::function<bool(const World& world, std::int64_t id)> playing;
    // Sets one's volume, in dB.
    std::function<void(World& world, std::int64_t id, float volume_db)> set_volume;
};

// Sets how a world's audio tracks sound, kept in its context.
void set_audio_track_sound(World& world, AudioTrackSound sound);
// How they sound; null when nothing set it, so audio tracks play nothing.
[[nodiscard]] const AudioTrackSound* audio_track_sound(const World& world);

} // namespace slash::world
