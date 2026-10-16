#pragma once

#include "slash/asset/assets.hpp"
#include "slash/canvas/debug_draw.hpp"
#include "slash/canvas/light.hpp"
#include "slash/canvas/material.hpp"
#include "slash/canvas/tilemap.hpp"
#include "slash/core/result.hpp"
#include "slash/render/device.hpp"
#include "slash/text/text.hpp"
#include "slash/ui/ui.hpp"
#include "slash/world/camera2d.hpp"
#include "slash/world/components.hpp"
#include "slash/world/tree2d.hpp"
#include "slash/world/world.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// 2D drawing, as Godot's canvas: Sprite2D, AnimatedSprite2D, TileMapLayer and CPUParticles2D
// entities drawn through a render device, in world pixels with y down, seen through the current
// Camera2D (camera2d.hpp) and shaded by 2D lights (light.hpp).
namespace slash::canvas {

// Asset types: "texture" (.png and .bmp files as render::Image), "sprite_frames" (.frames.json
// files as SpriteFrames), "material" (.material.json files as CanvasMaterial) and "tile_set"
// (.tileset.json files as TileSet).
void add_asset_types(asset::Assets& assets);

// The "animated_sprite_2d" system, which plays each AnimatedSprite2D on its Sprite2D, the
// "camera_2d" system (camera2d.hpp), for a screen of ui::settings' viewport size, and the
// "cpu_particles_2d" system (particles.hpp). The first reads .frames.json files through assets,
// which must outlive the world.
void add_systems(world::World& world, asset::Assets& assets);

// Both of the above, and ui::install, for a world and the assets it loads from.
void install(world::World& world, asset::Assets& assets);

// The two triangles a sprite draws, in world pixels, for a texture of the given size: the frame
// it picks from the region and sheet, centered or not, offset, flipped, then scaled, skewed,
// rotated and moved by the transform. Fails, saying why, when there is nothing to draw, such as
// a frame outside the sheet.
[[nodiscard]] Result<std::array<render::Vertex2D, 6>>
sprite_quad(const world::Sprite2D& sprite, const world::Transform2D& transform,
            std::uint32_t texture_width, std::uint32_t texture_height);

// A transform part of the way from one tick's to the next: alpha 0 is from, 1 is to. Rotation
// turns the short way round. Children play no part.
[[nodiscard]] world::Transform2D interpolate(const world::Transform2D& from,
                                             const world::Transform2D& to, double alpha);

// What a 2D entity takes from the sprites, tile map layers and particles of its parents, as
// Godot's CanvasItem: their z_index added to its own (Godot's z_as_relative), hidden with them,
// and tinted by their modulate.
struct Inherited {
    int z_index = 0;
    bool visible = true;
    Vec4 modulate{1.0F, 1.0F, 1.0F, 1.0F};
};
[[nodiscard]] Inherited inherited(const world::World& world, const world::Tree2D& tree,
                                  entt::entity entity);
// An entity's z_index with its parents', held to Godot's -4096 to 4096.
[[nodiscard]] int z_index(int own, const Inherited& from_parents);

// ---- What a frame drew, for frame summaries ----

// One draw of a frame's: triangles in viewport pixels, three vertices each, over a texture's
// straight-alpha RGBA pixels, or white when there are none.
struct FramePiece {
    const std::uint8_t* pixels = nullptr;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    render::DistanceField field; // glyphs drawn from a distance field
    std::vector<render::Vertex2D> vertices;
    std::size_t order = 0; // where it drew among the frame's pieces
};

// Something a frame drew: a sprite, a tile map layer, a CPUParticles2D's particles or a control.
// A control's pieces are cut to its clip, and what it draws over its children (a
// ScrollContainer's scroll bars) draws after them.
struct FrameItem {
    enum class Kind : std::uint8_t { Sprite, Tiles, Particles, Control };
    Kind kind = Kind::Sprite;
    entt::entity entity = entt::null;
    std::size_t count = 0; // tiles or particles drawn
    std::vector<FramePiece> pieces;
    // For a control a clip cut: its pieces as they would draw whole. A control cut away entirely
    // has these and no pieces.
    std::vector<FramePiece> uncut;
};

// Something that should have drawn and couldn't, such as a sprite whose texture is missing.
struct FrameProblem {
    entt::entity entity = entt::null;
    std::string path; // JSON Pointer in the entity, e.g. "/components/Sprite2D/texture"
    std::string message;
};

// What a frame drew, in drawing order, for CanvasOptions::contents. The pixels are the
// renderer's textures' and the world's fonts': read them before anything loads again.
struct FrameContents {
    std::vector<FrameItem> items;
    std::vector<FrameProblem> problems;
    ui::Layout layout;        // the controls as laid out
    world::CameraView camera; // what the world was seen through
};

struct CanvasOptions {
    render::TextureId target = render::TextureId::None; // None draws to the window
    render::Color clear = render::kDefaultClearColor;
    // The viewport's pixels across the target, which the camera fills; 0 uses the target's own
    // pixels. A window on a high-density display has more pixels than its size, so games pass
    // the window's size.
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
    // Draw every collision shape over the world, as Godot's Visible Collision Shapes
    // (debug_draw.hpp).
    bool collisions = false;
    // Another world whose controls draw over everything, laid out in the same viewport with
    // this world's fonts, such as the debug overlay's.
    const world::World* overlay = nullptr;
    // When set, filled with what the frame draws of the world: its items, what couldn't draw
    // and the controls' layout. Collision shapes and the overlay are left out.
    FrameContents* contents = nullptr;
};

// The time a frame shows, for CanvasOptions::time: tick ticks of tick_seconds have run, and the
// frame is interpolation of the way from the tick before to the last one.
[[nodiscard]] double material_time(std::uint64_t tick, double tick_seconds, double interpolation);

// Draws a world's sprites and tile map layers, lit by its lights. Textures, materials and tile
// sets load through the assets, by path, and reload when their files change.
class CanvasRenderer {
public:
    CanvasRenderer(render::RenderDevice& device, asset::Assets& assets);
    ~CanvasRenderer();
    CanvasRenderer(const CanvasRenderer&) = delete;
    CanvasRenderer& operator=(const CanvasRenderer&) = delete;
    CanvasRenderer(CanvasRenderer&&) = delete;
    CanvasRenderer& operator=(CanvasRenderer&&) = delete;

