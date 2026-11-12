// Characters: .character.json recipes, their measures, the rigged models and 2D cut-outs they
// build, and character.generate, character.check and character.place.
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/platform/file.hpp"
#include "slash/scene3d/character.hpp"
#include "slash/scene3d/model.hpp"
#include "slash/world/character.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace command = slash::command;
namespace json = slash::json;
namespace scene3d = slash::scene3d;
namespace world = slash::world;

namespace {

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE_MESSAGE(parsed.ok(), text);
    return std::move(parsed).value();
}

// A knight in armor with a cape and a sword, a farmer in a dress, and the farmer's twin, who
// changes only her name.
constexpr std::string_view kKnight = R"({"format": 1, "name": "Knight", "role": "player",
    "body": {"height": 1.9, "build": 0.8, "shoulders": 1.2, "skin": [0.62, 0.44, 0.31]},
    "hair": {"style": "short", "color": [0.06, 0.05, 0.05]},
    "face": {"beard": "short"},
    "clothing": [{"kind": "shirt", "color": [0.4, 0.1, 0.1]},
                 {"kind": "pants", "color": [0.2, 0.2, 0.25]},
                 {"kind": "boots", "color": [0.3, 0.3, 0.32], "material": "metal"},
                 {"kind": "armor", "color": [0.7, 0.7, 0.72], "material": "metal"},
                 {"kind": "cape", "color": [0.5, 0.05, 0.05], "cloth": true}],
    "props": [{"model": "sword.model.json", "point": "right_hand", "rotation": [1.5708, 0, 0]}]})";
constexpr std::string_view kFarmer = R"({"format": 1, "name": "Farmer",
    "body": {"height": 1.6, "build": 0.3, "hips": 1.1, "skin": [0.93, 0.76, 0.62]},
    "hair": {"style": "ponytail", "color": [0.85, 0.72, 0.45], "length": 0.7},
    "clothing": [{"kind": "dress", "color": [0.25, 0.5, 0.3], "length": 0.8, "loose": 0.4},
                 {"kind": "shoes", "color": [0.35, 0.22, 0.12], "material": "leather"},
                 {"kind": "hat", "color": [0.85, 0.75, 0.4], "length": 0.2}]})";
constexpr std::string_view kTwin = R"({"format": 1, "extends": "farmer.character.json",
    "name": "Twin"})";
constexpr std::string_view kSword = R"({"format": 1,
    "materials": {"steel": {"albedo_color": [0.8, 0.8, 0.85, 1], "metallic": 1}},
    "parts": [{"name": "blade", "shape": "box", "size": [0.04, 0.9, 0.01],
               "position": [0, 0.45, 0], "material": "steel"}]})";

struct Project {
    std::string root;
    world::ComponentRegistry components;
    command::Registry commands;
    std::unique_ptr<command::Session> session;

    Project(std::string name, const std::map<std::string, std::string_view>& files)
        : root(std::move(name)) {
        std::filesystem::remove_all(root);
        for (const auto& [file, text] : files) {
            const std::string path = root + "/" + file;
            REQUIRE(slash::platform::create_directories(
                        std::filesystem::path(path).parent_path().string())
                        .ok());
            REQUIRE(slash::platform::write_file(path, text).ok());
        }
        REQUIRE(slash::platform::write_file(root + "/main.scene.json",
                                            R"({"format": 1, "entities": {
              "camera": {"components": {"Transform3D": {"position": [0, 1, 4]},
                                        "Camera3D": {"current": true}}}}})")
                    .ok());
        world::register_builtin_components(components);
        command::register_builtin_commands(commands);
        command::SessionOptions options;
        options.project_root = root;
        options.capture_logs = false;
        session = std::make_unique<command::Session>(components, commands, std::move(options));
        ok("scene.open", R"({"path": "main.scene.json"})");
    }
    Project(const Project&) = delete;
    Project& operator=(const Project&) = delete;
    Project(Project&&) = delete;
    Project& operator=(Project&&) = delete;
    ~Project() {
        session.reset();
        std::filesystem::remove_all(root);
    }

    json::Value ok(std::string_view name, std::string_view params = "{}") {
        command::Reply reply = session->call(name, parse(params));
        REQUIRE_MESSAGE(reply.ok, reply.message);
        return std::move(reply.value);
    }

    command::Reply call(std::string_view name, std::string_view params) {
        return session->call(name, parse(params));
    }
};

