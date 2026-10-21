// The software device: draws frames on the CPU into render-target textures, as the GPU device's
// shaders would, with their CPU twins (run_program, shade, distance_field_coverage and
// sample_image). Triangles are rasterized as GPUs do: vertices snapped to 1/256 of a pixel,
// pixels covered at their centers by the top-left rule, and attributes interpolated linearly, so
// edges fall on the same pixels as on a GPU.

#include "checks.hpp"
#include "cpu_particles.hpp"

#include "slash/render/device.hpp"
#include "slash/render/image.hpp"
#include "slash/render/light.hpp"
#include "slash/render/material.hpp"
#include "slash/render/shader.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace slash::render {

namespace {

using detail::check_pixels;
using detail::check_texture_desc;
using detail::check_vertices;

// Vertex positions snap to this many steps per pixel, as GPUs' 8 bits of subpixel precision.
constexpr std::int64_t kSubpixel = 256;

using Float4 = std::array<float, 4>;

struct Texture {
    TextureDesc desc;
    Image image;
};

// A run of draws with the same texture and material, as the GPU device batches them.
struct Batch {
    TextureId texture = TextureId::None;
    Material material;
    std::size_t first = 0;
    std::size_t count = 0;
};

// A vertex on the target: where it lands in 1/256 pixels, and what the shader reads from it.
struct ScreenVertex {
    std::int64_t x = 0;
    std::int64_t y = 0;
    // u, v, r, g, b, a, then the position lights are placed by.
    std::array<double, 8> varying{};
};

// A vertex as the vertex stage leaves it: a game's vertex() may move it and change its texture
// coordinate and color.
struct ShadedVertex {
    float x = 0.0F;
    float y = 0.0F;
    float u = 0.0F;
    float v = 0.0F;
    std::array<double, 4> color{};
};

// A game's shader on the CPU.
struct SoftwareShader {
    std::optional<ShaderVm> vertex;
    ShaderVm fragment;
    bool normal_used = false;
    bool specular_used = false;
};

// The signed doubled area of a, b, p, positive when p is right of a to b on a y-down target.
std::int64_t edge(const ScreenVertex& a, const ScreenVertex& b, std::int64_t px, std::int64_t py) {
    return ((b.x - a.x) * (py - a.y)) - ((b.y - a.y) * (px - a.x));
}

// Whether an edge of a triangle with positive area is a top or left edge, whose pixel centers
// count as inside.
bool top_left(const ScreenVertex& a, const ScreenVertex& b) {
    const std::int64_t dy = b.y - a.y;
    return dy < 0 || (dy == 0 && b.x > a.x);
}

float clamp01(float value) {
    return std::clamp(value, 0.0F, 1.0F);
}

std::uint8_t to_byte(float value) {
    return static_cast<std::uint8_t>(std::lround(clamp01(value) * 255.0F));
}

// The GPU device's blend states (gpu_device.cpp), on a straight-alpha source color clamped to 0
// to 1 and the target's color.
Float4 blend(BlendMode mode, const Float4& src, const Float4& dst) {
    Float4 out{};
    const float sa = src[3];
    switch (mode) {
    case BlendMode::Mix:
        for (std::size_t i = 0; i < 3; ++i) {
            out.at(i) = (src.at(i) * sa) + (dst.at(i) * (1.0F - sa));
        }
        out[3] = sa + (dst[3] * (1.0F - sa));
        break;
    case BlendMode::Add:
        for (std::size_t i = 0; i < 3; ++i) {
            out.at(i) = (src.at(i) * sa) + dst.at(i);
        }
        out[3] = (sa * sa) + dst[3];
        break;
    case BlendMode::Sub:
        for (std::size_t i = 0; i < 3; ++i) {
            out.at(i) = dst.at(i) - (src.at(i) * sa);
        }
        out[3] = dst[3] - (sa * sa);
        break;
    case BlendMode::Mul:
        for (std::size_t i = 0; i < 4; ++i) {
            out.at(i) = src.at(i) * dst.at(i);
        }
        break;
    case BlendMode::PremulAlpha:
        for (std::size_t i = 0; i < 3; ++i) {
            out.at(i) = src.at(i) + (dst.at(i) * (1.0F - sa));
        }
        out[3] = sa + (dst[3] * (1.0F - sa));
        break;
    }
    return out;
}

class SoftwareDevice final : public RenderDevice {
public:
    using RenderDevice::draw;
    using RenderDevice::process_particles;

    [[nodiscard]] Backend backend() const override { return Backend::Software; }

    // It samples RGBA8 and decodes compressed texels as a GPU would.
    [[nodiscard]] bool supports(TextureFormat format) const override {
        return format == TextureFormat::Rgba8;
    }

