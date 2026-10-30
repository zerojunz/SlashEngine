// Builds without AccessKit (no cargo where they were configured) serve no screen reader; the
// tree and its snapshots still work.

#include "slash/accessibility/accessibility.hpp"

namespace slash::accessibility {

bool available() {
    return false;
}

std::unique_ptr<Backend> Backend::attach(SDL_Window* /*window*/, std::string /*name*/) {
    return nullptr;
}

} // namespace slash::accessibility
