#include "slash/exporter/forms.hpp"

#include "steps.hpp"

#include "slash/core/sha256.hpp"
#include "slash/library/library.hpp"
#include "slash/platform/file.hpp"
#include "slash/render/image.hpp"
#include "slash/render/texture_encoder.hpp"
#include "slash/runtime/project.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace slash::exporter {
namespace {

namespace fs = std::filesystem;
using render::TextureFormat;

// Where the library keeps forms. The number changes whenever the encoders' settings do, so a
// cache never hands back blocks the current settings wouldn't make.
constexpr std::string_view kFormsFolder = "forms/1";
constexpr std::uint32_t kBlock = 4; // texels on each side of a BC7 or ASTC 4x4 block

constexpr std::array<TextureFormat, 1> kDesktop{TextureFormat::Bc7};
constexpr std::array<TextureFormat, 1> kPhone{TextureFormat::Astc4x4};
constexpr std::array<TextureFormat, 2> kWeb{TextureFormat::Bc7, TextureFormat::Astc4x4};
constexpr std::array<std::string_view, 6> kPlatforms{"macos", "windows", "linux",
                                                     "web",   "android", "ios"};

std::string cache_path(const std::string& library_dir, std::string_view texture,
                       TextureFormat format) {
    const std::string hash = sha256_hex(texture);
    return library_dir + "/" + std::string(kFormsFolder) + "/" + hash.substr(0, 2) + "/" + hash +
           "." + std::string(render::texture_format_name(format)) + ".ktx2";
}

std::uint64_t gpu_bytes(const render::Image& image, TextureFormat format) {
    return render::texture_bytes({.width = image.width,
                                  .height = image.height,
                                  .filter = render::Filter::Linear,
                                  .render_target = false,
                                  .format = format});
}

bool is_texture_path(std::string_view name) {
    return render::is_image_path(name);
}

// Whether a texture's project path is one of paths or under one of them; any when there are none.
bool selected(std::string_view file, std::span<const std::string> paths) {
    if (paths.empty()) {
        return true;
    }
    return std::ranges::any_of(paths, [&](const std::string& path) {
        return file == path || (path.ends_with('/') && file.starts_with(path));
    });
}

} // namespace

std::span<const TextureFormat> platform_formats(std::string_view platform) {
    if (platform == "android" || platform == "ios") {
        return kPhone;
    }
    if (platform == "web") {
        return kWeb;
    }
    return kDesktop;
}

std::vector<TextureFormat> shipped_formats(std::string_view mode, std::string_view platform) {
    if (mode == "vram_compressed") {
        const std::span<const TextureFormat> formats = platform_formats(platform);
        return {formats.begin(), formats.end()};
    }
    if (mode == "vram_uncompressed") {
        return {TextureFormat::Rgba8};
    }
    return {};
}

std::string form_problem(const render::Image& image, TextureFormat format) {
    if (render::is_compressed(format) &&
        (image.width % kBlock != 0 || image.height % kBlock != 0)) {
        return "its sides, " + std::to_string(image.width) + "x" + std::to_string(image.height) +
               ", aren't multiples of 4, as BC7 and ASTC 4x4 blocks need";
    }
    return "";
}

Result<TextureForm> texture_form(std::string_view texture, const render::Image& image,
                                 TextureFormat format, const std::string& library_dir) {
    if (std::string problem = form_problem(image, format); !problem.empty()) {
        return make_error(ErrorCode::InvalidArgument, std::move(problem));
    }
    const std::string cached = library_dir.empty() ? "" : cache_path(library_dir, texture, format);
    if (!cached.empty() && platform::exists(cached)) {
        if (auto read = platform::read_file(cached); read) {
            return TextureForm{.format = format, .ktx2 = std::move(read).value(), .cached = true};
        }
    }
    render::Image form{.width = image.width, .height = image.height, .pixels = image.pixels};
    if (render::is_compressed(format)) {
        if (image.block_format == format && !image.blocks.empty()) {
            form.blocks = image.blocks; // already in this format
        } else {
            auto blocks = render::encode_texels(format, image.width, image.height, image.pixels);
            if (!blocks) {
                return blocks.error();
            }
            form.blocks = std::move(blocks).value();
        }
        form.block_format = format;
    }
    auto ktx2 = render::encode_ktx2(form);
    if (!ktx2) {
        return ktx2.error();
    }
    if (!cached.empty()) {
        if (auto written = library::write_whole(cached, ktx2.value()); !written) {
            return written.error();
        }
    }
    return TextureForm{.format = format, .ktx2 = std::move(ktx2).value(), .cached = false};
}

Result<std::optional<int>> form_psnr(const render::Image& image, const TextureForm& form) {
    auto decoded = render::decode_ktx2(form.ktx2);
    if (!decoded) {
        return decoded.error();
    }
    const std::vector<std::uint8_t>& pixels = decoded.value().pixels;
    if (pixels.size() != image.pixels.size()) {
        return make_error(ErrorCode::InvalidArgument, "the form is not the texture's size");
    }
    std::uint64_t squared = 0;
    for (std::size_t i = 0; i < pixels.size(); ++i) {
        const int difference = int{pixels[i]} - int{image.pixels[i]};
        squared += static_cast<std::uint64_t>(difference * difference);
    }
    if (squared == 0) {
        return std::optional<int>();
    }
    const double mean = static_cast<double>(squared) / static_cast<double>(pixels.size());
    return std::optional<int>(
        static_cast<int>(std::lround(10.0 * std::log10(255.0 * 255.0 / mean))));
}

