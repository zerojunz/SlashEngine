#pragma once

// Avoidance, as Godot 4.6's: optimal reciprocal collision avoidance (ORCA) from the RVO2 and
// RVO2-3D libraries, as Godot changes them in thirdparty/rvo2 (rvo2_2d and rvo2_3d): agents with
// heights, elevations, avoidance layers, masks and priorities, and static obstacles with heights,
// elevations and layers.
//
// Adapted from RVO2 and RVO2-3D, Copyright 2008 University of North Carolina at Chapel Hill
// (https://gamma.cs.unc.edu/RVO2/), by Jur van den Berg, Stephen J. Guy, Jamie Snape, Ming C. Lin
// and Dinesh Manocha.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
// except in compliance with the License. You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the
// License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
// either express or implied. See the License for the specific language governing permissions and
// limitations under the License.
//
// Changes: written in the engine's style, with indices in place of pointers; each step computes
// every agent's new velocity before it moves any, as RVO2's own RVOSimulator::doStep does, so the
// result doesn't depend on the order or the threads; and an obstacle edge split while building
// the obstacle tree keeps its obstacle's height, elevation and layers, which Godot's leaves at
// the defaults; and an agent right on top of another at the same velocity leaves it out rather
// than dividing by zero. The arithmetic is RVO2's in the same order (a vector divides by
// multiplying by the inverse), built without fused multiply-adds.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace slash::navigation::rvo {

inline constexpr std::size_t kNone = std::numeric_limits<std::size_t>::max();

inline float sqr(float a) {
    return a * a;
}

// ---- 2D, as rvo2_2d.

// RVO2's Vector2.
struct V2 {
    float x = 0.0F;
    float y = 0.0F;
};
inline V2 operator+(V2 a, V2 b) {
    return {a.x + b.x, a.y + b.y};
}
inline V2 operator-(V2 a, V2 b) {
    return {a.x - b.x, a.y - b.y};
}
inline V2 operator-(V2 a) {
    return {-a.x, -a.y};
}
inline V2 operator*(V2 a, float s) {
    return {a.x * s, a.y * s};
}
inline V2 operator*(float s, V2 a) {
    return {s * a.x, s * a.y};
}
inline V2 operator/(V2 a, float s) {
    const float inverse = 1.0F / s;
    return {a.x * inverse, a.y * inverse};
}
inline float dot(V2 a, V2 b) {
    return (a.x * b.x) + (a.y * b.y);
}
inline float abs_sq(V2 a) {
    return dot(a, a);
}
inline float abs(V2 a) {
    return std::sqrt(dot(a, a));
}
inline float det(V2 a, V2 b) {
    return (a.x * b.y) - (a.y * b.x);
}
inline V2 normalize(V2 a) {
    return a / abs(a);
}
// How far c is left of the line from a to b, times its length.
inline float left_of(V2 a, V2 b, V2 c) {
    return det(a - c, b - a);
}

struct Line {
    V2 point;
    V2 direction;
};

// One vertex of a static obstacle, and the edge from it to the next, as Obstacle2D.
struct Obstacle2D {
    bool convex = false;
    std::size_t next = 0;
    std::size_t previous = 0;
    V2 point;
    V2 unit_dir;
    float height = 1.0F;
    float elevation = 0.0F;
    std::uint32_t avoidance_layers = 1;
};

// An agent, as Agent2D. velocity is what it moved at last; step sets new_velocity, then
// velocity to it.
struct Agent2D {
    std::size_t max_neighbors = 0;
    float max_speed = 0.0F;
    float neighbor_dist = 0.0F;
    float radius = 0.0F;
    float time_horizon = 0.0F;
    float time_horizon_obst = 0.0F;
    V2 position;
    V2 pref_velocity;
    V2 velocity;
    V2 new_velocity;
    float height = 0.0F;
    float elevation = 0.0F;
    std::uint32_t avoidance_layers = 1;
    std::uint32_t avoidance_mask = 1;
    float avoidance_priority = 1.0F;
};

// RVOSimulator2D with its k-d trees: agents and static obstacles, stepped together.
class Simulation2D {
public:
    std::vector<Agent2D> agents;

    // Adds a static obstacle of at least two vertices, as Godot's _update_rvo_obstacles_tree:
    // agents keep to the right of each edge, so a counterclockwise outline keeps them out.
    void add_obstacle(const std::vector<V2>& vertices, float height, float elevation,
                      std::uint32_t avoidance_layers);
    // Builds the trees, computes each agent's new velocity over time_step seconds, then moves
    // each agent at it.
    void step(float time_step);

private:
    struct AgentTreeNode {
        std::size_t begin = 0;
        std::size_t end = 0;
        std::size_t left = 0;
        std::size_t right = 0;
        float max_x = 0.0F;
        float max_y = 0.0F;
        float min_x = 0.0F;
        float min_y = 0.0F;
    };
    struct ObstacleTreeNode {
        std::size_t obstacle = 0;
        std::size_t left = kNone;
        std::size_t right = kNone;
    };

