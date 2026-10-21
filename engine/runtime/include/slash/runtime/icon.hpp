#pragma once

#include "slash/core/result.hpp"
#include "slash/render/device.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

// The game's icon, as Godot's application/config/icon: project.json's "icon", or the engine's
// own. Exports make each platform's icons from it, and the game's window shows it.
namespace slash::runtime {

// Icons are made from an image this big on a side, the size the App Store asks for.
inline constexpr std::uint32_t kIconSize = 1024;
// Windows show their icon at this size at most.
inline constexpr std::uint32_t kWindowIconSize = 256;

// The engine's icon, for games that name none: a white slash on a dark blue square, size pixels
// on a side, drawn in code and the same on every machine.
[[nodiscard]] render::Image default_icon(std::uint32_t size);

// An icon file's bytes decoded. The error says why it can't be one: not an image, or not
// square, since every platform's icons are.
[[nodiscard]] Result<render::Image> decode_icon(std::string_view bytes);

// The icon a window shows: the one at path, read through read, or the engine's when path is
// empty or the file isn't an icon (with a warning). At most kWindowIconSize on a side.
[[nodiscard]] render::Image
window_icon(std::string_view path,
            const std::function<Result<std::string>(std::string_view path)>& read);

} // namespace slash::runtime
