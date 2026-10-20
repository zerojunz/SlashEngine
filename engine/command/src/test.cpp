#include "slash/command/test.hpp"

#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/log.hpp"
#include "slash/core/text.hpp"
#include "slash/platform/file.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slash::command {
namespace {

// The longest value a failure's message quotes whole, in bytes.
constexpr std::size_t kQuoteLength = 60;

// In the world of a session that runs a test, so a test can't run tests.
struct RunningTest {};

json::Value typed(std::string_view type, std::string_view description) {
    json::Value schema;
    schema["type"] = type;
    schema["description"] = description;
    return schema;
}

json::Value names_schema(std::string_view description) {
    json::Value schema = typed("array", description);
    schema["items"]["type"] = "string";
    schema["items"]["minLength"] = 1;
    schema["minItems"] = 1;
    return schema;
}

std::string quoted(std::string_view name) {
    return "\"" + std::string(name) + "\"";
}

// A value as a message quotes it: compact JSON, cut short when it is long.
std::string quote(const json::Value& value) {
    std::string text = json::write(value, json::Style::Compact);
    if (text.size() <= kQuoteLength) {
        return text;
    }
    std::size_t end = kQuoteLength - 3;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0U) == 0x80U) {
        --end; // not inside a UTF-8 character
    }
    text.resize(end);
    return text + "...";
}

std::string items(std::size_t count) {
    return std::to_string(count) + (count == 1 ? " item" : " items");
}

// Watches the log while tests run: keeps the first warning or error since it was last asked,
// and passes every line on to the sink it replaced.
class LogWatch {
public:
    LogWatch() : previous_(log::sink()) {
        log::set_sink([this](log::Level level, std::string_view line) {
            if (level >= log::Level::Warn) {
                const std::scoped_lock lock(mutex_);
                if (!first_) {
                    first_ = std::string(line);
                }
            }
            previous_(level, line);
        });
    }
    ~LogWatch() { log::set_sink(previous_); }
    LogWatch(const LogWatch&) = delete;
    LogWatch& operator=(const LogWatch&) = delete;
    LogWatch(LogWatch&&) = delete;
    LogWatch& operator=(LogWatch&&) = delete;

    // The first warning or error logged since the last call, if one was.
    std::optional<std::string> take() {
        const std::scoped_lock lock(mutex_);
        return std::exchange(first_, std::nullopt);
    }

private:
    log::Sink previous_;
    std::mutex mutex_;
    std::optional<std::string> first_;
};

struct TestFile {
    std::string path;
    json::Value doc;
};

// What a test's session may do: what the host's may, but write files or export.
SessionOptions test_options(const Session& host) {
    SessionOptions options = host.options();
    options.capture_logs = false;
    options.write_files = false;
    std::erase(options.capabilities, Capability::Build); // exports write files of their own
    return options;
}

// Runs one test in a session of its own. Returns its failed check, if one failed.
class TestRun {
public:
    TestRun(const Session& host, const TestFile& file, const std::string& name, LogWatch& watch)
        : file_(file), name_(name), base_("/tests/" + json::pointer_escape(name)), watch_(watch),
          session_(host.components(), host.commands(), test_options(host)) {
        session_.world().ecs().ctx().emplace<RunningTest>();
    }

    std::optional<json::Value> run() {
        const json::Value& test = *file_.doc.find("tests")->find(name_);
        // The test's own scene, else the file's.
        std::string at = base_;
        const json::Value* scene = test.find("scene");
        if (scene != nullptr) {
            at += "/scene";
        } else if (scene = file_.doc.find("scene"); scene != nullptr) {
            at = "/scene";
        }
        if (scene != nullptr) {
            json::Value params;
            params["path"] = scene->as_string();
            if (Reply opened = session_.call("scene.open", params); !opened.ok) {
                return failure(at, std::move(opened.message));
            }
        }
        if (auto line = watch_.take()) {
            return logged(at, *line);
        }
        const json::Array& steps = test.find("steps")->as_array();
        for (std::size_t i = 0; i < steps.size(); ++i) {
            const json::Value& step = steps[i];
            const std::string step_at = base_ + "/steps/" + std::to_string(i);
            const json::Value* params = step.find("params");
            const Reply reply = session_.call(step.find("command")->as_string(),
                                              params != nullptr ? *params : json::Object{});
            if (!reply.ok) {
                return command_failure(step_at, reply);
            }
            if (auto line = watch_.take()) {
                return logged(step_at, *line);
            }
            if (const json::Value* expect = step.find("expect"); expect != nullptr) {
                if (auto differs = mismatch(*expect, reply.value, step_at + "/expect")) {
                    return failure(std::move(differs->path), std::move(differs->message));
                }
            }
        }
        return std::nullopt;
    }

private:
    json::Value failure(std::string path, std::string message) const {
        json::Value item;
        item["file"] = file_.path;
        item["test"] = name_;
        item["path"] = std::move(path);
        item["tick"] = session_.world().tick();
        item["message"] = std::move(message);
        return item;
    }

