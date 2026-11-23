// texture.inspect, texture.edit and texture.check: reading, changing and checking texture graphs
// (.texture.json files of "type": "graph") as text, so an agent tunes one without looking at it.

#include "procedural.hpp"

#include "slash/command/session.hpp"
#include "slash/core/text.hpp"
#include "slash/platform/file.hpp"
#include "slash/procedural/texture.hpp"
#include "slash/procedural/texture_graph.hpp"
#include "slash/render/image.hpp"
#include "slash/storyboard/storyboard.hpp"
#include "slash/world/ragdoll.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slash::command {
namespace {

constexpr std::array<std::string_view, 8> kMeasures{"value", "r",   "g",    "b",
                                                    "a",     "low", "high", "contrast"};
constexpr std::string_view kTiles = "tiles";

json::Value property(std::string_view type, std::string_view description) {
    json::Value schema;
    schema["type"] = type;
    schema["description"] = description;
    return schema;
}

json::Value params_schema(json::Object properties, json::Array required = {}) {
    json::Value schema;
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["properties"] = std::move(properties);
    if (!required.empty()) {
        schema["required"] = std::move(required);
    }
    return schema;
}

std::string quoted(std::string_view name) {
    return "\"" + std::string(name) + "\"";
}

Reply param_error(int error_code, std::string_view path, const std::string& message) {
    json::Value data;
    data["path"] = path;
    return Reply::failure(error_code, std::string(path) + ": " + message, std::move(data));
}

// Rounded to three decimals, whole numbers without ".0".
json::Value short_number(double value) {
    const double out = std::round(value * 1000.0) / 1000.0;
    if (out == std::floor(out)) {
        return static_cast<std::int64_t>(out);
    }
    return out;
}

// Whether an image tiles across one axis: the step from its last column (or row) to its first is
// no larger than the largest step between neighbors inside it, plus one level, so a brick wall
// whose edges fall on joints tiles and noise cut off at the edge doesn't.
bool tiles(const render::Image& image, bool across) {
    const auto width = static_cast<std::size_t>(image.width);
    const auto height = static_cast<std::size_t>(image.height);
    const std::size_t lines = across ? width : height;
    const std::size_t along = across ? height : width;
    if (lines < 2 || along == 0) {
        return true;
    }
    const auto step = [&](std::size_t line, std::size_t next) {
        std::int64_t total = 0;
        for (std::size_t i = 0; i < along; ++i) {
            const std::size_t a = across ? (i * width) + line : (line * width) + i;
            const std::size_t b = across ? (i * width) + next : (next * width) + i;
            for (std::size_t c = 0; c < 4; ++c) {
                total += std::abs(int{image.pixels[(a * 4) + c]} - int{image.pixels[(b * 4) + c]});
            }
        }
        return static_cast<double>(total) / static_cast<double>(along * 4);
    };
    double largest = 0.0;
    for (std::size_t line = 0; line + 1 < lines; ++line) {
        largest = std::max(largest, step(line, line + 1));
    }
    return step(lines - 1, 0) <= largest + 1.0;
}

// What texture.check holds an output to, each from 0 to 1: its mean luminance ("value"), each
// channel's mean, the darkest and brightest luminance, its contrast (the luminance's standard
// deviation) and whether it tiles (see `tiles`).
json::Value measure(const render::Image& image) {
    const std::size_t count = image.pixels.size() / 4;
    std::array<double, 4> sums{};
    double luminance_sum = 0.0;
    double luminance_squares = 0.0;
    double low = 1.0;
    double high = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint8_t* pixel = &image.pixels[i * 4];
        for (std::size_t c = 0; c < 4; ++c) {
            sums.at(c) += pixel[c];
        }
        const double luminance =
            ((0.2126 * pixel[0]) + (0.7152 * pixel[1]) + (0.0722 * pixel[2])) / 255.0;
        luminance_sum += luminance;
        luminance_squares += luminance * luminance;
        low = std::min(low, luminance);
        high = std::max(high, luminance);
    }
    const double n = count > 0 ? static_cast<double>(count) : 1.0;
    const double mean = luminance_sum / n;
    json::Value out;
    out["value"] = short_number(mean);
    constexpr std::array<std::string_view, 4> kChannels{"r", "g", "b", "a"};
    for (std::size_t c = 0; c < 4; ++c) {
        out[kChannels.at(c)] = short_number(sums.at(c) / n / 255.0);
    }
    out["low"] = short_number(count > 0 ? low : 0.0);
    out["high"] = short_number(high);
    out["contrast"] =
        short_number(std::sqrt(std::max(0.0, (luminance_squares / n) - (mean * mean))));
    out[kTiles] = tiles(image, true) && tiles(image, false);
    return out;
}

