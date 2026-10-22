#include "slash/scene3d/skeleton.hpp"

#include "slash/core/json.hpp"
#include "slash/core/text.hpp"
#include "slash/world/world.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <map>
#include <unordered_map>
#include <utility>

namespace slash::scene3d {
namespace {

float dot(Vec3 a, Vec3 b) {
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

Vec3 normalized(Vec3 v) {
    const float length = std::sqrt(dot(v, v));
    return length == 0.0F ? v : v * (1.0F / length);
}

float dot(const Quat& a, const Quat& b) {
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z) + (a.w * b.w);
}

Quat normalized(const Quat& q) {
    const float length = std::sqrt(dot(q, q));
    if (length == 0.0F) {
        return {};
    }
    const float inverse = 1.0F / length;
    return {q.x * inverse, q.y * inverse, q.z * inverse, q.w * inverse};
}

// Godot's Quaternion::operator*: this rotation after o.
Quat multiply(const Quat& a, const Quat& b) {
    return {(a.w * b.x) + (a.x * b.w) + (a.y * b.z) - (a.z * b.y),
            (a.w * b.y) + (a.y * b.w) + (a.z * b.x) - (a.x * b.z),
            (a.w * b.z) + (a.z * b.w) + (a.x * b.y) - (a.y * b.x),
            (a.w * b.w) - (a.x * b.x) - (a.y * b.y) - (a.z * b.z)};
}

// The inverse of a unit quaternion.
Quat conjugate(const Quat& q) {
    return {-q.x, -q.y, -q.z, q.w};
}

// Godot's Quaternion::slerp, with angles worked out the same way on every platform.
Quat slerp(const Quat& from, const Quat& to, float weight) {
    float cosom = dot(from, to);
    Quat target = to;
    if (cosom < 0.0F) {
        cosom = -cosom;
        target = {-to.x, -to.y, -to.z, -to.w};
    }
    float scale0 = 1.0F - weight;
    float scale1 = weight;
    if (1.0F - cosom > 0.00001F) { // Godot's CMP_EPSILON
        const float sinom = std::sqrt(std::max(0.0F, 1.0F - (cosom * cosom)));
        const float omega = atan2(sinom, cosom);
        scale0 = cos_sin((1.0F - weight) * omega).y / sinom;
        scale1 = cos_sin(weight * omega).y / sinom;
    }
    return {(scale0 * from.x) + (scale1 * target.x), (scale0 * from.y) + (scale1 * target.y),
            (scale0 * from.z) + (scale1 * target.z), (scale0 * from.w) + (scale1 * target.w)};
}

Vec3 lerp(Vec3 a, Vec3 b, float t) {
    return a + ((b - a) * t);
}

// Godot's Basis::get_quaternion, on a rotation basis whose columns are x, y and z.
Quat basis_quaternion(const world::Affine3D& m) {
    // rows[r][c] is column c's component r.
    const std::array<std::array<float, 3>, 3> rows{
        {{m.x.x, m.y.x, m.z.x}, {m.x.y, m.y.y, m.z.y}, {m.x.z, m.y.z, m.z.z}}};
    const float trace = rows[0][0] + rows[1][1] + rows[2][2];
    std::array<float, 4> q{};
    if (trace > 0.0F) {
        float s = std::sqrt(trace + 1.0F);
        q[3] = s * 0.5F;
        s = 0.5F / s;
        q[0] = (rows[2][1] - rows[1][2]) * s;
        q[1] = (rows[0][2] - rows[2][0]) * s;
        q[2] = (rows[1][0] - rows[0][1]) * s;
    } else {
        std::size_t i = 0;
        if (rows[0][0] < rows[1][1]) {
            i = rows[1][1] < rows[2][2] ? 2 : 1;
        } else {
            i = rows[0][0] < rows[2][2] ? 2 : 0;
        }
        const std::size_t j = (i + 1) % 3;
        const std::size_t k = (i + 2) % 3;
        float s = std::sqrt(rows.at(i).at(i) - rows.at(j).at(j) - rows.at(k).at(k) + 1.0F);
        q.at(i) = s * 0.5F;
        s = 0.5F / s;
        q[3] = (rows.at(k).at(j) - rows.at(j).at(k)) * s;
        q.at(j) = (rows.at(j).at(i) + rows.at(i).at(j)) * s;
        q.at(k) = (rows.at(k).at(i) + rows.at(i).at(k)) * s;
    }
    return {q[0], q[1], q[2], q[3]};
}

float determinant(const world::Affine3D& m) {
    return (m.x.x * ((m.y.y * m.z.z) - (m.z.y * m.y.z))) -
           (m.y.x * ((m.x.y * m.z.z) - (m.z.y * m.x.z))) +
           (m.z.x * ((m.x.y * m.y.z) - (m.y.y * m.x.z)));
}

std::array<float, 4> array_of(const Quat& q) {
    return {q.x, q.y, q.z, q.w};
}

Quat quat_of(const std::array<float, 4>& v) {
    return {v[0], v[1], v[2], v[3]};
}

Vec3 vec_of(const std::array<float, 4>& v) {
    return {v[0], v[1], v[2]};
}

// Where the clip source is kept, in the world's context.
struct ClipSourceHolder {
    ClipSource source;
};

} // namespace

std::vector<NodePose> rest_pose(const Model& model) {
    std::vector<NodePose> out;
    out.reserve(model.nodes.size());
    for (const ModelNode& node : model.nodes) {
        out.push_back({.position = node.position, .rotation = node.rotation, .scale = node.scale});
    }
    return out;
}

int find_node(const Model& model, std::string_view name) {
    for (std::size_t i = 0; i < model.nodes.size(); ++i) {
        if (model.nodes[i].name == name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::vector<int> node_map(const Model& target, const Model& source) {
    std::unordered_map<std::string_view, int> by_name;
    for (std::size_t i = 0; i < target.nodes.size(); ++i) {
        by_name.emplace(target.nodes[i].name, static_cast<int>(i));
    }
    std::vector<int> out(source.nodes.size(), -1);
    for (std::size_t i = 0; i < source.nodes.size(); ++i) {
        if (const auto it = by_name.find(source.nodes[i].name); it != by_name.end()) {
            out[i] = it->second;
        }
    }
    return out;
}

std::array<float, 4> sample_track(const NodeTrack& track, double time, double length, bool loop) {
    const std::vector<float>& times = track.times;
    const std::size_t count = std::min(times.size(), track.values.size());
    if (count == 0) {
        return {0.0F, 0.0F, 0.0F, track.property == NodeProperty::Rotation ? 1.0F : 0.0F};
    }
    if (count == 1) {
        return track.values.front();
    }
    time = std::clamp(time, 0.0, length);
    const auto before = static_cast<std::size_t>(
        std::ranges::upper_bound(times.begin(), times.begin() + static_cast<std::ptrdiff_t>(count),
                                 time, std::less<>{},
                                 [](float key) { return static_cast<double>(key); }) -
        times.begin());
    if (track.step) {
        if (before == 0) {
            return loop ? track.values[count - 1] : track.values.front();
        }
        return track.values[before - 1];
    }
    // The keys either side of the time, on the same clock: round the loop for a looping track
    // before its first key or after its last.
    std::size_t a = before - 1;
    std::size_t b = before;
    double a_time = 0.0;
    double b_time = 0.0;
    if (before == 0) {
        if (!loop) {
            return track.values.front();
        }
        a = count - 1;
        b = 0;
        a_time = static_cast<double>(times[count - 1]) - length;
        b_time = times[0];
    } else if (before == count) {
        if (!loop || time == static_cast<double>(times[count - 1])) {
            return track.values[count - 1];
        }
        b = 0;
        a_time = times[count - 1];
        b_time = static_cast<double>(times[0]) + length;
    } else {
        a_time = times[a];
        b_time = times[b];
    }
    const double span = b_time - a_time;
    const auto weight = static_cast<float>(span > 0.0 ? (time - a_time) / span : 0.0);
    const std::array<float, 4>& from = track.values[a];
    const std::array<float, 4>& to = track.values[b];
    if (track.property == NodeProperty::Rotation) {
        return array_of(slerp(quat_of(from), quat_of(to), weight));
    }
    const Vec3 v = lerp(vec_of(from), vec_of(to), weight);
    return {v.x, v.y, v.z, 0.0F};
}

void blend_animation(const Model& target, const Model& source, const ModelAnimation& animation,
                     double time, float weight, std::span<const int> map,
                     std::vector<NodePose>& pose) {
    const bool same = map.empty();
    for (const NodeTrack& track : animation.tracks) {
        int node = static_cast<int>(track.node);
        if (!same) {
            node = track.node < map.size() ? map[track.node] : -1;
        } else if (track.node >= source.nodes.size()) {
            node = -1;
        }
        if (node < 0 || static_cast<std::size_t>(node) >= pose.size() ||
            static_cast<std::size_t>(node) >= target.nodes.size()) {
            continue;
        }
        const ModelNode& rest = target.nodes[static_cast<std::size_t>(node)];
        NodePose& posed = pose[static_cast<std::size_t>(node)];
        const std::array<float, 4> value =
            sample_track(track, time, animation.length, animation.loop);
        switch (track.property) {
        case NodeProperty::Position:
            posed.position = posed.position + ((vec_of(value) - rest.position) * weight);
            break;
        case NodeProperty::Rotation: {
            const Quat turn = multiply(conjugate(rest.rotation), quat_of(value));
            posed.rotation = normalized(multiply(posed.rotation, slerp(Quat{}, turn, weight)));
            break;
        }
        case NodeProperty::Scale:
            posed.scale = posed.scale + ((vec_of(value) - rest.scale) * weight);
            break;
        }
    }
}

void apply_skeleton(const Model& model, const world::Skeleton3D& skeleton,
                    std::vector<NodePose>& pose) {
    const auto set = [&](const std::map<std::string, Vec3>& values, auto&& apply) {
        for (const auto& [name, value] : values) {
            const int node = find_node(model, name);
            if (node >= 0 && static_cast<std::size_t>(node) < pose.size()) {
                apply(pose[static_cast<std::size_t>(node)], value);
            }
        }
    };
    set(skeleton.bone_positions, [](NodePose& p, Vec3 v) { p.position = v; });
    set(skeleton.bone_rotations, [](NodePose& p, Vec3 v) { p.rotation = world::quaternion(v); });
    set(skeleton.bone_scales, [](NodePose& p, Vec3 v) { p.scale = v; });
}

world::Affine3D node_transform(const NodePose& pose) {
    world::Affine3D out = world::rotation_basis(pose.rotation);
    out.x = out.x * pose.scale.x;
    out.y = out.y * pose.scale.y;
    out.z = out.z * pose.scale.z;
    out.origin = pose.position;
    return out;
}

NodePose decompose(const world::Affine3D& transform) {
    NodePose out;
    out.position = transform.origin;
    const float sign = determinant(transform) < 0.0F ? -1.0F : 1.0F;
    out.scale =
        Vec3{std::sqrt(dot(transform.x, transform.x)), std::sqrt(dot(transform.y, transform.y)),
             std::sqrt(dot(transform.z, transform.z))} *
        sign;
    // Godot's Basis::orthonormalized: Gram-Schmidt on the columns, x first; then turned into a
    // proper rotation when it mirrors.
    world::Affine3D m = transform;
    m.x = normalized(m.x);
    m.y = normalized(m.y - (m.x * dot(m.x, m.y)));
    m.z = normalized(m.z - (m.x * dot(m.x, m.z)) - (m.y * dot(m.y, m.z)));
    if (determinant(m) < 0.0F) {
        m.x = m.x * -1.0F;
        m.y = m.y * -1.0F;
        m.z = m.z * -1.0F;
    }
    out.rotation = basis_quaternion(m);
    return out;
}

std::vector<world::Affine3D> node_globals(const Model& model, std::span<const NodePose> pose) {
    std::vector<world::Affine3D> out(model.nodes.size());
    for (std::size_t i = 0; i < model.nodes.size() && i < pose.size(); ++i) {
        const world::Affine3D local = node_transform(pose[i]);
        const int parent = model.nodes[i].parent;
        out[i] = parent >= 0 && static_cast<std::size_t>(parent) < i
                     ? out[static_cast<std::size_t>(parent)] * local
                     : local;
    }
    return out;
}

std::vector<world::Affine3D> joint_transforms(const ModelSkin& skin,
                                              std::span<const world::Affine3D> globals) {
    std::vector<world::Affine3D> out(skin.joints.size());
    for (std::size_t i = 0; i < skin.joints.size(); ++i) {
        const std::size_t node = skin.joints[i];
        const world::Affine3D global = node < globals.size() ? globals[node] : world::Affine3D{};
        out[i] = i < skin.inverse_binds.size() ? global * skin.inverse_binds[i] : global;
    }
    return out;
}

void skin_vertices(const ModelSurface& surface, std::span<const world::Affine3D> joints,
                   std::vector<render::Vertex3D>& out) {
    out.resize(surface.vertices.size());
    for (std::size_t i = 0; i < surface.vertices.size(); ++i) {
        render::Vertex3D vertex = surface.vertices[i];
        if (i < surface.weights.size()) {
            const VertexWeights& w = surface.weights[i];
            world::Affine3D m{.x = {}, .y = {}, .z = {}, .origin = {}};
            float total = 0.0F;
            for (std::size_t k = 0; k < w.weights.size(); ++k) {
                const float weight = w.weights.at(k);
                const std::size_t joint = w.joints.at(k);
                if (weight == 0.0F || joint >= joints.size()) {
                    continue;
                }
                const world::Affine3D& j = joints[joint];
                m.x = m.x + (j.x * weight);
                m.y = m.y + (j.y * weight);
                m.z = m.z + (j.z * weight);
                m.origin = m.origin + (j.origin * weight);
                total += weight;
            }
            if (total > 0.0F) {
                const Vec3 p =
                    m.apply({vertex.position[0], vertex.position[1], vertex.position[2]});
                vertex.position = {p.x, p.y, p.z};
                const Vec3 n =
                    normalized(m.basis({vertex.normal[0], vertex.normal[1], vertex.normal[2]}));
                vertex.normal = {n.x, n.y, n.z};
                const Vec3 t =
                    normalized(m.basis({vertex.tangent[0], vertex.tangent[1], vertex.tangent[2]}));
                vertex.tangent = {t.x, t.y, t.z, vertex.tangent[3]};
            }
        }
        out[i] = vertex;
    }
}

std::vector<NodePose> blend_poses(std::span<const NodePose> a, std::span<const NodePose> b,
                                  float weight) {
    std::vector<NodePose> out(b.begin(), b.end());
    for (std::size_t i = 0; i < out.size() && i < a.size(); ++i) {
        out[i].position = lerp(a[i].position, b[i].position, weight);
        out[i].rotation = world::nlerp(a[i].rotation, b[i].rotation, weight);
        out[i].scale = lerp(a[i].scale, b[i].scale, weight);
    }
    return out;
}

void set_clip_source(world::World& world, ClipSource source) {
    world.ecs().ctx().insert_or_assign(ClipSourceHolder{std::move(source)});
}

std::vector<ClipSample> clips(const world::World& world, entt::entity entity) {
    const auto* holder = world.ecs().ctx().find<ClipSourceHolder>();
    if (holder == nullptr || !holder->source) {
        return {};
    }
    return holder->source(world, entity);
}

std::vector<NodePose> pose_of(const Model& model, std::span<const ClipSample> clips,
                              const world::Skeleton3D* skeleton, const ModelLoader& load) {
    std::vector<NodePose> pose = rest_pose(model);
    if (skeleton != nullptr && skeleton->show_rest_only) {
        return pose;
    }
    for (const ClipSample& clip : clips) {
        if (!load) {
            break;
        }
        auto source = load(clip.model);
        if (!source || source.value() == nullptr) {
            continue;
        }
        const Model& from = *source.value();
        const auto found =
            std::ranges::find(from.animations, clip.animation, &ModelAnimation::name);
        if (found == from.animations.end()) {
            continue;
        }
        const std::vector<int> map = &from == &model ? std::vector<int>{} : node_map(model, from);
        blend_animation(model, from, *found, clip.time, clip.weight, map, pose);
    }
    if (skeleton != nullptr) {
        apply_skeleton(model, *skeleton, pose);
    }
    return pose;
}

std::vector<SkeletonProblem> check_skeleton(const world::Skeleton3D& skeleton, const Model* model) {
    std::vector<SkeletonProblem> out;
    if (model == nullptr) {
        out.push_back({"", "poses bones, but the entity's MeshInstance3D draws no model: set its "
                           "mesh to a model file with a skeleton"});
        return out;
    }
    std::vector<std::string_view> names;
    names.reserve(model->nodes.size());
    for (const ModelNode& node : model->nodes) {
        names.emplace_back(node.name);
    }
    const auto check = [&](const std::map<std::string, Vec3>& values, const char* field) {
        for (const auto& [name, value] : values) {
            if (find_node(*model, name) >= 0) {
                continue;
            }
            std::string message = "the model has no bone or node named \"" + name + "\"";
            if (const std::string_view hint = closest_match(name, names); !hint.empty()) {
                message += "; did you mean \"" + std::string(hint) + "\"?";
            }
            out.push_back(
                {"/" + std::string(field) + "/" + json::pointer_escape(name), std::move(message)});
        }
    };
    check(skeleton.bone_positions, "bone_positions");
    check(skeleton.bone_rotations, "bone_rotations");
    check(skeleton.bone_scales, "bone_scales");
    return out;
}

} // namespace slash::scene3d
