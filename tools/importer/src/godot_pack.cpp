#include "slash/importer/godot_pack.hpp"

#include "common.hpp"
#include "godot_binary.hpp"

#include "slash/core/text.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

// Godot's exported formats written from its text ones, as its export writes them: binary
// resources (ResourceFormatSaverBinaryInstance::save), scenes packed as SceneState::pack packs
// them, values as marshalls.cpp's encode_variant encodes them, project.binary
// (ProjectSettings::_save_settings_binary), imported textures and sounds, and the pack
// (EditorExportPlatform::save_pack).
namespace slash::importer::godot {
namespace {

Value make_kind(Value::Kind kind) {
    Value value;
    value.kind = kind;
    return value;
}

Value make_int(std::int64_t number) {
    Value value = make_kind(Value::Kind::Int);
    value.integer = number;
    return value;
}

Value make_string(std::string text) {
    Value value = make_kind(Value::Kind::String);
    value.text = std::move(text);
    return value;
}

Value make_call(std::string name, std::vector<Value> items = {}) {
    Value value = make_kind(Value::Kind::Call);
    value.text = std::move(name);
    value.items = std::move(items);
    return value;
}

std::string res_path(std::string_view path) {
    return path.starts_with("res://") ? std::string(path) : "res://" + std::string(path);
}

// --- Values ------------------------------------------------------------------------------------

// The fixed-size math types, by their text constructor, with how both binary formats code them.
struct MathType {
    std::string_view name;
    std::size_t count;
    bool integers;
    VariantType type;   // marshalls.cpp's
    std::uint32_t code; // resource_format_binary.cpp's
};

constexpr std::array<MathType, 16> kMathTypes = {{
    {"Vector2", 2, false, VariantType::Vector2, kVariantVector2},
    {"Vector2i", 2, true, VariantType::Vector2i, kVariantVector2i},
    {"Rect2", 4, false, VariantType::Rect2, kVariantRect2},
    {"Rect2i", 4, true, VariantType::Rect2i, kVariantRect2i},
    {"Vector3", 3, false, VariantType::Vector3, kVariantVector3},
    {"Vector3i", 3, true, VariantType::Vector3i, kVariantVector3i},
    {"Vector4", 4, false, VariantType::Vector4, kVariantVector4},
    {"Vector4i", 4, true, VariantType::Vector4i, kVariantVector4i},
    {"Plane", 4, false, VariantType::Plane, kVariantPlane},
    {"Quaternion", 4, false, VariantType::Quaternion, kVariantQuaternion},
    {"AABB", 6, false, VariantType::Aabb, kVariantAabb},
    {"Basis", 9, false, VariantType::Basis, kVariantBasis},
    {"Transform2D", 6, false, VariantType::Transform2d, kVariantTransform2d},
    {"Transform3D", 12, false, VariantType::Transform3d, kVariantTransform3d},
    {"Projection", 16, false, VariantType::Projection, kVariantProjection},
    {"Color", 4, false, VariantType::Color, kVariantColor},
}};

enum class Element : std::uint8_t { Byte, Int32, Int64, Float32, Float64, String, Reals };

struct PackedType {
    std::string_view name;
    Element element;
    std::size_t components; // numbers per element, for Reals
    VariantType type;
    std::uint32_t code;
};

constexpr std::array<PackedType, 10> kPackedTypes = {{
    {"PackedByteArray", Element::Byte, 1, VariantType::PackedByteArray, kVariantPackedByteArray},
    {"PackedInt32Array", Element::Int32, 1, VariantType::PackedInt32Array,
     kVariantPackedInt32Array},
    {"PackedInt64Array", Element::Int64, 1, VariantType::PackedInt64Array,
     kVariantPackedInt64Array},
    {"PackedFloat32Array", Element::Float32, 1, VariantType::PackedFloat32Array,
     kVariantPackedFloat32Array},
    {"PackedFloat64Array", Element::Float64, 1, VariantType::PackedFloat64Array,
     kVariantPackedFloat64Array},
    {"PackedStringArray", Element::String, 1, VariantType::PackedStringArray,
     kVariantPackedStringArray},
    {"PackedVector2Array", Element::Reals, 2, VariantType::PackedVector2Array,
     kVariantPackedVector2Array},
    {"PackedVector3Array", Element::Reals, 3, VariantType::PackedVector3Array,
     kVariantPackedVector3Array},
    {"PackedVector4Array", Element::Reals, 4, VariantType::PackedVector4Array,
     kVariantPackedVector4Array},
    {"PackedColorArray", Element::Reals, 4, VariantType::PackedColorArray,
     kVariantPackedColorArray},
}};

const MathType* math_type(std::string_view name) {
    for (const MathType& type : kMathTypes) {
        if (type.name == name) {
            return &type;
        }
    }
    return nullptr;
}

const PackedType* packed_type(std::string_view name) {
    for (const PackedType& type : kPackedTypes) {
        if (type.name == name) {
            return &type;
        }
    }
    return nullptr;
}

Error value_error(const std::string& message) {
    return make_error(ErrorCode::InvalidArgument, message);
}

// A math type's numbers, checked against its size.
Result<std::vector<double>> math_numbers(const Value& value, const MathType& type) {
    auto numbers = value.numbers();
    if (!numbers || numbers->size() != type.count) {
        return value_error(std::string(type.name) + " takes " + std::to_string(type.count) +
                           " numbers");
    }
    return std::move(numbers.value());
}

void write_math(ByteWriter& out, const MathType& type, const std::vector<double>& numbers) {
    for (const double number : numbers) {
        if (type.integers) {
            out.u32(static_cast<std::uint32_t>(static_cast<std::int32_t>(number)));
        } else {
            out.f32(static_cast<float>(number));
        }
    }
}

// A PackedByteArray's bytes: base64, as Godot 4.3 and later write them, or a list of numbers.
Result<std::string> byte_array_bytes(const Value& value) {
    if (value.items.size() == 1 && value.items.front().is_string()) {
        auto bytes = base64_decode(value.items.front().text);
        if (!bytes) {
            return value_error("a PackedByteArray whose base64 doesn't decode");
        }
        return std::move(bytes.value());
    }
    std::string bytes;
    for (const Value& item : value.items) {
        bytes += static_cast<char>(static_cast<std::uint8_t>(item.number()));
    }
    return bytes;
}

// Writes a packed array's count and elements; `string` writes one of a PackedStringArray's.
template <typename WriteString>
Result<void> write_packed(ByteWriter& out, const Value& value, const PackedType& type,
                          WriteString&& string) {
    if (type.element == Element::Byte) {
        auto bytes = byte_array_bytes(value);
        if (!bytes) {
            return bytes.error();
        }
        const std::size_t size = bytes.value().size();
        out.u32(static_cast<std::uint32_t>(size));
        out.bytes(bytes.value());
        out.bytes(std::string((4 - (size % 4)) % 4, '\0')); // by the size, not the position
        return {};
    }
    if (type.element == Element::String) {
        out.u32(static_cast<std::uint32_t>(value.items.size()));
        for (const Value& item : value.items) {
            string(item.text);
        }
        return {};
    }
    if (value.items.size() % type.components != 0) {
        return value_error(std::string(type.name) + " takes numbers in groups of " +
                           std::to_string(type.components));
    }
    out.u32(static_cast<std::uint32_t>(value.items.size() / type.components));
    for (const Value& item : value.items) {
        switch (type.element) {
        case Element::Int32:
            out.u32(static_cast<std::uint32_t>(static_cast<std::int32_t>(item.number())));
            break;
        case Element::Int64:
            out.u64(static_cast<std::uint64_t>(item.kind == Value::Kind::Int
                                                   ? item.integer
                                                   : static_cast<std::int64_t>(item.number())));
            break;
        case Element::Float64:
            out.f64(item.number());
            break;
        default:
            out.f32(static_cast<float>(item.number()));
            break;
        }
    }
    return {};
}

bool fits_int32(std::int64_t value) {
    return value >= std::numeric_limits<std::int32_t>::min() &&
           value <= std::numeric_limits<std::int32_t>::max();
}

// Whether a float survives as 32 bits; NaN doesn't, as Godot's check has it.
bool fits_float(double value) {
    return static_cast<double>(static_cast<float>(value)) == value;
}

// NodePath's parts as its constructor splits its text: names between '/', then subnames after
// each ':'.
struct NodePathParts {
    std::vector<std::string> names;
    std::vector<std::string> subnames;
    bool absolute = false;
};

NodePathParts node_path_parts(std::string_view text) {
    NodePathParts parts;
    if (text.starts_with('/')) {
        parts.absolute = true;
        text.remove_prefix(1);
    }
    const std::size_t colon = text.find(':');
    for (const std::string_view name : split_path(text.substr(0, colon))) {
        if (!name.empty()) {
            parts.names.emplace_back(name);
        }
    }
    if (colon != std::string_view::npos) {
        for (const std::string_view name : split_path(text.substr(colon + 1), ':')) {
            if (!name.empty()) {
                parts.subnames.emplace_back(name);
            }
        }
    }
    return parts;
}

// --- marshalls.cpp's encode_variant, objects in full ---

void marshal_string(ByteWriter& out, std::string_view text) {
    out.u32(static_cast<std::uint32_t>(text.size()));
    out.bytes(text);
    out.pad(4);
}

Result<void> marshal(ByteWriter& out, const Value& value, int depth) {
    if (depth > kMaxDepth) {
        return value_error("values nest too deeply");
    }
    const auto header = [&](VariantType type, std::uint32_t flags = 0) {
        out.u32(static_cast<std::uint32_t>(type) | flags);
    };
    switch (value.kind) {
    case Value::Kind::Nil:
        header(VariantType::Nil);
        return {};
    case Value::Kind::Bool:
        header(VariantType::Bool);
        out.u32(value.boolean ? 1 : 0);
        return {};
    case Value::Kind::Int:
        if (fits_int32(value.integer)) {
            header(VariantType::Int);
            out.u32(static_cast<std::uint32_t>(value.integer));
        } else {
            header(VariantType::Int, kHeaderFlag64);
            out.u64(static_cast<std::uint64_t>(value.integer));
        }
        return {};
    case Value::Kind::Float:
        if (fits_float(value.real)) {
            header(VariantType::Float);
            out.f32(static_cast<float>(value.real));
        } else {
            header(VariantType::Float, kHeaderFlag64);
            out.f64(value.real);
        }
        return {};
    case Value::Kind::String:
    case Value::Kind::StringName:
        header(value.kind == Value::Kind::String ? VariantType::String : VariantType::StringName);
        marshal_string(out, value.text);
        return {};
    case Value::Kind::Array:
        header(VariantType::Array);
        out.u32(static_cast<std::uint32_t>(value.items.size()));
        for (const Value& item : value.items) {
            if (auto done = marshal(out, item, depth + 1); !done) {
                return done;
            }
        }
        return {};
    case Value::Kind::Dictionary:
        header(VariantType::Dictionary);
        out.u32(static_cast<std::uint32_t>(value.entries.size()));
        for (const Value::Entry& entry : value.entries) {
            if (auto done = marshal(out, entry.key, depth + 1); !done) {
                return done;
            }
            if (auto done = marshal(out, entry.value, depth + 1); !done) {
                return done;
            }
        }
        return {};
    case Value::Kind::Call:
        break;
    }
    if (const MathType* type = math_type(value.text); type != nullptr) {
        auto numbers = math_numbers(value, *type);
        if (!numbers) {
            return numbers.error();
        }
        header(type->type);
        write_math(out, *type, numbers.value());
        return {};
    }
    if (const PackedType* type = packed_type(value.text); type != nullptr) {
        header(type->type);
        return write_packed(out, value, *type, [&](std::string_view text) {
            // A PackedStringArray's strings keep their terminating zero.
            out.u32(static_cast<std::uint32_t>(text.size() + 1));
            out.bytes(text);
            out.u8(0);
            out.pad(4);
        });
    }
    if (value.text == "NodePath") {
        const NodePathParts parts = node_path_parts(value.string_argument().value_or(""));
        header(VariantType::NodePath);
        out.u32(static_cast<std::uint32_t>(parts.names.size()) | 0x80000000U);
        out.u32(static_cast<std::uint32_t>(parts.subnames.size()));
        out.u32(parts.absolute ? 1 : 0);
        for (const auto* list : {&parts.names, &parts.subnames}) {
            for (const std::string& name : *list) {
                marshal_string(out, name);
            }
        }
        return {};
    }
    if (value.text == "RID") {
        header(VariantType::Rid);
        out.u64(0);
        return {};
    }
    if (value.text == "Object") {
        header(VariantType::Object);
        marshal_string(out, value.string_argument().value_or(""));
        out.u32(static_cast<std::uint32_t>(value.entries.size()));
        for (const Value::Entry& entry : value.entries) {
            marshal_string(out, entry.key.text);
            if (auto done = marshal(out, entry.value, depth + 1); !done) {
                return done;
            }
        }
        return {};
    }
    return value_error(value.text + "(...) can't be encoded as a value");
}

} // namespace

Result<std::string> encode_variant(const Value& value) {
    ByteWriter out;
    if (auto done = marshal(out, value, 0); !done) {
        return done.error();
    }
    return out.take();
}

namespace {

// --- SceneState::pack, from a .tscn file's node, connection and editable sections ---

class ScenePacker {
public:
    Result<Value> pack(const std::vector<Section>& sections) {
        std::vector<const Section*> nodes;
        for (const Section& section : sections) {
            if (section.tag == "node") {
                nodes.push_back(&section);
            }
        }
        if (nodes.empty()) {
            return value_error("a scene with no nodes");
        }
        // An inherited scene's base comes first, as pack() adds it before any node.
        if (nodes.front()->field("parent") == nullptr) {
            if (const Value* instance = nodes.front()->field("instance"); instance != nullptr) {
                base_scene_ = value_index(*instance);
            }
        }
        for (const Section* node : nodes) {
            if (auto done = add_node(*node); !done) {
                return done.error();
            }
        }
        for (const Section& section : sections) {
            if (section.tag == "connection") {
                add_connection(section);
            } else if (section.tag == "editable") {
                editables_.push_back(
                    make_call("NodePath", {make_string(section.field_string("path"))}));
            }
        }

        Value bundled = make_kind(Value::Kind::Dictionary);
        const auto put = [&](std::string key, Value value) {
            bundled.entries.push_back({make_string(std::move(key)), std::move(value)});
        };
        Value names = make_call("PackedStringArray");
        for (const std::string& name : names_) {
            names.items.push_back(make_string(name));
        }
        put("names", std::move(names));
        Value variants = make_kind(Value::Kind::Array);
        variants.items = variants_;
        put("variants", std::move(variants));
        put("node_count", make_int(static_cast<std::int64_t>(nodes.size())));
        put("nodes", ints(node_data_));
        put("conn_count", make_int(connection_count_));
        put("conns", ints(connection_data_));
        Value paths = make_kind(Value::Kind::Array);
        for (const std::string& path : node_paths_) {
            paths.items.push_back(make_call("NodePath", {make_string(path)}));
        }
        put("node_paths", std::move(paths));
        Value editables = make_kind(Value::Kind::Array);
        editables.items = editables_;
        put("editable_instances", std::move(editables));
        if (base_scene_ >= 0) {
            put("base_scene", make_int(base_scene_));
        }
        put("version", make_int(kPackedSceneVersion));
        return bundled;
    }

private:
    static Value ints(const std::vector<std::int32_t>& values) {
        Value out = make_call("PackedInt32Array");
        for (const std::int32_t value : values) {
            out.items.push_back(make_int(value));
        }
        return out;
    }

