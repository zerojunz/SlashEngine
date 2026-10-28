#include "slash/storyboard/storyboard.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace slash::storyboard {

namespace {

// How far apart, per channel, two colors of a drafted palette are at least, and the tolerance it
// gets: images drawn in a few flat colors keep them within that.
constexpr int kDraftTolerance = 8;
// The most colors a drafted palette has, and that one draft adds to a palette.
constexpr std::size_t kMaxColors = 16;

using Rgb = std::array<int, 3>;

// Whether a link names the path, or a folder the path is in.
bool covers(std::string_view link, std::string_view path) {
    return path.starts_with(link) && (path.size() == link.size() || path[link.size()] == '/');
}

bool links(const json::Value& entry, std::string_view path) {
    const json::Value* list = entry.find("links");
    return list != nullptr && list->is_array() &&
           std::ranges::any_of(list->as_array(), [&](const json::Value& link) {
               return link.is_string() && covers(link.as_string(), path);
           });
}

bool everywhere(const json::Value& entry) {
    return entry.find("links") == nullptr && entry.find("refs") == nullptr;
}

// Whether a budget covers a path: by its links, or everything with neither links nor refs.
bool budget_covers(const json::Value& entry, std::string_view path) {
    return everywhere(entry) || links(entry, path);
}

std::int64_t integer(const json::Value& entry, std::string_view field) {
    const json::Value* value = entry.find(field);
    return value != nullptr && value->is_int() ? value->as_int() : std::int64_t{0};
}

// The entries of one kind, by ID.
std::vector<std::pair<std::string, const json::Value*>> of_kind(const json::Object& entries,
                                                                std::string_view name) {
    std::vector<std::pair<std::string, const json::Value*>> out;
    for (const auto& [id, entry] : entries) {
        if (const Kind* kind = kind_of(id); kind != nullptr && kind->name == name) {
            out.emplace_back(id, &entry);
        }
    }
    return out;
}

std::string hex_digits(const Rgb& rgb) {
    static constexpr std::string_view kHex = "0123456789abcdef";
    std::string text;
    for (const int channel : rgb) {
        text += kHex[static_cast<std::size_t>(channel) >> 4U];
        text += kHex[static_cast<std::size_t>(channel) & 0xFU];
    }
    return text;
}

// A color named in words, as palettes name theirs: "dark_blue", "light_gray", "orange".
std::string color_name(const Rgb& rgb) {
    const int most = std::max({rgb[0], rgb[1], rgb[2]});
    const int least = std::min({rgb[0], rgb[1], rgb[2]});
    const int range = most - least;
    const int lightness = most + least; // 0 to 510
    if (most == 0 || range * 100 < most * 15) {
        if (lightness < 77) {
            return "black";
        }
        if (lightness > 433) {
            return "white";
        }
        if (lightness < 204) {
            return "dark_gray";
        }
        return lightness > 306 ? "light_gray" : "gray";
    }
    // The hue in degrees, as HSV has it.
    int hue = 0;
    if (most == rgb[0]) {
        hue = (60 * (rgb[1] - rgb[2]) / range + 360) % 360;
    } else if (most == rgb[1]) {
        hue = (60 * (rgb[2] - rgb[0]) / range) + 120;
    } else {
        hue = (60 * (rgb[0] - rgb[1]) / range) + 240;
    }
    static constexpr std::array<std::pair<int, std::string_view>, 9> kHues = {{{15, "red"},
                                                                               {45, "orange"},
                                                                               {70, "yellow"},
                                                                               {160, "green"},
                                                                               {200, "cyan"},
                                                                               {260, "blue"},
                                                                               {290, "purple"},
                                                                               {335, "pink"},
                                                                               {360, "red"}}};
    std::string_view name = "red";
    for (const auto& [below, each] : kHues) {
        if (hue < below) {
            name = each;
            break;
        }
    }
    if (lightness < 153) {
        return "dark_" + std::string(name);
    }
    return lightness > 357 ? "light_" + std::string(name) : std::string(name);
}

// A name for a color not among a palette's names.
std::string free_name(const json::Value& colors, const Rgb& rgb) {
    const std::string base = color_name(rgb);
    std::string name = base;
    for (int n = 2; colors.find(name) != nullptr; ++n) {
        name = base + "_" + std::to_string(n);
    }
    return name;
}

bool parse_color(const json::Value& value, Rgb& rgb) {
    if (!value.is_string()) {
        return false;
    }
    const std::string& text = value.as_string();
    if (text.size() != 7 || text[0] != '#') {
        return false;
    }
    for (std::size_t i = 0; i < 3; ++i) {
        int channel = 0;
        for (std::size_t j = 0; j < 2; ++j) {
            const char c = text[1 + (i * 2) + j];
            int digit = 0;
            if (c >= '0' && c <= '9') {
                digit = c - '0';
            } else if ((c | 0x20) >= 'a' && (c | 0x20) <= 'f') {
                digit = (c | 0x20) - 'a' + 10;
            } else {
                return false;
            }
            channel = (channel * 16) + digit;
        }
        rgb[i] = channel;
    }
    return true;
}

bool near(const Rgb& a, const Rgb& b, int tolerance) {
    return std::abs(a[0] - b[0]) <= tolerance && std::abs(a[1] - b[1]) <= tolerance &&
           std::abs(a[2] - b[2]) <= tolerance;
}

struct Counted {
    Rgb rgb{};
    std::size_t pixels = 0;
};

// An image's opaque colors, the most used first (ties by color), each with its pixel count.
std::vector<Counted> colors_of(const Texture& texture) {
    std::unordered_map<std::uint32_t, std::size_t> counts;
    const std::size_t count = std::size_t{texture.width} * texture.height;
    for (std::size_t i = 0; i < count && (i * 4) + 3 < texture.pixels.size(); ++i) {
        const std::uint8_t* pixel = &texture.pixels[i * 4];
        if (pixel[3] != 0) {
            ++counts[(std::uint32_t{pixel[0]} << 16U) | (std::uint32_t{pixel[1]} << 8U) | pixel[2]];
        }
    }
    std::vector<Counted> out;
    out.reserve(counts.size());
    for (const auto& [key, pixels] : counts) {
        out.push_back({{static_cast<int>(key >> 16U), static_cast<int>((key >> 8U) & 0xFFU),
                        static_cast<int>(key & 0xFFU)},
                       pixels});
    }
    std::ranges::sort(out, [](const Counted& a, const Counted& b) {
        return a.pixels != b.pixels ? a.pixels > b.pixels : a.rgb < b.rgb;
    });
    return out;
}

// Colors gathered into at most limit groups, each the most used color of those within tolerance
// of it, the most used first. Colors no group takes are left out; all_placed says whether any
// was.
std::vector<Counted> grouped(const std::vector<Counted>& colors, int tolerance, std::size_t limit,
                             bool* all_placed = nullptr) {
    std::vector<Counted> groups;
    bool placed = true;
    for (const Counted& color : colors) {
        const auto group = std::ranges::find_if(
            groups, [&](const Counted& each) { return near(each.rgb, color.rgb, tolerance); });
        if (group != groups.end()) {
            group->pixels += color.pixels;
        } else if (groups.size() < limit) {
            groups.push_back(color);
        } else {
            placed = false;
        }
    }
    std::ranges::stable_sort(
        groups, [](const Counted& a, const Counted& b) { return a.pixels > b.pixels; });
    if (all_placed != nullptr) {
        *all_placed = placed;
    }
    return groups;
}

// Text as an ID's name: lowercase letters, digits, '_' and '-', with the rest as one '_'.
std::string slug(std::string_view text) {
    std::string out;
    for (char c : text) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_') {
            out.push_back(c);
        } else if (!out.empty() && out.back() != '_') {
            out.push_back('_');
        }
    }
    while (!out.empty() && out.back() == '_') {
        out.pop_back();
    }
    if (out.size() > kMaxNameLength) {
        out.resize(kMaxNameLength);
    }
    return out.empty() ? "asset" : out;
}

