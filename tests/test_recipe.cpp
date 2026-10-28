#include "slash/core/json.hpp"
#include "slash/scene3d/model.hpp"
#include "slash/scene3d/recipe.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace json = slash::json;
namespace scene3d = slash::scene3d;

json::Value parse(std::string_view text) {
    auto doc = json::parse(text);
    REQUIRE_MESSAGE(doc.ok(), (doc.ok() ? "" : doc.error().message));
    return std::move(doc).value();
}

scene3d::Model build(std::string_view text) {
    auto model = scene3d::build_model_recipe(parse(text));
    REQUIRE_MESSAGE(model.ok(), (model.ok() ? "" : model.error().message));
    return std::move(model).value();
}

std::vector<json::ValidationError> errors_of(std::string_view text) {
    return scene3d::validate_model_recipe(parse(text));
}

// The first error's path and message, joined, for readable checks.
std::string first_error(std::string_view text) {
    const auto errors = errors_of(text);
    REQUIRE_FALSE(errors.empty());
    return errors.front().path + ": " + errors.front().message;
}

std::size_t triangles(const scene3d::Model& model) {
    std::size_t total = 0;
    for (const auto& mesh : model.meshes) {
        for (const auto& surface : mesh.surfaces) {
            total += surface.indices.size() / 3;
        }
    }
    return total;
}

bool near(float a, float b) {
    return std::abs(a - b) < 1e-4F;
}

constexpr std::string_view kTable = R"({
  "format": 1,
  "parameters": {"width": 1.2, "depth": 0.8, "height": 0.75, "top": 0.05, "leg": "=top * 1.2"},
  "materials": {"wood": {"albedo_color": [0.6, 0.4, 0.2, 1]}, "steel": {"metallic": 1}},
  "parts": [
    {"name": "top", "shape": "box", "material": "wood",
     "size": ["=width", "=top", "=depth"], "position": [0, "=height - top / 2", 0]},
    {"name": "leg", "shape": "cylinder", "material": "steel", "radius": "=leg / 2",
     "height": "=height - top", "position": ["=-width / 2 + leg", "=(height - top) / 2", 0],
     "array": {"count": 2, "position": ["=width - leg * 2", 0, 0]}}
  ]
})";

} // namespace

TEST_CASE("a model recipe builds a node and a mesh per part, sized by its parameters") {
    const scene3d::Model model = build(kTable);
    REQUIRE(model.meshes.size() == 3);
    REQUIRE(model.nodes.size() == 3);
    CHECK(model.nodes[0].name == "top");
    CHECK(model.nodes[1].name == "leg");
    CHECK(model.nodes[2].name == "leg2");
    CHECK(model.parts.size() == 3);
    // Each material is the recipe's, by name.
    REQUIRE(model.materials.size() == 2);
    CHECK(model.materials[0].name == "wood");
    CHECK(model.materials[1].name == "steel");
    CHECK(model.meshes[0].surfaces.at(0).material == 0);
    CHECK(model.meshes[1].surfaces.at(0).material == 1);
    // The legs stand on the floor under the top's corners, which reach the table's height.
    const scene3d::Bounds box = scene3d::bounds(model);
    CHECK(near(box.min.y, 0.0F));
    CHECK(near(box.max.y, 0.75F));
    CHECK(near(box.min.x, -0.6F));
    CHECK(near(box.max.x, 0.6F));
    CHECK(near(model.nodes[2].position.x, 0.6F - 0.06F));
    // Building it again makes the same bits.
    const scene3d::Model again = build(kTable);
    for (std::size_t m = 0; m < model.meshes.size(); ++m) {
        const auto& a = model.meshes[m].surfaces.at(0);
        const auto& b = again.meshes[m].surfaces.at(0);
        CHECK(a.indices == b.indices);
        CHECK(std::ranges::equal(a.vertices, b.vertices, [](const auto& x, const auto& y) {
            return x.position == y.position && x.normal == y.normal;
        }));
    }
}

