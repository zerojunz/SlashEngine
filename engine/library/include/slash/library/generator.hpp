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

// Image, sound, music and model generators behind one interface, as ai::Provider is for language
// models, so no engine code outside the adapters names a vendor. providers.json's "generators" says
// which a project can call: "openai-image", "gemini-image" and "automatic1111" (a Stable Diffusion
// web UI) make and refine images, "openai-speech" speaks lines, "elevenlabs-sound" makes sound
// effects and "elevenlabs-music" and "stability-audio" make music, "stability-3d" makes a model
// from a concept image and "meshy-rig" rigs a model of a humanoid figure. The built-in "test"
// generator makes placeholders on the machine, so agents and CI use the library with no model or
// key.
namespace slash::library {

struct GenerateRequest {
    std::string type; // "image", "sound", "music" or "model"
    std::string prompt;
    int width = 0; // images; 0 for the generator's default
    int height = 0;
    double seconds = 0.0; // sounds and music; 0 for the generator's default
    // Music: the tempo, beats per bar and key asked for (0 or "" for the generator's choice), and
    // whether it should loop, its end leading back to its start.
    double bpm = 0.0;
    int bar_beats = 0;
    std::string key{}; // NOLINT(readability-redundant-member-init): such as "D minor"
    bool loop = false;
    std::optional<std::int64_t> seed{}; // NOLINT(readability-redundant-member-init)
    // To refine an asset: its record and bytes; the prompt says what to change.
    const Record* parent = nullptr;
    std::string parent_data{}; // NOLINT(readability-redundant-member-init)
    // A model: the concept image it is made from, its record and bytes.
    const Record* image = nullptr;
    std::string image_data{}; // NOLINT(readability-redundant-member-init)
    // To rig a model, the parent: the skeleton asked for, "humanoid". Its record's size says how
    // tall the figure is.
    std::string rig{}; // NOLINT(readability-redundant-member-init)
};

struct Generated {
    std::string data;   // the file's bytes
    std::string format; // its extension without the dot, such as "png", "wav" or "glb"
    std::string model;  // the model that made it
    std::optional<std::int64_t> seed;
    // Music: the tempo and key the generator says it made, where it knows; 0 or "" otherwise.
    double bpm = 0.0;
    std::string key{}; // NOLINT(readability-redundant-member-init)
};

// What a generator makes.
struct GeneratorCapabilities {
    bool images = false;
    bool sounds = false;
    bool refines_images = false; // takes an image and a prompt saying what to change
    bool refines_sounds = false;
    bool music = false;
    bool refines_music = false;
    bool models = false; // makes a model from a concept image
    bool rigs = false;   // rigs a model of a humanoid figure (GenerateRequest::rig)
};

// Whether the generator makes assets of the type ("rig" for one that rigs models), and refines
// them.
[[nodiscard]] bool makes(const GeneratorCapabilities& can, std::string_view type) noexcept;
[[nodiscard]] bool refines(const GeneratorCapabilities& can, std::string_view type) noexcept;

class Generator {
public:
    virtual ~Generator() = default;

    [[nodiscard]] virtual const GeneratorCapabilities& capabilities() const = 0;
    // Makes the asset; safe to call from several threads at once.
    [[nodiscard]] virtual Result<Generated> generate(const GenerateRequest& request,
                                                     const ai::CancelFlag* cancel = nullptr) = 0;
};

// Placeholders made on the machine, the same on every platform: images in the colors and shape
// the prompt names (a circle for "ball", "coin" and the like, transparent where it says so),
// tones whose pitch follows the prompt, and music: chords in the key asked for (C major, or A
// minor for a dark or sad prompt), a beat at the tempo asked for (120 bpm) and whole bars that
// loop cleanly; and models: a concept image's shape in up to 24 cells a meter across, standing
// on the ground and pushed back into a slab of each cell's color: one cell deep for a thin or
// flat prompt, half its width for a thick or deep one, else a sixth; and rigs: a figure's
// skeleton placed by average proportions (scene3d::rig_humanoid). Refining recolors, mirrors,
// lightens or darkens an image, raises, lowers, lengthens or shortens a sound, and makes music
// faster, slower, higher, lower, longer or shorter, as the prompt says.
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
