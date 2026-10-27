#pragma once

#include "slash/command/command.hpp"
#include "slash/command/session.hpp"

#include <string>
#include <vector>

namespace slash::command {

// A message a game shows, as Godot's POT generator collects them: a control's text in a scene or
// prefab, or a tr or tr_n call's in the game's C++ code.
struct ExtractedMessage {
    std::string context;
    std::string message;
    std::string plural;             // tr_n's, else ""
    std::vector<std::string> where; // "scenes/menu.scene.json", "src/hud.cpp:12", each once
};

// Every message the project's scenes, prefabs and gameplay sources show, in the order they are
// first found (files in path order), each (context, message) once. Controls whose
// auto_translate_mode leaves them as written are left out, as Godot's scene parser leaves them.
[[nodiscard]] std::vector<ExtractedMessage> extract_messages(Session& session);

// A POT file of messages, as Godot's POTGenerator writes one.
[[nodiscard]] std::string write_pot(std::string_view project_name,
                                    const std::vector<std::string>& files,
                                    const std::vector<ExtractedMessage>& messages);

void register_l10n_extract(Registry& registry);

} // namespace slash::command
