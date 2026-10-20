#include "slash/library/generator.hpp"

#include "slash/core/text.hpp"
#include "slash/render/image.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slash::library {
namespace {

constexpr std::string_view kTest = "test";
constexpr int kDefaultImageSide = 64;
constexpr int kSampleRate = 22050;
constexpr double kDefaultSeconds = 0.5;

struct Rgb {
    int r = 0;
    int g = 0;
    int b = 0;
};

struct NamedColor {
    std::string_view name;
    Rgb rgb;
};

constexpr std::array<NamedColor, 16> kColors = {{
    {"red", {220, 50, 50}},
    {"green", {60, 180, 75}},
    {"blue", {50, 100, 220}},
    {"yellow", {240, 210, 50}},
    {"orange", {245, 130, 48}},
    {"purple", {145, 30, 180}},
    {"pink", {240, 120, 180}},
    {"brown", {140, 90, 40}},
    {"black", {30, 30, 30}},
    {"white", {240, 240, 240}},
    {"gray", {128, 128, 128}},
    {"grey", {128, 128, 128}},
    {"cyan", {70, 220, 220}},
    {"gold", {212, 175, 55}},
    {"golden", {212, 175, 55}},
    {"silver", {192, 192, 192}},
}};

// The prompt's words, lowercase, in order.
std::vector<std::string> words(std::string_view text) {
    std::vector<std::string> out;
    std::string word;
    for (const char c : text) {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            word += c;
        } else if (c >= 'A' && c <= 'Z') {
            word += static_cast<char>(c - 'A' + 'a');
        } else if (!word.empty()) {
            out.push_back(std::move(word));
            word.clear();
        }
    }
    if (!word.empty()) {
        out.push_back(std::move(word));
    }
    return out;
}

bool has_any(const std::vector<std::string>& found,
             std::initializer_list<std::string_view> wanted) {
    return std::ranges::any_of(found, [&wanted](const std::string& word) {
        return std::ranges::find(wanted, word) != wanted.end();
    });
}

std::vector<Rgb> colors_in(const std::vector<std::string>& found) {
    std::vector<Rgb> out;
    for (const std::string& word : found) {
        for (const NamedColor& color : kColors) {
            if (color.name == word) {
                out.push_back(color.rgb);
            }
        }
    }
    return out;
}

// FNV-1a over the prompt and seed, then splitmix64 for each value drawn from it.
class Hash {
public:
    Hash(std::string_view text, std::int64_t seed) {
        for (const char c : text) {
            state_ = (state_ ^ static_cast<unsigned char>(c)) * 0x100000001b3ULL;
        }
        state_ ^= static_cast<std::uint64_t>(seed) * 0x9e3779b97f4a7c15ULL;
    }

    std::uint64_t next() {
        std::uint64_t z = (state_ += 0x9e3779b97f4a7c15ULL);
        z = (z ^ (z >> 30U)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27U)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31U);
    }

    int below(int limit) { return static_cast<int>(next() % static_cast<std::uint64_t>(limit)); }

private:
    std::uint64_t state_ = 0xcbf29ce484222325ULL;
};

std::uint8_t channel(int value) {
    return static_cast<std::uint8_t>(std::clamp(value, 0, 255));
}

enum class Shape : std::uint8_t { Box, Circle, Triangle };

bool inside(Shape shape, int x, int y, int width, int height) {
    const std::int64_t w = width;
    const std::int64_t h = height;
    switch (shape) {
    case Shape::Circle: {
        // An ellipse filling seven eighths of the image, in doubled coordinates.
        const std::int64_t dx = 2 * std::int64_t{x} + 1 - w;
        const std::int64_t dy = 2 * std::int64_t{y} + 1 - h;
        const std::int64_t r = w * h * 7 / 8;
        return dx * dx * h * h + dy * dy * w * w <= r * r;
    }
    case Shape::Triangle: {
        const std::int64_t margin_y = h / 8;
        const std::int64_t rows = h - 2 * margin_y;
        const std::int64_t row = y - margin_y;
        if (row < 0 || row >= rows || rows <= 0) {
            return false;
        }
        const std::int64_t dx = 2 * std::int64_t{x} + 1 - w;
        return (dx < 0 ? -dx : dx) * rows <= (row + 1) * (w - w / 4);
    }
    case Shape::Box:
        break;
    }
    return x >= width / 8 && x < width - width / 8 && y >= height / 8 && y < height - height / 8;
}

