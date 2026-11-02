#include "slash/ai/transport.hpp"
#include "slash/core/json.hpp"
#include "slash/platform/file.hpp"
#include "slash/services/endpoint.hpp"
#include "slash/services/services.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <httplib.h>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace json = slash::json;
namespace services = slash::services;
namespace world = slash::world;

namespace {

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? std::string() : parsed.error().message));
    return std::move(parsed).value();
}

std::string compact(const json::Value& value) {
    return json::write(value, json::Style::Compact);
}

// A server that answers each POST when the test says, with the status it says, keeping the
// bodies; and a clock and IDs the test moves on.
struct FakeServer {
    std::vector<services::HttpPost> posts;
    std::vector<std::function<void(int)>> waiting;
    std::int64_t now = 1'790'000'000'000; // 2026-09-21T14:13:20Z
    int ids = 0;

    services::EndpointOptions options(std::string store = {}) {
        return {.post =
                    [this](services::HttpPost post, std::function<void(int)> done) {
                        posts.push_back(std::move(post));
                        waiting.push_back(std::move(done));
                    },
                .store_path = std::move(store),
                .app_name = "Dungeon",
                .app_version = "1.2.0",
                .platform = "linux",
                .now_ms = [this] { return now; },
                .new_id = [this] { return "id" + std::to_string(++ids); }};
    }

    void answer(int status) {
        REQUIRE_FALSE(waiting.empty());
        auto done = std::move(waiting.front());
        waiting.erase(waiting.begin());
        done(status);
    }

    // The event names of a post's body, in the engine's own format.
    [[nodiscard]] std::vector<std::string> events(std::size_t post) const {
        std::vector<std::string> names;
        const json::Value body = parse(posts.at(post).body);
        for (const json::Value& event : body.find("events")->as_array()) {
            names.push_back(event.find("event")->as_string());
        }
        return names;
    }
};

services::AnalyticsEndpoint endpoint() {
    services::AnalyticsEndpoint out;
    out.url = "https://stats.example.com/v1/events";
    out.headers = {{"X-Game", "dungeon"}};
    out.batch_size = 4;
    out.flush_seconds = 10;
    out.session_minutes = 30;
    out.max_queued = 6;
    return out;
}

} // namespace

TEST_CASE("analytics endpoint settings and funnels validate and parse") {
    const json::Value doc = parse(R"({"analytics": {
        "endpoint": {"url": "https://ph.example.com/batch/", "format": "posthog",
                     "api_key": "phc_public", "headers": {"X-Game": "dungeon"},
                     "batch_size": 50, "flush_seconds": 5, "session_minutes": 15,
                     "max_queued": 200},
        "funnels": {"tutorial": ["tutorial_start", "first_jump", "tutorial_done"]}}})");
    REQUIRE(services::validate_settings(doc, "/services").empty());
    const services::Settings settings = services::parse_settings(doc);
    REQUIRE(settings.analytics_endpoint.has_value());
    const services::AnalyticsEndpoint& parsed = *settings.analytics_endpoint;
    CHECK(parsed.url == "https://ph.example.com/batch/");
    CHECK(parsed.format == services::AnalyticsEndpoint::Format::PostHog);
    CHECK(parsed.api_key == "phc_public");
    CHECK(parsed.headers ==
          std::vector<std::pair<std::string, std::string>>{{"X-Game", "dungeon"}});
    CHECK(parsed.batch_size == 50);
    CHECK(parsed.flush_seconds == 5.0);
    CHECK(parsed.session_minutes == 15.0);
    CHECK(parsed.max_queued == 200);
    REQUIRE(settings.funnels.size() == 1);
    CHECK(settings.funnels[0].steps.size() == 3);
    CHECK_FALSE(services::parse_settings(parse("{}")).analytics_endpoint.has_value());

    const auto errors = services::validate_settings(parse(R"({"analytics": {
        "endpoint": {"url": "ftp://x", "format": "posthog"},
        "funnels": {"tutorial": ["tutorial_start", "2nd"], "one": ["a"]}}})"),
                                                    "/services");
    std::vector<std::string> paths;
    for (const auto& error : errors) {
        paths.push_back(error.path);
    }
    // The schema finds the funnel with one step first, then nothing else is checked.
    CHECK(paths == std::vector<std::string>{"/services/analytics/funnels/one"});
    const auto more = services::validate_settings(parse(R"({"analytics": {
        "endpoint": {"url": "ftp://x", "format": "posthog"},
        "funnels": {"tutorial": ["tutorial_start", "2nd"]}}})"),
                                                  "/services");
    paths.clear();
    for (const auto& error : more) {
        paths.push_back(error.path);
    }
    CHECK(paths == std::vector<std::string>{"/services/analytics/endpoint/url",
                                            "/services/analytics/endpoint/api_key",
                                            "/services/analytics/funnels/tutorial/1"});
}

