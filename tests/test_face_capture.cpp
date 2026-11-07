#include "gltf.hpp"

#include "slash/command/builtin.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/xr/face.hpp"

#include <doctest/doctest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace slash;

namespace {

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? std::string() : parsed.error().message));
    return std::move(parsed).value();
}

std::size_t shape(std::string_view name) {
    const std::optional<std::size_t> found = xr::face_shape_named(name);
    REQUIRE_MESSAGE(found.has_value(), name);
    return *found;
}

std::string read(const std::filesystem::path& path) {
    std::ifstream in(path);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// A project with a face whose blend shapes are named as ARKit's and Unified Expressions' (the
// test quad's "smile" and "wide" renamed), followed by an XRFaceModifier3D.
struct FaceProject {
    std::filesystem::path root;
    world::ComponentRegistry components;
    command::Registry commands;
    std::unique_ptr<command::Session> session;

    explicit FaceProject(std::string name) : root(std::move(name)) {
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root / "models");
        std::ofstream(root / "project.json") << R"({"name": "Face", "main_scene": "main.scene.json",
            "xr": {"openxr": {"enabled": true}}})";
        test::Gltf face = test::face();
        face.document["meshes"].as_array()[0]["extras"]["targetNames"] =
            parse(R"(["MouthSmile", "jawOpen"])");
        std::ofstream(root / "models/face.gltf") << face.gltf();
        std::ofstream(root / "main.scene.json") << R"({
  "entities": {
    "face": {"components": {"MeshInstance3D": {"mesh": "models/face.gltf"}, "Transform3D": {},
                            "XRFaceModifier3D": {}}},
    "mirror": {"components": {"XRFaceModifier3D": {"target": "twin"}}},
    "twin": {"components": {"MeshInstance3D": {"mesh": "models/face.gltf"}, "Transform3D": {}}}
  },
  "format": 1
})";
        world::register_builtin_components(components);
        command::register_builtin_commands(commands);
        command::register_run_commands(commands);
        command::SessionOptions options;
        options.project_root = root.string();
        options.capture_logs = false;
        session = std::make_unique<command::Session>(components, commands, std::move(options));
    }
    FaceProject(const FaceProject&) = delete;
    FaceProject& operator=(const FaceProject&) = delete;
    ~FaceProject() {
        session.reset();
        std::filesystem::remove_all(root);
    }

    json::Value ok(std::string_view name, std::string_view params) {
        command::Reply reply = session->call(name, parse(params));
        REQUIRE_MESSAGE(reply.ok, reply.message);
        return std::move(reply.value);
    }

    command::Reply call(std::string_view name, std::string_view params) {
        return session->call(name, parse(params));
    }

    double blend_shape(std::string_view entity, std::string_view name) {
        json::Value got = ok("entity.get", R"({"id": ")" + std::string(entity) +
                                               R"(", "components": ["MeshInstance3D"]})");
        const json::Value* shapes =
            got.find("components")->find("MeshInstance3D")->find("blend_shapes");
        const json::Value* value = shapes != nullptr ? shapes->find(name) : nullptr;
        return value != nullptr ? value->as_double() : -1.0;
    }
};

} // namespace

TEST_CASE("face: names map to face shapes as Godot's XRFaceModifier3D finds them") {
    // Unified Expressions, ARKit, SRanipal and Meta's names, in any case, underscores ignored.
    CHECK(xr::face_shape_name(shape("JawOpen")) == "JawOpen");
    CHECK(shape("jawOpen") == shape("JawOpen"));
    CHECK(shape("jaw_open") == shape("JawOpen"));
    CHECK(shape("eyeBlinkLeft") == shape("EyeClosedLeft"));
    CHECK_FALSE(xr::face_shape_named("smile").has_value());
    CHECK(xr::face_shape_parts(shape("JawOpen")).empty());
    CHECK(xr::face_shape_parts(shape("MouthSmile")).size() == 4);

    // A blended shape whose parts the mesh has is left to them, so the face moves once.
    const std::vector<std::string> names{"MouthSmile", "mouthSmileLeft", "mouthSmileRight",
                                         "jawOpen", "smile"};
    const auto mapping = xr::face_blend_mapping(names);
    CHECK(mapping.size() == 4);
    CHECK(mapping.contains("MouthSmile"));
    CHECK(mapping.at("jawOpen") == shape("JawOpen"));
    CHECK_FALSE(mapping.contains("smile"));
    const std::vector<std::string> blended{"EyeClosedLeft", "EyeClosedRight", "EyeClosed"};
    CHECK_FALSE(xr::face_blend_mapping(blended).contains("EyeClosed"));
}

