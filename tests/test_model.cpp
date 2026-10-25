#include "devices.hpp"
#include "fbx.hpp"
#include "gltf.hpp"
#include "jpeg.hpp"

#include "slash/animation/animation.hpp"
#include "slash/asset/assets.hpp"
#include "slash/canvas/canvas.hpp"
#include "slash/core/json.hpp"
#include "slash/core/math.hpp"
#include "slash/core/text.hpp"
#include "slash/render/device.hpp"
#include "slash/render/image.hpp"
#include "slash/scene3d/model.hpp"
#include "slash/scene3d/scene3d.hpp"
#include "slash/scene3d/skeleton.hpp"
#include "slash/ui/ui.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace asset = slash::asset;
namespace canvas = slash::canvas;
namespace json = slash::json;
namespace render = slash::render;
namespace scene3d = slash::scene3d;
namespace world = slash::world;
using slash::Vec3;
using slash::test::drawing_devices;
using slash::test::Fbx;
using slash::test::FbxNode;
using slash::test::Gltf;
using slash::test::quad;

namespace {

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE(parsed.ok());
    return std::move(parsed).value();
}

template <typename T>
std::string error_of(const slash::Result<T>& result) {
    return result.ok() ? "(succeeded)" : result.error().message;
}

// Files in memory, by project path.
struct MemoryFiles {
    std::map<std::string, std::string, std::less<>> files;

    [[nodiscard]] scene3d::ModelReader reader() const {
        return [this](std::string_view path) -> slash::Result<std::string> {
            const auto it = files.find(path);
            if (it == files.end()) {
                return slash::make_error(slash::ErrorCode::NotFound, "no such file");
            }
            return it->second;
        };
    }

    asset::FileSource source() {
        return {
            .read = reader(),
            .stamp = [this](std::string_view path) -> std::optional<asset::FileStamp> {
                const auto it = files.find(path);
                if (it == files.end()) {
                    return std::nullopt;
                }
                return asset::FileStamp{.modified = 1, .size = it->second.size()};
            },
        };
    }
};

slash::Result<scene3d::Model> load(const MemoryFiles& files, const std::string& path) {
    return scene3d::load_model(files.files.at(path), path, files.reader());
}

bool near(float a, float b) {
    return std::abs(a - b) < 1e-5F;
}

bool near(Vec3 a, Vec3 b) {
    return near(a.x, b.x) && near(a.y, b.y) && near(a.z, b.z);
}

template <std::size_t N>
bool near(const std::array<float, N>& a, const std::array<float, N>& b) {
    for (std::size_t i = 0; i < N; ++i) {
        if (!near(a[i], b[i])) {
            return false;
        }
    }
    return true;
}

Vec3 vec(const std::array<float, 3>& v) {
    return {v[0], v[1], v[2]};
}

Vec3 cross(Vec3 a, Vec3 b) {
    return {(a.y * b.z) - (a.z * b.y), (a.z * b.x) - (a.x * b.z), (a.x * b.y) - (a.y * b.x)};
}

float dot(Vec3 a, Vec3 b) {
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

// Whether every triangle winds clockwise seen from the side its corners' normals face, as
// Godot's front faces do.
bool clockwise(const scene3d::ModelSurface& surface) {
    for (std::size_t i = 0; i + 2 < surface.indices.size(); i += 3) {
        const render::Vertex3D& a = surface.vertices[surface.indices[i]];
        const Vec3 b = vec(surface.vertices[surface.indices[i + 1]].position);
        const Vec3 c = vec(surface.vertices[surface.indices[i + 2]].position);
        const Vec3 facing = cross(b - vec(a.position), c - vec(a.position));
        if (dot(facing, vec(a.normal)) >= 0.0F) {
            return false;
        }
    }
    return true;
}

// A 2x2 image: red and green over blue and white.
render::Image checker() {
    return {.width = 2,
            .height = 2,
            .pixels = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255}};
}

using Rgba = std::array<std::uint8_t, 4>;

Rgba pixel(const render::Image& image, std::uint32_t x, std::uint32_t y) {
    const std::size_t at = ((std::size_t{y} * image.width) + x) * 4;
    return {image.pixels[at], image.pixels[at + 1], image.pixels[at + 2], image.pixels[at + 3]};
}

// A world with the built-in components and the canvas asset types, in a viewport of 64x64,
// seeing the 3D world through a camera 3 units up +z over a blue background.
struct Scene {
    MemoryFiles files;
    asset::Assets assets{files.source()};
    world::ComponentRegistry components;
    world::World world{components};

    Scene() {
        world::register_builtin_components(components);
        canvas::install(world, assets);
        world.set_file_reader(files.reader());
        slash::ui::Settings ui;
        ui.viewport = {64.0F, 64.0F};
        slash::ui::set_settings(world, ui);
        create("camera", R"({"Transform3D": {"position": [0, 0, 3]}, "Camera3D": {}})");
        create("env", R"({"WorldEnvironment": {"background_mode": "color",
                                               "background_color": [0, 0, 1, 1]}})");
    }

    entt::entity create(std::string_view id, std::string_view components_json) {
        const entt::entity entity = world.create(id).value();
        const json::Value values = parse(components_json);
        for (const auto& [name, fields] : values.as_object()) {
            std::string path;
            const auto assigned = components.find(name)->assign(world, entity, fields, path);
            REQUIRE_MESSAGE(assigned.ok(), (assigned.ok() ? "" : assigned.error().message));
        }
        return entity;
    }
};

} // namespace

TEST_CASE("glTF models load as Godot's importer reads them") {
    MemoryFiles files;
    CHECK(scene3d::is_model_path("models/ship.gltf"));
    CHECK(scene3d::is_model_path("models/ship.glb"));
    CHECK(scene3d::is_model_path("models/ship.obj"));
    CHECK_FALSE(scene3d::is_model_path("models/ship.png"));

    Gltf model = quad(0);
    model.document["materials"] = parse(R"([
        {"name": "Glass", "doubleSided": true, "alphaMode": "BLEND", "emissiveFactor": [1, 0, 0],
         "pbrMetallicRoughness": {"baseColorFactor": [0.5, 0.2, 0, 0.5], "metallicFactor": 0.25}},
        {"alphaMode": "MASK", "alphaCutoff": 0.25, "extensions": {"KHR_materials_unlit": {}}}])");
    // The quad's node is the child of one moved 2 along x and scaled 2.
    model.document["nodes"] = parse(R"([
        {"name": "Root", "translation": [2, 0, 0], "scale": [2, 2, 2], "children": [1]},
        {"name": "Quad", "mesh": 0, "translation": [0, 1, 0]}])");
    files.files["models/quad.gltf"] = model.gltf();

    auto loaded = load(files, "models/quad.gltf");
    REQUIRE_MESSAGE(loaded.ok(), error_of(loaded));
    const scene3d::Model& m = loaded.value();
    REQUIRE(m.meshes.size() == 1);
    CHECK(m.meshes[0].name == "Quad");
    REQUIRE(m.meshes[0].surfaces.size() == 1);
    const scene3d::ModelSurface& surface = m.meshes[0].surfaces[0];
    CHECK(surface.material == 0);
    // The corners stay four, each triangle's last two swapped for Godot's clockwise winding.
    REQUIRE(surface.vertices.size() == 4);
    CHECK(surface.indices == std::vector<std::uint32_t>{0, 2, 1, 0, 3, 2});
    CHECK(clockwise(surface));
    CHECK(near(surface.vertices[0].uv, {0.0F, 1.0F}));
    for (const render::Vertex3D& v : surface.vertices) {
        CHECK(near(v.normal, {0.0F, 0.0F, 1.0F}));
        // MikkTSpace's tangent points along +u, as Godot's SurfaceTool makes it.
        CHECK(near(v.tangent, {1.0F, 0.0F, 0.0F, 1.0F}));
    }

    REQUIRE(m.materials.size() == 2);
    const world::StandardMaterial3D& glass = m.materials[0].standard;
    CHECK(m.materials[0].name == "Glass");
    // glTF colors are linear; Godot's are sRGB.
    CHECK(near(glass.albedo_color.x, 0.735357F));
    CHECK(near(glass.albedo_color.y, 0.484529F));
    CHECK(near(glass.albedo_color.z, 0.0F));
    CHECK(near(glass.albedo_color.w, 0.5F));
    CHECK(near(glass.metallic, 0.25F));
    CHECK(near(glass.roughness, 1.0F));
    CHECK(glass.cull_mode == "disabled");
    CHECK(glass.transparency == "alpha");
    CHECK(glass.emission_enabled);
    CHECK(near(glass.emission.x, 1.0F));
    CHECK(m.materials[0].albedo_image == -1);
    const world::StandardMaterial3D& cutout = m.materials[1].standard;
    CHECK(m.materials[1].name == "material_1");
    CHECK(cutout.shading_mode == "unshaded");
    CHECK(cutout.transparency == "alpha_scissor");
    CHECK(near(cutout.alpha_scissor_threshold, 0.25F));
    CHECK(near(cutout.metallic, 0.0F)); // no pbrMetallicRoughness: Godot's defaults
    CHECK(cutout.cull_mode == "back");

    REQUIRE(m.parts.size() == 1);
    CHECK(m.parts[0].node == "Quad");
    CHECK(m.parts[0].mesh == 0);
    CHECK(near(m.parts[0].transform.origin, {2.0F, 2.0F, 0.0F}));
    CHECK(near(m.parts[0].transform.x, {2.0F, 0.0F, 0.0F}));
    const scene3d::Bounds box = scene3d::bounds(m);
    CHECK(near(box.min, {0.0F, 0.0F, 0.0F}));
    CHECK(near(box.max, {4.0F, 4.0F, 0.0F}));
    CHECK(scene3d::model_bytes(m) > 4 * sizeof(render::Vertex3D));
}

TEST_CASE("glTF primitives without normals get flat ones, and corner colors color them") {
    MemoryFiles files;
    Gltf model;
    json::Value attributes;
    attributes["POSITION"] = model.floats(
        {0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 1.0F, 1.0F, 0.0F}, "VEC3");
    attributes["COLOR_0"] = model.floats(
        {1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 1.0F, 1.0F, 1.0F}, "VEC3");
    json::Value strip;
    strip["attributes"] = std::move(attributes);
    strip["mode"] = 5; // a triangle strip
    model.document["meshes"].push_back(
        json::Value(json::Object{{"primitives", json::Array{strip}}}));
    model.document["nodes"] = parse(R"([{"mesh": 0}])");
    files.files["strip.gltf"] = model.gltf();

    auto loaded = load(files, "strip.gltf");
    REQUIRE_MESSAGE(loaded.ok(), error_of(loaded));
    const scene3d::Model& m = loaded.value();
    const scene3d::ModelSurface& surface = m.meshes.at(0).surfaces.at(0);
    // Two triangles, each corner its own vertex for its face's normal.
    CHECK(surface.indices.size() == 6);
    CHECK(surface.vertices.size() == 6);
    CHECK(clockwise(surface));
    for (const render::Vertex3D& v : surface.vertices) {
        CHECK(near(v.normal, {0.0F, 0.0F, 1.0F}));
        // With no texture coordinates, Godot's stand-in tangent.
        CHECK(near(v.tangent, {0.0F, -1.0F, 0.0F, 1.0F}));
    }
    CHECK(surface.vertices[0].rgba == std::array<std::uint8_t, 4>{255, 0, 0, 255});
    // A primitive with no material and corner colors gets one that uses them as albedo.
    REQUIRE(m.materials.size() == 1);
    CHECK(surface.material == 0);
    CHECK(m.materials[0].standard.vertex_color_use_as_albedo);
    // With no scene, the nodes without parents are drawn; one without a name is named for its
    // mesh, as Godot's importer names it.
    REQUIRE(m.parts.size() == 1);
    CHECK(m.parts[0].node == "Mesh");
}

