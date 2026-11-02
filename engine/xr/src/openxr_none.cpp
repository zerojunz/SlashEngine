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

Result<OpenXRDevice> prepare_openxr_device(world::World& /*world*/, std::string_view /*driver*/) {
    return make_error(ErrorCode::NotFound, "OpenXR: this platform's build has no OpenXR loader");
}

Result<void> use_openxr_interface(world::World& /*world*/, render::RenderDevice* /*device*/) {
    return make_error(ErrorCode::NotFound, "OpenXR: this platform's build has no OpenXR loader");
}

Result<OpenXRFrame> begin_openxr_frame(world::World& /*world*/) {
    return OpenXRFrame{};
}

Result<void> end_openxr_frame(world::World& /*world*/) {
    return {};
}

} // namespace slash::xr
