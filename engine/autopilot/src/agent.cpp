// SlashEngine's own playtest agent: an agent service in the game's process that plays a run with
// any model providers.json names. Its instructions, request kinds, rules of play and report
// follow Autopilot's agent session (github.com/ramenvr/autopilot, autopilot/agents/session.py,
// Apache 2.0), cut to a model that plays through tools itself, one turn at a time, where
// Autopilot's has a coding agent write a harness for its hosted decision model.

#include "slash/autopilot/agent.hpp"

#include "internal.hpp"

#include "slash/ai/provider.hpp"
#include "slash/core/json.hpp"
#include "slash/core/log.hpp"
#include "slash/core/result.hpp"
#include "slash/core/text.hpp"
#include "slash/platform/file.hpp"
#include "slash/world/world.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slash::autopilot {

namespace {

// Where notes from earlier runs are kept, in the project.
constexpr std::string_view kNotesFile = ".slash/autopilot/notes.md";
// The most characters of one tool's answer, or of the notes and the project in brief, a model is
// sent.
constexpr std::size_t kMaxToolText = 12000;
constexpr std::size_t kMaxBrief = 12000;
// Seconds of play when neither the options nor the run say.
constexpr double kDefaultPlaySeconds = 600.0;
// The most seconds one tool may play.
constexpr double kMaxStep = 120.0;
// The longest side of a picture sent to the model.
constexpr int kPictureSize = 768;
constexpr int kPictureQuality = 80;

constexpr std::string_view kInstructions =
    R"(You are a playtester. You play a video game made with SlashEngine through tools, to settle one request: verify a claim about it, reach a goal in it, or explore it for bugs. The game's developer reads your report.

How the game runs: it is paused while you think, and runs only inside the tools that take time (key and action held for seconds or tapped, move, wait, watch, ui_act, restart). The other tools read the game as it stands. Every turn's results end with the turns and the seconds of play you have left.

Positions and directions are in the kit frame. In a 2D game X is right and Z is up the screen, in pixels (Y is always 0). In a 3D game X is forward, Y is right and Z is up, in centimeters. Entities are named by their IDs.

How to work:
1. The request comes with what you would look at first: the game's context (the player, its camera, its controls), what is on screen, the project in brief and notes from earlier runs. Don't ask for those again.
2. Read the code where the request touches it (files, read_files): how the mechanic works, which inputs drive it and what counts as done. Be targeted, and read several files in one call. What you see in the game outranks what you read.
3. Play: press keys and actions, move, click controls, then see what changed (look, find, transforms, watch, screenshot). Put several tool calls in one turn when you know what to do: they run in order. Small, checked steps beat long blind ones.
4. When the request is settled, or more play won't help, call report at once. Before that, save what a later run on this game needs with note: controls, mechanics, numbers, pitfalls, in short lines and never guesses.

The request is one of:
- a goal: make it happen in play. Report "achieved" or "not_achieved", with the evidence from play.
- a claim to verify: play so the claim is tested in the game, and measure what happens. The verdict rests on what was observed in play, never on code alone: CONFIRMED (shown true), DENIED (shown false: give the observation that contradicts it) or INCONCLUSIVE. Code tells you where to look and what to measure; a bug in the code is only a lead until play shows it. A claim that something can happen is shown true by doing it once. Failing to do it is not a contradiction, as it is as likely your play: try other ways, and say DENIED only when the game's own numbers prove it impossible (say which numbers), else INCONCLUSIVE.
- an exploration: try the mechanics broadly (what hasn't been tried, edge cases, limits) and report "explored" with each finding and its observation. A finding is something the game was seen to do wrong: a value past its limit, a rule broken, a state that contradicts another. That you failed to do something is not a finding.
- auto: decide which of the three the prompt asks for, and report as that kind does.

Rules:
- In play, the player acts only through input: keys, actions, moves and controls. Getting somewhere, collecting and surviving happen through play. rc's writes and calls change the game directly: use them only to set up a test by removing what merely gets in the way, never for what the request is about, and give the reason. The report lists each one.
- The game's detectors watch for errors in its log, NaNs, falls out of the world, hitches and on-screen counts past their maximum, and what they find is added to your results. Report one as a finding when play shows it is wrong.
- When a request is about how something looks, read what the game tells first (look, ui), but only the picture settles it: before you take a screenshot, write down what true would look like and what false would.
- In the report call the player "the agent", and keep it short: what happened, and the observations it rests on with their game times.)";

[[nodiscard]] std::string string_of(const json::Value& object, std::string_view key) {
    const json::Value* value = object.find(key);
    return value != nullptr && value->is_string() ? value->as_string() : std::string();
}

[[nodiscard]] double number_of(const json::Value& object, std::string_view key, double fallback) {
    const json::Value* value = object.find(key);
    return value != nullptr && value->is_number() ? value->as_double() : fallback;
}

[[nodiscard]] std::string cut(std::string text, std::size_t most) {
    if (text.size() > most) {
        text.resize(most);
        text += "\n... (cut at " + std::to_string(most) + " characters)";
    }
    return text;
}

[[nodiscard]] double round1(double value) {
    return std::round(value * 10.0) / 10.0;
}

[[nodiscard]] json::Value schema(json::Object properties, json::Array required = {}) {
    json::Value out;
    out["type"] = "object";
    out["properties"] = std::move(properties);
    if (!required.empty()) {
        out["required"] = std::move(required);
    }
    return out;
}

[[nodiscard]] json::Value field(std::string_view type, std::string_view doc) {
    json::Value out;
    out["type"] = type;
    out["description"] = doc;
    return out;
}

[[nodiscard]] json::Value strings(std::string_view doc) {
    json::Value out = field("array", doc);
    out["items"]["type"] = "string";
    return out;
}

[[nodiscard]] json::Value choice(std::string_view doc, json::Array values) {
    json::Value out = field("string", doc);
    out["enum"] = std::move(values);
    return out;
}

[[nodiscard]] json::Value direction_field() {
    json::Value out = field("object", "The way to go, in the kit frame: {\"X\": 1} is right in "
                                      "2D and forward in 3D, {\"Z\": 1} up the screen in 2D.");
    out["properties"]["X"]["type"] = "number";
    out["properties"]["Y"]["type"] = "number";
    out["properties"]["Z"]["type"] = "number";
    return out;
}

// The tools the model plays with: the bridge's requests, with what only the agent does.
[[nodiscard]] std::vector<ai::ToolSpec> tool_specs() {
    std::vector<ai::ToolSpec> tools;
    const auto add = [&tools](std::string_view name, std::string_view doc, json::Value params) {
        tools.push_back({.name = std::string(name),
                         .description = std::string(doc),
                         .parameters = std::move(params)});
    };
    const json::Value seconds = field("number", "Seconds of play (default 1).");
    add("look",
        "Where the player is and how it moves, the text and controls on screen, and in 3D what the "
        "camera sees.",
        schema({}));
    add("context", "The scene, the player, its camera, 2d or 3d, and the controls the game binds.",
        schema({}));
    add("find", "Entities by ID (name) or by a component they have (class), with where they are.",
        schema({{"name", field("string", "Part of an entity ID.")},
                {"class", field("string", "A component name, such as CharacterBody2D.")}}));
    add("transforms", "Where the named entities are, how they are turned and their size.",
        schema({{"objects", strings("Entity IDs.")}}, {"objects"}));
    add("key",
        "Presses a key by Unreal's name (A-Z, SpaceBar, Enter, Escape, Up, LeftMouseButton, "
        "Gamepad_FaceButton_Bottom...): a tap, a hold for seconds, or a release.",
        schema({{"key", field("string", "The key.")},
                {"mode", choice("press (a tap, default), hold or release.",
                                json::Array{"press", "hold", "release"})},
                {"seconds", seconds},
                {"value", field("number", "An axis's value, -1 to 1.")}},
               {"key"}));
    add("action",
        "Presses an input action the game binds, by name: a tap, a hold for seconds, or a "
        "release.",
        schema({{"action", field("string", "The action.")},
                {"mode", choice("press (a tap, default), hold or release.",
                                json::Array{"press", "hold", "release"})},
                {"seconds", seconds},
                {"value", field("number", "Its strength, 0 to 1 (default 1).")}},
               {"action"}));
    add("move",
        "Walks the player for seconds along a direction with the actions W, A, S and D (or the "
        "arrows) are bound to; answers how far it went.",
        schema({{"direction", direction_field()},
                {"seconds", seconds},
                {"scale", field("number", "How far the stick is pushed, 0 to 1 (default 1).")}},
               {"direction"}));
    add("wait", "Lets the game play on for seconds, and says what changed on the named entities.",
        schema({{"seconds", seconds}, {"objects", strings("Entity IDs to watch.")}}));
    add("watch",
        "Plays for seconds and says what changed on the named entities and when: moves, turns, "
        "sizes, parts shown or hidden, animations.",
        schema({{"objects", strings("Entity IDs.")}, {"seconds", seconds}}, {"objects"}));
    add("ui", "The controls on screen with their state and where their middle is.", schema({}));
    add("ui_act",
        "Works a control by entity ID: click, right_click, double_click, set (a slider's value, "
        "a text field's text, a toggle) or drag to another control.",
        schema(
            {{"id", field("string", "The control's entity ID.")},
             {"action", choice("click (default), right_click, double_click, set or drag.",
                               json::Array{"click", "right_click", "double_click", "set", "drag"})},
             {"value", field("number", "A slider's value, or a toggle's (1 or 0).")},
             {"text", field("string", "A text field's text.")},
             {"option", field("integer", "An option button's option.")},
             {"to", field("string", "The control to drag to.")}},
            {"id"}));
    add("screenshot", "A picture of the game as it is now.", schema({}));
    add("files", "The project's text files: scenes, prefabs, gameplay sources, settings, notes.",
        schema({}));
    add("read_files", "The text of project files, by their paths from files.",
        schema({{"paths", strings("Project paths.")}}, {"paths"}));
    add("rc",
        "Reads or changes the game directly. describe: an entity's components and fields; "
        "property: a field read, or written with propertyValue and access WRITE_ACCESS; call: a "
        "command (functionName) with parameters. Writes and calls need why.",
        schema({{"verb", choice("describe, property or call.",
                                json::Array{"describe", "property", "call"})},
                {"body", field("object", "{\"objectPath\": entity ID, \"propertyName\": "
                                         "\"Component/field\", ...} as Unreal's Remote Control "
                                         "takes it.")},
                {"why", field("string", "Why a write or a call is needed.")}},
               {"verb", "body"}));
    add("restart", "Starts the scene over, or another scene by path or name.",
        schema({{"scene", field("string", "The scene to play instead.")}}));
    add("note",
        "Saves a verified fact for later runs on this game (controls, mechanics, numbers, "
        "pitfalls).",
        schema({{"text", field("string", "One short line.")}}, {"text"}));
    add("report",
        "The final answer, which ends the run. The developer reads it: call the player \"the "
        "agent\".",
        schema({{"outcome",
                 choice("CONFIRMED, DENIED or INCONCLUSIVE for a claim; achieved or not_achieved "
                        "for a goal; explored for an exploration.",
                        json::Array{"CONFIRMED", "DENIED", "INCONCLUSIVE", "achieved",
                                    "not_achieved", "explored"})},
                {"summary", field("string", "What happened, in a few sentences.")},
                {"evidence", strings("The observations from play it rests on, with game times.")},
                {"findings", strings("Each suspected bug with its observation.")}},
               {"outcome", "summary", "evidence"}));
    return tools;
}

// Tools that run the game.
[[nodiscard]] bool plays(std::string_view tool, const json::Value& args) {
    if (tool == "key" || tool == "action") {
        return string_of(args, "mode") != "release";
    }
    return tool == "move" || tool == "wait" || tool == "watch" || tool == "ui_act" ||
           tool == "restart";
}

// The run's member holding what to do, as the bridge's hello asks for each kind.
[[nodiscard]] std::string_view prompt_field(std::string_view kind) {
    if (kind == "verify") {
        return "claim";
    }
    if (kind == "goal") {
        return "goal";
    }
    if (kind == "explore") {
        return "brief";
    }
    return "prompt";
}

[[nodiscard]] std::string kind_title(std::string_view kind) {
    if (kind == "verify") {
        return "Verify";
    }
    if (kind == "goal") {
        return "Goal";
    }
    if (kind == "explore") {
        return "Explore";
    }
    return "Request";
}

class Agent final : public Transport {
public:
    explicit Agent(AgentOptions options) : options_(std::move(options)), tools_(tool_specs()) {}

