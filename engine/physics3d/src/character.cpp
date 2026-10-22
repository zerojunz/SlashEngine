// Character movement, as Godot's CharacterBody3D.move_and_slide, on Space::test_motion (Godot's
// body_test_motion). Each function names the one of Godot's it follows, line for line.

#include "space.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <optional>
#include <ranges>
#include <vector>

namespace slash::physics3d::detail {

namespace {

using world::CharacterBody3D;

// Godot adds this to floor_max_angle and wall_min_slide_angle, so a surface right at the limit
// counts.
constexpr float kFloorAngleThreshold = 0.01F;

// Godot's CharacterBody3D::CollisionState.
struct CollisionState {
    bool floor = false;
    bool wall = false;
    bool ceiling = false;
};

constexpr CollisionState kApplyAll{.floor = true, .wall = true, .ceiling = true};

// Godot's Math::is_equal_approx.
bool equal_approx(float a, float b) {
    if (a == b) {
        return true;
    }
    const float tolerance = std::max(kEpsilon * std::abs(a), kEpsilon);
    return std::abs(a - b) < tolerance;
}

// Godot's Vector3::is_equal_approx.
bool equal_approx(Vec3 a, Vec3 b) {
    return equal_approx(a.x, b.x) && equal_approx(a.y, b.y) && equal_approx(a.z, b.z);
}

Vec3 divided(Vec3 v, float s) {
    return {v.x / s, v.y / s, v.z / s};
}

Vec3 negated(Vec3 v) {
    return v * -1.0F;
}

float length_squared(Vec3 v) {
    return dot(v, v);
}

// One call of move_and_slide: the CharacterBody3D's members Godot keeps, loaded from the
// component and the body, and written back when the move is done.
class Mover {
public:
    Mover(const Space& space, Body& body, world::Affine3D& transform, CharacterBody3D& character)
        : space_(&space), body_(&body), transform_(&transform), c_(&character),
          grounded_mode_(character.motion_mode != "floating"),
          up_(normalized(character.up_direction)), margin_(character.safe_margin),
          floor_normal_(character.floor_normal), wall_normal_(character.wall_normal),
          platform_velocity_(character.platform_velocity), platform_(body.platform),
          platform_layer_(body.platform_layer) {}

    // CharacterBody3D::move_and_slide.
    void move_and_slide(float delta) {
        CharacterBody3D& c = *c_;
        Vec3 current_platform_velocity = platform_velocity_;

        if ((c.on_floor || c.on_wall) && platform_ != entt::null) {
            bool excluded = false;
            if (c.on_floor) {
                excluded = (c.platform_floor_layers & platform_layer_) == 0;
            } else if (c.on_wall) {
                excluded = (c.platform_wall_layers & platform_layer_) == 0;
            }
            if (!excluded) {
                const Body* platform = space_->body(platform_);
                const std::optional<Vec3> velocity =
                    platform != nullptr ? space_->velocity_at(*platform, transform_->origin)
                                        : std::nullopt;
                if (velocity) {
                    current_platform_velocity = *velocity;
                } else {
                    // Body is removed or destroyed, invalidate floor.
                    current_platform_velocity = {};
                    platform_ = entt::null;
                }
            } else {
                current_platform_velocity = {};
            }
        }

        const bool was_on_floor = c.on_floor;
        state_ = {};

        if (!zero_approx(current_platform_velocity)) {
            MotionParameters parameters = parameters_for(current_platform_velocity * delta);
            parameters.recovery_as_collision = true;
            if (platform_ != entt::null) {
                parameters.exclude.push_back(platform_);
            }
            MotionResult floor_result;
            if (move_and_collide(parameters, floor_result, false, false)) {
                note(floor_result);
                CollisionState result_state;
                set_collision_direction(floor_result, result_state);
            }
        }

        if (grounded_mode_) {
            move_and_slide_grounded(delta, was_on_floor);
        } else {
            move_and_slide_floating(delta);
        }

        if (c.platform_on_leave != "do_nothing") {
            // Add last platform velocity when just left a moving platform.
            if (!state_.floor && !state_.wall) {
                if (c.platform_on_leave == "add_upward_velocity" &&
                    dot(current_platform_velocity, up_) < 0.0F) {
                    current_platform_velocity = slide(current_platform_velocity, up_);
                }
                c.velocity = c.velocity + current_platform_velocity;
            }
        }

        c.on_floor = state_.floor;
        c.on_wall = state_.wall;
        c.on_ceiling = state_.ceiling;
        c.floor_normal = floor_normal_;
        c.wall_normal = wall_normal_;
        c.platform_velocity = platform_velocity_;
        body_->platform = platform_;
        body_->platform_layer = platform_layer_;
        body_->touching = std::move(touching_);
    }

private:
    [[nodiscard]] MotionParameters parameters_for(Vec3 motion) const {
        MotionParameters parameters;
        parameters.from = *transform_;
        parameters.motion = motion;
        parameters.margin = margin_;
        return parameters;
    }

