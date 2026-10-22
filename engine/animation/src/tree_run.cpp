#include "tree_run.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <deque>
#include <utility>

namespace slash::animation::detail {
namespace {

// Godot's HUGE_LENGTH: longer than any animation, for what has no end.
constexpr double kHugeLength = 31540000.0;

constexpr std::array<std::string_view, 4> kRequests = {"none", "fire", "abort", "fade_out"};
enum class Request : std::uint8_t { None, Fire, Abort, FadeOut };

// How a node passes its own track weights on to an input, as Godot's FilterAction.
enum class Filter : std::uint8_t { Ignore, Pass, Stop, Blend };

// Godot's AnimationNode::NodeTimeInfo.
struct NodeTimeInfo {
    double length = 0.0;
    double position = 0.0;
    double delta = 0.0;
    LoopMode loop = LoopMode::None;
    bool will_end = false;
    bool infinity = false;

    [[nodiscard]] double remain(bool break_loop) const {
        const bool looping = loop != LoopMode::None;
        if ((looping && !break_loop) || infinity) {
            return kHugeLength;
        }
        if (looping && break_loop && will_end) {
            return 0.0;
        }
        const double left = length - position;
        return is_zero_approx(left) ? 0.0 : left;
    }
};

constexpr std::string_view kStart = "Start";
constexpr std::string_view kEnd = "End";

// Godot's AnimationNodeStateMachinePlayback: a state machine's state between ticks, kept in its
// "playback" parameter, and the requests made of it.
struct Playback {
    std::string current;
    std::string fading_from;
    double fading_time = 0.0;
    double fading_pos = 0.0;
    std::vector<std::string> path; // the states a travel goes through, its target last
    bool playing = false;
    TreeCurve current_curve; // the cross-fade's, from the transition to current
    std::string start_request;
    std::string travel_request;
    bool reset_request = false;
    bool reset_request_on_teleport = false;
    bool reset_request_for_fading_from = false;
    bool next_request = false;
    bool stop_request = false;
    bool teleport_request = false;
    // Within a tick only.
    NodeTimeInfo current_nti;
    NodeTimeInfo fading_from_nti;

    [[nodiscard]] bool is_end() const { return current == kEnd && fading_from.empty(); }

    // Godot's _travel_main and _start_main.
    void travel_main(std::string state, bool reset_on_teleport) {
        travel_request = std::move(state);
        reset_request_on_teleport = reset_on_teleport;
        stop_request = false;
    }
    void start_main(std::string state, bool reset) {
        travel_request.clear();
        path.clear();
        reset_request = reset;
        start_request = std::move(state);
        stop_request = false;
    }
};

std::string text_of(const json::Value& value, std::string_view key) {
    const json::Value* found = value.find(key);
    return found != nullptr && found->is_string() ? found->as_string() : std::string();
}

bool bool_of(const json::Value& value, std::string_view key) {
    const json::Value* found = value.find(key);
    return found != nullptr && found->is_bool() && found->as_bool();
}

double number_of(const json::Value& value, std::string_view key) {
    const json::Value* found = value.find(key);
    return found != nullptr && found->is_number() ? found->as_double() : 0.0;
}

Playback playback_of(const json::Value& value) {
    Playback out;
    if (!value.is_object()) {
        return out;
    }
    out.current = text_of(value, "current");
    out.fading_from = text_of(value, "fading_from");
    out.fading_time = number_of(value, "fading_time");
    out.fading_pos = number_of(value, "fading_pos");
    if (const json::Value* path = value.find("path"); path != nullptr && path->is_array()) {
        for (const json::Value& state : path->as_array()) {
            if (state.is_string()) {
                out.path.push_back(state.as_string());
            }
        }
    }
    out.playing = bool_of(value, "playing");
    if (const json::Value* curve = value.find("xfade_curve");
        curve != nullptr && curve->is_array()) {
        for (const json::Value& point : curve->as_array()) {
            if (point.is_array() && point.size() == 2 && point.as_array()[0].is_number() &&
                point.as_array()[1].is_number()) {
                out.current_curve.push_back(
                    {point.as_array()[0].as_double(), point.as_array()[1].as_double()});
            }
        }
    }
    out.start_request = text_of(value, "start_request");
    out.travel_request = text_of(value, "travel_request");
    out.reset_request = bool_of(value, "reset_request");
    out.reset_request_on_teleport = bool_of(value, "reset_request_on_teleport");
    out.reset_request_for_fading_from = bool_of(value, "reset_request_for_fading_from");
    out.next_request = bool_of(value, "next_request");
    out.stop_request = bool_of(value, "stop_request");
    out.teleport_request = bool_of(value, "teleport_request");
    return out;
}

// The playback as its parameter keeps it, each field left out while it has its default.
json::Value json_of(const Playback& playback) {
    json::Object out;
    const auto text = [&](const char* key, const std::string& value) {
        if (!value.empty()) {
            out.emplace(key, value);
        }
    };
    const auto flag = [&](const char* key, bool value) {
        if (value) {
            out.emplace(key, true);
        }
    };
    text("current", playback.current);
    // The cross-fade's time and position count only while it fades.
    if (!playback.fading_from.empty()) {
        out.emplace("fading_from", playback.fading_from);
        if (playback.fading_time != 0.0) {
            out.emplace("fading_time", playback.fading_time);
        }
        if (playback.fading_pos != 0.0) {
            out.emplace("fading_pos", playback.fading_pos);
        }
    }
    if (!playback.path.empty()) {
        json::Array path;
        for (const std::string& state : playback.path) {
            path.emplace_back(state);
        }
        out.emplace("path", std::move(path));
    }
    flag("playing", playback.playing);
    if (!playback.current_curve.empty()) {
        json::Array curve;
        for (const auto& point : playback.current_curve) {
            curve.emplace_back(json::Array{point[0], point[1]});
        }
        out.emplace("xfade_curve", std::move(curve));
    }
    text("start_request", playback.start_request);
    text("travel_request", playback.travel_request);
    flag("reset_request", playback.reset_request);
    flag("reset_request_on_teleport", playback.reset_request_on_teleport);
    flag("reset_request_for_fading_from", playback.reset_request_for_fading_from);
    flag("next_request", playback.next_request);
    flag("stop_request", playback.stop_request);
    flag("teleport_request", playback.teleport_request);
    return out;
}

// Godot's String::split("/"), keeping empty parts.
std::vector<std::string> split_path(std::string_view path) {
    std::vector<std::string> out;
    std::size_t begin = 0;
    while (true) {
        const std::size_t slash = path.find('/', begin);
        if (slash == std::string_view::npos) {
            out.emplace_back(path.substr(begin));
            return out;
        }
        out.emplace_back(path.substr(begin, slash - begin));
        begin = slash + 1;
    }
}

// The first count parts of a split path joined again.
std::string join_path(const std::vector<std::string>& parts, std::size_t count) {
    std::string out;
    for (std::size_t i = 0; i < count; ++i) {
        out += (i == 0 ? "" : "/") + parts[i];
    }
    return out;
}

std::uint64_t mix(std::uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27U)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31U);
}

// A number from 0 to 1 that depends only on what it is given, in place of Godot's Math::randd.
double random_unit(std::uint64_t seed, std::string_view path, double time) {
    std::uint64_t hash = mix(seed);
    for (const char c : path) {
        hash = mix(hash ^ static_cast<unsigned char>(c));
    }
    std::uint64_t bits = 0;
    std::memcpy(&bits, &time, sizeof(bits));
    hash = mix(hash ^ bits);
    return static_cast<double>(hash >> 11U) * (1.0 / 9007199254740992.0);
}

bool approx(Vec2 a, Vec2 b) {
    return is_equal_approx(a.x, b.x) && is_equal_approx(a.y, b.y);
}

float dot(Vec2 a, Vec2 b) {
    return (a.x * b.x) + (a.y * b.y);
}

float cross(Vec2 a, Vec2 b) {
    return (a.x * b.y) - (a.y * b.x);
}

float distance(Vec2 a, Vec2 b) {
    return std::sqrt(dot(a - b, a - b));
}

// Godot's Geometry2D::is_point_in_triangle.
bool in_triangle(Vec2 s, Vec2 a, Vec2 b, Vec2 c) {
    const Vec2 an = a - s;
    const Vec2 bn = b - s;
    const Vec2 cn = c - s;
    const bool orientation = cross(an, bn) > 0.0F;
    if ((cross(bn, cn) > 0.0F) != orientation) {
        return false;
    }
    return (cross(cn, an) > 0.0F) == orientation;
}

// Godot's Geometry2D::get_closest_point_to_segment.
Vec2 closest_on_segment(Vec2 point, Vec2 from, Vec2 to) {
    const Vec2 p = point - from;
    const Vec2 n = to - from;
    const float l2 = dot(n, n);
    if (l2 < 1e-20F) {
        return from;
    }
    const float d = dot(n, p) / l2;
    if (d <= 0.0F) {
        return from;
    }
    if (d >= 1.0F) {
        return to;
    }
    return from + (n * d);
}

// Godot's AnimationNodeBlendSpace2D::_blend_triangle: a point's barycentric weights.
std::array<float, 3> triangle_weights(Vec2 pos, const std::array<Vec2, 3>& points) {
    for (std::size_t i = 0; i < 3; ++i) {
        if (approx(pos, points.at(i))) {
            std::array<float, 3> out{};
            out.at(i) = 1.0F;
            return out;
        }
    }
    const Vec2 v0 = points[1] - points[0];
    const Vec2 v1 = points[2] - points[0];
    const Vec2 v2 = pos - points[0];
    const float d00 = dot(v0, v0);
    const float d01 = dot(v0, v1);
    const float d11 = dot(v1, v1);
    const float d20 = dot(v2, v0);
    const float d21 = dot(v2, v1);
    const float denom = (d00 * d11) - (d01 * d01);
    if (denom == 0.0F) {
        return {1.0F, 0.0F, 0.0F};
    }
    const float v = ((d11 * d20) - (d01 * d21)) / denom;
    const float w = ((d00 * d21) - (d01 * d20)) / denom;
    return {1.0F - v - w, v, w};
}

