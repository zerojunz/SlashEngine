#include "slash/scene3d/model.hpp"

#include "model_internal.hpp"

#include "slash/asset/assets.hpp"
#include "slash/core/text.hpp"
#include "slash/render/image.hpp"
#include "slash/scene3d/blend.hpp"
#include "slash/scene3d/gridmap.hpp"
#include "slash/scene3d/skeleton.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cgltf.h>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace slash::scene3d {
namespace {

// The extensions a model may require: those load_model reads, and Godot's lights, which it
// leaves out as it leaves out cameras.
constexpr std::array<std::string_view, 6> kRequirable{
    "KHR_lights_punctual", "KHR_materials_emissive_strength", "KHR_materials_pbrSpecularGlossiness",
    "KHR_materials_unlit", "KHR_mesh_quantization",           "KHR_texture_transform"};

std::string result_message(cgltf_result result) {
    switch (result) {
    case cgltf_result_data_too_short:
        return "the file is cut short, or an accessor reaches past its buffer";
    case cgltf_result_unknown_format:
        return "not a glTF 2.0 file (.gltf JSON or .glb)";
    case cgltf_result_invalid_json:
        return "its JSON doesn't parse";
    case cgltf_result_invalid_gltf:
        return "it breaks the glTF 2.0 rules";
    case cgltf_result_legacy_gltf:
        return "it is glTF 1.0; only glTF 2.0 loads";
    case cgltf_result_out_of_memory:
        return "it needs more memory than there is";
    default:
        return "it doesn't load";
    }
}

// A URI the model names, as a project path: relative to the model's folder, percent-decoded.
Result<std::string> project_path(std::string_view model, std::string_view uri) {
    const std::size_t slash = model.rfind('/');
    std::string combined(uri);
    combined.push_back('\0');
    cgltf_decode_uri(combined.data());
    combined.resize(std::strlen(combined.c_str()));
    combined =
        (slash == std::string_view::npos ? "" : std::string(model.substr(0, slash + 1))) + combined;
    auto path = normalized(combined);
    if (!path || !is_project_path(*path)) {
        return make_error(ErrorCode::InvalidArgument,
                          "it names " + quoted(uri) + ", which is outside the project folder");
    }
    return *std::move(path);
}

// What cgltf's file callbacks read through.
struct Files {
    const ModelReader* read = nullptr;
    std::string error;
};

cgltf_result read_file(const cgltf_memory_options* /*memory*/, const cgltf_file_options* options,
                       const char* path, cgltf_size* size, void** data) {
    auto* files = static_cast<Files*>(options->user_data);
    auto clean = normalized(path);
    if (!clean || !is_project_path(*clean)) {
        files->error = "a buffer it names, " + quoted(path) + ", is outside the project folder";
        return cgltf_result_file_not_found;
    }
    auto text = (*files->read)(*clean);
    if (!text) {
        files->error =
            "its buffer " + quoted(*clean) + " could not be read: " + text.error().message;
        return cgltf_result_file_not_found;
    }
    // cgltf frees it with release_file. NOLINTNEXTLINE(cppcoreguidelines-no-malloc)
    void* copy = std::malloc(std::max<std::size_t>(text.value().size(), 1));
    if (copy == nullptr) {
        return cgltf_result_out_of_memory;
    }
    std::memcpy(copy, text.value().data(), text.value().size());
    *size = text.value().size();
    *data = copy;
    return cgltf_result_success;
}

void release_file(const cgltf_memory_options* /*memory*/, const cgltf_file_options* /*options*/,
                  void* data) {
    std::free(data); // NOLINT(cppcoreguidelines-no-malloc)
}

struct DataDeleter {
    void operator()(cgltf_data* data) const { cgltf_free(data); }
};
using DataPtr = std::unique_ptr<cgltf_data, DataDeleter>;

Vec4 srgb_color(const float* linear, bool alpha) {
    return {linear_to_srgb(linear[0]), linear_to_srgb(linear[1]), linear_to_srgb(linear[2]),
            alpha ? linear[3] : 1.0F};
}

std::string label(const char* name, std::size_t index, const char* kind) {
    std::string out = std::string(kind) + " " + std::to_string(index);
    if (name != nullptr && *name != '\0') {
        out += " (" + quoted(name) + ")";
    }
    return out;
}

// An image inside the model: a buffer view's bytes or a base64 data URI.
Result<render::Image> embedded_image(const cgltf_options& options, const cgltf_image& image) {
    std::string_view bytes;
    std::unique_ptr<void, decltype(&std::free)> decoded(nullptr, &std::free);
    if (image.buffer_view != nullptr) {
        const std::uint8_t* data = cgltf_buffer_view_data(image.buffer_view);
        if (data == nullptr) {
            return make_error(ErrorCode::InvalidArgument, "its buffer has no data");
        }
        bytes = {reinterpret_cast<const char*>(data), image.buffer_view->size};
    } else {
        const std::string_view uri = image.uri;
        const std::size_t comma = uri.find(',');
        if (comma == std::string_view::npos ||
            uri.substr(0, comma).find(";base64") == std::string_view::npos) {
            return make_error(ErrorCode::InvalidArgument, "a data URI that isn't base64");
        }
        const std::string_view base64 = uri.substr(comma + 1);
        std::size_t size = base64.size() / 4 * 3;
        const std::string_view padding =
            base64.substr(base64.size() - std::min<std::size_t>(base64.size(), 2));
        size -= static_cast<std::size_t>(std::ranges::count(padding, '='));
        void* out = nullptr;
        if (cgltf_load_buffer_base64(&options, size, base64.data(), &out) != cgltf_result_success) {
            return make_error(ErrorCode::InvalidArgument, "its base64 doesn't decode");
        }
        decoded.reset(out);
        bytes = {static_cast<const char*>(out), size};
    }
    return render::decode_image(bytes);
}

// Godot's GLTFTextureSampler::get_filter_mode and get_wrap_mode, on a material's albedo texture.
void apply_sampler(const cgltf_sampler* sampler, world::StandardMaterial3D& standard) {
    if (sampler == nullptr) {
        return; // Godot's default sampler: linear with mipmaps, repeating
    }
    switch (sampler->min_filter) {
    case cgltf_filter_type_nearest:
        standard.texture_filter = "nearest";
        break;
    case cgltf_filter_type_linear:
        standard.texture_filter = "linear";
        break;
    case cgltf_filter_type_nearest_mipmap_nearest:
    case cgltf_filter_type_nearest_mipmap_linear:
        standard.texture_filter = "nearest_with_mipmaps";
        break;
    default:
        standard.texture_filter = "linear_with_mipmaps";
        break;
    }
    standard.texture_repeat =
        sampler->wrap_s == cgltf_wrap_mode_repeat && sampler->wrap_t == cgltf_wrap_mode_repeat;
}

int image_of(const cgltf_data& data, const cgltf_texture_view& view) {
    if (view.texture == nullptr || view.texture->image == nullptr) {
        return -1;
    }
    return static_cast<int>(view.texture->image - data.images);
}

// Godot's GLTFDocument::_parse_materials.
ModelMaterial material_of(const cgltf_data& data, std::size_t index) {
    const cgltf_material& in = data.materials[index];
    ModelMaterial out;
    out.name =
        in.name != nullptr && *in.name != '\0' ? in.name : "material_" + std::to_string(index);
    world::StandardMaterial3D& standard = out.standard;
    if (in.unlit != 0) {
        standard.shading_mode = "unshaded";
    }
    if (in.has_emissive_strength != 0) {
        standard.emission_energy_multiplier = in.emissive_strength.emissive_strength;
    }
    const auto uv_transform = [&](const cgltf_texture_view& view) {
        if (view.has_transform != 0) {
            standard.uv1_offset = {view.transform.offset[0], view.transform.offset[1], 0.0F};
            standard.uv1_scale = {view.transform.scale[0], view.transform.scale[1], 1.0F};
        }
    };
    if (in.has_pbr_metallic_roughness != 0) {
        const cgltf_pbr_metallic_roughness& pbr = in.pbr_metallic_roughness;
        standard.albedo_color = srgb_color(pbr.base_color_factor, true);
        out.albedo_image = image_of(data, pbr.base_color_texture);
        if (out.albedo_image >= 0) {
            apply_sampler(pbr.base_color_texture.texture->sampler, standard);
            uv_transform(pbr.base_color_texture);
        }
        standard.metallic = pbr.metallic_factor;
        standard.roughness = pbr.roughness_factor;
        out.metallic_image = image_of(data, pbr.metallic_roughness_texture);
        out.roughness_image = out.metallic_image;
        if (out.metallic_image >= 0) {
            standard.metallic_texture_channel = "blue";
            standard.roughness_texture_channel = "green";
        }
    } else if (in.has_pbr_specular_glossiness != 0) {
        // Its diffuse color and glossiness, as a non-metal; specular colors are left out.
        const cgltf_pbr_specular_glossiness& sg = in.pbr_specular_glossiness;
        standard.albedo_color = srgb_color(sg.diffuse_factor, true);
        out.albedo_image = image_of(data, sg.diffuse_texture);
        if (out.albedo_image >= 0) {
            apply_sampler(sg.diffuse_texture.texture->sampler, standard);
            uv_transform(sg.diffuse_texture);
        }
        standard.roughness = 1.0F - std::clamp(sg.glossiness_factor, 0.0F, 1.0F);
    }
    out.normal_image = image_of(data, in.normal_texture);
    if (out.normal_image >= 0) {
        standard.normal_enabled = true;
        standard.normal_scale = in.normal_texture.scale;
    }
    if (in.emissive_factor[0] > 0.0F || in.emissive_factor[1] > 0.0F ||
        in.emissive_factor[2] > 0.0F) {
        standard.emission_enabled = true;
        standard.emission = srgb_color(in.emissive_factor, false);
    }
    out.emission_image = image_of(data, in.emissive_texture);
    if (out.emission_image >= 0) {
        // Godot's importer adds the texture to black, leaving emissiveFactor out.
        standard.emission_enabled = true;
        standard.emission = {0.0F, 0.0F, 0.0F, 1.0F};
    }
    if (in.double_sided != 0) {
        standard.cull_mode = "disabled";
    }
    if (in.alpha_mode == cgltf_alpha_mode_blend) {
        standard.transparency = "alpha";
    } else if (in.alpha_mode == cgltf_alpha_mode_mask) {
        standard.transparency = "alpha_scissor";
    }
    standard.alpha_scissor_threshold = in.alpha_cutoff;
    return out;
}

const cgltf_accessor* attribute(const cgltf_primitive& primitive, cgltf_attribute_type type,
                                int index = 0) {
    for (cgltf_size i = 0; i < primitive.attributes_count; ++i) {
        const cgltf_attribute& a = primitive.attributes[i];
        if (a.type == type && a.index == index) {
            return a.data;
        }
    }
    return nullptr;
}

std::vector<float> floats(const cgltf_accessor& accessor) {
    std::vector<float> out(cgltf_num_components(accessor.type) * accessor.count);
    cgltf_accessor_unpack_floats(&accessor, out.data(), out.size());
    return out;
}

// A primitive's triangles as a surface with Godot's clockwise winding; nullopt for points and
// lines, which don't draw.
Result<std::optional<ModelSurface>>
surface_of(const cgltf_data& data, const cgltf_primitive& primitive, const std::string& where) {
    if (primitive.type != cgltf_primitive_type_triangles &&
        primitive.type != cgltf_primitive_type_triangle_strip &&
        primitive.type != cgltf_primitive_type_triangle_fan) {
        return std::optional<ModelSurface>();
    }
    const cgltf_accessor* positions = attribute(primitive, cgltf_attribute_type_position);
    if (positions == nullptr) {
        return make_error(ErrorCode::InvalidArgument, where + " has no POSITION attribute");
    }
    const std::size_t count = positions->count;
    if (count > render::kMaxMeshVertices) {
        return make_error(ErrorCode::InvalidArgument,
                          where + " has " + std::to_string(count) + " vertices; at most " +
                              std::to_string(render::kMaxMeshVertices) + " draw");
    }
    Triangles triangles;
    triangles.vertices.resize(count);
    const std::vector<float> xyz = floats(*positions);
    for (std::size_t i = 0; i < count; ++i) {
        triangles.vertices[i].position = {xyz[i * 3], xyz[(i * 3) + 1], xyz[(i * 3) + 2]};
    }
    const cgltf_accessor* normals = attribute(primitive, cgltf_attribute_type_normal);
    if (normals != nullptr) {
        const std::vector<float> n = floats(*normals);
        for (std::size_t i = 0; i < count; ++i) {
            triangles.vertices[i].normal = {n[i * 3], n[(i * 3) + 1], n[(i * 3) + 2]};
        }
    }
    const cgltf_accessor* tangents = attribute(primitive, cgltf_attribute_type_tangent);
    if (tangents != nullptr) {
        const std::vector<float> t = floats(*tangents);
        for (std::size_t i = 0; i < count; ++i) {
            triangles.vertices[i].tangent = {t[i * 4], t[(i * 4) + 1], t[(i * 4) + 2],
                                             t[(i * 4) + 3] < 0.0F ? -1.0F : 1.0F};
        }
    }
    const cgltf_accessor* uvs = attribute(primitive, cgltf_attribute_type_texcoord);
    if (uvs != nullptr) {
        const std::vector<float> uv = floats(*uvs);
        for (std::size_t i = 0; i < count; ++i) {
            triangles.vertices[i].uv = {uv[i * 2], uv[(i * 2) + 1]};
        }
    }
    const cgltf_accessor* colors = attribute(primitive, cgltf_attribute_type_color);
    if (colors != nullptr) {
        const std::vector<float> c = floats(*colors);
        const std::size_t n = cgltf_num_components(colors->type);
        const auto byte = [](float value) {
            return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
        };
        for (std::size_t i = 0; i < count; ++i) {
            triangles.vertices[i].rgba = {byte(c[i * n]), byte(c[(i * n) + 1]),
                                          byte(c[(i * n) + 2]),
                                          n == 4 ? byte(c[(i * n) + 3]) : std::uint8_t{255}};
        }
    }

    // Joints and weights, four a vertex or eight with a second set, normalized to add up to 1,
    // as Godot's GLTFDocument::_parse_meshes reads them.
    const cgltf_accessor* joints = attribute(primitive, cgltf_attribute_type_joints);
    const cgltf_accessor* weights = attribute(primitive, cgltf_attribute_type_weights);
    if (joints != nullptr && weights != nullptr) {
        const cgltf_accessor* joints_1 = attribute(primitive, cgltf_attribute_type_joints, 1);
        const cgltf_accessor* weights_1 = attribute(primitive, cgltf_attribute_type_weights, 1);
        const bool eight = joints_1 != nullptr && weights_1 != nullptr;
        triangles.weights.resize(count);
        const auto read = [&](const cgltf_accessor& j, const cgltf_accessor& w, std::size_t first) {
            for (std::size_t i = 0; i < count && i < j.count && i < w.count; ++i) {
                std::array<cgltf_uint, 4> ids{};
                std::array<float, 4> amounts{};
                cgltf_accessor_read_uint(&j, i, ids.data(), 4);
                cgltf_accessor_read_float(&w, i, amounts.data(), 4);
                VertexWeights& out = triangles.weights[i];
                for (std::size_t k = 0; k < 4; ++k) {
                    out.joints.at(first + k) =
                        static_cast<std::uint16_t>(std::min<cgltf_uint>(ids.at(k), 0xFFFFU));
                    out.weights.at(first + k) = std::max(amounts.at(k), 0.0F);
                }
            }
        };
        read(*joints, *weights, 0);
        if (eight) {
            read(*joints_1, *weights_1, 4);
        }
        for (VertexWeights& w : triangles.weights) {
            float total = 0.0F;
            for (const float weight : w.weights) {
                total += weight;
            }
            if (total > 0.0F) {
                for (float& weight : w.weights) {
                    weight /= total;
                }
            }
        }
    }

    // The corners in order, then as a list of triangles.
    std::vector<std::uint32_t> order;
    if (primitive.indices != nullptr) {
        order.resize(primitive.indices->count);
        cgltf_accessor_unpack_indices(primitive.indices, order.data(), sizeof(std::uint32_t),
                                      order.size());
        for (const std::uint32_t index : order) {
            if (index >= count) {
                return make_error(ErrorCode::InvalidArgument,
                                  where + " has index " + std::to_string(index) + " past its " +
                                      std::to_string(count) + " vertices");
            }
        }
    } else {
        order.resize(count);
        for (std::size_t i = 0; i < count; ++i) {
            order[i] = static_cast<std::uint32_t>(i);
        }
    }
    if (primitive.type == cgltf_primitive_type_triangles) {
        order.resize(order.size() / 3 * 3);
        triangles.indices = std::move(order);
    } else {
        for (std::size_t i = 2; i < order.size(); ++i) {
            if (primitive.type == cgltf_primitive_type_triangle_fan) {
                triangles.indices.insert(triangles.indices.end(),
                                         {order[0], order[i - 1], order[i]});
            } else if (i % 2 == 0) {
                triangles.indices.insert(triangles.indices.end(),
                                         {order[i - 2], order[i - 1], order[i]});
            } else {
                triangles.indices.insert(triangles.indices.end(),
                                         {order[i - 1], order[i - 2], order[i]});
            }
        }
    }
    if (triangles.indices.size() > render::kMaxMeshIndices) {
        return make_error(ErrorCode::InvalidArgument,
                          where + " has " + std::to_string(triangles.indices.size()) +
                              " indices; at most " + std::to_string(render::kMaxMeshIndices) +
                              " draw");
    }

    if (normals == nullptr) {
        flat_normals(triangles);
    }
    if (tangents == nullptr) {
        if (uvs != nullptr) {
            generate_tangents(triangles);
        } else {
            dummy_tangents(triangles);
        }
    }
    // Godot's importer swaps each triangle's last two corners, for its clockwise front faces.
    for (std::size_t i = 0; i + 2 < triangles.indices.size(); i += 3) {
        std::swap(triangles.indices[i + 1], triangles.indices[i + 2]);
    }
    ModelSurface surface;
    surface.vertices = std::move(triangles.vertices);
    surface.indices = std::move(triangles.indices);
    surface.weights = std::move(triangles.weights);
    surface.material =
        primitive.material == nullptr ? -1 : static_cast<int>(primitive.material - data.materials);
    return std::optional<ModelSurface>(std::move(surface));
}

world::Affine3D affine_of(const std::array<float, 16>& m) {
    world::Affine3D out;
    out.x = {m[0], m[1], m[2]};
    out.y = {m[4], m[5], m[6]};
    out.z = {m[8], m[9], m[10]};
    out.origin = {m[12], m[13], m[14]};
    return out;
}

// The scene's nodes in tree order, parents first, and the meshes they place, as Godot's
// GLTFDocument::_parse_nodes and _assign_node_names make them.
struct SceneReader {
    const cgltf_data& data;
    Model& model;
    std::unordered_map<const cgltf_node*, std::size_t> indices;
    NameSet names;
    std::vector<const cgltf_node*> part_nodes; // the node of each part

