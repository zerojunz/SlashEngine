#pragma once

#include "slash/services/services.hpp"

#include <functional>

// The platform's App Tracking Transparency prompt: Apple's, in apple.mm; none elsewhere.
namespace slash::services::detail {

// Whether the platform has the prompt.
[[nodiscard]] bool tracking_available();
// What the player answered before, without asking.
[[nodiscard]] TrackingStatus platform_tracking_status();
// Shows the prompt and calls done with the answer, from any thread.
void request_platform_tracking(std::function<void(TrackingStatus status)> done);

} // namespace slash::services::detail
