#include "slash/asset/assets.hpp"
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/input/input.hpp"
#include "slash/text/text.hpp"
#include "slash/ui/ui.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Scroll bars, scroll containers and clipping, as Godot's ScrollBar, ScrollContainer and
// Control.clip_contents (scene/gui/scroll_bar.cpp and scroll_container.cpp). The sizes and
// values expected here follow those files and Godot's default theme: scroll bars with no arrow
// icons and a grabber at least 8 pixels long.

namespace asset = slash::asset;
namespace command = slash::command;
namespace input = slash::input;
namespace json = slash::json;
namespace text = slash::text;
namespace ui = slash::ui;
namespace world = slash::world;
using slash::Vec2;

namespace {

constexpr double kTick = 1.0 / 60.0;

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
    REQUIRE_MESSAGE(parsed.ok(), std::string(source));
    return std::move(parsed).value();
}

std::string compact(const json::Value& value) {
    return json::write(value, json::Style::Compact);
}

// A number as JSON writes it, whole numbers without ".0".
std::string number(double value) {
    if (value == std::floor(value) && std::abs(value) < 1e15) {
        return std::to_string(static_cast<std::int64_t>(value));
    }
    return compact(json::Value(value));
}
std::string numbers(std::initializer_list<double> values) {
    std::string out = "[";
    for (const double value : values) {
        out += (out.size() > 1 ? "," : "") + number(value);
    }
    return out + "]";
}

std::string point(int x, int y) {
    return "[" + std::to_string(x) + ", " + std::to_string(y) + "]";
}
std::string motion(int x, int y) {
    return R"({"type": "mouse_motion", "position": )" + point(x, y) + "}";
}
std::string click(int x, int y, bool pressed, std::string_view button = "left",
                  bool shift = false) {
    return R"({"type": "mouse_button", "button": ")" + std::string(button) + R"(", "position": )" +
           point(x, y) + ", \"pressed\": " + (pressed ? "true" : "false") +
           (shift ? ", \"shift\": true" : "") + "}";
}
std::string touch(int x, int y, bool pressed) {
    return R"({"type": "screen_touch", "index": 0, "position": )" + point(x, y) +
           ", \"pressed\": " + (pressed ? "true" : "false") + "}";
}
std::string drag(int x, int y, int dx, int dy) {
    return R"({"type": "screen_drag", "index": 0, "position": )" + point(x, y) +
           ", \"relative\": " + point(dx, dy) + "}";
}

// A world with the built-in components, ui and input, in a 400 x 300 viewport.
struct Gui {
    MemoryFiles files;
    asset::Assets assets{files.source()};
    world::ComponentRegistry components;
    world::World world{components};

    Gui() {
        world::register_builtin_components(components);
        ui::install(world, assets, {.viewport = {400.0F, 300.0F}, .theme = ""});
        input::install(world, input::Map::with_defaults(), {});
    }

    entt::entity create(std::string_view id, std::string_view components_json) {
        const entt::entity entity = world.create(id).value();
        set(id, components_json);
        return entity;
    }

    void set(std::string_view id, std::string_view components_json) {
        const json::Value values = parse(components_json);
        for (const auto& [name, fields] : values.as_object()) {
            std::string path;
            auto assigned = components.find(name)->assign(world, world.find(id), fields, path);
            REQUIRE_MESSAGE(assigned.ok(), assigned.error().message);
        }
    }

    // A ScrollContainer "box" over the window's top left 100 x 100, holding "content" of a
    // minimum size, with box's fields and content's components and Control fields added.
    void scroll_box(Vec2 content, std::string_view box_fields = "",
                    std::string_view content_components = R"("ColorRect": {})",
                    std::string_view box_control = "", std::string_view content_control = "") {
        create("content", R"({"Control": {"custom_minimum_size": )" +
                              numbers({content.x, content.y}) +
                              (content_control.empty() ? "" : ", " + std::string(content_control)) +
                              "}, " + std::string(content_components) + "}");
        create("box", R"({"Control": {"offset_right": 100, "offset_bottom": 100, )" +
                          (box_control.empty() ? "" : std::string(box_control) + ", ") +
                          R"("children": ["content"]}, "ScrollContainer": {)" +
                          std::string(box_fields) + "}}");
    }

