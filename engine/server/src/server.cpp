#include "slash/server/server.hpp"

#include "slash/command/command.hpp"
#include "slash/command/mcp.hpp"
#include "slash/command/rpc.hpp"
#include "slash/command/session.hpp"
#include "slash/core/log.hpp"
#include "slash/platform/file.hpp"
#include "slash/server/openapi.hpp"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <httplib.h>
#include <mutex>
#include <random>
#include <thread>
#include <utility>

namespace slash::server {
namespace {

constexpr std::size_t kMaxBodyBytes = std::size_t{8} * 1024 * 1024;

bool is_loopback(std::string_view host) {
    return host == "127.0.0.1" || host == "::1" || host == "localhost";
}

// httplib's default sets SO_REUSEPORT, which would let a second server share a port with a running
// one. Keep each port to one listener while still allowing a quick restart on POSIX.
void exclusive_port(socket_t sock) {
#ifdef _WIN32
    httplib::set_socket_opt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, 1);
#else
    httplib::set_socket_opt(sock, SOL_SOCKET, SO_REUSEADDR, 1);
#endif
}

// 128 random bits as 32 hex digits.
std::string random_token() {
    constexpr std::string_view kHex = "0123456789abcdef";
    std::random_device device;
    std::string token;
    for (int word = 0; word < 4; ++word) {
        std::uint32_t bits = device();
        for (int digit = 0; digit < 8; ++digit) {
            token += kHex[bits & 0xFU];
            bits >>= 4U;
        }
    }
    return token;
}

// Compares in time that depends only on the length, so the token can't be guessed byte by byte.
bool same_secret(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    unsigned difference = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        difference |= static_cast<unsigned>(static_cast<unsigned char>(a[i]) ^
                                            static_cast<unsigned char>(b[i]));
    }
    return difference == 0;
}

json::Value error_body(int error_code, std::string message, json::Value data = {}) {
    json::Value error;
    error["code"] = error_code;
    error["message"] = std::move(message);
    if (!data.is_null()) {
        error["data"] = std::move(data);
    }
    json::Value body;
    body["error"] = std::move(error);
    return body;
}

void send_json(httplib::Response& res, int status, const json::Value& body) {
    res.status = status;
    res.set_content(json::write(body, json::Style::Compact), "application/json");
}

json::Value commands_json(const command::Registry& registry) {
    json::Array commands;
    for (const command::Command& command : registry.commands()) {
        json::Value item;
        item["name"] = command.name;
        item["description"] = command.description;
        json::Array capabilities;
        for (const command::Capability capability : command.capabilities) {
            capabilities.emplace_back(command::to_string(capability));
        }
        item["capabilities"] = std::move(capabilities);
        item["params"] = command.params;
        commands.push_back(std::move(item));
    }
    json::Value result;
    result["commands"] = std::move(commands);
    return result;
}

} // namespace

std::string Endpoint::url() const {
    const bool ipv6 = host.find(':') != std::string::npos;
    return "http://" + (ipv6 ? "[" + host + "]" : host) + ":" + std::to_string(port);
}

json::Value Endpoint::to_json() const {
    json::Value value;
    value["host"] = host;
    value["port"] = port;
    value["token"] = token;
    value["url"] = url();
    return value;
}

Result<Endpoint> Endpoint::from_json(const json::Value& value) {
    const json::Value* host = value.find("host");
    const json::Value* port = value.find("port");
    const json::Value* token = value.find("token");
    if (host == nullptr || !host->is_string() || port == nullptr || !port->is_int() ||
        token == nullptr || !token->is_string()) {
        return make_error(ErrorCode::InvalidArgument,
                          "an endpoint needs a string host, an integer port and a string token");
    }
    return Endpoint{host->as_string(), static_cast<int>(port->as_int()), token->as_string()};
}

Result<Endpoint> read_endpoint(std::string_view project_root) {
    std::string path(project_root);
    if (!path.empty() && path.back() != '/' && path.back() != '\\') {
        path += '/';
    }
    path += kEndpointFile;
    auto text = platform::read_file(path);
    if (!text) {
        return make_error(ErrorCode::NotFound,
                          "no running engine server found (" + path +
                              " is missing); start one with \"slash-cli serve\" or "
                              "\"slash-cli run --serve\"");
    }
    auto parsed = json::parse(text.value());
    if (!parsed) {
        return make_error(ErrorCode::InvalidArgument, path + ": " + parsed.error().message);
    }
    return Endpoint::from_json(parsed.value());
}

struct Server::Impl {
    // A request waiting for the session's owner thread.
    struct Job {
        std::function<void()> run;
        bool taken = false;
        bool done = false;
    };

    Impl(command::Session& owner_session, ServerOptions server_options)
        : session(&owner_session), options(std::move(server_options)), mcp(owner_session) {}

