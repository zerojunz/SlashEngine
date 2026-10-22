#include "slash/scene3d/primitive.hpp"

#include "slash/core/math.hpp"
#include "slash/world/world.hpp"

#include <cmath>
#include <numbers>
#include <string>

// Each maker follows the Godot function of the same mesh in scene/resources/3d/
// primitive_meshes.cpp, loop for loop, leaving out the second texture coordinates (UV2).
namespace slash::scene3d {

namespace {

constexpr float kTau = 2.0F * std::numbers::pi_v<float>;
constexpr float kPi = std::numbers::pi_v<float>;
constexpr float kOneThird = 1.0F / 3.0F;
constexpr float kTwoThirds = 2.0F / 3.0F;

// Builds a mesh corner by corner, as Godot's arrays.
struct Builder {
    MeshData mesh;
    std::uint32_t point = 0;

    void add(Vec3 position, Vec3 normal, std::array<float, 4> tangent, Vec2 uv) {
        mesh.vertices.push_back({.position = {position.x, position.y, position.z},
                                 .normal = {normal.x, normal.y, normal.z},
                                 .tangent = tangent,
                                 .uv = {uv.x, uv.y}});
        ++point;
    }
    void index(int i) { mesh.indices.push_back(static_cast<std::uint32_t>(i)); }
    void triangle(int a, int b, int c) {
        index(a);
        index(b);
        index(c);
    }
};

Vec3 normalized(Vec3 v) {
    const float length = std::sqrt((v.x * v.x) + (v.y * v.y) + (v.z * v.z));
    return length == 0.0F ? v : Vec3{v.x / length, v.y / length, v.z / length};
}

// The angle's cosine and sine as Godot reads them going round: x = sin, z = cos.
Vec2 around(float u) {
    return cos_sin(u * kTau);
}

// Turns a mesh inside out, as PrimitiveMesh::_update does for flip_faces: each triangle's first
// two corners swap and the normals point the other way.
MeshData flipped(MeshData mesh, bool flip) {
    if (!flip) {
        return mesh;
    }
    for (render::Vertex3D& vertex : mesh.vertices) {
        for (float& n : vertex.normal) {
            n = -n;
        }
    }
    for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        std::swap(mesh.indices[i], mesh.indices[i + 1]);
    }
    return mesh;
}

// The four index pattern of a strip of quads between two rows of corners, as Godot pushes it.
void quad(Builder& b, int prev_a, int prev_b, int this_a, int this_b) {
    b.triangle(prev_a, prev_b, this_a);
    b.triangle(prev_b, this_b, this_a);
}

} // namespace

