#pragma once

#include "slash/audio/audio.hpp"
#include "slash/core/json.hpp"
#include "slash/core/result.hpp"

#include <memory>
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

// The effect types, as named in .bus_layout.json files.
[[nodiscard]] std::vector<std::string_view> effect_types();

// JSON Schema for an effect in a .bus_layout.json file: one of the types and its settings.
[[nodiscard]] json::Value effect_schema();

// A running effect for a slot validated against effect_schema(). context must outlive it.
[[nodiscard]] Result<std::unique_ptr<EffectInstance>> make_effect(const EffectSlot& slot,
                                                                  MixContext& context);

} // namespace slash::audio
