#include "slash/asset/assets.hpp"
#include "slash/canvas/canvas.hpp"
#include "slash/core/json.hpp"
#include "slash/platform/process.hpp"
#include "slash/render/device.hpp"
#include "slash/render/image.hpp"
#include "slash/text/text.hpp"
#include "slash/ui/ui.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace asset = slash::asset;
namespace canvas = slash::canvas;
namespace json = slash::json;
namespace render = slash::render;
namespace text = slash::text;
namespace ui = slash::ui;
namespace world = slash::world;
using slash::Vec2;

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

// A world with the built-in components and ui installed, in a 400 x 300 viewport.
struct Scene {
    MemoryFiles files;
    asset::Assets assets{files.source()};
    world::ComponentRegistry components;
    world::World world{components};

    Scene() {
        world::register_builtin_components(components);
        ui::install(world, assets, {.viewport = {400.0F, 300.0F}});
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

    text::Fonts& fonts() { return *ui::fonts(world); }
    ui::Layout layout() { return ui::layout(world, fonts(), ui::settings(world).viewport); }

    float width_of(std::string_view source, int size = text::kDefaultFontSize) {
        return fonts().layout(source, {.font_size = size}).value().width;
    }
};

// The text of each line a label shows, with the ellipsis on lines it cut.
std::vector<std::string> shown_lines(const ui::LabelText& label) {
    std::vector<std::string> out;
    for (std::size_t l = label.first; l < label.last; ++l) {
        const text::Line& line = label.text.lines[l];
        std::string line_text = label.source.substr(line.start, line.end - line.start);
        while (line_text.ends_with(' ')) {
            line_text.pop_back();
        }
        if (std::ranges::find(label.trimmed, l) != label.trimmed.end()) {
            line_text += label.ellipsis;
        }
        out.push_back(line_text);
    }
    return out;
}

std::vector<std::string> ids(const world::World& world, const ui::Layout& layout) {
    std::vector<std::string> out;
    for (const ui::Placed& placed : layout.controls) {
        out.emplace_back(world.id_of(placed.entity));
    }
    return out;
}

} // namespace

TEST_CASE("controls are placed in their parent's rect by anchors and offsets") {
    Scene scene;
    const entt::entity box = scene.create(
        "box", R"({"Control": {"anchor_left": 0.5, "anchor_top": 0.5, "anchor_right": 0.5,
                               "anchor_bottom": 0.5, "offset_left": -50, "offset_top": -20,
                               "offset_right": 50, "offset_bottom": 20}})");
    const entt::entity panel = scene.create(
        "panel", R"({"Control": {"anchor_right": 1, "anchor_bottom": 1, "offset_left": 10,
                                 "offset_top": 10, "offset_right": -10, "offset_bottom": -10}})");
    scene.set(panel, R"({"Control": {"anchor_right": 1, "anchor_bottom": 1, "offset_left": 10,
                                     "offset_top": 10, "offset_right": -10,
                                     "offset_bottom": -10, "children": ["box"]}})");

    const ui::Layout layout = scene.layout();
    CHECK(layout.problems.empty());
    const ui::Placed* placed_panel = layout.find(panel);
    REQUIRE(placed_panel != nullptr);
    CHECK(placed_panel->position == Vec2{10.0F, 10.0F});
    CHECK(placed_panel->size == Vec2{380.0F, 280.0F});
    CHECK((placed_panel->parent == entt::null));

    const ui::Placed* placed_box = layout.find(box);
    REQUIRE(placed_box != nullptr);
    CHECK(placed_box->parent == panel);
    CHECK(placed_box->position == Vec2{140.0F, 120.0F});
    CHECK(placed_box->size == Vec2{100.0F, 40.0F});
    // In the viewport, moved by the panel too.
    CHECK(placed_box->transform.apply({0.0F, 0.0F}) == Vec2{150.0F, 130.0F});
    CHECK(placed_box->transform.translation_only());
    // Parents draw before their children.
    CHECK(ids(scene.world, layout) == std::vector<std::string>{"panel", "box"});

    SUBCASE("a viewport of another size moves them") {
        const ui::Layout wide = ui::layout(scene.world, scene.fonts(), {800.0F, 300.0F});
        CHECK(wide.find(panel)->size == Vec2{780.0F, 280.0F});
        CHECK(wide.find(box)->position == Vec2{340.0F, 120.0F});
    }
}

