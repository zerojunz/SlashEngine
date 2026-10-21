add_executable(slash-cli main.cpp init.cpp)
target_link_libraries(slash-cli PRIVATE
    slash::ai slash::ai_commands slash::animation slash::audio slash::bench slash::canvas slash::command slash::core slash::debug slash::exporter slash::importer slash::input slash::library slash::library_commands slash::platform slash::render slash::runtime slash::server slash::shader_compiler
    slash::soundtrack slash::soundtrack_commands slash::storyboard slash::text slash::ui slash::world)
slash_target_warnings(slash-cli)
slash_gameplay_host(slash-cli)