    void push(std::string_view event_json) {
        auto parsed = input::parse_event(parse(event_json), input::Use::Event);
        REQUIRE_MESSAGE(parsed.ok(), parsed.error().message);
        input::push(world, std::move(parsed).value());
    }

    // Queues events, steps one tick and returns that tick's UI events.
    std::string send(std::initializer_list<std::string_view> events) {
        for (const std::string_view event : events) {
            push(event);
        }
        return step();
    }
    std::string send(std::string_view event) { return send({event}); }
    // A wheel turn at a point: its press and release.
    std::string wheel(int x, int y, std::string_view button, bool shift = false) {
        return send({click(x, y, true, button, shift), click(x, y, false, button, shift)});
    }

    std::string step() {
        world.step(kTick);
        return events();
    }

    // This tick's UI events, with a scroll bar's value.
    [[nodiscard]] std::string events() const {
        std::string out;
        for (const ui::UiEvent& event : ui::events(world)) {
            if (!out.empty()) {
                out += ", ";
            }
            out += event.kind + " " + event.id;
            if (event.kind == "value_changed") {
                out += " " + number(event.value);
            }
        }
        return out;
    }

    text::Fonts& fonts() { return *ui::fonts(world); }
    ui::Layout layout() { return ui::layout(world, fonts(), ui::settings(world).viewport); }

    // A ScrollContainer's scroll as its component has it, [horizontal, vertical].
    [[nodiscard]] std::string scroll(std::string_view id = "box") const {
        const auto& box = world.ecs().get<world::ScrollContainer>(world.find(id));
        return numbers(
            {static_cast<double>(box.scroll_horizontal), static_cast<double>(box.scroll_vertical)});
    }

    [[nodiscard]] double value(std::string_view id) const {
        return world.ecs().get<world::ScrollBar>(world.find(id)).value;
    }

    // A control's rect as [x, y, w, h].
    std::string rect(std::string_view id) {
        const ui::Layout placed = layout();
        const ui::Placed* found = placed.find(world.find(id));
        if (found == nullptr) {
            return "none";
        }
        const Vec2 origin = found->transform.apply({0.0F, 0.0F});
        return numbers({origin.x, origin.y, found->size.x, found->size.y});
    }

    // Boxes as "flat [x, y, w, h]", the empty ones left out.
    static std::string boxes(const std::vector<ui::Box>& list) {
        std::string out;
        for (const ui::Box& box : list) {
            if (box.style.type == ui::StyleBox::Type::Empty) {
                continue;
            }
            if (!out.empty()) {
                out += ", ";
            }
            out += box.style.type == ui::StyleBox::Type::Texture ? box.style.texture : "flat";
            out += " " + numbers({box.position.x, box.position.y, box.size.x, box.size.y});
        }
        return out;
    }
    std::string boxes(std::string_view id) {
        const ui::Layout placed = layout();
        const auto found = placed.boxes.find(world.find(id));
        return found != placed.boxes.end() ? boxes(found->second) : std::string();
    }
    std::string over_children(std::string_view id) {
        const ui::Layout placed = layout();
        const auto found = placed.over_children.find(world.find(id));
        return found != placed.over_children.end() ? boxes(found->second) : std::string();
    }

    // A bar's rect in its control as "[x, y, w, h]", or "hidden".
    static std::string bar(const ui::ScrollBarView& view) {
        if (!view.visible) {
            return "hidden";
        }
        return numbers({view.position.x, view.position.y, view.size.x, view.size.y});
    }
    std::string bars(std::string_view id = "box") {
        const ui::Layout placed = layout();
        const ui::ScrollView& view = placed.scrolls.at(world.find(id));
        return "h " + bar(view.h) + ", v " + bar(view.v);
    }

