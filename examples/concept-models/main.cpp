// A model made concept first: a cheap image is generated and checked, then a model is made from
// it with the image kept as its parent, reviewed and made again with what was wrong until it is
// accepted.
//
// The built-in test generator makes the image and the model (a relief of the image's shape), so
// the example needs no key. The vision model asset.review asks is a stand-in on the loopback
// interface; it can't see, so it judges from the size the request gives it, as a real model
// would see a stool too thin to sit on.

#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/library/commands.hpp"
#include "slash/platform/file.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <cstdio>
#include <cstdlib>
#include <httplib.h>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace {

namespace command = slash::command;
namespace json = slash::json;

// Prints the call as an agent would make it, then the reply.
json::Value call(command::Session& session, std::string_view name, std::string_view params) {
    const json::Value value = json::parse(params).value();
    std::printf("%.*s %s\n", static_cast<int>(name.size()), name.data(),
                json::write(value, json::Style::Compact).c_str());
    const command::Reply reply = session.call(name, value);
    std::puts(json::write(reply.ok ? reply.value : json::Value(reply.message), json::Style::Compact)
                  .c_str());
    return reply.ok ? reply.value : json::Value();
}

// An OpenAI-compatible chat server that reads the model's depth from the size in the request,
// "W x H x D m", and wants a stool at least 30 cm deep.
class VisionModel {
public:
    VisionModel() {
        server_.Post("/v1/chat/completions", [](const httplib::Request& request,
                                                httplib::Response& response) {
            response.set_content(answer(json::parse(request.body).value()), "application/json");
        });
        port_ = server_.bind_to_any_port("127.0.0.1");
        thread_ = std::thread([this] { server_.listen_after_bind(); });
        server_.wait_until_ready();
    }

    ~VisionModel() {
        server_.stop();
        thread_.join();
    }

    VisionModel(const VisionModel&) = delete;
    VisionModel& operator=(const VisionModel&) = delete;

    [[nodiscard]] std::string url() const {
        return "http://127.0.0.1:" + std::to_string(port_) + "/v1";
    }

private:
    static std::string answer(const json::Value& body) {
        const json::Value& content =
            body.find("messages")->as_array().back().find("content")->as_array().back();
        const std::string& text = content.find("text")->as_string();
        const std::size_t known = text.find("What is known about the model: ");
        const std::size_t x = text.find(" x ", text.find(" x ", known) + 3);
        const double depth = std::strtod(text.c_str() + x + 3, nullptr);
        std::printf("the stand-in read a depth of %.2f m\n", depth);
        json::Value verdict;
        verdict["matches"] = depth >= 0.3;
        verdict["problems"] = json::Array{};
        if (depth < 0.3) {
            json::Value problem;
            problem["view"] = 2;
            problem["problem"] = "The stool is too thin to sit on; make it thick.";
            verdict["problems"].push_back(std::move(problem));
        }
        json::Value reply;
        reply["model"] = "vision-model";
        json::Value choice;
        choice["finish_reason"] = "stop";
        choice["message"]["role"] = "assistant";
        choice["message"]["content"] = json::write(verdict, json::Style::Compact);
        reply["choices"].push_back(std::move(choice));
        return json::write(reply, json::Style::Compact);
    }

    httplib::Server server_;
    int port_ = 0;
    std::thread thread_;
};

} // namespace

int main() {
    slash::world::ComponentRegistry components;
    slash::world::register_builtin_components(components);
    command::Registry commands;
    command::register_builtin_commands(commands);
    slash::library::register_commands(commands);

    const VisionModel vision;
    (void)slash::platform::write_file(
        "game/providers.json",
        R"({"providers": {"local": {"type": "openai", "model": "vision-model", "url": ")" +
            vision.url() + R"("}}})");

    // The library would be ~/.slash/library; the example keeps its own.
    command::SessionOptions options;
    options.capture_logs = false;
    options.library_dir = "library";
    options.project_root = "game";
    command::Session game(components, commands, options);

    // A cheap concept first: the agent looks at it and accepts it before any model is made.
    const std::string concept_image =
        call(game, "asset.generate",
             R"({"prompt": "a round red mushroom cap with white spots, transparent sprite",
                 "generator": "test", "width": 48, "path": "concepts/stool.png"})")["id"]
            .as_string();
    call(game, "asset.feedback",
         R"({"id": ")" + concept_image +
             R"(", "verdict": "accepted", "reason": "reads as a mushroom cap at a glance"})");

    // The model from it, reviewed: the first is too thin, so it is made again with that in the
    // prompt, accepted and copied into the project.
    const std::string stool =
        call(game, "asset.generate",
             R"({"type": "model", "prompt": "a mushroom stool for a forest cafe", "image": ")" +
                 concept_image +
                 R"(", "generator": "test", "review": 3, "path": "props/stool.glb"})")["id"]
            .as_string();
    call(game, "asset.info", R"({"id": ")" + stool + R"("})");

    // A model is always made from an image.
    call(game, "asset.generate", R"({"type": "model", "prompt": "a mushroom stool"})");
    return 0;
}
