#pragma once

#include "slash/asset/assets.hpp"
#include "slash/canvas/debug_draw.hpp"
#include "slash/canvas/gpu_particles.hpp"
#include "slash/canvas/light.hpp"
#include "slash/canvas/material.hpp"
#include "slash/canvas/tilemap.hpp"
#include "slash/core/result.hpp"
#include "slash/render/device.hpp"
#include "slash/render/upscaler.hpp"
#include "slash/scene3d/lightmap.hpp"
#include "slash/scene3d/scene3d.hpp"
#include "slash/scene3d/voxel_gi.hpp"
#include "slash/text/text.hpp"
#include "slash/ui/ui.hpp"
#include "slash/world/camera2d.hpp"
#include "slash/world/canvas_layer.hpp"
#include "slash/world/components.hpp"
#include "slash/world/tree2d.hpp"
#include "slash/world/world.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// 2D drawing, as Godot's canvas: Sprite2D, AnimatedSprite2D, TileMapLayer, CPUParticles2D and
// GPUParticles2D
// entities drawn through a render device, in world pixels with y down, seen through the current
// Camera2D (camera2d.hpp) and shaded by 2D lights (light.hpp).
namespace slash::canvas {

// Asset types: "texture" (.png, .bmp and .ktx2 files as render::Image), "sprite_frames"
// (.frames.json files as SpriteFrames), "material" (.material.json files as CanvasMaterial) and
// "tile_set"
// (.tileset.json files as TileSet).
void add_asset_types(asset::Assets& assets);

// Files as an exported game reads them. An export ships a compressed texture as one KTX2 file a
// format beside where it was (render::texture_form_path: "a.png" as "a.png.bc7.ktx2"), so a
// texture that isn't there as it was is read from its form in a format the device samples
// (samples), else its RGBA8 form, else any (decoded when it loads).
[[nodiscard]] asset::FileSource
with_texture_forms(const asset::FileSource& files,
                   std::function<bool(render::TextureFormat)> samples);

// The "animated_sprite_2d" system, which plays each AnimatedSprite2D on its Sprite2D, the
// "camera_2d" and "parallax_2d" systems (camera2d.hpp, canvas_layer.hpp), for a screen of
// ui::settings' viewport size, and the "cpu_particles_2d" system (particles.hpp), and gives 2D
// physics the shapes of TileMapLayer cells (physics2d::set_tile_shape_source). The first and the
// last read files through assets, which must outlive the world.
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
// Godot's CanvasItem: their z_index added to its own, hidden with them, and tinted by their
// modulate. As Godot's, the sum of z_index stops at a parent whose z_as_relative is false, and a
// top_level entity takes neither z_index nor tint from the parents it is placed above, though
// they still hide it.
struct Inherited {
    int z_index = 0;
    bool visible = true;
    Vec4 modulate{1.0F, 1.0F, 1.0F, 1.0F};
};
[[nodiscard]] Inherited inherited(const world::World& world, const world::Tree2D& tree,
                                  entt::entity entity);
// An entity's z_index with its parents' when relative (Godot's z_as_relative), held to Godot's
// -4096 to 4096.
[[nodiscard]] int z_index(int own, bool relative, const Inherited& from_parents);

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
    // The project path of the texture it samples; empty for glyphs, style boxes, flat colors and
    // built-in icons.
    std::string_view texture;
};

// Something a frame drew: a sprite, a tile map layer, a CPUParticles2D's or GPUParticles2D's
// particles, or a control.
// A control's pieces are cut to its clip, and what it draws over its children (a
// ScrollContainer's scroll bars) draws after them.
struct FrameItem {
    // Popup is an open popup menu, over every control: its entity is the PopupMenu, OptionButton
    // or MenuButton that shows it. Tooltip is the tooltip shown, over those: its entity is the
    // control (or popup menu) whose tooltip it is.
    enum class Kind : std::uint8_t {
        Sprite,
        Tiles,
        Particles, // a CPUParticles2D's
        GpuParticles,
        Control,
        Popup,
        Tooltip
    };
    Kind kind = Kind::Sprite;
    entt::entity entity = entt::null;
    std::size_t count = 0; // tiles or particles drawn
    // Its place in FrameContents::canvases; canvases.size() for a popup menu, the tooltip or a
    // drag's preview, over every canvas.
    std::size_t canvas = 0;
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
    ui::Layout layout;                   // the controls as laid out
    world::CameraView camera;            // what the world was seen through
    std::vector<world::Canvas> canvases; // the canvases drawn, in order (world::Canvases)
};

