#include "slash/core/math.hpp"
#include "slash/scene3d/humanoid.hpp"
#include "slash/scene3d/model.hpp"
#include "slash/world/tree3d.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

// A humanoid skeleton fitted to a mesh by its proportions, the rigging a service such as Meshy's
// does with a model of the body: no code of anyone's, only the averages of a standing figure.
namespace slash::scene3d {
namespace {

// A bone of the fitted skeleton: its profile name, its parent (an index into the list, before
// it) and where its head is, as fractions of the figure: x from the middle across the width, y
// up from the floor and z forward from the middle, each times the height.
struct Placed {
    std::string_view name;
    int parent;
    Vec3 head;
};

float length(Vec3 v) {
    return std::sqrt((v.x * v.x) + (v.y * v.y) + (v.z * v.z));
}

float dot(Vec3 a, Vec3 b) {
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

Vec3 normalized(Vec3 v) {
    const float l = length(v);
    return l > 0.0F ? v * (1.0F / l) : v;
}

// How far p is from the segment from a to b.
float to_segment(Vec3 p, Vec3 a, Vec3 b) {
    const Vec3 ab = b - a;
    const float span = dot(ab, ab);
    const float t = span > 0.0F ? std::clamp(dot(p - a, ab) / span, 0.0F, 1.0F) : 0.0F;
    return length(p - (a + (ab * t)));
}

// The side a bone is on: 1 for the figure's left (+x, as the profile faces +z), -1 for its
// right, 0 for the middle.
int side_of(std::string_view name) {
    if (name.starts_with("Left")) {
        return 1;
    }
    return name.starts_with("Right") ? -1 : 0;
}

world::Affine3D translation(Vec3 at) {
    world::Affine3D out;
    out.origin = at;
    return out;
}

} // namespace

Result<Model> rig_humanoid(const Model& model) {
    if (!model.skins.empty()) {
        return make_error(ErrorCode::InvalidArgument, "the model is rigged already");
    }
    // The figure's bounds, its parts placed.
    Vec3 low{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
             std::numeric_limits<float>::max()};
    Vec3 high = low * -1.0F;
    std::size_t corners = 0;
    for (const ModelPart& part : model.parts) {
        for (const ModelSurface& surface : model.meshes.at(part.mesh).surfaces) {
            for (const render::Vertex3D& vertex : surface.vertices) {
                const Vec3 p = part.transform.apply(
                    {vertex.position[0], vertex.position[1], vertex.position[2]});
                low = {std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z)};
                high = {std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z)};
                ++corners;
            }
        }
    }
    const float height = high.y - low.y;
    if (corners == 0 || !(height > 0.0F)) {
        return make_error(ErrorCode::InvalidArgument, "the model has no mesh to rig");
    }
    const Vec3 middle{(low.x + high.x) * 0.5F, low.y, (low.z + high.z) * 0.5F};
    const float half_width = (high.x - low.x) * 0.5F;

    // Where the arms reach: the hands a little in from the widest point, at the height of what
    // is out there (level in a T-pose, lower in an A-pose), or hanging an arm's length below the
    // shoulders when nothing reaches past the body.
    constexpr float kShoulder = 0.81F;
    constexpr float kUpperArm = 0.11F;
    constexpr float kArm = 0.34F;
    constexpr float kBody = 0.2F; // half a figure's width without its arms
    const float spread = half_width / height;
    const float hand_x = std::clamp(spread - 0.04F, kUpperArm + 0.04F, kUpperArm + kArm);
    float hand_y = kShoulder - kArm;
    if (spread > kBody) {
        double sum = 0.0;
        std::size_t count = 0;
        for (const ModelPart& part : model.parts) {
            for (const ModelSurface& surface : model.meshes.at(part.mesh).surfaces) {
                for (const render::Vertex3D& vertex : surface.vertices) {
                    const Vec3 p = part.transform.apply(
                        {vertex.position[0], vertex.position[1], vertex.position[2]});
                    if (std::abs(p.x - ((low.x + high.x) * 0.5F)) >= half_width * 0.9F) {
                        sum += p.y - low.y;
                        ++count;
                    }
                }
            }
        }
        hand_y = std::clamp(static_cast<float>(sum / static_cast<double>(count)) / height,
                            kShoulder - kArm, kShoulder);
    }
    const float elbow_x = kUpperArm + ((hand_x - kUpperArm) * 0.55F);
    const float elbow_y = kShoulder + ((hand_y - kShoulder) * 0.55F);

