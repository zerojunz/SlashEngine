#include "slash/core/sha256.hpp"
#include "slash/library/generator.hpp"
#include "slash/scene3d/blend.hpp"

#include <memory>
#include <string>
#include <utility>

namespace slash::library {
namespace {

class BlenderGenerator final : public Generator {
public:
    BlenderGenerator(std::string program, int timeout_seconds)
        : program_(std::move(program)), timeout_seconds_(timeout_seconds) {}

    [[nodiscard]] const GeneratorCapabilities& capabilities() const override {
        return capabilities_;
    }

    [[nodiscard]] Result<Generated> generate(const GenerateRequest& request,
                                             const ai::CancelFlag* /*cancel*/) override {
        if (request.type != "model" || request.script.empty()) {
            return make_error(ErrorCode::InvalidArgument,
                              "the blender generator builds models with a script; give script");
        }
        if (request.folder.empty()) {
            return make_error(ErrorCode::InvalidArgument,
                              "the blender generator needs a folder to run Blender in");
        }
        scene3d::BlenderScript job;
        job.script = request.script;
        job.params = request.parameters.is_object()
                         ? json::write(request.parameters, json::Style::Compact)
                         : "{}";
        job.seed = request.seed.value_or(0);
        job.folder = request.folder;
        // Named by what it runs, so agents running scripts at once don't share files.
        Sha256 hash;
        hash.update(job.script);
        hash.update(job.params);
        hash.update(std::to_string(job.seed));
        job.name = "build-" + hash.hex().substr(0, 12);
        job.blender = program_;
        job.timeout_seconds = timeout_seconds_;
        auto built = scene3d::run_blender_script(job);
        if (!built) {
            const std::string& message = built.error().message;
            // find_blender says what converting a .blend file needs; a script needs the same.
            constexpr std::string_view kConverting = "converting a .blend file";
            if (message.starts_with(kConverting)) {
                return make_error(built.error().code,
                                  "running a Blender script" + message.substr(kConverting.size()));
            }
            return built.error();
        }
        Generated out;
        out.data = std::move(built.value().glb);
        out.format = "glb";
        out.model = "blender " + built.value().version;
        out.seed = job.seed;
        return out;
    }

private:
    std::string program_;
    int timeout_seconds_ = 600;
    GeneratorCapabilities capabilities_{.scripts = true};
};

} // namespace

std::shared_ptr<Generator> blender_generator(std::string program, int timeout_seconds) {
    return std::make_shared<BlenderGenerator>(std::move(program), timeout_seconds);
}

} // namespace slash::library
