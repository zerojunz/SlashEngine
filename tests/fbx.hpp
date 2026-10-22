#pragma once

#include <doctest/doctest.h>

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

// FBX 7.4 files written in tests, as ASCII or binary, so tests make the models they read rather
// than commit them.
namespace slash::test {

// One record of an FBX file: its name, its properties and the records inside it.
struct FbxNode {
    // An object's name, which ASCII files write "Kind::name" and binary ones "name\0\1Kind".
    struct Name {
        std::string kind;
        std::string name;
    };
    // Bytes, such as an image a file carries; binary files only.
    struct Raw {
        std::string bytes;
    };
    using Property = std::variant<std::int64_t, double, std::string, Name, std::vector<double>,
                                  std::vector<std::int32_t>, Raw>;

    std::string name;
    std::vector<Property> properties;
    std::vector<FbxNode> children;

    // Adds a record inside this one. The reference lasts until the next one is added.
    FbxNode& add(std::string child, std::vector<Property> values = {}) {
        children.push_back({std::move(child), std::move(values), {}});
        return children.back();
    }
};

// A property of a Properties70 record: P: "name", "type", "label", "flags", values...
inline void fbx_property(FbxNode& properties, std::string name, std::string type,
                         const std::vector<FbxNode::Property>& values, std::string flags = "A") {
    std::vector<FbxNode::Property> all{std::move(name), std::move(type), std::string(),
                                       std::move(flags)};
    all.insert(all.end(), values.begin(), values.end());
    properties.add("P", std::move(all));
}

// An FBX file a test writes: its settings, objects and the connections between them.
struct Fbx {
    // GlobalSettings: the up and front axes (0 x, 1 y, 2 z) and signs, and centimeters per unit.
    std::int64_t up_axis = 1;
    std::int64_t up_sign = 1;
    std::int64_t front_axis = 2;
    std::int64_t front_sign = 1;
    double unit_scale = 100.0;
    FbxNode objects{.name = "Objects", .properties = {}, .children = {}};
    FbxNode connections{.name = "Connections", .properties = {}, .children = {}};

    FbxNode& object(std::string type, std::int64_t id, std::string kind, std::string name,
                    std::string subclass = "") {
        return objects.add(std::move(type), {id, FbxNode::Name{std::move(kind), std::move(name)},
                                             std::move(subclass)});
    }

    // Puts child under parent (0 for the scene's root), or on parent's property if one is named.
    void connect(std::int64_t child, std::int64_t parent, std::string property = "") {
        if (property.empty()) {
            connections.add("C", {std::string("OO"), child, parent});
        } else {
            connections.add("C", {std::string("OP"), child, parent, std::move(property)});
        }
    }

    // A mesh's corners: positions, and each polygon's corners with the last one written -1 - i.
    // Normals, texture coordinates and colors are per corner of each polygon when given, and
    // materials per polygon (one for all of them when there's one).
    FbxNode& geometry(std::int64_t id, std::string name, std::vector<double> positions,
                      std::vector<std::int32_t> polygons, std::vector<double> normals = {},
                      std::vector<double> uvs = {}, std::vector<double> colors = {},
                      std::vector<std::int32_t> materials = {}) {
        FbxNode& node = object("Geometry", id, "Geometry", std::move(name), "Mesh");
        node.add("Vertices", {std::move(positions)});
        node.add("PolygonVertexIndex", {std::move(polygons)});
        std::vector<std::string> layers;
        const auto layer = [&](std::string type, std::string mapping, std::string reference) {
            FbxNode& element = node.add(type, {std::int64_t{0}});
            element.add("Version", {std::int64_t{101}});
            element.add("Name", {std::string()});
            element.add("MappingInformationType", {std::move(mapping)});
            element.add("ReferenceInformationType", {std::move(reference)});
            layers.push_back(std::move(type));
            return &node.children.back();
        };
        if (!normals.empty()) {
            layer("LayerElementNormal", "ByPolygonVertex", "Direct")
                ->add("Normals", {std::move(normals)});
        }
        if (!uvs.empty()) {
            std::vector<std::int32_t> index;
            for (std::size_t i = 0; i < uvs.size() / 2; ++i) {
                index.push_back(static_cast<std::int32_t>(i));
            }
            FbxNode* element = layer("LayerElementUV", "ByPolygonVertex", "IndexToDirect");
            element->add("UV", {std::move(uvs)});
            element->add("UVIndex", {std::move(index)});
        }
        if (!colors.empty()) {
            layer("LayerElementColor", "ByPolygonVertex", "Direct")
                ->add("Colors", {std::move(colors)});
        }
        if (!materials.empty()) {
            const char* mapping = materials.size() == 1 ? "AllSame" : "ByPolygon";
            layer("LayerElementMaterial", mapping, "IndexToDirect")
                ->add("Materials", {std::move(materials)});
        }
        FbxNode& all = node.add("Layer", {std::int64_t{0}});
        all.add("Version", {std::int64_t{100}});
        for (std::string& type : layers) {
            FbxNode& element = all.add("LayerElement");
            element.add("Type", {std::move(type)});
            element.add("TypedIndex", {std::int64_t{0}});
        }
        return node;
    }