TEST_CASE("a control smaller than its minimum size grows as grow_horizontal and _vertical say") {
    Scene scene;
    const entt::entity end = scene.create(
        "end", R"({"Control": {"offset_left": 100, "offset_top": 100, "offset_right": 100,
                               "offset_bottom": 100, "custom_minimum_size": [40, 10]}})");
    const entt::entity both = scene.create(
        "both", R"({"Control": {"offset_left": 100, "offset_top": 100, "offset_right": 100,
                                "offset_bottom": 100, "custom_minimum_size": [40, 10],
                                "grow_horizontal": "both", "grow_vertical": "begin"}})");
    const ui::Layout layout = scene.layout();
    CHECK(layout.find(end)->position == Vec2{100.0F, 100.0F});
    CHECK(layout.find(end)->size == Vec2{40.0F, 10.0F});
    CHECK(layout.find(both)->position == Vec2{80.0F, 90.0F});
    CHECK(layout.find(both)->size == Vec2{40.0F, 10.0F});
    CHECK(layout.find(both)->minimum_size == Vec2{40.0F, 10.0F});
}

TEST_CASE("rotation and scale turn a control and its children around its pivot") {
    Scene scene;
    const entt::entity child = scene.create(
        "child", R"({"Control": {"offset_left": 10, "offset_right": 20, "offset_bottom": 5}})");
    const entt::entity parent = scene.create("parent", "{}");
    scene.set(parent, R"({"Control": {"offset_left": 100, "offset_top": 50, "offset_right": 140,
                                      "offset_bottom": 70, "pivot_offset": [20, 10],
                                      "rotation": 1.5707963705062866, "scale": [2, 2],
                                      "children": ["child"]}})");
    const ui::Layout layout = scene.layout();
    const ui::Transform& turned = layout.find(parent)->transform;
    CHECK_FALSE(turned.translation_only());
    // The pivot stays put: (100, 50) + (20, 10).
    const Vec2 pivot = turned.apply({20.0F, 10.0F});
    CHECK(pivot.x == doctest::Approx(120.0F));
    CHECK(pivot.y == doctest::Approx(60.0F));
    // A quarter turn clockwise (y is down), doubled: one pixel right of the pivot is two below.
    const Vec2 right = turned.apply({21.0F, 10.0F});
    CHECK(right.x == doctest::Approx(120.0F));
    CHECK(right.y == doctest::Approx(62.0F));
    // The child's corner turns with it.
    const Vec2 corner = layout.find(child)->transform.apply({0.0F, 0.0F});
    CHECK(corner.x == doctest::Approx(120.0F + 20.0F));
    CHECK(corner.y == doctest::Approx(60.0F - 20.0F));
}

TEST_CASE("modulate, visible and z_index pass from parents to children") {
    Scene scene;
    const entt::entity a = scene.create("a", R"({"ColorRect": {}})");
    const entt::entity b = scene.create("b", R"({"Control": {"z_index": -1}, "ColorRect": {}})");
    const entt::entity top = scene.create("top", "{}");
    scene.set(top, R"({"Control": {"modulate": [1, 0.5, 1, 0.5], "z_index": 2, "visible": false,
                                   "children": ["a", "b"]}, "ColorRect": {}})");
    const entt::entity loose = scene.create("loose", R"({"ColorRect": {}})");
    const ui::Layout layout = scene.layout();
    CHECK(layout.find(a)->modulate == slash::Vec4{1.0F, 0.5F, 1.0F, 0.5F});
    CHECK_FALSE(layout.find(a)->visible);
    CHECK(layout.find(a)->z == 2);
    CHECK(layout.find(b)->z == 1);
    CHECK(layout.find(loose)->visible);
    // By z, then parents before children and in order.
    CHECK(ids(scene.world, layout) == std::vector<std::string>{"loose", "b", "top", "a"});
    // Hidden controls draw nothing.
    const ui::DrawList drawn = ui::draw_calls(layout, scene.world, scene.fonts());
    CHECK(drawn.controls == 1);
}

