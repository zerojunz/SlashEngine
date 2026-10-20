add_library(slash_services
    src/services.cpp
    src/settings.cpp)
add_library(slash::services ALIAS slash_services)

target_include_directories(slash_services PUBLIC include)
target_link_libraries(slash_services
    PUBLIC slash::core slash::world
    PRIVATE slash::platform)
slash_engine_target(slash_services)

# Backends written in Java or Objective-C (slash/services/bridges.hpp). The Java interfaces they
# implement are in android/, which Android exports compile into the APK.
if(ANDROID)
    target_sources(slash_services PRIVATE src/android.cpp)
    target_link_libraries(slash_services PRIVATE SDL3::SDL3-static)
elseif(APPLE)
    target_sources(slash_services PRIVATE src/apple.mm)
    set_source_files_properties(src/apple.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(slash_services PRIVATE "-framework Foundation")
endif()
