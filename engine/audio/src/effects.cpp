// Bus effects, ported from Godot's servers/audio/effects. Each instance's process() follows the
// Godot effect of the same name, keeping its float and double arithmetic. Settings are fixed for
// an instance's life: a bus layout change makes new ones, as Godot's set_bus_layout does.

#include "slash/audio/effects.hpp"

#include "filter.hpp"

#include "slash/core/assert.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <numbers>
#include <span>
#include <string>

namespace slash::audio {

namespace {

constexpr double kTau = 2.0 * std::numbers::pi; // Math::TAU

Frame operator*(Frame a, float s) {
    return {a.left * s, a.right * s};
}
Frame operator*(Frame a, Frame b) {
    return {a.left * b.left, a.right * b.right};
}
Frame operator+(Frame a, Frame b) {
    return {a.left + b.left, a.right + b.right};
}
Frame operator-(Frame a, Frame b) {
    return {a.left - b.left, a.right - b.right};
}
Frame& operator+=(Frame& a, Frame b) {
    a = a + b;
    return a;
}

// Godot's undenormalize (servers/audio/audio_frame.h), Tim Blechmann's version.
float undenormalize(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return (bits & 0x7f800000U) < 0x08000000U ? 0.0F : value;
}

// A ring buffer's size for a length in milliseconds: the next power of two above it.
std::uint32_t ring_size(double milliseconds, int mix_rate) {
    auto size = static_cast<int>(static_cast<float>(milliseconds / 1000.0 * mix_rate));
    int bits = 0;
    while (size > 0) {
        ++bits;
        size /= 2;
    }
    return 1U << static_cast<unsigned>(bits);
}

// ---------------------------------------------------------------------------------------------
// Settings

enum class Kind : std::uint8_t { Number, Integer, Boolean };

// A setting, by Godot's property name, with Godot's editor range and default.
struct Setting {
    std::string_view name;
    Kind kind;
    double minimum;
    double maximum;
    double fallback;
    std::string_view doc;
};

constexpr Setting number(std::string_view name, double minimum, double maximum, double fallback,
                         std::string_view doc) {
    return {name, Kind::Number, minimum, maximum, fallback, doc};
}

constexpr Setting boolean(std::string_view name, bool fallback, std::string_view doc) {
    return {name, Kind::Boolean, 0, 1, fallback ? 1.0 : 0.0, doc};
}

// A slot's settings, or their defaults.
class Params {
public:
    Params(const json::Value& values, std::span<const Setting> settings)
        : values_(values), settings_(settings) {}

    [[nodiscard]] float operator()(std::string_view name) const {
        if (const json::Value* value = values_.is_object() ? values_.find(name) : nullptr;
            value != nullptr) {
            if (value->is_bool()) {
                return value->as_bool() ? 1.0F : 0.0F;
            }
            return static_cast<float>(value->as_double());
        }
        const auto it = std::ranges::find(settings_, name, &Setting::name);
        SLASH_ASSERT(it != settings_.end(), "an audio effect read a setting it doesn't have");
        return static_cast<float>(it->fallback);
    }

    [[nodiscard]] bool flag(std::string_view name) const { return (*this)(name) != 0.0F; }

    [[nodiscard]] const json::Value* find(std::string_view name) const {
        return values_.is_object() ? values_.find(name) : nullptr;
    }

private:
    const json::Value& values_;
    std::span<const Setting> settings_;
};

// ---------------------------------------------------------------------------------------------
// Amplify (audio_effect_amplify.cpp)

constexpr std::array kAmplify{
    number("volume_db", -80, 24, 0, "Gain in dB."),
};

class Amplify final : public EffectInstance {
public:
    explicit Amplify(const Params& params) : volume_(db_to_linear(params("volume_db"))) {}

    void process(const Frame* src, Frame* dst, int frames) override {
        for (int i = 0; i < frames; ++i) {
            dst[i] = src[i] * volume_;
        }
    }

private:
    float volume_;
};

// ---------------------------------------------------------------------------------------------
// Filters (audio_effect_filter.cpp)

constexpr std::string_view kCutoffDoc = "Frequency in Hz the filter acts at.";
constexpr std::string_view kResonanceDoc = "How much frequencies near the cutoff are boosted.";
constexpr std::string_view kDbDoc =
    "Steepness: 6, 12, 18 or 24 dB per octave, as one to four filter stages.";

constexpr std::array kFilter{
    number("cutoff_hz", 1, 20500, 2000, kCutoffDoc),
    number("resonance", 0, 1, 0.5, kResonanceDoc),
    Setting{"db", Kind::Integer, 6, 24, 6, kDbDoc},
};

constexpr std::array kShelfFilter{
    number("cutoff_hz", 1, 20500, 2000, kCutoffDoc),
    number("resonance", 0, 1, 0.5, kResonanceDoc),
    number("gain", 0, 4, 1, "Gain of the frequencies beyond the cutoff."),
    Setting{"db", Kind::Integer, 6, 24, 6, kDbDoc},
};

class FilterEffect final : public EffectInstance {
public:
    FilterEffect(Filter::Mode mode, const Params& params, int mix_rate) {
        const bool shelf = mode == Filter::Mode::LowShelf || mode == Filter::Mode::HighShelf;
        filter_.set_cutoff(std::max(params("cutoff_hz"), 1.0F));
        filter_.set_gain(shelf ? params("gain") : 1.0F);
        filter_.set_resonance(params("resonance"));
        filter_.set_mode(mode);
        stages_ = std::clamp(static_cast<int>(params("db")) / 6, 1, 4);
        filter_.set_stages(stages_);
        filter_.set_sampling_rate(static_cast<float>(mix_rate));
        for (auto& channel : processors_) {
            for (Filter::Processor& processor : channel) {
                processor.set_filter(&filter_);
                processor.update_coeffs();
            }
        }
    }

    void process(const Frame* src, Frame* dst, int frames) override {
        for (int i = 0; i < frames; ++i) {
            float f = src[i].left;
            for (int s = 0; s < stages_; ++s) {
                processors_[0][static_cast<std::size_t>(s)].process_one(f);
            }
            dst[i].left = f;
        }
        for (int i = 0; i < frames; ++i) {
            float f = src[i].right;
            for (int s = 0; s < stages_; ++s) {
                processors_[1][static_cast<std::size_t>(s)].process_one(f);
            }
            dst[i].right = f;
        }
    }

private:
    Filter filter_;
    int stages_ = 1;
    std::array<std::array<Filter::Processor, 4>, 2> processors_;
};

// ---------------------------------------------------------------------------------------------
// Equalizers (audio_effect_eq.cpp, eq_filter.cpp)

constexpr std::array<double, 6> kBands6{32, 100, 320, 1e3, 3200, 10e3};
constexpr std::array<double, 10> kBands10{31.25, 62.5, 125, 250, 500, 1e3, 2e3, 4e3, 8e3, 16e3};
constexpr std::array<double, 21> kBands21{22,   32,  44,   63,  90,   125,  175,
                                          250,  350, 500,  700, 1e3,  1400, 2e3,
                                          2800, 4e3, 5600, 8e3, 11e3, 16e3, 22e3};

// Godot's property name for a band: band_db/<whole Hz>_hz.
std::string band_name(double frequency) {
    return std::to_string(static_cast<std::int64_t>(static_cast<float>(frequency))) + "_hz";
}

// Godot's solve_quadratic: the number of roots.
int solve_quadratic(double a, double b, double c, double* r1, double* r2) {
    const double base = 2 * a;
    if (base == 0.0) {
        return 0;
    }
    double squared = b * b - 4 * a * c;
    if (squared < 0.0) {
        return 0;
    }
    squared = std::sqrt(squared);
    *r1 = (-b + squared) / base;
    *r2 = (-b - squared) / base;
    return *r1 == *r2 ? 1 : 2;
}

class Equalizer final : public EffectInstance {
public:
    Equalizer(std::span<const double> frequencies, const Params& params, int mix_rate) {
        bands_.resize(frequencies.size());
        for (std::size_t i = 0; i < frequencies.size(); ++i) {
            bands_[i].freq = static_cast<float>(frequencies[i]);
        }
        recalculate_band_coefficients(static_cast<float>(mix_rate));
        const json::Value* gains = params.find("band_db");
        for (std::size_t i = 0; i < bands_.size(); ++i) {
            float db = 0.0F;
            if (gains != nullptr) {
                if (const json::Value* value = gains->find(band_name(frequencies[i]));
                    value != nullptr) {
                    db = static_cast<float>(value->as_double());
                }
            }
            bands_[i].gain = db_to_linear(db);
        }
    }