TEST_CASE("glTF files name buffers and images beside them, or carry them inside") {
    MemoryFiles files;
    const std::string png = render::encode_png(checker()).value();
    files.files["textures/checker.png"] = png;

    // A .gltf whose buffer and texture are files, named relative to it.
    Gltf beside = quad(0);
    beside.document["images"] = parse(R"([{"uri": "../textures/checker.png"}])");
    beside.document["samplers"] = parse(R"([{"magFilter": 9728, "minFilter": 9728,
                                             "wrapS": 33071, "wrapT": 10497}])");
    beside.document["textures"] = parse(R"([{"source": 0, "sampler": 0}])");
    beside.document["materials"] = parse(R"([{"pbrMetallicRoughness": {
        "baseColorTexture": {"index": 0, "extensions": {"KHR_texture_transform":
            {"offset": [0.5, 0], "scale": [2, 2]}}},
        "metallicRoughnessTexture": {"index": 0}},
        "normalTexture": {"index": 0, "scale": 0.5}, "emissiveTexture": {"index": 0}}])");
    files.files["models/quad.bin"] = beside.bin;
    files.files["models/quad.gltf"] = beside.gltf("quad.bin");
    auto loaded = load(files, "models/quad.gltf");
    REQUIRE_MESSAGE(loaded.ok(), error_of(loaded));
    REQUIRE(loaded.value().images.size() == 1);
    CHECK(loaded.value().images[0].path == "textures/checker.png");
    CHECK(loaded.value().images[0].image.pixels.empty());
    const scene3d::ModelMaterial& textured = loaded.value().materials.at(0);
    CHECK(textured.albedo_image == 0);
    CHECK(textured.metallic_image == 0);
    CHECK(textured.roughness_image == 0);
    CHECK(textured.normal_image == 0);
    CHECK(textured.emission_image == 0);
    CHECK(textured.standard.texture_filter == "nearest");
    CHECK_FALSE(textured.standard.texture_repeat); // only both wraps repeating repeat
    CHECK(near(textured.standard.uv1_offset, {0.5F, 0.0F, 0.0F}));
    CHECK(near(textured.standard.uv1_scale, {2.0F, 2.0F, 1.0F}));
    CHECK(textured.standard.metallic_texture_channel == "blue");
    CHECK(textured.standard.roughness_texture_channel == "green");
    CHECK(textured.standard.normal_enabled);
    CHECK(near(textured.standard.normal_scale, 0.5F));
    // An emission texture adds to black, as Godot's importer leaves emissiveFactor out.
    CHECK(textured.standard.emission_enabled);
    CHECK(near(textured.standard.emission.x, 0.0F));
    CHECK(scene3d::check_model(files.files["models/quad.gltf"], "models/quad.gltf", files.reader())
              .empty());

    // A .glb carries its buffer and images inside: PNG, and JPEG, which SDL can't read.
    Gltf inside = quad(0);
    inside.image(png, "image/png");
    inside.image(slash::test::make_jpeg(checker()), "image/jpeg");
    inside.document["textures"] = parse(R"([{"source": 1}])");
    inside.document["materials"] =
        parse(R"([{"pbrMetallicRoughness": {"baseColorTexture": {"index": 0}}}])");
    files.files["ship.glb"] = inside.glb();
    auto packed = load(files, "ship.glb");
    REQUIRE_MESSAGE(packed.ok(), error_of(packed));
    REQUIRE(packed.value().images.size() == 2);
    CHECK(packed.value().images[0].path.empty());
    CHECK(packed.value().images[0].image.pixels == checker().pixels);
    CHECK(packed.value().images[1].image.width == 2);
    CHECK(packed.value().materials.at(0).albedo_image == 1);
    CHECK(packed.value().materials.at(0).standard.texture_filter == "linear_with_mipmaps");
    CHECK(packed.value().materials.at(0).standard.texture_repeat);

    // A data URI image.
    Gltf uri = quad();
    uri.document["images"].push_back(
        json::Value(json::Object{{"uri", "data:image/png;base64," + slash::base64_encode(png)}}));
    files.files["uri.gltf"] = uri.gltf();
    auto embedded = load(files, "uri.gltf");
    REQUIRE_MESSAGE(embedded.ok(), error_of(embedded));
    CHECK(embedded.value().images.at(0).image.pixels == checker().pixels);
}

TEST_CASE("models that can't load say why, and check_model reports their files") {
    MemoryFiles files;
    const auto refused = [&](std::string_view path, const std::string& text) {
        files.files[std::string(path)] = text;
        auto loaded = load(files, std::string(path));
        REQUIRE_FALSE(loaded.ok());
        return loaded.error().message;
    };
    CHECK(refused("a.gltf", "this is not a model at all") == "its JSON doesn't parse");
    CHECK(refused("b.gltf", R"({"asset": {"version": "1.0"}})") ==
          "it is glTF 1.0; only glTF 2.0 loads");

    Gltf draco = quad();
    draco.document["extensionsUsed"] = parse(R"(["KHR_draco_mesh_compression"])");
    draco.document["extensionsRequired"] = parse(R"(["KHR_draco_mesh_compression"])");
    CHECK(refused("draco.gltf", draco.gltf()) ==
          "it requires the extension KHR_draco_mesh_compression, which isn't read; export it "
          "without that extension");

    Gltf missing = quad();
    CHECK(refused("models/missing.gltf", missing.gltf("missing.bin")) ==
          "its buffer \"models/missing.bin\" could not be read: no such file");
    CHECK(refused("models/climb.gltf", missing.gltf("../../outside.bin")) ==
          "a buffer it names, \"models/../../outside.bin\", is outside the project folder");

    Gltf picture = quad();
    picture.document["images"] = parse(R"([{"name": "Paint", "uri": "paint.gif"}])");
    CHECK(refused("models/paint.gltf", picture.gltf()) ==
          "image 0 (\"Paint\") is \"models/paint.gif\"; textures are .png, .bmp, .jpg, .jpeg or "
          ".ktx2 files");

    // The file loads, but its texture is missing: check_model finds it.
    picture.document["images"] = parse(R"([{"uri": "paint.png"}])");
    files.files["models/paint.gltf"] = picture.gltf();
    const auto errors =
        scene3d::check_model(files.files["models/paint.gltf"], "models/paint.gltf", files.reader());
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].path == "/images/0/uri");
    CHECK(errors[0].message == "\"models/paint.png\" could not be read: no such file");
}

TEST_CASE("MeshInstance3D draws a glTF model as it draws a primitive mesh") {
    Scene scene;
    // An unlit red quad, and the same quad textured with the checker.
    Gltf red = quad(0);
    red.document["materials"] =
        parse(R"([{"pbrMetallicRoughness": {"baseColorFactor": [1, 0, 0, 1]},
        "extensions": {"KHR_materials_unlit": {}}}])");
    scene.files.files["models/red.gltf"] = red.gltf();
    Gltf textured = quad(0);
    textured.image(render::encode_png(checker()).value(), "image/png");
    textured.document["samplers"] = parse(R"([{"magFilter": 9728, "minFilter": 9728}])");
    textured.document["textures"] = parse(R"([{"source": 0, "sampler": 0}])");
    textured.document["materials"] = parse(R"([{"pbrMetallicRoughness":
        {"baseColorTexture": {"index": 0}}, "extensions": {"KHR_materials_unlit": {}}}])");
    scene.files.files["models/checker.glb"] = textured.glb();

    const entt::entity model = scene.create(
        "model", R"({"Transform3D": {"position": [-1.2, 0, 0], "scale": [0.5, 0.5, 0.5]},
                                  "MeshInstance3D": {"mesh": "models/red.gltf"}})");
    scene.create("primitive",
                 R"({"Transform3D": {"position": [1.2, 0, 0], "scale": [0.5, 0.5, 0.5]},
        "MeshInstance3D": {}, "QuadMesh": {"size": [2, 2]},
        "StandardMaterial3D": {"shading_mode": "unshaded", "albedo_color": [1, 0, 0, 1]}})");
    scene.create("checker", R"({"Transform3D": {"position": [0, 1.2, 0], "scale": [0.5, 0.5, 0.5]},
                                "MeshInstance3D": {"mesh": "models/checker.glb"}})");
    CHECK(scene3d::check_meshes(scene.world).empty());

    const auto captured = canvas::capture(scene.world, scene.assets);
    REQUIRE_MESSAGE(captured.ok(), error_of(captured));
    const render::Image& image = captured.value().image;
    CHECK(captured.value().meshes == 3);
    // The model and the QuadMesh draw alike, either side of the middle.
    for (std::uint32_t y = 27; y < 38; y += 3) {
        for (std::uint32_t x = 10; x < 22; x += 3) {
            CAPTURE(x);
            CAPTURE(y);
            CHECK(pixel(image, x, y) == pixel(image, 63 - x, y));
        }
    }
    CHECK(pixel(image, 13, 32) == Rgba{255, 0, 0, 255});
    CHECK(pixel(image, 32, 32) == Rgba{0, 0, 255, 255}); // the background between them
    // The checker's texels, its top row at +y.
    CHECK(pixel(image, 29, 13) == Rgba{255, 0, 0, 255});
    CHECK(pixel(image, 35, 13) == Rgba{0, 255, 0, 255});
    CHECK(pixel(image, 29, 19) == Rgba{0, 0, 255, 255});
    CHECK(pixel(image, 35, 19) == Rgba{255, 255, 255, 255});

    for (const auto& device : drawing_devices()) {
        CAPTURE(render::backend_name(device->backend()));
        canvas::CanvasRenderer renderer(*device, scene.assets);
        auto target = device->create_texture(
            {.width = 64, .height = 64, .filter = render::Filter::Linear, .render_target = true});
        REQUIRE(target.ok());
        canvas::CanvasOptions options;
        options.target = target.value();
        options.width = 64.0F;
        options.height = 64.0F;
        REQUIRE(renderer.draw(scene.world, options).ok());
        CHECK(renderer.meshes_drawn() == 3);
        const auto drawn = device->read_texture(target.value());
        REQUIRE(drawn.ok());
        for (const auto& [x, y] : {std::pair{13U, 32U}, std::pair{50U, 32U}, std::pair{29U, 13U},
                                   std::pair{35U, 13U}, std::pair{29U, 19U}, std::pair{35U, 19U}}) {
            CAPTURE(x);
            CAPTURE(y);
            const Rgba want = pixel(image, x, y);
            const Rgba got = pixel(drawn.value(), x, y);
            for (std::size_t c = 0; c < 4; ++c) {
                CHECK(std::abs(int{got[c]} - int{want[c]}) <= 2);
            }
        }
        device->destroy_texture(target.value());
    }

    // Editing the file reloads the model, and a renderer that drew it draws the new version.
    const auto software = render::create_software_device();
    canvas::CanvasRenderer renderer(*software, scene.assets);
    auto target = software->create_texture(
        {.width = 64, .height = 64, .filter = render::Filter::Linear, .render_target = true});
    REQUIRE(target.ok());
    canvas::CanvasOptions options;
    options.target = target.value();
    options.width = 64.0F;
    options.height = 64.0F;
    const auto drawn_at = [&](std::uint32_t x, std::uint32_t y) {
        REQUIRE(renderer.draw(scene.world, options).ok());
        const auto drawn = software->read_texture(target.value());
        REQUIRE(drawn.ok());
        return pixel(drawn.value(), x, y);
    };
    CHECK(drawn_at(13, 32) == Rgba{255, 0, 0, 255});
    red.document["materials"] =
        parse(R"([{"name": "Yellow", "pbrMetallicRoughness": {"baseColorFactor": [1, 1, 0, 1]},
        "extensions": {"KHR_materials_unlit": {}}}])");
    scene.files.files["models/red.gltf"] = red.gltf();
    const auto changes = scene.assets.poll();
    REQUIRE(changes.size() == 1);
    CHECK(changes[0].path == "models/red.gltf");
    CHECK(drawn_at(13, 32) == Rgba{255, 255, 0, 255});
    software->destroy_texture(target.value());

    // A StandardMaterial3D on the entity takes the place of the model's materials.
    world::StandardMaterial3D override;
    override.shading_mode = "unshaded";
    override.albedo_color = {0.0F, 1.0F, 0.0F, 1.0F};
    scene.world.ecs().emplace<world::StandardMaterial3D>(model, override);
    const auto green = canvas::capture(scene.world, scene.assets);
    REQUIRE(green.ok());
    CHECK(pixel(green.value().image, 13, 32) == Rgba{0, 255, 0, 255});
}

TEST_CASE("models that can't draw are reported, in the world and in scene files") {
    Scene scene;
    scene.files.files["models/quad.gltf"] = quad().gltf();
    scene.create("both", R"({"Transform3D": {}, "BoxMesh": {},
                             "MeshInstance3D": {"mesh": "models/quad.gltf"}})");
    scene.create("dae", R"({"Transform3D": {}, "MeshInstance3D": {"mesh": "models/quad.dae"}})");
    scene.create("gone", R"({"Transform3D": {}, "MeshInstance3D": {"mesh": "models/gone.glb"}})");
    const auto problems = scene3d::check_meshes(scene.world);
    REQUIRE(problems.size() == 3);
    CHECK(problems[0].path == "/components/MeshInstance3D/mesh");
    CHECK(problems[0].message == "draws a model, so its BoxMesh is left out: remove one of them");
    CHECK(problems[1].message == "expected a .gltf, .glb, .obj, .fbx or .blend file");
    CHECK(problems[2].message == "\"models/gone.glb\" could not be read: no such file");

    // Drawing reports them too, and draws nothing for them.
    const auto device = render::create_null_device();
    canvas::CanvasRenderer renderer(*device, scene.assets);
    canvas::FrameContents contents;
    canvas::CanvasOptions options;
    options.contents = &contents;
    REQUIRE(renderer.draw(scene.world, options).ok());
    CHECK(renderer.meshes_drawn() == 0);
    REQUIRE(contents.problems.size() == 3);
    CHECK(contents.problems[2].path == "/components/MeshInstance3D/mesh");

    const auto errors = scene3d::check_meshes(scene.components, parse(R"({"format": 1,
        "entities": {"ship": {"components": {"Transform3D": {},
            "MeshInstance3D": {"mesh": "models/ship.gltf"}}}}})"),
                                              scene.files.reader());
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].path == "/entities/ship/components/MeshInstance3D/mesh");
}

