#include "checkpoints.hpp"

#include "slash/canvas/canvas.hpp"
#include "slash/command/session.hpp"
#include "slash/command/work.hpp"
#include "slash/core/log.hpp"
#include "slash/platform/file.hpp"
#include "slash/render/image.hpp"
#include "slash/ui/ui.hpp"
#include "slash/world/scene.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Checkpoints: .slash/work/checkpoints/<session>/
//   base/manifest.json   {"files": {"<path>": "b0" or null}}: each file as it was before the
//                        session first wrote it, or null where there was none
//   base/b0, b1, ...     those contents
//   <n>/checkpoint.json  Checkpoint::to_json, plus "stored": {"<path>": "f0", ...}
//   <n>/f0, f1, ...      the files written since checkpoint n - 1, as they were at n
//   <n>/world.scene.json the world, as scene.get returns it
//   <n>/frame.png        the world drawn on the CPU
// A file's contents at checkpoint n are those of the latest checkpoint up to n that holds it, or
// the base's.
namespace slash::command::work {
namespace {

constexpr std::string_view kBase = "base";
constexpr std::string_view kManifest = "manifest.json";
constexpr std::string_view kCheckpointFile = "checkpoint.json";
constexpr std::string_view kWorldFile = "world.scene.json";
constexpr std::string_view kFrameFile = "frame.png";
// A frame's width at most, in pixels: enough for a timeline.
constexpr double kFrameWidth = 480.0;

std::string join(std::string_view root, std::string_view relative) {
    std::string path(root);
    if (!path.empty() && path.back() != '/' && path.back() != '\\') {
        path += '/';
    }
    path += relative;
    return path;
}

std::string session_folder(std::string_view root, std::string_view session) {
    return join(join(root, kCheckpointsFolder), session);
}

std::string checkpoint_folder(std::string_view root, std::string_view session, std::int64_t n) {
    return join(session_folder(root, session), std::to_string(n));
}

// A path a checkpoint may hold: inside the project and outside .slash/.
bool keepable(std::string_view path) {
    if (path.empty() || path.front() == '/' || path.find(':') != std::string_view::npos ||
        path.find('\\') != std::string_view::npos || path.starts_with(".slash/")) {
        return false;
    }
    std::size_t at = 0;
    while (at <= path.size()) {
        const std::size_t end = std::min(path.find('/', at), path.size());
        const std::string_view part = path.substr(at, end - at);
        if (part.empty() || part == "." || part == "..") {
            return false;
        }
        at = end + 1;
    }
    return true;
}

Result<void> write_making_folders(const std::string& full, std::string_view contents) {
    if (const std::size_t slash = full.find_last_of('/'); slash != std::string::npos && slash > 0) {
        if (const std::string folder = full.substr(0, slash); !platform::exists(folder)) {
            if (auto made = platform::create_directories(folder); !made) {
                return made.error();
            }
        }
    }
    return platform::write_file(full, contents);
}

std::optional<json::Value> read_json(const std::string& full) {
    auto text = platform::read_file(full);
    if (!text) {
        return std::nullopt;
    }
    auto parsed = json::parse(text.value());
    if (!parsed) {
        return std::nullopt;
    }
    return std::move(parsed).value();
}

// A checkpoint as stored, with where it keeps each file.
struct Stored {
    Checkpoint checkpoint;
    json::Value stored = json::Object{}; // NOLINT(readability-redundant-member-init)
};

std::vector<Stored> read_all(std::string_view root, std::string_view session) {
    std::vector<Stored> all;
    auto files = platform::list_files(session_folder(root, session));
    if (!files) {
        return all;
    }
    for (const std::string& name : files.value()) {
        const std::size_t slash = name.find('/');
        if (slash == std::string::npos || name.substr(slash + 1) != kCheckpointFile) {
            continue;
        }
        const auto value = read_json(join(session_folder(root, session), name));
        if (!value) {
            continue;
        }
        auto checkpoint = Checkpoint::from_json(*value);
        if (!checkpoint || std::to_string(checkpoint->n) != name.substr(0, slash)) {
            continue;
        }
        Stored entry{.checkpoint = std::move(*checkpoint)};
        if (const json::Value* stored = value->find("stored");
            stored != nullptr && stored->is_object()) {
            entry.stored = *stored;
        }
        all.push_back(std::move(entry));
    }
    std::ranges::sort(all, {}, [](const Stored& entry) { return entry.checkpoint.n; });
    return all;
}

json::Value read_base(std::string_view root, std::string_view session) {
    const auto manifest = read_json(join(join(session_folder(root, session), kBase), kManifest));
    if (manifest) {
        if (const json::Value* files = manifest->find("files");
            files != nullptr && files->is_object()) {
            return *files;
        }
    }
    return json::Object{};
}

// What a file held at a checkpoint, or nullopt where there was none. `base` says so for files
// no checkpoint up to it holds.
std::optional<std::string> state_at(std::string_view root, std::string_view session,
                                    const std::vector<Stored>& all, const json::Value& base,
                                    const std::string& path, std::int64_t n) {
    for (const Stored& stored : std::views::reverse(all)) {
        if (stored.checkpoint.n > n) {
            continue;
        }
        if (const json::Value* file = stored.stored.find(path);
            file != nullptr && file->is_string()) {
            auto text = platform::read_file(
                join(checkpoint_folder(root, session, stored.checkpoint.n), file->as_string()));
            return text ? std::optional<std::string>(std::move(text).value()) : std::nullopt;
        }
        if (std::ranges::find(stored.checkpoint.removed, path) != stored.checkpoint.removed.end()) {
            return std::nullopt;
        }
    }
    if (const json::Value* file = base.find(path); file != nullptr && file->is_string()) {
        auto text = platform::read_file(
            join(join(session_folder(root, session), kBase), file->as_string()));
        return text ? std::optional<std::string>(std::move(text).value()) : std::nullopt;
    }
    if (base.find(path) != nullptr) {
        return std::nullopt;
    }
    // Never written by the session: as it is now.
    auto text = platform::read_file(join(root, path));
    return text ? std::optional<std::string>(std::move(text).value()) : std::nullopt;
}

// Every path the session's checkpoints know of.
std::set<std::string, std::less<>> known_paths(const std::vector<Stored>& all,
                                               const json::Value& base) {
    std::set<std::string, std::less<>> paths;
    for (const auto& [path, file] : base.as_object()) {
        paths.insert(path);
    }
    for (const Stored& entry : all) {
        paths.insert(entry.checkpoint.files.begin(), entry.checkpoint.files.end());
        paths.insert(entry.checkpoint.removed.begin(), entry.checkpoint.removed.end());
    }
    return paths;
}

const Stored* find_checkpoint(const std::vector<Stored>& all, std::int64_t n) {
    const auto found =
        std::ranges::find_if(all, [&](const Stored& entry) { return entry.checkpoint.n == n; });
    return found == all.end() ? nullptr : &*found;
}

Error no_checkpoint(std::string_view session, std::int64_t n) {
    return make_error(ErrorCode::NotFound, "session " + std::string(session) +
                                               " has no checkpoint " + std::to_string(n));
}

std::vector<std::string_view> lines_of(std::string_view text) {
    std::vector<std::string_view> lines;
    std::size_t at = 0;
    while (at < text.size()) {
        const std::size_t end = std::min(text.find('\n', at), text.size());
        lines.push_back(text.substr(at, end - at));
        at = end + 1;
    }
    return lines;
}

// Writes a checkpoint's files from (path, contents or nullopt) pairs.
Result<Checkpoint>
write_checkpoint(std::string_view root, std::string_view session, Checkpoint checkpoint,
                 const std::vector<std::pair<std::string, std::optional<std::string>>>& files,
                 const json::Value* world, std::string_view png) {
    const std::string folder = checkpoint_folder(root, session, checkpoint.n);
    if (auto made = platform::create_directories(folder); !made) {
        return made.error();
    }
    json::Value stored = json::Object{};
    for (const auto& [path, contents] : files) {
        if (!contents) {
            checkpoint.removed.push_back(path);
            continue;
        }
        const std::string name = "f" + std::to_string(checkpoint.files.size());
        if (auto written = platform::write_file(join(folder, name), *contents); !written) {
            return written.error();
        }
        stored[path] = name;
        checkpoint.files.push_back(path);
    }
    if (world != nullptr) {
        if (auto written = platform::write_file(join(folder, kWorldFile), json::write(*world));
            !written) {
            return written.error();
        }
    }
    if (!png.empty()) {
        if (auto written = platform::write_file(join(folder, kFrameFile), png); !written) {
            return written.error();
        }
        checkpoint.frame = true;
    }
    json::Value manifest = checkpoint.to_json();
    manifest["stored"] = std::move(stored);
    if (auto written = platform::write_file(join(folder, kCheckpointFile), json::write(manifest));
        !written) {
        return written.error();
    }
    return checkpoint;
}

} // namespace

json::Value Checkpoint::to_json() const {
    json::Value value;
    value["n"] = n;
    value["time"] = time;
    value["reason"] = reason;
    if (!label.empty()) {
        value["label"] = label;
    }
    value["line"] = line;
    json::Array written(files.begin(), files.end());
    value["files"] = std::move(written);
    if (!removed.empty()) {
        json::Array gone(removed.begin(), removed.end());
        value["removed"] = std::move(gone);
    }
    value["frame"] = frame;
    return value;
}

std::optional<Checkpoint> Checkpoint::from_json(const json::Value& value) {
    const json::Value* n = value.find("n");
    const json::Value* time = value.find("time");
    const json::Value* reason = value.find("reason");
    if (n == nullptr || !n->is_int() || time == nullptr || !time->is_string() ||
        reason == nullptr || !reason->is_string()) {
        return std::nullopt;
    }
    Checkpoint checkpoint;
    checkpoint.n = n->as_int();
    checkpoint.time = time->as_string();
    checkpoint.reason = reason->as_string();
    if (const json::Value* label = value.find("label"); label != nullptr && label->is_string()) {
        checkpoint.label = label->as_string();
    }
    if (const json::Value* line = value.find("line"); line != nullptr && line->is_int()) {
        checkpoint.line = line->as_int();
    }
    for (const auto& [key, list] :
         {std::pair{"files", &checkpoint.files}, std::pair{"removed", &checkpoint.removed}}) {
        if (const json::Value* paths = value.find(key); paths != nullptr && paths->is_array()) {
            for (const json::Value& path : paths->as_array()) {
                if (path.is_string() && keepable(path.as_string())) {
                    list->push_back(path.as_string());
                }
            }
        }
    }
    if (const json::Value* frame = value.find("frame"); frame != nullptr && frame->is_bool()) {
        checkpoint.frame = frame->as_bool();
    }
    return checkpoint;
}

void Journal::before_write(std::string_view path) {
    if (!options_.checkpoints || full_path_.empty() || !keepable(path)) {
        return;
    }
    if (std::ranges::find(written_, path) == written_.end()) {
        written_.emplace_back(path);
    }
    if (kept_.find(path) != nullptr) {
        return;
    }
    const std::string base = join(session_folder(options_.project_root, id_), kBase);
    auto contents = platform::read_file(join(options_.project_root, path));
    if (contents) {
        const std::string name = "b" + std::to_string(kept_.as_object().size());
        if (auto written = write_making_folders(join(base, name), contents.value()); !written) {
            log::warn("a checkpoint can't keep a file as it was",
                      {{"path", std::string(path)}, {"detail", written.error().message}});
            return;
        }
        kept_[path] = name;
    } else {
        kept_[path] = json::Value{};
    }
    json::Value manifest;
    manifest["files"] = kept_;
    if (auto written = write_making_folders(join(base, kManifest), json::write(manifest));
        !written) {
        log::warn("a checkpoint can't keep a file as it was",
                  {{"path", std::string(path)}, {"detail", written.error().message}});
    }
}

Result<Checkpoint> Journal::checkpoint(std::string_view reason, std::string_view label,
                                       const json::Value& world, std::string_view png) {
    if (!options_.checkpoints) {
        return make_error(ErrorCode::InvalidArgument,
                          "project.json's work block turns checkpoints off");
    }
    if (full_path_.empty()) {
        return make_error(ErrorCode::PlatformFailure, "the work journal stopped");
    }
    flush();
    if (checkpoints_ == 0) {
        for (const Checkpoint& before : list_checkpoints(options_.project_root, id_)) {
            checkpoints_ = std::max(checkpoints_, before.n);
        }
    }
    Checkpoint checkpoint;
    checkpoint.n = checkpoints_ + 1;
    checkpoint.time = format_time(now());
    checkpoint.reason = reason;
    checkpoint.label = label;
    checkpoint.line = static_cast<std::int64_t>(sequence_);
    std::vector<std::pair<std::string, std::optional<std::string>>> files;
    for (const std::string& path : written_) {
        auto contents = platform::read_file(join(options_.project_root, path));
        files.emplace_back(path, contents ? std::optional<std::string>(std::move(contents).value())
                                          : std::nullopt);
    }
    auto written =
        write_checkpoint(options_.project_root, id_, std::move(checkpoint), files, &world, png);
    if (!written) {
        return written.error();
    }
    checkpoints_ = written.value().n;
    written_.clear();
    json::Value line;
    line["checkpoint"] = written.value().n;
    line["reason"] = written.value().reason;
    if (!written.value().label.empty()) {
        line["label"] = written.value().label;
    }
    if (!written.value().files.empty() || !written.value().removed.empty()) {
        json::Array paths(written.value().files.begin(), written.value().files.end());
        paths.insert(paths.end(), written.value().removed.begin(), written.value().removed.end());
        line["files"] = std::move(paths);
    }
    write(std::move(line));
    return written;
}

std::vector<Checkpoint> list_checkpoints(std::string_view project_root, std::string_view session) {
    std::vector<Checkpoint> list;
    for (Stored& entry : read_all(project_root, session)) {
        list.push_back(std::move(entry.checkpoint));
    }
    return list;
}

Result<std::string> checkpoint_frame(std::string_view project_root, std::string_view session,
                                     std::int64_t n) {
    return platform::read_file(join(checkpoint_folder(project_root, session, n), kFrameFile));
}

Result<json::Value> compare_checkpoints(std::string_view project_root, std::string_view session,
                                        std::int64_t from, std::int64_t to,
                                        const world::ComponentRegistry& components) {
    const std::vector<Stored> all = read_all(project_root, session);
    for (const std::int64_t n : {from, to}) {
        if (find_checkpoint(all, n) == nullptr) {
            return no_checkpoint(session, n);
        }
    }
    const json::Value base = read_base(project_root, session);
    json::Array added;
    json::Array removed;
    json::Array changed;
    for (const std::string& path : known_paths(all, base)) {
        const auto before = state_at(project_root, session, all, base, path, from);
        const auto after = state_at(project_root, session, all, base, path, to);
        if (before == after) {
            continue;
        }
        if (!before) {
            added.emplace_back(path);
        } else if (!after) {
            removed.emplace_back(path);
        } else {
            // Lines in one and not the other, counted as a multiset.
            std::map<std::string_view, std::int64_t> count;
            for (const std::string_view line : lines_of(*before)) {
                --count[line];
            }
            for (const std::string_view line : lines_of(*after)) {
                ++count[line];
            }
            std::int64_t plus = 0;
            std::int64_t minus = 0;
            for (const auto& [line, n] : count) {
                (n > 0 ? plus : minus) += n > 0 ? n : -n;
            }
            json::Value entry;
            entry["path"] = path;
            entry["lines"] = "+" + std::to_string(plus) + " -" + std::to_string(minus);
            changed.push_back(std::move(entry));
        }
    }
    json::Value result;
    for (const auto& [key, n] : {std::pair{"from", from}, std::pair{"to", to}}) {
        json::Value side = find_checkpoint(all, n)->checkpoint.to_json();
        side.as_object().erase("files");
        side.as_object().erase("removed");
        result[key] = std::move(side);
    }
    json::Value files = json::Object{};
    if (!added.empty()) {
        files["added"] = std::move(added);
    }
    if (!removed.empty()) {
        files["removed"] = std::move(removed);
    }
    if (!changed.empty()) {
        files["changed"] = std::move(changed);
    }
    result["files"] = std::move(files);
    const auto world_of = [&](std::int64_t n) {
        return read_json(join(checkpoint_folder(project_root, session, n), kWorldFile));
    };
    const auto before = world_of(from);
    const auto after = world_of(to);
    if (before && after) {
        result["entities"] = world::diff_scenes(components, *before, *after);
    }
    return result;
}

Result<json::Value> restore_checkpoint(std::string_view project_root, std::string_view session,
                                       std::int64_t n) {
    if (const std::vector<SessionEntry> live = live_sessions(project_root); !live.empty()) {
        return make_error(ErrorCode::Conflict,
                          "session " + live.back().id + " (" + live.back().host +
                              ") is running; restore once no agent is working in the project");
    }
    const std::vector<Stored> all = read_all(project_root, session);
    if (find_checkpoint(all, n) == nullptr) {
        return no_checkpoint(session, n);
    }
    const json::Value base = read_base(project_root, session);
    const std::set<std::string, std::less<>> paths = known_paths(all, base);

    // What is there now, kept as a checkpoint of its own first.
    std::vector<std::pair<std::string, std::optional<std::string>>> now;
    for (const std::string& path : paths) {
        auto contents = platform::read_file(join(project_root, path));
        now.emplace_back(path, contents ? std::optional<std::string>(std::move(contents).value())
                                        : std::nullopt);
    }
    Checkpoint kept;
    kept.n = all.back().checkpoint.n + 1;
    kept.time = format_time(std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count());
    kept.reason = "restore";
    kept.label = "before restoring checkpoint " + std::to_string(n);
    kept.line = all.back().checkpoint.line;
    auto written = write_checkpoint(project_root, session, std::move(kept), now, nullptr, "");
    if (!written) {
        return written.error();
    }

    json::Array changed;
    for (const auto& [path, contents] : now) {
        const auto then = state_at(project_root, session, all, base, path, n);
        if (then == contents) {
            continue;
        }
        const std::string full = join(project_root, path);
        if (then) {
            if (auto put = write_making_folders(full, *then); !put) {
                return put.error();
            }
        } else if (auto gone = platform::remove_file(full); !gone) {
            return gone.error();
        }
        changed.emplace_back(path);
    }
    json::Value result;
    result["restored"] = n;
    result["files"] = std::move(changed);
    result["kept"] = written.value().n;
    if (platform::exists(join(checkpoint_folder(project_root, session, n), kWorldFile))) {
        result["world"] = std::string(kCheckpointsFolder) + "/" + std::string(session) + "/" +
                          std::to_string(n) + "/" + std::string(kWorldFile);
    }
    return result;
}

Result<Checkpoint> take_checkpoint(Session& session, std::string_view reason,
                                   std::string_view label) {
    Journal* journal = session.options().journal;
    if (journal == nullptr) {
        return make_error(ErrorCode::InvalidArgument, "this session keeps no work journal");
    }
    if (session.watching()) {
        return make_error(ErrorCode::PermissionDenied, "watchers can't take checkpoints");
    }
    const json::Value world = world::save_scene(session.world());
    std::string png;
    const Vec2 viewport = ui::settings(session.world()).viewport;
    const double scale = viewport.x > kFrameWidth ? kFrameWidth / viewport.x : 1.0;
    if (auto captured = canvas::capture(session.world(), session.assets(),
                                        {.scale = static_cast<float>(scale)});
        captured) {
        if (auto encoded = render::encode_png(captured.value().image); encoded) {
            png = std::move(encoded).value();
        }
    }
    return journal->checkpoint(reason, label, world, png);
}

std::uint64_t checkpoint_bytes(std::string_view project_root, std::string_view session) {
    std::uint64_t bytes = 0;
    const std::string folder = session_folder(project_root, session);
    if (auto files = platform::list_files(folder); files) {
        for (const std::string& name : files.value()) {
            if (const auto stamp = platform::file_stamp(join(folder, name)); stamp) {
                bytes += stamp->size;
            }
        }
    }
    return bytes;
}

void remove_checkpoints(std::string_view project_root, std::string_view session) {
    const std::string folder = session_folder(project_root, session);
    auto files = platform::list_files(folder);
    if (!files) {
        return;
    }
    std::set<std::string, std::greater<>> folders;
    for (const std::string& name : files.value()) {
        (void)platform::remove_file(join(folder, name));
        for (std::size_t slash = name.rfind('/'); slash != std::string::npos && slash > 0;
             slash = name.rfind('/', slash - 1)) {
            folders.insert(name.substr(0, slash));
        }
    }
    // Deepest first, so each is empty when removed.
    for (const std::string& name : folders) {
        (void)platform::remove_file(join(folder, name));
    }
    (void)platform::remove_file(folder);
}

} // namespace slash::command::work
