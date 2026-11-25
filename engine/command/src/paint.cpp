// paint.edit, paint.bake and paint.preview: paint stacks (.paint.json, slash/procedural/paint.hpp)
// changed, baked into a material's textures and drawn, as text and numbers; and texture.check's
// checks of them.

#include "slash/procedural/paint.hpp"

#include "procedural.hpp"

#include "slash/canvas/canvas.hpp"
#include "slash/command/session.hpp"
#include "slash/core/math.hpp"
#include "slash/core/text.hpp"
#include "slash/procedural/texture.hpp"
#include "slash/render/image.hpp"
#include "slash/scene3d/mesh_maps.hpp"
#include "slash/scene3d/model.hpp"
#include "slash/storyboard/storyboard.hpp"
#include "slash/world/ragdoll.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

double rounded(double v) {
    return std::round(v * 1000.0) / 1000.0;
}

// A stack's file, read from the project (staged text first).
struct StackFile {
    std::optional<std::string> text;
    json::Value doc;
};

std::optional<Reply> read_stack(Session& session, const std::string& path, std::string_view at,
                                bool may_be_missing, StackFile& out) {
    if (!path.ends_with(procedural::kPaintSuffix)) {
        return param_error(code::kInvalidArgument, at,
                           "expected a .paint.json file, e.g. \"props/chest.paint.json\"");
    }
    auto text = session.read_project_file(path);
    if (!text) {
        return param_error(code::kInvalidArgument, at, text.error().message);
    }
    out.text = std::move(text).value();
    if (!out.text) {
        if (may_be_missing) {
            out.doc = json::Object{};
            return std::nullopt;
        }
        return param_error(code::kNotFound, at, "the project has no file " + quoted(path));
    }
    auto doc = json::parse(*out.text);
    if (!doc) {
        return param_error(code::kInvalidArgument, at,
                           quoted(path) + " is not valid JSON: " + doc.error().message);
    }
    if (!procedural::is_paint_stack(doc.value())) {
        return param_error(code::kInvalidArgument, at,
                           quoted(path) + R"( is not a paint stack ("type": "paint"))");
    }
    out.doc = std::move(doc).value();
    return std::nullopt;
}

// Where a stack's textures and material go: beside it, named after it.
std::string stem_of(std::string_view path) {
    return std::string(path.substr(0, path.size() - procedural::kPaintSuffix.size()));
}

// A stack ready to compile: what it extends merged in, and what it paints on.
struct Prepared {
    json::Value resolved;
    procedural::PaintSurface surface;
    std::string model;
    std::string sprite;
    float texel_density = 0.0F;
    std::vector<std::string> files;
};

Result<render::Image> read_image(Session& session, const std::string& path) {
    auto bytes = session.read_project_file(path);
    if (!bytes) {
        return bytes.error();
    }
    if (!bytes.value()) {
        return make_error(ErrorCode::NotFound, "the project has no file " + quoted(path));
    }
    return procedural::decode_texture(path, *bytes.value());
}

