#include "devices.hpp"

#include "slash/asset/assets.hpp"
#include "slash/canvas/canvas.hpp"
#include "slash/core/jobs.hpp"
#include "slash/core/json.hpp"
#include "slash/render/device.hpp"
#include "slash/render/image.hpp"
#include "slash/render/mesh.hpp"
#include "slash/scene3d/lightmap.hpp"
#include "slash/scene3d/primitive.hpp"
#include "slash/ui/ui.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace asset = slash::asset;
namespace canvas = slash::canvas;
namespace json = slash::json;
namespace jobs = slash::jobs;
namespace render = slash::render;
namespace scene3d = slash::scene3d;
namespace world = slash::world;

namespace {

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? "" : parsed.error().message));
    return std::move(parsed).value();
}

// A world with the built-in components.
struct Scene {
    world::ComponentRegistry components;
    world::World world{components};

    Scene() { world::register_builtin_components(components); }

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

// A floor of 4 by 4 meters with a box of 1 meter on it, lit from above at a slant by a sun whose
// light is baked, under a grey sky color, and a LightmapGI baking them with few rays.
struct Room : Scene {
    entt::entity gi = entt::null;

    explicit Room(std::string_view quality = "low") {
        create("floor", R"({"Transform3D": {}, "MeshInstance3D": {},
            "PlaneMesh": {"size": [4, 4]}})");
        create("box", R"({"Transform3D": {"position": [0, 0.5, 0]}, "MeshInstance3D": {},
            "BoxMesh": {}, "StandardMaterial3D": {"albedo_color": [1, 0.2, 0.2, 1]}})");
        create("sun", R"({"Transform3D": {"rotation": [-1.0, 0.6, 0]},
            "DirectionalLight3D": {"light_bake_mode": "static"}})");
        gi = create("gi", std::string(R"({"LightmapGI": {"quality": ")") + std::string(quality) +
                              R"(", "bounces": 1, "environment_mode": "custom_color",
            "environment_custom_color": [0.2, 0.2, 0.2, 1]}})");
    }
};

// A texel's light, by where it is in a user's rectangle of the baked texture.
std::array<float, 3> light_at(const scene3d::LightmapBake& bake, std::string_view entity,
                              const std::array<float, 2>& uv2) {
    for (const scene3d::LightmapUser& user : bake.data.users) {
        if (user.entity != entity) {
            continue;
        }
        const float u = user.uv_rect[0] + (uv2[0] * user.uv_rect[2]);
        const float v = user.uv_rect[1] + (uv2[1] * user.uv_rect[3]);
        const auto x = std::min(static_cast<std::uint32_t>(u * static_cast<float>(bake.data.width)),
                                bake.data.width - 1);
        const auto y =
            std::min(static_cast<std::uint32_t>(v * static_cast<float>(bake.data.height)),
                     bake.data.height - 1) +
            (user.slice * bake.data.height);
        const std::size_t at = ((std::size_t{y} * bake.texels.width) + x) * 3;
        return {bake.texels.rgb[at], bake.texels.rgb[at + 1], bake.texels.rgb[at + 2]};
    }
    FAIL("no user " << entity);
    return {};
}

// Where on the floor's lightmap a point of the floor is: the UV2 of its unwrap's triangle there.
std::array<float, 2> floor_uv2(const scene3d::LightmapBake& bake, float x, float z) {
    world::PlaneMesh floor;
    floor.size = {4.0F, 4.0F};
    const scene3d::MeshData plane = scene3d::plane_mesh(floor);
    const scene3d::LightmapUnwrap& unwrap =
        bake.data.meshes.at(scene3d::lightmap_mesh_hash(plane.vertices, plane.indices));
    for (std::size_t i = 0; i + 2 < unwrap.indices.size(); i += 3) {
        std::array<std::array<float, 2>, 3> p{};
        std::array<std::array<float, 2>, 3> uv{};
        for (std::size_t k = 0; k < 3; ++k) {
            const std::uint32_t v = unwrap.indices[i + k];
            const render::Vertex3D& source = plane.vertices[unwrap.xref[v]];
            p.at(k) = {source.position[0], source.position[2]};
            uv.at(k) = unwrap.uv2[v];
        }
        const float d = ((p[1][1] - p[2][1]) * (p[0][0] - p[2][0])) +
                        ((p[2][0] - p[1][0]) * (p[0][1] - p[2][1]));
        const float a =
            (((p[1][1] - p[2][1]) * (x - p[2][0])) + ((p[2][0] - p[1][0]) * (z - p[2][1]))) / d;
        const float b =
            (((p[2][1] - p[0][1]) * (x - p[2][0])) + ((p[0][0] - p[2][0]) * (z - p[2][1]))) / d;
        const float c = 1.0F - a - b;
        if (a >= 0.0F && b >= 0.0F && c >= 0.0F) {
            return {(a * uv[0][0]) + (b * uv[1][0]) + (c * uv[2][0]),
                    (a * uv[0][1]) + (b * uv[1][1]) + (c * uv[2][1])};
        }
    }
    FAIL("no floor at " << x << ", " << z);
    return {};
}

