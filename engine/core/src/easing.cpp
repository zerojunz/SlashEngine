// Godot's easing equations (scene/animation/easing_equations.h), with its sines, powers and
// exponentials through slash::cos_sin, exp and ln. Derived, as Godot's are, from Robert Penner's
// easing equations (http://robertpenner.com/easing/):
//
// Copyright (c) 2001 Robert Penner
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software
// and associated documentation files (the "Software"), to deal in the Software without
// restriction, including without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all copies or
// substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
// BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

#include "slash/core/easing.hpp"

#include "slash/core/math.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>

namespace slash {
namespace {

constexpr float kPi = std::numbers::pi_v<float>;

float sin_of(float x) {
    return cos_sin(x).y;
}

float cos_of(float x) {
    return cos_sin(x).x;
}

// std::pow(2, x).
float exp2_of(float x) {
    return slash::exp(x * std::numbers::ln2_v<float>);
}

// std::pow(x, y) for x of 0 or more.
float pow_of(float x, float y) {
    return x <= 0.0F ? 0.0F : slash::exp(y * slash::ln(x));
}

float power(float x, int n) {
    float out = 1.0F;
    for (int i = 0; i < n; ++i) {
        out *= x;
    }
    return out;
}

using Equation = float (*)(float t, float b, float c, float d);

template <Equation Out, Equation In>
float out_in(float t, float b, float c, float d) {
    if (t < d / 2) {
        return Out(t * 2, b, c / 2, d);
    }
    const float h = c / 2;
    return In((t * 2) - d, b + h, h, d);
}

template <Equation In, Equation Out>
float in_out_halves(float t, float b, float c, float d) {
    if (t < d / 2) {
        return In(t * 2, b, c / 2, d);
    }
    const float h = c / 2;
    return Out((t * 2) - d, b + h, h, d);
}

float linear(float t, float b, float c, float d) {
    return (c * t / d) + b;
}

float sine_in(float t, float b, float c, float d) {
    return (-c * cos_of(t / d * (kPi / 2))) + c + b;
}
float sine_out(float t, float b, float c, float d) {
    return (c * sin_of(t / d * (kPi / 2))) + b;
}
float sine_in_out(float t, float b, float c, float d) {
    return (-c / 2 * (cos_of(kPi * t / d) - 1)) + b;
}

template <int N>
float poly_in(float t, float b, float c, float d) {
    return (c * power(t / d, N)) + b;
}
template <int N>
float poly_out(float t, float b, float c, float d) {
    if constexpr (N % 2 == 1) {
        return (c * (power((t / d) - 1, N) + 1)) + b;
    } else {
        return (-c * (power((t / d) - 1, N) - 1)) + b;
    }
}
template <int N>
float poly_in_out(float t, float b, float c, float d) {
    t = t / d * 2;
    if (t < 1) {
        return (c / 2 * power(t, N)) + b;
    }
    if constexpr (N % 2 == 1) {
        return (c / 2 * (power(t - 2, N) + 2)) + b;
    } else {
        return (-c / 2 * (power(t - 2, N) - 2)) + b;
    }
}

// Godot's Quad::out and in_out, written as Penner wrote them.
float quad_out(float t, float b, float c, float d) {
    t /= d;
    return (-c * t * (t - 2)) + b;
}
float quad_in_out(float t, float b, float c, float d) {
    t = t / d * 2;
    if (t < 1) {
        return (c / 2 * power(t, 2)) + b;
    }
    return (-c / 2 * (((t - 1) * (t - 3)) - 1)) + b;
}

float cubic_in(float t, float b, float c, float d) {
    t /= d;
    return (c * t * t * t) + b;
}
float cubic_out(float t, float b, float c, float d) {
    t = (t / d) - 1;
    return (c * ((t * t * t) + 1)) + b;
}
float cubic_in_out(float t, float b, float c, float d) {
    t /= d / 2;
    if (t < 1) {
        return (c / 2 * t * t * t) + b;
    }
    t -= 2;
    return (c / 2 * ((t * t * t) + 2)) + b;
}

float expo_in(float t, float b, float c, float d) {
    if (t == 0) {
        return b;
    }
    return (c * exp2_of(10 * ((t / d) - 1))) + b - (c * 0.001F);
}
float expo_out(float t, float b, float c, float d) {
    if (t == d) {
        return b + c;
    }
    return (c * 1.001F * (-exp2_of(-10 * t / d) + 1)) + b;
}
float expo_in_out(float t, float b, float c, float d) {
    if (t == 0) {
        return b;
    }
    if (t == d) {
        return b + c;
    }
    t = t / d * 2;
    if (t < 1) {
        return (c / 2 * exp2_of(10 * (t - 1))) + b - (c * 0.0005F);
    }
    return (c / 2 * 1.0005F * (-exp2_of(-10 * (t - 1)) + 2)) + b;
}

float elastic_in(float t, float b, float c, float d) {
    if (t == 0) {
        return b;
    }
    t /= d;
    if (t == 1) {
        return b + c;
    }
    t -= 1;
    const float p = d * 0.3F;
    const float a = c * exp2_of(10 * t);
    const float s = p / 4;
    return -(a * sin_of(((t * d) - s) * (2 * kPi) / p)) + b;
}
float elastic_out(float t, float b, float c, float d) {
    if (t == 0) {
        return b;
    }
    t /= d;
    if (t == 1) {
        return b + c;
    }
    const float p = d * 0.3F;
    const float s = p / 4;
    return (c * exp2_of(-10 * t) * sin_of(((t * d) - s) * (2 * kPi) / p)) + c + b;
}
float elastic_in_out(float t, float b, float c, float d) {
    if (t == 0) {
        return b;
    }
    t /= d / 2;
    if (t == 2) {
        return b + c;
    }
    const float p = d * (0.3F * 1.5F);
    float a = c;
    const float s = p / 4;
    if (t < 1) {
        t -= 1;
        a *= exp2_of(10 * t);
        return (-0.5F * (a * sin_of(((t * d) - s) * (2 * kPi) / p))) + b;
    }
    t -= 1;
    a *= exp2_of(-10 * t);
    return (a * sin_of(((t * d) - s) * (2 * kPi) / p) * 0.5F) + c + b;
}

float circ_in(float t, float b, float c, float d) {
    t /= d;
    return (-c * (std::sqrt(1 - (t * t)) - 1)) + b;
}
float circ_out(float t, float b, float c, float d) {
    t = (t / d) - 1;
    return (c * std::sqrt(1 - (t * t))) + b;
}
float circ_in_out(float t, float b, float c, float d) {
    t /= d / 2;
    if (t < 1) {
        return (-c / 2 * (std::sqrt(1 - (t * t)) - 1)) + b;
    }
    t -= 2;
    return (c / 2 * (std::sqrt(1 - (t * t)) + 1)) + b;
}

float bounce_out(float t, float b, float c, float d) {
    t /= d;
    if (t < (1 / 2.75F)) {
        return (c * (7.5625F * t * t)) + b;
    }
    if (t < (2 / 2.75F)) {
        t -= 1.5F / 2.75F;
        return (c * ((7.5625F * t * t) + 0.75F)) + b;
    }
    if (t < static_cast<float>(2.5 / 2.75)) {
        t -= 2.25F / 2.75F;
        return (c * ((7.5625F * t * t) + 0.9375F)) + b;
    }
    t -= 2.625F / 2.75F;
    return (c * ((7.5625F * t * t) + 0.984375F)) + b;
}
float bounce_in(float t, float b, float c, float d) {
    return c - bounce_out(d - t, 0, c, d) + b;
}

float back_in(float t, float b, float c, float d) {
    const float s = 1.70158F;
    t /= d;
    return (c * t * t * (((s + 1) * t) - s)) + b;
}
float back_out(float t, float b, float c, float d) {
    const float s = 1.70158F;
    t = (t / d) - 1;
    return (c * ((t * t * (((s + 1) * t) + s)) + 1)) + b;
}
float back_in_out(float t, float b, float c, float d) {
    const float s = 1.70158F * 1.525F;
    t /= d / 2;
    if (t < 1) {
        return (c / 2 * (t * t * (((s + 1) * t) - s))) + b;
    }
    t -= 2;
    return (c / 2 * ((t * t * (((s + 1) * t) + s)) + 2)) + b;
}

float spring_out(float t, float b, float c, float d) {
    t /= d;
    const float s = 1.0F - t;
    t = ((sin_of(t * kPi * (0.2F + (2.5F * t * t * t))) * pow_of(s, 2.2F)) + t) *
        (1.0F + (1.2F * s));
    return (c * t) + b;
}
float spring_in(float t, float b, float c, float d) {
    return c - spring_out(d - t, 0, c, d) + b;
}

// Tween's interpolaters table, by transition then ease.
constexpr std::array<std::array<Equation, 4>, 12> kEquations{{
    {linear, linear, linear, linear},
    {sine_in, sine_out, sine_in_out, out_in<sine_out, sine_in>},
    {poly_in<5>, poly_out<5>, poly_in_out<5>, out_in<poly_out<5>, poly_in<5>>},
    {poly_in<4>, poly_out<4>, poly_in_out<4>, out_in<poly_out<4>, poly_in<4>>},
    {poly_in<2>, quad_out, quad_in_out, out_in<quad_out, poly_in<2>>},
    {expo_in, expo_out, expo_in_out, out_in<expo_out, expo_in>},
    {elastic_in, elastic_out, elastic_in_out, out_in<elastic_out, elastic_in>},
    {cubic_in, cubic_out, cubic_in_out, out_in<cubic_out, cubic_in>},
    {circ_in, circ_out, circ_in_out, out_in<circ_out, circ_in>},
    {bounce_in, bounce_out, in_out_halves<bounce_in, bounce_out>, out_in<bounce_out, bounce_in>},
    {back_in, back_out, back_in_out, out_in<back_out, back_in>},
    {spring_in, spring_out, in_out_halves<spring_in, spring_out>, out_in<spring_out, spring_in>},
}};

constexpr std::array<std::string_view, 12> kTransitionNames{
    "linear",  "sine",  "quint", "quart",  "quad", "expo",
    "elastic", "cubic", "circ",  "bounce", "back", "spring"};
constexpr std::array<std::string_view, 4> kEaseNames{"in", "out", "in_out", "out_in"};

} // namespace

bool parse_transition(std::string_view name, Transition& out) {
    for (std::size_t i = 0; i < kTransitionNames.size(); ++i) {
        if (kTransitionNames[i] == name) {
            out = static_cast<Transition>(i);
            return true;
        }
    }
    return false;
}

bool parse_ease(std::string_view name, Ease& out) {
    for (std::size_t i = 0; i < kEaseNames.size(); ++i) {
        if (kEaseNames[i] == name) {
            out = static_cast<Ease>(i);
            return true;
        }
    }
    return false;
}

float run_equation(Transition transition, Ease ease, float time, float initial, float delta,
                   float duration) {
    if (duration == 0) {
        // Special case to avoid dividing by 0 in equations.
        return initial + delta;
    }
    const Equation equation =
        kEquations.at(static_cast<std::size_t>(transition)).at(static_cast<std::size_t>(ease));
    return equation(time, initial, delta, duration);
}

} // namespace slash
