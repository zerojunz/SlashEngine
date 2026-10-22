#include "tree_system.hpp"

#include "internal.hpp"
#include "tree_run.hpp"

#include "slash/animation/animation.hpp"
#include "slash/animation/tree.hpp"
#include "slash/core/json.hpp"
#include "slash/core/log.hpp"
#include "slash/core/text.hpp"
#include "slash/world/components.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <numbers>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace slash::animation::detail {
namespace {

constexpr double kTau = 2.0 * std::numbers::pi;
constexpr double kPi = std::numbers::pi;

// The kinds of track a tree mixes, as Godot's track caches.
enum class TrackKind : std::uint8_t { Value, Event, Node };

// One of a tree's tracks, which every animation's tracks of the same name share, as Godot's
// AnimationMixer::TrackCache: a value track's path, an event track's event or a model node.
struct TreeTrack {
    TrackKind kind = TrackKind::Value;
    std::string name;   // as filters name it
    std::string target; // a name in the AnimationTree's targets; "" for its own entity
    // A value track's field, or why it has none.
    std::optional<Target> resolved;
    std::string error;
    json::Value init;       // a value track's value while no animation plays it
    bool angle = false;     // some animation turns it the short way round
    scene3d::NodePose rest; // a model node's
};

// How an animation's tracks map to the tree's.
struct AnimationTracks {
    std::string name; // in the tree
    const Animation* animation = nullptr;
    std::vector<int> tracks; // the tree track of each of the animation's tracks
    // The model animation it plays, and the tree track of each of that animation's tracks.
    const scene3d::Model* model = nullptr;
    const scene3d::ModelAnimation* clip = nullptr;
    std::vector<int> node_tracks;
    std::vector<int> unique; // the tree tracks it has, each once
};

// What a tree's tracks are, built once per version of the tree, its libraries and the component
// types.
struct TreeTable {
    std::vector<TreeTrack> tracks;
    std::map<std::string, AnimationTracks, std::less<>> animations;
    std::vector<std::vector<int>> filters; // for each of the tree's nodes
};

// What the table was built from.
struct TableKey {
    std::string tree;
    std::uint64_t tree_version = 0;
    std::vector<std::pair<std::string, std::uint64_t>> libraries; // path and build
    std::uint64_t types = 0;

    friend bool operator==(const TableKey&, const TableKey&) = default;
};

// What the system keeps for an entity between ticks: its table, and which value tracks went back
// to their rest values, as Godot's TrackCacheValue::is_init. It is only a cache: an entity whose
// state is empty starts over.
struct EntityCache {
    TableKey key;
    TreeTable table;
    std::vector<bool> is_init;
    bool used = false;
};

struct LoadedTree {
    asset::Handle<AnimationTreeResource> handle;
    std::uint64_t version = 0;
    std::vector<TreeParameter> parameters;
    bool used = false;
};

// Godot's Animation::is_variant_interpolatable, for the values fields take: numbers, booleans and
// arrays of numbers. As Godot does, booleans blend as 0 and 1.
bool interpolatable(const json::Value& value) {
    if (value.is_number() || value.is_bool()) {
        return true;
    }
    if (!value.is_array() || value.as_array().empty()) {
        return false;
    }
    return std::ranges::all_of(value.as_array(),
                               [](const json::Value& item) { return item.is_number(); });
}

// Godot's cast_to_blendwise: a value as numbers.
std::optional<std::vector<double>> blendwise(const json::Value& value) {
    if (value.is_number()) {
        return std::vector<double>{value.as_double()};
    }
    if (value.is_bool()) {
        return std::vector<double>{value.as_bool() ? 1.0 : 0.0};
    }
    if (!interpolatable(value)) {
        return std::nullopt;
    }
    std::vector<double> out;
    out.reserve(value.size());
    for (const json::Value& item : value.as_array()) {
        out.push_back(item.as_double());
    }
    return out;
}

// Whether a field's items are whole numbers, as a Vec2i's.
bool integer_items(const Target& target) {
    const json::Value* items = target.schema.find("items");
    if (items == nullptr) {
        return false;
    }
    const json::Value* type = items->find("type");
    return type != nullptr && type->is_string() && type->as_string() == "integer";
}

// Godot's cast_from_blendwise: numbers back as the init value's kind.
json::Value from_blendwise(const std::vector<double>& values, const json::Value& init,
                           const Target& target) {
    if (init.is_bool()) {
        return !values.empty() && values[0] >= 0.5;
    }
    if (!init.is_array()) {
        return values.empty() ? 0.0 : values[0];
    }
    const bool whole = integer_items(target);
    json::Array out;
    out.reserve(values.size());
    for (const double value : values) {
        if (whole) {
            out.emplace_back(static_cast<std::int64_t>(std::llround(value)));
        } else {
            out.emplace_back(value);
        }
    }
    return out;
}

// Godot's Math::fposmod on floats.
float fposmod_float(float x, float y) {
    float value = std::fmod(x, y);
    if ((value < 0.0F && y > 0.0F) || (value > 0.0F && y < 0.0F)) {
        value += y;
    }
    return value;
}

// Godot's Vector3 with Quaternion math on Vec3.
Vec3 vec_of(const std::array<float, 4>& v) {
    return {v[0], v[1], v[2]};
}

Quat quat_of(const std::array<float, 4>& v) {
    return {v[0], v[1], v[2], v[3]};
}

// A track's value as a model node track stores it.
std::array<float, 4> sample_node(const scene3d::NodeTrack& track,
                                 const scene3d::ModelAnimation& clip, double time) {
    return scene3d::sample_track(track, time, clip.length, clip.loop);
}

std::uint64_t hash_id(std::string_view id) {
    std::uint64_t hash = 14695981039346656037ULL; // FNV-1a
    for (const char c : id) {
        hash = (hash ^ static_cast<unsigned char>(c)) * 1099511628211ULL;
    }
    return hash;
}

bool near(float a, float b) {
    return std::abs(a - b) <= 0.000001F;
}

} // namespace

