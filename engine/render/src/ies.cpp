#include "slash/render/ies.hpp"

#include "slash/core/math.hpp"
#include "slash/core/text.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace slash::render {
namespace {

constexpr float kDegrees = 180.0F / std::numbers::pi_v<float>;

// The text's numbers after `from`, split at spaces, commas and line ends.
std::vector<std::string_view> words_of(std::string_view text) {
    std::vector<std::string_view> words;
    std::size_t at = 0;
    while (at < text.size()) {
        while (at < text.size() && (text[at] == ' ' || text[at] == '\t' || text[at] == '\r' ||
                                    text[at] == '\n' || text[at] == ',')) {
            ++at;
        }
        const std::size_t start = at;
        while (at < text.size() && text[at] != ' ' && text[at] != '\t' && text[at] != '\r' &&
               text[at] != '\n' && text[at] != ',') {
            ++at;
        }
        if (at > start) {
            words.push_back(text.substr(start, at - start));
        }
    }
    return words;
}

Error ies_error(const std::string& message) {
    return make_error(ErrorCode::InvalidArgument, "IES profile: " + message);
}

// Godot's Color::linear_to_srgb for one channel, through slash::exp and ln.
float to_srgb(float c) {
    if (c <= 0.0F) {
        return 0.0F;
    }
    if (c >= 1.0F) {
        return 1.0F;
    }
    if (c < 0.0031308F) {
        return 12.92F * c;
    }
    return (1.055F * slash::exp(slash::ln(c) / 2.4F)) - 0.055F;
}

std::uint8_t to_byte(float c) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(to_srgb(c), 0.0F, 1.0F) * 255.0F));
}

// Where value falls among ascending angles: the angle before it and how far toward the next.
struct Between {
    std::size_t index = 0;
    float t = 0.0F;
};

Between between(const std::vector<float>& angles, float value) {
    if (angles.size() < 2 || value <= angles.front()) {
        return {};
    }
    if (value >= angles.back()) {
        return {.index = angles.size() - 1, .t = 0.0F};
    }
    const auto upper = std::ranges::upper_bound(angles, value);
    const auto index = static_cast<std::size_t>(upper - angles.begin()) - 1;
    const float span = angles[index + 1] - angles[index];
    return {.index = index, .t = span > 0.0F ? (value - angles[index]) / span : 0.0F};
}

float maximum(const IesProfile& profile) {
    float most = 0.0F;
    for (const float value : profile.candela) {
        most = std::max(most, value);
    }
    return most;
}

// The vertical and horizontal angles, in degrees, of a direction whose cosine from the axis is
// along and whose sideways parts are x and y.
float vertical_of(float along, float x, float y) {
    return slash::atan2(std::sqrt((x * x) + (y * y)), along) * kDegrees;
}

float horizontal_of(float x, float y) {
    float angle = slash::atan2(y, x) * kDegrees;
    if (angle < 0.0F) {
        angle += 360.0F;
    }
    return angle;
}

} // namespace

Result<IesProfile> parse_ies(std::string_view text) {
    // Keywords run to the TILT line; the numbers start after it.
    const std::size_t tilt = text.find("TILT=");
    if (tilt == std::string_view::npos) {
        return ies_error("no TILT= line, so this is not an IES LM-63 file");
    }
    std::size_t line_end = text.find('\n', tilt);
    if (line_end == std::string_view::npos) {
        line_end = text.size();
    }
    std::string_view tilt_value = text.substr(tilt + 5, line_end - tilt - 5);
    while (!tilt_value.empty() && (tilt_value.back() == '\r' || tilt_value.back() == ' ')) {
        tilt_value.remove_suffix(1);
    }
    const std::vector<std::string_view> words = words_of(text.substr(line_end));
    std::vector<float> numbers;
    numbers.reserve(words.size());
    for (const std::string_view word : words) {
        const std::optional<double> value = parse_decimal(word);
        if (!value) {
            return ies_error("\"" + std::string(word) + "\" is not a number");
        }
        numbers.push_back(static_cast<float>(*value));
    }
    std::size_t at = 0;
    if (tilt_value == "INCLUDE") {
        // Lamp-to-luminaire geometry, then pairs of angles and factors, which only matter to a
        // lamp that tilts.
        if (numbers.size() < 2) {
            return ies_error("the tilt table ends early");
        }
        const auto pairs = static_cast<std::size_t>(numbers[1]);
        at = 2 + (2 * pairs);
    } else if (tilt_value != "NONE") {
        return ies_error("TILT=" + std::string(tilt_value) +
                         " names a tilt file; put its table in the profile with TILT=INCLUDE");
    }
    if (numbers.size() < at + 13) {
        return ies_error("the photometric data ends early");
    }
    IesProfile profile;
    const float lamps = numbers[at];
    const float lumens = numbers[at + 1];
    const float multiplier = numbers[at + 2];
    const auto vertical_count = static_cast<std::size_t>(numbers[at + 3]);
    const auto horizontal_count = static_cast<std::size_t>(numbers[at + 4]);
    const auto type = static_cast<int>(numbers[at + 5]);
    if (type != 1) {
        return ies_error("photometric type " + std::to_string(type) +
                         " (type B or A) is not taken; only type C");
    }
    if (vertical_count == 0 || horizontal_count == 0) {
        return ies_error("no angles");
    }
    profile.lumens = lumens > 0.0F ? lamps * lumens : 0.0F;
    at += 13; // the ten numbers above, then the ballast factor, a future use and the watts
    const std::size_t needed =
        vertical_count + horizontal_count + (vertical_count * horizontal_count);
    if (numbers.size() < at + needed) {
        return ies_error("the candela values end early: " + std::to_string(needed) +
                         " numbers expected after the header, " +
                         std::to_string(numbers.size() - at) + " found");
    }
    profile.vertical.assign(numbers.begin() + static_cast<std::ptrdiff_t>(at),
                            numbers.begin() + static_cast<std::ptrdiff_t>(at + vertical_count));
    at += vertical_count;
    profile.horizontal.assign(numbers.begin() + static_cast<std::ptrdiff_t>(at),
                              numbers.begin() + static_cast<std::ptrdiff_t>(at + horizontal_count));
    at += horizontal_count;
    profile.candela.reserve(vertical_count * horizontal_count);
    for (std::size_t i = 0; i < vertical_count * horizontal_count; ++i) {
        profile.candela.push_back(numbers[at + i] * multiplier);
    }
    if (!std::ranges::is_sorted(profile.vertical) || !std::ranges::is_sorted(profile.horizontal)) {
        return ies_error("angles must go up");
    }
    return profile;
}

