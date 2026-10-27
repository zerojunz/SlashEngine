// Shadow maps: which lights cast shadows, the passes that draw their maps and the matrices the
// forward pass samples them through, as Godot's Compatibility renderer works them out
// (RendererSceneCull::_light_instance_setup_directional_shadow and _light_instance_update_shadow,
// LightStorage::_get_directional_shadow_rect and RasterizerSceneGLES3::_setup_lights and
// render_shadow), and the CPU twins of shaders/shadow.hlsl and of sampling a shadow atlas. Float
// math throughout, built with -ffp-contract=off, so the software device's shadows are the same
// pixels on every machine.

#include "mesh_shading.hpp"

#include "slash/core/math.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <vector>

namespace slash::render::detail {

namespace {

using Double4 = std::array<double, 4>;
using DMat4 = std::array<double, 16>;

float dot3(const Float3& a, const Float3& b) {
    return (a[0] * b[0]) + (a[1] * b[1]) + (a[2] * b[2]);
}

Float3 cross(const Float3& a, const Float3& b) {
    return {(a[1] * b[2]) - (a[2] * b[1]), (a[2] * b[0]) - (a[0] * b[2]),
            (a[0] * b[1]) - (a[1] * b[0])};
}

Float3 scale(const Float3& a, float s) {
    return {a[0] * s, a[1] * s, a[2] * s};
}

Float3 add(const Float3& a, const Float3& b) {
    return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}

Float3 sub(const Float3& a, const Float3& b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}

float length(const Float3& a) {
    return std::sqrt(dot3(a, a));
}

Float3 normalize(const Float3& a) {
    return scale(a, 1.0F / length(a));
}

Float4 transform(const Mat4& m, const Float4& v) {
    Float4 out{};
    for (std::size_t row = 0; row < 4; ++row) {
        out.at(row) = (m.at(row) * v[0]) + (m.at(4 + row) * v[1]) + (m.at(8 + row) * v[2]) +
                      (m.at(12 + row) * v[3]);
    }
    return out;
}

// The inverse of a matrix, in doubles; the identity for one that has none.
DMat4 inverse(const Mat4& m) {
    DMat4 a{};
    for (std::size_t i = 0; i < 16; ++i) {
        a.at(i) = m.at(i);
    }
    // Gauss-Jordan elimination with partial pivoting, on rows of [a | identity], a read
    // row-major from the column-major matrix.
    std::array<std::array<double, 8>, 4> rows{};
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t column = 0; column < 4; ++column) {
            rows.at(row).at(column) = a.at((column * 4) + row);
        }
        rows.at(row).at(4 + row) = 1.0;
    }
    for (std::size_t column = 0; column < 4; ++column) {
        std::size_t pivot = column;
        for (std::size_t row = column + 1; row < 4; ++row) {
            if (std::abs(rows.at(row).at(column)) > std::abs(rows.at(pivot).at(column))) {
                pivot = row;
            }
        }
        if (rows.at(pivot).at(column) == 0.0) {
            DMat4 identity{};
            identity[0] = identity[5] = identity[10] = identity[15] = 1.0;
            return identity;
        }
        std::swap(rows.at(pivot), rows.at(column));
        const double inv = 1.0 / rows.at(column).at(column);
        for (double& value : rows.at(column)) {
            value *= inv;
        }
        for (std::size_t row = 0; row < 4; ++row) {
            if (row == column) {
                continue;
            }
            const double factor = rows.at(row).at(column);
            for (std::size_t k = 0; k < 8; ++k) {
                rows.at(row).at(k) -= factor * rows.at(column).at(k);
            }
        }
    }
    DMat4 out{};
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t column = 0; column < 4; ++column) {
            out.at((column * 4) + row) = rows.at(row).at(4 + column);
        }
    }
    return out;
}

Double4 transform(const DMat4& m, const Double4& v) {
    Double4 out{};
    for (std::size_t row = 0; row < 4; ++row) {
        out.at(row) = (m.at(row) * v[0]) + (m.at(4 + row) * v[1]) + (m.at(8 + row) * v[2]) +
                      (m.at(12 + row) * v[3]);
    }
    return out;
}

