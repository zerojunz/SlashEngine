#include "internal.hpp"
#include "tree_system.hpp"

#include "slash/animation/animation.hpp"
#include "slash/animation/tree.hpp"
#include "slash/core/json.hpp"
#include "slash/core/log.hpp"
#include "slash/core/text.hpp"
#include "slash/scene3d/ik.hpp"
#include "slash/scene3d/skeleton.hpp"
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

// The names of a map's keys, for "did you mean".
template <typename Map>
std::vector<std::string_view> names_of(const Map& map) {
    std::vector<std::string_view> names;
    names.reserve(map.size());
    for (const auto& [name, value] : map) {
        names.emplace_back(name);
    }
    return names;
}

// The entity a track acts on: the player's own, or the one its targets name; null, with why in
// error, for a target that names none.
entt::entity track_entity(const world::World& world, entt::entity entity,
                          const world::AnimationPlayer& player, const Track& track,
                          std::string& error) {
    if (track.target.empty()) {
        return entity;
    }
    const auto it = player.targets.find(track.target);
    if (it == player.targets.end()) {
        error = "no entity for target \"" + track.target + "\" in the AnimationPlayer's targets";
        if (const std::string_view hint = closest_match(track.target, names_of(player.targets));
            !hint.empty()) {
            error += "; did you mean \"" + std::string(hint) + "\"?";
        }
        return entt::null;
    }
    if (it->second == entt::null || !world.valid(it->second)) {
        error = "target \"" + track.target + "\" names no entity";
        return entt::null;
    }
    return it->second;
}

// An animation of a library, or null with why in error.
const Animation* find_animation(const AnimationLibrary& library, const std::string& path,
                                const std::string& name, std::string& error) {
    const auto found = library.animations.find(name);
    if (found != library.animations.end()) {
        return &found->second;
    }
    error = path + " has no animation \"" + name + "\"";
    if (const std::string_view hint = closest_match(name, names_of(library.animations));
        !hint.empty()) {
        error += "; did you mean \"" + std::string(hint) + "\"?";
    }
    return nullptr;
}

// Where an animation track's player is in the animation a key started, the track's playback at
// time: as Godot's AnimationMixer finds it when seeking, the time since the key, held at the end,
// round a loop or back and forth.
double started_position(const Animation& started, double since) {
    const double end = started.length;
    switch (started.loop) {
    case LoopMode::None:
        return std::min(end, since);
    case LoopMode::Linear:
        return detail::fposmod(since, end);
    case LoopMode::PingPong:
        return detail::pingpong(since, end);
    }
    return 0.0;
}

// How deep animation tracks seek players that play players: as deep as any cinematic nests.
constexpr int kMaxSeekDepth = 8;

