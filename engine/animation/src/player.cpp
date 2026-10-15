#include "slash/animation/animation.hpp"
#include "slash/core/json.hpp"
#include "slash/core/log.hpp"
#include "slash/core/text.hpp"
#include "slash/world/components.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace slash::animation {
namespace {

// Time added up tick by tick drifts by tiny amounts, as 1/60 has no exact binary form. Within this
// of a key or an end, playback has reached it, so a key at 0.5 seconds is passed on tick 30 at
// 60 Hz and a half-second animation ends on that tick.
constexpr double kEpsilon = 1.0e-9;

// A stretch of the animation that playback passed in one tick, from `from` to `to`, which runs
// backward when to < from. As in Godot, it passes the keys after where playback was, up to and
// including where it got to, and the key at `from` too when with_from is set: when playback
// starts at the start of the animation.
struct Span {
    double from = 0.0;
    double to = 0.0;
    bool with_from = false;

    [[nodiscard]] bool contains(double time) const {
        if (to >= from) {
            return (with_from ? time >= from - kEpsilon : time > from + kEpsilon) &&
                   time <= to + kEpsilon;
        }
        return (with_from ? time <= from + kEpsilon : time < from - kEpsilon) &&
               time >= to - kEpsilon;
    }

    // The keys of a track in the span, in the order playback passed them.
    [[nodiscard]] std::vector<const Key*> keys_of(const Track& track) const {
        std::vector<const Key*> out;
        for (const Key& key : track.keys) {
            if (contains(key.time)) {
                out.push_back(&key);
            }
        }
        if (to < from) {
            std::ranges::reverse(out);
        }
        return out;
    }
};

// One tick of playback: where it went, and whether an animation that doesn't loop finished.
struct Step {
    std::vector<Span> spans;
    bool finished = false;
};

// Moves a player on by one tick, as Godot's AnimationPlayer does: time runs at speed_scale,
// backward while backwards is set. An animation that doesn't loop stops at its end, and plays
// again from the start if asked to play from there; a linear loop wraps to the start, passing
// keys at both; a pingpong loop turns round at each end and flips backwards. Whole loops skipped
// in one tick pass no keys.
Step advance(world::AnimationPlayer& player, const Animation& animation, double tick_seconds) {
    Step step;
    const double length = animation.length;
    double time = std::isfinite(player.time) ? std::clamp(player.time, 0.0, length) : 0.0;
    const double delta =
        tick_seconds * static_cast<double>(player.speed_scale) * (player.backwards ? -1.0 : 1.0);
    if (delta == 0.0 || !std::isfinite(delta)) {
        player.time = time;
        return step;
    }
    const bool forward = delta > 0.0;
    if (animation.loop == LoopMode::None) {
        if (forward && time >= length - kEpsilon) {
            time = 0.0;
        } else if (!forward && time <= kEpsilon) {
            time = length;
        }
    }
    // Whether playback starts at the start of the animation, for the direction it plays in.
    const bool at_start = forward ? time <= kEpsilon : time >= length - kEpsilon;
    switch (animation.loop) {
    case LoopMode::None: {
        double next = time + delta;
        if (forward ? next >= length - kEpsilon : next <= kEpsilon) {
            next = forward ? length : 0.0;
            step.finished = true;
        }
        step.spans.push_back({time, next, at_start});
        time = next;
        break;
    }
    case LoopMode::Linear: {
        const double next = time + delta;
        if (forward ? next < length - kEpsilon : next > kEpsilon) {
            step.spans.push_back({time, next, at_start});
            time = next;
            break;
        }
        // The end and the start are one moment of a loop, so a key at either is passed. Landing
        // on the end stays there, and the next tick starts the next loop.
        const double edge = forward ? length : 0.0;
        const double over = std::fmod(std::max(0.0, forward ? next - length : -next), length);
        step.spans.push_back({time, edge, at_start});
        if (over <= kEpsilon) {
            time = edge;
            break;
        }
        time = forward ? over : length - over;
        step.spans.push_back({length - edge, time, true});
        break;
    }
    case LoopMode::PingPong: {
        double remaining = std::fmod(std::abs(delta), 2.0 * length);
        bool ahead = forward;
        bool with_from = at_start;
        while (true) {
            const double edge = ahead ? length : 0.0;
            const double room = std::abs(edge - time);
            // Landing on an end stays there; the next tick turns round.
            if (remaining <= room + kEpsilon) {
                const double next = std::abs(remaining - room) <= kEpsilon
                                        ? edge
                                        : time + (ahead ? remaining : -remaining);
                step.spans.push_back({time, next, with_from});
                time = next;
                break;
            }
            step.spans.push_back({time, edge, with_from});
            remaining -= room;
            time = edge;
            ahead = !ahead;
            player.backwards = !player.backwards;
            with_from = false;
        }
        break;
    }
    }
    player.time = time;
    if (step.finished) {
        player.playing = false;
    }
    return step;
}

// Plays AnimationPlayer components. Its state is only a cache of loaded .anim.json files, the
// targets their tracks resolve to and the warnings already logged, so a world's snapshot, undo or
// dry run is never out of step with it.
struct PlayerState {
    asset::Assets* assets = nullptr;