// A point through a matrix, divided by w.
Float3 project(const DMat4& m, const Double4& v) {
    const Double4 out = transform(m, v);
    return {static_cast<float>(out[0] / out[3]), static_cast<float>(out[1] / out[3]),
            static_cast<float>(out[2] / out[3])};
}

// An orthonormal basis, as a node's columns: x right, y up, z back.
struct Basis {
    Float3 x{1.0F, 0.0F, 0.0F};
    Float3 y{0.0F, 1.0F, 0.0F};
    Float3 z{0.0F, 0.0F, 1.0F};
};

// A light's basis: its z is against direction and its y as near up as can be, as its node's.
Basis light_basis(const Light3D& light) {
    Basis b;
    b.z = scale(normalize(light.direction), -1.0F);
    Float3 up = sub(light.up, scale(b.z, dot3(light.up, b.z)));
    if (!(length(up) > 1e-6F)) {
        // An up along the light, or none: any square to it.
        up = std::abs(b.z[1]) < 0.9F ? Float3{0.0F, 1.0F, 0.0F} : Float3{1.0F, 0.0F, 0.0F};
        up = sub(up, scale(b.z, dot3(up, b.z)));
    }
    b.y = normalize(up);
    b.x = cross(b.y, b.z);
    return b;
}

// The world to the space of a camera turned by basis at origin: its inverse transform.
Mat4 view_of(const Basis& basis, const Float3& origin) {
    const std::array<Float3, 3> axes{basis.x, basis.y, basis.z};
    Mat4 m{};
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 3; ++column) {
            m.at((column * 4) + row) = axes.at(row).at(column);
        }
        m.at(12 + row) = -dot3(axes.at(row), origin);
    }
    m[15] = 1.0F;
    return m;
}

// Godot's Projection::set_orthogonal(left, right, bottom, top, 0, far) for a camera centered on
// the box, with reversed depth.
Mat4 ortho_box(float half_x, float half_y, float far) {
    Mat4 m{};
    m[0] = 1.0F / half_x;
    m[5] = 1.0F / half_y;
    m[10] = 1.0F / far;
    m[14] = 1.0F;
    m[15] = 1.0F;
    return m;
}

// Clip space to a rect of an atlas size texels across: x and y become its texture coordinates
// (from its top left) times w.
Mat4 atlas_matrix(const AtlasRect& rect, std::uint32_t size) {
    const auto s = static_cast<float>(size);
    const auto w = static_cast<float>(rect.width);
    const auto h = static_cast<float>(rect.height);
    Mat4 m{};
    m[0] = 0.5F * w / s;
    m[5] = -0.5F * h / s;
    m[10] = 1.0F;
    m[12] = (static_cast<float>(rect.x) + (0.5F * w)) / s;
    m[13] = (static_cast<float>(rect.y) + (0.5F * h)) / s;
    m[15] = 1.0F;
    return m;
}

// Math::snapped.
float snapped(float value, float step) {
    return std::floor((value / step) + 0.5F) * step;
}

float taps(ShadowQuality quality) {
    switch (quality) {
    case ShadowQuality::Hard:
    case ShadowQuality::SoftVeryLow:
        return 1.0F;
    case ShadowQuality::SoftLow:
    case ShadowQuality::SoftMedium:
        return 5.0F;
    case ShadowQuality::SoftHigh:
    case ShadowQuality::SoftUltra:
        break;
    }
    return 13.0F;
}

// What a camera sees: its frustum's corners on the near and far planes, in its own space, the
// depths of those planes, the way back to the world, and whether it is orthogonal.
struct CameraFrustum {
    std::array<Float3, 4> near_corners{};
    std::array<Float3, 4> far_corners{};
    float near = 0.0F;
    float far = 0.0F;
    DMat4 to_world{};
    bool orthogonal = false;
};