    std::int32_t name_index(const std::string& name) {
        const auto [it, added] =
            name_ids_.try_emplace(name, static_cast<std::int32_t>(names_.size()));
        if (added) {
            names_.push_back(name);
        }
        return it->second;
    }

    // Values listed once, as pack()'s variant map compares them.
    std::int32_t value_index(const Value& value) {
        std::string key = std::to_string(static_cast<int>(value.kind)) + write_value(value);
        const auto [it, added] =
            variant_ids_.try_emplace(std::move(key), static_cast<std::int32_t>(variants_.size()));
        if (added) {
            variants_.push_back(value);
        }
        return it->second;
    }

    // A saved node by its path from the root, or a path in node_paths flagged as one.
    std::int32_t target(const std::string& path) {
        if (const auto it = node_ids_.find(path.empty() ? "." : path); it != node_ids_.end()) {
            return it->second;
        }
        const auto [it, added] =
            path_ids_.try_emplace(path, static_cast<std::int32_t>(node_paths_.size()));
        if (added) {
            node_paths_.push_back(path);
        }
        return it->second | kFlagIdIsPath;
    }

    Result<void> add_node(const Section& node) {
        const std::string name = node.field_string("name");
        const Value* parent_field = node.field("parent");
        const bool root = parent_field == nullptr;
        const std::string parent = node.field_string("parent");
        const Value* type = node.field("type");
        const Value* instance = node.field("instance");
        const Value* placeholder = node.field("instance_placeholder");
        if (root && !node_ids_.empty()) {
            return value_error("node \"" + name + "\" has no parent, but the scene has a root");
        }

        const std::int32_t name_id = name_index(name);
        std::int32_t instance_id = -1;
        if (!root && instance != nullptr) {
            instance_id = value_index(*instance);
        } else if (!root && placeholder != nullptr) {
            instance_id = value_index(*placeholder) | kFlagInstanceIsPlaceholder;
        }
        std::set<std::string> node_path_properties;
        if (const Value* paths = node.field("node_paths"); paths != nullptr) {
            for (const Value& item : paths->items) {
                node_path_properties.insert(item.text);
            }
        }
        std::vector<std::int32_t> properties;
        for (const auto& [property, value] : node.properties) {
            std::int32_t id = name_index(property);
            if (node_path_properties.contains(property)) {
                id |= kFlagPathPropertyIsNode;
            }
            properties.push_back(id);
            properties.push_back(value_index(value));
        }
        std::vector<std::int32_t> groups;
        if (const Value* list = node.field("groups"); list != nullptr) {
            for (const Value& group : list->items) {
                groups.push_back(name_index(group.text));
            }
        }
        const std::int32_t type_id = type != nullptr ? name_index(type->text) : kTypeInstantiated;

        // The text loader's owner: the root's for a node it made, none for one an instanced
        // scene made.
        std::int32_t owner = -1;
        if (node.field("owner") != nullptr) {
            owner = target(node.field_string("owner"));
        } else if (!root && (type != nullptr || instance != nullptr)) {
            owner = 0;
        }
        std::int32_t index = -1;
        if (const auto number = parse_int(node.field_string("index")); number) {
            index = static_cast<std::int32_t>(*number);
        }

        node_data_.push_back(root ? -1 : target(parent));
        node_data_.push_back(owner);
        node_data_.push_back(type_id);
        auto name_word = static_cast<std::uint32_t>(name_id);
        if (index >= 0 && index < (1 << (32 - kNameIndexBits)) - 1) {
            name_word |= static_cast<std::uint32_t>(index + 1) << kNameIndexBits;
        }
        node_data_.push_back(static_cast<std::int32_t>(name_word));
        node_data_.push_back(instance_id);
        node_data_.push_back(static_cast<std::int32_t>(properties.size() / 2));
        node_data_.insert(node_data_.end(), properties.begin(), properties.end());
        node_data_.push_back(static_cast<std::int32_t>(groups.size()));
        node_data_.insert(node_data_.end(), groups.begin(), groups.end());

        std::string path = ".";
        if (!root) {
            path = parent == "." ? name : parent + "/" + name;
        }
        node_ids_[path] = static_cast<std::int32_t>(node_ids_.size());
        return {};
    }