Result<std::string> make_image(const GenerateRequest& request, std::int64_t seed) {
    const std::vector<std::string> found = words(request.prompt);
    Hash hash(request.prompt, seed);
    const int width = request.width > 0 ? request.width : kDefaultImageSide;
    const int height = request.height > 0 ? request.height : width;
    std::vector<Rgb> colors = colors_in(found);
    while (colors.size() < 2) {
        colors.push_back({60 + hash.below(180), 60 + hash.below(180), 60 + hash.below(180)});
    }
    const Rgb main = colors[0];
    const Rgb accent = colors[1];
    Shape shape = Shape::Box;
    if (has_any(found, {"ball", "bubble", "circle", "coin", "egg", "moon", "orb", "planet", "round",
                        "slime", "sun", "wheel"})) {
        shape = Shape::Circle;
    } else if (has_any(found, {"arrow", "mountain", "pyramid", "spike", "tree", "triangle"})) {
        shape = Shape::Triangle;
    }
    const bool transparent =
        has_any(found, {"transparent", "sprite", "icon", "cutout", "isolated"});
    const int band = 2 + hash.below(6);
    render::Image image;
    image.width = static_cast<std::uint32_t>(width);
    image.height = static_cast<std::uint32_t>(height);
    image.pixels.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            Rgb color{main.r / 4, main.g / 4, main.b / 4};
            int alpha = transparent ? 0 : 255;
            if (inside(shape, x, y, width, height)) {
                const bool edge = !inside(shape, x - 1, y, width, height) ||
                                  !inside(shape, x + 1, y, width, height) ||
                                  !inside(shape, x, y - 1, width, height) ||
                                  !inside(shape, x, y + 1, width, height);
                if (edge) {
                    color = {main.r / 3, main.g / 3, main.b / 3};
                } else if (((x + y) / band) % 4 == 0) {
                    color = accent;
                } else {
                    // Lit from the top left.
                    const int light = 24 - (48 * (x + y)) / (width + height);
                    color = {main.r + light, main.g + light, main.b + light};
                }
                alpha = 255;
            }
            const std::size_t at = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                    static_cast<std::size_t>(x)) *
                                   4;
            image.pixels[at] = channel(color.r);
            image.pixels[at + 1] = channel(color.g);
            image.pixels[at + 2] = channel(color.b);
            image.pixels[at + 3] = channel(alpha);
        }
    }
    return render::encode_png(image);
}

Result<std::string> refine_image(const GenerateRequest& request, std::int64_t seed) {
    auto decoded = render::decode_image(request.parent_data);
    if (!decoded) {
        return decoded.error();
    }
    render::Image image = std::move(decoded).value();
    const std::vector<std::string> found = words(request.prompt);
    const std::vector<Rgb> colors = colors_in(found);
    const bool mirror = has_any(found, {"flip", "flipped", "mirror", "mirrored"});
    const bool darker = has_any(found, {"dark", "darker", "darken", "shadow"});
    const bool lighter = has_any(found, {"bright", "brighter", "light", "lighter", "lighten"});
    const bool known = !colors.empty() || mirror || darker || lighter;
    Hash hash(request.prompt, seed);
    const int turn = 1 + hash.below(2);
    const std::size_t width = image.width;
    for (std::size_t y = 0; y < image.height; ++y) {
        if (mirror) {
            for (std::size_t x = 0; x < width / 2; ++x) {
                for (std::size_t c = 0; c < 4; ++c) {
                    std::swap(image.pixels[(y * width + x) * 4 + c],
                              image.pixels[(y * width + (width - 1 - x)) * 4 + c]);
                }
            }
        }
        for (std::size_t x = 0; x < width; ++x) {
            std::uint8_t* pixel = &image.pixels[(y * width + x) * 4];
            if (pixel[3] == 0) {
                continue;
            }
            std::array<int, 3> rgb{pixel[0], pixel[1], pixel[2]};
            if (!colors.empty()) {
                // The new color, as light or dark as the old one was.
                const int luma = (rgb[0] * 3 + rgb[1] * 6 + rgb[2]) / 10;
                rgb = {colors[0].r * luma / 160, colors[0].g * luma / 160,
                       colors[0].b * luma / 160};
            }
            if (darker) {
                rgb = {rgb[0] * 3 / 4, rgb[1] * 3 / 4, rgb[2] * 3 / 4};
            }
            if (lighter) {
                rgb = {rgb[0] + (255 - rgb[0]) / 3, rgb[1] + (255 - rgb[1]) / 3,
                       rgb[2] + (255 - rgb[2]) / 3};
            }
            if (!known) {
                // Nothing it understood: a variation, with the channels turned round.
                std::ranges::rotate(rgb, rgb.begin() + turn);
            }
            for (std::size_t c = 0; c < 3; ++c) {
                pixel[c] = channel(rgb.at(c));
            }
        }
    }
    return render::encode_png(image);
}

