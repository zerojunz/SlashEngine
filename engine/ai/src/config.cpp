#include "slash/ai/config.hpp"

#include "slash/ai/adapters.hpp"
#include "slash/core/text.hpp"
#include "slash/platform/file.hpp"
#include "slash/platform/process.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slash::ai {
namespace {

constexpr std::string_view kSchema = R"({
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "title": "providers.json",
  "description": "The language models a project calls and the role each feature uses. Keys come from environment variables, never from this file.",
  "type": "object",
  "additionalProperties": false,
  "properties": {
    "providers": {
      "description": "Providers by name.",
      "type": "object",
      "additionalProperties": {
        "type": "object",
        "additionalProperties": false,
        "required": ["type", "model"],
        "properties": {
          "type": {
            "description": "openai: OpenAI's chat completions, also served by Ollama, llama.cpp, vLLM, LM Studio and OpenRouter. anthropic: Anthropic's Messages API. gemini: Google's Gemini API.",
            "enum": ["anthropic", "gemini", "openai"]
          },
          "model": {"description": "The model's ID, as the provider names it.", "type": "string", "minLength": 1},
          "url": {"description": "The API's base URL, such as http://localhost:11434/v1 for Ollama. Default: the vendor's.", "type": "string"},
          "api_key_env": {"description": "The environment variable holding the API key. Leave out for servers that take none.", "type": "string", "minLength": 1},
          "headers": {"description": "Headers sent with every request. Never put a key here.", "type": "object", "additionalProperties": {"type": "string"}},
          "capabilities": {
            "description": "What the model can do; each defaults to true. Without json_schema_output, an output schema goes in the prompt.",
            "type": "object",
            "additionalProperties": false,
            "properties": {
              "tools": {"type": "boolean"},
              "json_schema_output": {"type": "boolean"},
              "vision": {"type": "boolean"},
              "streaming": {"type": "boolean"},
              "context_tokens": {"description": "The model's context window, if known.", "type": "integer", "minimum": 0}
            }
          },
          "max_tokens": {"description": "Sent when a request sets none. Anthropic's default is 16000.", "type": "integer", "minimum": 1},
          "max_tokens_param": {"description": "openai only: the name the server takes max_tokens under. Default: max_tokens.", "enum": ["max_tokens", "max_completion_tokens"]},
          "timeout_seconds": {"description": "How long to wait for a reply. Default: 600.", "type": "integer", "minimum": 1},
          "max_retries": {"description": "Retries after a rate limit or server error. Default: 2.", "type": "integer", "minimum": 0, "maximum": 10}
        }
      }
    },
    "roles": {
      "description": "What each feature uses, such as dialogue or assistant: a provider's name, or an object. The role \"default\" is used when none is named.",
      "type": "object",
      "additionalProperties": {
        "anyOf": [
          {"type": "string", "minLength": 1},
          {
            "type": "object",
            "additionalProperties": false,
            "required": ["provider"],
            "properties": {
              "provider": {"type": "string", "minLength": 1},
              "model": {"description": "In place of the provider's model.", "type": "string", "minLength": 1},
              "system": {"description": "Instructions put before every request.", "type": "string"},
              "max_tokens": {"type": "integer", "minimum": 1},
              "temperature": {"type": "number", "minimum": 0},
              "stop": {"type": "array", "items": {"type": "string"}},
              "seed": {"type": "integer"}
            }
          }
        ]
      }
    },
    "replay": {
      "description": "Recorded replies, so tests run offline. SLASH_AI_REPLAY (replay, record, auto or off) overrides the mode.",
      "type": "object",
      "additionalProperties": false,
      "required": ["file"],
      "properties": {
        "file": {"description": "A JSON lines file in the project, such as tests/ai.replay.jsonl.", "type": "string", "minLength": 1},
        "mode": {"description": "replay: only from the file. record: call the model every time and record. auto (default): replay what the file has, record the rest.", "enum": ["replay", "record", "auto"]}
      }
    }
  }
})";

std::string role_provider(const json::Value& role) {
    if (role.is_string()) {
        return role.as_string();
    }
    const json::Value* provider = role.find("provider");
    return provider != nullptr && provider->is_string() ? provider->as_string() : std::string();
}

