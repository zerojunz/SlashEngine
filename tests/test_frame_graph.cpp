// The frame graph (engine/render/src/frame_graph.hpp): culling passes nothing kept reads, transient
// resources sharing allocations where their uses never overlap, and compute passes merged into
// one where none touches what another writes.

#include "../engine/render/src/frame_graph.hpp"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using slash::render::detail::FrameGraph;
using slash::render::detail::GraphContext;
using slash::render::detail::GraphPassKind;
using slash::render::detail::GraphShape;

namespace {

constexpr GraphShape kHalf{.key = 1, .bytes = 1000};
constexpr GraphShape kFull{.key = 2, .bytes = 4000};

} // namespace

TEST_CASE("the frame graph leaves out passes nothing kept reads, and runs the rest in order") {
    FrameGraph graph;
    const auto target = graph.import("target");
    const auto depth = graph.transient("depth", kFull);
    const auto unused = graph.transient("unused", kHalf);
    const auto glow = graph.transient("glow", kHalf);
    std::vector<std::string> ran;
    const auto record = [&](const char* name) {
        return [&ran, name](const GraphContext&) { ran.emplace_back(name); };
    };
    graph.add_pass(
        {.name = "prepass", .kind = GraphPassKind::Render, .reads = {}, .writes = {depth}},
        record("prepass"));
    graph.add_pass(
        {.name = "orphan", .kind = GraphPassKind::Compute, .reads = {depth}, .writes = {unused}},
        record("orphan"));
    graph.add_pass(
        {.name = "glow", .kind = GraphPassKind::Render, .reads = {depth}, .writes = {glow}},
        record("glow"));
    graph.add_pass(
        {.name = "tonemap", .kind = GraphPassKind::Render, .reads = {glow}, .writes = {target}},
        record("tonemap"));
    graph.add_pass({.name = "stats",
                    .kind = GraphPassKind::Cpu,
                    .reads = {},
                    .writes = {},
                    .side_effects = true},
                   record("stats"));
    const auto compiled = graph.compile();
    REQUIRE(compiled.ok());
    CHECK(compiled.value().culled == std::vector<std::uint32_t>{1});
    graph.execute(compiled.value());
    CHECK(ran == std::vector<std::string>{"prepass", "glow", "tonemap", "stats"});
    // The culled pass's resource takes no memory.
    CHECK(compiled.value().transient_bytes == kFull.bytes + kHalf.bytes);
    const auto report = graph.report(compiled.value());
    CHECK(report.passes.at(1).culled);
    CHECK_FALSE(report.passes.at(2).culled);
}

TEST_CASE(
    "the frame graph shares allocations between transient resources whose uses don't overlap") {
    FrameGraph graph;
    const auto target = graph.import("target");
    const auto a = graph.transient("a", kHalf);
    const auto b = graph.transient("b", kHalf);
    const auto c = graph.transient("c", kHalf);
    const auto d = graph.transient("d", kFull);
    graph.add_pass({.name = "1", .kind = GraphPassKind::Render, .reads = {}, .writes = {a}});
    graph.add_pass({.name = "2", .kind = GraphPassKind::Render, .reads = {a}, .writes = {b}});
    // a is done: c can take its allocation; d has another shape and can't.
    graph.add_pass({.name = "3", .kind = GraphPassKind::Render, .reads = {b}, .writes = {c, d}});
    graph.add_pass(
        {.name = "4", .kind = GraphPassKind::Render, .reads = {c, d}, .writes = {target}});
    const auto compiled = graph.compile();
    REQUIRE(compiled.ok());
    const auto& slots = compiled.value().slots;
    CHECK(slots[a] == slots[c]);
    CHECK(slots[a] != slots[b]);
    CHECK(slots[d] != slots[a]);
    CHECK(compiled.value().allocations.size() == 3);
    CHECK(compiled.value().transient_bytes == (3 * kHalf.bytes) + kFull.bytes);
    CHECK(compiled.value().allocated_bytes == (2 * kHalf.bytes) + kFull.bytes);
}

