#include "devices.hpp"

#include "slash/asset/assets.hpp"
#include "slash/canvas/canvas.hpp"
#include "slash/canvas/sprite_frames.hpp"
#include "slash/core/json.hpp"
#include "slash/core/log.hpp"
#include "slash/platform/process.hpp"
#include "slash/render/device.hpp"
#include "slash/render/image.hpp"
#include "slash/ui/ui.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace asset = slash::asset;
namespace canvas = slash::canvas;
namespace json = slash::json;
namespace render = slash::render;
namespace world = slash::world;
using slash::test::drawing_devices;

namespace {

// Files in memory; editing one gives it a new stamp, so poll() reloads it.
struct MemoryFiles {
    std::map<std::string, std::string, std::less<>> files;
    std::map<std::string, std::int64_t, std::less<>> edits;

    void write(const std::string& path, std::string text) {
        files[path] = std::move(text);
        ++edits[path];
    }

    asset::FileSource source() {
        return {
            .read = [this](std::string_view path) -> slash::Result<std::string> {
                const auto it = files.find(path);
                if (it == files.end()) {
                    return slash::make_error(slash::ErrorCode::NotFound, "no such file");
                }
                return it->second;
            },
            .stamp = [this](std::string_view path) -> std::optional<asset::FileStamp> {
                const auto it = files.find(path);
                if (it == files.end()) {
                    return std::nullopt;
                }
                return asset::FileStamp{.modified = edits.find(path)->second,
                                        .size = it->second.size()};
            },
        };
    }
};

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE(parsed.ok());
    return std::move(parsed).value();
}

using Rgba = std::array<std::uint8_t, 4>;

// A PNG of one color per frame, frames side by side, each size x size.
std::string strip_png(const std::vector<Rgba>& frames, std::uint32_t size) {
    render::Image image;
    image.width = size * static_cast<std::uint32_t>(frames.size());
    image.height = size;
    for (std::uint32_t y = 0; y < size; ++y) {
        for (const Rgba& color : frames) {
            for (std::uint32_t x = 0; x < size; ++x) {
                image.pixels.insert(image.pixels.end(), color.begin(), color.end());
            }
        }
    }
    auto png = render::encode_png(image);
    REQUIRE(png.ok());
    return png.value();
}

// A world with the built-in components, the canvas asset types and its system.
struct Scene {
    MemoryFiles files;
    asset::Assets assets{files.source()};
    world::ComponentRegistry components;
    world::World world{components};

    Scene() {
        world::register_builtin_components(components);
        canvas::install(world, assets);
    }

    entt::entity create(std::string_view id, std::string_view components_json) {
        const entt::entity entity = world.create(id).value();
        const json::Value values = parse(components_json);
        for (const auto& [name, fields] : values.as_object()) {
            std::string path;
            REQUIRE(components.find(name)->assign(world, entity, fields, path).ok());
        }
        return entity;
    }

    const world::Sprite2D& sprite(entt::entity entity) {
        return world.ecs().get<world::Sprite2D>(entity);
    }
    const world::AnimatedSprite2D& player(entt::entity entity) {
        return world.ecs().get<world::AnimatedSprite2D>(entity);
    }
};

std::string first_error(const json::Value& doc) {
    const auto errors = canvas::validate_sprite_frames(doc);
    return errors.empty() ? "(valid)" : errors.front().path + ": " + errors.front().message;
}

} // namespace

TEST_CASE("images round-trip through PNG, and other bytes are refused") {
    const std::string png = strip_png({{255, 0, 0, 255}, {0, 0, 255, 128}}, 2);
    auto image = render::decode_image(png);
    REQUIRE(image.ok());
    CHECK(image.value().width == 4);
    CHECK(image.value().height == 2);
    CHECK(Rgba{image.value().pixels[0], image.value().pixels[1], image.value().pixels[2],
               image.value().pixels[3]} == Rgba{255, 0, 0, 255});
    CHECK(Rgba{image.value().pixels[12], image.value().pixels[13], image.value().pixels[14],
               image.value().pixels[15]} == Rgba{0, 0, 255, 128});

    auto bad = render::decode_image("GIF89a not really");
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.error().message.find("not a PNG or BMP image") != std::string::npos);
    CHECK_FALSE(render::encode_png(render::Image{}).ok());
}

TEST_CASE("sprite frames files are checked, and frame numbers need a sheet") {
    CHECK(first_error(parse(
              R"({"format": 1, "animations": {"default": {"frames": ["a.png"]}}})")) == "(valid)");
    CHECK(first_error(parse(R"({"format": 1, "animations": {}})")) ==
          "/animations: expected at least 1 properties, got 0");
    CHECK(first_error(parse(R"({"format": 1, "animations": {"walk": {"frames": [0]}}})")) ==
          "/animations/walk/frames/0: a frame number needs the animation's \"texture\"");
    CHECK(first_error(parse(R"({"format": 1, "animations": {"walk": {"texture": "hero.png",
          "hframes": 2, "frames": [0, 2]}}})")) ==
          "/animations/walk/frames/1: the sheet has 2 frames, numbered from 0");
    CHECK(first_error(parse(R"({"format": 1, "animations": {"walk": {}}})")) ==
          "/animations/walk: needs \"frames\", or a \"texture\" sheet to play");
    CHECK(first_error(parse(R"({"format": 1, "animations": {"walk": {"frames": ["../up.png"]}}})"))
              .starts_with("/animations/walk/frames/0: expected a path inside the project"));
    CHECK(first_error(parse(R"({"format": 1, "animations": {"walk": {"frame": [0]}}})"))
              .find("did you mean \"frames\"") != std::string::npos);

    auto frames = canvas::parse_sprite_frames(parse(R"({"format": 1, "animations": {
        "walk": {"texture": "hero.png", "hframes": 2, "vframes": 2, "fps": 8},
        "idle": {"texture": "hero.png", "hframes": 2, "vframes": 2, "frames": [3, "idle.png"],
                 "loop": false}}})"));
    REQUIRE(frames.ok());
    const canvas::Animation& walk = frames.value().animations.at("walk");
    CHECK(walk.frames.size() == 4);
    CHECK(walk.frames[3].frame == 3);
    CHECK(walk.frames[3].hframes == 2);
    CHECK(walk.fps == doctest::Approx(8.0));
    CHECK(walk.loop);
    const canvas::Animation& idle = frames.value().animations.at("idle");
    REQUIRE(idle.frames.size() == 2);
    CHECK(idle.frames[0].texture == "hero.png");
    CHECK(idle.frames[0].frame == 3);
    CHECK(idle.frames[1].texture == "idle.png");
    CHECK(idle.frames[1].hframes == 1);
    CHECK(idle.fps == doctest::Approx(5.0)); // Godot's default
    CHECK_FALSE(idle.loop);
}

TEST_CASE("AnimatedSprite2D plays its animation on the entity's Sprite2D") {
    Scene scene;
    scene.files.write("hero.frames.json", R"({"format": 1, "animations": {
        "walk": {"texture": "hero.png", "hframes": 4, "fps": 10},
        "jump": {"frames": ["jump_0.png", "jump_1.png"], "fps": 10, "loop": false}}})");
    std::vector<world::Event> events;
    scene.world.set_event_listener([&](const world::Event& event) { events.push_back(event); });
    const entt::entity hero = scene.create(
        "hero",
        R"({"AnimatedSprite2D": {"sprite_frames": "hero.frames.json", "animation": "walk"}})");

    // Ticks of 1/20 s at 10 frames per second: a new frame every other tick.
    scene.world.step(0.05);
    REQUIRE(scene.world.ecs().all_of<world::Sprite2D>(hero)); // added for it
    CHECK(scene.sprite(hero).texture == "hero.png");
    CHECK(scene.sprite(hero).hframes == 4);
    CHECK(scene.sprite(hero).frame == 0);
    CHECK(scene.player(hero).frame_progress == doctest::Approx(0.5));
    scene.world.step(0.05);
    CHECK(scene.sprite(hero).frame == 1);
    for (int i = 0; i < 6; ++i) {
        scene.world.step(0.05);
    }
    CHECK(scene.player(hero).frame == 0); // looped
    CHECK(scene.sprite(hero).frame == 0);

    // Backwards, wrapping to the last frame.
    scene.world.ecs().get<world::AnimatedSprite2D>(hero).speed_scale = -1.0F;
    scene.world.ecs().get<world::AnimatedSprite2D>(hero).frame_progress = 0.0F;
    scene.world.step(0.05);
    CHECK(scene.player(hero).frame == 3);

    // An animation that doesn't loop stops on its last frame and says so.
    auto& player = scene.world.ecs().get<world::AnimatedSprite2D>(hero);
    player = {.sprite_frames = "hero.frames.json",
              .animation = "jump",
              .playing = true,
              .speed_scale = 1.0F,
              .frame = 0,
              .frame_progress = 0.0F};
    for (int i = 0; i < 10; ++i) {
        scene.world.step(0.05);
    }
    CHECK_FALSE(scene.player(hero).playing);
    CHECK(scene.player(hero).frame == 1);
    CHECK(scene.player(hero).frame_progress == doctest::Approx(1.0));
    CHECK(scene.sprite(hero).texture == "jump_1.png");
    CHECK(scene.sprite(hero).hframes == 1);
    REQUIRE(events.size() == 2); // entity.created, then the finish
    CHECK(events.back().kind == "animation_finished");
    CHECK(events.back().id == "hero");
    CHECK(events.back().data == parse(R"({"animation": "jump"})"));
}