    // What a collision touched, for the body's touching list, as Godot's motion_results.
    void note(const MotionResult& result) {
        for (const MotionCollision& collision : result.collisions) {
            if (std::find(touching_.begin(), touching_.end(), collision.collider) ==
                touching_.end()) {
                touching_.push_back(collision.collider);
            }
        }
    }

    // PhysicsBody3D::move_and_collide.
    bool move_and_collide(const MotionParameters& parameters, MotionResult& result, bool test_only,
                          bool cancel_sliding) {
        const bool colliding = space_->test_motion(*body_, parameters, result);

        // Restore direction of motion to be along original motion, in order to avoid sliding
        // due to recovery, but only if collision depth is low enough to avoid tunneling.
        if (cancel_sliding) {
            const float motion_length = length(parameters.motion);
            float precision = 0.001F;

            if (colliding) {
                // Can't just use margin as a threshold because collision depth is calculated on
                // unsafe motion, so even in normal resting cases the depth can be a bit more
                // than the margin.
                precision += motion_length *
                             (result.collision_unsafe_fraction - result.collision_safe_fraction);
                if (result.collisions.front().depth > parameters.margin + precision) {
                    cancel_sliding = false;
                }
            }

            if (cancel_sliding) {
                // When motion is null, recovery is the resulting motion.
                Vec3 motion_normal;
                if (motion_length > kEpsilon) {
                    motion_normal = divided(parameters.motion, motion_length);
                }
                // Check depth of recovery.
                const float projected_length = dot(result.travel, motion_normal);
                const Vec3 recovery = result.travel - (motion_normal * projected_length);
                // Fixes cases where canceling slide causes the motion to go too deep into the
                // ground, because we're only taking rest information into account and not
                // general recovery.
                if (length(recovery) < parameters.margin + precision) {
                    // Apply adjustment to motion.
                    result.travel = motion_normal * projected_length;
                    result.remainder = parameters.motion - result.travel;
                }
            }
        }

        if (!test_only) {
            world::Affine3D moved = parameters.from;
            moved.origin = moved.origin + result.travel;
            *transform_ = moved;
        }
        return colliding;
    }

