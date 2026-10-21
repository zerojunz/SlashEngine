#include "slash/exporter/export.hpp"

#include "android.hpp"
#include "cross.hpp"
#include "ios.hpp"
#include "steps.hpp"

#include "slash/ai/config.hpp"
#include "slash/animation/animation.hpp"
#include "slash/audio/audio.hpp"
#include "slash/canvas/gpu_particles.hpp"
#include "slash/canvas/material.hpp"
#include "slash/canvas/sprite_frames.hpp"
#include "slash/canvas/tilemap.hpp"
#include "slash/core/log.hpp"
#include "slash/core/text.hpp"
#include "slash/exporter/android.hpp"
#include "slash/exporter/android_library.hpp"
#include "slash/exporter/forms.hpp"
#include "slash/exporter/icons.hpp"
#include "slash/exporter/maven.hpp"
#include "slash/exporter/presets.hpp"
#include "slash/exporter/sdk.hpp"
#include "slash/exporter/toolchains.hpp"
#include "slash/library/library.hpp"
#include "slash/platform/file.hpp"
#include "slash/platform/process.hpp"
#include "slash/render/image.hpp"
#include "slash/render/shader.hpp"
#include "slash/runtime/gameplay.hpp"
#include "slash/runtime/project.hpp"
#include "slash/shader_compiler/shader_compiler.hpp"
#include "slash/text/text.hpp"
#include "slash/ui/ui.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/scene.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifndef SLASH_ENGINE_SOURCE_DIR
#define SLASH_ENGINE_SOURCE_DIR ""
#endif

