// A shopkeeper who talks through a language model behind ai::Provider.
//
// A stand-in for a local model server, as llama.cpp's or Ollama's OpenAI-compatible one, answers
// on the loopback interface. providers.json names it and gives the "shopkeeper" role its
// instructions. The game asks on a thread while it keeps ticking, gets an answer checked against
// a JSON Schema, lets the model put an item in the world with an engine command, then asks again
// with the server gone and gets the same answers from the replay file, as CI does.

#include "slash/ai/commands.hpp"
#include "slash/ai/config.hpp"
#include "slash/ai/provider.hpp"
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/platform/file.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <httplib.h>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace slash;

void print(const std::string& line) {
    std::printf("%s\n", line.c_str());
}

std::vector<std::string> words(const std::string& text) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t space = text.find(' ', start + 1);
        const std::size_t end = space == std::string::npos ? text.size() : space;
        out.push_back(text.substr(start, end - start));
        start = end;
    }
    return out;
}

// Answers chat completions as a model would: the tool call when offered tools, a JSON value when
// asked for one, and text otherwise, streamed a word at a time when the request streams.
class ModelServer {
public:
    ModelServer() {
        server_.Post("/v1/chat/completions",
                     [this](const httplib::Request& request, httplib::Response& response) {
                         ++requests_;
                         answer(json::parse(request.body).value(), request.body.size(), response);
                     });
        port_ = server_.bind_to_any_port("127.0.0.1");
        thread_ = std::thread([this] { server_.listen_after_bind(); });
        server_.wait_until_ready();
    }

    ~ModelServer() { stop(); }

    ModelServer(const ModelServer&) = delete;
    ModelServer& operator=(const ModelServer&) = delete;

    void stop() {
        if (thread_.joinable()) {
            server_.stop();
            thread_.join();
        }
    }

    [[nodiscard]] std::string url() const {
        return "http://127.0.0.1:" + std::to_string(port_) + "/v1";
    }
    [[nodiscard]] int requests() const { return requests_; }

private:
    static void answer(const json::Value& body, std::size_t size, httplib::Response& response) {
        const json::Array& messages = body.find("messages")->as_array();
        const std::string last_role = messages.back().find("role")->as_string();
        json::Value message;
        message["role"] = "assistant";
        std::string finish = "stop";
        if (last_role == "tool") {
            message["content"] = "There you are, a fresh loaf on the counter. Three coins, please.";
        } else if (body.find("tools") != nullptr) {
            json::Value call;
            call["id"] = "call_1";
            call["type"] = "function";
            call["function"]["name"] = "entity_create";
            call["function"]["arguments"] =
                R"({"id":"bread_loaf","components":{"Transform2D":{"position":[40,12]}}})";
            message["content"] = nullptr;
            message["tool_calls"].push_back(std::move(call));
            finish = "tool_calls";
        } else if (body.find("response_format") != nullptr) {
            message["content"] = R"({"mood":"cheerful","price":3})";
        } else {
            message["content"] = "Fresh bread, honey cakes and, on Fridays, plum tarts.";
        }
        json::Value usage;
        usage["prompt_tokens"] = static_cast<std::int64_t>(size / 4);
        usage["completion_tokens"] = static_cast<std::int64_t>(
            message["content"].is_string() ? words(message["content"].as_string()).size() : 12);

        const json::Value* stream = body.find("stream");
        if (stream == nullptr || !stream->as_bool()) {
            json::Value reply;
            reply["model"] = "llama3.2";
            json::Value choice;
            choice["finish_reason"] = finish;
            choice["message"] = std::move(message);
            reply["choices"].push_back(std::move(choice));
            reply["usage"] = std::move(usage);
            response.set_content(json::write(reply, json::Style::Compact), "application/json");
            return;
        }
        // Server-sent events: a delta per word, then the finish reason and usage, then [DONE].
        std::string events;
        for (const std::string& word : words(message["content"].as_string())) {
            json::Value chunk;
            json::Value choice;
            choice["delta"]["content"] = word;
            chunk["choices"].push_back(std::move(choice));
            events += "data: " + json::write(chunk, json::Style::Compact) + "\n\n";
        }
        json::Value last;
        json::Value choice;
        choice["delta"] = json::Object{};
        choice["finish_reason"] = finish;
        last["choices"].push_back(std::move(choice));
        last["model"] = "llama3.2";
        last["usage"] = std::move(usage);
        events += "data: " + json::write(last, json::Style::Compact) + "\n\ndata: [DONE]\n\n";
        response.set_chunked_content_provider(
            "text/event-stream", [events](std::size_t /*offset*/, httplib::DataSink& sink) {
                sink.write(events.data(), events.size());
                sink.done();
                return true;
            });
    }

