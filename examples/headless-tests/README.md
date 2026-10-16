# Headless tests

Checking a game after a change without reading logs or screenshots. [tests/main.test.json](tests/main.test.json)
holds three tests: each opens `main.scene.json` in a session of its own and runs engine
commands, with what each reply must hold in `expect`. `test.run` runs them headless and returns
only the checks that failed. [calls.jsonl](calls.jsonl) runs them, breaks the floor and then
the scene, and runs them again; [results.jsonl](results.jsonl) holds the responses, line for
line.

```bash
slash-cli rpc --project . < calls.jsonl
slash-cli test                      # the same run from the shell; exits 1 if a check failed
```

- `crate_lands` waits with `run.until` for the crate to fall asleep, then checks where it is.
  With the floor's shape disabled, the crate never sleeps, and the failure points at the
  expected value in the test file (`/tests/crate_lands/steps/0/expect/entities`) with the tick.
- `coin_sees_the_crate` reads the `body_entered` event the coin's `Area2D` emits as the crate
  falls through it.
- `sound_button_toggles` presses a toggle `Button` with `ui.press`.
- A warning logged while a test runs fails it too: an `AnimatedSprite2D` whose `.frames.json`
  file is missing logs one on its first tick, and the failure carries the log line and the tick
  the step ended at.
- Tests start from the scene file, so save before testing an edit. They can't write files.
- With no `paths` or `tests`, `test.run` also runs `storyboard.check`.
