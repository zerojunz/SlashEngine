// WebSocketPeer and WebSocketMultiplayerPeer, ported from Godot 4.5's modules/websocket (MIT):
// WSLPeer's handshakes and wslay callbacks over TCP sockets made with ENet's portable socket
// calls everywhere but the web, and EMWSPeer's use of the browser's WebSocket on the web.
#include "slash/net/websocket.hpp"

#include "slash/core/log.hpp"

#if SLASH_NET_WEBSOCKET
#include <enet/enet.h>
#include <mbedtls/base64.h>
#include <mbedtls/sha1.h>
#include <wslay/wslay.h>
#elif defined(__EMSCRIPTEN__)
#include <emscripten/em_js.h>
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstring>
#include <deque>
#include <map>
#include <random>
#include <set>
#include <utility>

namespace slash::net {

namespace {

constexpr int kDefaultBufferSize = 65535;
constexpr int kDefaultMaxQueuedPackets = 4096;
constexpr int kProtoSize = 9; // WebSocketMultiplayerPeer's PROTO_SIZE
[[maybe_unused]] constexpr std::size_t kMaxHeaderSize = 4096;

std::uint64_t ticks_msec() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

std::string lower(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string_view strip(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

[[maybe_unused]] std::vector<std::string_view> split(std::string_view text,
                                                     std::string_view separator) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (true) {
        const std::size_t at = text.find(separator, start);
        parts.push_back(
            text.substr(start, at == std::string_view::npos ? std::string_view::npos : at - start));
        if (at == std::string_view::npos) {
            return parts;
        }
        start = at + separator.size();
    }
}

// A URL's parts, as Godot's String::parse_url.
struct Url {
    std::string scheme; // "ws://" or "wss://"
    std::string host;
    int port = 0;
    std::string path;
};

Result<Url> parse_url(std::string_view text) {
    Url url;
    if (const std::size_t at = text.find("://"); at != std::string_view::npos) {
        url.scheme = lower(text.substr(0, at + 3));
        text.remove_prefix(at + 3);
    } else {
        url.scheme = "ws://";
    }
    if (url.scheme != "ws://" && url.scheme != "wss://") {
        return make_error(ErrorCode::InvalidArgument,
                          "\"" + url.scheme + "\" isn't ws:// or wss://");
    }
    const std::size_t path_at = text.find('/');
    std::string_view authority = text.substr(0, path_at);
    url.path = path_at == std::string_view::npos ? "/" : std::string(text.substr(path_at));
    if (const std::size_t hash = url.path.find('#'); hash != std::string::npos) {
        url.path.resize(hash);
    }
    if (const std::size_t colon = authority.rfind(':'); colon != std::string_view::npos) {
        const std::string_view digits = authority.substr(colon + 1);
        const auto [end, error] =
            std::from_chars(digits.data(), digits.data() + digits.size(), url.port);
        if (error != std::errc{} || end != digits.data() + digits.size() || url.port <= 0 ||
            url.port > 65535) {
            return make_error(ErrorCode::InvalidArgument, "the URL's port isn't a valid port");
        }
        authority = authority.substr(0, colon);
    }
    if (authority.empty()) {
        return make_error(ErrorCode::InvalidArgument, "the URL has no host");
    }
    url.host = std::string(authority);
    if (url.port == 0) {
        url.port = url.scheme == "wss://" ? 443 : 80;
    }
    return url;
}

// A message that arrived, as PacketBuffer keeps it.
struct Message {
    std::vector<std::uint8_t> bytes;
    bool is_string = false;
};

} // namespace

#if SLASH_NET_WEBSOCKET

namespace {

bool initialize_sockets() {
    static const bool initialized = enet_initialize() == 0;
    return initialized;
}

// A TCP connection, as Godot's StreamPeerTCP, over ENet's socket calls.
class TcpStream {
public:
    enum class Status : std::uint8_t { None, Connecting, Connected, Error };

    TcpStream() = default;
    TcpStream(const TcpStream&) = delete;
    TcpStream& operator=(const TcpStream&) = delete;
    ~TcpStream() { disconnect(); }

    // Takes a socket accepted by a server.
    void accept(ENetSocket socket) {
        disconnect();
        socket_ = socket;
        enet_socket_set_option(socket_, ENET_SOCKOPT_NONBLOCK, 1);
        enet_socket_set_option(socket_, ENET_SOCKOPT_NODELAY, 1);
        status_ = Status::Connected;
    }

    Result<void> connect(std::string_view host, int port) {
        disconnect();
        ENetAddress address{};
        address.port = static_cast<enet_uint16>(port);
        const std::string name(host);
        if (enet_address_set_host_ip(&address, name.c_str()) != 0 &&
            enet_address_set_host(&address, name.c_str()) != 0) {
            return make_error(ErrorCode::NotFound, "couldn't resolve \"" + name + "\"");
        }
        socket_ = enet_socket_create(ENET_SOCKET_TYPE_STREAM);
        if (socket_ == ENET_SOCKET_NULL) {
            return make_error(ErrorCode::PlatformFailure, "couldn't make a TCP socket");
        }
        enet_socket_set_option(socket_, ENET_SOCKOPT_NONBLOCK, 1);
        if (enet_socket_connect(socket_, &address) != 0) {
            disconnect();
            return make_error(ErrorCode::PlatformFailure, "couldn't connect to \"" + name + "\"");
        }
        status_ = Status::Connecting;
        return {};
    }

    // Moves a connection on, as StreamPeerTCP::poll.
    void poll() {
        if (status_ != Status::Connecting) {
            return;
        }
        enet_uint32 condition = ENET_SOCKET_WAIT_SEND;
        if (enet_socket_wait(socket_, &condition, 0) != 0) {
            status_ = Status::Error;
            return;
        }
        if ((condition & ENET_SOCKET_WAIT_SEND) == 0) {
            return; // still connecting
        }
        int error = 0;
        if (enet_socket_get_option(socket_, ENET_SOCKOPT_ERROR, &error) != 0 || error != 0) {
            status_ = Status::Error;
            return;
        }
        enet_socket_set_option(socket_, ENET_SOCKOPT_NODELAY, 1);
        status_ = Status::Connected;
    }

    [[nodiscard]] Status status() const { return status_; }

    // Reads what is there, up to size bytes: read is 0 when nothing is. Fails when the
    // connection broke or the other side closed it.
    Result<void> get_partial(std::uint8_t* data, std::size_t size, std::size_t& read) {
        read = 0;
        if (status_ != Status::Connected) {
            return make_error(ErrorCode::InvalidArgument, "not connected");
        }
        enet_uint32 condition = ENET_SOCKET_WAIT_RECEIVE;
        if (enet_socket_wait(socket_, &condition, 0) != 0) {
            status_ = Status::Error;
            return make_error(ErrorCode::PlatformFailure, "the connection broke");
        }
        if ((condition & ENET_SOCKET_WAIT_RECEIVE) == 0) {
            return {};
        }
        ENetBuffer buffer{};
        buffer.data = data;
        buffer.dataLength = size;
        const int got = enet_socket_receive(socket_, nullptr, &buffer, 1);
        if (got <= 0) {
            // Readable with nothing to read: the other side closed it.
            status_ = Status::Error;
            return make_error(ErrorCode::PlatformFailure, "the connection closed");
        }
        read = static_cast<std::size_t>(got);
        return {};
    }

