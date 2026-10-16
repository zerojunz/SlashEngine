add_library(slash_ui
    src/draw.cpp
    src/gui.cpp
    src/layout.cpp
    src/line_edit.cpp
    src/style.cpp
    src/text_edit.cpp
    src/text_editor.cpp
    src/theme.cpp
    src/ui.cpp)
add_library(slash::ui ALIAS slash_ui)

target_include_directories(slash_ui PUBLIC include)
target_link_libraries(slash_ui
    PUBLIC slash::asset slash::core slash::input slash::render slash::text slash::world)
slash_engine_target(slash_ui)
# Rounded style boxes and rotated controls come out the same on every platform: no fused
# multiply-adds, which arm64 compilers would otherwise emit and x86-64 ones not.
target_compile_options(slash_ui PRIVATE
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>)
