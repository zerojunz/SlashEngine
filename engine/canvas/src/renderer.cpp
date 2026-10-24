#include "slash/canvas/canvas.hpp"
#include "slash/canvas/gpu_particles.hpp"
#include "slash/canvas/particles.hpp"
#include "slash/core/log.hpp"
#include "slash/core/profile.hpp"
#include "slash/ui/ui.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace slash::canvas {
namespace {

std::uint8_t to_byte(float channel) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(channel, 0.0F, 1.0F) * 255.0F));
}

std::string number(float value) {
    std::string text = std::to_string(value);
    text.erase(text.find_last_not_of('0') + 1);
    if (text.ends_with('.')) {
        text.pop_back();
    }
    return text;
}

render::Filter resolve_filter(std::string_view filter, render::Filter fallback) {
    if (filter == "nearest") {
        return render::Filter::Nearest;
    }
    if (filter == "linear") {
        return render::Filter::Linear;
    }
    return fallback;
}

// A canvas transform as the matrix a draw's material moves its positions by.
render::Mat4 matrix(const world::Affine2D& t) {
    return render::Mat4{t.x.x, t.x.y, 0.0F, 0.0F, t.y.x,      t.y.y,      0.0F, 0.0F,
                        0.0F,  0.0F,  1.0F, 0.0F, t.origin.x, t.origin.y, 0.0F, 1.0F};
}

// Where a Parallax2D's repeat draws its children, as offsets in their canvas's pixels for a
// Parallax2D whose global transform is global, in the order Godot's canvas_render_items draws
// them, the first place included; empty when it doesn't repeat.
std::vector<Vec2> repeat_offsets(const world::Parallax2D& parallax, const world::Affine2D& global) {
    const Vec2 size{std::max(parallax.repeat_size.x, 0.0F), std::max(parallax.repeat_size.y, 0.0F)};
    if (size.x == 0.0F && size.y == 0.0F) {
        return {};
    }
    const int times = std::max(parallax.repeat_times, 1);
    const int before = times / 2; // copies before the first place, rounded down
    const Vec2 start = size * static_cast<float>(-before);
    const int times_x = size.x != 0.0F ? times : 0;
    const int times_y = size.y != 0.0F ? times : 0;
    std::vector<Vec2> out;
    for (int ry = 0; ry <= times_y; ++ry) {
        const float oy = start.y + (static_cast<float>(ry) * size.y);
        for (int rx = 0; rx <= times_x; ++rx) {
            const float ox = start.x + (static_cast<float>(rx) * size.x);
            // final_transform.basis_xform(offset).
            out.push_back(
                {(global.x.x * ox) + (global.y.x * oy), (global.x.y * ox) + (global.y.y * oy)});
        }
    }
    return out;
}

// The live particles of a system with trails of trail particles (1 without), copies not counted.
std::size_t live_count(std::span<const render::Particle> particles, std::uint32_t trail) {
    return static_cast<std::size_t>(
        std::ranges::count_if(particles.first(particles.size() / std::max(trail, 1U)),
                              [](const render::Particle& p) { return p.active(); }));
}

// Adds a GPUParticles2D's live particles' quads, or its trails' segments, as the device draws
// them: one not live has no area.
void append_live_quads(std::span<const render::Particle> particles, const render::ParticleDraw& at,
                       std::uint32_t trail, std::vector<render::Vertex2D>& vertices) {
    std::vector<render::Vertex2D> quads;
    render::particle_vertices(particles, at, quads, trail);
    for (std::size_t q = 0; q + 6 <= quads.size(); q += 6) {
        const auto quad = std::span(quads).subspan(q, 6);
        if (std::ranges::any_of(quad, [](const render::Vertex2D& v) {
                return v.rgba != std::array<std::uint8_t, 4>{};
            })) {
            vertices.insert(vertices.end(), quad.begin(), quad.end());
        }
    }
}

// A sprite, or a run of a layer's tiles with one texture: vertices from first, in order.
struct Item {
    std::size_t canvas = 0; // in canvases.all()
    int z_index = 0;
    std::uint64_t order = 0; // Tree2D::draw_key
    std::uint32_t part = 0;
    render::TextureId texture = render::TextureId::None;
    std::size_t first = 0;
    std::size_t count = 0;
    render::Material material;
    // For FrameContents.
    entt::entity entity = entt::null;
    FrameItem::Kind kind = FrameItem::Kind::Sprite;
    const render::Image* image = nullptr; // white when null
    std::string_view path;                // the image's project path
    std::size_t drawn = 0;                // tiles or particles
    // A GPUParticles2D's particles, which the device draws instead of the vertices.
    render::ParticlesId particles = render::ParticlesId::None;
    render::ParticleDraw look;
};

} // namespace

Vec2 to_world(const world::World& world, Vec2 point, entt::entity in) {
    const world::Tree2D tree(world);
    const world::Canvases canvases = world::canvases(world, tree, ui::settings(world).viewport);
    return world::screen_transform(world, canvases, in).inverse().apply(point);
}

Vec2 to_screen(const world::World& world, Vec2 point, entt::entity in) {
    const world::Tree2D tree(world);
    const world::Canvases canvases = world::canvases(world, tree, ui::settings(world).viewport);
    return world::screen_transform(world, canvases, in).apply(point);
}

Result<std::array<render::Vertex2D, 6>> sprite_quad(const world::Sprite2D& sprite,
                                                    const world::Transform2D& transform,
                                                    std::uint32_t texture_width,
                                                    std::uint32_t texture_height) {
    if (texture_width == 0 || texture_height == 0) {
        return make_error(ErrorCode::InvalidArgument, "the texture is empty");
    }
    const auto width = static_cast<float>(texture_width);
    const auto height = static_cast<float>(texture_height);
    // The part of the texture to split into frames.
    float rx = 0.0F;
    float ry = 0.0F;
    float rw = width;
    float rh = height;
    if (sprite.region.z > 0.0F && sprite.region.w > 0.0F) {
        rx = sprite.region.x;
        ry = sprite.region.y;
        rw = sprite.region.z;
        rh = sprite.region.w;
        if (rx < 0.0F || ry < 0.0F || rx + rw > width || ry + rh > height) {
            return make_error(ErrorCode::InvalidArgument,
                              "region [" + number(rx) + ", " + number(ry) + ", " + number(rw) +
                                  ", " + number(rh) + "] goes outside the " +
                                  std::to_string(texture_width) + "x" +
                                  std::to_string(texture_height) + " texture");
        }
    }
    if (sprite.hframes < 1 || sprite.vframes < 1) {
        return make_error(ErrorCode::InvalidArgument, "hframes and vframes must be at least 1");
    }
    const int count = sprite.hframes * sprite.vframes;
    if (sprite.frame < 0 || sprite.frame >= count) {
        return make_error(ErrorCode::InvalidArgument,
                          "frame " + std::to_string(sprite.frame) + " is outside the sheet's " +
                              std::to_string(count) + " frames, numbered from 0");
    }
    const float cell_w = rw / static_cast<float>(sprite.hframes);
    const float cell_h = rh / static_cast<float>(sprite.vframes);
    const int column = sprite.frame % sprite.hframes;
    const int row = sprite.frame / sprite.hframes;
    const float cell_x = rx + (static_cast<float>(column) * cell_w);
    const float cell_y = ry + (static_cast<float>(row) * cell_h);

    float u0 = cell_x / width;
    float u1 = (cell_x + cell_w) / width;
    float v0 = cell_y / height;
    float v1 = (cell_y + cell_h) / height;
    // As in Godot, flipping mirrors the texture in place; the rectangle stays where it is.
    if (sprite.flip_h) {
        std::swap(u0, u1);
    }
    if (sprite.flip_v) {
        std::swap(v0, v1);
    }

    const float x0 = sprite.offset.x - (sprite.centered ? cell_w / 2.0F : 0.0F);
    const float y0 = sprite.offset.y - (sprite.centered ? cell_h / 2.0F : 0.0F);
    const float x1 = x0 + cell_w;
    const float y1 = y0 + cell_h;
    const float cos_r = std::cos(transform.rotation);
    const float sin_r = std::sin(transform.rotation);
    // The y axis's angle: skew leans it from square.
    const float cos_k =
        transform.skew == 0.0F ? cos_r : std::cos(transform.rotation + transform.skew);
    const float sin_k =
        transform.skew == 0.0F ? sin_r : std::sin(transform.rotation + transform.skew);
    const std::array<std::uint8_t, 4> rgba{to_byte(sprite.modulate.x), to_byte(sprite.modulate.y),
                                           to_byte(sprite.modulate.z), to_byte(sprite.modulate.w)};
    const auto corner = [&](float x, float y, float u, float v) {
        const float sx = x * transform.scale.x;
        const float sy = y * transform.scale.y;
        return render::Vertex2D{.x = transform.position.x + (sx * cos_r) - (sy * sin_k),
                                .y = transform.position.y + (sx * sin_r) + (sy * cos_k),
                                .u = u,
                                .v = v,
                                .rgba = rgba};
    };
    const render::Vertex2D top_left = corner(x0, y0, u0, v0);
    const render::Vertex2D top_right = corner(x1, y0, u1, v0);
    const render::Vertex2D bottom_left = corner(x0, y1, u0, v1);
    const render::Vertex2D bottom_right = corner(x1, y1, u1, v1);
    return std::array<render::Vertex2D, 6>{top_left,  top_right,    bottom_left,
                                           top_right, bottom_right, bottom_left};
}

world::Transform2D interpolate(const world::Transform2D& from, const world::Transform2D& to,
                               double alpha) {
    const auto t = static_cast<float>(std::clamp(alpha, 0.0, 1.0));
    const auto lerp = [t](float a, float b) { return a + ((b - a) * t); };
    constexpr float kTwoPi = 2.0F * std::numbers::pi_v<float>;
    // The turn from one rotation to the other, between -pi and pi.
    float turn = std::fmod(to.rotation - from.rotation, kTwoPi);
    if (turn > std::numbers::pi_v<float>) {
        turn -= kTwoPi;
    } else if (turn < -std::numbers::pi_v<float>) {
        turn += kTwoPi;
    }
    world::Transform2D out;
    out.position = {lerp(from.position.x, to.position.x), lerp(from.position.y, to.position.y)};
    out.rotation = t >= 1.0F ? to.rotation : from.rotation + (turn * t);
    out.scale = {lerp(from.scale.x, to.scale.x), lerp(from.scale.y, to.scale.y)};
    out.skew = lerp(from.skew, to.skew);
    return out;
}

Inherited inherited(const world::World& world, const world::Tree2D& tree, entt::entity entity) {
    Inherited out;
    const auto& ecs = world.ecs();
    const auto top_level = [&](entt::entity of) {
        const auto* transform = ecs.try_get<world::Transform2D>(of);
        return transform != nullptr && transform->top_level;
    };
    // Godot's top-level items hang from the canvas, not their parents: no z_index or tint
    // comes past one, though hiding a parent still hides it.
    bool add_z = !top_level(entity);
    bool add_tint = add_z;
    for (entt::entity up = tree.parent(entity); up != entt::null; up = tree.parent(up)) {
        const auto take = [&](int z, bool relative, bool visible, const Vec4* modulate) {
            if (add_z) {
                out.z_index += z;
                add_z = relative;
            }
            out.visible = out.visible && visible;
            if (add_tint && modulate != nullptr) {
                out.modulate = {out.modulate.x * modulate->x, out.modulate.y * modulate->y,
                                out.modulate.z * modulate->z, out.modulate.w * modulate->w};
            }
        };
        if (const auto* sprite = ecs.try_get<world::Sprite2D>(up); sprite != nullptr) {
            take(sprite->z_index, sprite->z_as_relative, sprite->visible, &sprite->modulate);
        } else if (const auto* layer = ecs.try_get<world::TileMapLayer>(up); layer != nullptr) {
            take(layer->z_index, layer->z_as_relative, layer->visible, &layer->modulate);
        } else if (const auto* emitter = ecs.try_get<world::CPUParticles2D>(up);
                   emitter != nullptr) {
            take(emitter->z_index, emitter->z_as_relative, emitter->visible, nullptr);
        } else if (const auto* gpu = ecs.try_get<world::GPUParticles2D>(up); gpu != nullptr) {
            take(gpu->z_index, gpu->z_as_relative, gpu->visible, nullptr);
        }
        if (top_level(up)) {
            add_z = false;
            add_tint = false;
        }
    }
    return out;
}

