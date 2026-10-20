#include "slash/ai/world.hpp"

#include "slash/core/json.hpp"
#include "slash/platform/file.hpp"
#include "slash/world/world.hpp"

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace slash::ai {
namespace {

struct Asked {
    std::uint64_t number = 0;
    std::string id; // "ai_<number>"
    std::string role;
    std::unique_ptr<Call> call;
};

struct State {
    WorldSettings settings;
    // providers.json as last read, and its stamp then, to read it again when it changes.
    bool read = false;
    std::optional<platform::FileStamp> stamp;
    Result<Config> config = Config{};
    // Providers opened so far, by the role or provider name asked for.
    std::map<std::string, std::shared_ptr<Provider>, std::less<>> providers;
    std::vector<Asked> asked; // in the order asked
    std::uint64_t next_request = 1;
};

using Handle = std::shared_ptr<State>;

State* state_of(world::World& world) {
    auto* found = world.ecs().ctx().find<Handle>();
    return found != nullptr ? found->get() : nullptr;
}

const State* state_of(const world::World& world) {
    const auto* found = world.ecs().ctx().find<Handle>();
    return found != nullptr ? found->get() : nullptr;
}

const Asked* find_asked(const State* state, std::string_view request) {
    if (state == nullptr) {
        return nullptr;
    }
    const auto found = std::ranges::find(state->asked, request, &Asked::id);
    return found == state->asked.end() ? nullptr : &*found;
}

// The provider for a role, reading providers.json again first if it changed.
Result<std::shared_ptr<Provider>> provider_for(State& state, std::string_view role) {
    std::string path = state.settings.open.project_root;
    if (!path.empty() && path.back() != '/') {
        path += '/';
    }
    path += kConfigFile;
    const std::optional<platform::FileStamp> stamp = platform::file_stamp(path);
    if (!state.read || (stamp && stamp != state.stamp)) {
        state.config = load_config(state.settings.open.project_root);
        state.stamp = stamp;
        state.read = true;
        state.providers.clear();
    }
    if (!state.config) {
        return state.config.error();
    }
    if (const auto found = state.providers.find(role); found != state.providers.end()) {
        return found->second;
    }
    auto opened = open(state.config.value(), role, state.settings.open);
    if (!opened) {
        return opened.error();
    }
    state.providers.emplace(std::string(role), opened.value().provider);
    return opened.value().provider;
}

json::Value reply_data(const std::string& role, const Response& response) {
    json::Value data;
    data["role"] = role;
    data["text"] = response.message.text;
    data["message"] = to_json(response.message);
    if (!response.output.is_null()) {
        data["output"] = response.output;
    }
    const json::Value whole = to_json(response);
    data["stop"] = *whole.find("stop");
    data["usage"] = *whole.find("usage");
    return data;
}

// Hands each finished reply to the game, in the order the requests were asked; with wait, each
// request asked before this tick finishes first.
void run(world::World& world) {
    State* state = state_of(world);
    if (state == nullptr || state->asked.empty()) {
        return;
    }
    std::vector<Asked> asked = std::move(state->asked);
    state->asked.clear();
    for (Asked& item : asked) {
        if (state->settings.wait) {
            item.call->wait();
        }
        const Result<Response>* result = item.call->result();
        if (result == nullptr) {
            state->asked.push_back(std::move(item));
            continue;
        }
        if (result->ok()) {
            world.emit("ai_reply", item.id, reply_data(item.role, result->value()));
        } else {
            json::Value data;
            data["role"] = item.role;
            data["code"] = std::string(to_string(result->error().code));
            data["message"] = result->error().message;
            world.emit("ai_failed", item.id, std::move(data));
        }
    }
}

} // namespace

void install(world::World& world, WorldSettings settings) {
    if (State* state = state_of(world); state != nullptr) {
        *state = State{};
        state->settings = std::move(settings);
        return;
    }
    auto state = std::make_shared<State>();
    state->settings = std::move(settings);
    world.ecs().ctx().emplace<Handle>(std::move(state));
    // A world played again from the start asks again from the first ID.
    world.add_reset([](world::World& in_world) {
        if (State* reset = state_of(in_world); reset != nullptr) {
            reset->asked.clear();
            reset->next_request = 1;
        }
    });
    world.add_system(
        "ai", [](world::World& in_world, double /*tick_seconds*/) { run(in_world); },
        {.doc = "Hands each finished reply from a language model to the game as an ai_reply or "
                "ai_failed event.",
         .priority = 1150});
}

bool installed(const world::World& world) {
    return state_of(world) != nullptr;
}

void set_wait(world::World& world, bool wait) {
    if (State* state = state_of(world); state != nullptr) {
        state->settings.wait = wait;
    }
}

Result<std::string> ask(world::World& world, std::string_view role, Request request) {
    State* state = state_of(world);
    if (state == nullptr) {
        return make_error(ErrorCode::NotFound,
                          "this game calls no language models: it has no providers.json");
    }
    auto provider = provider_for(*state, role);
    if (!provider) {
        return provider.error();
    }
    Asked item;
    item.number = state->next_request++;
    item.id = "ai_" + std::to_string(item.number);
    item.role = std::string(role);
    item.call =
        std::make_unique<Call>(std::move(provider).value(), std::move(request), /*stream=*/true);
    std::string id = item.id;
    state->asked.push_back(std::move(item));
    return id;
}

Result<std::string> ask(world::World& world, std::string_view role, std::string prompt) {
    Request request;
    request.messages.push_back({.role = Role::User, .text = std::move(prompt)});
    return ask(world, role, std::move(request));
}

std::string reply_text(const world::World& world, std::string_view request) {
    const Asked* item = find_asked(state_of(world), request);
    return item != nullptr ? item->call->text() : std::string();
}

bool pending(const world::World& world, std::string_view request) {
    return find_asked(state_of(world), request) != nullptr;
}

void cancel(world::World& world, std::string_view request) {
    if (State* state = state_of(world); state != nullptr) {
        std::erase_if(state->asked, [request](const Asked& item) { return item.id == request; });
    }
}

Snapshot save(const world::World& world) {
    const State* state = state_of(world);
    return {state != nullptr ? state->next_request : 1};
}

void restore(world::World& world, const Snapshot& snapshot) {
    State* state = state_of(world);
    if (state == nullptr) {
        return;
    }
    std::erase_if(state->asked,
                  [&snapshot](const Asked& item) { return item.number >= snapshot.next_request; });
    state->next_request = snapshot.next_request;
}

} // namespace slash::ai