TEST_CASE("children that can't be laid out are reported with their JSON Pointer") {
    Scene scene;
    const entt::entity a = scene.create("a", R"({"Control": {}})");
    const entt::entity b = scene.create("b", R"({"Control": {}})");
    scene.create("sprite", R"({"Transform2D": {}})");
    scene.set(a, R"({"Control": {"children": ["b", "a", "sprite", null]}})");
    scene.set(b, R"({"Control": {"children": ["a"]}})");
    const entt::entity c = scene.create("c", R"({"Control": {"children": ["b"]}})");

    const ui::Layout layout = scene.layout();
    std::vector<std::string> problems;
    for (const ui::Problem& problem : layout.problems) {
        problems.push_back(problem.id + problem.path + ": " + problem.message);
    }
    CHECK(problems == std::vector<std::string>{
                          "a/components/Control/children/1: a control can't be its own child",
                          "a/components/Control/children/2: \"sprite\" isn't a control; give it "
                          "a Control, Label or ColorRect",
                          "a/components/Control/children/3: names no entity",
                          "c/components/Control/children/0: \"b\" is already a child of \"a\"",
                          "b/components/Control/children/0: \"a\" contains \"b\", so it can't be "
                          "its child too",
                      });
    // Every control is still placed once.
    CHECK(layout.controls.size() == 3);
    CHECK((layout.find(a)->parent == entt::null));
    CHECK(layout.find(b)->parent == a);
    CHECK((layout.find(c)->parent == entt::null));
}

TEST_CASE("a label is as big as its text and aligns its lines in its rect") {
    Scene scene;
    const float hello = scene.width_of("Hello");
    REQUIRE(hello == 36.0F);
    const entt::entity plain = scene.create("plain", R"({"Label": {"text": "Hello"}})");
    const entt::entity centered =
        scene.create("centered", R"({"Control": {"offset_right": 100, "offset_bottom": 50},
                        "Label": {"text": "Hello", "horizontal_alignment": "center",
                                  "vertical_alignment": "center"}})");
    const entt::entity corner =
        scene.create("corner", R"({"Control": {"offset_right": 100, "offset_bottom": 50},
                      "Label": {"text": "Hello", "horizontal_alignment": "right",
                                "vertical_alignment": "bottom"}})");
    const entt::entity two = scene.create("two", R"({"Label": {"text": "Hello\nHello, world"}})");
    const entt::entity empty = scene.create("empty", R"({"Label": {}})");

    const ui::Layout layout = scene.layout();
    // Roboto at 16 pixels: 17 above the baseline, 5 below.
    CHECK(layout.find(plain)->size == Vec2{36.0F, 22.0F});
    const ui::LabelText* label = layout.label(plain);
    REQUIRE(label != nullptr);
    CHECK(label->pens == std::vector<Vec2>{{0.0F, 17.0F}});
    CHECK_FALSE(label->overflow);

    CHECK(layout.label(centered)->pens == std::vector<Vec2>{{32.0F, 14.0F + 17.0F}});
    CHECK(layout.label(corner)->pens == std::vector<Vec2>{{64.0F, 28.0F + 17.0F}});

    // Lines 3 pixels apart, Godot's line_spacing.
    const float wide = scene.width_of("Hello, world");
    CHECK(layout.find(two)->size == Vec2{wide, 22.0F + 3.0F + 22.0F});
    CHECK(layout.label(two)->pens == std::vector<Vec2>{{0.0F, 17.0F}, {0.0F, 17.0F + 25.0F}});
    CHECK(shown_lines(*layout.label(two)) == std::vector<std::string>{"Hello", "Hello, world"});

    // Empty text still takes a line.
    CHECK(layout.find(empty)->size == Vec2{1.0F, 22.0F});
}

