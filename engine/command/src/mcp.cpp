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
- The tools are the commands most tasks need. tools_load with no arguments lists every command by group; tools_load {"groups": ["ui"]} or {"commands": ["audio.play"]} adds them as tools and returns their params. These notes name commands as tools (ui_layout is the command ui.layout); load one before you call it, or run any command with batch {"commands": [{"command": "ui.layout", "params": {...}}]}.
- Call engine_describe first: it lists every component with its default field values, and every command.
- Use batch to run many commands in one call. It is all or nothing, and one undo reverts it.
- For many similar entities, prefab_save one to a .prefab.json file and entity_create the rest with "extends"; each saves only the fields it overrides.
- To put an entity in another's space (a gun on a ship), list its ID in the parent's Transform2D children: it moves, turns, scales and draws with the parent, and a child with a CollisionShape2D and no body is a shape of its parent's body. entity_get gives a child's parent and global transform; entity_destroy takes children with it.
- events_subscribe, then events_read, tells you what changed and what the game emitted since you last looked, without re-querying.
- Add "dry_run": true to any edit or batch to see what it would change without changing anything. world_snapshot, then world_diff later, lists every entity and field that changed; world_restore goes back.
- world_query returns only entity IDs unless you pass include. Filter with where, e.g. {"component": "Transform2D", "field": "position.y", "op": ">", "value": 100}.
- component_set returns only the fields that changed.
- To test gameplay, run_until steps the game until an entity matches with/where (or none does) and returns the tick and the matches, instead of a step and a query per tick.
- To tune a value (jump height, enemy speed) or playtest, playtest_run plays many headless copies of the game at once: vary sets values by "entity/Component/field", a bot presses input actions (held, every n ticks, by chance or when entities match) and end conditions stop runs. It returns per value how the runs ended, their ticks, event counts and measured fields.
- Keep tests in .test.json files (schema_get kind "test"): each opens a scene and runs command steps, with what each reply must hold in "expect". test_run runs them all headless after a change and returns only the checks that failed (a failed command, a reply that doesn't match, a warning or error logged), plus storyboard_check. Save the scene first: tests start from scene files.
- replay_record records a run (the ticks, input and commands) to a .replay.json file and replay_stop writes it; replay_run plays one back exactly in any session, stops at "tick" if given, and says whether it matched and the first tick it diverged. replay_seek then moves the playback to a tick, by "ticks" (negative goes back), or to the first tick where with/without/where matches. Use them to reproduce a recorded bug and query the world where it happens, or to check a change against a recorded run.
- The storyboard holds the game's facts (characters, places, story, rules, names, decisions, open work). Before a task, storyboard_brief with the scenes or entities it touches, and use those facts instead of guessing; record new ones with storyboard_edit (pass the version you read); after it, storyboard_check.
- Before making an image or sound, asset_search the machine's asset library, which every project shares, and asset_use a match. asset_generate keeps what it makes there: an exact repeat returns the stored asset with no model call, and close matches come back as candidates (similar: false generates anyway). asset_refine makes a new version linked to the old. After using one, asset_feedback says whether it worked and why; that ranks later searches and goes into later prompts. asset_check (and a whole-project test_run) records by itself what the scenes show of them. With a team library in providers.json, these cover teammates' assets too.
- project_map says where things are: each scene's components and prefabs, and in the game's C++ sources each type and function by line and where each component, system, event and input action is declared and used. Pass find (a name) or paths (what a change touches) instead of reading or grepping files.
- Sprite2D draws a texture or one frame of a sprite sheet; AnimatedSprite2D plays an animation from a .frames.json file on it (schema_get kind "frames"). Sprite2D's material is a .material.json node graph (schema_get kind "material"); material_eval returns the colors it draws, so you can check it without a screenshot.
- AnimationPlayer plays keyframe animations from a .anim.json file (schema_get kind "animation"): tracks set any component field, such as "Sprite2D/modulate" or "Transform2D/position/y", and event tracks emit events. animation_eval returns the values at any times, so you can check an animation without playing it. AnimationTree blends a library's animations as Godot's, by a .animtree.json tree (schema_get kind "animtree") of blend, one-shot, transition and blend space nodes; animation_tree lists its parameters and what plays, and animation_set_parameters sets them.
- TileMapLayer draws a grid of tiles from a .tileset.json file (schema_get kind "tileset"); its cells are rows of one-letter legend keys. tilemap_paint paints a terrain such as "grass" on cells and picks the tiles whose edges connect; tilemap_set places one tile ("crate", or "crate:hv" flipped both ways); tilemap_get returns the rows. Cells may be isometric diamonds or hexagons (tile_shape); tilemap_convert turns cells into points and back. Tiles with collision polygons are solid; physics_raycast reports the cell hit.
- 2D lights are Godot's: PointLight2D and DirectionalLight2D light sprites and tile layers once the scene has a light or a CanvasModulate (its color is the ambient light), LightOccluder2D polygons cast shadows from lights with shadow_enabled, and normal_texture on a Sprite2D or a tile set source adds bumps. A light reaches the canvases its range_layer_min to range_layer_max holds (the world is layer 0, a CanvasLayer its layer), and each canvas has its own CanvasModulate. light_sample returns how lit points or an entity are and which lights reach them, so you can check lighting without a screenshot.
- Camera2D picks what the window shows, as Godot's: the first enabled one in drawing order is current, so one in the player's Transform2D children follows the player. zoom, the limits, drag margins and smoothing shape the view; controls stay put on the screen. frame_summary's camera gives the world rect the window shows.
- CanvasLayer draws what its children list in a canvas of its own, as Godot's: a HUD that stays put over the world, or a background under it (layer -1). Parallax2D scrolls its entity at scroll_scale times the camera's speed; repeat_size draws what it holds again to fill the window and autoscroll drifts it. canvas_convert turns window pixels, such as a click, into the world through the camera, or back.
- 2D physics is Godot's: StaticBody2D, AnimatableBody2D, RigidBody2D, CharacterBody2D and Area2D take their shapes from CollisionShape2D or CollisionPolygon2D, and collide by collision_layer and collision_mask. A CharacterBody2D moves by its velocity, as move_and_slide, and reports on_floor and on_wall. Area2D space overrides change gravity and damping inside them (physics_gravity reports a body's), and a StaticBody2D's constant velocities make a conveyor. After a step, physics_raycast, physics_overlap and physics_contacts say what is where and what touches what.
- Navigation is Godot's: NavigationRegion2D and 3D place .navpoly.json and .navmesh.json files of convex polygons (schema_get kinds "navpoly", "navmesh"), a TileMapLayer's cells are regions of its tiles' navigation outlines, and NavigationLink2D and 3D join two points, such as a ladder's ends. navigation_path finds a path. A NavigationAgent2D or 3D finds one to its target_position while gameplay code moves it; navigation_agent shows its progress. Agents with avoidance_enabled steer round each other and NavigationObstacle2D and 3D: gameplay code sets the agent's velocity and moves it at the safe velocity in its velocity_computed event.
- NPCs plan, as F.E.A.R.'s goal-oriented action planning: a .npc.json brain (schema_get kind "npc") names facts, goals (when, want, priority) and actions (needs, effects, cost, and do: wait, go_to an entity or emit an event). An NpcAgent names its brain and keeps its facts, goal and plan; gameplay code answers an emit action's event by setting facts. npc_plan shows what an agent would choose and why; npc_draft has a model write or revise a brain from a brief and storyboard entries, checked before it is written.
)"
    // MSVC takes string literals of at most 16380 bytes, so the text comes in pieces.
    R"(- CSG shapes are Godot's: CSG shapes in a CSG shape's Transform3D children add to it, cut it or intersect it by their operation, in order; only the root draws the result, and with use_collision it is a static body. csg_get returns a root's triangles, volume and problems.