    void add_connection(const Section& connection) {
        const std::int32_t from = target(connection.field_string("from"));
        const std::int32_t to = target(connection.field_string("to"));
        const std::int32_t method = name_index(connection.field_string("method"));
        const std::int32_t signal = name_index(connection.field_string("signal"));
        std::int32_t flags = kConnectPersist;
        if (const Value* value = connection.field("flags"); value != nullptr) {
            flags = static_cast<std::int32_t>(value->number());
        }
        std::vector<std::int32_t> binds;
        if (const Value* list = connection.field("binds"); list != nullptr) {
            for (const Value& bind : list->items) {
                binds.push_back(value_index(bind));
            }
        }
        std::int32_t unbinds = 0;
        if (const Value* value = connection.field("unbinds"); value != nullptr) {
            unbinds = static_cast<std::int32_t>(value->number());
        }
        connection_data_.insert(connection_data_.end(), {from, to, signal, method, flags});
        connection_data_.push_back(static_cast<std::int32_t>(binds.size()));
        connection_data_.insert(connection_data_.end(), binds.begin(), binds.end());
        connection_data_.push_back(unbinds);
        ++connection_count_;
    }

    std::vector<std::string> names_;
    std::map<std::string, std::int32_t> name_ids_;
    std::vector<Value> variants_;
    std::map<std::string, std::int32_t> variant_ids_;
    std::vector<std::string> node_paths_;
    std::map<std::string, std::int32_t> path_ids_;
    std::map<std::string, std::int32_t> node_ids_; // saved nodes by path from the root
    std::vector<std::int32_t> node_data_;
    std::vector<std::int32_t> connection_data_;
    std::int64_t connection_count_ = 0;
    std::vector<Value> editables_;
    std::int64_t base_scene_ = -1;
};

// --- ResourceFormatSaverBinaryInstance ---

constexpr std::uint32_t kGodotMinor = 5;
constexpr std::uint32_t kCompressedBlockSize = 4096;
constexpr std::uint32_t kCompressionZstd = 2;

class ResourceWriter {
public:
    Result<std::string> write(const std::vector<Section>& sections,
                              const ResourceOptions& options) {
        const Section* header = nullptr;
        const Section* resource = nullptr;
        for (const Section& section : sections) {
            if (section.tag == "gd_scene" || section.tag == "gd_resource") {
                header = &section;
            } else if (section.tag == "ext_resource") {
                externals_by_id_[section.field_string("id")] = &section;
            } else if (section.tag == "sub_resource") {
                subs_by_id_[section.field_string("id")] = &section;
            } else if (section.tag == "resource") {
                resource = &section;
            }
        }
        if (header == nullptr) {
            return value_error("no [gd_scene] or [gd_resource] header");
        }
        const bool scene = header->tag == "gd_scene";
        const std::string main_type = scene ? "PackedScene" : header->field_string("type");
        std::vector<std::pair<std::string, Value>> main_properties;
        if (scene) {
            auto bundled = ScenePacker().pack(sections);
            if (!bundled) {
                return bundled.error();
            }
            main_properties.emplace_back("_bundled", std::move(bundled.value()));
        } else if (resource != nullptr) {
            main_properties = resource->properties;
        }

        // _find_resources: externals by first use, sub-resources after what they hold, node
        // path names into the string table; then each resource's property names.
        for (const auto& property : main_properties) {
            if (auto found = find(property.second, 0); !found) {
                return found.error();
            }
        }
        for (const Resource& saved : saved_) {
            for (const auto& property : *saved.properties) {
                static_cast<void>(string_index(property.first));
            }
        }
        for (const auto& property : main_properties) {
            static_cast<void>(string_index(property.first));
        }

        ByteWriter out;
        out.u32(0); // little-endian
        out.u32(0); // 32-bit reals
        out.u32(4);
        out.u32(kGodotMinor);
        out.u32(kFormatVersion);
        string(out, main_type);
        out.u64(0); // import metadata offset
        const std::string script_class = header->field_string("script_class");
        out.u32(kFlagNamedSceneIds | kFlagUids |
                (script_class.empty() ? 0U : static_cast<std::uint32_t>(kFlagHasScriptClass)));
        out.u64(static_cast<std::uint64_t>(uid_id(header->field_string("uid"))));
        if (!script_class.empty()) {
            string(out, script_class);
        }
        for (int i = 0; i < kReservedFields; ++i) {
            out.u32(0);
        }
        out.u32(static_cast<std::uint32_t>(strings_.size()));
        for (const std::string& text : strings_) {
            string(out, text);
        }
        out.u32(static_cast<std::uint32_t>(externals_.size()));
        for (const Section* external : externals_) {
            string(out, external->field_string("type"));
            string(out, external->field_string("path"));
            out.u64(static_cast<std::uint64_t>(uid_id(external->field_string("uid"))));
        }
        out.u32(static_cast<std::uint32_t>(saved_.size() + 1));
        std::vector<std::size_t> offsets;
        for (const Resource& saved : saved_) {
            string(out, "local://" + saved.id);
            offsets.push_back(out.size());
            out.u64(0);
        }
        // A packed scene is saved new, so it is built in; a resource keeps its path.
        string(out, scene ? "local://PackedScene_" + md5_hex(options.path).substr(0, 5)
                          : res_path(options.path));
        offsets.push_back(out.size());
        out.u64(0);

        // Offsets count from the file's start, "RSRC" included, or from the start of what a
        // compressed file holds.
        const std::size_t base = options.compress ? 0 : 4;
        const auto write_one = [&](const std::string& type,
                                   const std::vector<std::pair<std::string, Value>>& properties,
                                   std::size_t offset) -> Result<void> {
            out.patch_u64(offset, out.size() + base);
            string(out, type);
            out.u32(static_cast<std::uint32_t>(properties.size()));
            for (const auto& [name, value] : properties) {
                out.u32(string_index(name));
                if (auto done = variant(out, value, 0); !done) {
                    return make_error(ErrorCode::InvalidArgument,
                                      "property \"" + name + "\": " + done.error().message);
                }
            }
            return {};
        };
        for (std::size_t i = 0; i < saved_.size(); ++i) {
            if (auto done = write_one(saved_[i].type, *saved_[i].properties, offsets[i]); !done) {
                return make_error(ErrorCode::InvalidArgument,
                                  "sub-resource \"" + saved_[i].id + "\": " + done.error().message);
            }
        }
        if (auto done = write_one(main_type, main_properties, offsets.back()); !done) {
            return done.error();
        }
        out.bytes("RSRC");
        const std::string body = out.take();
        if (!options.compress) {
            return "RSRC" + body;
        }
        return compressed(body);
    }

private:
    struct Resource {
        std::string id;
        std::string type;
        const std::vector<std::pair<std::string, Value>>* properties = nullptr;
    };