// Seeks a player and the players its animation tracks play, as Godot's
// AnimationPlayer::seek(time, true, true): sets the values its value and bezier tracks have there,
// and moves each player an animation track started to where it would be.
Result<void> seek_player(world::World& world, detail::LibraryCache& cache, entt::entity entity,
                         double time, std::string_view name, int depth) {
    auto& ecs = world.ecs();
    const std::string id(world.id_of(entity));
    if (!ecs.all_of<world::AnimationPlayer>(entity)) {
        return make_error(ErrorCode::InvalidArgument, "\"" + id + "\" has no AnimationPlayer");
    }
    const world::AnimationPlayer player = ecs.get<world::AnimationPlayer>(entity);
    if (player.library.empty()) {
        return make_error(ErrorCode::InvalidArgument,
                          "\"" + id + "\"'s AnimationPlayer has no library");
    }
    std::string error;
    detail::LibraryCache::Entry* cached =
        cache.get(player.library, [&](const std::string& message) { error = message; });
    if (cached == nullptr) {
        return make_error(ErrorCode::InvalidArgument, error);
    }
    const std::string animation_name = name.empty() ? player.animation : std::string(name);
    if (animation_name.empty()) {
        return make_error(ErrorCode::InvalidArgument,
                          "\"" + id + "\"'s AnimationPlayer has no animation; name one");
    }
    const Animation* found = find_animation(cached->library, player.library, animation_name, error);
    if (found == nullptr) {
        return make_error(ErrorCode::NotFound, error);
    }
    const Animation& animation = *found;
    if (!std::isfinite(time) || time < 0.0 || time > animation.length + kEpsilon) {
        return make_error(ErrorCode::InvalidArgument,
                          "the time is past " + animation_name + "'s length, " +
                              json::write(json::Value(animation.length), json::Style::Compact));
    }
    time = std::min(time, animation.length);
    {
        auto& stored = ecs.get<world::AnimationPlayer>(entity);
        stored.animation = animation_name;
        stored.time = time;
    }
    // Audio tracks start nothing, but streams playing stop where the seek went back before
    // their key or past their end, as Godot's update-only seek.
    detail::begin_audio(world, entity, detail::Mixer::Player);
    for (std::size_t i = 0; i < animation.tracks.size(); ++i) {
        if (animation.tracks[i].type == TrackType::Audio) {
            detail::AudioTrackTick tick;
            tick.owner = entity;
            tick.animation_name = animation_name;
            tick.animation = &animation;
            tick.track_index = i;
            tick.track = &animation.tracks[i];
            tick.time = time;
            tick.backward = player.backwards;
            static_cast<void>(detail::audio_track_tick(world, tick));
        }
    }
    detail::end_audio(world, entity, detail::Mixer::Player);
    const std::vector<detail::LibraryCache::Resolved>& resolved =
        detail::LibraryCache::targets(*cached, animation_name, animation, world.components());
    for (std::size_t i = 0; i < animation.tracks.size(); ++i) {
        const Track& track = animation.tracks[i];
        if (track.type != TrackType::Value && track.type != TrackType::Bezier &&
            track.type != TrackType::Animation) {
            continue;
        }
        std::string why;
        const entt::entity on = track_entity(world, entity, player, track, why);
        if (on == entt::null) {
            log::warn("an AnimationPlayer can't seek a track", {{"id", id}, {"error", why}});
            continue;
        }
        if (track.type == TrackType::Animation) {
            // The key at or before the time, and the animation it started there.
            const Key* key = detail::find_key(track, animation.length, time, true, true);
            if (key == nullptr || !key->value.is_string()) {
                continue;
            }
            if (auto sought = detail::seek_started(world, cache, on, key->value.as_string(),
                                                   time - key->time, depth + 1, true);
                !sought) {
                log::warn("an animation track can't seek its player",
                          {{"id", id}, {"error", sought.error().message}});
            }
            continue;
        }
        const std::optional<Target>& target = resolved[i].target;
        if (!target) {
            log::warn("an AnimationPlayer can't seek a track",
                      {{"id", id}, {"error", resolved[i].error}});
            continue;
        }
        const bool discrete = track.type == TrackType::Value &&
                              (track.update == UpdateMode::Discrete ||
                               (track.update == UpdateMode::Auto && target->discrete));
        if (auto applied = apply(world, on, *target, sample(animation, track, time, discrete));
            !applied) {
            log::warn("an AnimationPlayer can't seek a track",
                      {{"id", id}, {"error", track.path + ": " + applied.error().message}});
        }
    }
    return {};
}

} // namespace

