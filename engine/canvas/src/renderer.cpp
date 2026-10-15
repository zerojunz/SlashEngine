#include "slash/canvas/canvas.hpp"
#include "slash/core/log.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <string>
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
}

void CanvasRenderer::before_tick(const world::World& world) {
    previous_.clear();
    const auto& ecs = world.ecs();
    for (const auto [entity, transform, sprite] :
         ecs.view<const world::Transform2D, const world::Sprite2D>().each()) {
        previous_.emplace(entity, transform);
    }
}

void CanvasRenderer::warn_once(const std::string& key, std::string_view message,
                               std::string_view what) {
    if (warned_.insert(key + "\n" + std::string(message)).second) {
        log::warn("a sprite can't draw", {{what, key}, {"error", message}});
    }
}

Result<const CanvasRenderer::Texture*> CanvasRenderer::texture(const std::string& path,
                                                               render::Filter filter) {
    const auto key = std::make_pair(path, filter);
    auto it = textures_.find(key);
    if (it == textures_.end()) {
        auto image = assets_->load<render::Image>(path);
        if (!image) {
            return image.error();
        }
        Texture entry;
        entry.image = std::move(image).value();
        it = textures_.emplace(key, std::move(entry)).first;
    }
    Texture& entry = it->second;
    if (entry.id != render::TextureId::None && entry.version == entry.image.version()) {
        return &entry;
    }
    // New, or its file changed since it was made.
    const render::Image& image = *entry.image;
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
    entry.version = entry.image.version();
    entry.width = image.width;
    entry.height = image.height;
    return &entry;
}

Result<render::Material> CanvasRenderer::material(const world::Sprite2D& sprite,
                                                  render::Filter filter, double time) {
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

Result<void> CanvasRenderer::draw(const world::World& world, const CanvasOptions& options) {
    struct Item {
        int z_index = 0;
        std::uint32_t order = 0;
        render::TextureId texture = render::TextureId::None;
        std::array<render::Vertex2D, 6> quad{};
        render::Material material;
    };
    std::vector<Item> items;
    const auto& ecs = world.ecs();
    for (const auto [entity, current, sprite] :
         ecs.view<const world::Transform2D, const world::Sprite2D>().each()) {
        if (!sprite.visible || sprite.texture.empty()) {
            continue;
        }
        const render::Filter filter = resolve_filter(sprite.texture_filter, options.texture_filter);
        auto texture = this->texture(sprite.texture, filter);
        if (!texture) {
            warn_once(sprite.texture, texture.error().message, "texture");
            continue;
        }
        render::Material material;
        if (!sprite.material.empty()) {
            auto made = this->material(sprite, filter, options.time);
            if (!made) {
                warn_once(sprite.material, made.error().message, "material");
                continue;
            }
            material = std::move(made).value();
        }
        world::Transform2D transform = current;
        if (options.interpolation < 1.0) {
            if (const auto before = previous_.find(entity); before != previous_.end()) {
                transform = interpolate(before->second, current, options.interpolation);
            }
        }
        auto quad = sprite_quad(sprite, transform, texture.value()->width, texture.value()->height);
        if (!quad) {
            const std::string_view id = world.id_of(entity);
            warn_once(id.empty() ? "entity " + std::to_string(entt::to_entity(entity))
                                 : std::string(id),
                      quad.error().message, "id");
            continue;
        }
        items.push_back({.z_index = sprite.z_index,
                         .order = static_cast<std::uint32_t>(entt::to_entity(entity)),
                         .texture = texture.value()->id,
                         .quad = quad.value(),
                         .material = std::move(material)});
    }
    std::ranges::sort(items, [](const Item& a, const Item& b) {
        return a.z_index != b.z_index ? a.z_index < b.z_index : a.order < b.order;
    });

    // The null device stands in for a window it doesn't have, as draw_empty_frame allows: the
    // sprites are checked and counted, and nothing is drawn.
    if (options.target == render::TextureId::None && device_->backend() == render::Backend::Null) {
        sprites_drawn_ = items.size();
        return {};
    }
    render::Frame frame;
    frame.target = options.target;
    frame.clear = options.clear;
    if (options.width > 0.0F && options.height > 0.0F) {
        frame.view = render::pixel_projection(options.width, options.height);
    }
    auto began = device_->begin_frame(frame);
    if (!began) {
        return began.error();
    }
    for (const Item& item : items) {
        if (auto drawn = device_->draw(item.texture, item.quad, item.material); !drawn) {
            (void)device_->end_frame();
            return drawn.error();
        }
    }
    sprites_drawn_ = began.value() ? items.size() : 0;
    return device_->end_frame();
}

} // namespace slash::canvas