    // A node of the scene: its translation, rotation in degrees and scale.
    FbxNode& model(std::int64_t id, std::string name, std::vector<double> translation = {},
                   std::vector<double> rotation = {}, std::vector<double> scaling = {}) {
        FbxNode& node = object("Model", id, "Model", std::move(name), "Mesh");
        node.add("Version", {std::int64_t{232}});
        FbxNode& properties = node.add("Properties70");
        const auto vector = [&](const char* property, std::vector<double>& v) {
            if (!v.empty()) {
                fbx_property(properties, property, property, {v[0], v[1], v[2]});
            }
        };
        vector("Lcl Translation", translation);
        vector("Lcl Rotation", rotation);
        vector("Lcl Scaling", scaling);
        return node;
    }

    // A material of the given shading model ("lambert" or "phong"); its properties go in the
    // Properties70 record it returns.
    FbxNode& material(std::int64_t id, std::string name, std::string shading = "lambert") {
        FbxNode& node = object("Material", id, "Material", std::move(name));
        node.add("Version", {std::int64_t{102}});
        node.add("ShadingModel", {std::move(shading)});
        return node.add("Properties70");
    }

    // A texture of a file, as exporters write one: FileName as it was on the machine that wrote
    // it, RelativeFilename relative to the model.
    FbxNode& texture(std::int64_t id, std::string name, std::string file, std::string relative) {
        FbxNode& node = object("Texture", id, "Texture", std::move(name));
        node.add("Type", {std::string("TextureVideoClip")});
        node.add("FileName", {std::move(file)});
        node.add("RelativeFilename", {std::move(relative)});
        return node;
    }

    // The image a texture carries inside the file, as a video clip connected to it.
    FbxNode& video(std::int64_t id, std::int64_t texture, std::string file, std::string content) {
        FbxNode& node = object("Video", id, "Video", file, "Clip");
        node.add("Type", {std::string("Clip")});
        node.add("FileName", {file});
        node.add("RelativeFilename", {std::move(file)});
        node.add("Content", {FbxNode::Raw{std::move(content)}});
        connect(id, texture);
        return objects.children.back();
    }

    [[nodiscard]] std::vector<FbxNode> records() const {
        FbxNode header{.name = "FBXHeaderExtension", .properties = {}, .children = {}};
        header.add("FBXHeaderVersion", {std::int64_t{1003}});
        header.add("FBXVersion", {std::int64_t{7400}});
        FbxNode settings{.name = "GlobalSettings", .properties = {}, .children = {}};
        settings.add("Version", {std::int64_t{1000}});
        FbxNode& properties = settings.add("Properties70");
        const std::int64_t coord_axis = 3 - up_axis - front_axis;
        fbx_property(properties, "UpAxis", "int", {up_axis}, "");
        fbx_property(properties, "UpAxisSign", "int", {up_sign}, "");
        fbx_property(properties, "FrontAxis", "int", {front_axis}, "");
        fbx_property(properties, "FrontAxisSign", "int", {front_sign}, "");
        fbx_property(properties, "CoordAxis", "int", {coord_axis}, "");
        fbx_property(properties, "CoordAxisSign", "int", {std::int64_t{1}}, "");
        fbx_property(properties, "UnitScaleFactor", "double", {unit_scale}, "");
        return {header, settings, objects, connections};
    }

    [[nodiscard]] std::string ascii() const {
        std::string out = "; FBX 7.4.0 project file\n";
        for (const FbxNode& node : records()) {
            write_ascii(node, out, 0);
        }
        return out;
    }

    [[nodiscard]] std::string binary() const {
        std::string out("Kaydara FBX Binary  \0\x1a\0", 23);
        word(out, 7400);
        for (const FbxNode& node : records()) {
            write_binary(node, out);
        }
        out.append(13, '\0');
        return out;
    }

private:
    static std::string number(double value) {
        std::array<char, 32> text{};
        const auto end = std::to_chars(text.data(), text.data() + text.size(), value).ptr;
        return {text.data(), end};
    }

