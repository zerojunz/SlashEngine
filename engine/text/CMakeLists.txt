# The default font, built into the engine from the fetched Roboto (cmake/embed.cmake).
set(_slash_default_font ${roboto_SOURCE_DIR}/src/hinted/Roboto-Regular.ttf)
set(_slash_default_font_cpp ${CMAKE_CURRENT_BINARY_DIR}/default_font.cpp)
add_custom_command(
    OUTPUT ${_slash_default_font_cpp}
    COMMAND ${CMAKE_COMMAND} -DINPUT=${_slash_default_font} -DOUTPUT=${_slash_default_font_cpp}
            -DNAMESPACE=slash::text::detail -DNAME=kDefaultFont
            -P ${PROJECT_SOURCE_DIR}/cmake/embed.cmake
    DEPENDS ${_slash_default_font} ${PROJECT_SOURCE_DIR}/cmake/embed.cmake
    COMMENT "Building the default font into the engine"
    VERBATIM)

add_library(slash_text
    src/box_font.cpp
    src/editing.cpp
    src/font.cpp
    src/fonts.cpp
    ${_slash_default_font_cpp})
add_library(slash::text ALIAS slash_text)

target_include_directories(slash_text PUBLIC include)
target_link_libraries(slash_text
    PUBLIC slash::asset slash::core
    PRIVATE freetype slash::platform slash_harfbuzz slash_msdfgen slash_sheenbidi)
slash_engine_target(slash_text)