    // Sends what the socket takes now: sent is 0 when it takes nothing.
    Result<void> put_partial(const std::uint8_t* data, std::size_t size, std::size_t& sent) {
        sent = 0;
        if (status_ != Status::Connected) {
            return make_error(ErrorCode::InvalidArgument, "not connected");
        }
        ENetBuffer buffer{};
        buffer.data =
            const_cast<std::uint8_t*>(data); // NOLINT(cppcoreguidelines-pro-type-const-cast)
        buffer.dataLength = size;
        const int put = enet_socket_send(socket_, nullptr, &buffer, 1);
        if (put < 0) {
            status_ = Status::Error;
            return make_error(ErrorCode::PlatformFailure, "the connection broke");
        }
        sent = static_cast<std::size_t>(put);
        return {};
    }

    void disconnect() {
        if (socket_ != ENET_SOCKET_NULL) {
            enet_socket_shutdown(socket_, ENET_SOCKET_SHUTDOWN_READ_WRITE);
            enet_socket_destroy(socket_);
            socket_ = ENET_SOCKET_NULL;
        }
        status_ = Status::None;
    }

private:
    ENetSocket socket_ = ENET_SOCKET_NULL;
    Status status_ = Status::None;
};

// A listening TCP socket, as Godot's TCPServer.
class TcpServer {
public:
    TcpServer() = default;
    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;
    ~TcpServer() { stop(); }

    Result<void> listen(int port, std::string_view bind_address) {
        stop();
        if (!initialize_sockets()) {
            return make_error(ErrorCode::PlatformFailure, "couldn't start the network");
        }
        ENetAddress address{};
        address.host = ENET_HOST_ANY;
        address.port = static_cast<enet_uint16>(port);
        if (bind_address != "*") {
            const std::string ip(bind_address);
            if (enet_address_set_host_ip(&address, ip.c_str()) != 0) {
                return make_error(ErrorCode::InvalidArgument,
                                  R"(")" + ip + R"(" isn't an IPv4 address or "*")");
            }
        }
        socket_ = enet_socket_create(ENET_SOCKET_TYPE_STREAM);
        if (socket_ == ENET_SOCKET_NULL) {
            return make_error(ErrorCode::PlatformFailure, "couldn't make a TCP socket");
        }
        enet_socket_set_option(socket_, ENET_SOCKOPT_REUSEADDR, 1);
        enet_socket_set_option(socket_, ENET_SOCKOPT_NONBLOCK, 1);
        if (enet_socket_bind(socket_, &address) != 0 || enet_socket_listen(socket_, -1) != 0) {
            stop();
            return make_error(ErrorCode::PlatformFailure,
                              "couldn't listen on port " + std::to_string(port));
        }
        ENetAddress bound{};
        if (enet_socket_get_address(socket_, &bound) == 0) {
            port_ = bound.port;
        }
        return {};
    }

    // A connection waiting to be taken, or ENET_SOCKET_NULL.
    [[nodiscard]] ENetSocket take_connection() const {
        if (socket_ == ENET_SOCKET_NULL) {
            return ENET_SOCKET_NULL;
        }
        enet_uint32 condition = ENET_SOCKET_WAIT_RECEIVE;
        if (enet_socket_wait(socket_, &condition, 0) != 0 ||
            (condition & ENET_SOCKET_WAIT_RECEIVE) == 0) {
            return ENET_SOCKET_NULL;
        }
        return enet_socket_accept(socket_, nullptr);
    }

    [[nodiscard]] bool listening() const { return socket_ != ENET_SOCKET_NULL; }
    [[nodiscard]] int port() const { return port_; }

    void stop() {
        if (socket_ != ENET_SOCKET_NULL) {
            enet_socket_destroy(socket_);
            socket_ = ENET_SOCKET_NULL;
        }
        port_ = 0;
    }

private:
    ENetSocket socket_ = ENET_SOCKET_NULL;
    int port_ = 0;
};

void random_bytes(std::uint8_t* out, std::size_t size) {
    static std::random_device device;
    for (std::size_t i = 0; i < size; ++i) {
        out[i] = static_cast<std::uint8_t>(device() & 0xFFU);
    }
}

std::string base64(const std::uint8_t* data, std::size_t size) {
    std::string out(((size + 2) / 3 * 4) + 1, '\0');
    std::size_t written = 0;
    mbedtls_base64_encode(reinterpret_cast<unsigned char*>(out.data()), out.size(), &written, data,
                          size);
    out.resize(written);
    return out;
}

// As WSLPeer::_generate_key: 16 random bytes in base 64.
std::string generate_key() {
    std::array<std::uint8_t, 16> key{};
    random_bytes(key.data(), key.size());
    return base64(key.data(), key.size());
}

// As WSLPeer::_compute_key_response.
std::string compute_key_response(std::string_view key) {
    const std::string text = std::string(key) + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::array<unsigned char, 20> sha{};
    mbedtls_sha1(reinterpret_cast<const unsigned char*>(text.data()), text.size(), sha.data());
    return base64(sha.data(), sha.size());
}

// A handshake's header lines, by lower-case name, as WSLPeer reads them.
Result<std::map<std::string, std::string>>
parse_headers(const std::vector<std::string_view>& lines) {
    std::map<std::string, std::string> headers;
    for (std::size_t i = 1; i < lines.size(); ++i) {
        const std::size_t colon = lines[i].find(':');
        if (colon == std::string_view::npos || colon == 0) {
            return make_error(ErrorCode::InvalidArgument,
                              "invalid header \"" + std::string(lines[i]) + "\"");
        }
        const std::string name = lower(strip(lines[i].substr(0, colon)));
        const std::string_view value = strip(lines[i].substr(colon + 1));
        auto [entry, added] = headers.emplace(name, std::string(value));
        if (!added) {
            entry->second += "," + std::string(value);
        }
    }
    return headers;
}

} // namespace

struct WebSocketPeer::State {
    // Settings.
    std::uint64_t heartbeat_interval_msec = 0;
    std::vector<std::string> supported_protocols;
    std::vector<std::string> handshake_headers;

    // The connection.
    std::unique_ptr<TcpStream> tcp;
    wslay_event_context_ptr ctx = nullptr;
    std::size_t handshake_at = 0;
    std::uint64_t last_heartbeat = 0;
    std::size_t in_bytes = 0; // what arrived, in in_buffer
    std::string requested_url;
    std::string requested_host;
    std::string session_key;
    std::string selected_protocol;
    std::vector<std::uint8_t> handshake; // the request or response being sent or read

    // What arrived.
    Message pending_message;
    Message current;
    std::string close_reason;
    std::deque<Message> in_buffer;

    int inbound_buffer_size = kDefaultBufferSize;
    int outbound_buffer_size = kDefaultBufferSize;
    int max_queued_packets = kDefaultMaxQueuedPackets;
    int close_code = -1;
    WebSocketState ready_state = WebSocketState::Closed;
    bool is_server = false;
    bool pending_request = true;
    bool heartbeat_waiting = false;
    std::uint8_t pending_opcode = 0;
    bool was_string = false;

    State() = default;
    State(const State&) = delete;
    State& operator=(const State&) = delete;
    ~State() { clear(); }

