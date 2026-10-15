#include "slash/asset/assets.hpp"
#include "slash/canvas/canvas.hpp"
#include "slash/core/json.hpp"
#include "slash/render/device.hpp"
#include "slash/render/image.hpp"
#include "slash/text/text.hpp"
#include "slash/ui/ui.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Containers, themes, style boxes and the controls that draw them. The positions expected here
// are worked out by hand from Godot's code for each container (scene/gui/*_container.cpp).

namespace asset = slash::asset;
namespace canvas = slash::canvas;
namespace json = slash::json;
namespace render = slash::render;
namespace text = slash::text;
namespace ui = slash::ui;
namespace world = slash::world;
using slash::Vec2;
using slash::Vec4;

namespace {

struct MemoryFiles {
    std::map<std::string, std::string, std::less<>> files;

    asset::FileSource source() {
        return {
            .read = [this](std::string_view path) -> slash::Result<std::string> {
                const auto it = files.find(path);
                if (it == files.end()) {
                    return slash::make_error(slash::ErrorCode::NotFound, "no such file");
                }
                return it->second;
            },
            .stamp = [this](std::string_view path) -> std::optional<asset::FileStamp> {
                const auto it = files.find(path);
                if (it == files.end()) {
                    return std::nullopt;
                }
                return asset::FileStamp{.modified = 1, .size = it->second.size()};
            },
        };
    }
};

json::Value parse(std::string_view source) {
    auto parsed = json::parse(source);
    REQUIRE(parsed.ok());
    return std::move(parsed).value();
}

// A PNG of one color.
std::string png(std::uint32_t width, std::uint32_t height) {
    render::Image image{.width = width, .height = height, .pixels = {}};
    for (std::uint32_t i = 0; i < width * height; ++i) {
        image.pixels.insert(image.pixels.end(), {255, 255, 255, 255});
    }
    auto encoded = render::encode_png(image);
    REQUIRE(encoded.ok());
    return encoded.value();
}

// A world with the built-in components and ui installed, in a 400 x 300 viewport.
struct Scene {
    MemoryFiles files;
    asset::Assets assets{files.source()};
    world::ComponentRegistry components;
    world::World world{components};

    Scene() {
        world::register_builtin_components(components);
        ui::install(world, assets, {.viewport = {400.0F, 300.0F}, .theme = ""});
    }

    entt::entity create(std::string_view id, std::string_view components_json) {
        const entt::entity entity = world.create(id).value();
        set(entity, components_json);
        return entity;
    }

    void set(entt::entity entity, std::string_view components_json) {
        const json::Value values = parse(components_json);
        for (const auto& [name, fields] : values.as_object()) {
            std::string path;
            auto assigned = components.find(name)->assign(world, entity, fields, path);
            REQUIRE_MESSAGE(assigned.ok(), assigned.error().message);
        }
    }

    void set(std::string_view id, std::string_view components_json) {
        set(world.find(id), components_json);
    }

    // A ColorRect with a minimum size, for containers to place.
    void block(std::string_view id, int width, int height, std::string_view control_fields = "") {
        std::string control = "{\"custom_minimum_size\": [" + std::to_string(width) + ", " +
                              std::to_string(height) + "]";
        if (!control_fields.empty()) {
            control += ", " + std::string(control_fields);
        }
        control += "}";
        create(id, "{\"Control\": " + control + ", \"ColorRect\": {}}");
    }

    text::Fonts& fonts() { return *ui::fonts(world); }
    ui::Layout layout() { return ui::layout(world, fonts(), ui::settings(world).viewport); }
};

// A control's rect on screen, [x, y, w, h], for controls that aren't turned or scaled.
std::array<float, 4> rect_of(const Scene& scene, const ui::Layout& layout, std::string_view id) {
    const ui::Placed* placed = layout.find(scene.world.find(id));
    REQUIRE(placed != nullptr);
    return {placed->transform.tx, placed->transform.ty, placed->size.x, placed->size.y};
}

using Rect = std::array<float, 4>;

std::vector<std::string> problem_lines(const std::vector<ui::Problem>& problems) {
    std::vector<std::string> lines;
    for (const ui::Problem& problem : problems) {
        lines.push_back(problem.id + problem.path + ": " + problem.message);
    }
    return lines;
}

std::vector<std::string> error_lines(const std::vector<json::ValidationError>& errors) {
    std::vector<std::string> lines;
    for (const json::ValidationError& error : errors) {
        lines.push_back(error.path + ": " + error.message);
    }
    return lines;
}

} // namespace

TEST_CASE("a box container lines its children up and gives expanding ones the space left") {
    Scene scene;
    scene.block("a", 50, 20);
    scene.block("b", 60, 30, R"("size_flags_horizontal": "expand_fill")");
    scene.block("c", 40, 10);
    scene.create("box", R"({"Control": {"offset_right": 300, "offset_bottom": 50,
                                        "children": ["a", "b", "c"]},
                            "BoxContainer": {}})");
    // 300 less two separations of 4 and the minimums leave 142 for b, the one that expands.
    ui::Layout layout = scene.layout();
    CHECK(rect_of(scene, layout, "a") == Rect{0, 0, 50, 50});
    CHECK(rect_of(scene, layout, "b") == Rect{54, 0, 202, 50});
    CHECK(rect_of(scene, layout, "c") == Rect{260, 0, 40, 50});

    // With nothing expanding, alignment places the row: center leaves 71 on the left.
    scene.set("b", R"({"Control": {"size_flags_horizontal": "fill"}})");
    scene.set("box", R"({"BoxContainer": {"alignment": "center"}})");
    layout = scene.layout();
    CHECK(rect_of(scene, layout, "a") == Rect{71, 0, 50, 50});
    CHECK(rect_of(scene, layout, "b") == Rect{125, 0, 60, 50});
    CHECK(rect_of(scene, layout, "c") == Rect{189, 0, 40, 50});
    scene.set("box", R"({"BoxContainer": {"alignment": "end"}})");
    CHECK(rect_of(scene, scene.layout(), "a") == Rect{142, 0, 50, 50});

    // Across the box, size flags place each child: shrink_end keeps its height at the bottom.
    scene.set("a", R"({"Control": {"size_flags_vertical": "shrink_end"}})");
    scene.set("c", R"({"Control": {"size_flags_vertical": "shrink_center"}})");
    layout = scene.layout();
    CHECK(rect_of(scene, layout, "a") == Rect{142, 30, 50, 20});
    CHECK(rect_of(scene, layout, "c") == Rect{260, 20, 40, 10});

    // Hidden children take no room.
    scene.set("b", R"({"Control": {"visible": false}})");
    layout = scene.layout();
    CHECK(rect_of(scene, layout, "a") == Rect{206, 30, 50, 20});
    CHECK(rect_of(scene, layout, "c") == Rect{260, 20, 40, 10});
}

