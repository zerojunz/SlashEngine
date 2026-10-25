#include "model_internal.hpp"

#include "slash/core/text.hpp"
#include "slash/render/image.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <ufbx.h>
#include <utility>
#include <vector>

// FBX models, binary and ASCII, read with ufbx as Godot's FBX importer reads them
// (modules/fbx/fbx_document.cpp: _parse, _parse_nodes, _parse_skins, _parse_meshes,
// _parse_images, _parse_materials, _parse_animations and _import_animation): in meters with y up,
// the file's tree as the model's nodes, each node with a mesh a part where the tree puts it, each
// of a mesh's materials a surface, skin deformers as skins and animation stacks as animations.
// Blend shapes, cameras and lights are left out, as they are of glTF models.
namespace slash::scene3d {
namespace {

struct SceneDeleter {
    void operator()(ufbx_scene* scene) const { ufbx_free_scene(scene); }
};
using ScenePtr = std::unique_ptr<ufbx_scene, SceneDeleter>;

struct BakedDeleter {
    void operator()(ufbx_baked_anim* baked) const { ufbx_free_baked_anim(baked); }
};
using BakedPtr = std::unique_ptr<ufbx_baked_anim, BakedDeleter>;

// The rate Godot's importer bakes FBX animations at, its default animation/fps.
constexpr double kBakeRate = 30.0;

// The folders Godot's _get_texture_path looks in for a texture, in each folder from the model's
// up to the project's.
constexpr std::array<std::string_view, 11> kTextureFolders{
    "",           "textures/", "Textures/", "images/", "Images/", "materials/",
    "Materials/", "maps/",     "Maps/",     "tex/",    "Tex/"};

// How many warnings of one kind are reported, as Godot prints them.
constexpr int kWarningsPerKind = 10;

std::string text_of(ufbx_string s) {
    return {s.data, s.length};
}

Vec3 vec3_of(ufbx_vec3 v) {
    return {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)};
}

Vec4 srgb(Vec4 c) {
    return {linear_to_srgb(c.x), linear_to_srgb(c.y), linear_to_srgb(c.z), c.w};
}

// Godot's _material_color: a map's value as a color. A four-part value takes its alpha from its
// third part, as Godot's does.
Vec4 material_color(const ufbx_material_map& map) {
    if (map.value_components == 1) {
        const auto r = static_cast<float>(map.value_real);
        return {r, r, r, 1.0F};
    }
    if (map.value_components == 3) {
        const Vec3 c = vec3_of(map.value_vec3);
        return {c.x, c.y, c.z, 1.0F};
    }
    const ufbx_vec4 c = map.value_vec4;
    return {static_cast<float>(c.x), static_cast<float>(c.y), static_cast<float>(c.z),
            static_cast<float>(c.z)};
}

Vec4 material_color(const ufbx_material_map& map, const ufbx_material_map& factor) {
    Vec4 color = material_color(map);
    if (factor.has_value) {
        const auto f = static_cast<float>(factor.value_real);
        color = {color.x * f, color.y * f, color.z * f, color.w};
    }
    return color;
}

// Godot's _get_file_texture: the first file a texture samples, or null.
const ufbx_texture* file_texture(const ufbx_texture* texture) {
    if (texture == nullptr) {
        return nullptr;
    }
    for (const ufbx_texture* file : texture->file_textures) {
        if (file->file_index != UFBX_NO_INDEX) {
            return file;
        }
    }
    return nullptr;
}

// Godot's Image::detect_alpha, on an image's pixels.
enum class AlphaMode : std::uint8_t { None, Bit, Blend };

AlphaMode detect_alpha(const render::Image& image) {
    bool bit = false;
    for (std::size_t i = 3; i < image.pixels.size(); i += 4) {
        if (image.pixels[i] < 2) {
            bit = true;
        } else if (image.pixels[i] < 254) {
            return AlphaMode::Blend;
        }
    }
    return bit ? AlphaMode::Bit : AlphaMode::None;
}

std::uint8_t color_byte(double value) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0, 1.0) * 255.0));
}

world::Affine3D affine_of(const ufbx_matrix& m) {
    return {.x = vec3_of(m.cols[0]),
            .y = vec3_of(m.cols[1]),
            .z = vec3_of(m.cols[2]),
            .origin = vec3_of(m.cols[3])};
}