TEST_CASE("an AnimatedSprite2D that can't play leaves its sprite alone") {
    Scene scene;
    scene.files.write("hero.frames.json",
                      R"({"format": 1, "animations": {"walk": {"frames": ["a.png"]}}})");
    const entt::entity typo = scene.create(
        "typo", R"({"AnimatedSprite2D": {"sprite_frames": "hero.frames.json", "animation": "wlak"},
                   "Sprite2D": {"texture": "still.png"}})");
    const entt::entity missing =
        scene.create("missing", R"({"AnimatedSprite2D": {"sprite_frames": "gone.frames.json"}})");
    scene.world.step(0.05);
    CHECK(scene.sprite(typo).texture == "still.png");
    CHECK(scene.sprite(missing).texture.empty());
    CHECK(scene.player(typo).frame == 0);
    // Once the file is fixed, it plays.
    scene.files.write("gone.frames.json",
                      R"({"format": 1, "animations": {"default": {"frames": ["b.png"]}}})");
    scene.world.step(0.05);
    CHECK(scene.sprite(missing).texture == "b.png");
}

TEST_CASE("sprite_quad places a sprite's frame like Godot's Sprite2D") {
    world::Sprite2D sprite;
    world::Transform2D at;
    at.position = {100.0F, 50.0F};
    auto quad = canvas::sprite_quad(sprite, at, 32, 16);
    REQUIRE(quad.ok());
    // Centered: top left, top right, bottom left, ..., bottom right.
    CHECK(quad.value()[0].x == doctest::Approx(84.0F));
    CHECK(quad.value()[0].y == doctest::Approx(42.0F));
    CHECK(quad.value()[4].x == doctest::Approx(116.0F));
    CHECK(quad.value()[4].y == doctest::Approx(58.0F));
    CHECK(quad.value()[0].u == doctest::Approx(0.0F));
    CHECK(quad.value()[4].u == doctest::Approx(1.0F));

    // The third frame of a 2x2 sheet, uncentered, offset, flipped and tinted.
    sprite.hframes = 2;
    sprite.vframes = 2;
    sprite.frame = 2;
    sprite.centered = false;
    sprite.offset = {1.0F, 2.0F};
    sprite.flip_h = true;
    sprite.modulate = {1.0F, 0.5F, 0.0F, 1.0F};
    quad = canvas::sprite_quad(sprite, at, 32, 16);
    REQUIRE(quad.ok());
    CHECK(quad.value()[0].x == doctest::Approx(101.0F));
    CHECK(quad.value()[0].y == doctest::Approx(52.0F));
    CHECK(quad.value()[4].x == doctest::Approx(117.0F));
    CHECK(quad.value()[4].y == doctest::Approx(60.0F));
    CHECK(quad.value()[0].u == doctest::Approx(0.5F)); // flipped: the right edge on the left
    CHECK(quad.value()[1].u == doctest::Approx(0.0F));
    CHECK(quad.value()[0].v == doctest::Approx(0.5F));
    CHECK(quad.value()[4].v == doctest::Approx(1.0F));
    CHECK(quad.value()[0].rgba == Rgba{255, 128, 0, 255});

    // A region, then frames inside it.
    sprite = {};
    sprite.region = {8.0F, 0.0F, 16.0F, 8.0F};
    sprite.hframes = 2;
    sprite.frame = 1;
    sprite.centered = false;
    quad = canvas::sprite_quad(sprite, {}, 32, 16);
    REQUIRE(quad.ok());
    CHECK(quad.value()[0].u == doctest::Approx(0.5F));
    CHECK(quad.value()[4].u == doctest::Approx(0.75F));
    CHECK(quad.value()[4].v == doctest::Approx(0.5F));
    CHECK(quad.value()[4].x == doctest::Approx(8.0F));

    // Scaled by 2 and turned a quarter clockwise: right becomes down.
    sprite = {};
    sprite.centered = false;
    world::Transform2D turned;
    turned.rotation = std::numbers::pi_v<float> / 2.0F;
    turned.scale = {2.0F, 2.0F};
    quad = canvas::sprite_quad(sprite, turned, 4, 2);
    REQUIRE(quad.ok());
    CHECK(quad.value()[1].x == doctest::Approx(0.0F).epsilon(1e-5));
    CHECK(quad.value()[1].y == doctest::Approx(8.0F));
    CHECK(quad.value()[2].x == doctest::Approx(-4.0F));

    sprite.frame = 4;
    sprite.hframes = 2;
    sprite.vframes = 2;
    CHECK(canvas::sprite_quad(sprite, {}, 4, 4).error().message ==
          "frame 4 is outside the sheet's 4 frames, numbered from 0");
    sprite = {};
    sprite.region = {0.0F, 0.0F, 8.0F, 8.0F};
    CHECK(canvas::sprite_quad(sprite, {}, 4, 4).error().message ==
          "region [0, 0, 8, 8] goes outside the 4x4 texture");
}

TEST_CASE("interpolate blends transforms and turns the short way") {
    world::Transform2D from;
    from.position = {0.0F, 10.0F};
    from.rotation = 3.0F;
    world::Transform2D to;
    to.position = {10.0F, 20.0F};
    to.rotation = -3.0F; // 0.28 radians on from 3.0, past pi
    to.scale = {3.0F, 1.0F};
    const world::Transform2D half = canvas::interpolate(from, to, 0.5);
    CHECK(half.position.x == doctest::Approx(5.0F));
    CHECK(half.position.y == doctest::Approx(15.0F));
    CHECK(half.scale.x == doctest::Approx(2.0F));
    CHECK(half.rotation ==
          doctest::Approx(3.0F + ((2.0F * std::numbers::pi_v<float> - 6.0F) / 2.0F)));
    CHECK(canvas::interpolate(from, to, 1.0).rotation == doctest::Approx(-3.0F));
    CHECK(canvas::interpolate(from, to, 0.0).position.x == doctest::Approx(0.0F));
}

TEST_CASE("the canvas draws visible sprites, skips broken ones and reloads textures") {
    Scene scene;
    scene.files.write("red.png", strip_png({{255, 0, 0, 255}}, 4));
    const auto device = render::create_null_device();
    canvas::CanvasRenderer renderer(*device, scene.assets);
    scene.create("a", R"({"Transform2D": {}, "Sprite2D": {"texture": "red.png"}})");
    scene.create("b", R"({"Transform2D": {}, "Sprite2D": {"texture": "red.png", "z_index": 2,
                          "texture_filter": "nearest"}})");
    scene.create("hidden", R"({"Transform2D": {}, "Sprite2D": {"texture": "red.png",
                               "visible": false}})");
    scene.create("missing", R"({"Transform2D": {}, "Sprite2D": {"texture": "gone.png"}})");
    scene.create("bad_frame", R"({"Transform2D": {}, "Sprite2D": {"texture": "red.png",
                                  "frame": 1}})");
    scene.create("no_transform", R"({"Sprite2D": {"texture": "red.png"}})");

    // The null device has no window, so a window frame only checks and counts the sprites.
    REQUIRE(renderer.draw(scene.world, {}).ok());
    CHECK(renderer.sprites_drawn() == 2);
    CHECK(device->stats().frames == 0);

    auto target = device->create_texture(
        {.width = 8, .height = 8, .filter = render::Filter::Linear, .render_target = true});
    REQUIRE(target.ok());
    canvas::CanvasOptions options;
    options.target = target.value();
    REQUIRE(renderer.draw(scene.world, options).ok());
    CHECK(renderer.sprites_drawn() == 2);
    // The target, then one texture per filter: two textures, so two GPU draws.
    CHECK(device->stats().textures == 3);
    CHECK(device->stats().draws == 2);
    CHECK(device->stats().triangles == 4);

    // A changed file updates the texture in place.
    scene.files.write("red.png", strip_png({{0, 255, 0, 255}}, 4));
    REQUIRE(scene.assets.poll().size() == 1);
    REQUIRE(renderer.draw(scene.world, options).ok());
    CHECK(device->stats().textures == 3);
    CHECK(renderer.sprites_drawn() == 2);
}

namespace {

// The texture times the vertex color, mixed toward a flash color by "flash".
constexpr std::string_view kFlashMaterial = R"({
  "color": {"op": "vec4", "args": [
    {"op": "mix", "args": ["base.rgb", "flash_color.rgb", "flash"]}, "base.a"]},
  "format": 1,
  "nodes": {
    "base": {"op": "mul", "args": [{"op": "texture", "args": ["texture", "uv"]}, "color"]}
  },
  "params": {
    "flash": {"type": "float", "value": 0, "min": 0, "max": 1},
    "flash_color": {"type": "color", "value": [1, 1, 1, 1]}
  }
})";