    void poll() override {
        while (!inbox_.empty()) {
            const json::Value frame = std::move(inbox_.front());
            inbox_.pop_front();
            handle(frame);
        }
        if (phase_ == Phase::Thinking && call_ != nullptr && call_->done()) {
            const Result<ai::Response> reply = *call_->result();
            call_.reset();
            answered(reply);
        }
    }

    [[nodiscard]] State state() const override {
        return phase_ == Phase::Closed ? State::Closed : State::Open;
    }

    std::optional<std::string> receive() override {
        if (outbox_.empty()) {
            return std::nullopt;
        }
        std::string text = std::move(outbox_.front());
        outbox_.pop_front();
        return text;
    }

    bool send(std::string_view text) override {
        Result<json::Value> frame = json::parse(text);
        if (frame.ok() && frame.value().is_object()) {
            inbox_.push_back(std::move(frame.value()));
        }
        return phase_ != Phase::Closed;
    }

    [[nodiscard]] std::string address() const override { return "the built-in agent"; }

private:
    enum class Phase : std::uint8_t { Hello, Briefing, Thinking, Acting, Done, Closed };

    // A request to the bridge, or with no op a step of the agent's own, run in order.
    struct Step {
        std::string op;
        json::Value args;
        std::function<void(bool ok, const json::Value& value)> done;
    };

