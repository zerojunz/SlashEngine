#include "slash/canvas/canvas.hpp"
#include "slash/canvas/particles.hpp"
#include "slash/core/log.hpp"
#include "slash/ui/ui.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace slash::canvas {
namespace {

std::uint8_t to_byte(float channel) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(channel, 0.0F, 1.0F) * 255.0F));
}

std::string number(float value) {
    std::string text = std::to_string(value);
    text.erase(text.find_last_not_of('0') + 1);
    if (text.ends_with('.')) {
        text.pop_back();
    }
    return text;
}

render::Filter resolve_filter(std::string_view filter, render::Filter fallback) {
    if (filter == "nearest") {
        return render::Filter::Nearest;
    }
    if (filter == "linear") {
        return render::Filter::Linear;
    }
    return fallback;
}

} // namespace

Result<std::array<render::Vertex2D, 6>> sprite_quad(const world::Sprite2D& sprite,
                                                    const world::Transform2D& transform,
                                                    std::uint32_t texture_width,
                                                    std::uint32_t texture_height) {
    if (texture_width == 0 || texture_height == 0) {
        return make_error(ErrorCode::InvalidArgument, "the texture is empty");
    }
    const auto width = static_cast<float>(texture_width);
    const auto height = static_cast<float>(texture_height);
    // The part of the texture to split into frames.
    float rx = 0.0F;
    float ry = 0.0F;
    float rw = width;
    float rh = height;
    if (sprite.region.z > 0.0F && sprite.region.w > 0.0F) {
        rx = sprite.region.x;
        ry = sprite.region.y;
        rw = sprite.region.z;
        rh = sprite.region.w;
        if (rx < 0.0F || ry < 0.0F || rx + rw > width || ry + rh > height) {
            return make_error(ErrorCode::InvalidArgument,
                              "region [" + number(rx) + ", " + number(ry) + ", " + number(rw) +
                                  ", " + number(rh) + "] goes outside the " +
                                  std::to_string(texture_width) + "x" +
                                  std::to_string(texture_height) + " texture");
        }
    }
    if (sprite.hframes < 1 || sprite.vframes < 1) {
        return make_error(ErrorCode::InvalidArgument, "hframes and vframes must be at least 1");
    }
    const int count = sprite.hframes * sprite.vframes;
    if (sprite.frame < 0 || sprite.frame >= count) {
        return make_error(ErrorCode::InvalidArgument,
                          "frame " + std::to_string(sprite.frame) + " is outside the sheet's " +
                              std::to_string(count) + " frames, numbered from 0");
    }
    const float cell_w = rw / static_cast<float>(sprite.hframes);
    const float cell_h = rh / static_cast<float>(sprite.vframes);
    const int column = sprite.frame % sprite.hframes;
    const int row = sprite.frame / sprite.hframes;
    const float cell_x = rx + (static_cast<float>(column) * cell_w);
    const float cell_y = ry + (static_cast<float>(row) * cell_h);

    float u0 = cell_x / width;
    float u1 = (cell_x + cell_w) / width;
    float v0 = cell_y / height;
    float v1 = (cell_y + cell_h) / height;
    // As in Godot, flipping mirrors the texture in place; the rectangle stays where it is.
    if (sprite.flip_h) {
        std::swap(u0, u1);
    }
    if (sprite.flip_v) {
        std::swap(v0, v1);
    }

    const float x0 = sprite.offset.x - (sprite.centered ? cell_w / 2.0F : 0.0F);
    const float y0 = sprite.offset.y - (sprite.centered ? cell_h / 2.0F : 0.0F);
    const float x1 = x0 + cell_w;
    const float y1 = y0 + cell_h;
    const float cos_r = std::cos(transform.rotation);
    const float sin_r = std::sin(transform.rotation);
    const std::array<std::uint8_t, 4> rgba{to_byte(sprite.modulate.x), to_byte(sprite.modulate.y),
                                           to_byte(sprite.modulate.z), to_byte(sprite.modulate.w)};
    const auto corner = [&](float x, float y, float u, float v) {
        const float sx = x * transform.scale.x;
        const float sy = y * transform.scale.y;
        return render::Vertex2D{.x = transform.position.x + (sx * cos_r) - (sy * sin_r),
                                .y = transform.position.y + (sx * sin_r) + (sy * cos_r),
                                .u = u,
                                .v = v,
                                .rgba = rgba};
    };
    const render::Vertex2D top_left = corner(x0, y0, u0, v0);
    const render::Vertex2D top_right = corner(x1, y0, u1, v0);
    const render::Vertex2D bottom_left = corner(x0, y1, u0, v1);
    const render::Vertex2D bottom_right = corner(x1, y1, u1, v1);
    return std::array<render::Vertex2D, 6>{top_left,  top_right,    bottom_left,
                                           top_right, bottom_right, bottom_left};
}

