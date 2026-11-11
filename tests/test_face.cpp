// Faces: .face.json recipes (controls, carves, blends, styles), the sculpted head both
// dimensions draw, and characters wearing them in 3D and 2D.
#include "slash/core/json.hpp"
#include "slash/scene3d/character.hpp"
#include "slash/scene3d/model.hpp"
#include "slash/scene3d/skeleton.hpp"
#include "slash/world/character.hpp"
#include "slash/world/face.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace json = slash::json;
namespace scene3d = slash::scene3d;
namespace world = slash::world;
using slash::real_t;
using slash::Vec2;
using slash::Vec3;

namespace {

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE_MESSAGE(parsed.ok(), text);
    return std::move(parsed).value();
}

world::FileReader reader(const std::map<std::string, std::string_view>& files) {
    return [files](std::string_view path) -> slash::Result<std::string> {
        const auto found = files.find(std::string(path));
        if (found == files.end()) {
            return slash::make_error(slash::ErrorCode::NotFound, "no such file");
        }
        return std::string(found->second);
    };
}

world::CharacterFace face_of(std::string_view text,
                             const std::map<std::string, std::string_view>& files = {}) {
    auto face = world::parse_face(parse(text), reader(files));
    REQUIRE_MESSAGE(face.ok(), face.error().message);
    return std::move(face).value();
}

real_t dot(Vec3 a, Vec3 b) {
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

Vec3 cross(Vec3 a, Vec3 b) {
    return {(a.y * b.z) - (a.z * b.y), (a.z * b.x) - (a.x * b.z), (a.x * b.y) - (a.y * b.x)};
}

real_t length(Vec3 v) {
    return std::sqrt(dot(v, v));
}

const world::HeadShape kHead{.center = {0.0F, 1.6F, 0.0F}, .radii = {0.1F, 0.12F, 0.112F}};

Vec3 landmark(const world::SculptedHead& head, std::string_view name) {
    for (const world::FaceLandmark& mark : head.landmarks) {
        if (mark.name == name) {
            return mark.position;
        }
    }
    FAIL("no landmark " << name);
    return {};
}

std::size_t count_area(const world::SculptedHead& head, world::FaceArea area) {
    return static_cast<std::size_t>(std::ranges::count(head.areas, area));
}

} // namespace

TEST_CASE("face: files are checked, with the closest control's name for a misspelled one") {
    CHECK(
        world::validate_face(parse(R"({"format": 1, "controls": {"nose.length": 0.5}})")).empty());
    auto errors = world::validate_face(parse(R"({"format": 1, "controls": {"nose.lenght": 0.5}})"));
    REQUIRE_FALSE(errors.empty());
    CHECK(errors.front().path == "/controls/nose.lenght");
    CHECK(errors.front().message.find("nose.length") != std::string::npos);
    errors = world::validate_face(
        parse(R"({"format": 1, "carves": [{"kind": "dent", "at": "elbow"}]})"));
    REQUIRE_FALSE(errors.empty());
    CHECK(errors.front().path == "/carves/0/at");
    CHECK_FALSE(world::validate_face(parse(R"({"controls": {}})")).empty());
    CHECK(world::face_controls().size() == 46);
    CHECK(world::face_control_index("definition").has_value());
    CHECK(world::is_face_path("faces/mira.face.json"));
}

