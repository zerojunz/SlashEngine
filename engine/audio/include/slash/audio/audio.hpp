#pragma once

#include "slash/asset/assets.hpp"
#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/result.hpp"
#include "slash/world/world.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Sound, as Godot's AudioServer and its players. Sound files load as assets: .wav files decode to
// samples when loaded, as Godot imports them, and .ogg and .mp3 files stay compressed and decode
// as they play. A .audio.json file gives a sound Godot's import settings (loops), or picks one of
// several sounds at random each time it plays, as Godot's AudioStreamRandomizer:
//
//   {"format": 1, "loop": true, "loop_offset": 4, "source": "music/theme.ogg"}
//   {"format": 1, "random_pitch": 1.1, "streams": [{"stream": "sfx/step1.wav"},
//                                                  {"stream": "sfx/step2.wav"}]}
//
// AudioStreamPlayer and AudioStreamPlayer2D components play them through the buses a
// .bus_layout.json file sets up, each bus with Godot's volume, mute, solo, effects and send.
// Mixing follows the fixed ticks rather than the sound card, so the same ticks always make the
// same sound: each tick, the "audio" system mixes that tick's share of samples and hands them to
// whatever plays them (an SdlOutput in a window), and agents can read what was mixed.
namespace slash::audio {

inline constexpr std::string_view kStreamSuffix = ".audio.json";
inline constexpr std::string_view kBusLayoutSuffix = ".bus_layout.json";

// One stereo sample, as Godot's AudioFrame.
struct Frame {
    float left = 0.0F;
    float right = 0.0F;
};

// Decibels to a linear gain and back, as Godot's Math::db_to_linear and linear_to_db.
[[nodiscard]] float db_to_linear(float db);
[[nodiscard]] float linear_to_db(float linear);

// ---------------------------------------------------------------------------------------------
// Sounds and streams

// A sound file's audio. More than two channels are mixed down to two.
struct Sound {
    enum class Format : std::uint8_t { Wav, Vorbis, Mp3 };
    Format format = Format::Wav;
    int mix_rate = 0;           // frames per second
    int channels = 0;           // 1 or 2
    std::int64_t frames = 0;    // length in frames
    std::vector<float> samples; // .wav: every frame, channels interleaved
    std::string data;           // .ogg and .mp3: the file, decoded as it plays

    [[nodiscard]] double length() const {
        return mix_rate > 0 ? static_cast<double>(frames) / mix_rate : 0.0;
    }
};

// Whether a path names a sound file: .wav, .ogg or .mp3.
[[nodiscard]] bool is_sound_path(std::string_view path);

// Decodes a sound file's header and, for .wav, its samples; the path's extension says which
// format it is.
[[nodiscard]] Result<Sound> decode_sound(std::string_view data, std::string_view path);

// Godot's AudioStreamWAV loop modes.
enum class LoopMode : std::uint8_t { Disabled, Forward, PingPong, Backward };

// Godot's AudioStreamRandomizer playback modes.
enum class PlaybackMode : std::uint8_t { RandomNoRepeats, Random, Sequential };

// A .audio.json file: a sound with settings, or a randomizer of streams.
struct StreamFile {
    std::string source; // a sound file, or "" for a randomizer

    // .wav sources, as Godot's import settings: loop_end -1 is the sound's end.
    LoopMode loop_mode = LoopMode::Disabled;
    std::int64_t loop_begin = 0; // frames
    std::int64_t loop_end = -1;
    // .ogg and .mp3 sources: after the end (or beat_count beats at bpm), go to loop_offset.
    bool loop = false;
    double loop_offset = 0.0; // seconds
    double bpm = 0.0;
    int beat_count = 0;

