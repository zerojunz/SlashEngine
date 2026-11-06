#pragma once

// What the animation module's sources share: files read for checks, the cache of loaded
// libraries both systems play from, Godot's float comparisons and key searches, and the
// triangulation of blend spaces.

#include "slash/animation/animation.hpp"
#include "slash/asset/assets.hpp"
#include "slash/core/math.hpp"
#include "slash/core/result.hpp"
#include "slash/scene3d/model.hpp"
#include "slash/world/component_registry.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace slash::animation::detail {

// Godot's CMP_EPSILON, and its comparisons with it, as Math:: and Animation:: have them.
inline constexpr double kCmpEpsilon = 0.00001;
[[nodiscard]] bool is_equal_approx(double a, double b);
[[nodiscard]] bool is_zero_approx(double value);
// The same on floats, as Godot compares weights, which are real_t.
[[nodiscard]] bool is_equal_approx(float a, float b);
[[nodiscard]] bool is_zero_approx(float value);
[[nodiscard]] bool is_less_approx(double a, double b);
[[nodiscard]] bool is_less_or_equal_approx(double a, double b);
[[nodiscard]] bool is_greater_approx(double a, double b);
[[nodiscard]] bool is_greater_or_equal_approx(double a, double b);
// Godot's Math::fposmod and Math::pingpong.
[[nodiscard]] double fposmod(double x, double y);
[[nodiscard]] double pingpong(double value, double length);

// Godot's Variant::zero for the values fields take: what a deterministic tree gives a value
// track no animation plays, without a RESET animation; null for values it leaves alone.
[[nodiscard]] json::Value zero_of(const json::Value& value);

// Which end of a loop playback crossed in a tick, as Godot's Animation::LoopedFlag.
enum class LoopedFlag : std::uint8_t { None, End, Start };

// The keys of a track that playback passed in a tick that ended at time after moving delta, in
// the order it passed them, as Godot's Animation::track_get_key_indices_in_range: those after
// where it was, up to and including where it got to, round a loop's end.
[[nodiscard]] std::vector<const Key*> keys_in_range(const Track& track, double length,
                                                    LoopMode loop, double time, double delta,
                                                    LoopedFlag looped);
// The key at a time, as Godot's Animation::track_find_key: exactly there, or else the last one
// at or before it (nearest); null for none.
[[nodiscard]] const Key* find_key(const Track& track, double length, double time, bool nearest,
                                  bool limit);

// .anim.json files and models read for checks, each once.
class CheckedFiles {
public:
    explicit CheckedFiles(const world::FileReader& reader) : reader_(reader) {}

    [[nodiscard]] const Result<scene3d::Model>& model(const std::string& path);
    // A .anim.json file's animations, those from models given their lengths and loops, or a
    // model's animations; the error has the JSON Pointer of what is wrong.
    [[nodiscard]] const Result<AnimationLibrary>& library(const std::string& path);
    // A model's animation, or null when the model doesn't load or hasn't one by that name.
    [[nodiscard]] const scene3d::ModelAnimation* clip(const std::string& model,
                                                      const std::string& animation);

private:
    const world::FileReader& reader_;
    std::map<std::string, Result<scene3d::Model>, std::less<>> models_;
    std::map<std::string, Result<AnimationLibrary>, std::less<>> libraries_;
};

// Libraries loaded through assets for playing: .anim.json files, their animations from models
// given those models' lengths and loops, or models' animations, rebuilt when a file changes.
class LibraryCache {
public:
    // A track's target, or why it has none.
    struct Resolved {
        std::optional<Target> target;
        std::string error;
    };
    struct Entry {
        asset::Handle<AnimationLibrary> file; // a .anim.json library
        asset::Handle<scene3d::Model> model;  // or a model's animations
        // The models its animations come from, by path.
        std::map<std::string, asset::Handle<scene3d::Model>, std::less<>> models;
        // The library, as of the versions of its files in versions.
        AnimationLibrary library;
        // Its node tracks as a model's animations (node_animations), or null for none.
        std::shared_ptr<const scene3d::Model> nodes;
        std::vector<std::uint64_t> versions;
        // Which build of the library this is: no two builds of any library share one.
        std::uint64_t version = 0;
        std::uint64_t resolved = 0; // the version targets were resolved for
        std::uint64_t types = 0;    // the component registry's revision when targets resolved
        // By animation name, one per track.
        std::map<std::string, std::vector<Resolved>, std::less<>> targets;
        bool used = false;
    };
    using Warn = std::function<void(const std::string& message)>;