TEST_CASE("a vertical box shares space by stretch ratio, carrying the leftover pixels") {
    Scene scene;
    scene.block("a", 50, 20, R"("size_flags_horizontal": "shrink_center")");
    scene.block("b", 30, 30, R"("size_flags_vertical": "expand_fill")");
    scene.block("c", 10, 10,
                R"("size_flags_vertical": "expand_fill", "size_flags_stretch_ratio": 2)");
    scene.create("box", R"({"Control": {"offset_right": 100, "offset_bottom": 200,
                                        "children": ["a", "b", "c"]},
                            "BoxContainer": {"vertical": true}})");
    // 172 pixels to share 1:2 are 57.33 and 114.67; the fractions add up to a pixel for c, and
    // the last expanding child reaches the end.
    const ui::Layout layout = scene.layout();
    CHECK(rect_of(scene, layout, "a") == Rect{25, 0, 50, 20});
    CHECK(rect_of(scene, layout, "b") == Rect{0, 24, 100, 57});
    CHECK(rect_of(scene, layout, "c") == Rect{0, 85, 100, 115});
    const ui::Placed* box = layout.find(scene.world.find("box"));
    REQUIRE(box != nullptr);
    CHECK(box->minimum_size == Vec2{50.0F, 68.0F});
}

TEST_CASE("a grid sizes columns and rows by their widest and tallest children") {
    Scene scene;
    scene.block("a", 30, 10, R"("size_flags_horizontal": "expand_fill")");
    scene.block("b", 50, 20);
    scene.block("c", 40, 30, R"("size_flags_horizontal": "expand_fill")");
    scene.block("d", 20, 5);
    scene.create("grid", R"({"Control": {"offset_right": 201, "offset_bottom": 100,
                                         "children": ["a", "b", "c", "d"]},
                             "GridContainer": {"columns": 3}})");
    // Columns 0 and 2 expand into 143 pixels: 71 each, and the odd pixel goes to the first.
    ui::Layout layout = scene.layout();
    CHECK(rect_of(scene, layout, "a") == Rect{0, 0, 72, 30});
    CHECK(rect_of(scene, layout, "b") == Rect{76, 0, 50, 30});
    CHECK(rect_of(scene, layout, "c") == Rect{130, 0, 71, 30});
    CHECK(rect_of(scene, layout, "d") == Rect{0, 34, 72, 5});

    // Too small, it grows to its minimum, and a column that can't fit its share stops
    // expanding: column 2 (40 wide) leaves column 0 the remaining 30.
    scene.set("grid", R"({"Control": {"offset_right": 10, "offset_bottom": 10,
                                      "children": ["a", "b", "c", "d"]}})");
    layout = scene.layout();
    CHECK(rect_of(scene, layout, "grid") == Rect{0, 0, 128, 39});
    CHECK(rect_of(scene, layout, "a") == Rect{0, 0, 30, 30});
    CHECK(rect_of(scene, layout, "b") == Rect{34, 0, 50, 30});
    CHECK(rect_of(scene, layout, "c") == Rect{88, 0, 40, 30});
}

TEST_CASE("margin, center and panel containers place their children inside them") {
    Scene scene;
    scene.block("in_margin", 10, 10);
    scene.create("margin", R"({"Control": {"offset_right": 200, "offset_bottom": 100,
                                           "children": ["in_margin"],
                                           "theme_override_constants": {
                                               "margin_left": 10, "margin_top": 5,
                                               "margin_right": 20, "margin_bottom": 15}},
                               "MarginContainer": {}})");
    scene.block("in_center", 50, 20);
    scene.create("center", R"({"Control": {"offset_right": 201, "offset_bottom": 101,
                                           "children": ["in_center"]},
                               "CenterContainer": {}})");
    scene.block("in_panel", 10, 10);
    scene.create("panel", R"({"Control": {"offset_right": 200, "offset_bottom": 100,
                                          "children": ["in_panel"],
                                          "theme_override_styles": {
                                              "panel": {"content_margin": 8}}},
                              "PanelContainer": {}})");
    ui::Layout layout = scene.layout();
    CHECK(rect_of(scene, layout, "in_margin") == Rect{10, 5, 170, 80});
    CHECK(rect_of(scene, layout, "in_center") == Rect{75, 40, 50, 20});
    CHECK(rect_of(scene, layout, "in_panel") == Rect{8, 8, 184, 84});
    CHECK(layout.find(scene.world.find("panel"))->minimum_size == Vec2{26.0F, 26.0F});
    // The panel container draws its style box under its child.
    REQUIRE(layout.boxes.contains(scene.world.find("panel")));
    CHECK(layout.boxes.at(scene.world.find("panel")).front().size == Vec2{200.0F, 100.0F});

    scene.set("center", R"({"CenterContainer": {"use_top_left": true}})");
    CHECK(rect_of(scene, scene.layout(), "in_center") == Rect{-25, -10, 50, 20});
}

