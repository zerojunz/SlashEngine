#include "slash/canvas/gpu_particles.hpp"

#include "slash/canvas/particles.hpp"
#include "slash/canvas/tilemap.hpp"
#include "slash/ui/ui.hpp"
#include "slash/world/canvas_layer.hpp"
#include "slash/world/scene.hpp"
#include "slash/world/tree2d.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <memory>
#include <numeric>
#include <set>
#include <string>
#include <utility>

namespace slash::canvas {
namespace {

using world::GPUParticles2D;

// A new number for each emitter made, in any world.
std::uint64_t next_serial() {
    static std::atomic<std::uint64_t> serial{0};
    return ++serial;
}

std::array<float, 6> matrix_of(const world::Transform2D& transform) {
    const world::Affine2D m = world::affine(transform);
    return {m.x.x, m.x.y, m.y.x, m.y.y, m.origin.x, m.origin.y};
}

// The shapes an emitter's particles collide with in a step, and how they are measured there.
struct Collision {
    std::vector<render::ParticleShape> shapes;
    std::array<float, 6> to_shapes{1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F};
    std::array<float, 4> from_shapes{1.0F, 0.0F, 0.0F, 1.0F};
    float size = 1.0F;
};

// One GPUParticles2D's state: Godot's GPUParticles2D node and its particles storage, and the
// steps kept.
struct Emitter {
    std::uint64_t serial = next_serial();
    // The node, and what the component said when last seen.
    bool seen = false;
    bool emitting = false;
    bool one_shot = false;
    std::uint32_t amount = 1;
    std::uint32_t restarts = 0; // one_shot restarts, which vary the seed
    Vec2 previous_position;
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
    std::vector<render::ParticleStep> steps;
    std::size_t start = 0;
    std::uint64_t first = 0;
    // The particle lifetimes the steps kept after steps[start] cover.
    double covered = 0.0;
    // The shapes the steps kept collide with, and the last set added: where it starts and its
    // size. Steps name sets in the order they were added, so the first kept that names one
    // names the first still needed.
    std::vector<render::ParticleShape> shapes;
    std::size_t last_shapes = 0;
    std::size_t last_count = 0;
    // The chain of sub-emitters it steps in, by its emitters' keys and amounts ("" on its own),
    // the root's key, its place there and, for the root, the chain's entities when last stepped.
    std::string chain;
    std::string root;
    std::uint32_t link = 0;
    std::uint32_t links = 1;
    std::vector<std::string> member_keys;
    std::vector<entt::entity> members;

    [[nodiscard]] std::span<const render::ParticleStep> kept() const {
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

    // Where a set of shapes is in shapes: the last set added, again, while they are the same.
    std::uint32_t add_shapes(const std::vector<render::ParticleShape>& set) {
        if (last_count != set.size() || last_shapes + last_count > shapes.size() ||
            !std::equal(set.begin(), set.end(),
                        shapes.begin() + static_cast<std::ptrdiff_t>(last_shapes))) {
            last_shapes = shapes.size();
            last_count = set.size();
            shapes.insert(shapes.end(), set.begin(), set.end());
        }
        return static_cast<std::uint32_t>(last_shapes);
    }

    // Drops the shapes no step kept names.
    void drop_unused_shapes() {
        std::size_t used = shapes.size();
        for (const render::ParticleStep& step : kept()) {
            if (step.shape_count > 0) {
                used = step.shape_first;
                break;
            }
        }
        if (used == shapes.size()) {
            shapes.clear();
            last_shapes = 0;
            last_count = 0;
        } else if (used >= 16 && used * 2 >= shapes.size()) {
            shapes.erase(shapes.begin(), shapes.begin() + static_cast<std::ptrdiff_t>(used));
            for (std::size_t i = start; i < steps.size(); ++i) {
                if (steps[i].shape_count > 0) {
                    steps[i].shape_first -= static_cast<std::uint32_t>(used);
                }
            }
            last_shapes -= used;
        }
    }

    // Keeps a step and as many before it as can still have made a live particle: a particle
    // lives at most a lifetime, so a step more than a lifetime of steps before the last made
    // none that live (with a hundredth to spare for rounding).
    void keep(const render::ParticleStep& step) {
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
            const render::ParticleStep& next = steps[start + 1];
            if (covered - (next.delta / next.lifetime) < 1.01 &&
                steps.size() - start <= kMaxParticleSteps) {
                break;
            }
            drop_front();
        }
        compact();
    }

