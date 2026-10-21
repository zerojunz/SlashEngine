#include "slash/exporter/icons.hpp"

#include "slash/core/json.hpp"
#include "slash/platform/file.hpp"
#include "slash/render/image.hpp"
#include "slash/runtime/icon.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <map>
#include <utility>

namespace slash::exporter {
namespace {

namespace fs = std::filesystem;

constexpr std::size_t kChannels = 4;

// The .icns images, as Godot's macOS export's icon_infos: PNG ones from the largest, then 32 and
// 16 pixel ones of run-length encoded RGB with an 8-bit alpha mask, which Finder's smallest
// views read.
struct IcnsImage {
    std::string_view type;
    std::string_view mask;
    std::uint32_t size;
};
constexpr std::array<IcnsImage, 10> kIcnsImages{{
    {"ic10", "", 1024}, // 1024x1024, and 512x512@2x
    {"ic09", "", 512},
    {"ic14", "", 512}, // 256x256@2x
    {"ic08", "", 256},
    {"ic13", "", 256}, // 128x128@2x
    {"ic07", "", 128},
    {"ic12", "", 64}, // 32x32@2x
    {"ic11", "", 32}, // 16x16@2x
    {"il32", "l8mk", 32},
    {"is32", "s8mk", 16},
}};

// The .ico images, as Godot's Windows export writes them.
constexpr std::array<std::uint32_t, 6> kIcoSizes{16, 32, 48, 64, 128, 256};

// Android's densities and an adaptive icon layer's pixels at each: 108 dp.
struct Density {
    std::string_view name;
    std::uint32_t layer;
};
constexpr std::array<Density, 5> kDensities{{
    {"mdpi", 108},
    {"hdpi", 162},
    {"xhdpi", 216},
    {"xxhdpi", 324},
    {"xxxhdpi", 432},
}};

constexpr std::uint32_t kFaviconSize = 192;
constexpr std::uint32_t kAppleTouchSize = 180;

void put_be32(std::string& out, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<char>((value >> static_cast<unsigned>(shift)) & 0xffU));
    }
}

void put_le16(std::string& out, std::uint32_t value) {
    out.push_back(static_cast<char>(value & 0xffU));
    out.push_back(static_cast<char>((value >> 8U) & 0xffU));
}

void put_le32(std::string& out, std::uint32_t value) {
    put_le16(out, value & 0xffffU);
    put_le16(out, value >> 16U);
}

// Resizes the icon once for each size asked for.
class Sizes {
public:
    explicit Sizes(const render::Image& icon) : icon_(icon) {}

    const render::Image& at(std::uint32_t size) {
        auto found = images_.find(size);
        if (found == images_.end()) {
            found = images_.emplace(size, render::resize_image(icon_, size, size)).first;
        }
        return found->second;
    }

private:
    const render::Image& icon_;
    std::map<std::uint32_t, render::Image> images_;
};

// Apple's run-length encoding of one channel: a byte under 0x80 is followed by that many plus
// one bytes to copy; one from 0x80 by a byte to repeat that many minus 0x80 plus three times.
void pack_bits(const std::vector<std::uint8_t>& bytes, std::string& out) {
    constexpr std::size_t kLongestRun = 130;
    constexpr std::size_t kLongestCopy = 128;
    std::size_t i = 0;
    const std::size_t count = bytes.size();
    while (i < count) {
        std::size_t run = 1;
        while (i + run < count && run < kLongestRun && bytes[i + run] == bytes[i]) {
            ++run;
        }
        if (run >= 3) {
            out.push_back(static_cast<char>(0x80U + run - 3));
            out.push_back(static_cast<char>(bytes[i]));
            i += run;
            continue;
        }
        const std::size_t start = i;
        std::size_t length = 0;
        // Copied bytes stop where a run of three starts.
        const auto run_starts = [&](std::size_t at) {
            return at + 2 < count && bytes[at] == bytes[at + 1] && bytes[at] == bytes[at + 2];
        };
        while (i < count && length < kLongestCopy && !run_starts(i)) {
            ++i;
            ++length;
        }
        out.push_back(static_cast<char>(length - 1));
        for (std::size_t k = start; k < start + length; ++k) {
            out.push_back(static_cast<char>(bytes[k]));
        }
    }
}

void add_icns_image(std::string& out, std::string_view type, const std::string& data) {
    out.append(type);
    put_be32(out, static_cast<std::uint32_t>(data.size() + 8));
    out.append(data);
}