world::Transform2D interpolate(const world::Transform2D& from, const world::Transform2D& to,
                               double alpha) {
    const auto t = static_cast<float>(std::clamp(alpha, 0.0, 1.0));
    const auto lerp = [t](float a, float b) { return a + ((b - a) * t); };
    constexpr float kTwoPi = 2.0F * std::numbers::pi_v<float>;
    // The turn from one rotation to the other, between -pi and pi.
    float turn = std::fmod(to.rotation - from.rotation, kTwoPi);
    if (turn > std::numbers::pi_v<float>) {
        turn -= kTwoPi;
    } else if (turn < -std::numbers::pi_v<float>) {
        turn += kTwoPi;
    }
    world::Transform2D out;
    out.position = {lerp(from.position.x, to.position.x), lerp(from.position.y, to.position.y)};
    out.rotation = t >= 1.0F ? to.rotation : from.rotation + (turn * t);
    out.scale = {lerp(from.scale.x, to.scale.x), lerp(from.scale.y, to.scale.y)};
    return out;
}

double material_time(std::uint64_t tick, double tick_seconds, double interpolation) {
    return std::max(static_cast<double>(tick) - 1.0 + std::clamp(interpolation, 0.0, 1.0), 0.0) *
           tick_seconds;
}

CanvasRenderer::CanvasRenderer(render::RenderDevice& device, asset::Assets& assets)
    : device_(&device), assets_(&assets) {}

CanvasRenderer::~CanvasRenderer() {
    for (const auto& [key, texture] : textures_) {
        device_->destroy_texture(texture.id);
    }
    for (const auto& [texture, version] : pages_) {
        device_->destroy_texture(texture);
    }
    for (const render::TextureId texture : {atlas_, shadow_map_}) {
        if (texture != render::TextureId::None) {
            device_->destroy_texture(texture);
        }
    }
}

void CanvasRenderer::before_tick(const world::World& world) {
    previous_.clear();
    const auto& ecs = world.ecs();
    for (const auto [entity, transform, sprite] :
         ecs.view<const world::Transform2D, const world::Sprite2D>().each()) {
        previous_.emplace(entity, transform);
    }
    for (const auto [entity, transform, layer] :
         ecs.view<const world::Transform2D, const world::TileMapLayer>().each()) {
        previous_.emplace(entity, transform);
    }
    for (const auto [entity, transform, light] :
         ecs.view<const world::Transform2D, const world::PointLight2D>().each()) {
        previous_.emplace(entity, transform);
    }
    for (const auto [entity, transform, light] :
         ecs.view<const world::Transform2D, const world::DirectionalLight2D>().each()) {
        previous_.emplace(entity, transform);
    }
    for (const auto [entity, transform, occluder] :
         ecs.view<const world::Transform2D, const world::LightOccluder2D>().each()) {
        previous_.emplace(entity, transform);
    }
    for (const auto [entity, transform, emitter] :
         ecs.view<const world::Transform2D, const world::CPUParticles2D>().each()) {
        previous_.emplace(entity, transform);
    }
    for (const auto [entity, transform, shape] :
         ecs.view<const world::Transform2D, const world::CollisionShape2D>().each()) {
        previous_.emplace(entity, transform);
    }
    for (const auto [entity, transform, polygon] :
         ecs.view<const world::Transform2D, const world::CollisionPolygon2D>().each()) {
        previous_.emplace(entity, transform);
    }
}

void CanvasRenderer::warn_once(const std::string& key, std::string_view message,
                               std::string_view what, std::string_view event) {
    if (warned_.insert(key + "\n" + std::string(message)).second) {
        log::warn(event, {{what, key}, {"error", message}});
    }
}

Result<const TileSet*> CanvasRenderer::tile_set(const std::string& path) {
    auto it = tile_sets_.find(path);
    if (it == tile_sets_.end()) {
        auto loaded = assets_->load<TileSet>(path);
        if (!loaded) {
            return loaded.error();
        }
        it = tile_sets_.emplace(path, std::move(loaded).value()).first;
    }
    return it->second.get();
}

const CanvasRenderer::LayerCells& CanvasRenderer::cells(entt::entity entity,
                                                        const world::TileMapLayer& layer,
                                                        const TileSet& set,
                                                        std::uint64_t set_version) {
    auto [it, added] = layer_cells_.try_emplace(entity);
    LayerCells& cached = it->second;
    cached.seen = true;
    if (added || cached.set_version != set_version || cached.layer.tile_set != layer.tile_set ||
        !(cached.layer.origin == layer.origin) || cached.layer.legend != layer.legend ||
        cached.layer.rows != layer.rows) {
        cached.set_version = set_version;
        cached.layer = layer;
        auto decoded = decode_cells(layer, set);
        cached.error = decoded ? std::string() : decoded.error().message;
        cached.cells = decoded ? std::move(decoded).value() : Cells{};
    }
    return cached;
}

Result<const render::Image*> CanvasRenderer::light_image(const std::string& path) {
    auto it = light_images_.find(path);
    if (it == light_images_.end()) {
        auto loaded = assets_->load<render::Image>(path);
        if (!loaded) {
            return loaded.error();
        }
        it = light_images_.emplace(path, std::move(loaded).value()).first;
    }
    return it->second.get();
}

