#pragma once

#include "slash/core/assert.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace slash {

enum class ErrorCode : std::uint8_t {
    Unknown,
    InvalidArgument,
    NotFound,
    PlatformFailure,
    PermissionDenied,
};

constexpr std::string_view to_string(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::InvalidArgument:
        return "invalid_argument";
    case ErrorCode::NotFound:
        return "not_found";
    case ErrorCode::PlatformFailure:
        return "platform_failure";
    case ErrorCode::PermissionDenied:
        return "permission_denied";
    case ErrorCode::Unknown:
        break;
    }
    return "unknown";
}

struct Error {
    ErrorCode code = ErrorCode::Unknown;
    std::string message;
};

// The engine's error channel: engine code is built without exceptions, so every fallible call
// returns either a value or an Error.
template <typename T>
class [[nodiscard]] Result {
    static_assert(!std::is_same_v<std::remove_cvref_t<T>, Error>,
                  "Result<Error> is ambiguous; return Result<void> instead");

public:
    Result(T value) : storage_(std::in_place_index<0>, std::move(value)) {}
    Result(Error error) : storage_(std::in_place_index<1>, std::move(error)) {}

    [[nodiscard]] bool ok() const noexcept { return storage_.index() == 0; }
    explicit operator bool() const noexcept { return ok(); }

    [[nodiscard]] T& value() & {
        SLASH_ASSERT(ok(), "value() called on a failed Result");
        return *std::get_if<0>(&storage_);
    }
    [[nodiscard]] const T& value() const& {
        SLASH_ASSERT(ok(), "value() called on a failed Result");
        return *std::get_if<0>(&storage_);
    }
    [[nodiscard]] T&& value() && {
        SLASH_ASSERT(ok(), "value() called on a failed Result");
        return std::move(*std::get_if<0>(&storage_));
    }

    [[nodiscard]] const Error& error() const& {
        SLASH_ASSERT(!ok(), "error() called on a successful Result");
        return *std::get_if<1>(&storage_);
    }

private:
    std::variant<T, Error> storage_;
};

template <>
class [[nodiscard]] Result<void> {
public:
    Result() = default;
    Result(Error error) : storage_(std::in_place_index<1>, std::move(error)) {}

    [[nodiscard]] bool ok() const noexcept { return storage_.index() == 0; }
    explicit operator bool() const noexcept { return ok(); }

    [[nodiscard]] const Error& error() const& {
        SLASH_ASSERT(!ok(), "error() called on a successful Result");
        return *std::get_if<1>(&storage_);
    }

private:
    std::variant<std::monostate, Error> storage_;
};

inline Error make_error(ErrorCode code, std::string message) {
    return Error{code, std::move(message)};
}

} // namespace slash
