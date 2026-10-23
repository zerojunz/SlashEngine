#include "tree_run.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
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

    const TreeContext& context_;
    TreeParameters& parameters_;
    const std::vector<TreeNode>& nodes_;
    // Each node's track weights, as Godot's NodeState::track_weights.
    std::vector<std::vector<float>> weights_;
    // Godot's ProcessState::is_testing: set by each node processed, and left set after.
    bool testing_ = false;
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
