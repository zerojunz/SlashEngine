// Builds without AccessKit (no cargo where they were configured) serve no screen reader; the
// tree and its snapshots still work.

#include "slash/accessibility/accessibility.hpp"

namespace slash::accessibility {

bool available() {
    return false;
}

// By value as AccessKit's backend takes it, which moves the name into its window node.
std::unique_ptr<Backend>
Backend::attach(SDL_Window* /*window*/,
                std::string /*name*/) { // NOLINT(performance-unnecessary-value-param)
    return nullptr;
}

} // namespace slash::accessibility
