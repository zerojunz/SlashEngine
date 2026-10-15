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
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// UI, as Godot's Control nodes: Control places a rect in its parent's by anchors and offsets,
// containers place their children, and Label, Panel, TextureRect and the rest draw in it. Theme
// items (colors, constants, fonts, font sizes and style boxes) come from each control's
// overrides, the .theme.json themes of it and its parents, the project's theme and Godot's
// default theme. layout() places every control of a world in a viewport, and draw_calls() turns
// that into triangles for the canvas renderer, which draws them above the world. Text comes from
// the world's text::Fonts, and themes and textures load through the assets it loads from.
//
// The "gui" system sends each tick's input events to controls, as Godot's Viewport does: the
// mouse hovers them, clicks and touches press buttons, and the ui_* actions move focus and press
// the focused button. It emits what happened as UI events (events()) and world events.
//
//   ui::install(world, assets, {.viewport = {1280.0F, 720.0F}});
//   const ui::Layout placed = ui::layout(world, *ui::fonts(world), {1280.0F, 720.0F});
namespace slash::ui {

inline constexpr std::string_view kThemeSuffix = ".theme.json";

struct Settings {
    // The viewport controls are placed in when nothing gives its size, as Godot's
    // display/window/size/viewport_width and _height.
    Vec2 viewport{1280.0F, 720.0F};
    // A .theme.json file every control uses after its own and its parents' themes, as Godot's
    // gui/theme/custom; "" for none.
    std::string theme;
};

// Asset types "theme" (.theme.json files as Theme) and "texture" (.png and .bmp files as
// render::Image), plus the font types (text::add_asset_types).
void add_asset_types(asset::Assets& assets);

// Adds the asset types, gives the world a text::Fonts that loads through assets, which must
// outlive the world, and adds the "gui" system (priority -900, after input::install's "input").
void install(world::World& world, asset::Assets& assets, const Settings& settings = {});
void set_settings(world::World& world, const Settings& settings);
// Sets only the viewport: games set it to the window's size each frame, so controls are hit
// where they are drawn.
void set_viewport(world::World& world, Vec2 viewport);
// The settings of a world ui was installed in, or the defaults.
[[nodiscard]] Settings settings(const world::World& world);
// The fonts of a world ui was installed in, or nullptr.
[[nodiscard]] text::Fonts* fonts(const world::World& world);

// Whether an entity is a control: it has Control or one of the UI components that draw in or lay
// out one (Label, ColorRect, Panel, a container and so on).
[[nodiscard]] bool is_control(const world::World& world, entt::entity entity);

// A style box, as Godot's StyleBox: what a panel or a bar draws behind its content, and the
// margins its content keeps. Flat ones are rects with rounded corners, borders and a shadow
// (StyleBoxFlat), texture ones a texture cut into nine parts (StyleBoxTexture), and empty ones
// draw nothing (StyleBoxEmpty). Sides are left, top, right and bottom; corners top left, top
// right, bottom right and bottom left.
struct StyleBox {
    enum class Type : std::uint8_t { Flat, Texture, Empty };
    enum class Stretch : std::uint8_t { Stretch, Tile, TileFit };
    Type type = Type::Flat;
    // The content's margins; -1 takes the style's own: its borders, or its texture margins.
    std::array<float, 4> content_margin{-1.0F, -1.0F, -1.0F, -1.0F};
    std::array<float, 4> expand_margin{}; // drawn this far outside the rect
    bool draw_center = true;
    // Flat.
    Vec4 bg_color{0.6F, 0.6F, 0.6F, 1.0F};
    Vec4 border_color{0.8F, 0.8F, 0.8F, 1.0F};
    Vec4 shadow_color{0.0F, 0.0F, 0.0F, 0.6F};
    std::array<int, 4> border_width{};
    std::array<int, 4> corner_radius{};
    Vec2 shadow_offset;
    Vec2 skew;
    int corner_detail = 8;
    int shadow_size = 0;
    float anti_aliasing_size = 1.0F;
    bool border_blend = false;
    bool anti_aliasing = true;
    // Texture.
    std::string texture;
    Vec4 region_rect; // [x, y, width, height] in pixels; a zero size uses the whole texture
    std::array<float, 4> texture_margin{};
    Vec4 modulate_color{1.0F, 1.0F, 1.0F, 1.0F};
    Stretch axis_stretch_horizontal = Stretch::Stretch;
    Stretch axis_stretch_vertical = Stretch::Stretch;
    bool flip_h = false; // for TextureRect: drawn mirrored
    bool flip_v = false;