CameraFrustum camera_frustum(const Scene3D& scene) {
    CameraFrustum out;
    const DMat4 unproject = inverse(scene.projection);
    constexpr std::array<std::array<double, 2>, 4> kCorners{
        {{-1.0, -1.0}, {1.0, -1.0}, {1.0, 1.0}, {-1.0, 1.0}}};
    for (std::size_t i = 0; i < 4; ++i) {
        // Reversed depth: the near plane at 1, the far one at 0.
        out.near_corners.at(i) =
            project(unproject, {kCorners.at(i)[0], kCorners.at(i)[1], 1.0, 1.0});
        out.far_corners.at(i) =
            project(unproject, {kCorners.at(i)[0], kCorners.at(i)[1], 0.0, 1.0});
    }
    out.near = -project(unproject, {0.0, 0.0, 1.0, 1.0})[2];
    out.far = -project(unproject, {0.0, 0.0, 0.0, 1.0})[2];
    if (!std::isfinite(out.far) || !(out.far > out.near)) {
        out.far = out.near + 1e6F;
    }
    out.to_world = inverse(scene.camera);
    const Mat4& p = scene.projection;
    out.orthogonal = p[11] == 0.0F && p[15] == 1.0F;
    return out;
}

// The eight corners of the view between depths from and to, in the world.
std::array<Float3, 8> slice(const CameraFrustum& camera, float from, float to) {
    std::array<Float3, 8> out{};
    const float span = camera.far - camera.near;
    for (std::size_t end = 0; end < 2; ++end) {
        const float t = ((end == 0 ? from : to) - camera.near) / span;
        for (std::size_t i = 0; i < 4; ++i) {
            const Float3& a = camera.near_corners.at(i);
            const Float3& b = camera.far_corners.at(i);
            const Float3 seen = add(a, scale(sub(b, a), t));
            out.at((end * 4) + i) = project(camera.to_world, {seen[0], seen[1], seen[2], 1.0});
        }
    }
    return out;
}

// LightStorage::_get_directional_shadow_rect.
AtlasRect directional_rect(std::uint32_t size, std::size_t count, std::size_t index) {
    std::uint32_t split_h = 1;
    std::uint32_t split_v = 1;
    while (std::size_t{split_h} * split_v < count) {
        if (split_h == split_v) {
            split_h <<= 1U;
        } else {
            split_v <<= 1U;
        }
    }
    AtlasRect rect{.width = size / split_h, .height = size / split_v};
    rect.x = rect.width * static_cast<std::uint32_t>(index % split_h);
    rect.y = rect.height * static_cast<std::uint32_t>(index / split_h);
    return rect;
}

std::size_t cascades(DirectionalShadowMode mode) {
    switch (mode) {
    case DirectionalShadowMode::Orthogonal:
        return 1;
    case DirectionalShadowMode::Parallel2Splits:
        return 2;
    case DirectionalShadowMode::Parallel4Splits:
        break;
    }
    return 4;
}

