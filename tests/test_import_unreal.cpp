#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/importer/commands.hpp"
#include "slash/importer/import.hpp"
#include "slash/importer/unreal.hpp"
#include "slash/platform/file.hpp"
#include "slash/render/mesh.hpp"
#include "slash/scene3d/model.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <SDL3/SDL_stdinc.h>
#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <map>
#include <numbers>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace json = slash::json;
namespace command = slash::command;
namespace importer = slash::importer;
namespace unreal = slash::importer::unreal;
namespace scene3d = slash::scene3d;
namespace fs = std::filesystem;

namespace {

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? std::string() : parsed.error().message));
    return std::move(parsed).value();
}

void write(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    REQUIRE(slash::platform::write_file(path.string(), text).ok());
}

struct ScopedVariable {
    std::string name;
    ScopedVariable(std::string variable, const std::string& value) : name(std::move(variable)) {
        REQUIRE(SDL_setenv_unsafe(name.c_str(), value.c_str(), 1) == 0);
    }
    ScopedVariable(const ScopedVariable&) = delete;
    ScopedVariable& operator=(const ScopedVariable&) = delete;
    ScopedVariable(ScopedVariable&&) = delete;
    ScopedVariable& operator=(ScopedVariable&&) = delete;
    ~ScopedVariable() { SDL_unsetenv_unsafe(name.c_str()); }
};

// The axes a glTF file is written in, from Unreal's: each of the file's axes is one of Unreal's
// (0 = x, 1 = y, 2 = z), with a sign.
struct FileAxes {
    std::array<int, 3> axis;
    std::array<float, 3> sign;
};
// Unreal's glTF exporter: x, z and y.
constexpr FileAxes kGltfAxes{.axis = {0, 2, 1}, .sign = {1.0F, 1.0F, 1.0F}};

// A box mesh, Unreal's bounds (centimeters) in a file's axes and meters, as the exporter writes a
// static mesh.
scene3d::Model box_model(std::array<float, 3> low, std::array<float, 3> high,
                         const FileAxes& axes) {
    std::array<float, 3> a{};
    std::array<float, 3> b{};
    for (std::size_t i = 0; i < 3; ++i) {
        const auto from = static_cast<std::size_t>(axes.axis.at(i));
        a.at(i) = std::min(low.at(from) * axes.sign.at(i), high.at(from) * axes.sign.at(i)) / 100;
        b.at(i) = std::max(low.at(from) * axes.sign.at(i), high.at(from) * axes.sign.at(i)) / 100;
    }
    scene3d::ModelMesh mesh;
    mesh.name = "Mesh";
    scene3d::ModelSurface& surface = mesh.surfaces.emplace_back();
    const auto corner = [&](std::size_t c) {
        return std::array<float, 3>{(c & 1U) != 0 ? b[0] : a[0], (c & 2U) != 0 ? b[1] : a[1],
                                    (c & 4U) != 0 ? b[2] : a[2]};
    };
    const std::array<std::array<std::size_t, 4>, 6> faces{
        {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 4, 6, 2}, {1, 3, 7, 5}, {0, 1, 5, 4}, {2, 6, 7, 3}}};
    for (const auto& ring : faces) {
        const auto first = static_cast<std::uint32_t>(surface.vertices.size());
        for (const std::size_t c : ring) {
            slash::render::Vertex3D vertex;
            const std::array<float, 3> at = corner(c);
            vertex.position = {at[0], at[1], at[2]};
            surface.vertices.push_back(vertex);
        }
        for (const std::uint32_t i : {0U, 2U, 1U, 0U, 3U, 2U}) {
            surface.indices.push_back(first + i);
        }
    }
    scene3d::Model model;
    model.meshes.push_back(mesh);
    scene3d::ModelNode node;
    node.name = "Mesh";
    model.nodes.push_back(node);
    model.parts.push_back(
        {.node = "Mesh", .mesh = 0, .transform = {}, .node_index = 0, .skin = -1});
    return model;
}

void write_model(const fs::path& path, const scene3d::Model& model) {
    const scene3d::ModelReader none = [](std::string_view) -> slash::Result<std::string> {
        return slash::make_error(slash::ErrorCode::NotFound, "no files");
    };
    write(path, scene3d::write_glb(model, path.string(), none).value());
}

void write_box(const fs::path& path, std::array<float, 3> low, std::array<float, 3> high,
               const FileAxes& axes) {
    write_model(path, box_model(low, high, axes));
}