    // FileAccessCompressed: the block size, the total, each block's packed size, then the blocks.
    static std::string compressed(std::string_view body) {
        std::vector<std::string> blocks;
        for (std::size_t at = 0; at <= body.size(); at += kCompressedBlockSize) {
            blocks.push_back(zstd_store(body.substr(at, kCompressedBlockSize)));
        }
        ByteWriter out;
        out.bytes("RSCC");
        out.u32(kCompressionZstd);
        out.u32(kCompressedBlockSize);
        out.u32(static_cast<std::uint32_t>(body.size()));
        for (const std::string& block : blocks) {
            out.u32(static_cast<std::uint32_t>(block.size()));
        }
        for (const std::string& block : blocks) {
            out.bytes(block);
        }
        return out.take();
    }

    // save_unicode_string: the size with its terminating zero, the text and the zero.
    static void string(ByteWriter& out, std::string_view text) {
        out.u32(static_cast<std::uint32_t>(text.size() + 1));
        out.bytes(text);
        out.u8(0);
    }

    std::uint32_t string_index(const std::string& text) {
        const auto [it, added] =
            string_ids_.try_emplace(text, static_cast<std::uint32_t>(strings_.size()));
        if (added) {
            strings_.push_back(text);
        }
        return it->second;
    }

    Result<void> find(const Value& value, int depth) {
        if (depth > kMaxDepth) {
            return value_error("values nest too deeply");
        }
        if (value.kind == Value::Kind::Array) {
            for (const Value& item : value.items) {
                if (auto found = find(item, depth + 1); !found) {
                    return found;
                }
            }
        } else if (value.kind == Value::Kind::Dictionary) {
            for (const Value::Entry& entry : value.entries) {
                if (auto found = find(entry.key, depth + 1); !found) {
                    return found;
                }
                if (auto found = find(entry.value, depth + 1); !found) {
                    return found;
                }
            }
        } else if (value.is_call("ExtResource")) {
            const std::string id = value.string_argument().value_or("");
            const auto it = externals_by_id_.find(id);
            if (it == externals_by_id_.end()) {
                return value_error("ExtResource(\"" + id + "\") names no [ext_resource]");
            }
            if (external_ids_.try_emplace(id, static_cast<std::uint32_t>(externals_.size()))
                    .second) {
                externals_.push_back(it->second);
            }
        } else if (value.is_call("SubResource")) {
            const std::string id = value.string_argument().value_or("");
            const auto it = subs_by_id_.find(id);
            if (it == subs_by_id_.end()) {
                return value_error("SubResource(\"" + id + "\") names no [sub_resource]");
            }
            if (internal_ids_.contains(id) || !visiting_.insert(id).second) {
                return {}; // saved already, or holds itself
            }
            for (const auto& property : it->second->properties) {
                if (auto found = find(property.second, depth + 1); !found) {
                    return found;
                }
            }
            internal_ids_[id] = static_cast<std::uint32_t>(saved_.size());
            saved_.push_back({.id = id,
                              .type = it->second->field_string("type"),
                              .properties = &it->second->properties});
        } else if (value.is_call("NodePath")) {
            const NodePathParts parts = node_path_parts(value.string_argument().value_or(""));
            for (const auto* list : {&parts.names, &parts.subnames}) {
                for (const std::string& name : *list) {
                    static_cast<void>(string_index(name));
                }
            }
        }
        return {};
    }

