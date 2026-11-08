#include "slash/scene3d/splats.hpp"

#include "slash/asset/assets.hpp"
#include "slash/world/components.hpp"
#include "slash/world/tree3d.hpp"
#include "slash/world/world.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace slash::scene3d {
namespace {

// The entities with a GaussianSplat3D, in the order they were made.
std::vector<entt::entity> made_order(const world::World& world) {
    std::vector<entt::entity> out;
    for (const entt::entity entity : world.ecs().view<const world::GaussianSplat3D>()) {
        out.push_back(entity);
    }
    std::ranges::sort(out, {}, [](entt::entity entity) { return entt::to_entity(entity); });
    return out;
}

constexpr std::string_view kField = "/components/GaussianSplat3D/splats";

} // namespace

void add_splat_type(asset::Assets& assets) {
    assets.add_type<render::GaussianSplats>(
        "gaussian_splats", {std::string(kSplatSuffix)},
        [](std::string_view bytes, std::string_view /*path*/) {
            return render::read_splat_ply(bytes);
        },
        [](const render::GaussianSplats& splats) {
            return sizeof(render::GaussianSplats) +
                   (splats.positions.capacity() * sizeof(splats.positions[0])) +
                   (splats.scales.capacity() * sizeof(splats.scales[0])) +
                   (splats.rotations.capacity() * sizeof(splats.rotations[0])) +
                   (splats.opacities.capacity() * sizeof(float)) +
                   (splats.sh.capacity() * sizeof(float));
        });
}

std::vector<Problem> check_splats(const world::World& world) {
    std::vector<Problem> problems;
    for (const entt::entity entity : made_order(world)) {
        const auto& cloud = world.ecs().get<const world::GaussianSplat3D>(entity);
        if (cloud.sh_degree < 0 || cloud.sh_degree > 3) {
            problems.push_back({entity, "/components/GaussianSplat3D/sh_degree",
                                "is " + std::to_string(cloud.sh_degree) + ", not 0 to 3"});
        }
        if (cloud.splats.empty()) {
            continue;
        }
        if (!cloud.splats.ends_with(kSplatSuffix)) {
            problems.push_back({entity, std::string(kField),
                                "\"" + cloud.splats + "\" is no .ply file of Gaussian splats"});
            continue;
        }
        const world::FileReader& reader = world.file_reader();
        if (!reader) {
            continue;
        }
        auto bytes = reader(cloud.splats);
        if (!bytes) {
            problems.push_back(
                {entity, std::string(kField),
                 "\"" + cloud.splats + "\" could not be read: " + bytes.error().message});
            continue;
        }
        if (auto splats = render::read_splat_ply(bytes.value()); !splats) {
            problems.push_back({entity, std::string(kField),
                                "\"" + cloud.splats + "\": " + splats.error().message});
        }
    }
    return problems;
}

void Renderer::add_splats(const world::World& world, const world::Tree3D& tree,
                          const std::function<world::Affine3D(entt::entity)>& global,
                          std::uint32_t cull_mask, render::Splats3D& out) {
    for (const entt::entity entity : made_order(world)) {
        const auto& cloud = world.ecs().get<const world::GaussianSplat3D>(entity);
        if (cloud.splats.empty() || !tree.visible(entity) || (cloud.layers & cull_mask) == 0 ||
            cloud.opacity <= 0.0F) {
            continue;
        }
        Result<std::shared_ptr<const render::GaussianSplats>> splats =
            make_error(ErrorCode::NotFound, "this renderer reads no Gaussian splats");
        if (splats_) {
            splats = splats_(cloud.splats);
        }
        if (!splats) {
            problems_.push_back({entity, std::string(kField), splats.error().message});
            continue;
        }
        out.clouds.push_back(
            {.splats = std::move(splats).value(),
             .transform = global(entity).matrix(),
             .sh_degree = static_cast<std::uint32_t>(std::clamp(cloud.sh_degree, 0, 3)),
             .opacity = std::min(cloud.opacity, 1.0F)});
    }
}

} // namespace slash::scene3d
