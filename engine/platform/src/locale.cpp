#include "slash/platform/locale.hpp"

#include <SDL3/SDL.h>

#include <string>

namespace slash::platform {

std::string preferred_locale() {
    int count = 0;
    SDL_Locale** locales = SDL_GetPreferredLocales(&count);
    std::string out;
    if (locales != nullptr && count > 0 && locales[0] != nullptr &&
        locales[0]->language != nullptr) {
        out = locales[0]->language;
        if (locales[0]->country != nullptr && locales[0]->country[0] != '\0') {
            out += '_';
            out += locales[0]->country;
        }
    }
    SDL_free(reinterpret_cast<void*>(locales));
    return out;
}

} // namespace slash::platform