    // The content's margin on a side: content_margin, or the style's own.
    [[nodiscard]] float margin(std::size_t side) const;
    // The margins added up, [left + right, top + bottom].
    [[nodiscard]] Vec2 minimum_size() const;
    bool operator==(const StyleBox&) const = default;
};

// JSON Schema for a style box: an object with "type" ("flat", the default, "texture" or
// "empty") and Godot's property names. "content_margin", "expand_margin", "texture_margin",
// "border_width" and "corner_radius" set all four sides or corners; the named ones override them.
[[nodiscard]] json::Value style_box_schema();
// Every error in a style box, each with the JSON Pointer of the bad value.
[[nodiscard]] std::vector<json::ValidationError> validate_style_box(const json::Value& doc);
// A style box, or its first error.
[[nodiscard]] Result<StyleBox> parse_style_box(const json::Value& doc);
// A style box as JSON, with only the fields its type uses that differ from the defaults.
[[nodiscard]] json::Value style_box_json(const StyleBox& style);

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
    bool clip = false;     // drawn cut off at the rect
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
    // Its texture_filter, or the nearest parent's that isn't "inherit"; "inherit" if none is.
    std::string texture_filter = "inherit";
};

// A style box a control draws, and where in its rect.
struct Box {
    StyleBox style;
    Vec2 position;
    Vec2 size;
    Vec2 texture_size; // for texture style boxes: the texture's size in pixels
};

// What a button looks like, as Godot's BaseButton::DrawMode: hover_pressed is a toggle button
// that is on and hovered, or one whose shortcut just pressed it.
enum class DrawMode : std::uint8_t { Normal, Pressed, Hover, Disabled, HoverPressed };
// "normal", "pressed", "hover", "disabled" or "hover_pressed".
[[nodiscard]] std::string_view draw_mode_name(DrawMode mode);

// A Button's or TextureButton's state as laid out.
struct ButtonState {
    DrawMode mode = DrawMode::Normal;
    float click_margin = 0.0F; // pixels around its rect that still hit it
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
    // Every control in tree order, as Godot's scene tree: parents before children and children
    // in order, roots in the order their entities were made. Pointers hit the last first.
    std::vector<entt::entity> tree;
    std::vector<Problem> problems;
    std::map<entt::entity, LabelText> labels;       // each Label's text
    std::map<entt::entity, std::vector<Box>> boxes; // what each control draws under its text
    std::map<entt::entity, LabelText> percentages;  // each ProgressBar's percentage
    std::map<entt::entity, ButtonState> buttons;    // each Button and TextureButton

    [[nodiscard]] const Placed* find(entt::entity entity) const;
    [[nodiscard]] const LabelText* label(entt::entity entity) const;
};

// Places every control of a world in a viewport of the given size.
[[nodiscard]] Layout layout(const world::World& world, text::Fonts& fonts, Vec2 viewport);

// Theme items by name: colors, constants, fonts, font sizes and style boxes.
struct ThemeItems {
    std::map<std::string, Vec4, std::less<>> colors;
    std::map<std::string, int, std::less<>> constants;
    std::map<std::string, std::string, std::less<>> fonts; // "" is the default font
    std::map<std::string, int, std::less<>> font_sizes;
    std::map<std::string, StyleBox, std::less<>> styles;
};
// The theme items a control type has in Godot's default theme, its base types' included: a
// VBoxContainer has BoxContainer's, Container's and Control's.
[[nodiscard]] const ThemeItems& default_theme(std::string_view type);
// A control type and the types it is built on, as Godot's classes: "Label" gives Label and
// Control. Empty for a type that isn't one.
[[nodiscard]] std::vector<std::string_view> base_types(std::string_view type);
// Every control type, for suggestions.
[[nodiscard]] const std::vector<std::string_view>& control_types();