// Warnings logged while it lives.
struct Warnings {
    std::vector<std::string> lines;
    Warnings() {
        slash::log::set_sink([this](slash::log::Level level, std::string_view line) {
            if (level == slash::log::Level::Warn) {
                lines.emplace_back(line);
            }
        });
    }
    ~Warnings() { slash::log::reset_sink(); }
    [[nodiscard]] bool logged(std::string_view text) const {
        return std::ranges::any_of(lines, [text](const std::string& line) {
            return line.find(text) != std::string::npos;
        });
    }
    Warnings(const Warnings&) = delete;
    Warnings& operator=(const Warnings&) = delete;
    Warnings(Warnings&&) = delete;
    Warnings& operator=(Warnings&&) = delete;
};

} // namespace

TEST_CASE("the canvas draws each visible CPUParticles2D's live particles") {
    Scene scene;
    scene.files.write("red.png", strip_png({{255, 0, 0, 255}}, 4));
    const auto device = render::create_null_device();
    canvas::CanvasRenderer renderer(*device, scene.assets);
    scene.create("dots", R"({"Transform2D": {}, "CPUParticles2D": {"amount": 4,
                             "explosiveness": 1}})");
    scene.create("sparks", R"({"Transform2D": {}, "CPUParticles2D": {"amount": 3,
                               "explosiveness": 1, "texture": "red.png", "z_index": 1}})");
    scene.create("hidden", R"({"Transform2D": {}, "CPUParticles2D": {"amount": 5,
                               "explosiveness": 1, "visible": false}})");
    scene.create("missing", R"({"Transform2D": {}, "CPUParticles2D": {"amount": 6,
                                "explosiveness": 1, "texture": "gone.png"}})");
    // Nothing has been emitted before the first tick.
    REQUIRE(renderer.draw(scene.world, {}).ok());
    CHECK(renderer.particles_drawn() == 0);
    scene.world.step(1.0 / 60.0);
    REQUIRE(renderer.draw(scene.world, {}).ok());
    CHECK(renderer.particles_drawn() == 7);
    CHECK(renderer.sprites_drawn() == 0);

    // One draw per emitter, two triangles per particle.
    auto target = device->create_texture(
        {.width = 8, .height = 8, .filter = render::Filter::Linear, .render_target = true});
    REQUIRE(target.ok());
    canvas::CanvasOptions options;
    options.target = target.value();
    REQUIRE(renderer.draw(scene.world, options).ok());
    CHECK(renderer.particles_drawn() == 7);
    CHECK(device->stats().draws == 2);
    CHECK(device->stats().triangles == 14);
}

TEST_CASE("sprites draw with their materials, and a broken material leaves its sprite out") {
    Scene scene;
    scene.files.write("red.png", strip_png({{255, 0, 0, 255}}, 4));
    scene.files.write("noise.png", strip_png({{9, 9, 9, 255}}, 2));
    scene.files.write("flash.material.json", std::string(kFlashMaterial));
    scene.files.write("noise.material.json", R"({
      "color": {"op": "texture", "args": ["noise", "uv"]},
      "format": 1,
      "params": {"noise": {"type": "texture", "value": "noise.png", "repeat": true}}
    })");
    scene.files.write("bad.material.json", R"({"format": 1, "color": "colr"})");
    scene.create("plain", R"({"Transform2D": {}, "Sprite2D": {"texture": "red.png"}})");
    scene.create("flashing", R"({"Transform2D": {}, "Sprite2D": {"texture": "red.png",
        "material": "flash.material.json", "material_params": {"flash": 1}}})");
    scene.create("flashing_too", R"({"Transform2D": {}, "Sprite2D": {"texture": "red.png",
        "material": "flash.material.json", "material_params": {"flash": 1}}})");
    scene.create("noisy", R"({"Transform2D": {}, "Sprite2D": {"texture": "red.png",
        "material": "noise.material.json"}})");
    scene.create("typo", R"({"Transform2D": {}, "Sprite2D": {"texture": "red.png",
        "material": "flash.material.json", "material_params": {"flsh": 1}}})");
    scene.create("broken", R"({"Transform2D": {}, "Sprite2D": {"texture": "red.png",
        "material": "bad.material.json"}})");
    scene.create("missing", R"({"Transform2D": {}, "Sprite2D": {"texture": "red.png",
        "material": "gone.material.json"}})");

    const auto device = render::create_null_device();
    canvas::CanvasRenderer renderer(*device, scene.assets);
    auto target = device->create_texture(
        {.width = 8, .height = 8, .filter = render::Filter::Linear, .render_target = true});
    REQUIRE(target.ok());
    canvas::CanvasOptions options;
    options.target = target.value();
    Warnings warnings;
    REQUIRE(renderer.draw(scene.world, options).ok());
    CHECK(renderer.sprites_drawn() == 4);
    // Plain, then the two flashing sprites in one batch, then the noisy one: three draws. The
    // noise texture is made alongside the target and red.png.
    CHECK(device->stats().draws == 3);
    CHECK(device->stats().textures == 3);
    REQUIRE(warnings.lines.size() == 3);
    CHECK(warnings.logged(R"("material":"flash.material.json","error":"material_params/flsh: the )"
                          R"(material has no param \"flsh\"; did you mean \"flash\"?")"));
    CHECK(warnings.logged(R"("material":"bad.material.json","error":"asset \"bad.material.json)"
                          R"(\" did not load: /color: no node, param or input \"colr\"; did )"
                          R"(you mean \"color\"?")"));
    CHECK(warnings.logged(R"("material":"gone.material.json")"));

    // Each problem is told once, and a fixed file draws on the next frame.
    REQUIRE(renderer.draw(scene.world, options).ok());
    CHECK(warnings.lines.size() == 3);
    scene.files.write("bad.material.json", R"({"format": 1, "color": "color"})");
    (void)scene.assets.poll();
    REQUIRE(renderer.draw(scene.world, options).ok());
    CHECK(renderer.sprites_drawn() == 5);
}

TEST_CASE("Sprite2D texture_filter takes only known names") {
    world::ComponentRegistry components;
    world::register_builtin_components(components);
    world::World w(components);
    const entt::entity entity = w.create("s").value();
    std::string path;
    CHECK(components.find("Sprite2D")->assign(w, entity, parse(R"({"hframes": 2})"), path).ok());
    world::SchemaBuilder builder;
    (void)components.find("Sprite2D")->schema(builder);
    const json::Value& definition = builder.definition("Sprite2D");
    CHECK(*definition.find("properties")->find("texture_filter")->find("enum") ==
          parse(R"(["inherit", "linear", "nearest"])"));
}

namespace {

Rgba pixel(const render::Image& image, std::uint32_t x, std::uint32_t y) {
    const std::size_t at = ((std::size_t{y} * image.width) + x) * 4;
    return {image.pixels[at], image.pixels[at + 1], image.pixels[at + 2], image.pixels[at + 3]};
}

} // namespace

TEST_CASE("sprites draw where Godot would put them, in z order, on the software device and a GPU") {
    for (const auto& device : drawing_devices()) {
        CAPTURE(slash::render::backend_name(device->backend()));
        Scene scene;
        // Two 8x8 frames: red, then blue.
        scene.files.write("sheet.png", strip_png({{255, 0, 0, 255}, {0, 0, 255, 255}}, 8));
        scene.create("red", R"({"Transform2D": {"position": [16, 16]},
                            "Sprite2D": {"texture": "sheet.png", "hframes": 2,
                                         "texture_filter": "nearest", "z_index": 1}})");
        // Blue, drawn under red where they overlap, mirrored and twice as big.
        scene.create("blue", R"({"Transform2D": {"position": [24, 24], "scale": [2, 2]},
                             "Sprite2D": {"texture": "sheet.png", "hframes": 2, "frame": 1,
                                          "texture_filter": "nearest", "flip_h": true}})");
        // Half-transparent white over the bottom right corner.
        scene.create("tint", R"({"Transform2D": {"position": [40, 40]},
                             "Sprite2D": {"texture": "sheet.png", "hframes": 2, "centered": false,
                                          "texture_filter": "nearest",
                                          "modulate": [1, 1, 1, 0.5], "z_index": 5}})");
        auto target = device->create_texture(
            {.width = 48, .height = 48, .filter = render::Filter::Nearest, .render_target = true});
        REQUIRE(target.ok());
        canvas::CanvasRenderer renderer(*device, scene.assets);
        canvas::CanvasOptions options;
        options.target = target.value();
        options.clear = {.r = 0.0F, .g = 0.0F, .b = 0.0F, .a = 1.0F};
        REQUIRE(renderer.draw(scene.world, options).ok());
        CHECK(renderer.sprites_drawn() == 3);
        auto image = device->read_texture(target.value());
        REQUIRE(image.ok());
        CHECK(pixel(image.value(), 16, 16) == Rgba{255, 0, 0, 255}); // red on top of blue
        CHECK(pixel(image.value(), 12, 12) == Rgba{255, 0, 0, 255});
        CHECK(pixel(image.value(), 30, 30) == Rgba{0, 0, 255, 255}); // blue spans 16..32
        CHECK(pixel(image.value(), 10, 10) == Rgba{0, 0, 0, 255});   // outside both
        CHECK(pixel(image.value(), 34, 34) == Rgba{0, 0, 0, 255});
        // Red at half alpha over black, from 40 to 48.
        const Rgba tinted = pixel(image.value(), 44, 44);
        CHECK(tinted[0] >= 126);
        CHECK(tinted[0] <= 129);
        CHECK(tinted[2] == 0);

        // Halfway between ticks, a sprite is halfway between where it was and where it is.
        scene.create("mover", R"({"Transform2D": {"position": [4, 44]},
                              "Sprite2D": {"texture": "sheet.png", "hframes": 2, "frame": 1,
                                           "texture_filter": "nearest", "z_index": 9}})");
        renderer.before_tick(scene.world);
        scene.world.ecs().get<world::Transform2D>(scene.world.find("mover")).position.x = 20.0F;
        options.interpolation = 0.5;
        REQUIRE(renderer.draw(scene.world, options).ok());
        image = device->read_texture(target.value());
        REQUIRE(image.ok());
        CHECK(pixel(image.value(), 15, 44) == Rgba{0, 0, 255, 255}); // spans 8..16 at x = 12
        CHECK(pixel(image.value(), 17, 44) == Rgba{0, 0, 0, 255});
        CHECK(pixel(image.value(), 7, 44) == Rgba{0, 0, 0, 255});
    }
}

