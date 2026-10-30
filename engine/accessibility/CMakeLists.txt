add_library(slash_accessibility src/tree.cpp)
add_library(slash::accessibility ALIAS slash_accessibility)

target_include_directories(slash_accessibility PUBLIC include)
target_link_libraries(slash_accessibility PUBLIC slash::core slash::ui slash::world
    PRIVATE SDL3::SDL3-static)
slash_engine_target(slash_accessibility)

# The screen reader adapters, over AccessKit where cargo built it (cmake/Dependencies.cmake).
if(TARGET slash_accesskit)
    target_sources(slash_accessibility PRIVATE src/accesskit.cpp)
    target_link_libraries(slash_accessibility PUBLIC slash_accesskit)
    add_dependencies(slash_accessibility slash_accesskit_build)
    if(WIN32)
        # accesskit.h includes <windows.h>.
        target_compile_definitions(slash_accessibility PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    elseif(APPLE)
        target_sources(slash_accessibility PRIVATE src/apple.mm)
        set_source_files_properties(src/apple.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    endif()
else()
    target_sources(slash_accessibility PRIVATE src/accesskit_none.cpp)
endif()
