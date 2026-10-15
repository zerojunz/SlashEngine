#pragma once

#include "slash/core/json.hpp"
#include "slash/core/math.hpp"
#include "slash/core/reflect.hpp"

#include <map>
#include <string>
#include <vector>

// Components every SlashEngine world understands. Names follow Godot (Node2D and Node3D carry
// the same transforms).
namespace slash::world {

class ComponentRegistry;

// Position, rotation and scale in 2D. Units are pixels, and y points down, as in Godot.
struct Transform2D {
    Vec2 position;
    float rotation = 0.0F; // radians, clockwise
    Vec2 scale{1.0F, 1.0F};
};
SLASH_COMPONENT(Transform2D, position, rotation, scale)

// Position, rotation and scale in 3D. Units are meters, and y points up, as in Godot.
struct Transform3D {
    Vec3 position;
    Quat rotation;
    Vec3 scale{1.0F, 1.0F, 1.0F};
};
SLASH_COMPONENT(Transform3D, position, rotation, scale)

// Ties an entity to the storyboard entries it stands for, such as the character it plays or the
// place it is, so a brief for the scene includes them and checks catch a misspelled ID.
struct StoryRef {
    std::vector<std::string> ids;
};
SLASH_COMPONENT(StoryRef, ids)

// A texture drawn at the entity's Transform2D, as Godot's Sprite2D. A texture can be a sprite
// sheet of hframes x vframes equal frames, of which frame is drawn, and region picks a part of
// it first. Sprites draw in order of z_index, then in the order their entities were made.
struct Sprite2D {
    std::string texture; // project path of a PNG or BMP file; "" draws nothing
    bool centered = true;
    Vec2 offset;
    bool flip_h = false;
    bool flip_v = false;
    int hframes = 1;
    int vframes = 1;
    int frame = 0;
    Vec4 region; // [x, y, width, height] in pixels; a zero size uses the whole texture
    Vec4 modulate{1.0F, 1.0F, 1.0F, 1.0F};
    int z_index = 0;
    bool visible = true;
    std::string texture_filter = "inherit"; // "inherit", "linear" or "nearest"
    std::string material;                   // project path of a .material.json file; "" for none
    std::map<std::string, json::Value> material_params; // values for the material's params
};
SLASH_COMPONENT(Sprite2D, texture, centered, offset, flip_h, flip_v, hframes, vframes, frame,
                region, modulate, z_index, visible, texture_filter, material, material_params)

// Plays animations from a .frames.json file (Godot's SpriteFrames) on the entity's Sprite2D,
// as Godot's AnimatedSprite2D: each tick it sets the sprite's texture, sheet and frame.
struct AnimatedSprite2D {
    std::string sprite_frames; // project path of a .frames.json file
    std::string animation = "default";
    bool playing = true;
    float speed_scale = 1.0F;
    int frame = 0;               // the animation's current frame
    float frame_progress = 0.0F; // how far through the current frame, from 0 to 1
};
SLASH_COMPONENT(AnimatedSprite2D, sprite_frames, animation, playing, speed_scale, frame,
                frame_progress)

// Plays keyframe animations from a .anim.json file on the entity's own components, as Godot's
// AnimationPlayer: each tick it moves the time on and sets every field the animation's tracks
// name.
struct AnimationPlayer {
    std::string library; // project path of a .anim.json file
    std::string animation;
    bool playing = true;
    float speed_scale = 1.0F;
    double time = 0.0;      // seconds into the animation
    bool backwards = false; // play toward the start; pingpong loops flip it at each end
};
SLASH_COMPONENT(AnimationPlayer, library, animation, playing, speed_scale, time, backwards)

// A grid of tiles from a .tileset.json file, as Godot's TileMapLayer, drawn at the entity's
// Transform2D with cell [0, 0]'s top left corner there. Its cells are text: each row is a string
// of keys, each key standing for the tile the legend names, and "." for an empty cell. The
// tilemap commands keep them in canonical form, so most edits go through them.
struct TileMapLayer {
    std::string tile_set;                      // project path of a .tileset.json file
    Vec2i origin;                              // the cell the first key of the first row stands for
    std::map<std::string, std::string> legend; // key -> tile: a tile's name or "source:x,y"
    std::vector<std::string> rows;
    int z_index = 0;
    bool visible = true;
    Vec4 modulate{1.0F, 1.0F, 1.0F, 1.0F};
    std::string texture_filter = "inherit"; // "linear", "nearest" or "inherit"
};
SLASH_COMPONENT(TileMapLayer, tile_set, origin, legend, rows, z_index, visible, modulate,
                texture_filter)

// Registers the components above, with their docs.
void register_builtin_components(ComponentRegistry& registry);

} // namespace slash::world