TEST_CASE("a flow container wraps its children into lines") {
    Scene scene;
    scene.block("a", 80, 20);
    scene.block("b", 80, 30);
    scene.block("c", 80, 10, R"("size_flags_horizontal": "expand_fill")");
    scene.block("d", 50, 10);
    scene.create("flow", R"({"Control": {"offset_right": 200, "offset_bottom": 100,
                                         "children": ["a", "b", "c", "d"]},
                             "FlowContainer": {}})");
    // c doesn't fit after a and b, so it starts the second line and takes the 66 pixels left.
    ui::Layout layout = scene.layout();
    CHECK(rect_of(scene, layout, "a") == Rect{0, 0, 80, 30});
    CHECK(rect_of(scene, layout, "b") == Rect{84, 0, 80, 30});
    CHECK(rect_of(scene, layout, "c") == Rect{0, 34, 146, 10});
    CHECK(rect_of(scene, layout, "d") == Rect{150, 34, 50, 10});
    CHECK(layout.find(scene.world.find("flow"))->minimum_size == Vec2{80.0F, 44.0F});

    scene.set("c", R"({"Control": {"size_flags_horizontal": "fill"}})");
    scene.set("flow", R"({"FlowContainer": {"alignment": "center"}})");
    layout = scene.layout();
    CHECK(rect_of(scene, layout, "a") == Rect{18, 0, 80, 30});
    CHECK(rect_of(scene, layout, "c") == Rect{33, 34, 80, 10});
    CHECK(rect_of(scene, layout, "d") == Rect{117, 34, 50, 10});

    // The last line aligned to the end of the line before it.
    scene.set("flow", R"({"FlowContainer": {"alignment": "begin", "last_wrap_alignment": "end"}})");
    layout = scene.layout();
    CHECK(rect_of(scene, layout, "a") == Rect{0, 0, 80, 30});
    CHECK(rect_of(scene, layout, "c") == Rect{30, 34, 80, 10});
}

TEST_CASE("a wrapping label in a container gets the height of its lines at the width it gets") {
    Scene scene;
    const std::string words = "one two three four five six seven eight nine ten";
    scene.create("label", R"({"Label": {"text": ")" + words + R"(", "autowrap_mode": "word"}})");
    scene.block("after", 10, 10);
    scene.create("box", R"({"Control": {"offset_right": 120, "offset_bottom": 10,
                                        "children": ["label", "after"]},
                            "BoxContainer": {"vertical": true}})");
    const ui::Layout layout = scene.layout();
    const ui::LabelText* label = layout.label(scene.world.find("label"));
    REQUIRE(label != nullptr);
    const Rect placed = rect_of(scene, layout, "label");
    CHECK(placed[2] == 120.0F);
    CHECK(label->last - label->first > 1);
    CHECK(label->overflow == false);
    // The box grew to fit, and the next child comes after the label's lines.
    CHECK(rect_of(scene, layout, "after")[1] == placed[3] + 4.0F);
    CHECK(rect_of(scene, layout, "box")[3] == placed[3] + 14.0F);
}

TEST_CASE("themes give controls their items, nearest first, then the project's theme") {
    Scene scene;
    scene.files.files["ui/game.theme.json"] = R"({
        "format": 1,
        "types": {
            "Label": {"colors": {"font_color": [1, 0, 0, 1]}, "font_sizes": {"font_size": 20}},
            "Title": {"base_type": "Label", "font_sizes": {"font_size": 32}},
            "BoxContainer": {"constants": {"separation": 10}},
            "Panel": {"styles": {"panel": {"bg_color": [0, 0, 1, 1], "corner_radius": 6}}}
        }
    })";
    scene.files.files["ui/project.theme.json"] = R"({
        "format": 1,
        "default_font_size": 12,
        "types": {"Label": {"colors": {"font_color": [0, 0, 1, 1]}}}
    })";
    ui::set_settings(scene.world, {.viewport = {400.0F, 300.0F}, .theme = "ui/project.theme.json"});
    scene.create("title", R"({"Control": {"theme_type_variation": "Title"},
                              "Label": {"text": "Hi"}})");
    scene.create("body", R"({"Control": {"theme_override_colors": {"font_color": [0, 1, 0, 1]}},
                             "Label": {"text": "Hi"}})");
    scene.create("box", R"({"Control": {"children": ["title", "body"]},
                            "BoxContainer": {"vertical": true}})");
    scene.create("root", R"({"Control": {"offset_right": 400, "offset_bottom": 300,
                                         "theme": "ui/game.theme.json", "children": ["box"]},
                             "Panel": {}})");
    scene.create("outside", R"({"Label": {"text": "Hi"}})");

    const ui::Layout layout = scene.layout();
    CHECK(layout.problems.empty());
    const ui::LabelText& title = *layout.label(scene.world.find("title"));
    const ui::LabelText& body = *layout.label(scene.world.find("body"));
    const ui::LabelText& outside = *layout.label(scene.world.find("outside"));
    CHECK(title.font_size == 32);
    CHECK(title.font_color == Vec4{1.0F, 0.0F, 0.0F, 1.0F});
    CHECK(body.font_size == 20);
    CHECK(body.font_color == Vec4{0.0F, 1.0F, 0.0F, 1.0F});
    // Outside the root, only the project's theme applies: its color, and its default size.
    CHECK(outside.font_size == 12);
    CHECK(outside.font_color == Vec4{0.0F, 0.0F, 1.0F, 1.0F});
    CHECK(rect_of(scene, layout, "body")[1] == rect_of(scene, layout, "title")[3] + 10.0F);
    const ui::StyleBox& panel = layout.boxes.at(scene.world.find("root")).front().style;
    CHECK(panel.bg_color == Vec4{0.0F, 0.0F, 1.0F, 1.0F});
    CHECK(panel.corner_radius == std::array<int, 4>{6, 6, 6, 6});

    // Where each item comes from.
    std::map<std::string, std::pair<std::string, std::string>> items;
    for (const ui::ThemeItem& item :
         ui::theme_items(scene.world, scene.fonts(), scene.world.find("title"))) {
        items[item.kind + "/" + item.name] = {json::write(item.value, json::Style::Compact),
                                              item.from};
    }
    CHECK(items["font_sizes/font_size"] ==
          std::pair<std::string, std::string>{"32", "ui/game.theme.json"});
    CHECK(items["colors/font_color"] ==
          std::pair<std::string, std::string>{"[1.0,0.0,0.0,1.0]", "ui/game.theme.json"});
    CHECK(items["constants/line_spacing"] == std::pair<std::string, std::string>{"3", "default"});
    CHECK(items.size() == 11);
    const auto body_items = ui::theme_items(scene.world, scene.fonts(), scene.world.find("body"));
    const auto color = std::ranges::find_if(
        body_items, [](const ui::ThemeItem& item) { return item.name == "font_color"; });
    REQUIRE(color != body_items.end());
    CHECK(color->from == "override");
}

