#pragma once

#include "slash/ai/config.hpp"
#include "slash/ai/provider.hpp"
#include "slash/ai/transport.hpp"
#include "slash/core/json.hpp"
#include "slash/core/result.hpp"
#include "slash/library/library.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

// Image and sound generators behind one interface, as ai::Provider is for language models, so no
// engine code outside the adapters names a vendor. providers.json's "generators" says which a
// project can call: "openai-image", "gemini-image" and "automatic1111" (a Stable Diffusion web UI)
// make and refine images, "openai-speech" speaks lines and "elevenlabs-sound" makes sound effects.
// The built-in "test" generator makes placeholders on the machine, so agents and CI use the
// library with no model or key.
namespace slash::library {

struct GenerateRequest {
    std::string type; // "image" or "sound"
    std::string prompt;
    int width = 0; // images; 0 for the generator's default
    int height = 0;
    double seconds = 0.0;               // sounds; 0 for the generator's default
    std::optional<std::int64_t> seed{}; // NOLINT(readability-redundant-member-init)
    // To refine an asset: its record and bytes; the prompt says what to change.
    const Record* parent = nullptr;
    std::string parent_data{}; // NOLINT(readability-redundant-member-init)
};

struct Generated {
    std::string data;   // the file's bytes
    std::string format; // its extension without the dot, such as "png" or "wav"
    std::string model;  // the model that made it
    std::optional<std::int64_t> seed;
};

// What a generator makes.
struct GeneratorCapabilities {
    bool images = false;
    bool sounds = false;
    bool refines_images = false; // takes an image and a prompt saying what to change
    bool refines_sounds = false;
};

class Generator {
public:
    virtual ~Generator() = default;

    [[nodiscard]] virtual const GeneratorCapabilities& capabilities() const = 0;
    // Makes the asset; safe to call from several threads at once.
    [[nodiscard]] virtual Result<Generated> generate(const GenerateRequest& request,
                                                     const ai::CancelFlag* cancel = nullptr) = 0;
};

// Placeholders made on the machine, the same on every platform: images in the colors and shape
// the prompt names (a circle for "ball", "coin" and the like, transparent where it says so), and
// tones whose pitch follows the prompt. Refining recolors, mirrors, lightens or darkens an image,
// and raises, lowers, lengthens or shortens a sound, as the prompt says.
[[nodiscard]] std::shared_ptr<Generator> test_generator();

// How generators reach their services.
struct GeneratorOptions {
    // null for ai::http_transport().
    std::shared_ptr<ai::Transport> transport{}; // NOLINT(readability-redundant-member-init)
    // Reads environment variables; null for the process's own.
    std::function<std::string(const std::string& name)>
        environment{}; // NOLINT(readability-redundant-member-init)
};

struct OpenedGenerator {
    std::shared_ptr<Generator> generator;
    std::string name;
    std::string type;
    std::string model;
    std::string license;
};

// The generator providers.json names, or the built-in "test" one. An empty name picks the only
// configured generator that makes the type; with none, or several, it is an error. Images a
// service makes at another size are fitted to the size asked for (a refine's parent's when none).
[[nodiscard]] Result<OpenedGenerator> open_generator(const ai::Config& config,
                                                     std::string_view name, std::string_view type,
                                                     const GeneratorOptions& options = {});

} // namespace slash::library
