#include "adapters.hpp"

#include "slash/ai/transport.hpp"
#include "slash/core/json.hpp"
#include "slash/core/sha256.hpp"
#include "slash/core/text.hpp"
#include "slash/platform/process.hpp"
#include "slash/render/image.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace slash::library::detail {
namespace {

// Raw speech from OpenAI's API: 16-bit mono samples at 24 kHz, with no header.
constexpr int kSpeechSampleRate = 24000;
// Stable Diffusion draws badly below this, so smaller images are made larger, then fitted.
constexpr int kDiffusionMinSide = 512;
constexpr int kDiffusionMaxSide = 2048;
constexpr double kDenoisingStrength = 0.6;
// ElevenLabs makes sound effects from half a second to thirty.
constexpr double kMinSoundSeconds = 0.5;
constexpr double kMaxSoundSeconds = 30.0;
// ElevenLabs makes music from 3 seconds to 10 minutes, Stable Audio 2 from 1 second to 190; it
// makes 190 unless asked, so music services are asked for 30 when the request says nothing.
constexpr double kMinMusicSeconds = 3.0;
constexpr double kMaxElevenLabsMusicSeconds = 600.0;
constexpr double kMaxStableAudioSeconds = 190.0;
constexpr double kDefaultMusicSeconds = 30.0;

std::string_view default_url(std::string_view type) noexcept {
    if (type == "openai-image" || type == "openai-speech") {
        return "https://api.openai.com/v1";
    }
    if (type == "gemini-image") {
        return "https://generativelanguage.googleapis.com/v1beta";
    }
    if (type == "elevenlabs-sound" || type == "elevenlabs-music") {
        return "https://api.elevenlabs.io";
    }
    if (type == "stability-audio" || type == "stability-3d") {
        return "https://api.stability.ai";
    }
    return "http://127.0.0.1:7860"; // automatic1111
}

// What a providers.json entry says about reaching its service.
struct Service {
    std::string type;
    std::string url; // without a trailing '/'
    std::string model;
    std::string api_key;
    std::string missing_key; // the variable api_key_env names, if it is not set
    std::vector<std::pair<std::string, std::string>> headers;
    json::Value options; // an object, or null
    int timeout_seconds = 600;
    int max_retries = 2;
    std::shared_ptr<ai::Transport> transport;
};

// The options merged into a request body: objects member by member, anything else replaced.
void merge(json::Value& into, const json::Value& from) {
    if (!from.is_object()) {
        return;
    }
    if (!into.is_object()) {
        into = json::Object{};
    }
    for (const auto& [key, value] : from.as_object()) {
        json::Value* existing = into.find(key);
        if (existing != nullptr && existing->is_object() && value.is_object()) {
            merge(*existing, value);
        } else {
            into[key] = value;
        }
    }
}

struct Size {
    int width = 0;
    int height = 0;

    [[nodiscard]] bool empty() const noexcept { return width <= 0 || height <= 0; }
};

// The size to fit an image to: the request's (one side given makes a square), else a refined
// image's, else none.
Size wanted(const GenerateRequest& request) {
    Size size{request.width, request.height};
    if (size.width == 0 && size.height == 0 && request.parent != nullptr) {
        size = {request.parent->width, request.parent->height};
    }
    if (size.width == 0) {
        size.width = size.height;
    }
    if (size.height == 0) {
        size.height = size.width;
    }
    return size;
}

// The parent's bytes as a PNG file, as every image API takes.
Result<std::string> parent_png(const GenerateRequest& request) {
    if (request.parent != nullptr && request.parent->format == "png") {
        return request.parent_data;
    }
    auto image = render::decode_image(request.parent_data);
    if (!image) {
        return image.error();
    }
    return render::encode_png(image.value());
}

// A made image as a PNG file at the size asked for.
Result<std::string> fitted_png(std::string bytes, const GenerateRequest& request,
                               std::string_view who) {
    auto image = render::decode_image(bytes);
    if (!image) {
        return make_error(ErrorCode::PlatformFailure, std::string(who) +
                                                          " returned an image the library can't "
                                                          "read: " +
                                                          image.error().message);
    }
    const Size size = wanted(request);
    const bool same =
        size.empty() || (image.value().width == static_cast<std::uint32_t>(size.width) &&
                         image.value().height == static_cast<std::uint32_t>(size.height));
    if (same && bytes.starts_with("\x89PNG")) {
        return bytes;
    }
    return render::encode_png(same ? image.value()
                                   : fit_image(image.value(), size.width, size.height));
}

void put_u16(std::string& out, std::uint32_t value) {
    out += static_cast<char>(value & 0xFFU);
    out += static_cast<char>((value >> 8U) & 0xFFU);
}

void put_u32(std::string& out, std::uint32_t value) {
    put_u16(out, value & 0xFFFFU);
    put_u16(out, value >> 16U);
}

// 16-bit little-endian samples as a .wav file.
std::string wav_file(std::string_view samples, int sample_rate, int channels) {
    const auto rate = static_cast<std::uint32_t>(sample_rate);
    const auto count = static_cast<std::uint32_t>(channels);
    const auto data_bytes = static_cast<std::uint32_t>(samples.size() & ~std::size_t{1});
    std::string out = "RIFF";
    put_u32(out, 36 + data_bytes);
    out += "WAVEfmt ";
    put_u32(out, 16);
    put_u16(out, 1); // PCM
    put_u16(out, count);
    put_u32(out, rate);
    put_u32(out, rate * count * 2);
    put_u16(out, count * 2);
    put_u16(out, 16);
    out += "data";
    put_u32(out, data_bytes);
    out.append(samples.substr(0, data_bytes));
    return out;
}

// "wav", "ogg" or "mp3" from a sound file's first bytes, or "" for none of them.
std::string_view sound_format(std::string_view bytes) {
    if (bytes.size() >= 12 && bytes.starts_with("RIFF") && bytes.substr(8, 4) == "WAVE") {
        return "wav";
    }
    if (bytes.starts_with("OggS")) {
        return "ogg";
    }
    const auto byte = [&bytes](std::size_t at) {
        return at < bytes.size() ? static_cast<unsigned char>(bytes[at]) : 0U;
    };
    if (bytes.starts_with("ID3") || (byte(0) == 0xFFU && (byte(1) & 0xE0U) == 0xE0U)) {
        return "mp3";
    }
    return "";
}

Result<json::Value> reply_json(const ai::HttpResponse& reply, std::string_view who) {
    auto body = json::parse(reply.body);
    if (!body || !body.value().is_object()) {
        return make_error(ErrorCode::PlatformFailure, std::string(who) + "'s reply is not JSON");
    }
    return std::move(body).value();
}

std::string text_or_empty(const json::Value& value, std::string_view key) {
    const json::Value* member = value.is_object() ? value.find(key) : nullptr;
    return member != nullptr && member->is_string() ? member->as_string() : std::string();
}

// A multipart/form-data body, for APIs that take files that way.
class Multipart {
public:
    // The boundary hashes the file, so it never appears in it.
    explicit Multipart(std::string_view file)
        : boundary_("slash-" + sha256_hex(file).substr(0, 32)) {}

