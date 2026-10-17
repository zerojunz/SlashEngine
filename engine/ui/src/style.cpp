#include "slash/core/text.hpp"
#include "slash/ui/ui.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Style boxes as Godot's StyleBoxFlat, StyleBoxTexture, StyleBoxLine and StyleBoxEmpty
// (scene/resources/style_box*.cpp), read from JSON with Godot's property names.
namespace slash::ui {
namespace {

constexpr std::array<std::string_view, 4> kSides{"left", "top", "right", "bottom"};
constexpr std::array<std::string_view, 4> kCorners{"top_left", "top_right", "bottom_right",
                                                   "bottom_left"};
constexpr std::array<std::pair<std::string_view, StyleBox::Type>, 4> kTypes{{
    {"flat", StyleBox::Type::Flat},
    {"texture", StyleBox::Type::Texture},
    {"line", StyleBox::Type::Line},
    {"empty", StyleBox::Type::Empty},
}};
constexpr std::array<std::pair<std::string_view, StyleBox::Stretch>, 3> kStretches{{
    {"stretch", StyleBox::Stretch::Stretch},
    {"tile", StyleBox::Stretch::Tile},
    {"tile_fit", StyleBox::Stretch::TileFit},
}};
constexpr std::array<std::string_view, 2> kTextureSuffixes{".png", ".bmp"};

// Which types a property applies to.
enum Applies : std::uint8_t { kAll = 15, kFlat = 1, kTexture = 2, kFlatAndTexture = 3, kLine = 8 };

std::uint8_t type_bit(StyleBox::Type type) {
    switch (type) {
    case StyleBox::Type::Flat:
        return kFlat;
    case StyleBox::Type::Texture:
        return kTexture;
    case StyleBox::Type::Line:
        return kLine;
    case StyleBox::Type::Empty:
        break;
    }
    return 4;
}

std::string_view type_name(StyleBox::Type type) {
    for (const auto& [name, each] : kTypes) {
        if (each == type) {
            return name;
        }
    }
    return "flat";
}

json::Value number(std::string_view description, std::optional<double> minimum = std::nullopt,
                   std::optional<double> maximum = std::nullopt, bool integer = false) {
    json::Value schema;
    schema["type"] = integer ? "integer" : "number";
    if (minimum) {
        schema["minimum"] = *minimum;
    }
    if (maximum) {
        schema["maximum"] = *maximum;
    }
    schema["description"] = description;
    return schema;
}

json::Value numbers(std::string_view description, std::size_t count,
                    std::optional<double> minimum = std::nullopt) {
    json::Value schema;
    schema["type"] = "array";
    schema["items"]["type"] = "number";
    if (minimum) {
        schema["items"]["minimum"] = *minimum;
    }
    schema["minItems"] = count;
    schema["maxItems"] = count;
    schema["description"] = description;
    return schema;
}

json::Value boolean(std::string_view description) {
    json::Value schema;
    schema["type"] = "boolean";
    schema["description"] = description;
    return schema;
}

template <typename T, std::size_t N>
json::Value choice(std::string_view description,
                   const std::array<std::pair<std::string_view, T>, N>& names) {
    json::Value schema;
    schema["type"] = "string";
    json::Array values;
    for (const auto& [name, value] : names) {
        values.emplace_back(name);
    }
    schema["enum"] = std::move(values);
    schema["default"] = names.front().first;
    schema["description"] = description;
    return schema;
}

struct Property {
    std::string name;
    std::uint8_t applies = kAll;
    json::Value schema;
};

// A property for all four sides or corners, then one for each.
void add_four(std::vector<Property>& out, std::string_view base,
              std::span<const std::string_view, 4> parts, std::uint8_t applies,
              const json::Value& schema, std::string_view what) {
    json::Value all = schema;
    all["description"] = std::string(what) + ", for all four; the named ones override it.";
    out.push_back({std::string(base), applies, std::move(all)});
    for (const std::string_view part : parts) {
        json::Value one = schema;
        one["description"] = std::string(what) + ".";
        out.push_back({std::string(base) + "_" + std::string(part), applies, std::move(one)});
    }
}

const std::vector<Property>& properties() {
    static const std::vector<Property> kProperties = [] {
        std::vector<Property> out;
        out.push_back({"type", kAll,
                       choice("flat draws rects with rounded corners, borders and a shadow; "
                              "texture a texture cut into nine parts; line a line across the "
                              "top of the rect, or down its left side; empty nothing.",
                              kTypes)});
        add_four(out, "content_margin", kSides, kAll, number("", -1.0),
                 "The content's margin in pixels; -1 takes the borders' or texture margins'");
        add_four(out, "expand_margin", kSides, kFlatAndTexture, number(""),
                 "Pixels drawn outside the rect");
        out.push_back({"draw_center", kFlatAndTexture,
                       boolean("Draw the middle, not only the "
                               "borders.")});
        out.push_back({"bg_color", kFlat, numbers("As [r, g, b, a] from 0 to 1.", 4, 0.0)});
        add_four(out, "border_width", kSides, kFlat, number("", 0.0, std::nullopt, true),
                 "The border's width in pixels");
        out.push_back(
            {"border_color", kFlat, numbers("The borders' color, as [r, g, b, a].", 4, 0.0)});
        out.push_back({"border_blend", kFlat,
                       boolean("Blend the borders' inner edge into the middle's color.")});
        add_four(out, "corner_radius", kCorners, kFlat, number("", 0.0, std::nullopt, true),
                 "The corner's radius in pixels");
        out.push_back({"corner_detail", kFlat,
                       number("Points around each corner: more is smoother.", 1.0, 20.0, true)});
        out.push_back(
            {"shadow_color", kFlat, numbers("The shadow's color, as [r, g, b, a].", 4, 0.0)});
        out.push_back(
            {"shadow_size", kFlat,
             number("How far the shadow fades out, in pixels.", 0.0, std::nullopt, true)});
        out.push_back(
            {"shadow_offset", kFlat, numbers("Where the shadow is, [x, y] in pixels.", 2)});
        out.push_back({"skew", kFlat,
                       numbers("Slants it, [x, y]: x 1 moves the top edge right by half the "
                               "height and the bottom edge left.",
                               2)});
        out.push_back(
            {"anti_aliasing", kFlat, boolean("Smooth the edges of rounded corners and slants.")});
        out.push_back(
            {"anti_aliasing_size", kFlat, number("Pixels the smoothing fades over.", 0.01, 10.0)});
        json::Value texture;
        texture["type"] = "string";
        texture["description"] = "Project path of a PNG or BMP file.";
        out.push_back({"texture", kTexture, std::move(texture)});
        out.push_back({"region_rect", kTexture,
                       numbers("The part of the texture used, [x, y, width, height] in pixels; a "
                               "zero size uses the whole texture.",
                               4, 0.0)});
        add_four(out, "texture_margin", kSides, kTexture, number("", 0.0),
                 "Pixels of the texture on this side that don't stretch");
        out.push_back({"axis_stretch_horizontal", kTexture,
                       choice("How the middle fills the rect across: stretched, tiled, or tiled "
                              "with tiles stretched to fit a whole number.",
                              kStretches)});
        out.push_back({"axis_stretch_vertical", kTexture,
                       choice("How the middle fills the rect down.", kStretches)});
        out.push_back(
            {"modulate_color", kTexture, numbers("Tints the texture, as [r, g, b, a].", 4, 0.0)});
        out.push_back({"color", kLine, numbers("The line's color, as [r, g, b, a].", 4, 0.0)});
        out.push_back(
            {"thickness", kLine, number("The line's width in pixels.", 0.0, std::nullopt, true)});
        out.push_back(
            {"grow_begin", kLine, number("Pixels the line reaches before the rect's start.")});
        out.push_back({"grow_end", kLine, number("Pixels the line reaches past the rect's end.")});
        out.push_back({"vertical", kLine, boolean("Run down the rect's left side.")});
        return out;
    }();
    return kProperties;
}

const Property* find_property(std::string_view name) {
    for (const Property& property : properties()) {
        if (property.name == name) {
            return &property;
        }
    }
    return nullptr;
}

StyleBox::Type type_of(const json::Value& doc) {
    if (const json::Value* type = doc.find("type"); type != nullptr && type->is_string()) {
        for (const auto& [name, each] : kTypes) {
            if (name == type->as_string()) {
                return each;
            }
        }
    }
    return StyleBox::Type::Flat;
}

Vec4 vec4(const json::Value& value) {
    const json::Array& channels = value.as_array();
    return {
        static_cast<float>(channels[0].as_double()), static_cast<float>(channels[1].as_double()),
        static_cast<float>(channels[2].as_double()), static_cast<float>(channels[3].as_double())};
}

Vec2 point(const json::Value& value) {
    const json::Array& xy = value.as_array();
    return {static_cast<float>(xy[0].as_double()), static_cast<float>(xy[1].as_double())};
}

// Reads base, then base_<part> for each part, into four values.
template <typename T>
void read_four(const json::Value& doc, std::string_view base,
               std::span<const std::string_view, 4> parts, std::array<T, 4>& out) {
    const auto as = [](const json::Value& value) {
        if constexpr (std::is_same_v<T, int>) {
            return static_cast<int>(value.as_int());
        } else {
            return static_cast<float>(value.as_double());
        }
    };
    if (const json::Value* all = doc.find(base); all != nullptr) {
        out.fill(as(*all));
    }
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const std::string name = std::string(base) + "_" + std::string(parts[i]);
        if (const json::Value* one = doc.find(name); one != nullptr) {
            out.at(i) = as(*one);
        }
    }
}

// A float as JSON, without the noise of its binary fraction: 0.1F is 0.1.
json::Value decimal(float value) {
    return std::round(static_cast<double>(value) * 1e6) / 1e6;
}

json::Value color_json(Vec4 c) {
    return json::Array{decimal(c.x), decimal(c.y), decimal(c.z), decimal(c.w)};
}

// Writes four values as base when they are all the same, else the ones that differ from the
// default as base_<part>.
template <typename T>
void write_four(json::Value& out, std::string_view base, std::span<const std::string_view, 4> parts,
                const std::array<T, 4>& values, T fallback) {
    const bool same = values[0] == values[1] && values[1] == values[2] && values[2] == values[3];
    if (same) {
        if (values[0] != fallback) {
            if constexpr (std::is_same_v<T, int>) {
                out[base] = values[0];
            } else {
                out[base] = decimal(values[0]);
            }
        }
        return;
    }
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (values.at(i) == fallback) {
            continue;
        }
        const std::string name = std::string(base) + "_" + std::string(parts[i]);
        if constexpr (std::is_same_v<T, int>) {
            out[name] = values.at(i);
        } else {
            out[name] = decimal(values.at(i));
        }
    }
}

} // namespace

