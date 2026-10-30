#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/gameplay/host.hpp"
#include "slash/importer/commands.hpp"
#include "slash/importer/import.hpp"
#include "slash/importer/scripts.hpp"
#include "slash/importer/unreal.hpp"
#include "slash/platform/file.hpp"
#include "slash/render/image.hpp"
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
#include <set>
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
    write(folder / "Source/BoxDrop/Dropper.cpp", R"(#include "Dropper.h"

void ADropper::DropNext()
{
    GetWorld()->SpawnActor<AActor>(GetActorLocation(), FRotator::ZeroRotator);
}
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
       "properties": {"class": "/Script/BoxDrop.Dropper", "values": {"Interval": 3.5}},
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

    // The C++ class is a skeleton, as Unity's scripts are: its component on its actor's entity
    // with the interval the level sets, and a system calling its functions.
    CHECK(component(scene, "dropper", "Dropper").find("interval")->as_double() == 3.5);
    const json::Value& made = report.find("classes")->as_array().at(0);
    CHECK(made.find("class")->as_string() == "ADropper");
    CHECK(made.find("code")->as_string() == "src/imported/dropper.cpp");
    CHECK(made.find("entities")->as_int() == 1);
    CHECK(made.find("task")->as_string() == "task/port-dropper");
    CHECK(report.find("module")->as_string() == "src/game.cpp");
    std::string code;
    for (const auto& [path, contents] : plan.files) {
        if (path == "src/imported/dropper.cpp") {
            code = contents;
        }
    }
    CHECK(code.find("float interval = 2.0F; // Interval") != std::string::npos);
    CHECK(code.find("void drop_next(World& /*world*/, entt::entity /*entity*/, Dropper& /*self*/) "
                    "{\n    // GetWorld()->SpawnActor<AActor>(GetActorLocation(), "
                    "FRotator::ZeroRotator);\n}") != std::string::npos);
    CHECK(settings.find("gameplay")->find("sources")->as_array().at(0).as_string() == "src");
}

// A class of "Crate Run": properties levels set, a delegate it broadcasts, an enum, and the
// functions Unreal calls, defined in its source file.
constexpr std::string_view kCoinHeader = R"cpp(#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "CoinPickup.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnCollected, AActor*, Collector, int32, Value);

UENUM(BlueprintType)
enum class ECoinKind : uint8 {
    Copper UMETA(DisplayName = "Copper"),
    Silver,
    Gold = 5
};

/** A coin that spins and gives points when the hero touches it. */
UCLASS()
class CRATERUN_API ACoinPickup : public AActor {
    GENERATED_BODY()

public:
    ACoinPickup();

