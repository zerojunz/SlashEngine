#include "slash/canvas/canvas.hpp"

#include "slash/canvas/material.hpp"
#include "slash/canvas/particles.hpp"
#include "slash/canvas/sprite_frames.hpp"
#include "slash/canvas/tilemap.hpp"
#include "slash/core/json.hpp"
#include "slash/core/log.hpp"
#include "slash/core/text.hpp"
#include "slash/physics2d/physics2d.hpp"
#include "slash/procedural/foliage.hpp"
#include "slash/procedural/texture.hpp"
#include "slash/render/image.hpp"
#include "slash/scene3d/collision.hpp"
#include "slash/scene3d/csg.hpp"
#include "slash/scene3d/flesh.hpp"
#include "slash/scene3d/foliage.hpp"
#include "slash/scene3d/gridmap.hpp"
#include "slash/scene3d/particles.hpp"
#include "slash/scene3d/streaming.hpp"
#include "slash/scene3d/terrain.hpp"
#include "slash/ui/ui.hpp"
#include "slash/video/video.hpp"
#include "slash/world/character.hpp"
#include "slash/world/flesh.hpp"
#include "slash/world/hair.hpp"
#include "slash/world/path.hpp"
#include "slash/world/tree2d.hpp"
#include "slash/world/vfx.hpp"
#include "slash/world/wind.hpp"

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
#include <utility>
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
    // A procedural texture (.texture.json), made into pixels as it loads.
    assets.add_type<render::Image>(
        "procedural_texture", {std::string(procedural::kTextureSuffix)},
        [](std::string_view text, std::string_view path) {
            return procedural::decode_texture(path, text);
        },
        [](const render::Image& image) { return sizeof(render::Image) + image.pixels.capacity(); });
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
    assets.add_type<ProcessMaterial>(
        "process_material", {std::string(kProcessMaterialSuffix)},
        [](std::string_view text, std::string_view /*path*/) -> Result<ProcessMaterial> {
            auto doc = json::parse(text);
            if (!doc) {
                return doc.error();
            }
            return parse_process_material_both(doc.value());
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

// Gives 2D physics each Foliage2D's blocking instances as polygons, one a "cell" of [group,
// instance], made again only when its instances or types change.
struct FoliageShapeCache {
    struct Entry {
        world::Foliage2D foliage;
        std::vector<std::shared_ptr<const world::FoliageType>> types;
        physics2d::TileShapes shapes;
    };
    std::unordered_map<entt::entity, Entry> entries;

    // A type's collision as a polygon in an instance's space, before its offset: a circle and
    // a capsule's ends of twelve sides a turn.
    static std::vector<Vec2> outline(const world::FoliageCollision& collision) {
        constexpr int kSides = 12;
        constexpr float kTurn = 6.28318530718F;
        std::vector<Vec2> points;
        if (collision.shape == "rectangle") {
            const float x = collision.size.x * 0.5F;
            const float y = collision.size.y * 0.5F;
            points = {{-x, -y}, {x, -y}, {x, y}, {-x, y}};
        } else if (collision.shape == "circle") {
            for (int i = 0; i < kSides; ++i) {
                const Vec2f at = cos_sin(kTurn * static_cast<float>(i) / kSides);
                points.push_back({at.x * collision.radius, at.y * collision.radius});
            }
        } else if (collision.shape == "capsule") {
            const float half = std::max((collision.height * 0.5F) - collision.radius, 0.0F);
            for (int i = 0; i <= kSides / 2; ++i) {
                const Vec2f at = cos_sin(kTurn * static_cast<float>(i) / kSides);
                points.push_back({at.x * collision.radius, half + (at.y * collision.radius)});
            }
            for (int i = kSides / 2; i <= kSides; ++i) {
                const Vec2f at = cos_sin(kTurn * static_cast<float>(i) / kSides);
                points.push_back({at.x * collision.radius, -half + (at.y * collision.radius)});
            }
        }
        return points;
    }

    static physics2d::TileShapes
    make(const world::Foliage2D& foliage,
         const std::vector<std::shared_ptr<const world::FoliageType>>& types) {
        auto shapes = std::make_shared<std::vector<physics2d::TileShape>>();
        for (std::size_t g = 0; g < foliage.groups.size(); ++g) {
            if (types[g] == nullptr || types[g]->space != "2d") {
                continue;
            }
            const std::vector<Vec2> base = outline(types[g]->collision);
            if (base.empty()) {
                continue;
            }
            const Vec3 offset = types[g]->collision.offset;
            const std::size_t count = procedural::foliage_count(foliage.groups[g]);
            for (std::size_t i = 0; i < count; ++i) {
                const procedural::FoliagePlace2D place =
                    procedural::foliage_place(foliage.groups[g], i);
                const Vec2f spin = cos_sin(place.rotation);
                const float scale = std::abs(place.scale);
                const float mirror = place.scale < 0.0F ? -1.0F : 1.0F;
                physics2d::TileShape shape;
                shape.cell = {static_cast<int>(g), static_cast<int>(i)};
                shape.collision_layer = foliage.collision_layer;
                shape.collision_mask = foliage.collision_mask;
                for (const Vec2 point : base) {
                    const float x = (point.x + offset.x) * scale * mirror;
                    const float y = (point.y + offset.y) * scale;
                    shape.points.push_back({place.position.x + (x * spin.x) - (y * spin.y),
                                            place.position.y + (x * spin.y) + (y * spin.x)});
                }
                shapes->push_back(std::move(shape));
            }
        }
        return shapes;
    }

    physics2d::TileShapes shapes(const world::World& world, entt::entity entity) {
        const auto* foliage = world.ecs().try_get<world::Foliage2D>(entity);
        if (foliage == nullptr) {
            return nullptr;
        }
        std::vector<std::shared_ptr<const world::FoliageType>> types;
        types.reserve(foliage->groups.size());
        for (const world::FoliageGroup2D& group : foliage->groups) {
            types.push_back(procedural::foliage_type(world, group.type));
        }
        Entry& entry = entries[entity];
        if (entry.shapes == nullptr || entry.foliage != *foliage || entry.types != types) {
            entry = {.foliage = *foliage, .types = types, .shapes = make(*foliage, types)};
        }
        const std::size_t kept = world.ecs().view<const world::Foliage2D>().size();
        if (entries.size() > (2 * kept) + 8) {
            std::erase_if(entries, [&](const auto& pair) {
                return !world.valid(pair.first) ||
                       !world.ecs().all_of<world::Foliage2D>(pair.first);
            });
        }
        return entries[entity].shapes;
    }
};

// An asset as a pointer that also keeps its handle, so the asset stays loaded, and loading it
// again gives the same pointer, for as long as anything holds it.
template <typename T>
std::shared_ptr<const T> held(const asset::Handle<T>& handle) {
    auto holder = std::make_shared<std::pair<asset::Handle<T>, std::shared_ptr<const T>>>(
        handle, handle.share());
    return {holder, holder->second.get()};
}

void add_systems(world::World& world, asset::Assets& assets) {
    auto tiles = std::make_shared<TileShapeCache>();
    tiles->assets = &assets;
    auto foliage = std::make_shared<FoliageShapeCache>();
    physics2d::set_tile_shape_source(world,
                                     [tiles, foliage](const world::World& w, entt::entity entity) {
                                         if (!w.ecs().all_of<world::TileMapLayer>(entity)) {
                                             return foliage->shapes(w, entity);
                                         }
                                         return tiles->shapes(w, entity);
                                     });
    // Blasts carve tile map layers that are Destructible2Ds: the cells whose centers they reach.
    physics2d::set_tile_carver(world, [tiles](world::World& w, entt::entity entity, Vec2 center,
                                              float radius, bool apply) {
        auto* layer = w.ecs().try_get<world::TileMapLayer>(entity);
        if (layer == nullptr || layer->tile_set.empty() || radius <= 0.0F) {
            return 0;
        }
        auto set = tiles->assets->load<TileSet>(layer->tile_set);
        if (!set) {
            return 0;
        }
        auto cells = decode_cells(*layer, *set.value());
        if (!cells) {
            return 0;
        }
        const world::Affine2D global = world::affine(world::Tree2D(w).global(entity));
        const real_t reach = static_cast<real_t>(radius) * radius;
        int taken = 0;
        for (auto it = cells.value().begin(); it != cells.value().end();) {
            const Vec2 d = global.apply(map_to_local(*set.value(), it->first)) - center;
            if ((d.x * d.x) + (d.y * d.y) <= reach) {
                it = cells.value().erase(it);
                ++taken;
            } else {
                ++it;
            }
        }
        if (taken > 0 && apply) {
            encode_cells(cells.value(), *set.value(), *layer);
        }
        return taken;
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
    world::add_path_systems(world);
    add_particles_system(world);
    add_gpu_particles_system(world, &assets);
    scene3d::add_particles_system(world);
    scene3d::add_streaming_system(world);
    world::add_wind_system(world);
    world::add_vfx_system(world);
    world::add_character_system(world);
    world::add_flesh_2d_system(world);
    procedural::install_foliage(world);
    // Grooms' length, density and color maps read through assets.
    world::install_hair(world);
    world::set_hair_image_reader(
        world, [&assets](const std::string& path) -> Result<world::HairImage> {
            auto found = assets.load<render::Image>(path);
            if (!found) {
                return found.error();
            }
            const std::shared_ptr<const render::Image> image = held(found.value());
            world::HairImage out;
            out.pixels = std::shared_ptr<const std::vector<std::uint8_t>>(image, &image->pixels);
            out.width = image->width;
            out.height = image->height;
            out.version = found.value().version();
            return out;
        });
    // Vector field attractors' textures read through assets.
    const scene3d::ImageLoader images =
        [&assets](const std::string& path) -> Result<std::shared_ptr<const render::Image>> {
        auto found = assets.load<render::Image>(path);
        if (!found) {
            return found.error();
        }
        return held(found.value());
    };
    // CSG roots and GridMaps keep their meshes, and models their collision shapes, until they
    // change, models and mesh libraries read through assets.
    const scene3d::ModelLoader models =
        [&assets](const std::string& path) -> Result<std::shared_ptr<const scene3d::Model>> {
        auto found = assets.load<scene3d::Model>(path);
        if (!found) {
            return found.error();
        }
        return held(found.value());
    };
    // SDF colliders' textures too, and height fields draw models.
    const scene3d::SdfLoader sdfs =
        [&assets](const std::string& path) -> Result<std::shared_ptr<const render::Gray16>> {
        auto found = assets.load<render::Gray16>(path);
        if (!found) {
            return found.error();
        }
        return held(found.value());
    };
    scene3d::add_gpu_particles_system(world, images, sdfs, models);
    scene3d::install_csg(world, models);
    scene3d::install_flesh(world, models);
    scene3d::install_model_collision(world, models);
    scene3d::install_foliage_collision(world);
    scene3d::install_gridmap(
        world, models,
        [&assets](const std::string& path) -> Result<std::shared_ptr<const scene3d::MeshLibrary>> {
            auto found = assets.load<scene3d::MeshLibrary>(path);
            if (!found) {
                return found.error();
            }
            return held(found.value());
        });
    // Terrains keep their data until a map changes, the maps read through assets.
    scene3d::install_terrain(
        world,
        [&assets](const std::string& path) -> Result<std::shared_ptr<const render::Gray16>> {
            auto found = assets.load<render::Gray16>(path);
            if (!found) {
                return found.error();
            }
            return held(found.value());
        },
        images);
}

void install(world::World& world, asset::Assets& assets) {
    // Gameplay code makes textures and meshes into these (asset::Assets::put).
    asset::attach(world.ecs(), assets);
    add_asset_types(assets);
    add_systems(world, assets);
    video::install(world, assets);
    ui::install(world, assets, ui::settings(world));
}

} // namespace slash::canvas
