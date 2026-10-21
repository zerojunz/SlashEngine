#include "slash/core/math.hpp"
#include "slash/soundtrack/render.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The built-in sound bank: waveforms and drums synthesized with arithmetic and slash::cos_sin
// only, laid out as a SoundFont 2 file (the RIFF "sfbk" form of the SoundFont 2.01 specification)
// that TinySoundFont plays like any other.
namespace slash::soundtrack {
namespace {

constexpr double kTwoPi = 6.283185307179586;

// Melodic voices loop one cycle of kCycle samples, played at C3 (130.81 Hz) at kCycleRate.
constexpr int kCycle = 256;
constexpr int kCycleRoot = 48;
constexpr std::uint32_t kCycleRate = 33488;
constexpr std::uint32_t kDrumRate = 32000;
// After a loop, a copy of its first samples, which interpolation reads past the loop's end.
constexpr int kGuard = 8;
// Zero samples after each sample, as SoundFont 2 requires.
constexpr int kPadding = 46;
// Samples peak 12 dB below full scale, drums 8 dB, so chords and several tracks together stay
// clear of it. (In the samples rather than as initialAttenuation, which players scale
// differently.)
constexpr double kVoiceLevel = 0.25;
constexpr double kDrumLevel = 0.4;

// SoundFont 2 generators.
enum Gen : std::uint8_t {
    kVibLfoToPitch = 6,
    kFilterFc = 8,
    kDelayVibLfo = 23,
    kFreqVibLfo = 24,
    kAttackVolEnv = 34,
    kDecayVolEnv = 36,
    kSustainVolEnv = 37,
    kReleaseVolEnv = 38,
    kKeynumToVolEnvDecay = 40,
    kInstrument = 41,
    kKeyRange = 43,
    kSampleId = 53,
    kSampleModes = 54,
    kExclusiveClass = 57,
    kRootKey = 58,
};

struct Generator {
    Gen oper;
    std::uint16_t amount;
};

// A generator's signed amount, stored as SoundFont 2 stores it.
std::uint16_t amount(int value) {
    return static_cast<std::uint16_t>(static_cast<std::int16_t>(value));
}

std::uint16_t key_range(int low, int high) {
    return static_cast<std::uint16_t>(low | (high << 8));
}

// Seconds as timecents, 1200 * log2(seconds).
int timecents(double seconds) {
    return static_cast<int>(std::lround(1200.0 * std::log2(seconds)));
}

// e^-x for the small x of a decay from one sample to the next, by its series, so decays are the
// same on every platform.
double exp_neg(double x) {
    return 1.0 - x + (x * x / 2.0) - (x * x * x / 6.0) + (x * x * x * x / 24.0);
}

// The factor a level falls by each sample to fall to 1/e over the given seconds.
double decay_factor(double seconds, double rate) {
    return exp_neg(1.0 / (seconds * rate));
}

double sine(double phase) {
    const double turns = phase / kTwoPi;
    const double wrapped = (turns - std::floor(turns)) * kTwoPi;
    return static_cast<double>(cos_sin(static_cast<float>(wrapped)).y);
}

// Samples scaled to peak at the given level, as 16-bit integers.
std::vector<std::int16_t> to_pcm(std::span<const double> values, double level = kVoiceLevel) {
    double peak = 0.0;
    for (const double value : values) {
        peak = std::max(peak, std::abs(value));
    }
    const double scale = peak > 0.0 ? level * 32767.0 / peak : 0.0;
    std::vector<std::int16_t> out;
    out.reserve(values.size());
    for (const double value : values) {
        out.push_back(static_cast<std::int16_t>(std::lround(value * scale)));
    }
    return out;
}

// White noise from a xorshift generator, the same sequence everywhere.
class Noise {
public:
    double next() {
        state_ ^= state_ << 13U;
        state_ ^= state_ >> 17U;
        state_ ^= state_ << 5U;
        return (static_cast<double>(state_) / 2147483647.5) - 1.0;
    }

private:
    std::uint32_t state_ = 2463534242U;
};

enum class Wave : std::uint8_t {
    Sine,
    Triangle,
    Saw,
    Square,
    Pluck,
    Organ,
    Reed,
    Voice,
    Bell,
    Noise
};

// The amplitude of a wave's nth harmonic.
double harmonic(Wave wave, int n) {
    const double odd = n % 2 == 1 ? 1.0 : 0.0;
    const double sign = ((n - 1) / 2) % 2 == 0 ? 1.0 : -1.0;
    switch (wave) {
    case Wave::Sine:
        return n == 1 ? 1.0 : 0.0;
    case Wave::Triangle:
        return odd * sign / (n * n);
    case Wave::Saw:
        return (n % 2 == 1 ? 1.0 : -1.0) / n;
    case Wave::Square:
        return odd / n;
    case Wave::Pluck:
        return 1.0 / (n * std::sqrt(static_cast<double>(n)));
    case Wave::Organ: {
        constexpr std::array<double, 9> kDrawbars = {0.0, 1.0, 0.7, 0.5, 0.45, 0.0, 0.25, 0.0, 0.2};
        return n < static_cast<int>(kDrawbars.size()) ? kDrawbars.at(static_cast<std::size_t>(n))
                                                      : 0.0;
    }
    case Wave::Reed:
        return (odd + ((1.0 - odd) * 0.2)) / n;
    case Wave::Voice: {
        constexpr std::array<double, 6> kVoice = {0.0, 1.0, 0.5, 0.35, 0.15, 0.1};
        return n < static_cast<int>(kVoice.size()) ? kVoice.at(static_cast<std::size_t>(n)) : 0.0;
    }
    case Wave::Bell:
        switch (n) {
        case 1:
            return 1.0;
        case 4:
            return 0.45;
        case 7:
            return 0.12;
        default:
            return 0.0;
        }
    case Wave::Noise:
        return 0.0;
    }
    return 0.0;
}

// One cycle of a wave with its harmonics up to the given one, then the guard samples.
std::vector<std::int16_t> cycle(Wave wave, int harmonics) {
    std::vector<double> values(kCycle);
    for (int i = 0; i < kCycle; ++i) {
        double value = 0.0;
        for (int n = 1; n <= harmonics; ++n) {
            const double level = harmonic(wave, n);
            if (level != 0.0) {
                value += level * sine(kTwoPi * ((n * i) % kCycle) / kCycle);
            }
        }
        values[static_cast<std::size_t>(i)] = value;
    }
    std::vector<std::int16_t> out = to_pcm(values);
    for (int i = 0; i < kGuard; ++i) {
        out.push_back(out[static_cast<std::size_t>(i)]);
    }
    return out;
}

std::vector<std::int16_t> looped_noise() {
    Noise noise;
    std::vector<double> values(4096);
    for (double& value : values) {
        value = noise.next();
    }
    std::vector<std::int16_t> out = to_pcm(values);
    for (int i = 0; i < kGuard; ++i) {
        out.push_back(out[static_cast<std::size_t>(i)]);
    }
    return out;
}

// The drums, each a short sound made from swept sines, noise and decays.
enum class Drum : std::uint8_t {
    Kick,
    Snare,
    Clap,
    Hat,
    Tom,
    Crash,
    Ride,
    Cowbell,
    Click,
    Shaker,
    Metal,
    Whistle
};

// High-passes noise by taking differences, once per pass.
class Bright {
public:
    explicit Bright(int passes) : passes_(passes) {}

