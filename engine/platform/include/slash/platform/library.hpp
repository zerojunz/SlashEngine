#pragma once

#include "slash/core/result.hpp"

#include <string>

// Shared libraries loaded while the program runs, for a game's own code (engine/gameplay). Not
// available on the web.
namespace slash::platform {

// A shared library (.so, .dylib or .dll) loaded into this process. It stays loaded when the
// object is gone, until the process exits or unload() is called, so code and data that still
// point into it stay valid.
class SharedLibrary {
public:
    using Function = void (*)();

    // Loads a library and resolves every symbol it needs, so one that the process lacks fails
    // here with the loader's message rather than later.
    [[nodiscard]] static Result<SharedLibrary> open(const std::string& path);

    // An exported function, or nullptr.
    [[nodiscard]] Function symbol(const char* name) const;
    // Unloads it. Only for a library none of whose code or data is in use.
    void unload();

private:
    explicit SharedLibrary(void* handle) : handle_(handle) {}

    void* handle_ = nullptr;
};

// Whether the tools on this platform load shared libraries of a game's own code: macOS and Linux
// do. Windows would need the engine built as DLLs, and phones and the web don't allow it.
[[nodiscard]] bool can_load_libraries();

// Where the executable or library holding an address starts: the same for every function and
// variable in one image, nullptr if the address is in none or the platform can't tell (the web).
[[nodiscard]] const void* image_of(const void* address);

} // namespace slash::platform
