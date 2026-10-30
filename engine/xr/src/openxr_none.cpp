// Builds without the OpenXR loader (the web, Android and iOS): no OpenXR interface.
#include "slash/xr/openxr.hpp"

namespace slash::xr {

bool openxr_available() {
    return false;
}

void set_openxr_runtime(std::string_view /*manifest*/) {}

std::shared_ptr<Interface> make_openxr_interface() {
    return nullptr;
}

const OpenXRInfo* openxr_info(const Interface& /*interface*/) {
    return nullptr;
}

Result<void> use_openxr_interface(world::World& /*world*/) {
    return make_error(ErrorCode::NotFound, "OpenXR: this platform's build has no OpenXR loader");
}

} // namespace slash::xr