struct TreePlayer::State {
    explicit State(asset::Assets& store) : assets(&store), libraries(store) {}

    asset::Assets* assets;
    LibraryCache libraries;
    std::map<std::string, LoadedTree, std::less<>> trees;
    std::map<std::string, EntityCache, std::less<>> entities; // by entity ID
    std::set<std::string, std::less<>> warned;

    void warn_once(std::string_view id, const std::string& message) {
        if (warned.insert(std::string(id) + "\n" + message).second) {
            log::warn("an AnimationTree can't play", {{"id", id}, {"error", message}});
        }
    }

    // The tree at a path, loading it the first time; the error says why it doesn't load.
    Result<LoadedTree*> tree(const std::string& path) {
        auto it = trees.find(path);
        if (it == trees.end()) {
            auto loaded = assets->load<AnimationTreeResource>(path);
            if (!loaded) {
                return loaded.error();
            }
            LoadedTree loaded_tree;
            loaded_tree.handle = std::move(loaded).value();
            it = trees.emplace(path, std::move(loaded_tree)).first;
        }
        LoadedTree& tree = it->second;
        tree.used = true;
        if (tree.parameters.empty() || tree.version != tree.handle.version()) {
            tree.parameters = tree_parameters(*tree.handle);
            tree.version = tree.handle.version();
        }
        return &tree;
    }

    // The model an animation from a model plays from, through the library it is in.
    static const scene3d::Model* clip_model(const LibraryCache::Entry& entry,
                                            const std::string& library_path,
                                            const Animation& animation) {
        if (animation.from.model.empty()) {
            return nullptr;
        }
        if (entry.model && animation.from.model == library_path) {
            return &*entry.model;
        }
        const auto found = entry.models.find(animation.from.model);
        return found != entry.models.end() && found->second ? &*found->second : nullptr;
    }

    // The libraries of a tree, by name, loaded.
    std::vector<std::pair<const std::string*, LibraryCache::Entry*>>
    load_libraries(const world::AnimationTree& tree, std::string_view id) {
        std::vector<std::pair<const std::string*, LibraryCache::Entry*>> out;
        out.reserve(tree.libraries.size());
        for (const auto& [name, path] : tree.libraries) {
            out.emplace_back(&name, libraries.get(path, [&](const std::string& message) {
                warn_once(id, message);
            }));
        }
        return out;
    }

