#include "slash/command/builtin.hpp"
#include "slash/command/session.hpp"
#include "slash/platform/file.hpp"
#include "slash/video/video.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace slash;

namespace {

render::Image image_of(std::uint32_t width, std::uint32_t height,
                       const auto& color /* (x, y) -> std::array<uint8_t, 3> */) {
    render::Image image;
    image.width = width;
    image.height = height;
    image.pixels.resize(static_cast<std::size_t>(width) * height * 4);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto rgb = color(x, y);
            std::uint8_t* pixel = &image.pixels[((static_cast<std::size_t>(y) * width) + x) * 4];
            pixel[0] = rgb[0];
            pixel[1] = rgb[1];
            pixel[2] = rgb[2];
            pixel[3] = 255;
        }
    }
    return image;
}

// The largest difference of any channel between two images of one size.
int largest_difference(const render::Image& a, const render::Image& b) {
    REQUIRE(a.width == b.width);
    REQUIRE(a.height == b.height);
    int largest = 0;
    for (std::size_t i = 0; i < a.pixels.size(); ++i) {
        largest = std::max(largest, std::abs(a.pixels[i] - b.pixels[i]));
    }
    return largest;
}

// The image as 4:2:0 at limited range keeps it, which is the best any AV1 video can show.
render::Image through_yuv(const render::Image& image) {
    const video::Yuv420 yuv = video::to_yuv420(image);
    video::YuvPicture picture;
    picture.width = yuv.width;
    picture.height = yuv.height;
    picture.planes[0] = yuv.y.data();
    picture.planes[1] = yuv.u.data();
    picture.planes[2] = yuv.v.data();
    picture.strides[0] = yuv.width;
    picture.strides[1] = (yuv.width + 1) / 2;
    render::Image back;
    video::to_rgba(picture, back);
    return back;
}

// The mean difference of the channels, in hundredths.
int mean_difference(const render::Image& a, const render::Image& b) {
    long long sum = 0;
    for (std::size_t i = 0; i < a.pixels.size(); ++i) {
        sum += std::abs(a.pixels[i] - b.pixels[i]);
    }
    return static_cast<int>(sum * 100 / static_cast<long long>(a.pixels.size()));
}

} // namespace

TEST_CASE("video: YUV 4:2:0 converts back to the same flat colors") {
    const render::Image flat = image_of(16, 8, [](std::uint32_t x, std::uint32_t) {
        return x < 8 ? std::array<std::uint8_t, 3>{200, 40, 90}
                     : std::array<std::uint8_t, 3>{10, 220, 160};
    });
    const video::Yuv420 yuv = video::to_yuv420(flat);
    video::YuvPicture picture;
    picture.width = yuv.width;
    picture.height = yuv.height;
    picture.planes[0] = yuv.y.data();
    picture.planes[1] = yuv.u.data();
    picture.planes[2] = yuv.v.data();
    picture.strides[0] = yuv.width;
    picture.strides[1] = (yuv.width + 1) / 2;
    render::Image back;
    video::to_rgba(picture, back);
    CHECK(largest_difference(flat, back) <= 2);
}

TEST_CASE("video: the palette encoder's frames decode with dav1d") {
    // Four flat quarters, then a gradient and stripes that need more than 8 colors a block.
    const render::Image quarters = image_of(128, 64, [](std::uint32_t x, std::uint32_t y) {
        if (y < 32) {
            return x < 64 ? std::array<std::uint8_t, 3>{255, 0, 0}
                          : std::array<std::uint8_t, 3>{0, 255, 0};
        }
        return x < 64 ? std::array<std::uint8_t, 3>{0, 0, 255}
                      : std::array<std::uint8_t, 3>{255, 255, 255};
    });
    const render::Image busy = image_of(128, 64, [](std::uint32_t x, std::uint32_t y) {
        return std::array<std::uint8_t, 3>{
            static_cast<std::uint8_t>(x * 2), static_cast<std::uint8_t>(y * 4),
            static_cast<std::uint8_t>((((x / 3) + y) % 2) != 0 ? 200 : 30)};
    });
    const std::vector<render::Image> frames{quarters, busy};
    const auto bytes = video::encode_ivf(frames, 30);
    REQUIRE(bytes);
    const auto file = video::read_ivf(bytes.value());
    REQUIRE(file);
    CHECK(file.value().width == 128);
    CHECK(file.value().height == 64);
    REQUIRE(file.value().frames.size() == 2);
    CHECK(file.value().frames[0].key);
    CHECK(file.value().frames[1].key);

    video::Decoder decoder;
    render::Image out;
    const auto first = decoder.decode(file.value(), 0, out);
    if (!first) {
        FAIL(first.error().message);
    }
    CHECK(largest_difference(quarters, out) <= 2);
    const auto second = decoder.decode(file.value(), 1, out);
    if (!second) {
        FAIL(second.error().message);
    }
    // More than 8 colors a block: close to what 4:2:0 keeps of it.
    CHECK(largest_difference(through_yuv(busy), out) <= 16);
    CHECK(mean_difference(through_yuv(busy), out) <= 200);
}

namespace {

json::Value parse_json(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? std::string() : parsed.error().message));
    return std::move(parsed).value();
}

struct VideoEngine {
    world::ComponentRegistry components;
    command::Registry commands;
    std::unique_ptr<command::Session> session;

    explicit VideoEngine(const std::string& root) {
        world::register_builtin_components(components);
        command::register_builtin_commands(commands);
        command::register_run_commands(commands);
        command::SessionOptions options;
        options.project_root = root;
        options.capture_logs = false;
        session = std::make_unique<command::Session>(components, commands, std::move(options));
    }