TEST_CASE("the endpoint backend batches events with sessions and sends them as JSON") {
    FakeServer server;
    auto backend = services::make_endpoint_analytics(endpoint(), server.options());
    CHECK(backend->name() == "endpoint");
    backend->start(nullptr);
    backend->set_user_id("player-7");
    backend->set_user_property("class", "mage");
    // The first event starts the install's first session.
    backend->log_event("level_start", parse(R"({"level": 1})"));
    CHECK(server.posts.empty()); // 3 of 4
    server.now += 1000;
    backend->log_event("level_end", parse(R"({"won": true})"));
    REQUIRE(server.posts.size() == 1);
    const services::HttpPost& post = server.posts[0];
    CHECK(post.url == "https://stats.example.com/v1/events");
    CHECK(post.headers == std::vector<std::pair<std::string, std::string>>{{"X-Game", "dungeon"}});
    CHECK(compact(parse(post.body)) ==
          R"({"app":{"name":"Dungeon","platform":"linux","version":"1.2.0"},"device":"id1",)"
          R"("events":[)"
          R"({"event":"first_open","seq":1,"session":"id2","time":"2026-09-21T14:13:20.000Z",)"
          R"("user_id":"player-7","user_properties":{"class":"mage"}},)"
          R"({"event":"session_start","seq":2,"session":"id2","time":"2026-09-21T14:13:20.000Z",)"
          R"("user_id":"player-7","user_properties":{"class":"mage"}},)"
          R"({"event":"level_start","params":{"level":1},"seq":3,"session":"id2",)"
          R"("time":"2026-09-21T14:13:20.000Z","user_id":"player-7",)"
          R"("user_properties":{"class":"mage"}},)"
          R"({"event":"level_end","params":{"won":true},"seq":4,"session":"id2",)"
          R"("time":"2026-09-21T14:13:21.000Z","user_id":"player-7",)"
          R"("user_properties":{"class":"mage"}}],)"
          R"("sent_at":"2026-09-21T14:13:21.000Z"})");
    CHECK(backend->status().find("in_flight")->as_int() == 4);

    // One batch at a time: the next waits, then goes after flush_seconds.
    backend->log_event("shop", json::Value());
    server.answer(200);
    backend->poll();
    CHECK(compact(backend->status()) ==
          R"({"device":"id1","dropped":0,"in_flight":0,"last_status":200,"queued":1,)"
          R"("sent":4,"session":"id2"})");
    server.now += 9000;
    backend->poll();
    CHECK(server.posts.size() == 1);
    server.now += 1000;
    backend->poll();
    REQUIRE(server.posts.size() == 2);
    CHECK(server.events(1) == std::vector<std::string>{"shop"});
    server.answer(200);

    // Half an hour without events starts another session.
    server.now += 31 * 60'000;
    backend->log_event("menu", json::Value());
    backend->flush();
    REQUIRE(server.posts.size() == 3);
    CHECK(server.events(2) == std::vector<std::string>{"session_start", "menu"});
    CHECK(parse(server.posts[2].body).find("events")->as_array()[0].find("session")->as_string() ==
          "id3");
}