    void field(std::string_view name, std::string_view value) {
        start(name);
        body_ += "\r\n\r\n";
        body_ += value;
        body_ += "\r\n";
    }

    void file(std::string_view name, std::string_view filename, std::string_view type,
              std::string_view data) {
        start(name);
        body_ += "; filename=\"" + std::string(filename) +
                 "\"\r\nContent-Type: " + std::string(type) + "\r\n\r\n";
        body_ += data;
        body_ += "\r\n";
    }

    [[nodiscard]] std::string content_type() const {
        return "multipart/form-data; boundary=" + boundary_;
    }

    [[nodiscard]] std::string finish() {
        body_ += "--" + boundary_ + "--\r\n";
        return std::move(body_);
    }

private:
    void start(std::string_view name) {
        body_ += "--" + boundary_ + "\r\nContent-Disposition: form-data; name=\"" +
                 std::string(name) + "\"";
    }

    std::string boundary_;
    std::string body_;
};

class HttpGenerator : public Generator {
public:
    HttpGenerator(Service service, GeneratorCapabilities capabilities)
        : service_(std::move(service)), capabilities_(capabilities),
          who_("the " + service_.type + " generator") {}

    [[nodiscard]] const GeneratorCapabilities& capabilities() const final { return capabilities_; }

    [[nodiscard]] Result<Generated> generate(const GenerateRequest& request,
                                             const ai::CancelFlag* cancel) final {
        if (!service_.missing_key.empty()) {
            return make_error(ErrorCode::PermissionDenied, who_ + " reads its key from " +
                                                               service_.missing_key +
                                                               ", which is not set");
        }
        auto built = build(request);
        if (!built) {
            return built.error();
        }
        ai::HttpRequest& http = built.value();
        http.timeout_seconds = service_.timeout_seconds;
        http.headers.insert(http.headers.end(), service_.headers.begin(), service_.headers.end());
        auto reply =
            ai::post_with_retries(*service_.transport, http, who_, service_.max_retries, cancel);
        if (!reply) {
            return reply.error();
        }
        return read(request, reply.value());
    }

protected:
    [[nodiscard]] virtual Result<ai::HttpRequest> build(const GenerateRequest& request) const = 0;
    [[nodiscard]] virtual Result<Generated> read(const GenerateRequest& request,
                                                 const ai::HttpResponse& reply) const = 0;

    [[nodiscard]] const Service& service() const noexcept { return service_; }
    [[nodiscard]] const std::string& who() const noexcept { return who_; }
    [[nodiscard]] std::string url(std::string_view path) const {
        return service_.url + std::string(path);
    }

    // The body with the options merged in.
    [[nodiscard]] json::Value with_options(json::Value body) const {
        merge(body, service_.options);
        return body;
    }

    [[nodiscard]] ai::HttpRequest json_request(std::string_view path,
                                               const json::Value& body) const {
        ai::HttpRequest http;
        http.url = url(path);
        http.body = json::write(body, json::Style::Compact);
        return http;
    }

