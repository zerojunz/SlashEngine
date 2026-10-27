#include "slash/scene3d/gpu_particles.hpp"

#include "slash/core/json.hpp"
#include "slash/core/log.hpp"
#include "slash/scene3d/particles.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <utility>

namespace slash::scene3d {
namespace {

using world::Affine3D;
using world::GPUParticles3D;

// A new number for each emitter made, in any world.
std::uint64_t next_serial() {
    static std::atomic<std::uint64_t> serial{0};
    return ++serial;
}

// One GPUParticles3D's state: Godot's GPUParticles3D node and its particles storage, and the steps
// kept. As canvas's GPUParticles2D emitter, without sub-emitters, trails or collision.
struct Emitter {
    std::uint64_t serial = next_serial();
    // The node, and what the component said when last seen.
    bool seen = false;
    bool emitting = false;
    bool one_shot = false;
    std::uint32_t amount = 1;
    std::uint32_t restarts = 0; // one_shot restarts, which vary the seed
    Vec3 previous_position;
    bool active = false;
    bool canceled = false;
    double time = 0.0;
    double emission_time = 0.0;
    double active_time = 0.0;
    // The storage.
    bool storage_emitting = false;
    bool inactive = true;
    bool clear = true;
    bool restart = false;
    float inactive_time = 0.0F;
    float remainder = 0.0F;
    double phase = 0.0;
    std::uint32_t cycle = 0;
    // Seconds since the storage started over, back to 0 every hour: the steps' time.
    double particle_time = 0.0;
    // The steps kept are steps[start] on, the first of them step number first.
    std::vector<render::ParticleStep3D> steps;
    std::size_t start = 0;
    std::uint64_t first = 0;
    // The particle lifetimes the steps kept after steps[start] cover.
    double covered = 0.0;

    [[nodiscard]] std::span<const render::ParticleStep3D> kept() const {
        return {steps.data() + start, steps.size() - start};
    }

    void drop_front() {
        ++start;
        ++first;
        if (start < steps.size()) {
            covered -= steps[start].delta / steps[start].lifetime;
        }
    }

    void drop_all() {
        first += steps.size() - start;
        steps.clear();
        start = 0;
        covered = 0.0;
    }

    // Keeps a step and as many before it as can still have made a live particle: a particle
    // lives at most a lifetime, so a step more than a lifetime of steps before the last made none
    // that live (with a hundredth to spare for rounding).
    void keep(const render::ParticleStep3D& step) {
        // A step that moves nothing changes nothing.
        if (step.delta <= 0.0F && !step.clear) {
            return;
        }
        if (step.clear) {
            drop_all();
        }
        if (start < steps.size()) {
            covered += step.delta / step.lifetime;
        }
        steps.push_back(step);
        while (steps.size() - start > 1) {
            const render::ParticleStep3D& next = steps[start + 1];
            if (covered - (next.delta / next.lifetime) < 1.01 &&
                steps.size() - start <= kMaxParticleSteps3D) {
                break;
            }
            drop_front();
        }
        // Lets go of the room the steps dropped took.
        if (start > 64 && start * 2 > steps.size()) {
            steps.erase(steps.begin(), steps.begin() + static_cast<std::ptrdiff_t>(start));
            start = 0;
        }
    }
};

std::uint32_t amount_of(const GPUParticles3D& settings) {
    return static_cast<std::uint32_t>(
        std::clamp(settings.amount, 1, static_cast<int>(render::kMaxParticles3D)));
}

double lifetime_of(const GPUParticles3D& settings) {
    return std::max(static_cast<double>(settings.lifetime), 0.001);
}

int fixed_fps_of(const GPUParticles3D& settings) {
    return std::clamp(settings.fixed_fps, 0, 1000);
}

// GPUParticles3D::set_emitting.
void set_emitting(Emitter& e, const GPUParticles3D& settings, bool emitting, bool first) {
    if (emitting && emitting != e.emitting && !settings.use_fixed_seed && settings.one_shot &&
        !first) {
        ++e.restarts;
    }
    if (emitting && settings.one_shot) {
        if (!e.active && !e.emitting) {
            // The last cycle ended.
            e.active = true;
            e.time = 0.0;
            e.canceled = false;
            e.emission_time = lifetime_of(settings);
            e.active_time =
                lifetime_of(settings) *
                (2.0 - std::clamp(static_cast<double>(settings.explosiveness), 0.0, 1.0));
            // As GPUParticles2D's: the storage starts over, so a burst always emits.
            e.restart = !first;
        } else {
            e.canceled = true;
        }
    }
    e.emitting = emitting;
    e.storage_emitting = emitting;
}

class Stepper {
public:
    Stepper(const GPUParticles3D& settings, const Affine3D& transform, Vec3 velocity,
            std::uint32_t seed, const std::vector<render::ParticleAttractor3D>& attractors)
        : s_(settings), transform_(transform), velocity_(velocity), seed_(seed),
          attractors_(attractors) {}

