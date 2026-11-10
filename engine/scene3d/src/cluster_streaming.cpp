#include "slash/scene3d/cluster_streaming.hpp"

#include "slash/core/bits.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <numeric>
#include <string>
#include <utility>

namespace slash::scene3d {

namespace {

constexpr std::array<std::uint8_t, 4> kMagic{'S', 'L', 'C', 'P'};
constexpr std::uint32_t kVersion = 1;
constexpr std::size_t kHeaderBytes = 16;
constexpr float kNotWanted = -std::numeric_limits<float>::infinity();

class Writer {
public:
    void u32(std::uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8) {
            bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
        }
    }
    void u64(std::uint64_t value) {
        u32(static_cast<std::uint32_t>(value & 0xFFFFFFFFU));
        u32(static_cast<std::uint32_t>(value >> 32U));
    }
    void i32(int value) { u32(static_cast<std::uint32_t>(value)); }
    void f32(float value) { u32(bit_cast<std::uint32_t>(value)); }
    void vec(const Vec3f& value) {
        f32(value.x);
        f32(value.y);
        f32(value.z);
    }
    void bounds(const ClusterBounds& value) {
        vec(value.center);
        f32(value.radius);
        f32(value.error);
    }

    std::vector<std::uint8_t> bytes;
};

class Reader {
public:
    explicit Reader(const std::vector<std::uint8_t>& from) : bytes_(from) {}

    std::uint32_t u32() {
        if (at_ + 4 > bytes_.size()) {
            short_ = true;
            return 0;
        }
        std::uint32_t value = 0;
        for (std::size_t k = 0; k < 4; ++k) {
            value |= static_cast<std::uint32_t>(bytes_[at_ + k]) << (8U * k);
        }
        at_ += 4;
        return value;
    }
    std::uint64_t u64() {
        const std::uint64_t low = u32();
        return low | (static_cast<std::uint64_t>(u32()) << 32U);
    }
    int i32() { return static_cast<int>(u32()); }
    float f32() { return bit_cast<float>(u32()); }
    Vec3f vec() {
        const float x = f32();
        const float y = f32();
        const float z = f32();
        return {x, y, z};
    }
    ClusterBounds bounds() {
        const Vec3f center = vec();
        const float radius = f32();
        const float error = f32();
        return {.center = center, .radius = radius, .error = error};
    }
    [[nodiscard]] bool short_read() const noexcept { return short_; }
    [[nodiscard]] bool done() const noexcept { return at_ == bytes_.size(); }

private:
    const std::vector<std::uint8_t>& bytes_;
    std::size_t at_ = 0;
    bool short_ = false;
};

Error bad_file(const std::string& why) {
    return Error{.code = ErrorCode::InvalidArgument, .message = "cluster page file: " + why};
}

} // namespace

std::vector<std::uint8_t> write_cluster_pages(const ClusterHierarchy& hierarchy) {
    Writer index;
    index.u32(static_cast<std::uint32_t>(hierarchy.clusters.size()));
    index.u64(hierarchy.source_triangles);
    index.i32(hierarchy.levels);
    for (const Cluster& cluster : hierarchy.clusters) {
        index.bounds(cluster.bounds);
        index.vec(cluster.cone.apex);
        index.vec(cluster.cone.axis);
        index.f32(cluster.cone.cutoff);
        index.i32(cluster.refined);
        index.i32(cluster.group);
        index.i32(cluster.depth);
        index.u32(static_cast<std::uint32_t>(cluster.indices.size()));
    }
    // Each group's page follows the index in group order, so its offset is known up front.
    std::size_t index_bytes = index.bytes.size();
    for (const ClusterGroup& group : hierarchy.groups) {
        index_bytes += (5 * 4) + 4 + 4 + (group.clusters.size() * 4) + 8 + 4;
    }
    std::uint64_t offset = kHeaderBytes + index_bytes;
    Writer pages;
    for (const ClusterGroup& group : hierarchy.groups) {
        index.bounds(group.simplified);
        index.i32(group.depth);
        index.u32(static_cast<std::uint32_t>(group.clusters.size()));
        std::uint32_t bytes = 0;
        for (const std::uint32_t c : group.clusters) {
            index.u32(c);
            for (const std::uint32_t i : hierarchy.clusters.at(c).indices) {
                pages.u32(i);
                bytes += 4;
            }
        }
        index.u64(offset);
        index.u32(bytes);
        offset += bytes;
    }
    Writer out;
    out.bytes.assign(kMagic.begin(), kMagic.end());
    out.u32(kVersion);
    out.u32(static_cast<std::uint32_t>(index.bytes.size()));
    out.u32(static_cast<std::uint32_t>(hierarchy.groups.size()));
    out.bytes.insert(out.bytes.end(), index.bytes.begin(), index.bytes.end());
    out.bytes.insert(out.bytes.end(), pages.bytes.begin(), pages.bytes.end());
    return std::move(out.bytes);
}

