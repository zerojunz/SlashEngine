#include "slash/scene3d/vehicle.hpp"

#include "model_internal.hpp"
#include "rig_builder.hpp"

#include "slash/core/json.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slash::scene3d {

using world::VehicleRecipe;
using namespace rig; // NOLINT(google-build-using-namespace)

namespace {

// A box's eight corners and its twelve triangles, as a convex shape.
ModelCollision box_collision(Vec3 low, Vec3 high) {
    ModelCollision out;
    out.node = "Body";
    out.node_index = 0;
    out.convex = true;
    for (int i = 0; i < 8; ++i) {
        out.points.push_back({(i & 1) != 0 ? high.x : low.x, (i & 2) != 0 ? high.y : low.y,
                              (i & 4) != 0 ? high.z : low.z});
    }
    constexpr std::array<std::uint32_t, 36> kFaces{0, 1, 3, 0, 3, 2, 4, 6, 7, 4, 7, 5,
                                                   0, 4, 5, 0, 5, 1, 2, 3, 7, 2, 7, 6,
                                                   0, 2, 6, 0, 6, 4, 1, 5, 7, 1, 7, 3};
    out.indices.assign(kFaces.begin(), kFaces.end());
    return out;
}

MeshData unit_box() {
    world::BoxMesh box;
    box.size = {1.0F, 1.0F, 1.0F};
    return box_mesh(box);
}

// A part's shape a meter across, centered on its origin, as its side view's outline is.
MeshData unit_shape(const std::string& shape) {
    if (shape == "cylinder") {
        world::CylinderMesh cylinder;
        cylinder.height = 1.0F;
        cylinder.radial_segments = 24;
        cylinder.rings = 1;
        return cylinder_mesh(cylinder);
    }
    if (shape == "sphere") {
        world::SphereMesh sphere;
        sphere.radial_segments = 24;
        sphere.rings = 12;
        return sphere_mesh(sphere);
    }
    if (shape == "prism") {
        return prism_mesh(world::PrismMesh{});
    }
    return unit_box();
}

// Where a part is in the vehicle's space: turned by its rotation, scaled by its size.
world::Affine3D part_place(const world::VehiclePart& part) {
    world::Transform3D at;
    at.position = part.position;
    at.rotation = part.rotation;
    at.scale = part.size;
    return world::affine(at);
}

bool body_material(std::string_view name) {
    return name == "paint" || name == "trim" || name == "glass";
}

} // namespace