float brightness(const std::array<float, 3>& rgb) {
    return (rgb[0] + rgb[1] + rgb[2]) / 3.0F;
}

} // namespace

TEST_CASE("half floats and Radiance HDR files keep a lightmap's light") {
    CHECK(render::float_to_half(1.0F) == 0x3C00);
    CHECK(render::float_to_half(-2.0F) == 0xC000);
    CHECK(render::float_to_half(65504.0F) == 0x7BFF);
    CHECK(render::float_to_half(70000.0F) == 0x7C00);
    CHECK(render::float_to_half(5.9604645e-8F) == 0x0001);
    // Ties go to the even half.
    CHECK(render::float_to_half(1.0F + (1.0F / 2048.0F)) == 0x3C00);
    CHECK(render::float_to_half(1.0F + (3.0F / 2048.0F)) == 0x3C02);
    for (std::uint32_t bits = 0; bits < 0x7C00; ++bits) {
        CHECK(render::float_to_half(render::half_to_float(static_cast<std::uint16_t>(bits))) ==
              bits);
    }
    CHECK(std::isnan(render::half_to_float(render::float_to_half(std::nanf("")))));

    render::HdrImage image{.width = 3, .height = 2, .rgb = {}};
    image.rgb = {0.0F, 0.5F, 1.0F, 2.0F, 100.0F, 0.001F, -1.0F, 0.25F, 3.0F,
                 7.5F, 7.5F, 7.5F, 0.0F, 0.0F,   0.0F,   1e-3F, 1e3F,  1.0F};
    const std::string bytes = render::encode_hdr(image).value();
    CHECK(bytes.starts_with("#?RADIANCE\n"));
    const render::HdrImage read = render::decode_hdr(bytes).value();
    REQUIRE(read.width == 3);
    REQUIRE(read.height == 2);
    for (std::size_t i = 0; i < image.rgb.size(); ++i) {
        CAPTURE(i);
        const float want = std::max(image.rgb[i], 0.0F);
        // RGBE keeps 8 bits of each color under the brightest's exponent.
        const std::size_t first = i - (i % 3);
        const float largest =
            std::max({image.rgb[first], image.rgb[first + 1], image.rgb[first + 2]});
        CHECK(std::abs(read.rgb[i] - want) <= largest / 128.0F);
    }
    // Encoding what was decoded gives the same bytes.
    CHECK(render::encode_hdr(read).value() == bytes);
    CHECK_FALSE(render::decode_hdr("#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 1\n").ok());
}