std::string png_or_empty(const render::Image& image, Result<void>& status) {
    auto png = render::encode_png(image);
    if (!png) {
        status = png.error();
        return {};
    }
    return std::move(png).value();
}

std::string hex(Rgb color) {
    constexpr std::string_view kDigits = "0123456789abcdef";
    std::string out = "#";
    for (const std::uint8_t channel : color) {
        out.push_back(kDigits[channel >> 4U]);
        out.push_back(kDigits[channel & 0xfU]);
    }
    return out;
}

// The icon drawn over a clear square of side pixels, size pixels on a side, in its middle.
render::Image centered(const render::Image& icon, std::uint32_t side, std::uint32_t size) {
    render::Image out{.width = side, .height = side, .pixels = {}};
    out.pixels.assign(static_cast<std::size_t>(side) * side * kChannels, 0);
    const render::Image inner = render::resize_image(icon, size, size);
    const std::uint32_t offset = (side - size) / 2;
    for (std::uint32_t y = 0; y < size; ++y) {
        std::copy_n(
            &inner.pixels[static_cast<std::size_t>(y) * size * kChannels],
            static_cast<std::size_t>(size) * kChannels,
            &out.pixels[((static_cast<std::size_t>(y + offset) * side) + offset) * kChannels]);
    }
    return out;
}

std::string icon_problem(const fs::path& project_dir, const std::string& path) {
    std::error_code error;
    if (!fs::is_regular_file(project_dir / path, error)) {
        return "no file \"" + path + "\"";
    }
    auto bytes = platform::read_file((project_dir / path).string());
    if (!bytes) {
        return bytes.error().message;
    }
    if (auto icon = runtime::decode_icon(bytes.value()); !icon) {
        return "\"" + path + "\" can't be an icon: " + icon.error().message;
    }
    return {};
}

Result<render::Image> read_icon(const fs::path& project_dir, const std::string& path) {
    auto bytes = platform::read_file((project_dir / path).string());
    if (!bytes) {
        return bytes.error();
    }
    auto icon = runtime::decode_icon(bytes.value());
    if (!icon) {
        return make_error(icon.error().code, path + ": " + icon.error().message);
    }
    return icon;
}

} // namespace

std::optional<Rgb> parse_color(std::string_view text) {
    constexpr std::size_t kLength = 7;
    if (text.size() != kLength || text[0] != '#') {
        return std::nullopt;
    }
    const auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F') {
            return c - 'A' + 10;
        }
        return -1;
    };
    Rgb color{};
    for (std::size_t i = 0; i < color.size(); ++i) {
        const int high = digit(text[1 + (i * 2)]);
        const int low = digit(text[2 + (i * 2)]);
        if (high < 0 || low < 0) {
            return std::nullopt;
        }
        color[i] = static_cast<std::uint8_t>((high * 16) + low);
    }
    return color;
}

render::Image flatten_icon(const render::Image& icon, Rgb background) {
    render::Image out{.width = icon.width, .height = icon.height, .pixels = icon.pixels};
    for (std::size_t i = 0; i + 3 < out.pixels.size(); i += kChannels) {
        const std::uint32_t alpha = out.pixels[i + 3];
        for (std::size_t c = 0; c < 3; ++c) {
            out.pixels[i + c] = static_cast<std::uint8_t>(
                ((out.pixels[i + c] * alpha) + (background[c] * (255 - alpha)) + 127) / 255);
        }
        out.pixels[i + 3] = 255;
    }
    return out;
}

render::Image macos_icon(const render::Image& icon) {
    constexpr std::uint32_t kSide = runtime::kIconSize;
    bool opaque = true;
    for (std::size_t i = 3; i < icon.pixels.size() && opaque; i += kChannels) {
        opaque = icon.pixels[i] == 255;
    }
    if (!opaque) {
        return render::resize_image(icon, kSide, kSide);
    }
    // Apple's grid for macOS 11 icons: the shape is 824 pixels of 1024, its corners rounded 185.
    constexpr std::uint32_t kShape = 824;
    constexpr double kRadius = 185.0;
    render::Image out = centered(icon, kSide, kShape);
    const double center = static_cast<double>(kSide) / 2.0;
    const double straight = (static_cast<double>(kShape) / 2.0) - kRadius;
    for (std::uint32_t y = 0; y < kSide; ++y) {
        for (std::uint32_t x = 0; x < kSide; ++x) {
            const double qx = std::abs(static_cast<double>(x) + 0.5 - center) - straight;
            const double qy = std::abs(static_cast<double>(y) + 0.5 - center) - straight;
            const double ox = std::max(qx, 0.0);
            const double oy = std::max(qy, 0.0);
            const double distance =
                std::sqrt((ox * ox) + (oy * oy)) + std::min(std::max(qx, qy), 0.0) - kRadius;
            const double cover = std::clamp(0.5 - distance, 0.0, 1.0);
            std::uint8_t& alpha =
                out.pixels[(((static_cast<std::size_t>(y) * kSide) + x) * kChannels) + 3];
            alpha = static_cast<std::uint8_t>(std::floor((alpha * cover) + 0.5));
        }
    }
    return out;
}

