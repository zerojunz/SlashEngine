#include "slash/ai/commands.hpp"
#include "slash/ai/config.hpp"
#include "slash/ai/transport.hpp"
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/core/result.hpp"
#include "slash/core/text.hpp"
#include "slash/library/commands.hpp"
#include "slash/library/generator.hpp"
#include "slash/library/library.hpp"
#include "slash/platform/file.hpp"
#include "slash/render/device.hpp"
#include "slash/render/image.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <httplib.h>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace json = slash::json;
namespace command = slash::command;
namespace library = slash::library;
namespace fs = std::filesystem;
using slash::ErrorCode;

namespace {

json::Value parse(std::string_view text) {
    auto value = json::parse(text);
    REQUIRE_MESSAGE(value.ok(), std::string(text));
    return std::move(value).value();
}

// Answers each request with the next reply, and keeps the requests.
class ScriptedTransport final : public slash::ai::Transport {
public:
    struct Reply {
        int status = 200;
        std::string body;
        std::string retry_after{}; // NOLINT(readability-redundant-member-init)
    };

    explicit ScriptedTransport(std::vector<Reply> replies) : replies_(std::move(replies)) {}

    slash::Result<slash::ai::HttpResponse> post(const slash::ai::HttpRequest& request,
                                                const slash::ai::BodySink& /*sink*/,
                                                const slash::ai::CancelFlag* /*cancel*/) override {
        requests.push_back(request);
        const Reply& reply = replies_[std::min(requests.size(), replies_.size()) - 1];
        return slash::ai::HttpResponse{
            .status = reply.status, .body = reply.body, .retry_after = reply.retry_after};
    }

    [[nodiscard]] json::Value body(std::size_t index = 0) const {
        return parse(requests.at(index).body);
    }

    [[nodiscard]] std::string header(std::string_view name, std::size_t index = 0) const {
        for (const auto& [key, value] : requests.at(index).headers) {
            if (key == name) {
                return value;
            }
        }
        return {};
    }

    std::vector<slash::ai::HttpRequest> requests;

private:
    std::vector<Reply> replies_;
};

// An image of one color, with a transparent right half when half is set, as a PNG file.
std::string png(std::uint32_t width, std::uint32_t height, std::array<std::uint8_t, 3> color,
                bool half = false) {
    slash::render::Image image;
    image.width = width;
    image.height = height;
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const bool clear = half && x >= width / 2;
            image.pixels.insert(image.pixels.end(), {color[0], color[1], color[2],
                                                     clear ? std::uint8_t{0} : std::uint8_t{255}});
        }
    }
    return slash::render::encode_png(image).value();
}

slash::render::Image decode(const std::string& bytes) {
    auto image = slash::render::decode_image(bytes);
    REQUIRE(image.ok());
    return std::move(image).value();
}

std::array<int, 4> first_pixel(const slash::render::Image& image) {
    return {image.pixels[0], image.pixels[1], image.pixels[2], image.pixels[3]};
}

// A generator of the providers.json entry, reaching the transport, with TEST_KEY set to "key".
library::OpenedGenerator open(std::string_view entry,
                              const std::shared_ptr<ScriptedTransport>& transport,
                              std::string_view type = "image") {
    auto config =
        slash::ai::parse_config(parse(R"({"generators": {"g": )" + std::string(entry) + "}}"));
    REQUIRE_MESSAGE(config.ok(), config.error().message);
    auto opened = library::open_generator(
        config.value(), "g", type,
        {.transport = transport, .environment = [](const std::string& name) {
             return name == "TEST_KEY" ? std::string("key") : std::string();
         }});
    REQUIRE_MESSAGE(opened.ok(), opened.error().message);
    return std::move(opened).value();
}

library::GenerateRequest request(std::string type, std::string prompt, int width = 0,
                                 int height = 0) {
    library::GenerateRequest out;
    out.type = std::move(type);
    out.prompt = std::move(prompt);
    out.width = width;
    out.height = height;
    return out;
}

std::string has_text(const std::string& body, std::string_view text) {
    return body.find(text) != std::string::npos ? std::string(text) : std::string("(missing)");
}

} // namespace