    Result<TextureId> create_texture(const TextureDesc& desc,
                                     std::span<const std::uint8_t> texels) override {
        if (auto checked = check_texture_desc(desc); !checked) {
            return checked.error();
        }
        if (auto checked = check_pixels(desc, texels, true); !checked) {
            return checked.error();
        }
        Texture texture{.desc = desc,
                        .image = {.width = desc.width, .height = desc.height, .pixels = {}}};
        if (texels.empty()) {
            texture.image.pixels.assign(std::size_t{desc.width} * desc.height * 4, 0);
        } else {
            auto pixels = decode_texels(desc.format, desc.width, desc.height, texels);
            if (!pixels) {
                return pixels.error();
            }
            texture.image.pixels = std::move(pixels.value());
        }
        const auto id = static_cast<TextureId>(next_texture_++);
        textures_[id] = std::move(texture);
        return id;
    }

    Result<void> update_texture(TextureId texture, std::span<const std::uint8_t> texels) override {
        const auto it = textures_.find(texture);
        if (it == textures_.end()) {
            return detail::unknown_texture(texture);
        }
        const TextureDesc& desc = it->second.desc;
        if (auto checked = check_pixels(desc, texels, false); !checked) {
            return checked;
        }
        auto pixels = decode_texels(desc.format, desc.width, desc.height, texels);
        if (!pixels) {
            return pixels.error();
        }
        it->second.image.pixels = std::move(pixels.value());
        return {};
    }

    void destroy_texture(TextureId texture) override {
        const auto it = textures_.find(texture);
        if (it == textures_.end()) {
            return;
        }
        // The frame being built may still draw with it, so it goes once the frame is drawn.
        if (in_frame_) {
            retired_[texture] = std::move(it->second);
        }
        textures_.erase(it);
    }

    Result<ShaderId> create_shader(const CompiledShader& shader) override {
        if (auto checked = detail::check_compiled_shader(shader, Backend::Software); !checked) {
            return checked.error();
        }
        auto made = std::make_shared<SoftwareShader>();
        made->normal_used = shader.normal_map_used;
        made->specular_used = shader.specular_used;
        auto fragment = ShaderVm::load(shader.cpu_fragment);
        if (!fragment) {
            return fragment.error();
        }
        made->fragment = std::move(fragment).value();
        if (!shader.cpu_vertex.empty()) {
            auto vertex = ShaderVm::load(shader.cpu_vertex);
            if (!vertex) {
                return vertex.error();
            }
            made->vertex = std::move(vertex).value();
        }
        const auto id = static_cast<ShaderId>(next_shader_++);
        shaders_[id] = std::move(made);
        return id;
    }

    void destroy_shader(ShaderId shader) override {
        const auto it = shaders_.find(shader);
        if (it == shaders_.end()) {
            return;
        }
        if (in_frame_) {
            retired_shaders_[shader] = std::move(it->second);
        }
        shaders_.erase(it);
    }

    Result<ParticlesId> create_particles(std::uint32_t amount) override {
        return particles_.create(amount);
    }

    void destroy_particles(ParticlesId particles) override { particles_.destroy(particles); }

    Result<void> process_particles(ParticlesId particles, const ParticleProcess& process,
                                   std::span<const ParticleStep> steps,
                                   std::span<const ParticleShape> shapes) override {
        return particles_.process(particles, process, steps, shapes);
    }

    Result<void> draw_particles(ParticlesId particles, TextureId texture, const ParticleDraw& look,
                                const Material& material) override {
        if (!in_frame_) {
            return detail::no_frame("draw_particles");
        }
        if (auto made = particles_.vertices(particles, look, vertices_.size(), particle_vertices_);
            !made) {
            return made;
        }
        // As the GPU device, each draw of particles is a draw of its own.
        break_batch_ = true;
        auto drawn = draw(texture, particle_vertices_, material);
        break_batch_ = true;
        return drawn;
    }

    Result<std::vector<Particle>> read_particles(ParticlesId particles) override {
        if (in_frame_) {
            return detail::frame_in_progress("read_particles");
        }
        return particles_.read(particles);
    }

    Result<bool> begin_frame(const Frame& frame) override {
        if (in_frame_) {
            return detail::frame_in_progress("begin_frame");
        }
        if (frame.target == TextureId::None) {
            return make_error(ErrorCode::InvalidArgument,
                              "the software device has no window; draw to a render-target "
                              "texture");
        }
        const auto it = textures_.find(frame.target);
        if (it == textures_.end()) {
            return detail::unknown_texture(frame.target);
        }
        if (!it->second.desc.render_target) {
            return detail::not_a_render_target(frame.target);
        }
        if (auto checked = detail::check_lighting(
                frame.lighting, frame.target, [&](TextureId id) { return textures_.contains(id); },
                [&](TextureId id) {
                    const TextureDesc& desc = textures_.at(id).desc;
                    return std::pair{desc.width, desc.height};
                });
            !checked) {
            return checked.error();
        }
        frame_ = frame;
        in_frame_ = true;
        vertices_.clear();
        batches_.clear();
        return true;
    }

