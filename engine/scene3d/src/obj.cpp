#include "model_internal.hpp"

#include "slash/core/text.hpp"
#include "slash/render/image.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// Wavefront OBJ models and their MTL material libraries, read as Godot's OBJ importer reads one
// as a scene (editor/import/3d/resource_importer_obj.cpp: _parse_obj and
// _parse_material_library), with the tangents its scenes are imported with. Each "o" object is a
// mesh of its own, and each run of faces between "usemtl" lines a surface.
namespace slash::scene3d {
namespace {

// Godot's smooth group for the corners of faces after "s off".
constexpr std::uint32_t kFlat = std::numeric_limits<std::uint32_t>::max();
// Larger than any index a model can use, so integer() can't overflow.
constexpr std::int64_t kHuge = std::int64_t{1} << 40;

// Godot's String::strip_edges: the text without the characters up to ' ' at either end.
std::string_view trimmed(std::string_view text) {
    while (!text.empty() && static_cast<unsigned char>(text.front()) <= ' ') {
        text.remove_prefix(1);
    }
    while (!text.empty() && static_cast<unsigned char>(text.back()) <= ' ') {
        text.remove_suffix(1);
    }
    return text;
}

// A line's words, split at spaces and tabs.
std::vector<std::string_view> words(std::string_view line) {
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && static_cast<unsigned char>(line[i]) <= ' ') {
            ++i;
        }
        const std::size_t start = i;
        while (i < line.size() && static_cast<unsigned char>(line[i]) > ' ') {
            ++i;
        }
        if (i > start) {
            out.push_back(line.substr(start, i - start));
        }
    }
    return out;
}

// A face corner's parts, split at '/', empty ones kept: "1//3" is "1", "" and "3".
std::vector<std::string_view> parts(std::string_view word) {
    std::vector<std::string_view> out;
    std::size_t start = 0;
    while (true) {
        const std::size_t slash = word.find('/', start);
        out.push_back(word.substr(start, slash - start));
        if (slash == std::string_view::npos) {
            return out;
        }
        start = slash + 1;
    }
}

// What follows a line's keyword, as Godot's importer takes names and paths.
std::string rest(std::string_view line, std::string_view keyword) {
    return std::string(trimmed(line.substr(keyword.size())));
}

// Godot's String::to_float: the number the word starts with, or 0 if it starts with none.
float number(std::string_view word) {
    std::size_t i = 0;
    const auto digits = [&] {
        const std::size_t start = i;
        while (i < word.size() && word[i] >= '0' && word[i] <= '9') {
            ++i;
        }
        return i - start;
    };
    if (i < word.size() && (word[i] == '+' || word[i] == '-')) {
        ++i;
    }
    std::size_t count = digits();
    if (i < word.size() && word[i] == '.') {
        ++i;
        count += digits();
    }
    if (count == 0) {
        return 0.0F;
    }
    std::size_t end = i;
    if (i < word.size() && (word[i] == 'e' || word[i] == 'E')) {
        ++i;
        if (i < word.size() && (word[i] == '+' || word[i] == '-')) {
            ++i;
        }
        if (digits() > 0) {
            end = i;
        }
    }
    return static_cast<float>(parse_decimal(word.substr(0, end)).value_or(0.0));
}

// Godot's String::to_int: the word's digits up to any '.', other characters skipped, negative
// after a '-' that comes before them.
std::int64_t integer(std::string_view word) {
    std::int64_t value = 0;
    std::int64_t sign = 1;
    for (const char c : word.substr(0, word.find('.'))) {
        if (c >= '0' && c <= '9') {
            value = std::min((value * 10) + (c - '0'), kHuge);
        } else if (c == '-' && value == 0) {
            sign = -sign;
        }
    }
    return sign * value;
}

Vec3 normalized_or_zero(Vec3 v) {
    const float length = std::sqrt((v.x * v.x) + (v.y * v.y) + (v.z * v.z));
    return length > 0.0F ? v * (1.0F / length) : Vec3{};
}