int z_index(int own, bool relative, const Inherited& from_parents) {
    constexpr int kZLimit = 4096;
    return std::clamp(relative ? own + from_parents.z_index : own, -kZLimit, kZLimit);
}

double material_time(std::uint64_t tick, double tick_seconds, double interpolation) {
    return std::max(static_cast<double>(tick) - 1.0 + std::clamp(interpolation, 0.0, 1.0), 0.0) *
           tick_seconds;
}

CanvasRenderer::CanvasRenderer(render::RenderDevice& device, asset::Assets& assets)
    : device_(&device), assets_(&assets),
      scene3d_(
          device,
          [this](const std::string& path, render::Filter filter) -> Result<scene3d::SourceTexture> {
              auto made = texture(path, filter);
              if (!made) {
                  return made.error();
              }
              return scene3d::SourceTexture{.id = made.value()->id,
                                            .width = made.value()->width,
                                            .height = made.value()->height};
          },
          [this](const std::string& path) -> Result<scene3d::SourceModel> {
              auto it = models_.find(path);
              if (it == models_.end()) {
                  auto loaded = assets_->load<scene3d::Model>(path);
                  if (!loaded) {
                      return loaded.error();
                  }
                  it = models_.emplace(path, std::move(loaded).value()).first;
              }
              return scene3d::SourceModel{.model = it->second.share(),
                                          .version = it->second.version()};
          }) {}

CanvasRenderer::~CanvasRenderer() {
    for (const auto& [key, texture] : textures_) {
        device_->destroy_texture(texture.id);
    }
    for (const auto& [texture, version] : pages_) {
        device_->destroy_texture(texture);
    }
    for (const render::TextureId texture : {atlas_, shadow_map_}) {
        if (texture != render::TextureId::None) {
            device_->destroy_texture(texture);
        }
    }
    for (const auto& [path, entry] : materials_) {
        if (entry.shader != render::ShaderId::None) {
            device_->destroy_shader(entry.shader);
        }
    }
    for (const auto& [entity, system] : gpu_systems_) {
        if (system.id != render::ParticlesId::None) {
            device_->destroy_particles(system.id);
        }
    }
    if (upscaler_ != nullptr) {
        upscaler_->release(*device_);
    }
}

void CanvasRenderer::before_tick(const world::World& world) {
    scene3d_.before_tick(world);
    previous_.clear();
    const auto& ecs = world.ecs();
    const world::Tree2D tree(world);
    // Where each thing that draws is, after its parents.
    const auto keep = [&](auto view) {
        for (const entt::entity entity : view) {
            previous_.emplace(entity, tree.global(entity));
        }
    };
    keep(ecs.view<const world::Transform2D, const world::Sprite2D>());
    keep(ecs.view<const world::Transform2D, const world::TileMapLayer>());
    keep(ecs.view<const world::Transform2D, const world::PointLight2D>());
    keep(ecs.view<const world::Transform2D, const world::DirectionalLight2D>());
    keep(ecs.view<const world::Transform2D, const world::LightOccluder2D>());
    keep(ecs.view<const world::Transform2D, const world::CPUParticles2D>());
    keep(ecs.view<const world::Transform2D, const world::GPUParticles2D>());
    keep(ecs.view<const world::Transform2D, const world::CollisionShape2D>());
    keep(ecs.view<const world::Transform2D, const world::CollisionPolygon2D>());
    previous_parallax_.clear();
    for (const auto [entity, parallax, transform] :
         ecs.view<const world::Parallax2D, const world::Transform2D>().each()) {
        previous_parallax_.emplace(entity, transform.position);
    }
    previous_view_ = world::camera_view(world, tree, ui::settings(world).viewport);
}

void CanvasRenderer::warn_once(const std::string& key, std::string_view message,
                               std::string_view what, std::string_view event) {
    if (warned_.insert(key + "\n" + std::string(message)).second) {
        log::warn(event, {{what, key}, {"error", message}});
    }
}

Result<const TileSet*> CanvasRenderer::tile_set(const std::string& path) {
    auto it = tile_sets_.find(path);
    if (it == tile_sets_.end()) {
        auto loaded = assets_->load<TileSet>(path);
        if (!loaded) {
            return loaded.error();
        }
        it = tile_sets_.emplace(path, std::move(loaded).value()).first;
    }
    return it->second.get();
}

Result<const render::ParticleProcess*> CanvasRenderer::process_material(const std::string& path) {
    auto it = processes_.find(path);
    if (it == processes_.end()) {
        auto loaded = assets_->load<render::ParticleProcess>(path);
        if (!loaded) {
            return loaded.error();
        }
        it = processes_.emplace(path, std::move(loaded).value()).first;
    }
    return it->second.get();
}

namespace {

// The steps before steps[from] that a system runs again with those after, for its trails' copies
// to run behind: as many as the trails of the steps after span, or all there are.
std::size_t trail_history(std::span<const render::ParticleStep> steps, std::size_t from) {
    std::uint32_t span = 1;
    for (std::size_t i = from; i < steps.size(); ++i) {
        span = std::max(span, steps[i].trail_steps);
    }
    return std::min<std::size_t>(from, span - 1);
}

} // namespace

Result<std::optional<CanvasRenderer::ParticleJob>>
CanvasRenderer::particle_job(GpuSystem& system, const ParticleSteps& kept,
                             const render::ParticleProcess* process) {
    const std::uint64_t end = kept.first + kept.steps.size();
    std::optional<ParticleJob> job;
    if (system.id == render::ParticlesId::None || system.serial != kept.serial ||
        system.amount != kept.amount || system.trail != kept.trail || system.next < kept.first) {
        if (system.id != render::ParticlesId::None) {
            device_->destroy_particles(system.id);
            system.id = render::ParticlesId::None;
        }
        auto made = device_->create_particles(kept.amount, kept.trail);
        if (!made) {
            return made.error();
        }
        system.id = made.value();
        system.serial = kept.serial;
        system.amount = kept.amount;
        system.trail = kept.trail;
        render::ParticleStep first = kept.steps.front();
        first.clear = true;
        job = ParticleJob{.particles = system.id,
                          .process = process,
                          .first = first,
                          .rest = kept.steps,
                          .history = 1,
                          .shapes = kept.shapes};
    } else if (system.next < end) {
        const auto from = static_cast<std::size_t>(system.next - kept.first);
        const std::size_t history = trail_history(kept.steps, from);
        job = ParticleJob{.particles = system.id,
                          .process = process,
                          .first = std::nullopt,
                          .rest = kept.steps.subspan(from - history),
                          .history = history,
                          .shapes = kept.shapes};
    }
    system.next = end;
    return job;
}

Result<void> CanvasRenderer::chain_jobs(std::span<const ParticleChainMember> chain,
                                        std::span<const render::ParticleProcess* const> processes,
                                        std::vector<ParticleJob>& jobs) {
    const ParticleSteps& root = chain.front().steps;
    const std::uint64_t end = root.first + root.steps.size();
    std::vector<GpuSystem*> systems;
    for (const ParticleChainMember& member : chain) {
        systems.push_back(&gpu_systems_[member.entity]);
    }
    const std::uint64_t next = systems.front()->next;
    const bool remake = std::ranges::any_of(chain, [&](const ParticleChainMember& member) {
        const GpuSystem& system = gpu_systems_[member.entity];
        return system.id == render::ParticlesId::None || system.serial != member.steps.serial ||
               system.amount != member.steps.amount || system.trail != member.steps.trail ||
               system.next < root.first || system.next != next;
    });
    if (remake) {
        for (std::size_t k = 0; k < chain.size(); ++k) {
            GpuSystem& system = *systems[k];
            if (system.id != render::ParticlesId::None) {
                device_->destroy_particles(system.id);
                system.id = render::ParticlesId::None;
            }
            auto made = device_->create_particles(chain[k].steps.amount, chain[k].steps.trail);
            if (!made) {
                return made.error();
            }
            system.id = made.value();
            system.serial = chain[k].steps.serial;
            system.amount = chain[k].steps.amount;
            system.trail = chain[k].steps.trail;
        }
    }
    if (remake || next < end) {
        // The links run as many steps each, after as much history as any needs.
        const auto from = static_cast<std::size_t>(next - root.first);
        std::size_t history = 1;
        if (!remake) {
            history = 0;
            for (const ParticleChainMember& member : chain) {
                history = std::max(history, trail_history(member.steps.steps, from));
            }
        }
        for (std::size_t k = 0; k < chain.size(); ++k) {
            const ParticleSteps& kept = chain[k].steps;
            ParticleJob job{.particles = systems[k]->id,
                            .process = processes[k],
                            .first = std::nullopt,
                            .rest = kept.steps,
                            .history = history,
                            .shapes = kept.shapes,
                            .links = k == 0 ? chain.size() : 1};
            if (remake) {
                job.first = kept.steps.front();
                job.first->clear = true;
            } else {
                job.rest = kept.steps.subspan(from - history);
            }
            jobs.push_back(job);
        }
    }
    for (GpuSystem* system : systems) {
        system->next = end;
    }
    return {};
}

void CanvasRenderer::mirror_chain(std::span<const ParticleChainMember> chain,
                                  std::span<const render::ParticleProcess* const> processes) {
    const ParticleSteps& root = chain.front().steps;
    const std::uint64_t end = root.first + root.steps.size();
    std::vector<GpuSystem*> systems;
    for (const ParticleChainMember& member : chain) {
        systems.push_back(&gpu_systems_[member.entity]);
    }
    const std::uint64_t next = systems.front()->cpu_next;
    const bool stale = std::ranges::any_of(chain, [&](const ParticleChainMember& member) {
        const GpuSystem& system = gpu_systems_[member.entity];
        return system.cpu.size() != std::size_t{member.steps.amount} * member.steps.trail ||
               system.cpu_serial != member.steps.serial || system.cpu_next < root.first ||
               system.cpu_next != next;
    });
    if (stale) {
        std::vector<ChainRun> runs;
        for (std::size_t k = 0; k < chain.size(); ++k) {
            runs.push_back({.amount = chain[k].steps.amount,
                            .process = processes[k],
                            .steps = chain[k].steps.steps,
                            .shapes = chain[k].steps.shapes,
                            .trail = chain[k].steps.trail});
        }
        std::vector<std::vector<render::Particle>> all = run_chain(runs);
        for (std::size_t k = 0; k < chain.size(); ++k) {
            systems[k]->cpu = std::move(all[k]);
        }
    } else if (next < end) {
        // As chain_jobs runs them on the device.
        const auto from = static_cast<std::size_t>(next - root.first);
        std::size_t history = 0;
        for (const ParticleChainMember& member : chain) {
            history = std::max(history, trail_history(member.steps.steps, from));
        }
        std::vector<render::ParticleChainLink> links;
        for (std::size_t k = 0; k < chain.size(); ++k) {
            const ParticleSteps& kept = chain[k].steps;
            links.push_back({.particles = systems[k]->cpu,
                             .process = processes[k],
                             .steps = kept.steps.subspan(from - history),
                             .shapes = kept.shapes,
                             .trail = kept.trail,
                             .history = history});
        }
        render::process_particle_chain(links);
    }
    for (std::size_t k = 0; k < chain.size(); ++k) {
        systems[k]->cpu_serial = chain[k].steps.serial;
        systems[k]->cpu_next = end;
    }
}

