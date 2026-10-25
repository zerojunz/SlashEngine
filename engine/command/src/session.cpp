#include "slash/command/session.hpp"

#include "slash/ai/transport.hpp"
#include "slash/ai/world.hpp"
#include "slash/animation/animation.hpp"
#include "slash/audio/audio.hpp"
#include "slash/canvas/canvas.hpp"
#include "slash/core/assert.hpp"
#include "slash/core/jobs.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/log.hpp"
#include "slash/core/profile.hpp"
#include "slash/core/text.hpp"
#include "slash/gameplay/host.hpp"
#include "slash/input/input.hpp"
#include "slash/navigation/navigation.hpp"
#include "slash/physics2d/physics2d.hpp"
#include "slash/physics3d/physics3d.hpp"
#include "slash/platform/file.hpp"
#include "slash/scene3d/blend.hpp"
#include "slash/services/services.hpp"
#include "slash/ui/ui.hpp"
#include "slash/world/scene.hpp"
#include "slash/world/tree2d.hpp"

#include <algorithm>
#include <cstdio>
#include <ranges>
#include <utility>

namespace slash::command {
namespace {

// How many log lines a session keeps for log.tail.
constexpr std::size_t kMaxLogLines = 1000;

std::string in_quotes(std::string_view text) {
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

// The project's physics_3d settings, or the defaults if it has none or they don't validate.
physics3d::Settings physics_3d_settings(const json::Value& project) {
    const json::Value* settings = project.is_object() ? project.find("physics_3d") : nullptr;
    if (settings == nullptr || !json::validate(*settings, physics3d::settings_schema()).empty()) {
        return {};
    }
    return physics3d::parse_settings(*settings);
}

// The project's navigation_2d and navigation_3d settings; a map whose settings don't validate
// keeps the defaults.
navigation::Settings navigation_settings(const json::Value& project) {
    navigation::Settings out;
    if (!project.is_object()) {
        return out;
    }
    for (const navigation::Map map : {navigation::Map::TwoD, navigation::Map::ThreeD}) {
        const json::Value* settings =
            project.find(map == navigation::Map::TwoD ? "navigation_2d" : "navigation_3d");
        if (settings == nullptr ||
            !json::validate(*settings, navigation::settings_schema(map)).empty()) {
            continue;
        }
        (map == navigation::Map::TwoD ? out.map_2d : out.map_3d) =
            navigation::parse_settings(map, *settings);
    }
    return out;
}

// The project's threading.max_threads, or -1 (one thread per core) if it doesn't validate.
int max_threads(const json::Value& project) {
    const json::Value* threading = project.is_object() ? project.find("threading") : nullptr;
    if (threading == nullptr || !json::validate(*threading, jobs::settings_schema()).empty()) {
        return -1;
    }
    return jobs::parse_max_threads(*threading);
}

// The project's input actions over Godot's ui_* ones, or only those if its "input" doesn't
// validate.
input::Map input_map(const json::Value& project) {
    const json::Value* map = project.is_object() ? project.find("input") : nullptr;
    if (map == nullptr) {
        return input::Map::with_defaults();
    }
    auto parsed = input::parse_map(*map);
    return parsed ? std::move(parsed).value() : input::Map::with_defaults();
}

input::Settings input_settings(const json::Value& project) {
    const json::Value* settings = project.is_object() ? project.find("input_devices") : nullptr;
    if (settings == nullptr || !json::validate(*settings, input::settings_schema()).empty()) {
        return {};
    }
    return input::parse_settings(*settings);
}

// How long sessions keep each bus's sound for audio.capture.
constexpr double kAudioHistorySeconds = 10.0;

// The project's audio settings, or the defaults if they don't validate.
audio::Settings audio_settings(const json::Value& project) {
    const json::Value* settings = project.is_object() ? project.find("audio") : nullptr;
    audio::Settings out;
    if (settings != nullptr && audio::validate_settings(*settings, "").empty()) {
        out = audio::parse_settings(*settings);
    }
    out.history_seconds = kAudioHistorySeconds;
    return out;
}

// The project's ad placements, products and consent, or none if they don't validate.
services::Settings services_settings(const json::Value& project) {
    const json::Value* settings = project.is_object() ? project.find("services") : nullptr;
    if (settings == nullptr || !services::validate_settings(*settings, "").empty()) {
        return {};
    }
    return services::parse_settings(*settings);
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
    if (const json::Value* gui = project.is_object() ? project.find("gui") : nullptr;
        gui != nullptr) {
        ui::read_settings(*gui, settings);
    }
    return settings;
}

// How the project's frames look, for frame.capture: rendering's clear color and filter, and
// the tick length, or the defaults for what doesn't validate.
canvas::Settings canvas_settings(const json::Value& project) {
    canvas::Settings settings;
    if (!project.is_object()) {
        return settings;
    }
    const json::Value* rendering = project.find("rendering");
    if (rendering != nullptr && rendering->is_object()) {
        const json::Value* clear = rendering->find("clear_color");
        if (clear != nullptr && clear->is_array() && clear->as_array().size() == 4 &&
            std::ranges::all_of(clear->as_array(),
                                [](const json::Value& each) { return each.is_number(); })) {
            const json::Array& rgba = clear->as_array();
            settings.clear_color = {.r = static_cast<float>(rgba[0].as_double()),
                                    .g = static_cast<float>(rgba[1].as_double()),
                                    .b = static_cast<float>(rgba[2].as_double()),
                                    .a = static_cast<float>(rgba[3].as_double())};
        }
        const json::Value* filter = rendering->find("texture_filter");
        if (filter != nullptr && filter->is_string() && filter->as_string() == "nearest") {
            settings.texture_filter = render::Filter::Nearest;
        }
        if (const json::Value* scaling = rendering->find("scaling"); scaling != nullptr) {
            settings.scaling = render::scaling_from_json(*scaling);
        }
        if (const json::Value* method = rendering->find("rendering_method");
            method != nullptr && method->is_string()) {
            settings.rendering_method = render::parse_render_method(method->as_string())
                                            .value_or(render::RenderMethod::ForwardPlus);
        }
        if (const json::Value* shadows = rendering->find("lights_and_shadows");
            shadows != nullptr) {
            settings.shadows = render::shadow_settings_from_json(*shadows);
        }
        if (const json::Value* camera = rendering->find("camera");
            camera != nullptr && camera->is_object()) {
            if (const json::Value* dof = camera->find("depth_of_field"); dof != nullptr) {
                settings.depth_of_field = render::depth_of_field_settings_from_json(*dof);
            }
        }
        if (const json::Value* environment = rendering->find("environment");
            environment != nullptr && environment->is_object()) {
            if (const json::Value* ssao = environment->find("ssao"); ssao != nullptr) {
                settings.ssao = render::ssao_settings_from_json(*ssao);
            }
            if (const json::Value* ssr = environment->find("screen_space_reflection");
                ssr != nullptr) {
                settings.ssr = render::ssr_settings_from_json(*ssr);
            }
            if (const json::Value* fog = environment->find("volumetric_fog"); fog != nullptr) {
                settings.volumetric_fog = render::volumetric_fog_settings_from_json(*fog);
            }
        }
        if (const json::Value* reflections = rendering->find("reflections");
            reflections != nullptr && reflections->is_object()) {
            if (const json::Value* atlas = reflections->find("reflection_atlas");
                atlas != nullptr) {
                settings.reflection_atlas = render::reflection_atlas_settings_from_json(*atlas);
            }
        }
        settings.voxel_gi_high_quality = render::voxel_gi_high_quality(*rendering);
        settings.use_taa = render::use_taa(*rendering);
        settings.decal_filter = render::decal_filter(*rendering);
        settings.mesh_lod_threshold = render::mesh_lod_threshold(*rendering);
        settings.occlusion_culling = render::occlusion_culling(*rendering);
    }
    const json::Value* tick_hz = project.find("tick_hz");
    if (tick_hz != nullptr && tick_hz->is_number() && tick_hz->as_double() > 0.0) {
        settings.tick_seconds = 1.0 / tick_hz->as_double();
    }
    return settings;
}

} // namespace

Session::Session(const world::ComponentRegistry& component_registry,
                 const Registry& command_registry, SessionOptions session_options)
    : commands_(&command_registry), options_(std::move(session_options)),
      // .blend files read as the glTF the artist's Blender makes of them, which runs a program,
      // so only for callers with the build capability.
      assets_(scene3d::with_blender(
          asset::FileSource{
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
          },
          {.project_root = options_.project_root,
           .run_blender = std::ranges::find(options_.capabilities, Capability::Build) !=
                          options_.capabilities.end(),
           .why_not = "this session runs Blender only with the build capability (--allow build)",
           .blender = {},
           .timeout_seconds = 600})),
      world_(component_registry) {
    world_.set_event_listener([this](const world::Event& event) { on_world_event(event); });
    world_.set_file_reader(
        [this](std::string_view path) { return assets_.read(path, world::kPrefabType); });
    // Textures, fonts, sprite frames, animation libraries and the systems that play
    // AnimatedSprite2D and AnimationPlayer, so step animates, 2D physics, input actions and
    // sound.
    if (auto file = resolve_path("project.json")) {
        project_stamp_ = platform::file_stamp(file.value());
    }
    const json::Value project = read_project(options_.project_root);
    // The job system's threads, if nothing has started it yet.
    jobs::configure(max_threads(project));
    canvas::install(world_, assets_);
    canvas::set_settings(world_, canvas_settings(project));
    ui::set_settings(world_, ui_settings(project));
    animation::install(world_, assets_);
    physics2d::install(world_, physics_settings(project));
    physics3d::install(world_, physics_3d_settings(project));
    navigation::install(world_, assets_, navigation_settings(project));
    input::install(world_, input_map(project), input_settings(project));
    audio::install(world_, assets_, audio_settings(project));
    // Ads load and purchases complete as services.test says, with no store account.
    services::install(world_, services_settings(project));
    services::use_test_backend(world_);
    // Gameplay code asks the models providers.json names. Ticks wait for replies, so runs
    // repeat; a session without the network capability answers only from the replay file.
    ai::OpenOptions open;
    open.project_root = options_.project_root;
    open.write_replays = options_.write_files;
    if (!allowed(Capability::Network)) {
        open.transport = ai::refusing_transport(
            "this session may not call models: it lacks the network capability");
    }
    ai::install(world_, {.open = std::move(open), .wait = true});
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
    if (options_.gameplay != nullptr) {
        // Before any scene loads, so scenes may use the game's components.
        (void)options_.gameplay->attach(world_);
    }
}

Session::~Session() {
    if (options_.gameplay != nullptr) {
        options_.gameplay->detach(world_);
    }
    world_.set_event_listener(nullptr);
    if (options_.capture_logs) {
        log::reset_sink();
    }
}

Reply Session::call(std::string_view name, const json::Value& params) {
    SLASH_PROFILE_ZONE_NAMED(name);
    const bool outermost = depth_ == 0;
    const std::uint64_t tick = world_.tick();
    Reply reply = call_command(name, params);
    if (outermost && reply.ok && call_listener_) {
        const json::Value* dry_run = params.is_object() ? params.find("dry_run") : nullptr;
        if (dry_run == nullptr || !dry_run->is_bool() || !dry_run->as_bool()) {
            // A copy, since the listener may stop listening.
            const CallListener listener = call_listener_;
            listener(name, params.is_null() ? json::Value(json::Object{}) : params, tick);
        }
    }
    return reply;
}

Reply Session::call_command(std::string_view name, const json::Value& params) {
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
        if (command->effect != Effect::Read) {
            ++revision_;
        }
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

    Snapshot before{command->name, save(), {}, true, {}};
    // Input events a failed command or dry run stepped through wait for the next tick again,
    // focus, hover and buttons being pressed are as they were, and so are the world's events.
    // Requests to models asked meanwhile are cancelled, and agents' paths are as they were.
    const input::Snapshot input_before = input::save(world_);
    const ui::Snapshot ui_before = ui::save(world_);
    const audio::Snapshot audio_before = audio::save(world_);
    const services::Snapshot services_before = services::save(world_);
    const ai::Snapshot ai_before = ai::save(world_);
    const navigation::Snapshot navigation_before = navigation::save(world_);
    const std::string path_before = scene_path_;
    const world::World::Counters counters_before = world_.counters();
    world::World::EventQueues events_before = world_.event_queues();
    edited_ = false;
    replaced_ = false;
    pending_events_.clear();
    buffering_events_ = true;
    dry_running_ = dry_run;
    undo_calls_.clear();
    undo_files_.clear();
    begin_files();
    const std::vector<world::TreeProblem> tree_before = world::check_tree(world_);
    Reply reply = run(*command, *command_args);
    if (reply.ok && (edited_ || replaced_)) {
        reply = keeps_tree(tree_before, std::move(reply));
    }
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
    std::vector<UndoFile> files = std::move(undo_files_);
    undo_files_.clear();
    if (!reply.ok || dry_run) {
        pending_events_.clear();
        json::Value changes;
        if (reply.ok) {
            const SavedWorld after = save();
            changes = world::diff_scenes(components(), before.saved.scene, after.scene,
                                         before.saved.links, after.links);
        }
        restore(before.saved);
        input::restore(world_, input_before);
        ui::restore(world_, ui_before);
        audio::restore(world_, audio_before);
        services::restore(world_, services_before);
        ai::restore(world_, ai_before);
        navigation::restore(world_, navigation_before);
        scene_path_ = path_before;
        world_.set_counters(counters_before);
        world_.set_event_queues(std::move(events_before));
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
    if (!calls.empty() || !files.empty()) {
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
        before.files = std::move(files);
        before.restores_world = world_changed;
        if (!world_changed) {
            before.saved = {};
        }
        remember(undo_, std::move(before));
        redo_.clear();
    }
    return reply;
}

Reply Session::keeps_tree(const std::vector<world::TreeProblem>& before, Reply reply) const {
    // As Godot's add_child refuses a node that has a parent or is an ancestor: an edit that
    // breaks the Transform2D children's tree fails, unless the tree was already broken there.
    for (const world::TreeProblem& problem : world::check_tree(world_)) {
        if (std::ranges::none_of(before, [&](const world::TreeProblem& old) {
                return old.path == problem.path && old.message == problem.message;
            })) {
            json::Value data;
            data["path"] = problem.path;
            return Reply::failure(code::kInvalidArgument, problem.path + ": " + problem.message,
                                  std::move(data));
        }
    }
    return reply;
}

Reply Session::run(const Command& command, const json::Value& params) {
    ++depth_;
    const bool outer_unchanged = unchanged_;
    unchanged_ = false;
    Reply reply = command.handler(*this, params);
    const bool unchanged = unchanged_;
    unchanged_ = outer_unchanged;
    --depth_;
    if (reply.ok) {
        edited_ = edited_ || (command.effect == Effect::Edit && !unchanged);
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

Result<void> Session::write_file(const std::string& full, std::string_view contents) const {
    if (!options_.write_files) {
        return make_error(ErrorCode::PermissionDenied,
                          "tests don't write files, so the project stays as it was");
    }
    if (const std::size_t slash = full.find_last_of('/');
        slash != std::string::npos && slash > 0 && !platform::exists(full.substr(0, slash))) {
        if (auto made = platform::create_directories(full.substr(0, slash)); !made) {
            return made.error();
        }
    }
    return platform::write_file(full, contents);
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
                     std::move(target.calls), target.restores_world, std::move(target.files)};
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
                     std::move(target.calls), target.restores_world, std::move(target.files)};
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
    if (staged_file.remove) {
        return std::optional<std::string>();
    }
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
    file.value()->remove = false;
    return {};
}

Result<void> Session::stage_remove(std::string_view path) {
    auto file = staged(path);
    if (!file) {
        return file.error();
    }
    file.value()->text.reset();
    file.value()->append.clear();
    file.value()->remove = true;
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

void Session::add_undo_file(std::string path, std::optional<std::string> before,
                            std::string after) {
    if (!replaying_) {
        undo_files_.push_back({std::move(path), std::move(before), std::move(after)});
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
        if (!file.text && !file.remove) {
            continue;
        }
        auto now = read_disk(path);
        if (!now) {
            return now.error();
        }
        if (now.value() != file.seen) {
            return make_error(ErrorCode::Conflict,
                              in_quotes(path) +
                                  " changed on disk while the command ran; nothing was written, "
                                  "so read it again and retry");
        }
    }
    for (const auto& [path, file] : staged_) {
        if (!file.text && file.append.empty() && (!file.remove || !file.seen)) {
            continue;
        }
        auto resolved = resolve_path(path);
        if (!resolved) {
            return resolved.error();
        }
        // Assets reload what was written even when its stamp didn't change.
        assets_.mark_changed(path);
        if (file.remove) {
            if (auto removed = platform::remove_file(resolved.value()); !removed) {
                return removed.error();
            }
            continue;
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
        if (auto written = write_file(resolved.value(), text); !written) {
            return written.error();
        }
    }
    return {};
}

Reply Session::replay(const Snapshot& step, bool undo) {
    if (step.calls.empty() && step.files.empty()) {
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
    const auto put_back = [&](const UndoFile& file) {
        const std::optional<std::string>& expected = undo ? file.after : file.before;
        const std::optional<std::string>& wanted = undo ? file.before : file.after;
        auto now = read_project_file(file.path);
        if (!now) {
            reply = Reply::failure(now.error());
            return false;
        }
        if (now.value() != expected) {
            reply = Reply::failure(code::kConflict,
                                   in_quotes(file.path) + " changed since; nothing was changed");
            return false;
        }
        auto staged = wanted ? stage_file(file.path, *wanted) : stage_remove(file.path);
        if (!staged) {
            reply = Reply::failure(staged.error());
        }
        return staged.ok();
    };
    if (undo) {
        for (const UndoFile& file : std::views::reverse(step.files)) {
            if (!put_back(file)) {
                break;
            }
        }
        for (const UndoCall& call : std::views::reverse(step.calls)) {
            if (!reply.ok || !run_call(call)) {
                break;
            }
        }
    } else {
        for (const UndoCall& call : step.calls) {
            if (!run_call(call)) {
                break;
            }
        }
        for (const UndoFile& file : step.files) {
            if (!reply.ok || !put_back(file)) {
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
                                  in_quotes(step.label) + ": " + reply.message,
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

void Session::poll_project() {
    auto file = resolve_path("project.json");
    const std::optional<platform::FileStamp> stamp =
        file ? platform::file_stamp(file.value()) : std::nullopt;
    if (stamp == project_stamp_) {
        return;
    }
    project_stamp_ = stamp;
    const json::Value project = read_project(options_.project_root);
    canvas::set_settings(world_, canvas_settings(project));
    ui::set_settings(world_, ui_settings(project));
    physics2d::set_settings(world_, physics_settings(project));
    physics3d::set_settings(world_, physics_3d_settings(project));
    navigation::set_settings(world_, navigation_settings(project));
    input::install(world_, input_map(project), input_settings(project));
    audio::set_settings(world_, audio_settings(project));
    services::install(world_, services_settings(project));
}

std::optional<gameplay::Load> Session::poll_gameplay() {
    if (options_.gameplay == nullptr || !options_.gameplay->changed()) {
        return std::nullopt;
    }
    std::optional<SavedWorld> before;
    if (wants("command")) {
        before = save();
    }
    std::optional<gameplay::Load> load = options_.gameplay->poll();
    if (!load) {
        return load;
    }
    ++revision_;
    publish({"gameplay.reloaded", "", load->to_json(), world_.tick()});
    if (before) {
        publish_changes("gameplay.reload", *before, save());
    }
    return load;
}

void Session::poll_assets() {
    SLASH_PROFILE_ZONE("Session::poll_assets");
    poll_project();
    (void)poll_gameplay();
    std::vector<asset::Assets::Change> changes = assets_.poll();
    if (changes.empty()) {
        return;
    }
    ++revision_;
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
    if (!reload.changed.empty() && call_listener_) {
        // Replays keep what a reload did to the world, since the files may change again.
        const CallListener listener = call_listener_;
        listener("asset.reload", json::Value(json::Object{}), world_.tick());
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
