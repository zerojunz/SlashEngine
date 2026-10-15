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
    add_type<json::Value>("json", {".json"}, [](std::string_view text, std::string_view /*path*/) {
        return json::parse(text);
    });
    add_type<std::string>(
        "text", {".txt", ".csv"},
        [](std::string_view text, std::string_view /*path*/) -> Result<std::string> {
            return std::string(text);
        });
}

void Assets::add_loader(std::string name, entt::id_type id, std::vector<std::string> suffixes,
                        detail::Loader load) {
    types_.push_back({std::move(name), id, std::move(suffixes), std::move(load)});
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

Result<std::shared_ptr<detail::Record>> Assets::load_record(std::string_view path,
                                                            entt::id_type id) {
    if (!is_project_path(path)) {
        return make_error(ErrorCode::InvalidArgument,
                          "asset paths are relative to the project folder and separated by '/', "
                          "with no \".\" or \"..\" parts; got " +
                              quoted(path));
    }
    const Type* type = type_for(path);
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
    const std::optional<FileStamp> stamp = source_.stamp(path);
    auto text = source_.read(path);
    if (!text) {
        return make_error(ErrorCode::NotFound,
                          "asset " + quoted(path) + " could not be read: " + text.error().message);
    }
    auto value = type->load(text.value(), path);
    if (!value) {
        return make_error(value.error().code,
                          "asset " + quoted(path) + " did not load: " + value.error().message);
    }
    auto record = std::make_shared<detail::Record>(detail::Record{
        .path = std::string(path),
        .type = type->name,
        .value = std::move(value).value(),
        .version = 1,
        .stamp = stamp,
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
    if (!watched_.contains(path)) {
        watched_.emplace(std::string(path), Watched{std::string(type), stamp, 1});
    }
    return text;
}

std::vector<Assets::Change> Assets::poll() {
    std::vector<Change> changes;
    for (auto it = loaded_.begin(); it != loaded_.end();) {
        const std::shared_ptr<detail::Record> record = it->second.lock();
        if (record == nullptr) {
            it = loaded_.erase(it);
            continue;
        }
        ++it;
        const std::optional<FileStamp> stamp = source_.stamp(record->path);
        if (stamp == record->stamp) {
            continue;
        }
        record->stamp = stamp;
        Change change{record->path, record->type, record->version, {}};
        if (!stamp) {
            change.error = "the file is missing";
        } else if (auto text = source_.read(record->path); !text) {
            change.error = text.error().message;
        } else if (auto value = type_for(record->path)->load(text.value(), record->path); !value) {
            change.error = value.error().message;
        } else {
            record->value = std::move(value).value();
            change.version = ++record->version;
        }
        changes.push_back(std::move(change));
    }
    for (auto& [path, watched] : watched_) {
        const std::optional<FileStamp> stamp = source_.stamp(path);
        if (stamp == watched.stamp) {
            continue;
        }
        watched.stamp = stamp;
        changes.push_back({path, watched.type, ++watched.version,
                           stamp ? std::string() : std::string("the file is missing")});
    }
    std::ranges::sort(changes, {}, &Change::path);
    return changes;
}

std::vector<Assets::Info> Assets::list() const {
    std::vector<Info> infos;
    for (const auto& [path, weak] : loaded_) {
        if (const std::shared_ptr<detail::Record> record = weak.lock(); record != nullptr) {
            // The lock above holds one reference of its own.
            infos.push_back({path, record->type, static_cast<std::size_t>(record.use_count() - 1),
                             record->version});
        }
    }
    for (const auto& [path, watched] : watched_) {
        infos.push_back({path, watched.type, 0, watched.version});
    }
    std::ranges::sort(infos, {}, &Info::path);
    return infos;
}

} // namespace slash::asset
