#include "slash/exporter/presets.hpp"

#include "slash/core/text.hpp"
#include "slash/platform/file.hpp"
#include "slash/runtime/project.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <utility>

namespace slash::exporter {
namespace {

constexpr std::size_t kMaxPresetName = 64;

json::Value property(std::string_view type, std::string_view description) {
    json::Value schema;
    schema["type"] = type;
    schema["description"] = description;
    return schema;
}

json::Value object_schema(json::Object properties, std::string_view description) {
    json::Value schema;
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["description"] = description;
    schema["properties"] = std::move(properties);
    return schema;
}

bool valid_preset_name(std::string_view name) {
    if (name.empty() || name.size() > kMaxPresetName) {
        return false;
    }
    return std::ranges::all_of(name, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '-' || c == '_';
    });
}

bool is_platform(std::string_view name) {
    return std::ranges::find(kPlatforms, name) != kPlatforms.end();
}

// The last segment of a JSON Pointer, unescaped enough for the checks below.
std::string_view last_segment(std::string_view pointer) {
    const std::size_t slash = pointer.find_last_of('/');
    return slash == std::string_view::npos ? pointer : pointer.substr(slash + 1);
}

// The value a JSON Pointer names inside objects, or nullptr.
const json::Value* resolve(const json::Value& root, std::string_view pointer) {
    const json::Value* value = &root;
    std::size_t start = 1;
    while (value != nullptr && start <= pointer.size()) {
        std::size_t end = pointer.find('/', start);
        if (end == std::string_view::npos) {
            end = pointer.size();
        }
        std::string key;
        for (std::size_t i = start; i < end; ++i) {
            if (pointer[i] == '~' && i + 1 < end) {
                key += pointer[i + 1] == '1' ? '/' : '~';
                ++i;
            } else {
                key += pointer[i];
            }
        }
        value = value->is_object() ? value->find(key) : nullptr;
        start = end + 1;
    }
    return value;
}

bool looks_secret(std::string_view key) {
    constexpr std::array<std::string_view, 4> kWords{"password", "secret", "token", "private"};
    return std::ranges::any_of(
        kWords, [&](std::string_view word) { return key.find(word) != std::string_view::npos; });
}

// Rewrites schema errors that deserve a better message: a misspelled platform gets a "did you
// mean", and a field that looks like a secret says where secrets go instead.
void improve(json::ValidationError& error, const json::Value& presets) {
    const std::string_view key = last_segment(error.path);
    if (error.message.starts_with("unknown property") && looks_secret(key)) {
        error.message = "secrets are read from environment variables, never from " +
                        std::string(kPresetsFile) +
                        "; \"slash-cli schema presets\" lists the variables";
        return;
    }
    if (key == "platform" && error.message.starts_with("expected one of")) {
        const json::Value* value = resolve(presets, error.path);
        if (value != nullptr && value->is_string()) {
            error.message = "unknown platform \"" + value->as_string() + "\"";
            if (const std::string_view hint = closest_match(value->as_string(), kPlatforms);
                !hint.empty()) {
                error.message += "; did you mean \"" + std::string(hint) + "\"?";
            }
        }
    }
}

void check_preset(const std::string& name, const json::Value& preset,
                  std::vector<json::ValidationError>& errors) {
    const std::string path = "/presets/" + json::pointer_escape(name);
    const std::string& platform = preset.find("platform")->as_string();
    if (!valid_preset_name(name)) {
        errors.push_back({path, "preset names are 1 to 64 letters, digits, '-' or '_'"});
    }
    if (is_platform(name) && name != platform) {
        errors.push_back({path + "/platform", "a preset named after a platform must build for it; "
                                              "expected \"" +
                                                  name + "\""});
    }
    if (const json::Value* output = preset.find("output");
        output != nullptr && !runtime::is_project_path(output->as_string())) {
        errors.push_back({path + "/output", "expected a folder inside the project, separated by "
                                            "'/', with no \".\" or \"..\" parts"});
    }
    for (const std::string_view section : {"macos", "windows"}) {
        if (preset.find(section) != nullptr && platform != section) {
            errors.push_back({path + "/" + std::string(section),
                              "only a " + std::string(section) + " preset can have this section"});
        }
    }
    if (const json::Value* macos = preset.find("macos"); macos != nullptr) {
        const json::Value* identity = macos->find("codesign_identity");
        const bool ad_hoc = identity == nullptr || identity->as_string() == kAdHocIdentity;
        if (const json::Value* notarize = macos->find("notarize");
            notarize != nullptr && notarize->as_bool() && ad_hoc) {
            errors.push_back({path + "/macos/notarize",
                              "notarizing needs a Developer ID codesign_identity, not ad hoc "
                              "signing"});
        }
        if (const json::Value* entitlements = macos->find("entitlements");
            entitlements != nullptr && !runtime::is_project_path(entitlements->as_string())) {
            errors.push_back({path + "/macos/entitlements",
                              "expected a file inside the project, separated by '/', with no "
                              "\".\" or \"..\" parts"});
        }
    }
    if (const json::Value* windows = preset.find("windows"); windows != nullptr) {
        if (const json::Value* url = windows->find("timestamp_url");
            url != nullptr && !url->as_string().empty() &&
            !url->as_string().starts_with("http://") && !url->as_string().starts_with("https://")) {
            errors.push_back({path + "/windows/timestamp_url",
                              "expected an http:// or https:// URL, or \"\" for no timestamp"});
        }
    }
}

} // namespace