// A .theme.json file, as Godot's Theme: items by type, a type being a control type or a variation
// of one (base_type), plus a font and a font size for every type that has none.
struct Theme {
    struct Type {
        std::string base_type; // for a variation: the type it is a variation of
        ThemeItems items;
    };
    std::optional<std::string> default_font;
    int default_font_size = 0; // 0 for none
    std::map<std::string, Type, std::less<>> types;
};

// JSON Schema for .theme.json files.
[[nodiscard]] json::Value theme_schema();
// Every error in a .theme.json document, each with the JSON Pointer of the bad value: unknown
// types and item names with the closest ones, and variations that lead to no control type.
[[nodiscard]] std::vector<json::ValidationError> validate_theme(const json::Value& doc);
// The theme in a .theme.json document, or its first error.
[[nodiscard]] Result<Theme> parse_theme(const json::Value& doc);

// One theme item a control uses, and where its value comes from: "override", a theme file's
// path, or "default".
struct ThemeItem {
    std::string kind; // "colors", "constants", "fonts", "font_sizes" or "styles"
    std::string name;
    json::Value value;
    std::string from;
};
// Every theme item an entity's components use, by kind then name.
[[nodiscard]] std::vector<ThemeItem> theme_items(const world::World& world, text::Fonts& fonts,
                                                 entt::entity entity);

// Mistakes in a world's controls: a child listed by two controls or by itself, a child that
// isn't a control, two containers on one entity, a theme override with a name its type doesn't
// have (with the closest one), a style box, theme, font or texture that doesn't load.
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
    std::string texture;            // a project path, for Texture
    std::string filter = "inherit"; // for Texture: "linear", "nearest" or the renderer's default
    std::uint32_t page = 0;         // a page of the fonts' atlas, for Atlas
    render::DistanceField field;
    std::vector<render::Vertex2D> vertices;
};

// What drawing a layout takes, in order: draws in a row with the same texture are merged.
struct DrawList {
    std::vector<DrawCall> calls;
    std::size_t controls = 0; // controls drawn
    std::size_t glyphs = 0;   // glyph images drawn, outlines and shadows included
    std::size_t boxes = 0;    // style boxes and textures drawn
};
[[nodiscard]] DrawList draw_calls(const Layout& layout, const world::World& world,
                                  text::Fonts& fonts);

// ---- Buttons, focus and hover ----

// Something controls did, as Godot's signals: "pressed", "toggled", "button_down" and
// "button_up" from buttons, and "mouse_entered", "mouse_exited", "focus_entered" and
// "focus_exited" from any control. Each is a world event (World::emit) too, with data
// {"toggled_on": bool} for toggled.
struct UiEvent {
    std::string kind;
    std::string id;          // the control's entity ID
    bool toggled_on = false; // for toggled
};

// The UI events of the tick the "gui" system last ran (inside a system: this tick), in order.
// Controls are known by entity ID, so ones made without an ID take no part.
[[nodiscard]] std::span<const UiEvent> events(const world::World& world);
// Whether a button emitted pressed that tick.
[[nodiscard]] bool was_pressed(const world::World& world, entt::entity button);
// Presses a button at the next tick, as Godot's BaseButton::press: a toggle button toggles. A
// disabled one isn't pressed.
void press(world::World& world, entt::entity button);
[[nodiscard]] DrawMode draw_mode(const world::World& world, entt::entity button);

// The control with focus, which gets the ui_* actions first, or entt::null, as Godot's
// gui_get_focus_owner.
[[nodiscard]] entt::entity focus_owner(const world::World& world);
// Gives a control focus and shows it, as Godot's grab_focus; false if its focus_mode is none.
bool grab_focus(world::World& world, entt::entity control);
void release_focus(world::World& world);
// The control under the mouse, or entt::null.
[[nodiscard]] entt::entity hovered(const world::World& world);

// A copy of what the gui system keeps between ticks (focus, hover, buttons being pressed, UI
// events), so a session can put it back when a command fails or was a dry run.
struct Snapshot {
    std::shared_ptr<const void> data;
};
[[nodiscard]] Snapshot save(const world::World& world);
void restore(world::World& world, const Snapshot& snapshot);

} // namespace slash::ui
