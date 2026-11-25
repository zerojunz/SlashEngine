#pragma once

#include "slash/audio/audio.hpp"
#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/result.hpp"
#include "slash/core/text.hpp"
#include "slash/input/input.hpp"
#include "slash/l10n/l10n.hpp"
#include "slash/navigation/navigation.hpp"
#include "slash/npc/npc.hpp"
#include "slash/physics2d/physics2d.hpp"
#include "slash/physics3d/physics3d.hpp"
#include "slash/render/mesh.hpp"
#include "slash/render/upscaler.hpp"
#include "slash/scene3d/cluster_streaming.hpp"
#include "slash/services/services.hpp"
#include "slash/world/scene.hpp"
#include "slash/xr/xr.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// A game project is a folder with a project.json at its root:
//
//   {
//     "format": 1,
//     "main_scene": "scenes/main.scene.json",
//     "name": "Pong"
//   }
//
// Optional fields: "id" (reverse-DNS identifier for bundles and stores), "version"
// ("major.minor.patch"), "window" ({"width", "height", "vsync",
// "linux_display_driver"}), "tick_hz", "max_fps",
// "rendering" ({"clear_color", "driver", "renderer", "texture_filter", "interpolation",
// "scaling", "lights_and_shadows", "camera", "textures"}),
// "physics_2d",
// "gui" ({"theme"}), "input" (actions by name), "input_devices" ({"pointing"}), "audio"
// ({"bus_layout", "mix_rate", "output_latency", "panning_strength_2d"}), "services" (ad
// placements, products, analytics and consent), "gameplay" ({"sources"}, the game's own C++
// code), "plugins" (.plugin.json files), "sdks" (folders of SDK packages) and "threading"
// ({"max_threads"}).
namespace slash::runtime {

inline constexpr std::string_view kProjectFile = "project.json";
inline constexpr std::int64_t kProjectFormat = 1;

// How exports ship textures: "lossless" as the file is, "vram_compressed" as BC7 or ASTC 4x4 blocks
// for each platform's GPUs, or "vram_uncompressed" as RGBA8 texels ready for the GPU.
struct TextureImport {
    std::string compress_mode = "lossless";
    // Modes for folders ("ui/") and files, the longest that holds a texture winning.
    std::map<std::string, std::string, std::less<>> paths;