// Godot's _parse_meshes: a vertex's first weights in its skin, the largest as ufbx sorts them,
// scaled to add up to one.
VertexWeights weights_of(const ufbx_skin_deformer& skin, std::uint32_t vertex,
                         std::uint32_t places) {
    VertexWeights out;
    if (vertex >= skin.vertices.count) {
        return out;
    }
    const ufbx_skin_vertex& in = skin.vertices.data[vertex];
    const std::uint32_t count = std::min(in.num_weights, places);
    float total = 0.0F;
    for (std::uint32_t i = 0; i < count; ++i) {
        const ufbx_skin_weight& weight = skin.weights.data[in.weight_begin + i];
        out.joints.at(i) = static_cast<std::uint16_t>(weight.cluster_index);
        out.weights.at(i) = static_cast<float>(weight.weight);
        total += out.weights.at(i);
    }
    if (total > 0.0F) {
        for (std::uint32_t i = 0; i < count; ++i) {
            out.weights.at(i) /= total;
        }
    }
    return out;
}

std::array<float, 4> value_of(ufbx_vec3 v) {
    return {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z), 0.0F};
}

std::array<float, 4> value_of(ufbx_quat q) {
    // Normalized, as Godot's importer compares them.
    const double length = std::sqrt((q.x * q.x) + (q.y * q.y) + (q.z * q.z) + (q.w * q.w));
    const double scale = length > 0.0 ? 1.0 / length : 0.0;
    return {static_cast<float>(q.x * scale), static_cast<float>(q.y * scale),
            static_cast<float>(q.z * scale), static_cast<float>(q.w * scale)};
}

// Godot's Math::is_equal_approx, for each part.
bool equal_approx(const std::array<float, 4>& a, const std::array<float, 4>& b) {
    for (std::size_t i = 0; i < 4; ++i) {
        const float tolerance = std::max(0.00001F * std::abs(a.at(i)), 0.00001F);
        if (a.at(i) != b.at(i) && std::abs(a.at(i) - b.at(i)) >= tolerance) {
            return false;
        }
    }
    return true;
}

class FbxReader {
public:
    FbxReader(std::string_view path, const ModelReader& read, Problems problems)
        : path_(path), read_(read), problems_(problems) {
        const std::size_t slash = path_.rfind('/');
        folder_ = slash == std::string::npos ? "" : path_.substr(0, slash);
    }

    Result<Model> run(std::string_view bytes);

private:
    void warnings();
    void images();
    std::string find_texture(const std::string& named);
    ModelMaterial material(const ufbx_material& in, std::size_t index);
    int image(const ufbx_texture* texture) const;
    AlphaMode alpha_of(int image);
    Result<ModelMesh> mesh(const ufbx_mesh& in);
    void add_nodes(const ufbx_node& node, int parent, const ufbx_matrix& to_model);
    void skins();
    void animations();

    std::string path_;
    std::string folder_; // the model's folder, without its trailing '/'
    const ModelReader& read_;
    Problems problems_;
    ScenePtr scene_;
    Model model_;
    std::vector<int> images_; // the model's image for each of the file's texture files, or -1
    std::map<int, AlphaMode> alpha_;
    std::optional<int> colored_default_;
    NameSet names_ = node_names();
    std::vector<int> nodes_;      // the model's node for each of the file's nodes, or -1
    std::vector<int> skins_;      // the model's skin for each of the file's skin deformers, or -1
    std::vector<int> mesh_skins_; // the skin that deforms each of the file's meshes, or -1
};

