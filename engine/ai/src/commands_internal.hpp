#pragma once

#include "slash/command/command.hpp"

#include <string_view>

// Shared by the ai.* commands (commands.cpp) and storyboard.review (review.cpp).
namespace slash::ai::detail {

// The media type of an image file, from its extension: "image/png", "image/jpeg", "image/gif"
// or "image/webp", or "" for anything else.
[[nodiscard]] std::string_view media_type(std::string_view path);

// storyboard.review.
void register_review(command::Registry& registry);

} // namespace slash::ai::detail
