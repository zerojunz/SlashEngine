// Gameplay libraries for test_gameplay.cpp, one per SLASH_TEST_MODULE_* definition, built as
// cmake/gameplay builds a game's code.

#include "slash/world/game_module.hpp"

#include <string>

namespace test_game {

#if defined(SLASH_TEST_MODULE_V3)
// Version 3 holds text where version 1 held a number.
struct Health {
    std::string current = "full";
};
SLASH_COMPONENT(Health, current)
#elif defined(SLASH_TEST_MODULE_V2)
// Version 2 drops armor and adds shield.
struct Health {
    float current = 10.0F;
    int shield = 0;
};
SLASH_COMPONENT(Health, current, shield)

struct Speed {
    float value = 1.0F;
};
SLASH_COMPONENT(Speed, value)
#else
struct Health {
    float current = 10.0F;
    float armor = 2.0F;
};
SLASH_COMPONENT(Health, current, armor)
#endif

// Never registered: reloading can't keep it.
struct Scratch {
    int value = 0;
};
SLASH_COMPONENT(Scratch, value)

#if defined(SLASH_TEST_MODULE_ENGINE_COMPONENT)
// The same name as one of the engine's.
struct Transform2D {
    float x = 0.0F;
};
SLASH_COMPONENT(Transform2D, x)
#endif

} // namespace test_game

#if defined(SLASH_TEST_MODULE_BAD_ABI)
extern "C" SLASH_GAME_MODULE_EXPORT const slash::world::GameModuleAbi* slash_game_module_abi() {
    static constexpr slash::world::GameModuleAbi kAbi{"0000000000000000", SLASH_COMPILER_ID};
    return &kAbi;
}
extern "C" SLASH_GAME_MODULE_EXPORT void slash_game_module(slash::world::GameModule& /*game*/) {}
#elif !defined(SLASH_TEST_MODULE_NO_ENTRY)
SLASH_GAME_MODULE(game) {
    using test_game::Health;
    game.add_component<Health>().set_doc("Hit points.");
#if defined(SLASH_TEST_MODULE_DUPLICATE)
    game.add_component<Health>();
#endif
#if defined(SLASH_TEST_MODULE_ENGINE_COMPONENT)
    game.add_component<test_game::Transform2D>();
#endif
#if defined(SLASH_TEST_MODULE_V2)
    game.add_component<test_game::Speed>();
    game.add_system("haste", [](slash::world::World& /*world*/, double /*tick_seconds*/) {});
#endif
#if !defined(SLASH_TEST_MODULE_V3)
    game.add_system("regenerate",
                    [](slash::world::World& world, double /*tick_seconds*/) {
#if defined(SLASH_TEST_MODULE_V2)
                        constexpr float kHeal = 2.0F;
#else
            constexpr float kHeal = 1.0F;
#endif
                        auto& ecs = world.ecs();
                        for (const entt::entity entity : ecs.view<Health>()) {
                            ecs.get<Health>(entity).current += kHeal;
                            ecs.emplace_or_replace<test_game::Scratch>(entity);
                        }
                    },
                    {.doc = "Heals every tick."});
#endif
#if defined(SLASH_TEST_MODULE_ENGINE_SYSTEM)
    game.add_system("engine_system", [](slash::world::World& /*world*/, double /*seconds*/) {});
#endif
}
#endif