    void bearer(ai::HttpRequest& http) const {
        if (!service_.api_key.empty()) {
            http.headers.emplace_back("Authorization", "Bearer " + service_.api_key);
        }
    }

private:
    Service service_;
    GeneratorCapabilities capabilities_;
    std::string who_;
};

// OpenAI's Images API: generations, and edits for a refine. Servers that copy it, such as LocalAI,
// take it too.
class OpenAiImage final : public HttpGenerator {
public:
    explicit OpenAiImage(Service service)
        : HttpGenerator(std::move(service), {.images = true,
                                             .sounds = false,
                                             .refines_images = true,
                                             .refines_sounds = false,
                                             .music = false,
                                             .refines_music = false}) {}

protected:
    [[nodiscard]] Result<ai::HttpRequest> build(const GenerateRequest& request) const override {
        const std::string& model = service().model;
        const bool gpt_image = !model.starts_with("dall-e");
        const std::string size = size_for(model, wanted(request));
        if (request.parent == nullptr) {
            json::Value body = json::Object{};
            body["model"] = model;
            body["prompt"] = request.prompt;
            body["n"] = 1;
            if (!size.empty()) {
                body["size"] = size;
            }
            body = with_options(std::move(body));
            // The library keeps PNG files, whatever the options say.
            if (gpt_image) {
                body["output_format"] = "png";
            } else {
                body["response_format"] = "b64_json";
            }
            ai::HttpRequest http = json_request("/images/generations", body);
            bearer(http);
            return http;
        }
        auto image = parent_png(request);
        if (!image) {
            return image.error();
        }
        json::Value fields = json::Object{};
        fields["model"] = model;
        fields["prompt"] = request.prompt;
        fields["n"] = 1;
        if (!size.empty()) {
            fields["size"] = size;
        }
        fields = with_options(std::move(fields));
        if (gpt_image) {
            fields["output_format"] = "png";
        } else {
            fields["response_format"] = "b64_json";
        }
        Multipart form(image.value());
        for (const auto& [name, value] : fields.as_object()) {
            form.field(name, value.is_string() ? value.as_string()
                                               : json::write(value, json::Style::Compact));
        }
        form.file("image", "image.png", "image/png", image.value());
        ai::HttpRequest http;
        http.url = url("/images/edits");
        http.content_type = form.content_type();
        http.body = form.finish();
        bearer(http);
        return http;
    }

    [[nodiscard]] Result<Generated> read(const GenerateRequest& request,
                                         const ai::HttpResponse& reply) const override {
        auto body = reply_json(reply, who());
        if (!body) {
            return body.error();
        }
        std::string encoded;
        if (const json::Value* data = body.value().find("data");
            data != nullptr && data->is_array() && !data->as_array().empty()) {
            encoded = text_or_empty(data->as_array().front(), "b64_json");
        }
        auto bytes = base64_decode(encoded);
        if (encoded.empty() || !bytes) {
            return make_error(ErrorCode::PlatformFailure,
                              who() + "'s reply has no image in data[0].b64_json");
        }
        auto png = fitted_png(std::move(*bytes), request, who());
        if (!png) {
            return png.error();
        }
        return Generated{.data = std::move(png).value(),
                         .format = "png",
                         .model = service().model,
                         .seed = std::nullopt};
    }

private:
    // The size the model makes closest to the shape asked for, or "" to let it choose.
    static std::string size_for(std::string_view model, Size size) {
        if (size.empty()) {
            return {};
        }
        if (model.starts_with("dall-e-2")) {
            const int side = std::max(size.width, size.height);
            if (side <= 256) {
                return "256x256";
            }
            return side <= 512 ? "512x512" : "1024x1024";
        }
        const bool dalle = model.starts_with("dall-e");
        if (std::int64_t{size.width} * 4 >= std::int64_t{size.height} * 5) {
            return dalle ? "1792x1024" : "1536x1024";
        }
        if (std::int64_t{size.height} * 4 >= std::int64_t{size.width} * 5) {
            return dalle ? "1024x1792" : "1024x1536";
        }
        return "1024x1024";
    }
};

// Gemini's image models through generateContent: the prompt, and the image to change for a
// refine, in; images out.
class GeminiImage final : public HttpGenerator {
public:
    explicit GeminiImage(Service service)
        : HttpGenerator(std::move(service), {.images = true,
                                             .sounds = false,
                                             .refines_images = true,
                                             .refines_sounds = false,
                                             .music = false,
                                             .refines_music = false}) {}

protected:
    [[nodiscard]] Result<ai::HttpRequest> build(const GenerateRequest& request) const override {
        json::Array parts;
        json::Value text = json::Object{};
        text["text"] = request.prompt;
        parts.push_back(std::move(text));
        if (request.parent != nullptr) {
            auto image = parent_png(request);
            if (!image) {
                return image.error();
            }
            json::Value inline_data = json::Object{};
            inline_data["mime_type"] = "image/png";
            inline_data["data"] = base64_encode(image.value());
            json::Value part = json::Object{};
            part["inline_data"] = std::move(inline_data);
            parts.push_back(std::move(part));
        }
        json::Value content = json::Object{};
        content["role"] = "user";
        content["parts"] = std::move(parts);
        json::Value config = json::Object{};
        config["responseModalities"] = json::Array{"TEXT", "IMAGE"};
        if (const Size size = wanted(request); !size.empty()) {
            config["imageConfig"]["aspectRatio"] = aspect_ratio(size);
        }
        if (request.seed) {
            config["seed"] = *request.seed;
        }
        json::Value body = json::Object{};
        body["contents"] = json::Array{std::move(content)};
        body["generationConfig"] = std::move(config);
        ai::HttpRequest http = json_request("/models/" + service().model + ":generateContent",
                                            with_options(std::move(body)));
        if (!service().api_key.empty()) {
            http.headers.emplace_back("x-goog-api-key", service().api_key);
        }
        return http;
    }

