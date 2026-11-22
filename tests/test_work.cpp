#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/command/work.hpp"
#include "slash/core/json.hpp"
#include "slash/platform/file.hpp"
#include "slash/platform/process.hpp"
#include "slash/server/client.hpp"
#include "slash/server/server.hpp"
#include "slash/server/watch.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace json = slash::json;
namespace command = slash::command;
namespace work = slash::command::work;
namespace server = slash::server;
using namespace std::chrono_literals;

namespace {

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? std::string() : parsed.error().message));
    return std::move(parsed).value();
}

// A project folder of its own for each test, emptied first.
std::string fresh_project(std::string_view name) {
    const std::string root = "work_test_" + std::string(name);
    std::filesystem::remove_all(root);
    REQUIRE(slash::platform::create_directories(root).ok());
    return root;
}

// 2026-11-22T13:00:00Z, moved on by a millisecond each time it is read, so lines repeat.
work::Clock test_clock() {
    auto time = std::make_shared<std::int64_t>(1795352400000);
    return [time] { return (*time)++; };
}

// A session keeping a journal under a fixed ID and clock.
struct Journaled {
    slash::world::ComponentRegistry components;
    command::Registry commands;
    std::unique_ptr<work::Journal> journal;
    std::unique_ptr<command::Session> session;

    explicit Journaled(const std::string& root,
                       std::uint64_t max_bytes = work::Settings{}.max_bytes,
                       std::string id = "20261122-130000-test") {
        slash::world::register_builtin_components(components);
        command::register_builtin_commands(commands);
        work::JournalOptions options;
        options.project_root = root;
        options.host = "test";
        options.id = std::move(id);
        options.max_bytes = max_bytes;
        options.clock = test_clock();
        auto opened = work::Journal::open(std::move(options));
        REQUIRE_MESSAGE(opened.ok(), (opened.ok() ? std::string() : opened.error().message));
        journal = std::move(opened).value();
        command::SessionOptions session_options;
        session_options.project_root = root;
        session_options.capture_logs = false;
        session_options.journal = journal.get();
        session = std::make_unique<command::Session>(components, commands, session_options);
    }

    command::Reply call(std::string_view name, std::string_view params = "{}") {
        return session->call(name, parse(params));
    }

    std::vector<json::Value> lines() {
        journal->flush();
        auto read = work::read_journal(session->options().project_root, journal->id());
        REQUIRE(read.ok());
        return read.value();
    }
};