Result<std::string> encode_icns(const render::Image& icon) {
    Sizes sizes(icon);
    std::string images;
    Result<void> status;
    for (const IcnsImage& entry : kIcnsImages) {
        const render::Image& image = sizes.at(entry.size);
        if (entry.mask.empty()) {
            const std::string png = png_or_empty(image, status);
            if (!status) {
                return status.error();
            }
            add_icns_image(images, entry.type, png);
            continue;
        }
        // Each channel packed in turn, then the alpha as it is.
        std::string packed;
        std::string mask;
        const std::size_t pixels = static_cast<std::size_t>(entry.size) * entry.size;
        std::vector<std::uint8_t> channel(pixels);
        for (std::size_t c = 0; c < 3; ++c) {
            for (std::size_t i = 0; i < pixels; ++i) {
                channel[i] = image.pixels[(i * kChannels) + c];
            }
            pack_bits(channel, packed);
        }
        for (std::size_t i = 0; i < pixels; ++i) {
            mask.push_back(static_cast<char>(image.pixels[(i * kChannels) + 3]));
        }
        add_icns_image(images, entry.type, packed);
        add_icns_image(images, entry.mask, mask);
    }
    std::string out = "icns";
    put_be32(out, static_cast<std::uint32_t>(images.size() + 8));
    out += images;
    return out;
}

Result<std::string> encode_ico(const render::Image& icon) {
    Sizes sizes(icon);
    std::vector<std::string> pngs;
    Result<void> status;
    for (const std::uint32_t size : kIcoSizes) {
        pngs.push_back(png_or_empty(sizes.at(size), status));
        if (!status) {
            return status.error();
        }
    }
    constexpr std::uint32_t kHeader = 6;
    constexpr std::uint32_t kEntry = 16;
    std::string out;
    put_le16(out, 0); // reserved
    put_le16(out, 1); // an icon, not a cursor
    put_le16(out, static_cast<std::uint32_t>(pngs.size()));
    auto offset = static_cast<std::uint32_t>(kHeader + (kEntry * pngs.size()));
    for (std::size_t i = 0; i < pngs.size(); ++i) {
        // 256 is written 0.
        out.push_back(static_cast<char>(kIcoSizes[i] & 0xffU));
        out.push_back(static_cast<char>(kIcoSizes[i] & 0xffU));
        out.push_back(0);  // colors in a palette: none
        out.push_back(0);  // reserved
        put_le16(out, 1);  // color planes
        put_le16(out, 32); // bits per pixel
        put_le32(out, static_cast<std::uint32_t>(pngs[i].size()));
        put_le32(out, offset);
        offset += static_cast<std::uint32_t>(pngs[i].size());
    }
    for (const std::string& png : pngs) {
        out += png;
    }
    return out;
}