    httplib::Server server_;
    int port_ = 0;
    std::atomic<int> requests_ = 0;
    std::thread thread_;
};

ai::Request ask(std::string question) {
    ai::Request request;
    request.messages.push_back({.role = ai::Role::User, .text = std::move(question)});
    return request;
}

} // namespace

int main() {
    ModelServer server;
    // The URL holds the server's port, so the file is written here; a game commits its own.
    const std::string config_text = R"({
  "providers": {
    "local": {"type": "openai", "model": "llama3.2", "url": ")" +
                                    server.url() + R"("}
  },
  "roles": {
    "shopkeeper": {
      "provider": "local",
      "system": "You are Mira, who keeps the bakery in Hollow Vale. Answer in one sentence.",
      "temperature": 0.7
    }
  },
  "replay": {"file": "tests/ai.replay.jsonl"}
}
)";
    (void)platform::write_file("providers.json", config_text);

    const ai::Config config = ai::load_config(".").value();
    std::map<std::string, std::string> environment;
    ai::OpenOptions options;
    options.environment = [&environment](const std::string& name) {
        const auto found = environment.find(name);
        return found != environment.end() ? found->second : std::string();
    };
    const ai::Opened opened = ai::open(config, "shopkeeper", options).value();
    print("role shopkeeper: provider " + opened.provider_name + ", " + opened.type + " model " +
          opened.model);

    // 1. The game keeps ticking while the model answers, showing call.text() as it grows.
    print("\n> What do you sell?");
    {
        const ai::Call call(opened.provider, ask("What do you sell?"), /*stream=*/true);
        while (!call.done()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1)); // a tick and a frame
        }
        const ai::Response& reply = call.result()->value();
        print("Mira: " + reply.message.text);
        print("stop " + std::string(ai::to_string(reply.stop)) + ", " +
              std::to_string(reply.usage.input_tokens) + " tokens in, " +
              std::to_string(reply.usage.output_tokens) + " out");
    }

    // 2. An answer the game can use, checked against a JSON Schema.
    print("\n> How are you today, and what does a loaf cost?");
    ai::Request typed = ask("How are you today, and what does a loaf cost?");
    typed.output_schema = json::parse(R"({"type": "object", "required": ["mood", "price"],
        "properties": {"mood": {"enum": ["cheerful", "tired", "cross"]},
                       "price": {"type": "integer", "minimum": 1}}})")
                              .value();
    const ai::Response mood = opened.provider->complete(typed).value();
    print("output " + json::write(mood.output, json::Style::Compact));

    // 3. The model acts through the engine's own commands, as an agent does over MCP.
    command::Registry commands;
    command::register_builtin_commands(commands);
    world::ComponentRegistry components;
    world::register_builtin_components(components);
    command::SessionOptions session_options;
    session_options.capture_logs = false;
    command::Session session(components, commands, session_options);

    print("\n> Put a loaf on the counter, please.");
    ai::Request act = ask("Put a loaf on the counter, please.");
    act.tools = ai::command_tools(commands, std::array<std::string, 1>{"entity.create"}).value();
    const ai::Response called = opened.provider->complete(act).value();
    for (const ai::ToolCall& call : called.message.tool_calls) {
        print("tool call " + call.name + " " + json::write(call.arguments, json::Style::Compact));
    }
    act.messages.push_back(called.message);
    for (ai::Message& result : ai::run_tool_calls(session, called.message)) {
        print("result " + result.text);
        act.messages.push_back(std::move(result));
    }
    print("Mira: " + opened.provider->complete(act).value().message.text);
    print("world " +
          json::write(session.call("world.query", json::Object{}).value, json::Style::Compact));
    print("model requests: " + std::to_string(server.requests()));

    // 4. The server is gone; SLASH_AI_REPLAY=replay answers from the recordings or fails.
    server.stop();
    environment["SLASH_AI_REPLAY"] = "replay";
    const ai::Opened offline = ai::open(config, "shopkeeper", options).value();
    const std::string replay = platform::read_file("tests/ai.replay.jsonl").value();
    std::size_t recordings = 0;
    for (const char c : replay) {
        recordings += c == '\n' ? 1 : 0;
    }
    print("\nreplaying " + std::to_string(recordings) + " recordings with no model");
    const ai::Response again = offline.provider->complete(ask("What do you sell?")).value();
    print("Mira: " + again.message.text + (again.replayed ? " (replayed)" : ""));
    const ai::Response typed_again = offline.provider->complete(typed).value();
    print("output " + json::write(typed_again.output, json::Style::Compact) +
          (typed_again.replayed ? " (replayed)" : ""));
    auto unrecorded = offline.provider->complete(ask("Do you sell cheese?"));
    const std::string message = unrecorded.error().message;
    print("new question: " + message.substr(0, message.find(" (key")));
    return 0;
}
