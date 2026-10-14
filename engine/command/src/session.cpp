#include "slash/command/session.hpp"

#include "slash/core/json_schema.hpp"
#include "slash/core/log.hpp"
#include "slash/core/text.hpp"
#include "slash/world/scene.hpp"

#include <algorithm>
#include <cstdio>
#include <ranges>
#include <utility>

namespace slash::command {
namespace {

// How many log lines a session keeps for log.tail.
constexpr std::size_t kMaxLogLines = 1000;

} // namespace

Session::Session(const world::ComponentRegistry& component_registry,
                 const Registry& command_registry, SessionOptions session_options)
    : commands_(&command_registry), options_(std::move(session_options)),
      world_(component_registry) {
    if (options_.capture_logs) {
        log::set_sink([this](log::Level /*level*/, std::string_view line) {
            std::fwrite(line.data(), 1, line.size(), stderr);
            std::fputc('\n', stderr);
            std::fflush(stderr);
            const std::scoped_lock lock(logs_mutex_);
            logs_.emplace_back(line);
            if (logs_.size() > kMaxLogLines) {
                logs_.pop_front();
            }
        });
    }
}

Session::~Session() {
    if (options_.capture_logs) {
        log::reset_sink();
    }
}

Reply Session::call(std::string_view name, const json::Value& params) {
    const Command* command = commands_->find(name);
    if (command == nullptr) {
        std::vector<std::string_view> names;
        for (const Command& candidate : commands_->commands()) {
            names.emplace_back(candidate.name);
        }
        std::string message = "unknown command \"" + std::string(name) + "\"";
        if (const std::string_view hint = closest_match(name, names); !hint.empty()) {
            message += "; did you mean \"" + std::string(hint) + "\"?";
        }
        return Reply::failure(code::kMethodNotFound, std::move(message));
    }

    for (const Capability capability : command->capabilities) {
        if (!allowed(capability)) {
            json::Value data;
            data["capability"] = to_string(capability);
            return Reply::failure(code::kPermissionDenied,
                                  "\"" + command->name + "\" needs the \"" +
                                      std::string(to_string(capability)) +
                                      "\" capability, which this session does not grant",
                                  std::move(data));
        }
    }

    const json::Value empty = json::Object{};
    const json::Value& args = params.is_null() ? empty : params;
    if (const auto errors = json::validate(args, command->params); !errors.empty()) {
        return Reply::invalid_params(errors);
    }

    // Nested calls are snapshotted, recorded and rolled back by the outermost call.
    const bool snapshots = command->effect == Effect::Edit || command->effect == Effect::Replace ||
                           command->effect == Effect::Compound;
    if (depth_ > 0 || !snapshots) {
        return run(*command, args);
    }

    Snapshot before{command->name, world::save_scene(world_)};
    const std::string path_before = scene_path_;
    edited_ = false;
    replaced_ = false;
    Reply reply = run(*command, args);
    if (!reply.ok) {
        restore(before.scene);
        scene_path_ = path_before;
        return reply;
    }
    if (replaced_) {
        clear_history();
    } else if (edited_) {
        remember(undo_, std::move(before));
        redo_.clear();
    }
    return reply;
}

Reply Session::run(const Command& command, const json::Value& params) {
    ++depth_;
    Reply reply = command.handler(*this, params);
    --depth_;
    if (reply.ok) {
        edited_ = edited_ || command.effect == Effect::Edit;
        replaced_ = replaced_ || command.effect == Effect::Replace;
    }
    return reply;
}

bool Session::allowed(Capability capability) const {
    return std::ranges::find(options_.capabilities, capability) != options_.capabilities.end();
}

Result<std::string> Session::resolve_path(std::string_view relative) const {
    const auto invalid = [&](std::string_view why) {
        return make_error(ErrorCode::PermissionDenied,
                          "path \"" + std::string(relative) + "\" " + std::string(why));
    };
    if (relative.empty()) {
        return make_error(ErrorCode::InvalidArgument, "path is empty");
    }
    if (relative.front() == '/' || relative.front() == '\\') {
        return invalid("is absolute; use a path relative to the project folder");
    }
    if (relative.find(':') != std::string_view::npos) {
        return invalid("contains ':'; use a path relative to the project folder");
    }
    std::string path(relative);
    std::ranges::replace(path, '\\', '/');
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t end = std::min(path.find('/', start), path.size());
        if (std::string_view(path).substr(start, end - start) == "..") {
            return invalid("leaves the project folder");
        }
        start = end + 1;
    }
    if (options_.project_root.empty() || options_.project_root == ".") {
        return path;
    }
    std::string root = options_.project_root;
    if (root.back() != '/' && root.back() != '\\') {
        root += '/';
    }
    return root + path;
}

Reply Session::undo() {
    if (undo_.empty()) {
        return Reply::failure(code::kInvalidArgument, "nothing to undo");
    }
    const Snapshot target = std::move(undo_.back());
    undo_.pop_back();
    Snapshot current{target.label, world::save_scene(world_)};
    restore(target.scene);
    remember(redo_, std::move(current));
    json::Value result;
    result["undone"] = target.label;
    result["can_undo"] = undo_.size();
    result["can_redo"] = redo_.size();
    return Reply::success(std::move(result));
}

Reply Session::redo() {
    if (redo_.empty()) {
        return Reply::failure(code::kInvalidArgument, "nothing to redo");
    }
    const Snapshot target = std::move(redo_.back());
    redo_.pop_back();
    Snapshot current{target.label, world::save_scene(world_)};
    restore(target.scene);
    remember(undo_, std::move(current));
    json::Value result;
    result["redone"] = target.label;
    result["can_undo"] = undo_.size();
    result["can_redo"] = redo_.size();
    return Reply::success(std::move(result));
}

std::vector<std::string> Session::undo_labels() const {
    std::vector<std::string> labels;
    labels.reserve(undo_.size());
    for (const Snapshot& snapshot : undo_) {
        labels.push_back(snapshot.label);
    }
    return labels;
}

std::vector<std::string> Session::redo_labels() const {
    std::vector<std::string> labels;
    labels.reserve(redo_.size());
    for (const Snapshot& snapshot : std::views::reverse(redo_)) {
        labels.push_back(snapshot.label);
    }
    return labels;
}

void Session::clear_history() {
    undo_.clear();
    redo_.clear();
}

std::vector<std::string> Session::recent_logs(std::size_t count) const {
    const std::scoped_lock lock(logs_mutex_);
    const std::size_t first = logs_.size() > count ? logs_.size() - count : 0;
    return {logs_.begin() + static_cast<std::ptrdiff_t>(first), logs_.end()};
}

void Session::restore(const json::Value& scene) {
    world_.clear();
    // Snapshots are saved from a valid world, so loading one cannot fail.
    const auto loaded = world::load_scene(world_, scene);
    SLASH_ASSERT(loaded.ok(), "a session snapshot failed to load");
}

void Session::remember(std::deque<Snapshot>& stack, Snapshot snapshot) const {
    if (options_.history_limit == 0) {
        return;
    }
    stack.push_back(std::move(snapshot));
    while (stack.size() > options_.history_limit) {
        stack.pop_front();
    }
}

} // namespace slash::command
