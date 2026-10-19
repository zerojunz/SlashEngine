#pragma once

#include "slash/core/assert.hpp"
#include "slash/core/json.hpp"
#include "slash/core/math.hpp"
#include "slash/core/reflect.hpp"
#include "slash/core/result.hpp"
#include "slash/world/world.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <entt/core/type_info.hpp>
#include <entt/entity/entity.hpp>
#include <functional>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

// Conversions between reflected C++ values and JSON, plus the JSON Schema of each type.
//
// Supported field types: bool, integers, float, double, std::string, Vec2, Vec3, Vec4 and Quat
// (written as arrays of numbers), Vec2i (an array of two integers), entt::entity (written as the
// target's ID, or null), std::vector of a supported type, std::map from std::string to a supported
// type (written as an object, and replaced whole when read), json::Value (any JSON, as written),
// and structs declared with SLASH_REFLECT or SLASH_COMPONENT (written as objects).
namespace slash::world {

namespace detail {

template <typename T>
struct IsVector : std::false_type {};
template <typename T, typename Allocator>
struct IsVector<std::vector<T, Allocator>> : std::true_type {};

template <typename T>
struct IsStringMap : std::false_type {};
template <typename T, typename Compare, typename Allocator>
struct IsStringMap<std::map<std::string, T, Compare, Allocator>> : std::true_type {};

// Math types are written as fixed-length arrays of numbers, e.g. a Vec3 as [x, y, z].
template <typename T>
struct FloatArray;
template <>
struct FloatArray<Vec2> {
    static constexpr std::array<float Vec2::*, 2> kMembers{&Vec2::x, &Vec2::y};
};
template <>
struct FloatArray<Vec3> {
    static constexpr std::array<float Vec3::*, 3> kMembers{&Vec3::x, &Vec3::y, &Vec3::z};
};
template <>
struct FloatArray<Vec4> {
    static constexpr std::array<float Vec4::*, 4> kMembers{&Vec4::x, &Vec4::y, &Vec4::z, &Vec4::w};
};
template <>
struct FloatArray<Quat> {
    static constexpr std::array<float Quat::*, 4> kMembers{&Quat::x, &Quat::y, &Quat::z, &Quat::w};
};

// A default value of T, to compare fields against.
template <typename T>
const T& defaults_of() {
    static const T kValue{};
    return kValue;
}

template <typename T>
concept FloatArrayType = requires { FloatArray<T>::kMembers; };

// Integer vectors are written as fixed-length arrays of integers, e.g. a Vec2i as [x, y].
template <typename T>
struct IntArray;
template <>
struct IntArray<Vec2i> {
    static constexpr std::array<int Vec2i::*, 2> kMembers{&Vec2i::x, &Vec2i::y};
};

template <typename T>
concept IntArrayType = requires { IntArray<T>::kMembers; };

template <typename T>
inline constexpr bool kUnsupported = false;

inline Error type_error(std::string_view expected, const json::Value& actual) {
    return make_error(ErrorCode::InvalidArgument, "expected " + std::string(expected) + ", got " +
                                                      std::string(json::to_string(actual.type())));
}

inline void push_path(std::string& path, std::string_view key) {
    path += '/';
    path += json::pointer_escape(key);
}

} // namespace detail

// Collects the "$defs" of a generated schema. Each reflected struct is defined once and
// referenced as {"$ref": "#/$defs/<TypeName>"}.
class SchemaBuilder {
public:
    template <reflect::Reflected T>
    json::Value ref();

    // The definition named name; it must already exist.
    [[nodiscard]] json::Value& definition(std::string_view name);
    // Adds a definition no C++ type stands behind, and returns a $ref to it.
    json::Value define(std::string_view name, json::Value schema);

    [[nodiscard]] const json::Object& definitions() const noexcept { return definitions_; }