TEST_CASE("layout reports themes, variations and textures it can't use") {
    Scene scene;
    scene.files.files["ui/game.theme.json"] = R"({"format": 1})";
    scene.create("a", R"({"Control": {"theme": "ui/missing.theme.json"}})");
    scene.create("b", R"({"Control": {"theme": "ui/game.theme.json",
                                      "theme_type_variation": "Title"},
                          "Label": {"text": "Hi"}})");
    scene.create("c", R"({"TextureRect": {"texture": "ui/missing.png"}})");
    CHECK(problem_lines(scene.layout().problems) ==
          std::vector<std::string>{
              "a/components/Control/theme: asset \"ui/missing.theme.json\" could not be read: no "
              "such file",
              "b/components/Control/theme_type_variation: \"Title\" is no type variation in the "
              "themes this control uses; give it a base_type in one",
              "c/components/TextureRect/texture: asset \"ui/missing.png\" could not be read: no "
              "such file",
          });
}

TEST_CASE("style boxes read Godot's property names, with shorthands for every side") {
    const auto style = ui::parse_style_box(parse(R"({"corner_radius": 4,
                                                     "corner_radius_top_left": 0,
                                                     "border_width": 2,
                                                     "content_margin_left": 10})"));
    REQUIRE(style.ok());
    CHECK(style.value().type == ui::StyleBox::Type::Flat);
    CHECK(style.value().corner_radius == std::array<int, 4>{0, 4, 4, 4});
    CHECK(style.value().border_width == std::array<int, 4>{2, 2, 2, 2});
    // A content margin, or the border where it has none.
    CHECK(style.value().margin(0) == 10.0F);
    CHECK(style.value().margin(1) == 2.0F);
    CHECK(style.value().minimum_size() == Vec2{12.0F, 4.0F});
    // Back to JSON with only what differs from the defaults, four equal sides as one.
    CHECK(json::write(ui::style_box_json(style.value()), json::Style::Compact) ==
          R"({"border_width":2,"content_margin_left":10.0,"corner_radius_bottom_left":4,)"
          R"("corner_radius_bottom_right":4,"corner_radius_top_right":4})");

    const auto texture = ui::parse_style_box(
        parse(R"({"type": "texture", "texture": "ui/box.png", "texture_margin": 6})"));
    REQUIRE(texture.ok());
    CHECK(texture.value().margin(2) == 6.0F);
    CHECK(texture.value().minimum_size() == Vec2{12.0F, 12.0F});
    CHECK(ui::parse_style_box(parse(R"({"type": "empty"})")).value().minimum_size() == Vec2{});

    CHECK(error_lines(ui::validate_style_box(parse(R"({"bg_colour": [1, 1, 1, 1]})"))) ==
          std::vector<std::string>{"/bg_colour: unknown property \"bg_colour\"; did you mean "
                                   "\"bg_color\"?"});
    CHECK(error_lines(ui::validate_style_box(parse(R"({"texture": "ui/box.png"})"))) ==
          std::vector<std::string>{
              "/texture: flat style boxes have no texture; it is for \"type\": \"texture\""});
    CHECK(error_lines(ui::validate_style_box(parse(R"({"type": "texture"})"))) ==
          std::vector<std::string>{": a texture style box needs a texture"});
}

TEST_CASE("theme files are checked for unknown types, items and broken variations") {
    CHECK(ui::validate_theme(parse(R"({"format": 1, "default_font_size": 14, "types": {
              "Title": {"base_type": "Label", "font_sizes": {"font_size": 32}},
              "Label": {"colors": {"font_color": [1, 1, 1, 1]}}}})"))
              .empty());
    CHECK(error_lines(ui::validate_theme(parse(R"({"format": 1, "types": {
              "Lable": {},
              "Big": {"base_type": "Labl"},
              "Label": {"colors": {"font_colour": [1, 1, 1, 1]}},
              "Loop": {"base_type": "Loop2"},
              "Loop2": {"base_type": "Loop"},
              "Panel": {"styles": {"panel": {"bg_color": [1, 1, 1, 1]}}}}})"))) ==
          std::vector<std::string>{
              "/types/Big/base_type: \"Labl\" isn't a control type or a type in this theme; did "
              "you mean \"Label\"?",
              "/types/Label/colors/font_colour: Label has no colors item \"font_colour\"; did "
              "you mean \"font_color\"?",
              "/types/Lable: \"Lable\" isn't a control type; did you mean \"Label\"?",
              "/types/Loop/base_type: the base types of \"Loop\" go round in a loop",
              "/types/Loop2/base_type: the base types of \"Loop2\" go round in a loop",
          });
    // The schema's errors come first, alone.
    CHECK(error_lines(ui::validate_theme(parse(R"({"format": 1, "types": {
              "Panel": {"styles": {"panel": {"bg_colour": [1, 1, 1, 1]}}}}})"))) ==
          std::vector<std::string>{"/types/Panel/styles/panel/bg_colour: unknown property "
                                   "\"bg_colour\"; did you mean \"bg_color\"?"});
}