TEST_CASE("openai-image asks for the nearest size, fits the result and edits to refine") {
    const std::string made = png(64, 48, {200, 30, 40});
    auto transport = std::make_shared<ScriptedTransport>(
        std::vector<ScriptedTransport::Reply>{{200, R"({"created": 1, "data": [{"b64_json": ")" +
                                                        slash::base64_encode(made) + R"("}]})"}});
    const library::OpenedGenerator generator =
        open(R"({"type": "openai-image", "api_key_env": "TEST_KEY", "license": "OpenAI terms",
                 "options": {"quality": "low", "output_format": "jpeg"}})",
             transport);
    CHECK(generator.model == "gpt-image-1");
    CHECK(generator.license == "OpenAI terms");

    auto result = generator.generator->generate(request("image", "red slime", 16, 16));
    REQUIRE_MESSAGE(result.ok(), result.error().message);
    CHECK(transport->requests[0].url == "https://api.openai.com/v1/images/generations");
    CHECK(transport->requests[0].content_type == "application/json");
    CHECK(transport->header("Authorization") == "Bearer key");
    // The options are merged in, but the library keeps PNG files.
    CHECK(json::write(transport->body(), json::Style::Compact) ==
          R"({"model":"gpt-image-1","n":1,"output_format":"png","prompt":"red slime",)"
          R"("quality":"low","size":"1024x1024"})");
    CHECK(result.value().format == "png");
    CHECK(result.value().model == "gpt-image-1");
    const slash::render::Image image = decode(result.value().data);
    CHECK(image.width == 16);
    CHECK(image.height == 16);
    CHECK(first_pixel(image) == std::array<int, 4>{200, 30, 40, 255});

    // Wide and tall shapes ask for the sizes the model makes in that shape.
    (void)generator.generator->generate(request("image", "a banner", 64, 16));
    CHECK(transport->body(1)["size"].as_string() == "1536x1024");
    (void)generator.generator->generate(request("image", "a door", 16, 48));
    CHECK(transport->body(2)["size"].as_string() == "1024x1536");
    // With no size, the model chooses, and the image stays as it came.
    auto native = generator.generator->generate(request("image", "anything"));
    CHECK(transport->body(3).find("size") == nullptr);
    CHECK(decode(native.value().data).width == 64);

    // A refine sends the image to change as a file, with the prompt saying what to change.
    library::Record parent;
    parent.id = std::string(64, 'a');
    parent.format = "png";
    parent.width = 16;
    parent.height = 16;
    library::GenerateRequest refine = request("image", "make it blue");
    refine.parent = &parent;
    refine.parent_data = png(16, 16, {200, 30, 40});
    auto refined = generator.generator->generate(refine);
    REQUIRE(refined.ok());
    const slash::ai::HttpRequest& edit = transport->requests[4];
    CHECK(edit.url == "https://api.openai.com/v1/images/edits");
    CHECK(edit.content_type.starts_with("multipart/form-data; boundary=slash-"));
    CHECK(has_text(edit.body, "name=\"prompt\"\r\n\r\nmake it blue\r\n") ==
          "name=\"prompt\"\r\n\r\nmake it blue\r\n");
    CHECK(has_text(edit.body, "name=\"output_format\"\r\n\r\npng\r\n") ==
          "name=\"output_format\"\r\n\r\npng\r\n");
    CHECK(
        has_text(edit.body, "name=\"image\"; filename=\"image.png\"\r\nContent-Type: image/png") ==
        "name=\"image\"; filename=\"image.png\"\r\nContent-Type: image/png");
    // Fitted to the parent's size when the refine asks for none.
    CHECK(decode(refined.value().data).width == 16);

    // DALL-E 3 makes other sizes and must be asked for base64.
    auto dalle = std::make_shared<ScriptedTransport>(std::vector<ScriptedTransport::Reply>{
        {200, R"({"data": [{"b64_json": ")" + slash::base64_encode(made) + R"("}]})"}});
    (void)open(R"({"type": "openai-image", "model": "dall-e-3"})", dalle)
        .generator->generate(request("image", "a banner", 64, 16));
    CHECK(dalle->body()["size"].as_string() == "1792x1024");
    CHECK(dalle->body()["response_format"].as_string() == "b64_json");
    CHECK(dalle->header("Authorization").empty());
}

