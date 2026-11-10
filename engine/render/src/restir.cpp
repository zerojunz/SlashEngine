#include "restir.hpp"

#include "slash/core/bits.hpp"
#include "slash/core/jobs.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace slash::render::detail {
namespace {

// How alike a neighbor's or last frame's surface must be to share its reservoirs: normals at
// least this close to the same way, and places within this share of the view depth.
constexpr float kAlikeNormals = 0.9F;
constexpr float kAlikeDepth = 0.1F;
// Shadow rays stop this share short of the light, as run_ray_shadows's.
constexpr float kShortOfLight = 1e-4F;

Float3 sub(const Float3& a, const Float3& b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
Float3 scale(const Float3& a, float s) {
    return {a[0] * s, a[1] * s, a[2] * s};
}
float dot(const Float3& a, const Float3& b) {
    return (a[0] * b[0]) + (a[1] * b[1]) + (a[2] * b[2]);
}
float length(const Float3& a) {
    return std::sqrt(dot(a, a));
}

// A 32-bit integer hash with good avalanche (Chris Wellons' lowbias32), as the path tracer's.
std::uint32_t mix(std::uint32_t x) {
    x ^= x >> 16U;
    x *= 0x7FEB352DU;
    x ^= x >> 15U;
    x *= 0x846CA68BU;
    x ^= x >> 16U;
    return x;
}

// From 0 up to but not including 1, in steps of 2^-24.
float unit(std::uint32_t bits) {
    return static_cast<float>(bits >> 8U) * (1.0F / 16777216.0F);
}

// The steps a pixel's random numbers are for.
enum class Step : std::uint8_t { Candidate, Keep, History, Neighbor, Place, Shadow };

std::uint32_t seed_of(std::size_t at, std::uint32_t frame, std::uint32_t sample, Step step,
                      std::uint32_t k) {
    return mix(static_cast<std::uint32_t>(at) ^
               mix((frame * 0x9E3779B9U) ^
                   mix((sample << 24U) ^ (static_cast<std::uint32_t>(step) << 16U) ^ k)));
}

float luminance(const MeshLightUniforms& light) {
    return (0.2126F * light.color[0]) + (0.7152F * light.color[1]) + (0.0722F * light.color[2]);
}

// The light an omni or spot light brings to a surface at world facing normal, with no shadow:
// what its reservoirs resample against.
float target_of(const MeshLightUniforms& light, const Float3& world, const Float3& normal) {
    const Float3 to = sub({light.position[0], light.position[1], light.position[2]}, world);
    const float distance = length(to);
    if (!(distance > 0.0F)) {
        return 0.0F;
    }
    const float facing = dot(normal, scale(to, 1.0F / distance));
    if (!(facing > 0.0F)) {
        return 0.0F;
    }
    return luminance(light) * light_falloff(light, world) * facing;
}

// A pixel's surface, as its rays leave it, and its view depth.
struct Surface {
    bool valid = false;
    Float3 origin{};
    Float3 normal{};
    float depth = 0.0F;
};

// Adds a candidate or another reservoir to r with weight, keeping its light by chance.
void take(Reservoir& r, std::int32_t light, float weight, float count, float random) {
    r.weight_sum += weight;
    r.count += count;
    if (weight > 0.0F && random * r.weight_sum < weight) {
        r.light = light;
    }
}

} // namespace

std::optional<ManyLights> many_lights(const Scene3D& scene) {
    if (scene.method != RenderMethod::ForwardPlus || !scene.ray_tracing.many_lights) {
        return std::nullopt;
    }
    return ManyLights{
        .candidates = std::clamp<std::uint32_t>(scene.ray_tracing.many_light_candidates, 1,
                                                kMaxManyLightCandidates),
        .samples = std::clamp<std::uint32_t>(scene.ray_tracing.many_light_samples, 1,
                                             kMaxManyLightSamples),
    };
}

std::uint64_t restir_lights_key(const Scene3D& scene) {
    std::uint64_t key = 0xCBF29CE484222325ULL;
    const auto add = [&](std::uint32_t value) {
        key ^= value;
        key *= 0x100000001B3ULL;
    };
    const auto add_float = [&](float value) { add(slash::bit_cast<std::uint32_t>(value)); };
    for (const Light3D& light : scene.lights) {
        if (light.type == LightType::Directional) {
            continue;
        }
        add(static_cast<std::uint32_t>(light.type));
        for (const float value : light.position) {
            add_float(value);
        }
        for (const float value : light.color) {
            add_float(value);
        }
        add_float(light.energy);
        add_float(light.range);
    }
    return key;
}

std::vector<float> run_restir(RestirState& state, const ManyLights& settings,
                              const Scene3D& lights_scene, const LightClusters& clusters,
                              const Mat4& view_projection, const PathScene& scene,
                              const PathTextures* textures, const RayShadowFrame& frame,
                              std::span<const float> depth, std::span<const float> normals,
                              jobs::Pool* pool) {
    const std::uint32_t width = frame.width;
    const std::uint32_t height = frame.height;
    const std::size_t pixels = std::size_t{width} * height;
    const std::uint32_t samples = settings.samples;
    const auto candidates = static_cast<float>(settings.candidates);

    // Each scene light's place in clusters.lights, which holds the omni and spot lights.
    std::vector<std::int32_t> cluster_of(lights_scene.lights.size(), -1);
    for (std::size_t i = 0; i < clusters.sources.size(); ++i) {
        cluster_of[clusters.sources[i]] = static_cast<std::int32_t>(i);
    }
    const auto light_at = [&](std::int32_t light) -> const MeshLightUniforms* {
        if (light < 0 || static_cast<std::size_t>(light) >= cluster_of.size() ||
            cluster_of[static_cast<std::size_t>(light)] < 0) {
            return nullptr;
        }
        return &clusters
                    .lights[static_cast<std::size_t>(cluster_of[static_cast<std::size_t>(light)])];
    };
    const auto target = [&](std::int32_t light, const Surface& surface) {
        const MeshLightUniforms* found = light_at(light);
        return found != nullptr ? target_of(*found, surface.origin, surface.normal) : 0.0F;
    };
    const auto finish = [&](Reservoir& r, const Surface& surface) {
        const float aim = target(r.light, surface);
        r.weight = aim > 0.0F && r.count > 0.0F ? r.weight_sum / (r.count * aim) : 0.0F;
    };
    // Whether nothing stops a ray from the surface to the light, through its shadow's opacity.
    const auto visibility = [&](std::int32_t light, const Surface& surface, std::size_t at,
                                std::uint32_t sample) {
        const MeshLightUniforms* found = light_at(light);
        if (found == nullptr) {
            return 0.0F;
        }
        const Light3D& source = lights_scene.lights[static_cast<std::size_t>(light)];
        if (!source.shadow) {
            return 1.0F;
        }
        const Float3 to =
            sub({found->position[0], found->position[1], found->position[2]}, surface.origin);
        const float distance = length(to);
        if (!(distance > 0.0F)) {
            return 1.0F;
        }
        const bool blocked = path_blocked(
            scene, textures, surface.origin, scale(to, 1.0F / distance),
            distance * (1.0F - kShortOfLight), source.shadow_caster_mask,
            seed_of(at, state.frame, sample, Step::Shadow, static_cast<std::uint32_t>(light)));
        return blocked ? 1.0F - std::clamp(source.shadow_opacity, 0.0F, 1.0F) : 1.0F;
    };
    const auto each_row = [&](const auto& row) {
        if (pool != nullptr) {
            pool->parallel_for(height, row);
        } else {
            for (std::size_t y = 0; y < height; ++y) {
                row(y);
            }
        }
    };

    // Each pixel's surface, from the prepass.
    std::vector<Surface> surfaces(pixels);
    each_row([&](std::size_t y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t at = (y * width) + x;
            const std::optional<RaySurface> surface =
                ray_surface(frame, x, static_cast<std::uint32_t>(y), depth[at],
                            {normals[at * 4], normals[(at * 4) + 1], normals[(at * 4) + 2]});
            if (!surface || dot(surface->normal, surface->normal) == 0.0F) {
                continue;
            }
            const Float4& z = frame.view_rows[2];
            surfaces[at] = {.valid = true,
                            .origin = surface->origin,
                            .normal = surface->normal,
                            .depth = -(dot({z[0], z[1], z[2]}, surface->origin) + z[3])};
        }
    });
    const auto alike = [](const Surface& a, const Float3& origin, const Float3& normal) {
        return dot(a.normal, normal) >= kAlikeNormals &&
               length(sub(a.origin, origin)) <= kAlikeDepth * std::max(a.depth, 1.0F);
    };

