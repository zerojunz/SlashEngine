// Plays a video's sound with its picture. The clip is made here: four colors, a quarter of a
// second each, written by the engine's own AV1 encoder into a Matroska file with two audio
// tracks of a square wave that halves in loudness with each color: PCM, then the same at 48 kHz
// encoded as Opus. The VideoStreamPlayer "tv" plays it on the "Cutscenes" bus, and the output is
// what audio.state and audio.capture say as it plays, gets quieter, pauses, seeks, finishes and
// plays its Opus track.
#include "slash/command/builtin.hpp"
#include "slash/command/session.hpp"
#include "slash/platform/file.hpp"
#include "slash/video/video.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace slash;

namespace {

// The mix rate, so the square wave's corners stay square.
constexpr int kRate = 44100;
constexpr std::array<std::array<std::uint8_t, 3>, 4> kColors{
    {{200, 40, 40}, {40, 160, 60}, {40, 70, 200}, {230, 190, 40}}};

// The clip's frames: 64x32, each one color.
std::vector<render::Image> clip_frames() {
    std::vector<render::Image> frames;
    for (const auto& color : kColors) {
        render::Image image;
        image.width = 64;
        image.height = 32;
        image.pixels.resize(std::size_t{64} * 32 * 4);
        for (std::size_t i = 0; i < image.pixels.size(); i += 4) {
            image.pixels[i] = color[0];
            image.pixels[i + 1] = color[1];
            image.pixels[i + 2] = color[2];
            image.pixels[i + 3] = 255;
        }
        frames.push_back(std::move(image));
    }
    return frames;
}

// Its sound at a rate: a square wave in stereo, its half period 120 samples, at half of full
// scale for the first frame and half as loud again for each frame after.
video::PcmAudio clip_sound(int rate) {
    video::PcmAudio audio;
    audio.sample_rate = rate;
    audio.channels = 2;
    for (int i = 0; i < rate; ++i) {
        const int frame = i / (rate / 4);
        const auto level = static_cast<std::int16_t>(16384 >> frame);
        const auto sample = static_cast<std::int16_t>((i / 120) % 2 == 0 ? level : -level);
        audio.samples.push_back(sample);
        audio.samples.push_back(sample);
    }
    return audio;
}

struct Game {
    world::ComponentRegistry components;
    command::Registry commands;
    std::unique_ptr<command::Session> session;

    Game() {
        world::register_builtin_components(components);
        command::register_builtin_commands(commands);
        command::register_run_commands(commands);
        command::SessionOptions options;
        options.capture_logs = false;
        session = std::make_unique<command::Session>(components, commands, std::move(options));
    }

    [[nodiscard]] json::Value ok(std::string_view command, std::string_view params = "{}") const {
        command::Reply reply = session->call(command, json::parse(params).value());
        if (!reply.ok) {
            std::printf("  %.*s failed: %s\n", static_cast<int>(command.size()), command.data(),
                        reply.message.c_str());
            return {};
        }
        return std::move(reply.value);
    }

    // Steps ticks of 1/20 s.
    void step(int ticks) const {
        (void)ok("step", R"({"ticks": )" + std::to_string(ticks) + R"(, "tick_seconds": 0.05})");
    }

    void set(std::string_view fields) const {
        (void)ok("component.set", R"({"id": "tv", "components": {"VideoStreamPlayer": )" +
                                      std::string(fields) + "}}");
    }

    // The picture's position, the sound as audio.state lists it, and how loud the Cutscenes bus
    // was over the last 0.02 s, after any change of volume has ramped.
    void show(const char* what) const {
        const json::Value got = ok("entity.get", R"({"id": "tv"})");
        const json::Value* position =
            got.find("components")->find("VideoStreamPlayer")->find("stream_position");
        std::printf("%s\n  picture at %.2f s\n", what,
                    position != nullptr ? position->as_double() : 0.0);
        const json::Value state = ok("audio.state", R"({"ids": ["tv"]})");
        std::printf(
            "  audio.state: %s\n",
            json::write(state.find("players")->as_array().at(0), json::Style::Compact).c_str());
        const json::Value captured =
            ok("audio.capture", R"({"bus": "Cutscenes", "seconds": 0.02, "slices": 0})");
        const json::Value* peak = captured.find("peak_db");
        if (peak != nullptr && peak->is_number()) {
            std::printf("  Cutscenes over the last 0.02 s: peak %.1f dB\n", peak->as_double());
        } else {
            std::printf("  Cutscenes over the last 0.02 s: silent\n");
        }
    }
};

} // namespace

int main() {
    const video::PcmAudio sound = clip_sound(kRate);
    const auto opus = video::encode_opus(clip_sound(48000), 128000);
    if (!opus) {
        std::printf("couldn't encode the Opus track: %s\n", opus.error().message.c_str());
        return 1;
    }
    const auto clip = video::encode_matroska(clip_frames(), 4, 1, &sound, &opus.value());
    if (!clip ||
        !platform::write_file("clip.mkv", std::string(clip.value().begin(), clip.value().end()))) {
        std::printf("couldn't write clip.mkv\n");
        return 1;
    }
    const auto file = video::read_matroska(clip.value());
    if (!file) {
        std::printf("couldn't read clip.mkv: %s\n", file.error().message.c_str());
        return 1;
    }
    std::printf("clip.mkv: %zu frames of %ux%u, %.2f s, %zu bytes\n", file.value().frames.size(),
                file.value().width, file.value().height, file.value().length(),
                clip.value().size());
    for (const video::AudioTrack& track : file.value().audio) {
        std::printf("  audio %s, %d Hz, %d channels, %zu packets\n", track.codec_id.c_str(),
                    track.sample_rate, track.channels, track.packets.size());
    }

    const Game game;
    (void)game.ok("scene.open", R"({"path": "main.scene.json"})");
    game.step(1);
    game.show("tick 1: autoplay starts the picture and its sound together");
    game.step(5);
    game.show("tick 6: the second frame, its sound 6 dB quieter");

    game.set(R"({"volume_db": -10})");
    game.step(1);
    game.show("volume_db -10: 10 dB quieter again");

    game.set(R"({"paused": true})");
    game.step(2);
    game.show("paused: the sound fades out and holds where it got to");

    game.set(R"({"paused": false, "stream_position": 0.75, "volume_db": 0})");
    game.step(1);
    game.show("seeked to 0.75 s: the last frame's sound");

    game.step(6);
    game.show("past the end: both stop");

    game.set(R"({"audio_track": 1, "playing": true, "stream_position": 0})");
    game.step(1);
    game.show("audio_track 1: the same sound as Opus, decoded by libopus");

    game.set(R"({"audio_track": 2, "stream_position": 0})");
    game.step(1);
    game.show("audio_track 2, which the clip doesn't have");
    return 0;
}
