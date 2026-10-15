add_executable(slash-cli main.cpp init.cpp)
target_link_libraries(slash-cli PRIVATE
    slash::command slash::core slash::exporter slash::platform slash::runtime slash::server
    slash::storyboard slash::world)
slash_target_warnings(slash-cli)