namespace slash::exporter {
namespace {

namespace fs = std::filesystem;
using detail::absolute_text;
using detail::display;
using detail::environment;
using detail::Failure;
using detail::find_on_path;
using detail::find_summary;
using detail::folder_bytes;
using detail::kTestTicks;
using detail::program_runs;
using detail::skipped_folder;
using detail::step;

constexpr std::string_view kMacosDeploymentTarget = "11.0";

// What a Mac app signed with the hardened runtime needs to record the microphone.
constexpr std::string_view kMicrophoneEntitlements =
    R"(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>com.apple.security.device.audio-input</key>
    <true/>
</dict>
</plist>
)";

#if defined(__x86_64__) || defined(_M_X64)
constexpr std::string_view kHostArch = kLinuxX86_64;
#elif defined(__aarch64__) || defined(_M_ARM64)
constexpr std::string_view kHostArch = kLinuxArm64;
#else
constexpr std::string_view kHostArch = "";
#endif

// Web and Android exports build anywhere. Macs and Linux PCs also cross-build Windows and Linux
// with downloaded toolchains (toolchains.hpp); Windows PCs build only Windows, with MSVC.
bool host_builds(std::string_view platform) {
    if (platform == "web" || platform == "android") {
        return true;
    }
#if defined(__APPLE__)
    return platform == "macos" || platform == "ios" || platform == "windows" || platform == "linux";
#elif defined(_WIN32)
    return platform == "windows";
#elif defined(__linux__)
    return platform == "linux" || platform == "windows";
#else
    return false;
#endif
}

std::string host_name() {
#if defined(__APPLE__)
    return "macos";
#elif defined(_WIN32)
    return "windows";
#elif defined(__linux__)
    return "linux";
#else
    return "this system";
#endif
}

bool is_engine_dir(const fs::path& path) {
    std::error_code error;
    return !path.empty() && fs::exists(path / "export" / "CMakeLists.txt", error);
}

// The Emscripten CMake toolchain file from an activated emsdk.
std::optional<fs::path> find_emscripten_toolchain() {
    const fs::path relative = fs::path("cmake") / "Modules" / "Platform" / "Emscripten.cmake";
    std::vector<fs::path> roots;
    if (const std::string emscripten = environment("EMSCRIPTEN"); !emscripten.empty()) {
        roots.emplace_back(emscripten);
    }
    if (const std::string emsdk = environment("EMSDK"); !emsdk.empty()) {
        roots.push_back(fs::path(emsdk) / "upstream" / "emscripten");
    }
    std::error_code error;
    for (const fs::path& root : roots) {
        if (fs::exists(root / relative, error)) {
            return root / relative;
        }
    }
    return std::nullopt;
}

// The files a game reads at run time: scenes, prefabs, the assets sprites draw, animations, tile
// sets, particle process materials, fonts and sounds.
enum class DataFile : std::uint8_t {
    None,
    Scene,
    Prefab,
    SpriteFrames,
    Material,
    ProcessMaterial,
    Animation,
    TileSet,
    Texture,
    FontSettings,
    Font,
    Theme,
    Sound,
    AudioStream,
    BusLayout
};

DataFile data_file(std::string_view name) {
    if (name.ends_with(world::kPrefabSuffix)) {
        return DataFile::Prefab;
    }
    if (name.ends_with(world::kSceneSuffix)) {
        return DataFile::Scene;
    }
    if (name.ends_with(canvas::kSpriteFramesSuffix)) {
        return DataFile::SpriteFrames;
    }
    if (name.ends_with(canvas::kMaterialSuffix)) {
        return DataFile::Material;
    }
    if (name.ends_with(canvas::kProcessMaterialSuffix)) {
        return DataFile::ProcessMaterial;
    }
    if (name.ends_with(animation::kAnimationSuffix)) {
        return DataFile::Animation;
    }
    if (name.ends_with(canvas::kTileSetSuffix)) {
        return DataFile::TileSet;
    }
    if (name.ends_with(".png") || name.ends_with(".bmp") || name.ends_with(".ktx2")) {
        return DataFile::Texture;
    }
    if (name.ends_with(text::kFontSuffix)) {
        return DataFile::FontSettings;
    }
    if (name.ends_with(ui::kThemeSuffix)) {
        return DataFile::Theme;
    }
    if (name.ends_with(audio::kStreamSuffix)) {
        return DataFile::AudioStream;
    }
    if (name.ends_with(audio::kBusLayoutSuffix)) {
        return DataFile::BusLayout;
    }
    if (audio::is_sound_path(name)) {
        return DataFile::Sound;
    }
    if (text::is_font_path(name)) {
        return DataFile::Font;
    }
    return DataFile::None;
}

// Every problem in one of a game's files; none for a file it can use.
std::vector<json::ValidationError> check_data_file(DataFile kind, std::string_view file,
                                                   std::string_view text,
                                                   const world::ComponentRegistry& components,
                                                   const world::FileReader& reader) {
    if (kind == DataFile::Texture) {
        if (auto image = render::decode_image(text); !image) {
            return {{"", image.error().message}};
        }
        return {};
    }
    if (kind == DataFile::Font) {
        if (auto font = text::check_font_data(text); !font) {
            return {{"", font.error().message}};
        }
        return {};
    }
    if (kind == DataFile::Sound) {
        if (auto sound = audio::decode_sound(text, file); !sound) {
            return {{"", sound.error().message}};
        }
        return {};
    }
    auto parsed = json::parse(text);
    if (!parsed) {
        return {{"", parsed.error().message}};
    }
    std::vector<json::ValidationError> errors;
    switch (kind) {
    case DataFile::SpriteFrames:
        return canvas::validate_sprite_frames(parsed.value());
    case DataFile::Material:
        errors = canvas::validate_material(parsed.value());
        if (errors.empty()) {
            errors = canvas::check_material_shader(parsed.value(), reader);
        }
        return errors;
    case DataFile::ProcessMaterial:
        return canvas::validate_process_material(parsed.value());
    case DataFile::Animation:
        return animation::validate_animation_library(parsed.value(), &components);
    case DataFile::TileSet:
        return canvas::validate_tileset(parsed.value());
    case DataFile::FontSettings:
        return text::validate_font(parsed.value());
    case DataFile::Theme:
        return ui::validate_theme(parsed.value());
    case DataFile::AudioStream:
        return audio::validate_stream_file(parsed.value());
    case DataFile::BusLayout:
        return audio::validate_bus_layout(parsed.value());
    case DataFile::Prefab:
        errors = world::validate_prefab(components, parsed.value(), reader);
        break;
    default:
        errors = world::validate_scene(components, parsed.value(), reader);
        break;
    }
    // Each tile map layer's cells must name tiles in its tile set, each AnimationPlayer's
    // animation and track targets must be in its library and targets, controls must lay out and
    // each sub-emitter must take part in a chain.
    if (errors.empty()) {
        errors = canvas::check_tile_layers(parsed.value(), reader);
    }
    if (errors.empty()) {
        errors = animation::check_players(parsed.value(), reader);
    }
    if (errors.empty() && kind == DataFile::Scene) {
        errors = ui::check_scene(components, parsed.value(), reader);
    }
    if (errors.empty() && kind == DataFile::Scene) {
        errors = canvas::check_sub_emitters(components, parsed.value(), reader);
    }
    return errors;
}

// The backends a platform's games draw with (render::create_device): Metal on Apple's, Vulkan or
// Direct3D 12 on Windows, Vulkan elsewhere, and OpenGL ES where a game can ask for it or a browser
// has nothing else.
canvas::ShaderTargets shader_targets(std::string_view platform) {
    if (platform == "macos" || platform == "ios") {
        return {.spirv = false, .msl = true, .glsl = false, .hlsl = false};
    }
    if (platform == "web") {
        return {.spirv = false, .msl = false, .glsl = true, .hlsl = false};
    }
    return {.spirv = true, .msl = false, .glsl = true, .hlsl = platform == "windows"};
}

// A material file with its shader compiled in as "compiled", for a game, which can't compile
// shaders; nullopt for a graph material, or a shader that doesn't compile (check_data_file says
// why).
std::optional<std::string> with_compiled_shader(std::string_view text,
                                                const world::FileReader& reader,
                                                std::string_view target) {
    auto doc = json::parse(text);
    if (!doc) {
        return std::nullopt;
    }
    auto material = canvas::parse_material(doc.value());
    if (!material || material.value().shader.empty()) {
        return std::nullopt;
    }
    auto compiled = canvas::compile_material_shader(material.value(), reader);
    if (!compiled || !compiled.value()) {
        return std::nullopt;
    }
    doc.value()["compiled"] =
        canvas::compiled_shader_json(*compiled.value(), shader_targets(target));
    return json::write(doc.value());
}

// How many texture forms an export made, read from the library's cache and left out.
struct FormCounts {
    std::int64_t made = 0;
    std::int64_t cached = 0;
    std::int64_t as_is = 0; // textures their mode would compress, shipped as they are
};

// Writes a texture's forms for the platform in place of its file, beside where it would be
// (render::texture_form_path), as project.json's rendering.textures says. False when the texture
// ships as it is: a lossless one, or one whose sides don't fit the platform's blocks.
Result<bool> stage_texture_forms(std::string_view file, std::string_view bytes,
                                 const runtime::TextureImport& import, std::string_view platform,
                                 const fs::path& data, const std::string& library_dir,
                                 FormCounts& counts) {
    const std::vector<render::TextureFormat> formats =
        shipped_formats(import.mode_for(file), platform);
    if (formats.empty()) {
        return false;
    }
    auto image = render::decode_image(bytes);
    if (!image) {
        return image.error();
    }
    for (const render::TextureFormat format : formats) {
        if (std::string problem = form_problem(image.value(), format); !problem.empty()) {
            log::warn("texture ships as it is", {{"file", file}, {"reason", problem}});
            ++counts.as_is;
            return false;
        }
    }
    for (const render::TextureFormat format : formats) {
        auto form = texture_form(bytes, image.value(), format, library_dir);
        if (!form) {
            return form.error();
        }
        ++(form.value().cached ? counts.cached : counts.made);
        const std::string path = (data / render::texture_form_path(file, format)).string();
        if (auto written = platform::write_file(path, form.value().ktx2); !written) {
            return written.error();
        }
    }
    return true;
}

// Copies project.json, providers.json and every scene, prefab, texture, .frames.json,
// .material.json, .anim.json, .tileset.json, font, .theme.json, sound, .audio.json and
// .bus_layout.json file into data, checking each on the way. Materials take their shaders compiled
// for the platform, and textures ship in the forms project.json's rendering.textures asks for,
// kept in library_dir's cache.
bool stage_data(const fs::path& project_dir, const runtime::Project& project, const fs::path& data,
                std::string_view target, const std::string& library_dir, Failure& failure) {
    std::error_code error;
    fs::remove_all(data, error);
    fs::create_directories(data, error);
    if (error) {
        failure = {"stage", "could not create " + display(data) + ": " + error.message(), {}};
        return false;
    }
    fs::copy_file(project_dir / runtime::kProjectFile, data / runtime::kProjectFile, error);
    if (error) {
        failure = {"stage", "could not copy project.json: " + error.message(), {}};
        return false;
    }

    world::ComponentRegistry components;
    world::register_builtin_components(components);
    // The game's own components, checked by the names and fields its sources declare.
    (void)runtime::add_gameplay_types(components, project_dir.string(), project, false);
    const world::FileReader reader = runtime::folder_reader(project_dir.string());
    json::Array problems;
    bool found_main = false;
    FormCounts forms;
    auto it = fs::recursive_directory_iterator(project_dir, error);
    for (; !error && it != fs::recursive_directory_iterator(); it.increment(error)) {
        const fs::directory_entry& entry = *it;
        if (entry.is_directory(error)) {
            if (skipped_folder(entry.path().filename())) {
                it.disable_recursion_pending();
            }
            continue;
        }
        const DataFile kind = data_file(entry.path().filename().string());
        if (kind == DataFile::None) {
            continue;
        }
        const fs::path relative = entry.path().lexically_relative(project_dir);
        const std::string file = relative.generic_string();
        found_main = found_main || file == project.main_scene;
        auto text = platform::read_file(entry.path().string());
        const std::vector<json::ValidationError> errors =
            text ? check_data_file(kind, file, text.value(), components, reader)
                 : std::vector<json::ValidationError>{{"", text.error().message}};
        for (const json::ValidationError& problem : errors) {
            json::Value item;
            item["file"] = file;
            item["path"] = problem.path;
            item["message"] = problem.message;
            problems.push_back(std::move(item));
        }
        fs::create_directories((data / relative).parent_path(), error);
        std::optional<std::string> compiled;
        if (kind == DataFile::Material && errors.empty()) {
            compiled = with_compiled_shader(text.value(), reader, target);
        }
        if (compiled) {
            if (auto written = platform::write_file((data / relative).string(), *compiled);
                !written) {
                failure = {"stage", "could not write " + file + ": " + written.error().message, {}};
                return false;
            }
            continue;
        }
        if (kind == DataFile::Texture && errors.empty()) {
            auto staged = stage_texture_forms(file, text.value(), project.texture_import, target,
                                              data, library_dir, forms);
            if (!staged) {
                failure = {
                    "stage", "could not make " + file + "'s forms: " + staged.error().message, {}};
                return false;
            }
            if (staged.value()) {
                continue;
            }
        }
        fs::copy_file(entry.path(), data / relative, fs::copy_options::overwrite_existing, error);
        if (error) {
            failure = {"stage", "could not copy " + file + ": " + error.message(), {}};
            return false;
        }
    }
    if (error) {
        failure = {"stage", "could not list the project folder: " + error.message(), {}};
        return false;
    }
    if (forms.made + forms.cached + forms.as_is > 0) {
        log::info("texture forms",
                  {{"made", forms.made}, {"cached", forms.cached}, {"as_is", forms.as_is}});
    }
    // providers.json, for a game that calls language models, without its replay settings: a
    // game calls the models it names, as the project asked when it was made.
    if (const fs::path providers = project_dir / ai::kConfigFile; fs::exists(providers, error)) {
        auto text = platform::read_file(providers.string());
        auto document = text ? json::parse(text.value()) : Result<json::Value>(text.error());
        const std::vector<json::ValidationError> errors =
            document ? ai::check_config(document.value())
                     : std::vector<json::ValidationError>{{"", document.error().message}};
        for (const json::ValidationError& problem : errors) {
            json::Value item;
            item["file"] = std::string(ai::kConfigFile);
            item["path"] = problem.path;
            item["message"] = problem.message;
            problems.push_back(std::move(item));
        }
        if (errors.empty()) {
            document.value().as_object().erase("replay");
            if (auto written = platform::write_file((data / ai::kConfigFile).string(),
                                                    json::write(document.value()));
                !written) {
                failure = {
                    "stage", "could not write providers.json: " + written.error().message, {}};
                return false;
            }
        }
    }
    if (!found_main) {
        json::Value item;
        item["file"] = std::string(runtime::kProjectFile);
        item["path"] = "/main_scene";
        item["message"] = "the main scene \"" + project.main_scene + "\" does not exist";
        problems.push_back(std::move(item));
    }
    if (!problems.empty()) {
        failure.step = "validate";
        failure.message = std::to_string(problems.size()) + " problem" +
                          (problems.size() == 1 ? "" : "s") + " in the project";
        failure.details["errors"] = std::move(problems);
        return false;
    }
    return true;
}

// An existing output folder is replaced only if a previous export made it, so a mistyped --out
// can't delete someone's files. Checked before building, so the refusal comes right away.
bool output_writable(const fs::path& output, const fs::path& record, Failure& failure) {
    std::error_code error;
    if (!fs::exists(output, error) || fs::is_empty(output, error)) {
        return true;
    }
    if (auto previous = platform::read_file(record.string()); previous) {
        if (auto parsed = json::parse(previous.value()); parsed) {
            const json::Value* recorded = parsed.value().find("output");
            if (recorded != nullptr && recorded->is_string() &&
                recorded->as_string() == absolute_text(output)) {
                return true;
            }
        }
    }
    failure = {"options",
               display(output) + " already exists and was not made by slash-cli export; delete "
                                 "it or pass another --out",
               {}};
    return false;
}

// Replaces the output folder with the freshly installed package.
bool publish(const fs::path& package, const fs::path& output, const fs::path& record,
             Failure& failure) {
    std::error_code error;
    fs::remove_all(output, error);
    fs::create_directories(output.parent_path(), error);
    fs::rename(package, output, error);
    if (error) {
        error.clear();
        fs::copy(package, output, fs::copy_options::recursive, error);
    }
    if (error) {
        failure = {"publish", "could not write " + display(output) + ": " + error.message(), {}};
        return false;
    }
    json::Value written;
    written["output"] = absolute_text(output);
    (void)platform::write_file(record.string(), json::write(written));
    return true;
}

// node is the Node.js web games run on: the downloaded one, or empty for node on PATH.
json::Value run_test(const std::string& platform, const std::string& arch, const fs::path& output,
                     const fs::path& program, const fs::path& log_path, const fs::path& node) {
    json::Value test;
    std::vector<std::string> args;
    if (platform == "web") {
        if (node.empty() && !program_runs("node")) {
            test["skipped"] = "Node.js is not on PATH";
            return test;
        }
        args = {node.empty() ? std::string("node") : node.string(), program.string()};
    } else if (platform == host_name() && (platform != "linux" || arch == kHostArch)) {
        args = {program.string()};
    } else if (platform == "linux" && platform == host_name()) {
        test["skipped"] = "this machine can't run " + arch + " Linux games";
        return test;
    } else {
        test["skipped"] = "this machine can't run " + platform + " games";
        return test;
    }
    args.insert(args.end(), {"--headless", "--ticks", std::to_string(kTestTicks)});
    log::info("export step", {{"step", "test"}});
    platform::ProcessOptions process;
    process.working_directory = output.string();
    process.log_path = log_path.string();
    process.tail_lines = 20;
    auto result = platform::run_process(args, process);
    if (!result) {
        test["passed"] = false;
        test["message"] = result.error().message;
        return test;
    }
    const auto summary = find_summary(result.value().tail);
    const bool passed = result.value().exit_code == 0 && summary.has_value() &&
                        summary->find("ticks")->is_int() &&
                        summary->find("ticks")->as_int() == static_cast<std::int64_t>(kTestTicks);
    test["passed"] = passed;
    if (summary.has_value()) {
        detail::add_summary(test, *summary);
    }
    if (!passed) {
        test["exit_code"] = result.value().exit_code;
        test["tail"] = result.value().tail;
    }
    return test;
}

ExportResult fail(const Failure& failure, const fs::path& log_path) {
    json::Value error = failure.details.is_object() ? failure.details : json::Value(json::Object{});
    error["step"] = failure.step;
    error["message"] = failure.message;
    std::error_code ignored;
    if (!log_path.empty() && fs::exists(log_path, ignored)) {
        error["log"] = display(log_path);
    }
    ExportResult result;
    result.json["error"] = std::move(error);
    return result;
}

// Validation errors for one file, as {"file", "path", "message"} items.
json::Array file_errors(std::string_view file, const std::vector<json::ValidationError>& errors) {
    json::Array items;
    for (const json::ValidationError& problem : errors) {
        json::Value item;
        item["file"] = std::string(file);
        item["path"] = problem.path;
        item["message"] = problem.message;
        items.push_back(std::move(item));
    }
    return items;
}

// Re-reads a file that failed to load, to report every error instead of the first.
template <typename Validate>
json::Value describe_errors(const fs::path& project_dir, std::string_view file, Validate validate) {
    json::Value details;
    if (auto text = platform::read_file((project_dir / file).string()); text) {
        if (auto parsed = json::parse(text.value()); parsed) {
            json::Array errors = file_errors(file, validate(parsed.value()));
            if (!errors.empty()) {
                details["errors"] = std::move(errors);
            }
        }
    }
    return details;
}

// signtool from PATH, or from the newest Windows 10 or 11 SDK.
std::string find_signtool() {
    if (std::string found = find_on_path("signtool"); !found.empty()) {
        return found;
    }
    const fs::path kits =
        fs::path(environment("ProgramFiles(x86)")) / "Windows Kits" / "10" / "bin";
    std::error_code error;
    fs::path best;
    for (auto it = fs::directory_iterator(kits, error); !error && it != fs::directory_iterator();
         it.increment(error)) {
        const fs::path candidate = it->path() / "x64" / "signtool.exe";
        if (fs::is_regular_file(candidate, error) && (best.empty() || candidate > best)) {
            best = candidate;
        }
    }
    return best.string();
}

// What signing needs, found before building so a missing tool or secret fails right away.
struct Signing {
    std::string signtool;
    std::string certificate;
    std::string password;
    std::string api_key;
    std::string api_key_id;
    std::string api_issuer;
    fs::path entitlements;
};

bool prepare_signing(const Preset& preset, const fs::path& project_dir, Signing& signing,
                     Failure& failure) {
    std::error_code error;
    if (preset.platform == "macos") {
        const MacosOptions& macos = preset.macos;
        if (!macos.entitlements.empty()) {
            signing.entitlements = fs::absolute(project_dir / macos.entitlements, error);
            if (!fs::is_regular_file(signing.entitlements, error)) {
                failure = {"options",
                           "the entitlements file " + macos.entitlements + " named by preset \"" +
                               preset.name + "\" does not exist",
                           {}};
                return false;
            }
        }
        if (macos.codesign_identity != kAdHocIdentity) {
            auto identities =
                platform::run_process({"security", "find-identity", "-v", "-p", "codesigning"});
            if (!identities ||
                identities.value().tail.find(macos.codesign_identity) == std::string::npos) {
                failure = {"options",
                           "the codesign identity \"" + macos.codesign_identity +
                               "\" is not in the keychain; \"security find-identity "
                               "-v -p codesigning\" lists the ones that are",
                           {}};
                return false;
            }
        }
        if (macos.notarize) {
            signing.api_key = environment(kAppleApiKeyPath);
            signing.api_key_id = environment(kAppleApiKeyId);
            signing.api_issuer = environment(kAppleApiIssuer);
            if (signing.api_key.empty() || signing.api_key_id.empty() ||
                signing.api_issuer.empty()) {
                failure = {"options",
                           "notarizing needs an App Store Connect API key: set " +
                               std::string(kAppleApiKeyPath) + " (the .p8 file), " +
                               std::string(kAppleApiKeyId) + " and " + std::string(kAppleApiIssuer),
                           {}};
                return false;
            }
        }
    } else if (preset.platform == "windows" && preset.windows.sign) {
        if (host_name() != "windows") {
            failure = {"options",
                       "signing Windows executables needs signtool, which runs only on Windows; "
                       "export on Windows or turn off windows.sign in the preset",
                       {}};
            return false;
        }
        signing.certificate = environment(kWindowsCertificatePath);
        signing.password = environment(kWindowsCertificatePassword);
        if (signing.certificate.empty() || !fs::is_regular_file(signing.certificate, error)) {
            failure = {"options",
                       "signing needs a .pfx code-signing certificate: set " +
                           std::string(kWindowsCertificatePath) + " to its path and " +
                           std::string(kWindowsCertificatePassword) + " to its password",
                       {}};
            return false;
        }
        signing.signtool = find_signtool();
        if (signing.signtool.empty()) {
            failure = {"options",
                       "signing needs signtool from the Windows SDK; install the SDK with Visual "
                       "Studio or put signtool.exe on PATH",
                       {}};
            return false;
        }
    }
    return true;
}

// A failure naming the first problem with the game's SDK packages, with all of them in
// "problems".
Failure sdk_failure(std::string step, const std::vector<SdkProblem>& problems) {
    const SdkProblem& first = problems.front();
    Failure failure{std::move(step),
                    "SDK package " + first.sdk + (first.path.empty() ? "" : " " + first.path) +
                        ": " + first.message,
                    {}};
    json::Array items;
    for (const SdkProblem& problem : problems) {
        json::Value item;
        item["sdk"] = problem.sdk;
        if (!problem.path.empty()) {
            item["path"] = problem.path;
        }
        item["message"] = problem.message;
        items.push_back(std::move(item));
    }
    failure.details["problems"] = std::move(items);
    return failure;
}

// The keys an Info.plist template sets itself, which SDK packages can't set again.
std::set<std::string, std::less<>> template_plist_keys(const fs::path& path) {
    std::set<std::string, std::less<>> keys;
    auto text = platform::read_file(path.string());
    if (!text) {
        return keys;
    }
    const std::string_view all = text.value();
    constexpr std::string_view kOpen = "<key>";
    constexpr std::string_view kClose = "</key>";
    for (std::size_t at = all.find(kOpen); at != std::string_view::npos; at = all.find(kOpen, at)) {
        at += kOpen.size();
        const std::size_t end = all.find(kClose, at);
        if (end == std::string_view::npos) {
            break;
        }
        keys.emplace(all.substr(at, end - at));
        at = end;
    }
    return keys;
}

// Writes the entitlements at existing (if any) with entries added to out, for codesign. With
// granted, existing is a provisioning profile's entitlements, which are what the app ID may
// use: each entry's key must be there already, and nothing is written.
bool merge_entitlements(const fs::path& existing, const json::Value& entries, bool granted,
                        const fs::path& out, Failure& failure) {
    json::Value merged = json::Object{};
    if (!existing.empty()) {
        auto read = read_plist(existing);
        if (!read) {
            failure = {"sdks", read.error().message, {}};
            return false;
        }
        merged = std::move(read).value();
    }
    for (const auto& [key, value] : entries.as_object()) {
        const json::Value* found = merged.find(key);
        if (found == nullptr && granted) {
            failure = {"sdks",
                       "an SDK package needs the entitlement " + key +
                           ", which the provisioning profile doesn't grant; turn the capability "
                           "on for the app ID and download the profile again",
                       {}};
            return false;
        }
        if (found != nullptr && !(*found == value) && !granted) {
            failure = {"sdks",
                       "an SDK package sets the entitlement " + key + ", which " +
                           display(existing) + " sets to something else",
                       {}};
            return false;
        }
        merged[key] = value;
    }
    if (granted) {
        return true;
    }
    if (!platform::write_file(out.string(), plist_document(merged))) {
        failure = {"sdks", "could not write " + display(out), {}};
        return false;
    }
    return true;
}

// Gives each dylib an SDK package put in a Mac app's Frameworks folder an @rpath install name and
// points the game at it, so libraries made to sit beside the executable (@loader_path) or at a
// vendor's install path load from Frameworks.
bool fix_install_names(const fs::path& frameworks, const fs::path& executable,
                       const platform::ProcessOptions& process, Failure& failure) {
    std::error_code error;
    if (!fs::is_directory(frameworks, error)) {
        return true;
    }
    platform::ProcessOptions quiet = process;
    quiet.tail_lines = 1;
    for (const fs::directory_entry& entry : fs::directory_iterator(frameworks, error)) {
        if (entry.path().extension() != ".dylib" || !entry.is_regular_file(error)) {
            continue;
        }
        // "<file>:" then the install name, once per architecture in a universal library.
        auto shown = platform::run_process({"otool", "-D", entry.path().string()}, quiet);
        if (!shown || shown.value().exit_code != 0) {
            failure = {"sign", "otool could not read " + display(entry.path()), {}};
            return false;
        }
        std::string name = shown.value().tail;
        while (!name.empty() &&
               (name.back() == '\n' || name.back() == '\r' || name.back() == ' ')) {
            name.pop_back();
        }
        const std::string wanted = "@rpath/" + entry.path().filename().string();
        if (name.empty() || name.ends_with(':') || name == wanted) {
            continue;
        }
        if (!step("sign", {"install_name_tool", "-id", wanted, entry.path().string()}, process,
                  failure) ||
            !step("sign", {"install_name_tool", "-change", name, wanted, executable.string()},
                  process, failure)) {
            return false;
        }
    }
    return true;
}

// Signs the packaged game in place, before it is published, and records what it signed with in
// signed_with. Leaves signed_with null when the preset doesn't sign.
bool sign(const Preset& preset, const Signing& signing, const fs::path& package,
          const std::string& binary, const fs::path& work, const platform::ProcessOptions& process,
          json::Value& signed_with, Failure& failure) {
    std::error_code error;
    if (preset.platform == "macos") {
        // Every macOS export is signed, ad hoc by default, so the bundle and its data are
        // sealed and Apple silicon Macs will run it.
        const MacosOptions& macos = preset.macos;
        const std::string app = fs::absolute(package / (binary + ".app"), error).string();
        std::vector<std::string> args{"codesign", "--force", "--sign", macos.codesign_identity};
        if (macos.codesign_identity != kAdHocIdentity) {
            args.insert(args.end(), {"--options", "runtime", "--timestamp"});
        }
        // What SDK packages put in Contents/Frameworks is signed first, without entitlements.
        const fs::path contents = fs::path(app) / "Contents";
        if (!fix_install_names(contents / "Frameworks", contents / "MacOS" / binary, process,
                               failure) ||
            !detail::sign_nested(contents / "Frameworks", args, process, failure)) {
            return false;
        }
        if (!signing.entitlements.empty()) {
            args.insert(args.end(), {"--entitlements", signing.entitlements.string()});
        }
        args.push_back(app);
        if (!step("sign", args, process, failure) ||
            !step("sign", {"codesign", "--verify", "--strict", app}, process, failure)) {
            return false;
        }
        signed_with["identity"] = macos.codesign_identity;
        if (macos.notarize) {
            const std::string archive = fs::absolute(work / "notarize.zip", error).string();
            if (!step("notarize", {"ditto", "-c", "-k", "--keepParent", app, archive}, process,
                      failure) ||
                !step("notarize",
                      {"xcrun", "notarytool", "submit", archive, "--key", signing.api_key,
                       "--key-id", signing.api_key_id, "--issuer", signing.api_issuer, "--wait"},
                      process, failure) ||
                !step("notarize", {"xcrun", "stapler", "staple", app}, process, failure)) {
                return false;
            }
        }
        signed_with["notarized"] = macos.notarize;
    } else if (preset.platform == "windows" && preset.windows.sign) {
        const std::string exe = fs::absolute(package / (binary + ".exe"), error).string();
        std::vector<std::string> args{signing.signtool, "sign", "/fd",
                                      "SHA256",         "/f",   signing.certificate};
        if (!signing.password.empty()) {
            args.insert(args.end(), {"/p", signing.password});
        }
        if (!preset.windows.timestamp_url.empty()) {
            args.insert(args.end(), {"/tr", preset.windows.timestamp_url, "/td", "SHA256"});
        }
        args.push_back(exe);
        if (!step("sign", args, process, failure)) {
            return false;
        }
        signed_with["certificate"] = fs::path(signing.certificate).filename().string();
        signed_with["timestamped"] = !preset.windows.timestamp_url.empty();
    }
    return true;
}

} // namespace