Result<Model> FbxReader::run(std::string_view bytes) {
    // Godot's _parse, with its importer's default of no helper nodes. Formats are not guessed
    // from the bytes, so a file named .fbx reads as FBX or not at all.
    ufbx_load_opts options{};
    options.target_axes = ufbx_axes_right_handed_y_up;
    options.target_unit_meters = 1.0;
    options.space_conversion = UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY;
    options.geometry_transform_handling =
        UFBX_GEOMETRY_TRANSFORM_HANDLING_MODIFY_GEOMETRY_NO_FALLBACK;
    options.inherit_mode_handling = UFBX_INHERIT_MODE_HANDLING_COMPENSATE_NO_FALLBACK;
    options.pivot_handling = UFBX_PIVOT_HANDLING_ADJUST_TO_PIVOT;
    options.node_depth_limit = 512;
    options.generate_missing_normals = true;
    options.clean_skin_weights = true;
    options.file_format = UFBX_FILE_FORMAT_FBX;
    options.path_separator = '/';
    ufbx_error error{};
    scene_.reset(ufbx_load_memory(bytes.data(), bytes.size(), &options, &error));
    if (scene_ == nullptr) {
        std::string message = "it doesn't read as FBX: " + text_of(error.description);
        if (error.info_length > 0) {
            message += " (" + std::string(error.info, error.info_length) + ")";
        }
        return make_error(ErrorCode::InvalidArgument, message);
    }
    warnings();
    images();
    for (std::size_t i = 0; i < scene_->materials.count; ++i) {
        model_.materials.push_back(material(*scene_->materials.data[i], i));
    }
    // Godot's scene starts at the root's children, whose transforms carry the axis conversion.
    nodes_.assign(scene_->nodes.count, -1);
    for (const ufbx_node* child : scene_->root_node->children) {
        add_nodes(*child, -1, child->node_to_parent);
    }
    skins();
    mesh_skins_.assign(scene_->meshes.count, -1);
    for (const ufbx_mesh* in : scene_->meshes) {
        auto out = mesh(*in);
        if (!out) {
            return out.error();
        }
        model_.meshes.push_back(std::move(out).value());
    }
    for (ModelPart& part : model_.parts) {
        part.skin = mesh_skins_[part.mesh];
    }
    animations();
    return std::move(model_);
}

void FbxReader::warnings() {
    std::map<ufbx_warning_type, int> counts;
    int left_out = 0;
    for (const ufbx_warning& warning : scene_->metadata.warnings) {
        if (counts[warning.type]++ >= kWarningsPerKind) {
            ++left_out;
            continue;
        }
        std::string message = "warning";
        if (warning.element_id != UFBX_NO_INDEX && warning.count <= 1) {
            const ufbx_element* element = scene_->elements.data[warning.element_id];
            if (element->name.length > 0) {
                message += " in " + quoted(text_of(element->name));
            }
        }
        message += ": " + text_of(warning.description);
        if (warning.count > 1) {
            message += " (" + std::to_string(warning.count) + " times)";
        }
        problems_.add(std::move(message));
    }
    if (left_out > 0) {
        problems_.add(std::to_string(left_out) + " more warnings are left out");
    }
}

// Godot's _parse_images: each texture file the model names, decoded if the model carries it,
// else found as _get_texture_path finds it. One that can't be drawn is left out of the
// materials that sample it.
void FbxReader::images() {
    for (const ufbx_texture_file& file : scene_->texture_files) {
        // The path relative to the model, or with none the one the exporter's machine had.
        std::string named = text_of(file.filename);
        if (named.empty()) {
            named = text_of(file.absolute_filename);
        }
        std::ranges::replace(named, '\\', '/');
        // Godot keeps only the name of an absolute path, which another machine won't have.
        if (named.starts_with('/') || named.find(":/") != std::string::npos) {
            named = named.substr(named.rfind('/') + 1);
        }
        if (file.content.size > 0) {
            auto image = render::decode_image(
                std::string_view(static_cast<const char*>(file.content.data), file.content.size));
            if (!image) {
                problems_.add("the image it carries as " + quoted(named) + " doesn't decode: " +
                              image.error().message + "; materials draw without it");
                images_.push_back(-1);
                continue;
            }
            images_.push_back(static_cast<int>(model_.images.size()));
            model_.images.push_back({.path = "", .image = std::move(image).value()});
            continue;
        }
        if (named.empty() || named.ends_with('/')) {
            problems_.add("a texture names no file; materials draw without it");
            images_.push_back(-1);
            continue;
        }
        if (!render::is_image_path(named)) {
            problems_.add("texture " + quoted(named) +
                          " isn't a texture SlashEngine reads (.png, .bmp, .jpg, .jpeg or "
                          ".ktx2); materials draw without it");
            images_.push_back(-1);
            continue;
        }
        std::string found = find_texture(named);
        if (found.empty()) {
            problems_.add("texture " + quoted(named) +
                          " isn't beside the model or in a folder Godot looks in; materials "
                          "draw without it");
            images_.push_back(-1);
            continue;
        }
        images_.push_back(static_cast<int>(model_.images.size()));
        model_.images.push_back({.path = std::move(found), .image = {}});
    }
}