// A directional light's cascades: passes into rect of the atlas, and their uniforms.
void plan_directional(const Scene3D& scene, const CameraFrustum& camera, std::size_t index,
                      int slot, const AtlasRect& rect, std::uint32_t size, ShadowPlan& plan) {
    const Light3D& light = scene.lights[index];
    MeshDirectionalShadowUniforms& out =
        plan.uniforms->directional.at(static_cast<std::size_t>(slot));
    const std::size_t splits = cascades(light.shadow_mode);

    float max_distance = camera.far;
    if (light.shadow_max_distance > 0.0F && !camera.orthogonal) {
        max_distance = std::min(light.shadow_max_distance, max_distance);
    }
    max_distance = std::max(max_distance, camera.near + 0.001F);
    const float min_distance = std::min(camera.near, max_distance);
    const float range = max_distance - min_distance;
    std::array<float, 5> distances{};
    distances[0] = min_distance;
    for (std::size_t i = 0; i < splits && i < 3; ++i) {
        distances.at(i + 1) = min_distance + (light.shadow_splits.at(i) * range);
    }
    distances.at(splits) = max_distance;

    // get_directional_light_shadow_size: the larger side of a cascade's share.
    AtlasRect cascade_rect = rect;
    if (splits == 2) {
        cascade_rect.height /= 2;
    } else if (splits == 4) {
        cascade_rect.width /= 2;
        cascade_rect.height /= 2;
    }
    const auto texture_size = static_cast<float>(std::max(cascade_rect.width, cascade_rect.height));
    const bool overlap = light.shadow_blend_splits;
    const Basis basis = light_basis(light);

    for (std::size_t i = 0; i < splits; ++i) {
        const std::array<Float3, 8> endpoints =
            slice(camera, distances.at((i == 0 || !overlap) ? i : i - 1), distances.at(i + 1));
        Float3 center{};
        for (const Float3& point : endpoints) {
            center = add(center, point);
        }
        center = scale(center, 1.0F / 8.0F);
        float radius = 0.0F;
        for (const Float3& point : endpoints) {
            radius = std::max(radius, length(sub(point, center)));
        }
        radius *= texture_size / (texture_size - 2.0F); // a texel more on each side
        const float z_min_cam = dot3(basis.z, center) - radius;
        // Snapping to whole texels keeps edges still as the camera moves.
        const float unit = radius * 4.0F / texture_size;
        const float x_max_cam = snapped(dot3(basis.x, center) + radius, unit);
        const float x_min_cam = snapped(dot3(basis.x, center) - radius, unit);
        const float y_max_cam = snapped(dot3(basis.y, center) + radius, unit);
        const float y_min_cam = snapped(dot3(basis.y, center) - radius, unit);
        const float z_max = dot3(basis.z, center) + radius + light.shadow_pancake_size;
        const float half_x = (x_max_cam - x_min_cam) * 0.5F;
        const float half_y = (y_max_cam - y_min_cam) * 0.5F;
        const Float3 origin =
            add(add(scale(basis.x, x_min_cam + half_x), scale(basis.y, y_min_cam + half_y)),
                scale(basis.z, z_max));
        const float depth = z_max - z_min_cam;

        AtlasRect pass_rect = cascade_rect;
        if (splits == 4) {
            pass_rect.x = rect.x + (i % 2 == 1 ? cascade_rect.width : 0);
            pass_rect.y = rect.y + (i >= 2 ? cascade_rect.height : 0);
        } else if (splits == 2) {
            pass_rect.y = rect.y + (i == 1 ? cascade_rect.height : 0);
        }
        ShadowPass pass;
        pass.light = index;
        pass.rect = pass_rect;
        pass.uniforms.view = view_of(basis, origin);
        pass.uniforms.projection = ortho_box(half_x, half_y, depth);
        pass.uniforms.options = {light.shadow_bias / 100.0F * depth, 0.0F,
                                 light.shadow_pancake_size > 0.0F ? 1.0F : 0.0F, 0.0F};
        pass.reverse_cull = light.shadow_reverse_cull_face;
        pass.caster_mask = light.shadow_caster_mask;
        out.matrices.at(i) = multiply(atlas_matrix(pass_rect, size),
                                      multiply(pass.uniforms.projection, pass.uniforms.view));
        out.normal_bias.at(i) = light.shadow_normal_bias * (radius * 2.0F / texture_size);
        plan.passes.push_back(pass);
    }
    for (std::size_t i = 0; i < 4; ++i) {
        out.splits.at(i) = distances.at(std::min(i, splits - 1) + 1);
    }
    out.params = {-out.splits[3] * std::min(light.shadow_fade_start, 0.999F), -out.splits[3],
                  light.shadow_opacity, static_cast<float>(splits)};
    out.options = {splits > 1 && light.shadow_blend_splits ? 1.0F : 0.0F,
                   1.0F / static_cast<float>(size), 0.0F, 0.0F};
}