    // A grabber's color, to tell its normal, highlighted and pressed styles apart.
    std::string grabber_color(std::string_view id) {
        const ui::Layout placed = layout();
        const ui::Box& grabber = placed.boxes.at(world.find(id)).back();
        const slash::Vec4 c = grabber.style.bg_color;
        return numbers({c.x, c.y, c.z, c.w});
    }
};

// A session over a scene, for the commands.
struct Session {
    world::ComponentRegistry components;
    command::Registry commands;
    std::unique_ptr<command::Session> session;

    Session() {
        world::register_builtin_components(components);
        command::register_builtin_commands(commands);
        command::SessionOptions options;
        options.capture_logs = false;
        session = std::make_unique<command::Session>(components, commands, options);
    }

    json::Value ok(std::string_view name, std::string_view params = "{}") const {
        command::Reply reply = session->call(name, parse(params));
        REQUIRE_MESSAGE(reply.ok, reply.message);
        return std::move(reply.value);
    }
};

} // namespace

TEST_CASE("a scroll bar sizes its grabber by its page and places it by its value") {
    Gui gui;
    // 200 x 12, from 0 to 100 with a page of 25, at 50.
    gui.create("bar", R"({"Control": {"offset_left": 10, "offset_top": 20, "offset_right": 210,
                          "offset_bottom": 32}, "ScrollBar": {"page": 25, "value": 50}})");
    const ui::Layout layout = gui.layout();
    const ui::ScrollBarView& view = layout.scroll_bars.at(gui.world.find("bar"));
    CHECK(view.area == 192.0);        // its length less the smallest grabber
    CHECK(view.grabber_size == 56.0); // a quarter of the area, and 8
    CHECK(view.grabber_offset == 96.0);
    CHECK(view.ratio == 0.5);
    // Its track, then its grabber, the bar's height less the padding (none).
    CHECK(gui.boxes("bar") == "flat [0,0,200,12], flat [96,0,56,12]");

    // With no size of its own it is as small as its track and grabber allow, either way.
    gui.create("h", R"({"Control": {}, "ScrollBar": {}})");
    gui.create("v", R"({"Control": {}, "ScrollBar": {"vertical": true}})");
    CHECK(gui.rect("h") == "[0,0,8,8]");
    CHECK(gui.rect("v") == "[0,0,8,8]");
    CHECK(gui.boxes("v") == "flat [0,0,8,8], flat [0,0,8,8]");
    CHECK(ui::control_class(gui.world, gui.world.find("v")) == "VScrollBar");
}

TEST_CASE("clicks page a scroll bar, its grabber drags, and the wheel moves it") {
    Gui gui;
    gui.create("bar", R"({"Control": {"offset_left": 10, "offset_top": 20, "offset_right": 210,
                          "offset_bottom": 32}, "ScrollBar": {"page": 25, "value": 50}})");
    CHECK(gui.step().empty());
    // The grabber spans 106 to 162: a click before it goes back a page.
    CHECK(gui.send({click(50, 26, true), click(50, 26, false)}) ==
          "mouse_entered bar, value_changed bar 25, scrolling bar");
    // Now it spans 58 to 114. Grabbed 10 in and dragged 48 right, a quarter of the area, it
    // goes up a quarter of the range.
    CHECK(gui.send({click(68, 26, true), motion(116, 26)}) ==
          "value_changed bar 50, scrolling bar");
    CHECK(gui.send(click(116, 26, false)).empty());
    // After it: forward a page, kept to max less page.
    CHECK(gui.send({click(200, 26, true), click(200, 26, false)}) ==
          "value_changed bar 75, scrolling bar");
    CHECK(gui.send({click(200, 26, true), click(200, 26, false)}).empty());
    // The wheel moves an eighth of a page.
    CHECK(gui.wheel(100, 26, "wheel_up") == "value_changed bar 71.875, scrolling bar");
    // Its value set from outside is kept in range at the next step, and told.
    gui.world.ecs().get<world::ScrollBar>(gui.world.find("bar")).value = 300.0;
    CHECK(gui.step() == "value_changed bar 75");
    CHECK(gui.value("bar") == 75.0);
}