TEST_CASE("meshes unwrap into lightmap UVs as Godot's lightmap_unwrap, the same every time") {
    const scene3d::MeshData box = scene3d::box_mesh({});
    const scene3d::LightmapUnwrap unwrap =
        scene3d::unwrap_lightmap(box.vertices, box.indices).value();
    // Six faces of 5 by 5 texels at 0.2 meters a texel, a texel apart.
    CHECK(unwrap.width > 10);
    CHECK(unwrap.height > 10);
    CHECK(unwrap.width < 40);
    CHECK(unwrap.indices.size() == box.indices.size());
    REQUIRE(unwrap.xref.size() == unwrap.uv2.size());
    for (std::size_t i = 0; i < unwrap.xref.size(); ++i) {
        CHECK(unwrap.xref[i] < box.vertices.size());
        CHECK(unwrap.uv2[i][0] >= 0.0F);
        CHECK(unwrap.uv2[i][0] <= 1.0F);
        CHECK(unwrap.uv2[i][1] >= 0.0F);
        CHECK(unwrap.uv2[i][1] <= 1.0F);
    }
    // Each triangle is the source's, by its corners' source vertices.
    for (std::size_t i = 0; i < box.indices.size(); ++i) {
        CHECK(unwrap.xref[unwrap.indices[i]] == box.indices[i]);
    }
    CHECK(scene3d::unwrap_lightmap(box.vertices, box.indices).value() == unwrap);
    // Smaller texels make more of them.
    const scene3d::LightmapUnwrap fine =
        scene3d::unwrap_lightmap(box.vertices, box.indices, 0.05F).value();
    CHECK(fine.width > unwrap.width * 3);

    const std::vector<render::Vertex3D> flat(3);
    const std::vector<std::uint32_t> triangle{0, 1, 2};
    CHECK(scene3d::unwrap_lightmap(flat, triangle).error().message ==
          "the mesh has no triangle of any area to unwrap");
    CHECK(scene3d::unwrap_lightmap(box.vertices, {}).error().message ==
          "the mesh has no triangles to unwrap");

    const std::string hash = scene3d::lightmap_mesh_hash(box.vertices, box.indices);
    CHECK(hash.size() == 16);
    CHECK(hash.find_first_not_of("0123456789abcdef") == std::string::npos);
    CHECK(scene3d::lightmap_mesh_hash(box.vertices, box.indices) == hash);
    const scene3d::MeshData bigger = scene3d::box_mesh({.size = {2.0F, 1.0F, 1.0F}});
    CHECK(scene3d::lightmap_mesh_hash(bigger.vertices, bigger.indices) != hash);
}

TEST_CASE(".lightmap.json files keep unwraps and users, and say what's wrong with them") {
    const scene3d::MeshData box = scene3d::box_mesh({});
    scene3d::LightmapData data;
    data.texture = "level.lightmap.hdr";
    data.width = 64;
    data.height = 32;
    data.slices = 2;
    data.baked_exposure = 0.5F;
    const std::string hash = scene3d::lightmap_mesh_hash(box.vertices, box.indices);
    data.meshes.emplace(hash, scene3d::unwrap_lightmap(box.vertices, box.indices).value());
    data.users.push_back({.entity = "box",
                          .part = 0,
                          .mesh = hash,
                          .slice = 1,
                          .uv_rect = {0.25F, 0.0F, 0.5F, 0.75F}});
    const json::Value doc = scene3d::lightmap_json(data);
    CHECK(scene3d::validate_lightmap(doc).empty());
    CHECK(scene3d::parse_lightmap(doc).value() == data);
    CHECK(doc.find("users")->as_array()[0].find("part") == nullptr);
    // What it writes reads back the same.
    CHECK(scene3d::parse_lightmap(parse(json::write(doc, json::Style::Compact))).value() == data);

    json::Value bad = doc;
    bad["users"].as_array()[0]["mesh"] = "0000000000000000";
    bad["users"].as_array()[0]["slice"] = 2;
    bad["users"].as_array()[0]["uv_rect"] = json::Array{0.75, 0.0, 0.5, 0.5};
    bad["meshes"][hash]["indices"] = "AAAAAAAAAAAAAAAAAAAAAA=="; // four zeros
    const auto errors = scene3d::validate_lightmap(bad);
    REQUIRE(errors.size() == 4);
    CHECK(errors[0].path == "/meshes/" + hash + "/indices");
    CHECK(errors[0].message == "expected triangles: a multiple of 3 indices");
    CHECK(errors[1].path == "/users/0/mesh");
    CHECK(errors[2].path == "/users/0/slice");
    CHECK(errors[2].message == "is past the last of 2 slices");
    CHECK(errors[3].path == "/users/0/uv_rect");
    CHECK(scene3d::parse_lightmap(bad).error().message ==
          "/meshes/" + hash + "/indices: expected triangles: a multiple of 3 indices");
    CHECK(scene3d::validate_lightmap(parse(R"({"format": 1})")).size() == 4);

    CHECK(scene3d::lightmap_texture_path("levels/one.lightmap.json") == "levels/one.lightmap.hdr");
    CHECK(scene3d::lightmap_texture_path("one.json") == "one.json.hdr");
}

