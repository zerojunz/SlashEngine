#include "slash/storyboard/fingerprint.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <utility>

namespace slash::storyboard {
namespace {

// Pixels at least this opaque are measured.
constexpr std::uint8_t kOpaque = 128;
// Colors closer than this to a swatch's first color join it, in 0 to 255 RGB.
constexpr double kJoin = 48.0;
// Swatches smaller than this share are left out.
constexpr double kSmallest = 0.005;
// A pixel is on an edge where |gx| + |gy| of the Sobel filter over luma reaches this: a step of
// about a sixth of the range.
constexpr int kEdge = 160;

double round4(double value) {
    constexpr double kPlaces = 10000.0;
    return std::round(value * kPlaces) / kPlaces;
}

int luma(const std::uint8_t* pixel) {
    // Rec. 709's weights in 256ths: 54 + 183 + 19.
    return (54 * pixel[0] + 183 * pixel[1] + 19 * pixel[2] + 128) >> 8;
}

double distance(const std::array<std::uint8_t, 3>& a, const std::array<std::uint8_t, 3>& b) {
    double sum = 0.0;
    for (std::size_t i = 0; i < 3; ++i) {
        const double d = static_cast<double>(a[i]) - static_cast<double>(b[i]);
        sum += d * d;
    }
    return std::sqrt(sum);
}

std::string hex(const std::array<std::uint8_t, 3>& color) {
    std::array<char, 8> text{};
    std::snprintf(text.data(), text.size(), "#%02x%02x%02x", color[0], color[1], color[2]);
    return {text.data(), 7};
}

struct Group {
    std::uint64_t count = 0;
    std::array<std::uint64_t, 3> sum{};
    std::array<std::uint8_t, 3> seed{};