    void process(const Frame* src, Frame* dst, int frames) override {
        for (int i = 0; i < frames; ++i) {
            const Frame in = src[i];
            Frame out{};
            for (Band& band : bands_) {
                float l = in.left;
                float r = in.right;
                band.process_one(band.history[0], l);
                band.process_one(band.history[1], r);
                out.left += l * band.gain;
                out.right += r * band.gain;
            }
            dst[i] = out;
        }
    }

private:
    struct History {
        float a1 = 0, a2 = 0, a3 = 0;
        float b1 = 0, b2 = 0, b3 = 0;
    };

    struct Band {
        float freq = 0;
        float c1 = 0, c2 = 0, c3 = 0;
        float gain = 1;
        std::array<History, 2> history;

        void process_one(History& h, float& data) const {
            h.a1 = data;
            h.b1 = c1 * (h.a1 - h.a3) + c3 * h.b2 - c2 * h.b3;
            data = h.b1;
            h.a3 = h.a2;
            h.a2 = h.a1;
            h.b3 = h.b2;
            h.b2 = h.b1;
        }
    };

    // EQ::recalculate_band_coefficients.
    void recalculate_band_coefficients(float mix_rate) {
        const auto band_log = [](double f) { return std::log(f) / std::numbers::ln2; };
        const auto pow2 = [](double v) { return v * v; };
        for (std::size_t i = 0; i < bands_.size(); ++i) {
            double octave_size = 0;
            const double frq = bands_[i].freq;
            if (i == 0) {
                octave_size = band_log(bands_[1].freq) - band_log(frq);
            } else if (i == bands_.size() - 1) {
                octave_size = band_log(frq) - band_log(bands_[i - 1].freq);
            } else {
                const double next = band_log(bands_[i + 1].freq) - band_log(frq);
                const double prev = band_log(frq) - band_log(bands_[i - 1].freq);
                octave_size = (next + prev) / 2.0;
            }
            const double frq_l = std::round(frq / std::pow(2.0, octave_size / 2.0));
            const double side_gain2 = pow2(std::numbers::sqrt2 / 2);
            const double th = kTau * frq / mix_rate;
            const double th_l = kTau * frq_l / mix_rate;
            const double c2a = side_gain2 * pow2(std::cos(th)) -
                               2.0 * side_gain2 * std::cos(th_l) * std::cos(th) + side_gain2 -
                               pow2(std::sin(th_l));
            const double c2b = 2.0 * side_gain2 * pow2(std::cos(th_l)) +
                               side_gain2 * pow2(std::cos(th)) -
                               2.0 * side_gain2 * std::cos(th_l) * std::cos(th) - side_gain2 +
                               pow2(std::sin(th_l));
            const double c2c = 0.25 * side_gain2 * pow2(std::cos(th)) -
                               0.5 * side_gain2 * std::cos(th_l) * std::cos(th) +
                               0.25 * side_gain2 - 0.25 * pow2(std::sin(th_l));
            double r1 = 0;
            double r2 = 0;
            if (solve_quadratic(c2a, c2b, c2c, &r1, &r2) == 0) {
                continue;
            }
            bands_[i].c1 = static_cast<float>(2.0 * ((0.5 - r1) / 2.0));
            bands_[i].c2 = static_cast<float>(2.0 * r1);
            bands_[i].c3 = static_cast<float>(2.0 * (0.5 + r1) * std::cos(th));
        }
    }

    std::vector<Band> bands_;
};

// ---------------------------------------------------------------------------------------------
// Delay (audio_effect_delay.cpp)

constexpr std::array kDelay{
    number("dry", 0, 1, 1, "How much of the sound passes through unchanged."),
    boolean("tap1_active", true, "Whether the first echo plays."),
    number("tap1_delay_ms", 0, 1500, 250, "Milliseconds before the first echo."),
    number("tap1_level_db", -60, 0, -6, "The first echo's volume in dB."),
    number("tap1_pan", -1, 1, 0.2, "The first echo's pan, from -1 (left) to 1 (right)."),
    boolean("tap2_active", true, "Whether the second echo plays."),
    number("tap2_delay_ms", 0, 1500, 500, "Milliseconds before the second echo."),
    number("tap2_level_db", -60, 0, -12, "The second echo's volume in dB."),
    number("tap2_pan", -1, 1, -0.4, "The second echo's pan, from -1 (left) to 1 (right)."),
    boolean("feedback_active", false, "Whether echoes repeat, fading."),
    number("feedback_delay_ms", 0, 1500, 340, "Milliseconds between repeats."),
    number("feedback_level_db", -60, 0, -6, "Each repeat's volume against the last, in dB."),
    number("feedback_lowpass", 1, 16000, 16000, "Repeats lose frequencies above this, in Hz."),
};

class Delay final : public EffectInstance {
public:
    Delay(const Params& params, int mix_rate) : mix_rate_(static_cast<float>(mix_rate)) {
        dry_ = params("dry");
        tap_1_active_ = params.flag("tap1_active");
        tap_1_delay_ms_ = params("tap1_delay_ms");
        tap_1_level_ = params("tap1_level_db");
        tap_1_pan_ = params("tap1_pan");
        tap_2_active_ = params.flag("tap2_active");
        tap_2_delay_ms_ = params("tap2_delay_ms");
        tap_2_level_ = params("tap2_level_db");
        tap_2_pan_ = params("tap2_pan");
        feedback_active_ = params.flag("feedback_active");
        feedback_delay_ms_ = params("feedback_delay_ms");
        feedback_level_ = params("feedback_level_db");
        feedback_lowpass_ = std::max(params("feedback_lowpass"), 1.0F);
        // MAX_DELAY_MS, with 100 ms of extra room.
        const std::uint32_t size = ring_size(3000 + 100, mix_rate);
        ring_buffer_mask_ = size - 1;
        ring_buffer_.resize(size);
        feedback_buffer_.resize(size);
    }