    // Builds an entity's track table, as Godot's AnimationMixer::_update_caches.
    static void
    build(EntityCache& cache, const AnimationTreeResource& resource,
          const world::AnimationTree& tree,
          const std::vector<std::pair<const std::string*, LibraryCache::Entry*>>& loaded,
          const world::ComponentRegistry& components) {
        TreeTable table;
        std::map<std::pair<TrackKind, std::string>, int> by_name;
        const auto track_of = [&](TrackKind kind, const std::string& name) -> std::pair<int, bool> {
            const auto [it, added] =
                by_name.try_emplace({kind, name}, static_cast<int>(table.tracks.size()));
            if (added) {
                TreeTrack track;
                track.kind = kind;
                track.name = name;
                table.tracks.push_back(std::move(track));
            }
            return {it->second, added};
        };
        // RESET, in the library named "", gives value tracks their rest values.
        const Animation* reset = nullptr;
        for (const auto& [name, entry] : loaded) {
            if (name->empty() && entry != nullptr) {
                if (const auto it = entry->library.animations.find("RESET");
                    it != entry->library.animations.end()) {
                    reset = &it->second;
                }
            }
        }
        for (const auto& [library_name, entry] : loaded) {
            if (entry == nullptr) {
                continue;
            }
            const std::string& library_path = tree.libraries.at(*library_name);
            for (const auto& [animation_name, animation] : entry->library.animations) {
                AnimationTracks out;
                out.name =
                    library_name->empty() ? animation_name : *library_name + "/" + animation_name;
                out.animation = &animation;
                std::set<int> unique;
                for (const Track& track : animation.tracks) {
                    if (track.path.empty()) {
                        const auto [index, added] =
                            track_of(TrackKind::Event, event_track_name(track));
                        if (added) {
                            table.tracks[static_cast<std::size_t>(index)].target = track.target;
                        }
                        out.tracks.push_back(index);
                        unique.insert(index);
                        continue;
                    }
                    const auto [index, added] = track_of(TrackKind::Value, value_track_name(track));
                    TreeTrack& cached = table.tracks[static_cast<std::size_t>(index)];
                    if (added) {
                        cached.target = track.target;
                        if (auto target = resolve_target(track.path, components)) {
                            cached.resolved = std::move(target).value();
                        } else {
                            cached.error = track.path + ": " + target.error().message;
                        }
                        cached.init =
                            track.keys.empty() ? json::Value() : zero_of(track.keys[0].value);
                        if (reset != nullptr) {
                            const auto found =
                                std::ranges::find_if(reset->tracks, [&](const Track& t) {
                                    return !t.path.empty() && value_track_name(t) == cached.name;
                                });
                            if (found != reset->tracks.end() && !found->keys.empty()) {
                                cached.init = found->keys[0].value;
                            }
                        }
                    }
                    cached.angle = cached.angle ||
                                   track.interpolation == Interpolation::LinearAngle ||
                                   track.interpolation == Interpolation::CubicAngle;
                    out.tracks.push_back(index);
                    unique.insert(index);
                }
                out.model = clip_model(*entry, library_path, animation);
                if (out.model != nullptr) {
                    const auto found =
                        std::ranges::find(out.model->animations, animation.from.animation,
                                          &scene3d::ModelAnimation::name);
                    if (found != out.model->animations.end()) {
                        out.clip = &*found;
                    }
                }
                if (out.clip != nullptr) {
                    for (const scene3d::NodeTrack& track : out.clip->tracks) {
                        if (track.node >= out.model->nodes.size()) {
                            out.node_tracks.push_back(-1);
                            continue;
                        }
                        const scene3d::ModelNode& node = out.model->nodes[track.node];
                        const auto [index, added] = track_of(TrackKind::Node, node.name);
                        if (added) {
                            table.tracks[static_cast<std::size_t>(index)].rest = {
                                .position = node.position,
                                .rotation = node.rotation,
                                .scale = node.scale};
                        }
                        out.node_tracks.push_back(index);
                        unique.insert(index);
                    }
                }
                out.unique.assign(unique.begin(), unique.end());
                table.animations.insert_or_assign(out.name, std::move(out));
            }
        }
        // Filters name tracks of any kind.
        std::map<std::string, std::vector<int>, std::less<>> named;
        for (std::size_t i = 0; i < table.tracks.size(); ++i) {
            named[table.tracks[i].name].push_back(static_cast<int>(i));
        }
        table.filters.resize(resource.nodes.size());
        for (std::size_t i = 0; i < resource.nodes.size(); ++i) {
            for (const std::string& filter : resource.nodes[i].filters) {
                if (const auto found = named.find(filter); found != named.end()) {
                    table.filters[i].insert(table.filters[i].end(), found->second.begin(),
                                            found->second.end());
                }
            }
        }
        cache.table = std::move(table);
        cache.is_init.assign(cache.table.tracks.size(), false);
    }