TEST_CASE("face: a face extends another, blends presets and is set inline in a character") {
    const std::map<std::string, std::string_view> files{
        {"base.face.json",
         R"({"format": 1, "eye_size": 1.2, "controls": {"nose.length": 0.4, "jaw.width": 0.2}})"},
        {"wide.face.json", R"({"format": 1, "controls": {"jaw.width": 1, "eyes.spacing": 0.6}})"},
        {"narrow.face.json", R"({"format": 1, "controls": {"jaw.width": -1}})"},
        {"loop.face.json", R"({"format": 1, "extends": "loop.face.json"})"},
        {"self.face.json", R"({"format": 1, "blend": [{"face": "self.face.json", "weight": 1}]})"}};
    const world::CharacterFace child = face_of(
        R"({"format": 1, "extends": "base.face.json", "controls": {"jaw.width": 0.5}})", files);
    CHECK(child.eye_size == doctest::Approx(1.2));
    CHECK(child.control("nose.length") == doctest::Approx(0.4));
    CHECK(child.control("jaw.width") == doctest::Approx(0.5));
    // Presets mix by weight under the face's own controls.
    const world::CharacterFace blended = face_of(R"({"format": 1, "controls": {"jaw.width": 0.1},
        "blend": [{"face": "wide.face.json", "weight": 0.75},
                  {"face": "narrow.face.json", "weight": 0.25}]})",
                                                 files);
    CHECK(blended.control("jaw.width") == doctest::Approx(0.1 + 0.75 - 0.25));
    CHECK(blended.control("eyes.spacing") == doctest::Approx(0.45));
    // Weights summing over 1 are divided by their sum.
    const world::CharacterFace heavy = face_of(R"({"format": 1,
        "blend": [{"face": "wide.face.json", "weight": 1}, {"face": "narrow.face.json", "weight": 1}]})",
                                               files);
    CHECK(heavy.control("jaw.width") == doctest::Approx(0.0));
    CHECK_FALSE(
        world::parse_face(parse(R"({"format": 1, "extends": "loop.face.json"})"), reader(files))
            .ok());
    CHECK_FALSE(
        world::parse_face(parse(R"({"format": 1, "extends": "self.face.json"})"), reader(files))
            .ok());
    CHECK_FALSE(
        world::parse_face(parse(R"({"format": 1, "extends": "none.face.json"})"), reader(files))
            .ok());
    // A character's face names a file with from and changes it.
    auto recipe = world::parse_character(
        parse(R"({"format": 1, "face": {"from": "base.face.json", "beard": "short",
                  "controls": {"chin.cleft": 1}}})"),
        reader(files));
    REQUIRE_MESSAGE(recipe.ok(), recipe.error().message);
    CHECK(recipe.value().face.control("nose.length") == doctest::Approx(0.4));
    CHECK(recipe.value().face.control("chin.cleft") == doctest::Approx(1.0));
    CHECK(recipe.value().face.beard == "short");
    CHECK(world::validate_character(parse(R"({"format": 1, "face": {"controls": {"x": 1}}})"))
              .front()
              .path == "/face/controls/x");
    auto missing = world::parse_character(
        parse(R"({"format": 1, "face": {"from": "gone.face.json"}})"), reader(files));
    REQUIRE_FALSE(missing.ok());
    CHECK(missing.error().message.starts_with("/face"));
}

TEST_CASE("face: a sculpted head is closed, faces out and has lips, brows, eyes and a mouth") {
    const world::CharacterFace face;
    const world::SculptedHead head = world::sculpt_head(face, kHead);
    CHECK(head.positions.size() == (head.rings + 1) * (head.segments + 1));
    CHECK(head.areas.size() * 3 == head.indices.size());
    CHECK(head.bearded.size() == head.areas.size());
    // Godot's front faces turn clockwise seen from outside, so (c - a) x (b - a) points out.
    std::size_t outward = 0;
    std::size_t counted = 0;
    for (std::size_t t = 0; t < head.areas.size(); ++t) {
        if (head.areas[t] != world::FaceArea::Skin) {
            continue;
        }
        const Vec3 a = head.positions[head.indices[t * 3]];
        const Vec3 b = head.positions[head.indices[(t * 3) + 1]];
        const Vec3 c = head.positions[head.indices[(t * 3) + 2]];
        const Vec3 n = cross(c - a, b - a);
        if (length(n) < 1e-12F) {
            continue;
        }
        ++counted;
        if (dot(n, ((a + b + c) * (1.0F / 3.0F)) - kHead.center) > 0.0F) {
            ++outward;
        }
    }
    CHECK(outward > counted * 99 / 100);
    CHECK(count_area(head, world::FaceArea::Lips) > 20);
    CHECK(count_area(head, world::FaceArea::Brows) > 20);
    CHECK(count_area(head, world::FaceArea::Mouth) > 4);
    CHECK(count_area(head, world::FaceArea::Hidden) > 4);
    // The lids wrap the eyeballs: no skin but what hides behind them is inside one.
    for (std::size_t v = 0; v < head.positions.size(); ++v) {
        for (const Vec3 eye : {head.eyes.left, head.eyes.right}) {
            const real_t inside = length(head.positions[v] - eye) / head.eyes.radius;
            if (inside < 0.99F) {
                CHECK(inside == doctest::Approx(0.8).epsilon(0.01));
            }
        }
    }
    // The nose stands out of the face, the eyes are set either side and the mouth below them.
    const Vec3 tip = landmark(head, "nose_tip");
    CHECK(tip.z > landmark(head, "cheek_left").z + 0.015F);
    CHECK(tip.z > landmark(head, "upper_lip").z);
    CHECK(head.eyes.left.x > 0.0F);
    CHECK(head.eyes.right.x == doctest::Approx(-head.eyes.left.x));
    CHECK(head.mouth < tip.y);
    CHECK(head.mouth < head.eyes.left.y - 0.04F);
    CHECK(landmark(head, "chin").y < head.mouth);
    CHECK(head.landmarks.size() == world::face_landmark_names().size());
    // The same face sculpts the same head, to the bit.
    const world::SculptedHead again = world::sculpt_head(face, kHead);
    CHECK(again.positions == head.positions);
    CHECK(again.normals == head.normals);
}

