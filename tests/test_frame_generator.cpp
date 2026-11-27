// A game's frame generator (frame_generator.hpp): the registry, choosing one, the devices handing
// it a frame's textures (RenderDevice::generate_frames), and the canvas renderer showing as many
// frames as it makes before each drawn one.

#include "devices.hpp"

#include "slash/asset/assets.hpp"
#include "slash/canvas/canvas.hpp"
#include "slash/core/json.hpp"
#include "slash/render/device.hpp"
#include "slash/render/frame_generator.hpp"
#include "slash/render/image.hpp"
#include "slash/render/mesh.hpp"
#include "slash/runtime/project.hpp"
#include "slash/ui/ui.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace asset = slash::asset;
namespace canvas = slash::canvas;
namespace json = slash::json;
namespace render = slash::render;
namespace world = slash::world;
using render::TextureId;

namespace {

// What the test generator was given for a call.
struct Seen {
    render::Backend backend = render::Backend::Null;
    bool commands = false;
    render::UpscaleSize size;
    bool color = false;
    std::size_t depth_values = 0;
    std::size_t motion_values = 0;
    bool depth_image = false;
    float depth_center = 0.0F;
    float depth_corner = 0.0F;
    std::size_t outputs = 0;
    bool reset = false;
    render::Mat4 projection{};
};

// Records each call, then fills output i with the gray (i + 1) / 4 on the software device, or
// fails when told to.
struct Recorder {
    std::vector<Seen> calls;
    bool fail = false;
    int made = 0;
};

class TestGenerator final : public render::FrameGenerator {
public:
    explicit TestGenerator(Recorder& recorder) : recorder_(&recorder) {}

    slash::Result<void> generate(const render::GeneratedFrames& frames) override {
        Seen seen{.backend = frames.device.backend,
                  .commands = frames.commands != nullptr,
                  .size = frames.size,
                  .color = frames.color.image != nullptr || !frames.color.pixels.empty(),
                  .depth_values = frames.depth.pixels.size(),
                  .motion_values = frames.motion.pixels.size(),
                  .depth_image = frames.depth.image != nullptr,
                  .depth_center = 0.0F,
                  .depth_corner = 0.0F,
                  .outputs = frames.outputs.size(),
                  .reset = frames.reset,
                  .projection = frames.projection};
        if (!frames.depth.pixels.empty()) {
            const std::uint32_t w = frames.depth.width;
            const std::uint32_t h = frames.depth.height;
            seen.depth_center = frames.depth.pixels[(std::size_t{h / 2} * w) + (w / 2)];
            seen.depth_corner = frames.depth.pixels[0];
        }
        recorder_->calls.push_back(seen);
        if (recorder_->fail) {
            return slash::make_error(slash::ErrorCode::InvalidArgument, "the vendor said no");
        }
        for (std::size_t i = 0; i < frames.outputs.size(); ++i) {
            for (float& value : frames.outputs[i].pixels) {
                value = static_cast<float>(i + 1) / 4.0F;
            }
        }
        return {};
    }

private:
    Recorder* recorder_;
};

// Adds "test_generator", which makes up to max_frames frames, for the test's length.
struct Added {
    Recorder recorder;

