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

#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Tooltips and drag and drop, as Godot's Viewport (scene/main/viewport.cpp: _gui_show_tooltip,
// _gui_input_event, _perform_drop), Control (get_drag_data, can_drop_data, drop_data,
// set_drag_preview, force_drag) and the text fields' own drag and drop (scene/gui/line_edit.cpp
// and text_edit.cpp). The sizes expected here follow those files and Godot's default theme.

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
// Ticks of rest before a tooltip shows: Godot's default 0.5 seconds.
constexpr int kRest = 30;
// The box font at 20 pixels: 12 pixels a letter, 5 a space; a text field's text starts 4 pixels
// in.
constexpr float kBox = 12.0F;
constexpr float kLeft = 4.0F;
constexpr float kRow = 24.0F;

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

std::string bytes_of(const std::vector<std::uint8_t>& bytes) {
    return {bytes.begin(), bytes.end()};
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

std::string point(float x, float y) {
    return "[" + number(x) + ", " + number(y) + "]";
}
// Mouse motion; how far it moved and the buttons held come from the input state.
std::string move(float x, float y) {
    return R"({"type": "mouse_motion", "position": )" + point(x, y) + "}";
}
std::string click(float x, float y, bool pressed, std::string_view button = "left",
                  std::string_view extra = "") {
    std::string out = R"({"type": "mouse_button", "button": ")" + std::string(button) +
                      R"(", "position": )" + point(x, y) +
                      ", \"pressed\": " + (pressed ? "true" : "false");
    if (!extra.empty()) {
        out += ", " + std::string(extra);
    }
    return out + "}";
}
std::string press(float x, float y) {
    return click(x, y, true);
}
std::string release(float x, float y) {
    return click(x, y, false);
}
std::string key(std::string_view name, bool pressed) {
    return R"({"type": "key", "key": ")" + std::string(name) +
           "\", \"pressed\": " + (pressed ? "true" : "false") + "}";
}
// The key that copies a dragged selection instead of moving it: Command on a Mac, Ctrl elsewhere.
std::string_view copy_key() {
    return input::host_keyboard() == input::Keyboard::Mac ? "Meta" : "Ctrl";
}

// A world with the built-in components, ui and input with a PC keyboard's shortcuts, in a 400
// x 300 viewport, with a theme that draws tooltips and text fields in a box font.
struct Gui {
    MemoryFiles files;
    asset::Assets assets{files.source()};
    world::ComponentRegistry components;
    world::World world{components};