    // One tool call's answer.
    struct Answer {
        std::string id;
        json::Value value = json::Object{}; // NOLINT(readability-redundant-member-init)
        std::string error;
        std::optional<ai::Image> image;
    };

    void write(const json::Value& frame) {
        outbox_.push_back(json::write(frame, json::Style::Compact));
    }

    void handle(const json::Value& frame) {
        const std::string type = string_of(frame, "type");
        if (type == "hello" && phase_ == Phase::Hello) {
            hello(frame);
        } else if (type == "response") {
            const json::Value* id = frame.find("id");
            if (waiting_ && id != nullptr && id->is_number() && id->as_int() == waiting_id_) {
                waiting_ = false;
                const json::Value* ok = frame.find("ok");
                const bool success = ok != nullptr && ok->is_bool() && ok->as_bool();
                const json::Value* value = frame.find(success ? "result" : "error");
                const Step step = std::move(current_);
                step.done(success, value != nullptr ? *value : json::Value());
                pump();
            }
        } else if (type == "events") {
            if (const json::Value* events = frame.find("events");
                events != nullptr && events->is_array()) {
                for (const json::Value& event : events->as_array()) {
                    if (string_of(event, "kind") == "finding") {
                        if (const json::Value* found = event.find("finding"); found != nullptr) {
                            new_findings_.push_back(*found);
                            findings_.push_back(*found);
                        }
                    }
                }
            }
        }
    }

    // ---- Steps ----

