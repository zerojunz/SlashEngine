#include "slash/core/json.hpp"
#include "slash/library/material.hpp"
#include "slash/render/image.hpp"
#include "slash/scene3d/material.hpp"
#include "slash/scene3d/model.hpp"
#include "slash/scene3d/recipe.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace json = slash::json;
namespace library = slash::library;
namespace render = slash::render;
namespace scene3d = slash::scene3d;

json::Value parse(std::string_view text) {
    auto doc = json::parse(text);
    REQUIRE_MESSAGE(doc.ok(), (doc.ok() ? "" : doc.error().message));
    return std::move(doc).value();
}

// A reader over files kept in memory, by path.
scene3d::ModelReader reader(const std::map<std::string, std::string, std::less<>>& files) {
    return [&files](std::string_view path) -> slash::Result<std::string> {
        if (const auto found = files.find(path); found != files.end()) {
            return found->second;
        }
        return slash::make_error(slash::ErrorCode::NotFound, "no such file");
    };
}

render::Image image_of(std::uint32_t width, std::uint32_t height) {
    render::Image image;
    image.width = width;
    image.height = height;
    image.pixels.resize(static_cast<std::size_t>(width) * height * 4);
    return image;
}

void set(render::Image& image, std::uint32_t x, std::uint32_t y, std::uint8_t r, std::uint8_t g,
         std::uint8_t b) {
    const std::size_t at = (static_cast<std::size_t>(y) * image.width + x) * 4;
    image.pixels[at] = r;
    image.pixels[at + 1] = g;
    image.pixels[at + 2] = b;
    image.pixels[at + 3] = 255;
}

std::array<std::uint8_t, 4> pixel(const render::Image& image, std::uint32_t x, std::uint32_t y) {
    const std::size_t at = (static_cast<std::size_t>(y) * image.width + x) * 4;
    return {image.pixels[at], image.pixels[at + 1], image.pixels[at + 2], image.pixels[at + 3]};
}

// A diagonal ramp, brighter down and to the right, which doesn't tile: its edges are far apart.
render::Image ramp(std::uint32_t side) {
    render::Image image = image_of(side, side);
    for (std::uint32_t y = 0; y < side; ++y) {
        for (std::uint32_t x = 0; x < side; ++x) {
            const auto value = static_cast<std::uint8_t>((x + y) * 255 / (2 * side - 2));
            set(image, x, y, value, value, value);
        }
    }
    return image;
}

std::string png(const render::Image& image) {
    auto bytes = render::encode_png(image);
    REQUIRE(bytes.ok());
    return std::move(bytes).value();
}

} // namespace