    // A command that failed, pointed at the bad value in its params when the error names one.
    json::Value command_failure(const std::string& at, const Reply& reply) const {
        if (reply.code == code::kMethodNotFound) {
            return failure(at + "/command", reply.message);
        }
        const json::Value* where = reply.data.find("path");
        if (const json::Value* errors = reply.data.find("errors");
            where == nullptr && errors != nullptr && errors->is_array() && errors->size() > 0) {
            where = errors->as_array().front().find("path");
        }
        if (where == nullptr || !where->is_string()) {
            return failure(at, reply.message);
        }
        // Errors about params start with where they are, which the failure's path says.
        std::string message = reply.message;
        const std::string prefix =
            (where->as_string().empty() ? std::string("params") : where->as_string()) + ": ";
        if (message.starts_with(prefix)) {
            message.erase(0, prefix.size());
        }
        return failure(at + "/params" + where->as_string(), std::move(message));
    }

    json::Value logged(const std::string& at, const std::string& line) const {
        auto parsed = json::parse(line);
        if (!parsed || !parsed.value().is_object()) {
            return failure(at, "logged: " + line);
        }
        const json::Value& entry = parsed.value();
        const json::Value* level = entry.find("level");
        const json::Value* message = entry.find("msg");
        const bool error = level != nullptr && level->is_string() && level->as_string() == "error";
        json::Value item =
            failure(at, std::string(error ? "logged an error: " : "logged a warning: ") +
                            (message != nullptr && message->is_string() ? message->as_string()
                                                                        : std::string()));
        item["log"] = std::move(parsed).value();
        return item;
    }

    const TestFile& file_;
    const std::string& name_;
    std::string base_;
    LogWatch& watch_;
    Session session_;
};

// The .test.json files a test.run call names, or every one in the project.
Result<std::vector<std::string>> test_files(const Session& session, const json::Value* paths,
                                            Reply& failure) {
    const auto outside_build = [](std::string_view folder) { return folder != "build/"; };
    std::vector<std::string> files;
    const auto add_folder = [&](const std::string& folder, const std::string& prefix) -> bool {
        auto listed = platform::list_files(folder, outside_build);
        if (!listed) {
            return false;
        }
        for (const std::string& path : listed.value()) {
            if (path.ends_with(kTestSuffix)) {
                files.push_back(prefix + path);
            }
        }
        return true;
    };
    if (paths == nullptr) {
        if (!add_folder(session.options().project_root, "")) {
            failure = Reply::failure(code::kPlatformFailure, "could not list the project's files");
            return make_error(ErrorCode::PlatformFailure, failure.message);
        }
        return files;
    }
    for (std::size_t i = 0; i < paths->size(); ++i) {
        const std::string pointer = "/paths/" + std::to_string(i);
        const auto fail = [&](int error_code, const std::string& message) {
            json::Value data;
            data["path"] = pointer;
            std::string located = pointer;
            located += ": ";
            located += message;
            failure = Reply::failure(error_code, std::move(located), std::move(data));
            return make_error(ErrorCode::InvalidArgument, message);
        };
        std::string path = paths->as_array()[i].as_string();
        while (path.size() > 1 && path.back() == '/') {
            path.pop_back();
        }
        auto full = session.resolve_path(path);
        if (!full) {
            return fail(Reply::failure(full.error()).code, full.error().message);
        }
        if (path.ends_with(kTestSuffix)) {
            if (!platform::exists(full.value())) {
                auto read = platform::read_file_in(session.options().project_root, path);
                return fail(code::kNotFound,
                            read ? "no such file" : quoted(path) + ": " + read.error().message);
            }
            files.push_back(path);
            continue;
        }
        if (!add_folder(full.value(), path == "." ? "" : path + "/")) {
            return fail(code::kNotFound,
                        "no folder or .test.json file " + quoted(path) + " in the project");
        }
    }
    std::ranges::sort(files);
    const auto repeated = std::ranges::unique(files);
    files.erase(repeated.begin(), repeated.end());
    return files;
}