    explicit Added(std::uint32_t max_frames = 3,
                   std::function<bool(const render::RenderDevice&)> supports = {}) {
        render::clear_added_frame_generators();
        REQUIRE(render::add_frame_generator({.name = "test_generator",
                                             .max_frames = max_frames,
                                             .supports = std::move(supports),
                                             .make =
                                                 [this] {
                                                     ++recorder.made;
                                                     return std::unique_ptr<render::FrameGenerator>(
                                                         std::make_unique<TestGenerator>(recorder));
                                                 }})
                    .ok());
    }
    Added(const Added&) = delete;
    Added& operator=(const Added&) = delete;
    Added(Added&&) = delete;
    Added& operator=(Added&&) = delete;
    ~Added() { render::clear_added_frame_generators(); }
};

// A square facing +Z, one unit across.
std::vector<render::Vertex3D> square() {
    const auto corner = [](float cx, float cy) {
        return render::Vertex3D{.position = {cx * 0.5F, cy * 0.5F, 0.0F},
                                .normal = {0.0F, 0.0F, 1.0F},
                                .tangent = {1.0F, 0.0F, 0.0F, 1.0F},
                                .uv = {(cx + 1.0F) * 0.5F, (1.0F - cy) * 0.5F}};
    };
    return {corner(-1, 1), corner(1, 1),  corner(1, -1),
            corner(-1, 1), corner(1, -1), corner(-1, -1)};
}

render::Scene3D square_scene(std::string generator) {
    render::Scene3D scene;
    scene.method = render::RenderMethod::ForwardPlus;
    scene.frame_generation = true;
    scene.frame_generator = std::move(generator);
    scene.camera = render::look_at({0.0F, 0.0F, 2.5F}, {0.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F});
    scene.projection = render::orthogonal(3.0F, 1.0F, 0.1F, 20.0F);
    return scene;
}

// Draws the square at x into target, its frame keeping its prepass for generator.
void draw_square(render::RenderDevice& device, TextureId target, render::MeshId mesh, float x,
                 const std::string& generator) {
    REQUIRE(device
                .begin_frame({.target = target,
                              .clear = {0.1F, 0.1F, 0.1F, 1.0F},
                              .scene = square_scene(generator)})
                .ok());
    const render::Material3D white{.unshaded = true, .cull = render::CullMode::Disabled};
    const std::array<float, 16> moved{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, 0, 0, 1};
    REQUIRE(device.draw_mesh(mesh, moved, white, 1, render::ShadowCasting::On, moved).ok());
    REQUIRE(device.end_frame().ok());
}

// A 32x32 2D world with a frame generator in its settings.
struct Scene2D {
    asset::Assets assets{asset::FileSource{
        .read = [](std::string_view) -> slash::Result<std::string> {
            return slash::make_error(slash::ErrorCode::NotFound, "no such file");
        },
        .stamp = [](std::string_view) -> std::optional<asset::FileStamp> { return std::nullopt; },
    }};
    world::ComponentRegistry components;
    world::World world{components};

    Scene2D(const std::string& generator, std::uint32_t frames) {
        world::register_builtin_components(components);
        canvas::install(world, assets);
        slash::ui::Settings ui = slash::ui::settings(world);
        ui.viewport = {32.0F, 32.0F};
        slash::ui::set_settings(world, ui);
        canvas::Settings settings;
        settings.clear_color = {.r = 0.0F, .g = 0.0F, .b = 0.0F, .a = 1.0F};
        settings.frame_generation = true;
        settings.frame_generator = generator;
        settings.generated_frames = frames;
        canvas::set_settings(world, settings);
    }
};

} // namespace

TEST_CASE("Games add frame generators by name") {
    render::clear_added_frame_generators();
    const auto make = [] { return std::unique_ptr<render::FrameGenerator>(); };
    CHECK(render::frame_generator_names() == std::vector<std::string>{"fsr3"});
    CHECK_FALSE(render::add_frame_generator({.name = "DLSS", .make = make}).ok());
    CHECK_FALSE(render::add_frame_generator({.name = "", .make = make}).ok());
    CHECK_FALSE(render::add_frame_generator({.name = "dlss_g"}).ok());
    CHECK_FALSE(
        render::add_frame_generator({.name = "dlss_g", .max_frames = 0, .make = make}).ok());
    CHECK_FALSE(
        render::add_frame_generator({.name = "dlss_g", .max_frames = 4, .make = make}).ok());
    CHECK_FALSE(render::add_frame_generator({.name = "fsr3", .make = make}).ok());
    REQUIRE(render::add_frame_generator({.name = "dlss_g", .max_frames = 3, .make = make}).ok());
    const auto taken = render::add_frame_generator({.name = "dlss_g", .make = make});
    REQUIRE_FALSE(taken.ok());
    CHECK(taken.error().code == slash::ErrorCode::Conflict);
    CHECK(render::frame_generator_names() == std::vector<std::string>{"fsr3", "dlss_g"});
    REQUIRE(render::find_frame_generator("dlss_g"));
    CHECK(render::find_frame_generator("dlss_g")->max_frames == 3);
    CHECK_FALSE(render::find_frame_generator("fsr3"));
    render::clear_added_frame_generators();
    CHECK_FALSE(render::find_frame_generator("dlss_g"));
}

TEST_CASE("A frame generator is chosen where it runs, else the engine's own") {
    const auto software = render::create_software_device();
    const auto null = render::create_null_device();
    {
        const Added added(2);
        const render::FrameGeneratorChoice chosen =
            render::choose_frame_generator("test_generator", 3, *software);
        CHECK(chosen.name == "test_generator");
        CHECK(chosen.frames == 2); // at most its max_frames
        CHECK(render::choose_frame_generator("test_generator", 0, *software).frames == 1);
        CHECK(render::choose_frame_generator("test_generator", 3, *null).name.empty());
        CHECK(render::choose_frame_generator("fsr3", 3, *software).name.empty());
        CHECK(render::choose_frame_generator("", 3, *software).frames == 1);
        CHECK(render::choose_frame_generator("missing", 3, *software).name.empty());
        CHECK(render::runs_frame_generators(*software));
        CHECK_FALSE(render::runs_frame_generators(*null));
    }
    const Added refusing(3, [](const render::RenderDevice&) { return false; });
    CHECK(render::choose_frame_generator("test_generator", 3, *software).name.empty());
}