namespace {

// The normals of a surface's vertices at a position.
std::vector<Vec3> normals_at(const scene3d::ModelSurface& surface, Vec3 at) {
    std::vector<Vec3> out;
    for (const render::Vertex3D& v : surface.vertices) {
        if (near(vec(v.position), at)) {
            out.push_back(vec(v.normal));
        }
    }
    return out;
}

bool has_normal(const std::vector<Vec3>& normals, Vec3 n) {
    for (const Vec3& each : normals) {
        if (near(each, n)) {
            return true;
        }
    }
    return false;
}

// A quad facing +z, from (-1, -1) to (1, 1), its texture coordinates and normal.
constexpr std::string_view kObjQuad = R"(v -1 -1 0
v 1 -1 0
v 1 1 0
v -1 1 0
vt 0 0
vt 1 0
vt 1 1
vt 0 1
vn 0 0 1
)";

} // namespace

TEST_CASE("OBJ models load as Godot's importer reads them as a scene") {
    MemoryFiles files;
    files.files["models/shapes.obj"] = std::string(kObjQuad) + R"(f 1/1/1 2/2/1 3/3/1 4/4/1
o Pentagon
# a vertex color after its position
v 0 0 1 1 0 0
v 1 0 1
v 1 1 1
v 0.5 1.5 1
v 0 1 1
f -5//1 -4//1 -3//1 -2//1 -1//1
)";
    auto loaded = load(files, "models/shapes.obj");
    REQUIRE_MESSAGE(loaded.ok(), error_of(loaded));
    const scene3d::Model& m = loaded.value();
    // Each object is a mesh, named by its "o" line, or "Mesh" before the first.
    REQUIRE(m.meshes.size() == 2);
    CHECK(m.meshes[0].name == "Mesh");
    CHECK(m.meshes[1].name == "Pentagon");
    REQUIRE(m.parts.size() == 2);
    CHECK(m.parts[1].node == "Pentagon");
    CHECK(m.parts[1].mesh == 1);
    CHECK(near(m.parts[1].transform.origin, {0.0F, 0.0F, 0.0F}));
    CHECK(m.materials.empty());
    CHECK(m.images.empty());

    // The quad: two triangles from the first corner, equal corners merged, clockwise as Godot's.
    REQUIRE(m.meshes[0].surfaces.size() == 1);
    const scene3d::ModelSurface& quad = m.meshes[0].surfaces[0];
    CHECK(quad.material == -1);
    REQUIRE(quad.vertices.size() == 4);
    CHECK(quad.indices == std::vector<std::uint32_t>{0, 2, 1, 0, 3, 2});
    CHECK(clockwise(quad));
    // OBJ's v runs up the texture, Godot's down it.
    CHECK(near(quad.vertices[0].uv, {0.0F, 1.0F}));
    for (const render::Vertex3D& v : quad.vertices) {
        CHECK(near(v.normal, {0.0F, 0.0F, 1.0F}));
        CHECK(near(v.tangent, {1.0F, 0.0F, 0.0F, 1.0F}));
        CHECK(v.rgba == std::array<std::uint8_t, 4>{255, 255, 255, 255});
    }

    // The pentagon: three triangles, its corners named back from the latest vertex.
    const scene3d::ModelSurface& pentagon = m.meshes[1].surfaces.at(0);
    CHECK(pentagon.indices.size() == 9);
    REQUIRE(pentagon.vertices.size() == 5);
    CHECK(clockwise(pentagon));
    for (const render::Vertex3D& v : pentagon.vertices) {
        // With no texture coordinates, Godot's stand-in tangent for the normal.
        CHECK(near(v.tangent, {0.0F, -1.0F, 0.0F, 1.0F}));
    }
    CHECK(near(pentagon.vertices[0].position, {0.0F, 0.0F, 1.0F}));
    CHECK(pentagon.vertices[0].rgba == std::array<std::uint8_t, 4>{255, 0, 0, 255});
    CHECK(pentagon.vertices[1].rgba == std::array<std::uint8_t, 4>{255, 255, 255, 255});

    const scene3d::Bounds box = scene3d::bounds(m);
    CHECK(near(box.min, {-1.0F, -1.0F, 0.0F}));
    CHECK(near(box.max, {1.0F, 1.5F, 1.0F}));
    CHECK(
        scene3d::check_model(files.files["models/shapes.obj"], "models/shapes.obj", files.reader())
            .empty());
}

TEST_CASE("models write as glTF files that load back as written") {
    MemoryFiles files;
    files.files["textures/wood grain.png"] = render::encode_png(checker()).value();
    scene3d::Model model;
    // An image beside the model, in another folder, and one inside it.
    model.images.push_back({.path = "textures/wood grain.png", .image = {}});
    model.images.push_back({.path = "", .image = checker()});

    scene3d::ModelMaterial wood;
    wood.name = "Wood";
    wood.standard.albedo_color = {0.8F, 0.5F, 0.25F, 1.0F};
    wood.standard.metallic = 0.25F;
    wood.standard.roughness = 0.75F;
    wood.standard.uv1_scale = {2.0F, 3.0F, 1.0F};
    wood.standard.uv1_offset = {0.5F, 0.25F, 0.0F};
    wood.standard.texture_filter = "nearest";
    wood.standard.texture_repeat = false;
    wood.standard.metallic_texture_channel = "blue";
    wood.standard.roughness_texture_channel = "green";
    wood.standard.normal_enabled = true;
    wood.standard.normal_scale = 0.5F;
    wood.standard.cull_mode = "disabled";
    wood.albedo_image = 0;
    wood.metallic_image = 1;
    wood.roughness_image = 1;
    wood.normal_image = 1;
    scene3d::ModelMaterial glow;
    glow.name = "Glow";
    glow.standard.albedo_color = {1.0F, 1.0F, 1.0F, 0.5F};
    glow.standard.emission_enabled = true;
    glow.standard.emission = {1.0F, 0.5F, 0.0F, 1.0F};
    glow.standard.emission_energy_multiplier = 4.0F;
    glow.standard.shading_mode = "unshaded";
    glow.standard.transparency = "alpha";
    glow.standard.vertex_color_use_as_albedo = true;
    // A material naming its texture by path, as the importers make them.
    scene3d::ModelMaterial cutout;
    cutout.name = "Cutout";
    cutout.standard.albedo_texture = "textures/wood grain.png";
    cutout.standard.transparency = "alpha_scissor";
    cutout.standard.alpha_scissor_threshold = 0.25F;
    model.materials = {wood, glow, cutout};

    // A board of two triangles, and a gem: a colored triangle in the glow and one of the cutout.
    const auto vertex = [](float x, float y, float z) {
        render::Vertex3D v;
        v.position = {x, y, z};
        v.normal = {0.0F, 0.0F, 1.0F};
        v.tangent = {1.0F, 0.0F, 0.0F, -1.0F};
        v.uv = {x, y};
        return v;
    };
    scene3d::ModelSurface board{
        .vertices = {vertex(0, 0, 0), vertex(1, 0, 0), vertex(1, 1, 0), vertex(0, 1, 0)},
        .indices = {0, 2, 1, 0, 3, 2},
        .material = 0,
        .weights = {}};
    scene3d::ModelSurface gem{.vertices = {vertex(0, 0, 1), vertex(1, 0, 1), vertex(0, 1, 1)},
                              .indices = {0, 2, 1},
                              .material = 1,
                              .weights = {}};
    gem.vertices[1].rgba = {255, 0, 0, 128};
    scene3d::ModelSurface facet = gem;
    facet.material = 2;
    facet.vertices[1].rgba = {255, 255, 255, 255};
    // A surface of more corners than 16-bit indices reach.
    scene3d::ModelSurface big;
    big.vertices.resize(70000, vertex(0, 0, 0));
    big.vertices.back() = vertex(2, 2, 2);
    big.indices = {0, 69999, 1};
    model.meshes = {{.name = "Board", .surfaces = {board}},
                    {.name = "Gem", .surfaces = {gem, facet}},
                    {.name = "Big", .surfaces = {big}},
                    {.name = "Empty", .surfaces = {}}};
    model.nodes = {{.name = "Root",
                    .parent = -1,
                    .position = {1.0F, 2.0F, 3.0F},
                    .rotation = {0.0F, 0.70710677F, 0.0F, 0.70710677F},
                    .scale = {2.0F, 2.0F, 2.0F},
                    .file_index = 0},
                   {.name = "Board",
                    .parent = 0,
                    .position = {},
                    .rotation = {},
                    .scale = {1.0F, 1.0F, 1.0F},
                    .file_index = 1},
                   {.name = "Gem",
                    .parent = 0,
                    .position = {0.0F, 1.0F, 0.0F},
                    .rotation = {},
                    .scale = {1.0F, 1.0F, 1.0F},
                    .file_index = 2}};
    const std::vector<world::Affine3D> globals = scene3d::node_globals(
        model, std::vector<scene3d::NodePose>{
                   {model.nodes[0].position, model.nodes[0].rotation, model.nodes[0].scale},
                   {},
                   {model.nodes[2].position, {}, {1.0F, 1.0F, 1.0F}}});
    model.parts = {{.node = "Board", .mesh = 0, .transform = globals[1], .node_index = 1},
                   {.node = "Gem", .mesh = 1, .transform = globals[2], .node_index = 2},
                   {.node = "Gem", .mesh = 2, .transform = globals[2], .node_index = 2},
                   {.node = "Root", .mesh = 3, .transform = globals[0], .node_index = 0}};

    auto written = scene3d::write_gltf(model, "models/props/board.gltf");
    REQUIRE_MESSAGE(written.ok(), error_of(written));
    const json::Value document = parse(written.value());
    // The value at a JSON Pointer of the file.
    const auto at = [&](std::string_view pointer) -> const json::Value& {
        const json::Value* value = &document;
        std::size_t start = 1;
        while (start <= pointer.size() && value != nullptr) {
            const std::size_t slash = std::min(pointer.find('/', start), pointer.size());
            const std::string key(pointer.substr(start, slash - start));
            value = value->is_array() ? &value->as_array().at(std::stoul(key)) : value->find(key);
            start = slash + 1;
        }
        REQUIRE(value != nullptr);
        return *value;
    };
    CHECK(at("/images/0/uri").as_string() == "../../textures/wood%20grain.png");
    CHECK(at("/images/1/uri").as_string().starts_with("data:image/png;base64,"));
    CHECK(at("/images").size() == 2); // the cutout's path is the wood's image
    CHECK(at("/extensionsUsed") ==
          parse(R"(["KHR_texture_transform", "KHR_materials_emissive_strength",
                    "KHR_materials_unlit"])"));
    CHECK(at("/buffers/0/uri").as_string().starts_with("data:application/octet-stream;base64,"));
    // A node has one mesh, so the gem's second goes on a child; the empty mesh is left out.
    CHECK(at("/meshes").size() == 3);
    CHECK(at("/nodes").size() == 4);
    CHECK(at("/nodes/2/children") == parse("[3]"));
    CHECK(at("/nodes/3/name").as_string() == "Gem_2");

    files.files["models/props/board.gltf"] = written.value();
    auto loaded = load(files, "models/props/board.gltf");
    REQUIRE_MESSAGE(loaded.ok(), error_of(loaded));
    const scene3d::Model& m = loaded.value();
    REQUIRE(m.images.size() == 2);
    CHECK(m.images[0].path == "textures/wood grain.png");
    CHECK(m.images[1].path.empty());
    CHECK(m.images[1].image.pixels == checker().pixels);

    REQUIRE(m.materials.size() == 3);
    const auto same_material = [](const scene3d::ModelMaterial& read,
                                  const scene3d::ModelMaterial& wrote) {
        // Colors go through linear and back.
        CHECK(near(read.standard.albedo_color.x, wrote.standard.albedo_color.x));
        CHECK(near(read.standard.albedo_color.y, wrote.standard.albedo_color.y));
        CHECK(near(read.standard.albedo_color.z, wrote.standard.albedo_color.z));
        CHECK(near(read.standard.emission.x, wrote.standard.emission.x));
        CHECK(near(read.standard.emission.y, wrote.standard.emission.y));
        world::StandardMaterial3D standard = read.standard;
        standard.albedo_color = wrote.standard.albedo_color;
        standard.emission = wrote.standard.emission;
        CHECK(standard == wrote.standard);
        CHECK(read.name == wrote.name);
        CHECK(read.albedo_image == wrote.albedo_image);
        CHECK(read.metallic_image == wrote.metallic_image);
        CHECK(read.roughness_image == wrote.roughness_image);
        CHECK(read.normal_image == wrote.normal_image);
        CHECK(read.emission_image == wrote.emission_image);
    };
    same_material(m.materials[0], wood);
    same_material(m.materials[1], glow);
    scene3d::ModelMaterial read_cutout = cutout;
    read_cutout.standard.albedo_texture.clear();
    read_cutout.albedo_image = 0;
    same_material(m.materials[2], read_cutout);

    // Every corner as written, and the triangles in Godot's order again.
    const auto same_surface = [](const scene3d::ModelSurface& read,
                                 const scene3d::ModelSurface& wrote) {
        CHECK(read.indices == wrote.indices);
        CHECK(read.material == wrote.material);
        REQUIRE(read.vertices.size() == wrote.vertices.size());
        std::size_t differ = 0;
        for (std::size_t i = 0; i < read.vertices.size(); ++i) {
            const render::Vertex3D& a = read.vertices[i];
            const render::Vertex3D& b = wrote.vertices[i];
            if (a.position != b.position || a.normal != b.normal || a.tangent != b.tangent ||
                a.uv != b.uv || a.rgba != b.rgba) {
                ++differ;
            }
        }
        CHECK(differ == 0);
    };
    REQUIRE(m.meshes.size() == 3);
    CHECK(m.meshes[0].name == "Board");
    same_surface(m.meshes[0].surfaces.at(0), board);
    same_surface(m.meshes[1].surfaces.at(0), gem);
    same_surface(m.meshes[1].surfaces.at(1), facet);
    same_surface(m.meshes[2].surfaces.at(0), big);

    // The nodes, and the parts where they put them.
    REQUIRE(m.nodes.size() == 4);
    CHECK(m.nodes[0].name == "Root");
    CHECK(near(m.nodes[0].position, {1.0F, 2.0F, 3.0F}));
    CHECK(m.nodes[2].parent == 0);
    CHECK(m.nodes[3].name == "Gem_2");
    CHECK(m.nodes[3].parent == 2);
    REQUIRE(m.parts.size() == 3);
    CHECK(m.parts[0].node == "Board");
    CHECK(near(m.parts[0].transform.origin, globals[1].origin));
    CHECK(near(m.parts[1].transform.x, globals[2].x));
    CHECK(m.parts[2].mesh == 2);
    CHECK(near(m.parts[2].transform.origin, globals[2].origin));

    // A model without nodes, as an OBJ model: a node for each part.
    files.files["models/shapes.obj"] = std::string(kObjQuad) + R"(f 1/1/1 2/2/1 3/3/1 4/4/1
o Pentagon
v 0 0 1
v 1 0 1
v 1 1 1
v 0.5 1.5 1
v 0 1 1
f -5//1 -4//1 -3//1 -2//1 -1//1
)";
    auto obj = load(files, "models/shapes.obj");
    REQUIRE(obj.ok());
    auto obj_gltf = scene3d::write_gltf(obj.value(), "models/shapes.gltf");
    REQUIRE(obj_gltf.ok());
    files.files["models/shapes.gltf"] = obj_gltf.value();
    auto again = load(files, "models/shapes.gltf");
    REQUIRE_MESSAGE(again.ok(), error_of(again));
    REQUIRE(again.value().parts.size() == 2);
    CHECK(again.value().parts[1].node == "Pentagon");
    CHECK(again.value().meshes.at(1).name == "Pentagon");
    same_surface(again.value().meshes[0].surfaces.at(0), obj.value().meshes[0].surfaces.at(0));
    same_surface(again.value().meshes[1].surfaces.at(0), obj.value().meshes[1].surfaces.at(0));
}

