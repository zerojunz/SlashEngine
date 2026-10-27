#pragma once

#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/result.hpp"
#include "slash/render/device.hpp"
#include "slash/render/image.hpp"
#include "slash/render/mesh.hpp"
#include "slash/scene3d/model.hpp"
#include "slash/scene3d/occlusion.hpp"
#include "slash/scene3d/primitive.hpp"
#include "slash/scene3d/skeleton.hpp"
#include "slash/scene3d/surface.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/scene.hpp"
#include "slash/world/tree3d.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <entt/entity/entity.hpp>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
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
    world::Affine3D eye; // where the camera is, as Godot's get_camera_transform: view undone
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
// (WorldEnvironment's docs) and the CameraAttributesPractical camera uses (camera_attributes);
// clear is the project's clear color.
struct Environment {
    render::Color background;
    render::Sky3D sky; // with no texture made yet and no lights
    // The sky's light for ambient light and reflections, from the material sky_material names.
    render::Radiance3D radiance;
    // The entity's sky material, read when the background or the sky's light uses it.
    render::SkyKind sky_material = render::SkyKind::None;
    render::Fog3D fog;
    render::Ssao3D ssao; // with the default settings, which the project's replace
    render::Ssr3D ssr;   // the same
    render::VolumetricFog3D volumetric_fog;         // the same
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
    // The entity whose CameraAttributesPractical the camera uses, or entt::null, and its
    // exposure_multiplier, as Godot's exposure normalization without physical light units: the
    // background, the sky (and so its light) and post's auto exposure have it already, and the
    // frame's lights, the sky's suns and emission take it too.
    entt::entity attributes = entt::null;
    float exposure = 1.0F;
    std::vector<Problem> problems; // such as more than one sky material
};
[[nodiscard]] Environment environment(const world::World& world, render::Color clear,
                                      entt::entity camera = entt::null);
// The environment of the first WorldEnvironment made, before any camera's attributes, as
// Godot's environment bakes take it (a LightmapGI's).
[[nodiscard]] Environment world_environment(const world::World& world, render::Color clear);

// The entity whose CameraAttributesPractical a camera uses, as Godot's Camera3D.attributes and
// WorldEnvironment.camera_attributes: the camera's own, else the first WorldEnvironment's;
// entt::null when neither has one.
[[nodiscard]] entt::entity camera_attributes(const world::World& world, entt::entity camera);

// The lights of the world that shine, as a frame's lights: those visible in the tree whose
// sky_mode isn't "sky_only", placed by global, the directional ones first, each kind in tree
// order and at most render::kMaxDirectionalLights3D and render::kMaxClusteredLights3D.
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

struct LightmapData;
struct LightmapUnwrap;

// A baked lightmap loaded by its .lightmap.json file's project path (lightmap.hpp), with the
// texels of its .hdr file: a reload of either gives new ones, and its texture is made again.
struct SourceLightmap {
    std::shared_ptr<const LightmapData> data;
    std::shared_ptr<const render::HdrImage> texels;
};
// The lightmaps LightmapGI's light_data names, loaded by project path.
using LightmapSource = std::function<Result<SourceLightmap>(const std::string& path)>;

struct VoxelGIData;

// The cells a VoxelGI's data names (a .voxelgi.json file, voxel_gi.hpp), loaded by project path:
// a reload gives new ones, and their light is injected again.
using VoxelGISource =
    std::function<Result<std::shared_ptr<const VoxelGIData>>(const std::string& path)>;

// The images loaded by project path that the CPU reads, as moving meshes' materials plotted into
// VoxelGIs: a reload gives new ones.
using ImageSource =
    std::function<Result<std::shared_ptr<const render::Image>>(const std::string& path)>;

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
// set, each model is read and checked as check_model does, and the bone names of Skeleton3Ds,
// skeleton modifiers and bone attachments against the models they follow. CSG shapes that can't
// be made (check_csg), path followers that can't follow (world::check_paths), GridMaps whose
// cells can't be made (check_gridmaps), terrains (check_terrains) and LightmapGIs
// (check_lightmaps) come after.
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
// a mesh no entity used last frame goes. Static meshes a LightmapGI baked take their ambient
// light, and their baked lights' light, from its lightmap, as Godot's lightmapped instances, and
// dynamic ones their ambient light from its light probes, eased as they move. In Forward+, the
// other meshes take ambient light and reflections from the VoxelGIs their bounds touch, whose
// cells the lights are injected into again whenever they change, and into whose light the
// dynamic meshes in their boxes are plotted again whenever they move or change (their materials'
// textures read through images).
class Renderer {
public:
    Renderer(render::RenderDevice& device, TextureSource textures, ModelSource models = {},
             LightmapSource lightmaps = {}, VoxelGISource voxel_gis = {}, ImageSource images = {});
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    Renderer(Renderer&&) = delete;
    Renderer& operator=(Renderer&&) = delete;

