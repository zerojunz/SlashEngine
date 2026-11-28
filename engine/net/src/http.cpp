#include "slash/net/http.hpp"

#include "slash/world/world.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Requests from game code, and their replies handed back at tick starts.
namespace slash::net {
namespace {

constexpr std::size_t kKeptFinished = 32;

// Where replies land, from whatever thread the sender answers on.
struct Mailbox {
    std::mutex mutex;
    std::vector<std::pair<std::uint64_t, HttpReply>> arrived;

    void post(std::uint64_t id, HttpReply reply) {
        const std::scoped_lock lock(mutex);
        arrived.emplace_back(id, std::move(reply));
    }
    std::vector<std::pair<std::uint64_t, HttpReply>> take() {
        const std::scoped_lock lock(mutex);
        return std::exchange(arrived, {});
    }
};

struct Pending {
    HttpRecord record;
    HttpCallback done;
};

struct HttpState {
    HttpSender sender;
    std::shared_ptr<Mailbox> mailbox = std::make_shared<Mailbox>();
    std::uint64_t next_id = 1;
    std::map<std::uint64_t, Pending> pending;
    std::deque<HttpRecord> finished;
};

using Handle = std::shared_ptr<HttpState>;

HttpState* state_of(world::World& world) {
    auto* found = world.ecs().ctx().find<Handle>();
    return found != nullptr ? found->get() : nullptr;
}

const HttpState* state_of(const world::World& world) {
    const auto* found = world.ecs().ctx().find<Handle>();
    return found != nullptr ? found->get() : nullptr;
}

bool equal_ignoring_case(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::ranges::equal(a, b, [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) ==
                      std::tolower(static_cast<unsigned char>(y));
           });
}

constexpr std::array<std::string_view, 6> kMethods{"GET", "POST", "PUT", "PATCH", "DELETE", "HEAD"};

// Why a request can't go, or "".
std::string refusal(const HttpRequest& request) {
    if (!request.url.starts_with("http://") && !request.url.starts_with("https://")) {
        return "\"" + request.url + "\" is not an http:// or https:// URL";
    }
    if (std::ranges::find(kMethods, request.method) == kMethods.end()) {
        return "no HTTP method \"" + request.method +
               "\"; expected GET, POST, PUT, PATCH, DELETE or HEAD";
    }
    return {};
}

// Hands each reply that arrived to its request's callback, in the order the requests were made.
void deliver(world::World& world) {
    HttpState* state = state_of(world);
    if (state == nullptr) {
        return;
    }
    auto arrived = state->mailbox->take();
    std::ranges::sort(arrived, {}, &std::pair<std::uint64_t, HttpReply>::first);
    for (auto& [id, reply] : arrived) {
        const auto found = state->pending.find(id);
        if (found == state->pending.end()) {
            continue; // cancelled
        }
        Pending pending = std::move(found->second);
        state->pending.erase(found);
        pending.record.done = true;
        pending.record.status = reply.status;
        pending.record.error = reply.error;
        pending.record.finished_tick = world.tick();
        pending.record.bytes = reply.body.size();
        state->finished.push_back(pending.record);
        if (state->finished.size() > kKeptFinished) {
            state->finished.pop_front();
        }
        if (pending.done) {
            pending.done(world, reply);
        }
        // A callback may have installed HTTP again; state stays the same object.
    }
}

} // namespace

std::string_view HttpReply::header(std::string_view name) const {
    for (const auto& [key, value] : headers) {
        if (equal_ignoring_case(key, name)) {
            return value;
        }
    }
    return {};
}

void install_http(world::World& world, HttpSender sender) {
    if (HttpState* state = state_of(world); state != nullptr) {
        state->sender = std::move(sender);
        return;
    }
    auto state = std::make_shared<HttpState>();
    state->sender = std::move(sender);
    world.ecs().ctx().emplace<Handle>(std::move(state));
    world.add_system(
        "http", [](world::World& in_world, double /*tick_seconds*/) { deliver(in_world); },
        {.doc = "Hands HTTP replies that arrived since the last tick to game code's callbacks.",
         .priority = -940});
}

void set_http_sender(world::World& world, HttpSender sender) {
    if (HttpState* state = state_of(world); state != nullptr) {
        state->sender = std::move(sender);
    }
}

std::uint64_t http_request(world::World& world, HttpRequest request, HttpCallback done) {
    HttpState* state = state_of(world);
    if (state == nullptr) {
        return 0;
    }
    const std::uint64_t id = state->next_id++;
    Pending& pending = state->pending[id];
    pending.record = {.id = id,
                      .method = request.method,
                      .url = request.url,
                      .done = false,
                      .status = 0,
                      .error = {},
                      .started_tick = world.tick(),
                      .finished_tick = 0,
                      .bytes = 0};
    pending.done = std::move(done);
    std::string refused = refusal(request);
    if (refused.empty() && !state->sender) {
        refused = "no HTTP here: this program sends no requests";
    }
    if (!refused.empty()) {
        state->mailbox->post(id,
                             {.status = 0, .headers = {}, .body = {}, .error = std::move(refused)});
        return id;
    }
    const std::weak_ptr<Mailbox> mailbox = state->mailbox;
    state->sender(std::move(request), [mailbox, id](HttpReply reply) {
        if (const auto box = mailbox.lock()) {
            box->post(id, std::move(reply));
        }
    });
    return id;
}

std::uint64_t http_get(world::World& world, std::string url, HttpCallback done) {
    HttpRequest request;
    request.url = std::move(url);
    return http_request(world, std::move(request), std::move(done));
}

std::uint64_t http_post_json(world::World& world, std::string url, const json::Value& body,
                             HttpCallback done) {
    HttpRequest request;
    request.url = std::move(url);
    request.method = "POST";
    request.body = json::write(body, json::Style::Compact);
    return http_request(world, std::move(request), std::move(done));
}

void http_cancel(world::World& world, std::uint64_t id) {
    if (HttpState* state = state_of(world); state != nullptr) {
        state->pending.erase(id);
    }
}

std::vector<HttpRecord> http_requests(const world::World& world) {
    std::vector<HttpRecord> out;
    const HttpState* state = state_of(world);
    if (state == nullptr) {
        return out;
    }
    out.assign(state->finished.begin(), state->finished.end());
    for (const auto& [id, pending] : state->pending) {
        out.push_back(pending.record);
    }
    return out;
}

void HttpFakes::add(HttpFake fake) {
    fakes_.push_back(std::move(fake));
}

void HttpFakes::clear() {
    fakes_.clear();
}

HttpSender HttpFakes::sender(HttpSender fallback) const {
    return [this, fallback = std::move(fallback)](HttpRequest request,
                                                  std::function<void(HttpReply)> done) {
        for (const HttpFake& fake : std::ranges::reverse_view(fakes_)) {
            if (request.url.starts_with(fake.url_prefix) &&
                (fake.method.empty() || fake.method == request.method)) {
                done({.status = fake.status,
                      .headers = fake.headers,
                      .body = request.method == "HEAD" ? std::string() : fake.body,
                      .error = {}});
                return;
            }
        }
        if (fallback) {
            fallback(std::move(request), std::move(done));
            return;
        }
        done({.status = 0,
              .headers = {},
              .body = {},
              .error = "no fake for " + request.method + " " + request.url +
                       "; add one with net.http_fake"});
    };
}

} // namespace slash::net
