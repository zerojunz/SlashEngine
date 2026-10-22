#include "slash/canvas/canvas.hpp"

#include "slash/canvas/material.hpp"
#include "slash/canvas/particles.hpp"
#include "slash/canvas/sprite_frames.hpp"
#include "slash/canvas/tilemap.hpp"
#include "slash/core/json.hpp"
#include "slash/core/log.hpp"
#include "slash/core/text.hpp"
#include "slash/physics2d/physics2d.hpp"
#include "slash/render/image.hpp"
#include "slash/ui/ui.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace slash::canvas {
namespace {

// Plays AnimatedSprite2D components. Its state is only a cache of loaded .frames.json files and
// the warnings already logged, so a world's snapshot, undo or dry run is never out of step with
// it.
struct AnimationState {
    asset::Assets* assets = nullptr;
    struct Cached {
        asset::Handle<SpriteFrames> frames;
        bool used = false;
    };
    std::map<std::string, Cached, std::less<>> cache;
    std::set<std::string, std::less<>> warned;

    void warn_once(std::string_view id, const std::string& message) {
        if (warned.insert(std::string(id) + "\n" + message).second) {
            log::warn("an AnimatedSprite2D can't play", {{"id", id}, {"error", message}});
        }
    }

    const SpriteFrames* frames(const std::string& path, std::string_view id) {
        if (auto it = cache.find(path); it != cache.end()) {
            it->second.used = true;
            return it->second.frames.get();
        }
        auto loaded = assets->load<SpriteFrames>(path);
        if (!loaded) {
            warn_once(id, path + ": " + loaded.error().message);
            return nullptr;
        }
        auto& cached = cache[path];
        cached.frames = std::move(loaded).value();
        cached.used = true;
        return cached.frames.get();
    }

    void run(world::World& world, double tick_seconds) {
        for (auto& [path, cached] : cache) {
            cached.used = false;
        }
        auto& ecs = world.ecs();
        for (const entt::entity entity : ecs.view<world::AnimatedSprite2D>()) {
            auto& player = ecs.get<world::AnimatedSprite2D>(entity);
            auto& sprite = ecs.get_or_emplace<world::Sprite2D>(entity);
            const std::string_view id = world.id_of(entity);
            if (player.sprite_frames.empty()) {
                continue;
            }
            const SpriteFrames* frames = this->frames(player.sprite_frames, id);
            if (frames == nullptr) {
                continue;
            }
            const auto found = frames->animations.find(player.animation);
            if (found == frames->animations.end()) {
                std::vector<std::string_view> names;
                names.reserve(frames->animations.size());
                for (const auto& [name, animation] : frames->animations) {
                    names.emplace_back(name);
                }
                std::string message =
                    player.sprite_frames + " has no animation \"" + player.animation + "\"";
                if (const std::string_view hint = closest_match(player.animation, names);
                    !hint.empty()) {
                    message += "; did you mean \"" + std::string(hint) + "\"?";
                }
                warn_once(id, message);
                continue;
            }
            advance(world, id, player, found->second, tick_seconds);
            const SpriteFrame& frame =
                found->second.frames.at(static_cast<std::size_t>(player.frame));
            sprite.texture = frame.texture;
            sprite.hframes = frame.hframes;
            sprite.vframes = frame.vframes;
            sprite.frame = frame.frame;
            sprite.region = {};
        }
        std::erase_if(cache, [](const auto& entry) { return !entry.second.used; });
    }

