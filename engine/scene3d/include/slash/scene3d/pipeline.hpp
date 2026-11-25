#pragma once

#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/result.hpp"
#include "slash/render/compositor.hpp"
#include "slash/world/world.hpp"

#include <array>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace slash::render {
struct Scene3D;
}

// Pipeline files: a .pipeline.json
// file that a Environment3D's or a Camera3D's compositor names (the camera's wins)
// turns the frame's passes off and adds the game's own screen passes. Each effect is a compositor
// material (a .material.json file with "shader_type": "compositor", whose fragment() reads
// SCREEN_TEXTURE, DEPTH_TEXTURE and NORMAL_ROUGHNESS_TEXTURE and writes COLOR) drawn over the 3D
// world at one point of a clustered frame (render::EffectCallback), in the file's order within a
// point; its code names a C++ callback the game registers, run each frame before drawing with the
// effect to change. The compatibility renderer and the OpenGL devices draw none, as they have
// no compositor.
namespace slash::scene3d {

inline constexpr std::string_view kPipelineSuffix = ".pipeline.json";
[[nodiscard]] bool is_pipeline_path(std::string_view path);

// The passes a pipeline file can turn off, by name: "ssao", "ssr", "glow", "adjustments",
// "subsurface", "volumetric_fog", "sdfgi", "dof", "auto_exposure", "taa", "decals" and "fog".
inline constexpr std::array<std::string_view, 12> kPipelinePasses{
    "ssao",  "ssr", "glow",          "adjustments", "subsurface", "volumetric_fog",
    "sdfgi", "dof", "auto_exposure", "taa",         "decals",     "fog"};

// One of a pipeline file's effects.
struct PipelineEffect {
    std::string name;
    bool enabled = true;
    render::EffectCallback callback = render::EffectCallback::PostTransparent;
    std::string material; // a compositor material's project path
    std::map<std::string, json::Value> material_params;
    bool needs_normal_roughness = false;
    std::string code; // a callback add_compositor_code registered, or ""
};

// A .pipeline.json file: the passes it turns off (false) or leaves to the environment (true),
// and its effects in order.
struct PipelineFile {
    std::map<std::string, bool> passes;
    std::vector<PipelineEffect> effects;
};

// The names of the callback points: "pre_opaque", "post_opaque", "post_sky",
// "pre_transparent" and "post_transparent".
[[nodiscard]] std::string_view effect_callback_name(render::EffectCallback callback);
[[nodiscard]] std::optional<render::EffectCallback> effect_callback_named(std::string_view name);

// The JSON Schema of .pipeline.json files ($cli schema pipeline).
[[nodiscard]] json::Value pipeline_schema();
// Every error in a pipeline file, with its JSON Pointer: the schema's (which holds the effects to
// render::kMaxCompositorEffects), then effects named twice and materials that aren't
// .material.json files.
[[nodiscard]] std::vector<json::ValidationError> validate_pipeline(const json::Value& doc);
// Reads a pipeline file that validates.
[[nodiscard]] Result<PipelineFile> parse_pipeline(const json::Value& doc);

// A game's code for an effect: run each frame
// before the effect draws, it may change the effect's material_params, enabled and anything else
// (but not its name).
using CompositorCode = std::function<void(const world::World& world, PipelineEffect& effect)>;
// Registers code by name for every world; registering a name
// again replaces it (as a reload of the game's code does), and empty code removes it. Call it
// from a SLASH_GAME_MODULE function.
void add_compositor_code(const std::string& name, CompositorCode code);
// The code registered by a name, or empty.
[[nodiscard]] CompositorCode compositor_code(const std::string& name);

// The pipeline file a world's frames use: the current camera's compositor, else the first
// Environment3D's; "" for none. at is where it was named, for problems
// ("/components/Camera3D/compositor").
struct PipelineChoice {
    entt::entity entity = entt::null;
    std::string path;
    std::string at;
};
[[nodiscard]] PipelineChoice pipeline_of(const world::World& world, entt::entity camera);

// Turns off the passes a pipeline file says are off in a frame's scene, after the environment
// and the project's settings filled it in.
void apply_pipeline_passes(const std::map<std::string, bool>& passes, render::Scene3D& scene);

} // namespace slash::scene3d
