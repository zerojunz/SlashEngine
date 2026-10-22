// RVO2's 2D agents, k-d tree and linear programs (Agent2d.cpp, KdTree2d.cpp), as Godot 4.6
// changes them in thirdparty/rvo2/rvo2_2d. See rvo2.hpp for the license and the changes.

#include "rvo2.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <utility>
#include <vector>

namespace slash::navigation::rvo {
namespace {

constexpr float kEpsilon = 0.00001F; // RVO_EPSILON
constexpr std::size_t kMaxLeafSize = 10;
constexpr float kInfinity = std::numeric_limits<float>::infinity();

float dist_sq_point_line_segment(V2 a, V2 b, V2 c) {
    const float r = dot(c - a, b - a) / abs_sq(b - a);
    if (r < 0.0F) {
        return abs_sq(c - a);
    }
    if (r > 1.0F) {
        return abs_sq(c - b);
    }
    return abs_sq(c - (a + (r * (b - a))));
}

// The leg of the velocity obstacle a vertex at relative position p makes for radius r, as
// Agent2D::computeNewVelocity writes it out: left and right.
V2 left_leg(V2 p, float dist_sq, float r) {
    const float leg = std::sqrt(dist_sq - sqr(r));
    return V2{(p.x * leg) - (p.y * r), (p.x * r) + (p.y * leg)} / dist_sq;
}
V2 right_leg(V2 p, float dist_sq, float r) {
    const float leg = std::sqrt(dist_sq - sqr(r));
    return V2{(p.x * leg) + (p.y * r), (-p.x * r) + (p.y * leg)} / dist_sq;
}

// linearProgram1: the best point of line line_no within the circle and the lines before it.
bool linear_program_1(const std::vector<Line>& lines, std::size_t line_no, float radius,
                      V2 opt_velocity, bool direction_opt, V2& result) {
    const Line& line = lines[line_no];
    const float dot_product = dot(line.point, line.direction);
    const float discriminant = sqr(dot_product) + sqr(radius) - abs_sq(line.point);
    if (discriminant < 0.0F) {
        // The max speed circle fully invalidates the line.
        return false;
    }
    const float sqrt_discriminant = std::sqrt(discriminant);
    float t_left = -dot_product - sqrt_discriminant;
    float t_right = -dot_product + sqrt_discriminant;
    for (std::size_t i = 0; i < line_no; ++i) {
        const float denominator = det(line.direction, lines[i].direction);
        const float numerator = det(lines[i].direction, line.point - lines[i].point);
        if (std::fabs(denominator) <= kEpsilon) {
            // The lines are (almost) parallel.
            if (numerator < 0.0F) {
                return false;
            }
            continue;
        }
        const float t = numerator / denominator;
        if (denominator >= 0.0F) {
            // Line i bounds the line on the right.
            t_right = std::min(t_right, t);
        } else {
            // Line i bounds the line on the left.
            t_left = std::max(t_left, t);
        }
        if (t_left > t_right) {
            return false;
        }
    }
    if (direction_opt) {
        // Optimize the direction: the extreme it points to.
        result = dot(opt_velocity, line.direction) > 0.0F ? line.point + (t_right * line.direction)
                                                          : line.point + (t_left * line.direction);
    } else {
        // Optimize the closest point.
        const float t = dot(line.direction, opt_velocity - line.point);
        if (t < t_left) {
            result = line.point + (t_left * line.direction);
        } else if (t > t_right) {
            result = line.point + (t_right * line.direction);
        } else {
            result = line.point + (t * line.direction);
        }
    }
    return true;
}

// linearProgram2: the best velocity within the circle and every line; the number of the line
// it fails on, or the number of lines.
std::size_t linear_program_2(const std::vector<Line>& lines, float radius, V2 opt_velocity,
                             bool direction_opt, V2& result) {
    if (direction_opt) {
        // Optimize the direction; opt_velocity is of unit length.
        result = opt_velocity * radius;
    } else if (abs_sq(opt_velocity) > sqr(radius)) {
        // Optimize the closest point, outside the circle.
        result = normalize(opt_velocity) * radius;
    } else {
        // Optimize the closest point, inside the circle.
        result = opt_velocity;
    }
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (det(lines[i].direction, lines[i].point - result) > 0.0F) {
            // The result breaks line i: compute a new one.
            const V2 temp_result = result;
            if (!linear_program_1(lines, i, radius, opt_velocity, direction_opt, result)) {
                result = temp_result;
                return i;
            }
        }
    }
    return lines.size();
}

// linearProgram3: when the lines leave no velocity, the one that breaks the agents' lines the
// least, keeping the obstacles' lines (the first num_obst_lines).
void linear_program_3(const std::vector<Line>& lines, std::size_t num_obst_lines,
                      std::size_t begin_line, float radius, V2& result) {
    float distance = 0.0F;
    std::vector<Line> proj_lines;
    for (std::size_t i = begin_line; i < lines.size(); ++i) {
        if (det(lines[i].direction, lines[i].point - result) <= distance) {
            continue;
        }
        // The result breaks line i by more than the distance so far.
        proj_lines.assign(lines.begin(),
                          std::next(lines.begin(), static_cast<std::ptrdiff_t>(num_obst_lines)));
        for (std::size_t j = num_obst_lines; j < i; ++j) {
            Line line;
            const float determinant = det(lines[i].direction, lines[j].direction);
            if (std::fabs(determinant) <= kEpsilon) {
                // Lines i and j are parallel.
                if (dot(lines[i].direction, lines[j].direction) > 0.0F) {
                    // They point the same way.
                    continue;
                }
                // They point opposite ways.
                line.point = 0.5F * (lines[i].point + lines[j].point);
            } else {
                line.point =
                    lines[i].point +
                    ((det(lines[j].direction, lines[i].point - lines[j].point) / determinant) *
                     lines[i].direction);
            }
            line.direction = normalize(lines[j].direction - lines[i].direction);
            proj_lines.push_back(line);
        }
        const V2 temp_result = result;
        if (linear_program_2(proj_lines, radius, V2{-lines[i].direction.y, lines[i].direction.x},
                             true, result) < proj_lines.size()) {
            // In principle this can't happen: the result is in the program's feasible region
            // already. A failure is a small floating point error, so the result is kept.
            result = temp_result;
        }
        distance = det(lines[i].direction, lines[i].point - result);
    }
}

} // namespace

