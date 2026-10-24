#include "slash/command/terrain.hpp"

#include "slash/command/session.hpp"
#include "slash/core/math.hpp"
#include "slash/core/text.hpp"
#include "slash/render/image.hpp"
#include "slash/scene3d/terrain.hpp"
#include "slash/world/components.hpp"
#include "slash/world/tree3d.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slash::command {
namespace {

constexpr std::int64_t kMaxPoints = 1024;
constexpr float kPi = std::numbers::pi_v<float>;
constexpr std::string_view kPng = ".png";

json::Value property(std::string_view type, std::string_view description) {
    json::Value schema;
    schema["type"] = type;
    schema["description"] = description;
    return schema;
}

json::Value number_property(std::string_view description, double minimum, double maximum) {
    json::Value schema = property("number", description);
    schema["minimum"] = minimum;
    schema["maximum"] = maximum;
    return schema;
}

json::Value integer_property(std::string_view description, std::int64_t minimum,
                             std::int64_t maximum) {
    json::Value schema = property("integer", description);
    schema["minimum"] = minimum;
    schema["maximum"] = maximum;
    return schema;
}

json::Value pair_property(std::string_view description) {
    json::Value schema = property("array", description);
    schema["items"]["type"] = "number";
    schema["minItems"] = 2;
    schema["maxItems"] = 2;
    return schema;
}

json::Value points_property(std::string_view description) {
    json::Value schema = property("array", description);
    schema["items"] = pair_property("[x, z]");
    schema["minItems"] = 1;
    schema["maxItems"] = kMaxPoints;
    return schema;
}

json::Value id_property() {
    json::Value schema = property("string", "An entity with an HTerrain.");
    schema["minLength"] = 1;
    schema["maxLength"] = 255;
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

// A number rounded to four decimals, with no ".0" on whole ones, so replies stay short.
json::Value short_number(float value) {
    constexpr double kScale = 10000.0;
    const double out = std::round(static_cast<double>(value) * kScale) / kScale;
    if (out == std::floor(out) && std::abs(out) < 1.0e15) {
        return static_cast<std::int64_t>(out);
    }
    return out;
}

json::Value point_value(Vec2 point) {
    return json::Array{short_number(point.x), short_number(point.y)};
}

json::Value point_value(Vec3 point) {
    return json::Array{short_number(point.x), short_number(point.y), short_number(point.z)};
}

float number(const json::Value& params, std::string_view key, float fallback) {
    const json::Value* value = params.find(key);
    return value != nullptr ? static_cast<float>(value->as_double()) : fallback;
}

Vec2 pair(const json::Value& value) {
    const json::Array& xy = value.as_array();
    return {static_cast<float>(xy[0].as_double()), static_cast<float>(xy[1].as_double())};
}

Vec2 pair(const json::Value& params, std::string_view key, Vec2 fallback) {
    const json::Value* value = params.find(key);
    return value != nullptr ? pair(*value) : fallback;
}

bool flag(const json::Value& params, std::string_view key, bool fallback = false) {
    const json::Value* value = params.find(key);
    return value != nullptr ? value->as_bool() : fallback;
}

float length(Vec3 v) {
    return std::sqrt((v.x * v.x) + (v.y * v.y) + (v.z * v.z));
}

float dot(Vec3 a, Vec3 b) {
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

bool valid_resolution(int resolution) {
    return resolution >= 65 && resolution <= 4097 && ((resolution - 1) & (resolution - 2)) == 0;
}

// One of an HTerrain's map files: its path ("" for none) and what it held before the command,
// nullopt when it doesn't exist yet.
struct MapFile {
    std::string path;
    std::optional<std::string> before;
};

// An entity's HTerrain and its maps as the command finds them, edits staged earlier in a batch
// included: a heightmap that doesn't exist yet is flat at the bottom of height_range, a splat
// map the first texture everywhere and a color map white with no holes.
struct Target {
    std::string id;
    entt::entity entity = entt::null;
    world::HTerrain terrain;
    scene3d::TerrainMaps maps;
    MapFile heightmap;
    MapFile splatmap;
    MapFile colormap;
};

// Reads a map's file into bytes, keeping what it held for undo.
std::optional<Reply> read_map(Session& session, MapFile& file, std::string& bytes) {
    if (file.path.empty()) {
        return std::nullopt;
    }
    auto read = session.read_project_file(file.path);
    if (!read) {
        return Reply::failure(read.error());
    }
    file.before = std::move(read).value();
    if (file.before) {
        bytes = *file.before;
    }
    return std::nullopt;
}

std::optional<Reply> find_terrain(Session& session, const json::Value& params, Target& out) {
    world::World& world = session.world();
    out.id = params.find("id")->as_string();
    out.entity = world.find(out.id);
    if (out.entity == entt::null) {
        return unknown_entity(world, out.id, "/id");
    }
    const auto* terrain = world.ecs().try_get<const world::HTerrain>(out.entity);
    if (terrain == nullptr) {
        return param_error(code::kInvalidParams, "/id", quoted(out.id) + " has no HTerrain");
    }
    out.terrain = *terrain;
    const int r = terrain->resolution;
    if (!valid_resolution(r)) {
        return param_error(code::kInvalidArgument, "/id",
                           quoted(out.id) +
                               "'s HTerrain resolution is a power of two plus one from 65 to "
                               "4097, such as 513, not " +
                               std::to_string(r));
    }
    const auto cells = static_cast<std::size_t>(r) * static_cast<std::size_t>(r);
    const std::string size = std::to_string(r) + "x" + std::to_string(r);
    const auto wrong_size = [&](std::string_view name, const std::string& path, std::uint32_t width,
                                std::uint32_t height) {
        return param_error(code::kInvalidArgument, "/id",
                           quoted(out.id) + "'s " + std::string(name) + " " + quoted(path) +
                               " is " + std::to_string(width) + "x" + std::to_string(height) +
                               " pixels, but its resolution makes the maps " + size);
    };
    out.maps.resolution = r;
    out.heightmap.path = terrain->heightmap;
    out.splatmap.path = terrain->splatmap;
    out.colormap.path = terrain->colormap;
    std::string bytes;
    if (auto failed = read_map(session, out.heightmap, bytes)) {
        return failed;
    }
    if (out.heightmap.before) {
        auto decoded = render::decode_gray16(bytes);
        if (!decoded) {
            return param_error(code::kInvalidArgument, "/id",
                               quoted(out.heightmap.path) + ": " + decoded.error().message);
        }
        if (decoded.value().width != static_cast<std::uint32_t>(r) ||
            decoded.value().height != static_cast<std::uint32_t>(r)) {
            return wrong_size("heightmap", out.heightmap.path, decoded.value().width,
                              decoded.value().height);
        }
        out.maps.heights = scene3d::decode_heights(decoded.value(), terrain->height_range);
    } else {
        out.maps.heights.assign(cells, terrain->height_range.x);
    }
    const auto read_image = [&](std::string_view name, MapFile& file,
                                std::vector<std::uint8_t>& pixels,
                                std::array<std::uint8_t, 4> fill) -> std::optional<Reply> {
        bytes.clear();
        if (auto failed = read_map(session, file, bytes)) {
            return failed;
        }
        if (!file.before) {
            pixels.resize(cells * 4);
            for (std::size_t i = 0; i < cells; ++i) {
                std::copy(fill.begin(), fill.end(),
                          pixels.begin() + static_cast<std::ptrdiff_t>(i * 4));
            }
            return std::nullopt;
        }
        auto decoded = render::decode_image(bytes);
        if (!decoded) {
            return param_error(code::kInvalidArgument, "/id",
                               quoted(file.path) + ": " + decoded.error().message);
        }
        render::Image& image = decoded.value();
        if (image.width != static_cast<std::uint32_t>(r) ||
            image.height != static_cast<std::uint32_t>(r)) {
            return wrong_size(name, file.path, image.width, image.height);
        }
        if (image.pixels.size() != cells * 4) {
            return param_error(code::kInvalidArgument, "/id",
                               quoted(file.path) + " has no pixels to read: use a PNG");
        }
        pixels = std::move(image.pixels);
        return std::nullopt;
    };
    if (auto failed = read_image("splatmap", out.splatmap, out.maps.splat, {255, 0, 0, 0})) {
        return failed;
    }
    if (auto failed = read_image("colormap", out.colormap, out.maps.color, {255, 255, 255, 255})) {
        return failed;
    }
    return std::nullopt;
}

// The map a command writes must be named, and be a file of its kind.
std::optional<Reply> need_map(const Target& target, std::string_view name, const std::string& path,
                              std::string_view suffix, std::string_view example) {
    if (path.empty()) {
        return param_error(code::kInvalidArgument, "/id",
                           quoted(target.id) + "'s HTerrain names no " + std::string(name) +
                               " to write: set " + std::string(name) + " to a path such as " +
                               quoted(example));
    }
    if (!path.ends_with(suffix)) {
        return param_error(code::kInvalidArgument, "/id",
                           quoted(target.id) + "'s " + std::string(name) + " " + quoted(path) +
                               " is not a " + std::string(suffix) + " file");
    }
    return std::nullopt;
}

std::optional<Reply> need_heightmap(const Target& target) {
    return need_map(target, "heightmap", target.terrain.heightmap, scene3d::kHeightmapSuffix,
                    "terrain/land.height.png");
}

// The maps as the terrain's data, for sampling heights, normals and holes.
scene3d::TerrainData data_of(const Target& target) {
    scene3d::TerrainData data;
    const auto r = static_cast<std::uint32_t>(target.maps.resolution);
    data.resolution = target.maps.resolution;
    data.heights = target.maps.heights;
    auto splat = std::make_shared<render::Image>();
    splat->width = r;
    splat->height = r;
    splat->pixels = target.maps.splat;
    data.splatmap = std::move(splat);
    auto color = std::make_shared<render::Image>();
    color->width = r;
    color->height = r;
    color->pixels = target.maps.color;
    data.colormap = std::move(color);
    scene3d::update_ranges(data);
    return data;
}

// Where the terrain's cells are in the space points are given in: its entity's, or the world's
// with global.
world::Affine3D cells_of(const world::World& world, const Target& target, bool global) {
    return scene3d::terrain_transform(target.terrain, target.maps.resolution,
                                      global ? world::Tree3D(world).global(target.entity)
                                             : world::Affine3D{});
}

// Stages a map's new bytes and the undo that puts the old ones back, unless they are the same.
std::optional<Reply> write_map(Session& session, MapFile& file, std::string bytes,
                               json::Array& written) {
    if (file.before && *file.before == bytes) {
        return std::nullopt;
    }
    if (auto staged = session.stage_file(file.path, bytes); !staged) {
        return Reply::failure(staged.error());
    }
    session.add_undo_file(file.path, file.before, std::move(bytes));
    written.emplace_back(file.path);
    return std::nullopt;
}

// Writes the heights, widening height_range to whole meters that hold them when they leave it:
// a heightmap's values only reach from its bottom to its top.
std::optional<Reply> write_heights(Session& session, Target& target, json::Value& result,
                                   json::Array& written) {
    float low = std::numeric_limits<float>::max();
    float high = std::numeric_limits<float>::lowest();
    for (const float h : target.maps.heights) {
        if (!std::isfinite(h)) {
            return Reply::failure(code::kInvalidArgument,
                                  "the heights came out infinite; try other settings");
        }
        low = std::min(low, h);
        high = std::max(high, h);
    }
    Vec2 range = target.terrain.height_range;
    if (low < range.x || high > range.y) {
        range = {std::min(range.x, std::floor(low)), std::max(range.y, std::ceil(high))};
        session.world().ecs().get<world::HTerrain>(target.entity).height_range = range;
        target.terrain.height_range = range;
        result["height_range"] = point_value(range);
    }
    auto png = render::encode_gray16_png(
        scene3d::encode_heights(target.maps.heights, target.maps.resolution, range));
    if (!png) {
        return Reply::failure(png.error());
    }
    result["heights"] = point_value(Vec2{low, high});
    return write_map(session, target.heightmap, std::move(png).value(), written);
}

std::optional<Reply> write_image(Session& session, const Target& target, MapFile& file,
                                 const std::vector<std::uint8_t>& pixels, json::Array& written) {
    render::Image image;
    image.width = static_cast<std::uint32_t>(target.maps.resolution);
    image.height = image.width;
    image.pixels = pixels;
    auto png = render::encode_png(image);
    if (!png) {
        return Reply::failure(png.error());
    }
    return write_map(session, file, std::move(png).value(), written);
}

Reply finish(Session& session, json::Value result, json::Array written) {
    if (written.empty()) {
        session.mark_unchanged();
        result["unchanged"] = true;
    } else {
        result["files"] = std::move(written);
    }
    return Reply::success(std::move(result));
}

// An HTerrain's bounds and holes, or what is at points over it: the height and normal there and
// the nearest cell's texture weights, color and hole, as HTerrainData's get_interpolated_height_at
// and the maps' pixels.
Reply terrain_get(Session& session, const json::Value& params) {
    Target target;
    if (auto failed = find_terrain(session, params, target)) {
        return std::move(*failed);
    }
    const world::World& world = session.world();
    const bool global = flag(params, "global");
    const world::Affine3D cells = cells_of(world, target, global);
    const scene3d::TerrainData data = data_of(target);
    const int r = data.resolution;
    const auto last = static_cast<float>(r - 1);
    json::Value result;
    if (const json::Value* points = params.find("points"); points != nullptr) {
        const world::Affine3D to_cells = cells.inverse();
        // Normals turn by the inverse transpose of the entity's place; terrain_normal has
        // map_scale in already.
        const world::Affine3D undo_place =
            global ? world::Tree3D(world).global(target.entity).inverse() : world::Affine3D{};
        const bool weights = target.terrain.textures.size() > 1 || target.splatmap.before;
        json::Array out;
        for (const json::Value& point : points->as_array()) {
            const Vec2 xz = pair(point);
            const Vec3 at = to_cells.apply({xz.x, 0.0F, xz.y});
            const float x = std::clamp(at.x, 0.0F, last);
            const float z = std::clamp(at.z, 0.0F, last);
            json::Value each;
            each["position"] = point_value(cells.apply({x, data.interpolated_height(x, z), z}));
            const Vec3 n = scene3d::terrain_normal(data, target.terrain.map_scale, x, z);
            const Vec3 turned{dot(undo_place.x, n), dot(undo_place.y, n), dot(undo_place.z, n)};
            const float size = length(turned);
            each["normal"] = point_value(size > 0.0F ? turned * (1.0F / size) : n);
            const int cx = static_cast<int>(std::lround(x));
            const int cz = static_cast<int>(std::lround(z));
            const std::size_t pixel =
                ((static_cast<std::size_t>(cz) * static_cast<std::size_t>(r)) +
                 static_cast<std::size_t>(cx)) *
                4;
            if (weights) {
                json::Array splat;
                for (std::size_t c = 0; c < 4; ++c) {
                    splat.push_back(
                        short_number(static_cast<float>(target.maps.splat[pixel + c]) / 255.0F));
                }
                each["splat"] = std::move(splat);
            }
            if (target.colormap.before) {
                json::Array color;
                for (std::size_t c = 0; c < 3; ++c) {
                    color.push_back(
                        short_number(static_cast<float>(target.maps.color[pixel + c]) / 255.0F));
                }
                each["color"] = std::move(color);
            }
            if (data.hole(cx, cz)) {
                each["hole"] = true;
            }
            if (at.x < 0.0F || at.z < 0.0F || at.x > last || at.z > last) {
                each["outside"] = true;
            }
            out.push_back(std::move(each));
        }
        result["points"] = std::move(out);
        return Reply::success(std::move(result));
    }
    result["resolution"] = r;
    Vec3 low{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
             std::numeric_limits<float>::max()};
    Vec3 high{std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
              std::numeric_limits<float>::lowest()};
    for (int corner = 0; corner < 8; ++corner) {
        const Vec3 p = cells.apply({(corner & 1) != 0 ? last : 0.0F,
                                    (corner & 2) != 0 ? data.highest : data.lowest,
                                    (corner & 4) != 0 ? last : 0.0F});
        low = {std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z)};
        high = {std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z)};
    }
    result["aabb"]["min"] = point_value(low);
    result["aabb"]["max"] = point_value(high);
    std::size_t holes = 0;
    for (std::size_t i = 3; i < target.maps.color.size(); i += 4) {
        holes += target.maps.color[i] < 128 ? std::size_t{1} : std::size_t{0};
    }
    if (holes > 0) {
        result["holes"] = holes;
    }
    json::Array problems;
    for (const scene3d::Problem& problem : scene3d::check_terrains(world)) {
        if (problem.entity == target.entity) {
            json::Value item;
            item["path"] = problem.path;
            item["message"] = problem.message;
            problems.push_back(std::move(item));
        }
    }
    if (!problems.empty()) {
        result["problems"] = std::move(problems);
    }
    return Reply::success(std::move(result));
}

// Makes heights from noise, as HTerrain's generator dialog, and writes them.
Reply terrain_generate(Session& session, const json::Value& params) {
    Target target;
    if (auto failed = find_terrain(session, params, target)) {
        return std::move(*failed);
    }
    if (auto failed = need_heightmap(target)) {
        return std::move(*failed);
    }
    scene3d::TerrainNoise noise;
    noise.seed = static_cast<int>(number(params, "seed", static_cast<float>(noise.seed)));
    noise.offset = pair(params, "offset", noise.offset);
    noise.base_height = number(params, "base_height", noise.base_height);
    noise.height_range = number(params, "height_range", noise.height_range);
    noise.scale = number(params, "scale", noise.scale);
    noise.roughness = number(params, "roughness", noise.roughness);
    noise.curve = number(params, "curve", noise.curve);
    noise.octaves = static_cast<int>(number(params, "octaves", static_cast<float>(noise.octaves)));
    noise.erosion_steps =
        static_cast<int>(number(params, "erosion_steps", static_cast<float>(noise.erosion_steps)));
    noise.erosion_weight = number(params, "erosion_weight", noise.erosion_weight);
    noise.erosion_slope_factor = number(params, "erosion_slope_factor", noise.erosion_slope_factor);
    noise.erosion_slope_direction =
        pair(params, "erosion_slope_direction", noise.erosion_slope_direction);
    noise.erosion_slope_invert = flag(params, "erosion_slope_invert", noise.erosion_slope_invert);
    noise.dilation = number(params, "dilation", noise.dilation);
    noise.island_weight = number(params, "island_weight", noise.island_weight);
    noise.island_sharpness = number(params, "island_sharpness", noise.island_sharpness);
    noise.island_height_ratio = number(params, "island_height_ratio", noise.island_height_ratio);
    noise.island_shape = number(params, "island_shape", noise.island_shape);
    const bool additive = flag(params, "additive");
    target.maps.heights = scene3d::generate_heights(noise, target.maps.resolution,
                                                    additive ? &target.maps.heights : nullptr);
    json::Value result;
    result["id"] = target.id;
    json::Array written;
    if (auto failed = write_heights(session, target, result, written)) {
        return std::move(*failed);
    }
    return finish(session, std::move(result), std::move(written));
}

// Paints dabs of a round brush at points, as HTerrain's painter, and writes the maps they change.
Reply terrain_brush(Session& session, const json::Value& params) {
    Target target;
    if (auto failed = find_terrain(session, params, target)) {
        return std::move(*failed);
    }
    using enum scene3d::BrushMode;
    const std::string& mode = params.find("mode")->as_string();
    scene3d::BrushDab dab;
    if (mode == "raise") {
        dab.mode = Raise;
    } else if (mode == "lower") {
        dab.mode = Lower;
    } else if (mode == "smooth") {
        dab.mode = Smooth;
    } else if (mode == "flatten") {
        dab.mode = Flatten;
    } else if (mode == "level") {
        dab.mode = Level;
    } else if (mode == "splat") {
        dab.mode = Splat;
    } else if (mode == "color") {
        dab.mode = Color;
    } else if (mode == "hole") {
        dab.mode = Hole;
    } else {
        dab.mode = Unhole;
    }
    const bool heights = dab.mode == Raise || dab.mode == Lower || dab.mode == Smooth ||
                         dab.mode == Flatten || dab.mode == Level;
    if (heights) {
        if (auto failed = need_heightmap(target)) {
            return std::move(*failed);
        }
    } else if (dab.mode == Splat) {
        if (auto failed = need_map(target, "splatmap", target.terrain.splatmap, kPng,
                                   "terrain/land.splat.png")) {
            return std::move(*failed);
        }
    } else if (auto failed = need_map(target, "colormap", target.terrain.colormap, kPng,
                                      "terrain/land.color.png")) {
        return std::move(*failed);
    }
    const bool global = flag(params, "global");
    const world::Affine3D cells = cells_of(session.world(), target, global);
    const world::Affine3D to_cells = cells.inverse();
    // Lengths along the ground and up turn into cells and heightmap meters by the sizes of a
    // cell's sides where the points are.
    const float across = length(cells.x);
    const float up = length(cells.y);
    dab.radius = number(params, "radius", 8.0F) / (across > 0.0F ? across : 1.0F);
    dab.falloff = number(params, "falloff", dab.falloff);
    dab.opacity = number(params, "opacity", dab.opacity);
    dab.amount = number(params, "amount", dab.amount) / (up > 0.0F ? up : 1.0F);
    dab.texture = static_cast<int>(number(params, "texture", 0.0F));
    if (const json::Value* color = params.find("color"); color != nullptr) {
        const json::Array& rgb = color->as_array();
        dab.color = {static_cast<float>(rgb[0].as_double()), static_cast<float>(rgb[1].as_double()),
                     static_cast<float>(rgb[2].as_double()), 1.0F};
    }
    // Slopes in degrees from flat, as HTerrain's slope limit slider, as the normal's y.
    const Vec2 slope = pair(params, "slope", {0.0F, 90.0F});
    dab.slope_min = cos_sin(std::min(slope.y, 90.0F) * kPi / 180.0F).x;
    dab.slope_max = cos_sin(std::max(slope.x, 0.0F) * kPi / 180.0F).x + 0.001F;
    dab.map_scale = target.terrain.map_scale;
    const float height = number(params, "height", 0.0F);
    std::size_t changed = 0;
    for (const json::Value& point : params.find("points")->as_array()) {
        const Vec2 xz = pair(point);
        const Vec3 at = to_cells.apply({xz.x, height, xz.y});
        dab.center = {at.x, at.z};
        dab.height = at.y;
        changed += scene3d::paint(target.maps, dab);
    }
    json::Value result;
    result["id"] = target.id;
    result["changed"] = changed;
    json::Array written;
    if (changed > 0) {
        std::optional<Reply> failed;
        if (heights) {
            failed = write_heights(session, target, result, written);
        } else if (dab.mode == Splat) {
            failed = write_image(session, target, target.splatmap, target.maps.splat, written);
        } else {
            failed = write_image(session, target, target.colormap, target.maps.color, written);
        }
        if (failed) {
            return std::move(*failed);
        }
    }
    return finish(session, std::move(result), std::move(written));
}

} // namespace