// A box drawn with one material, of a name and a color.
scene3d::Model painted_box(std::array<float, 3> low, std::array<float, 3> high,
                           std::string_view material, std::array<float, 4> color) {
    scene3d::Model model = box_model(low, high, kGltfAxes);
    scene3d::ModelMaterial& paint = model.materials.emplace_back();
    paint.name = material;
    paint.standard.albedo_color = {color[0], color[1], color[2], color[3]};
    model.meshes.at(0).surfaces.at(0).material = 0;
    return model;
}

// A small Unreal project, "Box Drop", with what its editor's export would write in FakeExport:
// a level with a floor, a crate Unreal simulates, a lamp, a sun, a trigger box, a pawn the player
// possesses with no camera, a Blueprint and a C++ class.
void write_project(const fs::path& folder, const FileAxes& axes = kGltfAxes) {
    write(folder / "BoxDrop.uproject", R"({"FileVersion": 3, "EngineAssociation": "5.4",
        "Modules": [{"Name": "BoxDrop", "Type": "Runtime"}]})");
    write(folder / "Config/DefaultEngine.ini", R"([/Script/EngineSettings.GameMapsSettings]
GameDefaultMap=/Game/Maps/Drop.Drop

[/Script/Engine.PhysicsSettings]
DefaultGravityZ=-980.0
)");
    write(folder / "Config/DefaultGame.ini", R"([/Script/EngineSettings.GeneralProjectSettings]
ProjectName=Box Drop
ProjectVersion=0.3.1.7
)");
    write(folder / "Config/DefaultInput.ini", R"([/Script/Engine.InputSettings]
+ActionMappings=(ActionName="Drop",bShift=False,bCtrl=True,bAlt=False,bCmd=False,Key=D)
+AxisMappings=(AxisName="Slide",Scale=-1.000000,Key=Left)
+AxisMappings=(AxisName="Slide",Scale=1.000000,Key=Right)
)");
    write(folder / "Source/BoxDrop/Dropper.h", R"(#pragma once
#include "GameFramework/Actor.h"
#include "Dropper.generated.h"

UCLASS()
class BOXDROP_API ADropper : public AActor {
    GENERATED_BODY()
public:
    UPROPERTY(EditAnywhere)
    float Interval = 2.0f;

    UFUNCTION(BlueprintCallable)
    void DropNext();
};
)");
    write(folder / "Content/Blueprints/BP_Player.uasset", "stand-in");
    write_box(folder / "FakeExport/meshes/Game/SM_Floor.glb", {-500, -500, -10}, {500, 500, 0},
              axes);
    write_box(folder / "FakeExport/meshes/Game/SM_Crate.glb", {-50, -30, 0}, {50, 30, 100}, axes);
    write(folder / "FakeExport/export.json", R"({
  "format": 1,
  "engine_version": "5.4.4-35576357+++UE5+Release-5.4",
  "project": {"name": "BoxDrop"},
  "levels": [{
    "path": "/Game/Maps/Drop",
    "kill_z": -500.0,
    "actors": [
      {"name": "WorldSettings", "label": "WorldSettings", "class": "WorldSettings",
       "class_path": "/Script/Engine.WorldSettings", "components": [],
       "transform": {"location": [0, 0, 0], "rotation": [0, 0, 0, 1], "scale": [1, 1, 1]}},
      {"name": "StaticMeshActor_0", "label": "Floor", "class": "StaticMeshActor",
       "class_path": "/Script/Engine.StaticMeshActor", "root": "StaticMeshComponent0",
       "transform": {"location": [0, 0, 0], "rotation": [0, 0, 0, 1], "scale": [1, 1, 1]},
       "components": [{"name": "StaticMeshComponent0", "class": "StaticMeshComponent",
         "mesh": "/Game/SM_Floor", "collision": "query_and_physics", "profile": "BlockAll",
         "mobility": "static",
         "transform": {"location": [0, 0, 0], "rotation": [0, 0, 0, 1], "scale": [1, 1, 1]}}]},
      {"name": "StaticMeshActor_1", "label": "Crate", "class": "StaticMeshActor",
       "class_path": "/Script/Engine.StaticMeshActor", "root": "StaticMeshComponent0",
       "transform": {"location": [200, 100, 300], "rotation": [0, 0, 0.7071068, 0.7071068],
                     "scale": [1, 1, 1]},
       "components": [{"name": "StaticMeshComponent0", "class": "StaticMeshComponent",
         "mesh": "/Game/SM_Crate", "collision": "query_and_physics", "profile": "PhysicsActor",
         "mobility": "movable", "simulate": true, "mass": 40.0, "linear_damping": 0.01,
         "angular_damping": 0.0, "gravity": true,
         "transform": {"location": [200, 100, 300], "rotation": [0, 0, 0.7071068, 0.7071068],
                       "scale": [1, 1, 1]}}]},
      {"name": "PointLight_0", "label": "Lamp", "class": "PointLight",
       "class_path": "/Script/Engine.PointLight", "root": "LightComponent0",
       "transform": {"location": [0, -200, 400], "rotation": [0, 0, 0, 1], "scale": [1, 1, 1]},
       "components": [{"name": "LightComponent0", "class": "PointLightComponent",
         "light": {"type": "point", "units": "lumens", "intensity": 1000.0,
                   "color": [255, 255, 255], "attenuation_radius": 1500.0,
                   "source_radius": 5.0, "shadows": true},
         "transform": {"location": [0, -200, 400], "rotation": [0, 0, 0, 1],
                       "scale": [1, 1, 1]}}]},
      {"name": "DirectionalLight_0", "label": "Sun", "class": "DirectionalLight",
       "class_path": "/Script/Engine.DirectionalLight", "root": "LightComponent0",
       "transform": {"location": [0, 0, 1000], "rotation": [0, 0.7071068, 0, 0.7071068],
                     "scale": [1, 1, 1]},
       "components": [{"name": "LightComponent0", "class": "DirectionalLightComponent",
         "light": {"type": "directional", "intensity": 5.0, "color": [255, 255, 255],
                   "shadows": false},
         "transform": {"location": [0, 0, 1000], "rotation": [0, 0.7071068, 0, 0.7071068],
                       "scale": [1, 1, 1]}}]},
      {"name": "TriggerBox_0", "label": "Bin", "class": "TriggerBox",
       "class_path": "/Script/Engine.TriggerBox", "root": "CollisionComp", "tags": ["bin"],
       "transform": {"location": [-300, 0, 50], "rotation": [0, 0, 0, 1], "scale": [1, 1, 1]},
       "components": [{"name": "CollisionComp", "class": "BoxComponent", "collision": "query_only",
         "profile": "Trigger", "shape": {"type": "box", "extent": [50, 100, 50]},
         "transform": {"location": [-300, 0, 50], "rotation": [0, 0, 0, 1],
                       "scale": [1, 1, 1]}}]},
      {"name": "BP_Player_C_0", "label": "Player", "class": "BP_Player_C",
       "class_path": "/Game/Blueprints/BP_Player.BP_Player_C", "root": "DefaultSceneRoot",
       "auto_possess": "player0",
       "transform": {"location": [0, 0, 100], "rotation": [0, 0, 0, 1], "scale": [1, 1, 1]},
       "components": [{"name": "DefaultSceneRoot", "class": "SceneComponent",
         "transform": {"location": [0, 0, 100], "rotation": [0, 0, 0, 1], "scale": [1, 1, 1]}},
        {"name": "Arrow", "class": "ArrowComponent", "parent": "DefaultSceneRoot",
         "transform": {"location": [0, 0, 0], "rotation": [0, 0, 0, 1], "scale": [1, 1, 1]}}]},
      {"name": "Dropper_0", "label": "Dropper", "class": "Dropper",
       "class_path": "/Script/BoxDrop.Dropper",
       "transform": {"location": [0, 0, 500], "rotation": [0, 0, 0, 1], "scale": [1, 1, 1]},
       "components": []}
    ]
  }],
  "blueprints": [{"path": "/Game/Blueprints/BP_Player", "kind": "Blueprint",
                  "parent": "/Script/Engine.Pawn", "native_parent": "/Script/Engine.Pawn"}],
  "input_actions": {},
  "input_contexts": {},
  "meshes": {
    "/Game/SM_Floor": {"file": "meshes/Game/SM_Floor.glb", "skeletal": false,
      "bounds": {"origin": [0, 0, -5], "extent": [500, 500, 5]},
      "collision_complexity": "ctf_use_complex_as_simple", "simple_collision": 0,
      "convex_collision": 0, "materials": []},
    "/Game/SM_Crate": {"file": "meshes/Game/SM_Crate.glb", "skeletal": false,
      "bounds": {"origin": [0, 0, 50], "extent": [50, 30, 50]},
      "collision_complexity": "ctf_use_default", "simple_collision": 1,
      "convex_collision": 0, "materials": []}
  },
  "sounds": {},
  "problems": []
}
)");
}