    explicit LibraryCache(asset::Assets& assets) : assets_(&assets) {}

    // The library at a path, loading it the first time, or null (with a warning) when it doesn't
    // load. Marks it used.
    Entry* get(const std::string& path, const Warn& warn);
    // A model loaded through the assets, or null (with a warning), and its version. Marks it
    // used.
    [[nodiscard]] const scene3d::Model* model(const std::string& path, const Warn& warn,
                                              std::uint64_t* version = nullptr);
    // The targets of an animation's tracks, resolved once per version of its library and of the
    // component types, which change when a game's code is reloaded.
    static const std::vector<Resolved>& targets(Entry& entry, const std::string& name,
                                                const Animation& animation,
                                                const world::ComponentRegistry& components);
    // Marks every library unused; end() then drops those not used since.
    void begin();
    void end();

private:
    void refresh(Entry& entry, const std::string& path, const Warn& warn);

    asset::Assets* assets_;
    std::uint64_t builds_ = 0;
    std::map<std::string, Entry, std::less<>> entries_;
    struct UsedModel {
        asset::Handle<scene3d::Model> handle;
        bool used = false;
    };
    std::map<std::string, UsedModel, std::less<>> models_;
};

// One audio track's tick, as Godot's AnimationMixer::_blend_process takes it: the owner (the
// player or tree playing it), the entity whose audio player it plays on, where the animation is,
// how much it counts, and the key to start, if any (seeked starts it from the middle).
// What plays an animation's audio tracks: an AnimationPlayer or an AnimationTree, which an entity
// can have both of, as Godot's are nodes of their own.
enum class Mixer : std::uint8_t { Player, Tree };
struct AudioTrackTick {
    entt::entity owner = entt::null;
    Mixer mixer = Mixer::Player;
    entt::entity on = entt::null;
    std::string animation_name;
    const Animation* animation = nullptr;
    std::size_t track_index = 0;
    const Track* track = nullptr;
    double time = 0.0;
    bool backward = false;
    float blend = 1.0F;
    const Key* key = nullptr;
    bool seeked = false;
};
// Before an owner's tick: its audio tracks count for nothing until ticked, as _blend_init.
void begin_audio(world::World& world, entt::entity owner, Mixer mixer);
// Notes where an audio track is and starts its key's stream; fails if that can't play.
Result<void> audio_track_tick(world::World& world, const AudioTrackTick& tick);
// After an owner's tick, as _blend_apply: stops the streams playback went back before or past
// their end offsets, and sets the others' volumes by how much their animation counted.
void end_audio(world::World& world, entt::entity owner, Mixer mixer);
// Stops every stream an owner's audio tracks play, as _clear_audio_streams when a player stops.
void stop_audio(world::World& world, entt::entity owner, Mixer mixer);
// Stops those of owners that went away.
void sweep_audio(world::World& world);
// Forgets every stream, for World::reset_state.
void reset_audio(world::World& world);

// Plays an animation on an animation track's player, as Godot's AnimationPlayer::play: another
// animation starts from its start (its end, playing backward), the same one goes on, or starts
// again once it ended; "[stop]" stops it and moves it back to the start, as stop() does.
Result<void> play_started(world::World& world, LibraryCache& cache, entt::entity entity,
                          const std::string& name);
// Seeks an animation track's player to where the animation a key started since seconds ago is,
// as Godot's mixer seeks an animation track's AnimationPlayer: sets what its tracks set there,
// and those of the players its own animation tracks start, depth players down. False for
// "[stop]", an animation the player lacks, or, unless past_end, one that doesn't loop and ended
// since.
Result<bool> seek_started(world::World& world, LibraryCache& cache, entt::entity entity,
                          const std::string& name, double since, int depth, bool past_end);

// Godot's Delaunay2D::triangulate, as AnimationNodeBlendSpace2D adds its triangles: each sorted,
// the same three points only once, in the order it made them.
[[nodiscard]] std::vector<std::array<int, 3>> delaunay(const std::vector<Vec2>& input);

} // namespace slash::animation::detail