TEST_CASE("a recipe's children combine with their parent in order, as CSG shapes") {
    const scene3d::Model solid = build(R"({"format": 1, "parts": [
        {"name": "block", "shape": "box", "size": [2, 1, 2]}]})");
    const scene3d::Model holed = build(R"({"format": 1, "parts": [
        {"name": "block", "shape": "box", "size": [2, 1, 2], "children": [
          {"shape": "cylinder", "operation": "subtraction", "radius": 0.5, "height": 2,
           "sides": 16}]}]})");
    CHECK(triangles(holed) > triangles(solid));
    const scene3d::Bounds box = scene3d::bounds(holed);
    CHECK(near(box.max.x, 1.0F));
    CHECK(near(box.max.y, 0.5F));
    // An intersection keeps only what both hold: a box cut down to a cylinder's height.
    const scene3d::Model cut = build(R"({"format": 1, "parts": [
        {"shape": "box", "size": [2, 2, 2], "children": [
          {"shape": "sphere", "operation": "intersection", "radius": 1.2,
           "radial_segments": 24, "rings": 12}]}]})");
    CHECK(cut.nodes.at(0).name == "box");
    CHECK(scene3d::bounds(cut).max.y <= 1.0F + 1e-4F);
}

TEST_CASE("recipes push, spin and sweep polygons, and round boxes' and cylinders' edges") {
    // A lathe: a profile spun around y.
    const scene3d::Model vase = build(R"({"format": 1, "parts": [
        {"name": "vase", "shape": "polygon", "mode": "spin", "spin_sides": 16,
         "polygon": [[0, 0], [0.3, 0], [0.4, 0.5], [0.2, 1], [0, 1]]}]})");
    const scene3d::Bounds vase_box = scene3d::bounds(vase);
    CHECK(near(vase_box.max.y, 1.0F));
    CHECK(vase_box.max.x > 0.35F);
    // An extrusion along -z.
    const scene3d::Model plank = build(R"({"format": 1, "parts": [
        {"shape": "polygon", "depth": 3, "polygon": [[0, 0], [0, 0.2], [1, 0.2], [1, 0]]}]})");
    CHECK(near(scene3d::bounds(plank).min.z, -3.0F));
    // A sweep along a path.
    const scene3d::Model rail = build(R"({"format": 1, "parts": [
        {"shape": "polygon", "mode": "path",
         "polygon": [[-0.1, -0.1], [-0.1, 0.1], [0.1, 0.1], [0.1, -0.1]],
         "path": {"points": [{"position": [0, 0, 0]}, {"position": [0, 0, -4]}]}}]})");
    CHECK(triangles(rail) > 0);
    CHECK(near(scene3d::bounds(rail).min.z, -4.0F));

    // A rounded box fills its size, its flat faces flat and its edges smooth.
    const scene3d::Model rounded = build(R"({"format": 1, "parts": [
        {"shape": "box", "size": [2, 1, 1], "bevel": 0.2, "bevel_segments": 3}]})");
    const scene3d::Bounds rounded_box = scene3d::bounds(rounded);
    CHECK(near(rounded_box.max.x, 1.0F));
    CHECK(near(rounded_box.min.y, -0.5F));
    const scene3d::Model chamfered = build(R"({"format": 1, "parts": [
        {"shape": "box", "size": [2, 1, 1], "bevel": 0.2, "bevel_segments": 1}]})");
    CHECK(triangles(chamfered) < triangles(rounded));
    CHECK(triangles(chamfered) > 12);
    const scene3d::Model drum = build(R"({"format": 1, "parts": [
        {"shape": "cylinder", "radius": 1, "height": 1, "sides": 24, "bevel": 0.1}]})");
    const scene3d::Bounds drum_box = scene3d::bounds(drum);
    CHECK(near(drum_box.max.y, 0.5F));
    CHECK(drum_box.max.x > 0.99F);
    // Capsules and prisms come from Godot's primitive meshes.
    const scene3d::Model pill = build(R"({"format": 1, "parts": [
        {"shape": "capsule", "radius": 0.25, "height": 1, "radial_segments": 16, "rings": 4}]})");
    CHECK(near(scene3d::bounds(pill).max.y, 0.5F));
}

