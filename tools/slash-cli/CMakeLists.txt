add_executable(slash-cli main.cpp export.cpp init.cpp)
target_link_libraries(slash-cli PRIVATE
    slash::command slash::core slash::platform slash::runtime slash::server slash::world)
# Where export finds the engine sources when SLASH_ENGINE_DIR and an installed copy are absent.
target_compile_definitions(slash-cli PRIVATE SLASH_ENGINE_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
slash_target_warnings(slash-cli)