json::Value file_json(const importer::ImportPlan& plan, std::string_view path) {
    for (const auto& [planned, contents] : plan.files) {
        if (planned == path) {
            return parse(contents);
        }
    }
    FAIL("no " << path);
    return {};
}

const json::Value& component(const json::Value& scene, std::string_view id, std::string_view name) {
    const json::Value* entity = scene.find("entities")->find(id);
    REQUIRE_MESSAGE(entity != nullptr, id);
    const json::Value* found = entity->find("components")->find(name);
    REQUIRE_MESSAGE(found != nullptr, (std::string(id) + " " + std::string(name)));
    return *found;
}

std::array<double, 3> triple(const json::Value& value) {
    const json::Array& items = value.as_array();
    return {items.at(0).as_double(), items.at(1).as_double(), items.at(2).as_double()};
}

} // namespace

TEST_CASE("Unreal config files read as Unreal applies their lines") {
    const unreal::Config config = unreal::parse_config(R"(; a comment
[/Script/Engine.InputSettings]
+ActionMappings=(ActionName="Jump",Key=SpaceBar)
+ActionMappings=(ActionName="Fire",Key=LeftMouseButton)
-ActionMappings=(ActionName="Jump",Key=SpaceBar)
.ActionMappings=(ActionName="Fire",Key=LeftMouseButton)
Name="Quoted value"

[Other]
!List=ClearArray
+List=a
+List=b
Key=1
Key=2
)");
    CHECK(config.values("/Script/Engine.InputSettings", "ActionMappings") ==
          std::vector<std::string>{"(ActionName=\"Fire\",Key=LeftMouseButton)",
                                   "(ActionName=\"Fire\",Key=LeftMouseButton)"});
    CHECK(config.value("/Script/Engine.InputSettings", "Name") == "Quoted value");
    CHECK(config.values("Other", "List") == std::vector<std::string>{"a", "b"});
    CHECK(config.value("Other", "Key") == "2");
    CHECK(config.value("Missing", "Key").empty());

    const auto fields = unreal::parse_struct(
        R"((AxisName="Move Forward",Scale=-1.000000,Key=Gamepad_LeftY,Nested=(A=1,B="x,y")))");
    REQUIRE(fields.size() == 4);
    CHECK(fields[0] == std::pair<std::string, std::string>{"AxisName", "Move Forward"});
    CHECK(fields[1].second == "-1.000000");
    CHECK(fields[2].second == "Gamepad_LeftY");
    CHECK(fields[3] == std::pair<std::string, std::string>{"Nested", "(A=1,B=\"x,y\")"});
}