    void free_context() {
        if (ctx != nullptr) {
            wslay_event_context_free(ctx);
            ctx = nullptr;
        }
    }

    void clear() {
        ready_state = WebSocketState::Closed;
        is_server = false;
        free_context();
        tcp.reset();
        requested_url.clear();
        requested_host.clear();
        pending_request = true;
        handshake.clear();
        handshake_at = 0;
        selected_protocol.clear();
        session_key.clear();
        was_string = false;
        in_buffer.clear();
        in_bytes = 0;
        pending_message = {};
        pending_opcode = 0;
        current = {};
        close_code = -1;
        close_reason.clear();
    }

    // ------------------------------------------------------------------------------------------
    // wslay's callbacks.

    static ssize_t recv_callback(wslay_event_context_ptr context, std::uint8_t* data,
                                 std::size_t length, int /*flags*/, void* user) {
        auto* self = static_cast<State*>(user);
        // Read no more than the buffer holds.
        const auto limit = static_cast<std::size_t>(self->inbound_buffer_size);
        const std::size_t used = self->in_bytes + self->pending_message.bytes.size();
        const std::size_t space = used < limit ? limit - used : 0;
        const std::size_t packets_left =
            static_cast<std::size_t>(self->max_queued_packets) > self->in_buffer.size()
                ? static_cast<std::size_t>(self->max_queued_packets) - self->in_buffer.size()
                : 0;
        const std::size_t to_read = std::min({length, space, packets_left * 2});
        if (to_read == 0 || !self->tcp) {
            wslay_event_set_error(context,
                                  self->tcp ? WSLAY_ERR_WOULDBLOCK : WSLAY_ERR_CALLBACK_FAILURE);
            return -1;
        }
        std::size_t read = 0;
        if (!self->tcp->get_partial(data, to_read, read)) {
            wslay_event_set_error(context, WSLAY_ERR_CALLBACK_FAILURE);
            return -1;
        }
        if (read == 0) {
            wslay_event_set_error(context, WSLAY_ERR_WOULDBLOCK);
            return -1;
        }
        return static_cast<ssize_t>(read);
    }

    static ssize_t send_callback(wslay_event_context_ptr context, const std::uint8_t* data,
                                 std::size_t length, int /*flags*/, void* user) {
        auto* self = static_cast<State*>(user);
        std::size_t sent = 0;
        if (!self->tcp || !self->tcp->put_partial(data, length, sent)) {
            wslay_event_set_error(context, WSLAY_ERR_CALLBACK_FAILURE);
            return -1;
        }
        if (sent == 0) {
            wslay_event_set_error(context, WSLAY_ERR_WOULDBLOCK);
            return -1;
        }
        return static_cast<ssize_t>(sent);
    }

    static int genmask_callback(wslay_event_context_ptr /*context*/, std::uint8_t* buffer,
                                std::size_t length, void* /*user*/) {
        random_bytes(buffer, length);
        return 0;
    }

    static void recv_start_callback(wslay_event_context_ptr /*context*/,
                                    const wslay_event_on_frame_recv_start_arg* arg, void* user) {
        auto* self = static_cast<State*>(user);
        if (arg->opcode == WSLAY_TEXT_FRAME || arg->opcode == WSLAY_BINARY_FRAME) {
            self->pending_opcode = arg->opcode;
        }
    }

    static void frame_recv_chunk_callback(wslay_event_context_ptr /*context*/,
                                          const wslay_event_on_frame_recv_chunk_arg* arg,
                                          void* user) {
        auto* self = static_cast<State*>(user);
        if (self->pending_opcode != 0) {
            self->pending_message.bytes.insert(self->pending_message.bytes.end(), arg->data,
                                               arg->data + arg->data_length);
        }
    }

    static void msg_recv_callback(wslay_event_context_ptr /*context*/,
                                  const wslay_event_on_msg_recv_arg* arg, void* user) {
        auto* self = static_cast<State*>(user);
        const std::uint8_t op = arg->opcode;
        if (op == WSLAY_CONNECTION_CLOSE) {
            // A close request or its answer.
            self->close_code = arg->status_code;
            self->close_reason.clear();
            if (arg->msg_length > 2) { // the first two bytes are the code
                self->close_reason.assign(reinterpret_cast<const char*>(arg->msg) + 2,
                                          arg->msg_length - 2);
            }
            if (self->ready_state == WebSocketState::Open) {
                self->ready_state = WebSocketState::Closing;
            }
            return;
        }
        if (op == WSLAY_PONG) {
            self->heartbeat_waiting = false;
        } else if (op == WSLAY_TEXT_FRAME || op == WSLAY_BINARY_FRAME) {
            if (self->pending_opcode != op) {
                return;
            }
            self->pending_message.is_string = op == WSLAY_TEXT_FRAME;
            self->in_bytes += self->pending_message.bytes.size();
            self->in_buffer.push_back(std::move(self->pending_message));
            self->pending_message = {};
            self->pending_opcode = 0;
        }
    }

    static constexpr wslay_event_callbacks kCallbacks = {recv_callback,
                                                         send_callback,
                                                         genmask_callback,
                                                         recv_start_callback,
                                                         frame_recv_chunk_callback,
                                                         nullptr,
                                                         msg_recv_callback};

    void open(bool server) {
        if (server) {
            wslay_event_context_server_init(&ctx, &kCallbacks, this);
        } else {
            wslay_event_context_client_init(&ctx, &kCallbacks, this);
        }
        wslay_event_config_set_no_buffering(ctx, 1);
        wslay_event_config_set_max_recv_msg_length(ctx,
                                                   static_cast<std::uint64_t>(inbound_buffer_size));
        last_heartbeat = ticks_msec();
        ready_state = WebSocketState::Open;
    }

    // ------------------------------------------------------------------------------------------
    // Handshakes.

    // Reads the handshake a byte at a time up to its blank line, as WSLPeer does: true once
    // it ended, false while more is to come or on an error (then closed).
    bool read_handshake() {
        while (true) {
            if (handshake.size() >= kMaxHeaderSize) {
                log::error("a WebSocket handshake's headers are too big");
                force_close();
                return false;
            }
            std::uint8_t byte = 0;
            std::size_t read = 0;
            if (!tcp->get_partial(&byte, 1, read)) {
                force_close();
                return false;
            }
            if (read != 1) {
                return false; // wait for the next poll
            }
            handshake.push_back(byte);
            const std::size_t n = handshake.size();
            if (n > 3 && handshake[n - 1] == '\n' && handshake[n - 2] == '\r' &&
                handshake[n - 3] == '\n' && handshake[n - 4] == '\r') {
                return true;
            }
        }
    }

    // Sends what is left of the handshake: true once it is all sent.
    bool write_handshake() {
        const std::size_t left = handshake.size() - handshake_at;
        std::size_t sent = 0;
        if (!tcp->put_partial(handshake.data() + handshake_at, left, sent)) {
            force_close();
            return false;
        }
        handshake_at += sent;
        return handshake_at == handshake.size();
    }

    [[nodiscard]] std::vector<std::string_view> handshake_lines() const {
        const std::string_view text(reinterpret_cast<const char*>(handshake.data()),
                                    handshake.size() - 4);
        return split(text, "\r\n");
    }