TEST_CASE("a wrapping label grows down to fit its lines, and max_lines_visible trims them") {
    Scene scene;
    const entt::entity all = scene.create("all", R"({"Control": {"offset_right": 60},
                   "Label": {"text": "Hello world again", "autowrap_mode": "word"}})");
    const entt::entity two = scene.create("two", R"({"Control": {"offset_right": 60},
                   "Label": {"text": "Hello world again", "autowrap_mode": "word",
                             "max_lines_visible": 2,
                             "text_overrun_behavior": "trim_ellipsis"}})");
    const entt::entity skip = scene.create("skip", R"({"Control": {"offset_right": 60},
                    "Label": {"text": "Hello world again", "autowrap_mode": "word",
                              "lines_skipped": 1}})");
    const ui::Layout layout = scene.layout();
    CHECK(layout.find(all)->size == Vec2{60.0F, (22.0F * 3) + (3.0F * 2)});
    CHECK(shown_lines(*layout.label(all)) == std::vector<std::string>{"Hello", "world", "again"});

    CHECK(layout.find(two)->size == Vec2{60.0F, (22.0F * 2) + 3.0F});
    CHECK(shown_lines(*layout.label(two)) == std::vector<std::string>{"Hello", "world…"});
    CHECK_FALSE(layout.label(two)->overflow);

    CHECK(shown_lines(*layout.label(skip)) == std::vector<std::string>{"world", "again"});
    CHECK(layout.find(skip)->size.y == (22.0F * 2) + 3.0F);
}

TEST_CASE("labels trim lines too wide for their rect, and clip_text cuts off what's outside") {
    Scene scene;
    const entt::entity trimmed = scene.create("trimmed", R"({"Control": {"offset_right": 30},
                       "Label": {"text": "Hello", "text_overrun_behavior": "trim_char"}})");
    const entt::entity dots = scene.create("dots", R"({"Control": {"offset_right": 30},
                    "Label": {"text": "Hello", "text_overrun_behavior": "trim_ellipsis",
                              "ellipsis_char": ".."}})");
    const entt::entity wide =
        scene.create("wide", R"({"Control": {"offset_right": 20}, "Label": {"text": "Hello"}})");
    const entt::entity clipped = scene.create(
        "clipped", R"({"Control": {"offset_left": 200, "offset_top": 200, "offset_right": 220,
                                   "offset_bottom": 210},
                       "Label": {"text": "Hello", "clip_text": true}})");

    const ui::Layout layout = scene.layout();
    // Trimming lets the rect be narrower than the text.
    CHECK(layout.find(trimmed)->size.x == 30.0F);
    const ui::LabelText& cut = *layout.label(trimmed);
    CHECK(cut.trimmed == std::vector<std::size_t>{0});
    CHECK(cut.text.lines[0].width <= 30.0F);
    CHECK(shown_lines(cut).front().size() < 5);
    // Only the ellipsis's first character is used, as in Godot.
    CHECK(shown_lines(*layout.label(dots)).front().ends_with("."));
    CHECK_FALSE(shown_lines(*layout.label(dots)).front().ends_with(".."));

    // Without trimming, the rect grows to the text.
    CHECK(layout.find(wide)->size.x == 36.0F);
    // clip_text lets it stay narrow (not short), and the text overflows it.
    CHECK(layout.find(clipped)->size == Vec2{20.0F, 22.0F});
    CHECK(layout.label(clipped)->overflow);
    const ui::DrawList drawn = ui::draw_calls(layout, scene.world, scene.fonts());
    // Nothing of the clipped label is drawn outside its rect.
    const ui::Placed& placed = *layout.find(clipped);
    for (const ui::DrawCall& call : drawn.calls) {
        for (std::size_t v = 0; v < call.vertices.size(); v += 6) {
            const render::Vertex2D& first = call.vertices[v];
            if (first.x >= placed.position.x && first.y >= placed.position.y) {
                for (std::size_t k = v; k < v + 6; ++k) {
                    CHECK(call.vertices[k].x <= placed.position.x + 20.0F);
                    CHECK(call.vertices[k].y <= placed.position.y + 22.0F);
                }
            }
        }
    }
}

TEST_CASE("fill alignment stretches the spaces of a line to the rect's width") {
    Scene scene;
    const entt::entity filled = scene.create("filled", R"({"Control": {"offset_right": 200},
                      "Label": {"text": "a b c", "horizontal_alignment": "fill"}})");
    const ui::Layout layout = scene.layout();
    const ui::LabelText& label = *layout.label(filled);
    const float natural = label.text.lines[0].width;
    REQUIRE(natural < 200.0F);
    CHECK(label.stretch.front() == doctest::Approx((200.0F - natural) / 2.0F));
}

