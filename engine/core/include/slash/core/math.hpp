#pragma once

// Small value types for positions, directions, rotations and grid cells. They are plain
// aggregates so they can live in components and serialize as JSON arrays, e.g. a Vec3 is written
// as [x, y, z].
namespace slash {

struct Vec2 {
    float x = 0.0F;
    float y = 0.0F;

    friend constexpr bool operator==(const Vec2&, const Vec2&) = default;
    friend constexpr Vec2 operator+(Vec2 a, Vec2 b) { return {a.x + b.x, a.y + b.y}; }
    friend constexpr Vec2 operator-(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
    friend constexpr Vec2 operator*(Vec2 v, float s) { return {v.x * s, v.y * s}; }
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
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;

    friend constexpr bool operator==(const Vec3&, const Vec3&) = default;
    friend constexpr Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
    friend constexpr Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
    friend constexpr Vec3 operator*(Vec3 v, float s) { return {v.x * s, v.y * s, v.z * s}; }
};

struct Vec4 {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
    float w = 0.0F;

    friend constexpr bool operator==(const Vec4&, const Vec4&) = default;
};

// A rotation as a unit quaternion, written to JSON as [x, y, z, w]. The default is no rotation.
struct Quat {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
    float w = 1.0F;

    friend constexpr bool operator==(const Quat&, const Quat&) = default;
};

// The cosine and sine of an angle in radians, computed the same way on every platform, as
// {cos, sin}. Particles and UI use it instead of libm, whose results differ between platforms.
[[nodiscard]] Vec2 cos_sin(float radians);
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
