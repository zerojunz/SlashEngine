#include "slash/exporter/publish.hpp"

#include "steps.hpp"

#include "slash/core/text.hpp"
#include "slash/exporter/export.hpp"
#include "slash/exporter/presets.hpp"
#include "slash/platform/file.hpp"
#include "slash/platform/process.hpp"
#include "slash/runtime/project.hpp"

#include <algorithm>
#include <filesystem>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace slash::exporter {
namespace {

namespace fs = std::filesystem;
using detail::display;

// The longest itch.io project and channel names: generous, but enough to catch a pasted URL.
constexpr std::int64_t kMaxStoreName = 128;

json::Value property(std::string_view type, std::string_view description) {
    json::Value schema;
    schema["type"] = type;
    schema["description"] = description;
    return schema;
}

// One build a publish pushes: a preset and the channel it goes to.
struct Build {
    Preset preset;
    std::string channel;
    fs::path output; // relative to the project, as presets name it
};

PublishResult fail(std::string step, std::string message, json::Value details = {},
                   json::Array pushed = {}) {
    PublishResult result;
    json::Value& error = result.json["error"];
    if (details.is_object()) {
        error = std::move(details);
    }
    error["step"] = std::move(step);
    error["message"] = std::move(message);
    if (!pushed.empty()) {
        result.json["pushed"] = std::move(pushed);
    }
    return result;
}

// Where an export of the preset puts its game, relative to the project.
fs::path output_of(const Preset& preset) {
    if (!preset.output.empty()) {
        return {preset.output};
    }
    return fs::path("build") / "export" / preset.name;
}

std::string store_names() {
    std::string names;
    for (const std::string_view store : kStores) {
        names += names.empty() ? "" : ", ";
        names += store;
    }
    return names;
}

// The command that pushes a build's output, with butler as its program.
std::vector<std::string> push_command(const std::string& butler, const std::string& project,
                                      const Build& build, const std::string& version,
                                      const fs::path& output) {
    return {butler,          "push",  display(output), project + ":" + build.channel,
            "--userversion", version, "--if-changed"};
}

json::Value command_json(const std::vector<std::string>& command) {
    json::Array out;
    for (const std::string& arg : command) {
        out.emplace_back(arg);
    }
    return out;
}

// The builds a publish to itch.io pushes, in order.
Result<std::vector<Build>> itch_builds(const ItchStore& itch, const Presets& presets,
                                       const std::vector<std::string>& targets) {
    std::vector<std::string> names = targets;
    if (names.empty()) {
        for (const auto& [name, channel] : itch.channels) {
            names.push_back(name);
        }
    }
    if (names.empty()) {
        for (const auto& [name, preset] : presets) {
            if (!itch_channel(preset.platform).empty()) {
                names.push_back(name);
            }
        }
    }
    if (names.empty()) {
        return make_error(ErrorCode::InvalidArgument,
                          "nothing to publish: name presets or platforms, or list them in " +
                              std::string(kPresetsFile) + "'s stores.itch.channels");
    }
    std::vector<Build> builds;
    std::set<std::string, std::less<>> channels;
    for (const std::string& name : names) {
        auto preset = find_preset(presets, name);
        if (!preset) {
            return preset.error();
        }
        Build build;
        build.preset = std::move(preset).value();
        if (const auto it = itch.channels.find(name); it != itch.channels.end()) {
            build.channel = it->second;
        } else {
            build.channel = itch_channel(build.preset.platform, build.preset.linux_options.arch);
        }
        if (build.channel.empty()) {
            return make_error(ErrorCode::InvalidArgument, "\"" + name + "\" builds for " +
                                                              build.preset.platform +
                                                              ", which itch.io doesn't take");
        }
        if (!channels.insert(build.channel).second) {
            return make_error(ErrorCode::InvalidArgument,
                              "two builds go to the itch.io channel \"" + build.channel +
                                  "\", so the second would replace the first; give \"" + name +
                                  "\" a channel of its own in stores.itch.channels");
        }
        build.output = output_of(build.preset);
        builds.push_back(std::move(build));
    }
    return builds;
}

// butler's program: SLASH_BUTLER_PATH's, else butler on PATH.
Result<std::string> find_butler() {
    if (std::string configured = detail::environment(kButlerPath); !configured.empty()) {
        if (!platform::exists(configured)) {
            return make_error(ErrorCode::NotFound, std::string(kButlerPath) + " names \"" +
                                                       configured + "\", which doesn't exist");
        }
        return configured;
    }
    if (std::string found = detail::find_on_path("butler"); !found.empty()) {
        return found;
    }
    return make_error(ErrorCode::NotFound,
                      "uploading to itch.io needs butler, itch.io's upload tool, which isn't on "
                      "PATH: install it from https://itch.io/docs/butler/, or set " +
                          std::string(kButlerPath) + " to its program");
}

// Whether text is 1 or more of the characters allowed, each a letter, a digit or one of extra.
bool name_of(std::string_view text, std::string_view extra) {
    return !text.empty() && std::ranges::all_of(text, [extra](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               extra.find(c) != std::string_view::npos;
    });
}

// export_presets.json's stores.itch, or null.
const json::Value* itch_section(const json::Value& presets) {
    const json::Value* stores = presets.find("stores");
    return stores == nullptr ? nullptr : stores->find("itch");
}

} // namespace