    [[nodiscard]] Result<Generated> read(const GenerateRequest& request,
                                         const ai::HttpResponse& reply) const override {
        auto body = reply_json(reply, who());
        if (!body) {
            return body.error();
        }
        std::string encoded;
        std::string said;
        std::string finish;
        if (const json::Value* candidates = body.value().find("candidates");
            candidates != nullptr && candidates->is_array() && !candidates->as_array().empty()) {
            const json::Value& candidate = candidates->as_array().front();
            finish = text_or_empty(candidate, "finishReason");
            static const json::Array kNoParts;
            const json::Value* content = candidate.find("content");
            const json::Value* parts = content != nullptr ? content->find("parts") : nullptr;
            for (const json::Value& part :
                 parts != nullptr && parts->is_array() ? parts->as_array() : kNoParts) {
                const json::Value* data = part.find("inlineData");
                if (data == nullptr) {
                    data = part.find("inline_data");
                }
                if (data != nullptr && encoded.empty()) {
                    encoded = text_or_empty(*data, "data");
                }
                said += text_or_empty(part, "text");
            }
        }
        if (encoded.empty()) {
            std::string message = who() + " returned no image";
            if (const json::Value* feedback = body.value().find("promptFeedback");
                feedback != nullptr && !text_or_empty(*feedback, "blockReason").empty()) {
                message += " (blocked: " + text_or_empty(*feedback, "blockReason") + ")";
            } else if (!finish.empty() && finish != "STOP") {
                message += " (" + finish + ")";
            }
            if (!said.empty()) {
                message += ": " + said.substr(0, 300);
            }
            return make_error(ErrorCode::PlatformFailure, std::move(message));
        }
        auto bytes = base64_decode(encoded);
        if (!bytes) {
            return make_error(ErrorCode::PlatformFailure, who() + "'s image is not base64");
        }
        auto png = fitted_png(std::move(*bytes), request, who());
        if (!png) {
            return png.error();
        }
        std::string model = text_or_empty(body.value(), "modelVersion");
        if (model.empty()) {
            model = service().model;
        }
        return Generated{.data = std::move(png).value(),
                         .format = "png",
                         .model = std::move(model),
                         .seed = request.seed};
    }

private:
    // The ratio Gemini takes closest to the size's: the one the size's differs from by the
    // smallest factor.
    static std::string aspect_ratio(Size size) {
        static constexpr std::array<std::array<int, 2>, 10> kRatios = {
            {{1, 1}, {2, 3}, {3, 2}, {3, 4}, {4, 3}, {4, 5}, {5, 4}, {9, 16}, {16, 9}, {21, 9}}};
        std::array<int, 2> best{};
        std::int64_t best_high = 0;
        std::int64_t best_low = 0;
        for (const auto& ratio : kRatios) {
            const std::int64_t a = std::int64_t{size.width} * ratio[1];
            const std::int64_t b = std::int64_t{size.height} * ratio[0];
            const std::int64_t high = std::max(a, b);
            const std::int64_t low = std::min(a, b);
            // high / low < best_high / best_low, without dividing.
            if (best_high == 0 || high * best_low < best_high * low) {
                best = ratio;
                best_high = high;
                best_low = low;
            }
        }
        return std::to_string(best[0]) + ":" + std::to_string(best[1]);
    }
};

// A Stable Diffusion web UI's API (AUTOMATIC1111 and the forks that keep it): txt2img, and
// img2img for a refine.
class Automatic1111 final : public HttpGenerator {
public:
    explicit Automatic1111(Service service)
        : HttpGenerator(std::move(service), {.images = true,
                                             .sounds = false,
                                             .refines_images = true,
                                             .refines_sounds = false,
                                             .music = false,
                                             .refines_music = false}) {}

protected:
    [[nodiscard]] Result<ai::HttpRequest> build(const GenerateRequest& request) const override {
        json::Value body = json::Object{};
        body["prompt"] = request.prompt;
        body["seed"] = request.seed.value_or(-1);
        body["batch_size"] = 1;
        body["n_iter"] = 1;
        if (const Size size = drawn(wanted(request)); !size.empty()) {
            body["width"] = size.width;
            body["height"] = size.height;
        }
        if (!service().model.empty()) {
            body["override_settings"]["sd_model_checkpoint"] = service().model;
        }
        std::string_view path = "/sdapi/v1/txt2img";
        if (request.parent != nullptr) {
            auto image = parent_png(request);
            if (!image) {
                return image.error();
            }
            body["init_images"] = json::Array{base64_encode(image.value())};
            body["denoising_strength"] = kDenoisingStrength;
            path = "/sdapi/v1/img2img";
        }
        ai::HttpRequest http = json_request(path, with_options(std::move(body)));
        if (!service().api_key.empty()) {
            // The web UI's --api-auth takes user:password.
            http.headers.emplace_back("Authorization", "Basic " + base64_encode(service().api_key));
        }
        return http;
    }

