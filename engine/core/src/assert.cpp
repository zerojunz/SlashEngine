#include "slash/core/assert.hpp"

#include "slash/core/log.hpp"

#include <cstdlib>

namespace slash::detail {

void assert_failed(const char* expression, const char* message, std::source_location location) {
    log::error("assertion failed", {{"expression", expression},
                                    {"message", message},
                                    {"file", location.file_name()},
                                    {"line", location.line()}});
    std::abort();
}

} // namespace slash::detail