TEST_CASE("face: shapes a recording gives fill in the shapes they imply") {
    std::array<float, xr::kFaceShapeCount> weights{};
    std::array<bool, xr::kFaceShapeCount> given{};
    weights.at(shape("mouthSmileLeft")) = 0.8F;
    weights.at(shape("mouthSmileRight")) = 0.4F;
    given.at(shape("mouthSmileLeft")) = true;
    given.at(shape("mouthSmileRight")) = true;
    xr::fill_blended_shapes(weights, given);
    // The parts of each smile take its value, then the whole smile is their mean.
    for (const std::size_t part : xr::face_shape_parts(shape("mouthSmileLeft"))) {
        CHECK(given.at(part));
        CHECK(weights.at(part) == doctest::Approx(0.8));
    }
    CHECK(given.at(shape("MouthSmile")));
    CHECK(weights.at(shape("MouthSmile")) == doctest::Approx(0.6));
    // Shapes nothing implies stay out.
    CHECK_FALSE(given.at(shape("JawOpen")));
    CHECK_FALSE(given.at(shape("EyeClosed")));
    CHECK(weights.at(shape("EyeClosed")) == 0.0F);
}

TEST_CASE("face: XRFaceModifier3D drives its mesh's blend shapes from the face tracker") {
    FaceProject project("face_tracker_project");
    project.ok("scene.open", R"({"path": "main.scene.json"})");
    project.ok("xr.set", R"({"trackers": {"/user/face_tracker": {"type": "face",
        "blend_shapes": {"mouthSmileLeft": 0.8, "mouthSmileRight": 0.4, "jawOpen": 0.5}}}})");
    project.ok("step", R"({"ticks": 1})");
    CHECK(project.blend_shape("face", "MouthSmile") == doctest::Approx(0.6));
    CHECK(project.blend_shape("face", "jawOpen") == doctest::Approx(0.5));
    // A modifier drives another entity it names.
    CHECK(project.blend_shape("twin", "jawOpen") == doctest::Approx(0.5));

    json::Value state = project.ok("xr.state", R"({"trackers": ["/user/face_tracker"]})");
    const json::Value* face = state.find("trackers")->find("/user/face_tracker");
    REQUIRE(face != nullptr);
    CHECK(face->find("blend_shapes")->find("JawOpen")->as_double() == doctest::Approx(0.5));

    // Unknown shapes are refused with the closest name.
    const command::Reply wrong = project.call(
        "xr.set", R"({"trackers": {"/user/face_tracker": {"blend_shapes": {"JawOpne": 1}}}})");
    REQUIRE_FALSE(wrong.ok);
    CHECK(wrong.message.find("did you mean \"JawOpen\"") != std::string::npos);
}