class TreeRun {
public:
    TreeRun(const TreeContext& context, TreeParameters& parameters)
        : context_(context), parameters_(parameters), nodes_(context.tree.nodes),
          weights_(context.tree.nodes.size()) {}

    TreeTick tick(double delta, bool started) {
        if (nodes_.empty()) {
            return {};
        }
        weights_[0].assign(context_.track_count, 1.0F);
        PlaybackInfo info;
        info.seeked = started;
        info.delta = delta;
        process(0, info, false);
        if (!out_.invalid.empty()) {
            out_.instances.clear();
        }
        // The state machines' playbacks, which Godot keeps as objects in the parameters.
        for (const auto& [index, playback] : playbacks_) {
            set(index, "playback", json_of(playback));
        }
        return std::move(out_);
    }

private:
    [[nodiscard]] const TreeNode& node(int index) const {
        return nodes_[static_cast<std::size_t>(index)];
    }

    [[nodiscard]] json::Value get(int index, std::string_view name) const {
        return parameters_.get(node(index), name);
    }
    [[nodiscard]] double number(int index, std::string_view name) const {
        const json::Value value = get(index, name);
        return value.is_number() ? value.as_double() : 0.0;
    }
    [[nodiscard]] bool flag(int index, std::string_view name) const {
        const json::Value value = get(index, name);
        return value.is_bool() && value.as_bool();
    }
    [[nodiscard]] int integer(int index, std::string_view name) const {
        const json::Value value = get(index, name);
        return value.is_number() ? static_cast<int>(value.as_double()) : -1;
    }
    // As Godot's set_parameter, which does nothing while a node is processed only to test it.
    void set(int index, std::string_view name, json::Value value) {
        if (!testing_) {
            parameters_.set(node(index), name, std::move(value));
        }
    }

    void make_invalid(const std::string& reason) {
        if (!out_.invalid.empty()) {
            out_.invalid += "; ";
        }
        out_.invalid += reason;
    }

    // The name a node has in the blend tree it is in, for messages.
    [[nodiscard]] std::string label(int index) const {
        const TreeNode& n = node(index);
        return n.name.empty() ? std::string(tree_node_type_name(n.type)) : n.name;
    }

    // Godot's AnimationNodeAnimation::get_node_time_info: the animation's length and loop, or
    // the node's custom timeline's.
    [[nodiscard]] NodeTimeInfo animation_time(int index, const Animation* animation) const {
        NodeTimeInfo out;
        if (animation == nullptr) {
            return out;
        }
        const TreeNode& n = node(index);
        if (n.use_custom_timeline) {
            out.length = n.timeline_length;
            out.loop = n.loop_mode;
        } else {
            out.length = animation->length;
            out.loop = animation->loop;
        }
        out.position = number(index, "current_position");
        return out;
    }

    // Godot's AnimationNode::get_node_time_info: what the node stored last tick.
    [[nodiscard]] NodeTimeInfo stored_time(int index) const {
        return {.length = number(index, "current_length"),
                .position = number(index, "current_position"),
                .delta = number(index, "current_delta")};
    }

    // Godot's AnimationNode::process (and AnimationNodeAnimation's): the node's time from where
    // it was, then its own processing, then its time info stored.
    NodeTimeInfo process(int index, const PlaybackInfo& in, bool test_only) {
        const bool outer = testing_;
        testing_ = test_only;
        PlaybackInfo info = in;
        if (in.seeked) {
            if (in.external) {
                info.delta = number(index, "current_position") - in.time;
            }
        } else {
            const bool backward =
                node(index).type == TreeNodeType::Animation && flag(index, "backward");
            info.time = number(index, "current_position") + (backward ? -in.delta : in.delta);
        }
        const NodeTimeInfo out = run(index, info, test_only);
        if (!test_only) {
            set(index, "current_length", out.length);
            set(index, "current_position", out.position);
            set(index, "current_delta", out.delta);
        }
        testing_ = outer;
        return out;
    }

    // Godot's AnimationNode::_blend_node: the child's track weights from this node's, through its
    // filters, then the child processed.
    NodeTimeInfo blend(int index, int child, PlaybackInfo info, Filter filter, bool sync,
                       bool test_only) {
        const std::vector<float>& from = weights_[static_cast<std::size_t>(index)];
        std::vector<float>& to = weights_[static_cast<std::size_t>(child)];
        const std::size_t count = from.size();
        to.resize(count);
        bool any_valid = false;
        const TreeNode& n = node(index);
        if (tree_node_has_filter(n.type) && n.filter_enabled && filter != Filter::Ignore) {
            std::ranges::fill(to, 0.0F);
            for (const int track : context_.filters[static_cast<std::size_t>(index)]) {
                to[static_cast<std::size_t>(track)] = 1.0F;
            }
            for (std::size_t i = 0; i < count; ++i) {
                switch (filter) {
                case Filter::Ignore:
                    break;
                case Filter::Pass: // filtered tracks pass, the rest don't
                    if (to[i] == 0.0F) {
                        continue;
                    }
                    to[i] = from[i] * info.weight;
                    break;
                case Filter::Stop: // filtered tracks don't pass, the rest blend
                    if (to[i] > 0.0F) {
                        continue;
                    }
                    to[i] = from[i] * info.weight;
                    break;
                case Filter::Blend: // filtered tracks blend, the rest pass as they are
                    to[i] = to[i] == 1.0F ? from[i] * info.weight : from[i];
                    break;
                }
                any_valid = any_valid || !is_zero_approx(to[i]);
            }
        } else {
            for (std::size_t i = 0; i < count; ++i) {
                to[i] = from[i] * info.weight;
                any_valid = any_valid || !is_zero_approx(to[i]);
            }
        }
        if (!info.seeked && !sync && !any_valid) {
            info.delta = 0.0;
        }
        return process(child, info, test_only);
    }

    // Godot's AnimationNode::blend_input: the node connected to one of a blend tree node's inputs.
    NodeTimeInfo blend_input(int index, int input, PlaybackInfo info, Filter filter, bool sync,
                             bool test_only) {
        const TreeNode& n = node(index);
        if (input < 0 || static_cast<std::size_t>(input) >= n.inputs.size()) {
            const std::vector<std::string> names = tree_node_input_names(n);
            make_invalid("Nothing connected to input '" +
                         (static_cast<std::size_t>(input) < names.size()
                              ? names[static_cast<std::size_t>(input)]
                              : std::to_string(input)) +
                         "' of node '" + label(index) + "'.");
            return {};
        }
        return blend(index, n.inputs[static_cast<std::size_t>(input)], info, filter, sync,
                     test_only);
    }

    // Godot's AnimationNode::blend_node: one of a blend space's points.
    NodeTimeInfo blend_point(int index, int point, PlaybackInfo info, Filter filter, bool sync,
                             bool test_only) {
        return blend(index, node(index).children[static_cast<std::size_t>(point)], info, filter,
                     sync, test_only);
    }

    NodeTimeInfo run(int index, const PlaybackInfo& info, bool test_only) {
        switch (node(index).type) {
        case TreeNodeType::Animation:
            return run_animation(index, info, test_only);
        case TreeNodeType::BlendTree: {
            // Its output, with the blend tree as the parent.
            PlaybackInfo pi = info;
            pi.weight = 1.0F;
            return blend(index, node(index).children.back(), pi, Filter::Ignore, true, test_only);
        }
        case TreeNodeType::Output: {
            PlaybackInfo pi = info;
            pi.weight = 1.0F;
            return blend_input(index, 0, pi, Filter::Ignore, true, test_only);
        }
        case TreeNodeType::OneShot:
            return run_one_shot(index, info, test_only);
        case TreeNodeType::Add2:
            return run_add2(index, info, test_only);
        case TreeNodeType::Add3:
            return run_add3(index, info, test_only);
        case TreeNodeType::Blend2:
            return run_blend2(index, info, test_only);
        case TreeNodeType::Blend3:
            return run_blend3(index, info, test_only);
        case TreeNodeType::Sub2:
            return run_sub2(index, info, test_only);
        case TreeNodeType::TimeScale: {
            PlaybackInfo pi = info;
            pi.weight = 1.0F;
            if (!pi.seeked) {
                pi.delta *= number(index, "scale");
            }
            return blend_input(index, 0, pi, Filter::Ignore, true, test_only);
        }
        case TreeNodeType::TimeSeek: {
            const double seek = number(index, "seek_request");
            PlaybackInfo pi = info;
            pi.weight = 1.0F;
            if (is_greater_or_equal_approx(seek, 0.0)) {
                pi.time = seek;
                pi.seeked = true;
                pi.external = node(index).explicit_elapse;
                set(index, "seek_request", -1.0);
            }
            return blend_input(index, 0, pi, Filter::Ignore, true, test_only);
        }
        case TreeNodeType::Transition:
            return run_transition(index, info, test_only);
        case TreeNodeType::BlendSpace1D:
            return run_blend_space_1d(index, info, test_only);
        case TreeNodeType::BlendSpace2D:
            return run_blend_space_2d(index, info, test_only);
        case TreeNodeType::StateMachine:
            return run_state_machine(index, info, test_only);
        }
        return {};
    }