    // Moves the animation on by one tick, as Godot does: frames change each 1 / (fps *
    // speed_scale) seconds, and an animation that doesn't loop stops on its last frame.
    static void advance(world::World& world, std::string_view id, world::AnimatedSprite2D& player,
                        const Animation& animation, double tick_seconds) {
        const auto count = static_cast<std::int64_t>(animation.frames.size());
        player.frame = static_cast<int>(std::clamp<std::int64_t>(player.frame, 0, count - 1));
        player.frame_progress = std::clamp(player.frame_progress, 0.0F, 1.0F);
        const double rate = animation.fps * player.speed_scale;
        if (!player.playing || rate == 0.0 || !std::isfinite(rate)) {
            return;
        }
        double progress = player.frame_progress + (tick_seconds * rate);
        if (progress >= 0.0 && progress < 1.0) {
            player.frame_progress = static_cast<float>(progress);
            return;
        }
        const double steps = std::floor(progress);
        progress -= steps;
        const double next = static_cast<double>(player.frame) + steps;
        bool finished = false;
        if (animation.loop) {
            const double wrapped =
                std::fmod(std::fmod(next, static_cast<double>(count)) + static_cast<double>(count),
                          static_cast<double>(count));
            player.frame = static_cast<int>(wrapped);
        } else if (next >= static_cast<double>(count)) {
            player.frame = static_cast<int>(count - 1);
            progress = 1.0;
            finished = true;
        } else if (next < 0.0) {
            player.frame = 0;
            progress = 0.0;
            finished = true;
        } else {
            player.frame = static_cast<int>(next);
        }
        player.frame_progress = static_cast<float>(progress);
        if (finished) {
            player.playing = false;
            json::Value data;
            data["animation"] = player.animation;
            world.emit("animation_finished", id, std::move(data));
        }
    }
};

} // namespace

asset::FileSource with_texture_forms(const asset::FileSource& files,
                                     std::function<bool(render::TextureFormat)> samples) {
    using render::TextureFormat;
    // The form to read in place of path, or "" to read path itself.
    auto form_of = [files, samples = std::move(samples)](std::string_view path) -> std::string {
        if (!render::is_image_path(path) || files.stamp(path)) {
            return {};
        }
        constexpr std::array<TextureFormat, 2> kCompressed{TextureFormat::Bc7,
                                                           TextureFormat::Astc4x4};
        std::vector<TextureFormat> order;
        for (const TextureFormat format : kCompressed) {
            if (samples(format)) {
                order.push_back(format);
            }
        }
        order.push_back(TextureFormat::Rgba8);
        order.insert(order.end(), kCompressed.begin(), kCompressed.end());
        for (const TextureFormat format : order) {
            std::string form = render::texture_form_path(path, format);
            if (files.stamp(form)) {
                return form;
            }
        }
        return {};
    };
    return {
        .read = [files, form_of](std::string_view path) -> Result<std::string> {
            const std::string form = form_of(path);
            return files.read(form.empty() ? path : form);
        },
        .stamp = [files, form_of](std::string_view path) -> std::optional<asset::FileStamp> {
            const std::string form = form_of(path);
            return files.stamp(form.empty() ? path : form);
        },
    };
}

void add_asset_types(asset::Assets& assets) {
    scene3d::add_asset_types(assets);
    // A texture's decoded texels; renderers keep a copy on the GPU too (perf.stats' gpu).
    assets.add_type<render::Image>(
        "texture", {render::kImageSuffixes.begin(), render::kImageSuffixes.end()},
        [](std::string_view bytes, std::string_view /*path*/) {
            return render::decode_image(bytes);
        },
        [](const render::Image& image) {
            return sizeof(render::Image) + image.pixels.capacity() + image.blocks.capacity();
        });
    assets.add_type<SpriteFrames>(
        "sprite_frames", {std::string(kSpriteFramesSuffix)},
        [](std::string_view text, std::string_view /*path*/) -> Result<SpriteFrames> {
            auto doc = json::parse(text);
            if (!doc) {
                return doc.error();
            }
            return parse_sprite_frames(doc.value());
        });
    assets.add_type<CanvasMaterial>(
        "material", {std::string(kMaterialSuffix)},
        [](std::string_view text, std::string_view /*path*/) -> Result<CanvasMaterial> {
            auto doc = json::parse(text);
            if (!doc) {
                return doc.error();
            }
            return parse_material(doc.value());
        });
    // A game's shaders, as text: renderers compile them for the materials that name them.
    assets.add_type<std::string>("shader", {std::string(kShaderSuffix)},
                                 [](std::string_view text, std::string_view /*path*/) {
                                     return Result<std::string>(std::string(text));
                                 });
    assets.add_type<render::ParticleProcess>(
        "process_material", {std::string(kProcessMaterialSuffix)},
        [](std::string_view text, std::string_view /*path*/) -> Result<render::ParticleProcess> {
            auto doc = json::parse(text);
            if (!doc) {
                return doc.error();
            }
            return parse_process_material(doc.value());
        });
    assets.add_type<TileSet>(
        "tile_set", {std::string(kTileSetSuffix)},
        [](std::string_view text, std::string_view /*path*/) -> Result<TileSet> {
            auto doc = json::parse(text);
            if (!doc) {
                return doc.error();
            }
            return parse_tileset(doc.value());
        });
}