    // Lets go of the room the steps dropped took.
    void compact() {
        if (start > 64 && start * 2 > steps.size()) {
            steps.erase(steps.begin(), steps.begin() + static_cast<std::ptrdiff_t>(start));
            start = 0;
        }
        drop_unused_shapes();
    }
};

// Keeps a step of each emitter in a chain of sub-emitters, the root's first, and as many before
// them as can still matter. A sub-emitter's live particles started within its lifetime, and
// it takes particles in turn through its slots from where its emission cycle started, so its
// steps go back two of its lifetimes (with a hundredth to spare), to the start of the cycle
// before; the system before it must be right from there, and so on to the root, which goes
// back one lifetime as an emitter on its own does. The emitters keep as many steps each.
void keep_chain(std::span<Emitter* const> links, std::span<const render::ParticleStep> steps) {
    const Emitter& root = *links.front();
    if (steps.front().delta <= 0.0F && !steps.front().clear) {
        return;
    }
    for (std::size_t k = 0; k < links.size(); ++k) {
        if (steps[k].clear) {
            links[k]->drop_all();
        }
        links[k]->steps.push_back(steps[k]);
    }
    // Walk back from the newest step, through each link's lifetimes from the last link's.
    std::size_t from = root.steps.size();
    std::size_t link = links.size() - 1;
    double need = 2.02;
    double covered = 0.0;
    bool met = false;
    while (from > root.start) {
        --from;
        const render::ParticleStep& step = links[link]->steps[from];
        covered += step.delta / step.lifetime;
        if (covered >= need) {
            if (link == 0) {
                met = true;
                break;
            }
            --link;
            covered = 0.0;
            need = link == 0 ? 1.01 : 2.02;
        }
    }
    // One step more, as an emitter on its own keeps, since the first step kept clears.
    std::size_t keep_from = met && from > root.start ? from - 1 : root.start;
    keep_from =
        std::max(keep_from, root.steps.size() - std::min(root.steps.size(), kMaxParticleSteps));
    for (Emitter* emitter : links) {
        while (emitter->start < keep_from) {
            emitter->drop_front();
        }
        emitter->compact();
    }
}

std::uint32_t amount_of(const GPUParticles2D& settings) {
    return static_cast<std::uint32_t>(
        std::clamp(settings.amount, 1, static_cast<int>(render::kMaxParticles)));
}

double lifetime_of(const GPUParticles2D& settings) {
    return std::max(static_cast<double>(settings.lifetime), 0.001);
}

// GPUParticles2D::set_emitting.
void set_emitting(Emitter& e, const GPUParticles2D& settings, bool emitting, bool first) {
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
            // Godot's docs say this starts a new cycle once the last one's particles are gone,
            // but its storage carries on with its phase until it has been idle 1.2 lifetimes,
            // so a burst set off from "finished" may emit nothing there. The storage starts
            // over here, as GPUParticles2D::restart starts it, so it always does.
            e.restart = !first;
        } else {
            e.canceled = true;
        }
    }
    e.emitting = emitting;
    e.storage_emitting = emitting;
}

// A sub-emitter in a chain, stepped with its root (Stepper): what it emits only its parent's
// particles start, so its own emitting, one_shot, fixed_fps and preprocess go unused.
struct SubLink {
    Emitter* emitter = nullptr;
    const GPUParticles2D* settings = nullptr;
    std::array<float, 6> emission{1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F};
    Vec2 velocity;
    std::uint32_t seed = 0;
    std::optional<Collision> collision;
};

// How long an emitter stays active once it stops emitting, in its own seconds: until its last
// particles have had time to die, and those its sub-emitters started from them.
double idle_limit(const GPUParticles2D& settings, std::span<const SubLink> subs) {
    double limit = lifetime_of(settings) * 1.2;
    for (const SubLink& sub : subs) {
        double lifetime = lifetime_of(*sub.settings);
        if (sub.settings->speed_scale > 0.0F) {
            lifetime *= static_cast<double>(std::max(settings.speed_scale, 0.0F)) /
                        static_cast<double>(sub.settings->speed_scale);
        }
        limit += lifetime * 1.2;
    }
    return limit;
}

class Stepper {
public:
    Stepper(const GPUParticles2D& settings, const world::Transform2D& transform, Vec2 velocity,
            std::uint32_t seed, const Collision* collision, std::span<SubLink> subs = {})
        : s_(settings), transform_(transform), velocity_(velocity), seed_(seed),
          collision_(collision), subs_(subs), idle_(idle_limit(settings, subs)) {}

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
        render::ParticleStep step;
        if (!s_.local_coords) {
            step.emission = matrix_of(transform_);
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
        if (collision_ != nullptr && !collision_->shapes.empty()) {
            step.shape_first = e.add_shapes(collision_->shapes);
            step.shape_count = static_cast<std::uint32_t>(collision_->shapes.size());
            step.collision_size = collision_->size;
            step.to_shapes = collision_->to_shapes;
            step.from_shapes = collision_->from_shapes;
        }
        step.clear = e.clear;
        e.clear = false;
        if (subs_.empty()) {
            e.keep(step);
            return;
        }
        std::array<render::ParticleStep, render::kMaxParticleChain> steps;
        std::array<Emitter*, render::kMaxParticleChain> links{};
        steps[0] = step;
        links[0] = &e;
        for (std::size_t i = 0; i < subs_.size(); ++i) {
            steps[i + 1] = sub_step(subs_[i], delta, step.clear);
            links[i + 1] = subs_[i].emitter;
        }
        keep_chain({links.data(), subs_.size() + 1}, {steps.data(), subs_.size() + 1});
    }