TEST_CASE("the Unreal Editor is the one given, then SLASH_UNREAL_EDITOR's") {
    const fs::path folder = fs::absolute("unreal_editor_lookup");
    fs::remove_all(folder);
    write(folder / "Game/Game.uproject", R"({"EngineAssociation": "5.3"})");
    CHECK(unreal::find_project(folder / "Game") == folder / "Game/Game.uproject");
    CHECK_FALSE(unreal::find_project(folder).has_value());

    // An engine folder as the launcher installs it, given by its folder or its program.
#if defined(_WIN32)
    const fs::path program = folder / "UE_5.3/Engine/Binaries/Win64/UnrealEditor-Cmd.exe";
#elif defined(__APPLE__)
    const fs::path program =
        folder / "UE_5.3/Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor";
#else
    const fs::path program = folder / "UE_5.3/Engine/Binaries/Linux/UnrealEditor-Cmd";
#endif
    write(program, "");
    const fs::path project = folder / "Game/Game.uproject";
    auto by_folder = unreal::find_editor(project, (folder / "UE_5.3").string());
    REQUIRE(by_folder.ok());
    CHECK(fs::path(by_folder.value().program) == program);
    CHECK(by_folder.value().version == "5.3");
    auto by_program = unreal::find_editor(project, program.string());
    REQUIRE(by_program.ok());
    CHECK(by_program.value().version == "5.3");

    const ScopedVariable named("SLASH_UNREAL_EDITOR", program.string());
    auto from_variable = unreal::find_editor(project, "");
    REQUIRE(from_variable.ok());
    CHECK(fs::path(from_variable.value().program) == program);

    auto missing = unreal::find_editor(project, (folder / "nowhere").string());
    REQUIRE_FALSE(missing.ok());
    CHECK(missing.error().message.ends_with("nowhere, which doesn't exist"));
    auto empty = unreal::find_editor(project, folder.string());
    REQUIRE_FALSE(empty.ok());
    CHECK(empty.error().message.find("a folder without Unreal Editor's program") !=
          std::string::npos);

    const std::vector<std::string> run = unreal::export_command(
        {.program = "UnrealEditor-Cmd", .version = "5.3"}, project, folder / "unreal_export.py");
    CHECK(run.at(1) == project.generic_string());
    CHECK(run.at(2) == "-run=pythonscript");
    CHECK(run.at(3) == "-script=" + (folder / "unreal_export.py").generic_string());
    CHECK(unreal::export_script().find("SLASH_UNREAL_EXPORT") != std::string_view::npos);
}

