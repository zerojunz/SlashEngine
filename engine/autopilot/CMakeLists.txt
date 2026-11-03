# Autopilot's game-side plugin (github.com/ramenvr/autopilot, Apache 2.0): a game's link to the
# agent that plays it to find bugs, verify claims and reach goals, and SlashEngine's own agent,
# which plays through the same link with a model from providers.json. See
# docs/reference/autopilot.md.
add_library(slash_autopilot
    src/agent.cpp
    src/bridge.cpp
    src/codex.cpp
    src/rc.cpp
    src/ui.cpp
    src/world.cpp)
add_library(slash::autopilot ALIAS slash_autopilot)

target_include_directories(slash_autopilot PUBLIC include)
target_link_libraries(slash_autopilot
    PUBLIC slash::command slash::core
    PRIVATE slash::ai slash::canvas slash::input slash::net slash::physics2d slash::physics3d
            slash::platform slash::render slash::runtime slash::scene3d slash::ui slash::world)
slash_engine_target(slash_autopilot)