    // As _parse_client_request.
    Result<void> parse_client_request() {
        const std::vector<std::string_view> lines = handshake_lines();
        if (lines.size() < 4) {
            return make_error(ErrorCode::InvalidArgument, "not enough request headers");
        }
        const std::vector<std::string_view> request = split(lines[0], " ");
        if (request.size() < 3 || request[0] != "GET" || request[2] != "HTTP/1.1") {
            return make_error(ErrorCode::InvalidArgument, "invalid method or HTTP version");
        }
        auto headers = parse_headers(lines);
        if (!headers) {
            return headers.error();
        }
        auto& h = headers.value();
        requested_host = h.contains("host") ? h["host"] : "";
        requested_url = "ws://" + requested_host + std::string(request[1]);
        if (lower(h["upgrade"]) != "websocket" || h["sec-websocket-version"] != "13" ||
            !h.contains("sec-websocket-key") || !h.contains("connection")) {
            return make_error(ErrorCode::InvalidArgument,
                              "missing or invalid WebSocket request headers");
        }
        session_key = h["sec-websocket-key"];
        if (const auto requested = h.find("sec-websocket-protocol"); requested != h.end()) {
            for (const std::string_view proto : split(requested->second, ",")) {
                if (std::ranges::find(supported_protocols, strip(proto)) !=
                    supported_protocols.end()) {
                    selected_protocol = std::string(strip(proto));
                    break;
                }
            }
            if (selected_protocol.empty()) {
                return make_error(ErrorCode::InvalidArgument,
                                  "the client asked only for subprotocols this side lacks");
            }
        } else if (!supported_protocols.empty()) {
            return make_error(ErrorCode::InvalidArgument, "the client asked for no subprotocol");
        }
        return {};
    }

    // As _do_server_handshake.
    void server_handshake() {
        if (pending_request) {
            if (!read_handshake()) {
                return;
            }
            if (auto parsed = parse_client_request(); !parsed) {
                log::warn("refused a WebSocket client", {{"error", parsed.error().message}});
                force_close();
                return;
            }
            std::string response = "HTTP/1.1 101 Switching Protocols\r\n"
                                   "Upgrade: websocket\r\n"
                                   "Connection: Upgrade\r\n";
            response += "Sec-WebSocket-Accept: " + compute_key_response(session_key) + "\r\n";
            if (!selected_protocol.empty()) {
                response += "Sec-WebSocket-Protocol: " + selected_protocol + "\r\n";
            }
            for (const std::string& header : handshake_headers) {
                response += header + "\r\n";
            }
            response += "\r\n";
            handshake.assign(response.begin(), response.end());
            handshake_at = 0;
            pending_request = false;
        }
        if (write_handshake()) {
            open(true);
        }
    }

    // As _verify_server_response.
    Result<void> verify_server_response() {
        const std::vector<std::string_view> lines = handshake_lines();
        if (lines.size() < 4) {
            return make_error(ErrorCode::InvalidArgument, "not enough response headers");
        }
        const std::vector<std::string_view> status = split(lines[0], " ");
        if (status.size() < 2 || status[0] != "HTTP/1.1" || status[1] != "101") {
            return make_error(ErrorCode::InvalidArgument, R"(the server answered ")" +
                                                              std::string(lines[0]) +
                                                              R"(", not "HTTP/1.1 101")");
        }
        auto headers = parse_headers(lines);
        if (!headers) {
            return headers.error();
        }
        auto& h = headers.value();
        if (lower(h["connection"]) != "upgrade" || lower(h["upgrade"]) != "websocket" ||
            h["sec-websocket-accept"] != compute_key_response(session_key)) {
            return make_error(ErrorCode::InvalidArgument,
                              "missing or invalid WebSocket response headers");
        }
        const auto proto = h.find("sec-websocket-protocol");
        if (supported_protocols.empty()) {
            if (proto != h.end()) {
                return make_error(ErrorCode::InvalidArgument,
                                  "the server chose a subprotocol no one asked for");
            }
        } else {
            if (proto == h.end() || std::ranges::find(supported_protocols, proto->second) ==
                                        supported_protocols.end()) {
                return make_error(ErrorCode::InvalidArgument,
                                  "the server chose none of the subprotocols asked for");
            }
            selected_protocol = proto->second;
        }
        return {};
    }

    // As _do_client_handshake.
    void client_handshake() {
        tcp->poll();
        if (tcp->status() == TcpStream::Status::Connecting) {
            return;
        }
        if (tcp->status() != TcpStream::Status::Connected) {
            force_close(); // couldn't connect
            return;
        }
        if (pending_request) {
            if (write_handshake()) {
                pending_request = false;
                handshake.clear();
                handshake_at = 0;
            }
            return;
        }
        if (!read_handshake()) {
            return;
        }
        if (auto verified = verify_server_response(); !verified) {
            log::warn("a WebSocket server's answer was refused",
                      {{"url", requested_url}, {"error", verified.error().message}});
            force_close();
            return;
        }
        open(false);
    }

    void force_close() {
        free_context();
        tcp.reset();
        ready_state = WebSocketState::Closed;
        heartbeat_waiting = false;
        in_buffer.clear();
        in_bytes = 0;
        pending_message = {};
        pending_opcode = 0;
    }

    // As WSLPeer::poll.
    void poll() {
        if (ready_state == WebSocketState::Closed) {
            return;
        }
        if (ready_state == WebSocketState::Connecting) {
            if (is_server) {
                server_handshake();
            } else {
                client_handshake();
            }
        }
        if (ready_state != WebSocketState::Open && ready_state != WebSocketState::Closing) {
            return;
        }
        const std::uint64_t ticks = ticks_msec();
        if (heartbeat_interval_msec != 0 && ticks - last_heartbeat > heartbeat_interval_msec &&
            ready_state == WebSocketState::Open) {
            if (heartbeat_waiting) {
                force_close();
                return;
            }
            heartbeat_waiting = true;
            const wslay_event_msg ping{WSLAY_PING, nullptr, 0};
            if (wslay_event_queue_msg(ctx, &ping) != 0) {
                force_close();
                return;
            }
            last_heartbeat = ticks;
        }
        if (wslay_event_recv(ctx) != 0 || wslay_event_send(ctx) != 0) {
            force_close();
            return;
        }
        if (wslay_event_get_close_sent(ctx) != 0) {
            if (wslay_event_get_close_received(ctx) != 0) {
                // A clean close.
                force_close();
                return;
            }
            if (wslay_event_get_read_enabled(ctx) == 0) {
                // A protocol error stopped reading, so no close will come.
                close_code = wslay_event_get_status_code_sent(ctx);
                switch (close_code) {
                case WSLAY_CODE_MESSAGE_TOO_BIG:
                    close_reason = "Message too big";
                    break;
                case WSLAY_CODE_PROTOCOL_ERROR:
                    close_reason = "Protocol error";
                    break;
                case WSLAY_CODE_ABNORMAL_CLOSURE:
                    close_reason = "Abnormal closure";
                    break;
                case WSLAY_CODE_INVALID_FRAME_PAYLOAD_DATA:
                    close_reason = "Invalid frame payload data";
                    break;
                default:
                    close_reason = "Unknown";
                }
                force_close();
            }
        }
    }

