#include "slash/audio/analysis.hpp"

#include "slash/core/math.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ebur128.h>
#include <limits>
#include <miniaudio.h>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace slash::audio {

namespace {

constexpr double kPi = std::numbers::pi;
// Quieter than -60 dBFS is silence.
constexpr float kSilence = 0.001F;
// Samples this close to full scale count toward clipping: a 16-bit sound's largest is 0.99997.
constexpr float kFullScale = 0.999F;
constexpr std::size_t kClipRun = 3;
constexpr double kMinGapSeconds = 0.5;
constexpr std::size_t kMaxGaps = 8;
// EBU R 128's absolute gate: nothing quieter counts.
constexpr double kGateLufs = -70.0;
// Tempo and key are measured on the sound low-passed and decimated to about this rate.
constexpr int kAnalysisRate = 11025;
// Tempos searched, in beats per minute, and the one the search leans toward, as most music is
// near it: a beat at 60 could as well be heard at 120.
constexpr double kSlowestBpm = 50.0;
constexpr double kFastestBpm = 240.0;
constexpr double kLikelyBpm = 120.0;
constexpr double kMinTempoSeconds = 3.0;
constexpr double kMinTempoConfidence = 0.25;
// Onsets on the beat must rise by this share of the sound's average compressed spectrum, or
// the "beat" is only a steady sound's ripple.
constexpr double kMinOnsetStrength = 0.01;
// Onsets are measured in octave bands from 150 Hz up, below it the first.
constexpr double kLowestBandHz = 150.0;
constexpr std::size_t kOnsetBands = 7;
// The bands below 300 Hz, where kicks and bass notes are, and how surely they must keep to the
// tempo to say where its beats fall.
constexpr std::size_t kLowBands = 2;
constexpr double kLowBeatConfidence = 0.5;
constexpr double kMinKeyConfidence = 0.4;
// Notes whose pitch classes count toward the key: A1 to A6.
constexpr double kLowestNoteHz = 55.0;
constexpr double kHighestNoteHz = 1760.0;
// A join clicks when its jump is this much bigger than the sound's own changes, and audible.
constexpr double kClickRatio = 4.0;
constexpr double kClickLevel = 0.005;

double to_db(double linear) {
    return 20.0 * std::log10(linear);
}

float sine(double radians) {
    return cos_sin(static_cast<float>(radians)).y;
}

float cosine(double radians) {
    return cos_sin(static_cast<float>(radians)).x;
}

// A radix-2 FFT of a power-of-two size, with a Hann window, its twiddles from slash::cos_sin
// so spectra are the same on every platform.
class Spectrum {
public:
    explicit Spectrum(std::size_t size)
        : size_(size), cos_(size / 2), sin_(size / 2), window_(size), reversed_(size), re_(size),
          im_(size) {
        const auto count = static_cast<double>(size);
        for (std::size_t k = 0; k < size / 2; ++k) {
            const double angle = -2.0 * kPi * static_cast<double>(k) / count;
            cos_[k] = cosine(angle);
            sin_[k] = sine(angle);
        }
        for (std::size_t n = 0; n < size; ++n) {
            window_[n] = 0.5 - (0.5 * cosine(2.0 * kPi * static_cast<double>(n) / count));
        }
        std::size_t bits = 0;
        while ((std::size_t{1} << bits) < size) {
            ++bits;
        }
        for (std::size_t i = 0; i < size; ++i) {
            std::size_t reversed = 0;
            for (std::size_t bit = 0; bit < bits; ++bit) {
                if (((i >> bit) & 1U) != 0) {
                    reversed |= std::size_t{1} << (bits - 1 - bit);
                }
            }
            reversed_[i] = reversed;
        }
    }

    [[nodiscard]] std::size_t size() const { return size_; }

    // The power of two sounds together at each frequency from 0 to half the rate (size / 2 + 1
    // bins), from one transform: left as the real part, right as the imaginary.
    void power(std::span<const float> left, std::span<const float> right,
               std::vector<double>& out) {
        for (std::size_t n = 0; n < size_; ++n) {
            re_[n] = static_cast<double>(left[n]) * window_[n];
            im_[n] = static_cast<double>(right[n]) * window_[n];
        }
        transform();
        out.resize(size_ / 2 + 1);
        for (std::size_t k = 0; k <= size_ / 2; ++k) {
            const std::size_t mirror = (size_ - k) % size_;
            out[k] = 0.5 * ((re_[k] * re_[k]) + (im_[k] * im_[k]) + (re_[mirror] * re_[mirror]) +
                            (im_[mirror] * im_[mirror]));
        }
    }