    // CharacterBody3D::_move_and_slide_grounded.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity): Godot's, kept whole
    void move_and_slide_grounded(float delta, bool was_on_floor) {
        CharacterBody3D& c = *c_;
        Vec3 motion = c.velocity * delta;
        const Vec3 motion_slide_up = slide(motion, up_);
        const Vec3 prev_floor_normal = floor_normal_;

        platform_ = entt::null;
        platform_velocity_ = {};
        platform_ceiling_velocity_ = {};
        floor_normal_ = {};
        wall_normal_ = {};
        ceiling_normal_ = {};

        // No sliding on first attempt to keep floor motion stable when possible, when stop on
        // slope is enabled or when there is no up direction.
        bool sliding_enabled = !c.floor_stop_on_slope;
        // Constant speed can be applied only the first time sliding is enabled.
        bool can_apply_constant_speed = sliding_enabled;
        // If the platform's ceiling push down the body.
        bool apply_ceiling_velocity = false;
        bool first_slide = true;
        const bool vel_dir_facing_up = dot(c.velocity, up_) > 0.0F;
        Vec3 total_travel;

        for (int iteration = 0; iteration < c.max_slides; ++iteration) {
            MotionParameters parameters = parameters_for(motion);
            // There can be 4 collisions between 2 walls + 2 more for the floor.
            parameters.max_collisions = 6;
            parameters.recovery_as_collision = true;

            MotionResult result;
            bool collided = move_and_collide(parameters, result, false, !sliding_enabled);

            if (collided) {
                note(result);
                const CollisionState previous_state = state_;
                CollisionState result_state;
                set_collision_direction(result, result_state);

                // If we hit a ceiling platform, we set the vertical velocity to at least the
                // platform one.
                if (state_.ceiling && platform_ceiling_velocity_ != Vec3{} &&
                    dot(platform_ceiling_velocity_, up_) < 0.0F) {
                    // If ceiling sliding is on, only apply when the ceiling is flat or when the
                    // motion is upward.
                    if (!c.slide_on_ceiling || dot(motion, up_) < 0.0F ||
                        length(ceiling_normal_ + up_) < 0.01F) {
                        apply_ceiling_velocity = true;
                        const Vec3 ceiling_vertical_velocity =
                            up_ * dot(up_, platform_ceiling_velocity_);
                        const Vec3 motion_vertical_velocity = up_ * dot(up_, c.velocity);
                        if (dot(motion_vertical_velocity, up_) > 0.0F ||
                            length_squared(ceiling_vertical_velocity) >
                                length_squared(motion_vertical_velocity)) {
                            c.velocity = ceiling_vertical_velocity + slide(c.velocity, up_);
                        }
                    }
                }

                if (state_.floor && c.floor_stop_on_slope &&
                    length(normalized(c.velocity) + up_) < 0.01F) {
                    world::Affine3D gt = *transform_;
                    if (length(result.travel) <= margin_ + kEpsilon) {
                        gt.origin = gt.origin - result.travel;
                    }
                    *transform_ = gt;
                    c.velocity = {};
                    motion = {};
                    break;
                }

                if (zero_approx(result.remainder)) {
                    motion = {};
                    break;
                }

                // Apply regular sliding by default.
                bool apply_default_sliding = true;

                // Wall collision checks.
                if (result_state.wall && dot(motion_slide_up, wall_normal_) <= 0.0F) {
                    // Move on floor only checks.
                    if (c.floor_block_on_wall) {
                        // Needs horizontal motion from current motion instead of
                        // motion_slide_up to properly test the angle and avoid standing on
                        // slopes.
                        const Vec3 horizontal_motion = slide(motion, up_);
                        const Vec3 horizontal_normal = normalized(slide(wall_normal_, up_));
                        const float motion_angle =
                            std::abs(acos(-dot(horizontal_normal, normalized(horizontal_motion))));

                        // Avoid to move forward on a wall if floor_block_on_wall is true.
                        // Applies only when the motion angle is under 90 degrees, in order to
                        // avoid blocking lateral motion along a wall.
                        if (motion_angle < 0.5F * std::numbers::pi_v<float>) {
                            apply_default_sliding = false;
                            if (was_on_floor && !vel_dir_facing_up) {
                                // Cancel the motion.
                                world::Affine3D gt = *transform_;
                                const float travel_total = length(result.travel);
                                const float cancel_dist_max = std::min(0.1F, margin_ * 20.0F);
                                if (travel_total <= margin_ + kEpsilon) {
                                    gt.origin = gt.origin - result.travel;
                                    // Cancel for constant speed computation.
                                    result.travel = {};
                                } else if (travel_total < cancel_dist_max) {
                                    // If the movement is large the body can be prevented from
                                    // reaching the walls.
                                    gt.origin = gt.origin - slide(result.travel, up_);
                                    // Keep remaining motion in sync with amount canceled.
                                    motion = slide(motion, up_);
                                    result.travel = {};
                                } else {
                                    // Travel is too high to be safely canceled, we take it
                                    // into account.
                                    result.travel = slide(result.travel, up_);
                                    motion = result.remainder;
                                }
                                *transform_ = gt;
                                // Determines if you are on the ground, and limits the
                                // possibility of climbing on the walls because of the
                                // approximations.
                                snap_on_floor(true, false);
                            } else {
                                // If the movement is not canceled we only keep the remaining.
                                motion = result.remainder;
                            }

                            // Apply slide on forward in order to allow only lateral motion on
                            // next step.
                            const Vec3 forward = normalized(slide(wall_normal_, up_));
                            motion = slide(motion, forward);

                            // Scales the horizontal velocity according to the wall slope.
                            if (vel_dir_facing_up) {
                                const Vec3 slide_motion =
                                    slide(c.velocity, result.collisions.front().normal);
                                // Keeps the vertical motion from velocity and add the
                                // horizontal motion of the projection.
                                c.velocity =
                                    (up_ * dot(up_, c.velocity)) + slide(slide_motion, up_);
                            } else {
                                c.velocity = slide(c.velocity, forward);
                            }

                            // Allow only lateral motion along previous floor when already on
                            // floor. Fixes slowing down when moving in diagonal against an
                            // inclined wall.
                            if (was_on_floor && !vel_dir_facing_up && dot(motion, up_) > 0.0F) {
                                // Slide along the corner between the wall and previous floor.
                                const Vec3 floor_side = cross(prev_floor_normal, wall_normal_);
                                if (floor_side != Vec3{}) {
                                    motion = floor_side * dot(motion, floor_side);
                                }
                            }

                            // Stop all motion when a second wall is hit (unless sliding down or
                            // jumping), in order to avoid jittering in corner cases.
                            bool stop_all_motion = previous_state.wall && !vel_dir_facing_up;

                            // Allow sliding when the body falls.
                            if (!state_.floor && dot(motion, up_) < 0.0F) {
                                const Vec3 slide_motion = slide(motion, wall_normal_);
                                // Test again to allow sliding only if the result goes
                                // downwards. Fixes jittering issues at the bottom of inclined
                                // walls.
                                if (dot(slide_motion, up_) < 0.0F) {
                                    stop_all_motion = false;
                                    motion = slide_motion;
                                }
                            }

                            if (stop_all_motion) {
                                motion = {};
                                c.velocity = {};
                            }
                        }
                    }

                    // Stop horizontal motion when under wall slide threshold.
                    if (was_on_floor && c.wall_min_slide_angle > 0.0F && result_state.wall) {
                        const Vec3 horizontal_normal = normalized(slide(wall_normal_, up_));
                        const float motion_angle =
                            std::abs(acos(-dot(horizontal_normal, normalized(motion_slide_up))));
                        if (motion_angle < c.wall_min_slide_angle) {
                            motion = up_ * dot(motion, up_);
                            c.velocity = up_ * dot(c.velocity, up_);
                            apply_default_sliding = false;
                        }
                    }
                }

                if (apply_default_sliding) {
                    // Regular sliding, the last part of the test handle the case when you don't
                    // want to slide on the ceiling.
                    if ((sliding_enabled || !state_.floor) &&
                        (!state_.ceiling || c.slide_on_ceiling || !vel_dir_facing_up) &&
                        !apply_ceiling_velocity) {
                        const MotionCollision& collision = result.collisions.front();

                        Vec3 slide_motion = slide(result.remainder, collision.normal);
                        if (state_.floor && !state_.wall && !zero_approx(motion_slide_up)) {
                            // Slide using the intersection between the motion plane and the
                            // floor plane, in order to keep the direction intact.
                            const float motion_length = length(slide_motion);
                            slide_motion = cross(cross(up_, result.remainder), floor_normal_);

                            // Keep the length from default slide to change speed in slopes by
                            // default, when constant speed is not enabled.
                            slide_motion = normalized(slide_motion) * motion_length;
                        }

                        if (dot(slide_motion, c.velocity) > 0.0F) {
                            motion = slide_motion;
                        } else {
                            motion = {};
                        }

                        if (c.slide_on_ceiling && result_state.ceiling) {
                            // Apply slide only in the direction of the input motion, otherwise
                            // just stop to avoid jittering when moving against a wall.
                            if (vel_dir_facing_up) {
                                c.velocity = slide(c.velocity, collision.normal);
                            } else {
                                // Avoid acceleration in slope when falling.
                                c.velocity = up_ * dot(up_, c.velocity);
                            }
                        }
                    } else {
                        // No sliding on first attempt to keep floor motion stable when possible.
                        motion = result.remainder;
                        if (result_state.ceiling && !c.slide_on_ceiling && vel_dir_facing_up) {
                            c.velocity = slide(c.velocity, up_);
                            motion = slide(motion, up_);
                        }
                    }
                }

                total_travel = total_travel + result.travel;

                // Apply Constant Speed.
                if (was_on_floor && c.floor_constant_speed && can_apply_constant_speed &&
                    state_.floor && !zero_approx(motion)) {
                    const Vec3 travel_slide_up = slide(total_travel, up_);
                    motion = normalized(motion) *
                             std::max(0.0F, length(motion_slide_up) - length(travel_slide_up));
                }
            } else if (c.floor_constant_speed && first_slide &&
                       on_floor_if_snapped(was_on_floor, vel_dir_facing_up)) {
                // When you move forward in a downward slope you don't collide because you will
                // be in the air. This test ensures that constant speed is applied, only if the
                // player is still on the ground after the snap is applied.
                can_apply_constant_speed = false;
                sliding_enabled = true;
                world::Affine3D gt = *transform_;
                gt.origin = gt.origin - result.travel;
                *transform_ = gt;

                // Slide using the intersection between the motion plane and the floor plane, in
                // order to keep the direction intact.
                const Vec3 motion_slide_norm =
                    normalized(cross(cross(up_, motion), prev_floor_normal));
                motion = motion_slide_norm * length(motion_slide_up);
                collided = true;
            }

            if (!collided || zero_approx(motion)) {
                break;
            }

            can_apply_constant_speed = !can_apply_constant_speed && !sliding_enabled;
            sliding_enabled = true;
            first_slide = false;
        }

        snap_on_floor(was_on_floor, vel_dir_facing_up);

        // Reset the gravity accumulation when touching the ground.
        if (state_.floor && !vel_dir_facing_up) {
            c.velocity = slide(c.velocity, up_);
        }
    }