namespace detail {

Result<bool> seek_started(world::World& world, LibraryCache& cache, entt::entity entity,
                          const std::string& name, double since, int depth, bool past_end) {
    if (name == kStopAnimation || depth > kMaxSeekDepth) {
        return false;
    }
    const auto* player = world.ecs().try_get<world::AnimationPlayer>(entity);
    if (player == nullptr || player->library.empty()) {
        return false;
    }
    std::string missing;
    LibraryCache::Entry* library =
        cache.get(player->library, [&](const std::string& message) { missing = message; });
    const Animation* started =
        library == nullptr ? nullptr
                           : find_animation(library->library, player->library, name, missing);
    if (started == nullptr) {
        return false;
    }
    if (!past_end && started->loop == LoopMode::None &&
        is_greater_or_equal_approx(since, started->length)) {
        return false;
    }
    if (auto sought =
            seek_player(world, cache, entity, started_position(*started, since), name, depth);
        !sought) {
        return sought.error();
    }
    return true;
}

Result<void> play_started(world::World& world, LibraryCache& cache, entt::entity entity,
                          const std::string& name) {
    auto& ecs = world.ecs();
    auto* player = ecs.try_get<world::AnimationPlayer>(entity);
    if (player == nullptr) {
        return make_error(ErrorCode::InvalidArgument,
                          "\"" + std::string(world.id_of(entity)) + "\" has no AnimationPlayer");
    }
    if (name == kStopAnimation) {
        player->playing = false;
        if (player->library.empty() || player->animation.empty()) {
            return {};
        }
        return seek_player(world, cache, entity, 0.0, {}, 0);
    }
    std::string error;
    LibraryCache::Entry* library =
        player->library.empty()
            ? nullptr
            : cache.get(player->library, [&](const std::string& message) { error = message; });
    if (library == nullptr) {
        return make_error(ErrorCode::InvalidArgument,
                          error.empty() ? "its AnimationPlayer has no library" : error);
    }
    const Animation* animation = find_animation(library->library, player->library, name, error);
    if (animation == nullptr) {
        return make_error(ErrorCode::NotFound, error);
    }
    if (player->animation != name) {
        player->animation = name;
        player->time = player->backwards ? animation->length : 0.0;
    }
    player->playing = true;
    return {};
}

} // namespace detail

namespace {

// Plays AnimationPlayer components. Its state is only a cache of loaded .anim.json files and
// models, the targets their tracks resolve to and the warnings already logged, so a world's
// snapshot, undo or dry run is never out of step with it.
struct PlayerState {
    explicit PlayerState(asset::Assets& assets) : libraries(assets) {}

    detail::LibraryCache libraries;
    std::set<std::string, std::less<>> warned;

    void warn_once(std::string_view id, const std::string& message) {
        if (warned.insert(std::string(id) + "\n" + message).second) {
            log::warn("an AnimationPlayer can't play", {{"id", id}, {"error", message}});
        }
    }

    detail::LibraryCache::Entry* library(const std::string& path, std::string_view id) {
        return libraries.get(path, [&](const std::string& message) { warn_once(id, message); });
    }

    void run(world::World& world, double tick_seconds) {
        detail::sweep_audio(world);
        libraries.begin();
        auto& ecs = world.ecs();
        const auto view = ecs.view<world::AnimationPlayer>();
        const std::vector<entt::entity> entities(view.begin(), view.end());
        for (const entt::entity entity : entities) {
            if (!ecs.valid(entity) || !ecs.all_of<world::AnimationPlayer>(entity)) {
                continue;
            }
            play(world, entity, tick_seconds);
        }
        libraries.end();
    }