TEST_CASE("the frame graph merges compute passes that touch nothing another in the group writes") {
    FrameGraph graph;
    const auto target = graph.import("target");
    const auto cells = graph.transient("cells", kFull);
    const auto light = graph.transient("light", kFull);
    const auto probes = graph.transient("probes", kHalf);
    const auto spare = graph.transient("spare", kHalf);
    graph.add_pass(
        {.name = "cells", .kind = GraphPassKind::Compute, .reads = {}, .writes = {cells}});
    // Independent of the first: same group.
    graph.add_pass(
        {.name = "probes", .kind = GraphPassKind::Compute, .reads = {}, .writes = {probes}});
    // Reads what the group wrote: a group of its own.
    graph.add_pass(
        {.name = "light", .kind = GraphPassKind::Compute, .reads = {cells}, .writes = {light}});
    // Reads only what an earlier group wrote: joins "light"'s.
    graph.add_pass(
        {.name = "spare", .kind = GraphPassKind::Compute, .reads = {probes}, .writes = {spare}});
    // A render pass ends the group.
    graph.add_pass({.name = "draw",
                    .kind = GraphPassKind::Render,
                    .reads = {light, spare},
                    .writes = {target}});
    const auto compiled = graph.compile();
    REQUIRE(compiled.ok());
    const auto& passes = compiled.value().passes;
    REQUIRE(passes.size() == 5);
    CHECK(passes[0].group == passes[1].group);
    CHECK(passes[2].group == passes[1].group + 1);
    CHECK(passes[3].group == passes[2].group);
    CHECK(passes[4].group == passes[3].group + 1);
    CHECK(compiled.value().groups == 3);
    // Within a group nothing shares memory: probes is still in use when light is written.
    CHECK(compiled.value().slots[probes] != compiled.value().slots[spare]);
    std::vector<std::uint32_t> opened;
    std::vector<std::uint32_t> closed;
    graph.execute(
        compiled.value(), [&](std::uint32_t group) { opened.push_back(group); },
        [&](std::uint32_t group) { closed.push_back(group); });
    CHECK(opened == std::vector<std::uint32_t>{0, 1});
    CHECK(closed == std::vector<std::uint32_t>{0, 1});
}

TEST_CASE("the frame graph refuses a pass reading a transient resource nothing wrote") {
    FrameGraph graph;
    const auto target = graph.import("target");
    const auto history = graph.transient("history", kHalf);
    graph.add_pass(
        {.name = "resolve", .kind = GraphPassKind::Render, .reads = {history}, .writes = {target}});
    const auto compiled = graph.compile();
    REQUIRE_FALSE(compiled.ok());
    CHECK(compiled.error().message.find("'history'") != std::string::npos);
    FrameGraph other;
    other.add_pass({.name = "bad", .kind = GraphPassKind::Render, .reads = {}, .writes = {7}});
    CHECK_FALSE(other.compile().ok());
}

TEST_CASE("a transient pool keeps allocations from frame to frame and drops those left unused") {
    slash::render::detail::TransientPool<int> pool;
    int made = 0;
    std::vector<int> dropped;
    const auto make = [&](const GraphShape&) -> slash::Result<int> { return ++made; };
    const auto drop = [&](int handle) { dropped.push_back(handle); };
    slash::render::detail::CompiledGraph two;
    two.allocations = {kHalf, kHalf};
    slash::render::detail::CompiledGraph one;
    one.allocations = {kFull};
    const auto first = pool.acquire(two, make, drop);
    REQUIRE(first.ok());
    CHECK(first.value() == std::vector<int>{1, 2});
    // The same shapes the next frame: the same allocations.
    const auto second = pool.acquire(two, make, drop);
    REQUIRE(second.ok());
    CHECK(second.value() == std::vector<int>{1, 2});
    CHECK(made == 2);
    CHECK(pool.bytes() == 2 * kHalf.bytes);
    // Frames without them: kept for a while, then dropped.
    for (int frame = 0; frame < 3; ++frame) {
        REQUIRE(pool.acquire(one, make, drop).ok());
    }
    CHECK(dropped == std::vector<int>{1, 2});
    CHECK(pool.bytes() == kFull.bytes);
    pool.clear(drop);
    CHECK(dropped == std::vector<int>{1, 2, 3});
}
