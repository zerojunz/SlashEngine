#pragma once

#include "slash/core/result.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Unity's text serialization: the YAML subset its scenes, prefabs, assets, project settings and
// .meta files are written in (block mappings and sequences, flow mappings and sequences, plain
// and quoted scalars, lines wrapped at 80 columns), and the objects of a serialized file, each a
// document under a "--- !u!<class ID> &<file ID>" line.
namespace slash::importer::unity {

// One value: a scalar's text, a mapping's entries in file order or a sequence's items. Unity
// writes numbers, booleans (0 and 1) and strings alike as scalars, so they are read as asked.
struct Node {
    enum class Kind : std::uint8_t { Scalar, Map, Seq };

    struct Entry;

    Kind kind = Kind::Scalar;
    std::string text;
    std::vector<Entry> entries;
    std::vector<Node> items;
    int line = 0; // 1-based line it starts on

    [[nodiscard]] bool is_map() const noexcept { return kind == Kind::Map; }
    [[nodiscard]] bool is_seq() const noexcept { return kind == Kind::Seq; }
    [[nodiscard]] bool is_scalar() const noexcept { return kind == Kind::Scalar; }
    // A mapping's value by key, or nullptr.
    [[nodiscard]] const Node* get(std::string_view key) const;
    [[nodiscard]] Node* get(std::string_view key);
    // A value by a dotted path of keys, such as "m_Size.x" or "m_Curve.0.value" (a number indexes
    // a sequence), or nullptr.
    [[nodiscard]] const Node* path(std::string_view dotted) const;
    // A scalar as a number, or nullopt when it isn't one.
    [[nodiscard]] std::optional<double> number() const;
    [[nodiscard]] std::optional<std::int64_t> integer() const;
    // The scalar's text, or "" for a mapping or sequence.
    [[nodiscard]] std::string_view string() const;

    // A value's number at a key, or fallback.
    [[nodiscard]] double number_at(std::string_view dotted, double fallback = 0.0) const;
    [[nodiscard]] std::int64_t integer_at(std::string_view dotted, std::int64_t fallback = 0) const;
    // Whether the value at a key is 1, as Unity writes true.
    [[nodiscard]] bool flag_at(std::string_view dotted, bool fallback = false) const;
    [[nodiscard]] std::string string_at(std::string_view dotted) const;
};

struct Node::Entry {
    std::string key;
    Node value;
};

// A reference to an object, as {fileID: 1, guid: 0123..., type: 3}: an object of the same file
// when guid is empty, else one of the asset with that GUID.
struct ObjectRef {
    std::int64_t file_id = 0;
    std::string guid;
    int type = 0;

    [[nodiscard]] bool null() const noexcept { return file_id == 0; }
};

// A reference written as a node, or nullopt when the node isn't one.
[[nodiscard]] std::optional<ObjectRef> object_ref(const Node& node);
[[nodiscard]] std::optional<ObjectRef> object_ref_at(const Node& node, std::string_view dotted);

// One object of a serialized file: the document under "--- !u!<class_id> &<file_id>". Objects
// marked "stripped" stand for an object of an instanced prefab and hold only what refers to it.
struct Object {
    int class_id = 0;
    std::int64_t file_id = 0;
    bool stripped = false;
    std::string type; // the document's one key, such as "GameObject"
    Node body;        // its value
    int line = 0;     // 1-based line of the "---" line
};

// Parses a scene, prefab, asset or project settings file. Errors name the line.
[[nodiscard]] Result<std::vector<Object>> parse_file(std::string_view text);

// Parses a file of one plain YAML document, such as a .meta file.
[[nodiscard]] Result<Node> parse_document(std::string_view text);

} // namespace slash::importer::unity
