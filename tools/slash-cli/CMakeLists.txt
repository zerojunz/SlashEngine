add_executable(slash-cli main.cpp init.cpp)
target_link_libraries(slash-cli PRIVATE
    slash::animation slash::audio slash::canvas slash::command slash::core slash::debug slash::exporter slash::importer slash::input slash::platform slash::render slash::runtime slash::server
    slash::storyboard slash::text slash::ui slash::world)
slash_target_warnings(slash-cli)