struct CanvasOptions {
    render::TextureId target = render::TextureId::None; // None draws to the window
    render::Color clear = render::kDefaultClearColor;
    // The viewport's pixels across the target, which the camera fills; 0 uses the target's own
    // pixels. A window on a high-density display has more pixels than its size, so games pass
    // the window's size.
    float width = 0.0F;
    float height = 0.0F;
    // The target's own pixels, which the settings' render scale scales and the upscaler fills;
    // 0 takes width and height. The world draws at full size when neither is known.
    std::uint32_t pixel_width = 0;
    std::uint32_t pixel_height = 0;
    // How far from the transforms before the last tick to the current ones; see
    // CanvasRenderer::before_tick. 1 draws the current ones.
    double interpolation = 1.0;
    // The filter for sprites whose texture_filter is "inherit".
    render::Filter texture_filter = render::Filter::Linear;
    // Seconds of game time, which materials read as time and animated tiles play by. Callers
    // pass the moment the frame shows, between ticks too; see material_time.
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

// A point on the screen in a canvas's pixels, as Godot's get_canvas_transform().affine_inverse():
// in the world's, through the current camera, or in those of the CanvasLayer the entity in
// draws in, for a screen of ui::settings' viewport size (world::screen_transform). Godot's
// get_global_mouse_position is to_world(world, mouse, entity).
[[nodiscard]] Vec2 to_world(const world::World& world, Vec2 point, entt::entity in = entt::null);
// The other way: a point of the canvas the entity in draws in (the world's when null) on the
// screen.
[[nodiscard]] Vec2 to_screen(const world::World& world, Vec2 point, entt::entity in = entt::null);

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

    // Draws one frame: the target cleared, the 3D world drawn through its current Camera3D
    // (scene3d.hpp) when it has one, then each canvas in order (world::Canvases), then
    // the popup menus, tooltip and drag preview, then the overlay's controls. The world's
    // canvas is every visible sprite, tile map layer and particles not in a CanvasLayer, in
    // z_index order, then the order entities were made, seen through the current Camera2D,
    // then the collision shapes if the options ask, then the controls not in a CanvasLayer
    // (ui.hpp), on the screen. A CanvasLayer's is its own sprites, layers and particles, then
    // its controls, all placed by its transform. Controls are laid out in a viewport of the
    // options' width and height, or ui::settings' when they are 0. The children of a
    // Parallax2D with a repeat_size draw again at each repeat, as Godot repeats them. A layer
    // draws its
    // cells row by row, each tile's region centered on its cell, moved by the entity's
    // Transform2D if it has one. Particles draw as their emitter's draw_order says, each its
    // texture's frame (or a white pixel) centered on it, turned, scaled and colored; a
    // GPUParticles2D's system on the device first runs the steps it hasn't (gpu_particles.hpp). A
    // sprite or tile that can't draw, such as one whose texture or material is missing, or whose
    // material_params name a param the material doesn't have, is left out with one warning in the
    // log. On the null device a window frame checks and counts them but draws nothing.
    //
    // When the world has a light or a CanvasModulate, every sprite and layer of the world's
    // canvas is shaded by them, as its material's light_mode says (normal by default), with its
    // normal and specular textures; those in CanvasLayers are unshaded, as Godot's lights shine
    // on layer 0 unless told otherwise. A light that can't shine, such as one whose texture is
    // missing, is left out with one warning.
    [[nodiscard]] Result<void> draw(const world::World& world, const CanvasOptions& options);