    // Randomizers.
    struct Entry {
        std::string stream; // a sound file or another .audio.json file
        float weight = 1.0F;
    };
    std::vector<Entry> streams;
    PlaybackMode playback_mode = PlaybackMode::RandomNoRepeats;
    float random_pitch = 1.0F;            // pitch scale up to this far up or down, at least 1
    float random_volume_offset_db = 0.0F; // volume up to this far up or down
};

[[nodiscard]] std::string_view loop_mode_name(LoopMode mode);
[[nodiscard]] std::string_view playback_mode_name(PlaybackMode mode);

// JSON Schema for .audio.json files.
[[nodiscard]] json::Value stream_file_schema();
// Every error in a .audio.json document, each with the JSON Pointer of the bad value.
[[nodiscard]] std::vector<json::ValidationError> validate_stream_file(const json::Value& doc);
// The settings in a .audio.json document, or its first error.
[[nodiscard]] Result<StreamFile> parse_stream_file(const json::Value& doc);

// ---------------------------------------------------------------------------------------------
// Buses

// An effect on a bus: its type ("reverb", "low_pass_filter", ...) and its settings, which
// effects.hpp lists.
struct EffectSlot {
    std::string type;
    json::Value params; // the slot's other members, as written
    bool enabled = true;
};

// A bus, as Godot's AudioBusLayout keeps one.
struct BusSettings {
    std::string name;
    float volume_db = 0.0F;
    bool solo = false;
    bool mute = false;
    bool bypass_effects = false;
    std::string send = "Master"; // the bus this one mixes into; ignored for Master
    std::vector<EffectSlot> effects;
};

// A .bus_layout.json file: the buses, Master first.
struct BusLayout {
    std::vector<BusSettings> buses;
};

// One Master bus, Godot's layout when a project names none.
[[nodiscard]] BusLayout default_bus_layout();
// JSON Schema for .bus_layout.json files.
[[nodiscard]] json::Value bus_layout_schema();
// Every error in a .bus_layout.json document, each with the JSON Pointer of the bad value.
[[nodiscard]] std::vector<json::ValidationError> validate_bus_layout(const json::Value& doc);
// The buses in a .bus_layout.json document, or its first error.
[[nodiscard]] Result<BusLayout> parse_bus_layout(const json::Value& doc);

// ---------------------------------------------------------------------------------------------
// Playbacks and the server

// A stream playing, as Godot's AudioStreamPlayback.
class Playback {
public:
    Playback() = default;
    virtual ~Playback() = default;
    Playback(const Playback&) = delete;
    Playback& operator=(const Playback&) = delete;
    Playback(Playback&&) = delete;
    Playback& operator=(Playback&&) = delete;

    virtual void start(double from_seconds) = 0;
    virtual void stop() = 0;
    [[nodiscard]] virtual bool is_playing() const = 0;
    [[nodiscard]] virtual double position() const = 0; // seconds into the stream
    [[nodiscard]] virtual int loop_count() const { return 0; }
    // Writes frames at the server's mix rate, played rate_scale times as fast; returns how many
    // came from the stream (fewer once it ends).
    virtual int mix(Frame* buffer, float rate_scale, int frames) = 0;
};

// A stream ready to play: a sound with its settings, or a randomizer of streams.
class Stream {
public:
    Stream() = default;
    virtual ~Stream() = default;
    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;
    Stream(Stream&&) = delete;
    Stream& operator=(Stream&&) = delete;

    // A new playback at mix_rate. random is a value from 0 to 2^32 - 1 the randomizer picks with.
    [[nodiscard]] virtual std::unique_ptr<Playback> instantiate(int mix_rate,
                                                                std::uint32_t random) const = 0;
    [[nodiscard]] virtual double length() const = 0; // seconds, 0 if unknown
};

// A stream for a sound with a .audio.json file's settings (or none).
[[nodiscard]] std::shared_ptr<const Stream> sound_stream(std::shared_ptr<const Sound> sound,
                                                         const StreamFile& settings = {});
// A randomizer over streams, with their weights.
[[nodiscard]] std::shared_ptr<const Stream>
randomizer_stream(std::vector<std::pair<std::shared_ptr<const Stream>, float>> streams,
                  const StreamFile& settings);

// A stream loaded through assets, with handles to the files it was made from, which keep them
// loaded and tell when one was reloaded.
struct LoadedStream {
    std::shared_ptr<const Stream> stream;
    std::vector<asset::Handle<Sound>> sounds;
    std::vector<asset::Handle<StreamFile>> files;
    std::vector<std::uint64_t> versions; // each handle's version, sounds first

    // Whether a file it was made from changed since, so it should be loaded again.
    [[nodiscard]] bool stale() const;
};

// Loads the stream a path names (a sound file or a .audio.json file) through assets.
[[nodiscard]] Result<LoadedStream> load_stream(asset::Assets& assets, std::string_view path);

// The mixer, as Godot's AudioServer: playbacks mix into buses, buses run their effects and mix
// into the bus they send to, and the Master bus is what plays. Stereo only.
class Server {
public:
    using PlaybackId = std::uint32_t;
    // Frames in each mix step, as Godot's buffer_size.
    static constexpr int kMixStep = 512;