    void ask(std::string op, json::Value args,
             std::function<void(bool ok, const json::Value& value)> done) {
        steps_.push_back({.op = std::move(op), .args = std::move(args), .done = std::move(done)});
    }

    void then(std::function<void()> step) {
        steps_.push_back({.op = {},
                          .args = {},
                          .done = [step = std::move(step)](bool, const json::Value&) { step(); }});
    }

    void pump() {
        while (!waiting_ && !steps_.empty() && phase_ != Phase::Done && phase_ != Phase::Closed) {
            Step step = std::move(steps_.front());
            steps_.pop_front();
            if (step.op.empty()) {
                step.done(true, json::Value());
                continue;
            }
            json::Value frame;
            frame["type"] = "request";
            frame["id"] = ++next_id_;
            frame["op"] = step.op;
            frame["args"] = step.args.is_object() ? step.args : json::Value(json::Object{});
            write(frame);
            waiting_ = true;
            waiting_id_ = next_id_;
            current_ = std::move(step);
        }
    }

    void pause(bool paused) {
        ask("pause", json::Object{{"paused", paused}}, [](bool, const json::Value&) {});
    }

    void clock() {
        ask("ping", json::Object{}, [this](bool ok, const json::Value& value) {
            if (ok) {
                game_time_ = number_of(value, "game_time", game_time_);
            }
        });
    }

    // ---- The run ----

    void hello(const json::Value& frame) {
        project_ = string_of(frame, "project");
        json::Value welcome;
        welcome["type"] = "welcome";
        welcome["protocol"] = kProtocol;
        welcome["role"] = "game";
        write(welcome);
        const json::Value* run = frame.find("run");
        if (run == nullptr || !run->is_object()) {
            phase_ = Phase::Briefing;
            fail("the built-in agent needs a run to play: give it one with --autopilot-run");
            return;
        }
        run_ = *run;
        kind_ = string_of(run_, "kind");
        prompt_ = string_of(run_, prompt_field(kind_));
        play_seconds_ = options_.play_seconds;
        if (!(play_seconds_ > 0.0)) {
            const double minutes =
                number_of(run_, kind_ == "explore" ? "minutes" : "time_limit_minutes", 0.0);
            play_seconds_ = minutes > 0.0 ? minutes * 60.0 : kDefaultPlaySeconds;
        }
        phase_ = Phase::Briefing;
        log::info("autopilot agent: playing", {{"kind", kind_}, {"model", options_.model}});

        // What the model sees first, so its first turns go to the request.
        pause(true);
        ask("context", json::Object{}, [this](bool ok, const json::Value& value) {
            if (ok) {
                context_ = value;
            }
        });
        ask("screen", json::Object{{"ui", true}}, [this](bool ok, const json::Value& value) {
            if (ok) {
                screen_ = value;
            }
        });
        json::Value read;
        read["paths"] = json::Array{std::string(detail::kCodexPrefix) + "summary.txt"};
        ask("codex_read", std::move(read), [this](bool ok, const json::Value& value) {
            if (const json::Value* files = value.find("files"); ok && files != nullptr) {
                for (const auto& [path, text] : files->as_object()) {
                    if (text.is_string()) {
                        summary_ = text.as_string();
                    }
                }
            }
        });
        clock();
        then([this] {
            started_at_ = game_time_;
            begin();
        });
        pump();
    }

    void begin() {
        std::string brief = "Request (" + (kind_.empty() ? std::string("auto") : kind_) +
                            "): " + (prompt_.empty() ? std::string("explore the game") : prompt_) +
                            "\n";
        brief += "Game: " + project_ + ". You have " + std::to_string(options_.max_turns) +
                 " turns and " + std::to_string(static_cast<int>(play_seconds_)) +
                 " seconds of play.\n\n";
        brief += "Context: " + json::write(context_, json::Style::Compact) + "\n\n";
        brief += "On screen: " + json::write(screen_, json::Style::Compact) + "\n\n";
        if (!summary_.empty()) {
            brief += "The project in brief:\n" + cut(summary_, kMaxBrief) + "\n\n";
        }
        Result<std::string> notes =
            platform::read_file(options_.project_root + "/" + std::string(kNotesFile));
        if (notes.ok() && !notes.value().empty()) {
            brief +=
                "Notes from earlier runs on this game:\n" + cut(notes.value(), kMaxBrief) + "\n";
        } else {
            brief += "No notes from earlier runs on this game.\n";
        }
        messages_.push_back({.role = ai::Role::System, .text = std::string(kInstructions)});
        messages_.push_back({.role = ai::Role::User, .text = std::move(brief)});
        think();
    }

    [[nodiscard]] double play_left() const {
        return std::max(0.0, play_seconds_ - (game_time_ - started_at_));
    }

    void think() {
        if (turns_ >= options_.max_turns) {
            end_unreported("the agent ran out of turns before it reported");
            return;
        }
        ai::Request request;
        request.messages = messages_;
        const bool last = turns_ + 1 >= options_.max_turns || report_only_;
        for (const ai::ToolSpec& tool : tools_) {
            if (!last || tool.name == "report") {
                request.tools.push_back(tool);
            }
        }
        request.params.max_tokens = options_.reply_tokens;
        phase_ = Phase::Thinking;
        call_ = std::make_unique<ai::Call>(options_.provider, std::move(request));
        if (options_.wait) {
            call_->wait();
        }
    }