void Simulation2D::add_obstacle(const std::vector<V2>& vertices, float height, float elevation,
                                std::uint32_t avoidance_layers) {
    // Godot's NavMap2D::_update_rvo_obstacles_tree, after RVOSimulator::addObstacle.
    if (vertices.size() < 2) {
        return;
    }
    const std::size_t first = obstacles_.size();
    const std::size_t count = vertices.size();
    for (std::size_t i = 0; i < count; ++i) {
        Obstacle2D obstacle;
        obstacle.point = vertices[i];
        obstacle.height = height;
        obstacle.elevation = elevation;
        obstacle.avoidance_layers = avoidance_layers;
        const std::size_t index = obstacles_.size();
        if (i != 0) {
            obstacle.previous = index - 1;
            obstacles_[index - 1].next = index;
        }
        if (i == count - 1) {
            obstacle.next = first;
            obstacles_[first].previous = index;
        }
        obstacle.unit_dir = normalize(vertices[i == count - 1 ? 0 : i + 1] - vertices[i]);
        if (count == 2) {
            obstacle.convex = true;
        } else {
            obstacle.convex = left_of(vertices[i == 0 ? count - 1 : i - 1], vertices[i],
                                      vertices[i == count - 1 ? 0 : i + 1]) >= 0.0F;
        }
        obstacles_.push_back(obstacle);
        if (i == count - 1) {
            obstacles_[first].previous = index;
        }
    }
    original_obstacles_ = obstacles_.size();
}

