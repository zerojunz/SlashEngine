// MetalFX off Apple's platforms: there is none (metalfx.hpp).
#include "metalfx.hpp"

namespace slash::render::detail {

struct MetalFxScaler {};

void MetalFxScalerDeleter::operator()(MetalFxScaler* scaler) const {
    delete scaler;
}

MetalFxSupport metalfx_support(SDL_GPUDevice* /*device*/) {
    return {};
}

namespace {

Result<void> none() {
    return make_error(ErrorCode::InvalidArgument, "MetalFX is only on Apple's platforms");
}

} // namespace

Result<void> encode_metalfx_spatial(SDL_GPUDevice* /*device*/, SDL_GPUCommandBuffer* /*commands*/,
                                    MetalFxScalerPtr& /*scaler*/, SDL_GPUTexture* /*color*/,
                                    SDL_GPUTexture* /*output*/, bool /*perceptual*/) {
    return none();
}

Result<void> encode_metalfx_temporal(SDL_GPUDevice* /*device*/, SDL_GPUCommandBuffer* /*commands*/,
                                     MetalFxScalerPtr& /*scaler*/,
                                     const MetalFxTemporalFrame& /*frame*/) {
    return none();
}

} // namespace slash::render::detail