// Adds a role's instructions and params to every request.
class RoleProvider final : public Provider {
public:
    RoleProvider(std::shared_ptr<Provider> inner, RoleConfig role)
        : inner_(std::move(inner)), role_(std::move(role)) {}

    [[nodiscard]] const Capabilities& capabilities() const override {
        return inner_->capabilities();
    }

    Result<Response> complete(const Request& request, const StreamSink& sink,
                              const CancelFlag* cancel) override {
        Request full = request;
        if (!role_.system.empty()) {
            full.messages.insert(full.messages.begin(),
                                 Message{.role = Role::System, .text = role_.system});
        }
        if (!full.params.max_tokens) {
            full.params.max_tokens = role_.params.max_tokens;
        }
        if (!full.params.temperature) {
            full.params.temperature = role_.params.temperature;
        }
        if (full.params.stop.empty()) {
            full.params.stop = role_.params.stop;
        }
        if (!full.params.seed) {
            full.params.seed = role_.params.seed;
        }
        return inner_->complete(full, sink, cancel);
    }

private:
    std::shared_ptr<Provider> inner_;
    RoleConfig role_;
};

// Fails every request with the same error, for a provider that can't be called.
class FailingProvider final : public Provider {
public:
    FailingProvider(Capabilities capabilities, Error error)
        : capabilities_(capabilities), error_(std::move(error)) {}

    [[nodiscard]] const Capabilities& capabilities() const override { return capabilities_; }

    Result<Response> complete(const Request& /*request*/, const StreamSink& /*sink*/,
                              const CancelFlag* /*cancel*/) override {
        return error_;
    }

private:
    Capabilities capabilities_;
    Error error_;
};

} // namespace

const json::Value& config_schema() {
    static const json::Value schema = json::parse(kSchema).value();
    return schema;
}

std::vector<json::ValidationError> check_config(const json::Value& document) {
    std::vector<json::ValidationError> errors = json::validate(document, config_schema());
    if (!errors.empty()) {
        return errors;
    }
    const json::Value* providers = document.find("providers");
    std::vector<std::string> names;
    if (providers != nullptr) {
        for (const auto& [name, provider] : providers->as_object()) {
            names.push_back(name);
        }
    }
    if (const json::Value* roles = document.find("roles"); roles != nullptr) {
        for (const auto& [name, role] : roles->as_object()) {
            const std::string provider = role_provider(role);
            if (std::ranges::find(names, provider) != names.end()) {
                continue;
            }
            std::string message = "no provider named \"" + provider + "\"";
            if (const std::string_view hint = closest_match(provider, names); !hint.empty()) {
                message += "; did you mean \"" + std::string(hint) + "\"?";
            }
            errors.push_back(
                {"/roles/" + json::pointer_escape(name) + (role.is_string() ? "" : "/provider"),
                 std::move(message)});
        }
    }
    if (const json::Value* replay = document.find("replay"); replay != nullptr) {
        const std::string& file = replay->find("file")->as_string();
        if (!is_project_path(file)) {
            errors.push_back({"/replay/file", "\"" + file +
                                                  "\" is not a path inside the project, such as "
                                                  "tests/ai.replay.jsonl"});
        }
    }
    return errors;
}