    // Remembers every sprite's, layer's, light's and occluder's transform, and the camera's
    // view. Call it just before each tick, so frames between ticks can draw them part of the way
    // from where they were to where they are.
    void before_tick(const world::World& world);

    // Draws one frame: the target cleared, then every visible sprite, tile map layer and
    // CPUParticles2D, in z_index order, then the order entities were made, then the collision
    // shapes if the options ask, all seen through the current Camera2D, then every visible
    // control (ui.hpp) over them, laid out in a viewport of the options' width and height, or
    // ui::settings' when they are 0, then the overlay's controls. A layer draws its
    // cells row by row, each tile's region centered on its cell, moved by the entity's
    // Transform2D if it has one. Particles draw as their emitter's draw_order says, each its
    // texture's frame (or a white pixel) centered on it, turned, scaled and colored. A
    // sprite or tile that can't draw, such as one whose texture or material is missing, or whose
    // material_params name a param the material doesn't have, is left out with one warning in the
    // log. On the null device a window frame checks and counts them but draws nothing.
    //
    // When the world has a light or a CanvasModulate, every sprite and layer is shaded by them,
    // as its material's light_mode says (normal by default), with its normal and specular
    // textures. A light that can't shine, such as one whose texture is missing, is left out with
    // one warning.
    [[nodiscard]] Result<void> draw(const world::World& world, const CanvasOptions& options);

