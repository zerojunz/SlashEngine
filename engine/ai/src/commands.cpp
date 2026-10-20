#include "slash/ai/commands.hpp"

#include "slash/ai/adapters.hpp"
#include "slash/ai/config.hpp"
#include "slash/command/mcp.hpp"
#include "slash/core/text.hpp"
#include "slash/platform/file.hpp"
#include "slash/platform/process.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slash::ai {
namespace {

using command::Capability;
using command::Reply;
using command::Session;

json::Value property(std::string_view type, std::string_view description) {
    json::Value schema;
    schema["type"] = type;
    schema["description"] = description;
    return schema;
}

json::Value string_array(std::string_view description) {
    json::Value schema = property("array", description);
    schema["items"]["type"] = "string";
    return schema;
}

json::Value params_schema(json::Object properties) {
    json::Value schema;
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["properties"] = std::move(properties);
    return schema;
}

json::Value message_schema() {
    json::Value text;
    text["type"] = "string";
    json::Value role;
    role["enum"] = json::Array{"system", "user", "assistant", "tool"};
    json::Value image = params_schema({
        {"media_type", property("string", "image/png, image/jpeg, image/gif or image/webp.")},
        {"data", property("string", "The image's bytes in base64.")},
    });
    image["required"] = json::Array{"media_type", "data"};
    json::Value images = property("array", "Images, in a user message.");
    images["items"] = std::move(image);
    json::Value call = params_schema({
        {"id", text},
        {"name", text},
        {"arguments", json::Value(json::Object{})},
    });
    call["required"] = json::Array{"id", "name"};
    json::Value calls = property("array", "Tool calls, in an assistant message.");
    calls["items"] = std::move(call);
    json::Value schema = params_schema({
        {"role", std::move(role)},
        {"text", text},
        {"images", std::move(images)},
        {"tool_calls", std::move(calls)},
        {"tool_call_id", property("string", "In a tool message: the call it answers.")},
        {"is_error", property("boolean", "In a tool message: the call failed.")},
        {"native", property("object", "As a reply's message returned it.")},
    });
    schema["required"] = json::Array{"role"};
    return schema;
}

// The media type of an image file, from its extension.
std::string_view media_type(std::string_view path) {
    const std::size_t dot = path.rfind('.');
    std::string extension(dot == std::string_view::npos ? std::string_view() : path.substr(dot));
    for (char& c : extension) {
        c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    }
    if (extension == ".png") {
        return "image/png";
    }
    if (extension == ".jpg" || extension == ".jpeg") {
        return "image/jpeg";
    }
    if (extension == ".gif") {
        return "image/gif";
    }
    if (extension == ".webp") {
        return "image/webp";
    }
    return {};
}

Reply failure_at(const Error& error, std::string path) {
    Reply reply = Reply::failure(error);
    reply.data["path"] = std::move(path);
    return reply;
}

Reply ai_providers(Session& session, const json::Value& /*params*/) {
    auto config = load_config(session.options().project_root);
    if (!config) {
        return Reply::failure(config.error());
    }
    json::Value out;
    out["providers"] = json::Array{};
    for (const auto& [name, provider] : config.value().providers) {
        json::Value item;
        item["name"] = name;
        item["type"] = provider.type;
        item["model"] = provider.model;
        item["url"] = provider.url.empty() ? std::string(default_url(provider.type)) : provider.url;
        if (!provider.api_key_env.empty()) {
            item["api_key_env"] = provider.api_key_env;
            item["key_set"] = !platform::environment_variable(provider.api_key_env).empty();
        }
        out["providers"].push_back(std::move(item));
    }
    out["roles"] = json::Object{};
    for (const auto& [name, role] : config.value().roles) {
        json::Value item;
        item["provider"] = role.provider;
        if (!role.model.empty()) {
            item["model"] = role.model;
        }
        out["roles"][name] = std::move(item);
    }
    const std::string override_mode = platform::environment_variable(std::string(kReplayVariable));
    if (config.value().replay) {
        out["replay"]["file"] = config.value().replay->file;
        out["replay"]["mode"] = override_mode.empty()
                                    ? std::string(to_string(config.value().replay->mode))
                                    : override_mode;
    } else if (!override_mode.empty()) {
        out["replay"]["mode"] = override_mode;
    }
    return Reply::success(std::move(out));
}

Reply ai_complete(Session& session, const json::Value& params) {
    const std::string& root = session.options().project_root;
    const json::Value* prompt = params.find("prompt");
    const json::Value* messages = params.find("messages");
    if (prompt == nullptr && messages == nullptr) {
        return Reply::failure(command::code::kInvalidParams,
                              "ai.complete needs a prompt or messages");
    }

    Request request;
    if (const json::Value* system = params.find("system"); system != nullptr) {
        request.messages.push_back({.role = Role::System, .text = system->as_string()});
    }
    if (messages != nullptr) {
        for (std::size_t i = 0; i < messages->size(); ++i) {
            auto message = message_from_json(messages->as_array()[i]);
            if (!message) {
                return failure_at(message.error(), "/messages/" + std::to_string(i));
            }
            request.messages.push_back(std::move(message).value());
        }
    }
    if (prompt != nullptr) {
        Message user{.role = Role::User, .text = prompt->as_string()};
        if (const json::Value* images = params.find("images"); images != nullptr) {
            for (std::size_t i = 0; i < images->size(); ++i) {
                const std::string& path = images->as_array()[i].as_string();
                const std::string at = "/images/" + std::to_string(i);
                const std::string_view type = media_type(path);
                if (type.empty()) {
                    return failure_at(
                        make_error(ErrorCode::InvalidArgument,
                                   "\"" + path + "\" is not a .png, .jpg, .gif or .webp file"),
                        at);
                }
                auto bytes = platform::read_file_in(root, path);
                if (!bytes) {
                    return failure_at(bytes.error(), at);
                }
                user.images.push_back({std::string(type), std::move(bytes).value()});
            }
        }
        request.messages.push_back(std::move(user));
    }
    std::map<std::string, std::string> commands; // tool name to command
    if (const json::Value* tools = params.find("tools"); tools != nullptr) {
        std::vector<std::string> names;
        for (const json::Value& name : tools->as_array()) {
            names.push_back(name.as_string());
        }
        auto specs = command_tools(session.commands(), names);
        if (!specs) {
            return failure_at(specs.error(), "/tools");
        }
        request.tools = std::move(specs).value();
        for (const std::string& name : names) {
            commands[command::mcp::tool_name(name)] = name;
        }
    }
    if (const json::Value* schema = params.find("output_schema"); schema != nullptr) {
        request.output_schema = *schema;
    }
    if (const json::Value* tokens = params.find("max_tokens"); tokens != nullptr) {
        request.params.max_tokens = tokens->as_int();
    }
    if (const json::Value* temperature = params.find("temperature"); temperature != nullptr) {
        request.params.temperature = temperature->as_double();
    }
    if (const json::Value* stop = params.find("stop"); stop != nullptr) {
        for (const json::Value& item : stop->as_array()) {
            request.params.stop.push_back(item.as_string());
        }
    }
    if (const json::Value* seed = params.find("seed"); seed != nullptr) {
        request.params.seed = seed->as_int();
    }

    auto config = load_config(root);
    if (!config) {
        return Reply::failure(config.error());
    }
    const json::Value* name = params.find("role");
    auto opened = open(config.value(), name != nullptr ? name->as_string() : std::string(),
                       {.project_root = root, .write_replays = session.options().write_files});
    if (!opened) {
        return name != nullptr ? failure_at(opened.error(), "/role")
                               : Reply::failure(opened.error());
    }
    auto response = opened.value().provider->complete(request);
    if (!response) {
        return Reply::failure(response.error());
    }

    const Response& reply = response.value();
    json::Value out;
    out["text"] = reply.message.text;
    if (!reply.message.tool_calls.empty()) {
        for (const ToolCall& call : reply.message.tool_calls) {
            json::Value item;
            item["id"] = call.id;
            const auto found = commands.find(call.name);
            item["command"] = found != commands.end() ? found->second : call.name;
            item["arguments"] = call.arguments;
            out["tool_calls"].push_back(std::move(item));
        }
        // Passed back in messages, with each call's result as a tool message, to continue.
        out["message"] = to_json(reply.message);
    }
    if (!reply.output.is_null()) {
        out["output"] = reply.output;
    }
    out["stop"] = to_string(reply.stop);
    out["usage"]["input_tokens"] = reply.usage.input_tokens;
    out["usage"]["output_tokens"] = reply.usage.output_tokens;
    out["provider"] = opened.value().provider_name;
    out["model"] = reply.model.empty() ? opened.value().model : reply.model;
    if (reply.replayed) {
        out["replayed"] = true;
    }
    return Reply::success(std::move(out));
}

} // namespace