    // A sub-emitter's step for its root's step of delta: as many of its own seconds as its
    // speed_scale makes them, emitting nothing of its own.
    [[nodiscard]] render::ParticleStep sub_step(SubLink& sub, double delta, bool clear) const {
        const GPUParticles2D& settings = *sub.settings;
        Emitter& e = *sub.emitter;
        double own = 0.0;
        if (s_.speed_scale > 0.0F) {
            own = delta * static_cast<double>(std::max(settings.speed_scale, 0.0F)) /
                  static_cast<double>(s_.speed_scale);
        }
        if (clear) {
            e.phase = 0.0;
            e.cycle = 0;
            e.particle_time = 0.0;
        }
        const double new_phase = std::fmod(e.phase + (own / lifetime_of(settings)), 1.0);
        if (!clear && new_phase < e.phase) {
            ++e.cycle;
        }
        render::ParticleStep step;
        if (!settings.local_coords) {
            step.emission = sub.emission;
        }
        step.emitter_velocity = sub.velocity;
        step.emitting = false;
        step.system_phase = static_cast<float>(new_phase);
        step.prev_system_phase = static_cast<float>(e.phase);
        e.phase = new_phase;
        step.delta = static_cast<float>(own);
        step.lifetime = static_cast<float>(lifetime_of(settings));
        step.explosiveness = std::clamp(settings.explosiveness, 0.0F, 1.0F);
        step.randomness = std::clamp(settings.randomness, 0.0F, 1.0F);
        step.amount_ratio = std::clamp(settings.amount_ratio, 0.0F, 1.0F);
        step.interp_to_end = std::clamp(settings.interp_to_end, 0.0F, 1.0F);
        e.particle_time = std::fmod(e.particle_time + own, 3600.0);
        step.time = static_cast<float>(e.particle_time);
        step.cycle = e.cycle;
        step.seed = sub.seed;
        step.fractional_delta = settings.fract_delta;
        if (sub.collision && !sub.collision->shapes.empty()) {
            step.shape_first = e.add_shapes(sub.collision->shapes);
            step.shape_count = static_cast<std::uint32_t>(sub.collision->shapes.size());
            step.collision_size = sub.collision->size;
            step.to_shapes = sub.collision->to_shapes;
            step.from_shapes = sub.collision->from_shapes;
        }
        step.clear = clear;
        return step;
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
            e.inactive_time += s_.speed_scale * static_cast<float>(seconds);
            if (e.inactive_time > idle_) {
                e.inactive = true;
                return;
            }
        }
        const int fixed_fps = std::clamp(s_.fixed_fps, 0, 1000);
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
        for (const SubLink& sub : subs_) {
            sub.emitter->remainder =
                time_scale > 0.0F
                    ? e.remainder * std::max(sub.settings->speed_scale, 0.0F) / time_scale
                    : 0.0F;
        }
    }

private:
    const GPUParticles2D& s_;
    const world::Transform2D& transform_;
    Vec2 velocity_;
    std::uint32_t seed_;
    const Collision* collision_;
    std::span<SubLink> subs_;
    double idle_;
};

