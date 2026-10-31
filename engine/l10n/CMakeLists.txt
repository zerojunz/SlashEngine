add_library(slash_l10n
    src/l10n.cpp
    src/locale.cpp
    src/translation.cpp)
add_library(slash::l10n ALIAS slash_l10n)

target_include_directories(slash_l10n PUBLIC include)
target_link_libraries(slash_l10n
    PUBLIC slash::asset slash::core slash::world
    PRIVATE slash::platform)
slash_engine_target(slash_l10n)
