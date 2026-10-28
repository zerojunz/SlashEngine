#pragma once

#include "slash/core/result.hpp"
#include "slash/platform/file.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <entt/core/type_info.hpp>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Assets are files a game loads by project path, like Godot's resources: a JSON table, a text
// file and, as the renderer and audio arrive, textures and sounds. Loading one returns a handle;
// every handle to the same path shares one copy, which stays loaded while a handle exists.
//
//   auto tuning = assets.load<json::Value>("data/tuning.json");
//   const double speed = tuning.value()->find("speed")->as_double();
//
// Hot reload: poll() checks each loaded file and reloads the ones that changed, so the game picks
// up an edit without restarting. A handle always gives the latest version.
namespace slash::asset {

// What separates a file from the form of it an asset path names, as Godot's "::".
inline constexpr std::string_view kSubresource = "::";

// The file an asset path reads: the path up to "::".
[[nodiscard]] constexpr std::string_view file_of(std::string_view path) noexcept {
    return path.substr(0, std::min(path.find(kSubresource), path.size()));
}

// What an asset path names after "::", or "".
[[nodiscard]] constexpr std::string_view subresource_of(std::string_view path) noexcept {
    const std::size_t at = path.find(kSubresource);
    return at == std::string_view::npos ? std::string_view()
                                        : path.substr(at + kSubresource.size());
}

using platform::FileStamp;

// Where asset files come from, by project path (slash::is_project_path).
struct FileSource {
    std::function<Result<std::string>(std::string_view path)> read;
    // The file's stamp, or nullopt if it is missing. poll() reloads a file whose stamp changed.
    std::function<std::optional<FileStamp>(std::string_view path)> stamp;
};

// Files in a folder, read through the platform layer.
[[nodiscard]] FileSource folder_source(std::string folder);

namespace detail {

using Value = std::shared_ptr<const void>;

struct Loaded {
    Value value;
    std::size_t bytes = 0; // what it holds in memory
};
using Loader = std::function<Result<Loaded>(std::string_view text, std::string_view path)>;

struct Record {
    std::string path;
    std::string type;
    Value value;
    std::uint64_t version = 1;
    std::optional<FileStamp> stamp;
    std::size_t bytes = 0;
    bool changed = false; // mark_changed: reload on the next poll, whatever the stamp says
};

} // namespace detail

// A shared reference to a loaded asset.
template <typename T>
class Handle {
public:
    Handle() = default;

    // The asset as last loaded. A reload replaces it, so read it through the handle each time
    // rather than keeping the pointer; share() keeps one version alive.
    [[nodiscard]] const T* get() const { return static_cast<const T*>(record_->value.get()); }
    [[nodiscard]] const T& operator*() const { return *get(); }
    [[nodiscard]] const T* operator->() const { return get(); }
    [[nodiscard]] std::shared_ptr<const T> share() const {
        return std::static_pointer_cast<const T>(record_->value);
    }

    [[nodiscard]] const std::string& path() const { return record_->path; }
    // 1 when first loaded, and one more after each reload.
    [[nodiscard]] std::uint64_t version() const { return record_->version; }
    [[nodiscard]] explicit operator bool() const noexcept { return record_ != nullptr; }

private:
    friend class Assets;
    explicit Handle(std::shared_ptr<detail::Record> record) : record_(std::move(record)) {}

    std::shared_ptr<detail::Record> record_;
};

class Assets {
public:
    // Registers the built-in types: "json" (.json files as json::Value) and "text" (.txt and
    // .csv files as std::string).
    explicit Assets(FileSource source);

    // Registers an asset type: files ending in one of the suffixes load with load. The longest
    // matching suffix wins, so ".scene.json" can have its own type beside ".json". Registering a
    // name again replaces that type, so modules that share one can each register it. bytes says
    // what a loaded asset holds in memory, for list() and perf.stats; without it, its file's size
    // stands in.
    template <typename T>
    void add_type(std::string name, std::vector<std::string> suffixes,
                  std::function<Result<T>(std::string_view text, std::string_view path)> load,
                  std::function<std::size_t(const T& asset)> bytes = {}) {
        detail::Loader loader = [load = std::move(load), bytes = std::move(bytes)](
                                    std::string_view text,
                                    std::string_view path) -> Result<detail::Loaded> {
            auto loaded = load(text, path);
            if (!loaded) {
                return loaded.error();
            }
            auto value = std::make_shared<const T>(std::move(loaded).value());
            const std::size_t size = bytes ? bytes(*value) : text.size();
            return detail::Loaded{std::move(value), size};
        };
        const entt::id_type id = entt::type_hash<T>::value();
        add_loader(std::move(name), id, std::move(suffixes), std::move(loader));
    }

    // The asset at a project path, loaded on first use and shared by every handle after that.
    // T must be the type its suffix loads as. A path can name one form of its file after "::",
    // as Godot names a file's sub-resources ("props/crate.model.json::damaged"): the file is read
    // and watched as itself, and the loader gets the whole path.
    template <typename T>
    [[nodiscard]] Result<Handle<T>> load(std::string_view path) {
        auto record = load_record(path, entt::type_hash<T>::value());
        if (!record) {
            return record.error();
        }
        return Handle<T>(std::move(record).value());
    }

    // Reads a file that other code parses, such as a prefab, and watches it so poll() reports
    // edits. type names it in list(). A file read while an asset loads, such as a texture or part
    // a model names, is that asset's too: an edit to it reloads the asset.
    [[nodiscard]] Result<std::string> read(std::string_view path, std::string_view type);

    struct Change {
        std::string path;
        std::string type;
        std::uint64_t version = 0;
        std::string error; // why it did not reload; the asset keeps its last good version
    };
    // Reloads every loaded asset whose file changed since it was read, and returns what changed,
    // watched files included. Assets with no handles left are unloaded first.
    std::vector<Change> poll();
    // Makes the next poll() reload the file, whatever its stamp says: a file written again at
    // the same size within one tick of the file system's clock keeps its stamp. Sessions call it
    // for the files their commands write.
    void mark_changed(std::string_view path);

    struct Info {
        std::string path;
        std::string type;
        std::size_t refs = 0; // live handles; 0 for watched files
        std::uint64_t version = 0;
        // What it holds in memory, as its type counts it (add_type); 0 for watched files.
        std::size_t bytes = 0;
    };
    // Every loaded asset and watched file, by path.
    [[nodiscard]] std::vector<Info> list() const;

private:
    struct Type {
        std::string name;
        entt::id_type id;
        std::vector<std::string> suffixes;
        detail::Loader load;
    };
    struct Watched {
        std::string type;
        std::optional<FileStamp> stamp;
        std::uint64_t version = 1;
        bool changed = false;
        std::set<std::string, std::less<>> readers; // assets that read it as they loaded
    };

    // Loads an asset's file with its type, as the asset that the files read meanwhile belong to.
    [[nodiscard]] Result<detail::Loaded> run_loader(const Type& type, std::string_view text,
                                                    std::string_view path);

    void add_loader(std::string name, entt::id_type id, std::vector<std::string> suffixes,
                    detail::Loader load);
    [[nodiscard]] const Type* type_for(std::string_view path) const;
    Result<std::shared_ptr<detail::Record>> load_record(std::string_view path, entt::id_type id);

    FileSource source_;
    std::vector<Type> types_;
    std::map<std::string, std::weak_ptr<detail::Record>, std::less<>> loaded_;
    std::map<std::string, Watched, std::less<>> watched_;
    std::vector<std::string> loading_; // the assets loading now, innermost last
};

} // namespace slash::asset
