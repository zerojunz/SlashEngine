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

    // A bone: a LimbNode model with the node attribute that makes it one.
    FbxNode& bone(std::int64_t id, std::int64_t attribute, std::string name,
                  std::vector<double> translation = {}) {
        object("NodeAttribute", attribute, "NodeAttribute", name, "LimbNode")
            .add("TypeFlags", {std::string("Skeleton")});
        connect(attribute, id);
        FbxNode& node = model(id, std::move(name), std::move(translation));
        node.properties[2] = std::string("LimbNode");
        return node;
    }

    // A skin deforming a geometry.
    void skin(std::int64_t id, std::int64_t geometry, std::string name) {
        FbxNode& node = object("Deformer", id, "Deformer", std::move(name), "Skin");
        node.add("Version", {std::int64_t{101}});
        node.add("Link_DeformAcuracy", {50.0});
        connect(id, geometry);
    }

    // A cluster of a skin: the vertices a bone moves and how much, with the mesh in the bone's
    // space and the bone in the world's when they were bound (4x4 matrices, a column at a time),
    // as Blender's exporter writes them.
    void cluster(std::int64_t id, std::int64_t skin, std::int64_t bone,
                 std::vector<std::int32_t> indexes, std::vector<double> weights,
                 std::vector<double> transform, std::vector<double> link) {
        FbxNode& node = object("Deformer", id, "SubDeformer", "", "Cluster");
        node.add("Version", {std::int64_t{100}});
        node.add("UserData", {std::string(), std::string()});
        node.add("Indexes", {std::move(indexes)});
        node.add("Weights", {std::move(weights)});
        node.add("Transform", {std::move(transform)});
        node.add("TransformLink", {std::move(link)});
        connect(id, skin);
        connect(bone, id);
    }

    // FBX's time unit: ticks a second.
    static constexpr std::int64_t kTicks = 46186158000;

    // An animation stack from start to stop seconds, with one layer.
    void animation_stack(std::int64_t id, std::int64_t layer, std::string name, double start,
                         double stop) {
        FbxNode& properties =
            object("AnimationStack", id, "AnimStack", std::move(name)).add("Properties70");
        const auto ticks = [](double seconds) {
            return static_cast<std::int64_t>(seconds * static_cast<double>(kTicks));
        };
        fbx_property(properties, "LocalStart", "KTime", {ticks(start)}, "");
        fbx_property(properties, "LocalStop", "KTime", {ticks(stop)}, "");
        fbx_property(properties, "ReferenceStart", "KTime", {ticks(start)}, "");
        fbx_property(properties, "ReferenceStop", "KTime", {ticks(stop)}, "");
        object("AnimationLayer", layer, "AnimLayer", "BaseLayer");
        connect(layer, id);
    }

    // Animates a model's property ("Lcl Translation", "Lcl Rotation" in degrees or
    // "Lcl Scaling") in a layer: a curve node holding its value, and for each axis given ("X",
    // "Y" or "Z") a linear curve through values at times in seconds.
    void animate(
        std::int64_t id, std::int64_t layer, std::int64_t model, std::string property,
        const std::vector<double>& value,
        const std::vector<std::pair<std::string, std::vector<std::pair<double, double>>>>& curves) {
        FbxNode& properties =
            object("AnimationCurveNode", id, "AnimCurveNode", property.substr(4, 1)) // T, R or S
                .add("Properties70");
        fbx_property(properties, "d|X", "Number", {value.at(0)});
        fbx_property(properties, "d|Y", "Number", {value.at(1)});
        fbx_property(properties, "d|Z", "Number", {value.at(2)});
        connect(id, layer);
        connect(id, model, std::move(property));
        std::int64_t curve_id = id;
        for (const auto& [axis, keys] : curves) {
            ++curve_id;
            FbxNode& curve = object("AnimationCurve", curve_id, "AnimCurve", "");
            std::vector<double> times;
            std::vector<double> values;
            for (const auto& [time, v] : keys) {
                times.push_back(time * static_cast<double>(kTicks));
                values.push_back(v);
            }
            curve.add("Default", {values.empty() ? 0.0 : values.front()});
            curve.add("KeyVer", {std::int64_t{4009}});
            curve.add("KeyTime", {std::move(times)});
            curve.add("KeyValueFloat", {std::move(values)});
            curve.add("KeyAttrFlags", {std::vector<std::int32_t>{4}}); // linear
            curve.add("KeyAttrDataFloat", {std::vector<std::int32_t>{0, 0, 0, 0}});
            curve.add("KeyAttrRefCount",
                      {std::vector<std::int32_t>{static_cast<std::int32_t>(keys.size())}});
            connect(curve_id, id, "d|" + axis);
        }
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

// The arm of tests/gltf.hpp as an FBX file, with y up and units_per_meter units to a meter (100
// for centimeters, as Blender writes): a strip from y 0 to 2 meters, half a meter either side of
// x 0, whose bottom row follows the bone "Upper" (at the origin), its top row the bone "Lower" (at
// y 1) and its middle row both, half each. Its animation stacks are "Bend", which turns Lower a
// quarter turn about z over a second, and "Idle-loop", which raises Upper by 0.25 and back.
inline Fbx fbx_arm(double units_per_meter = 1.0) {
    const double u = units_per_meter;
    Fbx file;
    file.unit_scale = 100.0 / u;
    file.geometry(10, "Arm",
                  {-0.5 * u, 0.0, 0.0, 0.5 * u, 0.0, 0.0, -0.5 * u, u, 0.0, 0.5 * u, u, 0.0,
                   -0.5 * u, 2.0 * u, 0.0, 0.5 * u, 2.0 * u, 0.0},
                  {0, 1, 3, -3, 2, 3, 5, -5});
    file.model(20, "Armature").properties[2] = std::string("Null");
    file.bone(21, 31, "Upper");
    file.bone(22, 32, "Lower", {0.0, u, 0.0});
    file.model(23, "Arm");
    file.connect(20, 0);
    file.connect(21, 20);
    file.connect(22, 21);
    file.connect(23, 20);
    file.connect(10, 23);
    const std::vector<double> identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const std::vector<double> up{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, u, 0, 1};
    const std::vector<double> down{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, -u, 0, 1};
    file.skin(40, 10, "ArmSkin");
    file.cluster(41, 40, 21, {0, 1, 2, 3}, {1.0, 1.0, 0.5, 0.5}, identity, identity);
    file.cluster(42, 40, 22, {2, 3, 4, 5}, {0.5, 0.5, 1.0, 1.0}, down, up);
    file.animation_stack(50, 51, "Bend", 0.0, 1.0);
    file.animate(52, 51, 22, "Lcl Rotation", {0.0, 0.0, 0.0}, {{"Z", {{0.0, 0.0}, {1.0, 90.0}}}});
    file.animation_stack(60, 61, "Idle-loop", 0.0, 1.0);
    file.animate(62, 61, 21, "Lcl Translation", {0.0, 0.0, 0.0},
                 {{"Y", {{0.0, 0.0}, {0.5, 0.25 * u}, {1.0, 0.0}}}});
    return file;
}

} // namespace slash::test