TEST_CASE("face: controls move what they name, and carves dent the skin") {
    const world::SculptedHead plain = world::sculpt_head(world::CharacterFace{}, kHead);
    const auto with = [](std::string_view text) {
        return world::sculpt_head(face_of(text), kHead);
    };
    const world::SculptedHead nose = with(R"({"format": 1, "controls": {"nose.length": 1}})");
    CHECK(landmark(nose, "nose_tip").y < landmark(plain, "nose_tip").y - 0.003F);
    const world::SculptedHead bridge = with(R"({"format": 1, "controls": {"nose.bridge": 1}})");
    CHECK(landmark(bridge, "nose_bridge").z > landmark(plain, "nose_bridge").z + 0.001F);
    const world::SculptedHead jaw = with(R"({"format": 1, "controls": {"jaw.width": 1}})");
    CHECK(landmark(jaw, "jaw_left").x > landmark(plain, "jaw_left").x + 0.003F);
    const world::SculptedHead eyes =
        with(R"({"format": 1, "controls": {"eyes.spacing": 1, "eyes.size": 1}})");
    CHECK(eyes.eyes.left.x > plain.eyes.left.x + 0.003F);
    CHECK(eyes.eyes.radius > plain.eyes.radius);
    const world::SculptedHead chin = with(R"({"format": 1, "controls": {"chin.protrusion": 1}})");
    CHECK(landmark(chin, "chin").z > landmark(plain, "chin").z);
    const world::SculptedHead long_chin = with(R"({"format": 1, "controls": {"chin.length": 1}})");
    CHECK(landmark(long_chin, "chin").y < landmark(plain, "chin").y);
    const world::SculptedHead mouth = with(R"({"format": 1, "controls": {"mouth.width": 1}})");
    CHECK(landmark(mouth, "mouth_corner_left").x > landmark(plain, "mouth_corner_left").x);
    CHECK(count_area(mouth, world::FaceArea::Lips) > count_area(plain, world::FaceArea::Lips));
    const world::SculptedHead dented = with(R"({"format": 1, "carves": [
        {"kind": "dent", "at": "cheek_left", "radius": 0.08, "depth": 1, "mirror": true}]})");
    const Vec3 cheek = landmark(plain, "cheek_left");
    CHECK(length(landmark(dented, "cheek_left") - kHead.center) <
          length(cheek - kHead.center) - 0.002F);
    CHECK(landmark(dented, "cheek_right").z < landmark(plain, "cheek_right").z);
    const world::SculptedHead bumped = with(R"({"format": 1, "carves": [
        {"kind": "ridge", "at": "forehead", "length": 0.3, "radius": 0.06, "depth": 1}]})");
    CHECK(landmark(bumped, "forehead").z > landmark(plain, "forehead").z + 0.002F);
    // Asymmetry moves the sides apart, as the seed picks.
    const world::SculptedHead uneven = with(R"({"format": 1, "asymmetry": 1, "seed": 7})");
    CHECK(uneven.eyes.left.y != doctest::Approx(uneven.eyes.right.y));
    const world::SculptedHead other = with(R"({"format": 1, "asymmetry": 1, "seed": 8})");
    CHECK(other.eyes.left.y != doctest::Approx(uneven.eyes.left.y));
}