std::optional<std::string> find_engine(const std::string& engine) {
    std::error_code error;
    if (!engine.empty()) {
        return is_engine_dir(engine) ? std::optional<std::string>(
                                           fs::absolute(engine, error).lexically_normal().string())
                                     : std::nullopt;
    }
    for (const fs::path& candidate :
         {fs::path(environment("SLASH_ENGINE_DIR")),
          fs::path(platform::base_path()) / ".." / "share" / "slashengine",
          fs::path(SLASH_ENGINE_SOURCE_DIR)}) {
        if (is_engine_dir(candidate)) {
            return fs::absolute(candidate, error).lexically_normal().string();
        }
    }
    return std::nullopt;
}

ExportResult export_game(const ExportOptions& options) {
    const auto started = std::chrono::steady_clock::now();
    const fs::path project_dir = options.project;

    auto presets = load_presets(project_dir.string());
    if (!presets) {
        return fail({"validate", presets.error().message,
                     describe_errors(project_dir, kPresetsFile, validate_presets)},
                    {});
    }
    auto found = find_preset(presets.value(), options.target);
    if (!found) {
        return fail({"options", found.error().message, {}}, {});
    }
    const Preset& preset = found.value();
    const std::string& platform = preset.platform;
    const std::string arch = platform == "linux" ? preset.linux_options.arch : std::string();
    const bool debug = options.debug || preset.debug;
    const fs::path work = project_dir / ".slash" / "export" / preset.name;
    fs::path output = project_dir / "build" / "export" / preset.name;
    if (!options.output.empty()) {
        output = options.output;
    } else if (!preset.output.empty()) {
        output = project_dir / preset.output;
    }
    const fs::path log_path = work / "export.log";

    if (!host_builds(platform)) {
        return fail({"options",
                     platform + " games can't be built on " + host_name() + " yet; build them on " +
                         platform + " or in CI",
                     {}},
                    {});
    }

    auto project = runtime::load_project(project_dir.string());
    if (!project) {
        return fail(
            {"validate", project.error().message,
             describe_errors(project_dir, runtime::kProjectFile, runtime::validate_project)},
            {});
    }

    // The SDK packages this export links, with their keys from the environment.
    std::vector<SdkProblem> sdk_problems;
    const std::vector<Sdk> sdks =
        load_project_sdks(project_dir, project.value().sdks, sdk_problems);
    const std::vector<const Sdk*> linked = sdks_for_preset(sdks, preset, sdk_problems);
    if (!sdk_problems.empty()) {
        return fail(sdk_failure("validate", sdk_problems), {});
    }
    std::map<std::string, SdkPlaces> sdk_places;
    for (const Sdk* sdk : linked) {
        sdk_places[sdk->name].keys = sdk_keys(*sdk, sdk_problems);
    }
    if (!sdk_problems.empty()) {
        return fail(sdk_failure("options", sdk_problems), {});
    }

    const std::optional<std::string> engine = find_engine(options.engine);
    if (!engine) {
        return fail({"options",
                     "could not find the engine sources; pass --engine <folder> or set "
                     "SLASH_ENGINE_DIR to a SlashEngine checkout",
                     {}},
                    {});
    }
    // An activated emsdk's Emscripten; without one, web exports download their own below.
    std::optional<fs::path> toolchain;
    if (platform == "web") {
        toolchain = find_emscripten_toolchain();
    }
    if (!program_runs("cmake")) {
        return fail({"options", "exports need CMake 3.25 or newer on PATH", {}}, {});
    }
    Failure failure;
    Signing signing;
    if (!prepare_signing(preset, project_dir, signing, failure)) {
        return fail(failure, {});
    }
    if (platform == "macos" && project.value().audio.enable_input &&
        preset.macos.codesign_identity != kAdHocIdentity && signing.entitlements.empty()) {
        // The hardened runtime keeps the microphone from apps not entitled to it.
        std::error_code error;
        signing.entitlements = fs::absolute(work / "entitlements.plist", error);
        fs::create_directories(work, error);
        if (!platform::write_file(signing.entitlements.string(), kMicrophoneEntitlements)) {
            return fail({"options", "could not write " + signing.entitlements.string(), {}}, {});
        }
    }

    std::optional<AndroidTools> android;
    detail::AndroidKey android_key;
    detail::ApkInfo apk_info;
    std::string ninja = program_runs("ninja") ? "ninja" : "";
    if (platform == "android") {
        auto tools = find_android_tools();
        if (!tools) {
            return fail({"options", tools.error().message, {}}, {});
        }
        android = std::move(tools).value();
        log::info(
            "android tools",
            {{"sdk", android->sdk}, {"ndk", android->ndk}, {"java_home", android->java_home}});
        apk_info.package =
            preset.android.package.empty() ? project.value().bundle_id() : preset.android.package;
        if (const std::string problem = android_package_problem(apk_info.package);
            !problem.empty()) {
            return fail({"options",
                         "\"" + apk_info.package +
                             "\" can't be an Android package name: " + problem +
                             "; set \"id\" in project.json or \"package\" in the preset's android "
                             "section",
                         {}},
                        {});
        }
        apk_info.version_code = preset.android.version_code;
        if (apk_info.version_code == 0) {
            auto code = android_version_code(project.value().version);
            if (!code) {
                return fail({"options", code.error().message, {}}, {});
            }
            apk_info.version_code = code.value();
        }
        apk_info.version_name = project.value().version;
        apk_info.debug = debug;
        if (!detail::prepare_android_key(debug, android_key, failure)) {
            return fail(failure, {});
        }
        ninja = detail::android_ninja(*android);
#ifdef _WIN32
        if (ninja.empty()) {
            return fail({"options",
                         "Android exports on Windows need Ninja; put ninja.exe on PATH or install "
                         "it with sdkmanager \"cmake;3.31.6\"",
                         {}},
                        {});
        }
#endif
    }

    std::string ios_bundle_id;
    detail::IosSigning ios_signing;
    if (platform == "ios") {
        ios_bundle_id =
            preset.ios.bundle_id.empty() ? project.value().bundle_id() : preset.ios.bundle_id;
        if (const std::string problem = ios_bundle_id_problem(ios_bundle_id); !problem.empty()) {
            return fail({"options",
                         "\"" + ios_bundle_id + "\" can't be an iOS bundle ID: " + problem +
                             "; set \"id\" in project.json or \"bundle_id\" in the preset's ios "
                             "section",
                         {}},
                        {});
        }
        if (!detail::prepare_ios(preset.ios, ios_bundle_id, debug, work, ios_signing, failure)) {
            return fail(failure, {});
        }
    }

    std::error_code error;
    fs::create_directories(work, error);
    (void)platform::write_file(log_path.string(), "");

    if (!output_writable(output, work / "output.json", failure)) {
        return fail(failure, {});
    }
    log::info("export step", {{"step", "validate"}});
    if (!render::has_shader_compiler()) {
        shader_compiler::install(); // games get their materials' shaders compiled
    }
    const fs::path data = fs::absolute(work / "data", error);
    const std::string library_dir =
        options.library.empty() ? library::default_dir() : options.library;
    if (!stage_data(project_dir, project.value(), data, platform, library_dir, failure)) {
        return fail(failure, log_path);
    }
    // The platform's icons, made from the preset's icon, the project's or the engine's.
    if (const std::vector<IconProblem> problems =
            check_icons(project_dir, project.value(), {&preset});
        !problems.empty()) {
        json::Array errors;
        for (const IconProblem& problem : problems) {
            json::Value item;
            item["file"] = problem.file;
            item["path"] = problem.path;
            item["message"] = problem.message;
            errors.push_back(std::move(item));
        }
        Failure icon_failure{"validate",
                             std::to_string(errors.size()) + " problem" +
                                 (errors.size() == 1 ? "" : "s") + " in the project",
                             {}};
        icon_failure.details["errors"] = std::move(errors);
        return fail(icon_failure, log_path);
    }
    auto icon_images = preset_icons(project_dir, project.value(), preset);
    auto icon_files = icon_images ? platform_icons(platform, icon_images.value().settings)
                                  : Result<std::vector<IconFile>>(icon_images.error());
    const fs::path icons = fs::absolute(work / "icons", error);
    fs::remove_all(icons, error);
    if (icon_files) {
        for (const IconFile& file : icon_files.value()) {
            fs::create_directories((icons / file.path).parent_path(), error);
            if (auto written = platform::write_file((icons / file.path).string(), file.bytes);
                !written) {
                icon_files = written.error();
                break;
            }
        }
    }
    if (!icon_files) {
        return fail({"icons", "could not make the icons: " + icon_files.error().message, {}},
                    log_path);
    }
    if (!linked.empty()) {
        // Each download once, kept in sdks_dir() for every project and export after this one.
        log::info("export step", {{"step", "sdks"}});
        const fs::path dir = sdks_dir();
        for (const Sdk* sdk : linked) {
            for (const SdkDownload& download : sdk->downloads) {
                auto location = fetch_download(download, sdk->folder, dir, log_path.string());
                if (!location) {
                    json::Value details;
                    details["sdk"] = sdk->location;
                    details["path"] = "/downloads/" + download.id;
                    return fail({"sdks", sdk->location + ": " + location.error().message,
                                 std::move(details)},
                                log_path);
                }
                sdk_places[sdk->name].downloads[download.id] = location.value();
            }
        }
    }
    // Android libraries from the packages: their own .aar files and folders first, then their
    // Maven dependencies as their sdk.lock.json files pin them, joined into one graph.
    std::vector<AndroidLibrary> android_libraries;
    if (android) {
        std::vector<std::vector<MavenArtifact>> graphs;
        for (const Sdk* sdk : linked) {
            const auto target = sdk->platforms.find("android");
            if (target == sdk->platforms.end() || target->second.maven.empty()) {
                continue;
            }
            auto lock = load_maven_lock(*sdk);
            if (!lock) {
                json::Value details;
                details["sdk"] = sdk->location;
                details["path"] = "/platforms/android/maven";
                return fail(
                    {"sdks", sdk->location + ": " + lock.error().message, std::move(details)},
                    log_path);
            }
            graphs.push_back(std::move(lock).value().artifacts);
        }
        std::vector<SdkProblem> problems;
        for (const Sdk* sdk : linked) {
            // No {arch} or {build} in aars, so any ABI resolves them.
            const ResolvedSdk resolved = resolve_sdk(*sdk, "android", preset.android.abis.front(),
                                                     sdk_places[sdk->name], true, problems);
            for (const std::string& path : resolved.aars) {
                android_libraries.push_back({sdk->name + ": " + path, path});
            }
        }
        if (!problems.empty()) {
            return fail(sdk_failure("sdks", problems), log_path);
        }
        for (const MavenArtifact& artifact : merge_maven_artifacts(graphs)) {
            auto file = fetch_maven_artifact(artifact, maven_cache(), log_path.string());
            if (!file) {
                json::Value details;
                details["artifact"] = artifact.id;
                return fail({"sdks", artifact.id + ": " + file.error().message, std::move(details)},
                            log_path);
            }
            android_libraries.push_back({artifact.id, file.value()});
        }
    }

    const std::string config = debug ? "Debug" : "Release";
    const fs::path engine_dir = engine.value();
    std::vector<std::string> configure{"cmake",
                                       "-S",
                                       (engine_dir / "export").string(),
                                       "-DCMAKE_BUILD_TYPE=" + config,
                                       "-DSLASH_GAME_NAME=" + project.value().name,
                                       "-DSLASH_GAME_BINARY=" + project.value().binary_name(),
                                       "-DSLASH_GAME_ID=" + project.value().bundle_id(),
                                       "-DSLASH_GAME_VERSION=" + project.value().version,
                                       "-DSLASH_GAME_DATA=" + data.generic_string()};
    // The game's own C++ code, linked in. Always passed, so a project that drops its code
    // doesn't keep the last export's.
    std::string sources;
    for (const std::string& source : project.value().gameplay_sources) {
        if (!sources.empty()) {
            sources += ';';
        }
        sources += fs::absolute(project_dir / source, error).lexically_normal().generic_string();
    }
    configure.push_back("-DSLASH_GAME_SOURCES=" + sources);
    configure.push_back("-DSLASH_GAME_ICONS=" + icons.generic_string());
    // Arguments that pick the compilers and targets, shared with SDK packages' CMake projects.
    std::vector<std::string> toolchain_args;
    // Games with a providers.json call language models: main() installs engine/ai, and Android
    // games get the INTERNET permission.
    if (fs::exists(project_dir / ai::kConfigFile, error)) {
        configure.emplace_back("-DSLASH_GAME_AI=ON");
    } else {
        configure.emplace_back("-DSLASH_GAME_AI=OFF");
    }
    if (const audio::Settings& sound = project.value().audio; sound.enable_input) {
        // Games that record ask for the microphone: Info.plist's reason on macOS and iOS, the
        // RECORD_AUDIO permission on Android.
        configure.emplace_back("-DSLASH_GAME_MICROPHONE=ON");
        configure.push_back("-DSLASH_GAME_MICROPHONE_USAGE=" + sound.input_usage_description);
    }
    if (platform == "macos") {
        toolchain_args.push_back("-DCMAKE_OSX_DEPLOYMENT_TARGET=" +
                                 std::string(kMacosDeploymentTarget));
        // Release builds run on Apple silicon and Intel Macs; debug builds only on this one.
        if (!debug) {
            toolchain_args.emplace_back("-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64");
        }
    }
    if (toolchain) {
        toolchain_args.push_back("-DCMAKE_TOOLCHAIN_FILE=" + toolchain->generic_string());
    }
    configure.insert(configure.end(), toolchain_args.begin(), toolchain_args.end());

    platform::ProcessOptions process;
    process.log_path = log_path.string();
    if (environment("CPM_SOURCE_CACHE").empty()) {
        const fs::path engine_cache = engine_dir / ".cpm-cache";
        process.environment.emplace_back(
            "CPM_SOURCE_CACHE",
            fs::exists(engine_cache, error)
                ? engine_cache.string()
                : fs::absolute(work / ".." / "cpm-cache", error).lexically_normal().string());
    }

    // Windows and Linux builds on other hosts, every Linux build and web builds with no emsdk use
    // downloaded toolchains.
    detail::CrossBuild cross;
    if (platform == "web" && !toolchain) {
        // Linux runs Emscripten's tools with its own Python, which must be 3.10 or newer.
        const std::vector<std::string> web = toolchains_for("web");
        if (std::ranges::find(web, std::string(kPython)) == web.end()) {
            auto python = platform::run_process(
                {"python3", "-c", "import sys; sys.exit(sys.version_info < (3, 10))"});
            if (!python || python.value().exit_code != 0) {
                return fail({"options",
                             "web exports need Python 3.10 or newer on PATH (python3) to run "
                             "Emscripten",
                             {}},
                            log_path);
            }
        }
    }
    if (!toolchain &&
        !detail::prepare_cross(platform, arch, work, log_path.string(), cross, failure)) {
        return fail(failure, log_path);
    }
    process.environment.insert(process.environment.end(), cross.environment.begin(),
                               cross.environment.end());

    // Ninja for a build folder configured for the first time, where there is one.
    const auto add_generator = [&](std::vector<std::string>& args, const fs::path& build) {
        if (!fs::exists(build / "CMakeCache.txt", error) && !ninja.empty()) {
            args.insert(args.begin() + 1, {"-G", "Ninja"});
            if (ninja != "ninja") {
                args.push_back("-DCMAKE_MAKE_PROGRAM=" + fs::path(ninja).generic_string());
            }
        }
    };

    // For one build of the game: builds the SDK packages' own CMake projects with its compilers,
    // fills in their paths for its {arch}, copies their licenses into the data and writes the
    // slash_game_sdk() calls export/CMakeLists.txt includes. Returns that file.
    json::Value sdk_entitlements = json::Object{};
    const auto link_sdks = [&](const fs::path& build, const std::vector<std::string>& extra,
                               std::string_view build_arch) -> std::optional<std::string> {
        std::vector<ResolvedSdk> resolved;
        std::vector<SdkProblem> problems;
        for (const Sdk* sdk : linked) {
            SdkPlaces places = sdk_places[sdk->name];
            if (!sdk->build.empty()) {
                const fs::path sdk_build = fs::absolute(build / "sdks" / sdk->name, error);
                places.build = sdk_build / "install";
                std::vector<std::string> args{"cmake",
                                              "-S",
                                              (sdk->folder / sdk->build).string(),
                                              "-B",
                                              (sdk_build / "build").string(),
                                              "-DCMAKE_BUILD_TYPE=" + config,
                                              "-DCMAKE_INSTALL_PREFIX=" +
                                                  places.build.generic_string()};
                add_generator(args, sdk_build / "build");
                args.insert(args.end(), toolchain_args.begin(), toolchain_args.end());
                args.insert(args.end(), extra.begin(), extra.end());
                for (const auto& [option, value] : sdk->build_options) {
                    std::string define = "-D" + option;
                    define += "=";
                    define += value;
                    args.push_back(std::move(define));
                }
                if (!step("sdks", args, process, failure) ||
                    !step("sdks",
                          {"cmake", "--build", (sdk_build / "build").string(), "--config", config,
                           "--parallel"},
                          process, failure) ||
                    !step(
                        "sdks",
                        {"cmake", "--install", (sdk_build / "build").string(), "--config", config},
                        process, failure)) {
                    failure.details["sdk"] = sdk->location;
                    return std::nullopt;
                }
            }
            resolved.push_back(resolve_sdk(*sdk, platform, build_arch, places, true, problems));
        }
        const json::Value plist = merge_sdk_entries(resolved, "plist", problems);
        const json::Value entitlements = merge_sdk_entries(resolved, "entitlements", problems);
        // The packages' services backends, registered when the game starts.
        const std::string register_source = sdk_register_source(resolved, problems);
        if (platform == "macos" || platform == "ios") {
            std::set<std::string, std::less<>> reserved =
                template_plist_keys(engine_dir / "export" / platform / "Info.plist.in");
            if (project.value().audio.enable_input) {
                reserved.emplace("NSMicrophoneUsageDescription");
            }
            for (const ResolvedSdk& sdk : resolved) {
                if (!sdk.plist.is_object()) {
                    continue;
                }
                for (const auto& [key, value] : sdk.plist.as_object()) {
                    if (reserved.contains(key)) {
                        problems.push_back(
                            {sdk.sdk->location,
                             "/platforms/" + platform + "/plist/" + json::pointer_escape(key),
                             "the engine sets " + key + " itself"});
                    }
                }
            }
        }
        if (!problems.empty()) {
            failure = sdk_failure("sdks", problems);
            return std::nullopt;
        }
        // License files ship with the game's data, in licenses/<package>/.
        for (const ResolvedSdk& sdk : resolved) {
            if (sdk.license_file.empty()) {
                continue;
            }
            const fs::path to =
                data / "licenses" / sdk.sdk->name / fs::path(sdk.license_file).filename();
            fs::create_directories(to.parent_path(), error);
            fs::copy_file(sdk.license_file, to, fs::copy_options::overwrite_existing, error);
            if (error) {
                failure = {"sdks",
                           "could not copy " + sdk.license_file +
                               " into the game's data: " + error.message(),
                           {}};
                return std::nullopt;
            }
        }
        sdk_entitlements = entitlements;
        const fs::path file = fs::absolute(build / "sdks.cmake", error);
        fs::create_directories(build, error);
        std::string register_file;
        if (!register_source.empty()) {
            const fs::path source = fs::absolute(build / "sdk_register.cpp", error);
            if (!platform::write_file(source.string(), register_source)) {
                failure = {"sdks", "could not write " + display(source), {}};
                return std::nullopt;
            }
            register_file = source.generic_string();
        }
        if (!platform::write_file(file.string(), sdk_cmake(resolved, plist, register_file))) {
            failure = {"sdks", "could not write " + display(file), {}};
            return std::nullopt;
        }
        return file.generic_string();
    };

    // Configures, builds and installs export/CMakeLists.txt in build, into prefix. build_arch is
    // what {arch} means in SDK packages' paths for this build.
    const auto build_into = [&](const fs::path& build, const std::vector<std::string>& extra,
                                const fs::path& prefix, bool strip, std::string_view build_arch) {
        std::vector<std::string> args = configure;
        args.insert(args.begin() + 1, {"-B", build.string()});
        args.insert(args.end(), extra.begin(), extra.end());
        add_generator(args, build);
        // Always passed, so a game that stops linking packages doesn't keep the last build's.
        std::string sdk_file;
        if (!linked.empty()) {
            std::optional<std::string> written = link_sdks(build, extra, build_arch);
            if (!written) {
                return false;
            }
            sdk_file = std::move(*written);
        }
        args.push_back("-DSLASH_GAME_SDKS=" + sdk_file);
        std::vector<std::string> install{
            "cmake",    "--install", build.string(),
            "--config", config,      "--component",
            "game",     "--prefix",  fs::absolute(prefix, error).string()};
        if (strip) {
            install.emplace_back("--strip");
        }
        return step("configure", args, process, failure) &&
               step("build", {"cmake", "--build", build.string(), "--config", config, "--parallel"},
                    process, failure) &&
               step("install", install, process, failure);
    };

    const std::string binary = project.value().binary_name();
    const fs::path package = work / "package";
    json::Value signed_with;
    fs::remove_all(package, error);
    if (android) {
        // One build per ABI, installed into one tree that becomes the APK.
        const fs::path tree = work / "android";
        fs::remove_all(tree, error);
        for (const std::string& abi : preset.android.abis) {
            if (!build_into(work / ("build-" + abi),
                            detail::android_configure_args(*android, abi, apk_info.package), tree,
                            !debug, abi)) {
                failure.details["abi"] = abi;
                return fail(failure, log_path);
            }
        }
        // The game's own resources: its adaptive icon.
        fs::copy(icons / "res", tree / "res",
                 fs::copy_options::recursive | fs::copy_options::overwrite_existing, error);
        if (error) {
            return fail({"icons", "could not add the icon to the APK: " + error.message(), {}},
                        log_path);
        }
        AndroidResources resources;
        if (!android_libraries.empty()) {
            auto added =
                add_android_libraries(android_libraries, tree, work / "aars", preset.android.abis,
                                      apk_info.package, kAndroidMinSdk, log_path.string());
            if (!added) {
                return fail({"sdks", added.error().message, {}}, log_path);
            }
            resources = std::move(added).value();
        }
        if (!detail::package_apk(*android, android_key, apk_info, tree, resources, work,
                                 package / (binary + ".apk"),
                                 detail::android_process(*android, process), failure)) {
            return fail(failure, log_path);
        }
        signed_with["release"] = android_key.release;
        signed_with["keystore"] = fs::path(android_key.keystore).filename().string();
    } else if (platform == "ios") {
        const fs::path app = package / (binary + ".app");
        if (!build_into(work / ("build-" + preset.ios.destination),
                        detail::ios_configure_args(preset.ios, debug, ios_bundle_id), package,
                        !debug, preset.ios.destination)) {
            return fail(failure, log_path);
        }
        if (!detail::compile_ios_icons(icons / "Assets.xcassets", app, preset.ios.destination, work,
                                       process, failure)) {
            return fail(failure, log_path);
        }
        // A device build's entitlements are its profile's, which must grant what SDKs need.
        if (!sdk_entitlements.as_object().empty() && !ios_signing.profile.empty() &&
            !merge_entitlements(ios_signing.entitlements, sdk_entitlements, true, {}, failure)) {
            return fail(failure, log_path);
        }
        if (!detail::sign_ios_app(ios_signing, app, process, failure)) {
            return fail(failure, log_path);
        }
        // Devices install .ipa files; the Simulator installs the app itself.
        if (ios_signing.device) {
            if (!detail::package_ipa(app, package / (binary + ".ipa"), work, process, failure)) {
                return fail(failure, log_path);
            }
            fs::remove_all(app, error);
        }
        signed_with["identity"] = ios_signing.identity_name.empty() ? std::string(kAdHocIdentity)
                                                                    : ios_signing.identity_name;
        if (!ios_signing.profile.empty()) {
            signed_with["profile"] = ios_signing.profile_name;
        }
    } else {
        const std::string sdk_arch = sdk_archs(preset).front();
        const bool built =
            cross.toolchain_file.empty()
                ? build_into(work / "build", {}, package, false, sdk_arch)
                : build_into(work / ("build-" + cross.build_name),
                             {"-DCMAKE_TOOLCHAIN_FILE=" + cross.toolchain_file.generic_string()},
                             package, false, sdk_arch);
        if (!built) {
            return fail(failure, log_path);
        }
        if (platform == "macos" && !sdk_entitlements.as_object().empty()) {
            const fs::path merged = fs::absolute(work / "sdk-entitlements.plist", error);
            if (!merge_entitlements(signing.entitlements, sdk_entitlements, false, merged,
                                    failure)) {
                return fail(failure, log_path);
            }
            signing.entitlements = merged;
        }
        if (!sign(preset, signing, package, binary, work, process, signed_with, failure)) {
            return fail(failure, log_path);
        }
    }
    if (!publish(package, output, work / "output.json", failure)) {
        return fail(failure, log_path);
    }

    fs::path executable;
    fs::path program;
    if (platform == "macos") {
        executable = output / (binary + ".app");
        program = executable / "Contents" / "MacOS" / binary;
    } else if (platform == "windows") {
        executable = output / (binary + ".exe");
        program = executable;
    } else if (platform == "linux") {
        executable = output / binary;
        program = executable;
    } else if (platform == "android") {
        executable = output / (binary + ".apk");
        program = executable;
    } else if (platform == "ios") {
        executable = output / (binary + (ios_signing.device ? ".ipa" : ".app"));
        program = executable;
    } else {
        executable = output / "index.html";
        program = output / (binary + ".js");
    }

    ExportResult result;
    result.ok = true;
    json::Value& out = result.json;
    out["preset"] = preset.name;
    out["platform"] = platform;
    out["output"] = display(output);
    out["executable"] = display(executable);
    out["bytes"] = folder_bytes(output);
    out["log"] = display(log_path);
    // The image the icons were made from: the project's file, or the engine's icon.
    out["icon"] =
        icon_images.value().source.empty() ? std::string("default") : icon_images.value().source;
    if (!signed_with.is_null()) {
        out["signed"] = std::move(signed_with);
    }
    if (!linked.empty()) {
        json::Array linked_sdks;
        for (const Sdk* sdk : linked) {
            json::Value item;
            item["name"] = sdk->name;
            item["version"] = sdk->version;
            item["license"] = sdk->license;
            linked_sdks.push_back(std::move(item));
        }
        out["sdks"] = std::move(linked_sdks);
    }
    if (android) {
        json::Value& info = out["android"];
        info["package"] = apk_info.package;
        info["version_code"] = apk_info.version_code;
        json::Array abis;
        for (const std::string& abi : preset.android.abis) {
            abis.emplace_back(abi);
        }
        info["abis"] = std::move(abis);
    }
    if (platform == "linux") {
        out["linux"]["arch"] = arch;
    }
    if (platform == "ios") {
        json::Value& info = out["ios"];
        info["bundle_id"] = ios_bundle_id;
        info["destination"] = preset.ios.destination;
    }
    if (options.test) {
        json::Value test;
        if (android) {
            test = detail::test_apk(*android, fs::absolute(program, error), apk_info.package,
                                    preset.android.abis, log_path);
        } else if (platform == "ios" && ios_signing.device) {
            test["skipped"] = "device builds don't run in the Simulator; export with "
                              "\"destination\": \"simulator\" to test";
        } else if (platform == "ios") {
            test = detail::test_ios(fs::absolute(program, error), ios_bundle_id,
                                    fs::absolute(work, error), log_path);
        } else {
            test = run_test(platform, arch, fs::absolute(output, error),
                            fs::absolute(program, error), log_path, cross.node);
        }
        if (const json::Value* passed = test.find("passed");
            passed != nullptr && !passed->as_bool()) {
            result.ok = false;
        }
        out["test"] = std::move(test);
    }
    const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - started;
    out["seconds"] = static_cast<double>(static_cast<std::int64_t>(elapsed.count() * 10.0)) / 10.0;
    return result;
}

} // namespace slash::exporter
