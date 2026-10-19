#include "slash/command/mcp.hpp"

#include "slash/command/rpc.hpp"
#include "slash/command/session.hpp"
#include "slash/core/assert.hpp"
#include "slash/core/text.hpp"
#include "slash/core/version.hpp"

#include <algorithm>
#include <utility>
#include <vector>

namespace slash::command::mcp {
namespace {

// MCP's own code for an unknown resource.
constexpr int kResourceNotFound = -32002;

constexpr std::string_view kInstructions =
    R"(SlashEngine is a game engine. These tools change the open scene directly and safely: every change is checked against the engine's schemas, can be undone, and returns a compact result. Use them instead of editing .scene.json files by hand.
- Call engine_describe first: it lists every component with its default field values, and every command.
- Use batch to run many commands in one call. It is all or nothing, and one undo reverts it.
- For many similar entities, prefab_save one to a .prefab.json file and entity_create the rest with "extends"; each saves only the fields it overrides.
- To put an entity in another's space (a gun on a ship), list its ID in the parent's Transform2D children: it moves, turns, scales and draws with the parent, and a child with a CollisionShape2D and no body is a shape of its parent's body. entity_get gives a child's parent and global transform; entity_destroy takes children with it.
- events_subscribe, then events_read, tells you what changed and what the game emitted since you last looked, without re-querying.
- Add "dry_run": true to any edit or batch to see what it would change without changing anything. world_snapshot, then world_diff later, lists every entity and field that changed; world_restore goes back.
- world_query returns only entity IDs unless you pass include. Filter with where, e.g. {"component": "Transform2D", "field": "position.y", "op": ">", "value": 100}.
- component_set returns only the fields that changed.
- To test gameplay, run_until steps the game until an entity matches with/where (or none does) and returns the tick and the matches, instead of a step and a query per tick.
- Keep tests in .test.json files (schema_get kind "test"): each opens a scene and runs command steps, with what each reply must hold in "expect". test_run runs them all headless after a change and returns only the checks that failed (a failed command, a reply that doesn't match, a warning or error logged), plus storyboard_check. Save the scene first: tests start from scene files.
- replay_record records a run (the ticks, input and commands) to a .replay.json file and replay_stop writes it; replay_run plays one back exactly in any session, stops at "tick" if given, and says whether it matched and the first tick it diverged. replay_seek then moves the playback to a tick, by "ticks" (negative goes back), or to the first tick where with/without/where matches. Use them to reproduce a recorded bug and query the world where it happens, or to check a change against a recorded run.
- The storyboard holds the game's facts (characters, places, story, rules, names, decisions, open work). Before a task, storyboard_brief with the scenes or entities it touches, and use those facts instead of guessing; record new ones with storyboard_edit (pass the version you read); after it, storyboard_check.
- Sprite2D draws a texture or one frame of a sprite sheet; AnimatedSprite2D plays an animation from a .frames.json file on it (schema_get kind "frames"). Sprite2D's material is a .material.json node graph (schema_get kind "material"); material_eval returns the colors it draws, so you can check it without a screenshot.
- AnimationPlayer plays keyframe animations from a .anim.json file (schema_get kind "animation"): tracks set any component field, such as "Sprite2D/modulate" or "Transform2D/position/y", and event tracks emit events. animation_eval returns the values at any times, so you can check an animation without playing it.
- TileMapLayer draws a grid of tiles from a .tileset.json file (schema_get kind "tileset"); its cells are rows of one-letter legend keys. tilemap_paint paints a terrain such as "grass" on cells and picks the tiles whose edges connect; tilemap_set places one tile ("crate", or "crate:hv" flipped both ways); tilemap_get returns the rows. Cells may be isometric diamonds or hexagons (tile_shape); tilemap_convert turns cells into points and back. Tiles with collision polygons are solid; physics_raycast reports the cell hit.
- 2D lights are Godot's: PointLight2D and DirectionalLight2D light sprites and tile layers once the scene has a light or a CanvasModulate (its color is the ambient light), LightOccluder2D polygons cast shadows from lights with shadow_enabled, and normal_texture on a Sprite2D or a tile set source adds bumps. A light reaches the canvases its range_layer_min to range_layer_max holds (the world is layer 0, a CanvasLayer its layer), and each canvas has its own CanvasModulate. light_sample returns how lit points or an entity are and which lights reach them, so you can check lighting without a screenshot.
- Camera2D picks what the window shows, as Godot's: the first enabled one in drawing order is current, so one in the player's Transform2D children follows the player. zoom, the limits, drag margins and smoothing shape the view; controls stay put on the screen. frame_summary's camera gives the world rect the window shows.
- CanvasLayer draws what its children list in a canvas of its own, as Godot's: a HUD that stays put over the world, or a background under it (layer -1). Parallax2D scrolls its entity at scroll_scale times the camera's speed; repeat_size draws what it holds again to fill the window and autoscroll drifts it. canvas_convert turns window pixels, such as a click, into the world through the camera, or back.
- 2D physics is Godot's: StaticBody2D, AnimatableBody2D, RigidBody2D, CharacterBody2D and Area2D take their shapes from CollisionShape2D or CollisionPolygon2D, and collide by collision_layer and collision_mask. A CharacterBody2D moves by its velocity, as move_and_slide, and reports on_floor and on_wall. Area2D space overrides change gravity and damping inside them (physics_gravity reports a body's), and a StaticBody2D's constant velocities make a conveyor. After a step, physics_raycast, physics_overlap and physics_contacts say what is where and what touches what.
- CPUParticles2D emits particles as Godot's: an emission shape, a direction and spread, gravity and the rest as _min/_max ranges with curves over each particle's life, and color_ramp. particles_eval returns how many are alive, where they are and, with list, each one, now or after a time, so you can check particles without a screenshot. A one_shot emits a finished event when its last particle dies.
- UI is Godot's Control nodes: Control places a rect in its parent's (or the window's) by anchors and offsets and lists its children; containers (BoxContainer, GridContainer, MarginContainer, CenterContainer, FlowContainer, PanelContainer, ScrollContainer) place their children instead; Label, ColorRect, Panel, TextureRect, NinePatchRect and ProgressBar draw in it, above the world. Themes are .theme.json files (schema_get kind "theme") set with Control.theme or project.json's gui.theme; theme_override_* sets one item, theme_override_styles a style box. Fonts are .ttf, .otf or .font.json files (schema_get kind "font"). A .font.json file's fallbacks draw the characters its font lacks; Control.layout_direction rtl mirrors a control and its children, and right-to-left text orders itself. ui_layout returns each control's rect and the lines labels show, ui_theme the theme items a control uses and where each comes from, and text_measure measures text, so you can check UI without a screenshot.
- Input is Godot's: project.json's "input" binds actions such as "jump" to keys, mouse buttons and gamepad buttons and sticks, over the built-in ui_* actions, and gameplay code reads actions. To play, input_inject events (keys, clicks, touches, sticks or actions by name) then step: a press and a release before one step is a tap. input_state shows what the game saw and input_actions the bindings.
- Button, CheckBox, CheckButton and TextureButton are Godot's: a click or tap, ui_accept while focused or a shortcut presses them, and they emit pressed, toggled, button_down and button_up events; toggle_mode makes a switch and button_group a set of options (CheckBox and CheckButton toggle, drawn as a box or a switch). ui_press presses one at the next step and ui_focus gives one focus; or input_inject a click, Tab or arrow keys and step. ui_layout reports each button's draw_mode and which control has focus and is hovered.
- Slider is Godot's HSlider, or VSlider with vertical: a click or drag, the wheel, arrow keys or a stick set its value between min_value and max_value by step, emitting value_changed, drag_started and drag_ended; ui_layout reports its value and ratio.
- ScrollContainer is Godot's: it shows its child through its rect, with scroll bars when the child doesn't fit (horizontal_scroll_mode and vertical_scroll_mode), and scroll_horizontal and scroll_vertical say how far it scrolled; the wheel, its bars and a finger drag scroll it, and follow_focus scrolls to the control with focus. Control.clip_contents cuts a control's drawing and the clicks its children get at its rect, as a ScrollContainer always does. ScrollBar is a scroll bar of its own (HScrollBar, or VScrollBar with vertical). ui_layout reports each ScrollContainer's scroll, scroll_max and scroll_bars and a clipped control's shown_rect, and frame_summary lists controls clipped away.
- LineEdit is Godot's text field: a click, Tab or ui_focus starts editing it; input_inject {"type": "text", "text": "..."} types at the caret, {"type": "ime"} events compose as an input method does, and ui_text_* actions, ui_copy, ui_paste and ui_undo edit; Enter emits text_submitted. ui_layout reports its caret_column and selection, and ui_clipboard reads the clipboard. TextEdit is the same for several lines: Enter types a new line, Tab indents when tab_input_mode is on, and ui_layout reports its caret and selection as [line, column] and the lines in view. A CodeEdit beside it adds line numbers and breakpoints (breakpointed_lines), whose gutters ui_layout reports.
- Sound is Godot's: AudioStreamPlayer and AudioStreamPlayer2D play .wav, .ogg or .mp3 files, or .audio.json files that loop a sound or pick one of several at random (schema_get kind "audio"), through buses with effects from a .bus_layout.json file (schema_get kind "bus_layout") named by project.json's audio.bus_layout. audio_play and audio_stop play and stop one at the next step; audio_state shows the buses' levels and what each player plays, and audio_capture measures what a bus played (peak and RMS dB, slice by slice), so you can check sound without listening.
- frame_summary says what the window shows as data: each sprite, tile map layer, particle emitter and control that shows, with its rect, its text and how much of it shows; what is covered, offscreen, clear or clipped away; and what can't draw (a missing texture). "map": [columns, rows] adds a text picture of the frame, "ids" and "region" narrow it. Use it to check what a player sees instead of an image.
- frame_capture draws what the window shows, the same on every machine: "points" returns pixel colors (cheapest), "image": true a PNG you can look at (keep it small with "region" and "scale") and "path" saves one; "collisions": true draws the collision shapes too. Prefer the checks above when they answer the question.
- In a running game, perf_stats reports fps and frame times, display_set changes V-Sync and the frame cap, and debug_overlay shows the person watching a panel of stats and the collision shapes.
- The game's own components and systems are C++ in the folders project.json's "gameplay" "sources" names (src/), registered in its SLASH_GAME_MODULE function. After changing the code, call gameplay_build: it returns compiler errors with file and line, or swaps the new code into the open world, which keeps its entities and the fields the code still has. Keep gameplay state in registered components.
- Edited files reload on their own before each call: prefab instances update and keep their overrides. asset_list shows what is loaded.
- Errors give the JSON Pointer of the bad value and suggest the right name when one is misspelled, so fix it and retry.
- Paths are relative to the project folder. Call scene_save when you are done.)";

struct Resource {
    std::string_view uri;
    std::string_view name;
    std::string_view title;
    std::string_view description;
    std::string_view mime_type;
    std::string_view command; // the command that reads it, so capabilities apply
};

constexpr std::array<Resource, 3> kResources{{
    {"slash://scene", "scene", "Open scene", "The open scene in .scene.json form.",
     "application/json", "scene.get"},
    {"slash://schema/scene", "scene-schema", "Scene schema", "JSON Schema for .scene.json files.",
     "application/schema+json", "schema.get"},
    {"slash://log", "log", "Recent logs", "The most recent engine log lines.", "application/json",
     "log.tail"},
}};

bool has(const Command& command, Capability capability) {
    return std::ranges::find(command.capabilities, capability) != command.capabilities.end();
}

json::Value text_content(std::string text) {
    json::Value block;
    block["type"] = "text";
    block["text"] = std::move(text);
    return block;
}

} // namespace

std::string tool_name(std::string_view command) {
    std::string name(command);
    std::ranges::replace(name, '.', '_');
    return name;
}

std::string_view instructions() {
    return kInstructions;
}

Server::Server(Session& session) : session_(&session) {
    for (const Command& command : session.commands().commands()) {
        const bool added = tools_.emplace(tool_name(command.name), &command).second;
        SLASH_ASSERT(added, "two commands map to the same MCP tool name");
    }
}

std::string Server::handle(std::string_view message) {
    return rpc::handle(message, [this](std::string_view method, const json::Value& params) {
        return dispatch(method, params);
    });
}

Reply Server::dispatch(std::string_view method, const json::Value& params) {
    if (method == "initialize") {
        // The client's name says who made a change, e.g. in the storyboard journal.
        if (const json::Value* info = params.find("clientInfo"); info != nullptr) {
            if (const json::Value* name = info->find("name");
                name != nullptr && name->is_string()) {
                session_->set_client(name->as_string());
            }
        }
        return initialize(params);
    }
    if (method == "ping") {
        return Reply::success(json::Object{});
    }
    if (method == "tools/list") {
        return list_tools();
    }
    if (method == "tools/call") {
        return call_tool(params);
    }
    if (method == "resources/list") {
        return list_resources();
    }
    if (method == "resources/templates/list") {
        json::Value result;
        result["resourceTemplates"] = json::Array{};
        return Reply::success(std::move(result));
    }
    if (method == "resources/read") {
        return read_resource(params);
    }
    if (method.starts_with("notifications/")) {
        return Reply::success({});
    }
    return Reply::failure(code::kMethodNotFound, "unknown method \"" + std::string(method) + "\"");
}

Reply Server::initialize(const json::Value& params) {
    std::string_view version = kProtocolVersions.front();
    if (const json::Value* requested = params.find("protocolVersion");
        requested != nullptr && requested->is_string() &&
        std::ranges::find(kProtocolVersions, requested->as_string()) != kProtocolVersions.end()) {
        version = requested->as_string();
    }
    json::Value result;
    result["protocolVersion"] = version;
    result["capabilities"]["tools"]["listChanged"] = false;
    result["capabilities"]["resources"]["listChanged"] = false;
    result["capabilities"]["resources"]["subscribe"] = false;
    result["serverInfo"]["name"] = "slashengine";
    result["serverInfo"]["title"] = "SlashEngine";
    result["serverInfo"]["version"] = slash::version();
    result["instructions"] = kInstructions;
    return Reply::success(std::move(result));
}

Reply Server::list_tools() const {
    json::Array tools;
    for (const auto& [name, command] : tools_) {
        const bool read_only =
            command->effect == Effect::Read && !has(*command, Capability::Edit) &&
            !has(*command, Capability::Run) && !has(*command, Capability::Filesystem) &&
            !has(*command, Capability::Build);
        json::Value tool;
        tool["name"] = name;
        tool["description"] = command->description;
        tool["inputSchema"] = command->params;
        tool["annotations"]["readOnlyHint"] = read_only;
        if (!read_only) {
            // Edits can be undone; replacing the scene, writing files or building cannot.
            tool["annotations"]["destructiveHint"] = command->effect == Effect::Replace ||
                                                     has(*command, Capability::Filesystem) ||
                                                     has(*command, Capability::Build);
        }
        tool["annotations"]["openWorldHint"] = false;
        tools.push_back(std::move(tool));
    }
    json::Value result;
    result["tools"] = std::move(tools);
    return Reply::success(std::move(result));
}

Reply Server::call_tool(const json::Value& params) {
    const json::Value* name = params.find("name");
    if (name == nullptr || !name->is_string()) {
        return Reply::failure(code::kInvalidParams, "tools/call needs a tool name");
    }
    const auto tool = tools_.find(name->as_string());
    if (tool == tools_.end()) {
        std::vector<std::string_view> names;
        names.reserve(tools_.size());
        for (const auto& [candidate, command] : tools_) {
            names.emplace_back(candidate);
        }
        std::string message = "unknown tool \"" + name->as_string() + "\"";
        if (const std::string_view hint = closest_match(name->as_string(), names); !hint.empty()) {
            message += "; did you mean \"" + std::string(hint) + "\"?";
        }
        return Reply::failure(code::kInvalidParams, std::move(message));
    }

    const json::Value* arguments = params.find("arguments");
    Reply reply =
        session_->call(tool->second->name, arguments != nullptr ? *arguments : json::Value());

    // Failures are tool results rather than protocol errors, so the model sees them and can
    // correct its call.
    json::Value result;
    if (reply.ok) {
        // A frame.capture image goes to the model as an image rather than as base64 text.
        std::string png;
        if (const json::Value* image = reply.value.is_object() ? reply.value.find("png") : nullptr;
            image != nullptr && image->is_string()) {
            png = image->as_string();
            reply.value.as_object().erase("png");
        }
        json::Array content{text_content(json::write(reply.value, json::Style::Compact))};
        if (!png.empty()) {
            json::Value block;
            block["type"] = "image";
            block["data"] = std::move(png);
            block["mimeType"] = "image/png";
            content.push_back(std::move(block));
        }
        result["content"] = std::move(content);
        if (reply.value.is_object()) {
            result["structuredContent"] = std::move(reply.value);
        }
        result["isError"] = false;
    } else {
        json::Value error;
        error["code"] = reply.code;
        error["message"] = std::move(reply.message);
        if (!reply.data.is_null()) {
            error["data"] = std::move(reply.data);
        }
        json::Value wrapped;
        wrapped["error"] = std::move(error);
        result["content"] = json::Array{text_content(json::write(wrapped, json::Style::Compact))};
        result["isError"] = true;
    }
    return Reply::success(std::move(result));
}

Reply Server::list_resources() {
    json::Array resources;
    for (const Resource& resource : kResources) {
        json::Value item;
        item["uri"] = resource.uri;
        item["name"] = resource.name;
        item["title"] = resource.title;
        item["description"] = resource.description;
        item["mimeType"] = resource.mime_type;
        resources.push_back(std::move(item));
    }
    json::Value result;
    result["resources"] = std::move(resources);
    return Reply::success(std::move(result));
}

Reply Server::read_resource(const json::Value& params) {
    const json::Value* uri = params.find("uri");
    if (uri == nullptr || !uri->is_string()) {
        return Reply::failure(code::kInvalidParams, "resources/read needs a uri");
    }
    const Resource* resource = nullptr;
    for (const Resource& candidate : kResources) {
        if (candidate.uri == uri->as_string()) {
            resource = &candidate;
        }
    }
    if (resource == nullptr) {
        json::Value data;
        data["uri"] = uri->as_string();
        return Reply::failure(kResourceNotFound, "unknown resource \"" + uri->as_string() + "\"",
                              std::move(data));
    }
    json::Value args = json::Object{};
    if (resource->command == "schema.get") {
        args["kind"] = "scene";
    }
    Reply reply = session_->call(resource->command, args);
    if (!reply.ok) {
        return reply;
    }
    const json::Value& value =
        resource->command == "scene.get" ? *reply.value.find("scene") : reply.value;
    json::Value content;
    content["uri"] = resource->uri;
    content["mimeType"] = resource->mime_type;
    content["text"] = json::write(value, json::Style::Compact);
    json::Value result;
    result["contents"] = json::Array{std::move(content)};
    return Reply::success(std::move(result));
}

} // namespace slash::command::mcp