TEST_CASE("an Unreal project imports through its editor into scenes, settings and tasks") {
    const fs::path project = fs::absolute("import_unreal");
    fs::remove_all(project);
    write_project(project / "unreal");
    const ScopedVariable log("FAKE_UNREAL_LOG", (project / "editor.log").string());
    auto planned = importer::plan_import({.source = (project / "unreal").string(),
                                          .destination = project.string(),
                                          .unpack_to = {},
                                          .assetripper = {},
                                          .unreal_editor = SLASH_FAKE_UNREAL});
    REQUIRE_MESSAGE(planned.ok(), (planned.ok() ? std::string() : planned.error().message));
    const importer::ImportPlan& plan = planned.value();

    // The editor ran the export script on the project, headless.
    const std::string ran = slash::platform::read_file((project / "editor.log").string()).value();
    CHECK(ran.find("BoxDrop.uproject -run=pythonscript -script=") != std::string::npos);
    CHECK(ran.find("-unattended") != std::string::npos);

    const json::Value& report = plan.report;
    CHECK(report.find("engine")->as_string() == "unreal");
    CHECK(report.find("project")->as_string() == "Box Drop");
    CHECK(report.find("main_scene")->as_string() == "unreal/Content/Maps/Drop.scene.json");
    CHECK(report.find("converted")->find("meshes")->as_int() == 2);

    // Unreal's centimeters, z up, are Godot's meters, y up: x stays, Unreal's y is z.
    const json::Value scene = file_json(plan, "unreal/Content/Maps/Drop.scene.json");
    const json::Value& crate = component(scene, "crate", "Transform3D");
    CHECK(triple(*crate.find("position")) == std::array<double, 3>{2.0, 3.0, 1.0});
    CHECK(triple(*crate.find("rotation"))[1] == doctest::Approx(-1.570796));
    CHECK(component(scene, "crate", "RigidBody3D").find("mass")->as_double() == 40.0);
    CHECK(component(scene, "crate", "MeshInstance3D").find("mesh")->as_string() ==
          "unreal/Content/SM_Crate.glb");
    CHECK(component(scene, "floor", "MeshInstance3D").find("mesh")->as_string() ==
          "unreal/Content/SM_Floor.glb");
    // 1000 lumens are 1000 / 4π candelas, Unreal's 8 cd being energy 1.
    const json::Value& lamp = component(scene, "lamp", "OmniLight3D");
    CHECK(lamp.find("light_energy")->as_double() ==
          doctest::Approx(1000.0 / (4.0 * std::numbers::pi) / 8.0));
    CHECK(lamp.find("omni_range")->as_double() == doctest::Approx(15.0));
    CHECK(triple(*component(scene, "lamp", "Transform3D").find("position")) ==
          std::array<double, 3>{0.0, 4.0, -2.0});
    // The sun points down Unreal's x, pitched 90 degrees: straight down.
    CHECK(component(scene, "sun", "DirectionalLight3D").find("light_energy")->as_double() ==
          doctest::Approx(0.5));
    CHECK(triple(*component(scene, "sun", "Transform3D").find("rotation"))[0] ==
          doctest::Approx(-1.570796));
    // A trigger box is an area, as big as Unreal's extent twice.
    CHECK(component(scene, "bin", "Area3D").is_object());
    CHECK(triple(*component(scene, "bin", "CollisionShape3D").find("size")) ==
          std::array<double, 3>{1.0, 1.0, 2.0});
    // The player's pawn had no camera, so one is added where its eyes would be; its arrow
    // shows only in Unreal's editor.
    CHECK(component(scene, "player_camera", "Camera3D").find("current")->as_bool());
    CHECK(scene.find("entities")->find("player_arrow") == nullptr);
    CHECK(scene.find("entities")->find("world_settings") == nullptr);

    const json::Value settings = file_json(plan, "project.json");
    CHECK(settings.find("name")->as_string() == "Box Drop");
    CHECK(settings.find("version")->as_string() == "0.3.1");
    CHECK(settings.find("physics_3d")->find("gravity")->as_double() == doctest::Approx(9.8));
    const json::Value* input = settings.find("input");
    REQUIRE(input != nullptr);
    CHECK(input->find("Drop")->find("events")->as_array().at(0).find("ctrl")->as_bool());
    CHECK(
        input->find("Slide/negative")->find("events")->as_array().at(0).find("key")->as_string() ==
        "Left");
    CHECK(input->find("Slide/positive") != nullptr);

    const json::Value unreal_report = file_json(plan, "import/unreal.json");
    CHECK(unreal_report.find("axes")->as_string() == "x = x, y = z, z = y");
    CHECK(unreal_report.find("meters_per_unit")->as_double() == doctest::Approx(0.01));
    const json::Value* level = unreal_report.find("levels")->find("/Game/Maps/Drop");
    CHECK(level->find("kill_y")->as_double() == doctest::Approx(-5.0));
    CHECK(level->find("camera_added")->as_string() == "player_camera");
    CHECK(level->find("actors")->find("bin")->find("tags")->as_array().at(0).as_string() == "bin");

    // A task per Blueprint and C++ class.
    std::vector<std::string> tasks;
    for (const json::Value& task : report.find("tasks")->as_array()) {
        tasks.push_back(task.find("task")->as_string());
    }
    CHECK(tasks == std::vector<std::string>{"task/port-bp-player", "task/port-dropper"});
}