TEST_CASE("a LightmapGI bakes its static meshes' light, shadows and bounces, as Godot's") {
    Room room;
    const scene3d::LightmapBake bake = scene3d::bake_lightmap(room.world, room.gi, {}).value();
    CHECK(bake.problems.empty());
    CHECK(bake.meshes == 2);
    CHECK(bake.lights == 1);
    REQUIRE(bake.data.users.size() == 2);
    CHECK(bake.data.users[0].entity == "floor");
    CHECK(bake.data.users[1].entity == "box");
    CHECK(bake.data.meshes.size() == 2);
    // Two slices of the smallest square hold the two as cheaply as one slice twice as wide, and
    // Godot keeps the first it tried.
    CHECK(bake.data.slices == 2);
    CHECK(bake.texels.width == bake.data.width);
    CHECK(bake.texels.height == bake.data.height * bake.data.slices);
    for (const float value : bake.texels.rgb) {
        REQUIRE(std::isfinite(value));
        REQUIRE(value >= 0.0F);
    }
    // The sun shines down at a slant, toward -x and -z: the floor beside the box is lit, the
    // floor in its shadow takes only the sky's light and the bounces.
    const float lit = brightness(light_at(bake, "floor", floor_uv2(bake, -1.5F, -1.5F)));
    const float shadowed = brightness(light_at(bake, "floor", floor_uv2(bake, -0.6F, -0.7F)));
    CAPTURE(lit);
    CAPTURE(shadowed);
    CHECK(lit > 0.5F);
    CHECK(shadowed < lit * 0.5F);
    CHECK(shadowed > 0.0F);
}

TEST_CASE("bakes are the same for any number of threads") {
    Room room;
    const scene3d::LightmapBake bake = scene3d::bake_lightmap(room.world, room.gi, {}).value();
    for (const int threads : {0, 3}) {
        CAPTURE(threads);
        jobs::Pool pool(threads);
        Room again;
        again.world.set_pool(pool);
        const scene3d::LightmapBake other =
            scene3d::bake_lightmap(again.world, again.gi, {}).value();
        CHECK(other.data == bake.data);
        CHECK(other.texels == bake.texels);
    }
}

namespace {

// The room on a 160 by 90 screen, seen from 4 meters straight above, with its files in memory and
// the canvas installed as games do.
struct DrawnRoom {
    std::map<std::string, std::string, std::less<>> files;
    asset::Assets assets{asset::FileSource{
        .read = [this](std::string_view path) -> slash::Result<std::string> {
            const auto it = files.find(path);
            if (it == files.end()) {
                return slash::make_error(slash::ErrorCode::NotFound, "no such file");
            }
            return it->second;
        },
        .stamp = [this](std::string_view path) -> std::optional<asset::FileStamp> {
            const auto it = files.find(path);
            if (it == files.end()) {
                return std::nullopt;
            }
            return asset::FileStamp{.modified = 1, .size = it->second.size()};
        },
    }};
    Room room;

    DrawnRoom() {
        canvas::install(room.world, assets);
        slash::ui::set_viewport(room.world, {160.0F, 90.0F});
        room.create("camera", R"({"Transform3D": {"position": [0, 4, 0],
            "rotation": [-1.5707964, 0, 0]}, "Camera3D": {}})");
    }

    render::Image capture() {
        auto captured = canvas::capture(room.world, assets);
        REQUIRE_MESSAGE(captured.ok(), (captured.ok() ? "" : captured.error().message));
        return std::move(captured).value().image;
    }
};

// How bright the floor's pixel is where a point of it shows: the camera looks down -y with +x to
// the right and -z up, about 14.7 pixels a meter at the floor.
float floor_pixel(const render::Image& image, float x, float z) {
    constexpr float kPixelsPerMeter = 45.0F / (4.0F * 0.76732698F); // tan(37.5 degrees)
    const auto px = static_cast<std::uint32_t>(80.0F + (x * kPixelsPerMeter));
    const auto py = static_cast<std::uint32_t>(45.0F + (z * kPixelsPerMeter));
    const std::size_t at = ((std::size_t{py} * image.width) + px) * 4;
    return static_cast<float>(image.pixels[at] + image.pixels[at + 1] + image.pixels[at + 2]) /
           (3.0F * 255.0F);
}

} // namespace

