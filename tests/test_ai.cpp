#include "slash/ai/adapters.hpp"
#include "slash/ai/commands.hpp"
#include "slash/ai/config.hpp"
#include "slash/ai/provider.hpp"
#include "slash/ai/replay.hpp"
#include "slash/ai/transport.hpp"
#include "slash/ai/world.hpp"
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/mcp.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/platform/file.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <httplib.h>
#include <map>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pk.h>
#include <mbedtls/x509_crt.h>
#include <memory>
#include <mutex>
#include <psa/crypto.h>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace slash;

namespace {

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? std::string() : parsed.error().message));
    return std::move(parsed).value();
}

bool contains(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

// Splits text into pieces of a few bytes, so events and lines cross the pieces' edges.
std::vector<std::string> pieces(std::string_view text, std::size_t size = 7) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < text.size(); i += size) {
        out.emplace_back(text.substr(i, size));
    }
    return out;
}

// A stream of server-sent events with these data lines, each named if a name is given.
std::string sse(const std::vector<std::pair<std::string, std::string>>& events) {
    std::string out;
    for (const auto& [name, data] : events) {
        if (!name.empty()) {
            out += "event: " + name + "\n";
        }
        out += "data: " + data + "\n\n";
    }
    return out;
}

// A transport that answers from a script and keeps each request it is sent.
class FakeTransport final : public ai::Transport {
public:
    struct Reply {
        int status = 200;
        std::string body;
        std::string retry_after{}; // NOLINT(readability-redundant-member-init)
    };

    explicit FakeTransport(std::vector<Reply> replies) : replies_(std::move(replies)) {}

    Result<ai::HttpResponse> post(const ai::HttpRequest& request, const ai::BodySink& sink,
                                  const ai::CancelFlag* /*cancel*/) override {
        const std::scoped_lock lock(mutex_);
        requests.push_back(request);
        const Reply& reply = replies_[std::min(requests.size(), replies_.size()) - 1];
        ai::HttpResponse out;
        out.status = reply.status;
        out.retry_after = reply.retry_after;
        if (reply.status == 200 && sink) {
            for (const std::string& piece : pieces(reply.body)) {
                if (!sink(piece)) {
                    return make_error(ErrorCode::PlatformFailure, "the reply was stopped");
                }
            }
        } else {
            out.body = reply.body;
        }
        return out;
    }

    [[nodiscard]] json::Value body(std::size_t index = 0) const {
        return parse(requests.at(index).body);
    }

    [[nodiscard]] std::string header(std::string_view name, std::size_t index = 0) const {
        for (const auto& [key, value] : requests.at(index).headers) {
            if (key == name) {
                return value;
            }
        }
        return {};
    }

    std::vector<ai::HttpRequest> requests;

private:
    std::vector<Reply> replies_;
    std::mutex mutex_;
};

std::shared_ptr<ai::Provider> adapter(std::string_view type,
                                      const std::shared_ptr<FakeTransport>& transport,
                                      ai::AdapterOptions options = {}) {
    if (options.model.empty()) {
        options.model = "test-model";
    }
    options.transport = transport;
    return ai::make_adapter(type, std::move(options)).value();
}

ai::Message message(ai::Role role, std::string text) {
    return {.role = role, .text = std::move(text)};
}

ai::Request ask(std::string text) {
    ai::Request request;
    request.messages.push_back(message(ai::Role::User, std::move(text)));
    return request;
}

// A conversation with everything adapters translate: instructions, an image, a tool call and its
// result, and a tool the model may call.
ai::Request tool_conversation(json::Value native = {}) {
    ai::Request request;
    request.messages.push_back(message(ai::Role::System, "You are a shopkeeper."));
    ai::Message user = message(ai::Role::User, "What is this?");
    user.images.push_back({"image/png", "PNG"});
    request.messages.push_back(std::move(user));
    ai::Message assistant = message(ai::Role::Assistant, "Let me look.");
    assistant.tool_calls.push_back(
        {.id = "call_1", .name = "item_find", .arguments = parse(R"({"name":"sword"})")});
    assistant.native = std::move(native);
    request.messages.push_back(std::move(assistant));
    ai::Message result = message(ai::Role::Tool, R"({"price":12})");
    result.tool_call_id = "call_1";
    request.messages.push_back(std::move(result));
    request.tools.push_back({.name = "item_find",
                             .description = "Finds an item.",
                             .parameters = parse(R"({"type":"object"})")});
    request.params.temperature = 0.5;
    request.params.stop = {"END"};
    return request;
}

std::string text_of(const Result<ai::Response>& response) {
    REQUIRE_MESSAGE(response.ok(), (response.ok() ? std::string() : response.error().message));
    return response.value().message.text;
}

// A certificate for localhost that is its own issuer, and its key, as PEM files. Made when a test
// runs, so no key is committed.
struct Certificate {
    std::string cert_file;
    std::string key_file;
};

Certificate make_certificate(const std::string& folder) {
    REQUIRE(psa_crypto_init() == PSA_SUCCESS);
    mbedtls_entropy_context entropy;
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_context random;
    mbedtls_ctr_drbg_init(&random);
    mbedtls_pk_context key;
    mbedtls_pk_init(&key);
    mbedtls_x509write_cert writer;
    mbedtls_x509write_crt_init(&writer);
    std::array<unsigned char, 4096> cert_pem{};
    std::array<unsigned char, 4096> key_pem{};
    std::array<unsigned char, 1> serial = {1};
    const bool made =
        mbedtls_ctr_drbg_seed(&random, mbedtls_entropy_func, &entropy, nullptr, 0) == 0 &&
        mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) == 0 &&
        mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key), mbedtls_ctr_drbg_random,
                            &random) == 0 &&
        mbedtls_x509write_crt_set_subject_name(&writer, "CN=localhost") == 0 &&
        mbedtls_x509write_crt_set_issuer_name(&writer, "CN=localhost") == 0 &&
        mbedtls_x509write_crt_set_serial_raw(&writer, serial.data(), serial.size()) == 0 &&
        mbedtls_x509write_crt_set_validity(&writer, "20250101000000", "20450101000000") == 0 &&
        mbedtls_x509write_crt_set_basic_constraints(&writer, 1, -1) == 0;
    mbedtls_x509write_crt_set_version(&writer, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&writer, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&writer, &key);
    mbedtls_x509write_crt_set_issuer_key(&writer, &key);
    const bool written = made &&
                         mbedtls_x509write_crt_pem(&writer, cert_pem.data(), cert_pem.size(),
                                                   mbedtls_ctr_drbg_random, &random) == 0 &&
                         mbedtls_pk_write_key_pem(&key, key_pem.data(), key_pem.size()) == 0;
    mbedtls_x509write_crt_free(&writer);
    mbedtls_pk_free(&key);
    mbedtls_ctr_drbg_free(&random);
    mbedtls_entropy_free(&entropy);
    REQUIRE(written);
    Certificate out{folder + "/localhost.crt", folder + "/localhost.key"};
    std::filesystem::create_directories(folder);
    REQUIRE(platform::write_file(out.cert_file, reinterpret_cast<const char*>(cert_pem.data())));
    REQUIRE(platform::write_file(out.key_file, reinterpret_cast<const char*>(key_pem.data())));
    return out;
}

// An OpenAI-compatible server on the loopback interface that answers each chat completion with
// the next scripted reply (the last repeats), streaming it when asked, over https if given a
// certificate. It keeps every body.
class FakeModelServer {
public:
    explicit FakeModelServer(std::vector<json::Value> replies,
                             const Certificate* certificate = nullptr)
        : server_(certificate == nullptr
                      ? std::make_unique<httplib::Server>()
                      : std::make_unique<httplib::SSLServer>(certificate->cert_file.c_str(),
                                                             certificate->key_file.c_str())),
          replies_(std::move(replies)), scheme_(certificate == nullptr ? "http" : "https") {
        server_->Post("/v1/chat/completions", [this](const httplib::Request& request,
                                                     httplib::Response& response) {
            json::Value reply;
            {
                const std::scoped_lock lock(mutex_);
                bodies_.push_back(parse(request.body));
                reply = replies_[std::min(bodies_.size(), replies_.size()) - 1];
            }
            if (const json::Value* status = reply.find("status"); status != nullptr) {
                response.status = static_cast<int>(status->as_int());
                response.set_content(json::write(*reply.find("body"), json::Style::Compact),
                                     "application/json");
                return;
            }
            const json::Value body = parse(request.body);
            const json::Value* stream = body.find("stream");
            if (stream == nullptr || !stream->as_bool()) {
                response.set_content(json::write(reply, json::Style::Compact), "application/json");
                return;
            }
            // The reply's text in two deltas, then the rest of the reply.
            const json::Value& message =
                reply.find("choices")->as_array()[0].find("message")->as_object().at("content");
            const std::string text = message.is_string() ? message.as_string() : std::string();
            const std::string half = text.substr(0, text.size() / 2);
            std::string events;
            for (const std::string& part : {half, text.substr(half.size())}) {
                json::Value chunk;
                chunk["choices"] = json::Array(1);
                json::Value& choice = chunk["choices"].as_array().front();
                choice["index"] = 0;
                choice["delta"]["content"] = part;
                events += "data: " + json::write(chunk, json::Style::Compact) + "\n\n";
            }
            json::Value last;
            last["choices"] = json::Array{};
            last["usage"] = *reply.find("usage");
            last["model"] = *reply.find("model");
            json::Value& finish = last["choices"].as_array().emplace_back();
            finish["finish_reason"] = "stop";
            finish["delta"] = json::Object{};
            events += "data: " + json::write(last, json::Style::Compact) + "\n\ndata: [DONE]\n\n";
            response.set_chunked_content_provider(
                "text/event-stream", [events](std::size_t /*offset*/, httplib::DataSink& sink) {
                    for (const std::string& piece : pieces(events, 16)) {
                        sink.write(piece.data(), piece.size());
                    }
                    sink.done();
                    return true;
                });
        });
        port_ = server_->bind_to_any_port("127.0.0.1");
        thread_ = std::thread([this] { server_->listen_after_bind(); });
        server_->wait_until_ready();
    }