    Result<void> draw(TextureId texture, std::span<const Vertex2D> vertices,
                      const Material& material) override {
        if (!in_frame_) {
            return detail::no_frame("draw");
        }
        if (texture != TextureId::None && !textures_.contains(texture)) {
            return detail::unknown_texture(texture);
        }
        if (texture != TextureId::None && texture == frame_.target) {
            return detail::target_in_use(texture);
        }
        if (auto checked = detail::check_material(
                material, frame_.target, [&](TextureId id) { return textures_.contains(id); },
                [&](ShaderId id) { return shaders_.contains(id); });
            !checked) {
            return checked;
        }
        if (auto checked = check_vertices(vertices, vertices_.size()); !checked) {
            return checked;
        }
        if (auto checked = detail::check_lighting_textures(
                frame_.lighting, [&](TextureId id) { return textures_.contains(id); });
            !checked) {
            return checked;
        }
        if (vertices.empty()) {
            return {};
        }
        if (batches_.empty() || break_batch_ || batches_.back().texture != texture ||
            batches_.back().material != material) {
            batches_.push_back(
                {.texture = texture, .material = material, .first = vertices_.size(), .count = 0});
            ++stats_.draws;
            break_batch_ = false;
        }
        batches_.back().count += vertices.size();
        vertices_.insert(vertices_.end(), vertices.begin(), vertices.end());
        stats_.triangles += vertices.size() / 3;
        return {};
    }

    Result<void> end_frame() override {
        if (!in_frame_) {
            return detail::no_frame("end_frame");
        }
        in_frame_ = false;
        ++stats_.frames;
        Image& target = find(frame_.target)->image;
        const std::array<std::uint8_t, 4> clear = detail::to_rgba8(frame_.clear);
        for (std::size_t i = 0; i < target.pixels.size(); i += 4) {
            std::copy(clear.begin(), clear.end(),
                      target.pixels.begin() + static_cast<std::ptrdiff_t>(i));
        }
        const Mat4 view = frame_.view.value_or(
            pixel_projection(static_cast<float>(target.width), static_cast<float>(target.height)));
        for (const Batch& batch : batches_) {
            draw_batch(batch,
                       batch.material.transform ? multiply(view, *batch.material.transform) : view,
                       target);
        }
        retired_.clear();
        retired_shaders_.clear();
        return {};
    }

    Result<Image> read_texture(TextureId texture) override {
        if (in_frame_) {
            return detail::frame_in_progress("read_texture");
        }
        const auto it = textures_.find(texture);
        if (it == textures_.end()) {
            return detail::unknown_texture(texture);
        }
        if (!it->second.desc.render_target) {
            return detail::not_a_render_target(texture);
        }
        return it->second.image;
    }

    VsyncMode set_vsync(VsyncMode mode) override {
        vsync_ = mode;
        return vsync_;
    }
    [[nodiscard]] VsyncMode vsync() const override { return vsync_; }

    [[nodiscard]] Stats stats() const override {
        Stats stats = stats_;
        stats.textures = static_cast<std::uint32_t>(textures_.size());
        for (const auto& [id, texture] : textures_) {
            // Compressed texels are kept decoded.
            stats.texture_bytes += texture.image.pixels.size();
        }
        return stats;
    }

private:
    // A texture as the shader binds it: None, or one gone, is white.
    struct Bound {
        const Image* image = nullptr;
        Filter filter = Filter::Linear;
        bool repeat = false;

        [[nodiscard]] Float4 sample(float u, float v) const {
            if (image == nullptr) {
                return {1.0F, 1.0F, 1.0F, 1.0F};
            }
            return sample_image(*image, filter, repeat, u, v);
        }
    };

    // A live texture, or one destroyed during this frame.
    Texture* find(TextureId texture) {
        if (const auto it = textures_.find(texture); it != textures_.end()) {
            return &it->second;
        }
        if (const auto it = retired_.find(texture); it != retired_.end()) {
            return &it->second;
        }
        return nullptr;
    }

    [[nodiscard]] Bound bound(TextureId texture, bool repeat) {
        const Texture* found = texture == TextureId::None ? nullptr : find(texture);
        if (found == nullptr) {
            return {};
        }
        return {.image = &found->image, .filter = found->desc.filter, .repeat = repeat};
    }

    // A vertex as it is drawn, before any vertex().
    static ShadedVertex shaded(const Vertex2D& vertex) {
        constexpr double kByte = 255.0;
        return {.x = vertex.x,
                .y = vertex.y,
                .u = vertex.u,
                .v = vertex.v,
                .color = {vertex.rgba[0] / kByte, vertex.rgba[1] / kByte, vertex.rgba[2] / kByte,
                          vertex.rgba[3] / kByte}};
    }

