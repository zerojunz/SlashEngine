add_library(slash_asset
    src/assets.cpp)
add_library(slash::asset ALIAS slash_asset)

target_include_directories(slash_asset PUBLIC include)
target_link_libraries(slash_asset PUBLIC slash::core slash::platform EnTT::EnTT)
target_compile_definitions(slash_asset PUBLIC ENTT_NOEXCEPTION)
slash_engine_target(slash_asset)