TEST_CASE("capture draws the viewport on the software device, at any scale") {
    Scene scene;
    slash::ui::Settings ui;
    ui.viewport = {64.0F, 32.0F};
    slash::ui::set_settings(scene.world, ui);
    canvas::Settings settings;
    settings.clear_color = {.r = 0.0F, .g = 0.0F, .b = 1.0F, .a = 1.0F};
    settings.texture_filter = render::Filter::Nearest;
    canvas::set_settings(scene.world, settings);
    scene.files.write("red.png", strip_png({{255, 0, 0, 255}}, 8));
    scene.create("red", R"({"Transform2D": {"position": [16, 16]},
                        "Sprite2D": {"texture": "red.png"}})");

    auto full = canvas::capture(scene.world, scene.assets);
    REQUIRE(full.ok());
    CHECK(full.value().image.width == 64);
    CHECK(full.value().image.height == 32);
    CHECK(full.value().sprites == 1);
    CHECK(pixel(full.value().image, 15, 15) == Rgba{255, 0, 0, 255});
    CHECK(pixel(full.value().image, 40, 15) == Rgba{0, 0, 255, 255});

    // Half size: the sprite spans 6..10, and the project's nearest filter keeps it sharp.
    auto half = canvas::capture(scene.world, scene.assets, {.scale = 0.5F});
    REQUIRE(half.ok());
    CHECK(half.value().image.width == 32);
    CHECK(half.value().image.height == 16);
    CHECK(pixel(half.value().image, 6, 6) == Rgba{255, 0, 0, 255});
    CHECK(pixel(half.value().image, 10, 6) == Rgba{0, 0, 255, 255});

    const auto huge = canvas::capture(scene.world, scene.assets, {.scale = 1000.0F});
    REQUIRE_FALSE(huge.ok());
    CHECK(huge.error().message ==
          "a capture is 1 to 16384 pixels wide and high; the viewport is 64x32");
}

TEST_CASE("particles draw their frame, color and scale on the software device and a GPU") {
    for (const auto& device : drawing_devices()) {
        CAPTURE(slash::render::backend_name(device->backend()));
        Scene scene;
        scene.files.write("sheet.png", strip_png({{255, 0, 0, 255}, {0, 0, 255, 255}}, 8));
        // A white pixel scaled to 8x8 and colored green.
        scene.create("green", R"({"Transform2D": {"position": [12, 12]},
                              "CPUParticles2D": {"amount": 1, "gravity": [0, 0],
                                                 "scale_amount_min": 8, "scale_amount_max": 8,
                                                 "color": [0, 1, 0, 1]}})");
        // The second frame of the sheet, picked by anim_offset.
        scene.create("blue", R"({"Transform2D": {"position": [36, 36]},
                             "CPUParticles2D": {"amount": 1, "gravity": [0, 0],
                                                "texture": "sheet.png", "hframes": 2,
                                                "texture_filter": "nearest",
                                                "anim_offset_min": 0.5,
                                                "anim_offset_max": 0.5}})");
        scene.world.step(1.0 / 60.0);
        auto target = device->create_texture(
            {.width = 48, .height = 48, .filter = render::Filter::Nearest, .render_target = true});
        REQUIRE(target.ok());
        canvas::CanvasRenderer renderer(*device, scene.assets);
        canvas::CanvasOptions options;
        options.target = target.value();
        options.clear = {.r = 0.0F, .g = 0.0F, .b = 0.0F, .a = 1.0F};
        REQUIRE(renderer.draw(scene.world, options).ok());
        CHECK(renderer.particles_drawn() == 2);
        auto image = device->read_texture(target.value());
        REQUIRE(image.ok());
        CHECK(pixel(image.value(), 9, 9) == Rgba{0, 255, 0, 255}); // spans 8..16
        CHECK(pixel(image.value(), 15, 15) == Rgba{0, 255, 0, 255});
        CHECK(pixel(image.value(), 17, 12) == Rgba{0, 0, 0, 255});
        CHECK(pixel(image.value(), 33, 33) == Rgba{0, 0, 255, 255}); // spans 32..40
        CHECK(pixel(image.value(), 41, 36) == Rgba{0, 0, 0, 255});
    }
}

TEST_CASE("materials color a sprite's pixels on the software device and a GPU, and reload when "
          "their files change") {
    for (const auto& device : drawing_devices()) {
        CAPTURE(slash::render::backend_name(device->backend()));
        Scene scene;
        scene.files.write("red.png", strip_png({{255, 0, 0, 255}}, 8));
        scene.files.write("stripes.png", strip_png({{0, 255, 0, 255}, {0, 0, 255, 255}}, 1));
        scene.files.write("flash.material.json", std::string(kFlashMaterial));
        // The stripes, four times across the sprite.
        scene.files.write("tiles.material.json", R"({
      "color": {"op": "texture", "args": ["stripes", {"op": "mul", "args": ["uv", 4]}]},
      "format": 1,
      "params": {"stripes": {"type": "texture", "value": "stripes.png", "repeat": true,
                             "filter": "nearest"}}
    })");
        // Gray, as bright as the fraction of a second.
        scene.files.write("clock.material.json", R"({
      "color": {"op": "vec4", "args": [{"op": "vec3", "args": [{"op": "fract", "args": ["time"]}]},
                                       1]},
      "format": 1
    })");
        scene.create("flash", R"({"Transform2D": {"position": [8, 8]},
                              "Sprite2D": {"texture": "red.png", "texture_filter": "nearest",
                                           "material": "flash.material.json",
                                           "material_params": {"flash": 0.5}}})");
        scene.create("tiles", R"({"Transform2D": {"position": [24, 8]},
                              "Sprite2D": {"texture": "red.png", "texture_filter": "nearest",
                                           "material": "tiles.material.json"}})");
        scene.create("clock", R"({"Transform2D": {"position": [8, 24]},
                              "Sprite2D": {"texture": "red.png", "texture_filter": "nearest",
                                           "material": "clock.material.json"}})");
        auto target = device->create_texture(
            {.width = 32, .height = 32, .filter = render::Filter::Nearest, .render_target = true});
        REQUIRE(target.ok());
        canvas::CanvasRenderer renderer(*device, scene.assets);
        canvas::CanvasOptions options;
        options.target = target.value();
        options.clear = {.r = 0.0F, .g = 0.0F, .b = 0.0F, .a = 1.0F};
        options.time = 7.25;
        REQUIRE(renderer.draw(scene.world, options).ok());
        CHECK(renderer.sprites_drawn() == 3);
        auto image = device->read_texture(target.value());
        REQUIRE(image.ok());
        const auto near = [](Rgba got, Rgba want) {
            for (std::size_t i = 0; i < 4; ++i) {
                if (got.at(i) + 1 < want.at(i) || got.at(i) > want.at(i) + 1) {
                    return false;
                }
            }
            return true;
        };
        CHECK(near(pixel(image.value(), 8, 8), {255, 128, 128, 255}));
        CHECK(pixel(image.value(), 20, 8) == Rgba{0, 255, 0, 255});
        CHECK(pixel(image.value(), 21, 8) == Rgba{0, 0, 255, 255});
        CHECK(pixel(image.value(), 26, 8) == Rgba{0, 255, 0, 255});
        CHECK(near(pixel(image.value(), 8, 24), {64, 64, 64, 255}));

        // A new flash color, from the file.
        std::string blue(kFlashMaterial);
        blue.replace(blue.find("[1, 1, 1, 1]"), 12, "[0, 0, 1, 1]");
        scene.files.write("flash.material.json", blue);
        REQUIRE(scene.assets.poll().size() == 1);
        REQUIRE(renderer.draw(scene.world, options).ok());
        image = device->read_texture(target.value());
        REQUIRE(image.ok());
        CHECK(near(pixel(image.value(), 8, 8), {128, 0, 128, 255}));
    }
}