MeshData box_mesh(const world::BoxMesh& box) {
    Builder b;
    const Vec3 size = box.size;
    const int subdivide_w = std::max(box.subdivide_width, 0);
    const int subdivide_h = std::max(box.subdivide_height, 0);
    const int subdivide_d = std::max(box.subdivide_depth, 0);
    const Vec3 start_pos = size * -0.5F;
    const auto w1 = static_cast<float>(subdivide_w + 1);
    const auto h1 = static_cast<float>(subdivide_h + 1);
    const auto d1 = static_cast<float>(subdivide_d + 1);

    // front + back
    float y = start_pos.y;
    int thisrow = 0;
    int prevrow = 0;
    for (int j = 0; j <= subdivide_h + 1; ++j) {
        const float v = static_cast<float>(j) / (2.0F * h1);
        float x = start_pos.x;
        for (int i = 0; i <= subdivide_w + 1; ++i) {
            const float u = static_cast<float>(i) / (3.0F * w1);
            b.add({x, -y, -start_pos.z}, {0.0F, 0.0F, 1.0F}, {1.0F, 0.0F, 0.0F, 1.0F}, {u, v});
            b.add({-x, -y, start_pos.z}, {0.0F, 0.0F, -1.0F}, {-1.0F, 0.0F, 0.0F, 1.0F},
                  {kTwoThirds + u, v});
            if (i > 0 && j > 0) {
                const int i2 = i * 2;
                quad(b, prevrow + i2 - 2, prevrow + i2, thisrow + i2 - 2, thisrow + i2);
                quad(b, prevrow + i2 - 1, prevrow + i2 + 1, thisrow + i2 - 1, thisrow + i2 + 1);
            }
            x += size.x / w1;
        }
        y += size.y / h1;
        prevrow = thisrow;
        thisrow = static_cast<int>(b.point);
    }

    // left + right
    y = start_pos.y;
    thisrow = static_cast<int>(b.point);
    prevrow = 0;
    for (int j = 0; j <= subdivide_h + 1; ++j) {
        const float v = static_cast<float>(j) / (2.0F * h1);
        float z = start_pos.z;
        for (int i = 0; i <= subdivide_d + 1; ++i) {
            const float u = static_cast<float>(i) / (3.0F * d1);
            b.add({-start_pos.x, -y, -z}, {1.0F, 0.0F, 0.0F}, {0.0F, 0.0F, -1.0F, 1.0F},
                  {kOneThird + u, v});
            b.add({start_pos.x, -y, z}, {-1.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 1.0F, 1.0F},
                  {u, 0.5F + v});
            if (i > 0 && j > 0) {
                const int i2 = i * 2;
                quad(b, prevrow + i2 - 2, prevrow + i2, thisrow + i2 - 2, thisrow + i2);
                quad(b, prevrow + i2 - 1, prevrow + i2 + 1, thisrow + i2 - 1, thisrow + i2 + 1);
            }
            z += size.z / d1;
        }
        y += size.y / h1;
        prevrow = thisrow;
        thisrow = static_cast<int>(b.point);
    }

    // top + bottom
    float z = start_pos.z;
    thisrow = static_cast<int>(b.point);
    prevrow = 0;
    for (int j = 0; j <= subdivide_d + 1; ++j) {
        const float v = static_cast<float>(j) / (2.0F * d1);
        float x = start_pos.x;
        for (int i = 0; i <= subdivide_w + 1; ++i) {
            const float u = static_cast<float>(i) / (3.0F * w1);
            b.add({-x, -start_pos.y, -z}, {0.0F, 1.0F, 0.0F}, {-1.0F, 0.0F, 0.0F, 1.0F},
                  {kOneThird + u, 0.5F + v});
            b.add({x, start_pos.y, -z}, {0.0F, -1.0F, 0.0F}, {1.0F, 0.0F, 0.0F, 1.0F},
                  {kTwoThirds + u, 0.5F + v});
            if (i > 0 && j > 0) {
                const int i2 = i * 2;
                quad(b, prevrow + i2 - 2, prevrow + i2, thisrow + i2 - 2, thisrow + i2);
                quad(b, prevrow + i2 - 1, prevrow + i2 + 1, thisrow + i2 - 1, thisrow + i2 + 1);
            }
            x += size.x / w1;
        }
        z += size.z / d1;
        prevrow = thisrow;
        thisrow = static_cast<int>(b.point);
    }
    return flipped(std::move(b.mesh), box.flip_faces);
}

MeshData sphere_mesh(const world::SphereMesh& sphere) {
    Builder b;
    const float radius = sphere.radius;
    const int radial_segments = std::max(sphere.radial_segments, 4);
    const int rings = std::max(sphere.rings, 1);
    const float scale = sphere.height / radius * (sphere.is_hemisphere ? 1.0F : 0.5F);
    int thisrow = 0;
    int prevrow = 0;
    for (int j = 0; j <= rings + 1; ++j) {
        const float v = static_cast<float>(j) / static_cast<float>(rings + 1);
        float w = 0.0F;
        float y = -1.0F;
        if (j != rings + 1) {
            const Vec2 cs = cos_sin(kPi * v);
            w = cs.y;
            y = cs.x;
        }
        for (int i = 0; i <= radial_segments; ++i) {
            const float u = static_cast<float>(i) / static_cast<float>(radial_segments);
            float x = 0.0F;
            float z = 1.0F;
            if (i != radial_segments) {
                const Vec2 cs = around(u);
                x = cs.y;
                z = cs.x;
            }
            if (sphere.is_hemisphere && y < 0.0F) {
                b.add({x * radius * w, 0.0F, z * radius * w}, {0.0F, -1.0F, 0.0F},
                      {z, 0.0F, -x, 1.0F}, {u, v});
            } else {
                const Vec3 p{x * w, y * scale, z * w};
                b.add(p * radius, normalized({x * w * scale, y, z * w * scale}),
                      {z, 0.0F, -x, 1.0F}, {u, v});
            }
            if (i > 0 && j > 0) {
                quad(b, prevrow + i - 1, prevrow + i, thisrow + i - 1, thisrow + i);
            }
        }
        prevrow = thisrow;
        thisrow = static_cast<int>(b.point);
    }
    return flipped(std::move(b.mesh), sphere.flip_faces);
}

