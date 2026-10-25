Before: a state machine transition could only wait for a `conditions/<name>` flag. Godot's `advance_expression` was reported on import and dropped, so a game had to turn every comparison (`speed > 0.5`, `jump and grounded`) into flags that gameplay code kept in step.

After: an auto transition's `advance_expression` is taken once it runs to a value that counts as true, as Godot's. Its names come from the tree's `variables`, which the `.animtree.json` file declares with their defaults and games set as parameters (`"speed"`, beside `travel`). `triggers` lists variables that start `false` and go back to `false` once a transition that reads them is taken on its own, as Unity's Animator triggers. This also gives the Unity Animator import, next, somewhere to land.

```json
{"format": 1, "variables": {"speed": 0.0, "grounded": true}, "triggers": ["jump"],
 "root": {"type": "StateMachine", "transitions": [
   {"from": "run", "to": "jump", "advance_mode": "auto", "advance_expression": "jump and grounded"}]}}
```

How:

- **Expressions:** `slash::Expression` (`engine/core/include/slash/core/expression.hpp`) parses and runs Godot's `Expression` syntax.
  - It has Godot's operators and their priorities, its constants and its math functions (`abs`, `min`, `clamp`, `lerp`, `is_equal_approx` and the rest).
  - Values follow `Variant`'s rules. Integers wrap and divide as integers, and comparing values of different kinds fails, except `==` and `!=` with `null`. `and` and `or` take any value by `booleanize`.
  - Errors name the character they are at, and a misspelled function gets "did you mean".
  - Member access and method calls aren't supported, since there is no base object.
- **Trees:** `tree.cpp` checks each expression against the declared names, with "did you mean". It also checks the variables' names, a name that is both a variable and a trigger, and a variable that shares a name with one of the root node's parameters. The variables are listed as parameters first, and `check_parameter` takes a number, a boolean or `null` for a `null` default. `tree_run.cpp` runs the expression in `_check_advance_condition`, and an expression that fails isn't taken. A transition taken on its own sets the triggers it read back to `false`; travel steps leave them.
- **Godot import:** a transition's `advance_expression` carries over. The names it reads become the file's `variables`, `null` until gameplay code sets them, and the report says which ones. An expression with method calls or member access (`is_on_floor()`, `velocity.x`) is reported with the parse error.
- **Tests:**
  - `tests/test_expression.cpp` covers operator binding, the `Variant` rules, names and functions, and error positions.
  - `tests/test_animation_tree.cpp` has a state machine run on speed, a trigger that waits and is used up, and an expression that fails on `null`. It also covers the file checks, the parameter list and the checks for `null` defaults.
  - `tests/test_import_godot.cpp` checks that an expression converts with its variables, and that a method call is reported.
- **Example:** `examples/animation-expressions` runs, jumps on a trigger once grounded and settles back in idle.
- **Docs:** the state machine section of `docs/reference/scene-format.md`, `docs/reference/godot-import.md`, AGENTS.md, the examples index and the roadmap.

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_01A7FAm2JgA9jtumg74YFzUN