TEST_CASE("uppercase and visible_characters change what a label shows") {
    Scene scene;
    const entt::entity upper =
        scene.create("upper", R"({"Label": {"text": "héllo", "uppercase": true}})");
    const entt::entity before =
        scene.create("before", R"({"Label": {"text": "Hello", "visible_characters": 2}})");
    const entt::entity after =
        scene.create("after", R"({"Label": {"text": "Hello", "visible_characters": 2,
                               "visible_characters_behavior": "chars_after_shaping"}})");
    const ui::Layout layout = scene.layout();
    CHECK(layout.label(upper)->source == "HÉLLO");
    CHECK(layout.label(before)->source == "He");
    CHECK(layout.find(before)->size.x == scene.width_of("He"));
    // After shaping, the layout keeps every character and hides the rest when drawing.
    CHECK(layout.label(after)->source == "Hello");
    CHECK(layout.find(after)->size.x == 36.0F);
    CHECK(layout.label(after)->visible_bytes == 2);

    scene.world.destroy(upper);
    scene.world.destroy(before);
    const ui::Layout rest = scene.layout();
    CHECK(ui::draw_calls(rest, scene.world, scene.fonts()).glyphs == 2);
}

TEST_CASE("draw_calls draws color rects, then text with its shadow and outline under it") {
    Scene scene;
    scene.create("back", R"({"Control": {"offset_right": 50, "offset_bottom": 20,
                                         "self_modulate": [1, 1, 1, 0.5]},
                             "ColorRect": {"color": [1, 0, 0, 1]}})");
    const entt::entity label = scene.create(
        "label", R"({"Control": {"offset_left": 4, "offset_top": 2}, "Label": {"text": "Hi"}})");

    ui::DrawList drawn = ui::draw_calls(scene.layout(), scene.world, scene.fonts());
    CHECK(drawn.controls == 2);
    CHECK(drawn.glyphs == 2);
    REQUIRE(drawn.calls.size() == 2);
    CHECK(drawn.calls[0].source == ui::DrawCall::Source::None);
    REQUIRE(drawn.calls[0].vertices.size() == 6);
    CHECK(drawn.calls[0].vertices[0].rgba == std::array<std::uint8_t, 4>{255, 0, 0, 128});
    CHECK(drawn.calls[0].vertices[4].x == 50.0F);
    CHECK(drawn.calls[0].vertices[4].y == 20.0F);
    CHECK(drawn.calls[1].source == ui::DrawCall::Source::Atlas);
    CHECK(drawn.calls[1].vertices.size() == 12);
    // Glyph images draw on whole pixels.
    for (const render::Vertex2D& vertex : drawn.calls[1].vertices) {
        CHECK(vertex.x == std::floor(vertex.x));
        CHECK(vertex.y == std::floor(vertex.y));
    }

    scene.set(label, R"({"Control": {"offset_left": 4, "offset_top": 2,
                                     "theme_override_colors": {"font_shadow_color": [0, 0, 0, 1]},
                                     "theme_override_constants": {"outline_size": 4}}})");
    drawn = ui::draw_calls(scene.layout(), scene.world, scene.fonts());
    // The shadow's outline, the shadow, the outline and the text: two glyphs each.
    CHECK(drawn.glyphs == 8);
    // They share the atlas page, so they are one call after the rect's.
    CHECK(drawn.calls.size() == 2);
}

