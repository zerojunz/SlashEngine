#include "slash/platform/library.hpp"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_loadso.h>
#include <SDL3/SDL_platform_defines.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif !defined(__EMSCRIPTEN__)
#include <dlfcn.h>
#endif

namespace slash::platform {

Result<SharedLibrary> SharedLibrary::open(const std::string& path) {
    SDL_SharedObject* handle = SDL_LoadObject(path.c_str());
    if (handle == nullptr) {
        return make_error(ErrorCode::PlatformFailure, SDL_GetError());
    }
    return SharedLibrary(handle);
}

SharedLibrary::Function SharedLibrary::symbol(const char* name) const {
    if (handle_ == nullptr) {
        return nullptr;
    }
    return SDL_LoadFunction(static_cast<SDL_SharedObject*>(handle_), name);
}

void SharedLibrary::unload() {
    if (handle_ != nullptr) {
        SDL_UnloadObject(static_cast<SDL_SharedObject*>(handle_));
        handle_ = nullptr;
    }
}

bool can_load_libraries() {
#if defined(SDL_PLATFORM_MACOS) || (defined(SDL_PLATFORM_LINUX) && !defined(SDL_PLATFORM_ANDROID))
    return true;
#else
    return false;
#endif
}

const void* image_of(const void* address) {
#if defined(_WIN32)
    HMODULE module = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(address), &module) == 0) {
        return nullptr;
    }
    return module;
#elif defined(__EMSCRIPTEN__)
    (void)address;
    return nullptr;
#else
    Dl_info info{};
    if (dladdr(address, &info) == 0) {
        return nullptr;
    }
    return info.dli_fbase;
#endif
}

} // namespace slash::platform