void Simulation2D::step(float time_step) {
    time_step_ = time_step;
    // KdTree2D::buildObstacleTree, from the obstacles as added.
    obstacles_.resize(original_obstacles_);
    obstacle_tree_.clear();
    std::vector<std::size_t> all(obstacles_.size());
    for (std::size_t i = 0; i < all.size(); ++i) {
        all[i] = i;
    }
    obstacle_root_ = build_obstacle_tree_recursive(all);
    build_agent_tree();
    for (std::size_t i = 0; i < agents.size(); ++i) {
        compute_neighbors(i);
        compute_new_velocity(i);
    }
    // Agent2D::update.
    for (Agent2D& agent : agents) {
        agent.velocity = agent.new_velocity;
        agent.position = agent.position + (agent.velocity * time_step_);
    }
}

void Simulation2D::build_agent_tree() {
    tree_agents_.resize(agents.size());
    for (std::size_t i = 0; i < agents.size(); ++i) {
        tree_agents_[i] = i;
    }
    agent_tree_.clear();
    if (!agents.empty()) {
        agent_tree_.resize((2 * agents.size()) - 1);
        build_agent_tree_recursive(0, agents.size(), 0);
    }
}

void Simulation2D::build_agent_tree_recursive(std::size_t begin, std::size_t end,
                                              std::size_t node) {
    AgentTreeNode& n = agent_tree_[node];
    n.begin = begin;
    n.end = end;
    const V2 first = agents[tree_agents_[begin]].position;
    n.min_x = n.max_x = first.x;
    n.min_y = n.max_y = first.y;
    for (std::size_t i = begin + 1; i < end; ++i) {
        const V2 p = agents[tree_agents_[i]].position;
        n.max_x = std::max(n.max_x, p.x);
        n.min_x = std::min(n.min_x, p.x);
        n.max_y = std::max(n.max_y, p.y);
        n.min_y = std::min(n.min_y, p.y);
    }
    if (end - begin <= kMaxLeafSize) {
        return;
    }
    // Not a leaf: split along the longer side.
    const bool vertical = n.max_x - n.min_x > n.max_y - n.min_y;
    const float split = vertical ? 0.5F * (n.max_x + n.min_x) : 0.5F * (n.max_y + n.min_y);
    const auto coord = [&](std::size_t i) {
        const V2 p = agents[tree_agents_[i]].position;
        return vertical ? p.x : p.y;
    };
    std::size_t left = begin;
    std::size_t right = end;
    while (left < right) {
        while (left < right && coord(left) < split) {
            ++left;
        }
        while (right > left && coord(right - 1) >= split) {
            --right;
        }
        if (left < right) {
            std::swap(tree_agents_[left], tree_agents_[right - 1]);
            ++left;
            --right;
        }
    }
    if (left == begin) {
        ++left;
        ++right;
    }
    n.left = node + 1;
    n.right = node + (2 * (left - begin));
    const std::size_t left_node = n.left;
    const std::size_t right_node = n.right;
    build_agent_tree_recursive(begin, left, left_node);
    build_agent_tree_recursive(left, end, right_node);
}