TEST_CASE("an Unreal import draws meshes' override materials and plays their animations") {
    const fs::path project = fs::absolute("import_unreal_looks");
    fs::remove_all(project);
    const fs::path exported = project / "unreal/FakeExport";
    write_project(project / "unreal");
    // The crate draws M_Wood, and a second crate draws it red.
    write_model(exported / "meshes/Game/SM_Crate.glb",
                painted_box({-50, -30, 0}, {50, 30, 100}, "M_Wood", {0.6F, 0.4F, 0.2F, 1.0F}));
    write_model(exported / "materials/Game/M_Red.glb",
                painted_box({-50, -50, -50}, {50, 50, 50}, "M_Red", {1.0F, 0.0F, 0.0F, 1.0F}));
    // A figure and a sequence that turns it, exported on its skeleton.
    const scene3d::Model figure = box_model({-20, -20, 0}, {20, 20, 180}, kGltfAxes);
    write_model(exported / "meshes/Game/SK_Figure.glb", figure);
    scene3d::Model wave = figure;
    scene3d::ModelAnimation& clip = wave.animations.emplace_back();
    clip.name = "Wave";
    clip.length = 1.0;
    clip.tracks.push_back(
        {.node = 0,
         .property = scene3d::NodeProperty::Rotation,
         .step = false,
         .loop_wrap = true,
         .times = {0.0F, 1.0F},
         .values = {{0.0F, 0.0F, 0.0F, 1.0F}, {0.0F, 0.7071068F, 0.0F, 0.7071068F}}});
    write_model(exported / "animations/Game/A_Wave.glb", wave);

    json::Value description =
        parse(slash::platform::read_file((exported / "export.json").string()).value());
    json::Value& meshes = description["meshes"];
    meshes["/Game/SM_Crate"]["materials"] = json::Array{json::Value("/Game/M_Wood")};
    meshes["/Game/SK_Figure"] = parse(R"({"file": "meshes/Game/SK_Figure.glb",
        "skeletal": true, "bounds": {"origin": [0, 0, 90], "extent": [20, 20, 90]},
        "materials": []})");
    description["materials"] =
        parse(R"({"/Game/M_Red": {"class": "Material", "file": "materials/Game/M_Red.glb"}})");
    description["animations"] = parse(R"({"/Game/A_Wave": {"class": "AnimSequence",
        "file": "animations/Game/A_Wave.glb", "length": 1.0}})");
    json::Array& actors = description["levels"].as_array().at(0)["actors"].as_array();
    actors.push_back(parse(R"({"name": "StaticMeshActor_2", "label": "Red Crate",
        "class": "StaticMeshActor", "class_path": "/Script/Engine.StaticMeshActor",
        "root": "StaticMeshComponent0",
        "transform": {"location": [300, 0, 0], "rotation": [0, 0, 0, 1], "scale": [1, 1, 1]},
        "components": [{"name": "StaticMeshComponent0", "class": "StaticMeshComponent",
          "mesh": "/Game/SM_Crate", "override_materials": ["/Game/M_Red"],
          "collision": "query_and_physics", "profile": "BlockAll", "mobility": "static",
          "transform": {"location": [300, 0, 0], "rotation": [0, 0, 0, 1],
                        "scale": [1, 1, 1]}}]})"));
    actors.push_back(parse(R"({"name": "SkeletalMeshActor_0", "label": "Figure",
        "class": "SkeletalMeshActor", "class_path": "/Script/Engine.SkeletalMeshActor",
        "root": "SkeletalMeshComponent0",
        "transform": {"location": [-200, 0, 0], "rotation": [0, 0, 0, 1], "scale": [1, 1, 1]},
        "components": [{"name": "SkeletalMeshComponent0", "class": "SkeletalMeshComponent",
          "mesh": "/Game/SK_Figure", "collision": "no_collision", "mobility": "movable",
          "animation": {"mode": "single_node", "animation": "/Game/A_Wave", "looping": true,
                        "playing": true, "position": 0.25, "play_rate": 2.0},
          "transform": {"location": [-200, 0, 0], "rotation": [0, 0, 0, 1],
                        "scale": [1, 1, 1]}}]})"));
    actors.push_back(parse(R"({"name": "SkeletalMeshActor_1", "label": "Statue",
        "class": "SkeletalMeshActor", "class_path": "/Script/Engine.SkeletalMeshActor",
        "root": "SkeletalMeshComponent0",
        "transform": {"location": [-400, 0, 0], "rotation": [0, 0, 0, 1], "scale": [1, 1, 1]},
        "components": [{"name": "SkeletalMeshComponent0", "class": "SkeletalMeshComponent",
          "mesh": "/Game/SK_Figure", "collision": "no_collision", "mobility": "movable",
          "animation": {"mode": "blueprint", "blueprint": "/Game/ABP_Figure"},
          "transform": {"location": [-400, 0, 0], "rotation": [0, 0, 0, 1],
                        "scale": [1, 1, 1]}}]})"));
    write(exported / "export.json", json::write(description));

    auto planned = importer::plan_import({.source = (project / "unreal").string(),
                                          .destination = project.string(),
                                          .unpack_to = {},
                                          .assetripper = {},
                                          .unreal_editor = SLASH_FAKE_UNREAL});
    REQUIRE_MESSAGE(planned.ok(), (planned.ok() ? std::string() : planned.error().message));
    const importer::ImportPlan& plan = planned.value();
    const auto file = [&](std::string_view path) {
        for (const auto& [planned_path, contents] : plan.files) {
            if (planned_path == path) {
                return contents;
            }
        }
        FAIL("no " << path);
        return std::string();
    };
    const json::Value scene = file_json(plan, "unreal/Content/Maps/Drop.scene.json");

    // The red crate draws a copy of the crate's mesh with M_Red in M_Wood's place.
    CHECK(component(scene, "crate", "MeshInstance3D").find("mesh")->as_string() ==
          "unreal/Content/SM_Crate.glb");
    CHECK(component(scene, "red_crate", "MeshInstance3D").find("mesh")->as_string() ==
          "unreal/Content/SM_Crate_M_Red.glb");
    const scene3d::ModelReader none = [](std::string_view) -> slash::Result<std::string> {
        return slash::make_error(slash::ErrorCode::NotFound, "no files");
    };
    auto red = scene3d::load_model(file("unreal/Content/SM_Crate_M_Red.glb"),
                                   "unreal/Content/SM_Crate_M_Red.glb", none);
    REQUIRE(red.ok());
    REQUIRE(red.value().materials.size() == 1);
    CHECK(red.value().materials[0].name == "M_Red");
    CHECK(red.value().materials[0].standard.albedo_color.x == doctest::Approx(1.0));
    CHECK(red.value().materials[0].standard.albedo_color.y == doctest::Approx(0.0));
    // It still collides as the crate's mesh does.
    CHECK_FALSE(red.value().collision.empty());

    // The figure plays its sequence from where Unreal left it, looping, at twice the speed.
    const json::Value& player = component(scene, "figure", "AnimationPlayer");
    CHECK(player.find("library")->as_string() == "unreal/Content/A_Wave.anim.json");
    CHECK(player.find("animation")->as_string() == "Wave");
    CHECK(player.find("speed_scale")->as_double() == doctest::Approx(2.0));
    CHECK(player.find("time")->as_double() == doctest::Approx(0.25));
    const json::Value library = file_json(plan, "unreal/Content/A_Wave.anim.json");
    const json::Value* animation = library.find("animations")->find("Wave");
    REQUIRE(animation != nullptr);
    CHECK(animation->find("from")->find("model")->as_string() == "unreal/Content/A_Wave.glb");
    CHECK(animation->find("loop")->as_string() == "linear");
    CHECK_FALSE(file("unreal/Content/A_Wave.glb").empty());
    // An Animation Blueprint is left for its task.
    CHECK(scene.find("entities")->find("statue")->find("components")->find("AnimationPlayer") ==
          nullptr);
    bool reported = false;
    for (const json::Value& problem : plan.report.find("unsupported")->as_array()) {
        reported = reported || problem.find("what")->as_string() == "Animation Blueprint";
    }
    CHECK(reported);

    CHECK(plan.report.find("converted")->find("animations")->as_int() == 1);
    CHECK(plan.report.find("converted")->find("material_variants")->as_int() == 1);
}