    void build_agent_tree();
    void build_agent_tree_recursive(std::size_t begin, std::size_t end, std::size_t node);
    std::size_t build_obstacle_tree_recursive(const std::vector<std::size_t>& obstacles);
    void compute_neighbors(std::size_t agent);
    void compute_new_velocity(std::size_t agent);
    void insert_agent_neighbor(std::size_t agent, std::size_t other, float& range_sq);
    void insert_obstacle_neighbor(std::size_t agent, std::size_t obstacle, float range_sq);
    void query_agent_tree_recursive(std::size_t agent, float& range_sq, std::size_t node);
    void query_obstacle_tree_recursive(std::size_t agent, float range_sq, std::size_t node);

    float time_step_ = 0.0F;
    std::vector<Obstacle2D> obstacles_;
    std::size_t original_obstacles_ = 0; // those added, before the tree splits any
    std::vector<std::size_t> tree_agents_;
    std::vector<AgentTreeNode> agent_tree_;
    std::vector<ObstacleTreeNode> obstacle_tree_;
    std::size_t obstacle_root_ = kNone;
    // One agent's, while its new velocity is computed.
    std::vector<std::pair<float, std::size_t>> agent_neighbors_;
    std::vector<std::pair<float, std::size_t>> obstacle_neighbors_;
    std::vector<Line> orca_lines_;
};

// ---- 3D, as rvo2_3d.

// RVO2-3D's Vector3.
struct V3 {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;

    [[nodiscard]] float operator[](std::size_t i) const {
        if (i == 0) {
            return x;
        }
        return i == 1 ? y : z;
    }
};
inline V3 operator+(V3 a, V3 b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
inline V3 operator-(V3 a, V3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
inline V3 operator-(V3 a) {
    return {-a.x, -a.y, -a.z};
}
inline V3 operator*(V3 a, float s) {
    return {a.x * s, a.y * s, a.z * s};
}
inline V3 operator*(float s, V3 a) {
    return {s * a.x, s * a.y, s * a.z};
}
inline V3 operator/(V3 a, float s) {
    const float inverse = 1.0F / s;
    return {a.x * inverse, a.y * inverse, a.z * inverse};
}
inline float dot(V3 a, V3 b) {
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}
inline float abs_sq(V3 a) {
    return dot(a, a);
}
inline float abs(V3 a) {
    return std::sqrt(dot(a, a));
}
inline V3 cross(V3 a, V3 b) {
    return {(a.y * b.z) - (a.z * b.y), (a.z * b.x) - (a.x * b.z), (a.x * b.y) - (a.y * b.x)};
}
inline V3 normalize(V3 a) {
    return a / abs(a);
}

struct Plane {
    V3 point;
    V3 normal;
};

// An agent, as Agent3D: avoidance in every direction, with no static obstacles.
struct Agent3D {
    std::size_t max_neighbors = 0;
    float max_speed = 0.0F;
    float neighbor_dist = 0.0F;
    float radius = 0.0F;
    float time_horizon = 0.0F;
    V3 position;
    V3 pref_velocity;
    V3 velocity;
    V3 new_velocity;
    float height = 1.0F;
    std::uint32_t avoidance_layers = 1;
    std::uint32_t avoidance_mask = 1;
    float avoidance_priority = 1.0F;
};

// RVOSimulator3D with its k-d tree.
class Simulation3D {
public:
    std::vector<Agent3D> agents;

    void step(float time_step);

private:
    struct AgentTreeNode {
        std::size_t begin = 0;
        std::size_t end = 0;
        std::size_t left = 0;
        std::size_t right = 0;
        V3 max_coord;
        V3 min_coord;
    };

    void build_agent_tree_recursive(std::size_t begin, std::size_t end, std::size_t node);
    void compute_new_velocity(std::size_t agent);
    void insert_agent_neighbor(std::size_t agent, std::size_t other, float& range_sq);
    void query_agent_tree_recursive(std::size_t agent, float& range_sq, std::size_t node);

    float time_step_ = 0.0F;
    std::vector<std::size_t> tree_agents_;
    std::vector<AgentTreeNode> agent_tree_;
    std::vector<std::pair<float, std::size_t>> agent_neighbors_;
    std::vector<Plane> orca_planes_;
};

} // namespace slash::navigation::rvo