TEST_CASE("The software device hands a frame generator the frame, its depth and motion") {
    Added added;
    const auto device = render::create_software_device();
    auto target = device->create_texture({.width = 32, .height = 32, .render_target = true});
    REQUIRE(target.ok());
    std::vector<TextureId> into;
    for (int i = 0; i < 3; ++i) {
        auto made = device->create_texture({.width = 32, .height = 32, .render_target = true});
        REQUIRE(made.ok());
        into.push_back(made.value());
    }
    auto mesh = device->create_mesh(square(), {});
    REQUIRE(mesh.ok());

    draw_square(*device, target.value(), mesh.value(), 0.0F, "test_generator");
    REQUIRE(device->generate_frames(target.value(), into, "test_generator").ok());
    REQUIRE(added.recorder.calls.size() == 1);
    const Seen& first = added.recorder.calls[0];
    CHECK(first.backend == render::Backend::Software);
    CHECK_FALSE(first.commands);
    CHECK(first.size.render_width == 32);
    CHECK(first.size.output_width == 32);
    CHECK(first.color);
    CHECK(first.depth_values == 32 * 32);
    CHECK(first.motion_values == 32 * 32 * 2);
    CHECK(first.depth_center > 0.0F); // the square
    CHECK(first.depth_corner == 0.0F);
    CHECK(first.outputs == 3);
    CHECK(first.reset);
    CHECK(first.projection == square_scene("").projection);
    // Each output holds what the generator wrote, as 8 bits a channel.
    for (std::size_t i = 0; i < into.size(); ++i) {
        auto image = device->read_texture(into[i]);
        REQUIRE(image.ok());
        CHECK(int{image.value().pixels[0]} ==
              static_cast<int>(std::lround(static_cast<double>(i + 1) / 4.0 * 255.0)));
    }

    // The next frame follows on; one made with no 3D has no depth or motion.
    draw_square(*device, target.value(), mesh.value(), 0.25F, "test_generator");
    REQUIRE(device->generate_frames(target.value(), into, "test_generator").ok());
    CHECK_FALSE(added.recorder.calls.back().reset);
    REQUIRE(device->begin_frame({.target = target.value(), .clear = {}}).ok());
    REQUIRE(device->end_frame().ok());
    REQUIRE(
        device->generate_frames(target.value(), std::span(into).first(1), "test_generator").ok());
    CHECK_FALSE(added.recorder.calls.back().reset);
    CHECK(added.recorder.calls.back().depth_values == 0);
    CHECK(added.recorder.calls.back().motion_values == 0);
    // A frame skipped between starts it over.
    REQUIRE(device->begin_frame({.target = target.value(), .clear = {}}).ok());
    REQUIRE(device->end_frame().ok());
    REQUIRE(device->begin_frame({.target = target.value(), .clear = {}}).ok());
    REQUIRE(device->end_frame().ok());
    REQUIRE(device->generate_frames(target.value(), into, "test_generator").ok());
    CHECK(added.recorder.calls.back().reset);
    CHECK(added.recorder.made == 1);

    // What it can't do is refused, and its failures returned.
    const std::size_t calls = added.recorder.calls.size();
    CHECK_FALSE(device->generate_frames(target.value(), into, "missing").ok());
    CHECK_FALSE(device->generate_frames(target.value(), {}, "test_generator").ok());
    const std::array<TextureId, 1> itself{target.value()};
    CHECK_FALSE(device->generate_frames(target.value(), itself, "test_generator").ok());
    CHECK_FALSE(device->generate_frames(target.value(), into, "").ok()); // the engine's makes 1
    CHECK(device->generate_frames(target.value(), std::span(into).first(1), "").ok());
    CHECK(added.recorder.calls.size() == calls);
    added.recorder.fail = true;
    const auto failed = device->generate_frames(target.value(), into, "test_generator");
    REQUIRE_FALSE(failed.ok());
    CHECK(failed.error().message == "the vendor said no");
    added.recorder.fail = false;

    const auto null = render::create_null_device();
    CHECK_FALSE(null->generate_frames(TextureId{1}, into, "test_generator").ok());
    device->destroy_mesh(mesh.value());
}