    // Where a vertex lands on a target of this size, or nullopt behind the viewer.
    static std::optional<ScreenVertex> to_screen(const ShadedVertex& vertex, const Mat4& view,
                                                 const Image& target) {
        const float x = vertex.x;
        const float y = vertex.y;
        const float cx = (view[0] * x) + (view[4] * y) + view[12];
        const float cy = (view[1] * x) + (view[5] * y) + view[13];
        const float cw = (view[3] * x) + (view[7] * y) + view[15];
        if (!(cw > 0.0F)) {
            return std::nullopt;
        }
        const double sx = (static_cast<double>(cx / cw) + 1.0) * 0.5 * target.width;
        const double sy = (1.0 - static_cast<double>(cy / cw)) * 0.5 * target.height;
        // Far enough for any 2D draw, near enough that edge() can't overflow.
        constexpr double kLimit = 4.0e6;
        if (!(std::abs(sx) < kLimit) || !(std::abs(sy) < kLimit)) {
            return std::nullopt;
        }
        ScreenVertex out;
        out.x = std::llround(sx * kSubpixel);
        out.y = std::llround(sy * kSubpixel);
        out.varying = {
            vertex.u, vertex.v, vertex.color[0], vertex.color[1], vertex.color[2], vertex.color[3],
            x,        y};
        return out;
    }

    // A game's shader, if there is one by that ID, live or destroyed during this frame.
    SoftwareShader* find_shader(ShaderId shader) {
        if (const auto it = shaders_.find(shader); it != shaders_.end()) {
            return it->second.get();
        }
        if (const auto it = retired_shaders_.find(shader); it != retired_shaders_.end()) {
            return it->second.get();
        }
        return nullptr;
    }

    void draw_batch(const Batch& batch, const Mat4& view, Image& target) {
        const Material& material = batch.material;
        std::array<Bound, kMaterialTextureSlots> slots{};
        slots[0] = bound(batch.texture, false);
        for (std::size_t slot = 1; slot < kMaterialTextureSlots; ++slot) {
            const MaterialTexture& texture = material.textures.at(slot - 1);
            slots.at(slot) = bound(texture.texture, texture.repeat);
        }
        Shader shader{.material = &material,
                      .slots = slots,
                      .normal = bound(material.shading.normal_map, false),
                      .specular = bound(material.shading.specular_map, false),
                      .atlas = bound(frame_.lighting.atlas, false),
                      .shadows = bound(frame_.lighting.shadow_map, false),
                      .texel = {1.0F, 1.0F},
                      .game = nullptr,
                      .uniforms = {}};
        if (slots[0].image != nullptr) {
            shader.texel = {1.0F / static_cast<float>(slots[0].image->width),
                            1.0F / static_cast<float>(slots[0].image->height)};
        }
        std::vector<ShadedVertex> corners;
        corners.reserve(batch.count);
        for (std::size_t i = batch.first; i < batch.first + batch.count; ++i) {
            corners.push_back(shaded(vertices_[i]));
        }
        if (material.shader != ShaderId::None) {
            shader.game = find_shader(material.shader);
            // The constants, then the texel size, as cpu_shader lays them out.
            const std::size_t constants =
                std::min(material.program.constants.size(), cpu_shader::kConstants);
            std::memcpy(shader.uniforms.data(), material.program.constants.data(),
                        constants * sizeof(std::array<float, 4>));
            const std::array<float, 4> texel{shader.texel[0], shader.texel[1], 0.0F, 0.0F};
            std::memcpy(shader.uniforms.data() + (cpu_shader::kConstants * sizeof(texel)),
                        texel.data(), sizeof(texel));
            if (shader.game != nullptr) {
                if (std::optional<ShaderVm>& vertex = shader.game->vertex; vertex.has_value()) {
                    run_vertex(*vertex, shader, corners);
                }
            }
        }
        for (std::size_t i = 0; i + 2 < corners.size(); i += 3) {
            const auto a = to_screen(corners[i], view, target);
            const auto b = to_screen(corners[i + 1], view, target);
            const auto c = to_screen(corners[i + 2], view, target);
            if (a && b && c) {
                rasterize(*a, *b, *c, shader, target);
            }
        }
    }

    // What one batch's pixels are shaded with.
    struct Shader {
        const Material* material = nullptr;
        std::array<Bound, kMaterialTextureSlots> slots{};
        Bound normal;
        Bound specular;
        Bound atlas;
        Bound shadows;
        std::array<float, 2> texel{};
        // A game's shader, and the uniform buffer it reads.
        SoftwareShader* game = nullptr;
        std::array<std::byte, cpu_shader::kUniformBytes> uniforms{};

