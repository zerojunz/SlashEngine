#pragma once

#include "slash/command/command.hpp"

// The video commands for the tools (slash::video_commands): video.encode, which codes image files,
// such as the frames frame.capture writes, as an AV1 video with libaom.
namespace slash::video {

void register_commands(command::Registry& registry);

} // namespace slash::video