    command::Session* session;
    ServerOptions options;
    command::mcp::Server mcp; // used only on the owner thread
    Endpoint endpoint;
    httplib::Server http;
    std::thread listener;

    mutable std::mutex mutex;
    std::condition_variable queued;
    std::condition_variable finished;
    std::deque<std::shared_ptr<Job>> jobs;
    bool stopping = false;

    // Runs work on the owner thread and waits for it. Returns false if the server is stopping
    // or the owner did not start the work within the request timeout.
    bool run_on_owner(std::function<void()> work) {
        auto job = std::make_shared<Job>();
        job->run = std::move(work);
        std::unique_lock lock(mutex);
        if (stopping) {
            return false;
        }
        jobs.push_back(job);
        queued.notify_one();
        finished.wait_for(lock, options.request_timeout, [&] { return job->done || stopping; });
        if (!job->done && job->taken) {
            // The owner is running it now, so its locals must outlive the work.
            finished.wait(lock, [&] { return job->done; });
        }
        if (!job->done) {
            std::erase(jobs, job);
        }
        return job->done;
    }

    std::size_t poll(std::chrono::milliseconds wait) {
        std::unique_lock lock(mutex);
        if (jobs.empty() && wait.count() > 0) {
            queued.wait_for(lock, wait, [&] { return !jobs.empty() || stopping; });
        }
        std::size_t count = 0;
        while (!jobs.empty()) {
            const std::shared_ptr<Job> job = jobs.front();
            jobs.pop_front();
            job->taken = true;
            lock.unlock();
            job->run();
            lock.lock();
            job->done = true;
            ++count;
            finished.notify_all();
        }
        return count;
    }

    void stop() {
        {
            const std::scoped_lock lock(mutex);
            if (stopping) {
                return;
            }
            stopping = true;
        }
        finished.notify_all();
        queued.notify_all();
        http.stop();
        if (listener.joinable()) {
            listener.join();
        }
    }

    [[nodiscard]] bool allowed_host(std::string_view value) const {
        const std::string port = ":" + std::to_string(endpoint.port);
        return std::ranges::any_of(
            std::array<std::string_view, 3>{"127.0.0.1", "localhost", "[::1]"},
            [&](std::string_view name) {
                return value == name || value == std::string(name) + port;
            });
    }

    // Checks Host, Origin and the bearer token. Fills the response and returns false if the
    // request must be refused.
    bool admit(const httplib::Request& req, httplib::Response& res) const {
        if (!allowed_host(req.get_header_value("Host"))) {
            send_json(res, 403,
                      error_body(command::code::kPermissionDenied,
                                 "the Host header must be 127.0.0.1, localhost or [::1]"));
            return false;
        }
        if (req.has_header("Origin")) {
            const std::string origin = req.get_header_value("Origin");
            constexpr std::string_view kScheme = "http://";
            if (!origin.starts_with(kScheme) || !allowed_host(origin.substr(kScheme.size()))) {
                send_json(res, 403,
                          error_body(command::code::kPermissionDenied,
                                     "requests from other web origins are not allowed"));
                return false;
            }
        }
        constexpr std::string_view kBearer = "Bearer ";
        const std::string authorization = req.get_header_value("Authorization");
        if (!authorization.starts_with(kBearer) ||
            !same_secret(std::string_view(authorization).substr(kBearer.size()), endpoint.token)) {
            res.set_header("WWW-Authenticate", "Bearer");
            send_json(res, 401,
                      error_body(command::code::kPermissionDenied,
                                 "missing or wrong bearer token; send \"Authorization: Bearer "
                                 "<token>\" with the token from " +
                                     std::string(kEndpointFile)));
            return false;
        }
        return true;
    }

    void unavailable(httplib::Response& res) const {
        bool shutting_down = false;
        {
            const std::scoped_lock lock(mutex);
            shutting_down = stopping;
        }
        send_json(res, 503,
                  error_body(command::code::kInternalError,
                             shutting_down ? "the engine is shutting down"
                                           : "the engine did not run the request in time; is "
                                             "its main loop running?"));
    }

