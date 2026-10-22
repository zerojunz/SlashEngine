#include "gl_device.hpp"

#include "checks.hpp"
#include "cpu_particles.hpp"
#include "gpu_device.hpp"
#include "material_shaders.hpp"
#include "mesh_shaders.hpp"
#include "mesh_shading.hpp"
#include "post_shaders.hpp"
#include "post_shading.hpp"
#include "radiance_shaders.hpp"
#include "shader_code.hpp"
#include "shadow_shaders.hpp"
#include "sky_shaders.hpp"
#include "sky_shading.hpp"
#include "sprite_shaders.hpp"
#include "uniforms.hpp"

#include "slash/core/assert.hpp"
#include "slash/core/log.hpp"
#include "slash/render/image.hpp"

#include <SDL3/SDL.h>

#ifdef __EMSCRIPTEN__
#include <GLES3/gl3.h>
#endif
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <numeric>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace slash::render::detail {
namespace {

// OpenGL's types and the enums the device uses, so it needs no GL headers: the functions come
// from SDL_GL_GetProcAddress.
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
constexpr GLenum kNone = 0;
constexpr GLenum kGreater = 0x0204;
constexpr GLenum kGequal = 0x0206;
constexpr GLenum kFront = 0x0404;
constexpr GLenum kBack = 0x0405;
constexpr GLenum kCcw = 0x0901;
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
constexpr GLenum kUnsignedInt = 0x1405;
constexpr GLenum kFloat = 0x1406;
constexpr GLenum kDepth = 0x1801;
constexpr GLenum kDepthComponent = 0x1902;
constexpr GLenum kRgba = 0x1908;
constexpr GLenum kRenderer = 0x1F01;
constexpr GLenum kVersion = 0x1F02;
constexpr GLenum kExtensions = 0x1F03;
constexpr GLenum kNearest = 0x2600;
constexpr GLenum kLinear = 0x2601;
constexpr GLenum kTextureMagFilter = 0x2800;
constexpr GLenum kTextureMinFilter = 0x2801;
constexpr GLenum kTextureWrapS = 0x2802;
constexpr GLenum kTextureWrapT = 0x2803;
constexpr GLenum kRepeat = 0x2901;
constexpr GLbitfield kColorBufferBit = 0x4000;
constexpr GLenum kRgba8 = 0x8058;
constexpr GLenum kRgb10A2 = 0x8059;
constexpr GLenum kTextureWrapR = 0x8072;
constexpr GLenum kTextureBaseLevel = 0x813C;
constexpr GLenum kTextureMaxLevel = 0x813D;
constexpr GLenum kLinearMipmapLinear = 0x2703;
constexpr GLenum kUnsignedInt2101010Rev = 0x8368;
constexpr GLenum kTextureCubeMap = 0x8513;
constexpr GLenum kTextureCubeMapPositiveX = 0x8515;
constexpr GLenum kTextureCubeMapSeamless = 0x884F;
constexpr GLenum kClampToEdge = 0x812F;
constexpr GLenum kNumExtensions = 0x821D;
constexpr GLenum kTexture0 = 0x84C0;
constexpr GLenum kArrayBuffer = 0x8892;
constexpr GLenum kElementArrayBuffer = 0x8893;
constexpr GLenum kTextureCompareMode = 0x884C;
constexpr GLenum kTextureCompareFunc = 0x884D;
constexpr GLenum kCompareRefToTexture = 0x884E;
constexpr GLenum kStreamDraw = 0x88E0;
constexpr GLenum kStaticDraw = 0x88E4;
constexpr GLenum kUniformBuffer = 0x8A11;
constexpr GLenum kUniformBufferOffsetAlignment = 0x8A34;
constexpr GLenum kFragmentShader = 0x8B30;
constexpr GLenum kVertexShader = 0x8B31;
constexpr GLenum kCompileStatus = 0x8B81;
constexpr GLenum kLinkStatus = 0x8B82;
constexpr GLenum kInfoLogLength = 0x8B84;
constexpr GLenum kReadFramebuffer = 0x8CA8;
constexpr GLenum kDepthComponent32f = 0x8CAC;
constexpr GLenum kDrawFramebuffer = 0x8CA9;
constexpr GLenum kFramebufferComplete = 0x8CD5;
constexpr GLenum kColorAttachment0 = 0x8CE0;
constexpr GLenum kDepthAttachment = 0x8D00;
constexpr GLenum kFramebuffer = 0x8D40;
constexpr GLenum kRenderbuffer = 0x8D41;
constexpr GLenum kCompressedRgbaBptcUnorm = 0x8E8C; // ARB_ and EXT_texture_compression_bptc
constexpr GLenum kCompressedRgbaAstc4x4 = 0x93B0;   // KHR_texture_compression_astc_ldr
constexpr GLuint kInvalidIndex = 0xFFFFFFFF;
} // namespace gl

// Each function the device calls, in both OpenGL 3.3 and OpenGL ES 3.0: X(name after "gl", member,
// result, parameters).
// NOLINTBEGIN(bugprone-macro-parentheses): the arguments are types and parameter lists
#define SLASH_GL_FUNCTIONS(X)                                                                      \
    X(ActiveTexture, active_texture, void, (GLenum))                                               \
    X(AttachShader, attach_shader, void, (GLuint, GLuint))                                         \
    X(BindBuffer, bind_buffer, void, (GLenum, GLuint))                                             \
    X(BindBufferRange, bind_buffer_range, void, (GLenum, GLuint, GLuint, GLintptr, GLsizeiptr))    \
    X(BindFramebuffer, bind_framebuffer, void, (GLenum, GLuint))                                   \
    X(BindRenderbuffer, bind_renderbuffer, void, (GLenum, GLuint))                                 \
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
    X(ClearBufferfv, clear_bufferfv, void, (GLenum, GLint, const GLfloat*))                        \
    X(ClearColor, clear_color, void, (GLfloat, GLfloat, GLfloat, GLfloat))                         \
    X(CompileShader, compile_shader, void, (GLuint))                                               \
    X(CompressedTexImage2D, compressed_tex_image_2d, void,                                         \
      (GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void*))                      \
    X(CompressedTexSubImage2D, compressed_tex_sub_image_2d, void,                                  \
      (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLsizei, const void*))               \
    X(CreateProgram, create_program, GLuint, ())                                                   \
    X(CreateShader, create_shader, GLuint, (GLenum))                                               \
    X(CullFace, cull_face, void, (GLenum))                                                         \
    X(DeleteBuffers, delete_buffers, void, (GLsizei, const GLuint*))                               \
    X(DeleteFramebuffers, delete_framebuffers, void, (GLsizei, const GLuint*))                     \
    X(DeleteProgram, delete_program, void, (GLuint))                                               \
    X(DeleteRenderbuffers, delete_renderbuffers, void, (GLsizei, const GLuint*))                   \
    X(DeleteSamplers, delete_samplers, void, (GLsizei, const GLuint*))                             \
    X(DeleteShader, delete_shader, void, (GLuint))                                                 \
    X(DeleteTextures, delete_textures, void, (GLsizei, const GLuint*))                             \
    X(DeleteVertexArrays, delete_vertex_arrays, void, (GLsizei, const GLuint*))                    \
    X(DepthFunc, depth_func, void, (GLenum))                                                       \
    X(DepthMask, depth_mask, void, (GLboolean))                                                    \
    X(Disable, disable, void, (GLenum))                                                            \
    X(DrawArrays, draw_arrays, void, (GLenum, GLint, GLsizei))                                     \
    X(DrawBuffers, draw_buffers, void, (GLsizei, const GLenum*))                                   \
    X(DrawElements, draw_elements, void, (GLenum, GLsizei, GLenum, const void*))                   \
    X(Enable, enable, void, (GLenum))                                                              \
    X(EnableVertexAttribArray, enable_vertex_attrib_array, void, (GLuint))                         \
    X(FramebufferRenderbuffer, framebuffer_renderbuffer, void, (GLenum, GLenum, GLenum, GLuint))   \
    X(FramebufferTexture2D, framebuffer_texture_2d, void, (GLenum, GLenum, GLenum, GLuint, GLint)) \
    X(FrontFace, front_face, void, (GLenum))                                                       \
    X(GenBuffers, gen_buffers, void, (GLsizei, GLuint*))                                           \
    X(GenFramebuffers, gen_framebuffers, void, (GLsizei, GLuint*))                                 \
    X(GenerateMipmap, generate_mipmap, void, (GLenum))                                             \
    X(GenRenderbuffers, gen_renderbuffers, void, (GLsizei, GLuint*))                               \
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
    X(GetStringi, get_stringi, const GLubyte*, (GLenum, GLuint))                                   \
    X(GetUniformBlockIndex, get_uniform_block_index, GLuint, (GLuint, const GLchar*))              \
    X(GetUniformLocation, get_uniform_location, GLint, (GLuint, const GLchar*))                    \
    X(LinkProgram, link_program, void, (GLuint))                                                   \
    X(PixelStorei, pixel_storei, void, (GLenum, GLint))                                            \
    X(ReadBuffer, read_buffer, void, (GLenum))                                                     \
    X(ReadPixels, read_pixels, void, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*))      \
    X(RenderbufferStorage, renderbuffer_storage, void, (GLenum, GLenum, GLsizei, GLsizei))         \
    X(SamplerParameteri, sampler_parameteri, void, (GLuint, GLenum, GLint))                        \
    X(Scissor, scissor, void, (GLint, GLint, GLsizei, GLsizei))                                    \
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
        return make_error(ErrorCode::PlatformFailure, "OpenGL has no gl" #name);                   \
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
// The mesh shader's.
constexpr GLuint kCameraBinding = 4;
constexpr GLuint kInstanceBinding = 5;
constexpr GLuint kSurfaceBinding = 6;
constexpr GLuint kSceneBinding = 7;
constexpr GLuint kShadowPassBinding = 8;
constexpr GLuint kShadowsBinding = 9;
// The post-processing shader's, and the sky's.
constexpr GLuint kPostBinding = 10;
constexpr GLuint kSkyBinding = 11;
constexpr GLuint kFilterBinding = 12; // the radiance filter's

// The two shaders: plain sprites, and materials that run a program.
enum class ShaderKind : std::uint8_t { Sprite, Material };

// Each shader's samplers by the names SPIRV-Cross gives them, in the units the device binds:
// the order of a batch's textures (see kMaterialSamplers).
constexpr std::array<std::string_view, 1> kSpriteSamplers{"sprite_texture"};
constexpr std::array<std::string_view, kMaterialSamplers> kMaterialSamplerNames{
    "texture0",   "texture1",     "texture2",    "texture3",
    "normal_map", "specular_map", "light_atlas", "shadow_map"};
constexpr std::array<std::string_view, kMeshTextures + 3> kMeshSamplerNames{
    "albedo_texture", "metallic_texture",  "roughness_texture", "emission_texture",
    "normal_texture", "directional_atlas", "positional_atlas",  "radiance_map"};
constexpr std::array<std::string_view, 3> kPostSamplerNames{"source", "glow_texture", "lut"};
constexpr std::array<std::string_view, 1> kSkySamplerNames{"sky_texture"};
constexpr std::array<std::string_view, 1> kRadianceSamplerNames{"source_cube"};

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

// The two APIs the device draws with: desktop OpenGL 3.3 (core profile), and OpenGL ES 3.0, which
// is WebGL 2 on the web.
enum class GlApi : std::uint8_t { Desktop, Es };

std::string_view api_name(GlApi api) {
    return api == GlApi::Desktop ? "OpenGL 3.3" : "OpenGL ES 3.0";
}

// The APIs a driver tries, in order: "opengl" is OpenGL 3.3, falling back to OpenGL ES 3.0 where
// that doesn't start, as Godot's opengl3 driver falls back to GLES; "opengl_es" is only OpenGL
// ES 3.0, which is all the web, Android and iOS have.
std::vector<GlApi> gl_apis(std::string_view driver) {
#if defined(__EMSCRIPTEN__) || defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
    (void)driver;
    return {GlApi::Es};
#else
    if (driver == "opengl_es") {
        return {GlApi::Es};
    }
    return {GlApi::Desktop, GlApi::Es};
#endif
}

void request_gl_context(GlApi api) {
    if (api == GlApi::Desktop) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    } else {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    }
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
}