    // Sprites the last frame drew.
    [[nodiscard]] std::size_t sprites_drawn() const noexcept { return sprites_drawn_; }
    // Tiles the last frame drew, over every layer.
    [[nodiscard]] std::size_t tiles_drawn() const noexcept { return tiles_drawn_; }
    // Particles the last frame drew: every CPUParticles2D's live ones, and each GPUParticles2D's
    // amount (the device draws each one, those not live with no area).
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
    // 3D meshes the last frame drew, the 3D lights they were lit by, and the meshes kept on the
    // device (meshes of equal settings share one).
    [[nodiscard]] std::size_t meshes_drawn() const noexcept { return meshes_drawn_; }
    // Of those, the ones drawn at a coarser LOD than their surface's own.
    [[nodiscard]] std::size_t lods_drawn() const noexcept { return lods_drawn_; }
    // 3D meshes the OccluderInstance3Ds hid from the camera in the last frame.
    [[nodiscard]] std::size_t meshes_occluded() const noexcept { return meshes_occluded_; }
    [[nodiscard]] std::size_t lights_3d_drawn() const noexcept { return lights_3d_drawn_; }
    [[nodiscard]] std::size_t meshes_kept() const noexcept { return scene3d_.meshes_kept(); }
    // The upscaler the last frame scaled the world with (render/upscaler.hpp), and its sizes;
    // an empty name when the world drew at full size.
    struct Scaled {
        std::string upscaler;
        render::UpscaleSize size;
    };
    [[nodiscard]] const Scaled& scaled() const noexcept { return scaled_; }

private:
    struct Texture {
        asset::Handle<render::Image> image;
        const render::Image* builtin = nullptr; // a "builtin:" icon (ui.hpp) instead of a file
        render::TextureId id = render::TextureId::None;
        std::uint64_t version = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        render::TextureFormat format = render::TextureFormat::Rgba8; // as made on the device

        [[nodiscard]] const render::Image& pixels() const {
            return builtin != nullptr ? *builtin : *image;
        }
        [[nodiscard]] std::uint64_t image_version() const {
            return builtin != nullptr ? 1 : image.version();
        }
    };

    // A material, with its shader made on the device when it has one.
    struct MaterialEntry {
        asset::Handle<CanvasMaterial> material;
        asset::Handle<std::string> source; // a shader material's .hlsl file, unless compiled
        std::uint64_t material_version = 0;
        std::uint64_t source_version = 0;
        std::shared_ptr<const render::CompiledShader> compiled;
        render::ShaderId shader = render::ShaderId::None;
        std::string error; // why its shader can't draw, or ""
    };

    // The GPU texture for a path and filter, made or updated from the loaded image.
    Result<const Texture*> texture(const std::string& path, render::Filter filter);
    // The material at a path; its shader is compiled and made again when it or its file changes.
    Result<const MaterialEntry*> material_entry(const std::string& path);
    // The material a sprite draws with, its params and the time filled in, and its light mode.
    Result<render::Material> material(const world::Sprite2D& sprite, render::Filter filter,
                                      double time, render::LightMode* light_mode);
    // A GPUParticles2D's process material.
    Result<const render::ParticleProcess*> process_material(const std::string& path);
    // A GPUParticles2D's particle system on the device, and the steps run on it; and the same
    // on the CPU for FrameContents, run only when a frame asks for them.
    struct GpuSystem {
        render::ParticlesId id = render::ParticlesId::None;
        std::uint64_t serial = 0;
        std::uint32_t amount = 0;
        std::uint32_t trail = 1;
        std::uint64_t next = 0; // the next step to run
        std::vector<render::Particle> cpu;
        std::uint64_t cpu_serial = 0;
        std::uint64_t cpu_next = 0;
        bool seen = false;
    };
    // Steps to run on a GPUParticles2D's system before it draws: the first one, made to clear
    // the system for a system made anew, then the rest, which start with history steps it ran
    // already, for trails' copies to run behind. A chain of sub-emitters' root's job runs with
    // the next links - 1 jobs, its sub-emitters'.
    struct ParticleJob {
        render::ParticlesId particles = render::ParticlesId::None;
        const render::ParticleProcess* process = nullptr;
        std::optional<render::ParticleStep> first;
        std::span<const render::ParticleStep> rest;
        std::size_t history = 0;
        std::span<const render::ParticleShape> shapes; // that the steps name
        std::size_t links = 1;
    };
    // The steps a GPUParticles2D's system hasn't run, none if it has run them all. The system is
    // made again when the emitter started over or this renderer missed steps no longer kept;
    // then it runs every step kept, the first clearing it. Fails if it can't be made.
    Result<std::optional<ParticleJob>> particle_job(GpuSystem& system, const ParticleSteps& kept,
                                                    const render::ParticleProcess* process);
    // The jobs of a chain of sub-emitters (particle_chain), each with its process material, as
    // particle_job makes one emitter's: the systems are all made again if any is. Fails if one
    // can't be made.
    Result<void> chain_jobs(std::span<const ParticleChainMember> chain,
                            std::span<const render::ParticleProcess* const> processes,
                            std::vector<ParticleJob>& jobs);
    // Brings a system's CPU copy up to its steps kept, as the device runs them, and returns how
    // many of its particles are live.
    static std::size_t mirror(GpuSystem& system, const ParticleSteps& kept,
                              const render::ParticleProcess& process);
    // Brings a chain's CPU copies up to their steps kept, as mirror does one system's.
    void mirror_chain(std::span<const ParticleChainMember> chain,
                      std::span<const render::ParticleProcess* const> processes);
    // Runs the steps, before the systems draw. If they can't run, the systems are made again
    // next frame.
    Result<void> run_particle_jobs(std::span<const ParticleJob> jobs);
    // What draw gathered for a frame (src/renderer.cpp), and drawing it on the device: the
    // world, scaled by the settings' render scale if they ask, then the controls and overlay.
    struct Submission;
    Result<void> submit(const Submission& submission);
    // Destroys the systems of emitters this frame didn't see.
    void drop_unseen_gpu_systems();
    // A layer's cells, decoded again only when the layer or its tile set changes.
    struct LayerCells {
        std::uint64_t set_version = 0;
        world::TileMapLayer layer;
        Cells cells;
        std::vector<const Cells::value_type*> order; // the cells as they draw (draw_order)
        std::string error;                           // why the cells don't decode, or ""
        bool seen = false;
    };