    void answered(const Result<ai::Response>& reply) {
        if (!reply.ok()) {
            fail("the model's reply failed: " + reply.error().message);
            return;
        }
        const ai::Response& response = reply.value();
        ++turns_;
        input_tokens_ += response.usage.input_tokens;
        output_tokens_ += response.usage.output_tokens;
        if (!response.model.empty()) {
            served_model_ = response.model;
        }
        messages_.push_back(response.message);
        if (options_.max_tokens > 0 && input_tokens_ + output_tokens_ >= options_.max_tokens) {
            report_only_ = true;
        }
        const std::vector<ai::ToolCall>& calls = response.message.tool_calls;
        if (calls.empty()) {
            if (report_only_ || turns_ >= options_.max_turns) {
                end_unreported("the agent stopped without a report: " +
                               cut(response.message.text, 400));
                return;
            }
            messages_.push_back(
                {.role = ai::Role::User,
                 .text = "Go on playing with the tools, or call report to finish."});
            think();
            return;
        }
        act(calls);
    }

    // ---- Tools ----

    void act(const std::vector<ai::ToolCall>& calls) {
        phase_ = Phase::Acting;
        answers_.clear();
        answers_.resize(calls.size());
        bool played = false;
        for (const ai::ToolCall& call : calls) {
            played = played || plays(call.name, call.arguments);
        }
        planned_ = play_left();
        if (played) {
            pause(false);
        }
        for (std::size_t i = 0; i < calls.size(); ++i) {
            answers_[i].id = calls[i].id;
            use(calls[i], i);
        }
        if (played) {
            pause(true);
            clock();
        }
        then([this] { acted(); });
        pump();
    }

    // Stores an op's answer as the tool's, or under key in it.
    std::function<void(bool, const json::Value&)> into(std::size_t i, std::string key = {}) {
        return [this, i, key = std::move(key)](bool ok, const json::Value& value) {
            Answer& answer = answers_[i];
            if (!ok) {
                const std::string error = value.is_string() ? value.as_string() : "failed";
                if (key.empty()) {
                    answer.error = error;
                } else {
                    answer.value[key] = json::Object{{"error", error}};
                }
                return;
            }
            if (key.empty()) {
                answer.value = value;
            } else {
                answer.value[key] = value;
            }
        };
    }

    // Seconds a tool may play: what it asks, within what is left.
    [[nodiscard]] double take(const json::Value& args, std::size_t i) {
        const double asked = std::clamp(number_of(args, "seconds", 1.0), 0.05, kMaxStep);
        if (planned_ <= 0.0) {
            answers_[i].error = "play time is up: call report";
            return 0.0;
        }
        const double seconds = std::min(asked, planned_);
        planned_ -= seconds;
        return seconds;
    }

    void use(const ai::ToolCall& call, std::size_t i) {
        const std::string& name = call.name;
        const json::Value& args = call.arguments;
        const bool is3d = string_of(context_, "dimension") == "3d";
        if (name == "look") {
            ask("pose", json::Object{}, into(i, "pose"));
            ask("screen", json::Object{{"ui", true}}, into(i, "screen"));
            if (is3d) {
                ask("view", json::Object{}, into(i, "view"));
            }
        } else if (name == "context" || name == "find" || name == "transforms" || name == "ui") {
            ask(name, args, into(i));
            if (name == "context") {
                then([this, i] {
                    if (answers_[i].error.empty()) {
                        context_ = answers_[i].value;
                    }
                });
            }
        } else if (name == "key" || name == "action") {
            key(name, args, i);
        } else if (name == "move") {
            const double seconds = take(args, i);
            if (seconds <= 0.0) {
                return;
            }
            json::Value move;
            move["direction"] = args.find("direction") != nullptr ? *args.find("direction")
                                                                  : json::Value(json::Object{});
            move["seconds"] = seconds;
            move["scale"] = number_of(args, "scale", 1.0);
            ask("move", std::move(move), into(i));
        } else if (name == "wait" || name == "watch") {
            const double seconds = take(args, i);
            if (seconds <= 0.0) {
                return;
            }
            json::Value watch;
            const json::Value* objects = args.find("objects");
            watch["objects"] =
                objects != nullptr && objects->is_array() ? *objects : json::Value(json::Array{});
            watch["seconds"] = seconds;
            ask("watch", std::move(watch), into(i));
        } else if (name == "ui_act") {
            if (take(json::Object{{"seconds", 0.1}}, i) <= 0.0) {
                return;
            }
            ask("ui_act", args, into(i));
        } else if (name == "screenshot") {
            screenshot(i);
        } else if (name == "files") {
            ask("codex_manifest", json::Object{}, [this, i](bool ok, const json::Value& value) {
                if (!ok) {
                    answers_[i].error = value.is_string() ? value.as_string() : "failed";
                    return;
                }
                json::Array files;
                if (const json::Value* listed = value.find("files");
                    listed != nullptr && listed->is_array()) {
                    for (const json::Value& file : listed->as_array()) {
                        std::string path = string_of(file, "path");
                        if (path.starts_with(detail::kCodexPrefix)) {
                            path.erase(0, detail::kCodexPrefix.size());
                        }
                        files.emplace_back(path + " (" +
                                           std::to_string(static_cast<std::int64_t>(
                                               number_of(file, "size", 0.0))) +
                                           " bytes)");
                    }
                }
                answers_[i].value = json::Object{{"files", std::move(files)}};
            });
        } else if (name == "read_files") {
            json::Value read;
            read["paths"] = json::Array{};
            if (const json::Value* paths = args.find("paths");
                paths != nullptr && paths->is_array()) {
                for (const json::Value& path : paths->as_array()) {
                    if (path.is_string()) {
                        std::string served = path.as_string();
                        if (!served.starts_with(detail::kCodexPrefix)) {
                            served.insert(0, detail::kCodexPrefix);
                        }
                        read["paths"].push_back(std::move(served));
                    }
                }
            }
            ask("codex_read", std::move(read), [this, i](bool ok, const json::Value& value) {
                if (!ok) {
                    answers_[i].error = value.is_string() ? value.as_string() : "failed";
                    return;
                }
                json::Value out = value;
                json::Value files = json::Object{};
                if (const json::Value* read_files = value.find("files");
                    read_files != nullptr && read_files->is_object()) {
                    for (const auto& [path, text] : read_files->as_object()) {
                        std::string plain = path;
                        if (plain.starts_with(detail::kCodexPrefix)) {
                            plain.erase(0, detail::kCodexPrefix.size());
                        }
                        files[plain] = text;
                    }
                }
                out["files"] = std::move(files);
                answers_[i].value = std::move(out);
            });
        } else if (name == "rc") {
            rc(args, i);
        } else if (name == "restart") {
            json::Value restart;
            restart["action"] = "restart";
            if (const std::string scene = string_of(args, "scene"); !scene.empty()) {
                restart["map"] = scene;
            }
            ask("session", std::move(restart), into(i));
        } else if (name == "note") {
            note(string_of(args, "text"), i);
        } else if (name == "report") {
            report_ = args;
            answers_[i].value = json::Object{{"reported", true}};
        } else {
            answers_[i].error = "no tool named " + name;
        }
    }

