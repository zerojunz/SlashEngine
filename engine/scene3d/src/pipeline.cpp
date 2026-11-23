#include "slash/scene3d/pipeline.hpp"

#include "slash/core/assert.hpp"
#include "slash/render/mesh.hpp"
#include "slash/world/components.hpp"

#include <algorithm>
#include <array>
#include <mutex>
#include <set>
#include <utility>

namespace slash::scene3d {

namespace {

constexpr std::array<std::string_view, render::kEffectCallbacks> kCallbackNames{
    "pre_opaque", "post_opaque", "post_sky", "pre_transparent", "post_transparent"};

constexpr std::string_view kSchema = R"({
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "title": "SlashEngine pipeline",
  "description": "A frame's passes and compositor effects, as Godot 4.3's Compositor and CompositorEffect, named by a Environment3D's or a Camera3D's compositor. Forward+ only: Compatibility draws no effects.",
  "type": "object",
  "additionalProperties": false,
  "properties": {
    "passes": {
      "type": "object",
      "description": "Passes by name: false turns one off whatever the environment and project say; true or missing leaves it to them.",
      "additionalProperties": false,
      "properties": {
        "ssao": {"type": "boolean", "description": "Screen-space ambient occlusion."},
        "ssr": {"type": "boolean", "description": "Screen-space reflections."},
        "glow": {"type": "boolean", "description": "Glow."},
        "adjustments": {"type": "boolean", "description": "Brightness, contrast, saturation and the color correction."},
        "subsurface": {"type": "boolean", "description": "Subsurface scattering."},
        "volumetric_fog": {"type": "boolean", "description": "Volumetric fog."},
        "sdfgi": {"type": "boolean", "description": "SDFGI."},
        "dof": {"type": "boolean", "description": "Depth of field, near and far."},
        "auto_exposure": {"type": "boolean", "description": "Auto exposure."},
        "taa": {"type": "boolean", "description": "TAA."},
        "decals": {"type": "boolean", "description": "Decals."},
        "fog": {"type": "boolean", "description": "Environment fog."}
      }
    },
    "effects": {
      "type": "array",
      "maxItems": 16,
      "description": "Compositor effects, drawn in this order within each point of the frame.",
      "items": {
        "type": "object",
        "additionalProperties": false,
        "required": ["name", "material"],
        "properties": {
          "name": {"type": "string", "minLength": 1, "description": "The effect's name, unique in the file."},
          "enabled": {"type": "boolean", "description": "Whether it draws. Default true."},
          "effect_callback_type": {"enum": ["pre_opaque", "post_opaque", "post_sky", "pre_transparent", "post_transparent"], "description": "Where in the frame it draws, as Godot's EffectCallbackType: pre_opaque over the sky before the opaque meshes, post_opaque, post_sky and pre_transparent in that order after them, post_transparent after the blended meshes, before TAA, FSR and post-processing. Default post_transparent."},
          "material": {"type": "string", "pattern": "\\.material\\.json$", "description": "Project path of a material with \"shader_type\": \"compositor\"."},
          "material_params": {"type": "object", "description": "Values of the material's params by name, over its defaults."},
          "needs_normal_roughness": {"type": "boolean", "description": "Whether NORMAL_ROUGHNESS_TEXTURE reads the prepass's normals and roughness, which the frame then draws, as Godot's needs_normal_roughness. Default false: it reads a normal facing the camera and a roughness of 1."},
          "code": {"type": "string", "description": "The name of a C++ callback the game's code registers with scene3d::add_compositor_code, run each frame before the effect draws to change it."}
        }
      }
    }
  }
})";

struct CodeRegistry {
    std::mutex lock;
    std::map<std::string, CompositorCode, std::less<>> code;
};

CodeRegistry& registry() {
    static CodeRegistry codes;
    return codes;
}

bool flag(const json::Value& object, std::string_view key, bool otherwise) {
    const json::Value* found = object.find(key);
    return found != nullptr ? found->as_bool() : otherwise;
}

std::string text(const json::Value& object, std::string_view key) {
    const json::Value* found = object.find(key);
    return found != nullptr ? found->as_string() : std::string();
}

} // namespace

bool is_pipeline_path(std::string_view path) {
    return path.ends_with(kPipelineSuffix);
}

std::string_view effect_callback_name(render::EffectCallback callback) {
    return kCallbackNames.at(static_cast<std::size_t>(callback));
}

std::optional<render::EffectCallback> effect_callback_named(std::string_view name) {
    for (std::size_t i = 0; i < kCallbackNames.size(); ++i) {
        if (kCallbackNames.at(i) == name) {
            return static_cast<render::EffectCallback>(i);
        }
    }
    return std::nullopt;
}

json::Value pipeline_schema() {
    static const json::Value schema = [] {
        auto parsed = json::parse(kSchema);
        SLASH_ASSERT(parsed.ok(), "the pipeline schema parses");
        return std::move(parsed).value();
    }();
    return schema;
}

