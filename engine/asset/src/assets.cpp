#include "slash/asset/assets.hpp"

#include "slash/core/json.hpp"
#include "slash/core/text.hpp"

#include <algorithm>
#include <utility>

namespace slash::asset {
namespace {

std::string quoted(std::string_view text) {
    return "\"" + std::string(text) + "\"";
}

} // namespace

FileSource folder_source(std::string folder) {
    if (!folder.empty() && folder.back() != '/' && folder.back() != '\\') {
        folder += '/';
    }
    return {
        .read =
            [folder](std::string_view path) {
                return platform::read_file(folder + std::string(path));
            },
        .stamp =
            [folder](std::string_view path) {
                return platform::file_stamp(folder + std::string(path));
            },
    };
}

Assets::Assets(FileSource source) : source_(std::move(source)) {
    add_type<json::Value>(
        "json", {".json"},
        [](std::string_view text, std::string_view /*path*/) { return json::parse(text); },
        [](const json::Value& value) { return sizeof(json::Value) + json::heap_bytes(value); });
    add_type<std::string>(
        "text", {".txt", ".csv"},
        [](std::string_view text, std::string_view /*path*/) -> Result<std::string> {
            return std::string(text);
        },
        [](const std::string& text) { return sizeof(std::string) + json::heap_bytes(text); });
}

void Assets::add_loader(std::string name, entt::id_type id, std::vector<std::string> suffixes,
                        detail::Loader load) {
    Type type{std::move(name), id, std::move(suffixes), std::move(load)};
    const auto same = std::ranges::find(types_, type.name, &Type::name);
    if (same != types_.end()) {
        *same = std::move(type);
    } else {
        types_.push_back(std::move(type));
    }
}

const Assets::Type* Assets::type_for(std::string_view path) const {
    const Type* best = nullptr;
    std::size_t best_length = 0;
    for (const Type& type : types_) {
        for (const std::string& suffix : type.suffixes) {
            if (path.ends_with(suffix) && path.size() > suffix.size() &&
                suffix.size() > best_length) {
                best = &type;
                best_length = suffix.size();
            }
        }
    }
    return best;
}

Result<detail::Loaded> Assets::run_loader(const Type& type, std::string_view text,
                                          std::string_view path) {
    loading_.emplace_back(path);
    auto loaded = type.load(text, path);
    loading_.pop_back();
    return loaded;
}

Result<std::shared_ptr<detail::Record>> Assets::load_record(std::string_view path,
                                                            entt::id_type id) {
    const std::string_view file = file_of(path);
    if (!is_project_path(file)) {
        return make_error(ErrorCode::InvalidArgument,
                          "asset paths are relative to the project folder and separated by '/', "
                          "with no \".\" or \"..\" parts; got " +
                              quoted(path));
    }
    if (file.size() != path.size() && subresource_of(path).empty()) {
        return make_error(ErrorCode::InvalidArgument, quoted(path) + " names nothing after \"::\"");
    }
    const Type* type = type_for(file);
    if (type == nullptr) {
        std::string known;
        for (const Type& candidate : types_) {
            for (const std::string& suffix : candidate.suffixes) {
                known += (known.empty() ? "" : ", ") + suffix;
            }
        }
        return make_error(ErrorCode::InvalidArgument,
                          "no asset type loads " + quoted(path) + "; known suffixes: " + known);
    }
    if (type->id != id) {
        return make_error(ErrorCode::InvalidArgument,
                          quoted(path) + " is a " + type->name + " asset, not the type asked for");
    }
    if (const auto it = loaded_.find(path); it != loaded_.end()) {
        if (auto record = it->second.lock(); record != nullptr) {
            return record;
        }
    }
    // The stamp comes first, so an edit made while reading still counts as a change.
    const std::optional<FileStamp> stamp = source_.stamp(file);
    auto text = source_.read(file);
    if (!text) {
        return make_error(ErrorCode::NotFound,
                          "asset " + quoted(path) + " could not be read: " + text.error().message);
    }
    auto loaded = run_loader(*type, text.value(), path);
    if (!loaded) {
        return make_error(loaded.error().code,
                          "asset " + quoted(path) + " did not load: " + loaded.error().message);
    }
    auto record = std::make_shared<detail::Record>(detail::Record{
        .path = std::string(path),
        .type = type->name,
        .value = std::move(loaded.value().value),
        .version = 1,
        .stamp = stamp,
        .bytes = loaded.value().bytes,
    });
    loaded_[std::string(path)] = record;
    return record;
}

Result<std::string> Assets::read(std::string_view path, std::string_view type) {
    if (!is_project_path(path)) {
        return make_error(ErrorCode::InvalidArgument,
                          "file paths are relative to the project folder and separated by '/', "
                          "with no \".\" or \"..\" parts; got " +
                              quoted(path));
    }
    const std::optional<FileStamp> stamp = source_.stamp(path);
    auto text = source_.read(path);
    if (!text) {
        return text;
    }
    // A file read again keeps its first stamp, so poll() still reports an edit made since.
    auto it = watched_.find(path);
    if (it == watched_.end()) {
        Watched watched;
        watched.type = std::string(type);
        watched.stamp = stamp;
        it = watched_.emplace(std::string(path), std::move(watched)).first;
    }
    if (!loading_.empty() && loading_.back() != path) {
        it->second.readers.insert(loading_.back());
    }
    return text;
}

std::vector<Assets::Change> Assets::poll() {
    std::vector<Change> changes;
    // Files assets read first, so the assets reload with them.
    for (auto& [path, watched] : watched_) {
        const std::optional<FileStamp> stamp = source_.stamp(path);
        if (stamp == watched.stamp && !watched.changed) {
            continue;
        }
        watched.stamp = stamp;
        watched.changed = false;
        for (const std::string& reader : watched.readers) {
            mark_changed(reader);
        }
        changes.push_back({path, watched.type, ++watched.version,
                           stamp ? std::string() : std::string("the file is missing")});
    }
    for (auto it = loaded_.begin(); it != loaded_.end();) {
        const std::shared_ptr<detail::Record> record = it->second.lock();
        if (record == nullptr) {
            it = loaded_.erase(it);
            continue;
        }
        ++it;
        const std::string_view file = file_of(record->path);
        const std::optional<FileStamp> stamp = source_.stamp(file);
        if (stamp == record->stamp && !record->changed) {
            continue;
        }
        record->stamp = stamp;
        record->changed = false;
        Change change{record->path, record->type, record->version, {}};
        if (!stamp) {
            change.error = "the file is missing";
        } else if (auto text = source_.read(file); !text) {
            change.error = text.error().message;
        } else if (auto loaded = run_loader(*type_for(file), text.value(), record->path); !loaded) {
            change.error = loaded.error().message;
        } else {
            record->value = std::move(loaded.value().value);
            record->bytes = loaded.value().bytes;
            change.version = ++record->version;
        }
        changes.push_back(std::move(change));
    }
    std::ranges::sort(changes, {}, &Change::path);
    return changes;
}

void Assets::mark_changed(std::string_view path) {
    // The file's assets, and the forms of it others name after "::".
    for (const auto& [loaded_path, weak] : loaded_) {
        if (loaded_path != path && file_of(loaded_path) != path) {
            continue;
        }
        if (const std::shared_ptr<detail::Record> record = weak.lock(); record != nullptr) {
            record->changed = true;
        }
    }
    if (const auto it = watched_.find(path); it != watched_.end()) {
        it->second.changed = true;
    }
}

std::vector<Assets::Info> Assets::list() const {
    std::vector<Info> infos;
    for (const auto& [path, weak] : loaded_) {
        if (const std::shared_ptr<detail::Record> record = weak.lock(); record != nullptr) {
            // The lock above holds one reference of its own.
            infos.push_back({path, record->type, static_cast<std::size_t>(record.use_count() - 1),
                             record->version, record->bytes});
        }
    }
    for (const auto& [path, watched] : watched_) {
        infos.push_back({path, watched.type, 0, watched.version, 0});
    }
    std::ranges::sort(infos, {}, &Info::path);
    return infos;
}

} // namespace slash::asset