    void add(const cgltf_node& node, int parent) {
        const std::size_t index = model.nodes.size();
        if (!indices.emplace(&node, index).second) {
            return; // a node listed twice keeps its first place
        }
        ModelNode out;
        const bool named = node.name != nullptr && *node.name != '\0';
        const char* unnamed = node.mesh != nullptr ? "Mesh" : "Node";
        out.name = unique_name(names, named ? node.name : unnamed);
        out.parent = parent;
        if (node.has_matrix != 0) {
            std::array<float, 16> matrix{};
            std::copy(std::begin(node.matrix), std::end(node.matrix), matrix.begin());
            const NodePose pose = decompose(affine_of(matrix));
            out.position = pose.position;
            out.rotation = pose.rotation;
            out.scale = pose.scale;
        } else {
            if (node.has_translation != 0) {
                out.position = {node.translation[0], node.translation[1], node.translation[2]};
            }
            if (node.has_rotation != 0) {
                const Quat q{node.rotation[0], node.rotation[1], node.rotation[2],
                             node.rotation[3]};
                const float length =
                    std::sqrt((q.x * q.x) + (q.y * q.y) + (q.z * q.z) + (q.w * q.w));
                out.rotation = length > 0.0F
                                   ? Quat{q.x / length, q.y / length, q.z / length, q.w / length}
                                   : Quat{};
            }
            if (node.has_scale != 0) {
                out.scale = {node.scale[0], node.scale[1], node.scale[2]};
            }
        }
        const std::string name = out.name;
        model.nodes.push_back(std::move(out));
        if (node.mesh != nullptr) {
            std::array<float, 16> matrix{};
            cgltf_node_transform_world(&node, matrix.data());
            model.parts.push_back({.node = name,
                                   .mesh = static_cast<std::size_t>(node.mesh - data.meshes),
                                   .transform = affine_of(matrix),
                                   .node_index = static_cast<int>(index)});
            part_nodes.push_back(&node);
        }
        for (cgltf_size i = 0; i < node.children_count; ++i) {
            add(*node.children[i], static_cast<int>(index));
        }
    }
};

// The skins whose joints are all in the scene, as Godot's GLTFDocument::_parse_skins reads
// them; a part whose skin is left out draws where its node puts it.
void read_skins(const cgltf_data& data, const SceneReader& scene, Model& model) {
    std::vector<int> made(data.skins_count, -1);
    for (cgltf_size i = 0; i < data.skins_count; ++i) {
        const cgltf_skin& in = data.skins[i];
        ModelSkin skin;
        skin.name = in.name != nullptr ? in.name : "";
        bool whole = in.joints_count > 0;
        for (cgltf_size j = 0; j < in.joints_count && whole; ++j) {
            const auto found = scene.indices.find(in.joints[j]);
            whole = found != scene.indices.end();
            if (whole) {
                skin.joints.push_back(found->second);
            }
        }
        if (!whole) {
            continue;
        }
        skin.inverse_binds.resize(skin.joints.size());
        if (in.inverse_bind_matrices != nullptr) {
            for (cgltf_size j = 0; j < skin.joints.size() && j < in.inverse_bind_matrices->count;
                 ++j) {
                std::array<float, 16> matrix{};
                cgltf_accessor_read_float(in.inverse_bind_matrices, j, matrix.data(), 16);
                skin.inverse_binds[j] = affine_of(matrix);
            }
        }
        made[i] = static_cast<int>(model.skins.size());
        model.skins.push_back(std::move(skin));
    }
    for (std::size_t p = 0; p < model.parts.size(); ++p) {
        const cgltf_node* node = scene.part_nodes[p];
        if (node->skin != nullptr) {
            model.parts[p].skin = made[static_cast<std::size_t>(node->skin - data.skins)];
        }
    }
}

// Hermite interpolation between two keys of a CUBICSPLINE sampler, as the glTF specification
// gives it: from and to with from's out-tangent and to's in-tangent, over span seconds.
std::array<float, 4> hermite(const std::array<float, 4>& from, const std::array<float, 4>& out,
                             const std::array<float, 4>& in, const std::array<float, 4>& to,
                             float t, float span) {
    const float t2 = t * t;
    const float t3 = t2 * t;
    const float a = (2.0F * t3) - (3.0F * t2) + 1.0F;
    const float b = (t3 - (2.0F * t2) + t) * span;
    const float c = (-2.0F * t3) + (3.0F * t2);
    const float d = (t3 - t2) * span;
    std::array<float, 4> v{};
    for (std::size_t k = 0; k < 4; ++k) {
        v.at(k) = (a * from.at(k)) + (b * out.at(k)) + (c * to.at(k)) + (d * in.at(k));
    }
    return v;
}

std::array<float, 4> normalized_rotation(std::array<float, 4> q) {
    const float length = std::sqrt((q[0] * q[0]) + (q[1] * q[1]) + (q[2] * q[2]) + (q[3] * q[3]));
    if (length == 0.0F) {
        return {0.0F, 0.0F, 0.0F, 1.0F};
    }
    for (float& v : q) {
        v /= length;
    }
    return q;
}

// The model's animations, as Godot's GLTFDocument::_parse_animations and _import_animation make
// them: a track per channel that moves a node of the scene (morph target weights are left out),
// cubic splines baked into linear keys at 30 a second, Godot's default bake rate.
void read_animations(const cgltf_data& data, const SceneReader& scene, Model& model) {
    NameSet taken;
    for (cgltf_size i = 0; i < data.animations_count; ++i) {
        const cgltf_animation& in = data.animations[i];
        ModelAnimation out;
        const bool named = in.name != nullptr && *in.name != '\0';
        out.name = unique_name(taken, named ? in.name : "Animation", true);
        out.loop = named && loops_by_name(in.name);
        struct Channel {
            NodeTrack track;
            bool cubic = false;
            std::vector<std::array<float, 4>> spline; // in-tangent, value, out-tangent per key
        };
        std::vector<Channel> channels;
        float end = 0.0F;
        for (cgltf_size c = 0; c < in.channels_count; ++c) {
            const cgltf_animation_channel& channel = in.channels[c];
            const cgltf_animation_sampler* sampler = channel.sampler;
            if (channel.target_node == nullptr || sampler == nullptr || sampler->input == nullptr ||
                sampler->output == nullptr) {
                continue;
            }
            const auto node = scene.indices.find(channel.target_node);
            if (node == scene.indices.end()) {
                continue;
            }
            Channel made;
            made.track.node = node->second;
            switch (channel.target_path) {
            case cgltf_animation_path_type_translation:
                made.track.property = NodeProperty::Position;
                break;
            case cgltf_animation_path_type_rotation:
                made.track.property = NodeProperty::Rotation;
                break;
            case cgltf_animation_path_type_scale:
                made.track.property = NodeProperty::Scale;
                break;
            default:
                continue;
            }
            made.cubic = sampler->interpolation == cgltf_interpolation_type_cubic_spline;
            made.track.step = sampler->interpolation == cgltf_interpolation_type_step;
            made.track.times = floats(*sampler->input);
            const bool rotation = made.track.property == NodeProperty::Rotation;
            const std::size_t size = rotation ? 4 : 3;
            const std::size_t per_key = made.cubic ? 3 : 1;
            const std::size_t keys =
                std::min<std::size_t>(made.track.times.size(), sampler->output->count / per_key);
            made.track.times.resize(keys);
            std::vector<std::array<float, 4>> values(keys * per_key);
            for (std::size_t k = 0; k < values.size(); ++k) {
                std::array<float, 4> v{0.0F, 0.0F, 0.0F, 0.0F};
                cgltf_accessor_read_float(sampler->output, k, v.data(), size);
                values[k] = v;
            }
            if (made.cubic) {
                made.spline = std::move(values);
            } else {
                if (rotation) {
                    for (std::array<float, 4>& v : values) {
                        v = normalized_rotation(v);
                    }
                }
                made.track.values = std::move(values);
            }
            if (!made.track.times.empty()) {
                end = std::max(end, made.track.times.back());
                channels.push_back(std::move(made));
            }
        }
        for (Channel& channel : channels) {
            if (channel.cubic) {
                const std::vector<float> times = std::move(channel.track.times);
                const std::size_t count = times.size();
                const auto value_at = [&](float t) {
                    const auto after = static_cast<std::size_t>(std::ranges::upper_bound(times, t) -
                                                                times.begin());
                    if (after == 0) {
                        return channel.spline[1];
                    }
                    if (after >= count) {
                        return channel.spline[((count - 1) * 3) + 1];
                    }
                    const std::size_t k = after - 1;
                    const float span = times[k + 1] - times[k];
                    const float weight = span > 0.0F ? (t - times[k]) / span : 0.0F;
                    return hermite(channel.spline[(k * 3) + 1], channel.spline[(k * 3) + 2],
                                   channel.spline[((k + 1) * 3)], channel.spline[((k + 1) * 3) + 1],
                                   weight, span);
                };
                channel.track.times.clear();
                for (int frame = 0;; ++frame) {
                    const float t = std::min(static_cast<float>(frame) / 30.0F, end);
                    std::array<float, 4> v = value_at(t);
                    if (channel.track.property == NodeProperty::Rotation) {
                        v = normalized_rotation(v);
                    }
                    channel.track.times.push_back(t);
                    channel.track.values.push_back(v);
                    if (t >= end) {
                        break;
                    }
                }
            }
            out.tracks.push_back(std::move(channel.track));
        }
        // Godot's shortest animation length.
        out.length = std::max(static_cast<double>(end), 0.001);
        model.animations.push_back(std::move(out));
    }
}

} // namespace

