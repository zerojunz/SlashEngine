// A vendor's latency reduction (latency.hpp): the registry, making one for a device, and a game's
// loop sleeping before each frame and marking its parts, the device marking its present.

#include "slash/core/json.hpp"
#include "slash/platform/file.hpp"
#include "slash/render/device.hpp"
#include "slash/render/latency.hpp"
#include "slash/runtime/game.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace json = slash::json;
namespace platform = slash::platform;
namespace render = slash::render;
namespace runtime = slash::runtime;

namespace {

// Writes each call as "sleep 1" or "<marker> 1".
class Recorder final : public render::LatencyReducer {
public:
    explicit Recorder(std::vector<std::string>& calls) : calls_(&calls) {}

    void sleep(std::uint64_t frame) override {
        calls_->push_back("sleep " + std::to_string(frame));
    }
    void mark(render::LatencyMarker marker, std::uint64_t frame) override {
        calls_->push_back(std::string(render::latency_marker_name(marker)) + " " +
                          std::to_string(frame));
    }

private:
    std::vector<std::string>* calls_;
};

// Adds "test_reducer" for the test's length, which records into calls.
struct Added {
    std::vector<std::string> calls;
    render::Backend made_for = render::Backend::Vulkan;

    explicit Added(bool runs = true) {
        render::clear_added_latency_reducers();
        REQUIRE(render::add_latency_reducer(
                    {.name = "test_reducer",
                     .supports = [runs](const render::RenderDevice&) { return runs; },
                     .make =
                         [this](const render::NativeDevice& device) {
                             made_for = device.backend;
                             return std::unique_ptr<render::LatencyReducer>(
                                 std::make_unique<Recorder>(calls));
                         }})
                    .ok());
    }
    Added(const Added&) = delete;
    Added& operator=(const Added&) = delete;
    Added(Added&&) = delete;
    Added& operator=(Added&&) = delete;
    ~Added() { render::clear_added_latency_reducers(); }
};

// A project folder that names reducer in rendering.latency_reduction.
std::string project(const std::string& root, const std::string& reducer) {
    REQUIRE(platform::create_directories(root).ok());
    REQUIRE(
        platform::write_file(root + "/main.scene.json", R"({"entities": {}, "format": 1})").ok());
    REQUIRE(platform::write_file(root + "/project.json",
                                 R"({"format": 1, "name": "G", "main_scene": "main.scene.json",
                                     "rendering": {"latency_reduction": ")" +
                                     reducer + R"("}})")
                .ok());
    return root;
}

} // namespace

TEST_CASE("Games add latency reducers by name") {
    render::clear_added_latency_reducers();
    const auto make = [](const render::NativeDevice&) {
        return std::unique_ptr<render::LatencyReducer>();
    };
    CHECK(render::latency_reducer_names().empty());
    CHECK_FALSE(render::add_latency_reducer({.name = "Reflex", .make = make}).ok());
    CHECK_FALSE(render::add_latency_reducer({.name = "reflex"}).ok());
    REQUIRE(render::add_latency_reducer({.name = "reflex", .make = make}).ok());
    CHECK(render::add_latency_reducer({.name = "reflex", .make = make}).error().code ==
          slash::ErrorCode::Conflict);
    REQUIRE(render::add_latency_reducer({.name = "anti_lag_2", .make = make}).ok());
    CHECK(render::latency_reducer_names() == std::vector<std::string>{"reflex", "anti_lag_2"});
    CHECK(render::find_latency_reducer("reflex"));
    CHECK_FALSE(render::find_latency_reducer("xell"));

    // One that makes nothing, or isn't there, isn't made.
    const auto device = render::create_software_device();
    CHECK_FALSE(render::make_latency_reducer("reflex", *device).ok());
    CHECK(render::make_latency_reducer("xell", *device).error().code == slash::ErrorCode::NotFound);
    CHECK_FALSE(render::make_latency_reducer("", *device).ok());
    render::clear_added_latency_reducers();
    CHECK(render::latency_reducer_names().empty());

    CHECK(render::latency_marker_name(render::LatencyMarker::SimulationStart) ==
          "simulation_start");
    CHECK(render::latency_marker_name(render::LatencyMarker::RenderSubmitEnd) ==
          "render_submit_end");
    CHECK(render::latency_marker_name(render::LatencyMarker::PresentStart) == "present_start");
    CHECK(render::latency_marker_name(render::LatencyMarker::InputSample) == "input_sample");
}

TEST_CASE("A latency reducer is made for a device it runs on") {
    const auto device = render::create_software_device();
    {
        Added added;
        auto made = render::make_latency_reducer("test_reducer", *device);
        REQUIRE(made.ok());
        CHECK(added.made_for == device->native_device().backend);
        made.value()->sleep(3);
        CHECK(added.calls == std::vector<std::string>{"sleep 3"});
    }
    const Added refusing(false);
    const auto refused = render::make_latency_reducer("test_reducer", *device);
    REQUIRE_FALSE(refused.ok());
    CHECK(refused.error().message.find("doesn't run on the software device") != std::string::npos);
}

TEST_CASE("A game sleeps before each frame and marks its parts for the latency reducer") {
    Added added;
    auto game = runtime::Game::start(
        {.data_dir = project("runtime_latency", "test_reducer"), .headless = true, .max_ticks = 2});
    REQUIRE_MESSAGE(game.ok(), (game.ok() ? std::string() : game.error().message));
    CHECK(game.value()->summary().find("latency_reduction")->as_bool());
    REQUIRE(game.value()->frame());
    const std::vector<std::string> first{
        "sleep 1",          "simulation_start 1",    "input_sample 1",
        "simulation_end 1", "render_submit_start 1", "render_submit_end 1",
    };
    CHECK(added.calls == first);
    // The device marks its present for the frame, as the GPU and OpenGL devices do around it.
    game.value()->renderer().presenting(false);
    game.value()->renderer().presenting(true);
    CHECK(added.calls.at(6) == "present_start 1");
    CHECK(added.calls.at(7) == "present_end 1");
    added.calls.clear();
    CHECK_FALSE(game.value()->frame()); // the last tick, drawn too
    CHECK(added.calls.front() == "sleep 2");
    CHECK(added.calls.back() == "render_submit_end 2");
}

TEST_CASE("A game whose latency reducer doesn't run goes on without it") {
    const Added refusing(false);
    {
        auto game =
            runtime::Game::start({.data_dir = project("runtime_latency_off", "test_reducer"),
                                  .headless = true,
                                  .max_ticks = 1});
        REQUIRE(game.ok());
        CHECK_FALSE(game.value()->summary().find("latency_reduction")->as_bool());
        CHECK_FALSE(game.value()->frame());
        CHECK(refusing.calls.empty());
    }
    // One game at a time: the first is gone.
    auto missing = runtime::Game::start({.data_dir = project("runtime_latency_missing", "reflex"),
                                         .headless = true,
                                         .max_ticks = 1});
    REQUIRE_MESSAGE(missing.ok(), (missing.ok() ? std::string() : missing.error().message));
    CHECK_FALSE(missing.value()->summary().find("latency_reduction")->as_bool());
}