    Result<void> variant(ByteWriter& out, const Value& value, int depth) {
        if (depth > kMaxDepth) {
            return value_error("values nest too deeply");
        }
        switch (value.kind) {
        case Value::Kind::Nil:
            out.u32(kVariantNil);
            return {};
        case Value::Kind::Bool:
            out.u32(kVariantBool);
            out.u32(value.boolean ? 1 : 0);
            return {};
        case Value::Kind::Int:
            if (fits_int32(value.integer)) {
                out.u32(kVariantInt);
                out.u32(static_cast<std::uint32_t>(value.integer));
            } else {
                out.u32(kVariantInt64);
                out.u64(static_cast<std::uint64_t>(value.integer));
            }
            return {};
        case Value::Kind::Float:
            if (fits_float(value.real)) {
                out.u32(kVariantFloat);
                out.f32(static_cast<float>(value.real));
            } else {
                out.u32(kVariantDouble);
                out.f64(value.real);
            }
            return {};
        case Value::Kind::String:
            out.u32(kVariantString);
            string(out, value.text);
            return {};
        case Value::Kind::StringName:
            out.u32(kVariantStringName);
            string(out, value.text);
            return {};
        case Value::Kind::Array:
            out.u32(kVariantArray);
            out.u32(static_cast<std::uint32_t>(value.items.size()));
            for (const Value& item : value.items) {
                if (auto done = variant(out, item, depth + 1); !done) {
                    return done;
                }
            }
            return {};
        case Value::Kind::Dictionary:
            out.u32(kVariantDictionary);
            out.u32(static_cast<std::uint32_t>(value.entries.size()));
            for (const Value::Entry& entry : value.entries) {
                if (auto done = variant(out, entry.key, depth + 1); !done) {
                    return done;
                }
                if (auto done = variant(out, entry.value, depth + 1); !done) {
                    return done;
                }
            }
            return {};
        case Value::Kind::Call:
            break;
        }
        if (const MathType* type = math_type(value.text); type != nullptr) {
            auto numbers = math_numbers(value, *type);
            if (!numbers) {
                return numbers.error();
            }
            out.u32(type->code);
            write_math(out, *type, numbers.value());
            return {};
        }
        if (const PackedType* type = packed_type(value.text); type != nullptr) {
            out.u32(type->code);
            return write_packed(out, value, *type,
                                [&](std::string_view text) { string(out, text); });
        }
        if (value.text == "NodePath") {
            const NodePathParts parts = node_path_parts(value.string_argument().value_or(""));
            out.u32(kVariantNodePath);
            out.u16(static_cast<std::uint32_t>(parts.names.size()));
            out.u16(static_cast<std::uint32_t>(parts.subnames.size()) |
                    (parts.absolute ? 0x8000U : 0U));
            for (const auto* list : {&parts.names, &parts.subnames}) {
                for (const std::string& name : *list) {
                    out.u32(string_index(name));
                }
            }
            return {};
        }
        if (value.text == "RID") {
            out.u32(kVariantRid);
            out.u32(0);
            return {};
        }
        if (value.text == "ExtResource" || value.text == "SubResource") {
            const std::string id = value.string_argument().value_or("");
            const bool external = value.text == "ExtResource";
            const auto& ids = external ? external_ids_ : internal_ids_;
            const auto it = ids.find(id);
            out.u32(kVariantObject);
            if (it == ids.end()) {
                out.u32(kObjectEmpty); // a sub-resource that holds itself
                return {};
            }
            out.u32(external ? kObjectExternalResourceIndex : kObjectInternalResource);
            out.u32(it->second);
            return {};
        }
        return value_error(value.text + "(...) can't be saved in a binary resource");
    }

