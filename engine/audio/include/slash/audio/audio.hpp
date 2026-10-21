#pragma once

#include "slash/asset/assets.hpp"
#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/result.hpp"
#include "slash/world/world.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Sound, as Godot's AudioServer and its players. Sound files load as assets: .wav files decode to
// samples when loaded, as Godot imports them, and .ogg and .mp3 files stay compressed and decode
// as they play. A .audio.json file gives a sound Godot's import settings (loops and beats), picks
// one of several sounds at random each time it plays, as Godot's AudioStreamRandomizer, or plays
// music as Godot's interactive_music module: streams in turn (AudioStreamPlaylist), together
// (AudioStreamSynchronized), or as clips a game switches between (AudioStreamInteractive). It can
// also play frames a game makes as it goes (AudioStreamGenerator), sounds a game starts on one
// player (AudioStreamPolyphonic), or what the microphone records (AudioStreamMicrophone):
//
//   {"format": 1, "loop": true, "loop_offset": 4, "source": "music/theme.ogg"}
//   {"format": 1, "random_pitch": 1.1, "streams": [{"stream": "sfx/step1.wav"},
//                                                  {"stream": "sfx/step2.wav"}]}
//   {"format": 1, "playlist": [{"stream": "music/a.ogg"}, {"stream": "music/b.ogg"}]}
//   {"format": 1, "synchronized": [{"stream": "music/drums.ogg"},
//                                  {"stream": "music/strings.ogg", "volume_db": -80}]}
//   {"format": 1, "clips": [{"name": "explore", "stream": "music/explore.ogg"},
//                           {"name": "battle", "stream": "music/battle.ogg"}],
//    "transitions": [{"to": "battle", "from_time": "next_bar", "fade_mode": "cross"}]}
//   {"format": 1, "generator": {"mix_rate": 22050, "buffer_length": 0.25}}
//   {"format": 1, "polyphonic": {"polyphony": 16}}
//   {"format": 1, "microphone": {}}
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

    friend bool operator==(const Frame&, const Frame&) = default;
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

// Godot's AudioStreamInteractive settings: what a clip does when it ends, and how a transition
// times its change and fades.
enum class AutoAdvance : std::uint8_t { Disabled, Enabled, ReturnToHold };
enum class TransitionFrom : std::uint8_t { Immediate, NextBeat, NextBar, End };
enum class TransitionTo : std::uint8_t { SamePosition, Start, PreviousPosition };
enum class FadeMode : std::uint8_t { Disabled, In, Out, Cross, Automatic };

// Godot's AudioStreamGenerator mix rates: the server's, the rate microphone input is recorded at
// (the server's), or the generator's own.
enum class MixRateMode : std::uint8_t { Output, Input, Custom };

// A .audio.json file: a sound with settings, a randomizer of streams, music, a generator, a
// polyphonic stream or the microphone. Members are ordered by size, to pack.
struct StreamFile {
    enum class Kind : std::uint8_t {
        Sound,
        Randomizer,
        Playlist,
        Synchronized,
        Interactive,
        Generator,
        Polyphonic,
        Microphone
    };

    std::string source; // a sound's file

    // A randomizer's, playlist's or synchronized stream's streams.
    struct Entry {
        std::string stream;     // a sound file or another .audio.json file
        float weight = 1.0F;    // randomizers
        float volume_db = 0.0F; // synchronized streams
    };
    std::vector<Entry> streams;

    // Interactive music. Clips and transitions name clips by index; -1 is any clip.
    struct Clip {
        std::string name;
        std::string stream;
        AutoAdvance auto_advance = AutoAdvance::Disabled;
        int next_clip = -1;
    };
    struct Transition {
        int from_clip = -1;
        int to_clip = -1;
        TransitionFrom from_time = TransitionFrom::NextBeat;
        TransitionTo to_time = TransitionTo::Start;
        FadeMode fade_mode = FadeMode::Automatic;
        float fade_beats = 1.0F;
        int filler_clip = -1; // a clip played between the two, or -1
        bool hold_previous = false;
    };
    std::vector<Clip> clips;
    std::vector<Transition> transitions;

    // .wav sources, as Godot's import settings (with loop_mode): loop_end -1 is the sound's end.
    std::int64_t loop_begin = 0; // frames
    std::int64_t loop_end = -1;
    // .ogg and .mp3 sources: after the end (or beat_count beats at bpm), go to loop_offset (with
    // loop).
    double loop_offset = 0.0; // seconds
    // A sound's beats, as Godot's bpm, beat_count and bar_beats, which interactive music and
    // playlists time their changes by.
    double bpm = 0.0;
    int beat_count = 0;
    int bar_beats = 4;