    // ParticlesStorage::_particles_process.
    void process(Emitter& e, double delta) const {
        const double new_phase = std::fmod(e.phase + (delta / lifetime_of(s_)), 1.0);
        if (e.clear) {
            e.cycle = 0;
            e.particle_time = 0.0;
        } else if (new_phase < e.phase) {
            if (s_.one_shot) {
                e.storage_emitting = false;
            }
            ++e.cycle;
        }
        render::ParticleStep3D step;
        if (!s_.local_coords) {
            step.emission = particle_transform(transform_);
        }
        step.emitter_velocity = velocity_;
        step.emitting = e.storage_emitting;
        step.system_phase = static_cast<float>(new_phase);
        step.prev_system_phase = static_cast<float>(e.phase);
        e.phase = new_phase;
        step.delta = static_cast<float>(delta);
        step.lifetime = static_cast<float>(lifetime_of(s_));
        step.explosiveness = std::clamp(s_.explosiveness, 0.0F, 1.0F);
        step.randomness = std::clamp(s_.randomness, 0.0F, 1.0F);
        step.amount_ratio = std::clamp(s_.amount_ratio, 0.0F, 1.0F);
        step.interp_to_end = std::clamp(s_.interp_to_end, 0.0F, 1.0F);
        e.particle_time = std::fmod(e.particle_time + delta, 3600.0);
        step.time = static_cast<float>(e.particle_time);
        step.cycle = e.cycle;
        step.seed = seed_;
        step.fractional_delta = s_.fract_delta;
        step.clear = e.clear;
        step.attractors = attractors_;
        e.clear = false;
        e.keep(step);
    }