    void run(world::World& world, double tick_seconds) {
        libraries.begin();
        for (auto& [path, tree] : trees) {
            tree.used = false;
        }
        for (auto& [id, cache] : entities) {
            cache.used = false;
        }
        auto& ecs = world.ecs();
        const auto view = ecs.view<world::AnimationTree>();
        const std::vector<entt::entity> list(view.begin(), view.end());
        for (const entt::entity entity : list) {
            if (ecs.valid(entity) && ecs.all_of<world::AnimationTree>(entity)) {
                play(world, entity, tick_seconds);
            }
        }
        libraries.end();
        std::erase_if(trees, [](const auto& tree) { return !tree.second.used; });
        std::erase_if(entities, [](const auto& cache) { return !cache.second.used; });
    }

    // Puts what an AnimationTree reports back to what it reports while it plays nothing.
    static void clear_report(world::AnimationTree& tree, bool with_state) {
        const world::AnimationTree defaults;
        if (with_state) {
            tree.state.clear();
        }
        tree.playing.clear();
        tree.root_motion_position = defaults.root_motion_position;
        tree.root_motion_rotation = defaults.root_motion_rotation;
        tree.root_motion_scale = defaults.root_motion_scale;
        tree.root_motion_position_accumulator = defaults.root_motion_position_accumulator;
        tree.root_motion_rotation_accumulator = defaults.root_motion_rotation_accumulator;
        tree.root_motion_scale_accumulator = defaults.root_motion_scale_accumulator;
    }

    void play(world::World& world, entt::entity entity, double tick_seconds) {
        auto& ecs = world.ecs();
        // A copy, since a track may set the tree's own fields.
        world::AnimationTree tree = ecs.get<world::AnimationTree>(entity);
        const std::string id(world.id_of(entity));
        if (!tree.active || tree.tree_root.empty()) {
            auto& stored = ecs.get<world::AnimationTree>(entity);
            clear_report(stored, true);
            stored.invalid.clear();
            return;
        }
        auto loaded_tree = this->tree(tree.tree_root);
        if (!loaded_tree) {
            const std::string message = tree.tree_root + ": " + loaded_tree.error().message;
            warn_once(id, message);
            auto& stored = ecs.get<world::AnimationTree>(entity);
            clear_report(stored, false);
            stored.invalid = message;
            return;
        }
        const LoadedTree& source = *loaded_tree.value();
        const AnimationTreeResource& resource = *source.handle;
        const auto loaded = load_libraries(tree, id);

        EntityCache& cache = entities[id];
        cache.used = true;
        TableKey key;
        key.tree = tree.tree_root;
        key.tree_version = source.handle.version();
        key.types = world.components().revision();
        for (const auto& [name, entry] : loaded) {
            key.libraries.emplace_back(tree.libraries.at(*name),
                                       entry != nullptr ? entry->version : 0);
        }
        if (!(cache.key == key)) {
            build(cache, resource, tree, loaded, world.components());
            cache.key = std::move(key);
        }
        const TreeTable& table = cache.table;

        // What the tree keeps that this tree hasn't, as from another tree, goes.
        std::erase_if(tree.state, [&](const auto& entry) {
            const auto found =
                std::ranges::find(source.parameters, entry.first, &TreeParameter::key);
            return found == source.parameters.end() || found->input;
        });
        const bool started = tree.state.empty();
        if (started) {
            cache.is_init.assign(cache.is_init.size(), false);
        }
        TreeParameters parameters(source.parameters, tree.parameters, tree.state);
        const TreeContext context{
            .tree = resource,
            .animation = [&table](const std::string& name) -> const Animation* {
                const auto found = table.animations.find(name);
                return found != table.animations.end() ? found->second.animation : nullptr;
            },
            .track_count = table.tracks.size(),
            .filters = table.filters,
            .seed = hash_id(id)};
        const TreeTick tick = run_tree(context, parameters, tick_seconds, started);

        {
            auto& stored = ecs.get<world::AnimationTree>(entity);
            stored.parameters = tree.parameters;
            stored.state = tree.state;
            stored.invalid = tick.invalid;
            clear_report(stored, false);
        }
        std::vector<std::pair<std::string, json::Value>> events;
        if (tick.invalid.empty()) {
            mix(world, entity, id, tree, cache, tick, events);
        }
        for (const TreeSignal& signal : tick.signals) {
            json::Value data;
            data["animation"] = signal.animation;
            world.emit(signal.kind, id, std::move(data));
        }
        for (auto& [kind, data] : events) {
            // data holds the source entity's ID and the key's data.
            world.emit(kind, data["source"].as_string(), std::move(data["data"]));
        }
    }