    void key(const std::string& name, const json::Value& args, std::size_t i) {
        const bool is_key = name == "key";
        const std::string op = is_key ? "key" : "input";
        const std::string mode =
            string_of(args, "mode").empty() ? "press" : string_of(args, "mode");
        json::Value base;
        base[is_key ? "key" : "action"] = string_of(args, is_key ? "key" : "action");
        if (const json::Value* value = args.find("value"); value != nullptr && value->is_number()) {
            base["value"] = *value;
        }
        if (mode == "release" || mode == "press") {
            if (mode == "press" && take(json::Object{{"seconds", 0.1}}, i) <= 0.0) {
                return;
            }
            json::Value request = base;
            request["mode"] = mode;
            ask(op, std::move(request), into(i));
            return;
        }
        if (mode != "hold") {
            answers_[i].error = "mode is press, hold or release";
            return;
        }
        const double seconds = take(args, i);
        if (seconds <= 0.0) {
            return;
        }
        json::Value hold = base;
        hold["mode"] = "hold";
        if (is_key) {
            hold["seconds"] = seconds;
        }
        ask(op, std::move(hold), [this, i, seconds](bool ok, const json::Value& value) {
            if (!ok) {
                answers_[i].error = value.is_string() ? value.as_string() : "failed";
                return;
            }
            answers_[i].value = json::Object{{"held_seconds", round1(seconds)}};
        });
        ask("watch", json::Object{{"objects", json::Array{}}, {"seconds", seconds}},
            [](bool, const json::Value&) {});
        if (!is_key) {
            json::Value release = base;
            release["mode"] = "release";
            ask(op, std::move(release), [](bool, const json::Value&) {});
        }
    }

    void screenshot(std::size_t i) {
        if (!options_.pictures || !options_.provider->capabilities().vision) {
            answers_[i].error = "pictures are off for this run: use look, ui and find";
            return;
        }
        json::Value capture;
        capture["max_dim"] = kPictureSize;
        capture["quality"] = kPictureQuality;
        ask("capture", std::move(capture), [this, i](bool ok, const json::Value& value) {
            if (!ok) {
                answers_[i].error = value.is_string() ? value.as_string() : "failed";
                return;
            }
            std::optional<std::string> bytes = base64_decode(string_of(value, "jpeg"));
            if (!bytes) {
                answers_[i].error = "the picture could not be read";
                return;
            }
            answers_[i].image = ai::Image{.media_type = "image/jpeg", .data = std::move(*bytes)};
            json::Value out;
            out["width"] = number_of(value, "width", 0.0);
            out["height"] = number_of(value, "height", 0.0);
            out["picture"] = "attached after the results";
            answers_[i].value = std::move(out);
        });
    }