// "wooden_crate" as "Wooden crate".
std::string title(std::string_view name) {
    std::string out(name);
    std::ranges::replace(out, '_', ' ');
    std::ranges::replace(out, '-', ' ');
    if (!out.empty() && out[0] >= 'a' && out[0] <= 'z') {
        out[0] = static_cast<char>(out[0] - 'a' + 'A');
    }
    return out;
}

// A file's name without its folder or extensions.
std::string_view stem(std::string_view path) {
    path = path.substr(path.rfind('/') + 1);
    return path.substr(0, path.find('.'));
}

// The folder a path is in, or "" at the top.
std::string folder(std::string_view path) {
    const std::size_t slash = path.rfind('/');
    return slash == std::string_view::npos ? std::string() : std::string(path.substr(0, slash));
}

// n rounded up to two significant digits.
std::int64_t round_up(std::int64_t n) {
    std::int64_t step = 1;
    while (n / step >= 100) {
        step *= 10;
    }
    return ((n + step - 1) / step) * step;
}

std::int64_t power_of_two(std::int64_t n) {
    std::int64_t out = 1;
    while (out < n) {
        out *= 2;
    }
    return out;
}

// An ID of the kind for name not among entries or taken, which it joins.
std::string free_id(const json::Object& entries, std::set<std::string>& taken,
                    std::string_view kind, std::string_view name) {
    const std::string base = std::string(kind) + "/" + std::string(name);
    std::string id = base;
    for (int n = 2; entries.contains(id) || taken.contains(id); ++n) {
        id = base + "_" + std::to_string(n);
    }
    taken.insert(id);
    return id;
}

