#include "slash/scene3d/lightmap.hpp"

#include "slash/asset/assets.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/math.hpp"
#include "slash/core/sha256.hpp"
#include "slash/core/text.hpp"
#include "slash/scene3d/primitive.hpp"
#include "slash/world/components.hpp"
#include "slash/world/tree3d.hpp"
#include "slash/world/world.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <numbers>
#include <optional>
#include <utility>
#include <xatlas.h>

namespace slash::scene3d {

namespace {

static_assert(std::endian::native == std::endian::little,
              "lightmap files and hashes hold the machine's bytes, little-endian");

constexpr float kPi = std::numbers::pi_v<float>;
// The panorama Godot bakes a scene's environment into for a lightmap.
constexpr std::uint32_t kEnvironmentWidth = 128;
constexpr std::uint32_t kEnvironmentHeight = 64;

Error first_error(const std::vector<json::ValidationError>& errors) {
    const json::ValidationError& first = errors.front();
    return Error{ErrorCode::InvalidArgument,
                 (first.path.empty() ? std::string("root") : first.path) + ": " + first.message};
}

// ---- The file

json::Value count_schema(std::string_view description, std::int64_t minimum) {
    json::Value out;
    out["type"] = "integer";
    out["minimum"] = minimum;
    out["maximum"] = 16384;
    out["description"] = description;
    return out;
}

json::Value make_schema() {
    json::Value mesh;
    mesh["type"] = "object";
    mesh["additionalProperties"] = false;
    mesh["required"] = json::Array{"width", "height", "xref", "uv2", "indices"};
    mesh["description"] = "A mesh unwrapped into lightmap UVs, as Godot's lightmap_unwrap.";
    mesh["properties"]["width"] = count_schema("Its lightmap's size hint, in texels.", 1);
    mesh["properties"]["height"] = count_schema("The same, down.", 1);
    mesh["properties"]["xref"]["type"] = "string";
    mesh["properties"]["xref"]["description"] =
        "Each vertex's source vertex, as base64 of little-endian 32-bit integers.";
    mesh["properties"]["uv2"]["type"] = "string";
    mesh["properties"]["uv2"]["description"] =
        "Each vertex's lightmap UV (u, v), as base64 of little-endian 32-bit floats.";
    mesh["properties"]["indices"]["type"] = "string";
    mesh["properties"]["indices"]["description"] =
        "Its triangles' vertices, as base64 of little-endian 32-bit integers.";

    json::Value user;
    user["type"] = "object";
    user["additionalProperties"] = false;
    user["required"] = json::Array{"entity", "mesh", "uv_rect"};
    user["description"] = "Where one mesh's light is, as Godot's LightmapGIData user.";
    user["properties"]["entity"]["type"] = "string";
    user["properties"]["entity"]["minLength"] = 1;
    user["properties"]["entity"]["description"] = "The MeshInstance3D's entity ID.";
    user["properties"]["part"]["type"] = "integer";
    user["properties"]["part"]["minimum"] = 0;
    user["properties"]["part"]["default"] = 0;
    user["properties"]["part"]["description"] = "Its model's part; 0 for a primitive mesh.";
    user["properties"]["mesh"]["type"] = "string";
    user["properties"]["mesh"]["pattern"] = "^[0-9a-f]{16}$";
    user["properties"]["mesh"]["description"] = "Its unwrap, a key of meshes.";
    user["properties"]["slice"]["type"] = "integer";
    user["properties"]["slice"]["minimum"] = 0;
    user["properties"]["slice"]["default"] = 0;
    user["properties"]["slice"]["description"] = "The slice of the texture its texels are in.";
    user["properties"]["uv_rect"]["type"] = "array";
    user["properties"]["uv_rect"]["items"]["type"] = "number";
    user["properties"]["uv_rect"]["items"]["minimum"] = 0;
    user["properties"]["uv_rect"]["items"]["maximum"] = 1;
    user["properties"]["uv_rect"]["minItems"] = 4;
    user["properties"]["uv_rect"]["maxItems"] = 4;
    user["properties"]["uv_rect"]["description"] =
        "Where its texels are in the slice: offset x, y, then scale x, y.";

    json::Value probes;
    probes["type"] = "object";
    probes["additionalProperties"] = false;
    probes["required"] = json::Array{"bounds", "points", "sh", "tetrahedra", "bsp"};
    probes["description"] =
        "Its light probes, as Godot's LightmapGIData capture data, which meshes that move take "
        "their light from; all in the LightmapGI's space.";
    probes["properties"]["bounds"]["type"] = "array";
    probes["properties"]["bounds"]["items"]["type"] = "number";
    probes["properties"]["bounds"]["minItems"] = 6;
    probes["properties"]["bounds"]["maxItems"] = 6;
    probes["properties"]["bounds"]["description"] =
        "The box the probes fill: position x, y, z, then size x, y, z.";
    probes["properties"]["interior"]["type"] = "boolean";
    probes["properties"]["interior"]["default"] = false;
    probes["properties"]["interior"]["description"] =
        "Meshes inside take only interior lightmaps' light, as Godot's interior.";
    probes["properties"]["points"]["type"] = "string";
    probes["properties"]["points"]["description"] =
        "Each probe's place (x, y, z), as base64 of little-endian 32-bit floats.";
    probes["properties"]["sh"]["type"] = "string";
    probes["properties"]["sh"]["description"] =
        "Each probe's light: nine spherical harmonics coefficients (l0; l1 y, z, x; l2 xy, yz, "
        "3z²-1, xz, x²-y²), each linear RGB, as base64 of little-endian 32-bit floats.";
    probes["properties"]["tetrahedra"]["type"] = "string";
    probes["properties"]["tetrahedra"]["description"] =
        "The Delaunay tetrahedra between the probes, four probes each, as base64 of "
        "little-endian 32-bit integers.";
    probes["properties"]["bsp"]["type"] = "string";
    probes["properties"]["bsp"]["description"] =
        "The BSP tree that finds a point's tetrahedron, as Godot's: each node a plane (normal x, "
        "y, z, then d; a point with normal·p > d goes over) as 32-bit floats, then over and under "
        "as 32-bit integers (a later node, -(tetrahedron + 1), or -2147483648 for none), as "
        "base64 of little-endian values.";

    json::Value keys;
    keys["pattern"] = "^[0-9a-f]{16}$";

    json::Value schema;
    schema["$schema"] = "https://json-schema.org/draft/2020-12/schema";
    schema["title"] = "SlashEngine lightmap";
    schema["description"] =
        "The light a LightmapGI baked, as Godot's LightmapGIData: its .hdr texture, the meshes' "
        "lightmap UVs and where each mesh's light is. lightmap.bake writes it.";
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["required"] = json::Array{"format", "texture", "width", "height", "slices"};
    json::Value& properties = schema["properties"];
    properties["format"]["const"] = 1;
    properties["format"]["description"] = "Format version. Must be 1.";
    properties["texture"]["type"] = "string";
    properties["texture"]["minLength"] = 1;
    properties["texture"]["description"] =
        "Its .hdr file, project path: the slices, each width by height, top to bottom.";
    properties["width"] = count_schema("A slice's width, in texels.", 1);
    properties["height"] = count_schema("A slice's height, in texels.", 1);
    properties["slices"] = count_schema("The slices.", 1);
    properties["baked_exposure"]["type"] = "number";
    properties["baked_exposure"]["exclusiveMinimum"] = 0;
    properties["baked_exposure"]["default"] = 1;
    properties["baked_exposure"]["description"] =
        "The exposure multiplier it was baked with; a camera's over it scales its light.";
    properties["meshes"]["type"] = "object";
    properties["meshes"]["description"] =
        "Unwraps by mesh hash: 16 hex digits of the SHA-256 of its positions, normals and "
        "triangles.";
    properties["meshes"]["propertyNames"] = std::move(keys);
    properties["meshes"]["additionalProperties"] = std::move(mesh);
    properties["users"]["type"] = "array";
    properties["users"]["items"] = std::move(user);
    properties["users"]["description"] = "The meshes it lights.";
    properties["probes"] = std::move(probes);
    return schema;
}

const json::Value& file_schema() {
    static const json::Value schema = make_schema();
    return schema;
}

template <typename T>
std::string encode_values(const std::vector<T>& values) {
    std::string bytes(values.size() * sizeof(T), '\0');
    if (!values.empty()) {
        std::memcpy(bytes.data(), values.data(), bytes.size());
    }
    return base64_encode(bytes);
}

template <typename T>
std::optional<std::vector<T>> decode_values(const std::string& text) {
    const std::optional<std::string> bytes = base64_decode(text);
    if (!bytes || bytes->size() % sizeof(T) != 0) {
        return std::nullopt;
    }
    std::vector<T> out(bytes->size() / sizeof(T));
    if (!out.empty()) {
        std::memcpy(out.data(), bytes->data(), bytes->size());
    }
    return out;
}

static_assert(sizeof(LightmapBspNode) == 24, "BSP nodes are kept as their bytes");

// The light probes of a document the schema passed, each error into errors.
LightmapProbes read_probes(const json::Value& doc, std::vector<json::ValidationError>& errors) {
    LightmapProbes out;
    const json::Array& bounds = doc.find("bounds")->as_array();
    for (std::size_t i = 0; i < 3; ++i) {
        out.bounds_position.at(i) = static_cast<float>(bounds[i].as_double());
        out.bounds_size.at(i) = static_cast<float>(bounds[i + 3].as_double());
        if (out.bounds_size.at(i) < 0.0F) {
            errors.push_back({"/probes/bounds/" + std::to_string(i + 3), "is negative"});
        }
    }
    if (const json::Value* interior = doc.find("interior")) {
        out.interior = interior->as_bool();
    }
    const auto finite = [](const std::vector<float>& values) {
        return std::ranges::all_of(values, [](float v) { return std::isfinite(v); });
    };
    auto points = decode_values<float>(doc.find("points")->as_string());
    auto sh = decode_values<float>(doc.find("sh")->as_string());
    auto tetrahedra = decode_values<std::uint32_t>(doc.find("tetrahedra")->as_string());
    auto bsp = decode_values<LightmapBspNode>(doc.find("bsp")->as_string());
    if (!points || points->size() % 3 != 0 || !finite(*points)) {
        errors.push_back({"/probes/points", "isn't base64 of finite 32-bit floats, three each"});
        return out;
    }
    const std::vector<float> positions = std::move(*points);
    const std::size_t count = positions.size() / 3;
    if (!sh || sh->size() != count * 27 || !finite(*sh)) {
        errors.push_back({"/probes/sh", "expected base64 of 27 finite 32-bit floats for each of " +
                                            std::to_string(count) + " probes"});
        return out;
    }
    const std::vector<float> coefficients = std::move(*sh);
    if (!tetrahedra || tetrahedra->size() % 4 != 0) {
        errors.push_back({"/probes/tetrahedra", "isn't base64 of 32-bit integers, four each"});
        return out;
    }
    const std::vector<std::uint32_t> corners = std::move(*tetrahedra);
    const auto bad =
        std::ranges::find_if(corners, [&](std::uint32_t index) { return index >= count; });
    if (bad != corners.end()) {
        errors.push_back({"/probes/tetrahedra",
                          "names probe " + std::to_string(*bad) + " of " + std::to_string(count)});
        return out;
    }
    if (!bsp) {
        errors.push_back({"/probes/bsp", "isn't base64 of 24-byte nodes"});
        return out;
    }
    std::vector<LightmapBspNode> nodes = std::move(*bsp);
    const std::size_t tetrahedron_count = corners.size() / 4;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const LightmapBspNode& node = nodes[i];
        const bool plane_ok =
            std::ranges::all_of(node.plane, [](float v) { return std::isfinite(v); });
        const auto side_ok = [&](std::int32_t side) {
            if (side == kLightmapBspEmpty) {
                return true;
            }
            if (side >= 0) {
                return static_cast<std::size_t>(side) > i &&
                       static_cast<std::size_t>(side) < nodes.size();
            }
            return static_cast<std::size_t>(-(static_cast<std::int64_t>(side) + 1)) <
                   tetrahedron_count;
        };
        if (!plane_ok || !side_ok(node.over) || !side_ok(node.under)) {
            errors.push_back({"/probes/bsp", "node " + std::to_string(i) +
                                                 " has a plane that isn't finite or a side that "
                                                 "isn't a later node, a tetrahedron or none"});
            return out;
        }
    }
    for (std::size_t i = 0; i < count; ++i) {
        out.points.push_back({positions[i * 3], positions[(i * 3) + 1], positions[(i * 3) + 2]});
        LightmapSh light{};
        for (std::size_t j = 0; j < 9; ++j) {
            for (std::size_t c = 0; c < 3; ++c) {
                light.at(j).at(c) = coefficients[(i * 27) + (j * 3) + c];
            }
        }
        out.sh.push_back(light);
    }
    for (std::size_t i = 0; i < tetrahedron_count; ++i) {
        out.tetrahedra.push_back(
            {corners[i * 4], corners[(i * 4) + 1], corners[(i * 4) + 2], corners[(i * 4) + 3]});
    }
    out.bsp = std::move(nodes);
    return out;
}

// The meshes and users of a document the schema passed: into data where given, each error into
// errors.
void read_lightmap(const json::Value& doc, LightmapData* data,
                   std::vector<json::ValidationError>& errors) {
    const auto slices = static_cast<std::uint32_t>(doc.find("slices")->as_int());
    LightmapData out;
    out.texture = doc.find("texture")->as_string();
    out.width = static_cast<std::uint32_t>(doc.find("width")->as_int());
    out.height = static_cast<std::uint32_t>(doc.find("height")->as_int());
    out.slices = slices;
    if (const json::Value* exposure = doc.find("baked_exposure")) {
        out.baked_exposure = static_cast<float>(exposure->as_double());
    }
    if (const json::Value* meshes = doc.find("meshes")) {
        for (const auto& [hash, mesh] : meshes->as_object()) {
            const std::string at = "/meshes/" + hash;
            LightmapUnwrap unwrap;
            unwrap.width = static_cast<std::uint32_t>(mesh.find("width")->as_int());
            unwrap.height = static_cast<std::uint32_t>(mesh.find("height")->as_int());
            auto xref = decode_values<std::uint32_t>(mesh.find("xref")->as_string());
            auto uv = decode_values<float>(mesh.find("uv2")->as_string());
            auto indices = decode_values<std::uint32_t>(mesh.find("indices")->as_string());
            if (!xref) {
                errors.push_back({at + "/xref", "isn't base64 of 32-bit integers"});
            }
            if (!uv) {
                errors.push_back({at + "/uv2", "isn't base64 of 32-bit floats"});
            }
            if (!indices) {
                errors.push_back({at + "/indices", "isn't base64 of 32-bit integers"});
            }
            if (!xref || !uv || !indices) {
                continue;
            }
            std::vector<std::uint32_t>& vertices = *xref;
            const std::vector<float>& uvs = *uv;
            std::vector<std::uint32_t>& triangles = *indices;
            if (uvs.size() != vertices.size() * 2) {
                errors.push_back({at + "/uv2", "has " + std::to_string(uvs.size() / 2) +
                                                   " UVs for " + std::to_string(vertices.size()) +
                                                   " vertices; expected one each"});
                continue;
            }
            if (triangles.empty() || triangles.size() % 3 != 0) {
                errors.push_back({at + "/indices", "expected triangles: a multiple of 3 indices"});
                continue;
            }
            const auto bad = std::ranges::find_if(
                triangles, [&](std::uint32_t index) { return index >= vertices.size(); });
            if (bad != triangles.end()) {
                errors.push_back({at + "/indices", "names vertex " + std::to_string(*bad) + " of " +
                                                       std::to_string(vertices.size())});
                continue;
            }
            for (std::size_t i = 0; i < vertices.size(); ++i) {
                unwrap.uv2.push_back({uvs[i * 2], uvs[(i * 2) + 1]});
            }
            unwrap.xref = std::move(vertices);
            unwrap.indices = std::move(triangles);
            out.meshes.emplace(hash, std::move(unwrap));
        }
    }
    if (const json::Value* users = doc.find("users")) {
        const json::Array& list = users->as_array();
        for (std::size_t i = 0; i < list.size(); ++i) {
            const json::Value& entry = list[i];
            const std::string at = "/users/" + std::to_string(i);
            LightmapUser user;
            user.entity = entry.find("entity")->as_string();
            if (const json::Value* part = entry.find("part")) {
                user.part = static_cast<std::uint32_t>(part->as_int());
            }
            user.mesh = entry.find("mesh")->as_string();
            if (const json::Value* slice = entry.find("slice")) {
                user.slice = static_cast<std::uint32_t>(slice->as_int());
            }
            const json::Array& rect = entry.find("uv_rect")->as_array();
            for (std::size_t c = 0; c < 4; ++c) {
                user.uv_rect.at(c) = static_cast<float>(rect[c].as_double());
            }
            if (doc.find("meshes") == nullptr || doc.find("meshes")->find(user.mesh) == nullptr) {
                errors.push_back({at + "/mesh", "isn't one of meshes"});
            }
            if (user.slice >= slices) {
                errors.push_back(
                    {at + "/slice", "is past the last of " + std::to_string(slices) + " slices"});
            }
            if (user.uv_rect[0] + user.uv_rect[2] > 1.0F ||
                user.uv_rect[1] + user.uv_rect[3] > 1.0F) {
                errors.push_back({at + "/uv_rect", "reaches past the slice"});
            }
            out.users.push_back(std::move(user));
        }
    }
    if (const json::Value* probes = doc.find("probes")) {
        out.probes = read_probes(*probes, errors);
    }
    if (data != nullptr) {
        *data = std::move(out);
    }
}

std::size_t lightmap_bytes(const LightmapData& data) {
    std::size_t bytes = sizeof(LightmapData) + (data.users.capacity() * sizeof(LightmapUser));
    for (const auto& [hash, unwrap] : data.meshes) {
        bytes += hash.capacity() + (unwrap.xref.capacity() * sizeof(std::uint32_t)) +
                 (unwrap.uv2.capacity() * sizeof(std::array<float, 2>)) +
                 (unwrap.indices.capacity() * sizeof(std::uint32_t));
    }
    const LightmapProbes& probes = data.probes;
    bytes += (probes.points.capacity() * sizeof(std::array<float, 3>)) +
             (probes.sh.capacity() * sizeof(LightmapSh)) +
             (probes.tetrahedra.capacity() * sizeof(std::array<std::uint32_t, 4>)) +
             (probes.bsp.capacity() * sizeof(LightmapBspNode));
    return bytes;
}

// ---- Baking a world

// sRGB to linear, as GPUs read an sRGB texture, the same on every platform.
float srgb_to_linear(float c) {
    if (c <= 0.04045F) {
        return c / 12.92F;
    }
    return slash::exp(2.4F * slash::ln((c + 0.055F) / 1.055F));
}

std::array<float, 3> linear_rgb(const Vec4& srgb) {
    return {srgb_to_linear(srgb.x), srgb_to_linear(srgb.y), srgb_to_linear(srgb.z)};
}

render::CullMode cull_of(const std::string& name) {
    if (name == "front") {
        return render::CullMode::Front;
    }
    return name == "disabled" ? render::CullMode::Disabled : render::CullMode::Back;
}

float channel_of(const std::array<float, 4>& texel, const std::string& name) {
    if (name == "green") {
        return texel[1];
    }
    if (name == "blue") {
        return texel[2];
    }
    if (name == "alpha") {
        return texel[3];
    }
    if (name == "gray") {
        return (texel[0] + texel[1] + texel[2]) / 3.0F;
    }
    return texel[0];
}

// A material's textures, as images where they could be found.
struct MaterialImages {
    std::shared_ptr<const render::Image> albedo;
    std::shared_ptr<const render::Image> metallic;
    std::shared_ptr<const render::Image> emission;
};

// A StandardMaterial3D as a bake sees it: what StandardMaterial3D's shader gives as albedo and
// emission at a point, Godot's bake_render_uv2, with metals' albedo taken out (they reflect
// rather than scatter light).
void set_material(LightmapMesh::Surface& surface, const world::StandardMaterial3D& standard,
                  const MaterialImages& images) {
    const bool nearest = standard.texture_filter.starts_with("nearest");
    const render::Filter filter = nearest ? render::Filter::Nearest : render::Filter::Linear;
    const bool repeat = standard.texture_repeat;
    const auto sample = [filter, repeat, standard](const render::Image& image,
                                                   const LightmapPoint& at) {
        const float u = (at.uv[0] * standard.uv1_scale.x) + standard.uv1_offset.x;
        const float v = (at.uv[1] * standard.uv1_scale.y) + standard.uv1_offset.y;
        return render::sample_image(image, filter, repeat, u, v);
    };
    surface.cull = cull_of(standard.cull_mode);
    surface.albedo = [standard, images, sample](const LightmapPoint& at) {
        std::array<float, 4> color{standard.albedo_color.x, standard.albedo_color.y,
                                   standard.albedo_color.z, standard.albedo_color.w};
        if (images.albedo) {
            const std::array<float, 4> texel = sample(*images.albedo, at);
            for (std::size_t c = 0; c < 4; ++c) {
                color.at(c) *= texel.at(c);
            }
        }
        if (standard.vertex_color_use_as_albedo) {
            for (std::size_t c = 0; c < 4; ++c) {
                color.at(c) *= at.color.at(c);
            }
        }
        float metallic = standard.metallic;
        if (images.metallic) {
            metallic *= channel_of(sample(*images.metallic, at), standard.metallic_texture_channel);
        }
        const float scatter = 1.0F - std::clamp(metallic, 0.0F, 1.0F);
        const float alpha = standard.transparency == "disabled" ? 1.0F : color[3];
        return std::array<float, 4>{srgb_to_linear(color[0]) * scatter,
                                    srgb_to_linear(color[1]) * scatter,
                                    srgb_to_linear(color[2]) * scatter, alpha};
    };
    if (!standard.emission_enabled) {
        surface.emission = [](const LightmapPoint& /*at*/) {
            return std::array<float, 3>{0.0F, 0.0F, 0.0F};
        };
        return;
    }
    surface.emission = [standard, images, sample](const LightmapPoint& at) {
        std::array<float, 3> color{standard.emission.x, standard.emission.y, standard.emission.z};
        if (images.emission) {
            const std::array<float, 4> texel = sample(*images.emission, at);
            for (std::size_t c = 0; c < 3; ++c) {
                color.at(c) += texel.at(c);
            }
        }
        const float energy = standard.emission_energy_multiplier;
        return std::array<float, 3>{srgb_to_linear(color[0]) * energy,
                                    srgb_to_linear(color[1]) * energy,
                                    srgb_to_linear(color[2]) * energy};
    };
}

// Godot's default material: white, lit, back faces culled.
void set_default_material(LightmapMesh::Surface& surface) {
    set_material(surface, world::StandardMaterial3D{}, {});
}

// Whether entity is in the subtree under scope (every entity for a null scope).
bool under(const world::Tree3D& tree, entt::entity entity, entt::entity scope) {
    if (scope == entt::null) {
        return true;
    }
    for (entt::entity at = tree.parent(entity); at != entt::null; at = tree.parent(at)) {
        if (at == scope) {
            return true;
        }
    }
    return false;
}

template <typename T>
std::vector<entt::entity> made(const world::World& world) {
    std::vector<entt::entity> out;
    for (const entt::entity entity : world.ecs().view<const T>()) {
        out.push_back(entity);
    }
    std::ranges::sort(out, {}, [](entt::entity entity) { return entt::to_entity(entity); });
    return out;
}

class Gatherer {
public:
    Gatherer(const world::World& world, const LightmapSources& sources, LightmapBake& out)
        : world_(world), sources_(sources), tree_(world), out_(out) {}

