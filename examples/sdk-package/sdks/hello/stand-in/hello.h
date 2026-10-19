// The stand-in vendor SDK's one header: what a closed SDK ships beside its binaries.
#pragma once

#if defined(_WIN32) && !defined(HELLO_STATIC)
#ifdef HELLO_BUILDING
#define HELLO_API __declspec(dllexport)
#else
#define HELLO_API __declspec(dllimport)
#endif
#elif defined(__GNUC__)
#define HELLO_API __attribute__((visibility("default")))
#else
#define HELLO_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Starts the SDK with the game's app ID, as a vendor SDK's init call does. Returns 1 if the ID
// looks right (not empty), else 0.
HELLO_API int hello_init(const char* app_id);

// A greeting from the SDK, once it has started.
HELLO_API const char* hello_greeting(void);

#ifdef __cplusplus
}
#endif
