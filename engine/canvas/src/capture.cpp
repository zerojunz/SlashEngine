#include "slash/canvas/canvas.hpp"
#include "slash/ui/ui.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

namespace slash::canvas {

namespace {

// The software device and renderer captures draw with, kept between captures.
struct Capturer {
    asset::Assets* assets = nullptr;
    std::unique_ptr<render::RenderDevice> device;
    std::unique_ptr<CanvasRenderer> renderer;
    render::TextureId target = render::TextureId::None;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

// Copies of a world's context hold this, so it is shared rather than copied.
using CapturerHandle = std::shared_ptr<Capturer>;

} // namespace

void set_settings(world::World& world, const Settings& settings) {
    world.ecs().ctx().insert_or_assign(settings);
}

Settings settings(const world::World& world) {
    const auto* found = world.ecs().ctx().find<Settings>();
    return found != nullptr ? *found : Settings{};
}

Result<Capture> capture(world::World& world, asset::Assets& assets, const CaptureOptions& options) {
    const Settings frame = settings(world);
    const Vec2 viewport = ui::settings(world).viewport;
    const double wide = std::round(static_cast<double>(viewport.x) * options.scale);
    const double high = std::round(static_cast<double>(viewport.y) * options.scale);
    if (!(options.scale > 0.0F) || !(wide >= 1.0) || !(high >= 1.0) ||
        wide > render::kMaxTextureSize || high > render::kMaxTextureSize) {
        return make_error(ErrorCode::InvalidArgument,
                          "a capture is 1 to " + std::to_string(render::kMaxTextureSize) +
                              " pixels wide and high; the viewport is " +
                              std::to_string(static_cast<int>(viewport.x)) + "x" +
                              std::to_string(static_cast<int>(viewport.y)));
    }
    auto& ctx = world.ecs().ctx();
    if (!ctx.contains<CapturerHandle>() || ctx.get<CapturerHandle>()->assets != &assets) {
        auto made = std::make_shared<Capturer>();
        made->assets = &assets;
        made->device = render::create_software_device();
        made->renderer = std::make_unique<CanvasRenderer>(*made->device, assets);
        ctx.insert_or_assign<CapturerHandle>(std::move(made));
    }
    Capturer& capturer = *ctx.get<CapturerHandle>();
    const auto width = static_cast<std::uint32_t>(wide);
    const auto height = static_cast<std::uint32_t>(high);
    if (capturer.target == render::TextureId::None || capturer.width != width ||
        capturer.height != height) {
        if (capturer.target != render::TextureId::None) {
            capturer.device->destroy_texture(capturer.target);
            capturer.target = render::TextureId::None;
        }
        auto target = capturer.device->create_texture({.width = width,
                                                       .height = height,
                                                       .filter = render::Filter::Linear,
                                                       .render_target = true});
        if (!target) {
            return target.error();
        }
        capturer.target = target.value();
        capturer.width = width;
        capturer.height = height;
    }
    CanvasOptions draw;
    draw.target = capturer.target;
    draw.clear = frame.clear_color;
    draw.width = viewport.x;
    draw.height = viewport.y;
    draw.pixel_width = width;
    draw.pixel_height = height;
    draw.interpolation = 1.0;
    draw.texture_filter = frame.texture_filter;
    draw.time =
        options.time >= 0.0 ? options.time : material_time(world.tick(), frame.tick_seconds, 1.0);
    draw.collisions = options.collisions;
    for (std::uint32_t frame_index = 0; frame_index < std::max(options.frames, 1U); ++frame_index) {
        if (auto drawn = capturer.renderer->draw(world, draw); !drawn) {
            return drawn.error();
        }
    }
    auto image = capturer.device->read_texture(capturer.target);
    if (!image) {
        return image.error();
    }
    const CanvasRenderer& renderer = *capturer.renderer;
    return Capture{.image = std::move(image).value(),
                   .sprites = renderer.sprites_drawn(),
                   .tiles = renderer.tiles_drawn(),
                   .particles = renderer.particles_drawn(),
                   .lights = renderer.lights_drawn(),
                   .controls = renderer.controls_drawn(),
                   .shapes = renderer.shapes_drawn(),
                   .meshes = renderer.meshes_drawn(),
                   .scaled = renderer.scaled()};
}

} // namespace slash::canvas
