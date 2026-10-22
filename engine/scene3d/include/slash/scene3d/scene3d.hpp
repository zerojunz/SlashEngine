#pragma once

#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/result.hpp"
#include "slash/render/device.hpp"
#include "slash/render/mesh.hpp"
#include "slash/scene3d/model.hpp"
#include "slash/scene3d/primitive.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/scene.hpp"
#include "slash/world/tree3d.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <entt/entity/entity.hpp>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace slash::world {
class World;
}

// The 3D world, as Godot's 3D nodes draw it: Transform3D trees (world/tree3d.hpp), meshes
// (MeshInstance3D with a primitive mesh and a StandardMaterial3D, or a glTF model), the current
// Camera3D, the DirectionalLight3D, OmniLight3D and SpotLight3D lights and the WorldEnvironment,
// turned into a frame's render::Scene3D and mesh draws (render/mesh.hpp). canvas::CanvasRenderer
// draws them under the canvas, as Godot's viewports draw 3D under 2D.
namespace slash::scene3d {

// The camera the world is seen through, as Godot's Viewport::get_camera_3d: the first Camera3D
// with current set in tree order, else the first one; null with none.
[[nodiscard]] entt::entity current_camera(const world::World& world, const world::Tree3D& tree);

// What a camera sees: the world to its space and its space to clip space, with reversed depth.
struct CameraView {
    std::array<float, 16> view{};
    std::array<float, 16> projection{};
};

// The view through a camera at global for a target of aspect width over height, as Godot's
// Camera3D: its transform orthonormalized and moved by h_offset and v_offset, and its projection
// spanning fov (or size) across the axis keep_aspect names. The same bits on every platform.
[[nodiscard]] CameraView camera_view(const world::Camera3D& camera, const world::Affine3D& global,
                                     float aspect);

// Something that couldn't draw, such as a mesh whose texture is missing: path is the JSON
// Pointer of the bad value in the entity, e.g. "/components/StandardMaterial3D/albedo_texture".
struct Problem {
    entt::entity entity = entt::null;
    std::string path;
    std::string message;
};

// The background a frame clears to, the sky and fog, the light every lit pixel gets and
// reflects and what happens to colors after, from the first WorldEnvironment made
// (WorldEnvironment's docs); clear is the project's clear color.
struct Environment {
    render::Color background;
    render::Sky3D sky; // with no texture made yet and no lights
    // The sky's light for ambient light and reflections, from the material sky_material names.
    render::Radiance3D radiance;
    // The entity's sky material, read when the background or the sky's light uses it.
    render::SkyKind sky_material = render::SkyKind::None;
    render::Fog3D fog;
    std::array<float, 3> ambient{0.0F, 0.0F, 0.0F}; // sRGB
    float ambient_energy = 1.0F;
    render::PostProcess post;     // with no color correction texture made yet
    std::string color_correction; // its project path, or ""
    // The texture the sky material samples: its project path, or "", how it filters and the
    // field that names it, e.g. "/components/PanoramaSkyMaterial/panorama".
    std::string sky_texture;
    render::Filter sky_filter = render::Filter::Linear;
    std::string sky_texture_field;
    entt::entity entity = entt::null; // the WorldEnvironment, if there is one
    std::vector<Problem> problems;    // such as more than one sky material
};
[[nodiscard]] Environment environment(const world::World& world, render::Color clear);

// The lights of the world that shine, in tree order, as a frame's lights: those visible in the
// tree whose sky_mode isn't "sky_only", placed by global, at most render::kMaxLights3D.
[[nodiscard]] std::vector<render::Light3D>
lights(const world::World& world, const world::Tree3D& tree,
       const std::function<world::Affine3D(entt::entity)>& global);

// The suns a sky shows, as Godot's: the DirectionalLight3Ds visible in the tree whose sky_mode
// isn't "light_only", in tree order, at most render::kMaxSkyLights.
[[nodiscard]] std::vector<render::SkyLight>
sky_lights(const world::World& world, const world::Tree3D& tree,
           const std::function<world::Affine3D(entt::entity)>& global);

// A texture made on the device, and its size.
struct SourceTexture {
    render::TextureId id = render::TextureId::None;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};
// Textures a material or the environment samples, made on the device by project path and filter.
using TextureSource =
    std::function<Result<SourceTexture>(const std::string& path, render::Filter filter)>;

// A model loaded by project path, and its version: a reload raises it, and its meshes are made
// again.
struct SourceModel {
    std::shared_ptr<const Model> model;
    std::uint64_t version = 0;
};
// The models MeshInstance3D's mesh names, loaded by project path.
using ModelSource = std::function<Result<SourceModel>(const std::string& path)>;

// The meshes of a world that can't draw, in the order their entities were made: an entity with
// more than one primitive mesh, a MeshInstance3D with none or with both a model and a primitive
// mesh, and settings Godot refuses (a torus whose radii are equal). With the world's file reader
// set, each model is read and checked as check_model does.
[[nodiscard]] std::vector<Problem> check_meshes(const world::World& world);

// The same in a parsed scene file, each at its JSON Pointer in the file. Prefabs and instanced
// scenes are read with reader. A scene that doesn't load has none: world::validate_scene reports
// why.
[[nodiscard]] std::vector<json::ValidationError>
check_meshes(const world::ComponentRegistry& components, const json::Value& scene,
             const world::FileReader& reader);

// A StandardMaterial3D as the render device takes it, its textures made by textures. A texture
// that can't load is left out, with a problem.
[[nodiscard]] render::Material3D material(const world::StandardMaterial3D& standard,
                                          const TextureSource& textures,
                                          std::vector<Problem>& problems,
                                          entt::entity entity = entt::null);

// What the 3D world adds to a frame.
struct Frame3D {
    entt::entity camera = entt::null;
    render::Scene3D scene;
    render::Color clear; // the WorldEnvironment's background, or the clear color
};

// Draws a world's meshes through a render device, keeping their meshes on the device between
// frames: meshes of equal settings share one, each model's meshes and images are made once, and
// a mesh no entity used last frame goes.
class Renderer {
public:
    Renderer(render::RenderDevice& device, TextureSource textures, ModelSource models = {});
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    Renderer(Renderer&&) = delete;
    Renderer& operator=(Renderer&&) = delete;

