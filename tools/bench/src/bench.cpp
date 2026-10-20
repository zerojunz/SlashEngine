#include "slash/bench/bench.hpp"

#include "slash/command/mcp.hpp"
#include "slash/command/test.hpp"
#include "slash/core/text.hpp"
#include "slash/platform/file.hpp"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace slash::bench {
namespace {

namespace fs = std::filesystem;

constexpr std::int64_t kTaskFormat = 1;

// How many characters of each kind a token holds, on average, in a tokenizer like the ones models
// use: measured on the engine's tool definitions, replies and AGENTS.md.
constexpr std::int64_t kLettersPerToken = 10;
constexpr std::int64_t kDigitsPerToken = 3;
constexpr std::int64_t kPunctuationPerToken = 3;

bool letter(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool digit(unsigned char c) {
    return c >= '0' && c <= '9';
}

bool line_space(unsigned char c) {
    return c == '\n' || c == '\r' || c == '\t';
}

// Characters that are none of the above, nor spaces nor beyond ASCII.
bool punctuation(unsigned char c) {
    return c < 0x80U && !letter(c) && !digit(c) && c != ' ' && !line_space(c);
}

std::int64_t round_up(std::size_t count, std::int64_t per) {
    return (static_cast<std::int64_t>(count) + per - 1) / per;
}

json::Value property(std::string_view type, std::string_view description) {
    json::Value out;
    out["type"] = std::string(type);
    out["description"] = std::string(description);
    return out;
}

// What a tool call came back with, for the model and for checks: its text, and the reply as data.
struct Answer {
    std::string text;
    json::Value value;
    bool error = false;
};

Answer answer_of(const json::Value& result) {
    Answer out;
    if (const json::Value* content = result.find("content");
        content != nullptr && content->is_array()) {
        for (const json::Value& block : content->as_array()) {
            if (const json::Value* text = block.find("text");
                text != nullptr && text->is_string()) {
                out.text += text->as_string();
            }
        }
    }
    const json::Value* error = result.find("isError");
    out.error = error != nullptr && error->is_bool() && error->as_bool();
    if (const json::Value* structured = result.find("structuredContent"); structured != nullptr) {
        out.value = *structured;
    } else if (auto parsed = json::parse(out.text)) {
        out.value = std::move(parsed).value();
    }
    return out;
}

json::Value failure(std::string task, std::string path, std::string message) {
    json::Value out;
    out["task"] = std::move(task);
    out["path"] = std::move(path);
    out["message"] = std::move(message);
    return out;
}

// A fresh copy of a task's project at <work>/<task>.
Result<std::string> copy_project(const Task& task, const std::string& work) {
    const fs::path to = fs::path(work) / task.name;
    std::error_code error;
    fs::remove_all(to, error);
    fs::create_directories(to, error);
    if (error) {
        return make_error(ErrorCode::PlatformFailure,
                          "could not make " + to.generic_string() + ": " + error.message());
    }
    const fs::path from = fs::path(task.folder) / "project";
    if (fs::is_directory(from, error)) {
        fs::copy(from, to, fs::copy_options::recursive, error);
        if (error) {
            return make_error(ErrorCode::PlatformFailure,
                              "could not copy " + from.generic_string() + ": " + error.message());
        }
    }
    return to.generic_string();
}

std::int64_t number(const json::Value& object, std::string_view key) {
    const json::Value* found = object.find(key);
    return found != nullptr && found->is_int() ? found->as_int() : 0;
}

json::Value regression(std::string what, const json::Value* baseline, std::int64_t now) {
    json::Value out;
    out["what"] = std::move(what);
    out["baseline"] = baseline != nullptr ? *baseline : json::Value();
    out["now"] = now;
    return out;
}

} // namespace

std::int64_t estimate_tokens(std::string_view text) {
    std::int64_t tokens = 0;
    std::size_t i = 0;
    const auto at = [&text](std::size_t index) { return static_cast<unsigned char>(text[index]); };
    const auto run = [&](auto in_run) {
        const std::size_t start = i;
        while (i < text.size() && in_run(at(i))) {
            ++i;
        }
        return i - start;
    };
    while (i < text.size()) {
        const unsigned char c = at(i);
        if (letter(c)) {
            tokens += round_up(run(letter), kLettersPerToken);
        } else if (digit(c)) {
            tokens += round_up(run(digit), kDigitsPerToken);
        } else if (c == ' ') {
            ++i; // spaces go with the word after them
        } else if (line_space(c)) {
            run([](unsigned char next) { return line_space(next) || next == ' '; });
            ++tokens; // a line break and the indent after it
        } else if (c >= 0x80U) {
            ++i; // one token for each character beyond ASCII
            while (i < text.size() && (at(i) & 0xC0U) == 0x80U) {
                ++i;
            }
            ++tokens;
        } else {
            tokens += round_up(run(punctuation), kPunctuationPerToken);
        }
    }
    return tokens;
}

json::Value task_schema() {
    json::Value schema;
    schema["$schema"] = "https://json-schema.org/draft/2020-12/schema";
    schema["title"] = "SlashEngine benchmark task";
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["required"] = json::Array{"format", "prompt", "solution"};
    json::Value& properties = schema["properties"];
    properties["format"]["const"] = kTaskFormat;
    properties["format"]["description"] = "The task.json format version: 1.";
    properties["prompt"] = property("string", "The task as a person would ask an agent for it.");
    properties["prompt"]["minLength"] = 1;

    json::Value call = property("object", "An MCP tool call.");
    call["additionalProperties"] = false;
    call["required"] = json::Array{"tool"};
    call["properties"]["tool"] = property("string", "The tool's name, e.g. \"scene_open\".");
    call["properties"]["arguments"] = property("object", "Its arguments.");
    properties["solution"] = property(
        "array", "The calls an agent that knows the engine well makes for the task, in order.");
    properties["solution"]["minItems"] = 1;
    properties["solution"]["items"] = std::move(call);

    json::Value check =
        property("object", "A command and what its reply must hold, as a .test.json step; or "
                           "what a solution step replied.");
    check["additionalProperties"] = false;
    check["properties"]["command"] = property("string", "Command name, e.g. \"entity.get\".");
    check["properties"]["params"] = property("object", "The command's params.");
    check["properties"]["step"] =
        property("integer", "A solution step, from 1, whose reply to check instead.");
    check["properties"]["step"]["minimum"] = 1;
    check["properties"]["expect"]["description"] =
        "What the reply must hold: objects need only these members, arrays the same number of "
        "items, other values must be equal.";
    properties["checks"] =
        property("array", "What must hold once the solution has run, so the task still does "
                          "what it says.");
    properties["checks"]["items"] = std::move(check);
    return schema;
}

std::vector<json::ValidationError> validate_task(const json::Value& task) {
    std::vector<json::ValidationError> errors = json::validate(task, task_schema());
    if (!errors.empty()) {
        return errors;
    }
    const json::Value* checks = task.find("checks");
    if (checks == nullptr) {
        return errors;
    }
    const std::size_t steps = task.find("solution")->size();
    for (std::size_t i = 0; i < checks->size(); ++i) {
        const json::Value& check = checks->as_array()[i];
        const std::string at = "/checks/" + std::to_string(i);
        const json::Value* step = check.find("step");
        const bool has_command = check.find("command") != nullptr;
        if ((step != nullptr) == has_command) {
            errors.push_back({at, "needs a command or a step, and not both"});
        } else if (step != nullptr && static_cast<std::size_t>(step->as_int()) > steps) {
            errors.push_back(
                {at + "/step", "the solution has only " + std::to_string(steps) + " steps"});
        } else if (step != nullptr && check.find("expect") == nullptr) {
            errors.push_back({at, "a step check needs expect"});
        } else if (check.find("params") != nullptr && !has_command) {
            errors.push_back({at + "/params", "params go with a command"});
        }
    }
    return errors;
}

Result<std::vector<Task>> load_tasks(const std::string& folder,
                                     std::span<const std::string> names) {
    std::error_code error;
    if (!fs::is_directory(folder, error)) {
        return make_error(ErrorCode::NotFound, "there is no task folder at " + folder);
    }
    std::vector<Task> tasks;
    for (const fs::directory_entry& entry : fs::directory_iterator(folder, error)) {
        const fs::path file = entry.path() / "task.json";
        if (!entry.is_directory(error) || !fs::is_regular_file(file, error)) {
            continue;
        }
        const std::string shown = file.generic_string();
        auto text = platform::read_file(shown);
        if (!text) {
            return text.error();
        }
        auto spec = json::parse(text.value());
        if (!spec) {
            return make_error(ErrorCode::InvalidArgument, shown + ": " + spec.error().message);
        }
        if (const auto problems = validate_task(spec.value()); !problems.empty()) {
            return make_error(ErrorCode::InvalidArgument, shown + " at " + problems.front().path +
                                                              ": " + problems.front().message);
        }
        tasks.push_back({entry.path().filename().generic_string(), entry.path().generic_string(),
                         std::move(spec).value()});
    }
    std::ranges::sort(tasks, {}, &Task::name);
    if (names.empty()) {
        return tasks;
    }
    std::vector<Task> chosen;
    for (const std::string& name : names) {
        const auto found = std::ranges::find(tasks, name, &Task::name);
        if (found == tasks.end()) {
            std::vector<std::string_view> known;
            known.reserve(tasks.size());
            for (const Task& task : tasks) {
                known.emplace_back(task.name);
            }
            std::string message = "there is no task \"";
            message += name;
            message += "\" in ";
            message += folder;
            if (const std::string_view hint = closest_match(name, known); !hint.empty()) {
                message += "; did you mean \"";
                message += hint;
                message += "\"?";
            }
            return make_error(ErrorCode::NotFound, std::move(message));
        }
        chosen.push_back(*found);
    }
    return chosen;
}

Result<json::Value> run(const Options& options) {
    json::Value report;
    report["setup"] = json::Object{};
    report["tasks"] = json::Object{};
    json::Array failed;
    std::int64_t total_calls = 0;
    std::int64_t total_tokens = 0;
    bool measured_setup = false;
    for (const Task& task : options.tasks) {
        auto root = copy_project(task, options.work);
        if (!root) {
            return root.error();
        }
        json::Value measured;
        options.with_session(root.value(), [&](command::Session& session) {
            command::mcp::Server server(session);
            if (!measured_setup) {
                json::Value initialize;
                initialize["protocolVersion"] = std::string(command::mcp::kProtocolVersions[0]);
                const command::Reply hello = server.dispatch("initialize", initialize);
                const command::Reply tools = server.dispatch("tools/list", json::Object{});
                json::Value& setup = report["setup"];
                setup["instructions"] =
                    estimate_tokens(hello.value.find("instructions")->as_string());
                setup["tools"] = static_cast<std::int64_t>(tools.value.find("tools")->size());
                setup["tool_definitions"] =
                    estimate_tokens(json::write(*tools.value.find("tools"), json::Style::Compact));
                measured_setup = true;
            }

            std::int64_t written = 0;
            std::int64_t read = 0;
            std::vector<json::Value> replies;
            const json::Array& solution = task.spec.find("solution")->as_array();
            for (std::size_t i = 0; i < solution.size(); ++i) {
                json::Value call;
                call["name"] = *solution[i].find("tool");
                const json::Value* arguments = solution[i].find("arguments");
                call["arguments"] = arguments != nullptr ? *arguments : json::Object{};
                written += estimate_tokens(json::write(call, json::Style::Compact));
                const command::Reply reply = server.dispatch("tools/call", call);
                const std::string at = "/solution/" + std::to_string(i);
                if (!reply.ok) {
                    failed.push_back(failure(task.name, at, reply.message));
                    replies.emplace_back();
                    continue;
                }
                Answer answer = answer_of(reply.value);
                read += estimate_tokens(answer.text);
                if (answer.error) {
                    failed.push_back(failure(task.name, at, answer.text));
                }
                replies.push_back(std::move(answer.value));
            }

            if (const json::Value* checks = task.spec.find("checks"); checks != nullptr) {
                for (std::size_t i = 0; i < checks->size(); ++i) {
                    const json::Value& check = checks->as_array()[i];
                    const std::string at = "/checks/" + std::to_string(i);
                    json::Value actual;
                    if (const json::Value* step = check.find("step"); step != nullptr) {
                        actual = replies[static_cast<std::size_t>(step->as_int()) - 1];
                    } else {
                        const json::Value* params = check.find("params");
                        const command::Reply reply =
                            session.call(check.find("command")->as_string(),
                                         params != nullptr ? *params : json::Object{});
                        if (!reply.ok) {
                            failed.push_back(failure(task.name, at, reply.message));
                            continue;
                        }
                        actual = reply.value;
                    }
                    if (const json::Value* expect = check.find("expect"); expect != nullptr) {
                        if (auto differs = command::mismatch(*expect, actual, at + "/expect")) {
                            failed.push_back(failure(task.name, differs->path, differs->message));
                        }
                    }
                }
            }
            measured["calls"] = static_cast<std::int64_t>(solution.size());
            measured["written"] = written;
            measured["read"] = read;
            measured["tokens"] = written + read;
        });
        total_calls += number(measured, "calls");
        total_tokens += number(measured, "tokens");
        report["tasks"][task.name] = std::move(measured);
    }
    report["total"]["calls"] = total_calls;
    report["total"]["tokens"] = total_tokens;
    report["failed"] = std::move(failed);
    return report;
}

json::Array regressions(const json::Value& report, const json::Value& baseline, double tolerance) {
    json::Array out;
    const auto grew = [tolerance](std::int64_t now, const json::Value* before) {
        return before != nullptr && before->is_int() &&
               static_cast<double>(now) > static_cast<double>(before->as_int()) * (1.0 + tolerance);
    };
    const json::Value* setup_before = baseline.find("setup");
    if (const json::Value* setup = report.find("setup"); setup != nullptr) {
        for (const char* key : {"instructions", "tool_definitions"}) {
            const json::Value* before = setup_before != nullptr ? setup_before->find(key) : nullptr;
            if (grew(number(*setup, key), before)) {
                out.push_back(regression("setup/" + std::string(key), before, number(*setup, key)));
            }
        }
    }
    const json::Value* tasks_before = baseline.find("tasks");
    if (const json::Value* tasks = report.find("tasks"); tasks != nullptr) {
        for (const auto& [name, now] : tasks->as_object()) {
            const json::Value* before =
                tasks_before != nullptr ? tasks_before->find(name) : nullptr;
            if (before == nullptr) {
                out.push_back(regression(name, nullptr, number(now, "tokens")));
                continue;
            }
            const json::Value* calls = before->find("calls");
            if (calls != nullptr && calls->is_int() && number(now, "calls") > calls->as_int()) {
                out.push_back(regression(name + "/calls", calls, number(now, "calls")));
            }
            if (grew(number(now, "tokens"), before->find("tokens"))) {
                out.push_back(
                    regression(name + "/tokens", before->find("tokens"), number(now, "tokens")));
            }
        }
    }
    return out;
}

json::Value baseline_of(const json::Value& report) {
    json::Value out = report;
    out.as_object().erase("failed");
    return out;
}

} // namespace slash::bench