json::Value file_failure(const std::string& file, std::string path, std::string message) {
    json::Value item;
    item["file"] = file;
    item["path"] = std::move(path);
    item["message"] = std::move(message);
    return item;
}

Reply test_run(Session& session, const json::Value& params) {
    if (session.world().ecs().ctx().contains<RunningTest>()) {
        return Reply::failure(code::kInvalidArgument, "a test can't run tests");
    }
    const json::Value* paths = params.find("paths");
    Reply failure;
    auto files = test_files(session, paths, failure);
    if (!files) {
        return failure;
    }

    // A test file that can't be read or isn't valid is a failed check, and the rest still run.
    json::Array failed;
    std::vector<TestFile> loaded;
    for (const std::string& path : files.value()) {
        auto text = session.read_project_file(path);
        if (!text || !text.value()) {
            failed.push_back(
                file_failure(path, "", text ? std::string("no such file") : text.error().message));
            continue;
        }
        auto doc = json::parse(*text.value());
        if (!doc) {
            failed.push_back(file_failure(path, "", "not valid JSON: " + doc.error().message));
            continue;
        }
        const auto errors = validate_test(doc.value());
        for (const json::ValidationError& error : errors) {
            failed.push_back(file_failure(path, error.path, error.message));
        }
        if (errors.empty()) {
            loaded.push_back({path, std::move(doc).value()});
        }
    }

    // Names to run, each of which must name a test.
    std::vector<std::string> wanted;
    if (const json::Value* names = params.find("tests"); names != nullptr) {
        std::vector<std::string> all;
        for (const TestFile& file : loaded) {
            for (const auto& [name, test] : file.doc.find("tests")->as_object()) {
                all.push_back(name);
            }
        }
        for (std::size_t i = 0; i < names->size(); ++i) {
            const std::string& name = names->as_array()[i].as_string();
            if (std::ranges::find(all, name) == all.end()) {
                const std::string pointer = "/tests/" + std::to_string(i);
                std::string message = pointer + ": no test is named " + quoted(name);
                if (const std::string_view hint = closest_match(name, all); !hint.empty()) {
                    message += "; did you mean " + quoted(hint) + "?";
                }
                json::Value data;
                data["path"] = pointer;
                return Reply::failure(code::kNotFound, std::move(message), std::move(data));
            }
            wanted.push_back(name);
        }
    }

    std::int64_t ran = 0;
    std::int64_t passed = 0;
    {
        LogWatch watch;
        for (const TestFile& file : loaded) {
            for (const auto& [name, test] : file.doc.find("tests")->as_object()) {
                if (!wanted.empty() && std::ranges::find(wanted, name) == wanted.end()) {
                    continue;
                }
                ++ran;
                (void)watch.take(); // what came before isn't this test's
                if (auto failure_found = TestRun(session, file, name, watch).run()) {
                    failed.push_back(std::move(*failure_found));
                } else {
                    ++passed;
                }
            }
        }
    }

    // The whole project's run checks the storyboard too.
    if (paths == nullptr && wanted.empty()) {
        Reply checked = session.call("storyboard.check", json::Object{});
        if (!checked.ok) {
            failed.push_back(file_failure("storyboard", "", checked.message));
        } else if (json::Value* errors = checked.value.find("errors"); errors != nullptr) {
            for (json::Value& error : errors->as_array()) {
                failed.push_back(std::move(error));
            }
        }
    }

    json::Value result;
    result["tests"] = ran;
    result["passed"] = passed;
    result["failed"] = std::move(failed);
    return Reply::success(std::move(result));
}

} // namespace

