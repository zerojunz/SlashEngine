#include "slash/asset/assets.hpp"
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/input/input.hpp"
#include "slash/render/image.hpp"
#include "slash/text/text.hpp"
#include "slash/ui/ui.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Check boxes, check buttons and sliders, as Godot's CheckBox, CheckButton and Slider
// (scene/gui/check_box.cpp, check_button.cpp, slider.cpp and range.cpp), and the theme icons
// they draw. The sizes and events expected here follow those files and Godot's default theme.

namespace asset = slash::asset;
namespace command = slash::command;
namespace input = slash::input;
namespace json = slash::json;
namespace render = slash::render;
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

// A PNG of one size, white.
std::string png(std::uint32_t width, std::uint32_t height) {
    render::Image image{.width = width, .height = height, .pixels = {}};
    image.pixels.assign(std::size_t{width} * height * 4, 255);
    auto encoded = render::encode_png(image);
    REQUIRE(encoded.ok());
    return encoded.value();
}

// Mouse events at a point, as input.inject takes them.
std::string motion(int x, int y) {
    return R"({"type": "mouse_motion", "position": [)" + std::to_string(x) + ", " +
           std::to_string(y) + "]}";
}
std::string click(int x, int y, bool pressed, std::string_view button = "left") {
    return R"({"type": "mouse_button", "button": ")" + std::string(button) + R"(", "position": [)" +
           std::to_string(x) + ", " + std::to_string(y) +
           "], \"pressed\": " + (pressed ? "true" : "false") + "}";
}
std::string key(std::string_view name, bool pressed) {
    return R"({"type": "key", "key": ")" + std::string(name) +
           "\", \"pressed\": " + (pressed ? "true" : "false") + "}";
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

    // A control at [x, y] with no size of its own, so it takes its minimum size.
    entt::entity at(std::string_view id, int x, int y, std::string_view type,
                    std::string_view fields = "", std::string_view control = "") {
        std::string components_json = R"({"Control": {"offset_left": )" + std::to_string(x) +
                                      R"(, "offset_top": )" + std::to_string(y) +
                                      R"(, "offset_right": )" + std::to_string(x) +
                                      R"(, "offset_bottom": )" + std::to_string(y);
        if (!control.empty()) {
            components_json += ", " + std::string(control);
        }
        components_json += "}, \"" + std::string(type) + "\": {" + std::string(fields) + "}}";
        return create(id, components_json);
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

    // This tick's UI events, with a slider's value and whether a drag changed it.
    [[nodiscard]] std::string events() const {
        std::string out;
        for (const ui::UiEvent& event : ui::events(world)) {
            if (!out.empty()) {
                out += ", ";
            }
            out += event.kind + " " + event.id;
            if (event.kind == "toggled") {
                out += event.toggled_on ? " on" : " off";
            } else if (event.kind == "value_changed") {
                out += " " + number(event.value);
            } else if (event.kind == "drag_ended") {
                out += event.value_changed ? " changed" : " unchanged";
            }
        }
        return out;
    }

    text::Fonts& fonts() { return *ui::fonts(world); }
    ui::Layout layout() { return ui::layout(world, fonts(), ui::settings(world).viewport); }

    [[nodiscard]] double value(std::string_view id) const {
        return world.ecs().get<world::Slider>(world.find(id)).value;
    }

    // A control's rect as [x, y, w, h].
    std::string rect(std::string_view id) {
        const ui::Layout placed = layout();
        for (const ui::Placed& each : placed.controls) {
            if (each.entity == world.find(id)) {
                const Vec2 origin = each.transform.apply({0.0F, 0.0F});
                return numbers({origin.x, origin.y, each.size.x, each.size.y});
            }
        }
        return "none";
    }

    // The textures a control draws, each with its rect in the control: "path [x, y, w, h]",
    // flipped ones marked; flat style boxes as "flat [x, y, w, h]".
    std::string boxes(std::string_view id) {
        const ui::Layout placed = layout();
        const auto found = placed.boxes.find(world.find(id));
        std::string out;
        if (found == placed.boxes.end()) {
            return out;
        }
        for (const ui::Box& box : found->second) {
            if (box.style.type == ui::StyleBox::Type::Empty) {
                continue;
            }
            if (!out.empty()) {
                out += ", ";
            }
            out += box.style.type == ui::StyleBox::Type::Texture ? box.style.texture : "flat";
            out += " " + numbers({box.position.x, box.position.y, box.size.x, box.size.y});
            if (box.style.flip_h) {
                out += " flip_h";
            }
            if (box.style.flip_v) {
                out += " flip_v";
            }
        }
        return out;
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

TEST_CASE("CheckBox and CheckButton are toggle buttons with Button's fields") {
    world::ComponentRegistry components;
    world::register_builtin_components(components);
    for (const std::string_view name : {"CheckBox", "CheckButton"}) {
        const world::ComponentType* type = components.find(name);
        REQUIRE(type != nullptr);
        // Godot's CheckBox and CheckButton set toggle_mode and align their text left.
        CHECK(compact(*type->defaults().find("toggle_mode")) == "true");
        CHECK(compact(*type->defaults().find("alignment")) == "\"left\"");
        CHECK(type->defaults().size() == components.find("Button")->defaults().size());
    }
    // Saved, a check box leaves out what its own defaults say.
    world::World world{components};
    const entt::entity box = world.create("box").value();
    std::string path;
    REQUIRE(components.find("CheckBox")
                ->assign(world, box, parse(R"({"text": "Sound", "button_pressed": true})"), path)
                .ok());
    CHECK(compact(components.find("CheckBox")->save(world, box)) ==
          R"({"button_pressed":true,"text":"Sound"})");
    CHECK(ui::control_class(world, box) == "CheckBox");
}

TEST_CASE("a check box draws its box before its text, and a click toggles it") {
    Gui gui;
    gui.at("label", 0, 100, "Label", R"("text": "Sound")");
    gui.at("sound", 0, 0, "CheckBox", R"("text": "Sound")");
    const json::Value label = parse(gui.rect("label"));
    const float text_width = static_cast<float>(label.as_array()[2].as_double());
    const float text_height = static_cast<float>(label.as_array()[3].as_double());
    // Margins of 4, the 16-pixel box, 4 between it and the text.
    const float width = 4 + 16 + 4 + text_width + 4;
    const float height = std::max(text_height, 16.0F) + 8;
    CHECK(gui.rect("sound") == numbers({0, 0, width, height}));
    const auto y = static_cast<float>(static_cast<int>((height - 16) / 2));
    CHECK(gui.boxes("sound") == "builtin:unchecked " + numbers({4, y, 16, 16}));

    CHECK(gui.send({click(10, 10, true), click(10, 10, false)}) ==
          "mouse_entered sound, focus_entered sound, button_down sound, toggled sound on, "
          "pressed sound, button_up sound");
    CHECK(gui.world.ecs().get<world::CheckBox>(gui.world.find("sound")).button_pressed);
    CHECK(gui.boxes("sound") == "builtin:checked " + numbers({4, y, 16, 16}));
    CHECK(ui::draw_mode_name(ui::draw_mode(gui.world, gui.world.find("sound"))) == "hover_pressed");

    // Disabled, it draws the disabled box; right to left, the box ends the button.
    gui.set("sound", R"({"CheckBox": {"disabled": true}, "Control": {"layout_direction": "rtl"}})");
    CHECK(gui.boxes("sound") == "builtin:checked_disabled " + numbers({width - 4 - 16, y, 16, 16}));
}

TEST_CASE("check boxes in a button group draw as radio buttons, and one stays on") {
    Gui gui;
    gui.at("low", 0, 0, "CheckBox", R"("text": "Low", "button_group": "quality")");
    gui.at("high", 0, 50, "CheckBox",
           R"("text": "High", "button_group": "quality", "button_pressed": true)");
    CHECK(gui.boxes("low").starts_with("builtin:radio_unchecked "));
    CHECK(gui.boxes("high").starts_with("builtin:radio_checked "));
    CHECK(gui.send({click(5, 10, true), click(5, 10, false)}) ==
          "mouse_entered low, focus_entered low, button_down low, toggled high off, toggled low "
          "on, pressed low, button_up low");
    // Pressed again, the one that is on stays on.
    CHECK(gui.send({click(5, 10, true), click(5, 10, false)}) ==
          "button_down low, toggled low on, pressed low, button_up low");
    CHECK(gui.boxes("low").starts_with("builtin:radio_checked "));
    CHECK(gui.boxes("high").starts_with("builtin:radio_unchecked "));
}

TEST_CASE("a check button draws its switch after its text, mirrored right to left") {
    Gui gui;
    gui.at("label", 0, 100, "Label", R"("text": "V-Sync")");
    gui.at("vsync", 0, 0, "CheckButton", R"("text": "V-Sync", "button_pressed": true)");
    const json::Value label = parse(gui.rect("label"));
    const float text_width = static_cast<float>(label.as_array()[2].as_double());
    const float text_height = static_cast<float>(label.as_array()[3].as_double());
    // Margins of 6 and 4, the 32 x 16 switch, 4 between it and the text.
    const float width = 6 + text_width + 4 + 32 + 6;
    const float height = std::max(text_height, 16.0F) + 8;
    CHECK(gui.rect("vsync") == numbers({0, 0, width, height}));
    // CheckButton doesn't round the switch's y down, as CheckBox does.
    const float y = (height - 16) / 2;
    CHECK(gui.boxes("vsync") == "builtin:toggle_on " + numbers({width - 6 - 32, y, 32, 16}));
    gui.set("vsync", R"({"CheckButton": {"button_pressed": false}})");
    CHECK(gui.boxes("vsync").starts_with("builtin:toggle_off "));
    gui.set("vsync", R"({"Control": {"layout_direction": "rtl"}})");
    CHECK(gui.boxes("vsync") == "builtin:toggle_off_mirrored " + numbers({6, y, 32, 16}));
}

TEST_CASE("theme icons come from overrides, then themes, then the built-in ones") {
    Gui gui;
    gui.files.files["ui/box.png"] = png(20, 12);
    gui.files.files["ui/dot.png"] = png(10, 10);
    gui.files.files["ui/theme.theme.json"] =
        R"({"format": 1, "types": {"CheckBox": {"icons": {"unchecked": "ui/dot.png"}}}})";
    gui.at("a", 0, 0, "CheckBox", R"("text": "A")", R"("theme": "ui/theme.theme.json")");
    gui.at("b", 0, 60, "CheckBox", R"("text": "B")",
           R"("theme_override_icons": {"unchecked": "ui/box.png"})");
    gui.at("c", 0, 120, "CheckBox", R"("text": "C")",
           R"("theme_override_icons": {"unchecked": "", "checked": "", "radio_checked": "",
               "radio_unchecked": "", "checked_disabled": "", "unchecked_disabled": "",
               "radio_checked_disabled": "", "radio_unchecked_disabled": ""})");
    CHECK(gui.boxes("a").starts_with("ui/dot.png "));
    // The icon size is the largest of a check box's icons: 20 wide, 16 high.
    CHECK(gui.boxes("b").starts_with("ui/box.png [4,"));
    CHECK(gui.boxes("b").ends_with(",20,12]"));
    // With no icons, it is a button with its text at the left.
    CHECK(gui.boxes("c").empty());
    std::map<std::string, std::string> icons;
    for (const ui::ThemeItem& item : ui::theme_items(gui.world, gui.fonts(), gui.world.find("b"))) {
        if (item.kind == "icons") {
            icons[item.name] = compact(item.value) + " from " + item.from;
        }
    }
    CHECK(icons["unchecked"] == R"("ui/box.png" from override)");
    CHECK(icons["checked"] == R"("builtin:checked" from default)");
    CHECK(icons.size() == 8);

    // Mistakes: a name the type doesn't have, a built-in icon that doesn't exist, a file that
    // isn't there.
    gui.at("d", 0, 180, "CheckBox", "",
           R"("theme_override_icons": {"chekced": "builtin:checked", "checked": "builtin:chekced",
               "unchecked": "ui/missing.png"})");
    const std::vector<std::string> lines = problem_lines(ui::check(gui.world, gui.fonts()));
    CHECK(lines == std::vector<std::string>{
                       "d/components/Control/theme_override_icons/chekced: CheckBox has no icons "
                       "item \"chekced\"; did you mean \"checked\"?",
                       "d/components/Control/theme_override_icons/checked: \"chekced\" isn't a "
                       "built-in icon; did you mean \"checked\"?",
                       "d/components/Control/theme_override_icons/unchecked: asset "
                       "\"ui/missing.png\" could not be read: no such file",
                   });
}

