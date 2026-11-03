#include "../engine/render/src/lightmap_light_msl.hpp"
#include "../engine/render/src/lightmap_light_shaders.hpp"
#include "../engine/render/src/ray_light_msl.hpp"
#include "../engine/render/src/ray_light_shaders.hpp"
#include "../engine/render/src/ray_records.hpp"
#include "../engine/render/src/ray_shadows_msl.hpp"
#include "../engine/render/src/ray_shadows_shaders.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

using namespace slash::render;
using namespace slash::render::detail;

namespace {

std::uint64_t fnv1a(std::string_view text) {
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (const char c : text) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

// A texture handle that is never dereferenced.
SDL_GPUTexture* fake_texture(std::uintptr_t n) {
    return reinterpret_cast<SDL_GPUTexture*>(n * 16); // NOLINT(performance-no-int-to-ptr)
}

} // namespace

TEST_CASE("the ray shaders' MSL was made from the GLSL their SPIR-V was") {
    // slash-ray-msl writes the hash of the GLSL it compiled; the SPIR-V headers hold the GLSL,
    // which shaders_up_to_date_* hold to the shaders.
    CHECK(fnv1a(shaders::kRayShadowsGlsl) == shaders::kRayShadowsMslFrom);
    CHECK(fnv1a(shaders::kRayLightGlsl) == shaders::kRayLightMslFrom);
    CHECK(fnv1a(shaders::kLightmapLightGlsl) == shaders::kLightmapLightMslFrom);
    // Metal samples the prepass's depth as a depth texture.
    CHECK(std::string_view(shaders::kRayShadowsMsl).find("depth2d<float> depth_texture") !=
          std::string_view::npos);
}

TEST_CASE("the ray shaders' argument buffers place each binding where Metal's side sets it") {
    // The light's shader's textures take kRayTextures places, then their samplers as many.
    const auto& light = shaders::kRayLightMslIds;
    REQUIRE(std::size(light) == 13);
    CHECK(light[9][0] - light[8][0] == static_cast<int>(2 * kRayTextures));
    CHECK(light[8][1] - light[8][0] == static_cast<int>(kRayTextures));
    for (const std::size_t sampled : {1U, 2U, 7U, 8U, 12U}) {
        CAPTURE(sampled);
        CHECK(light[sampled][1] >= 0);
    }
    const auto& shadows = shaders::kRayShadowsMslIds;
    REQUIRE(std::size(shadows) == 8);
    CHECK(shadows[1][1] >= 0);
    CHECK(shadows[2][1] >= 0);
    CHECK(shadows[7][1] == -1);
    // The bake's structure and its fourteen buffers.
    CHECK(std::size(shaders::kLightmapLightMslIds) == 15);
}

TEST_CASE("rays meet a draw by how it casts shadows and whether the frame is lit") {
    RayDraw draw;
    CHECK(ray_mask(draw, false) == kRayCastMask);
    CHECK(ray_mask(draw, true) == (kRayCastMask | kRaySeeMask));
    draw.shadows = ShadowCasting::ShadowsOnly;
    CHECK(ray_mask(draw, true) == kRayCastMask);
    draw.shadows = ShadowCasting::Off;
    CHECK(ray_mask(draw, false) == 0);
    CHECK(ray_mask(draw, true) == kRaySeeMask);
}

TEST_CASE("ray shadows' uniforms say when every draw casts for every light") {
    std::array<RayShadowLight, 2> lights{};
    lights[0].directional = true;
    lights[0].toward = {0.0F, 1.0F, 0.0F};
    lights[0].size = 0.1F;
    lights[1].position = {1.0F, 2.0F, 3.0F};
    lights[1].slot = 4;
    RayWork work;
    work.lights = lights;
    RayShadowFrame frame;
    frame.width = 64;
    frame.height = 32;
    frame.samples = 2;
    frame.to_world.at(5) = 2.0;
    RayShadowsUniforms uniforms = ray_shadows_uniforms(work, frame, true);
    CHECK(uniforms.size == std::array<std::uint32_t, 4>{64, 32, 2, 2});
    CHECK(uniforms.to_world.at(5) == 2.0F);
    CHECK(uniforms.options[0] == 1);
    CHECK(uniforms.options[1] == 0);
    CHECK(uniforms.lights[0].place == std::array<float, 4>{0.0F, 1.0F, 0.0F, 0.1F});
    CHECK(uniforms.lights[0].params[1] == 1.0F);
    CHECK(uniforms.lights[1].place == std::array<float, 4>{1.0F, 2.0F, 3.0F, 0.0F});
    CHECK(uniforms.lights[1].params[2] == 4.0F);
    CHECK(ray_shadows_uniforms(work, frame, false).options[0] == 0);
    lights[1].caster_mask = 1;
    work.occlusion = RayOcclusion{.samples = 8, .radius = 2.0F, .intensity = 3.0F, .power = 1.0F};
    uniforms = ray_shadows_uniforms(work, frame, true);
    CHECK(uniforms.options[0] == 0);
    CHECK(uniforms.options[1] == 1);
    CHECK(uniforms.occlusion == std::array<float, 4>{2.0F, 1.0F, 8.0F, 3.0F});
}

TEST_CASE("a frame's textures take a place each, with the sampler their draws sample them by") {
    RayTextureTable table;
    CHECK(table.index({}) == kRayNoTexture);
    CHECK(table.index({.texture = fake_texture(1)}) == 0);
    CHECK(table.index({.texture = fake_texture(2), .repeat = true}) == 1);
    CHECK(table.index({.texture = fake_texture(1)}) == 0);
    CHECK(table.index({.texture = fake_texture(1), .nearest = true}) == 2);
    REQUIRE(table.used().size() == 3);
    CHECK(table.used()[1].second == 2);
    CHECK(table.used()[2].second == 1);
    for (std::uintptr_t i = 3; table.used().size() < kRayTextures; ++i) {
        const std::uint32_t index = table.index({.texture = fake_texture(i)});
        CHECK(index == table.used().size() - 1);
    }
    CHECK(table.index({.texture = fake_texture(1000)}) == kRayNoTexture);

    RayDraw draw;
    draw.layers = 5;
    draw.cull = CullMode::Front;
    draw.textures[4] = {.texture = fake_texture(1)};
    const RayDrawRecord record = ray_draw_record(draw, 0x1234567890ULL, 0xABCDEF012ULL, table);
    CHECK(record.buffers == std::array<std::uint32_t, 4>{0x34567890U, 0x12U, 0xBCDEF012U, 0xAU});
    CHECK(record.options[2] == 5);
    CHECK(record.textures[0] == kRayNoTexture);
    CHECK(record.more[0] == 0);
    CHECK(record.more[1] == 1);
}

TEST_CASE("a lightmap's dispatches go a region of a slice, then a pass of rays, at a time") {
    std::vector<std::array<float, 4>> texels(std::size_t{600} * 500 * 2);
    std::vector<std::array<float, 4>> sky(4);
    std::vector<std::array<float, 3>> probes(5);
    LightmapTrace trace;
    trace.vertices.resize(3);
    trace.indices = {0, 1, 2};
    trace.triangles.resize(1);
    trace.width = 600;
    trace.height = 500;
    trace.slices = 2;
    trace.albedo = texels;
    trace.emission = texels;
    trace.position = texels;
    trace.normal = texels;
    trace.environment_width = 2;
    trace.environment_height = 2;
    trace.environment = sky;
    trace.rays = 70;
    trace.bounces = 1;
    trace.probe_points = probes;
    trace.probe_rays = 100;
    CHECK(lightmap_trace_fits(trace));

    std::array<std::size_t, kBakePasses> runs{};
    std::uint32_t direct_texels = 0;
    std::uint32_t last_rays = 0;
    REQUIRE(run_lightmap_passes(trace, 0,
                                [&](std::size_t pass, std::uint32_t count,
                                    const LightmapParams& params) -> slash::Result<void> {
                                    ++runs.at(pass);
                                    if (pass == 0) {
                                        direct_texels += count;
                                        CHECK(params.flags[2] - params.flags[1] == count);
                                    }
                                    if (pass == 2) {
                                        CHECK(count == 5);
                                        last_rays = params.step[2];
                                    }
                                    return {};
                                }));
    // 300000 texels a slice in regions of 512 x 512; rays 32 a pass of bounces and 64 a pass of
    // the probes'.
    CHECK(runs[0] == 4);
    CHECK(direct_texels == 600000);
    CHECK(runs[1] == 12);
    CHECK(runs[2] == 2);
    CHECK(last_rays == 100);

    trace.normal = std::span<const std::array<float, 4>>(texels).first(10);
    CHECK_FALSE(lightmap_trace_fits(trace));
}