        [[nodiscard]] ShaderVm::Textures textures() const {
            return {
                .sample =
                    [this](std::uint32_t binding, float u, float v) {
                        return slots.at(std::min<std::size_t>(binding, 3)).sample(u, v);
                    },
                .fetch = [this](std::uint32_t binding, std::int32_t x, std::int32_t y) -> Float4 {
                    const Image* image = slots.at(std::min<std::size_t>(binding, 3)).image;
                    if (image == nullptr) {
                        return {1.0F, 1.0F, 1.0F, 1.0F};
                    }
                    if (x < 0 || y < 0 || std::cmp_greater_equal(x, image->width) ||
                        std::cmp_greater_equal(y, image->height)) {
                        return {0.0F, 0.0F, 0.0F, 0.0F};
                    }
                    const std::size_t at = ((static_cast<std::size_t>(y) * image->width) +
                                            static_cast<std::size_t>(x)) *
                                           4;
                    const auto channel = [&](std::size_t i) {
                        return static_cast<float>(image->pixels[at + i]) / 255.0F;
                    };
                    return {channel(0), channel(1), channel(2), channel(3)};
                },
                .size = [this](std::uint32_t binding) -> std::array<std::uint32_t, 2> {
                    const Image* image = slots.at(std::min<std::size_t>(binding, 3)).image;
                    if (image == nullptr) {
                        return {1, 1};
                    }
                    return {image->width, image->height};
                }};
        }
    };

    // Runs a game's vertex() on each corner, four at a time.
    static void run_vertex(ShaderVm& vm, const Shader& shader, std::vector<ShadedVertex>& corners) {
        const std::array<std::span<const std::byte>, 1> uniforms{shader.uniforms};
        const ShaderVm::Textures textures = shader.textures();
        for (std::size_t first = 0; first < corners.size(); first += ShaderVm::kLanes) {
            ShaderVm::Lanes lanes;
            lanes.count =
                static_cast<std::uint32_t>(std::min(ShaderVm::kLanes, corners.size() - first));
            for (std::size_t lane = 0; lane < lanes.count; ++lane) {
                const ShadedVertex& corner = corners[first + lane];
                auto& in = lanes.inputs.at(lane);
                in.at(cpu_shader::kVertex) = {corner.x, corner.y, 0.0F, 1.0F};
                in.at(cpu_shader::kUv) = {corner.u, corner.v, 0.0F, 0.0F};
                in.at(cpu_shader::kColor) = {
                    static_cast<float>(corner.color[0]), static_cast<float>(corner.color[1]),
                    static_cast<float>(corner.color[2]), static_cast<float>(corner.color[3])};
            }
            vm.run(lanes, uniforms, textures);
            for (std::size_t lane = 0; lane < lanes.count; ++lane) {
                const auto& out = lanes.outputs.at(lane);
                ShadedVertex& corner = corners[first + lane];
                corner.x = out.at(cpu_shader::kVertex)[0];
                corner.y = out.at(cpu_shader::kVertex)[1];
                corner.u = out.at(cpu_shader::kUv)[0];
                corner.v = out.at(cpu_shader::kUv)[1];
                for (std::size_t k = 0; k < 4; ++k) {
                    corner.color.at(k) = out.at(cpu_shader::kColor).at(k);
                }
            }
        }
    }

    // ddx and ddy of each attribute, constant over a triangle.
    struct Derivatives {
        std::array<float, 8> dx{};
        std::array<float, 8> dy{};
    };

    // A triangle ready to rasterize: its corners with positive area, the pixels whose centers
    // can be inside, the top-left rule's biases and how its attributes change per pixel.
    struct Triangle {
        ScreenVertex a;
        ScreenVertex b;
        ScreenVertex c;
        double area = 0.0;
        std::int64_t x0 = 0;
        std::int64_t y0 = 0;
        std::int64_t x1 = 0;
        std::int64_t y1 = 0;
        std::array<std::int64_t, 3> bias{};
        Derivatives slopes;

        // Whether the pixel's center is inside, and its attributes there when it is. With
        // outside, outside pixels get them too, as a GPU extrapolates them for the pixels of a
        // 2x2 quad that a triangle misses.
        bool at(std::int64_t px, std::int64_t py, std::array<float, 8>& varying,
                bool outside = false) const {
            const std::int64_t sx = (px * kSubpixel) + (kSubpixel / 2);
            const std::int64_t sy = (py * kSubpixel) + (kSubpixel / 2);
            const std::int64_t wa = edge(b, c, sx, sy);
            const std::int64_t wb = edge(c, a, sx, sy);
            const std::int64_t wc = edge(a, b, sx, sy);
            const bool inside = wa - bias[0] >= 0 && wb - bias[1] >= 0 && wc - bias[2] >= 0;
            if (!inside && !outside) {
                return false;
            }
            const double la = static_cast<double>(wa) / area;
            const double lb = static_cast<double>(wb) / area;
            const double lc = static_cast<double>(wc) / area;
            for (std::size_t k = 0; k < varying.size(); ++k) {
                varying.at(k) = static_cast<float>((la * a.varying.at(k)) + (lb * b.varying.at(k)) +
                                                   (lc * c.varying.at(k)));
            }
            return inside;
        }
    };