std::size_t CanvasRenderer::mirror(GpuSystem& system, const ParticleSteps& kept,
                                   const render::ParticleProcess& process) {
    const std::uint64_t end = kept.first + kept.steps.size();
    if (system.cpu.size() != std::size_t{kept.amount} * kept.trail ||
        system.cpu_serial != kept.serial || system.cpu_next < kept.first) {
        system.cpu = run_steps(kept.amount, process, kept.steps, kept.shapes, kept.trail);
    } else if (system.cpu_next < end) {
        // As particle_job runs them on the device.
        const auto from = static_cast<std::size_t>(system.cpu_next - kept.first);
        const std::size_t history = trail_history(kept.steps, from);
        const render::ParticleChainLink link{.particles = system.cpu,
                                             .process = &process,
                                             .steps = kept.steps.subspan(from - history),
                                             .shapes = kept.shapes,
                                             .trail = kept.trail,
                                             .history = history};
        render::process_particle_chain({&link, 1});
    }
    system.cpu_serial = kept.serial;
    system.cpu_next = end;
    return live_count(system.cpu, kept.trail);
}

Result<void> CanvasRenderer::run_particle_jobs(std::span<const ParticleJob> jobs) {
    for (std::size_t i = 0, links = 1; i < jobs.size(); i += links) {
        links = std::clamp<std::size_t>(jobs[i].links, 1,
                                        std::min(jobs.size() - i, render::kMaxParticleChain));
        std::array<render::ParticleRun, render::kMaxParticleChain> runs;
        const auto run = [&](bool first) {
            for (std::size_t k = 0; k < links; ++k) {
                const ParticleJob& job = jobs[i + k];
                runs.at(k) = {.particles = job.particles,
                              .process = job.process,
                              .steps = first ? std::span<const render::ParticleStep>{&*job.first, 1}
                                             : job.rest,
                              .shapes = job.shapes,
                              .history = first ? 0 : job.history};
            }
            return device_->process_particle_chain({runs.data(), links});
        };
        Result<void> ran;
        if (jobs[i].first) {
            ran = run(true);
        }
        if (ran && jobs[i].rest.size() > jobs[i].history) {
            ran = run(false);
        }
        if (!ran) {
            for (auto& [entity, system] : gpu_systems_) {
                system.serial = 0;
            }
            return ran;
        }
    }
    return {};
}

void CanvasRenderer::drop_unseen_gpu_systems() {
    std::erase_if(gpu_systems_, [&](const auto& entry) {
        if (entry.second.seen) {
            return false;
        }
        if (entry.second.id != render::ParticlesId::None) {
            device_->destroy_particles(entry.second.id);
        }
        return true;
    });
}

const CanvasRenderer::LayerCells& CanvasRenderer::cells(entt::entity entity,
                                                        const world::TileMapLayer& layer,
                                                        const TileSet& set,
                                                        std::uint64_t set_version) {
    auto [it, added] = layer_cells_.try_emplace(entity);
    LayerCells& cached = it->second;
    cached.seen = true;
    if (added || cached.set_version != set_version || cached.layer.tile_set != layer.tile_set ||
        !(cached.layer.origin == layer.origin) || cached.layer.legend != layer.legend ||
        cached.layer.rows != layer.rows) {
        cached.set_version = set_version;
        cached.layer = layer;
        auto decoded = decode_cells(layer, set);
        cached.error = decoded ? std::string() : decoded.error().message;
        cached.cells = decoded ? std::move(decoded).value() : Cells{};
        cached.order = draw_order(set, cached.cells);
    }
    return cached;
}

Result<const render::Image*> CanvasRenderer::light_image(const std::string& path) {
    auto it = light_images_.find(path);
    if (it == light_images_.end()) {
        auto loaded = assets_->load<render::Image>(path);
        if (!loaded) {
            return loaded.error();
        }
        it = light_images_.emplace(path, std::move(loaded).value()).first;
    }
    return it->second.get();
}

render::Lighting CanvasRenderer::lighting(const world::World& world,
                                          const TransformOf& transform_of,
                                          const world::Canvases& canvases) {
    constexpr std::string_view kEvent = "a light can't shine";
    const auto images = [this](const std::string& path) { return light_image(path); };
    const auto tile_sets = [this](const std::string& path) { return tile_set(path); };
    std::vector<LightProblem> problems;
    SceneLighting scene = collect_lighting(world, images, transform_of, problems, &shadow_cache_,
                                           &canvases, tile_sets);
    const auto name_of = [&](entt::entity entity) {
        const std::string_view id = world.id_of(entity);
        return id.empty() ? "entity " + std::to_string(entt::to_entity(entity)) : std::string(id);
    };
    for (const LightProblem& problem : problems) {
        warn_once(name_of(problem.entity), problem.message, "id", kEvent);
    }
    canvas_lighting_ = std::move(scene.canvases);
    render::Lighting out;
    if (!scene.lit) {
        // Nothing lights the world now: let the light textures go.
        for (render::TextureId* texture : {&atlas_, &shadow_map_}) {
            if (*texture != render::TextureId::None) {
                device_->destroy_texture(*texture);
                *texture = render::TextureId::None;
            }
        }
        atlas_key_.clear();
        atlas_rects_.clear();
        shadow_rows_ = 0;
        shadow_pixels_.clear();
        return out;
    }
    // The atlas, packed again when a light texture comes, goes or changes.
    std::vector<std::pair<std::string, std::uint64_t>> key;
    for (const SceneLight& light : scene.lights) {
        if (light.light.kind != render::LightKind::Point) {
            continue;
        }
        const std::uint64_t version =
            light.texture.empty() ? 0 : light_images_.at(light.texture).version();
        if (std::ranges::find(key, std::pair{light.texture, version}) == key.end()) {
            key.emplace_back(light.texture, version);
        }
    }
    if (key != atlas_key_) {
        if (atlas_ != render::TextureId::None) {
            device_->destroy_texture(atlas_);
            atlas_ = render::TextureId::None;
        }
        atlas_key_.clear();
        atlas_rects_.clear();
        if (!key.empty()) {
            auto atlas = pack_light_atlas(scene.lights, images);
            if (!atlas) {
                warn_once("atlas", atlas.error().message, "lights", kEvent);
            } else if (auto made = device_->create_texture({.width = atlas.value().width,
                                                            .height = atlas.value().height,
                                                            .filter = render::Filter::Linear,
                                                            .render_target = false},
                                                           atlas.value().pixels);
                       !made) {
                warn_once("atlas", made.error().message, "lights", kEvent);
            } else {
                atlas_ = made.value();
                atlas_key_ = key;
                for (const SceneLight& light : scene.lights) {
                    atlas_rects_[light.texture] = light.light.atlas_rect;
                }
            }
        }
    }
    for (SceneLight& light : scene.lights) {
        if (light.light.kind != render::LightKind::Point) {
            out.lights.push_back(light.light);
        } else if (const auto rect = atlas_rects_.find(light.texture); rect != atlas_rects_.end()) {
            light.light.atlas_rect = rect->second;
            out.lights.push_back(light.light);
        }
    }
    out.atlas = atlas_;

    // The shadow map, uploaded when its rows change.
    if (scene.shadow_rows != shadow_rows_ && shadow_map_ != render::TextureId::None) {
        device_->destroy_texture(shadow_map_);
        shadow_map_ = render::TextureId::None;
        shadow_pixels_.clear();
    }
    shadow_rows_ = scene.shadow_rows;
    if (scene.shadow_rows > 0) {
        Result<void> uploaded;
        if (shadow_map_ == render::TextureId::None) {
            auto made = device_->create_texture({.width = render::kShadowMapWidth,
                                                 .height = scene.shadow_rows,
                                                 .filter = render::Filter::Nearest,
                                                 .render_target = false},
                                                scene.shadow_pixels);
            if (made) {
                shadow_map_ = made.value();
            } else {
                uploaded = made.error();
            }
        } else if (scene.shadow_pixels != shadow_pixels_) {
            uploaded = device_->update_texture(shadow_map_, scene.shadow_pixels);
        }
        if (!uploaded || shadow_map_ == render::TextureId::None) {
            if (!uploaded) {
                warn_once("shadow map", uploaded.error().message, "lights", kEvent);
            }
            for (render::Light& light : out.lights) {
                light.shadow_row = -1;
            }
        } else {
            shadow_pixels_ = std::move(scene.shadow_pixels);
            out.shadow_map = shadow_map_;
        }
    }
    return out;
}

Result<const CanvasRenderer::Texture*> CanvasRenderer::texture(const std::string& path,
                                                               render::Filter filter) {
    const auto key = std::make_pair(path, filter);
    auto it = textures_.find(key);
    if (it == textures_.end()) {
        Texture entry;
        if (path.starts_with(ui::kBuiltinPrefix)) {
            entry.builtin = ui::builtin_icon(path);
            if (entry.builtin == nullptr) {
                return make_error(ErrorCode::NotFound, "no built-in icon \"" + path + "\"");
            }
        } else {
            auto image = assets_->load<render::Image>(path);
            if (!image) {
                return image.error();
            }
            entry.image = std::move(image).value();
        }
        it = textures_.emplace(key, std::move(entry)).first;
    }
    Texture& entry = it->second;
    if (entry.id != render::TextureId::None && entry.version == entry.image_version()) {
        return &entry;
    }
    // New, or its file changed since it was made. A file's compressed blocks go to a device that
    // samples them as they are, its pixels to any other.
    const render::Image& image = entry.pixels();
    const bool blocks = !image.blocks.empty() && device_->supports(image.block_format);
    const render::TextureFormat format = blocks ? image.block_format : render::TextureFormat::Rgba8;
    const std::span<const std::uint8_t> texels = blocks ? image.blocks : image.pixels;
    if (entry.id != render::TextureId::None &&
        (entry.width != image.width || entry.height != image.height || entry.format != format)) {
        device_->destroy_texture(entry.id);
        entry.id = render::TextureId::None;
    }
    if (entry.id == render::TextureId::None) {
        auto made = device_->create_texture({.width = image.width,
                                             .height = image.height,
                                             .filter = filter,
                                             .render_target = false,
                                             .format = format},
                                            texels);
        if (!made) {
            return made.error();
        }
        entry.id = made.value();
    } else if (auto updated = device_->update_texture(entry.id, texels); !updated) {
        return updated.error();
    }
    entry.version = entry.image_version();
    entry.width = image.width;
    entry.height = image.height;
    entry.format = format;
    return &entry;
}

Result<const CanvasRenderer::MaterialEntry*>
CanvasRenderer::material_entry(const std::string& path) {
    auto it = materials_.find(path);
    if (it == materials_.end()) {
        auto loaded = assets_->load<CanvasMaterial>(path);
        if (!loaded) {
            return loaded.error();
        }
        MaterialEntry entry;
        entry.material = std::move(loaded).value();
        it = materials_.emplace(path, std::move(entry)).first;
    }
    MaterialEntry& entry = it->second;
    const bool changed = entry.material_version != entry.material.version() ||
                         (entry.source && entry.source_version != entry.source.version());
    if (changed) {
        entry.material_version = entry.material.version();
        entry.error.clear();
        const CanvasMaterial& material = *entry.material;
        std::shared_ptr<const render::CompiledShader> compiled;
        if (!material.shader.empty()) {
            // The shader file is an asset, so poll() reloads it and the shader is made again.
            auto made = compile_material_shader(
                material, [&](std::string_view file) -> Result<std::string> {
                    if (!entry.source || entry.source.path() != file) {
                        auto source = assets_->load<std::string>(file);
                        if (!source) {
                            return source.error();
                        }
                        entry.source = std::move(source).value();
                    }
                    return *entry.source;
                });
            if (made) {
                compiled = std::move(made).value();
            } else {
                entry.error = made.error().message;
            }
        }
        if (material.shader.empty() || material.compiled) {
            entry.source = {};
        }
        entry.source_version = entry.source ? entry.source.version() : 0;
        if (compiled != entry.compiled) {
            if (entry.shader != render::ShaderId::None) {
                device_->destroy_shader(entry.shader);
                entry.shader = render::ShaderId::None;
            }
            entry.compiled = compiled;
            if (compiled) {
                auto shader = device_->create_shader(*compiled);
                if (shader) {
                    entry.shader = shader.value();
                } else {
                    entry.error = material.shader + ": " + shader.error().message;
                }
            }
        }
    }
    if (!entry.error.empty()) {
        return make_error(ErrorCode::InvalidArgument, entry.error);
    }
    return &entry;
}