float StyleBox::margin(std::size_t side) const {
    const float content = content_margin.at(side);
    if (content >= 0.0F) {
        return content;
    }
    switch (type) {
    case Type::Flat:
        return static_cast<float>(border_width.at(side));
    case Type::Texture:
        return texture_margin.at(side);
    case Type::Line:
        // Half the line across it, on each side.
        if ((side % 2 == 1) != vertical) {
            return static_cast<float>(thickness) / 2.0F;
        }
        break;
    case Type::Empty:
        break;
    }
    return 0.0F;
}

Vec2 StyleBox::minimum_size() const {
    return {margin(0) + margin(2), margin(1) + margin(3)};
}

json::Value style_box_schema() {
    json::Value schema;
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["description"] = "A style box, as Godot's StyleBoxFlat, StyleBoxTexture, StyleBoxLine "
                            "and StyleBoxEmpty. Sides are left, top, right and bottom.";
    json::Value& out = schema["properties"];
    for (const Property& property : properties()) {
        json::Value each = property.schema;
        if (property.applies != kAll) {
            std::string description = each["description"].as_string();
            if (property.applies == kFlat) {
                description += " (flat)";
            } else if (property.applies == kTexture) {
                description += " (texture)";
            } else if (property.applies == kLine) {
                description += " (line)";
            } else {
                description += " (flat and texture)";
            }
            each["description"] = std::move(description);
        }
        out[property.name] = std::move(each);
    }
    return schema;
}