// ---- Lights ----

TEST_CASE("cast_shadows keeps the nearest occluder at each angle of a point light") {
    render::Light light;
    light.position = {0.0F, 0.0F};
    light.shadow_far = 100.0F;
    // Two walls to the right: the nearer one, 30 away, hides the farther one where they overlap.
    const std::vector<canvas::OccluderEdge> edges{{.a = {50.0F, -10.0F}, .b = {50.0F, 10.0F}},
                                                  {.a = {30.0F, -2.0F}, .b = {30.0F, 2.0F}}};
    const auto row = canvas::cast_shadows(light, edges, 0.0F);
    REQUIRE(row.size() == render::kShadowMapWidth);
    // Columns go round from -pi, so pi is column 0 again.
    const auto depth_at = [&](float x, float y) {
        const auto column = static_cast<std::size_t>(render::shadow_coord(light, {x, y}).column);
        return render::decode_shadow_depth(row.at(column % render::kShadowMapWidth));
    };
    CHECK(depth_at(80.0F, 0.0F) == doctest::Approx(0.3).epsilon(0.001));
    // Past the near wall's ends, the far wall: 50 / cos(angle) away.
    CHECK(depth_at(80.0F, 8.0F) == doctest::Approx(std::hypot(50.0F, 5.0F) / 100.0F).epsilon(0.01));
    // Beyond both, and on the other side of the light, nothing.
    CHECK(depth_at(80.0F, 30.0F) == 2.0F);
    CHECK(depth_at(-80.0F, 0.0F) == 2.0F);
}

TEST_CASE("cast_shadows gives a directional light a row across its direction") {
    render::Light sun;
    sun.kind = render::LightKind::Directional;
    sun.position = {0.0F, 1.0F}; // down the screen
    // A ledge 20 wide at y = 50; shadows reach 100 past it.
    const std::vector<canvas::OccluderEdge> edges{{.a = {-10.0F, 50.0F}, .b = {10.0F, 50.0F}}};
    const auto row = canvas::cast_shadows(sun, edges, 100.0F);
    REQUIRE(row.size() == render::kShadowMapWidth);
    // The row spans the ledge, a pixel more each side, and depths go 100 past it.
    CHECK(sun.shadow_span == doctest::Approx(22.0));
    CHECK(sun.shadow_far == doctest::Approx(100.0));
    CHECK(sun.shadow_reach == doctest::Approx(1.0));
    // Under the ledge, the row holds the ledge's depth, above the point's.
    const render::ShadowCoord under = render::shadow_coord(sun, {0.0F, 80.0F});
    REQUIRE(under.column >= 0.0F);
    REQUIRE(under.column < static_cast<float>(render::kShadowMapWidth));
    const float stored =
        render::decode_shadow_depth(row.at(static_cast<std::size_t>(under.column)));
    CHECK(stored == doctest::Approx(render::shadow_coord(sun, {0.0F, 50.0F}).depth).epsilon(0.001));
    CHECK(under.depth == doctest::Approx(0.3));
    // Beside it, off the row, which the shader reads as no shadow.
    CHECK(render::shadow_coord(sun, {30.0F, 80.0F}).column < 0.0F);
}

TEST_CASE("collect_lighting gathers lights, the ambient color and shadow rows, and says what it "
          "leaves out") {
    Scene scene;
    scene.files.write("white.png", strip_png({{255, 255, 255, 255}}, 4));
    std::vector<asset::Handle<render::Image>> kept;
    const auto images = [&](const std::string& path) -> slash::Result<const render::Image*> {
        auto image = scene.assets.load<render::Image>(path);
        if (!image) {
            return image.error();
        }
        kept.push_back(std::move(image).value());
        return kept.back().get();
    };
    std::vector<canvas::LightProblem> problems;

    // Nothing to light, or only a disabled light: draws stay unshaded.
    scene.create("off", R"({"Transform2D": {}, "PointLight2D": {"enabled": false}})");
    CHECK_FALSE(canvas::collect_lighting(scene.world, images, {}, problems).lit);
    CHECK(problems.empty());

    scene.create("dark", R"({"CanvasModulate": {"color": [0.1, 0.2, 0.3, 1]}})");
    const entt::entity second =
        scene.create("darker", R"({"CanvasModulate": {"color": [0, 0, 0, 1]}})");
    // A lamp turned a quarter turn and stretched: its texture still centers on it.
    const entt::entity lamp =
        scene.create("lamp", R"({"Transform2D": {"position": [100, 50], "rotation": 1.5707964,
                                     "scale": [2, 1]},
                    "PointLight2D": {"texture": "white.png", "texture_scale": 10,
                                     "energy": 2, "color": [1, 0.5, 0.25, 1],
                                     "blend_mode": "sub", "range_item_cull_mask": 6,
                                     "shadow_enabled": true}})");
    const entt::entity sun =
        scene.create("sun", R"({"DirectionalLight2D": {"height": 3, "shadow_enabled": true,
                                          "shadow_filter": "pcf13"}})");
    const entt::entity gone =
        scene.create("gone", R"({"Transform2D": {}, "PointLight2D": {"texture": "gone.png"}})");
    const entt::entity flat =
        scene.create("flat", R"({"Transform2D": {"scale": [0, 1]}, "PointLight2D": {}})");
    const entt::entity typo =
        scene.create("typo", R"({"Transform2D": {}, "PointLight2D": {"blend_mode": "mul"}})");
    const entt::entity bad_cull =
        scene.create("bad_cull", R"({"Transform2D": {}, "LightOccluder2D": {
                       "polygon": [[0, 0], [1, 0]], "cull_mode": "both"}})");
    // A wall between the lamp and the bottom of the screen.
    const entt::entity wall =
        scene.create("wall", R"({"Transform2D": {"position": [100, 60]}, "LightOccluder2D": {
                   "polygon": [[-5, 0], [5, 0]], "closed": false}})");

    canvas::SceneLighting lighting = canvas::collect_lighting(scene.world, images, {}, problems);
    CHECK(lighting.lit);
    REQUIRE(lighting.canvases.size() == 1); // with no canvases given, the world's alone
    CHECK(lighting.canvases[0].lit);
    CHECK(lighting.canvases[0].ambient == std::array{0.1F, 0.2F, 0.3F, 1.0F});
    REQUIRE(lighting.lights.size() == 2);
    const render::Light& point = lighting.lights[0].light;
    CHECK(lighting.lights[0].entity == lamp);
    CHECK(lighting.lights[0].texture == "white.png");
    CHECK(point.kind == render::LightKind::Point);
    CHECK(point.blend == render::BlendMode::Sub);
    CHECK(point.color == std::array{2.0F, 1.0F, 0.5F, 1.0F});
    CHECK(point.item_cull_mask == 6);
    const auto uv = [&](float x, float y) {
        const auto& m = point.texture_transform;
        return std::array{(m[0] * x) + (m[1] * y) + m[2], (m[3] * x) + (m[4] * y) + m[5]};
    };
    CHECK(uv(100.0F, 50.0F)[0] == doctest::Approx(0.5));
    CHECK(uv(100.0F, 50.0F)[1] == doctest::Approx(0.5));
    // 40 pixels square, stretched 2x along the light's x, which points down the screen.
    CHECK(uv(100.0F, 90.0F)[0] == doctest::Approx(1.0).epsilon(0.001));
    CHECK(uv(120.0F, 50.0F)[1] == doctest::Approx(0.0).epsilon(0.001));
    CHECK(lighting.lights[1].entity == sun);
    CHECK(lighting.lights[1].light.kind == render::LightKind::Directional);
    CHECK(lighting.lights[1].light.height == 1.0F); // clamped
    CHECK(lighting.lights[1].light.shadow_filter == render::ShadowFilter::Pcf13);
    // Both lights see the wall, so each has a row.
    CHECK(lighting.shadow_rows == 2);
    CHECK(point.shadow_row == 0);
    CHECK(lighting.lights[1].light.shadow_row == 1);
    CHECK(lighting.shadow_pixels.size() == std::size_t{2} * render::kShadowMapWidth * 4);

    std::map<entt::entity, std::string> messages;
    for (const canvas::LightProblem& problem : problems) {
        messages.emplace(problem.entity, problem.message);
    }
    REQUIRE(messages.size() == 5);
    CHECK(messages[second] == "a canvas has one CanvasModulate, and \"dark\" is the one used");
    CHECK(messages[gone].starts_with("gone.png: "));
    CHECK(messages[flat] == "its texture_scale or Transform2D scale is 0, so it lights nothing");
    CHECK(messages[typo].find("\"mul\"") != std::string::npos);
    CHECK(messages[bad_cull].find("\"both\"") != std::string::npos);

    // Culling and masks decide which occluders cast. Seen from the lamp, the wall runs
    // counterclockwise on screen, so culling counterclockwise edges leaves the lamp unshadowed.
    auto& occluder = scene.world.ecs().get<world::LightOccluder2D>(wall);
    occluder.cull_mode = "counter_clockwise";
    problems.clear();
    lighting = canvas::collect_lighting(scene.world, images, {}, problems);
    CHECK(lighting.lights[0].light.shadow_row == -1);
    occluder.cull_mode = "clockwise";
    lighting = canvas::collect_lighting(scene.world, images, {}, problems);
    CHECK(lighting.lights[0].light.shadow_row == 0);
    occluder.cull_mode = "disabled";
    occluder.occluder_light_mask = 2; // the lights' shadow_item_cull_mask is 1
    lighting = canvas::collect_lighting(scene.world, images, {}, problems);
    CHECK(lighting.shadow_rows == 0);
    occluder.occluder_light_mask = 1;

    // The cache gives the same rows, and casts again when an occluder moves.
    canvas::ShadowCache cache;
    const canvas::SceneLighting first =
        canvas::collect_lighting(scene.world, images, {}, problems, &cache);
    CHECK(cache.rows.size() == 2);
    const canvas::SceneLighting again =
        canvas::collect_lighting(scene.world, images, {}, problems, &cache);
    CHECK(again.shadow_pixels == first.shadow_pixels);
    scene.world.ecs().get<world::Transform2D>(wall).position.y = 70.0F;
    const canvas::SceneLighting moved =
        canvas::collect_lighting(scene.world, images, {}, problems, &cache);
    CHECK(moved.shadow_pixels != first.shadow_pixels);
    scene.world.ecs().remove<world::LightOccluder2D>(wall);
    (void)canvas::collect_lighting(scene.world, images, {}, problems, &cache);
    CHECK(cache.rows.empty());
}