    // A track's target, or why it has none.
    struct Resolved {
        std::optional<Target> target;
        std::string error;
    };
    struct Cached {
        asset::Handle<AnimationLibrary> library;
        std::uint64_t version = 0;
        // By animation name, one per track.
        std::map<std::string, std::vector<Resolved>, std::less<>> targets;
        bool used = false;
    };
    std::map<std::string, Cached, std::less<>> cache;
    std::set<std::string, std::less<>> warned;

    void warn_once(std::string_view id, const std::string& message) {
        if (warned.insert(std::string(id) + "\n" + message).second) {
            log::warn("an AnimationPlayer can't play", {{"id", id}, {"error", message}});
        }
    }

    Cached* library(const std::string& path, std::string_view id) {
        if (auto it = cache.find(path); it != cache.end()) {
            it->second.used = true;
            return &it->second;
        }
        auto loaded = assets->load<AnimationLibrary>(path);
        if (!loaded) {
            warn_once(id, path + ": " + loaded.error().message);
            return nullptr;
        }
        Cached& cached = cache[path];
        cached.library = std::move(loaded).value();
        cached.used = true;
        return &cached;
    }

    // The targets of an animation's tracks, resolved once per version of its file.
    static const std::vector<Resolved>& targets(Cached& cached, const std::string& name,
                                                const Animation& animation,
                                                const world::ComponentRegistry& components) {
        if (cached.version != cached.library.version()) {
            cached.targets.clear();
            cached.version = cached.library.version();
        }
        auto [it, added] = cached.targets.try_emplace(name);
        if (added) {
            it->second.reserve(animation.tracks.size());
            for (const Track& track : animation.tracks) {
                Resolved resolved;
                if (!track.path.empty()) {
                    auto target = resolve_target(track.path, components);
                    if (target) {
                        resolved.target = std::move(target).value();
                    } else {
                        resolved.error = track.path + ": " + target.error().message;
                    }
                }
                it->second.push_back(std::move(resolved));
            }
        }
        return it->second;
    }

    void run(world::World& world, double tick_seconds) {
        for (auto& [path, cached] : cache) {
            cached.used = false;
        }
        auto& ecs = world.ecs();
        const auto view = ecs.view<world::AnimationPlayer>();
        const std::vector<entt::entity> entities(view.begin(), view.end());
        for (const entt::entity entity : entities) {
            if (!ecs.valid(entity) || !ecs.all_of<world::AnimationPlayer>(entity)) {
                continue;
            }
            play(world, entity, tick_seconds);
        }
        std::erase_if(cache, [](const auto& entry) { return !entry.second.used; });
    }