std::vector<json::ValidationError> validate_pipeline(const json::Value& doc) {
    std::vector<json::ValidationError> errors = json::validate(doc, pipeline_schema());
    if (!errors.empty()) {
        return errors;
    }
    const json::Value* effects = doc.find("effects");
    if (effects == nullptr) {
        return errors;
    }
    std::set<std::string, std::less<>> names;
    const json::Array& list = effects->as_array();
    for (std::size_t i = 0; i < list.size(); ++i) {
        const std::string& name = list[i].find("name")->as_string();
        if (!names.insert(name).second) {
            errors.push_back({"/effects/" + std::to_string(i) + "/name",
                              "another effect is named \"" + name + "\""});
        }
        // The schema's pattern, which json::validate leaves to its users.
        if (!list[i].find("material")->as_string().ends_with(".material.json")) {
            errors.push_back({"/effects/" + std::to_string(i) + "/material",
                              "a compositor effect's material is a .material.json file"});
        }
    }
    return errors;
}

Result<PipelineFile> parse_pipeline(const json::Value& doc) {
    if (const auto errors = validate_pipeline(doc); !errors.empty()) {
        return make_error(ErrorCode::InvalidArgument,
                          errors.front().path + ": " + errors.front().message);
    }
    PipelineFile out;
    if (const json::Value* passes = doc.find("passes"); passes != nullptr) {
        for (const auto& [name, on] : passes->as_object()) {
            out.passes.emplace(name, on.as_bool());
        }
    }
    if (const json::Value* effects = doc.find("effects"); effects != nullptr) {
        for (const json::Value& item : effects->as_array()) {
            PipelineEffect effect;
            effect.name = text(item, "name");
            effect.enabled = flag(item, "enabled", true);
            if (const json::Value* point = item.find("effect_callback_type"); point != nullptr) {
                if (const auto named = effect_callback_named(point->as_string())) {
                    effect.callback = *named;
                }
            }
            effect.material = text(item, "material");
            if (const json::Value* params = item.find("material_params"); params != nullptr) {
                for (const auto& [name, value] : params->as_object()) {
                    effect.material_params.emplace(name, value);
                }
            }
            effect.needs_normal_roughness = flag(item, "needs_normal_roughness", false);
            effect.code = text(item, "code");
            out.effects.push_back(std::move(effect));
        }
    }
    return out;
}

void add_compositor_code(const std::string& name, CompositorCode code) {
    CodeRegistry& codes = registry();
    const std::scoped_lock held(codes.lock);
    if (code) {
        codes.code.insert_or_assign(name, std::move(code));
    } else {
        codes.code.erase(name);
    }
}

CompositorCode compositor_code(const std::string& name) {
    CodeRegistry& codes = registry();
    const std::scoped_lock held(codes.lock);
    const auto found = codes.code.find(name);
    return found != codes.code.end() ? found->second : CompositorCode{};
}

PipelineChoice pipeline_of(const world::World& world, entt::entity camera) {
    const auto& ecs = world.ecs();
    if (camera != entt::null && world.valid(camera)) {
        if (const auto* lens = ecs.try_get<world::Camera3D>(camera);
            lens != nullptr && !lens->compositor.empty()) {
            return {.entity = camera,
                    .path = lens->compositor,
                    .at = "/components/Camera3D/compositor"};
        }
    }
    // The first Environment3D made, as the environment's other settings take it.
    entt::entity first = entt::null;
    for (const entt::entity entity : ecs.view<const world::Environment3D>()) {
        if (first == entt::null || entt::to_entity(entity) < entt::to_entity(first)) {
            first = entity;
        }
    }
    if (first == entt::null) {
        return {};
    }
    const auto& environment = ecs.get<const world::Environment3D>(first);
    if (environment.compositor.empty()) {
        return {};
    }
    return {.entity = first,
            .path = environment.compositor,
            .at = "/components/Environment3D/compositor"};
}

void apply_pipeline_passes(const std::map<std::string, bool>& passes, render::Scene3D& scene) {
    const auto off = [&](std::string_view name) {
        const auto found = passes.find(std::string(name));
        return found != passes.end() && !found->second;
    };
    if (off("ssao")) {
        scene.ssao.enabled = false;
    }
    if (off("ssr")) {
        scene.ssr.enabled = false;
    }
    if (off("glow")) {
        scene.post.glow = false;
    }
    if (off("adjustments")) {
        scene.post.adjustments = false;
    }
    if (off("subsurface")) {
        scene.subsurface.quality = render::SubsurfaceQuality::Disabled;
    }
    if (off("volumetric_fog")) {
        scene.volumetric_fog.enabled = false;
        scene.volumetric_fog.volumes.clear();
    }
    if (off("sdfgi")) {
        scene.sdfgi.enabled = false;
    }
    if (off("dof")) {
        scene.post.dof_far = false;
        scene.post.dof_near = false;
    }
    if (off("auto_exposure")) {
        scene.post.auto_exposure = false;
    }
    if (off("taa")) {
        scene.use_taa = false;
    }
    if (off("decals")) {
        scene.decals.decals.clear();
    }
    if (off("fog")) {
        scene.fog.enabled = false;
    }
}

} // namespace slash::scene3d
