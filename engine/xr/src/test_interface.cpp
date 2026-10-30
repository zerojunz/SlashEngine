#include "internal.hpp"

#include <algorithm>
#include <utility>

namespace slash::xr {
namespace {

constexpr float kStandingHeight = 1.6F;
// Where the hands rest from the head: forward and down, a shoulder's width apart.
constexpr Vec3 kLeftHand{-0.2F, -0.4F, -0.3F};
constexpr Vec3 kRightHand{0.2F, -0.4F, -0.3F};
// The poses Godot's default action map gives each controller.
constexpr std::array<std::string_view, 4> kHandPoses{"default", "aim", "grip", "palm"};

world::Affine3D at(Vec3 position) {
    world::Affine3D transform;
    transform.origin = position;
    return transform;
}

TestTracker hand(Hand side, std::string_view description, Vec3 head, Vec3 offset) {
    TestTracker tracker;
    tracker.type = TrackerType::Controller;
    tracker.hand = side;
    tracker.description = description;
    for (const std::string_view pose : kHandPoses) {
        tracker.poses[std::string(pose)].transform = at(head + offset);
    }
    return tracker;
}

PlayArea play_area_of(std::string_view reference_space) {
    // OpenXRInterface::get_play_area_mode
    if (reference_space == "local") {
        return PlayArea::Sitting;
    }
    if (reference_space == "local_floor") {
        return PlayArea::Roomscale;
    }
    return PlayArea::Stage;
}

} // namespace

std::uint32_t TestInterface::capabilities() const {
    // As OpenXRInterface: virtual reality in the view configuration the settings ask for, and
    // augmented reality as it can blend with the room.
    return kVr | kAr | (state_.stereo ? kStereo : kMono);
}

Result<void> TestInterface::initialize(world::World& world) {
    if (state_.initialized) {
        return {};
    }
    const Settings& current = settings(world);
    state_.initialized = true;
    state_.stereo = current.view_configuration != "mono";
    state_.play_area_mode = play_area_of(current.reference_space);
    state_.blend_mode = current.environment_blend_mode;
    if (state_.trackers.empty()) {
        reset_trackers(world);
    }
    // As OpenXRInterface::initialize: the trackers are there at once, their poses come with the
    // first process.
    for (const auto& [name, tracker] : state_.trackers) {
        add_tracker(world, Tracker{.type = tracker.type,
                                   .name = name,
                                   .description = tracker.description,
                                   .profile = tracker.profile,
                                   .hand = tracker.hand,
                                   .poses = {},
                                   .inputs = {}});
        state_.published.push_back(name);
    }
    detail::Server& s = detail::server(world);
    if (s.primary == nullptr) {
        s.primary = this;
    }
    return {};
}

void TestInterface::uninitialize(world::World& world) {
    if (!state_.initialized) {
        return;
    }
    for (const std::string& name : std::exchange(state_.published, {})) {
        remove_tracker(world, name);
    }
    state_.initialized = false;
    detail::Server& s = detail::server(world);
    if (s.primary == this) {
        s.primary = nullptr;
    }
}

void TestInterface::reset(world::World& world) {
    if (!state_.initialized) {
        return;
    }
    state_.initialized = false;
    state_.published.clear();
    state_.pulses.clear();
    state_.trackers.clear();
    (void)initialize(world);
}

void TestInterface::reset_trackers(const world::World& world) {
    const Settings& current = settings(world);
    // A local space starts at the head; the others on the floor under it.
    const Vec3 head{0.0F, current.reference_space == "local" ? 0.0F : kStandingHeight, 0.0F};
    state_.trackers.clear();
    TestTracker hmd;
    hmd.type = TrackerType::Head;
    hmd.description = "HMD tracker";
    hmd.poses["default"].transform = at(head);
    state_.trackers.emplace("head", std::move(hmd));
    state_.trackers.emplace("left_hand", hand(Hand::Left, "Left hand controller", head, kLeftHand));
    state_.trackers.emplace("right_hand",
                            hand(Hand::Right, "Right hand controller", head, kRightHand));
}

void TestInterface::process(world::World& world) {
    if (!state_.initialized) {
        return;
    }
    // Trackers taken away go, new ones come.
    std::vector<std::string> published;
    for (const std::string& name : state_.published) {
        if (state_.trackers.contains(name)) {
            published.push_back(name);
        } else {
            remove_tracker(world, name);
        }
    }
    for (const auto& [name, tracker] : state_.trackers) {
        const Tracker* found = find_tracker(world, name);
        if (found == nullptr || found->type != tracker.type || found->hand != tracker.hand) {
            add_tracker(world, Tracker{.type = tracker.type,
                                       .name = name,
                                       .description = tracker.description,
                                       .profile = tracker.profile,
                                       .hand = tracker.hand,
                                       .poses = {},
                                       .inputs = {}});
            if (std::find(published.begin(), published.end(), name) == published.end()) {
                published.push_back(name);
            }
        }
        set_profile(world, name, tracker.profile);
        for (const auto& [pose_name, pose] : tracker.poses) {
            if (pose.tracking) {
                set_pose(world, name, pose_name, pose.transform, pose.linear_velocity,
                         pose.angular_velocity, pose.confidence);
            } else {
                invalidate_pose(world, name, pose_name);
            }
        }
        for (const auto& [input_name, value] : tracker.inputs) {
            set_input(world, name, input_name, value);
        }
    }
    state_.published = std::move(published);
}

world::Affine3D TestInterface::camera_transform(const world::World& world) const {
    world::Affine3D head;
    if (const auto found = state_.trackers.find("head"); found != state_.trackers.end()) {
        if (const auto pose = found->second.poses.find("default");
            pose != found->second.poses.end()) {
            head = pose->second.transform;
        }
    }
    // scale our origin point of our transform
    head.origin = head.origin * world_scale(world);
    return reference_frame(world) * head;
}

bool TestInterface::supports_play_area_mode(PlayArea mode) const {
    return mode == PlayArea::Sitting || mode == PlayArea::Roomscale || mode == PlayArea::Stage;
}

bool TestInterface::set_play_area_mode(world::World& /*world*/, PlayArea mode) {
    if (!supports_play_area_mode(mode)) {
        return false;
    }
    state_.play_area_mode = mode;
    return true;
}

std::vector<Vec3> TestInterface::play_area(const world::World& world) const {
    // OpenXRInterface::get_play_area
    constexpr std::array<Vec3, 4> kSides{{
        {-0.5F, 0.0F, -0.5F},
        {0.5F, 0.0F, -0.5F},
        {0.5F, 0.0F, 0.5F},
        {-0.5F, 0.0F, 0.5F},
    }};
    std::vector<Vec3> area;
    if (!state_.initialized || state_.play_area_size.x == 0.0F || state_.play_area_size.y == 0.0F) {
        return area;
    }
    const world::Affine3D frame = reference_frame(world);
    for (Vec3 coord : kSides) {
        // Scale it up.
        coord.x *= state_.play_area_size.x;
        coord.z *= state_.play_area_size.y;
        // Now apply our reference.
        area.push_back(frame.apply(coord));
    }
    return area;
}

std::vector<BlendMode> TestInterface::supported_environment_blend_modes() const {
    return {BlendMode::Opaque, BlendMode::Additive, BlendMode::AlphaBlend};
}

bool TestInterface::set_environment_blend_mode(BlendMode mode) {
    state_.blend_mode = mode;
    return true;
}

void TestInterface::trigger_haptic_pulse(world::World& world, std::string_view action_name,
                                         std::string_view tracker_name, double frequency,
                                         double amplitude, double duration, double delay) {
    state_.pulses.push_back({.action = std::string(action_name),
                             .tracker = std::string(tracker_name),
                             .frequency = frequency,
                             .amplitude = amplitude,
                             .duration = duration,
                             .delay = delay,
                             .tick = world.tick()});
    if (state_.pulses.size() > kMaxPulses) {
        state_.pulses.erase(state_.pulses.begin());
    }
}

TestInterface& use_test_interface(world::World& world) {
    detail::Server& s = detail::server(world);
    s.quiet = true;
    if (s.test == nullptr) {
        auto made = std::make_shared<TestInterface>();
        s.test = made.get();
        add_interface(world, std::move(made));
    }
    if (s.data.settings.enabled) {
        (void)s.test->initialize(world);
    }
    s.quiet = false;
    return *s.test;
}

TestInterface* test_interface(const world::World& world) {
    const detail::Server* s = detail::server_of(world);
    return s != nullptr ? s->test : nullptr;
}

} // namespace slash::xr
