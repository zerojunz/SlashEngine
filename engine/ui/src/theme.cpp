#include "internal.hpp"

#include "slash/core/text.hpp"
#include "slash/render/image.hpp"
#include "slash/ui/ui.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Themes as Godot's Theme resource (scene/resources/theme.cpp) and its default theme
// (scene/theme/default_theme.cpp).
namespace slash::ui {
namespace {

// Each control type and the type it is built on, as Godot's classes.
constexpr std::array<std::pair<std::string_view, std::string_view>, 22> kTypes{{
    {"Control", ""},
    {"Label", "Control"},
    {"BaseButton", "Control"},
    {"Button", "BaseButton"},
    {"TextureButton", "BaseButton"},
    {"ColorRect", "Control"},
    {"Panel", "Control"},
    {"TextureRect", "Control"},
    {"NinePatchRect", "Control"},
    {"Range", "Control"},
    {"ProgressBar", "Range"},
    {"Container", "Control"},
    {"BoxContainer", "Container"},
    {"HBoxContainer", "BoxContainer"},
    {"VBoxContainer", "BoxContainer"},
    {"GridContainer", "Container"},
    {"MarginContainer", "Container"},
    {"CenterContainer", "Container"},
    {"FlowContainer", "Container"},
    {"HFlowContainer", "FlowContainer"},
    {"VFlowContainer", "FlowContainer"},
    {"PanelContainer", "Container"},
}};

constexpr std::array<std::string_view, 5> kKinds{"colors", "constants", "fonts", "font_sizes",
                                                 "styles"};

// Godot's make_flat_stylebox: a rounded rect with content margins and smooth corners.
StyleBox flat_style(Vec4 color, float margin, int corner_radius, bool draw_center = true,
                    int border_width = 0) {
    StyleBox style;
    style.bg_color = color;
    style.content_margin.fill(margin);
    style.corner_radius.fill(corner_radius);
    style.anti_aliasing = true;
    style.corner_detail = std::min(static_cast<int>(std::ceil(1.5 * corner_radius)), 6);
    style.draw_center = draw_center;
    style.border_width.fill(border_width);
    return style;
}

} // namespace

// Godot's default theme, each type's own items.
const std::map<std::string, ThemeItems, std::less<>>& own_defaults() {
    static const std::map<std::string, ThemeItems, std::less<>> kThemes = [] {
        constexpr int kSeparation = 4;
        constexpr float kMargin = 4.0F;
        constexpr int kCornerRadius = 3;
        const Vec4 style_normal_color{0.1F, 0.1F, 0.1F, 0.6F};
        const Vec4 style_hover_color{0.225F, 0.225F, 0.225F, 0.6F};
        const Vec4 style_pressed_color{0.0F, 0.0F, 0.0F, 0.6F};
        const Vec4 style_disabled_color{0.1F, 0.1F, 0.1F, 0.3F};
        const Vec4 style_focus_color{1.0F, 1.0F, 1.0F, 0.75F};
        const Vec4 style_progress_color{1.0F, 1.0F, 1.0F, 0.4F};
        const Vec4 control_font_color{0.875F, 0.875F, 0.875F, 1.0F};
        const Vec4 control_font_hover_color{0.95F, 0.95F, 0.95F, 1.0F};
        const Vec4 control_font_focus_color{0.95F, 0.95F, 0.95F, 1.0F};
        const Vec4 control_font_pressed_color{1.0F, 1.0F, 1.0F, 1.0F};
        const Vec4 control_font_disabled_color{0.875F, 0.875F, 0.875F, 0.5F};
        const Vec4 white{1.0F, 1.0F, 1.0F, 1.0F};
        std::map<std::string, ThemeItems, std::less<>> themes;
        themes["BaseButton"].constants = {{"click_margin", 0}};
        ThemeItems& button = themes["Button"];
        // The focus outline sits just outside the button, so it doesn't cover its content.
        StyleBox focus = flat_style(style_focus_color, kMargin, kCornerRadius, false, 2);
        focus.expand_margin.fill(2.0F);
        button.styles = {{"normal", flat_style(style_normal_color, kMargin, kCornerRadius)},
                         {"hover", flat_style(style_hover_color, kMargin, kCornerRadius)},
                         {"pressed", flat_style(style_pressed_color, kMargin, kCornerRadius)},
                         {"disabled", flat_style(style_disabled_color, kMargin, kCornerRadius)},
                         {"focus", focus}};
        button.colors = {{"font_color", control_font_color},
                         {"font_pressed_color", control_font_pressed_color},
                         {"font_hover_color", control_font_hover_color},
                         {"font_focus_color", control_font_focus_color},
                         {"font_hover_pressed_color", control_font_pressed_color},
                         {"font_disabled_color", control_font_disabled_color},
                         {"font_outline_color", {0.0F, 0.0F, 0.0F, 1.0F}},
                         {"icon_normal_color", white},
                         {"icon_pressed_color", white},
                         {"icon_hover_color", white},
                         {"icon_hover_pressed_color", white},
                         {"icon_focus_color", white},
                         {"icon_disabled_color", {1.0F, 1.0F, 1.0F, 0.4F}}};
        button.constants = {{"outline_size", 0},
                            {"h_separation", kSeparation},
                            {"icon_max_width", 0},
                            {"align_to_largest_stylebox", 0},
                            {"line_spacing", 0}};
        button.fonts = {{"font", ""}};
        button.font_sizes = {{"font_size", text::kDefaultFontSize}};
        ThemeItems& label = themes["Label"];
        label.colors = {{"font_color", {1.0F, 1.0F, 1.0F, 1.0F}},
                        {"font_shadow_color", {0.0F, 0.0F, 0.0F, 0.0F}},
                        {"font_outline_color", {0.0F, 0.0F, 0.0F, 1.0F}}};
        label.constants = {{"line_spacing", 3},    {"paragraph_spacing", 0},
                           {"outline_size", 0},    {"shadow_offset_x", 1},
                           {"shadow_offset_y", 1}, {"shadow_outline_size", 1}};
        label.fonts = {{"font", ""}};
        label.font_sizes = {{"font_size", text::kDefaultFontSize}};
        themes["Panel"].styles = {{"panel", flat_style(style_normal_color, 0.0F, 3)}};
        themes["PanelContainer"].styles = themes["Panel"].styles;
        ThemeItems& bar = themes["ProgressBar"];
        bar.styles = {{"background", flat_style(style_disabled_color, 2.0F, 6)},
                      {"fill", flat_style(style_progress_color, 2.0F, 6)}};
        bar.fonts = {{"font", ""}};
        bar.font_sizes = {{"font_size", text::kDefaultFontSize}};
        bar.colors = {{"font_color", control_font_hover_color},
                      {"font_outline_color", {0.0F, 0.0F, 0.0F, 1.0F}}};
        bar.constants = {{"outline_size", 0}};
        for (const std::string_view box : {"BoxContainer", "HBoxContainer", "VBoxContainer"}) {
            themes[std::string(box)].constants = {{"separation", kSeparation}};
        }
        themes["MarginContainer"].constants = {
            {"margin_left", 0}, {"margin_top", 0}, {"margin_right", 0}, {"margin_bottom", 0}};
        for (const std::string_view grid :
             {"GridContainer", "FlowContainer", "HFlowContainer", "VFlowContainer"}) {
            themes[std::string(grid)].constants = {{"h_separation", kSeparation},
                                                   {"v_separation", kSeparation}};
        }
        return themes;
    }();
    return kThemes;
}

const ThemeItems& optional_items(std::string_view type) {
    static const ThemeItems kNone;
    // Button's hover_pressed style is used when a theme has one, else pressed is, and a state's
    // _mirrored style in place of its own when the button lays out right to left, as Godot's.
    static const ThemeItems kButton = [] {
        ThemeItems items;
        StyleBox empty;
        empty.type = StyleBox::Type::Empty;
        for (const std::string_view state :
             {"hover_pressed", "normal", "hover", "pressed", "disabled"}) {
            if (state == "hover_pressed") {
                items.styles.emplace(state, empty);
            }
            items.styles.emplace(std::string(state) + "_mirrored", empty);
        }
        return items;
    }();
    const std::vector<std::string_view> bases = base_types(type);
    return std::ranges::find(bases, std::string_view("Button")) != bases.end() ? kButton : kNone;
}

namespace {

// Adds the items of from that into doesn't have.
void merge(ThemeItems& into, const ThemeItems& from) {
    into.colors.insert(from.colors.begin(), from.colors.end());
    into.constants.insert(from.constants.begin(), from.constants.end());
    into.fonts.insert(from.fonts.begin(), from.fonts.end());
    into.font_sizes.insert(from.font_sizes.begin(), from.font_sizes.end());
    into.styles.insert(from.styles.begin(), from.styles.end());
}

// The names a kind of item has.
std::vector<std::string_view> names_of(const ThemeItems& items, std::string_view kind) {
    std::vector<std::string_view> names;
    const auto add = [&](const auto& map) {
        for (const auto& [name, value] : map) {
            names.emplace_back(name);
        }
    };
    if (kind == "colors") {
        add(items.colors);
    } else if (kind == "constants") {
        add(items.constants);
    } else if (kind == "fonts") {
        add(items.fonts);
    } else if (kind == "font_sizes") {
        add(items.font_sizes);
    } else {
        add(items.styles);
    }
    return names;
}

std::string quoted(std::string_view text) {
    return "\"" + std::string(text) + "\"";
}

std::string with_hint(std::string message, std::string_view name,
                      const std::vector<std::string_view>& candidates) {
    if (const std::string_view hint = closest_match(name, candidates); !hint.empty()) {
        message += "; did you mean " + quoted(hint) + "?";
    }
    return message;
}

json::Value object_of(json::Value items, std::string_view description) {
    json::Value schema;
    schema["type"] = "object";
    schema["additionalProperties"] = std::move(items);
    schema["description"] = description;
    return schema;
}

std::string font_path_error(const std::string& path) {
    if (path.empty()) {
        return "";
    }
    if (!is_project_path(path)) {
        return "expected a path inside the project folder, separated by '/', with no \".\" or "
               "\"..\" parts";
    }
    if (!text::is_font_path(path)) {
        return "expected a .ttf, .otf or .font.json file";
    }
    return "";
}

Vec4 vec4(const json::Value& value) {
    const json::Array& channels = value.as_array();
    return {
        static_cast<float>(channels[0].as_double()), static_cast<float>(channels[1].as_double()),
        static_cast<float>(channels[2].as_double()), static_cast<float>(channels[3].as_double())};
}

} // namespace

std::vector<std::string_view> base_types(std::string_view type) {
    std::vector<std::string_view> out;
    bool more = !type.empty();
    while (more) {
        more = false;
        for (const auto& [name, base] : kTypes) {
            if (name == type) {
                out.push_back(name);
                type = base;
                more = !type.empty();
                break;
            }
        }
    }
    return out;
}

const std::vector<std::string_view>& control_types() {
    static const std::vector<std::string_view> kNames = [] {
        std::vector<std::string_view> names;
        names.reserve(kTypes.size());
        for (const auto& [name, base] : kTypes) {
            names.push_back(name);
        }
        return names;
    }();
    return kNames;
}

const ThemeItems& default_theme(std::string_view type) {
    static const std::map<std::string, ThemeItems, std::less<>> kMerged = [] {
        std::map<std::string, ThemeItems, std::less<>> merged;
        for (const auto& [name, base] : kTypes) {
            ThemeItems& items = merged[std::string(name)];
            for (const std::string_view each : base_types(name)) {
                if (const auto own = own_defaults().find(each); own != own_defaults().end()) {
                    merge(items, own->second);
                }
            }
        }
        return merged;
    }();
    const auto found = kMerged.find(type);
    return found != kMerged.end() ? found->second : kMerged.find("Control")->second;
}

json::Value theme_schema() {
    json::Value color;
    color["type"] = "array";
    color["items"]["type"] = "number";
    color["items"]["minimum"] = 0;
    color["minItems"] = 4;
    color["maxItems"] = 4;
    json::Value constant;
    constant["type"] = "integer";
    json::Value font;
    font["type"] = "string";
    json::Value font_size;
    font_size["type"] = "integer";
    font_size["minimum"] = 1;
    font_size["maximum"] = text::kMaxFontSize;

    json::Value type;
    type["type"] = "object";
    type["additionalProperties"] = false;
    json::Value& items = type["properties"];
    items["base_type"]["type"] = "string";
    items["base_type"]["description"] =
        "Makes this type a variation of another, e.g. \"Label\": controls whose "
        "theme_type_variation names it use its items, then the base type's.";
    items["colors"] = object_of(color, "Colors by name, as [r, g, b, a] from 0 to 1.");
    items["constants"] = object_of(constant, "Constants by name, e.g. \"separation\": 8.");
    items["fonts"] = object_of(font, "Fonts by name: a .ttf, .otf or .font.json file; \"\" is "
                                     "the built-in font.");
    items["font_sizes"] = object_of(font_size, "Font sizes by name, in pixels.");
    items["styles"] = object_of(style_box_schema(), "Style boxes by name.");

    json::Value schema;
    schema["$schema"] = "https://json-schema.org/draft/2020-12/schema";
    schema["title"] = "SlashEngine theme";
    schema["description"] = "Theme items by control type, as Godot's Theme. A control uses its "
                            "theme_override_* items, then its own and its parents' themes, the "
                            "project's theme and the default theme.";
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["required"] = json::Array{"format"};
    json::Value& properties = schema["properties"];
    properties["format"]["const"] = 1;
    properties["format"]["description"] = "Format version. Must be 1.";
    properties["default_font"]["type"] = "string";
    properties["default_font"]["description"] =
        "The font of every type in this theme that names none: a .ttf, .otf or .font.json file.";
    properties["default_font_size"] = font_size;
    properties["default_font_size"]["description"] =
        "The font size of every type in this theme that names none.";
    properties["types"] = object_of(type, "Items by type: a control type such as \"Label\" or "
                                          "\"PanelContainer\", or a variation with a base_type.");
    return schema;
}

std::vector<json::ValidationError> validate_theme(const json::Value& doc) {
    std::vector<json::ValidationError> errors = json::validate(doc, theme_schema());
    if (!errors.empty()) {
        return errors;
    }
    if (const json::Value* font = doc.find("default_font"); font != nullptr) {
        if (std::string error = font_path_error(font->as_string()); !error.empty()) {
            errors.push_back({"/default_font", std::move(error)});
        }
    }
    const json::Value* types = doc.find("types");
    if (types == nullptr) {
        return errors;
    }
    const json::Object& all = types->as_object();
    for (const auto& [name, type] : all) {
        const std::string path = "/types/" + json::pointer_escape(name);
        // Down the variations to the control type they lead to.
        std::string_view native;
        std::set<std::string_view> seen;
        std::string_view at = name;
        std::string at_path = path;
        while (true) {
            if (!base_types(at).empty()) {
                native = at;
                break;
            }
            const auto found = all.find(at);
            const json::Value* base =
                found != all.end() ? found->second.find("base_type") : nullptr;
            if (found == all.end() || base == nullptr) {
                if (at == name) {
                    std::string message = quoted(name) + " isn't a control type";
                    if (closest_match(name, control_types()).empty()) {
                        message += "; give it a base_type to make it a variation";
                    } else {
                        message = with_hint(std::move(message), name, control_types());
                    }
                    errors.push_back({path, std::move(message)});
                }
                break;
            }
            if (!seen.insert(at).second) {
                errors.push_back({path + "/base_type",
                                  "the base types of " + quoted(name) + " go round in a loop"});
                break;
            }
            const std::string& next = base->as_string();
            if (base_types(next).empty() && !all.contains(next)) {
                std::vector<std::string_view> candidates = control_types();
                for (const auto& [other, value] : all) {
                    candidates.emplace_back(other);
                }
                errors.push_back(
                    {at_path + "/base_type",
                     with_hint(quoted(next) + " isn't a control type or a type in this theme", next,
                               candidates)});
                break;
            }
            at = next;
            at_path = "/types/" + json::pointer_escape(next);
        }
        ThemeItems known = default_theme(native.empty() ? "Control" : native);
        merge(known, optional_items(native));
        for (const std::string_view kind : kKinds) {
            const json::Value* items = type.find(kind);
            if (items == nullptr) {
                continue;
            }
            const std::vector<std::string_view> names = names_of(known, kind);
            for (const auto& [item, value] : items->as_object()) {
                const std::string item_path =
                    path + "/" + std::string(kind) + "/" + json::pointer_escape(item);
                if (!native.empty() && std::ranges::find(names, item) == names.end()) {
                    std::string message = std::string(native) + " has no " + std::string(kind) +
                                          " item " + quoted(item);
                    if (names.empty()) {
                        message += "; it has none";
                    } else {
                        message = with_hint(std::move(message), item, names);
                    }
                    errors.push_back({item_path, std::move(message)});
                    continue;
                }
                if (kind == "styles") {
                    for (json::ValidationError& error : validate_style_box(value)) {
                        errors.push_back({item_path + error.path, std::move(error.message)});
                    }
                } else if (kind == "fonts") {
                    if (std::string error = font_path_error(value.as_string()); !error.empty()) {
                        errors.push_back({item_path, std::move(error)});
                    }
                }
            }
        }
    }
    return errors;
}

Result<Theme> parse_theme(const json::Value& doc) {
    if (const auto errors = validate_theme(doc); !errors.empty()) {
        const json::ValidationError& first = errors.front();
        return make_error(ErrorCode::InvalidArgument,
                          (first.path.empty() ? std::string("root") : first.path) + ": " +
                              first.message);
    }
    Theme out;
    if (const json::Value* font = doc.find("default_font"); font != nullptr) {
        out.default_font = font->as_string();
    }
    if (const json::Value* size = doc.find("default_font_size"); size != nullptr) {
        out.default_font_size = static_cast<int>(size->as_int());
    }
    const json::Value* types = doc.find("types");
    if (types == nullptr) {
        return out;
    }
    for (const auto& [name, value] : types->as_object()) {
        Theme::Type& type = out.types[name];
        if (const json::Value* base = value.find("base_type"); base != nullptr) {
            type.base_type = base->as_string();
        }
        if (const json::Value* items = value.find("colors"); items != nullptr) {
            for (const auto& [item, color] : items->as_object()) {
                type.items.colors.emplace(item, vec4(color));
            }
        }
        if (const json::Value* items = value.find("constants"); items != nullptr) {
            for (const auto& [item, constant] : items->as_object()) {
                type.items.constants.emplace(item, static_cast<int>(constant.as_int()));
            }
        }
        if (const json::Value* items = value.find("fonts"); items != nullptr) {
            for (const auto& [item, font] : items->as_object()) {
                type.items.fonts.emplace(item, font.as_string());
            }
        }
        if (const json::Value* items = value.find("font_sizes"); items != nullptr) {
            for (const auto& [item, size] : items->as_object()) {
                type.items.font_sizes.emplace(item, static_cast<int>(size.as_int()));
            }
        }
        if (const json::Value* items = value.find("styles"); items != nullptr) {
            for (const auto& [item, style] : items->as_object()) {
                type.items.styles.emplace(item, parse_style_box(style).value());
            }
        }
    }
    return out;
}

void add_asset_types(asset::Assets& assets) {
    text::add_asset_types(assets);
    assets.add_type<render::Image>("texture", {".png", ".bmp"},
                                   [](std::string_view bytes, std::string_view /*path*/) {
                                       return render::decode_image(bytes);
                                   });
    assets.add_type<Theme>("theme", {std::string(kThemeSuffix)},
                           [](std::string_view text, std::string_view /*path*/) -> Result<Theme> {
                               auto doc = json::parse(text);
                               if (!doc) {
                                   return doc.error();
                               }
                               return parse_theme(doc.value());
                           });
}

} // namespace slash::ui