TEST_CASE("the built-in icons are drawn in code, the same on every machine") {
    const std::vector<std::string_view>& names = ui::builtin_icon_names();
    CHECK(names.size() == 25);
    for (const std::string_view name : names) {
        const render::Image* image =
            ui::builtin_icon(std::string(ui::kBuiltinPrefix) + std::string(name));
        REQUIRE(image != nullptr);
        CHECK(image->pixels.size() == std::size_t{image->width} * image->height * 4);
    }
    CHECK(ui::builtin_icon("checked") == nullptr);
    CHECK(ui::builtin_icon("builtin:nothing") == nullptr);
    const render::Image& checked = *ui::builtin_icon("builtin:checked");
    CHECK(checked.width == 16);
    CHECK(checked.height == 16);
    const auto pixel = [&](const render::Image& image, int x, int y) {
        const std::size_t at =
            ((static_cast<std::size_t>(y) * image.width) + static_cast<std::size_t>(x)) * 4;
        return compact(json::Array{image.pixels[at], image.pixels[at + 1], image.pixels[at + 2],
                                   image.pixels[at + 3]});
    };
    // A corner outside the rounded box is clear, the box is white at 0.75 and the check mark
    // dark.
    CHECK(pixel(checked, 0, 0) == "[0,0,0,0]");
    CHECK(pixel(checked, 3, 3) == "[255,255,255,191]");
    CHECK(pixel(checked, 6, 10) == "[26,26,26,255]");
    const render::Image& toggle = *ui::builtin_icon("builtin:toggle_on");
    CHECK(toggle.width == 32);
    CHECK(toggle.height == 16);
    // The knob is at the end that is on.
    CHECK(pixel(toggle, 24, 8) == "[26,26,26,255]");
    CHECK(pixel(*ui::builtin_icon("builtin:toggle_on_mirrored"), 8, 8) == "[26,26,26,255]");
    // CodeEdit's: a red dot at half opacity, a bookmark notched at the bottom and an arrow.
    const render::Image& breakpoint = *ui::builtin_icon("builtin:breakpoint");
    CHECK(pixel(breakpoint, 8, 8) == "[255,93,93,128]");
    CHECK(pixel(breakpoint, 0, 0) == "[0,0,0,0]");
    const render::Image& bookmark = *ui::builtin_icon("builtin:bookmark");
    CHECK(pixel(bookmark, 8, 4) == "[254,255,254,249]");
    CHECK(pixel(bookmark, 8, 13) == "[0,0,0,0]");
    const render::Image& arrow = *ui::builtin_icon("builtin:arrow_right");
    CHECK(pixel(arrow, 7, 8) == "[178,178,178,97]");
    CHECK(pixel(arrow, 3, 8) == "[0,0,0,0]");
}

