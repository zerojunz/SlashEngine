// model.bake_maps: a model's mesh maps (slash/scene3d/mesh_maps.hpp), baked into PNG files in a
// folder beside it, for texture graphs' image nodes and paint stacks to mask with.

#include "slash/scene3d/mesh_maps.hpp"

#include "procedural.hpp"

#include "slash/command/session.hpp"
#include "slash/core/sha256.hpp"
#include "slash/render/device.hpp"
#include "slash/render/image.hpp"
#include "slash/scene3d/model.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slash::command {
namespace {

constexpr std::string_view kMapsSuffix = ".maps";
constexpr std::string_view kManifest = "maps.json";

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

json::Array numbers(Vec3 v) {
    return json::Array{rounded(v.x), rounded(v.y), rounded(v.z)};
}

std::string hex_color(const std::array<std::uint8_t, 3>& rgb) {
    constexpr std::string_view kDigits = "0123456789abcdef";
    std::string out = "#";
    for (const std::uint8_t c : rgb) {
        out += kDigits[c >> 4U];
        out += kDigits[c & 0xFU];
    }
    return out;
}

// Where a model's maps go by default: its path without its extensions, plus ".maps"; a recipe's
// variant ("crate.model.json::worn") is named after it ("crate.worn.maps").
std::string default_folder(std::string_view path) {
    std::string_view variant;
    if (const std::size_t split = path.find("::"); split != std::string_view::npos) {
        variant = path.substr(split + 2);
        path = path.substr(0, split);
    }
    const std::size_t slash = path.rfind('/');
    const std::size_t name = slash == std::string_view::npos ? 0 : slash + 1;
    const std::size_t dot = path.find('.', name);
    std::string out(path.substr(0, dot));
    if (!variant.empty()) {
        out += "." + std::string(variant);
    }
    return out + std::string(kMapsSuffix);
}

// A device that traces rays, or none.
std::unique_ptr<render::RenderDevice> bake_device() {
    for (const char* driver : {"auto", "vulkan"}) {
        auto made = render::create_device({.window = nullptr, .driver = driver, .debug = false});
        if (made && made.value()->ray_tracing()) {
            return std::move(made).value();
        }
    }
    return nullptr;
}

void hash_mesh(Sha256& hash, const scene3d::MeshMapMesh& mesh) {
    const auto put = [&](const void* data, std::size_t bytes) {
        hash.update(std::string_view(static_cast<const char*>(data), bytes));
    };
    const auto count = static_cast<std::uint64_t>(mesh.vertices.size());
    put(&count, sizeof(count));
    for (const render::Vertex3D& v : mesh.vertices) {
        put(v.position.data(), sizeof(v.position));
        put(v.normal.data(), sizeof(v.normal));
        put(v.tangent.data(), sizeof(v.tangent));
        put(v.uv.data(), sizeof(v.uv));
        put(v.uv2.data(), sizeof(v.uv2));
    }
    put(mesh.indices.data(), mesh.indices.size() * sizeof(std::uint32_t));
    put(mesh.parts.data(), mesh.parts.size() * sizeof(std::uint32_t));
    put(mesh.materials.data(), mesh.materials.size() * sizeof(std::uint32_t));
}

json::Array id_list(const std::vector<std::string>& names) {
    json::Array out;
    for (std::size_t i = 0; i < names.size(); ++i) {
        json::Value item;
        item["name"] = names[i];
        item["color"] = hex_color(scene3d::mesh_map_id_color(static_cast<std::uint32_t>(i)));
        out.emplace_back(std::move(item));
    }
    return out;
}

} // namespace

