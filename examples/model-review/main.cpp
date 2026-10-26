// Automatic review of a 3D model: asset.review checks a model's geometry against the storyboard,
// draws it from four sides headless and asks a vision-capable model whether the views show what
// was asked for, then records the verdict as the asset's feedback.
//
// The vision model is a stand-in on the loopback interface, so the example needs no network or
// key: it decodes the views it is sent and looks for brass-colored pixels, as a real model would
// look for the hinges the request asks for. An oak chest without hinges is rejected, the chest
// with hinges added is accepted, and a sign with an open back and a stretched texture is checked
// with no model asked.

#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/core/text.hpp"
#include "slash/library/commands.hpp"
#include "slash/platform/file.hpp"
#include "slash/render/image.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <httplib.h>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

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

// Pixels of polished brass: much more red and green than blue, and bright.
std::size_t brass_pixels(const slash::render::Image& image) {
    std::size_t count = 0;
    for (std::size_t i = 0; i + 3 < image.pixels.size(); i += 4) {
        const int red = image.pixels[i];
        const int green = image.pixels[i + 1];
        const int blue = image.pixels[i + 2];
        if (red > 120 && green * 4 > red * 3 && blue * 2 < green) {
            ++count;
        }
    }
    return count;
}

// An OpenAI-compatible chat server that looks at the images it is sent for brass, and answers
// with a verdict in the schema asset.review asks for.
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
        const json::Value* messages = body.find("messages");
        std::vector<std::size_t> brass;
        for (const json::Value& part : messages->as_array().back().find("content")->as_array()) {
            if (const json::Value* image = part.find("image_url")) {
                const std::string& url = image->find("url")->as_string();
                const std::string png =
                    slash::base64_decode(url.substr(url.find(',') + 1)).value_or("");
                brass.push_back(brass_pixels(slash::render::decode_image(png).value()));
            }
        }
        std::string seen = "the stand-in saw brass pixels:";
        json::Value verdict;
        verdict["matches"] = false;
        verdict["problems"] = json::Array{};
        for (std::size_t view = 0; view < brass.size(); ++view) {
            seen += " " + std::to_string(brass[view]) + " in view " + std::to_string(view + 1);
            if (brass[view] > 0) {
                verdict["matches"] = true;
            }
        }
        std::puts(seen.c_str());
        if (!verdict["matches"].as_bool()) {
            json::Value problem;
            problem["view"] = 2;
            problem["problem"] = "The lid has no brass hinges; its back edge is plain wood.";
            verdict["problems"].push_back(std::move(problem));
        }
        json::Value reply;
        reply["model"] = "vision-model";
        json::Value choice;
        choice["finish_reason"] = "stop";
        choice["message"]["role"] = "assistant";
        choice["message"]["content"] = json::write(verdict, json::Style::Compact);
        reply["choices"].push_back(std::move(choice));
        reply["usage"]["prompt_tokens"] = 1100;
        reply["usage"]["completion_tokens"] = 30;
        return json::write(reply, json::Style::Compact);
    }

    httplib::Server server_;
    int port_ = 0;
    std::thread thread_;
};

