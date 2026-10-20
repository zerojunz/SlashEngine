#pragma once

#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"

#include <optional>

// Shared by the soundtrack commands' sources.
namespace slash::soundtrack {

// Loads the storyboard's entries into entries, or returns a failure naming its first problem.
[[nodiscard]] std::optional<command::Reply> load_storyboard(command::Session& session,
                                                            json::Object& entries);

// soundtrack.check (src/check.cpp).
[[nodiscard]] command::Reply soundtrack_check(command::Session& session, const json::Value& params);

} // namespace slash::soundtrack