TEST_CASE("the endpoint backend retries, drops what the server refuses and keeps what waits") {
    const std::filesystem::path folder = "analytics_endpoint_store";
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);
    const std::string store = (folder / "analytics.json").string();
    FakeServer server;
    {
        auto backend = services::make_endpoint_analytics(endpoint(), server.options(store));
        backend->start(nullptr);
        backend->log_event("a", json::Value());
        backend->flush(); // first_open, session_start, a
        server.answer(503);
        backend->poll();
        CHECK(backend->status().find("queued")->as_int() == 3);
        // Tried again a minute later, then two.
        server.now += 59'000;
        backend->poll();
        CHECK(server.posts.size() == 1);
        server.now += 1000;
        backend->poll();
        REQUIRE(server.posts.size() == 2);
        server.answer(0); // unreachable
        backend->poll();
        server.now += 60'000;
        backend->poll();
        CHECK(server.posts.size() == 2);
        server.now += 60'000;
        backend->poll();
        REQUIRE(server.posts.size() == 3);
        server.answer(400); // refused: dropped
        backend->poll();
        CHECK(backend->status().find("dropped")->as_int() == 3);

        // Over max_queued (6), the oldest go.
        for (const char* name : {"e1", "e2", "e3", "e4", "e5", "e6", "e7"}) {
            backend->log_event(name, json::Value());
        }
        // e1-e4 went as a batch, which is still out; e5 made way for e7.
        CHECK(server.events(3) == std::vector<std::string>{"e1", "e2", "e3", "e4"});
        CHECK(backend->status().find("queued")->as_int() == 6);
        CHECK(backend->status().find("dropped")->as_int() == 4);
        backend->log_event("e8", json::Value());
        backend->log_event("e9", json::Value());
        CHECK(backend->status().find("queued")->as_int() == 6);
        CHECK(backend->status().find("dropped")->as_int() == 6);
    } // the backend goes, keeping what is unsent and its device ID
    const json::Value kept = parse(slash::platform::read_file(store).value());
    CHECK(kept.find("device")->as_string() == "id1");
    CHECK(kept.find("events")->size() == 6);

    // The next run sends them, with no first_open (its session_start and first event take the
    // places of the two oldest), then consent taken back drops what waits.
    FakeServer next;
    next.now = server.now + 1000;
    next.ids = 10;
    auto backend = services::make_endpoint_analytics(endpoint(), next.options(store));
    backend->start(nullptr);
    backend->log_event("back", json::Value());
    REQUIRE(next.posts.size() == 1);
    CHECK(next.events(0) == std::vector<std::string>{"e3", "e4", "e8", "e9"});
    CHECK(parse(next.posts[0].body).find("device")->as_string() == "id1");
    backend->set_consent(
        {.analytics = services::Consent::Denied, .personalized_ads = services::Consent::Unknown});
    CHECK(backend->status().find("queued")->as_int() == 4); // being sent
    next.answer(200);
    backend->poll();
    CHECK(backend->status().find("queued")->as_int() == 0);
    backend.reset();
    CHECK(parse(slash::platform::read_file(store).value()).find("events")->size() == 0);
    std::filesystem::remove_all(folder);
}