TEST_CASE("gemini-image picks the nearest aspect ratio and reads inline images") {
    const std::string made = png(32, 18, {20, 160, 60});
    auto transport = std::make_shared<ScriptedTransport>(std::vector<ScriptedTransport::Reply>{
        {200, R"({"candidates": [{"content": {"parts": [{"text": "Here it is."},
            {"inlineData": {"mimeType": "image/png", "data": ")" +
                  slash::base64_encode(made) + R"("}}]}, "finishReason": "STOP"}],
            "modelVersion": "gemini-2.5-flash-image-001"})"},
        {200, R"({"candidates": [{"content": {"parts": [{"text": "I can't draw that."}]},
            "finishReason": "IMAGE_SAFETY"}]})"},
        {200, R"({"promptFeedback": {"blockReason": "SAFETY"}})"}});
    const library::OpenedGenerator generator = open(
        R"({"type": "gemini-image", "api_key_env": "TEST_KEY", "url": "https://example.test/v1beta/"})",
        transport);
    CHECK(generator.model == "gemini-2.5-flash-image");

    library::GenerateRequest asked = request("image", "a green field", 32, 18);
    asked.seed = 7;
    auto result = generator.generator->generate(asked);
    REQUIRE_MESSAGE(result.ok(), result.error().message);
    CHECK(transport->requests[0].url ==
          "https://example.test/v1beta/models/gemini-2.5-flash-image:generateContent");
    CHECK(transport->header("x-goog-api-key") == "key");
    CHECK(json::write(transport->body(), json::Style::Compact) ==
          R"({"contents":[{"parts":[{"text":"a green field"}],"role":"user"}],)"
          R"("generationConfig":{"imageConfig":{"aspectRatio":"16:9"},)"
          R"("responseModalities":["TEXT","IMAGE"],"seed":7}})");
    CHECK(result.value().model == "gemini-2.5-flash-image-001");
    CHECK(result.value().seed == 7);
    CHECK(decode(result.value().data).width == 32);

    CHECK(generator.generator->generate(request("image", "something")).error().message ==
          "the gemini-image generator returned no image (IMAGE_SAFETY): I can't draw that.");
    CHECK(generator.generator->generate(request("image", "something")).error().message ==
          "the gemini-image generator returned no image (blocked: SAFETY)");

    // A refine sends the image along with the prompt.
    library::Record parent;
    parent.format = "png";
    parent.width = 32;
    parent.height = 18;
    library::GenerateRequest refine = request("image", "add a tree");
    refine.parent = &parent;
    refine.parent_data = made;
    (void)generator.generator->generate(refine);
    const json::Value sent = transport->body(3);
    const json::Value& parts = sent.find("contents")->as_array()[0].as_object().at("parts");
    REQUIRE(parts.as_array().size() == 2);
    CHECK(parts.as_array()[1].find("inline_data")->find("mime_type")->as_string() == "image/png");
    CHECK(slash::base64_decode(
              parts.as_array()[1].find("inline_data")->find("data")->as_string()) == made);
}

TEST_CASE("automatic1111 draws at a size Stable Diffusion likes and keeps the seed it drew with") {
    const std::string made = png(512, 512, {90, 90, 200});
    const std::string reply =
        R"({"images": [")" + slash::base64_encode(made) +
        R"("], "info": "{\"seed\": 1234, \"sd_model_name\": \"dreamshaper\"}"})";
    auto transport =
        std::make_shared<ScriptedTransport>(std::vector<ScriptedTransport::Reply>{{200, reply}});
    const library::OpenedGenerator generator = open(
        R"({"type": "automatic1111", "api_key_env": "TEST_KEY",
            "options": {"steps": 20, "negative_prompt": "blurry"}})",
        transport);
    CHECK(generator.model.empty());

    auto result = generator.generator->generate(request("image", "blue gem icon", 16, 16));
    REQUIRE_MESSAGE(result.ok(), result.error().message);
    CHECK(transport->requests[0].url == "http://127.0.0.1:7860/sdapi/v1/txt2img");
    CHECK(transport->header("Authorization") == "Basic " + slash::base64_encode("key"));
    CHECK(json::write(transport->body(), json::Style::Compact) ==
          R"({"batch_size":1,"height":512,"n_iter":1,"negative_prompt":"blurry",)"
          R"("prompt":"blue gem icon","seed":-1,"steps":20,"width":512})");
    CHECK(result.value().seed == 1234);
    CHECK(result.value().model == "dreamshaper");
    CHECK(decode(result.value().data).width == 16);

    // A wide sprite keeps its shape; a named model is loaded for the request.
    auto wide =
        std::make_shared<ScriptedTransport>(std::vector<ScriptedTransport::Reply>{{200, reply}});
    const library::OpenedGenerator named =
        open(R"({"type": "automatic1111", "model": "pixelart.safetensors"})", wide);
    library::Record parent;
    parent.format = "png";
    parent.width = 48;
    parent.height = 24;
    library::GenerateRequest refine = request("image", "night time");
    refine.parent = &parent;
    refine.parent_data = png(48, 24, {90, 90, 200});
    refine.seed = 5;
    auto refined = named.generator->generate(refine);
    REQUIRE(refined.ok());
    const json::Value sent = wide->body();
    CHECK(wide->requests[0].url == "http://127.0.0.1:7860/sdapi/v1/img2img");
    CHECK(sent.find("width")->as_int() == 1536);
    CHECK(sent.find("height")->as_int() == 768);
    CHECK(sent.find("seed")->as_int() == 5);
    CHECK(sent.find("denoising_strength")->as_double() == doctest::Approx(0.6));
    CHECK(sent.find("override_settings")->find("sd_model_checkpoint")->as_string() ==
          "pixelart.safetensors");
    CHECK(slash::base64_decode(sent.find("init_images")->as_array()[0].as_string()) ==
          refine.parent_data);
    CHECK(refined.value().model == "pixelart.safetensors");
    const slash::render::Image fitted = decode(refined.value().data);
    CHECK(fitted.width == 48);
    CHECK(fitted.height == 24);
}