    Gui() {
        files.files["boxes.ttf"] = bytes_of(
            text::box_font(U"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 "
                           ".,-_!?"));
        files.files["game.theme.json"] = R"({"format": 1, "types": {
            "TooltipLabel": {"fonts": {"font": "boxes.ttf"}, "font_sizes": {"font_size": 20}},
            "LineEdit": {"fonts": {"font": "boxes.ttf"}, "font_sizes": {"font_size": 20}},
            "TextEdit": {"fonts": {"font": "boxes.ttf"}, "font_sizes": {"font_size": 20}}}})";
        world::register_builtin_components(components);
        ui::install(world, assets,
                    {.viewport = {400.0F, 300.0F},
                     .theme = "game.theme.json",
                     .allow_system_fallback = false});
        input::install(world, input::Map::with_defaults(input::Keyboard::Pc), {});
    }

    entt::entity create(std::string_view id, std::string_view components_json) {
        const entt::entity entity = world.create(id).value();
        set(id, components_json);
        return entity;
    }

    // A ColorRect at [x, y, w, h] with more components.
    entt::entity box(std::string_view id, std::array<int, 4> rect, std::string_view control = "",
                     std::string_view more = "") {
        return create(id, "{\"Control\": {\"offset_left\": " + std::to_string(rect[0]) +
                              ", \"offset_top\": " + std::to_string(rect[1]) +
                              ", \"offset_right\": " + std::to_string(rect[0] + rect[2]) +
                              ", \"offset_bottom\": " + std::to_string(rect[1] + rect[3]) +
                              (control.empty() ? "" : ", ") + std::string(control) +
                              "}, \"ColorRect\": {}" + (more.empty() ? "" : ", ") +
                              std::string(more) + "}");
    }

    void set(std::string_view id, std::string_view components_json) {
        const json::Value values = parse(components_json);
        for (const auto& [name, fields] : values.as_object()) {
            std::string path;
            auto assigned = components.find(name)->assign(world, world.find(id), fields, path);
            REQUIRE_MESSAGE(assigned.ok(), assigned.error().message);
        }
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
    std::string tap(std::string_view name) { return send({key(name, true), key(name, false)}); }

    std::string step() {
        world.step(kTick);
        return events();
    }
    // Steps n ticks, returning their UI events.
    std::string steps(int n) {
        std::string out;
        for (int i = 0; i < n; ++i) {
            const std::string tick = step();
            out += (out.empty() || tick.empty() ? "" : ", ") + tick;
        }
        return out;
    }

    // This tick's UI events, with what drags carry.
    [[nodiscard]] std::string events() const {
        std::string out;
        for (const ui::UiEvent& event : ui::events(world)) {
            if (!out.empty()) {
                out += ", ";
            }
            out += event.kind + " " + event.id;
            if (event.kind == "drag_begin") {
                out += " " + compact(event.data);
            } else if (event.kind == "drop_data") {
                out += " " + compact(event.data) + " from " + event.from + " at " +
                       numbers({event.position.x, event.position.y});
            } else if (event.kind == "drag_end") {
                out += event.successful ? " dropped" : " cancelled";
            } else if (event.kind.starts_with("text_")) {
                out += " \"" + event.text + "\"";
            }
        }
        return out;
    }

    text::Fonts& fonts() { return *ui::fonts(world); }
    ui::Layout layout() { return ui::layout(world, fonts(), ui::settings(world).viewport); }

    // A control's rect as [x, y, w, h].
    std::string rect(std::string_view id) {
        const ui::Layout placed = layout();
        const ui::Placed* found = placed.find(world.find(id));
        if (found == nullptr) {
            return "none";
        }
        if (!found->visible) {
            return "hidden";
        }
        const Vec2 origin = found->transform.apply({0.0F, 0.0F});
        return numbers({origin.x, origin.y, found->size.x, found->size.y});
    }

    // The tooltip shown: its control and text, or "none".
    [[nodiscard]] std::string tooltip() const {
        const ui::Tooltip shown = ui::tooltip(world);
        if (shown.owner == entt::null) {
            return "none";
        }
        return std::string(world.id_of(shown.owner)) + ": " + shown.text;
    }
    // The tooltip's rect as [x, y, w, h].
    std::string tooltip_rect() {
        const ui::Layout placed = layout();
        REQUIRE(placed.tooltip.has_value());
        const ui::TooltipView& view = *placed.tooltip;
        return numbers({view.position.x, view.position.y, view.size.x, view.size.y});
    }

    // The control a drag going on would drop on, or "none"; "no drag" with none going on.
    [[nodiscard]] std::string over() const {
        const std::optional<ui::Drag> going = ui::drag(world);
        if (!going) {
            return "no drag";
        }
        return going->over == entt::null ? "none" : std::string(world.id_of(going->over));
    }

    // A text field's text and selection.
    [[nodiscard]] std::string text_of(std::string_view id) const {
        const entt::entity entity = world.find(id);
        if (const auto* edit = world.ecs().try_get<world::LineEdit>(entity)) {
            return edit->text;
        }
        return world.ecs().get<world::TextEdit>(entity).text;
    }
    [[nodiscard]] std::optional<std::pair<int, int>> selection(std::string_view id) const {
        return ui::selection(world, world.find(id));
    }
    // A TextEdit's selection, as [from_line, from_column, to_line, to_column].
    std::optional<std::array<int, 4>> text_selection(std::string_view id) {
        const ui::Layout placed = layout();
        const auto found = placed.text_edits.find(world.find(id));
        REQUIRE(found != placed.text_edits.end());
        return found->second.selection;
    }
};

std::vector<std::string> problem_lines(const std::vector<ui::Problem>& problems) {
    std::vector<std::string> lines;
    lines.reserve(problems.size());
    for (const ui::Problem& problem : problems) {
        lines.push_back(problem.id + problem.path + ": " + problem.message);
    }
    return lines;
}

// An inventory: a sword that drags with a hidden icon as its preview, a slot that takes weapons
// and a bag that takes potions and gold.
void inventory(Gui& gui) {
    gui.box("icon", {0, 0, 16, 16}, R"("visible": false)");
    gui.box("sword", {20, 20, 40, 40}, "",
            R"("DragAndDrop": {"drag_data": {"item": "sword", "kind": "weapon"},
                               "drag_preview": "icon"})");
    gui.box("slot", {200, 20, 40, 40}, "", R"("DragAndDrop": {"can_drop": [{"kind": "weapon"}]})");
    gui.box("bag", {200, 100, 40, 40}, "",
            R"("DragAndDrop": {"can_drop": [{"kind": "potion"}, "gold"]})");
}

// Presses on the sword and moves past the drag threshold, which starts its drag.
std::string drag_sword(Gui& gui) {
    gui.send(move(30, 30));
    gui.send(press(30, 30));
    return gui.send(move(42, 30));
}

} // namespace

