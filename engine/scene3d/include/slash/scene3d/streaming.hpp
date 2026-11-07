#pragma once

#include "slash/core/math.hpp"
#include "slash/scene3d/scene3d.hpp"

#include <entt/entity/entity.hpp>
#include <optional>
#include <string>
#include <vector>

namespace slash::world {
class World;
}

// World streaming, as Unreal's World Partition streams cells (Godot has none): each
// StreamedScene3D loads its scene file while a StreamingSource3D is near and unloads it when they
// have all gone, so a world too large to keep whole is kept in pieces.
namespace slash::scene3d {

// Adds the world_streaming system (priority -700, after input and before gameplay), which loads
// and unloads each StreamedScene3D's scene, in the order their entities were made, and emits
// streamed_in and streamed_out events (or stream_failed, saying why, once for each scene that
// can't load). A loaded scene's root is the entity "<id>_scene", its other entities
// "<id>_scene/<their id>", and the root is one of the StreamedScene3D's Transform3D children.
void add_streaming_system(world::World& world);

// The ID of the entity a StreamedScene3D's scene becomes: "<id>_scene".
[[nodiscard]] std::string streamed_root_id(std::string_view id);

// One StreamedScene3D as the system sees it.
struct StreamedCell {
    entt::entity entity = entt::null;
    bool loaded = false;
    // Meters from the nearest enabled source to its box; none when there is no source.
    std::optional<float> distance;
};

// The world's StreamedScene3Ds, in the order their entities were made.
[[nodiscard]] std::vector<StreamedCell> streamed_cells(const world::World& world);

// The StreamedScene3Ds of a world that can't load: a scene path that names no .scene.json file,
// a file that can't be read or names no root, and an entity with no Transform3D to place it under,
// or whose ID is too long for its scene's root.
[[nodiscard]] std::vector<Problem> check_streaming(const world::World& world);

} // namespace slash::scene3d