// One tick of an emitter: the node's part, which may set the component's emitting, then the
// storage's. Returns whether a one_shot finished.
bool tick(Emitter& e, GPUParticles2D& settings, const world::Transform2D& transform,
          std::string_view id, double seconds, const Collision* collision = nullptr,
          std::span<SubLink> subs = {}) {
    const std::uint32_t amount = amount_of(settings);
    if (!e.seen) {
        // As a node made with these properties: emitting set last.
        e.seen = true;
        e.amount = amount;
        e.one_shot = settings.one_shot;
        e.previous_position = transform.position;
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
            // GPUParticles2D::set_one_shot.
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
    // NOTIFICATION_INTERNAL_PHYSICS_PROCESS.
    Vec2 velocity;
    if (seconds > 0.0) {
        const auto per_second = static_cast<float>(1.0 / seconds);
        velocity = (transform.position - e.previous_position) * per_second;
    }
    e.previous_position = transform.position;
    Stepper(settings, transform, velocity, gpu_particle_seed(settings, id, e.restarts), collision,
            subs)
        .update(e, seconds);
    return finished;
}

// What an entity's emitter is kept under: its ID, which outlives the entity handles that
// restoring a copy of the world (a failed command, a dry run, undo) hands out anew.
std::string key_of(const world::World& world, entt::entity entity) {
    const std::string_view id = world.id_of(entity);
    return id.empty() ? "#" + std::to_string(entt::to_integral(entity)) : std::string(id);
}

// What GPU particles collide with, as Godot's viewport draws its signed distance field: the
// outlines of LightOccluder2Ds with sdf_collision and of tile occluders in occlusion layers with
// it, in visible canvases, each in the world canvas's coordinates with its bounds. Gathered the
// first time an emitter that collides asks, once a tick.
class Colliders {
public:
    Colliders(const world::World& world, const world::Tree2D& tree, asset::Assets* assets)
        : world_(world), tree_(tree), assets_(assets) {}

    // What an emitter collides with this tick, if its process material collides.
    [[nodiscard]] std::optional<Collision> of(entt::entity entity, const GPUParticles2D& settings,
                                              const world::Transform2D& global) {
        if (assets_ == nullptr || settings.process_material.empty()) {
            return std::nullopt;
        }
        const auto process = assets_->load<render::ParticleProcess>(settings.process_material);
        if (!process || process.value()->collision_mode == render::ParticleCollision::Disabled) {
            return std::nullopt;
        }
        gather();
        if (shapes_.empty()) {
            return std::nullopt;
        }
        // Particles go to the shapes through their canvas, and through the emitter when they
        // move with it.
        const std::size_t canvas = canvases_->of(entity);
        world::Affine2D to = to_world_[canvas];
        if (settings.local_coords) {
            to = to * world::affine(global);
        }
        const float determinant = (to.x.x * to.y.y) - (to.y.x * to.x.y);
        if (determinant == 0.0F || !std::isfinite(determinant)) {
            return std::nullopt;
        }
        const world::Affine2D back = to.inverse();
        Collision out;
        out.to_shapes = {to.x.x, to.x.y, to.y.x, to.y.y, to.origin.x, to.origin.y};
        out.from_shapes = {back.x.x, back.x.y, back.y.x, back.y.y};
        // GPUParticles2D::_update_collision_size: half the texture's average side, a radius.
        out.size = std::max(settings.collision_base_size, 0.0F);
        if (!settings.texture.empty()) {
            if (const auto image = assets_->load<render::Image>(settings.texture); image) {
                out.size *= static_cast<float>(image.value()->width + image.value()->height) / 4.0F;
            }
        }
        // All of them, or the nearest the emitter that fit, in the order they were found.
        std::vector<std::size_t> picked(shapes_.size());
        std::iota(picked.begin(), picked.end(), std::size_t{0});
        if (points_ > render::kMaxParticleShapePoints) {
            const Vec2 at = to_world_[canvas].apply(global.position);
            const auto distance = [&](std::size_t i) {
                const Vec2 nearest{std::clamp(at.x, shapes_[i].low.x, shapes_[i].high.x),
                                   std::clamp(at.y, shapes_[i].low.y, shapes_[i].high.y)};
                const Vec2 apart = at - nearest;
                return (apart.x * apart.x) + (apart.y * apart.y);
            };
            std::ranges::stable_sort(picked, {}, distance);
            std::size_t points = 0;
            std::size_t fit = 0;
            while (fit < picked.size() && points + shapes_[picked[fit]].shape.points.size() <=
                                              render::kMaxParticleShapePoints) {
                points += shapes_[picked[fit]].shape.points.size();
                ++fit;
            }
            picked.resize(fit);
            std::ranges::sort(picked);
        }
        out.shapes.reserve(picked.size());
        for (const std::size_t i : picked) {
            out.shapes.push_back(shapes_[i].shape);
        }
        if (out.shapes.empty()) {
            return std::nullopt;
        }
        return out;
    }

private:
    struct Shape {
        render::ParticleShape shape;
        Vec2 low;
        Vec2 high;
    };

    void add(std::vector<Vec2> points, bool closed) {
        if (points.empty()) {
            return;
        }
        Shape shape{
            .shape = {.points = std::move(points), .closed = closed}, .low = {}, .high = {}};
        shape.low = shape.high = shape.shape.points.front();
        for (const Vec2& p : shape.shape.points) {
            shape.low = {std::min(shape.low.x, p.x), std::min(shape.low.y, p.y)};
            shape.high = {std::max(shape.high.x, p.x), std::max(shape.high.y, p.y)};
        }
        points_ += shape.shape.points.size();
        shapes_.push_back(std::move(shape));
    }

    void gather() {
        if (canvases_) {
            return;
        }
        const auto& ecs = world_.ecs();
        canvases_.emplace(world::canvases(world_, tree_, ui::settings(world_).viewport));
        const auto& all = canvases_->all();
        const world::Affine2D undo_world = all[canvases_->world_canvas()].transform.inverse();
        to_world_.resize(all.size());
        for (std::size_t c = 0; c < all.size(); ++c) {
            if (c != canvases_->world_canvas()) {
                to_world_[c] = undo_world * all[c].transform;
            }
        }
        // In the order their entities were made, so the shapes come out the same every run.
        const auto in_order = [](auto view) {
            std::vector<entt::entity> entities(view.begin(), view.end());
            std::ranges::sort(entities, {}, [](entt::entity e) { return entt::to_entity(e); });
            return entities;
        };
        for (const entt::entity entity : in_order(ecs.view<const world::LightOccluder2D>())) {
            const auto& occluder = ecs.get<world::LightOccluder2D>(entity);
            const std::size_t canvas = canvases_->of(entity);
            if (!occluder.sdf_collision || !all[canvas].visible) {
                continue;
            }
            const world::Affine2D placed = to_world_[canvas] * tree_.global_affine(entity);
            std::vector<Vec2> points;
            points.reserve(occluder.polygon.size());
            for (const Vec2& p : occluder.polygon) {
                points.push_back(placed.apply(p));
            }
            add(std::move(points), occluder.closed);
        }
        for (const entt::entity entity : in_order(ecs.view<const world::TileMapLayer>())) {
            const auto& layer = ecs.get<world::TileMapLayer>(entity);
            const std::size_t canvas = canvases_->of(entity);
            if (!all[canvas].visible || !layer.visible || !layer.occlusion_enabled ||
                layer.tile_set.empty()) {
                continue;
            }
            const auto set = assets_->load<TileSet>(layer.tile_set);
            if (!set || std::ranges::none_of(set.value()->occlusion_layers,
                                             [](const OcclusionLayer& occlusion) {
                                                 return occlusion.sdf_collision;
                                             })) {
                continue;
            }
            const auto cells = decode_cells(layer, *set.value());
            if (!cells) {
                continue;
            }
            const world::Affine2D placed = to_world_[canvas] * tree_.global_affine(entity);
            for (const auto& [cell, tile] : cells.value()) {
                const Vec2 center = map_to_local(*set.value(), cell);
                for (const TileOccluder& polygon : tile_occluders(*set.value(), tile)) {
                    const auto index = static_cast<std::size_t>(polygon.layer);
                    if (index >= set.value()->occlusion_layers.size() ||
                        !set.value()->occlusion_layers[index].sdf_collision) {
                        continue;
                    }
                    std::vector<Vec2> points;
                    points.reserve(polygon.points.size());
                    for (const Vec2& p : polygon.points) {
                        points.push_back(placed.apply(p + center));
                    }
                    add(std::move(points), polygon.closed);
                }
            }
        }
    }

    const world::World& world_;
    const world::Tree2D& tree_;
    asset::Assets* assets_;
    std::optional<world::Canvases> canvases_;
    std::vector<world::Affine2D> to_world_; // each canvas's coordinates to the world canvas's
    std::vector<Shape> shapes_;
    std::size_t points_ = 0;
};

// The chains of sub-emitters in a world, the root first: each GPUParticles2D's sub_emitter, in
// the order of their keys, where it names another GPUParticles2D that no other has claimed, that
// isn't one before it in its chain and that keeps the chain to render::kMaxParticleChain. Those
// left out go in left_out, with why.
std::vector<std::vector<entt::entity>>
particle_chains(const world::World& world,
                const std::map<std::string, entt::entity, std::less<>>& by_key,
                std::vector<SubEmitterProblem>* left_out = nullptr) {
    const auto& ecs = world.ecs();
    std::map<entt::entity, entt::entity> sub_of;
    std::map<entt::entity, entt::entity> parent_of;
    const auto leave_out = [&](const std::string& key, std::string message) {
        if (left_out != nullptr) {
            left_out->push_back({.id = key, .message = std::move(message)});
        }
    };
    for (const auto& [key, entity] : by_key) {
        const entt::entity sub = ecs.get<GPUParticles2D>(entity).sub_emitter;
        if (sub == entt::null) {
            continue;
        }
        if (sub == entity) {
            leave_out(key, "an emitter can't be its own sub-emitter");
            continue;
        }
        if (!ecs.valid(sub)) {
            leave_out(key, "its sub-emitter no longer exists");
            continue;
        }
        if (!ecs.all_of<GPUParticles2D>(sub)) {
            leave_out(key, "\"" + key_of(world, sub) + "\" has no GPUParticles2D");
            continue;
        }
        if (const auto claimed = parent_of.find(sub); claimed != parent_of.end()) {
            leave_out(key, "\"" + key_of(world, sub) + "\" is already the sub-emitter of \"" +
                               key_of(world, claimed->second) + "\"");
            continue;
        }
        // Not a loop, and not too long: the links above this one and from the sub down.
        std::size_t length = 1;
        bool loop = false;
        for (entt::entity at = sub;; ++length) {
            if (at == entity) {
                loop = true;
                break;
            }
            const auto next = sub_of.find(at);
            if (next == sub_of.end()) {
                break;
            }
            at = next->second;
        }
        for (auto up = parent_of.find(entity); up != parent_of.end();
             up = parent_of.find(up->second)) {
            ++length;
        }
        if (loop) {
            leave_out(key, "\"" + key_of(world, sub) +
                               "\" leads back to this emitter through its sub-emitters");
            continue;
        }
        if (length + 1 > render::kMaxParticleChain) {
            leave_out(key, "a chain of sub-emitters has at most " +
                               std::to_string(render::kMaxParticleChain) + " emitters");
            continue;
        }
        sub_of[entity] = sub;
        parent_of[sub] = entity;
    }
    std::vector<std::vector<entt::entity>> chains;
    for (const auto& [key, entity] : by_key) {
        if (!sub_of.contains(entity) || parent_of.contains(entity)) {
            continue;
        }
        std::vector<entt::entity>& chain = chains.emplace_back();
        for (entt::entity at = entity;;) {
            chain.push_back(at);
            const auto next = sub_of.find(at);
            if (next == sub_of.end()) {
                break;
            }
            at = next->second;
        }
    }
    return chains;
}

// Every GPUParticles2D in a world by its key.
std::map<std::string, entt::entity, std::less<>> emitters_by_key(const world::World& world) {
    std::map<std::string, entt::entity, std::less<>> by_key;
    for (const entt::entity entity : world.ecs().view<GPUParticles2D>()) {
        by_key.emplace(key_of(world, entity), entity);
    }
    return by_key;
}

struct GpuParticleSystem {
    std::map<std::string, Emitter, std::less<>> emitters;
    asset::Assets* assets = nullptr;

    // The emitter kept under a key, made anew when the chain it steps in changes.
    Emitter& emitter(const std::string& key, const std::string& chain) {
        Emitter& e = emitters[key];
        if (e.chain != chain) {
            e = Emitter{};
            e.chain = chain;
        }
        return e;
    }

    void run_all(world::World& world, double tick_seconds) {
        auto& ecs = world.ecs();
        const std::map<std::string, entt::entity, std::less<>> by_key = emitters_by_key(world);
        std::erase_if(emitters, [&](const auto& entry) { return !by_key.contains(entry.first); });
        if (tick_seconds <= 0.0) {
            return;
        }
        const world::Tree2D tree(world);
        Colliders colliders(world, tree, assets);
        const std::vector<std::vector<entt::entity>> chains = particle_chains(world, by_key);
        std::set<entt::entity> in_chain;
        for (const auto& chain : chains) {
            in_chain.insert(chain.begin(), chain.end());
        }
        for (const entt::entity entity : ecs.view<GPUParticles2D>()) {
            if (in_chain.contains(entity)) {
                continue;
            }
            auto& component = ecs.get<GPUParticles2D>(entity);
            const world::Transform2D global = tree.global(entity);
            const std::optional<Collision> collision = colliders.of(entity, component, global);
            if (tick(emitter(key_of(world, entity), ""), component, global, world.id_of(entity),
                     tick_seconds, collision ? &*collision : nullptr)) {
                world.emit("finished", world.id_of(entity), {});
            }
        }
        for (const auto& chain : chains) {
            run_chain(world, tree, colliders, chain, tick_seconds);
        }
    }

    // A tick of a chain of sub-emitters: the root's, stepping the rest with it. The chain starts
    // over when its emitters or their amounts change.
    void run_chain(world::World& world, const world::Tree2D& tree, Colliders& colliders,
                   std::span<const entt::entity> chain, double tick_seconds) {
        auto& ecs = world.ecs();
        std::vector<std::string> keys;
        std::string signature;
        for (const entt::entity entity : chain) {
            keys.push_back(key_of(world, entity));
            signature += (signature.empty() ? "" : ">") + keys.back() + ":" +
                         std::to_string(amount_of(ecs.get<GPUParticles2D>(entity)));
        }
        std::vector<SubLink> subs(chain.size() - 1);
        for (std::size_t k = 0; k < chain.size(); ++k) {
            Emitter& e = emitter(keys[k], signature);
            e.root = keys.front();
            e.link = static_cast<std::uint32_t>(k);
            e.links = static_cast<std::uint32_t>(chain.size());
            if (k == 0) {
                e.member_keys = keys;
                e.members.assign(chain.begin(), chain.end());
                continue;
            }
            const auto& settings = ecs.get<GPUParticles2D>(chain[k]);
            const world::Transform2D global = tree.global(chain[k]);
            SubLink& sub = subs[k - 1];
            sub.emitter = &e;
            sub.settings = &settings;
            sub.emission = matrix_of(global);
            if (!e.seen) {
                e.seen = true;
                e.amount = amount_of(settings);
                e.previous_position = global.position;
            }
            sub.velocity =
                (global.position - e.previous_position) * static_cast<float>(1.0 / tick_seconds);
            e.previous_position = global.position;
            sub.seed = gpu_particle_seed(settings, world.id_of(chain[k]), 0);
            sub.collision = colliders.of(chain[k], settings, global);
        }
        auto& root = ecs.get<GPUParticles2D>(chain.front());
        const world::Transform2D global = tree.global(chain.front());
        const std::optional<Collision> collision = colliders.of(chain.front(), root, global);
        if (tick(emitter(keys.front(), signature), root, global, world.id_of(chain.front()),
                 tick_seconds, collision ? &*collision : nullptr, subs)) {
            world.emit("finished", world.id_of(chain.front()), {});
        }
    }

    // The steps kept to replay; the particles themselves are on the GPU.
    [[nodiscard]] world::MemoryPart memory() const {
        world::MemoryPart part{"gpu_particles_2d", 0, {}, {}};
        std::uint64_t amount = 0;
        std::uint64_t steps = 0;
        for (const auto& [key, emitter] : emitters) {
            part.bytes += world::kMapNodeBytes<decltype(emitters)> + json::heap_bytes(key) +
                          (emitter.steps.capacity() * sizeof(render::ParticleStep)) +
                          (emitter.shapes.capacity() * sizeof(render::ParticleShape));
            for (const render::ParticleShape& shape : emitter.shapes) {
                part.bytes += shape.points.capacity() * sizeof(Vec2);
            }
            amount += emitter.amount;
            steps += emitter.kept().size();
        }
        part.counts = {{"emitters", emitters.size()}, {"particles", amount}, {"steps", steps}};
        return part;
    }
};

} // namespace