TEST_CASE("a recipe reports each mistake at its pointer, with the closest name") {
    CHECK(first_error(R"({"format": 1, "parameters": {"width": 1}, "parts": [
        {"shape": "box", "size": ["=widht", 1, 1]}]})") ==
          "/parts/0/size/0: expression \"=widht\" reads \"widht\", which isn't a parameter (did "
          "you mean \"width\"?)");
    CHECK(first_error(R"({"format": 1, "parameters": {"a": "=b", "b": "=a"}, "parts": [
        {"shape": "box"}]})") == "/parameters/a: depends on itself: a -> b -> a");
    CHECK(first_error(R"({"format": 1, "materials": {"wood": {}}, "parts": [
        {"shape": "box", "material": "wod"}]})") ==
          "/parts/0/material: \"wod\" isn't in materials (did you mean \"wood\"?)");
    CHECK(first_error(R"({"format": 1, "parts": [{"shape": "box", "radius": 1}]})") ==
          "/parts/0/radius: isn't a field of a box: its fields are size, bevel, bevel_segments");
    CHECK(first_error(R"({"format": 1, "parts": [
        {"shape": "box", "size": [1, 0.2, 1], "bevel": 0.2}]})") ==
          "/parts/0/bevel: is more than half the box's smallest side (0.2 meters)");
    CHECK(first_error(R"({"format": 1, "parts": [{"shape": "box", "size": ["=1 +", 1, 1]}]})")
              .starts_with("/parts/0/size/0: expression \"=1 +\": "));
    CHECK(first_error(R"({"format": 1, "parts": [{"shape": "box", "size": ["one", 1, 1]}]})")
              .starts_with("/parts/0/size/0: "));
    CHECK(first_error(R"({"format": 1, "parameters": {"PI": 3}, "parts": [{"shape": "box"}]})")
              .starts_with("/parameters/PI: isn't a parameter name"));
    // What the CSG can't make is reported at the shape that makes it.
    CHECK(first_error(R"({"format": 1, "parts": [
        {"shape": "box", "children": [{"shape": "box", "size": [2, 2, 2],
                                       "operation": "subtraction"}]}]})")
              .starts_with("/parts/0: makes an empty shape"));
    CHECK(first_error(R"({"format": 1, "parts": [
        {"shape": "polygon", "polygon": [[0, 0], [1, 1]]}]})") ==
          "/parts/0/polygon: has 2 points; a polygon needs at least 3");
    CHECK(errors_of(kTable).empty());
    // Building fails with the first error the schema, an expression or a field gives.
    auto failed = scene3d::build_model_recipe(parse(R"({"format": 1, "parts": []})"));
    REQUIRE_FALSE(failed.ok());
    CHECK(failed.error().message.starts_with("/parts: "));
}