    // Sprites the last frame drew.
    [[nodiscard]] std::size_t sprites_drawn() const noexcept { return sprites_drawn_; }
    // Tiles the last frame drew, over every layer.
    [[nodiscard]] std::size_t tiles_drawn() const noexcept { return tiles_drawn_; }
    // Particles the last frame drew, over every CPUParticles2D.
    [[nodiscard]] std::size_t particles_drawn() const noexcept { return particles_drawn_; }
    // Lights the last frame shaded with, and how many of them cast shadows.
    [[nodiscard]] std::size_t lights_drawn() const noexcept { return lights_drawn_; }
    [[nodiscard]] std::size_t shadows_drawn() const noexcept { return shadows_drawn_; }
    // Controls the last frame drew (ui.hpp), the glyph images of their text, outlines and
    // shadows included, and their style boxes and textures.
    [[nodiscard]] std::size_t controls_drawn() const noexcept { return controls_drawn_; }
    [[nodiscard]] std::size_t glyphs_drawn() const noexcept { return glyphs_drawn_; }
    [[nodiscard]] std::size_t boxes_drawn() const noexcept { return boxes_drawn_; }
    // Collision shapes the last frame drew.
    [[nodiscard]] std::size_t shapes_drawn() const noexcept { return shapes_drawn_; }

private:
    struct Texture {
        asset::Handle<render::Image> image;
        const render::Image* builtin = nullptr; // a "builtin:" icon (ui.hpp) instead of a file
        render::TextureId id = render::TextureId::None;
        std::uint64_t version = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;

        [[nodiscard]] const render::Image& pixels() const {
            return builtin != nullptr ? *builtin : *image;
        }
        [[nodiscard]] std::uint64_t image_version() const {
            return builtin != nullptr ? 1 : image.version();
        }
    };

    // The GPU texture for a path and filter, made or updated from the loaded image.
    Result<const Texture*> texture(const std::string& path, render::Filter filter);
    // The material a sprite draws with, its params and the time filled in, and its light mode.
    Result<render::Material> material(const world::Sprite2D& sprite, render::Filter filter,
                                      double time, render::LightMode* light_mode);
    // A layer's cells, decoded again only when the layer or its tile set changes.
    struct LayerCells {
        std::uint64_t set_version = 0;
        world::TileMapLayer layer;
        Cells cells;
        std::string error; // why the cells don't decode, or ""
        bool seen = false;
    };

    // The tile set at a path.
    Result<const TileSet*> tile_set(const std::string& path);
    // A light texture's image.
    Result<const render::Image*> light_image(const std::string& path);
    // The frame's lights, with the atlas and shadow map uploaded; none when the world is unlit.
    render::Lighting lighting(const world::World& world, const TransformOf& transform_of);
    // The GPU texture of a glyph atlas page, uploaded again when the page changed.
    Result<render::TextureId> atlas_page(const text::Fonts& fonts, std::uint32_t page);
    const LayerCells& cells(entt::entity entity, const world::TileMapLayer& layer,
                            const TileSet& set, std::uint64_t set_version);
    void warn_once(const std::string& key, std::string_view message, std::string_view what,
                   std::string_view event = "a sprite can't draw");