    static std::optional<Triangle> set_up(ScreenVertex a, ScreenVertex b, ScreenVertex c,
                                          const Image& target) {
        std::int64_t area = edge(a, b, c.x, c.y);
        if (area == 0) {
            return std::nullopt;
        }
        if (area < 0) {
            std::swap(b, c);
            area = -area;
        }
        const std::int64_t width = target.width;
        const std::int64_t height = target.height;
        // The pixels whose centers can be inside.
        const auto first_pixel = [](std::int64_t lowest) {
            // The first pixel whose center, at (i + 0.5) * kSubpixel, is at or past lowest.
            const std::int64_t shifted = lowest - (kSubpixel / 2);
            return shifted <= 0 ? -((-shifted) / kSubpixel)
                                : ((shifted + kSubpixel - 1) / kSubpixel);
        };
        Triangle out;
        out.x0 = std::max<std::int64_t>(0, first_pixel(std::min({a.x, b.x, c.x})));
        out.y0 = std::max<std::int64_t>(0, first_pixel(std::min({a.y, b.y, c.y})));
        out.x1 = std::min<std::int64_t>(width - 1, first_pixel(std::max({a.x, b.x, c.x}) + 1) - 1);
        out.y1 = std::min<std::int64_t>(height - 1, first_pixel(std::max({a.y, b.y, c.y}) + 1) - 1);
        if (out.x0 > out.x1 || out.y0 > out.y1) {
            return std::nullopt;
        }
        out.bias = {top_left(b, c) ? 0 : 1, top_left(c, a) ? 0 : 1, top_left(a, b) ? 0 : 1};

        // How each attribute changes per pixel across and down, for ddx and ddy.
        const auto area_px = static_cast<double>(area) / static_cast<double>(kSubpixel);
        const std::array<double, 3> dx{static_cast<double>(b.y - c.y) / area_px,
                                       static_cast<double>(c.y - a.y) / area_px,
                                       static_cast<double>(a.y - b.y) / area_px};
        const std::array<double, 3> dy{static_cast<double>(c.x - b.x) / area_px,
                                       static_cast<double>(a.x - c.x) / area_px,
                                       static_cast<double>(b.x - a.x) / area_px};
        for (std::size_t k = 0; k < out.slopes.dx.size(); ++k) {
            out.slopes.dx.at(k) = static_cast<float>(
                (dx[0] * a.varying.at(k)) + (dx[1] * b.varying.at(k)) + (dx[2] * c.varying.at(k)));
            out.slopes.dy.at(k) = static_cast<float>(
                (dy[0] * a.varying.at(k)) + (dy[1] * b.varying.at(k)) + (dy[2] * c.varying.at(k)));
        }
        out.a = a;
        out.b = b;
        out.c = c;
        out.area = static_cast<double>(area);
        return out;
    }

    void rasterize(const ScreenVertex& a, const ScreenVertex& b, const ScreenVertex& c,
                   const Shader& shader, Image& target) {
        const auto triangle = set_up(a, b, c, target);
        if (!triangle) {
            return;
        }
        if (shader.game != nullptr) {
            rasterize_quads(*triangle, shader, target);
            return;
        }
        for (std::int64_t py = triangle->y0; py <= triangle->y1; ++py) {
            for (std::int64_t px = triangle->x0; px <= triangle->x1; ++px) {
                std::array<float, 8> varying{};
                if (!triangle->at(px, py, varying)) {
                    continue;
                }
                const Float4 color =
                    shade_pixel(shader, varying, triangle->slopes,
                                {static_cast<float>(px) + 0.5F, static_cast<float>(py) + 0.5F});
                write(shader, target, px, py, color);
            }
        }
    }

