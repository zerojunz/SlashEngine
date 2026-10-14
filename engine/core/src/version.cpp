#include "slash/core/version.hpp"

#ifndef SLASH_VERSION
#error "SLASH_VERSION must be defined by the build"
#endif

namespace slash {

std::string_view version() noexcept {
    return SLASH_VERSION;
}

} // namespace slash
