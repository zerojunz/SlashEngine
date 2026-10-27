#include "slash/render/upscaler.hpp"

#include "fsr_shaders.hpp"

#include "slash/core/text.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>
#include <numbers>
#include <utility>

namespace slash::render {

namespace {

// Two triangles over a width x height rectangle at the origin, texture coordinates 0 to 1.
std::array<Vertex2D, 6> quad(float width, float height) {
    const Vertex2D top_left{.x = 0.0F, .y = 0.0F, .u = 0.0F, .v = 0.0F};
    const Vertex2D top_right{.x = width, .y = 0.0F, .u = 1.0F, .v = 0.0F};
    const Vertex2D bottom_left{.x = 0.0F, .y = height, .u = 0.0F, .v = 1.0F};
    const Vertex2D bottom_right{.x = width, .y = height, .u = 1.0F, .v = 1.0F};
    return {top_left, top_right, bottom_right, top_left, bottom_right, bottom_left};
}

// 2^-stops, the same on every platform (libm's exp2 may differ in the last bit): the whole
// stops exactly, then the rest's exponential as a series in doubles.
float exp2_negative(float stops) {
    const double whole = std::floor(static_cast<double>(stops));
    const double x = -(static_cast<double>(stops) - whole) * std::numbers::ln2;
    double term = 1.0;
    double sum = 1.0;
    for (int n = 1; n < 20; ++n) {
        term = term * x / static_cast<double>(n);
        sum += term;
    }
    return static_cast<float>(std::ldexp(sum, -static_cast<int>(whole)));
}

// A render target of a size, made again when the size changes.
class Target {
public:
    Result<TextureId> get(RenderDevice& device, std::uint32_t width, std::uint32_t height,
                          Filter filter) {
        if (texture_ != TextureId::None && width == width_ && height == height_) {
            return texture_;
        }
        release(device);
        auto made = device.create_texture(
            {.width = width, .height = height, .filter = filter, .render_target = true});
        if (!made) {
            return made.error();
        }
        texture_ = made.value();
        width_ = width;
        height_ = height;
        return texture_;
    }

    void release(RenderDevice& device) {
        if (texture_ != TextureId::None) {
            device.destroy_texture(texture_);
            texture_ = TextureId::None;
        }
    }

    [[nodiscard]] TextureId texture() const { return texture_; }

private:
    TextureId texture_ = TextureId::None;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
};

// Stretches the render over the screen, filtered: Godot's bilinear mode. Scaling down by up to 2
// averages the texels under each pixel.
class Bilinear final : public Upscaler {
public:
    Result<TextureId> input(RenderDevice& device, const UpscaleSize& size) override {
        return input_.get(device, size.render_width, size.render_height, Filter::Linear);
    }

    Result<UpscaleDraw> upscale(RenderDevice& /*device*/, const UpscaleSize& /*size*/,
                                const Scaling& /*scaling*/) override {
        return UpscaleDraw{.texture = input_.texture(), .material = {}};
    }

    void release(RenderDevice& device) override { input_.release(device); }

private:
    Target input_;
};

// AMD's FidelityFX Super Resolution 1, as Godot's fsr mode: EASU scales the render up into a
// texture of the screen's size, then RCAS sharpens it as it draws over the screen.
class Fsr final : public Upscaler {
public:
    Result<TextureId> input(RenderDevice& device, const UpscaleSize& size) override {
        return input_.get(device, size.render_width, size.render_height, Filter::Nearest);
    }

