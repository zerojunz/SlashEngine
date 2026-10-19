#pragma once

#include "slash/audio/audio.hpp"
#include "slash/core/json.hpp"
#include "slash/core/result.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

// Bus effects, as Godot's AudioEffect resources: each bus runs its effects in order on what was
// mixed into it, before its volume. In a .bus_layout.json file an effect is its type and its
// settings, by Godot's property names:
//
//   {"type": "reverb", "room_size": 0.8, "wet": 0.3}
//   {"type": "low_pass_filter", "cutoff_hz": 800, "enabled": false}
//
// effect_schema() lists every type with its settings and their defaults.
namespace slash::audio {

// What an effect may use while it runs: the mix rate, and other buses' sound for a compressor's
// sidechain.
class MixContext {
public:
    MixContext() = default;
    virtual ~MixContext() = default;
    MixContext(const MixContext&) = delete;
    MixContext& operator=(const MixContext&) = delete;
    MixContext(MixContext&&) = delete;
    MixContext& operator=(MixContext&&) = delete;

    [[nodiscard]] virtual int mix_rate() const = 0;
    // A bus's sound so far in this mix step (Master if there is no such bus), as Godot's
    // thread_get_channel_mix_buffer, which also marks it in use.
    [[nodiscard]] virtual Frame* bus_buffer(std::string_view bus) = 0;
};

// An effect running on a bus, as Godot's AudioEffectInstance.
class EffectInstance {
public:
    EffectInstance() = default;
    virtual ~EffectInstance() = default;
    EffectInstance(const EffectInstance&) = delete;
    EffectInstance& operator=(const EffectInstance&) = delete;
    EffectInstance(EffectInstance&&) = delete;
    EffectInstance& operator=(EffectInstance&&) = delete;

    virtual void process(const Frame* src, Frame* dst, int frames) = 0;
    // Whether it runs while its bus is inactive, as Godot's process_silence. A bus stays active
    // while it plays above -60 dB, so a reverb's tail still runs.
    [[nodiscard]] virtual bool process_silence() const { return false; }
};

// Godot's AudioEffectSpectrumAnalyzerInstance: each window of fft_size * 2 frames is analyzed
// as it fills, and the newest analysis answers.
class SpectrumAnalyzerInstance final : public EffectInstance {
public:
    enum class MagnitudeMode : std::uint8_t { Average, Max };

    SpectrumAnalyzerInstance(int fft_size, int mix_rate);

    void process(const Frame* src, Frame* dst, int frames) override;

    // How strong frequencies from from_hz to to_hz were in the newest analysis, left and right:
    // the average or the strongest of their bins, each normalized by fft_size, as Godot's
    // get_magnitude_for_frequency_range. A full-scale sine wave measures about 0.5.
    [[nodiscard]] Frame
    magnitude_for_frequency_range(float from_hz, float to_hz,
                                  MagnitudeMode mode = MagnitudeMode::Max) const;

private:
    int fft_size_;
    float mix_rate_;
    std::vector<float> temporal_fft_; // both channels' windows, as complex numbers
    int temporal_fft_pos_ = 0;
    std::vector<Frame> magnitudes_; // the newest analysis, one per bin
};

// The buffer of Godot's AudioEffectCapture: what passes through, kept in a ring of
// buffer_length seconds (rounded up to a power of two frames) for game code to take. Frames that
// don't fit are discarded until some are taken.
class CaptureInstance final : public EffectInstance {
public:
    CaptureInstance(float buffer_length_seconds, int mix_rate);

    void process(const Frame* src, Frame* dst, int frames) override;
    [[nodiscard]] bool process_silence() const override { return true; }

    [[nodiscard]] bool can_get_buffer(int frames) const;
    // Takes the oldest frames held; none unless that many are, as Godot's get_buffer.
    [[nodiscard]] std::vector<Frame> get_buffer(int frames);
    void clear_buffer();
    [[nodiscard]] int frames_available() const;
    [[nodiscard]] int buffer_length_frames() const;
    [[nodiscard]] std::int64_t discarded_frames() const noexcept { return discarded_; }
    [[nodiscard]] std::int64_t pushed_frames() const noexcept { return pushed_; }

private:
    [[nodiscard]] int space_left() const;

    std::vector<Frame> buffer_;
    int read_ = 0;
    int write_ = 0;
    std::int64_t discarded_ = 0;
    std::int64_t pushed_ = 0;
};

// Godot's AudioEffectRecord: while recording, keeps everything that passes through, silence
// included.
class RecordInstance final : public EffectInstance {
public:
    enum class Format : std::uint8_t { Bits8, Bits16 };

    RecordInstance(Format format, int mix_rate) : format_(format), mix_rate_(mix_rate) {}

    void process(const Frame* src, Frame* dst, int frames) override;
    [[nodiscard]] bool process_silence() const override { return true; }

    // Starts a new recording, dropping the last one, or stops, as Godot's set_recording_active.
    void set_recording_active(bool record);
    [[nodiscard]] bool is_recording_active() const noexcept { return recording_; }
    [[nodiscard]] Format format() const noexcept { return format_; }
    // What was recorded, as it passed through.
    [[nodiscard]] std::span<const Frame> frames() const noexcept { return frames_; }
    // The recording as a sound to play, at the format's 8 or 16 bits, as Godot's get_recording.
    [[nodiscard]] Sound recording() const;

private:
    Format format_;
    int mix_rate_;
    bool recording_ = false;
    std::vector<Frame> frames_;
};

// The effect types, as named in .bus_layout.json files.
[[nodiscard]] std::vector<std::string_view> effect_types();

// JSON Schema for an effect in a .bus_layout.json file: one of the types and its settings.
[[nodiscard]] json::Value effect_schema();

// A running effect for a slot validated against effect_schema(). context must outlive it.
[[nodiscard]] Result<std::unique_ptr<EffectInstance>> make_effect(const EffectSlot& slot,
                                                                  MixContext& context);

// The running effect in a bus's slot, as Godot's AudioServer::get_bus_effect_instance, or
// nullptr if that slot holds another type.
[[nodiscard]] SpectrumAnalyzerInstance* spectrum_analyzer(Server& server, int bus, int effect);
[[nodiscard]] CaptureInstance* capture_effect(Server& server, int bus, int effect);
[[nodiscard]] RecordInstance* record_effect(Server& server, int bus, int effect);

} // namespace slash::audio
