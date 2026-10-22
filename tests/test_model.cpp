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
    CHECK(problems[1].message == "expected a .gltf, .glb or .obj file");
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