    float random_pitch = 1.0F;            // pitch scale up to this far up or down, at least 1
    float random_volume_offset_db = 0.0F; // volume up to this far up or down
    float fade_time = 0.3F;               // playlists: seconds the stream before fades over
    int initial_clip = 0;
    float mix_rate = 44100.0F;  // generators: frames per second pushed, with MixRateMode::Custom
    float buffer_length = 0.5F; // generators: seconds of frames it holds
    int polyphony = 32;         // polyphonic streams: streams that can play at once

    Kind kind = Kind::Sound;
    LoopMode loop_mode = LoopMode::Disabled;
    // .ogg and .mp3 sources: play again from loop_offset. Playlists: start over after the last
    // stream.
    bool loop = false;
    PlaybackMode playback_mode = PlaybackMode::RandomNoRepeats;
    bool shuffle = false; // playlists: a new order each time round
    MixRateMode mix_rate_mode = MixRateMode::Custom;
};

[[nodiscard]] std::string_view loop_mode_name(LoopMode mode);
[[nodiscard]] std::string_view playback_mode_name(PlaybackMode mode);

// The index of the transition interactive music takes from clip from to clip to, as Godot's
// transition_map lookup: from and to named, from named, to named, then neither; -1 for none,
// when it takes a transition with Godot's defaults.
[[nodiscard]] int transition_index(std::span<const StreamFile::Transition> transitions, int from,
                                   int to);

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

class EffectInstance; // effects.hpp
class GeneratorPlayback;
class PolyphonicPlayback;
class Server;

// Music starting a clip: one of an interactive stream's clips, or the next of a playlist's
// streams. The audio system emits each as a kClipChanged world event from the player.
struct ClipChange {
    std::string clip;       // the clip's name, or the playlist stream's path; "" once it stops
    std::string from;       // what played before it, or "" as the music starts
    int transition = -1;    // the index of the transitions entry it followed, or -1 for none
    bool filler = false;    // a transition's filler clip, between two others
    bool automatic = false; // auto advance or a playlist moving on, not a switch asked for
    double waited = 0.0;    // seconds from the switch to the clip starting: its beat or bar
};
// The world event a player emits as its music starts a clip, with the ClipChange's fields.
inline constexpr std::string_view kClipChanged = "clip_changed";
class Stream;

// A stream playing, as Godot's AudioStreamPlayback.
class Playback {
public:
    Playback() = default;
    virtual ~Playback() = default;
    Playback(const Playback&) = delete;
    Playback& operator=(const Playback&) = delete;
    Playback(Playback&&) = delete;
    Playback& operator=(Playback&&) = delete;

    // Called by Server::start before start(), for playbacks that read the server, as a
    // microphone reads its input.
    virtual void attach(Server& /*server*/) {}
    virtual void start(double from_seconds) = 0;
    virtual void stop() = 0;
    [[nodiscard]] virtual bool is_playing() const = 0;
    [[nodiscard]] virtual double position() const = 0; // seconds into the stream
    [[nodiscard]] virtual int loop_count() const { return 0; }
    // Writes frames at the server's mix rate, played rate_scale times as fast; returns how many
    // came from the stream (fewer once it ends).
    virtual int mix(Frame* buffer, float rate_scale, int frames) = 0;
    // A player's parameter, as Godot's set_parameter: an interactive stream's "switch_to_clip"
    // and a synchronized stream's "stream_<index>_volume_db". Others are ignored.
    virtual void set_parameter(std::string_view /*name*/, const json::Value& /*value*/) {}
    // A parameter as the playback has it now, as Godot's get_parameter, or null.
    [[nodiscard]] virtual json::Value parameter(std::string_view /*name*/) const { return {}; }
    // The interactive music clip playing, or "".
    [[nodiscard]] virtual std::string_view clip() const { return {}; }
    // Moves the clips music started since the last call to the end of changes, oldest first.
    virtual void take_clip_changes(std::vector<ClipChange>& /*changes*/) {}
    // This playback as Godot's AudioStreamGeneratorPlayback or AudioStreamPlaybackPolyphonic, or
    // nullptr if it plays another kind of stream.
    [[nodiscard]] virtual GeneratorPlayback* generator() { return nullptr; }
    [[nodiscard]] virtual const GeneratorPlayback* generator() const { return nullptr; }
    [[nodiscard]] virtual PolyphonicPlayback* polyphonic() { return nullptr; }
    [[nodiscard]] virtual const PolyphonicPlayback* polyphonic() const { return nullptr; }
};

