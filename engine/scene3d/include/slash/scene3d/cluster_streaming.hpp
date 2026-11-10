#pragma once

// Streaming of virtualized geometry's clusters, a research feature
// (docs/reference/virtualized-geometry.md#streaming), after Nanite's page streaming as Brian Karis
// describes it in "Nanite: A Deep Dive" (SIGGRAPH 2021). A cluster hierarchy is written as a page
// file: an index of every group's and cluster's bounds, errors and links, which stays in memory,
// then a page per group holding its clusters' triangles, which are loaded when a view needs them
// and dropped when the pool runs out of room. Views cut from what is loaded, coarser where finer
// pages are missing, so cuts still cover the surface once with no cracks.

#include "slash/core/json.hpp"
#include "slash/core/result.hpp"
#include "slash/scene3d/virtual_geometry.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace slash::scene3d {

// Where a group's page lies in a page file.
struct ClusterPage {
    std::uint64_t offset = 0;
    std::uint32_t bytes = 0;
};

// A page file's index: the hierarchy with every cluster's indices left out (each cluster's
// index_count says how many it has), and where each group's page is.
struct ClusterPageIndex {
    ClusterHierarchy hierarchy;
    std::vector<std::uint32_t> index_counts; // by cluster
    std::vector<ClusterPage> pages;          // by group
};

// Writes a hierarchy as a page file: a 16-byte header ("SLCP", its version, the index's size in
// bytes and the group count), the index, then each group's page of its clusters' indices, all
// little-endian. The same hierarchy writes the same bytes everywhere.
[[nodiscard]] std::vector<std::uint8_t> write_cluster_pages(const ClusterHierarchy& hierarchy);

// Reads bytes of a page file: those at offset, as many as asked, or an error.
using ClusterPageReader =
    std::function<Result<std::vector<std::uint8_t>>(std::uint64_t offset, std::size_t bytes)>;

// A reader over a page file held in memory.
[[nodiscard]] ClusterPageReader
cluster_pages_in_memory(std::shared_ptr<const std::vector<std::uint8_t>> file);

// Reads a page file's header and index, checking every link and page.
[[nodiscard]] Result<ClusterPageIndex> read_cluster_page_index(const ClusterPageReader& read);

// How streaming runs, from project.json's rendering.virtual_geometry: the pool's size in bytes
// (0 streams nothing: every page stays loaded) and how many pages load at most in an update.
struct ClusterStreamingSettings {
    std::size_t pool_bytes = 0;
    std::size_t max_page_loads = 32;
    bool operator==(const ClusterStreamingSettings&) const = default;
};

// Reads project.json's rendering object's virtual_geometry: streaming_pool_kb and
// max_page_loads_per_frame.
[[nodiscard]] ClusterStreamingSettings cluster_streaming_settings(const json::Value& rendering);

struct ClusterStreamingStats {
    std::size_t resident_pages = 0;
    std::size_t resident_bytes = 0;
    std::size_t pending_pages = 0; // asked for by the last cuts and not loaded
    std::size_t loads = 0;         // pages loaded since it opened
    std::size_t evictions = 0;     // pages dropped since it opened
};

// A hierarchy whose groups' pages load as views need them. The coarsest groups (those nothing
// was simplified from) load when it opens and stay. A group loads only after the groups its
// clusters were simplified into, and is dropped only when no finer group that needs it is loaded,
// so what is loaded always reaches the coarsest groups.
class ClusterStreamer {
public:
    // Opens a page file through read: its index, then the coarsest groups' pages.
    [[nodiscard]] static Result<ClusterStreamer> open(ClusterPageReader read,
                                                      const ClusterStreamingSettings& settings);

    // The clusters a view draws from what is loaded: select_clusters' cut with every group not
    // loaded counted as fine enough, so the coarser clusters made from it are drawn in its place.
    // The groups the view's full cut needs and lacks are asked for, the most visible first.
    [[nodiscard]] std::vector<std::uint32_t> select(const ClusterView& view);

    // Loads the pages asked for since the last update, at most max_page_loads of them, each
    // group's coarser groups first, dropping the pages used least recently by any cut while the
    // pool lacks room. A page that can't be read is an error, and what loaded before stays.
    Result<void> update();

    void set_settings(const ClusterStreamingSettings& settings) { settings_ = settings; }

    // The hierarchy, with the indices of the clusters of loaded groups.
    [[nodiscard]] const ClusterHierarchy& hierarchy() const noexcept { return index_.hierarchy; }
    [[nodiscard]] bool resident(std::size_t group) const { return resident_.at(group) != 0; }
    [[nodiscard]] ClusterStreamingStats stats() const;

private:
    ClusterStreamer() = default;
    Result<void> load(std::size_t group);
    bool evict_one();

    ClusterPageReader read_;
    ClusterStreamingSettings settings_;
    ClusterPageIndex index_;
    // By group: whether its page is loaded, the update a cut last used it in, the groups its
    // clusters were simplified into (coarser) and those simplified into it (finer).
    std::vector<std::uint8_t> resident_;
    std::vector<std::uint64_t> used_;
    std::vector<std::vector<std::uint32_t>> coarser_;
    std::vector<std::vector<std::uint32_t>> finer_;
    // Groups asked for since the last update, with how visible each is (the most wanted first).
    std::vector<float> wanted_;
    std::uint64_t tick_ = 1;
    std::size_t loads_left_ = 0;
    ClusterStreamingStats stats_;
};

} // namespace slash::scene3d
