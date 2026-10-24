// Builds xatlas (MIT, https://github.com/jpcy/xatlas), the unwrapper Godot uses for lightmap UVs,
// and stb_rect_pack, which packs Godot's lightmap atlases, so that their charts and atlases are
// the same on every platform: its sines, cosines and arc functions go
// through the engine's own (slash::cos_sin and slash::atan2) rather than libm's, whose last bits
// differ between platforms, and its one qsort, whose comparator never returns a negative number
// and so sorts differently with each C library, through a stable merge sort that reads only
// "greater than". It runs on one thread, so threads can't change the order charts are made in.
// stb_rect_pack sorts rectangles by height and width with qsort too, which leaves equal ones in
// whatever order the C library does; the same stable sort keeps them in the order given.
//
// Every header xatlas.cpp includes comes first, so the names below are swapped only in its code.
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cfloat>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

// NOLINTBEGIN(modernize-deprecated-headers): xatlas.cpp includes the C names of these.
#include <assert.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
// NOLINTEND(modernize-deprecated-headers)

#include "slash/core/math.hpp"

namespace {

float xatlas_sin(float radians) {
    return slash::cos_sin(radians).y;
}

float xatlas_cos(float radians) {
    return slash::cos_sin(radians).x;
}

float xatlas_atan2(float y, float x) {
    return slash::atan2(y, x);
}

float xatlas_acos(float x) {
    return slash::atan2(std::sqrt((1.0F - x) * (1.0F + x)), x);
}

// qsort's arguments, sorted stably: an element moves before an earlier one only if compare says
// the earlier one is greater.
void xatlas_sort(void* base, std::size_t count, std::size_t size,
                 int (*compare)(const void*, const void*)) {
    auto* bytes = static_cast<unsigned char*>(base);
    std::vector<unsigned char> scratch(count * size);
    for (std::size_t width = 1; width < count; width *= 2) {
        for (std::size_t start = 0; start < count; start += 2 * width) {
            const std::size_t middle = std::min(start + width, count);
            const std::size_t end = std::min(start + (2 * width), count);
            std::size_t left = start;
            std::size_t right = middle;
            std::size_t out = start;
            while (left < middle || right < end) {
                const bool take_right =
                    left == middle ||
                    (right < end && compare(bytes + (left * size), bytes + (right * size)) > 0);
                const std::size_t from = take_right ? right++ : left++;
                std::memcpy(scratch.data() + (out++ * size), bytes + (from * size), size);
            }
        }
        std::memcpy(bytes, scratch.data(), count * size);
    }
}

} // namespace

// NOLINTBEGIN
#define XA_MULTITHREADED 0
#define XA_DEBUG 0
#define XA_DEBUG_ASSERT(exp) ((void)0)
#define sinf xatlas_sin
#define cosf xatlas_cos
#define atan2f xatlas_atan2
#define acosf xatlas_acos
#define qsort xatlas_sort
#include <xatlas.cpp>
// NOLINTEND

// NOLINTBEGIN
#define STB_RECT_PACK_IMPLEMENTATION
#define STBRP_SORT xatlas_sort
#define STBRP_ASSERT(x) ((void)0)
#include <stb_rect_pack.h>
// NOLINTEND