// What a stack paints on. write lets the model's mesh maps bake into their folder, as
// model.bake_maps writes them; without it a bake the folder doesn't hold stays in memory.
Result<Prepared> prepare(Session& session, const json::Value& doc, bool write) {
    Prepared out;
    auto resolved = procedural::resolve_paint(doc, graph_reader(session), &out.files);
    if (!resolved) {
        return resolved.error();
    }
    out.resolved = std::move(resolved).value();
    if (const auto errors = procedural::validate_paint(out.resolved); !errors.empty()) {
        return make_error(ErrorCode::InvalidArgument,
                          "with what it extends, at " +
                              (errors.front().path.empty() ? "its top" : errors.front().path) +
                              ": " + errors.front().message);
    }
    const json::Value& stack = out.resolved;
    if (const json::Value* sprite = stack.find("sprite"); sprite != nullptr) {
        out.sprite = sprite->as_string();
        auto image = read_image(session, out.sprite);
        if (!image) {
            return make_error(image.error().code, "/sprite: " + image.error().message);
        }
        std::optional<render::Image> normal_map;
        if (const json::Value* given = stack.find("normal_map"); given != nullptr) {
            auto normals = read_image(session, given->as_string());
            if (!normals) {
                return make_error(normals.error().code, "/normal_map: " + normals.error().message);
            }
            normal_map = std::move(normals).value();
        }
        float edge = 4.0F;
        if (const json::Value* given = stack.find("edge_width"); given != nullptr) {
            edge = static_cast<float>(given->as_double());
        }
        auto surface = procedural::paint_sprite_surface(image.value(),
                                                        normal_map ? &*normal_map : nullptr, edge);
        if (!surface) {
            return make_error(surface.error().code, "/sprite: " + surface.error().message);
        }
        out.surface = std::move(surface).value();
        return out;
    }
    const json::Value* model = stack.find("model");
    if (model == nullptr) {
        return make_error(ErrorCode::InvalidArgument,
                          "/model: a smart material (no model or sprite) doesn't bake by itself; "
                          "make a stack that extends it and names a model or a sprite");
    }
    out.model = model->as_string();
    if (!scene3d::is_model_path(out.model)) {
        return make_error(ErrorCode::InvalidArgument,
                          "/model: " + quoted(out.model) +
                              " isn't a model file: give a .gltf, .glb, .obj, .fbx, .blend or "
                              ".model.json file");
    }
    auto loaded = session.assets().load<scene3d::Model>(out.model);
    if (!loaded) {
        return make_error(loaded.error().code, "/model: " + loaded.error().message);
    }
    const scene3d::MeshMapMesh low = scene3d::mesh_map_mesh(*loaded.value());
    std::uint32_t width = 1024;
    std::uint32_t height = 1024;
    if (const json::Value* size = stack.find("size"); size != nullptr) {
        if (size->is_array()) {
            width = static_cast<std::uint32_t>(size->as_array()[0].as_int());
            height = static_cast<std::uint32_t>(size->as_array()[1].as_int());
        } else {
            width = static_cast<std::uint32_t>(size->as_int());
            height = width;
        }
    }
    const json::Value* maps = stack.find("maps");
    std::uint32_t padding = 4;
    if (maps != nullptr) {
        if (const json::Value* given = maps->find("padding"); given != nullptr) {
            padding = static_cast<std::uint32_t>(given->as_int());
        }
    }
    auto surface = scene3d::mesh_map_surface(low, width, height, padding);
    if (!surface) {
        return make_error(surface.error().code, "/model: " + surface.error().message);
    }
    const scene3d::MeshMapSurface& s = surface.value();
    procedural::PaintSurface& p = out.surface;
    p.width = width;
    p.height = height;
    p.covered = s.covered;
    const std::size_t texels = std::size_t{width} * height;
    p.position.resize(texels * 3);
    p.normal.resize(texels * 3);
    for (std::size_t i = 0; i < texels; ++i) {
        p.position[i * 3] = s.position[i].x;
        p.position[(i * 3) + 1] = s.position[i].y;
        p.position[(i * 3) + 2] = s.position[i].z;
        p.normal[i * 3] = s.normal[i].x;
        p.normal[(i * 3) + 1] = s.normal[i].y;
        p.normal[(i * 3) + 2] = s.normal[i].z;
    }
    p.min = {static_cast<float>(s.min.x), static_cast<float>(s.min.y), static_cast<float>(s.min.z)};
    p.max = {static_cast<float>(s.max.x), static_cast<float>(s.max.y), static_cast<float>(s.max.z)};
    p.part = s.part;
    p.material = s.material;
    p.part_names = low.part_names;
    p.material_names = low.material_names;
    out.texel_density = s.texel_density;

    // The mesh maps its masks read, baked as model.bake_maps bakes every map, so a bake of the
    // model at this size serves both.
    const std::vector<std::string> needs = procedural::paint_needs(stack);
    const json::Value* high = stack.find("high");
    std::vector<std::string> wanted;
    for (const std::string& map : needs) {
        if ((map == "height" || map == "normal") && high == nullptr) {
            continue; // flat without a high model
        }
        wanted.push_back(map);
    }
    if (wanted.empty()) {
        return out;
    }
    if (width != height) {
        return make_error(ErrorCode::InvalidArgument,
                          "/size: mesh maps bake square; give size as a number");
    }
    json::Value params;
    params["path"] = out.model;
    if (high != nullptr) {
        params["high"] = *high;
    }
    params["size"] = width;
    if (maps != nullptr) {
        for (const auto& [key, value] : maps->as_object()) {
            params[key] = value;
        }
    }
    std::map<std::string, render::Image, std::less<>> images;
    Reply baked = bake_model_maps(session, params, write, &images);
    if (!baked.ok) {
        return make_error(ErrorCode::InvalidArgument, "the model's mesh maps: " + baked.message);
    }
    for (const std::string& map : wanted) {
        const auto found = images.find(map);
        if (found == images.end()) {
            continue;
        }
        const render::Image& image = found->second;
        if (map == "normal") {
            p.normal_map.resize(texels * 3);
            for (std::size_t i = 0; i < texels; ++i) {
                for (std::size_t c = 0; c < 3; ++c) {
                    p.normal_map[(i * 3) + c] =
                        static_cast<float>(image.pixels[(i * 4) + c]) / 255.0F;
                }
            }
            continue;
        }
        std::vector<float> values(texels);
        for (std::size_t i = 0; i < texels; ++i) {
            values[i] = static_cast<float>(image.pixels[i * 4]) / 255.0F;
        }
        p.maps.emplace(map, std::move(values));
    }
    return out;
}

// A stack baked: its compiled form and each output's image (textures, then masks).
struct Baked {
    procedural::CompiledPaint compiled;
    std::vector<render::Image> images;
};

Result<Baked> bake(Session& session, const Prepared& prepared, const json::Object& overrides) {
    auto compiled =
        procedural::compile_paint(prepared.resolved, prepared.surface, graph_reader(session),
                                  overrides, &session.world().pool());
    if (!compiled) {
        return compiled.error();
    }
    auto images = procedural::bake_texture_graph(
        compiled.value().program, procedural::graph_device(), &session.world().pool());
    if (!images) {
        return images.error();
    }
    return Baked{std::move(compiled).value(), std::move(images).value()};
}