Reply bake_model_maps(Session& session, const json::Value& params, bool write,
                      std::map<std::string, render::Image, std::less<>>* images) {
    const std::string& path = params.find("path")->as_string();
    if (!scene3d::is_model_path(path)) {
        return param_error(code::kInvalidParams, "/path",
                           quoted(path) + " isn't a model file: give a .gltf, .glb, .obj, .fbx, "
                                          ".blend or .model.json file");
    }
    auto low_model = session.assets().load<scene3d::Model>(path);
    if (!low_model) {
        return param_error(code::kInvalidParams, "/path", low_model.error().message);
    }
    const scene3d::MeshMapMesh low = scene3d::mesh_map_mesh(*low_model.value());
    std::optional<scene3d::MeshMapMesh> high;
    std::string high_path;
    if (const json::Value* given = params.find("high")) {
        high_path = given->as_string();
        if (!scene3d::is_model_path(high_path)) {
            return param_error(code::kInvalidParams, "/high",
                               quoted(high_path) + " isn't a model file");
        }
        auto high_model = session.assets().load<scene3d::Model>(high_path);
        if (!high_model) {
            return param_error(code::kInvalidParams, "/high", high_model.error().message);
        }
        high = scene3d::mesh_map_mesh(*high_model.value());
    }

    scene3d::MeshMapSettings settings;
    if (const json::Value* size = params.find("size")) {
        settings.width = static_cast<std::uint32_t>(size->as_int());
        settings.height = settings.width;
    }
    if (const json::Value* maps = params.find("maps")) {
        for (std::size_t i = 0; i < maps->as_array().size(); ++i) {
            const std::string& name = maps->as_array()[i].as_string();
            const std::optional<scene3d::MeshMap> map = scene3d::mesh_map_named(name);
            if (!map) {
                return param_error(code::kInvalidParams, "/maps/" + std::to_string(i),
                                   quoted(name) + " isn't a mesh map");
            }
            if (scene3d::mesh_map_needs_high(*map) && !high) {
                return param_error(code::kInvalidParams, "/maps/" + std::to_string(i),
                                   "the " + name + " map bakes from a high mesh: give \"high\"");
            }
            settings.maps.push_back(*map);
        }
    }
    if (const json::Value* uv = params.find("uv")) {
        settings.uv2 = uv->as_string() == "uv2";
    }
    if (const json::Value* rays = params.find("rays")) {
        settings.rays = static_cast<std::uint32_t>(rays->as_int());
    }
    if (const json::Value* padding = params.find("padding")) {
        settings.padding = static_cast<std::uint32_t>(padding->as_int());
    }
    const auto read_distance = [&](std::string_view name, float& into) {
        if (const json::Value* value = params.find(name)) {
            into = static_cast<float>(value->as_double());
        }
    };
    read_distance("occlusion_distance", settings.occlusion_distance);
    read_distance("thickness_distance", settings.thickness_distance);
    read_distance("curvature_radius", settings.curvature_radius);
    read_distance("cage_distance", settings.cage_distance);

    const json::Value* out_param = params.find("out");
    const std::string folder = out_param != nullptr ? out_param->as_string() : default_folder(path);
    if (auto full = session.resolve_path(folder + "/" + std::string(kManifest)); !full) {
        return param_error(code::kInvalidParams, out_param != nullptr ? "/out" : "/path",
                           full.error().message);
    }
    const json::Value* device = params.find("device");
    const bool on_gpu = device != nullptr && device->as_string() == "gpu";

    // What the bake depends on: the meshes, then the params but out and device.
    Sha256 hasher;
    hash_mesh(hasher, low);
    if (high) {
        hash_mesh(hasher, *high);
    }
    json::Value keyed = params;
    keyed.as_object().erase("out");
    keyed.as_object().erase("device");
    keyed.as_object().erase("path");
    keyed.as_object().erase("high");
    hasher.update(json::write(keyed));
    const std::string key = hasher.hex().substr(0, 16);

    // The bake the folder holds already, when it was made from the same meshes and params.
    const std::string manifest_path = folder + "/" + std::string(kManifest);
    auto manifest_before = session.read_project_file(manifest_path);
    if (!manifest_before) {
        return Reply::failure(manifest_before.error());
    }
    if (const std::optional<std::string>& held = manifest_before.value(); held && !on_gpu) {
        auto parsed = json::parse(*held);
        if (parsed && parsed.value().is_object()) {
            const json::Value* hash = parsed.value().find("hash");
            const json::Value* result = parsed.value().find("result");
            if (hash != nullptr && hash->is_string() && hash->as_string() == key &&
                result != nullptr && result->is_object()) {
                bool all_there = true;
                if (const json::Value* files = result->find("files"); files != nullptr) {
                    for (const json::Value& file : files->as_array()) {
                        auto bytes = session.read_project_file(file.as_string());
                        all_there = all_there && bytes && bytes.value();
                    }
                }
                if (all_there && images != nullptr) {
                    for (const auto& [name, file] : result->find("maps")->as_object()) {
                        auto bytes = session.read_project_file(file.as_string());
                        auto image = bytes && bytes.value()
                                         ? render::decode_image(*bytes.value())
                                         : Result<render::Image>(
                                               make_error(ErrorCode::NotFound, "unreadable"));
                        if (!image) {
                            all_there = false;
                            break;
                        }
                        images->insert_or_assign(name, std::move(image).value());
                    }
                }
                if (all_there) {
                    json::Value cached = *result;
                    cached["cached"] = true;
                    return Reply::success(std::move(cached));
                }
            }
        }
    }

    std::unique_ptr<render::RenderDevice> gpu;
    if (on_gpu) {
        gpu = bake_device();
        settings.device = gpu.get();
    }
    auto baked =
        scene3d::bake_mesh_maps(low, high ? &*high : nullptr, settings, &session.world().pool());
    if (!baked) {
        return param_error(code::kInvalidParams, "/path", baked.error().message);
    }
    const scene3d::MeshMapBake& bake = baked.value();

    json::Value result;
    result["out"] = folder;
    result["size"] = json::Array{bake.width, bake.height};
    result["coverage"] = rounded(bake.coverage);
    result["overlap"] = rounded(bake.overlap);
    if (high) {
        result["missed"] = rounded(bake.missed);
        result["height_range"] = rounded(bake.height_range);
    }
    json::Value box;
    box["min"] = numbers(bake.min);
    box["max"] = numbers(bake.max);
    result["position_box"] = std::move(box);
    json::Object maps;
    json::Array files;
    json::Value stats;
    std::vector<std::pair<std::string, std::string>> writes;
    for (const scene3d::MeshMapImage& map : bake.maps) {
        if (images != nullptr) {
            images->insert_or_assign(std::string(scene3d::mesh_map_name(map.map)), map.image);
        }
        auto png = render::encode_png(map.image);
        if (!png) {
            return Reply::failure(png.error());
        }
        const std::string name(scene3d::mesh_map_name(map.map));
        std::string file = folder;
        file += "/";
        file += name;
        file += ".png";
        files.emplace_back(file);
        writes.emplace_back(file, std::move(png).value());
        maps.emplace(name, json::Value(file));
        // The gray maps' [min, mean, max] over the texels a triangle covers.
        const scene3d::MeshMap kind = map.map;
        if (kind == scene3d::MeshMap::Ao || kind == scene3d::MeshMap::Curvature ||
            kind == scene3d::MeshMap::Thickness || kind == scene3d::MeshMap::Sky ||
            kind == scene3d::MeshMap::Height) {
            stats[name] =
                json::Array{rounded(map.stats[0]), rounded(map.stats[1]), rounded(map.stats[2])};
        }
    }
    result["maps"] = std::move(maps);
    result["files"] = files;
    if (!stats.is_null()) {
        result["stats"] = std::move(stats);
    }
    result["parts"] = id_list(low.part_names);
    result["materials"] = id_list(high ? high->material_names : low.material_names);
    json::Array warnings;
    if (bake.overlap > 0.01F) {
        warnings.emplace_back(
            "texture coordinates overlap on " +
            std::to_string(static_cast<int>(std::lround(bake.overlap * 100.0F))) +
            "% of covered texels, so the maps there show one surface: give each surface its own "
            "place in them, or bake into \"uv2\"");
    }
    if (high && bake.missed > 0.01F) {
        warnings.emplace_back(
            std::to_string(static_cast<int>(std::lround(bake.missed * 100.0F))) +
            "% of texels found no high surface within the cage: raise cage_distance");
    }
    if (!warnings.empty()) {
        result["warnings"] = std::move(warnings);
    }
    if (on_gpu) {
        result["device"] = bake.traced_on_gpu ? "gpu" : "cpu";
    }

    if (!write) {
        return Reply::success(std::move(result));
    }
    json::Value manifest;
    manifest["format"] = 1;
    manifest["hash"] = key;
    manifest["model"] = path;
    if (high) {
        manifest["high"] = high_path;
    }
    json::Value saved = result;
    saved.as_object().erase("device");
    manifest["result"] = std::move(saved);
    std::string manifest_text = json::write(manifest) + "\n";
    writes.emplace_back(manifest_path, std::move(manifest_text));

    bool changed = false;
    for (auto& [file, bytes] : writes) {
        auto before = session.read_project_file(file);
        if (!before) {
            return Reply::failure(before.error());
        }
        if (before.value() == bytes) {
            continue;
        }
        if (auto staged = session.stage_file(file, bytes); !staged) {
            return Reply::failure(staged.error());
        }
        session.add_undo_file(file, std::move(before).value(), std::move(bytes));
        changed = true;
    }
    if (!changed) {
        result["unchanged"] = true;
    }
    return Reply::success(std::move(result));
}