TEST_CASE("a slider places its grabber by its value, with ticks under it") {
    Gui gui;
    gui.create("volume", R"({"Control": {"offset_right": 216, "offset_bottom": 16},
        "Slider": {"value": 25, "tick_count": 3, "ticks_on_borders": true}})");
    // The grabber moves over the width less its own: 200 pixels, a quarter of the way.
    CHECK(gui.boxes("volume") == "flat [0,4,216,8], flat [0,4,58,8], builtin:hslider_tick "
                                 "[6,12,4,8], builtin:hslider_tick [106,12,4,8], "
                                 "builtin:hslider_tick [206,12,4,8], builtin:slider_grabber "
                                 "[50,0,16,16]");
    // Right to left, it fills from the right.
    gui.set("volume", R"({"Control": {"layout_direction": "rtl"}})");
    CHECK(gui.boxes("volume").ends_with("builtin:slider_grabber [150,0,16,16]"));

    // Vertical, the minimum is at the bottom; ticks on both sides flip on the left.
    gui.create("level", R"({"Control": {"offset_left": 300, "offset_right": 316,
        "offset_bottom": 116}, "Slider": {"vertical": true, "max_value": 1, "step": 0.1,
        "value": 0.3, "tick_count": 2, "ticks_on_borders": true, "ticks_position": "both"}})");
    CHECK(gui.boxes("level") == "flat [4,0,8,116], flat [4,78,8,38], builtin:vslider_tick "
                                "[12,6,8,4], builtin:vslider_tick [-4,6,8,4] flip_h, "
                                "builtin:vslider_tick [12,106,8,4], builtin:vslider_tick "
                                "[-4,106,8,4] flip_h, builtin:slider_grabber [0,70,16,16]");

    // Its minimum size is the grabber's height and the track's width.
    gui.at("small", 0, 200, "Slider");
    CHECK(gui.rect("small") == "[0,200,8,16]");
    const ui::Layout placed = gui.layout();
    const ui::SliderState& state = placed.sliders.at(gui.world.find("volume"));
    CHECK(state.value == 25.0);
    CHECK(state.ratio == 0.25);
}