MeshData cylinder_mesh(const world::CylinderMesh& cylinder) {
    Builder b;
    const float top_radius = cylinder.top_radius;
    const float bottom_radius = cylinder.bottom_radius;
    const float height = cylinder.height;
    const int radial_segments = std::max(cylinder.radial_segments, 4);
    const int rings = std::max(cylinder.rings, 0);
    int thisrow = 0;
    int prevrow = 0;
    const float side_normal_y = (bottom_radius - top_radius) / height;
    for (int j = 0; j <= rings + 1; ++j) {
        const float v = static_cast<float>(j) / static_cast<float>(rings + 1);
        const float radius = top_radius + ((bottom_radius - top_radius) * v);
        const float y = (height * 0.5F) - (height * v);
        for (int i = 0; i <= radial_segments; ++i) {
            const float u = static_cast<float>(i) / static_cast<float>(radial_segments);
            float x = 0.0F;
            float z = 1.0F;
            if (i != radial_segments) {
                const Vec2 cs = around(u);
                x = cs.y;
                z = cs.x;
            }
            b.add({x * radius, y, z * radius}, normalized({x, side_normal_y, z}),
                  {z, 0.0F, -x, 1.0F}, {u, v * 0.5F});
            if (i > 0 && j > 0) {
                quad(b, prevrow + i - 1, prevrow + i, thisrow + i - 1, thisrow + i);
            }
        }
        prevrow = thisrow;
        thisrow = static_cast<int>(b.point);
    }

    const auto cap = [&](float y, float radius, Vec3 normal, Vec2 middle, bool top) {
        thisrow = static_cast<int>(b.point);
        b.add({0.0F, y, 0.0F}, normal, {1.0F, 0.0F, 0.0F, 1.0F}, middle);
        for (int i = 0; i <= radial_segments; ++i) {
            const float r = static_cast<float>(i) / static_cast<float>(radial_segments);
            float x = 0.0F;
            float z = 1.0F;
            if (i != radial_segments) {
                const Vec2 cs = around(r);
                x = cs.y;
                z = cs.x;
            }
            const Vec2 uv = top ? Vec2{(x + 1.0F) * 0.25F, 0.5F + ((z + 1.0F) * 0.25F)}
                                : Vec2{0.5F + ((x + 1.0F) * 0.25F), 1.0F - ((z + 1.0F) * 0.25F)};
            b.add({x * radius, y, z * radius}, normal, {1.0F, 0.0F, 0.0F, 1.0F}, uv);
            if (i > 0) {
                const auto point = static_cast<int>(b.point);
                if (top) {
                    b.triangle(thisrow, point - 1, point - 2);
                } else {
                    b.triangle(thisrow, point - 2, point - 1);
                }
            }
        }
    };
    if (cylinder.cap_top && top_radius > 0.0F) {
        cap(height * 0.5F, top_radius, {0.0F, 1.0F, 0.0F}, {0.25F, 0.75F}, true);
    }
    if (cylinder.cap_bottom && bottom_radius > 0.0F) {
        cap(height * -0.5F, bottom_radius, {0.0F, -1.0F, 0.0F}, {0.75F, 0.75F}, false);
    }
    return flipped(std::move(b.mesh), cylinder.flip_faces);
}