Model build_vehicle(const VehicleRecipe& recipe, const Model* body,
                    std::span<const Model* const> part_models) {
    const world::VehicleBody& b = recipe.body;
    Builder out;
    const MeshData cube = unit_box();
    const int paint = out.material("paint", b.color, 0.35F, 0.3F);
    const int trim = out.material("trim", b.trim, 0.8F, 0.0F);
    const int glass = out.material("glass", {0.55F, 0.7F, 0.8F, 1.0F}, 0.05F, 0.0F);
    const real_t half = b.length * 0.5F;
    const real_t low = b.clearance;
    const real_t waist = low + ((b.height - low) * (b.cabin > 0.0F ? 0.55F : 1.0F));
    // A box from low to high corner, drawn with material.
    const auto box = [&](Vec3 from, Vec3 to, int material) {
        out.add(cube, placed(to - from, (from + to) * 0.5F), material, 0);
    };
    const std::string& kind = recipe.kind;
    if (body == nullptr && b.draw) {
        if (kind == "bike") {
            const real_t w = b.width * 0.3F;
            box({-w * 0.5F, low + 0.15F, -half * 0.6F}, {w * 0.5F, b.height * 0.55F, half * 0.6F},
                paint);
            box({-w * 0.6F, b.height * 0.55F, -half * 0.35F},
                {w * 0.6F, b.height * 0.62F, half * 0.05F}, trim); // the saddle
            box({-b.width * 0.5F, b.height * 0.78F, half * 0.5F},
                {b.width * 0.5F, b.height * 0.82F, half * 0.58F}, trim); // the handlebar
        } else if (kind == "boat") {
            box({-b.width * 0.5F, 0.0F, -half}, {b.width * 0.5F, waist, half * 0.6F}, paint);
            // The bow, narrower toward its point.
            box({-b.width * 0.35F, waist * 0.2F, half * 0.6F}, {b.width * 0.35F, waist, half},
                paint);
        } else {
            box({-b.width * 0.5F, low, -half}, {b.width * 0.5F, waist, half}, paint);
        }
        if (b.cabin > 0.0F && kind != "bike") {
            const real_t cabin = b.length * b.cabin;
            real_t back = -cabin * 0.6F;
            if (kind == "truck") {
                back = half - cabin;
            } else if (kind == "aircraft") {
                back = half * 0.2F;
            }
            const real_t narrow = kind == "tracked" ? 0.7F : 0.9F;
            box({-b.width * 0.5F * narrow, waist, back},
                {b.width * 0.5F * narrow, b.height, back + cabin},
                kind == "tracked" ? paint : glass);
            if (kind == "tracked") {
                // The gun, ahead of the turret.
                box({-0.08F, waist + ((b.height - waist) * 0.45F), back + cabin},
                    {0.08F, waist + ((b.height - waist) * 0.6F), half + (b.length * 0.25F)}, trim);
            }
        }
        if (kind == "aircraft") {
            const real_t chord = recipe.wing_chord;
            box({-recipe.wing_span * 0.5F, waist - 0.12F, -chord * 0.5F},
                {recipe.wing_span * 0.5F, waist, chord * 0.5F}, paint);
            box({-b.width * 1.2F, waist - 0.08F, -half}, {b.width * 1.2F, waist, -half + chord},
                paint); // the tailplane
            box({-0.05F, waist, -half},
                {0.05F, b.height + ((b.height - low) * 0.4F), -half + (chord * 0.8F)},
                paint); // the fin
        }
    }
    if (kind == "tracked") {
        const real_t r = recipe.wheels.empty() ? 0.4F : recipe.wheels.front().radius;
        const real_t track = 0.55F;
        for (const real_t side : {1.0F, -1.0F}) {
            const real_t x = side * ((b.width * 0.5F) - (track * 0.5F));
            box({x - (track * 0.5F), 0.0F, -half + 0.1F},
                {x + (track * 0.5F), r * 2.0F, half - 0.1F}, trim);
        }
    }
    // The recipe's own parts, drawn in the body's materials or colors of their own.
    for (const world::VehiclePart& part : recipe.parts) {
        if (!part.model.empty()) {
            continue;
        }
        int material = paint;
        if (const std::optional<Vec4>& own = part.color; own.has_value()) {
            material = out.material(body_material(part.material) ? part.name : part.material,
                                    own.value(), 0.5F, 0.0F);
        } else if (part.material == "trim") {
            material = trim;
        } else if (part.material == "glass") {
            material = glass;
        }
        out.add(unit_shape(part.shape), part_place(part), material, 0);
    }
    // The body's mesh: one surface per material, unskinned.
    for (ModelSurface& surface : out.body.surfaces) {
        surface.weights.clear();
    }
    ModelNode root;
    root.name = "Body";
    root.file_index = 0;
    out.model.nodes.push_back(std::move(root));
    out.body.name = recipe.name.empty() ? std::string("Body") : recipe.name;
    if (!out.body.surfaces.empty()) {
        out.model.parts.push_back(
            {.node = "Body", .mesh = 0, .transform = {}, .node_index = 0, .skin = -1});
        out.model.meshes.push_back(std::move(out.body));
    }
    NameSet taken = node_names();
    taken.insert("Body");
    if (body != nullptr) {
        add_model(out.model, *body, 0, {}, taken);
    }
    // Parts that are model files, each placed under the body where the part is.
    for (std::size_t i = 0; i < recipe.parts.size() && i < part_models.size(); ++i) {
        if (part_models[i] != nullptr) {
            add_model(out.model, *part_models[i], 0, part_place(recipe.parts[i]), taken);
        }
    }
    // Seats, and beside each seat with a door where one stands to get in.
    const auto point = [&](const std::string& name, Vec3 at) {
        ModelNode node;
        node.name = unique_name(taken, name);
        node.parent = 0;
        node.position = at;
        node.file_index = static_cast<int>(out.model.nodes.size());
        out.model.nodes.push_back(std::move(node));
    };
    for (const world::VehicleSeat& seat : recipe.seats) {
        point("seat_" + seat.name, seat.position);
    }
    for (const world::VehicleSeat& seat : recipe.seats) {
        if (seat.door) {
            const real_t side = seat.position.x < 0.0F ? -1.0F : 1.0F;
            point("door_" + seat.name, {side * ((b.width * 0.5F) + 0.6F), 0.0F, seat.position.z});
        }
    }
    // What it collides as: its body's box, and an aircraft's wings. A model the body names
    // brings its own.
    if (body == nullptr || body->collision.empty()) {
        const real_t bottom = kind == "boat" ? 0.0F : low;
        out.model.collision.push_back(
            box_collision({-b.width * 0.5F, bottom, -half}, {b.width * 0.5F, b.height, half}));
        if (kind == "aircraft") {
            const real_t chord = recipe.wing_chord;
            const real_t waist_low = waist - 0.12F;
            out.model.collision.push_back(
                box_collision({-recipe.wing_span * 0.5F, waist_low, -chord * 0.5F},
                              {recipe.wing_span * 0.5F, waist, chord * 0.5F}));
        }
    }
    // And each part that collides, as its turned box.
    for (const world::VehiclePart& part : recipe.parts) {
        if (part.collide) {
            ModelCollision shape = box_collision({-0.5F, -0.5F, -0.5F}, {0.5F, 0.5F, 0.5F});
            const world::Affine3D at = part_place(part);
            for (Vec3& p : shape.points) {
                p = at.apply(p);
            }
            out.model.collision.push_back(std::move(shape));
        }
    }
    return std::move(out.model);
}