    void install_routes() {
        http.set_payload_max_length(kMaxBodyBytes);
        http.set_read_timeout(std::chrono::seconds(10));

        http.set_pre_routing_handler([this](const httplib::Request& req, httplib::Response& res) {
            return admit(req, res) ? httplib::Server::HandlerResponse::Unhandled
                                   : httplib::Server::HandlerResponse::Handled;
        });

        http.set_error_handler([](const httplib::Request& /*req*/, httplib::Response& res) {
            if (res.body.empty() && res.status == 404) {
                send_json(res, 404,
                          error_body(command::code::kMethodNotFound,
                                     "no such endpoint; try GET /v1/commands, POST "
                                     "/v1/commands/<name>, POST /rpc or POST /mcp"));
            }
        });

        http.Get("/v1/commands", [this](const httplib::Request& /*req*/, httplib::Response& res) {
            // The registry never changes after setup, so it is safe to read here.
            send_json(res, 200, commands_json(session->commands()));
        });

        http.Get("/v1/openapi.json",
                 [this](const httplib::Request& /*req*/, httplib::Response& res) {
                     send_json(res, 200, openapi(session->commands(), endpoint.url()));
                 });

        http.Post("/v1/commands/:name", [this](const httplib::Request& req,
                                               httplib::Response& res) {
            const auto name = req.path_params.find("name");
            json::Value params = json::Object{};
            if (req.body.find_first_not_of(" \t\r\n") != std::string::npos) {
                auto parsed = json::parse(req.body);
                if (!parsed) {
                    send_json(res, 400,
                              error_body(command::code::kParseError,
                                         "the body is not valid JSON: " + parsed.error().message));
                    return;
                }
                params = std::move(parsed).value();
            }
            command::Reply reply;
            if (!run_on_owner([&] { reply = session->call(name->second, params); })) {
                unavailable(res);
                return;
            }
            if (reply.ok) {
                send_json(res, 200, reply.value);
            } else {
                send_json(res, http_status(reply.code),
                          error_body(reply.code, std::move(reply.message), std::move(reply.data)));
            }
        });

        http.Post("/rpc", [this](const httplib::Request& req, httplib::Response& res) {
            std::string response;
            if (!run_on_owner([&] { response = command::rpc::handle(*session, req.body); })) {
                unavailable(res);
                return;
            }
            if (response.empty()) {
                res.status = 204;
                return;
            }
            res.set_content(response, "application/json");
        });

        http.Post("/mcp", [this](const httplib::Request& req, httplib::Response& res) {
            if (req.has_header("MCP-Protocol-Version")) {
                const std::string version = req.get_header_value("MCP-Protocol-Version");
                if (std::ranges::find(command::mcp::kProtocolVersions, version) ==
                    command::mcp::kProtocolVersions.end()) {
                    send_json(res, 400,
                              error_body(command::code::kInvalidRequest,
                                         "unsupported MCP protocol version \"" + version + "\""));
                    return;
                }
            }
            std::string response;
            if (!run_on_owner([&] { response = mcp.handle(req.body); })) {
                unavailable(res);
                return;
            }
            if (response.empty()) {
                res.status = 202; // notifications get no response
                return;
            }
            res.set_content(response, "application/json");
        });

        // The server never pushes messages, so there is no event stream to open.
        http.Get("/mcp", [](const httplib::Request& /*req*/, httplib::Response& res) {
            res.set_header("Allow", "POST");
            send_json(res, 405,
                      error_body(command::code::kInvalidRequest,
                                 "this server sends no events; POST messages to /mcp"));
        });
    }
};

Server::Server(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Server::~Server() {
    stop();
}

Result<std::unique_ptr<Server>> Server::start(command::Session& session, ServerOptions options) {
    if (!is_loopback(options.host)) {
        return make_error(ErrorCode::InvalidArgument,
                          "the server only listens on this machine: use 127.0.0.1, ::1 or "
                          "localhost, not \"" +
                              options.host + "\"");
    }
    constexpr int kMaxPort = 65535;
    if (options.port < 0 || options.port > kMaxPort) {
        return make_error(ErrorCode::InvalidArgument,
                          "port " + std::to_string(options.port) + " is not between 0 and 65535");
    }
    if (options.token.empty()) {
        options.token = random_token();
    }
    const std::string host = options.host == "localhost" ? "127.0.0.1" : options.host;
    const int requested = options.port;
    const std::string token = options.token;

    auto impl = std::make_unique<Impl>(session, std::move(options));
    impl->http.set_socket_options(exclusive_port);
    int port = requested;
    if (requested == 0) {
        port = impl->http.bind_to_any_port(host);
    } else if (!impl->http.bind_to_port(host, requested)) {
        port = -1;
    }
    if (port <= 0) {
        return make_error(ErrorCode::PlatformFailure, "could not listen on " + host + ":" +
                                                          std::to_string(requested) +
                                                          "; is the port already in use?");
    }
    impl->endpoint = Endpoint{host, port, token};
    impl->install_routes();
    impl->listener = std::thread([raw = impl.get()] { raw->http.listen_after_bind(); });
    log::info("server listening", {{"url", impl->endpoint.url()}});
    return std::unique_ptr<Server>(new Server(std::move(impl)));
}

const Endpoint& Server::endpoint() const noexcept {
    return impl_->endpoint;
}

std::size_t Server::poll(std::chrono::milliseconds wait) {
    return impl_->poll(wait);
}

void Server::stop() {
    if (impl_) {
        impl_->stop();
    }
}

} // namespace slash::server