// Godot's SurfaceTool::generate_normals over corners three to a triangle, counter-clockwise: a
// corner in a smooth group takes the normalized sum of the face normals of the group's
// triangles around its position, and a corner after "s off" its own face's.
void smooth_normals(std::vector<render::Vertex3D>& corners,
                    const std::vector<std::uint32_t>& groups) {
    const auto key = [&](std::size_t i) {
        std::array<float, 3> at = corners[i].position;
        for (float& c : at) {
            if (c == 0.0F) {
                c = 0.0F; // -0 is the same position as 0
            }
        }
        std::string out(sizeof(at) + sizeof(std::uint32_t), '\0');
        std::memcpy(out.data(), at.data(), sizeof(at));
        std::memcpy(out.data() + sizeof(at), &groups[i], sizeof(std::uint32_t));
        return out;
    };
    std::unordered_map<std::string, Vec3> sums;
    std::vector<Vec3> faces(corners.size() / 3);
    for (std::size_t f = 0; f < faces.size(); ++f) {
        const render::Vertex3D* corner = &corners[f * 3];
        const Vec3 a = position_of(corner[1]) - position_of(corner[0]);
        const Vec3 b = position_of(corner[2]) - position_of(corner[0]);
        faces[f] = normalized_or_zero(
            {(a.y * b.z) - (a.z * b.y), (a.z * b.x) - (a.x * b.z), (a.x * b.y) - (a.y * b.x)});
        for (std::size_t k = 0; k < 3; ++k) {
            if (groups[(f * 3) + k] != kFlat) {
                Vec3& sum = sums[key((f * 3) + k)];
                sum = sum + faces[f];
            }
        }
    }
    for (std::size_t i = 0; i < faces.size() * 3; ++i) {
        Vec3 n = normalized_or_zero(groups[i] == kFlat ? faces[i / 3] : sums[key(i)]);
        if (n == Vec3{}) {
            n = {0.0F, 1.0F, 0.0F};
        }
        corners[i].normal = {n.x, n.y, n.z};
    }
}

// A material in a library, as Godot's _parse_material_library makes it, with the project paths
// of its textures, "" for none.
struct LibraryMaterial {
    world::StandardMaterial3D standard;
    std::string albedo;
    std::string metallic;
    std::string roughness;
    std::string normal;
};
using Library = std::unordered_map<std::string, LibraryMaterial>;

// A texture a library names, as a project path relative to the library's folder; "" if it can't
// draw, saying why in problems. Godot's importer reads backslashes as slashes.
std::string texture_path(const std::string& where, std::string_view library, std::string named,
                         const Problems& problems) {
    std::ranges::replace(named, '\\', '/');
    if (named.empty()) {
        problems.add(where + " names no file");
        return {};
    }
    const std::size_t slash = library.rfind('/');
    const std::string folder =
        slash == std::string_view::npos ? "" : std::string(library.substr(0, slash + 1));
    std::optional<std::string> path;
    if (named.front() != '/' && named.find(':') == std::string::npos) {
        path = normalized(folder + named);
    }
    if (!path || !is_project_path(*path)) {
        problems.add(where + " names " + quoted(named) +
                     ", which is outside the project folder; put it beside the model");
        return {};
    }
    if (!render::is_image_path(*path)) {
        problems.add(where + " names " + quoted(*path) +
                     ", which isn't a texture: textures are .png, .bmp, .jpg, .jpeg or .ktx2 "
                     "files");
        return {};
    }
    return *std::move(path);
}