    [[nodiscard]] std::array<std::uint8_t, 3> mean() const {
        std::array<std::uint8_t, 3> out{};
        for (std::size_t i = 0; i < 3; ++i) {
            out[i] = static_cast<std::uint8_t>((sum[i] + count / 2) / count);
        }
        return out;
    }
};

std::vector<Swatch> palette_of(std::span<const std::uint8_t> rgba, std::size_t pixels) {
    // Colors in bins of 16 levels a channel, then bins gathered into swatches by the most used.
    std::map<std::uint32_t, Group> bins;
    for (std::size_t i = 0; i + 3 < rgba.size(); i += 4) {
        if (rgba[i + 3] < kOpaque) {
            continue;
        }
        const std::uint32_t key = (static_cast<std::uint32_t>(rgba[i] >> 4U) << 8U) |
                                  (static_cast<std::uint32_t>(rgba[i + 1] >> 4U) << 4U) |
                                  (rgba[i + 2] >> 4U);
        Group& bin = bins[key];
        ++bin.count;
        for (std::size_t c = 0; c < 3; ++c) {
            bin.sum[c] += rgba[i + c];
        }
    }
    std::vector<std::pair<std::uint32_t, const Group*>> ordered;
    ordered.reserve(bins.size());
    for (const auto& [key, bin] : bins) {
        ordered.emplace_back(key, &bin);
    }
    std::ranges::sort(ordered, [](const auto& a, const auto& b) {
        return a.second->count != b.second->count ? a.second->count > b.second->count
                                                  : a.first < b.first;
    });
    constexpr std::size_t kWorking = kMaxSwatches * 2;
    std::vector<Group> groups;
    for (const auto& [key, bin] : ordered) {
        const std::array<std::uint8_t, 3> color = bin->mean();
        std::size_t nearest = groups.size();
        double best = 0.0;
        for (std::size_t g = 0; g < groups.size(); ++g) {
            const double d = distance(groups[g].seed, color);
            if (nearest == groups.size() || d < best) {
                nearest = g;
                best = d;
            }
        }
        if (nearest == groups.size() || (best > kJoin && groups.size() < kWorking)) {
            groups.push_back({.count = 0, .sum = {}, .seed = color});
            nearest = groups.size() - 1;
        }
        Group& group = groups[nearest];
        group.count += bin->count;
        for (std::size_t c = 0; c < 3; ++c) {
            group.sum[c] += bin->sum[c];
        }
    }
    std::ranges::stable_sort(groups,
                             [](const Group& a, const Group& b) { return a.count > b.count; });
    std::vector<Swatch> out;
    for (const Group& group : groups) {
        const double share = static_cast<double>(group.count) / static_cast<double>(pixels);
        if (out.size() == kMaxSwatches || share < kSmallest) {
            break;
        }
        out.push_back({.color = group.mean(), .share = round4(share)});
    }
    return out;
}

// Each edge pixel's width: the shorter of the runs of edge pixels across and down it.
double line_weight_of(const std::vector<std::uint8_t>& edge, std::uint32_t width,
                      std::uint32_t height) {
    std::vector<std::uint32_t> across(edge.size(), 0);
    for (std::uint32_t y = 0; y < height; ++y) {
        std::uint32_t x = 0;
        while (x < width) {
            if (edge[(std::size_t{y} * width) + x] == 0) {
                ++x;
                continue;
            }
            std::uint32_t end = x;
            while (end < width && edge[(std::size_t{y} * width) + end] != 0) {
                ++end;
            }
            for (std::uint32_t i = x; i < end; ++i) {
                across[(std::size_t{y} * width) + i] = end - x;
            }
            x = end;
        }
    }
    std::uint64_t sum = 0;
    std::uint64_t count = 0;
    for (std::uint32_t x = 0; x < width; ++x) {
        std::uint32_t y = 0;
        while (y < height) {
            if (edge[(std::size_t{y} * width) + x] == 0) {
                ++y;
                continue;
            }
            std::uint32_t end = y;
            while (end < height && edge[(std::size_t{end} * width) + x] != 0) {
                ++end;
            }
            for (std::uint32_t i = y; i < end; ++i) {
                sum += std::min(end - y, across[(std::size_t{i} * width) + x]);
                ++count;
            }
            y = end;
        }
    }
    return count == 0 ? 0.0 : static_cast<double>(sum) / static_cast<double>(count);
}

Error bad(std::string_view member, std::string_view what) {
    return make_error(ErrorCode::InvalidArgument,
                      "/" + std::string(member) + ": " + std::string(what));
}

json::Value number(std::string_view description, double minimum) {
    json::Value schema;
    schema["type"] = "number";
    schema["minimum"] = minimum;
    schema["description"] = description;
    return schema;
}

std::string number_text(double value) {
    return json::write(json::Value(round4(value)));
}

} // namespace

Fingerprint fingerprint(std::span<const std::uint8_t> rgba, std::uint32_t width,
                        std::uint32_t height) {
    Fingerprint out;
    const std::size_t count = std::size_t{width} * height;
    if (rgba.size() < count * 4) {
        return out;
    }
    std::vector<int> lumas(count, -1); // -1 where a pixel isn't measured
    std::array<std::uint64_t, 256> histogram{};
    std::uint64_t sum = 0;
    std::uint64_t squares = 0;
    std::uint64_t saturation = 0;
    std::int64_t warmth = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint8_t* pixel = &rgba[i * 4];
        if (pixel[3] < kOpaque) {
            continue;
        }
        const int l = luma(pixel);
        lumas[i] = l;
        ++histogram.at(static_cast<std::size_t>(l));
        sum += static_cast<std::uint64_t>(l);
        squares += static_cast<std::uint64_t>(l) * static_cast<std::uint64_t>(l);
        const int high = std::max({pixel[0], pixel[1], pixel[2]});
        const int low = std::min({pixel[0], pixel[1], pixel[2]});
        if (high > 0) {
            saturation += static_cast<std::uint64_t>(((high - low) * 255 + high / 2) / high);
        }
        warmth += static_cast<std::int64_t>(pixel[0]) - static_cast<std::int64_t>(pixel[2]);
        ++out.pixels;
    }
    if (out.pixels == 0) {
        return out;
    }
    const auto n = static_cast<std::uint64_t>(out.pixels);
    const auto nd = static_cast<double>(n);
    out.palette = palette_of(rgba.first(count * 4), out.pixels);

    // The percentiles: the first luma whose share of pixels at or below it reaches each.
    constexpr std::array<std::pair<std::uint64_t, std::uint64_t>, 3> kPercentiles = {
        {{1, 20}, {1, 2}, {19, 20}}};
    for (std::size_t p = 0; p < kPercentiles.size(); ++p) {
        std::uint64_t below = 0;
        for (std::size_t l = 0; l < histogram.size(); ++l) {
            below += histogram.at(l);
            if (below * kPercentiles.at(p).second >= n * kPercentiles.at(p).first) {
                out.value.at(p) = round4(static_cast<double>(l) / 255.0);
                break;
            }
        }
    }
    const double mean = static_cast<double>(sum) / nd;
    const double variance = std::max(0.0, (static_cast<double>(squares) / nd) - (mean * mean));
    out.contrast = round4(std::sqrt(variance) / 255.0);
    out.saturation = round4(static_cast<double>(saturation) / nd / 255.0);
    out.warmth = round4(static_cast<double>(warmth) / nd / 255.0);

