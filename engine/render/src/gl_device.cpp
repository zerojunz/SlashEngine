#include "gl_device.hpp"

#include "checks.hpp"
#include "cpu_particles.hpp"
#include "gpu_device.hpp"
#include "material_shaders.hpp"
#include "sprite_shaders.hpp"
#include "uniforms.hpp"

#include "slash/core/log.hpp"

#include <SDL3/SDL.h>

#ifdef __EMSCRIPTEN__
#include <GLES3/gl3.h>
#endif

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace slash::render::detail {
namespace {

// OpenGL ES 3.0's types and the enums the device uses, so it needs no GL headers: the functions
// come from SDL_GL_GetProcAddress.
using GLenum = unsigned int;
using GLuint = unsigned int;
using GLint = int;
using GLsizei = int;
using GLboolean = unsigned char;
using GLbitfield = unsigned int;
using GLfloat = float;
using GLchar = char;
using GLubyte = unsigned char;
using GLintptr = std::ptrdiff_t;
using GLsizeiptr = std::ptrdiff_t;

namespace gl {
constexpr GLenum kNoError = 0;
constexpr GLboolean kFalse = 0;
constexpr GLboolean kTrue = 1;
constexpr GLenum kTriangles = 0x0004;
constexpr GLenum kZero = 0;
constexpr GLenum kOne = 1;
constexpr GLenum kSrcAlpha = 0x0302;
constexpr GLenum kOneMinusSrcAlpha = 0x0303;
constexpr GLenum kDstAlpha = 0x0304;
constexpr GLenum kDstColor = 0x0306;
constexpr GLenum kFuncAdd = 0x8006;
constexpr GLenum kFuncReverseSubtract = 0x800B;
constexpr GLenum kCullFace = 0x0B44;
constexpr GLenum kDepthTest = 0x0B71;
constexpr GLenum kDither = 0x0BD0;
constexpr GLenum kBlend = 0x0BE2;
constexpr GLenum kScissorTest = 0x0C11;
constexpr GLenum kUnpackAlignment = 0x0CF5;
constexpr GLenum kPackAlignment = 0x0D05;
constexpr GLenum kMaxTextureSize = 0x0D33;
constexpr GLenum kTexture2D = 0x0DE1;
constexpr GLenum kUnsignedByte = 0x1401;
constexpr GLenum kFloat = 0x1406;
constexpr GLenum kRgba = 0x1908;
constexpr GLenum kRenderer = 0x1F01;
constexpr GLenum kVersion = 0x1F02;
constexpr GLenum kNearest = 0x2600;
constexpr GLenum kLinear = 0x2601;
constexpr GLenum kTextureMagFilter = 0x2800;
constexpr GLenum kTextureMinFilter = 0x2801;
constexpr GLenum kTextureWrapS = 0x2802;
constexpr GLenum kTextureWrapT = 0x2803;
constexpr GLenum kRepeat = 0x2901;
constexpr GLbitfield kColorBufferBit = 0x4000;
constexpr GLenum kRgba8 = 0x8058;
constexpr GLenum kClampToEdge = 0x812F;
constexpr GLenum kTexture0 = 0x84C0;
constexpr GLenum kArrayBuffer = 0x8892;
constexpr GLenum kStreamDraw = 0x88E0;
constexpr GLenum kUniformBuffer = 0x8A11;
constexpr GLenum kUniformBufferOffsetAlignment = 0x8A34;
constexpr GLenum kFragmentShader = 0x8B30;
constexpr GLenum kVertexShader = 0x8B31;
constexpr GLenum kCompileStatus = 0x8B81;
constexpr GLenum kLinkStatus = 0x8B82;
constexpr GLenum kInfoLogLength = 0x8B84;
constexpr GLenum kReadFramebuffer = 0x8CA8;
constexpr GLenum kDrawFramebuffer = 0x8CA9;
constexpr GLenum kFramebufferComplete = 0x8CD5;
constexpr GLenum kColorAttachment0 = 0x8CE0;
constexpr GLenum kFramebuffer = 0x8D40;
constexpr GLuint kInvalidIndex = 0xFFFFFFFF;
} // namespace gl

// Each OpenGL ES 3.0 function the device calls: X(name after "gl", member, result, parameters).
// NOLINTBEGIN(bugprone-macro-parentheses): the arguments are types and parameter lists
#define SLASH_GL_FUNCTIONS(X)                                                                      \
    X(ActiveTexture, active_texture, void, (GLenum))                                               \
    X(AttachShader, attach_shader, void, (GLuint, GLuint))                                         \
    X(BindBuffer, bind_buffer, void, (GLenum, GLuint))                                             \
    X(BindBufferRange, bind_buffer_range, void, (GLenum, GLuint, GLuint, GLintptr, GLsizeiptr))    \
    X(BindFramebuffer, bind_framebuffer, void, (GLenum, GLuint))                                   \
    X(BindSampler, bind_sampler, void, (GLuint, GLuint))                                           \
    X(BindTexture, bind_texture, void, (GLenum, GLuint))                                           \
    X(BindVertexArray, bind_vertex_array, void, (GLuint))                                          \
    X(BlendEquationSeparate, blend_equation_separate, void, (GLenum, GLenum))                      \
    X(BlendFuncSeparate, blend_func_separate, void, (GLenum, GLenum, GLenum, GLenum))              \
    X(BlitFramebuffer, blit_framebuffer, void,                                                     \
      (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum))                \
    X(BufferData, buffer_data, void, (GLenum, GLsizeiptr, const void*, GLenum))                    \
    X(CheckFramebufferStatus, check_framebuffer_status, GLenum, (GLenum))                          \
    X(Clear, clear, void, (GLbitfield))                                                            \
    X(ClearColor, clear_color, void, (GLfloat, GLfloat, GLfloat, GLfloat))                         \
    X(CompileShader, compile_shader, void, (GLuint))                                               \
    X(CreateProgram, create_program, GLuint, ())                                                   \
    X(CreateShader, create_shader, GLuint, (GLenum))                                               \
    X(DeleteBuffers, delete_buffers, void, (GLsizei, const GLuint*))                               \
    X(DeleteFramebuffers, delete_framebuffers, void, (GLsizei, const GLuint*))                     \
    X(DeleteProgram, delete_program, void, (GLuint))                                               \
    X(DeleteSamplers, delete_samplers, void, (GLsizei, const GLuint*))                             \
    X(DeleteShader, delete_shader, void, (GLuint))                                                 \
    X(DeleteTextures, delete_textures, void, (GLsizei, const GLuint*))                             \
    X(DeleteVertexArrays, delete_vertex_arrays, void, (GLsizei, const GLuint*))                    \
    X(Disable, disable, void, (GLenum))                                                            \
    X(DrawArrays, draw_arrays, void, (GLenum, GLint, GLsizei))                                     \
    X(Enable, enable, void, (GLenum))                                                              \
    X(EnableVertexAttribArray, enable_vertex_attrib_array, void, (GLuint))                         \
    X(FramebufferTexture2D, framebuffer_texture_2d, void, (GLenum, GLenum, GLenum, GLuint, GLint)) \
    X(GenBuffers, gen_buffers, void, (GLsizei, GLuint*))                                           \
    X(GenFramebuffers, gen_framebuffers, void, (GLsizei, GLuint*))                                 \
    X(GenSamplers, gen_samplers, void, (GLsizei, GLuint*))                                         \
    X(GenTextures, gen_textures, void, (GLsizei, GLuint*))                                         \
    X(GenVertexArrays, gen_vertex_arrays, void, (GLsizei, GLuint*))                                \
    X(GetError, get_error, GLenum, ())                                                             \
    X(GetIntegerv, get_integerv, void, (GLenum, GLint*))                                           \
    X(GetProgramInfoLog, get_program_info_log, void, (GLuint, GLsizei, GLsizei*, GLchar*))         \
    X(GetProgramiv, get_programiv, void, (GLuint, GLenum, GLint*))                                 \
    X(GetShaderInfoLog, get_shader_info_log, void, (GLuint, GLsizei, GLsizei*, GLchar*))           \
    X(GetShaderiv, get_shaderiv, void, (GLuint, GLenum, GLint*))                                   \
    X(GetString, get_string, const GLubyte*, (GLenum))                                             \
    X(GetUniformBlockIndex, get_uniform_block_index, GLuint, (GLuint, const GLchar*))              \
    X(GetUniformLocation, get_uniform_location, GLint, (GLuint, const GLchar*))                    \
    X(LinkProgram, link_program, void, (GLuint))                                                   \
    X(PixelStorei, pixel_storei, void, (GLenum, GLint))                                            \
    X(ReadPixels, read_pixels, void, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*))      \
    X(SamplerParameteri, sampler_parameteri, void, (GLuint, GLenum, GLint))                        \
    X(ShaderSource, shader_source, void, (GLuint, GLsizei, const GLchar* const*, const GLint*))    \
    X(TexImage2D, tex_image_2d, void,                                                              \
      (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*))                \
    X(TexParameteri, tex_parameteri, void, (GLenum, GLenum, GLint))                                \
    X(TexSubImage2D, tex_sub_image_2d, void,                                                       \
      (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*))                \
    X(Uniform1i, uniform_1i, void, (GLint, GLint))                                                 \
    X(UniformBlockBinding, uniform_block_binding, void, (GLuint, GLuint, GLuint))                  \
    X(UseProgram, use_program, void, (GLuint))                                                     \
    X(VertexAttribPointer, vertex_attrib_pointer, void,                                            \
      (GLuint, GLint, GLenum, GLboolean, GLsizei, const void*))                                    \
    X(Viewport, viewport, void, (GLint, GLint, GLsizei, GLsizei))

struct Gl {
#define SLASH_GL_MEMBER(name, member, result, parameters) result(*member) parameters = nullptr;
    SLASH_GL_FUNCTIONS(SLASH_GL_MEMBER)
#undef SLASH_GL_MEMBER
};

// The functions of the context that is current. On the web they are WebGL's, linked in.
Result<Gl> load_gl() {
    Gl gl;
#ifdef __EMSCRIPTEN__
#define SLASH_GL_LOAD(name, member, result, parameters)                                            \
    gl.member = reinterpret_cast<decltype(gl.member)>(&::gl##name);
#else
#define SLASH_GL_LOAD(name, member, result, parameters)                                            \
    gl.member = reinterpret_cast<decltype(gl.member)>(SDL_GL_GetProcAddress("gl" #name));          \
    if (gl.member == nullptr) {                                                                    \
        return make_error(ErrorCode::PlatformFailure, "OpenGL ES has no gl" #name);                \
    }
#endif
    SLASH_GL_FUNCTIONS(SLASH_GL_LOAD)
#undef SLASH_GL_LOAD
    return gl;
}
// NOLINTEND(bugprone-macro-parentheses)

// Uniform block bindings, as the shaders' cbuffers are numbered.
constexpr GLuint kViewBinding = 0;
constexpr GLuint kProgramBinding = 1;
constexpr GLuint kLightsBinding = 2;
// A game's shader's vertex stage reads the material's constants (tools/shader_compiler).
constexpr GLuint kVertexProgramBinding = 3;

// The two shaders: plain sprites, and materials that run a program.
enum class ShaderKind : std::uint8_t { Sprite, Material };

// Each shader's samplers by the names SPIRV-Cross gives them, in the units the device binds:
// the order of a batch's textures (see kMaterialSamplers).
constexpr std::array<std::string_view, 1> kSpriteSamplers{"sprite_texture"};
constexpr std::array<std::string_view, kMaterialSamplers> kMaterialSamplerNames{
    "texture0",   "texture1",     "texture2",    "texture3",
    "normal_map", "specular_map", "light_atlas", "shadow_map"};

// Turns clip space upside down. GL puts clip space's top at a framebuffer's last row, where the
// other backends put it at the first, so frames draw upside down into textures, which leaves
// rows, texture coordinates and gl_FragCoord as they are everywhere else; the window gets its
// frame turned back when it is copied there.
constexpr Mat4 kFlipY{1.0F, 0.0F, 0.0F, 0.0F, 0.0F, -1.0F, 0.0F, 0.0F,
                      0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F,  0.0F, 1.0F};

Error sdl_error(std::string_view what) {
    return make_error(ErrorCode::PlatformFailure, std::string(what) + ": " + SDL_GetError());
}

std::string hex(GLenum value) {
    static constexpr std::string_view kDigits = "0123456789abcdef";
    std::string out = "0x";
    for (int shift = 12; shift >= 0; shift -= 4) {
        out += kDigits[(value >> static_cast<unsigned>(shift)) & 0xFU];
    }
    return out;
}

void request_gl_context() {
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
}

class GlDevice final : public RenderDevice {
public:
    using RenderDevice::draw;

    // window is the game's when presents is set, or else a hidden one the device owns.
    GlDevice(SDL_Window* window, bool presents, bool owns_video)
        : window_(window), presents_(presents), owns_video_(owns_video) {}

    GlDevice(const GlDevice&) = delete;
    GlDevice& operator=(const GlDevice&) = delete;
    GlDevice(GlDevice&&) = delete;
    GlDevice& operator=(GlDevice&&) = delete;

    ~GlDevice() override {
        if (context_ != nullptr) {
            make_current();
            if (gl_.delete_textures != nullptr) {
                for (const auto& [id, texture] : textures_) {
                    delete_texture(texture);
                }
                release_pending();
                delete_texture(present_);
                gl_.delete_textures(1, &white_);
                gl_.delete_samplers(static_cast<GLsizei>(samplers_.size()), samplers_.data());
                for (const GLuint program : programs_) {
                    gl_.delete_program(program);
                }
                for (const auto& [id, program] : game_programs_) {
                    gl_.delete_program(program);
                }
                gl_.delete_buffers(1, &vertex_buffer_);
                gl_.delete_buffers(1, &uniform_buffer_);
                gl_.delete_vertex_arrays(1, &vertex_array_);
            }
            SDL_GL_DestroyContext(context_);
        }
        if (!presents_) {
            SDL_DestroyWindow(window_);
        }
        if (owns_video_) {
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
        }
    }

    // Makes the context, loads its functions, then the sprite program, samplers, buffers and
    // the white texture. The material program waits for the first material.
    Result<void> init() {
        request_gl_context();
        context_ = SDL_GL_CreateContext(window_);
        if (context_ == nullptr) {
            return sdl_error("OpenGL ES 3.0 didn't start");
        }
        auto loaded = load_gl();
        if (!loaded) {
            return loaded.error();
        }
        gl_ = loaded.value();
        const auto* version = reinterpret_cast<const char*>(gl_.get_string(gl::kVersion));
        if (version == nullptr || !std::string_view(version).starts_with("OpenGL ES 3")) {
            return make_error(ErrorCode::PlatformFailure,
                              std::string("the context is ") +
                                  (version != nullptr ? version : "unknown") + ", not OpenGL ES 3");
        }
        const auto* renderer = reinterpret_cast<const char*>(gl_.get_string(gl::kRenderer));
        renderer_ = renderer != nullptr ? renderer : "";
        gl_.get_integerv(gl::kMaxTextureSize, &max_texture_size_);
        gl_.get_integerv(gl::kUniformBufferOffsetAlignment, &uniform_alignment_);
        uniform_alignment_ = std::max(uniform_alignment_, 16);

        gl_.disable(gl::kDither);
        gl_.disable(gl::kDepthTest);
        gl_.disable(gl::kCullFace);
        gl_.disable(gl::kScissorTest);
        gl_.enable(gl::kBlend);
        gl_.pixel_storei(gl::kUnpackAlignment, 4);
        gl_.pixel_storei(gl::kPackAlignment, 4);

        gl_.gen_samplers(static_cast<GLsizei>(samplers_.size()), samplers_.data());
        for (const Filter filter : {Filter::Linear, Filter::Nearest}) {
            for (const bool repeat : {false, true}) {
                const GLuint sampler = samplers_.at(sampler_index(filter, repeat));
                const auto mode =
                    static_cast<GLint>(filter == Filter::Linear ? gl::kLinear : gl::kNearest);
                const auto wrap = static_cast<GLint>(repeat ? gl::kRepeat : gl::kClampToEdge);
                gl_.sampler_parameteri(sampler, gl::kTextureMinFilter, mode);
                gl_.sampler_parameteri(sampler, gl::kTextureMagFilter, mode);
                gl_.sampler_parameteri(sampler, gl::kTextureWrapS, wrap);
                gl_.sampler_parameteri(sampler, gl::kTextureWrapT, wrap);
            }
        }

        gl_.gen_buffers(1, &vertex_buffer_);
        gl_.gen_buffers(1, &uniform_buffer_);
        gl_.gen_vertex_arrays(1, &vertex_array_);
        gl_.bind_vertex_array(vertex_array_);
        gl_.bind_buffer(gl::kArrayBuffer, vertex_buffer_);
        constexpr auto kStride = static_cast<GLsizei>(sizeof(Vertex2D));
        gl_.vertex_attrib_pointer(0, 2, gl::kFloat, gl::kFalse, kStride,
                                  offset(offsetof(Vertex2D, x)));
        gl_.vertex_attrib_pointer(1, 2, gl::kFloat, gl::kFalse, kStride,
                                  offset(offsetof(Vertex2D, u)));
        gl_.vertex_attrib_pointer(2, 4, gl::kUnsignedByte, gl::kTrue, kStride,
                                  offset(offsetof(Vertex2D, rgba)));
        for (GLuint location = 0; location < 3; ++location) {
            gl_.enable_vertex_attrib_array(location);
        }

        if (auto made = program_for(ShaderKind::Sprite); !made) {
            return made.error();
        }
        constexpr std::array<std::uint8_t, 4> kWhite{255, 255, 255, 255};
        auto white = create_gl_texture(
            {.width = 1, .height = 1, .filter = Filter::Linear, .render_target = false}, kWhite);
        if (!white) {
            return white.error();
        }
        white_ = white.value();
        if (presents_ && !SDL_GL_SetSwapInterval(1)) {
            log::warn("V-Sync is unavailable", {{"error", SDL_GetError()}});
        }
        return check_errors("starting OpenGL ES");
    }

    [[nodiscard]] Backend backend() const override { return Backend::OpenGl; }
    [[nodiscard]] const std::string& renderer() const { return renderer_; }

    Result<TextureId> create_texture(const TextureDesc& desc,
                                     std::span<const std::uint8_t> rgba) override {
        if (auto checked = check_texture_desc(desc); !checked) {
            return checked.error();
        }
        if (auto checked = check_pixels(desc, rgba, true); !checked) {
            return checked.error();
        }
        make_current();
        auto gl_texture = create_gl_texture(desc, rgba);
        if (!gl_texture) {
            return gl_texture.error();
        }
        const auto id = static_cast<TextureId>(next_texture_++);
        textures_[id] = Texture{.gl = gl_texture.value(), .framebuffer = 0, .desc = desc};
        return id;
    }

    Result<void> update_texture(TextureId texture, std::span<const std::uint8_t> rgba) override {
        const auto it = textures_.find(texture);
        if (it == textures_.end()) {
            return unknown_texture(texture);
        }
        if (auto checked = check_pixels(it->second.desc, rgba, false); !checked) {
            return checked;
        }
        make_current();
        // Draws are sent at end_frame, so every draw of the frame being built sees the new pixels.
        gl_.bind_texture(gl::kTexture2D, it->second.gl);
        gl_.tex_sub_image_2d(gl::kTexture2D, 0, 0, 0, static_cast<GLsizei>(it->second.desc.width),
                             static_cast<GLsizei>(it->second.desc.height), gl::kRgba,
                             gl::kUnsignedByte, rgba.data());
        return check_errors("updating a texture");
    }

    void destroy_texture(TextureId texture) override {
        const auto it = textures_.find(texture);
        if (it == textures_.end()) {
            return;
        }
        // The frame being built may still draw with it, so it goes once the frame is sent.
        if (in_frame_) {
            pending_release_.push_back(it->second);
        } else {
            make_current();
            delete_texture(it->second);
        }
        textures_.erase(it);
    }

    Result<ShaderId> create_shader(const CompiledShader& shader) override {
        if (auto checked = check_compiled_shader(shader, Backend::OpenGl); !checked) {
            return checked.error();
        }
        make_current();
        auto program = link(shader.glsl_vertex, shader.glsl_fragment, kMaterialSamplerNames,
                            "the game's shader");
        if (!program) {
            return program.error();
        }
        const auto id = static_cast<ShaderId>(next_shader_++);
        game_programs_[id] = program.value();
        return id;
    }

    void destroy_shader(ShaderId shader) override {
        const auto it = game_programs_.find(shader);
        if (it == game_programs_.end()) {
            return;
        }
        // The frame being built may still draw with it, so it goes once the frame is sent.
        if (in_frame_) {
            pending_programs_.push_back(it->second);
        } else {
            make_current();
            gl_.delete_program(it->second);
        }
        game_programs_.erase(it);
    }

    Result<ParticlesId> create_particles(std::uint32_t amount) override {
        return particles_.create(amount);
    }

    void destroy_particles(ParticlesId particles) override { particles_.destroy(particles); }

    Result<void> process_particles(ParticlesId particles, const ParticleProcess& process,
                                   std::span<const ParticleStep> steps) override {
        return particles_.process(particles, process, steps);
    }

    Result<void> draw_particles(ParticlesId particles, TextureId texture, const ParticleDraw& look,
                                const Material& material) override {
        if (!in_frame_) {
            return no_frame("draw_particles");
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
            return frame_in_progress("read_particles");
        }
        return particles_.read(particles);
    }

    Result<bool> begin_frame(const Frame& frame) override {
        if (in_frame_) {
            return frame_in_progress("begin_frame");
        }
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        if (frame.target == TextureId::None) {
            if (!presents_) {
                return make_error(ErrorCode::InvalidArgument,
                                  "this device has no window; draw to a render-target texture");
            }
            int w = 0;
            int h = 0;
            SDL_GetWindowSizeInPixels(window_, &w, &h);
            if ((SDL_GetWindowFlags(window_) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN)) == 0 &&
                w > 0 && h > 0) {
                width = static_cast<std::uint32_t>(w);
                height = static_cast<std::uint32_t>(h);
            }
        } else {
            const auto it = textures_.find(frame.target);
            if (it == textures_.end()) {
                return unknown_texture(frame.target);
            }
            if (!it->second.desc.render_target) {
                return not_a_render_target(frame.target);
            }
            width = it->second.desc.width;
            height = it->second.desc.height;
        }
        if (auto checked = check_lighting(
                frame.lighting, frame.target, [&](TextureId id) { return textures_.contains(id); },
                [&](TextureId id) {
                    const TextureDesc& desc = textures_.at(id).desc;
                    return std::pair{desc.width, desc.height};
                });
            !checked) {
            return checked.error();
        }
        // The framebuffer is made now, so a failure leaves nothing started and a target destroyed
        // during the frame is still drawn into.
        GLuint framebuffer = 0;
        if (width != 0) {
            make_current();
            auto made = frame.target == TextureId::None ? present_framebuffer(width, height)
                                                        : attach(textures_.at(frame.target));
            if (!made) {
                return made.error();
            }
            framebuffer = made.value();
        }
        in_frame_ = true;
        frame_ = frame;
        frame_framebuffer_ = framebuffer;
        frame_width_ = width;
        frame_height_ = height;
        vertices_.clear();
        batches_.clear();
        // Minimized or hidden: there is nothing to draw to this time.
        return width != 0;
    }

    Result<void> draw(TextureId texture, std::span<const Vertex2D> vertices,
                      const Material& material) override {
        if (!in_frame_) {
            return no_frame("draw");
        }
        if (texture != TextureId::None) {
            if (!textures_.contains(texture)) {
                return unknown_texture(texture);
            }
            if (texture == frame_.target) {
                return target_in_use(texture);
            }
        }
        if (auto checked = check_material(
                material, frame_.target, [&](TextureId id) { return textures_.contains(id); },
                [&](ShaderId id) { return game_programs_.contains(id); });
            !checked) {
            return checked;
        }
        if (auto checked = check_vertices(vertices, vertices_.size()); !checked) {
            return checked;
        }
        if (auto checked = check_lighting_textures(
                frame_.lighting, [&](TextureId id) { return textures_.contains(id); });
            !checked) {
            return checked;
        }
        if (vertices.empty() || frame_width_ == 0) {
            return {}; // nothing to draw, or a skipped frame
        }
        const auto first = static_cast<GLint>(vertices_.size());
        const auto count = static_cast<GLsizei>(vertices.size());
        if (batches_.empty() || break_batch_ || batches_.back().texture != texture ||
            batches_.back().material != material) {
            break_batch_ = false;
            const ShaderKind kind =
                material.shader != ShaderId::None || uses_material_shader(material)
                    ? ShaderKind::Material
                    : ShaderKind::Sprite;
            make_current();
            auto program = material.shader != ShaderId::None
                               ? Result<GLuint>(game_programs_.at(material.shader))
                               : program_for(kind);
            if (!program) {
                return program.error();
            }
            Batch batch{.texture = texture,
                        .material = material,
                        .kind = kind,
                        .program = program.value(),
                        .units = {},
                        .texel = {1.0F, 1.0F, 0.0F, 0.0F},
                        .first = first,
                        .count = 0};
            batch.units.at(0) = binding(texture, false);
            for (std::size_t slot = 1; slot < kMaterialTextureSlots; ++slot) {
                const MaterialTexture& bound = material.textures.at(slot - 1);
                batch.units.at(slot) = binding(bound.texture, bound.repeat);
            }
            batch.units.at(kMaterialTextureSlots) = binding(material.shading.normal_map, false);
            batch.units.at(kMaterialTextureSlots + 1) =
                binding(material.shading.specular_map, false);
            batch.units.at(kMaterialTextureSlots + 2) = binding(frame_.lighting.atlas, false);
            batch.units.at(kMaterialTextureSlots + 3) = binding(frame_.lighting.shadow_map, false);
            if (texture != TextureId::None) {
                const TextureDesc& desc = textures_.at(texture).desc;
                batch.texel = {1.0F / static_cast<float>(desc.width),
                               1.0F / static_cast<float>(desc.height), 0.0F, 0.0F};
            }
            batches_.push_back(std::move(batch));
            ++stats_.draws;
        }
        batches_.back().count += count;
        vertices_.insert(vertices_.end(), vertices.begin(), vertices.end());
        stats_.triangles += static_cast<std::uint64_t>(count / 3);
        return {};
    }

    Result<void> end_frame() override {
        if (!in_frame_) {
            return no_frame("end_frame");
        }
        in_frame_ = false;
        if (frame_width_ == 0) {
            finish_frame();
            return {};
        }
        make_current();
        gl_.bind_framebuffer(gl::kFramebuffer, frame_framebuffer_);
        gl_.viewport(0, 0, static_cast<GLsizei>(frame_width_), static_cast<GLsizei>(frame_height_));
        gl_.clear_color(frame_.clear.r, frame_.clear.g, frame_.clear.b, frame_.clear.a);
        gl_.clear(gl::kColorBufferBit);
        if (!batches_.empty()) {
            draw_batches();
        }
        if (frame_.target == TextureId::None) {
            // The frame was drawn upside down (see kFlipY), so it is copied over turned back.
            const auto w = static_cast<GLint>(frame_width_);
            const auto h = static_cast<GLint>(frame_height_);
            gl_.bind_framebuffer(gl::kReadFramebuffer, frame_framebuffer_);
            gl_.bind_framebuffer(gl::kDrawFramebuffer, 0);
            gl_.blit_framebuffer(0, 0, w, h, 0, h, w, 0, gl::kColorBufferBit, gl::kNearest);
            gl_.bind_framebuffer(gl::kFramebuffer, 0);
            if (!SDL_GL_SwapWindow(window_)) {
                finish_frame();
                return sdl_error("showing the frame failed");
            }
        }
        ++stats_.frames;
        finish_frame();
        return check_errors("drawing a frame");
    }

    Result<Image> read_texture(TextureId texture) override {
        if (in_frame_) {
            return frame_in_progress("read_texture");
        }
        const auto it = textures_.find(texture);
        if (it == textures_.end()) {
            return unknown_texture(texture);
        }
        const TextureDesc& desc = it->second.desc;
        if (!desc.render_target) {
            return not_a_render_target(texture);
        }
        make_current();
        auto framebuffer = attach(it->second);
        if (!framebuffer) {
            return framebuffer.error();
        }
        Image image{.width = desc.width, .height = desc.height, .pixels = {}};
        image.pixels.resize(std::size_t{desc.width} * desc.height * 4);
        gl_.bind_framebuffer(gl::kFramebuffer, framebuffer.value());
        // Rows come from the first up, which is the top: frames draw upside down (see kFlipY).
        gl_.read_pixels(0, 0, static_cast<GLsizei>(desc.width), static_cast<GLsizei>(desc.height),
                        gl::kRgba, gl::kUnsignedByte, image.pixels.data());
        gl_.bind_framebuffer(gl::kFramebuffer, 0);
        if (auto checked = check_errors("reading a texture"); !checked) {
            return checked.error();
        }
        return image;
    }

    VsyncMode set_vsync(VsyncMode mode) override {
        if (!presents_) {
            vsync_ = mode;
            return vsync_;
        }
        make_current();
        // Swap intervals: 0 presents at once, 1 waits for the refresh and -1 is adaptive. GL has
        // no mailbox mode, so it, like an unsupported mode, falls back to waiting.
        if (mode == VsyncMode::Disabled && SDL_GL_SetSwapInterval(0)) {
            vsync_ = VsyncMode::Disabled;
        } else if (mode == VsyncMode::Adaptive && SDL_GL_SetSwapInterval(-1)) {
            vsync_ = VsyncMode::Adaptive;
        } else if (SDL_GL_SetSwapInterval(1)) {
            vsync_ = VsyncMode::Enabled;
        } else {
            log::warn("changing V-Sync failed", {{"error", SDL_GetError()}});
        }
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
        GLuint gl = 0;
        GLuint framebuffer = 0; // made the first time a frame draws into it
        TextureDesc desc;
    };

    struct Unit {
        GLuint texture = 0;
        GLuint sampler = 0;
    };

    // Draws in a row with the same texture and material, sent as one GL draw.
    struct Batch {
        TextureId texture = TextureId::None;
        Material material;
        ShaderKind kind = ShaderKind::Sprite;
        GLuint program = 0;
        // The draw's texture, the material's, the shading's and the frame's lighting textures.
        std::array<Unit, kMaterialSamplers> units{};
        std::array<float, 4> texel{};
        GLint first = 0;
        GLsizei count = 0;
    };

    static std::size_t sampler_index(Filter filter, bool repeat) {
        return (static_cast<std::size_t>(filter) * 2) + (repeat ? 1 : 0);
    }

    // A byte offset into the bound buffer, as GL takes one.
    static const void* offset(std::size_t bytes) {
        return reinterpret_cast<const void*>(bytes); // NOLINT(performance-no-int-to-ptr)
    }

    void make_current() const {
        if (SDL_GL_GetCurrentContext() != context_) {
            SDL_GL_MakeCurrent(window_, context_);
        }
    }

    // GL's errors since the last check, as one error.
    Result<void> check_errors(std::string_view what) const {
        std::string codes;
        for (GLenum code = gl_.get_error(); code != gl::kNoError; code = gl_.get_error()) {
            codes += (codes.empty() ? "" : ", ") + hex(code);
        }
        if (codes.empty()) {
            return {};
        }
        return make_error(ErrorCode::PlatformFailure,
                          std::string(what) + " failed with OpenGL error " + codes);
    }

    // A texture and its sampler; None is white. The texture must exist.
    Unit binding(TextureId texture, bool repeat) const {
        if (texture == TextureId::None) {
            return {.texture = white_,
                    .sampler = samplers_.at(sampler_index(Filter::Linear, repeat))};
        }
        const Texture& found = textures_.at(texture);
        return {.texture = found.gl,
                .sampler = samplers_.at(sampler_index(found.desc.filter, repeat))};
    }

    Result<GLuint> create_gl_texture(const TextureDesc& desc, std::span<const std::uint8_t> rgba) {
        const auto limit = static_cast<std::uint32_t>(std::max(max_texture_size_, 1));
        if (desc.width > limit || desc.height > limit) {
            return make_error(ErrorCode::InvalidArgument,
                              "this GPU's textures are at most " + std::to_string(limit) +
                                  " pixels wide and high, not " + std::to_string(desc.width) + "x" +
                                  std::to_string(desc.height));
        }
        // New GPU memory holds whatever was there before, so it starts as transparent black.
        std::vector<std::uint8_t> zeros;
        if (rgba.empty()) {
            zeros.resize(std::size_t{desc.width} * desc.height * 4);
            rgba = zeros;
        }
        GLuint texture = 0;
        gl_.gen_textures(1, &texture);
        gl_.bind_texture(gl::kTexture2D, texture);
        const auto filter =
            static_cast<GLint>(desc.filter == Filter::Linear ? gl::kLinear : gl::kNearest);
        gl_.tex_parameteri(gl::kTexture2D, gl::kTextureMinFilter, filter);
        gl_.tex_parameteri(gl::kTexture2D, gl::kTextureMagFilter, filter);
        gl_.tex_parameteri(gl::kTexture2D, gl::kTextureWrapS, static_cast<GLint>(gl::kClampToEdge));
        gl_.tex_parameteri(gl::kTexture2D, gl::kTextureWrapT, static_cast<GLint>(gl::kClampToEdge));
        gl_.tex_image_2d(gl::kTexture2D, 0, static_cast<GLint>(gl::kRgba8),
                         static_cast<GLsizei>(desc.width), static_cast<GLsizei>(desc.height), 0,
                         gl::kRgba, gl::kUnsignedByte, rgba.data());
        if (auto checked = check_errors("creating a " + std::to_string(desc.width) + "x" +
                                        std::to_string(desc.height) + " texture");
            !checked) {
            gl_.delete_textures(1, &texture);
            return checked.error();
        }
        return texture;
    }

    void delete_texture(const Texture& texture) const {
        if (texture.framebuffer != 0) {
            gl_.delete_framebuffers(1, &texture.framebuffer);
        }
        if (texture.gl != 0) {
            gl_.delete_textures(1, &texture.gl);
        }
    }

    // A framebuffer that draws into the texture.
    Result<GLuint> attach(Texture& texture) const {
        if (texture.framebuffer != 0) {
            return texture.framebuffer;
        }
        gl_.gen_framebuffers(1, &texture.framebuffer);
        gl_.bind_framebuffer(gl::kFramebuffer, texture.framebuffer);
        gl_.framebuffer_texture_2d(gl::kFramebuffer, gl::kColorAttachment0, gl::kTexture2D,
                                   texture.gl, 0);
        const GLenum status = gl_.check_framebuffer_status(gl::kFramebuffer);
        gl_.bind_framebuffer(gl::kFramebuffer, 0);
        if (status != gl::kFramebufferComplete) {
            return make_error(ErrorCode::PlatformFailure,
                              "OpenGL ES can't draw into the texture: framebuffer status " +
                                  hex(status));
        }
        return texture.framebuffer;
    }

    // The texture window frames draw into, at the window's size, before being copied to it.
    Result<GLuint> present_framebuffer(std::uint32_t width, std::uint32_t height) {
        if (present_.desc.width != width || present_.desc.height != height) {
            delete_texture(present_);
            present_ = {};
            const TextureDesc desc{
                .width = width, .height = height, .filter = Filter::Nearest, .render_target = true};
            auto texture = create_gl_texture(desc, {});
            if (!texture) {
                return texture.error();
            }
            present_ = Texture{.gl = texture.value(), .framebuffer = 0, .desc = desc};
        }
        return attach(present_);
    }

    // The program for a shader, made the first time a frame needs it.
    Result<GLuint> program_for(ShaderKind kind) {
        GLuint& slot = programs_.at(static_cast<std::size_t>(kind));
        if (slot != 0) {
            return slot;
        }
        const bool material = kind == ShaderKind::Material;
        auto program = material ? link(shaders::kMaterialVertexGlsl, shaders::kMaterialFragmentGlsl,
                                       kMaterialSamplerNames, "the material shader")
                                : link(shaders::kSpriteVertexGlsl, shaders::kSpriteFragmentGlsl,
                                       kSpriteSamplers, "the sprite shader");
        if (!program) {
            return program.error();
        }
        slot = program.value();
        return slot;
    }

    // A program made of the two stages, with its uniform blocks and samplers bound where the
    // device puts them.
    Result<GLuint> link(std::string_view vertex_source, std::string_view fragment_source,
                        std::span<const std::string_view> samplers, std::string_view what) {
        auto vertex = compile(gl::kVertexShader, vertex_source);
        if (!vertex) {
            return vertex.error();
        }
        auto fragment = compile(gl::kFragmentShader, fragment_source);
        if (!fragment) {
            gl_.delete_shader(vertex.value());
            return fragment.error();
        }
        const GLuint program = gl_.create_program();
        gl_.attach_shader(program, vertex.value());
        gl_.attach_shader(program, fragment.value());
        gl_.link_program(program);
        gl_.delete_shader(vertex.value());
        gl_.delete_shader(fragment.value());
        GLint linked = 0;
        gl_.get_programiv(program, gl::kLinkStatus, &linked);
        if (linked == 0) {
            const std::string log = program_log(program);
            gl_.delete_program(program);
            return make_error(ErrorCode::PlatformFailure,
                              std::string(what) + " didn't link: " + log);
        }
        for (const auto& [name, binding] :
             {std::pair{"View", kViewBinding}, std::pair{"Program", kProgramBinding},
              std::pair{"Lights", kLightsBinding},
              std::pair{"VertexProgram", kVertexProgramBinding}}) {
            const GLuint index = gl_.get_uniform_block_index(program, name);
            if (index != gl::kInvalidIndex) {
                gl_.uniform_block_binding(program, index, binding);
            }
        }
        gl_.use_program(program);
        for (std::size_t unit = 0; unit < samplers.size(); ++unit) {
            const std::string name(samplers[unit]);
            const GLint location = gl_.get_uniform_location(program, name.c_str());
            if (location >= 0) {
                gl_.uniform_1i(location, static_cast<GLint>(unit));
            }
        }
        gl_.use_program(0);
        return program;
    }

    Result<GLuint> compile(GLenum stage, std::string_view source) const {
        const GLuint shader = gl_.create_shader(stage);
        const GLchar* text = source.data();
        const auto length = static_cast<GLint>(source.size());
        gl_.shader_source(shader, 1, &text, &length);
        gl_.compile_shader(shader);
        GLint compiled = 0;
        gl_.get_shaderiv(shader, gl::kCompileStatus, &compiled);
        if (compiled != 0) {
            return shader;
        }
        GLint size = 0;
        gl_.get_shaderiv(shader, gl::kInfoLogLength, &size);
        std::string log(static_cast<std::size_t>(std::max(size, 1)), '\0');
        gl_.get_shader_info_log(shader, size, nullptr, log.data());
        gl_.delete_shader(shader);
        log.resize(std::strlen(log.c_str()));
        return make_error(ErrorCode::PlatformFailure,
                          std::string(stage == gl::kVertexShader ? "a vertex" : "a fragment") +
                              " shader didn't compile: " + log);
    }

    std::string program_log(GLuint program) const {
        GLint size = 0;
        gl_.get_programiv(program, gl::kInfoLogLength, &size);
        std::string log(static_cast<std::size_t>(std::max(size, 1)), '\0');
        gl_.get_program_info_log(program, size, nullptr, log.data());
        log.resize(std::strlen(log.c_str()));
        return log;
    }

    // Godot's canvas blend modes, for straight-alpha colors.
    void set_blend(BlendMode mode) const {
        switch (mode) {
        case BlendMode::Mix:
            gl_.blend_equation_separate(gl::kFuncAdd, gl::kFuncAdd);
            gl_.blend_func_separate(gl::kSrcAlpha, gl::kOneMinusSrcAlpha, gl::kOne,
                                    gl::kOneMinusSrcAlpha);
            break;
        case BlendMode::Add:
        case BlendMode::Sub: {
            const GLenum op = mode == BlendMode::Sub ? gl::kFuncReverseSubtract : gl::kFuncAdd;
            gl_.blend_equation_separate(op, op);
            gl_.blend_func_separate(gl::kSrcAlpha, gl::kOne, gl::kSrcAlpha, gl::kOne);
            break;
        }
        case BlendMode::Mul:
            gl_.blend_equation_separate(gl::kFuncAdd, gl::kFuncAdd);
            gl_.blend_func_separate(gl::kDstColor, gl::kZero, gl::kDstAlpha, gl::kZero);
            break;
        case BlendMode::PremulAlpha:
            gl_.blend_equation_separate(gl::kFuncAdd, gl::kFuncAdd);
            gl_.blend_func_separate(gl::kOne, gl::kOneMinusSrcAlpha, gl::kOne,
                                    gl::kOneMinusSrcAlpha);
            break;
        }
    }

    // Puts bytes in the frame's uniform data at the next offset GL can bind, and returns it.
    std::size_t place_uniforms(const void* data, std::size_t size) {
        const auto alignment = static_cast<std::size_t>(uniform_alignment_);
        const std::size_t at = (uniform_bytes_.size() + alignment - 1) / alignment * alignment;
        uniform_bytes_.resize(at + size);
        std::memcpy(uniform_bytes_.data() + at, data, size);
        return at;
    }

    // Sends the frame's vertices and uniforms in one buffer each, then draws each batch.
    void draw_batches() {
        gl_.bind_vertex_array(vertex_array_);
        gl_.bind_buffer(gl::kArrayBuffer, vertex_buffer_);
        gl_.buffer_data(gl::kArrayBuffer,
                        static_cast<GLsizeiptr>(vertices_.size() * sizeof(Vertex2D)),
                        vertices_.data(), gl::kStreamDraw);

        // Each batch's view and program, deduplicated in a row, and the frame's lights.
        const Mat4 view = frame_.view.value_or(
            pixel_projection(static_cast<float>(frame_width_), static_cast<float>(frame_height_)));
        uniform_bytes_.clear();
        std::optional<std::size_t> lights_at;
        if (std::ranges::any_of(
                batches_, [](const Batch& batch) { return batch.kind == ShaderKind::Material; })) {
            const auto lights = std::make_unique<LightsUniforms>(lights_uniforms(frame_.lighting));
            lights_at = place_uniforms(lights.get(), sizeof(LightsUniforms));
        }
        struct Placed {
            std::size_t view = 0;
            std::size_t program = 0;
            std::size_t vertex_program = 0;
        };
        std::vector<Placed> placed(batches_.size());
        std::optional<Mat4> previous;
        std::size_t view_at = 0;
        for (std::size_t i = 0; i < batches_.size(); ++i) {
            const Batch& batch = batches_[i];
            const Mat4 matrix = multiply(kFlipY, batch.material.transform
                                                     ? multiply(view, *batch.material.transform)
                                                     : view);
            if (previous != matrix) {
                view_at = place_uniforms(matrix.data(), sizeof(matrix));
                previous = matrix;
            }
            placed[i].view = view_at;
            if (batch.kind == ShaderKind::Material) {
                const ProgramUniforms uniforms = program_uniforms(batch.material, batch.texel);
                placed[i].program = place_uniforms(&uniforms, sizeof(uniforms));
            }
            if (batch.material.shader != ShaderId::None) {
                const VertexProgramUniforms uniforms =
                    vertex_program_uniforms(batch.material, batch.texel);
                placed[i].vertex_program = place_uniforms(&uniforms, sizeof(uniforms));
            }
        }
        gl_.bind_buffer(gl::kUniformBuffer, uniform_buffer_);
        gl_.buffer_data(gl::kUniformBuffer, static_cast<GLsizeiptr>(uniform_bytes_.size()),
                        uniform_bytes_.data(), gl::kStreamDraw);
        if (lights_at) {
            gl_.bind_buffer_range(gl::kUniformBuffer, kLightsBinding, uniform_buffer_,
                                  static_cast<GLintptr>(*lights_at),
                                  static_cast<GLsizeiptr>(sizeof(LightsUniforms)));
        }

        GLuint bound_program = 0;
        std::optional<BlendMode> bound_blend;
        std::optional<std::size_t> bound_view;
        for (std::size_t i = 0; i < batches_.size(); ++i) {
            const Batch& batch = batches_[i];
            if (batch.program != bound_program) {
                gl_.use_program(batch.program);
                bound_program = batch.program;
            }
            if (bound_blend != batch.material.blend) {
                set_blend(batch.material.blend);
                bound_blend = batch.material.blend;
            }
            if (bound_view != placed[i].view) {
                gl_.bind_buffer_range(gl::kUniformBuffer, kViewBinding, uniform_buffer_,
                                      static_cast<GLintptr>(placed[i].view),
                                      static_cast<GLsizeiptr>(sizeof(Mat4)));
                bound_view = placed[i].view;
            }
            std::size_t units = 1;
            if (batch.kind == ShaderKind::Material) {
                gl_.bind_buffer_range(gl::kUniformBuffer, kProgramBinding, uniform_buffer_,
                                      static_cast<GLintptr>(placed[i].program),
                                      static_cast<GLsizeiptr>(sizeof(ProgramUniforms)));
                units = batch.units.size();
            }
            if (batch.material.shader != ShaderId::None) {
                gl_.bind_buffer_range(gl::kUniformBuffer, kVertexProgramBinding, uniform_buffer_,
                                      static_cast<GLintptr>(placed[i].vertex_program),
                                      static_cast<GLsizeiptr>(sizeof(VertexProgramUniforms)));
            }
            for (std::size_t unit = 0; unit < units; ++unit) {
                gl_.active_texture(gl::kTexture0 + static_cast<GLenum>(unit));
                gl_.bind_texture(gl::kTexture2D, batch.units.at(unit).texture);
                gl_.bind_sampler(static_cast<GLuint>(unit), batch.units.at(unit).sampler);
            }
            gl_.draw_arrays(gl::kTriangles, batch.first, batch.count);
        }
        gl_.use_program(0);
        gl_.bind_vertex_array(0);
    }

    void finish_frame() {
        vertices_.clear();
        batches_.clear();
        if (!pending_release_.empty() || !pending_programs_.empty()) {
            make_current();
            release_pending();
        }
    }

    void release_pending() {
        for (const Texture& texture : pending_release_) {
            delete_texture(texture);
        }
        pending_release_.clear();
        for (const GLuint program : pending_programs_) {
            gl_.delete_program(program);
        }
        pending_programs_.clear();
    }

    SDL_Window* window_;
    bool presents_;
    bool owns_video_;
    SDL_GLContext context_ = nullptr;
    Gl gl_;
    std::string renderer_;
    GLint max_texture_size_ = 0;
    GLint uniform_alignment_ = 256;
    std::array<GLuint, 2> programs_{}; // by ShaderKind
    std::array<GLuint, 4> samplers_{}; // by sampler_index
    GLuint white_ = 0;
    GLuint vertex_buffer_ = 0;
    GLuint uniform_buffer_ = 0;
    GLuint vertex_array_ = 0;
    std::unordered_map<TextureId, Texture> textures_;
    std::uint32_t next_texture_ = 1;
    std::vector<Texture> pending_release_;
    std::unordered_map<ShaderId, GLuint> game_programs_;
    std::uint32_t next_shader_ = 1;
    std::vector<GLuint> pending_programs_;
    detail::CpuParticles particles_;
    std::vector<Vertex2D> particle_vertices_;
    Texture present_;

    // The frame being built. A skipped frame is 0 wide.
    bool in_frame_ = false;
    Frame frame_;
    GLuint frame_framebuffer_ = 0;
    std::uint32_t frame_width_ = 0;
    std::uint32_t frame_height_ = 0;
    std::vector<Vertex2D> vertices_;
    std::vector<Batch> batches_;
    bool break_batch_ = false; // the next draw starts a batch
    std::vector<std::uint8_t> uniform_bytes_;
    VsyncMode vsync_ = VsyncMode::Enabled;
    Stats stats_;
};

} // namespace

Result<std::unique_ptr<RenderDevice>> create_gl_device(const DeviceOptions& options) {
    SDL_Window* window = options.window;
    const bool presents = window != nullptr;
    if (presents && (SDL_GetWindowFlags(window) & SDL_WINDOW_OPENGL) == 0) {
        return make_error(ErrorCode::InvalidArgument,
                          "the window wasn't made for OpenGL; create it with SDL_WINDOW_OPENGL");
    }
    if (!presents) {
        if (auto started = init_video(); !started) {
            return started.error();
        }
        request_gl_context();
        window = SDL_CreateWindow("SlashEngine", 1, 1, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
        if (window == nullptr) {
            Error error = sdl_error("OpenGL ES doesn't work here");
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
            return error;
        }
    }
    auto device = std::make_unique<GlDevice>(window, presents, !presents);
    if (auto ready = device->init(); !ready) {
        return ready.error();
    }
    log::info("render device created", {{"backend", backend_name(device->backend())},
                                        {"renderer", device->renderer()},
                                        {"window", presents}});
    return std::unique_ptr<RenderDevice>(std::move(device));
}

} // namespace slash::render::detail