    void process(const Frame* src, Frame* dst, int frames) override {
        while (frames > 0) {
            const int to_mix = std::min(frames, 256);
            process_chunk(src, dst, to_mix);
            src += to_mix;
            dst += to_mix;
            frames -= to_mix;
        }
    }

private:
    void process_chunk(const Frame* src, Frame* dst, int frames) {
        const float main_level_f = dry_;
        const float tap_1_level_f = tap_1_active_ ? db_to_linear(tap_1_level_) : 0.0F;
        const int tap_1_delay_frames = static_cast<int>((tap_1_delay_ms_ / 1000.0) * mix_rate_);
        const float tap_2_level_f = tap_2_active_ ? db_to_linear(tap_2_level_) : 0.0F;
        const int tap_2_delay_frames = static_cast<int>((tap_2_delay_ms_ / 1000.0) * mix_rate_);
        const float feedback_level_f = feedback_active_ ? db_to_linear(feedback_level_) : 0.0F;
        const auto feedback_delay_frames =
            static_cast<std::uint32_t>(static_cast<int>((feedback_delay_ms_ / 1000.0) * mix_rate_));

        Frame tap1_vol{tap_1_level_f, tap_1_level_f};
        tap1_vol.left = static_cast<float>(tap1_vol.left * std::clamp(1.0 - tap_1_pan_, 0.0, 1.0));
        tap1_vol.right =
            static_cast<float>(tap1_vol.right * std::clamp(1.0 + tap_1_pan_, 0.0, 1.0));
        Frame tap2_vol{tap_2_level_f, tap_2_level_f};
        tap2_vol.left = static_cast<float>(tap2_vol.left * std::clamp(1.0 - tap_2_pan_, 0.0, 1.0));
        tap2_vol.right =
            static_cast<float>(tap2_vol.right * std::clamp(1.0 + tap_2_pan_, 0.0, 1.0));

        // The feedback's low pass.
        const auto lpf_c = static_cast<float>(std::exp(-kTau * feedback_lowpass_ / mix_rate_));
        const float lpf_ic = 1.0F - lpf_c;

        for (int i = 0; i < frames; ++i) {
            ring_buffer_[ring_buffer_pos_ & ring_buffer_mask_] = src[i];
            const Frame main_val = src[i] * main_level_f;
            const Frame tap_1_val =
                ring_buffer_[(ring_buffer_pos_ - static_cast<std::uint32_t>(tap_1_delay_frames)) &
                             ring_buffer_mask_] *
                tap1_vol;
            const Frame tap_2_val =
                ring_buffer_[(ring_buffer_pos_ - static_cast<std::uint32_t>(tap_2_delay_frames)) &
                             ring_buffer_mask_] *
                tap2_vol;
            Frame out = main_val + tap_1_val + tap_2_val;
            out += feedback_buffer_[feedback_buffer_pos_];
            // The low pass and feedback gain.
            Frame fb_in = out * feedback_level_f * lpf_ic + h_ * lpf_c;
            fb_in = {undenormalize(fb_in.left), undenormalize(fb_in.right)};
            h_ = fb_in;
            feedback_buffer_[feedback_buffer_pos_] = fb_in;
            dst[i] = out;
            ++ring_buffer_pos_;
            if (++feedback_buffer_pos_ >= feedback_delay_frames) {
                feedback_buffer_pos_ = 0;
            }
        }
    }

    float mix_rate_;
    float dry_ = 1;
    bool tap_1_active_ = true;
    float tap_1_delay_ms_ = 0;
    float tap_1_level_ = 0;
    float tap_1_pan_ = 0;
    bool tap_2_active_ = true;
    float tap_2_delay_ms_ = 0;
    float tap_2_level_ = 0;
    float tap_2_pan_ = 0;
    bool feedback_active_ = false;
    float feedback_delay_ms_ = 0;
    float feedback_level_ = 0;
    float feedback_lowpass_ = 0;
    std::vector<Frame> ring_buffer_;
    std::vector<Frame> feedback_buffer_;
    std::uint32_t ring_buffer_pos_ = 0;
    std::uint32_t ring_buffer_mask_ = 0;
    std::uint32_t feedback_buffer_pos_ = 0;
    Frame h_{};
};

// ---------------------------------------------------------------------------------------------
// Reverb (audio_effect_reverb.cpp, reverb_filter.cpp: Freeverb)

constexpr std::array kReverb{
    number("predelay_msec", 20, 500, 150, "Milliseconds before the first reflection."),
    number("predelay_feedback", 0, 0.98, 0.4, "How much the first reflection repeats."),
    number("room_size", 0, 1, 0.8, "The room's size: how long the reverb lasts."),
    number("damping", 0, 1, 0.5, "How much the walls absorb high frequencies."),
    number("spread", 0, 1, 1, "How far the left and right channels differ."),
    number("hipass", 0, 1, 0, "Cuts low frequencies from the reverb; 0 for none."),
    number("dry", 0, 1, 1, "How much of the sound passes through unchanged."),
    number("wet", 0, 1, 0.5, "How loud the reverb is."),
};

class Reverb {
public:
    static constexpr int kInputBufferMaxSize = 1024;

    Reverb(float mix_rate, float extra_spread_base) {
        params_.mix_rate = mix_rate;
        params_.extra_spread_base = extra_spread_base;
        input_buffer_.resize(kInputBufferMaxSize);
        configure_buffers();
        update_parameters();
    }

    void set_room_size(float size) {
        params_.room_size = size;
        update_parameters();
    }
    void set_damp(float damp) {
        params_.damp = damp;
        update_parameters();
    }
    void set_wet(float wet) { params_.wet = wet; }
    void set_dry(float dry) { params_.dry = dry; }
    void set_predelay(float predelay) { params_.predelay = predelay; }
    void set_predelay_feedback(float feedback) { params_.predelay_fb = feedback; }
    void set_highpass(float frq) { params_.hpf = std::clamp(frq, 0.0F, 1.0F); }
    void set_extra_spread(float spread) { params_.extra_spread = spread; }

    void process(const float* src, float* dst, int frames) {
        frames = std::min(frames, kInputBufferMaxSize);
        int predelay_frames =
            static_cast<int>(std::rint((params_.predelay / 1000.0) * params_.mix_rate));
        predelay_frames = std::max(predelay_frames, 10);
        if (predelay_frames >= echo_buffer_size_) {
            predelay_frames = echo_buffer_size_ - 1;
        }
        for (int i = 0; i < frames; ++i) {
            if (echo_buffer_pos_ >= echo_buffer_size_) {
                echo_buffer_pos_ = 0;
            }
            int read_pos = echo_buffer_pos_ - predelay_frames;
            while (read_pos < 0) {
                read_pos += echo_buffer_size_;
            }
            const float in = undenormalize(
                echo_buffer_[static_cast<std::size_t>(read_pos)] * params_.predelay_fb + src[i]);
            echo_buffer_[static_cast<std::size_t>(echo_buffer_pos_)] = in;
            input_buffer_[static_cast<std::size_t>(i)] = in;
            dst[i] = 0;
            ++echo_buffer_pos_;
        }

        if (params_.hpf > 0) {
            const auto hpaux =
                static_cast<float>(std::exp(-kTau * params_.hpf * 6000 / params_.mix_rate));
            const auto hp_a1 = static_cast<float>((1.0 + hpaux) / 2.0);
            const auto hp_a2 = static_cast<float>(-(1.0 + hpaux) / 2.0);
            const float hp_b1 = hpaux;
            for (int i = 0; i < frames; ++i) {
                float& value = input_buffer_[static_cast<std::size_t>(i)];
                const float in = value;
                value = in * hp_a1 + hpf_h1_ * hp_a2 + hpf_h2_ * hp_b1;
                hpf_h2_ = value;
                hpf_h1_ = in;
            }
        }

        for (Comb& c : comb_) {
            const int size_limit =
                c.size - static_cast<int>(std::rint(static_cast<float>(c.extra_spread_frames) *
                                                    (1.0 - params_.extra_spread)));
            for (int j = 0; j < frames; ++j) {
                if (c.pos >= size_limit) {
                    c.pos = 0;
                }
                float& slot = c.buffer[static_cast<std::size_t>(c.pos)];
                float out = undenormalize(slot * c.feedback);
                out = static_cast<float>(out * (1.0 - c.damp) + c.damp_h * c.damp); // low pass
                c.damp_h = out;
                slot = input_buffer_[static_cast<std::size_t>(j)] + out;
                dst[j] += out;
                ++c.pos;
            }
        }

        constexpr float kAllpassFeedback = 0.7F;
        for (AllPass& a : allpass_) {
            const int size_limit =
                a.size - static_cast<int>(std::rint(static_cast<float>(a.extra_spread_frames) *
                                                    (1.0 - params_.extra_spread)));
            for (int j = 0; j < frames; ++j) {
                if (a.pos >= size_limit) {
                    a.pos = 0;
                }
                float& slot = a.buffer[static_cast<std::size_t>(a.pos)];
                const float aux = slot;
                slot = undenormalize(kAllpassFeedback * aux + dst[j]);
                dst[j] = aux - kAllpassFeedback * slot;
                ++a.pos;
            }
        }

        constexpr float kWetScale = 0.6F;
        for (int i = 0; i < frames; ++i) {
            dst[i] = dst[i] * params_.wet * kWetScale + src[i] * params_.dry;
        }
    }

private:
    // Freeverb's tunings, in seconds.
    static constexpr std::array<float, 8> kCombTunings{
        0.025306122448979593F, 0.026938775510204082F, 0.028956916099773241F, 0.03074829931972789F,
        0.032244897959183672F, 0.03380952380952381F,  0.035306122448979592F, 0.036666666666666667F};
    static constexpr std::array<float, 4> kAllpassTunings{
        0.0051020408163265302F, 0.007732426303854875F, 0.01F, 0.012607709750566893F};
    static constexpr int kMaxEchoMs = 500;

