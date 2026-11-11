#pragma once

// Animals as recipes, after the creature tools of Unreal's Mutable and its Animal AI (Godot has
// none): a .creature.json file says a species' body plan, its look and how it behaves (where it
// roams, what it flees, hunts and eats, whether it flocks and whether people can tame and ride
// it). slash/scene3d/creature.hpp builds it into a rigged, animated model, creature_cutout here
// into a 2D cut-out drawn by the character_2d system, and slash/npc/fauna.hpp runs its behavior.

#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/math.hpp"
#include "slash/core/result.hpp"
#include "slash/world/character.hpp"
#include "slash/world/world.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace slash::world {

inline constexpr std::string_view kCreatureSuffix = ".creature.json";

// Whether a path names a .creature.json file.
[[nodiscard]] bool is_creature_path(std::string_view path);

// The body: its plan ("quadruped", "bird", "fish" or "insect") and its size in meters, with
// proportions 1 for the plan's usual ones.
struct CreatureBody {
    std::string plan = "quadruped";
    float length = 1.0F; // nose to rump (a fish's or bird's body, an insect's head to tail)
    float height = 0.8F; // to the top of the back, standing
    float build = 0.5F;  // 0 slight to 1 heavy
    float head = 1.0F;
    float neck = 1.0F;
    float legs = 1.0F;
    float tail = 1.0F; // 0: none
};

// What it looks like: its coat (fur, feathers, scales or shell), a lighter belly, markings in a
// pattern ("plain", "spots", "stripes" or "patches") and its eyes.
struct CreatureLook {
    Vec4 coat{0.55F, 0.4F, 0.25F, 1.0F};
    Vec4 belly{0.85F, 0.78F, 0.65F, 1.0F};
    Vec4 markings{0.25F, 0.18F, 0.1F, 1.0F};
    std::string pattern = "plain";
    Vec4 eyes{0.08F, 0.06F, 0.04F, 1.0F};
};

// Parts some species have: ears ("none", "short", "long" or "pointed"), horns ("none", "horns"
// or "antlers"), a mane, antennae, and wings span meters across (birds' and insects'; 0 none).
struct CreatureParts {
    std::string ears = "short";
    std::string horns = "none";
    bool mane = false;
    bool antennae = false;
    float wings = 0.0F;
};

// How it behaves. temperament: "timid" (flees what it notices), "wary" (flees what comes within
// flee), "calm" (flees nothing but predators that hunt it), "aggressive" (attacks people it
// notices) or "friendly" (comes to people). Speeds in meters a second, distances in meters.
struct CreatureBehavior {
    std::string temperament = "wary";
    float walk_speed = 1.2F;
    float run_speed = 4.0F;
    float notice = 10.0F; // how far it sees people and predators
    float flee = 5.0F;    // how near a wary one lets them come
    float roam = 12.0F;   // how far it wanders from home
    bool herd = false;    // keeps with others of its species: a herd, flock or school
    float herd_radius = 6.0F;
    std::vector<std::string> hunts; // kinds or species it hunts, as a predator
    std::vector<std::string> food;  // what it eats from people's hands; empty: anything
    int tame = 0;                   // feedings to tame it; 0: it can't be tamed
    bool ride = false;              // its owner can ride it once tamed
    float damage = 10.0F;           // what an attack or a catch does
    float reach = 1.2F;             // how near it must be to attack or catch
    float attack_cooldown = 1.0F;   // seconds between attacks
    float hunger_rate = 0.02F; // hunger gained a second, from 0 to 1; a predator hunts past 0.5
    float altitude = 0.0F;     // birds and insects: how high above home they fly; fish: how deep
};

struct CreatureRecipe {
    std::string name;
    std::string species;
    std::string kind = "grazer"; // "bird", "fish", "insect", "grazer", "predator" or "pet"
    CreatureBody body;
    CreatureLook look;
    CreatureParts parts;
    CreatureBehavior behavior;
};

// The JSON Schema of .creature.json files.
[[nodiscard]] json::Value creature_schema();

// Every problem with a recipe's JSON, after its schema: parts its plan has no place for (wings
// on a fish, horns on an insect), riding without taming.
[[nodiscard]] std::vector<json::ValidationError> validate_creature(const json::Value& doc);