    const world::Tree3D& tree() const { return tree_; }

    // The image at a project path, or none with a problem.
    std::shared_ptr<const render::Image> image(const std::string& path, entt::entity entity,
                                               const std::string& field) {
        if (path.empty()) {
            return nullptr;
        }
        if (!sources_.images) {
            out_.problems.push_back({entity, field, "images can't load here"});
            return nullptr;
        }
        auto loaded = sources_.images(path);
        if (!loaded) {
            out_.problems.push_back({entity, field, loaded.error().message});
            return nullptr;
        }
        return loaded.value();
    }

    MaterialImages standard_images(const world::StandardMaterial3D& standard, entt::entity entity) {
        const std::string field = "/components/StandardMaterial3D/";
        return {.albedo = image(standard.albedo_texture, entity, field + "albedo_texture"),
                .metallic = image(standard.metallic_texture, entity, field + "metallic_texture"),
                .emission = standard.emission_enabled ? image(standard.emission_texture, entity,
                                                              field + "emission_texture")
                                                      : nullptr};
    }

    // The LightmapProbes under scope, in the order they were made, as places in space (a
    // LightmapGI's), as LightmapGI::_find_meshes_and_lights finds them.
    void add_probes(entt::entity scope, const world::Affine3D& space,
                    std::vector<std::array<float, 3>>& probes) const {
        for (const entt::entity entity : made<world::LightmapProbe>(world_)) {
            if (under(tree_, entity, scope)) {
                const Vec3 at = space.apply(tree_.global(entity).origin);
                probes.push_back({at.x, at.y, at.z});
            }
        }
    }

