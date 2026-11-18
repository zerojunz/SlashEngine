#include "frame_graph.hpp"

#include <algorithm>
#include <limits>
#include <numeric>
#include <utility>

namespace slash::render::detail {

namespace {

constexpr std::uint32_t kNone = std::numeric_limits<std::uint32_t>::max();

bool contains(const std::vector<GraphResource>& list, GraphResource resource) {
    return std::ranges::find(list, resource) != list.end();
}

} // namespace

GraphResource FrameGraph::import(std::string_view name) {
    resources_.push_back({.name = std::string(name), .transient = false, .shape = {}});
    return static_cast<GraphResource>(resources_.size() - 1);
}

GraphResource FrameGraph::transient(std::string_view name, GraphShape shape) {
    resources_.push_back({.name = std::string(name), .transient = true, .shape = shape});
    return static_cast<GraphResource>(resources_.size() - 1);
}

std::uint32_t FrameGraph::add_pass(GraphPassDesc desc, GraphCallback run) {
    passes_.push_back({.desc = std::move(desc), .run = std::move(run)});
    return static_cast<std::uint32_t>(passes_.size() - 1);
}

void FrameGraph::clear() {
    resources_.clear();
    passes_.clear();
}

Result<CompiledGraph> FrameGraph::compile() const {
    const std::size_t resource_count = resources_.size();
    for (const Pass& pass : passes_) {
        for (const auto* list : {&pass.desc.reads, &pass.desc.writes}) {
            for (const GraphResource resource : *list) {
                if (resource >= resource_count) {
                    return Error{ErrorCode::InvalidArgument,
                                 "frame graph pass '" + pass.desc.name +
                                     "' names a resource the graph doesn't have"};
                }
            }
        }
    }
    // A transient resource read before any pass wrote it would read whatever the allocation held.
    {
        std::vector<bool> written(resource_count, false);
        for (const Pass& pass : passes_) {
            for (const GraphResource resource : pass.desc.reads) {
                if (resources_[resource].transient && !written[resource]) {
                    return Error{ErrorCode::InvalidArgument,
                                 "frame graph pass '" + pass.desc.name + "' reads '" +
                                     resources_[resource].name + "' before any pass writes it"};
                }
            }
            for (const GraphResource resource : pass.desc.writes) {
                written[resource] = true;
            }
        }
    }

    // Culling, from the last pass back: a pass is kept when it has side effects, writes an
    // imported resource, or writes what a kept pass after it reads.
    std::vector<bool> kept(passes_.size(), false);
    std::vector<bool> needed(resource_count, false);
    for (std::size_t i = passes_.size(); i-- > 0;) {
        const GraphPassDesc& desc = passes_[i].desc;
        const bool keep =
            desc.side_effects || std::ranges::any_of(desc.writes, [&](GraphResource resource) {
                return !resources_[resource].transient || needed[resource];
            });
        if (!keep) {
            continue;
        }
        kept[i] = true;
        for (const GraphResource resource : desc.reads) {
            needed[resource] = true;
        }
    }

    CompiledGraph compiled;
    // Groups: compute passes next to each other share one while none touches what an earlier one
    // in the group writes, or writes what an earlier one reads.
    std::vector<GraphResource> group_reads;
    std::vector<GraphResource> group_writes;
    bool open_compute = false;
    for (std::size_t i = 0; i < passes_.size(); ++i) {
        if (!kept[i]) {
            compiled.culled.push_back(static_cast<std::uint32_t>(i));
            continue;
        }
        const GraphPassDesc& desc = passes_[i].desc;
        bool joins = open_compute && desc.kind == GraphPassKind::Compute;
        if (joins) {
            const auto clashes = [&](GraphResource resource) {
                return contains(group_writes, resource);
            };
            joins = std::ranges::none_of(desc.reads, clashes) &&
                    std::ranges::none_of(desc.writes, clashes) &&
                    std::ranges::none_of(desc.writes, [&](GraphResource resource) {
                        return contains(group_reads, resource);
                    });
        }
        if (!joins) {
            if (!compiled.passes.empty()) {
                ++compiled.groups;
            }
            group_reads.clear();
            group_writes.clear();
        }
        group_reads.insert(group_reads.end(), desc.reads.begin(), desc.reads.end());
        group_writes.insert(group_writes.end(), desc.writes.begin(), desc.writes.end());
        open_compute = desc.kind == GraphPassKind::Compute;
        compiled.passes.push_back(
            {.pass = static_cast<std::uint32_t>(i), .group = compiled.groups});
    }
    if (!compiled.passes.empty()) {
        ++compiled.groups;
    }

    // Each transient resource's first and last group among the kept passes. Lifetimes count in
    // groups, since passes merged into one device pass run with no barrier between them.
    std::vector<std::uint32_t> first(resource_count, kNone);
    std::vector<std::uint32_t> last(resource_count, 0);
    for (const CompiledPass& at : compiled.passes) {
        const GraphPassDesc& desc = passes_[at.pass].desc;
        for (const auto* list : {&desc.reads, &desc.writes}) {
            for (const GraphResource resource : *list) {
                first[resource] = std::min(first[resource], at.group);
                last[resource] = std::max(last[resource], at.group);
            }
        }
    }
    std::vector<GraphResource> order(resource_count);
    std::iota(order.begin(), order.end(), GraphResource{0});
    std::ranges::stable_sort(order,
                             [&](GraphResource a, GraphResource b) { return first[a] < first[b]; });
    compiled.slots.assign(resource_count, kNone);
    std::vector<std::uint32_t> free_after; // each allocation's last group in use
    for (const GraphResource resource : order) {
        const Resource& made = resources_[resource];
        if (!made.transient) {
            compiled.slots[resource] = resource;
            continue;
        }
        if (first[resource] == kNone) {
            continue; // only culled passes touch it
        }
        compiled.transient_bytes += made.shape.bytes;
        std::uint32_t slot = kNone;
        for (std::uint32_t s = 0; s < compiled.allocations.size(); ++s) {
            if (compiled.allocations[s] == made.shape && free_after[s] < first[resource]) {
                slot = s;
                break;
            }
        }
        if (slot == kNone) {
            slot = static_cast<std::uint32_t>(compiled.allocations.size());
            compiled.allocations.push_back(made.shape);
            free_after.push_back(0);
            compiled.allocated_bytes += made.shape.bytes;
        }
        free_after[slot] = last[resource];
        compiled.slots[resource] = slot;
    }
    return compiled;
}

void FrameGraph::execute(const CompiledGraph& compiled,
                         const std::function<void(std::uint32_t group)>& begin_group,
                         const std::function<void(std::uint32_t group)>& end_group) const {
    const GraphContext context(*this, compiled.slots);
    for (std::size_t i = 0; i < compiled.passes.size(); ++i) {
        const CompiledPass& at = compiled.passes[i];
        const bool compute = passes_[at.pass].desc.kind == GraphPassKind::Compute;
        const bool opens = i == 0 || compiled.passes[i - 1].group != at.group;
        const bool closes =
            i + 1 == compiled.passes.size() || compiled.passes[i + 1].group != at.group;
        if (compute && opens && begin_group) {
            begin_group(at.group);
        }
        if (passes_[at.pass].run) {
            passes_[at.pass].run(context);
        }
        if (compute && closes && end_group) {
            end_group(at.group);
        }
    }
}

FrameGraphReport FrameGraph::report(const CompiledGraph& compiled) const {
    FrameGraphReport report;
    report.passes.resize(passes_.size());
    for (std::size_t i = 0; i < passes_.size(); ++i) {
        report.passes[i] = {
            .name = passes_[i].desc.name, .kind = passes_[i].desc.kind, .culled = true, .group = 0};
    }
    for (const CompiledPass& at : compiled.passes) {
        report.passes[at.pass].culled = false;
        report.passes[at.pass].group = at.group;
    }
    report.resources = static_cast<std::uint32_t>(std::ranges::count_if(
        resources_, [](const Resource& resource) { return resource.transient; }));
    report.allocations = static_cast<std::uint32_t>(compiled.allocations.size());
    report.groups = compiled.groups;
    report.transient_bytes = compiled.transient_bytes;
    report.allocated_bytes = compiled.allocated_bytes;
    return report;
}

void FrameGraphReport::add(const FrameGraphReport& other) {
    for (Pass pass : other.passes) {
        if (!pass.culled) {
            pass.group += groups;
        }
        passes.push_back(std::move(pass));
    }
    resources += other.resources;
    allocations += other.allocations;
    groups += other.groups;
    transient_bytes += other.transient_bytes;
    allocated_bytes += other.allocated_bytes;
}

std::string_view graph_pass_kind_name(GraphPassKind kind) {
    switch (kind) {
    case GraphPassKind::Render:
        return "render";
    case GraphPassKind::Compute:
        return "compute";
    case GraphPassKind::Copy:
        return "copy";
    case GraphPassKind::Cpu:
        return "cpu";
    }
    return "render";
}

} // namespace slash::render::detail