    // Edges and flat areas, where the pixels around are measured too.
    const auto at = [&](std::uint32_t x, std::uint32_t y) {
        return lumas[(std::size_t{y} * width) + x];
    };
    std::vector<std::uint8_t> edge(count, 0);
    std::uint64_t inside = 0;
    std::uint64_t edges = 0;
    for (std::uint32_t y = 1; y + 1 < height; ++y) {
        for (std::uint32_t x = 1; x + 1 < width; ++x) {
            std::array<int, 9> around{};
            bool all = true;
            for (std::uint32_t j = 0; j < 3 && all; ++j) {
                for (std::uint32_t i = 0; i < 3; ++i) {
                    around.at((j * 3) + i) = at(x + i - 1, y + j - 1);
                    all = all && around.at((j * 3) + i) >= 0;
                }
            }
            if (!all) {
                continue;
            }
            ++inside;
            const int gx = (around[2] + (2 * around[5]) + around[8]) -
                           (around[0] + (2 * around[3]) + around[6]);
            const int gy = (around[6] + (2 * around[7]) + around[8]) -
                           (around[0] + (2 * around[1]) + around[2]);
            if (std::abs(gx) + std::abs(gy) >= kEdge) {
                edge[(std::size_t{y} * width) + x] = 1;
                ++edges;
            }
        }
    }
    if (inside > 0) {
        out.edges = round4(static_cast<double>(edges) / static_cast<double>(inside));
    }
    out.line_weight = round4(line_weight_of(edge, width, height));
    std::uint64_t compared = 0;
    std::uint64_t flat = 0;
    for (std::uint32_t y = 0; y + 1 < height; ++y) {
        for (std::uint32_t x = 0; x + 1 < width; ++x) {
            const int here = at(x, y);
            const int right = at(x + 1, y);
            const int down = at(x, y + 1);
            if (here < 0 || right < 0 || down < 0) {
                continue;
            }
            ++compared;
            if (std::abs(here - right) <= 1 && std::abs(here - down) <= 1) {
                ++flat;
            }
        }
    }
    if (compared > 0) {
        out.flat = round4(static_cast<double>(flat) / static_cast<double>(compared));
    }
    return out;
}

const std::vector<std::string_view>& fingerprint_measures() {
    static const std::vector<std::string_view> kMeasures = {
        "palette", "value",       "contrast", "saturation",   "warmth",
        "edges",   "line_weight", "flat",     "texel_density"};
    return kMeasures;
}

double default_tolerance(std::string_view measure) {
    static const std::map<std::string_view, double, std::less<>> kTolerances = {
        {"palette", 0.1},      {"value", 0.08},  {"contrast", 0.05},
        {"saturation", 0.08},  {"warmth", 0.05}, {"edges", 0.05},
        {"line_weight", 0.75}, {"flat", 0.1},    {"texel_density", 0.5}};
    const auto it = kTolerances.find(measure);
    return it != kTolerances.end() ? it->second : 0.0;
}

json::Value to_json(const Fingerprint& fingerprint) {
    json::Value out;
    json::Value& palette = out["palette"];
    palette = json::Object{};
    for (const Swatch& swatch : fingerprint.palette) {
        palette[hex(swatch.color)] = round4(swatch.share);
    }
    out["value"] = json::Array{round4(fingerprint.value[0]), round4(fingerprint.value[1]),
                               round4(fingerprint.value[2])};
    out["contrast"] = round4(fingerprint.contrast);
    out["saturation"] = round4(fingerprint.saturation);
    out["warmth"] = round4(fingerprint.warmth);
    out["edges"] = round4(fingerprint.edges);
    out["line_weight"] = round4(fingerprint.line_weight);
    out["flat"] = round4(fingerprint.flat);
    if (fingerprint.texel_density) {
        out["texel_density"] = round4(*fingerprint.texel_density);
    }
    out["pixels"] = static_cast<std::int64_t>(fingerprint.pixels);
    return out;
}

