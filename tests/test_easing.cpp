#include "slash/core/easing.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <string_view>

using namespace slash;

namespace {

float eased(std::string_view transition, std::string_view ease, float t) {
    Transition kind = Transition::Linear;
    Ease way = Ease::In;
    REQUIRE(parse_transition(transition, kind));
    REQUIRE(parse_ease(ease, way));
    return run_equation(kind, way, t, 0.0F, 1.0F, 1.0F);
}

} // namespace

TEST_CASE("easing: Godot's tween equations by name, from start to end") {
    using doctest::Approx;
    CHECK(eased("linear", "in", 0.5F) == Approx(0.5F));
    CHECK(eased("sine", "in", 0.5F) == Approx(1.0F - std::sqrt(0.5F)).epsilon(1e-5));
    CHECK(eased("sine", "in_out", 0.5F) == Approx(0.5F).epsilon(1e-5));
    CHECK(eased("quad", "in", 0.5F) == Approx(0.25F));
    CHECK(eased("quad", "out", 0.5F) == Approx(0.75F));
    CHECK(eased("cubic", "in", 0.5F) == Approx(0.125F));
    CHECK(eased("quart", "in", 0.5F) == Approx(0.0625F));
    CHECK(eased("quint", "in", 0.5F) == Approx(0.03125F));
    CHECK(eased("circ", "in", 0.5F) == Approx(1.0F - std::sqrt(0.75F)).epsilon(1e-5));
    // Back overshoots below its start before it rises.
    CHECK(eased("back", "in", 0.5F) == Approx(-0.0876975F).epsilon(1e-4));
    // Out then in passes halfway at half time.
    CHECK(eased("cubic", "out_in", 0.5F) == Approx(0.5F));

    // Every pair starts at the initial value and ends at initial plus delta.
    constexpr std::array<std::string_view, 12> kTransitions{
        "linear",  "sine",  "quint", "quart",  "quad", "expo",
        "elastic", "cubic", "circ",  "bounce", "back", "spring"};
    constexpr std::array<std::string_view, 4> kEases{"in", "out", "in_out", "out_in"};
    for (const std::string_view transition : kTransitions) {
        for (const std::string_view ease : kEases) {
            CAPTURE(transition);
            CAPTURE(ease);
            CHECK(std::abs(eased(transition, ease, 0.0F)) < 2e-3F);
            CHECK(std::abs(eased(transition, ease, 1.0F) - 1.0F) < 2e-3F);
        }
    }
    // No time to ease: the end at once.
    CHECK(run_equation(Transition::Elastic, Ease::Out, 0.0F, 2.0F, 3.0F, 0.0F) == 5.0F);
    Transition kind = Transition::Linear;
    CHECK_FALSE(parse_transition("springy", kind));
    Ease way = Ease::In;
    CHECK_FALSE(parse_ease("inout", way));
}