std::size_t Simulation2D::build_obstacle_tree_recursive(const std::vector<std::size_t>& obstacles) {
    if (obstacles.empty()) {
        return kNone;
    }
    // The edge that splits the others into the most even sides, the fewest split.
    std::size_t optimal_split = 0;
    std::size_t min_left = obstacles.size();
    std::size_t min_right = obstacles.size();
    for (std::size_t i = 0; i < obstacles.size(); ++i) {
        std::size_t left_size = 0;
        std::size_t right_size = 0;
        const V2 i1 = obstacles_[obstacles[i]].point;
        const V2 i2 = obstacles_[obstacles_[obstacles[i]].next].point;
        for (std::size_t j = 0; j < obstacles.size(); ++j) {
            if (i == j) {
                continue;
            }
            const V2 j1 = obstacles_[obstacles[j]].point;
            const V2 j2 = obstacles_[obstacles_[obstacles[j]].next].point;
            const float j1_left_of_i = left_of(i1, i2, j1);
            const float j2_left_of_i = left_of(i1, i2, j2);
            if (j1_left_of_i >= -kEpsilon && j2_left_of_i >= -kEpsilon) {
                ++left_size;
            } else if (j1_left_of_i <= kEpsilon && j2_left_of_i <= kEpsilon) {
                ++right_size;
            } else {
                ++left_size;
                ++right_size;
            }
            if (std::pair(std::max(left_size, right_size), std::min(left_size, right_size)) >=
                std::pair(std::max(min_left, min_right), std::min(min_left, min_right))) {
                break;
            }
        }
        if (std::pair(std::max(left_size, right_size), std::min(left_size, right_size)) <
            std::pair(std::max(min_left, min_right), std::min(min_left, min_right))) {
            min_left = left_size;
            min_right = right_size;
            optimal_split = i;
        }
    }

    // The split node.
    std::vector<std::size_t> left_obstacles(min_left);
    std::vector<std::size_t> right_obstacles(min_right);
    std::size_t left_counter = 0;
    std::size_t right_counter = 0;
    const std::size_t i = optimal_split;
    const std::size_t split_obstacle = obstacles[i];
    const V2 i1 = obstacles_[split_obstacle].point;
    const V2 i2 = obstacles_[obstacles_[split_obstacle].next].point;
    for (std::size_t j = 0; j < obstacles.size(); ++j) {
        if (i == j) {
            continue;
        }
        const std::size_t obstacle_j1 = obstacles[j];
        const std::size_t obstacle_j2 = obstacles_[obstacle_j1].next;
        const V2 j1 = obstacles_[obstacle_j1].point;
        const V2 j2 = obstacles_[obstacle_j2].point;
        const float j1_left_of_i = left_of(i1, i2, j1);
        const float j2_left_of_i = left_of(i1, i2, j2);
        if (j1_left_of_i >= -kEpsilon && j2_left_of_i >= -kEpsilon) {
            left_obstacles[left_counter++] = obstacle_j1;
        } else if (j1_left_of_i <= kEpsilon && j2_left_of_i <= kEpsilon) {
            right_obstacles[right_counter++] = obstacle_j1;
        } else {
            // Split edge j where edge i's line crosses it.
            const float t = det(i2 - i1, j1 - i1) / det(i2 - i1, j1 - j2);
            Obstacle2D split = obstacles_[obstacle_j1];
            split.point = j1 + (t * (j2 - j1));
            split.previous = obstacle_j1;
            split.next = obstacle_j2;
            split.convex = true;
            const std::size_t added = obstacles_.size();
            obstacles_.push_back(split);
            obstacles_[obstacle_j1].next = added;
            obstacles_[obstacle_j2].previous = added;
            if (j1_left_of_i > 0.0F) {
                left_obstacles[left_counter++] = obstacle_j1;
                right_obstacles[right_counter++] = added;
            } else {
                right_obstacles[right_counter++] = obstacle_j1;
                left_obstacles[left_counter++] = added;
            }
        }
    }
    const std::size_t node = obstacle_tree_.size();
    obstacle_tree_.push_back({.obstacle = split_obstacle});
    const std::size_t left = build_obstacle_tree_recursive(left_obstacles);
    const std::size_t right = build_obstacle_tree_recursive(right_obstacles);
    obstacle_tree_[node].left = left;
    obstacle_tree_[node].right = right;
    return node;
}

void Simulation2D::compute_neighbors(std::size_t agent) {
    // Agent2D::computeNeighbors.
    const Agent2D& a = agents[agent];
    obstacle_neighbors_.clear();
    float range_sq = sqr((a.time_horizon_obst * a.max_speed) + a.radius);
    query_obstacle_tree_recursive(agent, range_sq, obstacle_root_);
    agent_neighbors_.clear();
    if (a.max_neighbors > 0) {
        range_sq = sqr(a.neighbor_dist);
        query_agent_tree_recursive(agent, range_sq, 0);
    }
}