TEST_CASE("pack_light_atlas places each light texture once, with room around it") {
    Scene scene;
    scene.files.write("blue.png", strip_png({{0, 0, 255, 255}}, 4));
    std::vector<asset::Handle<render::Image>> kept;
    const auto images = [&](const std::string& path) -> slash::Result<const render::Image*> {
        auto image = scene.assets.load<render::Image>(path);
        if (!image) {
            return image.error();
        }
        kept.push_back(std::move(image).value());
        return kept.back().get();
    };
    std::vector<canvas::SceneLight> lights(4);
    lights[0].texture = "blue.png";
    lights[1].texture = ""; // the default light
    lights[2].texture = "blue.png";
    lights[3].light.kind = render::LightKind::Directional; // needs no texture
    auto atlas = canvas::pack_light_atlas(lights, images);
    REQUIRE(atlas.ok());
    // 128x128 and 4x4 with a pixel between them fit in 256 across.
    CHECK(atlas.value().width == 256);
    CHECK(lights[0].light.atlas_rect == lights[2].light.atlas_rect);
    CHECK(lights[0].light.atlas_rect != lights[1].light.atlas_rect);
    const auto at = [&](const render::Light& light, float u, float v) {
        const auto& rect = light.atlas_rect;
        return render::sample_image(atlas.value(), render::Filter::Linear, false,
                                    rect[0] + (u * rect[2]), rect[1] + (v * rect[3]));
    };
    // Filtering at a texture's edges never reads its neighbors.
    for (const float u : {0.0F, 0.5F, 1.0F}) {
        CHECK(at(lights[0].light, u, u) == std::array{0.0F, 0.0F, 1.0F, 1.0F});
    }
    CHECK(at(lights[1].light, 0.5F, 0.5F)[3] > 0.95F);
    CHECK(at(lights[1].light, 0.0F, 0.5F)[3] == 0.0F);
    CHECK(canvas::default_light_image().width == 128);

    lights[0].texture = "gone.png";
    auto missing = canvas::pack_light_atlas(lights, images);
    REQUIRE_FALSE(missing.ok());
    CHECK(missing.error().message.starts_with("gone.png: "));
}

TEST_CASE("the canvas counts lights and shadows, and makes their textures only when needed") {
    Scene scene;
    scene.files.write("red.png", strip_png({{255, 0, 0, 255}}, 4));
    scene.create("hero", R"({"Transform2D": {}, "Sprite2D": {"texture": "red.png"}})");
    const auto device = render::create_null_device();
    canvas::CanvasRenderer renderer(*device, scene.assets);
    auto target = device->create_texture(
        {.width = 8, .height = 8, .filter = render::Filter::Linear, .render_target = true});
    REQUIRE(target.ok());
    canvas::CanvasOptions options;
    options.target = target.value();
    REQUIRE(renderer.draw(scene.world, options).ok());
    CHECK(renderer.lights_drawn() == 0);
    CHECK(device->stats().textures == 2); // the target and red.png

    // A lamp with shadows and a wall: the light atlas and the shadow map join them.
    scene.create("lamp", R"({"Transform2D": {"position": [4, 4]},
                             "PointLight2D": {"shadow_enabled": true}})");
    const entt::entity wall =
        scene.create("wall", R"({"Transform2D": {"position": [10, 4]}, "LightOccluder2D": {
                   "polygon": [[0, -2], [0, 2]], "closed": false}})");
    Warnings warnings;
    scene.create("gone", R"({"Transform2D": {}, "PointLight2D": {"texture": "gone.png"}})");
    REQUIRE(renderer.draw(scene.world, options).ok());
    CHECK(renderer.sprites_drawn() == 1);
    CHECK(renderer.lights_drawn() == 1);
    CHECK(renderer.shadows_drawn() == 1);
    CHECK(device->stats().textures == 4);
    REQUIRE(warnings.lines.size() == 1);
    CHECK(warnings.logged(R"("id":"gone")"));
    CHECK(warnings.logged("a light can't shine"));

    // Nothing changed: no new textures, and no new warnings.
    REQUIRE(renderer.draw(scene.world, options).ok());
    CHECK(device->stats().textures == 4);
    CHECK(warnings.lines.size() == 1);

    // No occluder, no shadow map; no lights, no atlas.
    scene.world.ecs().remove<world::LightOccluder2D>(wall);
    REQUIRE(renderer.draw(scene.world, options).ok());
    CHECK(renderer.shadows_drawn() == 0);
    CHECK(device->stats().textures == 3);
    scene.world.ecs().get<world::PointLight2D>(scene.world.find("lamp")).enabled = false;
    scene.world.ecs().get<world::PointLight2D>(scene.world.find("gone")).enabled = false;
    REQUIRE(renderer.draw(scene.world, options).ok());
    CHECK(renderer.lights_drawn() == 0);
    CHECK(device->stats().textures == 2);
}

TEST_CASE("lights shade sprites on the software device and a GPU, as their masks, materials and "
          "shadows say") {
    for (const auto& device : drawing_devices()) {
        CAPTURE(slash::render::backend_name(device->backend()));
        Scene scene;
        scene.files.write("white.png", strip_png({{255, 255, 255, 255}}, 8));
        scene.files.write("flat.material.json", R"({"format": 1, "light_mode": "unshaded"})");
        scene.create("dark", R"({"CanvasModulate": {"color": [0.5, 0.5, 0.5, 1]}})");
        // A red light over the right half: 16 across and, stretched, 32 down.
        scene.create("lamp", R"({"Transform2D": {"position": [24, 16], "scale": [1, 2]},
                             "PointLight2D": {"texture": "white.png", "texture_scale": 2,
                                              "color": [1, 0, 0, 1], "shadow_enabled": true}})");
        const std::string sprite =
            R"("Sprite2D": {"texture": "white.png", "texture_filter": "nearest")";
        scene.create("plain", R"({"Transform2D": {"position": [4, 4]}, )" + sprite + "}}");
        scene.create("flat", R"({"Transform2D": {"position": [4, 20]}, )" + sprite +
                                 R"(, "material": "flat.material.json"}})");
        scene.create("lit", R"({"Transform2D": {"position": [20, 4]}, )" + sprite + "}}");
        const entt::entity masked =
            scene.create("masked", R"({"Transform2D": {"position": [20, 24]}, )" + sprite +
                                       R"(, "light_mask": 2}})");
        auto target = device->create_texture(
            {.width = 32, .height = 32, .filter = render::Filter::Nearest, .render_target = true});
        REQUIRE(target.ok());
        canvas::CanvasRenderer renderer(*device, scene.assets);
        canvas::CanvasOptions options;
        options.target = target.value();
        options.clear = {.r = 0.0F, .g = 0.0F, .b = 0.0F, .a = 1.0F};
        REQUIRE(renderer.draw(scene.world, options).ok());
        auto image = device->read_texture(target.value());
        REQUIRE(image.ok());
        const auto near = [](Rgba got, Rgba want) {
            for (std::size_t i = 0; i < 4; ++i) {
                if (got.at(i) + 1 < want.at(i) || got.at(i) > want.at(i) + 1) {
                    return false;
                }
            }
            return true;
        };
        CHECK(near(pixel(image.value(), 4, 4), {128, 128, 128, 255}));   // ambient only
        CHECK(pixel(image.value(), 4, 20) == Rgba{255, 255, 255, 255});  // unshaded
        CHECK(near(pixel(image.value(), 20, 4), {255, 128, 128, 255}));  // ambient plus red
        CHECK(near(pixel(image.value(), 20, 24), {128, 128, 128, 255})); // not on the light's mask

        // A wall just under the lamp shadows everything below it.
        scene.world.ecs().get<world::Sprite2D>(masked).light_mask = 1;
        scene.create("wall", R"({"Transform2D": {"position": [24, 17]}, "LightOccluder2D": {
                             "polygon": [[-16, 0], [16, 0]], "closed": false}})");
        REQUIRE(renderer.draw(scene.world, options).ok());
        CHECK(renderer.shadows_drawn() == 1);
        image = device->read_texture(target.value());
        REQUIRE(image.ok());
        CHECK(near(pixel(image.value(), 20, 4), {255, 128, 128, 255}));
        CHECK(near(pixel(image.value(), 20, 24), {128, 128, 128, 255}));
        scene.world.ecs().remove<world::LightOccluder2D>(scene.world.find("wall"));
        REQUIRE(renderer.draw(scene.world, options).ok());
        image = device->read_texture(target.value());
        REQUIRE(image.ok());
        CHECK(near(pixel(image.value(), 20, 24), {255, 128, 128, 255}));
    }
}

