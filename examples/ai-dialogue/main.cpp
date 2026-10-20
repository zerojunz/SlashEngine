// A baker who talks through a language model, from gameplay code. Pressing "Talk" asks the
// "baker" role with ai::ask, and the reply arrives as an ai_reply event at a later tick, which
// the dialogue system shows in a label and keeps for the next question. The game never waits on
// the model.
//
// A stand-in for a local model server answers on the loopback interface. The game is played as
// an agent plays it, through a session: ui.press, step, events.read and entity.get. Sessions wait
// for each reply, so it arrives at the same tick on every run. Then the server is gone, and a
// session without the network capability plays the same conversation from the replay file.

#include "slash/ai/provider.hpp"
#include "slash/ai/world.hpp"
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/core/reflect.hpp"
#include "slash/platform/file.hpp"
#include "slash/ui/ui.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <httplib.h>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace game {

// Someone the player talks to: the questions the player asks in turn, the request being
// answered, and the conversation so far, kept with the world so saving and undo keep it too.
struct Dialogue {
    std::vector<std::string> questions;
    std::string request;
    std::vector<slash::json::Value> messages; // as ai::to_json writes them
};
SLASH_COMPONENT(Dialogue, questions, request, messages)

} // namespace game

namespace {

namespace ai = slash::ai;
namespace command = slash::command;
namespace json = slash::json;
namespace ui = slash::ui;
namespace world = slash::world;

// Answers chat completions as a model would, streaming its reply a word at a time: what is good
// today, then the price of two of whatever it said, which it knows only from the conversation.
class ModelServer {
public:
    ModelServer() {
        server_.Post("/v1/chat/completions",
                     [this](const httplib::Request& request, httplib::Response& response) {
                         ++requests_;
                         answer(json::parse(request.body).value(), response);
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
    static void answer(const json::Value& body, httplib::Response& response) {
        const json::Array& messages = body.find("messages")->as_array();
        bool said_cakes = false;
        for (const json::Value& message : messages) {
            said_cakes = said_cakes ||
                         (message.find("role")->as_string() == "assistant" &&
                          message.find("content")->as_string().find("cakes") != std::string::npos);
        }
        const std::string& question = messages.back().find("content")->as_string();
        std::string text = "I only bake, I'm afraid.";
        if (question == "What's good today?") {
            text = "Honey cakes, still warm from the oven.";
        } else if (question == "How much for two?") {
            text = said_cakes ? "Two honey cakes for three coins, friend." : "Two of what?";
        }
        // Server-sent events: a delta per word, then the finish reason and usage, then [DONE].
        std::string events;
        std::size_t words = 0;
        for (std::size_t start = 0; start < text.size(); ++words) {
            const std::size_t space = text.find(' ', start + 1);
            const std::size_t end = space == std::string::npos ? text.size() : space;
            json::Value chunk;
            json::Value choice;
            choice["delta"]["content"] = text.substr(start, end - start);
            chunk["choices"].push_back(std::move(choice));
            events += "data: " + json::write(chunk, json::Style::Compact) + "\n\n";
            start = end;
        }
        json::Value last;
        json::Value choice;
        choice["delta"] = json::Object{};
        choice["finish_reason"] = "stop";
        last["choices"].push_back(std::move(choice));
        last["model"] = "llama3.2";
        last["usage"]["prompt_tokens"] = static_cast<std::int64_t>(messages.size() * 12);
        last["usage"]["completion_tokens"] = static_cast<std::int64_t>(words);
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

// Asks the next question when "Talk" is pressed, and shows each reply when its event comes.
void dialogue(world::World& game_world, double /*tick_seconds*/) {
    auto* talk_ptr = game_world.ecs().try_get<game::Dialogue>(game_world.find("mira"));
    auto* speech_ptr = game_world.ecs().try_get<world::Label>(game_world.find("speech"));
    if (talk_ptr == nullptr || speech_ptr == nullptr) {
        return;
    }
    game::Dialogue& talk = *talk_ptr;
    world::Label& speech = *speech_ptr;
    for (const world::Event& event : game_world.events()) {
        if (talk.request.empty() || event.id != talk.request) {
            continue;
        }
        if (event.kind == "ai_reply") {
            talk.messages.push_back(*event.data.find("message"));
            speech.text = "Mira: " + event.data.find("text")->as_string();
        } else if (event.kind == "ai_failed") {
            talk.messages.pop_back(); // unanswered, so it is asked again at the next press
            speech.text = "Mira says nothing (" + event.data.find("message")->as_string() + ")";
        }
        talk.request.clear();
    }
    const std::size_t asked = talk.messages.size() / 2;
    if (!ui::was_pressed(game_world, game_world.find("talk")) || !talk.request.empty() ||
        asked >= talk.questions.size()) {
        return;
    }
    ai::Request request;
    for (const json::Value& message : talk.messages) {
        request.messages.push_back(ai::message_from_json(message).value());
    }
    const ai::Message question{.role = ai::Role::User, .text = talk.questions[asked]};
    request.messages.push_back(question);
    auto id = ai::ask(game_world, "baker", std::move(request));
    if (!id) {
        speech.text = "Mira can't hear you (" + id.error().message + ")";
        return;
    }
    talk.request = id.value();
    talk.messages.push_back(ai::to_json(question));
    speech.text = "You: " + question.text;
}

// Prints the call as an agent would make it, then the reply.
void call(command::Session& session, std::string_view name, std::string_view params) {
    const json::Value value = json::parse(params).value();
    std::printf("%.*s %s\n", static_cast<int>(name.size()), name.data(),
                json::write(value, json::Style::Compact).c_str());
    const command::Reply reply = session.call(name, value);
    std::puts(json::write(reply.ok ? reply.value : json::Value(reply.message), json::Style::Compact)
                  .c_str());
}

// Talks to Mira once: presses "Talk", then shows what she said a tick after the reply came.
void talk(command::Session& session) {
    call(session, "ui.press", R"({"id": "talk"})");
    call(session, "step", R"({"ticks": 2})");
    call(session, "entity.get",
         R"({"id": "speech", "components": ["Label"], "omit_defaults": true})");
}

} // namespace

int main() {
    ModelServer server;
    // The URL holds the server's port, so the file is written here; a game commits its own.
    const std::string config = R"({
  "providers": {
    "local": {"type": "openai", "model": "llama3.2", "url": ")" +
                               server.url() + R"("}
  },
  "roles": {
    "baker": {
      "provider": "local",
      "system": "You are Mira, who keeps the bakery in Hollow Vale. Answer in one sentence."
    }
  },
  "replay": {"file": "tests/ai.replay.jsonl"}
}
)";
    (void)slash::platform::write_file("providers.json", config);

    world::ComponentRegistry components;
    world::register_builtin_components(components);
    components.add<game::Dialogue>()
        .set_doc("Someone the player talks to through a language model.")
        .describe("questions", {.doc = "What the player asks, in turn."})
        .describe("request", {.doc = "The request being answered, or empty."})
        .describe("messages", {.doc = "The conversation so far."});
    command::Registry commands;
    command::register_builtin_commands(commands);

    std::puts("# Online: the session calls the model and records each reply.");
    {
        command::SessionOptions options;
        options.project_root = ".";
        options.capture_logs = false;
        command::Session session(components, commands, options);
        session.world().add_system("dialogue", dialogue);
        call(session, "scene.open", R"({"path": "main.scene.json"})");
        call(session, "events.subscribe", R"({"kinds": ["ai_reply", "ai_failed"]})");
        talk(session);
        call(session, "events.read", R"({"subscription": "events_1"})");
        talk(session);
        call(session, "entity.get", R"({"id": "mira", "components": ["Dialogue"]})");
    }
    server.stop();
    std::printf("the model answered %d requests\n", server.requests());

    std::puts("\n# Offline: no network capability, so replies come from the replay file.");
    {
        command::SessionOptions options;
        options.project_root = ".";
        options.capture_logs = false;
        options.capabilities = {command::Capability::Read, command::Capability::Edit,
                                command::Capability::Run, command::Capability::Filesystem};
        command::Session session(components, commands, options);
        session.world().add_system("dialogue", dialogue);
        call(session, "scene.open", R"({"path": "main.scene.json"})");
        talk(session);
        talk(session);
        // A question the replay file has no answer for can't reach the model.
        talk(session);
    }
    return 0;
}