void Simulation2D::compute_new_velocity(std::size_t agent) {
    // Agent2D::computeNewVelocity: the velocity closest to the preferred one that keeps out of
    // every obstacle's and neighbor's velocity obstacle.
    Agent2D& a = agents[agent];
    const V2 position = a.position;
    const V2 velocity = a.velocity;
    const float radius = a.radius;
    orca_lines_.clear();
    const float inv_time_horizon_obst = 1.0F / a.time_horizon_obst;

    // Obstacle ORCA lines.
    for (const auto& [distance, neighbor] : obstacle_neighbors_) {
        std::size_t obstacle1 = neighbor;
        std::size_t obstacle2 = obstacles_[obstacle1].next;
        const V2 relative_position1 = obstacles_[obstacle1].point - position;
        const V2 relative_position2 = obstacles_[obstacle2].point - position;

        // Whether earlier obstacle lines already take care of its velocity obstacle.
        bool already_covered = false;
        for (const Line& line : orca_lines_) {
            if (det((inv_time_horizon_obst * relative_position1) - line.point, line.direction) -
                        (inv_time_horizon_obst * radius) >=
                    -kEpsilon &&
                det((inv_time_horizon_obst * relative_position2) - line.point, line.direction) -
                        (inv_time_horizon_obst * radius) >=
                    -kEpsilon) {
                already_covered = true;
                break;
            }
        }
        if (already_covered) {
            continue;
        }

        // Not covered yet: check for collisions.
        const float dist_sq1 = abs_sq(relative_position1);
        const float dist_sq2 = abs_sq(relative_position2);
        const float radius_sq = sqr(radius);
        const V2 obstacle_vector = obstacles_[obstacle2].point - obstacles_[obstacle1].point;
        const float s = dot(-relative_position1, obstacle_vector) / abs_sq(obstacle_vector);
        const float dist_sq_line = abs_sq(-relative_position1 - (s * obstacle_vector));
        Line line;
        if (s < 0.0F && dist_sq1 <= radius_sq) {
            // Collision with the left vertex; ignored if it isn't convex.
            if (obstacles_[obstacle1].convex) {
                line.point = V2{0.0F, 0.0F};
                line.direction = normalize(V2{-relative_position1.y, relative_position1.x});
                orca_lines_.push_back(line);
            }
            continue;
        }
        if (s > 1.0F && dist_sq2 <= radius_sq) {
            // Collision with the right vertex; ignored if it isn't convex or the neighboring
            // edge takes care of it.
            if (obstacles_[obstacle2].convex &&
                det(relative_position2, obstacles_[obstacle2].unit_dir) >= 0.0F) {
                line.point = V2{0.0F, 0.0F};
                line.direction = normalize(V2{-relative_position2.y, relative_position2.x});
                orca_lines_.push_back(line);
            }
            continue;
        }
        if (s >= 0.0F && s < 1.0F && dist_sq_line <= radius_sq) {
            // Collision with the edge.
            line.point = V2{0.0F, 0.0F};
            line.direction = -obstacles_[obstacle1].unit_dir;
            orca_lines_.push_back(line);
            continue;
        }

        // No collision: the legs. Seen obliquely, both legs can come from one vertex; legs
        // extend the cut-off line at a vertex that isn't convex.
        V2 left_leg_direction;
        V2 right_leg_direction;
        if (s < 0.0F && dist_sq_line <= radius_sq) {
            // Seen obliquely so the left vertex makes the velocity obstacle.
            if (!obstacles_[obstacle1].convex) {
                continue;
            }
            obstacle2 = obstacle1;
            left_leg_direction = left_leg(relative_position1, dist_sq1, radius);
            right_leg_direction = right_leg(relative_position1, dist_sq1, radius);
        } else if (s > 1.0F && dist_sq_line <= radius_sq) {
            // Seen obliquely so the right vertex makes the velocity obstacle.
            if (!obstacles_[obstacle2].convex) {
                continue;
            }
            obstacle1 = obstacle2;
            left_leg_direction = left_leg(relative_position2, dist_sq2, radius);
            right_leg_direction = right_leg(relative_position2, dist_sq2, radius);
        } else {
            // The usual case.
            left_leg_direction = obstacles_[obstacle1].convex
                                     ? left_leg(relative_position1, dist_sq1, radius)
                                     : -obstacles_[obstacle1].unit_dir;
            right_leg_direction = obstacles_[obstacle2].convex
                                      ? right_leg(relative_position2, dist_sq2, radius)
                                      : obstacles_[obstacle1].unit_dir;
        }

        // A leg can't point into the neighboring edge at a convex vertex: that edge's cut-off
        // line is taken instead, and a velocity projected on such a "foreign" leg adds no line.
        const std::size_t left_neighbor = obstacles_[obstacle1].previous;
        bool left_leg_foreign = false;
        bool right_leg_foreign = false;
        if (obstacles_[obstacle1].convex &&
            det(left_leg_direction, -obstacles_[left_neighbor].unit_dir) >= 0.0F) {
            // The left leg points into the obstacle.
            left_leg_direction = -obstacles_[left_neighbor].unit_dir;
            left_leg_foreign = true;
        }
        if (obstacles_[obstacle2].convex &&
            det(right_leg_direction, obstacles_[obstacle2].unit_dir) <= 0.0F) {
            // The right leg points into the obstacle.
            right_leg_direction = obstacles_[obstacle2].unit_dir;
            right_leg_foreign = true;
        }

        // The cut-off centers.
        const V2 left_cutoff = inv_time_horizon_obst * (obstacles_[obstacle1].point - position);
        const V2 right_cutoff = inv_time_horizon_obst * (obstacles_[obstacle2].point - position);
        const V2 cutoff_vec = right_cutoff - left_cutoff;

        // Project the current velocity on the velocity obstacle, checking the cut-off circles
        // first.
        const float t = obstacle1 == obstacle2
                            ? 0.5F
                            : dot(velocity - left_cutoff, cutoff_vec) / abs_sq(cutoff_vec);
        const float t_left = dot(velocity - left_cutoff, left_leg_direction);
        const float t_right = dot(velocity - right_cutoff, right_leg_direction);
        if ((t < 0.0F && t_left < 0.0F) ||
            (obstacle1 == obstacle2 && t_left < 0.0F && t_right < 0.0F)) {
            // On the left cut-off circle.
            const V2 unit_w = normalize(velocity - left_cutoff);
            line.direction = V2{unit_w.y, -unit_w.x};
            line.point = left_cutoff + (radius * inv_time_horizon_obst * unit_w);
            orca_lines_.push_back(line);
            continue;
        }
        if (t > 1.0F && t_right < 0.0F) {
            // On the right cut-off circle.
            const V2 unit_w = normalize(velocity - right_cutoff);
            line.direction = V2{unit_w.y, -unit_w.x};
            line.point = right_cutoff + (radius * inv_time_horizon_obst * unit_w);
            orca_lines_.push_back(line);
            continue;
        }

        // On the left leg, the right leg or the cut-off line, whichever is closest.
        const float dist_sq_cutoff = (t < 0.0F || t > 1.0F || obstacle1 == obstacle2)
                                         ? kInfinity
                                         : abs_sq(velocity - (left_cutoff + (t * cutoff_vec)));
        const float dist_sq_left =
            t_left < 0.0F ? kInfinity
                          : abs_sq(velocity - (left_cutoff + (t_left * left_leg_direction)));
        const float dist_sq_right =
            t_right < 0.0F ? kInfinity
                           : abs_sq(velocity - (right_cutoff + (t_right * right_leg_direction)));
        if (dist_sq_cutoff <= dist_sq_left && dist_sq_cutoff <= dist_sq_right) {
            // On the cut-off line.
            line.direction = -obstacles_[obstacle1].unit_dir;
            line.point = left_cutoff +
                         (radius * inv_time_horizon_obst * V2{-line.direction.y, line.direction.x});
            orca_lines_.push_back(line);
            continue;
        }
        if (dist_sq_left <= dist_sq_right) {
            // On the left leg.
            if (left_leg_foreign) {
                continue;
            }
            line.direction = left_leg_direction;
            line.point = left_cutoff +
                         (radius * inv_time_horizon_obst * V2{-line.direction.y, line.direction.x});
            orca_lines_.push_back(line);
            continue;
        }
        // On the right leg.
        if (right_leg_foreign) {
            continue;
        }
        line.direction = -right_leg_direction;
        line.point = right_cutoff +
                     (radius * inv_time_horizon_obst * V2{-line.direction.y, line.direction.x});
        orca_lines_.push_back(line);
    }

    const std::size_t num_obst_lines = orca_lines_.size();
    const float inv_time_horizon = 1.0F / a.time_horizon;

    // Agent ORCA lines.
    for (const auto& [distance, neighbor] : agent_neighbors_) {
        const Agent2D& other = agents[neighbor];
        const V2 relative_position = other.position - position;
        const V2 relative_velocity = velocity - other.velocity;
        const float dist_sq = abs_sq(relative_position);
        const float combined_radius = radius + other.radius;
        const float combined_radius_sq = sqr(combined_radius);
        Line line;
        V2 u;
        if (dist_sq > combined_radius_sq) {
            // No collision. w goes from the cut-off center to the relative velocity.
            const V2 w = relative_velocity - (inv_time_horizon * relative_position);
            const float w_length_sq = abs_sq(w);
            const float dot_product1 = dot(w, relative_position);
            if (dot_product1 < 0.0F && sqr(dot_product1) > combined_radius_sq * w_length_sq) {
                // On the cut-off circle.
                const float w_length = std::sqrt(w_length_sq);
                const V2 unit_w = w / w_length;
                line.direction = V2{unit_w.y, -unit_w.x};
                u = ((combined_radius * inv_time_horizon) - w_length) * unit_w;
            } else {
                // On the legs.
                const float leg = std::sqrt(dist_sq - combined_radius_sq);
                if (det(relative_position, w) > 0.0F) {
                    // On the left leg.
                    line.direction =
                        V2{(relative_position.x * leg) - (relative_position.y * combined_radius),
                           (relative_position.x * combined_radius) + (relative_position.y * leg)} /
                        dist_sq;
                } else {
                    // On the right leg.
                    line.direction =
                        -V2{(relative_position.x * leg) + (relative_position.y * combined_radius),
                            (-relative_position.x * combined_radius) +
                                (relative_position.y * leg)} /
                        dist_sq;
                }
                const float dot_product2 = dot(relative_velocity, line.direction);
                u = (dot_product2 * line.direction) - relative_velocity;
            }
        } else {
            // Collision: on the cut-off circle of the time step.
            const float inv_time_step = 1.0F / time_step_;
            const V2 w = relative_velocity - (inv_time_step * relative_position);
            const float w_length = abs(w);
            if (w_length == 0.0F) {
                // Right on top of it at the same velocity: there is no way out to take, and
                // RVO2 would divide by zero. It is left out.
                continue;
            }
            const V2 unit_w = w / w_length;
            line.direction = V2{unit_w.y, -unit_w.x};
            u = ((combined_radius * inv_time_step) - w_length) * unit_w;
        }
        line.point = velocity + (0.5F * u);
        orca_lines_.push_back(line);
    }

    const std::size_t line_fail =
        linear_program_2(orca_lines_, a.max_speed, a.pref_velocity, false, a.new_velocity);
    if (line_fail < orca_lines_.size()) {
        linear_program_3(orca_lines_, num_obst_lines, line_fail, a.max_speed, a.new_velocity);
    }
}

