#pragma once

#include "slash/command/command.hpp"

#include <string_view>

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
void register_builtin_commands(Registry& registry);

// Adds play and pause, for hosts that tick the world on their own clock and step it only while
// Session::playing() is true, such as slash-cli run --serve.
void register_run_commands(Registry& registry);

// Opens a scene file if it exists, or else starts an empty scene that scene.save will write
// there. Used by the tools' --scene option.
Reply open_or_new_scene(Session& session, std::string_view path);

} // namespace slash::command