// An omni or spot light's passes into tiles of the positional atlas, from first, and its
// uniforms.
void plan_positional(const Scene3D& scene, std::size_t index, int slot, std::uint32_t tile,
                     std::uint32_t grid, std::size_t& next_tile, std::uint32_t size,
                     ShadowPlan& plan) {
    const Light3D& light = scene.lights[index];
    MeshPositionalShadowUniforms& out =
        plan.uniforms->positional.at(static_cast<std::size_t>(slot));
    const Basis basis = light_basis(light);
    const Float3 position = light.position;
    const float radius = std::max(0.001F, light.range);
    const float z_near = std::min(0.025F, radius);
    const bool omni = light.type == LightType::Omni;
    const auto s = static_cast<float>(size);

    // The cube's faces, as Godot's view_normals and view_up, each turned with the light.
    static constexpr std::array<Float3, 6> kNormals{{{1.0F, 0.0F, 0.0F},
                                                     {-1.0F, 0.0F, 0.0F},
                                                     {0.0F, -1.0F, 0.0F},
                                                     {0.0F, 1.0F, 0.0F},
                                                     {0.0F, 0.0F, 1.0F},
                                                     {0.0F, 0.0F, -1.0F}}};
    static constexpr std::array<Float3, 6> kUps{{{0.0F, -1.0F, 0.0F},
                                                 {0.0F, -1.0F, 0.0F},
                                                 {0.0F, 0.0F, -1.0F},
                                                 {0.0F, 0.0F, 1.0F},
                                                 {0.0F, -1.0F, 0.0F},
                                                 {0.0F, -1.0F, 0.0F}}};
    const auto to_world = [&](const Float3& local) {
        return add(add(scale(basis.x, local[0]), scale(basis.y, local[1])),
                   scale(basis.z, local[2]));
    };
    const std::size_t faces = omni ? 6 : 1;
    const Mat4 projection =
        omni ? perspective(90.0F, 1.0F, z_near, radius)
             : perspective(std::min(light.spot_angle * 2.0F, 179.0F), 1.0F, z_near, radius);
    for (std::size_t face = 0; face < faces; ++face) {
        Basis turned = basis;
        if (omni) {
            // Basis::looking_at(normal, up), then turned with the light.
            const Float3 z = scale(kNormals.at(face), -1.0F);
            const Float3 x = normalize(cross(kUps.at(face), z));
            const Float3 y = cross(z, x);
            turned = {.x = to_world(x), .y = to_world(y), .z = to_world(z)};
        }
        const std::size_t at = next_tile++;
        const AtlasRect rect{.x = static_cast<std::uint32_t>(at % grid) * tile,
                             .y = static_cast<std::uint32_t>(at / grid) * tile,
                             .width = tile,
                             .height = tile};
        ShadowPass pass;
        pass.positional = true;
        pass.light = index;
        pass.rect = rect;
        pass.uniforms.view = view_of(turned, position);
        pass.uniforms.projection = projection;
        pass.uniforms.options = omni ? Float4{light.shadow_bias, 1.0F, 0.0F, 0.0F}
                                     : Float4{light.shadow_bias / 10.0F * radius, 0.0F, 0.0F, 0.0F};
        pass.reverse_cull = light.shadow_reverse_cull_face;
        pass.caster_mask = light.shadow_caster_mask;
        out.faces.at(face) = multiply(atlas_matrix(rect, size),
                                      multiply(pass.uniforms.projection, pass.uniforms.view));
        // Taps stay half a texel inside the tile, so filtering never reads its neighbors.
        out.rects.at(face) = {(static_cast<float>(rect.x) + 0.5F) / s,
                              (static_cast<float>(rect.y) + 0.5F) / s,
                              (static_cast<float>(rect.x + tile) - 0.5F) / s,
                              (static_cast<float>(rect.y + tile) - 0.5F) / s};
        plan.passes.push_back(pass);
    }
    out.light = view_of(basis, position);
    out.params = {light.shadow_normal_bias * 10.0F / static_cast<float>(tile), light.shadow_opacity,
                  1.0F / s, static_cast<float>(faces)};
    out.position = {position[0], position[1], position[2], radius};
}

} // namespace

std::array<double, 16> inverse_matrix(const Mat4& m) {
    return inverse(m);
}

std::vector<int> shadow_slots(const Scene3D& scene) {
    std::vector<int> slots(scene.lights.size(), -1);
    int directional = 0;
    int positional = 0;
    for (std::size_t i = 0; i < lights_drawn(scene); ++i) {
        const Light3D& light = scene.lights[i];
        if (!light.shadow) {
            continue;
        }
        if (light.type == LightType::Directional) {
            if (directional < static_cast<int>(kMaxDirectionalShadows)) {
                slots.at(i) = directional++;
            }
        } else if (positional < static_cast<int>(kMaxPositionalShadows)) {
            slots.at(i) = positional++;
        }
    }
    return slots;
}