    // Remembers every Transform3D, and the animations each model plays, so frames between ticks
    // can place entities and pose models part of the way from where they were to where they are.
    // Call it just before each tick.
    void before_tick(const world::World& world);

    // Works out the frame for a target of width by height pixels, interpolation of the way from
    // the transforms before the last tick to the current ones, its shadows drawn with shadows,
    // shown at time seconds of game time, over which auto exposure adapts and from which depth of
    // field's jitter is seeded, its depth of field blurred as depth_of_field says, its ambient
    // occlusion worked out as ssao says and its reflections as ssr says: nullopt when the world
    // has no camera, and nothing 3D draws. The mesh draws wait for draw(). In Forward+ (method),
    // the ReflectionProbes' cubes that need it are drawn here, in frames of their own on the
    // device, into an atlas as reflection_atlas says; so call it before the frame begins. Its
    // VoxelGIs' cones are traced as Godot's high quality setting traces them where
    // voxel_gi_high_quality is set. With use_taa, the frame is antialiased over time, as Godot's
    // use_taa, its meshes given the transforms they were drawn at in the last frame; so it is with
    // fsr2, which draws its 3D smaller and upscales it with FSR 2 (render::uses_fsr2 says when).
    // Its Decals' textures are sampled with decal_filter, and its volumetric fog's froxels are as
    // volumetric_fog says. A model's surfaces are drawn at the coarsest LOD that is off by no more
    // than mesh_lod_threshold pixels, as Godot's rendering/mesh_lod/lod_change/threshold_pixels.
    // With occlusion on, meshes the OccluderInstance3Ds hide from the camera aren't drawn, though
    // they still cast shadows and show in reflection probes, as Godot's raycast occlusion culling.
    [[nodiscard]] std::optional<Frame3D>
    prepare(const world::World& world, float width, float height, double interpolation,
            render::Color clear, const render::ShadowSettings& shadows = {}, double time = 0.0,
            const render::DepthOfFieldSettings& depth_of_field = {},
            const render::SsaoSettings& ssao = {}, const render::SsrSettings& ssr = {},
            const render::ReflectionAtlasSettings& reflection_atlas = {},
            render::RenderMethod method = render::RenderMethod::ForwardPlus,
            bool voxel_gi_high_quality = false, bool use_taa = false,
            const std::optional<render::Fsr2Scaling>& fsr2 = std::nullopt,
            render::DecalFilter decal_filter = render::DecalFilter::LinearMipmaps,
            const render::VolumetricFogSettings& volumetric_fog = {},
            float mesh_lod_threshold = 1.0F, const render::OcclusionCulling& occlusion = {});
    // Draws the meshes prepare found into the frame begun with its scene.
    [[nodiscard]] Result<void> draw();