    double next() {
        double value = noise_.next();
        for (int i = 0; i < passes_; ++i) {
            const double difference = value - last_.at(static_cast<std::size_t>(i));
            last_.at(static_cast<std::size_t>(i)) = value;
            value = difference * 0.5;
        }
        return value;
    }

private:
    Noise noise_;
    int passes_;
    std::array<double, 4> last_{};
};

std::vector<std::int16_t> drum(Drum kind) {
    constexpr double rate = kDrumRate;
    const auto frames = [&](double seconds) { return static_cast<std::size_t>(seconds * rate); };
    std::vector<double> values;
    switch (kind) {
    case Drum::Kick: {
        values.resize(frames(0.45));
        Noise noise;
        double phase = 0.0;
        double sweep = 1.0;
        double level = 1.0;
        double click = 0.3;
        const double sweep_factor = decay_factor(0.035, rate);
        const double level_factor = decay_factor(0.22, rate);
        const double click_factor = decay_factor(0.002, rate);
        for (double& value : values) {
            value = (level * sine(phase)) + (click * noise.next());
            phase += kTwoPi * (48.0 + (110.0 * sweep)) / rate;
            sweep *= sweep_factor;
            level *= level_factor;
            click *= click_factor;
        }
        break;
    }
    case Drum::Snare: {
        values.resize(frames(0.35));
        Bright noise(1);
        double phase = 0.0;
        double tone = 0.5;
        double rattle = 0.8;
        const double tone_factor = decay_factor(0.06, rate);
        const double rattle_factor = decay_factor(0.09, rate);
        for (double& value : values) {
            value = (tone * sine(phase)) + (rattle * noise.next());
            phase += kTwoPi * 190.0 / rate;
            tone *= tone_factor;
            rattle *= rattle_factor;
        }
        break;
    }
    case Drum::Clap: {
        values.resize(frames(0.3));
        Bright noise(1);
        const double burst_factor = decay_factor(0.004, rate);
        const double tail_factor = decay_factor(0.08, rate);
        double burst = 0.0;
        double tail = 0.0;
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (i == 0 || i == frames(0.01) || i == frames(0.02)) {
                burst = 1.0;
            }
            if (i == frames(0.03)) {
                tail = 0.7;
            }
            values[i] = (burst + tail) * noise.next();
            burst *= burst_factor;
            tail *= tail_factor;
        }
        break;
    }
    case Drum::Hat:
    case Drum::Crash:
    case Drum::Shaker: {
        double seconds = 0.3; // a shaker's
        double decay = 0.05;
        if (kind == Drum::Hat) {
            seconds = 0.9;
            decay = 0.25;
        } else if (kind == Drum::Crash) {
            seconds = 2.0;
            decay = 0.6;
        }
        const double attack = kind == Drum::Shaker ? 0.01 : 0.001;
        values.resize(frames(seconds));
        Bright noise(kind == Drum::Crash ? 2 : 3);
        double level = 1.0;
        const double factor = decay_factor(decay, rate);
        for (std::size_t i = 0; i < values.size(); ++i) {
            const double rise = std::min(1.0, static_cast<double>(i) / (attack * rate));
            values[i] = rise * level * noise.next();
            if (rise >= 1.0) {
                level *= factor;
            }
        }
        break;
    }
    case Drum::Tom: {
        values.resize(frames(0.6));
        Noise noise;
        double phase = 0.0;
        double sweep = 1.0;
        double level = 1.0;
        double thump = 0.1;
        const double sweep_factor = decay_factor(0.04, rate);
        const double level_factor = decay_factor(0.18, rate);
        const double thump_factor = decay_factor(0.01, rate);
        for (double& value : values) {
            value = (level * sine(phase)) + (thump * noise.next());
            phase += kTwoPi * 110.0 * (1.0 + (0.4 * sweep)) / rate;
            sweep *= sweep_factor;
            level *= level_factor;
            thump *= thump_factor;
        }
        break;
    }
    case Drum::Ride:
    case Drum::Metal:
    case Drum::Cowbell: {
        // Partials that aren't harmonics of one another, as a struck plate's or bell's are.
        constexpr std::array<double, 4> kRide = {3127.0, 4421.0, 5683.0, 7193.0};
        constexpr std::array<double, 4> kMetal = {4180.0, 5650.0, 7020.0, 9100.0};
        constexpr std::array<double, 4> kCowbell = {560.0, 845.0, 1680.0, 2535.0};
        const std::array<double, 4>* partials = &kMetal;
        double seconds = 1.8;
        double decay = 0.5;
        if (kind == Drum::Ride) {
            partials = &kRide;
            seconds = 1.5;
            decay = 0.7;
        } else if (kind == Drum::Cowbell) {
            partials = &kCowbell;
            seconds = 0.35;
            decay = 0.09;
        }
        values.resize(frames(seconds));
        Bright noise(3);
        std::array<double, 4> phases{};
        double level = 1.0;
        const double factor = decay_factor(decay, rate);
        for (double& value : values) {
            double sum = kind == Drum::Ride ? 0.35 * noise.next() : 0.0;
            for (std::size_t p = 0; p < partials->size(); ++p) {
                sum += (p < 2 ? 0.3 : 0.15) * sine(phases.at(p));
                phases.at(p) += kTwoPi * partials->at(p) / rate;
            }
            value = level * sum;
            level *= factor;
        }
        break;
    }
    case Drum::Click: {
        values.resize(frames(0.08));
        Noise noise;
        double phase = 0.0;
        double level = 1.0;
        double tick = 0.2;
        const double level_factor = decay_factor(0.012, rate);
        const double tick_factor = decay_factor(0.002, rate);
        for (double& value : values) {
            value = (level * sine(phase)) + (tick * noise.next());
            phase += kTwoPi * 1700.0 / rate;
            level *= level_factor;
            tick *= tick_factor;
        }
        break;
    }
    case Drum::Whistle: {
        values.resize(frames(0.6));
        double phase = 0.0;
        double wobble = 0.0;
        const std::size_t release = frames(0.05);
        for (std::size_t i = 0; i < values.size(); ++i) {
            const double rise = std::min(1.0, static_cast<double>(i) / (0.02 * rate));
            const double fall = std::min(1.0, static_cast<double>(values.size() - i) /
                                                  static_cast<double>(release));
            values[i] = rise * fall * sine(phase);
            phase += kTwoPi * (2400.0 + (40.0 * sine(wobble))) / rate;
            wobble += kTwoPi * 12.0 / rate;
        }
        break;
    }
    }
    return to_pcm(values, kDrumLevel);
}

// How one instrument sounds: its wave and its volume envelope, filter and vibrato.
struct Voice {
    Wave wave = Wave::Saw;
    double attack = 0.005;
    double decay = 0.0; // seconds to fall to sustain_cb; 0 holds full level
    int sustain_cb = 0; // what the decay falls to, in centibels down; 960 is silence
    double release = 0.2;
    int keynum_decay = 0;     // timecents shorter the decay is for each key higher
    int filter_cents = 13500; // low-pass cutoff in absolute cents; 13500 is none
    int vibrato_cents = 0;
};

Voice plucked(Wave wave, double decay, double release, int filter = 13500, int keynum = 40) {
    return {.wave = wave,
            .attack = 0.002,
            .decay = decay,
            .sustain_cb = 960,
            .release = release,
            .keynum_decay = keynum,
            .filter_cents = filter};
}

Voice held(Wave wave, double attack, double release, int filter = 13500, int vibrato = 0) {
    return {.wave = wave,
            .attack = attack,
            .release = release,
            .filter_cents = filter,
            .vibrato_cents = vibrato};
}

// General MIDI's instruments, by family of eight, with the members that sound unlike their
// family's first.
Voice voice(int program) {
    switch (program) {
    case 4:
    case 5:
        return plucked(Wave::Bell, 3.0, 0.3);
    case 6:
        return plucked(Wave::Saw, 1.5, 0.15, 12500);
    case 7:
        return plucked(Wave::Square, 1.0, 0.1, 11000);
    case 11:
        return {.wave = Wave::Bell,
                .attack = 0.002,
                .decay = 3.0,
                .sustain_cb = 960,
                .release = 0.5,
                .keynum_decay = 60,
                .vibrato_cents = 8};
    case 12:
    case 13:
        return plucked(Wave::Sine, 0.4, 0.1, 13500, 60);
    case 14:
        return plucked(Wave::Bell, 4.0, 0.8, 13500, 30);
    case 19:
        return held(Wave::Organ, 0.06, 0.4);
    case 21:
    case 22:
    case 23:
        return held(Wave::Reed, 0.03, 0.1, 11500, 6);
    case 29:
    case 30:
        return held(Wave::Square, 0.005, 0.15, 11500);
    case 38:
    case 39:
        return {.wave = Wave::Saw,
                .attack = 0.003,
                .decay = 0.6,
                .sustain_cb = 300,
                .release = 0.08,
                .filter_cents = 10800};
    case 45:
        return plucked(Wave::Pluck, 0.4, 0.1, 12000, 20);
    case 46:
        return plucked(Wave::Triangle, 2.5, 0.5, 13500, 30);
    case 47:
        return plucked(Wave::Sine, 1.5, 0.8, 13500, 0);
    case 52:
    case 53:
    case 54:
        return held(Wave::Voice, 0.2, 0.5, 13500, 15);
    case 55:
        return plucked(Wave::Saw, 0.5, 0.2, 13500, 0);
    case 62:
    case 63:
        return held(Wave::Saw, 0.03, 0.12, 11500, 6);
    case 79:
        return held(Wave::Sine, 0.05, 0.12, 13500, 12);
    case 80:
        return held(Wave::Square, 0.004, 0.08);
    case 81:
        return held(Wave::Saw, 0.004, 0.08);
    case 89:
        return held(Wave::Triangle, 0.5, 1.2, 13500, 6);
    case 91:
        return held(Wave::Voice, 0.5, 1.2, 13500, 10);
    case 108:
        return plucked(Wave::Bell, 0.8, 0.2, 13500, 40);
    case 109:
    case 111:
        return held(Wave::Reed, 0.02, 0.1, 11500, 8);
    case 110:
        return held(Wave::Saw, 0.06, 0.2, 11300, 15);
    case 115:
        return plucked(Wave::Sine, 0.08, 0.05, 13500, 0);
    case 116:
        return plucked(Wave::Sine, 0.8, 0.3, 13500, 0);
    case 117:
    case 118:
        return plucked(Wave::Sine, 0.4, 0.2, 13500, 0);
    case 119:
        return held(Wave::Noise, 1.0, 0.1, 12500);
    case 120:
    case 127:
        return plucked(Wave::Noise, program == 120 ? 0.1 : 0.3, 0.1, 12000, 0);
    case 122:
        return held(Wave::Noise, 1.0, 1.5, 11000);
    case 123:
        return held(Wave::Sine, 0.01, 0.1, 13500, 100);
    case 124:
        return held(Wave::Square, 0.005, 0.05);
    case 126:
        return held(Wave::Noise, 0.3, 1.0, 12000);
    default:
        break;
    }
    switch (program / 8) {
    case 0: // piano
        return plucked(Wave::Pluck, 4.0, 0.35, 11500, 50);
    case 1: // chromatic percussion
        return plucked(Wave::Bell, 1.5, 0.4, 13500, 60);
    case 2: // organ
        return held(Wave::Organ, 0.008, 0.08);
    case 3: // guitar
        return plucked(Wave::Pluck, 2.0, 0.2, 10500, 40);
    case 4: // bass
        return plucked(Wave::Pluck, 2.5, 0.12, 10200, 20);
    case 5: // strings
        return held(Wave::Saw, 0.12, 0.3, 11300, 12);
    case 6: // ensemble
        return held(Wave::Saw, 0.18, 0.45, 11000, 8);
    case 7: // brass
        return {.wave = Wave::Saw,
                .attack = 0.04,
                .decay = 0.4,
                .sustain_cb = 40,
                .release = 0.12,
                .filter_cents = 10800,
                .vibrato_cents = 6};
    case 8: // reed
        return held(Wave::Reed, 0.03, 0.1, 11000, 10);
    case 9: // pipe
        return held(Wave::Triangle, 0.05, 0.12, 13500, 14);
    case 10: // synth lead
        return held(Wave::Saw, 0.005, 0.1, 12000);
    case 11: // synth pad
        return held(Wave::Saw, 0.5, 1.2, 10500, 6);
    case 12: // synth effects
        return held(Wave::Triangle, 0.3, 1.5);
    case 13: // ethnic
        return plucked(Wave::Pluck, 1.5, 0.2, 11500, 40);
    case 14: // percussive
        return plucked(Wave::Bell, 0.5, 0.2, 13500, 40);
    default: // sound effects
        return held(Wave::Noise, 0.05, 0.4, 12000);
    }
}

// A drum key: which drum, how many semitones above its own pitch it plays, how fast it dies
// away (0 for as long as the sound lasts) and which drums cut each other off.
struct DrumKey {
    Drum drum = Drum::Kick;
    int tune = 0;
    double decay = 0.0;
    int exclusive = 0;
};

// General MIDI's drums, from key 35 (acoustic_kick) to 81 (open_triangle).
constexpr std::array<DrumKey, 47> kDrumKeys = {{
    {Drum::Kick, -2},          // acoustic_kick
    {Drum::Kick},              // kick
    {Drum::Click},             // side_stick
    {Drum::Snare},             // snare
    {Drum::Clap},              // clap
    {Drum::Snare, 2},          // electric_snare
    {Drum::Tom, -4},           // low_floor_tom
    {Drum::Hat, 0, 0.06, 1},   // closed_hat
    {Drum::Tom, -2},           // high_floor_tom
    {Drum::Hat, -1, 0.08, 1},  // pedal_hat
    {Drum::Tom},               // low_tom
    {Drum::Hat, 0, 0.0, 1},    // open_hat
    {Drum::Tom, 2},            // low_mid_tom
    {Drum::Tom, 3},            // high_mid_tom
    {Drum::Crash},             // crash
    {Drum::Tom, 5},            // high_tom
    {Drum::Ride},              // ride
    {Drum::Crash, -3},         // china
    {Drum::Ride, 7},           // ride_bell
    {Drum::Shaker, 5, 0.25},   // tambourine
    {Drum::Crash, 4, 0.8},     // splash
    {Drum::Cowbell},           // cowbell
    {Drum::Crash, 1},          // crash_2
    {Drum::Shaker, -5},        // vibraslap
    {Drum::Ride, -1},          // ride_2
    {Drum::Tom, 17},           // high_bongo
    {Drum::Tom, 12},           // low_bongo
    {Drum::Tom, 10, 0.1},      // mute_high_conga
    {Drum::Tom, 10},           // open_high_conga
    {Drum::Tom, 7},            // low_conga
    {Drum::Tom, 15},           // high_timbale
    {Drum::Tom, 12},           // low_timbale
    {Drum::Cowbell, 5},        // high_agogo
    {Drum::Cowbell, 1},        // low_agogo
    {Drum::Shaker},            // cabasa
    {Drum::Shaker, 3, 0.06},   // maracas
    {Drum::Whistle, 0, 0.15},  // short_whistle
    {Drum::Whistle},           // long_whistle
    {Drum::Shaker, -7, 0.1},   // short_guiro
    {Drum::Shaker, -7},        // long_guiro
    {Drum::Click, 7},          // claves
    {Drum::Click, 3},          // high_wood_block
    {Drum::Click, -2},         // low_wood_block
    {Drum::Tom, 24, 0.1},      // mute_cuica
    {Drum::Tom, 22},           // open_cuica
    {Drum::Metal, 0, 0.12, 2}, // mute_triangle
    {Drum::Metal, 0, 0.0, 2},  // open_triangle
}};

// The key a drum sample's header says plays it at its own pitch; each drum key overrides it.
constexpr int kDrumRoot = 60;

// Writes the chunks of a SoundFont 2 file.
class Writer {
public:
    void u8(std::uint8_t value) { out_.push_back(static_cast<char>(value)); }
    void u16(std::uint16_t value) {
        u8(static_cast<std::uint8_t>(value & 0xFFU));
        u8(static_cast<std::uint8_t>(value >> 8U));
    }
    void u32(std::uint32_t value) {
        u16(static_cast<std::uint16_t>(value & 0xFFFFU));
        u16(static_cast<std::uint16_t>(value >> 16U));
    }
    // A name in a fixed field of 20 bytes, zero-filled.
    void name(std::string_view text) {
        for (std::size_t i = 0; i < 20; ++i) {
            u8(i < text.size() && i < 19 ? static_cast<std::uint8_t>(text[i]) : 0);
        }
    }
    void bytes(std::string_view data) { out_.append(data); }
    void chunk(std::string_view id, std::string_view data) {
        bytes(id);
        u32(static_cast<std::uint32_t>(data.size()));
        bytes(data);
        if (data.size() % 2 != 0) {
            u8(0);
        }
    }
    void list(std::string_view type, std::string_view contents) {
        bytes("LIST");
        u32(static_cast<std::uint32_t>(contents.size() + 4));
        bytes(type);
        bytes(contents);
    }
    [[nodiscard]] std::string take() { return std::move(out_); }

private:
    std::string out_;
};

struct SampleHeader {
    std::string name;
    std::uint32_t start;
    std::uint32_t end;
    std::uint32_t loop_start;
    std::uint32_t loop_end;
    std::uint32_t rate;
    std::uint8_t root;
};

struct Named {
    std::string name;
    std::uint16_t bag; // its first zone
    std::uint16_t program = 0;
    std::uint16_t bank = 0;
};

class Bank {
public:
    // Adds a sample, looped from its start over loop samples if loop is above 0.
    std::uint16_t add_sample(std::string name, std::span<const std::int16_t> data,
                             std::uint32_t rate, int root, int loop) {
        const auto start = static_cast<std::uint32_t>(samples_.size());
        samples_.insert(samples_.end(), data.begin(), data.end());
        const auto end = static_cast<std::uint32_t>(samples_.size());
        samples_.insert(samples_.end(), kPadding, 0);
        const auto loop_end = loop > 0 ? start + static_cast<std::uint32_t>(loop) : end;
        headers_.push_back(
            {std::move(name), start, end, start, loop_end, rate, static_cast<std::uint8_t>(root)});
        return static_cast<std::uint16_t>(headers_.size() - 1);
    }

