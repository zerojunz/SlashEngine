#pragma once

#include "slash/core/result.hpp"
#include "slash/world/audio_tracks.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slash::test {

// Sound for audio tracks that only notes what they ask: each stream started, by its ID, and the
// calls made, as "play <entity> <stream> <from>", "stop <id>" and "volume <id> <dB>".
struct FakeSound {
    struct Stream {
        std::string stream;
        bool playing = true;
    };
    std::map<std::int64_t, Stream> streams;
    std::vector<std::string> calls;
    std::map<std::string, double, std::less<>> lengths; // by stream; 0 if not here

    void install(world::World& world) {
        world::AudioTrackSound sound;
        sound.play = [this](world::World& w, entt::entity entity, std::string_view stream,
                            double from) -> Result<world::AudioTrackPlay> {
            if (!w.ecs().all_of<world::AudioStreamPlayer>(entity)) {
                return make_error(ErrorCode::InvalidArgument, "no player");
            }
            const auto id = static_cast<std::int64_t>(streams.size()) + 1;
            streams[id] = {std::string(stream)};
            char from_text[32];
            std::snprintf(from_text, sizeof(from_text), "%g", from);
            calls.push_back("play " + std::string(w.id_of(entity)) + " " + std::string(stream) +
                            " " + from_text);
            const auto length = lengths.find(stream);
            return world::AudioTrackPlay{id, length != lengths.end() ? length->second : 0.0};
        };
        sound.stop = [this](world::World& /*w*/, std::int64_t id) {
            streams[id].playing = false;
            calls.push_back("stop " + std::to_string(id));
        };
        sound.playing = [this](const world::World& /*w*/, std::int64_t id) {
            const auto found = streams.find(id);
            return found != streams.end() && found->second.playing;
        };
        sound.set_volume = [this](world::World& /*w*/, std::int64_t id, float db) {
            char text[32];
            std::snprintf(text, sizeof(text), "%g", static_cast<double>(db));
            calls.push_back("volume " + std::to_string(id) + " " + text);
        };
        world::set_audio_track_sound(world, std::move(sound));
    }

    std::vector<std::string> take() { return std::exchange(calls, {}); }
};

} // namespace slash::test