    ~FakeModelServer() {
        server_->stop();
        thread_.join();
    }

    FakeModelServer(const FakeModelServer&) = delete;
    FakeModelServer& operator=(const FakeModelServer&) = delete;

    // The API's URL at a name for this machine: 127.0.0.1 or localhost.
    [[nodiscard]] std::string url(std::string_view host = "127.0.0.1") const {
        return scheme_ + "://" + std::string(host) + ":" + std::to_string(port_) + "/v1";
    }

    [[nodiscard]] std::vector<json::Value> bodies() {
        const std::scoped_lock lock(mutex_);
        return bodies_;
    }

private:
    std::unique_ptr<httplib::Server> server_;
    std::vector<json::Value> replies_;
    std::string scheme_;
    std::vector<json::Value> bodies_;
    std::mutex mutex_;
    int port_ = 0;
    std::thread thread_;
};

json::Value chat_reply(std::string_view text) {
    json::Value reply;
    reply["model"] = "local-model";
    json::Value choice;
    choice["finish_reason"] = "stop";
    choice["message"]["role"] = "assistant";
    choice["message"]["content"] = text;
    reply["choices"].push_back(std::move(choice));
    reply["usage"]["prompt_tokens"] = 12;
    reply["usage"]["completion_tokens"] = 3;
    return reply;
}

void write(const std::string& path, std::string_view text) {
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    REQUIRE(platform::write_file(path, text).ok());
}

std::string read(const std::string& path) {
    auto text = platform::read_file(path);
    REQUIRE(text.ok());
    return text.value();
}

struct Engine {
    world::ComponentRegistry components;
    command::Registry commands;
    std::unique_ptr<command::Session> session;

    explicit Engine(std::string root) {
        world::register_builtin_components(components);
        command::register_builtin_commands(commands);
        ai::register_commands(commands);
        command::SessionOptions options;
        options.project_root = std::move(root);
        options.capture_logs = false;
        session = std::make_unique<command::Session>(components, commands, std::move(options));
    }

    command::Reply call(std::string_view name, const json::Value& params) {
        return session->call(name, params);
    }

    json::Value ok(std::string_view name, const json::Value& params) {
        command::Reply reply = call(name, params);
        REQUIRE_MESSAGE(reply.ok, reply.message);
        return std::move(reply.value);
    }
};

} // namespace

TEST_CASE("server-sent events are read however the stream is cut") {
    const std::string stream =
        "event: first\ndata: 1\n\n: keep-alive\ndata: two\r\ndata: lines\r\n\r\ndata: last";
    ai::SseParser parser;
    std::vector<ai::SseEvent> events;
    for (const std::string& piece : pieces(stream, 1)) {
        for (ai::SseEvent& event : parser.feed(piece)) {
            events.push_back(std::move(event));
        }
    }
    for (ai::SseEvent& event : parser.finish()) {
        events.push_back(std::move(event));
    }
    REQUIRE(events.size() == 3);
    CHECK(events[0].event == "first");
    CHECK(events[0].data == "1");
    CHECK(events[1].event.empty());
    CHECK(events[1].data == "two\nlines");
    CHECK(events[2].data == "last");
}

TEST_CASE("URLs split into scheme, host, port and path") {
    const ai::Url local = ai::parse_url("http://localhost:11434/v1").value();
    CHECK(local.scheme == "http");
    CHECK(local.host == "localhost");
    CHECK(local.port == 11434);
    CHECK(local.path == "/v1");
    const ai::Url hosted = ai::parse_url("https://api.example.com").value();
    CHECK(hosted.port == 443);
    CHECK(hosted.path.empty());
    const ai::Url ipv6 = ai::parse_url("http://[::1]:8080/a").value();
    CHECK(ipv6.host == "::1");
    CHECK(ipv6.port == 8080);
    for (const char* bad :
         {"localhost:1234", "ftp://example.com", "http://:80/", "http://h:99999"}) {
        CHECK_FALSE(ai::parse_url(bad).ok());
    }
}

TEST_CASE("requests and responses read back from JSON as they were") {
    const ai::Request request = tool_conversation(parse(R"({"type":"anthropic","content":[]})"));
    const json::Value written = ai::to_json(request);
    CHECK(written.find("messages")->as_array()[1].find("images")->as_array()[0] ==
          parse(R"({"media_type":"image/png","data":"UE5H"})"));
    const ai::Request read_back = ai::request_from_json(written).value();
    CHECK(ai::to_json(read_back) == written);
    CHECK(read_back.messages[1].images[0].data == "PNG");

    ai::Response response;
    response.message = message(ai::Role::Assistant, "Twelve gold.");
    response.output = parse(R"({"price":12})");
    response.stop = ai::StopReason::MaxTokens;
    response.usage = {.input_tokens = 40, .output_tokens = 5};
    response.model = "m";
    CHECK(ai::to_json(ai::response_from_json(ai::to_json(response)).value()) ==
          ai::to_json(response));

    auto bad = ai::message_from_json(parse(R"({"role":"narrator"})"));
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.error().message ==
          "/role: unknown role \"narrator\"; use system, user, assistant or tool");
    CHECK_FALSE(ai::message_from_json(parse(R"({"role":"tool","text":"x"})")).ok());
}

TEST_CASE("requests fit what the model can do, and replies are checked against the schema") {
    ai::Request request = ask("Rate the sword.");
    request.output_schema = parse(R"({"type":"object","required":["stars"]})");
    ai::Capabilities capabilities;
    capabilities.json_schema_output = false;
    const ai::Request fitted = ai::fit_request(request, capabilities).value();
    REQUIRE(fitted.messages.size() == 2);
    CHECK(fitted.messages[0].role == ai::Role::System);
    CHECK(contains(fitted.messages[0].text, R"({"required":["stars"],"type":"object"})"));

    request.messages.insert(request.messages.begin(), message(ai::Role::System, "Be terse."));
    const ai::Request appended = ai::fit_request(request, capabilities).value();
    CHECK(appended.messages.size() == 2);
    CHECK(appended.messages[0].text.starts_with("Be terse.\n\nReply with only a JSON value"));

    capabilities.tools = false;
    capabilities.vision = false;
    CHECK_FALSE(ai::fit_request(tool_conversation(), capabilities).ok());
    ai::Request with_image = ask("Look.");
    with_image.messages[0].images.push_back({"image/png", "PNG"});
    capabilities.tools = true;
    CHECK(contains(ai::fit_request(with_image, capabilities).error().message, "vision"));

    const json::Value schema =
        parse(R"({"type":"object","properties":{"stars":{"type":"integer"}}})");
    CHECK(ai::parse_output("```json\n{\"stars\": 4}\n```", schema).value() ==
          parse(R"({"stars":4})"));
    CHECK(contains(ai::parse_output("Four stars.", schema).error().message, "is not JSON"));
    CHECK(ai::parse_output(R"({"stars":"four"})", schema).error().message ==
          "the model's reply does not match output_schema: expected integer, got string at "
          "/stars");
}