json::Value stores_schema() {
    json::Value project = property("string", "The game's itch.io project as \"user/game\", from "
                                             "its address https://user.itch.io/game.");
    project["maxLength"] = kMaxStoreName;
    json::Value channel = property("string", "Letters, digits, '.', '_' or '-'.");
    channel["maxLength"] = kMaxStoreName;
    json::Value channels =
        property("object", "Channels by preset, or by platform for its defaults, such as "
                           "{\"mac-release\": \"mac\"}. butler tags a channel's downloads by "
                           "the platform in its name. Default: every preset but iOS ones, on a "
                           "channel named after its platform (mac, windows, linux, linux-arm64, "
                           "html5, android).");
    channels["additionalProperties"] = std::move(channel);
    json::Value itch;
    itch["type"] = "object";
    itch["description"] = "Where publish itch sends builds, through butler. butler reads its "
                          "API key from " +
                          std::string(kButlerApiKey) + ".";
    itch["additionalProperties"] = false;
    itch["required"] = json::Array{"project"};
    itch["properties"]["project"] = std::move(project);
    itch["properties"]["channels"] = std::move(channels);

    json::Value stores;
    stores["type"] = "object";
    stores["description"] = "Where slash-cli publish sends the presets' builds, by store.";
    stores["additionalProperties"] = false;
    stores["properties"]["itch"] = std::move(itch);
    return stores;
}

std::string itch_channel(std::string_view platform, std::string_view arch) {
    if (platform == "macos") {
        return "mac";
    }
    if (platform == "windows") {
        return "windows";
    }
    if (platform == "linux") {
        return arch == kLinuxArm64 ? "linux-arm64" : "linux";
    }
    if (platform == "web") {
        return "html5";
    }
    if (platform == "android") {
        return "android";
    }
    return {};
}

std::vector<json::ValidationError> validate_stores(const json::Value& presets) {
    std::vector<json::ValidationError> errors;
    const json::Value* itch = itch_section(presets);
    if (const json::Value* project = itch == nullptr ? nullptr : itch->find("project");
        project != nullptr) {
        const std::string& text = project->as_string();
        const std::size_t slash = text.find('/');
        if (slash == std::string::npos || !name_of(text.substr(0, slash), "_-") ||
            !name_of(text.substr(slash + 1), "_-")) {
            errors.push_back({"/stores/itch/project",
                              "expected \"user/game\", as in the game's address "
                              "https://user.itch.io/game"});
        }
    }
    const json::Value* channels = itch == nullptr ? nullptr : itch->find("channels");
    if (channels == nullptr) {
        return errors;
    }
    const json::Value* presets_section = presets.find("presets");
    for (const auto& [name, channel] : channels->as_object()) {
        const std::string pointer = "/stores/itch/channels/" + json::pointer_escape(name);
        const json::Value* preset =
            presets_section == nullptr ? nullptr : presets_section->find(name);
        if (!name_of(channel.as_string(), "._-")) {
            errors.push_back({pointer, "channel names are letters, digits, '.', '_' or '-'"});
        } else if (preset == nullptr && std::ranges::find(kPlatforms, name) == kPlatforms.end()) {
            errors.push_back({pointer, "no preset or platform called \"" + name + "\""});
        } else if ((preset != nullptr ? preset->find("platform")->as_string() : name) == "ios") {
            errors.push_back({pointer, "itch.io doesn't take iOS games"});
        }
    }
    return errors;
}

Stores parse_stores(const json::Value& presets) {
    Stores stores;
    if (const json::Value* itch = itch_section(presets); itch != nullptr) {
        ItchStore store;
        store.project = itch->find("project")->as_string();
        if (const json::Value* channels = itch->find("channels"); channels != nullptr) {
            for (const auto& [name, channel] : channels->as_object()) {
                store.channels.emplace(name, channel.as_string());
            }
        }
        stores.itch = std::move(store);
    }
    return stores;
}