// The numbers a bake returns: each texture's measures and each mask's coverage.
void summarize(const Baked& baked, const Prepared& prepared, json::Value& result) {
    json::Value outputs = json::Object{};
    for (std::size_t i = 0; i < baked.compiled.outputs.size(); ++i) {
        outputs[baked.compiled.outputs[i]] =
            procedural::measure_paint(baked.images[i], prepared.surface);
    }
    result["outputs"] = std::move(outputs);
    if (!baked.compiled.mask_layers.empty()) {
        json::Value masks = json::Object{};
        for (std::size_t k = 0; k < baked.compiled.mask_layers.size(); ++k) {
            const render::Image& image = baked.images[baked.compiled.outputs.size() + k];
            masks[baked.compiled.mask_layers[k]] =
                procedural::measure_paint(image, prepared.surface).find("r")->as_double();
        }
        result["masks"] = std::move(masks);
    }
    if (prepared.texel_density > 0.0F) {
        result["texel_density"] = rounded(prepared.texel_density);
    }
}

json::Object overrides_of(const json::Value& params) {
    if (const json::Value* given = params.find("parameters"); given != nullptr) {
        return given->as_object();
    }
    return {};
}

// A failure from a stack's errors: the pointer into it, or the param.
Reply stack_error(const std::string& file, std::string_view at, const Error& error) {
    return param_error(
        error.code == ErrorCode::NotFound ? code::kNotFound : code::kInvalidArgument, at,
        quoted(file) + (error.message.starts_with('/') ? " at " : ": ") + error.message);
}

Result<void> stage(Session& session, const std::string& file, std::string bytes, bool& changed) {
    auto before = session.read_project_file(file);
    if (!before) {
        return before.error();
    }
    if (before.value() == bytes) {
        return {};
    }
    if (auto staged = session.stage_file(file, bytes); !staged) {
        return staged.error();
    }
    session.add_undo_file(file, std::move(before).value(), std::move(bytes));
    changed = true;
    return {};
}

Reply paint_bake(Session& session, const json::Value& params) {
    const std::string& path = params.find("path")->as_string();
    StackFile read;
    if (auto failed = read_stack(session, path, "/path", false, read)) {
        return *failed;
    }
    if (const auto errors = procedural::validate_paint(read.doc); !errors.empty()) {
        return param_error(code::kInvalidArgument, "/path",
                           quoted(path) + " at " +
                               (errors.front().path.empty() ? "its top" : errors.front().path) +
                               ": " + errors.front().message);
    }
    auto prepared = prepare(session, read.doc, true);
    if (!prepared) {
        return stack_error(path, "/path", prepared.error());
    }
    const json::Object overrides = overrides_of(params);
    auto baked = bake(session, prepared.value(), overrides);
    if (!baked) {
        return stack_error(path, overrides.empty() ? "/path" : "/parameters", baked.error());
    }
    const Prepared& p = prepared.value();
    const std::string stem = stem_of(path);
    json::Value result;
    result["path"] = path;
    result["size"] = json::Array{p.surface.width, p.surface.height};
    json::Value textures = json::Object{};
    bool changed = false;
    for (std::size_t i = 0; i < baked.value().compiled.outputs.size(); ++i) {
        const std::string& name = baked.value().compiled.outputs[i];
        auto png = render::encode_png(baked.value().images[i]);
        if (!png) {
            return Reply::failure(png.error());
        }
        std::string file = stem;
        file += "_";
        file += name;
        file += ".png";
        if (auto staged = stage(session, file, std::move(png).value(), changed); !staged) {
            return Reply::failure(staged.error());
        }
        textures[name] = file;
    }
    result["textures"] = textures;

    // A model's material points at the textures, keeping its other fields.
    if (!p.model.empty()) {
        std::string material = stem + ".standard_material.json";
        if (const json::Value* given = p.resolved.find("material"); given != nullptr) {
            material = given->as_string();
        }
        json::Value doc = json::Object{};
        auto before = session.read_project_file(material);
        if (!before) {
            return Reply::failure(before.error());
        }
        if (before.value()) {
            auto parsed = json::parse(*before.value());
            if (parsed && parsed.value().is_object()) {
                doc = std::move(parsed).value();
            }
        }
        doc["format"] = 1;
        const auto texture = [&](std::string_view name) -> const json::Value* {
            return textures.find(name);
        };
        if (const json::Value* albedo = texture("albedo")) {
            doc["albedo_texture"] = *albedo;
            doc["albedo_color"] = json::Array{1, 1, 1, 1};
        }
        const json::Value* orm = texture("orm");
        const json::Value* roughness = texture("roughness");
        const json::Value* metallic = texture("metallic");
        if (roughness != nullptr || orm != nullptr) {
            doc["roughness_texture"] = roughness != nullptr ? *roughness : *orm;
            doc["roughness_texture_channel"] = roughness != nullptr ? "red" : "green";
            doc["roughness"] = 1;
        }
        if (metallic != nullptr || orm != nullptr) {
            doc["metallic_texture"] = metallic != nullptr ? *metallic : *orm;
            doc["metallic_texture_channel"] = metallic != nullptr ? "red" : "blue";
            doc["metallic"] = 1;
        }
        if (const json::Value* normal = texture("normal")) {
            doc["normal_enabled"] = true;
            doc["normal_texture"] = *normal;
        }
        if (const json::Value* emission = texture("emission")) {
            doc["emission_enabled"] = true;
            doc["emission_texture"] = *emission;
            doc["emission"] = json::Array{0, 0, 0, 1};
        }
        if (auto staged = stage(session, material, json::write(doc), changed); !staged) {
            return Reply::failure(staged.error());
        }
        result["material"] = material;
    }
    summarize(baked.value(), p, result);
    if (!changed) {
        session.mark_unchanged();
        result["unchanged"] = true;
    }
    return Reply::success(std::move(result));
}

