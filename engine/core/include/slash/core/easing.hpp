// Parts of this file are ported from Godot Engine (MIT); see THIRD_PARTY.md.
#pragma once

#include <cstdint>
#include <string_view>

// Tween easing: a value eased from initial by delta over duration, by a transition type and an
// ease type. Sines, powers and exponentials go through slash::cos_sin, exp and ln, so eased values
// are the same bits everywhere.
namespace slash {

// The curve a value follows.
enum class Transition : std::uint8_t {
    Linear,
    Sine,
    Quint,
    Quart,
    Quad,
    Expo,
    Elastic,
    Cubic,
    Circ,
    Bounce,
    Back,
    Spring,
};

// Which end of the curve eases.
enum class Ease : std::uint8_t { In, Out, InOut, OutIn };

// The names of transitions and eases as files write them ("linear", "sine", ...; "in", "out",
// "in_out", "out_in"), and which a name is (false for no such name).
[[nodiscard]] bool parse_transition(std::string_view name, Transition& out);
[[nodiscard]] bool parse_ease(std::string_view name, Ease& out);

// The value at time of a change from initial by delta lasting duration.
[[nodiscard]] float run_equation(Transition transition, Ease ease, float time, float initial,
                                 float delta, float duration);

} // namespace slash
