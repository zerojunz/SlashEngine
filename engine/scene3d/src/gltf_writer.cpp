#include "model_internal.hpp"

#include "slash/core/json.hpp"
#include "slash/core/text.hpp"
#include "slash/render/image.hpp"
#include "slash/scene3d/model.hpp"
#include "slash/scene3d/skeleton.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

// Models written as glTF 2.0, as Godot's GLTFDocument exports a scene (_serialize_nodes,
// _serialize_meshes, _serialize_materials, _serialize_images), and as load_model reads them back.
namespace slash::scene3d {
namespace {

constexpr int kFloat = 5126;
constexpr int kUnsignedInt = 5125;
constexpr int kUnsignedShort = 5123;
constexpr int kUnsignedByte = 5121;
constexpr int kArrayBuffer = 34962;
constexpr int kElementArrayBuffer = 34963;

// Godot's Color::srgb_to_linear, for one channel.
float srgb_to_linear(float c) {
    return c < 0.04045F
               ? c * (1.0F / 12.92F)
               : static_cast<float>(std::pow((static_cast<double>(c) + 0.055) / 1.055, 2.4));
}

json::Array linear_color(const Vec4& srgb, bool alpha) {
    json::Array out{srgb_to_linear(srgb.x), srgb_to_linear(srgb.y), srgb_to_linear(srgb.z)};
    if (alpha) {
        out.emplace_back(srgb.w);
    }
    return out;
}

// A path relative to a folder, both project paths, as a URI: "../textures/a%20b.png".
std::string relative_uri(std::string_view folder, std::string_view path) {
    std::vector<std::string_view> from;
    std::vector<std::string_view> to;
    const auto split = [](std::string_view text, std::vector<std::string_view>& parts) {
        std::size_t start = 0;
        while (start < text.size()) {
            const std::size_t slash = std::min(text.find('/', start), text.size());
            parts.push_back(text.substr(start, slash - start));
            start = slash + 1;
        }
    };
    split(folder, from);
    split(path, to);
    std::size_t same = 0;
    while (same < from.size() && same + 1 < to.size() && from[same] == to[same]) {
        ++same;
    }
    std::string relative;
    for (std::size_t i = same; i < from.size(); ++i) {
        relative += "../";
    }
    for (std::size_t i = same; i < to.size(); ++i) {
        relative += to[i];
        if (i + 1 < to.size()) {
            relative += '/';
        }
    }
    // Percent-encoded as RFC 3986 asks of a URI's path, leaving '/' and its unreserved marks.
    std::string uri;
    constexpr std::string_view kHex = "0123456789ABCDEF";
    for (const char c : relative) {
        const auto byte = static_cast<unsigned char>(c);
        if (std::isalnum(byte) != 0 || c == '/' || c == '-' || c == '.' || c == '_' || c == '~') {
            uri += c;
        } else {
            uri += '%';
            uri += kHex[byte >> 4U];
            uri += kHex[byte & 0xFU];
        }
    }
    return uri;
}

template <typename T>
void append(std::string& bin, const std::vector<T>& values) {
    const std::size_t at = bin.size();
    bin.resize(at + (values.size() * sizeof(T)));
    if (!values.empty()) {
        std::memcpy(bin.data() + at, values.data(), values.size() * sizeof(T));
    }
}

struct Writer {
    const Model& model;
    std::string folder; // the file's folder, which image paths are relative to
    json::Value document;
    std::string bin;
    std::map<std::string, int, std::less<>> image_paths;    // by project path
    std::map<int, int> model_images;                        // by index in the model
    std::map<std::tuple<int, int, int, int>, int> samplers; // by filters and wraps
    std::map<std::pair<int, int>, int> textures;            // by image and sampler
    std::vector<std::string> extensions;
    // A .glb file's: every image in the buffer, those beside the model read with read.
    const ModelReader* read = nullptr;

    void uses(std::string_view extension) {
        if (std::ranges::find(extensions, extension) == extensions.end()) {
            extensions.emplace_back(extension);
        }
    }