TEST_CASE("a slider's value snaps to its step and stays in range, as Godot's Range") {
    Gui gui;
    gui.create("s", R"({"Slider": {"min_value": 10, "max_value": 20, "step": 0.5}})");
    const auto set = [&](std::string_view fields) {
        gui.set("s", R"({"Slider": )" + std::string(fields) + "}");
        return number(
            ui::layout(gui.world, gui.fonts(), {400, 300}).sliders.at(gui.world.find("s")).value);
    };
    CHECK(set(R"({"value": 12.3})") == "12.5");
    CHECK(set(R"({"value": 25})") == "20");
    CHECK(set(R"({"value": -5})") == "10");
    CHECK(set(R"({"allow_greater": true, "value": 25})") == "25");
    CHECK(set(R"({"allow_lesser": true, "value": 3})") == "3");
    CHECK(set(R"({"step": 0, "value": 12.34})") == "12.34");
    CHECK(set(R"({"rounded": true, "value": 12.5})") == "13");
    // A page makes the largest value less by the page, as a scroll bar's.
    CHECK(set(R"({"rounded": false, "allow_greater": false, "page": 4, "value": 19})") == "16");
    // Steps of 0.1 come out clean.
    CHECK(set(R"({"page": 0, "min_value": 0, "max_value": 1, "step": 0.1, "value": 0.7})") ==
          "0.7");
}