    Result<UpscaleDraw> upscale(RenderDevice& device, const UpscaleSize& size,
                                const Scaling& scaling) override {
        if (easu_shader_ == ShaderId::None) {
            auto easu = device.create_shader(shaders::fsr_easu());
            if (!easu) {
                return easu.error();
            }
            easu_shader_ = easu.value();
            auto rcas = device.create_shader(shaders::fsr_rcas());
            if (!rcas) {
                return rcas.error();
            }
            rcas_shader_ = rcas.value();
        }
        auto scaled = easu_.get(device, size.output_width, size.output_height, Filter::Nearest);
        if (!scaled) {
            return scaled.error();
        }
        const auto width = static_cast<float>(size.output_width);
        const auto height = static_cast<float>(size.output_height);
        Frame frame;
        frame.target = scaled.value();
        frame.clear = {.r = 0.0F, .g = 0.0F, .b = 0.0F, .a = 0.0F};
        frame.view = pixel_projection(width, height);
        if (auto began = device.begin_frame(frame); !began) {
            return began.error();
        }
        Material easu;
        easu.shader = easu_shader_;
        const std::array<Vertex2D, 6> vertices = quad(width, height);
        if (auto drawn = device.draw(input_.texture(), vertices, easu); !drawn) {
            (void)device.end_frame();
            return drawn.error();
        }
        if (auto ended = device.end_frame(); !ended) {
            return ended.error();
        }
        UpscaleDraw out{.texture = scaled.value(), .material = {}};
        out.material.shader = rcas_shader_;
        const float sharpness = std::clamp(scaling.fsr_sharpness, 0.0F, kMaxFsrSharpness);
        out.material.program.constants.push_back({exp2_negative(sharpness), 0.0F, 0.0F, 0.0F});
        return out;
    }

    void release(RenderDevice& device) override {
        input_.release(device);
        easu_.release(device);
        for (ShaderId* shader : {&easu_shader_, &rcas_shader_}) {
            if (*shader != ShaderId::None) {
                device.destroy_shader(*shader);
                *shader = ShaderId::None;
            }
        }
    }

private:
    Target input_;
    Target easu_;
    ShaderId easu_shader_ = ShaderId::None;
    ShaderId rcas_shader_ = ShaderId::None;
};

// Apple's MetalFX spatial scaler, as Godot's metalfx_spatial mode, on a device that has it: the
// device scales the render up into a texture of the screen's size, which draws over the screen.
// The world is drawn as it shows (its colors aren't linear), so it scales perceptually.
class MetalFxSpatial final : public Upscaler {
public:
    Result<TextureId> input(RenderDevice& device, const UpscaleSize& size) override {
        return input_.get(device, size.render_width, size.render_height, Filter::Nearest);
    }

    Result<UpscaleDraw> upscale(RenderDevice& device, const UpscaleSize& size,
                                const Scaling& /*scaling*/) override {
        if (output_ == TextureId::None || width_ != size.output_width ||
            height_ != size.output_height) {
            release_output(device);
            auto made = device.create_texture({.width = size.output_width,
                                               .height = size.output_height,
                                               .filter = Filter::Nearest,
                                               .render_target = true,
                                               .scaled_into = true});
            if (!made) {
                return made.error();
            }
            output_ = made.value();
            width_ = size.output_width;
            height_ = size.output_height;
        }
        if (auto scaled = device.metalfx_spatial(input_.texture(), output_); !scaled) {
            return scaled.error();
        }
        return UpscaleDraw{.texture = output_, .material = {}};
    }

    void release(RenderDevice& device) override {
        input_.release(device);
        release_output(device);
    }

private:
    void release_output(RenderDevice& device) {
        if (output_ != TextureId::None) {
            device.destroy_texture(output_);
            output_ = TextureId::None;
        }
    }

    Target input_;
    TextureId output_ = TextureId::None;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
};

struct Registry {
    std::mutex mutex;
    std::vector<UpscalerInfo> added;
};

Registry& registry() {
    static Registry instance;
    return instance;
}

std::vector<UpscalerInfo> built_in() {
    return {
        {.name = std::string(kBilinear),
         .temporal = false,
         .supports = {},
         .make = [] { return std::unique_ptr<Upscaler>(std::make_unique<Bilinear>()); },
         .make_temporal = {}},
        {.name = std::string(kFsr),
         .temporal = false,
         .supports = {},
         .make = [] { return std::unique_ptr<Upscaler>(std::make_unique<Fsr>()); },
         .make_temporal = {}},
        {.name = std::string(kMetalFxSpatial),
         .temporal = false,
         .supports = [](const RenderDevice& device) { return device.metalfx().spatial; },
         .make = [] { return std::unique_ptr<Upscaler>(std::make_unique<MetalFxSpatial>()); },
         .make_temporal = {}},
    };
}

bool valid_name(std::string_view name) {
    return !name.empty() && std::ranges::all_of(name, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    });
}

} // namespace

