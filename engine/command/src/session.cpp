#include "slash/command/session.hpp"

#include "slash/animation/animation.hpp"
#include "slash/canvas/canvas.hpp"
#include "slash/core/assert.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/log.hpp"
#include "slash/core/text.hpp"
#include "slash/physics2d/physics2d.hpp"
#include "slash/platform/file.hpp"
#include "slash/ui/ui.hpp"
#include "slash/world/scene.hpp"

#include <algorithm>
#include <cstdio>
#include <ranges>
#include <utility>

namespace slash::command {
namespace {

// How many log lines a session keeps for log.tail.
constexpr std::size_t kMaxLogLines = 1000;

std::string quoted(std::string_view text) {
    return "\"" + std::string(text) + "\"";
}

// Whether two copies of the world hold the same entities, components and prefab links.
bool same_world(const Session::SavedWorld& a, const Session::SavedWorld& b) {
    return a.scene == b.scene &&
           std::ranges::equal(a.links, b.links, [](const auto& left, const auto& right) {
               return left.first == right.first && left.second.path == right.second.path;
           });
}

// The project's project.json, or null if it has none or it doesn't parse.
json::Value read_project(const std::string& project_root) {
    auto text = platform::read_file_in(project_root, "project.json");
    if (!text) {
        return {};
    }
    auto project = json::parse(text.value());
    return project && project.value().is_object() ? std::move(project).value() : json::Value();
}

// The project's physics_2d settings, or the defaults if it has none or they don't validate.
physics2d::Settings physics_settings(const json::Value& project) {
    const json::Value* settings = project.is_object() ? project.find("physics_2d") : nullptr;
    if (settings == nullptr || !json::validate(*settings, physics2d::settings_schema()).empty()) {
        return {};
    }
    return physics2d::parse_settings(*settings);
}

// The viewport controls are laid out in: the project's window size, or the default.
ui::Settings ui_settings(const json::Value& project) {
    ui::Settings settings;
    const json::Value* window = project.is_object() ? project.find("window") : nullptr;
    const json::Value* width =
        window != nullptr && window->is_object() ? window->find("width") : nullptr;
    const json::Value* height =
        window != nullptr && window->is_object() ? window->find("height") : nullptr;
    if (width != nullptr && height != nullptr && width->is_number() && height->is_number() &&
        width->as_double() > 0.0 && height->as_double() > 0.0) {
        settings.viewport = {static_cast<float>(width->as_double()),
                             static_cast<float>(height->as_double())};
    }
    return settings;
}

} // namespace

Session::Session(const world::ComponentRegistry& component_registry,
                 const Registry& command_registry, SessionOptions session_options)
    : commands_(&command_registry), options_(std::move(session_options)),
      assets_(asset::FileSource{
          .read = [this](std::string_view path) -> Result<std::string> {
              if (auto file = resolve_path(path); !file) {
                  return file.error();
              }
              return platform::read_file_in(options_.project_root, path);
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
    // Textures, fonts, sprite frames, animation libraries and the systems that play
    // AnimatedSprite2D and AnimationPlayer, so step animates, and 2D physics.
    const json::Value project = read_project(options_.project_root);
    canvas::install(world_, assets_);
    ui::set_settings(world_, ui_settings(project));
    animation::install(world_, assets_);
    physics2d::install(world_, physics_settings(project));
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

    Snapshot before{command->name, save(), {}, true};
    const std::string path_before = scene_path_;
    const world::World::Counters counters_before = world_.counters();
    edited_ = false;
    replaced_ = false;
    pending_events_.clear();
    buffering_events_ = true;
    dry_running_ = dry_run;
    undo_calls_.clear();
    begin_files();
    Reply reply = run(*command, *command_args);
    dry_running_ = false;
    buffering_events_ = false;
    if (reply.ok && !dry_run) {
        if (auto committed = commit_files(); !committed) {
            reply = Reply::failure(committed.error());
        }
    }
    discard_files();
    std::vector<UndoCall> calls = std::move(undo_calls_);
    undo_calls_.clear();
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
    // A command that changed only files, such as storyboard.edit, leaves the world alone on undo.
    bool world_changed = true;
    std::optional<SavedWorld> after;
    if (!calls.empty()) {
        after = save();
        world_changed = !same_world(before.saved, *after);
    }
    if ((edited_ || replaced_) && world_changed && wants("command")) {
        publish_changes(command->name, before.saved, after ? *after : save());
    }
    if (replaced_) {
        clear_history();
    } else if (edited_) {
        before.calls = std::move(calls);
        before.restores_world = world_changed;
        if (!world_changed) {
            before.saved = {};
        }
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
    if (!platform::reserved_device_name(relative).empty()) {
        return platform::reserved_device_error(relative);
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
    if (Reply replayed = replay(undo_.back(), true); !replayed.ok) {
        return replayed;
    }
    Snapshot target = std::move(undo_.back());
    undo_.pop_back();
    Snapshot current{target.label, target.restores_world ? save() : SavedWorld{},
                     std::move(target.calls), target.restores_world};
    if (target.restores_world) {
        restore(target.saved);
        publish_changes("undo", current.saved, target.saved);
    }
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
    if (Reply replayed = replay(redo_.back(), false); !replayed.ok) {
        return replayed;
    }
    Snapshot target = std::move(redo_.back());
    redo_.pop_back();
    Snapshot current{target.label, target.restores_world ? save() : SavedWorld{},
                     std::move(target.calls), target.restores_world};
    if (target.restores_world) {
        restore(target.saved);
        publish_changes("redo", current.saved, target.saved);
    }
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

Result<std::optional<std::string>> Session::read_disk(std::string_view path) const {
    auto file = resolve_path(path);
    if (!file) {
        return file.error();
    }
    if (!platform::exists(file.value())) {
        return std::optional<std::string>();
    }
    auto text = platform::read_file(file.value());
    if (!text) {
        return text.error();
    }
    return std::optional<std::string>(std::move(text).value());
}

Result<std::optional<std::string>> Session::read_project_file(std::string_view path) {
    if (!staging_) {
        return read_disk(path);
    }
    auto file = staged(path);
    if (!file) {
        return file.error();
    }
    const StagedFile& staged_file = *file.value();
    const std::optional<std::string>& text = staged_file.text ? staged_file.text : staged_file.seen;
    if (!text && staged_file.append.empty()) {
        return std::optional<std::string>();
    }
    return std::optional<std::string>(text.value_or(std::string()) + staged_file.append);
}

Result<void> Session::stage_file(std::string_view path, std::string text) {
    auto file = staged(path);
    if (!file) {
        return file.error();
    }
    file.value()->text = std::move(text);
    return {};
}

Result<void> Session::stage_append(std::string_view path, std::string_view text) {
    auto file = staged(path);
    if (!file) {
        return file.error();
    }
    file.value()->append += text;
    return {};
}

void Session::add_undo_call(std::string command, json::Value undo_params, json::Value redo_params) {
    if (!replaying_) {
        undo_calls_.push_back({std::move(command), std::move(undo_params), std::move(redo_params)});
    }
}

void Session::begin_files() {
    staged_.clear();
    staging_ = true;
}

void Session::discard_files() {
    staged_.clear();
    staging_ = false;
}

Result<Session::StagedFile*> Session::staged(std::string_view path) {
    if (!staging_) {
        return make_error(ErrorCode::InvalidArgument,
                          "only commands that change the world can write project files");
    }
    auto it = staged_.find(path);
    if (it == staged_.end()) {
        auto seen = read_disk(path);
        if (!seen) {
            return seen.error();
        }
        it = staged_.emplace(std::string(path), StagedFile{std::move(seen).value(), {}, {}}).first;
    }
    return &it->second;
}

Result<void> Session::commit_files() {
    // Nothing is written if a file changed on disk since the command read it.
    for (const auto& [path, file] : staged_) {
        if (!file.text) {
            continue;
        }
        auto now = read_disk(path);
        if (!now) {
            return now.error();
        }
        if (now.value() != file.seen) {
            return make_error(ErrorCode::Conflict,
                              quoted(path) +
                                  " changed on disk while the command ran; nothing was written, "
                                  "so read it again and retry");
        }
    }
    for (const auto& [path, file] : staged_) {
        if (!file.text && file.append.empty()) {
            continue;
        }
        auto resolved = resolve_path(path);
        if (!resolved) {
            return resolved.error();
        }
        std::string text;
        if (file.text) {
            text = *file.text;
        } else {
            // Logs are appended to what is on disk now, whoever else wrote to them.
            auto now = read_disk(path);
            if (!now) {
                return now.error();
            }
            text = now.value().value_or(std::string());
        }
        text += file.append;
        if (const std::size_t slash = resolved.value().rfind('/'); slash != std::string::npos) {
            if (auto created = platform::create_directories(resolved.value().substr(0, slash));
                !created) {
                return created.error();
            }
        }
        if (auto written = platform::write_file(resolved.value(), text); !written) {
            return written.error();
        }
    }
    return {};
}

Reply Session::replay(const Snapshot& step, bool undo) {
    if (step.calls.empty()) {
        return Reply::success({});
    }
    begin_files();
    replaying_ = true;
    Reply reply = Reply::success({});
    const auto run_call = [&](const UndoCall& call) {
        const Command* command = commands_->find(call.command);
        SLASH_ASSERT(command != nullptr, "undo calls name registered commands");
        reply = run(*command, undo ? call.undo : call.redo);
        return reply.ok;
    };
    if (undo) {
        for (const UndoCall& call : std::views::reverse(step.calls)) {
            if (!run_call(call)) {
                break;
            }
        }
    } else {
        for (const UndoCall& call : step.calls) {
            if (!run_call(call)) {
                break;
            }
        }
    }
    replaying_ = false;
    if (reply.ok) {
        if (auto committed = commit_files(); !committed) {
            reply = Reply::failure(committed.error());
        }
    }
    discard_files();
    if (!reply.ok) {
        return Reply::failure(reply.code,
                              std::string(undo ? "could not undo " : "could not redo ") +
                                  quoted(step.label) + ": " + reply.message,
                              std::move(reply.data));
    }
    return reply;
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
    // Copies are saved from this world with the prefab each entity extends, so restoring one
    // reads no files and cannot fail, even when a system moved a field outside its range.
    const auto loaded = world::restore_scene(world_, saved.scene, saved.links);
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
