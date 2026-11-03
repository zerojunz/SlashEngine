// Plugin components: C structs the engine keeps in storages of fixed-size blocks, one storage per
// type named by its hash, and saves, loads and describes field by field from the offsets and
// types the plugin gave.

#include "internal.hpp"

#include "slash/core/assert.hpp"
#include "slash/world/serialize.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <string_view>
#include <tuple>
#include <utility>

namespace slash::plugin::detail {
namespace {

struct Layouts {
    std::mutex mutex;
    std::deque<Layout> all; // a layout's id is its index + 1
};

Layouts& layouts() {
    static Layouts instance;
    return instance;
}

// Calls body with Blob<N> for a size class.
template <class Body>
decltype(auto) with_blob(std::size_t size_class, Body&& body) {
    switch (size_class) {
    case 0:
        return body(Blob<kBlobSizes[0]>{});
    case 1:
        return body(Blob<kBlobSizes[1]>{});
    case 2:
        return body(Blob<kBlobSizes[2]>{});
    case 3:
        return body(Blob<kBlobSizes[3]>{});
    case 4:
        return body(Blob<kBlobSizes[4]>{});
    case 5:
        return body(Blob<kBlobSizes[5]>{});
    case 6:
        return body(Blob<kBlobSizes[6]>{});
    case 7:
        return body(Blob<kBlobSizes[7]>{});
    default:
        return body(Blob<kBlobSizes[8]>{});
    }
}

const Layout& layout_of(entt::id_type hash) {
    const Layout* layout = layout_by_hash(hash);
    SLASH_ASSERT(layout != nullptr, "a plugin component's storage has no layout");
    return *layout;
}

std::size_t vector_count(SlashFieldType type) {
    switch (type) {
    case SLASH_FIELD_VEC2:
        return 2;
    case SLASH_FIELD_VEC3:
        return 3;
    case SLASH_FIELD_VEC4:
        return 4;
    default:
        return 0;
    }
}

template <class T>
T read(const std::byte* bytes) {
    T value{};
    std::memcpy(&value, bytes, sizeof(T));
    return value;
}

template <class T>
void write(std::byte* bytes, T value) {
    std::memcpy(bytes, &value, sizeof(T));
}

// A field as JSON; an entity field needs the world its entity is in.
json::Value field_json(const world::World* world, const Field& field, const std::byte* bytes) {
    const std::byte* at = bytes + field.offset;
    switch (field.type) {
    case SLASH_FIELD_BOOL:
        return {read<std::uint8_t>(at) != 0};
    case SLASH_FIELD_INT32:
        return {static_cast<std::int64_t>(read<std::int32_t>(at))};
    case SLASH_FIELD_INT64:
        return {read<std::int64_t>(at)};
    case SLASH_FIELD_FLOAT:
        return {static_cast<double>(read<float>(at))};
    case SLASH_FIELD_DOUBLE:
        return {read<double>(at)};
    case SLASH_FIELD_VEC2:
    case SLASH_FIELD_VEC3:
    case SLASH_FIELD_VEC4: {
        json::Array items;
        for (std::size_t i = 0; i < vector_count(field.type); ++i) {
            items.emplace_back(static_cast<double>(read<float>(at + (i * sizeof(float)))));
        }
        return {std::move(items)};
    }
    case SLASH_FIELD_STRING: {
        const auto* chars = reinterpret_cast<const char*>(at);
        return {std::string(chars, strnlen(chars, field.size))};
    }
    case SLASH_FIELD_ENTITY: {
        const auto entity = static_cast<entt::entity>(read<SlashEntity>(at));
        const std::string_view id =
            world == nullptr || entity == entt::null || !world->valid(entity)
                ? std::string_view()
                : world->id_of(entity);
        return id.empty() ? json::Value() : json::Value(id);
    }
    }
    return {};
}

std::string type_text(SlashFieldType type) {
    switch (type) {
    case SLASH_FIELD_BOOL:
        return "a boolean";
    case SLASH_FIELD_INT32:
    case SLASH_FIELD_INT64:
        return "an integer";
    case SLASH_FIELD_FLOAT:
    case SLASH_FIELD_DOUBLE:
        return "a number";
    case SLASH_FIELD_VEC2:
        return "an array of 2 numbers";
    case SLASH_FIELD_VEC3:
        return "an array of 3 numbers";
    case SLASH_FIELD_VEC4:
        return "an array of 4 numbers";
    case SLASH_FIELD_STRING:
        return "a string";
    case SLASH_FIELD_ENTITY:
        return "an entity id or null";
    }
    return "a value";
}

Error type_error(const Field& field) {
    return make_error(ErrorCode::InvalidArgument, "expected " + type_text(field.type));
}

Result<void> in_range(const Field& field, double value) {
    if (field.min && value < *field.min) {
        return make_error(ErrorCode::InvalidArgument,
                          "must be at least " + json::write(json::Value(*field.min)));
    }
    if (field.max && value > *field.max) {
        return make_error(ErrorCode::InvalidArgument,
                          "must be at most " + json::write(json::Value(*field.max)));
    }
    return {};
}

// Reads a field's JSON into bytes; path is extended into an array's bad item.
Result<void> read_field(world::World& world, const Field& field, const json::Value& value,
                        std::byte* bytes, std::string& path) {
    std::byte* at = bytes + field.offset;
    switch (field.type) {
    case SLASH_FIELD_BOOL:
        if (!value.is_bool()) {
            return type_error(field);
        }
        write<std::uint8_t>(at, value.as_bool() ? 1 : 0);
        return {};
    case SLASH_FIELD_INT32:
    case SLASH_FIELD_INT64: {
        if (!value.is_number() || std::floor(value.as_double()) != value.as_double()) {
            return type_error(field);
        }
        const double number = value.as_double();
        const bool narrow = field.type == SLASH_FIELD_INT32;
        const double low = narrow ? std::numeric_limits<std::int32_t>::min() : -9.2e18;
        const double high = narrow ? std::numeric_limits<std::int32_t>::max() : 9.2e18;
        if (number < low || number > high) {
            return make_error(ErrorCode::InvalidArgument, "out of range for its integer type");
        }
        if (auto ranged = in_range(field, number); !ranged) {
            return ranged;
        }
        if (narrow) {
            write<std::int32_t>(at, static_cast<std::int32_t>(value.as_int()));
        } else {
            write<std::int64_t>(at, value.as_int());
        }
        return {};
    }
    case SLASH_FIELD_FLOAT:
    case SLASH_FIELD_DOUBLE:
        if (!value.is_number()) {
            return type_error(field);
        }
        if (auto ranged = in_range(field, value.as_double()); !ranged) {
            return ranged;
        }
        if (field.type == SLASH_FIELD_FLOAT) {
            write<float>(at, static_cast<float>(value.as_double()));
        } else {
            write<double>(at, value.as_double());
        }
        return {};
    case SLASH_FIELD_VEC2:
    case SLASH_FIELD_VEC3:
    case SLASH_FIELD_VEC4: {
        const std::size_t count = vector_count(field.type);
        if (!value.is_array() || value.size() != count) {
            return type_error(field);
        }
        for (std::size_t i = 0; i < count; ++i) {
            const json::Value& item = value.as_array()[i];
            if (!item.is_number()) {
                path += "/" + std::to_string(i);
                return type_error(field);
            }
            if (auto ranged = in_range(field, item.as_double()); !ranged) {
                path += "/" + std::to_string(i);
                return ranged;
            }
        }
        for (std::size_t i = 0; i < count; ++i) {
            write<float>(at + (i * sizeof(float)),
                         static_cast<float>(value.as_array()[i].as_double()));
        }
        return {};
    }
    case SLASH_FIELD_STRING: {
        if (!value.is_string()) {
            return type_error(field);
        }
        const std::string& text = value.as_string();
        if (text.size() >= field.size || text.find('\0') != std::string::npos) {
            return make_error(ErrorCode::InvalidArgument,
                              "must be at most " + std::to_string(field.size - 1) + " bytes");
        }
        std::memset(at, 0, field.size);
        std::memcpy(at, text.data(), text.size());
        return {};
    }
    case SLASH_FIELD_ENTITY: {
        if (value.is_null()) {
            write<SlashEntity>(at, SLASH_NO_ENTITY);
            return {};
        }
        if (!value.is_string()) {
            return type_error(field);
        }
        const entt::entity target = world.find(value.as_string());
        if (target == entt::null) {
            return make_error(ErrorCode::NotFound,
                              "no entity has the id \"" + value.as_string() + "\"");
        }
        write<SlashEntity>(at, static_cast<SlashEntity>(entt::to_integral(target)));
        return {};
    }
    }
    return type_error(field);
}

json::Value field_schema(const Field& field) {
    json::Value schema;
    switch (field.type) {
    case SLASH_FIELD_BOOL:
        schema["type"] = "boolean";
        break;
    case SLASH_FIELD_INT32:
    case SLASH_FIELD_INT64:
        schema["type"] = "integer";
        break;
    case SLASH_FIELD_FLOAT:
    case SLASH_FIELD_DOUBLE:
        schema["type"] = "number";
        break;
    case SLASH_FIELD_VEC2:
    case SLASH_FIELD_VEC3:
    case SLASH_FIELD_VEC4: {
        json::Value item;
        item["type"] = "number";
        schema["type"] = "array";
        schema["items"] = std::move(item);
        schema["minItems"] = vector_count(field.type);
        schema["maxItems"] = vector_count(field.type);
        break;
    }
    case SLASH_FIELD_STRING:
        schema["type"] = "string";
        schema["maxLength"] = field.size - 1;
        break;
    case SLASH_FIELD_ENTITY:
        schema["type"] = json::Array{"string", "null"};
        schema["description"] = "ID of another entity in the scene, or null for none";
        break;
    }
    return schema;
}

template <class B>
const B* find_blob(const entt::registry& registry, entt::id_type hash, entt::entity entity) {
    const auto* storage = registry.storage<B>(hash);
    return storage != nullptr && storage->contains(entity) ? &storage->get(entity) : nullptr;
}

template <class B>
world::ComponentType::Ops blob_ops() {
    return {
        .has = [](const entt::registry& registry, entt::id_type hash,
                  entt::entity entity) { return find_blob<B>(registry, hash, entity) != nullptr; },
        .remove = [](entt::registry& registry, entt::id_type hash,
                     entt::entity entity) { registry.storage<B>(hash).remove(entity); },
        .save = [](const world::World& world, entt::id_type hash,
                   entt::entity entity) -> json::Value {
            const Layout& layout = layout_of(hash);
            const B* blob = find_blob<B>(world.ecs(), hash, entity);
            json::Object fields;
            for (const Field& field : layout.fields) {
                fields.emplace(field.name, field_json(&world, field, blob->bytes.data()));
            }
            return {std::move(fields)};
        },
        .assign = [](world::World& world, entt::id_type hash, entt::entity entity,
                     const json::Value& values, std::string& path) -> Result<void> {
            const Layout& layout = layout_of(hash);
            if (!values.is_object()) {
                return make_error(ErrorCode::InvalidArgument, "must be an object");
            }
            auto& storage = world.ecs().storage<B>(hash);
            // Into a copy, so a bad field changes nothing.
            B blob{};
            if (storage.contains(entity)) {
                blob = storage.get(entity);
            } else {
                std::memcpy(blob.bytes.data(), layout.defaults.data(), layout.size);
            }
            for (const auto& [name, value] : values.as_object()) {
                const auto field = std::ranges::find(layout.fields, name, &Field::name);
                if (field == layout.fields.end()) {
                    path += "/" + json::pointer_escape(name);
                    return make_error(ErrorCode::InvalidArgument, "unknown field \"" + name + "\"");
                }
                const std::size_t length = path.size();
                path += "/" + json::pointer_escape(name);
                if (auto read = read_field(world, *field, value, blob.bytes.data(), path); !read) {
                    return read;
                }
                path.resize(length);
            }
            if (storage.contains(entity)) {
                storage.get(entity) = blob;
            } else {
                storage.emplace(entity, blob);
            }
            return {};
        },
        .schema = [](world::SchemaBuilder& builder, entt::id_type hash) -> json::Value {
            const Layout& layout = layout_of(hash);
            json::Object properties;
            for (const Field& field : layout.fields) {
                properties.emplace(field.name, field_schema(field));
            }
            json::Value definition;
            definition["type"] = "object";
            definition["additionalProperties"] = false;
            definition["properties"] = std::move(properties);
            return builder.define(layout.name, std::move(definition));
        },
        // Entity fields hold IDs in scene files, which instanced scenes rename.
        .rename =
            [](entt::id_type hash, json::Value& fields, const world::EntityRename& rename) {
                if (!fields.is_object()) {
                    return;
                }
                for (const Field& field : layout_of(hash).fields) {
                    json::Value* value = fields.find(field.name);
                    if (field.type == SLASH_FIELD_ENTITY && value != nullptr &&
                        value->is_string()) {
                        *value = rename(value->as_string());
                    }
                }
            },
        .assure = [](entt::registry& registry,
                     entt::id_type hash) { static_cast<void>(registry.storage<B>(hash)); },
        .bytes = [](const entt::registry& registry, entt::id_type hash) -> std::size_t {
            const auto* storage = registry.storage<B>(hash);
            return storage != nullptr ? storage->capacity() * sizeof(B) : 0;
        },
    };
}

// Whether two layouts store and describe their structs alike.
bool same_shape(const Layout& a, const Layout& b) {
    const auto field = [](const Field& f) {
        return std::tie(f.name, f.type, f.offset, f.size, f.doc, f.min, f.max);
    };
    return a.size == b.size && a.defaults == b.defaults &&
           std::ranges::equal(a.fields, b.fields, {}, field, field);
}

} // namespace

const Layout* add_layout(Layout layout) {
    Layouts& all = layouts();
    const std::scoped_lock lock(all.mutex);
    for (const Layout& existing : all.all) {
        if (existing.name == layout.name) {
            // A plugin loaded again, for another registry (each session, test or tool makes its
            // own), keeps its type's ID; a type of that name stored otherwise clashes.
            return same_shape(existing, layout) ? &existing : nullptr;
        }
    }
    layout.id = static_cast<SlashComponent>(all.all.size() + 1);
    layout.hash = entt::hashed_string::value(layout.name.data(), layout.name.size());
    layout.size_class = 0;
    while (kBlobSizes.at(layout.size_class) < layout.size) {
        ++layout.size_class;
    }
    all.all.push_back(std::move(layout));
    return &all.all.back();
}

const Layout* layout_by_id(SlashComponent id) {
    Layouts& all = layouts();
    const std::scoped_lock lock(all.mutex);
    return id >= 1 && id <= all.all.size() ? &all.all[id - 1] : nullptr;
}

const Layout* layout_by_hash(entt::id_type hash) {
    Layouts& all = layouts();
    const std::scoped_lock lock(all.mutex);
    const auto found = std::ranges::find(all.all, hash, &Layout::hash);
    return found != all.all.end() ? &*found : nullptr;
}

const Layout* layout_by_name(std::string_view name) {
    Layouts& all = layouts();
    const std::scoped_lock lock(all.mutex);
    const auto found = std::ranges::find(all.all, name, &Layout::name);
    return found != all.all.end() ? &*found : nullptr;
}

std::unique_ptr<world::ComponentType> component_type(const Layout& layout) {
    const world::ComponentType::Ops ops =
        with_blob(layout.size_class, []<class B>(B /*unused*/) { return blob_ops<B>(); });
    std::vector<std::string> names;
    json::Object defaults;
    // The default struct's fields; entity fields default to none.
    for (const Field& field : layout.fields) {
        names.push_back(field.name);
        defaults.emplace(field.name, field_json(nullptr, field, layout.defaults.data()));
    }
    auto type = std::make_unique<world::ComponentType>(layout.name, layout.hash, std::move(names),
                                                       json::Value(std::move(defaults)), ops);
    for (const Field& field : layout.fields) {
        type->describe(field.name, {.doc = field.doc, .min = field.min, .max = field.max});
    }
    return type;
}

void* get(world::World& world, const Layout& layout, entt::entity entity) {
    if (!world.valid(entity)) {
        return nullptr;
    }
    return with_blob(layout.size_class, [&]<class B>(B /*unused*/) -> void* {
        // Looked up, not made: an entity without the component asks no storage of it.
        auto* storage = static_cast<entt::storage_for_t<B>*>(world.ecs().storage(layout.hash));
        return storage != nullptr && storage->contains(entity) ? storage->get(entity).bytes.data()
                                                               : nullptr;
    });
}

void* add(world::World& world, const Layout& layout, entt::entity entity) {
    if (!world.valid(entity)) {
        return nullptr;
    }
    return with_blob(layout.size_class, [&]<class B>(B /*unused*/) -> void* {
        auto& storage = world.ecs().storage<B>(layout.hash);
        if (!storage.contains(entity)) {
            B blob{};
            std::memcpy(blob.bytes.data(), layout.defaults.data(), layout.size);
            storage.emplace(entity, blob);
        }
        return storage.get(entity).bytes.data();
    });
}

bool remove(world::World& world, const Layout& layout, entt::entity entity) {
    if (!world.valid(entity)) {
        return false;
    }
    return with_blob(layout.size_class, [&]<class B>(B /*unused*/) {
        return static_cast<bool>(world.ecs().storage<B>(layout.hash).remove(entity));
    });
}

entt::sparse_set& storage(world::World& world, const Layout& layout) {
    return with_blob(layout.size_class, [&]<class B>(B /*unused*/) -> entt::sparse_set& {
        return world.ecs().storage<B>(layout.hash);
    });
}

} // namespace slash::plugin::detail