Scaling scaling_from_json(const json::Value& value) {
    Scaling out;
    if (!value.is_object()) {
        return out;
    }
    if (const json::Value* mode = value.find("mode"); mode != nullptr && mode->is_string()) {
        out.mode = mode->as_string();
    }
    if (const json::Value* scale = value.find("scale"); scale != nullptr && scale->is_number()) {
        out.scale =
            std::clamp(static_cast<float>(scale->as_double()), kMinRenderScale, kMaxRenderScale);
    }
    if (const json::Value* sharpness = value.find("fsr_sharpness");
        sharpness != nullptr && sharpness->is_number()) {
        out.fsr_sharpness =
            std::clamp(static_cast<float>(sharpness->as_double()), 0.0F, kMaxFsrSharpness);
    }
    if (const json::Value* upscaler = value.find("upscaler");
        upscaler != nullptr && upscaler->is_string()) {
        out.upscaler = upscaler->as_string();
    }
    return out;
}

json::Value scaling_json(const Scaling& scaling) {
    json::Value out;
    out["mode"] = scaling.mode;
    out["scale"] = scaling.scale;
    out["fsr_sharpness"] = scaling.fsr_sharpness;
    if (!scaling.upscaler.empty()) {
        out["upscaler"] = scaling.upscaler;
    }
    return out;
}

std::vector<std::string_view> scaling_modes() {
    return {kBilinear, kFsr, kFsr2, kFsr3, kMetalFxSpatial, kMetalFxTemporal};
}

std::string scaling_mode_problem(std::string_view mode) {
    const std::vector<std::string_view> modes = scaling_modes();
    if (std::ranges::find(modes, mode) != modes.end()) {
        return {};
    }
    std::string problem = "unknown scaling mode " + std::string(mode) +
                          "; expected bilinear, fsr, fsr2, fsr3, metalfx_spatial or "
                          "metalfx_temporal";
    if (const std::string_view near = closest_match(mode, modes); !near.empty()) {
        problem += " (did you mean " + std::string(near) + "?)";
    }
    return problem;
}

std::string_view scaling_mode_on(const Scaling& scaling, const RenderDevice& device) {
    const MetalFxSupport metalfx = device.metalfx();
    if (scaling.mode == kMetalFxTemporal) {
        if (!metalfx.temporal) {
            return metalfx.spatial ? kMetalFxSpatial : kFsr2;
        }
        if (scaling.scale < metalfx.min_scale || scaling.scale > metalfx.max_scale) {
            return kFsr2;
        }
        return kMetalFxTemporal;
    }
    if (scaling.mode == kMetalFxSpatial && !metalfx.spatial) {
        return kFsr;
    }
    for (const std::string_view mode : scaling_modes()) {
        if (mode == scaling.mode) {
            return mode;
        }
    }
    return kBilinear;
}

std::uint32_t render_size(std::uint32_t output, float scale) {
    const double scaled = std::floor(static_cast<double>(output) * static_cast<double>(scale));
    return static_cast<std::uint32_t>(std::clamp(scaled, 1.0, double{kMaxTextureSize}));
}