TEST_CASE("images are fitted by cropping the middle and averaging, weighted by alpha") {
    // 4 by 2: opaque red on the left, clear on the right. Fitted to 1 by 1, the middle 2 by 2
    // is half each: half as opaque, and still fully red rather than darkened.
    auto transport = std::make_shared<ScriptedTransport>(std::vector<ScriptedTransport::Reply>{
        {200, R"({"images": [")" + slash::base64_encode(png(4, 2, {255, 0, 0}, true)) + R"("]})"}});
    auto result = open(R"({"type": "automatic1111"})", transport)
                      .generator->generate(request("image", "a sprite", 1, 1));
    REQUIRE(result.ok());
    const slash::render::Image image = decode(result.value().data);
    REQUIRE(image.width == 1);
    CHECK(first_pixel(image) == std::array<int, 4>{255, 0, 0, 128});
}

TEST_CASE("openai-speech speaks the prompt and elevenlabs-sound makes effects") {
    // 0.1 seconds of 24 kHz samples, as OpenAI sends raw speech.
    const std::string samples(4800, '\x01');
    auto speech =
        std::make_shared<ScriptedTransport>(std::vector<ScriptedTransport::Reply>{{200, samples}});
    const library::OpenedGenerator voice =
        open(R"({"type": "openai-speech", "api_key_env": "TEST_KEY",
                 "options": {"voice": "marin", "instructions": "Whisper."}})",
             speech, "sound");
    CHECK_FALSE(voice.generator->capabilities().images);
    auto spoken = voice.generator->generate(request("sound", "Welcome back, traveler."));
    REQUIRE_MESSAGE(spoken.ok(), spoken.error().message);
    CHECK(speech->requests[0].url == "https://api.openai.com/v1/audio/speech");
    CHECK(json::write(speech->body(), json::Style::Compact) ==
          R"({"input":"Welcome back, traveler.","instructions":"Whisper.",)"
          R"("model":"gpt-4o-mini-tts","response_format":"pcm","voice":"marin"})");
    library::Record record;
    REQUIRE(library::describe_file(spoken.value().data, "line.wav", record).ok());
    CHECK(record.sample_rate == 24000);
    CHECK(record.seconds == doctest::Approx(0.1));

    // Raw samples at the rate output_format names, sent in the URL, not the body.
    auto effects = std::make_shared<ScriptedTransport>(
        std::vector<ScriptedTransport::Reply>{{200, std::string(3200, '\x02')}});
    const library::OpenedGenerator sounds = open(
        R"({"type": "elevenlabs-sound", "api_key_env": "TEST_KEY",
            "options": {"output_format": "pcm_16000", "prompt_influence": 0.5}})",
        effects, "sound");
    library::GenerateRequest asked = request("sound", "coin pickup");
    asked.seconds = 0.2;
    auto made = sounds.generator->generate(asked);
    REQUIRE_MESSAGE(made.ok(), made.error().message);
    CHECK(effects->requests[0].url ==
          "https://api.elevenlabs.io/v1/sound-generation?output_format=pcm_16000");
    CHECK(effects->header("xi-api-key") == "key");
    CHECK(json::write(effects->body(), json::Style::Compact) ==
          R"({"duration_seconds":0.5,"model_id":"eleven_text_to_sound_v2",)"
          R"("prompt_influence":0.5,"text":"coin pickup"})");
    REQUIRE(library::describe_file(made.value().data, "coin.wav", record).ok());
    CHECK(record.sample_rate == 16000);
    CHECK(record.seconds == doctest::Approx(0.1));

    // By default MP3, known by its first bytes.
    auto mp3 = std::make_shared<ScriptedTransport>(
        std::vector<ScriptedTransport::Reply>{{200, std::string("ID3\x04\x00", 5)}});
    auto sniffed = open(R"({"type": "elevenlabs-sound"})", mp3, "sound")
                       .generator->generate(request("sound", "thunder"));
    REQUIRE(sniffed.ok());
    CHECK(sniffed.value().format == "mp3");
    CHECK(mp3->requests[0].url ==
          "https://api.elevenlabs.io/v1/sound-generation?output_format=mp3_44100_128");
    CHECK(mp3->body().find("duration_seconds") == nullptr);
}

