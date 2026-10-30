// Analytics sent to a server the developer runs, as project.json's services.analytics.endpoint
// asks: events batched as JSON and POSTed, with sessions, and funnels counted as the player
// goes. A collector on this machine stands in for the server (a self-hosted PostHog takes the
// "posthog" format the same way). It is down at first, so the batch waits and goes again.
//
// The clock and IDs are the program's own, so the output is the same every run; a game uses the
// system's, and engine/ai's HTTP client sends each batch on a thread of its own.

#include "slash/ai/transport.hpp"
#include "slash/core/json.hpp"
#include "slash/services/endpoint.hpp"
#include "slash/services/services.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/world.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <httplib.h>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace json = slash::json;
namespace services = slash::services;
namespace world = slash::world;

// The collector: answers 503 while down, then takes batches with 204.
struct Collector {
    httplib::Server server;
    std::mutex mutex;
    std::vector<std::string> bodies;
    int refused = 0;
    bool down = true;
    int port = 0;
    std::thread thread;

    Collector() {
        server.Post("/v1/events",
                    [this](const httplib::Request& request, httplib::Response& response) {
                        const std::scoped_lock lock(mutex);
                        if (down) {
                            ++refused;
                            response.status = 503;
                            return;
                        }
                        bodies.push_back(request.body);
                        response.status = 204;
                    });
        port = server.bind_to_any_port("127.0.0.1");
        thread = std::thread([this] { server.listen_after_bind(); });
        server.wait_until_ready();
    }
    Collector(const Collector&) = delete;
    Collector& operator=(const Collector&) = delete;
    Collector(Collector&&) = delete;
    Collector& operator=(Collector&&) = delete;
    ~Collector() {
        server.stop();
        thread.join();
    }
};

void print(const char* text) {
    std::printf("%s\n", text);
}

} // namespace

int main() {
    Collector collector;
    const std::string url = "http://127.0.0.1:" + std::to_string(collector.port) + "/v1/events";

    // project.json's "services", with the collector's port.
    auto settings_json = json::parse(R"({
        "analytics": {
            "endpoint": {"url": ")" + url +
                                     R"(", "batch_size": 5, "flush_seconds": 10},
            "funnels": {"tutorial": ["tutorial_start", "first_jump", "tutorial_done"]}},
        "consent": {"analytics": "granted"}})");
    if (!settings_json || !services::validate_settings(settings_json.value(), "").empty()) {
        print("the settings aren't valid");
        return 1;
    }
    const services::Settings settings = services::parse_settings(settings_json.value());
    if (!settings.analytics_endpoint) {
        print("no endpoint");
        return 1;
    }

    const world::ComponentRegistry components;
    world::World game(components);
    services::install(game, settings);

    // What main() gives an exported game, with this program's clock and IDs.
    std::int64_t now = 1'790'000'000'000;
    int ids = 0;
    services::EndpointOptions options;
    options.post = [](services::HttpPost post, std::function<void(int)> done) {
        slash::ai::HttpRequest request;
        request.url = std::move(post.url);
        request.headers = std::move(post.headers);
        request.body = std::move(post.body);
        slash::ai::post_in_background(
            slash::ai::http_transport(), std::move(request),
            [done = std::move(done)](slash::Result<slash::ai::HttpResponse> reply) {
                done(reply ? reply.value().status : 0);
            });
    };
    options.app_name = "Dungeon";
    options.app_version = "1.2.0";
    options.platform = "linux";
    options.now_ms = [&now] { return now; };
    options.new_id = [&ids] { return "id" + std::to_string(++ids); };
    services::Backends backends;
    backends.analytics.push_back(
        services::make_endpoint_analytics(*settings.analytics_endpoint, std::move(options)));
    services::set_backends(game, std::move(backends));

    // Steps the game until the backend's batch has an answer.
    const auto step_until_answered = [&](std::int64_t sent) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline) {
            game.step(1.0 / 60.0);
            const json::Value status =
                *services::backends_json(game).find("analytics_status")->find("endpoint");
            if (status.find("in_flight")->as_int() == 0 &&
                status.find("last_status")->as_int() != 0 &&
                status.find("sent")->as_int() >= sent) {
                return status;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return json::Value();
    };

    print("The player goes through the tutorial; each step is a funnel_step event:");
    for (const char* name : {"tutorial_start", "first_jump", "tutorial_done"}) {
        (void)services::log_event(game, name);
        now += 4000;
        game.step(1.0 / 60.0);
        for (const world::Event& event : game.events()) {
            if (event.kind == "funnel_step") {
                std::printf("  %s: %s, step %lld of %lld\n", name, event.id.c_str(),
                            static_cast<long long>(event.data.find("step")->as_int()),
                            static_cast<long long>(event.data.find("steps")->as_int()));
            }
        }
    }
    json::Value params;
    params["level"] = 1;
    (void)services::log_event(game, "level_start", params);

    print("\nWith first_open and session_start, five events make a batch, but the server is down:");
    json::Value status = step_until_answered(0);
    std::printf("  it answered %lld; %lld events wait\n",
                static_cast<long long>(status.find("last_status")->as_int()),
                static_cast<long long>(status.find("queued")->as_int()));

    print("\nA minute later the server is up, and the batch goes again:");
    {
        const std::scoped_lock lock(collector.mutex);
        collector.down = false;
    }
    now += 60'000;
    status = step_until_answered(5);
    std::printf("  it answered %lld; %lld sent, %lld wait\n",
                static_cast<long long>(status.find("last_status")->as_int()),
                static_cast<long long>(status.find("sent")->as_int()),
                static_cast<long long>(status.find("queued")->as_int()));

    print("\nWhat the server received:");
    const std::scoped_lock lock(collector.mutex);
    for (const std::string& body : collector.bodies) {
        auto parsed = json::parse(body);
        std::printf("%s\n", json::write(parsed.value()).c_str());
    }
    std::printf("\nThe server refused %d batch while it was down.\n", collector.refused);
    return 0;
}
