// Godot's AudioFilterSW (servers/audio/audio_filter_sw.cpp).

#include "filter.hpp"

#include <cmath>
#include <numbers>

namespace slash::audio {

void Filter::prepare_coefficients(Coeffs* coeffs) const {
    const int sr_limit = static_cast<int>(sampling_rate_ / 2) + 512;
    double final_cutoff = cutoff_ > static_cast<float>(sr_limit) ? static_cast<double>(sr_limit)
                                                                 : static_cast<double>(cutoff_);
    if (final_cutoff < 1) {
        final_cutoff = 1; // no less than this
    }
    const double tau = 2.0 * std::numbers::pi;
    double omega = tau * final_cutoff / sampling_rate_;
    const double sin_v = std::sin(omega);
    const double cos_v = std::cos(omega);

    double q = resonance_;
    if (q <= 0.0) {
        q = 0.0001;
    }
    if (mode_ == Mode::BandPass) {
        q *= 2.0;
    } else if (mode_ == Mode::Peak) {
        q *= 3.0;
    }
    double tmpgain = gain_;
    if (tmpgain < 0.001) {
        tmpgain = 0.001;
    }
    if (stages_ > 1) {
        q = q > 1.0 ? std::pow(q, 1.0 / stages_) : q;
        tmpgain = std::pow(tmpgain, 1.0 / (stages_ + 1));
    }
    double alpha = sin_v / (2 * q);
    double a0 = 1.0 + alpha;

    switch (mode_) {
    case Mode::LowPass:
        coeffs->b0 = (1.0 - cos_v) / 2.0;
        coeffs->b1 = 1.0 - cos_v;
        coeffs->b2 = (1.0 - cos_v) / 2.0;
        coeffs->a1 = -2.0 * cos_v;
        coeffs->a2 = 1.0 - alpha;
        break;
    case Mode::HighPass:
        coeffs->b0 = (1.0 + cos_v) / 2.0;
        coeffs->b1 = -(1.0 + cos_v);
        coeffs->b2 = (1.0 + cos_v) / 2.0;
        coeffs->a1 = -2.0 * cos_v;
        coeffs->a2 = 1.0 - alpha;
        break;
    case Mode::BandPass:
        coeffs->b0 = alpha * std::sqrt(q + 1);
        coeffs->b1 = 0.0;
        coeffs->b2 = -alpha * std::sqrt(q + 1);
        coeffs->a1 = -2.0 * cos_v;
        coeffs->a2 = 1.0 - alpha;
        break;
    case Mode::Notch:
        coeffs->b0 = 1.0;
        coeffs->b1 = -2.0 * cos_v;
        coeffs->b2 = 1.0;
        coeffs->a1 = -2.0 * cos_v;
        coeffs->a2 = 1.0 - alpha;
        break;
    case Mode::Peak:
        coeffs->b0 = (1.0 + alpha * tmpgain);
        coeffs->b1 = (-2.0 * cos_v);
        coeffs->b2 = (1.0 - alpha * tmpgain);
        coeffs->a1 = -2 * cos_v;
        coeffs->a2 = (1 - alpha / tmpgain);
        break;
    case Mode::BandLimit: {
        // This one is extra tricky.
        const double hicutoff = resonance_;
        const double centercutoff = (cutoff_ + resonance_) / 2.0;
        const double bandwidth = (std::log(centercutoff) - std::log(hicutoff)) / std::numbers::ln2;
        omega = tau * centercutoff / sampling_rate_;
        alpha = std::sin(omega) *
                std::sinh(std::numbers::ln2 / 2 * bandwidth * omega / std::sin(omega));
        a0 = 1 + alpha;
        coeffs->b0 = alpha;
        coeffs->b1 = 0;
        coeffs->b2 = -alpha;
        coeffs->a1 = -2 * std::cos(omega);
        coeffs->a2 = 1 - alpha;
        break;
    }
    case Mode::LowShelf: {
        double tmpq = std::sqrt(q);
        if (tmpq <= 0) {
            tmpq = 0.001;
        }
        const double beta = std::sqrt(tmpgain) / tmpq;
        a0 = (tmpgain + 1.0) + (tmpgain - 1.0) * cos_v + beta * sin_v;
        coeffs->b0 = tmpgain * ((tmpgain + 1.0) - (tmpgain - 1.0) * cos_v + beta * sin_v);
        coeffs->b1 = 2.0 * tmpgain * ((tmpgain - 1.0) - (tmpgain + 1.0) * cos_v);
        coeffs->b2 = tmpgain * ((tmpgain + 1.0) - (tmpgain - 1.0) * cos_v - beta * sin_v);
        coeffs->a1 = -2.0 * ((tmpgain - 1.0) + (tmpgain + 1.0) * cos_v);
        coeffs->a2 = ((tmpgain + 1.0) + (tmpgain - 1.0) * cos_v - beta * sin_v);
        break;
    }
    case Mode::HighShelf: {
        double tmpq = std::sqrt(q);
        if (tmpq <= 0) {
            tmpq = 0.001;
        }
        const double beta = std::sqrt(tmpgain) / tmpq;
        a0 = (tmpgain + 1.0) - (tmpgain - 1.0) * cos_v + beta * sin_v;
        coeffs->b0 = tmpgain * ((tmpgain + 1.0) + (tmpgain - 1.0) * cos_v + beta * sin_v);
        coeffs->b1 = -2.0 * tmpgain * ((tmpgain - 1.0) + (tmpgain + 1.0) * cos_v);
        coeffs->b2 = tmpgain * ((tmpgain + 1.0) + (tmpgain - 1.0) * cos_v - beta * sin_v);
        coeffs->a1 = 2.0 * ((tmpgain - 1.0) - (tmpgain + 1.0) * cos_v);
        coeffs->a2 = ((tmpgain + 1.0) - (tmpgain - 1.0) * cos_v - beta * sin_v);
        break;
    }
    }

    coeffs->b0 /= a0;
    coeffs->b1 /= a0;
    coeffs->b2 /= a0;
    coeffs->a1 /= 0.0 - a0;
    coeffs->a2 /= 0.0 - a0;
}

float Filter::response(float freq, const Coeffs& coeffs) const {
    const auto tau = static_cast<float>(2.0 * std::numbers::pi);
    freq = freq / sampling_rate_ * tau;
    auto cx = static_cast<float>(coeffs.b0);
    float cy = 0.0F;
    cx += static_cast<float>(std::cos(freq) * coeffs.b1);
    cy -= static_cast<float>(std::sin(freq) * coeffs.b1);
    cx += static_cast<float>(std::cos(2 * freq) * coeffs.b2);
    cy -= static_cast<float>(std::sin(2 * freq) * coeffs.b2);
    float h = cx * cx + cy * cy;
    cx = 1.0F;
    cy = 0.0F;
    cx -= static_cast<float>(std::cos(freq) * coeffs.a1);
    cy += static_cast<float>(std::sin(freq) * coeffs.a1);
    cx -= static_cast<float>(std::cos(2 * freq) * coeffs.a2);
    cy += static_cast<float>(std::sin(2 * freq) * coeffs.a2);
    h = h / (cx * cx + cy * cy);
    return h;
}

void Filter::Processor::set_filter(const Filter* filter, bool clear_history) {
    if (clear_history) {
        ha1_ = ha2_ = hb1_ = hb2_ = 0;
    }
    filter_ = filter;
}

void Filter::Processor::update_coeffs(int interp_buffer_len) {
    if (filter_ == nullptr) {
        return;
    }
    if (interp_buffer_len != 0) {
        const Coeffs old = coeffs_;
        filter_->prepare_coefficients(&coeffs_);
        incr_.a1 = (coeffs_.a1 - old.a1) / interp_buffer_len;
        incr_.a2 = (coeffs_.a2 - old.a2) / interp_buffer_len;
        incr_.b0 = (coeffs_.b0 - old.b0) / interp_buffer_len;
        incr_.b1 = (coeffs_.b1 - old.b1) / interp_buffer_len;
        incr_.b2 = (coeffs_.b2 - old.b2) / interp_buffer_len;
        coeffs_ = old;
    } else {
        filter_->prepare_coefficients(&coeffs_);
    }
}

} // namespace slash::audio
