#include "slash/exporter/export.hpp"

#include "android.hpp"
#include "steps.hpp"

#include "slash/core/log.hpp"
#include "slash/core/text.hpp"
#include "slash/exporter/android.hpp"
#include "slash/exporter/presets.hpp"
#include "slash/platform/file.hpp"
#include "slash/platform/process.hpp"
#include "slash/runtime/project.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/scene.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
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
using detail::step;

constexpr std::string_view kMacosDeploymentTarget = "11.0";
constexpr std::string_view kSceneSuffix = ".scene.json";

bool host_builds(std::string_view platform) {
    if (platform == "web" || platform == "android") {
        return true;
    }
#if defined(__APPLE__)
    return platform == "macos";
#elif defined(_WIN32)
    return platform == "windows";
#elif defined(__linux__)
    return platform == "linux";
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

// Skips folders an export must never ship: hidden ones (.git, .slash), build output and game
// sources.
bool skipped_folder(const fs::path& name) {
    const std::string text = name.string();
    return text.starts_with(".") || text == "build" || text == "game";
}

// Copies project.json and every scene into data, validating each scene on the way.
bool stage_data(const fs::path& project_dir, const runtime::Project& project, const fs::path& data,
                Failure& failure) {
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
    json::Array problems;
    bool found_main = false;
    auto it = fs::recursive_directory_iterator(project_dir, error);
    for (; !error && it != fs::recursive_directory_iterator(); it.increment(error)) {
        const fs::directory_entry& entry = *it;
        if (entry.is_directory(error)) {
            if (skipped_folder(entry.path().filename())) {
                it.disable_recursion_pending();
            }
            continue;
        }
        const std::string name = entry.path().filename().string();
        if (!name.ends_with(kSceneSuffix)) {
            continue;
        }
        const fs::path relative = entry.path().lexically_relative(project_dir);
        const std::string file = relative.generic_string();
        found_main = found_main || file == project.main_scene;
        auto text = platform::read_file(entry.path().string());
        std::vector<json::ValidationError> errors;
        if (!text) {
            errors.push_back({"", text.error().message});
        } else if (auto parsed = json::parse(text.value()); !parsed) {
            errors.push_back({"", parsed.error().message});
        } else {
            errors = world::validate_scene(components, parsed.value());
        }
        for (const json::ValidationError& problem : errors) {
            json::Value item;
            item["file"] = file;
            item["path"] = problem.path;
            item["message"] = problem.message;
            problems.push_back(std::move(item));
        }
        fs::create_directories((data / relative).parent_path(), error);
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

json::Value run_test(const std::string& platform, const fs::path& output, const fs::path& program,
                     const fs::path& log_path) {
    json::Value test;
    std::vector<std::string> args;
    if (platform == "web") {
        if (!program_runs("node")) {
            test["skipped"] = "Node.js is not on PATH";
            return test;
        }
        args = {"node", program.string()};
    } else if (platform == host_name()) {
        args = {program.string()};
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
        test["ticks"] = *summary->find("ticks");
        if (const json::Value* entities = summary->find("entities"); entities != nullptr) {
            test["entities"] = *entities;
        }
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

    const std::optional<std::string> engine = find_engine(options.engine);
    if (!engine) {
        return fail({"options",
                     "could not find the engine sources; pass --engine <folder> or set "
                     "SLASH_ENGINE_DIR to a SlashEngine checkout",
                     {}},
                    {});
    }
    std::optional<fs::path> toolchain;
    if (platform == "web") {
        toolchain = find_emscripten_toolchain();
        if (!toolchain) {
            return fail({"options",
                         "web exports need Emscripten; install emsdk and activate it (source "
                         "emsdk_env.sh) so EMSDK is set",
                         {}},
                        {});
        }
    }
    if (!program_runs("cmake")) {
        return fail({"options", "exports need CMake 3.25 or newer on PATH", {}}, {});
    }
    Failure failure;
    Signing signing;
    if (!prepare_signing(preset, project_dir, signing, failure)) {
        return fail(failure, {});
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

    std::error_code error;
    fs::create_directories(work, error);
    (void)platform::write_file(log_path.string(), "");

    if (!output_writable(output, work / "output.json", failure)) {
        return fail(failure, {});
    }
    log::info("export step", {{"step", "validate"}});
    const fs::path data = fs::absolute(work / "data", error);
    if (!stage_data(project_dir, project.value(), data, failure)) {
        return fail(failure, log_path);
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
    if (platform == "macos") {
        configure.push_back("-DCMAKE_OSX_DEPLOYMENT_TARGET=" + std::string(kMacosDeploymentTarget));
        // Release builds run on Apple silicon and Intel Macs; debug builds only on this one.
        if (!debug) {
            configure.emplace_back("-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64");
        }
    }
    if (toolchain) {
        configure.push_back("-DCMAKE_TOOLCHAIN_FILE=" + toolchain->generic_string());
    }

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

    // Configures, builds and installs export/CMakeLists.txt in build, into prefix.
    const auto build_into = [&](const fs::path& build, const std::vector<std::string>& extra,
                                const fs::path& prefix, bool strip) {
        std::vector<std::string> args = configure;
        args.insert(args.begin() + 1, {"-B", build.string()});
        args.insert(args.end(), extra.begin(), extra.end());
        if (!fs::exists(build / "CMakeCache.txt", error) && !ninja.empty()) {
            args.insert(args.begin() + 1, {"-G", "Ninja"});
            if (ninja != "ninja") {
                args.push_back("-DCMAKE_MAKE_PROGRAM=" + fs::path(ninja).generic_string());
            }
        }
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
                            !debug)) {
                failure.details["abi"] = abi;
                return fail(failure, log_path);
            }
        }
        if (!detail::package_apk(*android, android_key, apk_info, tree, work,
                                 package / (binary + ".apk"),
                                 detail::android_process(*android, process), failure)) {
            return fail(failure, log_path);
        }
        signed_with["release"] = android_key.release;
        signed_with["keystore"] = fs::path(android_key.keystore).filename().string();
    } else if (!build_into(work / "build", {}, package, false) ||
               !sign(preset, signing, package, binary, work, process, signed_with, failure)) {
        return fail(failure, log_path);
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
    if (!signed_with.is_null()) {
        out["signed"] = std::move(signed_with);
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
    if (options.test) {
        json::Value test = android
                               ? detail::test_apk(*android, fs::absolute(program, error),
                                                  apk_info.package, preset.android.abis, log_path)
                               : run_test(platform, fs::absolute(output, error),
                                          fs::absolute(program, error), log_path);
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