    // Godot's AnimationNodeAnimation::_process.
    NodeTimeInfo run_animation(int index, const PlaybackInfo& info, bool test_only) {
        const TreeNode& n = node(index);
        const Animation* animation = context_.animation(n.animation);
        if (animation == nullptr) {
            if (n.parent >= 0 && node(n.parent).type == TreeNodeType::BlendTree) {
                make_invalid("On BlendTree node '" + n.name + "', animation not found: '" +
                             n.animation + "'");
            } else {
                make_invalid("Animation not found: '" + n.animation + "'");
            }
            return {};
        }
        const double anim_size = animation->length;
        const NodeTimeInfo cur_nti = animation_time(index, animation);
        const double cur_len = cur_nti.length;
        double cur_time = info.time;
        double cur_delta = info.delta;
        bool cur_backward = flag(index, "backward");
        const LoopMode cur_loop_mode = cur_nti.loop;
        const double prev_time = cur_nti.position;
        LoopedFlag looped = LoopedFlag::None;
        const bool node_backward = n.play_mode == PlayMode::Backward;
        const bool seek = info.seeked;
        const bool external = info.external;

        // 1. Progress for the node.
        const bool will_end = is_greater_or_equal_approx(cur_time + cur_delta, cur_len);
        const bool is_started = seek && !external && is_zero_approx(cur_time);
        if (is_started && n.advance_on_start) {
            cur_time = cur_delta;
        }
        if (cur_loop_mode != LoopMode::None) {
            if (cur_loop_mode == LoopMode::Linear) {
                if (!is_zero_approx(cur_len)) {
                    cur_time = fposmod(cur_time, cur_len);
                }
                cur_backward = false;
            } else if (!is_zero_approx(cur_len)) {
                if ((is_greater_or_equal_approx(prev_time, 0.0) && is_less_approx(cur_time, 0.0)) ||
                    (is_less_or_equal_approx(prev_time, cur_len) &&
                     is_greater_approx(cur_time, cur_len))) {
                    cur_backward = !cur_backward;
                }
                cur_time = pingpong(cur_time, cur_len);
            }
        } else {
            if (is_less_approx(cur_time, 0.0)) {
                cur_delta += cur_time;
                cur_time = 0.0;
            } else if (is_greater_approx(cur_time, cur_len)) {
                cur_delta += cur_time - cur_len;
                cur_time = cur_len;
            }
            cur_backward = false;
            // An animation that ended doesn't move on.
            if (!is_zero_approx(cur_delta)) {
                if (n.play_mode == PlayMode::Forward) {
                    if (is_greater_or_equal_approx(prev_time, cur_len)) {
                        cur_delta = 0.0;
                    }
                } else if (is_less_or_equal_approx(prev_time, 0.0)) {
                    cur_delta = 0.0;
                }
            }
        }

        // 2. The node's time info, not the animation's.
        const NodeTimeInfo out{.length = cur_len,
                               .position = cur_time,
                               .delta = cur_delta,
                               .loop = cur_loop_mode,
                               .will_end = will_end};

        // 3. Progress for the animation.
        double prev_playback_time = prev_time + n.start_offset;
        double cur_playback_time = cur_time + n.start_offset;
        if (n.stretch_time_scale) {
            const double scale = anim_size / cur_len;
            prev_playback_time *= scale;
            cur_playback_time *= scale;
            cur_delta *= scale;
        }
        if (cur_loop_mode == LoopMode::Linear) {
            if (!is_zero_approx(anim_size)) {
                prev_playback_time = fposmod(prev_playback_time, anim_size);
                cur_playback_time = fposmod(cur_playback_time, anim_size);
                if (is_greater_or_equal_approx(prev_playback_time, 0.0) &&
                    is_less_approx(cur_playback_time, 0.0)) {
                    looped = node_backward ? LoopedFlag::End : LoopedFlag::Start;
                }
                if (is_less_or_equal_approx(prev_playback_time, anim_size) &&
                    is_greater_approx(cur_playback_time, anim_size)) {
                    looped = node_backward ? LoopedFlag::Start : LoopedFlag::End;
                }
            }
        } else if (cur_loop_mode == LoopMode::PingPong) {
            if (!is_zero_approx(anim_size)) {
                if (is_greater_or_equal_approx(fposmod(cur_playback_time, anim_size * 2.0),
                                               anim_size)) {
                    cur_delta = -cur_delta; // for the keys passed on the way back
                }
                prev_playback_time = pingpong(prev_playback_time, anim_size);
                cur_playback_time = pingpong(cur_playback_time, anim_size);
                if (is_greater_or_equal_approx(prev_playback_time, 0.0) &&
                    is_less_approx(cur_playback_time, 0.0)) {
                    looped = node_backward ? LoopedFlag::End : LoopedFlag::Start;
                }
                if (is_less_or_equal_approx(prev_playback_time, anim_size) &&
                    is_greater_approx(cur_playback_time, anim_size)) {
                    looped = node_backward ? LoopedFlag::Start : LoopedFlag::End;
                }
            }
        } else {
            cur_playback_time = std::clamp(cur_playback_time, 0.0, std::max(anim_size, 0.0));
            // Started and finished, as Godot's tree emits them: the first tick seeks to 0.
            if (!test_only) {
                if (is_started) {
                    out_.signals.push_back({"animation_started", n.animation});
                }
                if (is_less_approx(prev_playback_time, anim_size) &&
                    is_greater_or_equal_approx(cur_playback_time, anim_size)) {
                    cur_playback_time = anim_size;
                    out_.signals.push_back({"animation_finished", n.animation});
                }
            }
        }

        if (!test_only) {
            AnimationInstance instance;
            instance.name = n.animation;
            instance.animation = animation;
            instance.info = info;
            instance.info.start = 0.0;
            instance.info.end = cur_len;
            if (n.play_mode == PlayMode::Forward) {
                instance.info.time = cur_playback_time;
                instance.info.delta = cur_delta;
            } else {
                instance.info.time = anim_size - cur_playback_time;
                instance.info.delta = -cur_delta;
            }
            instance.info.weight = 1.0F;
            instance.info.looped = looped;
            instance.track_weights = weights_[static_cast<std::size_t>(index)];
            out_.instances.push_back(std::move(instance));
        }
        set(index, "backward", cur_backward);
        return out;
    }

    // Godot's AnimationNodeOneShot::_process.
    NodeTimeInfo run_one_shot(int index, const PlaybackInfo& info, bool test_only) {
        const TreeNode& n = node(index);
        const json::Value request_value = get(index, "request");
        Request request = Request::None;
        if (request_value.is_string()) {
            const auto at =
                std::ranges::find(kRequests, request_value.as_string()) - kRequests.begin();
            if (at < std::ssize(kRequests)) {
                request = static_cast<Request>(at);
            }
        }
        const bool cur_active = flag(index, "active");
        bool cur_internal_active = flag(index, "internal_active");
        const NodeTimeInfo cur_nti = stored_time(index);
        double cur_time_to_restart = number(index, "time_to_restart");
        double cur_fade_in_remaining = number(index, "fade_in_remaining");
        double cur_fade_out_remaining = number(index, "fade_out_remaining");

        set(index, "request", "none");

        bool is_shooting = true;
        bool clear_remaining_fade = false;
        bool is_fading_out = cur_active && !cur_internal_active;
        const double abs_delta = std::abs(info.delta);
        const bool seek = info.seeked;

        if (is_zero_approx(info.time) && seek && !info.external) {
            clear_remaining_fade = true; // the tree starts over
        }

        bool do_start = request == Request::Fire;
        if (request == Request::Abort) {
            set(index, "internal_active", false);
            set(index, "active", false);
            set(index, "time_to_restart", -1.0);
            is_shooting = false;
        } else if (request == Request::FadeOut && !is_fading_out) {
            if (cur_active) {
                is_fading_out = true;
                cur_fade_out_remaining = n.fadeout_time;
                cur_fade_in_remaining = 0.0;
            } else {
                is_shooting = false; // the shot already ended
            }
            set(index, "internal_active", false);
            set(index, "time_to_restart", -1.0);
        } else if (!do_start && !cur_active) {
            if (is_greater_or_equal_approx(cur_time_to_restart, 0.0) && !seek) {
                cur_time_to_restart -= abs_delta;
                if (is_less_approx(cur_time_to_restart, 0.0)) {
                    do_start = true; // autorestart
                }
                set(index, "time_to_restart", cur_time_to_restart);
            }
            if (!do_start) {
                is_shooting = false;
            }
        }

        bool os_seek = seek;
        if (clear_remaining_fade) {
            os_seek = false;
            cur_fade_out_remaining = 0.0;
            set(index, "fade_out_remaining", 0.0);
            if (is_fading_out) {
                is_fading_out = false;
                set(index, "internal_active", false);
                set(index, "active", false);
            }
        }

        if (!is_shooting) {
            PlaybackInfo pi = info;
            pi.weight = 1.0F;
            return blend_input(index, 0, pi, Filter::Ignore, n.sync, test_only);
        }

        if (do_start) {
            os_seek = true;
            if (!cur_internal_active) {
                cur_fade_in_remaining = n.fadein_time; // a shot already playing doesn't fade in
            }
            cur_internal_active = true;
            set(index, "request", "none");
            set(index, "internal_active", true);
            set(index, "active", true);
        }

        float blend = 1.0F;
        bool use_blend = n.sync;
        if (is_greater_approx(cur_fade_in_remaining, 0.0)) {
            if (is_greater_approx(n.fadein_time, 0.0)) {
                use_blend = true;
                blend = static_cast<float>((n.fadein_time - cur_fade_in_remaining) / n.fadein_time);
                blend = static_cast<float>(sample_curve(n.fadein_curve, blend));
            } else {
                blend = 0.0F;
            }
        }
        if (is_fading_out) {
            use_blend = true;
            if (is_greater_approx(n.fadeout_time, 0.0)) {
                blend = static_cast<float>(cur_fade_out_remaining / n.fadeout_time);
                blend = static_cast<float>(1.0 - sample_curve(n.fadeout_curve, 1.0 - blend));
            } else {
                blend = 0.0F;
            }
        }

        PlaybackInfo pi = info;
        NodeTimeInfo main_nti;
        if (n.mix_mode == MixMode::Add) {
            pi.weight = 1.0F;
            main_nti = blend_input(index, 0, pi, Filter::Ignore, n.sync, test_only);
        } else {
            pi.seeked = pi.seeked && use_blend;
            pi.weight = 1.0F - blend;
            main_nti = blend_input(index, 0, pi, Filter::Blend, n.sync, test_only);
        }

        pi = info;
        if (do_start) {
            pi.time = 0.0;
        } else if (os_seek) {
            pi.time = cur_nti.position;
        }
        pi.seeked = os_seek;
        // Above zero, so the shot's discrete keys at its edges still play.
        pi.weight = is_zero_approx(blend) ? static_cast<float>(kCmpEpsilon) : blend;
        const NodeTimeInfo os_nti = blend_input(index, 1, pi, Filter::Pass, true, test_only);

        if (is_less_or_equal_approx(cur_fade_in_remaining, 0.0) && !do_start && !is_fading_out &&
            is_less_or_equal_approx(os_nti.remain(n.break_loop_at_end), n.fadeout_time)) {
            is_fading_out = true;
            cur_fade_out_remaining = os_nti.remain(n.break_loop_at_end);
            cur_fade_in_remaining = 0.0;
            set(index, "internal_active", false);
        }

        if (!seek) {
            if (is_less_or_equal_approx(os_nti.remain(n.break_loop_at_end), 0.0) ||
                (is_fading_out && is_less_or_equal_approx(cur_fade_out_remaining, 0.0))) {
                set(index, "internal_active", false);
                set(index, "active", false);
                if (n.autorestart) {
                    const double restart =
                        n.autorestart_delay + (random_unit(context_.seed, n.path, info.time) *
                                               n.autorestart_random_delay);
                    set(index, "time_to_restart", restart);
                }
            }
            const double d = std::abs(os_nti.delta);
            if (!do_start) {
                cur_fade_in_remaining = std::max(0.0, cur_fade_in_remaining - d);
            }
            cur_fade_out_remaining = std::max(0.0, cur_fade_out_remaining - d);
        }

        set(index, "fade_in_remaining", cur_fade_in_remaining);
        set(index, "fade_out_remaining", cur_fade_out_remaining);
        return cur_internal_active ? os_nti : main_nti;
    }