MeshData capsule_mesh(const world::CapsuleMesh& capsule) {
    Builder b;
    const float radius = capsule.radius;
    const float height = capsule.height;
    const int radial_segments = std::max(capsule.radial_segments, 4);
    const int rings = std::max(capsule.rings, 0);
    const auto ring_corners = [&](int i) {
        const float u = static_cast<float>(i) / static_cast<float>(radial_segments);
        if (i == radial_segments) {
            return std::array<float, 3>{u, 0.0F, 1.0F};
        }
        const Vec2 cs = around(u);
        return std::array<float, 3>{u, -cs.y, cs.x};
    };

    // top hemisphere
    int thisrow = 0;
    int prevrow = 0;
    for (int j = 0; j <= rings + 1; ++j) {
        const float v = static_cast<float>(j) / static_cast<float>(rings + 1);
        float w = 1.0F;
        float y = 0.0F;
        if (j != rings + 1) {
            const Vec2 cs = cos_sin(0.5F * kPi * v);
            w = cs.y;
            y = cs.x;
        }
        for (int i = 0; i <= radial_segments; ++i) {
            const auto [u, x, z] = ring_corners(i);
            const Vec3 p{x * w, y, -z * w};
            b.add((p * radius) + Vec3{0.0F, (0.5F * height) - radius, 0.0F}, p,
                  {-z, 0.0F, -x, 1.0F}, {u, v * kOneThird});
            if (i > 0 && j > 0) {
                quad(b, prevrow + i - 1, prevrow + i, thisrow + i - 1, thisrow + i);
            }
        }
        prevrow = thisrow;
        thisrow = static_cast<int>(b.point);
    }

    // cylinder
    thisrow = static_cast<int>(b.point);
    prevrow = 0;
    for (int j = 0; j <= rings + 1; ++j) {
        const float v = static_cast<float>(j) / static_cast<float>(rings + 1);
        const float y = ((0.5F * height) - radius) - ((height - (2.0F * radius)) * v);
        for (int i = 0; i <= radial_segments; ++i) {
            const auto [u, x, z] = ring_corners(i);
            b.add({x * radius, y, -z * radius}, {x, 0.0F, -z}, {-z, 0.0F, -x, 1.0F},
                  {u, kOneThird + (v * kOneThird)});
            if (i > 0 && j > 0) {
                quad(b, prevrow + i - 1, prevrow + i, thisrow + i - 1, thisrow + i);
            }
        }
        prevrow = thisrow;
        thisrow = static_cast<int>(b.point);
    }

    // bottom hemisphere
    thisrow = static_cast<int>(b.point);
    prevrow = 0;
    for (int j = 0; j <= rings + 1; ++j) {
        const float v = static_cast<float>(j) / static_cast<float>(rings + 1);
        float w = 0.0F;
        float y = -1.0F;
        if (j != rings + 1) {
            const Vec2 cs = cos_sin(0.5F * kPi * v);
            w = cs.x;
            y = -cs.y;
        }
        for (int i = 0; i <= radial_segments; ++i) {
            const auto [u, x, z] = ring_corners(i);
            const Vec3 p{x * w, y, -z * w};
            b.add((p * radius) + Vec3{0.0F, (-0.5F * height) + radius, 0.0F}, p,
                  {-z, 0.0F, -x, 1.0F}, {u, kTwoThirds + (v * kOneThird)});
            if (i > 0 && j > 0) {
                quad(b, prevrow + i - 1, prevrow + i, thisrow + i - 1, thisrow + i);
            }
        }
        prevrow = thisrow;
        thisrow = static_cast<int>(b.point);
    }
    return flipped(std::move(b.mesh), capsule.flip_faces);
}

