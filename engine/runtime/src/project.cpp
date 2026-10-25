#include "slash/runtime/project.hpp"

#include "slash/core/jobs.hpp"
#include "slash/platform/file.hpp"
#include "slash/platform/run.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <iterator>
#include <utility>

namespace slash::runtime {
namespace {

constexpr int kMaxWindowSide = 16384;
constexpr double kMaxTickHz = 1000.0;

json::Value integer_property(std::string_view description, int fallback) {
    json::Value property;
    property["type"] = "integer";
    property["minimum"] = 1;
    property["maximum"] = kMaxWindowSide;
    property["default"] = fallback;
    property["description"] = description;
    return property;
}

json::Value string_property(std::string_view description) {
    json::Value property;
    property["type"] = "string";
    property["description"] = description;
    return property;
}

bool is_ascii_alnum(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

// Letters, digits and hyphens in at least two dot-separated parts, as Apple and Android require.
bool valid_id(std::string_view id) {
    std::size_t parts = 0;
    std::size_t start = 0;
    while (start <= id.size()) {
        std::size_t end = id.find('.', start);
        if (end == std::string_view::npos) {
            end = id.size();
        }
        const std::string_view part = id.substr(start, end - start);
        if (part.empty()) {
            return false;
        }
        for (const char c : part) {
            if (!is_ascii_alnum(c) && c != '-' && c != '_') {
                return false;
            }
        }
        ++parts;
        start = end + 1;
    }
    return parts >= 2;
}

// "major.minor.patch" with each part a number of at most five digits.
bool valid_version(std::string_view version) {
    std::size_t parts = 0;
    std::size_t digits = 0;
    for (const char c : version) {
        if (c == '.') {
            if (digits == 0) {
                return false;
            }
            ++parts;
            digits = 0;
        } else if (c >= '0' && c <= '9' && digits < 5) {
            ++digits;
        } else {
            return false;
        }
    }
    return digits > 0 && parts == 2;
}

// rendering.scaling, as Godot's rendering/scaling_3d settings.
json::Value scaling_schema() {
    const render::Scaling defaults;
    json::Value mode;
    mode["type"] = "string";
    mode["default"] = defaults.mode;
    mode["description"] =
        "How the world scales to the screen: bilinear stretches it, fsr is AMD's FidelityFX Super "
        "Resolution 1, which finds edges and sharpens, fsr2 is FSR 2, which builds the 3D world "
        "from jittered frames in Forward+, and fsr3 is FSR 3.1's upscaler, which does the same "
        "with AMD's later passes.";
    json::Value scale;
    scale["type"] = "number";
    scale["minimum"] = render::kMinRenderScale;
    scale["maximum"] = render::kMaxRenderScale;
    scale["default"] = defaults.scale;
    scale["description"] = "The world's size over the screen's: below 1 draws fewer pixels and "
                           "scales up, above 1 draws more and scales down.";
    json::Value sharpness;
    sharpness["type"] = "number";
    sharpness["minimum"] = 0;
    sharpness["maximum"] = render::kMaxFsrSharpness;
    sharpness["default"] = defaults.fsr_sharpness;
    sharpness["description"] = "FSR's sharpening in stops: 0 is the sharpest, 2 the softest.";
    json::Value upscaler;
    upscaler["type"] = "string";
    upscaler["pattern"] = "^[a-z0-9_]*$";
    upscaler["description"] =
        "An upscaler the game adds from an SDK package (render::add_upscaler), used instead of "
        "mode where it runs.";
    json::Value scaling;
    scaling["type"] = "object";
    scaling["additionalProperties"] = false;
    scaling["description"] = "Draw the world at another size and scale it to the screen under the "
                             "UI, as Godot's scaling_3d settings.";
    scaling["properties"]["mode"] = std::move(mode);
    scaling["properties"]["scale"] = std::move(scale);
    scaling["properties"]["fsr_sharpness"] = std::move(sharpness);
    scaling["properties"]["upscaler"] = std::move(upscaler);
    return scaling;
}

// rendering.camera, as Godot's rendering/camera settings: depth of field's blur.
json::Value camera_schema() {
    const render::DepthOfFieldSettings defaults;
    const auto choice = [](const auto& names, std::size_t default_index, std::string description) {
        json::Array choices;
        for (const std::string_view name : names) {
            choices.emplace_back(std::string(name));
        }
        json::Value out;
        out["enum"] = std::move(choices);
        out["default"] = std::string(names.at(default_index));
        out["description"] = std::move(description);
        return out;
    };
    json::Value jitter;
    jitter["type"] = "boolean";
    jitter["default"] = defaults.use_jitter;
    jitter["description"] = "Moves each pixel's samples by a random part of a step, which turns "
                            "banding into noise.";
    json::Value dof;
    dof["type"] = "object";
    dof["additionalProperties"] = false;
    dof["description"] = "Depth of field's blur, for every camera's CameraAttributesPractical.";
    dof["properties"]["depth_of_field_bokeh_shape"] =
        choice(render::kBokehShapeNames, static_cast<std::size_t>(defaults.bokeh_shape),
               "The blur's shape: box is fastest, then hexagon, then circle.");
    dof["properties"]["depth_of_field_bokeh_quality"] =
        choice(render::kBokehQualityNames, static_cast<std::size_t>(defaults.bokeh_quality),
               "How many samples the blur takes: very_low and low blur at half size.");
    dof["properties"]["depth_of_field_use_jitter"] = std::move(jitter);
    json::Value out;
    out["type"] = "object";
    out["additionalProperties"] = false;
    out["description"] = "3D cameras, as Godot's rendering/camera settings.";
    out["properties"]["depth_of_field"] = std::move(dof);
    return out;
}

// rendering.environment, as Godot's rendering/environment settings: screen-space ambient
// occlusion's quality and screen-space reflections' resolution.
json::Value environment_schema() {
    const render::SsaoSettings defaults;
    const auto number = [](double value, double min, double max, std::string description) {
        json::Value out;
        out["type"] = "number";
        out["minimum"] = min;
        out["maximum"] = max;
        out["default"] = value;
        out["description"] = std::move(description);
        return out;
    };
    json::Array qualities;
    for (const std::string_view name : render::kSsaoQualityNames) {
        qualities.emplace_back(std::string(name));
    }
    json::Value quality;
    quality["enum"] = std::move(qualities);
    quality["default"] =
        std::string(render::kSsaoQualityNames.at(static_cast<std::size_t>(defaults.quality)));
    quality["description"] = "How many samples it takes: very_low 3 in half its slices, low 5, "
                             "medium 12, high 12 with depth mipmaps, ultra up to 32 where they "
                             "help most.";
    json::Value half_size;
    half_size["type"] = "boolean";
    half_size["default"] = defaults.half_size;
    half_size["description"] = "Works at a quarter of the frame's resolution rather than half.";
    json::Value blur_passes;
    blur_passes["type"] = "integer";
    blur_passes["minimum"] = 0;
    blur_passes["maximum"] = 6;
    blur_passes["default"] = defaults.blur_passes;
    blur_passes["description"] = "How many times it is blurred; very_low always blurs once.";
    json::Value ssao;
    ssao["type"] = "object";
    ssao["additionalProperties"] = false;
    ssao["description"] =
        "Screen-space ambient occlusion, for every WorldEnvironment with ssao_enabled.";
    ssao["properties"]["quality"] = std::move(quality);
    ssao["properties"]["half_size"] = std::move(half_size);
    ssao["properties"]["adaptive_target"] =
        number(defaults.adaptive_target, 0, 1, "Ultra: how many of the extra samples it takes.");
    ssao["properties"]["blur_passes"] = std::move(blur_passes);
    ssao["properties"]["fadeout_from"] =
        number(defaults.fadeout_from, 0, 1e6, "Meters from the camera where it starts to fade.");
    ssao["properties"]["fadeout_to"] =
        number(defaults.fadeout_to, 0, 1e6, "Meters from the camera where it is gone.");
    json::Value out;
    out["type"] = "object";
    out["additionalProperties"] = false;
    out["description"] = "3D environments, as Godot's rendering/environment settings.";
    out["properties"]["ssao"] = std::move(ssao);
    json::Value ssr_half_size;
    ssr_half_size["type"] = "boolean";
    ssr_half_size["default"] = render::SsrSettings{}.half_size;
    ssr_half_size["description"] =
        "Traces at half the frame's resolution, then resolves to its size along the depths.";
    json::Value ssr;
    ssr["type"] = "object";
    ssr["additionalProperties"] = false;
    ssr["description"] = "Screen-space reflections, for every WorldEnvironment with ssr_enabled.";
    ssr["properties"]["half_size"] = std::move(ssr_half_size);
    out["properties"]["screen_space_reflection"] = std::move(ssr);
    const render::VolumetricFogSettings fog_defaults;
    const auto froxels = [](std::uint32_t value, std::string_view description) {
        json::Value size;
        size["type"] = "integer";
        size["minimum"] = 16;
        size["maximum"] = 512;
        size["default"] = static_cast<std::int64_t>(value);
        size["description"] = std::string(description);
        return size;
    };
    json::Value use_filter;
    use_filter["type"] = "boolean";
    use_filter["default"] = fog_defaults.use_filter;
    use_filter["description"] = "Blurs the fog across and down, smoothing it.";
    json::Value fog;
    fog["type"] = "object";
    fog["additionalProperties"] = false;
    fog["description"] =
        "Forward+'s volumetric fog, for every WorldEnvironment with volumetric_fog_enabled.";
    fog["properties"]["volume_size"] =
        froxels(fog_defaults.volume_size, "Froxels across and down, for a square frame.");
    fog["properties"]["volume_depth"] =
        froxels(fog_defaults.volume_depth, "Froxels from the camera to the fog's length.");
    fog["properties"]["use_filter"] = std::move(use_filter);
    out["properties"]["volumetric_fog"] = std::move(fog);
    return out;
}

// rendering.reflections, as Godot's rendering/reflections settings.
json::Value reflections_schema() {
    const render::ReflectionAtlasSettings defaults;
    json::Value size;
    size["type"] = "integer";
    size["enum"] = json::Array{32, 64, 128, 256, 512, 1024, 2048};
    size["default"] = static_cast<std::int64_t>(defaults.reflection_size);
    size["description"] = "Texels across each face of a reflection probe's cube.";
    json::Value count;
    count["type"] = "integer";
    count["minimum"] = 1;
    count["maximum"] = 4096;
    count["default"] = static_cast<std::int64_t>(defaults.reflection_count);
    count["description"] = "How many probes a frame reflects at most (42 are drawn at most).";
    json::Value atlas;
    atlas["type"] = "object";
    atlas["additionalProperties"] = false;
    atlas["description"] = "Reflection probes' cubes, as Godot's reflection_atlas.";
    atlas["properties"]["reflection_size"] = std::move(size);
    atlas["properties"]["reflection_count"] = std::move(count);
    json::Value out;
    out["type"] = "object";
    out["additionalProperties"] = false;
    out["description"] = "Reflections, as Godot's rendering/reflections settings.";
    out["properties"]["reflection_atlas"] = std::move(atlas);
    return out;
}

// rendering.global_illumination, as Godot's rendering/global_illumination settings.
json::Value global_illumination_schema() {
    json::Value quality;
    quality["enum"] = json::Array{"low", "high"};
    quality["default"] = "low";
    quality["description"] =
        "How VoxelGIs trace ambient light: four wide cones, or six narrower ones for high.";
    json::Value voxel_gi;
    voxel_gi["type"] = "object";
    voxel_gi["additionalProperties"] = false;
    voxel_gi["properties"]["quality"] = std::move(quality);
    json::Value out;
    out["type"] = "object";
    out["additionalProperties"] = false;
    out["description"] = "Global illumination, as Godot's rendering/global_illumination settings.";
    out["properties"]["voxel_gi"] = std::move(voxel_gi);
    return out;
}

// rendering.anti_aliasing, as Godot's rendering/anti_aliasing settings.
json::Value anti_aliasing_schema() {
    json::Value use_taa;
    use_taa["type"] = "boolean";
    use_taa["default"] = false;
    use_taa["description"] =
        "Temporal antialiasing of the 3D world in Forward+: each frame is drawn with the camera "
        "moved within a pixel and blended with the frames before it.";
    json::Value quality;
    quality["type"] = "object";
    quality["additionalProperties"] = false;
    quality["properties"]["use_taa"] = std::move(use_taa);
    json::Value out;
    out["type"] = "object";
    out["additionalProperties"] = false;
    out["description"] = "Antialiasing, as Godot's rendering/anti_aliasing settings.";
    out["properties"]["quality"] = std::move(quality);
    return out;
}

// rendering.mesh_lod, as Godot's rendering/mesh_lod settings.
json::Value mesh_lod_schema() {
    json::Value threshold;
    threshold["type"] = "number";
    threshold["minimum"] = 0;
    threshold["default"] = 1;
    threshold["description"] =
        "How many pixels a coarser LOD of a model's surface may be off by where it is drawn: "
        "higher draws coarser LODs nearer. 0 draws every mesh in full.";
    json::Value lod_change;
    lod_change["type"] = "object";
    lod_change["additionalProperties"] = false;
    lod_change["properties"]["threshold_pixels"] = std::move(threshold);
    json::Value out;
    out["type"] = "object";
    out["additionalProperties"] = false;
    out["description"] = "Mesh LODs, as Godot's rendering/mesh_lod settings.";
    out["properties"]["lod_change"] = std::move(lod_change);
    return out;
}

// rendering.occlusion_culling, as Godot's rendering/occlusion_culling settings.
json::Value occlusion_culling_schema() {
    json::Value use;
    use["type"] = "boolean";
    use["default"] = false;
    use["description"] = "OccluderInstance3Ds hide the meshes behind them from the camera.";
    json::Value rays;
    rays["type"] = "integer";
    rays["minimum"] = 1;
    rays["maximum"] = 65536;
    rays["default"] = 512;
    rays["description"] =
        "Rays cast into the occluders per thread, for 8 threads: more culls more exactly and "
        "costs more.";
    json::Value jitter;
    jitter["type"] = "boolean";
    jitter["default"] = true;
    jitter["description"] =
        "Moves the rays a little each frame, so thin gaps between occluders show.";
    json::Value out;
    out["type"] = "object";
    out["additionalProperties"] = false;
    out["description"] = "Occlusion culling, as Godot's rendering/occlusion_culling settings.";
    out["properties"]["use_occlusion_culling"] = std::move(use);
    out["properties"]["occlusion_rays_per_thread"] = std::move(rays);
    out["properties"]["jitter_projection"] = std::move(jitter);
    return out;
}

// rendering.lights_and_shadows, as Godot's rendering/lights_and_shadows settings.
json::Value lights_and_shadows_schema() {
    const render::ShadowSettings defaults;
    json::Array qualities;
    for (const std::string_view name : render::kShadowQualityNames) {
        qualities.emplace_back(std::string(name));
    }
    const auto atlas = [&](std::string_view size_key, std::uint32_t size,
                           render::ShadowQuality quality, std::string description) {
        json::Value texels;
        texels["type"] = "integer";
        texels["minimum"] = 256;
        texels["maximum"] = 16384;
        texels["default"] = size;
        texels["description"] =
            "The atlas's width and height in texels, a power of two; devices draw it at most as "
            "large as their largest texture.";
        json::Value filter;
        filter["enum"] = qualities;
        filter["default"] =
            std::string(render::kShadowQualityNames.at(static_cast<std::size_t>(quality)));
        filter["description"] =
            "How shadow edges are filtered: hard takes one sample, the soft ones more.";
        json::Value out;
        out["type"] = "object";
        out["additionalProperties"] = false;
        out["description"] = std::move(description);
        out["properties"][std::string(size_key)] = std::move(texels);
        out["properties"]["soft_shadow_filter_quality"] = std::move(filter);
        return out;
    };
    json::Value out;
    out["type"] = "object";
    out["additionalProperties"] = false;
    out["description"] = "The 3D shadow maps, as Godot's lights_and_shadows settings.";
    out["properties"]["directional_shadow"] =
        atlas("size", defaults.directional_size, defaults.directional_quality,
              "The atlas directional lights' shadow cascades share.");
    out["properties"]["positional_shadow"] =
        atlas("atlas_size", defaults.positional_size, defaults.positional_quality,
              "The atlas omni and spot lights' shadow maps share.");
    return out;
}

} // namespace

std::string Project::binary_name() const {
    std::string binary;
    for (const char c : name) {
        if (is_ascii_alnum(c)) {
            binary += c;
        }
    }
    return binary.empty() ? std::string("Game") : binary;
}

std::string_view TextureImport::mode_for(std::string_view path) const {
    std::string_view mode = compress_mode;
    std::size_t matched = 0;
    for (const auto& [prefix, prefix_mode] : paths) {
        const bool holds = prefix.ends_with('/') ? path.starts_with(prefix) : path == prefix;
        if (holds && prefix.size() > matched) {
            mode = prefix_mode;
            matched = prefix.size();
        }
    }
    return mode;
}

std::string Project::bundle_id() const {
    if (!id.empty()) {
        return id;
    }
    std::string lowered = binary_name();
    for (char& c : lowered) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return "com.example." + lowered;
}

json::Value project_schema() {
    json::Value window;
    window["type"] = "object";
    window["additionalProperties"] = false;
    window["description"] = "The window: its initial size in pixels and how it is shown.";
    window["properties"]["width"] = integer_property("Window width.", Project{}.window_width);
    window["properties"]["height"] = integer_property("Window height.", Project{}.window_height);
    json::Value vsync_mode;
    vsync_mode["enum"] = json::Array{"disabled", "enabled", "adaptive", "mailbox"};
    vsync_mode["default"] = Project{}.vsync_mode;
    vsync_mode["description"] =
        "How frames wait for the display: enabled waits for each refresh; disabled presents at "
        "once and may tear; mailbox shows the newest frame each refresh; adaptive waits unless "
        "a frame is late. Unsupported modes fall back to enabled.";
    window["properties"]["vsync_mode"] = std::move(vsync_mode);
    json::Value linux_display_driver;
    linux_display_driver["enum"] = json::Array{"x11", "wayland"};
    linux_display_driver["default"] = Project{}.linux_display_driver;
    linux_display_driver["description"] =
        "The display server a game tries first on Linux, falling back to the other, as Godot's "
        "driver.linuxbsd: x11 (through XWayland on Wayland desktops) or wayland.";
    window["properties"]["linux_display_driver"] = std::move(linux_display_driver);

    json::Value tick_hz;
    tick_hz["type"] = "number";
    tick_hz["exclusiveMinimum"] = 0;
    tick_hz["maximum"] = kMaxTickHz;
    tick_hz["default"] = Project{}.tick_hz;
    tick_hz["description"] = "Fixed simulation ticks per second.";

    json::Value max_fps;
    max_fps["type"] = "integer";
    max_fps["minimum"] = 0;
    max_fps["maximum"] = platform::kMaxFps;
    max_fps["default"] = Project{}.max_fps;
    max_fps["description"] = "Frames per second at most, e.g. 30, 60, 120 or 240; 0 means no cap.";

    json::Value clear_color;
    clear_color["type"] = "array";
    clear_color["items"]["type"] = "number";
    clear_color["items"]["minimum"] = 0;
    clear_color["items"]["maximum"] = 1;
    clear_color["minItems"] = 4;
    clear_color["maxItems"] = 4;
    clear_color["default"] = json::Array{0.3, 0.3, 0.3, 1.0}; // Project{}.clear_color
    clear_color["description"] =
        "What the window shows behind everything, as [r, g, b, a] from 0 to 1.";
    json::Value driver;
    driver["enum"] =
        json::Array{"auto", "vulkan", "metal", "d3d12", "opengl", "opengl_es", "webgpu", "null"};
    driver["default"] = Project{}.rendering_driver;
    driver["description"] =
        "The GPU backend. auto picks the best one on each machine (on the web, WebGPU where the "
        "browser has it, else WebGL 2); opengl is OpenGL 3.3, or OpenGL ES 3.0 where that is what "
        "the system has; opengl_es is OpenGL ES 3.0; webgpu is the browser's WebGPU, on the web "
        "only; null draws nothing.";
    json::Value fallback;
    fallback["type"] = "boolean";
    fallback["default"] = Project{}.fallback_to_opengl;
    fallback["description"] = "Draw with opengl where the driver's GPU backend (Vulkan, Metal, "
                              "Direct3D 12 or WebGPU) doesn't work, as Godot's "
                              "fallback_to_opengl3.";
    json::Value rendering;
    rendering["type"] = "object";
    rendering["additionalProperties"] = false;
    rendering["description"] = "How the game draws.";
    rendering["properties"]["clear_color"] = std::move(clear_color);
    rendering["properties"]["driver"] = std::move(driver);
    rendering["properties"]["fallback_to_opengl"] = std::move(fallback);
    json::Value method;
    method["enum"] = json::Array{"forward_plus", "gl_compatibility"};
    method["default"] = "forward_plus";
    method["description"] =
        "How 3D meshes are lit, as Godot's rendering_method: forward_plus sorts up to 8 "
        "directional and 512 omni and spot lights into clusters of the screen; gl_compatibility "
        "shades every pixel with the first 64 lights, as the OpenGL drivers always do.";
    rendering["properties"]["rendering_method"] = std::move(method);
    json::Value texture_filter;
    texture_filter["enum"] = json::Array{"linear", "nearest"};
    texture_filter["default"] = Project{}.texture_filter;
    texture_filter["description"] =
        "How sprites whose texture_filter is \"inherit\" are sampled when scaled: linear is "
        "smooth, nearest keeps pixel art sharp.";
    rendering["properties"]["texture_filter"] = std::move(texture_filter);
    json::Value interpolation;
    interpolation["type"] = "boolean";
    interpolation["default"] = Project{}.interpolation;
    interpolation["description"] =
        "Draw sprites part of the way between fixed ticks, so motion looks smooth when the "
        "display refreshes faster than the game ticks.";
    rendering["properties"]["interpolation"] = std::move(interpolation);
    rendering["properties"]["scaling"] = scaling_schema();
    rendering["properties"]["lights_and_shadows"] = lights_and_shadows_schema();
    rendering["properties"]["camera"] = camera_schema();
    rendering["properties"]["environment"] = environment_schema();
    rendering["properties"]["reflections"] = reflections_schema();
    rendering["properties"]["global_illumination"] = global_illumination_schema();
    rendering["properties"]["anti_aliasing"] = anti_aliasing_schema();
    rendering["properties"]["mesh_lod"] = mesh_lod_schema();
    rendering["properties"]["occlusion_culling"] = occlusion_culling_schema();
    json::Value compress_mode;
    compress_mode["enum"] = json::Array{"lossless", "vram_compressed", "vram_uncompressed"};
    compress_mode["description"] =
        "How exports ship textures, as Godot's compress/mode: lossless as the file is; "
        "vram_compressed as BC7 (desktop, web) or ASTC 4x4 (phones, web) blocks, a quarter of the "
        "GPU memory; vram_uncompressed as RGBA8 texels ready for the GPU.";
    json::Value decal_filter;
    decal_filter["enum"] = json::Array{};
    for (const std::string_view filter : render::kDecalFilterNames) {
        decal_filter["enum"].push_back(std::string(filter));
    }
    decal_filter["default"] = "linear_mipmaps";
    decal_filter["description"] =
        "How Decals' textures are sampled, as Godot's rendering/textures/decals/filter: by the "
        "nearest texel or linearly, and with mipmaps, blending smaller copies further away. "
        "Anisotropic filters are drawn as the plain mipmap ones.";
    json::Value decals;
    decals["type"] = "object";
    decals["additionalProperties"] = false;
    decals["properties"]["filter"] = std::move(decal_filter);
    json::Value textures;
    textures["type"] = "object";
    textures["additionalProperties"] = false;
    textures["description"] = "How exports ship textures, and how decals sample theirs.";
    textures["properties"]["decals"] = std::move(decals);
    textures["properties"]["compress_mode"] = compress_mode;
    textures["properties"]["compress_mode"]["default"] = TextureImport{}.compress_mode;
    textures["properties"]["paths"]["type"] = "object";
    textures["properties"]["paths"]["additionalProperties"] = std::move(compress_mode);
    textures["properties"]["paths"]["description"] =
        "Modes for folders (\"ui/\") and files, e.g. {\"ui/\": \"lossless\"}; the longest match "
        "wins.";
    rendering["properties"]["textures"] = std::move(textures);

    json::Value name = string_property("The game's name, shown in its title bar. Exported files "
                                       "use it with only letters and digits kept.");
    name["minLength"] = 1;
    name["maxLength"] = 100;

    json::Value version = string_property("Version as major.minor.patch, e.g. \"1.2.0\".");
    version["default"] = Project{}.version;

    json::Value format;
    format["const"] = kProjectFormat;

    json::Value schema;
    schema["$schema"] = "https://json-schema.org/draft/2020-12/schema";
    schema["title"] = "SlashEngine project";
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["required"] = json::Array{"format", "name", "main_scene"};
    json::Value& properties = schema["properties"];
    properties["format"] = std::move(format);
    properties["name"] = std::move(name);
    properties["id"] = string_property("Reverse-DNS identifier for bundles and app stores, e.g. "
                                       "\"com.example.pong\". Default: com.example.<name>.");
    properties["version"] = std::move(version);
    properties["main_scene"] =
        string_property("The scene loaded at startup, relative to the project folder, e.g. "
                        "\"scenes/main.scene.json\".");
    properties["icon"] = string_property(
        "A square image, 1024x1024 or larger, that every platform's icons are made from and the "
        "window shows, relative to the project folder, as Godot's application/config/icon. "
        "Default: the engine's icon.");
    properties["window"] = std::move(window);
    properties["tick_hz"] = std::move(tick_hz);
    properties["max_fps"] = std::move(max_fps);
    properties["rendering"] = std::move(rendering);
    properties["physics_2d"] = physics2d::settings_schema();
    properties["physics_3d"] = physics3d::settings_schema();
    properties["navigation_2d"] = navigation::settings_schema(navigation::Map::TwoD);
    properties["navigation_3d"] = navigation::settings_schema(navigation::Map::ThreeD);
    json::Value gui;
    gui["type"] = "object";
    gui["additionalProperties"] = false;
    gui["description"] = "UI settings.";
    gui["properties"]["theme"] =
        string_property("A .theme.json file every control uses after its own and its parents' "
                        "themes, before Godot's default theme, e.g. \"ui/game.theme.json\".");
    gui["properties"]["allow_system_fallback"]["type"] = "boolean";
    gui["properties"]["allow_system_fallback"]["default"] = true;
    gui["properties"]["allow_system_fallback"]["description"] =
        "Use fonts installed on the machine for characters the project's fonts lack. Off, text "
        "lays out the same everywhere, as on the web, which has none.";
    gui["properties"]["tooltip_delay_sec"]["type"] = "number";
    gui["properties"]["tooltip_delay_sec"]["minimum"] = 0;
    gui["properties"]["tooltip_delay_sec"]["default"] = 0.5;
    gui["properties"]["tooltip_delay_sec"]["description"] =
        "Seconds the mouse rests on a control before its tooltip_text shows.";
    json::Value offset;
    offset["type"] = "array";
    offset["items"]["type"] = "number";
    offset["minItems"] = 2;
    offset["maxItems"] = 2;
    offset["default"] = json::Array{10, 10};
    offset["description"] = "Where a tooltip shows from the pointer, [x, y] pixels.";
    gui["properties"]["tooltip_position_offset"] = std::move(offset);
    gui["properties"]["drag_threshold"]["type"] = "integer";
    gui["properties"]["drag_threshold"]["minimum"] = 0;
    gui["properties"]["drag_threshold"]["default"] = 10;
    gui["properties"]["drag_threshold"]["description"] =
        "Pixels the mouse moves with the left button held before a drag and drop starts.";
    properties["gui"] = std::move(gui);
    properties["input"] = input::map_schema();
    properties["input_devices"] = input::settings_schema();
    properties["audio"] = audio::settings_schema();
    properties["services"] = services::settings_schema();
    json::Value gameplay;
    gameplay["type"] = "object";
    gameplay["additionalProperties"] = false;
    gameplay["required"] = json::Array{"sources"};
    gameplay["description"] = "The game's own C++ code: its components and systems.";
    json::Value& sources = gameplay["properties"]["sources"];
    sources["type"] = "array";
    sources["minItems"] = 1;
    sources["items"]["type"] = "string";
    sources["description"] =
        "Folders and .cpp files of the code, relative to the project; a folder means every .cpp "
        "file in it and its subfolders. gameplay.build makes the library the tools load, and "
        "exports link the code in.";
    properties["gameplay"] = std::move(gameplay);
    json::Value& sdks = properties["sdks"];
    sdks["type"] = "array";
    sdks["items"]["type"] = "string";
    sdks["description"] =
        "Folders of SDK packages, relative to the project: closed vendor SDKs, each with an "
        "sdk.json (\"slash-cli schema sdk\"), that exports link into the game.";
    properties["threading"] = jobs::settings_schema();
    return schema;
}

std::vector<json::ValidationError> validate_project(const json::Value& project) {
    std::vector<json::ValidationError> errors = json::validate(project, project_schema());
    if (!errors.empty()) {
        return errors;
    }
    if (const json::Value* id = project.find("id"); id != nullptr && !valid_id(id->as_string())) {
        errors.push_back({"/id", "expected a reverse-DNS identifier such as "
                                 "\"com.example.pong\": letters, digits, '-' and '_' in two or "
                                 "more parts separated by dots"});
    }
    if (const json::Value* version = project.find("version");
        version != nullptr && !valid_version(version->as_string())) {
        errors.push_back({"/version", "expected major.minor.patch, e.g. \"1.2.0\""});
    }
    const std::string& main_scene = project.find("main_scene")->as_string();
    if (!is_project_path(main_scene)) {
        errors.push_back({"/main_scene", "expected a path inside the project folder, separated "
                                         "by '/', with no \".\" or \"..\" parts"});
    } else if (!main_scene.ends_with(".scene.json")) {
        errors.push_back({"/main_scene", "expected a .scene.json file"});
    }
    if (const json::Value* icon = project.find("icon");
        icon != nullptr && !icon->as_string().empty() && !is_project_path(icon->as_string())) {
        errors.push_back({"/icon", "expected a path inside the project folder, separated by '/', "
                                   "with no \".\" or \"..\" parts"});
    }
    if (const json::Value* gui = project.find("gui"); gui != nullptr) {
        if (const json::Value* theme = gui->find("theme");
            theme != nullptr && !theme->as_string().empty()) {
            if (!is_project_path(theme->as_string())) {
                errors.push_back({"/gui/theme", "expected a path inside the project folder, "
                                                "separated by '/', with no \".\" or \"..\" parts"});
            } else if (!theme->as_string().ends_with(".theme.json")) {
                errors.push_back({"/gui/theme", "expected a .theme.json file"});
            }
        }
    }
    if (const json::Value* rendering = project.find("rendering"); rendering != nullptr) {
        if (const json::Value* textures = rendering->find("textures"); textures != nullptr) {
            if (const json::Value* paths = textures->find("paths"); paths != nullptr) {
                for (const auto& [path, mode] : paths->as_object()) {
                    const std::string_view folder =
                        path.ends_with('/') ? std::string_view(path).substr(0, path.size() - 1)
                                            : std::string_view(path);
                    if (!is_project_path(folder)) {
                        errors.push_back({"/rendering/textures/paths/" + json::pointer_escape(path),
                                          "expected a folder ending in '/' or a file inside the "
                                          "project, separated by '/', with no \".\" or \"..\" "
                                          "parts"});
                    }
                }
            }
        }
        if (const json::Value* scaling = rendering->find("scaling"); scaling != nullptr) {
            if (const json::Value* mode = scaling->find("mode"); mode != nullptr) {
                if (std::string problem = render::scaling_mode_problem(mode->as_string());
                    !problem.empty()) {
                    errors.push_back({"/rendering/scaling/mode", std::move(problem)});
                }
            }
            // The validator skips "pattern", so the upscaler's name is checked here.
            if (const json::Value* upscaler = scaling->find("upscaler");
                upscaler != nullptr && !std::ranges::all_of(upscaler->as_string(), [](char c) {
                    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
                })) {
                errors.push_back({"/rendering/scaling/upscaler",
                                  "expected lowercase letters, digits and underscores, as "
                                  "render::add_upscaler names upscalers"});
            }
        }
        if (const json::Value* shadows = rendering->find("lights_and_shadows");
            shadows != nullptr) {
            const std::array<std::pair<std::string_view, std::string_view>, 2> atlases{
                {{"directional_shadow", "size"}, {"positional_shadow", "atlas_size"}}};
            for (const auto& [atlas, key] : atlases) {
                const json::Value* settings = shadows->find(atlas);
                const json::Value* size = settings != nullptr ? settings->find(key) : nullptr;
                if (size != nullptr &&
                    !render::valid_shadow_atlas_size(static_cast<std::uint32_t>(size->as_int()))) {
                    errors.push_back({"/rendering/lights_and_shadows/" + std::string(atlas) + "/" +
                                          std::string(key),
                                      "expected a power of two from 256 to 16384"});
                }
            }
        }
    }
    if (const json::Value* gameplay = project.find("gameplay"); gameplay != nullptr) {
        const json::Array& sources = gameplay->find("sources")->as_array();
        for (std::size_t i = 0; i < sources.size(); ++i) {
            if (!is_project_path(sources[i].as_string())) {
                errors.push_back({"/gameplay/sources/" + std::to_string(i),
                                  "expected a path inside the project folder, separated by '/', "
                                  "with no \".\" or \"..\" parts"});
            }
        }
    }
    if (const json::Value* sdks = project.find("sdks"); sdks != nullptr) {
        const json::Array& folders = sdks->as_array();
        for (std::size_t i = 0; i < folders.size(); ++i) {
            if (!is_project_path(folders[i].as_string())) {
                errors.push_back({"/sdks/" + std::to_string(i),
                                  "expected a folder inside the project, separated by '/', with "
                                  "no \".\" or \"..\" parts"});
            }
        }
    }
    if (const json::Value* input = project.find("input"); input != nullptr) {
        auto found = input::validate_map(*input, "/input");
        errors.insert(errors.end(), std::make_move_iterator(found.begin()),
                      std::make_move_iterator(found.end()));
    }
    if (const json::Value* audio = project.find("audio"); audio != nullptr) {
        auto found = audio::validate_settings(*audio, "/audio");
        errors.insert(errors.end(), std::make_move_iterator(found.begin()),
                      std::make_move_iterator(found.end()));
    }
    if (const json::Value* services = project.find("services"); services != nullptr) {
        auto found = services::validate_settings(*services, "/services");
        errors.insert(errors.end(), std::make_move_iterator(found.begin()),
                      std::make_move_iterator(found.end()));
    }
    return errors;
}

Result<Project> parse_project(const json::Value& project) {
    if (const auto errors = validate_project(project); !errors.empty()) {
        const json::ValidationError& first = errors.front();
        return make_error(ErrorCode::InvalidArgument,
                          std::string(kProjectFile) + " " +
                              (first.path.empty() ? std::string("root") : first.path) + ": " +
                              first.message);
    }
    Project out;
    out.name = project.find("name")->as_string();
    out.main_scene = project.find("main_scene")->as_string();
    if (const json::Value* id = project.find("id"); id != nullptr) {
        out.id = id->as_string();
    }
    if (const json::Value* version = project.find("version"); version != nullptr) {
        out.version = version->as_string();
    }
    if (const json::Value* icon = project.find("icon"); icon != nullptr) {
        out.icon = icon->as_string();
    }
    if (const json::Value* window = project.find("window"); window != nullptr) {
        if (const json::Value* width = window->find("width"); width != nullptr) {
            out.window_width = static_cast<int>(width->as_int());
        }
        if (const json::Value* height = window->find("height"); height != nullptr) {
            out.window_height = static_cast<int>(height->as_int());
        }
        if (const json::Value* vsync = window->find("vsync_mode"); vsync != nullptr) {
            out.vsync_mode = vsync->as_string();
        }
        if (const json::Value* driver = window->find("linux_display_driver"); driver != nullptr) {
            out.linux_display_driver = driver->as_string();
        }
    }
    if (const json::Value* tick_hz = project.find("tick_hz"); tick_hz != nullptr) {
        out.tick_hz = tick_hz->as_double();
    }
    if (const json::Value* max_fps = project.find("max_fps"); max_fps != nullptr) {
        out.max_fps = static_cast<int>(max_fps->as_int());
    }
    if (const json::Value* rendering = project.find("rendering"); rendering != nullptr) {
        if (const json::Value* color = rendering->find("clear_color"); color != nullptr) {
            const json::Array& channels = color->as_array();
            for (std::size_t i = 0; i < out.clear_color.size(); ++i) {
                out.clear_color.at(i) = static_cast<float>(channels.at(i).as_double());
            }
        }
        if (const json::Value* driver = rendering->find("driver"); driver != nullptr) {
            out.rendering_driver = driver->as_string();
        }
        if (const json::Value* fallback = rendering->find("fallback_to_opengl");
            fallback != nullptr) {
            out.fallback_to_opengl = fallback->as_bool();
        }
        if (const json::Value* method = rendering->find("rendering_method"); method != nullptr) {
            out.rendering_method = render::parse_render_method(method->as_string())
                                       .value_or(render::RenderMethod::ForwardPlus);
        }
        if (const json::Value* filter = rendering->find("texture_filter"); filter != nullptr) {
            out.texture_filter = filter->as_string();
        }
        if (const json::Value* interpolation = rendering->find("interpolation");
            interpolation != nullptr) {
            out.interpolation = interpolation->as_bool();
        }
        if (const json::Value* scaling = rendering->find("scaling"); scaling != nullptr) {
            out.scaling = render::scaling_from_json(*scaling);
        }
        if (const json::Value* shadows = rendering->find("lights_and_shadows");
            shadows != nullptr) {
            out.shadows = render::shadow_settings_from_json(*shadows);
        }
        if (const json::Value* camera = rendering->find("camera"); camera != nullptr) {
            if (const json::Value* dof = camera->find("depth_of_field"); dof != nullptr) {
                out.depth_of_field = render::depth_of_field_settings_from_json(*dof);
            }
        }
        if (const json::Value* environment = rendering->find("environment");
            environment != nullptr) {
            if (const json::Value* ssao = environment->find("ssao"); ssao != nullptr) {
                out.ssao = render::ssao_settings_from_json(*ssao);
            }
            if (const json::Value* ssr = environment->find("screen_space_reflection");
                ssr != nullptr) {
                out.ssr = render::ssr_settings_from_json(*ssr);
            }
            if (const json::Value* fog = environment->find("volumetric_fog"); fog != nullptr) {
                out.volumetric_fog = render::volumetric_fog_settings_from_json(*fog);
            }
        }
        if (const json::Value* reflections = rendering->find("reflections");
            reflections != nullptr && reflections->is_object()) {
            if (const json::Value* atlas = reflections->find("reflection_atlas");
                atlas != nullptr) {
                out.reflection_atlas = render::reflection_atlas_settings_from_json(*atlas);
            }
        }
        out.voxel_gi_high_quality = render::voxel_gi_high_quality(*rendering);
        out.use_taa = render::use_taa(*rendering);
        out.decal_filter = render::decal_filter(*rendering);
        out.mesh_lod_threshold = render::mesh_lod_threshold(*rendering);
        out.occlusion_culling = render::occlusion_culling(*rendering);
        if (const json::Value* textures = rendering->find("textures"); textures != nullptr) {
            if (const json::Value* mode = textures->find("compress_mode"); mode != nullptr) {
                out.texture_import.compress_mode = mode->as_string();
            }
            if (const json::Value* paths = textures->find("paths"); paths != nullptr) {
                for (const auto& [path, mode] : paths->as_object()) {
                    out.texture_import.paths.emplace(path, mode.as_string());
                }
            }
        }
    }
    if (const json::Value* physics = project.find("physics_2d"); physics != nullptr) {
        out.physics_2d = physics2d::parse_settings(*physics);
    }
    if (const json::Value* physics = project.find("physics_3d"); physics != nullptr) {
        out.physics_3d = physics3d::parse_settings(*physics);
    }
    if (const json::Value* map = project.find("navigation_2d"); map != nullptr) {
        out.navigation.map_2d = navigation::parse_settings(navigation::Map::TwoD, *map);
    }
    if (const json::Value* map = project.find("navigation_3d"); map != nullptr) {
        out.navigation.map_3d = navigation::parse_settings(navigation::Map::ThreeD, *map);
    }
    if (const json::Value* gui = project.find("gui"); gui != nullptr) {
        if (const json::Value* theme = gui->find("theme"); theme != nullptr) {
            out.gui_theme = theme->as_string();
        }
        if (const json::Value* system = gui->find("allow_system_fallback"); system != nullptr) {
            out.allow_system_fallback = system->as_bool();
        }
        if (const json::Value* delay = gui->find("tooltip_delay_sec"); delay != nullptr) {
            out.tooltip_delay_sec = delay->as_double();
        }
        if (const json::Value* offset = gui->find("tooltip_position_offset"); offset != nullptr) {
            out.tooltip_position_offset = {static_cast<float>(offset->as_array()[0].as_double()),
                                           static_cast<float>(offset->as_array()[1].as_double())};
        }
        if (const json::Value* threshold = gui->find("drag_threshold"); threshold != nullptr) {
            out.drag_threshold = static_cast<int>(threshold->as_int());
        }
    }
    if (const json::Value* input = project.find("input"); input != nullptr) {
        auto map = input::parse_map(*input);
        if (!map) {
            return make_error(ErrorCode::InvalidArgument,
                              std::string(kProjectFile) + " /input" + map.error().message);
        }
        out.input = std::move(map).value();
    }
    if (const json::Value* devices = project.find("input_devices"); devices != nullptr) {
        out.input_devices = input::parse_settings(*devices);
    }
    if (const json::Value* audio = project.find("audio"); audio != nullptr) {
        out.audio = audio::parse_settings(*audio);
    }
    if (const json::Value* services = project.find("services"); services != nullptr) {
        out.services = services::parse_settings(*services);
    }
    if (const json::Value* gameplay = project.find("gameplay"); gameplay != nullptr) {
        for (const json::Value& source : gameplay->find("sources")->as_array()) {
            out.gameplay_sources.push_back(source.as_string());
        }
    }
    if (const json::Value* sdks = project.find("sdks"); sdks != nullptr) {
        for (const json::Value& folder : sdks->as_array()) {
            out.sdks.push_back(folder.as_string());
        }
    }
    if (const json::Value* threading = project.find("threading"); threading != nullptr) {
        out.max_threads = jobs::parse_max_threads(*threading);
    }
    return out;
}

Result<Project> load_project(std::string_view folder) {
    const std::string path = join_path(folder, kProjectFile);
    auto text = platform::read_file(path);
    if (!text) {
        return make_error(ErrorCode::NotFound, "no " + std::string(kProjectFile) + " in \"" +
                                                   std::string(folder) +
                                                   R"("; run "slash-cli init" to create one)");
    }
    auto parsed = json::parse(text.value());
    if (!parsed) {
        return make_error(ErrorCode::InvalidArgument,
                          std::string(kProjectFile) + ": " + parsed.error().message);
    }
    return parse_project(parsed.value());
}

json::Value new_project(std::string_view name, std::string_view main_scene) {
    json::Value project;
    project["format"] = kProjectFormat;
    project["name"] = name;
    project["main_scene"] = main_scene;
    project["version"] = Project{}.version;
    return project;
}

std::string join_path(std::string_view folder, std::string_view relative) {
    std::string path(folder);
    if (!path.empty() && path.back() != '/' && path.back() != '\\') {
        path += '/';
    }
    path += relative;
    return path;
}

world::FileReader folder_reader(std::string folder) {
    return [folder = std::move(folder)](std::string_view path) -> Result<std::string> {
        if (!is_project_path(path)) {
            return make_error(ErrorCode::PermissionDenied,
                              "path \"" + std::string(path) +
                                  "\" is not inside the project folder");
        }
        return platform::read_file_in(folder, path);
    };
}

} // namespace slash::runtime
