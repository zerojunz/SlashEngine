#include "slash/command/lightmap.hpp"

#include "slash/command/session.hpp"
#include "slash/core/text.hpp"
#include "slash/render/image.hpp"
#include "slash/scene3d/lightmap.hpp"
#include "slash/scene3d/model.hpp"
#include "slash/world/components.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace slash::command {
namespace {

json::Value property(std::string_view type, std::string_view description) {
    json::Value schema;
    schema["type"] = type;
    schema["description"] = description;
    return schema;
}

json::Value params_schema(json::Object properties, json::Array required = {}) {
    json::Value schema;
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["properties"] = std::move(properties);
    if (!required.empty()) {
        schema["required"] = std::move(required);
    }
    return schema;
}

Reply param_error(int error_code, std::string_view path, const std::string& message) {
    json::Value data;
    data["path"] = path;
    return Reply::failure(error_code, std::string(path) + ": " + message, std::move(data));
}

std::string quoted(std::string_view name) {
    return "\"" + std::string(name) + "\"";
}

Reply unknown_entity(const world::World& world, std::string_view id, std::string_view path) {
    std::string message = "no entity has the id " + quoted(id);
    if (const std::string_view hint = closest_match(id, world.ids()); !hint.empty()) {
        message += "; did you mean " + quoted(hint) + "?";
    }
    return param_error(code::kNotFound, path, message);
}

// Where a LightmapGI with no light_data bakes to, as Godot's: beside the scene, named after it,
// or after the entity when the scene isn't saved.
std::string default_path(const Session& session, std::string_view id) {
    std::string_view scene = session.scene_path();
    if (scene.ends_with(".scene.json")) {
        scene.remove_suffix(std::string_view(".scene.json").size());
        return std::string(scene) + std::string(scene3d::kLightmapSuffix);
    }
    return std::string(id) + std::string(scene3d::kLightmapSuffix);
}

Reply lightmap_bake(Session& session, const json::Value& params) {
    world::World& world = session.world();
    const std::string& id = params.find("id")->as_string();
    const entt::entity entity = world.find(id);
    if (entity == entt::null) {
        return unknown_entity(world, id, "/id");
    }
    auto* gi = world.ecs().try_get<world::LightmapGI>(entity);
    if (gi == nullptr) {
        return param_error(code::kInvalidParams, "/id", quoted(id) + " has no LightmapGI");
    }
    std::string path;
    std::string_view pointer = "/id";
    if (const json::Value* file = params.find("file")) {
        path = file->as_string();
        pointer = "/file";
    } else if (!gi->light_data.empty()) {
        path = gi->light_data;
    } else {
        path = default_path(session, id);
    }
    if (!path.ends_with(scene3d::kLightmapSuffix)) {
        return param_error(code::kInvalidArgument, pointer,
                           quoted(path) + " is not a " + std::string(scene3d::kLightmapSuffix) +
                               " file");
    }

    asset::Assets& assets = session.assets();
    const scene3d::LightmapSources sources{
        .images =
            [&assets](const std::string& image) -> Result<std::shared_ptr<const render::Image>> {
            auto loaded = assets.load<render::Image>(image);
            if (!loaded) {
                return loaded.error();
            }
            return loaded.value().share();
        },
        .models =
            [&assets](const std::string& model) -> Result<std::shared_ptr<const scene3d::Model>> {
            auto loaded = assets.load<scene3d::Model>(model);
            if (!loaded) {
                return loaded.error();
            }
            return loaded.value().share();
        },
    };
    auto baked = scene3d::bake_lightmap(world, entity, sources);
    if (!baked) {
        return Reply::failure(baked.error());
    }
    scene3d::LightmapBake& bake = baked.value();
    const std::string texture = scene3d::lightmap_texture_path(path);
    bake.data.texture = texture;
    std::string data_text = json::write(scene3d::lightmap_json(bake.data)) + "\n";
    auto texels = render::encode_hdr(bake.texels);
    if (!texels) {
        return Reply::failure(texels.error());
    }

    json::Value result;
    result["file"] = path;
    result["texture"] = texture;
    result["meshes"] = bake.meshes;
    result["lights"] = bake.lights;
    result["size"] = json::Array{bake.data.width, bake.data.height};
    result["slices"] = bake.data.slices;
    if (!bake.problems.empty()) {
        json::Array problems;
        for (const scene3d::Problem& problem : bake.problems) {
            json::Value item;
            if (problem.entity != entt::null) {
                item["id"] = world.id_of(problem.entity);
            }
            if (!problem.path.empty()) {
                item["path"] = problem.path;
            }
            item["message"] = problem.message;
            problems.emplace_back(std::move(item));
        }
        result["problems"] = std::move(problems);
    }

    auto data_before = session.read_project_file(path);
    if (!data_before) {
        return Reply::failure(data_before.error());
    }
    auto texels_before = session.read_project_file(texture);
    if (!texels_before) {
        return Reply::failure(texels_before.error());
    }
    const bool same_files =
        data_before.value() == data_text && texels_before.value() == texels.value();
    if (same_files && gi->light_data == path) {
        session.mark_unchanged();
        result["unchanged"] = true;
        return Reply::success(std::move(result));
    }
    gi->light_data = path;
    if (data_before.value() != data_text) {
        if (auto staged = session.stage_file(path, data_text); !staged) {
            return Reply::failure(staged.error());
        }
        session.add_undo_file(path, std::move(data_before).value(), std::move(data_text));
    }
    if (texels_before.value() != texels.value()) {
        if (auto staged = session.stage_file(texture, texels.value()); !staged) {
            return Reply::failure(staged.error());
        }
        session.add_undo_file(texture, std::move(texels_before).value(), std::move(texels).value());
    }
    return Reply::success(std::move(result));
}

} // namespace

void register_lightmap_commands(Registry& registry) {
    using enum Capability;
    json::Value id = property("string", "An entity with a LightmapGI.");
    id["minLength"] = 1;
    json::Value file =
        property("string", "The .lightmap.json file to write, project path. Default: the "
                           "LightmapGI's light_data, else beside the scene, named after it.");
    file["minLength"] = 1;
    registry.add({
        .name = "lightmap.bake",
        .description = "Bakes a LightmapGI's light into a .lightmap.json file and its .hdr "
                       "texels, as Godot's LightmapGI bake: the static meshes under its parent, "
                       "lit by the lights whose light_bake_mode isn't disabled and its "
                       "environment, with shadows and bounces. Sets light_data. Returns the "
                       "files, counts, size and problems.",
        .capabilities = {Edit, Filesystem},
        .effect = Effect::Edit,
        .params = params_schema({{"id", std::move(id)}, {"file", std::move(file)}}, {"id"}),
        .handler = lightmap_bake,
    });
}

} // namespace slash::command
