#pragma once

#include "slash/core/json.hpp"
#include "slash/core/result.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace slash::command {
class Session;
} // namespace slash::command

// A game's link to Autopilot (github.com/ramenvr/autopilot, Apache 2.0), the agent that plays a
// game to explore it for bugs, verify a claim about it or reach a goal in it. Autopilot's agent
// service (`autopilot serve`) listens on a WebSocket; engine plugins connect out to it, say
// hello and answer its requests: what the player sees, where things are, keys to press and
// controls to click. This is SlashEngine's plugin, after Autopilot's Godot addon
// (plugin/godot/addons/autopilot), whose protocol it speaks: its hello says "engine": "godot",
// since SlashEngine follows Godot's scene model, and its answers are in Autopilot's "kit" frame
// (Unreal's axes, centimeters) as the addon's are. Objects are entity IDs, rc reads and writes
// components ("Transform3D/position") and calls commands, and the codex is the project's
// scenes, prefabs, project.json and gameplay sources. See docs/reference/autopilot.md.
namespace slash::autopilot {

// The protocol version Autopilot's agent service speaks.
inline constexpr int kProtocol = 1;
// Where the agent service listens unless told otherwise, as the Godot addon's DEFAULT_URL.
inline constexpr std::string_view kDefaultUrl = "ws://127.0.0.1:8765/v1/bridge";

// Autopilot's settings folder: Autopilot/ under %LOCALAPPDATA% on Windows, ~/Library/Application
// Support on macOS and $XDG_CONFIG_HOME (or ~/.config) elsewhere; "" where none can be told.
[[nodiscard]] std::string settings_folder();
// The agent service's address: given, else AUTOPILOT_PLAYTEST_URL, else the one a running
// `autopilot serve` announces in brain.json in the settings folder (or the Aura app's, in
// aura-brain.json), else kDefaultUrl. Only addresses on this machine are taken from the files.
[[nodiscard]] std::string find_url(std::string_view given = {});
// The bearer token to send: given, else AUTOPILOT_PLAYTEST_TOKEN, else "" (none).
[[nodiscard]] std::string find_token(std::string_view given = {});
// Whether the player shares runs' data with Autopilot's developers: preferences.json's
// share_data in the settings folder, else false (SlashEngine never asked them).
[[nodiscard]] bool default_share();

// The connection to the agent service, as messages of JSON text.
class Transport {
public:
    enum class State : std::uint8_t { Connecting, Open, Closed };

    Transport() = default;
    virtual ~Transport() = default;
    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;

    // Takes in what arrived and sends what waits.
    virtual void poll() = 0;
    [[nodiscard]] virtual State state() const = 0;
    // The next message that arrived, or nullopt.
    virtual std::optional<std::string> receive() = 0;
    virtual bool send(std::string_view text) = 0;
    // Where it connects, for messages.
    [[nodiscard]] virtual std::string address() const = 0;
};

// A WebSocket to url, sending "Authorization: Bearer <token>" when token isn't empty.
[[nodiscard]] Result<std::unique_ptr<Transport>> connect_websocket(std::string_view url,
                                                                   std::string_view token);

// Makes a new connection each time the bridge connects or reconnects.
using Connector = std::function<Result<std::unique_ptr<Transport>>()>;
// A connector of WebSockets to find_url(url) and find_token(token), looked up again at every
// attempt, as an agent service that restarts may listen on another port.
[[nodiscard]] Connector websocket_connector(std::string url = {}, std::string token = {});

struct Options {
    // The run to ask for in the hello, as the Godot addon's command line asks: {"kind":
    // "verify", "claim"}, {"kind": "goal", "goal"}, {"kind": "explore", "minutes", "brief"} or
    // {"kind": "auto", "prompt"}, each with an optional "time_limit_minutes". Null: the game
    // only connects, and the agent service (or an agent through `autopilot mcp`) starts runs.
    json::Value run;
    // Where result.json and the report files the agent service sends go; "" for
    // .slash/autopilot/<time>/ in the project.
    std::string report_dir;
    // Seconds to wait for the agent service's welcome before a run asked for fails.
    double connect_timeout = 120.0;
    // Keep the game running and connected after the run's result (or a failed connection)
    // instead of finishing, as the addon's --playtest-stay-open.
    bool stay_open = false;
    // Whether runs' data is shared with Autopilot's developers (hello's "share").
    bool share = false;
    // Whether the game's picture can be recorded and captured (hello's "media" feature): drawn
    // on the software device (canvas::capture), so headless games have one too.
    bool media = true;
    // Seconds since some fixed moment, for key leases, heartbeats and timeouts; a steady clock
    // when empty. Tests give their own.
    std::function<double()> clock;
};

// What the bridge does with each tick. Make one, then call poll before each tick. It connects
// with connect, and again with a backoff (1 second doubling to 15) when a connection fails or
// closes, while it is still wanted.
class Bridge {
public:
    Bridge(command::Session& session, Connector connect, Options options);
    ~Bridge();
    Bridge(const Bridge&) = delete;
    Bridge& operator=(const Bridge&) = delete;

    // Before each tick: takes the agent service's frames, answers its requests, moves held keys
    // and answers that wait on ticks on, and sends events (heartbeats, findings, log lines).
    // tick_seconds is the tick about to run.
    void poll(double tick_seconds);

    // Whether the agent service has welcomed the connection open now.
    [[nodiscard]] bool welcomed() const;
    // Whether the run asked for is over (its result came, it failed or its connection closed):
    // the game should stop with exit_code.
    [[nodiscard]] bool finished() const;
    // 0 confirmed, achieved or explored without bugs; 2 denied or bugs found; 3 inconclusive;
    // 1 an error. The result's own exit_code when it has one.
    [[nodiscard]] int exit_code() const;
    // The folder result.json was written to, once it was.
    [[nodiscard]] const std::string& report_dir() const;
    // The last run's result frame's "result", or null.
    [[nodiscard]] const json::Value& result() const;
    // What went wrong with the connection last, in words; "" when nothing did.
    [[nodiscard]] const std::string& problem() const;

    struct State;

private:
    std::unique_ptr<State> state_;
};

} // namespace slash::autopilot
