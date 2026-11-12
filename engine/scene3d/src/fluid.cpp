// Fluids drawn as discs facing the camera (slash/scene3d/fluid.hpp).
#include "slash/scene3d/fluid.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace slash::scene3d {

namespace {

std::uint8_t to_byte(float value) {
    return static_cast<std::uint8_t>(std::floor((std::clamp(value, 0.0F, 1.0F) * 255.0F) + 0.5F));
}

} // namespace

render::Image fluid_disc_image() {
    constexpr std::uint32_t kSize = 64;
    render::Image image;
    image.width = kSize;
    image.height = kSize;
    image.pixels.assign(static_cast<std::size_t>(kSize) * kSize * 4, 255);
    for (std::uint32_t y = 0; y < kSize; ++y) {
        for (std::uint32_t x = 0; x < kSize; ++x) {
            const float u =
                ((static_cast<float>(x) + 0.5F) / static_cast<float>(kSize) * 2.0F) - 1.0F;
            const float v =
                ((static_cast<float>(y) + 0.5F) / static_cast<float>(kSize) * 2.0F) - 1.0F;
            const float r2 = (u * u) + (v * v);
            // Solid to half way, then falling smoothly to nothing at the rim.
            const float edge = std::clamp((1.0F - r2) / 0.75F, 0.0F, 1.0F);
            const std::size_t at = ((static_cast<std::size_t>(y) * kSize) + x) * 4;
            image.pixels[at + 3] = to_byte(edge * edge * (3.0F - (2.0F * edge)));
        }
    }
    return image;
}

void fluid_quads(const world::FluidRecipe& recipe, const world::FluidParticles& particles,
                 Vec3f right, Vec3f up, Vec3f shift, std::vector<render::Vertex3D>& vertices,
                 std::vector<std::uint32_t>& indices) {
    vertices.clear();
    indices.clear();
    const float half = world::fluid_draw_size(recipe) * 0.5F;
    const Vec3f facing{(right.y * up.z) - (right.z * up.y), (right.z * up.x) - (right.x * up.z),
                       (right.x * up.y) - (right.y * up.x)};
    constexpr std::array<std::array<float, 2>, 4> kCorners{
        {{-1.0F, -1.0F}, {1.0F, -1.0F}, {1.0F, 1.0F}, {-1.0F, 1.0F}}};
    for (std::size_t i = 0; i < particles.size(); ++i) {
        const Vec4 color = world::fluid_color(recipe, particles, i);
        if (color.w * 255.0F < 1.0F) {
            continue;
        }
        const std::array<std::uint8_t, 4> rgba{to_byte(color.x), to_byte(color.y), to_byte(color.z),
                                               to_byte(color.w)};
        const Vec3f middle = particles.position[i] + shift;
        const auto first = static_cast<std::uint32_t>(vertices.size());
        for (const auto& [cx, cy] : kCorners) {
            render::Vertex3D vertex;
            const Vec3f at = middle + (right * (cx * half)) + (up * (cy * half));
            vertex.position = {at.x, at.y, at.z};
            // A bulge: halfway between facing and out along the corner.
            Vec3f normal = facing + (((right * cx) + (up * cy)) * (recipe.gas ? 0.0F : 0.7F));
            const float n =
                std::sqrt((normal.x * normal.x) + (normal.y * normal.y) + (normal.z * normal.z));
            normal = n > 1e-9F ? normal * (1.0F / n) : facing;
            vertex.normal = {normal.x, normal.y, normal.z};
            vertex.tangent = {right.x, right.y, right.z, 1.0F};
            vertex.uv = {(cx + 1.0F) * 0.5F, (1.0F - cy) * 0.5F};
            vertex.rgba = rgba;
            vertices.push_back(vertex);
        }
        indices.insert(indices.end(), {first, first + 2, first + 1, first, first + 3, first + 2});
    }
}

} // namespace slash::scene3d