// What changed between two stacks: top-level fields by pointer, layers by name.
json::Array changes(const json::Value& before, const json::Value& after) {
    json::Array out;
    std::set<std::string> keys;
    for (const json::Value* doc : {&before, &after}) {
        if (doc->is_object()) {
            for (const auto& [key, value] : doc->as_object()) {
                keys.insert(key);
            }
        }
    }
    for (const std::string& key : keys) {
        const json::Value* a = before.find(key);
        const json::Value* b = after.find(key);
        if (a != nullptr && b != nullptr && *a == *b) {
            continue;
        }
        if (key != "layers") {
            out.emplace_back("/" + json::pointer_escape(key));
            continue;
        }
        std::map<std::string, const json::Value*> was;
        std::map<std::string, const json::Value*> is;
        for (const auto& [list, into] : {std::pair{a, &was}, std::pair{b, &is}}) {
            if (list != nullptr && list->is_array()) {
                for (const json::Value& layer : list->as_array()) {
                    if (const json::Value* name = layer.find("name"); name && name->is_string()) {
                        (*into)[name->as_string()] = &layer;
                    }
                }
            }
        }
        std::set<std::string> names;
        for (const auto& [name, layer] : was) {
            names.insert(name);
        }
        for (const auto& [name, layer] : is) {
            names.insert(name);
        }
        bool any = false;
        for (const std::string& name : names) {
            const auto x = was.find(name);
            const auto y = is.find(name);
            if (x != was.end() && y != is.end() && *x->second == *y->second) {
                continue;
            }
            out.emplace_back("/layers/" + json::pointer_escape(name));
            any = true;
        }
        if (!any) {
            out.emplace_back("/layers"); // the same layers in another order
        }
    }
    return out;
}

Reply paint_edit(Session& session, const json::Value& params) {
    const std::string& path = params.find("path")->as_string();
    StackFile read;
    if (auto failed = read_stack(session, path, "/path", true, read)) {
        return *failed;
    }
    json::Value doc = read.doc;
    if (const json::Value* patch = params.find("patch"); patch != nullptr) {
        doc = world::merge_patch(doc, *patch);
    }
    if (!read.text) {
        if (doc.find("format") == nullptr) {
            doc["format"] = 1;
        }
        if (doc.find("type") == nullptr) {
            doc["type"] = "paint";
        }
    }
    if (!procedural::is_paint_stack(doc)) {
        return param_error(code::kInvalidArgument, "/patch/type",
                           R"(a paint stack's type stays "paint")");
    }

    // Layers by name: a merge patch each, null removing one, new ones on top unless placed.
    if (const json::Value* edits = params.find("layers"); edits != nullptr) {
        json::Array layers;
        if (const json::Value* had = doc.find("layers"); had != nullptr && had->is_array()) {
            layers = had->as_array();
        }
        const auto index_of = [&](std::string_view name) -> std::optional<std::size_t> {
            for (std::size_t i = 0; i < layers.size(); ++i) {
                const json::Value* got = layers[i].find("name");
                if (got != nullptr && got->is_string() && got->as_string() == name) {
                    return i;
                }
            }
            return std::nullopt;
        };
        const auto names = [&] {
            std::vector<std::string> out;
            for (const json::Value& layer : layers) {
                if (const json::Value* got = layer.find("name"); got && got->is_string()) {
                    out.push_back(got->as_string());
                }
            }
            return out;
        };
        for (const auto& [name, edit] : edits->as_object()) {
            const std::string at = "/layers/" + json::pointer_escape(name);
            const std::optional<std::size_t> found = index_of(name);
            if (edit.is_null()) {
                if (!found) {
                    std::string message = "the stack has no layer " + quoted(name) + " to remove";
                    if (const std::string_view near = closest_match(name, names()); !near.empty()) {
                        message += " (did you mean " + quoted(near) + "?)";
                    }
                    return param_error(code::kInvalidArgument, at, message);
                }
                layers.erase(layers.begin() + static_cast<std::ptrdiff_t>(*found));
                continue;
            }
            if (!edit.is_object()) {
                return param_error(code::kInvalidParams, at,
                                   "expected a merge patch over the layer, or null");
            }
            json::Value patch = edit;
            std::optional<std::pair<bool, std::string>> place; // above?, the layer named
            for (const std::string_view key : {"above", "below"}) {
                if (const json::Value* where = patch.find(key); where != nullptr) {
                    if (!where->is_string()) {
                        return param_error(code::kInvalidParams, at + "/" + std::string(key),
                                           "expected a layer's name");
                    }
                    place = std::pair{key == "above", where->as_string()};
                    patch.as_object().erase(std::string(key));
                }
            }
            json::Value appended = json::Object{};
            for (const std::string_view key : {"add_strokes", "add_decals"}) {
                if (const json::Value* items = patch.find(key); items != nullptr) {
                    if (!items->is_array()) {
                        return param_error(code::kInvalidParams, at + "/" + std::string(key),
                                           "expected a list");
                    }
                    appended[key.substr(4)] = *items;
                    patch.as_object().erase(std::string(key));
                }
            }
            json::Value layer = found ? layers[*found] : json::Value(json::Object{{"name", name}});
            if (found) {
                layers.erase(layers.begin() + static_cast<std::ptrdiff_t>(*found));
            }
            layer = world::merge_patch(layer, patch);
            layer["name"] = name;
            for (const auto& [key, items] : appended.as_object()) {
                json::Value& list = layer[key];
                if (!list.is_array()) {
                    list = json::Array{};
                }
                for (const json::Value& item : items.as_array()) {
                    list.as_array().push_back(item);
                }
            }
            std::size_t insert = found ? std::min(*found, layers.size()) : layers.size();
            if (place) {
                const std::optional<std::size_t> other = index_of(place->second);
                if (!other) {
                    std::string message = "the stack has no layer " + quoted(place->second);
                    if (const std::string_view near = closest_match(place->second, names());
                        !near.empty()) {
                        message += " (did you mean " + quoted(near) + "?)";
                    }
                    return param_error(code::kInvalidArgument,
                                       at + (place->first ? "/above" : "/below"), message);
                }
                insert = place->first ? *other + 1 : *other;
            }
            layers.insert(layers.begin() + static_cast<std::ptrdiff_t>(insert), std::move(layer));
        }
        doc["layers"] = json::Value(std::move(layers));
    }

    if (std::vector<json::ValidationError> errors = procedural::validate_paint(doc);
        !errors.empty()) {
        for (json::ValidationError& error : errors) {
            error.message = "the stack it makes, at " +
                            (error.path.empty() ? std::string("its top") : error.path) + ": " +
                            error.message;
        }
        return Reply::invalid_params(errors);
    }

    json::Value result;
    result["path"] = path;
    result["changed"] = changes(read.doc, doc);
    json::Array names;
    if (const json::Value* layers = doc.find("layers"); layers != nullptr) {
        for (const json::Value& layer : layers->as_array()) {
            names.emplace_back(layer.find("name")->as_string());
        }
    }
    result["layers"] = std::move(names);

    // Measured when it paints something: what it extends and reads must be there and compile.
    const json::Value* measure = params.find("measure");
    const bool paints = doc.find("model") != nullptr || doc.find("sprite") != nullptr ||
                        doc.find("extends") != nullptr;
    if (paints && (measure == nullptr || measure->as_bool())) {
        auto prepared = prepare(session, doc, false);
        if (prepared) {
            auto baked = bake(session, prepared.value(), {});
            if (!baked) {
                return param_error(code::kInvalidArgument, "/layers",
                                   "the stack it makes: " + baked.error().message);
            }
            summarize(baked.value(), prepared.value(), result);
        } else if (doc.find("model") != nullptr || doc.find("sprite") != nullptr) {
            return param_error(code::kInvalidArgument, "/patch",
                               "the stack it makes: " + prepared.error().message);
        }
    }

    std::string written = json::write(doc);
    if (read.text && written == *read.text) {
        session.mark_unchanged();
    } else {
        if (auto staged = session.stage_file(path, written); !staged) {
            return Reply::failure(staged.error());
        }
        session.add_undo_file(path, read.text, std::move(written));
    }
    return Reply::success(std::move(result));
}

