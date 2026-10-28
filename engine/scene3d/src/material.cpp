#include "slash/scene3d/material.hpp"

#include "model_internal.hpp"

#include "slash/render/image.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/serialize.hpp"
#include "slash/world/world.hpp"

#include <memory>
#include <string>
#include <utility>

namespace slash::scene3d {

namespace {

const world::ComponentRegistry& components() {
    static const std::unique_ptr<const world::ComponentRegistry> registry = [] {
        auto made = std::make_unique<world::ComponentRegistry>();
        world::register_builtin_components(*made);
        return std::unique_ptr<const world::ComponentRegistry>(std::move(made));
    }();
    return *registry;
}

json::Value make_schema() {
    world::SchemaBuilder builder;
    (void)components().find("StandardMaterial3D")->schema(builder);
    json::Value schema = builder.definition("StandardMaterial3D");
    schema["$defs"] = builder.definitions();
    schema["$schema"] = "https://json-schema.org/draft/2020-12/schema";
    schema["title"] = "SlashEngine material";
    schema["description"] =
        "A StandardMaterial3D in a file of its own, as Godot saves one as a resource, which model "
        "recipes' materials name so models share it. Texture paths are the project's.";
    schema["required"] = json::Array{"format"};
    schema["properties"]["format"]["const"] = 1;
    schema["properties"]["format"]["description"] = "Format version. Must be 1.";
    return schema;
}

} // namespace

json::Value standard_material_schema() {
    static const json::Value schema = make_schema();
    return schema;
}

std::vector<json::ValidationError> validate_standard_material(const json::Value& doc) {
    std::vector<json::ValidationError> errors = json::validate(doc, standard_material_schema());
    if (!errors.empty()) {
        return errors;
    }
    world::World scratch{components()};
    world::StandardMaterial3D standard;
    std::string path;
    if (auto read = world::from_json(scratch, standard_material_fields(doc), standard, path);
        !read) {
        errors.push_back({path, read.error().message});
    }
    return errors;
}

Result<world::StandardMaterial3D> parse_standard_material(const json::Value& doc) {
    const std::vector<json::ValidationError> errors = validate_standard_material(doc);
    if (!errors.empty()) {
        return make_error(ErrorCode::InvalidArgument,
                          (errors.front().path.empty() ? "" : errors.front().path + ": ") +
                              errors.front().message);
    }
    world::World scratch{components()};
    world::StandardMaterial3D standard;
    std::string path;
    if (auto read = world::from_json(scratch, standard_material_fields(doc), standard, path);
        !read) {
        return read.error();
    }
    return standard;
}

json::Value standard_material_fields(const json::Value& doc) {
    json::Value fields = doc.is_object() ? doc : json::Value(json::Object{});
    fields.as_object().erase("format");
    return fields;
}

std::vector<json::ValidationError>
check_material_textures(const json::Value& fields, const ModelReader& read, std::string_view at) {
    std::vector<json::ValidationError> errors;
    if (!fields.is_object() || !read) {
        return errors;
    }
    for (const std::string_view field : kMaterialTextureFields) {
        const json::Value* file = fields.find(field);
        if (file == nullptr || !file->is_string() || file->as_string().empty()) {
            continue;
        }
        std::string where(at);
        where += '/';
        where += field;
        auto text = read(file->as_string());
        if (!text) {
            errors.push_back({std::move(where), quoted(file->as_string()) +
                                                    " could not be read: " + text.error().message});
        } else if (auto image = render::decode_image(text.value()); !image) {
            errors.push_back({std::move(where), quoted(file->as_string()) +
                                                    " doesn't decode: " + image.error().message});
        }
    }
    return errors;
}

std::vector<json::ValidationError> check_standard_material(std::string_view text,
                                                           const ModelReader& read) {
    auto doc = json::parse(text);
    if (!doc) {
        return {{"", doc.error().message}};
    }
    std::vector<json::ValidationError> errors = validate_standard_material(doc.value());
    if (!errors.empty()) {
        return errors;
    }
    return check_material_textures(doc.value(), read, "");
}

} // namespace slash::scene3d