    // ParticlesStorage::update_particles, for a tick of seconds.
    void update(Emitter& e, double seconds) const {
        if (e.restart) {
            e.phase = 0.0;
            e.clear = true;
            e.restart = false;
            e.remainder = 0.0F;
        }
        if (e.inactive && !e.storage_emitting) {
            return;
        }
        if (e.storage_emitting) {
            if (e.inactive) {
                // Starts again from scratch.
                e.phase = 0.0;
                e.clear = true;
            }
            e.inactive = false;
            e.inactive_time = 0.0F;
        } else {
            // Godot's storage waits 1.2 lifetimes for the last particles to die.
            e.inactive_time += s_.speed_scale * static_cast<float>(seconds);
            if (e.inactive_time > lifetime_of(s_) * 1.2) {
                e.inactive = true;
                return;
            }
        }
        const int fixed_fps = fixed_fps_of(s_);
        if (e.clear && s_.preprocess > 0.0F) {
            const float frame_time = 1.0F / static_cast<float>(fixed_fps > 0 ? fixed_fps : 30);
            float todo = s_.preprocess;
            while (todo > 0.0F) {
                process(e, std::min(frame_time, todo));
                todo -= frame_time;
            }
        }
        const float time_scale = std::max(s_.speed_scale, 0.0F);
        if (fixed_fps > 0) {
            const float frame_time = 1.0F / static_cast<float>(fixed_fps);
            // Fewer than 10 ticks a second runs slower rather than stall, as Godot.
            const float delta = std::clamp(static_cast<float>(seconds), 0.0F, 0.1F);
            float todo = e.remainder + (delta * time_scale);
            while (todo >= frame_time || e.clear) {
                process(e, frame_time);
                todo -= frame_time;
            }
            e.remainder = todo;
        } else {
            process(e, seconds * time_scale);
            e.remainder = 0.0F;
        }
    }

private:
    const GPUParticles3D& s_;
    const Affine3D& transform_;
    Vec3 velocity_;
    std::uint32_t seed_;
    const std::vector<render::ParticleAttractor3D>& attractors_;
};

// One tick of an emitter: the node's part, which may set the component's emitting, then the
// storage's. Returns whether a one_shot finished.
bool tick(Emitter& e, GPUParticles3D& settings, const Affine3D& transform, std::string_view id,
          double seconds, const std::vector<render::ParticleAttractor3D>& attractors = {}) {
    const std::uint32_t amount = amount_of(settings);
    if (!e.seen) {
        // As a node made with these properties: emitting set last.
        e.seen = true;
        e.amount = amount;
        e.one_shot = settings.one_shot;
        e.previous_position = transform.origin;
        set_emitting(e, settings, settings.emitting, true);
    } else {
        if (amount != e.amount) {
            // ParticlesStorage::particles_set_amount: the particles go.
            e.amount = amount;
            e.drop_all();
            e.phase = 0.0;
            e.clear = true;
        }
        if (settings.one_shot != e.one_shot) {
            // GPUParticles3D::set_one_shot.
            e.one_shot = settings.one_shot;
            if (e.emitting && !settings.one_shot) {
                e.restart = true;
            }
        }
        if (settings.emitting != e.emitting) {
            set_emitting(e, settings, settings.emitting, false);
        }
    }
    // NOTIFICATION_INTERNAL_PROCESS.
    bool finished = false;
    if (settings.one_shot) {
        e.time += seconds;
        const double scaled = e.time * settings.speed_scale;
        if (scaled > e.emission_time) {
            e.emitting = false;
        }
        if (scaled > e.active_time) {
            finished = e.active && !e.canceled;
            e.active = false;
        }
    }
    settings.emitting = e.emitting;
    // GPUParticles3D's velocity, from where it was the tick before.
    Vec3 velocity;
    if (seconds > 0.0) {
        const auto per_second = static_cast<float>(1.0 / seconds);
        velocity = (transform.origin - e.previous_position) * per_second;
    }
    e.previous_position = transform.origin;
    Stepper(settings, transform, velocity, gpu_particle_seed(settings, id, e.restarts), attractors)
        .update(e, seconds);
    return finished;
}

// An axis-aligned box, as Godot's AABB, by its corners.
struct Box3 {
    Vec3 low;
    Vec3 high;
};

// A box in an entity's space placed by its transform, as Godot's Transform3D::xform(AABB).
Box3 placed(const Affine3D& transform, Vec3 position, Vec3 size) {
    Box3 out{transform.origin, transform.origin};
    const std::array<Vec3, 3> axes{transform.x, transform.y, transform.z};
    const std::array<float, 3> low{position.x, position.y, position.z};
    const std::array<float, 3> high{position.x + size.x, position.y + size.y, position.z + size.z};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const Vec3 a = axes.at(axis) * low.at(axis);
        const Vec3 b = axes.at(axis) * high.at(axis);
        out.low = out.low + Vec3{std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)};
        out.high = out.high + Vec3{std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)};
    }
    return out;
}

// As Godot's AABB::intersects: boxes that only touch don't.
bool intersects(const Box3& a, const Box3& b) {
    return a.low.x < b.high.x && b.low.x < a.high.x && a.low.y < b.high.y && b.low.y < a.high.y &&
           a.low.z < b.high.z && b.low.z < a.high.z;
}

float length_of(Vec3 v) {
    return std::sqrt((v.x * v.x) + (v.y * v.y) + (v.z * v.z));
}

// Basis::get_scale: the axes' lengths, negative when the basis mirrors.
Vec3 scale_of(const Affine3D& t) {
    const float determinant = (t.x.x * ((t.y.y * t.z.z) - (t.z.y * t.y.z))) -
                              (t.y.x * ((t.x.y * t.z.z) - (t.z.y * t.x.z))) +
                              (t.z.x * ((t.x.y * t.y.z) - (t.y.y * t.x.z)));
    float sign = 0.0F;
    if (determinant > 0.0F) {
        sign = 1.0F;
    } else if (determinant < 0.0F) {
        sign = -1.0F;
    }
    return Vec3{length_of(t.x), length_of(t.y), length_of(t.z)} * sign;
}

// An attractor in the world, as the gpu_particles_3d system found it this tick.
struct FoundAttractor {
    render::AttractorShape3D shape = render::AttractorShape3D::Sphere;
    Affine3D transform;
    Box3 bounds;
    std::uint32_t cull_mask = 0;
    float strength = 1.0F;
    float attenuation = 1.0F;
    float directionality = 0.0F;
    float radius = 1.0F;
    Vec3 extents;
    std::shared_ptr<const render::ParticleField3D> field;
};