// Godot's _parse_material_library; nullopt where Godot's fails, which leaves the library's
// materials out, saying why in problems. Ambient colors and textures are left out, as Godot's
// renderer has no use for them, and so are other keywords.
std::optional<Library> parse_library(std::string_view text, const std::string& path,
                                     const Problems& problems) {
    Library library;
    LibraryMaterial* current = nullptr;
    std::size_t line_number = 0;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t end = std::min(text.find('\n', start), text.size());
        const std::string_view l = trimmed(text.substr(start, end - start));
        start = end + 1;
        ++line_number;
        const std::string where =
            "material library " + quoted(path) + " line " + std::to_string(line_number);
        const auto fail = [&](const std::string& why) {
            std::string message = where;
            message.append(": ").append(why).append("; its materials are left out");
            problems.add(std::move(message));
            return std::nullopt;
        };
        const auto color = [&]() -> std::optional<std::array<float, 3>> {
            const std::vector<std::string_view> v = words(l);
            if (v.size() < 4) {
                return std::nullopt;
            }
            return std::array<float, 3>{number(v[1]), number(v[2]), number(v[3])};
        };
        const auto one = [&]() -> std::optional<float> {
            const std::vector<std::string_view> v = words(l);
            if (v.size() != 2) {
                return std::nullopt;
            }
            return number(v[1]);
        };
        const auto texture = [&](std::string_view keyword) {
            return texture_path(where + ": " + std::string(keyword), path, rest(l, keyword),
                                problems);
        };
        if (l.starts_with("newmtl ")) {
            current = &library[rest(l, "newmtl")];
            *current = LibraryMaterial{};
        } else if (l.starts_with("Ka ") || l.starts_with("map_Ka ")) {
            continue;
        } else if (l.starts_with("Kd ")) {
            if (current == nullptr) {
                return fail("Kd comes before any newmtl");
            }
            const auto rgb = color();
            if (!rgb) {
                return fail("Kd needs red, green and blue");
            }
            current->standard.albedo_color.x = (*rgb)[0];
            current->standard.albedo_color.y = (*rgb)[1];
            current->standard.albedo_color.z = (*rgb)[2];
        } else if (l.starts_with("Ks ")) {
            if (current == nullptr) {
                return fail("Ks comes before any newmtl");
            }
            const auto rgb = color();
            if (!rgb) {
                return fail("Ks needs red, green and blue");
            }
            // As Godot's importer: the specular color's brightest channel is the metallic.
            current->standard.metallic = std::max({(*rgb)[0], (*rgb)[1], (*rgb)[2]});
        } else if (l.starts_with("Ns ")) {
            if (current == nullptr) {
                return fail("Ns comes before any newmtl");
            }
            const auto shininess = one();
            if (!shininess) {
                return fail("Ns needs one number");
            }
            // As Godot's importer, which sets the metallic, not the roughness, from it.
            current->standard.metallic = (1000.0F - *shininess) / 1000.0F;
        } else if (l.starts_with("d ") || l.starts_with("Tr ")) {
            const bool dissolve = l.starts_with("d ");
            if (current == nullptr) {
                return fail(std::string(dissolve ? "d" : "Tr") + " comes before any newmtl");
            }
            const auto value = one();
            if (!value) {
                return fail(std::string(dissolve ? "d" : "Tr") + " needs one number");
            }
            current->standard.albedo_color.w = dissolve ? *value : 1.0F - *value;
            if (current->standard.albedo_color.w < 0.99F) {
                current->standard.transparency = "alpha";
            }
        } else if (l.starts_with("map_Kd ") || l.starts_with("map_Ks ") ||
                   l.starts_with("map_Ns ") || l.starts_with("map_bump ")) {
            const std::string_view keyword = l.substr(0, l.find(' '));
            if (current == nullptr) {
                return fail(std::string(keyword) + " comes before any newmtl");
            }
            std::string file = texture(keyword);
            if (keyword == "map_Kd") {
                current->albedo = std::move(file);
            } else if (keyword == "map_Ks") {
                current->metallic = std::move(file);
            } else if (keyword == "map_Ns") {
                current->roughness = std::move(file);
            } else {
                current->standard.normal_enabled =
                    current->standard.normal_enabled || !file.empty();
                current->normal = std::move(file);
            }
        }
    }
    return library;
}

// Reads one OBJ file, as Godot's _parse_obj with p_single_mesh off.
class ObjReader {
public:
    ObjReader(std::string_view path, const ModelReader& read, Problems problems)
        : path_(path), read_(read), problems_(problems) {}

    Result<Model> run(std::string_view bytes);

private:
    Result<void> line(std::string_view l, std::size_t at);
    Result<void> face(const std::vector<std::string_view>& v, std::size_t at);
    Result<render::Vertex3D> corner(std::string_view word, std::size_t at);
    Result<void> commit_surface();
    void commit_mesh();
    void use_library(std::string name, std::size_t at);
    int material(const std::string& library, const std::string& name);
    int image(const std::string& file);

    std::string path_;
    const ModelReader& read_;
    Problems problems_;
    Model model_;

    std::vector<Vec3> vertices_;
    std::vector<Vec3> normals_;
    std::vector<std::array<float, 2>> uvs_;
    std::vector<std::array<std::uint8_t, 4>> colors_;
    std::map<std::string, std::optional<Library>> libraries_; // by the name mtllib gives
    std::map<std::string, int> materials_;                    // by library and name
    std::map<std::string, int> images_;                       // by project path