Result<render::Material> CanvasRenderer::material(const world::Sprite2D& sprite,
                                                  render::Filter filter, double time,
                                                  render::LightMode* light_mode) {
    auto entry = material_entry(sprite.material);
    if (!entry) {
        return entry.error();
    }
    std::string failed;
    auto instance = instantiate(*entry.value()->material, sprite.material_params, time, &failed);
    if (!instance) {
        return make_error(instance.error().code,
                          "material_params/" + failed + ": " + instance.error().message);
    }
    render::Material out;
    out.blend = instance.value().blend;
    *light_mode = instance.value().light_mode;
    out.program = std::move(instance.value().program);
    out.shader = entry.value()->shader;
    for (std::size_t slot = 0; slot < out.textures.size(); ++slot) {
        const MaterialTextureRef& ref = instance.value().textures.at(slot);
        if (ref.path.empty()) {
            continue;
        }
        auto texture = this->texture(ref.path, resolve_filter(ref.filter, filter));
        if (!texture) {
            return make_error(texture.error().code, ref.path + ": " + texture.error().message);
        }
        out.textures.at(slot) = {.texture = texture.value()->id, .repeat = ref.repeat};
    }
    return out;
}

Result<render::TextureId> CanvasRenderer::atlas_page(const text::Fonts& fonts, std::uint32_t page) {
    // Another world's fonts have their own pages: upload every page again.
    if (pages_of_ != &fonts) {
        pages_of_ = &fonts;
        for (auto& entry : pages_) {
            entry.second = static_cast<std::uint64_t>(-1);
        }
    }
    const text::AtlasPage& source = fonts.pages().at(page);
    while (pages_.size() <= page) {
        pages_.emplace_back(render::TextureId::None, static_cast<std::uint64_t>(-1));
    }
    auto& [texture, version] = pages_[page];
    if (texture == render::TextureId::None) {
        auto created = device_->create_texture({.width = source.width,
                                                .height = source.height,
                                                .filter = render::Filter::Linear,
                                                .render_target = false},
                                               source.rgba);
        if (!created) {
            return created.error();
        }
        texture = created.value();
        version = source.version;
    } else if (version != source.version) {
        if (auto updated = device_->update_texture(texture, source.rgba); !updated) {
            return updated.error();
        }
        version = source.version;
    }
    return texture;
}

// What draw gathered for a frame, for submit to draw.
struct CanvasRenderer::Submission {
    const CanvasOptions& options;
    const render::Lighting& lighting;
    render::Scaling scaling;
    const std::vector<Item>& items;
    const std::vector<render::Vertex2D>& vertices;
    const CollisionShapes& shapes;
    std::optional<render::Mat4> camera; // the world canvas's
    std::size_t world_canvas = 0;
    std::size_t canvases = 0; // canvases.all().size()
    const ui::DrawList& controls;
    const ui::DrawList& overlay;
    const std::vector<std::optional<render::TextureId>>& control_textures;
    const std::vector<std::optional<render::TextureId>>& overlay_textures;
    const std::vector<std::optional<render::Mat4>>& control_matrices;
    std::span<const ParticleJob> particle_jobs;
    text::Fonts& fonts;
    const std::optional<scene3d::Frame3D>& scene; // the 3D world's, under every canvas
    // What the frame draws, counted when it draws.
    std::size_t sprites = 0;
    std::size_t tiles = 0;
    std::size_t particles = 0;
    std::size_t shadows = 0;
};

