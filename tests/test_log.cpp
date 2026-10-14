#include "slash/core/log.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <string>
#include <vector>

namespace slog = slash::log;

TEST_CASE("format_line writes one JSON object with typed fields") {
    const std::string line = slog::format_line(
        slog::Level::Info, "window created",
        {{"width", 1280}, {"scale", 2.5}, {"headless", false}, {"driver", "cocoa"}});
    CHECK(line == R"({"level":"info","msg":"window created","width":1280,"scale":2.5,)"
                  R"("headless":false,"driver":"cocoa"})");
}

TEST_CASE("escape_json escapes quotes, backslashes and control characters") {
    CHECK(slog::escape_json("say \"hi\"") == R"(say \"hi\")");
    CHECK(slog::escape_json("a\\b") == R"(a\\b)");
    CHECK(slog::escape_json("line\nnext\ttab") == R"(line\nnext\ttab)");
    CHECK(slog::escape_json(std::string("\x01", 1)) == R"(\u0001)");
    CHECK(slog::escape_json("héllo") == "héllo");
}

TEST_CASE("non-finite numbers become null") {
    const std::string line = slog::format_line(slog::Level::Warn, "bad", {{"value", std::nan("")}});
    CHECK(line == R"({"level":"warn","msg":"bad","value":null})");
}

TEST_CASE("sink receives lines at or above the minimum level") {
    std::vector<std::string> lines;
    slog::set_sink([&lines](slog::Level, std::string_view line) { lines.emplace_back(line); });
    slog::set_min_level(slog::Level::Info);

    slog::debug("hidden");
    slog::info("shown", {{"n", 1}});
    slog::error("also shown");

    slog::reset_sink();
    REQUIRE(lines.size() == 2);
    CHECK(lines[0] == R"({"level":"info","msg":"shown","n":1})");
    CHECK(lines[1] == R"({"level":"error","msg":"also shown"})");
}