    struct Comb {
        int size = 0;
        std::vector<float> buffer;
        float feedback = 0;
        float damp = 0;   // low pass
        float damp_h = 0; // history
        int pos = 0;
        int extra_spread_frames = 0;
    };

    struct AllPass {
        int size = 0;
        std::vector<float> buffer;
        int pos = 0;
        int extra_spread_frames = 0;
    };

    struct Parameters {
        float room_size = 0.8F;
        float damp = 0.5F;
        float wet = 0.0F;
        float dry = 1.0F;
        float mix_rate = 44100;
        float extra_spread_base = 0;
        float extra_spread = 1.0F;
        float predelay = 150;
        float predelay_fb = 0.4F;
        float hpf = 0;
    };

    void configure_buffers() {
        const auto length = [&](float tuning, int extra) {
            const int len = static_cast<int>(std::rint(tuning * params_.mix_rate)) + extra;
            return std::max(len, 5);
        };
        for (std::size_t i = 0; i < comb_.size(); ++i) {
            Comb& c = comb_[i];
            c.extra_spread_frames =
                static_cast<int>(std::rint(params_.extra_spread_base * params_.mix_rate));
            c.size = length(kCombTunings[i], c.extra_spread_frames);
            c.buffer.assign(static_cast<std::size_t>(c.size), 0.0F);
            c.pos = 0;
        }
        for (std::size_t i = 0; i < allpass_.size(); ++i) {
            AllPass& a = allpass_[i];
            a.extra_spread_frames =
                static_cast<int>(std::rint(params_.extra_spread_base * params_.mix_rate));
            a.size = length(kAllpassTunings[i], a.extra_spread_frames);
            a.buffer.assign(static_cast<std::size_t>(a.size), 0.0F);
            a.pos = 0;
        }
        echo_buffer_size_ =
            static_cast<int>(((static_cast<float>(kMaxEchoMs) / 1000.0) * params_.mix_rate + 1.0));
        echo_buffer_.assign(static_cast<std::size_t>(echo_buffer_size_), 0.0F);
        echo_buffer_pos_ = 0;
    }

    void update_parameters() {
        // More Freeverb constants.
        constexpr float kRoomScale = 0.28F;
        constexpr float kRoomOffset = 0.7F;
        for (Comb& c : comb_) {
            c.feedback = std::clamp(kRoomOffset + params_.room_size * kRoomScale, kRoomOffset,
                                    kRoomOffset + kRoomScale);
            auto auxdmp = static_cast<float>(params_.damp / 2.0 + 0.5); // 0.5 .. 1.0 is enough
            auxdmp *= auxdmp;
            c.damp = static_cast<float>(std::exp(-kTau * auxdmp * 10000 / params_.mix_rate));
        }
    }

    std::array<Comb, 8> comb_;
    std::array<AllPass, 4> allpass_;
    std::vector<float> input_buffer_;
    std::vector<float> echo_buffer_;
    int echo_buffer_size_ = 0;
    int echo_buffer_pos_ = 0;
    float hpf_h1_ = 0.0F;
    float hpf_h2_ = 0.0F;
    Parameters params_;
};

class ReverbEffect final : public EffectInstance {
public:
    ReverbEffect(const Params& params, int mix_rate)
        : reverb_{Reverb(static_cast<float>(mix_rate), 0.0F),
                  Reverb(static_cast<float>(mix_rate), 0.000521F)} { // for a stereo effect
        for (Reverb& r : reverb_) {
            r.set_predelay(params("predelay_msec"));
            r.set_predelay_feedback(std::clamp(params("predelay_feedback"), 0.0F, 0.98F));
            r.set_highpass(params("hipass"));
            r.set_room_size(params("room_size"));
            r.set_damp(params("damping"));
            r.set_extra_spread(params("spread"));
            r.set_wet(params("wet"));
            r.set_dry(params("dry"));
        }
    }

    void process(const Frame* src, Frame* dst, int frames) override {
        int offset = 0;
        while (frames > 0) {
            const int to_mix = std::min(frames, Reverb::kInputBufferMaxSize);
            for (int j = 0; j < to_mix; ++j) {
                tmp_src_[static_cast<std::size_t>(j)] = src[offset + j].left;
            }
            reverb_[0].process(tmp_src_.data(), tmp_dst_.data(), to_mix);
            for (int j = 0; j < to_mix; ++j) {
                dst[offset + j].left = tmp_dst_[static_cast<std::size_t>(j)];
                tmp_src_[static_cast<std::size_t>(j)] = src[offset + j].right;
            }
            reverb_[1].process(tmp_src_.data(), tmp_dst_.data(), to_mix);
            for (int j = 0; j < to_mix; ++j) {
                dst[offset + j].right = tmp_dst_[static_cast<std::size_t>(j)];
            }
            offset += to_mix;
            frames -= to_mix;
        }
    }

private:
    std::array<Reverb, 2> reverb_;
    std::vector<float> tmp_src_ = std::vector<float>(Reverb::kInputBufferMaxSize);
    std::vector<float> tmp_dst_ = std::vector<float>(Reverb::kInputBufferMaxSize);
};

// ---------------------------------------------------------------------------------------------
// Chorus (audio_effect_chorus.cpp)

constexpr std::array kChorus{
    Setting{"voice_count", Kind::Integer, 1, 4, 2, "How many of the voices play."},
    number("dry", 0, 1, 1, "How much of the sound passes through unchanged."),
    number("wet", 0, 1, 0.5, "How loud the voices are."),
};

// A voice's settings, as Godot's voice/<n>/...
constexpr std::array kChorusVoice{
    number("delay_ms", 0, 50, 12, "Milliseconds the voice lags the sound."),
    number("rate_hz", 0.1, 20, 1, "How fast the voice's lag wavers."),
    number("depth_ms", 0, 20, 0, "How far the voice's lag wavers, in milliseconds."),
    number("level_db", -60, 24, 0, "The voice's volume in dB."),
    number("cutoff_hz", 1, 20500, 16000, "The voice loses frequencies above this, in Hz."),
    number("pan", -1, 1, 0, "The voice's pan, from -1 (left) to 1 (right)."),
};

class Chorus final : public EffectInstance {
public:
    static constexpr int kCyclesFrac = 16;
    static constexpr std::uint64_t kCyclesMask = (1U << kCyclesFrac) - 1;
    static constexpr float kCutoffMax = 16000; // MS_CUTOFF_MAX

    Chorus(const Params& params, int mix_rate) : mix_rate_(static_cast<float>(mix_rate)) {
        voice_count_ = std::clamp(static_cast<int>(params("voice_count")), 1, 4);
        dry_ = params("dry");
        wet_ = params("wet");
        // Godot's defaults: two voices set up, two left plain.
        voices_[0] = {15, 0.8F, 2, 0, 8000, -0.5F};
        voices_[1] = {20, 1.2F, 3, 0, 8000, 0.5F};
        if (const json::Value* list = params.find("voices"); list != nullptr) {
            const json::Array& items = list->as_array();
            for (std::size_t i = 0; i < items.size() && i < voices_.size(); ++i) {
                Voice& v = voices_[i];
                const auto read = [&](std::string_view name, float& out) {
                    if (const json::Value* value = items[i].find(name); value != nullptr) {
                        out = static_cast<float>(value->as_double());
                    }
                };
                read("delay_ms", v.delay);
                read("rate_hz", v.rate);
                read("depth_ms", v.depth);
                read("level_db", v.level);
                read("cutoff_hz", v.cutoff);
                read("pan", v.pan);
                v.cutoff = std::max(v.cutoff, 1.0F);
            }
        }
        // MAX_DELAY_MS + MAX_DEPTH_MS + MAX_WIDTH_MS, doubled to avoid complications.
        const std::uint32_t size = ring_size((50 + 20 + 50) * 2, mix_rate);
        buffer_mask_ = size - 1;
        audio_buffer_.resize(size);
    }