const std::map<std::string, std::string_view> kFiles{{"characters/knight.character.json", kKnight},
                                                     {"characters/farmer.character.json", kFarmer},
                                                     {"sword.model.json", kSword}};

world::CharacterRecipe recipe_of(std::string_view text) {
    auto recipe = world::parse_character(parse(text), nullptr);
    REQUIRE_MESSAGE(recipe.ok(), recipe.error().message);
    return std::move(recipe).value();
}

scene3d::ModelReader files_reader(const std::map<std::string, std::string_view>& files) {
    return [files](std::string_view path) -> slash::Result<std::string> {
        const auto found = files.find(std::string(path));
        if (found == files.end()) {
            return slash::make_error(slash::ErrorCode::NotFound, "no such file");
        }
        return std::string(found->second);
    };
}

// The model's height and width at rest.
std::pair<float, float> extent(const scene3d::Model& model) {
    float low = 1e9F;
    float high = -1e9F;
    float left = 1e9F;
    float right = -1e9F;
    for (const scene3d::ModelMesh& mesh : model.meshes) {
        for (const scene3d::ModelSurface& surface : mesh.surfaces) {
            for (const auto& vertex : surface.vertices) {
                low = std::min(low, vertex.position[1]);
                high = std::max(high, vertex.position[1]);
                left = std::min(left, vertex.position[0]);
                right = std::max(right, vertex.position[0]);
            }
        }
    }
    return {high - low, right - left};
}

} // namespace

TEST_CASE("character: recipes are checked: one garment a place, one cloth, one prop a point") {
    const auto errors = [](std::string_view text) {
        return world::validate_character(parse(text));
    };
    CHECK(errors(kKnight).empty());
    std::vector<json::ValidationError> found = errors(R"({"format": 1, "clothing": [
        {"kind": "jacket"}, {"kind": "coat"}]})");
    REQUIRE(found.size() == 1);
    CHECK(found[0].path == "/clothing/1/kind");
    found = errors(R"({"format": 1, "clothing": [{"kind": "shirt", "cloth": true}]})");
    REQUIRE(found.size() == 1);
    CHECK(found[0].path == "/clothing/0/cloth");
    found = errors(R"({"format": 1, "clothing": [{"kind": "skirt", "cloth": true},
        {"kind": "cape", "cloth": true}]})");
    REQUIRE(found.size() == 1);
    CHECK(found[0].path == "/clothing/1/cloth");
    found = errors(R"({"format": 1, "props": [{"model": "a.glb", "point": "back"},
        {"model": "b.glb", "point": "back"}]})");
    REQUIRE(found.size() == 1);
    CHECK(found[0].path == "/props/1/point");
    CHECK_FALSE(errors(R"({"format": 1, "body": {"height": 9}})").empty());
}

TEST_CASE("character: a recipe extends another as a merge patch") {
    const std::map<std::string, std::string_view> files{{"farmer.character.json", kFarmer}};
    auto twin = world::parse_character(parse(kTwin), files_reader(files));
    REQUIRE(twin.ok());
    CHECK(twin.value().name == "Twin");
    CHECK(twin.value().body.height == doctest::Approx(1.6));
    CHECK(twin.value().hair.style == "ponytail");
    REQUIRE(twin.value().clothing.size() == 3);
    CHECK(twin.value().clothing[0].kind == "dress");
    // A file that extends itself is refused.
    const std::map<std::string, std::string_view> loop{
        {"a.character.json", R"({"format": 1, "extends": "a.character.json"})"}};
    CHECK_FALSE(world::parse_character(parse(R"({"format": 1, "extends": "a.character.json"})"),
                                       files_reader(loop))
                    .ok());
}

TEST_CASE("character: measures put the head's top at the recipe's height") {
    const world::CharacterRecipe knight = recipe_of(kKnight);
    const world::CharacterMeasures m = world::character_measures(knight);
    CHECK(m.top == doctest::Approx(1.9));
    CHECK(m.ankle < m.knee);
    CHECK(m.knee < m.hip);
    CHECK(m.hip < m.waist);
    CHECK(m.waist < m.chest);
    CHECK(m.chest < m.shoulder);
    CHECK(m.shoulder < m.head_base);
    CHECK(m.head_base + m.head_height == doctest::Approx(m.top));
    // Each garment stands further out than the one under it.
    CHECK(world::garment_offset(knight, 3) > world::garment_offset(knight, 0));
}