TEST_CASE("a recipe's points are nodes with no mesh, placed in their parts") {
    const scene3d::Model model = build(R"({"format": 1, "parameters": {"width": 0.8},
        "parts": [
          {"name": "body", "shape": "box", "size": ["=width", 1, 0.4], "position": [0, 0.5, 0]},
          {"name": "door", "position": ["=-width / 2", 0.5, 0.2],
           "children": [{"shape": "box", "size": ["=width", 1, 0.02],
                         "position": ["=width / 2", 0, 0.01]}]}],
        "points": [{"name": "handle", "part": "door", "position": ["=width - 0.05", 0, 0.04]},
                   {"name": "top", "position": [0, 1, 0], "rotation": [0, 1, 0]}]})");
    // The points come after the parts' nodes, so the parts keep theirs.
    REQUIRE(model.nodes.size() == 4);
    CHECK(model.nodes[2].name == "handle");
    CHECK(model.nodes[2].parent == 1);
    CHECK(model.nodes[2].position.x == doctest::Approx(0.75));
    CHECK(model.nodes[3].name == "top");
    CHECK(model.nodes[3].parent == -1);

    const scene3d::ModelSummary summary = scene3d::summarize(model);
    CHECK(summary.bounds.min.y == doctest::Approx(0.0));
    CHECK(summary.bounds.max.z == doctest::Approx(0.22));
    CHECK(summary.pivot.x == doctest::Approx(0.5));
    CHECK(summary.pivot.y == doctest::Approx(0.0));
    REQUIRE(summary.parts.size() == 2);
    CHECK(summary.parts[1].name == "door");
    CHECK(summary.parts[1].bounds.min.x == doctest::Approx(-0.4));
    CHECK(summary.parts[0].triangles + summary.parts[1].triangles == summary.triangles);
    REQUIRE(summary.points.size() == 2);
    CHECK(summary.points[0].name == "handle");
    CHECK(summary.points[0].part == "door");
    CHECK(summary.points[0].position.x == doctest::Approx(0.35));
    CHECK(summary.points[0].position.y == doctest::Approx(0.5));
    CHECK(summary.points[0].position.z == doctest::Approx(0.24));
    CHECK(summary.points[1].part.empty());
    CHECK(summary.points[1].rotation.y == doctest::Approx(1.0));

    CHECK(first_error(R"({"format": 1, "parts": [{"name": "door", "shape": "box"}],
        "points": [{"name": "handle", "part": "dor"}]})") ==
          "/points/0/part: names no part (did you mean \"door\"?)");
    CHECK(first_error(R"({"format": 1, "parts": [{"name": "door", "shape": "box"}],
        "points": [{"name": "door"}]})")
              .starts_with("/points/0/name: a part or point is already named \"door\""));
    CHECK(first_error(R"({"format": 1, "parts": [{"shape": "box"}],
        "points": [{"name": "a.b"}]})") ==
          "/points/0/name: a node name can't hold . : @ / \" or %");
    CHECK(first_error(R"({"format": 1, "parameters": {"w": 1}, "parts": [{"shape": "box"}],
        "points": [{"name": "p", "position": ["=x", 0, 0]}]})")
              .starts_with("/points/0/position/0: expression \"=x\" reads \"x\""));
}

TEST_CASE("model recipes load as models, LODs and all, and their textures are checked") {
    const scene3d::ModelReader no_files =
        [](std::string_view /*path*/) -> slash::Result<std::string> {
        return slash::make_error(slash::ErrorCode::NotFound, "no such file");
    };
    CHECK(scene3d::is_model_path("props/table.model.json"));
    const std::string sphere = R"({"format": 1, "materials": {"stone":
        {"albedo_texture": "textures/stone.png"}}, "parts": [
        {"shape": "sphere", "material": "stone", "radial_segments": 32, "rings": 16}]})";
    auto model = scene3d::load_model(sphere, "props/ball.model.json", no_files);
    REQUIRE(model.ok());
    CHECK_FALSE(model.value().meshes.at(0).surfaces.at(0).lods.empty());
    REQUIRE(model.value().images.size() == 1);
    CHECK(model.value().images[0].path == "textures/stone.png");
    CHECK(model.value().materials.at(0).albedo_image == 0);
    CHECK(model.value().materials.at(0).standard.albedo_texture.empty());
    const auto errors = scene3d::check_model(sphere, "props/ball.model.json", no_files);
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].path == "/materials/stone/albedo_texture");
    CHECK(scene3d::check_model("{", "a.model.json", no_files).size() == 1);
}