    void process(const Frame* src, Frame* dst, int frames) override {
        while (frames > 0) {
            const int to_mix = std::min(frames, 256);
            process_chunk(src, dst, to_mix);
            src += to_mix;
            dst += to_mix;
            frames -= to_mix;
        }
    }

private:
    struct Voice {
        float delay = 12.0F;
        float rate = 1;
        float depth = 0;
        float level = 0;
        float cutoff = kCutoffMax;
        float pan = 0;
    };

    void process_chunk(const Frame* src, Frame* dst, int frames) {
        for (int i = 0; i < frames; ++i) {
            audio_buffer_[(buffer_pos_ + static_cast<std::uint32_t>(i)) & buffer_mask_] = src[i];
            dst[i] = src[i] * dry_;
        }
        for (std::size_t vc = 0; vc < static_cast<std::size_t>(voice_count_); ++vc) {
            const Voice& v = voices_[vc];
            const double time_to_mix = static_cast<float>(frames) / mix_rate_;
            const double cycles_to_mix = time_to_mix * v.rate;
            std::uint32_t local_rb_pos = buffer_pos_;
            const double delay_msec = v.delay;
            auto delay_frames = static_cast<std::uint32_t>(
                static_cast<int>(std::rint(static_cast<float>((delay_msec / 1000.0) * mix_rate_))));
            const auto max_depth_frames = static_cast<float>((v.depth / 1000.0) * mix_rate_);
            std::uint64_t local_cycles = cycles_[vc];
            const auto increment =
                static_cast<std::uint64_t>(std::rint(cycles_to_mix / static_cast<double>(frames) *
                                                     static_cast<double>(1U << kCyclesFrac)));
            // The LFO must not read ahead of where it writes; 10 frames spare for precision.
            if (static_cast<std::uint32_t>(max_depth_frames) + 10 > delay_frames) {
                delay_frames = static_cast<std::uint32_t>(static_cast<int>(max_depth_frames)) + 10;
            }
            // Low pass.
            if (v.cutoff == 0) {
                continue;
            }
            const auto auxlp = static_cast<float>(std::exp(-kTau * v.cutoff / mix_rate_));
            float c1 = 1.0F - auxlp;
            float c2 = auxlp;
            Frame h = filter_h_[vc];
            if (v.cutoff >= kCutoffMax) {
                c1 = 1.0F;
                c2 = 0.0F;
            }
            Frame vol_modifier = Frame{wet_, wet_} * db_to_linear(v.level);
            vol_modifier.left =
                static_cast<float>(vol_modifier.left * std::clamp(1.0 - v.pan, 0.0, 1.0));
            vol_modifier.right =
                static_cast<float>(vol_modifier.right * std::clamp(1.0 + v.pan, 0.0, 1.0));

            for (int i = 0; i < frames; ++i) {
                const float phase = static_cast<float>(local_cycles & kCyclesMask) /
                                    static_cast<float>(1U << kCyclesFrac);
                const auto wave_delay =
                    static_cast<float>(std::sin(phase * kTau) * max_depth_frames);
                const auto wave_delay_frames = static_cast<int>(std::rint(std::floor(wave_delay)));
                const float wave_delay_frac = wave_delay - static_cast<float>(wave_delay_frames);
                std::uint32_t rb_source = local_rb_pos;
                rb_source -= delay_frames;
                rb_source -= static_cast<std::uint32_t>(wave_delay_frames);
                // Read from the ring buffer, interpolating linearly.
                Frame val = audio_buffer_[rb_source & buffer_mask_];
                const Frame val_next = audio_buffer_[(rb_source - 1) & buffer_mask_];
                val += (val_next - val) * wave_delay_frac;
                val = val * c1 + h * c2;
                h = val;
                dst[i] += val * vol_modifier;
                local_cycles += increment;
                ++local_rb_pos;
            }
            filter_h_[vc] = h;
            cycles_[vc] += static_cast<std::uint64_t>(static_cast<int>(std::rint(
                static_cast<float>(cycles_to_mix * static_cast<double>(1U << kCyclesFrac)))));
        }
        buffer_pos_ += static_cast<std::uint32_t>(frames);
    }

    float mix_rate_;
    int voice_count_ = 2;
    float dry_ = 1;
    float wet_ = 0.5F;
    std::array<Voice, 4> voices_;
    std::vector<Frame> audio_buffer_;
    std::uint32_t buffer_pos_ = 0;
    std::uint32_t buffer_mask_ = 0;
    std::array<Frame, 4> filter_h_{};
    std::array<std::uint64_t, 4> cycles_{};
};

// ---------------------------------------------------------------------------------------------
// Phaser (audio_effect_phaser.cpp)

constexpr std::array kPhaser{
    number("range_min_hz", 10, 10000, 440, "The lowest frequency the notches sweep to."),
    number("range_max_hz", 10, 10000, 1600, "The highest frequency the notches sweep to."),
    number("rate_hz", 0.01, 20, 0.5, "Sweeps per second."),
    number("feedback", 0.1, 0.9, 0.7, "How much of the output feeds back in."),
    number("depth", 0.1, 4, 1, "How strong the effect is."),
};

class Phaser final : public EffectInstance {
public:
    Phaser(const Params& params, int mix_rate)
        : mix_rate_(static_cast<float>(mix_rate)), range_min_(params("range_min_hz")),
          range_max_(params("range_max_hz")), rate_(params("rate_hz")),
          feedback_(params("feedback")), depth_(params("depth")) {}

    void process(const Frame* src, Frame* dst, int frames) override {
        const float sampling_rate = mix_rate_;
        const auto dmin = static_cast<float>(range_min_ / (sampling_rate / 2.0));
        const auto dmax = static_cast<float>(range_max_ / (sampling_rate / 2.0));
        const auto increment = static_cast<float>(kTau * (rate_ / sampling_rate));
        for (int i = 0; i < frames; ++i) {
            phase_ += increment;
            while (phase_ >= kTau) {
                phase_ = static_cast<float>(phase_ - kTau);
            }
            const float d = dmin + (dmax - dmin) * ((std::sin(phase_) + 1.F) / 2.F);
            // Update the filters' coefficients.
            for (auto& channel : allpass_) {
                for (AllpassDelay& allpass : channel) {
                    allpass.delay(d);
                }
            }
            dst[i].left = src[i].left + run(allpass_[0], src[i].left, h_.left) * depth_;
            dst[i].right = src[i].right + run(allpass_[1], src[i].right, h_.right) * depth_;
        }
    }

private:
    class AllpassDelay {
    public:
        void delay(float d) { a_ = (1.F - d) / (1.F + d); }
        float update(float s) {
            const float y = s * -a_ + h_;
            h_ = y * a_ + s;
            return y;
        }

    private:
        float a_ = 0;
        float h_ = 0;
    };

    // The six all-pass stages, the last first.
    float run(std::array<AllpassDelay, 6>& stages, float in, float& h) const {
        float y = in + h * feedback_;
        for (std::size_t j = stages.size(); j-- > 0;) {
            y = stages[j].update(y);
        }
        h = y;
        return y;
    }

