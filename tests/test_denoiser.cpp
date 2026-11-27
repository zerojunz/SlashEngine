// A game's denoiser (denoiser.hpp): the registry, choosing one, project.json's
// rendering.ray_tracing.denoiser, and the devices handing it a frame's ray-traced passes to denoise
// in place before the mesh pass lights the frame from them.

#include "devices.hpp"

#include "slash/core/json.hpp"
#include "slash/render/denoiser.hpp"
#include "slash/render/device.hpp"
#include "slash/render/image.hpp"
#include "slash/render/mesh.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace json = slash::json;
namespace render = slash::render;
using render::Image;
using render::Vertex3D;
using Float3 = std::array<float, 3>;

namespace {

template <typename T>
std::string error_of(const slash::Result<T>& result) {
    return result.ok() ? "(succeeded)" : result.error().message;
}

// What the test denoiser was given for a call.
struct Seen {
    render::Backend backend = render::Backend::Null;
    bool commands = false;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::size_t depth_values = 0;
    bool depth_image = false;
    float depth_center = 0.0F;
    std::size_t motion_values = 0;
    // Whether each pass came: shadows' two halves, occlusion, reflections, ambient light.
    std::array<bool, 5> passes{};
    bool reset = false;
};

// What the test denoiser does to the passes it gets.
enum class Doing : std::uint8_t {
    Nothing, // leaves them
    Darken,  // sets every shadow, occlusion and ambient light to 0
    Fail,    // writes 0s, then fails
};

struct Recorder {
    std::vector<Seen> calls;
    Doing doing = Doing::Nothing;
    int made = 0;
};

class TestDenoiser final : public render::Denoiser {
public:
    explicit TestDenoiser(Recorder& recorder) : recorder_(&recorder) {}

    slash::Result<void> denoise(const render::DenoiseFrame& frame) override {
        const auto came = [](const render::NativeTexture& texture) {
            return texture.image != nullptr || !texture.pixels.empty();
        };
        Seen seen{.backend = frame.device.backend,
                  .commands = frame.commands != nullptr,
                  .width = frame.width,
                  .height = frame.height,
                  .depth_values = frame.depth.pixels.size(),
                  .depth_image = frame.depth.image != nullptr,
                  .motion_values = frame.motion.pixels.size(),
                  .passes = {came(frame.shadows[0]), came(frame.shadows[1]), came(frame.occlusion),
                             came(frame.reflections), came(frame.ambient_light)},
                  .reset = frame.reset};
        if (!frame.depth.pixels.empty()) {
            const std::size_t center =
                (std::size_t{frame.height / 2} * frame.width) + (frame.width / 2);
            seen.depth_center = frame.depth.pixels[center];
        }
        recorder_->calls.push_back(seen);
        if (recorder_->doing != Doing::Nothing) {
            for (const render::NativeTexture* pass : {frame.shadows.data(), &frame.shadows[1],
                                                      &frame.occlusion, &frame.ambient_light}) {
                for (float& value : pass->pixels) {
                    value = 0.0F;
                }
            }
        }
        if (recorder_->doing == Doing::Fail) {
            return slash::make_error(slash::ErrorCode::PlatformFailure, "no GPU of the vendor's");
        }
        return {};
    }

private:
    Recorder* recorder_;
};

// Adds "test_denoiser" for the test's length, which records into recorder; supports says runs.
struct Added {
    Recorder recorder;

    explicit Added(bool runs = true) {
        render::clear_added_denoisers();
        REQUIRE(
            render::add_denoiser({.name = "test_denoiser",
                                  .supports = [runs](const render::RenderDevice&) { return runs; },
                                  .make =
                                      [this] {
                                          ++recorder.made;
                                          return std::unique_ptr<render::Denoiser>(
                                              std::make_unique<TestDenoiser>(recorder));
                                      }})
                .ok());
    }
    Added(const Added&) = delete;
    Added& operator=(const Added&) = delete;
    Added(Added&&) = delete;
    Added& operator=(Added&&) = delete;
    ~Added() { render::clear_added_denoisers(); }
};

Float3 scaled(const Float3& v, float s) {
    return {v[0] * s, v[1] * s, v[2] * s};
}
Float3 sum(const Float3& a, const Float3& b) {
    return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}

// A square of side size around center, facing normal.
std::vector<Vertex3D> quad(const Float3& center, const Float3& u, const Float3& up,
                           const Float3& normal, float size) {
    const auto corner = [&](float s, float t) {
        return Vertex3D{
            .position = sum(center, sum(scaled(u, s * size * 0.5F), scaled(up, t * size * 0.5F))),
            .normal = normal,
            .tangent = {u[0], u[1], u[2], 1.0F},
            .uv = {(s + 1.0F) * 0.5F, (1.0F - t) * 0.5F}};
    };
    const Vertex3D top_left = corner(-1.0F, 1.0F);
    const Vertex3D top_right = corner(1.0F, 1.0F);
    const Vertex3D bottom_right = corner(1.0F, -1.0F);
    const Vertex3D bottom_left = corner(-1.0F, -1.0F);
    return {top_left, top_right, bottom_right, top_left, bottom_right, bottom_left};
}

// A cube of side 1 around the origin.
std::vector<Vertex3D> cube() {
    struct Face {
        Float3 normal;
        Float3 u;
        Float3 up;
    };
    const std::array<Face, 6> faces{{
        {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}},
        {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
        {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}},
        {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
        {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},
        {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}},
    }};
    std::vector<Vertex3D> out;
    for (const Face& face : faces) {
        const std::vector<Vertex3D> side =
            quad(scaled(face.normal, 0.5F), face.u, face.up, face.normal, 1.0F);
        out.insert(out.end(), side.begin(), side.end());
    }
    return out;
}

