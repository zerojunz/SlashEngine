#pragma once

#include "slash/command/command.hpp"

#include <string_view>

// Shared by the ai.* commands (commands.cpp), storyboard.review (review.cpp) and l10n.translate
// (translate.cpp).
namespace slash::ai::detail {

// The media type of an image file, from its extension: "image/png", "image/jpeg", "image/gif"
// or "image/webp", or "" for anything else.
[[nodiscard]] std::string_view media_type(std::string_view path);

// storyboard.review.
void register_review(command::Registry& registry);

// l10n.translate.
void register_translate(command::Registry& registry);

} // namespace slash::ai::detail