    NodeTimeInfo run_add2(int index, const PlaybackInfo& info, bool test_only) {
        const double amount = number(index, "add_amount");
        const bool sync = node(index).sync;
        PlaybackInfo pi = info;
        pi.weight = 1.0F;
        const NodeTimeInfo out = blend_input(index, 0, pi, Filter::Ignore, sync, test_only);
        pi.weight = static_cast<float>(amount);
        blend_input(index, 1, pi, Filter::Pass, sync, test_only);
        return out;
    }

    NodeTimeInfo run_add3(int index, const PlaybackInfo& info, bool test_only) {
        const double amount = number(index, "add_amount");
        const bool sync = node(index).sync;
        PlaybackInfo pi = info;
        pi.weight = static_cast<float>(std::max(0.0, -amount));
        blend_input(index, 0, pi, Filter::Pass, sync, test_only);
        pi.weight = 1.0F;
        const NodeTimeInfo out = blend_input(index, 1, pi, Filter::Ignore, sync, test_only);
        pi.weight = static_cast<float>(std::max(0.0, amount));
        blend_input(index, 2, pi, Filter::Pass, sync, test_only);
        return out;
    }

    NodeTimeInfo run_blend2(int index, const PlaybackInfo& info, bool test_only) {
        const double amount = number(index, "blend_amount");
        const bool sync = node(index).sync;
        PlaybackInfo pi = info;
        pi.weight = static_cast<float>(1.0 - amount);
        const NodeTimeInfo nti0 = blend_input(index, 0, pi, Filter::Blend, sync, test_only);
        pi.weight = static_cast<float>(amount);
        const NodeTimeInfo nti1 = blend_input(index, 1, pi, Filter::Pass, sync, test_only);
        return amount > 0.5 ? nti1 : nti0;
    }

    NodeTimeInfo run_blend3(int index, const PlaybackInfo& info, bool test_only) {
        const double amount = number(index, "blend_amount");
        const bool sync = node(index).sync;
        PlaybackInfo pi = info;
        pi.weight = static_cast<float>(std::max(0.0, -amount));
        const NodeTimeInfo nti0 = blend_input(index, 0, pi, Filter::Ignore, sync, test_only);
        pi.weight = static_cast<float>(1.0 - std::abs(amount));
        const NodeTimeInfo nti1 = blend_input(index, 1, pi, Filter::Ignore, sync, test_only);
        pi.weight = static_cast<float>(std::max(0.0, amount));
        const NodeTimeInfo nti2 = blend_input(index, 2, pi, Filter::Ignore, sync, test_only);
        if (amount > 0.5) {
            return nti2;
        }
        return amount < -0.5 ? nti0 : nti1;
    }

    NodeTimeInfo run_sub2(int index, const PlaybackInfo& info, bool test_only) {
        const double amount = number(index, "sub_amount");
        const bool sync = node(index).sync;
        PlaybackInfo pi = info;
        // out = sub's transform inverted times in's
        pi.weight = static_cast<float>(-amount);
        blend_input(index, 1, pi, Filter::Pass, sync, test_only);
        pi.weight = 1.0F;
        return blend_input(index, 0, pi, Filter::Ignore, sync, test_only);
    }

    // Godot's AnimationNodeTransition::_process.
    NodeTimeInfo run_transition(int index, const PlaybackInfo& info, bool test_only) {
        const TreeNode& n = node(index);
        const int input_count = static_cast<int>(n.transitions.size());
        const auto find_input = [&](const std::string& name) {
            for (int i = 0; i < input_count; ++i) {
                if (n.transitions[static_cast<std::size_t>(i)].name == name) {
                    return i;
                }
            }
            return -1;
        };
        // Godot's pending_update, every tick: an index out of range goes to the first input.
        // Unlike Godot, the first tick already plays it.
        if (const int stored = integer(index, "current_index");
            stored < 0 || stored >= input_count) {
            set(index, "prev_index", -1);
            if (input_count > 0) {
                set(index, "current_index", 0);
                set(index, "current_state", n.transitions[0].name);
            } else {
                set(index, "current_index", -1);
                set(index, "current_state", "");
            }
        } else {
            set(index, "current_state", n.transitions[static_cast<std::size_t>(stored)].name);
        }

        const json::Value request = get(index, "transition_request");
        std::string cur_transition_request = request.is_string() ? request.as_string() : "";
        int cur_current_index = integer(index, "current_index");
        int cur_prev_index = integer(index, "prev_index");
        NodeTimeInfo cur_nti;
        double cur_prev_xfading = number(index, "prev_xfading");

        bool switched = false;
        bool restart = false;
        bool clear_remaining_fade = false;

        if (is_zero_approx(info.time) && info.seeked && !info.external) {
            clear_remaining_fade = true; // the tree starts over
        }

        if (!cur_transition_request.empty()) {
            const int new_index = find_input(cur_transition_request);
            if (new_index >= 0) {
                if (cur_current_index == new_index) {
                    if (n.allow_transition_to_self) {
                        restart = n.transitions[static_cast<std::size_t>(cur_current_index)].reset;
                        clear_remaining_fade = true;
                    }
                } else {
                    switched = true;
                    cur_prev_index = cur_current_index;
                    set(index, "prev_index", cur_current_index);
                    cur_current_index = new_index;
                    set(index, "current_index", cur_current_index);
                    set(index, "current_state", cur_transition_request);
                }
            }
            cur_transition_request.clear();
            set(index, "transition_request", "");
        }

        if (clear_remaining_fade) {
            cur_prev_xfading = 0.0;
            set(index, "prev_xfading", 0.0);
            cur_prev_index = -1;
            set(index, "prev_index", -1);
        }

        PlaybackInfo pi = info;
        if (restart) {
            pi.time = 0.0;
            pi.seeked = true;
            pi.weight = 1.0F;
            return blend_input(index, cur_current_index, pi, Filter::Ignore, true, test_only);
        }
        if (switched) {
            cur_prev_xfading = n.xfade_time;
        }
        if (cur_current_index < 0 || cur_current_index >= input_count ||
            cur_prev_index >= input_count) {
            return {};
        }

        if (n.sync) {
            pi.weight = 0.0F;
            for (int i = 0; i < input_count; ++i) {
                if (i != cur_current_index && i != cur_prev_index) {
                    blend_input(index, i, pi, Filter::Ignore, true, test_only);
                }
            }
        }

        const TransitionInput& current = n.transitions[static_cast<std::size_t>(cur_current_index)];
        if (cur_prev_index < 0) {
            // The current input, going on to the next when it ends.
            pi.weight = 1.0F;
            cur_nti = blend_input(index, cur_current_index, pi, Filter::Ignore, true, test_only);
            if (current.auto_advance &&
                is_less_or_equal_approx(cur_nti.remain(current.break_loop_at_end), n.xfade_time)) {
                set(index, "transition_request",
                    n.transitions[static_cast<std::size_t>((cur_current_index + 1) % input_count)]
                        .name);
            }
        } else {
            // Cross-fading from the previous input to the current one.
            float blend = 0.0F;
            float blend_inv = 1.0F;
            bool use_blend = n.sync;
            if (n.xfade_time > 0.0) {
                use_blend = true;
                blend = static_cast<float>(cur_prev_xfading / n.xfade_time);
                blend = static_cast<float>(sample_curve(n.xfade_curve, blend));
                blend_inv = 1.0F - blend;
                // Above zero, so discrete keys at the edges still play.
                blend = is_zero_approx(blend) ? static_cast<float>(kCmpEpsilon) : blend;
                blend_inv = is_zero_approx(blend_inv) ? static_cast<float>(kCmpEpsilon) : blend_inv;
            }
            pi.weight = blend_inv;
            if (current.reset && !info.seeked && switched) {
                pi.time = 0.0; // just switched: the current input starts over
                pi.seeked = true;
            }
            cur_nti = blend_input(index, cur_current_index, pi, Filter::Ignore, true, test_only);

            pi = info;
            pi.seeked = pi.seeked && use_blend;
            pi.weight = blend;
            blend_input(index, cur_prev_index, pi, Filter::Ignore, true, test_only);
            if (!info.seeked) {
                if (is_less_or_equal_approx(cur_prev_xfading, 0.0)) {
                    set(index, "prev_index", -1);
                }
                cur_prev_xfading -= std::abs(info.delta);
            }
        }
        set(index, "prev_xfading", cur_prev_xfading);
        return cur_nti;
    }