ClusterPageReader cluster_pages_in_memory(std::shared_ptr<const std::vector<std::uint8_t>> file) {
    return [file = std::move(file)](std::uint64_t offset,
                                    std::size_t bytes) -> Result<std::vector<std::uint8_t>> {
        if (offset > file->size() || bytes > file->size() - offset) {
            return bad_file("a read past its end");
        }
        const auto from = file->begin() + static_cast<std::ptrdiff_t>(offset);
        return std::vector<std::uint8_t>(from, from + static_cast<std::ptrdiff_t>(bytes));
    };
}

Result<ClusterPageIndex> read_cluster_page_index(const ClusterPageReader& read) {
    auto header = read(0, kHeaderBytes);
    if (!header) {
        return header.error();
    }
    if (!std::equal(kMagic.begin(), kMagic.end(), header.value().begin())) {
        return bad_file("it doesn't start with SLCP");
    }
    Reader head(header.value());
    head.u32();
    if (const std::uint32_t version = head.u32(); version != kVersion) {
        return bad_file("version " + std::to_string(version) + " isn't " +
                        std::to_string(kVersion));
    }
    const std::uint32_t index_bytes = head.u32();
    const std::uint32_t group_count = head.u32();
    auto body = read(kHeaderBytes, index_bytes);
    if (!body) {
        return body.error();
    }
    Reader in(body.value());
    ClusterPageIndex out;
    ClusterHierarchy& hierarchy = out.hierarchy;
    const std::uint32_t cluster_count = in.u32();
    // A cluster takes 64 bytes of the index: more than the index holds is a broken file.
    if (static_cast<std::uint64_t>(cluster_count) * 64 > index_bytes ||
        static_cast<std::uint64_t>(group_count) * 40 > index_bytes) {
        return bad_file("more clusters or groups than its index holds");
    }
    hierarchy.source_triangles = in.u64();
    hierarchy.levels = in.i32();
    hierarchy.clusters.resize(cluster_count);
    out.index_counts.resize(cluster_count);
    for (std::size_t c = 0; c < cluster_count; ++c) {
        Cluster& cluster = hierarchy.clusters[c];
        cluster.bounds = in.bounds();
        cluster.cone.apex = in.vec();
        cluster.cone.axis = in.vec();
        cluster.cone.cutoff = in.f32();
        cluster.refined = in.i32();
        cluster.group = in.i32();
        cluster.depth = in.i32();
        out.index_counts[c] = in.u32();
        if (cluster.refined < -1 || std::cmp_greater_equal(cluster.refined, group_count) ||
            cluster.group < 0 || std::cmp_greater_equal(cluster.group, group_count) ||
            out.index_counts[c] % 3 != 0) {
            return bad_file("cluster " + std::to_string(c) + " links a group it lacks");
        }
    }
    hierarchy.groups.resize(group_count);
    out.pages.resize(group_count);
    for (std::size_t g = 0; g < group_count; ++g) {
        ClusterGroup& group = hierarchy.groups[g];
        group.simplified = in.bounds();
        group.depth = in.i32();
        const std::uint32_t count = in.u32();
        if (count > cluster_count) {
            return bad_file("group " + std::to_string(g) + " lists clusters it lacks");
        }
        std::uint64_t bytes = 0;
        for (std::uint32_t k = 0; k < count; ++k) {
            const std::uint32_t c = in.u32();
            if (c >= cluster_count || std::cmp_not_equal(hierarchy.clusters[c].group, g)) {
                return bad_file("group " + std::to_string(g) + " lists clusters it lacks");
            }
            group.clusters.push_back(c);
            bytes += static_cast<std::uint64_t>(out.index_counts[c]) * 4;
        }
        out.pages[g].offset = in.u64();
        out.pages[g].bytes = in.u32();
        if (bytes != out.pages[g].bytes) {
            return bad_file("group " + std::to_string(g) + "'s page isn't its clusters' size");
        }
    }
    if (in.short_read() || !in.done()) {
        return bad_file("its index isn't the size its header says");
    }
    return out;
}

