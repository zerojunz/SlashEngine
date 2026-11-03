#include "slash/accessibility/accessibility.hpp"
#include "slash/accessibility/mobile.hpp"
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/ui/ui.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The phones' screen reader backends: the controls' tree as Android's virtual views for TalkBack
// and UIAccessibilityElements for VoiceOver, and what their gestures ask of the controls.

namespace accessibility = slash::accessibility;
namespace command = slash::command;
namespace json = slash::json;
namespace ui = slash::ui;
namespace world = slash::world;

namespace {

json::Value parse(std::string_view source) {
    auto parsed = json::parse(source);
    REQUIRE_MESSAGE(parsed.ok(), std::string(source));
    return std::move(parsed).value();
}

struct Session {
    world::ComponentRegistry components;
    command::Registry commands;
    std::unique_ptr<command::Session> session;

    Session() {
        world::register_builtin_components(components);
        command::register_builtin_commands(commands);
        command::register_run_commands(commands);
        command::SessionOptions options;
        options.capture_logs = false;
        session = std::make_unique<command::Session>(components, commands, std::move(options));
        ui::set_viewport(session->world(), {400.0F, 300.0F});
    }

    world::World& world() { return session->world(); }

    void ok(std::string_view name, std::string_view params) {
        const command::Reply reply = session->call(name, parse(params));
        REQUIRE_MESSAGE(reply.ok, reply.message);
    }

    std::uint64_t id(std::string_view control) {
        const entt::entity entity = world().find(control);
        REQUIRE(world().valid(entity));
        return accessibility::node_id(entity);
    }

    std::string text_id(std::string_view control) { return std::to_string(id(control)); }

    json::Value get(std::string_view control, std::string_view component) {
        const command::Reply reply =
            session->call("entity.get", parse("{\"id\": \"" + std::string(control) + "\"}"));
        REQUIRE_MESSAGE(reply.ok, reply.message);
        const json::Value* found = reply.value.find("components");
        REQUIRE(found != nullptr);
        const json::Value* value = found->find(component);
        return value != nullptr ? *value : json::Value();
    }
};

// A settings box: a title, a name field, a check box, a slider, a toggle and a list that scrolls.
void make_settings(Session& s) {
    s.ok("batch", R"({"commands": [
        {"command": "entity.create", "params": {"id": "title", "components": {
            "Label": {"text": "Settings"}}}},
        {"command": "entity.create", "params": {"id": "name", "components": {
            "LineEdit": {"placeholder_text": "Your name"},
            "Control": {"accessibility_name": "Name", "custom_minimum_size": [120, 0]}}}},
        {"command": "entity.create", "params": {"id": "news", "components": {
            "CheckBox": {"text": "News", "button_pressed": true}}}},
        {"command": "entity.create", "params": {"id": "volume", "components": {
            "Slider": {"value": 50, "step": 5},
            "Control": {"accessibility_name": "Volume", "custom_minimum_size": [100, 0],
                        "accessibility_description": "Music and sounds"}}}},
        {"command": "entity.create", "params": {"id": "mute", "components": {
            "Button": {"text": "Mute", "toggle_mode": true, "disabled": true}}}},
        {"command": "entity.create", "params": {"id": "entry", "components": {
            "Label": {"text": "Day one"},
            "Control": {"custom_minimum_size": [0, 400]}}}},
        {"command": "entity.create", "params": {"id": "log", "components": {
            "ScrollContainer": {},
            "Control": {"children": ["entry"], "custom_minimum_size": [120, 60],
                        "accessibility_live": "polite"}}}},
        {"command": "entity.create", "params": {"id": "form", "components": {
            "BoxContainer": {"vertical": true},
            "Control": {"children": ["title", "name", "news", "volume", "mute", "log"]}}}}
    ]})");
}

accessibility::Node node_of(const accessibility::Snapshot& snapshot, std::uint64_t id) {
    const auto found =
        std::ranges::find_if(snapshot.nodes, [id](const auto& node) { return node.first == id; });
    REQUIRE(found != snapshot.nodes.end());
    return found->second;
}

using Strings = std::vector<std::string>;

} // namespace

