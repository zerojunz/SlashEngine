#include "slash/audio/audio.hpp"
#include "slash/core/text.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <miniaudio.h>
#include <optional>
#include <utility>

namespace slash::audio {

namespace {

constexpr std::array<std::pair<std::string_view, LoopMode>, 4> kLoopModes{{
    {"disabled", LoopMode::Disabled},
    {"forward", LoopMode::Forward},
    {"pingpong", LoopMode::PingPong},
    {"backward", LoopMode::Backward},
}};

constexpr std::array<std::pair<std::string_view, PlaybackMode>, 3> kPlaybackModes{{
    {"random_no_repeats", PlaybackMode::RandomNoRepeats},
    {"random", PlaybackMode::Random},
    {"sequential", PlaybackMode::Sequential},
}};

constexpr std::array<std::pair<std::string_view, AutoAdvance>, 3> kAutoAdvance{{
    {"disabled", AutoAdvance::Disabled},
    {"enabled", AutoAdvance::Enabled},
    {"return_to_hold", AutoAdvance::ReturnToHold},
}};

constexpr std::array<std::pair<std::string_view, TransitionFrom>, 4> kTransitionFrom{{
    {"immediate", TransitionFrom::Immediate},
    {"next_beat", TransitionFrom::NextBeat},
    {"next_bar", TransitionFrom::NextBar},
    {"end", TransitionFrom::End},
}};

constexpr std::array<std::pair<std::string_view, TransitionTo>, 3> kTransitionTo{{
    {"same_position", TransitionTo::SamePosition},
    {"start", TransitionTo::Start},
    {"previous_position", TransitionTo::PreviousPosition},
}};

constexpr std::array<std::pair<std::string_view, FadeMode>, 5> kFadeModes{{
    {"disabled", FadeMode::Disabled},
    {"in", FadeMode::In},
    {"out", FadeMode::Out},
    {"cross", FadeMode::Cross},
    {"automatic", FadeMode::Automatic},
}};

constexpr std::array<std::pair<std::string_view, MixRateMode>, 3> kMixRateModes{{
    {"output", MixRateMode::Output},
    {"input", MixRateMode::Input},
    {"custom", MixRateMode::Custom},
}};

constexpr std::string_view kPathMessage =
    R"(expected a path inside the project folder, separated by '/', with no "." or ".." parts)";

std::string lower_extension(std::string_view path) {
    const std::size_t dot = path.rfind('.');
    std::string extension(dot == std::string_view::npos ? std::string_view{} : path.substr(dot));
    std::ranges::transform(extension, extension.begin(), [](char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    });
    return extension;
}

json::Value number(std::string_view doc, double minimum, double maximum, double fallback) {
    json::Value value;
    value["type"] = "number";
    value["minimum"] = minimum;
    value["maximum"] = maximum;
    value["default"] = fallback;
    value["description"] = doc;
    return value;
}

template <typename Enum, std::size_t N>
json::Value choice(std::string_view doc,
                   const std::array<std::pair<std::string_view, Enum>, N>& names,
                   std::string_view fallback) {
    json::Value value;
    value["type"] = "string";
    json::Array options;
    for (const auto& [name, each] : names) {
        options.emplace_back(name);
    }
    value["enum"] = std::move(options);
    value["default"] = fallback;
    value["description"] = doc;
    return value;
}

template <typename Enum, std::size_t N>
Enum parse_choice(const json::Value* value,
                  const std::array<std::pair<std::string_view, Enum>, N>& names, Enum fallback) {
    if (value == nullptr) {
        return fallback;
    }
    for (const auto& [name, each] : names) {
        if (name == value->as_string()) {
            return each;
        }
    }
    return fallback;
}

void put_u16(std::string& out, std::uint16_t value) {
    out.push_back(static_cast<char>(value & 0xFFU));
    out.push_back(static_cast<char>((value >> 8U) & 0xFFU));
}

void put_u32(std::string& out, std::uint32_t value) {
    put_u16(out, static_cast<std::uint16_t>(value & 0xFFFFU));
    put_u16(out, static_cast<std::uint16_t>(value >> 16U));
}

} // namespace

float db_to_linear(float db) {
    return std::exp(db * 0.11512925464970228420089957273422F);
}

float linear_to_db(float linear) {
    return std::log(linear) * 8.6858896380650365530225783783321F;
}

bool is_sound_path(std::string_view path) {
    const std::string extension = lower_extension(path);
    return extension == ".wav" || extension == ".ogg" || extension == ".mp3";
}

Result<Sound> decode_sound(std::string_view data, std::string_view path) {
    const std::string extension = lower_extension(path);
    Sound sound;
    ma_encoding_format encoding = ma_encoding_format_wav;
    std::string_view name = "WAV";
    if (extension == ".ogg") {
        sound.format = Sound::Format::Vorbis;
        encoding = ma_encoding_format_vorbis;
        name = "Ogg Vorbis";
    } else if (extension == ".mp3") {
        sound.format = Sound::Format::Mp3;
        encoding = ma_encoding_format_mp3;
        name = "MP3";
    } else if (extension != ".wav") {
        return make_error(ErrorCode::InvalidArgument,
                          "\"" + std::string(path) + "\" is not a .wav, .ogg or .mp3 file");
    }
    if (data.empty()) {
        return make_error(ErrorCode::InvalidArgument, "\"" + std::string(path) + "\" is empty");
    }
    const auto open = [&](ma_format format, ma_uint32 channels, ma_decoder& decoder) {
        ma_decoder_config config = ma_decoder_config_init(format, channels, 0);
        config.encodingFormat = encoding;
        return ma_decoder_init_memory(data.data(), data.size(), &config, &decoder);
    };
    const auto invalid = [&] {
        return make_error(ErrorCode::InvalidArgument, "\"" + std::string(path) +
                                                          "\" is not a valid " + std::string(name) +
                                                          " file");
    };
    // Opened in the file's own sample format first, to see what it is.
    ma_decoder decoder;
    if (open(ma_format_unknown, 0, decoder) != MA_SUCCESS) {
        return invalid();
    }
    ma_format format = ma_format_unknown;
    ma_uint32 channels = 0;
    ma_uint32 rate = 0;
    if (ma_decoder_get_data_format(&decoder, &format, &channels, &rate, nullptr, 0) != MA_SUCCESS ||
        channels == 0 || rate == 0) {
        ma_decoder_uninit(&decoder);
        return invalid();
    }
    // Godot plays mono and stereo; more channels are mixed down to two. Integer samples are read
    // as 32-bit integers, which every integer size converts to exactly, then scaled as Godot's
    // importer does (an 8-bit 192 is 0.5), rather than by miniaudio's float conversion.
    const bool integer = format != ma_format_f32;
    ma_decoder_uninit(&decoder);
    channels = std::min<ma_uint32>(channels, 2);
    if (open(integer ? ma_format_s32 : ma_format_f32, channels, decoder) != MA_SUCCESS) {
        return invalid();
    }
    sound.channels = static_cast<int>(channels);
    sound.mix_rate = static_cast<int>(rate);
    ma_uint64 length = 0;
    if (ma_decoder_get_length_in_pcm_frames(&decoder, &length) != MA_SUCCESS) {
        length = 0;
    }
    if (sound.format == Sound::Format::Wav) {
        // As Godot imports WAV files: every sample decoded now.
        constexpr ma_uint64 kChunk = 4096;
        constexpr float kIntegerScale = 1.0F / 2147483648.0F;
        std::vector<float> samples;
        samples.reserve(static_cast<std::size_t>(length) * channels);
        std::array<float, kChunk * 2> chunk{};
        std::array<std::int32_t, kChunk * 2> ints{};
        for (;;) {
            ma_uint64 read = 0;
            const ma_result result = ma_decoder_read_pcm_frames(
                &decoder, integer ? static_cast<void*>(ints.data()) : chunk.data(), kChunk, &read);
            const auto count = static_cast<std::size_t>(read * channels);
            if (integer) {
                for (std::size_t i = 0; i < count; ++i) {
                    chunk[i] = static_cast<float>(ints[i]) * kIntegerScale;
                }
            }
            samples.insert(samples.end(), chunk.begin(),
                           chunk.begin() + static_cast<std::ptrdiff_t>(count));
            if (result != MA_SUCCESS || read < kChunk) {
                break;
            }
        }
        sound.frames = static_cast<std::int64_t>(samples.size() / channels);
        sound.samples = std::move(samples);
    } else {
        // Compressed sounds decode as they play; count their frames now if the format can't say.
        if (length == 0) {
            constexpr std::size_t kChunk = 4096;
            std::array<float, kChunk * 2> chunk{};
            for (;;) {
                ma_uint64 read = 0;
                const ma_result result =
                    ma_decoder_read_pcm_frames(&decoder, chunk.data(), kChunk, &read);
                length += read;
                if (result != MA_SUCCESS || read < kChunk) {
                    break;
                }
            }
        }
        sound.frames = static_cast<std::int64_t>(length);
        sound.data = std::string(data);
    }
    ma_decoder_uninit(&decoder);
    if (sound.frames == 0) {
        return make_error(ErrorCode::InvalidArgument,
                          "\"" + std::string(path) + "\" has no sound in it");
    }
    return sound;
}

std::string encode_wav(std::span<const Frame> frames, int mix_rate, int bits, int pairs) {
    const bool eight = bits == 8;
    const auto channels = static_cast<std::uint16_t>(2 * std::max(pairs, 1));
    const auto sample_size = static_cast<std::uint16_t>(eight ? 1 : 2);
    const auto frame_size = static_cast<std::uint32_t>(channels * sample_size);
    const auto data_size = static_cast<std::uint32_t>(frames.size() * 2 * sample_size);
    // More than two channels name their speakers, as WAVE_FORMAT_EXTENSIBLE does: front left
    // and right, center, LFE, back left and right (0x3F), then side left and right (0x600).
    const bool extensible = channels > 2;
    constexpr std::array<std::uint32_t, 4> kChannelMasks{0x3, 0xF, 0x3F, 0x63F};
    const std::uint32_t fmt_size = extensible ? 40 : 16;
    std::string out;
    out.reserve(28 + fmt_size + data_size);
    out += "RIFF";
    put_u32(out, 20 + fmt_size + data_size);
    out += "WAVEfmt ";
    put_u32(out, fmt_size);
    put_u16(out, extensible ? 0xFFFE : 1); // WAVE_FORMAT_EXTENSIBLE or PCM
    put_u16(out, channels);
    put_u32(out, static_cast<std::uint32_t>(mix_rate));
    put_u32(out, static_cast<std::uint32_t>(mix_rate) * frame_size);
    put_u16(out, static_cast<std::uint16_t>(frame_size));
    put_u16(out, eight ? 8 : 16);
    if (extensible) {
        put_u16(out, 22);             // the extension's size
        put_u16(out, eight ? 8 : 16); // valid bits per sample
        put_u32(out, kChannelMasks.at(static_cast<std::size_t>(channels / 2) - 1));
        // KSDATAFORMAT_SUBTYPE_PCM: 00000001-0000-0010-8000-00aa00389b71.
        put_u32(out, 1);
        put_u16(out, 0);
        put_u16(out, 0x10);
        out += std::string_view("\x80\x00\x00\xaa\x00\x38\x9b\x71", 8);
    }
    out += "data";
    put_u32(out, data_size);
    const auto put = [&](float value) {
        const float clamped = std::clamp(value, -1.0F, 1.0F);
        if (eight) {
            // 8-bit samples are unsigned, around 128.
            out +=
                static_cast<char>(static_cast<std::uint8_t>(std::lround(clamped * 127.0F) + 128));
        } else {
            put_u16(out, static_cast<std::uint16_t>(
                             static_cast<std::int16_t>(std::lround(clamped * 32767.0F))));
        }
    };
    for (const Frame& frame : frames) {
        put(frame.left);
        put(frame.right);
    }
    return out;
}

std::string_view loop_mode_name(LoopMode mode) {
    for (const auto& [name, each] : kLoopModes) {
        if (each == mode) {
            return name;
        }
    }
    return "disabled";
}

std::string_view playback_mode_name(PlaybackMode mode) {
    for (const auto& [name, each] : kPlaybackModes) {
        if (each == mode) {
            return name;
        }
    }
    return "random_no_repeats";
}

json::Value stream_file_schema() {
    json::Value schema;
    schema["$schema"] = "https://json-schema.org/draft/2020-12/schema";
    schema["title"] = "SlashEngine audio stream";
    schema["description"] =
        "One of: a sound with Godot's import settings (source); Godot's AudioStreamRandomizer, "
        "one of several sounds each time it plays (streams); music, as Godot's "
        "AudioStreamPlaylist (playlist), AudioStreamSynchronized (synchronized) and "
        "AudioStreamInteractive (clips); Godot's AudioStreamGenerator (generator) or "
        "AudioStreamPolyphonic (polyphonic), which a game's code plays frames or streams on; or "
        "Godot's AudioStreamMicrophone (microphone).";
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["required"] = json::Array{"format"};
    json::Value& properties = schema["properties"];
    properties["format"]["const"] = 1;
    properties["format"]["description"] = "Format version. Must be 1.";
    properties["source"]["type"] = "string";
    properties["source"]["minLength"] = 1;
    properties["source"]["description"] = "The .wav, .ogg or .mp3 file.";
    properties["loop_mode"] =
        choice(".wav: how it loops between loop_begin and loop_end.", kLoopModes, "disabled");
    json::Value& begin = properties["loop_begin"];
    begin["type"] = "integer";
    begin["minimum"] = 0;
    begin["default"] = 0;
    begin["description"] = ".wav: the frame the loop starts at.";
    json::Value& end = properties["loop_end"];
    end["type"] = "integer";
    end["minimum"] = -1;
    end["default"] = -1;
    end["description"] = ".wav: the frame the loop ends at; -1 for the end of the sound.";
    properties["loop"]["type"] = "boolean";
    properties["loop"]["description"] = ".ogg and .mp3: play again from loop_offset at the end "
                                        "(default false). Playlists: start over after the last "
                                        "stream (default true).";
    properties["loop_offset"] =
        number(".ogg and .mp3: seconds into the sound a loop goes back to.", 0, 86400, 0);
    properties["bpm"] = number("Beats per minute, which interactive music times transitions "
                               "by; an .ogg or .mp3 with beat_count loops after that many beats "
                               "instead of at the end.",
                               0, 400, 0);
    json::Value& beats = properties["beat_count"];
    beats["type"] = "integer";
    beats["minimum"] = 0;
    beats["maximum"] = 512;
    beats["default"] = 0;
    beats["description"] = "Its length in beats at bpm, for loops, playlists and transitions.";
    json::Value& bar = properties["bar_beats"];
    bar["type"] = "integer";
    bar["minimum"] = 2;
    bar["maximum"] = 32;
    bar["default"] = 4;
    bar["description"] = "Beats in a bar, for next_bar transitions.";
    const auto list = [](std::string_view doc, int max_items, json::Value item) {
        json::Value value;
        value["type"] = "array";
        value["minItems"] = 1;
        value["maxItems"] = max_items;
        value["description"] = doc;
        item["type"] = "object";
        item["additionalProperties"] = false;
        item["properties"]["stream"]["type"] = "string";
        item["properties"]["stream"]["minLength"] = 1;
        item["properties"]["stream"]["description"] = "A .wav, .ogg, .mp3 or .audio.json file.";
        item["required"] = json::Array{"stream"};
        value["items"] = std::move(item);
        return value;
    };
    json::Value weighted;
    weighted["properties"]["weight"] =
        number("How likely it is picked, against the others.", 0, 1000, 1);
    properties["streams"] =
        list("A randomizer's streams, each a sound or another .audio.json file.", 64, weighted);
    properties["playback_mode"] =
        choice("How a randomizer picks: at random but never the same one twice in a row, at "
               "random, or each in turn.",
               kPlaybackModes, "random_no_repeats");
    properties["random_pitch"] = number("A randomizer's pitch scale varies up to this many "
                                        "times higher or lower; 1 for none.",
                                        1, 16, 1);
    properties["random_volume_offset_db"] =
        number("A randomizer's volume varies up to this many dB louder or quieter.", 0, 40, 0);
    properties["playlist"] = list("A playlist's streams, played in turn, each for its beat_count "
                                  "beats if it has them, else to its end.",
                                  64, json::Value(json::Object{}));
    properties["shuffle"]["type"] = "boolean";
    properties["shuffle"]["default"] = false;
    properties["shuffle"]["description"] = "A playlist plays in a new random order each time.";
    properties["fade_time"] =
        number("Seconds a playlist's stream fades out over the start of the next.", 0, 1, 0.3);
    json::Value layered;
    layered["properties"]["volume_db"] =
        number("Its volume; a player's stream_<index>_volume_db parameter changes it.", -80, 24, 0);
    properties["synchronized"] = list(
        "Streams that play together from the same start, as layers of one piece.", 32, layered);
    json::Value clip;
    clip["properties"]["name"]["type"] = "string";
    clip["properties"]["name"]["minLength"] = 1;
    clip["properties"]["name"]["maxLength"] = 64;
    clip["properties"]["name"]["description"] = "The name a player's switch_to_clip uses.";
    clip["properties"]["auto_advance"] =
        choice("At its end: stop, go on to next_clip, or go back to the clip a hold_previous "
               "transition left.",
               kAutoAdvance, "disabled");
    clip["properties"]["next_clip"]["type"] = "string";
    clip["properties"]["next_clip"]["description"] =
        "The clip it goes on to, with auto_advance \"enabled\".";
    json::Value clips = list("Interactive music's clips, one playing at a time.", 63, clip);
    clips["items"]["required"] = json::Array{"name", "stream"};
    properties["clips"] = std::move(clips);
    properties["initial_clip"]["type"] = "string";
    properties["initial_clip"]["description"] = "The clip it starts with. Default: the first.";
    json::Value& transitions = properties["transitions"];
    transitions["type"] = "array";
    transitions["maxItems"] = 64 * 64; // each pair of clips, or any clip
    transitions["description"] =
        "How interactive music goes from one clip to another. The first that fits wins, in this "
        "order: from and to both named, from named, to named, neither; with none, it changes on "
        "the next beat and fades out the clip it left over a beat.";
    json::Value& transition = transitions["items"];
    transition["type"] = "object";
    transition["additionalProperties"] = false;
    json::Value& fields = transition["properties"];
    fields["from"]["type"] = "string";
    fields["from"]["description"] = "The clip it leaves. Default: any.";
    fields["to"]["type"] = "string";
    fields["to"]["description"] = "The clip it goes to. Default: any.";
    fields["from_time"] = choice("When it leaves: at once, on the next beat or bar, or at the "
                                 "clip's end.",
                                 kTransitionFrom, "next_beat");
    fields["to_time"] = choice("Where the next clip starts: at the same time into it, at its "
                               "start, or where it was last left.",
                               kTransitionTo, "start");
    fields["fade_mode"] =
        choice("Which clip fades: neither, the next in, the last out, both, or out when the "
               "next starts at its start and both otherwise.",
               kFadeModes, "automatic");
    fields["fade_beats"] =
        number("Beats a fade takes, or seconds if the clip has no bpm.", 0, 64, 1);
    fields["filler_clip"]["type"] = "string";
    fields["filler_clip"]["description"] = "A clip played once between the two.";
    fields["hold_previous"]["type"] = "boolean";
    fields["hold_previous"]["default"] = false;
    fields["hold_previous"]["description"] =
        "Remember the clip it left, for a clip with auto_advance \"return_to_hold\".";
    json::Value& generator = properties["generator"];
    generator["type"] = "object";
    generator["additionalProperties"] = false;
    generator["description"] =
        "Plays frames a game's code pushes as it runs, as Godot's AudioStreamGenerator.";
    generator["properties"]["mix_rate_mode"] =
        choice("The rate frames are pushed at: the mix rate, the rate microphone input is "
               "recorded at (the mix rate), or mix_rate.",
               kMixRateModes, "custom");
    generator["properties"]["mix_rate"] =
        number("Frames per second pushed, with mix_rate_mode \"custom\".", 20, 192000, 44100);
    generator["properties"]["buffer_length"] = number(
        "Seconds of frames it holds; a game pushes more before they run out.", 0.01, 10, 0.5);
    json::Value& polyphonic = properties["polyphonic"];
    polyphonic["type"] = "object";
    polyphonic["additionalProperties"] = false;
    polyphonic["description"] = "Plays streams a game starts on one player, each with its own "
                                "volume and pitch, as Godot's AudioStreamPolyphonic.";
    json::Value& polyphony = polyphonic["properties"]["polyphony"];
    polyphony["type"] = "integer";
    polyphony["minimum"] = 1;
    polyphony["maximum"] = 128;
    polyphony["default"] = 32;
    polyphony["description"] = "Streams that can play at once.";
    json::Value& microphone = properties["microphone"];
    microphone["type"] = "object";
    microphone["additionalProperties"] = false;
    microphone["properties"] = json::Object{};
    microphone["description"] =
        "Plays what the microphone records, 50 ms late, as Godot's AudioStreamMicrophone; only as "
        "a player's own stream, with project.json's audio.enable_input on.";
    return schema;
}

namespace {

// The kinds of .audio.json file, by the member that makes one.
constexpr std::array<std::pair<std::string_view, StreamFile::Kind>, 8> kKinds{{
    {"source", StreamFile::Kind::Sound},
    {"streams", StreamFile::Kind::Randomizer},
    {"playlist", StreamFile::Kind::Playlist},
    {"synchronized", StreamFile::Kind::Synchronized},
    {"clips", StreamFile::Kind::Interactive},
    {"generator", StreamFile::Kind::Generator},
    {"polyphonic", StreamFile::Kind::Polyphonic},
    {"microphone", StreamFile::Kind::Microphone},
}};

// Members only some kinds take, and the message for the others.
struct KindKey {
    std::string_view key;
    std::uint8_t kinds; // a bit per Kind
    std::string_view who;
};

constexpr std::uint8_t bit(StreamFile::Kind kind) {
    return static_cast<std::uint8_t>(1U << static_cast<unsigned>(kind));
}

constexpr std::string_view kSoundOnly = "only a sound (\"source\") takes this";
constexpr std::string_view kRandomizerOnly = "only a randomizer (\"streams\") takes this";
constexpr std::string_view kPlaylistOnly = "only a playlist takes this";
constexpr std::string_view kInteractiveOnly = "only interactive music (\"clips\") takes this";

constexpr std::array<KindKey, 15> kKindKeys{{
    {"loop_mode", bit(StreamFile::Kind::Sound), kSoundOnly},
    {"loop_begin", bit(StreamFile::Kind::Sound), kSoundOnly},
    {"loop_end", bit(StreamFile::Kind::Sound), kSoundOnly},
    {"loop", bit(StreamFile::Kind::Sound) | bit(StreamFile::Kind::Playlist),
     "only a sound or a playlist takes this"},
    {"loop_offset", bit(StreamFile::Kind::Sound), kSoundOnly},
    {"bpm", bit(StreamFile::Kind::Sound), kSoundOnly},
    {"beat_count", bit(StreamFile::Kind::Sound), kSoundOnly},
    {"bar_beats", bit(StreamFile::Kind::Sound), kSoundOnly},
    {"playback_mode", bit(StreamFile::Kind::Randomizer), kRandomizerOnly},
    {"random_pitch", bit(StreamFile::Kind::Randomizer), kRandomizerOnly},
    {"random_volume_offset_db", bit(StreamFile::Kind::Randomizer), kRandomizerOnly},
    {"shuffle", bit(StreamFile::Kind::Playlist), kPlaylistOnly},
    {"fade_time", bit(StreamFile::Kind::Playlist), kPlaylistOnly},
    {"initial_clip", bit(StreamFile::Kind::Interactive), kInteractiveOnly},
    {"transitions", bit(StreamFile::Kind::Interactive), kInteractiveOnly},
}};

// A clip's index by name, or -1 with an error at `at` naming the closest clip.
int clip_index(const json::Array& clips, std::string_view name, const std::string& at,
               std::vector<json::ValidationError>& errors) {
    std::vector<std::string_view> names;
    for (std::size_t i = 0; i < clips.size(); ++i) {
        const std::string& each = clips[i].find("name")->as_string();
        if (each == name) {
            return static_cast<int>(i);
        }
        names.emplace_back(each);
    }
    std::string message = "no clip is named \"" + std::string(name) + "\"";
    if (const std::string_view hint = closest_match(name, names); !hint.empty()) {
        message += "; did you mean \"" + std::string(hint) + "\"?";
    }
    errors.push_back({at, std::move(message)});
    return -1;
}

std::optional<int> optional_clip(const json::Value& object, std::string_view key,
                                 const json::Array& clips, const std::string& at,
                                 std::vector<json::ValidationError>& errors) {
    const json::Value* name = object.find(key);
    if (name == nullptr) {
        return std::nullopt;
    }
    return clip_index(clips, name->as_string(), at + "/" + std::string(key), errors);
}

void validate_clips(const json::Value& doc, std::vector<json::ValidationError>& errors) {
    const json::Array& clips = doc.find("clips")->as_array();
    for (std::size_t i = 0; i < clips.size(); ++i) {
        const std::string at = "/clips/" + std::to_string(i);
        const std::string& name = clips[i].find("name")->as_string();
        for (std::size_t j = 0; j < i; ++j) {
            if (clips[j].find("name")->as_string() == name) {
                errors.push_back(
                    {at + "/name", "clip " + std::to_string(j) + " already has this name"});
            }
        }
        const AutoAdvance advance =
            parse_choice(clips[i].find("auto_advance"), kAutoAdvance, AutoAdvance::Disabled);
        const std::optional<int> next = optional_clip(clips[i], "next_clip", clips, at, errors);
        if (advance == AutoAdvance::Enabled && !next.has_value()) {
            errors.push_back({at, "auto_advance \"enabled\" needs next_clip"});
        } else if (next.has_value() && advance != AutoAdvance::Enabled) {
            errors.push_back({at + "/next_clip", "only a clip with auto_advance \"enabled\" goes "
                                                 "on to next_clip"});
        } else if (next.has_value() && *next == static_cast<int>(i)) {
            errors.push_back({at + "/next_clip", "a clip can't go on to itself; loop its sound "
                                                 "instead"});
        }
    }
    static_cast<void>(optional_clip(doc, "initial_clip", clips, "", errors));
    const json::Value* transitions = doc.find("transitions");
    if (transitions == nullptr) {
        return;
    }
    std::vector<std::pair<int, int>> seen;
    const json::Array& list = transitions->as_array();
    for (std::size_t i = 0; i < list.size(); ++i) {
        const std::string at = "/transitions/" + std::to_string(i);
        const std::optional<int> from = optional_clip(list[i], "from", clips, at, errors);
        const std::optional<int> to = optional_clip(list[i], "to", clips, at, errors);
        static_cast<void>(optional_clip(list[i], "filler_clip", clips, at, errors));
        const std::pair<int, int> key{from.value_or(-1), to.value_or(-1)};
        if (const auto found = std::ranges::find(seen, key); found != seen.end()) {
            errors.push_back({at, "transition " + std::to_string(found - seen.begin()) +
                                      " is already from and to the same clips"});
        }
        seen.push_back(key);
    }
}

} // namespace

std::vector<json::ValidationError> validate_stream_file(const json::Value& doc) {
    std::vector<json::ValidationError> errors = json::validate(doc, stream_file_schema());
    if (!errors.empty()) {
        return errors;
    }
    std::vector<std::string_view> found;
    StreamFile::Kind kind = StreamFile::Kind::Sound;
    for (const auto& [key, each] : kKinds) {
        if (doc.find(key) != nullptr) {
            found.push_back(key);
            kind = each;
        }
    }
    if (found.size() != 1) {
        errors.push_back(
            {"", found.empty()
                     ? "needs one of \"source\" (a sound with settings), \"streams\" (a "
                       "randomizer), \"playlist\", \"synchronized\", \"clips\" (music), "
                       "\"generator\", \"polyphonic\" or \"microphone\""
                     : "has both \"" + std::string(found[0]) + "\" and \"" + std::string(found[1]) +
                           "\"; a file is one sound, randomizer, piece of music, generator, "
                           "polyphonic stream or microphone"});
        return errors;
    }
    for (const auto& [key, value] : doc.as_object()) {
        for (const KindKey& rule : kKindKeys) {
            if (rule.key == key && (rule.kinds & bit(kind)) == 0) {
                errors.push_back({"/" + key, std::string(rule.who)});
            }
        }
    }
    const auto check_path = [&](const std::string& path, const std::string& at, bool streams_too) {
        if (!is_project_path(path)) {
            errors.push_back({at, std::string(kPathMessage)});
        } else if (!is_sound_path(path) && !(streams_too && path.ends_with(kStreamSuffix))) {
            errors.push_back({at, streams_too ? "expected a .wav, .ogg, .mp3 or .audio.json file"
                                              : "expected a .wav, .ogg or .mp3 file"});
        }
    };
    if (kind == StreamFile::Kind::Sound) {
        const std::string& path = doc.find("source")->as_string();
        check_path(path, "/source", false);
        if (!is_project_path(path) || !is_sound_path(path)) {
            return errors;
        }
        if (lower_extension(path) == ".wav") {
            for (const char* key : {"loop", "loop_offset"}) {
                if (doc.find(key) != nullptr) {
                    errors.push_back({"/" + std::string(key),
                                      "a .wav source loops with loop_mode, loop_begin and "
                                      "loop_end"});
                }
            }
            const json::Value* begin_at = doc.find("loop_begin");
            const json::Value* end_at = doc.find("loop_end");
            if (begin_at != nullptr && end_at != nullptr && end_at->as_int() >= 0 &&
                end_at->as_int() <= begin_at->as_int()) {
                errors.push_back({"/loop_end", "must come after loop_begin"});
            }
        } else {
            for (const char* key : {"loop_mode", "loop_begin", "loop_end"}) {
                if (doc.find(key) != nullptr) {
                    errors.push_back({"/" + std::string(key),
                                      "an .ogg or .mp3 source loops with loop and loop_offset"});
                }
            }
        }
        return errors;
    }
    if (kind == StreamFile::Kind::Generator || kind == StreamFile::Kind::Polyphonic ||
        kind == StreamFile::Kind::Microphone) {
        return errors;
    }
    const std::string member(found[0]);
    const json::Array& entries = doc.find(member)->as_array();
    for (std::size_t i = 0; i < entries.size(); ++i) {
        check_path(entries[i].find("stream")->as_string(),
                   "/" + member + "/" + std::to_string(i) + "/stream", true);
    }
    if (kind == StreamFile::Kind::Interactive) {
        validate_clips(doc, errors);
    }
    return errors;
}

Result<StreamFile> parse_stream_file(const json::Value& doc) {
    if (const auto errors = validate_stream_file(doc); !errors.empty()) {
        const json::ValidationError& first = errors.front();
        return make_error(ErrorCode::InvalidArgument,
                          (first.path.empty() ? std::string("root") : first.path) + ": " +
                              first.message);
    }
    StreamFile out;
    for (const auto& [key, kind] : kKinds) {
        if (doc.find(key) != nullptr) {
            out.kind = kind;
        }
    }
    if (const json::Value* source = doc.find("source"); source != nullptr) {
        out.source = source->as_string();
    }
    out.loop_mode = parse_choice(doc.find("loop_mode"), kLoopModes, out.loop_mode);
    if (const json::Value* begin = doc.find("loop_begin"); begin != nullptr) {
        out.loop_begin = begin->as_int();
    }
    if (const json::Value* end = doc.find("loop_end"); end != nullptr) {
        out.loop_end = end->as_int();
    }
    // As Godot's, a playlist loops unless told not to.
    out.loop = out.kind == StreamFile::Kind::Playlist;
    if (const json::Value* loop = doc.find("loop"); loop != nullptr) {
        out.loop = loop->as_bool();
    }
    if (const json::Value* offset = doc.find("loop_offset"); offset != nullptr) {
        out.loop_offset = offset->as_double();
    }
    if (const json::Value* bpm = doc.find("bpm"); bpm != nullptr) {
        out.bpm = bpm->as_double();
    }
    if (const json::Value* beats = doc.find("beat_count"); beats != nullptr) {
        out.beat_count = static_cast<int>(beats->as_int());
    }
    if (const json::Value* bar = doc.find("bar_beats"); bar != nullptr) {
        out.bar_beats = static_cast<int>(bar->as_int());
    }
    for (const char* member : {"streams", "playlist", "synchronized"}) {
        const json::Value* streams = doc.find(member);
        if (streams == nullptr) {
            continue;
        }
        for (const json::Value& entry : streams->as_array()) {
            StreamFile::Entry item;
            item.stream = entry.find("stream")->as_string();
            if (const json::Value* weight = entry.find("weight"); weight != nullptr) {
                item.weight = static_cast<float>(weight->as_double());
            }
            if (const json::Value* volume = entry.find("volume_db"); volume != nullptr) {
                item.volume_db = static_cast<float>(volume->as_double());
            }
            out.streams.push_back(std::move(item));
        }
    }
    out.playback_mode = parse_choice(doc.find("playback_mode"), kPlaybackModes, out.playback_mode);
    if (const json::Value* pitch = doc.find("random_pitch"); pitch != nullptr) {
        out.random_pitch = static_cast<float>(pitch->as_double());
    }
    if (const json::Value* offset = doc.find("random_volume_offset_db"); offset != nullptr) {
        out.random_volume_offset_db = static_cast<float>(offset->as_double());
    }
    if (const json::Value* shuffle = doc.find("shuffle"); shuffle != nullptr) {
        out.shuffle = shuffle->as_bool();
    }
    if (const json::Value* fade = doc.find("fade_time"); fade != nullptr) {
        out.fade_time = static_cast<float>(fade->as_double());
    }
    if (const json::Value* generator = doc.find("generator"); generator != nullptr) {
        out.mix_rate_mode =
            parse_choice(generator->find("mix_rate_mode"), kMixRateModes, out.mix_rate_mode);
        if (const json::Value* rate = generator->find("mix_rate"); rate != nullptr) {
            out.mix_rate = static_cast<float>(rate->as_double());
        }
        if (const json::Value* length = generator->find("buffer_length"); length != nullptr) {
            out.buffer_length = static_cast<float>(length->as_double());
        }
    }
    if (const json::Value* polyphonic = doc.find("polyphonic"); polyphonic != nullptr) {
        if (const json::Value* polyphony = polyphonic->find("polyphony"); polyphony != nullptr) {
            out.polyphony = static_cast<int>(polyphony->as_int());
        }
    }
    if (const json::Value* clips = doc.find("clips"); clips != nullptr) {
        const json::Array& list = clips->as_array();
        std::vector<json::ValidationError> unused;
        const auto index = [&](const json::Value& object, std::string_view key) {
            return optional_clip(object, key, list, "", unused).value_or(-1);
        };
        for (const json::Value& entry : list) {
            StreamFile::Clip clip;
            clip.name = entry.find("name")->as_string();
            clip.stream = entry.find("stream")->as_string();
            clip.auto_advance =
                parse_choice(entry.find("auto_advance"), kAutoAdvance, clip.auto_advance);
            clip.next_clip = index(entry, "next_clip");
            out.clips.push_back(std::move(clip));
        }
        out.initial_clip = std::max(index(doc, "initial_clip"), 0);
        if (const json::Value* transitions = doc.find("transitions"); transitions != nullptr) {
            for (const json::Value& entry : transitions->as_array()) {
                StreamFile::Transition transition;
                transition.from_clip = index(entry, "from");
                transition.to_clip = index(entry, "to");
                transition.from_time =
                    parse_choice(entry.find("from_time"), kTransitionFrom, transition.from_time);
                transition.to_time =
                    parse_choice(entry.find("to_time"), kTransitionTo, transition.to_time);
                transition.fade_mode =
                    parse_choice(entry.find("fade_mode"), kFadeModes, transition.fade_mode);
                if (const json::Value* beats = entry.find("fade_beats"); beats != nullptr) {
                    transition.fade_beats = static_cast<float>(beats->as_double());
                }
                transition.filler_clip = index(entry, "filler_clip");
                if (const json::Value* hold = entry.find("hold_previous"); hold != nullptr) {
                    transition.hold_previous = hold->as_bool();
                }
                out.transitions.push_back(transition);
            }
        }
    }
    return out;
}

} // namespace slash::audio