TEST_CASE("draw_calls draws rounded style boxes with smooth edges, as Godot's StyleBoxFlat") {
    Scene scene;
    scene.create("panel", R"({"Control": {"offset_left": 10, "offset_top": 10,
                                          "offset_right": 110, "offset_bottom": 60,
                                          "theme_override_styles": {"panel": {
                                              "bg_color": [1, 0, 0, 1], "corner_radius": 4}}},
                              "Panel": {}})");
    ui::DrawList drawn = ui::draw_calls(scene.layout(), scene.world, scene.fonts());
    CHECK(drawn.boxes == 1);
    REQUIRE(drawn.calls.size() == 1);
    // The fill: 9 points round each corner, in 34 triangles; then the smoothing ring around it,
    // 72 triangles fading to clear.
    const std::vector<render::Vertex2D>& vertices = drawn.calls[0].vertices;
    REQUIRE(vertices.size() == (34 + 72) * 3);
    CHECK(vertices[0].x == doctest::Approx(10.5F));
    CHECK(vertices[0].y == doctest::Approx(14.0F));
    float left = 1e9F;
    float right = -1e9F;
    std::size_t clear = 0;
    for (const render::Vertex2D& vertex : vertices) {
        left = std::min(left, vertex.x);
        right = std::max(right, vertex.x);
        clear += vertex.rgba[3] == 0 ? 1U : 0U;
    }
    CHECK(left == doctest::Approx(9.5F));
    CHECK(right == doctest::Approx(110.5F));
    CHECK(clear > 0);
    CHECK(vertices[0].rgba == std::array<std::uint8_t, 4>{255, 0, 0, 255});

    // Square corners need no smoothing: a border ring and the fill inside it.
    scene.set("panel", R"({"Control": {"offset_left": 10, "offset_top": 10,
                                       "offset_right": 110, "offset_bottom": 60,
                                       "theme_override_styles": {"panel": {
                                           "bg_color": [0, 0, 0, 1], "border_width": 2}}}})");
    drawn = ui::draw_calls(scene.layout(), scene.world, scene.fonts());
    CHECK(drawn.calls[0].vertices.size() == (16 + 6) * 3);

    // A shadow is a ring fading out from under the box, plus the box's shape under it.
    scene.set("panel", R"({"Control": {"offset_left": 10, "offset_top": 10,
                                       "offset_right": 110, "offset_bottom": 60,
                                       "theme_override_styles": {"panel": {
                                           "shadow_size": 4, "shadow_offset": [2, 2]}}}})");
    drawn = ui::draw_calls(scene.layout(), scene.world, scene.fonts());
    CHECK(drawn.calls[0].vertices.size() == (16 + 6 + 6) * 3);
    float bottom_right = 0.0F;
    for (const render::Vertex2D& vertex : drawn.calls[0].vertices) {
        bottom_right = std::max(bottom_right, vertex.x);
    }
    CHECK(bottom_right == 116.0F);

    // An empty style box draws nothing.
    scene.set("panel", R"({"Control": {"theme_override_styles": {"panel": {"type": "empty"}}}})");
    drawn = ui::draw_calls(scene.layout(), scene.world, scene.fonts());
    CHECK(drawn.boxes == 0);
    CHECK(drawn.calls.empty());
}

TEST_CASE("nine-patch rects keep their corners and stretch or tile the rest") {
    Scene scene;
    scene.files.files["ui/patch.png"] = png(30, 30);
    scene.create("patch", R"({"Control": {"offset_right": 100, "offset_bottom": 50},
                              "NinePatchRect": {"texture": "ui/patch.png",
                                                "patch_margin_left": 10, "patch_margin_top": 10,
                                                "patch_margin_right": 10,
                                                "patch_margin_bottom": 10}})");
    ui::DrawList drawn = ui::draw_calls(scene.layout(), scene.world, scene.fonts());
    REQUIRE(drawn.calls.size() == 1);
    CHECK(drawn.calls[0].source == ui::DrawCall::Source::Texture);
    CHECK(drawn.calls[0].texture == "ui/patch.png");
    CHECK(drawn.calls[0].filter == "inherit");
    std::vector<render::Vertex2D> vertices = drawn.calls[0].vertices;
    REQUIRE(vertices.size() == 9 * 6);
    // The top left corner shows the texture's top left 10 pixels, unscaled...
    CHECK(vertices[0].x == 0.0F);
    CHECK(vertices[0].u == 0.0F);
    CHECK(vertices[4].x == 10.0F);
    CHECK(vertices[4].y == 10.0F);
    CHECK(vertices[4].u == doctest::Approx(1.0F / 3.0F));
    // ...and the middle stretches the texture's middle over the rest.
    CHECK(vertices[24].x == 10.0F);
    CHECK(vertices[28].x == 90.0F);
    CHECK(vertices[28].y == 40.0F);
    CHECK(vertices[28].u == doctest::Approx(2.0F / 3.0F));

    scene.set("patch", R"({"NinePatchRect": {"texture": "ui/patch.png", "patch_margin_left": 10,
                                             "patch_margin_top": 10, "patch_margin_right": 10,
                                             "patch_margin_bottom": 10, "draw_center": false}})");
    CHECK(ui::draw_calls(scene.layout(), scene.world, scene.fonts()).calls[0].vertices.size() ==
          8 * 6);

    // Tiled, a 75-pixel middle takes seven 10-pixel tiles and half of an eighth.
    scene.set("patch", R"({"Control": {"offset_right": 95, "offset_bottom": 50},
                           "NinePatchRect": {"texture": "ui/patch.png", "patch_margin_left": 10,
                                             "patch_margin_top": 10, "patch_margin_right": 10,
                                             "patch_margin_bottom": 10, "draw_center": true,
                                             "axis_stretch_horizontal": "tile"}})");
    vertices = ui::draw_calls(scene.layout(), scene.world, scene.fonts()).calls[0].vertices;
    REQUIRE(vertices.size() == 3 * 10 * 6);
    // The eighth tile of the top row: from 80 to 85, showing half the middle.
    CHECK(vertices[(8 * 6) + 0].x == 80.0F);
    CHECK(vertices[(8 * 6) + 4].x == 85.0F);
    CHECK(vertices[(8 * 6) + 4].u == doctest::Approx(0.5F));
    // Tile fit stretches eight tiles to fill it exactly.
    scene.set("patch", R"({"NinePatchRect": {"texture": "ui/patch.png", "patch_margin_left": 10,
                                             "patch_margin_top": 10, "patch_margin_right": 10,
                                             "patch_margin_bottom": 10,
                                             "axis_stretch_horizontal": "tile_fit"}})");
    vertices = ui::draw_calls(scene.layout(), scene.world, scene.fonts()).calls[0].vertices;
    REQUIRE(vertices.size() == 3 * 10 * 6);
    CHECK(vertices[(8 * 6) + 0].x == doctest::Approx(75.625F));
    CHECK(vertices[(8 * 6) + 4].u == doctest::Approx(2.0F / 3.0F));
}