TEST_CASE("triangulate fills simple polygons wound either way, and nothing without an area") {
    const std::vector<slash::Vec2> square{{0, 0}, {10, 0}, {10, 10}, {0, 10}};
    CHECK(canvas::triangulate(square).size() == 6);
    // An L, both ways round: 6 points make 4 triangles covering its area.
    std::vector<slash::Vec2> ell{{0, 0}, {20, 0}, {20, 10}, {10, 10}, {10, 20}, {0, 20}};
    // The area the triangles cover, and whether each has some.
    const auto area = [](const std::vector<slash::Vec2>& points) {
        const std::vector<std::uint32_t> triangles = canvas::triangulate(points);
        float total = 0.0F;
        for (std::size_t i = 0; i < triangles.size(); i += 3) {
            const slash::Vec2 a = points[triangles[i]];
            const slash::Vec2 b = points[triangles[i + 1]];
            const slash::Vec2 c = points[triangles[i + 2]];
            const float twice = std::abs(((b.x - a.x) * (c.y - a.y)) - ((b.y - a.y) * (c.x - a.x)));
            CHECK(twice > 0.0F);
            total += twice / 2.0F;
        }
        return total;
    };
    CHECK(canvas::triangulate(ell).size() == 12);
    CHECK(area(ell) == doctest::Approx(300.0F));
    std::ranges::reverse(ell);
    CHECK(canvas::triangulate(ell).size() == 12);
    CHECK(area(ell) == doctest::Approx(300.0F));
    // A point in line with its neighbors, or on another, makes no empty triangle.
    CHECK(area({{0, 0}, {5, 0}, {10, 0}, {10, 10}, {0, 10}}) == doctest::Approx(100.0F));
    CHECK(area({{0, 0}, {10, 0}, {10, 0}, {10, 10}, {0, 10}}) == doctest::Approx(100.0F));
    // No area: a line, a bow tie whose halves cancel, too few points.
    CHECK(canvas::triangulate(std::vector<slash::Vec2>{{0, 0}, {5, 5}, {10, 10}}).empty());
    CHECK(
        canvas::triangulate(std::vector<slash::Vec2>{{0, 0}, {10, 10}, {10, 0}, {0, 10}}).empty());
    CHECK(canvas::triangulate(std::vector<slash::Vec2>{{0, 0}, {5, 5}}).empty());
}

TEST_CASE("collision_shapes draws each shape as Godot's Visible Collision Shapes") {
    Scene scene;
    const auto byte = [](float channel) {
        return static_cast<std::uint8_t>(std::lround(std::clamp(channel, 0.0F, 1.0F) * 255.0F));
    };
    const auto rgba = [&](slash::Vec4 color) {
        return Rgba{byte(color.x), byte(color.y), byte(color.z), byte(color.w)};
    };
    const slash::Vec4 debug = canvas::kCollisionColor;
    const Rgba fill = rgba(debug);
    const Rgba edge = rgba({debug.x, debug.y, debug.z, 1.0F});
    // The box of a run of vertices.
    struct Box {
        float left = 1e9F;
        float top = 1e9F;
        float right = -1e9F;
        float bottom = -1e9F;
    };
    const auto box_of = [](std::span<const render::Vertex2D> vertices) {
        Box box;
        for (const render::Vertex2D& vertex : vertices) {
            box.left = std::min(box.left, vertex.x);
            box.top = std::min(box.top, vertex.y);
            box.right = std::max(box.right, vertex.x);
            box.bottom = std::max(box.bottom, vertex.y);
        }
        return box;
    };

    SUBCASE("a rectangle fills two triangles and has a one-pixel outline") {
        scene.create("crate", R"({"Transform2D": {"position": [100, 50]},
                              "CollisionShape2D": {"size": [20, 10]}})");
        const canvas::CollisionShapes drawn = canvas::collision_shapes(scene.world);
        CHECK(drawn.shapes == 1);
        REQUIRE(drawn.vertices.size() == 6 + (4 * 6));
        CHECK(drawn.vertices[0].rgba == fill);
        CHECK(drawn.vertices[6].rgba == edge);
        const Box filled = box_of(std::span(drawn.vertices).first(6));
        CHECK(filled.left == 90.0F);
        CHECK(filled.right == 110.0F);
        CHECK(filled.top == 45.0F);
        CHECK(filled.bottom == 55.0F);
        const Box outline = box_of(std::span(drawn.vertices).subspan(6));
        CHECK(outline.left == 89.5F);
        CHECK(outline.bottom == 55.5F);
    }

    SUBCASE("shapes are placed as physics places them: moved, turned, then scaled") {
        // The shape sits 10 right of the body; the body is turned a quarter turn and doubled
        // across, so the shape ends up 20 below it, 20 high and 10 wide.
        scene.create("body", R"({"Transform2D": {"position": [100, 50], "rotation": 1.5707964,
                                                 "scale": [2, 1]},
                             "CollisionShape2D": {"size": [10, 10], "position": [10, 0]}})");
        const canvas::CollisionShapes drawn = canvas::collision_shapes(scene.world);
        const Box filled = box_of(std::span(drawn.vertices).first(6));
        CHECK(filled.left == doctest::Approx(95.0F));
        CHECK(filled.right == doctest::Approx(105.0F));
        CHECK(filled.top == doctest::Approx(60.0F));
        CHECK(filled.bottom == doctest::Approx(80.0F));
    }

    SUBCASE("circles have 24 points and capsules 26, their radii scaled by the larger axis") {
        scene.create("ball", R"({"Transform2D": {"position": [50, 50], "scale": [2, 1]},
                             "CollisionShape2D": {"shape": "circle", "radius": 5}})");
        scene.create("pill", R"({"Transform2D": {"position": [200, 50]},
                             "CollisionShape2D": {"shape": "capsule", "radius": 5,
                                                  "height": 30}})");
        const canvas::CollisionShapes drawn = canvas::collision_shapes(scene.world);
        CHECK(drawn.shapes == 2);
        // 22 triangles and 24 edges, then 24 triangles and 26 edges.
        constexpr std::size_t kCircle = (22 * 3) + (24 * 6);
        constexpr std::size_t kCapsule = (24 * 3) + (26 * 6);
        REQUIRE(drawn.vertices.size() == kCircle + kCapsule);
        const Box ball = box_of(std::span(drawn.vertices).first(22 * 3));
        CHECK(ball.left == doctest::Approx(40.0F));
        CHECK(ball.right == doctest::Approx(60.0F));
        CHECK(ball.top == doctest::Approx(40.0F).epsilon(0.01));
        const Box pill = box_of(std::span(drawn.vertices).subspan(kCircle, 24 * 3));
        CHECK(pill.left == doctest::Approx(195.0F));
        CHECK(pill.right == doctest::Approx(205.0F));
        CHECK(pill.top == doctest::Approx(35.0F));
        CHECK(pill.bottom == doctest::Approx(65.0F));
    }

    SUBCASE("a segment is a line 3 wide, and a polygon shape fills its convex hull") {
        scene.create("rail", R"({"CollisionShape2D": {"shape": "segment", "a": [0, 10],
                                                      "b": [40, 10]}})");
        // The middle point is inside the hull, so a triangle is left.
        scene.create("wedge", R"({"CollisionShape2D": {"shape": "polygon",
                                    "points": [[0, 0], [30, 0], [10, 5], [0, 30]]}})");
        const canvas::CollisionShapes drawn = canvas::collision_shapes(scene.world);
        REQUIRE(drawn.vertices.size() == 6 + 3 + (3 * 6));
        CHECK(drawn.vertices[0].rgba == fill);
        const Box rail = box_of(std::span(drawn.vertices).first(6));
        CHECK(rail.top == 8.5F);
        CHECK(rail.bottom == 11.5F);
        CHECK(rail.right == 40.0F);
    }

    SUBCASE("a disabled shape is gray at half alpha, and one-way collision adds an arrow") {
        scene.create("ledge", R"({"CollisionShape2D": {"disabled": true,
                                                       "one_way_collision": true}})");
        const canvas::CollisionShapes drawn = canvas::collision_shapes(scene.world);
        REQUIRE(drawn.vertices.size() == 6 + 24 + 6 + 3);
        CHECK(drawn.vertices[0].rgba == rgba({0.7F, 0.7F, 0.7F, 0.21F}));
        CHECK(drawn.vertices[6].rgba == rgba({0.7F, 0.7F, 0.7F, 1.0F}));
        // The arrow: the color inverted, darkened a quarter, from the origin down to a tip at 28.
        const slash::Vec4 tip{(1.0F - debug.x) * 0.75F, (1.0F - debug.y) * 0.75F,
                              (1.0F - debug.z) * 0.75F, debug.w};
        CHECK(drawn.vertices[30].rgba == rgba(tip));
        const Box arrow = box_of(std::span(drawn.vertices).subspan(30));
        CHECK(arrow.top == 0.0F);
        CHECK(arrow.bottom == 28.0F);
        CHECK(arrow.left == doctest::Approx(-5.656854F));
        CHECK(arrow.right == doctest::Approx(5.656854F));
    }

    SUBCASE("a CollisionPolygon2D fills any simple polygon and has a red outline") {
        scene.create("ground", R"({"Transform2D": {"position": [10, 10]},
                               "CollisionPolygon2D": {
                                 "polygon": [[0, 0], [20, 0], [20, 10], [10, 10], [10, 20],
                                             [0, 20]],
                                 "one_way_collision": true}})");
        const canvas::CollisionShapes drawn = canvas::collision_shapes(scene.world);
        CHECK(drawn.shapes == 1);
        REQUIRE(drawn.vertices.size() == 12 + (6 * 6) + 6 + 3);
        CHECK(drawn.vertices[0].rgba == fill);
        CHECK(drawn.vertices[12].rgba == rgba({0.9F, 0.2F, 0.0F, 1.0F}));
        // Its arrow is the color made opaque, 3 wide.
        CHECK(drawn.vertices[48].rgba == edge);
        const Box arrow = box_of(std::span(drawn.vertices).subspan(48, 6));
        CHECK(arrow.left == 8.5F);
        CHECK(arrow.right == 11.5F);
        CHECK(arrow.bottom == 30.0F);
    }

    SUBCASE("shapes draw in the order entities were made, where transform_of puts them") {
        scene.create("first", R"({"Transform2D": {"position": [0, 0]},
                              "CollisionShape2D": {"shape": "segment", "a": [0, 0],
                                                   "b": [10, 0]}})");
        scene.create("second", R"({"CollisionPolygon2D": {"polygon": [[0, 0], [5, 0], [0, 5]]},
                               "CollisionShape2D": {"shape": "segment", "a": [0, 5],
                                                    "b": [10, 5]}})");
        const canvas::CollisionShapes drawn =
            canvas::collision_shapes(scene.world, [](entt::entity, const world::Transform2D& at) {
                world::Transform2D moved = at;
                moved.position.x += 100.0F;
                return moved;
            });
        CHECK(drawn.shapes == 3);
        REQUIRE(drawn.vertices.size() == 6 + 6 + 3 + (3 * 6));
        CHECK(box_of(std::span(drawn.vertices).first(6)).left == 100.0F);
        // The second entity's shape, then its polygon.
        CHECK(box_of(std::span(drawn.vertices).subspan(6, 6)).top == 3.5F);
        CHECK(drawn.vertices[12].rgba == fill);
        // With no Transform2D, an entity's shapes sit at the origin.
        CHECK(box_of(std::span(drawn.vertices).subspan(6, 6)).left == 0.0F);
    }
}

