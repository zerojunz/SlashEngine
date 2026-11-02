#pragma once

#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/math.hpp"
#include "slash/core/result.hpp"
#include "slash/procedural/gradient.hpp"
#include "slash/procedural/noise.hpp"
#include "slash/render/device.hpp"
#include "slash/world/world.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// Procedural textures: .texture.json files that are made into images as they load, as Godot's
// NoiseTexture2D, GradientTexture1D and GradientTexture2D resources. Canvas loads them as
// textures (canvas::add_asset_types), so a sprite, tile set, material, style box or model
// material can name one wherever it names a .png file.
//
//   {
//     "format": 1,
//     "type": "NoiseTexture2D",
//     "width": 256,
//     "height": 256,
//     "seamless": true,
//     "noise": {"noise_type": "cellular", "frequency": 0.04},
//     "color_ramp": {"points": [{"offset": 0, "color": [0.1, 0.2, 0.5, 1]},
//                               {"offset": 1, "color": [0.8, 0.9, 1, 1]}]}
//   }
//
// Every field but "format" and "type" is optional, with Godot's default. The pixels are the same
// on every machine.
namespace slash::procedural {

inline constexpr std::string_view kTextureSuffix = ".texture.json";

// Godot's NoiseTexture2D. Godot's generate_mipmaps is left out: textures here have one level.
struct NoiseTexture2D {
    int width = 512;
    int height = 512;
    bool invert = false;
    bool in_3d_space = false;
    bool seamless = false;
    float seamless_blend_skirt = 0.1F;
    bool as_normal_map = false;
    float bump_strength = 8.0F;
    bool normalize = true;
    std::optional<Gradient> color_ramp;
    NoiseSettings noise;
};

// Godot's GradientTexture1D: the gradient across width pixels, one high. Godot's use_hdr is left
// out: textures here are 8 bits a channel.
struct GradientTexture1D {
    int width = 256;
    Gradient gradient;
};

// Godot's GradientTexture2D.Fill and Repeat.
enum class GradientFill : std::uint8_t { Linear, Radial, Square };
enum class GradientRepeat : std::uint8_t { None, Repeat, Mirror };

// Godot's GradientTexture2D: the gradient from fill_from to fill_to, in UV coordinates (0 to 1
// across the texture).
struct GradientTexture2D {
    int width = 64;
    int height = 64;
    GradientFill fill = GradientFill::Linear;
    Vec2 fill_from{0.0F, 0.0F};
    Vec2 fill_to{1.0F, 0.0F};
    GradientRepeat repeat = GradientRepeat::None;
    Gradient gradient;
};

using ProceduralTexture = std::variant<NoiseTexture2D, GradientTexture1D, GradientTexture2D>;

// JSON Schema for .texture.json files.
[[nodiscard]] json::Value texture_schema();

// Every error in a .texture.json document, each with the JSON Pointer of the bad value.
[[nodiscard]] std::vector<json::ValidationError> validate_texture(const json::Value& doc);

// The texture a .texture.json document describes, or its first error.
[[nodiscard]] Result<ProceduralTexture> parse_texture(const json::Value& doc);

// The texture as a .texture.json document, leaving out fields at their defaults.
[[nodiscard]] json::Value texture_to_json(const ProceduralTexture& texture);

// The texture's RGBA8 pixels, as Godot makes them: NoiseTexture2D::_generate_texture (Noise's
// get_image or get_seamless_image, the color ramp, then bump_map_to_normal_map),
// GradientTexture1D::_update and GradientTexture2D::_update.
[[nodiscard]] render::Image make_image(const ProceduralTexture& texture);

// Whether a path names a texture: an image file (render::is_image_path), a .texture.json file or
// a texture gameplay code made (asset::is_made_path).
[[nodiscard]] bool is_texture_path(std::string_view path);

// Makes a texture from pixels gameplay code drew, as Godot's ImageTexture.create_from_image, an
// asset at "made:<name>" in the assets the world's modules load from (canvas::install attaches
// them): a sprite, tile set, TextureRect or StandardMaterial3D can name the path. Making it again
// with the same name changes it in place, and renderers upload it again. Returns the path, or
// fails when the world has no assets, name isn't letters, digits and "_-./", or the pixels aren't
// width x height x 4 bytes.
Result<std::string> make_texture(world::World& world, std::string_view name, render::Image image);

// A texture file's pixels: a .texture.json file's made by make_image, any other decoded as an
// image file by render::decode_image. For code that reads texture files outside assets, such as
// checks of a material's textures.
[[nodiscard]] Result<render::Image> decode_texture(std::string_view path, std::string_view bytes);

// Godot's Noise::get_image (seamless false) or get_seamless_image: one gray byte a pixel.
[[nodiscard]] std::vector<std::uint8_t> noise_image(const Noise& noise, int width, int height,
                                                    bool invert, bool in_3d_space, bool normalize,
                                                    bool seamless, float blend_skirt = 0.1F);

// Godot's Image::bump_map_to_normal_map on an image's red channel: tangent-space normals, wrapped
// at the edges.
void bump_to_normal_map(render::Image& image, float bump_scale);

} // namespace slash::procedural