std::uint32_t gpu_particle_seed(const GPUParticles2D& settings, std::string_view id,
                                std::uint32_t restarts) {
    if (settings.use_fixed_seed) {
        return static_cast<std::uint32_t>(settings.seed);
    }
    return particle_seed(id, settings.seed) + (restarts * 0x9e3779b9U);
}

render::ParticleOrder particle_order(std::string_view draw_order) {
    if (draw_order == "index") {
        return render::ParticleOrder::Index;
    }
    return draw_order == "reverse_lifetime" ? render::ParticleOrder::ReverseLifetime
                                            : render::ParticleOrder::Lifetime;
}

void add_gpu_particles_system(world::World& world, asset::Assets* assets) {
    if (world.ecs().ctx().contains<std::shared_ptr<GpuParticleSystem>>()) {
        return;
    }
    auto system = std::make_shared<GpuParticleSystem>();
    system->assets = assets;
    world.ecs().ctx().emplace<std::shared_ptr<GpuParticleSystem>>(system);
    world.add_reset([system](world::World& /*world*/) { system->emitters.clear(); });
    world.add_memory_source("gpu_particles_2d",
                            [system](const world::World& /*world*/) { return system->memory(); });
    world.add_system(
        "gpu_particles_2d",
        [system](world::World& w, double tick_seconds) { system->run_all(w, tick_seconds); },
        {.doc = "Steps each GPUParticles2D's emission for renderers to run.",
         .priority = 1110,
         .threaded = true,
         .reads = {"Transform2D", "LightOccluder2D", "TileMapLayer", "CanvasLayer", "Control",
                   "Camera2D"},
         .writes = {"GPUParticles2D", "assets"}});
}

