#include "slash/world/world.hpp"

#include "slash/core/jobs.hpp"
#include "slash/core/profile.hpp"
#include "slash/world/component_registry.hpp"

#include <algorithm>
#include <chrono>
#include <functional>
#include <utility>

namespace slash::world {

namespace detail {

// What a system did that waits for the thread stepping the world: a threaded system's events,
// and any system's deferred calls, in the order it made them.
struct SystemRun {
    struct Action {
        Event event;
        std::function<void(World&)> call; // a deferred call, or null for the event
    };
    std::vector<Action> actions;
};

} // namespace detail

namespace {

// The system the current thread runs, if any.
struct Running {
    const World* world = nullptr;
    detail::SystemRun* run = nullptr;
    bool threaded = false;
};
thread_local Running t_running;

using Clock = std::chrono::steady_clock;

double milliseconds_since(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

// Sets the thread's running system for as long as it lives.
class RunningScope {
public:
    explicit RunningScope(Running running) : saved_(t_running) { t_running = running; }
    ~RunningScope() { t_running = saved_; }
    RunningScope(const RunningScope&) = delete;
    RunningScope& operator=(const RunningScope&) = delete;
    RunningScope(RunningScope&&) = delete;
    RunningScope& operator=(RunningScope&&) = delete;

private:
    Running saved_;
};

bool names_meet(const std::vector<std::string>& a, const std::vector<std::string>& b) {
    return std::ranges::any_of(
        a, [&](const std::string& name) { return std::ranges::find(b, name) != b.end(); });
}

// Whether one system writes something the other reads or writes.
bool clash(const SystemOptions& a, const SystemOptions& b) {
    return names_meet(a.writes, b.reads) || names_meet(a.writes, b.writes) ||
           names_meet(b.writes, a.reads);
}

} // namespace

World::World(const ComponentRegistry& registry) : components_(&registry) {
    registry_.on_destroy<EntityId>().connect<&World::on_id_destroyed>(*this);
}

World::~World() {
    registry_.on_destroy<EntityId>().disconnect(this);
}

Result<entt::entity> World::create(std::string_view id) {
    if (!is_valid_id(id)) {
        return make_error(ErrorCode::InvalidArgument,
                          "invalid entity id \"" + std::string(id) +
                              "\": use 1 to 64 letters, digits, '_' or '-'");
    }
    return add(id);
}

Result<entt::entity> World::create_part(std::string_view id) {
    if (!is_valid_part_id(id)) {
        return make_error(ErrorCode::InvalidArgument,
                          "invalid id \"" + std::string(id) +
                              "\" for an entity an instanced scene brings: use the instance's ID, "
                              "'/' and its ID in the scene, such as \"hero/sprite\"");
    }
    return add(id);
}

Result<entt::entity> World::add(std::string_view id) {
    check_structural();
    if (by_id_.contains(id)) {
        return make_error(ErrorCode::InvalidArgument,
                          "entity id \"" + std::string(id) + "\" is already in use");
    }
    const entt::entity entity = registry_.create();
    registry_.emplace<EntityId>(entity, std::string(id));
    by_id_.emplace(std::string(id), entity);
    notify(std::string(kEntityCreated), id, {});
    return entity;
}

entt::entity World::create() {
    std::string id;
    do {
        id = "entity_" + std::to_string(next_generated_id_++);
    } while (by_id_.contains(id));
    return create(id).value();
}

void World::destroy(entt::entity entity) {
    check_structural();
    if (!registry_.valid(entity)) {
        return;
    }
    // The entities its instanced scene brought go with it.
    std::vector<entt::entity> parts;
    if (const auto* id = registry_.try_get<EntityId>(entity); id != nullptr) {
        const std::string prefix = id->value + "/";
        for (auto it = by_id_.lower_bound(prefix);
             it != by_id_.end() && it->first.starts_with(prefix); ++it) {
            parts.push_back(it->second);
        }
    }
    registry_.destroy(entity);
    for (const entt::entity part : parts) {
        if (registry_.valid(part)) {
            registry_.destroy(part);
        }
    }
}

void World::clear() {
    check_structural();
    registry_.clear();
    by_id_.clear();
    scene_root_.clear();
}

bool World::valid(entt::entity entity) const {
    return registry_.valid(entity);
}

entt::entity World::find(std::string_view id) const {
    const auto it = by_id_.find(id);
    return it == by_id_.end() ? entt::entity{entt::null} : it->second;
}

std::string_view World::id_of(entt::entity entity) const {
    if (!registry_.valid(entity)) {
        return {};
    }
    const auto* id = registry_.try_get<EntityId>(entity);
    return id == nullptr ? std::string_view() : std::string_view(id->value);
}

std::vector<std::string_view> World::ids() const {
    std::vector<std::string_view> out;
    out.reserve(by_id_.size());
    for (const auto& [id, entity] : by_id_) {
        out.emplace_back(id);
    }
    return out;
}

void World::add_system(std::string name, System system, SystemOptions options) {
    SLASH_ASSERT(system != nullptr, "add_system needs a callable system");
    SLASH_ASSERT(!stepping_, "systems cannot be added while the world is stepping");
    SLASH_ASSERT(std::ranges::none_of(systems_,
                                      [&](const NamedSystem& added) { return added.name == name; }),
                 "two systems have the same name");
    // After every system with the same or a lower priority, so equal priorities keep their order.
    const auto position =
        std::ranges::upper_bound(systems_, options.priority, std::less<>(),
                                 [](const NamedSystem& added) { return added.options.priority; });
    systems_.insert(position, {std::move(name), std::move(system), std::move(options), {}});
    plan_stages();
}

bool World::remove_system(std::string_view name) {
    SLASH_ASSERT(!stepping_, "systems cannot be removed while the world is stepping");
    const bool removed =
        std::erase_if(systems_, [&](const NamedSystem& added) { return added.name == name; }) > 0;
    plan_stages();
    return removed;
}

void World::plan_stages() {
    stages_.clear();
    for (std::size_t i = 0; i < systems_.size(); ++i) {
        const SystemOptions& options = systems_[i].options;
        if (options.threaded && !stages_.empty()) {
            Stage& last = stages_.back();
            const bool joins =
                systems_[last.begin].options.threaded &&
                std::none_of(
                    systems_.begin() + static_cast<std::ptrdiff_t>(last.begin),
                    systems_.begin() + static_cast<std::ptrdiff_t>(last.end),
                    [&](const NamedSystem& other) { return clash(other.options, options); });
            if (joins) {
                last.end = i + 1;
                continue;
            }
        }
        stages_.push_back({.begin = i, .end = i + 1});
    }
}

std::vector<SystemInfo> World::systems() const {
    std::vector<SystemInfo> out;
    out.reserve(systems_.size());
    for (std::size_t stage = 0; stage < stages_.size(); ++stage) {
        for (std::size_t i = stages_[stage].begin; i < stages_[stage].end; ++i) {
            const SystemOptions& options = systems_[i].options;
            out.push_back({.name = systems_[i].name,
                           .doc = options.doc,
                           .priority = options.priority,
                           .threaded = options.threaded,
                           .reads = options.reads,
                           .writes = options.writes,
                           .stage = stage,
                           .timing = systems_[i].samples.timing()});
        }
    }
    return out;
}

std::vector<std::string_view> World::system_names() const {
    std::vector<std::string_view> out;
    out.reserve(systems_.size());
    for (const NamedSystem& system : systems_) {
        out.emplace_back(system.name);
    }
    return out;
}

void World::Samples::add(double milliseconds) noexcept {
    ms.at(count % ms.size()) = static_cast<float>(milliseconds);
    ++count;
}

Timing World::Samples::timing() const noexcept {
    const std::size_t kept = std::min(count, ms.size());
    if (kept == 0) {
        return {};
    }
    Timing out;
    out.last_ms = ms.at((count - 1) % ms.size());
    double sum = 0.0;
    for (std::size_t i = 0; i < kept; ++i) {
        sum += ms.at(i);
        out.max_ms = std::max(out.max_ms, static_cast<double>(ms.at(i)));
    }
    out.avg_ms = sum / static_cast<double>(kept);
    return out;
}

void World::step(double tick_seconds) {
    SLASH_PROFILE_ZONE("World::step");
    const Clock::time_point start = Clock::now();
    events_ = std::move(next_events_);
    next_events_.clear();
    if (step_listener_) {
        step_listener_(*this, tick_seconds);
    }
    stepping_ = true;
    stepping_thread_ = std::this_thread::get_id();
    for (const Stage& stage : stages_) {
        if (systems_[stage.begin].options.threaded) {
            run_stage(stage, tick_seconds);
        } else {
            run_system(systems_[stage.begin], tick_seconds);
        }
    }
    stepping_ = false;
    ++tick_;
    tick_samples_.add(milliseconds_since(start));
}

void World::run_system(NamedSystem& system, double tick_seconds) {
    SLASH_PROFILE_ZONE_NAMED(system.name);
    const Clock::time_point start = Clock::now();
    detail::SystemRun run;
    const RunningScope scope({.world = this, .run = &run, .threaded = false});
    system.run(*this, tick_seconds);
    flush(run);
    system.samples.add(milliseconds_since(start));
}

void World::run_stage(const Stage& stage, double tick_seconds) {
    // Every storage the systems name exists before they start, so none is made while threads
    // read the registry.
    for (std::size_t i = stage.begin; i < stage.end; ++i) {
        for (const auto* names : {&systems_[i].options.reads, &systems_[i].options.writes}) {
            for (const std::string& name : *names) {
                if (const ComponentType* type = components_->find(name); type != nullptr) {
                    type->assure(*this);
                }
            }
        }
    }
    // The same whatever the thread count: each system keeps what it does for this thread, one
    // system runs alone just as a stage of several does, and the stage's events and deferred
    // calls are delivered in system order once it is done.
    std::vector<detail::SystemRun> runs(stage.end - stage.begin);
    std::vector<double> milliseconds(runs.size(), 0.0);
    const auto run_one = [&](std::size_t k) {
        const NamedSystem& system = systems_[stage.begin + k];
        SLASH_PROFILE_ZONE_NAMED(system.name);
        const Clock::time_point start = Clock::now();
        const RunningScope scope({.world = this, .run = &runs[k], .threaded = true});
        system.run(*this, tick_seconds);
        milliseconds[k] = milliseconds_since(start);
    };
    staging_ = true;
    if (runs.size() == 1) {
        run_one(0);
    } else {
        pool().parallel_for(runs.size(), run_one);
    }
    staging_ = false;
    for (std::size_t k = 0; k < runs.size(); ++k) {
        const Clock::time_point start = Clock::now();
        const RunningScope scope({.world = this, .run = &runs[k], .threaded = false});
        flush(runs[k]);
        systems_[stage.begin + k].samples.add(milliseconds[k] + milliseconds_since(start));
    }
}

void World::flush(detail::SystemRun& run) {
    // By index: deferred calls may defer more.
    for (std::size_t i = 0; i < run.actions.size(); ++i) { // NOLINT(modernize-loop-convert)
        detail::SystemRun::Action action = std::move(run.actions[i]);
        if (action.call) {
            action.call(*this);
        } else {
            deliver(std::move(action.event));
        }
    }
    run.actions.clear();
}

void World::defer(std::function<void(World&)> call) {
    SLASH_ASSERT(call != nullptr, "defer needs a callable");
    if (t_running.world == this) {
        t_running.run->actions.push_back({.event = {}, .call = std::move(call)});
        return;
    }
    SLASH_ASSERT(!stepping_, "World::defer is called on the thread running the system");
    call(*this);
}

jobs::Pool& World::pool() const {
    return pool_ != nullptr ? *pool_ : jobs::shared();
}

void World::check_structural() const {
    SLASH_ASSERT(!staging_, "threaded systems create and destroy entities through World::defer");
    SLASH_ASSERT(!stepping_ || std::this_thread::get_id() == stepping_thread_,
                 "entities are created and destroyed on the thread stepping the world");
}

void World::add_reset(std::function<void(World&)> reset) {
    resets_.push_back(std::move(reset));
}

void World::reset_state() {
    SLASH_ASSERT(!stepping_, "the world cannot be reset while it is stepping");
    for (const auto& reset : resets_) {
        reset(*this);
    }
    events_.clear();
    next_events_.clear();
}

void World::set_event_queues(EventQueues queues) {
    events_ = std::move(queues.last);
    next_events_ = std::move(queues.next);
}

bool World::is_valid_id(std::string_view id) {
    constexpr std::size_t kMaxLength = 64;
    if (id.empty() || id.size() > kMaxLength) {
        return false;
    }
    return std::ranges::all_of(id, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_' || c == '-';
    });
}

bool World::is_valid_part_id(std::string_view id) {
    constexpr std::size_t kMaxLength = 255;
    if (id.size() > kMaxLength || id.find('/') == std::string_view::npos) {
        return false;
    }
    std::size_t start = 0;
    while (true) {
        const std::size_t end = id.find('/', start);
        if (!is_valid_id(id.substr(start, end == std::string_view::npos ? end : end - start))) {
            return false;
        }
        if (end == std::string_view::npos) {
            return true;
        }
        start = end + 1;
    }
}

void World::on_id_destroyed(entt::registry& registry, entt::entity entity) {
    const std::string& id = registry.get<EntityId>(entity).value;
    const auto it = by_id_.find(id);
    if (it != by_id_.end() && it->second == entity) {
        by_id_.erase(it);
        notify(std::string(kEntityDestroyed), id, {});
    }
}

const PrefabLink* World::prefab_of(entt::entity entity) const {
    return registry_.valid(entity) ? registry_.try_get<PrefabLink>(entity) : nullptr;
}

PrefabLinks World::prefab_links() const {
    PrefabLinks links;
    for (const auto [entity, link] : registry_.view<const PrefabLink>().each()) {
        if (const std::string_view id = id_of(entity); !id.empty()) {
            links.emplace(id, link);
        }
    }
    return links;
}

std::shared_ptr<const json::Value> World::cached_prefab(std::string_view path) const {
    const auto it = prefabs_.find(path);
    return it != prefabs_.end() ? it->second : nullptr;
}

void World::cache_prefab(std::string path, std::shared_ptr<const json::Value> components) {
    prefabs_[std::move(path)] = std::move(components);
}

void World::emit(std::string kind, std::string_view id, json::Value data) {
    SLASH_ASSERT(!kind.empty(), "events need a kind");
    SLASH_ASSERT(!kind.starts_with("entity.") && kind != "command",
                 "event kinds starting with \"entity.\" and \"command\" are the engine's own");
    notify(std::move(kind), id, std::move(data));
}

void World::notify(std::string kind, std::string_view id, json::Value data) {
    Event event{std::move(kind), std::string(id), std::move(data), tick_};
    if (t_running.world == this && t_running.threaded) {
        t_running.run->actions.push_back({.event = std::move(event), .call = nullptr});
        return;
    }
    SLASH_ASSERT(!staging_ && (!stepping_ || std::this_thread::get_id() == stepping_thread_),
                 "events are emitted on the thread running the system");
    deliver(std::move(event));
}

void World::deliver(Event event) {
    if (listener_) {
        listener_(event);
    }
    if (next_events_.size() < kMaxEvents) {
        next_events_.push_back(std::move(event));
    }
}

} // namespace slash::world