    // What the last prepare couldn't draw.
    [[nodiscard]] const std::vector<Problem>& problems() const noexcept { return problems_; }
    // The meshes and lights the last prepare found to draw, and the meshes kept on the device.
    [[nodiscard]] std::size_t meshes() const noexcept { return draws_.size(); }
    [[nodiscard]] std::size_t lights() const noexcept { return lights_; }
    [[nodiscard]] std::size_t meshes_kept() const noexcept { return shared_.size(); }
    // How many of the last prepare's draws are at a coarser LOD than their surface's own.
    [[nodiscard]] std::size_t lods_drawn() const noexcept { return lods_drawn_; }
    // How many of the last prepare's meshes the occluders hid from the camera.
    [[nodiscard]] std::size_t occluded() const noexcept { return occluded_; }
    // The occlusion buffer the last prepare cast, empty with occlusion off.
    [[nodiscard]] const OcclusionBuffer& occlusion_buffer() const noexcept {
        return occlusion_buffer_;
    }

private:
    struct Local {
        Vec3 position;
        Quat rotation;
        Vec3 scale;
    };
    struct Shared {
        render::MeshId id = render::MeshId::None;
        std::uint64_t used = 0;       // the last frame that drew it
        Vec3 center;                  // the middle of its vertices' bounds, in its own space
        Bounds box;                   // its vertices' bounds, in its own space
        std::vector<SurfaceLod> lods; // a model surface's coarser LODs
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
        entt::entity entity = entt::null; // the entity it draws
        // Where it was drawn in the last frame with TAA, when that was somewhere else.
        std::optional<std::array<float, 16>> previous;
        // A model surface's coarser LODs (kept in its Shared or Skinned), its mesh's bounds in its
        // own space and its MeshInstance3D's lod_bias, which pick the LOD it is drawn at.
        const std::vector<SurfaceLod>* lods = nullptr;
        Bounds box;
        float lod_bias = 1.0F;
        std::uint32_t lod = 0;
    };
    // Picks each draw's LOD as Godot's Forward+ does for a camera at camera_position, seen
    // through projection at width pixels across with a threshold of threshold_pixels.
    void pick_lods(const Vec3& camera_position, const std::array<float, 16>& projection,
                   bool orthogonal, float width, float threshold_pixels);
    // Casts the occlusion buffer for the camera from the world's occluders and leaves the meshes
    // it hides out of the frame, as Godot's _scene_cull leaves them out of the camera's list:
    // drawn only into shadow maps when they cast shadows.
    void cull_occluded(const world::World& world, const world::Tree3D& tree, const CameraView& view,
                       const world::Camera3D& lens, float width, float height,
                       const render::OcclusionCulling& settings);
    // The bounds of each mesh drawn this frame, in its own space.
    [[nodiscard]] std::unordered_map<render::MeshId, Bounds> mesh_boxes() const;

    // What a probe's cube was drawn from: it is drawn again when this changes.
    struct ProbeKey {
        std::array<float, 16> transform{};
        Vec3 size;
        Vec3 origin_offset;
        float max_distance = 0.0F;
        std::uint32_t cull_mask = 0;
        bool interior = false;
        bool enable_shadows = false;

        friend bool operator==(const ProbeKey&, const ProbeKey&) = default;
    };
    struct ProbeSlot {
        std::uint32_t cube = 0;
        std::optional<ProbeKey> drawn;
    };
    // Gives the frame its probes, drawing their cubes first where they need it.
    [[nodiscard]] Result<void> reflect(const world::World& world, Frame3D& frame,
                                       render::Color clear,
                                       const render::ReflectionAtlasSettings& settings,
                                       render::RenderMethod method);

    struct Embedded {
        render::TextureId id = render::TextureId::None;
        std::uint64_t used = 0;
    };
    // What poses a model: its clips and its Skeleton3D.
    struct PoseKey {
        std::vector<ClipSample> clips;
        std::optional<world::Skeleton3D> skeleton;

        friend bool operator==(const PoseKey&, const PoseKey&) = default;
    };
    // A skinned surface of one entity's model, moved by its joints into a mesh of its own.
    struct Skinned {
        render::MeshId id = render::MeshId::None;
        std::uint64_t used = 0;
        std::vector<world::Affine3D> joints; // what moved its vertices last
        Bounds box;                          // its moved vertices' bounds
        std::vector<SurfaceLod> lods;        // its surface's coarser LODs
    };

    Result<render::MeshId> mesh_for(const world::World& world, entt::entity entity,
                                    const Primitive& primitive);
    // A model's surface, a CSG root's or a GridMap octant's, as a mesh on the device, made once
    // per key.
    // A model surface's LODs come with it, and are kept in its Shared.
    Result<render::MeshId> kept_mesh(const std::string& key,
                                     const std::vector<render::Vertex3D>& vertices,
                                     const std::vector<std::uint32_t>& indices,
                                     const std::vector<SurfaceLod>& lods = {});
    // A texture a model's material samples: a file through textures_, or an image inside it.
    // What can't be made is a problem at the entity's field.
    render::MaterialTexture model_texture(const std::string& path, const SourceModel& source,
                                          int image, const world::StandardMaterial3D& standard,
                                          entt::entity entity, const std::string& field);
    // place is where the entity is drawn, and global where others are, for its skeleton
    // modifiers' targets.
    void add_model(const world::World& world, entt::entity entity,
                   const world::MeshInstance3D& instance, const world::Affine3D& place,
                   const std::function<world::Affine3D(entt::entity)>& global,
                   render::ShadowCasting casting, float interpolation);
    // What a made mesh's surface is drawn with: its StandardMaterial3D, or its model's own
    // material, whose images that can't load are problems at the entity's model_field.
    render::Material3D surface_material(const SurfaceMaterial& from, entt::entity entity,
                                        const std::string& model_field);
    // A CSG root's result, drawn as Godot draws its root shape's mesh.
    void add_csg(const world::World& world, const world::Tree3D& tree, entt::entity root,
                 const world::Affine3D& place, std::uint32_t cull_mask);
    // A GridMap's octants, drawn as Godot draws their MultiMeshes.
    void add_gridmap(const world::World& world, entt::entity entity, const world::Affine3D& place,
                     std::uint32_t cull_mask);
    // An HTerrain's chunks for a camera at camera, as HTerrain draws them.
    void add_terrain(const world::World& world, entt::entity entity, const world::Affine3D& place,
                     const world::Affine3D& camera, const world::Camera3D& lens);
    // An image the renderer makes, such as a terrain's normal map, as a texture on the device,
    // made once per key.
    render::MaterialTexture kept_texture(const std::string& key, const render::Image& image);
    // A skinned surface's vertices moved by joints, in its entity's mesh on the device.
    Result<render::MeshId> skinned_mesh(const std::string& key, const ModelSurface& surface,
                                        const std::vector<world::Affine3D>& joints);