// ---- Preview ----

Vec3 looking(Vec3 from, Vec3 to) {
    const Vec3 d{to.x - from.x, to.y - from.y, to.z - from.z};
    const float across = std::sqrt((d.x * d.x) + (d.z * d.z));
    return {slash::atan2(d.y, across), slash::atan2(-d.x, -d.z), 0.0F};
}

json::Value vec3(Vec3 v) {
    return json::Array{static_cast<double>(v.x), static_cast<double>(v.y),
                       static_cast<double>(v.z)};
}

Result<void> called(const Reply& reply, std::string_view what) {
    if (reply.ok) {
        return {};
    }
    return make_error(ErrorCode::InvalidArgument, std::string(what) + ": " + reply.message);
}

// The model with its painted textures, from views round it: each frame side by side.
Result<render::Image> draw_model(Session& host, const Prepared& prepared, const Baked& baked,
                                 int views, std::array<std::uint32_t, 2> size) {
    SessionOptions options = host.options();
    options.capture_logs = false;
    options.write_files = false;
    std::erase(options.capabilities, Capability::Build);
    std::erase(options.capabilities, Capability::Network);
    Session stage(host.components(), host.commands(), options);
    json::Value surface;
    for (std::size_t i = 0; i < baked.compiled.outputs.size(); ++i) {
        const std::string& name = baked.compiled.outputs[i];
        auto made =
            procedural::make_texture(stage.world(), "paint_preview_" + name, baked.images[i]);
        if (!made) {
            return made.error();
        }
        if (name == "albedo") {
            surface["albedo_texture"] = made.value();
        } else if (name == "orm" || name == "roughness" || name == "metallic") {
            const bool packed = name == "orm";
            if (packed || name == "roughness") {
                surface["roughness_texture"] = made.value();
                surface["roughness_texture_channel"] = packed ? "green" : "red";
            }
            if (packed || name == "metallic") {
                surface["metallic_texture"] = made.value();
                surface["metallic_texture_channel"] = packed ? "blue" : "red";
                surface["metallic"] = 1;
            }
        } else if (name == "normal") {
            surface["normal_enabled"] = true;
            surface["normal_texture"] = made.value();
        } else if (name == "emission") {
            surface["emission_enabled"] = true;
            surface["emission_texture"] = made.value();
        }
    }
    const procedural::PaintSurface& s = prepared.surface;
    const Vec3 center{(s.min[0] + s.max[0]) / 2, (s.min[1] + s.max[1]) / 2,
                      (s.min[2] + s.max[2]) / 2};
    const Vec3 extent{s.max[0] - s.min[0], s.max[1] - s.min[1], s.max[2] - s.min[2]};
    const float radius =
        std::sqrt((extent.x * extent.x) + (extent.y * extent.y) + (extent.z * extent.z)) / 2;
    const std::array<std::pair<std::string, json::Value>, 4> entities = {{
        {"camera", json::Object{{"Camera3D", json::Object{{"current", true}}},
                                {"Transform3D", json::Object{}}}},
        {"key",
         json::Object{{"SunLight3D", json::Object{}},
                      {"Transform3D", json::Object{{"rotation", json::Array{-0.8, 0.6, 0.0}}}}}},
        {"environment",
         json::Object{{"Environment3D",
                       json::Object{{"background_mode", "color"},
                                    {"background_color", json::Array{0.5, 0.5, 0.5, 1.0}},
                                    {"ambient_light_source", "color"},
                                    {"ambient_light_color", json::Array{0.4, 0.4, 0.4, 1.0}}}}}},
        {"subject", json::Object{{"Model3D", json::Object{{"mesh", prepared.model}}},
                                 {"Surface3D", surface},
                                 {"Transform3D", json::Object{}}}},
    }};
    for (const auto& [id, components] : entities) {
        json::Value params;
        params["id"] = id;
        params["components"] = components;
        if (auto made = called(stage.call("entity.create", params), "making the stage"); !made) {
            return made.error();
        }
    }
    render::Image sheet;
    sheet.width = size[0] * static_cast<std::uint32_t>(views);
    sheet.height = size[1];
    sheet.pixels.assign(std::size_t{sheet.width} * sheet.height * 4, 0);
    const float reach = std::max(0.01F, radius) * 1.8F;
    for (int v = 0; v < views; ++v) {
        // Three quarters from the front, then round the model.
        const float angle =
            0.737F + (6.2831853F * static_cast<float>(v) / static_cast<float>(views));
        const Vec3 eye{center.x + (std::sin(angle) * 0.89F * reach), center.y + (0.45F * reach),
                       center.z + (std::cos(angle) * 0.89F * reach)};
        json::Value set;
        set["id"] = "camera";
        set["components"]["Transform3D"] =
            json::Object{{"position", vec3(eye)}, {"rotation", vec3(looking(eye, center))}};
        if (auto moved = called(stage.call("component.set", set), "moving the camera"); !moved) {
            return moved.error();
        }
        canvas::CaptureOptions capture;
        capture.viewport = Vec2{static_cast<real_t>(size[0]), static_cast<real_t>(size[1])};
        auto image = canvas::capture(stage.world(), stage.assets(), capture);
        if (!image) {
            return image.error();
        }
        const render::Image& frame = image.value().image;
        const std::uint32_t w = std::min(frame.width, size[0]);
        const std::uint32_t h = std::min(frame.height, size[1]);
        for (std::uint32_t y = 0; y < h; ++y) {
            const auto from = static_cast<std::size_t>(y) * frame.width * 4;
            const auto to = ((static_cast<std::size_t>(y) * sheet.width) +
                             (static_cast<std::size_t>(v) * size[0])) *
                            4;
            std::copy_n(frame.pixels.begin() + static_cast<std::ptrdiff_t>(from),
                        static_cast<std::ptrdiff_t>(w) * 4,
                        sheet.pixels.begin() + static_cast<std::ptrdiff_t>(to));
        }
    }
    return sheet;
}

