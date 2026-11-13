// A character's anatomy as meshes on its rig (src/anatomy.hpp): the bones of its skeleton and,
// for "::muscles", its muscles over them, each moved by the rig's bones as flesh moves them.
#include "anatomy.hpp"

#include <algorithm>

namespace slash::scene3d::rig {

namespace {

real_t smooth(real_t from, real_t to, real_t x) {
    const real_t t = std::clamp<real_t>((x - from) / (to - from), 0.0F, 1.0F);
    return t * t * (3.0F - (2.0F * t));
}

std::size_t joint_of(const std::vector<Bone>& bones, std::string_view name) {
    for (std::size_t i = 0; i < bones.size(); ++i) {
        if (bones[i].name == name) {
            return i;
        }
    }
    return 0;
}

void add_bone(const world::AnatomyBone& bone, const std::vector<Bone>& bones, Builder& out,
              int material) {
    const std::size_t joint = joint_of(bones, bone.bone);
    for (const world::AnatomyPiece& piece : bone.pieces) {
        const Vec3 span = piece.b - piece.a;
        const real_t long_way = length(span);
        if (long_way < 0.0001F) {
            out.add(unit_sphere(), placed({piece.radius, piece.radius, piece.radius}, piece.a),
                    material, joint);
            continue;
        }
        out.add(capsule(static_cast<float>(piece.radius), static_cast<float>(long_way)),
                placed(facing(span), {1, 1, 1}, lerp(piece.a, piece.b, 0.5F)), material, joint);
    }
}

// A muscle as tapered tubes between its rings and round ends at its tendons, each corner moved
// by its origin's, insertion's and resting bones as its flesh's points are.
void add_muscle(const world::AnatomyMuscle& muscle, const std::vector<Bone>& bones, Builder& out,
                int belly, int tendon) {
    if (muscle.rings.size() < 2) {
        return;
    }
    const std::size_t origin = joint_of(bones, muscle.origin_bone);
    const std::size_t insertion = joint_of(bones, muscle.insertion_bone);
    const std::size_t rest = joint_of(bones, muscle.rest_bone);
    const Vec3 from = muscle.rings.front().center;
    const Vec3 to = muscle.rings.back().center;
    const Vec3 axis = to - from;
    const real_t axis2 = std::max<real_t>(dot(axis, axis), 0.000001F);
    const Weigh weigh = [=](Vec3 p) {
        const real_t along = std::clamp<real_t>(dot(p - from, axis) / axis2, 0.0F, 1.0F);
        const real_t to_origin = origin != rest ? smooth(0.35F, 0.0F, along) : 0.0F;
        const real_t to_insertion = insertion != rest ? smooth(0.65F, 1.0F, along) : 0.0F;
        VertexWeights weights = one_joint(rest);
        weights = mix_weights(weights, one_joint(origin), static_cast<float>(to_origin));
        return mix_weights(weights, one_joint(insertion), static_cast<float>(to_insertion));
    };
    for (std::size_t k = 0; k + 1 < muscle.rings.size(); ++k) {
        const world::MuscleRing& low = muscle.rings[k];
        const world::MuscleRing& high = muscle.rings[k + 1];
        const Vec3 span = high.center - low.center;
        const real_t long_way = length(span);
        if (long_way < 0.0001F || high.rv <= 0.0F) {
            continue;
        }
        // Its section square to this stretch: x across (v), y along, z toward its front (u).
        const Vec3 y = span * (1.0F / long_way);
        const Vec3 z = normalized(high.u - (y * dot(high.u, y)));
        Affine3D turn;
        turn.x = cross(y, z);
        turn.y = y;
        turn.z = z;
        out.add(open_tube(static_cast<float>(low.rv / high.rv)),
                placed(turn, {high.rv, long_way, high.ru}, lerp(low.center, high.center, 0.5F)),
                belly, weigh);
    }
    for (const world::MuscleRing* end : {&muscle.rings.front(), &muscle.rings.back()}) {
        out.add(unit_sphere(), placed({end->rv, end->rv, end->ru}, end->center), tendon, weigh);
    }
}

} // namespace

void add_anatomy(const world::Anatomy& anatomy, const std::vector<Bone>& bones, Builder& out,
                 bool muscles) {
    const int bone_material = out.material("bone", {0.9F, 0.86F, 0.76F, 1.0F}, 0.55F, 0.0F);
    for (const world::AnatomyBone& bone : anatomy.bones) {
        add_bone(bone, bones, out, bone_material);
    }
    if (!muscles) {
        return;
    }
    const int belly = out.material("muscle", {0.66F, 0.16F, 0.14F, 1.0F}, 0.45F, 0.0F);
    const int tendon = out.material("tendon", {0.86F, 0.82F, 0.74F, 1.0F}, 0.35F, 0.0F);
    for (const world::AnatomyMuscle& muscle : anatomy.muscles) {
        add_muscle(muscle, bones, out, belly, tendon);
    }
}

} // namespace slash::scene3d::rig
