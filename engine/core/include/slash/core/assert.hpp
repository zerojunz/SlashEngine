#pragma once

// __FILE__ and __LINE__ rather than std::source_location, which the libstdc++ that Linux exports
// build against (GCC 10, for glibc 2.31) doesn't have.

namespace slash::detail {

[[noreturn]] void assert_failed(const char* expression, const char* message, const char* file,
                                int line);

} // namespace slash::detail

// Checks an invariant in every build type. On failure it logs the expression and location as a
// structured error line, then aborts. Use slash::Result for errors a caller can handle.
#define SLASH_ASSERT(expression, message)                                                          \
    do {                                                                                           \
        if (!(expression)) {                                                                       \
            ::slash::detail::assert_failed(#expression, message, __FILE__, __LINE__);              \
        }                                                                                          \
    } while (false)