TEST_CASE("texture rects size, stretch and flip their texture as their modes say") {
    Scene scene;
    scene.files.files["ui/icon.png"] = png(20, 10);
    scene.create("icon", R"({"TextureRect": {"texture": "ui/icon.png"}})");
    ui::Layout layout = scene.layout();
    // keep_size: at least as big as its texture.
    CHECK(rect_of(scene, layout, "icon") == Rect{0, 0, 20, 10});

    scene.set("icon", R"({"Control": {"offset_right": 100, "offset_bottom": 100},
                          "TextureRect": {"texture": "ui/icon.png", "expand_mode": "ignore_size",
                                          "stretch_mode": "keep_aspect_centered"}})");
    layout = scene.layout();
    const ui::Box& box = layout.boxes.at(scene.world.find("icon")).front();
    CHECK(box.position == Vec2{0.0F, 25.0F});
    CHECK(box.size == Vec2{100.0F, 50.0F});

    scene.set("icon", R"({"Control": {"offset_right": 40, "offset_bottom": 20},
                          "TextureRect": {"texture": "ui/icon.png", "flip_h": true}})");
    const ui::DrawList drawn = ui::draw_calls(scene.layout(), scene.world, scene.fonts());
    REQUIRE(drawn.calls.size() == 1);
    REQUIRE(drawn.calls[0].vertices.size() == 6);
    CHECK(drawn.calls[0].vertices[0].x == 0.0F);
    CHECK(drawn.calls[0].vertices[0].u == 1.0F);
    CHECK(drawn.calls[0].vertices[4].u == 0.0F);

    // Its texture_filter, or a parent's, picks how the renderer samples it.
    scene.set("icon", R"({"Control": {"offset_right": 40, "offset_bottom": 20,
                                      "texture_filter": "nearest"}})");
    CHECK(ui::draw_calls(scene.layout(), scene.world, scene.fonts()).calls[0].filter == "nearest");
}

TEST_CASE("a progress bar fills as far as its value and shows the percentage") {
    Scene scene;
    scene.create("bar", R"({"Control": {"offset_right": 200},
                            "ProgressBar": {"value": 50}})");
    const float text_height = scene.fonts().metrics("", text::kDefaultFontSize).value().height();
    ui::Layout layout = scene.layout();
    const float height = 4.0F + text_height;
    CHECK(rect_of(scene, layout, "bar") == Rect{0, 0, 200, height});
    // The fill's margins (2 a side) aren't part of the bar's length: 50% of 196, plus them.
    std::vector<ui::Box> boxes = layout.boxes.at(scene.world.find("bar"));
    REQUIRE(boxes.size() == 2);
    CHECK(boxes[0].size == Vec2{200.0F, height});
    CHECK(boxes[1].position == Vec2{});
    CHECK(boxes[1].size == Vec2{102.0F, height});
    CHECK(boxes[1].style.bg_color == Vec4{1.0F, 1.0F, 1.0F, 0.4F});
    REQUIRE(layout.percentages.contains(scene.world.find("bar")));
    CHECK(layout.percentages.at(scene.world.find("bar")).source == "50%");

    scene.set("bar", R"({"ProgressBar": {"value": 25, "fill_mode": "end_to_begin"}})");
    layout = scene.layout();
    boxes = layout.boxes.at(scene.world.find("bar"));
    CHECK(boxes[1].position == Vec2{147.0F, 0.0F});
    CHECK(boxes[1].size == Vec2{53.0F, height});

    // Stepped to 0.01, kept in range unless allowed out of it.
    scene.set("bar", R"({"ProgressBar": {"value": 150}})");
    CHECK(scene.layout().percentages.at(scene.world.find("bar")).source == "100%");
    scene.set("bar", R"({"ProgressBar": {"value": 150, "allow_greater": true}})");
    CHECK(scene.layout().percentages.at(scene.world.find("bar")).source == "150%");
    scene.set("bar", R"({"ProgressBar": {"value": 0, "show_percentage": false}})");
    layout = scene.layout();
    CHECK(rect_of(scene, layout, "bar") == Rect{0, 0, 200, 4});
    CHECK(layout.boxes.at(scene.world.find("bar")).size() == 1);
    CHECK_FALSE(layout.percentages.contains(scene.world.find("bar")));

    const ui::DrawList drawn = ui::draw_calls(scene.layout(), scene.world, scene.fonts());
    CHECK(drawn.boxes == 1);
}

