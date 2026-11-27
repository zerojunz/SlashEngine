#include "slash/render/latency.hpp"

#include <algorithm>
#include <mutex>
#include <utility>

namespace slash::render {
namespace {

struct Registry {
    std::mutex mutex;
    std::vector<LatencyReducerInfo> added;
};

Registry& registry() {
    static Registry instance;
    return instance;
}

bool valid_name(std::string_view name) {
    return !name.empty() && std::ranges::all_of(name, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    });
}

} // namespace

std::string_view latency_marker_name(LatencyMarker marker) {
    switch (marker) {
    case LatencyMarker::SimulationStart:
        return "simulation_start";
    case LatencyMarker::SimulationEnd:
        return "simulation_end";
    case LatencyMarker::RenderSubmitStart:
        return "render_submit_start";
    case LatencyMarker::RenderSubmitEnd:
        return "render_submit_end";
    case LatencyMarker::PresentStart:
        return "present_start";
    case LatencyMarker::PresentEnd:
        return "present_end";
    case LatencyMarker::InputSample:
        return "input_sample";
    }
    return "input_sample";
}

Result<void> add_latency_reducer(LatencyReducerInfo info) {
    if (!valid_name(info.name)) {
        return make_error(ErrorCode::InvalidArgument,
                          "a latency reducer's name is lowercase letters, digits and "
                          "underscores, not \"" +
                              info.name + "\"");
    }
    if (!info.make) {
        return make_error(ErrorCode::InvalidArgument,
                          "latency reducer " + info.name + " makes nothing: it needs make");
    }
    if (find_latency_reducer(info.name)) {
        return make_error(ErrorCode::Conflict, "latency reducer " + info.name + " already exists");
    }
    Registry& added = registry();
    const std::scoped_lock lock(added.mutex);
    added.added.push_back(std::move(info));
    return {};
}

std::optional<LatencyReducerInfo> find_latency_reducer(std::string_view name) {
    Registry& added = registry();
    const std::scoped_lock lock(added.mutex);
    for (const LatencyReducerInfo& info : added.added) {
        if (info.name == name) {
            return info;
        }
    }
    return std::nullopt;
}

std::vector<std::string> latency_reducer_names() {
    std::vector<std::string> names;
    Registry& added = registry();
    const std::scoped_lock lock(added.mutex);
    names.reserve(added.added.size());
    for (const LatencyReducerInfo& info : added.added) {
        names.push_back(info.name);
    }
    return names;
}

void clear_added_latency_reducers() {
    Registry& added = registry();
    const std::scoped_lock lock(added.mutex);
    added.added.clear();
}

Result<std::unique_ptr<LatencyReducer>> make_latency_reducer(std::string_view name,
                                                             const RenderDevice& device) {
    if (name.empty()) {
        return make_error(ErrorCode::InvalidArgument, "no latency reducer is named");
    }
    const std::optional<LatencyReducerInfo> info = find_latency_reducer(name);
    if (!info) {
        return make_error(ErrorCode::NotFound, "no latency reducer is named " + std::string(name) +
                                                   ": an SDK package's register function adds it");
    }
    if (info->supports && !info->supports(device)) {
        return make_error(ErrorCode::PlatformFailure,
                          "latency reducer " + info->name + " doesn't run on the " +
                              std::string(backend_name(device.backend())) + " device here");
    }
    std::unique_ptr<LatencyReducer> made = info->make(device.native_device());
    if (made == nullptr) {
        return make_error(ErrorCode::InvalidArgument,
                          "latency reducer " + info->name + " made nothing");
    }
    return made;
}

} // namespace slash::render