    // CharacterBody3D::_move_and_slide_floating.
    void move_and_slide_floating(float delta) {
        const CharacterBody3D& c = *c_;
        Vec3 motion = c.velocity * delta;

        platform_ = entt::null;
        floor_normal_ = {};
        platform_velocity_ = {};

        bool first_slide = true;
        for (int iteration = 0; iteration < c.max_slides; ++iteration) {
            MotionParameters parameters = parameters_for(motion);
            parameters.recovery_as_collision = true;

            MotionResult result;
            const bool collided = move_and_collide(parameters, result, false, false);

            if (collided) {
                note(result);
                CollisionState result_state;
                set_collision_direction(result, result_state);

                if (zero_approx(result.remainder)) {
                    motion = {};
                    break;
                }

                if (c.wall_min_slide_angle != 0.0F &&
                    acos(dot(wall_normal_, negated(normalized(c.velocity)))) <
                        c.wall_min_slide_angle + kFloorAngleThreshold) {
                    motion = {};
                    if (length(result.travel) < margin_ + kEpsilon) {
                        world::Affine3D gt = *transform_;
                        gt.origin = gt.origin - result.travel;
                        *transform_ = gt;
                    }
                } else if (first_slide) {
                    const Vec3 motion_slide_norm =
                        normalized(slide(result.remainder, wall_normal_));
                    motion = motion_slide_norm * (length(motion) - length(result.travel));
                } else {
                    motion = slide(result.remainder, wall_normal_);
                }

                if (dot(motion, c.velocity) <= 0.0F) {
                    motion = {};
                }
            }

            if (!collided || zero_approx(motion)) {
                break;
            }
            first_slide = false;
        }
    }