    [[nodiscard]] Result<Generated> read(const GenerateRequest& request,
                                         const ai::HttpResponse& reply) const override {
        auto body = reply_json(reply, who());
        if (!body) {
            return body.error();
        }
        std::string encoded;
        if (const json::Value* images = body.value().find("images");
            images != nullptr && images->is_array() && !images->as_array().empty() &&
            images->as_array().front().is_string()) {
            encoded = images->as_array().front().as_string();
        }
        auto bytes = base64_decode(encoded);
        if (encoded.empty() || !bytes) {
            return make_error(ErrorCode::PlatformFailure, who() + "'s reply has no image");
        }
        auto png = fitted_png(std::move(*bytes), request, who());
        if (!png) {
            return png.error();
        }
        Generated out{.data = std::move(png).value(),
                      .format = "png",
                      .model = service().model,
                      .seed = request.seed};
        // info is JSON in a string, with the seed it drew with and the checkpoint.
        if (auto info = json::parse(text_or_empty(body.value(), "info"));
            info && info.value().is_object()) {
            if (const json::Value* seed = info.value().find("seed");
                seed != nullptr && seed->is_int()) {
                out.seed = seed->as_int();
            }
            if (std::string model = text_or_empty(info.value(), "sd_model_name");
                out.model.empty() && !model.empty()) {
                out.model = std::move(model);
            }
        }
        return out;
    }

private:
    // The size to draw at: the shape asked for, doubled until its short side is at least 512 and
    // halved until its long side is at most 2048, in multiples of 8.
    static Size drawn(Size size) {
        if (size.empty()) {
            return size;
        }
        while (std::min(size.width, size.height) < kDiffusionMinSide &&
               std::max(size.width, size.height) * 2 <= kDiffusionMaxSide) {
            size.width *= 2;
            size.height *= 2;
        }
        while (std::max(size.width, size.height) > kDiffusionMaxSide) {
            size.width = std::max(1, size.width / 2);
            size.height = std::max(1, size.height / 2);
        }
        size.width = std::max(8, (size.width + 7) / 8 * 8);
        size.height = std::max(8, (size.height + 7) / 8 * 8);
        return size;
    }
};

// OpenAI's text to speech: the prompt is the line spoken; options choose the voice and say how
// to speak it ("instructions").
class OpenAiSpeech final : public HttpGenerator {
public:
    explicit OpenAiSpeech(Service service)
        : HttpGenerator(std::move(service), {.images = false,
                                             .sounds = true,
                                             .refines_images = false,
                                             .refines_sounds = false,
                                             .music = false,
                                             .refines_music = false}) {}

protected:
    [[nodiscard]] Result<ai::HttpRequest> build(const GenerateRequest& request) const override {
        json::Value body = json::Object{};
        body["model"] = service().model;
        body["input"] = request.prompt;
        body["voice"] = "alloy";
        body["response_format"] = "pcm";
        ai::HttpRequest http = json_request("/audio/speech", with_options(std::move(body)));
        bearer(http);
        return http;
    }