// Godot's _get_texture_path: the file as named, relative to the model, or else one of its name
// in the model's folder or a texture folder in it, then in each folder above it up to the
// project's; "" if there's none.
std::string FbxReader::find_texture(const std::string& named) {
    const auto readable = [&](const std::string& path) {
        return is_project_path(path) && read_(path).ok();
    };
    if (auto direct = normalized(folder_ + "/" + named); direct && readable(*direct)) {
        return *direct;
    }
    const std::string file = named.substr(named.rfind('/') + 1);
    std::string base = folder_;
    while (true) {
        for (const std::string_view sub : kTextureFolders) {
            std::string candidate = base.empty() ? std::string() : base + "/";
            candidate.append(sub).append(file);
            if (readable(candidate)) {
                return candidate;
            }
        }
        if (base.empty()) {
            return {};
        }
        const std::size_t slash = base.rfind('/');
        base = slash == std::string::npos ? "" : base.substr(0, slash);
    }
}

int FbxReader::image(const ufbx_texture* texture) const {
    return texture == nullptr || texture->file_index >= images_.size()
               ? -1
               : images_[texture->file_index];
}

AlphaMode FbxReader::alpha_of(int image) {
    if (const auto known = alpha_.find(image); known != alpha_.end()) {
        return known->second;
    }
    const ModelImage& in = model_.images[static_cast<std::size_t>(image)];
    AlphaMode mode = AlphaMode::None;
    if (in.path.empty()) {
        mode = detect_alpha(in.image);
    } else if (auto bytes = read_(in.path); bytes) {
        if (auto decoded = render::decode_image(bytes.value()); decoded) {
            mode = detect_alpha(decoded.value());
        }
    }
    alpha_.emplace(image, mode);
    return mode;
}

// Godot's _parse_materials. A separate transparency texture, which Godot multiplies into the
// albedo texture's alpha, and ambient occlusion are left out.
ModelMaterial FbxReader::material(const ufbx_material& in, std::size_t index) {
    ModelMaterial out;
    out.name = in.name.length > 0 ? text_of(in.name) : "material_" + std::to_string(index);
    world::StandardMaterial3D& standard = out.standard;
    standard.vertex_color_use_as_albedo = true;
    const ufbx_material_pbr_maps& pbr = in.pbr;
    if (pbr.base_color.has_value) {
        standard.albedo_color = srgb(material_color(pbr.base_color, pbr.base_factor));
    }
    if (in.features.double_sided.enabled) {
        standard.cull_mode = "disabled";
    }
    if (const ufbx_texture* base = file_texture(pbr.base_color.texture); base != nullptr) {
        standard.texture_repeat =
            base->wrap_u == UFBX_WRAP_REPEAT && base->wrap_v == UFBX_WRAP_REPEAT;
        out.albedo_image = image(base);
        if (out.albedo_image >= 0) {
            const AlphaMode mode = alpha_of(out.albedo_image);
            if (mode == AlphaMode::Blend) {
                standard.transparency = "alpha";
            } else if (mode == AlphaMode::Bit) {
                standard.transparency = "alpha_scissor";
            }
        }
        // The texture's own colors, times the base factor, as Godot finds combined colors and
        // textures unreliable in FBX files; the factor scales alpha too.
        const float factor =
            pbr.base_factor.has_value ? static_cast<float>(pbr.base_factor.value_real) : 1.0F;
        standard.albedo_color = srgb({factor, factor, factor, factor});
        standard.uv1_offset = vec3_of(base->uv_transform.translation);
        standard.uv1_scale = vec3_of(base->uv_transform.scale);
    }
    if (in.features.pbr.enabled) {
        standard.metallic =
            pbr.metalness.has_value ? static_cast<float>(pbr.metalness.value_real) : 1.0F;
        standard.roughness =
            pbr.roughness.has_value ? static_cast<float>(pbr.roughness.value_real) : 1.0F;
        if (const ufbx_texture* metallic = file_texture(pbr.metalness.texture); metallic) {
            out.metallic_image = image(metallic);
            standard.metallic_texture_channel = "red";
            standard.metallic = 1.0F;
        }
        if (const ufbx_texture* roughness = file_texture(pbr.roughness.texture); roughness) {
            out.roughness_image = image(roughness);
            standard.roughness_texture_channel = "red";
            standard.roughness = 1.0F;
        }
    }
    if (const ufbx_texture* normal = file_texture(pbr.normal_map.texture); normal != nullptr) {
        out.normal_image = image(normal);
        standard.normal_enabled = true;
        if (pbr.normal_map.has_value) {
            standard.normal_scale = static_cast<float>(pbr.normal_map.value_real);
        }
    }
    if (pbr.emission_color.has_value) {
        standard.emission_enabled = true;
        standard.emission = srgb(material_color(pbr.emission_color));
        standard.emission_energy_multiplier = static_cast<float>(pbr.emission_factor.value_real);
    }
    if (const ufbx_texture* emission = file_texture(pbr.emission_color.texture); emission) {
        out.emission_image = image(emission);
        standard.emission_enabled = true;
        standard.emission = {0.0F, 0.0F, 0.0F, 1.0F};
    }
    return out;
}