json::Value presets_schema() {
    json::Value identity = property(
        "string", "codesign identity from the keychain, e.g. \"Developer ID Application: Example "
                  "(ABCDE12345)\". \"-\" signs ad hoc (the default), which only runs on other "
                  "Macs after the player allows it.");
    identity["minLength"] = 1;
    identity["default"] = std::string(kAdHocIdentity);
    json::Value notarize = property(
        "boolean", "Submit the app to Apple's notary service and staple the ticket. Needs a "
                   "Developer ID identity and an App Store Connect API key in " +
                       std::string(kAppleApiKeyPath) + " (the .p8 file), " +
                       std::string(kAppleApiKeyId) + " and " + std::string(kAppleApiIssuer) + ".");
    notarize["default"] = false;
    json::Value macos = object_schema(
        {
            {"codesign_identity", std::move(identity)},
            {"entitlements",
             property("string", "Entitlements .plist to sign with, relative to the project.")},
            {"notarize", std::move(notarize)},
        },
        "How to sign the app bundle. Only in macos presets.");

    json::Value sign =
        property("boolean", "Sign the executable with signtool, using the .pfx certificate in " +
                                std::string(kWindowsCertificatePath) + " and its password in " +
                                std::string(kWindowsCertificatePassword) + ".");
    sign["default"] = false;
    json::Value timestamp = property("string", "RFC 3161 timestamp server, or \"\" for none.");
    timestamp["default"] = std::string(kDefaultTimestampUrl);
    json::Value windows =
        object_schema({{"sign", std::move(sign)}, {"timestamp_url", std::move(timestamp)}},
                      "How to sign the executable. Only in windows presets.");

    json::Value platform = property("string", "The platform this preset builds for.");
    json::Array platforms;
    for (const std::string_view name : kPlatforms) {
        platforms.emplace_back(name);
    }
    platform["enum"] = std::move(platforms);
    json::Value debug = property("boolean", "Debug build instead of Release.");
    debug["default"] = false;

    json::Value preset = object_schema(
        {
            {"platform", std::move(platform)},
            {"output", property("string", "Output folder relative to the project. Default: "
                                          "build/export/<preset name>.")},
            {"debug", std::move(debug)},
            {"macos", std::move(macos)},
            {"windows", std::move(windows)},
        },
        "One way to export the game.");
    preset["required"] = json::Array{"platform"};

    json::Value names;
    names["type"] = "string";
    names["minLength"] = 1;
    names["maxLength"] = kMaxPresetName;

    json::Value presets;
    presets["type"] = "object";
    presets["description"] = "Presets by name: 1 to 64 letters, digits, '-' or '_'. A preset "
                             "named after a platform replaces that platform's defaults.";
    presets["propertyNames"] = std::move(names);
    presets["additionalProperties"] = std::move(preset);

    json::Value format;
    format["const"] = kPresetsFormat;

    json::Value schema;
    schema["$schema"] = "https://json-schema.org/draft/2020-12/schema";
    schema["title"] = "SlashEngine export presets";
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["required"] = json::Array{"format", "presets"};
    schema["properties"]["format"] = std::move(format);
    schema["properties"]["presets"] = std::move(presets);
    return schema;
}

