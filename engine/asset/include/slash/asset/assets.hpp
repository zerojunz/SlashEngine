#pragma once

#include "slash/core/result.hpp"
#include "slash/platform/file.hpp"

#include <cstddef>
#include <cstdint>
#include <entt/core/type_info.hpp>
#include <functional>
#include <map>
#include <memory>
#include <optional>
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
using Loader = std::function<Result<Value>(std::string_view text, std::string_view path)>;

struct Record {
    std::string path;
    std::string type;
    Value value;
    std::uint64_t version = 1;
    std::optional<FileStamp> stamp;
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
    // name again replaces that type, so modules that share one can each register it.
    template <typename T>
    void add_type(std::string name, std::vector<std::string> suffixes,
                  std::function<Result<T>(std::string_view text, std::string_view path)> load) {
        detail::Loader loader =
            [load = std::move(load)](std::string_view text,
                                     std::string_view path) -> Result<detail::Value> {
            auto loaded = load(text, path);
            if (!loaded) {
                return loaded.error();
            }
            return detail::Value(std::make_shared<const T>(std::move(loaded).value()));
        };
        const entt::id_type id = entt::type_hash<T>::value();
        add_loader(std::move(name), id, std::move(suffixes), std::move(loader));
    }

    // The asset at a project path, loaded on first use and shared by every handle after that.
    // T must be the type its suffix loads as.
    template <typename T>
    [[nodiscard]] Result<Handle<T>> load(std::string_view path) {
        auto record = load_record(path, entt::type_hash<T>::value());
        if (!record) {
            return record.error();
        }
        return Handle<T>(std::move(record).value());
    }

    // Reads a file that other code parses, such as a prefab, and watches it so poll() reports
    // edits. type names it in list().
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

    struct Info {
        std::string path;
        std::string type;
        std::size_t refs = 0; // live handles; 0 for watched files
        std::uint64_t version = 0;
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
    };

    void add_loader(std::string name, entt::id_type id, std::vector<std::string> suffixes,
                    detail::Loader load);
    [[nodiscard]] const Type* type_for(std::string_view path) const;
    Result<std::shared_ptr<detail::Record>> load_record(std::string_view path, entt::id_type id);

    FileSource source_;
    std::vector<Type> types_;
    std::map<std::string, std::weak_ptr<detail::Record>, std::less<>> loaded_;
    std::map<std::string, Watched, std::less<>> watched_;
};

} // namespace slash::asset
