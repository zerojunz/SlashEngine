#pragma once

#include "slash/ai/config.hpp"
#include "slash/core/result.hpp"
#include "slash/library/generator.hpp"
#include "slash/render/device.hpp"

#include <array>
#include <memory>
#include <string_view>

// The generators that call a service over HTTP, one per API. Each builds its request from a
// GenerateRequest and the options providers.json gives it, sends it through an ai::Transport with
// retries and turns the reply into a file the library keeps.
namespace slash::library::detail {

inline constexpr std::array<std::string_view, 7> kHttpTypes = {
    "automatic1111", "elevenlabs-music", "elevenlabs-sound", "gemini-image",
    "openai-image",  "openai-speech",    "stability-audio"};

// The model a type's requests name when providers.json names none: "" for automatic1111, which
// draws with the checkpoint it has loaded.
[[nodiscard]] std::string_view default_model(std::string_view type) noexcept;

// The generator for a providers.json entry of one of kHttpTypes. A key its api_key_env names but
// the environment lacks fails each request, so listing generators needs no key.
[[nodiscard]] Result<std::shared_ptr<Generator>>
make_http_generator(const ai::GeneratorConfig& config, const GeneratorOptions& options);

// The image cropped around its center to the size's aspect ratio and scaled to it, each pixel the
// average of those it covers weighted by how much it covers and by alpha, so transparent pixels
// lend no color. Integer math only, so the same everywhere.
[[nodiscard]] render::Image fit_image(const render::Image& image, int width, int height);

} // namespace slash::library::detail