    // The entity a track acts on: the tree's own, or the one its targets name; null, with a
    // warning, when there is none.
    entt::entity entity_of(const world::World& world, entt::entity own, std::string_view id,
                           const world::AnimationTree& tree, const std::string& target) {
        if (target.empty()) {
            return own;
        }
        const auto it = tree.targets.find(target);
        if (it == tree.targets.end()) {
            std::vector<std::string_view> names;
            names.reserve(tree.targets.size());
            for (const auto& [name, entity] : tree.targets) {
                names.emplace_back(name);
            }
            std::string message =
                "no entity for target \"" + target + "\" in the AnimationTree's targets";
            if (const std::string_view hint = closest_match(target, names); !hint.empty()) {
                message += "; did you mean \"" + std::string(hint) + "\"?";
            }
            warn_once(id, message);
            return entt::null;
        }
        if (it->second == entt::null || !world.valid(it->second)) {
            warn_once(id, "target \"" + target + "\" names no entity");
            return entt::null;
        }
        return it->second;
    }

    // Mixes the animations a tick plays into the entity's components, as Godot's
    // AnimationMixer::_blend_calc_total_weight, _blend_process and _blend_apply, and reports
    // what it played and the root motion.
    void mix(world::World& world, entt::entity entity, std::string_view id,
             const world::AnimationTree& tree, EntityCache& cache, const TreeTick& tick,
             std::vector<std::pair<std::string, json::Value>>& events) {
        const TreeTable& table = cache.table;
        const std::size_t count = table.tracks.size();
        // Godot's track caches, as _blend_init leaves them.
        struct Mixed {
            float total = 0.0F;
            std::optional<std::vector<double>> value; // a value track's, as numbers
            json::Value direct;                       // or a value that doesn't blend
            bool use_continuous = false;
            bool use_discrete = false;
            scene3d::NodePose pose; // a model node's
        };
        std::vector<Mixed> mixed(count);
        for (std::size_t i = 0; i < count; ++i) {
            const TreeTrack& track = table.tracks[i];
            if (track.kind == TrackKind::Value) {
                mixed[i].value = blendwise(track.init);
                if (!mixed[i].value) {
                    mixed[i].direct = track.init;
                }
            } else if (track.kind == TrackKind::Node) {
                mixed[i].pose = track.rest;
            }
        }
        // Each instance's weight for a track.
        const auto blend_of = [](const AnimationInstance& instance, int track) {
            const auto index = static_cast<std::size_t>(track);
            return index < instance.track_weights.size()
                       ? instance.track_weights[index] * instance.info.weight
                       : instance.info.weight;
        };
        for (const AnimationInstance& instance : tick.instances) {
            const auto found = table.animations.find(instance.name);
            if (found == table.animations.end()) {
                continue;
            }
            for (const int track : found->second.unique) {
                mixed[static_cast<std::size_t>(track)].total += blend_of(instance, track);
            }
        }
        // As _blend_process: the weight a track counts with, normalized when the tree isn't
        // deterministic; null when it doesn't count.
        const auto final_blend = [&](const AnimationInstance& instance,
                                     int track) -> std::optional<float> {
            float blend = blend_of(instance, track);
            if (!tree.deterministic) {
                const float total = mixed[static_cast<std::size_t>(track)].total;
                if (is_zero_approx(total)) {
                    return std::nullopt;
                }
                blend /= total;
            }
            if (is_zero_approx(blend)) {
                return std::nullopt;
            }
            return blend;
        };

        const int root = [&] {
            if (tree.root_motion_track.empty()) {
                return -1;
            }
            for (std::size_t i = 0; i < count; ++i) {
                if (table.tracks[i].kind == TrackKind::Node &&
                    table.tracks[i].name == tree.root_motion_track) {
                    return static_cast<int>(i);
                }
            }
            return -1;
        }();
        Vec3 root_position;
        Quat root_rotation;
        Vec3 root_scale{1.0F, 1.0F, 1.0F};

        std::vector<world::TreeAnimation> playing;
        for (const AnimationInstance& instance : tick.instances) {
            const auto found = table.animations.find(instance.name);
            if (found == table.animations.end()) {
                continue;
            }
            const AnimationTracks& tracks = found->second;
            const Animation& animation = *tracks.animation;
            const PlaybackInfo& info = instance.info;
            const double time = info.time;
            const bool seeked = info.seeked;
            for (std::size_t i = 0; i < animation.tracks.size(); ++i) {
                const Track& track = animation.tracks[i];
                const int index = tracks.tracks[i];
                const std::optional<float> blend = final_blend(instance, index);
                if (!blend) {
                    continue;
                }
                const TreeTrack& cached = table.tracks[static_cast<std::size_t>(index)];
                Mixed& out = mixed[static_cast<std::size_t>(index)];
                if (cached.kind == TrackKind::Event) {
                    const entt::entity on = entity_of(world, entity, id, tree, cached.target);
                    if (on == entt::null) {
                        continue;
                    }
                    std::vector<const Key*> keys;
                    if (seeked) {
                        if (const Key* key =
                                find_key(track, animation.length, time, info.external, true)) {
                            keys.push_back(key);
                        }
                    } else {
                        keys = keys_in_range(track, animation.length, animation.loop, time,
                                             info.delta, info.looped);
                    }
                    for (const Key* key : keys) {
                        json::Value event;
                        event["source"] = std::string(world.id_of(on));
                        event["data"] = key->value;
                        events.emplace_back(track.event, std::move(event));
                    }
                    continue;
                }
                if (!cached.resolved) {
                    warn_once(id, cached.error);
                    continue;
                }
                const Target& target = *cached.resolved;
                const bool discrete = track.update == UpdateMode::Discrete ||
                                      (track.update == UpdateMode::Auto && target.discrete);
                if (!discrete) {
                    out.use_continuous = true;
                    if (!interpolatable(cached.init)) {
                        // Set as it is, as the nearest key has it.
                        if (const Key* key = find_key(track, animation.length, time, true, false)) {
                            out.direct = key->value;
                            out.value.reset();
                        }
                        continue;
                    }
                    const json::Value value = sample(animation, track, time, false);
                    const std::optional<std::vector<double>> numbers = blendwise(value);
                    const std::optional<std::vector<double>> init = blendwise(cached.init);
                    if (!numbers || !init || !out.value || numbers->size() != init->size() ||
                        out.value->size() != init->size()) {
                        continue;
                    }
                    if (cached.angle && numbers->size() == 1) {
                        // Godot's angle blend: never more than half a turn from the rest value.
                        auto rot_a = static_cast<float>((*out.value)[0]);
                        auto rot_b = static_cast<float>((*numbers)[0]);
                        auto rot_init = static_cast<float>((*init)[0]);
                        const auto tau = static_cast<float>(kTau);
                        rot_a = fposmod_float(rot_a, tau);
                        rot_b = fposmod_float(rot_b, tau);
                        rot_init = fposmod_float(rot_init, tau);
                        if (rot_init < kPi) {
                            rot_a =
                                rot_a > rot_init + kPi ? static_cast<float>(rot_a - kTau) : rot_a;
                            rot_b =
                                rot_b > rot_init + kPi ? static_cast<float>(rot_b - kTau) : rot_b;
                        } else {
                            rot_a =
                                rot_a < rot_init - kPi ? static_cast<float>(rot_a + kTau) : rot_a;
                            rot_b =
                                rot_b < rot_init - kPi ? static_cast<float>(rot_b + kTau) : rot_b;
                        }
                        (*out.value)[0] = fposmod_float(rot_a + ((rot_b - rot_init) * *blend), tau);
                        continue;
                    }
                    for (std::size_t j = 0; j < init->size(); ++j) {
                        (*out.value)[j] += ((*numbers)[j] - (*init)[j]) * *blend;
                    }
                    continue;
                }
                // Discrete keys are set as playback passes them.
                std::vector<const Key*> keys;
                if (seeked) {
                    if (const Key* key =
                            find_key(track, animation.length, time, info.external, false)) {
                        keys.push_back(key);
                    }
                } else {
                    keys = keys_in_range(track, animation.length, animation.loop, time, info.delta,
                                         info.looped);
                }
                const entt::entity on = entity_of(world, entity, id, tree, cached.target);
                for (const Key* key : keys) {
                    out.use_discrete = true;
                    if (on == entt::null || !world.valid(on)) {
                        continue;
                    }
                    if (auto applied = apply(world, on, target, key->value); !applied) {
                        warn_once(id, track.path + ": " + applied.error().message);
                    }
                }
            }

            // The model's nodes: how much each counts, and the root motion track's motion.
            world::TreeAnimation report;
            report.animation = instance.name;
            report.time = time;
            float most = 0.0F;
            bool any = false;
            for (const int index : tracks.unique) {
                const float blend = final_blend(instance, index).value_or(0.0F);
                if (!any || blend > most) {
                    most = blend;
                    any = true;
                }
            }
            if (!any) {
                most = info.weight;
                for (const float weight : instance.track_weights) {
                    most = std::max(most, weight * info.weight);
                }
            }
            report.weight = most;
            if (tracks.clip != nullptr) {
                const bool calc_root = !seeked || info.external;
                for (std::size_t i = 0; i < tracks.clip->tracks.size(); ++i) {
                    const int index = tracks.node_tracks[i];
                    if (index < 0) {
                        continue;
                    }
                    const TreeTrack& cached = table.tracks[static_cast<std::size_t>(index)];
                    const float blend = final_blend(instance, index).value_or(0.0F);
                    if (index != root) {
                        if (!near(blend, most)) {
                            report.node_weights.insert_or_assign(cached.name, blend);
                        }
                        continue;
                    }
                    if (is_zero_approx(blend)) {
                        continue;
                    }
                    root_motion(*tracks.clip, i, animation, info, blend, calc_root,
                                tree.root_motion_local, cached.rest,
                                mixed[static_cast<std::size_t>(index)].pose, root_position,
                                root_rotation, root_scale);
                }
            }
            playing.push_back(std::move(report));
        }

        // As _blend_apply: value tracks set, those no animation plays back to their rest values.
        for (std::size_t i = 0; i < count; ++i) {
            const TreeTrack& track = table.tracks[i];
            if (track.kind != TrackKind::Value) {
                continue;
            }
            Mixed& out = mixed[i];
            const bool zero = is_zero_approx(out.total);
            if (!tree.deterministic && zero) {
                continue;
            }
            if (!out.use_continuous && (out.use_discrete || !tree.deterministic)) {
                cache.is_init[i] = true;
            }
            if (cache.is_init[i] && (zero || !out.use_continuous)) {
                continue;
            }
            cache.is_init[i] = !out.use_continuous;
            if (!track.resolved) {
                continue;
            }
            const json::Value value =
                out.value ? from_blendwise(*out.value, track.init, *track.resolved) : out.direct;
            if (value.is_null()) {
                continue;
            }
            const entt::entity on = entity_of(world, entity, id, tree, track.target);
            if (on == entt::null || !world.valid(on)) {
                continue;
            }
            if (auto applied = apply(world, on, *track.resolved, value); !applied) {
                warn_once(id, track.name + ": " + applied.error().message);
            }
        }

        if (!world.valid(entity) || !world.ecs().all_of<world::AnimationTree>(entity)) {
            return;
        }
        auto& stored = world.ecs().get<world::AnimationTree>(entity);
        stored.playing = std::move(playing);
        if (root >= 0) {
            const scene3d::NodePose& pose = mixed[static_cast<std::size_t>(root)].pose;
            stored.root_motion_position = root_position;
            stored.root_motion_rotation = root_rotation;
            stored.root_motion_scale = root_scale - Vec3{1.0F, 1.0F, 1.0F};
            stored.root_motion_position_accumulator = pose.position;
            stored.root_motion_rotation_accumulator = pose.rotation;
            stored.root_motion_scale_accumulator = pose.scale;
        }
    }