// Gives 2D physics each TileMapLayer's shapes, made again only when the layer's cells or its
// tile set change.
struct TileShapeCache {
    struct Entry {
        std::string tile_set;
        Vec2i origin;
        std::map<std::string, std::string> legend;
        std::vector<std::string> rows;
        std::uint64_t version = 0;
        physics2d::TileShapes shapes;
    };
    asset::Assets* assets = nullptr;
    std::map<std::string, asset::Handle<TileSet>, std::less<>> sets;
    std::unordered_map<entt::entity, Entry> entries;

    physics2d::TileShapes shapes(const world::World& world, entt::entity entity) {
        const auto* layer = world.ecs().try_get<world::TileMapLayer>(entity);
        if (layer == nullptr || layer->tile_set.empty() || layer->rows.empty()) {
            return nullptr;
        }
        auto found = sets.find(layer->tile_set);
        if (found == sets.end()) {
            auto loaded = assets->load<TileSet>(layer->tile_set);
            if (!loaded) {
                return nullptr; // the renderer, validation and exports report it
            }
            found = sets.emplace(layer->tile_set, std::move(loaded).value()).first;
        }
        const asset::Handle<TileSet>& set = found->second;
        Entry& entry = entries[entity];
        if (entry.shapes != nullptr && entry.version == set.version() &&
            entry.tile_set == layer->tile_set && entry.origin == layer->origin &&
            entry.legend == layer->legend && entry.rows == layer->rows) {
            return entry.shapes;
        }
        auto cells = decode_cells(*layer, *set);
        entry = {.tile_set = layer->tile_set,
                 .origin = layer->origin,
                 .legend = layer->legend,
                 .rows = layer->rows,
                 .version = set.version(),
                 .shapes = cells ? std::make_shared<const std::vector<physics2d::TileShape>>(
                                       tile_shapes(*set, cells.value()))
                                 : std::make_shared<const std::vector<physics2d::TileShape>>()};
        // Entries of layers that are gone.
        const std::size_t layers = world.ecs().view<const world::TileMapLayer>().size();
        if (entries.size() > (2 * layers) + 8) {
            std::erase_if(entries, [&](const auto& pair) {
                return !world.valid(pair.first) ||
                       !world.ecs().all_of<world::TileMapLayer>(pair.first);
            });
        }
        return entries[entity].shapes;
    }
};

void add_systems(world::World& world, asset::Assets& assets) {
    auto tiles = std::make_shared<TileShapeCache>();
    tiles->assets = &assets;
    physics2d::set_tile_shape_source(world, [tiles](const world::World& w, entt::entity entity) {
        return tiles->shapes(w, entity);
    });
    auto state = std::make_shared<AnimationState>();
    state->assets = &assets;
    world.add_system(
        "animated_sprite_2d",
        [state](world::World& w, double tick_seconds) { state->run(w, tick_seconds); },
        {.doc = "Plays each AnimatedSprite2D's animation on its Sprite2D.", .priority = 1000});
    world::add_camera_system(world, [](const world::World& w) { return ui::settings(w).viewport; });
    world::add_parallax_system(world,
                               [](const world::World& w) { return ui::settings(w).viewport; });
    add_particles_system(world);
    add_gpu_particles_system(world, &assets);
}

void install(world::World& world, asset::Assets& assets) {
    add_asset_types(assets);
    add_systems(world, assets);
    ui::install(world, assets, ui::settings(world));
}

} // namespace slash::canvas
