# Add a component

How to add a reflected component and register its docs.

Status: current · Updated: 2026-11-20

Components are plain structs reflected with one macro, at namespace scope in the struct's own
namespace. Register them so scenes can save, load and describe them:

```cpp
struct Health {
    float current = 100.0F;
    float max = 100.0F;
};
SLASH_COMPONENT(Health, current, max)

registry.add<Health>()
    .set_doc("Hit points for anything that can take damage.")
    .describe("current", {.doc = "Hit points left.", .min = 0.0});
```

A game's own components are registered the same way in its `SLASH_GAME_MODULE` function, with
`game.add_component<Health>()` (see [Gameplay code](gameplay-code.md#gameplay-code)).

Supported field types are listed in `engine/world/include/slash/world/serialize.hpp`. Use
`SLASH_REFLECT` for structs that appear inside components, and describe their members by path
(`.describe("items[].text", ...)`, `{}` for a map's values). `slash-cli docs` writes the
reference from these descriptions (`engine/command/src/reference.cpp`), and the
`cli_docs_strict` test fails on any component, field, command or param without one. Built-in components are in
`engine/world/include/slash/world/components.hpp`.
