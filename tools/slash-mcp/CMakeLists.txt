add_executable(slash-mcp main.cpp)
target_link_libraries(slash-mcp PRIVATE
    slash::ai_commands slash::command slash::exporter slash::gameplay slash::importer slash::platform slash::runtime slash::server
    slash::shader_compiler slash::world)
slash_target_warnings(slash-mcp)
slash_gameplay_host(slash-mcp)