    int add(std::string_view key, json::Value item) {
        json::Value& list = document[key];
        if (list.is_null()) {
            list = json::Array{};
        }
        list.push_back(std::move(item));
        return static_cast<int>(list.size()) - 1;
    }

    // Bytes as a buffer view, at a multiple of four as accessors need; target 0 for none.
    int view(std::string_view bytes, int target) {
        while (bin.size() % 4 != 0) {
            bin.push_back('\0');
        }
        json::Value out = json::Object{};
        out["buffer"] = 0;
        out["byteOffset"] = static_cast<std::int64_t>(bin.size());
        out["byteLength"] = static_cast<std::int64_t>(bytes.size());
        if (target != 0) {
            out["target"] = target;
        }
        bin.append(bytes);
        return add("bufferViews", std::move(out));
    }

    template <typename T>
    int accessor(const std::vector<T>& values, std::size_t per, int component, std::string type,
                 int target, bool normalized = false) {
        std::string bytes;
        append(bytes, values);
        json::Value out = json::Object{};
        out["bufferView"] = view(bytes, target);
        out["componentType"] = component;
        out["count"] = static_cast<std::int64_t>(values.size() / per);
        out["type"] = std::move(type);
        if (normalized) {
            out["normalized"] = true;
        }
        return add("accessors", std::move(out));
    }

