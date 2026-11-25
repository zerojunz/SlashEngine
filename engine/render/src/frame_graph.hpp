// A frame's passes as a graph, after Unity's render graph: each pass names the resources it reads
// and writes, compile() leaves out the passes nothing kept reads, gives the textures and buffers
// made for the frame alone (transient ones) their first and last use, lets those whose uses never
// overlap share one allocation (aliasing), and groups compute passes next to each other that touch
// nothing another in the group writes, so a device records each group in one compute pass.
// execute() runs the kept passes in order.
//
// The devices build one a frame from the passes they run, so agents read what a frame did
// (FrameGraphReport) the same way on every device.

#pragma once

#include "slash/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace slash::render::detail {

// What a pass records into.
enum class GraphPassKind : std::uint8_t { Render, Compute, Copy, Cpu };

// A transient resource's shape: two resources with the same shape can share an allocation. key
// is the device's own (format, usage and size folded together); bytes is what it takes.
struct GraphShape {
    std::uint64_t key = 0;
    std::uint64_t bytes = 0;
    bool operator==(const GraphShape&) const = default;
};

// A resource's handle in its graph.
using GraphResource = std::uint32_t;

class FrameGraph;

// What a pass's callback is handed: where each resource it touches lives.
class GraphContext {
public:
    GraphContext(const FrameGraph& graph, std::span<const std::uint32_t> slots)
        : graph_(graph), slots_(slots) {}
    // The allocation a transient resource uses this frame (resources sharing one have the same),
    // or the resource itself for an imported one.
    [[nodiscard]] std::uint32_t slot(GraphResource resource) const { return slots_[resource]; }
    [[nodiscard]] const FrameGraph& graph() const { return graph_; }

private:
    const FrameGraph& graph_;
    std::span<const std::uint32_t> slots_;
};

using GraphCallback = std::function<void(const GraphContext&)>;

struct GraphPassDesc {
    std::string name;
    GraphPassKind kind = GraphPassKind::Render;
    std::vector<GraphResource> reads;
    std::vector<GraphResource> writes;
    // Kept whatever reads what it writes: it shows, reads back or keeps history of its own.
    bool side_effects = false;
};

// One pass of a compiled graph.
struct CompiledPass {
    std::uint32_t pass = 0;  // its index in the graph
    std::uint32_t group = 0; // passes with the same group record into one device pass
};

// A compiled graph: the passes kept in order, the allocation each resource uses and what the
// frame saves.
struct CompiledGraph {
    std::vector<CompiledPass> passes;
    std::vector<std::uint32_t> culled;   // passes left out, in order
    std::vector<std::uint32_t> slots;    // per resource; transient ones index allocations
    std::vector<GraphShape> allocations; // what each transient allocation holds
    std::uint32_t groups = 0;
    std::uint64_t transient_bytes = 0; // every transient resource's bytes, unshared
    std::uint64_t allocated_bytes = 0; // the allocations' bytes
};

// A frame's graph as data, for perf.stats and frame.capture.
struct FrameGraphReport {
    struct Pass {
        std::string name;
        GraphPassKind kind = GraphPassKind::Render;
        bool culled = false;
        std::uint32_t group = 0;
    };
    std::vector<Pass> passes;
    std::uint32_t resources = 0;   // transient resources
    std::uint32_t allocations = 0; // what they share
    std::uint32_t groups = 0;
    std::uint64_t transient_bytes = 0;
    std::uint64_t allocated_bytes = 0;

    // Adds another graph of the same frame: its passes after these, its counts to these. The
    // allocations the two share are the pool's to count (TransientPool::frame_bytes).
    void add(const FrameGraphReport& other);
};

class FrameGraph {
public:
    // A resource made outside the frame (a target, history, the swapchain): never shared, and a
    // pass writing one is kept.
    GraphResource import(std::string_view name);
    // A resource the frame makes and drops.
    GraphResource transient(std::string_view name, GraphShape shape);

    std::uint32_t add_pass(GraphPassDesc desc, GraphCallback run = {});