std::string unique_name(NameSet& taken, std::string_view name, bool animation) {
    // Godot's String::validate_node_name, with AnimationLibrary::validate_library_name's
    // characters for animations: those a name can't have become underscores.
    std::string base(name);
    for (char& c : base) {
        if (c == '.' || c == ':' || c == '@' || c == '/' || c == '"' || c == '%' ||
            (animation && (c == ',' || c == '['))) {
            c = '_';
        }
    }
    std::string out = base;
    for (int index = 2; taken.contains(out); ++index) {
        out = base + std::to_string(index);
    }
    taken.insert(out);
    return out;
}

NameSet node_names() {
    return {"Skeleton3D"};
}

bool loops_by_name(std::string_view name) {
    std::string lower(name);
    std::ranges::transform(lower, lower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower.starts_with("loop") || lower.ends_with("loop") || lower.starts_with("cycle") ||
           lower.ends_with("cycle");
}

bool is_model_path(std::string_view path) {
    return path.ends_with(".gltf") || path.ends_with(".glb") || path.ends_with(".obj") ||
           path.ends_with(".fbx") || path.ends_with(kBlendSuffix);
}

Result<Model> load_model(std::string_view bytes, std::string_view path, const ModelReader& read) {
    if (path.ends_with(".obj")) {
        return load_obj(bytes, path, read, nullptr);
    }
    if (path.ends_with(".fbx")) {
        return load_fbx(bytes, path, read, nullptr);
    }
    // A .blend file is read as the glTF Blender made of it (blend.hpp).
    if (path.ends_with(kBlendSuffix) && !is_converted_blend(bytes)) {
        return make_error(ErrorCode::InvalidArgument,
                          "it is a Blender file that hasn't been converted to glTF: it converts "
                          "where a session can run Blender, and exports ship it converted");
    }
    Files files{.read = &read, .error = {}};
    cgltf_options options{};
    options.file.read = read_file;
    options.file.release = release_file;
    options.file.user_data = &files;
    cgltf_data* parsed = nullptr;
    if (const cgltf_result result = cgltf_parse(&options, bytes.data(), bytes.size(), &parsed);
        result != cgltf_result_success) {
        return make_error(ErrorCode::InvalidArgument, result_message(result));
    }
    const DataPtr data(parsed);
    for (cgltf_size i = 0; i < data->extensions_required_count; ++i) {
        const std::string_view name = data->extensions_required[i];
        if (std::ranges::find(kRequirable, name) == kRequirable.end()) {
            return make_error(ErrorCode::InvalidArgument,
                              "it requires the extension " + std::string(name) +
                                  ", which isn't read; export it without that extension");
        }
    }
    const std::string model_path(path);
    if (const cgltf_result result = cgltf_load_buffers(&options, data.get(), model_path.c_str());
        result != cgltf_result_success) {
        return make_error(ErrorCode::InvalidArgument,
                          files.error.empty() ? result_message(result) : files.error);
    }
    if (const cgltf_result result = cgltf_validate(data.get()); result != cgltf_result_success) {
        return make_error(ErrorCode::InvalidArgument, result_message(result));
    }

    Model model;
    for (cgltf_size i = 0; i < data->images_count; ++i) {
        const cgltf_image& image = data->images[i];
        ModelImage out;
        const std::string what = label(image.name, i, "image");
        if (image.buffer_view != nullptr ||
            (image.uri != nullptr && std::string_view(image.uri).starts_with("data:"))) {
            auto decoded = embedded_image(options, image);
            if (!decoded) {
                return make_error(ErrorCode::InvalidArgument,
                                  what + " doesn't decode: " + decoded.error().message);
            }
            out.image = std::move(decoded).value();
        } else if (image.uri != nullptr) {
            auto file = project_path(path, image.uri);
            if (!file) {
                return make_error(ErrorCode::InvalidArgument, what + ": " + file.error().message);
            }
            if (!render::is_image_path(file.value())) {
                return make_error(ErrorCode::InvalidArgument,
                                  what + " is " + quoted(file.value()) +
                                      "; textures are .png, .bmp, .jpg, .jpeg or .ktx2 files");
            }
            out.path = std::move(file).value();
        }
        model.images.push_back(std::move(out));
    }
    for (cgltf_size i = 0; i < data->materials_count; ++i) {
        model.materials.push_back(material_of(*data, i));
    }
    // A primitive with corner colors multiplies them into its albedo, as Godot's importer does.
    std::optional<int> colored_default;
    for (cgltf_size i = 0; i < data->meshes_count; ++i) {
        const cgltf_mesh& mesh = data->meshes[i];
        ModelMesh out;
        out.name = mesh.name != nullptr ? mesh.name : "";
        for (cgltf_size j = 0; j < mesh.primitives_count; ++j) {
            const cgltf_primitive& primitive = mesh.primitives[j];
            auto surface =
                surface_of(*data, primitive,
                           label(mesh.name, i, "mesh") + "'s primitive " + std::to_string(j));
            if (!surface) {
                return surface.error();
            }
            if (!surface.value()) {
                continue;
            }
            ModelSurface& made = *surface.value();
            if (attribute(primitive, cgltf_attribute_type_color) != nullptr) {
                if (made.material >= 0) {
                    model.materials[static_cast<std::size_t>(made.material)]
                        .standard.vertex_color_use_as_albedo = true;
                } else {
                    if (!colored_default) {
                        ModelMaterial colored;
                        colored.name = "vertex_colors";
                        colored.standard.vertex_color_use_as_albedo = true;
                        colored_default = static_cast<int>(model.materials.size());
                        model.materials.push_back(std::move(colored));
                    }
                    made.material = *colored_default;
                }
            }
            out.surfaces.push_back(std::move(made));
        }
        model.meshes.push_back(std::move(out));
    }
    // The scene's nodes, or with no scene every node without a parent, as Godot's importer.
    const cgltf_scene* scene = data->scene;
    if (scene == nullptr && data->scenes_count > 0) {
        scene = &data->scenes[0];
    }
    SceneReader reader{
        .data = *data, .model = model, .indices = {}, .names = node_names(), .part_nodes = {}};
    if (scene != nullptr) {
        for (cgltf_size i = 0; i < scene->nodes_count; ++i) {
            reader.add(*scene->nodes[i], -1);
        }
    } else {
        for (cgltf_size i = 0; i < data->nodes_count; ++i) {
            if (data->nodes[i].parent == nullptr) {
                reader.add(data->nodes[i], -1);
            }
        }
    }
    read_skins(*data, reader, model);
    read_animations(*data, reader, model);
    return model;
}

std::vector<json::ValidationError> check_model(std::string_view bytes, std::string_view path,
                                               const ModelReader& read) {
    const bool obj = path.ends_with(".obj");
    const bool fbx = path.ends_with(".fbx");
    std::vector<json::ValidationError> errors;
    Result<Model> model = make_error(ErrorCode::InvalidArgument, "");
    if (obj) {
        model = load_obj(bytes, path, read, &errors);
    } else if (fbx) {
        model = load_fbx(bytes, path, read, &errors);
    } else {
        model = load_model(bytes, path, read);
    }
    if (!model) {
        return {{"", model.error().message}};
    }
    for (std::size_t i = 0; i < model.value().images.size(); ++i) {
        const std::string& file = model.value().images[i].path;
        if (file.empty()) {
            continue;
        }
        const std::string at = obj || fbx ? "" : "/images/" + std::to_string(i) + "/uri";
        auto text = read(file);
        if (!text) {
            errors.push_back({at, quoted(file) + " could not be read: " + text.error().message});
        } else if (auto image = render::decode_image(text.value()); !image) {
            errors.push_back({at, quoted(file) + " doesn't decode: " + image.error().message});
        }
    }
    return errors;
}

Bounds bounds(const Model& model) {
    std::optional<Bounds> out;
    const std::vector<world::Affine3D> globals = node_globals(model, rest_pose(model));
    std::vector<render::Vertex3D> skinned;
    for (const ModelPart& part : model.parts) {
        std::vector<world::Affine3D> joints;
        if (part.skin >= 0) {
            joints = joint_transforms(model.skins[static_cast<std::size_t>(part.skin)], globals);
        }
        for (const ModelSurface& surface : model.meshes[part.mesh].surfaces) {
            // A skinned surface is where its joints put it at rest.
            const bool skin = part.skin >= 0 && !surface.weights.empty();
            if (skin) {
                skin_vertices(surface, joints, skinned);
            }
            for (const render::Vertex3D& vertex : skin ? skinned : surface.vertices) {
                const Vec3 p =
                    skin ? position_of(vertex) : part.transform.apply(position_of(vertex));
                if (!out) {
                    out = Bounds{p, p};
                }
                out->min = {std::min(out->min.x, p.x), std::min(out->min.y, p.y),
                            std::min(out->min.z, p.z)};
                out->max = {std::max(out->max.x, p.x), std::max(out->max.y, p.y),
                            std::max(out->max.z, p.z)};
            }
        }
    }
    return out.value_or(Bounds{});
}

std::size_t model_bytes(const Model& model) {
    std::size_t bytes = sizeof(Model);
    for (const ModelMesh& mesh : model.meshes) {
        for (const ModelSurface& surface : mesh.surfaces) {
            bytes += (surface.vertices.capacity() * sizeof(render::Vertex3D)) +
                     (surface.indices.capacity() * sizeof(std::uint32_t)) +
                     (surface.weights.capacity() * sizeof(VertexWeights));
        }
    }
    bytes += model.nodes.capacity() * sizeof(ModelNode);
    for (const ModelSkin& skin : model.skins) {
        bytes += sizeof(ModelSkin) + (skin.joints.capacity() * sizeof(std::size_t)) +
                 (skin.inverse_binds.capacity() * sizeof(world::Affine3D));
    }
    for (const ModelAnimation& animation : model.animations) {
        bytes += sizeof(ModelAnimation);
        for (const NodeTrack& track : animation.tracks) {
            bytes += sizeof(NodeTrack) + (track.times.capacity() * sizeof(float)) +
                     (track.values.capacity() * sizeof(std::array<float, 4>));
        }
    }
    for (const ModelImage& image : model.images) {
        bytes += image.image.pixels.capacity();
    }
    return bytes + (model.materials.size() * sizeof(ModelMaterial)) +
           (model.parts.size() * sizeof(ModelPart));
}

void add_asset_types(asset::Assets& assets) {
    assets.add_type<Model>(
        "model", {".gltf", ".glb", ".obj", ".fbx", std::string(kBlendSuffix)},
        [&assets](std::string_view bytes, std::string_view path) {
            return load_model(bytes, path, [&assets](std::string_view file) {
                return assets.read(file, "model_file");
            });
        },
        model_bytes);
    add_mesh_library_type(assets);
}

} // namespace slash::scene3d