TEST_CASE("the openai adapter sends chat completions and reads the reply") {
    auto transport = std::make_shared<FakeTransport>(std::vector<FakeTransport::Reply>{{200, R"({
        "model": "test-model-2", "usage": {"prompt_tokens": 30, "completion_tokens": 9},
        "choices": [{"finish_reason": "tool_calls", "message": {"role": "assistant", "content": null,
          "tool_calls": [
            {"id": "call_2", "type": "function", "function": {"name": "item_find", "arguments": "{\"name\":\"shield\"}"}},
            {"id": "call_3", "type": "function", "function": {"name": "item_find", "arguments": "{oops"}}]}}]})"}});
    ai::AdapterOptions options;
    options.url = "http://localhost:8080/v1/";
    options.api_key = "secret";
    options.max_tokens = 100;
    options.max_tokens_param = "max_completion_tokens";
    options.headers = {{"X-Title", "Game"}};
    ai::Request request = tool_conversation();
    request.output_schema = parse(R"({"type":"object"})");
    request.params.seed = 7;
    auto response = adapter("openai", transport, options)->complete(request);
    REQUIRE_MESSAGE(response.ok(), (response.ok() ? std::string() : response.error().message));

    CHECK(transport->requests[0].url == "http://localhost:8080/v1/chat/completions");
    CHECK(transport->header("Authorization") == "Bearer secret");
    CHECK(transport->header("X-Title") == "Game");
    CHECK(transport->body() == parse(R"({
        "model": "test-model",
        "messages": [
          {"role": "system", "content": "You are a shopkeeper."},
          {"role": "user", "content": [
            {"type": "image_url", "image_url": {"url": "data:image/png;base64,UE5H"}},
            {"type": "text", "text": "What is this?"}]},
          {"role": "assistant", "content": "Let me look.", "tool_calls": [
            {"id": "call_1", "type": "function", "function": {"name": "item_find", "arguments": "{\"name\":\"sword\"}"}}]},
          {"role": "tool", "tool_call_id": "call_1", "content": "{\"price\":12}"}],
        "tools": [{"type": "function", "function": {"name": "item_find", "description": "Finds an item.", "parameters": {"type": "object"}}}],
        "max_completion_tokens": 100, "temperature": 0.5, "stop": ["END"], "seed": 7,
        "response_format": {"type": "json_schema", "json_schema": {"name": "output", "schema": {"type": "object"}}}})"));

    const ai::Response& reply = response.value();
    CHECK(reply.message.role == ai::Role::Assistant);
    CHECK(reply.message.text.empty());
    CHECK(reply.stop == ai::StopReason::ToolCalls);
    CHECK(reply.model == "test-model-2");
    CHECK(reply.usage.input_tokens == 30);
    CHECK(reply.usage.output_tokens == 9);
    REQUIRE(reply.message.tool_calls.size() == 2);
    CHECK(reply.message.tool_calls[0].id == "call_2");
    CHECK(reply.message.tool_calls[0].arguments == parse(R"({"name":"shield"})"));
    // Arguments that aren't JSON stay as text, for the tool to reject.
    CHECK(reply.message.tool_calls[1].arguments == json::Value("{oops"));
    CHECK(reply.output.is_null());
}

TEST_CASE("the openai adapter streams text and tool calls") {
    const std::string stream = sse({
        {"",
         R"({"model":"m","choices":[{"index":0,"delta":{"role":"assistant","content":"Hel"}}]})"},
        {"", R"({"choices":[{"index":0,"delta":{"content":"lo there."}}]})"},
        {"",
         R"({"choices":[{"index":0,"delta":{"tool_calls":[{"index":0,"id":"call_9","function":{"name":"item_","arguments":"{\"na"}}]}}]})"},
        {"",
         R"({"choices":[{"index":0,"delta":{"tool_calls":[{"index":0,"function":{"name":"find","arguments":"me\":\"axe\"}"}}]}}]})"},
        {"", R"({"choices":[{"index":0,"delta":{},"finish_reason":"tool_calls"}]})"},
        {"", R"({"choices":[],"usage":{"prompt_tokens":5,"completion_tokens":4}})"},
        {"", "[DONE]"},
    });
    auto transport =
        std::make_shared<FakeTransport>(std::vector<FakeTransport::Reply>{{200, stream}});
    std::string streamed;
    auto response =
        adapter("openai", transport)->complete(ask("Hi"), [&streamed](std::string_view text) {
            streamed += text;
        });
    CHECK(text_of(response) == "Hello there.");
    CHECK(streamed == "Hello there.");
    CHECK(transport->body().find("stream")->as_bool());
    CHECK(transport->body().find("stream_options")->find("include_usage")->as_bool());
    const ai::Response& reply = response.value();
    CHECK(reply.stop == ai::StopReason::ToolCalls);
    CHECK(reply.model == "m");
    CHECK(reply.usage.output_tokens == 4);
    REQUIRE(reply.message.tool_calls.size() == 1);
    CHECK(reply.message.tool_calls[0].id == "call_9");
    CHECK(reply.message.tool_calls[0].name == "item_find");
    CHECK(reply.message.tool_calls[0].arguments == parse(R"({"name":"axe"})"));
}

TEST_CASE("the anthropic adapter sends messages and returns thinking blocks unchanged") {
    const json::Value native = parse(R"({"type":"anthropic","content":[
        {"type":"thinking","thinking":"","signature":"sig-1"},
        {"type":"text","text":"Let me look."},
        {"type":"tool_use","id":"call_1","name":"item_find","input":{"name":"sword"}}]})");
    ai::Request request = tool_conversation(native);
    ai::Message second = message(ai::Role::Tool, "out of stock");
    second.tool_call_id = "call_0";
    second.is_error = true;
    request.messages.push_back(std::move(second));
    request.messages.push_back(message(ai::Role::User, "And the shield?"));
    request.messages.push_back(message(ai::Role::System, "Answer in one line."));
    request.output_schema = parse(R"({"type":"object"})");
    auto transport = std::make_shared<FakeTransport>(std::vector<FakeTransport::Reply>{{200, R"({
        "model": "test-model", "stop_reason": "tool_use",
        "usage": {"input_tokens": 10, "cache_read_input_tokens": 90, "output_tokens": 20},
        "content": [{"type": "thinking", "thinking": "", "signature": "sig-2"},
                    {"type": "text", "text": "Checking."},
                    {"type": "tool_use", "id": "toolu_5", "name": "item_find", "input": {"name": "shield"}}]})"}});
    ai::AdapterOptions options;
    options.api_key = "secret";
    auto response = adapter("anthropic", transport, options)->complete(request);
    CHECK(text_of(response) == "Checking.");

    CHECK(transport->requests[0].url == "https://api.anthropic.com/v1/messages");
    CHECK(transport->header("x-api-key") == "secret");
    CHECK(transport->header("anthropic-version") == "2023-06-01");
    CHECK(transport->body() == parse(R"({
        "model": "test-model", "max_tokens": 16000,
        "system": "You are a shopkeeper.\n\nAnswer in one line.",
        "messages": [
          {"role": "user", "content": [
            {"type": "image", "source": {"type": "base64", "media_type": "image/png", "data": "UE5H"}},
            {"type": "text", "text": "What is this?"}]},
          {"role": "assistant", "content": [
            {"type": "thinking", "thinking": "", "signature": "sig-1"},
            {"type": "text", "text": "Let me look."},
            {"type": "tool_use", "id": "call_1", "name": "item_find", "input": {"name": "sword"}}]},
          {"role": "user", "content": [
            {"type": "tool_result", "tool_use_id": "call_1", "content": "{\"price\":12}"},
            {"type": "tool_result", "tool_use_id": "call_0", "content": "out of stock", "is_error": true},
            {"type": "text", "text": "And the shield?"}]}],
        "tools": [{"name": "item_find", "description": "Finds an item.", "input_schema": {"type": "object"}}],
        "temperature": 0.5, "stop_sequences": ["END"],
        "output_config": {"format": {"type": "json_schema", "schema": {"type": "object"}}}})"));

    const ai::Response& reply = response.value();
    CHECK(reply.stop == ai::StopReason::ToolCalls);
    CHECK(reply.usage.input_tokens == 100);
    CHECK(reply.usage.output_tokens == 20);
    REQUIRE(reply.message.tool_calls.size() == 1);
    CHECK(reply.message.tool_calls[0].id == "toolu_5");
    CHECK(reply.message.tool_calls[0].arguments == parse(R"({"name":"shield"})"));
    CHECK(reply.message.native.find("content")->as_array()[0] ==
          parse(R"({"type":"thinking","thinking":"","signature":"sig-2"})"));
}

