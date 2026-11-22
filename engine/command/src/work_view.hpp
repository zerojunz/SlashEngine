#pragma once

#include "slash/command/command.hpp"
#include "slash/core/json.hpp"

#include <optional>
#include <string_view>

// What work.cpp's steering commands need of remote viewing in work_view.cpp.
namespace slash::command::work {

// Runs the command in the live session that params' "session" names, through the forwarder, when
// it names another session than this one's; nullopt when it names none or this one.
[[nodiscard]] std::optional<Reply> forward_elsewhere(Session& session, std::string_view command,
                                                     const json::Value& params);

// work.view, work.timeline and work.report.
void register_view_commands(Registry& registry);

} // namespace slash::command::work