TEST_CASE("a click moves a slider's grabber there, and dragging moves it with the pointer") {
    Gui gui;
    gui.create("volume", R"({"Control": {"offset_left": 100, "offset_top": 100,
        "offset_right": 316, "offset_bottom": 116}, "Slider": {"value": 50}})");
    // A click 58 pixels in puts the grabber's middle there: (58 - 8) / 200 is a quarter.
    CHECK(gui.send({motion(158, 108), click(158, 108, true)}) ==
          "mouse_entered volume, focus_entered volume, drag_started volume, value_changed volume "
          "25");
    CHECK(gui.value("volume") == 25.0);
    CHECK(gui.layout().sliders.at(gui.world.find("volume")).highlighted);
    // Dragging 100 pixels right is half the way.
    CHECK(gui.send(motion(258, 108)) == "value_changed volume 75");
    // Past the end it stops at the maximum, and the pointer leaves it while it drags.
    CHECK(gui.send(motion(400, 108)) == "mouse_exited volume, value_changed volume 100");
    CHECK(gui.send(motion(400, 108)).empty());
    CHECK(gui.send(click(400, 108, false)) == "drag_ended volume changed");
    // A click that leaves it where it was still says so, and the drag ends unchanged.
    CHECK(gui.send({motion(308, 108), click(308, 108, true)}) ==
          "mouse_entered volume, drag_started volume, value_changed volume 100");
    CHECK(gui.send(click(308, 108, false)) == "drag_ended volume unchanged");
    // The wheel steps it.
    CHECK(gui.send(click(308, 108, true, "wheel_down")) == "value_changed volume 99");
    CHECK(gui.send(click(308, 108, true, "wheel_up")) == "value_changed volume 100");
    // Not editable, it ignores the pointer.
    gui.set("volume", R"({"Slider": {"editable": false}})");
    CHECK(gui.send({click(158, 108, true), click(158, 108, false)}).empty());
    CHECK(gui.boxes("volume").ends_with("builtin:slider_grabber_disabled [200,0,16,16]"));
}

