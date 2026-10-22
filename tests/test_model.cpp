#include "devices.hpp"
#include "gltf.hpp"
#include "jpeg.hpp"

#include "slash/asset/assets.hpp"
#include "slash/canvas/canvas.hpp"
#include "slash/core/json.hpp"
#include "slash/core/math.hpp"
#include "slash/core/text.hpp"
#include "slash/render/device.hpp"
#include "slash/render/image.hpp"
#include "slash/scene3d/model.hpp"
#include "slash/scene3d/scene3d.hpp"
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
    CHECK_FALSE(scene3d::is_model_path("models/ship.obj"));

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
    // With no scene, the nodes without parents are drawn.
    REQUIRE(m.parts.size() == 1);
    CHECK(m.parts[0].node.empty());
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
    CHECK(textured.metallic_roughness_image == 0);
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
    scene.create("obj", R"({"Transform3D": {}, "MeshInstance3D": {"mesh": "models/quad.obj"}})");
    scene.create("gone", R"({"Transform3D": {}, "MeshInstance3D": {"mesh": "models/gone.glb"}})");
    const auto problems = scene3d::check_meshes(scene.world);
    REQUIRE(problems.size() == 3);
    CHECK(problems[0].path == "/components/MeshInstance3D/mesh");
    CHECK(problems[0].message == "draws a model, so its BoxMesh is left out: remove one of them");
    CHECK(problems[1].message == "expected a .gltf or .glb file");
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
