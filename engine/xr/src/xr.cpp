#include "slash/xr/xr.hpp"

#include "internal.hpp"

#include "slash/world/components.hpp"
#include "slash/xr/openxr.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace slash::xr {
namespace {

constexpr std::array<std::pair<TrackerType, std::string_view>, 7> kTrackerTypes{{
    {TrackerType::Head, "head"},
    {TrackerType::Controller, "controller"},
    {TrackerType::Basestation, "basestation"},
    {TrackerType::Anchor, "anchor"},
    {TrackerType::Hand, "hand"},
    {TrackerType::Body, "body"},
    {TrackerType::Face, "face"},
}};
constexpr std::array<std::string_view, 3> kHands{"unknown", "left", "right"};
constexpr std::array<std::string_view, 3> kConfidences{"none", "low", "high"};
constexpr std::array<std::string_view, 5> kPlayAreas{"unknown", "3dof", "sitting", "roomscale",
                                                     "stage"};
constexpr std::array<std::string_view, 3> kBlendModes{"opaque", "additive", "alpha"};
constexpr std::array<std::string_view, 5> kTrackingStatuses{
    "normal", "excessive_motion", "insufficient_features", "unknown", "not_tracking"};

template <std::size_t N>
std::optional<std::size_t> index_of(const std::array<std::string_view, N>& names,
                                    std::string_view name) {
    const auto found = std::find(names.begin(), names.end(), name);
    if (found == names.end()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(std::distance(names.begin(), found));
}

std::string key_of(const world::World& world, entt::entity entity) {
    const std::string_view id = world.id_of(entity);
    return id.empty() ? "#" + std::to_string(entt::to_integral(entity)) : std::string(id);
}

float length(Vec3 v) {
    return std::sqrt((v.x * v.x) + (v.y * v.y) + (v.z * v.z));
}

Vec3 normalized(Vec3 v) {
    const float l = length(v);
    return l > 0.0F ? v * (1.0F / l) : v;
}

Vec3 cross(Vec3 a, Vec3 b) {
    return {(a.y * b.z) - (a.z * b.y), (a.z * b.x) - (a.x * b.z), (a.x * b.y) - (a.y * b.x)};
}

float dot(Vec3 a, Vec3 b) {
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

// The vector with -0 made 0, so placed transforms don't save as -0.
Vec3 unsigned_zeros(Vec3 v) {
    return {v.x + 0.0F, v.y + 0.0F, v.z + 0.0F};
}

bool is_identity(const world::Affine3D& transform) {
    return transform == world::Affine3D{};
}

// Godot's Node3D::set_transform: the basis split into a rotation and a scale.
world::Transform3D placed(world::Transform3D transform, const world::Affine3D& to) {
    const float sign = dot(cross(to.x, to.y), to.z) < 0.0F ? -1.0F : 1.0F;
    transform.position = unsigned_zeros(to.origin);
    transform.scale = {sign * length(to.x), sign * length(to.y), sign * length(to.z)};
    world::Affine3D rotation = world::orthonormalized(to);
    if (sign < 0.0F) {
        rotation.x = rotation.x * -1.0F;
        rotation.y = rotation.y * -1.0F;
        rotation.z = rotation.z * -1.0F;
    }
    transform.rotation = unsigned_zeros(world::euler(rotation));
    return transform;
}

json::Value vec3_json(Vec3 v) {
    v = unsigned_zeros(v);
    return json::Array{v.x, v.y, v.z};
}

// A transform as position and, when it turns, rotation in Transform3D's Euler angles.
void put_transform(json::Value& out, const world::Affine3D& transform) {
    out["position"] = vec3_json(transform.origin);
    const Vec3 rotation = unsigned_zeros(world::euler(world::orthonormalized(transform)));
    if (rotation != Vec3{}) {
        out["rotation"] = vec3_json(rotation);
    }
}

json::Value input_json(const Input& value) {
    if (const bool* pressed = std::get_if<bool>(&value)) {
        return *pressed;
    }
    if (const float* number = std::get_if<float>(&value)) {
        return *number;
    }
    const Vec2 axis = std::get<Vec2>(value);
    return json::Array{axis.x, axis.y};
}

const world::XRNode3D* node_of(const world::World& world, entt::entity entity) {
    const auto& ecs = world.ecs();
    if (const auto* controller = ecs.try_get<world::XRController3D>(entity)) {
        return controller;
    }
    if (const auto* anchor = ecs.try_get<world::XRAnchor3D>(entity)) {
        return anchor;
    }
    return nullptr;
}

std::string_view node_type(const world::World& world, entt::entity entity) {
    const auto& ecs = world.ecs();
    if (ecs.all_of<world::XRController3D>(entity)) {
        return "XRController3D";
    }
    if (ecs.all_of<world::XRAnchor3D>(entity)) {
        return "XRAnchor3D";
    }
    return "XRCamera3D";
}

// The XR nodes, in tree order, then those outside the tree in the order they were made.
std::vector<entt::entity> xr_nodes(const world::World& world, const world::Tree3D& tree) {
    const auto& ecs = world.ecs();
    const auto is_node = [&](entt::entity entity) {
        return ecs.any_of<world::XRCamera3D, world::XRController3D, world::XRAnchor3D>(entity);
    };
    std::vector<entt::entity> nodes;
    for (const entt::entity entity : tree.order()) {
        if (is_node(entity)) {
            nodes.push_back(entity);
        }
    }
    return nodes;
}

// The current XROrigin3D, as Godot's: the one with current set, else the first.
entt::entity current_origin(const world::World& world, const world::Tree3D& tree) {
    entt::entity first = entt::null;
    for (const entt::entity entity : tree.order()) {
        if (const auto* origin = world.ecs().try_get<world::XROrigin3D>(entity)) {
            if (origin->current) {
                return entity;
            }
            if (first == entt::null) {
                first = entity;
            }
        }
    }
    return first;
}

} // namespace

std::string_view tracker_type_name(TrackerType type) {
    for (const auto& [each, name] : kTrackerTypes) {
        if (each == type) {
            return name;
        }
    }
    return "head";
}

std::optional<TrackerType> tracker_type_named(std::string_view name) {
    for (const auto& [each, each_name] : kTrackerTypes) {
        if (each_name == name) {
            return each;
        }
    }
    return std::nullopt;
}

std::string_view hand_name(Hand hand) {
    return kHands.at(static_cast<std::size_t>(hand));
}

std::optional<Hand> hand_named(std::string_view name) {
    const auto found = index_of(kHands, name);
    return found ? std::optional(static_cast<Hand>(*found)) : std::nullopt;
}

std::string_view confidence_name(Confidence confidence) {
    return kConfidences.at(static_cast<std::size_t>(confidence));
}

std::string_view play_area_name(PlayArea mode) {
    return kPlayAreas.at(static_cast<std::size_t>(mode));
}

std::optional<PlayArea> play_area_named(std::string_view name) {
    const auto found = index_of(kPlayAreas, name);
    return found ? std::optional(static_cast<PlayArea>(*found)) : std::nullopt;
}

std::string_view blend_mode_name(BlendMode mode) {
    return kBlendModes.at(static_cast<std::size_t>(mode));
}

std::optional<BlendMode> blend_mode_named(std::string_view name) {
    const auto found = index_of(kBlendModes, name);
    return found ? std::optional(static_cast<BlendMode>(*found)) : std::nullopt;
}

std::string_view tracking_status_name(TrackingStatus status) {
    return kTrackingStatuses.at(static_cast<std::size_t>(status));
}

namespace detail {

Server* server_of(const world::World& world) {
    const auto* found = world.ecs().ctx().find<std::shared_ptr<Server>>();
    return found != nullptr ? found->get() : nullptr;
}

Server& server(world::World& world) {
    auto& ctx = world.ecs().ctx();
    if (!ctx.contains<std::shared_ptr<Server>>()) {
        install(world);
    }
    return *ctx.get<std::shared_ptr<Server>>();
}

void Server::route(world::World& world) {
    auto& ecs = world.ecs();
    // XR nodes are Node3Ds: they are placed by their Transform3D.
    std::vector<entt::entity> missing;
    for (const entt::entity entity : ecs.view<world::XRController3D>()) {
        if (!ecs.all_of<world::Transform3D>(entity)) {
            missing.push_back(entity);
        }
    }
    for (const entt::entity entity : ecs.view<world::XRAnchor3D>()) {
        if (!ecs.all_of<world::Transform3D>(entity)) {
            missing.push_back(entity);
        }
    }
    for (const entt::entity entity : ecs.view<world::XRCamera3D>()) {
        if (!ecs.all_of<world::Transform3D>(entity)) {
            missing.push_back(entity);
        }
    }
    for (const entt::entity entity : missing) {
        ecs.emplace_or_replace<world::Transform3D>(entity);
    }

    const world::Tree3D tree(world);
    const std::vector<Signal> fired = std::exchange(data.signals, {});
    std::map<std::string, NodeState, std::less<>> seen;
    for (const entt::entity entity : xr_nodes(world, tree)) {
        const std::optional<NodeBinding> bound = binding(world, entity);
        if (!bound) {
            continue;
        }
        const bool controller = ecs.all_of<world::XRController3D>(entity);
        const world::XRNode3D* node = node_of(world, entity);
        const std::string key = key_of(world, entity);
        const std::string id(world.id_of(entity));
        NodeState state;
        if (const auto found = data.nodes.find(key); found != data.nodes.end()) {
            state = found->second;
        }
        const auto set_tracking = [&](bool tracking) {
            if (tracking == state.tracking) {
                return;
            }
            state.tracking = tracking;
            if (node != nullptr) {
                json::Value event;
                event["tracking"] = tracking;
                world.emit("tracking_changed", id, std::move(event));
                if (node->show_when_tracked) {
                    ecs.get<world::Transform3D>(entity).visible = tracking;
                }
            }
        };
        const auto place = [&](const Pose& pose) {
            auto& transform = ecs.get<world::Transform3D>(entity);
            const world::Transform3D moved = placed(transform, adjusted_transform(world, pose));
            if (!(moved == transform)) {
                transform = moved;
            }
        };
        const auto tracker = data.trackers.find(bound->tracker);
        if (tracker == data.trackers.end()) {
            // _unbind_tracker
            state.generation = 0;
            set_tracking(false);
            state.tracker = bound->tracker;
            state.pose = bound->pose;
            seen.emplace(key, state);
            continue;
        }
        const std::uint64_t generation = data.generations[bound->tracker];
        if (state.generation != generation || state.tracker != bound->tracker ||
            state.pose != bound->pose) {
            // _bind_tracker: the pose as it is now.
            state.generation = generation;
            state.tracker = bound->tracker;
            state.pose = bound->pose;
            const auto pose = tracker->second.poses.find(bound->pose);
            if (pose != tracker->second.poses.end()) {
                set_tracking(pose->second.has_tracking_data);
                place(pose->second);
            } else {
                set_tracking(false);
            }
            if (node != nullptr && node->show_when_tracked) {
                ecs.get<world::Transform3D>(entity).visible = state.tracking;
            }
        }
        for (const Signal& signal : fired) {
            if (signal.tracker != bound->tracker) {
                continue;
            }
            switch (signal.kind) {
            case Signal::Kind::PoseChanged:
                if (signal.name == bound->pose) {
                    const auto pose = tracker->second.poses.find(signal.name);
                    if (pose != tracker->second.poses.end()) {
                        place(pose->second);
                        set_tracking(pose->second.has_tracking_data);
                    }
                }
                break;
            case Signal::Kind::PoseLost:
                if (signal.name == bound->pose) {
                    set_tracking(false);
                }
                break;
            case Signal::Kind::ButtonPressed:
            case Signal::Kind::ButtonReleased:
                if (controller) {
                    json::Value event;
                    event["name"] = signal.name;
                    world.emit(signal.kind == Signal::Kind::ButtonPressed ? "button_pressed"
                                                                          : "button_released",
                               id, std::move(event));
                }
                break;
            case Signal::Kind::FloatChanged:
            case Signal::Kind::Vector2Changed:
                if (controller) {
                    json::Value event;
                    event["name"] = signal.name;
                    event["value"] = input_json(signal.value);
                    world.emit(signal.kind == Signal::Kind::FloatChanged ? "input_float_changed"
                                                                         : "input_vector2_changed",
                               id, std::move(event));
                }
                break;
            case Signal::Kind::ProfileChanged:
                if (controller) {
                    json::Value event;
                    event["role"] = signal.name;
                    world.emit("profile_changed", id, std::move(event));
                }
                break;
            }
        }
        seen.emplace(key, state);
    }
    data.nodes = std::move(seen);
}

void Server::run(world::World& world) {
    {
        const world::Tree3D tree(world);
        const entt::entity origin = current_origin(world, tree);
        if (origin != entt::null) {
            data.world_origin = tree.global(origin);
            data.world_scale = world.ecs().get<world::XROrigin3D>(origin).world_scale;
        }
    }
    // XRServer::_process
    for (const std::shared_ptr<Interface>& interface : interfaces) {
        if (interface->is_initialized()) {
            interface->process(world);
        }
    }
    route(world);
}

void Server::emit(world::World& world, std::string kind, json::Value event) const {
    if (!quiet) {
        world.emit(std::move(kind), {}, std::move(event));
    }
}

void Server::signal(Signal signal) {
    data.signals.push_back(std::move(signal));
}

} // namespace detail

using detail::Server;
using detail::server;
using detail::server_of;

void install(world::World& world, const Settings& settings) {
    auto& ctx = world.ecs().ctx();
    if (auto* existing = ctx.find<std::shared_ptr<Server>>()) {
        (*existing)->data.settings = settings;
        return;
    }
    auto made = std::make_shared<Server>();
    made->data.settings = settings;
    ctx.emplace<std::shared_ptr<Server>>(made);
    Server* raw = made.get();
    // Replays start from the trackers the interfaces start with.
    world.add_reset([raw](world::World& w) {
        const Settings kept = raw->data.settings;
        raw->data = detail::Data{};
        raw->data.settings = kept;
        for (const std::shared_ptr<Interface>& interface : raw->interfaces) {
            interface->reset(w);
        }
        raw->data.signals.clear();
    });
    world.add_memory_source("xr", [raw](const world::World& /*world*/) {
        world::MemoryPart part{"xr", 0, {}, {}};
        std::uint64_t poses = 0;
        for (const auto& [name, tracker] : raw->data.trackers) {
            part.bytes +=
                world::kMapNodeBytes<decltype(raw->data.trackers)> + json::heap_bytes(name) +
                (tracker.poses.size() * world::kMapNodeBytes<decltype(tracker.poses)>)+(
                    tracker.inputs.size() * world::kMapNodeBytes<decltype(tracker.inputs)>);
            poses += tracker.poses.size();
        }
        part.counts = {{"trackers", raw->data.trackers.size()},
                       {"poses", poses},
                       {"nodes", raw->data.nodes.size()}};
        return part;
    });
    world.add_system(
        "xr", [raw](world::World& w, double /*tick_seconds*/) { raw->run(w); },
        {.doc = "Has each XR interface update its trackers and places the XR nodes by their "
                "poses.",
         .priority = -850});
}

const Settings& settings(const world::World& world) {
    static const Settings kDefault;
    const Server* found = server_of(world);
    return found != nullptr ? found->data.settings : kDefault;
}

float world_scale(const world::World& world) {
    const Server* found = server_of(world);
    return found != nullptr ? found->data.world_scale : 1.0F;
}

void set_world_scale(world::World& world, float scale) {
    server(world).data.world_scale = scale;
}

world::Affine3D world_origin(const world::World& world) {
    const Server* found = server_of(world);
    return found != nullptr ? found->data.world_origin : world::Affine3D{};
}

world::Affine3D reference_frame(const world::World& world) {
    const Server* found = server_of(world);
    return found != nullptr ? found->data.reference_frame : world::Affine3D{};
}

void clear_reference_frame(world::World& world) {
    server(world).data.reference_frame = {};
    world.emit("reference_frame_changed");
}

void center_on_hmd(world::World& world, RotationMode rotation_mode, bool keep_height) {
    Server& s = server(world);
    if (s.primary == nullptr) {
        return;
    }
    if (s.primary->play_area_mode() == PlayArea::Stage) {
        // center_on_hmd is not available in this mode
        return;
    }
    // clear our current reference frame or we'll end up double adjusting it
    s.data.reference_frame = {};
    // requesting our EYE_MONO transform should return our current HMD position
    world::Affine3D frame = s.primary->camera_transform(world);
    if (rotation_mode == RotationMode::ResetButKeepTilt) {
        // take the Y out of our Z
        frame.z = normalized({frame.z.x, 0.0F, frame.z.z});
        // Y is straight up
        frame.y = {0.0F, 1.0F, 0.0F};
        // and X is our cross reference
        frame.x = normalized(cross(frame.y, frame.z));
    } else if (rotation_mode == RotationMode::DontResetRotation) {
        // remove our rotation, we're only interesting in centering on position
        frame.x = {1.0F, 0.0F, 0.0F};
        frame.y = {0.0F, 1.0F, 0.0F};
        frame.z = {0.0F, 0.0F, 1.0F};
    }
    // don't negate our height
    if (keep_height) {
        frame.origin.y = 0.0F;
    }
    s.data.reference_frame = frame.inverse();
    world.emit("reference_frame_changed");
}

world::Affine3D hmd_transform(const world::World& world) {
    const Server* found = server_of(world);
    if (found == nullptr || found->primary == nullptr) {
        return {};
    }
    return found->primary->camera_transform(world);
}

void add_interface(world::World& world, std::shared_ptr<Interface> interface) {
    Server& s = server(world);
    const std::string name(interface->name());
    if (find_interface(world, name) != nullptr) {
        return;
    }
    s.interfaces.push_back(std::move(interface));
    json::Value event;
    event["interface"] = name;
    s.emit(world, "interface_added", std::move(event));
}

void remove_interface(world::World& world, std::string_view name) {
    Server& s = server(world);
    const auto found = std::find_if(s.interfaces.begin(), s.interfaces.end(),
                                    [&](const auto& each) { return each->name() == name; });
    if (found == s.interfaces.end()) {
        return;
    }
    if (s.primary == found->get()) {
        s.primary = nullptr;
    }
    s.interfaces.erase(found);
    json::Value event;
    event["interface"] = name;
    world.emit("interface_removed", {}, std::move(event));
}

Interface* find_interface(const world::World& world, std::string_view name) {
    const Server* s = server_of(world);
    if (s == nullptr) {
        return nullptr;
    }
    for (const std::shared_ptr<Interface>& interface : s->interfaces) {
        if (interface->name() == name) {
            return interface.get();
        }
    }
    return nullptr;
}

std::vector<Interface*> interfaces(const world::World& world) {
    std::vector<Interface*> out;
    if (const Server* s = server_of(world)) {
        for (const std::shared_ptr<Interface>& interface : s->interfaces) {
            out.push_back(interface.get());
        }
    }
    return out;
}

Interface* primary_interface(const world::World& world) {
    const Server* s = server_of(world);
    return s != nullptr ? s->primary : nullptr;
}

void set_primary_interface(world::World& world, Interface* interface) {
    server(world).primary = interface;
}

void add_tracker(world::World& world, Tracker tracker) {
    Server& s = server(world);
    const std::string name = tracker.name;
    const TrackerType type = tracker.type;
    const bool replaced = s.data.trackers.contains(name);
    s.data.trackers.insert_or_assign(name, std::move(tracker));
    s.data.generations[name] = s.data.next_generation++;
    json::Value event;
    event["tracker"] = name;
    event["type"] = tracker_type_name(type);
    s.emit(world, replaced ? "tracker_updated" : "tracker_added", std::move(event));
}

void remove_tracker(world::World& world, std::string_view name) {
    Server& s = server(world);
    const auto found = s.data.trackers.find(name);
    if (found == s.data.trackers.end()) {
        return;
    }
    json::Value event;
    event["tracker"] = name;
    event["type"] = tracker_type_name(found->second.type);
    s.data.trackers.erase(found);
    if (const auto generation = s.data.generations.find(name);
        generation != s.data.generations.end()) {
        s.data.generations.erase(generation);
    }
    world.emit("tracker_removed", {}, std::move(event));
}

const Tracker* find_tracker(const world::World& world, std::string_view name) {
    const Server* s = server_of(world);
    if (s == nullptr) {
        return nullptr;
    }
    const auto found = s->data.trackers.find(name);
    return found != s->data.trackers.end() ? &found->second : nullptr;
}

std::vector<const Tracker*> trackers(const world::World& world, std::uint32_t type_mask) {
    std::vector<const Tracker*> out;
    if (const Server* s = server_of(world)) {
        for (const auto& [name, tracker] : s->data.trackers) {
            if ((static_cast<std::uint32_t>(tracker.type) & type_mask) != 0U) {
                out.push_back(&tracker);
            }
        }
    }
    return out;
}

void set_pose(world::World& world, std::string_view tracker, std::string_view name,
              const world::Affine3D& transform, Vec3 linear_velocity, Vec3 angular_velocity,
              Confidence confidence) {
    Server& s = server(world);
    const auto found = s.data.trackers.find(tracker);
    if (found == s.data.trackers.end()) {
        return;
    }
    Pose& pose = found->second.poses[std::string(name)];
    pose.name = name;
    pose.has_tracking_data = true;
    pose.transform = transform;
    pose.linear_velocity = linear_velocity;
    pose.angular_velocity = angular_velocity;
    pose.confidence = confidence;
    s.signal({.kind = detail::Signal::Kind::PoseChanged,
              .tracker = std::string(tracker),
              .name = std::string(name),
              .value = false});
}

void invalidate_pose(world::World& world, std::string_view tracker, std::string_view name) {
    Server& s = server(world);
    const auto found = s.data.trackers.find(tracker);
    if (found == s.data.trackers.end()) {
        return;
    }
    const auto pose = found->second.poses.find(name);
    if (pose == found->second.poses.end() || !pose->second.has_tracking_data) {
        return;
    }
    pose->second.has_tracking_data = false;
    s.signal({.kind = detail::Signal::Kind::PoseLost,
              .tracker = std::string(tracker),
              .name = std::string(name),
              .value = false});
}

void set_input(world::World& world, std::string_view tracker, std::string_view name, Input value) {
    Server& s = server(world);
    const auto found = s.data.trackers.find(tracker);
    if (found == s.data.trackers.end()) {
        return;
    }
    auto& inputs = found->second.inputs;
    const auto existing = inputs.find(name);
    if (existing != inputs.end() && existing->second == value) {
        return;
    }
    inputs.insert_or_assign(std::string(name), value);
    detail::Signal::Kind kind = detail::Signal::Kind::FloatChanged;
    if (const bool* pressed = std::get_if<bool>(&value)) {
        kind =
            *pressed ? detail::Signal::Kind::ButtonPressed : detail::Signal::Kind::ButtonReleased;
    } else if (std::holds_alternative<Vec2>(value)) {
        kind = detail::Signal::Kind::Vector2Changed;
    }
    s.signal(
        {.kind = kind, .tracker = std::string(tracker), .name = std::string(name), .value = value});
}

void set_profile(world::World& world, std::string_view tracker, std::string_view profile) {
    Server& s = server(world);
    const auto found = s.data.trackers.find(tracker);
    if (found == s.data.trackers.end() || found->second.profile == profile) {
        return;
    }
    found->second.profile = profile;
    s.signal({.kind = detail::Signal::Kind::ProfileChanged,
              .tracker = std::string(tracker),
              .name = std::string(profile),
              .value = false});
}

world::Affine3D adjusted_transform(const world::World& world, const Pose& pose) {
    world::Affine3D adjusted = pose.transform;
    // apply world scale
    adjusted.origin = adjusted.origin * world_scale(world);
    // apply reference frame
    return reference_frame(world) * adjusted;
}

std::vector<View> views(const world::World& world) {
    std::vector<View> out;
    const Interface* primary = primary_interface(world);
    if (primary == nullptr || !primary->is_initialized()) {
        return out;
    }
    const world::Affine3D origin = world_origin(world);
    for (std::uint32_t view = 0; view < primary->view_count(); ++view) {
        out.push_back({.transform = primary->transform_for_view(world, view, origin),
                       .fov = primary->fov_for_view(view)});
    }
    return out;
}

std::optional<NodeBinding> binding(const world::World& world, entt::entity node) {
    if (!world.valid(node)) {
        return std::nullopt;
    }
    if (const world::XRNode3D* found = node_of(world, node)) {
        return NodeBinding{found->tracker, found->pose};
    }
    if (world.ecs().all_of<world::XRCamera3D>(node)) {
        return NodeBinding{"head", "default"};
    }
    return std::nullopt;
}

bool has_tracking_data(const world::World& world, entt::entity node) {
    const Server* s = server_of(world);
    if (s == nullptr || !world.valid(node)) {
        return false;
    }
    const auto found = s->data.nodes.find(key_of(world, node));
    return found != s->data.nodes.end() && found->second.tracking;
}

const Pose* pose_of(const world::World& world, entt::entity node) {
    const std::optional<NodeBinding> bound = binding(world, node);
    if (!bound) {
        return nullptr;
    }
    const Tracker* tracker = find_tracker(world, bound->tracker);
    if (tracker == nullptr) {
        return nullptr;
    }
    const auto found = tracker->poses.find(bound->pose);
    return found != tracker->poses.end() ? &found->second : nullptr;
}

std::optional<Input> get_input(const world::World& world, entt::entity node,
                               std::string_view name) {
    const std::optional<NodeBinding> bound = binding(world, node);
    if (!bound) {
        return std::nullopt;
    }
    const Tracker* tracker = find_tracker(world, bound->tracker);
    if (tracker == nullptr) {
        return std::nullopt;
    }
    const auto found = tracker->inputs.find(name);
    return found != tracker->inputs.end() ? std::optional(found->second) : std::nullopt;
}

bool is_button_pressed(const world::World& world, entt::entity node, std::string_view name) {
    const std::optional<Input> input = get_input(world, node, name);
    if (!input) {
        return false;
    }
    // Variant's booleanize: anything but zero.
    if (const bool* pressed = std::get_if<bool>(&*input)) {
        return *pressed;
    }
    if (const float* value = std::get_if<float>(&*input)) {
        return *value != 0.0F;
    }
    return std::get<Vec2>(*input) != Vec2{};
}

float get_float(const world::World& world, entt::entity node, std::string_view name) {
    const std::optional<Input> input = get_input(world, node, name);
    if (!input) {
        return 0.0F;
    }
    if (const bool* pressed = std::get_if<bool>(&*input)) {
        return *pressed ? 1.0F : 0.0F;
    }
    if (const float* value = std::get_if<float>(&*input)) {
        return *value;
    }
    return 0.0F;
}

Vec2 get_vector2(const world::World& world, entt::entity node, std::string_view name) {
    const std::optional<Input> input = get_input(world, node, name);
    if (!input) {
        return {};
    }
    if (const bool* pressed = std::get_if<bool>(&*input)) {
        return {*pressed ? 1.0F : 0.0F, 0.0F};
    }
    if (const float* value = std::get_if<float>(&*input)) {
        return {*value, 0.0F};
    }
    return std::get<Vec2>(*input);
}

Hand tracker_hand(const world::World& world, entt::entity node) {
    const std::optional<NodeBinding> bound = binding(world, node);
    const Tracker* tracker = bound ? find_tracker(world, bound->tracker) : nullptr;
    return tracker != nullptr ? tracker->hand : Hand::Unknown;
}

void trigger_haptic_pulse(world::World& world, entt::entity node, std::string_view action_name,
                          double frequency, double amplitude, double duration, double delay) {
    const std::optional<NodeBinding> bound = binding(world, node);
    Interface* primary = primary_interface(world);
    if (!bound || primary == nullptr) {
        return;
    }
    primary->trigger_haptic_pulse(world, action_name, bound->tracker, frequency, amplitude,
                                  duration, delay);
}

std::vector<Problem> check_nodes(const world::World& world) {
    std::vector<Problem> problems;
    const auto& ecs = world.ecs();
    const world::Tree3D tree(world);
    const auto parent_is_origin = [&](entt::entity entity) {
        const entt::entity parent = tree.parent(entity);
        return parent != entt::null && ecs.all_of<world::XROrigin3D>(parent);
    };
    for (const entt::entity entity : tree.order()) {
        if (ecs.all_of<world::XROrigin3D>(entity)) {
            bool camera = false;
            for (const entt::entity child : ecs.get<world::Transform3D>(entity).children) {
                camera = camera || (world.valid(child) && ecs.all_of<world::XRCamera3D>(child));
            }
            if (!camera) {
                problems.push_back({entity, "/components/XROrigin3D",
                                    "an XROrigin3D needs an XRCamera3D among its children"});
            }
        }
        if (ecs.all_of<world::XRCamera3D>(entity)) {
            if (!ecs.all_of<world::Camera3D>(entity)) {
                problems.push_back({entity, "/components/XRCamera3D",
                                    "an XRCamera3D needs a Camera3D on the same entity"});
            }
            if (!parent_is_origin(entity)) {
                problems.push_back({entity, "/components/XRCamera3D",
                                    "an XRCamera3D may not work as expected without an "
                                    "XROrigin3D as its parent"});
            }
        }
        if (const world::XRNode3D* node = node_of(world, entity)) {
            const std::string type(node_type(world, entity));
            if (!parent_is_origin(entity)) {
                problems.push_back({entity, "/components/" + type,
                                    "an " + type +
                                        " may not work as expected without an XROrigin3D as "
                                        "its parent"});
            }
            if (node->tracker.empty()) {
                problems.push_back(
                    {entity, "/components/" + type + "/tracker", "no tracker name is set"});
            }
            if (node->pose.empty()) {
                problems.push_back({entity, "/components/" + type + "/pose", "no pose is set"});
            }
        }
    }
    return problems;
}

json::Value state_json(const world::World& world, const std::vector<std::string>& names) {
    json::Value out;
    const Server* s = server_of(world);
    if (s == nullptr) {
        return out;
    }
    json::Array listed;
    for (const std::shared_ptr<Interface>& interface : s->interfaces) {
        json::Value entry;
        entry["name"] = interface->name();
        entry["initialized"] = interface->is_initialized();
        if (s->primary == interface.get()) {
            entry["primary"] = true;
        }
        if (interface->is_initialized()) {
            json::Array capabilities;
            const std::uint32_t bits = interface->capabilities();
            constexpr std::array<std::pair<std::uint32_t, std::string_view>, 6> kNames{
                {{kMono, "mono"},
                 {kStereo, "stereo"},
                 {kQuad, "quad"},
                 {kVr, "vr"},
                 {kAr, "ar"},
                 {kExternal, "external"}}};
            for (const auto& [bit, name] : kNames) {
                if ((bits & bit) != 0U) {
                    capabilities.emplace_back(name);
                }
            }
            entry["capabilities"] = std::move(capabilities);
            entry["tracking_status"] = tracking_status_name(interface->tracking_status());
            entry["play_area_mode"] = play_area_name(interface->play_area_mode());
            json::Array area;
            for (const Vec3 corner : interface->play_area(world)) {
                area.push_back(vec3_json(corner));
            }
            if (!area.empty()) {
                entry["play_area"] = std::move(area);
            }
            entry["environment_blend_mode"] = blend_mode_name(interface->environment_blend_mode());
            if (const OpenXRInfo* openxr = openxr_info(*interface); openxr != nullptr) {
                json::Value runtime;
                runtime["runtime"] = openxr->runtime_name;
                runtime["runtime_version"] = openxr->runtime_version;
                runtime["api_version"] = openxr->api_version;
                runtime["system"] = openxr->system_name;
                runtime["session_state"] = session_state_name(openxr->session_state);
                runtime["running"] = openxr->running;
                runtime["headless"] = openxr->headless;
                runtime["reference_space"] = openxr->reference_space;
                runtime["frames"] = openxr->frames;
                entry["openxr"] = std::move(runtime);
            }
            entry["view_count"] = static_cast<std::int64_t>(interface->view_count());
            const Vec2i target = interface->render_target_size();
            entry["render_target_size"] = json::Array{target.x, target.y};
        }
        listed.push_back(std::move(entry));
    }
    out["interfaces"] = std::move(listed);
    if (s->data.world_scale != 1.0F) {
        out["world_scale"] = s->data.world_scale;
    }
    if (!is_identity(s->data.world_origin)) {
        put_transform(out["world_origin"], s->data.world_origin);
    }
    if (!is_identity(s->data.reference_frame)) {
        put_transform(out["reference_frame"], s->data.reference_frame);
    }
    json::Object trackers_out;
    for (const auto& [name, tracker] : s->data.trackers) {
        if (!names.empty() && std::find(names.begin(), names.end(), name) == names.end()) {
            continue;
        }
        json::Value entry;
        entry["type"] = tracker_type_name(tracker.type);
        if (tracker.hand != Hand::Unknown) {
            entry["hand"] = hand_name(tracker.hand);
        }
        if (!tracker.profile.empty()) {
            entry["profile"] = tracker.profile;
        }
        json::Object poses;
        for (const auto& [pose_name, pose] : tracker.poses) {
            json::Value p;
            put_transform(p, pose.transform);
            if (!pose.has_tracking_data) {
                p["tracking"] = false;
            }
            if (pose.confidence != Confidence::High && pose.has_tracking_data) {
                p["confidence"] = confidence_name(pose.confidence);
            }
            if (pose.linear_velocity != Vec3{}) {
                p["linear_velocity"] = vec3_json(pose.linear_velocity);
            }
            if (pose.angular_velocity != Vec3{}) {
                p["angular_velocity"] = vec3_json(pose.angular_velocity);
            }
            poses.emplace(pose_name, std::move(p));
        }
        if (!poses.empty()) {
            entry["poses"] = std::move(poses);
        }
        json::Object inputs;
        for (const auto& [input_name, value] : tracker.inputs) {
            inputs.emplace(input_name, input_json(value));
        }
        if (!inputs.empty()) {
            entry["inputs"] = std::move(inputs);
        }
        trackers_out.emplace(name, std::move(entry));
    }
    out["trackers"] = std::move(trackers_out);
    json::Object nodes;
    const world::Tree3D tree(world);
    for (const entt::entity entity : xr_nodes(world, tree)) {
        const std::optional<NodeBinding> bound = binding(world, entity);
        if (!bound || (!names.empty() &&
                       std::find(names.begin(), names.end(), bound->tracker) == names.end())) {
            continue;
        }
        json::Value entry;
        entry["tracker"] = bound->tracker;
        if (bound->pose != "default") {
            entry["pose"] = bound->pose;
        }
        entry["tracking"] = has_tracking_data(world, entity);
        nodes.emplace(key_of(world, entity), std::move(entry));
    }
    if (!nodes.empty()) {
        out["nodes"] = std::move(nodes);
    }
    if (const TestInterface* test = test_interface(world);
        test != nullptr && !test->pulses().empty()) {
        json::Array pulses;
        for (const HapticPulse& pulse : test->pulses()) {
            json::Value entry;
            entry["action"] = pulse.action;
            entry["tracker"] = pulse.tracker;
            entry["frequency"] = pulse.frequency;
            entry["amplitude"] = pulse.amplitude;
            entry["duration"] = pulse.duration;
            if (pulse.delay != 0.0) {
                entry["delay"] = pulse.delay;
            }
            entry["tick"] = pulse.tick;
            pulses.push_back(std::move(entry));
        }
        out["pulses"] = std::move(pulses);
    }
    json::Array problems;
    for (const Problem& problem : check_nodes(world)) {
        json::Value entry;
        entry["id"] = key_of(world, problem.entity);
        entry["path"] = problem.path;
        entry["message"] = problem.message;
        problems.push_back(std::move(entry));
    }
    if (!problems.empty()) {
        out["problems"] = std::move(problems);
    }
    return out;
}

namespace {

struct Saved {
    detail::Data data;
    std::optional<TestInterface::State> test;
};

} // namespace

Snapshot save(const world::World& world) {
    const Server* s = server_of(world);
    if (s == nullptr) {
        return {};
    }
    auto saved = std::make_shared<Saved>();
    saved->data = s->data;
    if (const TestInterface* test = test_interface(world)) {
        saved->test = test->state();
    }
    return {std::move(saved)};
}

void restore(world::World& world, const Snapshot& snapshot) {
    const auto* saved = static_cast<const Saved*>(snapshot.data.get());
    Server* s = server_of(world);
    if (saved == nullptr || s == nullptr) {
        return;
    }
    s->data = saved->data;
    if (TestInterface* test = test_interface(world); test != nullptr && saved->test) {
        test->set_state(*saved->test);
        if (test->is_initialized() && s->primary == nullptr) {
            s->primary = test;
        } else if (!test->is_initialized() && s->primary == test) {
            s->primary = nullptr;
        }
    }
}

} // namespace slash::xr