    [[nodiscard]] json::Value ok(std::string_view name, std::string_view params = "{}") const {
        command::Reply reply = session->call(name, parse_json(params));
        REQUIRE_MESSAGE(reply.ok, reply.message);
        return std::move(reply.value);
    }

    // The color at a window pixel, in 0..255.
    [[nodiscard]] std::array<int, 3> color_at(int x, int y) const {
        const json::Value captured = ok("frame.capture", R"({"points": [[)" + std::to_string(x) +
                                                             ", " + std::to_string(y) + "]]}");
        const json::Array& color = captured.find("colors")->as_array().at(0).as_array();
        return {static_cast<int>(std::lround(color.at(0).as_double() * 255.0)),
                static_cast<int>(std::lround(color.at(1).as_double() * 255.0)),
                static_cast<int>(std::lround(color.at(2).as_double() * 255.0))};
    }
};

bool near(std::array<int, 3> a, std::array<int, 3> b) {
    for (std::size_t i = 0; i < 3; ++i) {
        if (std::abs(a.at(i) - b.at(i)) > 3) {
            return false;
        }
    }
    return true;
}

render::Image flat(std::array<std::uint8_t, 3> color) {
    return image_of(32, 16, [color](std::uint32_t, std::uint32_t) { return color; });
}

} // namespace

TEST_CASE("video: a VideoStreamPlayer plays in its control and on sprites, then finishes") {
    const std::string root = "video_player_project";
    std::filesystem::remove_all(root);
    REQUIRE(platform::create_directories(root).ok());
    REQUIRE(platform::write_file(root + "/project.json", R"({"format": 1, "name": "Video",
        "main_scene": "main.scene.json", "version": "1.0.0", "window": {"width": 160, "height": 90},
        "rendering": {"clear_color": [0, 0, 0, 1]}})")
                .ok());
    // Red, green and blue, ten frames a second.
    const std::vector<render::Image> frames{flat({255, 0, 0}), flat({0, 255, 0}),
                                            flat({0, 0, 255})};
    const auto clip = video::encode_ivf(frames, 10);
    REQUIRE(clip);
    REQUIRE(platform::write_file(root + "/clip.ivf",
                                 std::string(clip.value().begin(), clip.value().end()))
                .ok());
    VideoEngine engine(root);
    (void)engine.ok("entity.create", R"({"id": "tv", "components": {
        "Control": {"offset_left": 10, "offset_top": 10},
        "VideoStreamPlayer": {"stream": "clip.ivf", "autoplay": true}}})");
    (void)engine.ok("entity.create", R"({"id": "screen", "components": {
        "Transform2D": {"position": [100, 50]}, "Sprite2D": {"texture": "video:tv"}}})");
    // Nothing decoded before the first tick.
    CHECK(near(engine.color_at(20, 15), {0, 0, 0}));

    // Autoplay starts it on the first tick, which shows the first frame.
    (void)engine.ok("step", R"({"tick_seconds": 0.05})");
    CHECK(near(engine.color_at(20, 15), {255, 0, 0}));
    CHECK(near(engine.color_at(100, 50), {255, 0, 0}));
    // The control is the video's size.
    CHECK(engine.ok("ui.layout", R"({"ids": ["tv"]})") ==
          parse_json(R"({"controls": [{"id": "tv", "rect": [10, 10, 32, 16]}],
                         "viewport": [160, 90]})"));

    (void)engine.ok("step", R"({"ticks": 2, "tick_seconds": 0.05})");
    CHECK(near(engine.color_at(20, 15), {0, 255, 0}));
    (void)engine.ok("step", R"({"ticks": 4, "tick_seconds": 0.05})");
    CHECK(near(engine.color_at(20, 15), {0, 0, 255}));
    // At the end it stops on the last frame.
    const auto player = [&engine] {
        return *engine.ok("entity.get", R"({"id": "tv"})")
                    .find("components")
                    ->find("VideoStreamPlayer");
    };
    CHECK_FALSE(player().find("playing")->as_bool());
    CHECK(player().find("stream_position")->as_double() == doctest::Approx(0.3));
    CHECK(video::stream_length(engine.session->world(), "tv") == doctest::Approx(0.3));

    // Looping, it starts again; stopped, setting the position seeks.
    (void)engine.ok("component.set", R"({"id": "tv", "components": {"VideoStreamPlayer":
        {"loop": true, "playing": true, "stream_position": 0.2}}})");
    (void)engine.ok("step", R"({"ticks": 3, "tick_seconds": 0.05})");
    CHECK(near(engine.color_at(20, 15), {255, 0, 0}));
    CHECK(player().find("playing")->as_bool());
    (void)engine.ok("component.set", R"({"id": "tv", "components": {"VideoStreamPlayer":
        {"playing": false, "stream_position": 0.15}}})");
    (void)engine.ok("step", R"({"tick_seconds": 0.05})");
    CHECK(near(engine.color_at(20, 15), {0, 255, 0}));
    CHECK(near(engine.color_at(100, 50), {0, 255, 0}));

    // A stream that doesn't load is a layout problem.
    (void)engine.ok(
        "component.set",
        R"({"id": "tv", "components": {"VideoStreamPlayer": {"stream": "missing.ivf"}}})");
    CHECK(engine.ok("ui.layout", R"({"ids": ["tv"]})") == parse_json(R"({
        "controls": [{"id": "tv", "rect": [10, 10, 0, 0]}],
        "problems": [{"id": "tv", "path": "/components/VideoStreamPlayer/stream",
                      "message": "asset \"missing.ivf\" could not be read: no such file"}],
        "viewport": [160, 90]})"));
}
