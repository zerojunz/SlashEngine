#include "slash/core/result.hpp"

#include <doctest/doctest.h>

#include <memory>
#include <string>

namespace {

slash::Result<int> parse_positive(int value) {
    if (value <= 0) {
        return slash::make_error(slash::ErrorCode::InvalidArgument, "must be positive");
    }
    return value;
}

slash::Result<void> check(bool good) {
    if (!good) {
        return slash::make_error(slash::ErrorCode::NotFound, "missing");
    }
    return {};
}

} // namespace

TEST_CASE("Result holds a value or an error") {
    const auto good = parse_positive(3);
    REQUIRE(good.ok());
    CHECK(good.value() == 3);

    const auto bad = parse_positive(-1);
    REQUIRE_FALSE(bad);
    CHECK(bad.error().code == slash::ErrorCode::InvalidArgument);
    CHECK(bad.error().message == "must be positive");
}

TEST_CASE("Result<void> reports success or an error") {
    CHECK(check(true).ok());
    const auto failed = check(false);
    REQUIRE_FALSE(failed.ok());
    CHECK(slash::to_string(failed.error().code) == "not_found");
}

TEST_CASE("Result works with move-only values") {
    slash::Result<std::unique_ptr<int>> result = std::make_unique<int>(7);
    REQUIRE(result.ok());
    const std::unique_ptr<int> taken = std::move(result).value();
    CHECK(*taken == 7);
}