    // A mesh's place in a baked lightmap this frame, by its entity and model part.
    struct Lit {
        std::shared_ptr<const LightmapData> data;
        const LightmapUnwrap* unwrap = nullptr;
        std::string mesh; // the unwrap's hash
        std::string key;  // the lightmap file and its version
        render::LightmapShading shading;
        bool reported = false; // that its mesh changed since, this frame
    };
    // A lightmap whose light probes light moving meshes this frame: its LightmapGI's place, and
    // the camera's exposure over the one it was baked with.
    struct ProbeSource {
        std::shared_ptr<const LightmapData> data;
        world::Affine3D transform;
        float exposure = 1.0F;
    };
    // The light probes' light a moving mesh took, eased toward where it is as Godot's
    // lightmap_probe_update_speed eases it.
    struct Captured {
        std::array<std::array<float, 3>, 9> sh{};
        double time = 0.0;
        std::uint64_t used = 0;
    };
    struct KeptLightmap {
        SourceLightmap source;
        render::TextureId texture = render::TextureId::None;
        std::string problem; // why its texture couldn't be made
        std::uint64_t version = 0;
        std::uint64_t used = 0;
    };
    struct MeshHash {
        std::string hash;
        std::uint64_t used = 0;
    };
    // The source of a lit mesh: a key that stays while it does, and its vertices and indices
    // made as they were unwrapped (a model part's surfaces one after another).
    struct LitSource {
        std::string key;
        std::function<void(std::vector<render::Vertex3D>&, std::vector<std::uint32_t>&)> make;
    };
    // Loads the lightmaps the world's LightmapGIs name, makes their textures where they changed
    // and finds each lit mesh's place and the probes, for a camera of this exposure.
    void find_lightmaps(const world::World& world, float exposure,
                        const std::function<world::Affine3D(entt::entity)>& global);
    // The Decals the camera's cull_mask sees, in the order they were made, their textures read
    // through the image source and their emission times exposure. Those a device would refuse
    // are left out, as are those whose textures don't fit in the atlas with the ones before.
    void add_decals(const world::World& world, const world::Tree3D& tree,
                    const std::function<world::Affine3D(entt::entity)>& global,
                    std::uint32_t cull_mask, float exposure, render::Decals3D& out);
    // The FogVolumes the camera's cull_mask sees, in the order they were made, each with the
    // FogMaterial on its entity; past render::kMaxFogVolumes they are left out and reported.
    void add_fog_volumes(const world::World& world, const world::Tree3D& tree,
                         const std::function<world::Affine3D(entt::entity)>& global,
                         std::uint32_t cull_mask, std::vector<render::FogVolume3D>& out);
    // The light probes' light for the entity's moving mesh, centered at center in the world:
    // set at once the first frame, then eased toward it; nothing where no probes hold it.
    render::LightmapCapture capture(entt::entity entity, const Vec3& center);
    // The lightmap of the entity's part, and its triangles from first, count of them, as a mesh
    // on the device with their lightmap UVs, made once; nullptr when it has none, or its mesh
    // changed since the bake, with a problem. A model's surface (with_lods) has LODs made from
    // those, as Godot's importer makes them after the unwrap; lods is set to them.
    const render::LightmapShading* lit_surface(entt::entity entity, std::uint32_t part,
                                               const LitSource& source, std::uint32_t first,
                                               std::uint32_t count, render::MeshId& mesh,
                                               bool with_lods = false,
                                               const std::vector<SurfaceLod>** lods = nullptr);
    void destroy_lightmaps(bool unused_only);