    float mix_rate_;
    float range_min_;
    float range_max_;
    float rate_;
    float feedback_;
    float depth_;
    float phase_ = 0;
    Frame h_{};
    std::array<std::array<AllpassDelay, 6>, 2> allpass_;
};

// ---------------------------------------------------------------------------------------------
// Distortion (audio_effect_distortion.cpp)

constexpr std::array<std::string_view, 5> kDistortionModes{"clip", "atan", "lofi", "overdrive",
                                                           "waveshape"};

constexpr std::array kDistortion{
    number("pre_gain", -60, 60, 0, "Gain in dB before the distortion."),
    number("keep_hf_hz", 1, 20500, 16000, "Frequencies above this, in Hz, are kept undistorted."),
    number("drive", 0, 1, 0, "How strong the distortion is."),
    number("post_gain", -80, 24, 0, "Gain in dB after the distortion."),
};

class Distortion final : public EffectInstance {
public:
    Distortion(const Params& params, int mix_rate)
        : mix_rate_(static_cast<float>(mix_rate)), pre_gain_(params("pre_gain")),
          keep_hf_hz_(std::max(params("keep_hf_hz"), 1.0F)), drive_(params("drive")),
          post_gain_(params("post_gain")) {
        if (const json::Value* mode = params.find("mode"); mode != nullptr) {
            if (const auto* const it = std::ranges::find(kDistortionModes, mode->as_string());
                it != kDistortionModes.end()) {
                mode_ = static_cast<std::size_t>(it - kDistortionModes.begin());
            }
        }
    }

    void process(const Frame* src, Frame* dst, int frames) override {
        const auto lpf_c = static_cast<float>(std::exp(-kTau * keep_hf_hz_ / mix_rate_));
        const float lpf_ic = 1.0F - lpf_c;
        const float drive_f = drive_;
        const float pregain_f = db_to_linear(pre_gain_);
        const float postgain_f = db_to_linear(post_gain_);
        const auto atan_mult =
            static_cast<float>(std::pow(10, drive_f * drive_f * 3.0) - 1.0 + 0.001);
        const auto atan_div =
            static_cast<float>(1.0 / (std::atan(atan_mult) * (1.0 + drive_f * 8)));
        // From 16 bits to 2.
        const auto lofi_mult = static_cast<float>(std::pow(2.0, 2.0 + (1.0 - drive_f) * 14));

        const auto shape = [&](float in, float& h) {
            const float out = undenormalize(in * lpf_ic + lpf_c * h);
            h = out;
            float a = out;
            const float ha = in - out; // high frequencies
            a *= pregain_f;
            switch (mode_) {
            case 0: { // clip
                const float a_sign = a < 0 ? -1.0F : 1.0F;
                a = static_cast<float>(std::pow(std::abs(a), 1.0001 - drive_f) * a_sign);
                a = std::clamp(a, -1.0F, 1.0F);
                break;
            }
            case 1: // atan
                a = std::atan(a * atan_mult) * atan_div;
                break;
            case 2: // lofi
                a = static_cast<float>(std::floor(a * lofi_mult + 0.5) / lofi_mult);
                break;
            case 3: { // overdrive
                const double x = a * 0.686306;
                const double z = 1 + std::exp(std::sqrt(std::abs(x)) * -0.75);
                a = static_cast<float>((std::exp(x) - std::exp(-x * z)) /
                                       (std::exp(x) + std::exp(-x)));
                break;
            }
            default: { // waveshape
                const float x = a;
                const auto k = static_cast<float>(2 * drive_f / (1.00001 - drive_f));
                a = static_cast<float>((1.0 + k) * x / (1.0 + k * std::abs(x)));
                break;
            }
            }
            return a * postgain_f + ha;
        };
        for (int i = 0; i < frames; ++i) {
            dst[i].left = shape(src[i].left, h_[0]);
            dst[i].right = shape(src[i].right, h_[1]);
        }
    }

private:
    float mix_rate_;
    float pre_gain_;
    float keep_hf_hz_;
    float drive_;
    float post_gain_;
    std::size_t mode_ = 0;
    std::array<float, 2> h_{};
};

// ---------------------------------------------------------------------------------------------
// Compressor (audio_effect_compressor.cpp)

constexpr std::array kCompressor{
    number("threshold", -60, 0, 0, "Level in dB above which the sound is compressed."),
    number("ratio", 1, 48, 4, "How much sound above the threshold is reduced."),
    number("gain", -20, 20, 0, "Gain in dB after compressing."),
    number("attack_us", 20, 2000, 20, "Microseconds compression takes to start."),
    number("release_ms", 20, 2000, 250, "Milliseconds compression takes to stop."),
    number("mix", 0, 1, 1, "How much of the compressed sound replaces the original."),
};

class Compressor final : public EffectInstance {
public:
    Compressor(const Params& params, MixContext& context)
        : context_(context), threshold_(params("threshold")), ratio_(params("ratio")),
          gain_(params("gain")), attack_us_(params("attack_us")), release_ms_(params("release_ms")),
          mix_(params("mix")) {
        if (const json::Value* sidechain = params.find("sidechain"); sidechain != nullptr) {
            sidechain_ = sidechain->as_string();
        }
    }

    void process(const Frame* src_frames, Frame* dst, int frames) override {
        const float threshold = db_to_linear(threshold_);
        const auto sample_rate = static_cast<float>(context_.mix_rate());
        const float ratatcoef = std::exp(-1 / (0.00001F * sample_rate));
        const float ratrelcoef = std::exp(-1 / (0.5F * sample_rate));
        const auto attime = static_cast<float>(attack_us_ / 1000000.0);
        const auto reltime = static_cast<float>(release_ms_ / 1000.0);
        const float atcoef = std::exp(-1 / (attime * sample_rate));
        const float relcoef = std::exp(-1 / (reltime * sample_rate));
        const float makeup = db_to_linear(gain_);
        const float mix = mix_;

        const Frame* src = src_frames;
        if (!sidechain_.empty()) {
            src = context_.bus_buffer(sidechain_);
        }
        for (int i = 0; i < frames; ++i) {
            const float peak = std::max(std::abs(src[i].left), std::abs(src[i].right));
            float overdb = 2.08136898F * linear_to_db(peak / threshold);
            // Only what goes over is compressed.
            overdb = std::max(overdb, 0.0F);
            if (overdb - rundb_ > 5) { // the difference is too large
                averatio_ = 4;
            }
            if (overdb > rundb_) {
                rundb_ = overdb + atcoef * (rundb_ - overdb);
                runratio_ = averatio_ + ratatcoef * (runratio_ - averatio_);
            } else {
                rundb_ = overdb + relcoef * (rundb_ - overdb);
                runratio_ = averatio_ + ratrelcoef * (runratio_ - averatio_);
            }
            overdb = rundb_;
            averatio_ = runratio_;
            const float cratio = ratio_;
            const float gr = -overdb * (cratio - 1) / cratio;
            const float grv = db_to_linear(gr);
            dst[i] =
                src_frames[i] * grv * makeup * mix + src_frames[i] * static_cast<float>(1.0 - mix);
        }
    }

private:
    MixContext& context_;
    float threshold_;
    float ratio_;
    float gain_;
    float attack_us_;
    float release_ms_;
    float mix_;
    std::string sidechain_;
    float rundb_ = 0;
    float averatio_ = 0;
    float runratio_ = 0;
};

// ---------------------------------------------------------------------------------------------
// Hard limiter (audio_effect_hard_limiter.cpp)

constexpr std::array kHardLimiter{
    number("pre_gain_db", -24, 24, 0, "Gain in dB before limiting."),
    number("ceiling_db", -24, 0, -0.3, "The loudest the sound may get, in dB."),
    number("release", 0.01, 3, 0.1, "Seconds the limiter takes to let go."),
};

class HardLimiter final : public EffectInstance {
public:
    HardLimiter(const Params& params, int mix_rate)
        : sample_rate_(static_cast<float>(mix_rate)), pre_gain_(params("pre_gain_db")),
          ceiling_(params("ceiling_db")), release_(params("release")) {
        const auto latency = static_cast<std::size_t>(std::ceil(sample_rate_ * kAttack)) + 1;
        sample_buffer_left_.assign(latency, 0.0F);
        sample_buffer_right_.assign(latency, 0.0F);
        gain_samples_to_store_ =
            static_cast<int>(std::ceil(sample_rate_ * (kAttack + kSustain) + 1));
        gain_bucket_size_ = static_cast<int>(sample_rate_ * kAttack);
        for (int i = 0; i < gain_samples_to_store_; i += gain_bucket_size_) {
            gain_buckets_.push_back(1.0F);
        }
    }