void put_u16(std::string& out, std::uint32_t value) {
    out += static_cast<char>(value & 0xFFU);
    out += static_cast<char>((value >> 8U) & 0xFFU);
}

void put_u32(std::string& out, std::uint32_t value) {
    put_u16(out, value & 0xFFFFU);
    put_u16(out, value >> 16U);
}

// A mono 16-bit PCM .wav file.
std::string wav(const std::vector<std::int16_t>& samples) {
    const auto data_bytes = static_cast<std::uint32_t>(samples.size() * 2);
    std::string out = "RIFF";
    put_u32(out, 36 + data_bytes);
    out += "WAVEfmt ";
    put_u32(out, 16);
    put_u16(out, 1); // PCM
    put_u16(out, 1); // mono
    put_u32(out, kSampleRate);
    put_u32(out, kSampleRate * 2);
    put_u16(out, 2);
    put_u16(out, 16);
    out += "data";
    put_u32(out, data_bytes);
    for (const std::int16_t sample : samples) {
        put_u16(out, static_cast<std::uint16_t>(sample));
    }
    return out;
}

std::uint32_t get_u32(std::string_view data, std::size_t at) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value |= std::uint32_t{static_cast<unsigned char>(data[at + i])} << (8 * i);
    }
    return value;
}

// The samples of a mono 16-bit PCM .wav file at kSampleRate, as the test generator writes them.
std::optional<std::vector<std::int16_t>> read_wav(std::string_view data) {
    if (data.size() < 44 || data.substr(0, 4) != "RIFF" || data.substr(8, 8) != "WAVEfmt " ||
        get_u32(data, 24) != kSampleRate || (get_u32(data, 20) & 0xFFFFU) != 1 ||
        (get_u32(data, 20) >> 16U) != 1 || (get_u32(data, 32) >> 16U) != 16 ||
        data.substr(36, 4) != "data") {
        return std::nullopt;
    }
    const std::size_t count = std::min<std::size_t>(get_u32(data, 40), data.size() - 44) / 2;
    std::vector<std::int16_t> samples(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto low = static_cast<unsigned char>(data[44 + 2 * i]);
        const auto high = static_cast<unsigned char>(data[45 + 2 * i]);
        samples[i] = static_cast<std::int16_t>(static_cast<std::uint16_t>(low | (high << 8U)));
    }
    return samples;
}