// A generator playing, as Godot's AudioStreamGeneratorPlayback: frames a game pushes, at the
// generator's mix rate, played as they come. Push at least a tick's worth of frames each tick,
// before the audio system mixes them; a mix that runs out plays silence and counts a skip.
class GeneratorPlayback {
public:
    // Adds a frame, or fails when the buffer is full.
    virtual bool push_frame(Frame frame) = 0;
    [[nodiscard]] virtual bool can_push_buffer(int frames) const = 0;
    // Adds every frame, or fails and adds none when they don't fit.
    virtual bool push_buffer(std::span<const Frame> frames) = 0;
    // Frames that can be pushed before the buffer is full.
    [[nodiscard]] virtual int frames_available() const = 0;
    // Mixes that ran out of frames since it started.
    [[nodiscard]] virtual int skips() const = 0;
    // Drops the frames pushed; as Godot's, only once it stopped.
    virtual void clear_buffer() = 0;

protected:
    ~GeneratorPlayback() = default;
};

// A polyphonic stream playing, as Godot's AudioStreamPlaybackPolyphonic: streams a game starts on
// one player, each with its own volume and pitch, up to the stream's polyphony at once.
class PolyphonicPlayback {
public:
    using StreamId = std::int64_t;
    static constexpr StreamId kInvalidId = -1;

    // Starts a stream at the next mix, from a position in seconds; its ID, or kInvalidId when
    // polyphony streams are playing already.
    virtual StreamId play_stream(std::shared_ptr<const Stream> stream, double from_seconds,
                                 float volume_db, float pitch_scale) = 0;
    virtual void set_stream_volume(StreamId id, float volume_db) = 0;
    virtual void set_stream_pitch_scale(StreamId id, float pitch_scale) = 0;
    // Whether it plays still: false once it ended or was stopped.
    [[nodiscard]] virtual bool is_stream_playing(StreamId id) const = 0;
    // Fades it out over the next mix.
    virtual void stop_stream(StreamId id) = 0;
    // The streams playing, and how many can.
    [[nodiscard]] virtual int streams_playing() const = 0;
    [[nodiscard]] virtual int polyphony() const = 0;

protected:
    ~PolyphonicPlayback() = default;
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
    // Its beats and whether it loops, as Godot's get_bpm, get_beat_count, get_bar_beats and
    // has_loop; 0 where it has none.
    [[nodiscard]] virtual double bpm() const { return 0.0; }
    [[nodiscard]] virtual int beat_count() const { return 0; }
    [[nodiscard]] virtual int bar_beats() const { return 0; }
    [[nodiscard]] virtual bool has_loop() const { return false; }
    // Whether a player's play() starts it over rather than adding a voice, as Godot's
    // is_monophonic: music, generators, polyphonic streams and randomizers of any of them.
    [[nodiscard]] virtual bool is_monophonic() const { return false; }
};

// A stream for a sound with a .audio.json file's settings (or none).
[[nodiscard]] std::shared_ptr<const Stream> sound_stream(std::shared_ptr<const Sound> sound,
                                                         const StreamFile& settings = {});
// A randomizer over streams, with their weights.
[[nodiscard]] std::shared_ptr<const Stream>
randomizer_stream(std::vector<std::pair<std::shared_ptr<const Stream>, float>> streams,
                  const StreamFile& settings);