    // An image in the buffer, as a .glb file holds it: a PNG or JPEG file as it is, anything
    // else as a PNG.
    Result<int> embedded(const ModelImage& in) {
        std::string bytes;
        std::string mime = "image/png";
        if (in.path.empty()) {
            auto png = render::encode_png(in.image);
            if (!png) {
                return png.error();
            }
            bytes = std::move(png).value();
        } else {
            auto file = (*read)(in.path);
            if (!file) {
                return file.error();
            }
            std::string lower = in.path;
            std::ranges::transform(lower, lower.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            if (lower.ends_with(".jpg") || lower.ends_with(".jpeg")) {
                mime = "image/jpeg";
                bytes = std::move(file).value();
            } else if (lower.ends_with(".png")) {
                bytes = std::move(file).value();
            } else {
                auto decoded = render::decode_image(file.value());
                if (!decoded) {
                    return make_error(ErrorCode::InvalidArgument,
                                      "\"" + in.path + "\": " + decoded.error().message);
                }
                auto png = render::encode_png(decoded.value());
                if (!png) {
                    return png.error();
                }
                bytes = std::move(png).value();
            }
        }
        json::Value out = json::Object{};
        out["bufferView"] = view(bytes, 0);
        out["mimeType"] = mime;
        const int index = add("images", std::move(out));
        if (!in.path.empty()) {
            image_paths.emplace(in.path, index);
        }
        return index;
    }

    // An image, by its index in the model or by the path of a file a material names.
    Result<int> image(const ModelImage& in) {
        if (read != nullptr) {
            if (const auto it = image_paths.find(in.path);
                !in.path.empty() && it != image_paths.end()) {
                return it->second;
            }
            return embedded(in);
        }
        if (!in.path.empty()) {
            if (const auto it = image_paths.find(in.path); it != image_paths.end()) {
                return it->second;
            }
            json::Value out = json::Object{};
            out["uri"] = relative_uri(folder, in.path);
            const int index = add("images", std::move(out));
            image_paths.emplace(in.path, index);
            return index;
        }
        auto png = render::encode_png(in.image);
        if (!png) {
            return png.error();
        }
        json::Value out = json::Object{};
        out["uri"] = "data:image/png;base64," + base64_encode(png.value());
        return add("images", std::move(out));
    }

    // Godot's GLTFTextureSampler: the material's filter and repeat, as glTF's sampler.
    int sampler(const world::StandardMaterial3D& standard) {
        const std::string& filter = standard.texture_filter;
        const bool nearest = filter.starts_with("nearest");
        const int mag = nearest ? 9728 : 9729;
        int min = mag;
        if (filter == "nearest_with_mipmaps") {
            min = 9984;
        } else if (filter == "linear_with_mipmaps") {
            min = 9987;
        }
        const int wrap = standard.texture_repeat ? 10497 : 33071;
        const auto key = std::tuple{mag, min, wrap, wrap};
        if (const auto it = samplers.find(key); it != samplers.end()) {
            return it->second;
        }
        json::Value out = json::Object{};
        out["magFilter"] = mag;
        out["minFilter"] = min;
        out["wrapS"] = wrap;
        out["wrapT"] = wrap;
        const int index = add("samplers", std::move(out));
        samplers.emplace(key, index);
        return index;
    }

    // A texture of one of the model's images, or of a file a material names, sampled as the
    // material says; nullopt for neither.
    Result<std::optional<int>> texture(int model_image, const std::string& path,
                                       const world::StandardMaterial3D& standard) {
        Result<int> source = -1;
        if (const auto it = model_images.find(model_image); it != model_images.end()) {
            source = it->second;
        } else if (model_image >= 0 &&
                   static_cast<std::size_t>(model_image) < model.images.size()) {
            source = image(model.images[static_cast<std::size_t>(model_image)]);
            if (source) {
                model_images.emplace(model_image, source.value());
            }
        } else if (!path.empty()) {
            source = image(ModelImage{.path = path, .image = {}});
        } else {
            return std::optional<int>();
        }
        if (!source) {
            return source.error();
        }
        const int with = sampler(standard);
        const auto key = std::pair{source.value(), with};
        if (const auto it = textures.find(key); it != textures.end()) {
            return std::optional<int>(it->second);
        }
        json::Value out = json::Object{};
        out["source"] = source.value();
        out["sampler"] = with;
        const int index = add("textures", std::move(out));
        textures.emplace(key, index);
        return std::optional<int>(index);
    }

    // Godot's GLTFDocument::_serialize_materials, undoing what material_of reads.
    Result<json::Value> material(const ModelMaterial& in) {
        const world::StandardMaterial3D& standard = in.standard;
        json::Value out = json::Object{};
        out["name"] = in.name;
        json::Value pbr = json::Object{};
        pbr["baseColorFactor"] = linear_color(standard.albedo_color, true);
        pbr["metallicFactor"] = standard.metallic;
        pbr["roughnessFactor"] = standard.roughness;
        auto albedo = texture(in.albedo_image, standard.albedo_texture, standard);
        if (!albedo) {
            return albedo.error();
        }
        if (const std::optional<int> index = albedo.value()) {
            json::Value view = json::Object{};
            view["index"] = *index;
            const Vec3& scale = standard.uv1_scale;
            const Vec3& offset = standard.uv1_offset;
            if (scale.x != 1.0F || scale.y != 1.0F || offset.x != 0.0F || offset.y != 0.0F) {
                json::Value transform = json::Object{};
                transform["offset"] = json::Array{offset.x, offset.y};
                transform["scale"] = json::Array{scale.x, scale.y};
                view["extensions"]["KHR_texture_transform"] = std::move(transform);
                uses("KHR_texture_transform");
            }
            pbr["baseColorTexture"] = std::move(view);
        }
        // glTF's metallic and roughness share one image, read from its blue and green.
        const bool shared =
            standard.metallic_texture_channel == "blue" &&
            standard.roughness_texture_channel == "green" &&
            (in.metallic_image >= 0 ? in.metallic_image == in.roughness_image
                                    : in.roughness_image < 0 &&
                                          standard.metallic_texture == standard.roughness_texture);
        if (shared) {
            auto both = texture(in.metallic_image, standard.metallic_texture, standard);
            if (!both) {
                return both.error();
            }
            if (const std::optional<int> index = both.value()) {
                pbr["metallicRoughnessTexture"]["index"] = *index;
            }
        }
        out["pbrMetallicRoughness"] = std::move(pbr);
        if (standard.normal_enabled) {
            auto normal = texture(in.normal_image, standard.normal_texture, standard);
            if (!normal) {
                return normal.error();
            }
            if (const std::optional<int> index = normal.value()) {
                out["normalTexture"]["index"] = *index;
                if (standard.normal_scale != 1.0F) {
                    out["normalTexture"]["scale"] = standard.normal_scale;
                }
            }
        }
        if (standard.emission_enabled) {
            auto emission = texture(in.emission_image, standard.emission_texture, standard);
            if (!emission) {
                return emission.error();
            }
            if (const std::optional<int> index = emission.value()) {
                // The texture is the emission, as Godot's importer reads it.
                out["emissiveTexture"]["index"] = *index;
                out["emissiveFactor"] = json::Array{1.0, 1.0, 1.0};
            } else {
                out["emissiveFactor"] = linear_color(standard.emission, false);
            }
            if (standard.emission_energy_multiplier != 1.0F) {
                out["extensions"]["KHR_materials_emissive_strength"]["emissiveStrength"] =
                    standard.emission_energy_multiplier;
                uses("KHR_materials_emissive_strength");
            }
        }
        if (standard.shading_mode == "unshaded") {
            out["extensions"]["KHR_materials_unlit"] = json::Object{};
            uses("KHR_materials_unlit");
        }
        if (standard.cull_mode == "disabled") {
            out["doubleSided"] = true;
        }
        if (standard.transparency == "alpha") {
            out["alphaMode"] = "BLEND";
        } else if (standard.transparency == "alpha_scissor") {
            out["alphaMode"] = "MASK";
            out["alphaCutoff"] = standard.alpha_scissor_threshold;
        }
        return out;
    }

    // A collision shape's triangles as a mesh of positions only, placed by where, in glTF's
    // counter-clockwise order, as Godot's importer reads them back by their node's hint.
    int collision_mesh(const ModelCollision& collision, const world::Affine3D& where) {
        std::vector<float> positions;
        positions.reserve(collision.points.size() * 3);
        std::array<float, 3> low{};
        std::array<float, 3> high{};
        for (std::size_t i = 0; i < collision.points.size(); ++i) {
            const Vec3 p = where.apply(collision.points[i]);
            const std::array<float, 3> v{p.x, p.y, p.z};
            positions.insert(positions.end(), v.begin(), v.end());
            for (std::size_t k = 0; k < 3; ++k) {
                low.at(k) = i == 0 ? v.at(k) : std::min(low.at(k), v.at(k));
                high.at(k) = i == 0 ? v.at(k) : std::max(high.at(k), v.at(k));
            }
        }
        json::Value attributes = json::Object{};
        const int position = accessor(positions, 3, kFloat, "VEC3", kArrayBuffer);
        document["accessors"].as_array()[static_cast<std::size_t>(position)]["min"] =
            json::Array{low[0], low[1], low[2]};
        document["accessors"].as_array()[static_cast<std::size_t>(position)]["max"] =
            json::Array{high[0], high[1], high[2]};
        attributes["POSITION"] = position;
        std::vector<std::uint32_t> indices = collision.indices;
        for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
            std::swap(indices[i + 1], indices[i + 2]);
        }
        json::Value primitive = json::Object{};
        primitive["attributes"] = std::move(attributes);
        primitive["indices"] = accessor(indices, 1, kUnsignedInt, "SCALAR", kElementArrayBuffer);
        json::Value mesh = json::Object{};
        mesh["name"] = collision.node;
        mesh["primitives"] = json::Array{std::move(primitive)};
        return add("meshes", std::move(mesh));
    }