Result<Fingerprint> fingerprint_from_json(const json::Value& value) {
    if (!value.is_object()) {
        return make_error(ErrorCode::InvalidArgument, "a fingerprint is an object");
    }
    Fingerprint out;
    if (const json::Value* palette = value.find("palette"); palette != nullptr) {
        if (!palette->is_object()) {
            return bad("palette", "expected {\"#rrggbb\": share}");
        }
        for (const auto& [color, share] : palette->as_object()) {
            if (color.size() != 7 || color[0] != '#' || !share.is_number()) {
                return bad("palette/" + json::pointer_escape(color),
                           "expected \"#rrggbb\": a share");
            }
            Swatch swatch;
            for (std::size_t c = 0; c < 3; ++c) {
                char* end = nullptr;
                const std::string part = color.substr(1 + (c * 2), 2);
                const long channel = std::strtol(part.c_str(), &end, 16);
                if (end != part.c_str() + 2) {
                    return bad("palette/" + json::pointer_escape(color),
                               "colors are written \"#rrggbb\"");
                }
                swatch.color.at(c) = static_cast<std::uint8_t>(channel);
            }
            swatch.share = share.as_double();
            out.palette.push_back(swatch);
        }
        std::ranges::stable_sort(
            out.palette, [](const Swatch& a, const Swatch& b) { return a.share > b.share; });
    }
    if (const json::Value* levels = value.find("value"); levels != nullptr) {
        if (!levels->is_array() || levels->size() != 3 ||
            !std::ranges::all_of(levels->as_array(),
                                 [](const json::Value& item) { return item.is_number(); })) {
            return bad("value", "expected [low, middle, high]");
        }
        for (std::size_t i = 0; i < 3; ++i) {
            out.value.at(i) = levels->as_array()[i].as_double();
        }
    }
    const std::array<std::pair<std::string_view, double*>, 6> kNumbers = {{
        {"contrast", &out.contrast},
        {"saturation", &out.saturation},
        {"warmth", &out.warmth},
        {"edges", &out.edges},
        {"line_weight", &out.line_weight},
        {"flat", &out.flat},
    }};
    for (const auto& [name, field] : kNumbers) {
        if (const json::Value* number = value.find(name); number != nullptr) {
            if (!number->is_number()) {
                return bad(name, "expected a number");
            }
            *field = number->as_double();
        }
    }
    if (const json::Value* density = value.find("texel_density"); density != nullptr) {
        if (!density->is_number()) {
            return bad("texel_density", "expected a number");
        }
        out.texel_density = density->as_double();
    }
    if (const json::Value* pixels = value.find("pixels"); pixels != nullptr && pixels->is_int()) {
        out.pixels = static_cast<std::size_t>(std::max<std::int64_t>(0, pixels->as_int()));
    }
    return out;
}

json::Value fingerprint_schema() {
    json::Value schema;
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["description"] = "The look measured from the reference's frame.";
    json::Value& properties = schema["properties"];
    json::Value& palette = properties["palette"];
    palette["type"] = "object";
    palette["description"] = "The colors its pixels gather into, \"#rrggbb\" to their share.";
    palette["additionalProperties"] = number("A share of the pixels.", 0.0);
    json::Value& value = properties["value"];
    value["type"] = "array";
    value["items"] = number("Luma, 0 to 1.", 0.0);
    value["minItems"] = 3;
    value["maxItems"] = 3;
    value["description"] = "Luma at the 5th, 50th and 95th percentiles.";
    properties["contrast"] = number("RMS contrast: the standard deviation of luma.", 0.0);
    properties["saturation"] = number("The mean HSV saturation.", 0.0);
    json::Value warmth;
    warmth["type"] = "number";
    warmth["description"] = "The mean of red less blue: above 0 warm, below 0 cool.";
    properties["warmth"] = std::move(warmth);
    properties["edges"] = number("The share of pixels on an edge.", 0.0);
    properties["line_weight"] = number("How many pixels wide edges are.", 0.0);
    properties["flat"] = number("The share of pixels the same as their neighbors.", 0.0);
    properties["texel_density"] = number("A model's albedo texels per meter.", 0.0);
    json::Value pixels;
    pixels["type"] = "integer";
    pixels["minimum"] = 0;
    pixels["description"] = "Pixels measured.";
    properties["pixels"] = std::move(pixels);
    return schema;
}

json::Value tolerances_schema() {
    json::Value schema;
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["description"] =
        "How far each measure may drift before style.check fails; missing ones take the defaults.";
    for (const std::string_view measure : fingerprint_measures()) {
        schema["properties"][measure] =
            number("Default " + number_text(default_tolerance(measure)) + ".", 0.0);
    }
    return schema;
}