TEST_CASE("a material file is a StandardMaterial3D that recipes share, alone or changed") {
    const std::map<std::string, std::string, std::less<>> files{
        {"materials/stone.standard_material.json",
         R"({"format": 1, "albedo_color": [0.5, 0.5, 0.5, 1], "roughness": 0.7,
             "albedo_texture": "materials/stone_albedo.png"})"},
        {"materials/stone_albedo.png", png(ramp(8))},
        {"materials/bad.standard_material.json", R"({"format": 1, "roughness": "high"})"},
        {"materials/lost.standard_material.json",
         R"({"format": 1, "albedo_texture": "materials/lost.png"})"},
    };
    const scene3d::ModelReader read = reader(files);

    // The file on its own: its schema, fields and textures.
    CHECK(scene3d::check_standard_material(files.at("materials/stone.standard_material.json"), read)
              .empty());
    auto stone =
        scene3d::parse_standard_material(parse(files.at("materials/stone.standard_material.json")));
    REQUIRE(stone.ok());
    CHECK(stone.value().roughness == doctest::Approx(0.7));
    CHECK(stone.value().albedo_texture == "materials/stone_albedo.png");
    const auto bad = scene3d::validate_standard_material(parse(R"({"roughness": 0.5})"));
    REQUIRE_FALSE(bad.empty());
    const auto lost =
        scene3d::check_standard_material(files.at("materials/lost.standard_material.json"), read);
    REQUIRE(lost.size() == 1);
    CHECK(lost[0].path == "/albedo_texture");
    CHECK(lost[0].message == "\"materials/lost.png\" could not be read: no such file");

    // A recipe's material names the file, as it is or with fields over it, and a variant can
    // change its fields or swap the file.
    const std::string wall = R"({"format": 1,
        "materials": {
          "stone": "materials/stone.standard_material.json",
          "wet": {"extends": "materials/stone.standard_material.json", "roughness": 0.2}},
        "parts": [{"name": "a", "shape": "box", "material": "stone"},
                  {"name": "b", "shape": "box", "material": "wet", "position": [2, 0, 0]}],
        "variants": {
          "mossy": {"materials": {"stone": {"albedo_color": [0.3, 0.5, 0.3, 1]}}},
          "plain": {"materials": {"wet": {"roughness": 1}}}}})";
    auto model = scene3d::load_model(wall, "props/wall.model.json", read);
    REQUIRE_MESSAGE(model.ok(), (model.ok() ? "" : model.error().message));
    REQUIRE(model.value().materials.size() == 2);
    CHECK(model.value().materials[0].standard.roughness == doctest::Approx(0.7));
    CHECK(model.value().materials[0].standard.albedo_color.x == doctest::Approx(0.5));
    CHECK(model.value().materials[1].standard.roughness == doctest::Approx(0.2));
    // Both draw the file's texture, which the model holds once.
    REQUIRE(model.value().images.size() == 1);
    CHECK(model.value().images[0].path == "materials/stone_albedo.png");
    CHECK(model.value().materials[0].albedo_image == 0);
    CHECK(model.value().materials[1].albedo_image == 0);
    CHECK(scene3d::check_model(wall, "props/wall.model.json", read).empty());
    auto mossy = scene3d::load_model(wall, "props/wall.model.json::mossy", read);
    REQUIRE(mossy.ok());
    CHECK(mossy.value().materials[0].standard.albedo_color.y == doctest::Approx(0.5));
    CHECK(mossy.value().materials[0].standard.roughness == doctest::Approx(0.7));

    // Checked alone, a recipe keeps to its own fields; built, it needs the files.
    CHECK(scene3d::validate_model_recipe(parse(wall)).empty());
    auto unread = scene3d::build_model_recipe(parse(wall));
    REQUIRE_FALSE(unread.ok());
    CHECK(unread.error().message ==
          "/materials/stone: a material file is read from the project: load the recipe as a "
          "model, through its MeshInstance3D or load_model");

    // Each problem with a file is the material's.
    const auto first = [&read](std::string_view material) {
        const std::string recipe = R"({"format": 1, "materials": {"m": )" + std::string(material) +
                                   R"(}, "parts": [{"shape": "box", "material": "m"}]})";
        const auto errors = scene3d::check_model(recipe, "a.model.json", read);
        REQUIRE_FALSE(errors.empty());
        return errors.front().path + ": " + errors.front().message;
    };
    CHECK(first(R"("materials/none.standard_material.json")") ==
          "/materials/m: \"materials/none.standard_material.json\" could not be read: no such "
          "file");
    CHECK(first(R"("materials/stone_albedo.png")") ==
          "/materials/m: \"materials/stone_albedo.png\" isn't a material file "
          "(.standard_material.json)");
    CHECK(first(R"({"extends": "materials/stone.png"})") ==
          "/materials/m/extends: \"materials/stone.png\" isn't a material file "
          "(.standard_material.json)");
    CHECK(first(R"("materials/bad.standard_material.json")")
              .starts_with("/materials/m: \"materials/bad.standard_material.json\" at /roughness"));
    CHECK(first(R"("materials/lost.standard_material.json")") ==
          "/materials/m: \"materials/lost.standard_material.json\" at /albedo_texture: "
          "\"materials/lost.png\" could not be read: no such file");
}

TEST_CASE("a material's image is made to tile, and its normal and roughness maps tile with it") {
    // One flat color has no seam; a ramp's edges are far apart.
    render::Image flat = image_of(8, 8);
    for (std::uint32_t y = 0; y < 8; ++y) {
        for (std::uint32_t x = 0; x < 8; ++x) {
            set(flat, x, y, 90, 120, 60);
        }
    }
    CHECK(library::seam_ratio(flat) == 0.0);
    const render::Image slope = ramp(32);
    CHECK(library::seam_ratio(slope) > 20.0);

    // Made to tile, its middle stays close to what it was and its edges meet.
    const render::Image tiled = library::make_seamless(slope);
    CHECK(library::seam_ratio(tiled) < library::kSeamless);
    CHECK(library::seam_ratio(tiled) < 1.0);
    const auto middle = pixel(tiled, 16, 16);
    CHECK(std::abs(static_cast<int>(middle[0]) - static_cast<int>(pixel(slope, 16, 16)[0])) <= 12);
    CHECK(middle[3] == 255);
    // The same bytes every time.
    CHECK(library::make_seamless(slope).pixels == tiled.pixels);

    // A flat image's normals point straight out; a slope's lean away from where it rises.
    const render::Image level = library::normal_map(flat);
    CHECK(pixel(level, 3, 3) == std::array<std::uint8_t, 4>{128, 128, 255, 255});
    render::Image rising = image_of(16, 16);
    for (std::uint32_t y = 0; y < 16; ++y) {
        for (std::uint32_t x = 0; x < 16; ++x) {
            const auto value = static_cast<std::uint8_t>(x * 8);
            set(rising, x, y, value, value, value);
        }
    }
    const auto leaning = pixel(library::normal_map(rising), 8, 8);
    CHECK(leaning[0] < 128); // rising to the right leans left
    CHECK(leaning[1] == 128);
    CHECK(leaning[2] > 200);

    // Darker is rougher.
    render::Image two = image_of(2, 1);
    set(two, 0, 0, 255, 255, 255);
    set(two, 1, 0, 0, 0, 0);
    const render::Image rough = library::roughness_map(two);
    CHECK(pixel(rough, 0, 0)[0] == 140);
    CHECK(pixel(rough, 1, 0)[0] == 240);
}