    [[nodiscard]] static json::Value reference(std::string_view name);

private:
    json::Object definitions_;
    std::map<std::string, entt::id_type, std::less<>> owners_;
};

// Converts a value to JSON. Entity references are written as the target's ID.
template <typename T>
[[nodiscard]] json::Value to_json(const World& world, const T& value);

// Reads JSON into out, changing only the fields present in an object. On failure the error says
// what was wrong, and path (a JSON Pointer, relative to the value read) is left pointing at the
// offending value. On success path is unchanged.
template <typename T>
[[nodiscard]] Result<void> from_json(World& world, const json::Value& json, T& out,
                                     std::string& path);

// The JSON Schema for a type.
template <typename T>
[[nodiscard]] json::Value schema_of(SchemaBuilder& builder);

// Maps one entity ID to another, as when an instanced scene's IDs become the instance's.
using EntityRename = std::function<std::string(const std::string& id)>;

// Renames the entity references in a value written as JSON for type T, which may leave out
// fields, as saved components do. Values of the wrong shape are left alone.
template <typename T>
void rename_entities(json::Value& json, const EntityRename& rename);

template <typename T>
json::Value to_json(const World& world, const T& value) {
    if constexpr (std::is_same_v<T, bool> || std::is_same_v<T, std::string> ||
                  std::is_integral_v<T> || std::is_same_v<T, float> || std::is_same_v<T, double>) {
        return json::Value(value);
    } else if constexpr (detail::FloatArrayType<T>) {
        json::Array items;
        for (const auto member : detail::FloatArray<T>::kMembers) {
            items.emplace_back(value.*member);
        }
        return items;
    } else if constexpr (detail::IntArrayType<T>) {
        json::Array items;
        for (const auto member : detail::IntArray<T>::kMembers) {
            items.emplace_back(value.*member);
        }
        return items;
    } else if constexpr (std::is_same_v<T, entt::entity>) {
        const std::string_view id = value == entt::null ? std::string_view() : world.id_of(value);
        return id.empty() ? json::Value() : json::Value(id);
    } else if constexpr (detail::IsVector<T>::value) {
        json::Array items;
        items.reserve(value.size());
        for (const auto& item : value) {
            items.push_back(to_json<typename T::value_type>(world, item));
        }
        return items;
    } else if constexpr (detail::IsStringMap<T>::value) {
        json::Object members;
        for (const auto& [key, item] : value) {
            members.emplace(key, to_json<typename T::mapped_type>(world, item));
        }
        return members;
    } else if constexpr (std::is_same_v<T, json::Value>) {
        return value;
    } else if constexpr (reflect::Reflected<T>) {
        // A struct inside a component, such as a menu item, leaves out fields equal to their
        // defaults, as saved components do. Components are written whole: saving leaves theirs
        // out, against ComponentType::defaults.
        json::Object members;
        reflect::for_each_field<T>([&](const auto& field) {
            if constexpr (!reflect::Component<T>) {
                if (field.get(value) == field.get(detail::defaults_of<T>())) {
                    return;
                }
            }
            members.emplace(std::string(field.name), to_json(world, field.get(value)));
        });
        return members;
    } else {
        static_assert(detail::kUnsupported<T>, "this field type cannot be serialized; see "
                                               "slash/world/serialize.hpp for supported types");
    }
}

template <typename T>
Result<void> from_json(World& world, const json::Value& json, T& out, std::string& path) {
    if constexpr (std::is_same_v<T, bool>) {
        if (!json.is_bool()) {
            return detail::type_error("boolean", json);
        }
        out = json.as_bool();
    } else if constexpr (std::is_integral_v<T>) {
        if (!json.is_integral()) {
            return detail::type_error("integer", json);
        }
        // Integral doubles below this bound convert to int64 exactly; larger ones can't fit any
        // integer type the engine reflects.
        constexpr double kLimit = 9.0e18;
        std::int64_t number = 0;
        bool in_range = false;
        if (json.is_int()) {
            number = json.as_int();
            in_range = std::in_range<T>(number);
        } else if (json.as_double() > -kLimit && json.as_double() < kLimit) {
            number = static_cast<std::int64_t>(json.as_double());
            in_range = std::in_range<T>(number);
        }
        if (!in_range) {
            return make_error(ErrorCode::InvalidArgument,
                              "integer " + json::write(json) + " is out of range " +
                                  std::to_string(std::numeric_limits<T>::min()) + " to " +
                                  std::to_string(std::numeric_limits<T>::max()));
        }
        out = static_cast<T>(number);
    } else if constexpr (std::is_same_v<T, float> || std::is_same_v<T, double>) {
        if (!json.is_number()) {
            return detail::type_error("number", json);
        }
        out = static_cast<T>(json.as_double());
    } else if constexpr (std::is_same_v<T, std::string>) {
        if (!json.is_string()) {
            return detail::type_error("string", json);
        }
        out = json.as_string();
    } else if constexpr (detail::FloatArrayType<T>) {
        constexpr std::size_t kCount = detail::FloatArray<T>::kMembers.size();
        if (!json.is_array() || json.size() != kCount) {
            return detail::type_error("array of " + std::to_string(kCount) + " numbers", json);
        }
        T value{};
        for (std::size_t i = 0; i < kCount; ++i) {
            const json::Value& item = json.as_array()[i];
            if (!item.is_number()) {
                detail::push_path(path, std::to_string(i));
                return detail::type_error("number", item);
            }
            value.*detail::FloatArray<T>::kMembers[i] = static_cast<float>(item.as_double());
        }
        out = value;
    } else if constexpr (detail::IntArrayType<T>) {
        constexpr std::size_t kCount = detail::IntArray<T>::kMembers.size();
        if (!json.is_array() || json.size() != kCount) {
            return detail::type_error("array of " + std::to_string(kCount) + " integers", json);
        }
        T value{};
        for (std::size_t i = 0; i < kCount; ++i) {
            const std::size_t length = path.size();
            detail::push_path(path, std::to_string(i));
            if (auto status = from_json(world, json.as_array()[i],
                                        value.*detail::IntArray<T>::kMembers[i], path);
                !status) {
                return status;
            }
            path.resize(length);
        }
        out = value;
    } else if constexpr (std::is_same_v<T, entt::entity>) {
        if (json.is_null()) {
            out = entt::null;
        } else if (json.is_string()) {
            const entt::entity target = world.find(json.as_string());
            if (target == entt::null) {
                return make_error(ErrorCode::NotFound,
                                  "no entity has the id \"" + json.as_string() + "\"");
            }
            out = target;
        } else {
            return detail::type_error("entity id or null", json);
        }
    } else if constexpr (detail::IsVector<T>::value) {
        if (!json.is_array()) {
            return detail::type_error("array", json);
        }
        T items;
        items.reserve(json.size());
        for (std::size_t i = 0; i < json.size(); ++i) {
            typename T::value_type item{};
            const std::size_t length = path.size();
            detail::push_path(path, std::to_string(i));
            if (auto status = from_json(world, json.as_array()[i], item, path); !status) {
                return status;
            }
            path.resize(length);
            items.push_back(std::move(item));
        }
        out = std::move(items);
    } else if constexpr (detail::IsStringMap<T>::value) {
        if (!json.is_object()) {
            return detail::type_error("object", json);
        }
        T items;
        for (const auto& [key, member] : json.as_object()) {
            typename T::mapped_type item{};
            const std::size_t length = path.size();
            detail::push_path(path, key);
            if (auto status = from_json(world, member, item, path); !status) {
                return status;
            }
            path.resize(length);
            items.emplace(key, std::move(item));
        }
        out = std::move(items);
    } else if constexpr (std::is_same_v<T, json::Value>) {
        out = json;
    } else if constexpr (reflect::Reflected<T>) {
        if (!json.is_object()) {
            return detail::type_error("object", json);
        }
        for (const auto& member : json.as_object()) {
            const std::string& key = member.first;
            bool known = false;
            Result<void> status;
            const std::size_t length = path.size();
            reflect::for_each_field<T>([&](const auto& field) {
                if (known || field.name != key) {
                    return;
                }
                known = true;
                detail::push_path(path, key);
                status = from_json(world, member.second, field.get(out), path);
            });
            if (!known) {
                detail::push_path(path, key);
                return make_error(ErrorCode::InvalidArgument, std::string(reflect::type_name<T>()) +
                                                                  " has no field \"" + key + "\"");
            }
            if (!status) {
                return status;
            }
            path.resize(length);
        }
    } else {
        static_assert(detail::kUnsupported<T>, "this field type cannot be serialized; see "
                                               "slash/world/serialize.hpp for supported types");
    }
    return {};
}

template <typename T>
void rename_entities(json::Value& json, const EntityRename& rename) {
    if constexpr (std::is_same_v<T, entt::entity>) {
        if (json.is_string()) {
            json = rename(json.as_string());
        }
    } else if constexpr (detail::IsVector<T>::value) {
        if (json.is_array()) {
            for (json::Value& item : json.as_array()) {
                rename_entities<typename T::value_type>(item, rename);
            }
        }
    } else if constexpr (detail::IsStringMap<T>::value) {
        if (json.is_object()) {
            for (auto& [key, item] : json.as_object()) {
                rename_entities<typename T::mapped_type>(item, rename);
            }
        }
    } else if constexpr (reflect::Reflected<T> && !detail::FloatArrayType<T> &&
                         !detail::IntArrayType<T>) {
        if (json.is_object()) {
            reflect::for_each_field<T>([&](const auto& field) {
                using Member = typename std::remove_cvref_t<decltype(field)>::member_type;
                if (json::Value* member = json.find(field.name); member != nullptr) {
                    rename_entities<Member>(*member, rename);
                }
            });
        }
    }
}

template <typename T>
json::Value schema_of(SchemaBuilder& builder) {
    json::Value schema;
    if constexpr (std::is_same_v<T, bool>) {
        schema["type"] = "boolean";
    } else if constexpr (std::is_integral_v<T>) {
        schema["type"] = "integer";
        if constexpr (sizeof(T) < sizeof(std::int64_t)) {
            schema["minimum"] = std::numeric_limits<T>::min();
            schema["maximum"] = std::numeric_limits<T>::max();
        } else if constexpr (std::is_unsigned_v<T>) {
            schema["minimum"] = 0;
        }
    } else if constexpr (std::is_same_v<T, float> || std::is_same_v<T, double>) {
        schema["type"] = "number";
    } else if constexpr (std::is_same_v<T, std::string>) {
        schema["type"] = "string";
    } else if constexpr (detail::FloatArrayType<T>) {
        constexpr std::size_t kCount = detail::FloatArray<T>::kMembers.size();
        schema["type"] = "array";
        schema["items"]["type"] = "number";
        schema["minItems"] = kCount;
        schema["maxItems"] = kCount;
    } else if constexpr (detail::IntArrayType<T>) {
        constexpr std::size_t kCount = detail::IntArray<T>::kMembers.size();
        schema["type"] = "array";
        schema["items"] = schema_of<int>(builder);
        schema["minItems"] = kCount;
        schema["maxItems"] = kCount;
    } else if constexpr (std::is_same_v<T, entt::entity>) {
        schema["type"] = json::Array{"string", "null"};
        schema["description"] = "ID of another entity in the scene, or null for none";
    } else if constexpr (detail::IsVector<T>::value) {
        schema["type"] = "array";
        schema["items"] = schema_of<typename T::value_type>(builder);
    } else if constexpr (detail::IsStringMap<T>::value) {
        schema["type"] = "object";
        schema["additionalProperties"] = schema_of<typename T::mapped_type>(builder);
    } else if constexpr (std::is_same_v<T, json::Value>) {
        schema = json::Object{}; // any JSON
    } else if constexpr (reflect::Reflected<T>) {
        schema = builder.ref<T>();
    } else {
        static_assert(detail::kUnsupported<T>, "this field type cannot be serialized; see "
                                               "slash/world/serialize.hpp for supported types");
    }
    return schema;
}

template <reflect::Reflected T>
json::Value SchemaBuilder::ref() {
    const std::string_view name = reflect::type_name<T>();
    const entt::id_type owner = entt::type_hash<T>::value();
    if (const auto it = owners_.find(name); it != owners_.end()) {
        SLASH_ASSERT(it->second == owner, "two reflected types have the same name");
        return reference(name);
    }
    owners_.emplace(std::string(name), owner);
    // Reserve the name first so a type that contains itself (through a vector) terminates.
    definitions_[std::string(name)] = json::Object{};
    json::Value properties = json::Object{};
    reflect::for_each_field<T>([&](const auto& field) {
        using Member = typename std::remove_cvref_t<decltype(field)>::member_type;
        properties[field.name] = schema_of<Member>(*this);
    });
    json::Value object_schema;
    object_schema["type"] = "object";
    object_schema["additionalProperties"] = false;
    object_schema["properties"] = std::move(properties);
    definitions_[std::string(name)] = std::move(object_schema);
    return reference(name);
}

} // namespace slash::world