// The recipe a .creature.json file's JSON makes, its "extends" chain read with read_file (each a
// merge patch over the file it extends, as JSON Merge Patch). A plan left out follows the kind:
// birds "bird", fish "fish", insects "insect", the rest "quadruped".
[[nodiscard]] Result<CreatureRecipe> parse_creature(const json::Value& source,
                                                    const FileReader& read_file);
// The JSON a file's "extends" chain merges to, without "extends".
[[nodiscard]] Result<json::Value> resolve_creature(const json::Value& doc,
                                                   const FileReader& read_file);

// Whether it flies, swims or walks, by its plan and wings.
[[nodiscard]] bool creature_flies(const CreatureRecipe& recipe);
[[nodiscard]] bool creature_swims(const CreatureRecipe& recipe);

// Where a recipe's body is, in meters with y up, the feet at 0 (a fish's belly; a bird's feet)
// and its nose toward +z, which its 3D model and 2D cut-out are both built by. Radii are half
// sizes.
struct CreatureMeasures {
    real_t length = 0.0F, height = 0.0F;
    real_t body_radius = 0.0F, body_depth = 0.0F; // across (x) and up (y) at the middle
    real_t body_z = 0.0F, body_half = 0.0F;       // the body's middle and half its length
    real_t back = 0.0F;                           // the spine's height
    real_t leg = 0.0F, leg_radius = 0.0F;         // how high the legs meet the body, and thickness
    real_t hip_z = 0.0F, shoulder_z = 0.0F;       // where the legs meet the body
    real_t neck = 0.0F, head_radius = 0.0F, snout = 0.0F;
    real_t tail = 0.0F, tail_radius = 0.0F;
    real_t wing = 0.0F; // each wing's span
};
[[nodiscard]] CreatureMeasures creature_measures(const CreatureRecipe& recipe);

// A bone of a recipe's skeleton, which its 3D model is skinned to and its 2D cut-out's parts are
// named after: "body" (the root), "neck", "head", "tail", "leg_front_near", "leg_back_far",
// "leg_middle_near" (insects), "leg_near" (birds), "wing_near", "wing_far"; near is +x. head is
// where it turns, in the model's space at rest.
struct CreatureBone {
    std::string name;
    int parent = -1;
    Vec3 head;
};
[[nodiscard]] std::vector<CreatureBone> creature_skeleton(const CreatureRecipe& recipe);

// A recipe's animations, which its 3D model has and its 2D cut-out is posed by: "idle", "walk",
// "run", "eat" and "attack" (once; the rest loop), "fly" for those that fly and "swim" for fish.
// Lengths are seconds; walk and run play one stride at the recipe's speeds.
struct CreatureAnimation {
    std::string name;
    double length = 1.0;
    bool loop = true;
};
[[nodiscard]] std::vector<CreatureAnimation> creature_animations(const CreatureRecipe& recipe);

// How far each bone turns from rest at a time in an animation, about an axis in its parent's
// space (x pitches a leg or neck, y wags a tail, z flaps a wing): the 3D model's keys and the 2D
// cut-out's turns. Bones at rest are left out; an unknown animation is idle.
struct CreatureTurn {
    std::string bone;
    Vec3 axis;
    float angle = 0.0F; // radians
};
[[nodiscard]] std::vector<CreatureTurn> creature_pose(const CreatureRecipe& recipe,
                                                      std::string_view animation, double time);

// A recipe's parts as a 2D cut-out, seen from the side facing +x, in meters with y up and the
// feet at 0: a bone for each of creature_skeleton's, as character_cutout's (the far legs and
// wing behind the body).
[[nodiscard]] std::vector<CutoutBone> creature_cutout(const CreatureRecipe& recipe);

// The recipe a Creature3D's MeshInstance3D or a Creature2D names, read through the world's
// assets (kept until a file it was made from changes).
[[nodiscard]] Result<std::shared_ptr<const CreatureRecipe>> load_creature(const World& world,
                                                                          std::string_view path);

// The entity each Creature2D keeps for a bone of its cut-out: <id>_<bone>, cut to 64 characters.
[[nodiscard]] std::string creature_bone_id(std::string_view owner, std::string_view bone);

} // namespace slash::world
