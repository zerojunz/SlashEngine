#pragma once

#include "slash/core/json.hpp"
#include "slash/core/result.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Save data for game code: named slots of JSON, each one file in the game's save folder, as
// Unreal's save game slots. A running game's folder is the player's own (the system's folder for the game's data, by the project's name,
// then saves/); headless runs, tests and the tools' sessions use the project's .slash/saves/, so
// an agent's playtests never touch a player's saves and an agent can read what a playtest saved
// (the save.* commands). A slot is written whole, to a file beside it first and then moved into
// place, so a crash mid-save leaves the last save. On the web the folder is kept in the browser's
// IndexedDB, read back when the game starts (save_data_ready says when) and written after each
// change.
namespace slash::platform {

// The format a save file is written in: {"format", "game_version", "saved", "data"}.
inline constexpr std::int64_t kSaveFormat = 1;

// A save as it was written: the game's version then, when (UTC, "2026-11-26T20:15:00Z"), and
// the game's data.
struct SaveFile {
    std::string game_version;
    std::string saved;
    json::Value data;
};

// A slot in the save folder.
struct SaveSlot {
    std::string name;
    std::string game_version;
    std::string saved;
    std::uint64_t bytes = 0;
};

// A save folder, ending in a separator, and the game version saves written there record.
struct SaveStore {
    std::string folder;
    std::string game_version;
};

// Where saves go from now on, for the whole process, and the game version they record: a game
// sets it when it starts, a session that writes files when it opens a project. The folder is
// made when a save is first written. On the web, mounts the browser's stored saves there and
// starts reading them.
void set_save_folder(std::string folder, std::string game_version);
// The process's save folder and game version: an empty folder before set_save_folder.
[[nodiscard]] SaveStore save_store();
// The player's save folder for a game of that name: the system's folder for the game's data
// (SDL_GetPrefPath: Application Support on macOS, AppData\Roaming on Windows, ~/.local/share on
// Linux, the app's files on iOS and Android), then saves/. Empty where the system has none.
[[nodiscard]] std::string user_save_folder(std::string_view game_name);
// The save folder in a project's folder: <project>/.slash/saves/.
[[nodiscard]] std::string project_save_folder(std::string_view project_folder);
// Whether saves can be read: always, but on the web only once the browser's stored ones are in.
[[nodiscard]] bool save_data_ready();

// Whether a slot's name is one: 1 to 64 lowercase letters, digits, '_' and '-'.
[[nodiscard]] bool valid_save_slot(std::string_view slot);

// Writes data to slot in the process's save folder, replacing what it held. Fails for a name that
// isn't one or with no folder set.
[[nodiscard]] Result<void> save_game(std::string_view slot, const json::Value& data);
// What slot holds: NotFound where it holds nothing, InvalidArgument for a file that isn't a
// save (or one from a later format).
[[nodiscard]] Result<SaveFile> load_game(std::string_view slot);
// Whether slot holds a save.
[[nodiscard]] bool has_save(std::string_view slot);
// Empties slot. Succeeds where it held nothing.
[[nodiscard]] Result<void> delete_save(std::string_view slot);
// Every slot that holds a save, most recently saved first (by name where two were saved in the
// same second). Files that aren't saves are left out.
[[nodiscard]] std::vector<SaveSlot> save_slots();

// The same in another store, such as the save.* commands' project folder.
[[nodiscard]] Result<void> save_game(const SaveStore& store, std::string_view slot,
                                     const json::Value& data);
[[nodiscard]] Result<SaveFile> load_game(const SaveStore& store, std::string_view slot);
[[nodiscard]] bool has_save(const SaveStore& store, std::string_view slot);
[[nodiscard]] Result<void> delete_save(const SaveStore& store, std::string_view slot);
[[nodiscard]] std::vector<SaveSlot> save_slots(const SaveStore& store);

} // namespace slash::platform