TEST_CASE("a scroll bar's grabber lights up under the pointer and while dragged") {
    Gui gui;
    gui.create("bar", R"({"Control": {"offset_right": 200, "offset_bottom": 12},
                          "ScrollBar": {"page": 25}})");
    gui.step();
    const std::string normal = gui.grabber_color("bar");
    gui.send(motion(100, 6));
    const std::string highlight = gui.grabber_color("bar");
    CHECK(highlight != normal);
    gui.send(click(20, 6, true));
    const std::string pressed = gui.grabber_color("bar");
    CHECK(pressed != highlight);
    CHECK(pressed != normal);
    gui.send(click(20, 6, false));
    CHECK(gui.grabber_color("bar") == highlight);
    // Leaving it puts the highlight out.
    gui.send(motion(300, 200));
    CHECK(gui.grabber_color("bar") == normal);
}

TEST_CASE("a scroll container shows bars when its child doesn't fit, and moves it by its scroll") {
    Gui gui;
    gui.scroll_box({300.0F, 150.0F});
    // Both bars, along the far edges; each page is the room the other bar leaves.
    CHECK(gui.bars() == "h [0,92,92,8], v [92,0,8,92]");
    const ui::Layout layout = gui.layout();
    const ui::ScrollView& view = layout.scrolls.at(gui.world.find("box"));
    CHECK(view.h.page == 92.0);
    CHECK(view.v.page == 92.0);
    CHECK(view.scroll_max == Vec2{208.0F, 58.0F});
    CHECK(view.visible_size == Vec2{92.0F, 92.0F});
    CHECK(gui.rect("content") == "[0,0,300,150]");
    gui.set("box", R"({"ScrollContainer": {"scroll_horizontal": 30, "scroll_vertical": 40}})");
    CHECK(gui.rect("content") == "[-30,-40,300,150]");
    // Its bars draw over its child.
    CHECK_FALSE(gui.over_children("box").empty());
}

TEST_CASE("scroll modes hide, show and keep room for the bars") {
    SUBCASE("a child that fits needs no bars") {
        Gui gui;
        gui.scroll_box({80.0F, 60.0F});
        CHECK(gui.bars() == "h hidden, v hidden");
    }
    SUBCASE("always shows a bar, and never hides one that would show") {
        Gui gui;
        gui.scroll_box({80.0F, 150.0F},
                       R"("horizontal_scroll_mode": "always", "vertical_scroll_mode": "never")");
        CHECK(gui.bars() == "h [0,92,100,8], v hidden");
    }
    SUBCASE("reserve keeps the bar's room even while it is hidden") {
        Gui gui;
        gui.scroll_box({80.0F, 60.0F}, R"("vertical_scroll_mode": "reserve")", R"("ColorRect": {})",
                       "", R"("size_flags_horizontal": "expand_fill")");
        CHECK(gui.bars() == "h hidden, v hidden");
        CHECK(gui.rect("content") == "[0,0,92,60]");
    }
    SUBCASE("disabled takes the child's whole size into its own minimum") {
        Gui gui;
        gui.scroll_box({300.0F, 150.0F}, R"("horizontal_scroll_mode": "disabled")");
        CHECK(gui.rect("box") == "[0,0,308,100]");
        CHECK(gui.bars() == "h hidden, v [300,0,8,100]");
    }
    SUBCASE("right to left, the vertical bar goes on the left and the child after it") {
        Gui gui;
        gui.scroll_box({300.0F, 150.0F}, "", R"("ColorRect": {})", R"("layout_direction": "rtl")");
        CHECK(gui.bars() == "h [8,92,92,8], v [0,0,8,92]");
        // The box itself is mirrored to the viewport's right edge.
        CHECK(gui.rect("content") == "[308,0,300,150]");
    }
}