    Result<void> connect_to_url(std::string_view text) {
        if (ready_state != WebSocketState::Closed && ready_state != WebSocketState::Closing) {
            return make_error(ErrorCode::InvalidArgument, "the WebSocket is already in use");
        }
        clear();
        auto url = parse_url(text);
        if (!url) {
            return url.error();
        }
        if (url.value().scheme == "wss://") {
            return make_error(ErrorCode::InvalidArgument,
                              "wss:// isn't available outside the web yet; use ws://");
        }
        if (!initialize_sockets()) {
            return make_error(ErrorCode::PlatformFailure, "couldn't start the network");
        }
        tcp = std::make_unique<TcpStream>();
        if (auto connected = tcp->connect(url.value().host, url.value().port); !connected) {
            clear();
            return connected;
        }
        requested_url = std::string(text);
        requested_host = url.value().host;
        session_key = generate_key();
        std::string request = "GET " + url.value().path + " HTTP/1.1\r\n";
        request += "Host: " + requested_host;
        if (url.value().port != 80) {
            request += ":" + std::to_string(url.value().port);
        }
        request += "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n";
        request += "Sec-WebSocket-Key: " + session_key + "\r\n";
        request += "Sec-WebSocket-Version: 13\r\n";
        if (!supported_protocols.empty()) {
            request += "Sec-WebSocket-Protocol: ";
            for (std::size_t i = 0; i < supported_protocols.size(); ++i) {
                request += (i != 0 ? "," : "") + supported_protocols[i];
            }
            request += "\r\n";
        }
        for (const std::string& header : handshake_headers) {
            request += header + "\r\n";
        }
        request += "\r\n";
        handshake.assign(request.begin(), request.end());
        handshake_at = 0;
        ready_state = WebSocketState::Connecting;
        is_server = false;
        return {};
    }

    void accept_stream(std::unique_ptr<TcpStream> stream) {
        clear();
        tcp = std::move(stream);
        is_server = true;
        ready_state = WebSocketState::Connecting;
    }

    Result<void> send(std::span<const std::uint8_t> message, bool text) {
        if (ready_state != WebSocketState::Open) {
            return make_error(ErrorCode::InvalidArgument, "the WebSocket isn't open");
        }
        if (wslay_event_get_queued_msg_count(ctx) >= static_cast<std::size_t>(max_queued_packets)) {
            return make_error(ErrorCode::InvalidArgument, "too many messages wait to be sent");
        }
        if (outbound_buffer_size > 0 && wslay_event_get_queued_msg_length(ctx) + message.size() >
                                            static_cast<std::size_t>(outbound_buffer_size)) {
            return make_error(ErrorCode::InvalidArgument, "too many bytes wait to be sent");
        }
        const wslay_event_msg msg{
            static_cast<std::uint8_t>(text ? WSLAY_TEXT_FRAME : WSLAY_BINARY_FRAME), message.data(),
            message.size()};
        if (wslay_event_queue_msg(ctx, &msg) != 0 || wslay_event_send(ctx) != 0) {
            force_close();
            return make_error(ErrorCode::PlatformFailure, "the WebSocket connection broke");
        }
        return {};
    }

    [[nodiscard]] int outbound_buffered() const {
        return ready_state == WebSocketState::Open && ctx != nullptr
                   ? static_cast<int>(wslay_event_get_queued_msg_length(ctx))
                   : 0;
    }

    void close(int code, std::string_view reason) {
        if (code < 0) {
            force_close();
            return;
        }
        if (ready_state == WebSocketState::Open && wslay_event_get_close_sent(ctx) == 0) {
            wslay_event_queue_close(ctx, static_cast<std::uint16_t>(code),
                                    reinterpret_cast<const std::uint8_t*>(reason.data()),
                                    reason.size());
            wslay_event_send(ctx);
            ready_state = WebSocketState::Closing;
        } else if (ready_state == WebSocketState::Connecting) {
            force_close();
        }
    }
};

#elif defined(__EMSCRIPTEN__)

// The browser's WebSocket, as Godot's library_godot_websocket.js: each socket's events wait in
// a list that poll() takes them from.
// NOLINTBEGIN
// clang-format off
EM_JS_DEPS(slash_ws, "$UTF8ToString");
EM_JS(int, slash_ws_create, (const char* url, const char* protocols), {
    const all = globalThis.slashWebSockets ||
                (globalThis.slashWebSockets = {next: 1, map: new Map()});
    let socket;
    try {
        const protos = UTF8ToString(protocols);
        socket = protos ? new WebSocket(UTF8ToString(url), protos.split(","))
                        : new WebSocket(UTF8ToString(url));
    } catch (e) {
        return 0;
    }
    socket.binaryType = "arraybuffer";
    const id = all.next++;
    const entry = {socket: socket, events: []};
    socket.onopen = () => {
        entry.events.push({kind: 0, data: new TextEncoder().encode(socket.protocol)});
    };
    socket.onmessage = (event) => {
        if (event.data instanceof ArrayBuffer) {
            entry.events.push({kind: 1, data: new Uint8Array(event.data)});
        } else if (typeof event.data === "string") {
            entry.events.push({kind: 2, data: new TextEncoder().encode(event.data)});
        }
    };
    socket.onerror = () => {
        entry.events.push({kind: 4, data: new Uint8Array(0)});
    };
    socket.onclose = (event) => {
        entry.events.push({kind: 3, code: event.code, data: new TextEncoder().encode(event.reason)});
    };
    all.map.set(id, entry);
    return id;
});
// The next event's size, its kind and code in info, or -1 for none.
EM_JS(int, slash_ws_peek, (int id, int* info), {
    const entry = globalThis.slashWebSockets && globalThis.slashWebSockets.map.get(id);
    if (!entry || entry.events.length === 0) {
        return -1;
    }
    const event = entry.events[0];
    HEAP32[info >> 2] = event.kind;
    HEAP32[(info >> 2) + 1] = event.code || 0;
    return event.data.length;
});
EM_JS(void, slash_ws_take, (int id, unsigned char* out), {
    const entry = globalThis.slashWebSockets.map.get(id);
    const event = entry.events.shift();
    HEAPU8.set(event.data, out);
});
EM_JS(int, slash_ws_send, (int id, const unsigned char* data, int size, int binary), {
    const entry = globalThis.slashWebSockets && globalThis.slashWebSockets.map.get(id);
    if (!entry || entry.socket.readyState !== 1) {
        return 1;
    }
    const bytes = HEAPU8.slice(data, data + size);
    entry.socket.send(binary ? bytes.buffer : new TextDecoder().decode(bytes));
    return 0;
});
EM_JS(int, slash_ws_buffered, (int id), {
    const entry = globalThis.slashWebSockets && globalThis.slashWebSockets.map.get(id);
    return entry ? entry.socket.bufferedAmount : 0;
});
EM_JS(void, slash_ws_close, (int id, int code, const char* reason), {
    const entry = globalThis.slashWebSockets && globalThis.slashWebSockets.map.get(id);
    if (entry && entry.socket.readyState < 2) {
        try {
            entry.socket.close(code, UTF8ToString(reason));
        } catch (e) {
            entry.socket.close(); // browsers take only 1000 and 3000 to 4999
        }
    }
});
EM_JS(void, slash_ws_destroy, (int id), {
    const entry = globalThis.slashWebSockets && globalThis.slashWebSockets.map.get(id);
    if (!entry) {
        return;
    }
    if (entry.socket.readyState < 2) {
        entry.socket.close(3001, "destroyed");
    }
    entry.socket.onopen = entry.socket.onmessage = entry.socket.onerror = entry.socket.onclose =
        null;
    globalThis.slashWebSockets.map.delete(id);
});
// clang-format on
// NOLINTEND

// As Godot's EMWSPeer.
struct WebSocketPeer::State {
    std::vector<std::string> supported_protocols;
    std::vector<std::string> handshake_headers;
    int inbound_buffer_size = kDefaultBufferSize;
    int outbound_buffer_size = kDefaultBufferSize;
    int max_queued_packets = kDefaultMaxQueuedPackets;
    std::uint64_t heartbeat_interval_msec = 0;