    [[nodiscard]] Result<Generated> read(const GenerateRequest& /*request*/,
                                         const ai::HttpResponse& reply) const override {
        Generated out{.data = {}, .format = {}, .model = service().model, .seed = std::nullopt};
        const std::string_view format = sound_format(reply.body);
        if (!format.empty()) {
            out.data = reply.body;
            out.format = std::string(format);
            return out;
        }
        const json::Value* asked =
            service().options.is_object() ? service().options.find("response_format") : nullptr;
        if (asked != nullptr && !(asked->is_string() && asked->as_string() == "pcm")) {
            return make_error(ErrorCode::PlatformFailure,
                              who() + " returned audio the library can't read; set "
                                      "response_format to wav, mp3 or pcm");
        }
        out.data = wav_file(reply.body, kSpeechSampleRate, 1);
        out.format = "wav";
        return out;
    }
};

// The output_format options name for ElevenLabs, which goes in the URL: mp3_44100_128 if none.
std::string elevenlabs_format(const json::Value& options) {
    const json::Value* format = options.is_object() ? options.find("output_format") : nullptr;
    return format != nullptr && format->is_string() ? format->as_string() : "mp3_44100_128";
}

// The audio an ElevenLabs reply holds: raw pcm_ samples at the format's rate made a .wav file,
// anything else kept as the file it is.
Result<Generated> elevenlabs_audio(const ai::HttpResponse& reply, const std::string& format,
                                   int channels, const std::string& model, const std::string& who) {
    Generated out{.data = {}, .format = {}, .model = model, .seed = std::nullopt};
    if (format.starts_with("pcm_")) {
        int rate = 0;
        const std::string_view digits = std::string_view(format).substr(4);
        const auto [end, error] =
            std::from_chars(digits.data(), digits.data() + digits.size(), rate);
        if (error != std::errc() || end != digits.data() + digits.size() || rate <= 0) {
            return make_error(ErrorCode::InvalidArgument,
                              who + "'s output_format \"" + format + "\" names no rate");
        }
        out.data = wav_file(reply.body, rate, channels);
        out.format = "wav";
        return out;
    }
    const std::string_view sniffed = sound_format(reply.body);
    if (sniffed.empty()) {
        return make_error(ErrorCode::PlatformFailure,
                          who + " returned audio the library can't read; use an mp3_, wav_ or "
                                "pcm_ output_format");
    }
    out.data = reply.body;
    out.format = std::string(sniffed);
    return out;
}

// What a music request asks for beyond its prompt, said in words for services that take only a
// prompt: its tempo, meter and key, and that it loops.
std::string music_prompt(const GenerateRequest& request) {
    std::string prompt = request.prompt;
    std::string details;
    if (request.bpm > 0.0) {
        std::string bpm = std::to_string(request.bpm);
        while (bpm.back() == '0') {
            bpm.pop_back();
        }
        if (bpm.back() == '.') {
            bpm.pop_back();
        }
        details += " Tempo: " + bpm + " BPM.";
    }
    if (request.bar_beats > 0) {
        details += " Meter: " + std::to_string(request.bar_beats) + "/4.";
    }
    if (!request.key.empty()) {
        details += " Key: " + request.key + ".";
    }
    if (request.loop) {
        details += " It loops seamlessly: its end leads straight back into its start.";
    }
    if (!details.empty()) {
        prompt += prompt.empty() ? details.substr(1) : "\n\n" + details.substr(1);
    }
    return prompt;
}

// ElevenLabs' sound effects. options' output_format (mp3_, wav_ or pcm_ formats) goes in the
// URL; the rest in the body.
class ElevenLabsSound final : public HttpGenerator {
public:
    explicit ElevenLabsSound(Service service)
        : HttpGenerator(std::move(service), {.images = false,
                                             .sounds = true,
                                             .refines_images = false,
                                             .refines_sounds = false,
                                             .music = false,
                                             .refines_music = false}) {}

protected:
    [[nodiscard]] Result<ai::HttpRequest> build(const GenerateRequest& request) const override {
        json::Value body = json::Object{};
        body["text"] = request.prompt;
        body["model_id"] = service().model;
        if (request.seconds > 0.0) {
            body["duration_seconds"] =
                std::clamp(request.seconds, kMinSoundSeconds, kMaxSoundSeconds);
        }
        body = with_options(std::move(body));
        body.as_object().erase("output_format");
        ai::HttpRequest http;
        http.url =
            url("/v1/sound-generation?output_format=" + elevenlabs_format(service().options));
        http.body = json::write(body, json::Style::Compact);
        if (!service().api_key.empty()) {
            http.headers.emplace_back("xi-api-key", service().api_key);
        }
        return http;
    }

    [[nodiscard]] Result<Generated> read(const GenerateRequest& /*request*/,
                                         const ai::HttpResponse& reply) const override {
        return elevenlabs_audio(reply, elevenlabs_format(service().options), 1, service().model,
                                who());
    }
};

// ElevenLabs' music: the prompt with the tempo, key and loop asked for in words, instrumental
// unless options say force_instrumental false. output_format goes in the URL, as for sounds.
class ElevenLabsMusic final : public HttpGenerator {
public:
    explicit ElevenLabsMusic(Service service)
        : HttpGenerator(std::move(service), {.images = false,
                                             .sounds = false,
                                             .refines_images = false,
                                             .refines_sounds = false,
                                             .music = true,
                                             .refines_music = false}) {}

protected:
    [[nodiscard]] Result<ai::HttpRequest> build(const GenerateRequest& request) const override {
        json::Value body = json::Object{};
        body["prompt"] = music_prompt(request);
        body["model_id"] = service().model;
        body["force_instrumental"] = true;
        const double seconds = request.seconds > 0.0 ? request.seconds : kDefaultMusicSeconds;
        body["music_length_ms"] = static_cast<std::int64_t>(std::lround(
            std::clamp(seconds, kMinMusicSeconds, kMaxElevenLabsMusicSeconds) * 1000.0));
        body = with_options(std::move(body));
        body.as_object().erase("output_format");
        ai::HttpRequest http;
        http.url = url("/v1/music?output_format=" + elevenlabs_format(service().options));
        http.body = json::write(body, json::Style::Compact);
        if (!service().api_key.empty()) {
            http.headers.emplace_back("xi-api-key", service().api_key);
        }
        return http;
    }

