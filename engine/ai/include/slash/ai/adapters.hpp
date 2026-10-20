#pragma once

#include "slash/ai/provider.hpp"
#include "slash/ai/transport.hpp"
#include "slash/core/result.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The adapters: each turns a Request into one vendor's HTTP API and its reply back into a
// Response, streaming included. "openai" speaks OpenAI's chat completions, which Ollama,
// llama.cpp's server, vLLM, LM Studio and OpenRouter serve too; "anthropic" speaks Anthropic's
// Messages API; "gemini" speaks Google's Gemini API.
namespace slash::ai {

inline constexpr std::array<std::string_view, 3> kAdapterTypes = {"anthropic", "gemini", "openai"};

struct AdapterOptions {
    std::string url; // the API's base URL; "" for the vendor's own
    std::string model;
    std::string api_key; // "" sends none, as local servers expect
    std::vector<std::pair<std::string, std::string>> headers; // sent with every request
    Capabilities capabilities;
    // Sent when a request sets no max_tokens. Anthropic needs one, so it sends 16000 by default.
    std::optional<std::int64_t> max_tokens;
    // The name an OpenAI-compatible server takes max_tokens under: "max_tokens", or
    // "max_completion_tokens" for OpenAI's reasoning models.
    std::string max_tokens_param = "max_tokens";
    int timeout_seconds = 600;
    // Retries after a 408, 409, 429 or 5xx reply, waiting as Retry-After says or 1, 2, 4...
    // seconds, until the reply has started streaming.
    int max_retries = 2;
    // null for http_transport().
    std::shared_ptr<Transport> transport;
};

// The vendor's base URL for an adapter type: "https://api.openai.com/v1",
// "https://api.anthropic.com" or "https://generativelanguage.googleapis.com/v1beta".
[[nodiscard]] std::string_view default_url(std::string_view type) noexcept;

// An adapter of the given type; an unknown type fails with the closest one as a hint.
[[nodiscard]] Result<std::shared_ptr<Provider>> make_adapter(std::string_view type,
                                                             AdapterOptions options);

} // namespace slash::ai