    const bool history = state.width == width && state.height == height &&
                         state.samples == samples &&
                         state.lights_key == restir_lights_key(lights_scene) && state.frame > 0;
    const float most = kRestirHistory * candidates;
    // Candidates from each pixel's cluster, kept by resampling, then last frame's reservoir where
    // its surface was.
    std::vector<Reservoir> fresh(pixels * samples);
    each_row([&](std::size_t y) {
        std::vector<std::int32_t> reaching;
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t at = (y * width) + x;
            const Surface& surface = surfaces[at];
            if (!surface.valid) {
                continue;
            }
            // The lights the pixel's cluster holds, as add_cluster_lights finds them.
            reaching.clear();
            const std::uint32_t tile_x = std::min(x / kClusterSize, clusters.tiles_across - 1);
            const std::uint32_t tile_y =
                std::min(static_cast<std::uint32_t>(y) / kClusterSize, clusters.tiles_down - 1);
            const std::size_t cell =
                ((std::size_t{tile_y} * clusters.tiles_across) + tile_x) * clusters.stride();
            const auto slice = static_cast<std::uint32_t>(
                std::clamp(surface.depth * clusters.slices_per_depth, 0.0F,
                           static_cast<float>(kClusterSlices - 1)));
            const std::uint32_t range = clusters.cells[cell + clusters.words + slice];
            const std::uint32_t first = range & 0xFFFFU;
            const std::uint32_t end = range >> 16U;
            for (std::uint32_t word = first >> 5U; word < (end + 31U) >> 5U; ++word) {
                std::uint32_t mask = clusters.cells[cell + word] & range_mask(word, first, end);
                while (mask != 0) {
                    const auto bit = static_cast<std::uint32_t>(std::countr_zero(mask));
                    mask &= mask - 1U;
                    reaching.push_back(
                        static_cast<std::int32_t>(clusters.sources[(word * 32U) + bit]));
                }
            }
            // Where the surface was last frame.
            std::optional<std::size_t> before;
            if (history) {
                const Mat4& m = state.view_projection;
                const Float3& p = surface.origin;
                std::array<float, 4> clip{};
                for (std::size_t row = 0; row < 4; ++row) {
                    clip.at(row) = (m.at(row) * p[0]) + (m.at(4 + row) * p[1]) +
                                   (m.at(8 + row) * p[2]) + m.at(12 + row);
                }
                if (clip[3] > 0.0F) {
                    const float px =
                        ((clip[0] / clip[3]) + 1.0F) * 0.5F * static_cast<float>(width);
                    const float py =
                        (1.0F - (clip[1] / clip[3])) * 0.5F * static_cast<float>(height);
                    if (px >= 0.0F && py >= 0.0F && px < static_cast<float>(width) &&
                        py < static_cast<float>(height)) {
                        const std::size_t then =
                            (static_cast<std::size_t>(py) * width) + static_cast<std::size_t>(px);
                        const Surface last{.valid = true,
                                           .origin = state.positions[then],
                                           .normal = state.normals[then],
                                           .depth = surface.depth};
                        if (alike(last, surface.origin, surface.normal)) {
                            before = then;
                        }
                    }
                }
            }
            for (std::uint32_t s = 0; s < samples; ++s) {
                Reservoir r;
                if (!reaching.empty()) {
                    const auto count = static_cast<float>(reaching.size());
                    for (std::uint32_t c = 0; c < settings.candidates; ++c) {
                        const std::uint32_t pick = seed_of(at, state.frame, s, Step::Candidate, c);
                        const std::int32_t light = reaching[std::min(
                            static_cast<std::size_t>(unit(pick) * count), reaching.size() - 1)];
                        take(r, light, target(light, surface) * count, 1.0F,
                             unit(seed_of(at, state.frame, s, Step::Keep, c)));
                    }
                    finish(r, surface);
                    // Visibility reuse: a light the pixel can't see isn't passed on.
                    if (r.weight > 0.0F) {
                        r.weight *= visibility(r.light, surface, at, s);
                    }
                }
                if (before) {
                    Reservoir last = state.reservoirs[(*before * samples) + s];
                    last.count = std::min(last.count, most);
                    Reservoir merged;
                    take(merged, r.light, target(r.light, surface) * r.weight * r.count, r.count,
                         0.0F);
                    take(merged, last.light, target(last.light, surface) * last.weight * last.count,
                         last.count, unit(seed_of(at, state.frame, s, Step::History, 0)));
                    finish(merged, surface);
                    r = merged;
                }
                fresh[(at * samples) + s] = r;
            }
        }
    });

    // Then a few neighbors' on the same surface, and one shadow ray to the light kept.
    std::vector<Reservoir> kept(pixels * samples);
    std::vector<float> out(pixels * samples * 2, 0.0F);
    for (std::size_t at = 0; at < pixels; ++at) {
        for (std::uint32_t s = 0; s < samples; ++s) {
            out[((at * samples) + s) * 2] = -1.0F;
        }
    }
    each_row([&](std::size_t y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t at = (y * width) + x;
            const Surface& surface = surfaces[at];
            if (!surface.valid) {
                continue;
            }
            for (std::uint32_t s = 0; s < samples; ++s) {
                const Reservoir& own = fresh[(at * samples) + s];
                Reservoir r;
                take(r, own.light, target(own.light, surface) * own.weight * own.count, own.count,
                     0.0F);
                for (std::uint32_t k = 0; k < kRestirNeighbors; ++k) {
                    const std::uint32_t place = seed_of(at, state.frame, s, Step::Place, k);
                    const auto dx = static_cast<std::int64_t>(
                        std::floor(((unit(place) * 2.0F) - 1.0F) * kRestirRadius));
                    const auto dy = static_cast<std::int64_t>(
                        std::floor(((unit(mix(place)) * 2.0F) - 1.0F) * kRestirRadius));
                    const std::int64_t nx = std::int64_t{x} + dx;
                    const std::int64_t ny = static_cast<std::int64_t>(y) + dy;
                    if ((dx == 0 && dy == 0) || nx < 0 || ny < 0 || nx >= width || ny >= height) {
                        continue;
                    }
                    const auto near = static_cast<std::size_t>((ny * std::int64_t{width}) + nx);
                    const Surface& other = surfaces[near];
                    if (!other.valid || !alike(other, surface.origin, surface.normal)) {
                        continue;
                    }
                    const Reservoir& q = fresh[(near * samples) + s];
                    take(r, q.light, target(q.light, surface) * q.weight * q.count, q.count,
                         unit(seed_of(at, state.frame, s, Step::Neighbor, k)));
                }
                finish(r, surface);
                if (r.weight > 0.0F) {
                    r.weight *= visibility(r.light, surface, at, s);
                }
                kept[(at * samples) + s] = r;
                const MeshLightUniforms* found = light_at(r.light);
                if (found != nullptr && r.weight > 0.0F) {
                    out[((at * samples) + s) * 2] =
                        static_cast<float>(cluster_of[static_cast<std::size_t>(r.light)]);
                    out[(((at * samples) + s) * 2) + 1] = r.weight;
                }
            }
        }
    });

    state.width = width;
    state.height = height;
    state.samples = samples;
    state.lights_key = restir_lights_key(lights_scene);
    state.view_projection = view_projection;
    state.reservoirs = std::move(kept);
    state.positions.assign(pixels, Float3{});
    state.normals.assign(pixels, Float3{});
    for (std::size_t at = 0; at < pixels; ++at) {
        if (surfaces[at].valid) {
            state.positions[at] = surfaces[at].origin;
            state.normals[at] = surfaces[at].normal;
        }
    }
    ++state.frame;
    return out;
}

} // namespace slash::render::detail