    // As Godot's discrete carry, the new point plays on from where the old one was, a pingpong
    // loop going the same way. (Godot means to copy the way it goes, but its call does nothing.)
    void carry_backward(int from, int to) {
        if (node(from).type == TreeNodeType::Animation &&
            node(to).type == TreeNodeType::Animation) {
            set(to, "backward", flag(from, "backward"));
        }
    }

    // Godot's AnimationNodeBlendSpace1D::_process.
    NodeTimeInfo run_blend_space_1d(int index, const PlaybackInfo& info, bool test_only) {
        const TreeNode& n = node(index);
        const int count = static_cast<int>(n.children.size());
        if (count == 0) {
            return {};
        }
        PlaybackInfo pi = info;
        if (count == 1) {
            pi.weight = 1.0F;
            return blend_point(index, 0, pi, Filter::Ignore, true, test_only);
        }
        const double blend_pos = number(index, "blend_position");
        int cur_closest = integer(index, "closest");
        NodeTimeInfo mind;
        const auto position = [&](int i) { return n.positions[static_cast<std::size_t>(i)].x; };

        if (n.blend_mode == BlendMode::Interpolated) {
            int point_lower = -1;
            float pos_lower = 0.0F;
            int point_higher = -1;
            float pos_higher = 0.0F;
            // The two points either side.
            for (int i = 0; i < count; ++i) {
                const float pos = position(i);
                if (pos <= blend_pos) {
                    if (point_lower == -1 || pos > pos_lower) {
                        point_lower = i;
                        pos_lower = pos;
                    }
                } else if (point_higher == -1 || pos < pos_higher) {
                    point_higher = i;
                    pos_higher = pos;
                }
            }
            std::vector<float> weights(static_cast<std::size_t>(count), 0.0F);
            if (point_lower == -1 && point_higher != -1) {
                weights[static_cast<std::size_t>(point_higher)] = 1.0F; // left of every point
            } else if (point_higher == -1) {
                weights[static_cast<std::size_t>(point_lower)] = 1.0F; // right of every point
            } else {
                const float between = pos_higher - pos_lower;
                const auto in = static_cast<float>(blend_pos - pos_lower);
                const float percentage = in / between;
                weights[static_cast<std::size_t>(point_lower)] = 1.0F - percentage;
                weights[static_cast<std::size_t>(point_higher)] = percentage;
            }
            bool first = true;
            double max_weight = 0.0;
            for (int i = 0; i < count; ++i) {
                if (i == point_lower || i == point_higher) {
                    pi.weight = weights[static_cast<std::size_t>(i)];
                    const NodeTimeInfo t =
                        blend_point(index, i, pi, Filter::Ignore, true, test_only);
                    if (first || pi.weight > max_weight) {
                        max_weight = pi.weight;
                        mind = t;
                        first = false;
                    }
                } else if (n.sync) {
                    pi.weight = 0.0F;
                    blend_point(index, i, pi, Filter::Ignore, true, test_only);
                }
            }
        } else {
            int new_closest = -1;
            double new_closest_dist = 1e20;
            for (int i = 0; i < count; ++i) {
                const double d = std::abs(position(i) - blend_pos);
                if (d < new_closest_dist) {
                    new_closest = i;
                    new_closest_dist = d;
                }
            }
            mind = run_discrete(index, info, pi, cur_closest, new_closest, test_only);
            cur_closest = new_closest == -1 ? cur_closest : new_closest;
        }
        set(index, "closest", cur_closest);
        return mind;
    }

    // The discrete blend modes' switch to the closest point, the same in both blend spaces.
    NodeTimeInfo run_discrete(int index, const PlaybackInfo& info, PlaybackInfo pi, int cur_closest,
                              int new_closest, bool test_only) {
        const TreeNode& n = node(index);
        const int count = static_cast<int>(n.children.size());
        NodeTimeInfo mind;
        int playing = cur_closest;
        if (new_closest != cur_closest && new_closest != -1) {
            if (n.blend_mode == BlendMode::DiscreteCarry && cur_closest >= 0 &&
                cur_closest < count) {
                carry_backward(n.children[static_cast<std::size_t>(cur_closest)],
                               n.children[static_cast<std::size_t>(new_closest)]);
                // Where the old point was.
                pi.seeked = false;
                pi.weight = 0.0F;
                const NodeTimeInfo from =
                    blend_point(index, cur_closest, pi, Filter::Ignore, true, true);
                pi.time = from.position;
            }
            pi.seeked = true;
            pi.weight = 1.0F;
            mind = blend_point(index, new_closest, pi, Filter::Ignore, true, test_only);
            playing = new_closest;
        } else if (cur_closest >= 0 && cur_closest < count) {
            pi.weight = 1.0F;
            mind = blend_point(index, cur_closest, pi, Filter::Ignore, true, test_only);
        }
        if (n.sync) {
            pi = info;
            pi.weight = 0.0F;
            for (int i = 0; i < count; ++i) {
                if (i != playing) {
                    blend_point(index, i, pi, Filter::Ignore, true, test_only);
                }
            }
        }
        return mind;
    }

    // Godot's AnimationNodeBlendSpace2D::_process.
    NodeTimeInfo run_blend_space_2d(int index, const PlaybackInfo& info, bool test_only) {
        const TreeNode& n = node(index);
        const int count = static_cast<int>(n.children.size());
        if (count == 0) {
            return {};
        }
        Vec2 blend_pos;
        if (const json::Value value = get(index, "blend_position");
            value.is_array() && value.size() == 2) {
            blend_pos = {static_cast<float>(value.as_array()[0].as_double()),
                         static_cast<float>(value.as_array()[1].as_double())};
        }
        const int cur_closest = integer(index, "closest");
        NodeTimeInfo mind;
        PlaybackInfo pi = info;
        const auto point = [&](int i) { return n.positions[static_cast<std::size_t>(i)]; };

        if (n.blend_mode == BlendMode::Interpolated) {
            if (n.triangles.empty()) {
                return {};
            }
            Vec2 best_point;
            bool first = true;
            int blend_triangle = -1;
            std::array<float, 3> blend_weights{};
            for (std::size_t i = 0; i < n.triangles.size(); ++i) {
                std::array<Vec2, 3> points{};
                for (std::size_t j = 0; j < 3; ++j) {
                    points.at(j) = point(n.triangles[i].at(j));
                }
                if (in_triangle(blend_pos, points[0], points[1], points[2])) {
                    blend_triangle = static_cast<int>(i);
                    blend_weights = triangle_weights(blend_pos, points);
                    break;
                }
                for (std::size_t j = 0; j < 3; ++j) {
                    const Vec2 a = points.at(j);
                    const Vec2 b = points.at((j + 1) % 3);
                    const Vec2 closest = closest_on_segment(blend_pos, a, b);
                    if (first || distance(closest, blend_pos) < distance(best_point, blend_pos)) {
                        best_point = closest;
                        blend_triangle = static_cast<int>(i);
                        first = false;
                        const float d = distance(a, b);
                        if (d == 0.0F) {
                            blend_weights.at(j) = 1.0F;
                            blend_weights.at((j + 1) % 3) = 0.0F;
                            blend_weights.at((j + 2) % 3) = 0.0F;
                        } else {
                            const float c = distance(a, closest) / d;
                            blend_weights.at(j) = 1.0F - c;
                            blend_weights.at((j + 1) % 3) = c;
                            blend_weights.at((j + 2) % 3) = 0.0F;
                        }
                    }
                }
            }
            if (blend_triangle < 0) {
                return {};
            }
            const std::array<int, 3>& corners =
                n.triangles[static_cast<std::size_t>(blend_triangle)];
            first = true;
            double max_weight = 0.0;
            for (int i = 0; i < count; ++i) {
                bool found = false;
                for (std::size_t j = 0; j < 3; ++j) {
                    if (i == corners.at(j)) {
                        pi.weight = blend_weights.at(j);
                        const NodeTimeInfo t =
                            blend_point(index, i, pi, Filter::Ignore, true, test_only);
                        if (first || pi.weight > max_weight) {
                            mind = t;
                            max_weight = pi.weight;
                            first = false;
                        }
                        found = true;
                        break;
                    }
                }
                if (n.sync && !found) {
                    pi.weight = 0.0F;
                    blend_point(index, i, pi, Filter::Ignore, true, test_only);
                }
            }
            set(index, "closest", cur_closest);
            return mind;
        }
        int new_closest = -1;
        float new_closest_dist = 1e20F;
        for (int i = 0; i < count; ++i) {
            const Vec2 d = point(i) - blend_pos;
            const float distance_squared = dot(d, d);
            if (distance_squared < new_closest_dist) {
                new_closest = i;
                new_closest_dist = distance_squared;
            }
        }
        mind = run_discrete(index, info, pi, cur_closest, new_closest, test_only);
        set(index, "closest", new_closest == -1 ? cur_closest : new_closest);
        return mind;
    }

    // State machines, as Godot's AnimationNodeStateMachine and AnimationNodeStateMachinePlayback.

    // Where a state machine goes next, as Godot's NextInfo.
    struct NextInfo {
        std::string node;
        double xfade = 0.0;
        TreeCurve curve;
        SwitchMode switch_mode = SwitchMode::Immediate;
        bool is_reset = false;
        bool break_loop_at_end = false;
    };

    // A transition as Godot's _check_group_transition finds it: a grouped state machine's
    // transitions from its Start and to its End are those into and out of it in its parent.
    struct GroupTransition {
        const MachineTransition* transition = nullptr;
        int machine = 0; // whose conditions it reads
        bool bypass = false;
    };