Result<void> CanvasRenderer::draw(const world::World& world, const CanvasOptions& options) {
    SLASH_PROFILE_ZONE("CanvasRenderer::draw");
    std::vector<ParticleJob> particle_jobs;
    std::vector<Item> items;
    std::vector<render::Vertex2D> vertices;
    std::size_t sprites = 0;
    std::size_t tiles = 0;
    std::size_t particles = 0;
    const auto& ecs = world.ecs();
    const auto name_of = [&](entt::entity entity) {
        const std::string_view id = world.id_of(entity);
        return id.empty() ? "entity " + std::to_string(entt::to_entity(entity)) : std::string(id);
    };
    const world::Tree2D tree(world);
    // The Parallax2D whose repeat an entity's drawing takes, the nearest of it and its parents
    // that repeats, as Godot's repeat_source_item; null for none.
    const bool parallaxes = !ecs.view<const world::Parallax2D>().empty();
    std::unordered_map<entt::entity, entt::entity> sources;
    const auto repeat_source = [&](entt::entity entity) -> entt::entity {
        if (!parallaxes) {
            return entt::null;
        }
        const auto [found, added] = sources.try_emplace(entity, entt::null);
        if (added) {
            // Not past a top_level entity, which hangs from the canvas as Godot's.
            for (entt::entity up = entity; up != entt::null; up = tree.transform_parent(up)) {
                if (const auto* parallax = ecs.try_get<world::Parallax2D>(up);
                    parallax != nullptr &&
                    (parallax->repeat_size.x > 0.0F || parallax->repeat_size.y > 0.0F)) {
                    found->second = up;
                    break;
                }
            }
        }
        return found->second;
    };
    // Whether a repeating Parallax2D wrapped round in the last tick: drawn part of the way from
    // where it was, it would slide back across a repeat, so it draws where it is, as Godot
    // doesn't interpolate a Parallax2D.
    const auto wrapped = [&](entt::entity source) {
        const auto before = previous_parallax_.find(source);
        const auto* transform = ecs.try_get<world::Transform2D>(source);
        if (before == previous_parallax_.end() || transform == nullptr) {
            return false;
        }
        const world::Parallax2D& parallax = ecs.get<world::Parallax2D>(source);
        const Vec2 moved = transform->position - before->second;
        const float across = std::abs(parallax.repeat_size.x * transform->scale.x);
        const float down = std::abs(parallax.repeat_size.y * transform->scale.y);
        return (across > 0.0F && std::abs(moved.x) * 2.0F >= across) ||
               (down > 0.0F && std::abs(moved.y) * 2.0F >= down);
    };
    // Where each entity draws: its transform after its parents', part of the way from the last
    // tick's.
    const auto transform_of = [&](entt::entity entity, const world::Transform2D& /*current*/) {
        world::Transform2D now = tree.global(entity);
        if (options.interpolation < 1.0) {
            if (const auto before = previous_.find(entity); before != previous_.end()) {
                const entt::entity source = repeat_source(entity);
                if (source == entt::null || !wrapped(source)) {
                    return interpolate(before->second, now, options.interpolation);
                }
            }
        }
        return now;
    };
    FrameContents* contents = options.contents;
    if (contents != nullptr) {
        *contents = {};
    }
    // Something that can't draw as written, for FrameContents.
    const auto problem = [&](entt::entity entity, std::string path, std::string message) {
        if (contents != nullptr) {
            contents->problems.push_back(
                {.entity = entity, .path = std::move(path), .message = std::move(message)});
        }
    };
    // The viewport, which controls are laid out in and the camera fills.
    const Vec2 viewport = options.width > 0.0F && options.height > 0.0F
                              ? Vec2{options.width, options.height}
                              : ui::settings(world).viewport;
    // The 3D world, seen through its current Camera3D, which fills the viewport.
    std::optional<scene3d::Frame3D> scene =
        scene3d_.prepare(world, viewport.x, viewport.y, options.interpolation, options.clear,
                         settings(world).shadows, options.time);
    if (scene) {
        scene->scene.method = settings(world).rendering_method;
    }
    for (const scene3d::Problem& unseen : scene3d_.problems()) {
        problem(unseen.entity, unseen.path, unseen.message);
        warn_once(name_of(unseen.entity), unseen.message, "id", "a 3D node can't draw");
    }
    // What the world is seen through: the current camera's view, part of the way from the one
    // before the last tick, as Godot interpolates its canvas transform.
    world::CameraView view = world::camera_view(world, tree, viewport);
    if (options.interpolation < 1.0 && view.camera != entt::null &&
        view.camera == previous_view_.camera) {
        view.canvas = world::affine(interpolate(world::parts(previous_view_.canvas),
                                                world::parts(view.canvas), options.interpolation));
    }
    // The canvases, the world's through that view, and the matrix each one's draws move by:
    // the camera's for the world, a CanvasLayer's transform, or none to leave them in place.
    const world::Canvases canvases(world, tree, view, viewport);
    const std::vector<world::Canvas>& canvas_list = canvases.all();
    const std::size_t world_canvas = canvases.world_canvas();
    std::vector<std::optional<render::Mat4>> matrices(canvas_list.size());
    for (std::size_t c = 0; c < canvas_list.size(); ++c) {
        if (c == world_canvas ? view.camera != entt::null
                              : canvas_list[c].transform != world::Affine2D{}) {
            matrices[c] = matrix(canvas_list[c].transform);
        }
    }
    const std::optional<render::Mat4>& camera = matrices[world_canvas];
    // Where a Parallax2D's repeat draws its children, worked out once each frame.
    std::unordered_map<entt::entity, std::vector<Vec2>> repeats;
    const auto offsets_of = [&](entt::entity source) -> const std::vector<Vec2>& {
        auto [found, added] = repeats.try_emplace(source);
        if (added) {
            found->second =
                repeat_offsets(ecs.get<world::Parallax2D>(source),
                               world::affine(transform_of(source, world::Transform2D{})));
        }
        return found->second;
    };
    // Draws the vertices from first on again at each repeat of the Parallax2D the entity is
    // under, as Godot's canvas_render_items records an item once for each.
    const auto repeat = [&](entt::entity entity, std::size_t first) {
        const entt::entity source = repeat_source(entity);
        if (source == entt::null || first >= vertices.size()) {
            return;
        }
        const std::vector<Vec2>& offsets = offsets_of(source);
        const std::vector<render::Vertex2D> once(
            vertices.begin() + static_cast<std::ptrdiff_t>(first), vertices.end());
        vertices.resize(first);
        for (const Vec2 offset : offsets) {
            for (render::Vertex2D vertex : once) {
                vertex.x += offset.x;
                vertex.y += offset.y;
                vertices.push_back(vertex);
            }
        }
    };
    const render::Lighting lights = lighting(world, transform_of, canvases);
    // How lights shade a sprite or layer: as its canvas is lit, or not at all.
    const auto shading_for = [&](render::LightMode mode, int light_mask, int z_index,
                                 std::size_t canvas) {
        render::Shading shading;
        if (const CanvasLighting& lit = canvas_lighting_.at(canvas); lit.lit) {
            shading.mode = mode;
            shading.light_mask = static_cast<std::uint32_t>(std::max(light_mask, 0));
            shading.z_index = z_index;
            shading.layer = lit.layer;
            shading.ambient = lit.ambient;
            shading.to_lights = lit.to_lights;
        }
        return shading;
    };
    for (const auto [entity, current, own_sprite] :
         ecs.view<const world::Transform2D, const world::Sprite2D>().each()) {
        const Inherited from_parents = inherited(world, tree, entity);
        const std::size_t canvas = canvases.of(entity);
        if (!own_sprite.visible || !from_parents.visible || own_sprite.texture.empty() ||
            !canvas_list[canvas].visible) {
            continue;
        }
        // Tinted by its parents' modulate too.
        std::optional<world::Sprite2D> tinted;
        if (from_parents.modulate != Vec4{1.0F, 1.0F, 1.0F, 1.0F}) {
            tinted = own_sprite;
            tinted->modulate = {own_sprite.modulate.x * from_parents.modulate.x,
                                own_sprite.modulate.y * from_parents.modulate.y,
                                own_sprite.modulate.z * from_parents.modulate.z,
                                own_sprite.modulate.w * from_parents.modulate.w};
        }
        const world::Sprite2D& sprite = tinted ? *tinted : own_sprite;
        const int z = z_index(sprite.z_index, sprite.z_as_relative, from_parents);
        const render::Filter filter = resolve_filter(sprite.texture_filter, options.texture_filter);
        auto texture = this->texture(sprite.texture, filter);
        if (!texture) {
            warn_once(sprite.texture, texture.error().message, "texture");
            problem(entity, "/components/Sprite2D/texture", texture.error().message);
            continue;
        }
        render::Material material;
        render::LightMode light_mode = render::LightMode::Normal;
        if (!sprite.material.empty()) {
            auto made = this->material(sprite, filter, options.time, &light_mode);
            if (!made) {
                warn_once(sprite.material, made.error().message, "material");
                problem(entity, "/components/Sprite2D/material", made.error().message);
                continue;
            }
            material = std::move(made).value();
        }
        material.shading = shading_for(light_mode, sprite.light_mask, z, canvas);
        if (material.shading.mode != render::LightMode::Unshaded) {
            // Normal and specular maps, left out with a warning if they don't load.
            for (const auto& [path, slot, field] :
                 {std::tuple{&sprite.normal_texture, &material.shading.normal_map,
                             "normal_texture"},
                  std::tuple{&sprite.specular_texture, &material.shading.specular_map,
                             "specular_texture"}}) {
                if (path->empty()) {
                    continue;
                }
                if (auto map = this->texture(*path, filter); map) {
                    *slot = map.value()->id;
                } else {
                    warn_once(*path, map.error().message, "texture");
                    problem(entity, std::string("/components/Sprite2D/") + field,
                            map.error().message);
                }
            }
            material.shading.specular = {sprite.specular_color.x, sprite.specular_color.y,
                                         sprite.specular_color.z,
                                         sprite.specular_color.w * sprite.specular_shininess};
        }
        auto quad = sprite_quad(sprite, transform_of(entity, current), texture.value()->width,
                                texture.value()->height);
        if (!quad) {
            warn_once(name_of(entity), quad.error().message, "id");
            problem(entity, "/components/Sprite2D", quad.error().message);
            continue;
        }
        const std::size_t first = vertices.size();
        vertices.insert(vertices.end(), quad.value().begin(), quad.value().end());
        repeat(entity, first);
        items.push_back({.canvas = canvas,
                         .z_index = z,
                         .order = tree.draw_key(entity),
                         .part = 0,
                         .texture = texture.value()->id,
                         .first = first,
                         .count = vertices.size() - first,
                         .material = std::move(material),
                         .entity = entity,
                         .kind = FrameItem::Kind::Sprite,
                         .image = &*texture.value()->image,
                         .path = texture.value()->image.path(),
                         .drawn = 1,
                         .particles = render::ParticlesId::None,
                         .look = {}});
        ++sprites;
    }

    for (const auto [entity, emitter] : ecs.view<const world::CPUParticles2D>().each()) {
        const Inherited from_parents = inherited(world, tree, entity);
        const std::size_t canvas = canvases.of(entity);
        if (!emitter.visible || !from_parents.visible || !canvas_list[canvas].visible) {
            continue;
        }
        const int z = z_index(emitter.z_index, emitter.z_as_relative, from_parents);
        const std::vector<Particle> live = canvas::particles(world, entity);
        if (live.empty()) {
            continue;
        }
        constexpr std::string_view kEvent = "particles can't draw";
        const render::Filter filter =
            resolve_filter(emitter.texture_filter, options.texture_filter);
        // A particle is its texture's frame, or a white pixel.
        render::TextureId texture_id = render::TextureId::None;
        const render::Image* image = nullptr;
        std::string_view path;
        float frame_w = 1.0F;
        float frame_h = 1.0F;
        float sheet_w = 1.0F;
        float sheet_h = 1.0F;
        const int hframes = std::max(emitter.hframes, 1);
        const int vframes = std::max(emitter.vframes, 1);
        if (!emitter.texture.empty()) {
            auto texture = this->texture(emitter.texture, filter);
            if (!texture) {
                warn_once(emitter.texture, texture.error().message, "texture", kEvent);
                problem(entity, "/components/CPUParticles2D/texture", texture.error().message);
                continue;
            }
            texture_id = texture.value()->id;
            image = &*texture.value()->image;
            path = texture.value()->image.path();
            sheet_w = static_cast<float>(texture.value()->width);
            sheet_h = static_cast<float>(texture.value()->height);
            frame_w = sheet_w / static_cast<float>(hframes);
            frame_h = sheet_h / static_cast<float>(vframes);
        }
        render::Material material;
        render::LightMode light_mode = render::LightMode::Normal;
        if (!emitter.material.empty()) {
            world::Sprite2D look;
            look.material = emitter.material;
            look.material_params = emitter.material_params;
            auto made = this->material(look, filter, options.time, &light_mode);
            if (!made) {
                warn_once(emitter.material, made.error().message, "material", kEvent);
                problem(entity, "/components/CPUParticles2D/material", made.error().message);
                continue;
            }
            material = std::move(made).value();
        }
        material.shading = shading_for(light_mode, emitter.light_mask, z, canvas);
        // With local_coords, particles move with the entity's transform.
        world::Transform2D local;
        if (const auto* current = ecs.try_get<world::Transform2D>(entity);
            emitter.local_coords && current != nullptr) {
            local = transform_of(entity, *current);
        }
        const auto place = [&](Vec2 point) { return world_position(emitter, local, point); };
        const int total = hframes * vframes;
        const auto t = static_cast<float>(std::clamp(options.interpolation, 0.0, 1.0));
        const std::size_t first = vertices.size();
        for (const Particle& particle : live) {
            // The frame its animation is on, as Godot's particle animation picks it.
            auto frame = static_cast<int>(std::floor(particle.frame * static_cast<float>(total)));
            frame = emitter.anim_loop ? ((frame % total) + total) % total
                                      : std::clamp(frame, 0, total - 1);
            const int column = frame % hframes;
            const int row = frame / hframes;
            const float u0 = static_cast<float>(column) * frame_w / sheet_w;
            const float v0 = static_cast<float>(row) * frame_h / sheet_h;
            const float u1 = u0 + (frame_w / sheet_w);
            const float v1 = v0 + (frame_h / sheet_h);
            const Vec2 at{particle.previous.x + ((particle.position.x - particle.previous.x) * t),
                          particle.previous.y + ((particle.position.y - particle.previous.y) * t)};
            const Vec2 x_axis = particle.axis * (frame_w * 0.5F * particle.scale);
            const Vec2 y_axis =
                Vec2{-particle.axis.y, particle.axis.x} * (frame_h * 0.5F * particle.scale);
            const std::array<std::uint8_t, 4> rgba{
                to_byte(particle.color.x * from_parents.modulate.x),
                to_byte(particle.color.y * from_parents.modulate.y),
                to_byte(particle.color.z * from_parents.modulate.z),
                to_byte(particle.color.w * from_parents.modulate.w)};
            const auto corner = [&](float sx, float sy, float u, float v) {
                const Vec2 point = place(at + (x_axis * sx) + (y_axis * sy));
                return render::Vertex2D{.x = point.x, .y = point.y, .u = u, .v = v, .rgba = rgba};
            };
            const render::Vertex2D top_left = corner(-1.0F, -1.0F, u0, v0);
            const render::Vertex2D top_right = corner(1.0F, -1.0F, u1, v0);
            const render::Vertex2D bottom_left = corner(-1.0F, 1.0F, u0, v1);
            const render::Vertex2D bottom_right = corner(1.0F, 1.0F, u1, v1);
            vertices.insert(vertices.end(), {top_left, top_right, bottom_left, top_right,
                                             bottom_right, bottom_left});
        }
        repeat(entity, first);
        items.push_back({.canvas = canvas,
                         .z_index = z,
                         .order = tree.draw_key(entity),
                         .part = 0,
                         .texture = texture_id,
                         .first = first,
                         .count = vertices.size() - first,
                         .material = std::move(material),
                         .entity = entity,
                         .kind = FrameItem::Kind::Particles,
                         .image = image,
                         .path = path,
                         .drawn = live.size(),
                         .particles = render::ParticlesId::None,
                         .look = {}});
        particles += live.size();
    }

    // GPUParticles2D: each system runs the steps it hasn't on the device once the frame begins,
    // then draws there.
    for (auto& [entity, cached] : gpu_systems_) {
        cached.seen = false;
    }
    static const render::ParticleProcess kStill{};
    const double tick_seconds = settings(world).tick_seconds;
    constexpr std::string_view kParticlesEvent = "particles can't draw";
    const auto shown = [&](entt::entity entity) {
        return ecs.get<world::GPUParticles2D>(entity).visible &&
               inherited(world, tree, entity).visible && canvas_list[canvases.of(entity)].visible;
    };
    std::set<entt::entity> unprocessed; // emitters whose process material didn't load
    const auto process_of = [&](entt::entity entity) -> const render::ParticleProcess* {
        const auto& emitter = ecs.get<world::GPUParticles2D>(entity);
        if (emitter.process_material.empty()) {
            return &kStill;
        }
        auto loaded = this->process_material(emitter.process_material);
        if (!loaded) {
            if (unprocessed.insert(entity).second) {
                warn_once(emitter.process_material, loaded.error().message, "process_material",
                          kParticlesEvent);
                problem(entity, "/components/GPUParticles2D/process_material",
                        loaded.error().message);
            }
            return nullptr;
        }
        return loaded.value();
    };
    // A chain of sub-emitters runs from its root, all of it while any of it shows, before any
    // of it draws.
    std::set<entt::entity> chained;
    for (const auto [entity, emitter] : ecs.view<const world::GPUParticles2D>().each()) {
        const std::optional<ParticleSteps> kept = particle_steps(world, entity);
        if (!kept || kept->links < 2 || kept->link != 0) {
            continue;
        }
        const std::vector<ParticleChainMember> chain = particle_chain(world, entity);
        for (const ParticleChainMember& member : chain) {
            chained.insert(member.entity);
            gpu_systems_[member.entity].seen = true;
        }
        if (chain.empty() || kept->steps.empty() ||
            std::ranges::none_of(
                chain, [&](const ParticleChainMember& member) { return shown(member.entity); })) {
            continue;
        }
        std::vector<const render::ParticleProcess*> processes;
        processes.reserve(chain.size());
        for (const ParticleChainMember& member : chain) {
            processes.push_back(process_of(member.entity));
        }
        if (std::ranges::find(processes, nullptr) != processes.end()) {
            continue;
        }
        if (auto made = chain_jobs(chain, processes, particle_jobs); !made) {
            warn_once(name_of(entity), made.error().message, "id", kParticlesEvent);
            problem(entity, "/components/GPUParticles2D/amount", made.error().message);
            continue;
        }
        if (contents != nullptr) {
            mirror_chain(chain, processes);
        }
    }
    for (const auto [entity, emitter] : ecs.view<const world::GPUParticles2D>().each()) {
        const std::optional<ParticleSteps> kept = particle_steps(world, entity);
        if (!kept) {
            continue;
        }
        // Kept while hidden, to run the steps it missed once shown.
        GpuSystem& cached = gpu_systems_[entity];
        cached.seen = true;
        const Inherited from_parents = inherited(world, tree, entity);
        const std::size_t canvas = canvases.of(entity);
        if (kept->steps.empty() || !emitter.visible || !from_parents.visible ||
            !canvas_list[canvas].visible) {
            continue;
        }
        const int z = z_index(emitter.z_index, emitter.z_as_relative, from_parents);
        const render::ParticleProcess* process = process_of(entity);
        if (process == nullptr) {
            continue;
        }
        const render::Filter filter =
            resolve_filter(emitter.texture_filter, options.texture_filter);
        // A particle is its texture's frame, or a white pixel.
        render::ParticleDraw look;
        look.hframes = static_cast<std::uint32_t>(std::max(emitter.hframes, 1));
        look.vframes = static_cast<std::uint32_t>(std::max(emitter.vframes, 1));
        render::TextureId texture_id = render::TextureId::None;
        const render::Image* image = nullptr;
        std::string_view path;
        if (!emitter.texture.empty()) {
            auto texture = this->texture(emitter.texture, filter);
            if (!texture) {
                warn_once(emitter.texture, texture.error().message, "texture", kParticlesEvent);
                problem(entity, "/components/GPUParticles2D/texture", texture.error().message);
                continue;
            }
            texture_id = texture.value()->id;
            image = &*texture.value()->image;
            path = texture.value()->image.path();
            look.size = {
                static_cast<float>(texture.value()->width) / static_cast<float>(look.hframes),
                static_cast<float>(texture.value()->height) / static_cast<float>(look.vframes)};
        }
        render::Material material;
        render::LightMode light_mode = render::LightMode::Normal;
        if (!emitter.material.empty()) {
            world::Sprite2D sprite;
            sprite.material = emitter.material;
            sprite.material_params = emitter.material_params;
            auto made = this->material(sprite, filter, options.time, &light_mode);
            if (!made) {
                warn_once(emitter.material, made.error().message, "material", kParticlesEvent);
                problem(entity, "/components/GPUParticles2D/material", made.error().message);
                continue;
            }
            material = std::move(made).value();
        }
        material.shading = shading_for(light_mode, emitter.light_mask, z, canvas);
        const bool in_chain = chained.contains(entity);
        if (!in_chain) {
            auto job = particle_job(cached, *kept, process);
            if (!job) {
                warn_once(name_of(entity), job.error().message, "id", kParticlesEvent);
                problem(entity, "/components/GPUParticles2D/amount", job.error().message);
                continue;
            }
            if (job.value()) {
                particle_jobs.push_back(*job.value());
            }
        }
        if (cached.id == render::ParticlesId::None) {
            continue;
        }
        look.order = particle_order(emitter.draw_order);
        if (look.order != render::ParticleOrder::Index) {
            look.lifetime_split =
                render::particle_lifetime_split(kept->amount, kept->steps.back().system_phase);
        }
        look.anim_loop = emitter.anim_loop;
        look.modulate = from_parents.modulate;
        if (kept->trail > 1) {
            look.trail_subdivisions = static_cast<std::uint32_t>(
                std::clamp(emitter.trail_section_subdivisions, 1,
                           static_cast<int>(render::kMaxParticleTrailSubdivisions)));
            look.trail_steps = kept->steps.back().trail_steps;
            look.step_delta = kept->steps.back().delta;
        }
        if (emitter.interpolate) {
            // Drawn between ticks, as the rest of the world is: where they were that much
            // before the tick ended.
            const double behind = (1.0 - std::clamp(options.interpolation, 0.0, 1.0)) *
                                  tick_seconds * std::max(emitter.speed_scale, 0.0F);
            look.remainder = kept->remainder - static_cast<float>(behind);
        }
        // Particles in the emitter's coordinates move with it.
        world::Affine2D placed;
        if (emitter.local_coords && ecs.all_of<world::Transform2D>(entity)) {
            placed = world::affine(transform_of(entity, world::Transform2D{}));
        }
        // FrameContents gets the particles run on the CPU, as the device runs them.
        std::size_t live = 0;
        if (contents != nullptr) {
            live = in_chain ? live_count(cached.cpu, kept->trail) : mirror(cached, *kept, *process);
        }
        // Again at each repeat of the Parallax2D it is under.
        const entt::entity source = repeat_source(entity);
        const std::vector<Vec2> unmoved{Vec2{}};
        const std::vector<Vec2>& offsets = source != entt::null ? offsets_of(source) : unmoved;
        for (std::size_t r = 0; r < offsets.size(); ++r) {
            render::ParticleDraw at = look;
            at.transform = {placed.x.x,
                            placed.x.y,
                            placed.y.x,
                            placed.y.y,
                            placed.origin.x + offsets[r].x,
                            placed.origin.y + offsets[r].y};
            const std::size_t first = vertices.size();
            if (contents != nullptr) {
                append_live_quads(cached.cpu, at, kept->trail, vertices);
            }
            items.push_back({.canvas = canvas,
                             .z_index = z,
                             .order = tree.draw_key(entity),
                             .part = static_cast<std::uint32_t>(r),
                             .texture = texture_id,
                             .first = first,
                             .count = vertices.size() - first,
                             .material = material,
                             .entity = entity,
                             .kind = FrameItem::Kind::GpuParticles,
                             .image = image,
                             .path = path,
                             .drawn = r == 0 ? live : 0,
                             .particles = cached.id,
                             .look = at});
        }
        particles += kept->amount;
    }
    drop_unseen_gpu_systems();

    for (auto& [entity, cached] : layer_cells_) {
        cached.seen = false;
    }
    for (const auto [entity, layer] : ecs.view<const world::TileMapLayer>().each()) {
        const Inherited from_parents = inherited(world, tree, entity);
        const std::size_t canvas = canvases.of(entity);
        if (!layer.visible || !from_parents.visible || layer.tile_set.empty() ||
            layer.rows.empty() || !canvas_list[canvas].visible) {
            continue;
        }
        const int z = z_index(layer.z_index, layer.z_as_relative, from_parents);
        constexpr std::string_view kEvent = "a tile map layer can't draw";
        auto set = tile_set(layer.tile_set);
        if (!set) {
            warn_once(layer.tile_set, set.error().message, "tile_set", kEvent);
            problem(entity, "/components/TileMapLayer/tile_set", set.error().message);
            continue;
        }
        const LayerCells& cached =
            cells(entity, layer, *set.value(), tile_sets_.at(layer.tile_set).version());
        if (!cached.error.empty()) {
            warn_once(name_of(entity), "TileMapLayer" + cached.error, "id", kEvent);
            // The error starts with the bad value's JSON Pointer in the layer.
            const std::size_t split = cached.error.find(": ");
            problem(entity,
                    "/components/TileMapLayer" +
                        (split == std::string::npos ? "" : cached.error.substr(0, split)),
                    split == std::string::npos ? cached.error : cached.error.substr(split + 2));
            continue;
        }
        world::Transform2D transform;
        if (const auto* current = ecs.try_get<world::Transform2D>(entity); current != nullptr) {
            transform = transform_of(entity, *current);
        }
        const float cos_r = std::cos(transform.rotation);
        const float sin_r = std::sin(transform.rotation);
        const float cos_k =
            transform.skew == 0.0F ? cos_r : std::cos(transform.rotation + transform.skew);
        const float sin_k =
            transform.skew == 0.0F ? sin_r : std::sin(transform.rotation + transform.skew);
        const Vec4 tint{
            layer.modulate.x * from_parents.modulate.x, layer.modulate.y * from_parents.modulate.y,
            layer.modulate.z * from_parents.modulate.z, layer.modulate.w * from_parents.modulate.w};
        std::array<std::uint8_t, 4> rgba{};
        const auto corner = [&](float x, float y, float u, float v) {
            const float sx = x * transform.scale.x;
            const float sy = y * transform.scale.y;
            return render::Vertex2D{.x = transform.position.x + (sx * cos_r) - (sy * sin_k),
                                    .y = transform.position.y + (sx * sin_r) + (sy * cos_k),
                                    .u = u,
                                    .v = v,
                                    .rgba = rgba};
        };
        const render::Filter filter = resolve_filter(layer.texture_filter, options.texture_filter);
        // How each source's tiles are shaded: with its normal and specular maps, left out with a
        // warning if they don't load.
        std::map<const AtlasSource*, render::Material> source_materials;
        const auto material_of = [&](const AtlasSource& source) -> const render::Material& {
            auto [found, added] = source_materials.try_emplace(&source);
            if (!added) {
                return found->second;
            }
            render::Shading& shading = found->second.shading;
            shading = shading_for(render::LightMode::Normal, layer.light_mask, z, canvas);
            if (shading.mode == render::LightMode::Unshaded) {
                return found->second;
            }
            for (const auto& [path, slot] :
                 {std::pair{&source.normal_texture, &shading.normal_map},
                  std::pair{&source.specular_texture, &shading.specular_map}}) {
                if (path->empty()) {
                    continue;
                }
                if (auto map = this->texture(*path, filter); map) {
                    *slot = map.value()->id;
                } else {
                    warn_once(*path, map.error().message, "texture", kEvent);
                    problem(entity, "/components/TileMapLayer/tile_set",
                            *path + ": " + map.error().message);
                }
            }
            shading.specular = {source.specular_color.x, source.specular_color.y,
                                source.specular_color.z,
                                source.specular_color.w * source.specular_shininess};
            return found->second;
        };
        // Tiles go in runs with one texture and material; each run is one draw.
        constexpr std::size_t kNoRun = SIZE_MAX;
        std::size_t run = kNoRun;
        std::uint32_t part = 0;
        const std::size_t first_run = items.size();
        for (const auto* entry : cached.order) {
            const auto& [cell, tile] = *entry;
            const AtlasSource& source = set.value()->sources.find(tile.source)->second;
            auto texture = this->texture(source.texture, filter);
            if (!texture) {
                warn_once(source.texture, texture.error().message, "texture", kEvent);
                problem(entity, "/components/TileMapLayer/tile_set",
                        source.texture + ": " + texture.error().message);
                continue;
            }
            // An animated tile shows the frame for the time drawn, from the cell's own start
            // with random_start_times; every frame must be in the texture, whichever shows.
            const TileData* data = set.value()->find(tile);
            const int frames =
                data != nullptr ? static_cast<int>(data->animation_frames.size()) : 1;
            int frame = 0;
            if (frames > 1) {
                frame = tile_frame(*data, options.time,
                                   data->animation_mode == TileAnimationMode::RandomStartTimes
                                       ? tile_animation_offset(world.id_of(entity), cell)
                                       : 0.0);
            }
            const auto width = static_cast<int>(texture.value()->width);
            const auto height = static_cast<int>(texture.value()->height);
            std::string outside;
            for (int each = 0; each < frames && outside.empty(); ++each) {
                const auto [x, y, w, h] = tile_region(source, tile.atlas, each);
                if (x + w > width || y + h > height) {
                    outside = "the tile " + tile.source + ":" + std::to_string(tile.atlas.x) + "," +
                              std::to_string(tile.atlas.y) +
                              (frames > 1 ? "'s frame " + std::to_string(each) : "") + " at [" +
                              std::to_string(x) + ", " + std::to_string(y) + ", " +
                              std::to_string(w) + ", " + std::to_string(h) + "] goes outside the " +
                              std::to_string(width) + "x" + std::to_string(height) + " texture " +
                              source.texture;
                }
            }
            if (!outside.empty()) {
                warn_once(layer.tile_set, outside, "tile_set", kEvent);
                problem(entity, "/components/TileMapLayer/tile_set", std::move(outside));
                continue;
            }
            const auto [rx, ry, rw, rh] = tile_region(source, tile.atlas, frame);
            const render::Material& material = material_of(source);
            if (run == kNoRun || items[run].texture != texture.value()->id ||
                items[run].material != material) {
                items.push_back({.canvas = canvas,
                                 .z_index = z,
                                 .order = tree.draw_key(entity),
                                 .part = part++,
                                 .texture = texture.value()->id,
                                 .first = vertices.size(),
                                 .count = 0,
                                 .material = material,
                                 .entity = entity,
                                 .kind = FrameItem::Kind::Tiles,
                                 .image = &*texture.value()->image,
                                 .path = texture.value()->image.path(),
                                 .drawn = 0,
                                 .particles = render::ParticlesId::None,
                                 .look = {}});
                run = items.size() - 1;
            }
            // The tile's region on its cell, moved by its texture origin and turned as Godot's
            // draw_tile turns it: transposed (the texture's x and y swapped), then flipped.
            const TileLook look = tile_look(*set.value(), tile);
            rgba = {to_byte(tint.x * look.modulate.x), to_byte(tint.y * look.modulate.y),
                    to_byte(tint.z * look.modulate.z), to_byte(tint.w * look.modulate.w)};
            const Vec2 center = map_to_local(*set.value(), cell);
            const float x0 = center.x + look.position.x;
            const float y0 = center.y + look.position.y;
            const float x1 = x0 + look.size.x;
            const float y1 = y0 + look.size.y;
            float u0 = static_cast<float>(rx) / static_cast<float>(width);
            float v0 = static_cast<float>(ry) / static_cast<float>(height);
            float u1 = static_cast<float>(rx + rw) / static_cast<float>(width);
            float v1 = static_cast<float>(ry + rh) / static_cast<float>(height);
            // Flips swap the texture's coordinates along the drawn x (u, or v when transposed)
            // and the drawn y (v, or u).
            if ((look.flip_h && !look.transpose) || (look.flip_v && look.transpose)) {
                std::swap(u0, u1);
            }
            if ((look.flip_v && !look.transpose) || (look.flip_h && look.transpose)) {
                std::swap(v0, v1);
            }
            const auto uv = [&](bool right, bool bottom) {
                return look.transpose ? std::array<float, 2>{bottom ? u1 : u0, right ? v1 : v0}
                                      : std::array<float, 2>{right ? u1 : u0, bottom ? v1 : v0};
            };
            const auto at = [&](float x, float y, bool right, bool bottom) {
                const auto [u, v] = uv(right, bottom);
                return corner(x, y, u, v);
            };
            const render::Vertex2D top_left = at(x0, y0, false, false);
            const render::Vertex2D top_right = at(x1, y0, true, false);
            const render::Vertex2D bottom_left = at(x0, y1, false, true);
            const render::Vertex2D bottom_right = at(x1, y1, true, true);
            vertices.insert(vertices.end(), {top_left, top_right, bottom_left, top_right,
                                             bottom_right, bottom_left});
            items[run].count += 6;
            ++items[run].drawn;
            ++tiles;
        }
        // Under a repeating Parallax2D, each run again at each repeat. The runs follow each
        // other in vertices, so each one's are taken out and put back repeated in turn.
        if (repeat_source(entity) != entt::null && first_run < items.size()) {
            const std::size_t from = items[first_run].first;
            const std::vector<render::Vertex2D> all(
                vertices.begin() + static_cast<std::ptrdiff_t>(from), vertices.end());
            vertices.resize(from);
            for (std::size_t i = first_run; i < items.size(); ++i) {
                const std::size_t start = vertices.size();
                const auto begin = all.begin() + static_cast<std::ptrdiff_t>(items[i].first - from);
                vertices.insert(vertices.end(), begin,
                                begin + static_cast<std::ptrdiff_t>(items[i].count));
                repeat(entity, start);
                items[i].first = start;
                items[i].count = vertices.size() - start;
            }
        }
    }
    std::erase_if(layer_cells_, [](const auto& entry) { return !entry.second.seen; });

    std::ranges::sort(items, [](const Item& a, const Item& b) {
        if (a.canvas != b.canvas) {
            return a.canvas < b.canvas;
        }
        if (a.z_index != b.z_index) {
            return a.z_index < b.z_index;
        }
        return a.order != b.order ? a.order < b.order : a.part < b.part;
    });
    for (Item& item : items) {
        item.material.transform = matrices[item.canvas];
    }

    // Collision shapes, over the world's canvas. Those of entities in a CanvasLayer are drawn
    // where the layer shows them, through the camera's view undone.
    CollisionShapes shapes;
    if (options.collisions) {
        const world::Affine2D undo_camera = canvas_list[world_canvas].transform.inverse();
        shapes =
            collision_shapes(world, [&](entt::entity entity, const world::Transform2D& current) {
                world::Transform2D placed = transform_of(entity, current);
                const std::size_t canvas = canvases.of(entity);
                if (canvas == world_canvas) {
                    return placed;
                }
                return world::parts(undo_camera * canvas_list[canvas].transform *
                                    world::affine(placed));
            });
    }

    // Controls, in their canvases, then the overlay's.
    text::Fonts* fonts = ui::fonts(world);
    if (fonts == nullptr) {
        if (fonts_ == nullptr) {
            text::add_asset_types(*assets_);
            fonts_ = std::make_unique<text::Fonts>(*assets_);
        }
        fonts = fonts_.get();
    }
    ui::Layout layout = ui::layout(world, *fonts, viewport);
    const ui::DrawList controls = ui::draw_calls(layout, world, *fonts);
    ui::DrawList overlay;
    if (options.overlay != nullptr) {
        overlay = ui::draw_calls(ui::layout(*options.overlay, *fonts, viewport), *options.overlay,
                                 *fonts);
    }
    // The layout places a CanvasLayer's controls with the camera as it is; following the
    // viewport between ticks, they move to where this frame shows the layer.
    std::vector<std::optional<render::Mat4>> control_matrices(canvas_list.size() + 1);
    if (options.interpolation < 1.0 && layout.canvases.size() == canvas_list.size()) {
        for (std::size_t c = 0; c < canvas_list.size(); ++c) {
            if (c != world_canvas && canvas_list[c].transform != layout.canvases[c].transform) {
                control_matrices[c] =
                    matrix(canvas_list[c].transform * layout.canvases[c].transform.inverse());
            }
        }
    }
    // Their textures, made now so the null device checks them too; a call whose texture doesn't
    // load draws nothing (layout leaves out the boxes whose textures don't).
    const auto textures_of = [&](const ui::DrawList& list) {
        std::vector<std::optional<render::TextureId>> made(list.calls.size());
        for (std::size_t i = 0; i < list.calls.size(); ++i) {
            const ui::DrawCall& call = list.calls[i];
            if (call.source != ui::DrawCall::Source::Texture) {
                made[i] = render::TextureId::None;
                continue;
            }
            if (auto loaded =
                    texture(call.texture, resolve_filter(call.filter, options.texture_filter));
                loaded) {
                made[i] = loaded.value()->id;
            }
        }
        return made;
    };
    const std::vector<std::optional<render::TextureId>> control_textures = textures_of(controls);
    const std::vector<std::optional<render::TextureId>> overlay_textures = textures_of(overlay);

    if (contents != nullptr) {
        // Each item's draws together: a layer's runs, a control's boxes and text.
        const auto add = [&](FrameItem::Kind kind, entt::entity entity,
                             std::size_t canvas) -> FrameItem& {
            if (contents->items.empty() || contents->items.back().entity != entity ||
                contents->items.back().kind != kind) {
                contents->items.push_back({.kind = kind,
                                           .entity = entity,
                                           .count = 0,
                                           .canvas = canvas,
                                           .pieces = {},
                                           .uncut = {}});
            }
            return contents->items.back();
        };
        std::size_t order = 0; // pieces in drawing order
        // A control draws as one item, though what it draws over its children comes later; the
        // popup menu it opened is another, and its tooltip another.
        std::unordered_map<entt::entity, std::size_t> control_items;
        std::unordered_map<entt::entity, std::size_t> popup_items;
        std::unordered_map<entt::entity, std::size_t> tooltip_items;
        const auto control_item = [&](entt::entity entity, bool popup, bool tooltip,
                                      std::size_t canvas) -> FrameItem& {
            FrameItem::Kind kind = FrameItem::Kind::Control;
            auto* kept = &control_items;
            if (tooltip) {
                kind = FrameItem::Kind::Tooltip;
                kept = &tooltip_items;
            } else if (popup) {
                kind = FrameItem::Kind::Popup;
                kept = &popup_items;
            }
            const auto [found, made] = kept->try_emplace(entity, contents->items.size());
            if (made) {
                contents->items.push_back({.kind = kind,
                                           .entity = entity,
                                           .count = 0,
                                           .canvas = canvas,
                                           .pieces = {},
                                           .uncut = {}});
            }
            return contents->items[found->second];
        };
        // A piece of a control's: triangles over its call's texture or glyph page, if it has one.
        const auto control_piece = [&](const ui::DrawCall& call, auto first,
                                       auto last) -> std::optional<FramePiece> {
            FramePiece piece{.pixels = nullptr,
                             .width = 0,
                             .height = 0,
                             .field = call.field,
                             .vertices = {first, last},
                             .order = order,
                             .texture = {}};
            if (call.source == ui::DrawCall::Source::Atlas) {
                const text::AtlasPage& page = fonts->pages().at(call.page);
                piece.pixels = page.rgba.data();
                piece.width = page.width;
                piece.height = page.height;
            } else if (call.source == ui::DrawCall::Source::Texture) {
                auto made =
                    texture(call.texture, resolve_filter(call.filter, options.texture_filter));
                if (!made) {
                    return std::nullopt;
                }
                const render::Image& image = made.value()->pixels();
                piece.pixels = image.pixels.data();
                piece.width = image.width;
                piece.height = image.height;
                if (made.value()->builtin == nullptr) {
                    piece.texture = made.value()->image.path();
                }
            }
            return piece;
        };
        // In drawing order: each canvas's sprites, layers and particles, then its controls,
        // then what draws over every canvas.
        std::size_t next_item = 0;
        std::size_t next_span = 0;
        for (std::size_t c = 0; c <= canvas_list.size(); ++c) {
            for (; next_item < items.size() && items[next_item].canvas == c; ++next_item) {
                const Item& item = items[next_item];
                FrameItem& out = add(item.kind, item.entity, c);
                out.count += item.drawn;
                const auto from = vertices.begin() + static_cast<std::ptrdiff_t>(item.first);
                FramePiece& piece = out.pieces.emplace_back(FramePiece{
                    .pixels = item.image != nullptr ? item.image->pixels.data() : nullptr,
                    .width = item.image != nullptr ? item.image->width : 0,
                    .height = item.image != nullptr ? item.image->height : 0,
                    .field = {},
                    .vertices = {from, from + static_cast<std::ptrdiff_t>(item.count)},
                    .order = order++,
                    .texture = item.path});
                // In viewport pixels, as the camera or the canvas layer shows them.
                if (matrices[c]) {
                    for (render::Vertex2D& vertex : piece.vertices) {
                        const Vec2 placed = canvas_list[c].transform.apply({vertex.x, vertex.y});
                        vertex.x = placed.x;
                        vertex.y = placed.y;
                    }
                }
            }
            for (; next_span < controls.spans.size() &&
                   controls.calls[controls.spans[next_span].call].canvas <= c;
                 ++next_span) {
                const ui::DrawSpan& span = controls.spans[next_span];
                const ui::DrawCall& call = controls.calls[span.call];
                if (!control_textures[span.call]) {
                    continue;
                }
                const auto from = call.vertices.begin() + static_cast<std::ptrdiff_t>(span.first);
                if (auto piece =
                        control_piece(call, from, from + static_cast<std::ptrdiff_t>(span.count));
                    piece) {
                    ++order;
                    control_item(span.entity, span.popup, span.tooltip, call.canvas)
                        .pieces.push_back(std::move(*piece));
                }
            }
        }
        for (const ui::UncutDraw& uncut : controls.uncut) {
            if (auto piece = control_piece(uncut.call, uncut.call.vertices.begin(),
                                           uncut.call.vertices.end());
                piece) {
                control_item(uncut.entity, uncut.popup, false, uncut.call.canvas)
                    .uncut.push_back(std::move(*piece));
            }
        }
        contents->layout = std::move(layout);
        contents->camera = view;
        contents->canvases = canvas_list;
    }

    // The null device stands in for a window it doesn't have, as draw_empty_frame allows: the
    // sprites and tiles are checked and counted, and nothing is drawn.
    const auto shadows = static_cast<std::size_t>(std::ranges::count_if(
        lights.lights, [](const render::Light& l) { return l.shadow_row >= 0; }));
    if (options.target == render::TextureId::None && device_->backend() == render::Backend::Null) {
        if (auto ran = run_particle_jobs(particle_jobs); !ran) {
            return ran.error();
        }
        sprites_drawn_ = sprites;
        tiles_drawn_ = tiles;
        particles_drawn_ = particles;
        lights_drawn_ = lights.lights.size();
        shadows_drawn_ = shadows;
        controls_drawn_ = controls.controls;
        glyphs_drawn_ = controls.glyphs;
        boxes_drawn_ = controls.boxes;
        shapes_drawn_ = shapes.shapes;
        meshes_drawn_ = scene3d_.meshes();
        lights_3d_drawn_ = scene ? scene3d_.lights() : 0;
        return {};
    }
    return submit({.options = options,
                   .lighting = lights,
                   .scaling = settings(world).scaling,
                   .items = items,
                   .vertices = vertices,
                   .shapes = shapes,
                   .camera = camera,
                   .world_canvas = world_canvas,
                   .canvases = canvas_list.size(),
                   .controls = controls,
                   .overlay = overlay,
                   .control_textures = control_textures,
                   .overlay_textures = overlay_textures,
                   .control_matrices = control_matrices,
                   .particle_jobs = particle_jobs,
                   .fonts = *fonts,
                   .scene = scene,
                   .sprites = sprites,
                   .tiles = tiles,
                   .particles = particles,
                   .shadows = shadows});
}