TEST_CASE("a tooltip shows once the pointer rests on a control, beside where it came to rest") {
    Gui gui;
    gui.box("play", {20, 20, 100, 30}, R"("tooltip_text": "  Start a new game\n")");
    gui.box("quit", {20, 60, 100, 30});
    CHECK(gui.send(move(30, 30)) == "mouse_entered play");
    gui.steps(kRest - 1);
    CHECK(gui.tooltip() == "none");
    gui.step();
    // Its text without the blanks at its ends.
    CHECK(gui.tooltip() == "play: Start a new game");
    CHECK(ui::tooltip(gui.world).position == Vec2{30.0F, 30.0F});
    // tooltip_position_offset from the pointer, as big as its label (13 letters and 3 spaces)
    // and TooltipPanel's margins of 8 and 2 pixels.
    CHECK(gui.tooltip_rect() == "[40,40,187,24]");
    const ui::TooltipView view = *gui.layout().tooltip;
    CHECK(view.owner == gui.world.find("play"));
    REQUIRE(view.panel.size() == 1);
    CHECK(view.panel[0].position == Vec2{0.0F, 0.0F});
    CHECK(view.panel[0].size == Vec2{187.0F, 24.0F});
    CHECK(view.label.source == "Start a new game");
    CHECK(view.label.pens.at(0).x == 8.0F);

    // Moving over the same tooltip keeps it; another control without one hides it.
    gui.send(move(60, 35));
    CHECK(gui.tooltip() == "play: Start a new game");
    CHECK(gui.send(move(30, 70)) == "mouse_exited play, mouse_entered quit");
    CHECK(gui.tooltip() == "none");
    gui.steps(kRest);
    CHECK(gui.tooltip() == "none");

    // Back on it, the pointer must rest again; moving more than 5 pixels starts the wait over.
    gui.send(move(30, 30));
    gui.steps(kRest - 5);
    gui.send(move(40, 30));
    gui.steps(kRest - 1);
    CHECK(gui.tooltip() == "none");
    gui.steps(1);
    CHECK(gui.tooltip() == "play: Start a new game");
    CHECK(ui::tooltip(gui.world).position == Vec2{40.0F, 30.0F});

    // Hidden, the control takes its tooltip with it.
    gui.set("play", R"({"Control": {"visible": false}})");
    gui.step();
    CHECK(gui.tooltip() == "none");
}

TEST_CASE("a tooltip that would leave the viewport shows on the other side of the pointer") {
    Gui gui;
    gui.box("corner", {300, 250, 100, 50}, R"("tooltip_text": "Corner")");
    gui.send(move(390, 290));
    gui.steps(kRest);
    // Left of and above the pointer by the offset, as Godot flips it.
    CHECK(gui.tooltip_rect() == "[292,256,88,24]");
    // Too wide for either side, it hugs the viewport's right edge.
    gui.set("corner", R"({"Control": {"tooltip_text": "A tooltip far too long to fit"}})");
    gui.send(move(310, 290));
    gui.steps(kRest);
    CHECK(gui.tooltip() == "corner: A tooltip far too long to fit");
    CHECK(gui.tooltip_rect() == "[78,256,322,24]");
}

TEST_CASE("a press or Escape hides a tooltip, and a parent's shows through a child that passes") {
    Gui gui;
    gui.box("panel", {20, 20, 200, 100}, R"("tooltip_text": "The panel")");
    gui.box("pass", {0, 0, 50, 50}, R"("mouse_filter": "pass")");
    gui.box("stop", {100, 0, 50, 50});
    gui.set("panel", R"({"Control": {"children": ["pass", "stop"]}})");
    gui.box("button", {20, 200, 100, 30}, R"("focus_mode": "all")");

    // Over a child that passes the mouse on, the panel's tooltip shows; over one that stops it,
    // none does.
    gui.send(move(30, 30));
    gui.steps(kRest);
    CHECK(gui.tooltip() == "panel: The panel");
    gui.send(move(130, 30));
    CHECK(gui.tooltip() == "none");
    gui.steps(kRest);
    CHECK(gui.tooltip() == "none");

    // A press hides it.
    gui.send(move(30, 30));
    gui.steps(kRest);
    CHECK(gui.tooltip() == "panel: The panel");
    gui.send({press(30, 30), release(30, 30)});
    CHECK(gui.tooltip() == "none");

    // Escape hides it and goes no further, and it doesn't show again until the pointer moves.
    gui.send(move(31, 30));
    gui.steps(kRest);
    CHECK(gui.tooltip() == "panel: The panel");
    gui.tap("Escape");
    CHECK(gui.tooltip() == "none");
    gui.steps(kRest);
    CHECK(gui.tooltip() == "none");
    gui.send(move(40, 40));
    gui.steps(kRest);
    CHECK(gui.tooltip() == "panel: The panel");

    // ui::show_tooltip shows it below a control's middle at once.
    ui::cancel_tooltip(gui.world);
    CHECK(gui.tooltip() == "none");
    ui::show_tooltip(gui.world, gui.world.find("pass"));
    CHECK(gui.tooltip() == "panel: The panel");
    CHECK(ui::tooltip(gui.world).position == Vec2{45.0F, 45.0F});
    // A control without one shows nothing.
    ui::cancel_tooltip(gui.world);
    ui::show_tooltip(gui.world, gui.world.find("button"));
    CHECK(gui.tooltip() == "none");
}

