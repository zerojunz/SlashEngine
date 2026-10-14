#include "slash/core/assert.hpp"

#include "slash/core/log.hpp"

#include <cstdlib>

namespace slash::detail {

void assert_failed(const char* expression, const char* message, const char* file, int line) {
    log::error("assertion failed",
               {{"expression", expression}, {"message", message}, {"file", file}, {"line", line}});
    std::abort();
}

} // namespace slash::detail
