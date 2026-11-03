#include "slash/accessibility/accessibility.hpp"
#include "slash/accessibility/web.hpp"
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
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The web's screen reader backend: the controls' tree as WAI-ARIA elements in the page, the
// changes that keep them up to date, and the clicks and focus a screen reader gives them.

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
};

// A sign-up box: a title, a name field, a check box, a slider the title labels and a toggle.
void make_form(Session& s) {
    s.ok("batch", R"({"commands": [
        {"command": "entity.create", "params": {"id": "title", "components": {
            "Label": {"text": "Sign up"}}}},
        {"command": "entity.create", "params": {"id": "name", "components": {
            "LineEdit": {"placeholder_text": "Your name"},
            "Control": {"accessibility_name": "Name", "tooltip_text": "As on your card",
                        "custom_minimum_size": [120, 0]}}}},
        {"command": "entity.create", "params": {"id": "news", "components": {
            "CheckBox": {"text": "News", "button_pressed": true}}}},
        {"command": "entity.create", "params": {"id": "volume", "components": {
            "Slider": {"value": 50, "step": 5},
            "Control": {"accessibility_name": "Volume", "custom_minimum_size": [100, 0],
                        "accessibility_labeled_by_nodes": ["title"]}}}},
        {"command": "entity.create", "params": {"id": "mute", "components": {
            "Button": {"text": "Mute", "toggle_mode": true},
            "Control": {"accessibility_description": "Silences the game"}}}},
        {"command": "entity.create", "params": {"id": "form", "components": {
            "BoxContainer": {"vertical": true},
            "Control": {"children": ["title", "name", "news", "volume", "mute"],
                        "accessibility_live": "polite"}}}}
    ]})");
}

json::Value set_of(const json::Value& changes, const std::string& id) {
    const json::Value* set = changes.find("set");
    REQUIRE(set != nullptr);
    const auto& items = set->as_array();
    const auto found = std::ranges::find_if(
        items, [&](const json::Value& item) { return item.find("id")->as_string() == id; });
    REQUIRE(found != items.end());
    return *found;
}

std::vector<std::string> ids_set(const json::Value& changes) {
    std::vector<std::string> out;
    if (const json::Value* set = changes.find("set"); set != nullptr) {
        for (const json::Value& item : set->as_array()) {
            out.push_back(item.find("id")->as_string());
        }
    }
    return out;
}

} // namespace

TEST_CASE("Nodes become ARIA elements: roles, names, states, numbers and relations") {
    Session s;
    make_form(s);
    const accessibility::Snapshot snapshot = accessibility::snapshot(s.world(), "Sign up");
    const auto element = [&](std::uint64_t id) {
        const auto found = std::ranges::find_if(
            snapshot.nodes, [id](const auto& node) { return node.first == id; });
        REQUIRE(found != snapshot.nodes.end());
        return accessibility::dom_element(found->second);
    };
    using Attributes = std::vector<std::pair<std::string, std::string>>;

    const accessibility::DomElement window = element(accessibility::kWindowNode);
    CHECK(window.role == "application");
    CHECK(window.attributes == Attributes{{"aria-label", "Sign up"}});

    // A label is its text; a container is a generic element that announces changes politely.
    const accessibility::DomElement title = element(s.id("title"));
    CHECK(title.role.empty());
    CHECK(title.text == "Sign up");
    const accessibility::DomElement form = element(s.id("form"));
    CHECK(form.role.empty());
    CHECK(form.attributes == Attributes{{"aria-live", "polite"}, {"data-actions", "focus"}});

    const accessibility::DomElement name = element(s.id("name"));
    CHECK(name.role == "textbox");
    CHECK(name.text.empty());
    CHECK(name.attributes == Attributes{{"aria-label", "Name"},
                                        {"aria-placeholder", "Your name"},
                                        {"data-actions", "focus"},
                                        {"title", "As on your card"}});

    const accessibility::DomElement news = element(s.id("news"));
    CHECK(news.role == "checkbox");
    CHECK(news.attributes == Attributes{{"aria-checked", "true"},
                                        {"aria-label", "News"},
                                        {"data-actions", "click focus"}});

    const accessibility::DomElement volume = element(s.id("volume"));
    CHECK(volume.role == "slider");
    CHECK(volume.attributes == Attributes{{"aria-label", "Volume"},
                                          {"aria-labelledby", accessibility::dom_id(s.id("title"))},
                                          {"aria-valuemax", "100"},
                                          {"aria-valuemin", "0"},
                                          {"aria-valuenow", "50"},
                                          {"data-actions", "focus"}});

    // A toggle button is pressed or not.
    const accessibility::DomElement mute = element(s.id("mute"));
    CHECK(mute.role == "button");
    CHECK(mute.attributes == Attributes{{"aria-description", "Silences the game"},
                                        {"aria-label", "Mute"},
                                        {"aria-pressed", "false"},
                                        {"data-actions", "click focus"}});
}