TEST_CASE("the anthropic adapter rebuilds a streamed message block by block") {
    const std::string stream = sse({
        {"message_start",
         R"({"type":"message_start","message":{"model":"test-model","usage":{"input_tokens":7,"output_tokens":1}}})"},
        {"content_block_start",
         R"({"type":"content_block_start","index":0,"content_block":{"type":"thinking","thinking":""}})"},
        {"content_block_delta",
         R"({"type":"content_block_delta","index":0,"delta":{"type":"thinking_delta","thinking":"Hmm."}})"},
        {"content_block_delta",
         R"({"type":"content_block_delta","index":0,"delta":{"type":"signature_delta","signature":"sig"}})"},
        {"content_block_stop", R"({"type":"content_block_stop","index":0})"},
        {"ping", R"({"type":"ping"})"},
        {"content_block_start",
         R"({"type":"content_block_start","index":1,"content_block":{"type":"text","text":""}})"},
        {"content_block_delta",
         R"({"type":"content_block_delta","index":1,"delta":{"type":"text_delta","text":"One "}})"},
        {"content_block_delta",
         R"({"type":"content_block_delta","index":1,"delta":{"type":"text_delta","text":"moment."}})"},
        {"content_block_stop", R"({"type":"content_block_stop","index":1})"},
        {"content_block_start",
         R"({"type":"content_block_start","index":2,"content_block":{"type":"tool_use","id":"toolu_1","name":"item_find","input":{}}})"},
        {"content_block_delta",
         R"({"type":"content_block_delta","index":2,"delta":{"type":"input_json_delta","partial_json":"{\"name\":"}})"},
        {"content_block_delta",
         R"({"type":"content_block_delta","index":2,"delta":{"type":"input_json_delta","partial_json":"\"bow\"}"}})"},
        {"content_block_stop", R"({"type":"content_block_stop","index":2})"},
        {"message_delta",
         R"({"type":"message_delta","delta":{"stop_reason":"tool_use"},"usage":{"output_tokens":15}})"},
        {"message_stop", R"({"type":"message_stop"})"},
    });
    auto transport =
        std::make_shared<FakeTransport>(std::vector<FakeTransport::Reply>{{200, stream}});
    std::string streamed;
    auto response =
        adapter("anthropic", transport)->complete(ask("Hi"), [&streamed](std::string_view text) {
            streamed += text;
        });
    CHECK(text_of(response) == "One moment.");
    CHECK(streamed == "One moment.");
    const ai::Response& reply = response.value();
    CHECK(reply.stop == ai::StopReason::ToolCalls);
    CHECK(reply.usage.input_tokens == 7);
    CHECK(reply.usage.output_tokens == 15);
    CHECK(reply.message.native == parse(R"({"type":"anthropic","content":[
        {"type":"thinking","thinking":"Hmm.","signature":"sig"},
        {"type":"text","text":"One moment."},
        {"type":"tool_use","id":"toolu_1","name":"item_find","input":{"name":"bow"}}]})"));

    auto failing = std::make_shared<FakeTransport>(std::vector<FakeTransport::Reply>{
        {200,
         sse({{"error",
               R"({"type":"error","error":{"type":"overloaded_error","message":"Overloaded"}})"}})}});
    auto failed = adapter("anthropic", failing)->complete(ask("Hi"), [](std::string_view) {});
    REQUIRE_FALSE(failed.ok());
    CHECK(failed.error().message == "the anthropic provider's stream failed: Overloaded");
}

TEST_CASE("the gemini adapter sends contents and returns thought signatures") {
    const json::Value native = parse(R"({"type":"gemini","parts":[
        {"text":"Let me look.","thoughtSignature":"sig-1"},
        {"functionCall":{"id":"call_1","name":"item_find","args":{"name":"sword"}}}]})");
    ai::Request request = tool_conversation(native);
    // A call made without an ID, as Gemini makes them, is answered without one.
    ai::Message assistant = message(ai::Role::Assistant, "");
    assistant.tool_calls.push_back(
        {.id = "call_0", .name = "item_count", .arguments = json::Object{}});
    request.messages.push_back(std::move(assistant));
    ai::Message result = message(ai::Role::Tool, "3");
    result.tool_call_id = "call_0";
    request.messages.push_back(std::move(result));
    request.output_schema = parse(R"({"type":"object"})");
    auto transport = std::make_shared<FakeTransport>(std::vector<FakeTransport::Reply>{{200, R"({
        "modelVersion": "test-model-001",
        "usageMetadata": {"promptTokenCount": 50, "candidatesTokenCount": 8, "thoughtsTokenCount": 30},
        "candidates": [{"finishReason": "STOP", "content": {"role": "model", "parts": [
          {"text": "Thinking it over.", "thought": true},
          {"text": "Here.", "thoughtSignature": "sig-2"},
          {"functionCall": {"name": "item_find", "args": {"name": "axe"}}}]}}]})"}});
    ai::AdapterOptions options;
    options.api_key = "secret";
    auto response = adapter("gemini", transport, options)->complete(request);
    CHECK(text_of(response) == "Here.");

    CHECK(transport->requests[0].url ==
          "https://generativelanguage.googleapis.com/v1beta/models/test-model:generateContent");
    CHECK(transport->header("x-goog-api-key") == "secret");
    CHECK(transport->body() == parse(R"({
        "systemInstruction": {"parts": [{"text": "You are a shopkeeper."}]},
        "contents": [
          {"role": "user", "parts": [{"inlineData": {"mimeType": "image/png", "data": "UE5H"}}, {"text": "What is this?"}]},
          {"role": "model", "parts": [
            {"text": "Let me look.", "thoughtSignature": "sig-1"},
            {"functionCall": {"id": "call_1", "name": "item_find", "args": {"name": "sword"}}}]},
          {"role": "user", "parts": [{"functionResponse": {"id": "call_1", "name": "item_find", "response": {"price": 12}}}]},
          {"role": "model", "parts": [{"functionCall": {"name": "item_count", "args": {}}}]},
          {"role": "user", "parts": [{"functionResponse": {"name": "item_count", "response": {"output": 3}}}]}],
        "tools": [{"functionDeclarations": [{"name": "item_find", "description": "Finds an item.", "parametersJsonSchema": {"type": "object"}}]}],
        "generationConfig": {"temperature": 0.5, "stopSequences": ["END"],
          "responseMimeType": "application/json", "responseJsonSchema": {"type": "object"}}})"));

    const ai::Response& reply = response.value();
    CHECK(reply.stop == ai::StopReason::ToolCalls);
    CHECK(reply.model == "test-model-001");
    CHECK(reply.usage.input_tokens == 50);
    CHECK(reply.usage.output_tokens == 38);
    REQUIRE(reply.message.tool_calls.size() == 1);
    CHECK(reply.message.tool_calls[0].id == "call_0");
    CHECK(reply.message.native.find("parts")->size() == 3);

    auto blocked = std::make_shared<FakeTransport>(std::vector<FakeTransport::Reply>{
        {200, R"({"promptFeedback": {"blockReason": "SAFETY"}})"}});
    CHECK(adapter("gemini", blocked)->complete(ask("Hi")).value().stop == ai::StopReason::Refused);
}

TEST_CASE("the gemini adapter streams parts and joins their text") {
    const std::string stream = sse({
        {"",
         R"({"candidates":[{"content":{"role":"model","parts":[{"text":"Good "}]}}],"modelVersion":"m"})"},
        {"", R"({"candidates":[{"content":{"role":"model","parts":[{"text":"morning."}]}}]})"},
        {"",
         R"({"candidates":[{"content":{"role":"model","parts":[{"functionCall":{"name":"door_open","args":{}},"thoughtSignature":"s"}]},"finishReason":"STOP"}],"usageMetadata":{"promptTokenCount":4,"candidatesTokenCount":6}})"},
    });
    auto transport =
        std::make_shared<FakeTransport>(std::vector<FakeTransport::Reply>{{200, stream}});
    std::string streamed;
    auto response =
        adapter("gemini", transport)->complete(ask("Hi"), [&streamed](std::string_view text) {
            streamed += text;
        });
    CHECK(text_of(response) == "Good morning.");
    CHECK(streamed == "Good morning.");
    CHECK(contains(transport->requests[0].url, "/models/test-model:streamGenerateContent?alt=sse"));
    const ai::Response& reply = response.value();
    CHECK(reply.stop == ai::StopReason::ToolCalls);
    CHECK(reply.usage.output_tokens == 6);
    CHECK(reply.message.native == parse(R"({"type":"gemini","parts":[
        {"text":"Good morning."},
        {"functionCall":{"name":"door_open","args":{}},"thoughtSignature":"s"}]})"));
}

TEST_CASE("adapters retry busy servers and report what the provider said") {
    const std::string busy = R"({"error":{"message":"Slow down."}})";
    auto transport = std::make_shared<FakeTransport>(std::vector<FakeTransport::Reply>{
        {429, busy, "0"}, {200, json::write(chat_reply("Done."), json::Style::Compact)}});
    CHECK(text_of(adapter("openai", transport)->complete(ask("Hi"))) == "Done.");
    CHECK(transport->requests.size() == 2);

    ai::AdapterOptions no_retries;
    no_retries.max_retries = 0;
    auto limited = std::make_shared<FakeTransport>(std::vector<FakeTransport::Reply>{{429, busy}});
    auto failed = adapter("openai", limited, no_retries)->complete(ask("Hi"));
    REQUIRE_FALSE(failed.ok());
    CHECK(failed.error().code == ErrorCode::PlatformFailure);
    CHECK(failed.error().message == "the openai provider answered HTTP 429: Slow down.");

    auto refused = std::make_shared<FakeTransport>(std::vector<FakeTransport::Reply>{
        {400,
         R"({"type":"error","error":{"type":"invalid_request_error","message":"max_tokens is too large"}})"}});
    auto invalid = adapter("anthropic", refused)->complete(ask("Hi"));
    CHECK(invalid.error().code == ErrorCode::InvalidArgument);
    CHECK(invalid.error().message ==
          "the anthropic provider answered HTTP 400: max_tokens is too large");

    auto denied = std::make_shared<FakeTransport>(std::vector<FakeTransport::Reply>{
        {403,
         R"({"error":{"code":403,"message":"API key not valid.","status":"PERMISSION_DENIED"}})"}});
    CHECK(adapter("gemini", denied)->complete(ask("Hi")).error().code ==
          ErrorCode::PermissionDenied);

    CHECK(ai::make_adapter("open-ai", {}).error().message ==
          "unknown provider type \"open-ai\"; did you mean \"openai\"? (anthropic, gemini or "
          "openai)");
}