    void begin_instrument(std::string name) {
        instruments_.push_back(
            {.name = std::move(name), .bag = static_cast<std::uint16_t>(instrument_bags_.size())});
    }

    void add_zone(std::span<const Generator> gens) {
        instrument_bags_.push_back(static_cast<std::uint16_t>(instrument_gens_.size()));
        instrument_gens_.insert(instrument_gens_.end(), gens.begin(), gens.end());
    }

    [[nodiscard]] std::uint16_t instrument_count() const {
        return static_cast<std::uint16_t>(instruments_.size());
    }

    void add_preset(std::string name, int program, int bank, std::uint16_t instrument) {
        presets_.push_back({.name = std::move(name),
                            .bag = static_cast<std::uint16_t>(preset_gens_.size()),
                            .program = static_cast<std::uint16_t>(program),
                            .bank = static_cast<std::uint16_t>(bank)});
        preset_gens_.push_back({kInstrument, instrument});
    }

    [[nodiscard]] std::string bytes() const {
        Writer info;
        {
            Writer version;
            version.u16(2);
            version.u16(1);
            info.chunk("ifil", version.take());
        }
        info.chunk("isng", std::string_view("EMU8000\0", 8));
        info.chunk("INAM", std::string_view("SlashEngine General MIDI\0\0", 26));

        Writer smpl;
        for (const std::int16_t sample : samples_) {
            smpl.u16(static_cast<std::uint16_t>(sample));
        }
        Writer sdta;
        sdta.chunk("smpl", smpl.take());

        Writer pdta;
        pdta.chunk("phdr", headers(presets_, true, preset_gens_.size()));
        pdta.chunk("pbag", bags(preset_gens_.size(), preset_gens_.size()));
        pdta.chunk("pmod", std::string(10, '\0'));
        pdta.chunk("pgen", generators(preset_gens_));
        pdta.chunk("inst", headers(instruments_, false, instrument_bags_.size()));
        pdta.chunk("ibag", bags(instrument_bags_, instrument_gens_.size()));
        pdta.chunk("imod", std::string(10, '\0'));
        pdta.chunk("igen", generators(instrument_gens_));
        pdta.chunk("shdr", sample_headers());

        Writer form;
        form.bytes("sfbk");
        form.list("INFO", info.take());
        form.list("sdta", sdta.take());
        form.list("pdta", pdta.take());
        Writer riff;
        riff.chunk("RIFF", form.take());
        return riff.take();
    }

private:
    // Preset headers (38 bytes each) or instrument headers (22), with the terminal record.
    [[nodiscard]] static std::string headers(std::span<const Named> named, bool presets,
                                             std::size_t bags) {
        Writer out;
        for (const Named& each : named) {
            out.name(each.name);
            if (presets) {
                out.u16(each.program);
                out.u16(each.bank);
            }
            out.u16(each.bag);
            if (presets) {
                out.u32(0);
                out.u32(0);
                out.u32(0);
            }
        }
        out.name(presets ? "EOP" : "EOI");
        if (presets) {
            out.u16(0);
            out.u16(0);
        }
        out.u16(static_cast<std::uint16_t>(bags));
        if (presets) {
            out.u32(0);
            out.u32(0);
            out.u32(0);
        }
        return out.take();
    }