void Simulation2D::insert_agent_neighbor(std::size_t agent, std::size_t other, float& range_sq) {
    // Agent2D::insertAgentNeighbor, with Godot's checks.
    if (agent == other) {
        return;
    }
    const Agent2D& a = agents[agent];
    const Agent2D& b = agents[other];
    // Not on a layer it avoids.
    if ((a.avoidance_mask & b.avoidance_layers) == 0) {
        return;
    }
    // Above or below it.
    if (a.elevation > b.elevation + b.height || a.elevation + a.height < b.elevation) {
        return;
    }
    // Of lower priority: it is left to get out of the way.
    if (a.avoidance_priority > b.avoidance_priority) {
        return;
    }
    const float dist_sq = abs_sq(a.position - b.position);
    if (dist_sq >= range_sq) {
        return;
    }
    if (agent_neighbors_.size() < a.max_neighbors) {
        agent_neighbors_.emplace_back(dist_sq, other);
    }
    std::size_t i = agent_neighbors_.size() - 1;
    while (i != 0 && dist_sq < agent_neighbors_[i - 1].first) {
        agent_neighbors_[i] = agent_neighbors_[i - 1];
        --i;
    }
    agent_neighbors_[i] = {dist_sq, other};
    if (agent_neighbors_.size() == a.max_neighbors) {
        range_sq = agent_neighbors_.back().first;
    }
}

