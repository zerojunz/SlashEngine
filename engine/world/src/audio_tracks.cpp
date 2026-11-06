#include "slash/world/audio_tracks.hpp"

#include <utility>

namespace slash::world {

void set_audio_track_sound(World& world, AudioTrackSound sound) {
    world.ecs().ctx().insert_or_assign<AudioTrackSound>(std::move(sound));
}

const AudioTrackSound* audio_track_sound(const World& world) {
    const auto* sound = world.ecs().ctx().find<AudioTrackSound>();
    return sound != nullptr && sound->play ? sound : nullptr;
}

} // namespace slash::world
