#include "slash/asset/assets.hpp"
#include "slash/canvas/canvas.hpp"
#include "slash/canvas/sprite_frames.hpp"
#include "slash/core/json.hpp"
#include "slash/platform/process.hpp"
#include "slash/render/device.hpp"
#include "slash/render/image.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

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

std::unique_ptr<render::RenderDevice> gpu_device() {
    auto device = render::create_device({.window = nullptr, .driver = "auto", .debug = false});
    if (!device) {
        if (!slash::platform::environment_variable("SLASH_EXPECT_GPU").empty()) {
            FAIL("SLASH_EXPECT_GPU is set, but no GPU device works: " << device.error().message);
        }
        MESSAGE("no GPU here, so sprite pixels are not checked: " << device.error().message);
        return nullptr;
    }
    return std::move(device).value();
}

Rgba pixel(const render::Image& image, std::uint32_t x, std::uint32_t y) {
    const std::size_t at = ((std::size_t{y} * image.width) + x) * 4;
    return {image.pixels[at], image.pixels[at + 1], image.pixels[at + 2], image.pixels[at + 3]};
}

} // namespace

TEST_CASE("sprites draw where Godot would put them, in z order, on a GPU") {
    auto device = gpu_device();
    if (device == nullptr) {
        return;
    }
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