    void add_meshes(entt::entity scope, std::vector<LightmapMesh>& meshes) {
        const auto& ecs = world_.ecs();
        for (const entt::entity entity : made<world::MeshInstance3D>(world_)) {
            const auto& instance = ecs.get<const world::MeshInstance3D>(entity);
            if (instance.gi_mode != "static" || !tree_.visible(entity) ||
                !under(tree_, entity, scope)) {
                continue;
            }
            const std::string id(world_.id_of(entity));
            if (id.empty()) {
                out_.problems.push_back(
                    {entity, "/components/MeshInstance3D",
                     "has no ID, which its place in the lightmap is kept by; left out"});
                continue;
            }
            const world::Affine3D place = tree_.global(entity);
            const auto* standard = ecs.try_get<const world::StandardMaterial3D>(entity);
            std::optional<MaterialImages> override_images;
            if (standard != nullptr) {
                override_images = standard_images(*standard, entity);
            }
            if (!instance.mesh.empty()) {
                add_model(entity, id, instance, place, standard, override_images, meshes);
                continue;
            }
            auto primitive = primitive_of(world_, entity);
            if (!primitive) {
                continue; // nothing drawn, which the renderer reports
            }
            const auto& shape = primitive.value();
            if (!shape) {
                continue;
            }
            auto data = make_mesh(*shape);
            if (!data) {
                continue;
            }
            LightmapMesh mesh;
            mesh.entity = id;
            mesh.transform = place;
            mesh.vertices = std::move(data.value().vertices);
            mesh.indices = std::move(data.value().indices);
            mesh.texel_scale = instance.gi_lightmap_texel_scale;
            LightmapMesh::Surface surface;
            if (standard != nullptr && override_images) {
                set_material(surface, *standard, *override_images);
            } else {
                set_default_material(surface);
            }
            mesh.surfaces.push_back(std::move(surface));
            meshes.push_back(std::move(mesh));
        }
    }