- Path2D and Path3D are Godot's curves; a PathFollow2D or PathFollow3D in a path's Transform children sits progress along it, turned along it. Set progress, or animate PathFollow3D/progress. A CSGPolygon3D in mode "path" pushes its polygon along its path_node's Path3D: rails, pipes, roads. path_get samples a curve and finds its nearest point.
- A GridMap is Godot's: items of a .meshlib.json library (schema_get kind meshlib) in cells written as legend keys and rows per floor, "name:orientation" for one of Godot's 24 turns. Change cells with gridmap_set (cells or a box), read them with gridmap_get; physics_raycast names the cell it hits.
- An HTerrain is the HTerrain plugin's heightmap terrain: heights from a .height.png file, a splat map weighing up to 4 textures and a color map whose alpha cuts holes; map files that don't exist yet are flat, the first texture and white. Make heights with terrain_generate, paint with terrain_brush (raise, lower, smooth, flatten, level, splat, color, hole), read heights, normals and textures at points with terrain_get; never write the maps by hand.
- CPUParticles2D emits particles as Godot's: an emission shape, a direction and spread, gravity and the rest as _min/_max ranges with curves over each particle's life, and color_ramp. particles_eval returns how many are alive, where they are and, with list, each one, now or after a time, so you can check particles without a screenshot. A one_shot emits a finished event when its last particle dies. CPUParticles3D is the same in meters, drawing the entity's mesh at each particle; a QuadMesh with a StandardMaterial3D billboard_mode "particles" faces the camera.
- UI is Godot's Control nodes: Control places a rect in its parent's (or the window's) by anchors and offsets and lists its children; containers (BoxContainer, GridContainer, MarginContainer, CenterContainer, FlowContainer, PanelContainer, ScrollContainer) place their children instead; Label, ColorRect, Panel, TextureRect, NinePatchRect and ProgressBar draw in it, above the world. Themes are .theme.json files (schema_get kind "theme") set with Control.theme or project.json's gui.theme; theme_override_* sets one item, theme_override_styles a style box. Fonts are .ttf, .otf or .font.json files (schema_get kind "font"). A .font.json file's fallbacks draw the characters its font lacks; Control.layout_direction rtl mirrors a control and its children, and right-to-left text orders itself. ui_layout returns each control's rect and the lines labels show, ui_theme the theme items a control uses and where each comes from, and text_measure measures text, so you can check UI without a screenshot.
- Input is Godot's: project.json's "input" binds actions such as "jump" to keys, mouse buttons and gamepad buttons and sticks, over the built-in ui_* actions, and gameplay code reads actions. To play, input_inject events (keys, clicks, touches, sticks or actions by name) then step: a press and a release before one step is a tap. input_state shows what the game saw and input_actions the bindings.
- Button, CheckBox, CheckButton and TextureButton are Godot's: a click or tap, ui_accept while focused or a shortcut presses them, and they emit pressed, toggled, button_down and button_up events; toggle_mode makes a switch and button_group a set of options (CheckBox and CheckButton toggle, drawn as a box or a switch). ui_press presses one at the next step and ui_focus gives one focus; or input_inject a click, Tab or arrow keys and step. ui_layout reports each button's draw_mode and which control has focus and is hovered.
- Slider is Godot's HSlider, or VSlider with vertical: a click or drag, the wheel, arrow keys or a stick set its value between min_value and max_value by step, emitting value_changed, drag_started and drag_ended; ui_layout reports its value and ratio.
- ScrollContainer is Godot's: it shows its child through its rect, with scroll bars when the child doesn't fit (horizontal_scroll_mode and vertical_scroll_mode), and scroll_horizontal and scroll_vertical say how far it scrolled; the wheel, its bars and a finger drag scroll it, and follow_focus scrolls to the control with focus. Control.clip_contents cuts a control's drawing and the clicks its children get at its rect, as a ScrollContainer always does. ScrollBar is a scroll bar of its own (HScrollBar, or VScrollBar with vertical). ui_layout reports each ScrollContainer's scroll, scroll_max and scroll_bars and a clipped control's shown_rect, and frame_summary lists controls clipped away.
- LineEdit is Godot's text field: a click, Tab or ui_focus starts editing it; input_inject {"type": "text", "text": "..."} types at the caret, {"type": "ime"} events compose as an input method does, and ui_text_* actions, ui_copy, ui_paste and ui_undo edit; Enter emits text_submitted. ui_layout reports its caret_column and selection, and ui_clipboard reads the clipboard. TextEdit is the same for several lines: Enter types a new line, Tab indents when tab_input_mode is on, and ui_layout reports its caret and selection as [line, column] and the lines in view. A CodeEdit beside it adds line numbers and breakpoints (breakpointed_lines), whose gutters ui_layout reports.
- Sound is Godot's: AudioStreamPlayer, AudioStreamPlayer2D and AudioStreamPlayer3D play .wav, .ogg or .mp3 files, or .audio.json files that loop a sound or pick one of several at random (schema_get kind "audio"), through buses with effects from a .bus_layout.json file (schema_get kind "bus_layout") named by project.json's audio.bus_layout. audio_play and audio_stop play and stop one at the next step; audio_state shows the buses' levels and what each player plays, and audio_capture measures what a bus played (peak and RMS dB, slice by slice), so you can check sound without listening.
- frame_summary says what the window shows as data: each sprite, tile map layer, particle emitter and control that shows, with its rect, its text and how much of it shows; what is covered, offscreen, clear or clipped away; and what can't draw (a missing texture). "map": [columns, rows] adds a text picture of the frame, "ids" and "region" narrow it, and "contrast": true lists what is hard to see against what is around it. Use it to check what a player sees instead of an image.
- frame_capture draws what the window shows, the same on every machine: "points" returns pixel colors (cheapest), "image": true a PNG you can look at (keep it small with "region" and "scale") and "path" saves one; "collisions": true draws the collision shapes too. Prefer the checks above when they answer the question. lighting_check measures a 3D frame's exposure and what each light lights, and names lights that light nothing in view or clip.
- In a running game, perf_stats reports fps and frame times, display_set changes V-Sync and the frame cap, and debug_overlay shows the person watching a panel of stats and the collision shapes.
- The game's own components and systems are C++ in the folders project.json's "gameplay" "sources" names (src/), registered in its SLASH_GAME_MODULE function. Before calling engine C++ you haven't seen, api_search it by name or by what it does: it returns the real signature, doc and header, and lines of verified examples that call it. After each edit, call build_check: it compiles only the files that changed and returns errors and warnings with file, line and the compiler's fix. Then call gameplay_build: it swaps the new code into the open world, which keeps its entities and the fields the code still has. Keep gameplay state in registered components.
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

// Kept to the commands nearly every session uses, so the tools fit every client and cost few
// tokens; the bench's setup counts them.
constexpr std::array<std::string_view, 45> kCoreCommands{
    "engine.describe",
    "schema.get",
    "project.map",
    "api.search",
    "scene.new",
    "scene.open",
    "scene.save",
    "scene.get",
    "entity.create",
    "entity.get",
    "entity.destroy",
    "component.set",
    "component.remove",
    "world.query",
    "prefab.save",
    "batch",
    "undo",
    "redo",
    "world.snapshot",
    "world.diff",
    "world.restore",
    "events.subscribe",
    "events.read",
    "step",
    "run.until",
    "play",
    "pause",
    "input.inject",
    "frame.summary",
    "frame.capture",
    "ui.layout",
    "physics.raycast",
    "tilemap.paint",
    "test.run",
    "playtest.run",
    "build.check",
    "gameplay.build",
    "log.tail",
    "storyboard.brief",
    "storyboard.query",
    "storyboard.edit",
    "storyboard.check",
    "asset.search",
    "asset.use",
    "asset.generate",
};

bool has(const Command& command, Capability capability) {
    return std::ranges::find(command.capabilities, capability) != command.capabilities.end();
}

// A command's group: its name up to the first dot ("ui" for ui.layout), or the whole name.
std::string_view group_of(std::string_view command) {
    return command.substr(0, command.find('.'));
}

// A failure's data: the JSON Pointer of the bad value.
json::Value error_data(std::string pointer) {
    json::Value data;
    data["path"] = std::move(pointer);
    return data;
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

std::span<const std::string_view> core_commands() {
    return kCoreCommands;
}

Result<Tools> parse_tools(std::string_view text) {
    if (text == "core") {
        return Tools::Core;
    }
    if (text == "all") {
        return Tools::All;
    }
    return Error{ErrorCode::InvalidArgument,
                 R"(tools must be "core" or "all", not ")" + std::string(text) + "\""};
}

Server::Server(Session& session, Tools tools) : session_(&session), offered_(tools) {
    for (const Command& command : session.commands().commands()) {
        const bool added = tools_.emplace(tool_name(command.name), &command).second;
        SLASH_ASSERT(added, "two commands map to the same MCP tool name");
    }
    SLASH_ASSERT(!tools_.contains(kLoadTool), "a command maps to the tools_load tool's name");
}

std::vector<std::string> Server::take_notifications() {
    return std::exchange(notifications_, {});
}

bool Server::offered(const Command& command) const {
    return offered_ == Tools::All ||
           std::ranges::find(kCoreCommands, command.name) != kCoreCommands.end() ||
           loaded_.contains(command.name);
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

Reply Server::initialize(const json::Value& params) const {
    std::string_view version = kProtocolVersions.front();
    if (const json::Value* requested = params.find("protocolVersion");
        requested != nullptr && requested->is_string() &&
        std::ranges::find(kProtocolVersions, requested->as_string()) != kProtocolVersions.end()) {
        version = requested->as_string();
    }
    json::Value result;
    result["protocolVersion"] = version;
    result["capabilities"]["tools"]["listChanged"] = offered_ == Tools::Core;
    result["capabilities"]["resources"]["listChanged"] = false;
    result["capabilities"]["resources"]["subscribe"] = false;
    result["serverInfo"]["name"] = "slashengine";
    result["serverInfo"]["title"] = "SlashEngine";
    result["serverInfo"]["version"] = slash::version();
    result["instructions"] = kInstructions;
    return Reply::success(std::move(result));
}

json::Value Server::definition(const std::string& name, const Command& command) const {
    const bool read_only = command.effect == Effect::Read && !has(command, Capability::Edit) &&
                           !has(command, Capability::Run) &&
                           !has(command, Capability::Filesystem) &&
                           !has(command, Capability::Build) && !has(command, Capability::Network);
    json::Value tool;
    tool["name"] = name;
    tool["description"] = command.description;
    tool["inputSchema"] = command.params;
    tool["annotations"]["readOnlyHint"] = read_only;
    if (!read_only) {
        // Edits can be undone; replacing the scene, writing files or building cannot.
        tool["annotations"]["destructiveHint"] = command.effect == Effect::Replace ||
                                                 has(command, Capability::Filesystem) ||
                                                 has(command, Capability::Build);
    }
    // Only commands that reach the network, such as ai.complete, leave the project.
    tool["annotations"]["openWorldHint"] = has(command, Capability::Network);
    return tool;
}

Reply Server::list_tools() const {
    json::Array tools;
    if (offered_ == Tools::Core) {
        json::Value load;
        load["name"] = std::string(kLoadTool);
        load["description"] =
            "With no arguments, lists every engine command by group. With groups or commands, "
            "adds them as tools and returns their descriptions and params.";
        load["inputSchema"]["type"] = "object";
        load["inputSchema"]["properties"]["groups"]["type"] = "array";
        load["inputSchema"]["properties"]["groups"]["items"]["type"] = "string";
        load["inputSchema"]["properties"]["groups"]["description"] =
            R"(Groups to load, e.g. "ui" or "audio".)";
        load["inputSchema"]["properties"]["commands"]["type"] = "array";
        load["inputSchema"]["properties"]["commands"]["items"]["type"] = "string";
        load["inputSchema"]["properties"]["commands"]["description"] =
            R"(Commands to load, e.g. "ui.layout".)";
        load["inputSchema"]["additionalProperties"] = false;
        load["annotations"]["readOnlyHint"] = true;
        load["annotations"]["openWorldHint"] = false;
        tools.push_back(std::move(load));
    }
    for (const auto& [name, command] : tools_) {
        if (offered(*command)) {
            tools.push_back(definition(name, *command));
        }
    }
    json::Value result;
    result["tools"] = std::move(tools);
    return Reply::success(std::move(result));
}

Reply Server::load_tools(const json::Value& arguments) {
    if (!arguments.is_null() && !arguments.is_object()) {
        return Reply::failure(code::kInvalidParams, "tools_load takes an object");
    }
    std::vector<std::string_view> groups;
    std::vector<std::string_view> names;
    for (const auto& [name, command] : tools_) {
        if (std::ranges::find(groups, group_of(command->name)) == groups.end()) {
            groups.push_back(group_of(command->name));
        }
        names.emplace_back(command->name);
    }
    std::vector<const Command*> wanted;
    const auto add = [&](const Command& command) {
        if (std::ranges::find(wanted, &command) == wanted.end()) {
            wanted.push_back(&command);
        }
    };
    const std::array<std::string_view, 2> keys{"groups", "commands"};
    bool asked = false;
    if (arguments.is_object()) {
        for (const auto& [key, value] : arguments.as_object()) {
            if (std::ranges::find(keys, key) == keys.end()) {
                return Reply::failure(code::kInvalidParams,
                                      "tools_load takes groups and commands, not \"" + key + "\"",
                                      error_data("/" + key));
            }
        }
    }
    for (std::size_t k = 0; k < keys.size(); ++k) {
        const json::Value* list = arguments.is_object() ? arguments.find(keys[k]) : nullptr;
        if (list == nullptr) {
            continue;
        }
        const std::string pointer = "/" + std::string(keys[k]);
        if (!list->is_array()) {
            return Reply::failure(code::kInvalidParams, std::string(keys[k]) + " must be an array",
                                  error_data(pointer));
        }
        asked = true;
        for (std::size_t i = 0; i < list->size(); ++i) {
            const json::Value& item = list->as_array()[i];
            const std::string at = pointer + "/" + std::to_string(i);
            if (!item.is_string()) {
                return Reply::failure(code::kInvalidParams, "must be a string", error_data(at));
            }
            // A tool name ("ui_layout") names its command too.
            std::string wanted_name = item.as_string();
            if (k == 1) {
                if (const auto tool = tools_.find(wanted_name); tool != tools_.end()) {
                    wanted_name = tool->second->name;
                }
            }
            bool found = false;
            for (const auto& [name, command] : tools_) {
                if ((k == 0 && group_of(command->name) == wanted_name) ||
                    (k == 1 && command->name == wanted_name)) {
                    add(*command);
                    found = true;
                }
            }
            if (!found) {
                std::string message = std::string(k == 0 ? "no group" : "no command") + " \"" +
                                      item.as_string() + "\"";
                if (const std::string_view hint =
                        closest_match(item.as_string(), k == 0 ? groups : names);
                    !hint.empty()) {
                    message += "; did you mean \"" + std::string(hint) + "\"?";
                }
                return Reply::failure(code::kInvalidParams, std::move(message), error_data(at));
            }
        }
    }

    json::Value result;
    if (!asked) {
        // The directory: each group's commands, by name only; loading gives the rest.
        json::Value directory = json::Object{};
        for (const auto& [name, command] : tools_) {
            json::Value& group = directory[std::string(group_of(command->name))];
            if (group.is_null()) {
                group = json::Array{};
            }
            group.push_back(command->name);
        }
        result["groups"] = std::move(directory);
        return Reply::success(std::move(result));
    }
    json::Array added;
    bool changed = false;
    for (const Command* command : wanted) {
        if (!offered(*command)) {
            loaded_.insert(command->name);
            changed = true;
        }
        json::Value tool = definition(tool_name(command->name), *command);
        tool.as_object().erase("annotations");
        added.push_back(std::move(tool));
    }
    if (changed) {
        notifications_.emplace_back(
            R"({"jsonrpc":"2.0","method":"notifications/tools/list_changed"})");
    }
    result["tools"] = std::move(added);
    return Reply::success(std::move(result));
}

Reply Server::call_tool(const json::Value& params) {
    const json::Value* name = params.find("name");
    if (name == nullptr || !name->is_string()) {
        return Reply::failure(code::kInvalidParams, "tools/call needs a tool name");
    }
    if (name->as_string() == kLoadTool && offered_ == Tools::Core) {
        const json::Value* arguments = params.find("arguments");
        Reply loaded = load_tools(arguments != nullptr ? *arguments : json::Value());
        json::Value result;
        if (loaded.ok) {
            result["content"] =
                json::Array{text_content(json::write(loaded.value, json::Style::Compact))};
            result["structuredContent"] = std::move(loaded.value);
            result["isError"] = false;
        } else {
            json::Value error;
            error["code"] = loaded.code;
            error["message"] = std::move(loaded.message);
            if (!loaded.data.is_null()) {
                error["data"] = std::move(loaded.data);
            }
            json::Value wrapped;
            wrapped["error"] = std::move(error);
            result["content"] =
                json::Array{text_content(json::write(wrapped, json::Style::Compact))};
            result["isError"] = true;
        }
        return Reply::success(std::move(result));
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
