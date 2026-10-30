#pragma once

#include "slash/xr/xr.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace slash::xr::detail {

// A tracker's signal, as Godot's XRPositionalTracker emits them, kept until the xr system sends
// it to the XR nodes following the tracker.
struct Signal {
    enum class Kind : std::uint8_t {
        PoseChanged,
        PoseLost,
        ButtonPressed,
        ButtonReleased,
        FloatChanged,
        Vector2Changed,
        ProfileChanged,
    };
    Kind kind = Kind::PoseChanged;
    std::string tracker;
    std::string name; // the pose, input or profile
    Input value;
};

// What an XR node keeps between ticks, as Godot's XRNode3D: the tracker it is bound to (by its
// generation, which goes up each time a tracker is added) and whether its pose is tracked.
struct NodeState {
    std::string tracker;
    std::string pose;
    std::uint64_t generation = 0;
    bool tracking = false;
};

// The server's state sessions save and put back.
struct Data {
    Settings settings;
    float world_scale = 1.0F;
    world::Affine3D world_origin;
    world::Affine3D reference_frame;
    std::map<std::string, Tracker, std::less<>> trackers;
    std::map<std::string, std::uint64_t, std::less<>> generations;
    std::uint64_t next_generation = 1;
    std::vector<Signal> signals;
    std::map<std::string, NodeState, std::less<>> nodes; // by entity ID
};

// A world's XRServer.
struct Server {
    Data data;
    std::vector<std::shared_ptr<Interface>> interfaces;
    Interface* primary = nullptr;
    TestInterface* test = nullptr;
    // Set while the engine sets the server up, before anything listens: its events are not news.
    bool quiet = false;

    void run(world::World& world);
    void route(world::World& world);
    void signal(Signal signal);
    void emit(world::World& world, std::string kind, json::Value event) const;
};

[[nodiscard]] Server* server_of(const world::World& world);
[[nodiscard]] Server& server(world::World& world);

} // namespace slash::xr::detail