    WebSocketState ready_state = WebSocketState::Closed;
    int socket = 0;
    std::string requested_url;
    std::string selected_protocol;
    std::deque<Message> in_buffer;
    std::size_t in_bytes = 0;
    Message current;
    bool was_string = false;
    int close_code = -1;
    std::string close_reason;

    State() = default;
    State(const State&) = delete;
    State& operator=(const State&) = delete;
    ~State() { clear(); }

    void destroy_socket() {
        if (socket != 0) {
            slash_ws_destroy(socket);
            socket = 0;
        }
    }

    void clear() {
        destroy_socket();
        ready_state = WebSocketState::Closed;
        was_string = false;
        close_code = -1;
        close_reason.clear();
        selected_protocol.clear();
        requested_url.clear();
        in_buffer.clear();
        in_bytes = 0;
        current = {};
    }

    Result<void> connect_to_url(std::string_view text) {
        if (ready_state != WebSocketState::Closed && ready_state != WebSocketState::Closing) {
            return make_error(ErrorCode::InvalidArgument, "the WebSocket is already in use");
        }
        clear();
        auto url = parse_url(text);
        if (!url) {
            return url.error();
        }
        if (!handshake_headers.empty()) {
            log::warn("browsers' WebSockets take no extra handshake headers");
        }
        std::string protocols;
        for (std::size_t i = 0; i < supported_protocols.size(); ++i) {
            protocols += (i != 0 ? "," : "") + supported_protocols[i];
        }
        const Url& u = url.value();
        requested_url = u.scheme + u.host;
        if ((u.scheme == "ws://" && u.port != 80) || (u.scheme == "wss://" && u.port != 443)) {
            requested_url += ":" + std::to_string(u.port);
        }
        requested_url += u.path;
        socket = slash_ws_create(requested_url.c_str(), protocols.c_str());
        if (socket == 0) {
            return make_error(ErrorCode::PlatformFailure, "the browser refused the WebSocket URL");
        }
        ready_state = WebSocketState::Connecting;
        return {};
    }

    void poll() {
        if (socket == 0) {
            return;
        }
        std::array<int, 2> info{};
        int size = 0;
        while ((size = slash_ws_peek(socket, info.data())) >= 0) {
            Message message;
            message.bytes.resize(static_cast<std::size_t>(size));
            slash_ws_take(socket, message.bytes.data());
            switch (info[0]) {
            case 0: // open
                ready_state = WebSocketState::Open;
                selected_protocol.assign(message.bytes.begin(), message.bytes.end());
                break;
            case 1: // binary
            case 2: // text
                if (static_cast<int>(in_buffer.size()) >= max_queued_packets ||
                    in_bytes + message.bytes.size() >
                        static_cast<std::size_t>(inbound_buffer_size)) {
                    log::error("a WebSocket message didn't fit the inbound buffer");
                    break;
                }
                message.is_string = info[0] == 2;
                in_bytes += message.bytes.size();
                in_buffer.push_back(std::move(message));
                break;
            case 3: // close
                close_code = info[1];
                close_reason.assign(message.bytes.begin(), message.bytes.end());
                ready_state = WebSocketState::Closed;
                break;
            default: // error; a close follows
                ready_state = WebSocketState::Closed;
                break;
            }
        }
        if (ready_state == WebSocketState::Closed) {
            destroy_socket();
        }
    }

    void accept_stream() {}

    Result<void> send(std::span<const std::uint8_t> message, bool text) {
        if (outbound_buffer_size > 0 &&
            outbound_buffered() + static_cast<int>(message.size()) >= outbound_buffer_size) {
            return make_error(ErrorCode::InvalidArgument, "too many bytes wait to be sent");
        }
        if (slash_ws_send(socket, message.data(), static_cast<int>(message.size()), text ? 0 : 1) !=
            0) {
            return make_error(ErrorCode::InvalidArgument, "the WebSocket isn't open");
        }
        return {};
    }

    [[nodiscard]] int outbound_buffered() const {
        return socket != 0 ? slash_ws_buffered(socket) : 0;
    }

    void close(int code, std::string_view reason) {
        if (code < 0) {
            destroy_socket();
            ready_state = WebSocketState::Closed;
        }
        if (ready_state == WebSocketState::Connecting || ready_state == WebSocketState::Open) {
            ready_state = WebSocketState::Closing;
            if (socket != 0) {
                const std::string text(reason);
                slash_ws_close(socket, code, text.c_str());
            } else {
                ready_state = WebSocketState::Closed;
            }
        }
        in_buffer.clear();
        in_bytes = 0;
    }
};

#else

// No WebSocket in this build.
struct WebSocketPeer::State {
    std::vector<std::string> supported_protocols;
    std::vector<std::string> handshake_headers;
    int inbound_buffer_size = kDefaultBufferSize;
    int outbound_buffer_size = kDefaultBufferSize;
    int max_queued_packets = kDefaultMaxQueuedPackets;
    std::uint64_t heartbeat_interval_msec = 0;
    WebSocketState ready_state = WebSocketState::Closed;
    std::string requested_url;
    std::string selected_protocol;
    std::deque<Message> in_buffer;
    std::size_t in_bytes = 0;
    Message current;
    bool was_string = false;
    int close_code = -1;
    std::string close_reason;

    static Result<void> connect_to_url(std::string_view /*url*/) {
        return make_error(ErrorCode::InvalidArgument, "this build has no WebSocket");
    }
    void poll() {}
    static Result<void> send(std::span<const std::uint8_t> /*message*/, bool /*text*/) {
        return make_error(ErrorCode::InvalidArgument, "this build has no WebSocket");
    }
    [[nodiscard]] static int outbound_buffered() { return 0; }
    void close(int /*code*/, std::string_view /*reason*/) {}
};

#endif

bool websocket_server_supported() {
    return SLASH_NET_WEBSOCKET != 0;
}

// ----------------------------------------------------------------------------------------------
// WebSocketPeer

WebSocketPeer::WebSocketPeer() : state_(std::make_unique<State>()) {}

WebSocketPeer::~WebSocketPeer() = default;

Result<void> WebSocketPeer::connect_to_url(std::string_view url) {
    return state_->connect_to_url(url);
}

void WebSocketPeer::poll() {
    state_->poll();
}

WebSocketState WebSocketPeer::ready_state() const {
    return state_->ready_state;
}

Result<void> WebSocketPeer::send(std::span<const std::uint8_t> message, WriteMode mode) {
    return state_->send(message, mode == WriteMode::Text);
}

Result<void> WebSocketPeer::send_text(std::string_view text) {
    return send(std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()),
                WriteMode::Text);
}

