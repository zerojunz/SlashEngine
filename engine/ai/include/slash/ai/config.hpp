#pragma once

#include "slash/ai/provider.hpp"
#include "slash/ai/replay.hpp"
#include "slash/ai/transport.hpp"
#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/result.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// providers.json, beside project.json: the providers a project can call, the role each feature
// uses, such as "dialogue" or "assistant", and the asset library's generators. Keys come from
// environment variables the file names, never from the file. $cli schema providers prints its
// JSON Schema.
namespace slash::ai {

inline constexpr std::string_view kConfigFile = "providers.json";

// The environment variable that overrides providers.json's replay mode: "replay", "record",
// "auto" or "off". CI sets "replay" so tests never call a model.
inline constexpr std::string_view kReplayVariable = "SLASH_AI_REPLAY";

struct ProviderConfig {
    std::string type; // an adapter type: "openai", "anthropic" or "gemini"
    std::string model;
    std::string url;         // "" for the vendor's own
    std::string api_key_env; // the variable holding the key; "" for none
    std::map<std::string, std::string> headers;
    Capabilities capabilities;
    std::optional<std::int64_t> max_tokens;
    std::string max_tokens_param = "max_tokens";
    int timeout_seconds = 600;
    int max_retries = 2;
};

// A role: the provider a feature uses, and what it adds to every request.
struct RoleConfig {
    std::string provider;
    std::string model;  // "" for the provider's
    std::string system; // instructions put before every request's messages
    Params params;      // used where a request sets none
};

// An image or sound generator for the asset library (slash/library/generator.hpp).
struct GeneratorConfig {
    std::string type;        // "test"
    std::string model;       // "" for the generator's default
    std::string url;         // "" for the vendor's own
    std::string api_key_env; // the variable holding the key; "" for none
    std::map<std::string, std::string> headers;
    std::string license; // the terms its outputs come under
    json::Value options; // sent with every request
    int timeout_seconds = 600;
    int max_retries = 2;
};

struct ReplayConfig {
    std::string file; // a project path
    ReplayMode mode = ReplayMode::Auto;
};

struct Config {
    std::map<std::string, ProviderConfig, std::less<>> providers;
    std::map<std::string, RoleConfig, std::less<>> roles;
    std::map<std::string, GeneratorConfig, std::less<>> generators;
    std::optional<ReplayConfig> replay;
};

[[nodiscard]] const json::Value& config_schema();

// Every problem in a providers.json document: schema errors, unknown adapter types and roles
// naming providers that don't exist, each with its JSON Pointer.
[[nodiscard]] std::vector<json::ValidationError> check_config(const json::Value& document);

// The config a providers.json document describes; errors name the first problem's pointer.
[[nodiscard]] Result<Config> parse_config(const json::Value& document);

// The project's providers.json; a project without one has no providers.
[[nodiscard]] Result<Config> load_config(std::string_view project_root);

struct OpenOptions {
    std::string project_root = ".";
    // null for http_transport().
    std::shared_ptr<Transport> transport{}; // NOLINT(readability-redundant-member-init)
    // Reads environment variables; null for the process's own.
    std::function<std::string(const std::string& name)>
        environment{}; // NOLINT(readability-redundant-member-init)
    // false records no replies, as sessions that run tests do.
    bool write_replays = true;
};

struct Opened {
    std::shared_ptr<Provider> provider;
    std::string provider_name;
    std::string type;
    std::string model;
};

// The provider for a role, or for a provider named directly: an adapter for its model with the
// role's instructions and params added to every request, answering from the replay file when the
// config has one. "" picks the "default" role, or the only provider. A missing key fails only
// when a request has to call the model, so replaying needs none.
[[nodiscard]] Result<Opened> open(const Config& config, std::string_view name,
                                  const OpenOptions& options = {});

} // namespace slash::ai
