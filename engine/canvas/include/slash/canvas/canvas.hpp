#pragma once

#include "slash/asset/assets.hpp"
#include "slash/canvas/material.hpp"
#include "slash/core/result.hpp"
#include "slash/render/device.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

// 2D drawing, as Godot's canvas: Sprite2D and AnimatedSprite2D entities drawn through a render
// device, in world pixels with y down.
namespace slash::canvas {

// Asset types: "texture" (.png and .bmp files as render::Image), "sprite_frames" (.frames.json
// files as SpriteFrames) and "material" (.material.json files as CanvasMaterial).
void add_asset_types(asset::Assets& assets);

// The "animated_sprite_2d" system, which plays each AnimatedSprite2D on its Sprite2D. It reads
// .frames.json files through assets, which must outlive the world.
void add_systems(world::World& world, asset::Assets& assets);

// Both of the above, for a world and the assets it loads from.
void install(world::World& world, asset::Assets& assets);

// The two triangles a sprite draws, in world pixels, for a texture of the given size: the frame
// it picks from the region and sheet, centered or not, offset, flipped, then scaled, rotated and
// moved by the transform. Fails, saying why, when there is nothing to draw, such as a frame
// outside the sheet.
[[nodiscard]] Result<std::array<render::Vertex2D, 6>>
sprite_quad(const world::Sprite2D& sprite, const world::Transform2D& transform,
            std::uint32_t texture_width, std::uint32_t texture_height);

// A transform part of the way from one tick's to the next: alpha 0 is from, 1 is to. Rotation
// turns the short way round.
[[nodiscard]] world::Transform2D interpolate(const world::Transform2D& from,
                                             const world::Transform2D& to, double alpha);

struct CanvasOptions {
    render::TextureId target = render::TextureId::None; // None draws to the window
    render::Color clear = render::kDefaultClearColor;
    // The world pixels across the target; 0 uses the target's own pixels. A window on a
    // high-density display has more pixels than its size, so games pass the window's size.
    float width = 0.0F;
    float height = 0.0F;
    // How far from the transforms before the last tick to the current ones; see
    // CanvasRenderer::before_tick. 1 draws the current ones.
    double interpolation = 1.0;
    // The filter for sprites whose texture_filter is "inherit".
    render::Filter texture_filter = render::Filter::Linear;
    // Seconds of game time, which materials read as time. Callers pass the moment the frame
    // shows, between ticks too; see material_time.
    double time = 0.0;
};

// The time a frame shows, for CanvasOptions::time: tick ticks of tick_seconds have run, and the
// frame is interpolation of the way from the tick before to the last one.
[[nodiscard]] double material_time(std::uint64_t tick, double tick_seconds, double interpolation);

// Draws a world's sprites. Textures and materials load through the assets, by path, and reload
// when their files change.
class CanvasRenderer {
public:
    CanvasRenderer(render::RenderDevice& device, asset::Assets& assets);
    ~CanvasRenderer();
    CanvasRenderer(const CanvasRenderer&) = delete;
    CanvasRenderer& operator=(const CanvasRenderer&) = delete;
    CanvasRenderer(CanvasRenderer&&) = delete;
    CanvasRenderer& operator=(CanvasRenderer&&) = delete;

    // Remembers every sprite's transform. Call it just before each tick, so frames between
    // ticks can draw sprites part of the way from where they were to where they are.
    void before_tick(const world::World& world);

    // Draws one frame: the target cleared, then every visible sprite. A sprite that can't draw,
    // such as one whose texture or material is missing, or whose material_params name a param
    // the material doesn't have, is left out with one warning in the log. On the null device a
    // window frame checks and counts the sprites but draws nothing.
    [[nodiscard]] Result<void> draw(const world::World& world, const CanvasOptions& options);

    // Sprites the last frame drew.
    [[nodiscard]] std::size_t sprites_drawn() const noexcept { return sprites_drawn_; }

private:
    struct Texture {
        asset::Handle<render::Image> image;
        render::TextureId id = render::TextureId::None;
        std::uint64_t version = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
    };

    // The GPU texture for a path and filter, made or updated from the loaded image.
    Result<const Texture*> texture(const std::string& path, render::Filter filter);
    // The material a sprite draws with, its params and the time filled in.
    Result<render::Material> material(const world::Sprite2D& sprite, render::Filter filter,
                                      double time);
    void warn_once(const std::string& key, std::string_view message, std::string_view what);

    render::RenderDevice* device_;
    asset::Assets* assets_;
    std::map<std::pair<std::string, render::Filter>, Texture> textures_;
    std::map<std::string, asset::Handle<CanvasMaterial>, std::less<>> materials_;
    std::unordered_map<entt::entity, world::Transform2D> previous_;
    std::set<std::string, std::less<>> warned_;
    std::size_t sprites_drawn_ = 0;
};

} // namespace slash::canvas