// A sign board: a box of 1 by 0.5 m by 5 cm with its back face missing, and a 16-pixel texture
// across its front, as a quick export from a modeling tool can come out.
void write_sign() {
    std::string obj = "mtllib sign.mtl\nusemtl paint\n";
    const std::vector<std::pair<std::string_view, std::string_view>> corners{
        {"-0.5 0 0.025", "0 0"},   {"0.5 0 0.025", "1 0"},    {"0.5 0.5 0.025", "1 1"},
        {"-0.5 0.5 0.025", "0 1"}, {"-0.5 0 -0.025", "0 0"},  {"0.5 0 -0.025", "1 0"},
        {"0.5 0.5 -0.025", "1 1"}, {"-0.5 0.5 -0.025", "0 1"}};
    for (const auto& [position, uv] : corners) {
        obj += "v " + std::string(position) + "\nvt " + std::string(uv) + "\n";
    }
    // Front, top, bottom, left and right; no back.
    obj += "f 1/1 2/2 3/3 4/4\nf 4/4 3/3 7/7 8/8\nf 5/5 6/6 2/2 1/1\nf 5/5 1/1 4/4 8/8\n"
           "f 2/2 6/6 7/7 3/3\n";
    (void)slash::platform::write_file("game/props/sign.obj", obj);
    (void)slash::platform::write_file("game/props/sign.mtl",
                                      "newmtl paint\nKd 1 1 1\nmap_Kd sign.png\n");
    slash::render::Image image;
    image.width = 16;
    image.height = 16;
    image.pixels.resize(std::size_t{16} * 16 * 4);
    for (std::size_t i = 0; i < image.pixels.size(); i += 4) {
        const bool stripe = (i / 4 / 16) % 4 < 2;
        image.pixels[i] = stripe ? 200 : 40;
        image.pixels[i + 1] = stripe ? 60 : 40;
        image.pixels[i + 2] = stripe ? 50 : 60;
        image.pixels[i + 3] = 255;
    }
    (void)slash::platform::write_file("game/props/sign.png",
                                      slash::render::encode_png(image).value());
}

// Replaces the first from in a project file with to, as an agent's edit does.
void edit(const std::string& path, std::string_view from, std::string_view to) {
    std::string text = slash::platform::read_file(path).value();
    text.replace(text.find(from), from.size(), to);
    (void)slash::platform::write_file(path, text);
    std::printf("edit %s\n", path.c_str());
}

} // namespace

int main() {
    slash::world::ComponentRegistry components;
    slash::world::register_builtin_components(components);
    command::Registry commands;
    command::register_builtin_commands(commands);
    slash::library::register_commands(commands);

    // providers.json names the stand-in; the role "" is its one provider.
    const VisionModel vision;
    (void)slash::platform::write_file(
        "game/providers.json",
        R"({"providers": {"local": {"type": "openai", "model": "vision-model", "url": ")" +
            vision.url() + R"("}}})");
    write_sign();

    // The library would be ~/.slash/library; the example keeps its own.
    command::SessionOptions options;
    options.capture_logs = false;
    options.library_dir = "library";
    options.project_root = "game";
    command::Session game(components, commands, options);

    // An oak chest, kept in the library, then reviewed against the request it was made for.
    const std::string chest =
        call(game, "asset.add",
             R"({"path": "props/chest.model.json", "description": "An oak chest",
                 "license": "CC0-1.0"})")["id"]
            .as_string();
    call(
        game, "asset.review",
        R"({"id": ")" + chest +
            R"(", "request": "An oak chest with brass hinges on its lid", "out": "review/chest"})");

    // The agent adds the hinges the review asked for, keeps the new version and reviews it.
    edit("game/props/chest.model.json", R"("materials": {)",
         R"("materials": {"brass": {"albedo_color": [0.9, 0.75, 0.2, 1], "metallic": 0.2},
                )");
    edit("game/props/chest.model.json", R"("position": [0, 0.57, 0]})",
         R"("position": [0, 0.57, 0]},
    {"name": "hinge_left", "shape": "box", "material": "brass", "size": [0.14, 0.12, 0.03], "position": [-0.3, 0.56, -0.33]},
    {"name": "hinge_right", "shape": "box", "material": "brass", "size": [0.14, 0.12, 0.03], "position": [0.3, 0.56, -0.33]})");
    const std::string hinged =
        call(game, "asset.add", R"({"path": "props/chest.model.json"})")["id"].as_string();
    call(game, "asset.review",
         R"({"id": ")" + hinged + R"(", "request": "An oak chest with brass hinges on its lid"})");
    call(game, "asset.info", R"({"id": ")" + hinged + R"("})");

    // The sign is checked where it is, with no model asked: its back is open, and its texture
    // is stretched far below the budget's texel density.
    call(game, "asset.review", R"({"path": "props/sign.obj", "ask": false})");
    return 0;
}