TEST_CASE("an Unreal export's axes and scale come from its meshes") {
    // A glTF exporter that writes Unreal's x as the file's z and its y as -x.
    const fs::path project = fs::absolute("import_unreal_axes");
    fs::remove_all(project);
    write_project(project / "unreal", {.axis = {1, 2, 0}, .sign = {-1.0F, 1.0F, 1.0F}});
    auto planned = importer::plan_import({.source = (project / "unreal").string(),
                                          .destination = project.string(),
                                          .unpack_to = {},
                                          .assetripper = {},
                                          .unreal_editor = SLASH_FAKE_UNREAL});
    REQUIRE_MESSAGE(planned.ok(), (planned.ok() ? std::string() : planned.error().message));
    const json::Value unreal_report = file_json(planned.value(), "import/unreal.json");
    CHECK(unreal_report.find("axes")->as_string() != "x = x, y = z, z = y");
    // The world follows the meshes: the crate at Unreal's (200, 100, 300) is at (-1, 3, 2).
    const json::Value scene = file_json(planned.value(), "unreal/Content/Maps/Drop.scene.json");
    const auto at = triple(*component(scene, "crate", "Transform3D").find("position"));
    CHECK(at[0] == doctest::Approx(-1.0));
    CHECK(at[1] == doctest::Approx(3.0));
    CHECK(at[2] == doctest::Approx(2.0));
}