    void process(const Frame* src, Frame* dst, int frames) override {
        const float ceiling = db_to_linear(ceiling_);
        const float release = release_;
        const float attack = kAttack;
        const float pre_gain = db_to_linear(pre_gain_);
        const auto lerp = [](float from, float to, float weight) {
            return from + (to - from) * weight;
        };
        for (int i = 0; i < frames; ++i) {
            const float sample_left = src[i].left * pre_gain;
            const float sample_right = src[i].right * pre_gain;
            const float largest_sample = std::max(std::abs(sample_left), std::abs(sample_right));
            release_factor_ =
                static_cast<float>(std::max(0.0, release_factor_ - 1.0 / sample_rate_));
            release_factor_ = std::min(release_factor_, release);
            if (release_factor_ > 0.0) {
                gain_ = lerp(gain_target_, 1.0F, 1.0F - release_factor_ / release);
            }
            if (largest_sample * gain_ > ceiling) {
                gain_target_ = ceiling / largest_sample;
                release_factor_ = release;
                attack_factor_ = attack;
            }
            // Ease the gain over the attack time to avoid distortion.
            attack_factor_ = std::max(0.0F, attack_factor_ - 1.0F / sample_rate_);
            if (attack_factor_ > 0.0) {
                gain_ = lerp(gain_target_, gain_, 1.0F - attack_factor_ / attack);
            }
            const auto bucket_id =
                static_cast<std::size_t>(gain_bucket_cursor_ / gain_bucket_size_);
            // The first item in a bucket resets it.
            if (gain_bucket_cursor_ % gain_bucket_size_ == 0) {
                gain_buckets_[bucket_id] = 1.0F;
            }
            gain_buckets_[bucket_id] = std::min(gain_buckets_[bucket_id], gain_);
            gain_bucket_cursor_ = (gain_bucket_cursor_ + 1) % gain_samples_to_store_;
            for (const float bucket : gain_buckets_) {
                gain_ = std::min(gain_, bucket);
            }
            // Latency: play the frame stored before, and store this one.
            const float dst_left = sample_buffer_left_[sample_cursor_];
            const float dst_right = sample_buffer_right_[sample_cursor_];
            sample_buffer_left_[sample_cursor_] = sample_left;
            sample_buffer_right_[sample_cursor_] = sample_right;
            sample_cursor_ = (sample_cursor_ + 1) % sample_buffer_left_.size();
            dst[i] = {dst_left * gain_, dst_right * gain_};
        }
    }

private:
    static constexpr float kAttack = 0.002F;
    static constexpr float kSustain = 0.02F;

    float sample_rate_;
    float pre_gain_;
    float ceiling_;
    float release_;
    std::size_t sample_cursor_ = 0;
    float release_factor_ = 0;
    float attack_factor_ = 0;
    float gain_ = 1;
    float gain_target_ = 1;
    std::vector<float> sample_buffer_left_;
    std::vector<float> sample_buffer_right_;
    int gain_samples_to_store_ = 0;
    int gain_bucket_cursor_ = 0;
    int gain_bucket_size_ = 1;
    std::vector<float> gain_buckets_;
};

// ---------------------------------------------------------------------------------------------
// Panner (audio_effect_panner.cpp)

constexpr std::array kPanner{
    number("pan", -1, 1, 0, "From -1 (all left) to 1 (all right)."),
};

class Panner final : public EffectInstance {
public:
    explicit Panner(const Params& params) : pan_(params("pan")) {}

    void process(const Frame* src, Frame* dst, int frames) override {
        const auto lvol = static_cast<float>(std::clamp(1.0 - pan_, 0.0, 1.0));
        const auto rvol = static_cast<float>(std::clamp(1.0 + pan_, 0.0, 1.0));
        for (int i = 0; i < frames; ++i) {
            dst[i].left = static_cast<float>(src[i].left * lvol + src[i].right * (1.0 - rvol));
            dst[i].right = static_cast<float>(src[i].right * rvol + src[i].left * (1.0 - lvol));
        }
    }

private:
    float pan_;
};

// ---------------------------------------------------------------------------------------------
// Stereo enhance (audio_effect_stereo_enhance.cpp)

constexpr std::array kStereoEnhance{
    number("pan_pullout", 0, 4, 1, "How far apart left and right are pushed; 1 for unchanged."),
    number("time_pullout_ms", 0, 50, 0, "Milliseconds the right channel lags the left."),
    number("surround", 0, 1, 0, "How much of the sound is spread around; 0 for none."),
};

class StereoEnhance final : public EffectInstance {
public:
    StereoEnhance(const Params& params, int mix_rate)
        : intensity_(params("pan_pullout")), surround_(params("surround")),
          delay_frames_(static_cast<std::uint32_t>((params("time_pullout_ms") / 1000.0) *
                                                   static_cast<float>(mix_rate))) {
        const std::uint32_t size = ring_size(50 + 2, mix_rate); // MAX_DELAY_MS + 2
        ringbuff_mask_ = size - 1;
        delay_ringbuff_.assign(size, 0.0F);
    }

