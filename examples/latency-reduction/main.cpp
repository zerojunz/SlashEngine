// A vendor's latency reduction, as NVIDIA Reflex, AMD Anti-Lag 2 or Intel XeLL come in from an
// SDK package: render::add_latency_reducer adds one, project.json's
// rendering.latency_reduction names it, and the game makes it for its render device when it
// starts.
//
// Each frame the game calls its sleep before the frame starts (a vendor's library waits there so
// input is read as late as it can be), then marks where the frame's simulation, input and drawing
// begin and end. The GPU and OpenGL devices mark the present too, around handing each frame to the
// display; this game runs headless, on the null device, so it has no present to mark.
//
// This one, "printer", prints each call.

#include "slash/render/device.hpp"
#include "slash/render/latency.hpp"
#include "slash/runtime/game.hpp"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

namespace {

namespace render = slash::render;

class Printer final : public render::LatencyReducer {
public:
    void sleep(std::uint64_t frame) override {
        std::printf("frame %llu: sleep\n", static_cast<unsigned long long>(frame));
    }
    void mark(render::LatencyMarker marker, std::uint64_t frame) override {
        const std::string name(render::latency_marker_name(marker));
        std::printf("frame %llu: %s\n", static_cast<unsigned long long>(frame), name.c_str());
    }
};

} // namespace

int main() {
    // An SDK package's register function would add the vendor's, with a supports function that
    // checks the GPU and driver, and make it for the device's native objects.
    if (auto added = render::add_latency_reducer(
            {.name = "printer",
             .supports = {},
             .make =
                 [](const render::NativeDevice& device) {
                     std::printf("made for the %s device\n",
                                 std::string(render::backend_name(device.backend)).c_str());
                     return std::unique_ptr<render::LatencyReducer>(std::make_unique<Printer>());
                 }});
        !added) {
        std::fprintf(stderr, "%s\n", added.error().message.c_str());
        return 1;
    }
    auto game = slash::runtime::Game::start({.data_dir = ".", .headless = true, .max_ticks = 2});
    if (!game) {
        std::fprintf(stderr, "%s\n", game.error().message.c_str());
        return 1;
    }
    while (game.value()->frame()) {
    }
    std::printf("started: %s\n",
                game.value()->summary().find("latency_reduction")->as_bool() ? "yes" : "no");
    return 0;
}
