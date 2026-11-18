#include "skin_gpu.hpp"

#include "slash/core/bits.hpp"

#include <algorithm>
#include <cstring>
#include <string>

namespace slash::render::detail {

namespace {

std::uint32_t word_of(float value) {
    return slash::bit_cast<std::uint32_t>(value);
}

void push3(std::vector<std::uint32_t>& out, const std::array<float, 3>& v) {
    out.push_back(word_of(v[0]));
    out.push_back(word_of(v[1]));
    out.push_back(word_of(v[2]));
}

} // namespace

Result<SkinWords> skin_words(const SkinDesc& desc) {
    const std::size_t count = desc.vertices.size();
    if (count == 0 || count > kMaxMeshVertices) {
        return Error{ErrorCode::InvalidArgument, "a skin needs between 1 and " +
                                                     std::to_string(kMaxMeshVertices) +
                                                     " vertices"};
    }
    if (!desc.weights.empty() && desc.weights.size() != count) {
        return Error{ErrorCode::InvalidArgument,
                     "a skin has " + std::to_string(desc.weights.size()) + " weights for " +
                         std::to_string(count) + " vertices"};
    }
    for (std::size_t k = 0; k < desc.shapes.size(); ++k) {
        const SkinShape& shape = desc.shapes[k];
        for (const std::size_t size :
             {shape.positions.size(), shape.normals.size(), shape.tangents.size()}) {
            if (size != 0 && size != count) {
                return Error{ErrorCode::InvalidArgument,
                             "blend shape " + std::to_string(k) + " has " + std::to_string(size) +
                                 " offsets for " + std::to_string(count) + " vertices"};
            }
        }
    }
    SkinWords out;
    out.vertex_count = static_cast<std::uint32_t>(count);
    out.shape_count = static_cast<std::uint32_t>(desc.shapes.size());
    static_assert(sizeof(Vertex3D) == 15 * sizeof(std::uint32_t));
    out.rest.resize(count * 15);
    std::memcpy(out.rest.data(), desc.vertices.data(), desc.vertices.size_bytes());
    out.weights.reserve(desc.weights.size() * kSkinWeightWords);
    for (const SkinWeights& w : desc.weights) {
        for (std::size_t k = 0; k < 8; k += 2) {
            out.weights.push_back(static_cast<std::uint32_t>(w.joints.at(k)) |
                                  (static_cast<std::uint32_t>(w.joints.at(k + 1)) << 16U));
        }
        for (const float weight : w.weights) {
            out.weights.push_back(word_of(weight));
        }
    }
    out.shapes.reserve(desc.shapes.size() * count * kSkinShapeWords);
    constexpr std::array<float, 3> kNone{0.0F, 0.0F, 0.0F};
    for (const SkinShape& shape : desc.shapes) {
        for (std::size_t i = 0; i < count; ++i) {
            push3(out.shapes, shape.positions.empty() ? kNone : shape.positions[i]);
            push3(out.shapes, shape.normals.empty() ? kNone : shape.normals[i]);
            push3(out.shapes, shape.tangents.empty() ? kNone : shape.tangents[i]);
        }
        out.turns.push_back(!shape.normals.empty() || !shape.tangents.empty());
    }
    return out;
}

SkinUniforms skin_pose(const SkinWords& skin, std::span<const SkinJoint> joints,
                       std::span<const float> shape_weights, std::vector<std::uint32_t>& poses) {
    SkinUniforms u;
    u.counts = {skin.vertex_count, static_cast<std::uint32_t>(joints.size()), skin.shape_count, 0};
    u.at = {static_cast<std::uint32_t>(poses.size()), 0, skin.weights.empty() ? 0U : 1U, 0};
    for (const SkinJoint& joint : joints) {
        push3(poses, joint.x);
        push3(poses, joint.y);
        push3(poses, joint.z);
        push3(poses, joint.origin);
    }
    u.at[1] = static_cast<std::uint32_t>(poses.size());
    for (std::uint32_t k = 0; k < skin.shape_count; ++k) {
        const float w = k < shape_weights.size() ? shape_weights[k] : 0.0F;
        poses.push_back(word_of(w));
        if (w != 0.0F && skin.turns[k]) {
            u.counts[3] = 1;
        }
    }
    // The shader reads a word even when the mesh has no joints or shapes.
    if (poses.empty()) {
        poses.push_back(0);
    }
    return u;
}

std::array<std::uint32_t, 2> skin_groups(std::uint32_t count) {
    constexpr std::uint32_t kAcross = 65536 / kSkinThreads;
    const std::uint32_t groups = (count + kSkinThreads - 1) / kSkinThreads;
    return {std::min(groups, kAcross), (groups + kAcross - 1) / kAcross};
}

} // namespace slash::render::detail