std::vector<std::int16_t> make_samples(const GenerateRequest& request, std::int64_t seed) {
    const std::vector<std::string> found = words(request.prompt);
    Hash hash(request.prompt, seed);
    const double seconds = request.seconds > 0.0 ? request.seconds : kDefaultSeconds;
    const auto count = static_cast<std::size_t>(std::lround(seconds * kSampleRate));
    std::uint32_t frequency = 220 + static_cast<std::uint32_t>(hash.below(660));
    if (has_any(found, {"bass", "deep", "low", "rumble", "thud"})) {
        frequency = 80 + static_cast<std::uint32_t>(hash.below(140));
    } else if (has_any(found, {"bell", "bird", "chime", "coin", "high", "ping", "sparkle"})) {
        frequency = 880 + static_cast<std::uint32_t>(hash.below(880));
    }
    const bool noise =
        has_any(found, {"crash", "explosion", "hit", "noise", "rain", "static", "wind"});
    const bool square = has_any(found, {"8bit", "chiptune", "jump", "laser", "retro", "square"});
    const bool rising = has_any(found, {"jump", "rise", "rising", "up"});
    const bool falling = has_any(found, {"down", "fall", "falling", "laser"});
    const bool held = has_any(found, {"ambient", "drone", "hum", "loop", "music"});
    std::vector<std::int16_t> samples(count);
    std::uint32_t phase = 0;
    auto lcg = static_cast<std::uint32_t>(hash.next());
    for (std::size_t i = 0; i < count; ++i) {
        // The pitch doubles or halves over the sound when it rises or falls.
        std::uint64_t hz = frequency;
        if (rising) {
            hz = frequency + frequency * i / count;
        } else if (falling) {
            hz = frequency - frequency * i / (2 * count);
        }
        phase += static_cast<std::uint32_t>((hz << 32U) / kSampleRate);
        std::int32_t wave = 0;
        if (noise) {
            lcg = lcg * 1664525U + 1013904223U;
            wave = static_cast<std::int32_t>(lcg >> 16U) - 32768;
        } else if (square) {
            wave = (phase >> 31U) != 0 ? 32767 : -32767;
        } else {
            const auto ramp = static_cast<std::int32_t>(phase >> 15U); // 0 to 131071
            wave = ramp < 65536 ? ramp - 32768 : 98303 - ramp;
        }
        const std::int64_t envelope =
            held ? 1000 : static_cast<std::int64_t>(1000 - 1000 * i / count);
        samples[i] = static_cast<std::int16_t>(wave * envelope / 2000);
    }
    return samples;
}

Result<std::string> refine_sound(const GenerateRequest& request, std::int64_t seed) {
    std::optional<std::vector<std::int16_t>> parent = read_wav(request.parent_data);
    if (!parent) {
        return make_error(ErrorCode::InvalidArgument,
                          "the test generator refines only the .wav sounds it makes");
    }
    const std::vector<std::string> found = words(request.prompt);
    // Pitch and length as a ratio of numerator to denominator: each output sample reads the
    // input at i * num / den.
    std::uint64_t num = 1;
    std::uint64_t den = 1;
    if (has_any(found, {"higher", "high", "faster"})) {
        num = 3;
        den = 2;
    } else if (has_any(found, {"lower", "low", "slower"})) {
        num = 2;
        den = 3;
    }
    std::size_t length = parent->size() * den / num;
    if (has_any(found, {"shorter", "short"})) {
        length /= 2;
    } else if (has_any(found, {"longer", "long"})) {
        length *= 2;
    }
    int gain = 100;
    if (has_any(found, {"louder", "loud"})) {
        gain = 150;
    } else if (has_any(found, {"quieter", "quiet", "softer", "soft"})) {
        gain = 50;
    }
    const bool reverse = has_any(found, {"reverse", "reversed", "backward", "backwards"});
    if (num == 1 && length == parent->size() && gain == 100 && !reverse) {
        // Nothing it understood: a variation a little higher, by the seed.
        Hash hash(request.prompt, seed);
        num = 17 + static_cast<std::uint64_t>(hash.below(3));
        den = 16;
        length = parent->size() * den / num;
    }
    std::vector<std::int16_t> samples(length);
    for (std::size_t i = 0; i < length; ++i) {
        // Past the end, a longer sound plays the original again.
        const std::size_t from = static_cast<std::size_t>(i * num / den) % parent->size();
        const std::int32_t value = (*parent)[reverse ? parent->size() - 1 - from : from];
        samples[i] = static_cast<std::int16_t>(std::clamp(value * gain / 100, -32767, 32767));
    }
    return wav(samples);
}

