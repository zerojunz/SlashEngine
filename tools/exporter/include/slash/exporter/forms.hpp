#pragma once

#include "slash/core/json.hpp"
#include "slash/core/result.hpp"
#include "slash/render/device.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Textures in the forms exports ship: KTX2 files of the texels as each platform's GPUs sample
// them, made once and kept in the asset library's cache, as Godot imports a texture once into
// .godot/imported.
namespace slash::exporter {

// The compressed formats a platform's GPUs sample: BC7 on macOS, Windows and Linux, ASTC 4x4 on
// Android and iOS, and both on the web, whose browsers have one or the other.
[[nodiscard]] std::span<const render::TextureFormat> platform_formats(std::string_view platform);

// The forms a texture ships as on a platform under a compress mode (project.json's
// rendering.textures): none for "lossless", which ships the file as it is, the platform's
// formats for "vram_compressed" and RGBA8 for "vram_uncompressed".
[[nodiscard]] std::vector<render::TextureFormat> shipped_formats(std::string_view mode,
                                                                 std::string_view platform);

// Why a texture can't take a format, or "" if it can: BC7 and ASTC 4x4 are blocks of 4x4
// texels, so a texture's sides must be multiples of 4.
[[nodiscard]] std::string form_problem(const render::Image& image, render::TextureFormat format);

struct TextureForm {
    render::TextureFormat format = render::TextureFormat::Rgba8;
    std::string ktx2;    // the KTX2 file an export ships
    bool cached = false; // read from the cache rather than made
};

// A texture file's bytes in a format, read from the cache in library_dir (its forms/ folder,
// keyed by the file's SHA-256) or encoded and kept there. An empty library_dir caches nothing.
[[nodiscard]] Result<TextureForm> texture_form(std::string_view texture, const render::Image& image,
                                               render::TextureFormat format,
                                               const std::string& library_dir);

// How close a form's texels are to the image's, as the peak signal-to-noise ratio of their RGBA
// channels in whole decibels; nullopt where they are the same.
[[nodiscard]] Result<std::optional<int>> form_psnr(const render::Image& image,
                                                   const TextureForm& form);

// The texture.forms command's work: every texture in the project (or only those under paths),
// with its compress mode, size and the forms the platforms ship, each made or read from the
// cache, with its file and GPU bytes and PSNR; then each platform's total.
[[nodiscard]] Result<json::Value> describe_texture_forms(const std::string& project_dir,
                                                         std::span<const std::string> platforms,
                                                         std::span<const std::string> paths,
                                                         const std::string& library_dir);

} // namespace slash::exporter