// Whether a context's GL_VERSION is the API asked for: "OpenGL ES 3.x ..." for OpenGL ES, and
// "major.minor ..." of at least 3.3 for desktop OpenGL ("4.6.0 NVIDIA 560.35", "4.1 Metal - 89").
bool version_matches(GlApi api, std::string_view version) {
    if (api == GlApi::Es) {
        return version.starts_with("OpenGL ES 3");
    }
    const auto digit = [&](std::size_t at) {
        return at < version.size() && version[at] >= '0' && version[at] <= '9';
    };
    if (!digit(0) || version.size() < 3 || version[1] != '.' || !digit(2)) {
        return false;
    }
    const int major = version[0] - '0';
    const int minor = version[2] - '0';
    return major > 3 || (major == 3 && minor >= 3);
}

// The shaders are GLSL ES 3.00. Desktop OpenGL compiles them as GLSL 3.30, which has everything
// they use and takes their precision qualifiers.
constexpr std::string_view kEsVersionLine = "#version 300 es";
constexpr std::string_view kDesktopVersionLine = "#version 330 core";

// Apple's software OpenGL renderer, which Macs without a GPU run (CI's among them), loses
// triangles that cross a screen edge unless each corner's w is 1. 3D shaders get around it
// exactly: a corner in front of the camera is scaled by 1/w, which names the same point and is
// clipped the same, and its outputs are scaled with it and divided back in each fragment by the
// interpolated scale, which gives the perspective-correct values they had. Corners behind the
// camera keep their w. at_w1_vertex rewrites a vertex shader so, and at_w1_fragment the fragment
// shader that reads its outputs.

// The names of a shader's non-flat inputs or outputs, declared as "<keyword> [precision] <type>
// <name>;" lines, and the lines' starts.
struct Varying {
    std::size_t line = 0;
    std::size_t end = 0; // past the line's ';'
    std::string precision;
    std::string type;
    std::string name;
};

std::vector<Varying> varyings(std::string_view source, std::string_view keyword) {
    std::vector<Varying> out;
    std::size_t line = 0;
    while (line < source.size()) {
        const std::size_t next = std::min(source.find('\n', line), source.size());
        const std::string_view text = source.substr(line, next - line);
        if (text.starts_with(keyword) && text.size() > keyword.size() &&
            text[keyword.size()] == ' ' && text.ends_with(';')) {
            std::vector<std::string_view> words;
            std::string_view rest =
                text.substr(keyword.size() + 1, text.size() - keyword.size() - 2);
            while (!rest.empty()) {
                const std::size_t space = std::min(rest.find(' '), rest.size());
                words.push_back(rest.substr(0, space));
                rest.remove_prefix(std::min(space + 1, rest.size()));
            }
            if (words.size() == 2 || words.size() == 3) {
                out.push_back({.line = line,
                               .end = next,
                               .precision = words.size() == 3 ? std::string(words[0]) : "",
                               .type = std::string(words[words.size() - 2]),
                               .name = std::string(words.back())});
            }
        }
        line = next + 1;
    }
    return out;
}

// Declares each varying of a shader under "slash_<name>" and its own name as a global that main
// reads or writes, so the rewrites below can scale it between the two.
std::string renamed(std::string_view source, const std::vector<Varying>& found,
                    std::string_view keyword) {
    std::string out(source);
    for (const Varying& v : std::ranges::reverse_view(found)) {
        const std::string type = (v.precision.empty() ? "" : v.precision + " ") + v.type;
        std::string declared(keyword);
        declared.append(" ").append(type).append(" slash_").append(v.name).append(";\n");
        declared.append(type).append(" ").append(v.name).append(";");
        out.replace(v.line, v.end - v.line, declared);
    }
    return out;
}

std::string at_w1_vertex(std::string_view source) {
    const std::vector<Varying> outputs = varyings(source, "out");
    std::string out = renamed(source, outputs, "out");
    std::string scaling = "    slash_scale = gl_Position.w > 1.0e-3 ? 1.0 / gl_Position.w : 1.0;\n"
                          "    gl_Position *= slash_scale;\n";
    for (const Varying& v : outputs) {
        scaling += "    slash_" + v.name + " = " + v.name + " * slash_scale;\n";
    }
    const std::size_t end = out.rfind('}');
    SLASH_ASSERT(end != std::string::npos, "a vertex shader has a main function");
    out.insert(end, scaling);
    const std::size_t main = out.find("void main()");
    SLASH_ASSERT(main != std::string::npos, "a vertex shader has a main function");
    out.insert(main, "out highp float slash_scale;\n");
    return out;
}

std::string at_w1_fragment(std::string_view source) {
    const std::vector<Varying> inputs = varyings(source, "in");
    std::string out = renamed(source, inputs, "in");
    std::string dividing;
    for (const Varying& v : inputs) {
        dividing += "    " + v.name + " = slash_" + v.name + " / slash_scale;\n";
    }
    const std::size_t main = out.find("void main()");
    SLASH_ASSERT(main != std::string::npos, "a fragment shader has a main function");
    out.insert(out.find('{', main) + 2, dividing);
    out.insert(main, "in highp float slash_scale;\n");
    return out;
}

class GlDevice final : public RenderDevice {
public:
    using RenderDevice::draw;

    // window is the game's when presents is set, or else a hidden one the device owns. apis are
    // the ones to try, in order.
    GlDevice(SDL_Window* window, bool presents, bool owns_video, std::vector<GlApi> apis)
        : window_(window), presents_(presents), owns_video_(owns_video), apis_(std::move(apis)) {}

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
                for (const auto& [id, mesh] : meshes_) {
                    delete_mesh(mesh);
                }
                if (mesh_program_ != 0) {
                    gl_.delete_program(mesh_program_);
                }
                if (shadow_program_ != 0) {
                    gl_.delete_program(shadow_program_);
                }
                for (const GlShadowAtlas& atlas :
                     {shadow_atlases_[0], shadow_atlases_[1], no_shadows_}) {
                    if (atlas.texture != 0) {
                        gl_.delete_textures(1, &atlas.texture);
                    }
                }
                if (shadow_sampler_ != 0) {
                    gl_.delete_samplers(1, &shadow_sampler_);
                    gl_.delete_framebuffers(1, &shadow_framebuffer_);
                }
                if (depth_ != 0) {
                    gl_.delete_renderbuffers(1, &depth_);
                }
                release_post_targets();
                release_pending();
                if (post_program_ != 0) {
                    gl_.delete_program(post_program_);
                    gl_.delete_vertex_arrays(1, &post_vertex_array_);
                }
                if (sky_program_ != 0) {
                    gl_.delete_program(sky_program_);
                    gl_.delete_vertex_arrays(1, &sky_vertex_array_);
                }
                if (radiance_program_ != 0) {
                    gl_.delete_program(radiance_program_);
                }
                for (const GLuint cube : {radiance_.raw, radiance_.filtered, no_radiance_}) {
                    if (cube != 0) {
                        gl_.delete_textures(1, &cube);
                    }
                }
                if (radiance_sampler_ != 0) {
                    gl_.delete_samplers(1, &radiance_sampler_);
                    gl_.delete_framebuffers(1, &radiance_framebuffer_);
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
        std::string failures;
        for (const GlApi api : apis_) {
            auto started = start_context(api);
            if (started) {
                break;
            }
            failures += (failures.empty() ? "" : "; ") + started.error().message;
        }
        if (context_ == nullptr) {
            return make_error(ErrorCode::PlatformFailure, failures);
        }
        const auto* renderer = reinterpret_cast<const char*>(gl_.get_string(gl::kRenderer));
        renderer_ = renderer != nullptr ? renderer : "";
        gl_.get_integerv(gl::kMaxTextureSize, &max_texture_size_);
        find_compressed_formats();
        gl_.get_integerv(gl::kUniformBufferOffsetAlignment, &uniform_alignment_);
        uniform_alignment_ = std::max(uniform_alignment_, 16);

        gl_.disable(gl::kDither);
        if (api_ == GlApi::Desktop) {
            // Cube textures filter across their faces' edges, as they always do in OpenGL ES.
            gl_.enable(gl::kTextureCubeMapSeamless);
        }
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
        return check_errors("starting " + std::string(api_name(api_)));
    }