// Music, as Godot's AudioStreamPlaylist, AudioStreamSynchronized and AudioStreamInteractive:
// streams holds a stream for each of settings.streams, or of settings.clips.
[[nodiscard]] std::shared_ptr<const Stream>
playlist_stream(std::vector<std::shared_ptr<const Stream>> streams, const StreamFile& settings);
[[nodiscard]] std::shared_ptr<const Stream>
synchronized_stream(std::vector<std::shared_ptr<const Stream>> streams, const StreamFile& settings);
[[nodiscard]] std::shared_ptr<const Stream>
interactive_stream(std::vector<std::shared_ptr<const Stream>> clips, const StreamFile& settings);
// Godot's AudioStreamGenerator, AudioStreamPolyphonic and AudioStreamMicrophone.
[[nodiscard]] std::shared_ptr<const Stream> generator_stream(const StreamFile& settings);
[[nodiscard]] std::shared_ptr<const Stream> polyphonic_stream(const StreamFile& settings);
[[nodiscard]] std::shared_ptr<const Stream> microphone_stream();

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
    // The running effect in a bus's slot, as Godot's get_bus_effect_instance, if it is of this
    // type ("spectrum_analyzer", "capture", ...); otherwise nullptr. effects.hpp has typed
    // versions.
    [[nodiscard]] EffectInstance* effect_instance(int bus, int effect, std::string_view type);
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
    // The playback itself, to set parameters on and read, or nullptr once it is gone.
    [[nodiscard]] Playback* playback(PlaybackId id);
    [[nodiscard]] const Playback* playback(PlaybackId id) const;
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

    // Microphone input, as Godot's AudioServer input calls. Input records only while it is on,
    // and turns on only once set_input_enabled allows it, as Godot's audio/driver/enable_input.
    // Each tick the audio system pushes that tick's frames from the input device.
    void set_input_enabled(bool enabled);
    [[nodiscard]] bool input_enabled() const noexcept { return input_.enabled; }
    // Turns input on or off for the game, as Godot's set_input_device_active; fails, with a
    // warning, when input isn't enabled. Microphone playbacks keep it on while they play too.
    bool set_input_active(bool active);
    // Whether input is on, for the game or a microphone playback.
    [[nodiscard]] bool input_active() const noexcept;
    // Adds frames recorded, while input is on. Turning it on starts the buffer over.
    void push_input(std::span<const Frame> frames);
    // Godot's get_input_frames_available, get_input_buffer_length_frames and get_input_frames:
    // the frames recorded that the game hasn't read, how many the buffer holds, and the next
    // frames, or none when fewer than that many are waiting.
    [[nodiscard]] int input_frames_available() const;
    [[nodiscard]] int input_buffer_length_frames() const;
    [[nodiscard]] std::vector<Frame> input_frames(int frames);
    // For microphone playbacks: keep input on (false when it isn't enabled) and let it go, the
    // frames recorded since input turned on, and one of them by that count while the buffer
    // holds it.
    bool acquire_input();
    void release_input();
    [[nodiscard]] std::uint64_t input_recorded() const noexcept { return input_.recorded; }
    [[nodiscard]] Frame input_frame(std::uint64_t index) const;

private:
    struct Input {
        std::vector<Frame> buffer;
        std::uint64_t recorded = 0; // frames pushed since input turned on
        std::uint64_t read = 0;     // frames input_frames read
        int users = 0;              // microphone playbacks playing
        bool enabled = false;
        bool game = false; // set_input_active
    };
    void begin_input();

    struct Impl;
    std::unique_ptr<Impl> impl_;
    int mix_rate_;
    BusLayout layout_;
    std::uint64_t mixed_frames_ = 0;
    BusListener bus_listener_;
    Input input_;
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
    // Whether microphone input can turn on, as Godot's audio/driver/enable_input. Exports then
    // ask the player for the microphone, giving input_usage_description as the reason on macOS
    // and iOS.
    bool enable_input = false;
    std::string input_usage_description;

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

// Where microphone input comes from, such as an SdlInput. The audio system starts it when input
// turns on, reads each tick's frames from it and stops it when input turns off.
class InputDevice {
public:
    InputDevice() = default;
    virtual ~InputDevice() = default;
    InputDevice(const InputDevice&) = delete;
    InputDevice& operator=(const InputDevice&) = delete;
    InputDevice(InputDevice&&) = delete;
    InputDevice& operator=(InputDevice&&) = delete;

    // Starts recording at the mix rate; fails where there is no microphone or the player said no.
    virtual Result<void> start(int mix_rate) = 0;
    virtual void stop() = 0;
    // The next frames recorded; any it doesn't have yet are silence.
    virtual void read(std::span<Frame> frames) = 0;
    [[nodiscard]] virtual std::string name() const = 0;
};
// The world's input device, which must outlive its use there; nullptr records silence.
void set_input_device(world::World& world, InputDevice* device);
// Plays a stream (a sound or a .audio.json file) into the input from the next tick, in place of
// the device, so agents and tests can speak into a game; "" goes back to the device. It plays
// while input is on, from where input turned on. Fails if the stream can't load.
Result<void> set_input_stream(world::World& world, std::string_view stream);
// What feeds the input and what it recorded last tick.
struct InputInfo {
    bool enabled = false;
    bool active = false;
    std::string source; // the input stream, the device's name, or "" for silence
    std::string error;  // why the device didn't start
    float peak_db = -200.0F;
};
[[nodiscard]] InputInfo input_info(const world::World& world);