    std::map<std::string, const Section*> externals_by_id_;
    std::map<std::string, const Section*> subs_by_id_;
    std::vector<const Section*> externals_;
    std::map<std::string, std::uint32_t> external_ids_;
    std::vector<Resource> saved_;
    std::map<std::string, std::uint32_t> internal_ids_;
    std::set<std::string> visiting_;
    std::vector<std::string> strings_;
    std::map<std::string, std::uint32_t> string_ids_;
};

// --- Imported assets (editor/import) ---

// CompressedTexture2D's header bits as Godot's importer sets them for a 2D texture: detect 3D,
// normal and roughness use, and no mipmap limit.
constexpr std::uint32_t kTextureFlags = 0x0D000000;
constexpr std::uint32_t kTextureVersion = 1;
constexpr std::uint32_t kDataFormatPng = 1;

// Image::Format of a PNG file as Godot's loader decodes it, by its IHDR color type.
std::uint32_t png_image_format(std::string_view png) {
    constexpr std::size_t kColorType = 25;
    if (png.size() <= kColorType) {
        return 5;
    }
    switch (static_cast<std::uint8_t>(png[kColorType])) {
    case 0:
        return 0; // L8
    case 2:
        return 4; // RGB8
    case 3:
        return png.find("tRNS") != std::string_view::npos ? 5 : 4;
    case 4:
        return 1; // LA8
    default:
        return 5; // RGBA8
    }
}

// A PNG file as a CompressedTexture2D (.ctex) stored lossless as PNG, as the importer writes one
// with rendering/textures/lossless_compression/force_png on.
Result<std::string> png_texture(std::string_view png) {
    const auto size = png_size(png);
    if (!size) {
        return value_error("not a PNG file");
    }
    const auto width = static_cast<std::uint32_t>(size->first);
    const auto height = static_cast<std::uint32_t>(size->second);
    ByteWriter out;
    out.bytes("GST2");
    out.u32(kTextureVersion);
    out.u32(width);
    out.u32(height);
    out.u32(kTextureFlags);
    out.u32(0xFFFFFFFFU); // mipmap limit: none
    for (int i = 0; i < 3; ++i) {
        out.u32(0);
    }
    out.u32(kDataFormatPng);
    out.u16(width);
    out.u16(height);
    out.u32(0); // mipmaps
    out.u32(png_image_format(png));
    out.u32(static_cast<std::uint32_t>(png.size() + 4));
    out.bytes("PNG "); // Godot's own prefix
    out.bytes(png);
    return out.take();
}

std::uint32_t le32(std::string_view bytes, std::size_t at) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[at + i])) << (8U * i);
    }
    return value;
}

std::uint32_t le16(std::string_view bytes, std::size_t at) {
    return static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[at])) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[at + 1])) << 8U);
}

Value byte_array(std::string_view bytes) {
    Value out = make_call("PackedByteArray");
    if (!bytes.empty()) {
        out.items.push_back(make_string(base64_encode(bytes)));
    }
    return out;
}

// A binary resource of one type with the given properties and no sub-resources.
Result<std::string> plain_resource(const std::string& type,
                                   std::vector<std::pair<std::string, Value>> properties,
                                   const std::string& path) {
    Section header;
    header.tag = "gd_resource";
    header.fields.emplace_back("type", make_string(type));
    Section resource;
    resource.tag = "resource";
    resource.properties = std::move(properties);
    return write_binary_resource({header, resource}, {.path = path, .compress = false});
}

