// The web's WebSocket: the browser's (Node's, in tests/web/run.mjs) against the echo server that
// run.mjs starts and names in SLASH_WS_ECHO.
#include "slash/net/websocket.hpp"

#include <doctest/doctest.h>

#include <cstdlib>
#include <emscripten.h>
#include <functional>
#include <span>
#include <string>
#include <string_view>

using namespace slash;

namespace {

// Polls until done says so, sleeping between polls so the browser's events arrive.
bool poll_until(net::WebSocketPeer& peer, const std::function<bool()>& done) {
    for (int i = 0; i < 5000; ++i) {
        peer.poll();
        if (done()) {
            return true;
        }
        emscripten_sleep(1);
    }
    return false;
}

std::span<const std::uint8_t> bytes_of(std::string_view text) {
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

} // namespace

TEST_CASE("websocket: the browser's connects, echoes and closes") {
    const char* echo = std::getenv("SLASH_WS_ECHO");
    if (echo == nullptr) {
        MESSAGE("SLASH_WS_ECHO isn't set; run the tests through tests/web/run.mjs");
        return;
    }
    CHECK_FALSE(net::websocket_server_supported());
    net::WebSocketPeer peer;
    peer.set_supported_protocols({"echo"});
    REQUIRE(peer.connect_to_url(std::string(echo) + "/web"));
    CHECK(peer.ready_state() == net::WebSocketState::Connecting);
    REQUIRE(poll_until(peer, [&] { return peer.ready_state() == net::WebSocketState::Open; }));
    CHECK(peer.selected_protocol() == "echo");
    CHECK(peer.requested_url().ends_with("/web"));

    REQUIRE(peer.send_text("hello"));
    REQUIRE(peer.put_packet(bytes_of("bytes")));
    REQUIRE(poll_until(peer, [&] { return peer.available_packet_count() == 2; }));
    const auto text = peer.get_packet().value();
    CHECK(std::string(text.begin(), text.end()) == "hello");
    CHECK(peer.was_string_packet());
    const auto binary = peer.get_packet().value();
    CHECK(std::string(binary.begin(), binary.end()) == "bytes");
    CHECK_FALSE(peer.was_string_packet());

    peer.close(4000, "done");
    CHECK(peer.ready_state() == net::WebSocketState::Closing);
    REQUIRE(poll_until(peer, [&] { return peer.ready_state() == net::WebSocketState::Closed; }));
    CHECK(peer.close_code() == 4000);
    CHECK(peer.close_reason() == "done");
}