TEST_CASE("a model without schema output gets the schema in its prompt") {
    auto transport = std::make_shared<FakeTransport>(std::vector<FakeTransport::Reply>{
        {200, json::write(chat_reply("```json\n{\"mood\": \"happy\"}\n```"), json::Style::Compact)},
        {200, json::write(chat_reply("{\"mood\": 3}"), json::Style::Compact)}});
    ai::AdapterOptions options;
    options.capabilities.json_schema_output = false;
    auto provider = adapter("openai", transport, options);
    ai::Request request = ask("How does the innkeeper feel?");
    request.output_schema = parse(R"({"type":"object","properties":{"mood":{"type":"string"}}})");
    auto response = provider->complete(request);
    REQUIRE(response.ok());
    CHECK(response.value().output == parse(R"({"mood":"happy"})"));
    CHECK(transport->body().find("response_format") == nullptr);
    CHECK(transport->body().find("messages")->as_array()[0].find("role")->as_string() == "system");
    CHECK(provider->complete(request).error().message ==
          "the model's reply does not match output_schema: expected string, got integer at "
          "/mood");
}

TEST_CASE("the HTTP transport talks to a local server") {
    FakeModelServer server(
        {chat_reply("Welcome, traveler."),
         parse(R"({"status":500,"body":{"error":{"message":"model crashed"}}})")});
    ai::AdapterOptions options;
    options.url = server.url();
    options.model = "local-model";
    options.max_retries = 0;
    auto provider = ai::make_adapter("openai", options).value();
    std::string streamed;
    auto first =
        provider->complete(ask("Hello"), [&streamed](std::string_view text) { streamed += text; });
    CHECK(text_of(first) == "Welcome, traveler.");
    CHECK(streamed == "Welcome, traveler.");
    CHECK(first.value().usage.input_tokens == 12);
    CHECK(server.bodies()[0].find("model")->as_string() == "local-model");

    auto crashed = provider->complete(ask("Hello"));
    REQUIRE_FALSE(crashed.ok());
    CHECK(crashed.error().message == "the openai provider answered HTTP 500: model crashed");
}

TEST_CASE("https trusts the system's certificates, or the ones given") {
    const std::string folder = "ai_tls_test";
    std::filesystem::remove_all(folder);
    const Certificate certificate = make_certificate(folder);
    FakeModelServer server({chat_reply("Welcome, traveler.")}, &certificate);
    const auto none = [](const std::string& /*name*/) { return std::string(); };
    const auto complete = [](const std::string& url, ai::HttpOptions http, bool stream = false) {
        ai::AdapterOptions options;
        options.url = url;
        options.model = "local-model";
        options.max_retries = 0;
        options.transport = ai::make_http_transport(std::move(http));
        std::string streamed;
        ai::StreamSink sink;
        if (stream) {
            sink = [&streamed](std::string_view text) { streamed += text; };
        }
        auto reply = ai::make_adapter("openai", options).value()->complete(ask("Hello"), sink);
        if (stream && reply.ok()) {
            CHECK(streamed == reply.value().message.text);
        }
        return reply;
    };

    const std::string url = server.url("localhost");
    CHECK(text_of(complete(url, {.ca_file = certificate.cert_file, .environment = none})) ==
          "Welcome, traveler.");
    const auto cert_file = [&certificate](const std::string& name) {
        return name == "SSL_CERT_FILE" ? certificate.cert_file : std::string();
    };
    CHECK(text_of(complete(url, {.environment = cert_file}, true)) == "Welcome, traveler.");

    // The system trusts no certificate a test made.
    auto untrusted = complete(url, {.environment = none});
    REQUIRE_FALSE(untrusted.ok());
    CHECK(untrusted.error().message ==
          "can't reach " + url +
              "/chat/completions: its certificate is not trusted; if a proxy or private server "
              "signs it, set SSL_CERT_FILE to a PEM file of the certificates to trust");
    // Issued to localhost, reached as 127.0.0.1.
    auto other_host =
        complete(server.url(), {.ca_file = certificate.cert_file, .environment = none});
    REQUIRE_FALSE(other_host.ok());
    CHECK(other_host.error().message ==
          "can't reach " + server.url() + "/chat/completions: its certificate is for another host");
    auto missing = complete(url, {.ca_file = folder + "/missing.crt", .environment = none});
    REQUIRE_FALSE(missing.ok());
    CHECK(missing.error().message == "can't reach " + url +
                                         "/chat/completions: the certificates to trust could not "
                                         "be loaded");
    CHECK(server.bodies().size() == 2);
}

TEST_CASE("requests go through the proxy the options or the environment name") {
    const auto url = [](std::string_view text) { return ai::parse_url(text).value(); };
    using Variables = std::map<std::string, std::string, std::less<>>;
    const auto environment = [](Variables variables) {
        return [variables = std::move(variables)](const std::string& name) {
            const auto found = variables.find(name);
            return found == variables.end() ? std::string() : found->second;
        };
    };
    ai::HttpOptions options;
    options.environment = environment({{"https_proxy", "http://proxy.corp:3128"},
                                       {"http_proxy", "http://plain.corp:8080"},
                                       {"no_proxy", "internal.example, .lan,10.0.0.5:8000"}});
    CHECK(ai::proxy_for(url("https://api.example.com/v1"), options) == "http://proxy.corp:3128");
    CHECK(ai::proxy_for(url("http://models.example.com/v1"), options) == "http://plain.corp:8080");
    CHECK(ai::proxy_for(url("https://internal.example/v1"), options).empty());
    CHECK(ai::proxy_for(url("https://api.Internal.example/v1"), options).empty());
    CHECK(ai::proxy_for(url("http://box.lan:11434/v1"), options).empty());
    CHECK(ai::proxy_for(url("http://10.0.0.5:9000/v1"), options).empty());
    CHECK(ai::proxy_for(url("http://notinternal.example/v1"), options) == "http://plain.corp:8080");
    // This machine is never reached through a proxy.
    CHECK(ai::proxy_for(url("http://localhost:11434/v1"), options).empty());
    CHECK(ai::proxy_for(url("http://127.0.0.1:11434/v1"), options).empty());
    CHECK(ai::proxy_for(url("http://[::1]:11434/v1"), options).empty());

    // As curl reads them: HTTPS_PROXY and NO_PROXY too, but never HTTP_PROXY, which a web
    // server's CGI scripts get from a request's Proxy header.
    options.environment =
        environment({{"HTTPS_PROXY", "http://upper:3128"}, {"HTTP_PROXY", "http://header:80"}});
    CHECK(ai::proxy_for(url("https://api.example.com/v1"), options) == "http://upper:3128");
    CHECK(ai::proxy_for(url("http://api.example.com/v1"), options).empty());
    options.environment = environment({{"https_proxy", "http://proxy:3128"}, {"NO_PROXY", "*"}});
    CHECK(ai::proxy_for(url("https://api.example.com/v1"), options).empty());
    // Networks, and domains written as wildcards.
    options.environment =
        environment({{"http_proxy", "http://proxy:3128"},
                     {"no_proxy", "10.0.0.0/8,192.168.1.0/24,*.svc.cluster.local,0.0.0.0/0x,::"}});
    CHECK(ai::proxy_for(url("http://10.200.3.4:11434/v1"), options).empty());
    CHECK(ai::proxy_for(url("http://192.168.1.20:11434/v1"), options).empty());
    CHECK(ai::proxy_for(url("http://192.168.2.20:11434/v1"), options) == "http://proxy:3128");
    CHECK(ai::proxy_for(url("http://ollama.models.svc.cluster.local/v1"), options).empty());
    CHECK(ai::proxy_for(url("http://11.0.0.1/v1"), options) == "http://proxy:3128");
    // A proxy in the options is used for every other host.
    options.proxy = "http://set:8080";
    CHECK(ai::proxy_for(url("https://api.example.com/v1"), options) == "http://set:8080");
    CHECK(ai::proxy_for(url("http://localhost/v1"), options).empty());

    // A stand-in proxy that answers every request itself, as if from the model.
    httplib::Server proxy;
    std::vector<std::pair<std::string, std::string>> seen;
    std::mutex mutex;
    proxy.Post(".*", [&](const httplib::Request& request, httplib::Response& response) {
        const std::scoped_lock lock(mutex);
        seen.emplace_back(request.target, request.get_header_value("Proxy-Authorization"));
        response.set_content(json::write(chat_reply("Through the proxy."), json::Style::Compact),
                             "application/json");
    });
    const int port = proxy.bind_to_any_port("127.0.0.1");
    std::thread thread([&proxy] { proxy.listen_after_bind(); });
    proxy.wait_until_ready();

    ai::AdapterOptions adapter_options;
    adapter_options.url = "http://models.example:11434/v1";
    adapter_options.model = "local-model";
    adapter_options.max_retries = 0;
    adapter_options.transport =
        ai::make_http_transport({.proxy = "http://agent:s3cret@127.0.0.1:" + std::to_string(port),
                                 .environment = environment({})});
    auto reply = ai::make_adapter("openai", adapter_options).value()->complete(ask("Hello"));
    // https:// goes through a tunnel, which the stand-in doesn't open.
    adapter_options.url = "https://models.example/v1";
    auto tunnel = ai::make_adapter("openai", adapter_options).value()->complete(ask("Hello"));
    adapter_options.transport = ai::make_http_transport({.proxy = "socks5://127.0.0.1:1080"});
    auto refused = ai::make_adapter("openai", adapter_options).value()->complete(ask("Hello"));
    proxy.stop();
    thread.join();

    CHECK(text_of(reply) == "Through the proxy.");
    REQUIRE(seen.size() == 1);
    CHECK(seen[0].first == "http://models.example:11434/v1/chat/completions");
    CHECK(seen[0].second == "Basic YWdlbnQ6czNjcmV0");
    REQUIRE_FALSE(tunnel.ok());
    CHECK(tunnel.error().message ==
          "can't reach https://models.example/v1/chat/completions: the proxy answered HTTP 400");
    REQUIRE_FALSE(refused.ok());
    CHECK(refused.error().message ==
          "the proxy \"socks5://127.0.0.1:1080\" is not an http://host:port URL");
}