// Godot's _parse_meshes: each of a mesh's materials a surface of its triangles; points and lines
// are left out, as they don't draw.
Result<ModelMesh> FbxReader::mesh(const ufbx_mesh& in) {
    ModelMesh out;
    if (in.name.length > 0) {
        out.name = text_of(in.name);
    } else if (in.instances.count > 0) {
        out.name = text_of(in.instances.data[in.instances.count - 1]->name);
    }
    if (out.name.empty()) {
        out.name = "mesh";
    }
    // Godot's importer skins a mesh with the first of its skin deformers it keeps, with four
    // weights a vertex, or eight when a vertex of the skin has more than four.
    const ufbx_skin_deformer* skin = nullptr;
    for (const ufbx_skin_deformer* deformer : in.skin_deformers) {
        if (skins_[deformer->typed_id] >= 0) {
            skin = deformer;
            mesh_skins_[in.typed_id] = skins_[deformer->typed_id];
            break;
        }
    }
    const std::uint32_t places = skin != nullptr && skin->max_weights_per_vertex > 4 ? 8 : 4;
    std::vector<std::uint32_t> corners(in.max_face_triangles * 3);
    for (const ufbx_mesh_part& part : in.material_parts) {
        if (part.num_triangles == 0) {
            continue;
        }
        if (part.num_triangles * 3 > render::kMaxMeshIndices) {
            return make_error(ErrorCode::InvalidArgument,
                              "a surface of " + quoted(out.name) + " has " +
                                  std::to_string(part.num_triangles * 3) + " indices; at most " +
                                  std::to_string(render::kMaxMeshIndices) + " draw");
        }
        Triangles triangles;
        triangles.vertices.reserve(part.num_triangles * 3);
        triangles.indices.reserve(part.num_triangles * 3);
        for (const std::uint32_t face_index : part.face_indices) {
            const ufbx_face face = in.faces.data[face_index];
            if (face.num_indices < 3) {
                continue;
            }
            const std::uint32_t count =
                ufbx_triangulate_face(corners.data(), corners.size(), &in, face);
            for (std::size_t i = 0; i < std::size_t{count} * 3; ++i) {
                const std::uint32_t corner = corners[i];
                render::Vertex3D v;
                const Vec3 p = vec3_of(in.vertex_position[corner]);
                const Vec3 n = vec3_of(in.vertex_normal[corner]);
                v.position = {p.x, p.y, p.z};
                v.normal = {n.x, n.y, n.z};
                if (in.vertex_tangent.exists) {
                    const Vec3 t = vec3_of(in.vertex_tangent[corner]);
                    float sign = 1.0F;
                    // Godot's bitangent sign: whether the file's bitangent faces the normal
                    // crossed with the tangent.
                    if (in.vertex_bitangent.exists) {
                        const Vec3 b = vec3_of(in.vertex_bitangent[corner]);
                        const Vec3 made{(n.y * t.z) - (n.z * t.y), (n.z * t.x) - (n.x * t.z),
                                        (n.x * t.y) - (n.y * t.x)};
                        if ((made.x * b.x) + (made.y * b.y) + (made.z * b.z) < 0.0F) {
                            sign = -1.0F;
                        }
                    }
                    v.tangent = {t.x, t.y, t.z, sign};
                }
                if (in.vertex_uv.exists) {
                    const ufbx_vec2 uv = in.vertex_uv[corner];
                    const auto u = static_cast<float>(uv.x);
                    const auto flipped = static_cast<float>(1.0 - static_cast<float>(uv.y));
                    v.uv = {u, flipped};
                }
                if (in.vertex_color.exists) {
                    const ufbx_vec4 c = in.vertex_color[corner];
                    v.rgba = {color_byte(c.x), color_byte(c.y), color_byte(c.z), color_byte(c.w)};
                }
                triangles.indices.push_back(static_cast<std::uint32_t>(triangles.vertices.size()));
                triangles.vertices.push_back(v);
                if (skin != nullptr) {
                    triangles.weights.push_back(
                        weights_of(*skin, in.vertex_indices[corner], places));
                }
            }
        }
        // Equal corners merged, as Godot's SurfaceTool::index, with MikkTSpace's tangents where
        // the file has none, or with no texture coordinates either Godot's stand-in ones.
        if (in.vertex_tangent.exists) {
            reindex(triangles);
        } else if (in.vertex_uv.exists) {
            generate_tangents(triangles);
        } else {
            dummy_tangents(triangles);
            reindex(triangles);
        }
        if (triangles.vertices.size() > render::kMaxMeshVertices) {
            return make_error(ErrorCode::InvalidArgument,
                              "a surface of " + quoted(out.name) + " has " +
                                  std::to_string(triangles.vertices.size()) +
                                  " vertices; at most " + std::to_string(render::kMaxMeshVertices) +
                                  " draw");
        }
        // Godot's importer reverses each triangle, for its clockwise front faces.
        for (std::size_t i = 0; i + 2 < triangles.indices.size(); i += 3) {
            std::swap(triangles.indices[i + 1], triangles.indices[i + 2]);
        }
        ModelSurface surface;
        surface.vertices = std::move(triangles.vertices);
        surface.indices = std::move(triangles.indices);
        surface.weights = std::move(triangles.weights);
        if (part.index < in.materials.count && in.materials.data[part.index] != nullptr) {
            surface.material = static_cast<int>(in.materials.data[part.index]->typed_id);
        } else if (in.vertex_color.exists) {
            // Godot gives a surface with corner colors and no material one that shows them.
            if (!colored_default_) {
                ModelMaterial colored;
                colored.name = "vertex_colors";
                colored.standard.vertex_color_use_as_albedo = true;
                colored_default_ = static_cast<int>(model_.materials.size());
                model_.materials.push_back(std::move(colored));
            }
            surface.material = *colored_default_;
        }
        out.surfaces.push_back(std::move(surface));
    }
    return out;
}