Result<std::vector<IconFile>> platform_icons(std::string_view platform,
                                             const IconSettings& settings) {
    std::vector<IconFile> files;
    Result<void> status;
    const auto png = [&](std::string path, const render::Image& image) {
        if (status) {
            std::string bytes = png_or_empty(image, status);
            files.push_back({std::move(path), std::move(bytes)});
        }
    };
    if (platform == "macos") {
        auto icns = encode_icns(macos_icon(settings.icon));
        if (!icns) {
            return icns.error();
        }
        files.push_back({"AppIcon.icns", std::move(icns).value()});
    } else if (platform == "windows") {
        auto ico = encode_ico(settings.icon);
        if (!ico) {
            return ico.error();
        }
        files.push_back({"icon.ico", std::move(ico).value()});
    } else if (platform == "web") {
        png("favicon.png", render::resize_image(settings.icon, kFaviconSize, kFaviconSize));
        png("apple-touch-icon.png",
            flatten_icon(render::resize_image(settings.icon, kAppleTouchSize, kAppleTouchSize),
                         settings.opaque_background));
    } else if (platform == "android") {
        // An adaptive icon: launchers mask and move its two layers.
        const bool background_image = settings.android_background_image.has_value();
        files.push_back(
            {"res/mipmap-anydpi-v26/icon.xml",
             std::string("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                         "<adaptive-icon "
                         "xmlns:android=\"http://schemas.android.com/apk/res/android\">\n"
                         "    <background android:drawable=\"") +
                 (background_image ? "@mipmap/icon_background" : "@color/icon_background") +
                 "\" />\n"
                 "    <foreground android:drawable=\"@mipmap/icon_foreground\" />\n"
                 "</adaptive-icon>\n"});
        for (const Density& density : kDensities) {
            const std::string folder = "res/mipmap-" + std::string(density.name) + "/";
            if (settings.android_foreground) {
                png(folder + "icon_foreground.png",
                    render::resize_image(*settings.android_foreground, density.layer,
                                         density.layer));
            } else {
                png(folder + "icon_foreground.png",
                    centered(settings.icon, density.layer, density.layer * 2 / 3));
            }
            if (background_image) {
                png(folder + "icon_background.png",
                    render::resize_image(*settings.android_background_image, density.layer,
                                         density.layer));
            }
        }
        if (!background_image) {
            files.push_back({"res/values/icon_background.xml",
                             "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<resources>\n"
                             "    <color name=\"icon_background\">" +
                                 hex(settings.android_background) + "</color>\n</resources>\n"});
        }
    } else if (platform == "ios") {
        // One 1024-pixel image, which actool makes every size from (Xcode's "single size").
        constexpr std::string_view kInfo = R"("info": {"author": "xcode", "version": 1})";
        files.push_back({"Assets.xcassets/Contents.json", "{" + std::string(kInfo) + "}\n"});
        files.push_back({"Assets.xcassets/AppIcon.appiconset/Contents.json",
                         R"({"images": [{"filename": "icon.png", "idiom": "universal", )"
                         R"("platform": "ios", "size": "1024x1024"}], )" +
                             std::string(kInfo) + "}\n"});
        png("Assets.xcassets/AppIcon.appiconset/icon.png",
            flatten_icon(
                render::resize_image(settings.icon, runtime::kIconSize, runtime::kIconSize),
                settings.opaque_background));
    }
    if (!status) {
        return status.error();
    }
    return files;
}

std::vector<IconProblem> check_icons(const fs::path& project_dir, const runtime::Project& project,
                                     const std::vector<const Preset*>& presets) {
    std::vector<IconProblem> problems;
    const auto check = [&](std::string_view file, std::string path, const std::string& icon) {
        if (icon.empty()) {
            return;
        }
        if (std::string problem = icon_problem(project_dir, icon); !problem.empty()) {
            problems.push_back({std::string(file), std::move(path), std::move(problem)});
        }
    };
    check(runtime::kProjectFile, "/icon", project.icon);
    for (const Preset* preset : presets) {
        const std::string at = "/presets/" + json::pointer_escape(preset->name);
        check(kPresetsFile, at + "/icon", preset->icon);
        check(kPresetsFile, at + "/android/icon_foreground", preset->android.icon_foreground);
        if (!parse_color(preset->android.icon_background)) {
            check(kPresetsFile, at + "/android/icon_background", preset->android.icon_background);
        }
    }
    return problems;
}

Result<PresetIcons> preset_icons(const fs::path& project_dir, const runtime::Project& project,
                                 const Preset& preset) {
    PresetIcons out;
    out.source = preset.icon.empty() ? project.icon : preset.icon;
    if (out.source.empty()) {
        out.settings.icon = runtime::default_icon(runtime::kIconSize);
    } else {
        auto icon = read_icon(project_dir, out.source);
        if (!icon) {
            return icon.error();
        }
        out.settings.icon = std::move(icon).value();
    }
    if (!preset.android.icon_foreground.empty()) {
        auto foreground = read_icon(project_dir, preset.android.icon_foreground);
        if (!foreground) {
            return foreground.error();
        }
        out.settings.android_foreground = std::move(foreground).value();
    }
    if (const std::optional<Rgb> color = parse_color(preset.android.icon_background)) {
        out.settings.android_background = *color;
    } else {
        auto background = read_icon(project_dir, preset.android.icon_background);
        if (!background) {
            return background.error();
        }
        out.settings.android_background_image = std::move(background).value();
    }
    if (const std::optional<Rgb> color = parse_color(preset.ios.icon_background)) {
        out.settings.opaque_background = *color;
    }
    return out;
}

} // namespace slash::exporter
