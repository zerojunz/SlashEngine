#include "slash/render/denoiser.hpp"

#include <algorithm>
#include <mutex>
#include <utility>

namespace slash::render {
namespace {

struct Registry {
    std::mutex mutex;
    std::vector<DenoiserInfo> added;
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

Result<void> add_denoiser(DenoiserInfo info) {
    if (!valid_name(info.name)) {
        return make_error(ErrorCode::InvalidArgument,
                          "a denoiser's name is lowercase letters, digits and underscores, not \"" +
                              info.name + "\"");
    }
    if (!info.make) {
        return make_error(ErrorCode::InvalidArgument,
                          "denoiser " + info.name + " makes nothing: it needs make");
    }
    if (find_denoiser(info.name)) {
        return make_error(ErrorCode::Conflict, "denoiser " + info.name + " already exists");
    }
    Registry& added = registry();
    const std::scoped_lock lock(added.mutex);
    added.added.push_back(std::move(info));
    return {};
}

std::optional<DenoiserInfo> find_denoiser(std::string_view name) {
    Registry& added = registry();
    const std::scoped_lock lock(added.mutex);
    for (const DenoiserInfo& info : added.added) {
        if (info.name == name) {
            return info;
        }
    }
    return std::nullopt;
}

std::vector<std::string> denoiser_names() {
    std::vector<std::string> names;
    Registry& added = registry();
    const std::scoped_lock lock(added.mutex);
    names.reserve(added.added.size());
    for (const DenoiserInfo& info : added.added) {
        names.push_back(info.name);
    }
    return names;
}

void clear_added_denoisers() {
    Registry& added = registry();
    const std::scoped_lock lock(added.mutex);
    added.added.clear();
}

std::optional<DenoiserInfo> choose_denoiser(std::string_view name, const RenderDevice& device) {
    if (name.empty() || !device.ray_tracing()) {
        return std::nullopt;
    }
    std::optional<DenoiserInfo> info = find_denoiser(name);
    if (!info || (info->supports && !info->supports(device))) {
        return std::nullopt;
    }
    return info;
}

} // namespace slash::render