TEST_CASE("OBJ files without normals get smooth ones by Godot's smoothing groups") {
    MemoryFiles files;
    // Two faces at a right angle, sharing an edge: one facing +z, one -y.
    files.files["fold.obj"] = R"(v 0 0 0
v 1 0 0
v 0 1 0
v 0 0 1
f 1 2 3
f 1 2 4
s off
f 1 2 3
s 1
f 1 2 4
)";
    auto loaded = load(files, "fold.obj");
    REQUIRE_MESSAGE(loaded.ok(), error_of(loaded));
    const scene3d::ModelSurface& surface = loaded.value().meshes.at(0).surfaces.at(0);
    CHECK(clockwise(surface));
    const float h = std::sqrt(0.5F);
    // The first two faces are smooth: their shared corners take the sum of their normals.
    const std::vector<Vec3> origin = normals_at(surface, {0.0F, 0.0F, 0.0F});
    CHECK(origin.size() == 3);
    CHECK(has_normal(origin, {0.0F, -h, h}));
    // After "s off", a face's corners take its own normal.
    CHECK(has_normal(origin, {0.0F, 0.0F, 1.0F}));
    // "s" after "s off" starts a new group, smoothed apart from the first.
    CHECK(has_normal(origin, {0.0F, -1.0F, 0.0F}));
    const std::vector<Vec3> top = normals_at(surface, {0.0F, 1.0F, 0.0F});
    CHECK(top.size() == 1);
    CHECK(has_normal(top, {0.0F, 0.0F, 1.0F}));
    for (const render::Vertex3D& v : surface.vertices) {
        CHECK(near(v.tangent, {1.0F, 0.0F, 0.0F, 1.0F}));
    }
}

TEST_CASE("OBJ material libraries become StandardMaterial3Ds as Godot's importer makes them") {
    MemoryFiles files;
    files.files["models/crate.obj"] = "mtllib ../materials/crate.mtl\n" + std::string(kObjQuad) +
                                      R"(usemtl Wood
f 1/1/1 2/2/1 3/3/1
usemtl Glass
f 1/1/1 3/3/1 4/4/1
usemtl Missing
f 1/1/1 2/2/1 4/4/1
o Lid
f 1/1/1 2/2/1 3/3/1
usemtl Wood
f 1/1/1 3/3/1 4/4/1
)";
    files.files["materials/crate.mtl"] = R"(# As Blender writes them
newmtl Wood
Ns 250.000000
Ka 1.000000 1.000000 1.000000
Kd 0.800000 0.400000 0.200000
Ks 0.500000 0.500000 0.500000
map_Kd textures\wood.png
map_Ks spec.png
map_Ns rough.png
map_bump wood_normal.png

newmtl Glass
Kd 0.2 0.4 0.6
Ks 0.9 0.1 0.1
Ns 900
d 0.5

newmtl Smoke
Tr 0.25
map_Kd C:\Users\me\smoke.png
map_Ks smoke.tga
)";
    files.files["materials/textures/wood.png"] = render::encode_png(checker()).value();
    files.files["materials/spec.png"] = "not a png";

    auto loaded = load(files, "models/crate.obj");
    REQUIRE_MESSAGE(loaded.ok(), error_of(loaded));
    const scene3d::Model& m = loaded.value();
    // The materials the faces use, in the order they're first used.
    REQUIRE(m.materials.size() == 2);
    const scene3d::ModelMaterial& wood = m.materials[0];
    CHECK(wood.name == "Wood");
    CHECK(near(wood.standard.albedo_color.x, 0.8F));
    CHECK(near(wood.standard.albedo_color.y, 0.4F));
    CHECK(near(wood.standard.albedo_color.z, 0.2F));
    CHECK(near(wood.standard.albedo_color.w, 1.0F));
    // Godot's importer takes the metallic from the specular color, or the shininess after it.
    CHECK(near(wood.standard.metallic, 0.5F));
    CHECK(near(wood.standard.roughness, 1.0F));
    CHECK(wood.standard.normal_enabled);
    CHECK(wood.standard.transparency == "disabled");
    // Textures are named from the library's folder, backslashes read as slashes.
    REQUIRE(m.images.size() == 4);
    CHECK(m.images[static_cast<std::size_t>(wood.albedo_image)].path ==
          "materials/textures/wood.png");
    CHECK(m.images[static_cast<std::size_t>(wood.metallic_image)].path == "materials/spec.png");
    CHECK(m.images[static_cast<std::size_t>(wood.roughness_image)].path == "materials/rough.png");
    CHECK(m.images[static_cast<std::size_t>(wood.normal_image)].path ==
          "materials/wood_normal.png");
    CHECK(wood.standard.metallic_texture_channel == "red");
    CHECK(wood.emission_image == -1);
    const scene3d::ModelMaterial& glass = m.materials[1];
    CHECK(near(glass.standard.metallic, 0.1F));
    CHECK(near(glass.standard.albedo_color.w, 0.5F));
    CHECK(glass.standard.transparency == "alpha");
    CHECK(glass.albedo_image == -1);

    // Each usemtl starts a surface; a material the library lacks, and the faces of an object
    // before its first usemtl, take Godot's default material.
    REQUIRE(m.meshes.size() == 2);
    REQUIRE(m.meshes[0].surfaces.size() == 3);
    CHECK(m.meshes[0].surfaces[0].material == 0);
    CHECK(m.meshes[0].surfaces[1].material == 1);
    CHECK(m.meshes[0].surfaces[2].material == -1);
    CHECK(m.meshes[1].name == "Lid");
    REQUIRE(m.meshes[1].surfaces.size() == 2);
    CHECK(m.meshes[1].surfaces[0].material == -1);
    CHECK(m.meshes[1].surfaces[1].material == 0);

    // The model loads, but check_model finds the textures that can't draw.
    const auto errors =
        scene3d::check_model(files.files["models/crate.obj"], "models/crate.obj", files.reader());
    REQUIRE(errors.size() == 5);
    for (const auto& error : errors) {
        CHECK(error.path.empty());
    }
    CHECK(errors[0].message ==
          "material library \"materials/crate.mtl\" line 20: map_Kd names "
          "\"C:/Users/me/smoke.png\", which is outside the project folder; put it beside the "
          "model");
    CHECK(errors[1].message ==
          "material library \"materials/crate.mtl\" line 21: map_Ks names "
          "\"materials/smoke.tga\", which isn't a texture: textures are .png, .bmp, .jpg, .jpeg "
          "or .ktx2 files");
    CHECK(errors[2].message.starts_with("\"materials/spec.png\" doesn't decode: "));
    CHECK(errors[3].message == "\"materials/rough.png\" could not be read: no such file");
    CHECK(errors[4].message == "\"materials/wood_normal.png\" could not be read: no such file");
}

TEST_CASE("OBJ files that can't load say why, and libraries that can't are left out") {
    MemoryFiles files;
    const auto error_for = [&](std::string text) {
        files.files["bad.obj"] = std::move(text);
        return error_of(load(files, "bad.obj"));
    };
    CHECK(error_for("") == "it seems to be binary, corrupted or empty, not OBJ text");
    CHECK(error_for(std::string("\x64\x86\x02\x00", 4)) ==
          "it seems to be binary, corrupted or empty, not OBJ text");
    CHECK(error_for("v 1 2\n") == "line 1: v needs x, y and z");
    CHECK(error_for("vt 1\n") == "line 1: vt needs u and v");
    CHECK(error_for("v 0 0 0\nv 1 0 0\nf 1 2\n") == "line 3: f needs at least three corners");
    CHECK(error_for("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 5\n") ==
          "line 4: corner \"5\" names vertex 5, but 3 come before it");
    CHECK(error_for("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 0 1 2\n") ==
          "line 4: corner \"0\" names vertex 0, but 3 come before it");
    CHECK(error_for("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1/1 2/1 3/1\n") ==
          "line 4: corner \"2/1\" names texture coordinate 1, but 0 come before it");
    CHECK(error_for("v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nf 1/1 2 3\n") ==
          "line 5: its corners \"1/1\" and \"2\" don't have the same parts (v, v/vt, v/vt/vn "
          "or v//vn)");
    // A line ending in a backslash goes on in the next.
    files.files["long.obj"] = "v 0 0 \\\n 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
    auto joined = load(files, "long.obj");
    REQUIRE_MESSAGE(joined.ok(), error_of(joined));
    CHECK(joined.value().meshes.at(0).surfaces.at(0).vertices.size() == 3);
    CHECK(scene3d::check_model(files.files["bad.obj"], "bad.obj", files.reader()).at(0).message ==
          "line 5: its corners \"1/1\" and \"2\" don't have the same parts (v, v/vt, v/vt/vn or "
          "v//vn)");

    // A library that can't be read or is broken leaves its materials out, as Godot's importer.
    const std::string face = "v 0 0 0\nv 1 0 0\nv 0 1 0\nusemtl A\nf 1 2 3\n";
    files.files["models/broken.mtl"] = "Kd 1 0 0\nnewmtl A\n";
    files.files["models/libraries.obj"] =
        "mtllib broken.mtl\nmtllib gone.mtl\nmtllib ../../up.mtl\n" + face;
    auto loaded = load(files, "models/libraries.obj");
    REQUIRE_MESSAGE(loaded.ok(), error_of(loaded));
    CHECK(loaded.value().materials.empty());
    CHECK(loaded.value().meshes.at(0).surfaces.at(0).material == -1);
    const auto errors = scene3d::check_model(files.files["models/libraries.obj"],
                                             "models/libraries.obj", files.reader());
    REQUIRE(errors.size() == 3);
    CHECK(errors[0].message == "material library \"models/broken.mtl\" line 1: Kd comes before "
                               "any newmtl; its materials are left out");
    CHECK(errors[1].message == "line 2: mtllib names \"models/gone.mtl\", which could not be "
                               "read: no such file; its materials are left out");
    CHECK(errors[2].message == "line 3: mtllib names \"../../up.mtl\", which is outside the "
                               "project folder; its materials are left out");
}

