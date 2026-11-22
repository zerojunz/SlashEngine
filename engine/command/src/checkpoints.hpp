#pragma once

#include <cstdint>
#include <string_view>

// What the journals' upkeep in work.cpp needs of a session's checkpoints.
namespace slash::command::work {

// The bytes a session's checkpoints take.
[[nodiscard]] std::uint64_t checkpoint_bytes(std::string_view project_root,
                                             std::string_view session);
// Removes a session's checkpoints, with their folders.
void remove_checkpoints(std::string_view project_root, std::string_view session);

} // namespace slash::command::work
