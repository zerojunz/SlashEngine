// Ray tracing on Metal off macOS: there is none (gpu_rays.hpp).
#include "gpu_rays.hpp"

namespace slash::render::detail {

std::unique_ptr<GpuRays> make_metal_rays(SDL_GPUDevice* /*device*/) {
    return nullptr;
}

} // namespace slash::render::detail