    [[nodiscard]] std::size_t pass_count() const { return passes_.size(); }
    [[nodiscard]] std::size_t resource_count() const { return resources_.size(); }
    [[nodiscard]] std::string_view resource_name(GraphResource resource) const {
        return resources_.at(resource).name;
    }
    [[nodiscard]] bool is_transient(GraphResource resource) const {
        return resources_.at(resource).transient;
    }
    [[nodiscard]] const GraphPassDesc& pass(std::uint32_t index) const {
        return passes_.at(index).desc;
    }

    // Fails for a pass naming a resource the graph doesn't have, or reading a transient resource
    // no pass before it wrote.
    [[nodiscard]] Result<CompiledGraph> compile() const;
    // Runs the kept passes' callbacks in order. begin_group and end_group, when set, are called
    // around each group of compute passes merged into one.
    void execute(const CompiledGraph& compiled,
                 const std::function<void(std::uint32_t group)>& begin_group = {},
                 const std::function<void(std::uint32_t group)>& end_group = {}) const;
    [[nodiscard]] FrameGraphReport report(const CompiledGraph& compiled) const;

    void clear();

private:
    struct Resource {
        std::string name;
        bool transient = false;
        GraphShape shape;
    };
    struct Pass {
        GraphPassDesc desc;
        GraphCallback run;
    };
    std::vector<Resource> resources_;
    std::vector<Pass> passes_;
};

// The allocations a device keeps for its graphs' transient resources, by shape, from frame to
// frame: acquire hands a compiled graph's allocations a handle each, making those the pool lacks;
// graphs acquired one after another in a frame share them, since each is done with its
// allocations before the next runs. next_frame drops what no graph has used for
// kTransientFrames frames.
template <typename Handle>
class TransientPool {
public:
    static constexpr std::uint64_t kTransientFrames = 3;

    // Handles for compiled's allocations, in order, or the first error make returned.
    template <typename Make, typename Drop>
    Result<std::vector<Handle>> acquire(const CompiledGraph& compiled, const Make& make,
                                        const Drop& /*drop*/) {
        std::vector<bool> taken(entries_.size(), false);
        std::vector<Handle> handles;
        handles.reserve(compiled.allocations.size());
        for (const GraphShape& shape : compiled.allocations) {
            std::size_t found = entries_.size();
            for (std::size_t i = 0; i < entries_.size(); ++i) {
                if (!taken[i] && entries_[i].shape == shape) {
                    found = i;
                    break;
                }
            }
            if (found == entries_.size()) {
                Result<Handle> made = make(shape);
                if (!made.ok()) {
                    return made.error();
                }
                entries_.push_back({.shape = shape, .handle = made.value(), .used = frame_});
                taken.push_back(false);
            }
            taken[found] = true;
            entries_[found].used = frame_;
            handles.push_back(entries_[found].handle);
        }
        return handles;
    }

    // Starts a frame, dropping what the last kTransientFrames frames didn't use.
    template <typename Drop>
    void next_frame(const Drop& drop) {
        ++frame_;
        std::erase_if(entries_, [&](const Entry& entry) {
            if (frame_ - entry.used < kTransientFrames) {
                return false;
            }
            drop(entry.handle);
            return true;
        });
    }

    template <typename Drop>
    void clear(const Drop& drop) {
        for (const Entry& entry : entries_) {
            drop(entry.handle);
        }
        entries_.clear();
    }

    // What the pool holds, in bytes.
    [[nodiscard]] std::uint64_t bytes() const {
        std::uint64_t total = 0;
        for (const Entry& entry : entries_) {
            total += entry.shape.bytes;
        }
        return total;
    }

    // What this frame's graphs used, in bytes, each allocation once.
    [[nodiscard]] std::uint64_t frame_bytes() const {
        std::uint64_t total = 0;
        for (const Entry& entry : entries_) {
            if (entry.used == frame_) {
                total += entry.shape.bytes;
            }
        }
        return total;
    }

private:
    struct Entry {
        GraphShape shape;
        Handle handle{};
        std::uint64_t used = 0;
    };
    std::vector<Entry> entries_;
    std::uint64_t frame_ = 0;
};

[[nodiscard]] std::string_view graph_pass_kind_name(GraphPassKind kind);

} // namespace slash::render::detail
