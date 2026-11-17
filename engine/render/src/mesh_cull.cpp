#include "mesh_cull.hpp"

#include "slash/core/bits.hpp"

#include <algorithm>
#include <cmath>

namespace slash::render::detail {

namespace {

float float_of(std::uint32_t word) {
    return slash::bit_cast<float>(word);
}

std::uint32_t word_of(float value) {
    return slash::bit_cast<std::uint32_t>(value);
}

} // namespace

void push_cull_record(std::vector<std::uint32_t>& words, const Mat4& view_projection,
                      const Mat4& transform, const Bounds& bounds, IndexRange range, bool keep) {
    const Mat4 clip = multiply(view_projection, transform);
    for (const float value : clip) {
        words.push_back(word_of(value));
    }
    // A box turned inside out is never culled.
    const Float3 low = keep ? Float3{1.0F, 1.0F, 1.0F} : bounds.low;
    const Float3 high = keep ? Float3{-1.0F, -1.0F, -1.0F} : bounds.high;
    for (const float value : low) {
        words.push_back(word_of(value));
    }
    for (const float value : high) {
        words.push_back(word_of(value));
    }
    words.push_back(range.first);
    words.push_back(range.count);
}

bool box_in_view(const Mat4& clip, const Float3& low, const Float3& high) {
    if (low[0] > high[0] || low[1] > high[1] || low[2] > high[2]) {
        return true;
    }
    // Whether every corner is past each edge: left, right, down, up, near (reversed depth puts
    // it at z = w) and far (z = 0).
    std::array<bool, 6> past{true, true, true, true, true, true};
    for (std::uint32_t corner = 0; corner < 8; ++corner) {
        const float x = (corner & 1U) != 0 ? high[0] : low[0];
        const float y = (corner & 2U) != 0 ? high[1] : low[1];
        const float z = (corner & 4U) != 0 ? high[2] : low[2];
        std::array<float, 4> c{};
        for (std::size_t row = 0; row < 4; ++row) {
            c.at(row) = (clip.at(row) * x) + (clip.at(4 + row) * y) + (clip.at(8 + row) * z) +
                        clip.at(12 + row);
        }
        const float margin = kCullMargin * std::abs(c[3]);
        past[0] = past[0] && c[0] < -c[3] - margin;
        past[1] = past[1] && c[0] > c[3] + margin;
        past[2] = past[2] && c[1] < -c[3] - margin;
        past[3] = past[3] && c[1] > c[3] + margin;
        past[4] = past[4] && c[2] > c[3] + margin;
        past[5] = past[5] && c[2] < -margin;
    }
    return std::ranges::none_of(past, [](bool edge) { return edge; });
}

std::vector<std::uint32_t> cull_draws(std::span<const std::uint32_t> records) {
    const std::size_t count = records.size() / kCullRecordWords;
    std::vector<std::uint32_t> args;
    args.reserve(count * kCullArgsWords);
    for (std::size_t i = 0; i < count; ++i) {
        const std::span<const std::uint32_t> record =
            records.subspan(i * kCullRecordWords, kCullRecordWords);
        Mat4 clip{};
        for (std::size_t k = 0; k < 16; ++k) {
            clip.at(k) = float_of(record[k]);
        }
        const Float3 low{float_of(record[16]), float_of(record[17]), float_of(record[18])};
        const Float3 high{float_of(record[19]), float_of(record[20]), float_of(record[21])};
        args.push_back(record[23]);
        args.push_back(box_in_view(clip, low, high) ? 1U : 0U);
        args.push_back(record[22]);
        args.push_back(0);
        args.push_back(0);
    }
    return args;
}

std::uint32_t cull_groups(std::uint32_t count) {
    return (count + kCullThreads - 1) / kCullThreads;
}

} // namespace slash::render::detail