TEST_CASE("an item of an open list shows its tooltip, and the tooltip goes with the list") {
    Gui gui;
    gui.create("size", R"({"Control": {"offset_left": 20, "offset_top": 20},
        "OptionButton": {"selected": 0, "items": [
            {"text": "Small", "tooltip": "For phones"}, {"text": "Large"}]}})");
    gui.send({press(30, 30), release(30, 30)});
    REQUIRE(gui.layout().popups.size() == 1);
    const ui::PopupView list = gui.layout().popups.back();
    const auto item_y = [&](int index) {
        const ui::PopupItemView& item = list.items.at(static_cast<std::size_t>(index));
        return list.position.y + list.list_position.y + item.y + (item.height / 2.0F);
    };
    const float x = list.position.x + 20.0F;
    gui.send(move(x, item_y(0)));
    gui.steps(kRest);
    CHECK(gui.tooltip() == "size: For phones");
    // Over an item without one, it goes.
    gui.send(move(x, item_y(1)));
    CHECK(gui.tooltip() == "none");
    gui.send(move(x, item_y(0)));
    gui.steps(kRest);
    CHECK(gui.tooltip() == "size: For phones");
    // Tooltips draw over the list.
    const ui::DrawList drawn = ui::draw_calls(gui.layout(), gui.world, gui.fonts());
    REQUIRE_FALSE(drawn.spans.empty());
    CHECK(drawn.spans.back().tooltip);
    gui.tap("Escape");
    CHECK(gui.tooltip() == "none");
    gui.tap("Escape");
    CHECK(gui.layout().popups.empty());
}

TEST_CASE("a control is dragged past the threshold and dropped on one that takes what it carries") {
    Gui gui;
    inventory(gui);
    gui.send(move(30, 30));
    gui.send(press(30, 30));
    // Within the threshold of 10 pixels, nothing starts.
    CHECK(gui.send(move(35, 30)).empty());
    CHECK_FALSE(ui::is_dragging(gui.world));
    // Past it, the drag starts with what the press point gives.
    CHECK(gui.send(move(42, 30)) == R"(drag_begin sword {"item":"sword","kind":"weapon"})");
    CHECK(ui::is_dragging(gui.world));
    CHECK(compact(ui::drag_data(gui.world)) == R"({"item":"sword","kind":"weapon"})");
    // The preview shows at the pointer though it is hidden, and the pointer finds what is
    // under it.
    CHECK(gui.rect("icon") == "[42,30,16,16]");
    const ui::Layout placed = gui.layout();
    REQUIRE(placed.drag.has_value());
    CHECK(placed.drag->from == gui.world.find("sword"));
    CHECK(placed.drag->preview == gui.world.find("icon"));
    CHECK(placed.drag->position == Vec2{42.0F, 30.0F});
    CHECK(gui.over() == "none");

    // The bag doesn't take weapons; the slot does.
    CHECK(gui.send(move(210, 110)) == "mouse_exited sword, mouse_entered bag");
    CHECK(gui.over() == "none");
    CHECK(gui.rect("icon") == "[210,110,16,16]");
    gui.send(move(215, 30));
    CHECK(gui.over() == "slot");

    // Let go over it, it drops there, at the point in its rect.
    CHECK(gui.send(release(215, 30)) ==
          R"(drop_data slot {"item":"sword","kind":"weapon"} from sword at [15,10], )"
          "drag_end sword dropped");
    CHECK_FALSE(ui::is_dragging(gui.world));
    CHECK(ui::is_drag_successful(gui.world));
    CHECK(gui.rect("icon") == "hidden");
    CHECK_FALSE(gui.layout().drag.has_value());
}

