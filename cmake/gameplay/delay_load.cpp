// Part of every gameplay library built on Windows (cmake/gameplay/CMakeLists.txt). The library
// calls the engine through slash-host.lib, an import library naming a program "slash-host.exe"
// whose exports are the engine's (engine/gameplay/CMakeLists.txt). The program is really
// slash-cli, slash-mcp, slash-game or a test, already running, so the library delay-loads it
// (/DELAYLOAD:slash-host.exe) and this hook hands the delay-load helper the program itself.
//
// slash_game_bind resolves every engine function at once, before the game's code runs: the host
// calls it after loading the library and refuses the library, naming a function, when the
// program doesn't have one (a library built against another engine build), rather than letting
// the first call to it end the program.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// The hooks as variables, as every version of delayimp.h declares them then.
#define DELAYIMP_INSECURE_WRITABLE_HOOKS
#include <cstdlib>
#include <cstring>
#include <delayimp.h>

namespace {

constexpr const char* kHost = "slash-host.exe";

// The first engine function the program didn't have, or null.
const char* missing = nullptr;

// What a missing function is bound to: the host refuses the library first, so it never runs.
void missing_function() {
    std::abort();
}

FARPROC WINAPI notify(unsigned reason, PDelayLoadInfo info) {
    if (info == nullptr || info->szDll == nullptr || _stricmp(info->szDll, kHost) != 0) {
        return nullptr;
    }
    if (reason == dliNotePreLoadLibrary) {
        return reinterpret_cast<FARPROC>(GetModuleHandleW(nullptr));
    }
    if (reason == dliFailGetProc) {
        if (missing == nullptr) {
            missing = info->dlp.fImportByName ? info->dlp.szProcName : "a function by number";
        }
        return reinterpret_cast<FARPROC>(&missing_function);
    }
    return nullptr;
}

} // namespace

extern "C" PfnDliHook __pfnDliNotifyHook2 = notify;
extern "C" PfnDliHook __pfnDliFailureHook2 = notify;

extern "C" __declspec(dllexport) const char* slash_game_bind() {
    (void)__HrLoadAllImportsForDll(kHost);
    return missing;
}