TEST_CASE("MeshInstance3D draws an OBJ model as it draws the same glTF one") {
    Scene scene;
    const std::string png = render::encode_png(checker()).value();
    scene.files.files["models/checker.png"] = png;
    scene.files.files["models/checker.mtl"] = "newmtl Checker\nmap_Kd checker.png\n";
    scene.files.files["models/checker.obj"] = "mtllib checker.mtl\n" + std::string(kObjQuad) +
                                              "usemtl Checker\nf 1/1/1 2/2/1 3/3/1 4/4/1\n";
    slash::test::Gltf gltf = quad(0);
    gltf.document["images"] = parse(R"([{"uri": "checker.png"}])");
    gltf.document["textures"] = parse(R"([{"source": 0}])");
    gltf.document["materials"] = parse(R"([{"pbrMetallicRoughness":
        {"baseColorTexture": {"index": 0}, "metallicFactor": 0}}])");
    scene.files.files["models/checker.gltf"] = gltf.gltf();
    scene.create("sun", R"({"Transform3D": {}, "DirectionalLight3D": {}})");
    const entt::entity model = scene.create(
        "model", R"({"Transform3D": {}, "MeshInstance3D": {"mesh": "models/checker.obj"}})");
    CHECK(scene3d::check_meshes(scene.world).empty());

    const auto obj = canvas::capture(scene.world, scene.assets);
    REQUIRE_MESSAGE(obj.ok(), error_of(obj));
    CHECK(obj.value().meshes == 1);
    scene.world.ecs().get<world::MeshInstance3D>(model).mesh = "models/checker.gltf";
    const auto gltf_drawn = canvas::capture(scene.world, scene.assets);
    REQUIRE_MESSAGE(gltf_drawn.ok(), error_of(gltf_drawn));
    const render::Image& a = obj.value().image;
    const render::Image& b = gltf_drawn.value().image;
    REQUIRE(a.pixels.size() == b.pixels.size());
    CHECK(a.pixels == b.pixels);
    // The checker's red and green texels, lit by the sun, top left and top right.
    const Rgba red = pixel(a, 22, 22);
    const Rgba green = pixel(a, 42, 22);
    CHECK(red[0] > green[0] + 40);
    CHECK(green[1] > red[1] + 40);
}

namespace {

// A quad from (-1, -1) to (1, 1) in x and y, facing +z, as exporters write one: its corners
// counter-clockwise seen from the front, with texture coordinates up the texture. Its node,
// "Quad", is under the scene's root.
Fbx fbx_quad(std::vector<std::int32_t> materials = {}) {
    Fbx out;
    out.geometry(10, "QuadMesh", {-1.0, -1.0, 0.0, 1.0, -1.0, 0.0, 1.0, 1.0, 0.0, -1.0, 1.0, 0.0},
                 {0, 1, 2, -4}, {}, {0.0, 0.0, 1.0, 0.0, 1.0, 1.0, 0.0, 1.0}, {},
                 std::move(materials));
    out.model(20, "Quad");
    out.connect(20, 0);
    out.connect(10, 20);
    return out;
}

} // namespace

TEST_CASE("FBX models load as Godot's importer reads them, in meters with y up") {
    // As Blender writes one: z up and centimeters, a mesh under an empty node.
    Fbx file;
    file.up_axis = 2;
    file.front_axis = 1;
    file.front_sign = -1;
    file.unit_scale = 1.0;
    file.geometry(10, "PanelMesh",
                  {-100.0, -100.0, 0.0, 100.0, -100.0, 0.0, 100.0, 100.0, 0.0, -100.0, 100.0, 0.0},
                  {0, 1, 2, -4}, {}, {0.0, 0.0, 1.0, 0.0, 1.0, 1.0, 0.0, 1.0});
    file.model(20, "Base", {100.0, 0.0, 0.0});
    file.model(21, "Panel", {0.0, 0.0, 50.0});
    FbxNode& red = file.material(30, "Red");
    slash::test::fbx_property(red, "DiffuseColor", "Color", {0.5, 0.25, 1.0});
    file.connect(20, 0);
    file.connect(21, 20);
    file.connect(10, 21);
    file.connect(30, 21);
    for (const bool binary : {false, true}) {
        CAPTURE(binary);
        MemoryFiles files;
        files.files["models/panel.fbx"] = binary ? file.binary() : file.ascii();
        auto loaded = load(files, "models/panel.fbx");
        REQUIRE_MESSAGE(loaded.ok(), error_of(loaded));
        const scene3d::Model& m = loaded.value();
        REQUIRE(m.meshes.size() == 1);
        CHECK(m.meshes[0].name == "PanelMesh");
        // Only nodes with a mesh are parts, placed by the nodes above them, turned to y up.
        REQUIRE(m.parts.size() == 1);
        CHECK(m.parts[0].node == "Panel");
        CHECK(m.parts[0].mesh == 0);
        CHECK(near(m.parts[0].transform.origin, {1.0F, 0.5F, 0.0F}));
        CHECK(near(m.parts[0].transform.basis({0.0F, 0.0F, 1.0F}), {0.0F, 1.0F, 0.0F}));

        const scene3d::ModelSurface& surface = m.meshes[0].surfaces.at(0);
        CHECK(surface.material == 0);
        REQUIRE(surface.vertices.size() == 4);
        CHECK(surface.indices.size() == 6);
        CHECK(clockwise(surface));
        // Meters; normals made for a mesh without them; the texture's v runs down, as Godot's.
        CHECK(near(surface.vertices[0].position, {-1.0F, -1.0F, 0.0F}));
        CHECK(near(surface.vertices[0].uv, {0.0F, 1.0F}));
        for (const render::Vertex3D& v : surface.vertices) {
            CHECK(near(v.normal, {0.0F, 0.0F, 1.0F}));
            CHECK(near(v.tangent, {1.0F, 0.0F, 0.0F, 1.0F}));
        }
        const scene3d::Bounds box = scene3d::bounds(m);
        CHECK(near(box.min, {0.0F, 0.5F, -1.0F}));
        CHECK(near(box.max, {2.0F, 0.5F, 1.0F}));

        // A Lambert material: its diffuse color, from linear to sRGB, and corner colors shown.
        REQUIRE(m.materials.size() == 1);
        CHECK(m.materials[0].name == "Red");
        const world::StandardMaterial3D& standard = m.materials[0].standard;
        CHECK(near(standard.albedo_color.x, 0.735357F));
        CHECK(near(standard.albedo_color.y, 0.537099F));
        CHECK(near(standard.albedo_color.z, 1.0F));
        CHECK(standard.vertex_color_use_as_albedo);
        CHECK(standard.metallic == 0.0F);
        CHECK(standard.roughness == 1.0F);
        CHECK_FALSE(standard.emission_enabled);
        CHECK(scene3d::check_model(files.files["models/panel.fbx"], "models/panel.fbx",
                                   files.reader())
                  .empty());
    }
}

TEST_CASE("FBX meshes keep their normals, tangents, colors and a surface per material") {
    Fbx file;
    // Two quads side by side, the second facing -z, each of its own material; the first's
    // corners are colored. Normals and tangents are the file's own.
    file.geometry(10, "Pair", {-1.0, -1.0, 0.0, 1.0, -1.0, 0.0, 1.0, 1.0, 0.0, -1.0, 1.0, 0.0,
                               1.0,  -1.0, 0.0, 3.0, -1.0, 0.0, 3.0, 1.0, 0.0, 1.0,  1.0, 0.0},
                  {0, 1, 2, -4, 4, 7, 6, -6},
                  {0.0, 0.0, 1.0,  0.0, 0.0, 1.0,  0.0, 0.0, 1.0,  0.0, 0.0, 1.0,
                   0.0, 0.0, -1.0, 0.0, 0.0, -1.0, 0.0, 0.0, -1.0, 0.0, 0.0, -1.0},
                  {},
                  {1.0, 0.0, 0.0, 1.0, 1.0, 0.0, 0.0, 1.0, 1.0, 0.0, 0.0, 1.0, 1.0, 0.0, 0.0, 1.0,
                   0.0, 0.0, 1.0, 0.5, 0.0, 0.0, 1.0, 0.5, 0.0, 0.0, 1.0, 0.5, 0.0, 0.0, 1.0, 0.5},
                  {0, 1});
    file.model(20, "Pair");
    file.material(30, "Front");
    file.material(31, "Back");
    file.connect(20, 0);
    file.connect(10, 20);
    file.connect(30, 20);
    file.connect(31, 20);
    MemoryFiles files;
    files.files["pair.fbx"] = file.ascii();
    auto loaded = load(files, "pair.fbx");
    REQUIRE_MESSAGE(loaded.ok(), error_of(loaded));
    const scene3d::ModelMesh& mesh = loaded.value().meshes.at(0);
    REQUIRE(mesh.surfaces.size() == 2);
    const scene3d::ModelSurface& front = mesh.surfaces[0];
    const scene3d::ModelSurface& back = mesh.surfaces[1];
    CHECK(front.material == 0);
    CHECK(back.material == 1);
    CHECK(clockwise(front));
    CHECK(clockwise(back));
    for (const render::Vertex3D& v : front.vertices) {
        CHECK(near(v.normal, {0.0F, 0.0F, 1.0F}));
        CHECK(v.rgba == std::array<std::uint8_t, 4>{255, 0, 0, 255});
        // No texture coordinates, so Godot's stand-in tangent for the normal.
        CHECK(near(v.tangent, {0.0F, -1.0F, 0.0F, 1.0F}));
    }
    for (const render::Vertex3D& v : back.vertices) {
        CHECK(near(v.normal, {0.0F, 0.0F, -1.0F}));
        CHECK(v.rgba == std::array<std::uint8_t, 4>{0, 0, 255, 128});
    }
    CHECK(loaded.value().materials.size() == 2);
}