// A node and those under it, in tree order, as Godot's _parse_nodes and _assign_node_names make
// them, and the parts among them, each where the nodes above it put it.
void FbxReader::add_nodes(const ufbx_node& node, int parent, const ufbx_matrix& to_model) {
    const auto index = static_cast<int>(model_.nodes.size());
    std::string name = text_of(node.name);
    if (name.empty() && node.mesh != nullptr) {
        name = "Mesh";
    } else if (name.empty()) {
        name = node.camera != nullptr ? "Camera3D" : "Node";
    }
    const ufbx_transform& local = node.local_transform;
    ModelNode out;
    out.name = unique_name(names_, name);
    out.parent = parent;
    out.file_index = static_cast<int>(node.typed_id);
    out.position = vec3_of(local.translation);
    out.rotation = {static_cast<float>(local.rotation.x), static_cast<float>(local.rotation.y),
                    static_cast<float>(local.rotation.z), static_cast<float>(local.rotation.w)};
    out.scale = vec3_of(local.scale);
    nodes_[node.typed_id] = index;
    if (node.mesh != nullptr) {
        model_.parts.push_back({.node = out.name,
                                .mesh = node.mesh->typed_id,
                                .transform = affine_of(to_model),
                                .node_index = index});
    }
    model_.nodes.push_back(std::move(out));
    for (const ufbx_node* child : node.children) {
        add_nodes(*child, index, ufbx_matrix_mul(&to_model, &child->node_to_parent));
    }
}