std::optional<ParticleSteps> particle_steps(const world::World& world, entt::entity entity) {
    const auto* system = world.ecs().ctx().find<std::shared_ptr<GpuParticleSystem>>();
    if (system == nullptr || !world.ecs().all_of<GPUParticles2D>(entity)) {
        return std::nullopt;
    }
    const auto found = (*system)->emitters.find(key_of(world, entity));
    if (found == (*system)->emitters.end()) {
        return std::nullopt;
    }
    const Emitter& e = found->second;
    return ParticleSteps{.serial = e.serial,
                         .amount = e.amount,
                         .first = e.first,
                         .steps = e.kept(),
                         .shapes = e.shapes,
                         .remainder = e.remainder,
                         .link = e.link,
                         .links = e.links};
}

std::vector<ParticleChainMember> particle_chain(const world::World& world, entt::entity entity) {
    const auto* system = world.ecs().ctx().find<std::shared_ptr<GpuParticleSystem>>();
    const std::optional<ParticleSteps> kept = particle_steps(world, entity);
    if (system == nullptr || !kept || kept->links < 2) {
        return {};
    }
    const std::map<std::string, Emitter, std::less<>>& emitters = (*system)->emitters;
    const Emitter& member = emitters.find(key_of(world, entity))->second;
    const auto root = emitters.find(member.root);
    if (root == emitters.end() || root->second.chain != member.chain) {
        return {};
    }
    std::vector<ParticleChainMember> out;
    const auto& ecs = world.ecs();
    for (std::size_t k = 0; k < root->second.member_keys.size(); ++k) {
        const std::string& key = root->second.member_keys[k];
        // The entity stepped, or the one with its ID after the world was put back.
        entt::entity at = root->second.members[k];
        if (!ecs.valid(at) || key_of(world, at) != key) {
            at = world.find(key);
        }
        if (at == entt::null || !ecs.all_of<GPUParticles2D>(at)) {
            return {};
        }
        std::optional<ParticleSteps> steps = particle_steps(world, at);
        if (!steps || steps->link != k || steps->first != kept->first ||
            steps->steps.size() != kept->steps.size()) {
            return {};
        }
        out.push_back({.entity = at, .steps = *steps});
    }
    return out;
}