TEST_CASE("replay records each reply once and answers offline after that") {
    const std::string folder = "ai_replay_test";
    std::filesystem::remove_all(folder);
    const std::string path = folder + "/tests/ai.replay.jsonl";
    auto transport = std::make_shared<FakeTransport>(std::vector<FakeTransport::Reply>{
        {200, json::write(chat_reply("Fresh bread!"), json::Style::Compact)}});
    auto model = adapter("openai", transport);
    ai::ReplayProvider recorder({.path = path, .model = "openai/test-model", .inner = model});

    std::string streamed;
    const auto sink = [&streamed](std::string_view text) { streamed += text; };
    auto first = recorder.complete(ask("What do you sell?"), sink);
    CHECK(text_of(first) == "Fresh bread!");
    CHECK_FALSE(first.value().replayed);
    CHECK(transport->requests.size() == 1);
    const std::string file = read(path);
    CHECK(file.starts_with("{\"chunks\":[\"Fresh bread!\"],\"key\":\"" +
                           ai::ReplayProvider::key("openai/test-model", ask("What do you sell?")) +
                           "\""));

    streamed.clear();
    auto again = recorder.complete(ask("What do you sell?"), sink);
    CHECK(text_of(again) == "Fresh bread!");
    CHECK(again.value().replayed);
    CHECK(streamed == "Fresh bread!");
    CHECK(transport->requests.size() == 1);

    // Replaying needs no model at all.
    ai::ReplayProvider offline(
        {.path = path, .model = "openai/test-model", .mode = ai::ReplayMode::Replay});
    CHECK(text_of(offline.complete(ask("What do you sell?"))) == "Fresh bread!");
    auto missing = offline.complete(ask("Any cheese?"));
    REQUIRE_FALSE(missing.ok());
    CHECK(missing.error().code == ErrorCode::NotFound);
    CHECK(contains(missing.error().message, path + " has no recording of this request (key "));
    CHECK(ai::ReplayProvider::key("openai/other-model", ask("What do you sell?")) !=
          ai::ReplayProvider::key("openai/test-model", ask("What do you sell?")));

    // Record calls the model again and replaces the recording; write false records nothing.
    ai::ReplayProvider rerecord({.path = path,
                                 .model = "openai/test-model",
                                 .mode = ai::ReplayMode::Record,
                                 .inner = model});
    CHECK_FALSE(rerecord.complete(ask("What do you sell?")).value().replayed);
    CHECK(transport->requests.size() == 2);
    CHECK(read(path).starts_with("{\"key\":"));
    ai::ReplayProvider dry(
        {.path = path, .model = "openai/test-model", .inner = model, .write = false});
    CHECK(dry.complete(ask("Any cheese?")).ok());
    CHECK(!contains(read(path), "Any cheese?"));
    std::filesystem::remove_all(folder);
}

TEST_CASE("providers.json names providers and roles and is checked") {
    const json::Value document = parse(R"({
        "providers": {
          "local": {"type": "openai", "model": "small", "url": "http://localhost:11434/v1"},
          "hosted": {"type": "anthropic", "model": "large", "api_key_env": "SLASH_TEST_KEY",
                     "capabilities": {"vision": false}, "max_tokens": 2000}},
        "roles": {
          "default": "local",
          "npc": {"provider": "local", "model": "tiny", "system": "You are a guard.", "temperature": 0.2}},
        "replay": {"file": "tests/ai.replay.jsonl", "mode": "replay"}})");
    CHECK(ai::check_config(document).empty());
    const ai::Config config = ai::parse_config(document).value();
    CHECK(config.providers.at("hosted").capabilities.vision == false);
    CHECK(config.providers.at("hosted").max_tokens == 2000);
    CHECK(config.roles.at("default").provider == "local");
    CHECK(config.roles.at("npc").model == "tiny");
    CHECK(config.roles.at("npc").params.temperature == 0.2);
    CHECK(config.replay->mode == ai::ReplayMode::Replay);

    const auto first_error = [](std::string_view text) {
        const std::vector<json::ValidationError> errors = ai::check_config(parse(text));
        REQUIRE_FALSE(errors.empty());
        return errors.front().path + ": " + errors.front().message;
    };
    CHECK(first_error(R"({"providers": {"local": {"type": "openai", "model": "m"}},
                          "roles": {"npc": "locale"}})") ==
          "/roles/npc: no provider named \"locale\"; did you mean \"local\"?");
    CHECK(contains(first_error(R"({"providers": {"x": {"type": "ollama", "model": "m"}}})"),
                   "/providers/x/type"));
    CHECK(first_error(R"({"replay": {"file": "../replays.jsonl"}})") ==
          "/replay/file: \"../replays.jsonl\" is not a path inside the project, such as "
          "tests/ai.replay.jsonl");
    CHECK(ai::parse_config(parse(R"({"providers": {"x": {"type": "openai"}}})"))
              .error()
              .message.starts_with("providers.json /providers/x"));
}

TEST_CASE("roles open their provider with their instructions, keys and replay file") {
    const ai::Config config = ai::parse_config(parse(R"({
        "providers": {
          "local": {"type": "openai", "model": "small", "url": "http://localhost:1/v1"},
          "hosted": {"type": "anthropic", "model": "large", "api_key_env": "SLASH_TEST_KEY"}},
        "roles": {
          "default": "local",
          "npc": {"provider": "local", "model": "tiny", "system": "You are a guard.", "temperature": 0.2}}})"))
                                  .value();
    auto transport = std::make_shared<FakeTransport>(std::vector<FakeTransport::Reply>{
        {200, json::write(chat_reply("Halt!"), json::Style::Compact)},
        {200, R"({"type":"message","role":"assistant","content":[{"type":"text","text":"Hello."}],
                  "stop_reason":"end_turn","usage":{"input_tokens":1,"output_tokens":1}})"}});
    std::map<std::string, std::string> environment;
    ai::OpenOptions options;
    options.transport = transport;
    options.environment = [&environment](const std::string& name) {
        const auto found = environment.find(name);
        return found != environment.end() ? found->second : std::string();
    };

    const ai::Opened npc = ai::open(config, "npc", options).value();
    CHECK(npc.provider_name == "local");
    CHECK(npc.model == "tiny");
    CHECK(text_of(npc.provider->complete(ask("Who goes there?"))) == "Halt!");
    CHECK(transport->body() == parse(R"({"model": "tiny", "temperature": 0.2, "messages": [
        {"role": "system", "content": "You are a guard."},
        {"role": "user", "content": "Who goes there?"}]})"));
    CHECK(ai::open(config, "", options).value().provider_name == "local");
    CHECK(ai::open(config, "local", options).value().model == "small");
    CHECK(ai::open(config, "npcs", options).error().message ==
          "no role or provider named \"npcs\"; did you mean \"npc\"?");

    // A missing key fails only when the model is called.
    const ai::Opened hosted = ai::open(config, "hosted", options).value();
    auto keyless = hosted.provider->complete(ask("Hi"));
    REQUIRE_FALSE(keyless.ok());
    CHECK(keyless.error().code == ErrorCode::PermissionDenied);
    CHECK(keyless.error().message ==
          "the provider \"hosted\" reads its key from SLASH_TEST_KEY, which is not set");
    environment["SLASH_TEST_KEY"] = "secret";
    CHECK(ai::open(config, "hosted", options).value().provider->complete(ask("Hi")).ok());
    CHECK(transport->header("x-api-key", transport->requests.size() - 1) == "secret");

    // SLASH_AI_REPLAY insists on replaying, which needs a replay file.
    environment["SLASH_AI_REPLAY"] = "replay";
    CHECK(contains(
        ai::open(config, "npc", options).value().provider->complete(ask("Hi")).error().message,
        "providers.json names no replay file"));
    environment["SLASH_AI_REPLAY"] = "sometimes";
    CHECK(ai::open(config, "npc", options).error().message ==
          "SLASH_AI_REPLAY is \"sometimes\"; use replay, record, auto or off");

    CHECK(ai::open(ai::Config{}, "", options).error().code == ErrorCode::NotFound);
}

