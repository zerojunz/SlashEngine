#include "slash/platform/save_data.hpp"

#include "slash/platform/file.hpp"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_stdinc.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <ctime>
#include <mutex>
#include <utility>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

// Mounts the browser's IndexedDB over the save folder and reads it back, then tells the engine
// saves are ready (slash_save_data_read).
// NOLINTBEGIN
// clang-format off
EM_JS_DEPS(slash_save_data, "$UTF8ToString,$FS,$IDBFS");
EM_JS(void, slash_save_data_mount, (const char* path), {
    var folder = UTF8ToString(path);
    if (folder.endsWith('/')) {
        folder = folder.slice(0, -1);
    }
    try {
        FS.mkdirTree(folder);
        FS.mount(IDBFS, {}, folder);
    } catch (error) {
    }
    FS.syncfs(true, function(error) { _slash_save_data_read(); });
});
// clang-format on
// NOLINTEND
#endif

namespace slash::platform {
namespace {

constexpr std::string_view kExtension = ".save.json";
constexpr std::size_t kLongestSlot = 64;

struct Process {
    std::mutex mutex;
    SaveStore store;
};

Process& process() {
    static Process instance;
    return instance;
}

// Whether the web's stored saves are in; elsewhere they always are.
std::atomic<bool>& ready() {
    static std::atomic<bool> instance{true};
    return instance;
}

std::string with_separator(std::string path) {
    if (!path.empty() && path.back() != '/' && path.back() != '\\') {
        path += '/';
    }
    return path;
}

// Now, in UTC, as "2026-11-26T20:15:00Z".
std::string utc_now() {
    const std::time_t now = std::time(nullptr);
    std::tm parts{};
#ifdef _WIN32
    gmtime_s(&parts, &now);
#else
    gmtime_r(&now, &parts);
#endif
    std::array<char, 32> buffer{};
    const std::size_t n = std::strftime(buffer.data(), buffer.size(), "%Y-%m-%dT%H:%M:%SZ", &parts);
    return {buffer.data(), n};
}

Error bad_slot(std::string_view slot) {
    return make_error(ErrorCode::InvalidArgument,
                      "a save slot's name is 1 to 64 lowercase letters, digits, '_' and '-', not "
                      "\"" +
                          std::string(slot) + "\"");
}

// A slot's file in store, or an error for a name that isn't one or a store with no folder.
Result<std::string> slot_file(const SaveStore& store, std::string_view slot) {
    if (!valid_save_slot(slot)) {
        return bad_slot(slot);
    }
    if (store.folder.empty()) {
        return make_error(ErrorCode::InvalidArgument,
                          "no save folder is set: a game sets it when it starts");
    }
    return with_separator(store.folder) + std::string(slot) + std::string(kExtension);
}

// Writes the save folder back to the browser's storage, on the web.
void keep() {
#ifdef __EMSCRIPTEN__
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): Emscripten's own macro
    EM_ASM({ FS.syncfs(false, function(error){}); });
#endif
}

// A save file's contents, read: an error where it isn't one.
Result<SaveFile> parse_save(std::string_view text, std::string_view slot) {
    auto parsed = json::parse(text);
    if (!parsed) {
        return make_error(ErrorCode::InvalidArgument,
                          "save slot " + std::string(slot) +
                              " holds no save: " + parsed.error().message);
    }
    const json::Value& file = parsed.value();
    const json::Value* format = file.is_object() ? file.find("format") : nullptr;
    const json::Value* data = file.is_object() ? file.find("data") : nullptr;
    if (format == nullptr || !format->is_number() || data == nullptr) {
        return make_error(ErrorCode::InvalidArgument,
                          "save slot " + std::string(slot) + " holds no save");
    }
    if (format->as_double() > static_cast<double>(kSaveFormat)) {
        return make_error(ErrorCode::InvalidArgument,
                          "save slot " + std::string(slot) +
                              " was written in a later save format by a later engine");
    }
    SaveFile out{.game_version = {}, .saved = {}, .data = *data};
    if (const json::Value* version = file.find("game_version");
        version != nullptr && version->is_string()) {
        out.game_version = version->as_string();
    }
    if (const json::Value* saved = file.find("saved"); saved != nullptr && saved->is_string()) {
        out.saved = saved->as_string();
    }
    return out;
}

} // namespace