TEST_CASE("face: animation.import_face_capture turns a recording into blend shape tracks") {
    FaceProject project("face_capture_project");
    // Live Link Face's columns: a timecode at 60 frames a second, a count, ARKit's shapes, then
    // the head's turn. The jaw opens and closes in a straight line; the smile holds.
    std::string csv = "Timecode,BlendShapeCount,JawOpen,MouthSmileLeft,MouthSmileRight,HeadYaw\n";
    for (int frame = 0; frame <= 30; ++frame) {
        const double jaw = frame <= 15 ? frame / 15.0 : (30 - frame) / 15.0;
        csv += "10:00:01:" + std::to_string(frame) + ".000,61," + std::to_string(jaw) +
               ",0.8,0.4,0.1\n";
    }
    std::ofstream(project.root / "talk.csv") << csv;

    json::Value plain = project.ok("animation.import_face_capture", R"({"path": "talk.csv"})");
    CHECK(plain.find("out")->as_string() == "talk.anim.json");
    CHECK(plain.find("animation")->as_string() == "talk");
    CHECK(plain.find("frames")->as_int() == 31);
    CHECK(plain.find("length")->as_double() == doctest::Approx(0.5));
    CHECK(plain.find("tracks")->as_int() == 3);
    CHECK(json::write(*plain.find("skipped"), json::Style::Compact) ==
          R"(["BlendShapeCount","HeadYaw"])");
    // Without a model, tracks are named as ARKit names its shapes; straight runs keep their ends.
    json::Value file = parse(read(project.root / "talk.anim.json"));
    const json::Value& tracks = *file.find("animations")->find("talk")->find("tracks");
    REQUIRE(tracks.as_array().size() == 3);
    CHECK(tracks.as_array()[0].find("path")->as_string() == "MeshInstance3D/blend_shapes/jawOpen");
    CHECK(json::write(*tracks.as_array()[0].find("keys"), json::Style::Compact) ==
          R"([{"time":0.0,"value":0.0},{"time":0.25,"value":1.0},{"time":0.5,"value":0.0}])");
    CHECK(tracks.as_array()[1].find("keys")->as_array().size() == 1);

    // With a model, each of its shapes the recording drives gets a track, into a library kept.
    std::ofstream(project.root / "face.anim.json")
        << R"({"animations": {"idle": {"length": 1.0, "tracks": []}}, "format": 1})";
    json::Value mapped = project.ok("animation.import_face_capture",
                                    R"({"path": "talk.csv", "model": "models/face.gltf",
                                        "out": "face.anim.json", "name": "smile"})");
    CHECK(mapped.find("tracks")->as_int() == 2);
    CHECK(mapped.find("undriven") == nullptr);
    file = parse(read(project.root / "face.anim.json"));
    CHECK(file.find("animations")->find("idle") != nullptr);
    const json::Value& smile = *file.find("animations")->find("smile")->find("tracks");
    CHECK(smile.as_array()[0].find("path")->as_string() ==
          "MeshInstance3D/blend_shapes/MouthSmile");
    CHECK(smile.as_array()[0].find("keys")->as_array()[0].find("value")->as_double() ==
          doctest::Approx(0.6));

    // Undo takes the file back, and redo writes it again.
    project.ok("undo", "{}");
    CHECK(parse(read(project.root / "face.anim.json")).find("animations")->find("smile") ==
          nullptr);
    project.ok("redo", "{}");

    // The animation plays on the face.
    project.ok("scene.open", R"({"path": "main.scene.json"})");
    project.ok("component.set", R"({"id": "face", "components": {"AnimationPlayer":
        {"library": "face.anim.json", "animation": "smile", "playing": true}}})");
    project.ok("step", R"({"ticks": 15})");
    CHECK(project.blend_shape("face", "jawOpen") > 0.9);
    CHECK(project.blend_shape("face", "MouthSmile") == doctest::Approx(0.6));

    // Problems name the line.
    std::ofstream(project.root / "bad.csv") << "Timecode,JawOpen\n10:00:00:00,0.1\n10:00:00:01,x\n";
    const command::Reply bad =
        project.call("animation.import_face_capture", R"({"path": "bad.csv"})");
    REQUIRE_FALSE(bad.ok);
    CHECK(bad.message == R"(/path: line 3's JawOpen "x" isn't a number)");
    std::ofstream(project.root / "untimed.csv") << "Frame,JawOpen\n0,0.1\n";
    const command::Reply untimed =
        project.call("animation.import_face_capture", R"({"path": "untimed.csv"})");
    REQUIRE_FALSE(untimed.ok);
    CHECK(untimed.message == "/path: line 1 has no Timecode, time or seconds column");
    const command::Reply missing = project.call("animation.import_face_capture",
                                                R"({"path": "talk.csv", "out": "talk.json"})");
    REQUIRE_FALSE(missing.ok);
    CHECK(missing.message.starts_with("/out: "));
}