    ModelMesh mesh_;
    std::string name_ = "Mesh";
    std::string library_;
    std::string material_;
    std::uint32_t smooth_group_ = 0;
    bool smoothing_ = true;

    // The surface being read: its corners, three to a triangle, counter-clockwise as the file
    // has them, with their smooth groups.
    std::vector<render::Vertex3D> corners_;
    std::vector<std::uint32_t> groups_;
    bool surface_normals_ = false;
    bool uses_uvs_ = false;
    // What Godot's SurfaceTool gives a corner that doesn't set it: the last corner's.
    render::Vertex3D last_;
};

Result<Model> ObjReader::run(std::string_view bytes) {
    // Godot's importer refuses Visual Studio's COFF objects, which share the extension.
    const auto byte = [&](std::size_t i) {
        return i < bytes.size() ? static_cast<unsigned>(static_cast<unsigned char>(bytes[i])) : 0U;
    };
    const unsigned machine = byte(0) | (byte(1) << 8U);
    if (machine == 0x0U || machine == 0x8664U || machine == 0x1c0U || machine == 0x14cU ||
        machine == 0x200U) {
        return make_error(ErrorCode::InvalidArgument,
                          "it seems to be binary, corrupted or empty, not OBJ text");
    }
    std::size_t start = 0;
    std::size_t lines = 0;
    while (start < bytes.size()) {
        const auto next = [&] {
            const std::size_t end = std::min(bytes.find('\n', start), bytes.size());
            const std::string_view text = trimmed(bytes.substr(start, end - start));
            start = end + 1;
            ++lines;
            return text;
        };
        const std::size_t first = lines + 1;
        std::string l(next());
        // A line ending in a backslash goes on in the next one.
        while (!l.empty() && l.back() == '\\' && start < bytes.size()) {
            l.back() = ' ';
            const std::string_view more = next();
            l += more;
            if (more.empty()) {
                break;
            }
        }
        if (auto done = line(l, first); !done) {
            return done.error();
        }
    }
    if (auto done = commit_surface(); !done) {
        return done.error();
    }
    commit_mesh();
    return std::move(model_);
}

Result<void> ObjReader::line(std::string_view l, std::size_t at) {
    const auto fail = [&](const std::string& why) {
        return make_error(ErrorCode::InvalidArgument, "line " + std::to_string(at) + ": " + why);
    };
    if (l.starts_with("v ")) {
        const std::vector<std::string_view> v = words(l);
        if (v.size() < 4) {
            return fail("v needs x, y and z");
        }
        vertices_.push_back({number(v[1]), number(v[2]), number(v[3])});
        // A vertex color after the position, as some tools write them.
        const auto byte = [](float value) {
            return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
        };
        if (v.size() >= 7) {
            colors_.resize(vertices_.size() - 1, {255, 255, 255, 255});
            colors_.push_back({byte(number(v[4])), byte(number(v[5])), byte(number(v[6])), 255});
        } else if (!colors_.empty()) {
            colors_.push_back({255, 255, 255, 255});
        }
    } else if (l.starts_with("vt ")) {
        const std::vector<std::string_view> v = words(l);
        if (v.size() < 3) {
            return fail("vt needs u and v");
        }
        // Godot's v runs down the texture, OBJ's up it.
        uvs_.push_back({number(v[1]), 1.0F - number(v[2])});
    } else if (l.starts_with("vn ")) {
        const std::vector<std::string_view> v = words(l);
        if (v.size() < 4) {
            return fail("vn needs x, y and z");
        }
        normals_.push_back({number(v[1]), number(v[2]), number(v[3])});
    } else if (l.starts_with("f ")) {
        return face(words(l), at);
    } else if (l.starts_with("s ")) {
        const bool smooth = rest(l, "s") != "off";
        if (smooth != smoothing_) {
            smoothing_ = smooth;
            if (smoothing_) {
                ++smooth_group_;
            }
        }
    } else if (l.starts_with("usemtl ") || l.starts_with("o ")) {
        // Godot's importer leaves "g" groups alone: they neither split meshes nor name surfaces.
        if (auto done = commit_surface(); !done) {
            return done;
        }
        if (l.starts_with("o ")) {
            commit_mesh();
            name_ = rest(l, "o");
        } else {
            material_ = rest(l, "usemtl");
        }
    } else if (l.starts_with("mtllib ")) {
        use_library(rest(l, "mtllib"), at);
    }
    return {};
}

