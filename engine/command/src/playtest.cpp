#include "playtest.hpp"

#include "selection.hpp"

#include "slash/animation/animation.hpp"
#include "slash/command/session.hpp"
#include "slash/core/jobs.hpp"
#include "slash/core/json.hpp"
#include "slash/core/result.hpp"
#include "slash/core/text.hpp"
#include "slash/input/input.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/scene.hpp"
#include "slash/world/world.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// playtest.run: many headless copies of the game at once. Each is a session of its own, started
// from the same world with a combination of tuning values set, played by a scripted bot that
// presses input actions, and stopped by the first end condition it meets; the runs are summed up
// per combination.
namespace slash::command {
namespace {

constexpr std::int64_t kDefaultTicks = 600;
constexpr double kDefaultTickSeconds = 1.0 / 60.0;
// The most runs one call makes: every combination of the values, times runs of each.
constexpr std::size_t kMaxRuns = 256;
// The most runs alive at once, well within the 128 worlds Box2D holds.
constexpr std::size_t kMaxWorldsAtOnce = 32;
// What the summary calls runs that met no end condition before their ticks ran out.
constexpr std::string_view kTimeout = "timeout";
constexpr std::string_view kPrefabSuffix = ".prefab.json";

json::Value typed(std::string_view type, std::string_view description) {
    json::Value schema;
    schema["type"] = type;
    schema["description"] = description;
    return schema;
}

std::string quoted(std::string_view text) {
    return "\"" + std::string(text) + "\"";
}

std::int64_t integer(const json::Value& params, std::string_view key, std::int64_t fallback) {
    const json::Value* value = params.find(key);
    return value != nullptr ? value->as_int() : fallback;
}

double number(const json::Value& params, std::string_view key, double fallback) {
    const json::Value* value = params.find(key);
    return value != nullptr ? value->as_double() : fallback;
}

Reply param_error(int error_code, std::string_view path, const std::string& message) {
    json::Value data;
    data["path"] = path;
    return Reply::failure(error_code, std::string(path) + ": " + message, std::move(data));
}

Reply param_error(const Error& error, std::string_view path) {
    return param_error(Reply::failure(error).code, path, error.message);
}

// An error about part of the params, moved under where that part is in playtest.run's.
Reply nested(Reply reply, std::string_view at) {
    json::Value* path = reply.data.find("path");
    const std::string inner = path != nullptr && path->is_string() ? path->as_string() : "";
    if (reply.message.starts_with(inner + ": ")) {
        reply.message.erase(0, inner.size() + 2);
    }
    return param_error(reply.code, std::string(at) + inner, reply.message);
}

// A whole number as an integer, so summaries read 600 rather than 600.0.
json::Value number_value(double value) {
    constexpr double kWhole = 1.0e15;
    if (value == std::trunc(value) && std::abs(value) < kWhole) {
        return static_cast<std::int64_t>(value);
    }
    return value;
}

// Means to three decimals: enough to compare values, few tokens.
json::Value mean_value(double sum, std::size_t count) {
    constexpr double kScale = 1000.0;
    return number_value(std::round(sum / static_cast<double>(count) * kScale) / kScale);
}

// {"min", "mean", "max"} of some numbers, at least one.
json::Value spread(const std::vector<double>& values) {
    double sum = 0.0;
    for (const double value : values) {
        sum += value;
    }
    json::Value out;
    out["min"] = number_value(*std::ranges::min_element(values));
    out["mean"] = mean_value(sum, values.size());
    out["max"] = number_value(*std::ranges::max_element(values));
    return out;
}

// ---- Fields to set and read ----

// Where a vary or measure key leads: an entity's field ("player/Player/jump_speed"), part of one
// ("player/Transform2D/position/y"), the field on every entity with the component
// ("*/Enemy/speed"), the field in a prefab file and so its instances
// ("prefabs/bat.prefab.json/Enemy/speed"), or for measure, how many entities have a component
// ("*/Coin").
struct Target {
    std::string entity;
    animation::Target field;                       // its component is null when counting
    const world::ComponentType* counted = nullptr; // the component "*/<Component>" counts