ShadowPlan plan_shadows(const Scene3D& scene, std::uint32_t max_size) {
    ShadowPlan plan;
    MeshShadowUniforms& uniforms = *plan.uniforms;
    const Mat4& c = scene.camera;
    uniforms.camera_z = {c[2], c[6], c[10], c[14]};
    uniforms.filters = {taps(scene.shadows.directional_quality),
                        taps(scene.shadows.positional_quality), 0.0F, 0.0F};
    const std::vector<int> slots = shadow_slots(scene);
    std::vector<std::size_t> directional;
    std::vector<std::size_t> positional;
    std::size_t tiles = 0;
    for (std::size_t i = 0; i < slots.size(); ++i) {
        if (slots.at(i) < 0) {
            continue;
        }
        if (scene.lights[i].type == LightType::Directional) {
            directional.push_back(i);
        } else {
            positional.push_back(i);
            tiles += scene.lights[i].type == LightType::Omni ? 6U : 1U;
        }
    }
    if (!directional.empty()) {
        const std::uint32_t size = std::min(scene.shadows.directional_size, max_size);
        plan.directional_size = size;
        const CameraFrustum camera = camera_frustum(scene);
        for (std::size_t k = 0; k < directional.size(); ++k) {
            plan_directional(scene, camera, directional[k], slots.at(directional[k]),
                             directional_rect(size, directional.size(), k), size, plan);
        }
    }
    if (!positional.empty()) {
        const std::uint32_t size = std::min(scene.shadows.positional_size, max_size);
        plan.positional_size = size;
        std::uint32_t grid = 1;
        while (std::size_t{grid} * grid < tiles) {
            grid *= 2;
        }
        const std::uint32_t tile = size / grid;
        std::size_t next_tile = 0;
        for (const std::size_t index : positional) {
            plan_positional(scene, index, slots.at(index), tile, grid, next_tile, size, plan);
        }
    }
    return plan;
}

bool shadow_pass_draws(const ShadowPass& pass, const MeshDraw& draw, const Bounds& bounds) {
    if (draw.shadows == ShadowCasting::Off || draw.material.transparency == Transparency::Alpha ||
        (draw.layers & pass.caster_mask) == 0) {
        return false;
    }
    const Mat4 to_clip =
        multiply(pass.uniforms.projection, multiply(pass.uniforms.view, draw.transform));
    std::array<Float4, 8> corners{};
    for (std::size_t i = 0; i < 8; ++i) {
        corners.at(i) = transform(to_clip, {(i & 1U) != 0 ? bounds.high[0] : bounds.low[0],
                                            (i & 2U) != 0 ? bounds.high[1] : bounds.low[1],
                                            (i & 4U) != 0 ? bounds.high[2] : bounds.low[2], 1.0F});
    }
    // Outside a plane when every corner is.
    const std::array<float (*)(const Float4&), 5> planes{
        [](const Float4& v) { return v[3] - v[0]; }, [](const Float4& v) { return v[3] + v[0]; },
        [](const Float4& v) { return v[3] - v[1]; }, [](const Float4& v) { return v[3] + v[1]; },
        [](const Float4& v) { return v[2]; }};
    return std::ranges::none_of(planes, [&](const auto inside) {
        return std::ranges::all_of(corners, [&](const Float4& v) { return inside(v) < 0.0F; });
    });
}

CullMode shadow_cull(const ShadowPass& pass, const MeshDraw& draw) {
    if (draw.shadows == ShadowCasting::DoubleSided || draw.material.cull == CullMode::Disabled) {
        return CullMode::Disabled;
    }
    const bool back = draw.material.cull == CullMode::Back;
    return back != pass.reverse_cull ? CullMode::Back : CullMode::Front;
}

