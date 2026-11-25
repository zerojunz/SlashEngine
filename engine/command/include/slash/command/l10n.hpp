// Parts of this file are ported from Godot Engine (MIT); see THIRD_PARTY.md.
#pragma once

#include "slash/command/command.hpp"
#include "slash/command/session.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace slash::command {

// Adds l10n.state (the locale, the translations loaded, what didn't load and messages as the
// game would show them), l10n.set_locale (the game's language, as an options menu changes it),
// l10n.extract (the messages the game shows) and l10n.check (what its translations get wrong),
// over slash/l10n/l10n.hpp.
void register_l10n_commands(Registry& registry);

// A message a game shows: a control's text in a scene or prefab, or a tr or tr_n call's in the
// game's C++ code.
struct ExtractedMessage {
    std::string context;
    std::string message;
    std::string plural;             // tr_n's, else ""
    std::vector<std::string> where; // "scenes/menu.scene.json", "src/hud.cpp:12", each once
};

// Every message the project's scenes, prefabs and gameplay sources show, in the order they are
// first found (files in path order), each (context, message) once. Controls whose
// auto_translate_mode leaves them as written are left out.
[[nodiscard]] std::vector<ExtractedMessage> extract_messages(Session& session);

// A POT file of messages.
[[nodiscard]] std::string write_pot(std::string_view project_name,
                                    const std::vector<std::string>& files,
                                    const std::vector<ExtractedMessage>& messages);

} // namespace slash::command