Result<void> WebSocketPeer::put_packet(std::span<const std::uint8_t> message) {
    return send(message, WriteMode::Binary);
}

int WebSocketPeer::available_packet_count() const {
    return state_->ready_state == WebSocketState::Open ? static_cast<int>(state_->in_buffer.size())
                                                       : 0;
}

Result<std::span<const std::uint8_t>> WebSocketPeer::get_packet() {
    State& s = *state_;
    if (s.in_buffer.empty()) {
        return make_error(ErrorCode::NotFound, "no message is waiting");
    }
    s.current = std::move(s.in_buffer.front());
    s.in_buffer.pop_front();
    s.in_bytes -= s.current.bytes.size();
    s.was_string = s.current.is_string;
    return std::span<const std::uint8_t>(s.current.bytes);
}

bool WebSocketPeer::was_string_packet() const {
    return state_->was_string;
}

int WebSocketPeer::current_outbound_buffered_amount() const {
    return state_->outbound_buffered();
}

void WebSocketPeer::close(int code, std::string_view reason) {
    state_->close(code, reason);
}

int WebSocketPeer::close_code() const {
    return state_->close_code;
}

const std::string& WebSocketPeer::close_reason() const {
    return state_->close_reason;
}

const std::string& WebSocketPeer::requested_url() const {
    return state_->requested_url;
}

const std::string& WebSocketPeer::selected_protocol() const {
    return state_->selected_protocol;
}

void WebSocketPeer::set_supported_protocols(std::vector<std::string> protocols) {
    for (std::string& protocol : protocols) {
        protocol = std::string(strip(protocol));
    }
    state_->supported_protocols = std::move(protocols);
}

const std::vector<std::string>& WebSocketPeer::supported_protocols() const {
    return state_->supported_protocols;
}

void WebSocketPeer::set_handshake_headers(std::vector<std::string> headers) {
    state_->handshake_headers = std::move(headers);
}

const std::vector<std::string>& WebSocketPeer::handshake_headers() const {
    return state_->handshake_headers;
}

void WebSocketPeer::set_inbound_buffer_size(int bytes) {
    state_->inbound_buffer_size = bytes;
}

int WebSocketPeer::inbound_buffer_size() const {
    return state_->inbound_buffer_size;
}

void WebSocketPeer::set_outbound_buffer_size(int bytes) {
    state_->outbound_buffer_size = bytes;
}

int WebSocketPeer::outbound_buffer_size() const {
    return state_->outbound_buffer_size;
}

void WebSocketPeer::set_max_queued_packets(int count) {
    state_->max_queued_packets = count;
}

int WebSocketPeer::max_queued_packets() const {
    return state_->max_queued_packets;
}

void WebSocketPeer::set_heartbeat_interval(double seconds) {
    state_->heartbeat_interval_msec = static_cast<std::uint64_t>(std::max(seconds, 0.0) * 1000.0);
}

// ----------------------------------------------------------------------------------------------
// WebSocketMultiplayerPeer, as Godot's.

struct WebSocketMultiplayerPeer::State {
    struct PendingPeer {
        std::uint64_t time = 0;
        std::unique_ptr<WebSocketPeer> ws;
    };
    struct Packet {
        int source = 0;
        std::vector<std::uint8_t> bytes;
    };

    WebSocketPeer config; // the settings new connections take
    ConnectionStatus status = ConnectionStatus::Disconnected;
    int unique_id = 0;
    int target_peer = 0;
    std::uint64_t handshake_timeout = 3000;
    std::map<int, std::unique_ptr<WebSocketPeer>> peers;
    std::map<int, PendingPeer> pending;
    std::deque<Packet> incoming;
    Packet current;
#if SLASH_NET_WEBSOCKET
    std::unique_ptr<TcpServer> server;
#endif

    [[nodiscard]] bool is_server() const {
#if SLASH_NET_WEBSOCKET
        return server != nullptr;
#else
        return false;
#endif
    }

    [[nodiscard]] std::unique_ptr<WebSocketPeer> create_peer() const {
        auto peer = std::make_unique<WebSocketPeer>();
        peer->set_supported_protocols(config.supported_protocols());
        peer->set_handshake_headers(config.handshake_headers());
        peer->set_inbound_buffer_size(config.inbound_buffer_size());
        peer->set_outbound_buffer_size(config.outbound_buffer_size());
        peer->set_max_queued_packets(config.max_queued_packets());
        return peer;
    }

    void clear() {
        status = ConnectionStatus::Disconnected;
        unique_id = 0;
        peers.clear();
#if SLASH_NET_WEBSOCKET
        server.reset();
#endif
        pending.clear();
        incoming.clear();
        current = {};
    }

    void take_packets(int source, WebSocketPeer& ws) {
        int count = ws.available_packet_count();
        while (count > 0 && ws.ready_state() == WebSocketState::Open) {
            auto packet = ws.get_packet();
            if (!packet || packet.value().empty()) {
                break;
            }
            incoming.push_back({source, {packet.value().begin(), packet.value().end()}});
            --count;
        }
    }
};

WebSocketMultiplayerPeer::WebSocketMultiplayerPeer() : state_(std::make_unique<State>()) {}

WebSocketMultiplayerPeer::~WebSocketMultiplayerPeer() = default;

Result<void> WebSocketMultiplayerPeer::create_server(int port, std::string_view bind_address) {
#if SLASH_NET_WEBSOCKET
    State& s = *state_;
    if (s.status != ConnectionStatus::Disconnected) {
        return make_error(ErrorCode::InvalidArgument, "the peer is already in use");
    }
    s.clear();
    s.server = std::make_unique<TcpServer>();
    if (auto listening = s.server->listen(port, bind_address); !listening) {
        s.server.reset();
        return listening;
    }
    s.unique_id = 1;
    s.status = ConnectionStatus::Connected;
    return {};
#else
    (void)port;
    (void)bind_address;
    return make_error(ErrorCode::InvalidArgument,
                      "this build can't host a WebSocket server; browsers only connect to them");
#endif
}

Result<void> WebSocketMultiplayerPeer::create_client(std::string_view url) {
    State& s = *state_;
    if (s.status != ConnectionStatus::Disconnected) {
        return make_error(ErrorCode::InvalidArgument, "the peer is already in use");
    }
    s.clear();
    std::unique_ptr<WebSocketPeer> peer = s.create_peer();
    if (auto connected = peer->connect_to_url(url); !connected) {
        return connected;
    }
    s.pending[1] = State::PendingPeer{.time = ticks_msec(), .ws = nullptr};
    s.peers[1] = std::move(peer);
    s.status = ConnectionStatus::Connecting;
    return {};
}

int WebSocketMultiplayerPeer::local_port() const {
#if SLASH_NET_WEBSOCKET
    return state_->server ? state_->server->port() : 0;
#else
    return 0;
#endif
}

WebSocketPeer* WebSocketMultiplayerPeer::peer(int id) const {
    const auto found = state_->peers.find(id);
    return found != state_->peers.end() ? found->second.get() : nullptr;
}

