// A model checks new dialogue against the storyboard, and only findings it can back up are kept.
//
// A stand-in for a model server answers on the loopback interface, so the example needs no
// network or key. Like a real model, it can be wrong: its answer has two findings that quote the
// dialogue and one that quotes words the dialogue never says, which storyboard.review drops.

#include "slash/ai/commands.hpp"
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/platform/file.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <httplib.h>
#include <string>
#include <thread>
#include <utility>

namespace {

using namespace slash;

void print(const std::string& line) {
    std::printf("%s\n", line.c_str());
}

// What the stand-in answers, as a model asked for storyboard.review's JSON would.
constexpr const char* kFindings = R"({"findings": [
  {"item": 1, "quote": "My sister Oren", "entry": "character/oren",
   "problem": "Oren is Mira's brother, not her sister.", "confidence": "high"},
  {"item": 2, "quote": "I took the ferry up to Hightown", "entry": "character/mira",
   "problem": "Mira has never left Lowtown.", "confidence": "high"},
  {"item": 2, "quote": "Vell sold me the ticket", "entry": "character/vell",
   "problem": "Vell only works the river.", "confidence": "low"}]})";

// Answers each chat completion with kFindings and keeps the last prompt it was sent.
class ModelServer {
public:
    ModelServer() {
        server_.Post("/v1/chat/completions", [this](const httplib::Request& request,
                                                    httplib::Response& response) {
            const json::Value body = json::parse(request.body).value();
            prompt_ = body.find("messages")->as_array()[1].find("content")->as_string();
            json::Value reply;
            reply["model"] = "llama3.2";
            json::Value choice;
            choice["finish_reason"] = "stop";
            choice["message"]["role"] = "assistant";
            choice["message"]["content"] = kFindings;
            reply["choices"].push_back(std::move(choice));
            reply["usage"]["prompt_tokens"] = 420;
            reply["usage"]["completion_tokens"] = 96;
            response.set_content(json::write(reply, json::Style::Compact), "application/json");
        });
        port_ = server_.bind_to_any_port("127.0.0.1");
        thread_ = std::thread([this] { server_.listen_after_bind(); });
        server_.wait_until_ready();
    }

    ~ModelServer() {
        server_.stop();
        thread_.join();
    }

    ModelServer(const ModelServer&) = delete;
    ModelServer& operator=(const ModelServer&) = delete;

    [[nodiscard]] std::string url() const {
        return "http://127.0.0.1:" + std::to_string(port_) + "/v1";
    }
    [[nodiscard]] const std::string& prompt() const { return prompt_; }

private:
    httplib::Server server_;
    int port_ = 0;
    std::string prompt_;
    std::thread thread_;
};

} // namespace

int main() {
    ModelServer server;
    // The URL holds the server's port, so the file is written here; a game commits its own.
    (void)platform::write_file("providers.json", R"({"providers": {"local": {"type": "openai", )"
                                                 R"("model": "llama3.2", "url": ")" +
                                                     server.url() + "\"}}}\n");

    world::ComponentRegistry components;
    world::register_builtin_components(components);
    command::Registry commands;
    command::register_builtin_commands(commands);
    ai::register_commands(commands);
    command::SessionOptions options;
    options.capture_logs = false;
    command::Session session(components, commands, options);

    // 1. The deterministic checks first: references, glossary words and StoryRef IDs.
    print("storyboard.check " + json::write(session.call("storyboard.check", json::Object{}).value,
                                            json::Style::Compact));

    // 2. The model reviews a scene's text and a dialogue file against the facts they touch.
    const json::Value params =
        json::parse(R"({"paths": ["scenes/tavern.scene.json", "dialogue/act1.txt"]})").value();
    const command::Reply reviewed = session.call("storyboard.review", params);
    print("\nstoryboard.review " + json::write(params, json::Style::Compact));
    print(json::write(reviewed.value));

    // What the model was sent: each fact by ID (the names the dialogue mentions, the scene's own
    // entry and the tone that applies everywhere), then each numbered item.
    print("\nthe model read:");
    const std::string& prompt = server.prompt();
    std::size_t start = 0;
    bool first_list = true;
    while (start < prompt.size()) {
        const std::size_t end = std::min(prompt.find('\n', start), prompt.size());
        const std::string line = prompt.substr(start, end - start);
        if (line.starts_with("[{")) {
            const json::Value facts = json::parse(line).value();
            std::string ids;
            for (const json::Value& fact : facts.as_array()) {
                ids += (ids.empty() ? "" : ", ") + fact.find("id")->as_string();
            }
            print(std::string(first_list ? "  facts: " : "  entries they name: ") + ids);
            first_list = false;
        } else if (line.starts_with("[")) {
            print("  " + line);
        }
        start = end + 1;
    }

    return 0;
}