std::vector<json::ValidationError> validate_style_box(const json::Value& doc) {
    std::vector<json::ValidationError> errors = json::validate(doc, style_box_schema());
    if (!errors.empty()) {
        return errors;
    }
    const StyleBox::Type type = type_of(doc);
    for (const auto& [name, value] : doc.as_object()) {
        const Property* property = find_property(name);
        if (property != nullptr && (property->applies & type_bit(type)) == 0) {
            std::string message = std::string(type_name(type)) + " style boxes have no " + name;
            if ((property->applies & kTexture) != 0) {
                message += R"(; it is for "type": "texture")";
            } else if ((property->applies & kFlat) != 0) {
                message += R"(; it is for "type": "flat")";
            } else if ((property->applies & kLine) != 0) {
                message += R"(; it is for "type": "line")";
            }
            errors.push_back({"/" + json::pointer_escape(name), std::move(message)});
        }
    }
    if (type == StyleBox::Type::Texture) {
        const json::Value* texture = doc.find("texture");
        if (texture == nullptr || texture->as_string().empty()) {
            errors.push_back({"", "a texture style box needs a texture"});
        } else if (const std::string& path = texture->as_string(); !is_project_path(path)) {
            errors.push_back({"/texture", "expected a path inside the project folder, separated "
                                          "by '/', with no \".\" or \"..\" parts"});
        } else if (!path.ends_with(kTextureSuffixes[0]) && !path.ends_with(kTextureSuffixes[1])) {
            errors.push_back({"/texture", "expected a .png or .bmp file"});
        }
    }
    return errors;
}