TEST_CASE("lightmapped meshes draw their baked light in place of baked lights, as Godot's") {
    DrawnRoom drawn;
    // Unbaked, the sun lights the floor live and casts no shadow, since its shadow is off.
    const render::Image live = drawn.capture();
    const float live_lit = floor_pixel(live, -1.5F, -1.5F);
    const float live_shadowed = floor_pixel(live, -0.2F, -0.95F);
    CAPTURE(live_lit);
    CAPTURE(live_shadowed);
    CHECK(live_lit > 0.2F);
    CHECK(std::abs(live_shadowed - live_lit) < 0.05F);

    scene3d::LightmapBake bake =
        scene3d::bake_lightmap(drawn.room.world, drawn.room.gi, {}).value();
    bake.data.texture = "room.lightmap.hdr";
    drawn.files["room.lightmap.json"] = json::write(scene3d::lightmap_json(bake.data));
    drawn.files["room.lightmap.hdr"] = render::encode_hdr(bake.texels).value();
    drawn.room.world.ecs().get<world::LightmapGI>(drawn.room.gi).light_data = "room.lightmap.json";
    // Baked, the floor takes the sun from the lightmap, with the box's shadow in it, and the sun
    // no longer lights it live.
    const render::Image baked = drawn.capture();
    const float lit = floor_pixel(baked, -1.5F, -1.5F);
    const float shadowed = floor_pixel(baked, -0.2F, -0.95F);
    CAPTURE(lit);
    CAPTURE(shadowed);
    CHECK(lit > 0.2F);
    CHECK(shadowed < lit * 0.6F);
    CHECK(shadowed > 0.0F);

    // A mesh changed since the bake draws without its lightmap.
    drawn.room.world.ecs().get<world::PlaneMesh>(drawn.room.world.find("floor")).size = {4.0F,
                                                                                         4.5F};
    const render::Image changed = drawn.capture();
    CHECK(std::abs(floor_pixel(changed, -0.2F, -0.95F) - live_shadowed) < 0.05F);
}

namespace {

// A floor two meters across seen from above, lit by a 4 by 4 lightmap: warm in one corner, dim
// blue in the opposite one. The scene's ambient light and a baked sun leave it alone, and a
// live omni light by one edge still lights it. Its left half samples bicubically, its right half
// bilinearly, from the lower half of the lightmap.
render::Image draw_lightmapped(render::RenderDevice& device,
                               render::RenderMethod method = render::RenderMethod::ForwardPlus,
                               bool lights = true) {
    auto target = device.create_texture({.width = 48, .height = 48, .render_target = true});
    REQUIRE(target.ok());
    std::vector<std::uint8_t> texels;
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            const float t = static_cast<float>(x + y) / 6.0F;
            for (const float value :
                 {2.0F - (1.8F * t), 0.9F - (0.6F * t), 0.1F + (0.5F * t), 1.0F}) {
                const std::uint16_t half = render::float_to_half(value);
                texels.push_back(static_cast<std::uint8_t>(half & 0xFFU));
                texels.push_back(static_cast<std::uint8_t>(half >> 8U));
            }
        }
    }
    auto lightmap = device.create_texture(
        {.width = 4, .height = 4, .format = render::TextureFormat::Rgba16F}, texels);
    REQUIRE(lightmap.ok());
    // Half of the floor: its uv2 goes 0 to 1 across it.
    const auto half_floor = [](float left) {
        const auto corner = [&](float x, float z) {
            return render::Vertex3D{
                .position = {left + x, 0.0F, z - 1.0F}, .uv = {x, z * 0.5F}, .uv2 = {x, z * 0.5F}};
        };
        return std::vector<render::Vertex3D>{corner(0, 0), corner(1, 2), corner(0, 2),
                                             corner(0, 0), corner(1, 0), corner(1, 2)};
    };
    auto left = device.create_mesh(half_floor(-1.0F), {});
    auto right = device.create_mesh(half_floor(0.0F), {});
    REQUIRE(left.ok());
    REQUIRE(right.ok());
    render::Scene3D scene;
    scene.method = method;
    scene.camera = render::look_at({0.0F, 2.5F, 0.0F}, {0.0F, 0.0F, 0.0F}, {0.0F, 0.0F, -1.0F});
    scene.projection = render::perspective(60.0F, 1.0F, 0.1F, 10.0F);
    scene.ambient = {0.6F, 0.6F, 0.6F};
    if (lights) {
        scene.lights = {
            {.type = render::LightType::Directional,
             .energy = 3.0F,
             .direction = {0.0F, -1.0F, 0.0F},
             .baked = true},
            {.type = render::LightType::Omni,
             .color = {0.2F, 1.0F, 0.3F},
             .energy = 2.0F,
             .position = {0.9F, 0.3F, 0.0F},
             .range = 1.0F},
        };
    }
    REQUIRE(device
                .begin_frame(
                    {.target = target.value(), .clear = {0.0F, 0.0F, 0.0F, 1.0F}, .scene = scene})
                .ok());
    const std::array<float, 16> identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    render::Material3D material{.albedo = {0.8F, 0.8F, 0.8F, 1.0F}};
    material.lightmap = {.texture = lightmap.value(), .uv_rect = {0.0F, 0.0F, 1.0F, 1.0F}};
    const auto drawn = device.draw_mesh(left.value(), identity, material);
    CHECK_MESSAGE(drawn.ok(), (drawn.ok() ? "" : drawn.error().message));
    material.lightmap.uv_rect = {0.0F, 0.5F, 1.0F, 0.5F};
    material.lightmap.bicubic = false;
    CHECK(device.draw_mesh(right.value(), identity, material).ok());
    REQUIRE(device.end_frame().ok());
    auto image = device.read_texture(target.value());
    REQUIRE(image.ok());
    device.destroy_mesh(left.value());
    device.destroy_mesh(right.value());
    device.destroy_texture(lightmap.value());
    device.destroy_texture(target.value());
    return std::move(image).value();
}

