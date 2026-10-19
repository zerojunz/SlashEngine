#include "slash/audio/audio.hpp"
#include "slash/core/text.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <miniaudio.h>
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

std::string encode_wav(std::span<const Frame> frames, int mix_rate, int bits) {
    const bool eight = bits == 8;
    const std::uint32_t frame_size = eight ? 2 : 4;
    const auto data_size = static_cast<std::uint32_t>(frames.size() * frame_size);
    std::string out;
    out.reserve(44 + data_size);
    out += "RIFF";
    put_u32(out, 36 + data_size);
    out += "WAVEfmt ";
    put_u32(out, 16);
    put_u16(out, 1); // PCM
    put_u16(out, 2); // stereo
    put_u32(out, static_cast<std::uint32_t>(mix_rate));
    put_u32(out, static_cast<std::uint32_t>(mix_rate) * frame_size);
    put_u16(out, static_cast<std::uint16_t>(frame_size));
    put_u16(out, eight ? 8 : 16);
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
        "A sound with Godot's import settings (source), or Godot's AudioStreamRandomizer "
        "(streams): one of several sounds each time it plays.";
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
    properties["loop"]["default"] = false;
    properties["loop"]["description"] = ".ogg and .mp3: play again from loop_offset at the end.";
    properties["loop_offset"] =
        number(".ogg and .mp3: seconds into the sound a loop goes back to.", 0, 86400, 0);
    properties["bpm"] = number(".ogg and .mp3: beats per minute; with beat_count, loops after "
                               "that many beats instead of at the end.",
                               0, 400, 0);
    json::Value& beats = properties["beat_count"];
    beats["type"] = "integer";
    beats["minimum"] = 0;
    beats["maximum"] = 512;
    beats["default"] = 0;
    beats["description"] = ".ogg and .mp3: beats before the loop, at bpm.";
    json::Value& streams = properties["streams"];
    streams["type"] = "array";
    streams["minItems"] = 1;
    streams["maxItems"] = 64;
    streams["description"] = "A randomizer's streams, each a sound or another .audio.json file.";
    json::Value& entry = streams["items"];
    entry["type"] = "object";
    entry["additionalProperties"] = false;
    entry["required"] = json::Array{"stream"};
    entry["properties"]["stream"]["type"] = "string";
    entry["properties"]["stream"]["minLength"] = 1;
    entry["properties"]["stream"]["description"] = "A .wav, .ogg, .mp3 or .audio.json file.";
    entry["properties"]["weight"] =
        number("How likely it is picked, against the others.", 0, 1000, 1);
    properties["playback_mode"] =
        choice("How a randomizer picks: at random but never the same one twice in a row, at "
               "random, or each in turn.",
               kPlaybackModes, "random_no_repeats");
    properties["random_pitch"] = number("A randomizer's pitch scale varies up to this many "
                                        "times higher or lower; 1 for none.",
                                        1, 16, 1);
    properties["random_volume_offset_db"] =
        number("A randomizer's volume varies up to this many dB louder or quieter.", 0, 40, 0);
    return schema;
}

std::vector<json::ValidationError> validate_stream_file(const json::Value& doc) {
    std::vector<json::ValidationError> errors = json::validate(doc, stream_file_schema());
    if (!errors.empty()) {
        return errors;
    }
    const json::Value* source = doc.find("source");
    const json::Value* streams = doc.find("streams");
    if ((source == nullptr) == (streams == nullptr)) {
        errors.push_back({"", source == nullptr
                                  ? "needs \"source\" (a sound with settings) or \"streams\" (a "
                                    "randomizer)"
                                  : "has both \"source\" and \"streams\"; a file is one sound "
                                    "or a randomizer"});
        return errors;
    }
    static constexpr std::array kWavKeys{"loop_mode", "loop_begin", "loop_end"};
    static constexpr std::array kCompressedKeys{"loop", "loop_offset", "bpm", "beat_count"};
    static constexpr std::array kRandomizerKeys{"playback_mode", "random_pitch",
                                                "random_volume_offset_db"};
    const auto refuse = [&](const auto& keys, std::string_view why) {
        for (const char* key : keys) {
            if (doc.find(key) != nullptr) {
                errors.push_back({"/" + std::string(key), std::string(why)});
            }
        }
    };
    if (source != nullptr) {
        const std::string& path = source->as_string();
        refuse(kRandomizerKeys, "only a randomizer (\"streams\") takes this");
        if (!is_project_path(path)) {
            errors.push_back({"/source", std::string(kPathMessage)});
        } else if (!is_sound_path(path)) {
            errors.push_back({"/source", "expected a .wav, .ogg or .mp3 file"});
        } else if (lower_extension(path) == ".wav") {
            refuse(kCompressedKeys, "a .wav source loops with loop_mode, loop_begin and loop_end");
            const json::Value* begin = doc.find("loop_begin");
            const json::Value* end = doc.find("loop_end");
            if (begin != nullptr && end != nullptr && end->as_int() >= 0 &&
                end->as_int() <= begin->as_int()) {
                errors.push_back({"/loop_end", "must come after loop_begin"});
            }
        } else {
            refuse(kWavKeys, "an .ogg or .mp3 source loops with loop and loop_offset");
        }
        return errors;
    }
    refuse(kWavKeys, "only a sound (\"source\") takes this");
    refuse(kCompressedKeys, "only a sound (\"source\") takes this");
    const json::Array& list = streams->as_array();
    for (std::size_t i = 0; i < list.size(); ++i) {
        const std::string& path = list[i].find("stream")->as_string();
        const std::string at = "/streams/" + std::to_string(i) + "/stream";
        if (!is_project_path(path)) {
            errors.push_back({at, std::string(kPathMessage)});
        } else if (!is_sound_path(path) && !path.ends_with(kStreamSuffix)) {
            errors.push_back({at, "expected a .wav, .ogg, .mp3 or .audio.json file"});
        }
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
    if (const json::Value* streams = doc.find("streams"); streams != nullptr) {
        for (const json::Value& entry : streams->as_array()) {
            StreamFile::Entry item;
            item.stream = entry.find("stream")->as_string();
            if (const json::Value* weight = entry.find("weight"); weight != nullptr) {
                item.weight = static_cast<float>(weight->as_double());
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
    return out;
}

} // namespace slash::audio