Result<json::Value> describe_texture_forms(const std::string& project_dir,
                                           std::span<const std::string> platforms,
                                           std::span<const std::string> paths,
                                           const std::string& library_dir) {
    auto project = runtime::load_project(project_dir);
    if (!project) {
        return project.error();
    }
    const runtime::TextureImport& import = project.value().texture_import;
    std::vector<std::string> wanted(platforms.begin(), platforms.end());
    if (wanted.empty()) {
        wanted.assign(kPlatforms.begin(), kPlatforms.end());
    }

    // The textures in path order, so results are the same on every machine.
    std::vector<std::string> files;
    std::error_code error;
    const fs::path root(project_dir);
    auto it = fs::recursive_directory_iterator(root, error);
    for (; !error && it != fs::recursive_directory_iterator(); it.increment(error)) {
        if (it->is_directory(error)) {
            if (detail::skipped_folder(it->path().filename())) {
                it.disable_recursion_pending();
            }
            continue;
        }
        const std::string file = it->path().lexically_relative(root).generic_string();
        if (is_texture_path(file) && selected(file, paths)) {
            files.push_back(file);
        }
    }
    if (error) {
        return make_error(ErrorCode::PlatformFailure,
                          "could not list the project folder: " + error.message());
    }
    std::ranges::sort(files);

    struct Total {
        std::uint64_t bytes = 0;
        std::uint64_t gpu_bytes = 0;
    };
    std::map<std::string, Total, std::less<>> totals;
    json::Array textures;
    std::int64_t made = 0;
    for (const std::string& file : files) {
        json::Value entry;
        entry["path"] = file;
        const std::string_view mode = import.mode_for(file);
        entry["mode"] = std::string(mode);
        auto bytes = platform::read_file((root / file).string());
        auto image =
            bytes ? render::decode_image(bytes.value()) : Result<render::Image>(bytes.error());
        if (!image) {
            entry["error"] = image.error().message;
            textures.push_back(std::move(entry));
            continue;
        }
        entry["size"] =
            json::Array{std::int64_t{image.value().width}, std::int64_t{image.value().height}};
        entry["bytes"] = static_cast<std::int64_t>(bytes.value().size());

        // Each format the wanted platforms ship, made once however many platforms take it.
        std::vector<TextureFormat> formats;
        std::string note;
        for (const std::string& platform : wanted) {
            for (const TextureFormat format : shipped_formats(mode, platform)) {
                if (const std::string problem = form_problem(image.value(), format);
                    !problem.empty()) {
                    note = problem + ", so it ships as it is";
                } else if (std::ranges::find(formats, format) == formats.end()) {
                    formats.push_back(format);
                }
            }
        }
        if (!note.empty()) {
            formats.clear();
            entry["note"] = note;
        }
        json::Array forms;
        std::vector<std::uint64_t> form_bytes; // each format's KTX2 file, as formats lists them
        for (const TextureFormat format : formats) {
            auto form = texture_form(bytes.value(), image.value(), format, library_dir);
            if (!form) {
                return make_error(form.error().code, file + ": " + form.error().message);
            }
            auto psnr = form_psnr(image.value(), form.value());
            if (!psnr) {
                return make_error(psnr.error().code, file + ": " + psnr.error().message);
            }
            json::Value item;
            item["format"] = std::string(render::texture_format_name(format));
            item["bytes"] = static_cast<std::int64_t>(form.value().ktx2.size());
            item["gpu_bytes"] = static_cast<std::int64_t>(gpu_bytes(image.value(), format));
            if (psnr.value()) {
                item["psnr"] = std::int64_t{*psnr.value()};
            }
            item["cached"] = form.value().cached;
            form_bytes.push_back(form.value().ktx2.size());
            made += form.value().cached ? 0 : 1;
            forms.push_back(std::move(item));
        }
        entry["forms"] = std::move(forms);

        // What each platform ships and samples: the form it reads first, as games pick it
        // (canvas::with_texture_forms), else the file as it is, decoded to RGBA8.
        for (const std::string& platform : wanted) {
            Total& total = totals[platform];
            const std::vector<TextureFormat> shipped =
                note.empty() ? shipped_formats(mode, platform) : std::vector<TextureFormat>{};
            if (shipped.empty()) {
                total.bytes += bytes.value().size();
                total.gpu_bytes += gpu_bytes(image.value(), TextureFormat::Rgba8);
                continue;
            }
            for (const TextureFormat format : shipped) {
                const auto index = std::ranges::find(formats, format) - formats.begin();
                total.bytes += form_bytes.at(static_cast<std::size_t>(index));
            }
            total.gpu_bytes += gpu_bytes(image.value(), shipped.front());
        }
        textures.push_back(std::move(entry));
    }

    json::Value result;
    result["textures"] = std::move(textures);
    json::Value by_platform = json::Object{};
    for (const std::string& platform : wanted) {
        const Total& total = totals[platform];
        by_platform[platform]["bytes"] = static_cast<std::int64_t>(total.bytes);
        by_platform[platform]["gpu_bytes"] = static_cast<std::int64_t>(total.gpu_bytes);
    }
    result["platforms"] = std::move(by_platform);
    result["made"] = made;
    return result;
}

} // namespace slash::exporter