TEST_CASE("FBX materials become StandardMaterial3Ds as Godot's importer makes them") {
    Fbx file = fbx_quad({0});
    // Phong: the diffuse color times its factor, and emission.
    FbxNode& glow = file.material(30, "Glow", "phong");
    slash::test::fbx_property(glow, "DiffuseColor", "Color", {0.5, 0.25, 1.0});
    slash::test::fbx_property(glow, "DiffuseFactor", "Number", {0.5});
    slash::test::fbx_property(glow, "EmissiveColor", "Color", {1.0, 0.5, 0.0});
    slash::test::fbx_property(glow, "EmissiveFactor", "Number", {2.0});
    // A diffuse texture, clamped in u, moved and scaled.
    FbxNode& wood = file.material(31, "Wood");
    slash::test::fbx_property(wood, "DiffuseFactor", "Number", {0.5});
    FbxNode& texture = file.texture(40, "WoodTexture", "C:/art/wood.png", "wood.png");
    FbxNode& settings = texture.add("Properties70");
    slash::test::fbx_property(settings, "WrapModeU", "enum", {std::int64_t{1}});
    slash::test::fbx_property(settings, "Translation", "Vector", {0.25, 0.5, 0.0});
    slash::test::fbx_property(settings, "Scaling", "Vector", {2.0, 3.0, 1.0});
    // 3ds Max's PBR material: metalness and roughness, a roughness texture and a normal map.
    FbxNode& metal = file.material(32, "Metal", "unknown");
    slash::test::fbx_property(metal, "3dsMax|ClassIDa", "int",
                              {std::int64_t{static_cast<std::int32_t>(0xd00f1e00U)}});
    slash::test::fbx_property(metal, "3dsMax|ClassIDb", "int",
                              {std::int64_t{static_cast<std::int32_t>(0xbe77e500U)}});
    slash::test::fbx_property(metal, "3dsMax|main|base_color", "ColorAndAlpha",
                              {0.25, 0.5, 0.75, 1.0});
    slash::test::fbx_property(metal, "3dsMax|main|metalness", "Float", {0.25});
    slash::test::fbx_property(metal, "3dsMax|main|roughness", "Float", {0.75});
    file.texture(41, "Rough", "rough.png", "rough.png");
    file.texture(42, "Bumps", "bumps.png", "bumps.png");
    for (const std::int64_t id : {30, 31, 32}) {
        file.connect(id, 20);
    }
    file.connect(40, 31, "DiffuseColor");
    file.connect(41, 32, "3dsMax|main|roughness_map");
    file.connect(42, 32, "3dsMax|main|norm_map");

    MemoryFiles files;
    files.files["models/quad.fbx"] = file.ascii();
    // The wood has see-through texels, and pixels only fully opaque or clear.
    render::Image cutout = checker();
    cutout.pixels[3] = 0;
    files.files["models/textures/wood.png"] = render::encode_png(cutout).value();
    files.files["models/rough.png"] = render::encode_png(checker()).value();
    files.files["models/bumps.png"] = render::encode_png(checker()).value();
    auto loaded = load(files, "models/quad.fbx");
    REQUIRE_MESSAGE(loaded.ok(), error_of(loaded));
    const scene3d::Model& m = loaded.value();
    REQUIRE(m.materials.size() == 3);

    const world::StandardMaterial3D& g = m.materials[0].standard;
    CHECK(near(g.albedo_color.x, 0.537099F));
    CHECK(near(g.albedo_color.y, 0.388573F));
    CHECK(near(g.albedo_color.z, 0.735357F));
    CHECK(g.emission_enabled);
    CHECK(near(g.emission.x, 1.0F));
    CHECK(near(g.emission.y, 0.735357F));
    CHECK(near(g.emission.z, 0.0F));
    CHECK(g.emission_energy_multiplier == 2.0F);
    CHECK(m.materials[0].albedo_image == -1);

    // The texture, found in the textures folder beside the model as Godot looks for it. Its
    // texels set the colors, times the factor (alpha too), and its clear ones a cutout.
    const scene3d::ModelMaterial& w = m.materials[1];
    REQUIRE(w.albedo_image >= 0);
    CHECK(m.images.at(static_cast<std::size_t>(w.albedo_image)).path == "models/textures/wood.png");
    CHECK(near(w.standard.albedo_color.x, 0.735357F));
    CHECK(near(w.standard.albedo_color.w, 0.5F));
    CHECK(w.standard.transparency == "alpha_scissor");
    CHECK_FALSE(w.standard.texture_repeat);
    CHECK(near(w.standard.uv1_offset, {0.25F, 0.5F, 0.0F}));
    CHECK(near(w.standard.uv1_scale, {2.0F, 3.0F, 1.0F}));

    // PBR: metalness and roughness, the roughness from its texture's red channel, and the normal
    // map. A four-part color takes its alpha from its third part, as Godot's importer does.
    const scene3d::ModelMaterial& p = m.materials[2];
    CHECK(near(p.standard.albedo_color.x, 0.537099F));
    CHECK(near(p.standard.albedo_color.w, 0.75F));
    CHECK(p.standard.metallic == 0.25F);
    CHECK(p.standard.roughness == 1.0F);
    CHECK(p.standard.roughness_texture_channel == "red");
    REQUIRE(p.roughness_image >= 0);
    CHECK(m.images.at(static_cast<std::size_t>(p.roughness_image)).path == "models/rough.png");
    CHECK(p.metallic_image == -1);
    CHECK(p.standard.normal_enabled);
    REQUIRE(p.normal_image >= 0);
    CHECK(m.images.at(static_cast<std::size_t>(p.normal_image)).path == "models/bumps.png");
    CHECK(scene3d::check_model(files.files["models/quad.fbx"], "models/quad.fbx", files.reader())
              .empty());
}

TEST_CASE("FBX textures are found where Godot's importer looks, and those that aren't reported") {
    Fbx file = fbx_quad({0});
    FbxNode& material = file.material(30, "Many");
    (void)material;
    file.connect(30, 20);
    const std::vector<std::pair<std::string, std::string>> textures{
        {"carried.png", "carried.png"},           // inside the file
        {"/home/art/up.png", "../shared/up.png"}, // relative to the model
        {"C:\\art\\metal.png", ""},               // in Textures/ at the project's top
        {"gone.png", "gone.png"},                 // nowhere
        {"bark.tga", "bark.tga"},                 // not a texture SlashEngine reads
        {"broken.png", "broken.png"},             // inside the file, and doesn't decode
    };
    const std::array<const char*, 6> slots{"DiffuseColor",     "NormalMap",     "EmissiveColor",
                                           "TransparentColor", "SpecularColor", "Bump"};
    for (std::size_t i = 0; i < textures.size(); ++i) {
        const auto id = static_cast<std::int64_t>(40 + i);
        file.texture(id, "T" + std::to_string(i), textures[i].first, textures[i].second);
        file.connect(id, 30, slots[i]);
    }
    file.video(50, 40, "carried.png", render::encode_png(checker()).value());
    file.video(55, 45, "broken.png", "not a PNG");
    MemoryFiles files;
    files.files["models/many.fbx"] = file.binary();
    files.files["shared/up.png"] = render::encode_png(checker()).value();
    files.files["Textures/metal.png"] = render::encode_png(checker()).value();
    files.files["models/bark.tga"] = "a TGA file";
    auto loaded = load(files, "models/many.fbx");
    REQUIRE_MESSAGE(loaded.ok(), error_of(loaded));
    const scene3d::Model& m = loaded.value();
    REQUIRE(m.images.size() == 3);
    CHECK(m.images[0].path.empty());
    CHECK(m.images[0].image.width == 2);
    CHECK(m.images[1].path == "shared/up.png");
    CHECK(m.images[2].path == "Textures/metal.png");
    const scene3d::ModelMaterial& many = m.materials.at(0);
    CHECK(many.albedo_image == 0);
    CHECK(many.normal_image == 1);
    CHECK(many.emission_image == 2);

    const auto errors =
        scene3d::check_model(files.files["models/many.fbx"], "models/many.fbx", files.reader());
    REQUIRE(errors.size() == 3);
    CHECK(errors[0].message == "texture \"gone.png\" isn't beside the model or in a folder Godot "
                               "looks in; materials draw without it");
    CHECK(errors[1].message == "texture \"bark.tga\" isn't a texture SlashEngine reads (.png, "
                               ".bmp, .jpg, .jpeg or .ktx2); materials draw without it");
    CHECK(errors[2].message.starts_with("the image it carries as \"broken.png\" doesn't decode: "));
}

TEST_CASE("FBX files that can't load say why, and warnings are reported") {
    MemoryFiles files;
    const auto error_for = [&](std::string bytes) {
        files.files["bad.fbx"] = std::move(bytes);
        return error_of(load(files, "bad.fbx"));
    };
    CHECK(error_for("") == "it doesn't read as FBX: Empty file");
    const std::string binary = fbx_quad().binary();
    CHECK(error_for(binary.substr(0, 100)) == "it doesn't read as FBX: Truncated file");
    // OBJ text read as FBX, as a file named .fbx is.
    CHECK(error_for(std::string(kObjQuad)).starts_with("it doesn't read as FBX: "));

    // A name that isn't UTF-8 loads, with the warning ufbx gives.
    Fbx file = fbx_quad({0});
    file.material(30, "Bad\xff");
    file.connect(30, 20);
    files.files["warned.fbx"] = file.binary();
    auto loaded = load(files, "warned.fbx");
    REQUIRE_MESSAGE(loaded.ok(), error_of(loaded));
    const auto errors =
        scene3d::check_model(files.files["warned.fbx"], "warned.fbx", files.reader());
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].message.starts_with("warning in \"Bad"));
    CHECK(errors[0].message.ends_with(": Bad UTF-8 string"));
}

TEST_CASE("MeshInstance3D draws an FBX model as it draws the same OBJ one") {
    Scene scene;
    const std::string png = render::encode_png(checker()).value();
    scene.files.files["models/checker.png"] = png;
    scene.files.files["models/checker.mtl"] = "newmtl Checker\nmap_Kd checker.png\n";
    scene.files.files["models/checker.obj"] = "mtllib checker.mtl\n" + std::string(kObjQuad) +
                                              "usemtl Checker\nf 1/1/1 2/2/1 3/3/1 4/4/1\n";
    Fbx file = fbx_quad({0});
    file.material(30, "Checker");
    file.texture(40, "CheckerTexture", "checker.png", "checker.png");
    file.connect(30, 20);
    file.connect(40, 30, "DiffuseColor");
    scene.files.files["models/checker.fbx"] = file.binary();
    scene.create("sun", R"({"Transform3D": {}, "DirectionalLight3D": {}})");
    const entt::entity model = scene.create(
        "model", R"({"Transform3D": {}, "MeshInstance3D": {"mesh": "models/checker.fbx"}})");
    CHECK(scene3d::check_meshes(scene.world).empty());

    const auto fbx = canvas::capture(scene.world, scene.assets);
    REQUIRE_MESSAGE(fbx.ok(), error_of(fbx));
    CHECK(fbx.value().meshes == 1);
    scene.world.ecs().get<world::MeshInstance3D>(model).mesh = "models/checker.obj";
    const auto obj = canvas::capture(scene.world, scene.assets);
    REQUIRE_MESSAGE(obj.ok(), error_of(obj));
    const render::Image& a = fbx.value().image;
    const render::Image& b = obj.value().image;
    REQUIRE(a.pixels.size() == b.pixels.size());
    CHECK(a.pixels == b.pixels);
}

TEST_CASE("glTF skins, bones and animations load as Godot's importer reads them") {
    MemoryFiles files;
    files.files["arm.gltf"] = slash::test::arm().gltf();
    const auto loaded = load(files, "arm.gltf");
    REQUIRE_MESSAGE(loaded.ok(), error_of(loaded));
    const scene3d::Model& model = loaded.value();

    // Every node, parents first, as Godot's bones are.
    REQUIRE(model.nodes.size() == 4);
    std::vector<std::string> names;
    std::vector<int> parents;
    for (const scene3d::ModelNode& node : model.nodes) {
        names.push_back(node.name);
        parents.push_back(node.parent);
    }
    CHECK(names == std::vector<std::string>{"Armature", "Upper", "Lower", "Arm"});
    CHECK(parents == std::vector<int>{-1, 0, 1, 0});
    CHECK(near(model.nodes[2].position, {0.0F, 1.0F, 0.0F}));
    CHECK(scene3d::find_node(model, "Lower") == 2);
    CHECK(scene3d::find_node(model, "Hand") == -1);

    REQUIRE(model.skins.size() == 1);
    CHECK(model.skins[0].name == "ArmSkin");
    CHECK(model.skins[0].joints == std::vector<std::size_t>{1, 2});
    CHECK(near(model.skins[0].inverse_binds[1].origin, {0.0F, -1.0F, 0.0F}));
    REQUIRE(model.parts.size() == 1);
    CHECK(model.parts[0].skin == 0);
    CHECK(model.parts[0].node_index == 3);

    // Each vertex's joints and weights; the middle row is half each bone's.
    const scene3d::ModelSurface& surface = model.meshes[0].surfaces[0];
    REQUIRE(surface.weights.size() == surface.vertices.size());
    for (std::size_t i = 0; i < surface.vertices.size(); ++i) {
        CAPTURE(i);
        const float y = surface.vertices[i].position[1];
        const scene3d::VertexWeights& weights = surface.weights[i];
        if (y < 0.5F) {
            CHECK(weights.weights[0] == 1.0F);
            CHECK(weights.joints[0] == 0);
        } else if (y < 1.5F) {
            CHECK(weights.weights[0] == 0.5F);
            CHECK(weights.weights[1] == 0.5F);
        } else {
            CHECK(weights.weights[0] == 1.0F);
            CHECK(weights.joints[0] == 1);
        }
    }

    // Animations as long as their last key; a "-loop" name loops, as Godot's import hint.
    REQUIRE(model.animations.size() == 2);
    CHECK(model.animations[0].name == "Bend");
    CHECK(model.animations[0].length == doctest::Approx(1.0));
    CHECK_FALSE(model.animations[0].loop);
    REQUIRE(model.animations[0].tracks.size() == 1);
    CHECK(model.animations[0].tracks[0].node == 2);
    CHECK(model.animations[0].tracks[0].property == scene3d::NodeProperty::Rotation);
    CHECK(model.animations[1].name == "Idle-loop");
    CHECK(model.animations[1].loop);
    // The skinned mesh's bounds are where its vertices are at rest.
    const scene3d::Bounds box = scene3d::bounds(model);
    CHECK(near(box.min, {-0.5F, 0.0F, 0.0F}));
    CHECK(near(box.max, {0.5F, 2.0F, 0.0F}));

    // Names Godot can't take in a node path are changed as its importer changes them, and names
    // made unique; nodes without names are named for what they are.
    slash::test::Gltf named = quad();
    named.document["nodes"] = parse(R"([{"name": "a.b:c", "children": [1, 2, 3]},
        {"name": "Arm"}, {"name": "Arm"}, {"mesh": 0}, {}])");
    named.document["scenes"] = parse(R"([{"nodes": [0, 4]}])");
    files.files["named.gltf"] = named.gltf();
    const auto renamed = load(files, "named.gltf");
    REQUIRE_MESSAGE(renamed.ok(), error_of(renamed));
    names.clear();
    for (const scene3d::ModelNode& node : renamed.value().nodes) {
        names.push_back(node.name);
    }
    CHECK(names == std::vector<std::string>{"a_b_c", "Arm", "Arm2", "Mesh", "Node"});
}