    /** Points it gives. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0", ClampMax = "100"))
    int32 Points = 10;

    UPROPERTY(EditAnywhere)
    float SpinSpeed = 90.f;

    UPROPERTY(EditInstanceOnly)
    ECoinKind Kind = ECoinKind::Silver;

    UPROPERTY(EditAnywhere)
    AActor* Target = nullptr;

    UPROPERTY(EditAnywhere)
    TArray<AActor*> Friends;

    UPROPERTY(EditAnywhere)
    USoundBase* PickupSound;

    UPROPERTY(VisibleAnywhere)
    UStaticMeshComponent* Mesh;

    UPROPERTY(VisibleAnywhere)
    class USphereComponent* Trigger;

    UPROPERTY(BlueprintAssignable)
    FOnCollected OnCollected;

    UPROPERTY(EditAnywhere)
    bool bRespawns{true};

    UPROPERTY(EditAnywhere)
    FVector Bob = FVector(0, 0, 10);

    static constexpr float MaxSpin = 360.f;

protected:
    virtual void BeginPlay() override;

    virtual void NotifyActorBeginOverlap(AActor* OtherActor) override;

    virtual void NotifyHit(UPrimitiveComponent* MyComp, AActor* Other, UPrimitiveComponent* OtherComp,
                           bool bSelfMoved, FVector HitLocation, FVector HitNormal,
                           FVector NormalImpulse, const FHitResult& Hit) override;

public:
    virtual void Tick(float DeltaTime) override;

    UFUNCTION(BlueprintCallable)
    int32 Collect(AActor* Collector, const FString& Reason);

private:
    float Elapsed = 0.0f;
    int GetDouble() const { return Points * 2; }
};
)cpp";
constexpr std::string_view kCoinSource = R"cpp(#include "CoinPickup.h"

ACoinPickup::ACoinPickup()
    : Super()
{
	PrimaryActorTick.bCanEverTick = true;
	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
}

void ACoinPickup::BeginPlay()
{
    Super::BeginPlay();
    Elapsed = 0.0f;
}

void ACoinPickup::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);
    AddActorLocalRotation(FRotator(0, SpinSpeed * DeltaTime, 0)); // { not a brace
}

void ACoinPickup::NotifyActorBeginOverlap(AActor* OtherActor)
{
    Collect(OtherActor, TEXT("touched"));
}

void ACoinPickup::NotifyHit(UPrimitiveComponent* MyComp, AActor* Other, UPrimitiveComponent* OtherComp, bool bSelfMoved, FVector HitLocation, FVector HitNormal, FVector NormalImpulse, const FHitResult& Hit)
{
    Super::NotifyHit(MyComp, Other, OtherComp, bSelfMoved, HitLocation, HitNormal, NormalImpulse, Hit);
}

int32 ACoinPickup::Collect(AActor* Collector, const FString& Reason)
{
    OnCollected.Broadcast(Collector, Points);
    Destroy();
    return Points;
}
)cpp";

TEST_CASE("an Unreal C++ class reads as a script's skeleton") {
    const importer::Script script = importer::read_unreal_class(
        "unreal/Source/CrateRun/CoinPickup.h", kCoinHeader, kCoinSource, "ACoinPickup");
    CHECK(script.source == importer::ScriptSource::UnrealCpp);
    CHECK(script.extends == "AActor");
    CHECK(script.component == "CoinPickup");
    CHECK(script.ns == "coin_pickup");
    CHECK(script.node);
    CHECK(script.doc == "A coin that spins and gives points when the hero touches it.");

    // Its properties: those levels set, and its state.
    std::map<std::string, const importer::ScriptField*> fields;
    for (const importer::ScriptField& field : script.fields) {
        fields[field.source_name] = &field;
    }
    const importer::ScriptField& points = *fields.at("Points");
    CHECK(points.name == "points");
    CHECK(points.type == "int");
    CHECK(points.init == " = 10");
    CHECK(points.exported);
    CHECK(points.min == 0.0);
    CHECK(points.max == 100.0);
    CHECK(points.doc == "Points it gives.");
    CHECK(fields.at("SpinSpeed")->init == " = 90.0F");
    CHECK(fields.at("Kind")->type == "int");
    CHECK(fields.at("Kind")->init == " = ECoinKind::kSilver");
    CHECK(fields.at("Kind")->exported); // EditInstanceOnly
    CHECK(fields.at("Target")->kind == importer::FieldKind::Entity);
    CHECK(fields.at("Friends")->kind == importer::FieldKind::Entities);
    CHECK(fields.at("PickupSound")->type == "std::string");
    CHECK(fields.at("Mesh")->type == "entt::entity");
    CHECK_FALSE(fields.at("Mesh")->exported);            // VisibleAnywhere
    CHECK(fields.at("Trigger")->type == "entt::entity"); // "class USphereComponent*"
    CHECK(fields.at("bRespawns")->name == "respawns");
    CHECK(fields.at("bRespawns")->init == " = true");
    CHECK(fields.at("Bob")->type == "slash::Vec3");
    CHECK_FALSE(fields.at("Elapsed")->exported);
    CHECK_FALSE(fields.contains("OnCollected"));
    REQUIRE(script.signals.size() == 1);
    CHECK(script.signals[0].name == "OnCollected");
    CHECK(script.signals[0].params.at(0).type == "entt::entity");
    CHECK(script.signals[0].params.at(1).name == "value");
    REQUIRE(script.enums.size() == 1);
    CHECK(script.enums[0].values == std::vector<std::string>{"kCopper", "kSilver", "kGold = 5"});
    REQUIRE(script.constants.size() == 1);
    CHECK(script.constants[0].name == "kMaxSpin");
    CHECK(script.constants[0].value == "360.0F");

    // Its functions, with the bodies its source file defines.
    std::map<std::string, const importer::ScriptFunction*> functions;
    for (const importer::ScriptFunction& function : script.functions) {
        functions[function.source_name] = &function;
    }
    CHECK(functions.at("ACoinPickup")->name == "construct");
    CHECK(functions.at("ACoinPickup")->body.at(0) == "PrimaryActorTick.bCanEverTick = true;");
    CHECK(functions.at("Tick")->body.at(1) ==
          "AddActorLocalRotation(FRotator(0, SpinSpeed * DeltaTime, 0)); // { not a brace");
    CHECK(functions.at("NotifyHit")->params.size() == 1); // the other actor
    CHECK(functions.at("NotifyHit")->params[0].name == "other");
    CHECK(functions.at("Collect")->returns == "int");
    CHECK(functions.at("Collect")->params.at(1).type == "const std::string&");
    CHECK(functions.at("GetDouble")->body.at(0) == "return Points * 2;");

    // The system calls them as Unreal does.
    const std::string code = importer::skeleton(script);
    CHECK(code.find("from the Unreal class unreal/Source/CrateRun/CoinPickup.h") !=
          std::string::npos);
    CHECK(code.find("                construct(world, entity, *self);\n"
                    "                begin_play(world, entity, *self);\n") != std::string::npos);
    CHECK(code.find("ecs.all_of<slash::world::Area3D>(first)") != std::string::npos);
    CHECK(code.find("notify_actor_begin_overlap(world, entity, *self, other);") !=
          std::string::npos);
    CHECK(code.find("notify_hit(world, entity, *self, other);") != std::string::npos);
    CHECK(code.find("tick(world, entity, self, tick_seconds);") != std::string::npos);
    CHECK(code.find(".describe(\"points\", {.doc = \"Points it gives.\", .min = 0.0, .max = "
                    "100.0})") != std::string::npos);
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
    scene3d::Model idle = figure;
    idle.animations.push_back(clip);
    idle.animations.back().name = "Idle";
    write_model(exported / "animations/Game/A_Idle.glb", idle);

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
        "file": "animations/Game/A_Wave.glb", "length": 1.0},
        "/Game/A_Idle": {"class": "AnimSequence", "file": "animations/Game/A_Idle.glb",
        "length": 1.0}})");
    // The statue's Animation Blueprint plays both sequences.
    description["blueprints"].as_array().push_back(
        parse(R"({"path": "/Game/ABP_Figure", "kind": "AnimBlueprint",
        "parent": "/Script/Engine.AnimInstance", "native_parent": "/Script/Engine.AnimInstance",
        "animations": ["/Game/A_Wave", "/Game/A_Idle"], "skeleton": "/Game/SKEL_Figure"})"));
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
    // An Animation Blueprint's sequences are an AnimationTree's states, starting at the idle, for
    // its task to join.
    const json::Value& tree = component(scene, "statue", "AnimationTree");
    CHECK(tree.find("tree_root")->as_string() == "unreal/Content/ABP_Figure.animtree.json");
    CHECK(tree.find("libraries")->find("")->as_string() == "unreal/Content/ABP_Figure.anim.json");
    const json::Value machine = file_json(plan, "unreal/Content/ABP_Figure.animtree.json");
    const json::Value& root = *machine.find("root");
    CHECK(root.find("type")->as_string() == "StateMachine");
    CHECK(root.find("states")->find("Wave") != nullptr);
    CHECK(root.find("states")->find("Idle")->find("node")->find("animation")->as_string() ==
          "Idle");
    REQUIRE(root.find("transitions")->as_array().size() == 1);
    CHECK(root.find("transitions")->as_array()[0].find("to")->as_string() == "Idle");
    const json::Value sequences = file_json(plan, "unreal/Content/ABP_Figure.anim.json");
    CHECK(sequences.find("animations")->find("Idle")->find("from")->find("model")->as_string() ==
          "unreal/Content/A_Idle.glb");
    CHECK(sequences.find("animations")->find("Wave")->find("loop")->as_string() == "linear");
    bool reported = false;
    for (const json::Value& problem : plan.report.find("unsupported")->as_array()) {
        reported = reported || problem.find("what")->as_string() == "Animation Blueprint";
    }
    CHECK(reported);

    CHECK(plan.report.find("converted")->find("animations")->as_int() == 2);
    CHECK(plan.report.find("converted")->find("animation_trees")->as_int() == 1);
    CHECK(plan.report.find("converted")->find("material_variants")->as_int() == 1);
}

namespace {

// A landscape of 5 by 3 vertices whose heights are a + 10b units over its own height at its
// vertex (a, b), and a streaming proxy of it, in the project write_project made with axes.
importer::ImportPlan landscape_import(const fs::path& project, const FileAxes& axes) {
    fs::remove_all(project);
    const fs::path exported = project / "unreal/FakeExport";
    write_project(project / "unreal", axes);
    std::string heights;
    for (int b = 0; b < 3; ++b) {
        for (int a = 0; a < 5; ++a) {
            const int value = 32768 + (128 * (a + (10 * b)));
            heights.push_back(static_cast<char>(value & 0xFF));
            heights.push_back(static_cast<char>(value >> 8));
        }
    }
    write(exported / "landscapes/Game/Maps/Drop/Landscape_0.r16", heights);
    json::Value description =
        parse(slash::platform::read_file((exported / "export.json").string()).value());
    json::Array& actors = description["levels"].as_array().at(0)["actors"].as_array();
    actors.push_back(parse(R"({"name": "Landscape_0", "label": "Hills", "class": "Landscape",
        "class_path": "/Script/Landscape.Landscape", "components": [],
        "transform": {"location": [1000, 500, 200], "rotation": [0, 0, 0, 1],
                      "scale": [100, 100, 50]},
        "landscape": {"heightmap": "landscapes/Game/Maps/Drop/Landscape_0.r16",
                      "size": [5, 3], "min": [-2, 0], "material": "/Game/M_Ground",
                      "layers": ["Grass", "Rock"], "collision": "query_and_physics"}})"));
    actors.push_back(parse(R"({"name": "LandscapeStreamingProxy_0", "label": "Hills Part",
        "class": "LandscapeStreamingProxy",
        "class_path": "/Script/Landscape.LandscapeStreamingProxy", "components": [],
        "transform": {"location": [1000, 500, 200], "rotation": [0, 0, 0, 1],
                      "scale": [100, 100, 50]}})"));
    write(exported / "export.json", json::write(description));
    auto planned = importer::plan_import({.source = (project / "unreal").string(),
                                          .destination = project.string(),
                                          .unpack_to = {},
                                          .assetripper = {},
                                          .unreal_editor = SLASH_FAKE_UNREAL});
    REQUIRE_MESSAGE(planned.ok(), (planned.ok() ? std::string() : planned.error().message));
    return std::move(planned).value();
}

// The file a plan writes at path.
std::string planned_file(const importer::ImportPlan& plan, std::string_view path) {
    for (const auto& [planned, contents] : plan.files) {
        if (planned == path) {
            return contents;
        }
    }
    FAIL("no " << path);
    return {};
}

// The landscape's height values at an HTerrain's (column, row).
std::uint16_t terrain_value(const slash::render::Gray16& heights, int column, int row) {
    return heights.values.at((static_cast<std::size_t>(row) * heights.width) +
                             static_cast<std::size_t>(column));
}

std::uint16_t landscape_value(int a, int b) {
    return static_cast<std::uint16_t>(32768 + (128 * (a + (10 * b))));
}

} // namespace

TEST_CASE("an Unreal landscape becomes an HTerrain") {
    const importer::ImportPlan plan = landscape_import(fs::absolute("import_unreal_land"), {});
    const json::Value scene = file_json(plan, "unreal/Content/Maps/Drop.scene.json");
    CHECK(scene.find("entities")->find("hills_part") == nullptr); // in the landscape's heights
    const json::Value& terrain = component(scene, "hills", "HTerrain");
    CHECK(terrain.find("heightmap")->as_string() == "unreal/Content/Maps/Drop_hills.height.png");
    CHECK(terrain.find("resolution")->as_int() == 65);
    CHECK(terrain.find("height_range")->as_array().at(0).as_double() == -256.0);
    CHECK(terrain.find("height_range")->as_array().at(1).as_double() ==
          doctest::Approx(32767.0 / 128.0));
    CHECK(terrain.find("collision_enabled") == nullptr);
    // Its first vertex, two units along Unreal's -x, is where the HTerrain starts; the actor's
    // scale makes a unit a meter across and half a meter up.
    const json::Value& transform = component(scene, "hills", "Transform3D");
    const auto at = triple(*transform.find("position"));
    CHECK(at[0] == doctest::Approx(8.0));
    CHECK(at[1] == doctest::Approx(2.0));
    CHECK(at[2] == doctest::Approx(5.0));
    CHECK(transform.find("scale") == nullptr);
    const auto scale = triple(*terrain.find("map_scale"));
    CHECK(scale[0] == doctest::Approx(1.0));
    CHECK(scale[1] == doctest::Approx(0.5));
    CHECK(scale[2] == doctest::Approx(1.0));

    // Unreal's values as they are: columns along its x, rows along its y.
    auto heights = slash::render::decode_gray16(
        planned_file(plan, "unreal/Content/Maps/Drop_hills.height.png"));
    REQUIRE(heights.ok());
    CHECK(heights.value().width == 65);
    CHECK(terrain_value(heights.value(), 0, 0) == landscape_value(0, 0));
    CHECK(terrain_value(heights.value(), 4, 0) == landscape_value(4, 0));
    CHECK(terrain_value(heights.value(), 1, 2) == landscape_value(1, 2));
    CHECK(terrain_value(heights.value(), 9, 1) == landscape_value(4, 1)); // past it: its edge
    // Past the landscape is a hole.
    auto color =
        slash::render::decode_image(planned_file(plan, "unreal/Content/Maps/Drop_hills.color.png"));
    REQUIRE(color.ok());
    const auto alpha = [&](int column, int row) {
        return color.value().pixels.at(
            (((static_cast<std::size_t>(row) * 65) + static_cast<std::size_t>(column)) * 4) + 3);
    };
    CHECK(alpha(4, 2) == 255);
    CHECK(alpha(5, 0) == 0);
    CHECK(alpha(0, 3) == 0);

    std::set<std::string> reported;
    for (const json::Value& problem : plan.report.find("unsupported")->as_array()) {
        reported.insert(problem.find("what")->as_string());
    }
    CHECK(reported.contains("landscape material"));
    CHECK(reported.contains("landscape paint layers"));
    CHECK_FALSE(reported.contains("landscape"));
    CHECK(plan.report.find("converted")->find("landscapes")->as_int() == 1);
}

TEST_CASE("an Unreal landscape follows the world's axes") {
    // Unreal's x is the world's z and its y the world's -x: the HTerrain's columns run along
    // the landscape's -y, so its first is the landscape's last.
    const importer::ImportPlan plan = landscape_import(
        fs::absolute("import_unreal_land_axes"), {.axis = {1, 2, 0}, .sign = {-1.0F, 1.0F, 1.0F}});
    const json::Value scene = file_json(plan, "unreal/Content/Maps/Drop.scene.json");
    const auto at = triple(*component(scene, "hills", "Transform3D").find("position"));
    CHECK(at[0] == doctest::Approx(-7.0));
    CHECK(at[1] == doctest::Approx(2.0));
    CHECK(at[2] == doctest::Approx(8.0));
    auto heights = slash::render::decode_gray16(
        planned_file(plan, "unreal/Content/Maps/Drop_hills.height.png"));
    REQUIRE(heights.ok());
    CHECK(terrain_value(heights.value(), 0, 0) == landscape_value(0, 2));
    CHECK(terrain_value(heights.value(), 1, 0) == landscape_value(0, 1));
    CHECK(terrain_value(heights.value(), 0, 1) == landscape_value(1, 2));
    CHECK(terrain_value(heights.value(), 2, 4) == landscape_value(4, 0));
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
    // The game's code, which the import writes: until it is built, its components are stand-ins.
    slash::gameplay::Host code(components, slash::gameplay::library_path(project.string()));
    command::SessionOptions options;
    options.project_root = project.string();
    options.capture_logs = false;
    options.gameplay = &code;
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

TEST_CASE("an Unreal import turns Niagara emitters into Godot's particles") {
    const fs::path project = fs::absolute("import_unreal_niagara");
    fs::remove_all(project);
    write_project(project / "unreal");
    const fs::path exported = project / "unreal/FakeExport";
    json::Value description =
        parse(slash::platform::read_file((exported / "export.json").string()).value());
    // NS_Sparks, as Unreal 5.8's NiagaraExternalEditUtilities describes it: inputs as the text
    // Unreal writes of their values, a random range as its dynamic input chain.
    description["niagara_systems"] = parse(R"json({
      "/Game/FX/NS_Sparks": {
        "random_seed": 42, "determinism": true, "warmup_time": 0.5,
        "fixed_bounds": {"min": [-100, -100, 0], "max": [100, 100, 300]},
        "user_variables": ["(Name=\"User.SmokeRate\",Value=(Value=10.000000))"],
        "emitters": [
          {"name": "Burst", "enabled": true, "sim_target": "gpucomputesim", "data": {},
           "scripts": {
             "EmitterUpdateScript": [
               {"module": "Emitter State", "script": "/Niagara/Modules/Emitter/EmitterState",
                "enabled": true, "inputs": {"Loop Behavior": {"value": "(Value=\"Once\")"}}},
               {"module": "Spawn Burst Instantaneous",
                "script": "/Niagara/Modules/Emitter/SpawnBurst_Instantaneous", "enabled": true,
                "inputs": {"Spawn Count": {"value": "(Value=(Value=40))"},
                           "Spawn Time": {"value": "(Value=(Value=0.000000))"}}}],
             "ParticleSpawnScript": [
               {"module": "Initialize Particle",
                "script": "/Niagara/Modules/Spawn/Initialization/InitializeParticle",
                "enabled": true, "inputs": {
                  "Lifetime": {"value": "(DynamicInput=\"/Niagara/DynamicInputs/RandomRangeFloat\")",
                               "chain": "(Name=\"Lifetime\",Inputs=((Name=\"Minimum\",Value=(Value=0.500000)),(Name=\"Maximum\",Value=(Value=1.500000))))"},
                  "Color": {"value": "(Value=(R=1.000000,G=0.500000,B=0.100000,A=1.000000))"},
                  "Uniform Sprite Size": {"value": "(Value=(Value=8.000000))"}}},
               {"module": "Add Velocity in Cone",
                "script": "/Niagara/Modules/Spawn/Velocity/AddVelocityInCone", "enabled": true,
                "inputs": {"Velocity Strength": {"value": "(Value=(Value=500.000000))"},
                           "Cone Angle": {"value": "(Value=(Value=60.000000))"},
                           "Cone Axis": {"value": "(Value=(X=0.000000,Y=0.000000,Z=1.000000))"}}},
               {"module": "Shape Location",
                "script": "/Niagara/Modules/Spawn/Location/ShapeLocation", "enabled": true,
                "inputs": {"Shape Primitive": {"value": "(Value=\"Sphere\")"},
                           "Sphere Radius": {"value": "(Value=(Value=20.000000))"}}}],
             "ParticleUpdateScript": [
               {"module": "Gravity Force", "script": "/Niagara/Modules/Update/Forces/GravityForce",
                "enabled": true,
                "inputs": {"Gravity": {"value": "(Value=(X=0.000000,Y=0.000000,Z=-980.000000))"}}},
               {"module": "Curl Noise Force",
                "script": "/Niagara/Modules/Update/Forces/CurlNoiseForce", "enabled": true,
                "inputs": {}},
               {"module": "Scale Color", "script": "/Niagara/Modules/Update/Color/ScaleColor",
                "enabled": true, "inputs": {}},
               {"module": "Solve Forces and Velocity",
                "script": "/Niagara/Modules/Solvers/SolveForcesAndVelocity", "enabled": true,
                "inputs": {}}]},
           "renderers": [{"class": "NiagaraSpriteRendererProperties", "data": {}}]},
          {"name": "Smoke", "enabled": true, "sim_target": "cpusim",
           "data": {"bLocalSpace": true},
           "scripts": {
             "EmitterUpdateScript": [
               {"module": "Spawn Rate", "script": "/Niagara/Modules/Emitter/SpawnRate",
                "enabled": true,
                "inputs": {"SpawnRate": {"value": "(LinkedParameter=\"User.SmokeRate\")"}}}],
             "ParticleSpawnScript": [
               {"module": "Initialize Particle",
                "script": "/Niagara/Modules/Spawn/Initialization/InitializeParticle",
                "enabled": true, "inputs": {"Lifetime": {"value": "(Value=(Value=2.000000))"}}},
               {"module": "Add Velocity", "script": "/Niagara/Modules/Spawn/Velocity/AddVelocity",
                "enabled": true, "inputs": {"Velocity": {
                  "value": "(DynamicInput=\"/Niagara/DynamicInputs/RandomRangeVector\")",
                  "chain": "(Inputs=((Name=\"Minimum\",Value=(Value=(X=-10.000000,Y=0.000000,Z=50.000000))),(Name=\"Maximum\",Value=(Value=(X=10.000000,Y=0.000000,Z=150.000000)))))"}}}],
             "ParticleUpdateScript": [
               {"module": "Drag", "script": "/Niagara/Modules/Update/Forces/Drag",
                "enabled": true, "inputs": {"Drag": {"value": "(Value=(Value=1.000000))"}}},
               {"module": "Old Spin", "script": "/Niagara/Modules/Update/Spin",
                "enabled": false, "inputs": {}}]},
           "renderers": [{"class": "NiagaraMeshRendererProperties",
                          "data": {"Meshes": [{"Mesh": "/Game/SM_Crate.SM_Crate"}]},
                          "meshes": ["/Game/SM_Crate"]}]},
          {"name": "Off", "enabled": false, "sim_target": "cpusim"}]},
      "/Game/FX/NS_Old": {"random_seed": 0, "determinism": false, "warmup_time": 0.0}})json");
    json::Array& actors = description["levels"].as_array().at(0)["actors"].as_array();
    actors.push_back(parse(R"({"name": "NiagaraActor_0", "label": "Sparks",
        "class": "NiagaraActor", "class_path": "/Script/Niagara.NiagaraActor",
        "root": "NiagaraComponent0",
        "transform": {"location": [0, 0, 200], "rotation": [0, 0, 0, 1], "scale": [1, 1, 1]},
        "components": [{"name": "NiagaraComponent0", "class": "NiagaraComponent",
          "niagara": {"auto_activate": true, "system": "/Game/FX/NS_Sparks"},
          "transform": {"location": [0, 0, 200], "rotation": [0, 0, 0, 1],
                        "scale": [1, 1, 1]}}]})"));
    actors.push_back(parse(R"({"name": "NiagaraActor_1", "label": "Embers",
        "class": "NiagaraActor", "class_path": "/Script/Niagara.NiagaraActor",
        "root": "NiagaraComponent0",
        "transform": {"location": [0, 300, 0], "rotation": [0, 0, 0, 1], "scale": [1, 1, 1]},
        "components": [{"name": "NiagaraComponent0", "class": "NiagaraComponent",
          "niagara": {"auto_activate": false, "system": "/Game/FX/NS_Old"},
          "transform": {"location": [0, 300, 0], "rotation": [0, 0, 0, 1],
                        "scale": [1, 1, 1]}}]})"));
    actors.push_back(parse(R"({"name": "Emitter_0", "label": "Torch Fire", "class": "Emitter",
        "class_path": "/Script/Engine.Emitter", "components": [],
        "transform": {"location": [0, 0, 0], "rotation": [0, 0, 0, 1], "scale": [1, 1, 1]}})"));
    description["problems"].as_array().push_back(parse(R"({"what": "Niagara emitters",
        "where": "/Game/FX/NS_Old",
        "why": "reading a Niagara system's emitters needs Unreal Engine 5.8 or later"})"));
    write(exported / "export.json", json::write(description));

    auto planned = importer::plan_import({.source = (project / "unreal").string(),
                                          .destination = project.string(),
                                          .unpack_to = {},
                                          .assetripper = {},
                                          .unreal_editor = SLASH_FAKE_UNREAL});
    REQUIRE_MESSAGE(planned.ok(), (planned.ok() ? std::string() : planned.error().message));
    const importer::ImportPlan& plan = planned.value();
    CHECK(plan.report.find("converted")->find("particle_systems")->as_int() == 1);
    const json::Value scene = file_json(plan, "unreal/Content/Maps/Drop.scene.json");

    // Two enabled emitters: an entity each under the component's.
    CHECK(*component(scene, "sparks", "Transform3D").find("children") ==
          parse(R"(["sparks_burst", "sparks_smoke"])"));

    // The GPU emitter: a one-shot burst of 40 with its system's seed, warmup and bounds, drawing
    // camera-facing quads.
    const json::Value& burst = component(scene, "sparks_burst", "GPUParticles3D");
    CHECK(burst.find("amount")->as_int() == 40);
    CHECK(burst.find("explosiveness")->as_double() == 1.0);
    CHECK(burst.find("one_shot")->as_bool());
    CHECK(burst.find("lifetime")->as_double() == 1.5);
    CHECK(burst.find("seed")->as_int() == 42);
    CHECK(burst.find("use_fixed_seed")->as_bool());
    CHECK(burst.find("preprocess")->as_double() == 0.5);
    CHECK(*burst.find("visibility_aabb_position") == parse("[-1.0, 0.0, -1.0]"));
    CHECK(*burst.find("visibility_aabb_size") == parse("[2.0, 3.0, 2.0]"));
    CHECK(component(scene, "sparks_burst", "StandardMaterial3D")
              .find("billboard_mode")
              ->as_string() == "particles");
    (void)component(scene, "sparks_burst", "QuadMesh");
    const std::string material = burst.find("process_material")->as_string();
    CHECK(material == "unreal/Content/FX/NS_Sparks_burst.process_material.json");
    const json::Value process = file_json(plan, material);
    CHECK(process.find("lifetime_randomness")->as_double() == doctest::Approx(0.666667));
    CHECK(*process.find("color") == parse("[1.0, 0.5, 0.1, 1.0]"));
    CHECK(process.find("scale_min")->as_double() == doctest::Approx(0.08));
    CHECK(process.find("initial_velocity_max")->as_double() == doctest::Approx(5.0));
    // Unreal's z is the world's y; the cone's 60 degrees spread 30 each side.
    CHECK(*process.find("direction") == parse("[0.0, 1.0, 0.0]"));
    CHECK(process.find("spread")->as_double() == 30.0);
    CHECK(*process.find("gravity") == parse("[0.0, -9.8, 0.0]"));
    CHECK(process.find("emission_shape")->as_string() == "sphere");
    CHECK(process.find("emission_sphere_radius")->as_double() == doctest::Approx(0.2));
    CHECK(process.find("turbulence_enabled")->as_bool());

    // The CPU emitter: a rate read from its user parameter, a random velocity as a cone, local
    // space, a mesh and no gravity.
    const json::Value& smoke = component(scene, "sparks_smoke", "CPUParticles3D");
    CHECK(smoke.find("amount")->as_int() == 20);
    CHECK(smoke.find("lifetime")->as_double() == 2.0);
    CHECK(*smoke.find("direction") == parse("[0.0, 1.0, 0.0]"));
    CHECK(smoke.find("initial_velocity_min")->as_double() == doctest::Approx(0.509902));
    CHECK(smoke.find("initial_velocity_max")->as_double() == doctest::Approx(1.50333));
    CHECK(smoke.find("spread")->as_double() == doctest::Approx(27.017123));
    CHECK(*smoke.find("gravity") == parse("[0.0, 0.0, 0.0]"));
    CHECK(smoke.find("local_coords")->as_bool());
    CHECK(smoke.find("mesh")->as_string() == "unreal/Content/SM_Crate.glb");
    CHECK(smoke.find("seed")->as_int() == 42);
    CHECK(scene.find("entities")->find("sparks_off") == nullptr);

    // A system an older editor couldn't read leaves its entity, and the export's report.
    CHECK(scene.find("entities")->find("embers")->find("components")->find("CPUParticles3D") ==
          nullptr);
    const auto reported = [&](std::string_view what) {
        for (const json::Value& entry : plan.report.find("unsupported")->as_array()) {
            if (entry.find("what")->as_string() == what) {
                return true;
            }
        }
        return false;
    };
    CHECK(reported("Niagara emitters"));
    CHECK(reported("Niagara module Scale Color"));
    CHECK(reported("Niagara module Drag"));
    CHECK_FALSE(reported("Niagara module Old Spin"));
    CHECK_FALSE(reported("Niagara module Solve Forces and Velocity"));
    CHECK(reported("Niagara curl noise force"));
    CHECK(reported("Niagara random velocity"));
    CHECK(reported("Niagara sprite material"));
    CHECK(reported("Cascade particle system"));
    CHECK_FALSE(reported("invalid value"));
    CHECK(scene.find("entities")->find("torch_fire") == nullptr);
    fs::remove_all(project);
}