Result<StyleBox> parse_style_box(const json::Value& doc) {
    if (const auto errors = validate_style_box(doc); !errors.empty()) {
        const json::ValidationError& first = errors.front();
        return make_error(ErrorCode::InvalidArgument,
                          (first.path.empty() ? std::string("root") : first.path) + ": " +
                              first.message);
    }
    StyleBox out;
    out.type = type_of(doc);
    read_four(doc, "content_margin", kSides, out.content_margin);
    read_four(doc, "expand_margin", kSides, out.expand_margin);
    if (const json::Value* value = doc.find("draw_center"); value != nullptr) {
        out.draw_center = value->as_bool();
    }
    if (const json::Value* value = doc.find("bg_color"); value != nullptr) {
        out.bg_color = vec4(*value);
    }
    read_four(doc, "border_width", kSides, out.border_width);
    if (const json::Value* value = doc.find("border_color"); value != nullptr) {
        out.border_color = vec4(*value);
    }
    if (const json::Value* value = doc.find("border_blend"); value != nullptr) {
        out.border_blend = value->as_bool();
    }
    read_four(doc, "corner_radius", kCorners, out.corner_radius);
    if (const json::Value* value = doc.find("corner_detail"); value != nullptr) {
        out.corner_detail = static_cast<int>(value->as_int());
    }
    if (const json::Value* value = doc.find("shadow_color"); value != nullptr) {
        out.shadow_color = vec4(*value);
    }
    if (const json::Value* value = doc.find("shadow_size"); value != nullptr) {
        out.shadow_size = static_cast<int>(value->as_int());
    }
    if (const json::Value* value = doc.find("shadow_offset"); value != nullptr) {
        out.shadow_offset = point(*value);
    }
    if (const json::Value* value = doc.find("skew"); value != nullptr) {
        out.skew = point(*value);
    }
    if (const json::Value* value = doc.find("anti_aliasing"); value != nullptr) {
        out.anti_aliasing = value->as_bool();
    }
    if (const json::Value* value = doc.find("anti_aliasing_size"); value != nullptr) {
        out.anti_aliasing_size = static_cast<float>(value->as_double());
    }
    if (const json::Value* value = doc.find("texture"); value != nullptr) {
        out.texture = value->as_string();
    }
    if (const json::Value* value = doc.find("region_rect"); value != nullptr) {
        out.region_rect = vec4(*value);
    }
    read_four(doc, "texture_margin", kSides, out.texture_margin);
    for (const auto& [name, field] :
         {std::pair{"axis_stretch_horizontal", &out.axis_stretch_horizontal},
          std::pair{"axis_stretch_vertical", &out.axis_stretch_vertical}}) {
        if (const json::Value* value = doc.find(name); value != nullptr) {
            for (const auto& [each, stretch] : kStretches) {
                if (each == value->as_string()) {
                    *field = stretch;
                }
            }
        }
    }
    if (const json::Value* value = doc.find("modulate_color"); value != nullptr) {
        out.modulate_color = vec4(*value);
    }
    if (const json::Value* value = doc.find("color"); value != nullptr) {
        out.color = vec4(*value);
    }
    if (const json::Value* value = doc.find("thickness"); value != nullptr) {
        out.thickness = static_cast<int>(value->as_int());
    }
    if (const json::Value* value = doc.find("grow_begin"); value != nullptr) {
        out.grow_begin = static_cast<float>(value->as_double());
    }
    if (const json::Value* value = doc.find("grow_end"); value != nullptr) {
        out.grow_end = static_cast<float>(value->as_double());
    }
    if (const json::Value* value = doc.find("vertical"); value != nullptr) {
        out.vertical = value->as_bool();
    }
    return out;
}