Result<std::vector<ToolSpec>> command_tools(const command::Registry& registry,
                                            std::span<const std::string> names) {
    std::vector<ToolSpec> tools;
    for (const std::string& name : names) {
        const command::Command* found = registry.find(name);
        if (found == nullptr) {
            std::vector<std::string_view> all;
            for (const command::Command& command : registry.commands()) {
                all.push_back(command.name);
            }
            std::string message = "no command named \"" + name + "\"";
            if (const std::string_view hint = closest_match(name, all); !hint.empty()) {
                message += "; did you mean \"" + std::string(hint) + "\"?";
            }
            return make_error(ErrorCode::NotFound, std::move(message));
        }
        tools.push_back({.name = command::mcp::tool_name(found->name),
                         .description = found->description,
                         .parameters = found->params});
    }
    return tools;
}

std::vector<Message> run_tool_calls(command::Session& session, const Message& message) {
    std::map<std::string, std::string, std::less<>> commands;
    for (const command::Command& command : session.commands().commands()) {
        commands.emplace(command::mcp::tool_name(command.name), command.name);
    }
    std::vector<Message> results;
    for (const ToolCall& call : message.tool_calls) {
        Message result{.role = Role::Tool, .tool_call_id = call.id};
        const auto found = commands.find(call.name);
        if (found == commands.end()) {
            result.text = "no command named " + call.name;
            result.is_error = true;
            results.push_back(std::move(result));
            continue;
        }
        const Reply reply = session.call(found->second, call.arguments);
        if (reply.ok) {
            result.text = json::write(reply.value, json::Style::Compact);
        } else {
            json::Value error;
            error["code"] = reply.code;
            error["message"] = reply.message;
            if (!reply.data.is_null()) {
                error["data"] = reply.data;
            }
            result.text = json::write(error, json::Style::Compact);
            result.is_error = true;
        }
        results.push_back(std::move(result));
    }
    return results;
}

