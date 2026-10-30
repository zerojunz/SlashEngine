#pragma once

namespace slash::accessibility {

// Whether VoiceOver runs, as Godot's macOS accessibility_screen_reader_active (src/apple.mm).
[[nodiscard]] bool voice_over_running();

} // namespace slash::accessibility