TEST_CASE("The page gets every element once, then only what changed") {
    Session s;
    make_form(s);
    const accessibility::Snapshot first = accessibility::snapshot(s.world(), "Sign up");
    const json::Value all = accessibility::dom_changes({}, first);
    CHECK(all.find("remove") == nullptr);
    CHECK(ids_set(all).size() == first.nodes.size());
    // The window's element first, holding the form, over the whole viewport.
    const json::Value window = set_of(all, "1");
    CHECK(window.find("role")->as_string() == "application");
    CHECK(*window.find("children") == parse("[\"" + s.text_id("form") + "\"]"));
    // Its bounds are the canvas's, which the page knows.
    CHECK(window.find("bounds")->size() == 0);
    CHECK(*set_of(all, s.text_id("title")).find("bounds") == parse("[0, 0, 120, 22]"));
    const json::Value form = set_of(all, s.text_id("form"));
    CHECK(*form.find("children") ==
          parse("[\"" + s.text_id("title") + "\", \"" + s.text_id("name") + "\", \"" +
                s.text_id("news") + "\", \"" + s.text_id("volume") + "\", \"" + s.text_id("mute") +
                "\"]"));
    // Nothing has focus: the window's element does.
    CHECK(all.find("focus")->as_string() == "1");

    CHECK(accessibility::dom_changes(first, first) == json::Value(json::Object{}));

    // A step of the slider changes its element alone; focus moving says where to.
    s.ok("ui.accessibility_action", R"({"id": "volume", "action": "increment"})");
    s.ok("ui.accessibility_action", R"({"id": "news", "action": "focus"})");
    const accessibility::Snapshot stepped = accessibility::snapshot(s.world(), "Sign up");
    const json::Value step = accessibility::dom_changes(first, stepped);
    CHECK(ids_set(step) == std::vector<std::string>{s.text_id("volume")});
    CHECK(
        set_of(step, s.text_id("volume")).find("attributes")->find("aria-valuenow")->as_string() ==
        "55");
    CHECK(step.find("focus")->as_string() == s.text_id("news"));

    // A control that goes is removed, and its parent's children change.
    const std::string mute = s.text_id("mute");
    s.ok("batch", R"({"commands": [
        {"command": "entity.destroy", "params": {"id": "mute"}}
    ]})");
    const accessibility::Snapshot smaller = accessibility::snapshot(s.world(), "Sign up");
    const json::Value removed = accessibility::dom_changes(stepped, smaller);
    CHECK(*removed.find("remove") == parse("[\"" + mute + "\"]"));
    CHECK(ids_set(removed) == std::vector<std::string>{s.text_id("form")});
}

TEST_CASE("Clicks and focus the page queued run on the controls they name") {
    Session s;
    make_form(s);
    const json::Value queued =
        parse("[{\"node\": \"" + s.text_id("news") + "\", \"action\": \"click\"}, " +
              "{\"node\": \"" + s.text_id("volume") + "\", \"action\": \"focus\"}, " +
              // The window takes nothing, the page sends no other actions, and IDs are strings.
              "{\"node\": \"1\", \"action\": \"click\"}, " + "{\"node\": \"" + s.text_id("volume") +
              "\", \"action\": \"increment\"}, " + "{\"node\": " + s.text_id("news") +
              ", \"action\": \"click\"}, " + "{\"node\": \"12x\", \"action\": \"click\"}]");
    const std::vector<accessibility::Request> requests = accessibility::dom_requests(queued);
    REQUIRE(requests.size() == 2);
    CHECK(requests[0].node == s.id("news"));
    CHECK(requests[0].action == "click");
    CHECK(requests[1].node == s.id("volume"));
    CHECK(requests[1].action == "focus");
    CHECK(accessibility::dom_requests(parse("{}")).empty());

    for (const accessibility::Request& request : requests) {
        REQUIRE(accessibility::perform(s.world(), request));
    }
    // The check box presses at the next tick, as ui::press does; focus moves now.
    s.ok("step", R"({"ticks": 1})");
    const accessibility::Snapshot after = accessibility::snapshot(s.world(), "Sign up");
    CHECK(after.focus == s.id("volume"));
    const auto news = std::ranges::find_if(
        after.nodes, [&](const auto& node) { return node.first == s.id("news"); });
    REQUIRE(news != after.nodes.end());
    CHECK(news->second.checked == false);
}