    void process(const Frame* src, Frame* dst, int frames) override {
        const bool surround_mode = surround_ > 0;
        for (int i = 0; i < frames; ++i) {
            float left = src[i].left;
            float right = src[i].right;
            const float center = (left + right) / 2.0F;
            left = (center + (left - center) * intensity_);
            right = (center + (right - center) * intensity_);
            if (surround_mode) {
                const auto val = static_cast<float>((left + right) / 2.0);
                delay_ringbuff_[ringbuff_pos_ & ringbuff_mask_] = val;
                const float out =
                    delay_ringbuff_[(ringbuff_pos_ - delay_frames_) & ringbuff_mask_] * surround_;
                left += out;
                right += -out;
            } else {
                delay_ringbuff_[ringbuff_pos_ & ringbuff_mask_] = right;
                // The right channel lags.
                right = delay_ringbuff_[(ringbuff_pos_ - delay_frames_) & ringbuff_mask_];
            }
            dst[i] = {left, right};
            ++ringbuff_pos_;
        }
    }

private:
    float intensity_;
    float surround_;
    std::uint32_t delay_frames_;
    std::vector<float> delay_ringbuff_;
    std::uint32_t ringbuff_pos_ = 0;
    std::uint32_t ringbuff_mask_ = 0;
};

// ---------------------------------------------------------------------------------------------
// The table

using Make = std::unique_ptr<EffectInstance> (*)(const Params&, MixContext&);

template <Filter::Mode M>
std::unique_ptr<EffectInstance> make_filter(const Params& params, MixContext& context) {
    return std::make_unique<FilterEffect>(M, params, context.mix_rate());
}

template <const auto& Bands>
std::unique_ptr<EffectInstance> make_eq(const Params& params, MixContext& context) {
    return std::make_unique<Equalizer>(Bands, params, context.mix_rate());
}

template <typename T>
std::unique_ptr<EffectInstance> make_with_rate(const Params& params, MixContext& context) {
    return std::make_unique<T>(params, context.mix_rate());
}

struct EffectType {
    std::string_view name;
    std::string_view doc;
    std::span<const Setting> settings;
    Make make;
    std::span<const double> bands; // equalizers
};

const std::array<EffectType, 20> kTypes{{
    {"amplify",
     "Godot's AudioEffectAmplify: changes the volume.",
     kAmplify,
     [](const Params& p, MixContext&) -> std::unique_ptr<EffectInstance> {
         return std::make_unique<Amplify>(p);
     },
     {}},
    {"low_pass_filter",
     "Godot's AudioEffectLowPassFilter: cuts frequencies above the cutoff.",
     kFilter,
     make_filter<Filter::Mode::LowPass>,
     {}},
    {"high_pass_filter",
     "Godot's AudioEffectHighPassFilter: cuts frequencies below the cutoff.",
     kFilter,
     make_filter<Filter::Mode::HighPass>,
     {}},
    {"band_pass_filter",
     "Godot's AudioEffectBandPassFilter: keeps frequencies near the cutoff and cuts the rest.",
     kFilter,
     make_filter<Filter::Mode::BandPass>,
     {}},
    {"notch_filter",
     "Godot's AudioEffectNotchFilter: cuts frequencies near the cutoff.",
     kFilter,
     make_filter<Filter::Mode::Notch>,
     {}},
    {"band_limit_filter",
     "Godot's AudioEffectBandLimitFilter: keeps a band from the cutoff, with resonance as Godot "
     "uses it.",
     kFilter,
     make_filter<Filter::Mode::BandLimit>,
     {}},
    {"low_shelf_filter",
     "Godot's AudioEffectLowShelfFilter: scales frequencies below the cutoff by gain.",
     kShelfFilter,
     make_filter<Filter::Mode::LowShelf>,
     {}},
    {"high_shelf_filter",
     "Godot's AudioEffectHighShelfFilter: scales frequencies above the cutoff by gain.",
     kShelfFilter,
     make_filter<Filter::Mode::HighShelf>,
     {}},
    {"eq6",
     "Godot's AudioEffectEQ6: six bands from 32 Hz to 10 kHz, each set in dB.",
     {},
     make_eq<kBands6>,
     kBands6},
    {"eq10",
     "Godot's AudioEffectEQ10: ten bands from 31 Hz to 16 kHz, each set in dB.",
     {},
     make_eq<kBands10>,
     kBands10},
    {"eq21",
     "Godot's AudioEffectEQ21: 21 bands from 22 Hz to 22 kHz, each set in dB.",
     {},
     make_eq<kBands21>,
     kBands21},
    {"delay",
     "Godot's AudioEffectDelay: two echoes and repeating feedback.",
     kDelay,
     make_with_rate<Delay>,
     {}},
    {"reverb",
     "Godot's AudioEffectReverb: the sound of a room.",
     kReverb,
     make_with_rate<ReverbEffect>,
     {}},
    {"chorus",
     "Godot's AudioEffectChorus: up to four delayed, wavering copies.",
     kChorus,
     make_with_rate<Chorus>,
     {}},
    {"phaser",
     "Godot's AudioEffectPhaser: notches that sweep up and down.",
     kPhaser,
     make_with_rate<Phaser>,
     {}},
    {"distortion",
     "Godot's AudioEffectDistortion: clips or shapes the waveform.",
     kDistortion,
     make_with_rate<Distortion>,
     {}},
    {"compressor",
     "Godot's AudioEffectCompressor: turns loud sound down, optionally by another bus's level "
     "(sidechain).",
     kCompressor,
     [](const Params& p, MixContext& context) -> std::unique_ptr<EffectInstance> {
         return std::make_unique<Compressor>(p, context);
     },
     {}},
    {"hard_limiter",
     "Godot's AudioEffectHardLimiter: keeps the sound under a ceiling, with 2 ms of latency.",
     kHardLimiter,
     make_with_rate<HardLimiter>,
     {}},
    {"panner",
     "Godot's AudioEffectPanner: moves the sound left or right.",
     kPanner,
     [](const Params& p, MixContext&) -> std::unique_ptr<EffectInstance> {
         return std::make_unique<Panner>(p);
     },
     {}},
    {"stereo_enhance",
     "Godot's AudioEffectStereoEnhance: widens or narrows the stereo image.",
     kStereoEnhance,
     make_with_rate<StereoEnhance>,
     {}},
}};

json::Value setting_schema(const Setting& setting) {
    json::Value value;
    switch (setting.kind) {
    case Kind::Boolean:
        value["type"] = "boolean";
        value["default"] = setting.fallback != 0.0;
        break;
    case Kind::Integer:
        value["type"] = "integer";
        value["minimum"] = static_cast<std::int64_t>(setting.minimum);
        value["maximum"] = static_cast<std::int64_t>(setting.maximum);
        value["default"] = static_cast<std::int64_t>(setting.fallback);
        break;
    case Kind::Number:
        value["type"] = "number";
        value["minimum"] = setting.minimum;
        value["maximum"] = setting.maximum;
        value["default"] = setting.fallback;
        break;
    }
    value["description"] = setting.doc;
    return value;
}

json::Value type_schema(const EffectType& type) {
    json::Value schema;
    schema["type"] = "object";
    schema["description"] = type.doc;
    schema["additionalProperties"] = false;
    schema["required"] = json::Array{"type"};
    json::Value& properties = schema["properties"];
    properties["type"]["const"] = type.name;
    properties["enabled"]["type"] = "boolean";
    properties["enabled"]["default"] = true;
    properties["enabled"]["description"] = "Whether the effect runs.";
    for (const Setting& setting : type.settings) {
        properties[setting.name] = setting_schema(setting);
    }
    if (type.settings.data() == kFilter.data() || type.settings.data() == kShelfFilter.data()) {
        properties["db"]["enum"] = json::Array{6, 12, 18, 24};
    }
    if (!type.bands.empty()) {
        json::Value& bands = properties["band_db"];
        bands["type"] = "object";
        bands["additionalProperties"] = false;
        bands["description"] = "Each band's gain in dB, by its frequency in whole Hz.";
        for (const double frequency : type.bands) {
            bands["properties"][band_name(frequency)] =
                setting_schema(number("", -60, 24, 0, "Gain in dB."));
        }
    }
    if (type.name == "chorus") {
        json::Value& voices = properties["voices"];
        voices["type"] = "array";
        voices["maxItems"] = 4;
        voices["description"] =
            "Each voice's settings, as Godot's voice/1 to voice/4; voice_count says how many "
            "play. Godot's defaults: voice 1 delay_ms 15, rate_hz 0.8, depth_ms 2, cutoff_hz "
            "8000, pan -0.5; voice 2 delay_ms 20, rate_hz 1.2, depth_ms 3, cutoff_hz 8000, pan "
            "0.5; voices 3 and 4 as each setting's default.";
        json::Value& voice = voices["items"];
        voice["type"] = "object";
        voice["additionalProperties"] = false;
        for (const Setting& setting : kChorusVoice) {
            voice["properties"][setting.name] = setting_schema(setting);
        }
    }
    if (type.name == "distortion") {
        json::Value& mode = properties["mode"];
        mode["type"] = "string";
        json::Array names;
        for (const std::string_view name : kDistortionModes) {
            names.emplace_back(name);
        }
        mode["enum"] = std::move(names);
        mode["default"] = "clip";
        mode["description"] = "How the waveform is shaped.";
    }
    if (type.name == "compressor") {
        json::Value& sidechain = properties["sidechain"];
        sidechain["type"] = "string";
        sidechain["description"] =
            "A bus whose level drives the compression instead of this bus's own.";
    }
    return schema;
}

} // namespace

std::vector<std::string_view> effect_types() {
    std::vector<std::string_view> names;
    names.reserve(kTypes.size());
    for (const EffectType& type : kTypes) {
        names.push_back(type.name);
    }
    return names;
}

json::Value effect_schema() {
    json::Value schema;
    schema["description"] =
        "An effect, as one of Godot's AudioEffect resources: its type and its settings.";
    json::Array options;
    for (const EffectType& type : kTypes) {
        options.push_back(type_schema(type));
    }
    schema["anyOf"] = std::move(options);
    return schema;
}

Result<std::unique_ptr<EffectInstance>> make_effect(const EffectSlot& slot, MixContext& context) {
    const auto* const it = std::ranges::find(kTypes, slot.type, &EffectType::name);
    if (it == kTypes.end()) {
        return make_error(ErrorCode::InvalidArgument,
                          "\"" + slot.type + "\" is not an audio effect type");
    }
    const Params params(slot.params, it->settings);
    return it->make(params, context);
}

} // namespace slash::audio