json::Object drift(const Fingerprint& reference, const Fingerprint& now) {
    json::Object out;
    if (!reference.palette.empty() && !now.palette.empty()) {
        const auto one_way = [](const std::vector<Swatch>& from, const std::vector<Swatch>& to) {
            double sum = 0.0;
            for (const Swatch& swatch : from) {
                double nearest = -1.0;
                for (const Swatch& other : to) {
                    const double d = distance(swatch.color, other.color);
                    nearest = nearest < 0.0 ? d : std::min(nearest, d);
                }
                sum += swatch.share * nearest / 255.0;
            }
            return sum;
        };
        out["palette"] = round4(0.5 * (one_way(reference.palette, now.palette) +
                                       one_way(now.palette, reference.palette)));
    }
    double value = 0.0;
    for (std::size_t i = 0; i < 3; ++i) {
        value = std::max(value, std::abs(reference.value.at(i) - now.value.at(i)));
    }
    out["value"] = round4(value);
    out["contrast"] = round4(std::abs(reference.contrast - now.contrast));
    out["saturation"] = round4(std::abs(reference.saturation - now.saturation));
    out["warmth"] = round4(std::abs(reference.warmth - now.warmth));
    out["edges"] = round4(std::abs(reference.edges - now.edges));
    out["line_weight"] = round4(std::abs(reference.line_weight - now.line_weight));
    out["flat"] = round4(std::abs(reference.flat - now.flat));
    if (reference.texel_density && now.texel_density && *reference.texel_density > 0.0) {
        out["texel_density"] = round4(std::abs(*now.texel_density - *reference.texel_density) /
                                      *reference.texel_density);
    }
    return out;
}

std::string describe_measure(const Fingerprint& fingerprint, std::string_view measure) {
    if (measure == "palette") {
        std::string out;
        constexpr std::size_t kShown = 4;
        for (std::size_t i = 0; i < fingerprint.palette.size() && i < kShown; ++i) {
            const Swatch& swatch = fingerprint.palette[i];
            out += (i == 0 ? "" : ", ") + hex(swatch.color) + " " +
                   std::to_string(static_cast<int>(std::round(swatch.share * 100.0))) + "%";
        }
        return out.empty() ? "no colors" : out;
    }
    if (measure == "value") {
        return "[" + number_text(fingerprint.value[0]) + ", " + number_text(fingerprint.value[1]) +
               ", " + number_text(fingerprint.value[2]) + "]";
    }
    const std::array<std::pair<std::string_view, double>, 6> kNumbers = {{
        {"contrast", fingerprint.contrast},
        {"saturation", fingerprint.saturation},
        {"warmth", fingerprint.warmth},
        {"edges", fingerprint.edges},
        {"line_weight", fingerprint.line_weight},
        {"flat", fingerprint.flat},
    }};
    for (const auto& [name, number] : kNumbers) {
        if (name == measure) {
            return number_text(number);
        }
    }
    if (measure == "texel_density" && fingerprint.texel_density) {
        return number_text(*fingerprint.texel_density);
    }
    return "none";
}

std::string describe_look(const Fingerprint& fingerprint) {
    std::vector<std::string> words;
    if (!fingerprint.palette.empty()) {
        constexpr std::size_t kNamed = 3;
        const std::size_t named = std::min(kNamed, fingerprint.palette.size());
        std::string colors = "mostly ";
        for (std::size_t i = 0; i < named; ++i) {
            if (i > 0) {
                colors += i + 1 == named ? " and " : ", ";
            }
            colors += hex(fingerprint.palette[i].color);
        }
        words.push_back(std::move(colors));
    }
    // A word for where a measure sits: below low, above high, or between.
    const auto band = [](double value, double low, double high, std::string_view below,
                         std::string_view between, std::string_view above) {
        if (value < low) {
            return std::string(below);
        }
        return std::string(value > high ? above : between);
    };
    constexpr double kTint = 0.04;
    words.push_back(band(fingerprint.warmth, -kTint, kTint, "cool", "neutral", "warm"));
    words.push_back(band(fingerprint.value[1], 0.3, 0.7, "dark", "mid-toned", "bright"));
    words.push_back(
        band(fingerprint.contrast, 0.1, 0.25, "low contrast", "medium contrast", "high contrast"));
    words.push_back(band(fingerprint.saturation, 0.2, 0.5, "nearly gray", "muted", "saturated"));
    words.emplace_back(fingerprint.flat > 0.8 ? "flat shading" : "smooth shading");
    if (fingerprint.edges > 0.1) {
        words.emplace_back("strong outlines about " +
                           std::to_string(static_cast<int>(std::round(fingerprint.line_weight))) +
                           " px wide");
    }
    std::string out;
    for (const std::string& word : words) {
        out += (out.empty() ? "" : ", ") + word;
    }
    return out;
}

} // namespace slash::storyboard
