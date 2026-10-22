#include "gltf.hpp"

#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/platform/file.hpp"
#include "slash/scene3d/blend.hpp"
#include "slash/scene3d/model.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <SDL3/SDL_stdinc.h>
#include <doctest/doctest.h>

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace command = slash::command;
namespace json = slash::json;
namespace scene3d = slash::scene3d;
namespace world = slash::world;
namespace fs = std::filesystem;

namespace {

// Sets an environment variable, or unsets it for an empty value, and puts it back on scope exit.
class Variable {
public:
    Variable(std::string name, const std::string& value) : name_(std::move(name)) {
        if (const char* old = SDL_getenv_unsafe(name_.c_str())) {
            old_ = old;
        }
        set(value);
    }
    ~Variable() { set(old_.value_or("")); }
    Variable(const Variable&) = delete;
    Variable& operator=(const Variable&) = delete;
    Variable(Variable&&) = delete;
    Variable& operator=(Variable&&) = delete;

    void set(const std::string& value) const {
        if (value.empty()) {
            SDL_unsetenv_unsafe(name_.c_str());
        } else {
            SDL_setenv_unsafe(name_.c_str(), value.c_str(), 1);
        }
    }

private:
    std::string name_;
    std::optional<std::string> old_;
};

bool contains(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

template <typename T>
std::string error_of(const slash::Result<T>& result) {
    return result.ok() ? "(succeeded)" : result.error().message;
}

// A stand-in .blend file, which Blender's stand-in (tests/fake_blender.cpp) exports as the glTF
// it holds: a quad with one material.
std::string stand_in(std::string_view material) {
    slash::test::Gltf model = slash::test::quad(0);
    json::Value brick;
    brick["name"] = material;
    model.document["materials"].push_back(std::move(brick));
    return "BLENDER-fake\n" + model.gltf();
}

std::string read(const fs::path& path) {
    auto text = slash::platform::read_file(path.string());
    REQUIRE_MESSAGE(text.ok(), error_of(text));
    return std::move(text).value();
}

void write(const fs::path& path, std::string_view text) {
    REQUIRE(slash::platform::create_directories(path.parent_path().string()).ok());
    REQUIRE(slash::platform::write_file(path.string(), text).ok());
}

// The files in a folder, by name.
std::vector<std::string> files_in(const fs::path& folder) {
    auto files = slash::platform::list_files(folder.string());
    REQUIRE(files.ok());
    return files.value();
}

std::size_t lines_in(const fs::path& path) {
    std::size_t lines = 0;
    for (const char c : slash::platform::exists(path.string()) ? read(path) : "") {
        lines += c == '\n' ? 1 : 0;
    }
    return lines;
}

} // namespace

TEST_CASE("Blender is found as Godot finds it, and must be 3.0 or later") {
    Variable version("FAKE_BLENDER_VERSION", "");
    auto blender = scene3d::find_blender(SLASH_FAKE_BLENDER);
    REQUIRE_MESSAGE(blender.ok(), error_of(blender));
    CHECK(blender.value().program == SLASH_FAKE_BLENDER);
    CHECK(blender.value().major == 4);
    CHECK(blender.value().minor == 2);
    version.set("3.6.5");
    blender = scene3d::find_blender(SLASH_FAKE_BLENDER);
    REQUIRE_MESSAGE(blender.ok(), error_of(blender));
    CHECK(blender.value().major == 3);
    CHECK(blender.value().minor == 6);

    version.set("2.93.18 LTS");
    blender = scene3d::find_blender(SLASH_FAKE_BLENDER);
    REQUIRE_FALSE(blender.ok());
    CHECK(contains(blender.error().message, "it is Blender 2.93"));
    CHECK(contains(blender.error().message, "needs Blender 3.0 or later"));
    version.set("next");
    blender = scene3d::find_blender(SLASH_FAKE_BLENDER);
    REQUIRE_FALSE(blender.ok());
    CHECK(contains(blender.error().message, "isn't one Blender prints: \"Blender next\""));
    version.set("");
    blender = scene3d::find_blender("no/such/blender");
    REQUIRE_FALSE(blender.ok());
    CHECK(contains(blender.error().message, "Blender \"no/such/blender\" can't be used"));

    // With no program given, SLASH_BLENDER_PATH names it.
    Variable path("SLASH_BLENDER_PATH", SLASH_FAKE_BLENDER);
    blender = scene3d::find_blender();
    REQUIRE_MESSAGE(blender.ok(), error_of(blender));
    CHECK(blender.value().program == SLASH_FAKE_BLENDER);
    path.set("no/such/blender");
    blender = scene3d::find_blender();
    REQUIRE_FALSE(blender.ok());
    CHECK(contains(blender.error().message, "SLASH_BLENDER_PATH names \"no/such/blender\""));
}

TEST_CASE("the export script uses Godot's import options for the Blender it runs on") {
    const scene3d::Blender blender42{.program = "blender", .major = 4, .minor = 2};
    std::string script =
        scene3d::blend_export_script(blender42, "models/house.blend", "out/house.glb");
    CHECK(contains(script, "'path': 'models/house.blend'"));
    CHECK(contains(script, "'filepath': 'out/house.glb'"));
    CHECK(contains(script, "'export_format': 'GLB'"));
    CHECK(contains(script, "'export_yup': True"));
    CHECK(contains(script, "'export_apply': True"));
    CHECK(contains(script, "'export_vertex_color': 'NONE'"));
    CHECK(contains(script, "'export_animation_mode': 'ACTIONS'"));
    CHECK_FALSE(contains(script, "export_colors"));
    CHECK(contains(script, "bpy.ops.wm.open_mainfile(filepath=source)\n"));
    CHECK(contains(script, "bpy.ops.export_scene.gltf(**opts['gltf_options'])\n"));
    // Before 4.2, vertex colors had another option, and before 3.6 animations did.
    script = scene3d::blend_export_script({.program = "blender", .major = 3, .minor = 6}, "a.blend",
                                          "a.glb");
    CHECK(contains(script, "'export_colors': False"));
    CHECK(contains(script, "'export_animation_mode': 'ACTIONS'"));
    script = scene3d::blend_export_script({.program = "blender", .major = 3, .minor = 3}, "a.blend",
                                          "a.glb");
    CHECK(contains(script, "'export_colors': False"));
    CHECK(contains(script, "'export_nla_strips': True"));
    CHECK_FALSE(contains(script, "export_animation_mode"));
    // Paths are Python strings.
    script = scene3d::blend_export_script(blender42, "models/it's\\here.blend", "a.glb");
    CHECK(contains(script, R"('path': 'models/it\'s\\here.blend')"));
}

TEST_CASE(".blend files convert once with Blender, and again when they change") {
    const fs::path root = "blend_test_project";
    fs::remove_all(root);
    const fs::path runs = fs::absolute(root / "runs.log");
    write(root / "models/house.blend", stand_in("Brick"));
    Variable log("FAKE_BLENDER_LOG", runs.string());
    Variable fail("FAKE_BLENDER_FAIL", "");
    Variable version("FAKE_BLENDER_VERSION", "");
    scene3d::BlendOptions options{.project_root = root.string(),
                                  .run_blender = true,
                                  .why_not = {},
                                  .blender = SLASH_FAKE_BLENDER,
                                  .timeout_seconds = 60};
    const auto model_reader = [&](std::string_view path) {
        return slash::platform::read_file_in(root.string(), path);
    };

    std::string bytes = read(root / "models/house.blend");
    CHECK_FALSE(scene3d::is_converted_blend(bytes));
    auto refused = scene3d::load_model(bytes, "models/house.blend", model_reader);
    REQUIRE_FALSE(refused.ok());
    CHECK(contains(refused.error().message, "a Blender file that hasn't been converted to glTF"));

    auto converted = scene3d::convert_blend("models/house.blend", bytes, options);
    REQUIRE_MESSAGE(converted.ok(), error_of(converted));
    CHECK(scene3d::is_converted_blend(converted.value()));
    CHECK(lines_in(runs) == 1);
    auto model = scene3d::load_model(converted.value(), "models/house.blend", model_reader);
    REQUIRE_MESSAGE(model.ok(), error_of(model));
    REQUIRE(model.value().meshes.size() == 1);
    REQUIRE(model.value().materials.size() == 1);
    CHECK(model.value().materials[0].name == "Brick");
    CHECK(scene3d::check_model(converted.value(), "models/house.blend", model_reader).empty());

    // It is kept in .slash/imported/, named after the file and a hash of it, with Blender's log.
    const std::string kept = scene3d::blend_import_path("models/house.blend", bytes);
    CHECK(kept.starts_with(".slash/imported/models/house.blend-"));
    CHECK(kept.ends_with(".glb"));
    CHECK(read(root / kept) == converted.value());
    const std::string stem = kept.substr(0, kept.size() - 4);
    CHECK(contains(read(root / (stem + ".log")), "Finished glTF 2.0 export"));
    CHECK(files_in(root / ".slash/imported/models").size() == 2);

    // Until the file changes, the kept one is used, even where Blender can't run.
    converted = scene3d::convert_blend("models/house.blend", bytes, options);
    REQUIRE_MESSAGE(converted.ok(), error_of(converted));
    CHECK(lines_in(runs) == 1);
    scene3d::BlendOptions no_blender = options;
    no_blender.run_blender = false;
    no_blender.why_not = "games don't run Blender";
    CHECK(scene3d::convert_blend("models/house.blend", bytes, no_blender).ok());
    // Bytes that are already glTF, as an export ships, are used as they are.
    const std::string glb = converted.value();
    converted = scene3d::convert_blend("models/house.blend", glb, no_blender);
    REQUIRE_MESSAGE(converted.ok(), error_of(converted));
    CHECK(converted.value() == glb);

    // An edited file converts again, and what was made of it before is removed.
    write(root / "models/house.blend", stand_in("Stone"));
    bytes = read(root / "models/house.blend");
    converted = scene3d::convert_blend("models/house.blend", bytes, no_blender);
    REQUIRE_FALSE(converted.ok());
    CHECK(converted.error().message ==
          "it hasn't been converted from Blender yet, and games don't run Blender");
    converted = scene3d::convert_blend("models/house.blend", bytes, options);
    REQUIRE_MESSAGE(converted.ok(), error_of(converted));
    CHECK(lines_in(runs) == 2);
    model = scene3d::load_model(converted.value(), "models/house.blend", model_reader);
    REQUIRE_MESSAGE(model.ok(), error_of(model));
    CHECK(model.value().materials[0].name == "Stone");
    const std::string now = scene3d::blend_import_path("models/house.blend", bytes);
    CHECK(now != kept);
    const std::string now_name = fs::path(now).filename().string();
    const std::string now_stem = now_name.substr(0, now_name.size() - 4);
    CHECK(files_in(root / ".slash/imported/models") ==
          std::vector<std::string>{now_name, now_stem + ".log"});
    fs::remove_all(root);
}

TEST_CASE(".blend files Blender can't convert give its last lines and its log") {
    const fs::path root = "blend_fail_project";
    fs::remove_all(root);
    write(root / "models/broken.blend", "not a blend file");
    write(root / "models/house.blend", stand_in("Brick"));
    Variable log("FAKE_BLENDER_LOG", "");
    Variable fail("FAKE_BLENDER_FAIL", "");
    Variable version("FAKE_BLENDER_VERSION", "");
    scene3d::BlendOptions options{.project_root = root.string(),
                                  .run_blender = true,
                                  .why_not = {},
                                  .blender = SLASH_FAKE_BLENDER,
                                  .timeout_seconds = 60};
    auto converted =
        scene3d::convert_blend("models/broken.blend", read(root / "models/broken.blend"), options);
    REQUIRE_FALSE(converted.ok());
    const std::string& message = converted.error().message;
    CHECK(message.starts_with("Blender couldn't convert it (exit code 1): "));
    CHECK(contains(message, "RuntimeError: Error: Cannot read file, not a blend file"));
    CHECK(contains(message, "; its log is .slash/imported/models/broken.blend-"));
    // Only the log is left.
    const std::vector<std::string> left = files_in(root / ".slash/imported/models");
    REQUIRE(left.size() == 1);
    CHECK(left[0].ends_with(".log"));

    const std::string house = read(root / "models/house.blend");
    fail.set("nothing");
    converted = scene3d::convert_blend("models/house.blend", house, options);
    REQUIRE_FALSE(converted.ok());
    CHECK(converted.error().message.starts_with("Blender wrote no glTF file for it: "));
    fail.set("hang");
    options.timeout_seconds = 1;
    converted = scene3d::convert_blend("models/house.blend", house, options);
    REQUIRE_FALSE(converted.ok());
    CHECK(converted.error().message.starts_with(
        "Blender was stopped after 1 s, before it finished converting it; its log is "));
    fail.set("");
    version.set("2.80");
    converted = scene3d::convert_blend("models/house.blend", house, options);
    REQUIRE_FALSE(converted.ok());
    CHECK(contains(converted.error().message, "needs Blender 3.0 or later"));
    fs::remove_all(root);
}

TEST_CASE("sessions read .blend files as glTF, running Blender only with the build capability") {
    const fs::path root = "blend_session_project";
    fs::remove_all(root);
    write(root / "models/house.blend", stand_in("Brick"));
    Variable path("SLASH_BLENDER_PATH", SLASH_FAKE_BLENDER);
    Variable fail("FAKE_BLENDER_FAIL", "");
    Variable version("FAKE_BLENDER_VERSION", "");
    world::ComponentRegistry components;
    world::register_builtin_components(components);
    command::Registry commands;
    command::register_builtin_commands(commands);
    const auto inspect = [&](std::vector<command::Capability> capabilities) {
        command::SessionOptions options;
        options.project_root = root.string();
        options.capabilities = std::move(capabilities);
        command::Session session(components, commands, std::move(options));
        auto params = json::parse(R"({"path": "models/house.blend"})");
        REQUIRE(params.ok());
        return session.call("model.inspect", params.value());
    };

    command::Reply reply = inspect({command::Capability::Read});
    REQUIRE_FALSE(reply.ok);
    CHECK(contains(reply.message, "hasn't been converted from Blender yet, and this session runs "
                                  "Blender only with the build capability (--allow build)"));
    reply = inspect({command::Capability::Read, command::Capability::Build});
    REQUIRE_MESSAGE(reply.ok, reply.message);
    const json::Value* materials = reply.value.find("materials");
    REQUIRE(materials != nullptr);
    CHECK(materials->as_array()[0].find("name")->as_string() == "Brick");
    // Once converted, every session reads it.
    reply = inspect({command::Capability::Read});
    CHECK_MESSAGE(reply.ok, reply.message);
    fs::remove_all(root);
}