TEST_CASE("a right click or Escape cancels a drag, and a drop where nothing takes it fails") {
    Gui gui;
    inventory(gui);
    CHECK(drag_sword(gui).starts_with("drag_begin sword"));
    CHECK(gui.send(click(42, 30, true, "right")) == "drag_end sword cancelled");
    CHECK_FALSE(ui::is_dragging(gui.world));
    CHECK_FALSE(ui::is_drag_successful(gui.world));
    gui.send({click(42, 30, false, "right"), release(42, 30)});

    CHECK(drag_sword(gui).starts_with("drag_begin sword"));
    CHECK(gui.tap("Escape") == "drag_end sword cancelled");
    gui.send(release(42, 30));

    CHECK(drag_sword(gui).starts_with("drag_begin sword"));
    gui.send(move(210, 110));
    CHECK(gui.send(release(210, 110)) == "drag_end sword cancelled");

    // The bag takes gold, and anything that has its members.
    gui.set("sword", R"({"DragAndDrop": {"drag_data": "gold"}})");
    drag_sword(gui);
    gui.send(move(210, 110));
    CHECK(gui.send(release(210, 110)) == R"(drop_data bag "gold" from sword at [10,10], )"
                                         "drag_end sword dropped");
    gui.set("sword", R"({"DragAndDrop": {"drag_data": {"kind": "potion", "heals": 5}}})");
    drag_sword(gui);
    gui.send(move(210, 110));
    CHECK(gui.send(release(210, 110)) ==
          R"(drop_data bag {"heals":5,"kind":"potion"} from sword at [10,10], )"
          "drag_end sword dropped");

    // A child that passes the mouse on drops on its parent; one that stops it doesn't.
    gui.box("gem", {5, 5, 10, 10}, R"("mouse_filter": "pass")");
    gui.set("bag", R"({"Control": {"children": ["gem"]}})");
    drag_sword(gui);
    gui.send(move(207, 107));
    CHECK(gui.over() == "bag");
    CHECK(gui.send(release(207, 107)) ==
          R"(drop_data bag {"heals":5,"kind":"potion"} from sword at [7,7], )"
          "drag_end sword dropped");
    gui.set("gem", R"({"Control": {"mouse_filter": "stop"}})");
    drag_sword(gui);
    gui.send(move(207, 107));
    CHECK(gui.over() == "none");
    gui.send(release(207, 107));

    // A control that gives nothing doesn't drag, and its parent is asked instead.
    gui.box("handle", {0, 0, 10, 10}, R"("mouse_filter": "pass")");
    gui.set("sword", R"({"Control": {"children": ["handle"]}})");
    gui.send(move(25, 25));
    gui.send(press(25, 25));
    CHECK(gui.send(move(40, 25)) ==
          R"(mouse_exited handle, drag_begin sword {"heals":5,"kind":"potion"})");
    gui.send(release(40, 25));
    gui.set("sword", R"({"DragAndDrop": {"drag_data": null}})");
    gui.send(move(30, 30));
    gui.send(press(30, 30));
    CHECK(gui.send(move(50, 30)).empty());
    CHECK_FALSE(ui::is_dragging(gui.world));
}

TEST_CASE("drag forwarding, drag previews and force_drag let gameplay code drive drags") {
    Gui gui;
    inventory(gui);
    gui.box("card", {0, 0, 30, 20}, R"("visible": false)");
    std::vector<std::string> calls;
    ui::set_drag_forwarding(gui.world, gui.world.find("sword"),
                            {.get_drag_data =
                                 [&](Vec2 at) {
                                     calls.push_back("get " + numbers({at.x, at.y}));
                                     ui::set_drag_preview(gui.world, gui.world.find("card"));
                                     return json::Value("forwarded");
                                 },
                             .can_drop_data = nullptr,
                             .drop_data = nullptr});
    ui::set_drag_forwarding(gui.world, gui.world.find("slot"),
                            {.get_drag_data = nullptr,
                             .can_drop_data =
                                 [&](Vec2 at, const json::Value& data) {
                                     return at.x < 20.0F && data == json::Value("forwarded");
                                 },
                             .drop_data =
                                 [&](Vec2 at, const json::Value& data) {
                                     calls.push_back("drop " + compact(data) + " at " +
                                                     numbers({at.x, at.y}));
                                 }});
    CHECK(drag_sword(gui) == R"(drag_begin sword "forwarded")");
    CHECK(gui.rect("card") == "[42,30,30,20]");
    CHECK(gui.rect("icon") == "hidden");
    gui.send(move(230, 30));
    CHECK(gui.over() == "none");
    gui.send(move(210, 30));
    CHECK(gui.over() == "slot");
    // The game's drop_data runs before drop_data is emitted.
    CHECK(gui.send(release(210, 30)) ==
          R"(drop_data slot "forwarded" from sword at [10,10], drag_end sword dropped)");
    CHECK(calls == std::vector<std::string>{"get [10,10]", "drop \"forwarded\" at [10,10]"});

    // No calls take the forwarding away.
    ui::set_drag_forwarding(gui.world, gui.world.find("sword"), {});
    CHECK(drag_sword(gui) == R"(drag_begin sword {"item":"sword","kind":"weapon"})");
    gui.send(release(42, 30));

    // force_drag starts a drag without the mouse; it drops where the left button goes down.
    gui.send(move(100, 200));
    ui::force_drag(gui.world, gui.world.find("bag"), json::Value("gold"), gui.world.find("icon"));
    CHECK(ui::is_dragging(gui.world));
    // Its events come with the next tick's.
    CHECK(gui.send(move(220, 120)) == R"(drag_begin bag "gold", mouse_entered bag)");
    CHECK(gui.rect("icon") == "[220,120,16,16]");
    CHECK(gui.send(press(220, 120)) ==
          R"(drop_data bag "gold" from bag at [20,20], drag_end bag dropped)");
}

