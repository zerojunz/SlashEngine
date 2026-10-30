// A cutscene coded lossy with libaom. The frames are made here: a sky that darkens as the sun
// sets, with a ball rolling across it, which no palette of 8 colors a block keeps. The engine's
// own encoder (video::encode_ivf) codes every frame as a lossless key frame of palettes;
// video::encode_lossy has libaom code key frames where kf_interval asks and every other frame
// as changes to the ones before, far smaller. The VideoStreamPlayer "cutscene" plays it over
// the whole window, seeks and finishes. libaom's bytes can differ a little between machines, so
// the output says how close each picture is rather than printing pixels or sizes.
#include "slash/command/builtin.hpp"
#include "slash/command/session.hpp"
#include "slash/platform/file.hpp"
#include "slash/video/lossy_encoder.hpp"
#include "slash/video/video.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

using namespace slash;

namespace {

constexpr std::uint32_t kWidth = 128;
constexpr std::uint32_t kHeight = 72;
constexpr std::size_t kFrames = 48;
constexpr std::uint32_t kRate = 24;

// Frame i: the sky fades from orange to dusk, the sun sinks and the ball rolls left to right.
render::Image sunset_frame(std::size_t i) {
    render::Image image;
    image.width = kWidth;
    image.height = kHeight;
    image.pixels.resize(std::size_t{kWidth} * kHeight * 4);
    const int dusk = static_cast<int>(i) * 2;
    const int sun_y = 20 + (static_cast<int>(i) / 2);
    const int ball_x = 8 + (static_cast<int>(i) * 5 / 2);
    for (std::uint32_t y = 0; y < kHeight; ++y) {
        for (std::uint32_t x = 0; x < kWidth; ++x) {
            const int sx = static_cast<int>(x) - 90;
            const int sy = static_cast<int>(y) - sun_y;
            const int bx = static_cast<int>(x) - ball_x;
            const int by = static_cast<int>(y) - 58;
            int r = 250 - (static_cast<int>(y) * 2) - dusk;
            int g = 150 - static_cast<int>(y) - dusk;
            int b = 60 + static_cast<int>(y) + (dusk / 2);
            if (y >= 56) { // the ground
                r = 60 - (dusk / 4);
                g = 90 + static_cast<int>(x / 8) - (dusk / 4);
                b = 40;
            }
            if ((sx * sx) + (sy * sy) <= 100) {
                r = 255;
                g = 220 - (sy * 3);
                b = 120;
            }
            if ((bx * bx) + (by * by) <= 36) {
                r = 40 + (bx * 8) + 60;
                g = 80 + (by * 8) + 60;
                b = 220;
            }
            std::uint8_t* pixel = &image.pixels[((std::size_t{y} * kWidth) + x) * 4];
            pixel[0] = static_cast<std::uint8_t>(std::clamp(r, 0, 255));
            pixel[1] = static_cast<std::uint8_t>(std::clamp(g, 0, 255));
            pixel[2] = static_cast<std::uint8_t>(std::clamp(b, 0, 255));
            pixel[3] = 255;
        }
    }
    return image;
}

// What 4:2:0 at limited range keeps of a picture: the best any AV1 video shows.
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

// The mean difference of the color channels.
double mean_difference(const render::Image& a, const render::Image& b) {
    long long sum = 0;
    for (std::size_t i = 0; i < a.pixels.size(); ++i) {
        if (i % 4 != 3) {
            sum += std::abs(a.pixels[i] - b.pixels[i]);
        }
    }
    const std::size_t channels = a.pixels.size() / 4 * 3;
    return static_cast<double>(sum) / static_cast<double>(channels);
}

const char* yes(bool value) {
    return value ? "yes" : "no";
}

struct Game {
    world::ComponentRegistry components;
    command::Registry commands;
    std::unique_ptr<command::Session> session;

    Game() {
        world::register_builtin_components(components);
        command::register_builtin_commands(commands);
        command::register_run_commands(commands);
        command::SessionOptions options;
        options.capture_logs = false;
        session = std::make_unique<command::Session>(components, commands, std::move(options));
    }

    [[nodiscard]] json::Value ok(std::string_view command, std::string_view params = "{}") const {
        command::Reply reply = session->call(command, json::parse(params).value());
        if (!reply.ok) {
            std::printf("  %.*s failed: %s\n", static_cast<int>(command.size()), command.data(),
                        reply.message.c_str());
            return {};
        }
        return std::move(reply.value);
    }

    void step(int ticks) const {
        (void)ok("step", R"({"ticks": )" + std::to_string(ticks) + R"(, "tick_seconds": 0.05})");
    }