    // A VoxelGI's cells and the light injected into them, as an atlas (voxel_gi.cpp).
    struct KeptVoxelGI;
    // The meshes a dynamic MeshInstance3D plots into the VoxelGIs it touches, and what they were
    // read from.
    struct VoxelObject;
    // Loads the cells of the world's VoxelGIs, injects the lights into those whose lights or
    // settings changed, plots the dynamic meshes in their boxes into their light where those or
    // it changed, puts their light in one atlas on the device and gives each draw the VoxelGIs its
    // bounds touch, at most two, and fog (when on) the first kMaxFogVoxelGIs, for a camera of
    // env's exposure.
    void trace_voxel_gis(const world::World& world, const world::Tree3D& tree,
                         const std::function<world::Affine3D(entt::entity)>& global,
                         const Environment& env, render::VolumetricFog3D& fog);
    // The entity's meshes as its VoxelGIs plot them, read again when its mesh, material, model or
    // their images change.
    const VoxelObject& voxel_object(const world::World& world, entt::entity entity);
    void destroy_voxel_gis();

    render::RenderDevice* device_;
    TextureSource textures_;
    ModelSource models_;
    LightmapSource lightmaps_;
    VoxelGISource voxel_gi_source_;
    ImageSource images_;
    std::unordered_map<entt::entity, std::unique_ptr<KeptVoxelGI>> voxel_gis_;
    std::unordered_map<entt::entity, std::unique_ptr<VoxelObject>> voxel_objects_;
    // The atlas of this frame's VoxelGIs' light, its size, and the VoxelGIs and versions in it.
    render::TextureId voxel_atlas_ = render::TextureId::None;
    std::uint32_t voxel_atlas_width_ = 0;
    std::uint32_t voxel_atlas_height_ = 0;
    std::vector<std::pair<entt::entity, std::uint64_t>> voxel_atlas_key_;
    std::uint64_t voxel_gi_versions_ = 0;
    std::unordered_map<std::string, KeptLightmap> kept_lightmaps_;
    std::map<std::pair<entt::entity, std::uint32_t>, Lit> lit_;
    std::unordered_map<std::string, MeshHash> mesh_hashes_;
    std::uint64_t lightmap_versions_ = 0;
    std::vector<ProbeSource> probe_sources_;
    std::unordered_map<entt::entity, Captured> captured_;
    // The middle of each model's bounds, by model and version, and the last frame that used it.
    std::unordered_map<std::string, std::pair<Vec3, std::uint64_t>> model_centers_;
    double capture_time_ = 0.0;
    std::unordered_map<std::string, Embedded> embedded_;
    std::unordered_map<entt::entity, Local> before_;
    std::unordered_map<entt::entity, PoseKey> before_poses_;
    std::unordered_map<std::string, Skinned> skinned_;
    std::vector<render::Vertex3D> skinned_vertices_;
    std::unordered_map<entt::entity, Cached> cached_;
    std::unordered_map<std::string, Shared> shared_;
    std::vector<Draw> draws_;
    std::size_t lods_drawn_ = 0;
    // Occlusion culling: the occluders' triangles, the depth buffer cast into them, each draw's
    // jitter timeout by its entity and place among the entity's draws, and how many it hid.
    OccluderScene occluder_scene_;
    OcclusionBuffer occlusion_buffer_;
    std::map<std::pair<entt::entity, std::size_t>, std::uint64_t> occlusion_timeouts_;
    std::size_t occluded_ = 0;
    // Where each entity's draws were drawn in the last frame with TAA, by the entity and the
    // draw's place among its draws.
    std::map<std::pair<entt::entity, std::size_t>, std::array<float, 16>> drawn_;
    std::vector<Problem> problems_;
    // The reflection probes' atlas, its cubes' size and count, and each probe's cube.
    render::TextureId atlas_ = render::TextureId::None;
    std::uint32_t atlas_size_ = 0;
    std::uint32_t atlas_cubes_ = 0;
    std::unordered_map<entt::entity, ProbeSlot> probe_slots_;
    std::uint64_t frame_ = 0;
    std::size_t lights_ = 0;
    // Auto exposure, as Godot's auto exposure version: the attributes it follows, the version of
    // its measure (0 for none) and the game time of the last frame.
    entt::entity exposure_entity_ = entt::null;
    std::uint64_t exposure_version_ = 0;
    double exposure_time_ = 0.0;
};

} // namespace slash::scene3d
