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
- events_subscribe, then events_read, tells you what changed and what the game emitted since you last looked, without re-querying.
- Add "dry_run": true to any edit or batch to see what it would change without changing anything. world_snapshot, then world_diff later, lists every entity and field that changed; world_restore goes back.
- world_query returns only entity IDs unless you pass include. Filter with where, e.g. {"component": "Transform2D", "field": "position.y", "op": ">", "value": 100}.
- component_set returns only the fields that changed.
- To test gameplay, run_until steps the game until an entity matches with/where (or none does) and returns the tick and the matches, instead of a step and a query per tick.
- The storyboard holds the game's facts (characters, places, story, rules, names, decisions, open work). Before a task, storyboard_brief with the scenes or entities it touches, and use those facts instead of guessing; record new ones with storyboard_edit (pass the version you read); after it, storyboard_check.
- Sprite2D draws a texture or one frame of a sprite sheet; AnimatedSprite2D plays an animation from a .frames.json file on it (schema_get kind "frames"). Sprite2D's material is a .material.json node graph (schema_get kind "material"); material_eval returns the colors it draws, so you can check it without a screenshot.
- AnimationPlayer plays keyframe animations from a .anim.json file (schema_get kind "animation"): tracks set any component field, such as "Sprite2D/modulate" or "Transform2D/position/y", and event tracks emit events. animation_eval returns the values at any times, so you can check an animation without playing it.
- TileMapLayer draws a grid of tiles from a .tileset.json file (schema_get kind "tileset"); its cells are rows of one-letter legend keys. tilemap_paint paints a terrain such as "grass" on cells and picks the tiles whose edges connect; tilemap_set places one tile; tilemap_get returns the rows.
- 2D lights are Godot's: PointLight2D and DirectionalLight2D light sprites and tile layers once the scene has a light or a CanvasModulate (its color is the ambient light), LightOccluder2D polygons cast shadows from lights with shadow_enabled, and Sprite2D normal_texture adds bumps. light_sample returns how lit points or an entity are and which lights reach them, so you can check lighting without a screenshot.
- 2D physics is Godot's: StaticBody2D, AnimatableBody2D, RigidBody2D, CharacterBody2D and Area2D take their shapes from CollisionShape2D or CollisionPolygon2D, and collide by collision_layer and collision_mask. A CharacterBody2D moves by its velocity, as move_and_slide, and reports on_floor and on_wall. After a step, physics_raycast, physics_overlap and physics_contacts say what is where and what touches what.
- In a running game, perf_stats reports fps and frame times, and display_set changes V-Sync and the frame cap.
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
        result["content"] =
            json::Array{text_content(json::write(reply.value, json::Style::Compact))};
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