TEST_CASE("a recipe's model parts place other model files' nodes under their own") {
    std::map<std::string, std::string, std::less<>> files;
    files["props/door.model.json"] = R"({"format": 1,
        "materials": {"oak": {"albedo_color": [0.5, 0.3, 0.1, 1]}},
        "parts": [{"name": "board", "shape": "box", "material": "oak", "size": [0.6, 1, 0.04],
                   "position": [0.3, 0, 0]}],
        "points": [{"name": "knob", "part": "board", "position": [0.25, 0, 0.03]}]})";
    files["props/sign.obj"] = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
    files["props/cabinet.model.json"] = R"({"format": 1, "parts": [
        {"name": "body", "shape": "box", "size": [1.2, 1, 0.5], "position": [0, 0.5, 0]},
        {"model": "props/door.model.json", "position": [-0.6, 0.5, 0.25],
         "array": {"count": 2, "position": [0.6, 0, 0]}},
        {"model": "props/sign.obj", "position": [0, 1, 0]}],
        "points": [{"name": "top", "part": "door2", "position": [0, 0.5, 0]}]})";
    const scene3d::ModelReader read = [&](std::string_view path) -> slash::Result<std::string> {
        const auto it = files.find(path);
        if (it == files.end()) {
            return slash::make_error(slash::ErrorCode::NotFound, "no such file");
        }
        return it->second;
    };
    auto loaded =
        scene3d::load_model(files["props/cabinet.model.json"], "props/cabinet.model.json", read);
    REQUIRE_MESSAGE(loaded.ok(), (loaded.ok() ? "" : loaded.error().message));
    const scene3d::Model& model = loaded.value();
    std::vector<std::string> names;
    std::vector<int> parents;
    for (const scene3d::ModelNode& node : model.nodes) {
        names.push_back(node.name);
        parents.push_back(node.parent);
    }
    // Each copy's nodes under its own, renamed where they would clash; the OBJ model's mesh
    // given a node named as Godot names it; then the points.
    CHECK(names == std::vector<std::string>{"body", "door", "board", "knob", "door2", "board2",
                                            "knob2", "sign", "Mesh", "top"});
    CHECK(parents == std::vector<int>{-1, -1, 1, 2, -1, 4, 5, -1, 7, 4});
    REQUIRE(model.parts.size() == 4);
    CHECK(model.parts[1].node == "board");
    CHECK(model.parts[1].node_index == 2);
    CHECK(model.parts[1].transform.origin.x == doctest::Approx(-0.3));
    CHECK(model.parts[2].node == "board2");
    CHECK(model.parts[2].transform.origin.x == doctest::Approx(0.3));
    CHECK(model.parts[3].node == "Mesh");
    CHECK(model.parts[3].transform.origin.y == doctest::Approx(1.0));
    // The door's material comes along once, shared by both copies.
    REQUIRE(model.materials.size() == 1);
    CHECK(model.materials[0].name == "oak");
    CHECK(model.meshes[model.parts[1].mesh].surfaces[0].material == 0);
    CHECK(model.meshes[model.parts[2].mesh].surfaces[0].material == 0);

    const scene3d::ModelSummary summary = scene3d::summarize(model);
    REQUIRE(summary.points.size() == 3);
    CHECK(summary.points[0].name == "knob");
    CHECK(summary.points[0].part == "board");
    CHECK(summary.points[0].position.x == doctest::Approx(-0.05));
    CHECK(summary.points[2].name == "top");
    CHECK(summary.points[2].position.y == doctest::Approx(1.0));
    CHECK(scene3d::check_model(files["props/cabinet.model.json"], "props/cabinet.model.json", read)
              .empty());

    // A model part takes only its file and where it goes, at the top level.
    CHECK(first_error(R"({"format": 1, "parts": [{"model": "a.glb", "shape": "box"}]})") ==
          "/parts/0/shape: a model part takes only name, position, rotation, scale and array; "
          "its model has its own shapes and materials");
    CHECK(first_error(R"({"format": 1, "parts": [{"shape": "box",
        "children": [{"model": "a.glb"}]}]})")
              .starts_with("/parts/0/children/0/model: only a top-level part can be a model"));
    // Its file must load, and not take in the recipe itself.
    files["props/bad.model.json"] = R"({"format": 1, "parts": [{"model": "props/nope.glb"}]})";
    auto errors = scene3d::check_model(files["props/bad.model.json"], "props/bad.model.json", read);
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].path == "/parts/0/model");
    CHECK(errors[0].message == "\"props/nope.glb\" didn't load: no such file");
    files["props/loop.model.json"] =
        R"({"format": 1, "parts": [{"model": "props/loop.model.json"}]})";
    auto loop = scene3d::load_model(files["props/loop.model.json"], "props/loop.model.json", read);
    REQUIRE_FALSE(loop.ok());
    CHECK(loop.error().message ==
          "/parts/0/model: \"props/loop.model.json\" didn't load: it takes in itself through its "
          "model parts");
    // Without files to read them from.
    auto alone = scene3d::build_model_recipe(parse(files["props/cabinet.model.json"]));
    REQUIRE_FALSE(alone.ok());
    CHECK(alone.error().message.starts_with("/parts/1/model: a model part's file is read"));
    CHECK(errors_of(files["props/cabinet.model.json"]).empty());
}
