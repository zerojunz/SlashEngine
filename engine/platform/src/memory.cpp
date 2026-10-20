#include "slash/platform/memory.hpp"

#if defined(__EMSCRIPTEN__)
#include <emscripten/heap.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// After windows.h, which it needs.
#include <psapi.h>
#else
#include <charconv>
#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>
#include <system_error>
#endif

namespace slash::platform {

ProcessMemory process_memory() {
    ProcessMemory memory;
#if defined(__EMSCRIPTEN__)
    memory.resident = emscripten_get_heap_size();
    memory.peak = memory.resident;
#elif defined(__APPLE__)
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): task_info's own interface
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info),
                  &count) == KERN_SUCCESS) {
        memory.resident = info.resident_size;
        memory.peak = info.resident_size_max;
    }
#elif defined(_WIN32)
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    // K32GetProcessMemoryInfo is in kernel32 since Windows 7, so nothing more needs linking.
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) != 0) {
        memory.resident = counters.WorkingSetSize;
        memory.peak = counters.PeakWorkingSetSize;
    }
#else
    // Linux and Android: VmRSS and VmHWM (the high-water mark), in kB.
    std::FILE* file = std::fopen("/proc/self/status", "r");
    if (file == nullptr) {
        return memory;
    }
    std::string status;
    char chunk[1024];
    for (std::size_t got = 0; (got = std::fread(chunk, 1, sizeof(chunk), file)) > 0;) {
        status.append(chunk, got);
    }
    std::fclose(file);
    const auto kilobytes = [&](std::string_view key) -> std::uint64_t {
        const std::size_t at = status.find(key);
        if (at == std::string::npos) {
            return 0;
        }
        const char* first = status.data() + at + key.size();
        const char* last = status.data() + status.size();
        while (first != last && (*first == ' ' || *first == '\t')) {
            ++first;
        }
        std::uint64_t value = 0;
        if (std::from_chars(first, last, value).ec != std::errc()) {
            return 0;
        }
        return value;
    };
    memory.resident = kilobytes("VmRSS:") * 1024;
    memory.peak = kilobytes("VmHWM:") * 1024;
#endif
    return memory;
}

} // namespace slash::platform
