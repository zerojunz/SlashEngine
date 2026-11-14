#include "slash/world/cloth2d.hpp"

#include <algorithm>
#include <utility>

namespace slash::world {

namespace {

struct Cloth2DSourceHolder {
    Cloth2DSource source;
};

} // namespace

int cloth_columns(const Cloth2D& cloth) {
    return std::clamp(cloth.columns, 2, 64);
}

int cloth_rows(const Cloth2D& cloth) {
    return std::clamp(cloth.rows, 2, 64);
}

std::vector<Vec2> cloth_rest_2d(const Cloth2D& cloth, const Affine2D& global) {
    const int columns = cloth_columns(cloth);
    const int rows = cloth_rows(cloth);
    std::vector<Vec2> out;
    out.reserve(static_cast<std::size_t>(columns) * static_cast<std::size_t>(rows));
    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < columns; ++column) {
            const Vec2 local{
                cloth.offset.x +
                    (cloth.size.x *
                     ((static_cast<float>(column) / static_cast<float>(columns - 1)) - 0.5F)),
                cloth.offset.y +
                    (cloth.size.y * static_cast<float>(row) / static_cast<float>(rows - 1))};
            out.push_back(global.apply(local));
        }
    }
    return out;
}

std::vector<float> cloth_shares_2d(const Cloth2D& cloth) {
    const int columns = cloth_columns(cloth);
    const int rows = cloth_rows(cloth);
    std::vector<float> out(static_cast<std::size_t>(columns) * static_cast<std::size_t>(rows),
                           1.0F);
    if (cloth.max_distance_map != "gradient") {
        return out;
    }
    // Along gradient across the sheet at rest, in its own space.
    const std::vector<Vec2> rest = cloth_rest_2d(cloth, Affine2D{});
    std::vector<real_t> along(rest.size());
    real_t low = 0.0F;
    real_t high = 0.0F;
    for (std::size_t i = 0; i < rest.size(); ++i) {
        along[i] = (rest[i].x * cloth.gradient.x) + (rest[i].y * cloth.gradient.y);
        low = i == 0 ? along[i] : std::min(low, along[i]);
        high = i == 0 ? along[i] : std::max(high, along[i]);
    }
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = high > low ? static_cast<float>((along[i] - low) / (high - low)) : 1.0F;
    }
    return out;
}

ClothGrid2D cloth_grid_2d(const Cloth2D& cloth) {
    ClothGrid2D grid;
    grid.columns = cloth_columns(cloth);
    grid.rows = cloth_rows(cloth);
    const auto count = static_cast<std::size_t>(grid.columns) * static_cast<std::size_t>(grid.rows);
    grid.across.assign(count, -1);
    grid.down.assign(count, -1);
    for (int r = 0; r < grid.rows; ++r) {
        for (int c = 0; c < grid.columns; ++c) {
            const int at = (r * grid.columns) + c;
            if (c + 1 < grid.columns) {
                grid.across[static_cast<std::size_t>(at)] = static_cast<int>(grid.edges.size());
                grid.edges.push_back({at, at + 1});
            }
            if (r + 1 < grid.rows) {
                grid.down[static_cast<std::size_t>(at)] = static_cast<int>(grid.edges.size());
                grid.edges.push_back({at, at + grid.columns});
            }
        }
    }
    grid.torn.assign(grid.edges.size(), false);
    for (const int edge : cloth.torn) {
        if (edge >= 0 && static_cast<std::size_t>(edge) < grid.torn.size()) {
            grid.torn[static_cast<std::size_t>(edge)] = true;
        }
    }
    return grid;
}

std::vector<bool> cloth_triangles_2d(const ClothGrid2D& grid) {
    const auto torn = [&](int edge) {
        return edge >= 0 && grid.torn[static_cast<std::size_t>(edge)];
    };
    std::vector<bool> out;
    out.reserve(static_cast<std::size_t>(grid.columns - 1) *
                static_cast<std::size_t>(grid.rows - 1) * 2);
    for (int r = 0; r + 1 < grid.rows; ++r) {
        for (int c = 0; c + 1 < grid.columns; ++c) {
            const std::size_t at =
                (static_cast<std::size_t>(r) * static_cast<std::size_t>(grid.columns)) +
                static_cast<std::size_t>(c);
            const auto right = at + 1;
            const auto below = at + static_cast<std::size_t>(grid.columns);
            out.push_back(!torn(grid.across[at]) && !torn(grid.down[at]));
            out.push_back(!torn(grid.down[right]) && !torn(grid.across[below]));
        }
    }
    return out;
}

void set_cloth_2d_source(World& world, Cloth2DSource source) {
    world.ecs().ctx().insert_or_assign(Cloth2DSourceHolder{std::move(source)});
}

const ClothPoints2D* cloth_points_2d(const World& world, entt::entity entity) {
    const auto* holder = world.ecs().ctx().find<Cloth2DSourceHolder>();
    if (holder == nullptr || !holder->source) {
        return nullptr;
    }
    return holder->source(world, entity);
}

} // namespace slash::world