// Plays an AudioStreamPlayer or AudioStreamPlayer2D from a position in seconds at the next
// tick, as Godot's play(): with max_polyphony 1 it restarts, with more another voice joins.
void play(world::World& world, entt::entity entity, double from_seconds = 0.0);
// Stops it at the next tick, as Godot's stop().
void stop(world::World& world, entt::entity entity);
// Plays a stream (a sound or a .audio.json file) on a player whose stream is polyphonic, at the
// next tick, as Godot's AudioStreamPlaybackPolyphonic::play_stream; the player starts first if
// it isn't playing. Fails if the player's stream isn't polyphonic or the stream can't load.
Result<void> play_stream(world::World& world, entt::entity entity, std::string_view stream,
                         double from_seconds = 0.0, float volume_db = 0.0F,
                         float pitch_scale = 1.0F);
// The playback of a player's newest voice, as Godot's get_stream_playback, or nullptr when it
// isn't playing. A voice starts when the audio system runs after play() (or autoplay), so
// gameplay systems, which run before it, see it from the next tick; ask again each tick, as it
// goes away when the voice ends. generator() and polyphonic() give the kind of playback.
[[nodiscard]] Playback* stream_playback(world::World& world, entt::entity entity);
// Where its newest voice is, in seconds, or 0.
[[nodiscard]] double playback_position(const world::World& world, entt::entity entity);
// Starts or stops a bus's record effect at the next tick, as play does, so sessions can take it
// back. Starting drops the last recording.
void set_recording(world::World& world, int bus, int effect, bool active);
// What set_recording asked of a record effect for the next tick: the last state asked, and
// whether a start, which drops the recording, is among the asks.
struct RecordingAsks {
    std::optional<bool> active;
    bool restarts = false;
};
[[nodiscard]] RecordingAsks recording_asks(const world::World& world, int bus, int effect);

// The plays, stops and recordings asked for the next tick and the buses' volumes and switches,
// which sessions put back with the world when a command fails or is a dry run.
struct Snapshot {
    struct Request {
        std::string player; // "<kind>/<entity ID>"
        std::string stream; // play_stream's stream, if any
        double from_seconds = 0.0;
        float volume_db = 0.0F;   // play_stream's
        float pitch_scale = 1.0F; // play_stream's
        bool play = true;
    };
    struct Recording {
        int bus = 0;
        int effect = 0;
        bool active = false;
    };
    std::vector<Request> requests;
    std::vector<Recording> recordings;
    std::vector<BusSettings> buses;
    std::optional<std::string> input_stream; // set_input_stream's ask for the next tick
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
    std::string clip;  // the interactive music clip playing, if any
    std::string error; // why its stream did not load
    // A generator's frames that can be pushed and mixes that ran out of frames.
    struct Generator {
        int frames_available = 0;
        int skips = 0;
    };
    std::optional<Generator> generator;
    // A polyphonic stream's streams playing and how many can.
    struct Polyphonic {
        int playing = 0;
        int polyphony = 0;
    };
    std::optional<Polyphonic> polyphonic;
};
// Every player with an ID, by ID.
[[nodiscard]] std::vector<PlayerInfo> players(const world::World& world);

// Records the default microphone through SDL, as Godot's audio drivers do once input turns on.
// On Android, starting asks the player for the RECORD_AUDIO permission and waits for the answer.
class SdlInput final : public InputDevice {
public:
    SdlInput() = default;
    ~SdlInput() override;
    SdlInput(const SdlInput&) = delete;
    SdlInput& operator=(const SdlInput&) = delete;
    SdlInput(SdlInput&&) = delete;
    SdlInput& operator=(SdlInput&&) = delete;

    Result<void> start(int mix_rate) override;
    void stop() override;
    void read(std::span<Frame> frames) override;
    [[nodiscard]] std::string name() const override { return name_; }

private:
    void close();

    void* stream_ = nullptr; // SDL_AudioStream
    std::string name_ = "microphone";
    int latency_frames_ = 0;
    bool primed_ = false; // latency_frames_ waited since it started or ran dry
};

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

// A PCM .wav file of frames, at 16 bits or 8.
[[nodiscard]] std::string encode_wav(std::span<const Frame> frames, int mix_rate, int bits = 16);

} // namespace slash::audio
