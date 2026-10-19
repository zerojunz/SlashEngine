// The game's own code, calling the hello SDK. Exports link the SDK and compile this file with
// SLASH_SDK_HELLO defined and the SDK's headers; the tools' build of it leaves the SDK out, so
// the same code runs in both.

#include "slash/world/game_module.hpp"

#include <string>

#ifdef SLASH_SDK_HELLO
#include "hello.h"
#endif

namespace {

struct Greeter {
    std::string greeting;
    bool sdk_started = false;
};
SLASH_COMPONENT(Greeter, greeting, sdk_started)

void greet(slash::world::World& world, double /*tick_seconds*/) {
#ifdef SLASH_SDK_HELLO
    static const bool started = hello_init(HELLO_APP_ID) != 0;
    const char* greeting = hello_greeting();
#else
    constexpr bool started = false;
    const char* greeting = "No SDK in this build";
#endif
    world.ecs().view<Greeter>().each([&](Greeter& greeter) {
        greeter.greeting = greeting;
        greeter.sdk_started = started;
    });
}

} // namespace

SLASH_GAME_MODULE(game) {
    game.add_component<Greeter>().set_doc("What the hello SDK says.");
    game.add_system("greet", greet, {.doc = "Asks the hello SDK for its greeting."});
}