    // The power at each frequency from 0 to half the rate (size / 2 + 1 bins) of samples, which
    // are zero past their end.
    void power(std::span<const float> samples, std::vector<double>& out) {
        for (std::size_t n = 0; n < size_; ++n) {
            re_[n] = n < samples.size() ? static_cast<double>(samples[n]) * window_[n] : 0.0;
            im_[n] = 0.0;
        }
        transform();
        out.resize(size_ / 2 + 1);
        for (std::size_t k = 0; k <= size_ / 2; ++k) {
            out[k] = (re_[k] * re_[k]) + (im_[k] * im_[k]);
        }
    }

private:
    void transform() {
        for (std::size_t i = 0; i < size_; ++i) {
            if (i < reversed_[i]) {
                std::swap(re_[i], re_[reversed_[i]]);
                std::swap(im_[i], im_[reversed_[i]]);
            }
        }
        for (std::size_t length = 2; length <= size_; length <<= 1U) {
            const std::size_t half = length / 2;
            const std::size_t step = size_ / length;
            for (std::size_t start = 0; start < size_; start += length) {
                for (std::size_t k = 0; k < half; ++k) {
                    const double wr = cos_[k * step];
                    const double wi = sin_[k * step];
                    const std::size_t a = start + k;
                    const std::size_t b = a + half;
                    const double tr = (re_[b] * wr) - (im_[b] * wi);
                    const double ti = (re_[b] * wi) + (im_[b] * wr);
                    re_[b] = re_[a] - tr;
                    im_[b] = im_[a] - ti;
                    re_[a] += tr;
                    im_[a] += ti;
                }
            }
        }
    }