float ies_candela(const IesProfile& profile, float vertical, float horizontal) {
    if (profile.candela.empty()) {
        return 0.0F;
    }
    // Fold the horizontal angle into what the profile measured, by its symmetry.
    float h = std::fmod(horizontal, 360.0F);
    if (h < 0.0F) {
        h += 360.0F;
    }
    const float last = profile.horizontal.back();
    if (profile.horizontal.size() == 1) {
        h = profile.horizontal.front();
    } else if (last <= 90.0F) {
        h = std::fmod(h, 180.0F);
        h = h > 90.0F ? 180.0F - h : h;
    } else if (last <= 180.0F) {
        h = h > 180.0F ? 360.0F - h : h;
    }
    const std::size_t count = profile.vertical.size();
    const Between across = between(profile.horizontal, h);
    const Between up = between(profile.vertical, vertical);
    const auto at = [&](std::size_t hi, std::size_t vi) {
        return profile.candela[(std::min(hi, profile.horizontal.size() - 1) * count) +
                               std::min(vi, count - 1)];
    };
    const float low = at(across.index, up.index) +
                      ((at(across.index, up.index + 1) - at(across.index, up.index)) * up.t);
    const float high =
        at(across.index + 1, up.index) +
        ((at(across.index + 1, up.index + 1) - at(across.index + 1, up.index)) * up.t);
    // Beyond the vertical angles measured, the lamp gives no light.
    if (vertical > profile.vertical.back() + 1e-3F || vertical < profile.vertical.front() - 1e-3F) {
        return 0.0F;
    }
    return low + ((high - low) * across.t);
}

Image bake_ies_spot(const IesProfile& profile, float spot_angle, std::uint32_t size) {
    Image image;
    image.width = size;
    image.height = size;
    image.pixels.resize(static_cast<std::size_t>(size) * size * 4);
    const float most = std::max(maximum(profile), 1e-6F);
    const Vec2f cone = slash::cos_sin(std::clamp(spot_angle, 0.1F, 89.9F) / kDegrees);
    const float tangent = cone.y / cone.x;
    for (std::uint32_t row = 0; row < size; ++row) {
        for (std::uint32_t column = 0; column < size; ++column) {
            // Godot flips a spot light's projector rect, so its top row is the light's +Y.
            const float u = ((static_cast<float>(column) + 0.5F) / static_cast<float>(size));
            const float v = 1.0F - ((static_cast<float>(row) + 0.5F) / static_cast<float>(size));
            const float x = ((2.0F * u) - 1.0F) * tangent;
            const float y = ((2.0F * v) - 1.0F) * tangent;
            // The direction (x, y, -1) from the light's -Z axis.
            const float value = ies_candela(profile, vertical_of(1.0F, x, y), horizontal_of(x, y));
            const std::uint8_t byte = to_byte(value / most);
            const std::size_t at = ((static_cast<std::size_t>(row) * size) + column) * 4;
            image.pixels[at] = byte;
            image.pixels[at + 1] = byte;
            image.pixels[at + 2] = byte;
            image.pixels[at + 3] = 255;
        }
    }
    return image;
}

Image bake_ies_omni(const IesProfile& profile, std::uint32_t size) {
    Image image;
    image.width = size;
    image.height = size * 2;
    image.pixels.resize(static_cast<std::size_t>(size) * size * 2 * 4);
    const float most = std::max(maximum(profile), 1e-6F);
    for (std::uint32_t half = 0; half < 2; ++half) {
        for (std::uint32_t row = 0; row < size; ++row) {
            for (std::uint32_t column = 0; column < size; ++column) {
                const float px =
                    (2.0F * (static_cast<float>(column) + 0.5F) / static_cast<float>(size)) - 1.0F;
                const float py =
                    (2.0F * (static_cast<float>(row) + 0.5F) / static_cast<float>(size)) - 1.0F;
                const float length = (px * px) + (py * py);
                std::uint8_t byte = 0;
                if (length <= 1.0F) {
                    // Godot's dual paraboloid: p = xy / (1 + |z|) on the unit sphere.
                    const float s = (1.0F - length) / (1.0F + length);
                    const float x = px * (1.0F + s);
                    const float y = py * (1.0F + s);
                    const float z = half == 0 ? -s : s;
                    // The profile's axis down the light's -Y, its angle 0 along +X toward +Z.
                    const float value =
                        ies_candela(profile, vertical_of(-y, x, z), horizontal_of(x, z));
                    byte = to_byte(value / most);
                }
                const std::size_t at =
                    ((static_cast<std::size_t>((half * size) + row) * size) + column) * 4;
                image.pixels[at] = byte;
                image.pixels[at + 1] = byte;
                image.pixels[at + 2] = byte;
                image.pixels[at + 3] = 255;
            }
        }
    }
    return image;
}

} // namespace slash::render