// A graph file's document, read from the project (staged text first).
struct GraphFile {
    std::optional<std::string> text;
    json::Value doc;
};

// Reads a graph file, or the reply to fail with; at is the param naming it.
std::optional<Reply> read_graph(Session& session, const std::string& path, std::string_view at,
                                bool may_be_missing, GraphFile& out) {
    if (!path.ends_with(procedural::kTextureSuffix)) {
        return param_error(code::kInvalidArgument, at,
                           "expected a .texture.json file, e.g. \"textures/wall.texture.json\"");
    }
    auto text = session.read_project_file(path);
    if (!text) {
        return param_error(code::kInvalidArgument, at, text.error().message);
    }
    out.text = std::move(text).value();
    if (!out.text) {
        if (may_be_missing) {
            out.doc = json::Object{};
            return std::nullopt;
        }
        return param_error(code::kNotFound, at, "the project has no file " + quoted(path));
    }
    auto doc = json::parse(*out.text);
    if (!doc) {
        return param_error(code::kInvalidArgument, at,
                           quoted(path) + " is not valid JSON: " + doc.error().message);
    }
    if (!procedural::is_texture_graph(doc.value())) {
        return param_error(
            code::kInvalidArgument, at,
            quoted(path) +
                R"( is not a texture graph ("type": "graph"); texture.generate makes and measures the others)");
    }
    out.doc = std::move(doc).value();
    return std::nullopt;
}

// The first of a graph's errors as a reply, pointing into the file.
Reply graph_errors(const std::string& file, std::string_view at,
                   const std::vector<json::ValidationError>& errors) {
    const json::ValidationError& first = errors.front();
    return param_error(
        code::kInvalidArgument, at,
        quoted(file) + (first.path.empty() ? "" : " at " + first.path) + ": " + first.message +
            (errors.size() > 1
                 ? " (and " + std::to_string(errors.size() - 1) + " more; texture.check lists them)"
                 : ""));
}

// Every output of a graph, baked, by name.
Result<std::vector<std::pair<std::string, render::Image>>> bake_all(Session& session,
                                                                    const json::Value& doc) {
    auto compiled = procedural::compile_texture_graph(doc, graph_reader(session));
    if (!compiled) {
        return compiled.error();
    }
    auto baked = procedural::bake_texture_graph(compiled.value(), procedural::graph_device());
    if (!baked) {
        return baked.error();
    }
    std::vector<std::pair<std::string, render::Image>> out;
    for (std::size_t i = 0; i < baked.value().size(); ++i) {
        out.emplace_back(compiled.value().outputs[i].name, std::move(baked.value()[i]));
    }
    return out;
}

// What changed between two graph documents: each parameter, node and output list, by pointer.
json::Array changes(const json::Value& before, const json::Value& after) {
    json::Array out;
    const auto keys_of = [](const json::Value& doc) {
        std::vector<std::string> keys;
        if (doc.is_object()) {
            for (const auto& [key, value] : doc.as_object()) {
                keys.push_back(key);
            }
        }
        return keys;
    };
    std::vector<std::string> keys = keys_of(before);
    for (std::string& key : keys_of(after)) {
        if (std::ranges::find(keys, key) == keys.end()) {
            keys.push_back(std::move(key));
        }
    }
    std::ranges::sort(keys);
    for (const std::string& key : keys) {
        const json::Value* a = before.is_object() ? before.find(key) : nullptr;
        const json::Value* b = after.is_object() ? after.find(key) : nullptr;
        if ((a != nullptr) && (b != nullptr) && *a == *b) {
            continue;
        }
        const bool by_name = key == "nodes" || key == "parameters" || key == "expect";
        if (by_name && a != nullptr && b != nullptr && a->is_object() && b->is_object()) {
            for (const json::Value& inner : changes(*a, *b)) {
                out.emplace_back("/" + key + inner.as_string());
            }
            continue;
        }
        out.emplace_back("/" + json::pointer_escape(key));
    }
    return out;
}

