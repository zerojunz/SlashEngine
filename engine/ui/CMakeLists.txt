add_library(slash_ui
    src/draw.cpp
    src/layout.cpp
    src/ui.cpp)
add_library(slash::ui ALIAS slash_ui)

target_include_directories(slash_ui PUBLIC include)
target_link_libraries(slash_ui
    PUBLIC slash::asset slash::core slash::render slash::text slash::world)
slash_engine_target(slash_ui)