TEST_CASE("a LineEdit's selection drags into another, moving it, or copying it with Ctrl") {
    Gui gui;
    const auto field = [&](std::string_view id, int y, std::string_view text) {
        gui.create(id, "{\"Control\": {\"offset_left\": 20, \"offset_top\": " + std::to_string(y) +
                           ", \"offset_right\": 380, \"offset_bottom\": " + std::to_string(y + 40) +
                           "}, \"LineEdit\": {\"text\": \"" + std::string(text) + "\"}}");
    };
    field("a", 20, "hello world");
    field("b", 80, "");
    const auto at = [](float x) { return 20.0F + kLeft + x; };
    // "hello " is 5 letters and a space; a double click on "world" selects it.
    const float world_x = at((5.0F * kBox) + 5.0F + (2.0F * kBox) + 1.0F);
    gui.send({press(world_x, 40), release(world_x, 40)});
    gui.send({click(world_x, 40, true, "left", R"("double_click": true)"), release(world_x, 40)});
    REQUIRE(gui.selection("a") == std::pair{6, 11});
    for (int i = 0; i < 40; ++i) {
        gui.step();
    }

    // A press on the selection keeps it, and a drag carries its text, shown at the pointer in
    // the field's font.
    gui.send(press(world_x, 40));
    CHECK(gui.selection("a") == std::pair{6, 11});
    CHECK(gui.send(move(100, 100)) == R"(mouse_exited a, mouse_entered b, drag_begin a "world")");
    const ui::Layout placed = gui.layout();
    REQUIRE(placed.drag.has_value());
    REQUIRE(placed.drag->text.has_value());
    CHECK(placed.drag->text->source == "world");
    CHECK(placed.drag->text->font == "boxes.ttf");
    CHECK(gui.over() == "b");
    // Dropped on the other field, it moves there, selected and focused.
    CHECK(gui.send(release(100, 100)) ==
          R"(drop_data b "world" from a at [80,20], focus_exited a, editing_toggled a, )"
          R"(focus_entered b, editing_toggled b, drag_end a dropped, text_changed a "hello ", )"
          R"(text_changed b "world")");
    CHECK(gui.text_of("a") == "hello ");
    CHECK(gui.text_of("b") == "world");
    CHECK(gui.selection("b") == std::pair{0, 5});
    CHECK(ui::focus_owner(gui.world) == gui.world.find("b"));

    // With Ctrl held, it is copied.
    for (int i = 0; i < 40; ++i) {
        gui.step();
    }
    const float b_x = at(2.0F * kBox);
    gui.send(press(b_x, 100));
    gui.send(move(at(6.0F * kBox), 40));
    CHECK(gui.over() == "a");
    gui.send(key(copy_key(), true));
    gui.send(release(at(6.0F * kBox), 40));
    gui.send(key(copy_key(), false));
    CHECK(gui.text_of("a") == "hello world");
    CHECK(gui.text_of("b") == "world");
    CHECK(gui.selection("a") == std::pair{6, 11});
    CHECK(gui.selection("b") == std::nullopt);

    // Dropped on its own selection, nothing changes; within the field, it moves.
    for (int i = 0; i < 40; ++i) {
        gui.step();
    }
    gui.send(press(world_x, 40));
    gui.send(move(world_x + 20.0F, 40));
    CHECK(gui.over() == "a");
    gui.send(release(world_x + 20.0F, 40));
    CHECK(gui.text_of("a") == "hello world");
    CHECK(gui.selection("a") == std::pair{6, 11});
    for (int i = 0; i < 40; ++i) {
        gui.step();
    }
    gui.send(press(world_x, 40));
    gui.send(move(at(0.0F), 40));
    gui.send(release(at(0.0F), 40));
    CHECK(gui.text_of("a") == "worldhello ");
    CHECK(gui.selection("a") == std::pair{0, 5});

    // With drag_and_drop_selection_enabled off, a press on the selection selects anew.
    gui.set("a", R"({"LineEdit": {"drag_and_drop_selection_enabled": false}})");
    for (int i = 0; i < 40; ++i) {
        gui.step();
    }
    gui.send(press(at(kBox + 2.0F), 40));
    CHECK(gui.send(move(at(4.0F * kBox), 40)).empty());
    gui.send(release(at(4.0F * kBox), 40));
    CHECK(gui.text_of("a") == "worldhello ");
    CHECK(gui.selection("a") == std::pair{1, 4});
}