std::vector<json::ValidationError> validate_presets(const json::Value& presets) {
    std::vector<json::ValidationError> errors = json::validate(presets, presets_schema());
    if (!errors.empty()) {
        for (json::ValidationError& error : errors) {
            improve(error, presets);
        }
        return errors;
    }
    for (const auto& [name, preset] : presets.find("presets")->as_object()) {
        check_preset(name, preset, errors);
    }
    return errors;
}

Result<Presets> parse_presets(const json::Value& presets) {
    if (const auto errors = validate_presets(presets); !errors.empty()) {
        const json::ValidationError& first = errors.front();
        return make_error(ErrorCode::InvalidArgument,
                          std::string(kPresetsFile) + " " +
                              (first.path.empty() ? std::string("root") : first.path) + ": " +
                              first.message);
    }
    Presets out;
    for (const auto& [name, value] : presets.find("presets")->as_object()) {
        Preset preset;
        preset.name = name;
        preset.platform = value.find("platform")->as_string();
        if (const json::Value* output = value.find("output"); output != nullptr) {
            preset.output = output->as_string();
        }
        if (const json::Value* debug = value.find("debug"); debug != nullptr) {
            preset.debug = debug->as_bool();
        }
        if (const json::Value* macos = value.find("macos"); macos != nullptr) {
            if (const json::Value* identity = macos->find("codesign_identity");
                identity != nullptr) {
                preset.macos.codesign_identity = identity->as_string();
            }
            if (const json::Value* entitlements = macos->find("entitlements");
                entitlements != nullptr) {
                preset.macos.entitlements = entitlements->as_string();
            }
            if (const json::Value* notarize = macos->find("notarize"); notarize != nullptr) {
                preset.macos.notarize = notarize->as_bool();
            }
        }
        if (const json::Value* windows = value.find("windows"); windows != nullptr) {
            if (const json::Value* sign = windows->find("sign"); sign != nullptr) {
                preset.windows.sign = sign->as_bool();
            }
            if (const json::Value* url = windows->find("timestamp_url"); url != nullptr) {
                preset.windows.timestamp_url = url->as_string();
            }
        }
        out.emplace(name, std::move(preset));
    }
    return out;
}

Result<Presets> load_presets(std::string_view folder) {
    const std::string path = runtime::join_path(folder, kPresetsFile);
    if (!platform::exists(path)) {
        return Presets{};
    }
    auto text = platform::read_file(path);
    if (!text) {
        return text.error();
    }
    auto parsed = json::parse(text.value());
    if (!parsed) {
        return make_error(ErrorCode::InvalidArgument,
                          std::string(kPresetsFile) + ": " + parsed.error().message);
    }
    return parse_presets(parsed.value());
}

Result<Preset> find_preset(const Presets& presets, std::string_view target) {
    if (const auto it = presets.find(target); it != presets.end()) {
        return it->second;
    }
    if (is_platform(target)) {
        Preset preset;
        preset.name = std::string(target);
        preset.platform = std::string(target);
        return preset;
    }
    std::vector<std::string_view> names(kPlatforms.begin(), kPlatforms.end());
    for (const auto& [name, preset] : presets) {
        names.emplace_back(name);
    }
    std::string message = "no preset or platform called \"" + std::string(target) + "\"";
    if (const std::string_view hint = closest_match(target, names); !hint.empty()) {
        message += "; did you mean \"" + std::string(hint) + "\"?";
    } else {
        message +=
            "; use a preset from " + std::string(kPresetsFile) + " or macos, windows, linux or web";
    }
    return make_error(ErrorCode::InvalidArgument, std::move(message));
}

} // namespace slash::exporter