Result<Model> load_vehicle_model(std::string_view bytes, const ModelReader& read) {
    auto doc = json::parse(bytes);
    if (!doc) {
        return doc.error();
    }
    auto recipe = world::parse_vehicle(doc.value(), read);
    if (!recipe) {
        return recipe.error();
    }
    // A model file the recipe names: its body's or a part's.
    const auto load = [&](const std::string& file, const std::string& field) -> Result<Model> {
        auto text = read(file);
        if (!text) {
            return make_error(ErrorCode::NotFound,
                              field + " " + quoted(file) + " didn't load: " + text.error().message);
        }
        auto model = load_model(text.value(), file, read);
        if (!model) {
            return make_error(ErrorCode::InvalidArgument,
                              field + " " + quoted(file) +
                                  " didn't load: " + model.error().message);
        }
        return model;
    };
    std::vector<Model> parts;
    std::vector<const Model*> part_models(recipe.value().parts.size(), nullptr);
    parts.reserve(recipe.value().parts.size());
    for (std::size_t i = 0; i < recipe.value().parts.size(); ++i) {
        const world::VehiclePart& part = recipe.value().parts[i];
        if (part.model.empty()) {
            continue;
        }
        auto model = load(part.model, "parts/" + std::to_string(i) + "/model");
        if (!model) {
            return model.error();
        }
        parts.push_back(std::move(model.value()));
        part_models[i] = &parts.back();
    }
    const std::string& file = recipe.value().body.model;
    if (file.empty()) {
        return build_vehicle(recipe.value(), nullptr, part_models);
    }
    auto body = load(file, "body.model");
    if (!body) {
        return body.error();
    }
    return build_vehicle(recipe.value(), &body.value(), part_models);
}

std::vector<json::ValidationError> check_vehicle_model(std::string_view bytes,
                                                       const ModelReader& read) {
    auto doc = json::parse(bytes);
    if (!doc) {
        return {{"", doc.error().message}};
    }
    if (auto errors = world::validate_vehicle(doc.value()); !errors.empty()) {
        return errors;
    }
    auto resolved = world::resolve_vehicle(doc.value(), read);
    if (!resolved) {
        return {{"/extends", resolved.error().message}};
    }
    auto errors = world::validate_vehicle(resolved.value());
    if (const json::Value* body = resolved.value().find("body"); body != nullptr) {
        if (const json::Value* model = body->find("model");
            model != nullptr && model->is_string()) {
            if (auto text = read(model->as_string()); !text) {
                errors.push_back({"/body/model", quoted(model->as_string()) +
                                                     " didn't load: " + text.error().message});
            }
        }
    }
    if (const json::Value* parts = resolved.value().find("parts");
        parts != nullptr && parts->is_array()) {
        for (std::size_t i = 0; i < parts->as_array().size(); ++i) {
            const json::Value* model = parts->as_array()[i].find("model");
            if (model == nullptr || !model->is_string()) {
                continue;
            }
            if (auto text = read(model->as_string()); !text) {
                errors.push_back(
                    {"/parts/" + std::to_string(i) + "/model",
                     quoted(model->as_string()) + " didn't load: " + text.error().message});
            }
        }
    }
    return errors;
}

} // namespace slash::scene3d
