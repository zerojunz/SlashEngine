#pragma once

#include <string>
#include <string_view>
#include <vector>

// Fonts installed on the machine, for text a game's own fonts can't draw, as Godot's
// OS::get_system_font_path_for_text.
namespace slash::platform {

// Font files installed on the machine that may draw some text (one character, UTF-8), best
// first: the operating system's own fallback choice (CoreText on macOS and iOS, DirectWrite on
// Windows, fontconfig on Linux, the system font list on Android) for a language (BCP 47, such as
// "ja"; "" for none) and a script (ISO 15924, such as "Arab"; "" for none). A file may be a
// collection (.ttc) and may still lack the text, so callers check. Empty where the platform has
// no font list (the web) or nothing fits; never Apple's LastResort font, which draws boxes.
[[nodiscard]] std::vector<std::string>
system_fonts_for(std::string_view text, std::string_view language, std::string_view script);

} // namespace slash::platform