ClusterStreamingSettings cluster_streaming_settings(const json::Value& rendering) {
    ClusterStreamingSettings out;
    const json::Value* found = rendering.is_object() ? rendering.find("virtual_geometry") : nullptr;
    if (found == nullptr || !found->is_object()) {
        return out;
    }
    if (const json::Value* pool = found->find("streaming_pool_kb");
        pool != nullptr && pool->is_integral() && pool->as_double() >= 0.0) {
        out.pool_bytes = static_cast<std::size_t>(pool->as_double()) * 1024;
    }
    if (const json::Value* loads = found->find("max_page_loads_per_frame");
        loads != nullptr && loads->is_integral() && loads->as_double() >= 1.0) {
        out.max_page_loads = static_cast<std::size_t>(loads->as_double());
    }
    return out;
}

Result<ClusterStreamer> ClusterStreamer::open(ClusterPageReader read,
                                              const ClusterStreamingSettings& settings) {
    auto index = read_cluster_page_index(read);
    if (!index) {
        return index.error();
    }
    ClusterStreamer out;
    out.read_ = std::move(read);
    out.settings_ = settings;
    out.index_ = std::move(index).value();
    const std::size_t groups = out.index_.hierarchy.groups.size();
    out.resident_.assign(groups, 0);
    out.used_.assign(groups, 0);
    out.coarser_.resize(groups);
    out.finer_.resize(groups);
    out.wanted_.assign(groups, kNotWanted);
    // A cluster simplified from group r and placed in group g makes g coarser than r.
    for (const Cluster& cluster : out.index_.hierarchy.clusters) {
        if (cluster.refined < 0) {
            continue;
        }
        const auto r = static_cast<std::uint32_t>(cluster.refined);
        const auto g = static_cast<std::uint32_t>(cluster.group);
        std::vector<std::uint32_t>& up = out.coarser_[r];
        if (std::ranges::find(up, g) == up.end()) {
            up.push_back(g);
            out.finer_[g].push_back(r);
        }
    }
    // The coarsest groups stay loaded, whatever the pool's size.
    out.loads_left_ = std::numeric_limits<std::size_t>::max();
    for (std::size_t g = 0; g < groups; ++g) {
        if (out.coarser_[g].empty()) {
            if (auto loaded = out.load(g); !loaded) {
                return loaded.error();
            }
        }
    }
    out.stats_.loads = 0;
    return out;
}

std::vector<std::uint32_t> ClusterStreamer::select(const ClusterView& view) {
    const ClusterHierarchy& hierarchy = index_.hierarchy;
    std::vector<float> full;
    full.reserve(hierarchy.groups.size());
    for (const ClusterGroup& group : hierarchy.groups) {
        full.push_back(projected_error(group.simplified, view));
    }
    // Errors stay monotone with a group not loaded counted as none: what isn't loaded is closed
    // toward the fine end (a group loads only after the groups coarser than it), so the cut still
    // covers the surface once.
    std::vector<std::uint32_t> chosen;
    for (std::size_t i = 0; i < hierarchy.clusters.size(); ++i) {
        const Cluster& cluster = hierarchy.clusters[i];
        const auto group = static_cast<std::size_t>(cluster.group);
        const bool too_fine_to_drop = resident_[group] != 0 && full[group] > view.threshold;
        const bool coarse_enough =
            cluster.refined < 0 || resident_[static_cast<std::size_t>(cluster.refined)] == 0 ||
            full[static_cast<std::size_t>(cluster.refined)] <= view.threshold;
        if (too_fine_to_drop && coarse_enough) {
            chosen.push_back(static_cast<std::uint32_t>(i));
            used_[group] = tick_;
        }
        // The full cut's clusters whose group isn't loaded are asked for.
        const bool wanted = full[group] > view.threshold &&
                            (cluster.refined < 0 ||
                             full[static_cast<std::size_t>(cluster.refined)] <= view.threshold);
        if (wanted && resident_[group] == 0) {
            wanted_[group] = std::max(wanted_[group], full[group]);
        }
    }
    return chosen;
}