    render::RenderDevice* device_;
    asset::Assets* assets_;
    std::map<std::pair<std::string, render::Filter>, Texture> textures_;
    std::map<std::string, asset::Handle<CanvasMaterial>, std::less<>> materials_;
    std::map<std::string, asset::Handle<TileSet>, std::less<>> tile_sets_;
    std::unordered_map<entt::entity, LayerCells> layer_cells_;
    std::map<std::string, asset::Handle<render::Image>, std::less<>> light_images_;
    ShadowCache shadow_cache_;
    // The light atlas, and the textures and versions it was packed from.
    render::TextureId atlas_ = render::TextureId::None;
    std::vector<std::pair<std::string, std::uint64_t>> atlas_key_;
    std::map<std::string, std::array<float, 4>, std::less<>> atlas_rects_;
    render::TextureId shadow_map_ = render::TextureId::None;
    std::uint32_t shadow_rows_ = 0;
    std::vector<std::uint8_t> shadow_pixels_;
    bool lit_ = false;
    std::unordered_map<entt::entity, world::Transform2D> previous_;
    world::CameraView previous_view_; // the camera's view before the last tick
    std::set<std::string, std::less<>> warned_;
    std::size_t sprites_drawn_ = 0;
    std::size_t tiles_drawn_ = 0;
    std::size_t particles_drawn_ = 0;
    std::size_t lights_drawn_ = 0;
    std::size_t shadows_drawn_ = 0;
    std::size_t controls_drawn_ = 0;
    std::size_t glyphs_drawn_ = 0;
    std::size_t boxes_drawn_ = 0;
    std::size_t shapes_drawn_ = 0;
    // Fonts for worlds ui wasn't installed in.
    std::unique_ptr<text::Fonts> fonts_;
    // The glyph atlas pages on the GPU, the fonts they came from and the versions uploaded.
    const text::Fonts* pages_of_ = nullptr;
    std::vector<std::pair<render::TextureId, std::uint64_t>> pages_;
};

// How a project's frames look, from project.json: what capture draws with.
struct Settings {
    // rendering.clear_color.
    render::Color clear_color = render::kDefaultClearColor;
    // rendering.texture_filter, for sprites whose texture_filter is "inherit".
    render::Filter texture_filter = render::Filter::Linear;
    // Seconds per tick (1 / tick_hz), for the time materials read.
    double tick_seconds = 1.0 / 60.0;
};
void set_settings(world::World& world, const Settings& settings);
// The settings given to the world, or the defaults.
[[nodiscard]] Settings settings(const world::World& world);

struct CaptureOptions {
    // Image pixels per world pixel: 0.5 halves the width and height.
    float scale = 1.0F;
    // Seconds of game time materials read; below 0, the world's tick times the settings'
    // tick_seconds.
    double time = -1.0;
    // Draw the collision shapes too, as CanvasOptions::collisions.
    bool collisions = false;
};

struct Capture {
    render::Image image;
    // What CanvasRenderer drew.
    std::size_t sprites = 0;
    std::size_t tiles = 0;
    std::size_t particles = 0;
    std::size_t lights = 0;
    std::size_t controls = 0;
    std::size_t shapes = 0;
};

// Draws the world as a window of ui::settings' viewport size would show it, with Settings'
// clear color and filter, into an image that size times the scale. It draws on the software
// device (render::create_software_device), so it needs no GPU and draws the same on every
// machine. The device and the textures it made stay in the world for the next capture, and are
// made again for other assets.
[[nodiscard]] Result<Capture> capture(world::World& world, asset::Assets& assets,
                                      const CaptureOptions& options = {});

// ---- Frame summaries ----

// A thing shows at a pixel when at least this share of the pixel's color comes from it: its
// alpha there times what the things drawn over it let through.
inline constexpr float kShownShare = 0.25F;

struct SummaryOptions {
    // The part of the window to summarize, [x, y, width, height] in its pixels; none for all
    // of it.
    std::optional<std::array<float, 4>> region;
    // The map's columns and rows; 0 for no map.
    int columns = 0;
    int rows = 0;
};

// Something a frame shows.
struct Shown {
    std::size_t item = 0; // in FrameSummary::contents.items
    // Its bounds in window pixels, [x, y, width, height], not cut to the window: a control's
    // rect as ui::layout places it, else the bounds of what it drew.
    std::array<float, 4> rect{};
    // The share of its pixels that show: inside the window (or region) and not under anything
    // drawn after it.
    float shown = 1.0F;
};

// What a frame shows, as data instead of an image.
struct FrameSummary {
    FrameContents contents;
    std::vector<Shown> shown;           // what shows, in drawing order
    std::vector<std::size_t> offscreen; // items drawn only outside the window
    std::vector<std::size_t> covered;   // items in it but under what was drawn after them
    std::vector<std::size_t> clear;     // items with no pixel that shows: all transparent
    std::vector<std::size_t>
        clipped; // controls a clip cut away, as a ScrollContainer scrolled past
    std::vector<std::vector<std::ptrdiff_t>> map; // rows of cells: the place in shown of what
                                                  // shows at each cell's center, or -1
};

// Summarizes the frame a window of ui::settings' viewport size would show: what CanvasRenderer
// draws there, with how much of each thing shows. A thing's pixels are those where its alpha
// (its texture's times its color's) is kShownShare or more, and it shows at those where its
// share of the color is too, as blending mixes it; materials and lights aren't run, so a
// material that makes its sprite clear still counts. Things are sampled at pixel centers: the
// part in the window (or region) at every pixel up to 64 pixels across and down and at evenly
// spaced pixels beyond, and all of it the same way for the share outside. The map
// shows the last thing drawn that shows at each cell's center. With a region, things that show
// nothing in it are left out, and offscreen stays empty. Like capture, it keeps what it needs in
// the world for the next summary.
[[nodiscard]] Result<FrameSummary> summarize(world::World& world, asset::Assets& assets,
                                             const SummaryOptions& options = {});

} // namespace slash::canvas
