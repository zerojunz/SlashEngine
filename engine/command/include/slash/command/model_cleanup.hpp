#pragma once

#include "slash/command/command.hpp"
#include "slash/scene3d/cleanup.hpp"

#include <string>
#include <string_view>

namespace slash::command {

// Adds model.cleanup, over slash/scene3d/cleanup.hpp, and model.retarget, over
// slash/scene3d/humanoid.hpp.
void register_model_cleanup_commands(Registry& registry);

// What a cleanup is asked to do, read from model.cleanup's params.
struct CleanupRequest {
    scene3d::CleanupOptions options;
    // The storyboard budget that set options.triangles, or "" when the params did or none did.
    std::string budget;
};

// The JSON Schema properties of model.cleanup's options: all its params but path and out.
[[nodiscard]] json::Object cleanup_properties();

// Reads model.cleanup's options (cleanup_properties) from params into request, for the model at
// path written to out, whose storyboard budget sets its triangles unless they are given. A bad
// option is a failed reply whose JSON Pointer is at followed by its name; ok otherwise.
[[nodiscard]] Reply read_cleanup(Session& session, const json::Value& params,
                                 const std::string& out, std::string_view at,
                                 CleanupRequest& request);

// Cleans the model up as asked, filling report. A failure is a reply at the JSON Pointer of the
// option at fault (after at), or path_at when the model itself is at fault.
[[nodiscard]] Reply clean_up(const CleanupRequest& request, scene3d::Model& model,
                             std::string_view at, std::string_view path_at,
                             scene3d::CleanupReport& report);

// What model.cleanup returns of a cleaned model: triangles before and after, the budget, size,
// pivot and, when it did them, unwrapped surfaces, delit textures and the light it took out,
// and its collision shapes.
[[nodiscard]] json::Value cleanup_result(const CleanupRequest& request,
                                         const scene3d::CleanupReport& report,
                                         const scene3d::Model& model);

} // namespace slash::command