TEST_CASE("check finds two containers on one entity, and style overrides it can't use") {
    Scene scene;
    scene.create("both", R"({"BoxContainer": {}, "GridContainer": {}, "CenterContainer": {}})");
    scene.create("panel", R"({"Control": {"theme_override_styles": {
                                  "panl": {},
                                  "panel": {"type": "texture", "texture": "ui/missing.png"}}},
                              "Panel": {}})");
    scene.create("box", R"({"Control": {"theme_override_styles": {"panel": {}}},
                            "BoxContainer": {}, "Label": {"text": "x"}})");
    CHECK(problem_lines(ui::check(scene.world, scene.fonts())) ==
          std::vector<std::string>{
              "both/components/GridContainer: an entity has one container; its BoxContainer "
              "places its children",
              "both/components/CenterContainer: an entity has one container; its BoxContainer "
              "places its children",
              "box/components/Control/theme_override_styles/panel: Label and HBoxContainer have "
              "no styles item \"panel\"; it has none",
              "panel/components/Control/theme_override_styles/panl: Panel has no styles item "
              "\"panl\"; did you mean \"panel\"?",
              "panel/components/Control/theme_override_styles/panel/texture: asset "
              "\"ui/missing.png\" could not be read: no such file",
          });
}

TEST_CASE("the canvas renderer counts the style boxes it draws") {
    MemoryFiles files;
    files.files["ui/patch.png"] = png(30, 30);
    asset::Assets assets{files.source()};
    world::ComponentRegistry components;
    world::register_builtin_components(components);
    world::World world{components};
    canvas::install(world, assets);
    auto device = render::create_null_device();
    canvas::CanvasRenderer renderer(*device, assets);
    const entt::entity panel = world.create("panel").value();
    std::string path;
    REQUIRE(components.find("Panel")->assign(world, panel, parse("{}"), path).ok());
    const entt::entity patch = world.create("patch").value();
    REQUIRE(components.find("NinePatchRect")
                ->assign(world, patch, parse(R"({"texture": "ui/patch.png"})"), path)
                .ok());
    REQUIRE(renderer.draw(world, {}).ok());
    CHECK(renderer.controls_drawn() == 2);
    CHECK(renderer.boxes_drawn() == 2);
}

TEST_CASE("controls laid out right to left mirror their anchors, offsets and grow directions") {
    Scene scene;
    // 10 pixels from the left edge, left to right, is 10 from the right edge.
    scene.create("tag", R"({"Control": {"offset_left": 10, "offset_right": 110,
                                        "offset_bottom": 20, "layout_direction": "rtl"},
                            "ColorRect": {}})");
    // Growing to its minimum size toward its end grows it toward the left.
    scene.create("grows", R"({"Control": {"offset_left": 100, "offset_right": 100,
                                          "custom_minimum_size": [40, 10],
                                          "layout_direction": "rtl"}})");
    // Children inherit their parent's direction unless they set their own.
    scene.block("inner", 20, 10, R"("offset_right": 20, "offset_bottom": 10)");
    scene.block("own", 20, 10,
                R"("offset_right": 20, "offset_bottom": 10, "layout_direction": "ltr")");
    scene.create("panel", R"({"Control": {"offset_left": 100, "offset_right": 300,
                                          "offset_bottom": 50, "layout_direction": "rtl",
                                          "children": ["inner", "own"]}})");
    const ui::Layout layout = scene.layout();
    CHECK(layout.problems.empty());
    CHECK(rect_of(scene, layout, "tag") == Rect{290, 0, 100, 20});
    CHECK(rect_of(scene, layout, "grows") == Rect{260, 0, 40, 10});
    CHECK(rect_of(scene, layout, "panel") == Rect{100, 0, 200, 50});
    CHECK(rect_of(scene, layout, "inner") == Rect{280, 0, 20, 10});
    CHECK(rect_of(scene, layout, "own") == Rect{100, 0, 20, 10});
}

TEST_CASE("containers laid out right to left place their children from the right") {
    Scene scene;
    scene.block("a", 50, 20);
    scene.block("b", 60, 30, R"("size_flags_horizontal": "expand_fill")");
    scene.block("c", 40, 10);
    scene.create("box", R"({"Control": {"offset_right": 300, "offset_bottom": 50,
                                        "layout_direction": "rtl", "children": ["a", "b", "c"]},
                            "BoxContainer": {}})");
    // The box sits at the right of the 400-wide viewport, its first child at its right.
    ui::Layout layout = scene.layout();
    CHECK(rect_of(scene, layout, "box") == Rect{100, 0, 300, 50});
    CHECK(rect_of(scene, layout, "a") == Rect{350, 0, 50, 50});
    CHECK(rect_of(scene, layout, "b") == Rect{144, 0, 202, 50});
    CHECK(rect_of(scene, layout, "c") == Rect{100, 0, 40, 50});
    // With nothing expanding, begin alignment puts the row at the right, end at the left.
    scene.set("b", R"({"Control": {"size_flags_horizontal": "fill"}})");
    layout = scene.layout();
    CHECK(rect_of(scene, layout, "a") == Rect{350, 0, 50, 50});
    CHECK(rect_of(scene, layout, "c") == Rect{242, 0, 40, 50});
    scene.set("box", R"({"BoxContainer": {"alignment": "end"}})");
    layout = scene.layout();
    CHECK(rect_of(scene, layout, "a") == Rect{208, 0, 50, 50});
    CHECK(rect_of(scene, layout, "c") == Rect{100, 0, 40, 50});
    // A vertical box keeps its order down the box; shrink_begin across it is at the right.
    scene.set("box", R"({"BoxContainer": {"vertical": true}})");
    scene.set("a", R"({"Control": {"size_flags_horizontal": "shrink_begin"}})");
    layout = scene.layout();
    CHECK(rect_of(scene, layout, "a") == Rect{350, 0, 50, 20});
    CHECK(rect_of(scene, layout, "b") == Rect{100, 24, 300, 30});

    // A grid's columns go from the right.
    scene.block("g1", 30, 10);
    scene.block("g2", 20, 10);
    scene.block("g3", 30, 10);
    scene.create("grid", R"({"Control": {"offset_right": 100, "layout_direction": "rtl",
                                         "children": ["g1", "g2", "g3"]},
                             "GridContainer": {"columns": 2}})");
    layout = scene.layout();
    CHECK(rect_of(scene, layout, "g1") == Rect{370, 0, 30, 10});
    CHECK(rect_of(scene, layout, "g2") == Rect{346, 0, 20, 10});
    CHECK(rect_of(scene, layout, "g3") == Rect{370, 14, 30, 10});

    // A flow's lines start at the right.
    scene.block("f1", 30, 10);
    scene.block("f2", 30, 10);
    scene.create("flow", R"({"Control": {"offset_right": 100, "layout_direction": "rtl",
                                         "children": ["f1", "f2"]},
                             "FlowContainer": {}})");
    layout = scene.layout();
    CHECK(rect_of(scene, layout, "f1") == Rect{370, 0, 30, 10});
    CHECK(rect_of(scene, layout, "f2") == Rect{336, 0, 30, 10});
}

