#pragma once

#include "slash/net/peer.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace slash::net {

// Whether this build can host WebSocket servers: every platform but the web, whose browsers
// only connect to them.
bool websocket_server_supported();

// A WebSocket connection's state, as Godot's WebSocketPeer::State.
enum class WebSocketState : std::uint8_t { Connecting, Open, Closing, Closed };

// How a message is sent: as UTF-8 text or as bytes.
enum class WriteMode : std::uint8_t { Text, Binary };

// One WebSocket connection, as Godot's WebSocketPeer: ws:// URLs over TCP with wslay's framing
// everywhere but the web, where the browser's WebSocket connects (to ws:// and wss:// URLs).
// poll() sends and receives; messages wait in order until get_packet() takes them. IPv4 only.
class WebSocketPeer {
public:
    WebSocketPeer();
    ~WebSocketPeer();
    WebSocketPeer(const WebSocketPeer&) = delete;
    WebSocketPeer& operator=(const WebSocketPeer&) = delete;

    // Starts connecting to a ws:// URL ("ws://host:port/path"); open once ready_state() says so.
    Result<void> connect_to_url(std::string_view url);
    // Takes in what arrived and sends what is waiting; call it once a frame or tick.
    void poll();
    [[nodiscard]] WebSocketState ready_state() const;

    Result<void> send(std::span<const std::uint8_t> message, WriteMode mode = WriteMode::Binary);
    Result<void> send_text(std::string_view text);
    // A binary message.
    Result<void> put_packet(std::span<const std::uint8_t> message);
    [[nodiscard]] int available_packet_count() const;
    // The next message, which stays valid until the next get_packet(), poll() or close().
    Result<std::span<const std::uint8_t>> get_packet();
    // Whether the message get_packet() last returned was text.
    [[nodiscard]] bool was_string_packet() const;
    // Bytes queued to send but not yet sent.
    [[nodiscard]] int current_outbound_buffered_amount() const;

    // Starts closing with a code and reason (closed once the other side answers); a code below
    // 0 closes at once without telling the other side.
    void close(int code = 1000, std::string_view reason = {});
    // The code and reason the connection closed with: -1 and "" until then, or if it closed
    // uncleanly.
    [[nodiscard]] int close_code() const;
    [[nodiscard]] const std::string& close_reason() const;
    [[nodiscard]] const std::string& requested_url() const;
    // The subprotocol the two sides agreed on, or "".
    [[nodiscard]] const std::string& selected_protocol() const;

    // Settings, as Godot's, taken when a connection starts: the subprotocols this side speaks,
    // extra handshake header lines ("Name: value"; not on the web), the largest message taken
    // in (65535 bytes) and the bytes and messages that may wait (65535 and 4096), and seconds
    // between pings that close the connection when unanswered (0: none; not on the web).
    void set_supported_protocols(std::vector<std::string> protocols);
    [[nodiscard]] const std::vector<std::string>& supported_protocols() const;
    void set_handshake_headers(std::vector<std::string> headers);
    [[nodiscard]] const std::vector<std::string>& handshake_headers() const;
    void set_inbound_buffer_size(int bytes);
    [[nodiscard]] int inbound_buffer_size() const;
    void set_outbound_buffer_size(int bytes);
    [[nodiscard]] int outbound_buffer_size() const;
    void set_max_queued_packets(int count);
    [[nodiscard]] int max_queued_packets() const;
    void set_heartbeat_interval(double seconds);

    struct State;

private:
    friend class WebSocketMultiplayerPeer;
    std::unique_ptr<State> state_;
};

// A Peer over WebSocket, as Godot's WebSocketMultiplayerPeer: a server listens on a TCP port
// and gives each client its ID in the first message, then every packet travels reliably and in
// order; clients reach each other through the server's relay. Web games connect as clients.
class WebSocketMultiplayerPeer final : public Peer {
public:
    WebSocketMultiplayerPeer();
    ~WebSocketMultiplayerPeer() override;

    // Listens on a port (0 picks a free one; local_port() says which) on an IPv4 address ("*"
    // for any).
    Result<void> create_server(int port, std::string_view bind_address = "*");
    // Connects to a server's ws:// URL; connected once poll() reports peer 1.
    Result<void> create_client(std::string_view url);
    [[nodiscard]] int local_port() const;
    // A connected peer's WebSocket (on a client, peer 1's), or null, as Godot's get_peer.
    [[nodiscard]] WebSocketPeer* peer(int id) const;

    // Seconds a joining peer may take to finish the handshake (3).
    void set_handshake_timeout(double seconds);
    // The settings each connection's WebSocketPeer starts with.
    void set_supported_protocols(std::vector<std::string> protocols);
    void set_handshake_headers(std::vector<std::string> headers);
    void set_inbound_buffer_size(int bytes);
    void set_outbound_buffer_size(int bytes);
    void set_max_queued_packets(int count);

    void set_target_peer(int peer) override;
    [[nodiscard]] int packet_peer() const override;
    [[nodiscard]] TransferMode packet_mode() const override { return TransferMode::Reliable; }
    [[nodiscard]] int packet_channel() const override { return 0; }
    void disconnect_peer(int peer, bool force = false) override;
    [[nodiscard]] bool is_server() const override;
    [[nodiscard]] bool is_server_relay_supported() const override { return true; }
    void poll() override;
    void close() override;
    [[nodiscard]] int unique_id() const override;
    [[nodiscard]] ConnectionStatus connection_status() const override;
    [[nodiscard]] int available_packet_count() const override;
    Result<std::span<const std::uint8_t>> get_packet() override;
    Result<void> put_packet(std::span<const std::uint8_t> packet) override;
    [[nodiscard]] int max_packet_size() const override;

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace slash::net
