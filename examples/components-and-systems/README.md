# Components and systems

A game's own component, `Velocity`, and a `movement` system that moves every entity with a
`Transform2D` and a `Velocity`, run headless for 64 ticks of 1/64 s. [main.cpp](main.cpp) prints
the scene afterwards: `ball` moved by its velocity, `wall` stayed put.

- Components are plain structs reflected with `SLASH_COMPONENT`, at namespace scope. Registering
  one lets scenes save and load it and makes `engine.describe` list it with its docs.
- Systems are functions the world runs every tick, in order of priority. They reach components
  through EnTT views.
