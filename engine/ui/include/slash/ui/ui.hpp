#pragma once

#include "slash/asset/assets.hpp"
#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/math.hpp"
#include "slash/core/result.hpp"
#include "slash/render/device.hpp"
#include "slash/text/text.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// UI, as Godot's Control nodes: Control places a rect in its parent's by anchors and offsets,
// and Label and ColorRect draw in it. layout() places every control of a world in a viewport,
// and draw_calls() turns that into triangles for the canvas renderer, which draws them above the
// world. Text comes from the world's text::Fonts.
//
//   ui::install(world, assets, {.viewport = {1280.0F, 720.0F}});
//   const ui::Layout placed = ui::layout(world, *ui::fonts(world), {1280.0F, 720.0F});
namespace slash::ui {

struct Settings {
    // The viewport controls are placed in when nothing gives its size, as Godot's
    // display/window/size/viewport_width and _height.
    Vec2 viewport{1280.0F, 720.0F};
};

// Adds the font asset types (text::add_asset_types) and gives the world a text::Fonts that loads
// through assets, which must outlive the world.
void install(world::World& world, asset::Assets& assets, const Settings& settings = {});
void set_settings(world::World& world, const Settings& settings);
// The settings of a world ui was installed in, or the defaults.
[[nodiscard]] Settings settings(const world::World& world);
// The fonts of a world ui was installed in, or nullptr.
[[nodiscard]] text::Fonts* fonts(const world::World& world);

// Whether an entity is a control: it has Control, Label or ColorRect.
[[nodiscard]] bool is_control(const world::World& world, entt::entity entity);

// An affine transform: x' = a x + c y + tx, y' = b x + d y + ty.
struct Transform {
    float a = 1.0F;
    float b = 0.0F;
    float c = 0.0F;
    float d = 1.0F;
    float tx = 0.0F;
    float ty = 0.0F;

    [[nodiscard]] Vec2 apply(Vec2 p) const {
        return {(a * p.x) + (c * p.y) + tx, (b * p.x) + (d * p.y) + ty};
    }
    [[nodiscard]] bool translation_only() const {
        return a == 1.0F && b == 0.0F && c == 0.0F && d == 1.0F;
    }
    [[nodiscard]] Transform operator*(const Transform& o) const;
};

// A Label's text as laid out in its rect.
struct LabelText {
    std::string source;    // the text laid out: in capitals or cut short, as the Label asks
    text::TextLayout text; // every line, those cut by text_overrun_behavior cut
    std::vector<std::size_t> trimmed; // the lines cut, which end with the ellipsis
    std::string ellipsis;  // what they end with: "" unless text_overrun_behavior adds one
    std::size_t first = 0; // the lines shown: [first, last)
    std::size_t last = 0;
    std::vector<Vec2> pens;     // where each shown line starts, on its baseline, in the rect
    std::vector<float> stretch; // extra pixels after each space of a shown line, for fill
    // Glyphs of the source's bytes from here on are hidden (visible_characters after shaping).
    std::size_t visible_bytes = static_cast<std::size_t>(-1);
    bool overflow = false; // a line is wider than the rect, or lines don't fit its height
    std::string error;     // why its font didn't load, so the default font drew it
    // Theme items.
    std::string font;
    int font_size = text::kDefaultFontSize;
    Vec4 font_color;
    Vec4 outline_color;
    Vec4 shadow_color;
    int outline_size = 0;
    int shadow_outline_size = 0;
    Vec2 shadow_offset;
};

// One control's place.
struct Placed {
    entt::entity entity = entt::null;
    entt::entity parent = entt::null;
    // In the parent's rect, before rotation and scale, as Godot's position and size.
    Vec2 position;
    Vec2 size;
    Vec2 minimum_size;
    // From its rect to the viewport.
    Transform transform;
    bool visible = true;                   // it and every parent are visible
    Vec4 modulate{1.0F, 1.0F, 1.0F, 1.0F}; // its and its parents' modulate
    int z = 0;                             // its z_index plus its parents'
};

// What one control the layout can't place properly is doing wrong.
struct Problem {
    std::string id;
    std::string path; // JSON Pointer in the entity, e.g. "/components/Control/children/2"
    std::string message;
};

struct Layout {
    Vec2 viewport;
    // Every control, in drawing order: by z, then parents before children and children in
    // order, roots in the order their entities were made.
    std::vector<Placed> controls;
    std::unordered_map<entt::entity, std::size_t> index; // entity to its place in controls
    std::vector<Problem> problems;
    std::map<entt::entity, LabelText> labels; // each Label's text

    [[nodiscard]] const Placed* find(entt::entity entity) const;
    [[nodiscard]] const LabelText* label(entt::entity entity) const;
};

// Places every control of a world in a viewport of the given size.
[[nodiscard]] Layout layout(const world::World& world, text::Fonts& fonts, Vec2 viewport);

// The theme items each control type has, as Godot's default theme gives them: colors,
// constants, fonts and font sizes by name.
struct ThemeItems {
    std::map<std::string, Vec4, std::less<>> colors;
    std::map<std::string, int, std::less<>> constants;
    std::map<std::string, std::string, std::less<>> fonts; // "" is the default font
    std::map<std::string, int, std::less<>> font_sizes;
};
[[nodiscard]] const ThemeItems& default_theme(std::string_view type);

// Mistakes in a world's controls: a child listed by two controls or by itself, a child that
// isn't a control, a theme override with a name its type doesn't have (with the closest one), or
// a font that doesn't load.
[[nodiscard]] std::vector<Problem> check(const world::World& world, text::Fonts& fonts);

// The same mistakes in a parsed scene file, each with the JSON Pointer of the bad value in it,
// e.g. "/entities/menu/components/Control/children/0". Prefabs and fonts are read with reader.
// A scene that doesn't load has none: validate_scene reports why.
[[nodiscard]] std::vector<json::ValidationError>
check_scene(const world::ComponentRegistry& components, const json::Value& scene,
            const world::FileReader& reader);

// One draw: triangles in viewport pixels, with a texture from a file or a glyph atlas page.
struct DrawCall {
    enum class Source : std::uint8_t { None, Texture, Atlas };
    Source source = Source::None;
    std::string texture;    // a project path, for Texture
    std::uint32_t page = 0; // a page of the fonts' atlas, for Atlas
    render::DistanceField field;
    std::vector<render::Vertex2D> vertices;
};

// What drawing a layout takes, in order: draws in a row with the same texture are merged.
struct DrawList {
    std::vector<DrawCall> calls;
    std::size_t controls = 0; // controls drawn
    std::size_t glyphs = 0;   // glyph images drawn, outlines and shadows included
};
[[nodiscard]] DrawList draw_calls(const Layout& layout, const world::World& world,
                                  text::Fonts& fonts);

} // namespace slash::ui