// A painted sprite over the stage's gray.
render::Image draw_sprite(const render::Image& albedo) {
    render::Image out = albedo;
    for (std::size_t i = 0; i + 3 < out.pixels.size(); i += 4) {
        const int a = out.pixels[i + 3];
        for (std::size_t c = 0; c < 3; ++c) {
            out.pixels[i + c] = static_cast<std::uint8_t>(
                ((out.pixels[i + c] * a) + (128 * (255 - a)) + 127) / 255);
        }
        out.pixels[i + 3] = 255;
    }
    return out;
}

Reply paint_preview(Session& session, const json::Value& params) {
    const std::string& path = params.find("path")->as_string();
    StackFile read;
    if (auto failed = read_stack(session, path, "/path", false, read)) {
        return *failed;
    }
    if (const auto errors = procedural::validate_paint(read.doc); !errors.empty()) {
        return param_error(code::kInvalidArgument, "/path",
                           quoted(path) + " at " +
                               (errors.front().path.empty() ? "its top" : errors.front().path) +
                               ": " + errors.front().message);
    }
    auto prepared = prepare(session, read.doc, false);
    if (!prepared) {
        return stack_error(path, "/path", prepared.error());
    }
    const json::Object overrides = overrides_of(params);
    auto baked = bake(session, prepared.value(), overrides);
    if (!baked) {
        return stack_error(path, overrides.empty() ? "/path" : "/parameters", baked.error());
    }
    int views = 4;
    if (const json::Value* given = params.find("views"); given != nullptr) {
        views = static_cast<int>(given->as_int());
    }
    std::array<std::uint32_t, 2> size{160, 160};
    if (const json::Value* given = params.find("size"); given != nullptr) {
        size = {static_cast<std::uint32_t>(given->as_array()[0].as_int()),
                static_cast<std::uint32_t>(given->as_array()[1].as_int())};
    }
    json::Value result;
    result["path"] = path;
    summarize(baked.value(), prepared.value(), result);
    render::Image picture;
    if (prepared.value().surface.sprite) {
        picture = draw_sprite(baked.value().images.front());
        views = 1;
    } else {
        auto drawn = draw_model(session, prepared.value(), baked.value(), views, size);
        if (!drawn) {
            return Reply::failure(drawn.error());
        }
        picture = std::move(drawn).value();
    }
    // Each view's look in numbers.
    json::Array measured;
    const std::uint32_t view_width = picture.width / static_cast<std::uint32_t>(views);
    procedural::PaintSurface all;
    for (int v = 0; v < views; ++v) {
        render::Image frame;
        frame.width = view_width;
        frame.height = picture.height;
        frame.pixels.resize(std::size_t{view_width} * picture.height * 4);
        for (std::uint32_t y = 0; y < picture.height; ++y) {
            const auto from = ((static_cast<std::size_t>(y) * picture.width) +
                               (static_cast<std::size_t>(v) * view_width)) *
                              4;
            std::copy_n(picture.pixels.begin() + static_cast<std::ptrdiff_t>(from),
                        static_cast<std::ptrdiff_t>(view_width) * 4,
                        frame.pixels.begin() +
                            static_cast<std::ptrdiff_t>(std::size_t{y} * view_width * 4));
        }
        json::Value numbers = procedural::measure_paint(frame, all);
        numbers.as_object().erase("a");
        measured.emplace_back(std::move(numbers));
    }
    result["views"] = std::move(measured);
    if (const json::Value* out = params.find("out"); out != nullptr) {
        auto full = session.resolve_path(out->as_string());
        if (!full) {
            return param_error(code::kInvalidParams, "/out", full.error().message);
        }
        auto png = render::encode_png(picture);
        if (!png) {
            return Reply::failure(png.error());
        }
        if (auto written = session.write_file(full.value(), png.value()); !written) {
            return Reply::failure(written.error());
        }
        result["out"] = out->as_string();
        result["image_size"] = json::Array{picture.width, picture.height};
    }
    return Reply::success(std::move(result));
}

