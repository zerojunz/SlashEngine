// Hair and fur over a model's skin, as Unreal's Groom: grooms grown over a model's section,
// guide strands carried by its skin, and strands, cards and shells made into triangles.
#include "slash/scene3d/hair.hpp"

#include "slash/asset/assets.hpp"

#include <algorithm>
#include <cmath>
#include <map>

namespace slash::scene3d {

namespace {

real_t dot(Vec3 a, Vec3 b) {
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

real_t length(Vec3 v) {
    return std::sqrt(dot(v, v));
}

Vec3 cross(Vec3 a, Vec3 b) {
    return {(a.y * b.z) - (a.z * b.y), (a.z * b.x) - (a.x * b.z), (a.x * b.y) - (a.y * b.x)};
}

Vec3 normalized(Vec3 v, Vec3 otherwise) {
    const real_t size = length(v);
    return size > 0.0F ? v * (1.0F / size) : otherwise;
}

std::array<float, 3> to_floats(Vec3 v) {
    return {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)};
}

std::array<std::uint8_t, 4> to_bytes(Vec4 color) {
    const auto byte = [](real_t value) {
        return static_cast<std::uint8_t>(std::lround(std::clamp<real_t>(value, 0.0F, 1.0F) * 255));
    };
    return {byte(color.x), byte(color.y), byte(color.z), byte(color.w)};
}

Vec4 mix(Vec4 a, Vec4 b, float t) {
    return {a.x + ((b.x - a.x) * t), a.y + ((b.y - a.y) * t), a.z + ((b.z - a.z) * t),
            a.w + ((b.w - a.w) * t)};
}

Vec4 times(Vec4 a, Vec4 b) {
    return {a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w};
}

const std::string* model_path(const world::World& world, entt::entity entity) {
    const auto* instance = world.ecs().try_get<const world::MeshInstance3D>(entity);
    if (instance == nullptr || instance->mesh.empty() || !is_model_path(instance->mesh)) {
        return nullptr;
    }
    return &instance->mesh;
}

// A blend of transforms, weight by weight, as linear blend skinning blends joints.
world::Affine3D blend(std::span<const std::pair<std::uint32_t, float>> weights,
                      std::span<const world::Affine3D> moves) {
    world::Affine3D out{.x = {}, .y = {}, .z = {}, .origin = {}};
    real_t total = 0.0F;
    for (const auto& [joint, weight] : weights) {
        if (weight <= 0.0F || joint >= moves.size()) {
            continue;
        }
        const world::Affine3D& m = moves[joint];
        out.x = out.x + (m.x * weight);
        out.y = out.y + (m.y * weight);
        out.z = out.z + (m.z * weight);
        out.origin = out.origin + (m.origin * weight);
        total += weight;
    }
    return total > 0.0F ? out : world::Affine3D{};
}

// How each of the section's joints has moved from rest, in the model's space.
std::vector<world::Affine3D> joint_moves(const world::World& world, const world::Tree3D& tree,
                                         entt::entity entity, const ClothSection& section) {
    const std::vector<world::Affine3D> now = cloth_joints(world, tree, entity, section);
    std::vector<world::Affine3D> out(now.size());
    for (std::size_t j = 0; j < now.size() && j < section.rest.size(); ++j) {
        out[j] = now[j] * section.rest[j].inverse();
    }
    return out;
}

// A point in a triangle from two numbers in [0, 1), spread evenly over it.
Vec3 bary_of(float r1, float r2) {
    const float s = std::sqrt(r1);
    return {1.0F - s, s * (1.0F - r2), s * r2};
}

HairRoot root_at(const ClothSection& section, std::uint32_t triangle, Vec3 bary) {
    HairRoot root;
    root.triangle = triangle;
    root.bary = bary;
    const std::array<std::uint32_t, 3> corners{
        section.indices[triangle], section.indices[triangle + 1], section.indices[triangle + 2]};
    const std::array<real_t, 3> w{bary.x, bary.y, bary.z};
    Vec3 normal;
    std::map<std::uint32_t, float> joints;
    for (std::size_t i = 0; i < 3; ++i) {
        const std::uint32_t v = corners.at(i);
        root.position = root.position + (section.positions[v] * w.at(i));
        normal = normal + (section.normals[v] * w.at(i));
        root.uv = root.uv + (section.uvs[v] * w.at(i));
        for (const auto& [joint, weight] : section.weights[v]) {
            if (weight > 0.0F) {
                joints[joint] += weight * static_cast<float>(w.at(i));
            }
        }
    }
    const Vec3 face = cross(section.positions[corners[1]] - section.positions[corners[0]],
                            section.positions[corners[2]] - section.positions[corners[0]]);
    root.normal = normalized(normal, normalized(face, {0.0F, 1.0F, 0.0F}));
    // The four heaviest joints, adding up to 1.
    std::vector<std::pair<float, std::uint32_t>> order;
    order.reserve(joints.size());
    for (const auto& [joint, weight] : joints) {
        order.emplace_back(weight, joint);
    }
    std::ranges::stable_sort(order, [](const auto& a, const auto& b) { return a.first > b.first; });
    float total = 0.0F;
    for (std::size_t k = 0; k < order.size() && k < 4; ++k) {
        total += order[k].first;
    }
    for (std::size_t k = 0; k < order.size() && k < 4; ++k) {
        root.weights.at(k) = {order[k].second, total > 0.0F ? order[k].first / total : 0.0F};
    }
    return root;
}

struct Maps {
    world::HairImage length;
    world::HairImage density;
    world::HairImage color;
};

Result<Maps> read_maps(const world::World& world, const world::Groom& groom) {
    Maps maps;
    for (const auto& [path, out] :
         {std::pair{&groom.length_map, &maps.length}, std::pair{&groom.density_map, &maps.density},
          std::pair{&groom.color_map, &maps.color}}) {
        auto image = world::read_hair_image(world, *path);
        if (!image) {
            return image.error();
        }
        *out = std::move(image).value();
    }
    return maps;
}

struct Loaded {
    std::shared_ptr<const world::Groom> groom;
    std::uint64_t version = 0;
};

Result<Loaded> groom_of(const world::World& world, const world::HairInstance3D& hair) {
    if (hair.groom.empty()) {
        return make_error(ErrorCode::InvalidArgument,
                          "names no groom: set groom to a .groom.json file");
    }
    auto loaded = world::load_groom(world, hair.groom);
    if (!loaded) {
        return make_error(loaded.error().code,
                          "\"" + hair.groom + "\" can't be read: " + loaded.error().message);
    }
    if (loaded.value().groom->space != "3d") {
        return make_error(ErrorCode::InvalidArgument,
                          "\"" + hair.groom + "\" is a 2D groom; a HairInstance3D needs a 3D one");
    }
    return Loaded{.groom = loaded.value().groom, .version = loaded.value().version};
}

// The groomed points of a guide from its root, as 2D hair grows them: out along the normal,
// turning toward comb by the tip as lift lets it.
void groom_points(const world::Groom& groom, const HairRoot& root, real_t size,
                  std::vector<Vec3>& out) {
    const int segments = std::clamp(groom.segments, 1, 32);
    const real_t comb_size = length(groom.comb);
    Vec3 at = root.position;
    out.push_back(at);
    for (int k = 1; k <= segments; ++k) {
        const real_t turn = comb_size > 0.0F
                                ? (1.0F - std::clamp(groom.lift, 0.0F, 1.0F)) *
                                      static_cast<float>(k) / static_cast<float>(segments)
                                : 0.0F;
        const Vec3 way =
            normalized((root.normal * (1.0F - turn)) +
                           (comb_size > 0.0F ? groom.comb * (turn / comb_size) : Vec3{}),
                       root.normal);
        at = at + (way * (size / static_cast<float>(segments)));
        out.push_back(at);
    }
}

} // namespace

std::string hair_key(const world::World& world, entt::entity entity) {
    const auto* hair = world.ecs().try_get<const world::HairInstance3D>(entity);
    const std::string* path = model_path(world, entity);
    if (hair == nullptr || path == nullptr) {
        return {};
    }
    std::uint64_t model_version = 0;
    if (asset::Assets* assets = asset::assets_of(world.ecs()); assets != nullptr) {
        auto found = assets->load<Model>(*path);
        if (!found) {
            return {};
        }
        model_version = found.value().version();
    }
    auto groom = groom_of(world, *hair);
    if (!groom) {
        return {};
    }
    auto maps = read_maps(world, *groom.value().groom);
    if (!maps) {
        return {};
    }
    return *path + "@" + std::to_string(model_version) + "#" + hair->section + "|" + hair->groom +
           "@" + std::to_string(groom.value().version) + "|" +
           std::to_string(maps.value().length.version) + "," +
           std::to_string(maps.value().density.version) + "," +
           std::to_string(maps.value().color.version);
}

namespace {

Result<std::shared_ptr<const HairGrowth3D>> grow(const world::World& world, entt::entity entity) {
    const auto* hair = world.ecs().try_get<const world::HairInstance3D>(entity);
    if (hair == nullptr) {
        return make_error(ErrorCode::InvalidArgument, "has no HairInstance3D");
    }
    auto groom_loaded = groom_of(world, *hair);
    if (!groom_loaded) {
        return groom_loaded.error();
    }
    if (model_path(world, entity) == nullptr) {
        return make_error(ErrorCode::InvalidArgument,
                          "hair needs a MeshInstance3D drawing a model file to grow on");
    }
    auto section = model_section(world, entity, hair->section);
    if (!section) {
        return section.error();
    }
    const world::Groom& groom = *groom_loaded.value().groom;
    auto maps_read = read_maps(world, groom);
    if (!maps_read) {
        return maps_read.error();
    }
    const world::HairMap length_map = maps_read.value().length.map();
    const world::HairMap density_map = maps_read.value().density.map();
    const world::HairMap color_map = maps_read.value().color.map();

    auto growth = std::make_shared<HairGrowth3D>();
    growth->section = section.value();
    growth->groom = groom_loaded.value().groom;
    growth->key = hair_key(world, entity);
    growth->points = std::clamp(groom.segments, 1, 32) + 1;
    const ClothSection& s = *growth->section;
    // Each triangle's area at rest, added up, to pick triangles by area.
    std::vector<real_t> running;
    for (std::size_t t = 0; t + 2 < s.indices.size(); t += 3) {
        const Vec3 a = s.positions[s.indices[t]];
        const Vec3 b = s.positions[s.indices[t + 1]];
        const Vec3 c = s.positions[s.indices[t + 2]];
        growth->area += length(cross(b - a, c - a)) * 0.5F;
        running.push_back(growth->area);
    }
    if (growth->area <= 0.0F) {
        return make_error(ErrorCode::InvalidArgument, "the surface it grows on has no area");
    }
    const auto triangle_at = [&](float share) {
        const real_t target = share * growth->area;
        const auto found = std::ranges::upper_bound(running, target);
        const auto index = static_cast<std::size_t>(std::min<std::ptrdiff_t>(
            found - running.begin(), static_cast<std::ptrdiff_t>(running.size()) - 1));
        return static_cast<std::uint32_t>(index * 3);
    };
    // Guides, spread over the surface by area, each where hair grows (red of the length map).
    const int guides = std::clamp(groom.guides, 1, 1024);
    std::vector<float> guide_sizes;
    for (int g = 0; g < guides; ++g) {
        HairRoot root;
        float size = 1.0F;
        for (std::uint32_t attempt = 0; attempt < 8; ++attempt) {
            const std::uint32_t hash = world::hair_hash(
                groom.seed ^ world::hair_hash((static_cast<std::uint32_t>(g) * 8U) + attempt));
            const float share =
                (static_cast<float>(g) + world::hair_unit(hash)) / static_cast<float>(guides);
            root = root_at(s, triangle_at(share),
                           bary_of(world::hair_unit(world::hair_hash(hash + 1U)),
                                   world::hair_unit(world::hair_hash(hash + 2U))));
            size = length_map.at(static_cast<float>(root.uv.x), static_cast<float>(root.uv.y)).x;
            if (size > 0.0F) {
                break;
            }
        }
        groom_points(groom, root, groom.length * size, growth->rest);
        growth->guides.push_back(root);
        guide_sizes.push_back(size);
    }
    const auto nearest_guide = [&](Vec3 at) {
        std::uint32_t best = 0;
        real_t best_d = 0.0F;
        for (std::size_t g = 0; g < growth->guides.size(); ++g) {
            const Vec3 d = growth->guides[g].position - at;
            const real_t d2 = dot(d, d);
            if (g == 0 || d2 < best_d) {
                best = static_cast<std::uint32_t>(g);
                best_d = d2;
            }
        }
        return best;
    };
    // The strands drawn.
    const auto wanted = static_cast<std::size_t>(std::clamp<real_t>(
        std::round(groom.density * growth->area), 0.0F, static_cast<float>(groom.max_strands)));
    for (std::size_t i = 0; i < wanted; ++i) {
        const std::uint32_t hash = world::hair_hash(
            (groom.seed * 2654435761U) ^ world::hair_hash(static_cast<std::uint32_t>(i) + 977U));
        const float share =
            (static_cast<float>(i) + world::hair_unit(hash)) / static_cast<float>(wanted);
        const HairRoot root = root_at(s, triangle_at(share),
                                      bary_of(world::hair_unit(world::hair_hash(hash + 1U)),
                                              world::hair_unit(world::hair_hash(hash + 2U))));
        const auto u = static_cast<float>(root.uv.x);
        const auto v = static_cast<float>(root.uv.y);
        if (!density_map.empty() &&
            world::hair_unit(world::hair_hash(hash + 3U)) >= density_map.at(u, v).x) {
            continue;
        }
        const float own = length_map.at(u, v).x;
        if (own <= 0.0F) {
            continue;
        }
        HairGrowth3D::Strand strand;
        strand.guide = nearest_guide(root.position);
        const float guide_size = guide_sizes[strand.guide];
        strand.offset = root.position - growth->guides[strand.guide].position;
        strand.length = (guide_size > 0.0F ? own / guide_size : 0.0F) *
                        (1.0F - (std::clamp(groom.length_variation, 0.0F, 1.0F) *
                                 world::hair_unit(world::hair_hash(hash + 4U))));
        strand.phase = 6.2831853F * world::hair_unit(world::hair_hash(hash + 5U));
        strand.clump = std::clamp(groom.clumping, 0.0F, 1.0F) *
                       (0.5F + (0.5F * world::hair_unit(world::hair_hash(hash + 6U))));
        strand.color = color_map.at(u, v);
        growth->strands.push_back(strand);
    }
    for (std::size_t i = 0; i < s.positions.size(); ++i) {
        growth->vertex_guides.push_back(nearest_guide(s.positions[i]));
        growth->vertex_lengths.push_back(
            groom.length *
            length_map.at(static_cast<float>(s.uvs[i].x), static_cast<float>(s.uvs[i].y)).x);
    }
    return std::shared_ptr<const HairGrowth3D>(std::move(growth));
}

} // namespace

Result<std::shared_ptr<const HairGrowth3D>> hair_growth(const world::World& world,
                                                        entt::entity entity) {
    std::string key = hair_key(world, entity);
    if (auto kept = world::kept_hair_3d(world, key); kept != nullptr) {
        return std::static_pointer_cast<const HairGrowth3D>(kept);
    }
    auto grown = grow(world, entity);
    if (!grown) {
        return grown.error();
    }
    world::keep_hair_3d(world, std::move(key), grown.value());
    return grown;
}

std::vector<world::Affine3D> hair_frames(const world::World& world, const world::Tree3D& tree,
                                         entt::entity entity, const HairGrowth3D& growth) {
    const std::vector<world::Affine3D> moves = joint_moves(world, tree, entity, *growth.section);
    const world::Affine3D global = tree.global(entity);
    std::vector<world::Affine3D> out;
    out.reserve(growth.guides.size());
    for (const HairRoot& root : growth.guides) {
        out.push_back(global * blend(root.weights, moves));
    }
    return out;
}

std::vector<Vec3> hair_targets(const HairGrowth3D& growth,
                               std::span<const world::Affine3D> frames) {
    const auto n = static_cast<std::size_t>(growth.points);
    std::vector<Vec3> out;
    out.reserve(growth.rest.size());
    for (std::size_t g = 0; g < growth.guides.size() && g < frames.size(); ++g) {
        for (std::size_t k = 0; k < n; ++k) {
            out.push_back(frames[g].apply(growth.rest[(g * n) + k]));
        }
    }
    return out;
}

void hair_ribbons(const HairGrowth3D& growth, std::span<const Vec3> guides,
                  std::span<const world::Affine3D> frames, Vec3 eye,
                  std::vector<render::Vertex3D>& vertices, std::vector<std::uint32_t>& indices) {
    vertices.clear();
    indices.clear();
    const world::Groom& groom = *growth.groom;
    const auto n = static_cast<std::size_t>(growth.points);
    if (guides.size() < growth.guides.size() * n || frames.size() < growth.guides.size()) {
        return;
    }
    const bool cards = groom.render == "cards";
    std::vector<Vec3> points;
    // A ribbon along points, width across at each, facing toward face (or eye when face is null).
    const auto ribbon = [&](std::span<const Vec3> along, const Vec3* face, Vec4 tint) {
        const auto base = static_cast<std::uint32_t>(vertices.size());
        const std::size_t count = along.size();
        for (std::size_t k = 0; k < count; ++k) {
            const float t =
                count > 1 ? static_cast<float>(k) / static_cast<float>(count - 1) : 0.0F;
            const Vec3 way = normalized(
                along[std::min(k + 1, count - 1)] - along[k == 0 ? 0 : k - 1], {0.0F, 1.0F, 0.0F});
            const Vec3 toward = face != nullptr ? *face : normalized(eye - along[k], way);
            Vec3 across = normalized(cross(way, toward), {});
            if (length(across) <= 0.0F) {
                across = normalized(cross(way, {0.0F, 0.0F, 1.0F}), {1.0F, 0.0F, 0.0F});
            }
            const real_t width =
                cards ? groom.card_width : groom.width + ((groom.tip_width - groom.width) * t);
            // Facing out: across the ribbon, then back along it.
            Vec3 normal = normalized(cross(across, way), toward);
            if (dot(normal, toward) < 0.0F) {
                normal = normal * -1.0F;
            }
            const std::array<std::uint8_t, 4> rgba =
                to_bytes(times(mix(groom.root_color, groom.tip_color, t), tint));
            for (const float side : {-0.5F, 0.5F}) {
                render::Vertex3D vertex;
                vertex.position = to_floats(along[k] + (across * (width * side)));
                vertex.normal = to_floats(normal);
                const std::array<float, 3> tangent = to_floats(way);
                vertex.tangent = {tangent[0], tangent[1], tangent[2], 1.0F};
                vertex.uv = {side + 0.5F, t};
                vertex.rgba = rgba;
                vertices.push_back(vertex);
            }
        }
        for (std::size_t k = 0; k + 1 < count; ++k) {
            const auto a = base + static_cast<std::uint32_t>(k * 2);
            indices.insert(indices.end(), {a, a + 2, a + 1, a + 1, a + 2, a + 3});
        }
    };
    if (cards) {
        for (std::size_t g = 0; g < growth.guides.size(); ++g) {
            const Vec3 face =
                normalized(frames[g].basis(growth.guides[g].normal), {0.0F, 1.0F, 0.0F});
            ribbon(guides.subspan(g * n, n), &face, {1.0F, 1.0F, 1.0F, 1.0F});
        }
        return;
    }
    for (const HairGrowth3D::Strand& strand : growth.strands) {
        const std::size_t g = strand.guide;
        const world::Affine3D& frame = frames[g];
        world::follow_guide(guides.subspan(g * n, n), frame.basis(strand.offset), strand.length,
                            strand.clump, groom.curl_radius, groom.curl_turns, strand.phase,
                            frame.x, false, points);
        ribbon(points, nullptr, strand.color);
    }
}

void hair_skin(const world::World& world, const world::Tree3D& tree, entt::entity entity,
               const HairGrowth3D& growth, std::vector<Vec3>& positions,
               std::vector<Vec3>& normals) {
    const ClothSection& section = *growth.section;
    const std::vector<world::Affine3D> moves = joint_moves(world, tree, entity, section);
    const world::Affine3D global = tree.global(entity);
    positions.clear();
    normals.clear();
    for (std::size_t v = 0; v < section.positions.size(); ++v) {
        const world::Affine3D place = global * blend(section.weights[v], moves);
        positions.push_back(place.apply(section.positions[v]));
        normals.push_back(normalized(place.basis(section.normals[v]), {0.0F, 1.0F, 0.0F}));
    }
}

void hair_shell(const HairGrowth3D& growth, std::span<const Vec3> positions,
                std::span<const Vec3> normals, std::span<const Vec3> guides,
                std::span<const Vec3> targets, int layer, std::vector<render::Vertex3D>& out) {
    out.clear();
    const world::Groom& groom = *growth.groom;
    const ClothSection& section = *growth.section;
    const int count = std::clamp(groom.shell_count, 1, 64);
    const float h = static_cast<float>(layer + 1) / static_cast<float>(count);
    const auto n = static_cast<std::size_t>(growth.points);
    const std::array<std::uint8_t, 4> rgba = to_bytes(mix(groom.root_color, groom.tip_color, h));
    for (std::size_t v = 0; v < positions.size() && v < normals.size(); ++v) {
        const std::size_t tip = (static_cast<std::size_t>(growth.vertex_guides[v]) * n) + n - 1;
        const Vec3 moved =
            tip < guides.size() && tip < targets.size() ? guides[tip] - targets[tip] : Vec3{};
        render::Vertex3D vertex;
        vertex.position = to_floats(positions[v] + (normals[v] * (growth.vertex_lengths[v] * h)) +
                                    (moved * (h * h)));
        vertex.normal = to_floats(normals[v]);
        vertex.uv = {static_cast<float>(section.uvs[v].x) * groom.shell_tiling,
                     static_cast<float>(section.uvs[v].y) * groom.shell_tiling};
        vertex.rgba = rgba;
        out.push_back(vertex);
    }
}

render::Image hair_shell_image() {
    constexpr std::uint32_t kSize = 128;
    render::Image image;
    image.width = kSize;
    image.height = kSize;
    image.pixels.assign(static_cast<std::size_t>(kSize) * kSize * 4, 255);
    for (std::uint32_t y = 0; y < kSize; ++y) {
        for (std::uint32_t x = 0; x < kSize; ++x) {
            const std::uint32_t hash = world::hair_hash((y * kSize) + x + 0x51ED27U);
            const float u = world::hair_unit(hash);
            const std::size_t at = ((static_cast<std::size_t>(y) * kSize) + x) * 4;
            image.pixels[at + 3] =
                u < 0.55F
                    ? static_cast<std::uint8_t>(40 + (world::hair_hash(hash) % 216U)) // its height
                    : 0;
        }
    }
    return image;
}

std::vector<Problem> check_hair(const world::World& world) {
    std::vector<Problem> problems;
    for (const auto [entity, hair] : world.ecs().view<const world::HairInstance3D>().each()) {
        if (auto groom = groom_of(world, hair); !groom) {
            problems.push_back({entity, "/components/HairInstance3D/groom", groom.error().message});
            continue;
        }
        if (model_path(world, entity) == nullptr) {
            problems.push_back({entity, "/components/HairInstance3D",
                                "hair needs a MeshInstance3D drawing a model file to grow on; "
                                "it draws nothing until it has one"});
            continue;
        }
        if (auto growth = hair_growth(world, entity); !growth) {
            problems.push_back(
                {entity, "/components/HairInstance3D/section", growth.error().message});
        }
    }
    return problems;
}

} // namespace slash::scene3d