    // Preset zones, one generator each.
    [[nodiscard]] static std::string bags(std::size_t count, std::size_t gens) {
        Writer out;
        for (std::size_t i = 0; i <= count; ++i) {
            out.u16(static_cast<std::uint16_t>(std::min(i, gens)));
            out.u16(0);
        }
        return out.take();
    }

    [[nodiscard]] static std::string bags(std::span<const std::uint16_t> first_gens,
                                          std::size_t gens) {
        Writer out;
        for (const std::uint16_t first : first_gens) {
            out.u16(first);
            out.u16(0);
        }
        out.u16(static_cast<std::uint16_t>(gens));
        out.u16(0);
        return out.take();
    }

    [[nodiscard]] static std::string generators(std::span<const Generator> gens) {
        Writer out;
        for (const Generator& gen : gens) {
            out.u16(gen.oper);
            out.u16(gen.amount);
        }
        out.u16(0);
        out.u16(0);
        return out.take();
    }

    [[nodiscard]] std::string sample_headers() const {
        Writer out;
        for (const SampleHeader& header : headers_) {
            out.name(header.name);
            out.u32(header.start);
            out.u32(header.end);
            out.u32(header.loop_start);
            out.u32(header.loop_end);
            out.u32(header.rate);
            out.u8(header.root);
            out.u8(0);  // pitch correction
            out.u16(0); // linked sample
            out.u16(1); // mono
        }
        out.name("EOS");
        for (int i = 0; i < 26; ++i) {
            out.u8(0);
        }
        return out.take();
    }