std::vector<render::Particle> run_steps(std::uint32_t amount,
                                        const render::ParticleProcess& process,
                                        std::span<const render::ParticleStep> steps,
                                        std::span<const render::ParticleShape> shapes) {
    std::vector<render::Particle> particles(amount);
    if (!steps.empty()) {
        // The first step kept starts from nothing, as it would after a gap.
        render::ParticleStep first = steps.front();
        first.clear = true;
        render::process_particles(particles, process, {&first, 1}, shapes);
        render::process_particles(particles, process, steps.subspan(1), shapes);
    }
    return particles;
}

std::vector<std::vector<render::Particle>> run_chain(std::span<const ChainRun> chain) {
    std::vector<std::vector<render::Particle>> particles;
    particles.reserve(chain.size());
    std::array<render::ParticleStep, render::kMaxParticleChain> firsts;
    std::array<render::ParticleChainLink, render::kMaxParticleChain> links;
    const std::size_t count = std::min(chain.size(), render::kMaxParticleChain);
    bool empty = false;
    for (std::size_t k = 0; k < count; ++k) {
        particles.emplace_back(chain[k].amount);
        empty = empty || chain[k].steps.empty();
    }
    if (empty) {
        return particles;
    }
    // The first step kept starts from nothing, as it would after a gap.
    for (std::size_t k = 0; k < count; ++k) {
        firsts[k] = chain[k].steps.front();
        firsts[k].clear = true;
        links[k] = {.particles = particles[k],
                    .process = chain[k].process,
                    .steps = {&firsts[k], 1},
                    .shapes = chain[k].shapes};
    }
    render::process_particle_chain({links.data(), count});
    for (std::size_t k = 0; k < count; ++k) {
        links[k].steps = chain[k].steps.subspan(1);
    }
    render::process_particle_chain({links.data(), count});
    return particles;
}