    void play(world::World& world, entt::entity entity, double tick_seconds) {
        auto& ecs = world.ecs();
        // A copy, since a track may set the player's own fields.
        world::AnimationPlayer player = ecs.get<world::AnimationPlayer>(entity);
        if (!player.playing || player.library.empty() || player.animation.empty()) {
            return;
        }
        const std::string id(world.id_of(entity));
        Cached* cached = library(player.library, id);
        if (cached == nullptr) {
            return;
        }
        const AnimationLibrary& library = *cached->library;
        const auto found = library.animations.find(player.animation);
        if (found == library.animations.end()) {
            std::vector<std::string_view> names;
            names.reserve(library.animations.size());
            for (const auto& [name, animation] : library.animations) {
                names.emplace_back(name);
            }
            std::string message = player.library + " has no animation \"" + player.animation + "\"";
            if (const std::string_view hint = closest_match(player.animation, names);
                !hint.empty()) {
                message += "; did you mean \"" + std::string(hint) + "\"?";
            }
            warn_once(id, message);
            return;
        }
        const Animation& animation = found->second;
        const Step step = advance(player, animation, tick_seconds);
        {
            auto& stored = ecs.get<world::AnimationPlayer>(entity);
            stored.time = player.time;
            stored.playing = player.playing;
            stored.backwards = player.backwards;
        }

        // Value tracks: continuous ones take their value at the new time, and discrete ones the
        // value of the last key playback passed, as Godot's update modes do.
        const std::vector<Resolved>& resolved =
            targets(*cached, found->first, animation, world.components());
        for (std::size_t i = 0; i < animation.tracks.size(); ++i) {
            const Track& track = animation.tracks[i];
            if (track.path.empty()) {
                continue;
            }
            const std::optional<Target>& found_target = resolved[i].target;
            if (!found_target) {
                warn_once(id, resolved[i].error);
                continue;
            }
            const Target& target = *found_target;
            const bool discrete = track.update == UpdateMode::Discrete ||
                                  (track.update == UpdateMode::Auto && target.discrete);
            json::Value value;
            if (discrete) {
                const Key* last = nullptr;
                for (const Span& span : step.spans) {
                    if (const auto keys = span.keys_of(track); !keys.empty()) {
                        last = keys.back();
                    }
                }
                if (last == nullptr) {
                    continue;
                }
                value = last->value;
            } else {
                value = sample(animation, track, player.time, false);
            }
            if (!world.valid(entity)) {
                return;
            }
            if (auto applied = apply(world, entity, target, value); !applied) {
                warn_once(id, track.path + ": " + applied.error().message);
            }
        }

        // Event tracks: each key passed, in the order playback passed them.
        for (const Span& span : step.spans) {
            struct Passed {
                const Key* key;
                const Track* track;
            };
            std::vector<Passed> passed;
            for (const Track& track : animation.tracks) {
                if (track.event.empty()) {
                    continue;
                }
                for (const Key* key : span.keys_of(track)) {
                    passed.push_back({key, &track});
                }
            }
            std::ranges::stable_sort(passed, [&span](const Passed& a, const Passed& b) {
                return span.to < span.from ? a.key->time > b.key->time : a.key->time < b.key->time;
            });
            for (const Passed& event : passed) {
                world.emit(event.track->event, id, event.key->value);
            }
        }
        if (step.finished) {
            json::Value data;
            data["animation"] = player.animation;
            world.emit("animation_finished", id, std::move(data));
        }
    }
};

} // namespace

void add_asset_types(asset::Assets& assets) {
    assets.add_type<AnimationLibrary>(
        "animation", {std::string(kAnimationSuffix)},
        [](std::string_view text, std::string_view /*path*/) -> Result<AnimationLibrary> {
            auto doc = json::parse(text);
            if (!doc) {
                return doc.error();
            }
            return parse_animation_library(doc.value());
        });
}

void add_systems(world::World& world, asset::Assets& assets) {
    auto state = std::make_shared<PlayerState>();
    state->assets = &assets;
    world.add_system("animation_player",
                     [state](world::World& w, double tick_seconds) { state->run(w, tick_seconds); },
                     {.doc = "Plays each AnimationPlayer's animation on its entity's components.",
                      .priority = 900});
}

void install(world::World& world, asset::Assets& assets) {
    add_asset_types(assets);
    add_systems(world, assets);
}

} // namespace slash::animation