    std::vector<Placed> bones{
        {"Root", -1, {0.0F, 0.0F, 0.0F}},       {"Hips", 0, {0.0F, 0.53F, 0.0F}},
        {"Spine", 1, {0.0F, 0.58F, 0.0F}},      {"Chest", 2, {0.0F, 0.66F, 0.0F}},
        {"UpperChest", 3, {0.0F, 0.74F, 0.0F}}, {"Neck", 4, {0.0F, 0.84F, 0.0F}},
        {"Head", 5, {0.0F, 0.88F, 0.0F}},
    };
    for (const float side : {1.0F, -1.0F}) {
        static constexpr std::array<std::string_view, 8> kLeft{
            "LeftShoulder", "LeftUpperArm", "LeftLowerArm", "LeftHand",
            "LeftUpperLeg", "LeftLowerLeg", "LeftFoot",     "LeftToes"};
        static constexpr std::array<std::string_view, 8> kRight{
            "RightShoulder", "RightUpperArm", "RightLowerArm", "RightHand",
            "RightUpperLeg", "RightLowerLeg", "RightFoot",     "RightToes"};
        const std::array<std::string_view, 8>& name = side > 0.0F ? kLeft : kRight;
        const int first = static_cast<int>(bones.size());
        bones.push_back({name[0], 4, {0.02F * side, kShoulder, 0.0F}});
        bones.push_back({name[1], first, {kUpperArm * side, kShoulder, 0.0F}});
        bones.push_back({name[2], first + 1, {elbow_x * side, elbow_y, 0.0F}});
        bones.push_back({name[3], first + 2, {hand_x * side, hand_y, 0.0F}});
        bones.push_back({name[4], 1, {0.065F * side, 0.50F, 0.0F}});
        bones.push_back({name[5], first + 4, {0.065F * side, 0.27F, 0.0F}});
        bones.push_back({name[6], first + 5, {0.065F * side, 0.045F, 0.0F}});
        bones.push_back({name[7], first + 6, {0.065F * side, 0.0F, 0.09F}});
    }
    const auto place = [&](Vec3 fraction) { return middle + (fraction * height); };

    // Each bone's segment, from its head to its child's (or on past its end for the last of a
    // chain), which vertices are weighted by their distance from.
    std::vector<std::array<Vec3, 2>> segments(bones.size());
    for (std::size_t i = 0; i < bones.size(); ++i) {
        const Vec3 head = place(bones[i].head);
        Vec3 tail = head;
        bool found = false;
        for (std::size_t c = i + 1; c < bones.size(); ++c) {
            if (bones[c].parent == static_cast<int>(i)) {
                tail = place(bones[c].head);
                found = true;
                break;
            }
        }
        if (!found) {
            const std::string_view name = bones[i].name;
            if (name == "Head") {
                tail = place({0.0F, 1.0F, 0.0F});
            } else if (name.ends_with("Hand")) {
                const Vec3 parent_head =
                    place(bones.at(static_cast<std::size_t>(bones[i].parent)).head);
                tail = head + (normalized(head - parent_head) * (0.09F * height));
            } else if (name.ends_with("Toes")) {
                tail = head + Vec3{0.0F, 0.0F, 0.05F * height};
            }
        }
        segments[i] = {head, tail};
    }

