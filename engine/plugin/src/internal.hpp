#pragma once

#include "slash/plugin/plugins.hpp"
#include "slash/plugin/slash_plugin.h"
#include "slash/world/component_registry.hpp"
#include "slash/world/world.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <entt/entity/registry.hpp>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace slash::plugin::detail {

// A field of a plugin component's struct.
struct Field {
    std::string name;
    SlashFieldType type = SLASH_FIELD_INT32;
    std::uint32_t offset = 0;
    std::uint32_t size = 0; // its bytes in the struct
    std::string doc;
    std::optional<double> min;
    std::optional<double> max;
};

// A plugin component type, as Godot's ClassDB keeps an extension's classes: for the whole
// process, since plugins are never unloaded.
struct Layout {
    std::string name;
    std::string plugin;
    SlashComponent id = 0;
    entt::id_type hash = 0;
    std::uint32_t size = 0;
    std::size_t size_class = 0; // an index into kBlobSizes
    std::vector<Field> fields;
    std::vector<std::byte> defaults; // size bytes
};

// The storage sizes components are kept in: a struct goes in the smallest that holds it.
inline constexpr std::array<std::size_t, 9> kBlobSizes{16, 32, 64, 128, 256, 512, 1024, 2048, 4096};
inline constexpr std::uint32_t kMaxAlign = 16;

// A component's bytes, in a storage of its own named by the type's hash.
template <std::size_t N>
struct alignas(16) Blob {
    std::array<std::byte, N> bytes{};
};

// Registers a layout, setting its id and hash, or finds the one registered alike before (the same
// plugin loaded for another registry). Nullptr if one of that name is stored otherwise.
const Layout* add_layout(Layout layout);
[[nodiscard]] const Layout* layout_by_id(SlashComponent id);
[[nodiscard]] const Layout* layout_by_hash(entt::id_type hash);
[[nodiscard]] const Layout* layout_by_name(std::string_view name);

// The engine's component type for a layout.
[[nodiscard]] std::unique_ptr<world::ComponentType> component_type(const Layout& layout);

// The component's bytes on an entity, or nullptr; and the same, added at its defaults if it
// had none.
[[nodiscard]] void* get(world::World& world, const Layout& layout, entt::entity entity);
[[nodiscard]] void* add(world::World& world, const Layout& layout, entt::entity entity);
bool remove(world::World& world, const Layout& layout, entt::entity entity);
// The storage of the layout's components in a world, made if it has none.
[[nodiscard]] entt::sparse_set& storage(world::World& world, const Layout& layout);

// The function get_proc gives for a name, or nullptr.
[[nodiscard]] SlashProc proc(const char* name);

// What a plugin registers while its entry function runs.
struct Registration {
    std::string plugin;
    world::ComponentRegistry* components = nullptr;
    std::vector<const Layout*> layouts;
    std::vector<PluginSystem> systems;
    std::vector<std::string> errors;
};

} // namespace slash::plugin::detail

// The opaque handles of the C interface.
struct SlashPlugin {
    slash::plugin::detail::Registration* registration = nullptr;
};