    void add_lights(entt::entity scope, std::vector<LightmapLight>& lights) {
        add<world::DirectionalLight3D>(scope, lights);
        add<world::OmniLight3D>(scope, lights);
        add<world::SpotLight3D>(scope, lights);
    }

private:
    void add_model(entt::entity entity, const std::string& id,
                   const world::MeshInstance3D& instance, const world::Affine3D& place,
                   const world::StandardMaterial3D* standard,
                   const std::optional<MaterialImages>& override_images,
                   std::vector<LightmapMesh>& meshes) {
        const std::string field = "/components/MeshInstance3D/mesh";
        if (!sources_.models) {
            out_.problems.push_back({entity, field, "models can't load here"});
            return;
        }
        auto loaded = sources_.models(instance.mesh);
        if (!loaded) {
            out_.problems.push_back({entity, field, loaded.error().message});
            return;
        }
        const std::shared_ptr<const Model> model = loaded.value();
        // Each of the model's materials, made once.
        std::vector<std::optional<LightmapMesh::Surface>> materials(model->materials.size());
        const auto model_image = [&](int index) -> std::shared_ptr<const render::Image> {
            if (index < 0 || static_cast<std::size_t>(index) >= model->images.size()) {
                return nullptr;
            }
            const ModelImage& found = model->images[static_cast<std::size_t>(index)];
            if (!found.path.empty()) {
                return image(found.path, entity, field);
            }
            // Kept alive with the model.
            return {model, &found.image};
        };
        for (std::size_t p = 0; p < model->parts.size(); ++p) {
            const ModelPart& part = model->parts[p];
            if (part.skin >= 0) {
                out_.problems.push_back(
                    {entity, field,
                     "part " + std::to_string(p) + " (\"" + part.node +
                         "\") has a skin, which moves it, so it isn't baked: set gi_mode to "
                         "\"dynamic\" or bake a mesh without one"});
                continue;
            }
            const ModelMesh& from = model->meshes[part.mesh];
            LightmapMesh mesh;
            mesh.entity = id;
            mesh.part = static_cast<std::uint32_t>(p);
            mesh.transform = place * part.transform;
            mesh.texel_scale = instance.gi_lightmap_texel_scale;
            for (const ModelSurface& surface : from.surfaces) {
                const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
                LightmapMesh::Surface made;
                if (standard != nullptr && override_images) {
                    set_material(made, *standard, *override_images);
                } else if (surface.material >= 0 &&
                           static_cast<std::size_t>(surface.material) < materials.size()) {
                    auto& kept = materials[static_cast<std::size_t>(surface.material)];
                    if (!kept) {
                        const ModelMaterial& material =
                            model->materials[static_cast<std::size_t>(surface.material)];
                        kept.emplace();
                        set_material(*kept, material.standard,
                                     {.albedo = model_image(material.albedo_image),
                                      .metallic = model_image(material.metallic_image),
                                      .emission = material.standard.emission_enabled
                                                      ? model_image(material.emission_image)
                                                      : nullptr});
                    }
                    made = *kept;
                } else {
                    set_default_material(made);
                }
                made.first_index = static_cast<std::uint32_t>(mesh.indices.size());
                mesh.surfaces.push_back(std::move(made));
                mesh.vertices.insert(mesh.vertices.end(), surface.vertices.begin(),
                                     surface.vertices.end());
                for (const std::uint32_t index : surface.indices) {
                    mesh.indices.push_back(base + index);
                }
            }
            if (!mesh.indices.empty()) {
                meshes.push_back(std::move(mesh));
            }
        }
    }