Float4 shadow_vertex(const MeshShadowPassUniforms& pass, const MeshInstanceUniforms& instance,
                     const Vertex3D& vertex) {
    const Float4 world = transform(
        instance.model, {vertex.position[0], vertex.position[1], vertex.position[2], 1.0F});
    const Float4 seen = transform(pass.view, world);
    Float3 v{seen[0], seen[1], seen[2]};
    const float bias = pass.options[0];
    if (pass.options[1] != 0.0F) {
        // An omni light's: away from the light by the bias, as its linear depth would be.
        v = add(v, scale(v, bias / std::max(length(v), 1e-6F)));
    } else {
        // Godot's normalize(v) * bias / length(v).
        v = add(v, scale(v, bias / std::max(dot3(v, v), 1e-12F)));
    }
    Float4 clip = transform(pass.projection, {v[0], v[1], v[2], 1.0F});
    if (pass.options[2] != 0.0F) {
        // The pancake: casters nearer the light than the near plane stay on it.
        clip[2] = std::min(clip[2], clip[3] * 0.9999F);
    }
    return clip;
}

float ShadowAtlas::compare(float u, float v, float reference) const {
    if (size == 0) {
        return 1.0F;
    }
    const auto s = static_cast<float>(size);
    const float x = (std::clamp(u, -1.0F, 2.0F) * s) - 0.5F;
    const float y = (std::clamp(v, -1.0F, 2.0F) * s) - 0.5F;
    const float x0 = std::floor(x);
    const float y0 = std::floor(y);
    const float fx = x - x0;
    const float fy = y - y0;
    const auto last = static_cast<std::int64_t>(size) - 1;
    const auto texel = [&](float column, float row) {
        const auto cx = std::clamp(static_cast<std::int64_t>(column), std::int64_t{0}, last);
        const auto cy = std::clamp(static_cast<std::int64_t>(row), std::int64_t{0}, last);
        return reference >=
                       depths[(static_cast<std::size_t>(cy) * size) + static_cast<std::size_t>(cx)]
                   ? 1.0F
                   : 0.0F;
    };
    const float top = texel(x0, y0) + ((texel(x0 + 1.0F, y0) - texel(x0, y0)) * fx);
    const float bottom =
        texel(x0, y0 + 1.0F) + ((texel(x0 + 1.0F, y0 + 1.0F) - texel(x0, y0 + 1.0F)) * fx);
    return top + ((bottom - top) * fy);
}

bool shadow_keeps(const MeshSurfaceUniforms& surface, const Float4& albedo_texel,
                  const Float4& color) {
    const std::uint32_t flags = surface.options[0];
    if ((flags & kMeshAlphaScissor) == 0) {
        return true;
    }
    float alpha = surface.albedo[3] * albedo_texel[3];
    if ((flags & kMeshVertexColorAlbedo) != 0) {
        alpha *= color[3];
    }
    return !(alpha < same_bits<float>(surface.options[1]));
}

} // namespace slash::render::detail

namespace slash::render {

bool valid_shadow_atlas_size(std::uint32_t size) {
    return size >= 256 && size <= kMaxTextureSize && (size & (size - 1)) == 0;
}

ShadowSettings shadow_settings_from_json(const json::Value& value) {
    ShadowSettings out;
    if (!value.is_object()) {
        return out;
    }
    const auto read = [](const json::Value* atlas, std::string_view size_key, std::uint32_t& size,
                         ShadowQuality& quality) {
        if (atlas == nullptr || !atlas->is_object()) {
            return;
        }
        if (const json::Value* found = atlas->find(size_key);
            found != nullptr && found->is_number() && found->as_double() >= 0.0 &&
            found->as_double() <= kMaxTextureSize &&
            valid_shadow_atlas_size(static_cast<std::uint32_t>(found->as_double()))) {
            size = static_cast<std::uint32_t>(found->as_double());
        }
        if (const json::Value* found = atlas->find("soft_shadow_filter_quality");
            found != nullptr && found->is_string()) {
            for (std::size_t i = 0; i < kShadowQualityNames.size(); ++i) {
                if (found->as_string() == kShadowQualityNames.at(i)) {
                    quality = static_cast<ShadowQuality>(i);
                }
            }
        }
    };
    read(value.find("directional_shadow"), "size", out.directional_size, out.directional_quality);
    read(value.find("positional_shadow"), "atlas_size", out.positional_size,
         out.positional_quality);
    return out;
}

} // namespace slash::render