json::Value edit_of(const json::Object& entries, const std::string& id, json::Value set) {
    json::Value edit;
    edit["id"] = id;
    const auto existing = entries.find(id);
    edit["version"] =
        existing != entries.end() ? json::Value(version(existing->second)) : json::Value();
    edit["set"] = std::move(set);
    return edit;
}

} // namespace

void check_models(const json::Object& entries, const std::vector<std::string>& models,
                  const std::function<std::optional<ModelFacts>(std::string_view path)>& load,
                  std::vector<FileError>& errors) {
    const auto budgets = of_kind(entries, "budget");
    for (const std::string& path : models) {
        std::vector<std::pair<std::string, const json::Value*>> limits;
        for (const auto& budget : budgets) {
            if ((integer(*budget.second, "triangles") > 0 ||
                 integer(*budget.second, "texture_size") > 0 ||
                 integer(*budget.second, "texel_density") > 0) &&
                budget_covers(*budget.second, path)) {
                limits.push_back(budget);
            }
        }
        if (limits.empty()) {
            continue;
        }
        const std::optional<ModelFacts> facts = load(path);
        if (!facts) {
            continue;
        }
        for (const auto& [id, budget] : limits) {
            const std::int64_t most = integer(*budget, "triangles");
            if (most > 0 && facts->triangles > static_cast<std::size_t>(most)) {
                errors.push_back({path, "",
                                  "has " + std::to_string(facts->triangles) + " triangles; " + id +
                                      " allows " + std::to_string(most)});
            }
            const std::int64_t density = integer(*budget, "texel_density");
            if (density > 0 && facts->texel_density &&
                (*facts->texel_density < static_cast<float>(density) / 2.0F ||
                 *facts->texel_density > static_cast<float>(density) * 2.0F)) {
                errors.push_back({path, "",
                                  "has " + std::to_string(std::lround(*facts->texel_density)) +
                                      " texels a meter; " + id + " asks for " +
                                      std::to_string(density) + ", from " +
                                      std::to_string(density / 2) + " to " +
                                      std::to_string(density * 2)});
            }
            const std::int64_t side = integer(*budget, "texture_size");
            if (side <= 0) {
                continue;
            }
            for (std::size_t i = 0; i < facts->images.size(); ++i) {
                const ModelImageSize& image = facts->images[i];
                if (image.width > side || image.height > side) {
                    errors.push_back({path, "",
                                      "holds image " + std::to_string(i) + ", " +
                                          std::to_string(image.width) + "x" +
                                          std::to_string(image.height) + " pixels; " + id +
                                          " allows " + std::to_string(side) + " on a side"});
                }
            }
        }
    }
}