    // The tile set at a path.
    Result<const TileSet*> tile_set(const std::string& path);
    // A light texture's image.
    Result<const render::Image*> light_image(const std::string& path);
    // The frame's lights, with the atlas and shadow map uploaded; none when no canvas is lit.
    render::Lighting lighting(const world::World& world, const TransformOf& transform_of,
                              const world::Canvases& canvases);
    // The GPU texture of a glyph atlas page, uploaded again when the page changed.
    Result<render::TextureId> atlas_page(const text::Fonts& fonts, std::uint32_t page);
    const LayerCells& cells(entt::entity entity, const world::TileMapLayer& layer,
                            const TileSet& set, std::uint64_t set_version);
    void warn_once(const std::string& key, std::string_view message, std::string_view what,
                   std::string_view event = "a sprite can't draw");

    render::RenderDevice* device_;
    asset::Assets* assets_;
    std::map<std::pair<std::string, render::Filter>, Texture> textures_;
    std::map<std::string, MaterialEntry, std::less<>> materials_;
    std::map<std::string, asset::Handle<TileSet>, std::less<>> tile_sets_;
    std::map<std::string, asset::Handle<render::ParticleProcess>, std::less<>> processes_;
    std::unordered_map<entt::entity, GpuSystem> gpu_systems_;
    std::unordered_map<entt::entity, LayerCells> layer_cells_;
    // Images read on the CPU: lights' textures, and those of the materials VoxelGIs plot.
    std::map<std::string, asset::Handle<render::Image>, std::less<>> light_images_;
    std::map<std::string, asset::Handle<scene3d::Model>, std::less<>> models_;
    // The lightmaps LightmapGIs name, and their texels' .hdr files.
    struct LightmapEntry {
        asset::Handle<scene3d::LightmapData> data;
        asset::Handle<render::HdrImage> texels;
    };
    std::map<std::string, LightmapEntry, std::less<>> lightmaps_;
    // The cells VoxelGIs name.
    std::map<std::string, asset::Handle<scene3d::VoxelGIData>, std::less<>> voxel_gis_;
    ShadowCache shadow_cache_;
    // The light atlas, and the textures and versions it was packed from.
    render::TextureId atlas_ = render::TextureId::None;
    std::vector<std::pair<std::string, std::uint64_t>> atlas_key_;
    std::map<std::string, std::array<float, 4>, std::less<>> atlas_rects_;
    render::TextureId shadow_map_ = render::TextureId::None;
    std::uint32_t shadow_rows_ = 0;
    std::vector<std::uint8_t> shadow_pixels_;
    // How lights shade each canvas this frame, by its place in world::Canvases::all().
    std::vector<CanvasLighting> canvas_lighting_;
    std::unordered_map<entt::entity, world::Transform2D> previous_;
    // Each Parallax2D's position before the last tick, to tell when its repeat wrapped.
    std::unordered_map<entt::entity, Vec2> previous_parallax_;
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
    std::size_t meshes_drawn_ = 0;
    std::size_t lods_drawn_ = 0;
    std::size_t meshes_occluded_ = 0;
    std::size_t lights_3d_drawn_ = 0;
    // The 3D world's meshes on the device; its textures and models are this renderer's.
    scene3d::Renderer scene3d_;
    // The upscaler the settings choose, made for the device when they first ask for it, and
    // what it is called.
    std::unique_ptr<render::Upscaler> upscaler_;
    std::string upscaler_name_;
    Scaled scaled_;
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
    // rendering.scaling: the size the world draws at and how it scales to the screen.
    render::Scaling scaling;
    // rendering.rendering_method: how 3D meshes are lit.
    render::RenderMethod rendering_method = render::RenderMethod::ForwardPlus;
    // rendering.lights_and_shadows: the 3D shadow atlases.
    render::ShadowSettings shadows;
    // rendering.camera.depth_of_field: depth of field's blur.
    render::DepthOfFieldSettings depth_of_field{};
    // rendering.environment.ssao: screen-space ambient occlusion's quality.
    render::SsaoSettings ssao{};
    // rendering.environment.screen_space_reflection: screen-space reflections' resolution.
    render::SsrSettings ssr{};
    // rendering.environment.volumetric_fog: volumetric fog's froxels.
    render::VolumetricFogSettings volumetric_fog{};
    // rendering.reflections.reflection_atlas: the size and number of reflection probes' cubes.
    render::ReflectionAtlasSettings reflection_atlas{};
    // rendering.global_illumination.voxel_gi.quality "high": VoxelGIs' narrower cones.
    bool voxel_gi_high_quality = false;
    // rendering.anti_aliasing.quality.use_taa: temporal antialiasing of the 3D world.
    bool use_taa = false;
    // rendering.textures.decals.filter: how decals' textures are sampled.
    render::DecalFilter decal_filter = render::DecalFilter::LinearMipmaps;
    // rendering.mesh_lod.lod_change.threshold_pixels: how many pixels a model's coarser LODs may
    // be off by.
    float mesh_lod_threshold = 1.0F;
    // rendering.occlusion_culling: whether OccluderInstance3Ds hide meshes from the camera.
    render::OcclusionCulling occlusion_culling{};
};
void set_settings(world::World& world, const Settings& settings);
// The settings given to the world, or the defaults.
[[nodiscard]] Settings settings(const world::World& world);

