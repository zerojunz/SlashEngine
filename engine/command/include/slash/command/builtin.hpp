#pragma once

#include "slash/command/command.hpp"

#include <optional>
#include <string_view>

namespace slash::platform {
class Loop;
class Platform;
} // namespace slash::platform

namespace slash::render {
class RenderDevice;
struct PathTrace3D;
} // namespace slash::render

namespace slash::canvas {
class CanvasRenderer;
} // namespace slash::canvas

namespace slash::command {

class Session;

// Adds the engine's built-in commands:
//
//   engine.describe, schema.get                     learn what exists and how to call it
//   scene.get, scene.new, scene.open, scene.save    whole scenes and their files
//   entity.create, entity.destroy, entity.get       single entities
//   component.set, component.remove                 component fields
//   world.query                                     find entities by component and field value
//   world.snapshot, world.diff, world.restore       compare against and go back to a saved copy
//   step                                            advance the simulation
//   undo, redo, history.get                         undo history
//   batch                                           run many commands as one, all or nothing
//   events.subscribe, events.read, events.unsubscribe  queued world events
//   log.tail                                        recent log lines
//   replay.record, replay.stop, replay.run          record a run and play it back (replay.hpp)
void register_builtin_commands(Registry& registry);

// Adds play and pause, for hosts that tick the world on their own clock and step it only while
// Session::playing() is true, such as slash-cli run --serve.
void register_run_commands(Registry& registry);

// The parts of a running game that the display and perf commands reach. They must outlive the
// registry's use; the first three are required.
struct LiveGame {
    platform::Platform* platform = nullptr;
    platform::Loop* loop = nullptr;
    render::RenderDevice* renderer = nullptr;
    // What draws the game's frames, for the upscaler and render size in effect.
    const canvas::CanvasRenderer* canvas = nullptr;
    // How the game's frames path trace its 3D world (canvas::CanvasOptions::path_trace), which
    // display.set changes; null when they can't.
    std::optional<render::PathTrace3D>* path_trace = nullptr;
};

// Adds display.list (displays and refresh rates), display.set (V-Sync, the frame cap, the
// render scale and path tracing) and perf.stats (frame pacing) for a running game.
void register_live_commands(Registry& registry, const LiveGame& game);

// Opens a scene file if it exists, or else starts an empty scene that scene.save will write
// there. Used by the tools' --scene option.
Reply open_or_new_scene(Session& session, std::string_view path);

} // namespace slash::command