TEST_CASE("Nodes become Android's virtual views, as TalkBack reads widgets") {
    Session s;
    make_settings(s);
    const accessibility::Snapshot snapshot = accessibility::snapshot(s.world(), "Settings");
    const auto element = [&](std::string_view control) {
        return accessibility::android_element(node_of(snapshot, s.id(control)));
    };

    const accessibility::AndroidElement title = element("title");
    CHECK(title.class_name == "android.widget.TextView");
    CHECK(title.text == "Settings");
    CHECK(title.actions == Strings{"focus", "show_on_screen"});

    // A field's text is its contents; its name and placeholder are its hint.
    const accessibility::AndroidElement name = element("name");
    CHECK(name.class_name == "android.widget.EditText");
    CHECK(name.text.empty());
    CHECK(name.hint == "Name, Your name");
    CHECK(name.editable);
    CHECK_FALSE(name.multi_line);
    CHECK(name.actions == Strings{"focus", "set_text", "show_on_screen"});

    const accessibility::AndroidElement news = element("news");
    CHECK(news.class_name == "android.widget.CheckBox");
    CHECK(news.text == "News");
    CHECK(news.checkable);
    CHECK(news.checked);
    CHECK(news.actions == Strings{"click", "focus", "show_on_screen"});

    // A range scrolls forward and backward by its step, and takes a value.
    const accessibility::AndroidElement volume = element("volume");
    CHECK(volume.class_name == "android.widget.SeekBar");
    CHECK(volume.text == "Volume");
    CHECK(volume.hint == "Music and sounds");
    REQUIRE(volume.range);
    CHECK(*volume.range == std::array{50.0, 0.0, 100.0});
    CHECK(volume.scrollable);
    CHECK(volume.actions ==
          Strings{"focus", "scroll_forward", "scroll_backward", "set_progress", "show_on_screen"});

    const accessibility::AndroidElement mute = element("mute");
    CHECK(mute.class_name == "android.widget.ToggleButton");
    CHECK(mute.checkable);
    CHECK_FALSE(mute.checked);
    CHECK_FALSE(mute.enabled);

    const accessibility::AndroidElement log = element("log");
    CHECK(log.class_name == "android.widget.ScrollView");
    CHECK(log.live_region == 1);
    CHECK(log.scrollable);
    CHECK(log.actions == Strings{"focus", "scroll_forward", "scroll_backward", "scroll_up",
                                 "scroll_down", "scroll_left", "scroll_right", "show_on_screen"});
}

TEST_CASE("TalkBack's actions run on the controls they name") {
    Session s;
    make_settings(s);
    const accessibility::Snapshot sent = accessibility::snapshot(s.world(), "Settings");

    // The Java side gets every view once, with its fields by name.
    const json::Value all = accessibility::android_changes({}, sent);
    CHECK(all.find("set")->size() == sent.nodes.size());
    const auto& items = all.find("set")->as_array();
    const auto volume = std::ranges::find_if(items, [&](const json::Value& item) {
        return item.find("id")->as_string() == s.text_id("volume");
    });
    REQUIRE(volume != items.end());
    CHECK(volume->find("class_name")->as_string() == "android.widget.SeekBar");
    CHECK(*volume->find("range") == parse("[50, 0, 100]"));
    CHECK(volume->find("children")->size() == 0);
    CHECK(accessibility::android_changes(sent, sent) == json::Value(json::Object{}));

    const std::string queued =
        std::string("[") +
        // Forward on a range steps it; a progress sets it; a text sets a field.
        "{\"node\": \"" + s.text_id("volume") + "\", \"action\": \"scroll_forward\"}, " +
        "{\"node\": \"" + s.text_id("name") + "\", \"action\": \"set_text\", \"text\": \"Ada\"}, " +
        "{\"node\": \"" + s.text_id("news") + "\", \"action\": \"click\"}, " +
        // Forward on a scroll view shows what is below.
        "{\"node\": \"" + s.text_id("log") + "\", \"action\": \"scroll_forward\"}, " +
        // Dropped: the window, an action the node doesn't take, a value of the wrong kind, a
        // node the page never had.
        "{\"node\": \"1\", \"action\": \"click\"}, " + "{\"node\": \"" + s.text_id("title") +
        "\", \"action\": \"click\"}, " + "{\"node\": \"" + s.text_id("name") +
        "\", \"action\": \"set_progress\", \"value\": 3}, " + "{\"node\": \"" +
        s.text_id("volume") + "\", \"action\": \"set_progress\"}, " +
        "{\"node\": \"99999999\", \"action\": \"click\"}]";
    const std::vector<accessibility::Request> requests =
        accessibility::android_requests(parse(queued), sent);
    REQUIRE(requests.size() == 4);
    CHECK(requests[0].node == s.id("volume"));
    CHECK(requests[0].action == "increment");
    CHECK(requests[1].action == "set_value");
    CHECK(requests[1].data == json::Value("Ada"));
    CHECK(requests[2].action == "click");
    CHECK(requests[3].node == s.id("log"));
    CHECK(requests[3].action == "scroll_down");

    for (const accessibility::Request& request : requests) {
        REQUIRE(accessibility::perform(s.world(), request));
    }
    const std::vector<accessibility::Request> progress =
        accessibility::android_requests(parse("[{\"node\": \"" + s.text_id("volume") +
                                              "\", \"action\": \"set_progress\", \"value\": 20}]"),
                                        sent);
    REQUIRE(progress.size() == 1);
    CHECK(progress[0].action == "set_value");
    CHECK(progress[0].data == json::Value(20));
    REQUIRE(accessibility::perform(s.world(), progress[0]));
    s.ok("step", R"({"ticks": 1})");

    CHECK(s.get("volume", "Slider").find("value")->as_double() == doctest::Approx(20.0));
    CHECK(s.get("name", "LineEdit").find("text")->as_string() == "Ada");
    CHECK(s.get("news", "CheckBox").find("button_pressed")->as_bool() == false);
    CHECK(s.get("log", "ScrollContainer").find("scroll_vertical")->as_double() > 0.0);
}