json::Value problem(const std::string& file, std::string path, std::string message) {
    json::Value item;
    item["file"] = file;
    item["path"] = std::move(path);
    item["message"] = std::move(message);
    return item;
}

void add_error(const std::string& file, const Error& error, json::Array& errors) {
    const std::string& message = error.message;
    const std::size_t colon = message.find(": ");
    const bool pointed = message.starts_with('/') && colon != std::string::npos;
    errors.push_back(problem(file, pointed ? message.substr(0, colon) : std::string(),
                             pointed ? message.substr(colon + 2) : message));
}

} // namespace

bool check_paint_file(Session& session, const std::string& file, json::Array& errors) {
    StackFile read;
    if (read_stack(session, file, "/paths", false, read)) {
        return false;
    }
    std::vector<json::ValidationError> invalid = procedural::validate_paint(read.doc);
    if (!invalid.empty()) {
        for (json::ValidationError& error : invalid) {
            errors.push_back(problem(file, std::move(error.path), std::move(error.message)));
        }
        return true;
    }
    if (read.doc.find("model") == nullptr && read.doc.find("sprite") == nullptr) {
        // A smart material: what it extends must be there, and itself valid.
        if (auto resolved = procedural::resolve_paint(read.doc, graph_reader(session)); !resolved) {
            add_error(file, resolved.error(), errors);
        }
        return true;
    }
    auto prepared = prepare(session, read.doc, false);
    if (!prepared) {
        add_error(file, prepared.error(), errors);
        return true;
    }
    auto baked = bake(session, prepared.value(), {});
    if (!baked) {
        add_error(file, baked.error(), errors);
        return true;
    }
    const json::Value* expect = read.doc.find("expect");
    if (expect == nullptr) {
        return true;
    }
    json::Value measured;
    summarize(baked.value(), prepared.value(), measured);
    const std::vector<std::string>& outputs = baked.value().compiled.outputs;
    const std::vector<std::string>& masks = baked.value().compiled.mask_layers;
    for (const auto& [key, wanted] : expect->as_object()) {
        const std::string at = "/expect/" + json::pointer_escape(key);
        const std::size_t dot = key.find('.');
        if (dot == std::string::npos) {
            errors.push_back(
                problem(file, at, R"(expected "<output>.<measure>" or "mask.<layer>")"));
            continue;
        }
        const std::string head = key.substr(0, dot);
        const std::string tail = key.substr(dot + 1);
        const json::Value* got = nullptr;
        std::string what;
        if (head == "mask") {
            got = measured.find("masks") != nullptr ? measured.find("masks")->find(tail) : nullptr;
            what = "the " + quoted(tail) + " layer's mask covers";
            if (got == nullptr) {
                std::string message = quoted(tail) + " isn't a masked layer";
                if (const std::string_view near = closest_match(tail, masks); !near.empty()) {
                    message += " (did you mean " + quoted(near) + "?)";
                }
                errors.push_back(problem(file, at, std::move(message)));
                continue;
            }
        } else {
            const json::Value* output = measured.find("outputs")->find(head);
            if (output == nullptr) {
                std::string message = quoted(head) + " isn't a texture the stack bakes";
                if (const std::string_view near = closest_match(head, outputs); !near.empty()) {
                    message += " (did you mean " + quoted(near) + "?)";
                }
                errors.push_back(problem(file, at, std::move(message)));
                continue;
            }
            got = output->find(tail);
            what = quoted(head) + "'s " + tail + " is";
            if (got == nullptr) {
                errors.push_back(problem(file, at,
                                         quoted(tail) + " isn't a measure (value, r, g, b, a, "
                                                        "low, high, contrast)"));
                continue;
            }
        }
        const double low = wanted.as_array()[0].as_double();
        const double high = wanted.as_array()[1].as_double();
        const double value = got->as_double();
        if (value < low || value > high) {
            errors.push_back(
                problem(file, at,
                        what + " " + json::write(*got, json::Style::Compact) + ", outside [" +
                            json::write(wanted.as_array()[0], json::Style::Compact) + ", " +
                            json::write(wanted.as_array()[1], json::Style::Compact) + "]"));
        }
    }
    return true;
}

