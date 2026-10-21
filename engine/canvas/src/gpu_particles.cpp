#include "slash/canvas/gpu_particles.hpp"

#include "slash/canvas/particles.hpp"
#include "slash/canvas/tilemap.hpp"
#include "slash/ui/ui.hpp"
#include "slash/world/canvas_layer.hpp"
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
        if (start > 64 && start * 2 > steps.size()) {
            steps.erase(steps.begin(), steps.begin() + static_cast<std::ptrdiff_t>(start));
            start = 0;
        }
        drop_unused_shapes();
    }
};

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

class Stepper {
public:
    Stepper(const GPUParticles2D& settings, const world::Transform2D& transform, Vec2 velocity,
            std::uint32_t seed, const Collision* collision)
        : s_(settings), transform_(transform), velocity_(velocity), seed_(seed),
          collision_(collision) {}

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
            e.inactive_time += s_.speed_scale * static_cast<float>(seconds);
            if (e.inactive_time > lifetime_of(s_) * 1.2) {
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
    }

private:
    const GPUParticles2D& s_;
    const world::Transform2D& transform_;
    Vec2 velocity_;
    std::uint32_t seed_;
    const Collision* collision_;
};

// One tick of an emitter: the node's part, which may set the component's emitting, then the
// storage's. Returns whether a one_shot finished.
bool tick(Emitter& e, GPUParticles2D& settings, const world::Transform2D& transform,
          std::string_view id, double seconds, const Collision* collision = nullptr) {
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
    Stepper(settings, transform, velocity, gpu_particle_seed(settings, id, e.restarts), collision)
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

struct GpuParticleSystem {
    std::map<std::string, Emitter, std::less<>> emitters;
    asset::Assets* assets = nullptr;

    void run_all(world::World& world, double tick_seconds) {
        auto& ecs = world.ecs();
        std::set<std::string, std::less<>> seen;
        const world::Tree2D tree(world);
        Colliders colliders(world, tree, assets);
        for (const entt::entity entity : ecs.view<GPUParticles2D>()) {
            auto& component = ecs.get<GPUParticles2D>(entity);
            const std::string key = key_of(world, entity);
            seen.insert(key);
            if (tick_seconds <= 0.0) {
                continue;
            }
            const world::Transform2D global = tree.global(entity);
            const std::optional<Collision> collision = colliders.of(entity, component, global);
            if (tick(emitters[key], component, global, world.id_of(entity), tick_seconds,
                     collision ? &*collision : nullptr)) {
                world.emit("finished", world.id_of(entity), {});
            }
        }
        std::erase_if(emitters, [&](const auto& entry) { return !seen.contains(entry.first); });
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
                         .remainder = e.remainder};
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
    render::ParticleProcess process;
    if (!settings.process_material.empty()) {
        auto loaded = assets.load<render::ParticleProcess>(settings.process_material);
        if (!loaded) {
            return loaded.error();
        }
        process = *loaded.value();
    }
    const std::vector<render::Particle> all =
        run_steps(kept->amount, process, kept->steps, kept->shapes);
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
