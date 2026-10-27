#pragma once

#include "mesh_shading.hpp"

#include "slash/core/result.hpp"
#include "slash/render/device.hpp"
#include "slash/render/mesh.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

// What every device shares to draw decals (Decals3D, in mesh.hpp) as Godot 4.6's Forward+ draws
// them: the atlas of their textures, packed as Godot's TextureStorage::update_decal_atlas packs it,
// the decals as mesh.hlsl's decals buffer holds them, and the CPU twin of mesh.hlsl's decal loop,
// which the software device draws with. The atlas is a storage buffer that the shader filters
// itself, not a texture, so that every device reads the same texels the same way.
namespace slash::render::detail {

// The atlas's levels, and Godot's border: 1 << its levels, which pads each texture and aligns it
// to cells this many texels across, so that the levels keep textures apart.
inline constexpr std::uint32_t kDecalAtlasLevels = 5;
inline constexpr std::uint32_t kDecalAtlasBorder = 1U << kDecalAtlasLevels;
// The most bytes a device's storage buffer grows to unless one frame's data needs more: WebGPU's
// default maxStorageBufferBindingSize, 128 MiB, which every device binds.
inline constexpr std::uint32_t kMaxStorageBytes = 1U << 27U;
// The most texels an atlas holds over all its levels: a storage buffer's worth, 2^25. A texture
// 2048 texels across takes an atlas of 4096 by 4096, 2^24 texels and a third more for its levels.
inline constexpr std::size_t kMaxDecalAtlasTexels = kMaxStorageBytes / 4;

// The textures a frame's decals use, each once, in the order they first use them: each decal's
// albedo, normal, ORM and emission, the decals in order.
[[nodiscard]] std::vector<std::shared_ptr<const Image>> decal_images(const Decals3D& decals);

// Where Godot's packing puts textures of these sizes in an atlas: its width and height in texels,
// and each texture's first texel. Each takes its size over kDecalAtlasBorder plus one cells, and
// they go tallest first (then widest, then in order) at the lowest spot of a skyline as many cells
// across as the widest needs, a power of two at least 8, doubled until the atlas is at most twice
// as tall as wide; its height is a power of two.
struct DecalAtlasLayout {
    std::uint32_t width = kDecalAtlasBorder;
    std::uint32_t height = kDecalAtlasBorder;
    std::vector<std::array<std::uint32_t, 2>> places;
};
[[nodiscard]] DecalAtlasLayout
layout_decal_atlas(const std::vector<std::array<std::uint32_t, 2>>& sizes);
// The texels an atlas this size holds over all its levels.
[[nodiscard]] std::size_t decal_atlas_texels(std::uint32_t width, std::uint32_t height);

// An atlas of a frame's decal textures: RGBA8 texels with red in the low byte, in rows from the
// top, its kDecalAtlasLevels levels one after another, each half the size of the one before.
struct DecalAtlas {
    std::uint32_t width = kDecalAtlasBorder;
    std::uint32_t height = kDecalAtlasBorder;
    std::vector<std::uint32_t> texels;
    // The textures it holds and their rects, left, top, width and height as fractions of its size.
    std::vector<std::shared_ptr<const Image>> images;
    std::vector<Float4> rects;
    // Counts the atlases a cache has packed, so that a device uploads one only when it changes.
    std::uint64_t generation = 0;

    // A texture's rect, or all 0 for none or one it doesn't hold.
    [[nodiscard]] Float4 rect(const std::shared_ptr<const Image>& image) const;
};
// Packs the atlas of a frame's decal textures, as Godot fills it: level 0 holds each texture at its
// place, transparent black around them, and each level after it the average of each 2x2 texels of
// the level before, rounded.
[[nodiscard]] DecalAtlas pack_decal_atlas(const Decals3D& decals);

// A device's atlas, packed again only when a frame's decal textures change.
class DecalAtlasCache {
public:
    // Brings the atlas up to date with a frame's decals, and returns whether it packed it again.
    bool update(const Decals3D& decals);
    [[nodiscard]] const DecalAtlas& atlas() const { return atlas_; }

private:
    DecalAtlas atlas_;
    std::uint64_t packed_ = 0;
};

// The decals a frame draws, in Godot's order (TextureStorage::update_decal_buffer), which is the
// decals buffer's: the first kMaxDecals3D within their distance fade, nearest the camera first
// less their sorting_offset, leaving out those with neither an albedo nor an emission texture and
// those whose box has no inverse; and with each, its box: its space to the world, as
// MeshDecalUniforms has it.
struct PlacedDecals {
    std::vector<MeshDecalUniforms> uniforms;
    std::vector<Mat4> boxes;
};
[[nodiscard]] PlacedDecals place_decals(const Scene3D& scene, const DecalAtlas& atlas);

// Sets the scene uniforms Forward+'s clusters give: MeshSceneUniforms::cluster, decal_info and
// screen.
void set_cluster_uniforms(MeshSceneUniforms& uniforms, const LightClusters& clusters,
                          const DecalAtlas* atlas, DecalFilter filter);

// What a pixel's decals change before it is lit: its albedo (linear), shading normal, ambient
// occlusion, roughness, metallic and emission (linear).
struct DecalSurface {
    Float3 base{};
    Float3 normal{};
    float ao = 1.0F;
    float roughness = 0.0F;
    float metallic = 0.0F;
    Float3 emitted{};
};
// The CPU twin of apply_decals in shaders/mesh.hlsl, Godot's decal loop: the decals of the pixel's
// tile within its slice's range, each word's from its highest bit, that share a layer with the
// mesh's, with geometry its vertex normal (turned to its side), as Godot's geo_normal.
void apply_decals(const MeshSceneUniforms& scene, const MeshMaps& maps, const MeshPixel& pixel,
                  std::uint32_t layers, const Float3& geometry, DecalSurface& surface);

} // namespace slash::render::detail