    void rc(const json::Value& args, std::size_t i) {
        const std::string verb = string_of(args, "verb");
        const json::Value* body = args.find("body");
        if (body == nullptr || !body->is_object()) {
            answers_[i].error = "rc needs a body";
            return;
        }
        const std::string access = string_of(*body, "access");
        const bool changes =
            verb == "call" || (verb == "property" && (access.starts_with("WRITE") ||
                                                      body->find("propertyValue") != nullptr));
        if (changes) {
            const std::string why = string_of(args, "why");
            if (why.empty()) {
                answers_[i].error = "a write or a call needs why";
                return;
            }
            json::Value change;
            change["verb"] = verb;
            change["body"] = *body;
            change["why"] = why;
            changes_.push_back(std::move(change));
        }
        json::Value request;
        request["verb"] = verb;
        request["body"] = *body;
        ask("rc", std::move(request), into(i));
    }

    void note(const std::string& text, std::size_t i) {
        if (text.empty()) {
            answers_[i].error = "note needs text";
            return;
        }
        const std::string path = options_.project_root + "/" + std::string(kNotesFile);
        std::string notes;
        if (Result<std::string> read = platform::read_file(path); read.ok()) {
            notes = std::move(read.value());
        }
        if (notes.empty()) {
            notes = "# Notes from playtests\n\n";
        }
        std::string line = text;
        std::replace(line.begin(), line.end(), '\n', ' ');
        notes += "- " + line + "\n";
        const std::string folder = path.substr(0, path.find_last_of('/'));
        if (const Result<void> made = platform::create_directories(folder); !made.ok()) {
            answers_[i].error = made.error().message;
            return;
        }
        if (const Result<void> written = platform::write_file(path, notes); !written.ok()) {
            answers_[i].error = written.error().message;
            return;
        }
        answers_[i].value = json::Object{{"saved", std::string(kNotesFile)}};
    }

    void acted() {
        if (report_) {
            finish(*report_);
            return;
        }
        std::vector<ai::Image> pictures;
        for (std::size_t i = 0; i < answers_.size(); ++i) {
            Answer& answer = answers_[i];
            ai::Message result{.role = ai::Role::Tool, .tool_call_id = answer.id};
            if (!answer.error.empty()) {
                result.text = answer.error;
                result.is_error = true;
            } else {
                result.text = cut(json::write(answer.value, json::Style::Compact), kMaxToolText);
            }
            if (answer.image) {
                pictures.push_back(std::move(*answer.image));
            }
            if (i + 1 == answers_.size()) {
                result.text += status();
            }
            messages_.push_back(std::move(result));
        }
        if (!pictures.empty()) {
            messages_.push_back({.role = ai::Role::User,
                                 .text = "The pictures you asked for, in order.",
                                 .images = std::move(pictures)});
        }
        think();
    }

    // What the detectors found since the last turn, and what is left.
    [[nodiscard]] std::string status() {
        std::string out;
        if (!new_findings_.empty()) {
            out += "\n\nThe detectors found:";
            for (const json::Value& finding : new_findings_) {
                out += "\n- " + string_of(finding, "summary") + " (" +
                       string_of(finding, "detector") + ")";
            }
            new_findings_.clear();
        }
        const int turns_left = std::max(0, options_.max_turns - turns_);
        const double left = play_left();
        out += "\n\n[" + std::to_string(turns_left) + " turns and " +
               std::to_string(static_cast<int>(std::floor(left))) +
               " seconds of play left; game time " +
               json::write(json::Value(round1(game_time_ - started_at_)), json::Style::Compact) +
               " s]";
        if (left <= 0.0 || turns_left <= 1 || report_only_) {
            out += " Call report now.";
            report_only_ = true;
        }
        return out;
    }

    // ---- The end ----

    void finish(const json::Value& report) {
        const std::string outcome = string_of(report, "outcome");
        json::Value result;
        std::string kind = kind_;
        if (kind.empty() || kind == "auto") {
            if (outcome == "achieved" || outcome == "not_achieved") {
                kind = "goal";
            } else if (outcome == "explored") {
                kind = "explore";
            } else {
                kind = "verify";
            }
        }
        result["kind"] = kind;
        result["outcome"] = outcome;
        const json::Value* evidence = report.find("evidence");
        const json::Value* found = report.find("findings");
        const bool bugs = found != nullptr && found->is_array() && !found->as_array().empty();
        int code = 3;
        if (outcome == "CONFIRMED" || outcome == "achieved" || (outcome == "explored" && !bugs)) {
            code = 0;
        } else if (outcome == "DENIED" || outcome == "not_achieved" || outcome == "explored") {
            code = 2;
        }
        if (kind == "verify") {
            result["verdict"] = outcome;
        }
        result["summary"] = string_of(report, "summary");
        result["evidence"] =
            evidence != nullptr && evidence->is_array() ? *evidence : json::Value(json::Array{});
        result["bugs"] = bugs ? *found : json::Value(json::Array{});
        end(std::move(result), code);
    }

    void end_unreported(const std::string& why) {
        log::warn("autopilot agent: " + why);
        json::Value result;
        result["kind"] = kind_.empty() ? std::string("auto") : kind_;
        result["outcome"] = "INCONCLUSIVE";
        if (kind_ == "verify") {
            result["verdict"] = "INCONCLUSIVE";
        }
        result["summary"] = "No report: " + why + ".";
        result["evidence"] = json::Array{};
        result["bugs"] = json::Array{};
        end(std::move(result), 3);
    }