Result<Config> parse_config(const json::Value& document) {
    if (const std::vector<json::ValidationError> errors = check_config(document); !errors.empty()) {
        const json::ValidationError& first = errors.front();
        return make_error(ErrorCode::InvalidArgument,
                          std::string(kConfigFile) + (first.path.empty() ? "" : " " + first.path) +
                              ": " + first.message);
    }
    Config config;
    if (const json::Value* providers = document.find("providers"); providers != nullptr) {
        for (const auto& [name, value] : providers->as_object()) {
            ProviderConfig provider;
            provider.type = value.find("type")->as_string();
            provider.model = value.find("model")->as_string();
            if (const json::Value* url = value.find("url"); url != nullptr) {
                provider.url = url->as_string();
            }
            if (const json::Value* env = value.find("api_key_env"); env != nullptr) {
                provider.api_key_env = env->as_string();
            }
            if (const json::Value* headers = value.find("headers"); headers != nullptr) {
                for (const auto& [header, text] : headers->as_object()) {
                    provider.headers[header] = text.as_string();
                }
            }
            if (const json::Value* caps = value.find("capabilities"); caps != nullptr) {
                const auto flag = [caps](std::string_view key, bool& out) {
                    if (const json::Value* member = caps->find(key); member != nullptr) {
                        out = member->as_bool();
                    }
                };
                flag("tools", provider.capabilities.tools);
                flag("json_schema_output", provider.capabilities.json_schema_output);
                flag("vision", provider.capabilities.vision);
                flag("streaming", provider.capabilities.streaming);
                if (const json::Value* tokens = caps->find("context_tokens"); tokens != nullptr) {
                    provider.capabilities.context_tokens = tokens->as_int();
                }
            }
            if (const json::Value* tokens = value.find("max_tokens"); tokens != nullptr) {
                provider.max_tokens = tokens->as_int();
            }
            if (const json::Value* param = value.find("max_tokens_param"); param != nullptr) {
                provider.max_tokens_param = param->as_string();
            }
            if (const json::Value* timeout = value.find("timeout_seconds"); timeout != nullptr) {
                provider.timeout_seconds = static_cast<int>(timeout->as_int());
            }
            if (const json::Value* retries = value.find("max_retries"); retries != nullptr) {
                provider.max_retries = static_cast<int>(retries->as_int());
            }
            config.providers.emplace(name, std::move(provider));
        }
    }
    if (const json::Value* roles = document.find("roles"); roles != nullptr) {
        for (const auto& [name, value] : roles->as_object()) {
            RoleConfig role;
            role.provider = role_provider(value);
            if (value.is_object()) {
                if (const json::Value* model = value.find("model"); model != nullptr) {
                    role.model = model->as_string();
                }
                if (const json::Value* system = value.find("system"); system != nullptr) {
                    role.system = system->as_string();
                }
                if (const json::Value* tokens = value.find("max_tokens"); tokens != nullptr) {
                    role.params.max_tokens = tokens->as_int();
                }
                if (const json::Value* temperature = value.find("temperature");
                    temperature != nullptr) {
                    role.params.temperature = temperature->as_double();
                }
                if (const json::Value* stop = value.find("stop"); stop != nullptr) {
                    for (const json::Value& item : stop->as_array()) {
                        role.params.stop.push_back(item.as_string());
                    }
                }
                if (const json::Value* seed = value.find("seed"); seed != nullptr) {
                    role.params.seed = seed->as_int();
                }
            }
            config.roles.emplace(name, std::move(role));
        }
    }
    if (const json::Value* replay = document.find("replay"); replay != nullptr) {
        ReplayConfig out;
        out.file = replay->find("file")->as_string();
        if (const json::Value* mode = replay->find("mode"); mode != nullptr) {
            out.mode = replay_mode_from_string(mode->as_string()).value_or(ReplayMode::Auto);
        }
        config.replay = std::move(out);
    }
    return config;
}

Result<Config> load_config(std::string_view project_root) {
    std::string path(project_root);
    if (!path.empty() && path.back() != '/') {
        path += '/';
    }
    path += kConfigFile;
    if (!platform::exists(path)) {
        return Config{};
    }
    auto text = platform::read_file(path);
    if (!text) {
        return text.error();
    }
    auto document = json::parse(text.value());
    if (!document) {
        return make_error(ErrorCode::InvalidArgument,
                          std::string(kConfigFile) + ": " + document.error().message);
    }
    return parse_config(document.value());
}