    template <typename T>
    void add(entt::entity scope, std::vector<LightmapLight>& lights) {
        for (const entt::entity entity : made<T>(world_)) {
            const T& light = world_.ecs().get<const T>(entity);
            if (light.light_bake_mode == "disabled" || !tree_.visible(entity) ||
                !under(tree_, entity, scope)) {
                continue;
            }
            LightmapLight out;
            if constexpr (std::is_same_v<T, world::DirectionalLight3D>) {
                if (light.sky_mode == "sky_only") {
                    continue;
                }
                out.type = render::LightType::Directional;
                // As LightmapperRD::add_directional_light: the tangent of the angular distance.
                const Vec2 cs = slash::cos_sin(light.light_angular_distance * kPi / 180.0F);
                out.size = cs.x != 0.0F ? cs.y / cs.x : 0.0F;
            } else if constexpr (std::is_same_v<T, world::OmniLight3D>) {
                out.type = render::LightType::Omni;
                out.range = light.omni_range;
                out.attenuation = light.omni_attenuation;
                out.size = light.light_size;
            } else {
                out.type = render::LightType::Spot;
                out.range = light.spot_range;
                out.attenuation = light.spot_attenuation;
                out.spot_angle = light.spot_angle;
                out.spot_attenuation = light.spot_angle_attenuation;
                out.size = light.light_size;
            }
            out.color = linear_rgb(light.light_color);
            out.energy = light.light_energy;
            out.indirect_energy = light.light_indirect_energy;
            out.shadow_blur = light.shadow_blur;
            out.static_bake = light.light_bake_mode == "static";
            const world::Affine3D placed = tree_.global(entity);
            out.position = {placed.origin.x, placed.origin.y, placed.origin.z};
            const Vec3 forward = placed.z * -1.0F;
            out.direction = {forward.x, forward.y, forward.z};
            lights.push_back(out);
        }
    }