std::vector<SubEmitterProblem> check_sub_emitters(const world::World& world) {
    std::vector<SubEmitterProblem> problems;
    (void)particle_chains(world, emitters_by_key(world), &problems);
    return problems;
}

std::vector<json::ValidationError> check_sub_emitters(const world::ComponentRegistry& components,
                                                      const json::Value& scene,
                                                      const world::FileReader& reader) {
    std::vector<json::ValidationError> errors;
    world::World world(components);
    world.set_file_reader(reader);
    if (!world::load_scene(world, scene)) {
        return errors;
    }
    for (SubEmitterProblem& problem : check_sub_emitters(world)) {
        errors.push_back({"/entities/" + json::pointer_escape(problem.id) +
                              "/components/GPUParticles2D/sub_emitter",
                          std::move(problem.message)});
    }
    return errors;
}

std::vector<render::Particle> live_particles(std::span<const render::Particle> particles,
                                             render::ParticleOrder order, double phase) {
    std::vector<render::Particle> out;
    const auto amount = static_cast<std::uint32_t>(particles.size());
    const std::uint32_t split =
        order == render::ParticleOrder::Index ? 0 : render::particle_lifetime_split(amount, phase);
    for (std::uint32_t slot = 0; slot < amount; ++slot) {
        // As particles_draw.hlsl picks each slot's particle.
        std::uint32_t index = slot;
        if (order != render::ParticleOrder::Index) {
            const std::uint32_t limit = amount - std::min(split, amount);
            if (order == render::ParticleOrder::ReverseLifetime) {
                index = amount - index - 1;
            }
            index = index < limit ? split + index : index - limit;
        }
        if (particles[index].active()) {
            out.push_back(particles[index]);
        }
    }
    return out;
}

Result<std::vector<render::Particle>> gpu_particles(const world::World& world, entt::entity entity,
                                                    asset::Assets& assets) {
    const std::optional<ParticleSteps> kept = particle_steps(world, entity);
    if (!kept || kept->steps.empty()) {
        return std::vector<render::Particle>{};
    }
    const auto& settings = world.ecs().get<GPUParticles2D>(entity);
    const auto process_of = [&](const GPUParticles2D& emitter)
        -> Result<std::shared_ptr<const render::ParticleProcess>> {
        if (emitter.process_material.empty()) {
            return std::make_shared<const render::ParticleProcess>();
        }
        auto loaded = assets.load<render::ParticleProcess>(emitter.process_material);
        if (!loaded) {
            return loaded.error();
        }
        return loaded.value().share();
    };
    const std::vector<ParticleChainMember> chain = particle_chain(world, entity);
    if (!chain.empty()) {
        std::vector<std::shared_ptr<const render::ParticleProcess>> processes;
        std::vector<ChainRun> runs;
        for (const ParticleChainMember& member : chain) {
            auto process = process_of(world.ecs().get<GPUParticles2D>(member.entity));
            if (!process) {
                return process.error();
            }
            processes.push_back(std::move(process).value());
            runs.push_back({.amount = member.steps.amount,
                            .process = processes.back().get(),
                            .steps = member.steps.steps,
                            .shapes = member.steps.shapes});
        }
        const std::vector<std::vector<render::Particle>> all = run_chain(runs);
        return live_particles(all[kept->link], particle_order(settings.draw_order),
                              kept->steps.back().system_phase);
    }
    auto process = process_of(settings);
    if (!process) {
        return process.error();
    }
    const std::vector<render::Particle> all =
        run_steps(kept->amount, *process.value(), kept->steps, kept->shapes);
    return live_particles(all, particle_order(settings.draw_order),
                          kept->steps.back().system_phase);
}

std::vector<render::Particle> simulate(const GPUParticles2D& settings,
                                       const render::ParticleProcess& process,
                                       const world::Transform2D& transform, std::string_view id,
                                       double seconds, double tick_seconds) {
    if (tick_seconds <= 0.0) {
        return {};
    }
    GPUParticles2D copy = settings;
    Emitter emitter;
    const auto ticks =
        static_cast<std::int64_t>(std::ceil((std::max(seconds, 0.0) / tick_seconds) - 1e-9));
    for (std::int64_t i = 0; i < ticks; ++i) {
        (void)tick(emitter, copy, transform, id, tick_seconds);
    }
    if (emitter.kept().empty()) {
        return {};
    }
    const std::vector<render::Particle> all = run_steps(emitter.amount, process, emitter.kept());
    return live_particles(all, particle_order(copy.draw_order), emitter.kept().back().system_phase);
}

} // namespace slash::canvas