    // The mode for a texture's project path.
    [[nodiscard]] std::string_view mode_for(std::string_view path) const;
};

struct Project {
    std::string name;
    std::string id; // empty means com.example.<binary name, lowercased>
    std::string version = "0.1.0";
    std::string main_scene;
    // A square image every platform's icons are made from; empty for the engine's own (icon.hpp).
    std::string icon;
    int window_width = 1280;
    int window_height = 720;
    // "disabled", "enabled", "adaptive" or "mailbox".
    std::string vsync_mode = "enabled";
    // The display server Linux games try first, "x11" or "wayland", falling back to the other.
    std::string linux_display_driver = "x11";
    // rendering.lightmaps.probe_capture.update_speed: how fast moving meshes' light from light
    // probes follows them, a share a second.
    double probe_update_speed = 15.0;
    double tick_hz = 60.0;
    // Frames per second at most; 0 means no cap.
    int max_fps = 0;
    // rendering.lod.threshold_pixels: how many pixels a coarser LOD may be off by, in pixels.
    float mesh_lod_threshold = 1.0F;
    // rendering.virtual_geometry: how virtualized surfaces' cluster pages stream.
    scene3d::ClusterStreamingSettings cluster_streaming{};
    // rendering.occlusion_culling: whether OccluderInstance3Ds hide meshes from the camera, as
    // rendering.occlusion_culling.
    render::OcclusionCulling occlusion_culling{};
    // rendering.ray_tracing: the ray-traced effects devices that trace rays draw.
    render::RayTracing3D ray_tracing{};
    // What the window shows behind everything, as RGBA from 0 to 1: a default gray.
    std::array<float, 4> clear_color{0.3F, 0.3F, 0.3F, 1.0F};
    // How sprites that inherit their filter are sampled: "linear" or "nearest".
    std::string texture_filter = "linear";
    // The GPU backend: "auto", "vulkan", "metal", "d3d12", "opengl", "opengl_es", "webgpu" or
    // "null" (draws nothing).
    std::string rendering_driver = "auto";
    // Draw sprites between fixed ticks, interpolated.
    bool interpolation = true;
    // Draw with OpenGL where the GPU backend doesn't work.
    bool fallback_to_opengl = true;
    // rendering.voxel_lighting.quality "high": VoxelGIs trace six narrower cones for ambient light
    // in place of four wide ones.
    bool voxel_gi_high_quality = false;
    // rendering.field_lighting: SDFGI's probe rays and how fast it follows changes.
    render::SdfgiSettings sdfgi{};
    // rendering.anti_aliasing.taa: temporal antialiasing of the 3D world in the clustered renderer.
    bool use_taa = false;
    // rendering.frame_generation: a frame made between each two drawn ones, as AMD's FSR 3 frame
    // generation (canvas::CanvasOptions::generate).
    bool frame_generation = false;
    // rendering.textures.decals.filter: how decals' textures are filtered.
    render::DecalFilter decal_filter = render::DecalFilter::LinearMipmaps;
    // The render scale and how the world scales to the screen.
    render::Scaling scaling;
    // How 3D meshes are lit: the clustered renderer's clustered lights, or the compatibility
    // renderer's first 64, as the OpenGL devices always draw.
    render::RenderMethod rendering_method = render::RenderMethod::ForwardPlus;
    // The 3D shadow atlases' sizes and filters.
    render::ShadowSettings shadows;
    // Depth of field's blur.
    render::DepthOfFieldSettings depth_of_field{};
    // Screen-space ambient occlusion's quality.
    render::SsaoSettings ssao{};
    // Screen-space reflections' resolution.
    render::SsrSettings ssr{};
    // Volumetric fog's froxels.
    render::VolumetricFogSettings volumetric_fog{};
    // Subsurface scattering.
    render::SubsurfaceSettings subsurface{};
    // Reflection probes' cubes.
    render::ReflectionAtlasSettings reflection_atlas{};
    // How exports ship textures.
    TextureImport texture_import;
    // Gravity and default damping for 2D physics.
    physics2d::Settings physics_2d;
    // Gravity and default damping for 3D physics.
    physics3d::Settings physics_3d;
    // The 2D and 3D navigation maps' cell sizes and how regions join.
    navigation::Settings navigation;
    // The npc clock, which NPC brains' hour and day facts follow.
    npc::Settings npc;
    // A .theme.json file every control uses after its own themes; empty for none.
    std::string gui_theme;
    // Seconds the mouse rests on a control before its tooltip shows.
    double tooltip_delay_sec = 0.5;
    // ui.text_edit_merge_seconds and button_shortcut_feedback_highlight_time (ui::Settings).
    double text_edit_idle_detect_sec = 3.0;
    double button_shortcut_feedback_highlight_time = 0.2;
    std::string accessibility_support = "auto"; // ui.accessibility_support
    // Where a tooltip shows from the pointer, in pixels.
    Vec2 tooltip_position_offset{10.0F, 10.0F};
    // Pixels the mouse moves with the left button held before a drag starts.
    int drag_threshold = 10;
    // ui.text_edit_undo_limit and incremental_search_max_interval_msec (ui::Settings).
    int text_edit_undo_stack_max_size = 1024;
    int incremental_search_max_interval_msec = 2000;
    // Text may use fonts installed on the machine for characters the project's fonts lack.
    bool allow_system_fallback = true;
    // Input actions and their bindings: the ui_* actions, then the project's.
    input::Map input = input::Map::with_defaults();
    // How touches and the mouse stand in for each other.
    input::Settings input_devices;
    // The bus layout, mix rate, output latency and 2D panning strength.
    audio::Settings audio;
    // Ad placements, products, analytics and the player's consent until the game sets it.
    services::Settings services;
    // Translation files, the fallback and test locales and pseudo-localization.
    l10n::Settings internationalization;
    xr::Settings xr; // XR settings
    // Folders and .cpp files of the game's own C++ code (slash/world/game_module.hpp); empty if
    // it has none.
    std::vector<std::string> gameplay_sources;
    // Plugin files (slash/plugin/plugins.hpp): libraries built against slash_plugin.h that add
    // components and systems.
    std::vector<std::string> plugins;
    // Folders of SDK packages (tools/exporter's slash/exporter/sdk.hpp), which exports link.
    std::vector<std::string> sdks;
    // The job system's worker threads (slash/core/jobs.hpp): -1 for one per CPU core.
    int max_threads = -1;
    // rendering.reflections.sky_samples: the samples the sky's radiance and reflection probes are
    // blurred with.
    std::uint32_t ggx_samples = 32;

    // The identifier to use: id, or the default derived from the name.
    [[nodiscard]] std::string bundle_id() const;
    // The name with only ASCII letters and digits kept, for executables and bundles, e.g.
    // "Space Pong 2" -> "SpacePong2". "Game" if nothing is left.
    [[nodiscard]] std::string binary_name() const;
};

// The JSON Schema for project.json.
[[nodiscard]] json::Value project_schema();

// Checks a parsed project.json against the schema and the rules it can't express (a relative
// main scene path, the id and version formats). Returns every error.
[[nodiscard]] std::vector<json::ValidationError> validate_project(const json::Value& given);

// Reads a validated project. The error names the first problem with its JSON Pointer.
[[nodiscard]] Result<Project> parse_project(const json::Value& given);

// Reads and parses <folder>/project.json.
[[nodiscard]] Result<Project> load_project(std::string_view folder);

// A new project with this name and main scene, written in canonical form.
[[nodiscard]] json::Value new_project(std::string_view name, std::string_view main_scene);

// Whether path names a file inside a project (see slash/core/text.hpp).
using slash::is_project_path;

// Joins a folder and a project-relative path with '/'.
[[nodiscard]] std::string join_path(std::string_view folder, std::string_view relative);

// Reads files by project path (is_project_path) from a folder, for World::set_file_reader.
[[nodiscard]] world::FileReader folder_reader(std::string folder);

} // namespace slash::runtime
