// Sound out of the speakers through SDL's audio streams. The engine mixes each tick's frames and
// queues them here; SDL's device thread plays what is queued.

#include "slash/audio/audio.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <string>
#include <vector>

namespace slash::audio {

namespace {

constexpr int kBytesPerFrame = static_cast<int>(sizeof(Frame));

SDL_AudioStream* as_stream(void* stream) {
    return static_cast<SDL_AudioStream*>(stream);
}

// The channels SDL is given for a mix: SDL has no 3.1 layout (its four channels are front and
// rear), so 3.1 goes as 5.1 with the rear silent.
int sdl_channels(SpeakerMode speakers) {
    return speakers == SpeakerMode::Surround31 ? 6 : 2 * channel_count(speakers);
}

} // namespace

Result<std::unique_ptr<SdlOutput>> SdlOutput::open(int mix_rate, float output_latency_ms) {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        return make_error(ErrorCode::PlatformFailure,
                          std::string("SDL can't start audio: ") + SDL_GetError());
    }
    // As Godot's drivers, the device's channels pick the speakers to mix for.
    SDL_AudioSpec device{};
    const SpeakerMode speakers =
        SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &device, nullptr)
            ? speaker_mode_for_channels(device.channels)
            : SpeakerMode::Stereo;
    const SDL_AudioSpec spec{SDL_AUDIO_F32, sdl_channels(speakers), mix_rate};
    SDL_AudioStream* stream =
        SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (stream == nullptr) {
        std::string message = std::string("no audio output device: ") + SDL_GetError();
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return make_error(ErrorCode::PlatformFailure, std::move(message));
    }
    SDL_ResumeAudioStreamDevice(stream);
    std::unique_ptr<SdlOutput> output(new SdlOutput());
    output->stream_ = stream;
    output->device_speakers_ = speakers;
    output->stream_speakers_ = speakers;
    output->latency_frames_ =
        std::max(1, static_cast<int>(static_cast<double>(output_latency_ms) * mix_rate / 1000.0));
    return output;
}

SdlOutput::~SdlOutput() {
    if (stream_ != nullptr) {
        SDL_DestroyAudioStream(as_stream(stream_));
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
}

void SdlOutput::push(std::span<const Frame> frames, SpeakerMode speakers) {
    if (frames.empty()) {
        return;
    }
    SDL_AudioStream* stream = as_stream(stream_);
    if (speakers != stream_speakers_) {
        // SDL turns the new layout into the device's.
        SDL_AudioSpec spec{};
        if (!SDL_GetAudioStreamFormat(stream, &spec, nullptr)) {
            return;
        }
        spec.channels = sdl_channels(speakers);
        if (!SDL_SetAudioStreamFormat(stream, &spec, nullptr)) {
            return;
        }
        stream_speakers_ = speakers;
    }
    const auto pairs = static_cast<std::size_t>(channel_count(speakers));
    const auto count = static_cast<int>(frames.size() / pairs);
    if (speakers == SpeakerMode::Surround31) {
        // 3.1 as 5.1: a silent rear pair after each frame's front and center pairs.
        constexpr std::size_t k51 = 3;
        expanded_.assign(static_cast<std::size_t>(count) * k51, Frame{});
        for (std::size_t i = 0; i < static_cast<std::size_t>(count); ++i) {
            expanded_[i * k51] = frames[i * pairs];
            expanded_[(i * k51) + 1] = frames[(i * pairs) + 1];
        }
        frames = expanded_;
    }
    const int frame_bytes = sdl_channels(speakers) / 2 * kBytesPerFrame;
    const int queued = SDL_GetAudioStreamQueued(stream) / frame_bytes;
    if (queued == 0) {
        // Ran dry (the game started, or a tick was late): queue the latency as silence first, so
        // the next late tick doesn't run dry too.
        const std::vector<char> silence(static_cast<std::size_t>(latency_frames_ * frame_bytes));
        SDL_PutAudioStreamData(stream, silence.data(), latency_frames_ * frame_bytes);
    } else if (queued > latency_frames_ + 4 * count) {
        // Ticks ran ahead of the speakers (catching up after a stall); drop these frames rather
        // than letting the delay grow.
        return;
    }
    SDL_PutAudioStreamData(stream, frames.data(), count * frame_bytes);
}

Sink SdlOutput::sink() {
    return [this](std::span<const Frame> frames, SpeakerMode speakers) { push(frames, speakers); };
}

SdlInput::~SdlInput() {
    close();
}

Result<void> SdlInput::start(int mix_rate) {
    if (stream_ != nullptr) {
        return {};
    }
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        return make_error(ErrorCode::PlatformFailure,
                          std::string("SDL can't start audio: ") + SDL_GetError());
    }
    // SDL converts what the device records to stereo floats at the mix rate. On Android this
    // asks for the RECORD_AUDIO permission first.
    const SDL_AudioSpec spec{SDL_AUDIO_F32, 2, mix_rate};
    SDL_AudioStream* stream =
        SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_RECORDING, &spec, nullptr, nullptr);
    if (stream == nullptr) {
        std::string message = std::string("no microphone to record: ") + SDL_GetError();
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return make_error(ErrorCode::PlatformFailure, std::move(message));
    }
    if (const char* name = SDL_GetAudioDeviceName(SDL_GetAudioStreamDevice(stream));
        name != nullptr && *name != '\0') {
        name_ = name;
    }
    SDL_ResumeAudioStreamDevice(stream);
    stream_ = stream;
    latency_frames_ = std::max(1, mix_rate * 3 / 100); // 30 ms
    primed_ = false;
    return {};
}

void SdlInput::stop() {
    close();
}

void SdlInput::close() {
    if (stream_ != nullptr) {
        SDL_DestroyAudioStream(as_stream(stream_));
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        stream_ = nullptr;
    }
}

void SdlInput::read(std::span<Frame> frames) {
    std::ranges::fill(frames, Frame{});
    if (stream_ == nullptr || frames.empty()) {
        return;
    }
    SDL_AudioStream* stream = as_stream(stream_);
    const auto count = static_cast<int>(frames.size());
    int available = SDL_GetAudioStreamAvailable(stream) / kBytesPerFrame;
    if (available > latency_frames_ + 4 * count) {
        // Recording ran ahead of the ticks (after a stall); drop the oldest frames rather than
        // letting the delay grow.
        std::vector<Frame> dropped(static_cast<std::size_t>(available - latency_frames_ - count));
        SDL_GetAudioStreamData(stream, dropped.data(),
                               static_cast<int>(dropped.size()) * kBytesPerFrame);
        available = latency_frames_ + count;
    }
    if (!primed_ && available < latency_frames_ + count) {
        // Starting, or ran dry: wait for the latency to be recorded, so the next late tick
        // doesn't run dry too.
        return;
    }
    primed_ = available >= count;
    SDL_GetAudioStreamData(stream, frames.data(), std::min(count, available) * kBytesPerFrame);
}

} // namespace slash::audio