    // A game's fragment() runs on 2x2 quads of pixels, as GPUs run it, so ddx and ddy work.
    void rasterize_quads(const Triangle& triangle, const Shader& shader, Image& target) {
        const Material& material = *shader.material;
        const std::array<std::span<const std::byte>, 1> uniforms{shader.uniforms};
        const ShaderVm::Textures textures = shader.textures();
        const bool normal_used =
            material.shading.normal_map != TextureId::None || shader.game->normal_used;
        const bool specular_used =
            material.shading.specular_map != TextureId::None || shader.game->specular_used;
        for (std::int64_t qy = triangle.y0 & ~std::int64_t{1}; qy <= triangle.y1; qy += 2) {
            for (std::int64_t qx = triangle.x0 & ~std::int64_t{1}; qx <= triangle.x1; qx += 2) {
                ShaderVm::Lanes lanes;
                lanes.count = 4;
                lanes.quad = true;
                std::array<bool, 4> covered{};
                std::array<std::array<float, 8>, 4> varyings{};
                for (std::size_t lane = 0; lane < 4; ++lane) {
                    const std::int64_t px = qx + static_cast<std::int64_t>(lane & 1U);
                    const std::int64_t py = qy + static_cast<std::int64_t>(lane >> 1U);
                    std::array<float, 8>& varying = varyings.at(lane);
                    covered.at(lane) = triangle.at(px, py, varying, true) && px >= triangle.x0 &&
                                       px <= triangle.x1 && py >= triangle.y0 && py <= triangle.y1;
                    const float u = varying[0];
                    const float v = varying[1];
                    Float4 normal{0.5F, 0.5F, 1.0F, 1.0F};
                    if (material.shading.normal_map != TextureId::None) {
                        normal = shader.normal.sample(u, v);
                    }
                    Float4 shine = shader.specular.sample(u, v);
                    for (std::size_t i = 0; i < 4; ++i) {
                        shine.at(i) *= material.shading.specular.at(i);
                    }
                    auto& in = lanes.inputs.at(lane);
                    in.at(cpu_shader::kUv) = {u, v, 0.0F, 0.0F};
                    in.at(cpu_shader::kColor) = {varying[2], varying[3], varying[4], varying[5]};
                    in.at(cpu_shader::kFragCoord) = {static_cast<float>(px) + 0.5F,
                                                     static_cast<float>(py) + 0.5F, 0.0F, 1.0F};
                    in.at(cpu_shader::kTextureColor) = shader.slots[0].sample(u, v);
                    in.at(cpu_shader::kNormalTexel) = normal;
                    in.at(cpu_shader::kSpecular) = shine;
                }
                if (!covered[0] && !covered[1] && !covered[2] && !covered[3]) {
                    continue;
                }
                shader.game->fragment.run(lanes, uniforms, textures);
                for (std::size_t lane = 0; lane < 4; ++lane) {
                    if (!covered.at(lane) || lanes.discarded.at(lane)) {
                        continue;
                    }
                    const auto& out = lanes.outputs.at(lane);
                    std::optional<Float4> normal;
                    if (normal_used) {
                        normal = out.at(cpu_shader::kOutNormalMap);
                    }
                    std::optional<Float4> shine;
                    if (specular_used) {
                        shine = out.at(cpu_shader::kOutSpecular);
                    }
                    const Float4 color = light(shader, out.at(cpu_shader::kOutColor),
                                               varyings.at(lane), triangle.slopes, normal, shine);
                    write(shader, target, qx + static_cast<std::int64_t>(lane & 1U),
                          qy + static_cast<std::int64_t>(lane >> 1U), color);
                }
            }
        }
    }

    // Blends a pixel's color into the target.
    static void write(const Shader& shader, Image& target, std::int64_t px, std::int64_t py,
                      const Float4& color) {
        const std::size_t at =
            ((static_cast<std::size_t>(py) * target.width) + static_cast<std::size_t>(px)) * 4;
        Float4 dst{};
        for (std::size_t i = 0; i < 4; ++i) {
            dst.at(i) = static_cast<float>(target.pixels[at + i]) / 255.0F;
        }
        const Float4 src{clamp01(color[0]), clamp01(color[1]), clamp01(color[2]),
                         clamp01(color[3])};
        const Float4 out = blend(shader.material->blend, src, dst);
        for (std::size_t i = 0; i < 4; ++i) {
            target.pixels[at + i] = to_byte(out.at(i));
        }
    }

    // fs_main in shaders/material.hlsl, or the sprite shader's texture times color.
    [[nodiscard]] Float4 shade_pixel(const Shader& shader, const std::array<float, 8>& varying,
                                     const Derivatives& slopes,
                                     std::array<float, 2> frag_coord) const {
        const Material& material = *shader.material;
        const float u = varying[0];
        const float v = varying[1];
        const Float4 vertex_color{varying[2], varying[3], varying[4], varying[5]};
        Float4 color{};
        if (!material.program.code.empty()) {
            color = run_program(
                material.program,
                {.uv = {u, v},
                 .color = vertex_color,
                 .frag_coord = frag_coord,
                 .texel = shader.texel},
                [&](std::uint32_t slot, float su, float sv) {
                    return shader.slots.at(std::min<std::size_t>(slot, 3)).sample(su, sv);
                });
        } else {
            const Float4 sampled = shader.slots[0].sample(u, v);
            if (material.field.pixel_range > 0.0F) {
                // field_coverage: the target pixels per texel, from how u and v change.
                const float across =
                    1.0F / std::max(std::abs(slopes.dx[0]) + std::abs(slopes.dy[0]), 1.0e-8F);
                const float down =
                    1.0F / std::max(std::abs(slopes.dx[1]) + std::abs(slopes.dy[1]), 1.0e-8F);
                const float scale = 0.5F * ((shader.texel[0] * across) + (shader.texel[1] * down));
                color = {vertex_color[0], vertex_color[1], vertex_color[2],
                         vertex_color[3] * distance_field_coverage(sampled, material.field, scale)};
            } else {
                for (std::size_t i = 0; i < 4; ++i) {
                    color.at(i) = sampled.at(i) * vertex_color.at(i);
                }
            }
        }
        const Shading& shading = material.shading;
        if (shading.mode == LightMode::Unshaded) {
            return color;
        }
        std::optional<Float4> normal;
        if (shading.normal_map != TextureId::None) {
            normal = shader.normal.sample(u, v);
        }
        std::optional<Float4> shine;
        if (shading.specular_map != TextureId::None) {
            shine = shader.specular.sample(u, v);
            for (std::size_t i = 0; i < 4; ++i) {
                shine->at(i) *= shading.specular.at(i);
            }
        }
        return light(shader, color, varying, slopes, normal, shine);
    }