std::array<int, 4> image_pixel(const render::Image& image, std::uint32_t x, std::uint32_t y) {
    const std::size_t at = ((std::size_t{y} * image.width) + x) * 4;
    return {image.pixels.at(at), image.pixels.at(at + 1), image.pixels.at(at + 2),
            image.pixels.at(at + 3)};
}

} // namespace

TEST_CASE("every device draws lightmaps as the software device does") {
    const auto software = render::create_software_device();
    const render::Image forward = draw_lightmapped(*software);
    // The lightmap's warm corner and its blue one show, and the baked sun adds nothing: only the
    // omni light by the right edge changes the picture.
    const std::array<int, 4> warm = image_pixel(forward, 12, 10);
    const std::array<int, 4> cool = image_pixel(forward, 20, 40);
    CAPTURE(warm[0]);
    CAPTURE(cool[2]);
    CHECK(warm[0] > warm[2] + 60);
    CHECK(cool[2] > cool[0] - 20);
    const render::Image unlit =
        draw_lightmapped(*software, render::RenderMethod::ForwardPlus, false);
    CHECK(image_pixel(unlit, 12, 10) == warm);
    CHECK(image_pixel(unlit, 39, 24) != image_pixel(forward, 39, 24));
    const render::Image compatibility =
        draw_lightmapped(*software, render::RenderMethod::Compatibility);
    for (auto& device : slash::test::hardware_devices()) {
        CAPTURE(render::backend_name(device->backend()));
        const bool opengl = device->backend() == render::Backend::OpenGl ||
                            device->backend() == render::Backend::OpenGlEs;
        const render::Image& expected = opengl ? compatibility : forward;
        const render::Image image = draw_lightmapped(*device);
        int differing = 0;
        int worst = 0;
        for (std::uint32_t y = 0; y < image.height; ++y) {
            for (std::uint32_t x = 0; x < image.width; ++x) {
                const std::array<int, 4> a = image_pixel(image, x, y);
                const std::array<int, 4> b = image_pixel(expected, x, y);
                int most = 0;
                for (std::size_t c = 0; c < 4; ++c) {
                    most = std::max(most, std::abs(a.at(c) - b.at(c)));
                }
                differing += most > 2 ? 1 : 0;
                worst = std::max(worst, most);
            }
        }
        CAPTURE(worst);
        CHECK(differing <= 10);
    }
}