const json::Value* find_command(const std::vector<json::Value>& lines, std::string_view name) {
    for (const json::Value& line : lines) {
        if (const json::Value* command = line.find("command");
            command != nullptr && command->as_string() == name) {
            return &line;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE("the work journal records edits with their changes, folds reads and keeps errors") {
    const std::string root = fresh_project("record");
    Journaled run(root);
    // Nothing done yet: no file.
    CHECK(work::list_journals(root).empty());

    CHECK(run.call("entity.create", R"({"id": "player", "components": {"Transform2D": {}}})").ok);
    for (int i = 0; i < 3; ++i) {
        CHECK(run.call("world.query").ok);
    }
    CHECK(run.call("entity.get", R"({"id": "player"})").ok);
    CHECK(run.call("component.set",
                   R"({"id": "player", "components": {"Transform2D": {"position": [4, 5]}}})")
              .ok);
    CHECK_FALSE(run.call("entity.get", R"({"id": "playr"})").ok);
    CHECK_FALSE(run.call("ai.complete", R"({"api_key": "sk-123", "prompt": "hi"})").ok);
    CHECK(run.call("entity.create", R"({"id": "ghost", "dry_run": true})").ok);
    CHECK(run.call("scene.save", R"({"path": "levels/one.scene.json"})").ok);

    const std::vector<json::Value> lines = run.lines();
    REQUIRE(lines.size() >= 7);
    const json::Value& first = lines.front();
    CHECK(first.find("session")->as_string() == "20261122-130000-test");
    CHECK(first.find("start")->find("host")->as_string() == "test");
    CHECK(first.find("start")->find("pid")->as_int() == slash::platform::process_id());
    CHECK(first.find("time")->as_string() == "2026-11-22T13:00:00.001Z");

    const json::Value* created = find_command(lines, "entity.create");
    REQUIRE(created != nullptr);
    CHECK(*created->find("changes")->find("added") == parse(R"(["player"])"));
    CHECK(created->find("params")->find("id")->as_string() == "player");

    // The three queries and the get are one line.
    const json::Value* reads = nullptr;
    for (const json::Value& line : lines) {
        if (line.find("reads") != nullptr && reads == nullptr) {
            reads = &line;
        }
    }
    REQUIRE(reads != nullptr);
    CHECK(reads->find("reads")->find("world.query")->as_int() == 3);
    CHECK(reads->find("reads")->find("entity.get")->as_int() == 1);

    const json::Value* set = find_command(lines, "component.set");
    REQUIRE(set != nullptr);
    CHECK(*set->find("changes")->find("changed")->find("player")->find("Transform2D") ==
          parse(R"(["position"])"));

    // A failed read is a line of its own, with its error.
    bool failed_get = false;
    for (const json::Value& line : lines) {
        if (const json::Value* command = line.find("command");
            command != nullptr && command->as_string() == "entity.get" &&
            line.find("error") != nullptr) {
            failed_get = true;
            CHECK(line.find("error")->find("code")->as_int() == command::code::kNotFound);
        }
    }
    CHECK(failed_get);

    // Secrets never reach the journal.
    const json::Value* asked = find_command(lines, "ai.complete");
    REQUIRE(asked != nullptr);
    CHECK(asked->find("params")->find("api_key")->as_string() == "[left out]");
    CHECK(asked->find("params")->find("prompt")->as_string() == "hi");
    const std::string text = slash::platform::read_file(root + "/" + run.journal->path()).value();
    CHECK(text.find("sk-123") == std::string::npos);

    // A dry run changes nothing, so it is a read.
    const json::Value* dry = nullptr;
    for (const json::Value& line : lines) {
        if (const json::Value* command = line.find("command");
            command != nullptr && command->as_string() == "entity.create" &&
            line.find("read") != nullptr) {
            dry = &line;
        }
    }
    CHECK(dry == nullptr); // folded into a run of reads

    const json::Value* saved = find_command(lines, "scene.save");
    REQUIRE(saved != nullptr);
    CHECK(*saved->find("files") == parse(R"(["levels/one.scene.json"])"));

    // Lines are numbered in order.
    for (std::size_t i = 0; i < lines.size(); ++i) {
        CHECK(lines[i].find("n")->as_int() == static_cast<std::int64_t>(i + 1));
    }

    // The session's end is the last line.
    run.session.reset();
    run.journal.reset();
    auto ended = work::read_journal(root, "20261122-130000-test");
    REQUIRE(ended.ok());
    CHECK(ended.value().back().find("end") != nullptr);
    CHECK(work::describe(ended.value().back()).find("session ended") != std::string::npos);
}

TEST_CASE("tests and checks keep their results in the journal") {
    const std::string root = fresh_project("checks");
    Journaled run(root);
    CHECK(run.call("storyboard.check").ok);
    const std::vector<json::Value> lines = run.lines();
    const json::Value* check = find_command(lines, "storyboard.check");
    REQUIRE(check != nullptr);
    CHECK(check->find("result") != nullptr);
    CHECK(check->find("read") != nullptr);
}

TEST_CASE("journals rotate past a quarter of the project's share and old ones are pruned") {
    const std::string root = fresh_project("rotate");
    {
        Journaled run(root, 8 * 1024, "20261121-090000-aaaa");
        for (int i = 0; i < 40; ++i) {
            CHECK(run.call("entity.create", R"({"components": {"Transform2D": {}}})").ok);
        }
        const std::vector<work::JournalFile> files = work::list_journals(root);
        REQUIRE(files.size() == 1);
        CHECK(files.front().paths.size() == 2);
        CHECK(files.front().paths.front().ends_with(".1.jsonl"));
        CHECK(files.front().bytes <= 4 * 1024);
        // Lines go on being numbered across the two files.
        const std::vector<json::Value> lines = run.lines();
        for (std::size_t i = 1; i < lines.size(); ++i) {
            CHECK(lines[i].find("n")->as_int() > lines[i - 1].find("n")->as_int());
        }
    }
    {
        Journaled other(root, 8 * 1024, "20261121-100000-bbbb");
        for (int i = 0; i < 40; ++i) {
            CHECK(other.call("entity.create", R"({"components": {"Transform2D": {}}})").ok);
        }
    }
    // Opening a third finds the two over half the share and drops the older.
    Journaled third(root, 8 * 1024, "20261121-110000-cccc");
    std::vector<std::string> ids;
    for (const work::JournalFile& file : work::list_journals(root)) {
        ids.push_back(file.id);
    }
    CHECK(ids == std::vector<std::string>{"20261121-100000-bbbb"});
}

TEST_CASE("sessions.json lists running sessions and drops ended ones") {
    const std::string root = fresh_project("sessions");
    work::SessionEntry mine;
    mine.id = "20261122-120000-0001";
    mine.host = "slash-mcp";
    mine.pid = slash::platform::process_id();
    mine.url = "http://127.0.0.1:1";
    mine.token = "t";
    REQUIRE(work::add_session(root, mine).ok());
    work::SessionEntry gone = mine;
    gone.id = "20261122-130000-0002";
    gone.pid = 0x7FFFFFF0; // no such process
    REQUIRE(work::add_session(root, gone).ok());

    const std::vector<work::SessionEntry> live = work::live_sessions(root);
    REQUIRE(live.size() == 1);
    CHECK(live.front().id == mine.id);
    CHECK(live.front().host == "slash-mcp");
    CHECK(work::pick_session(root) == mine.id);
    CHECK(work::pick_session(root, "named") == "named");
#if !defined(_WIN32)
    const auto permissions =
        std::filesystem::status(root + "/" + std::string(work::kSessionsFile)).permissions();
    CHECK((permissions & (std::filesystem::perms::group_all |
                          std::filesystem::perms::others_all)) == std::filesystem::perms::none);
#endif
    REQUIRE(work::remove_session(root, mine.id).ok());
    CHECK(work::live_sessions(root).empty());
    CHECK(work::pick_session(root).empty());
}

TEST_CASE("a watch server serves reads and refuses edits") {
    const std::string root = fresh_project("watch");
    Journaled run(root);
    CHECK(run.call("entity.create", R"({"id": "player"})").ok);
    auto started =
        server::Watched::start(*run.session, run.journal->id(), "test", run.journal->path());
    REQUIRE(started.ok());
    const std::unique_ptr<server::Watched>& watched = started.value();
    const std::vector<work::SessionEntry> live = work::live_sessions(root);
    REQUIRE(live.size() == 1);
    CHECK(live.front().journal == run.journal->path());

    server::Endpoint endpoint;
    endpoint.host = "127.0.0.1";
    endpoint.token = live.front().token;
    const std::string& url = live.front().url;
    endpoint.port = std::stoi(url.substr(url.rfind(':') + 1));
    const auto send = [&](std::string_view path, std::string_view body) {
        server::HttpRequest request;
        request.path = path;
        request.body = body;
        auto future =
            std::async(std::launch::async, [&] { return server::send(endpoint, request); });
        while (future.wait_for(0ms) != std::future_status::ready) {
            watched->poll(5ms);
        }
        auto response = future.get();
        REQUIRE(response.ok());
        return response.value();
    };
    const auto read = send("/v1/commands/world.query", "");
    CHECK(read.status == 200);
    CHECK(*parse(read.body).find("entities") == parse(R"(["player"])"));
    const auto edit = send("/v1/commands/entity.create", R"({"id": "intruder"})");
    CHECK(edit.status == 403);
    CHECK(run.session->world().size() == 1);
    CHECK_FALSE(run.session->watching());

    // Watchers aren't in the journal.
    for (const json::Value& line : run.lines()) {
        CHECK(line.find("reads") == nullptr);
    }
    started.value().reset();
    CHECK(work::live_sessions(root).empty());
}

TEST_CASE("work.feed pages through a journal and work.digest sums it up") {
    const std::string root = fresh_project("feed");
    Journaled run(root);
    CHECK(run.call("entity.create", R"({"id": "a"})").ok);
    CHECK(run.call("entity.create", R"({"id": "b"})").ok);
    CHECK(run.call("entity.destroy", R"({"id": "a"})").ok);
    CHECK_FALSE(run.call("entity.destroy", R"({"id": "zzz"})").ok);
    CHECK(run.call("scene.save", R"({"path": "main.scene.json"})").ok);

    command::Reply page = run.call("work.feed", R"({"after": 0, "limit": 2})");
    REQUIRE(page.ok);
    CHECK(page.value.find("session")->as_string() == run.journal->id());
    CHECK(page.value.find("lines")->as_array().size() == 2);
    CHECK(page.value.find("more")->as_bool());
    const std::int64_t next = page.value.find("next")->as_int();
    CHECK(next == 2);
    page = run.call("work.feed", R"({"after": 2, "text": true})");
    REQUIRE(page.ok);
    const json::Array& text = page.value.find("lines")->as_array();
    REQUIRE_FALSE(text.empty());
    CHECK(text.front().as_string().find("entity.create") != std::string::npos);
    CHECK(text.front().as_string().find("+b") != std::string::npos);

    command::Reply summed = run.call("work.digest", R"({"since": "2026-11-22T12:00:00Z"})");
    REQUIRE(summed.ok);
    const json::Value& digest = summed.value;
    CHECK(digest.find("commands")->as_int() == 5);
    CHECK(digest.find("failed")->as_int() == 1);
    CHECK(digest.find("entities")->find("added")->as_int() == 2);
    CHECK(digest.find("entities")->find("removed")->as_int() == 1);
    CHECK(*digest.find("scenes") == parse(R"(["main.scene.json"])"));
    CHECK(digest.find("last_failure")->find("command")->as_string() == "entity.destroy");
    CHECK(digest.find("lines")->as_array().size() >= 2);

    CHECK_FALSE(run.call("work.digest", R"({"since": "yesterday"})").ok);
    CHECK_FALSE(run.call("work.feed", R"({"session": "nope"})").ok);
}

TEST_CASE("journal times read and write as UTC") {
    CHECK(work::format_time(1795352400000) == "2026-11-22T13:00:00.000Z");
    CHECK(work::format_time(1795352703120) == "2026-11-22T13:05:03.120Z");
    CHECK(work::parse_duration("45s").value() == 45000);
    CHECK(work::parse_duration("90m").value() == 90 * 60000);
    CHECK(work::parse_duration("3h").value() == 3 * 3600000);
    CHECK(work::parse_duration("2d").value() == 2 * 86400000);
    CHECK_FALSE(work::parse_duration("3").ok());
    CHECK_FALSE(work::parse_duration("h").ok());
    CHECK(work::parse_since("2026-11-22T13:05:03.120Z").value() == 1795352703120);
    CHECK(work::parse_since("2026-11-22T13:05Z").value() == 1795352700000);
    CHECK_FALSE(work::parse_since("2026-13-40T00:00Z").ok());
    CHECK(work::line_time(parse(R"({"time": "2026-11-22T13:00:00.000Z"})")) == 1795352400000);
    CHECK_FALSE(work::line_time(parse(R"({"n": 1})")).has_value());
}

TEST_CASE("project.json's work block turns the journal and watch server off") {
    const std::string root = fresh_project("settings");
    CHECK(work::read_settings(root).journal);
    REQUIRE(
        slash::platform::write_file(root + "/project.json",
                                    R"({"work": {"journal": false, "serve": false, "max_mb": 2}})")
            .ok());
    const work::Settings settings = work::read_settings(root);
    CHECK_FALSE(settings.journal);
    CHECK_FALSE(settings.serve);
    CHECK(settings.max_bytes == 2U << 20U);
    server::Work host(root, "test");
    CHECK(host.journal() == nullptr);
}