render::Mat4 moved(float x, float y, float z) {
    return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, y, z, 1};
}

// A camera above a floor with a cube on it, a light from the side with ray-traced shadows, and
// ray-traced ambient occlusion (and, with lit, ambient light), denoised by denoiser.
render::Scene3D traced_scene(const std::string& denoiser, bool lit = false) {
    render::Scene3D scene;
    scene.camera = render::look_at({0.0F, 6.0F, 0.0F}, {0.0F, 0.0F, 0.0F}, {0.0F, 0.0F, -1.0F});
    scene.projection = render::perspective(60.0F, 1.0F, 0.1F, 20.0F);
    scene.ambient = {0.4F, 0.4F, 0.4F};
    render::Light3D light{.type = render::LightType::Directional, .shadow = true};
    light.direction = {0.70710677F, -0.70710677F, 0.0F};
    light.up = {0.70710677F, 0.70710677F, 0.0F};
    scene.lights = {light};
    scene.shadows = {.directional_size = 1024, .positional_size = 1024};
    scene.ssao.enabled = true;
    scene.ssao.radius = 1.0F;
    scene.ray_tracing.shadows = true;
    scene.ray_tracing.ambient_occlusion = true;
    scene.ray_tracing.global_illumination = lit;
    scene.ray_tracing.denoiser = denoiser;
    return scene;
}

// The sum of a picture's green channel.
long brightness(const Image& image) {
    long total = 0;
    for (std::size_t i = 1; i < image.pixels.size(); i += 4) {
        total += image.pixels[i];
    }
    return total;
}

// Draws scene frames times into one size x size target, and reads the last.
Image draw(render::RenderDevice& device, const render::Scene3D& scene, int frames = 1,
           std::uint32_t size = 24) {
    auto target = device.create_texture({.width = size, .height = size, .render_target = true});
    REQUIRE(target.ok());
    auto box = device.create_mesh(cube(), {});
    REQUIRE(box.ok());
    auto floor = device.create_mesh(quad({0, 0, 0}, {1, 0, 0}, {0, 0, -1}, {0, 1, 0}, 8.0F), {});
    REQUIRE(floor.ok());
    for (int i = 0; i < frames; ++i) {
        const auto begun =
            device.begin_frame({.target = target.value(), .clear = {}, .scene = scene});
        REQUIRE_MESSAGE(begun.ok(), error_of(begun));
        REQUIRE(device.draw_mesh(floor.value(), moved(0, 0, 0), {.albedo = {0.8F, 0.8F, 0.8F, 1}})
                    .ok());
        REQUIRE(
            device.draw_mesh(box.value(), moved(0, 1, 0), {.albedo = {0.9F, 0.2F, 0.2F, 1}}).ok());
        const auto ended = device.end_frame();
        REQUIRE_MESSAGE(ended.ok(), error_of(ended));
    }
    auto image = device.read_texture(target.value());
    REQUIRE_MESSAGE(image.ok(), error_of(image));
    device.destroy_mesh(box.value());
    device.destroy_mesh(floor.value());
    device.destroy_texture(target.value());
    return image.value();
}

} // namespace