namespace {

// PlaneMesh::_create_mesh_array, which QuadMesh shares.
MeshData flat_mesh(Vec2 size, int subdivide_w, int subdivide_d, Vec3 center_offset,
                   const std::string& orientation, bool flip_faces) {
    Builder b;
    subdivide_w = std::max(subdivide_w, 0);
    subdivide_d = std::max(subdivide_d, 0);
    const Vec2 start_pos = size * -0.5F;
    Vec3 normal{0.0F, 1.0F, 0.0F};
    if (orientation == "x") {
        normal = {1.0F, 0.0F, 0.0F};
    } else if (orientation == "z") {
        normal = {0.0F, 0.0F, 1.0F};
    }
    const auto w1 = static_cast<float>(subdivide_w + 1);
    const auto d1 = static_cast<float>(subdivide_d + 1);
    float z = start_pos.y;
    int thisrow = 0;
    int prevrow = 0;
    for (int j = 0; j <= subdivide_d + 1; ++j) {
        float x = start_pos.x;
        for (int i = 0; i <= subdivide_w + 1; ++i) {
            const float u = static_cast<float>(i) / w1;
            const float v = static_cast<float>(j) / d1;
            Vec3 point{-x, 0.0F, -z};
            if (orientation == "x") {
                point = {0.0F, z, x};
            } else if (orientation == "z") {
                point = {-x, z, 0.0F};
            }
            const std::array<float, 4> tangent = orientation == "x"
                                                     ? std::array{0.0F, 0.0F, -1.0F, 1.0F}
                                                     : std::array{1.0F, 0.0F, 0.0F, 1.0F};
            // 1 - uv to match orientation with Quad, as Godot says.
            b.add(point + center_offset, normal, tangent, {1.0F - u, 1.0F - v});
            if (i > 0 && j > 0) {
                quad(b, prevrow + i - 1, prevrow + i, thisrow + i - 1, thisrow + i);
            }
            x += size.x / w1;
        }
        z += size.y / d1;
        prevrow = thisrow;
        thisrow = static_cast<int>(b.point);
    }
    return flipped(std::move(b.mesh), flip_faces);
}

} // namespace

MeshData plane_mesh(const world::PlaneMesh& plane) {
    return flat_mesh(plane.size, plane.subdivide_width, plane.subdivide_depth, plane.center_offset,
                     plane.orientation, plane.flip_faces);
}

MeshData quad_mesh(const world::QuadMesh& quad) {
    return flat_mesh(quad.size, quad.subdivide_width, quad.subdivide_depth, quad.center_offset,
                     quad.orientation, quad.flip_faces);
}

