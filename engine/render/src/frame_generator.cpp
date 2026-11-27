#include "slash/render/frame_generator.hpp"

#include <algorithm>
#include <mutex>
#include <utility>

namespace slash::render {
namespace {

struct Registry {
    std::mutex mutex;
    std::vector<FrameGeneratorInfo> added;
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

Result<void> add_frame_generator(FrameGeneratorInfo info) {
    if (!valid_name(info.name)) {
        return make_error(ErrorCode::InvalidArgument,
                          "a frame generator's name is lowercase letters, digits and "
                          "underscores, not \"" +
                              info.name + "\"");
    }
    if (!info.make) {
        return make_error(ErrorCode::InvalidArgument,
                          "frame generator " + info.name + " makes nothing: it needs make");
    }
    if (info.max_frames < 1 || info.max_frames > kMaxGeneratedFrames) {
        return make_error(
            ErrorCode::InvalidArgument,
            "frame generator " + info.name + " makes 1 to " + std::to_string(kMaxGeneratedFrames) +
                " frames between two drawn ones, not " + std::to_string(info.max_frames));
    }
    if (info.name == kFsr3FrameGeneration || find_frame_generator(info.name)) {
        return make_error(ErrorCode::Conflict, "frame generator " + info.name + " already exists");
    }
    Registry& added = registry();
    const std::scoped_lock lock(added.mutex);
    added.added.push_back(std::move(info));
    return {};
}

std::optional<FrameGeneratorInfo> find_frame_generator(std::string_view name) {
    Registry& added = registry();
    const std::scoped_lock lock(added.mutex);
    for (const FrameGeneratorInfo& info : added.added) {
        if (info.name == name) {
            return info;
        }
    }
    return std::nullopt;
}

std::vector<std::string> frame_generator_names() {
    std::vector<std::string> names{std::string(kFsr3FrameGeneration)};
    Registry& added = registry();
    const std::scoped_lock lock(added.mutex);
    for (const FrameGeneratorInfo& info : added.added) {
        names.push_back(info.name);
    }
    return names;
}

void clear_added_frame_generators() {
    Registry& added = registry();
    const std::scoped_lock lock(added.mutex);
    added.added.clear();
}

bool runs_frame_generators(const RenderDevice& device) {
    switch (device.backend()) {
    case Backend::Software:
    case Backend::Vulkan:
    case Backend::D3D12:
    case Backend::Metal:
        return device.generates_frames();
    default:
        return false;
    }
}

FrameGeneratorChoice choose_frame_generator(std::string_view name, std::uint32_t frames,
                                            const RenderDevice& device) {
    if (name.empty() || name == kFsr3FrameGeneration || !runs_frame_generators(device)) {
        return {};
    }
    const std::optional<FrameGeneratorInfo> info = find_frame_generator(name);
    if (!info || (info->supports && !info->supports(device))) {
        return {};
    }
    return {.name = std::string(name), .frames = std::clamp(frames, 1U, info->max_frames)};
}

} // namespace slash::render