    [[nodiscard]] const MachineState* state(int machine, std::string_view name) const {
        for (const MachineState& s : node(machine).states) {
            if (s.name == name) {
                return &s;
            }
        }
        return nullptr;
    }

    [[nodiscard]] bool has_state(int machine, std::string_view name) const {
        return state(machine, name) != nullptr;
    }

    [[nodiscard]] Vec2 position(int machine, std::string_view name) const {
        const MachineState* s = state(machine, name);
        return s != nullptr ? s->position : Vec2{};
    }

    [[nodiscard]] bool has_transition_from(int machine, std::string_view name) const {
        return std::ranges::any_of(node(machine).machine_transitions,
                                   [&](const MachineTransition& t) { return t.from == name; });
    }

    [[nodiscard]] bool is_grouped(int index) const {
        return index >= 0 && node(index).type == TreeNodeType::StateMachine &&
               node(index).state_machine_type == StateMachineType::Grouped;
    }

    // Godot's AnimationNode::find_node_by_path: the node a path of names leads to from a node,
    // or -1. A state machine's Start and End lead nowhere.
    [[nodiscard]] int find_node_by_path(int index, std::string_view path) const {
        int at = index;
        for (const std::string& name : split_path(path)) {
            const TreeNode& n = node(at);
            int next = -1;
            if (n.type == TreeNodeType::StateMachine) {
                if (const MachineState* s = state(at, name); s != nullptr) {
                    next = s->node;
                }
            } else {
                for (const int child : n.children) {
                    if (node(child).name == name) {
                        next = child;
                        break;
                    }
                }
            }
            at = next;
            if (at < 0) {
                break;
            }
        }
        return at;
    }

    // A state machine's playback, read from its parameter the first time a tick needs it.
    Playback& playback(int index) {
        auto it = playbacks_.find(index);
        if (it == playbacks_.end()) {
            it = playbacks_.emplace(index, playback_of(get(index, "playback"))).first;
        }
        return it->second;
    }

    // A grouped state machine's playback for its parent to call: the real one, or, while
    // testing, a copy that goes when copies does, as Godot duplicates it.
    Playback& child_playback(int index, bool test_only, std::deque<Playback>& copies) {
        Playback& real = playback(index);
        if (!test_only) {
            return real;
        }
        copies.push_back(real);
        return copies.back();
    }

    // Godot's AnimationNodeStateMachine::blend_node for a state; Start and End play nothing.
    NodeTimeInfo blend_state(int machine, std::string_view name, const PlaybackInfo& info,
                             bool test_only) {
        const MachineState* s = state(machine, name);
        if (s == nullptr || s->node < 0) {
            return {};
        }
        return blend(machine, s->node, info, Filter::Ignore, true, test_only);
    }

    // Godot's AnimationNodeStateMachine::_process: the requests callers set in its parameters,
    // as calls to Godot's start, travel, next and stop in that order, then its playback.
    NodeTimeInfo run_state_machine(int index, const PlaybackInfo& info, bool test_only) {
        Playback copy;
        Playback* pb = &playback(index);
        if (test_only) {
            copy = *pb; // Don't process the original when testing.
            pb = &copy;
        }
        if (node(index).state_machine_type != StateMachineType::Grouped) {
            if (const json::Value start = get(index, "start");
                start.is_string() && !start.as_string().empty()) {
                pb->start_main(start.as_string(), true);
                set(index, "start", "");
            }
            if (const json::Value travel = get(index, "travel");
                travel.is_string() && !travel.as_string().empty()) {
                pb->travel_main(travel.as_string(), true);
                set(index, "travel", "");
            }
            if (flag(index, "next")) {
                pb->next_request = true;
                set(index, "next", false);
            }
            if (flag(index, "stop")) {
                pb->stop_request = true;
                set(index, "stop", false);
            }
        }
        const NodeTimeInfo out = process_playback(index, *pb, info, test_only);
        pb->start_request.clear();
        pb->next_request = false;
        pb->stop_request = false;
        pb->reset_request_on_teleport = false;
        return out;
    }

    // Godot's _start.
    static void start(Playback& pb) {
        pb.playing = true;
        pb.current = !pb.start_request.empty() ? pb.start_request : std::string(kStart);
        pb.teleport_request = true;
        pb.stop_request = false;
        pb.start_request.clear();
    }

    // Godot's _clear_path_children.
    void clear_path_children(int machine, const Playback& pb, bool test_only) {
        for (const MachineState& s : node(machine).states) {
            if (!is_grouped(s.node)) {
                continue;
            }
            std::deque<Playback> copies;
            Playback& child = child_playback(s.node, test_only, copies);
            child.path.clear();
            clear_path_children(s.node, child, test_only);
            if (pb.current != s.name) {
                start(child); // Can restart.
            }
        }
    }

    // Godot's _start_children: starts the grouped state machines a path goes into on its states.
    void start_children(int machine, Playback& pb, const std::string& path, bool test_only) {
        if (node(machine).state_machine_type == StateMachineType::Grouped) {
            return; // Only the top state machine does this.
        }
        const std::vector<std::string> temp_path = split_path(path);
        if (temp_path.size() <= 1) {
            return;
        }
        for (std::size_t i = 1; i < temp_path.size(); ++i) {
            const int group = find_node_by_path(machine, join_path(temp_path, i));
            if (!is_grouped(group)) {
                return; // A path goes only into grouped state machines.
            }
            std::deque<Playback> copies;
            Playback& child = child_playback(group, test_only, copies);
            child.start_main(temp_path[i], i == temp_path.size() - 1 ? pb.reset_request : false);
        }
        pb.reset_request = false;
    }

    // Godot's _travel_children: the travel paths inside the grouped state machines a path goes
    // into, or, when one has none, a start on each.
    bool travel_children(int machine, Playback& pb, const std::string& path,
                         bool allow_transition_to_self, bool parent_same_state, bool test_only) {
        if (node(machine).state_machine_type == StateMachineType::Grouped) {
            return false; // Only the top state machine does this.
        }
        struct ChildInfo {
            Playback* playback = nullptr;
            std::vector<std::string> path;
            bool is_reset = false;
        };
        const std::vector<std::string> temp_path = split_path(path);
        std::vector<ChildInfo> children;
        std::deque<Playback> copies;
        bool found_route = true;
        bool is_parent_same_state = parent_same_state;
        if (temp_path.size() > 1) {
            for (std::size_t i = 1; i < temp_path.size(); ++i) {
                const std::string concatenated = join_path(temp_path, i);
                const int group = find_node_by_path(machine, concatenated);
                if (!is_grouped(group)) {
                    return false; // A path goes only into grouped state machines.
                }
                Playback& child = child_playback(group, test_only, copies);
                if (!child.playing) {
                    start(child);
                }
                ChildInfo info;
                info.playback = &child;
                // When the parent's state changes, the child's groups travel to their ends and
                // it starts over.
                bool child_found_route = true;
                const bool is_current_same_state = temp_path[i] == child.current;
                if (!is_parent_same_state) {
                    std::string child_path = "/" + child.current;
                    while (true) {
                        const int inner = find_node_by_path(machine, concatenated + child_path);
                        if (!is_grouped(inner)) {
                            break;
                        }
                        Playback& inner_pb = child_playback(inner, test_only, copies);
                        inner_pb.travel_main(std::string(kEnd), true);
                        child_found_route =
                            travel(inner, inner_pb, false, test_only) && child_found_route;
                        child_path += "/" + inner_pb.current;
                    }
                    start(child);
                }
                is_parent_same_state = is_current_same_state;

                const bool is_deepest_state = i == temp_path.size() - 1;
                info.is_reset = is_deepest_state && pb.reset_request_on_teleport;
                child.travel_main(temp_path[i], info.is_reset);
                if (make_travel_path(group, child, is_deepest_state && allow_transition_to_self,
                                     info.path, test_only)) {
                    found_route = found_route && child_found_route;
                } else {
                    info.path.push_back(temp_path[i]);
                    found_route = false;
                }
                children.push_back(std::move(info));
            }
            pb.reset_request_on_teleport = false;
        }
        if (found_route) {
            for (ChildInfo& child : children) {
                child.playback->path = child.path;
            }
        } else {
            for (ChildInfo& child : children) {
                child.playback->travel_main("", child.is_reset); // Clear the travel.
                if (!child.path.empty()) {
                    child.playback->start_main(child.path.back(), child.is_reset);
                }
            }
        }
        return found_route;
    }

    // Godot's _travel.
    bool travel(int machine, Playback& pb, bool allow_transition_to_self, bool test_only) {
        std::vector<std::string> path;
        const bool found = make_travel_path(machine, pb, allow_transition_to_self, path, test_only);
        if (found) {
            pb.path = std::move(path);
        }
        return found;
    }

    // Godot's _validate_path: a path to a grouped state machine goes on to the state after its
    // Start.
    [[nodiscard]] std::string validate_path(int machine, const std::string& path) const {
        if (node(machine).state_machine_type == StateMachineType::Grouped) {
            return path;
        }
        std::string target = path;
        int group = find_node_by_path(machine, target);
        while (is_grouped(group)) {
            const auto& transitions = node(group).machine_transitions;
            const auto first = std::ranges::find(transitions, kStart, &MachineTransition::from);
            if (first == transitions.end()) {
                break;
            }
            target += "/" + first->to;
            group = find_node_by_path(machine, target);
        }
        return target;
    }