MeshData prism_mesh(const world::PrismMesh& prism) {
    Builder b;
    const Vec3 size = prism.size;
    const float left_to_right = prism.left_to_right;
    const int subdivide_w = std::max(prism.subdivide_width, 0);
    const int subdivide_h = std::max(prism.subdivide_height, 0);
    const int subdivide_d = std::max(prism.subdivide_depth, 0);
    const Vec3 start_pos = size * -0.5F;
    const auto w1 = static_cast<float>(subdivide_w + 1);
    const auto h1 = static_cast<float>(subdivide_h + 1);
    const auto d1 = static_cast<float>(subdivide_d + 1);

    // front + back
    float y = start_pos.y;
    int thisrow = 0;
    int prevrow = 0;
    for (int j = 0; j <= subdivide_h + 1; ++j) {
        const float scale = static_cast<float>(j) / h1;
        const float scaled_size_x = size.x * scale;
        const float start_x = start_pos.x + ((1.0F - scale) * size.x * left_to_right);
        const float offset_front = (1.0F - scale) * kOneThird * left_to_right;
        const float offset_back = (1.0F - scale) * kOneThird * (1.0F - left_to_right);
        const float v = static_cast<float>(j) / (2.0F * h1);
        float x = 0.0F;
        for (int i = 0; i <= subdivide_w + 1; ++i) {
            const float u = static_cast<float>(i) / (3.0F * w1) * scale;
            b.add({start_x + x, -y, -start_pos.z}, {0.0F, 0.0F, 1.0F}, {1.0F, 0.0F, 0.0F, 1.0F},
                  {offset_front + u, v});
            b.add({start_x + scaled_size_x - x, -y, start_pos.z}, {0.0F, 0.0F, -1.0F},
                  {-1.0F, 0.0F, 0.0F, 1.0F}, {kTwoThirds + offset_back + u, v});
            if (i > 0 && j == 1) {
                const int i2 = i * 2;
                b.triangle(prevrow + i2, thisrow + i2, thisrow + i2 - 2);
                b.triangle(prevrow + i2 + 1, thisrow + i2 + 1, thisrow + i2 - 1);
            } else if (i > 0 && j > 0) {
                const int i2 = i * 2;
                quad(b, prevrow + i2 - 2, prevrow + i2, thisrow + i2 - 2, thisrow + i2);
                quad(b, prevrow + i2 - 1, prevrow + i2 + 1, thisrow + i2 - 1, thisrow + i2 + 1);
            }
            x += scale * size.x / w1;
        }
        y += size.y / h1;
        prevrow = thisrow;
        thisrow = static_cast<int>(b.point);
    }

    // left + right
    const Vec3 normal_left = normalized({-size.y, size.x * left_to_right, 0.0F});
    const Vec3 normal_right = normalized({size.y, size.x * (1.0F - left_to_right), 0.0F});
    y = start_pos.y;
    thisrow = static_cast<int>(b.point);
    prevrow = 0;
    for (int j = 0; j <= subdivide_h + 1; ++j) {
        const float scale = static_cast<float>(j) / h1;
        const float left = start_pos.x + (size.x * (1.0F - scale) * left_to_right);
        const float right = left + (size.x * scale);
        const float v = static_cast<float>(j) / (2.0F * h1);
        float z = start_pos.z;
        for (int i = 0; i <= subdivide_d + 1; ++i) {
            const float u = static_cast<float>(i) / (3.0F * d1);
            b.add({right, -y, -z}, normal_right, {0.0F, 0.0F, -1.0F, 1.0F}, {kOneThird + u, v});
            b.add({left, -y, z}, normal_left, {0.0F, 0.0F, 1.0F, 1.0F}, {u, 0.5F + v});
            if (i > 0 && j > 0) {
                const int i2 = i * 2;
                quad(b, prevrow + i2 - 2, prevrow + i2, thisrow + i2 - 2, thisrow + i2);
                quad(b, prevrow + i2 - 1, prevrow + i2 + 1, thisrow + i2 - 1, thisrow + i2 + 1);
            }
            z += size.z / d1;
        }
        y += size.y / h1;
        prevrow = thisrow;
        thisrow = static_cast<int>(b.point);
    }

    // bottom
    float z = start_pos.z;
    thisrow = static_cast<int>(b.point);
    prevrow = 0;
    for (int j = 0; j <= subdivide_d + 1; ++j) {
        const float v = static_cast<float>(j) / (2.0F * d1);
        float x = start_pos.x;
        for (int i = 0; i <= subdivide_w + 1; ++i) {
            const float u = static_cast<float>(i) / (3.0F * w1);
            b.add({x, start_pos.y, -z}, {0.0F, -1.0F, 0.0F}, {1.0F, 0.0F, 0.0F, 1.0F},
                  {kTwoThirds + u, 0.5F + v});
            if (i > 0 && j > 0) {
                quad(b, prevrow + i - 1, prevrow + i, thisrow + i - 1, thisrow + i);
            }
            x += size.x / w1;
        }
        z += size.z / d1;
        prevrow = thisrow;
        thisrow = static_cast<int>(b.point);
    }
    return flipped(std::move(b.mesh), prism.flip_faces);
}