    // Godot's root motion for one of an animation's tracks of the root motion node: its motion
    // since the last tick added to the root motion, and its value to the node's pose.
    static void root_motion(const scene3d::ModelAnimation& clip, std::size_t index,
                            const Animation& animation, const PlaybackInfo& info, float blend,
                            bool calc_root, bool local, const scene3d::NodePose& rest,
                            scene3d::NodePose& pose, Vec3& position, Quat& rotation, Vec3& scale) {
        const scene3d::NodeTrack& track = clip.tracks[index];
        const double time = info.time;
        const double start = info.start;
        const double end = info.end;
        const bool backward = std::signbit(info.delta);
        if (calc_root) {
            double prev_time = time - info.delta;
            const auto wrap = [&](double t) {
                switch (animation.loop) {
                case LoopMode::None:
                    return backward ? end : start;
                case LoopMode::Linear:
                    return fposmod(t - start, end - start) + start;
                case LoopMode::PingPong:
                    return pingpong(t - start, end - start) + start;
                }
                return t;
            };
            if (!backward ? is_less_approx(prev_time, start) : is_greater_approx(prev_time, end)) {
                prev_time = wrap(prev_time);
            }
            // For local root motion, the node's rotation at a time.
            const scene3d::NodeTrack* turn = nullptr;
            if (local && track.property == scene3d::NodeProperty::Position) {
                for (const scene3d::NodeTrack& other : clip.tracks) {
                    if (other.node == track.node &&
                        other.property == scene3d::NodeProperty::Rotation) {
                        turn = &other;
                        break;
                    }
                }
            }
            const auto add = [&](double from, double to) {
                const std::array<float, 4> a = sample_node(track, clip, from);
                const std::array<float, 4> b = sample_node(track, clip, to);
                switch (track.property) {
                case scene3d::NodeProperty::Position: {
                    Vec3 moved = vec_of(b) - vec_of(a);
                    if (turn != nullptr) {
                        moved = scene3d::xform_inv(quat_of(sample_node(*turn, clip, to)), moved);
                    }
                    position = position + (moved * blend);
                    break;
                }
                case scene3d::NodeProperty::Rotation:
                    rotation = scene3d::normalized(scene3d::multiply(
                        rotation,
                        scene3d::slerp(Quat{},
                                       scene3d::multiply(scene3d::inverse(quat_of(a)), quat_of(b)),
                                       blend)));
                    break;
                case scene3d::NodeProperty::Scale:
                    scale = scale + ((vec_of(b) - vec_of(a)) * blend);
                    break;
                }
            };
            // Round a loop's end first.
            if (!backward ? is_greater_approx(prev_time, time) : is_less_approx(prev_time, time)) {
                add(prev_time, !backward ? end : start);
                prev_time = !backward ? start : end;
            }
            add(prev_time, time);
        }
        const std::array<float, 4> value = sample_node(track, clip, time);
        switch (track.property) {
        case scene3d::NodeProperty::Position:
            pose.position = pose.position + ((vec_of(value) - rest.position) * blend);
            break;
        case scene3d::NodeProperty::Rotation:
            pose.rotation = scene3d::normalized(scene3d::multiply(
                pose.rotation,
                scene3d::slerp(Quat{},
                               scene3d::multiply(scene3d::inverse(rest.rotation), quat_of(value)),
                               blend)));
            break;
        case scene3d::NodeProperty::Scale:
            pose.scale = pose.scale + ((vec_of(value) - rest.scale) * blend);
            break;
        }
    }