    [[nodiscard]] Backend backend() const override {
        return api_ == GlApi::Desktop ? Backend::OpenGl : Backend::OpenGlEs;
    }
    [[nodiscard]] const std::string& renderer() const { return renderer_; }
    [[nodiscard]] const std::string& version() const { return version_; }

    [[nodiscard]] bool supports(TextureFormat format) const override {
        return sampled_.at(static_cast<std::size_t>(format));
    }

    Result<TextureId> create_texture(const TextureDesc& desc,
                                     std::span<const std::uint8_t> texels) override {
        if (auto checked = check_texture_desc(desc); !checked) {
            return checked.error();
        }
        if (auto checked = check_pixels(desc, texels, true); !checked) {
            return checked.error();
        }
        // A format the GPU doesn't sample is kept as RGBA8.
        TextureDesc stored = desc;
        std::vector<std::uint8_t> decoded;
        if (!supports(desc.format)) {
            stored.format = TextureFormat::Rgba8;
            auto pixels = decode_texels(desc.format, desc.width, desc.height, texels);
            if (!pixels) {
                return pixels.error();
            }
            decoded = std::move(pixels.value());
            texels = decoded;
        }
        make_current();
        auto gl_texture = create_gl_texture(stored, texels);
        if (!gl_texture) {
            return gl_texture.error();
        }
        const auto id = static_cast<TextureId>(next_texture_++);
        textures_[id] = Texture{
            .gl = gl_texture.value(), .framebuffer = 0, .desc = desc, .stored = stored.format};
        return id;
    }

