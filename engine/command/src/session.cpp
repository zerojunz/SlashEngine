#include "slash/command/session.hpp"

#include "slash/core/json_schema.hpp"
#include "slash/core/log.hpp"
#include "slash/core/text.hpp"
#include "slash/platform/file.hpp"
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
      assets_(asset::FileSource{
          .read = [this](std::string_view path) -> Result<std::string> {
              auto file = resolve_path(path);
              if (!file) {
                  return file.error();
              }
              return platform::read_file(file.value());
          },
          .stamp = [this](std::string_view path) -> std::optional<platform::FileStamp> {
              auto file = resolve_path(path);
              return file ? platform::file_stamp(file.value()) : std::nullopt;
          },
      }),
      world_(component_registry) {
    world_.set_event_listener([this](const world::Event& event) { on_world_event(event); });
    world_.set_file_reader(
        [this](std::string_view path) { return assets_.read(path, world::kPrefabType); });
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
    world_.set_event_listener(nullptr);
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

    // Each command sees files as they are now: changed assets reload, and prefab files are read
    // afresh.
    if (depth_ == 0) {
        poll_assets();
        world_.forget_prefabs();
    }

    // Nested calls are snapshotted, recorded and rolled back by the outermost call.
    const bool snapshots = changes_world(command->effect);

    // dry_run is the session's param, added to the schema by the registry, so the command never
    // sees it.
    bool dry_run = false;
    json::Value without_dry_run;
    const json::Value* command_args = &args;
    if (const json::Value* flag = snapshots ? args.find("dry_run") : nullptr; flag != nullptr) {
        dry_run = flag->as_bool();
        without_dry_run = args;
        without_dry_run.as_object().erase("dry_run");
        command_args = &without_dry_run;
    }
    if (dry_run && depth_ > 0) {
        json::Value data;
        data["path"] = "/dry_run";
        return Reply::failure(code::kInvalidParams,
                              "/dry_run: set dry_run on the batch, not on the commands in it",
                              std::move(data));
    }
    if (depth_ > 0 || !snapshots) {
        return run(*command, *command_args);
    }

    Snapshot before{command->name, save()};
    const std::string path_before = scene_path_;
    const world::World::Counters counters_before = world_.counters();
    edited_ = false;
    replaced_ = false;
    pending_events_.clear();
    buffering_events_ = true;
    dry_running_ = dry_run;
    Reply reply = run(*command, *command_args);
    dry_running_ = false;
    buffering_events_ = false;
    if (!reply.ok || dry_run) {
        pending_events_.clear();
        json::Value changes;
        if (reply.ok) {
            const SavedWorld after = save();
            changes = world::diff_scenes(components(), before.saved.scene, after.scene,
                                         before.saved.links, after.links);
        }
        restore(before.saved);
        scene_path_ = path_before;
        world_.set_counters(counters_before);
        if (!reply.ok) {
            return reply;
        }
        json::Value result;
        result["dry_run"] = true;
        result["result"] = std::move(reply.value);
        result["changes"] = std::move(changes);
        return Reply::success(std::move(result));
    }
    // A new scene's entities are not news; its command event says what changed.
    for (const world::Event& event : pending_events_) {
        if (!replaced_ || !event.kind.starts_with("entity.")) {
            publish(event);
        }
    }
    pending_events_.clear();
    if ((edited_ || replaced_) && wants("command")) {
        publish_changes(command->name, before.saved, save());
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
    Snapshot current{target.label, save()};
    restore(target.saved);
    publish_changes("undo", current.saved, target.saved);
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
    Snapshot current{target.label, save()};
    restore(target.saved);
    publish_changes("redo", current.saved, target.saved);
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

Result<void> Session::take_snapshot(std::string name) {
    if (snapshots_.size() >= kMaxSnapshots && !snapshots_.contains(name)) {
        return make_error(ErrorCode::InvalidArgument,
                          "a session keeps at most " + std::to_string(kMaxSnapshots) +
                              " snapshots; reuse a name to replace one");
    }
    snapshots_[std::move(name)] = save();
    return {};
}

const Session::SavedWorld* Session::find_snapshot(std::string_view name) const {
    const auto it = snapshots_.find(name);
    return it != snapshots_.end() ? &it->second : nullptr;
}

std::vector<std::string_view> Session::snapshot_names() const {
    std::vector<std::string_view> names;
    names.reserve(snapshots_.size());
    for (const auto& [name, unused] : snapshots_) {
        names.emplace_back(name);
    }
    return names;
}

std::vector<std::string> Session::recent_logs(std::size_t count) const {
    const std::scoped_lock lock(logs_mutex_);
    const std::size_t first = logs_.size() > count ? logs_.size() - count : 0;
    return {logs_.begin() + static_cast<std::ptrdiff_t>(first), logs_.end()};
}

void Session::poll_assets() {
    std::vector<asset::Assets::Change> changes = assets_.poll();
    if (changes.empty()) {
        return;
    }
    std::optional<SavedWorld> before;
    world::PrefabReload reload;
    if (std::ranges::any_of(changes, [](const asset::Assets::Change& change) {
            return change.type == world::kPrefabType;
        })) {
        if (wants("command")) {
            before = save();
        }
        reload = world::reload_prefabs(world_);
        for (const json::ValidationError& error : reload.errors) {
            log::warn("prefab did not reload", {{"path", error.path}, {"detail", error.message}});
            const auto it = std::ranges::find(changes, error.path, &asset::Assets::Change::path);
            if (it != changes.end()) {
                it->error = error.message;
            } else {
                changes.push_back({error.path, std::string(world::kPrefabType), 0, error.message});
            }
        }
    }
    for (const asset::Assets::Change& change : changes) {
        json::Value data;
        data["path"] = change.path;
        data["type"] = change.type;
        if (change.error.empty()) {
            data["version"] = change.version;
        } else {
            data["error"] = change.error;
        }
        publish({"asset.reloaded", "", std::move(data), world_.tick()});
    }
    if (before && !reload.changed.empty()) {
        publish_changes("asset.reload", *before, save());
    }
}

Session::SavedWorld Session::save() const {
    return {world::save_scene(world_), world_.prefab_links()};
}

void Session::restore(const SavedWorld& saved) {
    muted_events_ = true;
    world_.clear();
    // Copies are saved from a valid world with the prefab each entity extends, so loading one
    // reads no files and cannot fail.
    const auto loaded = world::load_scene(world_, saved.scene, saved.links);
    muted_events_ = false;
    SLASH_ASSERT(loaded.ok(), "a session snapshot failed to load");
}

Result<std::string> Session::subscribe(std::vector<std::string> kinds,
                                       std::vector<std::string> ids) {
    if (subscriptions_.size() >= kMaxSubscriptions) {
        return make_error(ErrorCode::InvalidArgument, "a session keeps at most " +
                                                          std::to_string(kMaxSubscriptions) +
                                                          " subscriptions; unsubscribe one first");
    }
    std::string name = "events_" + std::to_string(next_subscription_++);
    subscriptions_[name] = Subscription{std::move(kinds), std::move(ids), {}, 0};
    return name;
}

std::optional<Session::EventBatch> Session::read_events(std::string_view subscription,
                                                        std::size_t limit) {
    const auto it = subscriptions_.find(subscription);
    if (it == subscriptions_.end()) {
        return std::nullopt;
    }
    Subscription& queue = it->second;
    EventBatch batch;
    while (!queue.events.empty() && batch.events.size() < limit) {
        batch.events.push_back(std::move(queue.events.front()));
        queue.events.pop_front();
    }
    batch.dropped = std::exchange(queue.dropped, 0);
    batch.more = !queue.events.empty();
    return batch;
}

bool Session::unsubscribe(std::string_view subscription) {
    const auto it = subscriptions_.find(subscription);
    if (it == subscriptions_.end()) {
        return false;
    }
    subscriptions_.erase(it);
    return true;
}

std::vector<std::string_view> Session::subscription_names() const {
    std::vector<std::string_view> names;
    names.reserve(subscriptions_.size());
    for (const auto& [name, unused] : subscriptions_) {
        names.emplace_back(name);
    }
    return names;
}

void Session::on_world_event(const world::Event& event) {
    if (muted_events_ || subscriptions_.empty()) {
        return;
    }
    if (buffering_events_) {
        pending_events_.push_back(event);
        return;
    }
    publish(event);
}

void Session::publish(const world::Event& event) {
    json::Value item;
    item["tick"] = event.tick;
    item["kind"] = event.kind;
    std::vector<std::string> ids;
    if (!event.id.empty()) {
        item["id"] = event.id;
        ids.push_back(event.id);
    }
    if (!event.data.is_null()) {
        item["data"] = event.data;
    }
    deliver(event.kind, ids, std::move(item));
}

void Session::publish_changes(std::string_view command, const SavedWorld& before,
                              const SavedWorld& after) {
    if (!wants("command")) {
        return;
    }
    json::Value changes =
        world::diff_scenes(components(), before.scene, after.scene, before.links, after.links);
    std::vector<std::string> ids;
    for (const std::string_view section : {"added", "removed"}) {
        if (const json::Value* list = changes.find(section); list != nullptr) {
            for (const json::Value& id : list->as_array()) {
                ids.push_back(id.as_string());
            }
        }
    }
    if (const json::Value* changed = changes.find("changed"); changed != nullptr) {
        for (const auto& [id, unused] : changed->as_object()) {
            ids.push_back(id);
        }
    }
    json::Value item;
    item["tick"] = world_.tick();
    item["kind"] = "command";
    item["command"] = command;
    item["changes"] = std::move(changes);
    deliver("command", ids, std::move(item));
}

bool Session::wants(std::string_view kind) const {
    return std::ranges::any_of(subscriptions_, [&](const auto& entry) {
        const std::vector<std::string>& kinds = entry.second.kinds;
        return kinds.empty() || std::ranges::find(kinds, kind) != kinds.end();
    });
}

void Session::deliver(std::string_view kind, const std::vector<std::string>& ids,
                      json::Value event) {
    const auto matches = [&](const Subscription& subscription) {
        const bool kind_ok =
            subscription.kinds.empty() ||
            std::ranges::find(subscription.kinds, kind) != subscription.kinds.end();
        const bool id_ok =
            subscription.ids.empty() || std::ranges::any_of(ids, [&](const std::string& id) {
                return std::ranges::find(subscription.ids, id) != subscription.ids.end();
            });
        return kind_ok && id_ok;
    };
    bool numbered = false;
    for (auto& [name, subscription] : subscriptions_) {
        if (!matches(subscription)) {
            continue;
        }
        if (!numbered) {
            event["seq"] = next_event_++;
            numbered = true;
        }
        subscription.events.push_back(event);
        if (subscription.events.size() > kMaxQueuedEvents) {
            subscription.events.pop_front();
            ++subscription.dropped;
        }
    }
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