Reply texture_inspect(Session& session, const json::Value& params) {
    const json::Value* path = params.find("path");
    const json::Value* inline_graph = params.find("texture");
    if ((path == nullptr) == (inline_graph == nullptr)) {
        return param_error(code::kInvalidParams, "", R"(give "path" or "texture", not both)");
    }
    json::Value doc;
    std::string file = "(texture)";
    if (path != nullptr) {
        file = path->as_string();
        GraphFile read;
        if (auto failed = read_graph(session, file, "/path", false, read)) {
            return *failed;
        }
        doc = std::move(read.doc);
    } else {
        doc = *inline_graph;
        if (!procedural::is_texture_graph(doc)) {
            return param_error(code::kInvalidArgument, "/texture",
                               R"(expected a texture graph ("type": "graph"))");
        }
    }
    const std::string at = path != nullptr ? "/path" : "/texture";
    if (const std::vector<json::ValidationError> errors = procedural::validate_texture_graph(doc);
        !errors.empty()) {
        return graph_errors(file, at, errors);
    }
    auto described = procedural::describe_texture_graph(doc, graph_reader(session));
    if (!described) {
        return param_error(code::kInvalidArgument, at,
                           quoted(file) + ": " + described.error().message);
    }
    json::Value result = std::move(described).value();
    if (path != nullptr) {
        result["path"] = file;
    }
    return Reply::success(std::move(result));
}

Reply texture_edit(Session& session, const json::Value& params) {
    const std::string& path = params.find("path")->as_string();
    GraphFile read;
    if (auto failed = read_graph(session, path, "/path", true, read)) {
        return *failed;
    }
    const json::Value& patch = *params.find("patch");
    json::Value doc = world::merge_patch(read.doc, patch);
    if (!read.text) {
        // A new graph: its format and type may go without saying.
        if (doc.find("format") == nullptr) {
            doc["format"] = 1;
        }
        if (doc.find("type") == nullptr) {
            doc["type"] = "graph";
        }
    }
    if (!procedural::is_texture_graph(doc)) {
        return param_error(code::kInvalidArgument, "/patch/type",
                           R"(a texture graph's type stays "graph")");
    }
    if (std::vector<json::ValidationError> errors = procedural::validate_texture_graph(doc);
        !errors.empty()) {
        for (json::ValidationError& error : errors) {
            error.message = "the graph it makes, at " +
                            (error.path.empty() ? std::string("its top") : error.path) + ": " +
                            error.message;
            error.path = "/patch" + error.path;
        }
        return Reply::invalid_params(errors);
    }
    // What it reads (what it extends, its images) must be there, and the graph must compile.
    std::optional<std::vector<std::pair<std::string, render::Image>>> baked;
    const json::Value* measure_param = params.find("measure");
    if (measure_param == nullptr || measure_param->as_bool()) {
        auto made = bake_all(session, doc);
        if (!made) {
            return param_error(code::kInvalidArgument, "/patch",
                               "the graph it makes: " + made.error().message);
        }
        baked = std::move(made).value();
    } else if (auto compiled = procedural::compile_texture_graph(doc, graph_reader(session));
               !compiled) {
        return param_error(code::kInvalidArgument, "/patch",
                           "the graph it makes: " + compiled.error().message);
    }

    json::Value result;
    result["path"] = path;
    result["changed"] = changes(read.doc, doc);
    std::string written = json::write(doc);
    if (read.text && written == *read.text) {
        session.mark_unchanged();
    } else {
        if (auto staged = session.stage_file(path, written); !staged) {
            return Reply::failure(staged.error());
        }
        session.add_undo_file(path, read.text, std::move(written));
    }
    if (baked) {
        json::Value outputs = json::Object{};
        for (const auto& [name, image] : *baked) {
            outputs[name] = measure(image);
        }
        result["outputs"] = std::move(outputs);
    }
    if (auto described = procedural::describe_texture_graph(doc, graph_reader(session));
        described) {
        if (json::Value* unused = described.value().find("unused"); unused != nullptr) {
            result["unused"] = std::move(*unused);
        }
    }
    return Reply::success(std::move(result));
}

// One problem texture.check found.
json::Value problem(const std::string& file, std::string path, std::string message) {
    json::Value item;
    item["file"] = file;
    item["path"] = std::move(path);
    item["message"] = std::move(message);
    return item;
}

