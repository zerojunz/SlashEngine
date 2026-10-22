// RVO2-3D's agents, k-d tree and linear programs (Agent3d.cpp, KdTree3d.cpp), as Godot 4.6
// changes them in thirdparty/rvo2/rvo2_3d. See rvo2.hpp for the license and the changes.

#include "rvo2.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

namespace slash::navigation::rvo {
namespace {

constexpr float kEpsilon = 0.00001F; // RVO3D_EPSILON
constexpr std::size_t kMaxLeafSize = 10;

struct Line3 {
    V3 direction;
    V3 point;
};

// linearProgram1: the best point of a line on plane plane_no within the sphere and the planes
// before it.
bool linear_program_1(const std::vector<Plane>& planes, std::size_t plane_no, const Line3& line,
                      float radius, V3 opt_velocity, bool direction_opt, V3& result) {
    const float dot_product = dot(line.point, line.direction);
    const float discriminant = sqr(dot_product) + sqr(radius) - abs_sq(line.point);
    if (discriminant < 0.0F) {
        // The max speed sphere fully invalidates the line.
        return false;
    }
    const float sqrt_discriminant = std::sqrt(discriminant);
    float t_left = -dot_product - sqrt_discriminant;
    float t_right = -dot_product + sqrt_discriminant;
    for (std::size_t i = 0; i < plane_no; ++i) {
        const float numerator = dot(planes[i].point - line.point, planes[i].normal);
        const float denominator = dot(line.direction, planes[i].normal);
        if (sqr(denominator) <= kEpsilon) {
            // The line is (almost) parallel to plane i.
            if (numerator > 0.0F) {
                return false;
            }
            continue;
        }
        const float t = numerator / denominator;
        if (denominator >= 0.0F) {
            // Plane i bounds the line on the left.
            t_left = std::max(t_left, t);
        } else {
            // Plane i bounds the line on the right.
            t_right = std::min(t_right, t);
        }
        if (t_left > t_right) {
            return false;
        }
    }
    if (direction_opt) {
        // Optimize the direction: the extreme it points to.
        if (dot(opt_velocity, line.direction) > 0.0F) {
            result = line.point + (t_right * line.direction);
        } else {
            result = line.point + (t_left * line.direction);
        }
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

// linearProgram2: the best point on plane plane_no within the sphere and the planes before it.
bool linear_program_2(const std::vector<Plane>& planes, std::size_t plane_no, float radius,
                      V3 opt_velocity, bool direction_opt, V3& result) {
    const Plane& plane = planes[plane_no];
    const float plane_dist = dot(plane.point, plane.normal);
    const float plane_dist_sq = sqr(plane_dist);
    const float radius_sq = sqr(radius);
    if (plane_dist_sq > radius_sq) {
        // The max speed sphere fully invalidates the plane.
        return false;
    }
    const float plane_radius_sq = radius_sq - plane_dist_sq;
    const V3 plane_center = plane_dist * plane.normal;
    if (direction_opt) {
        // Project the direction on the plane.
        const V3 plane_opt_velocity =
            opt_velocity - (dot(opt_velocity, plane.normal) * plane.normal);
        const float plane_opt_velocity_length_sq = abs_sq(plane_opt_velocity);
        if (plane_opt_velocity_length_sq <= kEpsilon) {
            result = plane_center;
        } else {
            result = plane_center + (std::sqrt(plane_radius_sq / plane_opt_velocity_length_sq) *
                                     plane_opt_velocity);
        }
    } else {
        // Project the point on the plane, then on its circle if it is outside.
        result = opt_velocity + (dot(plane.point - opt_velocity, plane.normal) * plane.normal);
        if (abs_sq(result) > radius_sq) {
            const V3 plane_result = result - plane_center;
            const float plane_result_length_sq = abs_sq(plane_result);
            result =
                plane_center + (std::sqrt(plane_radius_sq / plane_result_length_sq) * plane_result);
        }
    }
    for (std::size_t i = 0; i < plane_no; ++i) {
        if (dot(planes[i].normal, planes[i].point - result) <= 0.0F) {
            continue;
        }
        // The result breaks constraint i: the best point on the line where the planes meet.
        const V3 cross_product = cross(planes[i].normal, plane.normal);
        if (abs_sq(cross_product) <= kEpsilon) {
            // The planes are (almost) parallel, and plane i fully invalidates this one.
            return false;
        }
        Line3 line;
        line.direction = normalize(cross_product);
        const V3 line_normal = cross(line.direction, plane.normal);
        line.point = plane.point + ((dot(planes[i].point - plane.point, planes[i].normal) /
                                     dot(line_normal, planes[i].normal)) *
                                    line_normal);
        if (!linear_program_1(planes, i, line, radius, opt_velocity, direction_opt, result)) {
            return false;
        }
    }
    return true;
}

// linearProgram3: the best point within the sphere and every plane. Returns the plane it fails
// on, or the number of planes.
std::size_t linear_program_3(const std::vector<Plane>& planes, float radius, V3 opt_velocity,
                             bool direction_opt, V3& result) {
    if (direction_opt) {
        // Optimize the direction; opt_velocity is of unit length here.
        result = opt_velocity * radius;
    } else if (abs_sq(opt_velocity) > sqr(radius)) {
        // Optimize the closest point, outside the sphere.
        result = normalize(opt_velocity) * radius;
    } else {
        // Optimize the closest point, inside the sphere.
        result = opt_velocity;
    }
    for (std::size_t i = 0; i < planes.size(); ++i) {
        if (dot(planes[i].normal, planes[i].point - result) > 0.0F) {
            // The result breaks constraint i.
            const V3 temp_result = result;
            if (!linear_program_2(planes, i, radius, opt_velocity, direction_opt, result)) {
                result = temp_result;
                return i;
            }
        }
    }
    return planes.size();
}

// linearProgram4: when the constraints can't all be met, the velocity that breaks them least.
void linear_program_4(const std::vector<Plane>& planes, std::size_t begin_plane, float radius,
                      V3& result) {
    float distance = 0.0F;
    std::vector<Plane> proj_planes;
    for (std::size_t i = begin_plane; i < planes.size(); ++i) {
        if (dot(planes[i].normal, planes[i].point - result) <= distance) {
            continue;
        }
        // The result breaks the constraint of plane i.
        proj_planes.clear();
        for (std::size_t j = 0; j < i; ++j) {
            Plane plane;
            const V3 cross_product = cross(planes[j].normal, planes[i].normal);
            if (abs_sq(cross_product) <= kEpsilon) {
                // Planes i and j are (almost) parallel.
                if (dot(planes[i].normal, planes[j].normal) > 0.0F) {
                    // They point the same way.
                    continue;
                }
                // They point opposite ways.
                plane.point = 0.5F * (planes[i].point + planes[j].point);
            } else {
                // A point on the line where planes i and j meet.
                const V3 line_normal = cross(cross_product, planes[i].normal);
                plane.point =
                    planes[i].point + ((dot(planes[j].point - planes[i].point, planes[j].normal) /
                                        dot(line_normal, planes[j].normal)) *
                                       line_normal);
            }
            plane.normal = normalize(planes[j].normal - planes[i].normal);
            proj_planes.push_back(plane);
        }
        const V3 temp_result = result;
        if (linear_program_3(proj_planes, radius, planes[i].normal, true, result) <
            proj_planes.size()) {
            // Only floating point error gets here, since the result is already feasible; keep
            // it.
            result = temp_result;
        }
        distance = dot(planes[i].normal, planes[i].point - result);
    }
}

} // namespace

void Simulation3D::step(float time_step) {
    time_step_ = time_step;
    // KdTree3D::buildAgentTree.
    tree_agents_.resize(agents.size());
    for (std::size_t i = 0; i < agents.size(); ++i) {
        tree_agents_[i] = i;
    }
    agent_tree_.clear();
    if (!agents.empty()) {
        agent_tree_.resize((2 * agents.size()) - 1);
        build_agent_tree_recursive(0, agents.size(), 0);
    }
    for (std::size_t i = 0; i < agents.size(); ++i) {
        // Agent3D::computeNeighbors.
        agent_neighbors_.clear();
        if (agents[i].max_neighbors > 0) {
            float range_sq = sqr(agents[i].neighbor_dist);
            query_agent_tree_recursive(i, range_sq, 0);
        }
        compute_new_velocity(i);
    }
    // Agent3D::update.
    for (Agent3D& agent : agents) {
        agent.velocity = agent.new_velocity;
        agent.position = agent.position + (agent.velocity * time_step_);
    }
}

void Simulation3D::build_agent_tree_recursive(std::size_t begin, std::size_t end,
                                              std::size_t node) {
    AgentTreeNode& n = agent_tree_[node];
    n.begin = begin;
    n.end = end;
    n.min_coord = agents[tree_agents_[begin]].position;
    n.max_coord = n.min_coord;
    for (std::size_t i = begin + 1; i < end; ++i) {
        const V3 p = agents[tree_agents_[i]].position;
        n.max_coord.x = std::max(n.max_coord.x, p.x);
        n.min_coord.x = std::min(n.min_coord.x, p.x);
        n.max_coord.y = std::max(n.max_coord.y, p.y);
        n.min_coord.y = std::min(n.min_coord.y, p.y);
        n.max_coord.z = std::max(n.max_coord.z, p.z);
        n.min_coord.z = std::min(n.min_coord.z, p.z);
    }
    if (end - begin <= kMaxLeafSize) {
        return;
    }
    // Not a leaf: split along the longest side.
    const float size_x = n.max_coord.x - n.min_coord.x;
    const float size_y = n.max_coord.y - n.min_coord.y;
    const float size_z = n.max_coord.z - n.min_coord.z;
    std::size_t coord = 2;
    if (size_x > size_y && size_x > size_z) {
        coord = 0;
    } else if (size_y > size_z) {
        coord = 1;
    }
    const float split = 0.5F * (n.max_coord[coord] + n.min_coord[coord]);
    std::size_t left = begin;
    std::size_t right = end;
    while (left < right) {
        while (left < right && agents[tree_agents_[left]].position[coord] < split) {
            ++left;
        }
        while (right > left && agents[tree_agents_[right - 1]].position[coord] >= split) {
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

void Simulation3D::compute_new_velocity(std::size_t agent) {
    // Agent3D::computeNewVelocity: the velocity closest to the preferred one that keeps out of
    // every neighbor's velocity obstacle.
    Agent3D& a = agents[agent];
    orca_planes_.clear();
    const float inv_time_horizon = 1.0F / a.time_horizon;
    for (const auto& [dist, index] : agent_neighbors_) {
        const Agent3D& other = agents[index];
        const V3 relative_position = other.position - a.position;
        const V3 relative_velocity = a.velocity - other.velocity;
        const float dist_sq = abs_sq(relative_position);
        const float combined_radius = a.radius + other.radius;
        const float combined_radius_sq = sqr(combined_radius);
        Plane plane;
        V3 u;
        if (dist_sq > combined_radius_sq) {
            // No collision. w runs from the cutoff center to the relative velocity.
            const V3 w = relative_velocity - (inv_time_horizon * relative_position);
            const float w_length_sq = abs_sq(w);
            const float dot_product = dot(w, relative_position);
            if (dot_product < 0.0F && sqr(dot_product) > combined_radius_sq * w_length_sq) {
                // Project on the cutoff sphere.
                const float w_length = std::sqrt(w_length_sq);
                const V3 unit_w = w / w_length;
                plane.normal = unit_w;
                u = ((combined_radius * inv_time_horizon) - w_length) * unit_w;
            } else {
                // Project on the cone.
                const float qa = dist_sq;
                const float qb = dot(relative_position, relative_velocity);
                const float qc = abs_sq(relative_velocity) -
                                 (abs_sq(cross(relative_position, relative_velocity)) /
                                  (dist_sq - combined_radius_sq));
                const float t = (qb + std::sqrt(sqr(qb) - (qa * qc))) / qa;
                const V3 cone_w = relative_velocity - (t * relative_position);
                const float w_length = abs(cone_w);
                const V3 unit_w = cone_w / w_length;
                plane.normal = unit_w;
                u = ((combined_radius * t) - w_length) * unit_w;
            }
        } else {
            // Collision: get apart within one step.
            const float inv_time_step = 1.0F / time_step_;
            const V3 w = relative_velocity - (inv_time_step * relative_position);
            const float w_length = abs(w);
            if (w_length == 0.0F) {
                // Right on top of it at the same velocity, as in 2D.
                continue;
            }
            const V3 unit_w = w / w_length;
            plane.normal = unit_w;
            u = ((combined_radius * inv_time_step) - w_length) * unit_w;
        }
        plane.point = a.velocity + (0.5F * u);
        orca_planes_.push_back(plane);
    }
    const std::size_t plane_fail =
        linear_program_3(orca_planes_, a.max_speed, a.pref_velocity, false, a.new_velocity);
    if (plane_fail < orca_planes_.size()) {
        linear_program_4(orca_planes_, plane_fail, a.max_speed, a.new_velocity);
    }
}

void Simulation3D::insert_agent_neighbor(std::size_t agent, std::size_t other, float& range_sq) {
    // Agent3D::insertAgentNeighbor, with Godot's checks.
    if (agent == other) {
        return;
    }
    const Agent3D& a = agents[agent];
    const Agent3D& b = agents[other];
    if ((a.avoidance_mask & b.avoidance_layers) == 0) {
        return;
    }
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

void Simulation3D::query_agent_tree_recursive(std::size_t agent, float& range_sq,
                                              std::size_t node) {
    const AgentTreeNode& n = agent_tree_[node];
    if (n.end - n.begin <= kMaxLeafSize) {
        for (std::size_t i = n.begin; i < n.end; ++i) {
            insert_agent_neighbor(agent, tree_agents_[i], range_sq);
        }
        return;
    }
    const V3 p = agents[agent].position;
    const auto dist_sq = [&](const AgentTreeNode& child) {
        return sqr(std::max(0.0F, child.min_coord.x - p.x)) +
               sqr(std::max(0.0F, p.x - child.max_coord.x)) +
               sqr(std::max(0.0F, child.min_coord.y - p.y)) +
               sqr(std::max(0.0F, p.y - child.max_coord.y)) +
               sqr(std::max(0.0F, child.min_coord.z - p.z)) +
               sqr(std::max(0.0F, p.z - child.max_coord.z));
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

} // namespace slash::navigation::rvo