TEST_CASE("an Unreal import reports what its editor got wrong") {
    const fs::path project = fs::absolute("import_unreal_failures");
    fs::remove_all(project);
    write_project(project / "unreal");
    const importer::ImportOptions options{.source = (project / "unreal").string(),
                                          .destination = project.string(),
                                          .unpack_to = {},
                                          .assetripper = {},
                                          .unreal_editor = SLASH_FAKE_UNREAL};
    {
        const ScopedVariable fail("FAKE_UNREAL_FAIL", "script");
        auto failed = importer::plan_import(options);
        REQUIRE_FALSE(failed.ok());
        CHECK(failed.error().message.find("RuntimeError: the level wouldn't open") !=
              std::string::npos);
    }
    // An editor that never ran the script.
    fs::remove_all(project / "unreal/FakeExport");
    auto silent = importer::plan_import(options);
    REQUIRE_FALSE(silent.ok());
    CHECK(silent.error().message.find("PythonScriptPlugin is not enabled") != std::string::npos);
    // No editor.
    auto none = importer::plan_import({.source = (project / "unreal").string(),
                                       .destination = project.string(),
                                       .unpack_to = {},
                                       .assetripper = {},
                                       .unreal_editor = (project / "nowhere").string()});
    REQUIRE_FALSE(none.ok());
}

TEST_CASE("project.import runs Unreal's editor only where it may") {
    const fs::path project = fs::absolute("import_unreal_command");
    fs::remove_all(project);
    write_project(project / "unreal");
    const ScopedVariable editor("SLASH_UNREAL_EDITOR", SLASH_FAKE_UNREAL);
    slash::world::ComponentRegistry components;
    slash::world::register_builtin_components(components);
    command::Registry commands;
    command::register_builtin_commands(commands);
    importer::register_import_commands(commands);
    command::SessionOptions options;
    options.project_root = project.string();
    options.capture_logs = false;
    options.capabilities = {command::Capability::Read, command::Capability::Filesystem};
    const json::Value params = parse(R"({"source": "unreal"})");

    command::Session limited(components, commands, options);
    const command::Reply refused = limited.call("project.import", params);
    CHECK_FALSE(refused.ok);
    CHECK(refused.code == command::code::kPermissionDenied);
    CHECK(refused.data.find("capability")->as_string() == "build");

    command::Registry running;
    command::register_builtin_commands(running);
    importer::register_import_commands(running, {.unpack_builds = false});
    options.capabilities = command::SessionOptions{}.capabilities;
    command::Session game(components, running, options);
    CHECK_FALSE(game.call("project.import", params).ok);

    command::Session session(components, commands, options);
    const command::Reply done = session.call("project.import", params);
    REQUIRE_MESSAGE(done.ok, done.message);
    CHECK(fs::exists(project / "unreal/Content/Maps/Drop.scene.json"));
    CHECK(fs::exists(project / "unreal/Content/SM_Crate.glb"));
    const command::Reply opened =
        session.call("scene.open", parse(R"({"path": "unreal/Content/Maps/Drop.scene.json"})"));
    REQUIRE_MESSAGE(opened.ok, opened.message);
    // The crate falls onto the floor and rests on it.
    REQUIRE(session.call("step", parse(R"({"ticks": 120})")).ok);
    const command::Reply crate =
        session.call("entity.get", parse(R"({"id": "crate", "components": ["Transform3D"]})"));
    REQUIRE(crate.ok);
    const auto at = triple(*crate.value.find("components")->find("Transform3D")->find("position"));
    CHECK(at[1] == doctest::Approx(0.0).epsilon(0.03));
}