Result<void> ObjReader::face(const std::vector<std::string_view>& v, std::size_t at) {
    const auto fail = [&](const std::string& why) {
        return make_error(ErrorCode::InvalidArgument, "line " + std::to_string(at) + ": " + why);
    };
    if (v.size() < 4) {
        return fail("f needs at least three corners");
    }
    // A fan of triangles from the first corner. Godot's importer adds each triangle's corners in
    // the order 1, 0, 2, its clockwise order; they're kept counter-clockwise here until the
    // surface is done.
    std::array<std::string_view, 3> word{v[1], v[2], {}};
    const std::size_t count = parts(v[1]).size();
    if (parts(v[2]).size() != count) {
        return fail("its corners " + quoted(v[1]) + " and " + quoted(v[2]) +
                    " don't have the same parts (v, v/vt, v/vt/vn or v//vn)");
    }
    for (std::size_t i = 2; i + 1 < v.size(); ++i) {
        word[2] = v[i + 1];
        if (parts(word[2]).size() != count) {
            return fail("its corners " + quoted(v[1]) + " and " + quoted(word[2]) +
                        " don't have the same parts (v, v/vt, v/vt/vn or v//vn)");
        }
        if (corners_.size() + 3 > render::kMaxMeshIndices) {
            return fail("a surface has more than " + std::to_string(render::kMaxMeshIndices / 3) +
                        " triangles");
        }
        std::array<render::Vertex3D, 3> triangle;
        for (const std::size_t k : {1U, 0U, 2U}) {
            auto made = corner(word[k], at);
            if (!made) {
                return made.error();
            }
            triangle[k] = made.value();
        }
        corners_.insert(corners_.end(), triangle.begin(), triangle.end());
        groups_.insert(groups_.end(), 3, smoothing_ ? smooth_group_ : kFlat);
        word[1] = word[2];
    }
    return {};
}

Result<render::Vertex3D> ObjReader::corner(std::string_view word, std::size_t at) {
    const std::vector<std::string_view> part = parts(word);
    // An index counts from 1, or back from the latest one if negative.
    const auto resolve = [&](std::string_view written, std::size_t count,
                             const char* what) -> Result<std::size_t> {
        std::int64_t index = integer(written) - 1;
        if (index < 0) {
            index += static_cast<std::int64_t>(count) + 1;
        }
        if (index < 0 || index >= static_cast<std::int64_t>(count)) {
            return make_error(ErrorCode::InvalidArgument,
                              "line " + std::to_string(at) + ": corner " + quoted(word) +
                                  " names " + what + " " + std::string(written) + ", but " +
                                  std::to_string(count) + " come before it");
        }
        return static_cast<std::size_t>(index);
    };
    if (part.size() >= 2 && !part[1].empty()) {
        auto uv = resolve(part[1], uvs_.size(), "texture coordinate");
        if (!uv) {
            return uv.error();
        }
        last_.uv = uvs_[uv.value()];
        uses_uvs_ = true;
    }
    if (part.size() == 3) {
        auto normal = resolve(part[2], normals_.size(), "normal");
        if (!normal) {
            return normal.error();
        }
        const Vec3 n = normals_[normal.value()];
        last_.normal = {n.x, n.y, n.z};
        surface_normals_ = true;
        // Tangents need texture coordinates; without them, Godot's importer makes up one.
        if (!uses_uvs_) {
            last_ = dummy_tangent(last_);
        }
    } else if (!uses_uvs_) {
        last_.tangent = {1.0F, 0.0F, 0.0F, 1.0F};
    }
    auto vertex = resolve(part[0], vertices_.size(), "vertex");
    if (!vertex) {
        return vertex.error();
    }
    const Vec3 p = vertices_[vertex.value()];
    last_.position = {p.x, p.y, p.z};
    if (!colors_.empty()) {
        last_.rgba = colors_[vertex.value()];
    }
    return last_;
}