// Godot's _parse_skins: each skin deformer with bones and weights a skin, its clusters' bones its
// joints and their geometry-to-bone matrices its inverse binds.
void FbxReader::skins() {
    skins_.assign(scene_->skin_deformers.count, -1);
    for (const ufbx_skin_deformer* in : scene_->skin_deformers) {
        if (in->clusters.count == 0 || in->weights.count == 0) {
            continue;
        }
        ModelSkin skin;
        skin.name =
            in->name.length > 0 ? text_of(in->name) : "skin_" + std::to_string(in->typed_id);
        bool whole = true;
        for (const ufbx_skin_cluster* cluster : in->clusters) {
            const int joint =
                cluster->bone_node == nullptr ? -1 : nodes_[cluster->bone_node->typed_id];
            if (joint < 0) {
                whole = false;
                break;
            }
            skin.joints.push_back(static_cast<std::size_t>(joint));
            skin.inverse_binds.push_back(affine_of(cluster->geometry_to_bone));
        }
        if (!whole) {
            problems_.add("skin " + quoted(skin.name) +
                          " has a bone that isn't in the scene; the meshes it deforms draw where "
                          "their nodes put them");
            continue;
        }
        skins_[in->typed_id] = static_cast<int>(model_.skins.size());
        model_.skins.push_back(std::move(skin));
    }
}

// Godot's _parse_animations and _import_animation: each animation stack baked into linear keys
// at 30 a second, a track for each part of a node it moves, as long as the stack. Tracks that
// only hold the node where it is, and tracks on a skinned mesh's own node, which doesn't move it,
// are left out.
void FbxReader::animations() {
    std::vector<bool> joints(model_.nodes.size(), false);
    for (const ModelSkin& skin : model_.skins) {
        for (const std::size_t joint : skin.joints) {
            joints[joint] = true;
        }
    }
    std::vector<bool> skinned(model_.nodes.size(), false);
    for (const ModelPart& part : model_.parts) {
        if (part.skin >= 0 && !joints[static_cast<std::size_t>(part.node_index)]) {
            skinned[static_cast<std::size_t>(part.node_index)] = true;
        }
    }
    NameSet taken;
    for (const ufbx_anim_stack* stack : scene_->anim_stacks) {
        ModelAnimation out;
        const std::string named = text_of(stack->name);
        out.name = unique_name(taken, named.empty() ? "Animation" : named, true);
        out.loop = loops_by_name(named);
        ufbx_bake_opts options{};
        options.resample_rate = kBakeRate;
        options.minimum_sample_rate = kBakeRate;
        options.max_keyframe_segments = 1024;
        ufbx_error error{};
        const BakedPtr baked(ufbx_bake_anim(scene_.get(), stack->anim, &options, &error));
        if (baked == nullptr) {
            problems_.add("animation " + quoted(out.name) +
                          " doesn't bake: " + text_of(error.description) + "; it is left out");
            continue;
        }
        for (const ufbx_baked_node& in : baked->nodes) {
            const int index = in.typed_id < nodes_.size() ? nodes_[in.typed_id] : -1;
            if (index < 0 || skinned[static_cast<std::size_t>(index)]) {
                continue;
            }
            const ModelNode& node = model_.nodes[static_cast<std::size_t>(index)];
            const auto add = [&](NodeProperty property, auto keys, std::array<float, 4> rest) {
                NodeTrack track{.node = static_cast<std::size_t>(index),
                                .property = property,
                                .step = false,
                                .times = {},
                                .values = {}};
                bool moves = false;
                for (const auto& key : keys) {
                    const std::array<float, 4> value = value_of(key.value);
                    moves = moves || !equal_approx(value, rest);
                    track.times.push_back(static_cast<float>(key.time));
                    track.values.push_back(value);
                }
                if (moves) {
                    out.tracks.push_back(std::move(track));
                }
            };
            add(NodeProperty::Position, in.translation_keys,
                {node.position.x, node.position.y, node.position.z, 0.0F});
            add(NodeProperty::Rotation, in.rotation_keys,
                {node.rotation.x, node.rotation.y, node.rotation.z, node.rotation.w});
            add(NodeProperty::Scale, in.scale_keys,
                {node.scale.x, node.scale.y, node.scale.z, 0.0F});
        }
        // Godot's shortest animation length.
        out.length = std::max(stack->time_end, 0.001);
        model_.animations.push_back(std::move(out));
    }
}

} // namespace

Result<Model> load_fbx(std::string_view bytes, std::string_view path, const ModelReader& read,
                       std::vector<json::ValidationError>* problems) {
    return FbxReader(path, read, Problems{problems}).run(bytes);
}

} // namespace slash::scene3d