    explicit Server(int mix_rate);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    Server(Server&&) = delete;
    Server& operator=(Server&&) = delete;

    [[nodiscard]] int mix_rate() const noexcept { return mix_rate_; }

    // Replaces the buses, as Godot's set_bus_layout. The first bus is always named "Master".
    // Effects the server can't make are left out and logged.
    void set_layout(const BusLayout& layout);
    [[nodiscard]] const BusLayout& layout() const noexcept { return layout_; }
    // The index of the bus with this name, or -1.
    [[nodiscard]] int bus_index(std::string_view name) const;
    void set_bus_volume_db(int bus, float volume_db);
    void set_bus_mute(int bus, bool mute);
    void set_bus_solo(int bus, bool solo);
    void set_bus_bypass_effects(int bus, bool bypass);
    void set_bus_effect_enabled(int bus, int effect, bool enabled);
    // The bus's loudest sample over the last 512-frame mix step, in dB for left and right; -200
    // when it is silent.
    [[nodiscard]] Frame bus_peak_db(int bus) const;

    // Starts a playback from a position in seconds, mixing into a bus (Master if it has no
    // such bus) at volume, as Godot's start_playback_stream; paused, it waits for
    // set_paused(id, false).
    PlaybackId start(std::unique_ptr<Playback> playback, std::string bus, Frame volume,
                     double from_seconds, float pitch_scale, bool paused = false);
    // Fades a playback out over the next mix step and removes it.
    void stop(PlaybackId id);
    void set_paused(PlaybackId id, bool paused);
    void set_bus_exclusive(PlaybackId id, std::string bus, Frame volume);
    void set_pitch_scale(PlaybackId id, float pitch_scale);
    // Playing, and not paused or fading out.
    [[nodiscard]] bool is_active(PlaybackId id) const;
    [[nodiscard]] bool is_paused(PlaybackId id) const;
    [[nodiscard]] double position(PlaybackId id) const;
    [[nodiscard]] std::size_t playback_count() const;

    // The next frames of the Master bus, from -1 to 1, as Godot's driver asks for them. Each 512
    // frames run one mix step. Each bus's output also goes to the bus listener, if any.
    void mix(std::span<Frame> out);
    // Frames mixed so far.
    [[nodiscard]] std::uint64_t mixed_frames() const noexcept { return mixed_frames_; }

    // Called with each bus's output of a mix step (kMixStep frames), after its effects and
    // volume; no frames when the bus is silent and inactive.
    using BusListener = std::function<void(int bus, std::span<const Frame> frames)>;
    void set_bus_listener(BusListener listener) { bus_listener_ = std::move(listener); }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    int mix_rate_;
    BusLayout layout_;
    std::uint64_t mixed_frames_ = 0;
    BusListener bus_listener_;
};

// ---------------------------------------------------------------------------------------------
// Worlds

// The project's audio settings: project.json's "audio".
struct Settings {
    // A .bus_layout.json file, as Godot's audio/buses/default_bus_layout; "" for one Master bus.
    std::string bus_layout;
    // Frames per second mixed, as Godot's audio/driver/mix_rate.
    int mix_rate = 44100;
    // Milliseconds of sound queued ahead of the speakers, as Godot's audio/driver/output_latency.
    float output_latency = 15.0F;
    // How far 2D sounds pan left and right, as Godot's audio/general/2d_panning_strength.
    float panning_strength_2d = 0.5F;
    // Seconds of each bus's output kept for capture(); sessions keep some, games none.
    double history_seconds = 0.0;