TEST_CASE("labels, buttons and bars laid out right to left start at the right") {
    Scene scene;
    scene.files.files["ui/icon.png"] = png(16, 16);
    // Left alignment is the start of the line: the right.
    scene.create("label", R"({"Control": {"offset_right": 100, "layout_direction": "rtl"},
                              "Label": {"text": "Hello"}})");
    scene.create("right", R"({"Control": {"offset_right": 100, "layout_direction": "rtl"},
                              "Label": {"text": "Hello", "horizontal_alignment": "right"}})");
    // text_direction inherited takes the layout's direction.
    scene.create("inherits", R"({"Control": {"layout_direction": "rtl"},
                                 "Label": {"text": "Hello", "text_direction": "inherited"}})");
    // A button's icon on the left goes to the right.
    scene.create("button", R"({"Control": {"offset_right": 120, "layout_direction": "rtl"},
                               "Button": {"text": "Go", "icon": "ui/icon.png"}})");
    scene.create("bar", R"({"Control": {"offset_right": 200, "layout_direction": "rtl"},
                            "ProgressBar": {"value": 25}})");
    ui::Layout layout = scene.layout();
    CHECK(layout.problems.empty());
    const float hello = scene.fonts().layout("Hello", {}).value().width;
    CHECK(layout.label(scene.world.find("label"))->pens.front().x == 100.0F - hello);
    CHECK(layout.label(scene.world.find("right"))->pens.front().x == 0.0F);
    CHECK_FALSE(layout.label(scene.world.find("label"))->text.lines[0].rtl);
    CHECK(layout.label(scene.world.find("inherits"))->text.lines[0].rtl);

    const std::vector<ui::Box>& button = layout.boxes.at(scene.world.find("button"));
    const auto icon = std::ranges::find(button, std::string("ui/icon.png"),
                                        [](const ui::Box& box) { return box.style.texture; });
    REQUIRE(icon != button.end());
    CHECK(icon->position.x > 60.0F);
    CHECK(layout.label(scene.world.find("button"))->pens.front().x < icon->position.x);

    // begin_to_end fills from the right.
    const std::vector<ui::Box>& bar = layout.boxes.at(scene.world.find("bar"));
    CHECK(bar[1].position.x == 147.0F);

    // A theme's _mirrored style boxes are used for buttons laid out right to left.
    scene.set("button", R"({"Control": {"offset_right": 120, "layout_direction": "rtl",
        "theme_override_styles": {"normal_mirrored": {"bg_color": [1, 0, 0, 1]}}}})");
    layout = scene.layout();
    CHECK(layout.boxes.at(scene.world.find("button")).front().style.bg_color ==
          Vec4{1.0F, 0.0F, 0.0F, 1.0F});
    CHECK(ui::check(scene.world, scene.fonts()).empty());
    scene.set("button", R"({"Control": {"layout_direction": "ltr"}})");
    CHECK(scene.layout().boxes.at(scene.world.find("button")).front().style.bg_color !=
          Vec4{1.0F, 0.0F, 0.0F, 1.0F});
}

TEST_CASE("fill alignment stretches the spaces between words of right-to-left lines") {
    Scene scene;
    scene.files.files["hebrew.ttf"] = [] {
        const auto bytes = text::box_font(U"אבג ");
        return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }();
    scene.create("filled", R"({"Control": {"offset_right": 200,
                                           "theme_override_fonts": {"font": "hebrew.ttf"}},
                               "Label": {"text": "א ב ג ", "horizontal_alignment": "fill"}})");
    const ui::Layout layout = scene.layout();
    const ui::LabelText& label = *layout.label(scene.world.find("filled"));
    REQUIRE(label.text.lines[0].rtl);
    // Three boxes of 10 and two spaces of 4 between them; the trailing space hangs at the left.
    CHECK(label.text.lines[0].width == 38.0F);
    CHECK(label.stretch.front() == doctest::Approx((200.0F - 38.0F) / 2.0F));
    CHECK(label.pens.front().x == 0.0F);
}

TEST_CASE("labels and buttons with a language that isn't a BCP 47 tag are reported") {
    Scene scene;
    scene.create("label", R"({"Label": {"text": "x", "language": "english"}})");
    scene.create("button", R"({"Button": {"text": "x", "language": "zh-Hant"}})");
    CHECK(problem_lines(ui::check(scene.world, scene.fonts())) ==
          std::vector<std::string>{"label/components/Label/language: \"english\" isn't a BCP 47 "
                                   "language tag, such as \"ar\" or \"zh-Hant\""});
}