    Model out;
    out.materials = model.materials;
    out.images = model.images;
    ModelSkin skin;
    skin.name = "Skeleton";
    for (const Placed& bone : bones) {
        ModelNode node;
        node.name = std::string(bone.name);
        node.parent = bone.parent;
        const Vec3 at =
            bone.parent < 0
                ? place(bone.head)
                : (bone.head - bones.at(static_cast<std::size_t>(bone.parent)).head) * height;
        node.position = at;
        node.file_index = static_cast<int>(out.nodes.size());
        skin.joints.push_back(out.nodes.size());
        skin.inverse_binds.push_back(translation(place(bone.head) * -1.0F));
        out.nodes.push_back(std::move(node));
    }
    out.skins.push_back(std::move(skin));

    // The parts, placed in the model's space, each weighted to its two nearest bones on its side
    // of the figure by the inverse of the square of how far they are.
    const float margin = 0.02F * height;
    const float soft = (0.01F * height) * (0.01F * height);
    for (const ModelPart& part : model.parts) {
        ModelMesh mesh = model.meshes.at(part.mesh);
        for (ModelSurface& surface : mesh.surfaces) {
            surface.lods.clear();
            surface.weights.assign(surface.vertices.size(), {});
            for (std::size_t v = 0; v < surface.vertices.size(); ++v) {
                render::Vertex3D& vertex = surface.vertices[v];
                const Vec3 p = part.transform.apply(
                    {vertex.position[0], vertex.position[1], vertex.position[2]});
                const Vec3 n = normalized(
                    part.transform.basis({vertex.normal[0], vertex.normal[1], vertex.normal[2]}));
                const Vec3 t = normalized(part.transform.basis(
                    {vertex.tangent[0], vertex.tangent[1], vertex.tangent[2]}));
                vertex.position = {p.x, p.y, p.z};
                vertex.normal = {n.x, n.y, n.z};
                vertex.tangent = {t.x, t.y, t.z, vertex.tangent[3]};
                int side = 0;
                if (p.x > middle.x + margin) {
                    side = 1;
                } else if (p.x < middle.x - margin) {
                    side = -1;
                }
                std::array<std::size_t, 2> nearest{0, 0};
                std::array<float, 2> distance{std::numeric_limits<float>::max(),
                                              std::numeric_limits<float>::max()};
                for (std::size_t b = 1; b < bones.size(); ++b) {
                    const int bone_side = side_of(bones[b].name);
                    if (bone_side != 0 && bone_side != side) {
                        continue;
                    }
                    const float d = to_segment(p, segments[b][0], segments[b][1]);
                    if (d < distance[0]) {
                        nearest = {b, nearest[0]};
                        distance = {d, distance[0]};
                    } else if (d < distance[1]) {
                        nearest[1] = b;
                        distance[1] = d;
                    }
                }
                VertexWeights& weights = surface.weights[v];
                const float first = 1.0F / ((distance[0] * distance[0]) + soft);
                const float second = distance[1] < std::numeric_limits<float>::max()
                                         ? 1.0F / ((distance[1] * distance[1]) + soft)
                                         : 0.0F;
                weights.joints[0] = static_cast<std::uint16_t>(nearest[0]);
                weights.joints[1] = static_cast<std::uint16_t>(nearest[1]);
                weights.weights[0] = first / (first + second);
                weights.weights[1] = 1.0F - weights.weights[0];
            }
        }
        // Node names are unique in a model, so a part named as a bone is renamed.
        std::string name = part.node.empty() ? std::string("Body") : part.node;
        for (int suffix = 2; std::ranges::any_of(
                 out.nodes, [&](const ModelNode& other) { return other.name == name; });
             ++suffix) {
            name = (part.node.empty() ? std::string("Body") : part.node) + std::to_string(suffix);
        }
        ModelNode node;
        node.name = name;
        node.file_index = static_cast<int>(out.nodes.size());
        out.parts.push_back({.node = name,
                             .mesh = out.meshes.size(),
                             .transform = {},
                             .node_index = static_cast<int>(out.nodes.size()),
                             .skin = 0});
        out.nodes.push_back(std::move(node));
        out.meshes.push_back(std::move(mesh));
    }
    return out;
}

} // namespace slash::scene3d