    // CharacterBody3D::apply_floor_snap.
    void apply_floor_snap() {
        if (state_.floor) {
            return;
        }
        // Snap by at least collision margin to keep floor state consistent.
        const float snap = std::max(c_->floor_snap_length, margin_);

        MotionParameters parameters = parameters_for(negated(up_) * snap);
        parameters.max_collisions = 4;
        parameters.recovery_as_collision = true;

        MotionResult result;
        if (move_and_collide(parameters, result, true, false)) {
            CollisionState result_state;
            // Apply direction for floor only.
            set_collision_direction(result, result_state, {.floor = true});

            if (result_state.floor) {
                // Ensure that we only move the body along the up axis, because
                // move_and_collide may stray the object a bit when getting it unstuck.
                // Canceling this motion should not affect move_and_slide, as previous calls to
                // move_and_collide already took care of freeing the body.
                if (length(result.travel) > margin_) {
                    result.travel = up_ * dot(up_, result.travel);
                } else {
                    result.travel = {};
                }
                parameters.from.origin = parameters.from.origin + result.travel;
                *transform_ = parameters.from;
            }
        }
    }

    // CharacterBody3D::_snap_on_floor.
    void snap_on_floor(bool was_on_floor, bool vel_dir_facing_up) {
        if (state_.floor || !was_on_floor || vel_dir_facing_up) {
            return;
        }
        apply_floor_snap();
    }