TEST_CASE("check finds theme overrides a control's type doesn't have, and fonts that don't load") {
    Scene scene;
    scene.create("title", R"({"Control": {"theme_override_colors": {"font_colour": [1, 1, 1, 1]},
                                          "theme_override_fonts": {"font": "fonts/missing.ttf"}},
                              "Label": {"text": "Hi"}})");
    scene.create("rect", R"({"Control": {"theme_override_constants": {"margin": 4}},
                             "ColorRect": {}})");
    const std::vector<ui::Problem> problems = ui::check(scene.world, scene.fonts());
    std::vector<std::string> lines;
    for (const ui::Problem& problem : problems) {
        lines.push_back(problem.id + problem.path + ": " + problem.message);
    }
    CHECK(lines == std::vector<std::string>{
                       "rect/components/Control/theme_override_constants/margin: ColorRect has "
                       "no constants item \"margin\"; it has none",
                       "title/components/Control/theme_override_colors/font_colour: Label has no "
                       "colors item \"font_colour\"; did you mean \"font_color\"?",
                       "title/components/Control/theme_override_fonts/font: asset "
                       "\"fonts/missing.ttf\" could not be read: no such file",
                   });
    // The label still draws, with the default font.
    const ui::Layout layout = scene.layout();
    const ui::LabelText* label = layout.label(scene.world.find("title"));
    REQUIRE(label != nullptr);
    CHECK(label->error == "asset \"fonts/missing.ttf\" could not be read: no such file");
    CHECK(label->font.empty());
    CHECK(label->text.width > 0.0F);
}

TEST_CASE("the canvas renderer draws controls above the world") {
    MemoryFiles files;
    asset::Assets assets{files.source()};
    world::ComponentRegistry components;
    world::register_builtin_components(components);
    world::World world{components};
    canvas::install(world, assets);
    const entt::entity entity = world.create("hello").value();
    world.ecs().emplace<world::Label>(entity, world::Label{.text = "Hello"});
    world.ecs().emplace<world::ColorRect>(world.create("rect").value());

    const auto device = render::create_null_device();
    canvas::CanvasRenderer renderer(*device, assets);
    REQUIRE(renderer.draw(world, {}).ok());
    CHECK(renderer.controls_drawn() == 2);
    CHECK(renderer.glyphs_drawn() == 5);
}