    static void write_ascii(const FbxNode& node, std::string& out, int depth) {
        const std::string indent(static_cast<std::size_t>(depth), '\t');
        out += indent + node.name + ":";
        if (node.properties.size() == 1 &&
            (std::holds_alternative<std::vector<double>>(node.properties[0]) ||
             std::holds_alternative<std::vector<std::int32_t>>(node.properties[0]))) {
            std::string values;
            std::size_t count = 0;
            if (const auto* d = std::get_if<std::vector<double>>(&node.properties[0])) {
                for (const double v : *d) {
                    values += (count++ == 0 ? "" : ",") + number(v);
                }
            } else {
                for (const std::int32_t v :
                     std::get<std::vector<std::int32_t>>(node.properties[0])) {
                    values += (count++ == 0 ? "" : ",") + std::to_string(v);
                }
            }
            out += " *" + std::to_string(count) + " {\n" + indent + "\ta: " + values + "\n" +
                   indent + "}\n";
            return;
        }
        for (std::size_t i = 0; i < node.properties.size(); ++i) {
            out += i == 0 ? " " : ", ";
            const FbxNode::Property& p = node.properties[i];
            if (const auto* integer = std::get_if<std::int64_t>(&p)) {
                out += std::to_string(*integer);
            } else if (const auto* real = std::get_if<double>(&p)) {
                out += number(*real);
            } else if (const auto* text = std::get_if<std::string>(&p)) {
                out += "\"" + *text + "\"";
            } else if (const auto* name = std::get_if<FbxNode::Name>(&p)) {
                out += "\"" + name->kind + "::" + name->name + "\"";
            } else {
                FAIL("only binary FBX files carry bytes");
            }
        }
        if (node.children.empty()) {
            out += "\n";
            return;
        }
        out += " {\n";
        for (const FbxNode& child : node.children) {
            write_ascii(child, out, depth + 1);
        }
        out += indent + "}\n";
    }

    static void word(std::string& out, std::uint32_t value) {
        for (int i = 0; i < 4; ++i) {
            out.push_back(static_cast<char>((value >> (8 * i)) & 0xFFU));
        }
    }

    template <typename T>
    static void raw(std::string& out, T value) {
        std::array<char, sizeof(T)> bytes{};
        std::memcpy(bytes.data(), &value, sizeof(T));
        out.append(bytes.data(), bytes.size());
    }

    static void set_word(std::string& out, std::size_t at, std::size_t value) {
        const auto v = static_cast<std::uint32_t>(value);
        std::memcpy(out.data() + at, &v, sizeof(v));
    }

    static void write_binary(const FbxNode& node, std::string& out) {
        const std::size_t start = out.size();
        word(out, 0); // where the record ends
        word(out, static_cast<std::uint32_t>(node.properties.size()));
        word(out, 0); // the properties' size
        out.push_back(static_cast<char>(node.name.size()));
        out += node.name;
        const std::size_t properties = out.size();
        for (const FbxNode::Property& p : node.properties) {
            if (const auto* integer = std::get_if<std::int64_t>(&p)) {
                if (*integer >= std::numeric_limits<std::int32_t>::min() &&
                    *integer <= std::numeric_limits<std::int32_t>::max()) {
                    out.push_back('I');
                    raw(out, static_cast<std::int32_t>(*integer));
                } else {
                    out.push_back('L');
                    raw(out, *integer);
                }
            } else if (const auto* real = std::get_if<double>(&p)) {
                out.push_back('D');
                raw(out, *real);
            } else if (const auto* text = std::get_if<std::string>(&p)) {
                out.push_back('S');
                word(out, static_cast<std::uint32_t>(text->size()));
                out += *text;
            } else if (const auto* name = std::get_if<FbxNode::Name>(&p)) {
                const std::string joined = name->name + std::string("\0\1", 2) + name->kind;
                out.push_back('S');
                word(out, static_cast<std::uint32_t>(joined.size()));
                out += joined;
            } else if (const auto* reals = std::get_if<std::vector<double>>(&p)) {
                out.push_back('d');
                word(out, static_cast<std::uint32_t>(reals->size()));
                word(out, 0);
                word(out, static_cast<std::uint32_t>(reals->size() * sizeof(double)));
                for (const double v : *reals) {
                    raw(out, v);
                }
            } else if (const auto* integers = std::get_if<std::vector<std::int32_t>>(&p)) {
                out.push_back('i');
                word(out, static_cast<std::uint32_t>(integers->size()));
                word(out, 0);
                word(out, static_cast<std::uint32_t>(integers->size() * sizeof(std::int32_t)));
                for (const std::int32_t v : *integers) {
                    raw(out, v);
                }
            } else {
                const std::string& bytes = std::get<FbxNode::Raw>(p).bytes;
                out.push_back('R');
                word(out, static_cast<std::uint32_t>(bytes.size()));
                out += bytes;
            }
        }
        set_word(out, start + 8, out.size() - properties);
        for (const FbxNode& child : node.children) {
            write_binary(child, out);
        }
        if (!node.children.empty()) {
            out.append(13, '\0');
        }
        set_word(out, start, out.size());
    }
};

} // namespace slash::test