std::optional<BudgetLimit> budget_for(const json::Object& entries, std::string_view path,
                                      std::string_view field) {
    std::optional<BudgetLimit> tightest;
    for (const auto& [id, budget] : of_kind(entries, "budget")) {
        const std::int64_t most = integer(*budget, field);
        if (most > 0 && budget_covers(*budget, path) && (!tightest || most < tightest->most)) {
            tightest = BudgetLimit{.id = id, .most = most};
        }
    }
    return tightest;
}

json::Array style_edits(const json::Object& entries, const std::vector<Accepted>& accepted) {
    json::Array edits;
    std::set<std::string> taken;

    // Palettes: each covering palette gains the colors its accepted images use and it lacks, as
    // check_textures would find them.
    struct Palette {
        std::string id;
        const json::Value* entry = nullptr;
        std::vector<Rgb> colors;
        int tolerance = 0;
        std::vector<Rgb> added;
    };
    std::vector<Palette> palettes;
    for (const auto& [id, entry] : of_kind(entries, "palette")) {
        Palette& palette = palettes.emplace_back();
        palette.id = id;
        palette.entry = entry;
        palette.tolerance =
            static_cast<int>(std::clamp<std::int64_t>(integer(*entry, "tolerance"), 0, 255));
        if (const json::Value* colors = entry->find("colors");
            colors != nullptr && colors->is_object()) {
            for (const auto& [name, color] : colors->as_object()) {
                if (Rgb rgb{}; parse_color(color, rgb)) {
                    palette.colors.push_back(rgb);
                }
            }
        }
    }
    std::vector<Counted> pooled; // colors of flat images no palette covers
    std::set<std::string> pooled_folders;
    for (const Accepted& asset : accepted) {
        if (!asset.image) {
            continue;
        }
        std::vector<Palette*> covering;
        for (Palette& palette : palettes) {
            if (links(*palette.entry, asset.path)) {
                covering.push_back(&palette);
            }
        }
        const std::vector<Counted> colors = colors_of(*asset.image);
        if (covering.empty()) {
            if (!palettes.empty()) {
                continue;
            }
            bool flat = false;
            const std::vector<Counted> groups = grouped(colors, kDraftTolerance, kMaxColors, &flat);
            if (!flat || groups.empty()) {
                continue;
            }
            for (const Counted& group : groups) {
                const auto same = std::ranges::find_if(pooled, [&](const Counted& each) {
                    return near(each.rgb, group.rgb, kDraftTolerance);
                });
                if (same != pooled.end()) {
                    same->pixels += group.pixels;
                } else {
                    pooled.push_back(group);
                }
            }
            pooled_folders.insert(folder(asset.path));
            continue;
        }
        std::vector<Counted> off;
        for (const Counted& color : colors) {
            if (std::ranges::none_of(covering, [&](const Palette* palette) {
                    return std::ranges::any_of(palette->colors, [&](const Rgb& rgb) {
                        return near(rgb, color.rgb, palette->tolerance);
                    });
                })) {
                off.push_back(color);
            }
        }
        Palette& palette = *covering.front();
        const std::size_t room = kMaxColors - std::min(kMaxColors, palette.added.size());
        for (const Counted& group : grouped(off, palette.tolerance, room)) {
            palette.colors.push_back(group.rgb);
            palette.added.push_back(group.rgb);
        }
    }
    for (const Palette& palette : palettes) {
        if (palette.added.empty()) {
            continue;
        }
        json::Value colors = json::Object{};
        if (const json::Value* had = palette.entry->find("colors");
            had != nullptr && had->is_object()) {
            colors = *had;
        }
        for (const Rgb& rgb : palette.added) {
            colors[free_name(colors, rgb)] = "#" + hex_digits(rgb);
        }
        json::Value set;
        set["colors"] = std::move(colors);
        edits.push_back(edit_of(entries, palette.id, std::move(set)));
    }
    if (!pooled.empty()) {
        std::ranges::stable_sort(
            pooled, [](const Counted& a, const Counted& b) { return a.pixels > b.pixels; });
        pooled.resize(std::min(pooled.size(), kMaxColors));
        json::Value set;
        set["name"] = "Main palette";
        set["summary"] = "The colors of the accepted images.";
        json::Value colors = json::Object{};
        for (const Counted& color : pooled) {
            colors[free_name(colors, color.rgb)] = "#" + hex_digits(color.rgb);
        }
        set["colors"] = std::move(colors);
        set["tolerance"] = kDraftTolerance;
        json::Array folders;
        for (const std::string& each : pooled_folders) {
            // An image at the top of the project links itself.
            if (each.empty()) {
                for (const Accepted& asset : accepted) {
                    if (asset.image && folder(asset.path).empty()) {
                        folders.emplace_back(asset.path);
                    }
                }
            } else {
                folders.emplace_back(each);
            }
        }
        set["links"] = std::move(folders);
        edits.push_back(
            edit_of(entries, free_id(entries, taken, "palette", "main"), std::move(set)));
    }

    // Budgets: what accepted assets no budget limits have at most.
    const auto budgets = of_kind(entries, "budget");
    const auto limited = [&](std::string_view path, std::string_view field) {
        return std::ranges::any_of(budgets, [&](const auto& budget) {
            return integer(*budget.second, field) > 0 && budget_covers(*budget.second, path);
        });
    };
    std::int64_t triangles = 0;
    std::int64_t side = 0;
    for (const Accepted& asset : accepted) {
        if (asset.triangles > 0 && !limited(asset.path, "triangles")) {
            triangles = std::max(triangles, static_cast<std::int64_t>(asset.triangles));
        }
        if (asset.image && !limited(asset.path, "texture_size")) {
            side = std::max(
                {side, std::int64_t{asset.image->width}, std::int64_t{asset.image->height}});
        }
    }
    if (triangles > 0 || side > 0) {
        json::Value set;
        if (triangles > 0) {
            set["triangles"] = round_up(triangles);
        }
        if (side > 0) {
            set["texture_size"] = power_of_two(side);
        }
        const auto all = std::ranges::find_if(
            budgets, [](const auto& budget) { return everywhere(*budget.second); });
        if (all != budgets.end()) {
            edits.push_back(edit_of(entries, all->first, std::move(set)));
        } else {
            set["name"] = "Asset budgets";
            set["summary"] = "What the accepted assets need at most.";
            edits.push_back(
                edit_of(entries, free_id(entries, taken, "budget", "assets"), std::move(set)));
        }
    }

    // References: accepted assets with a reason, so requests follow what worked.
    const auto references = of_kind(entries, "reference");
    for (const Accepted& asset : accepted) {
        if (asset.reason.empty() || std::ranges::any_of(references, [&](const auto& reference) {
                return links(*reference.second, asset.path);
            })) {
            continue;
        }
        const std::string name = slug(stem(asset.path));
        json::Value set;
        set["name"] = asset.description.empty() ? title(name) : asset.description;
        set["summary"] = asset.reason;
        set["links"] = json::Array{asset.path};
        edits.push_back(
            edit_of(entries, free_id(entries, taken, "reference", name), std::move(set)));
    }
    return edits;
}

} // namespace slash::storyboard