    [[nodiscard]] bool every() const { return entity == "*"; }
    [[nodiscard]] bool prefab() const { return entity.ends_with(kPrefabSuffix); }
};

Result<Target> parse_target(std::string_view key, const world::ComponentRegistry& components,
                            bool measuring) {
    std::vector<std::string_view> segments;
    for (std::size_t start = 0; start <= key.size();) {
        const std::size_t end = std::min(key.find('/', start), key.size());
        segments.push_back(key.substr(start, end - start));
        start = end + 1;
    }
    // Entity IDs may hold "/", so the component is the first segment after one that names one.
    std::size_t at = 1;
    while (at < segments.size() && components.find(segments[at]) == nullptr) {
        ++at;
    }
    if (at >= segments.size() || segments[0].empty()) {
        std::string message = "name an entity, then a component and its field, as "
                              "\"player/Transform2D/position/y\"";
        if (segments.size() > 1) {
            std::vector<std::string_view> names;
            for (const auto& type : components.types()) {
                names.emplace_back(type->name());
            }
            message += "; there is no component " + quoted(segments[1]);
            if (const std::string_view hint = closest_match(segments[1], names); !hint.empty()) {
                message += "; did you mean " + quoted(hint) + "?";
            }
        }
        return make_error(ErrorCode::InvalidArgument, std::move(message));
    }
    Target target;
    const auto component_at = static_cast<std::size_t>(segments[at].data() - key.data());
    target.entity = std::string(key.substr(0, component_at - 1));
    if (measuring && target.prefab()) {
        return make_error(ErrorCode::InvalidArgument,
                          "measure an entity's field, or count entities with \"*/<Component>\"");
    }
    if (measuring && target.every()) {
        if (at + 1 != segments.size()) {
            return make_error(ErrorCode::InvalidArgument,
                              "\"*\" counts the entities with a component, as \"*/Enemy\"; name "
                              "an entity to read a field");
        }
        target.counted = components.find(segments[at]);
        return target;
    }
    auto field = animation::resolve_target(key.substr(component_at), components);
    if (!field) {
        return field.error();
    }
    target.field = std::move(field).value();
    return target;
}

// The part of an entity's field a target names, or nullopt if the entity lacks it.
std::optional<json::Value> read_field(const world::World& world, entt::entity entity,
                                      const animation::Target& field) {
    if (entity == entt::null || !field.component->has(world, entity)) {
        return std::nullopt;
    }
    const json::Value fields = field.component->get(world, entity);
    const json::Value* node = fields.find(field.field);
    for (const std::string& part : field.part) {
        if (node == nullptr) {
            return std::nullopt;
        }
        if (node->is_array()) {
            std::size_t index = 0;
            std::from_chars(part.data(), part.data() + part.size(), index);
            node = index < node->size() ? &node->as_array()[index] : nullptr;
        } else {
            node = node->find(part);
        }
    }
    if (node == nullptr) {
        return std::nullopt;
    }
    return *node;
}

std::optional<json::Value> read(const world::World& world, const Target& target) {
    if (target.counted != nullptr) {
        std::int64_t count = 0;
        for (const std::string_view id : world.ids()) {
            count += target.counted->has(world, world.find(id)) ? 1 : 0;
        }
        return json::Value(count);
    }
    return read_field(world, world.find(target.entity), target.field);
}

// Sets a target's part in a prefab file's components, starting from the field's default when
// the file leaves the field out.
void set_in_prefab(json::Value& components, const animation::Target& field, json::Value value) {
    json::Value& fields = components[field.component->name()];
    json::Value* node = &fields[field.field];
    if (node->is_null() && !field.part.empty()) {
        if (const json::Value* fallback = field.component->defaults().find(field.field);
            fallback != nullptr) {
            *node = *fallback;
        }
    }
    for (const std::string& part : field.part) {
        if (node->is_array()) {
            std::size_t index = 0;
            std::from_chars(part.data(), part.data() + part.size(), index);
            if (index >= node->size()) {
                return;
            }
            node = &node->as_array()[index];
        } else {
            node = &(*node)[part];
        }
    }
    *node = std::move(value);
}

// ---- Conditions and bots ----

// Entities to look for, as run.until's: met when any entity matches, or with "until": "none"
// when none does.
struct Condition {
    detail::Selection selection;
    bool none = false;