TEST_CASE("face: styles sculpt from fine detail to a few flat faces") {
    const world::SculptedHead realistic = world::sculpt_head(world::CharacterFace{}, kHead);
    const world::SculptedHead toon =
        world::sculpt_head(face_of(R"({"format": 1, "style": "toon"})"), kHead);
    const world::SculptedHead low =
        world::sculpt_head(face_of(R"({"format": 1, "style": "low_poly"})"), kHead);
    CHECK(toon.eyes.radius > realistic.eyes.radius * 1.4F);
    CHECK(toon.indices.size() < realistic.indices.size());
    CHECK(landmark(toon, "nose_tip").z < landmark(realistic, "nose_tip").z);
    CHECK(low.flat);
    CHECK(low.indices.size() < 2000);
    // Flat: every triangle's corners are its own and face its way.
    CHECK(low.positions.size() == low.indices.size());
    CHECK(low.normals[0] == low.normals[1]);
    CHECK(low.normals[1] == low.normals[2]);
}

TEST_CASE("face: a character's head is its sculpt, its eyes on its eye bones") {
    const std::map<std::string, std::string_view> files{
        {"faces/rook.face.json",
         R"({"format": 1, "controls": {"eyes.spacing": 0.8, "nose.length": 0.6}})"}};
    auto recipe = world::parse_character(
        parse(R"({"format": 1, "face": {"from": "faces/rook.face.json", "beard": "short"}})"),
        reader(files));
    REQUIRE_MESSAGE(recipe.ok(), recipe.error().message);
    auto model = scene3d::build_character(recipe.value(), {});
    REQUIRE_MESSAGE(model.ok(), model.error().message);
    std::set<std::string> materials;
    for (const scene3d::ModelMaterial& material : model.value().materials) {
        materials.insert(material.name);
    }
    for (const std::string_view name : {"lips", "brows", "mouth", "hair", "eyes", "teeth"}) {
        CHECK_MESSAGE(materials.contains(std::string(name)), name);
    }
    const world::CharacterMeasures m = world::character_measures(recipe.value());
    const world::FaceEyes eyes = world::face_eyes(recipe.value().face, world::character_head(m));
    const auto rests = scene3d::node_globals(model.value(), scene3d::rest_pose(model.value()));
    for (std::size_t i = 0; i < model.value().nodes.size(); ++i) {
        if (model.value().nodes[i].name == "LeftEye") {
            CHECK(rests[i].origin.x == doctest::Approx(eyes.left.x).epsilon(0.001));
            CHECK(rests[i].origin.y == doctest::Approx(eyes.left.y).epsilon(0.001));
        }
    }
    // Its spacing widens the eyes from a plain face's.
    const world::FaceEyes plain =
        world::face_eyes(world::CharacterFace{}, world::character_head(m));
    CHECK(eyes.left.x > plain.left.x);
}

TEST_CASE("face: 2D faces are the sculpt's profile, the chin on the jaw") {
    const world::HeadShape head = kHead;
    const world::FaceProfile plain = world::face_profile(world::CharacterFace{}, head);
    REQUIRE(plain.points.size() > 60);
    REQUIRE(plain.mouth_index > 0);
    REQUIRE(plain.mouth_index < plain.front_count);
    // The nose is the face's furthest point forward.
    std::size_t front = 0;
    for (std::size_t i = 0; i < plain.front_count; ++i) {
        if (plain.points[i].x > plain.points[front].x) {
            front = i;
        }
    }
    CHECK(plain.points[front].y > plain.points[plain.mouth_index].y);
    CHECK(plain.points[front].y < plain.eye.y);
    const world::FaceProfile longer = world::face_profile(
        face_of(R"({"format": 1, "controls": {"nose.length": 1, "nose.tip": 1}})"), head);
    real_t reach = 0.0F;
    for (const Vec2 point : longer.points) {
        reach = std::max(reach, point.x);
    }
    CHECK(reach > plain.points[front].x);
    // The cut-out's head is that outline, the chin a part of the jaw that turns.
    auto recipe = world::parse_character(
        parse(R"({"format": 1, "face": {"controls": {"chin.protrusion": 1}}})"), nullptr);
    REQUIRE(recipe.ok());
    const std::vector<world::CutoutBone> bones = world::character_cutout(recipe.value());
    std::size_t head_points = 0;
    bool chin = false;
    for (const world::CutoutBone& bone : bones) {
        if (bone.name == "head" && !bone.shapes.empty()) {
            head_points = std::ranges::max_element(bone.shapes, {}, [](const auto& shape) {
                              return shape.points.size();
                          })->points.size();
        }
        if (bone.name == "jaw" && !bone.shapes.empty()) {
            chin = bone.shapes.front().points.size() > 10;
        }
    }
    CHECK(head_points > 40);
    CHECK(chin);
}