std::optional<Mismatch> mismatch(const json::Value& expected, const json::Value& actual,
                                 const std::string& path) {
    const auto differs = [&] {
        return Mismatch{path, "expected " + quote(expected) + ", got " + quote(actual)};
    };
    if (expected.is_object()) {
        if (!actual.is_object()) {
            return differs();
        }
        for (const auto& [key, value] : expected.as_object()) {
            const std::string at = path + "/" + json::pointer_escape(key);
            const json::Value* found = actual.find(key);
            if (found == nullptr) {
                return Mismatch{at, "missing; expected " + quote(value)};
            }
            if (auto inner = mismatch(value, *found, at)) {
                return inner;
            }
        }
        return std::nullopt;
    }
    if (expected.is_array()) {
        if (!actual.is_array()) {
            return differs();
        }
        if (expected.size() != actual.size()) {
            return Mismatch{path, "expected " + items(expected.size()) + ", got " +
                                      items(actual.size()) + ": " + quote(actual)};
        }
        for (std::size_t i = 0; i < expected.size(); ++i) {
            if (auto inner = mismatch(expected.as_array()[i], actual.as_array()[i],
                                      path + "/" + std::to_string(i))) {
                return inner;
            }
        }
        return std::nullopt;
    }
    if (expected == actual) {
        return std::nullopt;
    }
    return differs();
}

json::Value test_schema() {
    json::Value step = typed("object", "A command call, and what its reply must hold.");
    step["additionalProperties"] = false;
    step["required"] = json::Array{"command"};
    step["properties"]["command"] = typed("string", "The command, such as \"run.until\".");
    step["properties"]["command"]["minLength"] = 1;
    step["properties"]["params"] = typed("object", "Its params.");
    step["properties"]["expect"]["description"] =
        "What its reply must hold: objects need only the members given, arrays the same number "
        "of items, and other values must be equal, numbers by value.";

    json::Value test = typed("object", "One test: the steps it runs from its scene.");
    test["additionalProperties"] = false;
    test["required"] = json::Array{"steps"};
    test["properties"]["scene"] =
        typed("string", "The scene file it starts from, in place of the file's scene.");
    test["properties"]["scene"]["minLength"] = 1;
    test["properties"]["steps"] = typed("array", "Command calls, run in order.");
    test["properties"]["steps"]["items"] = std::move(step);
    test["properties"]["steps"]["minItems"] = 1;

    json::Value schema;
    schema["$schema"] = "https://json-schema.org/draft/2020-12/schema";
    schema["title"] = "SlashEngine tests";
    schema["description"] =
        "Headless tests, which test.run plays: each opens its scene file in a session of its own "
        "and runs its steps, and fails at the first command that fails, reply that doesn't hold "
        "what a step expects, or warning or error logged.";
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["required"] = json::Array{"tests"};
    schema["properties"]["scene"] = typed(
        "string", "The scene file tests start from, relative to the project; with none, an empty "
                  "world.");
    schema["properties"]["scene"]["minLength"] = 1;
    json::Value& tests = schema["properties"]["tests"];
    tests = typed("object", "Tests by name.");
    tests["minProperties"] = 1;
    tests["propertyNames"]["minLength"] = 1;
    tests["additionalProperties"] = std::move(test);
    return schema;
}

std::vector<json::ValidationError> validate_test(const json::Value& test) {
    static const json::Value schema = test_schema();
    return json::validate(test, schema);
}

void register_test_commands(Registry& registry) {
    registry.add({
        .name = "test.run",
        .description = "Runs the project's .test.json tests headless, each from its scene file, "
                       "and returns only the checks that failed: a step's command failing, its "
                       "reply not holding what it expects, or a warning or error logged. With no "
                       "paths, runs every test file and storyboard.check.",
        .capabilities = {Capability::Run, Capability::Filesystem},
        .effect = Effect::Read,
        .params =
            [] {
                json::Value params;
                params["type"] = "object";
                params["additionalProperties"] = false;
                params["properties"]["paths"] = names_schema(
                    "Test files and folders in the project. Default: every .test.json file.");
                params["properties"]["tests"] =
                    names_schema("Run only the tests with these names.");
                return params;
            }(),
        .handler = test_run,
    });
}

} // namespace slash::command