TEST_CASE("character: a recipe builds a rigged humanoid with its points, layers and props") {
    auto model = scene3d::build_character(recipe_of(kKnight), files_reader(kFiles));
    REQUIRE_MESSAGE(model.ok(), model.error().message);
    const scene3d::Model& knight = model.value();
    // Godot's humanoid bones, skinned, then the points under their bones.
    REQUIRE(knight.skins.size() == 1);
    CHECK(knight.skins[0].joints.size() == 23);
    const auto node = [&](std::string_view name) {
        return std::ranges::find_if(knight.nodes, [&](const auto& n) { return n.name == name; });
    };
    for (const std::string_view name :
         {"Hips", "Head", "LeftHand", "RightFoot", "right_hand", "back", "head_top"}) {
        CHECK_MESSAGE(node(name) != knight.nodes.end(), name);
    }
    CHECK(knight.nodes.at(static_cast<std::size_t>(node("right_hand")->parent)).name ==
          "RightHand");
    // Materials by what they draw: the cape free to hang, the props' own.
    std::vector<std::string> materials;
    for (const scene3d::ModelMaterial& material : knight.materials) {
        materials.push_back(material.name);
    }
    for (const std::string_view name :
         {"skin", "eyes", "lips", "hair", "shirt", "armor", "cape", "steel"}) {
        CHECK_MESSAGE(std::ranges::find(materials, name) != materials.end(), name);
    }
    const auto armor = std::ranges::find(materials, "armor") - materials.begin();
    CHECK(knight.materials.at(static_cast<std::size_t>(armor)).standard.metallic == 1.0F);
    // Every vertex follows one joint.
    for (const scene3d::ModelSurface& surface : knight.meshes.at(0).surfaces) {
        REQUIRE(surface.weights.size() == surface.vertices.size());
    }
    // As tall as the recipe says, give or take the hair; a sword hangs from the hand.
    const auto [height, width] = extent(knight);
    CHECK(height > 1.85F);
    CHECK(height < 2.0F);
    CHECK(width > 0.5F);
    // The same recipe builds the same model.
    auto again = scene3d::build_character(recipe_of(kKnight), files_reader(kFiles));
    REQUIRE(again.ok());
    const auto& first = knight.meshes.at(0).surfaces.at(0).vertices;
    const auto& second = again.value().meshes.at(0).surfaces.at(0).vertices;
    REQUIRE(first.size() == second.size());
    CHECK(std::memcmp(first.data(), second.data(), first.size() * sizeof(first[0])) == 0);
    // A prop that can't be read says so.
    auto no_sword = scene3d::build_character(recipe_of(kKnight), files_reader({}));
    REQUIRE_FALSE(no_sword.ok());
    CHECK(no_sword.error().message.find("sword.model.json") != std::string::npos);
}

TEST_CASE("character: a groom grows on a scalp of its own, and loads as any model") {
    constexpr std::string_view kGroomed = R"({"format": 1, "hair": {"groom": "long.groom.json"}})";
    auto model = scene3d::load_model(kGroomed, "groomed.character.json", files_reader({}));
    REQUIRE_MESSAGE(model.ok(), model.error().message);
    const auto has = [&](std::string_view name) {
        return std::ranges::any_of(model.value().materials,
                                   [&](const auto& m) { return m.name == name; });
    };
    CHECK(has("scalp"));
    CHECK_FALSE(has("hair"));
    CHECK(scene3d::is_model_path("a/b.character.json"));
    // check_model reports a recipe's problems at their pointers.
    const auto problems =
        scene3d::check_model(R"({"format": 1, "props": [{"model": "gone.glb", "point": "back"}]})",
                             "a.character.json", files_reader({}));
    REQUIRE(problems.size() == 1);
    CHECK(problems[0].path == "/props/0/model");
}