TEST_CASE("the wheel scrolls a scroll container an eighth of a page, Shift sideways") {
    Gui gui;
    gui.scroll_box({300.0F, 150.0F});
    gui.step();
    gui.wheel(50, 50, "wheel_down");
    // 92 / 8 = 11.5: whole pixels in the component, the fraction kept for the next turn.
    CHECK(gui.scroll() == "[0,11]");
    gui.wheel(50, 50, "wheel_down");
    CHECK(gui.scroll() == "[0,23]");
    gui.wheel(50, 50, "wheel_down", true);
    CHECK(gui.scroll() == "[11,23]");
    gui.wheel(50, 50, "wheel_right");
    CHECK(gui.scroll() == "[23,23]");
    gui.wheel(50, 50, "wheel_up");
    CHECK(gui.scroll() == "[23,11]");
    CHECK(gui.rect("content") == "[-23,-11,300,150]");
    // scroll_horizontal_by_default turns the wheel sideways, and Shift back.
    gui.set("box", R"({"ScrollContainer": {"scroll_horizontal_by_default": true}})");
    gui.wheel(50, 50, "wheel_up");
    CHECK(gui.scroll() == "[11,11]");
    gui.wheel(50, 50, "wheel_up", true);
    CHECK(gui.scroll() == "[11,0]");
}

TEST_CASE("a wheel turn a scroll container can't take goes on to the one around it") {
    Gui gui;
    // outer holds a column of inner, 100 high, and 200 more; inner holds 200.
    gui.create("inner_content",
               R"({"Control": {"custom_minimum_size": [80, 200]}, "ColorRect": {}})");
    gui.create("inner", R"({"Control": {"custom_minimum_size": [92, 100],
                            "children": ["inner_content"]}, "ScrollContainer": {}})");
    gui.create("spacer", R"({"Control": {"custom_minimum_size": [80, 200]}})");
    gui.create("column", R"({"Control": {"children": ["inner", "spacer"]},
                             "BoxContainer": {"vertical": true}})");
    gui.create("outer", R"({"Control": {"offset_right": 100, "offset_bottom": 100,
                            "children": ["column"]}, "ScrollContainer": {}})");
    gui.step();
    gui.wheel(40, 40, "wheel_down");
    CHECK(gui.scroll("inner") == "[0,12]");
    CHECK(gui.scroll("outer") == "[0,0]");
    // At its end, inner lets the turn go on.
    gui.set("inner", R"({"ScrollContainer": {"scroll_vertical": 100}})");
    gui.wheel(40, 40, "wheel_down");
    CHECK(gui.scroll("inner") == "[0,100]");
    CHECK(gui.scroll("outer") == "[0,12]");
}

TEST_CASE("a scroll container's bars page and drag it") {
    Gui gui;
    gui.scroll_box({300.0F, 150.0F});
    gui.step();
    // Below the vertical bar's grabber: down a page, kept to the end.
    gui.send({click(96, 80, true), click(96, 80, false)});
    CHECK(gui.scroll() == "[0,58]");
    gui.set("box", R"({"ScrollContainer": {"scroll_vertical": 0}})");
    gui.step();
    // Its grabber is 84 * 92 / 150 + 8 long. Grabbed 10 down and dragged 21 more: a quarter.
    gui.send({click(96, 10, true), motion(96, 31)});
    CHECK(gui.scroll() == "[0,37]");
    gui.send(click(96, 31, false));
    // A click on a bar doesn't reach the child under it.
    CHECK(gui.rect("content") == "[0,-37,300,150]");
}