TEST_CASE("the endpoint backend speaks PostHog's batch API") {
    FakeServer server;
    services::AnalyticsEndpoint posthog = endpoint();
    posthog.format = services::AnalyticsEndpoint::Format::PostHog;
    posthog.api_key = "phc_public";
    posthog.batch_size = 2;
    auto backend = services::make_endpoint_analytics(posthog, server.options());
    backend->start(nullptr);
    backend->set_user_property("class", "mage");
    backend->log_event("level_start", parse(R"({"level": 1})"));
    // first_open and session_start went; level_start waits.
    REQUIRE(server.posts.size() == 1);
    server.answer(200);
    backend->set_user_id("player-7");
    backend->flush();
    REQUIRE(server.posts.size() == 2);
    CHECK(compact(parse(server.posts[1].body)) ==
          R"({"api_key":"phc_public","batch":[{"distinct_id":"id1","event":"level_start",)"
          R"("properties":{"$app_name":"Dungeon","$app_version":"1.2.0","$device_id":"id1",)"
          R"("$lib":"slashengine","$os":"linux","$session_id":"id2","$set":{"class":"mage"},)"
          R"("level":1},"timestamp":"2026-09-21T14:13:20.000Z"}]})");
}

TEST_CASE("funnels move on as their steps are logged, as world events") {
    world::ComponentRegistry components;
    world::World world(components);
    services::Settings settings = services::parse_settings(parse(R"({
        "analytics": {"funnels": {"tutorial": ["tutorial_start", "first_jump", "tutorial_done"]}},
        "consent": {"analytics": "granted"}})"));
    services::install(world, settings);
    FakeServer server;
    services::Backends backends;
    backends.analytics.push_back(services::make_endpoint_analytics(endpoint(), server.options()));
    services::set_backends(world, std::move(backends));

    REQUIRE(services::log_event(world, "first_jump").ok()); // not yet: out of order
    REQUIRE(services::log_event(world, "tutorial_start").ok());
    REQUIRE(services::log_event(world, "first_jump").ok());
    std::vector<std::string> steps;
    for (const world::Event& event : world.event_queues().next) {
        if (event.kind == "funnel_step") {
            steps.push_back(event.id + " " + compact(event.data));
        }
    }
    CHECK(steps == std::vector<std::string>{R"(tutorial {"step":1,"steps":3})",
                                            R"(tutorial {"step":2,"steps":3})"});
    const json::Value state = services::state_json(world, 0);
    CHECK(compact(*state.find("analytics")->find("funnels")) ==
          R"({"tutorial":{"step":2,"steps":3}})");
    const json::Value status = *services::backends_json(world).find("analytics_status");
    CHECK(status.find("endpoint")->find("in_flight")->as_int() == 4);
}

TEST_CASE("engine/ai's HTTP client posts analytics batches in the background") {
    httplib::Server collector;
    std::mutex mutex;
    std::vector<std::string> bodies;
    collector.Post("/events", [&](const httplib::Request& request, httplib::Response& response) {
        const std::scoped_lock lock(mutex);
        bodies.push_back(request.body);
        response.status = 204;
    });
    const int port = collector.bind_to_any_port("127.0.0.1");
    std::thread serving([&] { collector.listen_after_bind(); });
    collector.wait_until_ready();

    services::AnalyticsEndpoint local = endpoint();
    local.url = "http://127.0.0.1:" + std::to_string(port) + "/events";
    services::EndpointOptions options;
    options.post = [](services::HttpPost post, std::function<void(int)> done) {
        slash::ai::HttpRequest request;
        request.url = std::move(post.url);
        request.headers = std::move(post.headers);
        request.body = std::move(post.body);
        slash::ai::post_in_background(
            slash::ai::make_http_transport({}), std::move(request),
            [done = std::move(done)](slash::Result<slash::ai::HttpResponse> reply) {
                done(reply ? reply.value().status : 0);
            });
    };
    auto backend = services::make_endpoint_analytics(local, std::move(options));
    backend->start(nullptr);
    backend->log_event("boot", json::Value());
    backend->flush();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (backend->status().find("sent")->as_int() == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        backend->poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(backend->status().find("last_status")->as_int() == 204);
    CHECK(backend->status().find("sent")->as_int() == 3);
    collector.stop();
    serving.join();
    const std::scoped_lock lock(mutex);
    REQUIRE(bodies.size() == 1);
    CHECK(parse(bodies[0]).find("events")->size() == 3);
}