    [[nodiscard]] bool holds(const world::World& world) const {
        return detail::any_selected(world, selection) != none;
    }
};

std::optional<Reply> parse_condition(const world::ComponentRegistry& components,
                                     const json::Value& params, const std::string& at,
                                     Condition& out) {
    if (params.find("with") == nullptr && params.find("without") == nullptr &&
        params.find("where") == nullptr) {
        return param_error(code::kInvalidParams, at,
                           "give with, without or where: the entities to look for");
    }
    if (auto failed = detail::parse_selection(components, params, out.selection)) {
        return nested(std::move(*failed), at);
    }
    const json::Value* until = params.find("until");
    out.none = until != nullptr && until->as_string() == "none";
    return std::nullopt;
}

// One line of a bot's script: when it presses an action, and for how long.
struct Press {
    std::string action;
    std::int64_t from = 0;
    std::int64_t to = std::numeric_limits<std::int64_t>::max();
    std::int64_t every = 0;        // presses every this many ticks from `from`; 0 if not
    double chance = 0.0;           // presses at random with this chance each tick; 0 if not
    std::optional<Condition> when; // presses while this holds
    std::int64_t hold = 0;         // ticks each press lasts; 0 for until `to`
    float strength = 1.0F;
};

// A bot playing one run: before each tick, presses and releases its script's actions.
class Bot {
public:
    Bot(const std::vector<Press>& script, std::uint64_t seed)
        : script_(&script), seed_(seed), left_(script.size(), 0) {}

    void before_tick(world::World& world, std::int64_t tick) {
        std::map<std::string_view, float> wanted;
        for (std::size_t i = 0; i < script_->size(); ++i) {
            const Press& press = (*script_)[i];
            if (left_[i] == 0 && tick >= press.from && tick < press.to &&
                starts(press, i, world, tick)) {
                left_[i] = press.hold > 0 ? press.hold : press.to - tick;
            }
            if (left_[i] > 0) {
                --left_[i];
                float& strength = wanted[press.action];
                strength = std::max(strength, press.strength);
            }
        }
        for (const auto& [action, strength] : wanted) {
            const auto held = held_.find(action);
            if (held == held_.end() || held->second != strength) {
                input::action_press(world, action, strength);
            }
        }
        for (const auto& [action, strength] : held_) {
            if (!wanted.contains(action)) {
                input::action_release(world, action);
            }
        }
        held_ = std::move(wanted);
    }

private:
    // Whether a press starts this tick: every trigger the line gives holds, and a line with
    // none presses once, at `from`.
    [[nodiscard]] bool starts(const Press& press, std::size_t line, const world::World& world,
                              std::int64_t tick) const {
        if (press.every == 0 && press.chance == 0.0 && !press.when) {
            return tick == press.from;
        }
        return (press.every == 0 || (tick - press.from) % press.every == 0) &&
               (press.chance == 0.0 || random(line, tick) < press.chance) &&
               (!press.when || press.when->holds(world));
    }

    // A number in [0, 1) from the run's seed, the line and the tick, the same on every machine:
    // splitmix64's mix of the three.
    [[nodiscard]] double random(std::size_t line, std::int64_t tick) const {
        constexpr std::uint64_t kGolden = 0x9E3779B97F4A7C15ULL;
        constexpr std::uint64_t kMix1 = 0xBF58476D1CE4E5B9ULL;
        constexpr std::uint64_t kMix2 = 0x94D049BB133111EBULL;
        constexpr double kUnit = 0x1.0p-53;
        std::uint64_t x = (seed_ * kGolden) ^ (static_cast<std::uint64_t>(line) * kMix1) ^
                          (static_cast<std::uint64_t>(tick) * kMix2);
        x = (x ^ (x >> 30U)) * kMix1;
        x = (x ^ (x >> 27U)) * kMix2;
        x ^= x >> 31U;
        return static_cast<double>(x >> 11U) * kUnit;
    }

    const std::vector<Press>* script_;
    std::uint64_t seed_;
    std::vector<std::int64_t> left_; // ticks each line's press still lasts
    std::map<std::string_view, float> held_;
};

// ---- The plan ----

struct Varied {
    std::string key;
    Target target;
    std::vector<json::Value> values;
    std::vector<json::Value> fitted; // as the field takes them, such as 3 for 3.2 in an int
};

struct Plan {
    std::string scene; // the scene file runs open, or "" for the session's world
    std::vector<Varied> vary;
    std::size_t combinations = 1;
    std::size_t runs = 1;
    std::uint64_t seed = 0;
    std::vector<Press> bot;
    std::vector<std::pair<std::string, Condition>> ends;
    std::int64_t ticks = kDefaultTicks;
    double tick_seconds = kDefaultTickSeconds;
    bool count_all = true; // every event gameplay code emits
    std::vector<std::string> count;
    std::vector<std::pair<std::string, Target>> measure;