TEST_CASE("Nodes become UIAccessibilityElements, and VoiceOver's gestures requests") {
    Session s;
    make_settings(s);
    const accessibility::Snapshot sent = accessibility::snapshot(s.world(), "Settings");
    const auto element = [&](std::string_view control) {
        return accessibility::ios_element(node_of(sent, s.id(control)));
    };

    CHECK_FALSE(accessibility::ios_element(node_of(sent, accessibility::kWindowNode)).element);
    CHECK_FALSE(element("form").element);
    CHECK_FALSE(element("log").element);

    const accessibility::IosElement title = element("title");
    CHECK(title.element);
    CHECK(title.label == "Settings");
    CHECK(title.traits == Strings{"static_text"});

    const accessibility::IosElement name = element("name");
    CHECK(name.label == "Name");
    CHECK(name.value.empty());
    CHECK(name.traits.empty());

    const accessibility::IosElement news = element("news");
    CHECK(news.label == "News");
    CHECK(news.traits == Strings{"button", "toggle_button", "selected"});

    const accessibility::IosElement volume = element("volume");
    CHECK(volume.label == "Volume");
    CHECK(volume.value == "50");
    CHECK(volume.hint == "Music and sounds");
    CHECK(volume.traits == Strings{"adjustable"});

    const accessibility::IosElement mute = element("mute");
    CHECK(mute.traits == Strings{"button", "toggle_button", "not_enabled"});

    // A double tap clicks; swipes step a range; three fingers scroll the view holding the node.
    const auto request = [&](std::string_view control, std::string_view gesture) {
        return accessibility::ios_request(sent, s.id(control), gesture);
    };
    const std::optional<accessibility::Request> click = request("news", "activate");
    REQUIRE(click);
    CHECK(click->action == "click");
    const std::optional<accessibility::Request> step = request("volume", "decrement");
    REQUIRE(step);
    CHECK(step->action == "decrement");
    const std::optional<accessibility::Request> scroll = request("entry", "scroll_down");
    REQUIRE(scroll);
    CHECK(scroll->node == s.id("log"));
    CHECK(scroll->action == "scroll_down");
    CHECK_FALSE(request("title", "activate"));
    CHECK_FALSE(request("volume", "activate"));
    CHECK_FALSE(request("title", "scroll_down"));
    CHECK_FALSE(request("news", "focus"));
    CHECK_FALSE(accessibility::ios_request(sent, accessibility::kWindowNode, "activate"));

    for (const auto& gesture : {*click, *step, *scroll}) {
        REQUIRE(accessibility::perform(s.world(), gesture));
    }
    s.ok("step", R"({"ticks": 1})");
    CHECK(s.get("volume", "Slider").find("value")->as_double() == doctest::Approx(45.0));
    CHECK(s.get("news", "CheckBox").find("button_pressed")->as_bool() == false);
    CHECK(s.get("log", "ScrollContainer").find("scroll_vertical")->as_double() > 0.0);
}