Result<void> CanvasRenderer::submit(const Submission& submission) {
    const CanvasOptions& options = submission.options;
    const render::Lighting& lights = submission.lighting;
    const render::Scaling& scaling = submission.scaling;
    const std::vector<Item>& items = submission.items;
    const std::vector<render::Vertex2D>& vertices = submission.vertices;
    const CollisionShapes& shapes = submission.shapes;
    const std::optional<render::Mat4>& camera = submission.camera;
    const std::size_t world_canvas = submission.world_canvas;
    const ui::DrawList& controls = submission.controls;
    const ui::DrawList& overlay = submission.overlay;
    const std::span<const ParticleJob> particle_jobs = submission.particle_jobs;
    // The render scale (render/upscaler.hpp): the world, with the canvases below it, draws into
    // the upscaler's input at the render size, which is scaled over the screen before the
    // world's controls and the canvases above draw at full size.
    const std::uint32_t output_width =
        options.pixel_width > 0 ? options.pixel_width : static_cast<std::uint32_t>(options.width);
    const std::uint32_t output_height = options.pixel_height > 0
                                            ? options.pixel_height
                                            : static_cast<std::uint32_t>(options.height);
    std::string chosen;
    if (output_width > 0 && output_height > 0) {
        chosen = render::choose_upscaler(scaling, *device_);
    }
    if (!scaling.upscaler.empty() && chosen != scaling.upscaler &&
        warned_.insert("upscaler " + scaling.upscaler).second) {
        log::warn(
            "the upscaler isn't available here, so the scaling mode is used",
            {{"upscaler", scaling.upscaler}, {"mode", chosen.empty() ? "none" : chosen.c_str()}});
    }
    if (chosen != upscaler_name_) {
        if (upscaler_ != nullptr) {
            upscaler_->release(*device_);
            upscaler_.reset();
        }
        upscaler_name_ = chosen;
        if (const std::optional<render::UpscalerInfo> info = render::find_upscaler(chosen); info) {
            upscaler_ = info->make();
        }
    }
    scaled_ = {};
    const float view_width =
        options.width > 0.0F ? options.width : static_cast<float>(output_width);
    const float view_height =
        options.height > 0.0F ? options.height : static_cast<float>(output_height);

    render::Frame frame;
    frame.target = options.target;
    frame.clear = options.clear;
    frame.lighting = lights;
    // The 3D world draws under the world's canvas, in whichever frame draws that.
    const auto with_scene = [&](render::Frame& world_frame) {
        if (submission.scene) {
            world_frame.scene = submission.scene->scene;
            world_frame.clear = submission.scene->clear;
        }
    };
    const auto draw_scene = [&]() -> Result<void> {
        return submission.scene ? scene3d_.draw() : Result<void>{};
    };
    if (options.width > 0.0F && options.height > 0.0F) {
        frame.view = render::pixel_projection(options.width, options.height);
    }
    const auto draw_control = [&](const ui::DrawCall& call,
                                  const std::optional<render::TextureId>& made,
                                  const std::optional<render::Mat4>& transform) -> Result<void> {
        if (!made) {
            return {};
        }
        render::TextureId texture = *made;
        if (call.source == ui::DrawCall::Source::Atlas) {
            auto page = atlas_page(submission.fonts, call.page);
            if (!page) {
                return page.error();
            }
            texture = page.value();
        }
        render::Material material;
        material.field = call.field;
        material.transform = transform;
        return device_->draw(texture, call.vertices, material);
    };
    // Each canvas in turn: its sprites, layers and particles, the collision shapes over the
    // world's, then its controls; then what draws over every canvas, then the overlay.
    std::size_t next_item = 0;
    std::size_t next_call = 0;
    const auto draw_items = [&](std::size_t c) -> Result<void> {
        for (; next_item < items.size() && items[next_item].canvas == c; ++next_item) {
            const Item& item = items[next_item];
            if (item.particles != render::ParticlesId::None) {
                if (auto drawn = device_->draw_particles(item.particles, item.texture, item.look,
                                                         item.material);
                    !drawn) {
                    return drawn.error();
                }
                continue;
            }
            const std::span<const render::Vertex2D> span(vertices.data() + item.first, item.count);
            if (auto drawn = device_->draw(item.texture, span, item.material); !drawn) {
                return drawn.error();
            }
        }
        if (c == world_canvas && !shapes.vertices.empty()) {
            render::Material material;
            material.transform = camera;
            return device_->draw(render::TextureId::None, shapes.vertices, material);
        }
        return {};
    };
    const auto draw_controls = [&](std::size_t c) -> Result<void> {
        for (; next_call < controls.calls.size() && controls.calls[next_call].canvas <= c;
             ++next_call) {
            if (auto drawn =
                    draw_control(controls.calls[next_call], submission.control_textures[next_call],
                                 submission.control_matrices[c]);
                !drawn) {
                return drawn;
            }
        }
        return {};
    };
    const auto end_after = [&](const Error& error) -> Result<void> {
        (void)device_->end_frame();
        return error;
    };
    // A frame that didn't begin before the particle jobs ran: they run again next frame.
    const auto not_begun = [&](const Error& error) -> Result<void> {
        for (auto& [entity, cached] : gpu_systems_) {
            cached.serial = 0;
        }
        return error;
    };

    std::size_t first_canvas = 0;
    std::optional<render::UpscaleDraw> upscaled;
    if (upscaler_ != nullptr) {
        const render::UpscaleSize size{
            .render_width = render::render_size(output_width, scaling.scale),
            .render_height = render::render_size(output_height, scaling.scale),
            .output_width = output_width,
            .output_height = output_height};
        auto input = upscaler_->input(*device_, size);
        if (!input) {
            return not_begun(input.error());
        }
        render::Frame world_frame = frame;
        world_frame.target = input.value();
        world_frame.view = render::pixel_projection(view_width, view_height);
        with_scene(world_frame);
        if (auto began = device_->begin_frame(world_frame); !began) {
            return not_begun(began.error());
        }
        if (auto drawn = draw_scene(); !drawn) {
            return end_after(drawn.error());
        }
        if (auto ran = run_particle_jobs(particle_jobs); !ran) {
            return end_after(ran.error());
        }
        for (std::size_t c = 0; c <= world_canvas; ++c) {
            if (auto drawn = draw_items(c); !drawn) {
                return end_after(drawn.error());
            }
            if (c == world_canvas) {
                break;
            }
            if (auto drawn = draw_controls(c); !drawn) {
                return end_after(drawn.error());
            }
        }
        if (auto ended = device_->end_frame(); !ended) {
            return ended;
        }
        auto made = upscaler_->upscale(*device_, size, scaling);
        if (!made) {
            return made.error();
        }
        upscaled = std::move(made).value();
        frame.view = render::pixel_projection(view_width, view_height);
        first_canvas = world_canvas;
        scaled_ = {.upscaler = upscaler_name_, .size = size};
    }
    if (!upscaled) {
        with_scene(frame);
    }
    auto began = device_->begin_frame(frame);
    if (!began) {
        return upscaled ? Result<void>(began.error()) : not_begun(began.error());
    }
    if (!upscaled) {
        if (auto drawn = draw_scene(); !drawn) {
            return end_after(drawn.error());
        }
    }
    if (upscaled) {
        const std::array<render::Vertex2D, 6> screen{
            render::Vertex2D{.x = 0.0F, .y = 0.0F, .u = 0.0F, .v = 0.0F},
            render::Vertex2D{.x = view_width, .y = 0.0F, .u = 1.0F, .v = 0.0F},
            render::Vertex2D{.x = view_width, .y = view_height, .u = 1.0F, .v = 1.0F},
            render::Vertex2D{.x = 0.0F, .y = 0.0F, .u = 0.0F, .v = 0.0F},
            render::Vertex2D{.x = view_width, .y = view_height, .u = 1.0F, .v = 1.0F},
            render::Vertex2D{.x = 0.0F, .y = view_height, .u = 0.0F, .v = 1.0F}};
        if (auto drawn = device_->draw(upscaled->texture, screen, upscaled->material); !drawn) {
            return end_after(drawn.error());
        }
        if (auto drawn = draw_controls(world_canvas); !drawn) {
            return end_after(drawn.error());
        }
        first_canvas = world_canvas + 1;
    } else if (auto ran = run_particle_jobs(particle_jobs); !ran) {
        return end_after(ran.error());
    }
    for (std::size_t c = first_canvas; c <= submission.canvases; ++c) {
        if (auto drawn = draw_items(c); !drawn) {
            return end_after(drawn.error());
        }
        if (auto drawn = draw_controls(c); !drawn) {
            return end_after(drawn.error());
        }
    }
    for (std::size_t i = 0; i < overlay.calls.size(); ++i) {
        if (auto drawn =
                draw_control(overlay.calls[i], submission.overlay_textures[i], std::nullopt);
            !drawn) {
            return end_after(drawn.error());
        }
    }
    controls_drawn_ = began.value() ? controls.controls : 0;
    glyphs_drawn_ = began.value() ? controls.glyphs : 0;
    boxes_drawn_ = began.value() ? controls.boxes : 0;
    sprites_drawn_ = began.value() ? submission.sprites : 0;
    tiles_drawn_ = began.value() ? submission.tiles : 0;
    particles_drawn_ = began.value() ? submission.particles : 0;
    lights_drawn_ = began.value() ? lights.lights.size() : 0;
    shadows_drawn_ = began.value() ? submission.shadows : 0;
    shapes_drawn_ = began.value() ? shapes.shapes : 0;
    meshes_drawn_ = began.value() ? scene3d_.meshes() : 0;
    lights_3d_drawn_ = began.value() && submission.scene ? scene3d_.lights() : 0;
    return device_->end_frame();
}

} // namespace slash::canvas
