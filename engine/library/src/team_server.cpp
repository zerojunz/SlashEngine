#include "slash/core/log.hpp"
#include "slash/library/team.hpp"
#include "slash/platform/file.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <httplib.h>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace slash::library {
namespace {

// Room for a large image or sound, in base64, and its record.
constexpr std::size_t kMaxBodyBytes = std::size_t{96} * 1024 * 1024;
constexpr int kMaxPort = 65535;

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

void send(httplib::Response& res, const TeamAnswer& answer) {
    res.status = answer.status;
    res.set_content(json::write(answer.body, json::Style::Compact), "application/json");
}

TeamAnswer error_answer(int status, std::string message) {
    json::Value body;
    body["error"]["message"] = std::move(message);
    return {status, std::move(body)};
}

// Keeps each port to one listener, as the command server does.
void exclusive_port(socket_t sock) {
#ifdef _WIN32
    httplib::set_socket_opt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, 1);
#else
    httplib::set_socket_opt(sock, SOL_SOCKET, SO_REUSEADDR, 1);
#endif
}

} // namespace

struct TeamServer::Impl {
    Library library;
    std::string token;
    std::string url;
    httplib::Server http;
    std::thread listener;
    std::mutex writes; // put and feedback read a record, change it and write it back

    explicit Impl(std::string dir) : library(std::move(dir)) {}

    void install_routes() {
        http.set_payload_max_length(kMaxBodyBytes);
        http.set_read_timeout(std::chrono::seconds(60));
        http.set_write_timeout(std::chrono::seconds(60));
        http.set_pre_routing_handler([this](const httplib::Request& req, httplib::Response& res) {
            // Teammates' tools send no Origin; a web page does, and has no business here.
            if (req.has_header("Origin")) {
                send(res, error_answer(403, "requests from web pages are not allowed"));
                return httplib::Server::HandlerResponse::Handled;
            }
            constexpr std::string_view kBearer = "Bearer ";
            const std::string authorization = req.get_header_value("Authorization");
            if (!authorization.starts_with(kBearer) ||
                !same_secret(std::string_view(authorization).substr(kBearer.size()), token)) {
                res.set_header("WWW-Authenticate", "Bearer");
                send(res, error_answer(401, "missing or wrong bearer token; set the variable "
                                            "providers.json's library \"token_env\" names "
                                            "(default SLASH_LIBRARY_TOKEN) to the server's token"));
                return httplib::Server::HandlerResponse::Handled;
            }
            return httplib::Server::HandlerResponse::Unhandled;
        });
        http.set_error_handler([](const httplib::Request& /*req*/, httplib::Response& res) {
            if (res.body.empty()) {
                send(res, error_answer(res.status, "the library server answers POST "
                                                   "/v1/library/<op>"));
            }
        });
        http.Post("/v1/library/:op", [this](const httplib::Request& req, httplib::Response& res) {
            auto body = json::parse(req.body);
            if (!body) {
                send(res, error_answer(400, "the body is not JSON: " + body.error().message));
                return;
            }
            const std::string& op = req.path_params.at("op");
            if (op == "put" || op == "feedback") {
                const std::scoped_lock lock(writes);
                send(res, answer_team_request(library, op, body.value()));
            } else {
                send(res, answer_team_request(library, op, body.value()));
            }
        });
    }
};

TeamServer::TeamServer(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

TeamServer::~TeamServer() {
    stop();
}

Result<std::unique_ptr<TeamServer>> TeamServer::start(std::string dir,
                                                      const TeamServerOptions& options) {
    if (options.port < 0 || options.port > kMaxPort) {
        return make_error(ErrorCode::InvalidArgument,
                          "port " + std::to_string(options.port) + " is not between 0 and 65535");
    }
    if (auto made = platform::create_directories(dir); !made) {
        return made.error();
    }
    auto impl = std::make_unique<Impl>(std::move(dir));
    impl->token = options.token.empty() ? random_token() : options.token;
    const std::string host = options.host == "localhost" ? "127.0.0.1" : options.host;
    impl->http.set_socket_options(exclusive_port);
    int port = options.port;
    if (port == 0) {
        port = impl->http.bind_to_any_port(host);
    } else if (!impl->http.bind_to_port(host, port)) {
        port = -1;
    }
    if (port <= 0) {
        return make_error(ErrorCode::PlatformFailure, "could not listen on " + host + ":" +
                                                          std::to_string(options.port) +
                                                          "; is the port already in use?");
    }
    const bool ipv6 = host.find(':') != std::string::npos;
    impl->url = "http://" + (ipv6 ? "[" + host + "]" : host) + ":" + std::to_string(port);
    impl->install_routes();
    impl->listener = std::thread([raw = impl.get()] { raw->http.listen_after_bind(); });
    log::info("library server listening", {{"url", impl->url}, {"dir", impl->library.dir()}});
    return std::unique_ptr<TeamServer>(new TeamServer(std::move(impl)));
}

const std::string& TeamServer::url() const noexcept {
    return impl_->url;
}

const std::string& TeamServer::token() const noexcept {
    return impl_->token;
}

void TeamServer::stop() {
    if (impl_ && impl_->listener.joinable()) {
        impl_->http.stop();
        impl_->listener.join();
    }
}

} // namespace slash::library
