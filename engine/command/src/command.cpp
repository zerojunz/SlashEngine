#include "slash/command/command.hpp"

#include "slash/core/assert.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <utility>

namespace slash::command {

std::string_view to_string(Capability capability) noexcept {
    switch (capability) {
    case Capability::Read:
        return "read";
    case Capability::Edit:
        return "edit";
    case Capability::Run:
        return "run";
    case Capability::Filesystem:
        return "filesystem";
    case Capability::Build:
        return "build";
    }
    return "read";
}

std::optional<Capability> capability_from_string(std::string_view name) noexcept {
    for (const Capability capability : {Capability::Read, Capability::Edit, Capability::Run,
                                        Capability::Filesystem, Capability::Build}) {
        if (to_string(capability) == name) {
            return capability;
        }
    }
    return std::nullopt;
}

Result<std::vector<Capability>> parse_capabilities(std::string_view list) {
    constexpr std::array kAll{Capability::Read, Capability::Edit, Capability::Run,
                              Capability::Filesystem, Capability::Build};
    if (list == "all") {
        return std::vector<Capability>(kAll.begin(), kAll.end());
    }
    std::vector<Capability> capabilities;
    std::size_t start = 0;
    while (start <= list.size()) {
        const std::size_t end = std::min(list.find(',', start), list.size());
        const std::string_view name = list.substr(start, end - start);
        const auto capability = capability_from_string(name);
        if (!capability) {
            return make_error(ErrorCode::InvalidArgument,
                              "unknown capability \"" + std::string(name) +
                                  "\"; use read, edit, run, filesystem, build or all");
        }
        if (std::ranges::find(capabilities, *capability) == capabilities.end()) {
            capabilities.push_back(*capability);
        }
        start = end + 1;
    }
    return capabilities;
}

Reply Reply::success(json::Value value) {
    Reply reply;
    reply.value = std::move(value);
    return reply;
}

Reply Reply::failure(int code, std::string message, json::Value data) {
    Reply reply;
    reply.ok = false;
    reply.code = code;
    reply.message = std::move(message);
    reply.data = std::move(data);
    return reply;
}

Reply Reply::failure(const Error& error) {
    switch (error.code) {
    case ErrorCode::InvalidArgument:
        return failure(code::kInvalidArgument, error.message);
    case ErrorCode::NotFound:
        return failure(code::kNotFound, error.message);
    case ErrorCode::PlatformFailure:
        return failure(code::kPlatformFailure, error.message);
    case ErrorCode::PermissionDenied:
        return failure(code::kPermissionDenied, error.message);
    case ErrorCode::Unknown:
        break;
    }
    return failure(code::kInternalError, error.message);
}

Reply Reply::invalid_params(const std::vector<json::ValidationError>& errors) {
    SLASH_ASSERT(!errors.empty(), "invalid_params needs at least one error");
    const json::ValidationError& first = errors.front();
    std::string message =
        (first.path.empty() ? std::string("params") : first.path) + ": " + first.message;
    json::Value data;
    if (errors.size() == 1) {
        data["path"] = first.path;
        return failure(code::kInvalidParams, std::move(message), std::move(data));
    }
    message += " (and " + std::to_string(errors.size() - 1) + " more errors)";
    json::Array items;
    items.reserve(errors.size());
    for (const json::ValidationError& error : errors) {
        json::Value item;
        item["path"] = error.path;
        item["message"] = error.message;
        items.push_back(std::move(item));
    }
    data["errors"] = std::move(items);
    return failure(code::kInvalidParams, std::move(message), std::move(data));
}

void Registry::add(Command command) {
    SLASH_ASSERT(!command.name.empty(), "commands need a name");
    SLASH_ASSERT(command.handler != nullptr, "commands need a handler");
    SLASH_ASSERT(find(command.name) == nullptr, "two commands have the same name");
    const json::Value* type = command.params.find("type");
    SLASH_ASSERT(type != nullptr && type->is_string() && type->as_string() == "object",
                 "command params must be an object schema");
    if (changes_world(command.effect)) {
        json::Value& properties = command.params["properties"];
        SLASH_ASSERT(properties.find("dry_run") == nullptr,
                     "dry_run is added by the registry; leave it out of params");
        json::Value dry_run;
        dry_run["type"] = "boolean";
        dry_run["description"] = "Return what would change, or the error, and change nothing.";
        properties["dry_run"] = std::move(dry_run);
    }
    const auto position =
        std::ranges::upper_bound(commands_, command.name, std::less<>(),
                                 [](const Command& c) -> const std::string& { return c.name; });
    commands_.insert(position, std::move(command));
}

const Command* Registry::find(std::string_view name) const {
    const auto it =
        std::ranges::lower_bound(commands_, name, std::less<>(),
                                 [](const Command& c) -> const std::string& { return c.name; });
    return it != commands_.end() && it->name == name ? &*it : nullptr;
}

} // namespace slash::command
