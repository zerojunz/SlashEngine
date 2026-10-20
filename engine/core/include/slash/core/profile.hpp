#pragma once

// Zones and frame marks for the Tracy profiler (https://github.com/wolfpld/tracy), as Godot's
// profiler=tracy build option. In a build configured with -DSLASH_PROFILER=tracy (the tracy
// preset), they go to a Tracy profiler connected over the loopback interface, which shows every
// thread's zones on a timeline; in any other build they compile to nothing.
//
//   SLASH_PROFILE_ZONE("physics2d step");   // a zone from here to the end of the scope
//   SLASH_PROFILE_ZONE_NAMED(system.name);  // the same, named at run time by a string_view
//   SLASH_PROFILE_FRAME();                  // marks the start of a frame
//   SLASH_PROFILE_THREAD("worker 1");       // names the calling thread

#if defined(SLASH_TRACY)

#include <tracy/Tracy.hpp>

#define SLASH_PROFILE_ZONE(name) ZoneScopedN(name)
#define SLASH_PROFILE_ZONE_NAMED(name)                                                             \
    ZoneScoped;                                                                                    \
    ZoneName((name).data(), (name).size())
#define SLASH_PROFILE_FRAME() FrameMark
#define SLASH_PROFILE_THREAD(name) ::tracy::SetThreadName(name)

#else

#define SLASH_PROFILE_ZONE(name) static_cast<void>(0)
#define SLASH_PROFILE_ZONE_NAMED(name) static_cast<void>(0)
#define SLASH_PROFILE_FRAME() static_cast<void>(0)
#define SLASH_PROFILE_THREAD(name) static_cast<void>(0)

#endif