void Simulation2D::insert_obstacle_neighbor(std::size_t agent, std::size_t obstacle,
                                            float range_sq) {
    // Agent2D::insertObstacleNeighbor, with Godot's checks.
    const Agent2D& a = agents[agent];
    const Obstacle2D& o = obstacles_[obstacle];
    const Obstacle2D& next = obstacles_[o.next];
    if ((a.avoidance_mask & next.avoidance_layers) == 0) {
        return;
    }
    if (a.elevation > o.elevation + o.height || a.elevation + a.height < o.elevation) {
        return;
    }
    const float dist_sq = dist_sq_point_line_segment(o.point, next.point, a.position);
    if (dist_sq >= range_sq) {
        return;
    }
    obstacle_neighbors_.emplace_back(dist_sq, obstacle);
    std::size_t i = obstacle_neighbors_.size() - 1;
    while (i != 0 && dist_sq < obstacle_neighbors_[i - 1].first) {
        obstacle_neighbors_[i] = obstacle_neighbors_[i - 1];
        --i;
    }
    obstacle_neighbors_[i] = {dist_sq, obstacle};
}

void Simulation2D::query_agent_tree_recursive(std::size_t agent, float& range_sq,
                                              std::size_t node) {
    const AgentTreeNode& n = agent_tree_[node];
    if (n.end - n.begin <= kMaxLeafSize) {
        for (std::size_t i = n.begin; i < n.end; ++i) {
            insert_agent_neighbor(agent, tree_agents_[i], range_sq);
        }
        return;
    }
    const V2 p = agents[agent].position;
    const auto dist_sq = [&](const AgentTreeNode& child) {
        return sqr(std::max(0.0F, child.min_x - p.x)) + sqr(std::max(0.0F, p.x - child.max_x)) +
               sqr(std::max(0.0F, child.min_y - p.y)) + sqr(std::max(0.0F, p.y - child.max_y));
    };
    const std::size_t left = n.left;
    const std::size_t right = n.right;
    const float dist_sq_left = dist_sq(agent_tree_[left]);
    const float dist_sq_right = dist_sq(agent_tree_[right]);
    if (dist_sq_left < dist_sq_right) {
        if (dist_sq_left < range_sq) {
            query_agent_tree_recursive(agent, range_sq, left);
            if (dist_sq_right < range_sq) {
                query_agent_tree_recursive(agent, range_sq, right);
            }
        }
    } else if (dist_sq_right < range_sq) {
        query_agent_tree_recursive(agent, range_sq, right);
        if (dist_sq_left < range_sq) {
            query_agent_tree_recursive(agent, range_sq, left);
        }
    }
}

void Simulation2D::query_obstacle_tree_recursive(std::size_t agent, float range_sq,
                                                 std::size_t node) {
    if (node == kNone) {
        return;
    }
    const ObstacleTreeNode n = obstacle_tree_[node];
    const V2 p1 = obstacles_[n.obstacle].point;
    const V2 p2 = obstacles_[obstacles_[n.obstacle].next].point;
    const float agent_left_of_line = left_of(p1, p2, agents[agent].position);
    query_obstacle_tree_recursive(agent, range_sq, agent_left_of_line >= 0.0F ? n.left : n.right);
    const float dist_sq_line = sqr(agent_left_of_line) / abs_sq(p2 - p1);
    if (dist_sq_line < range_sq) {
        if (agent_left_of_line < 0.0F) {
            // Only when the agent is right of the edge, where it sees it.
            insert_obstacle_neighbor(agent, n.obstacle, range_sq);
        }
        // The other side of the line.
        query_obstacle_tree_recursive(agent, range_sq,
                                      agent_left_of_line >= 0.0F ? n.right : n.left);
    }
}

} // namespace slash::navigation::rvo