TEST_CASE("skins move vertices as Godot's skeleton shader, posed by animations and bones") {
    MemoryFiles files;
    files.files["arm.gltf"] = slash::test::arm().gltf();
    const auto loaded = load(files, "arm.gltf");
    REQUIRE(loaded.ok());
    const scene3d::Model& model = loaded.value();
    const scene3d::ModelSurface& surface = model.meshes[0].surfaces[0];
    const scene3d::ModelAnimation& bend = model.animations[0];

    // Where each vertex goes in a pose, by its position at rest.
    const auto skinned = [&](const std::vector<scene3d::NodePose>& pose) {
        const auto joints =
            scene3d::joint_transforms(model.skins[0], scene3d::node_globals(model, pose));
        std::vector<render::Vertex3D> out;
        scene3d::skin_vertices(surface, joints, out);
        REQUIRE(out.size() == surface.vertices.size());
        std::map<std::pair<float, float>, Vec3> by_rest;
        for (std::size_t i = 0; i < out.size(); ++i) {
            by_rest[{surface.vertices[i].position[0], surface.vertices[i].position[1]}] =
                vec(out[i].position);
        }
        return by_rest;
    };

    // At rest, nothing moves.
    std::vector<scene3d::NodePose> pose = scene3d::rest_pose(model);
    for (const auto& [rest, at] : skinned(pose)) {
        CHECK(near(at, {rest.first, rest.second, 0.0F}));
    }

    // Bent a quarter turn: the top row turns round Lower, and the middle row goes half way.
    scene3d::blend_animation(model, model, bend, 1.0, 1.0F, {}, pose);
    auto bent = skinned(pose);
    CHECK(near(bent[{0.5F, 2.0F}], {-1.0F, 1.5F, 0.0F}));
    CHECK(near(bent[{-0.5F, 2.0F}], {-1.0F, 0.5F, 0.0F}));
    CHECK(near(bent[{0.5F, 1.0F}], {0.25F, 1.25F, 0.0F}));
    CHECK(near(bent[{-0.5F, 1.0F}], {-0.25F, 0.75F, 0.0F}));
    CHECK(near(bent[{0.5F, 0.0F}], {0.5F, 0.0F, 0.0F}));
    // Normals turn with their vertices: the strip still faces +z.
    {
        const auto joints =
            scene3d::joint_transforms(model.skins[0], scene3d::node_globals(model, pose));
        std::vector<render::Vertex3D> out;
        scene3d::skin_vertices(surface, joints, out);
        for (const render::Vertex3D& vertex : out) {
            CHECK(near(vec(vertex.normal), {0.0F, 0.0F, 1.0F}));
        }
    }

    // Half way through, an eighth of a turn, by Godot's slerp.
    pose = scene3d::rest_pose(model);
    scene3d::blend_animation(model, model, bend, 0.5, 1.0F, {}, pose);
    const float eighth = std::sqrt(0.5F);
    bent = skinned(pose);
    CHECK(near(bent[{0.5F, 2.0F}], {-0.5F * eighth, 1.0F + (1.5F * eighth), 0.0F}));
    // The whole animation at half its weight turns as far.
    std::vector<scene3d::NodePose> half = scene3d::rest_pose(model);
    scene3d::blend_animation(model, model, bend, 1.0, 0.5F, {}, half);
    CHECK(near(half[2].rotation.z, pose[2].rotation.z));
    CHECK(near(half[2].rotation.w, pose[2].rotation.w));

    // A Skeleton3D's bone rotation, in radians as Node3D's, poses the bone the same way.
    world::Skeleton3D skeleton;
    skeleton.bone_rotations["Lower"] = {0.0F, 0.0F, 1.5707963F};
    const auto posed = scene3d::pose_of(model, {}, &skeleton, {});
    bent = skinned(posed);
    CHECK(near(bent[{0.5F, 2.0F}], {-1.0F, 1.5F, 0.0F}));
    // It wins over an animation, and show_rest_only shows the rest pose whatever plays.
    const scene3d::ModelLoader loader =
        [&](const std::string& path) -> slash::Result<std::shared_ptr<const scene3d::Model>> {
        auto found = load(files, path);
        if (!found) {
            return found.error();
        }
        return std::make_shared<const scene3d::Model>(std::move(found).value());
    };
    const std::vector<scene3d::ClipSample> idle{
        {.model = "arm.gltf", .animation = "Idle-loop", .time = 0.5, .weight = 1.0F}};
    const auto raised = scene3d::pose_of(model, idle, &skeleton, loader);
    CHECK(near(raised[1].position, {0.0F, 0.25F, 0.0F}));
    bent = skinned(raised);
    CHECK(near(bent[{0.5F, 2.0F}], {-1.0F, 1.75F, 0.0F}));
    skeleton.show_rest_only = true;
    for (const auto& [rest, at] : skinned(scene3d::pose_of(model, idle, &skeleton, loader))) {
        CHECK(near(at, {rest.first, rest.second, 0.0F}));
    }

    // A looping track goes round from its last key to its first; another holds its ends.
    const scene3d::NodeTrack& rise = model.animations[1].tracks[0];
    CHECK(near(scene3d::sample_track(rise, 0.25, 1.0, true)[1], 0.125F));
    CHECK(near(scene3d::sample_track(rise, 2.0, 1.0, false)[1], 0.0F));
    // Two poses blend part of the way, as frames between ticks do.
    const auto between = scene3d::blend_poses(scene3d::rest_pose(model), raised, 0.5F);
    CHECK(near(between[1].position, {0.0F, 0.125F, 0.0F}));

    // An animation from another model moves the nodes of the same names.
    slash::test::Gltf other = slash::test::arm();
    other.document["nodes"] = parse(R"([{"name": "Rig", "children": [1, 3]},
        {"name": "Upper", "children": [2]}, {"name": "Lower", "translation": [0, 1, 0]},
        {"name": "Arm", "mesh": 0, "skin": 0}])");
    files.files["other.gltf"] = other.gltf();
    const auto source = load(files, "other.gltf");
    REQUIRE(source.ok());
    const std::vector<int> map = scene3d::node_map(model, source.value());
    CHECK(map == std::vector<int>{-1, 1, 2, 3});
    pose = scene3d::rest_pose(model);
    scene3d::blend_animation(model, source.value(), source.value().animations[0], 1.0, 1.0F, map,
                             pose);
    CHECK(near(skinned(pose)[{0.5F, 2.0F}], {-1.0F, 1.5F, 0.0F}));
}

TEST_CASE("FBX skins and animation stacks load as Godot's importer reads them") {
    MemoryFiles files;
    files.files["arm.gltf"] = slash::test::arm().gltf();
    const auto gltf = load(files, "arm.gltf");
    REQUIRE(gltf.ok());
    // In meters and in centimeters, ASCII and binary.
    for (const auto& [units, binary] : {std::pair{1.0, false}, std::pair{1.0, true},
                                        std::pair{100.0, false}, std::pair{100.0, true}}) {
        CAPTURE(units);
        CAPTURE(binary);
        const Fbx file = slash::test::fbx_arm(units);
        files.files["arm.fbx"] = binary ? file.binary() : file.ascii();
        const auto loaded = load(files, "arm.fbx");
        REQUIRE_MESSAGE(loaded.ok(), error_of(loaded));
        const scene3d::Model& model = loaded.value();

        // The file's tree, parents first, with each node where it is in its parent.
        REQUIRE(model.nodes.size() == 4);
        CHECK(model.nodes[0].name == "Armature");
        CHECK(model.nodes[0].parent == -1);
        CHECK(model.nodes[1].name == "Upper");
        CHECK(model.nodes[1].parent == 0);
        CHECK(model.nodes[2].name == "Lower");
        CHECK(model.nodes[2].parent == 1);
        CHECK(near(model.nodes[2].position, {0.0F, 1.0F, 0.0F}));
        CHECK(model.nodes[3].name == "Arm");
        CHECK(model.nodes[3].parent == 0);

        // The skin deformer: its clusters' bones and where they were bound.
        REQUIRE(model.skins.size() == 1);
        const scene3d::ModelSkin& skin = model.skins[0];
        CHECK(skin.name == "ArmSkin");
        CHECK(skin.joints == std::vector<std::size_t>{1, 2});
        CHECK(near(skin.inverse_binds[0].origin, {0.0F, 0.0F, 0.0F}));
        CHECK(near(skin.inverse_binds[1].origin, {0.0F, -1.0F, 0.0F}));
        REQUIRE(model.parts.size() == 1);
        CHECK(model.parts[0].skin == 0);
        CHECK(model.parts[0].node_index == 3);

        // Each vertex's weights, in four places as no vertex has more.
        const scene3d::ModelSurface& surface = model.meshes[0].surfaces.at(0);
        REQUIRE(surface.weights.size() == surface.vertices.size());
        for (std::size_t i = 0; i < surface.vertices.size(); ++i) {
            const float y = surface.vertices[i].position[1];
            std::array<float, 2> by_joint{};
            for (std::size_t k = 0; k < 8; ++k) {
                if (surface.weights[i].weights.at(k) > 0.0F) {
                    REQUIRE(surface.weights[i].joints.at(k) < 2);
                    by_joint.at(surface.weights[i].joints.at(k)) +=
                        surface.weights[i].weights.at(k);
                }
            }
            const float lower = y > 1.5F ? 1.0F : (y > 0.5F ? 0.5F : 0.0F);
            CHECK(near(by_joint[0], 1.0F - lower));
            CHECK(near(by_joint[1], lower));
        }

        // The stacks, as long as they are, with only the tracks that move something: Bend turns
        // Lower, and Idle-loop, which loops by its name, raises Upper.
        REQUIRE(model.animations.size() == 2);
        const scene3d::ModelAnimation& bend = model.animations[0];
        CHECK(bend.name == "Bend");
        CHECK_FALSE(bend.loop);
        CHECK(bend.length == doctest::Approx(1.0));
        REQUIRE(bend.tracks.size() == 1);
        CHECK(bend.tracks[0].node == 2);
        CHECK(bend.tracks[0].property == scene3d::NodeProperty::Rotation);
        const scene3d::ModelAnimation& idle = model.animations[1];
        CHECK(idle.name == "Idle-loop");
        CHECK(idle.loop);
        REQUIRE(idle.tracks.size() == 1);
        CHECK(idle.tracks[0].node == 1);
        CHECK(idle.tracks[0].property == scene3d::NodeProperty::Position);

        // Posed by either animation at any time, it moves as the same arm made as glTF does.
        const auto skinned = [](const scene3d::Model& m, const scene3d::ModelAnimation& clip,
                                double time) {
            std::vector<scene3d::NodePose> pose = scene3d::rest_pose(m);
            scene3d::blend_animation(m, m, clip, time, 1.0F, {}, pose);
            const auto joints =
                scene3d::joint_transforms(m.skins[0], scene3d::node_globals(m, pose));
            std::vector<render::Vertex3D> out;
            scene3d::skin_vertices(m.meshes[0].surfaces[0], joints, out);
            std::map<std::pair<float, float>, Vec3> by_rest;
            for (std::size_t i = 0; i < out.size(); ++i) {
                const render::Vertex3D& rest = m.meshes[0].surfaces[0].vertices[i];
                by_rest[{rest.position[0], rest.position[1]}] = vec(out[i].position);
            }
            return by_rest;
        };
        for (std::size_t a = 0; a < 2; ++a) {
            for (const double time : {0.0, 0.25, 0.5, 0.8, 1.0}) {
                CAPTURE(a);
                CAPTURE(time);
                const auto want = skinned(gltf.value(), gltf.value().animations[a], time);
                const auto got = skinned(model, model.animations[a], time);
                REQUIRE(got.size() == want.size());
                for (const auto& [rest, at] : want) {
                    CAPTURE(rest.second);
                    CHECK(near(got.at(rest), at));
                }
            }
        }
        CHECK(scene3d::check_model(files.files["arm.fbx"], "arm.fbx", files.reader()).empty());
    }
}