std::optional<render::Image> paint_albedo(Session& session, const std::string& file) {
    StackFile read;
    if (read_stack(session, file, "/paths", false, read) ||
        !procedural::validate_paint(read.doc).empty()) {
        return std::nullopt;
    }
    auto prepared = prepare(session, read.doc, false);
    if (!prepared) {
        return std::nullopt;
    }
    auto baked = bake(session, prepared.value(), {});
    if (!baked) {
        return std::nullopt;
    }
    const auto& outputs = baked.value().compiled.outputs;
    const auto found = std::ranges::find(outputs, "albedo");
    if (found == outputs.end()) {
        return std::nullopt;
    }
    return std::move(baked.value().images[static_cast<std::size_t>(found - outputs.begin())]);
}

void register_paint_commands(Registry& registry) {
    using enum Capability;
    json::Value path = property("string", "The .paint.json stack, project path.");
    path["minLength"] = 1;
    json::Value parameters = property(
        "object", "Values for the stack's parameters by name, in place of its own, as a variant.");

    json::Value patch = property(
        "object", "A JSON merge patch over the stack's other fields: {\"parameters\": {\"wear\": "
                  "0.7}, \"model\": \"props/chest.obj\"}; null removes.");
    json::Value layers = property(
        "object",
        "Layers by name: a merge patch over each (a new name adds a layer on top), or null to "
        "remove one. In a patch, \"above\" or \"below\" a layer's name moves it there, and "
        "\"add_strokes\" and \"add_decals\" append to its lists.");
    registry.add({
        .name = "paint.edit",
        .description =
            "Changes a paint stack (.paint.json) or makes one: its fields by merge patch, and its "
            "layers by name (add, change, move, remove, append strokes and decals), checked "
            "before anything is written. Returns the changed pointers, the layers in order and, "
            "when it paints a model or sprite and \"measure\" isn't false, each texture's "
            "measures and each mask's coverage.",
        .capabilities = {Edit, Filesystem},
        .effect = Effect::Edit,
        .params = params_schema(
            {
                {"path", path},
                {"patch", std::move(patch)},
                {"layers", std::move(layers)},
                {"measure", property("boolean", "Bake and measure after (default true).")},
            },
            json::Array{"path"}),
        .handler = paint_edit,
    });

    registry.add({
        .name = "paint.bake",
        .description =
            "Bakes a paint stack into textures beside it (<stack>_albedo.png, _orm, _normal, and "
            "_height and _emission where painted) and, for a model, points a "
            ".standard_material.json at them; the model's mesh maps bake first as "
            "model.bake_maps would. Returns the files, each texture's measures (value, r, g, b, "
            "a, low, high, contrast over the painted texels), each mask's coverage and the "
            "model's texel density.",
        .capabilities = {Edit, Filesystem},
        .effect = Effect::Edit,
        .params = params_schema({{"path", path}, {"parameters", parameters}}, json::Array{"path"}),
        .handler = paint_bake,
    });

    json::Value views = property("integer", "Views round a model, from three quarters. Default 4.");
    views["minimum"] = 1;
    views["maximum"] = 8;
    json::Value size = property("array", "Each view's [width, height]. Default [160, 160].");
    size["items"]["type"] = "integer";
    size["items"]["minimum"] = 16;
    size["items"]["maximum"] = 1024;
    size["minItems"] = 2;
    size["maxItems"] = 2;
    registry.add({
        .name = "paint.preview",
        .description =
            "Bakes a paint stack in memory and draws it: a model from views round it on a gray "
            "stage, a sprite over gray. Returns the bake's numbers and each view's (value, r, g, "
            "b, low, high, contrast) and, with \"out\", writes the views side by side as a PNG. "
            "Writes nothing else.",
        .capabilities = {Read, Filesystem},
        .effect = Effect::Read,
        .params = params_schema(
            {
                {"path", path},
                {"parameters", parameters},
                {"views", std::move(views)},
                {"size", std::move(size)},
                {"out", property("string", "A PNG file to write the views to, project path.")},
            },
            json::Array{"path"}),
        .handler = paint_preview,
    });
}

} // namespace slash::command