json::Value style_box_json(const StyleBox& style) {
    const StyleBox defaults;
    json::Value out = json::Object{};
    if (style.type != StyleBox::Type::Flat) {
        out["type"] = type_name(style.type);
    }
    write_four(out, "content_margin", kSides, style.content_margin, -1.0F);
    if (style.type == StyleBox::Type::Empty) {
        return out;
    }
    if (style.type == StyleBox::Type::Line) {
        if (style.color != defaults.color) {
            out["color"] = color_json(style.color);
        }
        if (style.thickness != defaults.thickness) {
            out["thickness"] = style.thickness;
        }
        if (style.grow_begin != defaults.grow_begin) {
            out["grow_begin"] = decimal(style.grow_begin);
        }
        if (style.grow_end != defaults.grow_end) {
            out["grow_end"] = decimal(style.grow_end);
        }
        if (style.vertical) {
            out["vertical"] = true;
        }
        return out;
    }
    write_four(out, "expand_margin", kSides, style.expand_margin, 0.0F);
    if (style.draw_center != defaults.draw_center) {
        out["draw_center"] = style.draw_center;
    }
    if (style.type == StyleBox::Type::Texture) {
        out["texture"] = style.texture;
        if (style.region_rect != defaults.region_rect) {
            out["region_rect"] = color_json(style.region_rect);
        }
        write_four(out, "texture_margin", kSides, style.texture_margin, 0.0F);
        for (const auto& [name, value] :
             {std::pair{"axis_stretch_horizontal", style.axis_stretch_horizontal},
              std::pair{"axis_stretch_vertical", style.axis_stretch_vertical}}) {
            if (value != StyleBox::Stretch::Stretch) {
                for (const auto& [each, stretch] : kStretches) {
                    if (stretch == value) {
                        out[name] = each;
                    }
                }
            }
        }
        if (style.modulate_color != defaults.modulate_color) {
            out["modulate_color"] = color_json(style.modulate_color);
        }
        return out;
    }
    if (style.bg_color != defaults.bg_color) {
        out["bg_color"] = color_json(style.bg_color);
    }
    write_four(out, "border_width", kSides, style.border_width, 0);
    if (style.border_color != defaults.border_color) {
        out["border_color"] = color_json(style.border_color);
    }
    if (style.border_blend) {
        out["border_blend"] = true;
    }
    write_four(out, "corner_radius", kCorners, style.corner_radius, 0);
    if (style.corner_detail != defaults.corner_detail) {
        out["corner_detail"] = style.corner_detail;
    }
    if (style.shadow_color != defaults.shadow_color) {
        out["shadow_color"] = color_json(style.shadow_color);
    }
    if (style.shadow_size != 0) {
        out["shadow_size"] = style.shadow_size;
    }
    if (style.shadow_offset != defaults.shadow_offset) {
        out["shadow_offset"] =
            json::Array{decimal(style.shadow_offset.x), decimal(style.shadow_offset.y)};
    }
    if (style.skew != defaults.skew) {
        out["skew"] = json::Array{decimal(style.skew.x), decimal(style.skew.y)};
    }
    if (!style.anti_aliasing) {
        out["anti_aliasing"] = false;
    }
    if (style.anti_aliasing_size != defaults.anti_aliasing_size) {
        out["anti_aliasing_size"] = decimal(style.anti_aliasing_size);
    }
    return out;
}

} // namespace slash::ui