void register_terrain_commands(Registry& registry) {
    using enum Capability;
    registry.add({
        .name = "terrain.get",
        .description = "Returns an HTerrain's resolution, bounds, hole count and problems; or "
                       "at points [x, z] the ground's position, normal, texture weights, color "
                       "and holes. In its space, or the world's with global.",
        .capabilities = {Read},
        .effect = Effect::Read,
        .params = params_schema(
            {
                {"id", id_property()},
                {"points", points_property("Points over the ground as [x, z].")},
                {"global", property("boolean", "Points and results in the world's space, for "
                                               "a terrain not tilted. Default false.")},
            },
            {"id"}),
        .handler = terrain_get,
    });
    json::Value seed = integer_property("The noise's seed. Default 0.", -1'000'000, 1'000'000);
    registry.add({
        .name = "terrain.generate",
        .description = "Makes an HTerrain's heights from fractal noise, as HTerrain's "
                       "generator: curved, shaped into an island and eroded, or added to the "
                       "heights it has with additive. Heights are the heightmap's meters, "
                       "before map_scale. Writes the heightmap, widening height_range to hold "
                       "them, and returns the lowest and highest.",
        .capabilities = {Edit, Filesystem},
        .effect = Effect::Edit,
        .params = params_schema(
            {
                {"id", id_property()},
                {"seed", std::move(seed)},
                {"offset", pair_property("Moves the noise by [x, z] cells. Default [0, 0].")},
                {"base_height",
                 number_property("The lowest the noise goes. Default -50.", -100000.0, 100000.0)},
                {"height_range",
                 number_property("How far above base_height it reaches. Default 150.", -100000.0,
                                 100000.0)},
                {"scale", number_property("About how many cells one hill spans. Default 100.", 0.0,
                                          100000.0)},
                {"roughness",
                 number_property("How much each finer octave adds. Default 0.4.", 0.0, 1.0)},
                {"curve", number_property("Raises heights from 0 to 1 to this power, so over 1 "
                                          "flattens valleys. Default 1.",
                                          0.0, 10.0)},
                {"octaves", integer_property("Layers of finer noise. Default 6.", 1, 10)},
                {"erosion_steps",
                 integer_property("Passes of erosion, wearing peaks down. Default 0.", 0, 100)},
                {"erosion_weight",
                 number_property("How much each pass wears. Default 0.5.", 0.0, 1.0)},
                {"erosion_slope_factor",
                 number_property("How much erosion follows erosion_slope_direction. Default 0.",
                                 0.0, 1.0)},
                {"erosion_slope_direction",
                 pair_property("The way erosion runs, [x, z]. Default [0, 0].")},
                {"erosion_slope_invert",
                 property("boolean", "Erode against the direction. Default false.")},
                {"dilation", number_property("Grows plateaus instead of wearing peaks, 0 to 1. "
                                             "Default 0.",
                                             0.0, 1.0)},
                {"island_weight",
                 number_property("How much the edges fall away into an island. Default 0.", 0.0,
                                 1.0)},
                {"island_sharpness",
                 number_property("How sharply the island's shore falls. Default 0.", 0.0, 1.0)},
                {"island_height_ratio",
                 number_property("The island's edges' height, -1 (lowest) to 1 (highest, a "
                                 "canyon). Default -1.",
                                 -1.0, 1.0)},
                {"island_shape", number_property("0 round to 1 square. Default 0.", 0.0, 1.0)},
                {"additive",
                 property("boolean", "Add the noise to the current heights. Default false.")},
            },
            {"id"}),
        .handler = terrain_generate,
    });
    json::Value mode;
    mode["enum"] = json::Array{"raise", "lower", "smooth", "flatten", "level",
                               "splat", "color", "hole",   "unhole"};
    mode["description"] = "raise or lower by amount, smooth, flatten to height, level to the "
                          "mean under the brush, splat a texture, color, hole cuts holes and "
                          "unhole fills them.";
    json::Value color = property("array", "The color to paint, [r, g, b] from 0 to 1.");
    color["items"]["type"] = "number";
    color["items"]["minimum"] = 0;
    color["items"]["maximum"] = 1;
    color["minItems"] = 3;
    color["maxItems"] = 3;
    registry.add({
        .name = "terrain.brush",
        .description = "Paints an HTerrain with a round brush at each point [x, z] in turn, as "
                       "HTerrain's painter. Writes the heightmap, splatmap or colormap it "
                       "changes; returns the cells changed. In its space, or the world's with "
                       "global.",
        .capabilities = {Edit, Filesystem},
        .effect = Effect::Edit,
        .params = params_schema(
            {
                {"id", id_property()},
                {"mode", std::move(mode)},
                {"points", points_property("Where each dab goes, as [x, z].")},
                {"radius",
                 number_property("From the middle to the edge. Default 8.", 0.0, 100000.0)},
                {"falloff", number_property("The outer part that eases to nothing, 0 to 1. "
                                            "Default 0.5.",
                                            0.0, 1.0)},
                {"opacity",
                 number_property("How strong each dab is, 0 to 1. Default 1.", 0.0, 1.0)},
                {"amount", number_property("raise and lower: how far at the middle. Default 1.",
                                           0.0, 100000.0)},
                {"height",
                 number_property("flatten: the height to go to. Default 0.", -100000.0, 100000.0)},
                {"texture", integer_property("splat: which of its textures. Default 0.", 0, 3)},
                {"color", std::move(color)},
                {"slope", pair_property("splat and color: only where the ground slopes from "
                                        "[min, max] degrees. Default [0, 90].")},
                {"global", property("boolean", "Points and lengths in the world's space, for a "
                                               "terrain not tilted. Default false.")},
            },
            {"id", "mode", "points"}),
        .handler = terrain_brush,
    });
}

} // namespace slash::command
