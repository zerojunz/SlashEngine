#pragma once

#include "slash/command/command.hpp"

namespace slash::command {

// Adds l10n.state (the locale, the translations loaded, what didn't load and messages as the
// game would show them) and l10n.set_locale (the game's language, as an options menu changes
// it), over slash/l10n/l10n.hpp.
void register_l10n_commands(Registry& registry);

} // namespace slash::command