TEST_CASE("Games add denoisers by name") {
    render::clear_added_denoisers();
    const auto make = [] { return std::unique_ptr<render::Denoiser>(); };
    CHECK(render::denoiser_names().empty());
    CHECK_FALSE(render::add_denoiser({.name = "Ray Reconstruction", .make = make}).ok());
    CHECK_FALSE(render::add_denoiser({.name = "dlss_rr"}).ok());
    REQUIRE(render::add_denoiser({.name = "dlss_rr", .make = make}).ok());
    CHECK(render::add_denoiser({.name = "dlss_rr", .make = make}).error().code ==
          slash::ErrorCode::Conflict);
    REQUIRE(render::add_denoiser({.name = "nrd", .make = make}).ok());
    CHECK(render::denoiser_names() == std::vector<std::string>{"dlss_rr", "nrd"});
    CHECK(render::find_denoiser("nrd"));
    CHECK_FALSE(render::find_denoiser("fidelityfx"));

    // Chosen where it is added and runs, on a device that traces rays.
    const auto software = render::create_software_device();
    CHECK(render::choose_denoiser("nrd", *software));
    CHECK_FALSE(render::choose_denoiser("", *software));
    CHECK_FALSE(render::choose_denoiser("fidelityfx", *software));
    const auto null = render::create_null_device();
    CHECK_FALSE(render::choose_denoiser("nrd", *null));
    render::clear_added_denoisers();
    CHECK(render::denoiser_names().empty());
    const Added refusing(false);
    CHECK_FALSE(render::choose_denoiser("test_denoiser", *software));
}

TEST_CASE("project.json's rendering.ray_tracing.denoiser names the game's denoiser") {
    CHECK(render::ray_tracing(json::parse(R"({"ray_tracing": {"denoiser": "dlss_rr"}})").value())
              .denoiser == "dlss_rr");
    CHECK(render::ray_tracing(json::parse(R"({"ray_tracing": {"denoiser": 3}})").value())
              .denoiser.empty());
    CHECK(render::ray_tracing(json::parse(R"({})").value()).denoiser.empty());
}

TEST_CASE("The software device hands a denoiser the frame's ray-traced passes to denoise") {
    const auto device = render::create_software_device();
    const Image plain = draw(*device, traced_scene(""));
    Added added;
    // Left as they are, the passes light the frame as without a denoiser.
    const Image kept = draw(*device, traced_scene("test_denoiser"), 2);
    CHECK(kept.pixels == plain.pixels);
    REQUIRE(added.recorder.calls.size() == 2);
    CHECK(added.recorder.made == 1);
    const Seen& first = added.recorder.calls.front();
    CHECK(first.backend == render::Backend::Software);
    CHECK_FALSE(first.commands);
    CHECK(first.width == 24);
    CHECK(first.height == 24);
    CHECK(first.depth_values == std::size_t{24} * 24);
    CHECK(first.depth_center > 0.0F); // the cube's top
    CHECK(first.motion_values == 0);  // no TAA: no motion vectors
    CHECK(first.passes == std::array{true, true, true, false, false});
    CHECK(first.reset);
    CHECK_FALSE(added.recorder.calls.back().reset);

    // Darkened, the frame is darker.
    added.recorder.calls.clear();
    added.recorder.doing = Doing::Darken;
    CHECK(brightness(draw(*device, traced_scene("test_denoiser"))) < brightness(plain) - 1000);

    // Failing, the frame is lit by the passes as traced, and it fails only for itself.
    added.recorder.doing = Doing::Fail;
    CHECK(draw(*device, traced_scene("test_denoiser"), 2).pixels == plain.pixels);

    // With ray-traced ambient light, it comes too, as traced.
    added.recorder.calls.clear();
    added.recorder.doing = Doing::Nothing;
    (void)draw(*device, traced_scene("test_denoiser", true));
    REQUIRE(added.recorder.calls.size() == 1);
    CHECK(added.recorder.calls.front().passes == std::array{true, true, true, false, true});

    // With TAA, the motion vectors come too.
    added.recorder.calls.clear();
    render::Scene3D taa = traced_scene("test_denoiser");
    taa.use_taa = true;
    (void)draw(*device, taa);
    REQUIRE(added.recorder.calls.size() == 1);
    CHECK(added.recorder.calls.front().motion_values == std::size_t{24} * 24 * 2);
}

TEST_CASE("A denoiser that doesn't run, or isn't added, leaves the engine's own smoothing") {
    const auto device = render::create_software_device();
    const Image plain = draw(*device, traced_scene("", true));
    {
        const Added refusing(false);
        CHECK(draw(*device, traced_scene("test_denoiser", true)).pixels == plain.pixels);
        CHECK(refusing.recorder.calls.empty());
    }
    CHECK(draw(*device, traced_scene("missing", true)).pixels == plain.pixels);
}

TEST_CASE("The GPU device hands a denoiser its passes as the native API's textures") {
    const auto device = slash::test::gpu_device();
    if (device == nullptr || !device->ray_tracing()) {
        MESSAGE("no GPU that traces rays here, so the GPU device's denoiser isn't checked");
        return;
    }
    Added added;
    (void)draw(*device, traced_scene("test_denoiser"), 2);
    REQUIRE(added.recorder.calls.size() == 2);
    const Seen& first = added.recorder.calls.front();
    CHECK(first.backend == device->backend());
    CHECK(first.commands);
    CHECK(first.depth_image);
    CHECK(first.passes == std::array{true, true, true, false, false});
    CHECK(first.reset);
    CHECK_FALSE(added.recorder.calls.back().reset);
}