TEST_CASE("AnimationPlayer plays a model's animations on the skinned mesh its entity draws") {
    Scene scene;
    slash::animation::install(scene.world, scene.assets);
    scene.files.files["models/arm.gltf"] = slash::test::arm().gltf();
    // Unlit white, so pixels say where the arm is.
    world::StandardMaterial3D white;
    white.shading_mode = "unshaded";
    const entt::entity arm = scene.create("arm", R"({"Transform3D": {"position": [0, -1, 0]},
                   "MeshInstance3D": {"mesh": "models/arm.gltf"},
                   "AnimationPlayer": {"library": "models/arm.gltf", "animation": "Bend"}})");
    scene.world.ecs().emplace<world::StandardMaterial3D>(arm, white);
    CHECK(scene3d::check_meshes(scene.world).empty());

    // Paused at the start, the arm stands straight up from the middle of the view; bent, its top
    // lies to the left.
    constexpr std::array<std::uint8_t, 4> kBackground{0, 0, 255, 255};
    const auto at = [&](std::uint32_t x, std::uint32_t y) {
        const auto captured = canvas::capture(scene.world, scene.assets);
        REQUIRE_MESSAGE(captured.ok(), error_of(captured));
        return pixel(captured.value().image, x, y);
    };
    CHECK(at(32, 21) != kBackground);
    CHECK(at(22, 32) == kBackground);
    CHECK(scene3d::clips(scene.world, arm) ==
          std::vector<scene3d::ClipSample>{
              {.model = "models/arm.gltf", .animation = "Bend", .time = 0.0, .weight = 1.0F}});

    scene.world.ecs().get<world::AnimationPlayer>(arm).playing = true;
    for (int tick = 0; tick < 60; ++tick) {
        scene.world.step(1.0 / 60.0);
    }
    const auto& player = scene.world.ecs().get<world::AnimationPlayer>(arm);
    CHECK(player.time == doctest::Approx(1.0));
    CHECK_FALSE(player.playing); // Bend doesn't loop
    CHECK(scene3d::clips(scene.world, arm).at(0).time == doctest::Approx(1.0));
    CHECK(at(32, 21) == kBackground);
    CHECK(at(22, 32) != kBackground);

    // Every device draws the skinned arm where the software device does, as its pose changes.
    const auto drawn_by = [&](render::RenderDevice& device, canvas::CanvasRenderer& renderer) {
        auto target = device.create_texture(
            {.width = 64, .height = 64, .filter = render::Filter::Linear, .render_target = true});
        REQUIRE(target.ok());
        canvas::CanvasOptions options;
        options.target = target.value();
        options.width = 64.0F;
        options.height = 64.0F;
        REQUIRE(renderer.draw(scene.world, options).ok());
        auto drawn = device.read_texture(target.value());
        REQUIRE(drawn.ok());
        device.destroy_texture(target.value());
        return std::move(drawn).value();
    };
    for (const auto& device : drawing_devices()) {
        CAPTURE(render::backend_name(device->backend()));
        canvas::CanvasRenderer renderer(*device, scene.assets);
        scene.world.ecs().get<world::AnimationPlayer>(arm).time = 1.0;
        const render::Image bent = drawn_by(*device, renderer);
        CHECK(pixel(bent, 22, 32) != kBackground);
        CHECK(pixel(bent, 32, 21) == kBackground);
        // The same mesh, its vertices replaced.
        scene.world.ecs().get<world::AnimationPlayer>(arm).time = 0.0;
        const render::Image straight = drawn_by(*device, renderer);
        CHECK(pixel(straight, 22, 32) == kBackground);
        CHECK(pixel(straight, 32, 21) != kBackground);
    }

    // A Skeleton3D poses bones by name over the animation, and names the model hasn't are
    // reported with the closest one.
    world::Skeleton3D skeleton;
    skeleton.bone_rotations["Lower"] = {0.0F, 0.0F, 0.0F};
    skeleton.bone_positions["Lowr"] = {0.0F, 2.0F, 0.0F};
    scene.world.ecs().emplace<world::Skeleton3D>(arm, skeleton);
    scene.world.ecs().get<world::AnimationPlayer>(arm).time = 1.0;
    CHECK(at(32, 21) != kBackground);
    const auto problems = scene3d::check_meshes(scene.world);
    REQUIRE(problems.size() == 1);
    CHECK(problems[0].path == "/components/Skeleton3D/bone_positions/Lowr");
    CHECK(problems[0].message ==
          "the model has no bone or node named \"Lowr\"; did you mean \"Lower\"?");
    const entt::entity stray = scene.create("stray", R"({"Skeleton3D": {}})");
    const auto more = scene3d::check_meshes(scene.world);
    REQUIRE(more.size() == 2);
    CHECK(more[1].entity == stray);
    CHECK(more[1].path == "/components/Skeleton3D");
}

TEST_CASE("animation files play a model's animations with tracks of their own") {
    Scene scene;
    slash::animation::install(scene.world, scene.assets);
    std::vector<std::string> events;
    scene.world.set_event_listener([&events](const world::Event& event) {
        if (!event.kind.starts_with("entity.")) {
            events.push_back(event.kind);
        }
    });
    scene.files.files["models/arm.gltf"] = slash::test::arm().gltf();
    // Its length and loop come from the model's animation; its event is its own.
    scene.files.files["anim/arm.anim.json"] = R"({"format": 1, "animations": {
        "idle": {"from": {"model": "models/arm.gltf", "animation": "Idle-loop"},
                 "tracks": [{"event": "breath", "keys": [{"time": 0.5}]}]},
        "slow": {"from": {"model": "models/arm.gltf", "animation": "Bend"}, "length": 2}}})";
    const entt::entity arm =
        scene.create("arm", R"({"Transform3D": {}, "MeshInstance3D": {"mesh": "models/arm.gltf"},
                   "AnimationPlayer": {"library": "anim/arm.anim.json", "animation": "idle",
                                       "playing": true}})");
    for (int tick = 0; tick < 90; ++tick) {
        scene.world.step(1.0 / 60.0);
    }
    const auto& player = scene.world.ecs().get<world::AnimationPlayer>(arm);
    CHECK(player.playing); // it loops, as Idle-loop does
    CHECK(player.time == doctest::Approx(0.5));
    CHECK(events == std::vector<std::string>{"breath", "breath"});
    const auto playing = scene3d::clips(scene.world, arm);
    REQUIRE(playing.size() == 1);
    CHECK(playing[0].model == "models/arm.gltf");
    CHECK(playing[0].animation == "Idle-loop");
    CHECK(playing[0].time == doctest::Approx(0.5));

    // Scenes are checked against the model's animations.
    const json::Value doc = parse(R"({"format": 1, "entities": {
        "a": {"components": {"AnimationPlayer": {"library": "models/arm.gltf",
                                                 "animation": "Bnd"}}},
        "b": {"components": {"AnimationPlayer": {"library": "anim/bad.anim.json"}}},
        "c": {"components": {"AnimationPlayer": {"library": "anim/arm.anim.json",
                                                 "animation": "slow"}}}}})");
    scene.files.files["anim/bad.anim.json"] = R"({"format": 1, "animations": {
        "x": {"from": {"model": "models/arm.gltf", "animation": "Idel-loop"}}}})";
    std::vector<std::string> errors;
    for (const json::ValidationError& error :
         slash::animation::check_players(doc, scene.files.reader())) {
        errors.push_back(error.path + ": " + error.message);
    }
    CHECK(errors == std::vector<std::string>{
                        "/entities/a/components/AnimationPlayer/animation: models/arm.gltf has "
                        "no animation \"Bnd\"; did you mean \"Bend\"?",
                        "/entities/b/components/AnimationPlayer/library: anim/bad.anim.json: "
                        "/animations/x/from/animation: models/arm.gltf has no animation "
                        "\"Idel-loop\"; did you mean \"Idle-loop\"?",
                    });
}

TEST_CASE("node tracks in animation files move the nodes of the model the entity draws") {
    Scene scene;
    slash::animation::install(scene.world, scene.assets);
    scene.files.files["models/arm.gltf"] = slash::test::arm().gltf();
    // Lower turns a quarter turn about z over a second, as the model's Bend does, while Upper
    // stands half a unit up, then drops back.
    scene.files.files["anim/arm.anim.json"] = R"({"format": 1, "animations": {
        "bend": {"length": 1, "tracks": [
            {"node": "Lower", "property": "rotation",
             "keys": [{"time": 0, "value": [0, 0, 0]}, {"time": 1, "value": [0, 0, 1.5707964]}]},
            {"node": "Upper", "property": "position", "interpolation": "nearest",
             "keys": [{"time": 0, "value": [0, 0.5, 0]}, {"time": 0.5, "value": [0, 0, 0]}]}]}}})";
    const entt::entity arm =
        scene.create("arm", R"({"Transform3D": {}, "MeshInstance3D": {"mesh": "models/arm.gltf"},
                   "AnimationPlayer": {"library": "anim/arm.anim.json", "animation": "bend",
                                       "playing": true}})");
    auto model = scene.assets.load<scene3d::Model>("models/arm.gltf");
    REQUIRE(model.ok());
    const scene3d::ModelLoader load =
        [&](const std::string& path) -> slash::Result<std::shared_ptr<const scene3d::Model>> {
        auto found = scene.assets.load<scene3d::Model>(path);
        if (!found) {
            return found.error();
        }
        return found.value().share();
    };
    const auto pose_at = [&](const std::vector<scene3d::ClipSample>& clips) {
        return scene3d::pose_of(*model.value(), clips, nullptr, load);
    };
    for (int tick = 0; tick < 15; ++tick) {
        scene.world.step(1.0 / 60.0);
    }
    const auto clips = scene3d::clips(scene.world, arm);
    REQUIRE(clips.size() == 1);
    CHECK(clips[0].model == "anim/arm.anim.json");
    CHECK(clips[0].animation == "bend");
    CHECK(clips[0].time == doctest::Approx(0.25));
    REQUIRE(clips[0].source != nullptr);
    const auto bent = pose_at(clips);
    const auto bend = pose_at({{.model = "models/arm.gltf", .animation = "Bend", .time = 0.25}});
    const int upper = scene3d::find_node(*model.value(), "Upper");
    const int lower = scene3d::find_node(*model.value(), "Lower");
    REQUIRE(upper >= 0);
    REQUIRE(lower >= 0);
    const auto u = static_cast<std::size_t>(upper);
    const auto l = static_cast<std::size_t>(lower);
    CHECK(near(bent[l].rotation.z, bend[l].rotation.z));
    CHECK(near(bent[l].rotation.w, bend[l].rotation.w));
    CHECK(near(bent[l].position, {0.0F, 1.0F, 0.0F})); // untouched, at rest
    CHECK(near(bent[u].position, {0.0F, 0.5F, 0.0F}));
    for (int tick = 0; tick < 30; ++tick) {
        scene.world.step(1.0 / 60.0);
    }
    const auto later = pose_at(scene3d::clips(scene.world, arm));
    CHECK(near(later[u].position, {0.0F, 0.0F, 0.0F}));

    // Scenes are checked against the model the entity draws.
    scene.files.files["anim/typo.anim.json"] = R"({"format": 1, "animations": {
        "wave": {"length": 1, "tracks": [{"node": "Lowr", "property": "scale",
                                          "keys": [{"time": 0, "value": [1, 1, 1]}]}]}}})";
    const json::Value doc = parse(R"({"format": 1, "entities": {
        "a": {"components": {"MeshInstance3D": {"mesh": "models/arm.gltf"},
                             "AnimationPlayer": {"library": "anim/typo.anim.json"}}},
        "b": {"components": {"AnimationPlayer": {"library": "anim/arm.anim.json"}}},
        "c": {"components": {"MeshInstance3D": {"mesh": "models/arm.gltf"},
                             "AnimationPlayer": {"library": "anim/arm.anim.json"}}}}})");
    std::vector<std::string> errors;
    for (const json::ValidationError& error :
         slash::animation::check_players(doc, scene.files.reader())) {
        errors.push_back(error.path + ": " + error.message);
    }
    CHECK(errors == std::vector<std::string>{
                        "/entities/a/components/AnimationPlayer: \"Lowr\", which "
                        "anim/typo.anim.json's node tracks move, isn't a node of "
                        "models/arm.gltf; did you mean \"Lower\"?",
                        "/entities/b/components/AnimationPlayer: anim/arm.anim.json's node "
                        "tracks move the nodes of the model the entity draws, but it draws none: "
                        "give it a MeshInstance3D whose mesh is a model file",
                    });
}