TEST_CASE("a call runs on its own thread and stops when destroyed") {
    // Streams "Hi" and "!" one piece at a time, then waits to be cancelled if asked to hang, or
    // for two seconds, cancelled or not, if stubborn.
    class Slow final : public ai::Provider {
    public:
        explicit Slow(bool hang, bool stubborn = false) : hang_(hang), stubborn_(stubborn) {}
        const ai::Capabilities& capabilities() const override { return capabilities_; }
        Result<ai::Response> complete(const ai::Request& /*request*/, const ai::StreamSink& sink,
                                      const ai::CancelFlag* cancel) override {
            if (sink) {
                sink("Hi");
                sink("!");
            }
            if (stubborn_) {
                std::this_thread::sleep_for(std::chrono::seconds(2));
            }
            while (hang_ && !cancel->load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (hang_) {
                return make_error(ErrorCode::PlatformFailure, "the request was cancelled");
            }
            ai::Response response;
            response.message.text = "Hi!";
            return response;
        }

    private:
        ai::Capabilities capabilities_;
        bool hang_;
        bool stubborn_;
    };

    ai::Call call(std::make_shared<Slow>(false), ask("Hello"), true);
    call.wait();
    REQUIRE(call.done());
    REQUIRE(call.result() != nullptr);
    CHECK(call.result()->ok());
    CHECK(call.text() == "Hi!");

    const auto start = std::chrono::steady_clock::now();
    {
        ai::Call hung(std::make_shared<Slow>(true), ask("Hello"), true);
        while (hung.text() != "Hi!") {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK_FALSE(hung.done());
        CHECK(hung.result() == nullptr);
    }
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(5));

    // A request that doesn't notice it was cancelled ends on its own, after its Call has gone.
    const auto stubborn_start = std::chrono::steady_clock::now();
    { const ai::Call stubborn(std::make_shared<Slow>(false, true), ask("Hello"), true); }
    CHECK(std::chrono::steady_clock::now() - stubborn_start < std::chrono::seconds(1));
}

TEST_CASE("gameplay asks a role's model and hears back through world events") {
    const std::string root = "ai_world_project";
    std::filesystem::remove_all(root);
    json::Value refused;
    refused["status"] = 400;
    refused["body"] = parse(R"({"error":{"message":"The prompt is too long."}})");
    FakeModelServer server({chat_reply("Fresh bread, two coins."), chat_reply("Cake on Sundays."),
                            refused, chat_reply("Once upon a time.")});
    const std::string provider = R"("local": {"type": "openai", "model": "local-model", "url": ")" +
                                 server.url() + R"(", "max_retries": 0})";
    write(root + "/providers.json", R"({"providers": {)" + provider + R"(},
        "roles": {"dialogue": {"provider": "local", "system": "You are Mira, a baker."}}})");

    world::ComponentRegistry components;
    world::World world(components);
    std::vector<world::Event> heard;
    world.set_event_listener([&heard](const world::Event& event) {
        if (event.kind.starts_with("ai_")) {
            heard.push_back(event);
        }
    });
    constexpr double kTick = 1.0 / 60.0;
    CHECK(ai::ask(world, "dialogue", "Hello?").error().message ==
          "this game calls no language models: it has no providers.json");

    ai::install(world, {.open = {.project_root = root, .write_replays = false}, .wait = true});
    REQUIRE(ai::installed(world));
    const std::string asked = ai::ask(world, "dialogue", "Do you sell bread?").value();
    CHECK(asked == "ai_1");
    CHECK(ai::pending(world, asked));
    // Waiting, the reply comes at the next tick on every run, and gameplay sees it the tick after.
    world.step(kTick);
    REQUIRE(heard.size() == 1);
    CHECK(heard[0].kind == "ai_reply");
    CHECK(heard[0].id == "ai_1");
    CHECK(heard[0].data.find("role")->as_string() == "dialogue");
    CHECK(heard[0].data.find("text")->as_string() == "Fresh bread, two coins.");
    CHECK(heard[0].data.find("stop")->as_string() == "end");
    CHECK(heard[0].data.find("usage")->find("output_tokens")->as_int() == 3);
    CHECK_FALSE(ai::pending(world, asked));
    CHECK(server.bodies()[0].find("stream")->as_bool());
    CHECK(server.bodies()[0].find("messages")->as_array()[0] ==
          parse(R"({"role": "system", "content": "You are Mira, a baker."})"));
    world.step(kTick);
    const auto seen = std::ranges::find(world.events(), "ai_reply", &world::Event::kind);
    REQUIRE(seen != world.events().end());
    CHECK(seen->id == "ai_1");

    // A reply's message goes back into the next request of the conversation.
    ai::Request more;
    more.messages.push_back(message(ai::Role::User, "Do you sell bread?"));
    more.messages.push_back(ai::message_from_json(*heard[0].data.find("message")).value());
    more.messages.push_back(message(ai::Role::User, "And cake?"));
    CHECK(ai::ask(world, "dialogue", std::move(more)).value() == "ai_2");
    world.step(kTick);
    REQUIRE(heard.size() == 2);
    CHECK(heard[1].data.find("text")->as_string() == "Cake on Sundays.");
    CHECK(server.bodies()[1].find("messages")->as_array()[2] ==
          parse(R"({"role": "assistant", "content": "Fresh bread, two coins."})"));

    // A failed request says why; an unknown role fails right away.
    CHECK(ai::ask(world, "dialogue", "Tell me everything.").value() == "ai_3");
    world.step(kTick);
    REQUIRE(heard.size() == 3);
    CHECK(heard[2].kind == "ai_failed");
    CHECK(heard[2].data == parse(R"({"role": "dialogue", "code": "invalid_argument",
        "message": "the openai provider answered HTTP 400: The prompt is too long."})"));
    CHECK(ai::ask(world, "dialog", "Hi").error().message ==
          "no role or provider named \"dialog\"; did you mean \"dialogue\"?");

    // providers.json is read again when it changes.
    write(root + "/providers.json", R"({"providers": {)" + provider + R"(},
        "roles": {"narrator": {"provider": "local"}}})");
    CHECK(ai::ask(world, "narrator", "Begin.").value() == "ai_4");
    world.step(kTick);
    REQUIRE(heard.size() == 4);
    CHECK(heard[3].data.find("text")->as_string() == "Once upon a time.");
    CHECK(heard[3].data.find("role")->as_string() == "narrator");

    // A cancelled request sends nothing; a rolled back one gives its ID out again.
    CHECK(ai::ask(world, "narrator", "Again.").value() == "ai_5");
    ai::cancel(world, "ai_5");
    CHECK_FALSE(ai::pending(world, "ai_5"));
    const ai::Snapshot before = ai::save(world);
    CHECK(ai::ask(world, "narrator", "Again.").value() == "ai_6");
    ai::restore(world, before);
    CHECK_FALSE(ai::pending(world, "ai_6"));
    world.step(kTick);
    CHECK(heard.size() == 4);
    CHECK(ai::ask(world, "narrator", "Again.").value() == "ai_6");

    // Not waiting, replies come at whichever tick they finish by, and stream until then.
    ai::set_wait(world, false);
    const auto start = std::chrono::steady_clock::now();
    while (heard.size() == 4 &&
           std::chrono::steady_clock::now() - start < std::chrono::seconds(10)) {
        world.step(kTick);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(heard.size() == 5);
    CHECK(heard[4].id == "ai_6");
    CHECK(ai::reply_text(world, "ai_6").empty());

    // A world played again from the start asks from the first ID again.
    world.reset_state();
    CHECK(ai::ask(world, "narrator", "Begin.").value() == "ai_1");
    ai::cancel(world, "ai_1");
    std::filesystem::remove_all(root);
}