TEST_CASE("elevenlabs-music and stability-audio make music, asked its tempo and key in words") {
    library::GenerateRequest asked = request("music", "A tense boss fight with low strings.");
    asked.bpm = 140;
    asked.key = "D minor";
    asked.bar_beats = 3;
    asked.loop = true;
    const std::string details = "A tense boss fight with low strings.\n\nTempo: 140 BPM. Meter: "
                                "3/4. Key: D minor. It loops seamlessly: its end leads straight "
                                "back into its start.";

    // ElevenLabs: JSON, instrumental, 30 seconds unless asked; output_format in the URL.
    auto eleven = std::make_shared<ScriptedTransport>(
        std::vector<ScriptedTransport::Reply>{{200, std::string(1600, '\x02')}});
    const library::OpenedGenerator composer =
        open(R"({"type": "elevenlabs-music", "api_key_env": "TEST_KEY",
                 "options": {"output_format": "pcm_8000"}})",
             eleven, "music");
    CHECK(composer.generator->capabilities().music);
    CHECK_FALSE(composer.generator->capabilities().sounds);
    CHECK(composer.model == "music_v1");
    auto made = composer.generator->generate(asked);
    REQUIRE_MESSAGE(made.ok(), made.error().message);
    CHECK(eleven->requests[0].url == "https://api.elevenlabs.io/v1/music?output_format=pcm_8000");
    CHECK(eleven->header("xi-api-key") == "key");
    json::Value body = eleven->body();
    CHECK(body["prompt"].as_string() == details);
    body.as_object().erase("prompt");
    CHECK(json::write(body, json::Style::Compact) ==
          R"({"force_instrumental":true,"model_id":"music_v1","music_length_ms":30000})");
    // Raw samples are stereo.
    library::Record record;
    REQUIRE(library::describe_file(made.value().data, "boss.wav", record).ok());
    CHECK(record.channels == 2);
    CHECK(record.seconds == doctest::Approx(0.05));

    // A length is kept to what the service makes: 3 seconds to 10 minutes.
    asked.seconds = 1.0;
    REQUIRE(composer.generator->generate(asked).ok());
    CHECK(eleven->body(1)["music_length_ms"].as_int() == 3000);

    // Stable Audio: a form, the audio back as it is, the seed kept.
    auto stability = std::make_shared<ScriptedTransport>(
        std::vector<ScriptedTransport::Reply>{{200, std::string("ID3\x04\x00", 5)}});
    const library::OpenedGenerator stable =
        open(R"({"type": "stability-audio", "api_key_env": "TEST_KEY", "options": {"steps": 6}})",
             stability, "music");
    CHECK(stable.model == "stable-audio-2.5");
    asked.seconds = 12.4;
    asked.seed = 7;
    auto played = stable.generator->generate(asked);
    REQUIRE_MESSAGE(played.ok(), played.error().message);
    CHECK(played.value().format == "mp3");
    CHECK(played.value().seed == 7);
    const slash::ai::HttpRequest& sent = stability->requests[0];
    CHECK(sent.url == "https://api.stability.ai/v2beta/audio/stable-audio-2/text-to-audio");
    CHECK(sent.content_type.starts_with("multipart/form-data; boundary=slash-"));
    CHECK(stability->header("Authorization") == "Bearer key");
    CHECK(stability->header("Accept") == "audio/*");
    const auto field = [&sent](std::string_view name, std::string_view value) {
        return has_text(sent.body,
                        "name=\"" + std::string(name) + "\"\r\n\r\n" + std::string(value) + "\r\n");
    };
    CHECK(field("prompt", details) != "(missing)");
    CHECK(field("model", "stable-audio-2.5") != "(missing)");
    CHECK(field("duration", "12") != "(missing)");
    CHECK(field("seed", "7") != "(missing)");
    CHECK(field("steps", "6") != "(missing)");
    CHECK(field("output_format", "mp3") != "(missing)");

    // Neither makes images or sounds, nor refines music.
    auto config =
        slash::ai::parse_config(parse(R"({"generators": {"g": {"type": "stability-audio"}}})"));
    REQUIRE(config.ok());
    auto wrong = library::open_generator(config.value(), "g", "sound");
    REQUIRE_FALSE(wrong.ok());
    CHECK(wrong.error().message == R"(the generator "g" makes no sounds)");
    CHECK_FALSE(library::refines(stable.generator->capabilities(), "music"));
}

