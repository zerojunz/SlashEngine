#include "slash/command/save.hpp"

#include "slash/command/session.hpp"
#include "slash/platform/save_data.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace slash::command {
namespace {

json::Value property(std::string_view type, std::string_view description) {
    json::Value schema;
    schema["type"] = type;
    schema["description"] = description;
    return schema;
}

json::Value params_schema(json::Object properties, json::Array required = {}) {
    json::Value schema;
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["properties"] = std::move(properties);
    if (!required.empty()) {
        schema["required"] = std::move(required);
    }
    return schema;
}

json::Value any_json(std::string_view description) {
    json::Value schema;
    schema["description"] = description;
    return schema;
}

json::Value slot_property() {
    json::Value schema = property("string", "The slot: lowercase letters, digits, '_' and '-'.");
    schema["pattern"] = "^[a-z0-9_-]{1,64}$";
    return schema;
}

// The project's save folder, and the version project.json gives the game.
platform::SaveStore project_store(Session& session) {
    std::string version = "0.1.0";
    auto text = session.read_project_file("project.json");
    const std::optional<std::string> contents = text ? std::move(text.value()) : std::nullopt;
    if (contents.has_value()) {
        if (auto project = json::parse(*contents); project && project.value().is_object()) {
            if (const json::Value* found = project.value().find("version");
                found != nullptr && found->is_string()) {
                version = found->as_string();
            }
        }
    }
    return {.folder = platform::project_save_folder(session.options().project_root),
            .game_version = std::move(version)};
}

Reply save_list(Session& session, const json::Value& /*params*/) {
    json::Array slots;
    for (const platform::SaveSlot& slot : platform::save_slots(project_store(session))) {
        json::Value item;
        item["slot"] = slot.name;
        item["game_version"] = slot.game_version;
        item["saved"] = slot.saved;
        item["bytes"] = slot.bytes;
        slots.push_back(std::move(item));
    }
    json::Value out;
    out["slots"] = std::move(slots);
    return Reply::success(std::move(out));
}

Reply save_read(Session& session, const json::Value& params) {
    const std::string& slot = params.find("slot")->as_string();
    auto save = platform::load_game(project_store(session), slot);
    if (!save) {
        return Reply::failure(save.error());
    }
    json::Value out;
    out["slot"] = slot;
    out["game_version"] = save.value().game_version;
    out["saved"] = save.value().saved;
    out["data"] = std::move(save.value().data);
    return Reply::success(std::move(out));
}

Reply save_write(Session& session, const json::Value& params) {
    if (!session.options().write_files) {
        return Reply::failure(make_error(ErrorCode::PermissionDenied,
                                         "tests don't write files, so the project stays as it "
                                         "was"));
    }
    const std::string& slot = params.find("slot")->as_string();
    const platform::SaveStore store = project_store(session);
    if (auto saved = platform::save_game(store, slot, *params.find("data")); !saved) {
        return Reply::failure(saved.error());
    }
    auto save = platform::load_game(store, slot);
    if (!save) {
        return Reply::failure(save.error());
    }
    json::Value out;
    out["slot"] = slot;
    out["game_version"] = save.value().game_version;
    out["saved"] = save.value().saved;
    return Reply::success(std::move(out));
}

Reply save_delete(Session& session, const json::Value& params) {
    if (!session.options().write_files) {
        return Reply::failure(make_error(ErrorCode::PermissionDenied,
                                         "tests don't write files, so the project stays as it "
                                         "was"));
    }
    const std::string& slot = params.find("slot")->as_string();
    const platform::SaveStore store = project_store(session);
    const bool held = platform::has_save(store, slot);
    if (auto deleted = platform::delete_save(store, slot); !deleted) {
        return Reply::failure(deleted.error());
    }
    json::Value out;
    out["slot"] = slot;
    out["deleted"] = held;
    return Reply::success(std::move(out));
}

} // namespace

void register_save_commands(Registry& registry) {
    using enum Capability;
    registry.add({
        .name = "save.list",
        .description = "Lists the game's save slots in the project's save folder "
                       "(.slash/saves/), where game code saves in sessions and headless runs: "
                       "each slot, the game version that wrote it, when (UTC) and its size, "
                       "newest first.",
        .capabilities = {Read},
        .effect = Effect::Read,
        .params = params_schema({}),
        .handler = save_list,
    });
    registry.add({
        .name = "save.read",
        .description = "Returns what a save slot holds: the game's data, the game version that "
                       "wrote it and when.",
        .capabilities = {Read},
        .effect = Effect::Read,
        .params = params_schema({{"slot", slot_property()}}, {"slot"}),
        .handler = save_read,
    });
    registry.add({
        .name = "save.write",
        .description = "Writes data to a save slot as the game's save_game does, replacing what "
                       "it held, as a player's save to load in a playtest.",
        .capabilities = {Filesystem},
        .effect = Effect::Read,
        .params = params_schema(
            {{"slot", slot_property()}, {"data", any_json("The game's data, any JSON.")}},
            {"slot", "data"}),
        .handler = save_write,
    });
    registry.add({
        .name = "save.delete",
        .description = "Empties a save slot; deleted says whether it held a save.",
        .capabilities = {Filesystem},
        .effect = Effect::Read,
        .params = params_schema({{"slot", slot_property()}}, {"slot"}),
        .handler = save_delete,
    });
}

} // namespace slash::command