    [[nodiscard]] Result<Generated> read(const GenerateRequest& /*request*/,
                                         const ai::HttpResponse& reply) const override {
        return elevenlabs_audio(reply, elevenlabs_format(service().options), 2, service().model,
                                who());
    }
};

// Stability AI's Stable Audio 2 and 2.5, which take a form and return the audio: the prompt with
// the tempo, key and loop in words, the length and the seed. options (steps, cfg_scale,
// output_format: mp3 or wav) go in the form too.
class StabilityAudio final : public HttpGenerator {
public:
    explicit StabilityAudio(Service service)
        : HttpGenerator(std::move(service), {.images = false,
                                             .sounds = false,
                                             .refines_images = false,
                                             .refines_sounds = false,
                                             .music = true,
                                             .refines_music = false}) {}

protected:
    [[nodiscard]] Result<ai::HttpRequest> build(const GenerateRequest& request) const override {
        const std::string prompt = music_prompt(request);
        Multipart form(prompt);
        form.field("prompt", prompt);
        form.field("model", service().model);
        const double seconds = request.seconds > 0.0 ? request.seconds : kDefaultMusicSeconds;
        form.field("duration", std::to_string(static_cast<int>(
                                   std::lround(std::clamp(seconds, 1.0, kMaxStableAudioSeconds)))));
        if (request.seed) {
            form.field("seed", std::to_string(*request.seed));
        }
        bool format_given = false;
        if (service().options.is_object()) {
            for (const auto& [name, value] : service().options.as_object()) {
                format_given = format_given || name == "output_format";
                form.field(name, value.is_string() ? value.as_string()
                                                   : json::write(value, json::Style::Compact));
            }
        }
        if (!format_given) {
            form.field("output_format", "mp3");
        }
        ai::HttpRequest http;
        http.url = url("/v2beta/audio/stable-audio-2/text-to-audio");
        http.content_type = form.content_type();
        http.headers.emplace_back("Accept", "audio/*");
        bearer(http);
        http.body = form.finish();
        return http;
    }

    [[nodiscard]] Result<Generated> read(const GenerateRequest& request,
                                         const ai::HttpResponse& reply) const override {
        const std::string_view format = sound_format(reply.body);
        if (format.empty()) {
            return make_error(ErrorCode::PlatformFailure,
                              who() + " returned audio the library can't read; set output_format "
                                      "to mp3 or wav");
        }
        return Generated{.data = reply.body,
                         .format = std::string(format),
                         .model = service().model,
                         .seed = request.seed};
    }
};

// Stability AI's image to 3D: Stable Fast 3D (the default) or Stable Point Aware 3D, which take
// the concept image as a form and answer with a .glb file. The prompt isn't sent: the image is
// the request.
class StabilityModel final : public HttpGenerator {
public:
    explicit StabilityModel(Service service)
        : HttpGenerator(std::move(service), {.images = false,
                                             .sounds = false,
                                             .refines_images = false,
                                             .refines_sounds = false,
                                             .music = false,
                                             .refines_music = false,
                                             .models = true}) {}

protected:
    [[nodiscard]] Result<ai::HttpRequest> build(const GenerateRequest& request) const override {
        if (request.image_data.empty()) {
            return make_error(ErrorCode::InvalidArgument,
                              who() + " makes a model from a concept image, and none was given");
        }
        const std::string_view data = request.image_data;
        std::string_view format;
        if (data.starts_with("\x89PNG")) {
            format = "png";
        } else if (data.starts_with("\xFF\xD8")) {
            format = "jpeg";
        } else if (data.size() >= 12 && data.starts_with("RIFF") && data.substr(8, 4) == "WEBP") {
            format = "webp";
        } else {
            return make_error(ErrorCode::InvalidArgument,
                              who() + " takes a PNG, JPEG or WebP concept image");
        }
        Multipart form(data);
        form.file("image", "concept." + std::string(format), "image/" + std::string(format), data);
        // Stable Point Aware 3D takes a seed; Stable Fast 3D has none.
        if (request.seed && service().model == "stable-point-aware-3d") {
            form.field("seed", std::to_string(*request.seed));
        }
        if (service().options.is_object()) {
            for (const auto& [name, value] : service().options.as_object()) {
                form.field(name, value.is_string() ? value.as_string()
                                                   : json::write(value, json::Style::Compact));
            }
        }
        ai::HttpRequest http;
        http.url = url("/v2beta/3d/" + service().model);
        http.content_type = form.content_type();
        bearer(http);
        http.body = form.finish();
        return http;
    }

    [[nodiscard]] Result<Generated> read(const GenerateRequest& request,
                                         const ai::HttpResponse& reply) const override {
        if (!reply.body.starts_with("glTF")) {
            return make_error(ErrorCode::PlatformFailure,
                              who() + " returned something other than a .glb model");
        }
        return Generated{.data = reply.body,
                         .format = "glb",
                         .model = service().model,
                         .seed = service().model == "stable-point-aware-3d" ? request.seed
                                                                            : std::nullopt};
    }
};

// Per output pixel along one axis, the source pixels it covers and by how much, in units of
// 1/to of a source pixel: output o spans [o * from, (o + 1) * from).
std::vector<std::vector<std::pair<std::uint32_t, std::uint64_t>>>
coverage(std::uint64_t from, std::uint64_t to, std::uint64_t offset) {
    std::vector<std::vector<std::pair<std::uint32_t, std::uint64_t>>> spans(to);
    for (std::uint64_t o = 0; o < to; ++o) {
        const std::uint64_t start = o * from;
        const std::uint64_t end = start + from;
        for (std::uint64_t i = start / to; i * to < end; ++i) {
            const std::uint64_t weight = std::min(end, (i + 1) * to) - std::max(start, i * to);
            if (weight > 0) {
                spans[o].emplace_back(static_cast<std::uint32_t>(offset + i), weight);
            }
        }
    }
    return spans;
}

} // namespace