Result<Opened> open(const Config& config, std::string_view name, const OpenOptions& options) {
    const RoleConfig* role = nullptr;
    std::string provider_name;
    if (name.empty()) {
        if (const auto found = config.roles.find("default"); found != config.roles.end()) {
            role = &found->second;
        } else if (config.providers.size() == 1) {
            provider_name = config.providers.begin()->first;
        } else if (config.providers.empty()) {
            return make_error(ErrorCode::NotFound,
                              "the project has no providers; add them to providers.json (slash-cli "
                              "schema providers describes it)");
        } else {
            return make_error(ErrorCode::InvalidArgument,
                              "providers.json has several providers and no \"default\" role; name "
                              "a role or provider");
        }
    } else if (const auto found = config.roles.find(name); found != config.roles.end()) {
        role = &found->second;
    } else if (config.providers.contains(name)) {
        provider_name = std::string(name);
    } else {
        std::vector<std::string> names;
        names.reserve(config.roles.size() + config.providers.size());
        for (const auto& [role_name, unused] : config.roles) {
            names.push_back(role_name);
        }
        for (const auto& [provider, unused] : config.providers) {
            names.push_back(provider);
        }
        std::string message = "no role or provider named \"" + std::string(name) + "\"";
        if (names.empty()) {
            message += "; the project has no providers.json";
        } else if (const std::string_view hint = closest_match(name, names); !hint.empty()) {
            message += "; did you mean \"" + std::string(hint) + "\"?";
        }
        return make_error(ErrorCode::NotFound, std::move(message));
    }
    if (role != nullptr) {
        provider_name = role->provider;
    }
    const auto found = config.providers.find(provider_name);
    if (found == config.providers.end()) {
        return make_error(ErrorCode::NotFound,
                          "providers.json has no provider named \"" + provider_name + "\"");
    }
    const ProviderConfig& settings = found->second;
    const auto environment = [&options](const std::string& variable) {
        return options.environment ? options.environment(variable)
                                   : platform::environment_variable(variable);
    };

    Opened opened;
    opened.provider_name = provider_name;
    opened.type = settings.type;
    opened.model = role != nullptr && !role->model.empty() ? role->model : settings.model;

    AdapterOptions adapter;
    adapter.url = settings.url;
    adapter.model = opened.model;
    adapter.headers.assign(settings.headers.begin(), settings.headers.end());
    adapter.capabilities = settings.capabilities;
    adapter.max_tokens = settings.max_tokens;
    adapter.max_tokens_param = settings.max_tokens_param;
    adapter.timeout_seconds = settings.timeout_seconds;
    adapter.max_retries = settings.max_retries;
    adapter.transport = options.transport;
    std::shared_ptr<Provider> provider;
    if (!settings.api_key_env.empty()) {
        adapter.api_key = environment(settings.api_key_env);
        if (adapter.api_key.empty()) {
            provider = std::make_shared<FailingProvider>(
                settings.capabilities,
                make_error(ErrorCode::PermissionDenied,
                           "the provider \"" + provider_name + "\" reads its key from " +
                               settings.api_key_env + ", which is not set"));
        }
    }
    if (provider == nullptr) {
        auto made = make_adapter(settings.type, std::move(adapter));
        if (!made) {
            return made.error();
        }
        provider = std::move(made).value();
    }

    const std::string override_mode = environment(std::string(kReplayVariable));
    if (!override_mode.empty() && override_mode != "off" &&
        !replay_mode_from_string(override_mode)) {
        return make_error(ErrorCode::InvalidArgument, std::string(kReplayVariable) + " is \"" +
                                                          override_mode +
                                                          "\"; use replay, record, auto or off");
    }
    if (override_mode != "off") {
        if (config.replay) {
            // Errors name the file as the project does when the project is the current folder.
            std::string path = options.project_root == "." ? "" : options.project_root;
            if (!path.empty() && path.back() != '/') {
                path += '/';
            }
            path += config.replay->file;
            provider = std::make_shared<ReplayProvider>(ReplayProvider::Options{
                .path = std::move(path),
                .model = opened.type + "/" + opened.model,
                .mode = replay_mode_from_string(override_mode).value_or(config.replay->mode),
                .inner = std::move(provider),
                .write = options.write_replays,
            });
        } else if (!override_mode.empty()) {
            provider = std::make_shared<FailingProvider>(
                settings.capabilities,
                make_error(ErrorCode::InvalidArgument,
                           std::string(kReplayVariable) + " is \"" + override_mode +
                               "\", but providers.json names no replay file"));
        }
    }
    if (role != nullptr) {
        provider = std::make_shared<RoleProvider>(std::move(provider), *role);
    }
    opened.provider = std::move(provider);
    return opened;
}

} // namespace slash::ai
