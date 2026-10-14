#include "slash/platform/file.hpp"

#include <SDL3/SDL.h>

namespace slash::platform {

Result<std::string> read_file(std::string_view path) {
    const std::string name(path);
    std::size_t size = 0;
    void* data = SDL_LoadFile(name.c_str(), &size);
    if (data == nullptr) {
        return make_error(ErrorCode::NotFound,
                          "could not read \"" + name + "\": " + SDL_GetError());
    }
    std::string contents(static_cast<const char*>(data), size);
    SDL_free(data);
    return contents;
}

Result<void> write_file(std::string_view path, std::string_view contents) {
    const std::string name(path);
    if (!SDL_SaveFile(name.c_str(), contents.data(), contents.size())) {
        return make_error(ErrorCode::PlatformFailure,
                          "could not write \"" + name + "\": " + SDL_GetError());
    }
    return {};
}

Result<void> create_directories(std::string_view path) {
    const std::string name(path);
    if (!SDL_CreateDirectory(name.c_str())) {
        return make_error(ErrorCode::PlatformFailure,
                          "could not create the directory \"" + name + "\": " + SDL_GetError());
    }
    return {};
}

bool exists(std::string_view path) {
    const std::string name(path);
    return SDL_GetPathInfo(name.c_str(), nullptr);
}

} // namespace slash::platform