TEST_CASE("a finger drags a scroll container past its deadzone, and it glides when let go") {
    Gui gui;
    // The content lets the pointer pass, so its touches reach the box.
    gui.scroll_box({300.0F, 150.0F}, R"("scroll_deadzone": 10)", R"("ColorRect": {})", "",
                   R"("mouse_filter": "pass")");
    gui.step();
    // The touch is also an emulated mouse, which enters both.
    CHECK(gui.send(touch(50, 50, true)) == "mouse_entered box, mouse_entered content");
    // 5 pixels is inside the deadzone.
    CHECK(gui.send(drag(50, 45, 0, -5)).empty());
    CHECK(gui.scroll() == "[0,0]");
    // Past it, the scroll starts from there: by this motion only.
    CHECK(gui.send(drag(50, 30, 0, -15)) == "scroll_started box");
    CHECK(gui.scroll() == "[0,15]");
    // Let go at 900 pixels a second, it glides on, slowing by 1000 a second each second.
    CHECK(gui.send(touch(50, 30, false)).empty());
    CHECK(gui.scroll() == "[0,30]");
    CHECK(gui.step().empty());
    CHECK(gui.scroll() == "[0,44]");
    CHECK(gui.step() == "scroll_ended box");
    CHECK(gui.scroll() == "[0,58]");
}

TEST_CASE("a touch on a control that stops the pointer doesn't drag the scroll container") {
    Gui gui;
    gui.scroll_box({300.0F, 150.0F});
    gui.step();
    gui.send(touch(50, 50, true));
    CHECK(gui.send(drag(50, 20, 0, -30)).empty());
    CHECK(gui.scroll() == "[0,0]");
}

TEST_CASE("follow_focus scrolls a control that gets focus into view") {
    Gui gui;
    std::string children;
    for (int i = 0; i < 6; ++i) {
        const std::string id = "b" + std::to_string(i);
        gui.create(id, R"({"Control": {"custom_minimum_size": [80, 40]}, "Button": {}})");
        children += (children.empty() ? "\"" : ", \"") + id + "\"";
    }
    gui.create("list", R"({"Control": {"children": [)" + children + R"(]},
                           "BoxContainer": {"vertical": true}})");
    gui.create("box", R"({"Control": {"offset_right": 100, "offset_bottom": 100,
                          "children": ["list"]}, "ScrollContainer": {"follow_focus": true}})");
    gui.step();
    // b4 spans 176 to 216: scrolled until its bottom is the view's.
    REQUIRE(ui::grab_focus(gui.world, gui.world.find("b4")));
    gui.step();
    CHECK(gui.scroll() == "[0,116]");
    CHECK(gui.rect("b4") == "[0,60,80,40]");
    // b1 is above the view: scrolled until its top is the view's.
    REQUIRE(ui::grab_focus(gui.world, gui.world.find("b1")));
    gui.step();
    CHECK(gui.scroll() == "[0,44]");
    // Without follow_focus it stays.
    gui.set("box", R"({"ScrollContainer": {"follow_focus": false}})");
    REQUIRE(ui::grab_focus(gui.world, gui.world.find("b5")));
    gui.step();
    CHECK(gui.scroll() == "[0,44]");
}

TEST_CASE("draw_focus_border draws the focus style over the children while one has focus") {
    Gui gui;
    gui.scroll_box({80.0F, 60.0F}, R"("draw_focus_border": true)", R"("Button": {})");
    gui.step();
    CHECK(gui.over_children("box").empty());
    REQUIRE(ui::grab_focus(gui.world, gui.world.find("content")));
    CHECK_FALSE(gui.over_children("box").empty());
}