void set_save_folder(std::string folder, std::string game_version) {
    const std::string path = with_separator(std::move(folder));
    {
        Process& set = process();
        const std::scoped_lock lock(set.mutex);
        set.store = {.folder = path, .game_version = std::move(game_version)};
    }
#ifdef __EMSCRIPTEN__
    // The browser keeps the folder in IndexedDB: mounted over it, then read back, after which
    // saves are ready.
    if (path.empty()) {
        return;
    }
    ready() = false;
    slash_save_data_mount(path.c_str());
#endif
}

SaveStore save_store() {
    Process& set = process();
    const std::scoped_lock lock(set.mutex);
    return set.store;
}

std::string user_save_folder(std::string_view game_name) {
    const std::string name(game_name);
    char* found = SDL_GetPrefPath("", name.c_str());
    if (found == nullptr) {
        return {};
    }
    std::string out = std::string(found) + "saves/";
    SDL_free(found);
    return out;
}

std::string project_save_folder(std::string_view project_folder) {
    return with_separator(std::string(project_folder)) + ".slash/saves/";
}

bool save_data_ready() {
    return ready();
}

bool valid_save_slot(std::string_view slot) {
    return !slot.empty() && slot.size() <= kLongestSlot && std::ranges::all_of(slot, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}

Result<void> save_game(const SaveStore& store, std::string_view slot, const json::Value& data) {
    auto name = slot_file(store, slot);
    if (!name) {
        return name.error();
    }
    if (auto made = create_directories(store.folder); !made) {
        return made;
    }
    json::Value file;
    file["format"] = kSaveFormat;
    file["game_version"] = store.game_version;
    file["saved"] = utc_now();
    file["data"] = data;
    const std::string temporary = name.value() + ".new";
    if (auto written = write_file(temporary, json::write(file) + "\n"); !written) {
        return written;
    }
    if (auto moved = rename_file(temporary, name.value()); !moved) {
        (void)remove_file(temporary);
        return moved;
    }
    keep();
    return {};
}

Result<SaveFile> load_game(const SaveStore& store, std::string_view slot) {
    auto name = slot_file(store, slot);
    if (!name) {
        return name.error();
    }
    if (!exists(name.value())) {
        return make_error(ErrorCode::NotFound, "save slot " + std::string(slot) + " is empty");
    }
    auto text = read_file(name.value());
    if (!text) {
        return text.error();
    }
    return parse_save(text.value(), slot);
}

bool has_save(const SaveStore& store, std::string_view slot) {
    auto name = slot_file(store, slot);
    return name && exists(name.value());
}

Result<void> delete_save(const SaveStore& store, std::string_view slot) {
    auto name = slot_file(store, slot);
    if (!name) {
        return name.error();
    }
    if (auto removed = remove_file(name.value()); !removed) {
        return removed;
    }
    keep();
    return {};
}

std::vector<SaveSlot> save_slots(const SaveStore& store) {
    std::vector<SaveSlot> out;
    if (store.folder.empty() || !exists(store.folder)) {
        return out;
    }
    const std::string path = with_separator(store.folder);
    auto files = list_files(path, [](std::string_view) { return false; });
    if (!files) {
        return out;
    }
    for (const std::string& file : files.value()) {
        if (file.size() <= kExtension.size() || !file.ends_with(kExtension)) {
            continue;
        }
        const std::string slot = file.substr(0, file.size() - kExtension.size());
        if (!valid_save_slot(slot)) {
            continue;
        }
        auto text = read_file(path + file);
        if (!text) {
            continue;
        }
        auto save = parse_save(text.value(), slot);
        if (!save) {
            continue;
        }
        out.push_back({.name = slot,
                       .game_version = std::move(save.value().game_version),
                       .saved = std::move(save.value().saved),
                       .bytes = text.value().size()});
    }
    std::ranges::stable_sort(out, [](const SaveSlot& a, const SaveSlot& b) {
        return a.saved != b.saved ? a.saved > b.saved : a.name < b.name;
    });
    return out;
}

Result<void> save_game(std::string_view slot, const json::Value& data) {
    return save_game(save_store(), slot, data);
}

Result<SaveFile> load_game(std::string_view slot) {
    return load_game(save_store(), slot);
}

bool has_save(std::string_view slot) {
    return has_save(save_store(), slot);
}

Result<void> delete_save(std::string_view slot) {
    return delete_save(save_store(), slot);
}

std::vector<SaveSlot> save_slots() {
    return save_slots(save_store());
}

} // namespace slash::platform

#ifdef __EMSCRIPTEN__
// Called from JavaScript once the browser's stored saves are read back.
extern "C" EMSCRIPTEN_KEEPALIVE void slash_save_data_read() {
    slash::platform::ready() = true;
}
#endif