class TestGenerator final : public Generator {
public:
    [[nodiscard]] const GeneratorCapabilities& capabilities() const override {
        return capabilities_;
    }

    Result<Generated> generate(const GenerateRequest& request,
                               const ai::CancelFlag* /*cancel*/) override {
        const std::int64_t seed = request.seed.value_or(0);
        Generated out;
        out.model = std::string(kTest);
        out.seed = seed;
        if (request.type == "image") {
            auto made =
                request.parent != nullptr ? refine_image(request, seed) : make_image(request, seed);
            if (!made) {
                return made.error();
            }
            out.data = std::move(made).value();
            out.format = "png";
            return out;
        }
        if (request.parent != nullptr) {
            auto made = refine_sound(request, seed);
            if (!made) {
                return made.error();
            }
            out.data = std::move(made).value();
        } else {
            out.data = wav(make_samples(request, seed));
        }
        out.format = "wav";
        return out;
    }

private:
    GeneratorCapabilities capabilities_{
        .images = true, .sounds = true, .refines_images = true, .refines_sounds = true};
};

bool makes(const Generator& generator, std::string_view type) {
    return type == "image" ? generator.capabilities().images : generator.capabilities().sounds;
}

Result<std::shared_ptr<Generator>> make_generator(const ai::GeneratorConfig& config) {
    if (config.type == kTest) {
        return test_generator();
    }
    return make_error(ErrorCode::InvalidArgument,
                      "no generator of type \"" + config.type + "\"; this build has: test");
}

} // namespace

std::shared_ptr<Generator> test_generator() {
    return std::make_shared<TestGenerator>();
}

Result<OpenedGenerator> open_generator(const ai::Config& config, std::string_view name,
                                       std::string_view type) {
    std::string chosen(name);
    if (chosen.empty()) {
        std::vector<std::string> candidates;
        for (const auto& [generator_name, settings] : config.generators) {
            auto made = make_generator(settings);
            if (made && makes(*made.value(), type)) {
                candidates.push_back(generator_name);
            }
        }
        if (candidates.empty()) {
            return make_error(ErrorCode::NotFound,
                              "providers.json names no generator that makes " +
                                  std::string(type == "image" ? "images" : "sounds") +
                                  "; add one under \"generators\", or pass generator \"test\" "
                                  "for placeholders");
        }
        if (candidates.size() > 1) {
            std::string list;
            for (const std::string& candidate : candidates) {
                list += (list.empty() ? "" : ", ") + candidate;
            }
            return make_error(ErrorCode::InvalidArgument,
                              "several generators make " +
                                  std::string(type == "image" ? "images" : "sounds") + " (" + list +
                                  "); name one");
        }
        chosen = candidates.front();
    }
    ai::GeneratorConfig settings;
    if (const auto found = config.generators.find(chosen); found != config.generators.end()) {
        settings = found->second;
    } else if (chosen == kTest) {
        settings.type = std::string(kTest);
    } else {
        std::vector<std::string> names{std::string(kTest)};
        for (const auto& [generator_name, unused] : config.generators) {
            names.push_back(generator_name);
        }
        std::string message = "no generator named \"" + chosen + "\"";
        if (const std::string_view hint = closest_match(chosen, names); !hint.empty()) {
            message += "; did you mean \"" + std::string(hint) + "\"?";
        }
        return make_error(ErrorCode::NotFound, std::move(message));
    }
    auto made = make_generator(settings);
    if (!made) {
        return made.error();
    }
    if (!makes(*made.value(), type)) {
        return make_error(ErrorCode::InvalidArgument,
                          "the generator \"" + chosen + "\" makes no " + std::string(type) + "s");
    }
    OpenedGenerator opened;
    opened.generator = std::move(made).value();
    opened.name = chosen;
    opened.type = settings.type;
    opened.model =
        settings.model.empty() && settings.type == kTest ? std::string(kTest) : settings.model;
    opened.license = settings.license;
    if (opened.license.empty() && settings.type == kTest) {
        opened.license = "CC0-1.0";
    }
    return opened;
}

} // namespace slash::library