TEST_CASE("character: outlines tell builds apart, and colors are measured in Oklab") {
    const auto reader = files_reader(kFiles);
    auto knight = scene3d::build_character(recipe_of(kKnight), reader);
    auto farmer = scene3d::build_character(recipe_of(kFarmer), reader);
    REQUIRE(knight.ok());
    REQUIRE(farmer.ok());
    const scene3d::Silhouette a = scene3d::silhouette(knight.value(), false);
    const scene3d::Silhouette b = scene3d::silhouette(farmer.value(), false);
    CHECK(scene3d::silhouette_difference(a, a) == 0.0F);
    CHECK(scene3d::silhouette_difference(a, b) > 0.2F);
    std::size_t covered = 0;
    for (const auto cell : a.cells) {
        covered += cell;
    }
    CHECK(covered > 200);
    CHECK(world::color_difference({1, 0, 0, 1}, {1, 0, 0, 1}) == 0.0F);
    CHECK(world::color_difference({0, 0, 0, 1}, {1, 1, 1, 1}) ==
          doctest::Approx(100.0F).epsilon(0.01));
    CHECK(world::color_difference({0.5F, 0.5F, 0.5F, 1}, {0.51F, 0.5F, 0.5F, 1}) < 2.0F);
}

TEST_CASE("character: a 2D cut-out keeps bones in a tree, the far limbs behind") {
    const std::vector<world::CutoutBone> bones = world::character_cutout(recipe_of(kFarmer));
    REQUIRE_FALSE(bones.empty());
    CHECK(bones[0].name == "pelvis");
    CHECK(bones[0].parent == -1);
    for (std::size_t i = 1; i < bones.size(); ++i) {
        CHECK(bones[i].parent >= 0);
        CHECK(static_cast<std::size_t>(bones[i].parent) < i);
    }
    const auto find = [&](std::string_view name) {
        return *std::ranges::find_if(bones, [&](const auto& b) { return b.name == name; });
    };
    CHECK(find("upper_arm_back").z_index < find("chest").z_index);
    CHECK(find("upper_arm_front").z_index > find("chest").z_index);
    CHECK_FALSE(find("head").shapes.empty());
}

TEST_CASE("character: a Character2D keeps its cut-out's parts as Polygon2D children") {
    Project project("character_2d_project", kFiles);
    json::Value placed = project.ok(
        "character.place",
        R"({"recipe": "characters/farmer.character.json", "id": "farmer", "at": [200, 300]})");
    CHECK(placed["components"].as_array().size() == 2);
    project.ok("step", R"({"ticks": 1})");
    world::World& world = project.session->world();
    const entt::entity head = world.find("farmer_head");
    REQUIRE((head != entt::null));
    const auto* polygon = world.ecs().try_get<world::Polygon2D>(head);
    REQUIRE(polygon != nullptr);
    CHECK(polygon->polygon.size() > 3);
    CHECK_FALSE(polygon->polygons.empty());
    // Parts hang in a tree under the character, scaled to pixels.
    const auto& root = world.ecs().get<world::Transform2D>(world.find("farmer"));
    CHECK(std::ranges::find(root.children, world.find("farmer_pelvis")) != root.children.end());
    const auto& pelvis = world.ecs().get<world::Transform2D>(world.find("farmer_pelvis"));
    CHECK(pelvis.position.y < -50.0F);
    // They draw.
    json::Value summary = project.ok("frame.summary", "{}");
    CHECK(json::write(summary).find("farmer_head") != std::string::npos);
    // A changed recipe rebuilds them; the parts stay where animations put them.
    project.ok("component.set", R"({"id": "farmer", "components": {"Character2D": {
        "recipe": "characters/knight.character.json"}}})");
    project.ok("step", R"({"ticks": 1})");
    CHECK((world.find("farmer_head") != entt::null));
}

TEST_CASE("character: character.place draws a 3D character with its cloth and groom") {
    std::map<std::string, std::string_view> files = kFiles;
    files["groomed.character.json"] = R"({"format": 1, "hair": {"groom": "long.groom.json"}})";
    files["long.groom.json"] = R"({"format": 1, "guides": 6, "length": 0.15})";
    Project project("character_3d_project", files);
    json::Value placed = project.ok(
        "character.place",
        R"({"recipe": "characters/knight.character.json", "id": "knight", "at": [0, 0, 0]})");
    CHECK(placed["components"] == parse(R"(["Transform3D", "MeshInstance3D", "Cloth3D"])"));
    CHECK(project.session->world()
              .ecs()
              .get<world::Cloth3D>(project.session->world().find("knight"))
              .section == "cape");
    placed = project.ok(
        "character.place",
        R"({"recipe": "groomed.character.json", "id": "groomed", "at": [1, 0, 0], "yaw": 1})");
    CHECK(placed["components"].as_array().back().as_string() == "HairInstance3D");
    json::Value frame = project.ok("frame.capture", R"({"points": [[0, 0]]})");
    CHECK(frame["drawn"]["meshes"].as_int() >= 2);
    // Errors name the bad param.
    command::Reply again = project.call(
        "character.place",
        R"({"recipe": "characters/knight.character.json", "id": "knight", "at": [0, 0, 0]})");
    CHECK_FALSE(again.ok);
    CHECK(again.message.starts_with("/id"));
    again = project.call("character.place", R"({"recipe": "nope.glb", "id": "x", "at": [0, 0]})");
    CHECK(again.message.starts_with("/recipe"));
}

