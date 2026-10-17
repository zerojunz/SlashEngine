// Tooltips and drag and drop, as Godot's: main.scene.json is an inventory. Three slots, a sword
// in the first and a potion in the second, and a trash bin. The items' DragAndDrop components
// say what dragging them carries ({"kind": "item", "id": ...}) and which hidden control follows
// the pointer meanwhile; the slots and the bin take anything of kind "item". Each item and the
// bin have a tooltip_text. The inventory system below is the game's part: when an item is
// dropped, it moves it into the slot (swapping with the item there) or throws it away.
//
// This program works the inventory as an agent does over MCP or JSON-RPC: it rests the mouse on
// the sword until its tooltip shows, drags the sword to the empty slot with input.inject, drags
// the potion onto it with ui.drag, cancels a drag with a right click and throws the sword away,
// reading the drag events with events.read and the result with ui.layout and entity.get.

#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/ui/ui.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <algorithm>
#include <cstdio>
#include <string_view>

namespace {

namespace command = slash::command;
namespace json = slash::json;
namespace ui = slash::ui;
namespace world = slash::world;

// The slot an item is in: the control whose children hold it.
entt::entity slot_of(world::World& w, entt::entity item) {
    for (const auto [entity, control] : w.ecs().view<world::Control>().each()) {
        if (std::ranges::find(control.children, item) != control.children.end()) {
            return entity;
        }
    }
    return entt::null;
}

// Moves a dropped item into the slot it was dropped on, and the item that was there into the
// slot it came from, or destroys it when it was dropped on the trash. It runs after the gui
// system, so it sees this tick's drop_data events.
void inventory(world::World& w, double /*tick_seconds*/) {
    for (const ui::UiEvent& event : ui::events(w)) {
        if (event.kind != "drop_data") {
            continue;
        }
        const json::Value* id = event.data.find("id");
        const entt::entity item = id != nullptr ? w.find(id->as_string()) : entt::null;
        const entt::entity from = item != entt::null ? slot_of(w, item) : entt::null;
        const entt::entity to = w.find(event.id);
        if (from == entt::null || to == from) {
            continue;
        }
        auto& left = w.ecs().get<world::Control>(from).children;
        std::erase(left, item);
        if (event.id == "trash") {
            w.destroy(item);
            continue;
        }
        auto& slot = w.ecs().get<world::Control>(to).children;
        left = slot;
        slot = {item};
    }
}

// Prints the call as an agent would make it, then the reply.
void call(command::Session& session, std::string_view name, std::string_view params) {
    const json::Value value = json::parse(params).value();
    std::printf("%.*s %s\n", static_cast<int>(name.size()), name.data(),
                json::write(value, json::Style::Compact).c_str());
    const command::Reply reply = session.call(name, value);
    std::puts(json::write(reply.ok ? reply.value : json::Value(reply.message), json::Style::Compact)
                  .c_str());
}

} // namespace

int main() {
    world::ComponentRegistry components;
    world::register_builtin_components(components);
    command::Registry commands;
    command::register_builtin_commands(commands);
    command::SessionOptions options;
    options.project_root = ".";
    options.capture_logs = false;
    command::Session session(components, commands, options);
    world::World& w = session.world();
    w.add_system("inventory", inventory);

    call(session, "scene.open", R"({"path": "main.scene.json"})");
    call(session, "events.subscribe", R"({"kinds": ["drag_begin", "drop_data", "drag_end"]})");
    call(session, "ui.layout", R"({"ids": ["slot_1", "slot_2", "slot_3", "trash", "sword"]})");

    // The mouse rests on the sword for half a second, the tick that moves it and 30 more: its
    // tooltip shows below and right of the pointer, over everything. frame.summary lists it as
    // drawn.
    call(session, "input.inject",
         R"({"events": [{"type": "mouse_motion", "position": [40, 64]}]})");
    call(session, "step", R"({"ticks": 31})");
    call(session, "ui.layout", R"({"ids": ["sword"]})");
    call(session, "frame.summary", R"({"ids": ["sword"]})");

    // Press on the sword and move past the 10-pixel drag threshold: the press hides the
    // tooltip, the drag starts and the sword's ghost follows the pointer. Over the empty slot,
    // ui.layout says the slot would take it.
    call(session, "input.inject", R"({"events": [
        {"type": "mouse_button", "button": "left", "position": [40, 64], "pressed": true},
        {"type": "mouse_motion", "position": [60, 64]},
        {"type": "mouse_motion", "position": [152, 64]}]})");
    call(session, "step", "{}");
    call(session, "ui.layout", R"({"ids": ["sword_ghost"]})");

    // Let go: the slot takes the drop and the inventory system moves the sword into it.
    call(session, "input.inject", R"({"events": [
        {"type": "mouse_button", "button": "left", "position": [152, 64], "pressed": false}]})");
    call(session, "step", "{}");
    call(session, "events.read", R"({"subscription": "events_1"})");
    call(session, "ui.layout", R"({"ids": ["sword"]})");

    // ui.drag drags the potion onto the sword's slot in one call, as the mouse would. The sword
    // passes the mouse on to its slot, which takes the drop, so the two swap.
    call(session, "ui.drag", R"({"from": "potion", "to": "sword"})");
    call(session, "step", "{}");
    call(session, "events.read", R"({"subscription": "events_1"})");
    call(session, "ui.layout", R"({"ids": ["sword", "potion"]})");

    // A right click cancels a drag: nothing is dropped.
    call(session, "input.inject", R"({"events": [
        {"type": "mouse_button", "button": "left", "position": [96, 64], "pressed": true},
        {"type": "mouse_motion", "position": [200, 120]},
        {"type": "mouse_button", "button": "right", "position": [200, 120], "pressed": true},
        {"type": "mouse_button", "button": "right", "position": [200, 120], "pressed": false},
        {"type": "mouse_button", "button": "left", "position": [200, 120], "pressed": false}]})");
    call(session, "step", "{}");
    call(session, "events.read", R"({"subscription": "events_1"})");

    // Dropped on the trash, the sword is thrown away.
    call(session, "ui.drag", R"({"from": "sword", "to": "trash"})");
    call(session, "step", "{}");
    call(session, "events.read", R"({"subscription": "events_1"})");
    call(session, "world.query", R"({"with": ["DragAndDrop"]})");

    // ui.tooltip shows a tooltip without the mouse; a control with none shows nothing.
    call(session, "ui.tooltip", R"({"id": "trash"})");
    call(session, "ui.tooltip", R"({"id": "title"})");
    return 0;
}