    std::vector<std::int16_t> samples_;
    std::vector<SampleHeader> headers_;
    std::vector<Named> instruments_;
    std::vector<std::uint16_t> instrument_bags_;
    std::vector<Generator> instrument_gens_;
    std::vector<Named> presets_;
    std::vector<Generator> preset_gens_;
};

constexpr std::string_view wave_name(Wave wave) {
    constexpr std::array<std::string_view, 10> kNames = {
        "sine", "triangle", "saw", "square", "pluck", "organ", "reed", "voice", "bell", "noise"};
    return kNames.at(static_cast<std::size_t>(wave));
}

constexpr std::string_view drum_sample_name(Drum drum) {
    constexpr std::array<std::string_view, 12> kNames = {"kick",  "snare",  "clap",  "hat",
                                                         "tom",   "crash",  "ride",  "cowbell",
                                                         "click", "shaker", "metal", "whistle"};
    return kNames.at(static_cast<std::size_t>(drum));
}

// A wave's samples by key range: rich harmonics for low notes, fewer up high, so no harmonic
// passes half the output rate.
struct Tier {
    int low;
    int high;
    int harmonics;
};
constexpr std::array<Tier, 3> kTiers = {{{0, 59, 32}, {60, 83, 12}, {84, 127, 4}}};

std::string build() {
    Bank bank;
    // Each wave's samples, one per tier, or one for a sine or noise.
    std::array<std::array<std::uint16_t, 3>, 10> waves{};
    for (int w = 0; w < 10; ++w) {
        const auto wave = static_cast<Wave>(w);
        auto& ids = waves.at(static_cast<std::size_t>(w));
        if (wave == Wave::Noise) {
            const std::vector<std::int16_t> data = looped_noise();
            ids.fill(bank.add_sample("noise", data, kDrumRate, 60,
                                     static_cast<int>(data.size()) - kGuard));
            continue;
        }
        for (std::size_t t = 0; t < kTiers.size(); ++t) {
            if (wave == Wave::Sine && t > 0) {
                ids.at(t) = ids.at(0);
                continue;
            }
            const std::vector<std::int16_t> data = cycle(wave, kTiers.at(t).harmonics);
            ids.at(t) = bank.add_sample(std::string(wave_name(wave)) + " " + std::to_string(t),
                                        data, kCycleRate, kCycleRoot, kCycle);
        }
    }

    const std::span<const std::string_view> names = instrument_names();
    for (int program = 0; program < 128; ++program) {
        const Voice v = voice(program);
        bank.begin_instrument(std::string(names[static_cast<std::size_t>(program)]));
        for (std::size_t t = 0; t < kTiers.size(); ++t) {
            std::vector<Generator> gens = {
                {kKeyRange, key_range(kTiers.at(t).low, kTiers.at(t).high)},
                {kAttackVolEnv, amount(timecents(v.attack))},
                {kReleaseVolEnv, amount(timecents(v.release))},
            };
            if (v.decay > 0.0) {
                gens.push_back({kDecayVolEnv, amount(timecents(v.decay))});
                gens.push_back({kSustainVolEnv, amount(v.sustain_cb)});
            }
            if (v.keynum_decay != 0) {
                gens.push_back({kKeynumToVolEnvDecay, amount(v.keynum_decay)});
            }
            if (v.filter_cents < 13500) {
                gens.push_back({kFilterFc, amount(v.filter_cents)});
            }
            if (v.vibrato_cents != 0) {
                // 5.5 Hz, after 0.3 seconds.
                gens.push_back({kVibLfoToPitch, amount(v.vibrato_cents)});
                gens.push_back({kFreqVibLfo, amount(-686)});
                gens.push_back({kDelayVibLfo, amount(timecents(0.3))});
            }
            gens.push_back({kSampleModes, 1});
            gens.push_back({kSampleId, waves.at(static_cast<std::size_t>(v.wave)).at(t)});
            bank.add_zone(gens);
        }
        bank.add_preset(std::string(names[static_cast<std::size_t>(program)]), program, 0,
                        static_cast<std::uint16_t>(bank.instrument_count() - 1));
    }

    std::array<std::uint16_t, 12> drums{};
    for (std::size_t d = 0; d < drums.size(); ++d) {
        const auto kind = static_cast<Drum>(d);
        drums.at(d) = bank.add_sample(std::string(drum_sample_name(kind)), drum(kind), kDrumRate,
                                      kDrumRoot, 0);
    }
    bank.begin_instrument("standard drums");
    for (std::size_t k = 0; k < kDrumKeys.size(); ++k) {
        const DrumKey& key = kDrumKeys.at(k);
        const int note = kFirstDrum + static_cast<int>(k);
        std::vector<Generator> gens = {
            {kKeyRange, key_range(note, note)},
            // At least 2 ms: TinySoundFont plays shorter attacks as none, so a drum on the
            // first beat of a loop would click where it joins.
            {kAttackVolEnv, amount(timecents(0.002))},
            // Drums ring out when their notes end, as General MIDI's do.
            {kReleaseVolEnv, amount(timecents(2.0))},
        };
        if (key.decay > 0.0) {
            gens.push_back({kDecayVolEnv, amount(timecents(key.decay))});
            gens.push_back({kSustainVolEnv, amount(960)});
        }
        if (key.exclusive != 0) {
            gens.push_back({kExclusiveClass, amount(key.exclusive)});
        }
        gens.push_back({kRootKey, amount(note - key.tune)});
        gens.push_back({kSampleId, drums.at(static_cast<std::size_t>(key.drum))});
        bank.add_zone(gens);
    }
    bank.add_preset("standard drums", 0, 128,
                    static_cast<std::uint16_t>(bank.instrument_count() - 1));
    return bank.bytes();
}

} // namespace

const std::string& builtin_sound_bank() {
    static const std::string bank = build();
    return bank;
}

} // namespace slash::soundtrack
