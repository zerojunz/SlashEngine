#pragma once

// Small value types for positions, directions, rotations and grid cells. They are plain
// aggregates so they can live in components and serialize as JSON arrays, e.g. a Vec3 is written
// as [x, y, z].
namespace slash {

// The type of a coordinate, as Godot's real_t: a float, or a double in engines built with
// SLASH_PRECISION=double (SLASH_REAL_T_IS_DOUBLE), for worlds that reach far from the origin.
#if defined(SLASH_REAL_T_IS_DOUBLE)
using real_t = double;
#else
using real_t = float;
#endif

struct Vec2 {
    real_t x = 0.0F;
    real_t y = 0.0F;

    friend constexpr bool operator==(const Vec2&, const Vec2&) = default;
    friend constexpr Vec2 operator+(Vec2 a, Vec2 b) { return {a.x + b.x, a.y + b.y}; }
    friend constexpr Vec2 operator-(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
    friend constexpr Vec2 operator*(Vec2 v, real_t s) { return {v.x * s, v.y * s}; }
};

// A pair of integers, such as a cell of a tile map, written to JSON as [x, y], as Godot's
// Vector2i.
struct Vec2i {
    int x = 0;
    int y = 0;

    friend constexpr bool operator==(const Vec2i&, const Vec2i&) = default;
    friend constexpr Vec2i operator+(Vec2i a, Vec2i b) { return {a.x + b.x, a.y + b.y}; }
    friend constexpr Vec2i operator-(Vec2i a, Vec2i b) { return {a.x - b.x, a.y - b.y}; }
};

// Three integers, such as a cell of a GridMap, written to JSON as [x, y, z], as Godot's
// Vector3i.
struct Vec3i {
    int x = 0;
    int y = 0;
    int z = 0;

    friend constexpr bool operator==(const Vec3i&, const Vec3i&) = default;
    friend constexpr Vec3i operator+(Vec3i a, Vec3i b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
    friend constexpr Vec3i operator-(Vec3i a, Vec3i b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
};

struct Vec3 {
    real_t x = 0.0F;
    real_t y = 0.0F;
    real_t z = 0.0F;

    friend constexpr bool operator==(const Vec3&, const Vec3&) = default;
    friend constexpr Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
    friend constexpr Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
    friend constexpr Vec3 operator*(Vec3 v, real_t s) { return {v.x * s, v.y * s, v.z * s}; }
};

#if defined(SLASH_REAL_T_IS_DOUBLE)
// Vectors of floats, for what the render device takes and math done in floats on every build,
// such as particles' and their CPU twins'. In a double-precision engine they are types of their
// own that widen to Vec2 and Vec3 by themselves; narrowing takes to_float. Elsewhere they are
// Vec2 and Vec3.
struct Vec2f {
    float x = 0.0F;
    float y = 0.0F;

    friend constexpr bool operator==(const Vec2f&, const Vec2f&) = default;
    friend constexpr Vec2f operator+(Vec2f a, Vec2f b) { return {a.x + b.x, a.y + b.y}; }
    friend constexpr Vec2f operator-(Vec2f a, Vec2f b) { return {a.x - b.x, a.y - b.y}; }
    friend constexpr Vec2f operator*(Vec2f v, float s) { return {v.x * s, v.y * s}; }
    constexpr operator Vec2() const { return {x, y}; } // NOLINT(google-explicit-constructor)
};

struct Vec3f {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;

    friend constexpr bool operator==(const Vec3f&, const Vec3f&) = default;
    friend constexpr Vec3f operator+(Vec3f a, Vec3f b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
    friend constexpr Vec3f operator-(Vec3f a, Vec3f b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
    friend constexpr Vec3f operator*(Vec3f v, float s) { return {v.x * s, v.y * s, v.z * s}; }
    constexpr operator Vec3() const { return {x, y, z}; } // NOLINT(google-explicit-constructor)
};

// A vector rounded to floats.
[[nodiscard]] constexpr Vec2f to_float(Vec2 v) {
    return {static_cast<float>(v.x), static_cast<float>(v.y)};
}
[[nodiscard]] constexpr Vec3f to_float(Vec3 v) {
    return {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)};
}
// A coordinate rounded to a float.
[[nodiscard]] constexpr float to_float(double v) {
    return static_cast<float>(v);
}
#else
using Vec2f = Vec2;
using Vec3f = Vec3;

// A vector rounded to floats: itself in a single-precision engine.
[[nodiscard]] constexpr Vec2f to_float(Vec2 v) {
    return v;
}
[[nodiscard]] constexpr Vec3f to_float(Vec3 v) {
    return v;
}
// A coordinate rounded to a float: itself in a single-precision engine.
[[nodiscard]] constexpr float to_float(float v) {
    return v;
}
#endif

struct Vec4 {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
    float w = 0.0F;

    friend constexpr bool operator==(const Vec4&, const Vec4&) = default;
};

// A rotation as a unit quaternion, written to JSON as [x, y, z, w]. The default is no rotation.
struct Quat {
    real_t x = 0.0F;
    real_t y = 0.0F;
    real_t z = 0.0F;
    real_t w = 1.0F;

    friend constexpr bool operator==(const Quat&, const Quat&) = default;
};

// The cosine and sine of an angle in radians, computed the same way on every platform, as
// {cos, sin}. Particles and UI use it instead of libm, whose results differ between platforms.
[[nodiscard]] Vec2f cos_sin(float radians);
// The angle of the point [x, y] from the x axis, in radians from -pi to pi, as std::atan2 but
// the same bits on every platform; 0 for [0, 0].
[[nodiscard]] float atan2(float y, float x);
// e to the power x, as std::exp but the same bits on every platform: infinity past what a float
// holds and 0 below, NaN for NaN.
[[nodiscard]] float exp(float x);
// The natural logarithm of x, as std::log but the same bits on every platform: -infinity for 0,
// NaN below 0 and for NaN.
[[nodiscard]] float ln(float x);

} // namespace slash