    const world::World& world_;
    const LightmapSources& sources_;
    world::Tree3D tree_;
    LightmapBake& out_;
};

// The light rays that leave the scene bring, as LightmapGI::bake's environment.
render::HdrImage environment_of(const world::World& world, const world::Tree3D& tree,
                                const world::LightmapGI& gi, Gatherer& gatherer) {
    render::HdrImage out;
    if (gi.environment_mode == "disabled") {
        return out;
    }
    out.width = kEnvironmentWidth;
    out.height = kEnvironmentHeight;
    if (gi.environment_mode == "custom_color") {
        // As Godot's: the color times its energy, as it is.
        const Vec4& c = gi.environment_custom_color;
        const float energy = gi.environment_custom_energy;
        out.rgb.reserve(std::size_t{out.width} * out.height * 3);
        for (std::uint32_t i = 0; i < out.width * out.height; ++i) {
            out.rgb.push_back(c.x * energy);
            out.rgb.push_back(c.y * energy);
            out.rgb.push_back(c.z * energy);
        }
        return out;
    }
    // "scene": the ambient light the WorldEnvironment gives lit surfaces, as Godot's
    // environment_bake_panorama, before any camera's exposure.
    const Environment env = world_environment(world, {});
    render::Scene3D scene;
    scene.ambient = env.ambient;
    scene.ambient_energy = env.ambient_energy;
    scene.sky = env.sky;
    scene.radiance = env.radiance;
    scene.post = env.post;
    if (env.sky.kind != render::SkyKind::None || env.radiance.kind != render::SkyKind::None) {
        scene.sky.lights = sky_lights(world, tree, [&](entt::entity e) { return tree.global(e); });
    }
    std::shared_ptr<const render::Image> texture;
    if (!env.sky_texture.empty()) {
        texture = gatherer.image(env.sky_texture, env.entity, env.sky_texture_field);
    }
    const render::Filter filter = env.sky_filter;
    return render::ambient_panorama(
        scene, out.width, out.height, [&](const std::array<float, 2>& at) {
            return texture ? render::sample_image(*texture, filter, true, at[0], at[1])
                           : std::array<float, 4>{1.0F, 1.0F, 1.0F, 1.0F};
        });
}

} // namespace

Result<LightmapUnwrap> unwrap_lightmap(std::span<const render::Vertex3D> vertices,
                                       std::span<const std::uint32_t> indices, float texel_size) {
    if (!(texel_size > 0.0F)) {
        return make_error(ErrorCode::InvalidArgument, "the texel size must be above 0");
    }
    if (indices.empty() || indices.size() % 3 != 0) {
        return make_error(ErrorCode::InvalidArgument, "the mesh has no triangles to unwrap");
    }
    for (const std::uint32_t index : indices) {
        if (index >= vertices.size()) {
            return make_error(ErrorCode::InvalidArgument, "the mesh's indices name vertex " +
                                                              std::to_string(index) + " of " +
                                                              std::to_string(vertices.size()));
        }
    }
    std::vector<float> positions;
    std::vector<float> normals;
    positions.reserve(vertices.size() * 3);
    normals.reserve(vertices.size() * 3);
    for (const render::Vertex3D& v : vertices) {
        positions.insert(positions.end(), v.position.begin(), v.position.end());
        normals.insert(normals.end(), v.normal.begin(), v.normal.end());
    }
    // xatlas keeps its print function and allocator in globals; one unwrap at a time.
    static std::mutex mutex;
    const std::scoped_lock lock(mutex);
    xatlas::SetPrint(nullptr, false);
    xatlas::Atlas* atlas = xatlas::Create();
    // As Godot's xatlas_mesh_lightmap_unwrap_callback.
    xatlas::MeshDecl input;
    input.indexData = indices.data();
    input.indexCount = static_cast<std::uint32_t>(indices.size());
    input.indexFormat = xatlas::IndexFormat::UInt32;
    input.vertexCount = static_cast<std::uint32_t>(vertices.size());
    input.vertexPositionData = positions.data();
    input.vertexPositionStride = sizeof(float) * 3;
    input.vertexNormalData = normals.data();
    input.vertexNormalStride = sizeof(float) * 3;
    const xatlas::AddMeshError added = xatlas::AddMesh(atlas, input, 1);
    if (added != xatlas::AddMeshError::Success) {
        xatlas::Destroy(atlas);
        return make_error(ErrorCode::InvalidArgument, std::string("the mesh can't be unwrapped: ") +
                                                          xatlas::StringForEnum(added));
    }
    xatlas::ChartOptions chart_options;
    chart_options.fixWinding = true;
    xatlas::PackOptions pack_options;
    pack_options.padding = 1;
    pack_options.maxChartSize = 4094; // lightmap atlases put 2 texels between meshes
    pack_options.blockAlign = true;
    pack_options.texelsPerUnit = 1.0F / texel_size;
    xatlas::Generate(atlas, chart_options, pack_options);
    if (atlas->width == 0 || atlas->height == 0 || atlas->meshCount == 0) {
        xatlas::Destroy(atlas);
        return make_error(ErrorCode::InvalidArgument,
                          "the mesh has no triangle of any area to unwrap");
    }
    LightmapUnwrap out;
    out.width = atlas->width;
    out.height = atlas->height;
    const auto w = static_cast<float>(atlas->width);
    const auto h = static_cast<float>(atlas->height);
    const xatlas::Mesh& output = atlas->meshes[0];
    out.xref.reserve(output.vertexCount);
    out.uv2.reserve(output.vertexCount);
    for (std::uint32_t i = 0; i < output.vertexCount; ++i) {
        out.xref.push_back(output.vertexArray[i].xref);
        out.uv2.push_back({output.vertexArray[i].uv[0] / w, output.vertexArray[i].uv[1] / h});
    }
    out.indices.assign(output.indexArray, output.indexArray + output.indexCount);
    xatlas::Destroy(atlas);
    return out;
}

