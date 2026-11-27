#pragma once

#include "slash/core/result.hpp"
#include "slash/render/device.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Latency reduction from a vendor's SDK (NVIDIA Reflex, AMD Anti-Lag 2, Intel XeLL): a game adds
// one with add_latency_reducer, from an SDK package's glue code, and names it in project.json's
// rendering.latency_reduction. Each frame the game's loop calls its sleep before the frame starts,
// which waits so the GPU isn't queued frames ahead, and marks where the frame's simulation, input,
// render submission and present begin and end, which the vendor's library times. Nothing of the
// vendor's is in the engine: only these calls.
namespace slash::render {

// Where in a frame the game is, as Reflex's markers name them (Anti-Lag 2 and XeLL take the same
// points).
enum class LatencyMarker : std::uint8_t {
    SimulationStart,   // the frame's ticks begin
    SimulationEnd,     // its ticks are done
    RenderSubmitStart, // the frame's drawing begins
    RenderSubmitEnd,   // its drawing is sent to the GPU
    PresentStart,      // just before the frame is handed to the display
    PresentEnd,        // just after
    InputSample,       // the frame reads the player's input
};

// "simulation_start", "simulation_end", "render_submit_start", "render_submit_end",
// "present_start", "present_end" or "input_sample".
[[nodiscard]] std::string_view latency_marker_name(LatencyMarker marker);

// A vendor's latency reduction for one device, made when the game starts.
class LatencyReducer {
public:
    LatencyReducer() = default;
    LatencyReducer(const LatencyReducer&) = delete;
    LatencyReducer& operator=(const LatencyReducer&) = delete;
    LatencyReducer(LatencyReducer&&) = delete;
    LatencyReducer& operator=(LatencyReducer&&) = delete;
    virtual ~LatencyReducer() = default;

    // Called before frame starts, on the game's thread: waits as long as the vendor's library
    // says, so input is read as late as it can be.
    virtual void sleep(std::uint64_t frame) = 0;
    // Called at each marker of frame, in the order LatencyMarker lists the simulation's and
    // rendering's (input between the simulation's two), then the present's for each frame shown.
    virtual void mark(LatencyMarker marker, std::uint64_t frame) = 0;
};

// A latency reducer a game can choose by name.
struct LatencyReducerInfo {
    std::string name;
    // Whether it runs on a device, such as a vendor's on its own GPUs; always, when empty.
    // NOLINTNEXTLINE(readability-redundant-member-init): a call may leave it out
    std::function<bool(const RenderDevice& device)> supports{};
    // Makes it for the device's native objects (RenderDevice::native_device: empty on the
    // software and null devices).
    // NOLINTNEXTLINE(readability-redundant-member-init): a call may leave it out
    std::function<std::unique_ptr<LatencyReducer>(const NativeDevice& device)> make{};
};

// Adds a latency reducer for rendering.latency_reduction to name, for the whole process, as an
// SDK package's register function can. Fails for one without make, a name already taken or one
// that isn't lowercase letters, digits and underscores.
[[nodiscard]] Result<void> add_latency_reducer(LatencyReducerInfo info);
// The latency reducer added with that name, or nullopt.
[[nodiscard]] std::optional<LatencyReducerInfo> find_latency_reducer(std::string_view name);
// Every latency reducer's name, in the order they were added.
[[nodiscard]] std::vector<std::string> latency_reducer_names();
// Forgets the latency reducers add_latency_reducer added, for tests.
void clear_added_latency_reducers();

// The latency reducer of that name made for a device, or an error saying why there is none: no
// name, none added by it, its supports says no, or make made nothing.
[[nodiscard]] Result<std::unique_ptr<LatencyReducer>>
make_latency_reducer(std::string_view name, const RenderDevice& device);

} // namespace slash::render