render::Lighting CanvasRenderer::lighting(const world::World& world,
                                          const TransformOf& transform_of) {
    constexpr std::string_view kEvent = "a light can't shine";
    const auto images = [this](const std::string& path) { return light_image(path); };
    std::vector<LightProblem> problems;
    SceneLighting scene = collect_lighting(world, images, transform_of, problems, &shadow_cache_);
    const auto name_of = [&](entt::entity entity) {
        const std::string_view id = world.id_of(entity);
        return id.empty() ? "entity " + std::to_string(entt::to_entity(entity)) : std::string(id);
    };
    for (const LightProblem& problem : problems) {
        warn_once(name_of(problem.entity), problem.message, "id", kEvent);
    }
    lit_ = scene.lit;
    render::Lighting out;
    if (!scene.lit) {
        // Nothing lights the world now: let the light textures go.
        for (render::TextureId* texture : {&atlas_, &shadow_map_}) {
            if (*texture != render::TextureId::None) {
                device_->destroy_texture(*texture);
                *texture = render::TextureId::None;
            }
        }
        atlas_key_.clear();
        atlas_rects_.clear();
        shadow_rows_ = 0;
        shadow_pixels_.clear();
        return out;
    }
    out.ambient = {
        .r = scene.ambient[0], .g = scene.ambient[1], .b = scene.ambient[2], .a = scene.ambient[3]};

    // The atlas, packed again when a light texture comes, goes or changes.
    std::vector<std::pair<std::string, std::uint64_t>> key;
    for (const SceneLight& light : scene.lights) {
        if (light.light.kind != render::LightKind::Point) {
            continue;
        }
        const std::uint64_t version =
            light.texture.empty() ? 0 : light_images_.at(light.texture).version();
        if (std::ranges::find(key, std::pair{light.texture, version}) == key.end()) {
            key.emplace_back(light.texture, version);
        }
    }
    if (key != atlas_key_) {
        if (atlas_ != render::TextureId::None) {
            device_->destroy_texture(atlas_);
            atlas_ = render::TextureId::None;
        }
        atlas_key_.clear();
        atlas_rects_.clear();
        if (!key.empty()) {
            auto atlas = pack_light_atlas(scene.lights, images);
            if (!atlas) {
                warn_once("atlas", atlas.error().message, "lights", kEvent);
            } else if (auto made = device_->create_texture({.width = atlas.value().width,
                                                            .height = atlas.value().height,
                                                            .filter = render::Filter::Linear,
                                                            .render_target = false},
                                                           atlas.value().pixels);
                       !made) {
                warn_once("atlas", made.error().message, "lights", kEvent);
            } else {
                atlas_ = made.value();
                atlas_key_ = key;
                for (const SceneLight& light : scene.lights) {
                    atlas_rects_[light.texture] = light.light.atlas_rect;
                }
            }
        }
    }
    for (SceneLight& light : scene.lights) {
        if (light.light.kind != render::LightKind::Point) {
            out.lights.push_back(light.light);
        } else if (const auto rect = atlas_rects_.find(light.texture); rect != atlas_rects_.end()) {
            light.light.atlas_rect = rect->second;
            out.lights.push_back(light.light);
        }
    }
    out.atlas = atlas_;

    // The shadow map, uploaded when its rows change.
    if (scene.shadow_rows != shadow_rows_ && shadow_map_ != render::TextureId::None) {
        device_->destroy_texture(shadow_map_);
        shadow_map_ = render::TextureId::None;
        shadow_pixels_.clear();
    }
    shadow_rows_ = scene.shadow_rows;
    if (scene.shadow_rows > 0) {
        Result<void> uploaded;
        if (shadow_map_ == render::TextureId::None) {
            auto made = device_->create_texture({.width = render::kShadowMapWidth,
                                                 .height = scene.shadow_rows,
                                                 .filter = render::Filter::Nearest,
                                                 .render_target = false},
                                                scene.shadow_pixels);
            if (made) {
                shadow_map_ = made.value();
            } else {
                uploaded = made.error();
            }
        } else if (scene.shadow_pixels != shadow_pixels_) {
            uploaded = device_->update_texture(shadow_map_, scene.shadow_pixels);
        }
        if (!uploaded || shadow_map_ == render::TextureId::None) {
            if (!uploaded) {
                warn_once("shadow map", uploaded.error().message, "lights", kEvent);
            }
            for (render::Light& light : out.lights) {
                light.shadow_row = -1;
            }
        } else {
            shadow_pixels_ = std::move(scene.shadow_pixels);
            out.shadow_map = shadow_map_;
        }
    }
    return out;
}

Result<const CanvasRenderer::Texture*> CanvasRenderer::texture(const std::string& path,
                                                               render::Filter filter) {
    const auto key = std::make_pair(path, filter);
    auto it = textures_.find(key);
    if (it == textures_.end()) {
        Texture entry;
        if (path.starts_with(ui::kBuiltinPrefix)) {
            entry.builtin = ui::builtin_icon(path);
            if (entry.builtin == nullptr) {
                return make_error(ErrorCode::NotFound, "no built-in icon \"" + path + "\"");
            }
        } else {
            auto image = assets_->load<render::Image>(path);
            if (!image) {
                return image.error();
            }
            entry.image = std::move(image).value();
        }
        it = textures_.emplace(key, std::move(entry)).first;
    }
    Texture& entry = it->second;
    if (entry.id != render::TextureId::None && entry.version == entry.image_version()) {
        return &entry;
    }
    // New, or its file changed since it was made.
    const render::Image& image = entry.pixels();
    if (entry.id != render::TextureId::None &&
        (entry.width != image.width || entry.height != image.height)) {
        device_->destroy_texture(entry.id);
        entry.id = render::TextureId::None;
    }
    if (entry.id == render::TextureId::None) {
        auto made = device_->create_texture({.width = image.width,
                                             .height = image.height,
                                             .filter = filter,
                                             .render_target = false},
                                            image.pixels);
        if (!made) {
            return made.error();
        }
        entry.id = made.value();
    } else if (auto updated = device_->update_texture(entry.id, image.pixels); !updated) {
        return updated.error();
    }
    entry.version = entry.image_version();
    entry.width = image.width;
    entry.height = image.height;
    return &entry;
}

