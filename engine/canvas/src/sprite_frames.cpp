#include "slash/canvas/sprite_frames.hpp"

#include "slash/core/text.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace slash::canvas {
namespace {

// A frame is at least a pixel, and a texture at most 16384 pixels on a side.
constexpr std::int64_t kMaxFrames = 16384;

json::Value integer(std::string_view description, std::int64_t minimum, std::int64_t fallback) {
    json::Value schema;
    schema["type"] = "integer";
    schema["minimum"] = minimum;
    schema["maximum"] = kMaxFrames;
    schema["default"] = fallback;
    schema["description"] = description;
    return schema;
}

json::Value text(std::string_view description) {
    json::Value schema;
    schema["type"] = "string";
    schema["minLength"] = 1;
    schema["description"] = description;
    return schema;
}

std::string path_error() {
    return "expected a path inside the project folder, separated by '/', with no \".\" or "
           "\"..\" parts";
}

} // namespace

json::Value sprite_frames_schema() {
    json::Value frame;
    frame["description"] = "A texture path (the whole image), or a frame number in the "
                           "animation's sprite sheet, counted along rows from 0.";
    const json::Value by_path = text("Texture path.");
    json::Value by_number;
    by_number["type"] = "integer";
    by_number["minimum"] = 0;
    frame["anyOf"] = json::Array{by_path, by_number};

    json::Value animation;
    animation["type"] = "object";
    animation["additionalProperties"] = false;
    json::Value& properties = animation["properties"];
    properties["texture"] = text("The sprite sheet that frame numbers refer to.");
    properties["hframes"] = integer("Columns of frames in the sheet.", 1, 1);
    properties["vframes"] = integer("Rows of frames in the sheet.", 1, 1);
    properties["frames"]["type"] = "array";
    properties["frames"]["minItems"] = 1;
    properties["frames"]["items"] = frame;
    properties["frames"]["description"] =
        "The frames in order. Left out, every frame of the sheet.";
    properties["fps"]["type"] = "number";
    properties["fps"]["minimum"] = 0;
    properties["fps"]["default"] = Animation{}.fps;
    properties["fps"]["description"] = "Frames per second.";
    properties["loop"]["type"] = "boolean";
    properties["loop"]["default"] = true;
    properties["loop"]["description"] = "Start again after the last frame.";

    json::Value schema;
    schema["$schema"] = "https://json-schema.org/draft/2020-12/schema";
    schema["title"] = "SlashEngine sprite frames";
    schema["description"] = "Named flipbook animations that AnimatedSprite2D plays, like "
                            "Godot's SpriteFrames.";
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["required"] = json::Array{"format", "animations"};
    schema["properties"]["format"]["const"] = 1;
    schema["properties"]["format"]["description"] = "Format version. Must be 1.";
    schema["properties"]["animations"]["type"] = "object";
    schema["properties"]["animations"]["minProperties"] = 1;
    schema["properties"]["animations"]["additionalProperties"] = animation;
    schema["properties"]["animations"]["description"] =
        "Animations by name; AnimatedSprite2D plays \"default\" unless told otherwise.";
    return schema;
}

std::vector<json::ValidationError> validate_sprite_frames(const json::Value& doc) {
    std::vector<json::ValidationError> errors = json::validate(doc, sprite_frames_schema());
    if (!errors.empty()) {
        return errors;
    }
    for (const auto& [name, animation] : doc.find("animations")->as_object()) {
        const std::string at = "/animations/" + json::pointer_escape(name);
        const json::Value* texture = animation.find("texture");
        if (texture != nullptr && !is_project_path(texture->as_string())) {
            errors.push_back({at + "/texture", path_error()});
        }
        const json::Value* hframes = animation.find("hframes");
        const json::Value* vframes = animation.find("vframes");
        const std::int64_t count = (hframes != nullptr ? hframes->as_int() : 1) *
                                   (vframes != nullptr ? vframes->as_int() : 1);
        const json::Value* frames = animation.find("frames");
        if (frames == nullptr) {
            if (texture == nullptr) {
                errors.push_back({at, R"(needs "frames", or a "texture" sheet to play)"});
            }
            continue;
        }
        const json::Array& list = frames->as_array();
        for (std::size_t i = 0; i < list.size(); ++i) {
            const std::string item = at + "/frames/" + std::to_string(i);
            if (list[i].is_string()) {
                if (!is_project_path(list[i].as_string())) {
                    errors.push_back({item, path_error()});
                }
            } else if (texture == nullptr) {
                errors.push_back({item, "a frame number needs the animation's \"texture\""});
            } else if (list[i].as_int() >= count) {
                errors.push_back(
                    {item, "the sheet has " + std::to_string(count) + " frames, numbered from 0"});
            }
        }
    }
    return errors;
}

Result<SpriteFrames> parse_sprite_frames(const json::Value& doc) {
    if (const auto errors = validate_sprite_frames(doc); !errors.empty()) {
        const json::ValidationError& first = errors.front();
        return make_error(ErrorCode::InvalidArgument,
                          (first.path.empty() ? std::string("root") : first.path) + ": " +
                              first.message);
    }
    SpriteFrames out;
    for (const auto& [name, json_animation] : doc.find("animations")->as_object()) {
        Animation animation;
        SpriteFrame sheet;
        if (const json::Value* texture = json_animation.find("texture"); texture != nullptr) {
            sheet.texture = texture->as_string();
        }
        if (const json::Value* hframes = json_animation.find("hframes"); hframes != nullptr) {
            sheet.hframes = static_cast<int>(hframes->as_int());
        }
        if (const json::Value* vframes = json_animation.find("vframes"); vframes != nullptr) {
            sheet.vframes = static_cast<int>(vframes->as_int());
        }
        if (const json::Value* fps = json_animation.find("fps"); fps != nullptr) {
            animation.fps = fps->as_double();
        }
        if (const json::Value* loop = json_animation.find("loop"); loop != nullptr) {
            animation.loop = loop->as_bool();
        }
        if (const json::Value* frames = json_animation.find("frames"); frames != nullptr) {
            for (const json::Value& item : frames->as_array()) {
                if (item.is_string()) {
                    animation.frames.push_back(
                        {.texture = item.as_string(), .hframes = 1, .vframes = 1, .frame = 0});
                } else {
                    SpriteFrame frame = sheet;
                    frame.frame = static_cast<int>(item.as_int());
                    animation.frames.push_back(frame);
                }
            }
        } else {
            for (int i = 0; i < sheet.hframes * sheet.vframes; ++i) {
                SpriteFrame frame = sheet;
                frame.frame = i;
                animation.frames.push_back(frame);
            }
        }
        out.animations.emplace(name, std::move(animation));
    }
    return out;
}

} // namespace slash::canvas