    // shade_canvas_pixel in shaders/canvas.hlsl: a color shaded by the frame's lights, with a
    // normal map's texel and a specular color and shininess when the draw has them.
    [[nodiscard]] Float4 light(const Shader& shader, const Float4& color,
                               const std::array<float, 8>& varying, const Derivatives& slopes,
                               const std::optional<Float4>& normal_texel,
                               const std::optional<Float4>& shine) const {
        const Shading& shading = shader.material->shading;
        if (shading.mode == LightMode::Unshaded) {
            return color;
        }
        // Where the pixel is in the lights' coordinates, and how that changes across the screen.
        std::array<float, 2> position{varying[6], varying[7]};
        std::array<float, 2> position_dx{slopes.dx[6], slopes.dx[7]};
        std::array<float, 2> position_dy{slopes.dy[6], slopes.dy[7]};
        if (shading.to_lights) {
            const auto& m = *shading.to_lights;
            const auto linear = [&](std::array<float, 2> d) {
                return std::array{(m[0] * d[0]) + (m[1] * d[1]), (m[3] * d[0]) + (m[4] * d[1])};
            };
            position = linear(position);
            position[0] += m[2];
            position[1] += m[5];
            position_dx = linear(position_dx);
            position_dy = linear(position_dy);
        }
        ShadePixel pixel{
            .position = position, .color = color, .normal = std::nullopt, .specular = shine};
        if (normal_texel) {
            // The texture's u and v axes in the frame, from how both change across the screen.
            std::array<float, 2> u_axis{1.0F, 0.0F};
            std::array<float, 2> v_axis{0.0F, 1.0F};
            const float det = (slopes.dx[0] * slopes.dy[1]) - (slopes.dy[0] * slopes.dx[1]);
            if (std::abs(det) > 1.0e-12F) {
                const std::array<float, 2> along_u{
                    ((position_dx[0] * slopes.dy[1]) - (position_dy[0] * slopes.dx[1])) / det,
                    ((position_dx[1] * slopes.dy[1]) - (position_dy[1] * slopes.dx[1])) / det};
                const std::array<float, 2> along_v{
                    ((position_dy[0] * slopes.dx[0]) - (position_dx[0] * slopes.dy[0])) / det,
                    ((position_dy[1] * slopes.dx[0]) - (position_dx[1] * slopes.dy[0])) / det};
                const float lu = (along_u[0] * along_u[0]) + (along_u[1] * along_u[1]);
                const float lv = (along_v[0] * along_v[0]) + (along_v[1] * along_v[1]);
                if (lu > 0.0F && lv > 0.0F) {
                    const float su = std::sqrt(lu);
                    const float sv = std::sqrt(lv);
                    u_axis = {along_u[0] / su, along_u[1] / su};
                    v_axis = {along_v[0] / sv, along_v[1] / sv};
                }
            }
            pixel.normal = map_normal(*normal_texel, u_axis, v_axis);
        }
        const Image* shadow_image = shader.shadows.image;
        return shade(
            frame_.lighting.lights, shading.ambient,
            {.mode = shading.mode,
             .light_mask = shading.light_mask,
             .z_index = shading.z_index,
             .layer = shading.layer},
            pixel, [&](float su, float sv) { return shader.atlas.sample(su, sv); },
            [&](std::uint32_t column, std::uint32_t row) -> std::array<std::uint8_t, 4> {
                if (shadow_image == nullptr || column >= shadow_image->width ||
                    row >= shadow_image->height) {
                    return kNoShadow;
                }
                const std::size_t at = ((std::size_t{row} * shadow_image->width) + column) * 4;
                return {shadow_image->pixels[at], shadow_image->pixels[at + 1],
                        shadow_image->pixels[at + 2], shadow_image->pixels[at + 3]};
            });
    }

    std::unordered_map<TextureId, Texture> textures_;
    std::unordered_map<TextureId, Texture> retired_;
    std::uint32_t next_texture_ = 1;
    std::unordered_map<ShaderId, std::shared_ptr<SoftwareShader>> shaders_;
    std::unordered_map<ShaderId, std::shared_ptr<SoftwareShader>> retired_shaders_;
    std::uint32_t next_shader_ = 1;
    detail::CpuParticles particles_;
    std::vector<Vertex2D> particle_vertices_;
    bool in_frame_ = false;
    Frame frame_;
    std::vector<Vertex2D> vertices_;
    std::vector<Batch> batches_;
    bool break_batch_ = false; // the next draw starts a batch
    VsyncMode vsync_ = VsyncMode::Enabled;
    Stats stats_;
};

} // namespace

std::unique_ptr<RenderDevice> create_software_device() {
    return std::make_unique<SoftwareDevice>();
}

} // namespace slash::render