TEST_CASE("stability-3d makes a model from the concept image, sent as a form") {
    auto png = library::test_generator()->generate(request("image", "red ball", 16));
    REQUIRE(png.ok());
    library::GenerateRequest asked = request("model", "A red ball");
    asked.image_data = png.value().data;
    asked.seed = 3;
    const std::string glb = std::string("glTF\x02\x00\x00\x00", 8) + "...";

    // Stable Fast 3D: the image as a file, the options as fields, no seed, a .glb back.
    auto fast = std::make_shared<ScriptedTransport>(
        std::vector<ScriptedTransport::Reply>{{200, glb}, {200, "{}"}});
    const library::OpenedGenerator sculptor =
        open(R"({"type": "stability-3d", "api_key_env": "TEST_KEY",
                 "options": {"texture_resolution": 512, "remesh": "quad"}})",
             fast, "model");
    CHECK(sculptor.model == "stable-fast-3d");
    CHECK(sculptor.generator->capabilities().models);
    CHECK_FALSE(sculptor.generator->capabilities().images);
    auto made = sculptor.generator->generate(asked);
    REQUIRE_MESSAGE(made.ok(), made.error().message);
    CHECK(made.value().format == "glb");
    CHECK(made.value().data == glb);
    CHECK_FALSE(made.value().seed.has_value());
    const slash::ai::HttpRequest& sent = fast->requests[0];
    CHECK(sent.url == "https://api.stability.ai/v2beta/3d/stable-fast-3d");
    CHECK(sent.content_type.starts_with("multipart/form-data; boundary=slash-"));
    CHECK(fast->header("Authorization") == "Bearer key");
    CHECK(sent.body.find("name=\"image\"; filename=\"concept.png\"\r\nContent-Type: "
                         "image/png\r\n\r\n" +
                         png.value().data) != std::string::npos);
    CHECK(sent.body.find("name=\"texture_resolution\"\r\n\r\n512\r\n") != std::string::npos);
    CHECK(sent.body.find("name=\"remesh\"\r\n\r\nquad\r\n") != std::string::npos);
    CHECK(sent.body.find("name=\"seed\"") == std::string::npos);
    CHECK(sent.body.find("A red ball") == std::string::npos);

    // A reply that isn't a .glb file is an error, and so is a request with no concept.
    auto wrong = sculptor.generator->generate(asked);
    REQUIRE_FALSE(wrong.ok());
    CHECK(wrong.error().message.find("returned something other than a .glb model") !=
          std::string::npos);
    asked.image_data.clear();
    auto none = sculptor.generator->generate(asked);
    REQUIRE_FALSE(none.ok());
    CHECK(none.error().message.find("makes a model from a concept image") != std::string::npos);

    // Stable Point Aware 3D takes the seed.
    auto aware =
        std::make_shared<ScriptedTransport>(std::vector<ScriptedTransport::Reply>{{200, glb}});
    const library::OpenedGenerator pointed =
        open(R"({"type": "stability-3d", "model": "stable-point-aware-3d",
                 "api_key_env": "TEST_KEY"})",
             aware, "model");
    asked.image_data = png.value().data;
    auto seeded = pointed.generator->generate(asked);
    REQUIRE_MESSAGE(seeded.ok(), seeded.error().message);
    CHECK(seeded.value().seed == 3);
    CHECK(aware->requests[0].url == "https://api.stability.ai/v2beta/3d/stable-point-aware-3d");
    CHECK(aware->requests[0].body.find("name=\"seed\"\r\n\r\n3\r\n") != std::string::npos);
    CHECK_FALSE(library::refines(pointed.generator->capabilities(), "model"));
}