Result<void> add_upscaler(UpscalerInfo info) {
    if (!valid_name(info.name)) {
        return make_error(
            ErrorCode::InvalidArgument,
            "an upscaler's name is lowercase letters, digits and underscores, not \"" + info.name +
                "\"");
    }
    if (info.temporal ? !info.make_temporal : !info.make) {
        return make_error(ErrorCode::InvalidArgument,
                          "upscaler " + info.name + " makes nothing: " +
                              (info.temporal ? "a temporal upscaler needs make_temporal"
                                             : "an upscaler needs make"));
    }
    if (find_upscaler(info.name)) {
        return make_error(ErrorCode::Conflict, "upscaler " + info.name + " already exists");
    }
    Registry& added = registry();
    const std::scoped_lock lock(added.mutex);
    added.added.push_back(std::move(info));
    return {};
}

std::optional<UpscalerInfo> find_upscaler(std::string_view name) {
    for (UpscalerInfo& info : built_in()) {
        if (info.name == name) {
            return std::move(info);
        }
    }
    Registry& added = registry();
    const std::scoped_lock lock(added.mutex);
    for (const UpscalerInfo& info : added.added) {
        if (info.name == name) {
            return info;
        }
    }
    return std::nullopt;
}

std::vector<std::string> upscaler_names() {
    std::vector<std::string> names;
    for (const UpscalerInfo& info : built_in()) {
        names.push_back(info.name);
    }
    Registry& added = registry();
    const std::scoped_lock lock(added.mutex);
    for (const UpscalerInfo& info : added.added) {
        names.push_back(info.name);
    }
    return names;
}

void clear_added_upscalers() {
    Registry& added = registry();
    const std::scoped_lock lock(added.mutex);
    added.added.clear();
}

namespace {

// Whether a frame can upscale its 3D in the device, as Godot's temporal modes do: in Forward+, at a
// scale under 1, on a device that draws Forward+.
bool temporal_frame(const Scaling& scaling, RenderMethod method, const RenderDevice& device) {
    const Backend backend = device.backend();
    return scaling.scale < 1.0F && method == RenderMethod::ForwardPlus &&
           backend != Backend::OpenGl && backend != Backend::OpenGlEs;
}

// The game's temporal upscaler named by scaling, if it runs on device.
std::optional<UpscalerInfo> added_temporal(const Scaling& scaling, const RenderDevice& device) {
    if (scaling.upscaler.empty()) {
        return std::nullopt;
    }
    const Backend backend = device.backend();
    if (backend != Backend::Vulkan && backend != Backend::D3D12 && backend != Backend::Metal &&
        backend != Backend::Software) {
        return std::nullopt;
    }
    std::optional<UpscalerInfo> info = find_upscaler(scaling.upscaler);
    if (!info || !info->temporal || (info->supports && !info->supports(device))) {
        return std::nullopt;
    }
    return info;
}

} // namespace

bool uses_fsr2(const Scaling& scaling, RenderMethod method, const RenderDevice& device) {
    if (!temporal_frame(scaling, method, device)) {
        return false;
    }
    const std::string_view mode = scaling_mode_on(scaling, device);
    return mode == kFsr2 || mode == kFsr3 || mode == kMetalFxTemporal ||
           added_temporal(scaling, device).has_value();
}

std::string temporal_upscaler_on(const Scaling& scaling, RenderMethod method,
                                 const RenderDevice& device) {
    if (!temporal_frame(scaling, method, device)) {
        return {};
    }
    const std::optional<UpscalerInfo> info = added_temporal(scaling, device);
    return info ? info->name : std::string{};
}

std::string choose_upscaler(const Scaling& scaling, const RenderDevice& device) {
    if (!scaling.upscaler.empty()) {
        if (const std::optional<UpscalerInfo> info = find_upscaler(scaling.upscaler);
            info && !info->temporal && (!info->supports || info->supports(device))) {
            return info->name;
        }
    }
    const bool up = !(scaling.scale > 1.0F);
    const std::string_view mode = scaling_mode_on(scaling, device);
    if ((mode == kFsr || mode == kMetalFxSpatial) && up) {
        return std::string(mode);
    }
    if (scaling.scale == 1.0F) {
        return {};
    }
    return std::string(kBilinear);
}

} // namespace slash::render