    void end(json::Value result, int code) {
        json::Array detected;
        for (const json::Value& finding : findings_) {
            detected.push_back(finding);
        }
        result["detector_findings"] = std::move(detected);
        json::Array changes;
        for (const json::Value& change : changes_) {
            changes.push_back(change);
        }
        result["direct_changes"] = std::move(changes);
        json::Value agent;
        agent["model"] = served_model_.empty() ? options_.model : served_model_;
        agent["turns"] = turns_;
        agent["play_seconds"] = round1(game_time_ - started_at_);
        agent["input_tokens"] = input_tokens_;
        agent["output_tokens"] = output_tokens_;
        result["agent"] = std::move(agent);
        result["exit_code"] = code;
        const std::string report = report_text(result);

        json::Value frame;
        frame["type"] = "result";
        frame["result"] = std::move(result);
        frame["files"]["report.md"] = report;
        write(frame);
        phase_ = Phase::Done;
        log::info("autopilot agent: reported", {{"exit_code", code}, {"turns", turns_}});
    }

    [[nodiscard]] std::string report_text(const json::Value& result) const {
        const auto list = [](const json::Value* items) {
            std::string out;
            if (items != nullptr && items->is_array()) {
                for (const json::Value& item : items->as_array()) {
                    out += "- " +
                           (item.is_string() ? item.as_string()
                                             : json::write(item, json::Style::Compact)) +
                           "\n";
                }
            }
            return out;
        };
        std::string out = "# " + kind_title(string_of(result, "kind")) + ": " +
                          (prompt_.empty() ? std::string("explore the game") : prompt_) + "\n\n";
        out += "**Outcome: " + string_of(result, "outcome") + "**\n\n";
        out += string_of(result, "summary") + "\n";
        if (const std::string evidence = list(result.find("evidence")); !evidence.empty()) {
            out += "\n## Evidence\n\n" + evidence;
        }
        if (const std::string bugs = list(result.find("bugs")); !bugs.empty()) {
            out += "\n## Findings\n\n" + bugs;
        }
        if (!findings_.empty()) {
            out += "\n## What the detectors found\n\n";
            for (const json::Value& finding : findings_) {
                out += "- " + string_of(finding, "summary") + " (" +
                       string_of(finding, "detector") + ")\n";
            }
        }
        if (!changes_.empty()) {
            out += "\n## Direct changes to the game\n\n";
            for (const json::Value& change : changes_) {
                out += "- " + string_of(change, "verb") + " " +
                       json::write(*change.find("body"), json::Style::Compact) + ": " +
                       string_of(change, "why") + "\n";
            }
        }
        const json::Value& agent = *result.find("agent");
        out += "\nPlayed by " + string_of(agent, "model") + " in " +
               std::to_string(agent.find("turns")->as_int()) + " turns and " +
               json::write(*agent.find("play_seconds"), json::Style::Compact) +
               " seconds of play, with " +
               std::to_string(agent.find("input_tokens")->as_int() +
                              agent.find("output_tokens")->as_int()) +
               " tokens.\n";
        return out;
    }

    void fail(const std::string& message) {
        log::error("autopilot agent: " + message);
        json::Value result;
        result["kind"] = kind_;
        result["error"] = message;
        result["exit_code"] = 1;
        json::Value frame;
        frame["type"] = "result";
        frame["result"] = std::move(result);
        write(frame);
        phase_ = Phase::Done;
    }

    AgentOptions options_;
    std::vector<ai::ToolSpec> tools_;
    Phase phase_ = Phase::Hello;
    std::deque<json::Value> inbox_;
    std::deque<std::string> outbox_;
    std::deque<Step> steps_;
    Step current_;
    bool waiting_ = false;
    std::int64_t waiting_id_ = 0;
    std::int64_t next_id_ = 0;

    json::Value run_;
    std::string kind_;
    std::string prompt_;
    std::string project_;
    json::Value context_;
    json::Value screen_;
    std::string summary_;
    double play_seconds_ = kDefaultPlaySeconds;
    double game_time_ = 0.0;
    double started_at_ = 0.0;
    double planned_ = 0.0;

    std::vector<ai::Message> messages_;
    std::unique_ptr<ai::Call> call_;
    std::vector<Answer> answers_;
    std::optional<json::Value> report_;
    bool report_only_ = false;
    int turns_ = 0;
    std::int64_t input_tokens_ = 0;
    std::int64_t output_tokens_ = 0;
    std::string served_model_;
    std::vector<json::Value> findings_;
    std::vector<json::Value> new_findings_;
    std::vector<json::Value> changes_;
};

} // namespace

Connector agent_connector(AgentOptions options) {
    return [options = std::move(options)]() -> Result<std::unique_ptr<Transport>> {
        if (options.provider == nullptr) {
            return make_error(ErrorCode::InvalidArgument, "the agent has no model to play with");
        }
        return std::unique_ptr<Transport>(std::make_unique<Agent>(options));
    };
}

std::function<double()> game_clock(const world::World& world, double tick_seconds) {
    return [&world, tick_seconds] { return static_cast<double>(world.tick()) * tick_seconds; };
}

} // namespace slash::autopilot