    // Godot's _make_travel_path: the cheapest way through the transitions that can be taken
    // from the current state to the travel request, by A* over the states' positions, each step
    // costing its length times its transition's priority.
    bool make_travel_path(int machine, Playback& pb, bool allow_transition_to_self,
                          std::vector<std::string>& r_path, bool test_only) {
        const std::string target = std::move(pb.travel_request);
        pb.travel_request.clear();
        if (!pb.playing) {
            start(pb);
        }
        if (!has_state(machine, target) || !has_state(machine, pb.current)) {
            return false;
        }
        if (pb.current == target) {
            return !allow_transition_to_self;
        }

        struct AStarCost {
            float distance = 0.0F;
            std::string prev;
        };
        const std::vector<MachineTransition>& transitions = node(machine).machine_transitions;
        const int count = static_cast<int>(transitions.size());
        const auto transition = [&](int i) -> const MachineTransition& {
            return transitions[static_cast<std::size_t>(i)];
        };
        const Vec2 current_pos = position(machine, pb.current);
        const Vec2 target_pos = position(machine, target);
        bool found_route = false;
        std::map<std::string, AStarCost, std::less<>> cost_map;
        std::vector<int> open_list;

        for (int i = 0; i < count; ++i) {
            const MachineTransition& t = transition(i);
            if (t.advance_mode == AdvanceMode::Disabled || t.from != pb.current) {
                continue;
            }
            open_list.push_back(i);
            float cost = distance(position(machine, t.to), current_pos);
            cost *= static_cast<float>(t.priority);
            cost_map[t.to] = {.distance = cost, .prev = pb.current};
            if (t.to == target) {
                found_route = true; // Found it already.
                break;
            }
        }

        while (!found_route && !open_list.empty()) {
            // The open transition of least cost.
            std::size_t least = 0;
            float least_cost = 1e20F;
            for (std::size_t e = 0; e < open_list.size(); ++e) {
                const MachineTransition& t = transition(open_list[e]);
                float cost = cost_map[t.to].distance;
                cost += distance(position(machine, t.to), target_pos);
                if (cost < least_cost) {
                    least = e;
                    least_cost = cost;
                }
            }
            const std::string transition_prev = transition(open_list[least]).from;
            const std::string transition_to = transition(open_list[least]).to;
            for (int i = 0; i < count; ++i) {
                const MachineTransition& t = transition(i);
                if (t.advance_mode == AdvanceMode::Disabled || t.from != transition_to ||
                    t.to == transition_prev) {
                    continue;
                }
                float cost = distance(position(machine, t.from), position(machine, t.to));
                cost *= static_cast<float>(t.priority);
                cost += cost_map[t.from].distance;
                if (const auto it = cost_map.find(t.to); it != cost_map.end()) {
                    if (cost < it->second.distance) { // Visited already: is this way cheaper?
                        it->second.distance = cost;
                        it->second.prev = t.from;
                    }
                } else {
                    cost_map[t.to] = {.distance = cost, .prev = t.from};
                    open_list.push_back(i);
                    if (t.to == target) {
                        found_route = true;
                        break;
                    }
                }
            }
            if (found_route) {
                break;
            }
            open_list.erase(open_list.begin() + static_cast<std::ptrdiff_t>(least));
        }

        std::vector<std::string> new_path;
        if (found_route) {
            std::string at = target;
            while (at != pb.current && new_path.size() <= cost_map.size()) {
                new_path.push_back(at);
                at = cost_map[at].prev;
            }
            std::ranges::reverse(new_path);
            // The ways through the grouped state machines on the path, from Start to End:
            // [current - End] - [Start - End] - [Start - target].
            std::string current_path = pb.current;
            for (std::size_t i = 0; i < new_path.size() + 1; ++i) {
                const int group = find_node_by_path(machine, current_path);
                if (is_grouped(group)) {
                    std::deque<Playback> copies;
                    Playback& child = child_playback(group, test_only, copies);
                    if (i > 0) {
                        start(child);
                    }
                    if (i >= new_path.size()) {
                        break;
                    }
                    child.travel_main(std::string(kEnd), true);
                    if (!travel(group, child, false, test_only)) {
                        found_route = false;
                        break;
                    }
                }
                if (i >= new_path.size()) {
                    break;
                }
                current_path = new_path[i];
            }
        }
        if (found_route) {
            r_path = std::move(new_path);
        }
        return found_route;
    }

    // Godot's AnimationNodeStateMachinePlayback::_process.
    NodeTimeInfo process_playback(int machine, Playback& pb, const PlaybackInfo& info,
                                  bool test_only) {
        const TreeNode& n = node(machine);
        const double delta = info.delta;
        const bool seek = info.seeked;

        // A seek to 0 by the parent starts the state machine over, or a nested one's state.
        if (is_zero_approx(info.time) && seek && !info.external) {
            if (n.state_machine_type != StateMachineType::Nested || pb.is_end() || !pb.playing) {
                if (n.state_machine_type != StateMachineType::Grouped) {
                    pb.path.clear();
                    clear_path_children(machine, pb, test_only);
                }
                start(pb);
                pb.reset_request = true;
            } else {
                pb.reset_request = true;
                pb.teleport_request = true;
            }
        }

        if (pb.stop_request) {
            pb.start_request.clear();
            pb.travel_request.clear();
            pb.path.clear();
            pb.playing = false;
            return {};
        }
        if (!pb.playing && !pb.start_request.empty() && !pb.travel_request.empty()) {
            return {};
        }

        // Start and travel requests.
        if ((!pb.start_request.empty() || !pb.travel_request.empty()) &&
            n.state_machine_type != StateMachineType::Grouped) {
            clear_path_children(machine, pb, test_only);
        }
        if (!pb.start_request.empty()) {
            pb.path.clear();
            const std::string start_target = validate_path(machine, pb.start_request);
            pb.start_request = split_path(start_target)[0];
            start_children(machine, pb, start_target, test_only);
            if (!has_state(machine, pb.start_request)) {
                return {}; // No such state.
            }
            start(pb); // Teleport to it.
        }
        if (!pb.travel_request.empty()) {
            const std::string travel_target = validate_path(machine, pb.travel_request);
            const std::vector<std::string> travel_path = split_path(travel_target);
            pb.travel_request = travel_path[0];
            const std::string target = pb.travel_request; // for a teleport, if it can't travel
            std::vector<std::string> new_path;
            bool can_travel =
                make_travel_path(machine, pb, travel_path.size() <= 1 && n.allow_transition_to_self,
                                 new_path, test_only);
            if (can_travel) {
                can_travel = travel_children(machine, pb, travel_target, n.allow_transition_to_self,
                                             travel_path[0] == pb.current, test_only);
            } else {
                start_children(machine, pb, travel_target, test_only);
            }
            if (can_travel) {
                pb.path = std::move(new_path);
            } else {
                if (!has_state(machine, target)) {
                    return {}; // No such state.
                }
                pb.path.clear();
                if (pb.current != target || pb.reset_request_on_teleport) {
                    pb.current = target;
                    pb.reset_request = pb.reset_request_on_teleport;
                    pb.teleport_request = true;
                }
            }
        }

        PlaybackInfo pi = info;
        if (pb.teleport_request) {
            pb.teleport_request = false;
            // A teleport ends a cross-fade.
            pb.fading_from.clear();
            pb.fading_from_nti = {};
            pb.fading_pos = 0.0;
            // The current state's length.
            pi.time = 0.0;
            pi.seeked = true;
            pi.external = false;
            pi.weight = 0.0F;
            pb.current_nti = blend_state(machine, pb.current, pi, true);
            // A state that can be left at once isn't processed; the next one is.
            transition_to_next_recursive(machine, pb, delta, test_only);
        }

        if (!has_state(machine, pb.current)) {
            pb.playing = false;
            pb.current.clear();
            return {};
        }

        // A grouped state machine's Start and End count for nothing, so its parent's cross-fade
        // into and out of it shows; without reset_ends, so do a state machine's own.
        bool is_start_of_group = false;
        bool is_end_of_group = false;
        if (!n.reset_ends || n.state_machine_type == StateMachineType::Grouped) {
            is_start_of_group = pb.fading_from == kStart;
            is_end_of_group = pb.current == kEnd;
        }

        // The cross-fade's blend.
        float fade_blend = 1.0F;
        if (pb.fading_time != 0.0 && !pb.fading_from.empty()) {
            if (!has_state(machine, pb.fading_from)) {
                pb.fading_from.clear();
            } else {
                if (!seek) {
                    pb.fading_pos += std::abs(delta);
                }
                fade_blend = static_cast<float>(std::min(1.0, pb.fading_pos / pb.fading_time));
            }
        }
        if (!pb.current_curve.empty()) {
            fade_blend = static_cast<float>(sample_curve(pb.current_curve, fade_blend));
        }
        // Above zero, so discrete keys at the edges still play.
        fade_blend = is_zero_approx(fade_blend) ? static_cast<float>(kCmpEpsilon) : fade_blend;
        if (is_start_of_group) {
            fade_blend = 1.0F;
        } else if (is_end_of_group) {
            fade_blend = 0.0F;
        }

        pi = info;
        pi.weight = fade_blend;
        if (pb.reset_request) {
            pb.reset_request = false;
            pi.time = 0.0;
            pi.seeked = true;
        }
        pb.current_nti = blend_state(machine, pb.current, pi, test_only);

        if (!pb.fading_from.empty()) {
            double fade_blend_inv = 1.0 - fade_blend;
            fade_blend_inv = is_zero_approx(fade_blend_inv) ? kCmpEpsilon : fade_blend_inv;
            if (is_start_of_group) {
                fade_blend_inv = 0.0;
            } else if (is_end_of_group) {
                fade_blend_inv = 1.0;
            }
            pi = info;
            pi.weight = static_cast<float>(fade_blend_inv);
            if (pb.reset_request_for_fading_from) {
                pb.reset_request_for_fading_from = false;
                pi.time = 0.0;
                pi.seeked = true;
            }
            pb.fading_from_nti = blend_state(machine, pb.fading_from, pi, test_only);
            if (is_greater_or_equal_approx(pb.fading_pos, pb.fading_time)) {
                pb.fading_from.clear(); // The cross-fade is over.
                pb.fading_from_nti = {};
            }
        }

        const bool will_end =
            transition_to_next_recursive(machine, pb, delta, test_only) || pb.current == kEnd;

        // How long is left: with nothing after the current state, its time.
        if (will_end || (n.state_machine_type == StateMachineType::Nested &&
                         !has_transition_from(machine, pb.current))) {
            if (!pb.fading_from.empty()) {
                return is_greater_approx(pb.current_nti.remain(false),
                                         pb.fading_from_nti.remain(false))
                           ? pb.current_nti
                           : pb.fading_from_nti;
            }
            return pb.current_nti;
        }
        if (!pb.is_end()) {
            pb.current_nti.infinity = true;
        }
        return pb.current_nti;
    }