TEST_CASE("sessions without the network capability answer only from the replay file") {
    const std::string root = "ai_session_project";
    std::filesystem::remove_all(root);
    FakeModelServer server({chat_reply("Halt!")});
    write(root + "/providers.json", R"({
        "providers": {"local": {"type": "openai", "model": "local-model", "url": ")" +
                                        server.url() + R"("}},
        "roles": {"default": "local"},
        "replay": {"file": "tests/ai.replay.jsonl"}})");
    world::ComponentRegistry components;
    world::register_builtin_components(components);
    command::Registry commands;
    command::register_builtin_commands(commands);
    const auto heard = [](command::Session& session, std::string prompt) {
        std::vector<world::Event> events;
        session.world().set_event_listener([&events](const world::Event& event) {
            if (event.kind.starts_with("ai_")) {
                events.push_back(event);
            }
        });
        REQUIRE(ai::ask(session.world(), "", std::move(prompt)).ok());
        session.world().step(1.0 / 60.0);
        session.world().set_event_listener({});
        REQUIRE(events.size() == 1);
        return events[0];
    };

    // A session that may reach the network calls the model and records its reply.
    command::SessionOptions online;
    online.project_root = root;
    online.capture_logs = false;
    command::Session recorder(components, commands, std::move(online));
    REQUIRE(ai::installed(recorder.world()));
    CHECK(heard(recorder, "Who goes there?").data.find("text")->as_string() == "Halt!");
    CHECK(server.bodies().size() == 1);

    // One that may not answers the same request from the recording, and refuses others.
    command::SessionOptions offline;
    offline.project_root = root;
    offline.capture_logs = false;
    offline.capabilities = {command::Capability::Read, command::Capability::Edit,
                            command::Capability::Run};
    command::Session reader(components, commands, std::move(offline));
    CHECK(heard(reader, "Who goes there?").data.find("text")->as_string() == "Halt!");
    const world::Event refused = heard(reader, "Open the gate.");
    CHECK(refused.kind == "ai_failed");
    CHECK(refused.data.find("code")->as_string() == "permission_denied");
    CHECK(contains(refused.data.find("message")->as_string(), "lacks the network capability"));
    CHECK(server.bodies().size() == 1);
    std::filesystem::remove_all(root);
}

TEST_CASE("commands become tools, and tool calls run as commands") {
    Engine engine(".");
    const std::vector<std::string> names = {"entity.create", "world.query"};
    const std::vector<ai::ToolSpec> tools = ai::command_tools(engine.commands, names).value();
    REQUIRE(tools.size() == 2);
    CHECK(tools[0].name == "entity_create");
    CHECK(tools[0].parameters == engine.commands.find("entity.create")->params);
    CHECK(tools[1].description == engine.commands.find("world.query")->description);
    const std::vector<std::string> typo = {"entity.creat"};
    CHECK(ai::command_tools(engine.commands, typo).error().message ==
          "no command named \"entity.creat\"; did you mean \"entity.create\"?");

    ai::Message reply = message(ai::Role::Assistant, "");
    reply.tool_calls.push_back(
        {.id = "1", .name = "entity_create", .arguments = parse(R"({"id":"hero"})")});
    reply.tool_calls.push_back({.id = "2", .name = "entity_explode", .arguments = json::Object{}});
    reply.tool_calls.push_back(
        {.id = "3", .name = "entity_create", .arguments = json::Value("{oops")});
    const std::vector<ai::Message> results = ai::run_tool_calls(*engine.session, reply);
    REQUIRE(results.size() == 3);
    CHECK(results[0].role == ai::Role::Tool);
    CHECK(results[0].tool_call_id == "1");
    CHECK_FALSE(results[0].is_error);
    CHECK(contains(results[0].text, "hero"));
    CHECK((engine.session->world().find("hero") != entt::null));
    CHECK(results[1].is_error);
    CHECK(results[1].text == "no command named entity_explode");
    CHECK(results[2].is_error);
    CHECK(contains(results[2].text, "\"code\":-32602"));
}

TEST_CASE("ai.complete asks the project's model and ai.providers lists them") {
    const std::string root = "ai_command_project";
    std::filesystem::remove_all(root);
    json::Value tool_reply = chat_reply("");
    json::Value& choice = tool_reply["choices"].as_array()[0];
    choice["finish_reason"] = "tool_calls";
    choice["message"]["tool_calls"] = parse(
        R"([{"id":"call_1","type":"function","function":{"name":"entity_create","arguments":"{\"id\":\"guard\"}"}}])");
    FakeModelServer server(
        {tool_reply, chat_reply("{\"mood\":\"grumpy\"}"), chat_reply("A crate.")});
    write(root + "/providers.json", R"({
        "providers": {
          "local": {"type": "openai", "model": "local-model", "url": ")" +
                                        server.url() + R"("},
          "hosted": {"type": "gemini", "model": "big", "api_key_env": "SLASH_TEST_UNSET_KEY"}},
        "roles": {"default": "local"},
        "replay": {"file": "tests/ai.replay.jsonl"}})");
    write(root + "/art/crate.png", "PNG");
    Engine engine(root);

    CHECK(engine.ok("ai.providers", json::Object{}) == parse(R"({
        "providers": [
          {"name": "hosted", "type": "gemini", "model": "big",
           "url": "https://generativelanguage.googleapis.com/v1beta",
           "api_key_env": "SLASH_TEST_UNSET_KEY", "key_set": false},
          {"name": "local", "type": "openai", "model": "local-model", "url": ")" +
                                                             server.url() + R"("}],
        "roles": {"default": {"provider": "local"}},
        "replay": {"file": "tests/ai.replay.jsonl", "mode": "auto"}})"));

    const json::Value called = engine.ok("ai.complete", parse(R"({
        "prompt": "Add a guard.", "tools": ["entity.create"], "max_tokens": 50})"));
    CHECK(called.find("tool_calls")->as_array()[0] ==
          parse(R"({"id":"call_1","command":"entity.create","arguments":{"id":"guard"}})"));
    CHECK(called.find("stop")->as_string() == "tool_calls");
    CHECK(called.find("provider")->as_string() == "local");
    CHECK(called.find("message")->find("tool_calls") != nullptr);
    CHECK(called.find("replayed") == nullptr);
    const json::Value sent = server.bodies()[0];
    CHECK(sent.find("tools")->as_array()[0].find("function")->find("name")->as_string() ==
          "entity_create");
    CHECK(sent.find("max_tokens")->as_int() == 50);

    const json::Value parsed = engine.ok("ai.complete", parse(R"({
        "prompt": "How is the guard?", "output_schema": {"type": "object"}})"));
    CHECK(*parsed.find("output") == parse(R"({"mood":"grumpy"})"));

    const json::Value seen = engine.ok("ai.complete", parse(R"({
        "role": "local", "system": "Describe things.", "prompt": "What is this?", "images": ["art/crate.png"]})"));
    CHECK(seen.find("text")->as_string() == "A crate.");
    const json::Value third = server.bodies()[2];
    const json::Value& content = third.find("messages")->as_array()[1].as_object().at("content");
    CHECK(content.as_array()[0].find("image_url")->find("url")->as_string() ==
          "data:image/png;base64,UE5H");

    // The same request again comes from the replay file, with no model called.
    const json::Value replayed = engine.ok("ai.complete", parse(R"({
        "prompt": "How is the guard?", "output_schema": {"type": "object"}})"));
    CHECK(replayed.find("replayed")->as_bool());
    CHECK(server.bodies().size() == 3);
    CHECK(platform::exists(root + "/tests/ai.replay.jsonl"));

    command::Reply unknown =
        engine.call("ai.complete", parse(R"({"role": "locals", "prompt": "Hi"})"));
    CHECK(unknown.message == "no role or provider named \"locals\"; did you mean \"local\"?");
    CHECK(unknown.data == parse(R"({"path":"/role"})"));
    command::Reply keyless =
        engine.call("ai.complete", parse(R"({"role": "hosted", "prompt": "Hi"})"));
    CHECK(keyless.code == command::code::kPermissionDenied);
    CHECK(engine.call("ai.complete", parse(R"({"system": "Hi"})")).code ==
          command::code::kInvalidParams);
    command::Reply bad_image =
        engine.call("ai.complete", parse(R"({"prompt": "Hi", "images": ["art/crate.bmp"]})"));
    CHECK(bad_image.data == parse(R"({"path":"/images/0"})"));

    // Over MCP, ai.complete is marked as reaching outside the machine, and sessions without the
    // network capability can't call it.
    command::mcp::Server mcp(*engine.session);
    const json::Value listed =
        parse(mcp.handle(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})"));
    for (const json::Value& tool : listed.find("result")->find("tools")->as_array()) {
        const std::string& name = tool.find("name")->as_string();
        const json::Value& annotations = *tool.find("annotations");
        if (name == "ai_complete") {
            CHECK_FALSE(annotations.find("readOnlyHint")->as_bool());
            CHECK(annotations.find("openWorldHint")->as_bool());
        }
        if (name == "ai_providers") {
            CHECK(annotations.find("readOnlyHint")->as_bool());
            CHECK_FALSE(annotations.find("openWorldHint")->as_bool());
        }
    }
    command::SessionOptions offline;
    offline.project_root = root;
    offline.capture_logs = false;
    offline.capabilities = {command::Capability::Read};
    command::Session reader(engine.components, engine.commands, std::move(offline));
    command::Reply refused = reader.call("ai.complete", parse(R"({"prompt": "Hi"})"));
    CHECK(refused.code == command::code::kPermissionDenied);
    CHECK(refused.data == parse(R"({"capability":"network"})"));
    std::filesystem::remove_all(root);
}