MeshData torus_mesh(const world::TorusMesh& torus) {
    Builder b;
    const int rings = std::max(torus.rings, 3);
    const int ring_segments = std::max(torus.ring_segments, 3);
    const float min_radius = std::min(torus.inner_radius, torus.outer_radius);
    const float max_radius = std::max(torus.inner_radius, torus.outer_radius);
    const float radius = (max_radius - min_radius) * 0.5F;
    for (int i = 0; i <= rings; ++i) {
        const int prevrow = (i - 1) * (ring_segments + 1);
        const int thisrow = i * (ring_segments + 1);
        const float inci = static_cast<float>(i) / static_cast<float>(rings);
        Vec2 normali{0.0F, -1.0F};
        if (i != rings) {
            const Vec2 cs = around(inci);
            normali = {-cs.y, -cs.x};
        }
        for (int j = 0; j <= ring_segments; ++j) {
            const float incj = static_cast<float>(j) / static_cast<float>(ring_segments);
            Vec2 normalj{-1.0F, 0.0F};
            if (j != ring_segments) {
                const Vec2 cs = around(incj);
                normalj = {-cs.x, cs.y};
            }
            const Vec2 normalk = (normalj * radius) + Vec2{min_radius + radius, 0.0F};
            b.add({normali.x * normalk.x, normalk.y, normali.y * normalk.x},
                  {normali.x * normalj.x, normalj.y, normali.y * normalj.x},
                  {normali.y, 0.0F, -normali.x, 1.0F}, {inci, incj});
            if (i > 0 && j > 0) {
                b.triangle(thisrow + j - 1, prevrow + j, prevrow + j - 1);
                b.triangle(thisrow + j - 1, thisrow + j, prevrow + j);
            }
        }
    }
    return flipped(std::move(b.mesh), torus.flip_faces);
}

namespace {

template <std::size_t I = 0>
void find_primitives(const world::World& world, entt::entity entity,
                     std::vector<std::size_t>& found, std::optional<Primitive>& first) {
    if constexpr (I < std::variant_size_v<Primitive>) {
        using T = std::variant_alternative_t<I, Primitive>;
        if (const T* component = world.ecs().try_get<T>(entity); component != nullptr) {
            found.push_back(I);
            if (!first) {
                first = Primitive{std::in_place_index<I>, *component};
            }
        }
        find_primitives<I + 1>(world, entity, found, first);
    }
}

} // namespace

Result<std::optional<Primitive>> primitive_of(const world::World& world, entt::entity entity) {
    std::vector<std::size_t> found;
    std::optional<Primitive> first;
    if (world.valid(entity)) {
        find_primitives(world, entity, found, first);
    }
    if (found.size() > 1) {
        std::string names;
        for (const std::size_t index : found) {
            names += (names.empty() ? "" : " and ") + std::string(kPrimitiveNames[index]);
        }
        return Error{ErrorCode::InvalidArgument, "an entity draws one mesh, but it has " + names};
    }
    return first;
}

Result<void> check_primitive(const Primitive& primitive) {
    if (const auto* torus = std::get_if<world::TorusMesh>(&primitive);
        torus != nullptr && torus->inner_radius == torus->outer_radius) {
        return Error{ErrorCode::InvalidArgument,
                     "a TorusMesh's inner_radius and outer_radius can't be the same"};
    }
    return {};
}

Result<MeshData> make_mesh(const Primitive& primitive) {
    if (auto checked = check_primitive(primitive); !checked) {
        return checked.error();
    }
    return std::visit(
        [](const auto& mesh) -> MeshData {
            using T = std::decay_t<decltype(mesh)>;
            if constexpr (std::is_same_v<T, world::BoxMesh>) {
                return box_mesh(mesh);
            } else if constexpr (std::is_same_v<T, world::SphereMesh>) {
                return sphere_mesh(mesh);
            } else if constexpr (std::is_same_v<T, world::CylinderMesh>) {
                return cylinder_mesh(mesh);
            } else if constexpr (std::is_same_v<T, world::CapsuleMesh>) {
                return capsule_mesh(mesh);
            } else if constexpr (std::is_same_v<T, world::PlaneMesh>) {
                return plane_mesh(mesh);
            } else if constexpr (std::is_same_v<T, world::QuadMesh>) {
                return quad_mesh(mesh);
            } else if constexpr (std::is_same_v<T, world::PrismMesh>) {
                return prism_mesh(mesh);
            } else {
                return torus_mesh(mesh);
            }
        },
        primitive);
}

} // namespace slash::scene3d