TEST_CASE("character: character.check holds characters to the design rules") {
    std::map<std::string, std::string_view> files = kFiles;
    files["characters/twin.character.json"] = R"({"format": 1,
        "extends": "characters/farmer.character.json", "name": "Twin"})";
    files["storyboard/design.json"] = R"({"format": 1, "entries": {
        "character_rules/cast": {"name": "Cast", "silhouette_difference": 0.1,
            "color_difference": 8, "max_height": 1.8, "max_props": 0,
            "links": ["characters"]}}})";
    Project project("character_check_project", files);
    json::Value checked = project.ok("character.check", R"({"measures": true})");
    CHECK(checked["characters"].as_int() == 3);
    const json::Array& errors = checked["errors"].as_array();
    const auto has = [&](std::string_view file, std::string_view path, std::string_view words) {
        return std::ranges::any_of(errors, [&](const json::Value& error) {
            return error.find("file")->as_string() == file &&
                   error.find("path")->as_string() == path &&
                   error.find("message")->as_string().find(words) != std::string::npos;
        });
    };
    // The twin looks just like the farmer.
    CHECK(has("characters/twin.character.json", "", "outline is too like"));
    CHECK(has("characters/twin.character.json", "", "colors are too like"));
    // The knight is too tall and carries a sword.
    CHECK(has("characters/knight.character.json", "/body/height", "character_rules/cast"));
    CHECK(has("characters/knight.character.json", "/props", "allows 0"));
    CHECK(errors.size() == 4);
    CHECK(checked["pairs"].as_array().size() == 3);
    // test.run fails on them too.
    json::Value run = project.ok("test.run", "{}");
    CHECK(json::write(run).find("outline is too like") != std::string::npos);
}

TEST_CASE("character: character.generate writes a character unlike the others") {
    std::map<std::string, std::string_view> files = kFiles;
    files["storyboard/design.json"] = R"({"format": 1, "entries": {
        "character_rules/cast": {"name": "Cast", "palette": "palette/cast",
            "min_height": 1.6, "max_height": 1.7}}})";
    files["storyboard/style.json"] = R"({"format": 1, "entries": {
        "palette/cast": {"name": "Cast", "colors": {"tan": "#c89870", "brown": "#5a3a22",
            "red": "#a02020", "blue": "#2040a0", "cream": "#f0e8d0", "black": "#101010"}}}})";
    Project project("character_generate_project", files);
    json::Value made =
        project.ok("character.generate", R"({"name": "Mira Vale", "role": "npc", "seed": 3})");
    CHECK(made["out"].as_string() == "characters/mira_vale.character.json");
    CHECK(made["height"].as_double() >= 1.6);
    CHECK(made["height"].as_double() <= 1.7);
    CHECK(made["nearest"]["silhouette"].as_double() > 0.0);
    CHECK(made.find("warning") == nullptr);
    // It keeps the palette and builds; check passes on it.
    json::Value checked =
        project.ok("character.check", R"({"paths": ["characters/mira_vale.character.json"]})");
    CHECK(checked["errors"].as_array().empty());
    // The same name and seed make the same file; undo takes it away.
    auto first = slash::platform::read_file(project.root + "/characters/mira_vale.character.json");
    REQUIRE(first.ok());
    project.ok("undo");
    CHECK_FALSE(slash::platform::exists(project.root + "/characters/mira_vale.character.json"));
    project.ok("character.generate", R"({"name": "Mira Vale", "seed": 3})");
    auto second = slash::platform::read_file(project.root + "/characters/mira_vale.character.json");
    REQUIRE(second.ok());
    CHECK(first.value() == second.value());
}
