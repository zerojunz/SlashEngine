#include "slash/ai/replay.hpp"

#include "slash/platform/file.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slash::ai {
namespace {

constexpr std::array kModes{ReplayMode::Replay, ReplayMode::Record, ReplayMode::Auto};

// FNV-1a, 64 bits: stable on every platform, which std::hash is not.
std::uint64_t fnv1a(std::string_view text) {
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (const char c : text) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

std::string hex(std::uint64_t value) {
    constexpr std::string_view kDigits = "0123456789abcdef";
    std::string out(16, '0');
    for (std::size_t i = 16; i-- > 0;) {
        out[i] = kDigits[value & 0xF];
        value >>= 4;
    }
    return out;
}

std::string parent_folder(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

} // namespace

std::string_view to_string(ReplayMode mode) noexcept {
    switch (mode) {
    case ReplayMode::Replay:
        return "replay";
    case ReplayMode::Record:
        return "record";
    case ReplayMode::Auto:
        return "auto";
    }
    return "auto";
}

std::optional<ReplayMode> replay_mode_from_string(std::string_view name) noexcept {
    for (const ReplayMode mode : kModes) {
        if (to_string(mode) == name) {
            return mode;
        }
    }
    return std::nullopt;
}

ReplayProvider::ReplayProvider(Options options) : options_(std::move(options)) {
    if (options_.inner != nullptr) {
        capabilities_ = options_.inner->capabilities();
    }
}

const Capabilities& ReplayProvider::capabilities() const {
    return capabilities_;
}

std::string ReplayProvider::key(std::string_view model, const Request& request) {
    return hex(
        fnv1a(std::string(model) + '\n' + json::write(to_json(request), json::Style::Compact)));
}

Result<void> ReplayProvider::load() {
    if (loaded_) {
        return {};
    }
    if (!platform::exists(options_.path)) {
        loaded_ = true;
        return {};
    }
    auto text = platform::read_file(options_.path);
    if (!text) {
        return text.error();
    }
    std::size_t start = 0;
    std::size_t number = 1;
    const std::string& contents = text.value();
    while (start < contents.size()) {
        std::size_t end = contents.find('\n', start);
        if (end == std::string::npos) {
            end = contents.size();
        }
        const std::string_view line = std::string_view(contents).substr(start, end - start);
        start = end + 1;
        if (line.find_first_not_of(" \t\r") == std::string_view::npos) {
            ++number;
            continue;
        }
        auto value = json::parse(line);
        const json::Value* key = value ? value.value().find("key") : nullptr;
        if (key == nullptr || !key->is_string()) {
            return make_error(ErrorCode::InvalidArgument,
                              options_.path + " line " + std::to_string(number) +
                                  ": not a recording; each line is {\"key\", \"request\", "
                                  "\"response\"}");
        }
        Recording recording;
        if (const json::Value* request = value.value().find("request"); request != nullptr) {
            recording.request = *request;
        }
        if (const json::Value* response = value.value().find("response"); response != nullptr) {
            recording.response = *response;
        }
        if (const json::Value* chunks = value.value().find("chunks");
            chunks != nullptr && chunks->is_array()) {
            for (const json::Value& chunk : chunks->as_array()) {
                if (chunk.is_string()) {
                    recording.chunks.push_back(chunk.as_string());
                }
            }
        }
        recordings_[key->as_string()] = std::move(recording);
        ++number;
    }
    loaded_ = true;
    return {};
}

Result<void> ReplayProvider::save() const {
    std::string text;
    for (const auto& [key, recording] : recordings_) {
        json::Value line;
        line["key"] = key;
        line["request"] = recording.request;
        line["response"] = recording.response;
        for (const std::string& chunk : recording.chunks) {
            line["chunks"].push_back(chunk);
        }
        text += json::write(line, json::Style::Compact) + "\n";
    }
    if (const std::string folder = parent_folder(options_.path); !folder.empty()) {
        if (auto made = platform::create_directories(folder); !made) {
            return made;
        }
    }
    return platform::write_file(options_.path, text);
}

std::optional<Result<Response>> ReplayProvider::answer(const std::string& request_key,
                                                       const StreamSink& sink) {
    const std::scoped_lock lock(mutex_);
    if (auto loaded = load(); !loaded) {
        return Result<Response>(loaded.error());
    }
    if (options_.mode != ReplayMode::Record) {
        if (const auto found = recordings_.find(request_key); found != recordings_.end()) {
            auto response = response_from_json(found->second.response);
            if (!response) {
                return Result<Response>(make_error(ErrorCode::InvalidArgument,
                                                   options_.path + ", recording " + request_key +
                                                       ": " + response.error().message));
            }
            if (sink) {
                if (found->second.chunks.empty()) {
                    sink(response.value().message.text);
                }
                for (const std::string& chunk : found->second.chunks) {
                    sink(chunk);
                }
            }
            response.value().replayed = true;
            return response;
        }
        if (options_.mode == ReplayMode::Replay || options_.inner == nullptr) {
            return Result<Response>(
                make_error(ErrorCode::NotFound,
                           options_.path + " has no recording of this request (key " + request_key +
                               "); record it by calling the model with the replay mode "
                               "\"record\" or \"auto\""));
        }
    }
    if (options_.inner == nullptr) {
        return Result<Response>(make_error(ErrorCode::NotFound, "there is no provider to record " +
                                                                    options_.path + " from"));
    }
    return std::nullopt;
}

Result<Response> ReplayProvider::record(const std::string& request_key, const Request& request,
                                        Result<Response> response,
                                        std::vector<std::string> chunks) {
    if (!response || !options_.write) {
        return response;
    }
    const std::scoped_lock lock(mutex_);
    Recording& recording = recordings_[request_key];
    recording.request = to_json(request);
    recording.response = to_json(response.value());
    recording.chunks = std::move(chunks);
    if (auto saved = save(); !saved) {
        return saved.error();
    }
    return response;
}

Result<Response> ReplayProvider::complete(const Request& request, const StreamSink& sink,
                                          const CancelFlag* cancel) {
    const std::string request_key = key(options_.model, request);
    if (auto answered = answer(request_key, sink)) {
        return std::move(*answered);
    }
    // The model is called outside the lock, so recordings of other requests can run alongside.
    std::vector<std::string> chunks;
    StreamSink recorder;
    if (sink) {
        recorder = [&](std::string_view text) {
            chunks.emplace_back(text);
            sink(text);
        };
    }
    auto response = options_.inner->complete(request, recorder, cancel);
    return record(request_key, request, std::move(response), std::move(chunks));
}

void ReplayProvider::start(const Request& request, StreamSink sink, ReplyCallback done,
                           const CancelFlag* cancel) {
    auto self = std::static_pointer_cast<ReplayProvider>(weak_from_this().lock());
    if (self == nullptr) {
        Provider::start(request, std::move(sink), std::move(done), cancel);
        return;
    }
    std::string request_key = key(options_.model, request);
    if (auto answered = answer(request_key, sink)) {
        done(std::move(*answered));
        return;
    }
    auto chunks = std::make_shared<std::vector<std::string>>();
    StreamSink recorder;
    if (sink) {
        recorder = [chunks, sink = std::move(sink)](std::string_view text) {
            chunks->emplace_back(text);
            sink(text);
        };
    }
    options_.inner->start(
        request, std::move(recorder),
        [self, request_key = std::move(request_key), request, chunks,
         done = std::move(done)](Result<Response> response) {
            done(self->record(request_key, request, std::move(response), std::move(*chunks)));
        },
        cancel);
}

} // namespace slash::ai
