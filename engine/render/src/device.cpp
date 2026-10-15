#include "slash/render/device.hpp"

#include "checks.hpp"
#include "gpu_device.hpp"

#include "slash/core/log.hpp"
#include "slash/core/text.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace slash::render {

namespace detail {

Result<void> check_texture_desc(const TextureDesc& desc) {
    if (desc.width == 0 || desc.height == 0 || desc.width > kMaxTextureSize ||
        desc.height > kMaxTextureSize) {
        return make_error(ErrorCode::InvalidArgument,
                          "a texture is 1 to " + std::to_string(kMaxTextureSize) +
                              " pixels wide and high, not " + std::to_string(desc.width) + "x" +
                              std::to_string(desc.height));
    }
    return {};
}

Result<void> check_pixels(const TextureDesc& desc, std::span<const std::uint8_t> rgba,
                          bool allow_empty) {
    const std::size_t expected = std::size_t{desc.width} * desc.height * 4;
    if ((allow_empty && rgba.empty()) || rgba.size() == expected) {
        return {};
    }
    return make_error(ErrorCode::InvalidArgument,
                      "a " + std::to_string(desc.width) + "x" + std::to_string(desc.height) +
                          " texture takes " + std::to_string(expected) + " bytes of RGBA, not " +
                          std::to_string(rgba.size()));
}

Result<void> check_vertices(std::span<const Vertex2D> vertices, std::size_t in_frame) {
    if (vertices.size() % 3 != 0) {
        return make_error(ErrorCode::InvalidArgument, "every three vertices make a triangle, so " +
                                                          std::to_string(vertices.size()) +
                                                          " vertices is not a whole number "
                                                          "of triangles");
    }
    if (vertices.size() > kMaxFrameVertices - in_frame) {
        return make_error(ErrorCode::InvalidArgument,
                          "a frame draws up to " + std::to_string(kMaxFrameVertices) +
                              " vertices; this draw would make " +
                              std::to_string(in_frame + vertices.size()));
    }
    return {};
}

Error unknown_texture(TextureId texture) {
    return make_error(ErrorCode::NotFound,
                      "no texture " + std::to_string(static_cast<std::uint32_t>(texture)));
}

Error not_a_render_target(TextureId texture) {
    return make_error(ErrorCode::InvalidArgument,
                      "texture " + std::to_string(static_cast<std::uint32_t>(texture)) +
                          " is not a render target; create it with render_target set");
}

Error target_in_use(TextureId texture) {
    return make_error(
        ErrorCode::InvalidArgument,
        "texture " + std::to_string(static_cast<std::uint32_t>(texture)) +
            " is this frame's target, so it can't be drawn with until the frame ends");
}

Error no_frame(std::string_view call) {
    return make_error(ErrorCode::InvalidArgument,
                      std::string(call) + " needs a frame; call begin_frame first");
}

Error frame_in_progress(std::string_view call) {
    return make_error(ErrorCode::InvalidArgument,
                      std::string(call) + " can't run during a frame; call end_frame first");
}

std::array<std::uint8_t, 4> to_rgba8(const Color& color) {
    const auto byte = [](float value) {
        return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
    };
    return {byte(color.r), byte(color.g), byte(color.b), byte(color.a)};
}

} // namespace detail

namespace {

using detail::check_pixels;
using detail::check_texture_desc;
using detail::check_vertices;

class NullDevice final : public RenderDevice {
public:
    using RenderDevice::draw;

    [[nodiscard]] Backend backend() const override { return Backend::Null; }

    Result<TextureId> create_texture(const TextureDesc& desc,
                                     std::span<const std::uint8_t> rgba) override {
        if (auto checked = check_texture_desc(desc); !checked) {
            return checked.error();
        }
        if (auto checked = check_pixels(desc, rgba, true); !checked) {
            return checked.error();
        }
        const auto id = static_cast<TextureId>(next_texture_++);
        textures_[id] = Texture{.desc = desc, .cleared = {}};
        return id;
    }

    Result<void> update_texture(TextureId texture, std::span<const std::uint8_t> rgba) override {
        const auto it = textures_.find(texture);
        if (it == textures_.end()) {
            return detail::unknown_texture(texture);
        }
        return check_pixels(it->second.desc, rgba, false);
    }

    void destroy_texture(TextureId texture) override { textures_.erase(texture); }

