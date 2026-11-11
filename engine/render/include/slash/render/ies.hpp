#pragma once

#include "slash/core/result.hpp"
#include "slash/render/device.hpp"

#include <string_view>
#include <vector>

// IES light profiles: how bright a real lamp shines in each direction, as photometric labs
// measure it (IES LM-63, the .ies files lighting makers publish), which Unreal's lights take as
// IES textures. SlashEngine bakes a profile into a light projector texture (Light3D::projector),
// so a light shines its lamp's pattern through the same path Godot's light_projector does.
namespace slash::render {

// A type C profile: candelas at vertical angles from straight down the lamp's axis (0) to
// straight up (180) and horizontal angles around it, in degrees, both ascending.
struct IesProfile {
    std::vector<float> vertical;
    std::vector<float> horizontal;
    // candela[h * vertical.size() + v], already times the file's multiplier.
    std::vector<float> candela;
    float lumens = 0.0F; // the lamps' rated lumens in all, or 0 for absolute photometry
};

// Reads an LM-63 file (1986, 1991, 1995 and 2002): its keywords, TILT=NONE or INCLUDE (whose
// tilt table it skips; a TILT file is refused), then the photometric data. Only type C, the
// type every architectural and street lamp uses, is taken.
[[nodiscard]] Result<IesProfile> parse_ies(std::string_view text);

// The profile's candelas toward a direction at a vertical and horizontal angle in degrees,
// between its measurements, with its symmetry: one horizontal angle is round the axis, a last
// of 90 a quadrant mirrored, of 180 a half mirrored, and of 360 (or more) the whole way round.
[[nodiscard]] float ies_candela(const IesProfile& profile, float vertical, float horizontal);

// The profile baked as a projector texture, its brightest direction white and the rest by its
// share of that, sRGB, the same bytes everywhere. For a spot light, size by size texels as a
// SpotLight3D's projector maps them over its cone of spot_angle degrees, its axis the light's
// direction (-Z). For an omni light, size by 2 * size texels as Godot's dual paraboloid
// projector maps them: the top half toward the light's -Z, the bottom half toward +Z, with the
// profile's axis (vertical angle 0) down the light's -Y.
[[nodiscard]] Image bake_ies_spot(const IesProfile& profile, float spot_angle, std::uint32_t size);
[[nodiscard]] Image bake_ies_omni(const IesProfile& profile, std::uint32_t size);

} // namespace slash::render