Result<void> ObjReader::commit_surface() {
    if (corners_.empty()) {
        return {};
    }
    Triangles triangles;
    triangles.indices.resize(corners_.size());
    for (std::size_t i = 0; i < corners_.size(); ++i) {
        triangles.indices[i] = static_cast<std::uint32_t>(i);
    }
    // Godot's importer makes normals for a file without any; so does a surface without any here.
    if (!surface_normals_) {
        smooth_normals(corners_, groups_);
    }
    triangles.vertices = std::move(corners_);
    if (uses_uvs_) {
        generate_tangents(triangles);
    } else {
        reindex(triangles);
    }
    if (triangles.vertices.size() > render::kMaxMeshVertices) {
        return make_error(ErrorCode::InvalidArgument,
                          "a surface of " + quoted(name_) + " has " +
                              std::to_string(triangles.vertices.size()) + " vertices; at most " +
                              std::to_string(render::kMaxMeshVertices) + " draw");
    }
    for (std::size_t i = 0; i + 2 < triangles.indices.size(); i += 3) {
        std::swap(triangles.indices[i + 1], triangles.indices[i + 2]);
    }
    ModelSurface surface;
    surface.vertices = std::move(triangles.vertices);
    surface.indices = std::move(triangles.indices);
    surface.material = material(library_, material_);
    mesh_.surfaces.push_back(std::move(surface));
    corners_.clear();
    groups_.clear();
    surface_normals_ = false;
    uses_uvs_ = false;
    return {};
}

void ObjReader::commit_mesh() {
    if (!mesh_.surfaces.empty()) {
        mesh_.name = name_;
        model_.parts.push_back({.node = name_, .mesh = model_.meshes.size(), .transform = {}});
        model_.meshes.push_back(std::move(mesh_));
        mesh_ = {};
    }
    name_ = "Mesh";
    material_.clear();
}

void ObjReader::use_library(std::string name, std::size_t at) {
    library_ = name;
    if (libraries_.contains(name)) {
        return;
    }
    std::optional<Library>& library = libraries_[name];
    const std::string where = "line " + std::to_string(at) + ": mtllib names ";
    const std::size_t slash = path_.rfind('/');
    const std::string folder = slash == std::string::npos ? "" : path_.substr(0, slash + 1);
    std::optional<std::string> file;
    if (!name.empty() && name.front() != '/' && name.find(':') == std::string::npos) {
        file = normalized(folder + name);
    }
    if (!file || !is_project_path(*file)) {
        problems_.add(where + quoted(name) +
                      ", which is outside the project folder; its materials are left out");
        return;
    }
    auto text = read_(*file);
    if (!text) {
        problems_.add(where + quoted(*file) + ", which could not be read: " + text.error().message +
                      "; its materials are left out");
        return;
    }
    library = parse_library(text.value(), *file, problems_);
}

int ObjReader::material(const std::string& library, const std::string& name) {
    const auto found = libraries_.find(library);
    if (found == libraries_.end()) {
        return -1;
    }
    const std::optional<Library>& materials = found->second;
    if (!materials) {
        return -1;
    }
    const auto entry = materials->find(name);
    if (entry == materials->end()) {
        return -1;
    }
    const std::string key = library + '\n' + name;
    if (const auto made = materials_.find(key); made != materials_.end()) {
        return made->second;
    }
    const LibraryMaterial& from = entry->second;
    ModelMaterial out;
    out.name = name;
    out.standard = from.standard;
    out.albedo_image = image(from.albedo);
    out.metallic_image = image(from.metallic);
    out.roughness_image = image(from.roughness);
    out.normal_image = image(from.normal);
    const int index = static_cast<int>(model_.materials.size());
    model_.materials.push_back(std::move(out));
    materials_.emplace(key, index);
    return index;
}

int ObjReader::image(const std::string& file) {
    if (file.empty()) {
        return -1;
    }
    const auto [it, added] = images_.emplace(file, static_cast<int>(model_.images.size()));
    if (added) {
        model_.images.push_back({.path = file, .image = {}});
    }
    return it->second;
}

} // namespace

Result<Model> load_obj(std::string_view bytes, std::string_view path, const ModelReader& read,
                       std::vector<json::ValidationError>* problems) {
    return ObjReader(path, read, Problems{problems}).run(bytes);
}

} // namespace slash::scene3d