    std::size_t size_;
    std::vector<double> cos_;
    std::vector<double> sin_;
    std::vector<double> window_;
    std::vector<std::size_t> reversed_;
    std::vector<double> re_;
    std::vector<double> im_;
};

// The sound in mono, low-passed and decimated to about 11 kHz: enough to hear where notes start
// and which notes they are, at a quarter of the work.
struct Mono {
    std::vector<float> samples;
    double rate = 0.0;
};

Mono decimate(std::span<const Frame> frames, int mix_rate) {
    const int factor = std::max(1, mix_rate / kAnalysisRate);
    Mono mono;
    mono.rate = static_cast<double>(mix_rate) / factor;
    std::vector<float> mid(frames.size());
    for (std::size_t i = 0; i < frames.size(); ++i) {
        mid[i] = 0.5F * (frames[i].left + frames[i].right);
    }
    if (factor == 1) {
        mono.samples = std::move(mid);
        return mono;
    }
    // A windowed-sinc low-pass at 90% of the new Nyquist frequency, with a Blackman window.
    const int half = 8 * factor;
    const int taps = (2 * half) + 1;
    const double cutoff = 0.45 / factor; // cycles per input sample
    std::vector<float> kernel(static_cast<std::size_t>(taps));
    double sum = 0.0;
    for (int n = 0; n < taps; ++n) {
        const double x = 2.0 * cutoff * (n - half);
        const double sinc = n == half ? 1.0 : sine(kPi * x) / (kPi * x);
        const double phase = 2.0 * kPi * n / (taps - 1);
        const double blackman = 0.42 - (0.5 * cosine(phase)) + (0.08 * cosine(2.0 * phase));
        const double tap = sinc * blackman;
        kernel[static_cast<std::size_t>(n)] = static_cast<float>(tap);
        sum += tap;
    }
    for (float& tap : kernel) {
        tap = static_cast<float>(tap / sum);
    }
    const auto count = static_cast<std::int64_t>(mid.size());
    mono.samples.resize(static_cast<std::size_t>(count / factor));
    for (std::size_t j = 0; j < mono.samples.size(); ++j) {
        const std::int64_t centre = static_cast<std::int64_t>(j) * factor;
        double value = 0.0;
        for (int n = 0; n < taps; ++n) {
            const std::int64_t at = centre + n - half;
            if (at >= 0 && at < count) {
                value += static_cast<double>(kernel[static_cast<std::size_t>(n)]) *
                         mid[static_cast<std::size_t>(at)];
            }
        }
        mono.samples[j] = static_cast<float>(value);
    }
    return mono;
}

void measure_loudness(std::span<const Frame> frames, int mix_rate, Analysis& out) {
    ebur128_state* state =
        ebur128_init(2, static_cast<unsigned long>(mix_rate),
                     EBUR128_MODE_I | EBUR128_MODE_LRA | EBUR128_MODE_S | EBUR128_MODE_TRUE_PEAK);
    if (state == nullptr) {
        return;
    }
    // In tenths of a second, reading the short-term loudness after each once three seconds are
    // in, as a meter does.
    const auto chunk = static_cast<std::size_t>(std::max(1, mix_rate / 10));
    const std::size_t three_seconds = 3 * static_cast<std::size_t>(mix_rate);
    std::vector<float> interleaved(chunk * 2);
    double loudest = -std::numeric_limits<double>::infinity();
    for (std::size_t at = 0; at < frames.size(); at += chunk) {
        const std::size_t count = std::min(chunk, frames.size() - at);
        for (std::size_t i = 0; i < count; ++i) {
            interleaved[i * 2] = frames[at + i].left;
            interleaved[(i * 2) + 1] = frames[at + i].right;
        }
        ebur128_add_frames_float(state, interleaved.data(), count);
        double short_term = 0.0;
        if (at + count >= three_seconds &&
            ebur128_loudness_shortterm(state, &short_term) == EBUR128_SUCCESS) {
            loudest = std::max(loudest, short_term);
        }
    }
    double integrated = 0.0;
    if (ebur128_loudness_global(state, &integrated) == EBUR128_SUCCESS && integrated > kGateLufs) {
        out.lufs = integrated;
        double range = 0.0;
        if (ebur128_loudness_range(state, &range) == EBUR128_SUCCESS) {
            out.loudness_range = range;
        }
    }
    if (loudest > kGateLufs) {
        out.max_short_term_lufs = loudest;
    }
    double peak = 0.0;
    for (unsigned channel = 0; channel < 2; ++channel) {
        double channel_peak = 0.0;
        if (ebur128_true_peak(state, channel, &channel_peak) == EBUR128_SUCCESS) {
            peak = std::max(peak, channel_peak);
        }
    }
    if (peak > 0.0) {
        out.true_peak_db = to_db(peak);
    }
    ebur128_destroy(&state);
}

void measure_levels(std::span<const Frame> frames, int mix_rate, Analysis& out) {
    double peak = 0.0;
    double squares = 0.0;
    // A clip is a run of frames at full scale three long or more, or with a sample past it,
    // which the output clips.
    std::size_t run = 0;
    bool past = false;
    std::int64_t first_clip = -1;
    const auto end_run = [&](std::size_t at) {
        if (run >= kClipRun || past) {
            ++out.clips;
            if (first_clip < 0) {
                first_clip = static_cast<std::int64_t>(at - run);
            }
        }
        run = 0;
        past = false;
    };
    for (std::size_t i = 0; i < frames.size(); ++i) {
        const float left = std::abs(frames[i].left);
        const float right = std::abs(frames[i].right);
        const float loudest = std::max(left, right);
        peak = std::max(peak, static_cast<double>(loudest));
        squares += (static_cast<double>(frames[i].left) * frames[i].left) +
                   (static_cast<double>(frames[i].right) * frames[i].right);
        if (loudest >= kFullScale) {
            ++run;
            past = past || loudest > 1.0F;
        } else if (run > 0) {
            end_run(i);
        }
    }
    if (run > 0) {
        end_run(frames.size());
    }
    if (first_clip >= 0) {
        out.first_clip = static_cast<double>(first_clip) / mix_rate;
    }
    if (peak > 0.0) {
        out.peak_db = to_db(peak);
        out.rms_db = to_db(std::sqrt(squares / (2.0 * static_cast<double>(frames.size()))));
    }

    // Silence, in windows of a hundredth of a second.
    const auto window = static_cast<std::size_t>(std::max(1, mix_rate / 100));
    const std::size_t windows = (frames.size() + window - 1) / window;
    std::vector<bool> silent(windows);
    for (std::size_t w = 0; w < windows; ++w) {
        float loudest = 0.0F;
        for (std::size_t i = w * window; i < std::min(frames.size(), (w + 1) * window); ++i) {
            loudest = std::max({loudest, std::abs(frames[i].left), std::abs(frames[i].right)});
        }
        silent[w] = loudest < kSilence;
    }
    const auto seconds_at = [&](std::size_t w) {
        return static_cast<double>(std::min(frames.size(), w * window)) / mix_rate;
    };
    std::size_t first = 0;
    while (first < windows && silent[first]) {
        ++first;
    }
    if (first == windows) {
        out.silence_start = out.seconds;
        out.silence_end = out.seconds;
        return;
    }
    std::size_t last = windows;
    while (last > first && silent[last - 1]) {
        --last;
    }
    out.silence_start = seconds_at(first);
    out.silence_end = out.seconds - seconds_at(last);
    const auto min_gap = static_cast<std::size_t>(kMinGapSeconds * 100.0);
    for (std::size_t w = first; w < last && out.gaps.size() < kMaxGaps;) {
        if (!silent[w]) {
            ++w;
            continue;
        }
        std::size_t end = w;
        while (end < last && silent[end]) {
            ++end;
        }
        if (end - w >= min_gap) {
            out.gaps.emplace_back(seconds_at(w), seconds_at(end));
        }
        w = end;
    }
}

// Linear interpolation into values at a fractional index; 0 past either end.
double sample_at(const std::vector<double>& values, double index) {
    if (index < 0.0) {
        return 0.0;
    }
    const auto low = static_cast<std::size_t>(index);
    if (low + 1 >= values.size()) {
        return low < values.size() ? values[low] : 0.0;
    }
    const double t = index - static_cast<double>(low);
    return (values[low] * (1.0 - t)) + (values[low + 1] * t);
}

struct Comb {
    double score = 0.0;
    double phase = 0.0; // envelope frames
};

// The mean of values at every period from phase.
double comb_at(const std::vector<double>& values, double period, double phase) {
    const auto last = static_cast<double>(values.size() - 1);
    double sum = 0.0;
    int beats = 0;
    for (; phase + (beats * period) <= last; ++beats) {
        sum += sample_at(values, phase + (beats * period));
    }
    return beats > 0 ? sum / beats : 0.0;
}

// Onsets less their local average, so only rises stand out, then smoothed with a triangle 20 ms
// either side, so beats that drift off a tempo's grid lose score gradually.
std::vector<double> onset_envelope(const std::vector<double>& onsets, double fps) {
    const std::size_t frames = onsets.size();
    const auto reach = static_cast<std::size_t>(std::max(1.0, std::round(fps * 0.1)));
    std::vector<double> prefix(frames + 1, 0.0);
    for (std::size_t t = 0; t < frames; ++t) {
        prefix[t + 1] = prefix[t] + onsets[t];
    }
    std::vector<double> rises(frames, 0.0);
    for (std::size_t t = 0; t < frames; ++t) {
        const std::size_t from = t > reach ? t - reach : 0;
        const std::size_t to = std::min(frames, t + reach + 1);
        const double average = (prefix[to] - prefix[from]) / static_cast<double>(to - from);
        rises[t] = std::max(0.0, onsets[t] - average);
    }
    const auto spread = static_cast<std::ptrdiff_t>(std::max(1.0, std::round(fps * 0.02)));
    const auto norm = static_cast<double>((spread + 1) * (spread + 1));
    std::vector<double> envelope(frames, 0.0);
    for (std::size_t t = 0; t < frames; ++t) {
        double value = 0.0;
        for (std::ptrdiff_t k = -spread; k <= spread; ++k) {
            const std::ptrdiff_t at = static_cast<std::ptrdiff_t>(t) + k;
            if (at >= 0 && at < static_cast<std::ptrdiff_t>(frames)) {
                value += static_cast<double>(spread + 1 - std::abs(k)) / norm *
                         rises[static_cast<std::size_t>(at)];
            }
        }
        envelope[t] = value;
    }
    return envelope;
}

// How strongly beats every period frames line up with the onset envelope, at the best phase.
Comb comb(const std::vector<double>& envelope, double period) {
    Comb best;
    constexpr double kPhaseStep = 0.25;
    const auto phases = static_cast<int>(std::ceil(period / kPhaseStep));
    for (int step = 0; step < phases; ++step) {
        const double phase = step * kPhaseStep;
        const double score = comb_at(envelope, period, phase);
        if (score > best.score) {
            best = {score, phase};
        }
    }
    return best;
}

void measure_tempo(const Mono& mono, Analysis& out) {
    const std::size_t window = mono.rate >= 8000.0 ? 256 : 128;
    const auto hop = static_cast<std::size_t>(std::max(1.0, std::floor(mono.rate / 200.0)));
    const double fps = mono.rate / static_cast<double>(hop);
    if (out.seconds < kMinTempoSeconds || mono.samples.size() < window + (2 * hop)) {
        return;
    }
    // Where notes start: how much the compressed spectrum grows from one frame to the next, in
    // octave-wide bands. Each band with sound in it counts the same, so a kick and a bass note
    // weigh as much as a hi-hat's noise across every high bin.
    const std::size_t frames = ((mono.samples.size() - window) / hop) + 1;
    std::vector<std::size_t> band_of(window / 2 + 1, 0);
    std::size_t bands = 0;
    for (std::size_t k = 1; k <= window / 2; ++k) {
        const double hz = static_cast<double>(k) * mono.rate / static_cast<double>(window);
        std::size_t band = 0;
        for (double edge = kLowestBandHz; hz >= edge && band + 1 < kOnsetBands; edge *= 2.0) {
            ++band;
        }
        band_of[k] = band;
        bands = std::max(bands, band + 1);
    }
    Spectrum spectrum(window);
    std::vector<double> power;
    std::vector<double> previous(window / 2 + 1, 0.0);
    std::vector<std::vector<double>> band_flux(bands, std::vector<double>(frames, 0.0));
    std::vector<double> band_level(bands, 0.0);
    std::vector<double> total(frames, 0.0);
    double level = 0.0;
    for (std::size_t t = 0; t < frames; ++t) {
        spectrum.power(std::span<const float>(mono.samples).subspan(t * hop, window), power);
        for (std::size_t k = 1; k < power.size(); ++k) {
            const double compressed = std::sqrt(std::sqrt(power[k]));
            band_level[band_of[k]] += compressed;
            if (t > 0) {
                const double rise = std::max(0.0, compressed - previous[k]);
                band_flux[band_of[k]][t] += rise;
                total[t] += rise;
            }
            previous[k] = compressed;
        }
    }
    for (const double each : band_level) {
        level += each;
    }
    level /= static_cast<double>(frames);
    const double loudest_band = *std::max_element(band_level.begin(), band_level.end());
    std::vector<double> onsets(frames, 0.0);
    std::vector<double> low_onsets(frames, 0.0);
    for (std::size_t b = 0; b < bands; ++b) {
        double sum = 0.0;
        for (const double flux : band_flux[b]) {
            sum += flux;
        }
        // A band a thousand times quieter than the loudest holds only leakage.
        if (band_level[b] < 1e-3 * loudest_band || sum <= 0.0) {
            continue;
        }
        const double mean_flux = sum / static_cast<double>(frames);
        for (std::size_t t = 0; t < frames; ++t) {
            onsets[t] += band_flux[b][t] / mean_flux;
            if (b < kLowBands) {
                low_onsets[t] += band_flux[b][t] / mean_flux;
            }
        }
    }
    const std::vector<double> envelope = onset_envelope(onsets, fps);
    double mean = 0.0;
    for (const double value : envelope) {
        mean += value;
    }
    mean /= static_cast<double>(frames);
    if (mean <= 1e-9 || level <= 0.0) {
        return;
    }

    // The period: the autocorrelation's strongest lag, leaning toward kLikelyBpm.
    const auto shortest =
        static_cast<std::size_t>(std::max(1.0, std::floor(fps * 60.0 / kFastestBpm)));
    const auto longest =
        std::min(frames / 2, static_cast<std::size_t>(std::ceil(fps * 60.0 / kSlowestBpm)));
    if (longest < shortest + 2) {
        return;
    }
    std::vector<double> weighted(longest + 2, 0.0);
    std::size_t best_lag = 0;
    for (std::size_t lag = shortest; lag <= longest; ++lag) {
        double sum = 0.0;
        for (std::size_t t = 0; t + lag < frames; ++t) {
            sum += envelope[t] * envelope[t + lag];
        }
        const double correlation = sum / static_cast<double>(frames - lag);
        const double octaves = std::log2(fps * 60.0 / static_cast<double>(lag) / kLikelyBpm);
        weighted[lag] = correlation * std::exp(-0.5 * octaves * octaves);
        if (best_lag == 0 || weighted[lag] > weighted[best_lag]) {
            best_lag = lag;
        }
    }
    auto lag = static_cast<double>(best_lag);
    if (best_lag > shortest && best_lag < longest) {
        const double before = weighted[best_lag - 1];
        const double at = weighted[best_lag];
        const double after = weighted[best_lag + 1];
        const double curve = before - (2.0 * at) + after;
        if (curve < 0.0) {
            lag += 0.5 * (before - after) / curve;
        }
    }
    const double rough = fps * 60.0 / lag;

    // Then the tempo and phase whose beats line up best with the onsets, coarse then fine.
    const auto search = [&](double from, double to, double step, double& bpm, Comb& best) {
        const auto steps = static_cast<int>(std::floor(((to - from) / step) + 1e-6));
        for (int i = 0; i <= steps; ++i) {
            const double candidate = from + (i * step);
            const Comb found = comb(envelope, fps * 60.0 / candidate);
            if (found.score > best.score) {
                best = found;
                bpm = candidate;
            }
        }
    };
    double bpm = rough;
    Comb best;
    search(rough * 0.98, rough * 1.02, 0.05, bpm, best);
    search(bpm - 0.05, bpm + 0.05, 0.005, bpm, best);
    // Music is usually written at a whole tempo: take it when it lines up as well.
    const double whole = std::round(bpm);
    if (std::abs(whole - bpm) <= 0.15) {
        const Comb found = comb(envelope, fps * 60.0 / whole);
        if (found.score >= 0.995 * best.score) {
            bpm = whole;
            best = found;
        }
    }
    const double confidence = std::clamp(1.0 - (mean / best.score), 0.0, 1.0);
    const std::vector<double> raw = onset_envelope(total, fps);
    const double strength = comb_at(raw, fps * 60.0 / bpm, best.phase) / level;
    if (confidence < kMinTempoConfidence || strength < kMinOnsetStrength) {
        return;
    }
    out.bpm = bpm;
    out.tempo_confidence = confidence;
    // Kicks and bass notes mark the beat, more than hi-hats between beats: where the lows keep
    // to the tempo, their phase is the beat's.
    const std::vector<double> low = onset_envelope(low_onsets, fps);
    double low_mean = 0.0;
    for (const double value : low) {
        low_mean += value;
    }
    low_mean /= static_cast<double>(frames);
    const Comb low_best = comb(low, fps * 60.0 / bpm);
    if (low_best.score > 0.0 && 1.0 - (low_mean / low_best.score) >= kLowBeatConfidence) {
        best.phase = low_best.phase;
    }
    // An onset rises fastest in the frame whose window it is three quarters into, where a Hann
    // window grows steepest.
    const double beat = 60.0 / bpm;
    double offset =
        ((best.phase * static_cast<double>(hop)) + (0.75 * static_cast<double>(window))) /
        mono.rate;
    offset = std::fmod(offset, beat);
    if (beat - offset < 0.5 / fps) {
        offset = 0.0;
    }
    out.beat_offset = offset;
}

// Krumhansl and Kessler's probe-tone ratings of each pitch class above a major or minor tonic.
constexpr std::array<double, 12> kMajorProfile{6.35, 2.23, 3.48, 2.33, 4.38, 4.09,
                                               2.52, 5.19, 2.39, 3.66, 2.29, 2.88};
constexpr std::array<double, 12> kMinorProfile{6.33, 2.68, 3.52, 5.38, 2.60, 3.53,
                                               2.54, 4.75, 3.98, 2.69, 3.34, 3.17};
constexpr std::array<std::string_view, 12> kPitchNames{"C",  "C#", "D",  "Eb", "E",  "F",
                                                       "F#", "G",  "Ab", "A",  "Bb", "B"};

// Pearson's correlation between chroma and a profile turned to a tonic.
double correlation(const std::array<double, 12>& chroma, const std::array<double, 12>& profile,
                   std::size_t tonic) {
    double chroma_mean = 0.0;
    double profile_mean = 0.0;
    for (std::size_t i = 0; i < 12; ++i) {
        chroma_mean += chroma[i];
        profile_mean += profile[i];
    }
    chroma_mean /= 12.0;
    profile_mean /= 12.0;
    double both = 0.0;
    double chroma_squares = 0.0;
    double profile_squares = 0.0;
    for (std::size_t i = 0; i < 12; ++i) {
        const double a = chroma[i] - chroma_mean;
        const double b = profile[(i + 12 - tonic) % 12] - profile_mean;
        both += a * b;
        chroma_squares += a * a;
        profile_squares += b * b;
    }
    const double scale = std::sqrt(chroma_squares * profile_squares);
    return scale > 0.0 ? both / scale : 0.0;
}

void measure_key(const Mono& mono, Analysis& out) {
    std::size_t window = 1024;
    while (static_cast<double>(window * 2) <= mono.rate / 2.0) {
        window *= 2;
    }
    if (mono.samples.size() < window / 2) {
        return;
    }
    // Each bin's pitch class, for the bins between the lowest and highest notes counted.
    std::vector<int> pitch_class(window / 2 + 1, -1);
    for (std::size_t k = 1; k <= window / 2; ++k) {
        const double hz = static_cast<double>(k) * mono.rate / static_cast<double>(window);
        if (hz >= kLowestNoteHz && hz <= kHighestNoteHz) {
            const double note = 69.0 + (12.0 * std::log2(hz / 440.0));
            pitch_class[k] = static_cast<int>(std::lround(note)) % 12;
        }
    }
    Spectrum spectrum(window);
    std::vector<double> power;
    std::array<double, 12> chroma{};
    const std::size_t hop = window / 2;
    const std::span<const float> samples(mono.samples);
    for (std::size_t at = 0; at == 0 || at + window <= samples.size(); at += hop) {
        spectrum.power(samples.subspan(at, std::min(window, samples.size() - at)), power);
        for (std::size_t k = 0; k < power.size(); ++k) {
            if (pitch_class[k] >= 0) {
                chroma[static_cast<std::size_t>(pitch_class[k])] += std::sqrt(power[k]);
            }
        }
    }
    double best = -1.0;
    for (std::size_t tonic = 0; tonic < 12; ++tonic) {
        for (const bool minor : {false, true}) {
            const double r = correlation(chroma, minor ? kMinorProfile : kMajorProfile, tonic);
            if (r > best) {
                best = r;
                out.key = std::string(kPitchNames[tonic]) + (minor ? " minor" : " major");
            }
        }
    }
    if (best < kMinKeyConfidence) {
        out.key.reset();
        return;
    }
    out.key_confidence = best;
}

void measure_bands(std::span<const Frame> frames, int mix_rate, Analysis& out) {
    const std::size_t window = mix_rate > 24000 ? 4096 : 2048;
    Spectrum spectrum(window);
    std::vector<float> left(window);
    std::vector<float> right(window);
    std::vector<double> power;
    std::vector<double> total_power(window / 2 + 1, 0.0);
    for (std::size_t at = 0; at == 0 || at + window <= frames.size(); at += window) {
        const std::size_t count = std::min(window, frames.size() - at);
        for (std::size_t i = 0; i < window; ++i) {
            left[i] = i < count ? frames[at + i].left : 0.0F;
            right[i] = i < count ? frames[at + i].right : 0.0F;
        }
        spectrum.power(left, right, power);
        for (std::size_t k = 0; k < power.size(); ++k) {
            total_power[k] += power[k];
        }
    }
    double total = 0.0;
    std::array<double, kOctaveBands.size()> bands{};
    for (std::size_t k = 1; k < total_power.size(); ++k) {
        total += total_power[k];
        const double hz = static_cast<double>(k) * mix_rate / static_cast<double>(window);
        for (std::size_t b = 0; b < kOctaveBands.size(); ++b) {
            if (hz >= kOctaveBands[b][0] && hz < kOctaveBands[b][1]) {
                bands[b] += total_power[k];
            }
        }
    }
    if (total <= 0.0) {
        return;
    }
    // A hundred-millionth of the energy and less is none.
    constexpr double kLeast = 1e-10;
    for (std::size_t b = 0; b < kOctaveBands.size(); ++b) {
        const double share = bands[b] / total;
        if (share > kLeast) {
            out.bands_db[b] = 10.0 * std::log10(share);
        }
    }
}

double rms(std::span<const Frame> frames) {
    double squares = 0.0;
    for (const Frame& frame : frames) {
        squares += (static_cast<double>(frame.left) * frame.left) +
                   (static_cast<double>(frame.right) * frame.right);
    }
    return frames.empty() ? 0.0 : std::sqrt(squares / (2.0 * static_cast<double>(frames.size())));
}

std::optional<Analysis::Loop> measure_loop(std::span<const Frame> frames, int mix_rate,
                                           LoopPoints loop, std::optional<double> bpm) {
    const auto size = static_cast<std::int64_t>(frames.size());
    const std::int64_t start = std::clamp<std::int64_t>(loop.start, 0, size);
    const std::int64_t end = std::clamp<std::int64_t>(loop.end, 0, size);
    if (end - start < std::max(4, mix_rate / 20)) {
        return std::nullopt;
    }
    // The sound as played around the join: what comes before it, then the loop's start with what
    // followed the end fading out over it.
    const auto played_after = [&](std::int64_t k) {
        Frame frame = frames[static_cast<std::size_t>(start + k)];
        if (k < loop.crossfade && end + k < size) {
            const float weight =
                static_cast<float>(loop.crossfade - k) / static_cast<float>(loop.crossfade);
            frame.left += frames[static_cast<std::size_t>(end + k)].left * weight;
            frame.right += frames[static_cast<std::size_t>(end + k)].right * weight;
        }
        return frame;
    };
    // Levels are compared over a beat each side (a quarter second with no tempo), and the jump
    // against the 10 ms each side.
    const double level_seconds = bpm ? 60.0 / *bpm : 0.25;
    const std::int64_t level_frames = std::min<std::int64_t>(
        (end - start) / 2, static_cast<std::int64_t>(level_seconds * mix_rate));
    const std::int64_t near = std::min<std::int64_t>(level_frames, std::max(8, mix_rate / 100));
    std::vector<Frame> joined;
    joined.reserve(static_cast<std::size_t>(2 * level_frames));
    for (std::int64_t i = end - level_frames; i < end; ++i) {
        joined.push_back(frames[static_cast<std::size_t>(i)]);
    }
    const auto seam = static_cast<std::size_t>(level_frames);
    for (std::int64_t k = 0; k < level_frames; ++k) {
        joined.push_back(played_after(k));
    }

    Analysis::Loop result;
    result.seconds = static_cast<double>(end - start) / mix_rate;
    const std::span<const Frame> all(joined);
    const double before = rms(all.subspan(0, seam));
    const double after = rms(all.subspan(seam));
    constexpr double kMostChange = 60.0;
    if (before > 0.0 && after > 0.0) {
        result.level_change_db = std::clamp(to_db(after / before), -kMostChange, kMostChange);
    } else if (before > 0.0 || after > 0.0) {
        result.level_change_db = after > 0.0 ? kMostChange : -kMostChange;
    }
    // The jump across the join against the sound's own sample to sample changes near it.
    const auto difference = [&](std::size_t i, int c) {
        return c == 0 ? static_cast<double>(joined[i].left) - joined[i - 1].left
                      : static_cast<double>(joined[i].right) - joined[i - 1].right;
    };
    double step = 0.0;
    double squares = 0.0;
    int count = 0;
    const auto from = static_cast<std::size_t>(level_frames - near) + 1;
    const auto to = seam + static_cast<std::size_t>(near);
    for (int c = 0; c < 2; ++c) {
        step = std::max(step, std::abs(difference(seam, c)));
        for (std::size_t i = from; i < to; ++i) {
            if (i != seam) {
                squares += difference(i, c) * difference(i, c);
                ++count;
            }
        }
    }
    if (step > 0.0) {
        result.step_db = to_db(step);
    }
    const double typical = count > 0 ? std::sqrt(squares / count) : 0.0;
    result.clicks = step > kClickLevel && step > kClickRatio * typical;
    return result;
}

} // namespace