std::string_view default_model(std::string_view type) noexcept {
    if (type == "openai-image") {
        return "gpt-image-1";
    }
    if (type == "openai-speech") {
        return "gpt-4o-mini-tts";
    }
    if (type == "gemini-image") {
        return "gemini-2.5-flash-image";
    }
    if (type == "elevenlabs-sound") {
        return "eleven_text_to_sound_v2";
    }
    if (type == "elevenlabs-music") {
        return "music_v1";
    }
    if (type == "stability-audio") {
        return "stable-audio-2.5";
    }
    if (type == "stability-3d") {
        return "stable-fast-3d";
    }
    return ""; // automatic1111 draws with the checkpoint it has loaded
}

render::Image fit_image(const render::Image& image, int width, int height) {
    if (width <= 0 || height <= 0 || image.width == 0 || image.height == 0 ||
        (image.width == static_cast<std::uint32_t>(width) &&
         image.height == static_cast<std::uint32_t>(height))) {
        return image;
    }
    const auto out_width = static_cast<std::uint64_t>(width);
    const auto out_height = static_cast<std::uint64_t>(height);
    // The middle of the image at the size's aspect ratio.
    std::uint64_t crop_width = image.width;
    std::uint64_t crop_height = image.height;
    if (crop_width * out_height > crop_height * out_width) {
        crop_width =
            std::max<std::uint64_t>(1, (crop_height * out_width + out_height / 2) / out_height);
    } else if (crop_width * out_height < crop_height * out_width) {
        crop_height =
            std::max<std::uint64_t>(1, (crop_width * out_height + out_width / 2) / out_width);
    }
    const auto columns = coverage(crop_width, out_width, (image.width - crop_width) / 2);
    const auto rows = coverage(crop_height, out_height, (image.height - crop_height) / 2);
    const std::uint64_t total = crop_width * crop_height;

    render::Image out;
    out.width = static_cast<std::uint32_t>(width);
    out.height = static_cast<std::uint32_t>(height);
    out.pixels.resize(out_width * out_height * 4);
    for (std::uint64_t y = 0; y < out_height; ++y) {
        for (std::uint64_t x = 0; x < out_width; ++x) {
            std::uint64_t alpha = 0;
            std::array<std::uint64_t, 3> color{};
            for (const auto& [row, row_weight] : rows[y]) {
                for (const auto& [column, column_weight] : columns[x]) {
                    const std::uint64_t weight = row_weight * column_weight;
                    const std::size_t at = (std::size_t{row} * image.width + column) * 4;
                    const std::uint64_t a = image.pixels[at + 3];
                    alpha += a * weight;
                    for (std::size_t c = 0; c < 3; ++c) {
                        color.at(c) += std::uint64_t{image.pixels[at + c]} * a * weight;
                    }
                }
            }
            const std::size_t at = (y * out_width + x) * 4;
            for (std::size_t c = 0; c < 3; ++c) {
                out.pixels[at + c] =
                    alpha == 0 ? 0 : static_cast<std::uint8_t>((color.at(c) + alpha / 2) / alpha);
            }
            out.pixels[at + 3] = static_cast<std::uint8_t>((alpha + total / 2) / total);
        }
    }
    return out;
}

Result<std::shared_ptr<Generator>> make_http_generator(const ai::GeneratorConfig& config,
                                                       const GeneratorOptions& options) {
    Service service;
    service.type = config.type;
    service.url = config.url.empty() ? std::string(default_url(config.type)) : config.url;
    while (!service.url.empty() && service.url.back() == '/') {
        service.url.pop_back();
    }
    service.model = config.model.empty() ? std::string(default_model(config.type)) : config.model;
    if (!config.api_key_env.empty()) {
        service.api_key = options.environment ? options.environment(config.api_key_env)
                                              : platform::environment_variable(config.api_key_env);
        if (service.api_key.empty()) {
            service.missing_key = config.api_key_env;
        }
    }
    service.headers.assign(config.headers.begin(), config.headers.end());
    service.options = config.options;
    service.timeout_seconds = config.timeout_seconds;
    service.max_retries = config.max_retries;
    service.transport = options.transport != nullptr ? options.transport : ai::http_transport();

    if (config.type == "openai-image") {
        return std::shared_ptr<Generator>(std::make_shared<OpenAiImage>(std::move(service)));
    }
    if (config.type == "gemini-image") {
        return std::shared_ptr<Generator>(std::make_shared<GeminiImage>(std::move(service)));
    }
    if (config.type == "automatic1111") {
        return std::shared_ptr<Generator>(std::make_shared<Automatic1111>(std::move(service)));
    }
    if (config.type == "openai-speech") {
        return std::shared_ptr<Generator>(std::make_shared<OpenAiSpeech>(std::move(service)));
    }
    if (config.type == "elevenlabs-sound") {
        return std::shared_ptr<Generator>(std::make_shared<ElevenLabsSound>(std::move(service)));
    }
    if (config.type == "elevenlabs-music") {
        return std::shared_ptr<Generator>(std::make_shared<ElevenLabsMusic>(std::move(service)));
    }
    if (config.type == "stability-audio") {
        return std::shared_ptr<Generator>(std::make_shared<StabilityAudio>(std::move(service)));
    }
    if (config.type == "stability-3d") {
        return std::shared_ptr<Generator>(std::make_shared<StabilityModel>(std::move(service)));
    }
    return make_error(ErrorCode::InvalidArgument, "no generator of type \"" + config.type + "\"");
}

} // namespace slash::library::detail
