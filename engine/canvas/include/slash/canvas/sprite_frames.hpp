#pragma once

#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/result.hpp"

#include <map>
#include <string>
#include <string_view>
#include <vector>

// .frames.json files: named flipbook animations, like Godot's SpriteFrames. AnimatedSprite2D
// plays them.
//
//   {
//     "animations": {
//       "idle": {"frames": ["sprites/idle_0.png", "sprites/idle_1.png"], "fps": 2},
//       "walk": {"texture": "sprites/hero.png", "hframes": 4, "frames": [0, 1, 2, 3], "fps": 10}
//     },
//     "format": 1
//   }
//
// A frame is a texture path (the whole image) or a frame number in the animation's sprite sheet
// ("texture", "hframes", "vframes"). Left out, "frames" is every frame of the sheet in order.
// Animations play at "fps" frames per second (default 5) and loop unless "loop" is false.
namespace slash::canvas {

inline constexpr std::string_view kSpriteFramesSuffix = ".frames.json";

struct SpriteFrame {
    std::string texture;
    int hframes = 1;
    int vframes = 1;
    int frame = 0;
};

struct Animation {
    std::vector<SpriteFrame> frames;
    double fps = 5.0;
    bool loop = true;
};

struct SpriteFrames {
    std::map<std::string, Animation, std::less<>> animations;
};

// JSON Schema for .frames.json files.
[[nodiscard]] json::Value sprite_frames_schema();

// Every error in a .frames.json document, each with the JSON Pointer of the bad value.
[[nodiscard]] std::vector<json::ValidationError> validate_sprite_frames(const json::Value& doc);

// The animations in a .frames.json document, or its first error.
[[nodiscard]] Result<SpriteFrames> parse_sprite_frames(const json::Value& doc);

} // namespace slash::canvas