    // CharacterBody3D::_on_floor_if_snapped.
    bool on_floor_if_snapped(bool was_on_floor, bool vel_dir_facing_up) {
        if (up_ == Vec3{} || state_.floor || !was_on_floor || vel_dir_facing_up) {
            return false;
        }
        // Snap by at least collision margin to keep floor state consistent.
        const float snap = std::max(c_->floor_snap_length, margin_);

        MotionParameters parameters = parameters_for(negated(up_) * snap);
        parameters.max_collisions = 4;
        parameters.recovery_as_collision = true;

        MotionResult result;
        if (move_and_collide(parameters, result, true, false)) {
            CollisionState result_state;
            // Don't apply direction for any type.
            set_collision_direction(result, result_state, {});
            return result_state.floor;
        }
        return false;
    }

    // CharacterBody3D::_set_collision_direction.
    void set_collision_direction(const MotionResult& result, CollisionState& state,
                                 CollisionState apply = kApplyAll) {
        state = {};

        float wall_depth = -1.0F;
        float floor_depth = -1.0F;

        const bool was_on_wall = state_.wall;
        const Vec3 prev_wall_normal = wall_normal_;
        int wall_collision_count = 0;
        Vec3 combined_wall_normal;
        Vec3 tmp_wall_col; // Avoid duplicate on average calculation.

        for (const MotionCollision& collision : std::views::reverse(result.collisions)) {

            if (grounded_mode_) {
                // Check if any collision is floor.
                const float floor_angle = collision.angle(up_);
                if (floor_angle <= c_->floor_max_angle + kFloorAngleThreshold) {
                    state.floor = true;
                    if (apply.floor && collision.depth > floor_depth) {
                        state_.floor = true;
                        floor_normal_ = collision.normal;
                        floor_depth = collision.depth;
                        set_platform_data(collision);
                    }
                    continue;
                }

                // Check if any collision is ceiling.
                const float ceiling_angle = collision.angle(negated(up_));
                if (ceiling_angle <= c_->floor_max_angle + kFloorAngleThreshold) {
                    state.ceiling = true;
                    if (apply.ceiling) {
                        platform_ceiling_velocity_ = collision.collider_velocity;
                        ceiling_normal_ = collision.normal;
                        state_.ceiling = true;
                    }
                    continue;
                }
            }

            // Collision is wall by default.
            state.wall = true;

            if (apply.wall && collision.depth > wall_depth) {
                state_.wall = true;
                wall_depth = collision.depth;
                wall_normal_ = collision.normal;

                // Don't apply wall velocity when the collider is a CharacterBody3D.
                if (!collision.collider_is_character) {
                    set_platform_data(collision);
                }
            }

            // Collect normal for calculating average.
            if (!equal_approx(collision.normal, tmp_wall_col)) {
                tmp_wall_col = collision.normal;
                combined_wall_normal = combined_wall_normal + collision.normal;
                ++wall_collision_count;
            }
        }

        if (state.wall && wall_collision_count > 1 && !state.floor && grounded_mode_) {
            // Check if wall normals cancel out to floor support.
            combined_wall_normal = normalized(combined_wall_normal);
            const float floor_angle = acos(dot(combined_wall_normal, up_));
            if (floor_angle <= c_->floor_max_angle + kFloorAngleThreshold) {
                state.floor = true;
                state.wall = false;
                if (apply.floor) {
                    state_.floor = true;
                    floor_normal_ = combined_wall_normal;
                }
                if (apply.wall) {
                    state_.wall = was_on_wall;
                    wall_normal_ = prev_wall_normal;
                }
            }
        }
    }

    // CharacterBody3D::_set_platform_data.
    void set_platform_data(const MotionCollision& collision) {
        platform_ = collision.collider;
        platform_velocity_ = collision.collider_velocity;
        platform_layer_ = collision.collider_layer;
    }

    const Space* space_;
    Body* body_;
    world::Affine3D* transform_;
    CharacterBody3D* c_;
    bool grounded_mode_;
    Vec3 up_;
    float margin_;
    CollisionState state_;
    Vec3 floor_normal_;
    Vec3 wall_normal_;
    Vec3 ceiling_normal_;
    Vec3 platform_velocity_;
    Vec3 platform_ceiling_velocity_;
    entt::entity platform_;
    std::uint32_t platform_layer_;
    std::vector<entt::entity> touching_;
};

} // namespace

void move_character(const Space& space, Body& body, world::Affine3D& transform,
                    CharacterBody3D& character, float seconds) {
    Mover(space, body, transform, character).move_and_slide(seconds);
}

} // namespace slash::physics3d::detail