TEST_CASE("a TextEdit's selection drags within it, onto a LineEdit, and back") {
    Gui gui;
    gui.create("notes", R"({"Control": {"offset_left": 20, "offset_top": 20, "offset_right": 380,
        "offset_bottom": 140}, "TextEdit": {"text": "one two\nthree"}})");
    gui.create("line", R"({"Control": {"offset_left": 20, "offset_top": 200, "offset_right": 380,
        "offset_bottom": 240}, "LineEdit": {}})");
    const auto x_of = [](int column) {
        return 20.0F + kLeft + (static_cast<float>(column) * kBox) + 2.0F;
    };
    const auto y_of = [](int line) {
        return 20.0F + kLeft + (static_cast<float>(line) * kRow) + (kRow / 2.0F);
    };
    // "one " is 3 letters and a space, so "two" starts 41 pixels in; a double click selects it.
    const float two_x = 20.0F + kLeft + (3.0F * kBox) + 5.0F + kBox + 2.0F;
    gui.send({press(two_x, y_of(0)), release(two_x, y_of(0))});
    gui.send(
        {click(two_x, y_of(0), true, "left", R"("double_click": true)"), release(two_x, y_of(0))});
    REQUIRE(gui.text_selection("notes") == std::array{0, 4, 0, 7});
    for (int i = 0; i < 40; ++i) {
        gui.step();
    }

    // A press and release on the selection without a drag puts the caret there.
    gui.send({press(two_x, y_of(0)), release(two_x, y_of(0))});
    CHECK(gui.text_selection("notes") == std::nullopt);
    gui.send(
        {click(two_x, y_of(0), true, "left", R"("double_click": true)"), release(two_x, y_of(0))});
    for (int i = 0; i < 40; ++i) {
        gui.step();
    }

    // Dragged to the start of the second line, it moves there, selected; a drag caret shows
    // where it would go.
    gui.send(press(two_x, y_of(0)));
    CHECK(gui.send(move(x_of(0), y_of(1))) == R"(drag_begin notes "two")");
    CHECK(gui.over() == "notes");
    gui.send(move(x_of(0) - 2.0F, y_of(1)));
    gui.send(release(x_of(0) - 2.0F, y_of(1)));
    CHECK(gui.text_of("notes") == "one \ntwothree");
    CHECK(gui.text_selection("notes") == std::array{1, 0, 1, 3});

    // Onto the LineEdit, it leaves the TextEdit.
    for (int i = 0; i < 40; ++i) {
        gui.step();
    }
    gui.send(press(x_of(1), y_of(1)));
    gui.send(move(100, 220));
    CHECK(gui.over() == "line");
    CHECK(gui.send(release(100, 220)) ==
          R"(drop_data line "two" from notes at [80,20], focus_exited notes, focus_entered line, )"
          R"(editing_toggled line, lines_edited_from notes, drag_end notes dropped, )"
          R"(text_changed line "two", text_changed notes "", caret_changed notes)");
    CHECK(gui.text_of("notes") == "one \nthree");
    CHECK(gui.text_of("line") == "two");
}

TEST_CASE("check finds tooltips that can't show and drag previews that can't be shown") {
    Gui gui;
    gui.create("title", R"({"Control": {"tooltip_text": "Never shown"}, "Label": {"text": "T"}})");
    gui.create("ok", R"({"Control": {"tooltip_text": "Shown", "mouse_filter": "pass"},
        "Label": {"text": "T"}})");
    gui.create("thing", R"({"DragAndDrop": {"drag_data": 1}})");
    gui.box("child", {0, 0, 10, 10});
    gui.box("parent", {0, 0, 10, 10}, R"("children": ["child"])");
    gui.box("self", {0, 0, 10, 10});
    gui.set("self", R"({"DragAndDrop": {"drag_data": 1, "drag_preview": "self"}})");
    gui.box("nested", {0, 0, 10, 10}, "", R"("DragAndDrop": {"drag_preview": "child"})");
    gui.box("odd", {0, 0, 10, 10}, "", R"("DragAndDrop": {"drag_preview": "thing"})");
    CHECK(problem_lines(ui::check(gui.world, gui.fonts())) ==
          std::vector<std::string>{
              "nested/components/DragAndDrop/drag_preview: \"child\" is a child of \"parent\"; a "
              "drag preview must have no parent",
              "odd/components/DragAndDrop/drag_preview: \"thing\" isn't a control",
              "self/components/DragAndDrop/drag_preview: a control can't be its own drag preview",
              "title/components/Control/tooltip_text: the tooltip won't show: the pointer passes "
              "this control by; set mouse_filter to \"stop\" or \"pass\"",
              "thing/components/DragAndDrop: DragAndDrop works on a control; give it a Control"});
}

namespace {

struct Session {
    world::ComponentRegistry components;
    command::Registry commands;
    std::unique_ptr<command::Session> session;

    Session() {
        world::register_builtin_components(components);
        command::register_builtin_commands(commands);
        session =
            std::make_unique<command::Session>(components, commands, command::SessionOptions{});
    }

    command::Reply call(std::string_view name, std::string_view params = "{}") {
        return session->call(name, parse(params));
    }

    json::Value ok(std::string_view name, std::string_view params = "{}") {
        command::Reply reply = call(name, params);
        REQUIRE_MESSAGE(reply.ok, reply.message);
        return std::move(reply.value);
    }

    std::string fails(std::string_view name, std::string_view params) {
        const command::Reply reply = call(name, params);
        REQUIRE_FALSE(reply.ok);
        return reply.message;
    }
};

} // namespace