// A PCM WAV file as an AudioStreamWAV resource (.sample), as the importer writes one with its
// compression off: 8-bit samples signed, 16-bit ones as they are, a "smpl" loop kept.
Result<std::string> wav_sample(std::string_view wav, const std::string& path) {
    if (wav.size() < 12 || wav.substr(0, 4) != "RIFF" || wav.substr(8, 4) != "WAVE") {
        return value_error("not a WAV file");
    }
    std::uint32_t channels = 0;
    std::uint32_t rate = 0;
    std::uint32_t bits = 0;
    std::optional<std::string> data;
    std::int64_t loop_mode = 0;
    std::int64_t loop_begin = 0;
    std::int64_t loop_end = 0;
    for (std::size_t at = 12; at + 8 <= wav.size();) {
        const std::string_view id = wav.substr(at, 4);
        const std::size_t size = le32(wav, at + 4);
        if (at + 8 + size > wav.size()) {
            return value_error("the WAV file is cut short");
        }
        const std::string_view chunk = wav.substr(at + 8, size);
        if (id == "fmt " && size >= 16) {
            if (le16(chunk, 0) != 1) {
                return value_error("only PCM WAV files are exported here");
            }
            channels = le16(chunk, 2);
            rate = le32(chunk, 4);
            bits = le16(chunk, 14);
        } else if (id == "data") {
            data = std::string(chunk);
        } else if (id == "smpl" && size >= 60 && le32(chunk, 28) > 0) {
            loop_mode = static_cast<std::int64_t>(le32(chunk, 40)) + 1;
            loop_begin = le32(chunk, 44);
            loop_end = le32(chunk, 48);
        }
        at += 8 + size + (size % 2);
    }
    if (!data || (bits != 8 && bits != 16) || (channels != 1 && channels != 2)) {
        return value_error("only 8- and 16-bit mono or stereo WAV files are exported here");
    }
    if (bits == 8) {
        for (char& byte : *data) {
            byte = static_cast<char>(static_cast<std::uint8_t>(byte) ^ 0x80U);
        }
    }
    // Properties equal to their defaults are left out, as the saver leaves them.
    std::vector<std::pair<std::string, Value>> properties;
    properties.emplace_back("data", byte_array(*data));
    if (bits == 16) {
        properties.emplace_back("format", make_int(1));
    }
    if (loop_mode != 0) {
        properties.emplace_back("loop_mode", make_int(loop_mode));
        properties.emplace_back("loop_begin", make_int(loop_begin));
        properties.emplace_back("loop_end", make_int(loop_end));
    }
    constexpr std::uint32_t kDefaultRate = 44100;
    if (rate != kDefaultRate) {
        properties.emplace_back("mix_rate", make_int(rate));
    }
    if (channels == 2) {
        Value stereo = make_kind(Value::Kind::Bool);
        stereo.boolean = true;
        properties.emplace_back("stereo", std::move(stereo));
    }
    return plain_resource("AudioStreamWAV", std::move(properties), path);
}

// The .import file an export keeps: only its [remap] section, ending in a zero as Godot's do.
std::string import_file(std::string_view importer, std::string_view type, std::int64_t uid,
                        std::string_view path, bool texture) {
    std::string out = "[remap]\n\nimporter=\"" + std::string(importer) + "\"\ntype=\"" +
                      std::string(type) + "\"\nuid=\"" + uid_text(uid) + "\"\npath=\"res://" +
                      std::string(path) + "\"\n";
    if (texture) {
        out += "metadata={\n\"vram_texture\": false\n}\n";
    }
    out += '\0';
    return out;
}

std::string remap_file(std::string_view target) {
    return "[remap]\n\npath=\"res://" + std::string(target) + "\"\n";
}

// A UID for an asset whose project gives it none, made from its path so exports repeat.
std::int64_t path_uid(std::string_view path) {
    std::uint64_t id = 0;
    for (const std::uint8_t byte : md5_digest(path)) {
        id = (id << 8U) | byte;
    }
    return static_cast<std::int64_t>(id & 0x7FFFFFFFFFFFFFFFU);
}

// Where the importer saves an asset's imported form: its name and its path's MD5.
std::string imported_path(const std::string& path, std::string_view extension) {
    return ".godot/imported/" + path.substr(path.rfind('/') + 1) + "-" + md5_hex("res://" + path) +
           std::string(extension);
}

std::string trimmed(std::string_view text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' ||
                             text.back() == '\0')) {
        text.remove_suffix(1);
    }
    return std::string(text);
}

} // namespace

Result<std::string> write_binary_resource(const std::vector<Section>& sections,
                                          const ResourceOptions& options) {
    return ResourceWriter().write(sections, options);
}

Result<std::string> write_project_binary(const std::vector<Section>& sections) {
    // ProjectSettings::save_custom writes the sections in name order.
    std::vector<const Section*> sorted;
    sorted.reserve(sections.size());
    for (const Section& section : sections) {
        sorted.push_back(&section);
    }
    std::ranges::stable_sort(sorted, {}, &Section::tag);
    ByteWriter body;
    std::uint32_t count = 0;
    for (const Section* section_pointer : sorted) {
        const Section& section = *section_pointer;
        for (const auto& [name, value] : section.properties) {
            if (section.tag.empty() && name == "config_version") {
                continue; // the text file's own, not a setting
            }
            const std::string key = section.tag.empty() ? name : section.tag + "/" + name;
            auto encoded = encode_variant(value);
            if (!encoded) {
                return make_error(ErrorCode::InvalidArgument,
                                  "setting \"" + key + "\": " + encoded.error().message);
            }
            body.u32(static_cast<std::uint32_t>(key.size()));
            body.bytes(key);
            body.u32(static_cast<std::uint32_t>(encoded.value().size()));
            body.bytes(encoded.value());
            ++count;
        }
    }
    ByteWriter out;
    out.bytes("ECFG");
    out.u32(count);
    out.bytes(body.take());
    return out.take();
}

std::string write_pack(const std::map<std::string, std::string>& files,
                       const PackOptions& options) {
    constexpr std::uint32_t kRelativeBase = 1U << 1U;
    constexpr std::size_t kPadding = 16;
    const bool directory_first = options.format < 3;
    ByteWriter out;
    out.bytes(options.executable);
    const std::size_t embed_at = out.size();
    if (!options.executable.empty()) {
        out.pad(8);
    }
    const std::size_t start = out.size();
    out.bytes("GDPC");
    out.u32(options.format);
    out.u32(options.major);
    out.u32(options.minor);
    out.u32(options.patch);
    out.u32(kRelativeBase | options.flags);
    const std::size_t file_base_at = out.size();
    out.u64(0);
    std::size_t directory_at = 0;
    if (!directory_first) {
        directory_at = out.size();
        out.u64(0);
    }
    for (int i = 0; i < 16; ++i) {
        out.u32(0); // reserved
    }

    // Each file's offset from the files' base, every file padded to 16 bytes.
    std::vector<std::uint64_t> offsets;
    std::uint64_t running = 0;
    for (const auto& [path, data] : files) {
        offsets.push_back(running);
        running += data.size();
        running += (kPadding - (running % kPadding)) % kPadding;
    }
    const auto write_directory = [&] {
        out.u32(static_cast<std::uint32_t>(files.size()));
        std::size_t i = 0;
        for (const auto& [path, data] : files) {
            // Godot 4.5 stopped storing "res://" in front of each path.
            const std::string stored = directory_first ? "res://" + path : path;
            const std::size_t padded = stored.size() + ((4 - (stored.size() % 4)) % 4);
            out.u32(static_cast<std::uint32_t>(padded));
            out.bytes(stored);
            out.bytes(std::string(padded - stored.size(), '\0'));
            out.u64(offsets[i++]);
            out.u64(data.size());
            for (const std::uint8_t byte : md5_digest(data)) {
                out.u8(byte);
            }
            const auto flags = options.file_flags.find(path);
            out.u32(flags != options.file_flags.end() ? flags->second : 0);
        }
    };
    if (directory_first) {
        write_directory();
    }
    out.pad(kPadding);
    const std::size_t file_base = out.size();
    out.patch_u64(file_base_at, file_base - start);
    for (const auto& [path, data] : files) {
        out.bytes(data);
        out.bytes(std::string((kPadding - ((out.size() - file_base) % kPadding)) % kPadding, '\0'));
    }
    if (!directory_first) {
        out.pad(kPadding);
        out.patch_u64(directory_at, out.size() - start);
        write_directory();
    }
    if (!options.executable.empty()) {
        // The pack's end, with its size and the magic after it, lands on a multiple of 8.
        while ((out.size() - embed_at + 12) % 8 != 0) {
            out.u8(0);
        }
        out.u64(out.size() - start);
        out.bytes("GDPC");
    }
    return out.take();
}