    // Remembers every Transform3D, so frames between ticks can place entities part of the way
    // from where they were to where they are. Call it just before each tick.
    void before_tick(const world::World& world);

    // Works out the frame for a target of width by height pixels, interpolation of the way from
    // the transforms before the last tick to the current ones, its shadows drawn with shadows:
    // nullopt when the world has no camera, and nothing 3D draws. The mesh draws wait for draw().
    [[nodiscard]] std::optional<Frame3D> prepare(const world::World& world, float width,
                                                 float height, double interpolation,
                                                 render::Color clear,
                                                 const render::ShadowSettings& shadows = {});
    // Draws the meshes prepare found into the frame begun with its scene.
    [[nodiscard]] Result<void> draw();

    // What the last prepare couldn't draw.
    [[nodiscard]] const std::vector<Problem>& problems() const noexcept { return problems_; }
    // The meshes and lights the last prepare found to draw, and the meshes kept on the device.
    [[nodiscard]] std::size_t meshes() const noexcept { return draws_.size(); }
    [[nodiscard]] std::size_t lights() const noexcept { return lights_; }
    [[nodiscard]] std::size_t meshes_kept() const noexcept { return shared_.size(); }

private:
    struct Local {
        Vec3 position;
        Quat rotation;
        Vec3 scale;
    };
    struct Shared {
        render::MeshId id = render::MeshId::None;
        std::uint64_t used = 0; // the last frame that drew it
    };
    struct Cached {
        Primitive primitive;
        std::string key;
    };
    struct Draw {
        render::MeshId mesh = render::MeshId::None;
        std::array<float, 16> transform{};
        render::Material3D material;
        std::uint32_t layers = 1;
        render::ShadowCasting shadows = render::ShadowCasting::On;
    };

    struct Embedded {
        render::TextureId id = render::TextureId::None;
        std::uint64_t used = 0;
    };

    Result<render::MeshId> mesh_for(const world::World& world, entt::entity entity,
                                    const Primitive& primitive);
    // A model's surface as a mesh on the device, made once per version of the model.
    Result<render::MeshId> model_mesh(const std::string& key, const ModelSurface& surface);
    // A texture a model's material samples: a file through textures_, or an image inside it.
    render::MaterialTexture model_texture(const std::string& path, const SourceModel& source,
                                          int image, const world::StandardMaterial3D& standard,
                                          entt::entity entity);
    void add_model(const world::World& world, entt::entity entity,
                   const world::MeshInstance3D& instance, const world::Affine3D& place,
                   render::ShadowCasting casting);

    render::RenderDevice* device_;
    TextureSource textures_;
    ModelSource models_;
    std::unordered_map<std::string, Embedded> embedded_;
    std::unordered_map<entt::entity, Local> before_;
    std::unordered_map<entt::entity, Cached> cached_;
    std::unordered_map<std::string, Shared> shared_;
    std::vector<Draw> draws_;
    std::vector<Problem> problems_;
    std::uint64_t frame_ = 0;
    std::size_t lights_ = 0;
};

} // namespace slash::scene3d