TEST_CASE("ui.drag, ui.tooltip and ui.layout drag controls, show tooltips and report them") {
    Session s;
    s.ok("batch", R"({"commands": [
        {"command": "entity.create", "params": {"id": "sword", "components": {
            "Control": {"offset_left": 20, "offset_top": 20, "offset_right": 60,
                        "offset_bottom": 60, "tooltip_text": "A sword"},
            "ColorRect": {},
            "DragAndDrop": {"drag_data": {"item": "sword"}}}}},
        {"command": "entity.create", "params": {"id": "slot", "components": {
            "Control": {"offset_left": 200, "offset_top": 20, "offset_right": 240,
                        "offset_bottom": 60},
            "ColorRect": {},
            "DragAndDrop": {"can_drop": [{"item": "sword"}]}}}},
        {"command": "entity.create", "params": {"id": "bag", "components": {
            "Control": {"offset_left": 200, "offset_top": 100, "offset_right": 240,
                        "offset_bottom": 140},
            "ColorRect": {}}}}]})");
    s.ok("events.subscribe", R"({"kinds": ["drag_begin", "drop_data", "drag_end"]})");
    CHECK(s.ok("ui.drag", R"({"from": "sword", "to": "slot"})") == parse(R"({"tick": 0})"));
    s.ok("step");
    CHECK(*s.ok("events.read", R"({"subscription": "events_1"})").find("events") == parse(R"([
        {"seq": 1, "tick": 0, "kind": "drag_begin", "id": "sword",
         "data": {"data": {"item": "sword"}}},
        {"seq": 2, "tick": 0, "kind": "drop_data", "id": "slot",
         "data": {"data": {"item": "sword"}, "from": "sword", "position": [20, 20]}},
        {"seq": 3, "tick": 0, "kind": "drag_end", "id": "sword", "data": {"successful": true}}
    ])"));

    // ui.tooltip shows one below a control's middle; ui.layout and frame.summary report it.
    CHECK(s.ok("ui.tooltip", R"({"id": "sword"})") ==
          parse(R"({"tooltip": {"id": "sword", "text": "A sword"}})"));
    const json::Value layout = s.ok("ui.layout", R"({"ids": ["sword"]})");
    REQUIRE(layout.find("tooltip") != nullptr);
    CHECK(*layout.find("tooltip")->find("id") == json::Value("sword"));
    CHECK(*layout.find("tooltip")->find("text") == json::Value("A sword"));
    const json::Value rect = *layout.find("tooltip")->find("rect");
    CHECK(rect.as_array().at(0) == json::Value(50));
    CHECK(rect.as_array().at(1) == json::Value(50));
    const json::Value summary = s.ok("frame.summary", R"({"ids": ["sword"]})");
    const json::Value& drawn = *summary.find("drawn");
    REQUIRE(drawn.as_array().size() == 2);
    CHECK(*drawn.as_array().at(1).find("kind") == json::Value("Tooltip"));
    CHECK(*drawn.as_array().at(1).find("text") == json::Value("A sword"));
    CHECK(s.ok("ui.tooltip", R"({"show": false})") == parse(R"({"tooltip": null})"));
    CHECK(s.ok("ui.tooltip", R"({"id": "bag"})") == parse(R"({"tooltip": null})"));

    CHECK(s.fails("ui.drag", R"({"from": "swrd", "to": "slot"})") ==
          "/from: no entity has the id \"swrd\"; did you mean \"sword\"?");
    s.ok("entity.create", R"({"id": "plain"})");
    CHECK(s.fails("ui.drag", R"({"from": "sword", "to": "plain"})") ==
          "/to: \"plain\" isn't a control: it has no Control or UI component");
    CHECK(s.fails("ui.tooltip", "{}") == "/id: no entity has the id \"\"");
}

TEST_CASE("project.json's gui object sets the tooltip delay, its offset and the drag threshold") {
    ui::Settings settings;
    ui::read_settings(parse(R"({"tooltip_delay_sec": 1.5, "tooltip_position_offset": [4, 6],
        "drag_threshold": 3, "theme": 7})"),
                      settings);
    CHECK(settings.tooltip_delay_sec == 1.5);
    CHECK(settings.tooltip_position_offset == Vec2{4.0F, 6.0F});
    CHECK(settings.drag_threshold == 3);
    // Of the wrong type, it keeps its value.
    CHECK(settings.theme.empty());

    Gui gui;
    ui::Settings slow = ui::settings(gui.world);
    slow.tooltip_delay_sec = 1.0;
    slow.drag_threshold = 30;
    ui::set_settings(gui.world, slow);
    inventory(gui);
    gui.set("sword", R"({"Control": {"tooltip_text": "Sword"}})");
    gui.send(move(30, 30));
    gui.steps(kRest);
    CHECK(gui.tooltip() == "none");
    gui.steps(kRest);
    CHECK(gui.tooltip() == "sword: Sword");
    CHECK(drag_sword(gui).empty());
    CHECK(gui.send(move(70, 30)) ==
          R"(mouse_exited sword, drag_begin sword {"item":"sword","kind":"weapon"})");
}
