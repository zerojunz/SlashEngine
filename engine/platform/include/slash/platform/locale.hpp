#pragma once

#include <string>

// The language the player set on their machine, as Godot's OS::get_locale.
namespace slash::platform {

// The first of the operating system's preferred locales (SDL_GetPreferredLocales), as
// language_COUNTRY: "en_US", "fr" or "zh_TW"; "" where it says none.
[[nodiscard]] std::string preferred_locale();

} // namespace slash::platform
