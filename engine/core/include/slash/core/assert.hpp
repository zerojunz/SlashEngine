#pragma once

#include <source_location>

namespace slash::detail {

[[noreturn]] void assert_failed(const char* expression, const char* message,
                                std::source_location location);

} // namespace slash::detail

// Checks an invariant in every build type. On failure it logs the expression and location as a
// structured error line, then aborts. Use slash::Result for errors a caller can handle.
#define SLASH_ASSERT(expression, message)                                                          \
    do {                                                                                           \
        if (!(expression)) {                                                                       \
            ::slash::detail::assert_failed(#expression, message, std::source_location::current()); \
        }                                                                                          \
    } while (false)