TEST_CASE("arrow keys step a slider with focus, and Home and End go to the ends") {
    Gui gui;
    gui.create("volume", R"({"Control": {"offset_right": 216, "offset_bottom": 16},
        "Slider": {"value": 50, "step": 10}})");
    gui.create("level", R"({"Control": {"offset_left": 300, "offset_right": 316,
        "offset_bottom": 116}, "Slider": {"vertical": true, "value": 50, "step": 10}})");
    REQUIRE(ui::grab_focus(gui.world, gui.world.find("volume")));
    gui.step();
    CHECK(gui.tap("right") == "value_changed volume 60");
    CHECK(gui.tap("left") == "value_changed volume 50");
    CHECK(gui.tap("end") == "value_changed volume 100");
    CHECK(gui.tap("right").empty());
    CHECK(gui.tap("home") == "value_changed volume 0");
    // Up and down aren't for a horizontal slider: they move focus.
    CHECK(gui.tap("down").empty());
    CHECK(ui::grab_focus(gui.world, gui.world.find("level")));
    gui.step();
    CHECK(gui.tap("up") == "value_changed level 60");
    CHECK(gui.tap("down") == "value_changed level 50");
    // Right to left, left steps up.
    gui.set("volume", R"({"Control": {"layout_direction": "rtl"}})");
    REQUIRE(ui::grab_focus(gui.world, gui.world.find("volume")));
    gui.step();
    CHECK(gui.tap("left") == "value_changed volume 10");
}

TEST_CASE("a gamepad held on a slider steps it again after half a second, 20 times a second") {
    Gui gui;
    gui.create("volume", R"({"Control": {"offset_right": 216, "offset_bottom": 16},
        "Slider": {"value": 50}})");
    REQUIRE(ui::grab_focus(gui.world, gui.world.find("volume")));
    gui.step();
    CHECK(gui.send(R"({"type": "joypad_button", "button": "dpad_right", "pressed": true})") ==
          "value_changed volume 51");
    // Half a second waits, then it steps every 3 ticks: 10 more steps by 59 ticks.
    int steps = 0;
    for (int tick = 0; tick < 59; ++tick) {
        steps += gui.step().empty() ? 0 : 1;
    }
    CHECK(steps == 10);
    CHECK(
        gui.send(R"({"type": "joypad_button", "button": "dpad_right", "pressed": false})").empty());
    CHECK(gui.step().empty());
    CHECK(gui.value("volume") == 61.0);
}

TEST_CASE("a value changed from outside the gui emits value_changed once") {
    Gui gui;
    gui.create("volume", R"({"Slider": {"value": 50, "step": 5}})");
    CHECK(gui.step().empty());
    gui.world.ecs().get<world::Slider>(gui.world.find("volume")).value = 72;
    // Snapped to its step, as Range::set_value would.
    CHECK(gui.step() == "value_changed volume 70");
    CHECK(gui.value("volume") == 70.0);
    CHECK(gui.step().empty());
}

TEST_CASE("ui.press presses check boxes, and ui.layout and frame.summary report sliders") {
    const Session session;
    session.ok("batch", R"({"commands": [
        {"command": "entity.create", "params": {"id": "sound", "components": {
            "Control": {"offset_right": 100, "offset_bottom": 30},
            "CheckBox": {"text": "Sound"}}}},
        {"command": "entity.create", "params": {"id": "volume", "components": {
            "Control": {"offset_top": 40, "offset_right": 216, "offset_bottom": 56},
            "Slider": {"value": 30}}}}]})");
    CHECK(compact(session.ok("ui.press", R"({"id": "sound"})")) == R"({"tick":0})");
    session.ok("step", R"({"ticks": 1})");
    CHECK(
        compact(session.ok("ui.layout", R"({"ids": ["sound", "volume"]})")) ==
        R"({"controls":[{"draw_mode":"pressed","id":"sound","lines":["Sound"],"rect":[0,0,100,30]},)"
        R"({"id":"volume","ratio":0.3,"rect":[0,40,216,16],"value":30}],"viewport":[1280,720]})");
    const json::Value summary = session.ok("frame.summary", R"({"ids": ["volume"]})");
    CHECK(compact(*summary.find("drawn")) ==
          R"([{"id":"volume","kind":"HSlider","rect":[0,40,216,16],"value":30}])");
}