void register_commands(command::Registry& registry) {
    registry.add({
        .name = "ai.providers",
        .description = "Lists the language models providers.json configures: each provider's "
                       "type, model, URL and whether its key is set, the roles and the replay "
                       "file. Never returns keys.",
        .capabilities = {Capability::Read},
        .effect = command::Effect::Read,
        .params = params_schema({}),
        .handler = ai_providers,
    });

    json::Value tokens = property("integer", "The most tokens to reply with.");
    tokens["minimum"] = 1;
    json::Value temperature = property("number", "Sampling temperature, where the model takes "
                                                 "one.");
    temperature["minimum"] = 0;
    json::Value messages = property("array", "The conversation so far, before the prompt. To "
                                             "continue after tool calls, pass back the reply's "
                                             "message and a tool message per call.");
    messages["items"] = message_schema();
    registry.add({
        .name = "ai.complete",
        .description = "Asks a language model from providers.json. Returns its text, the tool "
                       "calls it asks for (not run), the output parsed as output_schema says, "
                       "token usage and whether the reply was replayed from the replay file.",
        .capabilities = {Capability::Read, Capability::Network},
        .effect = command::Effect::Read,
        .params = params_schema({
            {"role", property("string", "A role or provider from providers.json. Default: the "
                                        "\"default\" role, or the only provider.")},
            {"prompt", property("string", "What to ask, as a user message after messages.")},
            {"system", property("string", "Instructions, put first.")},
            {"images", string_array("Project paths of images sent with the prompt: .png, .jpg, "
                                    ".gif or .webp.")},
            {"messages", std::move(messages)},
            {"tools", string_array("Commands the model may ask to call, such as world.query.")},
            {"output_schema", property("object", "A JSON Schema the reply must match.")},
            {"max_tokens", std::move(tokens)},
            {"temperature", std::move(temperature)},
            {"stop", string_array("Strings that end the reply.")},
            {"seed", property("integer", "For repeatable replies, where the model takes one.")},
        }),
        .handler = ai_complete,
    });
}

} // namespace slash::ai