    Result<bool> begin_frame(const Frame& frame) override {
        if (in_frame_) {
            return detail::frame_in_progress("begin_frame");
        }
        if (frame.target == TextureId::None) {
            return make_error(ErrorCode::InvalidArgument,
                              "the null device has no window; draw to a render-target texture");
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
        it->second.cleared = detail::to_rgba8(frame.clear);
        lighting_ = frame.lighting;
        in_frame_ = true;
        target_ = frame.target;
        frame_vertices_ = 0;
        last_texture_.reset();
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
        if (texture != TextureId::None && texture == target_) {
            return detail::target_in_use(texture);
        }
        if (auto checked = detail::check_material(
                material, target_, [&](TextureId id) { return textures_.contains(id); });
            !checked) {
            return checked;
        }
        if (auto checked = check_vertices(vertices, frame_vertices_); !checked) {
            return checked;
        }
        if (auto checked = detail::check_lighting_textures(
                lighting_, [&](TextureId id) { return textures_.contains(id); });
            !checked) {
            return checked;
        }
        if (vertices.empty()) {
            return {};
        }
        // Batched as the GPU device batches.
        if (last_texture_ != texture || last_material_ != material) {
            ++stats_.draws;
            last_texture_ = texture;
            last_material_ = material;
        }
        frame_vertices_ += vertices.size();
        stats_.triangles += vertices.size() / 3;
        return {};
    }

    Result<void> end_frame() override {
        if (!in_frame_) {
            return detail::no_frame("end_frame");
        }
        in_frame_ = false;
        ++stats_.frames;
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
        const TextureDesc& desc = it->second.desc;
        if (!desc.render_target) {
            return detail::not_a_render_target(texture);
        }
        Image image{.width = desc.width, .height = desc.height, .pixels = {}};
        image.pixels.reserve(std::size_t{desc.width} * desc.height * 4);
        for (std::size_t i = 0; i < std::size_t{desc.width} * desc.height; ++i) {
            image.pixels.insert(image.pixels.end(), it->second.cleared.begin(),
                                it->second.cleared.end());
        }
        return image;
    }

    VsyncMode set_vsync(VsyncMode mode) override {
        vsync_ = mode;
        return vsync_;
    }
    [[nodiscard]] VsyncMode vsync() const override { return vsync_; }

    [[nodiscard]] Stats stats() const override {
        Stats stats = stats_;
        stats.textures = static_cast<std::uint32_t>(textures_.size());
        return stats;
    }

private:
    struct Texture {
        TextureDesc desc;
        std::array<std::uint8_t, 4> cleared{};
    };

    std::unordered_map<TextureId, Texture> textures_;
    std::uint32_t next_texture_ = 1;
    bool in_frame_ = false;
    TextureId target_ = TextureId::None;
    std::size_t frame_vertices_ = 0;
    std::optional<TextureId> last_texture_;
    Material last_material_;
    Lighting lighting_;
    VsyncMode vsync_ = VsyncMode::Enabled;
    Stats stats_;
};

constexpr std::array<std::string_view, 4> kVsyncModes = {"disabled", "enabled", "adaptive",
                                                         "mailbox"};

} // namespace

std::string_view vsync_mode_name(VsyncMode mode) {
    return kVsyncModes.at(static_cast<std::size_t>(mode));
}

std::optional<VsyncMode> parse_vsync_mode(std::string_view name) {
    for (std::size_t i = 0; i < kVsyncModes.size(); ++i) {
        if (kVsyncModes.at(i) == name) {
            return static_cast<VsyncMode>(i);
        }
    }
    return std::nullopt;
}

std::string_view backend_name(Backend backend) {
    switch (backend) {
    case Backend::Null:
        return "null";
    case Backend::Vulkan:
        return "vulkan";
    case Backend::Metal:
        return "metal";
    case Backend::D3D12:
        return "d3d12";
    }
    return "null";
}

Mat4 pixel_projection(float width, float height) {
    Mat4 m{};
    m[0] = 2.0F / width;
    m[5] = -2.0F / height;
    m[10] = 1.0F;
    m[12] = -1.0F;
    m[13] = 1.0F;
    m[15] = 1.0F;
    return m;
}

float distance_field_coverage(const std::array<float, 4>& texel, const DistanceField& field,
                              float scale) {
    // As field_coverage in shaders/material.hlsl, where the screen size of the texture over its
    // size in texels is the scale.
    const float px_size = std::max(field.pixel_range * scale, 1.0F);
    float d =
        std::max(std::min(texel[0], texel[1]), std::min(std::max(texel[0], texel[1]), texel[2]));
    if (field.outline > 0.0F) {
        const float cr =
            std::clamp(field.outline, 0.0F, (field.pixel_range / 2.0F) - 1.0F) / field.pixel_range;
        d = std::min(d, texel[3]);
        return std::clamp((d - 0.5F + cr) * px_size, 0.0F, 1.0F);
    }
    return std::clamp(((d - 0.5F) * px_size) + 0.5F, 0.0F, 1.0F);
}

std::unique_ptr<RenderDevice> create_null_device() {
    return std::make_unique<NullDevice>();
}

Result<std::unique_ptr<RenderDevice>> create_device(const DeviceOptions& options) {
    static constexpr std::array<std::string_view, 5> kDrivers = {"auto", "null", "vulkan", "metal",
                                                                 "d3d12"};
    if (std::ranges::find(kDrivers, options.driver) == kDrivers.end()) {
        std::string message =
            "no render driver \"" + options.driver + "\"; use auto, null, vulkan, metal or d3d12";
        if (const std::string_view hint = closest_match(options.driver, kDrivers); !hint.empty()) {
            message += "; did you mean \"" + std::string(hint) + "\"?";
        }
        return make_error(ErrorCode::InvalidArgument, std::move(message));
    }
    if (options.driver == "null") {
        return create_null_device();
    }
    return detail::create_gpu_device(options);
}

Result<void> draw_empty_frame(RenderDevice& device, const Color& clear) {
    if (device.backend() == Backend::Null) {
        return {};
    }
    auto began = device.begin_frame({.target = TextureId::None, .clear = clear, .view = {}});
    if (!began) {
        return began.error();
    }
    return device.end_frame(); // also ends a skipped frame, e.g. while minimized
}

std::unique_ptr<RenderDevice> create_device_or_null(const DeviceOptions& options) {
    auto device = create_device(options);
    if (device) {
        return std::move(device).value();
    }
    log::warn("drawing nothing: no render device works here",
              {{"driver", options.driver}, {"error", device.error().message}});
    return create_null_device();
}

} // namespace slash::render