// An attractor component's shape and settings; attractors() sets the rest.
template <typename Attractor>
FoundAttractor found_of(render::AttractorShape3D shape, const Attractor& attractor) {
    FoundAttractor out;
    out.shape = shape;
    out.cull_mask = attractor.cull_mask;
    out.strength = attractor.strength;
    out.attenuation = attractor.attenuation;
    out.directionality = attractor.directionality;
    return out;
}

// The attractor as an emitter's steps take it (ParticlesStorage::_particles_process): in the
// particles' space, its basis made orthonormal and its size scaled.
render::ParticleAttractor3D for_particles(const FoundAttractor& found,
                                          const Affine3D* to_particles) {
    const Affine3D to_collider =
        to_particles != nullptr ? *to_particles * found.transform : found.transform;
    const Vec3 scale = scale_of(to_collider);
    render::ParticleAttractor3D out;
    out.shape = found.shape;
    out.transform = particle_transform(world::orthonormalized(to_collider));
    out.strength = found.strength;
    out.attenuation = found.attenuation;
    out.directionality = found.directionality;
    if (found.shape == render::AttractorShape3D::Sphere) {
        const float radius = found.radius * ((scale.x + scale.y + scale.z) / 3.0F);
        out.extents = {radius, radius, radius};
    } else {
        out.extents = {found.extents.x * scale.x, found.extents.y * scale.y,
                       found.extents.z * scale.z};
        out.field = found.field;
    }
    return out;
}

// A field of one white texel, which Godot's vector fields without a texture read.
std::shared_ptr<const render::ParticleField3D> white_field() {
    static const auto kWhite =
        std::make_shared<const render::ParticleField3D>(render::ParticleField3D{
            .width = 1, .height = 1, .depth = 1, .texels = {Vec4{1.0F, 1.0F, 1.0F, 1.0F}}});
    return kWhite;
}

// What an entity's emitter is kept under: its ID, which outlives the entity handles that restoring
// a copy of the world (a failed command, a dry run, undo) hands out anew.
std::string key_of(const world::World& world, entt::entity entity) {
    const std::string_view id = world.id_of(entity);
    return id.empty() ? "#" + std::to_string(entt::to_integral(entity)) : std::string(id);
}

struct GpuParticleSystem3D {
    std::map<std::string, Emitter, std::less<>> emitters;
    ImageLoader images;
    // Each vector field's texels by texture and slices, made again when the image changes.
    struct Field {
        std::shared_ptr<const render::Image> image;
        std::shared_ptr<const render::ParticleField3D> field;
    };
    std::map<std::tuple<std::string, int, int>, Field> fields;
    // The textures that couldn't be used, warned about once.
    std::set<std::tuple<std::string, int, int>> warned;

    // A vector field's texels: its texture cut into slices, or white without one or when it
    // can't be read or cut.
    std::shared_ptr<const render::ParticleField3D>
    field_of(const world::GPUParticlesAttractorVectorField3D& attractor) {
        if (attractor.texture.empty()) {
            return white_field();
        }
        auto key = std::make_tuple(attractor.texture, attractor.slices_horizontal,
                                   attractor.slices_vertical);
        const auto fail = [&](const std::string& message) {
            if (warned.insert(key).second) {
                log::warn("vector field reads as white",
                          {{"texture", attractor.texture}, {"reason", message}});
            }
            return white_field();
        };
        if (!images) {
            return fail("textures can't load here");
        }
        auto image = images(attractor.texture);
        if (!image) {
            return fail(image.error().message);
        }
        const auto found = fields.find(key);
        if (found != fields.end() && found->second.image == image.value()) {
            return found->second.field;
        }
        auto field =
            particle_field(*image.value(), attractor.slices_horizontal, attractor.slices_vertical);
        if (!field) {
            return fail(field.error().message);
        }
        warned.erase(key);
        auto made = std::make_shared<const render::ParticleField3D>(std::move(field).value());
        fields.insert_or_assign(std::move(key), Field{.image = image.value(), .field = made});
        return made;
    }