    std::optional<std::vector<scene3d::ClipSample>> clips(const world::World& world,
                                                          entt::entity entity) {
        const auto* tree = world.ecs().try_get<const world::AnimationTree>(entity);
        if (tree == nullptr || !tree->active || tree->tree_root.empty()) {
            return std::nullopt;
        }
        std::vector<scene3d::ClipSample> out;
        const std::string id(world.id_of(entity));
        for (const world::TreeAnimation& played : tree->playing) {
            // "library/animation", or an animation of the library named "".
            const Animation* animation = nullptr;
            const scene3d::Model* model = nullptr;
            const auto find = [&](const std::string& library, const std::string& name) {
                const auto path = tree->libraries.find(library);
                if (path == tree->libraries.end()) {
                    return false;
                }
                LibraryCache::Entry* entry = libraries.get(
                    path->second, [&](const std::string& message) { warn_once(id, message); });
                if (entry == nullptr) {
                    return false;
                }
                const auto found = entry->library.animations.find(name);
                if (found == entry->library.animations.end()) {
                    return false;
                }
                animation = &found->second;
                model = clip_model(*entry, path->second, found->second);
                return true;
            };
            const std::size_t slash = played.animation.find('/');
            if (slash == std::string::npos ||
                !find(played.animation.substr(0, slash), played.animation.substr(slash + 1))) {
                find("", played.animation);
            }
            if (animation == nullptr || model == nullptr) {
                continue;
            }
            scene3d::ClipSample clip{.model = animation->from.model,
                                     .animation = animation->from.animation,
                                     .time = played.time,
                                     .weight = played.weight,
                                     .node_weights = played.node_weights};
            if (!tree->root_motion_track.empty()) {
                clip.node_weights.insert_or_assign(tree->root_motion_track, 0.0F);
            }
            out.push_back(std::move(clip));
        }
        return out;
    }
};

TreePlayer::TreePlayer(asset::Assets& assets) : state_(std::make_unique<State>(assets)) {}

TreePlayer::~TreePlayer() = default;

void TreePlayer::run(world::World& world, double tick_seconds) {
    state_->run(world, tick_seconds);
}

std::optional<std::vector<scene3d::ClipSample>> TreePlayer::clips(const world::World& world,
                                                                  entt::entity entity) {
    return state_->clips(world, entity);
}

} // namespace slash::animation::detail
