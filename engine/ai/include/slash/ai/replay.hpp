#pragma once

#include "slash/ai/provider.hpp"
#include "slash/core/json.hpp"
#include "slash/core/result.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace slash::ai {

enum class ReplayMode : std::uint8_t {
    Replay, // answer only from the file; a request it has no recording of fails
    Record, // call the model every time and record its reply, replacing an earlier one
    Auto,   // answer from the file when it can, otherwise call the model and record the reply
};

[[nodiscard]] std::string_view to_string(ReplayMode mode) noexcept;
[[nodiscard]] std::optional<ReplayMode> replay_mode_from_string(std::string_view name) noexcept;

// Answers requests from a file of recorded replies, so tests that use a model run offline, with
// no key, and get the same reply every time. Recordings are keyed by the model and the request,
// so changing either records anew. The file is JSON lines, one recording per line sorted by key:
// {"chunks": [...], "key": ..., "request": {...}, "response": {...}}, with "chunks" holding the
// pieces a streamed reply arrived in, which replaying streams again.
class ReplayProvider : public Provider {
public:
    struct Options {
        std::string path;  // the file; read on the first request, created by the first recording
        std::string model; // who answers, such as "openai/llama3.2"; part of every key
        ReplayMode mode = ReplayMode::Auto;
        // Called to record; null to only replay.
        std::shared_ptr<Provider> inner{}; // NOLINT(readability-redundant-member-init)
        bool write = true;                 // false records nothing, as sessions that run tests do
    };

    explicit ReplayProvider(Options options);

    [[nodiscard]] const Capabilities& capabilities() const override;
    [[nodiscard]] Result<Response> complete(const Request& request,
                                            const StreamSink& sink = nullptr,
                                            const CancelFlag* cancel = nullptr) override;
    void start(const Request& request, StreamSink sink, ReplyCallback done,
               const CancelFlag* cancel = nullptr) override;

    // A request's key: 16 hex digits hashing the model and the request's canonical JSON.
    [[nodiscard]] static std::string key(std::string_view model, const Request& request);

private:
    struct Recording {
        json::Value request;
        json::Value response;
        std::vector<std::string> chunks;
    };

    Result<void> load();
    Result<void> save() const;
    // The recorded reply, streamed to the sink, or an error; nullopt if the model must answer.
    [[nodiscard]] std::optional<Result<Response>> answer(const std::string& request_key,
                                                         const StreamSink& sink);
    // Records a reply the model gave in the pieces it streamed in, and returns it.
    [[nodiscard]] Result<Response> record(const std::string& request_key, const Request& request,
                                          Result<Response> response,
                                          std::vector<std::string> chunks);

    Options options_;
    Capabilities capabilities_;
    std::mutex mutex_;
    bool loaded_ = false;
    std::map<std::string, Recording, std::less<>> recordings_;
};

} // namespace slash::ai