    // Every attractor visible in the tree, in the order entities are stored.
    std::vector<FoundAttractor> attractors(const world::World& world, const world::Tree3D& tree) {
        std::vector<FoundAttractor> found;
        const auto& ecs = world.ecs();
        const auto add = [&](entt::entity entity, FoundAttractor attractor, Vec3 size) {
            if (!tree.visible(entity)) {
                return;
            }
            attractor.transform = tree.global(entity);
            attractor.bounds = placed(attractor.transform, size * -0.5F, size);
            found.push_back(std::move(attractor));
        };
        for (const entt::entity entity : ecs.view<const world::GPUParticlesAttractorSphere3D>()) {
            const auto& a = ecs.get<const world::GPUParticlesAttractorSphere3D>(entity);
            FoundAttractor sphere = found_of(render::AttractorShape3D::Sphere, a);
            sphere.radius = a.radius;
            add(entity, std::move(sphere), Vec3{a.radius, a.radius, a.radius} * 2.0F);
        }
        for (const entt::entity entity : ecs.view<const world::GPUParticlesAttractorBox3D>()) {
            const auto& a = ecs.get<const world::GPUParticlesAttractorBox3D>(entity);
            FoundAttractor box = found_of(render::AttractorShape3D::Box, a);
            box.extents = a.size * 0.5F;
            add(entity, std::move(box), a.size);
        }
        for (const entt::entity entity :
             ecs.view<const world::GPUParticlesAttractorVectorField3D>()) {
            const auto& a = ecs.get<const world::GPUParticlesAttractorVectorField3D>(entity);
            FoundAttractor field = found_of(render::AttractorShape3D::VectorField, a);
            field.extents = a.size * 0.5F;
            field.field = field_of(a);
            add(entity, std::move(field), a.size);
        }
        return found;
    }

    void run_all(world::World& world, double tick_seconds) {
        auto& ecs = world.ecs();
        std::map<std::string, entt::entity, std::less<>> by_key;
        for (const entt::entity entity : ecs.view<GPUParticles3D>()) {
            by_key.emplace(key_of(world, entity), entity);
        }
        std::erase_if(emitters, [&](const auto& entry) { return !by_key.contains(entry.first); });
        if (tick_seconds <= 0.0) {
            return;
        }
        const world::Tree3D tree(world);
        const std::vector<FoundAttractor> found = attractors(world, tree);
        std::vector<render::ParticleAttractor3D> paired;
        for (const entt::entity entity : ecs.view<GPUParticles3D>()) {
            auto& component = ecs.get<GPUParticles3D>(entity);
            const Affine3D transform = tree.global(entity);
            // The attractors whose bounds touch its visibility box and whose cull_mask shares a
            // bit with its layers, as Godot's scene cull pairs them; at most 32, and 7 fields.
            paired.clear();
            const Box3 box = placed(transform, component.visibility_aabb_position,
                                    component.visibility_aabb_size);
            const Affine3D to_particles = transform.inverse();
            std::size_t fields_used = 0;
            for (const FoundAttractor& attractor : found) {
                if (paired.size() >= render::kMaxParticleAttractors3D) {
                    break;
                }
                if ((attractor.cull_mask & component.layers) == 0 ||
                    !intersects(attractor.bounds, box)) {
                    continue;
                }
                if (attractor.shape == render::AttractorShape3D::VectorField) {
                    if (fields_used >= kMaxParticleFields3D) {
                        continue;
                    }
                    ++fields_used;
                }
                paired.push_back(
                    for_particles(attractor, component.local_coords ? &to_particles : nullptr));
            }
            if (tick(emitters[key_of(world, entity)], component, transform, world.id_of(entity),
                     tick_seconds, paired)) {
                world.emit("finished", world.id_of(entity), {});
            }
        }
    }

    // The steps kept to replay; the particles themselves are on the device.
    [[nodiscard]] world::MemoryPart memory() const {
        world::MemoryPart part{"gpu_particles_3d", 0, {}, {}};
        std::uint64_t amount = 0;
        std::uint64_t steps = 0;
        for (const auto& [key, emitter] : emitters) {
            part.bytes += world::kMapNodeBytes<decltype(emitters)> + json::heap_bytes(key) +
                          (emitter.steps.capacity() * sizeof(render::ParticleStep3D));
            for (const render::ParticleStep3D& step : emitter.steps) {
                part.bytes += step.attractors.capacity() * sizeof(render::ParticleAttractor3D);
            }
            amount += emitter.amount;
            steps += emitter.kept().size();
        }
        for (const auto& [key, field] : fields) {
            part.bytes += field.field->texels.capacity() * sizeof(Vec4);
        }
        part.counts = {{"emitters", emitters.size()}, {"particles", amount}, {"steps", steps}};
        return part;
    }
};

} // namespace

