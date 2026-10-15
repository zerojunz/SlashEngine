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

} // namespace slash