PublishResult publish_game(const PublishOptions& options) {
    if (std::ranges::find(kStores, options.store) == kStores.end()) {
        std::string message = "no store called \"" + options.store + "\"";
        if (const std::string_view hint = closest_match(options.store, kStores); !hint.empty()) {
            message += "; did you mean \"" + std::string(hint) + "\"?";
        } else {
            message += "; publish knows " + store_names();
        }
        return fail("options", std::move(message));
    }
    const fs::path project_dir(options.project);
    const std::string presets_path = (project_dir / std::string(kPresetsFile)).string();
    if (!platform::exists(presets_path)) {
        return fail("options", "publishing needs " + std::string(kPresetsFile) + " with a stores." +
                                   options.store + " section saying where builds go");
    }
    auto text = platform::read_file(presets_path);
    if (!text) {
        return fail("options", text.error().message);
    }
    auto parsed = json::parse(text.value());
    if (!parsed) {
        return fail("options", std::string(kPresetsFile) + ": " + parsed.error().message);
    }
    auto presets = parse_presets(parsed.value());
    if (!presets) {
        return fail("options", presets.error().message);
    }
    const Stores stores = parse_stores(parsed.value());
    if (!stores.itch) {
        return fail("options", std::string(kPresetsFile) +
                                   " has no stores.itch: add {\"project\": \"user/game\"}, the "
                                   "game's itch.io project");
    }
    const ItchStore& itch = *stores.itch;
    auto builds = itch_builds(itch, presets.value(), options.targets);
    if (!builds) {
        return fail("options", builds.error().message);
    }
    std::string version = options.version;
    if (version.empty()) {
        auto project = runtime::load_project(options.project);
        if (!project) {
            return fail("options", project.error().message);
        }
        version = project.value().version;
    }

    std::string butler = "butler";
    if (options.upload) {
        auto found = find_butler();
        if (!found) {
            return fail("setup", found.error().message);
        }
        butler = std::move(found).value();
        if (detail::environment(kButlerApiKey).empty()) {
            return fail("setup", "uploading to itch.io needs butler's API key in " +
                                     std::string(kButlerApiKey) +
                                     ": make one at https://itch.io/user/settings/api-keys");
        }
    }

    PublishResult result;
    json::Value& out = result.json;
    out["store"] = options.store;
    out["project"] = itch.project;
    out["version"] = version;
    out["upload"] = options.upload;
    json::Array planned;
    for (const Build& build : builds.value()) {
        json::Value item;
        item["preset"] = build.preset.name;
        item["platform"] = build.preset.platform;
        item["channel"] = build.channel;
        item["target"] = itch.project + ":" + build.channel;
        item["output"] = display(build.output);
        item["command"] =
            command_json(push_command("butler", itch.project, build, version, build.output));
        planned.push_back(std::move(item));
    }
    if (!options.upload) {
        out["builds"] = std::move(planned);
        out["build"] = options.build;
        out["needs"] = json::Array{std::string("butler on PATH or in ") + std::string(kButlerPath),
                                   std::string(kButlerApiKey)};
        result.ok = true;
        return result;
    }

    const fs::path log_dir = project_dir / ".slash" / "publish";
    std::error_code error;
    fs::create_directories(log_dir, error);
    json::Array pushed;
    for (std::size_t i = 0; i < builds.value().size(); ++i) {
        const Build& build = builds.value()[i];
        json::Value& item = planned[i];
        if (options.build) {
            ExportOptions export_options;
            export_options.target = build.preset.name;
            export_options.project = options.project;
            export_options.engine = options.engine;
            const ExportResult exported = export_game(export_options);
            if (!exported.ok) {
                json::Value details;
                details["preset"] = build.preset.name;
                if (const json::Value* export_error = exported.json.find("error");
                    export_error != nullptr) {
                    details["export"] = *export_error;
                }
                return fail("export", "exporting \"" + build.preset.name + "\" failed",
                            std::move(details), std::move(pushed));
            }
            item["export"] = exported.json;
        }
        const fs::path output = project_dir / build.output;
        if (!fs::is_directory(output, error)) {
            json::Value details;
            details["preset"] = build.preset.name;
            return fail("push",
                        "there's no export of \"" + build.preset.name + "\" in " +
                            display(build.output) + ": export it first, or publish with build on",
                        std::move(details), std::move(pushed));
        }
        platform::ProcessOptions process;
        process.log_path = (log_dir / (options.store + "-" + build.preset.name + ".log")).string();
        process.tail_lines = 20;
        const std::vector<std::string> command =
            push_command(butler, itch.project, build, version, output);
        auto ran = platform::run_process(command, process);
        if (!ran || ran.value().exit_code != 0) {
            json::Value details;
            details["preset"] = build.preset.name;
            details["target"] = item.find("target")->as_string();
            details["log"] = display(fs::path(process.log_path));
            std::string message;
            if (!ran) {
                message = "butler couldn't be started: " + ran.error().message;
            } else {
                details["exit_code"] = ran.value().exit_code;
                details["tail"] = ran.value().tail;
                message = "butler couldn't push \"" + build.preset.name + "\" to " +
                          item.find("target")->as_string();
            }
            return fail("push", std::move(message), std::move(details), std::move(pushed));
        }
        json::Value push;
        push["log"] = display(fs::path(process.log_path));
        push["tail"] = ran.value().tail;
        item["push"] = std::move(push);
        pushed.emplace_back(item.find("target")->as_string());
    }
    out["builds"] = std::move(planned);
    result.ok = true;
    return result;
}

} // namespace slash::exporter