Result<void> ClusterStreamer::load(std::size_t group) {
    if (resident_[group] != 0) {
        return {};
    }
    // The groups its clusters were simplified into come first, so what is loaded stays closed.
    for (const std::uint32_t up : coarser_[group]) {
        if (resident_[up] == 0) {
            if (auto loaded = load(up); !loaded) {
                return loaded;
            }
            if (resident_[up] == 0) {
                return {};
            }
        }
        // Kept while room is made for this one.
        used_[up] = tick_;
    }
    if (loads_left_ == 0) {
        return {};
    }
    const ClusterPage& page = index_.pages[group];
    if (settings_.pool_bytes > 0 && !coarser_[group].empty()) {
        while (stats_.resident_bytes + page.bytes > settings_.pool_bytes) {
            if (!evict_one()) {
                loads_left_ = 0; // no room: nothing more loads until cuts move on
                return {};
            }
        }
    }
    auto bytes = read_(page.offset, page.bytes);
    if (!bytes) {
        return bytes.error();
    }
    if (bytes.value().size() != page.bytes) {
        return bad_file("group " + std::to_string(group) + "'s page was cut short");
    }
    Reader in(bytes.value());
    for (const std::uint32_t c : index_.hierarchy.groups[group].clusters) {
        std::vector<std::uint32_t>& indices = index_.hierarchy.clusters[c].indices;
        indices.resize(index_.index_counts[c]);
        for (std::uint32_t& i : indices) {
            i = in.u32();
        }
    }
    resident_[group] = 1;
    used_[group] = tick_;
    stats_.resident_bytes += page.bytes;
    ++stats_.resident_pages;
    ++stats_.loads;
    --loads_left_;
    return {};
}

bool ClusterStreamer::evict_one() {
    // The page no cut used for longest, the finest first among those, then the last made; only
    // one no loaded finer group needs, and never a coarsest one or one used in this update.
    std::size_t best = index_.pages.size();
    for (std::size_t g = 0; g < index_.pages.size(); ++g) {
        if (resident_[g] == 0 || coarser_[g].empty() || used_[g] >= tick_ ||
            std::ranges::any_of(finer_[g], [&](std::uint32_t f) { return resident_[f] != 0; })) {
            continue;
        }
        if (best == index_.pages.size() || used_[g] < used_[best] ||
            (used_[g] == used_[best] &&
             index_.hierarchy.groups[g].depth <= index_.hierarchy.groups[best].depth)) {
            best = g;
        }
    }
    if (best == index_.pages.size()) {
        return false;
    }
    for (const std::uint32_t c : index_.hierarchy.groups[best].clusters) {
        std::vector<std::uint32_t>().swap(index_.hierarchy.clusters[c].indices);
    }
    resident_[best] = 0;
    stats_.resident_bytes -= index_.pages[best].bytes;
    --stats_.resident_pages;
    ++stats_.evictions;
    return true;
}

Result<void> ClusterStreamer::update() {
    std::vector<std::uint32_t> order;
    for (std::size_t g = 0; g < wanted_.size(); ++g) {
        if (wanted_[g] != kNotWanted) {
            order.push_back(static_cast<std::uint32_t>(g));
        }
    }
    // The most visible first, then in the hierarchy's order.
    std::ranges::stable_sort(
        order, [&](std::uint32_t a, std::uint32_t b) { return wanted_[a] > wanted_[b]; });
    loads_left_ = settings_.max_page_loads;
    Result<void> out;
    for (const std::uint32_t g : order) {
        if (loads_left_ == 0) {
            break;
        }
        if (auto loaded = load(g); !loaded) {
            out = loaded;
            break;
        }
    }
    stats_.pending_pages = static_cast<std::size_t>(
        std::ranges::count_if(order, [&](std::uint32_t g) { return resident_[g] == 0; }));
    std::ranges::fill(wanted_, kNotWanted);
    ++tick_;
    return out;
}

ClusterStreamingStats ClusterStreamer::stats() const {
    return stats_;
}

} // namespace slash::scene3d