    // Which of a key's values a combination takes: the first key's change slowest.
    [[nodiscard]] std::size_t choice(std::size_t combination, std::size_t key) const {
        std::size_t stride = 1;
        for (std::size_t later = key + 1; later < vary.size(); ++later) {
            stride *= vary[later].values.size();
        }
        return (combination / stride) % vary[key].values.size();
    }

    [[nodiscard]] bool counts(std::string_view kind) const {
        if (count_all) {
            return !kind.starts_with("entity.") && kind != "command";
        }
        return std::ranges::find(count, kind) != count.end();
    }
};

std::optional<Reply> parse_vary(Session& host, const json::Value& vary, Plan& plan) {
    const world::ComponentRegistry& components = host.components();
    // Values are tried on an entity of their own first, so a bad one fails before any run.
    world::World scratch(components);
    const entt::entity probe = scratch.create("probe").value();
    for (const auto& [key, values] : vary.as_object()) {
        const std::string at = "/vary/" + json::pointer_escape(key);
        auto target = parse_target(key, components, false);
        if (!target) {
            return param_error(target.error(), at);
        }
        Varied varied{.key = key, .target = std::move(target).value(), .values = {}, .fitted = {}};
        if (const Target& found = varied.target; found.prefab()) {
            auto base = world::resolve_prefab(host.world(), found.entity);
            if (!base) {
                return param_error(base.error(), at);
            }
            if (base.value()->find(found.field.component->name()) == nullptr) {
                return param_error(code::kNotFound, at,
                                   found.entity + " has no " + found.field.component->name());
            }
        }
        for (std::size_t i = 0; i < values.size(); ++i) {
            const json::Value& value = values.as_array()[i];
            if (auto applied = animation::apply(scratch, probe, varied.target.field, value);
                !applied) {
                return param_error(code::kInvalidParams, at + "/" + std::to_string(i),
                                   applied.error().message);
            }
            varied.values.push_back(value);
            varied.fitted.push_back(
                read_field(scratch, probe, varied.target.field).value_or(value));
        }
        plan.combinations *= varied.values.size();
        plan.vary.push_back(std::move(varied));
    }
    return std::nullopt;
}

std::optional<Reply> parse_bot(Session& host, const json::Value& bot, Plan& plan) {
    const input::Map* map = input::map(host.world());
    for (std::size_t i = 0; i < bot.size(); ++i) {
        const json::Value& line = bot.as_array()[i];
        const std::string at = "/bot/" + std::to_string(i);
        Press press;
        press.action = line.find("action")->as_string();
        if (map != nullptr && !map->has_action(press.action)) {
            std::string message = "no input action is named " + quoted(press.action);
            if (const std::string_view hint = closest_match(press.action, map->names());
                !hint.empty()) {
                message += "; did you mean " + quoted(hint) + "?";
            }
            return param_error(code::kNotFound, at + "/action", message);
        }
        press.from = integer(line, "from", 0);
        press.to = integer(line, "to", press.to);
        if (press.to <= press.from) {
            return param_error(code::kInvalidParams, at + "/to", "must be after from");
        }
        press.every = integer(line, "every", 0);
        press.chance = number(line, "chance", 0.0);
        press.hold = integer(line, "for", 0);
        press.strength = static_cast<float>(number(line, "strength", 1.0));
        if (const json::Value* when = line.find("when"); when != nullptr) {
            Condition condition;
            if (auto failed = parse_condition(host.components(), *when, at + "/when", condition)) {
                return failed;
            }
            press.when = std::move(condition);
        }
        if (press.hold == 0 && (press.every > 0 || press.chance > 0.0 || press.when)) {
            press.hold = 1;
        }
        plan.bot.push_back(std::move(press));
    }
    return std::nullopt;
}

std::optional<Reply> parse_plan(Session& host, const json::Value& params, Plan& plan) {
    if (const json::Value* scene = params.find("scene"); scene != nullptr) {
        plan.scene = scene->as_string();
    }
    plan.runs = static_cast<std::size_t>(integer(params, "runs", 1));
    plan.seed = static_cast<std::uint64_t>(integer(params, "seed", 0));
    plan.ticks = integer(params, "ticks", kDefaultTicks);
    plan.tick_seconds = number(params, "tick_seconds", kDefaultTickSeconds);
    const json::Value* vary = params.find("vary");
    if (vary != nullptr) {
        if (auto failed = parse_vary(host, *vary, plan)) {
            return failed;
        }
    }
    if (plan.combinations * plan.runs > kMaxRuns) {
        return param_error(code::kInvalidParams, vary != nullptr ? "/vary" : "/runs",
                           std::to_string(plan.combinations) + " combinations of values times " +
                               std::to_string(plan.runs) + " runs of each is " +
                               std::to_string(plan.combinations * plan.runs) + " runs; at most " +
                               std::to_string(kMaxRuns));
    }
    if (const json::Value* bot = params.find("bot"); bot != nullptr) {
        if (auto failed = parse_bot(host, *bot, plan)) {
            return failed;
        }
    }
    if (const json::Value* ends = params.find("end"); ends != nullptr) {
        for (const auto& [name, condition] : ends->as_object()) {
            const std::string at = "/end/" + json::pointer_escape(name);
            if (name == kTimeout) {
                return param_error(code::kInvalidParams, at,
                                   "\"timeout\" names the runs that meet no end condition; pick "
                                   "another name");
            }
            Condition parsed;
            if (auto failed = parse_condition(host.components(), condition, at, parsed)) {
                return failed;
            }
            plan.ends.emplace_back(name, std::move(parsed));
        }
    }
    if (const json::Value* count = params.find("count"); count != nullptr) {
        plan.count_all = false;
        for (const json::Value& kind : count->as_array()) {
            plan.count.push_back(kind.as_string());
        }
    }
    if (const json::Value* measure = params.find("measure"); measure != nullptr) {
        for (std::size_t i = 0; i < measure->size(); ++i) {
            const std::string& key = measure->as_array()[i].as_string();
            auto target = parse_target(key, host.components(), true);
            if (!target) {
                return param_error(target.error(), "/measure/" + std::to_string(i));
            }
            plan.measure.emplace_back(key, std::move(target).value());
        }
    }
    return std::nullopt;
}

// ---- Runs ----

// One copy of the game, in a session of its own, and what happened in it.
struct Run {
    std::size_t combination = 0;
    std::uint64_t seed = 0;
    // A pool with no threads: runs go in parallel, each stepping on one thread.
    std::unique_ptr<jobs::Pool> pool;
    std::unique_ptr<Session> session;
    std::int64_t ticks = 0;
    std::string ended; // the end condition it met, or "" if its ticks ran out
    std::map<std::string, std::int64_t, std::less<>> events;
    std::vector<std::optional<json::Value>> measured;
};

// What a run's session may do: what the playtest's may, but write files, export or call
// models, so runs leave the project as it was and answer models from the replay file.
SessionOptions run_options(const Session& host) {
    SessionOptions options = host.options();
    options.capture_logs = false;
    options.write_files = false;
    std::erase(options.capabilities, Capability::Build);
    std::erase(options.capabilities, Capability::Network);
    return options;
}

// Sets an entity target's value on its entity, or on every entity with its component.
std::optional<Reply> set_on_entities(world::World& world, const Varied& varied,
                                     const json::Value& value) {
    const std::string at = "/vary/" + json::pointer_escape(varied.key);
    const animation::Target& field = varied.target.field;
    std::vector<entt::entity> entities;
    if (varied.target.every()) {
        for (const std::string_view id : world.ids()) {
            if (const entt::entity entity = world.find(id); field.component->has(world, entity)) {
                entities.push_back(entity);
            }
        }
        if (entities.empty()) {
            return param_error(code::kNotFound, at,
                               "no entity has " + field.component->name() + " to set");
        }
    } else {
        const entt::entity entity = world.find(varied.target.entity);
        if (entity == entt::null) {
            std::string message = "no entity has the id " + quoted(varied.target.entity);
            if (const std::string_view hint = closest_match(varied.target.entity, world.ids());
                !hint.empty()) {
                message += "; did you mean " + quoted(hint) + "?";
            }
            return param_error(code::kNotFound, at, message);
        }
        if (!field.component->has(world, entity)) {
            return param_error(code::kNotFound, at,
                               varied.target.entity + " has no " + field.component->name());
        }
        entities.push_back(entity);
    }
    for (const entt::entity entity : entities) {
        if (auto applied = animation::apply(world, entity, field, value); !applied) {
            return param_error(code::kInvalidParams, at, applied.error().message);
        }
    }
    return std::nullopt;
}

// Makes a run's session: the world it starts from, with its combination's values set.
std::optional<Reply> set_up(Session& host, const Plan& plan, const Session::SavedWorld& saved,
                            Run& run) {
    run.pool = std::make_unique<jobs::Pool>(0);
    run.session = std::make_unique<Session>(host.components(), host.commands(), run_options(host));
    Session& session = *run.session;
    world::World& world = session.world();
    world.set_pool(*run.pool);

    // Prefab values go in as the prefab files are read, so instances made while the game runs
    // have them too.
    using Overrides = std::vector<std::pair<const animation::Target*, json::Value>>;
    std::map<std::string, Overrides, std::less<>> prefabs;
    for (std::size_t k = 0; k < plan.vary.size(); ++k) {
        const Varied& varied = plan.vary[k];
        if (varied.target.prefab()) {
            prefabs[varied.target.entity].emplace_back(
                &varied.target.field, varied.fitted[plan.choice(run.combination, k)]);
        }
    }
    if (!prefabs.empty()) {
        world.set_file_reader(
            [read = world.file_reader(), prefabs](std::string_view path) -> Result<std::string> {
                auto text = read(path);
                const auto found = prefabs.find(path);
                if (!text || found == prefabs.end()) {
                    return text;
                }
                auto doc = json::parse(text.value());
                if (!doc || !doc.value().is_object()) {
                    return text; // loading it reports what is wrong
                }
                json::Value& components = doc.value()["components"];
                for (const auto& [field, value] : found->second) {
                    set_in_prefab(components, *field, value);
                }
                return json::write(doc.value(), json::Style::Compact);
            });
    }

    if (!plan.scene.empty()) {
        json::Value open;
        open["path"] = plan.scene;
        if (Reply opened = session.call("scene.open", open); !opened.ok) {
            // scene.open's path param is this one's scene.
            constexpr std::string_view kOwnPath = "/path: ";
            if (opened.message.starts_with(kOwnPath)) {
                opened.message.erase(0, kOwnPath.size());
            }
            return param_error(opened.code, "/scene", opened.message);
        }
    } else if (prefabs.empty()) {
        session.restore(saved);
        session.set_scene_path(host.scene_path());
    } else {
        // Entities that extend a prefab take their base from it as this run reads it.
        Session::SavedWorld changed = saved;
        for (auto& [id, link] : changed.links) {
            if (link.path.ends_with(kPrefabSuffix)) {
                if (auto base = world::resolve_prefab(world, link.path)) {
                    link.base = std::move(base).value();
                }
            }
        }
        session.restore(changed);
        session.set_scene_path(host.scene_path());
    }

    for (std::size_t k = 0; k < plan.vary.size(); ++k) {
        const Varied& varied = plan.vary[k];
        if (varied.target.prefab()) {
            continue;
        }
        if (auto failed =
                set_on_entities(world, varied, varied.values[plan.choice(run.combination, k)])) {
            return failed;
        }
    }
    world.set_event_listener([&run, &plan](const world::Event& event) {
        if (plan.counts(event.kind)) {
            ++run.events[event.kind];
        }
    });
    return std::nullopt;
}

// Plays a run until it meets an end condition or its ticks run out, then measures it.
void play(const Plan& plan, Run& run) {
    world::World& world = run.session->world();
    Bot bot(plan.bot, run.seed);
    const auto ended = [&]() -> const std::string* {
        for (const auto& [name, condition] : plan.ends) {
            if (condition.holds(world)) {
                return &name;
            }
        }
        return nullptr;
    };
    const std::string* end = ended();
    while (end == nullptr && run.ticks < plan.ticks) {
        bot.before_tick(world, run.ticks);
        world.step(plan.tick_seconds);
        ++run.ticks;
        end = ended();
    }
    if (end != nullptr) {
        run.ended = *end;
    }
    for (const auto& [key, target] : plan.measure) {
        run.measured.push_back(read(world, target));
    }
}

// ---- The summary ----

// A measured value over runs: {"min", "mean", "max"} for numbers, else how many runs ended with
// each value; "missing" counts runs whose entity or component was gone.
json::Value summarize_measure(const std::vector<const std::optional<json::Value>*>& values) {
    std::vector<double> numbers;
    std::map<std::string, std::int64_t> others;
    std::int64_t missing = 0;
    for (const std::optional<json::Value>* value : values) {
        if (!value->has_value()) {
            ++missing;
        } else if ((*value)->is_number()) {
            numbers.push_back((*value)->as_double());
        } else {
            ++others[json::write(**value, json::Style::Compact)];
        }
    }
    json::Value out = json::Object{};
    if (others.empty() && !numbers.empty()) {
        out = spread(numbers);
    } else {
        for (const double value : numbers) {
            ++others[json::write(number_value(value), json::Style::Compact)];
        }
        for (const auto& [text, count] : others) {
            out[text] = count;
        }
    }
    if (missing > 0) {
        out["missing"] = missing;
    }
    return out;
}

json::Value summarize(const Plan& plan, std::size_t combination,
                      const std::vector<const Run*>& runs, const std::vector<std::string>& kinds) {
    json::Value out;
    if (!plan.vary.empty()) {
        json::Value& values = out["values"];
        for (std::size_t k = 0; k < plan.vary.size(); ++k) {
            values[plan.vary[k].key] = plan.vary[k].values[plan.choice(combination, k)];
        }
    }
    if (!plan.ends.empty()) {
        json::Value& ends = out["ends"];
        for (const auto& end : plan.ends) {
            ends[end.first] = std::int64_t{0};
        }
        ends[kTimeout] = std::int64_t{0};
        for (const Run* run : runs) {
            json::Value& count = ends[run->ended.empty() ? kTimeout : run->ended];
            count = count.as_int() + 1;
        }
    }
    std::vector<double> ticks;
    ticks.reserve(runs.size());
    for (const Run* run : runs) {
        ticks.push_back(static_cast<double>(run->ticks));
    }
    out["ticks"] = spread(ticks);
    if (!kinds.empty()) {
        json::Value& events = out["events"];
        for (const std::string& kind : kinds) {
            double sum = 0.0;
            for (const Run* run : runs) {
                const auto found = run->events.find(kind);
                sum += found != run->events.end() ? static_cast<double>(found->second) : 0.0;
            }
            events[kind] = mean_value(sum, runs.size());
        }
    }
    if (!plan.measure.empty()) {
        json::Value& measure = out["measure"];
        for (std::size_t m = 0; m < plan.measure.size(); ++m) {
            std::vector<const std::optional<json::Value>*> values;
            values.reserve(runs.size());
            for (const Run* run : runs) {
                values.push_back(&run->measured[m]);
            }
            measure[plan.measure[m].first] = summarize_measure(values);
        }
    }
    return out;
}

Reply playtest_run(Session& host, const json::Value& params) {
    Plan plan;
    if (auto failed = parse_plan(host, params, plan)) {
        return std::move(*failed);
    }
    const Session::SavedWorld saved = plan.scene.empty() ? host.save() : Session::SavedWorld{};
    const std::size_t total = plan.combinations * plan.runs;
    // Sessions are made and dropped on this thread, a wave at a time, and their worlds step on
    // the job system's threads at once.
    jobs::Pool& pool = jobs::shared();
    const std::size_t wave =
        std::min(static_cast<std::size_t>(pool.thread_count()) + 1, kMaxWorldsAtOnce);
    std::vector<std::unique_ptr<Run>> done;
    done.reserve(total);
    for (std::size_t first = 0; first < total; first += wave) {
        std::vector<std::unique_ptr<Run>> runs;
        for (std::size_t i = first; i < std::min(first + wave, total); ++i) {
            auto run = std::make_unique<Run>();
            run->combination = i / plan.runs;
            run->seed = plan.seed + (i % plan.runs);
            if (auto failed = set_up(host, plan, saved, *run)) {
                return std::move(*failed);
            }
            runs.push_back(std::move(run));
        }
        pool.parallel_for(runs.size(), [&](std::size_t i) { play(plan, *runs[i]); });
        for (std::unique_ptr<Run>& run : runs) {
            run->session.reset();
            run->pool.reset();
            done.push_back(std::move(run));
        }
    }

    std::vector<std::string> kinds = plan.count;
    if (plan.count_all) {
        for (const std::unique_ptr<Run>& run : done) {
            for (const auto& [kind, count] : run->events) {
                kinds.push_back(kind);
            }
        }
        std::ranges::sort(kinds);
        const auto repeated = std::ranges::unique(kinds);
        kinds.erase(repeated.begin(), repeated.end());
    }
    json::Array results;
    for (std::size_t combination = 0; combination < plan.combinations; ++combination) {
        std::vector<const Run*> runs;
        for (const std::unique_ptr<Run>& run : done) {
            if (run->combination == combination) {
                runs.push_back(run.get());
            }
        }
        results.push_back(summarize(plan, combination, runs, kinds));
    }
    json::Value result;
    result["runs"] = static_cast<std::int64_t>(total);
    result["results"] = std::move(results);
    return Reply::success(std::move(result));
}

// ---- Registration ----

json::Value integer_schema(std::string_view description, std::int64_t minimum) {
    json::Value schema = typed("integer", description);
    schema["minimum"] = minimum;
    return schema;
}

// Entities to look for, as run.until's; defined once and referred to, since it is long.
json::Value condition_schema() {
    json::Object properties = detail::selection_properties();
    json::Value schema;
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["properties"]["with"] = properties.at("with");
    schema["properties"]["without"] = properties.at("without");
    schema["properties"]["where"] = properties.at("where");
    json::Value until = typed("string", "Met when any entity matches, or none does.");
    until["enum"] = json::Array{"any", "none"};
    schema["properties"]["until"] = std::move(until);
    return schema;
}

json::Value condition_ref(std::string_view description) {
    json::Value schema;
    schema["$ref"] = "#/$defs/condition";
    schema["description"] = description;
    return schema;
}

json::Value fraction(std::string_view description) {
    json::Value schema = typed("number", description);
    schema["exclusiveMinimum"] = 0;
    schema["maximum"] = 1;
    return schema;
}

json::Value params_schema() {
    json::Value vary =
        typed("object", "Values to try by \"<entity>/<Component>/<field>\" (a part: "
                        "\"player/Transform2D/position/y\"); \"*\" for every entity with the "
                        "component, or a .prefab.json path. Every combination runs.");
    vary["maxProperties"] = 8;
    vary["additionalProperties"]["type"] = "array";
    vary["additionalProperties"]["minItems"] = 1;

    json::Value press = typed("object", "Pressed at from, or when all its triggers hold.");
    press["additionalProperties"] = false;
    press["required"] = json::Array{"action"};
    press["properties"]["action"] = typed("string", "Input action.");
    press["properties"]["from"] = integer_schema("First tick it may press.", 0);
    press["properties"]["to"] = integer_schema("Tick it stops at.", 1);
    press["properties"]["every"] = integer_schema("Trigger: every n ticks.", 1);
    press["properties"]["chance"] = fraction("Trigger: this chance each tick.");
    press["properties"]["when"] = condition_ref("Trigger: while entities match.");
    press["properties"]["for"] =
        integer_schema("Ticks a press lasts; default 1, or until to with no trigger.", 1);
    press["properties"]["strength"] = fraction("How far it is pressed; default 1.");
    json::Value bot = typed("array", "Input actions the bot presses.");
    bot["items"] = std::move(press);

    json::Value end = typed("object", "Conditions that end a run, by name; a run that meets none "
                                      "is a \"timeout\".");
    end["additionalProperties"] = condition_ref("Entities to look for, as run.until's.");

    json::Value seconds = fraction("Seconds per tick; default 1/60.");
    json::Value count = typed("array", "Event kinds to count; default: gameplay events.");
    count["items"]["type"] = "string";
    json::Value measure =
        typed("array", "Values to read at each run's end, as vary's keys; \"*/<Component>\" "
                       "counts entities.");
    measure["items"]["type"] = "string";

    json::Value runs = integer_schema("Runs of each combination; default 1.", 1);
    runs["maximum"] = 64;
    json::Value ticks = integer_schema("Most ticks a run lasts; default 600.", 1);
    ticks["maximum"] = 100000;

    json::Value params;
    params["type"] = "object";
    params["additionalProperties"] = false;
    params["properties"]["scene"] = typed("string", "Scene file runs open instead of the world.");
    params["properties"]["vary"] = std::move(vary);
    params["properties"]["runs"] = std::move(runs);
    params["properties"]["seed"] = integer_schema("Run n uses seed + n for chance presses.", 0);
    params["properties"]["bot"] = std::move(bot);
    params["properties"]["end"] = std::move(end);
    params["properties"]["ticks"] = std::move(ticks);
    params["properties"]["tick_seconds"] = std::move(seconds);
    params["properties"]["count"] = std::move(count);
    params["properties"]["measure"] = std::move(measure);
    params["$defs"]["condition"] = condition_schema();
    return params;
}

} // namespace

void register_playtest_commands(Registry& registry) {
    registry.add({
        .name = "playtest.run",
        .description =
            "Plays many headless copies of the game at once to tune values (jump height, enemy "
            "speed): each from the open world with one combination of vary set and a bot pressing "
            "input actions, until an end condition or ticks run out. Returns per combination how "
            "runs ended, ticks, event counts and measured values.",
        .capabilities = {Capability::Run},
        .effect = Effect::Read,
        .params = params_schema(),
        .handler = playtest_run,
    });
}

} // namespace slash::command
