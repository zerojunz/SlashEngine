add_executable(slash-mcp main.cpp)
target_link_libraries(slash-mcp PRIVATE
    slash::command slash::exporter slash::importer slash::platform slash::server slash::shader_compiler slash::world)
slash_target_warnings(slash-mcp)