    // Whether the window's pixels at a few points are within 12 levels of the frame's source.
    [[nodiscard]] bool looks_like(std::size_t frame) const {
        const render::Image source = through_yuv(sunset_frame(frame));
        const int points[4][2] = {
            {20, 10}, {90, 20 + (static_cast<int>(frame) / 2)}, {64, 40}, {100, 64}};
        std::string list;
        for (const auto& point : points) {
            list += (list.empty() ? "[" : ", [") + std::to_string(point[0]) + ", " +
                    std::to_string(point[1]) + "]";
        }
        const json::Value captured = ok("frame.capture", R"({"points": [)" + list + "]}");
        const json::Array& colors = captured.find("colors")->as_array();
        for (std::size_t p = 0; p < 4; ++p) {
            const std::uint8_t* expected =
                &source.pixels[((std::size_t(points[p][1]) * kWidth) + std::size_t(points[p][0])) *
                               4];
            for (std::size_t c = 0; c < 3; ++c) {
                const long got = std::lround(colors.at(p).as_array().at(c).as_double() * 255.0);
                if (std::abs(got - long{expected[c]}) > 12) {
                    return false;
                }
            }
        }
        return true;
    }

    void show(const char* what) const {
        const json::Value got = ok("entity.get", R"({"id": "cutscene"})");
        const json::Value& player = *got.find("components")->find("VideoStreamPlayer");
        const json::Value* position = player.find("stream_position");
        const json::Value* playing = player.find("playing");
        const video::Shown shown = video::shown(session->world(), "cutscene");
        std::printf("%s\n  position %.2f s, %s, frame %zu shown, looks like its source: %s\n", what,
                    position != nullptr ? position->as_double() : 0.0,
                    playing != nullptr && playing->as_bool() ? "playing" : "stopped", shown.frame,
                    yes(looks_like(shown.frame)));
    }
};

} // namespace

int main() {
    std::vector<render::Image> frames;
    for (std::size_t i = 0; i < kFrames; ++i) {
        frames.push_back(sunset_frame(i));
    }
    std::printf("sunset: %zu frames of %ux%u, %u a second\n", kFrames, kWidth, kHeight, kRate);

    const auto palette = video::encode_ivf(frames, kRate);
    video::LossyOptions options;
    options.cq_level = 28;
    options.kf_interval = 24;
    const auto lossy = video::encode_lossy(frames, kRate, 1, options);
    if (!palette || !lossy) {
        std::printf("couldn't encode: %s\n",
                    (!palette ? palette.error() : lossy.error()).message.c_str());
        return 1;
    }
    const auto file = video::read_ivf(lossy.value());
    if (!file) {
        std::printf("couldn't read the clip: %s\n", file.error().message.c_str());
        return 1;
    }
    std::string keys;
    for (std::size_t i = 0; i < file.value().frames.size(); ++i) {
        if (file.value().frames[i].key) {
            keys += (keys.empty() ? "" : ", ") + std::to_string(i);
        }
    }
    std::printf("video::encode_ivf, the palette encoder: every frame a lossless key frame\n");
    std::printf("video::encode_lossy, libaom at cq_level 28, kf_interval 24:\n");
    std::printf("  %zu frames, key frames at %s\n", file.value().frames.size(), keys.c_str());
    std::printf("  under a tenth of the palette clip's size: %s\n",
                yes(lossy.value().size() * 10 < palette.value().size()));
    video::Decoder decoder;
    double worst = 0.0;
    render::Image out;
    for (std::size_t i = 0; i < kFrames; ++i) {
        if (!decoder.decode(file.value(), i, out)) {
            std::printf("couldn't decode frame %zu\n", i);
            return 1;
        }
        worst = std::max(worst, mean_difference(through_yuv(frames[i]), out));
    }
    std::printf("  every frame within 3 levels of its source on average: %s\n", yes(worst <= 3.0));

    if (!platform::write_file("sunset.ivf",
                              std::string(lossy.value().begin(), lossy.value().end()))) {
        std::printf("couldn't write sunset.ivf\n");
        return 1;
    }
    const Game game;
    (void)game.ok("scene.open", R"({"path": "main.scene.json"})");
    game.step(1);
    game.show("tick 1: autoplay starts the cutscene");
    game.step(12);
    game.show("12 ticks on, frames predicted from the key frame");
    (void)game.ok("component.set", R"({"id": "cutscene", "components": {"VideoStreamPlayer":
                                    {"paused": true, "stream_position": 1.5}}})");
    game.step(1);
    game.show("paused and seeked to 1.5 s: decoded on from the key frame at 1 s");
    (void)game.ok("component.set",
                  R"({"id": "cutscene", "components": {"VideoStreamPlayer": {"paused": false}}})");
    game.step(12);
    game.show("12 ticks later: past its 2 s, it stops on the last frame");
    return 0;
}