    friend bool operator==(const Settings&, const Settings&) = default;
};

// JSON Schema for project.json's "audio".
[[nodiscard]] json::Value settings_schema();
// Every error in project.json's "audio", each with its JSON Pointer under at, such as "/audio".
[[nodiscard]] std::vector<json::ValidationError> validate_settings(const json::Value& settings,
                                                                   std::string_view at);
// The settings in a valid "audio" object.
[[nodiscard]] Settings parse_settings(const json::Value& settings);

// Asset types "sound" (.wav, .ogg and .mp3 files as Sound), "audio_stream" (.audio.json files as
// StreamFile) and "bus_layout" (.bus_layout.json files as BusLayout).
void add_asset_types(asset::Assets& assets);

// Gives the world a Server, loads the bus layout through assets (which must outlive the world)
// and adds the "audio" system (priority 1200, after everything else), which plays each
// AudioStreamPlayer and AudioStreamPlayer2D and mixes each tick's sound.
void install(world::World& world, asset::Assets& assets, const Settings& settings = {});
// Applies new settings; a change starts the buses over from the layout, and a new mix rate
// stops every sound.
void set_settings(world::World& world, const Settings& settings);
// The settings of a world audio was installed in, or the defaults.
[[nodiscard]] Settings settings(const world::World& world);
// The world's server, or nullptr if audio was not installed.
[[nodiscard]] Server* server(world::World& world);
[[nodiscard]] const Server* server(const world::World& world);

// Where each tick's mixed sound goes, such as an SdlOutput; nullptr for nowhere.
using Sink = std::function<void(std::span<const Frame> frames)>;
void set_sink(world::World& world, Sink sink);

// Plays an AudioStreamPlayer or AudioStreamPlayer2D from a position in seconds at the next
// tick, as Godot's play(): with max_polyphony 1 it restarts, with more another voice joins.
void play(world::World& world, entt::entity entity, double from_seconds = 0.0);
// Stops it at the next tick, as Godot's stop().
void stop(world::World& world, entt::entity entity);
// Where its newest voice is, in seconds, or 0.
[[nodiscard]] double playback_position(const world::World& world, entt::entity entity);

// The plays and stops asked for the next tick and the buses' volumes and switches, which
// sessions put back with the world when a command fails or is a dry run.
struct Snapshot {
    struct Request {
        std::string player; // "<kind>/<entity ID>"
        bool play = true;
        double from_seconds = 0.0;
    };
    std::vector<Request> requests;
    std::vector<BusSettings> buses;
};
[[nodiscard]] Snapshot save(const world::World& world);
void restore(world::World& world, const Snapshot& snapshot);

// A player as the audio system sees it.
struct PlayerInfo {
    std::string id;
    std::string kind; // "AudioStreamPlayer" or "AudioStreamPlayer2D"
    std::string stream;
    std::string bus;
    int voices = 0;      // playbacks mixing
    bool paused = false; // stream_paused holds them
    double position = 0.0;
    double length = 0.0;
    Frame volume{};    // the gain mixed in, after distance and panning for 2D players
    std::string error; // why its stream did not load
};
// Every player with an ID, by ID.
[[nodiscard]] std::vector<PlayerInfo> players(const world::World& world);

// Plays each tick's sound on the default output device through SDL, as Godot's audio driver,
// keeping about output_latency milliseconds queued.
class SdlOutput {
public:
    // Fails where there is no output device, as on a machine with no sound card.
    [[nodiscard]] static Result<std::unique_ptr<SdlOutput>> open(int mix_rate,
                                                                 float output_latency_ms);
    ~SdlOutput();
    SdlOutput(const SdlOutput&) = delete;
    SdlOutput& operator=(const SdlOutput&) = delete;
    SdlOutput(SdlOutput&&) = delete;
    SdlOutput& operator=(SdlOutput&&) = delete;

    void push(std::span<const Frame> frames);
    // A sink for set_sink that pushes here; this must outlive the world using it.
    [[nodiscard]] Sink sink();

private:
    SdlOutput() = default;

    void* stream_ = nullptr; // SDL_AudioStream
    int latency_frames_ = 0;
};

// What a bus played over the last seconds.
struct Capture {
    double seconds = 0.0; // how much was kept, at most what was asked
    float peak_db = -200.0F;
    float rms_db = -200.0F;
    // The loudest sample in each equal slice of that time, oldest first.
    std::vector<float> slice_peaks_db;
    std::vector<Frame> frames; // the samples, when asked for
};
// What a bus played over the last seconds, kept for Settings::history_seconds.
[[nodiscard]] Result<Capture> capture(const world::World& world, std::string_view bus,
                                      double seconds, int slices, bool frames = false);

// A 16-bit PCM .wav file of frames.
[[nodiscard]] std::string encode_wav(std::span<const Frame> frames, int mix_rate);

} // namespace slash::audio