    // A surface as a primitive of triangles, its corners back in glTF's counter-clockwise order.
    json::Value primitive(const ModelSurface& surface) {
        const std::size_t count = surface.vertices.size();
        std::vector<float> positions;
        std::vector<float> normals;
        std::vector<float> tangents;
        std::vector<float> uvs;
        std::vector<std::uint8_t> colors;
        positions.reserve(count * 3);
        std::array<float, 3> low{};
        std::array<float, 3> high{};
        for (std::size_t i = 0; i < count; ++i) {
            const render::Vertex3D& v = surface.vertices[i];
            positions.insert(positions.end(), v.position.begin(), v.position.end());
            normals.insert(normals.end(), v.normal.begin(), v.normal.end());
            tangents.insert(tangents.end(), v.tangent.begin(), v.tangent.end());
            uvs.insert(uvs.end(), v.uv.begin(), v.uv.end());
            colors.insert(colors.end(), v.rgba.begin(), v.rgba.end());
            for (std::size_t k = 0; k < 3; ++k) {
                low.at(k) = i == 0 ? v.position.at(k) : std::min(low.at(k), v.position.at(k));
                high.at(k) = i == 0 ? v.position.at(k) : std::max(high.at(k), v.position.at(k));
            }
        }
        json::Value attributes = json::Object{};
        const int position = accessor(positions, 3, kFloat, "VEC3", kArrayBuffer);
        document["accessors"].as_array()[static_cast<std::size_t>(position)]["min"] =
            json::Array{low[0], low[1], low[2]};
        document["accessors"].as_array()[static_cast<std::size_t>(position)]["max"] =
            json::Array{high[0], high[1], high[2]};
        attributes["POSITION"] = position;
        attributes["NORMAL"] = accessor(normals, 3, kFloat, "VEC3", kArrayBuffer);
        attributes["TANGENT"] = accessor(tangents, 4, kFloat, "VEC4", kArrayBuffer);
        attributes["TEXCOORD_0"] = accessor(uvs, 2, kFloat, "VEC2", kArrayBuffer);
        // Corner colors only where they count, in a material that multiplies them in.
        const bool colored = surface.material >= 0 &&
                             static_cast<std::size_t>(surface.material) < model.materials.size() &&
                             model.materials[static_cast<std::size_t>(surface.material)]
                                 .standard.vertex_color_use_as_albedo;
        if (colored) {
            attributes["COLOR_0"] =
                accessor(colors, 4, kUnsignedByte, "VEC4", kArrayBuffer, /*normalized=*/true);
        }
        std::vector<std::uint32_t> indices = surface.indices;
        for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
            std::swap(indices[i + 1], indices[i + 2]);
        }
        json::Value out = json::Object{};
        out["attributes"] = std::move(attributes);
        if (count <= 0xFFFFU) {
            const std::vector<std::uint16_t> small(indices.begin(), indices.end());
            out["indices"] = accessor(small, 1, kUnsignedShort, "SCALAR", kElementArrayBuffer);
        } else {
            out["indices"] = accessor(indices, 1, kUnsignedInt, "SCALAR", kElementArrayBuffer);
        }
        if (surface.material >= 0 &&
            static_cast<std::size_t>(surface.material) < model.materials.size()) {
            out["material"] = surface.material;
        }
        return out;
    }
};

json::Array numbers(const Vec3& v) {
    return {v.x, v.y, v.z};
}

// The import hint a written collision shape's node takes, by whether its node draws too.
std::string hint_of(const ModelCollision& collision, bool drawn) {
    if (drawn) {
        return collision.convex ? "-convcol" : "-col";
    }
    return collision.convex ? "-convcolonly" : "-colonly";
}

// The glTF document of a model and its buffer's bytes, which a .gltf file holds in a data URI
// and a .glb file after the document; with read, every image goes in the buffer.
Result<std::pair<json::Value, std::string>> document_of(const Model& model, std::string_view path,
                                                        const ModelReader* read) {
    const std::size_t slash = path.rfind('/');
    Writer writer{.model = model,
                  .folder =
                      slash == std::string_view::npos ? "" : std::string(path.substr(0, slash)),
                  .document = json::Object{},
                  .bin = {},
                  .image_paths = {},
                  .model_images = {},
                  .samplers = {},
                  .textures = {},
                  .extensions = {},
                  .read = read};
    writer.document["asset"]["version"] = "2.0";
    writer.document["asset"]["generator"] = "SlashEngine";
    for (const ModelMaterial& in : model.materials) {
        auto material = writer.material(in);
        if (!material) {
            return material.error();
        }
        writer.add("materials", std::move(material).value());
    }
    // Meshes with triangles to draw, by their index in the model.
    std::vector<int> meshes(model.meshes.size(), -1);
    for (std::size_t i = 0; i < model.meshes.size(); ++i) {
        json::Value primitives = json::Array{};
        for (const ModelSurface& surface : model.meshes[i].surfaces) {
            if (!surface.vertices.empty() && surface.indices.size() >= 3) {
                primitives.push_back(writer.primitive(surface));
            }
        }
        if (primitives.size() == 0) {
            continue;
        }
        json::Value mesh = json::Object{};
        if (!model.meshes[i].name.empty()) {
            mesh["name"] = model.meshes[i].name;
        }
        mesh["primitives"] = std::move(primitives);
        meshes[i] = writer.add("meshes", std::move(mesh));
    }
    const auto mesh_of = [&](const ModelPart& part) {
        return part.mesh < meshes.size() ? meshes[part.mesh] : -1;
    };
    json::Array roots;
    // Collision shapes that go on nodes of their own, as roots, in the model's space.
    std::vector<const ModelCollision*> loose;
    if (!model.nodes.empty()) {
        // Each node, then a child for each part past its first, since a glTF node has one mesh.
        std::vector<json::Value> nodes(model.nodes.size(), json::Value(json::Object{}));
        for (std::size_t i = 0; i < model.nodes.size(); ++i) {
            const ModelNode& in = model.nodes[i];
            json::Value& out = nodes[i];
            out["name"] = in.name;
            if (in.position != Vec3{}) {
                out["translation"] = numbers(in.position);
            }
            if (in.rotation != Quat{}) {
                out["rotation"] =
                    json::Array{in.rotation.x, in.rotation.y, in.rotation.z, in.rotation.w};
            }
            if (in.scale != Vec3{1.0F, 1.0F, 1.0F}) {
                out["scale"] = numbers(in.scale);
            }
        }
        std::vector<std::vector<int>> children(model.nodes.size());
        std::vector<json::Value> extra;
        for (const ModelPart& part : model.parts) {
            const int mesh = mesh_of(part);
            if (mesh < 0 || part.node_index < 0 ||
                static_cast<std::size_t>(part.node_index) >= nodes.size()) {
                continue;
            }
            json::Value& node = nodes[static_cast<std::size_t>(part.node_index)];
            if (node.find("mesh") == nullptr) {
                node["mesh"] = mesh;
                continue;
            }
            json::Value more = json::Object{};
            more["name"] = part.node + "_" + std::to_string(extra.size() + 2);
            more["mesh"] = mesh;
            children[static_cast<std::size_t>(part.node_index)].push_back(
                static_cast<int>(model.nodes.size() + extra.size()));
            extra.push_back(std::move(more));
        }
        // Collision from a node's hint goes back on its node as the hint: a node that draws
        // takes "-col" or "-convcol" and makes it again of what it draws, and one that doesn't
        // takes "-colonly" or "-convcolonly" and a mesh of the shape where the node is.
        std::vector<world::Affine3D> globals;
        std::vector<bool> hinted(model.nodes.size(), false);
        for (const ModelCollision& collision : model.collision) {
            if (collision.node_index < 0 ||
                static_cast<std::size_t>(collision.node_index) >= nodes.size() ||
                hinted[static_cast<std::size_t>(collision.node_index)]) {
                loose.push_back(&collision);
                continue;
            }
            const auto at = static_cast<std::size_t>(collision.node_index);
            hinted[at] = true;
            json::Value& node = nodes[at];
            const bool drawn = node.find("mesh") != nullptr;
            node["name"] = model.nodes[at].name + hint_of(collision, drawn);
            if (!drawn) {
                if (globals.empty()) {
                    globals = node_globals(model, rest_pose(model));
                }
                node["mesh"] = writer.collision_mesh(collision, globals[at].inverse());
            }
        }
        for (std::size_t i = 0; i < model.nodes.size(); ++i) {
            const int parent = model.nodes[i].parent;
            if (parent >= 0 && static_cast<std::size_t>(parent) < model.nodes.size()) {
                children[static_cast<std::size_t>(parent)].push_back(static_cast<int>(i));
            } else {
                roots.emplace_back(static_cast<std::int64_t>(i));
            }
        }
        for (std::size_t i = 0; i < model.nodes.size(); ++i) {
            std::ranges::sort(children[i]);
            if (!children[i].empty()) {
                json::Array list;
                for (const int child : children[i]) {
                    list.emplace_back(child);
                }
                nodes[i]["children"] = std::move(list);
            }
            writer.add("nodes", std::move(nodes[i]));
        }
        for (json::Value& more : extra) {
            writer.add("nodes", std::move(more));
        }
    } else {
        // A model without nodes, as an OBJ model: a node for each part, where it puts it, with
        // the hint of the collision its part made.
        std::vector<bool> placed(model.collision.size(), false);
        for (const ModelPart& part : model.parts) {
            const int mesh = mesh_of(part);
            if (mesh < 0) {
                continue;
            }
            json::Value node = json::Object{};
            node["name"] = part.node;
            for (std::size_t i = 0; i < model.collision.size(); ++i) {
                if (!placed[i] && model.collision[i].node == part.node) {
                    placed[i] = true;
                    node["name"] = part.node + hint_of(model.collision[i], true);
                    break;
                }
            }
            node["mesh"] = mesh;
            const NodePose pose = decompose(part.transform);
            if (pose.position != Vec3{}) {
                node["translation"] = numbers(pose.position);
            }
            if (pose.rotation != Quat{}) {
                node["rotation"] =
                    json::Array{pose.rotation.x, pose.rotation.y, pose.rotation.z, pose.rotation.w};
            }
            if (pose.scale != Vec3{1.0F, 1.0F, 1.0F}) {
                node["scale"] = numbers(pose.scale);
            }
            roots.emplace_back(writer.add("nodes", std::move(node)));
        }
        for (std::size_t i = 0; i < model.collision.size(); ++i) {
            if (!placed[i]) {
                loose.push_back(&model.collision[i]);
            }
        }
    }
    for (const ModelCollision* collision : loose) {
        json::Value node = json::Object{};
        node["name"] = collision->node + hint_of(*collision, false);
        node["mesh"] = writer.collision_mesh(*collision, {});
        roots.emplace_back(writer.add("nodes", std::move(node)));
    }
    json::Value scene = json::Object{};
    scene["nodes"] = std::move(roots);
    writer.add("scenes", std::move(scene));
    writer.document["scene"] = 0;
    if (!writer.extensions.empty()) {
        json::Array used;
        for (const std::string& name : writer.extensions) {
            used.emplace_back(name);
        }
        writer.document["extensionsUsed"] = std::move(used);
    }
    if (!writer.bin.empty()) {
        json::Value buffer = json::Object{};
        buffer["byteLength"] = static_cast<std::int64_t>(writer.bin.size());
        if (read == nullptr) {
            buffer["uri"] = "data:application/octet-stream;base64," + base64_encode(writer.bin);
        }
        writer.add("buffers", std::move(buffer));
    }
    return std::pair(std::move(writer.document), std::move(writer.bin));
}

void append_u32(std::string& out, std::uint32_t value) {
    for (std::uint32_t shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<char>((value >> shift) & 0xFFU));
    }
}

} // namespace

