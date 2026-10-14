#include "slash/core/fixed_step.hpp"

#include <doctest/doctest.h>

// Tick rates here are powers of two so every tick length is exact in binary floating point.

TEST_CASE("FixedStep runs whole ticks and carries the remainder") {
    slash::FixedStep step(4.0); // 0.25 s per tick
    CHECK(step.tick_seconds() == 0.25);
    CHECK(step.advance(0.1) == 0);
    CHECK(step.advance(0.15) == 1);
    CHECK(step.advance(0.5) == 2);
    CHECK(step.alpha() == doctest::Approx(0.0));
    CHECK(step.advance(0.125) == 0);
    CHECK(step.alpha() == doctest::Approx(0.5));
}

TEST_CASE("FixedStep ignores negative elapsed time") {
    slash::FixedStep step(4.0);
    CHECK(step.advance(-1.0) == 0);
    CHECK(step.advance(0.25) == 1);
}

TEST_CASE("FixedStep caps catch-up after a long stall") {
    slash::FixedStep step(8.0, 3); // 0.125 s per tick, at most 3 ticks per advance
    CHECK(step.advance(10.0) == 3);
    // The backlog is dropped, so the next small frame does not trigger another burst.
    CHECK(step.advance(0.0) == 0);
    CHECK(step.alpha() < 1.0);
}

TEST_CASE("FixedStep gives the same tick count however frames are split") {
    slash::FixedStep one_frame(8.0, 1000);
    slash::FixedStep many_frames(8.0, 1000);
    const int total = one_frame.advance(2.0);
    int split_total = 0;
    for (int i = 0; i < 16; ++i) {
        split_total += many_frames.advance(0.125);
    }
    CHECK(total == 16);
    CHECK(split_total == total);
}