namespace {

Reply model_bake_maps(Session& session, const json::Value& params) {
    Reply reply = bake_model_maps(session, params, true, nullptr);
    if (reply.ok &&
        (reply.data.find("cached") != nullptr || reply.data.find("unchanged") != nullptr)) {
        session.mark_unchanged();
    }
    return reply;
}

} // namespace

void register_mesh_map_commands(Registry& registry) {
    using enum Capability;
    json::Value path = property("string", "The low model: .gltf, .glb, .obj, .fbx, .blend or "
                                          ".model.json (a variant after \"::\"), project path.");
    path["minLength"] = 1;
    json::Value high = property(
        "string", "A detailed model to bake from onto path's texture coordinates: needed for the "
                  "height and normal maps, and the other maps then come from its surface.");
    high["minLength"] = 1;
    json::Value map_names = json::Array{};
    for (const scene3d::MeshMap map : scene3d::kMeshMaps) {
        map_names.as_array().emplace_back(std::string(scene3d::mesh_map_name(map)));
    }
    json::Value maps = property(
        "array", "The maps to bake. Default: all of them, height and normal only with high.");
    maps["items"]["type"] = "string";
    maps["items"]["enum"] = std::move(map_names);
    maps["uniqueItems"] = true;
    maps["minItems"] = 1;
    json::Value size = property("integer", "Texels wide and high. Default 1024.");
    size["minimum"] = 16;
    size["maximum"] = 8192;
    json::Value uv = property("string", "The texture coordinates to bake into. Default \"uv\".");
    uv["enum"] = json::Array{"uv", "uv2"};
    json::Value rays =
        property("integer", "Rays a texel traces for each of ao, sky and thickness. Default 32.");
    rays["minimum"] = 1;
    rays["maximum"] = 1024;
    const auto distance = [](std::string_view description) {
        json::Value schema = property("number", description);
        schema["exclusiveMinimum"] = 0;
        return schema;
    };
    json::Value padding =
        property("integer", "Texels each island grows into the empty ones around it. Default 4.");
    padding["minimum"] = 0;
    padding["maximum"] = 64;
    json::Value out = property("string", "The folder to write, project path. Default: path "
                                         "without its extensions, plus \".maps\".");
    out["minLength"] = 1;
    json::Value device = property("string", "\"gpu\" traces on a GPU that traces rays, else the "
                                            "CPU, and returns which did. Default \"cpu\".");
    device["enum"] = json::Array{"cpu", "gpu"};
    registry.add({
        .name = "model.bake_maps",
        .description =
            "Bakes a model's mesh maps into PNG files in its texture coordinates, for texture "
            "graphs (image nodes) to mask with: ao, curvature, thickness, sky, world_normal, "
            "position, part_id, material_id, and from a high model height and normal. Skips the "
            "bake when the folder's maps.json says it holds it. Returns files, coverage, overlap, "
            "stats and id colors.",
        .capabilities = {Edit, Filesystem},
        .effect = Effect::Edit,
        .params = params_schema(
            {{"path", std::move(path)},
             {"high", std::move(high)},
             {"maps", std::move(maps)},
             {"size", std::move(size)},
             {"uv", std::move(uv)},
             {"rays", std::move(rays)},
             {"occlusion_distance",
              distance("How far an occluding surface may be, in the model's units. Default a "
                       "quarter of its size.")},
             {"thickness_distance",
              distance("The most thickness measured. Default half the model's size.")},
             {"curvature_radius",
              distance("How far around a point curvature looks. Default a fiftieth of the "
                       "model's size.")},
             {"cage_distance",
              distance("How far out from path's surface and in rays look for high's. Default "
                       "a twentieth of the size.")},
             {"padding", std::move(padding)},
             {"out", std::move(out)},
             {"device", std::move(device)}},
            {"path"}),
        .handler = model_bake_maps,
    });
}

} // namespace slash::command
