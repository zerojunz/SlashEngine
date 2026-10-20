#pragma once

#include "slash/command/command.hpp"

namespace slash::command {

// Adds services.state (ads, products, purchases, consent and analytics events) and
// services.test (the test backend's settings, and loading, showing and buying as the game
// would), over slash/services/services.hpp.
void register_services_commands(Registry& registry);

} // namespace slash::command
