#pragma once

#include <cstdint>

// How much memory this process uses, as the operating system counts it, for perf.stats' memory
// report.
namespace slash::platform {

struct ProcessMemory {
    // Bytes of the process's memory in RAM now (the resident set, or the working set on
    // Windows), and the most it has been. On the web, the WebAssembly heap's size for both.
    std::uint64_t resident = 0;
    std::uint64_t peak = 0;
};

// Zeros where the platform cannot tell.
[[nodiscard]] ProcessMemory process_memory();

} // namespace slash::platform