TEST_CASE("Frames generated with too many targets for the generator are refused") {
    Added added(1);
    const auto device = render::create_software_device();
    auto target = device->create_texture({.width = 8, .height = 8, .render_target = true});
    auto a = device->create_texture({.width = 8, .height = 8, .render_target = true});
    auto b = device->create_texture({.width = 8, .height = 8, .render_target = true});
    REQUIRE(target.ok());
    REQUIRE(a.ok());
    REQUIRE(b.ok());
    const std::array<TextureId, 2> two{a.value(), b.value()};
    CHECK_FALSE(device->generate_frames(target.value(), two, "test_generator").ok());
    CHECK(device->generate_frames(target.value(), std::span(two).first(1), "test_generator").ok());
}

TEST_CASE("GPUs hand a frame generator the frame's native textures") {
    auto gpu = slash::test::gpu_device();
    if (gpu == nullptr) {
        MESSAGE("no GPU backend; skipped");
        return;
    }
    Added added;
    auto target = gpu->create_texture({.width = 32, .height = 32, .render_target = true});
    auto a = gpu->create_texture({.width = 32, .height = 32, .render_target = true});
    auto b = gpu->create_texture({.width = 32, .height = 32, .render_target = true});
    REQUIRE(target.ok());
    REQUIRE(a.ok());
    REQUIRE(b.ok());
    auto mesh = gpu->create_mesh(square(), {});
    REQUIRE(mesh.ok());
    const std::array<TextureId, 2> into{a.value(), b.value()};
    if (!render::runs_frame_generators(*gpu)) {
        CHECK_FALSE(gpu->generate_frames(target.value(), into, "test_generator").ok());
        return;
    }
    draw_square(*gpu, target.value(), mesh.value(), 0.0F, "test_generator");
    REQUIRE(gpu->generate_frames(target.value(), into, "test_generator").ok());
    draw_square(*gpu, target.value(), mesh.value(), 0.25F, "test_generator");
    REQUIRE(gpu->generate_frames(target.value(), into, "test_generator").ok());
    REQUIRE(added.recorder.calls.size() == 2);
    const Seen& seen = added.recorder.calls[1];
    CHECK(seen.backend == gpu->backend());
    CHECK(seen.commands);
    CHECK(seen.color);
    CHECK(seen.depth_image);
    CHECK(seen.outputs == 2);
    CHECK(added.recorder.calls[0].reset);
    CHECK_FALSE(seen.reset);
    gpu->destroy_mesh(mesh.value());
}

TEST_CASE("The canvas renderer shows each frame a frame generator makes before the drawn one") {
    Added added;
    const auto device = render::create_software_device();
    auto target = device->create_texture({.width = 32, .height = 32, .render_target = true});
    REQUIRE(target.ok());
    Scene2D scene("test_generator", 3);
    canvas::CanvasRenderer renderer(*device, scene.assets);
    std::vector<std::uint32_t> parts;
    canvas::CanvasOptions options{.target = target.value(), .width = 32.0F, .height = 32.0F};
    options.generate = true;
    options.between_frames = [&](std::uint32_t frames) { parts.push_back(frames); };
    REQUIRE(renderer.draw(scene.world, options).ok());
    REQUIRE(renderer.draw(scene.world, options).ok());
    CHECK(renderer.frame_generator() == "test_generator");
    CHECK(renderer.frames_generated() == 6);
    CHECK(parts == std::vector<std::uint32_t>(6, 4));
    REQUIRE(added.recorder.calls.size() == 2);
    CHECK(added.recorder.calls[0].reset);
    CHECK_FALSE(added.recorder.calls[1].reset);
    CHECK(added.recorder.calls[1].outputs == 3);

    // Without the generator, the engine's own makes one.
    render::clear_added_frame_generators();
    parts.clear();
    REQUIRE(renderer.draw(scene.world, options).ok());
    CHECK(renderer.frame_generator() == "fsr3");
    CHECK(parts == std::vector<std::uint32_t>{2});
}

TEST_CASE("project.json names the frame generator and how many frames it makes") {
    const json::Value rendering =
        json::parse(R"({"frame_generation": true, "frame_generator": "dlss_g",
                        "generated_frames": 3})")
            .value();
    CHECK(render::frame_generator(rendering) == "dlss_g");
    CHECK(render::generated_frames(rendering) == 3);
    const json::Value none = json::parse("{}").value();
    CHECK(render::frame_generator(none).empty());
    CHECK(render::generated_frames(none) == 1);
    CHECK(render::generated_frames(json::parse(R"({"generated_frames": 9})").value()) == 3);
}