TEST_CASE("capture and the canvas draw collision shapes over the world only when asked") {
    Scene scene;
    slash::ui::Settings ui;
    ui.viewport = {64.0F, 48.0F};
    slash::ui::set_settings(scene.world, ui);
    canvas::Settings settings;
    settings.clear_color = {.r = 0.0F, .g = 0.0F, .b = 0.0F, .a = 1.0F};
    canvas::set_settings(scene.world, settings);
    scene.files.write("white.png", strip_png({{255, 255, 255, 255}}, 8));
    // A white sprite under the right half of the box: shapes draw over sprites.
    scene.create("box", R"({"Transform2D": {"position": [16.5, 16.5]},
                        "CollisionShape2D": {"size": [20, 10]}})");
    scene.create("wall", R"({"Transform2D": {"position": [24, 16]},
                         "Sprite2D": {"texture": "white.png", "z_index": 5}})");
    const auto near = [](Rgba got, Rgba want) {
        for (std::size_t i = 0; i < 4; ++i) {
            if (std::abs(int{got[i]} - int{want[i]}) > 2) {
                return false;
            }
        }
        return true;
    };

    auto plain = canvas::capture(scene.world, scene.assets);
    REQUIRE(plain.ok());
    CHECK(plain.value().shapes == 0);
    CHECK(pixel(plain.value().image, 12, 16) == Rgba{0, 0, 0, 255});

    auto shown = canvas::capture(scene.world, scene.assets, {.collisions = true});
    REQUIRE(shown.ok());
    CHECK(shown.value().shapes == 1);
    // The fill is 42% of (0, 0.6, 0.7) over black, and over the white sprite.
    CHECK(near(pixel(shown.value().image, 12, 16), {0, 64, 75, 255}));
    CHECK(near(pixel(shown.value().image, 24, 16), {148, 212, 223, 255}));
    // The outline is opaque, one pixel wide along x = 6.5; the pixel left of it is clear.
    CHECK(near(pixel(shown.value().image, 6, 16), {0, 153, 179, 255}));
    CHECK(pixel(shown.value().image, 5, 16) == Rgba{0, 0, 0, 255});
}

TEST_CASE("collision shapes draw over sprites, and an overlay's controls over everything, on the "
          "software device and a GPU") {
    for (const auto& device : drawing_devices()) {
        CAPTURE(slash::render::backend_name(device->backend()));
        Scene scene;
        scene.files.write("white.png", strip_png({{255, 255, 255, 255}}, 8));
        scene.create("box", R"({"Transform2D": {"position": [16.5, 16.5]},
                            "CollisionShape2D": {"size": [20, 10]}})");
        scene.create("wall", R"({"Transform2D": {"position": [24, 16]},
                             "Sprite2D": {"texture": "white.png", "z_index": 5}})");
        // The game's control, and the overlay's over it in the bottom right corner.
        scene.create("hud", R"({"Control": {"offset_left": 32, "offset_top": 32,
                                            "offset_right": 48, "offset_bottom": 48},
                            "ColorRect": {"color": [1, 0, 0, 1]}})");
        Scene overlay;
        overlay.create("panel", R"({"Control": {"offset_left": 40, "offset_top": 40,
                                                "offset_right": 48, "offset_bottom": 48},
                                    "ColorRect": {"color": [0, 0, 1, 1]}})");
        auto target = device->create_texture(
            {.width = 48, .height = 48, .filter = render::Filter::Nearest, .render_target = true});
        REQUIRE(target.ok());
        canvas::CanvasRenderer renderer(*device, scene.assets);
        canvas::CanvasOptions options;
        options.target = target.value();
        options.clear = {.r = 0.0F, .g = 0.0F, .b = 0.0F, .a = 1.0F};
        options.width = 48.0F;
        options.height = 48.0F;
        options.collisions = true;
        options.overlay = &overlay.world;
        REQUIRE(renderer.draw(scene.world, options).ok());
        CHECK(renderer.shapes_drawn() == 1);
        CHECK(renderer.controls_drawn() == 1);
        auto image = device->read_texture(target.value());
        REQUIRE(image.ok());
        const auto near = [](Rgba got, Rgba want) {
            for (std::size_t i = 0; i < 4; ++i) {
                if (std::abs(int{got[i]} - int{want[i]}) > 2) {
                    return false;
                }
            }
            return true;
        };
        CHECK(near(pixel(image.value(), 12, 16), {0, 64, 75, 255}));
        CHECK(near(pixel(image.value(), 24, 16), {148, 212, 223, 255}));
        CHECK(near(pixel(image.value(), 6, 16), {0, 153, 179, 255}));
        CHECK(pixel(image.value(), 5, 16) == Rgba{0, 0, 0, 255});
        CHECK(pixel(image.value(), 36, 36) == Rgba{255, 0, 0, 255});
        CHECK(pixel(image.value(), 44, 44) == Rgba{0, 0, 255, 255});

        // Between ticks, shapes move with their entities.
        renderer.before_tick(scene.world);
        scene.world.ecs().get<world::Transform2D>(scene.world.find("box")).position.y = 32.5F;
        options.interpolation = 0.5;
        REQUIRE(renderer.draw(scene.world, options).ok());
        image = device->read_texture(target.value());
        REQUIRE(image.ok());
        CHECK(near(pixel(image.value(), 12, 26), {0, 64, 75, 255})); // y spans 19.5..29.5
        CHECK(pixel(image.value(), 12, 16) == Rgba{0, 0, 0, 255});
    }
}