struct CaptureOptions {
    // Image pixels per world pixel: 0.5 halves the width and height.
    float scale = 1.0F;
    // Seconds of game time materials and animated tiles read; below 0, the world's tick times the
    // settings' tick_seconds.
    double time = -1.0;
    // Draw the collision shapes too, as CanvasOptions::collisions.
    bool collisions = false;
    // How many times to draw the frame, returning the last: temporal antialiasing builds up over
    // frames, and the captures of a world keep its history between them.
    std::uint32_t frames = 1;
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
    std::size_t meshes = 0;   // 3D meshes (scene3d.hpp)
    std::size_t lods = 0;     // of those, drawn at a coarser LOD
    std::size_t occluded = 0; // 3D meshes occluders hid from the camera
    // How the settings' render scale scaled the world, if they did.
    CanvasRenderer::Scaled scaled;
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
// A thing whose edge has less contrast than this with what is around it is hard to see: as
// WCAG's contrast ratio, from 1 for the same lightness to 21 for black on white.
inline constexpr float kHardToSee = 1.5F;

struct SummaryOptions {
    // The part of the window to summarize, [x, y, width, height] in its pixels; none for all
    // of it.
    std::optional<std::array<float, 4>> region;
    // The map's columns and rows; 0 for no map.
    int columns = 0;
    int rows = 0;
    // Measure how much each thing that shows stands out from what is around it
    // (Shown::contrast), in the frame capture draws, lights and materials included.
    bool contrast = false;
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
    // With SummaryOptions::contrast, the contrast across its edge: WCAG's contrast ratio of the
    // frame's colors just inside and just outside it, the median along the edge in the window;
    // 0 when nothing around it shows in the window. darker says whether it is darker than what
    // is around it along most of the edge.
    float contrast = 0.0F;
    bool darker = false;
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
// nothing in it are left out, and offscreen stays empty. Contrast is measured on the same grid,
// between neighboring samples where one shows the thing on top and the other is outside it.
// Like capture, it keeps what it needs in the world for the next summary.
[[nodiscard]] Result<FrameSummary> summarize(world::World& world, asset::Assets& assets,
                                             const SummaryOptions& options = {});

} // namespace slash::canvas
