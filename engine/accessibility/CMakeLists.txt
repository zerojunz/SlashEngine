add_library(slash_accessibility src/dom.cpp src/mobile.cpp src/tree.cpp)
add_library(slash::accessibility ALIAS slash_accessibility)

target_include_directories(slash_accessibility PUBLIC include)
target_link_libraries(slash_accessibility PUBLIC slash::core slash::ui slash::world
    PRIVATE SDL3::SDL3-static)
slash_engine_target(slash_accessibility)

# The screen reader adapters: the page's own DOM on the web (src/web.cpp), virtual views for
# TalkBack (src/android.cpp, with the Java bridge in android/, which Android exports compile),
# UIAccessibilityElements for VoiceOver on iOS (src/ios.mm), and AccessKit's where cargo built it
# (cmake/Dependencies.cmake).
if(EMSCRIPTEN)
    target_sources(slash_accessibility PRIVATE src/web.cpp)
elseif(ANDROID)
    target_sources(slash_accessibility PRIVATE src/android.cpp)
elseif(IOS)
    target_sources(slash_accessibility PRIVATE src/ios.mm)
    set_source_files_properties(src/ios.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
elseif(TARGET slash_accesskit)
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