// Holds one graph's outputs to its "expect"; adds what breaks it to errors.
void check_expect(const std::string& file, const json::Value& expect,
                  const std::vector<std::pair<std::string, render::Image>>& baked,
                  json::Array& errors) {
    std::vector<std::string> outputs;
    outputs.reserve(baked.size());
    for (const auto& [name, image] : baked) {
        outputs.push_back(name);
    }
    std::vector<std::pair<std::string, json::Value>> measured;
    for (const auto& [key, wanted] : expect.as_object()) {
        const std::string at = "/expect/" + json::pointer_escape(key);
        const std::size_t dot = key.rfind('.');
        const std::string output = dot == std::string::npos ? std::string() : key.substr(0, dot);
        const std::string what = dot == std::string::npos ? key : key.substr(dot + 1);
        const auto found = std::ranges::find(outputs, output);
        if (found == outputs.end()) {
            std::string message = dot == std::string::npos
                                      ? "expected \"<output>.<measure>\""
                                      : quoted(output) + " isn't an output of the graph";
            if (const std::string_view near = closest_match(output, outputs); !near.empty()) {
                message += " (did you mean " + quoted(near) + "?)";
            }
            errors.push_back(problem(file, at, std::move(message)));
            continue;
        }
        const bool tiles = what == kTiles;
        if (!tiles && std::ranges::find(kMeasures, what) == kMeasures.end()) {
            std::vector<std::string_view> names(kMeasures.begin(), kMeasures.end());
            names.push_back(kTiles);
            std::string message = quoted(what) + " isn't a measure";
            if (const std::string_view near = closest_match(what, names); !near.empty()) {
                message += " (did you mean " + quoted(near) + "?)";
            }
            errors.push_back(problem(file, at, std::move(message)));
            continue;
        }
        if (tiles != wanted.is_bool()) {
            errors.push_back(problem(file, at,
                                     tiles ? "expected true or false"
                                           : "expected [min, max], each from 0 to 1"));
            continue;
        }
        const std::size_t index = static_cast<std::size_t>(found - outputs.begin());
        auto cached =
            std::ranges::find_if(measured, [&](const auto& m) { return m.first == output; });
        if (cached == measured.end()) {
            measured.emplace_back(output, measure(baked[index].second));
            cached = measured.end() - 1;
        }
        const json::Value& value = *cached->second.find(what);
        if (tiles) {
            if (value.as_bool() != wanted.as_bool()) {
                errors.push_back(
                    problem(file, at,
                            quoted(output) + (value.as_bool() ? " tiles, which it shouldn't"
                                                              : " doesn't tile: its edges don't "
                                                                "meet")));
            }
            continue;
        }
        const double low = wanted.as_array()[0].as_double();
        const double high = wanted.as_array()[1].as_double();
        const double got = value.as_double();
        if (got < low || got > high) {
            errors.push_back(problem(
                file, at,
                quoted(output) + "'s " + what + " is " + json::write(value, json::Style::Compact) +
                    ", outside [" + json::write(wanted.as_array()[0], json::Style::Compact) + ", " +
                    json::write(wanted.as_array()[1], json::Style::Compact) + "]"));
        }
    }
}

