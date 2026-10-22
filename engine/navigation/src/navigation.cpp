#include "slash/navigation/navigation.hpp"

#include "core.hpp"

#include "slash/world/components.hpp"
#include "slash/world/tree2d.hpp"
#include "slash/world/tree3d.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace slash::navigation {

namespace {

using world::NavigationAgent2D;
using world::NavigationAgent3D;
using world::NavigationLink2D;
using world::NavigationLink3D;
using world::NavigationRegion2D;
using world::NavigationRegion3D;

// NavigationDefaults3D::NAV_MESH_CELL_SIZE_MIN, the smallest cell a map takes.
constexpr float kCellSizeMin = 0.01F;

// The 2D map holds its points at (x, 0, y).
Vec3 to_map(Vec2 point) {
    return {point.x, 0.0F, point.y};
}
Vec2 to_2d(Vec3 point) {
    return {point.x, point.z};
}

// A float's bits. std::bit_cast would do, but the glibc 2.31 sysroot Linux exports build
// against has libstdc++ 10, which lacks it.
std::uint32_t bits_of(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

bool same_bits(Vec3 a, Vec3 b) {
    return bits_of(a.x) == bits_of(b.x) && bits_of(a.y) == bits_of(b.y) &&
           bits_of(a.z) == bits_of(b.z);
}

// The order regions, links and agents go in: entities with IDs by ID, then the rest by handle,
// so a map is built the same however its entities came to be.
struct Order {
    std::string id;
    entt::entity entity = entt::null;

    friend bool operator<(const Order& a, const Order& b) {
        if (a.id.empty() != b.id.empty()) {
            return !a.id.empty();
        }
        if (a.id != b.id) {
            return a.id < b.id;
        }
        return entt::to_integral(a.entity) < entt::to_integral(b.entity);
    }
};

template <typename Component>
std::vector<Order> ordered(const world::World& world) {
    std::vector<Order> out;
    for (const entt::entity entity : world.ecs().view<const Component>()) {
        out.push_back({std::string(world.id_of(entity)), entity});
    }
    std::sort(out.begin(), out.end());
    return out;
}

// What a region was built from, besides its entity: when only its entity changes (a session
// put the world back), the map is rebuilt with the same polygons and counts as unchanged.
struct RegionInput {
    std::string id;
    entt::entity entity = entt::null;
    std::string path;
    bool enabled = true;
    bool use_edge_connections = true;
    std::uint32_t navigation_layers = 1;
    float enter_cost = 0.0F;
    float travel_cost = 1.0F;
    std::array<float, 12> transform{};
    std::shared_ptr<const void> file; // the loaded mesh or polygon, kept so a reload is new
    // Kept so the file stays loaded while the map uses it, and isn't read again each tick.
    asset::Handle<NavigationMesh> mesh;
    asset::Handle<NavigationPolygon> polygon;

    [[nodiscard]] bool same_content(const RegionInput& o) const {
        return id == o.id && path == o.path && enabled == o.enabled &&
               use_edge_connections == o.use_edge_connections &&
               navigation_layers == o.navigation_layers &&
               bits_of(enter_cost) == bits_of(o.enter_cost) &&
               bits_of(travel_cost) == bits_of(o.travel_cost) &&
               std::ranges::equal(transform, o.transform,
                                  [](float a, float b) { return bits_of(a) == bits_of(b); }) &&
               file == o.file;
    }
};

struct BuiltRegion {
    RegionInput input;
    std::shared_ptr<const detail::Region> region;
};

struct LinkInput {
    std::string id;
    entt::entity entity = entt::null;
    bool enabled = true;
    bool bidirectional = true;
    std::uint32_t navigation_layers = 1;
    Vec3 start; // in the map's space
    Vec3 end;
    float enter_cost = 0.0F;
    float travel_cost = 1.0F;

    [[nodiscard]] bool same_content(const LinkInput& o) const {
        return id == o.id && enabled == o.enabled && bidirectional == o.bidirectional &&
               navigation_layers == o.navigation_layers && same_bits(start, o.start) &&
               same_bits(end, o.end) && bits_of(enter_cost) == bits_of(o.enter_cost) &&
               bits_of(travel_cost) == bits_of(o.travel_cost);
    }
};

struct MapError {
    std::string id;
    std::string message;
};

// One map: what it was built from and the map itself, as Godot's NavMap and its iteration.
struct MapState {
    Map kind = Map::ThreeD;
    bool built = false;
    MapSettings settings;
    std::uint64_t iteration_id = 0;
    std::vector<BuiltRegion> regions;
    std::vector<LinkInput> links;
    std::shared_ptr<detail::Map> map = std::make_shared<detail::Map>();
    std::vector<MapError> errors;
};

// An agent's path and progress, as the state of Godot's NavigationAgent nodes. Paths are in the
// map's space.
struct AgentState {
    bool seen = false;
    Vec3 last_target; // target_position as last seen, to tell when it changes
    bool target_position_submitted = false;
    bool target_reached = false;
    bool navigation_finished = true;
    bool last_waypoint_reached = false;
    std::size_t path_index = 0;
    std::uint64_t last_map_iteration_id = 0;
    Path<Vec3> path;
};

// The fields of either agent component.
struct AgentParams {
    Vec3 target;
    float path_desired_distance = 1.0F;
    float target_desired_distance = 1.0F;
    float path_height_offset = 0.0F;
    float path_max_distance = 5.0F;
    PathOptions options;
};

PathOptions options_of(const auto& agent) {
    PathOptions options;
    options.navigation_layers = agent.navigation_layers;
    if (!postprocessing_from_name(agent.path_postprocessing, options.postprocessing)) {
        options.postprocessing = Postprocessing::CorridorFunnel;
    }
    options.simplify_path = agent.simplify_path;
    options.simplify_epsilon = agent.simplify_epsilon;
    options.path_return_max_length = agent.path_return_max_length;
    options.path_return_max_radius = agent.path_return_max_radius;
    options.path_search_max_polygons = agent.path_search_max_polygons;
    options.path_search_max_distance = agent.path_search_max_distance;
    return options;
}

AgentParams params_of(const NavigationAgent2D& agent) {
    return {.target = to_map(agent.target_position),
            .path_desired_distance = agent.path_desired_distance,
            .target_desired_distance = agent.target_desired_distance,
            .path_height_offset = 0.0F,
            .path_max_distance = agent.path_max_distance,
            .options = options_of(agent)};
}

AgentParams params_of(const NavigationAgent3D& agent) {
    return {.target = agent.target_position,
            .path_desired_distance = agent.path_desired_distance,
            .target_desired_distance = agent.target_desired_distance,
            .path_height_offset = agent.path_height_offset,
            .path_max_distance = agent.path_max_distance,
            .options = options_of(agent)};
}

struct State {
    explicit State(asset::Assets& a, const Settings& s) : assets(&a), settings(s) {
        map_2d.kind = Map::TwoD;
        map_3d.kind = Map::ThreeD;
    }

    asset::Assets* assets;
    Settings settings;
    MapState map_2d;
    MapState map_3d;
    std::map<entt::entity, AgentState> agents_2d;
    std::map<entt::entity, AgentState> agents_3d;

    MapState& map(Map kind) { return kind == Map::TwoD ? map_2d : map_3d; }
    std::map<entt::entity, AgentState>& agents(Map kind) {
        return kind == Map::TwoD ? agents_2d : agents_3d;
    }

    void reset() {
        map_2d = MapState{};
        map_2d.kind = Map::TwoD;
        map_3d = MapState{};
        agents_2d.clear();
        agents_3d.clear();
    }
};

State* find_state(const world::World& world) {
    const auto* found = world.ecs().ctx().find<std::shared_ptr<State>>();
    return found != nullptr ? found->get() : nullptr;
}

// ---- Building maps.

// The global transform of a region or link, in the map's space.
struct Placement {
    world::Affine2D affine_2d;
    world::Affine3D affine_3d;
    bool three_d = true;

    [[nodiscard]] Vec3 apply(Vec3 point) const {
        if (three_d) {
            return affine_3d.apply(point);
        }
        return to_map(affine_2d.apply(to_2d(point)));
    }
    [[nodiscard]] std::array<float, 12> key() const {
        if (three_d) {
            const world::Affine3D& a = affine_3d;
            return {a.x.x, a.x.y, a.x.z, a.y.x,      a.y.y,      a.y.z,
                    a.z.x, a.z.y, a.z.z, a.origin.x, a.origin.y, a.origin.z};
        }
        const world::Affine2D& a = affine_2d;
        return {a.x.x, a.x.y, a.y.x, a.y.y, a.origin.x, a.origin.y, 0, 0, 0, 0, 0, 0};
    }
};

struct Trees {
    explicit Trees(const world::World& world) : tree_2d(world), tree_3d(world) {}
    world::Tree2D tree_2d;
    world::Tree3D tree_3d;

    [[nodiscard]] Placement placement(Map kind, entt::entity entity) const {
        Placement out;
        out.three_d = kind == Map::ThreeD;
        if (out.three_d) {
            out.affine_3d = tree_3d.global(entity);
        } else {
            out.affine_2d = tree_2d.global_affine(entity);
        }
        return out;
    }
};

Vec3 merge_cell_size(Map kind, const MapSettings& settings) {
    // NavMap3D::set_merge_rasterizer_cell_scale and _update_merge_rasterizer_cell_dimensions.
    const float scale =
        std::max(std::min(settings.merge_rasterizer_cell_scale, 0.1F), kCellSizeMin);
    const float cell = std::max(settings.cell_size, kCellSizeMin);
    const float height = kind == Map::ThreeD ? std::max(settings.cell_height, kCellSizeMin) : cell;
    return {cell * scale, height * scale, cell * scale};
}

template <typename Component, typename File>
void gather_regions(const world::World& world, const State& state, const Trees& trees, Map kind,
                    std::vector<RegionInput>& out, std::vector<MapError>& errors) {
    for (const Order& order : ordered<Component>(world)) {
        const Component& region = world.ecs().get<const Component>(order.entity);
        const std::string& path = [&]() -> const std::string& {
            if constexpr (std::is_same_v<Component, NavigationRegion3D>) {
                return region.navigation_mesh;
            } else {
                return region.navigation_polygon;
            }
        }();
        if (path.empty()) {
            continue;
        }
        const std::string_view suffix = kind == Map::ThreeD ? kMeshSuffix : kPolygonSuffix;
        if (!path.ends_with(suffix)) {
            errors.push_back({order.id, path + ": not a " + std::string(suffix) + " file"});
            continue;
        }
        auto loaded = state.assets->load<File>(path);
        if (!loaded) {
            errors.push_back({order.id, path + ": " + loaded.error().message});
            continue;
        }
        const Placement placement = trees.placement(kind, order.entity);
        RegionInput& input =
            out.emplace_back(RegionInput{.id = order.id,
                                         .entity = order.entity,
                                         .path = path,
                                         .enabled = region.enabled,
                                         .use_edge_connections = region.use_edge_connections,
                                         .navigation_layers = region.navigation_layers,
                                         .enter_cost = region.enter_cost,
                                         .travel_cost = region.travel_cost,
                                         .transform = placement.key(),
                                         .file = loaded.value().share(),
                                         .mesh = {},
                                         .polygon = {}});
        if constexpr (std::is_same_v<File, NavigationMesh>) {
            input.mesh = std::move(loaded).value();
        } else {
            input.polygon = std::move(loaded).value();
        }
    }
}

template <typename Component>
void gather_links(const world::World& world, const Trees& trees, Map kind,
                  std::vector<LinkInput>& out) {
    for (const Order& order : ordered<Component>(world)) {
        const Component& link = world.ecs().get<const Component>(order.entity);
        const Placement placement = trees.placement(kind, order.entity);
        Vec3 start;
        Vec3 end;
        if constexpr (std::is_same_v<Component, NavigationLink3D>) {
            start = link.start_position;
            end = link.end_position;
        } else {
            start = to_map(link.start_position);
            end = to_map(link.end_position);
        }
        out.push_back({.id = order.id,
                       .entity = order.entity,
                       .enabled = link.enabled,
                       .bidirectional = link.bidirectional,
                       .navigation_layers = link.navigation_layers,
                       .start = placement.apply(start),
                       .end = placement.apply(end),
                       .enter_cost = link.enter_cost,
                       .travel_cost = link.travel_cost});
    }
}

std::shared_ptr<const detail::Region> build_region(const RegionInput& input, Map kind,
                                                   const MapSettings& settings) {
    detail::RegionSource source;
    source.owner = input.entity;
    source.enabled = input.enabled;
    source.use_edge_connections = input.use_edge_connections;
    source.navigation_layers = input.navigation_layers;
    source.enter_cost = input.enter_cost;
    source.travel_cost = input.travel_cost;
    Placement placement;
    placement.three_d = kind == Map::ThreeD;
    const std::array<float, 12>& t = input.transform;
    if (placement.three_d) {
        placement.affine_3d = {.x = {t[0], t[1], t[2]},
                               .y = {t[3], t[4], t[5]},
                               .z = {t[6], t[7], t[8]},
                               .origin = {t[9], t[10], t[11]}};
        const auto* mesh = static_cast<const NavigationMesh*>(input.file.get());
        for (const Vec3 vertex : mesh->vertices) {
            source.vertices.push_back(placement.apply(vertex));
        }
        source.polygons = mesh->polygons;
    } else {
        placement.affine_2d = {.x = {t[0], t[1]}, .y = {t[2], t[3]}, .origin = {t[4], t[5]}};
        const auto* polygon = static_cast<const NavigationPolygon*>(input.file.get());
        for (const Vec2 vertex : polygon->vertices) {
            source.vertices.push_back(to_map(placement.affine_2d.apply(vertex)));
        }
        source.polygons = polygon->polygons;
    }
    return detail::build_region(source, merge_cell_size(kind, settings));
}

void sync_map(const world::World& world, State& state, const Trees& trees, MapState& m) {
    const MapSettings& settings =
        m.kind == Map::TwoD ? state.settings.map_2d : state.settings.map_3d;
    std::vector<RegionInput> regions;
    std::vector<LinkInput> links;
    std::vector<MapError> errors;
    if (m.kind == Map::ThreeD) {
        gather_regions<NavigationRegion3D, NavigationMesh>(world, state, trees, m.kind, regions,
                                                           errors);
        gather_links<NavigationLink3D>(world, trees, m.kind, links);
    } else {
        gather_regions<NavigationRegion2D, NavigationPolygon>(world, state, trees, m.kind, regions,
                                                              errors);
        gather_links<NavigationLink2D>(world, trees, m.kind, links);
    }
    m.errors = std::move(errors);

    bool content_changed = !m.built || settings != m.settings ||
                           regions.size() != m.regions.size() || links.size() != m.links.size();
    bool entity_changed = false;
    for (std::size_t i = 0; !content_changed && i < regions.size(); ++i) {
        content_changed = !regions[i].same_content(m.regions[i].input);
        entity_changed = entity_changed || regions[i].entity != m.regions[i].input.entity;
    }
    for (std::size_t i = 0; !content_changed && i < links.size(); ++i) {
        content_changed = !links[i].same_content(m.links[i]);
        entity_changed = entity_changed || links[i].entity != m.links[i].entity;
    }
    if (!content_changed && !entity_changed) {
        return;
    }

    // Regions that are as they were keep their polygons.
    std::vector<BuiltRegion> built;
    built.reserve(regions.size());
    const bool same_settings = m.built && settings == m.settings;
    for (RegionInput& input : regions) {
        std::shared_ptr<const detail::Region> region;
        if (same_settings) {
            const auto old = std::ranges::find_if(m.regions, [&](const BuiltRegion& each) {
                return each.input.entity == input.entity && each.input.same_content(input);
            });
            if (old != m.regions.end()) {
                region = old->region;
            }
        }
        if (region == nullptr) {
            region = build_region(input, m.kind, settings);
        }
        built.push_back({std::move(input), std::move(region)});
    }

    auto map = std::make_shared<detail::Map>();
    for (const BuiltRegion& region : built) {
        map->regions.push_back(region.region);
    }
    for (const LinkInput& input : links) {
        auto link = std::make_shared<detail::Link>();
        link->type = detail::OwnerType::Link;
        link->owner = input.entity;
        link->enabled = input.enabled;
        link->bidirectional = input.bidirectional;
        link->navigation_layers = input.navigation_layers;
        link->start_position = input.start;
        link->end_position = input.end;
        link->enter_cost = input.enter_cost;
        link->travel_cost = input.travel_cost;
        map->links.push_back(std::move(link));
    }
    detail::build_map(*map, {.use_edge_connections = settings.use_edge_connections,
                             .edge_connection_margin = settings.edge_connection_margin,
                             .link_connection_radius = settings.link_connection_radius,
                             .up = m.kind == Map::ThreeD ? settings.up : Vec3{0.0F, 1.0F, 0.0F}});
    m.map = std::move(map);
    m.regions = std::move(built);
    m.links = std::move(links);
    m.settings = settings;
    m.built = true;
    if (content_changed) {
        ++m.iteration_id;
    }
}

State* synced(world::World& world) {
    State* state = find_state(world);
    if (state != nullptr) {
        const Trees trees(world);
        sync_map(world, *state, trees, state->map_2d);
        sync_map(world, *state, trees, state->map_3d);
    }
    return state;
}

// ---- Queries.

detail::PathTask task_of(Vec3 start, Vec3 target, const PathOptions& options) {
    detail::PathTask task;
    task.start_position = start;
    task.target_position = target;
    task.navigation_layers = options.navigation_layers;
    switch (options.postprocessing) {
    case Postprocessing::CorridorFunnel:
        task.path_postprocessing = detail::PostProcessing::CorridorFunnel;
        break;
    case Postprocessing::EdgeCentered:
        task.path_postprocessing = detail::PostProcessing::EdgeCentered;
        break;
    case Postprocessing::None:
        task.path_postprocessing = detail::PostProcessing::None;
        break;
    }
    task.simplify_path = options.simplify_path;
    task.simplify_epsilon = options.simplify_epsilon;
    task.excluded_regions = options.excluded_regions;
    task.included_regions = options.included_regions;
    task.path_return_max_length = options.path_return_max_length;
    task.path_return_max_radius = options.path_return_max_radius;
    task.path_search_max_polygons = options.path_search_max_polygons;
    task.path_search_max_distance = options.path_search_max_distance;
    return task;
}

Path<Vec3> run_query(const MapState& m, Vec3 start, Vec3 target, const PathOptions& options) {
    detail::PathTask task = task_of(start, target, options);
    detail::query_path(*m.map, task);
    Path<Vec3> out;
    out.points = std::move(task.path_points);
    out.types.reserve(task.path_types.size());
    for (const detail::OwnerType type : task.path_types) {
        out.types.push_back(type == detail::OwnerType::Link ? Segment::Link : Segment::Region);
    }
    out.owners = std::move(task.path_owners);
    out.length = task.path_length;
    return out;
}

// ---- Agents, as Godot's NavigationAgent2D and NavigationAgent3D.

json::Value point_json(Map kind, Vec3 point) {
    if (kind == Map::TwoD) {
        return json::Array{point.x, point.z};
    }
    return json::Array{point.x, point.y, point.z};
}

Vec3 agent_origin(const world::World& world, Map kind, entt::entity entity) {
    if (kind == Map::ThreeD) {
        return world::Tree3D(world).global(entity).origin;
    }
    return to_map(world::Tree2D(world).global(entity).position);
}

AgentParams agent_params(const world::World& world, Map kind, entt::entity entity) {
    if (kind == Map::ThreeD) {
        return params_of(world.ecs().get<const NavigationAgent3D>(entity));
    }
    return params_of(world.ecs().get<const NavigationAgent2D>(entity));
}

class AgentUpdate {
public:
    AgentUpdate(world::World& world, State& state, Map kind, entt::entity entity, AgentState& agent)
        : world_(world), state_(state), kind_(kind), entity_(entity), agent_(agent),
          params_(agent_params(world, kind, entity)), id_(world.id_of(entity)) {}

    // NavigationAgent3D::set_target_position and _request_repath.
    void submit(Vec3 target) {
        agent_.last_target = target;
        agent_.target_position_submitted = true;
        agent_.path = {};
        agent_.target_reached = false;
        agent_.navigation_finished = false;
        agent_.last_waypoint_reached = false;
    }

    // A target_position changed since it was last seen counts as set; a new agent's counts if
    // it isn't the origin, as a scene's saved property sets Godot's.
    void notice_target() {
        if (!agent_.seen) {
            agent_.seen = true;
            agent_.last_target = params_.target;
            if (!same_bits(params_.target, Vec3{})) {
                submit(params_.target);
            }
        } else if (!same_bits(params_.target, agent_.last_target)) {
            submit(params_.target);
        }
    }

    // NavigationAgent3D::_update_navigation.
    void update() {
        if (!agent_.target_position_submitted) {
            return;
        }
        const Vec3 origin = agent_origin(world_, kind_, entity_);
        const MapState& m = state_.map(kind_);

        bool reload = false;
        // NavAgent3D::is_map_changed, which notes the iteration it saw.
        const bool map_changed = m.iteration_id != agent_.last_map_iteration_id;
        agent_.last_map_iteration_id = m.iteration_id;
        if (map_changed || agent_.path.points.empty()) {
            reload = true;
        } else if (agent_.path_index > 0) {
            // Too far from the path: find it again.
            Vec3 segment_a = agent_.path.points[agent_.path_index - 1];
            Vec3 segment_b = agent_.path.points[agent_.path_index];
            segment_a.y -= params_.path_height_offset;
            segment_b.y -= params_.path_height_offset;
            const Vec3 p = detail::closest_point_to_segment(origin, segment_a, segment_b);
            if (detail::distance(origin, p) >= params_.path_max_distance) {
                reload = true;
            }
        }
        if (reload) {
            agent_.path = run_query(m, origin, params_.target, params_.options);
            agent_.navigation_finished = false;
            agent_.last_waypoint_reached = false;
            agent_.path_index = 0;
            world_.emit("path_changed", id_);
        }
        if (agent_.path.points.empty() || agent_.navigation_finished) {
            return;
        }
        if (within_target_distance(origin)) {
            // waypoint_reached too, in case it came within distance of one.
            advance_waypoints(origin);
            agent_.target_reached = true;
            world_.emit("target_reached", id_);
            finish();
        } else {
            advance_waypoints(origin);
            // Navigation goes on past the last waypoint while the target can be reached.
            if (agent_.last_waypoint_reached && !target_reachable()) {
                finish();
            }
        }
    }

    [[nodiscard]] bool target_reachable() const {
        return params_.target_desired_distance >=
               detail::distance(final_position(), params_.target);
    }

    [[nodiscard]] Vec3 final_position() const {
        if (agent_.path.points.empty()) {
            return {};
        }
        return lowered(agent_.path.points.back());
    }

    [[nodiscard]] Vec3 next_position() const {
        if (agent_.path.points.empty()) {
            return agent_origin(world_, kind_, entity_);
        }
        return lowered(agent_.path.points[agent_.path_index]);
    }

private:
    [[nodiscard]] Vec3 lowered(Vec3 point) const {
        return {point.x, point.y - params_.path_height_offset, point.z};
    }

    [[nodiscard]] bool within_target_distance(Vec3 origin) const {
        return detail::distance(origin, params_.target) < params_.target_desired_distance;
    }

    [[nodiscard]] bool within_waypoint_distance(Vec3 origin) const {
        return detail::distance(origin, lowered(agent_.path.points[agent_.path_index])) <
               params_.path_desired_distance;
    }

    void advance_waypoints(Vec3 origin) {
        if (agent_.last_waypoint_reached) {
            return;
        }
        // To the farthest waypoint it can.
        while (within_waypoint_distance(origin)) {
            waypoint_reached();
            if (agent_.path_index == agent_.path.points.size() - 1) {
                agent_.last_waypoint_reached = true;
                break;
            }
            ++agent_.path_index;
        }
    }

    // NavigationAgent3D::_trigger_waypoint_reached, with every metadata flag on, as Godot's
    // default.
    void waypoint_reached() {
        const std::size_t index = agent_.path_index;
        const Vec3 waypoint = agent_.path.points[index];
        const Segment type = agent_.path.types[index];
        const entt::entity owner = agent_.path.owners[index];
        json::Value details;
        details["position"] = point_json(kind_, waypoint);
        details["type"] = type == Segment::Link ? "link" : "region";
        const std::string_view owner_id = world_.valid(owner) ? world_.id_of(owner) : "";
        details["owner"] = owner_id.empty() ? json::Value() : json::Value(owner_id);
        if (type == Segment::Link && world_.valid(owner)) {
            Vec3 start;
            Vec3 end;
            bool found = false;
            Placement placement;
            placement.three_d = kind_ == Map::ThreeD;
            if (kind_ == Map::ThreeD) {
                if (const auto* link = world_.ecs().try_get<const NavigationLink3D>(owner)) {
                    placement.affine_3d = world::Tree3D(world_).global(owner);
                    start = placement.apply(link->start_position);
                    end = placement.apply(link->end_position);
                    found = true;
                }
            } else if (const auto* link = world_.ecs().try_get<const NavigationLink2D>(owner)) {
                placement.affine_2d = world::Tree2D(world_).global_affine(owner);
                start = placement.apply(to_map(link->start_position));
                end = placement.apply(to_map(link->end_position));
                found = true;
            }
            if (found) {
                const bool from_start =
                    detail::distance(waypoint, start) < detail::distance(waypoint, end);
                details["link_entry_position"] = point_json(kind_, from_start ? start : end);
                details["link_exit_position"] = point_json(kind_, from_start ? end : start);
            }
        }
        if (type == Segment::Link) {
            world_.emit("waypoint_reached", id_, details);
            world_.emit("link_reached", id_, std::move(details));
        } else {
            world_.emit("waypoint_reached", id_, std::move(details));
        }
    }

    // NavigationAgent3D::_transition_to_navigation_finished.
    void finish() {
        agent_.navigation_finished = true;
        agent_.target_position_submitted = false;
        world_.emit("navigation_finished", id_);
    }

    world::World& world_;
    State& state_;
    Map kind_;
    entt::entity entity_;
    AgentState& agent_;
    AgentParams params_;
    std::string id_;
};

bool has_agent(const world::World& world, Map kind, entt::entity entity) {
    if (!world.valid(entity)) {
        return false;
    }
    return kind == Map::ThreeD ? world.ecs().all_of<NavigationAgent3D>(entity)
                               : world.ecs().all_of<NavigationAgent2D>(entity);
}

// Which map an agent is on: its 3D agent's, or else its 2D agent's.
Map agent_map(const world::World& world, entt::entity entity) {
    return has_agent(world, Map::ThreeD, entity) ? Map::ThreeD : Map::TwoD;
}

// The agent's state, brought up to date with the maps and its target_position, then its
// path, as Godot's getters call _update_navigation. Null if the entity has no agent.
AgentState* refreshed(world::World& world, entt::entity entity, Map kind) {
    State* state = synced(world);
    if (state == nullptr || !has_agent(world, kind, entity)) {
        return nullptr;
    }
    AgentState& agent = state->agents(kind)[entity];
    AgentUpdate update(world, *state, kind, entity, agent);
    update.notice_target();
    update.update();
    return &agent;
}

void drop_stale_agents(const world::World& world, State& state) {
    for (const Map kind : {Map::TwoD, Map::ThreeD}) {
        std::erase_if(state.agents(kind),
                      [&](const auto& entry) { return !has_agent(world, kind, entry.first); });
    }
}

void step(world::World& world, State& state) {
    {
        const Trees trees(world);
        sync_map(world, state, trees, state.map_2d);
        sync_map(world, state, trees, state.map_3d);
    }
    drop_stale_agents(world, state);
    const auto run = [&]<typename Component>(Map kind) {
        for (const Order& order : ordered<Component>(world)) {
            AgentState& agent = state.agents(kind)[order.entity];
            AgentUpdate update(world, state, kind, order.entity, agent);
            update.notice_target();
            update.update();
        }
    };
    run.template operator()<NavigationAgent2D>(Map::TwoD);
    run.template operator()<NavigationAgent3D>(Map::ThreeD);
}

// ---- Snapshots.

struct SavedAgent {
    AgentState state;
    std::vector<std::string> owners; // the path's owners by ID
};

struct Saved {
    MapState map_2d;
    MapState map_3d;
    std::map<std::string, SavedAgent, std::less<>> agents_2d;
    std::map<std::string, SavedAgent, std::less<>> agents_3d;
};

world::MemoryPart memory_of(const State& state) {
    world::MemoryPart part;
    part.name = "navigation";
    std::uint64_t polygons = 0;
    std::uint64_t bytes = 0;
    for (const MapState* m : {&state.map_2d, &state.map_3d}) {
        for (const BuiltRegion& region : m->regions) {
            polygons += region.region->polygons.size();
            for (const detail::Polygon& polygon : region.region->polygons) {
                bytes += sizeof(polygon) + (polygon.vertices.size() * sizeof(Vec3));
            }
            for (const auto& connections : region.region->internal_connections) {
                bytes += sizeof(std::vector<detail::Connection>) +
                         (connections.size() * sizeof(detail::Connection));
            }
        }
        bytes += m->map->path_corridor.size() * sizeof(detail::NavigationPoly);
    }
    const std::uint64_t agents = state.agents_2d.size() + state.agents_3d.size();
    for (const auto* agents_of : {&state.agents_2d, &state.agents_3d}) {
        for (const auto& [entity, agent] : *agents_of) {
            bytes += world::kMapNodeBytes<std::map<entt::entity, AgentState>> +
                     (agent.path.points.size() * (sizeof(Vec3) + sizeof(entt::entity) + 1));
        }
    }
    part.bytes = bytes;
    part.counts["regions"] = state.map_2d.regions.size() + state.map_3d.regions.size();
    part.counts["links"] = state.map_2d.links.size() + state.map_3d.links.size();
    part.counts["polygons"] = polygons;
    part.counts["agents"] = agents;
    return part;
}

} // namespace

// ---- Settings.

json::Value settings_schema(Map map) {
    const MapSettings defaults = map == Map::TwoD ? Settings{}.map_2d : Settings{}.map_3d;
    const std::string unit = map == Map::TwoD ? "pixels" : "meters";
    const auto number = [](std::string description, double fallback) {
        json::Value out;
        out["type"] = "number";
        out["minimum"] = 0;
        out["default"] = fallback;
        out["description"] = std::move(description);
        return out;
    };
    json::Value schema;
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["description"] =
        map == Map::TwoD ? "The 2D navigation map, as Godot's navigation/2d project settings."
                         : "The 3D navigation map, as Godot's navigation/3d project settings.";
    json::Value& properties = schema["properties"];
    properties["cell_size"] =
        number("The map's cell size in " + unit + ": navigation polygons should be made for it.",
               defaults.cell_size);
    if (map == Map::ThreeD) {
        properties["cell_height"] =
            number("The map's cell height in meters.", defaults.cell_height);
    }
    // Cells can't be empty: edge ends are rounded to them.
    for (const char* key : {"cell_size", "cell_height"}) {
        if (json::Value* cell = properties.find(key); cell != nullptr) {
            cell->as_object().erase("minimum");
            (*cell)["exclusiveMinimum"] = 0;
        }
    }
    properties["merge_rasterizer_cell_scale"] =
        number("Edge ends closer than cell_size times this, at most 0.1, are the same point.",
               defaults.merge_rasterizer_cell_scale);
    properties["use_edge_connections"]["type"] = "boolean";
    properties["use_edge_connections"]["default"] = defaults.use_edge_connections;
    properties["use_edge_connections"]["description"] =
        "Joins regions' edges that come within edge_connection_margin.";
    properties["edge_connection_margin"] =
        number("How close, in " + unit + ", regions' edges must come to join.",
               defaults.edge_connection_margin);
    properties["link_connection_radius"] =
        number("How far, in " + unit + ", a link's ends reach for a region.",
               defaults.link_connection_radius);
    if (map == Map::ThreeD) {
        json::Value up;
        up["type"] = "array";
        up["items"]["type"] = "number";
        up["minItems"] = 3;
        up["maxItems"] = 3;
        up["default"] = json::Array{defaults.up.x, defaults.up.y, defaults.up.z};
        up["description"] = "Which way is up for paths, as [x, y, z].";
        properties["up"] = std::move(up);
    }
    return schema;
}

MapSettings parse_settings(Map map, const json::Value& settings) {
    MapSettings out = map == Map::TwoD ? Settings{}.map_2d : Settings{}.map_3d;
    const auto number = [&](std::string_view key, float& field) {
        if (const json::Value* value = settings.find(key); value != nullptr) {
            field = static_cast<float>(value->as_double());
        }
    };
    number("cell_size", out.cell_size);
    if (map == Map::ThreeD) {
        number("cell_height", out.cell_height);
    } else {
        out.cell_height = out.cell_size;
    }
    number("merge_rasterizer_cell_scale", out.merge_rasterizer_cell_scale);
    number("edge_connection_margin", out.edge_connection_margin);
    number("link_connection_radius", out.link_connection_radius);
    if (const json::Value* use = settings.find("use_edge_connections"); use != nullptr) {
        out.use_edge_connections = use->as_bool();
    }
    if (const json::Value* up = settings.find("up"); up != nullptr && map == Map::ThreeD) {
        const json::Array& xyz = up->as_array();
        out.up = {static_cast<float>(xyz.at(0).as_double()),
                  static_cast<float>(xyz.at(1).as_double()),
                  static_cast<float>(xyz.at(2).as_double())};
    }
    return out;
}

void install(world::World& world, asset::Assets& assets, const Settings& settings) {
    add_asset_types(assets);
    if (State* state = find_state(world); state != nullptr) {
        state->assets = &assets;
        state->settings = settings;
        return;
    }
    auto state = std::make_shared<State>(assets, settings);
    world.ecs().ctx().emplace<std::shared_ptr<State>>(state);
    world.add_reset([state](world::World& /*world*/) { state->reset(); });
    world.add_memory_source("navigation",
                            [state](const world::World& /*world*/) { return memory_of(*state); });
    world.add_system("navigation",
                     [state](world::World& w, double /*tick_seconds*/) { step(w, *state); },
                     {.doc = "Rebuilds changed navigation maps and moves agents along their paths.",
                      .priority = 960});
}

void set_settings(world::World& world, const Settings& settings) {
    if (State* state = find_state(world); state != nullptr) {
        state->settings = settings;
    }
}

// ---- Paths and points.

std::string_view postprocessing_name(Postprocessing postprocessing) {
    switch (postprocessing) {
    case Postprocessing::CorridorFunnel:
        return "corridorfunnel";
    case Postprocessing::EdgeCentered:
        return "edgecentered";
    case Postprocessing::None:
        return "none";
    }
    return "corridorfunnel";
}

bool postprocessing_from_name(std::string_view name, Postprocessing& out) {
    for (const Postprocessing each :
         {Postprocessing::CorridorFunnel, Postprocessing::EdgeCentered, Postprocessing::None}) {
        if (postprocessing_name(each) == name) {
            out = each;
            return true;
        }
    }
    return false;
}

Path<Vec2> query_path(world::World& world, Vec2 start, Vec2 target, const PathOptions& options) {
    Path<Vec2> out;
    const State* state = synced(world);
    if (state == nullptr) {
        return out;
    }
    Path<Vec3> path = run_query(state->map_2d, to_map(start), to_map(target), options);
    out.points.reserve(path.points.size());
    for (const Vec3 point : path.points) {
        out.points.push_back(to_2d(point));
    }
    out.types = std::move(path.types);
    out.owners = std::move(path.owners);
    out.length = path.length;
    return out;
}

Path<Vec3> query_path(world::World& world, Vec3 start, Vec3 target, const PathOptions& options) {
    const State* state = synced(world);
    if (state == nullptr) {
        return {};
    }
    return run_query(state->map_3d, start, target, options);
}

ClosestPoint2D closest_point(world::World& world, Vec2 point) {
    const State* state = synced(world);
    if (state == nullptr) {
        return {};
    }
    const detail::ClosestPoint found = detail::closest_point(*state->map_2d.map, to_map(point));
    return {to_2d(found.point), found.owner};
}

ClosestPoint3D closest_point(world::World& world, Vec3 point) {
    const State* state = synced(world);
    if (state == nullptr) {
        return {};
    }
    const detail::ClosestPoint found = detail::closest_point(*state->map_3d.map, point);
    return {found.point, found.normal, found.owner};
}

Vec2 random_point_2d(world::World& world, std::uint32_t navigation_layers, bool uniformly,
                     std::uint64_t seed) {
    const State* state = synced(world);
    if (state == nullptr) {
        return {};
    }
    detail::Random random(seed);
    return to_2d(detail::random_point(*state->map_2d.map, navigation_layers, uniformly, random));
}

Vec3 random_point_3d(world::World& world, std::uint32_t navigation_layers, bool uniformly,
                     std::uint64_t seed) {
    const State* state = synced(world);
    if (state == nullptr) {
        return {};
    }
    detail::Random random(seed);
    return detail::random_point(*state->map_3d.map, navigation_layers, uniformly, random);
}

MapInfo map_info(world::World& world, Map map) {
    MapInfo out;
    State* state = synced(world);
    if (state == nullptr) {
        return out;
    }
    const MapState& m = state->map(map);
    out.iteration_id = m.iteration_id;
    out.regions = m.regions.size();
    out.links = m.links.size();
    out.polygons = static_cast<std::size_t>(m.map->polygon_count);
    out.edge_connections = static_cast<std::size_t>(m.map->edge_connection_count);
    out.free_edges = static_cast<std::size_t>(m.map->edge_free_count);
    out.merge_errors = static_cast<std::size_t>(m.map->edge_merge_errors);
    for (const BuiltRegion& region : m.regions) {
        out.merge_errors += static_cast<std::size_t>(region.region->edge_merge_errors);
    }
    for (const MapError& error : m.errors) {
        out.errors.push_back({error.id, error.message});
    }
    return out;
}

void sync(world::World& world) {
    (void)synced(world);
}

// ---- Agents.

void set_target_position(world::World& world, entt::entity agent, Vec2 position) {
    if (!has_agent(world, Map::TwoD, agent)) {
        return;
    }
    world.ecs().patch<NavigationAgent2D>(agent, [&](auto& a) { a.target_position = position; });
    State* state = find_state(world);
    if (state == nullptr) {
        return;
    }
    AgentState& agent_state = state->agents_2d[agent];
    agent_state.seen = true;
    AgentUpdate(world, *state, Map::TwoD, agent, agent_state).submit(to_map(position));
}

void set_target_position(world::World& world, entt::entity agent, Vec3 position) {
    if (!has_agent(world, Map::ThreeD, agent)) {
        return;
    }
    world.ecs().patch<NavigationAgent3D>(agent, [&](auto& a) { a.target_position = position; });
    State* state = find_state(world);
    if (state == nullptr) {
        return;
    }
    AgentState& agent_state = state->agents_3d[agent];
    agent_state.seen = true;
    AgentUpdate(world, *state, Map::ThreeD, agent, agent_state).submit(position);
}

Vec2 next_path_position_2d(world::World& world, entt::entity agent) {
    AgentState* state = refreshed(world, agent, Map::TwoD);
    if (state == nullptr) {
        return {};
    }
    return to_2d(AgentUpdate(world, *find_state(world), Map::TwoD, agent, *state).next_position());
}

Vec3 next_path_position_3d(world::World& world, entt::entity agent) {
    AgentState* state = refreshed(world, agent, Map::ThreeD);
    if (state == nullptr) {
        return {};
    }
    return AgentUpdate(world, *find_state(world), Map::ThreeD, agent, *state).next_position();
}

bool is_navigation_finished(world::World& world, entt::entity agent) {
    const AgentState* state = refreshed(world, agent, agent_map(world, agent));
    return state == nullptr || state->navigation_finished;
}

bool is_target_reached(const world::World& world, entt::entity agent) {
    const State* state = find_state(world);
    if (state == nullptr) {
        return false;
    }
    const Map kind = agent_map(world, agent);
    const auto& agents = kind == Map::TwoD ? state->agents_2d : state->agents_3d;
    const auto found = agents.find(agent);
    return found != agents.end() && found->second.target_reached;
}

bool is_target_reachable(world::World& world, entt::entity agent) {
    const Map kind = agent_map(world, agent);
    AgentState* state = refreshed(world, agent, kind);
    if (state == nullptr) {
        return false;
    }
    return AgentUpdate(world, *find_state(world), kind, agent, *state).target_reachable();
}

Vec2 final_position_2d(world::World& world, entt::entity agent) {
    AgentState* state = refreshed(world, agent, Map::TwoD);
    if (state == nullptr) {
        return {};
    }
    return to_2d(AgentUpdate(world, *find_state(world), Map::TwoD, agent, *state).final_position());
}

Vec3 final_position_3d(world::World& world, entt::entity agent) {
    AgentState* state = refreshed(world, agent, Map::ThreeD);
    if (state == nullptr) {
        return {};
    }
    return AgentUpdate(world, *find_state(world), Map::ThreeD, agent, *state).final_position();
}

float distance_to_target(const world::World& world, entt::entity agent) {
    const Map kind = agent_map(world, agent);
    if (!has_agent(world, kind, agent)) {
        return 0.0F;
    }
    return detail::distance(agent_origin(world, kind, agent),
                            agent_params(world, kind, agent).target);
}

AgentInfo agent_info(world::World& world, entt::entity agent) {
    AgentInfo out;
    const State* state = find_state(world);
    if (state == nullptr) {
        return out;
    }
    const Map kind = agent_map(world, agent);
    const auto& agents = kind == Map::TwoD ? state->agents_2d : state->agents_3d;
    const auto found = agents.find(agent);
    if (found == agents.end()) {
        return out;
    }
    const AgentState& a = found->second;
    out.target_submitted = a.target_position_submitted;
    out.navigation_finished = a.navigation_finished;
    out.target_reached = a.target_reached;
    out.last_waypoint_reached = a.last_waypoint_reached;
    out.path_index = a.path_index;
    out.path = a.path;
    if (kind == Map::TwoD) {
        for (Vec3& point : out.path.points) {
            point = {point.x, point.z, 0.0F};
        }
    }
    return out;
}

// ---- Snapshots.

Snapshot save(const world::World& world) {
    const State* state = find_state(world);
    if (state == nullptr) {
        return {};
    }
    auto saved = std::make_shared<Saved>();
    saved->map_2d = state->map_2d;
    saved->map_3d = state->map_3d;
    const auto save_agents = [&](const std::map<entt::entity, AgentState>& from,
                                 std::map<std::string, SavedAgent, std::less<>>& to) {
        for (const auto& [entity, agent] : from) {
            const std::string_view id = world.id_of(entity);
            if (id.empty()) {
                continue;
            }
            SavedAgent& out = to[std::string(id)];
            out.state = agent;
            for (const entt::entity owner : agent.path.owners) {
                out.owners.emplace_back(world.id_of(owner));
            }
        }
    };
    save_agents(state->agents_2d, saved->agents_2d);
    save_agents(state->agents_3d, saved->agents_3d);
    return {std::move(saved)};
}

void restore(world::World& world, const Snapshot& snapshot) {
    State* state = find_state(world);
    if (state == nullptr || snapshot.state == nullptr) {
        return;
    }
    const auto& saved = *static_cast<const Saved*>(snapshot.state.get());
    state->map_2d = saved.map_2d;
    state->map_3d = saved.map_3d;
    const auto restore_agents = [&](const std::map<std::string, SavedAgent, std::less<>>& from,
                                    std::map<entt::entity, AgentState>& to) {
        to.clear();
        for (const auto& [id, agent] : from) {
            const entt::entity entity = world.find(id);
            if (entity == entt::null) {
                continue;
            }
            AgentState& out = to[entity];
            out = agent.state;
            for (std::size_t i = 0; i < out.path.owners.size(); ++i) {
                out.path.owners[i] = agent.owners[i].empty() ? entt::entity{entt::null}
                                                             : world.find(agent.owners[i]);
            }
        }
    };
    restore_agents(saved.agents_2d, state->agents_2d);
    restore_agents(saved.agents_3d, state->agents_3d);
}

// ---- Scene checks.

std::vector<json::ValidationError> check_scene(const json::Value& doc,
                                               const world::FileReader& reader) {
    std::vector<json::ValidationError> errors;
    if (!reader) {
        return errors;
    }
    // Each file's error, or "", by whether it is a mesh and its path.
    std::map<std::pair<bool, std::string>, std::string> checked;
    const auto check_file = [&](const std::string& path, bool three_d) -> std::string {
        if (const auto found = checked.find({three_d, path}); found != checked.end()) {
            return found->second;
        }
        std::string error;
        const std::string_view suffix = three_d ? kMeshSuffix : kPolygonSuffix;
        if (!path.ends_with(suffix)) {
            error = three_d ? "navigation meshes are .navmesh.json files"
                            : "navigation polygons are .navpoly.json files";
        } else if (auto text = reader(path); !text) {
            error = path + ": " + text.error().message;
        } else if (auto parsed = json::parse(text.value()); !parsed) {
            error = path + ": " + parsed.error().message;
        } else {
            const auto problems =
                three_d ? validate_mesh(parsed.value()) : validate_polygon(parsed.value());
            if (!problems.empty()) {
                error =
                    path + ": " +
                    (problems.front().path.empty() ? std::string("root") : problems.front().path) +
                    ": " + problems.front().message;
            }
        }
        checked.emplace(std::pair{three_d, path}, error);
        return error;
    };
    const auto check = [&](const json::Value& components, const std::string& at) {
        for (const auto& [name, field, three_d] :
             {std::tuple{"NavigationRegion2D", "navigation_polygon", false},
              std::tuple{"NavigationRegion3D", "navigation_mesh", true}}) {
            const json::Value* region = components.find(name);
            if (region == nullptr || !region->is_object()) {
                continue;
            }
            const json::Value* path = region->find(field);
            if (path == nullptr || !path->is_string() || path->as_string().empty()) {
                continue;
            }
            if (std::string error = check_file(path->as_string(), three_d); !error.empty()) {
                errors.push_back({at + "/" + name + "/" + field, std::move(error)});
            }
        }
    };
    if (const json::Value* entities = doc.find("entities"); entities != nullptr) {
        for (const auto& [id, entity] : entities->as_object()) {
            if (const json::Value* components = entity.find("components"); components != nullptr) {
                check(*components, "/entities/" + json::pointer_escape(id) + "/components");
            }
        }
    } else if (const json::Value* components = doc.find("components"); components != nullptr) {
        check(*components, "/components");
    }
    return errors;
}

} // namespace slash::navigation
