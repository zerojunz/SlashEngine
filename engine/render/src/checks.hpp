#pragma once

#include "slash/core/result.hpp"
#include "slash/render/device.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

// Checks both devices make, so the null device fails exactly where a GPU device would.
namespace slash::render::detail {

inline constexpr std::uint32_t kMaxTextureSize = 16384;
// The most vertices one frame can draw: 1.4 million triangles.
inline constexpr std::size_t kMaxFrameVertices = std::size_t{1} << 22;

[[nodiscard]] Result<void> check_texture_desc(const TextureDesc& desc);
// rgba must be empty (when allow_empty) or exactly width * height * 4 bytes.
[[nodiscard]] Result<void> check_pixels(const TextureDesc& desc, std::span<const std::uint8_t> rgba,
                                        bool allow_empty);
// in_frame is how many vertices the frame has drawn so far.
[[nodiscard]] Result<void> check_vertices(std::span<const Vertex2D> vertices, std::size_t in_frame);
[[nodiscard]] Error unknown_texture(TextureId texture);
[[nodiscard]] Error not_a_render_target(TextureId texture);
[[nodiscard]] Error target_in_use(TextureId texture);
[[nodiscard]] Error no_frame(std::string_view call);
[[nodiscard]] Error frame_in_progress(std::string_view call);
[[nodiscard]] std::array<std::uint8_t, 4> to_rgba8(const Color& color);

} // namespace slash::render::detail