TEST_CASE("a clip cuts a control's drawing and the clicks its children get at its rect") {
    Gui gui;
    gui.scroll_box({300.0F, 150.0F}, "", R"("Button": {"text": "Wide"})");
    gui.step();
    const ui::Layout layout = gui.layout();
    const ui::Placed* content = layout.find(gui.world.find("content"));
    REQUIRE(content->clip.has_value());
    CHECK(*content->clip == slash::Vec4{0.0F, 0.0F, 100.0F, 100.0F});
    // Its triangles stay inside the box; as it would draw whole, they don't.
    const ui::DrawList drawn = ui::draw_calls(layout, gui.world, gui.fonts());
    float right = 0.0F;
    for (const ui::DrawSpan& span : drawn.spans) {
        if (span.entity != gui.world.find("content")) {
            continue;
        }
        const auto& vertices = drawn.calls[span.call].vertices;
        for (std::size_t i = span.first; i < span.first + span.count; ++i) {
            right = std::max(right, vertices[i].x);
        }
    }
    CHECK(right == 100.0F);
    const auto uncut = std::ranges::find_if(drawn.uncut, [&](const ui::UncutDraw& each) {
        return each.entity == gui.world.find("content");
    });
    REQUIRE(uncut != drawn.uncut.end());
    // The bars draw last, over the button.
    CHECK(drawn.spans.back().entity == gui.world.find("box"));
    // A click on the button's part outside the box doesn't reach it; one inside does.
    CHECK(gui.send({click(150, 50, true), click(150, 50, false)}).empty());
    CHECK(gui.send({click(50, 50, true), click(50, 50, false)}) ==
          "mouse_entered content, focus_entered content, button_down content, pressed content, "
          "button_up content");
}

TEST_CASE("clip_contents clips any control") {
    Gui gui;
    gui.create("inside",
               R"({"Control": {"offset_left": 20, "offset_right": 120, "offset_bottom": 10},
                             "ColorRect": {}})");
    gui.create("frame", R"({"Control": {"offset_right": 50, "offset_bottom": 50,
                            "clip_contents": true, "children": ["inside"]}})");
    const ui::Layout layout = gui.layout();
    CHECK(*layout.find(gui.world.find("inside"))->clip == slash::Vec4{0.0F, 0.0F, 50.0F, 50.0F});
    CHECK(layout.find(gui.world.find("frame"))->clips);
}

TEST_CASE("a scroll set from outside is kept in range and written back in whole pixels") {
    Gui gui;
    gui.scroll_box({300.0F, 150.0F});
    gui.set("box", R"({"ScrollContainer": {"scroll_horizontal": -20, "scroll_vertical": 9999}})");
    gui.step();
    CHECK(gui.scroll() == "[0,58]");
}

TEST_CASE("ui.layout reports scrolls and clips, and frame.summary what clips cut away") {
    const Session session;
    session.ok("batch", R"({"commands": [
        {"command": "entity.create", "params": {"id": "top", "components": {
            "Control": {"custom_minimum_size": [80, 40]}, "Label": {"text": "Top"}}}},
        {"command": "entity.create", "params": {"id": "bottom", "components": {
            "Control": {"custom_minimum_size": [80, 40]}, "Label": {"text": "Bottom"}}}},
        {"command": "entity.create", "params": {"id": "list", "components": {
            "Control": {"children": ["top", "bottom"],
                        "theme_override_constants": {"separation": 200}},
            "BoxContainer": {"vertical": true}}}},
        {"command": "entity.create", "params": {"id": "box", "components": {
            "Control": {"offset_right": 100, "offset_bottom": 100, "children": ["list"]},
            "ScrollContainer": {}}}},
        {"command": "entity.create", "params": {"id": "bar", "components": {
            "Control": {"offset_left": 200, "offset_right": 400, "offset_bottom": 12},
            "ScrollBar": {"page": 50, "value": 25}}}}]})");
    session.ok("step", R"({"ticks": 1})");
    CHECK(compact(session.ok("ui.layout", R"({"ids": ["box", "top", "bottom", "bar"]})")) ==
          R"({"controls":[{"id":"box","rect":[0,0,100,100],"scroll":[0,0],)"
          R"("scroll_bars":["vertical"],"scroll_max":[0,180]},)"
          R"({"id":"top","lines":["Top"],"rect":[0,0,80,40]},)"
          R"({"clipped":true,"id":"bottom","lines":["Bottom"],"rect":[0,240,80,40]},)"
          R"({"id":"bar","ratio":0.25,"rect":[200,0,200,12],"value":25}],"viewport":[1280,720]})");
    const json::Value summary = session.ok("frame.summary", R"({"ids": ["top", "bottom"]})");
    CHECK(compact(*summary.find("clipped")) == R"(["bottom"])");
}