    Result<void> update_texture(TextureId texture, std::span<const std::uint8_t> texels) override {
        const auto it = textures_.find(texture);
        if (it == textures_.end()) {
            return unknown_texture(texture);
        }
        const TextureDesc& desc = it->second.desc;
        if (auto checked = check_pixels(desc, texels, false); !checked) {
            return checked;
        }
        std::vector<std::uint8_t> decoded;
        if (it->second.stored != desc.format) {
            auto pixels = decode_texels(desc.format, desc.width, desc.height, texels);
            if (!pixels) {
                return pixels.error();
            }
            decoded = std::move(pixels.value());
            texels = decoded;
        }
        if (radiance_.key && radiance_.key->texture == texture) {
            radiance_.key.reset(); // drawn from the old texels
        }
        make_current();
        // Draws are sent at end_frame, so every draw of the frame being built sees the new pixels.
        gl_.bind_texture(gl::kTexture2D, it->second.gl);
        const auto width = static_cast<GLsizei>(desc.width);
        const auto height = static_cast<GLsizei>(desc.height);
        if (is_compressed(it->second.stored)) {
            gl_.compressed_tex_sub_image_2d(gl::kTexture2D, 0, 0, 0, width, height,
                                            gl_format(it->second.stored),
                                            static_cast<GLsizei>(texels.size()), texels.data());
        } else {
            gl_.tex_sub_image_2d(gl::kTexture2D, 0, 0, 0, width, height, gl::kRgba,
                                 gl::kUnsignedByte, texels.data());
        }
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

    Result<ParticlesId> create_particles(std::uint32_t amount, std::uint32_t trail) override {
        return particles_.create(amount, trail);
    }

    void destroy_particles(ParticlesId particles) override { particles_.destroy(particles); }

    Result<void> process_particle_chain(std::span<const ParticleRun> chain) override {
        return particles_.process(chain);
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

    Result<MeshId> create_mesh(std::span<const Vertex3D> vertices,
                               std::span<const std::uint32_t> indices) override {
        if (auto checked = check_mesh(vertices, indices); !checked) {
            return checked.error();
        }
        std::vector<std::uint32_t> implicit;
        if (indices.empty()) {
            implicit.resize(vertices.size());
            std::iota(implicit.begin(), implicit.end(), 0U);
            indices = implicit;
        }
        make_current();
        GlMesh mesh{.index_count = static_cast<GLsizei>(indices.size()),
                    .bounds = mesh_bounds(vertices)};
        gl_.gen_vertex_arrays(1, &mesh.vertex_array);
        gl_.gen_buffers(1, &mesh.vertices);
        gl_.gen_buffers(1, &mesh.indices);
        gl_.bind_vertex_array(mesh.vertex_array);
        gl_.bind_buffer(gl::kArrayBuffer, mesh.vertices);
        gl_.buffer_data(gl::kArrayBuffer, static_cast<GLsizeiptr>(vertices.size_bytes()),
                        vertices.data(), gl::kStaticDraw);
        constexpr auto kStride = static_cast<GLsizei>(sizeof(Vertex3D));
        gl_.vertex_attrib_pointer(0, 3, gl::kFloat, gl::kFalse, kStride,
                                  offset(offsetof(Vertex3D, position)));
        gl_.vertex_attrib_pointer(1, 3, gl::kFloat, gl::kFalse, kStride,
                                  offset(offsetof(Vertex3D, normal)));
        gl_.vertex_attrib_pointer(2, 4, gl::kFloat, gl::kFalse, kStride,
                                  offset(offsetof(Vertex3D, tangent)));
        gl_.vertex_attrib_pointer(3, 2, gl::kFloat, gl::kFalse, kStride,
                                  offset(offsetof(Vertex3D, uv)));
        gl_.vertex_attrib_pointer(4, 4, gl::kUnsignedByte, gl::kTrue, kStride,
                                  offset(offsetof(Vertex3D, rgba)));
        for (GLuint location = 0; location < 5; ++location) {
            gl_.enable_vertex_attrib_array(location);
        }
        // The vertex array keeps the index buffer bound to it.
        gl_.bind_buffer(gl::kElementArrayBuffer, mesh.indices);
        gl_.buffer_data(gl::kElementArrayBuffer, static_cast<GLsizeiptr>(indices.size_bytes()),
                        indices.data(), gl::kStaticDraw);
        gl_.bind_vertex_array(0);
        if (auto checked = check_errors("creating a mesh"); !checked) {
            delete_mesh(mesh);
            return checked.error();
        }
        const auto id = static_cast<MeshId>(next_mesh_++);
        meshes_[id] = mesh;
        return id;
    }

    void destroy_mesh(MeshId mesh) override {
        const auto it = meshes_.find(mesh);
        if (it == meshes_.end()) {
            return;
        }
        if (in_frame_) {
            // The frame's draws still read it when it is sent.
            pending_meshes_.push_back(it->second);
        } else {
            make_current();
            delete_mesh(it->second);
        }
        meshes_.erase(it);
    }

    Result<void> draw_mesh(MeshId mesh, const Mat4& transform, const Material3D& material,
                           std::uint32_t layers, ShadowCasting shadows) override {
        if (!in_frame_) {
            return no_frame("draw_mesh");
        }
        const auto it = meshes_.find(mesh);
        if (auto checked = check_mesh_draw(frame_.scene.has_value(), it != meshes_.end(), mesh,
                                           transform, material, frame_.target,
                                           [&](TextureId id) { return textures_.contains(id); });
            !checked) {
            return checked;
        }
        if (frame_width_ == 0 || !frame_.scene) {
            return {}; // a skipped frame (check_mesh_draw refused one with no scene)
        }
        // Made at the first draw that needs them, so a failure shows there.
        make_current();
        if (auto program = mesh_program(); !program) {
            return program.error();
        }
        if (auto depth = depth_buffer(frame_width_, frame_height_); !depth) {
            return depth.error();
        }
        if (auto ready = prepare_shadows(*frame_.scene); !ready) {
            return ready;
        }
        if (auto ready = prepare_radiance(*frame_.scene); !ready) {
            return ready;
        }
        MeshBatch batch{.mesh = it->second, .units = {}};
        const std::array<const MaterialTexture*, kMeshTextures> slots{
            &material.albedo_texture, &material.metallic_texture, &material.roughness_texture,
            &material.emission_texture, &material.normal_texture};
        for (std::size_t slot = 0; slot < kMeshTextures; ++slot) {
            batch.units.at(slot) = binding(slots.at(slot)->texture, slots.at(slot)->repeat);
        }
        mesh_draws_.push_back({.mesh = mesh,
                               .transform = transform,
                               .material = material,
                               .layers = layers,
                               .shadows = shadows});
        mesh_batches_.push_back(batch);
        ++stats_.draws;
        stats_.triangles += static_cast<std::uint64_t>(it->second.index_count / 3);
        return {};
    }

    Result<bool> begin_frame(const Frame& frame) override {
        if (in_frame_) {
            return frame_in_progress("begin_frame");
        }
        if (frame.scene) {
            if (auto checked = check_scene(*frame.scene); !checked) {
                return checked.error();
            }
            if (auto checked = check_post(
                    frame.scene->post, [&](TextureId id) { return textures_.contains(id); },
                    [&](TextureId id) {
                        const TextureDesc& desc = textures_.at(id).desc;
                        return std::pair{desc.width, desc.height};
                    });
                !checked) {
                return checked.error();
            }
            if (auto checked =
                    check_sky(*frame.scene, [&](TextureId id) { return textures_.contains(id); });
                !checked) {
                return checked.error();
            }
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
            if (frame.scene && post_processes(frame.scene->post)) {
                if (auto prepared = prepare_post(width, height); !prepared) {
                    return prepared.error();
                }
            }
            if (frame.scene && sky_draws(*frame.scene)) {
                if (auto prepared = prepare_sky(); !prepared) {
                    return prepared.error();
                }
            }
        }
        in_frame_ = true;
        frame_ = frame;
        frame_framebuffer_ = framebuffer;
        frame_width_ = width;
        frame_height_ = height;
        vertices_.clear();
        batches_.clear();
        mesh_draws_.clear();
        mesh_batches_.clear();
        shadow_plan_.reset();
        radiance_prepared_ = false;
        radiance_draws_ = false;
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
        if (radiance_draws_ && frame_.scene) {
            draw_radiance(*frame_.scene);
        }
        // A scene that post-processes draws its meshes into a texture first, which the post
        // passes draw over the frame's framebuffer.
        const bool post = frame_.scene && post_processes(frame_.scene->post);
        gl_.bind_framebuffer(gl::kFramebuffer,
                             post ? post_targets_.world.framebuffer : frame_framebuffer_);
        gl_.viewport(0, 0, static_cast<GLsizei>(frame_width_), static_cast<GLsizei>(frame_height_));
        gl_.clear_color(frame_.clear.r, frame_.clear.g, frame_.clear.b, frame_.clear.a);
        gl_.clear(gl::kColorBufferBit);
        // The sky and meshes draw first, and the 2D draws go over them.
        // Mesh draws need a scene, and the first planned the shadows, so a frame with them has
        // both.
        if (frame_.scene && sky_draws(*frame_.scene)) {
            draw_sky(*frame_.scene);
        }
        if (frame_.scene && shadow_plan_ && !mesh_draws_.empty()) {
            draw_meshes(*frame_.scene, *shadow_plan_);
        }
        if (post) {
            draw_post(frame_.scene->post);
        }
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
        for (const auto& [id, texture] : textures_) {
            TextureDesc stored = texture.desc;
            stored.format = texture.stored;
            stats.texture_bytes += texture_bytes(stored);
        }
        return stats;
    }

private:
    struct Texture {
        GLuint gl = 0;
        GLuint framebuffer = 0; // made the first time a frame draws into it
        // As it was made, in the format update_texture takes.
        TextureDesc desc;
        // The format on the GPU: desc's, or RGBA8 where the GPU doesn't sample that.
        TextureFormat stored = TextureFormat::Rgba8;
    };

    static GLenum gl_format(TextureFormat format) {
        return format == TextureFormat::Bc7 ? gl::kCompressedRgbaBptcUnorm
                                            : gl::kCompressedRgbaAstc4x4;
    }

    // Which compressed formats the context samples, by its extensions: OpenGL's and OpenGL ES's
    // names, or WebGL's, which Emscripten lists with and without "GL_" in front.
    void find_compressed_formats() {
        GLint count = 0;
        gl_.get_integerv(gl::kNumExtensions, &count);
        for (GLint i = 0; i < count; ++i) {
            const auto* name = reinterpret_cast<const char*>(
                gl_.get_stringi(gl::kExtensions, static_cast<GLuint>(i)));
            if (name == nullptr) {
                continue;
            }
            std::string_view extension(name);
            if (extension.starts_with("GL_")) {
                extension.remove_prefix(3);
            }
            if (extension == "EXT_texture_compression_bptc" ||
                extension == "ARB_texture_compression_bptc") {
                sampled_.at(static_cast<std::size_t>(TextureFormat::Bc7)) = true;
            }
            if (extension == "KHR_texture_compression_astc_ldr" ||
                extension == "WEBGL_compressed_texture_astc") {
                sampled_.at(static_cast<std::size_t>(TextureFormat::Astc4x4)) = true;
            }
        }
        // Reading past the list leaves an error the next check would report.
        (void)gl_.get_error();
    }

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

    // A mesh's vertex array, which holds its buffers.
    struct GlMesh {
        GLuint vertex_array = 0;
        GLuint vertices = 0;
        GLuint indices = 0;
        GLsizei index_count = 0;
        Bounds bounds;
    };

    // A shadow atlas: a depth texture the shadow passes draw into and the mesh pass samples.
    struct GlShadowAtlas {
        GLuint texture = 0;
        std::uint32_t size = 0;
    };

    // What a mesh draw binds, found when it is made: its mesh may be destroyed before end_frame.
    struct MeshBatch {
        GlMesh mesh;
        std::array<Unit, kMeshTextures> units{};
    };

    static std::size_t sampler_index(Filter filter, bool repeat) {
        return (static_cast<std::size_t>(filter) * 2) + (repeat ? 1 : 0);
    }

    // A byte offset into the bound buffer, as GL takes one.
    static const void* offset(std::size_t bytes) {
        return reinterpret_cast<const void*>(bytes); // NOLINT(performance-no-int-to-ptr)
    }

    // A context of the API, with its functions, or an error naming the API. A context that
    // starts but isn't the API asked for is let go.
    Result<void> start_context(GlApi api) {
        request_gl_context(api);
        context_ = SDL_GL_CreateContext(window_);
        if (context_ == nullptr) {
            return sdl_error(std::string(api_name(api)) + " didn't start");
        }
        auto loaded = load_gl();
        const auto* version =
            loaded ? reinterpret_cast<const char*>(loaded.value().get_string(gl::kVersion))
                   : nullptr;
        if (!loaded || version == nullptr || !version_matches(api, version)) {
            const std::string why = !loaded ? loaded.error().message
                                            : std::string("the context is ") +
                                                  (version != nullptr ? version : "unknown");
            SDL_GL_DestroyContext(context_);
            context_ = nullptr;
            return make_error(ErrorCode::PlatformFailure,
                              std::string(api_name(api)) + " didn't start: " + why);
        }
        gl_ = loaded.value();
        api_ = api;
        version_ = version;
        return {};
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

    // A texture of desc's format, filled with texels in it or, for RGBA8, transparent black.
    Result<GLuint> create_gl_texture(const TextureDesc& desc,
                                     std::span<const std::uint8_t> texels) {
        const auto limit = static_cast<std::uint32_t>(std::max(max_texture_size_, 1));
        if (desc.width > limit || desc.height > limit) {
            return make_error(ErrorCode::InvalidArgument,
                              "this GPU's textures are at most " + std::to_string(limit) +
                                  " pixels wide and high, not " + std::to_string(desc.width) + "x" +
                                  std::to_string(desc.height));
        }
        // New GPU memory holds whatever was there before, so it starts as transparent black.
        std::vector<std::uint8_t> zeros;
        if (texels.empty()) {
            zeros.resize(static_cast<std::size_t>(texture_bytes(desc)));
            texels = zeros;
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
        const auto width = static_cast<GLsizei>(desc.width);
        const auto height = static_cast<GLsizei>(desc.height);
        if (is_compressed(desc.format)) {
            gl_.compressed_tex_image_2d(gl::kTexture2D, 0, gl_format(desc.format), width, height, 0,
                                        static_cast<GLsizei>(texels.size()), texels.data());
        } else {
            gl_.tex_image_2d(gl::kTexture2D, 0, static_cast<GLint>(gl::kRgba8), width, height, 0,
                             gl::kRgba, gl::kUnsignedByte, texels.data());
        }
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
                              "OpenGL can't draw into the texture: framebuffer status " +
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
              std::pair{"VertexProgram", kVertexProgramBinding},
              std::pair{"Camera", kCameraBinding}, std::pair{"Instance", kInstanceBinding},
              std::pair{"Surface", kSurfaceBinding}, std::pair{"Scene", kSceneBinding},
              std::pair{"LightView", kShadowPassBinding}, std::pair{"Shadows", kShadowsBinding},
              std::pair{"Post", kPostBinding}, std::pair{"Sky", kSkyBinding},
              std::pair{"Filter", kFilterBinding}}) {
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
        std::string desktop;
        if (api_ == GlApi::Desktop && source.starts_with(kEsVersionLine)) {
            desktop = std::string(kDesktopVersionLine) +
                      std::string(source.substr(kEsVersionLine.size()));
            source = desktop;
        }
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

    void delete_mesh(const GlMesh& mesh) const {
        gl_.delete_vertex_arrays(1, &mesh.vertex_array);
        gl_.delete_buffers(1, &mesh.vertices);
        gl_.delete_buffers(1, &mesh.indices);
    }

    // The mesh program, made the first time a frame draws a mesh.
    Result<GLuint> mesh_program() {
        if (mesh_program_ != 0) {
            return mesh_program_;
        }
        auto program = link(at_w1_vertex(joined(shaders::kMeshVertexGlsl)),
                            at_w1_fragment(joined(shaders::kMeshFragmentGlsl)), kMeshSamplerNames,
                            "the mesh shader");
        if (!program) {
            return program.error();
        }
        mesh_program_ = program.value();
        return mesh_program_;
    }

    // The depth buffer meshes draw over, made again when the target's size changes. It is
    // attached to the frame's framebuffer only while the meshes draw.
    Result<GLuint> depth_buffer(std::uint32_t width, std::uint32_t height) {
        if (depth_ != 0 && depth_width_ == width && depth_height_ == height) {
            return depth_;
        }
        if (depth_ == 0) {
            gl_.gen_renderbuffers(1, &depth_);
        }
        gl_.bind_renderbuffer(gl::kRenderbuffer, depth_);
        gl_.renderbuffer_storage(gl::kRenderbuffer, gl::kDepthComponent32f,
                                 static_cast<GLsizei>(width), static_cast<GLsizei>(height));
        gl_.bind_renderbuffer(gl::kRenderbuffer, 0);
        if (auto checked = check_errors("creating the depth buffer"); !checked) {
            gl_.delete_renderbuffers(1, &depth_);
            depth_ = 0;
            return checked.error();
        }
        depth_width_ = width;
        depth_height_ = height;
        return depth_;
    }

    // The frame's shadow plan, and the atlases, program, sampler and framebuffer it draws with,
    // made at the frame's first mesh draw so a failure shows there.
    Result<void> prepare_shadows(const Scene3D& scene) {
        if (shadow_plan_) {
            return {};
        }
        if (shadow_sampler_ == 0) {
            // Each tap compares four texels and blends the results, as Godot's shadow sampler.
            gl_.gen_samplers(1, &shadow_sampler_);
            for (const auto& [name, value] :
                 {std::pair{gl::kTextureMinFilter, gl::kLinear},
                  std::pair{gl::kTextureMagFilter, gl::kLinear},
                  std::pair{gl::kTextureWrapS, gl::kClampToEdge},
                  std::pair{gl::kTextureWrapT, gl::kClampToEdge},
                  std::pair{gl::kTextureCompareMode, gl::kCompareRefToTexture},
                  std::pair{gl::kTextureCompareFunc, gl::kGequal}}) {
                gl_.sampler_parameteri(shadow_sampler_, name, static_cast<GLint>(value));
            }
            gl_.gen_framebuffers(1, &shadow_framebuffer_);
        }
        if (no_shadows_.texture == 0) {
            auto made = make_shadow_atlas(1);
            if (!made) {
                return made.error();
            }
            no_shadows_ = {.texture = made.value(), .size = 1};
        }
        // The largest power of two the context's textures reach.
        std::uint32_t limit = 1;
        while (limit < kMaxTextureSize && static_cast<GLint>(limit * 2) <= max_texture_size_) {
            limit *= 2;
        }
        ShadowPlan plan = plan_shadows(scene, limit);
        const std::array<std::uint32_t, 2> sizes{plan.directional_size, plan.positional_size};
        for (std::size_t i = 0; i < 2; ++i) {
            GlShadowAtlas& atlas = shadow_atlases_.at(i);
            if (sizes.at(i) == 0 || atlas.size == sizes.at(i)) {
                continue;
            }
            if (atlas.texture != 0) {
                gl_.delete_textures(1, &atlas.texture);
                atlas = {};
            }
            auto made = make_shadow_atlas(sizes.at(i));
            if (!made) {
                return made.error();
            }
            atlas = {.texture = made.value(), .size = sizes.at(i)};
        }
        if (!plan.passes.empty() && shadow_program_ == 0) {
            const std::array<std::string_view, 1> samplers{"albedo_texture"};
            auto program =
                link(at_w1_vertex(shaders::kShadowVertexGlsl),
                     at_w1_fragment(shaders::kShadowFragmentGlsl), samplers, "the shadow shader");
            if (!program) {
                return program.error();
            }
            shadow_program_ = program.value();
        }
        shadow_plan_ = std::move(plan);
        return {};
    }

    Result<GLuint> make_shadow_atlas(std::uint32_t size) {
        GLuint texture = 0;
        gl_.gen_textures(1, &texture);
        gl_.bind_texture(gl::kTexture2D, texture);
        gl_.tex_image_2d(gl::kTexture2D, 0, static_cast<GLint>(gl::kDepthComponent32f),
                         static_cast<GLsizei>(size), static_cast<GLsizei>(size), 0,
                         gl::kDepthComponent, gl::kFloat, nullptr);
        gl_.bind_texture(gl::kTexture2D, 0);
        if (auto checked = check_errors("creating a shadow atlas"); !checked) {
            gl_.delete_textures(1, &texture);
            return checked.error();
        }
        return texture;
    }

    // The frame's shadow passes, each atlas cleared and drawn through the shadow framebuffer.
    // They draw upside down, as frames do (see kFlipY), so the atlas's rows are the other
    // devices'; each rect's first row is GL's bottom, and front faces turn counterclockwise.
    void draw_shadows(const ShadowPlan& plan, std::span<const std::size_t> pass_uniforms,
                      std::span<const std::pair<std::size_t, std::size_t>> placed) {
        gl_.bind_framebuffer(gl::kFramebuffer, shadow_framebuffer_);
        const GLenum none = gl::kNone;
        gl_.draw_buffers(1, &none);
        gl_.read_buffer(gl::kNone);
        gl_.use_program(shadow_program_);
        gl_.disable(gl::kBlend);
        gl_.enable(gl::kDepthTest);
        gl_.depth_func(gl::kGreater);
        gl_.depth_mask(gl::kTrue);
        gl_.front_face(gl::kCcw);
        gl_.enable(gl::kScissorTest);
        for (std::size_t atlas = 0; atlas < 2; ++atlas) {
            const bool positional = atlas == 1;
            if (std::ranges::none_of(plan.passes, [&](const ShadowPass& pass) {
                    return pass.positional == positional;
                })) {
                continue;
            }
            const GlShadowAtlas& target = shadow_atlases_.at(atlas);
            gl_.framebuffer_texture_2d(gl::kFramebuffer, gl::kDepthAttachment, gl::kTexture2D,
                                       target.texture, 0);
            const auto size = static_cast<GLsizei>(target.size);
            gl_.viewport(0, 0, size, size);
            gl_.scissor(0, 0, size, size);
            constexpr GLfloat kFar = 0.0F;
            gl_.clear_bufferfv(gl::kDepth, 0, &kFar);
            for (std::size_t p = 0; p < plan.passes.size(); ++p) {
                const ShadowPass& pass = plan.passes[p];
                if (pass.positional != positional) {
                    continue;
                }
                const auto x = static_cast<GLint>(pass.rect.x);
                const auto y = static_cast<GLint>(pass.rect.y);
                const auto w = static_cast<GLsizei>(pass.rect.width);
                const auto h = static_cast<GLsizei>(pass.rect.height);
                gl_.viewport(x, y, w, h);
                gl_.scissor(x, y, w, h);
                gl_.bind_buffer_range(gl::kUniformBuffer, kShadowPassBinding, uniform_buffer_,
                                      static_cast<GLintptr>(pass_uniforms[p]),
                                      static_cast<GLsizeiptr>(sizeof(MeshShadowPassUniforms)));
                for (std::size_t i = 0; i < mesh_draws_.size(); ++i) {
                    const MeshDraw& draw = mesh_draws_[i];
                    const MeshBatch& batch = mesh_batches_[i];
                    if (!shadow_pass_draws(pass, draw, batch.mesh.bounds)) {
                        continue;
                    }
                    set_cull(shadow_cull(pass, draw));
                    gl_.bind_buffer_range(gl::kUniformBuffer, kInstanceBinding, uniform_buffer_,
                                          static_cast<GLintptr>(placed[i].first),
                                          static_cast<GLsizeiptr>(sizeof(MeshInstanceUniforms)));
                    gl_.bind_buffer_range(gl::kUniformBuffer, kSurfaceBinding, uniform_buffer_,
                                          static_cast<GLintptr>(placed[i].second),
                                          static_cast<GLsizeiptr>(sizeof(MeshSurfaceUniforms)));
                    gl_.active_texture(gl::kTexture0);
                    gl_.bind_texture(gl::kTexture2D, batch.units[0].texture);
                    gl_.bind_sampler(0, batch.units[0].sampler);
                    gl_.bind_vertex_array(batch.mesh.vertex_array);
                    gl_.draw_elements(gl::kTriangles, batch.mesh.index_count, gl::kUnsignedInt,
                                      nullptr);
                }
            }
        }
        gl_.framebuffer_texture_2d(gl::kFramebuffer, gl::kDepthAttachment, gl::kTexture2D, 0, 0);
        gl_.disable(gl::kScissorTest);
        gl_.bind_framebuffer(gl::kFramebuffer, frame_framebuffer_);
        gl_.viewport(0, 0, static_cast<GLsizei>(frame_width_), static_cast<GLsizei>(frame_height_));
    }

    void set_cull(CullMode cull) const {
        if (cull == CullMode::Disabled) {
            gl_.disable(gl::kCullFace);
        } else {
            gl_.enable(gl::kCullFace);
            gl_.cull_face(cull == CullMode::Back ? gl::kBack : gl::kFront);
        }
    }

    // The frame's meshes: first into the shadow atlases, then in the order mesh_draw_order gives,
    // over the depth buffer cleared to the far plane. Frames draw upside down (see kFlipY), which
    // turns the clockwise front faces counterclockwise.
    void draw_meshes(const Scene3D& scene, const ShadowPlan& plan) {
        uniform_bytes_.clear();
        MeshCameraUniforms camera = mesh_camera_uniforms(scene);
        camera.view_projection = multiply(kFlipY, camera.view_projection);
        const std::size_t camera_at = place_uniforms(&camera, sizeof(camera));
        const auto lights = std::make_unique<MeshSceneUniforms>(mesh_scene_uniforms(scene));
        const std::size_t scene_at = place_uniforms(lights.get(), sizeof(MeshSceneUniforms));
        const std::vector<std::size_t> order =
            mesh_draw_order(std::span<const MeshDraw>(mesh_draws_), scene,
                            [&](std::size_t at) { return mesh_batches_[at].mesh.bounds; });
        const std::size_t shadows_at =
            place_uniforms(plan.uniforms.get(), sizeof(MeshShadowUniforms));
        std::vector<std::size_t> pass_uniforms;
        for (const ShadowPass& pass : plan.passes) {
            MeshShadowPassUniforms uniforms = pass.uniforms;
            uniforms.projection = multiply(kFlipY, uniforms.projection);
            pass_uniforms.push_back(place_uniforms(&uniforms, sizeof(uniforms)));
        }
        std::vector<std::pair<std::size_t, std::size_t>> placed(mesh_draws_.size());
        for (std::size_t i = 0; i < mesh_draws_.size(); ++i) {
            const MeshInstanceUniforms instance =
                mesh_instance_uniforms(mesh_draws_[i].transform, mesh_draws_[i].material);
            const MeshSurfaceUniforms surface =
                mesh_surface_uniforms(mesh_draws_[i].material, mesh_draws_[i].layers);
            placed[i] = {place_uniforms(&instance, sizeof(instance)),
                         place_uniforms(&surface, sizeof(surface))};
        }
        gl_.bind_buffer(gl::kUniformBuffer, uniform_buffer_);
        gl_.buffer_data(gl::kUniformBuffer, static_cast<GLsizeiptr>(uniform_bytes_.size()),
                        uniform_bytes_.data(), gl::kStreamDraw);
        gl_.bind_buffer_range(gl::kUniformBuffer, kCameraBinding, uniform_buffer_,
                              static_cast<GLintptr>(camera_at),
                              static_cast<GLsizeiptr>(sizeof(MeshCameraUniforms)));
        gl_.bind_buffer_range(gl::kUniformBuffer, kSceneBinding, uniform_buffer_,
                              static_cast<GLintptr>(scene_at),
                              static_cast<GLsizeiptr>(sizeof(MeshSceneUniforms)));
        gl_.bind_buffer_range(gl::kUniformBuffer, kShadowsBinding, uniform_buffer_,
                              static_cast<GLintptr>(shadows_at),
                              static_cast<GLsizeiptr>(sizeof(MeshShadowUniforms)));
        if (!plan.passes.empty()) {
            draw_shadows(plan, pass_uniforms, placed);
        }

        gl_.framebuffer_renderbuffer(gl::kFramebuffer, gl::kDepthAttachment, gl::kRenderbuffer,
                                     depth_);
        gl_.enable(gl::kDepthTest);
        gl_.depth_func(gl::kGreater);
        gl_.depth_mask(gl::kTrue);
        constexpr GLfloat kFar = 0.0F;
        gl_.clear_bufferfv(gl::kDepth, 0, &kFar);
        gl_.front_face(gl::kCcw);
        gl_.use_program(mesh_program_);
        for (std::size_t atlas = 0; atlas < 2; ++atlas) {
            const bool drawn = (atlas == 0 ? plan.directional_size : plan.positional_size) != 0;
            gl_.active_texture(gl::kTexture0 + static_cast<GLenum>(kMeshTextures + atlas));
            gl_.bind_texture(gl::kTexture2D,
                             drawn ? shadow_atlases_.at(atlas).texture : no_shadows_.texture);
            gl_.bind_sampler(static_cast<GLuint>(kMeshTextures + atlas), shadow_sampler_);
        }
        gl_.active_texture(gl::kTexture0 + static_cast<GLenum>(kMeshTextures + 2));
        gl_.bind_texture(gl::kTextureCubeMap,
                         radiance_used(scene) ? radiance_.filtered : no_radiance_);
        gl_.bind_sampler(static_cast<GLuint>(kMeshTextures + 2), radiance_sampler_);
        for (const std::size_t i : order) {
            if (mesh_draws_[i].shadows == ShadowCasting::ShadowsOnly) {
                continue;
            }
            const Material3D& material = mesh_draws_[i].material;
            const MeshBatch& batch = mesh_batches_[i];
            const bool blended = material.transparency == Transparency::Alpha;
            if (blended) {
                gl_.enable(gl::kBlend);
                set_blend(material.blend);
            } else {
                gl_.disable(gl::kBlend);
            }
            gl_.depth_mask(blended ? gl::kFalse : gl::kTrue);
            set_cull(material.cull);
            gl_.bind_buffer_range(gl::kUniformBuffer, kInstanceBinding, uniform_buffer_,
                                  static_cast<GLintptr>(placed[i].first),
                                  static_cast<GLsizeiptr>(sizeof(MeshInstanceUniforms)));
            gl_.bind_buffer_range(gl::kUniformBuffer, kSurfaceBinding, uniform_buffer_,
                                  static_cast<GLintptr>(placed[i].second),
                                  static_cast<GLsizeiptr>(sizeof(MeshSurfaceUniforms)));
            for (std::size_t unit = 0; unit < kMeshTextures; ++unit) {
                gl_.active_texture(gl::kTexture0 + static_cast<GLenum>(unit));
                gl_.bind_texture(gl::kTexture2D, batch.units.at(unit).texture);
                gl_.bind_sampler(static_cast<GLuint>(unit), batch.units.at(unit).sampler);
            }
            gl_.bind_vertex_array(batch.mesh.vertex_array);
            gl_.draw_elements(gl::kTriangles, batch.mesh.index_count, gl::kUnsignedInt, nullptr);
        }
        // Back to the 2D state the rest of the device draws with.
        for (std::size_t atlas = 0; atlas < 2; ++atlas) {
            gl_.active_texture(gl::kTexture0 + static_cast<GLenum>(kMeshTextures + atlas));
            gl_.bind_texture(gl::kTexture2D, 0);
            gl_.bind_sampler(static_cast<GLuint>(kMeshTextures + atlas), 0);
        }
        gl_.active_texture(gl::kTexture0 + static_cast<GLenum>(kMeshTextures + 2));
        gl_.bind_texture(gl::kTextureCubeMap, 0);
        gl_.bind_sampler(static_cast<GLuint>(kMeshTextures + 2), 0);
        gl_.active_texture(gl::kTexture0);
        gl_.bind_vertex_array(0);
        gl_.use_program(0);
        gl_.depth_mask(gl::kTrue);
        gl_.disable(gl::kDepthTest);
        gl_.disable(gl::kCullFace);
        gl_.enable(gl::kBlend);
        gl_.framebuffer_renderbuffer(gl::kFramebuffer, gl::kDepthAttachment, gl::kRenderbuffer, 0);
    }

    // An RGB10A2 cube texture size texels across, with levels levels.
    Result<GLuint> make_cube(std::uint32_t size, std::uint32_t levels) {
        GLuint texture = 0;
        gl_.gen_textures(1, &texture);
        gl_.bind_texture(gl::kTextureCubeMap, texture);
        for (std::uint32_t level = 0; level < levels; ++level) {
            const auto n = static_cast<GLsizei>(std::max(size >> level, 1U));
            for (std::uint32_t face = 0; face < kCubeFaces; ++face) {
                gl_.tex_image_2d(gl::kTextureCubeMapPositiveX + face, static_cast<GLint>(level),
                                 static_cast<GLint>(gl::kRgb10A2), n, n, 0, gl::kRgba,
                                 gl::kUnsignedInt2101010Rev, nullptr);
            }
        }
        gl_.tex_parameteri(gl::kTextureCubeMap, gl::kTextureBaseLevel, 0);
        gl_.tex_parameteri(gl::kTextureCubeMap, gl::kTextureMaxLevel,
                           static_cast<GLint>(levels - 1));
        gl_.bind_texture(gl::kTextureCubeMap, 0);
        if (auto checked = check_errors("creating a radiance cube"); !checked) {
            gl_.delete_textures(1, &texture);
            return checked.error();
        }
        return texture;
    }

    // Points the radiance framebuffer at a level of one face of a cube, and the viewport at it.
    void target_cube(GLuint cube, std::uint32_t face, std::uint32_t level,
                     std::uint32_t size) const {
        gl_.framebuffer_texture_2d(gl::kFramebuffer, gl::kColorAttachment0,
                                   gl::kTextureCubeMapPositiveX + face, cube,
                                   static_cast<GLint>(level));
        const auto n = static_cast<GLsizei>(std::max(size >> level, 1U));
        gl_.viewport(0, 0, n, n);
    }

    // What the frame's meshes take the radiance from, made at its first mesh draw: the sampler,
    // framebuffer and a black cube for meshes with none, and for a scene with radiance, the
    // programs and cubes, drawn again at end_frame when what they are drawn from changed.
    Result<void> prepare_radiance(const Scene3D& scene) {
        if (radiance_prepared_) {
            return {};
        }
        if (radiance_sampler_ == 0) {
            gl_.gen_samplers(1, &radiance_sampler_);
            for (const auto& [name, value] :
                 {std::pair{gl::kTextureMinFilter, gl::kLinearMipmapLinear},
                  std::pair{gl::kTextureMagFilter, gl::kLinear},
                  std::pair{gl::kTextureWrapS, gl::kClampToEdge},
                  std::pair{gl::kTextureWrapT, gl::kClampToEdge},
                  std::pair{gl::kTextureWrapR, gl::kClampToEdge}}) {
                gl_.sampler_parameteri(radiance_sampler_, name, static_cast<GLint>(value));
            }
            gl_.gen_framebuffers(1, &radiance_framebuffer_);
        }
        if (no_radiance_ == 0) {
            auto made = make_cube(1, 1);
            if (!made) {
                return made.error();
            }
            no_radiance_ = made.value();
            // Cleared once, so that it is never read undefined.
            gl_.bind_framebuffer(gl::kFramebuffer, radiance_framebuffer_);
            gl_.clear_color(0.0F, 0.0F, 0.0F, 1.0F);
            for (std::uint32_t face = 0; face < kCubeFaces; ++face) {
                target_cube(no_radiance_, face, 0, 1);
                gl_.clear(gl::kColorBufferBit);
            }
            gl_.framebuffer_texture_2d(gl::kFramebuffer, gl::kColorAttachment0, gl::kTexture2D, 0,
                                       0);
            gl_.bind_framebuffer(gl::kFramebuffer, frame_framebuffer_);
        }
        if (radiance_used(scene)) {
            if (auto prepared = prepare_sky(); !prepared) {
                return prepared;
            }
            if (radiance_program_ == 0) {
                auto program = link(shaders::kRadianceVertexGlsl, shaders::kRadianceFragmentGlsl,
                                    kRadianceSamplerNames, "the radiance filter");
                if (!program) {
                    return program.error();
                }
                radiance_program_ = program.value();
            }
            const std::uint32_t size = scene.radiance.size;
            if (radiance_.size != size) {
                for (GLuint* cube : {&radiance_.raw, &radiance_.filtered}) {
                    if (*cube != 0) {
                        gl_.delete_textures(1, cube);
                        *cube = 0;
                    }
                }
                radiance_.size = 0;
                radiance_.key.reset();
                auto raw = make_cube(size, radiance_levels(size));
                if (!raw) {
                    return raw.error();
                }
                radiance_.raw = raw.value();
                auto filtered = make_cube(size, radiance_levels(size));
                if (!filtered) {
                    return filtered.error();
                }
                radiance_.filtered = filtered.value();
                radiance_.size = size;
            }
            const RadianceKey key = radiance_key(scene);
            radiance_draws_ = !radiance_.key || !(*radiance_.key == key);
            radiance_.key = key;
        }
        radiance_prepared_ = true;
        return {};
    }

    // The scene's radiance, as Godot's Compatibility renderer draws its radiance map: the sky into
    // each face of the raw cube, its mipmaps, then each level of each face filtered from it.
    void draw_radiance(const Scene3D& scene) {
        const std::uint32_t size = radiance_.size;
        const std::uint32_t levels = radiance_levels(size);
        uniform_bytes_.clear();
        std::array<std::size_t, kCubeFaces> faces{};
        for (std::uint32_t face = 0; face < kCubeFaces; ++face) {
            const SkyUniforms sky = radiance_face_uniforms(scene, face);
            faces.at(face) = place_uniforms(&sky, sizeof(sky));
        }
        std::vector<std::size_t> filters;
        for (std::uint32_t level = 0; level < levels; ++level) {
            const auto uniforms =
                std::make_unique<RadianceUniforms>(radiance_uniforms(size, levels, level));
            for (std::uint32_t face = 0; face < kCubeFaces; ++face) {
                uniforms->params[0] = static_cast<float>(face);
                filters.push_back(place_uniforms(uniforms.get(), sizeof(RadianceUniforms)));
            }
        }
        gl_.bind_buffer(gl::kUniformBuffer, uniform_buffer_);
        gl_.buffer_data(gl::kUniformBuffer, static_cast<GLsizeiptr>(uniform_bytes_.size()),
                        uniform_bytes_.data(), gl::kStreamDraw);
        gl_.bind_framebuffer(gl::kFramebuffer, radiance_framebuffer_);
        gl_.disable(gl::kBlend);
        gl_.bind_vertex_array(sky_vertex_array_);

        gl_.use_program(sky_program_);
        const Unit texture = binding(radiance_texture(scene), true);
        gl_.active_texture(gl::kTexture0);
        gl_.bind_texture(gl::kTexture2D, texture.texture);
        gl_.bind_sampler(0, texture.sampler);
        for (std::uint32_t face = 0; face < kCubeFaces; ++face) {
            target_cube(radiance_.raw, face, 0, size);
            gl_.bind_buffer_range(gl::kUniformBuffer, kSkyBinding, uniform_buffer_,
                                  static_cast<GLintptr>(faces.at(face)),
                                  static_cast<GLsizeiptr>(sizeof(SkyUniforms)));
            gl_.draw_arrays(gl::kTriangles, 0, 3);
        }
        gl_.bind_texture(gl::kTexture2D, 0);
        gl_.bind_texture(gl::kTextureCubeMap, radiance_.raw);
        gl_.generate_mipmap(gl::kTextureCubeMap);

        gl_.use_program(radiance_program_);
        gl_.bind_sampler(0, radiance_sampler_);
        for (std::uint32_t level = 0; level < levels; ++level) {
            for (std::uint32_t face = 0; face < kCubeFaces; ++face) {
                target_cube(radiance_.filtered, face, level, size);
                gl_.bind_buffer_range(
                    gl::kUniformBuffer, kFilterBinding, uniform_buffer_,
                    static_cast<GLintptr>(filters.at((level * kCubeFaces) + face)),
                    static_cast<GLsizeiptr>(sizeof(RadianceUniforms)));
                gl_.draw_arrays(gl::kTriangles, 0, 3);
            }
        }
        // Back to the 2D state the rest of the device draws with.
        gl_.bind_texture(gl::kTextureCubeMap, 0);
        gl_.bind_sampler(0, 0);
        gl_.framebuffer_texture_2d(gl::kFramebuffer, gl::kColorAttachment0, gl::kTexture2D, 0, 0);
        gl_.bind_framebuffer(gl::kFramebuffer, frame_framebuffer_);
        gl_.bind_vertex_array(0);
        gl_.use_program(0);
        gl_.enable(gl::kBlend);
    }

    // The sky's program, made at the start of the first frame with a sky so a failure shows
    // there.
    Result<void> prepare_sky() {
        if (sky_program_ != 0) {
            return {};
        }
        auto program =
            link(shaders::kSkyVertexGlsl, shaders::kSkyFragmentGlsl, kSkySamplerNames, "the sky");
        if (!program) {
            return program.error();
        }
        sky_program_ = program.value();
        // Its triangle has no vertices to read, but a vertex array must be bound.
        gl_.gen_vertex_arrays(1, &sky_vertex_array_);
        return {};
    }

    // sky.hlsl's one triangle over the whole of the bound framebuffer, before the meshes.
    void draw_sky(const Scene3D& scene) {
        uniform_bytes_.clear();
        const SkyUniforms uniforms = sky_uniforms(scene, frame_width_, frame_height_);
        place_uniforms(&uniforms, sizeof(uniforms));
        gl_.bind_buffer(gl::kUniformBuffer, uniform_buffer_);
        gl_.buffer_data(gl::kUniformBuffer, static_cast<GLsizeiptr>(uniform_bytes_.size()),
                        uniform_bytes_.data(), gl::kStreamDraw);
        gl_.bind_buffer_range(gl::kUniformBuffer, kSkyBinding, uniform_buffer_, 0,
                              static_cast<GLsizeiptr>(sizeof(SkyUniforms)));
        gl_.disable(gl::kBlend);
        gl_.use_program(sky_program_);
        gl_.bind_vertex_array(sky_vertex_array_);
        const Unit texture = binding(sky_texture(scene.sky), true);
        gl_.active_texture(gl::kTexture0);
        gl_.bind_texture(gl::kTexture2D, texture.texture);
        gl_.bind_sampler(0, texture.sampler);
        gl_.draw_arrays(gl::kTriangles, 0, 3);
        // Back to the 2D state the rest of the device draws with.
        gl_.bind_texture(gl::kTexture2D, 0);
        gl_.bind_sampler(0, 0);
        gl_.bind_vertex_array(0);
        gl_.use_program(0);
        gl_.enable(gl::kBlend);
    }

    // The post-processing program, and the textures the meshes and glow draw into at the frame's
    // size, made at the start of a frame that post-processes so a failure shows there.
    Result<void> prepare_post(std::uint32_t width, std::uint32_t height) {
        if (post_program_ == 0) {
            auto program = link(shaders::kPostVertexGlsl, shaders::kPostFragmentGlsl,
                                kPostSamplerNames, "the post-processing shader");
            if (!program) {
                return program.error();
            }
            post_program_ = program.value();
            // The passes' triangle has no vertices to read, but a vertex array must be bound.
            gl_.gen_vertex_arrays(1, &post_vertex_array_);
        }
        if (post_targets_.world.gl != 0 && post_targets_.world.desc.width == width &&
            post_targets_.world.desc.height == height) {
            return {};
        }
        release_post_targets();
        const auto made = [&](std::uint32_t w, std::uint32_t h, Texture& texture) -> Result<void> {
            const TextureDesc desc{
                .width = w, .height = h, .filter = Filter::Linear, .render_target = true};
            auto gl = create_gl_texture(desc, {});
            if (!gl) {
                return gl.error();
            }
            texture = Texture{.gl = gl.value(), .framebuffer = 0, .desc = desc};
            if (auto framebuffer = attach(texture); !framebuffer) {
                return framebuffer.error();
            }
            return {};
        };
        auto world = made(width, height, post_targets_.world);
        const std::array<PostSize, kGlowLevels> sizes = glow_sizes(width, height);
        for (std::size_t i = 0; i < kGlowLevels && world; ++i) {
            world = made(sizes.at(i).width, sizes.at(i).height, post_targets_.glow.at(i));
        }
        if (!world) {
            release_post_targets();
            return world.error();
        }
        return {};
    }

    void release_post_targets() {
        const auto release = [this](Texture& texture) {
            if (texture.gl != 0) {
                pending_release_.push_back(std::exchange(texture, Texture{}));
            }
        };
        release(post_targets_.world);
        for (Texture& texture : post_targets_.glow) {
            release(texture);
        }
    }

    // post.hlsl's passes, as the GPU device draws them: glow's filter, downsampling and
    // upsampling through its four textures, then the post pass from the meshes' texture over the
    // frame's framebuffer, which is left bound.
    void draw_post(const PostProcess& post) {
        struct PostDraw {
            GLuint framebuffer = 0;
            PostSize size;
            std::array<GLuint, 3> textures{}; // the source, glow and the color correction
            std::size_t uniforms = 0;
        };
        std::vector<PostDraw> draws;
        uniform_bytes_.clear();
        const auto add = [&](const Texture& target, const PostUniforms& uniforms,
                             std::array<GLuint, 3> textures) {
            draws.push_back({.framebuffer = target.framebuffer,
                             .size = {target.desc.width, target.desc.height},
                             .textures = textures,
                             .uniforms = place_uniforms(&uniforms, sizeof(uniforms))});
        };
        if (post.glow) {
            for (std::size_t i = 0; i < kGlowLevels; ++i) {
                const Texture& target = post_targets_.glow.at(i);
                add(target,
                    post_uniforms(post, i == 0 ? PostPass::GlowFilter : PostPass::GlowDownsample,
                                  target.desc.width, target.desc.height),
                    {i == 0 ? post_targets_.world.gl : post_targets_.glow.at(i - 1).gl, white_,
                     white_});
            }
            for (std::size_t i = kGlowLevels - 1; i-- > 0;) {
                const Texture& target = post_targets_.glow.at(i);
                add(target,
                    post_uniforms(post, PostPass::GlowUpsample, target.desc.width,
                                  target.desc.height),
                    {post_targets_.glow.at(i + 1).gl, white_, white_});
            }
        }
        GLuint lut = white_;
        TextureDesc lut_desc{};
        if (post.adjustments && post.color_correction != TextureId::None) {
            const Texture& found = textures_.at(post.color_correction);
            lut = found.gl;
            lut_desc = found.desc;
        }
        const Texture frame{.gl = 0,
                            .framebuffer = frame_framebuffer_,
                            .desc = {.width = frame_width_, .height = frame_height_}};
        add(frame,
            post_uniforms(post, PostPass::Post, frame_width_, frame_height_, lut_desc.width,
                          lut_desc.height),
            {post_targets_.world.gl, post.glow ? post_targets_.glow[0].gl : white_, lut});

        gl_.bind_buffer(gl::kUniformBuffer, uniform_buffer_);
        gl_.buffer_data(gl::kUniformBuffer, static_cast<GLsizeiptr>(uniform_bytes_.size()),
                        uniform_bytes_.data(), gl::kStreamDraw);
        gl_.disable(gl::kBlend);
        gl_.use_program(post_program_);
        gl_.bind_vertex_array(post_vertex_array_);
        const GLuint sampler = samplers_.at(sampler_index(Filter::Linear, false));
        for (const PostDraw& draw : draws) {
            gl_.bind_framebuffer(gl::kFramebuffer, draw.framebuffer);
            gl_.viewport(0, 0, static_cast<GLsizei>(draw.size.width),
                         static_cast<GLsizei>(draw.size.height));
            gl_.bind_buffer_range(gl::kUniformBuffer, kPostBinding, uniform_buffer_,
                                  static_cast<GLintptr>(draw.uniforms),
                                  static_cast<GLsizeiptr>(sizeof(PostUniforms)));
            for (std::size_t unit = 0; unit < draw.textures.size(); ++unit) {
                gl_.active_texture(gl::kTexture0 + static_cast<GLenum>(unit));
                gl_.bind_texture(gl::kTexture2D, draw.textures.at(unit));
                gl_.bind_sampler(static_cast<GLuint>(unit), sampler);
            }
            gl_.draw_arrays(gl::kTriangles, 0, 3);
        }
        // Back to the 2D state the rest of the device draws with.
        for (std::size_t unit = 0; unit < 3; ++unit) {
            gl_.active_texture(gl::kTexture0 + static_cast<GLenum>(unit));
            gl_.bind_texture(gl::kTexture2D, 0);
            gl_.bind_sampler(static_cast<GLuint>(unit), 0);
        }
        gl_.active_texture(gl::kTexture0);
        gl_.bind_vertex_array(0);
        gl_.use_program(0);
        gl_.enable(gl::kBlend);
    }

    void finish_frame() {
        vertices_.clear();
        batches_.clear();
        mesh_draws_.clear();
        mesh_batches_.clear();
        shadow_plan_.reset();
        radiance_prepared_ = false;
        radiance_draws_ = false;
        if (!pending_release_.empty() || !pending_programs_.empty() || !pending_meshes_.empty()) {
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
        for (const GlMesh& mesh : pending_meshes_) {
            delete_mesh(mesh);
        }
        pending_meshes_.clear();
    }

    SDL_Window* window_;
    bool presents_;
    bool owns_video_;
    std::vector<GlApi> apis_;
    SDL_GLContext context_ = nullptr;
    GlApi api_ = GlApi::Es;
    Gl gl_;
    std::string renderer_;
    std::string version_;
    GLint max_texture_size_ = 0;
    // Which formats the context samples, by TextureFormat.
    std::array<bool, 3> sampled_{true, false, false};
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
    std::unordered_map<MeshId, GlMesh> meshes_;
    std::uint32_t next_mesh_ = 1;
    std::vector<GlMesh> pending_meshes_;
    GLuint mesh_program_ = 0;
    GLuint depth_ = 0; // a renderbuffer
    // The frame's shadow plan, made at its first mesh draw, and what its passes draw with.
    std::optional<ShadowPlan> shadow_plan_;
    std::array<GlShadowAtlas, 2> shadow_atlases_{}; // directional, then positional
    GlShadowAtlas no_shadows_;                      // bound for an atlas the frame doesn't draw
    GLuint shadow_program_ = 0;
    GLuint shadow_sampler_ = 0;
    GLuint shadow_framebuffer_ = 0;
    std::uint32_t depth_width_ = 0;
    std::uint32_t depth_height_ = 0;
    detail::CpuParticles particles_;
    std::vector<Vertex2D> particle_vertices_;
    Texture present_;
    // What a frame that post-processes draws with: the meshes' texture and glow's.
    struct PostTargets {
        Texture world;
        std::array<Texture, kGlowLevels> glow{};
    };
    PostTargets post_targets_;
    GLuint post_program_ = 0;
    GLuint post_vertex_array_ = 0;
    GLuint sky_program_ = 0;
    GLuint sky_vertex_array_ = 0;
    // The radiance meshes are lit by: the raw cube the sky draws into and the one filtered from
    // it, and what they hold (nullopt to draw them again).
    struct RadianceCubes {
        GLuint raw = 0;
        GLuint filtered = 0;
        std::uint32_t size = 0;
        std::optional<RadianceKey> key;
    };
    RadianceCubes radiance_;
    GLuint radiance_program_ = 0;
    GLuint radiance_sampler_ = 0;
    GLuint radiance_framebuffer_ = 0;
    GLuint no_radiance_ = 0;         // bound when the frame's meshes have none
    bool radiance_prepared_ = false; // at the frame's first mesh draw
    bool radiance_draws_ = false;    // whether end_frame draws the radiance again

    // The frame being built. A skipped frame is 0 wide.
    bool in_frame_ = false;
    Frame frame_;
    GLuint frame_framebuffer_ = 0;
    std::uint32_t frame_width_ = 0;
    std::uint32_t frame_height_ = 0;
    std::vector<Vertex2D> vertices_;
    std::vector<Batch> batches_;
    // The frame's mesh draws, and what each binds.
    std::vector<MeshDraw> mesh_draws_;
    std::vector<MeshBatch> mesh_batches_;
    bool break_batch_ = false; // the next draw starts a batch
    std::vector<std::uint8_t> uniform_bytes_;
    VsyncMode vsync_ = VsyncMode::Enabled;
    Stats stats_;
};

} // namespace

bool gl_library_loads() {
    if (auto started = init_video(); !started) {
        return false;
    }
    // SDL counts loads, and a window made for OpenGL loads the library again.
    const bool loads = SDL_GL_LoadLibrary(nullptr);
    if (loads) {
        SDL_GL_UnloadLibrary();
    }
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    return loads;
}

Result<std::unique_ptr<RenderDevice>> create_gl_device(const DeviceOptions& options) {
    SDL_Window* window = options.window;
    const bool presents = window != nullptr;
    if (presents && (SDL_GetWindowFlags(window) & SDL_WINDOW_OPENGL) == 0) {
        return make_error(ErrorCode::InvalidArgument,
                          "the window wasn't made for OpenGL; create it with SDL_WINDOW_OPENGL");
    }
    std::vector<GlApi> apis = gl_apis(options.driver);
    if (!presents) {
        if (auto started = init_video(); !started) {
            return started.error();
        }
        request_gl_context(apis.front());
        window = SDL_CreateWindow("SlashEngine", 1, 1, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
        if (window == nullptr) {
            Error error = sdl_error("OpenGL doesn't work here");
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
            return error;
        }
    }
    auto device = std::make_unique<GlDevice>(window, presents, !presents, std::move(apis));
    if (auto ready = device->init(); !ready) {
        return ready.error();
    }
    log::info("render device created", {{"backend", backend_name(device->backend())},
                                        {"renderer", device->renderer()},
                                        {"version", device->version()},
                                        {"window", presents}});
    return std::unique_ptr<RenderDevice>(std::move(device));
}

} // namespace slash::render::detail