    // Godot's _transition_to_next_recursive: takes the transitions that can be taken now, one
    // after another in a tick, until one cross-fades. Whether it reached End.
    bool transition_to_next_recursive(int machine, Playback& pb, double delta, bool test_only) {
        pb.reset_request_for_fading_from = false;
        PlaybackInfo pi;
        pi.delta = delta;
        NextInfo next;
        std::vector<std::string> transition_path{pb.current};
        while (true) {
            next = find_next(machine, pb);
            if (!can_transition_to_next(machine, pb, next, test_only)) {
                break;
            }
            if (std::ranges::find(transition_path, next.node) != transition_path.end()) {
                break; // Transitions in a loop in one tick: stop rather than go round forever.
            }
            transition_path.push_back(next.node);

            if (next.xfade != 0.0) {
                pb.fading_from = pb.current;
                pb.fading_time = next.xfade;
                pb.fading_pos = 0.0;
            } else {
                if (pb.reset_request) {
                    // It can't be processed twice now, so the reset applies here.
                    pi.time = 0.0;
                    pi.seeked = true;
                    pi.external = false;
                    pi.weight = 0.0F;
                    blend_state(machine, pb.current, pi, test_only);
                }
                pb.fading_from.clear();
                pb.fading_from_nti = {};
                pb.fading_time = 0.0;
                pb.fading_pos = 0.0;
            }

            if (!pb.path.empty()) {
                pb.path.erase(pb.path.begin()); // It came from the travel path.
            }
            pb.current = next.node;
            pb.current_curve = next.curve;
            if (pb.current == kEnd) {
                break;
            }

            // Reset in the cross-fade in process_playback, so it isn't processed twice.
            pb.reset_request_for_fading_from = pb.reset_request;
            pb.reset_request = next.is_reset;
            pb.fading_from_nti = pb.current_nti;

            if (next.switch_mode == SwitchMode::Sync) {
                // The new state goes on from where the old one was.
                pi.time = pb.current_nti.position;
                pi.seeked = true;
                pi.external = false;
                pi.weight = 0.0F;
                blend_state(machine, pb.current, pi, test_only);
            }
            // Only its length, to find the next.
            pi.time = 0.0;
            pi.external = false;
            pi.weight = 0.0F;
            pi.seeked = next.is_reset;
            pb.current_nti = blend_state(machine, pb.current, pi, true);

            if (pb.fading_time != 0.0) {
                break; // The cross-fade must be processed.
            }
        }
        return next.node == kEnd;
    }

    // Godot's _can_transition_to_next.
    bool can_transition_to_next(int machine, Playback& pb, const NextInfo& next, bool test_only) {
        if (next.node.empty()) {
            return false;
        }
        if (pb.next_request) {
            pb.next_request = false; // Once.
            // A next request goes to the deepest state machine.
            const int group = find_node_by_path(machine, pb.current);
            if (!is_grouped(group)) {
                return true;
            }
            std::deque<Playback> copies;
            child_playback(group, test_only, copies).next_request = true;
            pb.fading_from.clear();
            pb.fading_from_nti = {};
            pb.fading_pos = 0.0;
        }
        if (!pb.fading_from.empty()) {
            return false;
        }
        if (pb.current != kStart && next.switch_mode == SwitchMode::AtEnd) {
            return is_less_or_equal_approx(pb.current_nti.remain(next.break_loop_at_end),
                                           next.xfade);
        }
        return true;
    }

    // Godot's _get_parent_playback: a grouped state machine's parent's playback, while its
    // current state is the group.
    Playback* parent_playback(int machine) {
        const int parent = node(machine).parent;
        if (parent < 0 || node(parent).type != TreeNodeType::StateMachine) {
            return nullptr;
        }
        Playback& out = playback(parent);
        return out.current == node(machine).name ? &out : nullptr;
    }

    // Godot's _check_group_transition.
    GroupTransition check_group_transition(int machine, const MachineTransition& transition) {
        GroupTransition out{.transition = &transition, .machine = machine};
        if (node(machine).state_machine_type != StateMachineType::Grouped) {
            return out;
        }
        const bool from_start = transition.from == kStart;
        if (!from_start && transition.to != kEnd) {
            return out;
        }
        const Playback* parent = parent_playback(machine);
        if (parent == nullptr) {
            return out;
        }
        out.bypass = from_start;
        // The parent's first transition into the group, or out of it.
        const int parent_machine = node(machine).parent;
        for (const MachineTransition& t : node(parent_machine).machine_transitions) {
            if ((from_start ? t.to : t.from) == parent->current) {
                out.transition = &t;
                out.machine = parent_machine;
                break;
            }
        }
        return out;
    }

    // Godot's _check_advance_condition: an auto transition whose condition, if it has one, holds.
    [[nodiscard]] bool check_advance_condition(int machine, const MachineTransition& t) const {
        if (t.advance_mode != AdvanceMode::Auto) {
            return false;
        }
        return t.advance_condition.empty() || flag(machine, "conditions/" + t.advance_condition);
    }

    // Godot's _find_next: the next state on the travel path, or the auto transition of best
    // priority whose condition holds, the last of equals.
    NextInfo find_next(int machine, const Playback& pb) {
        NextInfo next;
        const auto fill = [&](const std::string& to, const MachineTransition& t) {
            next.node = to;
            next.xfade = t.xfade_time;
            next.curve = t.xfade_curve;
            next.switch_mode = t.switch_mode;
            next.is_reset = t.reset;
            next.break_loop_at_end = t.break_loop_at_end;
        };
        const std::vector<MachineTransition>& transitions = node(machine).machine_transitions;
        if (!pb.path.empty()) {
            for (const MachineTransition& t : transitions) {
                const GroupTransition g = check_group_transition(machine, t);
                if (g.transition->advance_mode == AdvanceMode::Disabled) {
                    continue;
                }
                if (t.from == pb.current && t.to == pb.path[0]) {
                    fill(pb.path[0], *g.transition);
                }
            }
            return next;
        }
        const MachineTransition* best = nullptr;
        GroupTransition best_group;
        float priority_best = 1e20F;
        for (const MachineTransition& t : transitions) {
            const GroupTransition g = check_group_transition(machine, t);
            if (g.transition->advance_mode == AdvanceMode::Disabled) {
                continue;
            }
            if (t.from == pb.current &&
                (check_advance_condition(g.machine, *g.transition) || g.bypass) &&
                static_cast<float>(g.transition->priority) <= priority_best) {
                priority_best = static_cast<float>(g.transition->priority);
                best = &t;
                best_group = g;
            }
        }
        if (best != nullptr) {
            fill(best->to, *best_group.transition);
        }
        return next;
    }

    const TreeContext& context_;
    TreeParameters& parameters_;
    const std::vector<TreeNode>& nodes_;
    // Each node's track weights, as Godot's NodeState::track_weights.
    std::vector<std::vector<float>> weights_;
    // Godot's ProcessState::is_testing, set while a node is processed. Godot leaves it set after
    // a node processed only to test it, so the writes its parent makes next are lost; here the
    // parent's own setting comes back.
    bool testing_ = false;
    // The state machines' playbacks this tick, by node, read from their parameters when first
    // needed and written back after the tick.
    std::map<int, Playback> playbacks_;
    TreeTick out_;
};

bool same_value(const json::Value& a, const json::Value& b) {
    if (a.is_number() && b.is_number()) {
        return a.as_double() == b.as_double();
    }
    return a == b;
}

} // namespace

TreeParameters::TreeParameters(const std::vector<TreeParameter>& list, json::Object& inputs,
                               json::Object& state)
    : inputs_(inputs), state_(state) {
    for (const TreeParameter& parameter : list) {
        by_key_.emplace(parameter.key, &parameter);
    }
}

json::Value TreeParameters::get(const TreeNode& node, std::string_view name) const {
    const std::string key = node.path + std::string(name);
    const auto found = by_key_.find(key);
    if (found == by_key_.end()) {
        return {};
    }
    const json::Object& values = found->second->input ? inputs_ : state_;
    if (const auto it = values.find(key); it != values.end()) {
        return it->second;
    }
    return found->second->default_value;
}

void TreeParameters::set(const TreeNode& node, std::string_view name, json::Value value) {
    std::string key = node.path + std::string(name);
    const auto found = by_key_.find(key);
    if (found == by_key_.end()) {
        return;
    }
    json::Object& values = found->second->input ? inputs_ : state_;
    if (same_value(value, found->second->default_value)) {
        values.erase(key);
    } else {
        values.insert_or_assign(std::move(key), std::move(value));
    }
}

double sample_curve(const TreeCurve& curve, double offset) {
    if (curve.empty()) {
        return offset;
    }
    if (offset <= curve.front()[0]) {
        return curve.front()[1];
    }
    for (std::size_t i = 1; i < curve.size(); ++i) {
        if (offset <= curve[i][0]) {
            const double span = curve[i][0] - curve[i - 1][0];
            const double t = span > 0.0 ? (offset - curve[i - 1][0]) / span : 1.0;
            return curve[i - 1][1] + ((curve[i][1] - curve[i - 1][1]) * t);
        }
    }
    return curve.back()[1];
}

TreeTick run_tree(const TreeContext& context, TreeParameters& parameters, double delta,
                  bool started) {
    return TreeRun(context, parameters).tick(delta, started);
}

} // namespace slash::animation::detail