render::ParticleTransform particle_transform(const Affine3D& affine) {
    return {affine.x.x, affine.x.y, affine.x.z, affine.y.x,      affine.y.y,      affine.y.z,
            affine.z.x, affine.z.y, affine.z.z, affine.origin.x, affine.origin.y, affine.origin.z};
}

Result<render::ParticleField3D> particle_field(const render::Image& image, int slices_horizontal,
                                               int slices_vertical) {
    if (slices_horizontal < 1 || slices_vertical < 1) {
        return make_error(ErrorCode::InvalidArgument, "slices must be at least 1");
    }
    const auto across = static_cast<std::uint32_t>(slices_horizontal);
    const auto down = static_cast<std::uint32_t>(slices_vertical);
    // ResourceImporterLayeredTexture: each slice is the image's size over the slices, rounded
    // down, cut left to right, then top to bottom.
    render::ParticleField3D field;
    field.width = image.width / across;
    field.height = image.height / down;
    field.depth = across * down;
    if (field.width == 0 || field.height == 0) {
        return make_error(ErrorCode::InvalidArgument,
                          "a " + std::to_string(image.width) + "x" + std::to_string(image.height) +
                              " image can't be cut into " + std::to_string(across) + "x" +
                              std::to_string(down) + " slices");
    }
    const std::size_t count = std::size_t{field.width} * field.height * field.depth;
    if (count > render::kMaxParticleFieldTexels3D) {
        return make_error(ErrorCode::InvalidArgument,
                          "the field's " + std::to_string(count) + " texels are more than " +
                              std::to_string(render::kMaxParticleFieldTexels3D));
    }
    if (image.pixels.size() < std::size_t{image.width} * image.height * 4) {
        return make_error(ErrorCode::InvalidArgument, "the image has no pixels");
    }
    field.texels.reserve(count);
    for (std::uint32_t row = 0; row < down; ++row) {
        for (std::uint32_t column = 0; column < across; ++column) {
            for (std::uint32_t y = 0; y < field.height; ++y) {
                for (std::uint32_t x = 0; x < field.width; ++x) {
                    const std::size_t across_at = (std::size_t{column} * field.width) + x;
                    const std::size_t down_at = (std::size_t{row} * field.height) + y;
                    const std::size_t at = ((down_at * image.width) + across_at) * 4;
                    const auto texel = [&](std::size_t i) {
                        return static_cast<float>(image.pixels[at + i]) / 255.0F;
                    };
                    field.texels.push_back({texel(0), texel(1), texel(2), texel(3)});
                }
            }
        }
    }
    return field;
}

std::uint32_t gpu_particle_seed(const GPUParticles3D& settings, std::string_view id,
                                std::uint32_t restarts) {
    if (settings.use_fixed_seed) {
        return static_cast<std::uint32_t>(settings.seed);
    }
    return particle_seed(id, settings.seed) + (restarts * 0x9e3779b9U);
}

render::ParticleOrder3D particle_order_3d(std::string_view draw_order) {
    if (draw_order == "lifetime") {
        return render::ParticleOrder3D::Lifetime;
    }
    if (draw_order == "reverse_lifetime") {
        return render::ParticleOrder3D::ReverseLifetime;
    }
    return draw_order == "view_depth" ? render::ParticleOrder3D::ViewDepth
                                      : render::ParticleOrder3D::Index;
}

render::ParticleAlign particle_align(std::string_view transform_align) {
    if (transform_align == "z_billboard") {
        return render::ParticleAlign::ZBillboard;
    }
    if (transform_align == "y_to_velocity") {
        return render::ParticleAlign::YToVelocity;
    }
    return transform_align == "z_billboard_y_to_velocity"
               ? render::ParticleAlign::ZBillboardYToVelocity
               : render::ParticleAlign::Disabled;
}

void add_gpu_particles_system(world::World& world, ImageLoader images) {
    if (auto* found = world.ecs().ctx().find<std::shared_ptr<GpuParticleSystem3D>>();
        found != nullptr) {
        (*found)->images = std::move(images);
        (*found)->fields.clear();
        return;
    }
    auto system = std::make_shared<GpuParticleSystem3D>();
    system->images = std::move(images);
    world.ecs().ctx().emplace<std::shared_ptr<GpuParticleSystem3D>>(system);
    world.add_reset([system](world::World& /*world*/) { system->emitters.clear(); });
    world.add_memory_source("gpu_particles_3d",
                            [system](const world::World& /*world*/) { return system->memory(); });
    world.add_system(
        "gpu_particles_3d",
        [system](world::World& w, double tick_seconds) { system->run_all(w, tick_seconds); },
        {.doc = "Steps each GPUParticles3D's emission for renderers to run.",
         .priority = 1110,
         .threaded = true,
         .reads = {"Transform3D", "GPUParticlesAttractorSphere3D", "GPUParticlesAttractorBox3D",
                   "GPUParticlesAttractorVectorField3D"},
         .writes = {"GPUParticles3D", "assets"}});
}

