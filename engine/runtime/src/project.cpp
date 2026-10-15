#include "slash/runtime/project.hpp"

#include "slash/platform/file.hpp"

#include <cctype>
#include <cstddef>
#include <utility>

namespace slash::runtime {
namespace {

constexpr int kMaxWindowSide = 16384;
constexpr double kMaxTickHz = 1000.0;

json::Value integer_property(std::string_view description, int fallback) {
    json::Value property;
    property["type"] = "integer";
    property["minimum"] = 1;
    property["maximum"] = kMaxWindowSide;
    property["default"] = fallback;
    property["description"] = description;
    return property;
}

json::Value string_property(std::string_view description) {
    json::Value property;
    property["type"] = "string";
    property["description"] = description;
    return property;
}

bool is_ascii_alnum(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

// Letters, digits and hyphens in at least two dot-separated parts, as Apple and Android require.
bool valid_id(std::string_view id) {
    std::size_t parts = 0;
    std::size_t start = 0;
    while (start <= id.size()) {
        std::size_t end = id.find('.', start);
        if (end == std::string_view::npos) {
            end = id.size();
        }
        const std::string_view part = id.substr(start, end - start);
        if (part.empty()) {
            return false;
        }
        for (const char c : part) {
            if (!is_ascii_alnum(c) && c != '-' && c != '_') {
                return false;
            }
        }
        ++parts;
        start = end + 1;
    }
    return parts >= 2;
}

// "major.minor.patch" with each part a number of at most five digits.
bool valid_version(std::string_view version) {
    std::size_t parts = 0;
    std::size_t digits = 0;
    for (const char c : version) {
        if (c == '.') {
            if (digits == 0) {
                return false;
            }
            ++parts;
            digits = 0;
        } else if (c >= '0' && c <= '9' && digits < 5) {
            ++digits;
        } else {
            return false;
        }
    }
    return digits > 0 && parts == 2;
}

} // namespace

std::string Project::binary_name() const {
    std::string binary;
    for (const char c : name) {
        if (is_ascii_alnum(c)) {
            binary += c;
        }
    }
    return binary.empty() ? std::string("Game") : binary;
}

std::string Project::bundle_id() const {
    if (!id.empty()) {
        return id;
    }
    std::string lowered = binary_name();
    for (char& c : lowered) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return "com.example." + lowered;
}

json::Value project_schema() {
    json::Value window;
    window["type"] = "object";
    window["additionalProperties"] = false;
    window["description"] = "Initial window size in pixels.";
    window["properties"]["width"] = integer_property("Window width.", Project{}.window_width);
    window["properties"]["height"] = integer_property("Window height.", Project{}.window_height);

    json::Value tick_hz;
    tick_hz["type"] = "number";
    tick_hz["exclusiveMinimum"] = 0;
    tick_hz["maximum"] = kMaxTickHz;
    tick_hz["default"] = Project{}.tick_hz;
    tick_hz["description"] = "Fixed simulation ticks per second.";

    json::Value clear_color;
    clear_color["type"] = "array";
    clear_color["items"]["type"] = "number";
    clear_color["items"]["minimum"] = 0;
    clear_color["items"]["maximum"] = 1;
    clear_color["minItems"] = 4;
    clear_color["maxItems"] = 4;
    clear_color["default"] = json::Array{0.3, 0.3, 0.3, 1.0}; // Project{}.clear_color
    clear_color["description"] =
        "What the window shows behind everything, as [r, g, b, a] from 0 to 1.";
    json::Value driver;
    driver["enum"] = json::Array{"auto", "vulkan", "metal", "d3d12", "null"};
    driver["default"] = Project{}.rendering_driver;
    driver["description"] = "The GPU backend. auto picks the best one on each machine; null draws "
                            "nothing.";
    json::Value rendering;
    rendering["type"] = "object";
    rendering["additionalProperties"] = false;
    rendering["description"] = "How the game draws.";
    rendering["properties"]["clear_color"] = std::move(clear_color);
    rendering["properties"]["driver"] = std::move(driver);

    json::Value name = string_property("The game's name, shown in its title bar. Exported files "
                                       "use it with only letters and digits kept.");
    name["minLength"] = 1;
    name["maxLength"] = 100;

    json::Value version = string_property("Version as major.minor.patch, e.g. \"1.2.0\".");
    version["default"] = Project{}.version;

    json::Value format;
    format["const"] = kProjectFormat;

    json::Value schema;
    schema["$schema"] = "https://json-schema.org/draft/2020-12/schema";
    schema["title"] = "SlashEngine project";
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["required"] = json::Array{"format", "name", "main_scene"};
    json::Value& properties = schema["properties"];
    properties["format"] = std::move(format);
    properties["name"] = std::move(name);
    properties["id"] = string_property("Reverse-DNS identifier for bundles and app stores, e.g. "
                                       "\"com.example.pong\". Default: com.example.<name>.");
    properties["version"] = std::move(version);
    properties["main_scene"] =
        string_property("The scene loaded at startup, relative to the project folder, e.g. "
                        "\"scenes/main.scene.json\".");
    properties["window"] = std::move(window);
    properties["tick_hz"] = std::move(tick_hz);
    properties["rendering"] = std::move(rendering);
    return schema;
}

std::vector<json::ValidationError> validate_project(const json::Value& project) {
    std::vector<json::ValidationError> errors = json::validate(project, project_schema());
    if (!errors.empty()) {
        return errors;
    }
    if (const json::Value* id = project.find("id"); id != nullptr && !valid_id(id->as_string())) {
        errors.push_back({"/id", "expected a reverse-DNS identifier such as "
                                 "\"com.example.pong\": letters, digits, '-' and '_' in two or "
                                 "more parts separated by dots"});
    }
    if (const json::Value* version = project.find("version");
        version != nullptr && !valid_version(version->as_string())) {
        errors.push_back({"/version", "expected major.minor.patch, e.g. \"1.2.0\""});
    }
    const std::string& main_scene = project.find("main_scene")->as_string();
    if (!is_project_path(main_scene)) {
        errors.push_back({"/main_scene", "expected a path inside the project folder, separated "
                                         "by '/', with no \".\" or \"..\" parts"});
    } else if (!main_scene.ends_with(".scene.json")) {
        errors.push_back({"/main_scene", "expected a .scene.json file"});
    }
    return errors;
}

Result<Project> parse_project(const json::Value& project) {
    if (const auto errors = validate_project(project); !errors.empty()) {
        const json::ValidationError& first = errors.front();
        return make_error(ErrorCode::InvalidArgument,
                          std::string(kProjectFile) + " " +
                              (first.path.empty() ? std::string("root") : first.path) + ": " +
                              first.message);
    }
    Project out;
    out.name = project.find("name")->as_string();
    out.main_scene = project.find("main_scene")->as_string();
    if (const json::Value* id = project.find("id"); id != nullptr) {
        out.id = id->as_string();
    }
    if (const json::Value* version = project.find("version"); version != nullptr) {
        out.version = version->as_string();
    }
    if (const json::Value* window = project.find("window"); window != nullptr) {
        if (const json::Value* width = window->find("width"); width != nullptr) {
            out.window_width = static_cast<int>(width->as_int());
        }
        if (const json::Value* height = window->find("height"); height != nullptr) {
            out.window_height = static_cast<int>(height->as_int());
        }
    }
    if (const json::Value* tick_hz = project.find("tick_hz"); tick_hz != nullptr) {
        out.tick_hz = tick_hz->as_double();
    }
    if (const json::Value* rendering = project.find("rendering"); rendering != nullptr) {
        if (const json::Value* color = rendering->find("clear_color"); color != nullptr) {
            const json::Array& channels = color->as_array();
            for (std::size_t i = 0; i < out.clear_color.size(); ++i) {
                out.clear_color.at(i) = static_cast<float>(channels.at(i).as_double());
            }
        }
        if (const json::Value* driver = rendering->find("driver"); driver != nullptr) {
            out.rendering_driver = driver->as_string();
        }
    }
    return out;
}

Result<Project> load_project(std::string_view folder) {
    const std::string path = join_path(folder, kProjectFile);
    auto text = platform::read_file(path);
    if (!text) {
        return make_error(ErrorCode::NotFound, "no " + std::string(kProjectFile) + " in \"" +
                                                   std::string(folder) +
                                                   R"("; run "slash-cli init" to create one)");
    }
    auto parsed = json::parse(text.value());
    if (!parsed) {
        return make_error(ErrorCode::InvalidArgument,
                          std::string(kProjectFile) + ": " + parsed.error().message);
    }
    return parse_project(parsed.value());
}

json::Value new_project(std::string_view name, std::string_view main_scene) {
    json::Value project;
    project["format"] = kProjectFormat;
    project["name"] = name;
    project["main_scene"] = main_scene;
    project["version"] = Project{}.version;
    return project;
}

std::string join_path(std::string_view folder, std::string_view relative) {
    std::string path(folder);
    if (!path.empty() && path.back() != '/' && path.back() != '\\') {
        path += '/';
    }
    path += relative;
    return path;
}

world::FileReader folder_reader(std::string folder) {
    return [folder = std::move(folder)](std::string_view path) -> Result<std::string> {
        if (!is_project_path(path)) {
            return make_error(ErrorCode::PermissionDenied,
                              "path \"" + std::string(path) +
                                  "\" is not inside the project folder");
        }
        return platform::read_file_in(folder, path);
    };
}

} // namespace slash::runtime