void WebSocketMultiplayerPeer::set_handshake_timeout(double seconds) {
    if (seconds <= 0.0) {
        log::error("the WebSocket handshake timeout must be above 0");
        return;
    }
    state_->handshake_timeout = static_cast<std::uint64_t>(seconds * 1000.0);
}

void WebSocketMultiplayerPeer::set_supported_protocols(std::vector<std::string> protocols) {
    state_->config.set_supported_protocols(std::move(protocols));
}

void WebSocketMultiplayerPeer::set_handshake_headers(std::vector<std::string> headers) {
    state_->config.set_handshake_headers(std::move(headers));
}

void WebSocketMultiplayerPeer::set_inbound_buffer_size(int bytes) {
    state_->config.set_inbound_buffer_size(bytes);
}

void WebSocketMultiplayerPeer::set_outbound_buffer_size(int bytes) {
    state_->config.set_outbound_buffer_size(bytes);
}

void WebSocketMultiplayerPeer::set_max_queued_packets(int count) {
    state_->config.set_max_queued_packets(count);
}

void WebSocketMultiplayerPeer::set_target_peer(int peer) {
    state_->target_peer = peer;
}

int WebSocketMultiplayerPeer::packet_peer() const {
    return state_->incoming.empty() ? 1 : state_->incoming.front().source;
}

void WebSocketMultiplayerPeer::disconnect_peer(int peer, bool force) {
    State& s = *state_;
    const auto found = s.peers.find(peer);
    if (found == s.peers.end()) {
        return;
    }
    found->second->close();
    if (force) {
        s.peers.erase(found);
        if (!is_server()) {
            s.clear();
        }
    }
}

bool WebSocketMultiplayerPeer::is_server() const {
    return state_->is_server();
}

void WebSocketMultiplayerPeer::poll() {
    State& s = *state_;
    if (s.status == ConnectionStatus::Disconnected) {
        return;
    }
    if (!s.is_server()) {
        // As _poll_client.
        WebSocketPeer& ws = *s.peers[1];
        ws.poll();
        if (ws.ready_state() == WebSocketState::Open) {
            if (s.status == ConnectionStatus::Connecting) {
                if (ws.available_packet_count() == 0) {
                    return; // still waiting for an ID
                }
                auto id = ws.get_packet();
                std::int32_t got = 0;
                if (id && id.value().size() == 4) {
                    std::memcpy(&got, id.value().data(), 4);
                }
                if (got < 2) {
                    log::error("a WebSocket server sent an invalid peer ID");
                    ws.close(-1);
                    s.clear();
                    return;
                }
                s.unique_id = got;
                s.status = ConnectionStatus::Connected;
                s.pending.clear();
                peer_connected(1);
            }
            s.take_packets(1, ws);
        } else if (ws.ready_state() == WebSocketState::Closed) {
            if (s.status == ConnectionStatus::Connected) {
                peer_disconnected(1);
            }
            s.clear();
            return;
        }
        if (s.status == ConnectionStatus::Connecting &&
            ticks_msec() - s.pending[1].time > s.handshake_timeout) {
            log::warn("a WebSocket handshake timed out",
                      {{"seconds", static_cast<double>(s.handshake_timeout) / 1000.0}});
            s.clear();
        }
        return;
    }

#if SLASH_NET_WEBSOCKET
    // As _poll_server: take new connections.
    if (!is_refusing_new_connections()) {
        const ENetSocket socket = s.server->take_connection();
        if (socket != ENET_SOCKET_NULL) {
            auto stream = std::make_unique<TcpStream>();
            stream->accept(socket);
            auto ws = s.create_peer();
            ws->state_->accept_stream(std::move(stream));
            int id = 0;
            do {
                id = static_cast<int>(generate_unique_id());
            } while (s.pending.contains(id) || s.peers.contains(id));
            s.pending[id] = State::PendingPeer{.time = ticks_msec(), .ws = std::move(ws)};
        }
    }
    // Pending peers.
    std::set<int> to_remove;
    for (auto& [id, peer] : s.pending) {
        if (ticks_msec() - peer.time > s.handshake_timeout) {
            log::warn("a WebSocket handshake timed out",
                      {{"seconds", static_cast<double>(s.handshake_timeout) / 1000.0}});
            to_remove.insert(id);
            continue;
        }
        peer.ws->poll();
        const WebSocketState state = peer.ws->ready_state();
        if (state == WebSocketState::Open) {
            to_remove.insert(id);
            if (is_refusing_new_connections()) {
                continue; // dropped
            }
            const std::int32_t peer_id = id;
            std::array<std::uint8_t, 4> bytes{};
            std::memcpy(bytes.data(), &peer_id, 4);
            if (peer.ws->put_packet(bytes)) {
                s.peers[id] = std::move(peer.ws);
                peer_connected(id);
            } else {
                log::error("couldn't send a new WebSocket peer its ID");
            }
        } else if (state != WebSocketState::Connecting) {
            to_remove.insert(id); // an error
        }
    }
    for (const int id : to_remove) {
        s.pending.erase(id);
    }
    to_remove.clear();
    // Connected peers.
    for (auto& [id, ws] : s.peers) {
        ws->poll();
        if (ws->ready_state() != WebSocketState::Open) {
            to_remove.insert(id);
            continue;
        }
        s.take_packets(id, *ws);
    }
    for (const int id : to_remove) {
        peer_disconnected(id);
        s.peers.erase(id);
    }
#endif
}

void WebSocketMultiplayerPeer::close() {
    state_->clear();
}

int WebSocketMultiplayerPeer::unique_id() const {
    return state_->unique_id;
}

ConnectionStatus WebSocketMultiplayerPeer::connection_status() const {
    return state_->status;
}

int WebSocketMultiplayerPeer::available_packet_count() const {
    return static_cast<int>(state_->incoming.size());
}

Result<std::span<const std::uint8_t>> WebSocketMultiplayerPeer::get_packet() {
    State& s = *state_;
    if (s.status != ConnectionStatus::Connected) {
        return make_error(ErrorCode::InvalidArgument, "the peer isn't connected");
    }
    if (s.incoming.empty()) {
        return make_error(ErrorCode::NotFound, "no packet is waiting");
    }
    s.current = std::move(s.incoming.front());
    s.incoming.pop_front();
    return std::span<const std::uint8_t>(s.current.bytes);
}

Result<void> WebSocketMultiplayerPeer::put_packet(std::span<const std::uint8_t> packet) {
    State& s = *state_;
    if (s.status != ConnectionStatus::Connected) {
        return make_error(ErrorCode::InvalidArgument, "the peer isn't connected");
    }
    if (!is_server()) {
        return s.peers[1]->put_packet(packet);
    }
    if (s.target_peer > 0) {
        const auto found = s.peers.find(s.target_peer);
        if (found == s.peers.end()) {
            return make_error(ErrorCode::NotFound, "no peer " + std::to_string(s.target_peer));
        }
        return found->second->put_packet(packet);
    }
    for (auto& [id, ws] : s.peers) {
        if (s.target_peer != 0 && -s.target_peer == id) {
            continue; // excluded
        }
        (void)ws->put_packet(packet);
    }
    return {};
}

int WebSocketMultiplayerPeer::max_packet_size() const {
    return state_->config.outbound_buffer_size() - kProtoSize;
}

} // namespace slash::net