TEST_CASE("generators retry rate limits and report errors with the service's message") {
    const std::string made = png(4, 4, {1, 2, 3});
    auto transport = std::make_shared<ScriptedTransport>(std::vector<ScriptedTransport::Reply>{
        {429, R"({"error": {"message": "slow down"}})", "0"},
        {200, R"({"data": [{"b64_json": ")" + slash::base64_encode(made) + R"("}]})"},
        {401, R"({"error": {"message": "Incorrect API key provided"}})"}});
    const library::OpenedGenerator generator =
        open(R"({"type": "openai-image", "max_retries": 1})", transport);
    CHECK(generator.generator->generate(request("image", "x")).ok());
    CHECK(transport->requests.size() == 2);
    auto refused = generator.generator->generate(request("image", "x"));
    CHECK(refused.error().code == ErrorCode::PermissionDenied);
    CHECK(refused.error().message ==
          "the openai-image generator answered HTTP 401: Incorrect API key provided");

    // ElevenLabs puts its message under detail.
    auto quota = std::make_shared<ScriptedTransport>(std::vector<ScriptedTransport::Reply>{
        {400,
         R"({"detail": {"status": "quota_exceeded", "message": "You have no credits left."}})"}});
    CHECK(open(R"({"type": "elevenlabs-sound"})", quota, "sound")
              .generator->generate(request("sound", "x"))
              .error()
              .message ==
          "the elevenlabs-sound generator answered HTTP 400: You have no credits left.");

    // A key that isn't set fails before any request; listing generators needs none.
    auto unused = std::make_shared<ScriptedTransport>(std::vector<ScriptedTransport::Reply>{{}});
    const library::OpenedGenerator keyless =
        open(R"({"type": "gemini-image", "api_key_env": "UNSET_KEY"})", unused);
    CHECK(keyless.generator->generate(request("image", "x")).error().message ==
          "the gemini-image generator reads its key from UNSET_KEY, which is not set");
    CHECK(unused->requests.empty());

    // A sound generator can't make images, and the speech one can't refine.
    auto config =
        slash::ai::parse_config(parse(R"({"generators": {"voice": {"type": "openai-speech"}}})"));
    CHECK(library::open_generator(config.value(), "voice", "image").error().message ==
          "the generator \"voice\" makes no images");
    CHECK(slash::ai::parse_config(parse(R"({"generators": {"x": {"type": "openai-images"}}})"))
              .error()
              .message.starts_with("providers.json /generators/x/type: "));
}

namespace {

// A stand-in Stable Diffusion web UI on this machine.
class FakeDiffusionServer {
public:
    FakeDiffusionServer() {
        server_.Post("/sdapi/v1/txt2img", [this](const httplib::Request& request,
                                                 httplib::Response& response) {
            const json::Value body = parse(request.body);
            const auto width = static_cast<std::uint32_t>(body.find("width")->as_int());
            const auto height = static_cast<std::uint32_t>(body.find("height")->as_int());
            {
                const std::scoped_lock lock(mutex_);
                ++requests_;
            }
            response.set_content(R"({"images": [")" +
                                     slash::base64_encode(png(width, height, {240, 200, 40})) +
                                     R"("], "info": "{\"seed\": 99}"})",
                                 "application/json");
        });
        // OpenAI's image edits, as multipart/form-data, to check the form parses.
        server_.Post("/v1/images/edits", [this](const httplib::Request& request,
                                                httplib::Response& response) {
            if (!request.form.has_file("image") ||
                request.form.get_field("model") != "gpt-image-1") {
                response.status = 400;
                response.set_content(R"({"error": {"message": "bad form"}})", "application/json");
                return;
            }
            const httplib::FormData file = request.form.get_file("image");
            auto image = slash::render::decode_image(file.content);
            if (file.filename != "image.png" || !image) {
                response.status = 400;
                response.set_content(R"({"error": {"message": "bad image"}})", "application/json");
                return;
            }
            {
                const std::scoped_lock lock(mutex_);
                prompts_.push_back(request.form.get_field("prompt"));
            }
            response.set_content(R"({"data": [{"b64_json": ")" +
                                     slash::base64_encode(png(image.value().width,
                                                              image.value().height, {200, 0, 0})) +
                                     R"("}]})",
                                 "application/json");
        });
        port_ = server_.bind_to_any_port("127.0.0.1");
        thread_ = std::thread([this] { server_.listen_after_bind(); });
        server_.wait_until_ready();
    }

    ~FakeDiffusionServer() {
        server_.stop();
        thread_.join();
    }

    FakeDiffusionServer(const FakeDiffusionServer&) = delete;
    FakeDiffusionServer& operator=(const FakeDiffusionServer&) = delete;

    [[nodiscard]] std::string url() const { return "http://127.0.0.1:" + std::to_string(port_); }

    [[nodiscard]] int requests() {
        const std::scoped_lock lock(mutex_);
        return requests_;
    }

    [[nodiscard]] std::vector<std::string> prompts() {
        const std::scoped_lock lock(mutex_);
        return prompts_;
    }

