#pragma once

#include <cstdint>

namespace slash::audio {

// Godot's AudioFilterSW (servers/audio/audio_filter_sw.h): a biquad filter's coefficients from a
// mode, cutoff, resonance and gain, and a processor that runs one channel through them.
class Filter {
public:
    struct Coeffs {
        double a1 = 0.0;
        double a2 = 0.0;
        double b0 = 0.0;
        double b1 = 0.0;
        double b2 = 0.0;
    };

    enum class Mode : std::uint8_t {
        BandPass,
        HighPass,
        LowPass,
        Notch,
        Peak,
        BandLimit,
        LowShelf,
        HighShelf
    };

    // One channel's history.
    class Processor {
    public:
        void set_filter(const Filter* filter, bool clear_history = true);
        // With a length, the coefficients move from the old ones to the new over that many
        // samples, which process_one_interp steps through.
        void update_coeffs(int interp_buffer_len = 0);

        void process_one(float& sample) {
            const float pre = sample;
            sample = static_cast<float>(sample * coeffs_.b0 + hb1_ * coeffs_.b1 +
                                        hb2_ * coeffs_.b2 + ha1_ * coeffs_.a1 + ha2_ * coeffs_.a2);
            ha2_ = ha1_;
            hb2_ = hb1_;
            hb1_ = pre;
            ha1_ = sample;
        }

        void process_one_interp(float& sample) {
            process_one(sample);
            coeffs_.b0 += incr_.b0;
            coeffs_.b1 += incr_.b1;
            coeffs_.b2 += incr_.b2;
            coeffs_.a1 += incr_.a1;
            coeffs_.a2 += incr_.a2;
        }

    private:
        const Filter* filter_ = nullptr;
        Coeffs coeffs_;
        float ha1_ = 0.0F;
        float ha2_ = 0.0F;
        float hb1_ = 0.0F;
        float hb2_ = 0.0F;
        Coeffs incr_;
    };

    void set_mode(Mode mode) { mode_ = mode; }
    void set_cutoff(float cutoff) { cutoff_ = cutoff; }
    void set_resonance(float resonance) { resonance_ = resonance; }
    void set_gain(float gain) { gain_ = gain; }
    void set_sampling_rate(float rate) { sampling_rate_ = rate; }
    void set_stages(int stages) { stages_ = stages; }

    void prepare_coefficients(Coeffs* coeffs) const;
    // The filter's power at a frequency, as Godot's get_response.
    [[nodiscard]] float response(float freq, const Coeffs& coeffs) const;

private:
    float cutoff_ = 5000.0F;
    float resonance_ = 0.5F;
    float gain_ = 1.0F;
    float sampling_rate_ = 44100.0F;
    int stages_ = 1;
    Mode mode_ = Mode::LowPass;
};

} // namespace slash::audio