    void play(world::World& world, entt::entity entity, double tick_seconds) {
        auto& ecs = world.ecs();
        // A copy, since a track may set the player's own fields.
        world::AnimationPlayer player = ecs.get<world::AnimationPlayer>(entity);
        if (player.library.empty() || player.animation.empty() || !player.playing) {
            // A player that stopped, paused or finished stops its audio tracks' sounds, as
            // Godot's clears its caches.
            detail::stop_audio(world, entity, detail::Mixer::Player);
        }
        if (player.library.empty() || player.animation.empty()) {
            return;
        }
        const std::string id(world.id_of(entity));
        // Loaded while paused too, since a paused player still poses its entity's model.
        detail::LibraryCache::Entry* cached = library(player.library, id);
        if (cached == nullptr || !player.playing) {
            return;
        }
        std::string missing;
        const Animation* found =
            find_animation(cached->library, player.library, player.animation, missing);
        if (found == nullptr) {
            warn_once(id, missing);
            return;
        }
        const Animation& animation = *found;
        const bool backward = (static_cast<double>(player.speed_scale) < 0.0) != player.backwards;
        const Step step = advance(player, animation, tick_seconds);
        {
            auto& stored = ecs.get<world::AnimationPlayer>(entity);
            stored.time = player.time;
            stored.playing = player.playing;
            stored.backwards = player.backwards;
        }

        // The entity a track acts on: the player's own, or the one its targets name.
        const auto entity_of = [&](const Track& track) -> entt::entity {
            std::string why;
            const entt::entity on = track_entity(world, entity, player, track, why);
            if (on == entt::null) {
                warn_once(id, why);
            }
            return on;
        };

        // Value tracks: continuous ones take their value at the new time, and discrete ones the
        // value of the last key playback passed, as Godot's update modes do. Bezier tracks are
        // continuous.
        const std::vector<detail::LibraryCache::Resolved>& resolved =
            detail::LibraryCache::targets(*cached, player.animation, animation, world.components());
        for (std::size_t i = 0; i < animation.tracks.size(); ++i) {
            const Track& track = animation.tracks[i];
            if (track.path.empty()) {
                continue;
            }
            const entt::entity on = entity_of(track);
            if (on == entt::null) {
                continue;
            }
            const std::optional<Target>& found_target = resolved[i].target;
            if (!found_target) {
                warn_once(id, resolved[i].error);
                continue;
            }
            const Target& target = *found_target;
            const bool discrete = track.type == TrackType::Value &&
                                  (track.update == UpdateMode::Discrete ||
                                   (track.update == UpdateMode::Auto && target.discrete));
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
            if (!world.valid(on)) {
                continue;
            }
            if (auto applied = apply(world, on, target, value); !applied) {
                warn_once(id, track.path + ": " + applied.error().message);
            }
        }

        // Animation tracks: the last key playback passed plays its animation on the track's
        // player, as Godot's animation playback tracks do.
        for (const Track& track : animation.tracks) {
            if (track.type != TrackType::Animation) {
                continue;
            }
            const Key* last = nullptr;
            for (const Span& span : step.spans) {
                if (const auto keys = span.keys_of(track); !keys.empty()) {
                    last = keys.back();
                }
            }
            if (last == nullptr || !last->value.is_string()) {
                continue;
            }
            const entt::entity on = entity_of(track);
            if (on == entt::null || !world.valid(entity)) {
                continue;
            }
            if (auto played = detail::play_started(world, libraries, on, last->value.as_string());
                !played) {
                warn_once(id, "target \"" + track.target + "\": " + played.error().message);
            }
        }

        // Audio tracks: the last key playback passed starts its stream on the track's audio
        // player, as Godot's do, and streams stop where playback went back before their key or
        // past their end offset.
        detail::begin_audio(world, entity, detail::Mixer::Player);
        for (std::size_t i = 0; i < animation.tracks.size(); ++i) {
            const Track& track = animation.tracks[i];
            if (track.type != TrackType::Audio) {
                continue;
            }
            detail::AudioTrackTick tick;
            tick.owner = entity;
            tick.animation_name = player.animation;
            tick.animation = &animation;
            tick.track_index = i;
            tick.track = &track;
            tick.time = player.time;
            tick.backward = backward;
            for (const Span& span : step.spans) {
                if (const auto keys = span.keys_of(track); !keys.empty()) {
                    tick.key = keys.back();
                }
            }
            if (tick.key != nullptr) {
                tick.on = entity_of(track);
            }
            if (auto played = detail::audio_track_tick(world, tick); !played) {
                warn_once(id, "an audio track's stream: " + played.error().message);
            }
        }
        detail::end_audio(world, entity, detail::Mixer::Player);

        // Event tracks: each key passed, in the order playback passed them, from the entity the
        // track acts on.
        std::vector<std::optional<std::string>> sources(animation.tracks.size());
        for (std::size_t i = 0; i < animation.tracks.size(); ++i) {
            const Track& track = animation.tracks[i];
            if (!track.event.empty() && !step.spans.empty()) {
                if (const entt::entity on = entity_of(track); on != entt::null) {
                    sources[i] = std::string(world.id_of(on));
                }
            }
        }
        for (const Span& span : step.spans) {
            struct Passed {
                const Key* key;
                const Track* track;
                const std::string* source;
            };
            std::vector<Passed> passed;
            for (std::size_t i = 0; i < animation.tracks.size(); ++i) {
                const Track& track = animation.tracks[i];
                if (track.event.empty() || !sources[i]) {
                    continue;
                }
                for (const Key* key : span.keys_of(track)) {
                    passed.push_back({key, &track, &*sources[i]});
                }
            }
            std::ranges::stable_sort(passed, [&span](const Passed& a, const Passed& b) {
                return span.to < span.from ? a.key->time > b.key->time : a.key->time < b.key->time;
            });
            for (const Passed& event : passed) {
                world.emit(event.track->event, *event.source, event.key->value);
            }
        }
        if (step.finished) {
            detail::stop_audio(world, entity, detail::Mixer::Player);
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
    assets.add_type<AnimationTreeResource>(
        "animation_tree", {std::string(kTreeSuffix)},
        [](std::string_view text, std::string_view /*path*/) -> Result<AnimationTreeResource> {
            auto doc = json::parse(text);
            if (!doc) {
                return doc.error();
            }
            return parse_tree(doc.value());
        });
}

void add_systems(world::World& world, asset::Assets& assets) {
    auto state = std::make_shared<PlayerState>(assets);
    auto trees = std::make_shared<detail::TreePlayer>(assets);
    // A tree's animations from models pose the nodes of the model its entity draws, as Godot's
    // AnimationTree drives a Skeleton3D's bones; without one, a player's animation does, playing
    // or paused, as Godot's AnimationPlayer.
    scene3d::set_clip_source(world, [state, trees](const world::World& w, entt::entity entity) {
        if (auto clips = trees->clips(w, entity)) {
            return std::move(*clips);
        }
        std::vector<scene3d::ClipSample> out;
        const auto* player = w.ecs().try_get<const world::AnimationPlayer>(entity);
        if (player == nullptr || player->library.empty() || player->animation.empty()) {
            return out;
        }
        const std::string id(w.id_of(entity));
        detail::LibraryCache::Entry* cached = state->library(player->library, id);
        if (cached == nullptr) {
            return out;
        }
        const auto found = cached->library.animations.find(player->animation);
        if (found == cached->library.animations.end()) {
            return out;
        }
        const Animation& animation = found->second;
        const double time =
            std::isfinite(player->time) ? std::clamp(player->time, 0.0, animation.length) : 0.0;
        if (!animation.from.model.empty()) {
            out.push_back({.model = animation.from.model,
                           .animation = animation.from.animation,
                           .time = time,
                           .weight = 1.0F});
        } else if (cached->nodes != nullptr && has_node_tracks(animation)) {
            // Its node tracks, as the library's model of them has them.
            out.push_back({.model = player->library,
                           .animation = player->animation,
                           .time = time,
                           .weight = 1.0F,
                           .source = cached->nodes});
        }
        return out;
    });
    world.add_reset([](world::World& w) { detail::reset_audio(w); });
    world.add_system("animation_player",
                     [state](world::World& w, double tick_seconds) { state->run(w, tick_seconds); },
                     {.doc = "Plays each AnimationPlayer's animation on its entity's components.",
                      .priority = 900});
    world.add_system("animation_tree",
                     [trees](world::World& w, double tick_seconds) { trees->run(w, tick_seconds); },
                     {.doc = "Blends each AnimationTree's animations into its entity's components.",
                      .priority = 910});
    // IK modifiers turn bones after the animations, as Godot's SkeletonModifier3Ds process after
    // the AnimationMixer.
    scene3d::add_modifier_system(
        world, [&assets](const std::string& path) -> Result<std::shared_ptr<const scene3d::Model>> {
            auto found = assets.load<scene3d::Model>(path);
            if (!found) {
                return found.error();
            }
            return found.value().share();
        });
}

Result<void> seek(world::World& world, asset::Assets& assets, entt::entity entity, double time,
                  std::string_view animation) {
    if (!world.valid(entity)) {
        return make_error(ErrorCode::InvalidArgument, "no such entity");
    }
    detail::LibraryCache cache(assets);
    return seek_player(world, cache, entity, time, animation, 0);
}

void install(world::World& world, asset::Assets& assets) {
    add_asset_types(assets);
    add_systems(world, assets);
}

} // namespace slash::animation