private:
    httplib::Server server_;
    std::thread thread_;
    std::mutex mutex_;
    int requests_ = 0;
    std::vector<std::string> prompts_;
    int port_ = 0;
};

} // namespace

TEST_CASE("asset.generate calls the generator providers.json names, once per request") {
    FakeDiffusionServer server;
    const fs::path root = fs::absolute("library_generators");
    fs::remove_all(root);
    fs::create_directories(root / "project");
    fs::create_directories(root / "library");
    REQUIRE(slash::platform::write_file((root / "project" / "project.json").string(),
                                        R"({"format": 1, "name": "Generators"})")
                .ok());
    REQUIRE(slash::platform::write_file(
                (root / "project" / "providers.json").string(),
                R"({"generators": {"sd": {"type": "automatic1111", "url": ")" + server.url() +
                    R"(", "license": "CreativeML Open RAIL-M"}, "edits": {"type": "openai-image",
                    "url": ")" +
                    server.url() + R"(/v1"}}})")
                .ok());

    slash::world::ComponentRegistry components;
    slash::world::register_builtin_components(components);
    command::Registry commands;
    command::register_builtin_commands(commands);
    slash::ai::register_commands(commands);
    library::register_commands(commands);
    command::Session session(components, commands,
                             command::SessionOptions{.project_root = (root / "project").string(),
                                                     .capture_logs = false,
                                                     .library_dir = (root / "library").string()});
    const auto call = [&session](std::string_view name, std::string_view params) {
        command::Reply reply = session.call(name, parse(params));
        REQUIRE_MESSAGE(reply.ok, reply.message);
        return reply.value;
    };

    // The result is fitted to the size asked for.
    json::Value made = call("asset.generate", R"({"prompt": "gold coin", "width": 16,
                                                  "generator": "sd", "path": "art/coin.png"})");
    CHECK(made["width"].as_int() == 16);
    CHECK(server.requests() == 1);
    const json::Value info = call("asset.info", R"({"id": ")" + made["id"].as_string() + R"("})");
    CHECK(info.find("generator")->as_string() == "automatic1111");
    CHECK(info.find("seed")->as_int() == 99);
    CHECK(info.find("license")->as_string() == "CreativeML Open RAIL-M");
    const slash::render::Image copied = decode(
        slash::platform::read_file((root / "project" / "art" / "coin.png").string()).value());
    CHECK(copied.width == 16);
    CHECK(first_pixel(copied) == std::array<int, 4>{240, 200, 40, 255});

    // The same request again comes from the library, with no call to the server.
    made = call("asset.generate", R"({"prompt": "gold coin", "width": 16, "generator": "sd"})");
    CHECK(made["reused"].as_bool());
    CHECK(server.requests() == 1);

    // Another generator refines it, through OpenAI's multipart image edits.
    json::Value refined =
        call("asset.refine", R"({"id": ")" + made["id"].as_string() +
                                 R"(", "prompt": "make it red", "generator": "edits"})");
    CHECK(server.prompts() == std::vector<std::string>{"make it red"});
    CHECK(refined["width"].as_int() == 16);
    CHECK(call("asset.info", R"({"id": ")" + refined["id"].as_string() + R"("})")
              .find("parent")
              ->as_string() == made["id"].as_string());

    // ai.providers lists the generator.
    const json::Value providers = call("ai.providers", "{}");
    CHECK(providers.find("generators")->find("sd")->find("type")->as_string() == "automatic1111");
}