Result<std::map<std::string, std::string>>
export_project(const std::map<std::string, std::string>& project, const ExportOptions& options) {
    std::map<std::string, std::string> pack;
    std::map<std::string, std::int64_t> uids; // by res:// path
    const auto fail = [](const std::string& path, const Error& error) {
        return make_error(ErrorCode::InvalidArgument, path + ": " + error.message);
    };
    // The UID the project's .import file gives an asset, or one made from its path.
    const auto asset_uid = [&](const std::string& path) {
        if (const auto it = project.find(path + ".import"); it != project.end()) {
            if (auto sections = parse_project_settings(it->second); sections) {
                for (const Section& section : sections.value()) {
                    if (section.tag == "remap") {
                        if (const std::int64_t id = uid_id(section.field_string("uid")); id >= 0) {
                            return id;
                        }
                    }
                }
            }
        }
        return path_uid("res://" + path);
    };
    const auto import_asset = [&](const std::string& path, std::string data,
                                  std::string_view extension, std::string_view importer,
                                  std::string_view type) {
        const std::string target = imported_path(path, extension);
        const std::int64_t uid = asset_uid(path);
        uids["res://" + path] = uid;
        pack[target] = std::move(data);
        pack[path + ".import"] = import_file(importer, type, uid, target, extension == ".ctex");
    };

    for (const auto& [path, contents] : project) {
        const std::string extension = extension_of(path);
        if (path.starts_with(".godot/") || extension == ".import") {
            continue;
        }
        if (extension == ".uid") {
            if (const std::int64_t id = uid_id(trimmed(contents)); id >= 0) {
                uids["res://" + path.substr(0, path.size() - 4)] = id;
            }
            continue;
        }
        if (path == "project.godot") {
            auto sections = parse_project_settings(contents);
            if (!sections) {
                return fail(path, sections.error());
            }
            auto binary = write_project_binary(sections.value());
            if (!binary) {
                return fail(path, binary.error());
            }
            pack["project.binary"] = std::move(binary.value());
        } else if (extension == ".tscn" || extension == ".tres") {
            auto sections = parse_resource(contents);
            if (!sections) {
                return fail(path, sections.error());
            }
            for (Section& section : sections.value()) {
                if (section.tag == "gd_scene" || section.tag == "gd_resource") {
                    if (const std::int64_t id = uid_id(section.field_string("uid")); id >= 0) {
                        uids["res://" + path] = id;
                    }
                    // Saved under the exported path, which has no UID: the cache keeps it.
                    std::erase_if(section.fields,
                                  [](const auto& field) { return field.first == "uid"; });
                }
            }
            if (!options.binary_resources) {
                pack[path] = contents;
                continue;
            }
            auto binary = write_binary_resource(sections.value(), {.path = "res://" + path});
            if (!binary) {
                return fail(path, binary.error());
            }
            // Godot names the folder by a hash of its export plugins; any name does.
            const std::string target = ".godot/exported/0/export-" + md5_hex("res://" + path) +
                                       "-" + file_stem(path) +
                                       (extension == ".tscn" ? ".scn" : ".res");
            pack[target] = std::move(binary.value());
            pack[path + ".remap"] = remap_file(target);
        } else if (extension == ".gd" && options.script_tokens) {
            auto tokens = tokenize_gdscript(contents, options.tokens);
            if (!tokens) {
                return fail(path, tokens.error());
            }
            const std::string target = without_extension(path) + ".gdc";
            pack[target] = std::move(tokens.value());
            pack[path + ".remap"] = remap_file(target);
        } else if (extension == ".png") {
            auto texture = png_texture(contents);
            if (!texture) {
                return fail(path, texture.error());
            }
            import_asset(path, std::move(texture.value()), ".ctex", "texture",
                         "CompressedTexture2D");
        } else if (extension == ".wav") {
            auto sample = wav_sample(contents, imported_path(path, ".sample"));
            if (!sample) {
                return fail(path, sample.error());
            }
            import_asset(path, std::move(sample.value()), ".sample", "wav", "AudioStreamWAV");
        } else if (extension == ".mp3" || extension == ".ttf" || extension == ".otf") {
            const bool sound = extension == ".mp3";
            const std::string type = sound ? "AudioStreamMP3" : "FontFile";
            const std::string_view stored = sound ? ".mp3str" : ".fontdata";
            auto resource =
                plain_resource(type, {{"data", byte_array(contents)}}, imported_path(path, stored));
            if (!resource) {
                return fail(path, resource.error());
            }
            import_asset(path, std::move(resource.value()), stored,
                         sound ? "mp3" : "font_data_dynamic", type);
        } else {
            pack[path] = contents;
        }
    }
    if (!uids.empty()) {
        // ResourceUID::save_to_cache.
        ByteWriter cache;
        cache.u32(static_cast<std::uint32_t>(uids.size()));
        for (const auto& [path, id] : uids) {
            cache.u64(static_cast<std::uint64_t>(id));
            cache.u32(static_cast<std::uint32_t>(path.size()));
            cache.bytes(path);
        }
        pack[".godot/uid_cache.bin"] = cache.take();
    }
    return pack;
}

} // namespace slash::importer::godot