Result<render::Material> CanvasRenderer::material(const world::Sprite2D& sprite,
                                                  render::Filter filter, double time,
                                                  render::LightMode* light_mode) {
    auto it = materials_.find(sprite.material);
    if (it == materials_.end()) {
        auto loaded = assets_->load<CanvasMaterial>(sprite.material);
        if (!loaded) {
            return loaded.error();
        }
        it = materials_.emplace(sprite.material, std::move(loaded).value()).first;
    }
    std::string failed;
    auto instance = instantiate(*it->second, sprite.material_params, time, &failed);
    if (!instance) {
        return make_error(instance.error().code,
                          "material_params/" + failed + ": " + instance.error().message);
    }
    render::Material out;
    out.blend = instance.value().blend;
    *light_mode = instance.value().light_mode;
    out.program = std::move(instance.value().program);
    for (std::size_t slot = 0; slot < out.textures.size(); ++slot) {
        const MaterialTextureRef& ref = instance.value().textures.at(slot);
        if (ref.path.empty()) {
            continue;
        }
        auto texture = this->texture(ref.path, resolve_filter(ref.filter, filter));
        if (!texture) {
            return make_error(texture.error().code, ref.path + ": " + texture.error().message);
        }
        out.textures.at(slot) = {.texture = texture.value()->id, .repeat = ref.repeat};
    }
    return out;
}

Result<render::TextureId> CanvasRenderer::atlas_page(const text::Fonts& fonts, std::uint32_t page) {
    // Another world's fonts have their own pages: upload every page again.
    if (pages_of_ != &fonts) {
        pages_of_ = &fonts;
        for (auto& entry : pages_) {
            entry.second = static_cast<std::uint64_t>(-1);
        }
    }
    const text::AtlasPage& source = fonts.pages().at(page);
    while (pages_.size() <= page) {
        pages_.emplace_back(render::TextureId::None, static_cast<std::uint64_t>(-1));
    }
    auto& [texture, version] = pages_[page];
    if (texture == render::TextureId::None) {
        auto created = device_->create_texture({.width = source.width,
                                                .height = source.height,
                                                .filter = render::Filter::Linear,
                                                .render_target = false},
                                               source.rgba);
        if (!created) {
            return created.error();
        }
        texture = created.value();
        version = source.version;
    } else if (version != source.version) {
        if (auto updated = device_->update_texture(texture, source.rgba); !updated) {
            return updated.error();
        }
        version = source.version;
    }
    return texture;
}