std::optional<ParticleSteps3D> particle_steps_3d(const world::World& world, entt::entity entity) {
    const auto* system = world.ecs().ctx().find<std::shared_ptr<GpuParticleSystem3D>>();
    if (system == nullptr || !world.ecs().all_of<GPUParticles3D>(entity)) {
        return std::nullopt;
    }
    const auto found = (*system)->emitters.find(key_of(world, entity));
    if (found == (*system)->emitters.end()) {
        return std::nullopt;
    }
    const Emitter& e = found->second;
    return ParticleSteps3D{.serial = e.serial,
                           .amount = e.amount,
                           .first = e.first,
                           .steps = e.kept(),
                           .remainder = e.remainder};
}

std::vector<render::Particle3D> run_steps_3d(std::uint32_t amount,
                                             const render::ParticleProcess3D& process,
                                             std::span<const render::ParticleStep3D> steps) {
    std::vector<render::Particle3D> particles(amount);
    if (steps.empty()) {
        return particles;
    }
    // The first step kept starts the system over, as a renderer's new system does.
    std::vector<render::ParticleStep3D> run(steps.begin(), steps.end());
    run.front().clear = true;
    render::process_particles_3d(particles, process, run);
    return particles;
}

std::vector<render::Particle3D> live_particles_3d(std::span<const render::Particle3D> particles,
                                                  render::ParticleOrder3D order, float phase) {
    const auto total = static_cast<std::uint32_t>(particles.size());
    std::vector<render::Particle3D> out;
    if (total == 0) {
        return out;
    }
    const std::uint32_t split = render::particle_lifetime_split(total, phase);
    for (std::uint32_t slot = 0; slot < total; ++slot) {
        const std::uint32_t index = render::particle_draw_index_3d(slot, total, order, split);
        if (particles[index].active()) {
            out.push_back(particles[index]);
        }
    }
    return out;
}

std::vector<render::Particle3D> gpu_particles_3d(const world::World& world, entt::entity entity,
                                                 const render::ParticleProcess3D& process) {
    const std::optional<ParticleSteps3D> kept = particle_steps_3d(world, entity);
    if (!kept || kept->steps.empty()) {
        return {};
    }
    const auto& settings = world.ecs().get<const GPUParticles3D>(entity);
    return live_particles_3d(run_steps_3d(kept->amount, process, kept->steps),
                             particle_order_3d(settings.draw_order),
                             kept->steps.back().system_phase);
}

std::vector<render::Particle3D> simulate_gpu(const GPUParticles3D& settings,
                                             const Affine3D& transform,
                                             const render::ParticleProcess3D& process,
                                             std::string_view id, double seconds,
                                             double tick_seconds) {
    GPUParticles3D copy = settings;
    Emitter e;
    const double step = tick_seconds > 0.0 ? tick_seconds : 1.0 / 60.0;
    const auto ticks = static_cast<std::int64_t>(std::floor((seconds / step) + 1e-9));
    std::vector<render::Particle3D> particles(amount_of(settings));
    for (std::int64_t i = 0; i < ticks; ++i) {
        const std::uint64_t before = e.first + e.kept().size();
        tick(e, copy, transform, id, step);
        // Runs the steps this tick made as they come, so none drop before they run.
        const std::uint64_t after = e.first + e.kept().size();
        if (e.amount != particles.size()) {
            particles.assign(e.amount, render::Particle3D{});
        }
        if (after > before) {
            const std::uint64_t from = std::max(before, e.first);
            render::process_particles_3d(
                particles, process, e.kept().subspan(static_cast<std::size_t>(from - e.first)));
        }
    }
    if (e.kept().empty()) {
        return {};
    }
    return live_particles_3d(particles, particle_order_3d(settings.draw_order),
                             e.kept().back().system_phase);
}

} // namespace slash::scene3d