std::string lightmap_mesh_hash(std::span<const render::Vertex3D> vertices,
                               std::span<const std::uint32_t> indices) {
    std::string bytes;
    bytes.reserve((vertices.size() * 24) + (indices.size() * 4));
    const auto put = [&](const void* data, std::size_t size) {
        bytes.append(static_cast<const char*>(data), size);
    };
    for (const render::Vertex3D& v : vertices) {
        put(v.position.data(), sizeof(v.position));
    }
    for (const render::Vertex3D& v : vertices) {
        put(v.normal.data(), sizeof(v.normal));
    }
    put(indices.data(), indices.size_bytes());
    return sha256_hex(bytes).substr(0, 16);
}

json::Value lightmap_schema() {
    return file_schema();
}

std::vector<json::ValidationError> validate_lightmap(const json::Value& doc) {
    std::vector<json::ValidationError> errors = json::validate(doc, file_schema());
    if (errors.empty()) {
        read_lightmap(doc, nullptr, errors);
    }
    return errors;
}

Result<LightmapData> parse_lightmap(const json::Value& doc) {
    std::vector<json::ValidationError> errors = json::validate(doc, file_schema());
    if (!errors.empty()) {
        return first_error(errors);
    }
    LightmapData data;
    read_lightmap(doc, &data, errors);
    if (!errors.empty()) {
        return first_error(errors);
    }
    return data;
}

json::Value lightmap_json(const LightmapData& data) {
    json::Value doc;
    doc["format"] = 1;
    doc["texture"] = data.texture;
    doc["width"] = static_cast<std::int64_t>(data.width);
    doc["height"] = static_cast<std::int64_t>(data.height);
    doc["slices"] = static_cast<std::int64_t>(data.slices);
    if (data.baked_exposure != 1.0F) {
        doc["baked_exposure"] = data.baked_exposure;
    }
    if (!data.meshes.empty()) {
        json::Value& meshes = doc["meshes"];
        meshes = json::Object{};
        for (const auto& [hash, unwrap] : data.meshes) {
            json::Value& mesh = meshes[hash];
            mesh["width"] = static_cast<std::int64_t>(unwrap.width);
            mesh["height"] = static_cast<std::int64_t>(unwrap.height);
            mesh["xref"] = encode_values(unwrap.xref);
            std::vector<float> uv;
            uv.reserve(unwrap.uv2.size() * 2);
            for (const std::array<float, 2>& at : unwrap.uv2) {
                uv.push_back(at[0]);
                uv.push_back(at[1]);
            }
            mesh["uv2"] = encode_values(uv);
            mesh["indices"] = encode_values(unwrap.indices);
        }
    }
    if (!data.users.empty()) {
        json::Array users;
        for (const LightmapUser& user : data.users) {
            json::Value entry;
            entry["entity"] = user.entity;
            if (user.part != 0) {
                entry["part"] = static_cast<std::int64_t>(user.part);
            }
            entry["mesh"] = user.mesh;
            if (user.slice != 0) {
                entry["slice"] = static_cast<std::int64_t>(user.slice);
            }
            entry["uv_rect"] =
                json::Array{user.uv_rect[0], user.uv_rect[1], user.uv_rect[2], user.uv_rect[3]};
            users.push_back(std::move(entry));
        }
        doc["users"] = std::move(users);
    }
    const LightmapProbes& probes = data.probes;
    if (!probes.points.empty()) {
        json::Value& entry = doc["probes"];
        json::Array bounds;
        for (const float v : probes.bounds_position) {
            bounds.emplace_back(v);
        }
        for (const float v : probes.bounds_size) {
            bounds.emplace_back(v);
        }
        entry["bounds"] = std::move(bounds);
        if (probes.interior) {
            entry["interior"] = true;
        }
        std::vector<float> points;
        for (const std::array<float, 3>& point : probes.points) {
            points.insert(points.end(), point.begin(), point.end());
        }
        entry["points"] = encode_values(points);
        std::vector<float> sh;
        for (const LightmapSh& light : probes.sh) {
            for (const std::array<float, 3>& coefficient : light) {
                sh.insert(sh.end(), coefficient.begin(), coefficient.end());
            }
        }
        entry["sh"] = encode_values(sh);
        std::vector<std::uint32_t> tetrahedra;
        for (const std::array<std::uint32_t, 4>& tetrahedron : probes.tetrahedra) {
            tetrahedra.insert(tetrahedra.end(), tetrahedron.begin(), tetrahedron.end());
        }
        entry["tetrahedra"] = encode_values(tetrahedra);
        entry["bsp"] = encode_values(probes.bsp);
    }
    return doc;
}

void add_lightmap_types(asset::Assets& assets) {
    assets.add_type<LightmapData>(
        "lightmap", {std::string(kLightmapSuffix)},
        [](std::string_view text, std::string_view /*path*/) -> Result<LightmapData> {
            auto doc = json::parse(text);
            if (!doc) {
                return doc.error();
            }
            return parse_lightmap(doc.value());
        },
        lightmap_bytes);
    assets.add_type<render::HdrImage>(
        "hdr_image", {".hdr"},
        [](std::string_view bytes, std::string_view /*path*/) { return render::decode_hdr(bytes); },
        [](const render::HdrImage& image) {
            return sizeof(render::HdrImage) + (image.rgb.capacity() * sizeof(float));
        });
}