namespace {

std::string default_font_bytes() {
    const auto bytes = text::default_font();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

// What drawing a list of draws over a color should give, worked out on the CPU: each quad's
// pixels sampled from its texture as the GPU samples it, read as a distance field if it is one,
// and blended over what is there.
std::vector<float> composite(const ui::DrawList& drawn, const text::Fonts& fonts,
                             std::uint32_t width, std::uint32_t height) {
    std::vector<float> pixels(std::size_t{width} * height * 4);
    for (std::size_t i = 0; i < pixels.size(); i += 4) {
        pixels[i + 3] = 1.0F;
    }
    for (const ui::DrawCall& call : drawn.calls) {
        render::Image page;
        if (call.source == ui::DrawCall::Source::Atlas) {
            const text::AtlasPage& atlas = fonts.pages().at(call.page);
            page = {.width = atlas.width, .height = atlas.height, .pixels = atlas.rgba};
        }
        for (std::size_t v = 0; v + 6 <= call.vertices.size(); v += 6) {
            const render::Vertex2D& a = call.vertices[v];
            const render::Vertex2D& b = call.vertices[v + 4];
            const float scale = page.width == 0
                                    ? 1.0F
                                    : (b.x - a.x) / ((b.u - a.u) * static_cast<float>(page.width));
            for (std::uint32_t y = 0; y < height; ++y) {
                for (std::uint32_t x = 0; x < width; ++x) {
                    const float cx = static_cast<float>(x) + 0.5F;
                    const float cy = static_cast<float>(y) + 0.5F;
                    if (cx < a.x || cx >= b.x || cy < a.y || cy >= b.y) {
                        continue;
                    }
                    const float u = a.u + ((cx - a.x) / (b.x - a.x) * (b.u - a.u));
                    const float t = a.v + ((cy - a.y) / (b.y - a.y) * (b.v - a.v));
                    const std::array<float, 4> texel =
                        render::sample_image(page, render::Filter::Linear, false, u, t);
                    std::array<float, 4> color{};
                    for (std::size_t c = 0; c < 4; ++c) {
                        color.at(c) = static_cast<float>(a.rgba.at(c)) / 255.0F;
                    }
                    if (call.field.pixel_range > 0.0F) {
                        color[3] *= render::distance_field_coverage(texel, call.field, scale);
                    } else {
                        for (std::size_t c = 0; c < 4; ++c) {
                            color.at(c) *= texel.at(c);
                        }
                    }
                    float* out = &pixels[((std::size_t{y} * width) + x) * 4];
                    for (std::size_t c = 0; c < 3; ++c) {
                        out[c] = (color.at(c) * color[3]) + (out[c] * (1.0F - color[3]));
                    }
                }
            }
        }
    }
    return pixels;
}

} // namespace

TEST_CASE("text draws on a GPU as the CPU works it out, from glyph images and distance fields") {
    auto created = render::create_device({.window = nullptr, .driver = "auto", .debug = false});
    if (!created) {
        if (!slash::platform::environment_variable("SLASH_EXPECT_GPU").empty()) {
            FAIL("SLASH_EXPECT_GPU is set, but no GPU device works: " << created.error().message);
        }
        MESSAGE("no GPU here, so text pixels are not checked: " << created.error().message);
        return;
    }
    const std::unique_ptr<render::RenderDevice> device = std::move(created).value();
    Scene scene;
    scene.files.files["fonts/roboto.ttf"] = default_font_bytes();
    scene.files.files["fonts/sharp.font.json"] =
        R"({"format": 1, "font": "fonts/roboto.ttf", "multichannel_signed_distance_field": true})";
    constexpr std::uint32_t kWidth = 96;
    constexpr std::uint32_t kHeight = 64;
    scene.create("back", R"({"Control": {"offset_right": 96, "offset_bottom": 64},
                             "ColorRect": {"color": [0.1, 0.2, 0.6, 1]}})");
    scene.create("small", R"({"Control": {"offset_left": 3, "offset_top": 2},
                              "Label": {"text": "Hello!"}})");
    scene.create("sharp", R"({"Control": {"offset_left": 2, "offset_top": 22,
                                          "theme_override_fonts": {"font": "fonts/sharp.font.json"},
                                          "theme_override_font_sizes": {"font_size": 30},
                                          "theme_override_colors": {"font_color": [1, 0.9, 0.2, 1]}},
                              "Label": {"text": "Big"}})");

    auto target = device->create_texture({.width = kWidth,
                                          .height = kHeight,
                                          .filter = render::Filter::Nearest,
                                          .render_target = true});
    REQUIRE(target.ok());
    canvas::CanvasRenderer renderer(*device, scene.assets);
    canvas::CanvasOptions options;
    options.target = target.value();
    options.width = kWidth;
    options.height = kHeight;
    options.clear = {.r = 0.0F, .g = 0.0F, .b = 0.0F, .a = 1.0F};
    REQUIRE(renderer.draw(scene.world, options).ok());
    CHECK(renderer.controls_drawn() == 3);
    CHECK(renderer.glyphs_drawn() == 9);
    auto image = device->read_texture(target.value());
    REQUIRE(image.ok());

    const ui::Layout layout = ui::layout(scene.world, scene.fonts(), {96.0F, 64.0F});
    const ui::DrawList drawn = ui::draw_calls(layout, scene.world, scene.fonts());
    const std::vector<float> expected = composite(drawn, scene.fonts(), kWidth, kHeight);
    // The small label's rows, drawn from glyph images, and the big one's, from a distance field.
    std::array<int, 2> worst{};
    std::array<int, 2> total{};
    std::size_t text_pixels = 0;
    for (std::size_t i = 0; i < expected.size(); i += 4) {
        const std::size_t part = (i / 4) / kWidth < 22 ? 0 : 1;
        for (std::size_t c = 0; c < 3; ++c) {
            const int want = static_cast<int>(std::lround(expected[i + c] * 255.0F));
            const int diff = std::abs(want - int{image.value().pixels[i + c]});
            worst.at(part) = std::max(worst.at(part), diff);
            total.at(part) += diff;
        }
        if (image.value().pixels[i] > 128) {
            ++text_pixels;
        }
    }
    // Glyph images are drawn texel for texel, so they match to rounding. Distance fields are
    // filtered between texels, which GPUs do with less precision than floats (8 bits of weight
    // on Mesa's software Vulkan), so edge pixels can differ by a few levels.
    CHECK(worst[0] <= 1);
    CHECK(worst[1] <= 12);
    CHECK(total[1] < static_cast<int>(kWidth * (kHeight - 22)));
    CHECK(text_pixels > 100);
}