Analysis analyze(std::span<const Frame> frames, int mix_rate, std::optional<LoopPoints> loop) {
    Analysis out;
    if (mix_rate <= 0 || frames.empty()) {
        return out;
    }
    out.seconds = static_cast<double>(frames.size()) / mix_rate;
    measure_levels(frames, mix_rate, out);
    if (!out.peak_db) {
        return out; // silence: nothing more to measure
    }
    measure_loudness(frames, mix_rate, out);
    const Mono mono = decimate(frames, mix_rate);
    measure_tempo(mono, out);
    measure_key(mono, out);
    measure_bands(frames, mix_rate, out);
    if (loop) {
        out.loop = measure_loop(frames, mix_rate, *loop, out.bpm);
    }
    return out;
}

Result<std::vector<Frame>> decode_frames(const Sound& sound) {
    std::vector<Frame> frames;
    const bool stereo = sound.channels == 2;
    if (sound.format == Sound::Format::Wav) {
        frames.resize(static_cast<std::size_t>(sound.frames));
        for (std::size_t i = 0; i < frames.size(); ++i) {
            const std::size_t at = stereo ? i * 2 : i;
            frames[i] = {sound.samples[at], sound.samples[stereo ? at + 1 : at]};
        }
        return frames;
    }
    ma_decoder_config config =
        ma_decoder_config_init(ma_format_f32, static_cast<ma_uint32>(sound.channels), 0);
    config.encodingFormat =
        sound.format == Sound::Format::Vorbis ? ma_encoding_format_vorbis : ma_encoding_format_mp3;
    ma_decoder decoder;
    if (ma_decoder_init_memory(sound.data.data(), sound.data.size(), &config, &decoder) !=
        MA_SUCCESS) {
        return make_error(ErrorCode::InvalidArgument, "the sound could not be decoded");
    }
    frames.reserve(static_cast<std::size_t>(sound.frames));
    constexpr ma_uint64 kChunk = 4096;
    std::vector<float> chunk(kChunk * 2);
    for (;;) {
        ma_uint64 read = 0;
        const ma_result result = ma_decoder_read_pcm_frames(&decoder, chunk.data(), kChunk, &read);
        for (std::size_t i = 0; i < static_cast<std::size_t>(read); ++i) {
            const std::size_t at = stereo ? i * 2 : i;
            frames.push_back({chunk[at], chunk[stereo ? at + 1 : at]});
        }
        if (result != MA_SUCCESS || read < kChunk) {
            break;
        }
    }
    ma_decoder_uninit(&decoder);
    return frames;
}

} // namespace slash::audio