Reply texture_check(Session& session, const json::Value& params) {
    std::vector<std::string> files;
    if (const json::Value* paths = params.find("paths"); paths != nullptr) {
        for (const json::Value& path : paths->as_array()) {
            files.push_back(path.as_string());
        }
    } else {
        auto listed =
            platform::list_files(session.options().project_root,
                                 [](std::string_view folder) { return folder != "build/"; });
        if (!listed) {
            return Reply::failure(listed.error());
        }
        for (const std::string& file : listed.value()) {
            if (file.ends_with(procedural::kTextureSuffix)) {
                files.push_back(file);
            }
        }
    }

    json::Array errors;
    std::int64_t checked = 0;
    std::vector<std::string> graphs;
    for (std::size_t i = 0; i < files.size(); ++i) {
        const std::string& file = files[i];
        GraphFile read;
        const bool named = params.find("paths") != nullptr;
        if (auto failed = read_graph(session, file, "/paths/" + std::to_string(i), false, read)) {
            if (named) {
                return *failed;
            }
            continue; // another kind of .texture.json, or one texture.generate reports on
        }
        ++checked;
        std::vector<json::ValidationError> invalid = procedural::validate_texture_graph(read.doc);
        if (!invalid.empty()) {
            for (json::ValidationError& error : invalid) {
                errors.push_back(problem(file, std::move(error.path), std::move(error.message)));
            }
            continue;
        }
        auto baked = bake_all(session, read.doc);
        if (!baked) {
            const std::string& message = baked.error().message;
            const std::size_t colon = message.find(": ");
            const bool pointed = message.starts_with('/') && colon != std::string::npos;
            errors.push_back(problem(file, pointed ? message.substr(0, colon) : std::string(),
                                     pointed ? message.substr(colon + 2) : message));
            continue;
        }
        graphs.push_back(file);
        if (const json::Value* expect = read.doc.find("expect"); expect != nullptr) {
            check_expect(file, *expect, baked.value(), errors);
        }
    }

    // The graphs' first outputs, the textures their bare paths name, against the storyboard's
    // palettes and budgets that cover them.
    if (!graphs.empty()) {
        auto loaded = storyboard::load(
            [&session](std::string_view path) { return session.read_project_file(path); });
        if (loaded && !loaded.value().entries.empty()) {
            std::vector<storyboard::FileError> broken;
            storyboard::check_textures(
                loaded.value().entries, graphs,
                [&session](std::string_view path) -> std::optional<storyboard::Texture> {
                    auto text = session.read_project_file(path);
                    if (!text || !text.value()) {
                        return std::nullopt;
                    }
                    auto doc = json::parse(*text.value());
                    if (!doc) {
                        return std::nullopt;
                    }
                    auto image = procedural::texture_graph_image(
                        doc.value(), "", graph_reader(session), procedural::graph_device());
                    if (!image) {
                        return std::nullopt;
                    }
                    return storyboard::Texture{.width = image.value().width,
                                               .height = image.value().height,
                                               .pixels = std::move(image.value().pixels)};
                },
                broken);
            for (storyboard::FileError& error : broken) {
                errors.push_back(
                    problem(error.file, std::move(error.path), std::move(error.message)));
            }
        }
    }

    json::Value result;
    result["checked"] = checked;
    result["errors"] = std::move(errors);
    return Reply::success(std::move(result));
}

} // namespace

void register_texture_tools(Registry& registry) {
    using enum Capability;
    json::Value graph =
        property("object", R"(A texture graph document ("type": "graph") to read without a file.)");
    registry.add({
        .name = "texture.inspect",
        .description =
            "What a texture graph (.texture.json of \"type\": \"graph\") is made of, with what it "
            "extends merged in: size, tiling, each parameter's value and the nodes reading it, "
            "each node's op, the nodes it reads and the parameters it uses, each output's node "
            "and the nodes it needs in bake order, and \"unused\" nodes. No pixels.",
        .capabilities = {Read},
        .effect = Effect::Read,
        .params = params_schema({
            {"path", property("string", "A graph file.")},
            {"texture", std::move(graph)},
        }),
        .handler = texture_inspect,
    });

    json::Value patch = property(
        "object",
        "A JSON merge patch over the graph file: {\"parameters\": {\"mortar\": 0.08}, \"nodes\": "
        "{\"grain\": {\"noise\": {\"frequency\": 0.04}}, \"old\": null}}; null removes. Lists, "
        "such as outputs, are replaced whole.");
    registry.add({
        .name = "texture.edit",
        .description =
            "Changes a texture graph file (or makes one) by a merge patch: parameters, nodes, "
            "links and outputs, checked before anything is written. Returns the changed "
            "pointers and, unless \"measure\" is false, each output's measures (value, r, g, b, "
            "a, low, high, contrast from 0 to 1, and tiles).",
        .capabilities = {Edit, Filesystem},
        .effect = Effect::Edit,
        .params = params_schema(
            {
                {"path", property("string", "The .texture.json graph file.")},
                {"patch", std::move(patch)},
                {"measure",
                 property("boolean", "Bake and measure the outputs after (default true).")},
            },
            json::Array{"path", "patch"}),
        .handler = texture_edit,
    });

    json::Value paths = property("array", "Graph files to check (default: every one).");
    paths["items"]["type"] = "string";
    registry.add({
        .name = "texture.check",
        .description =
            "Checks the project's texture graphs: every error in each, what it reads, its "
            "outputs against its own \"expect\" ranges, and its texture against the "
            "storyboard's palettes and budgets. Returns problems with file and JSON Pointer.",
        .capabilities = {Read},
        .effect = Effect::Read,
        .params = params_schema({{"paths", std::move(paths)}}),
        .handler = texture_check,
    });
}

} // namespace slash::command