Result<void> CanvasRenderer::draw(const world::World& world, const CanvasOptions& options) {
    // A sprite, or a run of a layer's tiles with one texture: vertices from first, in order.
    struct Item {
        int z_index = 0;
        std::uint32_t order = 0;
        std::uint32_t part = 0;
        render::TextureId texture = render::TextureId::None;
        std::size_t first = 0;
        std::size_t count = 0;
        render::Material material;
        // For FrameContents.
        entt::entity entity = entt::null;
        FrameItem::Kind kind = FrameItem::Kind::Sprite;
        const render::Image* image = nullptr; // white when null
        std::size_t drawn = 0;                // tiles or particles
    };
    std::vector<Item> items;
    std::vector<render::Vertex2D> vertices;
    std::size_t sprites = 0;
    std::size_t tiles = 0;
    std::size_t particles = 0;
    const auto& ecs = world.ecs();
    const auto name_of = [&](entt::entity entity) {
        const std::string_view id = world.id_of(entity);
        return id.empty() ? "entity " + std::to_string(entt::to_entity(entity)) : std::string(id);
    };
    const auto transform_of = [&](entt::entity entity, const world::Transform2D& current) {
        if (options.interpolation < 1.0) {
            if (const auto before = previous_.find(entity); before != previous_.end()) {
                return interpolate(before->second, current, options.interpolation);
            }
        }
        return current;
    };
    FrameContents* contents = options.contents;
    if (contents != nullptr) {
        *contents = {};
    }
    // Something that can't draw as written, for FrameContents.
    const auto problem = [&](entt::entity entity, std::string path, std::string message) {
        if (contents != nullptr) {
            contents->problems.push_back(
                {.entity = entity, .path = std::move(path), .message = std::move(message)});
        }
    };
    const render::Lighting lights = lighting(world, transform_of);
    // How lights shade a sprite or layer: not at all in an unlit world.
    const auto shading_for = [&](render::LightMode mode, int light_mask, int z_index) {
        render::Shading shading;
        if (lit_) {
            shading.mode = mode;
            shading.light_mask = static_cast<std::uint32_t>(std::max(light_mask, 0));
            shading.z_index = z_index;
        }
        return shading;
    };
    for (const auto [entity, current, sprite] :
         ecs.view<const world::Transform2D, const world::Sprite2D>().each()) {
        if (!sprite.visible || sprite.texture.empty()) {
            continue;
        }
        const render::Filter filter = resolve_filter(sprite.texture_filter, options.texture_filter);
        auto texture = this->texture(sprite.texture, filter);
        if (!texture) {
            warn_once(sprite.texture, texture.error().message, "texture");
            problem(entity, "/components/Sprite2D/texture", texture.error().message);
            continue;
        }
        render::Material material;
        render::LightMode light_mode = render::LightMode::Normal;
        if (!sprite.material.empty()) {
            auto made = this->material(sprite, filter, options.time, &light_mode);
            if (!made) {
                warn_once(sprite.material, made.error().message, "material");
                problem(entity, "/components/Sprite2D/material", made.error().message);
                continue;
            }
            material = std::move(made).value();
        }
        material.shading = shading_for(light_mode, sprite.light_mask, sprite.z_index);
        if (material.shading.mode != render::LightMode::Unshaded) {
            // Normal and specular maps, left out with a warning if they don't load.
            for (const auto& [path, slot, field] :
                 {std::tuple{&sprite.normal_texture, &material.shading.normal_map,
                             "normal_texture"},
                  std::tuple{&sprite.specular_texture, &material.shading.specular_map,
                             "specular_texture"}}) {
                if (path->empty()) {
                    continue;
                }
                if (auto map = this->texture(*path, filter); map) {
                    *slot = map.value()->id;
                } else {
                    warn_once(*path, map.error().message, "texture");
                    problem(entity, std::string("/components/Sprite2D/") + field,
                            map.error().message);
                }
            }
            material.shading.specular = {sprite.specular_color.x, sprite.specular_color.y,
                                         sprite.specular_color.z,
                                         sprite.specular_color.w * sprite.specular_shininess};
        }
        auto quad = sprite_quad(sprite, transform_of(entity, current), texture.value()->width,
                                texture.value()->height);
        if (!quad) {
            warn_once(name_of(entity), quad.error().message, "id");
            problem(entity, "/components/Sprite2D", quad.error().message);
            continue;
        }
        items.push_back({.z_index = sprite.z_index,
                         .order = static_cast<std::uint32_t>(entt::to_entity(entity)),
                         .part = 0,
                         .texture = texture.value()->id,
                         .first = vertices.size(),
                         .count = quad.value().size(),
                         .material = std::move(material),
                         .entity = entity,
                         .kind = FrameItem::Kind::Sprite,
                         .image = &*texture.value()->image,
                         .drawn = 1});
        vertices.insert(vertices.end(), quad.value().begin(), quad.value().end());
        ++sprites;
    }

    for (const auto [entity, emitter] : ecs.view<const world::CPUParticles2D>().each()) {
        if (!emitter.visible) {
            continue;
        }
        const std::vector<Particle> live = canvas::particles(world, entity);
        if (live.empty()) {
            continue;
        }
        constexpr std::string_view kEvent = "particles can't draw";
        const render::Filter filter =
            resolve_filter(emitter.texture_filter, options.texture_filter);
        // A particle is its texture's frame, or a white pixel.
        render::TextureId texture_id = render::TextureId::None;
        const render::Image* image = nullptr;
        float frame_w = 1.0F;
        float frame_h = 1.0F;
        float sheet_w = 1.0F;
        float sheet_h = 1.0F;
        const int hframes = std::max(emitter.hframes, 1);
        const int vframes = std::max(emitter.vframes, 1);
        if (!emitter.texture.empty()) {
            auto texture = this->texture(emitter.texture, filter);
            if (!texture) {
                warn_once(emitter.texture, texture.error().message, "texture", kEvent);
                problem(entity, "/components/CPUParticles2D/texture", texture.error().message);
                continue;
            }
            texture_id = texture.value()->id;
            image = &*texture.value()->image;
            sheet_w = static_cast<float>(texture.value()->width);
            sheet_h = static_cast<float>(texture.value()->height);
            frame_w = sheet_w / static_cast<float>(hframes);
            frame_h = sheet_h / static_cast<float>(vframes);
        }
        render::Material material;
        render::LightMode light_mode = render::LightMode::Normal;
        if (!emitter.material.empty()) {
            world::Sprite2D look;
            look.material = emitter.material;
            look.material_params = emitter.material_params;
            auto made = this->material(look, filter, options.time, &light_mode);
            if (!made) {
                warn_once(emitter.material, made.error().message, "material", kEvent);
                problem(entity, "/components/CPUParticles2D/material", made.error().message);
                continue;
            }
            material = std::move(made).value();
        }
        material.shading = shading_for(light_mode, emitter.light_mask, emitter.z_index);
        // With local_coords, particles move with the entity's transform.
        world::Transform2D local;
        if (const auto* current = ecs.try_get<world::Transform2D>(entity);
            emitter.local_coords && current != nullptr) {
            local = transform_of(entity, *current);
        }
        const auto place = [&](Vec2 point) { return world_position(emitter, local, point); };
        const int total = hframes * vframes;
        const auto t = static_cast<float>(std::clamp(options.interpolation, 0.0, 1.0));
        const std::size_t first = vertices.size();
        for (const Particle& particle : live) {
            // The frame its animation is on, as Godot's particle animation picks it.
            auto frame = static_cast<int>(std::floor(particle.frame * static_cast<float>(total)));
            frame = emitter.anim_loop ? ((frame % total) + total) % total
                                      : std::clamp(frame, 0, total - 1);
            const int column = frame % hframes;
            const int row = frame / hframes;
            const float u0 = static_cast<float>(column) * frame_w / sheet_w;
            const float v0 = static_cast<float>(row) * frame_h / sheet_h;
            const float u1 = u0 + (frame_w / sheet_w);
            const float v1 = v0 + (frame_h / sheet_h);
            const Vec2 at{particle.previous.x + ((particle.position.x - particle.previous.x) * t),
                          particle.previous.y + ((particle.position.y - particle.previous.y) * t)};
            const Vec2 x_axis = particle.axis * (frame_w * 0.5F * particle.scale);
            const Vec2 y_axis =
                Vec2{-particle.axis.y, particle.axis.x} * (frame_h * 0.5F * particle.scale);
            const std::array<std::uint8_t, 4> rgba{
                to_byte(particle.color.x), to_byte(particle.color.y), to_byte(particle.color.z),
                to_byte(particle.color.w)};
            const auto corner = [&](float sx, float sy, float u, float v) {
                const Vec2 point = place(at + (x_axis * sx) + (y_axis * sy));
                return render::Vertex2D{.x = point.x, .y = point.y, .u = u, .v = v, .rgba = rgba};
            };
            const render::Vertex2D top_left = corner(-1.0F, -1.0F, u0, v0);
            const render::Vertex2D top_right = corner(1.0F, -1.0F, u1, v0);
            const render::Vertex2D bottom_left = corner(-1.0F, 1.0F, u0, v1);
            const render::Vertex2D bottom_right = corner(1.0F, 1.0F, u1, v1);
            vertices.insert(vertices.end(), {top_left, top_right, bottom_left, top_right,
                                             bottom_right, bottom_left});
        }
        items.push_back({.z_index = emitter.z_index,
                         .order = static_cast<std::uint32_t>(entt::to_entity(entity)),
                         .part = 0,
                         .texture = texture_id,
                         .first = first,
                         .count = vertices.size() - first,
                         .material = std::move(material),
                         .entity = entity,
                         .kind = FrameItem::Kind::Particles,
                         .image = image,
                         .drawn = live.size()});
        particles += live.size();
    }

    for (auto& [entity, cached] : layer_cells_) {
        cached.seen = false;
    }
    for (const auto [entity, layer] : ecs.view<const world::TileMapLayer>().each()) {
        if (!layer.visible || layer.tile_set.empty() || layer.rows.empty()) {
            continue;
        }
        constexpr std::string_view kEvent = "a tile map layer can't draw";
        auto set = tile_set(layer.tile_set);
        if (!set) {
            warn_once(layer.tile_set, set.error().message, "tile_set", kEvent);
            problem(entity, "/components/TileMapLayer/tile_set", set.error().message);
            continue;
        }
        const LayerCells& cached =
            cells(entity, layer, *set.value(), tile_sets_.at(layer.tile_set).version());
        if (!cached.error.empty()) {
            warn_once(name_of(entity), "TileMapLayer" + cached.error, "id", kEvent);
            // The error starts with the bad value's JSON Pointer in the layer.
            const std::size_t split = cached.error.find(": ");
            problem(entity,
                    "/components/TileMapLayer" +
                        (split == std::string::npos ? "" : cached.error.substr(0, split)),
                    split == std::string::npos ? cached.error : cached.error.substr(split + 2));
            continue;
        }
        world::Transform2D transform;
        if (const auto* current = ecs.try_get<world::Transform2D>(entity); current != nullptr) {
            transform = transform_of(entity, *current);
        }
        const float cos_r = std::cos(transform.rotation);
        const float sin_r = std::sin(transform.rotation);
        const std::array<std::uint8_t, 4> rgba{to_byte(layer.modulate.x), to_byte(layer.modulate.y),
                                               to_byte(layer.modulate.z),
                                               to_byte(layer.modulate.w)};
        const auto corner = [&](float x, float y, float u, float v) {
            const float sx = x * transform.scale.x;
            const float sy = y * transform.scale.y;
            return render::Vertex2D{.x = transform.position.x + (sx * cos_r) - (sy * sin_r),
                                    .y = transform.position.y + (sx * sin_r) + (sy * cos_r),
                                    .u = u,
                                    .v = v,
                                    .rgba = rgba};
        };
        const render::Filter filter = resolve_filter(layer.texture_filter, options.texture_filter);
        const Vec2i size = set.value()->tile_size;
        // Tiles go in runs with one texture; each run is one draw.
        constexpr std::size_t kNoRun = SIZE_MAX;
        std::size_t run = kNoRun;
        std::uint32_t part = 0;
        for (const auto& [cell, tile] : cached.cells) {
            const AtlasSource& source = set.value()->sources.find(tile.source)->second;
            auto texture = this->texture(source.texture, filter);
            if (!texture) {
                warn_once(source.texture, texture.error().message, "texture", kEvent);
                problem(entity, "/components/TileMapLayer/tile_set",
                        source.texture + ": " + texture.error().message);
                continue;
            }
            const auto [rx, ry, rw, rh] = tile_region(source, tile.atlas);
            const auto width = static_cast<int>(texture.value()->width);
            const auto height = static_cast<int>(texture.value()->height);
            if (rx + rw > width || ry + rh > height) {
                std::string message =
                    "the tile " + tile.source + ":" + std::to_string(tile.atlas.x) + "," +
                    std::to_string(tile.atlas.y) + " at [" + std::to_string(rx) + ", " +
                    std::to_string(ry) + ", " + std::to_string(rw) + ", " + std::to_string(rh) +
                    "] goes outside the " + std::to_string(width) + "x" + std::to_string(height) +
                    " texture " + source.texture;
                warn_once(layer.tile_set, message, "tile_set", kEvent);
                problem(entity, "/components/TileMapLayer/tile_set", std::move(message));
                continue;
            }
            if (run == kNoRun || items[run].texture != texture.value()->id) {
                render::Material material;
                material.shading =
                    shading_for(render::LightMode::Normal, layer.light_mask, layer.z_index);
                items.push_back({.z_index = layer.z_index,
                                 .order = static_cast<std::uint32_t>(entt::to_entity(entity)),
                                 .part = part++,
                                 .texture = texture.value()->id,
                                 .first = vertices.size(),
                                 .count = 0,
                                 .material = std::move(material),
                                 .entity = entity,
                                 .kind = FrameItem::Kind::Tiles,
                                 .image = &*texture.value()->image,
                                 .drawn = 0});
                run = items.size() - 1;
            }
            // The tile's region, centered on its cell.
            const float cx = (static_cast<float>(cell.x) + 0.5F) * static_cast<float>(size.x);
            const float cy = (static_cast<float>(cell.y) + 0.5F) * static_cast<float>(size.y);
            const float x0 = cx - (static_cast<float>(rw) / 2.0F);
            const float y0 = cy - (static_cast<float>(rh) / 2.0F);
            const float x1 = x0 + static_cast<float>(rw);
            const float y1 = y0 + static_cast<float>(rh);
            const float u0 = static_cast<float>(rx) / static_cast<float>(width);
            const float v0 = static_cast<float>(ry) / static_cast<float>(height);
            const float u1 = static_cast<float>(rx + rw) / static_cast<float>(width);
            const float v1 = static_cast<float>(ry + rh) / static_cast<float>(height);
            const render::Vertex2D top_left = corner(x0, y0, u0, v0);
            const render::Vertex2D top_right = corner(x1, y0, u1, v0);
            const render::Vertex2D bottom_left = corner(x0, y1, u0, v1);
            const render::Vertex2D bottom_right = corner(x1, y1, u1, v1);
            vertices.insert(vertices.end(), {top_left, top_right, bottom_left, top_right,
                                             bottom_right, bottom_left});
            items[run].count += 6;
            ++items[run].drawn;
            ++tiles;
        }
    }
    std::erase_if(layer_cells_, [](const auto& entry) { return !entry.second.seen; });

    std::ranges::sort(items, [](const Item& a, const Item& b) {
        if (a.z_index != b.z_index) {
            return a.z_index < b.z_index;
        }
        return a.order != b.order ? a.order < b.order : a.part < b.part;
    });

    // Collision shapes, over the world.
    CollisionShapes shapes;
    if (options.collisions) {
        shapes = collision_shapes(world, transform_of);
    }

    // Controls, over the world, then the overlay's.
    text::Fonts* fonts = ui::fonts(world);
    if (fonts == nullptr) {
        if (fonts_ == nullptr) {
            text::add_asset_types(*assets_);
            fonts_ = std::make_unique<text::Fonts>(*assets_);
        }
        fonts = fonts_.get();
    }
    const Vec2 viewport = options.width > 0.0F && options.height > 0.0F
                              ? Vec2{options.width, options.height}
                              : ui::settings(world).viewport;
    ui::Layout layout = ui::layout(world, *fonts, viewport);
    const ui::DrawList controls = ui::draw_calls(layout, world, *fonts);
    ui::DrawList overlay;
    if (options.overlay != nullptr) {
        overlay = ui::draw_calls(ui::layout(*options.overlay, *fonts, viewport), *options.overlay,
                                 *fonts);
    }
    // Their textures, made now so the null device checks them too; a call whose texture doesn't
    // load draws nothing (layout leaves out the boxes whose textures don't).
    const auto textures_of = [&](const ui::DrawList& list) {
        std::vector<std::optional<render::TextureId>> made(list.calls.size());
        for (std::size_t i = 0; i < list.calls.size(); ++i) {
            const ui::DrawCall& call = list.calls[i];
            if (call.source != ui::DrawCall::Source::Texture) {
                made[i] = render::TextureId::None;
                continue;
            }
            if (auto loaded =
                    texture(call.texture, resolve_filter(call.filter, options.texture_filter));
                loaded) {
                made[i] = loaded.value()->id;
            }
        }
        return made;
    };
    const std::vector<std::optional<render::TextureId>> control_textures = textures_of(controls);
    const std::vector<std::optional<render::TextureId>> overlay_textures = textures_of(overlay);

    if (contents != nullptr) {
        // Each item's draws together: a layer's runs, a control's boxes and text.
        const auto add = [&](FrameItem::Kind kind, entt::entity entity) -> FrameItem& {
            if (contents->items.empty() || contents->items.back().entity != entity ||
                contents->items.back().kind != kind) {
                contents->items.push_back(
                    {.kind = kind, .entity = entity, .count = 0, .pieces = {}});
            }
            return contents->items.back();
        };
        for (const Item& item : items) {
            FrameItem& out = add(item.kind, item.entity);
            out.count += item.drawn;
            const auto from = vertices.begin() + static_cast<std::ptrdiff_t>(item.first);
            out.pieces.push_back(
                {.pixels = item.image != nullptr ? item.image->pixels.data() : nullptr,
                 .width = item.image != nullptr ? item.image->width : 0,
                 .height = item.image != nullptr ? item.image->height : 0,
                 .field = {},
                 .vertices = {from, from + static_cast<std::ptrdiff_t>(item.count)}});
        }
        for (const ui::DrawSpan& span : controls.spans) {
            const ui::DrawCall& call = controls.calls[span.call];
            if (!control_textures[span.call]) {
                continue;
            }
            const auto from = call.vertices.begin() + static_cast<std::ptrdiff_t>(span.first);
            FramePiece piece{.pixels = nullptr,
                             .width = 0,
                             .height = 0,
                             .field = call.field,
                             .vertices = {from, from + static_cast<std::ptrdiff_t>(span.count)}};
            if (call.source == ui::DrawCall::Source::Atlas) {
                const text::AtlasPage& page = fonts->pages().at(call.page);
                piece.pixels = page.rgba.data();
                piece.width = page.width;
                piece.height = page.height;
            } else if (call.source == ui::DrawCall::Source::Texture) {
                const auto found = textures_.find(
                    {call.texture, resolve_filter(call.filter, options.texture_filter)});
                if (found == textures_.end()) {
                    continue;
                }
                const render::Image& image = found->second.pixels();
                piece.pixels = image.pixels.data();
                piece.width = image.width;
                piece.height = image.height;
            }
            add(FrameItem::Kind::Control, span.entity).pieces.push_back(std::move(piece));
        }
        contents->layout = std::move(layout);
    }

    // The null device stands in for a window it doesn't have, as draw_empty_frame allows: the
    // sprites and tiles are checked and counted, and nothing is drawn.
    const auto shadows = static_cast<std::size_t>(std::ranges::count_if(
        lights.lights, [](const render::Light& l) { return l.shadow_row >= 0; }));
    if (options.target == render::TextureId::None && device_->backend() == render::Backend::Null) {
        sprites_drawn_ = sprites;
        tiles_drawn_ = tiles;
        particles_drawn_ = particles;
        lights_drawn_ = lights.lights.size();
        shadows_drawn_ = shadows;
        controls_drawn_ = controls.controls;
        glyphs_drawn_ = controls.glyphs;
        boxes_drawn_ = controls.boxes;
        shapes_drawn_ = shapes.shapes;
        return {};
    }
    render::Frame frame;
    frame.target = options.target;
    frame.clear = options.clear;
    frame.lighting = lights;
    if (options.width > 0.0F && options.height > 0.0F) {
        frame.view = render::pixel_projection(options.width, options.height);
    }
    auto began = device_->begin_frame(frame);
    if (!began) {
        return began.error();
    }
    const auto failed = [&](const Error& error) -> Result<void> {
        (void)device_->end_frame();
        return error;
    };
    for (const Item& item : items) {
        const std::span<const render::Vertex2D> span(vertices.data() + item.first, item.count);
        if (auto drawn = device_->draw(item.texture, span, item.material); !drawn) {
            return failed(drawn.error());
        }
    }
    if (!shapes.vertices.empty()) {
        if (auto drawn = device_->draw(render::TextureId::None, shapes.vertices); !drawn) {
            return failed(drawn.error());
        }
    }
    const auto draw_controls =
        [&](const ui::DrawList& list,
            const std::vector<std::optional<render::TextureId>>& textures) -> Result<void> {
        for (std::size_t i = 0; i < list.calls.size(); ++i) {
            const ui::DrawCall& call = list.calls[i];
            if (!textures[i]) {
                continue;
            }
            render::TextureId texture = *textures[i];
            if (call.source == ui::DrawCall::Source::Atlas) {
                auto page = atlas_page(*fonts, call.page);
                if (!page) {
                    return page.error();
                }
                texture = page.value();
            }
            render::Material material;
            material.field = call.field;
            if (auto drawn = device_->draw(texture, call.vertices, material); !drawn) {
                return drawn.error();
            }
        }
        return {};
    };
    if (auto drawn = draw_controls(controls, control_textures); !drawn) {
        return failed(drawn.error());
    }
    if (auto drawn = draw_controls(overlay, overlay_textures); !drawn) {
        return failed(drawn.error());
    }
    controls_drawn_ = began.value() ? controls.controls : 0;
    glyphs_drawn_ = began.value() ? controls.glyphs : 0;
    boxes_drawn_ = began.value() ? controls.boxes : 0;
    sprites_drawn_ = began.value() ? sprites : 0;
    tiles_drawn_ = began.value() ? tiles : 0;
    particles_drawn_ = began.value() ? particles : 0;
    lights_drawn_ = began.value() ? lights.lights.size() : 0;
    shadows_drawn_ = began.value() ? shadows : 0;
    shapes_drawn_ = began.value() ? shapes.shapes : 0;
    return device_->end_frame();
}

} // namespace slash::canvas