Result<LightmapBake> bake_lightmap(const world::World& world, entt::entity entity,
                                   const LightmapSources& sources) {
    if (!world.valid(entity) || !world.ecs().all_of<world::LightmapGI>(entity)) {
        return make_error(ErrorCode::InvalidArgument, "the entity has no LightmapGI");
    }
    const auto& gi = world.ecs().get<const world::LightmapGI>(entity);
    LightmapSettings settings;
    if (gi.quality == "low") {
        settings.rays = 32;
    } else if (gi.quality == "high") {
        settings.rays = 512;
    } else if (gi.quality == "ultra") {
        settings.rays = 2048;
    } else {
        settings.rays = 128;
    }
    settings.bounces = static_cast<std::uint32_t>(std::max(gi.bounces, 0));
    settings.bounce_indirect_energy = gi.bounce_indirect_energy;
    settings.use_texture_for_bounces = gi.use_texture_for_bounces;
    settings.use_denoiser = gi.use_denoiser;
    settings.denoiser_strength = gi.denoiser_strength;
    settings.denoiser_range = static_cast<std::uint32_t>(std::max(gi.denoiser_range, 1));
    settings.bias = gi.bias;
    settings.texel_scale = gi.texel_scale;
    settings.max_texture_size = static_cast<std::uint32_t>(std::max(gi.max_texture_size, 1));
    settings.supersampling = gi.supersampling ? gi.supersampling_factor : 1.0F;
    if (!gi.camera_attributes.empty()) {
        const entt::entity attributes = world.find(gi.camera_attributes);
        if (attributes == entt::null ||
            !world.ecs().all_of<world::CameraAttributesPractical>(attributes)) {
            return make_error(ErrorCode::InvalidArgument,
                              "camera_attributes \"" + gi.camera_attributes +
                                  "\" isn't an entity with CameraAttributesPractical");
        }
        settings.exposure =
            world.ecs().get<const world::CameraAttributesPractical>(attributes).exposure_multiplier;
    }

    LightmapBake gathered;
    Gatherer gatherer(world, sources, gathered);
    const entt::entity scope = gatherer.tree().parent(entity);
    std::vector<LightmapMesh> meshes;
    gatherer.add_meshes(scope, meshes);
    std::vector<LightmapLight> lights;
    gatherer.add_lights(scope, lights);
    const render::HdrImage environment = environment_of(world, gatherer.tree(), gi, gatherer);

    // The light probes, placed in the LightmapGI's space as LightmapGI::bake places them, from
    // the meshes' triangles and the LightmapProbes.
    const world::Affine3D space = gatherer.tree().global(entity);
    const world::Affine3D to_space = space.inverse();
    std::vector<std::array<float, 3>> triangles;
    for (const LightmapMesh& mesh : meshes) {
        const world::Affine3D to_gi = to_space * mesh.transform;
        for (const std::uint32_t index : mesh.indices) {
            if (index < mesh.vertices.size()) {
                const std::array<float, 3>& p = mesh.vertices[index].position;
                const Vec3 at = to_gi.apply({p[0], p[1], p[2]});
                triangles.push_back({at.x, at.y, at.z});
            }
        }
    }
    std::vector<std::array<float, 3>> user;
    gatherer.add_probes(scope, to_space, user);
    int subdiv = 0;
    if (gi.generate_probes_subdiv != "disabled") {
        subdiv = std::max(std::atoi(gi.generate_probes_subdiv.c_str()), 4);
    }
    LightmapProbes probes = place_lightmap_probes(triangles, user, subdiv);
    LightmapProbeRequest request;
    request.space = space;
    for (const std::array<float, 3>& point : probes.points) {
        const Vec3 at = space.apply({point[0], point[1], point[2]});
        request.points.push_back({at.x, at.y, at.z});
    }
    // rendering/lightmapping/bake_quality/*_quality_probe_ray_count.
    if (gi.quality == "low") {
        request.rays = 64;
    } else if (gi.quality == "high") {
        request.rays = 512;
    } else if (gi.quality == "ultra") {
        request.rays = 2048;
    } else {
        request.rays = 256;
    }

    auto baked = bake_lightmap(meshes, lights, environment, settings, &world.pool(), request);
    if (!baked) {
        return baked.error();
    }
    LightmapBake out = std::move(baked).value();
    probes.sh = std::move(out.probe_sh);
    out.probe_sh.clear();
    probes.tetrahedra = tetrahedralize(probes.points);
    probes.bsp = lightmap_bsp_tree(probes.points, probes.tetrahedra, &world.pool());
    probes.interior = gi.interior;
    out.data.probes = std::move(probes);
    out.problems.insert(out.problems.begin(), gathered.problems.begin(), gathered.problems.end());
    return out;
}

std::vector<Problem> check_lightmaps(const world::World& world) {
    std::vector<Problem> problems;
    std::vector<entt::entity> entities;
    for (const entt::entity entity : world.ecs().view<const world::LightmapGI>()) {
        entities.push_back(entity);
    }
    std::ranges::sort(entities, {}, [](entt::entity entity) { return entt::to_entity(entity); });
    for (const entt::entity entity : entities) {
        const auto& gi = world.ecs().get<const world::LightmapGI>(entity);
        if (!gi.camera_attributes.empty()) {
            const entt::entity attributes = world.find(gi.camera_attributes);
            if (attributes == entt::null ||
                !world.ecs().all_of<world::CameraAttributesPractical>(attributes)) {
                problems.push_back({entity, "/components/LightmapGI/camera_attributes",
                                    "\"" + gi.camera_attributes +
                                        "\" isn't an entity with CameraAttributesPractical"});
            }
        }
        const std::string& path = gi.light_data;
        if (path.empty()) {
            continue;
        }
        const std::string field = "/components/LightmapGI/light_data";
        if (!path.ends_with(kLightmapSuffix)) {
            problems.push_back(
                {entity, field,
                 "\"" + path + "\" isn't a " + std::string(kLightmapSuffix) + " file"});
            continue;
        }
        const world::FileReader& reader = world.file_reader();
        if (!reader) {
            continue;
        }
        auto text = reader(path);
        if (!text) {
            problems.push_back(
                {entity, field, "\"" + path + "\" could not be read: " + text.error().message});
            continue;
        }
        auto doc = json::parse(text.value());
        if (!doc) {
            problems.push_back({entity, field, "\"" + path + "\": " + doc.error().message});
            continue;
        }
        const std::vector<json::ValidationError> errors = validate_lightmap(doc.value());
        if (!errors.empty()) {
            problems.push_back(
                {entity, field,
                 "\"" + path + "\" at " + errors.front().path + ": " + errors.front().message});
            continue;
        }
        const std::string texture = doc.value().find("texture")->as_string();
        if (auto texels = reader(texture); !texels) {
            problems.push_back(
                {entity, field,
                 "its texture \"" + texture + "\" could not be read: " + texels.error().message});
        }
    }
    return problems;
}

std::string lightmap_texture_path(std::string_view data_path) {
    if (data_path.ends_with(kLightmapSuffix)) {
        return std::string(data_path.substr(0, data_path.size() - kLightmapSuffix.size())) +
               ".lightmap.hdr";
    }
    return std::string(data_path) + ".hdr";
}

} // namespace slash::scene3d