Result<std::string> write_gltf(const Model& model, std::string_view path) {
    auto made = document_of(model, path, nullptr);
    if (!made) {
        return made.error();
    }
    return json::write(made.value().first, json::Style::Pretty) + "\n";
}

Result<std::string> write_glb(const Model& model, std::string_view path, const ModelReader& read) {
    auto made = document_of(model, path, &read);
    if (!made) {
        return made.error();
    }
    // The GLB container: a header, then the JSON chunk padded with spaces and the binary chunk
    // padded with zeros, each to four bytes.
    std::string text = json::write(made.value().first, json::Style::Compact);
    while (text.size() % 4 != 0) {
        text.push_back(' ');
    }
    std::string& bin = made.value().second;
    while (bin.size() % 4 != 0) {
        bin.push_back('\0');
    }
    const std::size_t length = 12 + 8 + text.size() + (bin.empty() ? 0 : 8 + bin.size());
    std::string out;
    out.reserve(length);
    out += "glTF";
    append_u32(out, 2);
    append_u32(out, static_cast<std::uint32_t>(length));
    append_u32(out, static_cast<std::uint32_t>(text.size()));
    out += "JSON";
    out += text;
    if (!bin.empty()) {
        append_u32(out, static_cast<std::uint32_t>(bin.size()));
        out += std::string_view("BIN\0", 4);
        out += bin;
    }
    return out;
}

} // namespace slash::scene3d
