#pragma once

#include <string>
#include <string_view>

namespace slash::server {

// The page GET /watch serves on a watch server (ServerOptions::watch_session): what the agent
// in `session` did, newest first, with each command's diff, its tests and checks, the work
// journal's summary, the agent's plan and a frame of its world. It reads the token from the
// link's fragment (/watch#token=...) and asks the server with it, as any watcher does.
[[nodiscard]] std::string watch_page(std::string_view session);

} // namespace slash::server
