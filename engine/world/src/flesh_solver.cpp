// Flesh: muscles' points and elements and how they step, after Unreal's Chaos Flesh, by XPBD
// with Macklin and Müller's Neo-Hookean constraints (slash/world/flesh.hpp).
#include "slash/world/flesh.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace slash::world {

namespace {

constexpr real_t kPi = std::numbers::pi_v<real_t>;
constexpr real_t kDensity = 1060.0F; // kilograms a cubic meter, muscle's
constexpr std::size_t kRingSides = 6;

real_t dot(Vec3 a, Vec3 b) {
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

Vec3 cross(Vec3 a, Vec3 b) {
    return {(a.y * b.z) - (a.z * b.y), (a.z * b.x) - (a.x * b.z), (a.x * b.y) - (a.y * b.x)};
}

real_t length(Vec3 v) {
    return std::sqrt(dot(v, v));
}

Vec3 normalized(Vec3 v) {
    const real_t l = length(v);
    return l > 0.0F ? v * (1.0F / l) : Vec3{};
}

Vec3 lerp(Vec3 a, Vec3 b, real_t t) {
    return a + ((b - a) * t);
}

real_t smooth(real_t from, real_t to, real_t x) {
    const real_t t = std::clamp<real_t>((x - from) / (to - from), 0.0F, 1.0F);
    return t * t * (3.0F - (2.0F * t));
}

// The turn that takes direction a onto direction b, the shorter way round.
Affine3D arc(Vec3 a, Vec3 b) {
    a = normalized(a);
    b = normalized(b);
    const real_t c = dot(a, b);
    Affine3D out;
    if (c < -0.9999F) {
        const Vec3 axis = normalized(std::abs(a.x) < 0.9F ? cross(a, {1.0F, 0.0F, 0.0F})
                                                          : cross(a, {0.0F, 1.0F, 0.0F}));
        out.x = (axis * (2.0F * axis.x)) - Vec3{1.0F, 0.0F, 0.0F};
        out.y = (axis * (2.0F * axis.y)) - Vec3{0.0F, 1.0F, 0.0F};
        out.z = (axis * (2.0F * axis.z)) - Vec3{0.0F, 0.0F, 1.0F};
        return out;
    }
    const Vec3 v = cross(a, b);
    const real_t k = 1.0F / (1.0F + c);
    const auto turn = [&](Vec3 u) { return (u * c) + cross(v, u) + (v * (dot(v, u) * k)); };
    out.x = turn({1.0F, 0.0F, 0.0F});
    out.y = turn({0.0F, 1.0F, 0.0F});
    out.z = turn({0.0F, 0.0F, 1.0F});
    return out;
}

Vec3 flat(Vec3 v, int dimensions) {
    return dimensions == 2 ? Vec3{v.x, v.y, 0.0F} : v;
}

int bone_slot(std::vector<std::string>& bones, const std::string& name) {
    const auto found = std::ranges::find(bones, name);
    if (found != bones.end()) {
        return static_cast<int>(found - bones.begin());
    }
    bones.push_back(name);
    return static_cast<int>(bones.size()) - 1;
}

// A 3x3 matrix's columns' inverse, column major; false where it can't be inverted.
bool invert3(Vec3 a, Vec3 b, Vec3 c, std::array<real_t, 9>& out) {
    const Vec3 r0 = cross(b, c);
    const Vec3 r1 = cross(c, a);
    const Vec3 r2 = cross(a, b);
    const real_t det = dot(a, r0);
    if (std::abs(det) < 1e-15F) {
        return false;
    }
    const real_t inv = 1.0F / det;
    // Rows of the inverse are r0, r1 and r2 over the determinant.
    out = {r0.x * inv, r1.x * inv, r2.x * inv, r0.y * inv, r1.y * inv,
           r2.y * inv, r0.z * inv, r1.z * inv, r2.z * inv};
    return true;
}

struct Builder {
    const Anatomy& anatomy;
    FleshBody& body;

    void add_point(const AnatomyMuscle& mu, int index, std::size_t ring, Vec3 at, Vec3 axis_from,
                   Vec3 axis, real_t span) {
        const auto rings = mu.rings.size();
        FleshPoint point;
        point.rest = flat(at, body.dimensions);
        point.along = static_cast<real_t>(ring) / static_cast<real_t>(rings - 1);
        point.radial =
            point.rest - flat(axis_from + (axis * (span * point.along)), body.dimensions);
        point.muscle = index;
        point.pinned = ring == 0 || ring + 1 == rings;
        const FleshMuscle& m = body.muscles.back();
        point.to_origin = m.origin_bone != m.rest_bone ? smooth(0.35F, 0.0F, point.along) : 0.0F;
        point.to_insertion =
            m.insertion_bone != m.rest_bone ? smooth(0.65F, 1.0F, point.along) : 0.0F;
        if (ring == 0) {
            point.to_origin = 1.0F;
            point.to_insertion = 0.0F;
        } else if (ring + 1 == rings) {
            point.to_origin = 0.0F;
            point.to_insertion = 1.0F;
        }
        body.points.push_back(point);
    }

    void add_element(std::array<std::uint32_t, 4> corners, int muscle, Vec3 fiber) {
        FleshElement e;
        e.corners = corners;
        e.muscle = muscle;
        e.fiber = fiber;
        const auto at = [&](std::size_t i) { return body.points[e.corners.at(i)].rest; };
        if (body.dimensions == 3) {
            const Vec3 a = at(1) - at(0);
            Vec3 b = at(2) - at(0);
            Vec3 c = at(3) - at(0);
            if (dot(a, cross(b, c)) < 0.0F) {
                std::swap(e.corners[2], e.corners[3]);
                b = at(2) - at(0);
                c = at(3) - at(0);
            }
            const real_t det = dot(a, cross(b, c));
            if (det <= 1e-12F || !invert3(a, b, c, e.rest_inverse)) {
                return;
            }
            e.volume = det / 6.0F;
        } else {
            Vec3 a = at(1) - at(0);
            Vec3 b = at(2) - at(0);
            if ((a.x * b.y) - (a.y * b.x) < 0.0F) {
                std::swap(e.corners[1], e.corners[2]);
                a = at(1) - at(0);
                b = at(2) - at(0);
            }
            const real_t det = (a.x * b.y) - (a.y * b.x);
            if (det <= 1e-12F) {
                return;
            }
            const real_t inv = 1.0F / det;
            // [a b] inverted: column major [d -c; -b a] / det of [[a.x b.x] [a.y b.y]].
            e.rest_inverse = {b.y * inv, -a.y * inv, -b.x * inv, a.x * inv, 0, 0, 0, 0, 0};
            e.volume = det * 0.5F;
        }
        body.elements.push_back(e);
    }

    void muscle(const AnatomyMuscle& mu) {
        if (mu.rings.size() < 3) {
            return;
        }
        const int index = static_cast<int>(body.muscles.size());
        FleshMuscle m;
        m.name = mu.name;
        m.kind = mu.kind;
        m.group = mu.group;
        m.origin = mu.origin;
        m.insertion = mu.insertion;
        m.rests_on = mu.rests_on;
        m.origin_bone = bone_slot(body.bones, mu.origin_bone);
        m.insertion_bone = bone_slot(body.bones, mu.insertion_bone);
        m.rest_bone = bone_slot(body.bones, mu.rest_bone);
        m.bone_a = flat(mu.bone_a, body.dimensions);
        m.bone_b = flat(mu.bone_b, body.dimensions);
        m.bone_radius = mu.bone_radius;
        m.origin_at = flat(mu.rings.front().center, body.dimensions);
        m.insertion_at = flat(mu.rings.back().center, body.dimensions);
        m.rest_length = length(m.insertion_at - m.origin_at);
        m.first_point = body.points.size();
        m.rings = mu.rings.size();
        m.ring_points = body.dimensions == 3 ? kRingSides + 1 : 3;
        const MuscleRing& middle = mu.rings[mu.rings.size() / 2];
        m.rest_radius = body.dimensions == 3 ? (middle.ru + middle.rv) * 0.5F : 0.0F;
        body.muscles.push_back(m);
        if (m.rest_length <= 0.0F) {
            body.muscles.pop_back();
            return;
        }
        const Vec3 axis = (m.insertion_at - m.origin_at) * (1.0F / m.rest_length);
        // In 2D, a muscle shows its outline square to it in the plane, as far as its section
        // reaches that way.
        const Vec3 across{-axis.y, axis.x, 0.0F};
        for (std::size_t k = 0; k < mu.rings.size(); ++k) {
            const MuscleRing& ring = mu.rings[k];
            add_point(mu, index, k, ring.center, m.origin_at, axis, m.rest_length);
            if (body.dimensions == 3) {
                for (std::size_t j = 0; j < kRingSides; ++j) {
                    const Vec2f cs = cos_sin(
                        static_cast<float>(2.0F * kPi * static_cast<real_t>(j) / kRingSides));
                    add_point(mu, index, k,
                              ring.center + (ring.u * (cs.x * ring.ru)) +
                                  (ring.v * (cs.y * ring.rv)),
                              m.origin_at, axis, m.rest_length);
                }
            } else {
                const real_t du = dot(ring.u, across) * ring.ru;
                const real_t dv = dot(ring.v, across) * ring.rv;
                const real_t reach = std::sqrt((du * du) + (dv * dv));
                for (const real_t sign : {1.0F, -1.0F}) {
                    add_point(mu, index, k, flat(ring.center, 2) + (across * (sign * reach)),
                              m.origin_at, axis, m.rest_length);
                }
            }
        }
        if (body.dimensions == 2) {
            const std::size_t center = m.first_point + ((mu.rings.size() / 2) * 3);
            body.muscles.back().rest_radius =
                length(body.points[center + 1].rest - body.points[center].rest);
        }
        const auto id = [&](std::size_t k, std::size_t j) {
            return static_cast<std::uint32_t>(m.first_point + (k * m.ring_points) + j);
        };
        for (std::size_t k = 0; k + 1 < mu.rings.size(); ++k) {
            if (body.dimensions == 3) {
                for (std::size_t j = 0; j < kRingSides; ++j) {
                    const std::size_t next = (j + 1) % kRingSides;
                    const std::uint32_t c0 = id(k, 0);
                    const std::uint32_t a0 = id(k, 1 + j);
                    const std::uint32_t b0 = id(k, 1 + next);
                    const std::uint32_t c1 = id(k + 1, 0);
                    const std::uint32_t a1 = id(k + 1, 1 + j);
                    const std::uint32_t b1 = id(k + 1, 1 + next);
                    add_element({c0, a0, b0, c1}, index, axis);
                    add_element({a0, b0, c1, a1}, index, axis);
                    add_element({b0, c1, a1, b1}, index, axis);
                }
            } else {
                for (const std::size_t side : {std::size_t{1}, std::size_t{2}}) {
                    add_element({id(k, 0), id(k, side), id(k + 1, side), 0}, index, axis);
                    add_element({id(k, 0), id(k + 1, side), id(k + 1, 0), 0}, index, axis);
                }
            }
        }
    }

    void masses() {
        std::vector<real_t> mass(body.points.size(), 0.0F);
        for (const FleshElement& e : body.elements) {
            real_t share = 0.0F;
            if (body.dimensions == 3) {
                share = kDensity * e.volume / 4.0F;
            } else {
                // A strip as thick as the muscle is across.
                const FleshMuscle& m = body.muscles[static_cast<std::size_t>(e.muscle)];
                share = kDensity * e.volume * (2.0F * m.rest_radius) / 3.0F;
            }
            const std::size_t corners = body.dimensions == 3 ? 4 : 3;
            for (std::size_t i = 0; i < corners; ++i) {
                mass[e.corners.at(i)] += share;
            }
        }
        for (std::size_t i = 0; i < body.points.size(); ++i) {
            FleshPoint& point = body.points[i];
            point.inverse_mass = point.pinned || mass[i] <= 0.0F ? 0.0F : 1.0F / mass[i];
        }
    }
};

// ---- Stepping ----

float activation_of(const FleshMuscle& m, const FleshSettings& settings, real_t ratio) {
    float manual = settings.activation;
    if (settings.muscles != nullptr) {
        for (const std::string* key : {&m.name, &m.kind, &m.group}) {
            if (const auto found = settings.muscles->find(*key); found != settings.muscles->end()) {
                manual = std::max(manual, found->second);
            }
        }
    }
    const float automatic =
        settings.automatic ? std::clamp(to_float((1.0F - ratio) * 4.0F), 0.0F, 1.0F) : 0.0F;
    return std::clamp(manual + automatic, 0.0F, 1.0F);
}

Affine3D move_of(std::span<const Affine3D> moves, int bone) {
    return bone >= 0 && static_cast<std::size_t>(bone) < moves.size()
               ? moves[static_cast<std::size_t>(bone)]
               : Affine3D{};
}

// Where the skeleton puts each point: carried by its bones, as a skin's vertex is, and the shape
// its muscle takes between where its ends are, its belly thicker as it shortens (keeping its
// volume) and as it is activated.
void place_targets(const FleshBody& body, FleshState& state, std::span<const Affine3D> moves,
                   const FleshSettings* settings) {
    const std::size_t n = body.points.size();
    state.targets.resize(n);
    state.skinned.resize(n);
    state.activations.resize(body.muscles.size(), 0.0F);
    for (std::size_t mi = 0; mi < body.muscles.size(); ++mi) {
        const FleshMuscle& m = body.muscles[mi];
        const Affine3D origin = move_of(moves, m.origin_bone);
        const Affine3D insertion = move_of(moves, m.insertion_bone);
        const Affine3D rest = move_of(moves, m.rest_bone);
        const Vec3 from = flat(origin.apply(m.origin_at), body.dimensions);
        const Vec3 to = flat(insertion.apply(m.insertion_at), body.dimensions);
        const real_t span = length(to - from);
        const real_t ratio = m.rest_length > 0.0F ? span / m.rest_length : 1.0F;
        const float a = settings != nullptr ? activation_of(m, *settings, ratio) : 0.0F;
        state.activations[mi] = a;
        const Vec3 axis0 = (m.insertion_at - m.origin_at) * (1.0F / m.rest_length);
        Affine3D turn = rest;
        turn.origin = {};
        const Affine3D shape = arc(turn.basis(axis0), to - from) * turn;
        const real_t thicker =
            std::clamp<real_t>(std::sqrt(1.0F / std::max<real_t>(ratio, 0.01F)), 0.75F, 1.5F);
        const std::size_t count = m.rings * m.ring_points;
        for (std::size_t i = m.first_point; i < m.first_point + count; ++i) {
            const FleshPoint& p = body.points[i];
            const real_t w_rest = 1.0F - p.to_origin - p.to_insertion;
            const Vec3 carried = (origin.apply(p.rest) * p.to_origin) +
                                 (insertion.apply(p.rest) * p.to_insertion) +
                                 (rest.apply(p.rest) * w_rest);
            state.skinned[i] = flat(carried, body.dimensions);
            if (p.pinned) {
                state.targets[i] = state.skinned[i];
                continue;
            }
            const real_t belly = cos_sin(static_cast<float>(kPi * p.along)).y;
            const real_t scale = thicker * (1.0F + (0.35F * a * belly));
            state.targets[i] =
                flat(lerp(from, to, p.along) + (shape.basis(p.radial) * scale), body.dimensions);
        }
    }
}

struct Solver {
    const FleshBody& body;
    FleshState& state;
    real_t h2 = 0.0F;
    real_t mu = 0.0F;

    // Moves an element's corners by one constraint, given its value and its gradient as to F.
    void project(const FleshElement& e, real_t c, const std::array<Vec3, 3>& p, real_t stiffness) {
        if (stiffness <= 0.0F || e.volume <= 0.0F) {
            return;
        }
        const std::size_t edges = body.dimensions == 3 ? 3 : 2;
        std::array<Vec3, 4> g{};
        for (std::size_t i = 0; i < edges; ++i) {
            Vec3 sum;
            for (std::size_t k = 0; k < edges; ++k) {
                sum = sum + (p.at(k) * e.rest_inverse.at((k * edges) + i));
            }
            g.at(i + 1) = sum;
            g[0] = g[0] - sum;
        }
        real_t weight = 0.0F;
        for (std::size_t i = 0; i <= edges; ++i) {
            weight += body.points[e.corners.at(i)].inverse_mass * dot(g.at(i), g.at(i));
        }
        const real_t compliance = 1.0F / (stiffness * e.volume * h2);
        const real_t denominator = weight + compliance;
        if (denominator <= 0.0F) {
            return;
        }
        const real_t delta = -c / denominator;
        for (std::size_t i = 0; i <= edges; ++i) {
            const std::uint32_t at = e.corners.at(i);
            state.positions[at] =
                state.positions[at] + (g.at(i) * (body.points[at].inverse_mass * delta));
        }
    }

    void element(const FleshElement& e, float activation) {
        const std::size_t edges = body.dimensions == 3 ? 3 : 2;
        const Vec3 x0 = state.positions[e.corners[0]];
        std::array<Vec3, 3> d{};
        for (std::size_t i = 0; i < edges; ++i) {
            d.at(i) = state.positions[e.corners.at(i + 1)] - x0;
        }
        // F's columns: the deformed edges times the rest edges' inverse.
        const auto columns = [&]() {
            std::array<Vec3, 3> f{};
            for (std::size_t k = 0; k < edges; ++k) {
                Vec3 sum;
                for (std::size_t i = 0; i < edges; ++i) {
                    sum = sum + (d.at(i) * e.rest_inverse.at((k * edges) + i));
                }
                f.at(k) = sum;
            }
            return f;
        };
        // Deviatoric: F's size, from its rest's.
        std::array<Vec3, 3> f = columns();
        real_t ic = 0.0F;
        for (std::size_t k = 0; k < edges; ++k) {
            ic += dot(f.at(k), f.at(k));
        }
        if (ic > 0.0F) {
            const real_t size = std::sqrt(ic);
            const real_t rest = body.dimensions == 3 ? std::numbers::sqrt3_v<real_t>
                                                     : std::numbers::sqrt2_v<real_t>;
            std::array<Vec3, 3> grad{};
            for (std::size_t k = 0; k < edges; ++k) {
                grad.at(k) = f.at(k) * (1.0F / size);
            }
            project(e, size - rest, grad, mu);
        }
        // Hydrostatic: its volume.
        for (std::size_t i = 0; i < edges; ++i) {
            d.at(i) = state.positions[e.corners.at(i + 1)] - state.positions[e.corners[0]];
        }
        f = columns();
        std::array<Vec3, 3> grad{};
        real_t det = 0.0F;
        if (body.dimensions == 3) {
            grad = {cross(f[1], f[2]), cross(f[2], f[0]), cross(f[0], f[1])};
            det = dot(f[0], grad[0]);
        } else {
            det = (f[0].x * f[1].y) - (f[1].x * f[0].y);
            grad = {Vec3{f[1].y, -f[1].x, 0.0F}, Vec3{-f[0].y, f[0].x, 0.0F}, Vec3{}};
        }
        project(e, det - 1.0F, grad, mu * 30.0F);
        // Active: its fibers shorten as it is activated.
        if (activation > 0.0F) {
            for (std::size_t i = 0; i < edges; ++i) {
                d.at(i) = state.positions[e.corners.at(i + 1)] - state.positions[e.corners[0]];
            }
            f = columns();
            const std::array<real_t, 3> a0{e.fiber.x, e.fiber.y, e.fiber.z};
            Vec3 along;
            for (std::size_t k = 0; k < edges; ++k) {
                along = along + (f.at(k) * a0.at(k));
            }
            const real_t span = length(along);
            if (span > 0.0F) {
                const Vec3 n = along * (1.0F / span);
                std::array<Vec3, 3> fiber_grad{};
                for (std::size_t k = 0; k < edges; ++k) {
                    fiber_grad.at(k) = n * a0.at(k);
                }
                project(e, span - (1.0F - (0.3F * activation)), fiber_grad, mu * 4.0F * activation);
            }
        }
    }

    // Out of the bone the muscle rests on.
    void collide(const FleshMuscle& m, const Affine3D& rest) {
        const Vec3 a = flat(rest.apply(m.bone_a), body.dimensions);
        const Vec3 b = flat(rest.apply(m.bone_b), body.dimensions);
        const Vec3 span = b - a;
        const real_t span2 = std::max<real_t>(dot(span, span), 1e-12F);
        const std::size_t count = m.rings * m.ring_points;
        for (std::size_t i = m.first_point; i < m.first_point + count; ++i) {
            if (body.points[i].pinned) {
                continue;
            }
            Vec3& x = state.positions[i];
            const real_t t = std::clamp<real_t>(dot(x - a, span) / span2, 0.0F, 1.0F);
            const Vec3 away = x - lerp(a, b, t);
            const real_t d = length(away);
            if (d < m.bone_radius && d > 0.0F) {
                x = x + (away * ((m.bone_radius - d) / d));
            }
        }
    }
};

} // namespace

FleshBody build_flesh(const Anatomy& anatomy) {
    FleshBody body;
    body.dimensions = anatomy.dimensions;
    Builder builder{anatomy, body};
    for (const AnatomyMuscle& mu : anatomy.muscles) {
        builder.muscle(mu);
    }
    builder.masses();
    return body;
}

FleshSettings flesh_settings(const Flesh3D& flesh) {
    return {.stiffness = flesh.stiffness,
            .damping = flesh.damping,
            .jiggle = flesh.jiggle,
            .activation = flesh.activation,
            .automatic = flesh.automatic,
            .muscles = &flesh.muscles,
            .substeps = flesh.substeps};
}

FleshSettings flesh_settings(const Flesh2D& flesh) {
    return {.stiffness = flesh.stiffness,
            .damping = flesh.damping,
            .jiggle = flesh.jiggle,
            .activation = flesh.activation,
            .automatic = flesh.automatic,
            .muscles = &flesh.muscles,
            .substeps = flesh.substeps};
}

void reset_flesh(const FleshBody& body, FleshState& state, std::span<const Affine3D> moves) {
    place_targets(body, state, moves, nullptr);
    state.positions = state.targets;
    state.velocities.assign(body.points.size(), Vec3{});
    state.started = true;
}

void step_flesh(const FleshBody& body, FleshState& state, const FleshStep& step) {
    if (!state.started || state.positions.size() != body.points.size()) {
        reset_flesh(body, state, step.moves);
    }
    const std::vector<Vec3> pinned_from = state.targets;
    place_targets(body, state, step.moves, &step.settings);
    const int substeps = std::clamp(step.settings.substeps, 1, 16);
    const real_t h = step.seconds / static_cast<real_t>(substeps);
    if (h <= 0.0F) {
        return;
    }
    const FleshSettings& s = step.settings;
    Solver solver{body, state};
    solver.h2 = h * h;
    solver.mu = 3000.0F * slash::exp(std::clamp(s.stiffness, 0.0F, 1.0F) * slash::ln(30.0F));
    // Tethers spring each point to its shape at a frequency loose flesh lowers.
    const real_t omega = 2.0F * kPi * (4.0F + (26.0F * (1.0F - std::clamp(s.jiggle, 0.0F, 1.0F))));
    const real_t pull = (omega * omega * solver.h2) / ((omega * omega * solver.h2) + 1.0F);
    const real_t drag = 1.0F / (1.0F + (h * (1.0F + (40.0F * std::clamp(s.damping, 0.0F, 1.0F)))));
    const Vec3 push = flat(step.acceleration, body.dimensions);
    std::vector<Vec3> before(body.points.size());
    for (int sub = 0; sub < substeps; ++sub) {
        const real_t share = static_cast<real_t>(sub + 1) / static_cast<real_t>(substeps);
        for (std::size_t i = 0; i < body.points.size(); ++i) {
            before[i] = state.positions[i];
            if (body.points[i].pinned) {
                state.positions[i] = lerp(pinned_from[i], state.targets[i], share);
                continue;
            }
            state.velocities[i] = state.velocities[i] + (push * h);
            state.positions[i] = state.positions[i] + (state.velocities[i] * h);
        }
        for (const FleshElement& e : body.elements) {
            solver.element(e, state.activations[static_cast<std::size_t>(e.muscle)]);
        }
        for (std::size_t i = 0; i < body.points.size(); ++i) {
            if (!body.points[i].pinned) {
                state.positions[i] =
                    state.positions[i] + ((state.targets[i] - state.positions[i]) * pull);
            }
        }
        for (const FleshMuscle& m : body.muscles) {
            solver.collide(m, move_of(step.moves, m.rest_bone));
        }
        for (std::size_t i = 0; i < body.points.size(); ++i) {
            state.velocities[i] = (state.positions[i] - before[i]) * (drag / h);
        }
    }
}

std::vector<MuscleReport> muscle_reports(const FleshBody& body, const FleshState& state) {
    std::vector<MuscleReport> out;
    for (std::size_t mi = 0; mi < body.muscles.size(); ++mi) {
        const FleshMuscle& m = body.muscles[mi];
        MuscleReport report;
        report.name = m.name;
        report.kind = m.kind;
        report.group = m.group;
        if (state.positions.size() == body.points.size() && m.rings > 0) {
            const Vec3 from = state.positions[m.first_point];
            const Vec3 to = state.positions[m.first_point + ((m.rings - 1) * m.ring_points)];
            report.length =
                m.rest_length > 0.0F ? static_cast<float>(length(to - from) / m.rest_length) : 1.0F;
            const std::size_t middle = m.first_point + ((m.rings / 2) * m.ring_points);
            real_t now = 0.0F;
            real_t rest = 0.0F;
            for (std::size_t j = 1; j < m.ring_points; ++j) {
                now += length(state.positions[middle + j] - state.positions[middle]);
                rest += length(body.points[middle + j].rest - body.points[middle].rest);
            }
            report.bulge = rest > 0.0F ? static_cast<float>((now / rest) - 1.0F) : 0.0F;
            report.activation = mi < state.activations.size() ? state.activations[mi] : 0.0F;
        }
        out.push_back(std::move(report));
    }
    return out;
}

FleshBinding bind_skin(const FleshBody& body, std::span<const SkinPoint> skin, float fat) {
    FleshBinding binding;
    binding.first.reserve(skin.size() + 1);
    const real_t softened = 1.0F / (1.0F + std::clamp(fat, 0.0F, 1.0F));
    struct Near {
        std::size_t muscle = 0;
        real_t distance = 0.0F;
        real_t reach = 0.0F;
    };
    for (const SkinPoint& point : skin) {
        binding.first.push_back(static_cast<std::uint32_t>(binding.entries.size()));
        std::vector<Near> near;
        for (std::size_t mi = 0; mi < body.muscles.size(); ++mi) {
            const FleshMuscle& m = body.muscles[mi];
            if (!(m.origin == point.part || m.insertion == point.part ||
                  m.rests_on == point.part)) {
                continue;
            }
            real_t best = -1.0F;
            for (std::size_t k = 0; k < m.rings; ++k) {
                for (std::size_t j = 1; j < m.ring_points; ++j) {
                    const FleshPoint& p = body.points[m.first_point + (k * m.ring_points) + j];
                    const real_t d = length(p.rest - flat(point.rest, body.dimensions));
                    best = best < 0.0F ? d : std::min(best, d);
                }
            }
            const real_t reach = m.rest_radius * (2.5F + (3.0F * std::clamp(fat, 0.0F, 1.0F)));
            if (best >= 0.0F && best < reach) {
                near.push_back({.muscle = mi, .distance = best, .reach = reach});
            }
        }
        std::ranges::stable_sort(near, [](const Near& a, const Near& b) {
            return a.distance / a.reach < b.distance / b.reach;
        });
        if (near.size() > 2) {
            near.resize(2);
        }
        real_t total = 0.0F;
        for (const Near& each : near) {
            const real_t f = 1.0F - (each.distance / each.reach);
            total += f * f;
        }
        for (const Near& each : near) {
            const FleshMuscle& m = body.muscles[each.muscle];
            const real_t f = 1.0F - (each.distance / each.reach);
            const real_t share = (f * f) / std::max<real_t>(total, 1.0F) * softened;
            // The three surface points nearest, by inverse squared distance.
            std::array<std::pair<real_t, std::uint32_t>, 3> best{};
            std::size_t found = 0;
            for (std::size_t k = 0; k < m.rings; ++k) {
                for (std::size_t j = 1; j < m.ring_points; ++j) {
                    const auto at =
                        static_cast<std::uint32_t>(m.first_point + (k * m.ring_points) + j);
                    const real_t d =
                        length(body.points[at].rest - flat(point.rest, body.dimensions));
                    if (found < best.size()) {
                        best.at(found++) = {d, at};
                    } else if (d < best.back().first) {
                        best.back() = {d, at};
                    } else {
                        continue;
                    }
                    std::sort(best.begin(), best.begin() + static_cast<std::ptrdiff_t>(found));
                }
            }
            real_t sum = 0.0F;
            const real_t soft = m.rest_radius * 0.1F;
            for (std::size_t i = 0; i < found; ++i) {
                sum += 1.0F / ((best.at(i).first * best.at(i).first) + (soft * soft));
            }
            for (std::size_t i = 0; i < found; ++i) {
                const real_t w = 1.0F / ((best.at(i).first * best.at(i).first) + (soft * soft));
                binding.entries.emplace_back(best.at(i).second,
                                             static_cast<float>(share * w / sum));
            }
        }
    }
    binding.first.push_back(static_cast<std::uint32_t>(binding.entries.size()));
    return binding;
}

void skin_offsets(const FleshState& state, const FleshBinding& binding, std::vector<Vec3>& out) {
    const std::size_t count = binding.first.empty() ? 0 : binding.first.size() - 1;
    out.assign(count, Vec3{});
    if (state.positions.size() != state.skinned.size()) {
        return;
    }
    for (std::size_t i = 0; i < count; ++i) {
        Vec3 sum;
        for (std::uint32_t e = binding.first[i]; e < binding.first[i + 1]; ++e) {
            const auto& [point, weight] = binding.entries[e];
            if (point < state.positions.size()) {
                sum = sum + ((state.positions[point] - state.skinned[point]) * weight);
            }
        }
        out[i] = sum;
    }
}

} // namespace slash::world
