// video.encode: image files (frame.capture's PNGs, or any the project has) to an .ivf video of
// AV1 through libaom (video::encode_lossy), so agents turn captured frames into cutscenes.

#include "slash/video/commands.hpp"

#include "slash/command/session.hpp"
#include "slash/platform/file.hpp"
#include "slash/render/image.hpp"
#include "slash/video/lossy_encoder.hpp"
#include "slash/video/video.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slash::video {
namespace {

using command::Capability;
using command::Reply;
using command::Session;

json::Value property(std::string_view type, std::string_view description) {
    json::Value schema;
    schema["type"] = type;
    schema["description"] = description;
    return schema;
}

json::Value integer(std::string_view description, int minimum, int maximum) {
    json::Value schema = property("integer", description);
    schema["minimum"] = minimum;
    schema["maximum"] = maximum;
    return schema;
}

Reply failure_at(int code, std::string_view path, const std::string& message) {
    json::Value data;
    data["path"] = path;
    return Reply::failure(code, std::string(path) + ": " + message, std::move(data));
}

Reply failure_at(const Error& error, std::string_view path) {
    return failure_at(Reply::failure(error).code, path, error.message);
}

int number(const json::Value& params, std::string_view key, int fallback) {
    const json::Value* value = params.find(key);
    return value != nullptr ? static_cast<int>(value->as_int()) : fallback;
}

bool is_image(std::string_view path) {
    std::string lower(path);
    for (char& c : lower) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return lower.ends_with(".png") || lower.ends_with(".jpg") || lower.ends_with(".jpeg") ||
           lower.ends_with(".bmp");
}

Reply video_encode(Session& session, const json::Value& params) {
    const std::string out = params.find("out")->as_string();
    if (!out.ends_with(".ivf")) {
        return failure_at(command::code::kInvalidArgument, "/out", "expected an .ivf file");
    }
    auto target = session.resolve_path(out);
    if (!target) {
        return failure_at(target.error(), "/out");
    }

    // Each entry is an image, or a folder ending in '/' whose images go in name order.
    std::vector<std::pair<std::string, std::string>> files; // project path, its JSON Pointer
    const json::Array& entries = params.find("frames")->as_array();
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const std::string& entry = entries[i].as_string();
        const std::string pointer = "/frames/" + std::to_string(i);
        if (!entry.ends_with('/')) {
            files.emplace_back(entry, pointer);
            continue;
        }
        auto folder = session.resolve_path(entry);
        if (!folder) {
            return failure_at(folder.error(), pointer);
        }
        auto listed = platform::list_files(folder.value(), [](std::string_view) { return false; });
        if (!listed) {
            return failure_at(listed.error(), pointer);
        }
        const std::size_t before = files.size();
        for (const std::string& name : listed.value()) {
            if (is_image(name)) {
                files.emplace_back(entry + name, pointer);
            }
        }
        if (files.size() == before) {
            return failure_at(command::code::kInvalidArgument, pointer,
                              "no .png, .jpg or .bmp files in " + entry);
        }
    }

    std::vector<render::Image> frames;
    frames.reserve(files.size());
    for (const auto& [path, pointer] : files) {
        auto read = session.read_project_file(path);
        if (!read) {
            return failure_at(read.error(), pointer);
        }
        if (!read.value()) {
            return failure_at(command::code::kNotFound, pointer, "no file " + path);
        }
        auto image = render::decode_image(*read.value());
        if (!image) {
            return failure_at(command::code::kInvalidArgument, pointer,
                              path + ": " + image.error().message);
        }
        if (!frames.empty() &&
            (image.value().width != frames[0].width || image.value().height != frames[0].height)) {
            return failure_at(command::code::kInvalidArgument, pointer,
                              path + " is " + std::to_string(image.value().width) + "x" +
                                  std::to_string(image.value().height) + ", not " +
                                  std::to_string(frames[0].width) + "x" +
                                  std::to_string(frames[0].height) + " as " + files[0].first +
                                  " is");
        }
        frames.push_back(std::move(image).value());
    }

    const int fps = number(params, "fps", 30);
    LossyOptions options;
    options.cq_level = number(params, "cq_level", options.cq_level);
    options.cpu_used = number(params, "cpu_used", options.cpu_used);
    options.kf_interval = static_cast<std::uint32_t>(number(params, "kf_interval", fps * 2));
    auto bytes = encode_lossy(frames, static_cast<std::uint32_t>(fps), 1, options);
    if (!bytes) {
        return Reply::failure(bytes.error());
    }
    auto file = read_ivf(bytes.value());
    if (!file) {
        return Reply::failure(file.error());
    }
    if (auto written = session.write_file(
            target.value(), std::string_view(reinterpret_cast<const char*>(bytes.value().data()),
                                             bytes.value().size()));
        !written) {
        return failure_at(written.error(), "/out");
    }

    json::Value result;
    result["path"] = out;
    result["frames"] = static_cast<std::int64_t>(frames.size());
    result["size"] = json::Array{static_cast<std::int64_t>(frames[0].width),
                                 static_cast<std::int64_t>(frames[0].height)};
    result["seconds"] = file.value().length();
    json::Array keys;
    for (std::size_t i = 0; i < file.value().frames.size(); ++i) {
        if (file.value().frames[i].key) {
            keys.emplace_back(static_cast<std::int64_t>(i));
        }
    }
    result["key_frames"] = std::move(keys);
    return Reply::success(std::move(result));
}

} // namespace

void register_commands(command::Registry& registry) {
    json::Value frames = property("array", "Image files in order (such as frame.capture's PNGs), "
                                           "or folders ending in '/' whose images go in name "
                                           "order. All one size.");
    frames["items"]["type"] = "string";
    frames["minItems"] = 1;
    json::Value params;
    params["type"] = "object";
    params["additionalProperties"] = false;
    params["required"] = json::Array{"frames", "out"};
    params["properties"]["frames"] = std::move(frames);
    params["properties"]["out"] = property("string", "The .ivf file to write.");
    params["properties"]["fps"] = integer("Frames a second. Default 30.", 1, 240);
    params["properties"]["cq_level"] =
        integer("Quantizer, 0 (finest) to 63 (smallest). Default 32.", 0, 63);
    params["properties"]["cpu_used"] =
        integer("libaom's speed, 0 (slowest, smallest) to 9. Default 6.", 0, 9);
    params["properties"]["kf_interval"] =
        integer("A key frame every this many frames, which seeking starts from; 0 for the "
                "first only. Default two seconds' worth.",
                0, 100000);
    registry.add({
        .name = "video.encode",
        .description = "Encodes image files, such as the frames frame.capture writes, as a lossy "
                       "AV1 .ivf video with libaom for a VideoStreamPlayer. Returns its frames, "
                       "size, seconds and key frames.",
        .capabilities = {Capability::Read, Capability::Filesystem},
        .effect = command::Effect::Read,
        .params = std::move(params),
        .handler = video_encode,
    });
}

} // namespace slash::video
